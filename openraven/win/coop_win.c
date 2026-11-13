// Coop groundwork: control of the game's clocks and a per-frame state hash (lockstep determinism tests).
//
// The game reads time through three functions, all replaced here:
//   0x909320  time_us()      CLOCK_MONOTONIC in microseconds (game code, ~500 calls/s)
//   0x909460  time_ms()      CLOCK_MONOTONIC in milliseconds (game code, ~60 calls/s)
//   0x6546e0  SDL_GetTicks   ms since SDL init (statically linked SDL; only SDL itself calls it, left real)
// The frame-time update 0x54cdc0 (called from the main loop 0x413e40) reads time_us once per frame,
// from 0x54ce02. That call is our frame tick.
//
// Virtual clock (on with RIO2PAD_RECORD / RIO2PAD_REPLAY, or RAVEN_VCLOCK=1): time_us/time_ms return a
// clock that only moves at frame ticks, by exactly 1/60 s, so every copy of the game sees the same times.
// If no frame tick comes for 250 ms of real time (loading), the clock moves on its own; every such
// step is logged ("[coop] stall"), since it is a likely desync source.
//
// RAVEN_HASHLOG=file writes "frame hash" per frame: FNV-1a over known simulation state.
// RAVEN_TIMETRACE=1 logs which code calls each clock, per 10 s.
#include "shim.h"
#include "elfload.h"
#include "coop.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_TICK_RA 0x54ce07ULL      // return address of the frame-time update's time_us call
#define FRAME_US      16667ULL

void raven_time_trace(const char *fn, void *ra);   // shim_core.c
void raven_time_trace_enable(int on);
int rio2pad_coop_mode(void);                       // rio2pad_win.c
void rio2pad_coop_frame(unsigned frame);

static void hook(uint64_t addr, const unsigned char *expect, size_t n, void *target, const char *name);
static LARGE_INTEGER g_freq, g_t0;
static int g_vc;
#define VBASE 1000000000ULL               // virtual clock origin (us), the same on every machine
static uint64_t g_boot_real;
static volatile uint64_t g_vnow;          // virtual time, us
static volatile uint64_t g_last_tick_real;
static unsigned g_frame;
static FILE *g_hashlog;
static volatile long g_stalls;
static int g_gate = 2;                    // RAVEN_GATE=n: turns per frame for the game's polling threads (0 = free running)
static unsigned g_threadlog;              // RAVEN_THREADLOG=n: thread states every n frames
static int g_stall_step = 0;              // RAVEN_VSTALL=0: the clock never moves between frame ticks
static FILE *g_pagelog;                   // RAVEN_PAGEHASH=file: hash of every 4 KB page of the game's data, every 50 frames
static unsigned g_memdump_frame, g_memdump_n = 1; static const char *g_memdump_file;   // RAVEN_MEMDUMP=frame,file or frame,file,count (file.<frame>)
#define DATA_LO 0x1661000ULL
#define DATA_HI 0x23d3000ULL
#define VEPOCH 1541030400ULL              // 2018-11-01 00:00 UTC: wall clock origin in virtual-clock mode

// Before the first frame tick the wall clock moves only by the main thread's own sleeps: the same on
// every PC, and startup waits that count on time() (the 4-player cabinet waits ~10 s for a linked
// second PC) still run out.
static volatile uint64_t g_pre_us;
static DWORD g_main_tid;   // the game's main thread (coop_game_thread)
void coop_presleep(unsigned us) { if (g_vc && !g_frame && GetCurrentThreadId() == g_main_tid) g_pre_us += us; }
// Wall clock for the libc time shims: fixed date + virtual time; frozen until the first frame tick.
int coop_vtime_us(uint64_t *us)
{
    if (!g_vc) return 0;
    *us = VEPOCH * 1000000ULL + (g_frame ? g_vnow - VBASE : 0) + g_pre_us;   // never steps back at the first tick
    return 1;
}

static void page_hashes(void)
{
    if (g_memdump_file && g_frame >= g_memdump_frame && g_frame < g_memdump_frame + g_memdump_n) {
        char nm[600];
        snprintf(nm, sizeof nm, "%s.%u", g_memdump_file, g_frame);
        FILE *f = fopen(g_memdump_n > 1 ? nm : g_memdump_file, "wb");
        if (f) { fwrite((void *)DATA_LO, 1, DATA_HI - DATA_LO, f); fclose(f); }
    }
    if (!g_pagelog || g_frame % 50) return;
    fwrite(&g_frame, 4, 1, g_pagelog);
    for (uint64_t a = DATA_LO; a < DATA_HI; a += 4096) {
        const uint64_t *w = (const uint64_t *)a;
        uint64_t h = 0x9e3779b97f4a7c15ULL;
        for (int i = 0; i < 512; i++) h = (h ^ w[i]) * 0xff51afd7ed558ccdULL, h ^= h >> 29;
        fwrite(&h, 8, 1, g_pagelog);
    }
    fflush(g_pagelog);
}

// Same base as the clock_gettime(CLOCK_MONOTONIC) shim: QPC since boot.
static uint64_t mono_us(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / g_freq.QuadPart) * 1000000ULL +
           (uint64_t)(c.QuadPart % g_freq.QuadPart) * 1000000ULL / (uint64_t)g_freq.QuadPart;
}

// ---- state hash ----
// Known simulation state; grows as more is found (enemy/object lists, RNG state).
static const struct { uint64_t addr; size_t len; const char *name; } g_hashed[] = {
    {0x1c0c7c0ULL, 4 * 0x250, "player state"},
    {0x17f85c0ULL, 0x20, "game time accumulators"},
};

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
    return h;
}

// Compared between the PCs of an online session. Player state without its pointer fields (+0x20..0x2f
// of each slot) and the two game-time totals.
uint64_t coop_state_hash(void)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 4; i++) {
        const unsigned char *s = (const unsigned char *)(0x1c0c7c0ULL + i * 0x250ULL);
        h = fnv(h, s, 0x20);
        h = fnv(h, s + 0x30, 0x250 - 0x30);
    }
    const uint64_t *acc = (const uint64_t *)0x17f85c0ULL;
    h = fnv(h, &acc[0], 8);
    return fnv(h, &acc[2], 8);
}

// Wider check, log only: game data pages that changed during play yet stayed identical on all 4 copies
// of a 4-player session on one PC (test_irt\l4_run.ps1 with L4_PAGEHASH, 2026-11-13). Enemies/world
// state likely live here. Not proven equal across different PCs, so a mismatch is logged, not shown.
static const struct { uint64_t addr; size_t len; } g_deep[] = {
    {0x169c000ULL, 0x1000}, {0x16a7000ULL, 0x1000}, {0x16e9000ULL, 0x3000}, {0x176d000ULL, 0x2000},
    {0x177c000ULL, 0x1000}, {0x1790000ULL, 0x1000}, {0x17f7000ULL, 0x1000}, {0x1c0b000ULL, 0x1000},
    {0x1c17000ULL, 0x1000}, {0x1cbf000ULL, 0x2000},
};
uint64_t coop_deep_hash(void)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned i = 0; i < sizeof g_deep / sizeof *g_deep; i++) h = fnv(h, (const void *)g_deep[i].addr, g_deep[i].len);
    return h;
}

static void frame_hash(void)
{
    if (!g_hashlog) return;
    fprintf(g_hashlog, "%u", g_frame);
    for (unsigned i = 0; i < sizeof g_hashed / sizeof *g_hashed; i++)
        fprintf(g_hashlog, " %016llx", (unsigned long long)fnv(0xcbf29ce484222325ULL, (void *)g_hashed[i].addr, g_hashed[i].len));
    fputc('\n', g_hashlog);
    if (g_frame % 500 == 0) {   // raw dump next to the hash log, to find which bytes differ
        for (unsigned i = 0; i < sizeof g_hashed / sizeof *g_hashed; i++) {
            fprintf(g_hashlog, "#dump %u %u", g_frame, i);
            const unsigned char *b = (const unsigned char *)g_hashed[i].addr;
            for (size_t k = 0; k < g_hashed[i].len; k++) fprintf(g_hashlog, " %02x", b[k]);
            fputc('\n', g_hashlog);
        }
    }
    if ((g_frame & 63) == 0) fflush(g_hashlog);
}

// ---- clocks ----

static void frame_tick(void)
{
    if (!g_frame) {
        coopnet_start();   // online: waits here until every PC is ready
        g_vnow = VBASE + 30000000ULL;
        // The game-time accumulators (0x551540 us, 0x5515a0 ms: total, last reading) ran on real time
        // during boot. Give them fixed values, as if boot had taken exactly 10 s.
        uint64_t *acc = (uint64_t *)0x17f85c0ULL;
        acc[0] = 10000; acc[1] = g_vnow / 1000; acc[2] = 10000000; acc[3] = g_vnow;
        *(uint64_t *)0x1e519e0ULL = g_vnow / 1000 - 10000;   // a start time taken from time_ms during boot (0x5e53c6)
        if (g_gate) { extern void raven_gate_enable(int on); raven_gate_enable(1); }
        raven_log("[coop] first frame tick: virtual clock starts\n");
    }
    g_frame++;
    g_vnow += FRAME_US;
    if (g_gate) { extern void raven_gate_tick(int turns); raven_gate_tick(g_gate); }
    g_last_tick_real = mono_us();
    rio2pad_coop_frame(g_frame);
    frame_hash();
    page_hashes();
    if (g_threadlog && g_frame % g_threadlog == 0) { extern void raven_thread_report(uint64_t fn); raven_log("[threads] frame %u\n", g_frame); raven_thread_report(0); }
}

static uint64_t vclock(void)
{
    uint64_t r = mono_us();
    if (!g_frame) return VBASE + (r - g_boot_real);   // boot: real elapsed time from a fixed base
    if (g_stall_step && r - g_last_tick_real > 250000) {   // no frame for 250 ms: let time move (logged)
        g_last_tick_real = r;
        g_vnow += 250000;
        long n = InterlockedIncrement(&g_stalls);
        if (n <= 20 || (n & 255) == 0) raven_log("[coop] stall #%ld at frame %u: clock stepped 250 ms\n", n, g_frame);
    }
    return g_vnow;
}

static SV uint64_t time_us_hook(void)
{
    void *ra = __builtin_return_address(0);
    raven_time_trace("time_us", ra);
    if (!g_vc) return mono_us();
    if ((uint64_t)ra == FRAME_TICK_RA) return g_vnow;   // the frame update itself: exact frame time
    return vclock();
}

// Frame-time update 0x54cdc0(int): the main loop calls it every pass, in attract mode too.
// Entry hook: tick, then run the original through a trampoline holding its first 17 bytes
// (push r12/rbp/rbx; mov ebx,edi; sub rsp,0x10; cmpl $1,timeVelType(%rip)), with the rip-relative
// cmp rewritten to an absolute address. Flags survive the jump back to 0x54cdd1 (je).
#define FRAME_UPDATE 0x54cdc0ULL
typedef SV int (*frame_update_fn)(int);
static frame_update_fn g_frame_update_orig;
static SV int frame_update_hook(int a)
{
    if (g_vc) frame_tick();
    return g_frame_update_orig(a);
}

static void hook_frame_update(void)
{
    static const unsigned char expect[17] = {0x41,0x54,0x55,0x53,0x89,0xfb,0x48,0x83,0xec,0x10,
                                             0x83,0x3d,0x0f,0xbe,0x19,0x01,0x01};
    unsigned char *f = (unsigned char *)FRAME_UPDATE;
    if (memcmp(f, expect, 17)) { raven_log("[coop] unexpected bytes at frame update, not hooking\n"); return; }
    unsigned char *t = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE), *p = t;
    memcpy(p, f, 10); p += 10;                                   // pushes, mov, sub
    uint64_t var = 0x16e8be0ULL;                                 // timeVelType
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &var, 8); p += 8;        // movabs $var,%rax
    *p++ = 0x83; *p++ = 0x38; *p++ = 0x01;                       // cmpl $1,(%rax)
    uint64_t back = FRAME_UPDATE + 17;
    *p++ = 0xff; *p++ = 0x25; memset(p, 0, 4); p += 4; memcpy(p, &back, 8);
    g_frame_update_orig = (frame_update_fn)t;
    hook(FRAME_UPDATE, expect, 17, (void *)frame_update_hook, "frame update");
}

static SV uint64_t time_ms_hook(void)
{
    raven_time_trace("time_ms", __builtin_return_address(0));
    return (g_vc ? vclock() : mono_us()) / 1000;
}

static SV uint32_t getticks_hook(void)
{
    raven_time_trace("SDL_GetTicks", __builtin_return_address(0));
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint32_t)((c.QuadPart - g_t0.QuadPart) * 1000 / g_freq.QuadPart);
}

static void hook(uint64_t addr, const unsigned char *expect, size_t n, void *target, const char *name)
{
    unsigned char *f = (unsigned char *)addr;
    if (memcmp(f, expect, n)) { raven_log("[coop] unexpected bytes at %s, not hooking\n", name); return; }
    DWORD old;
    VirtualProtect(f, 14, PAGE_EXECUTE_READWRITE, &old);
    uint64_t t = (uint64_t)target;
    f[0] = 0xff; f[1] = 0x25; memset(f + 2, 0, 4);   // jmp *0(%rip)
    memcpy(f + 6, &t, 8);
}

// ---- RAVEN_WATCH=hexaddr[,hexaddr..] (up to 4): log which code writes those 4-byte fields (main thread) ----
static HANDLE g_main_thread;
static uint64_t g_watch[4];
static int g_nwatch;
static struct { uint64_t rip; int idx; long n; } g_whits[128];
static LONG WINAPI watch_veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    int idx = -1;
    for (int i = 0; i < g_nwatch; i++) if (c->Dr6 & (1ULL << i)) idx = i;
    if (idx < 0) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    for (int i = 0; i < 128; i++) {
        if (g_whits[i].rip == c->Rip && g_whits[i].idx == idx) { g_whits[i].n++; break; }
        if (!g_whits[i].rip) {
            g_whits[i].rip = c->Rip; g_whits[i].idx = idx; g_whits[i].n = 1;
            char buf[400]; int o = 0;
            uint64_t *sp = (uint64_t *)c->Rsp;
            for (int k = 0, f = 0; k < 400 && f < 10; k++)
                if (sp[k] >= 0x400000 && sp[k] < 0x1200000) { o += snprintf(buf + o, sizeof buf - o, " %llx", (unsigned long long)sp[k]); f++; }
            raven_log("[watch] %llx written (now %08x) before rip %llx, frame %u, vclock %llu, stack:%s\n",
                      (unsigned long long)g_watch[idx], *(uint32_t *)g_watch[idx], (unsigned long long)c->Rip, g_frame,
                      (unsigned long long)g_vnow, buf);
            break;
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
static DWORD WINAPI watch_setter(void *p)
{
    (void)p;
    SuspendThread(g_main_thread);
    CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(g_main_thread, &c);
    c.Dr7 = 0;
    for (int i = 0; i < g_nwatch; i++) {
        (&c.Dr0)[i] = g_watch[i];
        c.Dr7 |= (1ULL << (i * 2)) | (0x1ULL << (16 + i * 4)) | (0x3ULL << (18 + i * 4));   // local enable, write, 4 bytes
    }
    SetThreadContext(g_main_thread, &c);
    ResumeThread(g_main_thread);
    return 0;
}
static void coop_game_thread_watch(void);
void coop_game_thread(void)   // main.c: start of the game's main thread
{
    g_main_tid = GetCurrentThreadId();
    coop_game_thread_watch();
}
static void coop_game_thread_watch(void)
{
    const char *w = raven_env("RAVEN_WATCH");
    if (!w) return;
    while (*w && g_nwatch < 4) {
        char *e; g_watch[g_nwatch++] = strtoull(w, &e, 16);
        w = *e ? e + 1 : e;
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    AddVectoredExceptionHandler(1, watch_veh);
    WaitForSingleObject(CreateThread(NULL, 0, watch_setter, NULL, 0, NULL), INFINITE);
    raven_log("[watch] %d write watchpoints set on the main thread\n", g_nwatch);
}

// Called from main.c after rio2pad_install.
void coop_install(void)
{
    const char *tt = raven_env("RAVEN_TIMETRACE"), *hl = raven_env("RAVEN_HASHLOG"), *vc = raven_env("RAVEN_VCLOCK");
    raven_time_trace_enable(tt && *tt == '1');
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_vc = rio2pad_coop_mode() || (vc && *vc == '1');
    g_vnow = VBASE;
    g_last_tick_real = g_boot_real = mono_us();
    const char *e;
    if ((e = raven_env("RAVEN_THREADLOG"))) g_threadlog = (unsigned)atoi(e);
    if ((e = raven_env("RAVEN_GATE"))) g_gate = atoi(e);
    if ((e = raven_env("RAVEN_VSTALL")) && *e == '1') g_stall_step = 1;
    if ((e = raven_env("RAVEN_PAGEHASH"))) g_pagelog = fopen(e, "wb");
    if ((e = raven_env("RAVEN_MEMDUMP")) && strchr(e, ',')) { g_memdump_frame = (unsigned)atoi(e); g_memdump_file = strchr(e, ',') + 1;
        char *c2 = strchr(g_memdump_file, ',');
        if (c2) { char *d = _strdup(g_memdump_file); d[c2 - g_memdump_file] = 0; g_memdump_file = d; g_memdump_n = (unsigned)atoi(c2 + 1); } }
    if (hl && !(g_hashlog = fopen(hl, "w"))) raven_log("[coop] cannot write hash log %s\n", hl);

    static const unsigned char clk[9] = {0x48, 0x83, 0xec, 0x28, 0xbf, 0x01, 0x00, 0x00, 0x00};
    static const unsigned char sdl[4] = {0x48, 0x83, 0xec, 0x28};
    hook(0x909320ULL, clk, 9, (void *)time_us_hook, "time_us");
    hook(0x909460ULL, clk, 9, (void *)time_ms_hook, "time_ms");
    hook(0x6546e0ULL, sdl, 4, (void *)getticks_hook, "SDL_GetTicks");
    hook_frame_update();
    raven_log("[coop] clocks hooked, virtual clock %s, hash log %s\n", g_vc ? "ON" : "off", g_hashlog ? hl : "off");
}
