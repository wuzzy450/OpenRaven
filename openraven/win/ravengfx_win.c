// Windows port of ravengfx.c (graphics settings). Same hooks on the 3.11-dec game:
//   cfg_get_int @0x924650 / cfg_get_float @0x9247a0 -> values from openraven/graphics.ini
//   video_set_mode @0x4fb730 -> RENDER_SIZE / RENDER_SCALE (render size != window size)
//   projection @0x49346e/0x4932d1 -> Hor+ for non-16:9 screens, FOV_SCALE
//   aspect lock `ja` @0x417e62 -> NOP (ultrawide can boot)
// The launcher part of play_halo.sh is here too: `resolution` (WxH or native) -> game args -fWxH -n.
// Per-frame work (VSYNC, FPS_CAP, Hor+ refresh) runs from s_glXSwapBuffers via ravengfx_frame().
// Settings: RAVENGFX_CONF (ini path), RAVENGFX_KEYLOG (file: every key the game reads),
// RAVENGFX_RENDER=WxH, RAVENGFX_SWAPINTERVAL, RAVENGFX_FPSCAP, RAVEN_RES=WxH (wins over ini).
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"

#define CFG_GET_INT    0x924650ULL
#define CFG_GET_FLOAT  0x9247a0ULL
#define ASPECT_LOCK_JA 0x417e62ULL
#define VIDEO_SET_MODE 0x4fb730ULL
#define VIDEO_SET_BODY 0x4fb739ULL
#define PROJ_CONT      0x49287cULL

static const uint8_t int_prologue[14] = {0x41,0x54,0x55,0x41,0x89,0xd4,0x53,0x48,0x83,0xec,0x10,0x48,0x85,0xff};
static const uint8_t flt_prologue[15] = {0x55,0x53,0x48,0x83,0xec,0x28,0x48,0x85,0xff,0xf3,0x0f,0x11,0x44,0x24,0x0c};

typedef SV int (*get_int_fn)(void *, const char *, int);
typedef SV float (*get_flt_fn)(void *, const char *, float);
static get_int_fn orig_int;
static get_flt_fn orig_flt;

#define MAXKV 256
static struct { char key[64]; char val[64]; } kv[MAXKV];
static int nkv;
static FILE *keylog;
static CRITICAL_SECTION loglock;
char ravengfx_conf_path[MAX_PATH * 2];
int ravengfx_win_w, ravengfx_win_h;
int ravengfx_res_w, ravengfx_res_h;   // the game's screen size (-fWxH)
float ravengfx_fg_fps;   // ravenfg.c: shown frames per second (real + generated)
void ravengfx_capture(const char *path) { raven_log("[ravengfx] capture %s: not on Windows yet\n", path); }
float ravengfx_fps;

static const char *lookup(const char *key) {
    if (!key) return NULL;
    int n = __atomic_load_n(&nkv, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (!strcmp(kv[i].key, key)) return kv[i].val;
    return NULL;
}
const char *ravengfx_get(const char *key) { return lookup(key); }
void ravengfx_set(const char *key, const char *val) {
    for (int i = 0; i < nkv; i++)
        if (!strcmp(kv[i].key, key)) { snprintf(kv[i].val, sizeof kv[0].val, "%s", val); return; }
    if (nkv >= MAXKV) return;
    snprintf(kv[nkv].key, sizeof kv[0].key, "%s", key);
    snprintf(kv[nkv].val, sizeof kv[0].val, "%s", val);
    __atomic_store_n(&nkv, nkv + 1, __ATOMIC_RELEASE);
}

static void logkey(const char *t, const char *key, const char *got, int ov) {
    if (!keylog || !key) return;
    EnterCriticalSection(&loglock);
    fprintf(keylog, "%s %s = %s%s\n", t, key, got, ov ? "   (override)" : "");
    fflush(keylog);
    LeaveCriticalSection(&loglock);
}
static SV int hook_int(void *cfg, const char *key, int def) {
    const char *o = lookup(key);
    int v = o ? (int)strtol(o, NULL, 0) : orig_int(cfg, key, def);
    if (keylog) { char b[48]; snprintf(b, sizeof b, "%d (def %d)", v, def); logkey("int", key, b, o != NULL); }
    return v;
}
static SV float hook_flt(void *cfg, const char *key, float def) {
    const char *o = lookup(key);
    float v = o ? strtof(o, NULL) : orig_flt(cfg, key, def);
    if (keylog) { char b[64]; snprintf(b, sizeof b, "%g (def %g)", v, def); logkey("flt", key, b, o != NULL); }
    return v;
}

// ---- patching helpers ----
static void write_abs_jmp(uint8_t *p, uint64_t to) {
    p[0] = 0xff; p[1] = 0x25; memset(p + 2, 0, 4);  // jmp *0(%rip)
    memcpy(p + 6, &to, 8);
}
static void make_rwx(uint64_t at, size_t n) {
    DWORD old;
    if (!VirtualProtect((void *)at, n, PAGE_EXECUTE_READWRITE, &old))
        raven_log("[ravengfx] VirtualProtect(%#llx) failed: %lu\n", (unsigned long long)at, GetLastError());
}
static uint8_t *exec_page(void) { return VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); }
// A page a rel32 from the game (0x400000..) can reach. 0x400000..0x4000000 is the loader's reserve.
static uint8_t *low_page(void) {
    for (uint64_t at = 0x10000000ULL; at < 0x70000000ULL; at += 0x1000000ULL) {
        uint8_t *t = VirtualAlloc((void *)at, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (t) return t;
    }
    return NULL;
}
// Copy the (position-independent) prologue into a trampoline, then jump back after it.
static void *hook(uint64_t at, const uint8_t *pro, size_t n, void *to) {
    if (memcmp((void *)at, pro, n)) return NULL;
    uint8_t *t = exec_page();
    if (!t) return NULL;
    memcpy(t, pro, n);
    write_abs_jmp(t + n, at + n);
    make_rwx(at, 16);
    write_abs_jmp((uint8_t *)at, (uint64_t)to);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, 16);
    return t;
}

static void load_conf(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { raven_log("[ravengfx] no config at %s, using game defaults\n", path); return; }
    char line[256];
    while (fgets(line, sizeof line, f) && nkv < MAXKV) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == ';' || *s == '\n' || *s == '\r' || !*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = s, *v = eq + 1, *e;
        for (e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
        while (*v == ' ' || *v == '\t') v++;
        if ((e = strchr(v, '#'))) *e = 0;
        for (e = v + strlen(v) - 1; e >= v && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t'); e--) *e = 0;
        if (!*k || !*v) continue;
        snprintf(kv[nkv].key, sizeof kv[0].key, "%s", k);
        snprintf(kv[nkv].val, sizeof kv[0].val, "%s", v);
        nkv++;
    }
    fclose(f);
    raven_log("[ravengfx] %d setting(s) from %s\n", nkv, path);
}

// ---- video mode (render size) ----
static int render_w, render_h;
typedef SV int (*set_mode_fn)(void *);
static set_mode_fn set_mode_body;
static SV int hook_set_mode(uint8_t *m) {
    if (!m) return -1;  // the original's null check, which the jmp replaced
    int *w = (int *)(m + 4), *h = (int *)(m + 8);
    raven_log("[ravengfx] video mode %dx%d aspect %.4f alt %dx%d\n", *w, *h, *(float *)(m + 0xc),
              *(int *)(m + 0x18), *(int *)(m + 0x1c));
    ravengfx_win_w = *w; ravengfx_win_h = *h;
    const char *rs = lookup("RENDER_SCALE"), *rz = lookup("RENDER_SIZE");
    if (render_w <= 0 && rz) sscanf(rz, "%dx%d", &render_w, &render_h);
    if (render_w <= 0 && rs && strtof(rs, NULL) > 0 && strtof(rs, NULL) != 1.0f) {
        float k = strtof(rs, NULL);
        render_w = (int)(*w * k + 0.5f) & ~1; render_h = (int)(*h * k + 0.5f) & ~1;
    }
    if (render_w > 0 && render_h > 0) {
        raven_log("[ravengfx] render size %dx%d\n", render_w, render_h);
        *(int *)(m + 0x18) = render_w; *(int *)(m + 0x1c) = render_h;
    }
    return set_mode_body(m);
}

// ---- Hor+ / FOV on the camera projection (see ravengfx.c) ----
uint64_t proj_ret = PROJ_CONT;
static float proj_aspect, proj_fovk = 1.0f;
static int proj_on, proj_wide4;
// Hybrid 4-player mode (RAVEN_WIDE4=hybrid): the game runs at the screen's own shape (16:9),
// where everything authored for the cabinet's 32:9 screen comes out squeezed. The loader picks
// a view per frame (raven_view, read by shim_glx.c / ravenhdr_win.c / gl_hooks.c / rio2pad_win.c):
//  1 menu    - full picture, background camera vert+ (like missions); the menu's 2D layer is
//              drawn twice as wide (gl_hooks.c), so its left copy - one whole cabinet screen -
//              fills the screen once in its right shape and the right copy falls off the edge;
//  2 mission - full picture, 3D camera vert+ (more above/below), the 2D layer (HUDs,
//              crosshairs) drawn into the middle 32:9 band so it keeps its shape and lines up
//              with the 3D view;
//  3 movie   - full picture; shim_core.c swaps in the 16:9 cutscene files (X_1080.webm).
//  4 wide menu - mission select: one layout across BOTH cabinet screens (6 mission cards): the
//              2D layer goes into the middle 32:9 band over the vert+ background. From condition
//              GAMESEL_CHAPTER until GAMESEL_DONE / the next game-select scene.
// Mission = the game drew its shadow map this frame (8192x8192 pass; menus/movies have none).
int raven_view = 0;
volatile int raven_shadow_seen;   // gl_hooks.c
static int proj_hybrid;
SV void proj_fix(uint8_t *cam) {
    float *p00 = (float *)(cam + 0x170), *p11 = (float *)(cam + 0x184);
    if (*p00 <= 0 || *p11 <= 0) return;
    float shape = *p11 / *p00;
    {   // RAVEN_PROCLOG debug: cameras used this frame
        extern void *dbg_cams[8]; extern int dbg_ncam;
        int k = 0; while (k < dbg_ncam && dbg_cams[k] != cam) k++;
        if (k == dbg_ncam && dbg_ncam < 8) dbg_cams[dbg_ncam++] = cam;
    }
    if (proj_hybrid && raven_view == 3) return;   // movies: cameras stay native
    if (!proj_on) return;
    // 4-player cabinet: the game's camera is 32:9 (two screens). Shown in one window it comes out tall
    // and skinny. Keep the full width of view and show more above and below (RAVEN_WIDE4FIT=crop: keep
    // the height and cut the sides instead, which looks zoomed in).
    //
    // In the native 32:9 video mode (RAVEN_WIDE4, default on for 4 players) the wide4 branch is a
    // no-op for the 32:9 camera (the mode aspect already matches) and, importantly, the 16:9 cameras
    // below must stay native: the cutscenes render each half with its own 16:9 camera into a half-size
    // viewport, so widening them to the full 32:9 would squeeze them again. 16:9 Hor+ only makes sense
    // without the 4-player mode, so return after the wide4 branch.
    if (proj_wide4) {
        if (shape > 3.4f && shape < 3.7f && proj_aspect > 0) {
            if (proj_wide4 == 2) *p00 = *p11 / proj_aspect; else *p11 = *p00 * proj_aspect;
            *p00 /= proj_fovk; *p11 /= proj_fovk;
        }
        return;
    }
    if (shape < 1.70f || shape > 1.85f) return;  // not the 16:9 camera
    if (proj_aspect > 16.0f / 9.0f) *p00 = *p11 / proj_aspect;
    else if (proj_aspect > 0) *p11 = *p00 * proj_aspect;
    *p00 /= proj_fovk; *p11 /= proj_fovk;
}
__attribute__((naked)) static void proj_stub(void) {
    __asm__ volatile(
        "push %rbp\n mov %rsp,%rbp\n and $-16,%rsp\n sub $256,%rsp\n"
        "movdqu %xmm0,0(%rsp)\n movdqu %xmm1,16(%rsp)\n movdqu %xmm2,32(%rsp)\n movdqu %xmm3,48(%rsp)\n"
        "movdqu %xmm4,64(%rsp)\n movdqu %xmm5,80(%rsp)\n movdqu %xmm6,96(%rsp)\n movdqu %xmm7,112(%rsp)\n"
        "movdqu %xmm8,128(%rsp)\n movdqu %xmm9,144(%rsp)\n movdqu %xmm10,160(%rsp)\n movdqu %xmm11,176(%rsp)\n"
        "movdqu %xmm12,192(%rsp)\n movdqu %xmm13,208(%rsp)\n movdqu %xmm14,224(%rsp)\n movdqu %xmm15,240(%rsp)\n"
        "push %rax\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n push %r8\n push %r9\n push %r10\n push %r11\n push %r11\n"
        "mov %rbx,%rdi\n call proj_fix\n"
        "pop %r11\n pop %r11\n pop %r10\n pop %r9\n pop %r8\n pop %rdi\n pop %rsi\n pop %rdx\n pop %rcx\n pop %rax\n"
        "movdqu 0(%rsp),%xmm0\n movdqu 16(%rsp),%xmm1\n movdqu 32(%rsp),%xmm2\n movdqu 48(%rsp),%xmm3\n"
        "movdqu 64(%rsp),%xmm4\n movdqu 80(%rsp),%xmm5\n movdqu 96(%rsp),%xmm6\n movdqu 112(%rsp),%xmm7\n"
        "movdqu 128(%rsp),%xmm8\n movdqu 144(%rsp),%xmm9\n movdqu 160(%rsp),%xmm10\n movdqu 176(%rsp),%xmm11\n"
        "movdqu 192(%rsp),%xmm12\n movdqu 208(%rsp),%xmm13\n movdqu 224(%rsp),%xmm14\n movdqu 240(%rsp),%xmm15\n"
        "mov %rbp,%rsp\n pop %rbp\n jmp *proj_ret(%rip)\n");
}
static void hook_proj(void) {
    static const uint64_t sites[] = {0x49346eULL, 0x4932d1ULL};
    uint8_t *t = low_page();
    if (!t) { raven_log("[ravengfx] no low memory for projection hook\n"); return; }
    write_abs_jmp(t, (uint64_t)proj_stub);
    int n = 0;
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *c = (uint8_t *)sites[i];
        int32_t rel; memcpy(&rel, c + 1, 4);
        if (c[0] != 0xe9 || (uint64_t)(c + 5) + rel != PROJ_CONT) continue;
        rel = (int32_t)((int64_t)t - (int64_t)(c + 5));
        make_rwx((uint64_t)c, 5);
        memcpy(c + 1, &rel, 4); n++;
    }
    raven_log("[ravengfx] projection hook at %d/2 sites\n", n);
}
static void proj_update(void) {
    const char *hp = lookup("HOR_PLUS"), *fv = lookup("FOV_SCALE");
    // Online coop: the host's camera settings (coopnet_preconnect), since they change every aim ray.
    if (raven_env("RAVEN_COOP_HORPLUS")) hp = raven_env("RAVEN_COOP_HORPLUS");
    if (raven_env("RAVEN_COOP_FOV")) fv = raven_env("RAVEN_COOP_FOV");
    proj_on = hp ? atoi(hp) != 0 : 1;
    proj_fovk = fv && atof(fv) > 0.3 ? (float)atof(fv) : 1.0f;
    if (ravengfx_win_h > 0) proj_aspect = (float)ravengfx_win_w / ravengfx_win_h;
    if (fabsf(proj_aspect - 16.0f / 9.0f) < 0.01f && proj_fovk == 1.0f) proj_on = 0;
    const char *pl = raven_env("RIO2PAD_PLAYERS");
    proj_wide4 = pl && atoi(pl) >= 3 ? (raven_env("RAVEN_WIDE4FIT") ? 2 : 1) : 0;
    if (proj_wide4 && fabsf(proj_aspect - 32.0f / 9.0f) > 0.05f) proj_on = 1;
    const char *w4 = raven_env("RAVEN_WIDE4");
    int hy = proj_wide4 && w4 && !strcmp(w4, "hybrid") && proj_aspect < 3.3f;
    if (hy && !proj_hybrid) raven_view = 1;   // start in the menus
    if (!hy) raven_view = 0;
    proj_hybrid = hy;
}
// RAVEN_PROCLOG=1: log the game's running proc names whenever the set changes (debug).
void *dbg_cams[8]; int dbg_ncam;
static void proc_log(void) {
    static int on = -1; static char last[4096];
    if (on < 0) on = raven_env("RAVEN_PROCLOG") != NULL;
    if (!on) return;
    char cur[4096]; int o = 0;
    for (int i = 0; i < 64 && o < 4000; i++) {
        const char *s = (const char *)(0x1dd78c0ULL + i * 0x448ULL);
        if (s[0x28] & 1) o += snprintf(cur + o, sizeof cur - o, " %.40s", s);
    }
    o += snprintf(cur + o, sizeof cur - o, " | ps");
    for (int i = 0; i < 4; i++) o += snprintf(cur + o, sizeof cur - o, " %02x", *(volatile unsigned char *)(0x1c0c7c0ULL + i * 0x250ULL));
    o += snprintf(cur + o, sizeof cur - o, " | cams");
    for (int i = 0; i < dbg_ncam; i++) {
        float *c = (float *)((char *)dbg_cams[i] + 0x170);
        o += snprintf(cur + o, sizeof cur - o, " %p:%.2f", dbg_cams[i], c[0] > 0 ? c[5] / c[0] : 0.0f);
    }
    dbg_ncam = 0;
    static unsigned long fr; fr++;
    if (strcmp(cur, last)) { strcpy(last, cur); raven_log("[procs] f%lu%s\n", fr, cur); }
}

// Script conditions (0x4ab530 = set(ctx, name), 0x4ab8a0 = the other half of the pair). The
// hybrid view watches them to know which menu screen is up; RAVEN_CONDLOG=1 logs every name.
typedef SV long (*cond_fn)(int, const char *);
static cond_fn cond_a_orig, cond_b_orig;
static unsigned long cond_frame;
static void cond_seen(char k, int ctx, const char *n) {
    static int on = -1;
    if (on < 0) on = raven_env("RAVEN_CONDLOG") != NULL;
    if (on && n) {
        static char last[64]; static char lk;
        if (lk != k || strncmp(last, n, 63)) {
            snprintf(last, sizeof last, "%s", n); lk = k;
            raven_log("[cond] f%lu %c %d %.60s\n", cond_frame, k, ctx, n);
        }
    }
}
static volatile int g_missionsel;   // mission select screen up (wide menu view)
static volatile int g_rackup;       // end-of-mission score screen (rackupN): one 32:9 layout, 4 stations
static SV long cond_a(int ctx, const char *n) {
    cond_seen('A', ctx, n);
    if (n) {
        if (!strncmp(n, "SCENE_START_rackup", 18)) g_rackup = 1;
        else if (!strcmp(n, "RACKUP_COMPLETE") || !strncmp(n, "SCENE_START_game_select", 23) || !strcmp(n, "MIN_LOAD_DONE")) g_rackup = 0;
        if (!strcmp(n, "GAMESEL_CHAPTER")) g_missionsel = 1;
        else if (!strcmp(n, "GAMESEL_DONE") || !strcmp(n, "BACK") || !strcmp(n, "SCENE_START_game_select2") || !strcmp(n, "MIN_LOAD_DONE")) g_missionsel = 0;
    }
    return cond_a_orig(ctx, n);
}
static SV long cond_b(int ctx, const char *n) { cond_seen('B', ctx, n); return cond_b_orig(ctx, n); }
static void hook_cond(void) {
    static const uint8_t pa[] = {0x41,0x54,0x55,0x53,0x48,0x83,0xec,0x10,0x65,0x48,0x8b,0x04,0x25,0x28,0x00,0x00,0x00};
    static const uint8_t pb[] = {0x55,0x53,0x48,0x83,0xec,0x18,0x65,0x48,0x8b,0x04,0x25,0x28,0x00,0x00,0x00};
    /* the loader rewrites the TLS canary load (fs -> gs, maybe more): match the first 8 bytes,
       then copy the 17/15 live prologue bytes as they are now (same instruction lengths) */
    if (!memcmp((void *)0x4ab530, pa, 8)) cond_a_orig = hook(0x4ab530, (const uint8_t *)0x4ab530, sizeof pa, (void *)cond_a);
    if (!memcmp((void *)0x4ab8a0, pb, 6)) cond_b_orig = hook(0x4ab8a0, (const uint8_t *)0x4ab8a0, sizeof pb, (void *)cond_b);
    const uint8_t *qa = cond_a_orig ? (const uint8_t *)cond_a_orig : (const uint8_t *)0x4ab530;
    raven_log("[ravengfx] condition hooks: %s %s (bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x)\n",
              cond_a_orig ? "ok" : "no", cond_b_orig ? "ok" : "no",
              qa[8], qa[9], qa[10], qa[11], qa[12], qa[13], qa[14], qa[15], qa[16], qa[17]);
}
static void hybrid_frame(void) {
    static int run, want_last;
    extern volatile int raven_movie_open;
    proc_log();
    cond_frame++;
    if (!proj_hybrid) { raven_shadow_seen = 0; return; }
    // The score screen draws a shadow map (the saluting soldier), so check it before the mission test.
    int want = raven_movie_open ? 3 : g_rackup ? 4 : raven_shadow_seen ? 2 : g_missionsel ? 4 : 1;
    if (want == 2) g_missionsel = 0;
    raven_shadow_seen = 0;
    if (want == raven_view) { run = 0; return; }
    if (want != want_last) run = 0;
    want_last = want;
    if (want != 3 && ++run < 8) return;   // a movie switches at once, the rest after 8 frames
    run = 0;
    raven_view = want;
    raven_log("[ravengfx] hybrid view: %s\n", want == 1 ? "menu (left half)" : want == 2 ? "mission (full, HUD band)" : want == 4 ? "mission select (32:9)" : "movie (full)");
}

// ---- per frame, from s_glXSwapBuffers before the picture is scaled and swapped ----
void raven_set_swap_interval(int n);  // shim_glx.c
static double now_s(void) {
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
void ravengfx_frame(void) {
    static long frames, fl;
    static double tl;
    double t = now_s();
    if (!tl) tl = t;
    hybrid_frame();
    if (frames % 30 == 0) {
        proj_update();
        // VSYNC (0 off, 1 on, adaptive = -1). The game sets its own interval; ours wins each time.
        const char *si = raven_env("RAVENGFX_SWAPINTERVAL");
        if (!si) si = lookup("VSYNC");
        if (!si) si = lookup("SWAP_INTERVAL");
        if (si) raven_set_swap_interval(!strcmp(si, "adaptive") ? -1 : atoi(si));
    }
    // FPS_CAP: sleep-based limiter (game logic runs on real time, so capping is safe).
    static double cap_dt, next_t;
    if (frames % 30 == 0) {
        const char *c = raven_env("RAVENGFX_FPSCAP");
        if (!c) c = lookup("FPS_CAP");
        cap_dt = c && atof(c) > 0 ? 1.0 / atof(c) : 0;
    }
    if (cap_dt > 0) {
        if (next_t > t) {
            DWORD ms = (DWORD)((next_t - t) * 1000.0);
            if (ms) Sleep(ms);
            while (now_s() < next_t) YieldProcessor();
        }
        next_t = (next_t > t - cap_dt ? next_t : t) + cap_dt;
    }
    // RAVEN_BLACKCHECK=1: sample an 8x8 grid of the finished game picture each frame without
    // stalling (pixel-pack buffers read back 3 frames later). Logs runs of black frames and
    // "dips" (a frame far darker than both neighbours = flicker, not a fade). Debug only.
    static int bc = -1;
    if (bc < 0) bc = raven_env("RAVEN_BLACKCHECK") != NULL;
    if (bc && ravengfx_win_w > 0) {
        typedef void (APIENTRY *rp_t)(int, int, int, int, unsigned, unsigned, void *);
        typedef void (APIENTRY *bf_t)(unsigned, unsigned);
        typedef void (APIENTRY *gi_t)(unsigned, int *);
        typedef void (APIENTRY *gb_t)(int, unsigned *);
        typedef void (APIENTRY *bd_t)(unsigned, ptrdiff_t, const void *, unsigned);
        typedef void *(APIENTRY *mr_t)(unsigned, ptrdiff_t, ptrdiff_t, unsigned);
        typedef unsigned char (APIENTRY *um_t)(unsigned);
        typedef void *(APIENTRY *fs_t)(unsigned, unsigned);
        typedef unsigned (APIENTRY *cw_t)(void *, unsigned, unsigned long long);
        typedef void (APIENTRY *ds_t)(void *);
        typedef void (APIENTRY *ps_t)(unsigned, int);
        extern void *gl_resolve(const char *name);
        static rp_t rp; static bf_t bf, bb; static gi_t gi; static gb_t gb; static bd_t bd; static mr_t mr;
        static um_t um; static fs_t fs; static cw_t cw; static ds_t ds; static ps_t ps;
        enum { NR = 4 };
        static unsigned pbo[NR]; static void *fence[NR]; static int fsr_at[NR]; static long frame_at[NR];
        if (!rp) {
            rp = (rp_t)gl_resolve("glReadPixels"); bf = (bf_t)gl_resolve("glBindFramebuffer");
            bb = (bf_t)gl_resolve("glBindBuffer"); gi = (gi_t)gl_resolve("glGetIntegerv");
            gb = (gb_t)gl_resolve("glGenBuffers"); bd = (bd_t)gl_resolve("glBufferData");
            mr = (mr_t)gl_resolve("glMapBufferRange"); um = (um_t)gl_resolve("glUnmapBuffer");
            fs = (fs_t)gl_resolve("glFenceSync"); cw = (cw_t)gl_resolve("glClientWaitSync");
            ds = (ds_t)gl_resolve("glDeleteSync"); ps = (ps_t)gl_resolve("glPixelStorei");
            gb(NR, pbo);
            int pp = 0; gi(0x88ED, &pp);
            for (int i = 0; i < NR; i++) { bb(0x88EB, pbo[i]); bd(0x88EB, 64 * 4, NULL, 0x88E9); }   // STREAM_READ
            bb(0x88EB, (unsigned)pp);
        }
        int rfb = 0, pp = 0, pa = 4;
        gi(0x8CAA, &rfb); gi(0x88ED, &pp); gi(0x0D05, &pa);
        int slot = (int)(frames % NR);
        // read back the oldest slot (written NR frames ago) if the GPU is done with it
        if (fence[slot]) {
            unsigned st = cw(fence[slot], 0, 0);
            if (st == 0x911A || st == 0x911C) {   // ALREADY_SIGNALED / CONDITION_SATISFIED
                bb(0x88EB, pbo[slot]);
                unsigned char *p = mr(0x88EB, 0, 64 * 4, 0x0001);   // MAP_READ_BIT
                if (p) {
                    int lit = 0, lum = 0;
                    for (int i = 0; i < 64; i++) {
                        if (p[i * 4] > 8 || p[i * 4 + 1] > 8 || p[i * 4 + 2] > 8) lit++;
                        lum += p[i * 4] + p[i * 4 + 1] + p[i * 4 + 2];
                    }
                    um(0x88EB);
                    long fr = frame_at[slot];
                    static int l1 = -1, l2 = -1, ndip, fsr1; static long f1;
                    if (l2 > 3000 && l1 >= 0 && lum > 3000 && l1 * 3 < l2 && l1 * 3 < lum && ndip++ < 300)
                        raven_log("[blackcheck] dip at frame %ld: brightness %d between %d and %d, FSR ran %d\n",
                                  f1, l1, l2, lum, fsr1);
                    l2 = l1; l1 = lum; f1 = fr; fsr1 = fsr_at[slot];
                    static long run_start = -1; static int run_fsr, nlog;
                    if (!lit) { if (run_start < 0) { run_start = fr; run_fsr = 0; } run_fsr += fsr_at[slot]; }
                    else if (run_start >= 0) {
                        if (nlog++ < 300)
                            raven_log("[blackcheck] %ld black frame(s) at frames %ld-%ld, FSR ran in %d of them\n",
                                      fr - run_start, run_start, fr - 1, run_fsr);
                        run_start = -1;
                    }
                }
            } else {
                static int nbusy;
                if (nbusy++ < 5) raven_log("[blackcheck] readback slot not ready, frame skipped\n");
            }
            ds(fence[slot]); fence[slot] = NULL;
        }
        // queue this frame's samples
        extern int ravenfsr_dispatched(void);
        bf(0x8CA8, 0);
        bb(0x88EB, pbo[slot]);
        ps(0x0D05, 1);
        for (int i = 0; i < 64; i++)
            rp(ravengfx_win_w * (2 * (i % 8) + 1) / 16, ravengfx_win_h * (2 * (i / 8) + 1) / 16, 1, 1,
               0x1908, 0x1401, (void *)(uintptr_t)(i * 4));
        fence[slot] = fs(0x9117, 0);   // SYNC_GPU_COMMANDS_COMPLETE
        fsr_at[slot] = ravenfsr_dispatched(); frame_at[slot] = frames;
        ps(0x0D05, pa);
        bb(0x88EB, (unsigned)pp);
        bf(0x8CA8, (unsigned)rfb);
    }
    extern void streamenc_frame(int sw, int sh);   // browser stream: game picture only, before the overlay
    streamenc_frame(ravengfx_win_w, ravengfx_win_h);
    extern void ravengfx_overlay_draw(int sw, int sh);  // ravenui_win.c (F1 overlay, FPS counter)
#ifdef RAVEN_REC
    extern void ravenrec_frame(int sw, int sh);   // local add-on (local.mk)
    ravenrec_frame(ravengfx_win_w, ravengfx_win_h);
#endif
    ravengfx_overlay_draw(ravengfx_win_w, ravengfx_win_h);
    extern void ravengl_frame_end(void);
    ravengl_frame_end();  // FSR 2: new frame (dispatch again, fresh jitter)
    frames++;
    if (t - tl >= 1) { ravengfx_fps = (float)((frames - fl) / (t - tl)); tl = t; fl = frames; }
}

// For the coop host's T_SETUP answer: the settings that shape the camera.
void ravengfx_coop_camera(float *fovk, int *horplus)
{
    const char *hp = lookup("HOR_PLUS"), *fv = lookup("FOV_SCALE");
    *horplus = hp ? atoi(hp) != 0 : 1;
    *fovk = fv && atof(fv) > 0.3 ? (float)atof(fv) : 1.0f;
}

// ---- launcher part: resolution -> game args ----
static char g_res_arg[32];
int ravengfx_args(char **argv, int argc) {
    const char *r = raven_env("RAVEN_RES");
    if (!r) r = lookup("resolution");
    int w = 0, h = 0;
    if (!r || !strcmp(r, "native") || sscanf(r, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
        if (r && strcmp(r, "native")) w = 1920, h = 1080;  // play_halo.sh default for a bad value
        else { w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); }
    }
    if (raven_env("RAVEN_WINDOWED") && !raven_env("RAVEN_RES")) w = 1280, h = 720;
    // 4-player cabinet (RIO2PAD_PLAYERS=4): the game draws two cabinet screens side by side, so it needs
    // a 32:9 picture. Keep the width that fits the screen (fullscreen) or double it (window).
    // Default ON for the 4-player cabinet (RAVEN_WIDE4=0 opts out): in a 16:9 window the per-half
    // content (cutscene cameras, wide menu layouts, movies) is squeezed to half width - skinny people.
    // Same players4 condition as rio2pad_win.c, so coop 4P gets it too.
    const char *pl = raven_env("RIO2PAD_PLAYERS");
    const char *cp = raven_env("RAVEN_COOP");
    const char *cpp = raven_env("RAVEN_COOP_PLAYERS");
    int players4 = (pl && atoi(pl) >= 3) || (cp && !strcmp(cp, "host") && cpp && atoi(cpp) >= 3);
    const char *w4 = raven_env("RAVEN_WIDE4");
    if (players4 && w4 && !strcmp(w4, "hybrid"))
        raven_log("[ravengfx] 4-player cabinet, hybrid: %dx%d, menus/cutscenes shown 32:9, missions full screen\n", w, h);
    else if (players4 && !raven_env("RAVEN_RES") && w * 9 < h * 32 && (!w4 || strcmp(w4, "0") != 0)) {
        if (raven_env("RAVEN_WINDOWED")) w *= 2;
        h = w * 9 / 32;
        raven_log("[ravengfx] 4-player cabinet: two screens side by side, %dx%d%s\n", w, h,
                  w4 ? "" : " (32:9 default; RAVEN_WIDE4=0 for the old squeezed 16:9 window)");
    }
    // Online coop joiner: the game camera (and so every aim ray) follows the picture's shape, and
    // PCs with different shapes desync. Use the host's shape (coopnet_preconnect sets this), as large
    // as fits this PC's picture; the rest of the screen is black bars.
    const char *ca = raven_env("RAVEN_COOP_ASPECT");
    int aw, ah;
    if (ca && sscanf(ca, "%d:%d", &aw, &ah) == 2 && aw > 0 && ah > 0 && (long long)w * ah != (long long)h * aw) {
        // exactly the host's shape (a rounded 1066x600 for 16:9 was enough to desync): whole
        // multiples of the reduced ratio, as large as fits
        int a = aw, b = ah; while (b) { int t = a % b; a = b; b = t; }
        int rw = aw / a, rh = ah / a, k = w / rw < h / rh ? w / rw : h / rh;
        if (k > 0) { w = rw * k; h = rh * k; }
        raven_log("[ravengfx] coop: picture %dx%d, the host's shape %d:%d (black bars around it)\n", w, h, aw, ah);
    }
    ravengfx_res_w = w; ravengfx_res_h = h;
    snprintf(g_res_arg, sizeof g_res_arg, "-f%dx%d", w, h);
    argv[argc++] = g_res_arg;
    argv[argc++] = "-n";
    raven_log("[ravengfx] game args: %s -n (resolution %s)\n", g_res_arg, r ? r : "unset");
    return argc;
}

void ravengfx_install(void) {
    InitializeCriticalSection(&loglock);
    const char *conf = raven_env("RAVENGFX_CONF");
    if (conf && *conf) snprintf(ravengfx_conf_path, sizeof ravengfx_conf_path, "%s", conf);
    else snprintf(ravengfx_conf_path, sizeof ravengfx_conf_path, "%s/openraven/graphics.ini", g_root);
    load_conf(ravengfx_conf_path);
    extern void display_detect(const char *);
    display_detect(ravengfx_conf_path);  // first launch only (autodetect = 1): HDR on/off + peak nits
    extern void ravenfsr_config(void);
    ravenfsr_config();  // UPSCALER = fsr2: sets RENDER_SCALE from FSR_QUALITY, forces TAA on, SMAA/FXAA off
    const char *kl = raven_env("RAVENGFX_KEYLOG");
    if (kl && *kl) keylog = fopen(kl, "w");
    const char *rr = raven_env("RAVENGFX_RENDER");
    if (rr) sscanf(rr, "%dx%d", &render_w, &render_h);

    orig_int = (get_int_fn)hook(CFG_GET_INT, int_prologue, sizeof int_prologue, (void *)hook_int);
    orig_flt = (get_flt_fn)hook(CFG_GET_FLOAT, flt_prologue, sizeof flt_prologue, (void *)hook_flt);
    raven_log("[ravengfx] config hooks %s\n", orig_int && orig_flt ? "installed" : "NOT installed (unexpected bytes)");

    if (!memcmp((void *)VIDEO_SET_MODE, "\x48\x85\xff\x0f\x84\x84\x00\x00\x00\x8b\x07\x8b\x57\x04", 14)) {
        uint8_t *t = exec_page();
        if (t) {
            memcpy(t, (void *)VIDEO_SET_BODY, 5);       // mov (%rdi),%eax; mov 4(%rdi),%edx
            write_abs_jmp(t + 5, VIDEO_SET_BODY + 5);   // back to 0x4fb73e
            set_mode_body = (set_mode_fn)(void *)t;
            make_rwx(VIDEO_SET_MODE, 16);
            write_abs_jmp((uint8_t *)VIDEO_SET_MODE, (uint64_t)hook_set_mode);
            raven_log("[ravengfx] video mode hook installed\n");
        }
    } else raven_log("[ravengfx] video mode hook NOT installed (unexpected bytes)\n");

    hook_proj();
    hook_cond();

    // Launcher/input keys from graphics.ini, like graphics.sh (env vars win). rio2pad reads them.
    static const char *map[][2] = {{"skip_boot", "RIO2PAD_SKIPBOOT"}, {"free_play", "RIO2PAD_FREEPLAY"},
                                   {"stick_speed", "RIO2PAD_STICKSPEED"}, {"aim", "RIO2PAD_AIM"},
                                   {"mouse", "RIO2PAD_MOUSE"}, {"mouse_sens", "RIO2PAD_MOUSESENS"},
                                   {"players", "RIO2PAD_PLAYERS"}, {"wide4", "RAVEN_WIDE4"}};
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++) {
        const char *v = lookup(map[i][0]);
        if (v && !raven_env(map[i][1])) SetEnvironmentVariableA(map[i][1], v);
    }
    // Default cabinet: the 4-player cabinet in the hybrid picture (F1 Cabinet: Players 2 switches back).
    if (!raven_env("RIO2PAD_PLAYERS")) SetEnvironmentVariableA("RIO2PAD_PLAYERS", "4");
    if (!raven_env("RAVEN_WIDE4")) SetEnvironmentVariableA("RAVEN_WIDE4", "hybrid");
    // Coop tab (F1): coop = off|host|join, coop_address = host to join. Env vars (coop_*.bat) win.
    // Browser stream (F1 Coop tab): works without the coop Mode, so mapped on its own.
    static const char *smap[][2] = {{"stream", "RAVEN_STREAM"}, {"stream_port", "RAVEN_STREAM_PORT"}, {"stream_kbps", "RAVEN_STREAM_KBPS"}, {"stream_encoder", "RAVEN_STREAM_ENCODER"}};
    for (unsigned i = 0; i < sizeof smap / sizeof *smap; i++) { const char *x = lookup(smap[i][0]); if (x && !raven_env(smap[i][1])) SetEnvironmentVariableA(smap[i][1], x); }
    static const char *cmap[][2] = {{"coop_players", "RAVEN_COOP_PLAYERS"}, {"coop_port", "RAVEN_COOP_PORT"},
                                    {"coop_delay", "RAVEN_COOP_DELAY"}, {"coop_upnp", "RAVEN_COOP_UPNP"}};
    const char *cm = lookup("coop");
    if (!raven_env("RAVEN_COOP") && cm && (!strcmp(cm, "host") || !strcmp(cm, "join"))) {
        const char *ad = lookup("coop_address");
        char v[300];
        if (!strcmp(cm, "host")) snprintf(v, sizeof v, "host");
        else snprintf(v, sizeof v, "join:%s", ad ? ad : "");
        if (strcmp(v, "join:")) SetEnvironmentVariableA("RAVEN_COOP", v);
        else raven_log("[coop] Coop tab says join, but no host address is set: coop off\n");
        for (unsigned i = 0; i < sizeof cmap / sizeof *cmap; i++) {
            const char *x = lookup(cmap[i][0]);
            if (x && !raven_env(cmap[i][1])) SetEnvironmentVariableA(cmap[i][1], x);
        }
    }
    extern void ravenui_start(void);
    ravenui_start();

    uint8_t *ja = (uint8_t *)ASPECT_LOCK_JA;
    if (ja[0] == 0x77 && ja[1] == 0x9f) { make_rwx(ASPECT_LOCK_JA, 2); ja[0] = 0x90; ja[1] = 0x90; }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
}
