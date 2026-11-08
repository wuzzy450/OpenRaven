// GLX over WGL. A GLX context becomes a real WGL context the first time it
// is made current on a window (WGL needs an HDC with a pixel format).
// GL entry points are the generated SysV wrappers in gl_thunks.c.
#include "shim.h"
#include "x11_shim.h"
#include <windows.h>
#include <GL/gl.h>
#include <stdlib.h>
#include <string.h>

// ---- resolving real GL functions -----------------------------------------------
void *gl_resolve(const char *name) {
    static HMODULE gl, glu;
    if (!gl) gl = LoadLibraryA("opengl32.dll");
    if (!glu) glu = LoadLibraryA("glu32.dll");
    void *p = (void *)GetProcAddress(gl, name);
    if (!p) p = (void *)GetProcAddress(glu, name);
    if (!p) {
        p = (void *)wglGetProcAddress(name);
        if ((uintptr_t)p <= 3 || p == (void *)-1) p = NULL;
    }
    if (!p) {
        raven_log("FATAL: GL function %s not available from the driver\n", name);
        fflush(NULL);
        ExitProcess(7);
    }
    return p;
}

// ---- configs --------------------------------------------------------------------
typedef struct { int id, r, g, b, a, depth, stencil, dbl, samples; } fbconfig;
static fbconfig g_cfg = {1, 8, 8, 8, 8, 24, 8, 1, 0};

static int cfg_attr(const fbconfig *c, int a, int *v) {
    switch (a) {
    case 1: *v = 1; break;                           // GLX_USE_GL
    case 2: *v = c->r + c->g + c->b + c->a; break;   // BUFFER_SIZE
    case 3: *v = 0; break;                           // LEVEL
    case 4: *v = 1; break;                           // RGBA
    case 5: *v = c->dbl; break;
    case 6: *v = 0; break;                           // STEREO
    case 7: *v = 0; break;                           // AUX_BUFFERS
    case 8: *v = c->r; break;
    case 9: *v = c->g; break;
    case 10: *v = c->b; break;
    case 11: *v = c->a; break;
    case 12: *v = c->depth; break;
    case 13: *v = c->stencil; break;
    case 14: case 15: case 16: case 17: *v = 0; break;  // accum
    case 0x20: *v = 0x8000; break;                   // CONFIG_CAVEAT = NONE
    case 0x22: *v = 0x8002; break;                   // X_VISUAL_TYPE = TRUE_COLOR
    case 0x23: *v = 0x8000; break;                   // TRANSPARENT_TYPE = NONE
    case 0x800B: *v = 0x21; break;                   // VISUAL_ID
    case 0x8010: *v = 7; break;                      // DRAWABLE_TYPE
    case 0x8011: *v = 1; break;                      // RENDER_TYPE = RGBA_BIT
    case 0x8012: *v = 1; break;                      // X_RENDERABLE
    case 0x8013: *v = c->id; break;                  // FBCONFIG_ID
    case 0x8016: case 0x8017: *v = 16384; break;     // MAX_PBUFFER_WIDTH/HEIGHT
    case 0x8018: *v = 16384 * 16384; break;
    case 100000: *v = c->samples ? 1 : 0; break;     // SAMPLE_BUFFERS
    case 100001: *v = c->samples; break;             // SAMPLES
    case 0x20B2: *v = 1; break;                      // FRAMEBUFFER_SRGB_CAPABLE
    default: *v = 0; return 2;                       // GLX_BAD_ATTRIBUTE
    }
    return 0;
}

static SV void *s_glXChooseVisual(void *d, int scr, int *attrs) { (void)d; (void)scr; (void)attrs; return x11_new_vinfo(); }
static SV int s_glXGetConfig(void *d, void *vis, int a, int *v) { (void)d; (void)vis; return cfg_attr(&g_cfg, a, v); }
static SV void **s_glXChooseFBConfig(void *d, int scr, const int *attrs, int *n) {
    (void)d; (void)scr; (void)attrs;
    void **r = malloc(sizeof(void *));
    r[0] = &g_cfg;
    *n = 1;
    return r;
}
static SV void **s_glXGetFBConfigs(void *d, int scr, int *n) { return s_glXChooseFBConfig(d, scr, NULL, n); }
static SV int s_glXGetFBConfigAttrib(void *d, fbconfig *c, int a, int *v) { (void)d; return cfg_attr(c ? c : &g_cfg, a, v); }
static SV void *s_glXGetVisualFromFBConfig(void *d, void *c) { (void)d; (void)c; return x11_new_vinfo(); }

// ---- contexts -----------------------------------------------------------------
typedef struct glx_ctx {
    HGLRC rc;
    struct glx_ctx *share;
    int attribs[32];
    int has_attribs;
} glx_ctx;
static __thread glx_ctx *t_cur;
static __thread uint64_t t_draw;
static void *g_dpy_seen;

typedef HGLRC (WINAPI *create_attribs_fn)(HDC, HGLRC, const int *);

static void set_pixel_format(HDC dc) {
    if (GetPixelFormat(dc)) return;
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1};
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | (g_cfg.dbl ? PFD_DOUBLEBUFFER : 0);
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cAlphaBits = (BYTE)g_cfg.a;
    pfd.cDepthBits = (BYTE)g_cfg.depth;
    pfd.cStencilBits = (BYTE)g_cfg.stencil;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd))
        raven_log("SetPixelFormat failed (%lu)\n", GetLastError());
}

static int realize(glx_ctx *c, HDC dc) {
    if (c->rc) return 1;
    if (c->share && !c->share->rc) realize(c->share, dc);
    HGLRC share = c->share ? c->share->rc : NULL;
    if (c->has_attribs) {
        // Need a temporary context to get wglCreateContextAttribsARB.
        HGLRC tmp = wglCreateContext(dc);
        HGLRC prev = wglGetCurrentContext();
        HDC prevdc = wglGetCurrentDC();
        wglMakeCurrent(dc, tmp);
        create_attribs_fn f = (create_attribs_fn)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
        if (f) c->rc = f(dc, share, c->attribs);
        wglMakeCurrent(prevdc, prev);
        wglDeleteContext(tmp);
        if (!c->rc) raven_log("wglCreateContextAttribsARB failed (%lu), falling back\n", GetLastError());
    }
    if (!c->rc) {
        c->rc = wglCreateContext(dc);
        if (c->rc && share) wglShareLists(share, c->rc);
    }
    raven_log("GL context created: %p (attribs=%d)\n", (void *)c->rc, c->has_attribs);
    return c->rc != NULL;
}

static SV glx_ctx *s_glXCreateContext(void *d, void *vis, glx_ctx *share, int direct) {
    (void)d; (void)vis; (void)direct;
    glx_ctx *c = calloc(1, sizeof *c);
    c->share = share;
    return c;
}
static SV glx_ctx *s_glXCreateNewContext(void *d, void *cfg, int type, glx_ctx *share, int direct) {
    (void)cfg; (void)type;
    return s_glXCreateContext(d, NULL, share, direct);
}
static SV glx_ctx *s_glXCreateContextAttribsARB(void *d, void *cfg, glx_ctx *share, int direct, const int *attrs) {
    (void)cfg;
    glx_ctx *c = s_glXCreateContext(d, NULL, share, direct);
    int n = 0;
    for (; attrs && attrs[n] && n < 30; n += 2) {
        c->attribs[n] = attrs[n];
        c->attribs[n + 1] = attrs[n + 1];
        raven_log("  context attrib 0x%x = 0x%x\n", attrs[n], attrs[n + 1]);
    }
    c->has_attribs = 1;
    return c;
}
static SV void s_glXDestroyContext(void *d, glx_ctx *c) {
    (void)d;
    if (!c) return;
    if (t_cur == c) { wglMakeCurrent(NULL, NULL); t_cur = NULL; }
    if (c->rc) wglDeleteContext(c->rc);
    free(c);
}
static SV int s_glXMakeCurrent(void *d, uint64_t draw, glx_ctx *c) {
    g_dpy_seen = d;
    if (!c || !draw) {
        wglMakeCurrent(NULL, NULL);
        t_cur = NULL;
        t_draw = 0;
        return 1;
    }
    HWND h = x11_hwnd(draw);
    if (!h) { raven_log("glXMakeCurrent: unknown drawable %llu\n", (unsigned long long)draw); return 0; }
    HDC dc = GetDC(h);
    set_pixel_format(dc);
    if (!realize(c, dc)) return 0;
    if (!wglMakeCurrent(dc, c->rc)) {
        raven_log("wglMakeCurrent failed (%lu)\n", GetLastError());
        return 0;
    }
    static int logged;
    if (!logged++) {
        typedef const char *(WINAPI *gs_fn)(unsigned);
        gs_fn gs = (gs_fn)(void *)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetString");
        raven_log("GL: %s | %s | %s\n", gs(GL_VENDOR), gs(GL_RENDERER), gs(GL_VERSION));
    }
    t_cur = c;
    t_draw = draw;
    return 1;
}
static SV int s_glXMakeContextCurrent(void *d, uint64_t draw, uint64_t read, glx_ctx *c) {
    (void)read;
    return s_glXMakeCurrent(d, draw, c);
}
// Screenshot of the back buffer as a BMP in <root>/ravenshots/ (RAVEN_SHOT_EVERY frames, default 600).
static void screenshot(HWND h, unsigned long frame) {
    RECT r;
    GetClientRect(h, &r);
    int w = r.right, ht = r.bottom;
    if (w <= 0 || ht <= 0) return;
    typedef void (WINAPI *rp_fn)(int, int, int, int, unsigned, unsigned, void *);
    typedef void (WINAPI *ps_fn)(unsigned, int);
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    rp_fn rp = (rp_fn)(void *)GetProcAddress(gl, "glReadPixels");
    ps_fn ps = (ps_fn)(void *)GetProcAddress(gl, "glPixelStorei");
    unsigned char *px = malloc((size_t)w * ht * 3 + 64);
    ps(0x0D05, 1);  // GL_PACK_ALIGNMENT
    rp(0, 0, w, ht, 0x80E0, 0x1401, px);  // GL_BGR, GL_UNSIGNED_BYTE
    char dir[MAX_PATH * 2], path[MAX_PATH * 2];
    snprintf(dir, sizeof dir, "%s/ravenshots", g_root);
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s/frame%06lu.bmp", dir, frame);
    FILE *f = fopen(path, "wb");
    if (f) {
        int row = (w * 3 + 3) & ~3;
        BITMAPFILEHEADER fh = {0x4d42, (DWORD)(54 + row * ht), 0, 0, 54};
        BITMAPINFOHEADER ih = {sizeof ih, w, ht, 1, 24, BI_RGB, (DWORD)(row * ht), 0, 0, 0, 0};
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&ih, sizeof ih, 1, f);
        static const char pad[4];
        for (int y = 0; y < ht; y++) {
            fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
            fwrite(pad, 1, (size_t)(row - w * 3), f);
        }
        fclose(f);
        raven_log("screenshot %s (%dx%d)\n", path, w, ht);
    }
    free(px);
}

// The game renders at the size it asked for (e.g. 1280x720) in the lower-left corner of our
// fullscreen window. Before each swap, copy that corner into an FBO and stretch it over the
// whole window, keeping the aspect ratio (black bars if it differs). RAVEN_NOSCALE=1 turns it off.
typedef void (APIENTRY *pfn_gen)(int, unsigned *);
typedef void (APIENTRY *pfn_bind)(unsigned, unsigned);
typedef void (APIENTRY *pfn_rbs)(unsigned, unsigned, int, int);
typedef void (APIENTRY *pfn_fbrb)(unsigned, unsigned, unsigned, unsigned);
typedef void (APIENTRY *pfn_blit)(int, int, int, int, int, int, int, int, unsigned, unsigned);
typedef void (APIENTRY *pfn_geti)(unsigned, int *);
typedef unsigned char (APIENTRY *pfn_isen)(unsigned);
typedef void (APIENTRY *pfn_cap)(unsigned);
typedef void (APIENTRY *pfn_clrc)(float, float, float, float);
typedef void (APIENTRY *pfn_getf)(unsigned, float *);
typedef void (APIENTRY *pfn_clr)(unsigned);
typedef void (APIENTRY *pfn_cmask)(unsigned char, unsigned char, unsigned char, unsigned char);
typedef void (APIENTRY *pfn_getb)(unsigned, unsigned char *);
static void present_scaled(uint64_t draw, int ww, int wh) {
    static int off = -1;
    if (off < 0) { const char *e = raven_env("RAVEN_NOSCALE"); off = e && *e == '1'; }
    int gw, gh;
    if (off || !x11_req_size(draw, &gw, &gh) || (gw >= ww - 1 && gh >= wh - 1)) return;   // 1 px slack: RAVEN_COMPOSE
    static void *ctx;
    static unsigned fbo, rb;
    static int fw, fh;
    static pfn_gen genfb, genrb;
    static pfn_bind bindfb, bindrb;
    static pfn_rbs rbstorage;
    static pfn_fbrb fbrb;
    static pfn_blit blit;
    if (!blit) {
        genfb = (pfn_gen)gl_resolve("glGenFramebuffers");
        genrb = (pfn_gen)gl_resolve("glGenRenderbuffers");
        bindfb = (pfn_bind)gl_resolve("glBindFramebuffer");
        bindrb = (pfn_bind)gl_resolve("glBindRenderbuffer");
        rbstorage = (pfn_rbs)gl_resolve("glRenderbufferStorage");
        fbrb = (pfn_fbrb)gl_resolve("glFramebufferRenderbuffer");
        blit = (pfn_blit)gl_resolve("glBlitFramebuffer");
        raven_log("scaling game picture %dx%d -> window %dx%d\n", gw, gh, ww, wh);
    }
    pfn_geti geti = (pfn_geti)gl_resolve("glGetIntegerv");
    pfn_isen isen = (pfn_isen)gl_resolve("glIsEnabled");
    pfn_cap enable = (pfn_cap)gl_resolve("glEnable"), disable = (pfn_cap)gl_resolve("glDisable");
    pfn_clrc clearcolor = (pfn_clrc)gl_resolve("glClearColor");
    pfn_getf getf = (pfn_getf)gl_resolve("glGetFloatv");
    pfn_clr clear = (pfn_clr)gl_resolve("glClear");
    pfn_cmask cmask = (pfn_cmask)gl_resolve("glColorMask");
    pfn_getb getb = (pfn_getb)gl_resolve("glGetBooleanv");
    if (ctx != (void *)t_cur || fw != gw || fh != gh) {   // FBOs are per context
        ctx = (void *)t_cur;
        fw = gw; fh = gh;
        genfb(1, &fbo);
        genrb(1, &rb);
        int prb = 0;
        geti(0x8CA7, &prb);                               // GL_RENDERBUFFER_BINDING
        bindrb(0x8D41, rb);                               // GL_RENDERBUFFER
        rbstorage(0x8D41, 0x8058, gw, gh);                // GL_RGBA8
        bindrb(0x8D41, (unsigned)prb);
        int pd = 0;
        geti(0x8CA6, &pd);                                // GL_DRAW_FRAMEBUFFER_BINDING
        bindfb(0x8CA9, fbo);                              // GL_DRAW_FRAMEBUFFER
        fbrb(0x8CA9, 0x8CE0, 0x8D41, rb);                 // GL_COLOR_ATTACHMENT0
        bindfb(0x8CA9, (unsigned)pd);
    }
    // Save the state a blit/clear depends on.
    int pread = 0, pdraw = 0;
    geti(0x8CAA, &pread);                                 // GL_READ_FRAMEBUFFER_BINDING
    geti(0x8CA6, &pdraw);
    unsigned char scissor = isen(0x0C11), srgb = isen(0x8DB9), cm[4];
    float cc[4];
    getf(0x0C22, cc);                                     // GL_COLOR_CLEAR_VALUE
    getb(0x0C23, cm);                                     // GL_COLOR_WRITEMASK
    if (scissor) disable(0x0C11);
    if (srgb) disable(0x8DB9);                            // GL_FRAMEBUFFER_SRGB
    cmask(1, 1, 1, 1);

    // back buffer corner -> FBO
    bindfb(0x8CA8, 0);                                    // GL_READ_FRAMEBUFFER
    bindfb(0x8CA9, fbo);
    blit(0, 0, gw, gh, 0, 0, gw, gh, 0x4000, 0x2600);     // GL_COLOR_BUFFER_BIT, GL_NEAREST
    // clear the window, then FBO -> whole window (aspect kept)
    bindfb(0x8CA9, 0);
    clearcolor(0, 0, 0, 1);
    clear(0x4000);
    int dw = ww, dh = (int)((long long)ww * gh / gw);
    if (dh > wh) { dh = wh; dw = (int)((long long)wh * gw / gh); }
    int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    bindfb(0x8CA8, fbo);
    blit(0, 0, gw, gh, dx, dy, dx + dw, dy + dh, 0x4000, 0x2601);   // GL_LINEAR

    bindfb(0x8CA8, (unsigned)pread);
    bindfb(0x8CA9, (unsigned)pdraw);
    clearcolor(cc[0], cc[1], cc[2], cc[3]);
    cmask(cm[0], cm[1], cm[2], cm[3]);
    if (scissor) enable(0x0C11);
    if (srgb) enable(0x8DB9);
}

// RAVEN_SHOT_RANGE=from-to: a 480x270 BMP of every frame in that range (ravenshots/sNNNNNN.bmp).
// Cheap (GPU downscale + tiny readback) so it does not change the game's timing much. Debug only.
static void small_shot(int ww, int wh, unsigned long frame) {
    static long from = -1, to = -1;
    if (from == -1) {
        const char *e = raven_env("RAVEN_SHOT_RANGE");
        from = -2;
        if (e) { from = strtol(e, NULL, 10); const char *d = strchr(e, '-'); to = d ? strtol(d + 1, NULL, 10) : from; }
    }
    if (from < 0 || (long)frame < from || (long)frame > to || ww <= 0) return;
    enum { SW = 480, SH = 270 };
    static void *ctx; static unsigned fbo, rb;
    pfn_gen genfb = (pfn_gen)gl_resolve("glGenFramebuffers"), genrb = (pfn_gen)gl_resolve("glGenRenderbuffers");
    pfn_bind bindfb = (pfn_bind)gl_resolve("glBindFramebuffer"), bindrb = (pfn_bind)gl_resolve("glBindRenderbuffer");
    pfn_rbs rbs = (pfn_rbs)gl_resolve("glRenderbufferStorage");
    pfn_fbrb fbrb = (pfn_fbrb)gl_resolve("glFramebufferRenderbuffer");
    pfn_blit blit = (pfn_blit)gl_resolve("glBlitFramebuffer");
    pfn_geti geti = (pfn_geti)gl_resolve("glGetIntegerv");
    pfn_isen isen = (pfn_isen)gl_resolve("glIsEnabled");
    pfn_cap enable = (pfn_cap)gl_resolve("glEnable"), disable = (pfn_cap)gl_resolve("glDisable");
    typedef void (APIENTRY *rp_fn)(int, int, int, int, unsigned, unsigned, void *);
    typedef void (APIENTRY *ps_fn)(unsigned, int);
    rp_fn rp = (rp_fn)gl_resolve("glReadPixels");
    ps_fn ps = (ps_fn)gl_resolve("glPixelStorei");
    int pread = 0, pdraw = 0, prb = 0, ppack = 0, palign = 4;
    geti(0x8CAA, &pread); geti(0x8CA6, &pdraw); geti(0x8CA7, &prb); geti(0x88ED, &ppack); geti(0x0D05, &palign);
    if (ctx != (void *)t_cur) {
        ctx = (void *)t_cur;
        genfb(1, &fbo); genrb(1, &rb);
        bindrb(0x8D41, rb); rbs(0x8D41, 0x8058, SW, SH); bindrb(0x8D41, (unsigned)prb);
        bindfb(0x8CA9, fbo); fbrb(0x8CA9, 0x8CE0, 0x8D41, rb);
    }
    unsigned char sc = isen(0x0C11), srgb = isen(0x8DB9);
    if (sc) disable(0x0C11);
    if (srgb) disable(0x8DB9);
    bindfb(0x8CA8, 0); bindfb(0x8CA9, fbo);
    blit(0, 0, ww, wh, 0, 0, SW, SH, 0x4000, 0x2601);
    bindfb(0x8CA8, fbo);
    pfn_bind bindbuf = (pfn_bind)gl_resolve("glBindBuffer");
    bindbuf(0x88EB, 0);
    ps(0x0D05, 1);
    static unsigned char px[SW * SH * 3];
    rp(0, 0, SW, SH, 0x80E0, 0x1401, px);
    ps(0x0D05, palign); bindbuf(0x88EB, (unsigned)ppack);
    bindfb(0x8CA8, (unsigned)pread); bindfb(0x8CA9, (unsigned)pdraw);
    if (sc) enable(0x0C11);
    if (srgb) enable(0x8DB9);
    char dir[MAX_PATH * 2], path[MAX_PATH * 2];
    snprintf(dir, sizeof dir, "%s/ravenshots", g_root);
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s/s%06lu.bmp", dir, frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh = {0x4d42, (DWORD)(54 + sizeof px), 0, 0, 54};
    BITMAPINFOHEADER ih = {sizeof ih, SW, SH, 1, 24, BI_RGB, (DWORD)sizeof px, 0, 0, 0, 0};
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(px, 1, sizeof px, f);
    fclose(f);
}

static HWND g_game_hwnd;
HWND raven_game_hwnd(void) { return g_game_hwnd; }  // window the game presents to (overlay mouse)
int ravenhdr_present(HWND h);   // ravenhdr_win.c: HDR10 through DXGI when HDR is on (1 = presented)
static void real_swap(void *h) { if (!ravenhdr_present((HWND)h)) SwapBuffers(GetDC((HWND)h)); }
void raven_fg_swap(void) { if (g_game_hwnd) real_swap(g_game_hwnd); }   // generated frames   // generated frames
static SV void s_glXSwapBuffers(void *d, uint64_t draw) {
    (void)d;
    HWND h = x11_hwnd(draw);
    if (!h) return;
    g_game_hwnd = h;
    static unsigned long frames, every;
    static ULONGLONG t0, tlast;
    static unsigned long flast;
    if (!t0) {
        // Off by default: a 4K readback + BMP write stalls the game ~0.5 s (looked like a flicker).
        const char *e = raven_env("RAVEN_SHOT_EVERY");
        every = e ? strtoul(e, NULL, 10) : 0;
        t0 = tlast = GetTickCount64();
    }
    frames++;
    extern void ravengfx_frame(void);
    ravengfx_frame();  // graphics.ini per-frame work (VSYNC, FPS_CAP, Hor+), on the game's picture
    RECT cr;
    GetClientRect(h, &cr);
    present_scaled(draw, cr.right, cr.bottom);
    if (every && frames % every == 0) screenshot(h, frames);
    small_shot(cr.right, cr.bottom, frames);
    // Flicker hunt: log hitches (frame gap > 50 ms), slow swaps, and another window in front.
    static LARGE_INTEGER qf, qprev;
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    LARGE_INTEGER qa, qb;
    QueryPerformanceCounter(&qa);
    extern void ravenfg_present(void (*real)(void *), void *ctx);
    ravenfg_present(real_swap, h);   // frame generation: may show generated frames first (ravenfg.c)
    QueryPerformanceCounter(&qb);
    {
        static int nh;
        double sw = (double)(qb.QuadPart - qa.QuadPart) * 1000.0 / (double)qf.QuadPart;
        double gap = qprev.QuadPart ? (double)(qb.QuadPart - qprev.QuadPart) * 1000.0 / (double)qf.QuadPart : 0;
        qprev = qb;
        if ((gap > 50 || sw > 40) && nh++ < 400)
            raven_log("[flicker] frame %lu: gap %.1f ms, SwapBuffers %.1f ms at %.1fs\n", frames, gap, sw,
                      (GetTickCount64() - t0) / 1000.0);
        HWND fg = GetForegroundWindow();
        static HWND lastfg;
        if (fg != lastfg) {
            char cls[64] = "", title[96] = "";
            DWORD pid = 0;
            if (fg) { GetClassNameA(fg, cls, sizeof cls); GetWindowTextA(fg, title, sizeof title); GetWindowThreadProcessId(fg, &pid); }
            if (nh++ < 400)
                raven_log("[flicker] frame %lu: foreground -> %p class '%s' title '%s' pid %lu%s at %.1fs\n", frames,
                          (void *)fg, cls, title, pid, pid == GetCurrentProcessId() ? " (us)" : "",
                          (GetTickCount64() - t0) / 1000.0);
            lastfg = fg;
        }
    }
    ULONGLONG now = GetTickCount64();
    if (now - tlast >= 5000) {
        RECT r;
        GetClientRect(h, &r);
        RECT wr;
        GetWindowRect(h, &wr);
        raven_log("frames %lu (%.1f fps, %ldx%ld, window %ldx%ld, dpi awareness %d) at %.1fs\n", frames,
                  (frames - flast) * 1000.0 / (double)(now - tlast), r.right, r.bottom,
                  wr.right - wr.left, wr.bottom - wr.top,
                  (int)GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()), (now - t0) / 1000.0);
        tlast = now;
        flast = frames;
    }
}
static SV glx_ctx *s_glXGetCurrentContext(void) { return t_cur; }
static SV uint64_t s_glXGetCurrentDrawable(void) { return t_draw; }
static SV void *s_glXGetCurrentDisplay(void) { return g_dpy_seen; }

typedef BOOL (WINAPI *swapint_fn)(int);
static int g_swap_interval = 1;
static int g_forced = -2;  // set from graphics.ini VSYNC (ravengfx): the game's own calls don't change it
static int set_interval(int n) {
    if (g_forced != -2) n = g_forced;
    swapint_fn f = (swapint_fn)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    g_swap_interval = n;
    raven_log("swap interval %d (%s)\n", n, f ? "ok" : "no wglSwapIntervalEXT");
    return f && f(n) ? 0 : 1;
}
void raven_set_swap_interval(int n) {
    if (n == g_forced) return;
    g_forced = n;
    set_interval(n);
}
static SV void s_glXSwapIntervalEXT(void *d, uint64_t draw, int n) { (void)d; (void)draw; set_interval(n); }
static SV int s_glXSwapIntervalMESA(unsigned n) { return set_interval((int)n); }
static SV int s_glXSwapIntervalSGI(int n) { return set_interval(n); }
static SV int s_glXGetSwapIntervalMESA(void) { return g_swap_interval; }
static SV void s_glXQueryDrawable(void *d, uint64_t draw, int a, unsigned *v) {
    (void)d;
    RECT r = {0};
    HWND h = x11_hwnd(draw);
    if (h) GetClientRect(h, &r);
    switch (a) {
    case 0x20F1: *v = (unsigned)g_swap_interval; break;  // SWAP_INTERVAL_EXT
    case 0x20F2: *v = 4; break;                          // MAX_SWAP_INTERVAL_EXT
    case 0x801D: *v = (unsigned)r.right; break;          // WIDTH
    case 0x801E: *v = (unsigned)r.bottom; break;         // HEIGHT
    default: *v = 0;
    }
}

static const char *g_glx_ext =
    "GLX_ARB_create_context GLX_ARB_create_context_profile GLX_ARB_get_proc_address "
    "GLX_ARB_multisample GLX_EXT_visual_info GLX_EXT_swap_control GLX_MESA_swap_control "
    "GLX_SGI_swap_control GLX_ARB_framebuffer_sRGB";
static SV int s_glXQueryExtension(void *d, int *err, int *ev) { (void)d; if (err) *err = 0; if (ev) *ev = 0; return 1; }
static SV int s_glXQueryVersion(void *d, int *maj, int *min) { (void)d; if (maj) *maj = 1; if (min) *min = 4; return 1; }
static SV const char *s_glXQueryExtensionsString(void *d, int s) { (void)d; (void)s; return g_glx_ext; }
static SV const char *s_glXGetClientString(void *d, int name) {
    (void)d;
    return name == 1 ? "ravenloader" : name == 2 ? "1.4" : g_glx_ext;  // VENDOR, VERSION, EXTENSIONS
}
static SV const char *s_glXQueryServerString(void *d, int s, int name) { (void)s; return s_glXGetClientString(d, name); }
static SV int s_glXIsDirect(void *d, void *c) { (void)d; (void)c; return 1; }
static SV void s_nop(void) {}
// GLX 1.3 windows: the X window itself is the drawable.
static SV uint64_t s_glXCreateWindow(void *d, void *cfg, uint64_t win, const int *a) { (void)d; (void)cfg; (void)a; return win; }
static SV int s_glXQueryContext(void *d, void *c, int a, int *v) {
    (void)d; (void)c;
    *v = a == 0x8013 ? 1 : a == 0x8011 ? 0x8014 : 0;  // FBCONFIG_ID, RENDER_TYPE = RGBA_TYPE
    return 0;
}
static SV void s_glXGetSelectedEvent(void *d, uint64_t draw, unsigned long *m) { (void)d; (void)draw; *m = 0; }

static SV void *s_glXGetProcAddress(const char *name) {
    void *p = shim_lookup(name);
    if (!p) raven_log("glXGetProcAddress(%s) -> NULL\n", name);
    return p;
}

const shim_entry shims_glx[] = {
    {"glXGetProcAddress", s_glXGetProcAddress}, {"glXGetProcAddressARB", s_glXGetProcAddress},
    {"glXChooseVisual", s_glXChooseVisual}, {"glXGetConfig", s_glXGetConfig},
    {"glXChooseFBConfig", s_glXChooseFBConfig}, {"glXGetFBConfigs", s_glXGetFBConfigs},
    {"glXGetFBConfigAttrib", s_glXGetFBConfigAttrib},
    {"glXGetVisualFromFBConfig", s_glXGetVisualFromFBConfig},
    {"glXCreateContext", s_glXCreateContext}, {"glXCreateNewContext", s_glXCreateNewContext},
    {"glXCreateContextAttribsARB", s_glXCreateContextAttribsARB},
    {"glXDestroyContext", s_glXDestroyContext}, {"glXMakeCurrent", s_glXMakeCurrent},
    {"glXMakeContextCurrent", s_glXMakeContextCurrent}, {"glXSwapBuffers", s_glXSwapBuffers},
    {"glXGetCurrentContext", s_glXGetCurrentContext}, {"glXGetCurrentDrawable", s_glXGetCurrentDrawable},
    {"glXGetCurrentDisplay", s_glXGetCurrentDisplay},
    {"glXSwapIntervalEXT", s_glXSwapIntervalEXT}, {"glXSwapIntervalMESA", s_glXSwapIntervalMESA},
    {"glXSwapIntervalSGI", s_glXSwapIntervalSGI}, {"glXGetSwapIntervalMESA", s_glXGetSwapIntervalMESA},
    {"glXQueryDrawable", s_glXQueryDrawable},
    {"glXQueryExtension", s_glXQueryExtension}, {"glXQueryVersion", s_glXQueryVersion},
    {"glXQueryExtensionsString", s_glXQueryExtensionsString},
    {"glXGetClientString", s_glXGetClientString}, {"glXQueryServerString", s_glXQueryServerString},
    {"glXIsDirect", s_glXIsDirect}, {"glXWaitGL", s_nop}, {"glXWaitX", s_nop},
    {"glXCreateWindow", s_glXCreateWindow}, {"glXDestroyWindow", s_nop},
    {"glXGetCurrentReadDrawable", s_glXGetCurrentDrawable},
    {"glXQueryContext", s_glXQueryContext}, {"glXSelectEvent", s_nop},
    {"glXGetSelectedEvent", s_glXGetSelectedEvent},
    {NULL, NULL},
};
