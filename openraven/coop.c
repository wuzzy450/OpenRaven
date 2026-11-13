// Coop lockstep, Linux side (part of rio2pad.so): control of the game's clocks, the per-frame tick,
// the state hashes and the thread gate. Port of win/coop_win.c + the gate in win/shim_thread.c; the
// game code hooks are at the same addresses (same game binary), the OS side is LD_PRELOAD
// interposition instead of the Windows loader's import shims.
//
// The game reads time through three functions, all replaced here:
//   0x909320  time_us()      CLOCK_MONOTONIC in microseconds
//   0x909460  time_ms()      CLOCK_MONOTONIC in milliseconds
//   0x6546e0  SDL_GetTicks   (statically linked SDL; left real)
// plus the libc wall clock (time, gettimeofday, clock_gettime REALTIME), interposed below for calls
// from the game itself only (Mesa, ALSA and our own code keep real time).
// The frame-time update 0x54cdc0 (called from the main loop) is our frame tick.
//
// Virtual clock (on with RIO2PAD_RECORD / RIO2PAD_REPLAY / RAVEN_COOP, or RAVEN_VCLOCK=1): time_us /
// time_ms return a clock that only moves at frame ticks, by exactly 1/60 s, so every copy of the game
// sees the same times. RAVEN_HASHLOG=file writes "frame hash" per frame. RAVEN_GATE=n (default 2):
// turns per frame for the game's polling threads.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "win/coop.h"

#define FRAME_TICK_RA 0x54ce07UL      // return address of the frame-time update's time_us call
#define FRAME_US      16667ULL
#define VBASE 1000000000ULL           // virtual clock origin (us), the same on every machine
#define VEPOCH 1541030400ULL          // 2018-11-01 00:00 UTC: wall clock origin in virtual-clock mode
#define DATA_LO 0x1661000UL
#define DATA_HI 0x23d3000UL

int rio2pad_coop_mode(void);          // rio2pad.c
void rio2pad_coop_frame(unsigned frame);

static int g_vc;
static uint64_t g_boot_real;
static volatile uint64_t g_vnow;
static volatile uint64_t g_last_tick_real;
static volatile unsigned g_frame;
static FILE *g_hashlog, *g_pagelog;
static volatile long g_stalls;
static int g_gate = 2, g_stall_step;
static pid_t g_main_tid;
static volatile uint64_t g_pre_us;

static pid_t tid(void) { return (pid_t)syscall(SYS_gettid); }

static uint64_t mono_us(void)
{
    struct timespec ts;
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);   // never our own interposed version
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

// ---- which code is the game's: the game image, plus halo.so (the game's own library) ----
static uintptr_t g_halo_lo, g_halo_hi;
static int phdr_cb(struct dl_phdr_info *i, size_t sz, void *d)
{
    (void)sz; (void)d;
    if (!i->dlpi_name || !strstr(i->dlpi_name, "halo.so")) return 0;
    for (int k = 0; k < i->dlpi_phnum; k++) {
        if (i->dlpi_phdr[k].p_type != PT_LOAD) continue;
        uintptr_t lo = i->dlpi_addr + i->dlpi_phdr[k].p_vaddr, hi = lo + i->dlpi_phdr[k].p_memsz;
        if (!g_halo_lo || lo < g_halo_lo) g_halo_lo = lo;
        if (hi > g_halo_hi) g_halo_hi = hi;
    }
    return 1;
}
static int in_game(const void *ra)
{
    uintptr_t a = (uintptr_t)ra;
    return (a >= 0x400000 && a < DATA_HI) || (a >= g_halo_lo && a < g_halo_hi);
}

// Wall clock for the libc time functions. During boot: the real one (the 4-player boot compares it
// with real dates and stalled on a fixed 2018 date). From the first frame tick: a fixed date (VEPOCH,
// as on Windows) moved only by frames, the same on every PC.
static void coop_presleep(unsigned us) { (void)us; }
static int coop_vtime_us(uint64_t *us)
{
    if (!g_vc || !g_frame) return 0;
    *us = VEPOCH * 1000000ULL + (g_vnow - VBASE - 30000000ULL);
    return 1;
}

// ---- state hash (same fields as Windows, so Linux and Windows PCs compare the same thing) ----
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
    return h;
}
uint64_t coop_state_hash(void)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 4; i++) {
        const unsigned char *s = (const unsigned char *)(0x1c0c7c0UL + i * 0x250UL);
        h = fnv(h, s, 0x20);
        h = fnv(h, s + 0x30, 0x250 - 0x30);
    }
    const uint64_t *acc = (const uint64_t *)0x17f85c0UL;
    h = fnv(h, &acc[0], 8);
    return fnv(h, &acc[2], 8);
}
static const struct { uintptr_t addr; size_t len; } g_deep[] = {
    {0x169c000, 0x1000}, {0x16a7000, 0x1000}, {0x16e9000, 0x3000}, {0x176d000, 0x2000},
    {0x177c000, 0x1000}, {0x1790000, 0x1000}, {0x17f7000, 0x1000}, {0x1c0b000, 0x1000},
    {0x1c17000, 0x1000}, {0x1cbf000, 0x2000},
};
uint64_t coop_deep_hash(void)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned i = 0; i < sizeof g_deep / sizeof *g_deep; i++) h = fnv(h, (const void *)g_deep[i].addr, g_deep[i].len);
    return h;
}
static void frame_hash(void)
{
    if (g_hashlog) {
        fprintf(g_hashlog, "%u %016llx %016llx\n", g_frame, (unsigned long long)fnv(0xcbf29ce484222325ULL, (void *)0x1c0c7c0UL, 4 * 0x250),
                (unsigned long long)fnv(0xcbf29ce484222325ULL, (void *)0x17f85c0UL, 0x20));
        static unsigned dump_at = (unsigned)-1;
        if (dump_at == (unsigned)-1) { const char *e = getenv("RAVEN_HASHDUMP"); dump_at = e ? (unsigned)atoi(e) : 0; }
        if (dump_at && g_frame % dump_at == 0) {   // RAVEN_HASHDUMP=n: raw player state every n frames
            fprintf(g_hashlog, "#dump %u", g_frame);
            const unsigned char *b = (const unsigned char *)0x1c0c7c0UL;
            for (int k = 0; k < 4 * 0x250; k++) fprintf(g_hashlog, " %02x", b[k]);
            fputc('\n', g_hashlog);
        }
        if ((g_frame & 63) == 0) fflush(g_hashlog);
    }
    if (g_pagelog && g_frame % 50 == 0) {   // RAVEN_PAGEHASH=file: hash of every 4 KB data page every 50 frames
        unsigned f = g_frame;
        fwrite(&f, 4, 1, g_pagelog);
        for (uintptr_t a = DATA_LO; a < DATA_HI; a += 4096) {
            const uint64_t *w = (const uint64_t *)a;
            uint64_t h = 0x9e3779b97f4a7c15ULL;
            for (int i = 0; i < 512; i++) h = (h ^ w[i]) * 0xff51afd7ed558ccdULL, h ^= h >> 29;
            fwrite(&h, 8, 1, g_pagelog);
        }
        fflush(g_pagelog);
    }
}

// ---- thread gate (lockstep determinism), as win/shim_thread.c ----
// The game has helper threads that poll in a loop with short sleeps (movie player, loaders...). Free
// running, they act at slightly different moments on every PC. With the gate on, such a thread stops
// at each short sleep and gets a fixed number of turns per game frame, one thread at a time, while the
// main thread waits at its frame tick. While the main thread itself sleeps the helpers run.
struct gthread {
    void *(*fn)(void *);
    void *arg;
    volatile int alive, gated;
    volatile long permits, iters;
};
#define MAX_THREADS 512
static struct gthread *g_threads[MAX_THREADS];
static volatile int g_nthreads;
static __thread struct gthread *t_self;
static volatile int g_gate_on, g_gate_open;
static volatile long g_gate_timeouts;

static int gate_is_main(void) { return g_gate_on && tid() == g_main_tid; }
static int gate_sleep(unsigned us)   // 1 = the sleep was replaced by a wait at the gate
{
    struct gthread *t = t_self;
    if (!g_gate_on || !t || !t->alive || us > 2000) return 0;
    t->gated = 1;
    __atomic_add_fetch(&t->iters, 1, __ATOMIC_SEQ_CST);
    for (;;) {
        if (!g_gate_on) break;
        long p = t->permits;
        if (p > 0 && __atomic_compare_exchange_n(&t->permits, &p, p - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
        if (g_gate_open > 0) { struct timespec s = {0, 1000000}; syscall(SYS_nanosleep, &s, NULL); break; }
        struct timespec s = {0, 100000};
        syscall(SYS_nanosleep, &s, NULL);
    }
    return 1;
}
static void gate_tick(int turns)
{
    for (int i = 0; i < g_nthreads && i < MAX_THREADS; i++) {
        struct gthread *t = g_threads[i];
        if (!t || !t->gated) continue;
        for (int k = 0; k < turns && t->alive; k++) {
            long it = t->iters;
            __atomic_add_fetch(&t->permits, 1, __ATOMIC_SEQ_CST);
            uint64_t t0 = mono_us();
            while (t->iters == it && t->alive) {
                sched_yield();
                if (mono_us() - t0 > 5000000) {
                    if (++g_gate_timeouts <= 20) fprintf(stderr, "[gate] thread fn %p did not come back in 5 s, going on\n", (void *)t->fn);
                    break;
                }
            }
        }
    }
}
static void *thread_start(void *p)
{
    struct gthread *t = p;
    t_self = t;
    t->alive = 1;
    void *r = t->fn(t->arg);
    t->alive = 0;
    return r;
}
int pthread_create(pthread_t *th, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (!real) real = dlsym(RTLD_NEXT, "pthread_create");
    if (!g_vc || !in_game((void *)fn) || g_nthreads >= MAX_THREADS) return real(th, a, fn, arg);
    struct gthread *t = calloc(1, sizeof *t);
    t->fn = fn; t->arg = arg;
    g_threads[__atomic_fetch_add(&g_nthreads, 1, __ATOMIC_SEQ_CST)] = t;
    return real(th, a, thread_start, t);
}

// Sleeps: helpers wait at the gate, the main thread lets every helper run once.
static volatile long dbg_n[8];   // RAVEN_CLOCKDBG: calls of time, gettimeofday, clock_gettime(real), sleeps, cond/sem waits
static int sleep_hook(unsigned us, const void *ra)
{
    if (in_game(ra)) dbg_n[3]++;
    if (!g_vc || !in_game(ra)) return 0;
    if (gate_sleep(us)) return 1;
    if (gate_is_main()) gate_tick(1);
    coop_presleep(us);
    return 0;
}
int usleep(useconds_t us)
{
    static int (*real)(useconds_t);
    if (!real) real = dlsym(RTLD_NEXT, "usleep");
    if (sleep_hook(us, __builtin_return_address(0))) return 0;
    return real(us);
}
int nanosleep(const struct timespec *r, struct timespec *rem)
{
    static int (*real)(const struct timespec *, struct timespec *);
    if (!real) real = dlsym(RTLD_NEXT, "nanosleep");
    if (r && sleep_hook(r->tv_sec ? 1000000u * (unsigned)r->tv_sec : (unsigned)(r->tv_nsec / 1000), __builtin_return_address(0))) return 0;
    return real(r, rem);
}

// ---- the game's own networking ----
// The Windows loader gives the game no network (its socket calls fail). On Linux the cabinet-link code
// would really talk: copies of the game find each other on the network, which changes the game state
// on one PC only (4-player cabinet desync in the same-PC test). In coop, fail them like Windows does.
int socket(int domain, int type, int proto)
{
    static int (*real)(int, int, int);
    if (!real) real = dlsym(RTLD_NEXT, "socket");
    if (g_vc && in_game(__builtin_return_address(0))) {
        static int said;
        if (!said++) fprintf(stderr, "[coop] the game's own network calls are off (as on Windows)\n");
        errno = EACCES;
        return -1;
    }
    return real(domain, type, proto);
}

// ---- wall clock ----
time_t time(time_t *t)
{
    uint64_t vu;
    time_t v;
    if (in_game(__builtin_return_address(0))) dbg_n[0]++;
    if (in_game(__builtin_return_address(0)) && coop_vtime_us(&vu)) v = (time_t)(vu / 1000000);
    else { struct timespec ts; syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts); v = ts.tv_sec; }
    if (t) *t = v;
    return v;
}
int gettimeofday(struct timeval *tv, void *tz)
{
    uint64_t vu;
    (void)tz;
    if (!tv) return 0;
    if (in_game(__builtin_return_address(0))) dbg_n[1]++;
    if (in_game(__builtin_return_address(0)) && coop_vtime_us(&vu)) { tv->tv_sec = vu / 1000000; tv->tv_usec = vu % 1000000; return 0; }
    struct timespec ts;
    syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts);
    tv->tv_sec = ts.tv_sec; tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}
int clock_gettime(clockid_t clk, struct timespec *ts)
{
    static int (*real)(clockid_t, struct timespec *);
    if (!real) real = dlsym(RTLD_NEXT, "clock_gettime");
    uint64_t vu;
    if (in_game(__builtin_return_address(0))) dbg_n[clk == CLOCK_REALTIME || clk == CLOCK_REALTIME_COARSE ? 2 : 5]++;
    if ((clk == CLOCK_REALTIME || clk == CLOCK_REALTIME_COARSE) && ts && in_game(__builtin_return_address(0)) && coop_vtime_us(&vu)) {
        ts->tv_sec = vu / 1000000; ts->tv_nsec = (vu % 1000000) * 1000;
        return 0;
    }
    return real(clk, ts);
}

// ---- game clocks and the frame tick ----
void coopnet_start(void);
static void import_slots(const char *when);
static int patch_import_slots(void);
static void frame_tick(void)
{
    if (!g_frame) {
        import_slots("first frame");
        coopnet_start();   // online: waits here until every PC is ready
        g_vnow = VBASE + 30000000ULL;
        // The game-time accumulators ran on real time during boot: give them fixed values, as if boot
        // had taken exactly 10 s.
        uint64_t *acc = (uint64_t *)0x17f85c0UL;
        acc[0] = 10000; acc[1] = g_vnow / 1000; acc[2] = 10000000; acc[3] = g_vnow;
        *(uint64_t *)0x1e519e0UL = g_vnow / 1000 - 10000;   // a start time taken from time_ms during boot
        if (g_gate) g_gate_on = 1;
        fprintf(stderr, "[coop] first frame tick: virtual clock starts\n");
    }
    g_frame++;
    g_vnow += FRAME_US;
    if (g_gate) gate_tick(g_gate);
    g_last_tick_real = mono_us();
    rio2pad_coop_frame(g_frame);
    frame_hash();
}

static uint64_t vclock(void)
{
    uint64_t r = mono_us();
    if (!g_frame) return VBASE + (r - g_boot_real);   // boot: real elapsed time from a fixed base
    if (g_stall_step && r - g_last_tick_real > 250000) {   // no frame for 250 ms: let time move (logged)
        g_last_tick_real = r;
        g_vnow += 250000;
        long n = __atomic_add_fetch(&g_stalls, 1, __ATOMIC_SEQ_CST);
        if (n <= 20 || (n & 255) == 0) fprintf(stderr, "[coop] stall #%ld at frame %u: clock stepped 250 ms\n", n, g_frame);
    }
    return g_vnow;
}

static uint64_t time_us_hook(void)
{
    if (!g_vc) return mono_us();
    dbg_n[7]++;
    { static uint64_t last; static int on = -1; if (on < 0) on = getenv("RAVEN_CLOCKDBG") != NULL;
      uint64_t r = mono_us(); if (on && r - last > 2000000) { last = r; uint64_t vu = 0; coop_vtime_us(&vu);
        fprintf(stderr, "[clockdbg] frame %u wall %llu s pre %llu ms | time %ld gtod %ld cgt-real %ld sleeps %ld waits %ld cgt-mono %ld time_ms %ld time_us %ld\n",
                g_frame, (unsigned long long)(vu / 1000000), (unsigned long long)(g_pre_us / 1000), dbg_n[0], dbg_n[1], dbg_n[2], dbg_n[3], dbg_n[4], dbg_n[5], dbg_n[6], dbg_n[7]); } }
    // the import table fills in during boot: keep pointing new slots at us until the first frame
    if (!g_frame && tid() == g_main_tid) { static int calls; if (++calls % 16 == 1) { int n = patch_import_slots(); if (n) fprintf(stderr, "[coop] %d game import slot(s) -> virtual clock / gate (boot, time_us call %d)\n", n, calls); } }
    if ((uintptr_t)__builtin_return_address(0) == FRAME_TICK_RA) return g_vnow;   // the frame update itself
    return vclock();
}
static uint64_t time_ms_hook(void) { dbg_n[6]++; return (g_vc ? vclock() : mono_us()) / 1000; }

static void write_abs_jmp(unsigned char *p, uint64_t target)
{
    p[0] = 0xff; p[1] = 0x25; memset(p + 2, 0, 4);   // jmp *0(%rip)
    memcpy(p + 6, &target, 8);
}
static int hook(uintptr_t addr, const unsigned char *expect, size_t n, void *target, const char *name)
{
    unsigned char *f = (unsigned char *)addr;
    if (memcmp(f, expect, n)) { fprintf(stderr, "[coop] unexpected bytes at %s, not hooking\n", name); return 0; }
    mprotect((void *)(addr & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    write_abs_jmp(f, (uint64_t)target);
    return 1;
}

// Frame-time update 0x54cdc0(int): entry hook, then the original through a trampoline holding its
// first 17 bytes, with the rip-relative cmp rewritten to an absolute address.
#define FRAME_UPDATE 0x54cdc0UL
typedef int (*frame_update_fn)(int);
static frame_update_fn g_frame_update_orig;
static int frame_update_hook(int a)
{
    if (g_vc) frame_tick();
    return g_frame_update_orig(a);
}
static void hook_frame_update(void)
{
    static const unsigned char expect[17] = {0x41,0x54,0x55,0x53,0x89,0xfb,0x48,0x83,0xec,0x10,
                                             0x83,0x3d,0x0f,0xbe,0x19,0x01,0x01};
    if (memcmp((void *)FRAME_UPDATE, expect, 17)) { fprintf(stderr, "[coop] unexpected bytes at frame update, not hooking\n"); return; }
    unsigned char *t = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), *p = t;
    memcpy(p, (void *)FRAME_UPDATE, 10); p += 10;                // pushes, mov, sub
    uint64_t var = 0x16e8be0ULL;                                 // timeVelType
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &var, 8); p += 8;        // movabs $var,%rax
    *p++ = 0x83; *p++ = 0x38; *p++ = 0x01;                       // cmpl $1,(%rax)
    write_abs_jmp(p, FRAME_UPDATE + 17);
    g_frame_update_orig = (frame_update_fn)t;
    hook(FRAME_UPDATE, expect, 17, (void *)frame_update_hook, "frame update");
}

// The game's protection wrapper resolves libc itself, so the functions above (same names as libc's)
// are not what the game calls. Point the game's own import slots at them; repeated until the table
// is filled in (it may be after our constructor). Returns how many slots were changed.
static int patch_import_slots(void)
{
    // (timed waits are left alone: their deadlines are not all on the clock the game reads, and moving
    // them stalled the 4-player boot)
    enum { N = 7 };
    static const char *names[N] = {"time", "gettimeofday", "clock_gettime", "usleep", "nanosleep", "pthread_create", "socket"};
    void *mine[N] = {(void *)time, (void *)gettimeofday, (void *)clock_gettime, (void *)usleep, (void *)nanosleep, (void *)pthread_create, (void *)socket};
    void *real[N];
    for (int k = 0; k < N; k++) real[k] = dlsym(RTLD_NEXT, names[k]);
    const char *skip = getenv("RAVEN_SLOTSKIP");   // debug: comma list of names to leave alone
    for (int k = 0; k < N; k++) if (skip && strstr(skip, names[k])) real[k] = NULL;
    int n = 0;
    for (uint64_t *q = (uint64_t *)0x1690000UL; q < (uint64_t *)0x1692800UL; q++)
        for (int k = 0; k < N; k++)
            if (real[k] && *q == (uint64_t)real[k]) { *q = (uint64_t)mine[k]; n++; }
    return n;
}
static void import_slots(const char *when)
{
    static int total;
    int n = patch_import_slots();
    if (n) { total += n; fprintf(stderr, "[coop] %d game import slot(s) -> virtual clock / gate (%s, %d so far)\n", n, when, total); }
}

// Called from rio2pad.c's constructor (main thread), after coop_open.
void coop_install(void)
{
    const char *e;
    g_main_tid = tid();
    dl_iterate_phdr(phdr_cb, NULL);
    g_vc = rio2pad_coop_mode() || ((e = getenv("RAVEN_VCLOCK")) && *e == '1');
    g_vnow = VBASE;
    g_last_tick_real = g_boot_real = mono_us();
    if ((e = getenv("RAVEN_GATE"))) g_gate = atoi(e);
    if ((e = getenv("RAVEN_VSTALL")) && *e == '1') g_stall_step = 1;
    if ((e = getenv("RAVEN_PAGEHASH"))) g_pagelog = fopen(e, "wb");
    if ((e = getenv("RAVEN_HASHLOG")) && !(g_hashlog = fopen(e, "w"))) fprintf(stderr, "[coop] cannot write hash log %s\n", e);
    static const unsigned char clk[9] = {0x48, 0x83, 0xec, 0x28, 0xbf, 0x01, 0x00, 0x00, 0x00};
    hook(0x909320UL, clk, 9, (void *)time_us_hook, "time_us");
    hook(0x909460UL, clk, 9, (void *)time_ms_hook, "time_ms");
    hook_frame_update();
    if (g_vc) import_slots("start");
    fprintf(stderr, "[coop] clocks hooked, virtual clock %s, halo.so %#lx-%#lx\n", g_vc ? "ON" : "off",
            (unsigned long)g_halo_lo, (unsigned long)g_halo_hi);
}
