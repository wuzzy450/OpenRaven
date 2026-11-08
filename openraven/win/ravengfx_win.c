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
static int proj_on;
SV void proj_fix(uint8_t *cam) {
    float *p00 = (float *)(cam + 0x170), *p11 = (float *)(cam + 0x184);
    if (!proj_on || *p00 <= 0 || *p11 <= 0) return;
    float shape = *p11 / *p00;
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
    proj_on = hp ? atoi(hp) != 0 : 1;
    proj_fovk = fv && atof(fv) > 0.3 ? (float)atof(fv) : 1.0f;
    if (ravengfx_win_h > 0) proj_aspect = (float)ravengfx_win_w / ravengfx_win_h;
    if (fabsf(proj_aspect - 16.0f / 9.0f) < 0.01f && proj_fovk == 1.0f) proj_on = 0;
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
    extern void ravengfx_overlay_draw(int sw, int sh);  // ravenui_win.c (F1 overlay, FPS counter)
    extern void ravenrec_frame(int sw, int sh);  // ravenrec_win.c: game-only frames for the recorder
    ravenrec_frame(ravengfx_win_w, ravengfx_win_h);
    ravengfx_overlay_draw(ravengfx_win_w, ravengfx_win_h);
    extern void ravengl_frame_end(void);
    ravengl_frame_end();  // FSR 2: new frame (dispatch again, fresh jitter)
    frames++;
    if (t - tl >= 1) { ravengfx_fps = (float)((frames - fl) / (t - tl)); tl = t; fl = frames; }
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

    // Launcher/input keys from graphics.ini, like graphics.sh (env vars win). rio2pad reads them.
    static const char *map[][2] = {{"skip_boot", "RIO2PAD_SKIPBOOT"}, {"free_play", "RIO2PAD_FREEPLAY"},
                                   {"stick_speed", "RIO2PAD_STICKSPEED"}, {"aim", "RIO2PAD_AIM"}};
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++) {
        const char *v = lookup(map[i][0]);
        if (v && !raven_env(map[i][1])) SetEnvironmentVariableA(map[i][1], v);
    }
    extern void ravenui_start(void);
    ravenui_start();

    uint8_t *ja = (uint8_t *)ASPECT_LOCK_JA;
    if (ja[0] == 0x77 && ja[1] == 0x9f) { make_rwx(ASPECT_LOCK_JA, 2); ja[0] = 0x90; ja[1] = 0x90; }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
}
