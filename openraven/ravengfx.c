// ravengfx.so - graphics settings shim for Halo: Fireteam Raven (v3.11).
// Preloaded next to rio2pad.so. Kept separate so input and graphics work don't collide.
//
// The engine reads its render options (MSAA, SMAA, FXAA, ANISOTROPY_LEVEL, shadows, ...) from the
// encrypted data/prod/conf/gameconf.txt through two getters:
//   int   cfg_get_int  (cfg *, const char *key, int   def)  @0x924650
//   float cfg_get_float(cfg *, const char *key, float def)  @0x9247a0
// We inline-hook both and return values from a plain text file (RAVENGFX_CONF, KEY=VALUE lines).
// RAVENGFX_KEYLOG=/abs/file writes every key the game asks for with the value it got.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
#include <math.h>

#define CFG_GET_INT   0x924650UL
#define CFG_GET_FLOAT 0x9247a0UL
#define ASPECT_LOCK_JA 0x417e62UL
#define VIDEO_SET_MODE 0x4fb730UL   // int video_set_mode(struct mode *): +4 w, +8 h, +0xc aspect, +0x18/+0x1c alt size
#define VIDEO_SET_BODY 0x4fb739UL   // just after its `test rdi; je` null check

static const uint8_t int_prologue[14] = {0x41,0x54,0x55,0x41,0x89,0xd4,0x53,0x48,0x83,0xec,0x10,0x48,0x85,0xff};
static const uint8_t flt_prologue[15] = {0x55,0x53,0x48,0x83,0xec,0x28,0x48,0x85,0xff,0xf3,0x0f,0x11,0x44,0x24,0x0c};

typedef int   (*get_int_fn)(void *, const char *, int);
typedef float (*get_flt_fn)(void *, const char *, float);
static get_int_fn orig_int;
static get_flt_fn orig_flt;

#define MAXKV 256
static struct { char key[64]; char val[64]; } kv[MAXKV];
static int nkv;
static FILE *keylog;
static pthread_mutex_t loglock = PTHREAD_MUTEX_INITIALIZER;

static const char *lookup(const char *key)
{
    if (!key) return NULL;
    for (int i = 0; i < nkv; i++)
        if (!strcmp(kv[i].key, key)) return kv[i].val;
    return NULL;
}

// Used by the overlay (ravenui.c).
const char *ravengfx_conf_path;
float ravengfx_fps;
const char *ravengfx_get(const char *key) { return lookup(key); }
void ravengfx_set(const char *key, const char *val)
{
    for (int i = 0; i < nkv; i++)
        if (!strcmp(kv[i].key, key)) { snprintf(kv[i].val, sizeof kv[0].val, "%s", val); return; }
    if (nkv >= MAXKV) return;
    snprintf(kv[nkv].key, sizeof kv[0].key, "%s", key);
    snprintf(kv[nkv].val, sizeof kv[0].val, "%s", val);
    __atomic_store_n(&nkv, nkv + 1, __ATOMIC_RELEASE);   // entry is complete before getters see it
}
void ravenui_start(void);

static void logkey(const char *t, const char *key, const char *got, int overridden)
{
    if (!keylog || !key) return;
    pthread_mutex_lock(&loglock);
    fprintf(keylog, "%s %s = %s%s\n", t, key, got, overridden ? "   (override)" : "");
    fflush(keylog);
    pthread_mutex_unlock(&loglock);
}

static int hook_int(void *cfg, const char *key, int def)
{
    const char *o = lookup(key);
    int v = o ? (int)strtol(o, NULL, 0) : orig_int(cfg, key, def);
    char b[32]; snprintf(b, sizeof b, "%d (def %d)", v, def);
    logkey("int", key, b, o != NULL);
    return v;
}

static float hook_flt(void *cfg, const char *key, float def)
{
    const char *o = lookup(key);
    float v = o ? strtof(o, NULL) : orig_flt(cfg, key, def);
    char b[48]; snprintf(b, sizeof b, "%g (def %g)", v, def);
    logkey("flt", key, b, o != NULL);
    return v;
}

static void write_abs_jmp(uint8_t *p, uint64_t to)
{
    p[0] = 0xff; p[1] = 0x25; memset(p + 2, 0, 4);  // jmp *0(%rip)
    memcpy(p + 6, &to, 8);
}

// Copy the (position-independent) prologue into a trampoline, then jump back after it.
static void *hook(uint64_t at, const uint8_t *pro, size_t n, void *to)
{
    if (memcmp((void *)at, pro, n)) return NULL;
    uint8_t *t = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) return NULL;
    memcpy(t, pro, n);
    write_abs_jmp(t + n, at + n);
    mprotect((void *)(at & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    write_abs_jmp((uint8_t *)at, (uint64_t)to);
    return t;
}

static void load_conf(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[ravengfx] no config at %s, using game defaults\n", path); return; }
    char line[256];
    while (fgets(line, sizeof line, f) && nkv < MAXKV) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == ';' || *s == '\n' || !*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = s, *v = eq + 1, *e;
        for (e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (e = v + strlen(v) - 1; e >= v && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t' || *e == '#'); e--) *e = 0;
        if ((e = strchr(v, '#'))) { *e = 0; for (e--; e >= v && (*e == ' ' || *e == '\t'); e--) *e = 0; }
        if (!*k || !*v) continue;
        snprintf(kv[nkv].key, sizeof kv[0].key, "%s", k);
        snprintf(kv[nkv].val, sizeof kv[0].val, "%s", v);
        nkv++;
    }
    fclose(f);
    fprintf(stderr, "[ravengfx] %d setting override(s) from %s\n", nkv, path);
}

static int render_w, render_h;
int ravengfx_win_w, ravengfx_win_h;
int ravengfx_res_w, ravengfx_res_h;   // the game's screen size (same as win_w/h here; the Windows name)
// Our 14-byte jmp at VIDEO_SET_MODE also overwrites the first 5 body bytes (0x4fb739..0x4fb73d:
// mov (%rdi),%eax; mov 4(%rdi),%edx). Calling VIDEO_SET_BODY directly jumped into the middle of the
// jmp and crashed every boot, so run those two instructions from a copy and jump back to 0x4fb73e.
static void *set_mode_body;
static int hook_set_mode(uint8_t *m)
{
    if (!m) return -1;   // the original's null check, which the jmp replaced
    int *w = (int *)(m + 4), *h = (int *)(m + 8);
    fprintf(stderr, "[ravengfx] video mode %dx%d aspect %.4f alt %dx%d\n", *w, *h, *(float *)(m + 0xc),
            *(int *)(m + 0x18), *(int *)(m + 0x1c));
    ravengfx_win_w = ravengfx_res_w = *w; ravengfx_win_h = ravengfx_res_h = *h;
    // graphics.ini: RENDER_SCALE=1.25 (supersample relative to screen) or RENDER_SIZE=WxH; env wins.
    const char *rs = lookup("RENDER_SCALE"), *rz = lookup("RENDER_SIZE");
    if (render_w <= 0 && rz) sscanf(rz, "%dx%d", &render_w, &render_h);
    if (render_w <= 0 && rs && strtof(rs, NULL) > 0 && strtof(rs, NULL) != 1.0f) {
        float k = strtof(rs, NULL);
        render_w = (int)(*w * k + 0.5f) & ~1; render_h = (int)(*h * k + 0.5f) & ~1;
    }
    if (render_w > 0) fprintf(stderr, "[ravengfx] render size %dx%d\n", render_w, render_h);
    if (render_w > 0 && render_h > 0) { *(int *)(m + 0x18) = render_w; *(int *)(m + 0x1c) = render_h; }
    return ((int (*)(void *))set_mode_body)(m);
}


// ---- frame hook: glXSwapBuffers (FPS log, frame capture, overlay) ----
// Capture works with the window hidden/behind others: we read the GL back buffer, not the screen.
// RAVENGFX_SHOT="secs,secs,..." (seconds since first frame) + RAVENGFX_SHOTDIR=/abs/dir -> shot_<s>.ppm
// RAVENGFX_FPSLOG=1 prints FPS every 5 s.
typedef void (*swap_fn)(void *, unsigned long);
static swap_fn real_swap;
static volatile int swap_seen;
static void (*p_glReadPixels)(int,int,int,int,unsigned,unsigned,void *);
static void (*p_glReadBuffer)(unsigned);
static void (*p_glGetIntegerv)(unsigned, int *);
static void (*p_glPixelStorei)(unsigned, int);
static void (*p_glBindFramebuffer)(unsigned, unsigned);
static double shot_t[160]; static int nshot, shot_i;
static const char *shot_dir;
static int fpslog;
static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static void *glsym(const char *n){
    void *f = dlsym(RTLD_DEFAULT, n);
    if (!f) { void *(*gpa)(const char*) = dlsym(RTLD_DEFAULT, "glXGetProcAddressARB"); if (gpa) f = gpa(n); }
    return f;
}
void ravengfx_overlay_draw(int w, int h) __attribute__((weak));
void ravengl_trace_frame(double t);
static void capture(const char *path);
void ravengfx_capture(const char *path) { capture(path); }
float ravengfx_fg_fps;
void ravenfg_request_shot(const char *base); void ravenfg_present(void (*real)(void *), void *ctx);
static void capture(const char *path){
    int vp[4]; p_glGetIntegerv(0x0BA2, vp);   // GL_VIEWPORT is unreliable; use drawable size below
    int fbo=0; p_glGetIntegerv(0x8CA6, &fbo);  // GL_DRAW_FRAMEBUFFER_BINDING
    int rfbo=0; p_glGetIntegerv(0x8CAA, &rfbo); int rbuf=0x0405; p_glGetIntegerv(0x0C02, &rbuf);
    unsigned ravengl_screen_fbo(void); unsigned sfb = ravengl_screen_fbo();   // native HDR: the game's screen
    if (p_glBindFramebuffer) p_glBindFramebuffer(0x8CA8, sfb);  // read the window (or its HDR stand-in)
    int w = vp[2], h = vp[3];
    if (ravengfx_win_w > 0) { w = ravengfx_win_w; h = ravengfx_win_h; }
    unsigned char *buf = malloc((size_t)w*h*3);
    if (!buf) return;
    p_glReadBuffer(sfb ? 0x8CE0 : 0x0405); p_glPixelStorei(0x0D05, 1);   // GL_BACK, GL_PACK_ALIGNMENT
    p_glReadPixels(0,0,w,h,0x1907,0x1401,buf);             // GL_RGB, GL_UNSIGNED_BYTE
    (void)fbo; if (p_glBindFramebuffer) p_glBindFramebuffer(0x8CA8, rfbo);
    p_glReadBuffer(rbuf);
    FILE *f = fopen(path,"wb");
    if (f) { fprintf(f,"P6\n%d %d\n255\n",w,h); for (int y=h-1;y>=0;y--) fwrite(buf+(size_t)y*w*3,1,(size_t)w*3,f); fclose(f); }
    free(buf);
    fprintf(stderr,"[ravengfx] captured %s (%dx%d)\n", path, w, h);
}


// ---- Hor+ / FOV: fix the projection right after the engine builds it ----
// The camera projection is built at 0x4932f8..0x49346e from a fixed 16:9 shape and stored at
// cam+0x170 (P[0][0]) and cam+0x184 (P[1][1]); then `jmp 0x49287c`. We send that jmp through a stub.
// Wider than 16:9: P00 = P11/aspect (Hor+). Narrower: P11 = P00*aspect (keep horizontal view).
// FOV_SCALE > 1 widens both. Only matrices with a ~16:9 shape (the 3D camera) are touched.
#define PROJ_JMP 0x49346eUL
#define PROJ_CONT 0x49287cUL
__attribute__((used, visibility("hidden"))) uint64_t proj_ret = PROJ_CONT;
static float proj_aspect, proj_fovk = 1.0f; static int proj_on, proj_wide4, proj_hybrid;
// Hybrid 4-player mode (RAVEN_WIDE4=hybrid, as win/ravengfx_win.c): the game runs at the screen's own
// shape (16:9), where everything authored for the cabinet's 32:9 screen comes out squeezed. A view is
// picked per frame (raven_view, read by ravengltrace.c and rio2pad.c):
//  1 menu    - camera vert+; the menu's 2D layer drawn twice as wide (left cabinet screen fills the screen)
//  2 mission - camera vert+, each 2D item placed (HUD bars at the edges, crosshairs follow the aim)
//  3 movie   - the 16:9 cutscene files are played instead (fopen below)
//  4 mission select - the 2D layer in the middle 32:9 band
int raven_view = 0;
volatile int raven_shadow_seen;   // ravengltrace.c: the 8192x8192 shadow pass ran (= mission)
volatile int raven_movie_open;    // a .webm cutscene file is open
__attribute__((used, visibility("hidden"))) void proj_fix(uint8_t *cam);
void proj_fix(uint8_t *cam)
{
    float *p00 = (float *)(cam + 0x170), *p11 = (float *)(cam + 0x184);
    if (!proj_on || *p00 <= 0 || *p11 <= 0) return;
    float shape = *p11 / *p00;
    if (proj_hybrid && raven_view == 3) return;              // movies: cameras stay native
    // 4-player cabinet: the camera is 32:9 (two screens). Keep the full width of view and show more
    // above and below (RAVEN_WIDE4FIT: keep the height, cut the sides). The 16:9 cameras stay native.
    if (proj_wide4) {
        if (shape > 3.4f && shape < 3.7f && proj_aspect > 0) {
            if (proj_wide4 == 2) *p00 = *p11 / proj_aspect; else *p11 = *p00 * proj_aspect;
            *p00 /= proj_fovk; *p11 /= proj_fovk;
        }
        return;
    }
    if (shape < 1.70f || shape > 1.85f) return;            // not the 16:9 camera (already fixed or other)
    if (proj_aspect > 16.0f / 9.0f) *p00 = *p11 / proj_aspect;
    else if (proj_aspect > 0) *p11 = *p00 * proj_aspect;
    *p00 /= proj_fovk; *p11 /= proj_fovk;
}
__attribute__((naked)) static void proj_stub(void)
{
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
static void hook_proj(void)
{
    static const uint64_t sites[] = {0x49346eUL, 0x4932d1UL};   // gameplay camera path, second (cutscene) path
    uint8_t *t = MAP_FAILED;
    for (uint64_t at = 0x11000000UL; at < 0x70000000UL && t == MAP_FAILED; at += 0x1000000UL)
        t = mmap((void *)at, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (t == MAP_FAILED) return;
    write_abs_jmp(t, (uint64_t)proj_stub);
    int n = 0;
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *c = (uint8_t *)sites[i]; int32_t rel; memcpy(&rel, c + 1, 4);
        if (c[0] != 0xe9 || (uint64_t)(c + 5) + rel != PROJ_CONT) continue;
        rel = (int32_t)((int64_t)t - (int64_t)(c + 5));
        mprotect((void *)((uintptr_t)c & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
        memcpy(c + 1, &rel, 4); n++;
    }
    fprintf(stderr, "[ravengfx] projection hook at %d/2 sites\n", n);
}
// called once per frame: pick up overlay changes
static void proj_update(void)
{
    const char *hp = lookup("HOR_PLUS"), *fv = lookup("FOV_SCALE");
    // Online coop: the host's camera settings (coopnet_preconnect), since they change every aim ray.
    if (getenv("RAVEN_COOP_HORPLUS")) hp = getenv("RAVEN_COOP_HORPLUS");
    if (getenv("RAVEN_COOP_FOV")) fv = getenv("RAVEN_COOP_FOV");
    proj_on = hp ? atoi(hp) != 0 : 1;                      // default on
    proj_fovk = fv && atof(fv) > 0.3 ? atof(fv) : 1.0f;
    if (ravengfx_win_h > 0) proj_aspect = (float)ravengfx_win_w / ravengfx_win_h;
    if (fabsf(proj_aspect - 16.0f / 9.0f) < 0.01f && proj_fovk == 1.0f) proj_on = 0;
    const char *pl = getenv("RIO2PAD_PLAYERS");
    proj_wide4 = pl && atoi(pl) >= 3 ? (getenv("RAVEN_WIDE4FIT") ? 2 : 1) : 0;
    if (proj_wide4 && fabsf(proj_aspect - 32.0f / 9.0f) > 0.05f) proj_on = 1;
    const char *w4 = getenv("RAVEN_WIDE4");
    int hy = proj_wide4 && w4 && !strcmp(w4, "hybrid") && proj_aspect < 3.3f;
    if (hy && !proj_hybrid) raven_view = 1;   // start in the menus
    if (!hy) raven_view = 0;
    proj_hybrid = hy;
}

// For the coop host's T_SETUP answer (coopnet.c): the settings that shape the camera.
void ravengfx_coop_camera(float *fovk, int *horplus)
{
    const char *hp = lookup("HOR_PLUS"), *fv = lookup("FOV_SCALE");
    *horplus = hp ? atoi(hp) != 0 : 1;
    *fovk = fv && atof(fv) > 0.3 ? (float)atof(fv) : 1.0f;
}

// Script conditions (0x4ab530 = set(ctx, name), 0x4ab8a0 = the other half). The hybrid view watches
// them to know when the mission select screen is up; RAVEN_CONDLOG=1 logs every name.
typedef long (*cond_fn)(int, const char *);
static cond_fn cond_a_orig, cond_b_orig;
static void cond_seen(char k, int ctx, const char *n)
{
    static int on = -1;
    if (on < 0) on = getenv("RAVEN_CONDLOG") != NULL;
    if (!on || !n) return;
    static char last[64]; static char lk;
    if (lk != k || strncmp(last, n, 63)) { snprintf(last, sizeof last, "%s", n); lk = k; fprintf(stderr, "[cond] %c %d %.60s\n", k, ctx, n); }
}
static volatile int g_missionsel;
static long cond_a(int ctx, const char *n)
{
    cond_seen('A', ctx, n);
    if (n) {
        if (!strcmp(n, "GAMESEL_CHAPTER")) g_missionsel = 1;
        else if (!strcmp(n, "GAMESEL_DONE") || !strcmp(n, "BACK") || !strcmp(n, "SCENE_START_game_select2") || !strcmp(n, "MIN_LOAD_DONE")) g_missionsel = 0;
    }
    return cond_a_orig(ctx, n);
}
static long cond_b(int ctx, const char *n) { cond_seen('B', ctx, n); return cond_b_orig(ctx, n); }
static void *hook(uint64_t at, const uint8_t *pro, size_t n, void *to);
static void hook_cond(void)
{
    static const uint8_t pa[] = {0x41,0x54,0x55,0x53,0x48,0x83,0xec,0x10,0x64,0x48,0x8b,0x04,0x25,0x28,0x00,0x00,0x00};
    static const uint8_t pb[] = {0x55,0x53,0x48,0x83,0xec,0x18,0x64,0x48,0x8b,0x04,0x25,0x28,0x00,0x00,0x00};
    cond_a_orig = hook(0x4ab530, pa, sizeof pa, (void *)cond_a);
    cond_b_orig = hook(0x4ab8a0, pb, sizeof pb, (void *)cond_b);
    fprintf(stderr, "[ravengfx] condition hooks: %s %s\n", cond_a_orig ? "ok" : "no", cond_b_orig ? "ok" : "no");
}
static void hybrid_frame(void)
{
    static int run, want_last;
    if (!proj_hybrid) { raven_shadow_seen = 0; return; }
    int want = raven_movie_open ? 3 : raven_shadow_seen ? 2 : g_missionsel ? 4 : 1;
    if (want == 2) g_missionsel = 0;
    raven_shadow_seen = 0;
    if (want == raven_view) { run = 0; return; }
    if (want != want_last) run = 0;
    want_last = want;
    if (want != 3 && ++run < 8) return;   // a movie switches at once, the rest after 8 frames
    run = 0;
    raven_view = want;
    fprintf(stderr, "[ravengfx] hybrid view: %s\n", want == 1 ? "menu (left half)" : want == 2 ? "mission (full, HUD band)" : want == 4 ? "mission select (32:9)" : "movie (full)");
}

// Hybrid mode: play the 16:9 cutscene movies the 2-player cabinet uses (X_1080.webm; X_wide.webm ->
// X.webm) instead of the 32:9 ones. Also tells the view picker a movie is on screen.
static FILE *movie_f;
static int hybrid16(void) { static int on = -1; if (on < 0) { const char *e = getenv("RAVEN_WIDE4"), *pl = getenv("RIO2PAD_PLAYERS"); on = e && !strcmp(e, "hybrid") && pl && atoi(pl) >= 3; } return on; }
FILE *fopen(const char *p, const char *m)
{
    static FILE *(*real)(const char *, const char *);
    if (!real) real = dlsym(RTLD_NEXT, "fopen");
    char buf[1024];
    size_t n = p ? strlen(p) : 0;
    if (p && hybrid16() && n > 10 && n + 8 < sizeof buf && strstr(p, "/webm/") && !strcmp(p + n - 5, ".webm") && strcmp(p + n - 10, "_1080.webm")) {
        if (!strcmp(p + n - 10, "_wide.webm")) snprintf(buf, sizeof buf, "%.*s.webm", (int)(n - 10), p);
        else snprintf(buf, sizeof buf, "%.*s_1080.webm", (int)(n - 5), p);
        if (!access(buf, R_OK)) p = buf;
    }
    FILE *f = real(p, m);
    if (f && p && strchr(m, 'r') && strstr(p, "/webm/")) { movie_f = f; raven_movie_open = 1; }
    return f;
}
FILE *fopen64(const char *p, const char *m) { return fopen(p, m); }
// The game's protection wrapper resolves libc itself, so the symbols above are not what the game
// calls. Point the game's own import slots (same table the tanf hook scans) at them.
static void hook_game_stdio(void)
{
    static int done;
    if (done) return;
    void *rf = dlsym(RTLD_NEXT, "fopen"), *rf64 = dlsym(RTLD_NEXT, "fopen64"), *rc = dlsym(RTLD_NEXT, "fclose");
    int n = 0;
    for (uint64_t *q = (uint64_t *)0x1690000UL; q < (uint64_t *)0x1692800UL; q++) {
        if (rf && *q == (uint64_t)rf) { *q = (uint64_t)fopen; n++; }
        else if (rf64 && *q == (uint64_t)rf64) { *q = (uint64_t)fopen64; n++; }
        else if (rc && *q == (uint64_t)rc) { *q = (uint64_t)fclose; n++; }
    }
    if (n) { done = 1; fprintf(stderr, "[ravengfx] hybrid 4P: %d stdio slot(s) hooked for the 16:9 cutscenes\n", n); }
}
int fclose(FILE *f)
{
    static int (*real)(FILE *);
    if (!real) real = dlsym(RTLD_NEXT, "fclose");
    if (f && f == movie_f) { movie_f = NULL; raven_movie_open = 0; }
    return real(f);
}

#define HORPLUS_SITE 0x493f0fUL
static void hook_tanf(void);
static float horplus_k = 1.0f;
static int tan_log;
static uintptr_t horplus_site = HORPLUS_SITE;
static void my_swap(void *dpy, unsigned long drawable)
{
    static double t0, tl; static long frames, fl;
    static int inited;
    if (!inited) {
        inited = 1;
        p_glReadPixels = glsym("glReadPixels"); p_glReadBuffer = glsym("glReadBuffer");
        p_glGetIntegerv = glsym("glGetIntegerv"); p_glPixelStorei = glsym("glPixelStorei");
        p_glBindFramebuffer = glsym("glBindFramebuffer");
        t0 = tl = now_s();
        fprintf(stderr, "[ravengfx] first frame through hook\n");
    }
    double t = now_s(); swap_seen = 1;
    static int tan_done;
    if (!tan_done) {
        tan_done = 1;
        tan_log = getenv("RAVENGFX_TANLOG") != NULL;
        const char *hp = lookup("HOR_PLUS");      // HOR_PLUS=1: experimental, site not found yet (view stays Vert-)
        float aspect = ravengfx_win_h > 0 ? (float)ravengfx_win_w / ravengfx_win_h : 0;
        (void)hp; (void)aspect;   // HOR_PLUS is now done on the projection matrix (proj_fix)
        if (getenv("RAVENGFX_HORPLUS_K")) horplus_k = strtof(getenv("RAVENGFX_HORPLUS_K"), NULL);
        if (getenv("RAVENGFX_HORPLUS_SITE")) horplus_site = strtoul(getenv("RAVENGFX_HORPLUS_SITE"), NULL, 0);
        if (tan_log || horplus_k != 1.0f) hook_tanf();
        if (horplus_k != 1.0f) fprintf(stderr, "[ravengfx] Hor+ widening x%.3f\n", horplus_k);
    }
    if (hybrid16()) hook_game_stdio();
    hybrid_frame();
    ravengl_trace_frame(t - t0);
    if (frames % 30 == 0) proj_update();
    { void streamenc_frame(int sw, int sh); streamenc_frame(ravengfx_win_w, ravengfx_win_h); }   // browser stream: game picture only
    if (ravengfx_overlay_draw) ravengfx_overlay_draw(ravengfx_win_w, ravengfx_win_h);
    if (shot_i < nshot && t - t0 >= shot_t[shot_i] && p_glReadPixels) {
        char path[512]; snprintf(path, sizeof path, "%s/shot_%g.ppm", shot_dir, shot_t[shot_i]);
        capture(path); path[strlen(path) - 4] = 0; ravenfg_request_shot(path); shot_i++;
    }
    frames++;
    if (t - tl >= 1) {
        ravengfx_fps = (frames - fl) / (t - tl);
        static double tlog;
        if (fpslog && t - tlog >= 5) { fprintf(stderr, "[ravengfx] fps %.1f at %.0fs\n", ravengfx_fps, t - t0); tlog = t; }
        tl = t; fl = frames;
    }
    (void)dpy; (void)drawable;
}

// The game presents each frame with `call 0x49b060` @0x4fc64e (that function ends in glXSwapBuffers
// from libGLX, called directly, so hooking libGL's dispatch or the GOT doesn't see it). Redirect the
// call through a jmp placed in low memory (a rel32 call can't reach the .so).
#define PRESENT_CALL 0x4fc64eUL
#define PRESENT_FN   0x49b060UL
// RAVENGFX_TANLOG=1: redirect the game's tanf GOT slot and log each distinct caller (for FOV RE).
static float (*real_tanf)(float);

static float log_tanf(float x)
{
    static void *seen[64]; static int ns;
    void *ra = __builtin_return_address(0);
    int k; for (k = 0; k < ns && seen[k] != ra; k++);
    if (tan_log && k == ns && ns < 64) { seen[ns++] = ra; fprintf(stderr, "[ravengfx] tanf(%g) from %p\n", x, ra); }
    float r = real_tanf(x);
    // Hor+ for screens wider than 16:9: camera setup @0x493f0a stores tan(fovA/2) at cam+0xac.
    if (horplus_k > 1.0f && ra == (void *)horplus_site) r *= horplus_k;
    return r;
}
static void hook_tanf(void)
{
    real_tanf = dlsym(RTLD_DEFAULT, "tanf");
    for (uint64_t *q = (uint64_t *)0x1690000UL; q < (uint64_t *)0x1692800UL; q++)
        if (*q == (uint64_t)real_tanf) { *q = (uint64_t)log_tanf; fprintf(stderr, "[ravengfx] tanf slot %p\n", (void *)q); }
}

static void try_set_fullscreen(void *cd_ptr, unsigned long cdr_win)
{
    static int attempts = 0;
    if (attempts >= 5 || !cd_ptr || !cdr_win) return;
    const char *fs = getenv("HALO_FULLSCREEN");
    if (!fs) fs = lookup("fullscreen");
    if (fs && (!strcmp(fs, "0") || !strcasecmp(fs, "off") || !strcasecmp(fs, "false"))) {
        attempts = 999;
        return;
    }

    static void *x11 = NULL;
    if (!x11) x11 = dlopen("libX11.so.6", RTLD_LAZY);
    if (!x11) return;

    typedef unsigned long (*pfn_XInternAtom)(void *, const char *, int);
    typedef unsigned long (*pfn_XDefaultRootWindow)(void *);
    typedef int (*pfn_XSendEvent)(void *, unsigned long, int, long, void *);
    typedef int (*pfn_XFlush)(void *);

    pfn_XInternAtom pXInternAtom = (pfn_XInternAtom)dlsym(x11, "XInternAtom");
    pfn_XDefaultRootWindow pXDefaultRootWindow = (pfn_XDefaultRootWindow)dlsym(x11, "XDefaultRootWindow");
    pfn_XSendEvent pXSendEvent = (pfn_XSendEvent)dlsym(x11, "XSendEvent");
    pfn_XFlush pXFlush = (pfn_XFlush)dlsym(x11, "XFlush");

    if (!pXInternAtom || !pXFlush) return;

    {
        // Standard EWMH _NET_WM_STATE_FULLSCREEN
        if (!pXDefaultRootWindow || !pXSendEvent) return;
        unsigned long wm_state = pXInternAtom(cd_ptr, "_NET_WM_STATE", 0);
        unsigned long wm_fullscreen = pXInternAtom(cd_ptr, "_NET_WM_STATE_FULLSCREEN", 0);
        if (!wm_state || !wm_fullscreen) return;

        struct {
            int type;
            unsigned long serial;
            int send_event;
            void *display;
            unsigned long window;
            unsigned long message_type;
            int format;
            long data[5];
        } xev;
        memset(&xev, 0, sizeof(xev));
        xev.type = 33; // ClientMessage
        xev.window = cdr_win;
        xev.message_type = wm_state;
        xev.format = 32;
        xev.data[0] = 1; // _NET_WM_STATE_ADD
        xev.data[1] = (long)wm_fullscreen;
        xev.data[2] = 0;
        xev.data[3] = 1; // normal application

        unsigned long root = pXDefaultRootWindow(cd_ptr);
        long mask = (1L << 17) | (1L << 19); // SubstructureNotifyMask | SubstructureRedirectMask
        pXSendEvent(cd_ptr, root, 0, mask, &xev);
        pXFlush(cd_ptr);
        attempts++;
        if (attempts == 1) {
            fprintf(stderr, "[ravengfx] requested X11 fullscreen on window 0x%lx\n", cdr_win);
        }
    }
}

// Steam Gaming Mode (gamescope) shows the game only once a window carries STEAM_GAME = the app id.
// The game is started inside a sandbox, so Steam can't label it itself. Done once, on the game's own window.
static void set_steam_game(void *d, unsigned long w)
{
    static int done; if (done) return; done = 1;
    const char *id = getenv("SteamAppId"); if (!id || !*id) id = getenv("SteamGameId");
    unsigned long v = id ? strtoul(id, NULL, 10) : 0;
    if (!v) return;
    void *x11 = dlopen("libX11.so.6", RTLD_LAZY); if (!x11) return;
    unsigned long (*ia)(void *, const char *, int) = dlsym(x11, "XInternAtom");
    int (*cp)(void *, unsigned long, unsigned long, unsigned long, int, int, const unsigned char *, int) = dlsym(x11, "XChangeProperty");
    int (*fl)(void *) = dlsym(x11, "XFlush");
    if (!ia || !cp || !fl) return;
    unsigned long atom = ia(d, "STEAM_GAME", 0); if (!atom) return;
    cp(d, w, atom, 6 /* XA_CARDINAL */, 32, 0 /* PropModeReplace */, (const unsigned char *)&v, 1);
    fl(d);
    fprintf(stderr, "[ravengfx] STEAM_GAME = %lu on window 0x%lx\n", v, w);
}

// The window can be larger than the game's picture (a coop joiner using the host's shape, the 32:9
// 4-player picture, or a fullscreen window): the game draws into the bottom-left corner. Scale the
// picture to fit and centre it, with black bars, before the swap. ravengfx_pic = where it ended up
// (x, y from the top-left, w, h in window pixels), for the mouse aim in rio2pad.c.
int ravengfx_pic[4];
static void centre_picture(void)
{
    static void *(*cd)(void); static unsigned long (*cdr)(void); static void (*qd)(void *, unsigned long, int, unsigned *);
    static void (*gf)(int, unsigned *), (*bf)(unsigned, unsigned), (*gt)(int, unsigned *), (*bt)(unsigned, unsigned);
    static void (*ti)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *), (*ft)(unsigned, unsigned, unsigned, unsigned, int);
    static void (*blit)(int, int, int, int, int, int, int, int, unsigned, unsigned), (*cc)(float, float, float, float), (*cl)(unsigned);
    static void (*gi)(unsigned, int *); static unsigned char (*ie)(unsigned); static void (*dis)(unsigned), (*ena)(unsigned);
    static unsigned fbo, tex; static int tw, th;
    if (!cd) {
        cd = glsym("glXGetCurrentDisplay"); cdr = glsym("glXGetCurrentDrawable"); qd = glsym("glXQueryDrawable");
        gf = glsym("glGenFramebuffers"); bf = glsym("glBindFramebuffer"); gt = glsym("glGenTextures"); bt = glsym("glBindTexture");
        ti = glsym("glTexImage2D"); ft = glsym("glFramebufferTexture2D"); blit = glsym("glBlitFramebuffer");
        cc = glsym("glClearColor"); cl = glsym("glClear"); gi = glsym("glGetIntegerv"); ie = glsym("glIsEnabled");
        dis = glsym("glDisable"); ena = glsym("glEnable");
    }
    int W = ravengfx_win_w, H = ravengfx_win_h;
    if (!qd || !blit || W <= 0 || H <= 0 || !cd() || !cdr()) return;
    unsigned ww = 0, wh = 0;
    qd(cd(), cdr(), 0x801D, &ww); qd(cd(), cdr(), 0x801E, &wh);   // GLX_WIDTH, GLX_HEIGHT
    if (!ww || !wh || ((int)ww == W && (int)wh == H)) { ravengfx_pic[0] = ravengfx_pic[1] = 0; ravengfx_pic[2] = ww; ravengfx_pic[3] = wh; return; }
    int pw = (int)ww, ph = (int)((long long)ww * H / W);
    if (ph > (int)wh) { ph = (int)wh; pw = (int)((long long)wh * W / H); }
    int px = ((int)ww - pw) / 2, py = ((int)wh - ph) / 2;
    ravengfx_pic[0] = px; ravengfx_pic[1] = py; ravengfx_pic[2] = pw; ravengfx_pic[3] = ph;
    int rf, df, tx, sci = ie(0x0C11);
    gi(0x8CAA, &rf); gi(0x8CA6, &df); gi(0x8069, &tx);
    if (!fbo || tw != W || th != H) {
        if (!fbo) { gf(1, &fbo); gt(1, &tex); }
        bt(0x0DE1, tex); ti(0x0DE1, 0, 0x8058, W, H, 0, 0x1908, 0x1401, NULL); bt(0x0DE1, (unsigned)tx);
        bf(0x8CA9, fbo); ft(0x8CA9, 0x8CE0, 0x0DE1, tex, 0);
        tw = W; th = H;
        fprintf(stderr, "[ravengfx] picture %dx%d centred in the %ux%u window at %d,%d (%dx%d)\n", W, H, ww, wh, px, py, pw, ph);
    }
    if (sci) dis(0x0C11);
    bf(0x8CA8, 0); bf(0x8CA9, fbo);                         // copy the game's picture out of the corner
    blit(0, 0, W, H, 0, 0, W, H, 0x4000, 0x2600);
    bf(0x8CA9, 0); cc(0, 0, 0, 1); cl(0x4000);              // black bars
    bf(0x8CA8, fbo);
    blit(0, 0, W, H, px, (int)wh - py - ph, px + pw, (int)wh - py, 0x4000, 0x2601);
    bf(0x8CA8, (unsigned)rf); bf(0x8CA9, (unsigned)df);
    if (sci) ena(0x0C11);
}

// HDR = 1: ravenhdr.c presents through Vulkan HDR10 instead of the game's swap (else it falls back).
int ravenhdr_present(void);
static void present_or_hdr(void *ctx) { if (!ravenhdr_present()) { centre_picture(); ((void (*)(void *))PRESENT_FN)(ctx); } }
static void my_present(void *ctx)
{
    my_swap(NULL, 0);                       // overlay + capture + FPS, before the real swap
    void *(*cd)(void) = glsym("glXGetCurrentDisplay"); unsigned long (*cdr)(void) = glsym("glXGetCurrentDrawable");
    if (cd && cdr) {
        void *d = cd(); unsigned long w = cdr();
        if (d && w) { set_steam_game(d, w); try_set_fullscreen(d, w); }
    }
    static double in_sum, out_sum, tprev; static long n;
    static int si_cur = -2, si_n;          // VSYNC / SWAP_INTERVAL (live from the overlay) / RAVENGFX_SWAPINTERVAL
    if (si_n++ % 30 == 0) {                // 0 = off, 1 = on, -1 = adaptive (tear only when late)
        const char *si = getenv("RAVENGFX_SWAPINTERVAL"); if (!si) si = lookup("VSYNC"); if (!si) si = lookup("SWAP_INTERVAL");
        int want = si ? (!strcmp(si, "adaptive") ? -1 : atoi(si)) : -2;
        if (want != -2 && want != si_cur) {
            si_cur = want;
            void (*ext)(void *, unsigned long, int) = glsym("glXSwapIntervalEXT");
            int (*mesa)(unsigned) = glsym("glXSwapIntervalMESA");
            if (ext && cd && cdr && cd() && cdr()) ext(cd(), cdr(), want); else if (mesa) mesa(want < 0 ? 1 : want);
            fprintf(stderr, "[ravengfx] swap interval %d (%s)\n", want, ext ? "EXT" : mesa ? "MESA" : "none");
        }
    }
    // FPS_CAP (graphics.ini, live from the overlay) / RAVENGFX_FPSCAP: sleep-based frame limiter.
    // Game logic runs on real time (same progress at 30 and 60 FPS), so capping is safe.
    static double cap_dt, next_t; static int cap_n;
    if (cap_n++ % 30 == 0) {
        const char *c = getenv("RAVENGFX_FPSCAP"); if (!c) c = lookup("FPS_CAP");
        cap_dt = c && atof(c) > 0 ? 1.0 / atof(c) : 0;
    }
    if (cap_dt > 0) {
        double t = now_s();
        if (next_t > t) { struct timespec ts = {0, (long)((next_t - t) * 1e9)}; nanosleep(&ts, NULL); }
        next_t = (next_t > t - cap_dt ? next_t : t) + cap_dt;
    }
    double a = now_s();
    ravenfg_present(present_or_hdr, ctx);
    double b = now_s();
    if (fpslog && tprev > 0) {             // where the frame time goes: inside present vs. the rest
        in_sum += b - a; out_sum += a - tprev;
        if (++n == 300) { fprintf(stderr, "[ravengfx] frame ms: present %.2f, rest %.2f\n", in_sum / n * 1e3, out_sum / n * 1e3); n = 0; in_sum = out_sum = 0; }
    }
    tprev = b;
}
// All four callers of the present function (3 calls + 1 tail jmp); patch each rel32 to our low jmp.
static const uint64_t present_sites[] = {0x4b13c5, 0x4b1868, 0x4dfe6f, 0x4fc64e};
static void hook_present(void)
{
    uint8_t *t = MAP_FAILED;
    for (uint64_t at = 0x10000000UL; at < 0x70000000UL && t == MAP_FAILED; at += 0x1000000UL)
        t = mmap((void *)at, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (t == MAP_FAILED) { fprintf(stderr, "[ravengfx] no low memory for present hook\n"); return; }
    write_abs_jmp(t, (uint64_t)my_present);
    int n = 0;
    for (unsigned i = 0; i < sizeof present_sites / sizeof *present_sites; i++) {
        uint8_t *c = (uint8_t *)present_sites[i];
        int32_t rel; memcpy(&rel, c + 1, 4);
        if ((c[0] != 0xe8 && c[0] != 0xe9) || (uint64_t)(c + 5) + rel != PRESENT_FN) continue;
        rel = (int32_t)((int64_t)t - (int64_t)(c + 5));
        mprotect((void *)((uintptr_t)c & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
        memcpy(c + 1, &rel, 4); n++;
    }
    fprintf(stderr, "[ravengfx] frame hook installed at %d/4 present sites\n", n);
}

__attribute__((constructor)) static void ravengfx_init(void)
{
    // Only patch the game binary itself (bash/env etc. also get the preload).
    char exe[256] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0 || !strstr(exe, "/game")) return;

    const char *conf = getenv("RAVENGFX_CONF");
    if (conf && *conf) { load_conf(conf); ravengfx_conf_path = conf; }
    { void ravenfsr_config(void); ravenfsr_config(); }
    const char *sh = getenv("RAVENGFX_SHOT");
    shot_dir = getenv("RAVENGFX_SHOTDIR");
    if (sh && shot_dir) for (char *e; *sh && nshot < 160; sh = *e ? e + 1 : e) shot_t[nshot++] = strtod(sh, &e);
    fpslog = getenv("RAVENGFX_FPSLOG") != NULL;
    const char *kl = getenv("RAVENGFX_KEYLOG");
    if (kl && *kl) keylog = fopen(kl, "w");

    orig_int = hook(CFG_GET_INT, int_prologue, sizeof int_prologue, (void *)hook_int);
    orig_flt = hook(CFG_GET_FLOAT, flt_prologue, sizeof flt_prologue, (void *)hook_flt);
    if (!orig_int || !orig_flt)
        fprintf(stderr, "[ravengfx] unexpected bytes at config getters (different build?), not hooking\n");
    else
        fprintf(stderr, "[ravengfx] config hooks installed\n");

    // Startup check @0x417e53: on 2P/4P gun cabinets (types 3,4,8) an aspect ratio above 2.0 shows
    // "wrong cabinet type" and locks up. NOP the `ja` so ultrawide (21:9, 32:9) can boot.
    if (getenv("RAVENGFX_RENDER")) sscanf(getenv("RAVENGFX_RENDER"), "%dx%d", &render_w, &render_h);
    if (!memcmp((void *)VIDEO_SET_MODE, "\x48\x85\xff\x0f\x84\x84\x00\x00\x00\x8b\x07\x8b\x57\x04", 14)) {
        uint8_t *t = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (t != MAP_FAILED) {
            memcpy(t, (void *)VIDEO_SET_BODY, 5);             // mov (%rdi),%eax; mov 4(%rdi),%edx
            write_abs_jmp(t + 5, VIDEO_SET_BODY + 5);         // back to 0x4fb73e
            set_mode_body = t;
            mprotect((void *)(VIDEO_SET_MODE & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
            write_abs_jmp((uint8_t *)VIDEO_SET_MODE, (uint64_t)hook_set_mode);
        }
    }

    hook_present();
    hook_proj();
    hook_cond();
    proj_update();
    if (!getenv("RAVENGFX_NOUI")) ravenui_start();

    uint8_t *ja = (uint8_t *)ASPECT_LOCK_JA;
    if (ja[0] == 0x77 && ja[1] == 0x9f) {
        mprotect((void *)(ASPECT_LOCK_JA & ~0xfffUL), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
        ja[0] = 0x90; ja[1] = 0x90;
    }
}
