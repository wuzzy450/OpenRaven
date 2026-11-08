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
static struct { char key[64]; char val[32]; } kv[MAXKV];
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
    ravengfx_win_w = *w; ravengfx_win_h = *h;
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
    if (p_glBindFramebuffer) p_glBindFramebuffer(0x8CA8, 0);  // read default framebuffer
    int w = vp[2], h = vp[3];
    if (ravengfx_win_w > 0) { w = ravengfx_win_w; h = ravengfx_win_h; }
    unsigned char *buf = malloc((size_t)w*h*3);
    if (!buf) return;
    p_glReadBuffer(0x0405); p_glPixelStorei(0x0D05, 1);   // GL_BACK, GL_PACK_ALIGNMENT
    p_glReadPixels(0,0,w,h,0x1907,0x1401,buf);             // GL_RGB, GL_UNSIGNED_BYTE
    if (p_glBindFramebuffer) p_glBindFramebuffer(0x8CA8, fbo);
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
static float proj_aspect, proj_fovk = 1.0f; static int proj_on;
__attribute__((used, visibility("hidden"))) void proj_fix(uint8_t *cam);
void proj_fix(uint8_t *cam)
{
    float *p00 = (float *)(cam + 0x170), *p11 = (float *)(cam + 0x184);
    if (!proj_on || *p00 <= 0 || *p11 <= 0) return;
    float shape = *p11 / *p00;
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
    proj_on = hp ? atoi(hp) != 0 : 1;                      // default on
    proj_fovk = fv && atof(fv) > 0.3 ? atof(fv) : 1.0f;
    if (ravengfx_win_h > 0) proj_aspect = (float)ravengfx_win_w / ravengfx_win_h;
    if (fabsf(proj_aspect - 16.0f / 9.0f) < 0.01f && proj_fovk == 1.0f) proj_on = 0;
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
    ravengl_trace_frame(t - t0);
    if (frames % 30 == 0) proj_update();
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
    typedef int (*pfn_XDefaultScreen)(void *);
    typedef int (*pfn_XDisplayWidth)(void *, int);
    typedef int (*pfn_XDisplayHeight)(void *, int);
    typedef int (*pfn_XMoveResizeWindow)(void *, unsigned long, int, int, unsigned int, unsigned int);
    typedef int (*pfn_XChangeProperty)(void *, unsigned long, unsigned long, unsigned long, int, int, const unsigned char *, int);

    pfn_XInternAtom pXInternAtom = (pfn_XInternAtom)dlsym(x11, "XInternAtom");
    pfn_XDefaultRootWindow pXDefaultRootWindow = (pfn_XDefaultRootWindow)dlsym(x11, "XDefaultRootWindow");
    pfn_XSendEvent pXSendEvent = (pfn_XSendEvent)dlsym(x11, "XSendEvent");
    pfn_XFlush pXFlush = (pfn_XFlush)dlsym(x11, "XFlush");
    pfn_XDefaultScreen pXDefaultScreen = (pfn_XDefaultScreen)dlsym(x11, "XDefaultScreen");
    pfn_XDisplayWidth pXDisplayWidth = (pfn_XDisplayWidth)dlsym(x11, "XDisplayWidth");
    pfn_XDisplayHeight pXDisplayHeight = (pfn_XDisplayHeight)dlsym(x11, "XDisplayHeight");
    pfn_XMoveResizeWindow pXMoveResizeWindow = (pfn_XMoveResizeWindow)dlsym(x11, "XMoveResizeWindow");
    pfn_XChangeProperty pXChangeProperty = (pfn_XChangeProperty)dlsym(x11, "XChangeProperty");

    if (!pXInternAtom || !pXFlush) return;

    const char *is_wsl_env = getenv("IS_WSL");
    int is_wsl = (is_wsl_env && atoi(is_wsl_env) != 0) || access("/mnt/wslg", F_OK) == 0;

    if (is_wsl) {
        // Under WSLg (Weston FreeRDP RAIL backend), _NET_WM_STATE_FULLSCREEN causes Weston
        // to unmap or hide the window from the Windows desktop. To achieve borderless fullscreen
        // without dropping out of RAIL, strip decorations via _MOTIF_WM_HINTS and resize to screen.
        if (pXChangeProperty) {
            unsigned long motif_hints = pXInternAtom(cd_ptr, "_MOTIF_WM_HINTS", 0);
            if (motif_hints) {
                // flags = MWM_HINTS_DECORATIONS (2), decorations = 0 (none)
                unsigned long hints[5] = { 2, 0, 0, 0, 0 };
                pXChangeProperty(cd_ptr, cdr_win, motif_hints, motif_hints, 32, 0 /* PropModeReplace */, (const unsigned char *)hints, 5);
            }
        }
        if (pXMoveResizeWindow && pXDefaultScreen && pXDisplayWidth && pXDisplayHeight) {
            int scr = pXDefaultScreen(cd_ptr);
            int sw = pXDisplayWidth(cd_ptr, scr);
            int sh = pXDisplayHeight(cd_ptr, scr);
            if (sw <= 640 && ravengfx_win_w > sw) sw = ravengfx_win_w;
            if (sh <= 480 && ravengfx_win_h > sh) sh = ravengfx_win_h;
            pXMoveResizeWindow(cd_ptr, cdr_win, 0, 0, (unsigned int)sw, (unsigned int)sh);
        }
        pXFlush(cd_ptr);
        attempts++;
        if (attempts == 1) {
            fprintf(stderr, "[ravengfx] applied borderless window for WSLg on window 0x%lx\n", cdr_win);
        }
    } else {
        // Native Linux: standard EWMH _NET_WM_STATE_FULLSCREEN
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

static void my_present(void *ctx)
{
    my_swap(NULL, 0);                       // overlay + capture + FPS, before the real swap
    void *(*cd)(void) = glsym("glXGetCurrentDisplay"); unsigned long (*cdr)(void) = glsym("glXGetCurrentDrawable");
    if (cd && cdr) {
        void *d = cd(); unsigned long w = cdr();
        if (d && w) try_set_fullscreen(d, w);
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
    ravenfg_present((void (*)(void *))PRESENT_FN, ctx);
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
    if (!getenv("RAVENGFX_NOUI")) ravenui_start();

    uint8_t *ja = (uint8_t *)ASPECT_LOCK_JA;
    if (ja[0] == 0x77 && ja[1] == 0x9f) {
        mprotect((void *)(ASPECT_LOCK_JA & ~0xfffUL), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
        ja[0] = 0x90; ja[1] = 0x90;
    }
}
