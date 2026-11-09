// ravenfg.c - frame generation (2x/3x/4x) from the engine's own motion vectors and depth.
//
// graphics.ini (all live from the F1 overlay):
//   FRAME_GEN = off | 2x | 3x | 4x     FG_MODE = interpolate | extrapolate     FG_DEBUG = 0 | 1
//
// How it works:
// - During the frame (GL wrappers in ravengltrace.c call ravenfg_draw before each draw):
//   * the G-buffer FBO is found by its RG16F velocity attachment; we remember its velocity and depth
//     textures (they still hold this frame's data when the frame is presented);
//   * just before the HUD is drawn (the first time the program drawing to the window changes), the
//     window contents are copied: that is the scene without HUD.
// - At present (ravenfg_present): the finished frame (scene + HUD) is copied, then N-1 frames are
//   generated and presented, then the real frame is put back and presented by the game as usual.
//   Interpolate: frames between the previous real frame A and the current one B (t = k/N), adds
//   about one generated frame of latency. Extrapolate: frames after B, predicted from B's motion
//   (no added latency, more artefacts at edges of moving objects).
// - The generated image: for each pixel we solve x_B = y + (1-t) v(x_B) (v = engine velocity, UV,
//   current minus previous, depth-dilated so object edges follow the nearer surface). If that
//   converges we sample B there and A at y - t v and blend; if not (an area hidden in one frame),
//   we use the background motion and take the pixel from whichever frame sees it.
// - HUD: the difference between the finished frame and the scene copy is laid over the generated
//   frame unmoved, so HUD and text never warp.
// Plain OpenGL 3.3 (fragment shaders, blits). Works the same on NVIDIA, AMD, Intel and Mesa.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
// Windows (openraven/win, built into ravenloader.exe): GL from the loader, swaps on the game window.
#include <windows.h>
void *gl_resolve(const char *name); void raven_log(const char *fmt, ...); const char *raven_env(const char *name);
void raven_fg_swap(void);   // shim_glx.c: SwapBuffers on the game window
#define fprintf(f, ...) raven_log(__VA_ARGS__)
#define getenv raven_env
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#else
#include <dlfcn.h>
#endif
#include <stdarg.h>

typedef unsigned GLenum, GLuint; typedef int GLint;
const char *ravengfx_get(const char *key);
extern int ravengfx_win_w, ravengfx_win_h;
void ravengfx_capture(const char *path);
extern float ravengfx_fg_fps;            // displayed frames per second (real + generated)

#ifdef _WIN32
void *gl_resolve_opt(const char *name);
static void *gpa(const char *n) { return gl_resolve_opt(n); }   // missing = NULL (ravenfg/ravennn check)
static void w_swap(void *d, unsigned long w) { (void)d; (void)w; raven_fg_swap(); }
static void *w_dpy(void) { return (void *)1; }
static unsigned long w_drw(void) { return 1; }
#else
static void *gpa(const char *n)
{
    static void *(*g)(const char *);
    if (!g) g = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    void *f = g ? g(n) : NULL;
    return f ? f : dlsym(RTLD_DEFAULT, n);
}
#endif
#define G(ret, name, args) static ret (*name) args;
G(void, gGetIntegerv, (GLenum, GLint *)) G(void, gGetBooleanv, (GLenum, unsigned char *))
G(unsigned char, gIsEnabled, (GLenum)) G(void, gEnable, (GLenum)) G(void, gDisable, (GLenum))
G(void, gBindFramebuffer, (GLenum, GLuint)) G(void, gGenFramebuffers, (int, GLuint *))
G(void, gFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
G(void, gBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, unsigned, GLenum))
G(void, gGenTextures, (int, GLuint *)) G(void, gDeleteTextures, (int, const GLuint *)) G(void, gBindTexture, (GLenum, GLuint))
G(void, gTexImage2D, (GLenum, GLint, GLint, int, int, GLint, GLenum, GLenum, const void *))
G(void, gTexParameteri, (GLenum, GLenum, GLint)) G(void, gGetTexParameteriv, (GLenum, GLenum, GLint *))
G(void, gGetTexLevelParameteriv, (GLenum, GLint, GLenum, GLint *))
G(void, gActiveTexture, (GLenum)) G(void, gBindSampler, (GLuint, GLuint))
G(GLuint, gCreateShader, (GLenum)) G(void, gShaderSource, (GLuint, int, const char **, const int *))
G(void, gCompileShader, (GLuint)) G(GLuint, gCreateProgram, (void)) G(void, gAttachShader, (GLuint, GLuint))
G(void, gLinkProgram, (GLuint)) G(void, gGetProgramiv, (GLuint, GLenum, GLint *))
G(void, gGetShaderInfoLog, (GLuint, int, int *, char *)) G(void, gGetProgramInfoLog, (GLuint, int, int *, char *))
G(void, gUseProgram, (GLuint)) G(GLint, gGetUniformLocation, (GLuint, const char *))
G(void, gUniform1i, (GLint, GLint)) G(void, gUniform1f, (GLint, float)) G(void, gUniform2f, (GLint, float, float))
G(void, gUniform4f, (GLint, float, float, float, float))
G(void, gGenVertexArrays, (int, GLuint *)) G(void, gBindVertexArray, (GLuint)) G(void, gDrawArrays, (GLenum, GLint, int))
G(void, gViewport, (GLint, GLint, int, int)) G(void, gColorMask, (unsigned char, unsigned char, unsigned char, unsigned char))
G(void, gDepthMask, (unsigned char)) G(void, gReadBuffer, (GLenum)) G(void, gDrawBuffer, (GLenum))
G(void, gGetFramebufferAttachmentParameteriv, (GLenum, GLenum, GLenum, GLint *))
G(void, gFlush, (void))
static void (*xSwap)(void *, unsigned long); static void *(*xDpy)(void); static unsigned long (*xDrw)(void);

static int gl_ok = -1; static int nn_on, nn_ran; static float nn_str = 1, nn_stable_set = 0.6f, nn_protect = 0.9f;
static int n_gen = 1, mode_extra, dbg;       // n_gen = N (1 = off)
static GLuint fbo_rd, fbo_wr, vao, prog;
static GLuint sc[3], fin;                    // scene copies (ring of 3), finished frame copy
static int cur;                              // index of the newest scene copy in sc[]
#define PREV ((cur + 2) % 3)
#define OLD ((cur + 1) % 3)
static int eval_mode, eval_pending, have_old; static float vscale = 1;
static int tw, th;                           // copy size (window)
static int have_prev, have_scene, scene_frame;
static GLuint vel_tex, dep_tex; static int vel_w, vel_h, have_vel;
static GLuint win_prog; static int win_draws, captured;
static GLint u_vs, u_A, u_B, u_F, u_V, u_D, u_t, u_kb, u_hasA, u_hasV, u_dbg, u_vres, u_hud;
static long frames;
static double now_s(void);
static double last_real_t, real_dt = 1.0 / 60, last_swap_t;
// Adaptive: generated frames only fill refresh slots the game can't. render_t = game's own time per
// frame (from the real present returning to the next present call); period = screen refresh interval
// (shortest swap-to-swap interval seen in the last second). Generating when the game already reaches
// the refresh rate would only lower its real frame rate, so we don't.
static double last_real_ret, render_t = 1.0 / 120, period = 1.0 / 60, win_min = 1, win_t0, prev_swap_ret;
static int n_use = 1, force_all;
static double hw_period;   // from XRandR (or REFRESH_RATE in graphics.ini); 0 = unknown
static void query_refresh(void)
{
    const char *o = ravengfx_get("REFRESH_RATE"); if (getenv("RAVENFG_REFRESH")) o = getenv("RAVENFG_REFRESH");
    if (o && atof(o) > 20) { hw_period = 1.0 / atof(o); fprintf(stderr, "[ravenfg] refresh %.1f Hz (graphics.ini)\n", atof(o)); return; }
#ifdef _WIN32
    DEVMODEA dm = {0}; dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 20) {
        hw_period = 1.0 / dm.dmDisplayFrequency;
        fprintf(stderr, "[ravenfg] screen refresh %lu Hz\n", (unsigned long)dm.dmDisplayFrequency);
    }
#else
    void *x = dlopen("libXrandr.so.2", RTLD_NOW); if (!x || !xDpy) return;
    void *(*gsi)(void *, unsigned long) = dlsym(x, "XRRGetScreenInfo"); short (*rate)(void *) = dlsym(x, "XRRConfigCurrentRate");
    void (*fr)(void *) = dlsym(x, "XRRFreeScreenConfigInfo");
    unsigned long (*root)(void *, int) = dlsym(RTLD_DEFAULT, "XRootWindow"); int (*dscr)(void *) = dlsym(RTLD_DEFAULT, "XDefaultScreen");
    void *d = xDpy(); if (!gsi || !rate || !root || !dscr || !d) return;
    void *ci = gsi(d, root(d, dscr(d))); if (!ci) return;
    short r = rate(ci); if (fr) fr(ci);
    if (r > 20) hw_period = 1.0 / r;
    fprintf(stderr, "[ravenfg] screen refresh %d Hz\n", r);
#endif
}
static void swap_done(void)
{
    double t = now_s(), d = t - prev_swap_ret; prev_swap_ret = t; last_swap_t = t;
    if (d > 0.003 && d < win_min) win_min = d;
    if (t - win_t0 > 1.0) { if (win_min < 0.1) period = win_min; if (hw_period > 0) period = hw_period; win_min = 1; win_t0 = t; }
}

#ifdef _WIN32
static double now_s(void) { static LARGE_INTEGER f; LARGE_INTEGER c; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
#else
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#endif

static const char *vs_src = "#version 330 core\nout vec2 uv;\n"
    "void main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *fs_src =
"#version 330 core\n"
"uniform sampler2D A, B, F, V, D;\n"     // scene A (older), scene B (newer), finished B, velocity, depth
"uniform float t, kb, vs;\n"                 // t = time of the generated frame (A=0, B=1); kb = 1-t
"uniform int hasA, hasV, dbg, hud;\n"
"uniform vec2 vres;\n"                   // velocity/depth texture size
"in vec2 uv; out vec4 o;\n"
"vec3 vd(vec2 p){ ivec2 q = ivec2(clamp(p, vec2(0.0), vec2(0.99999)) * vres); return vec3(texelFetch(V, q, 0).xy * vs, texelFetch(D, q, 0).r); }\n"
// nearest-depth velocity in a 3x3 texel neighbourhood (object edges follow the foreground)
"vec3 vnear(vec2 p){ vec3 b = vd(p); vec2 s = 1.0 / vres;\n"
"  for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) { if (i == 0 && j == 0) continue;\n"
"    vec3 c = vd(p + vec2(i, j) * s); if (c.z < b.z) b = c; } return b; }\n"
// farthest-depth velocity over a wider area (the background behind a moving object)
"vec3 vfar(vec2 p){ vec3 b = vd(p); vec2 s = 6.0 / vres;\n"
"  for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) { vec3 c = vd(p + vec2(i, j) * s); if (c.z > b.z) b = c; } return b; }\n"
"bool inside(vec2 p){ return all(greaterThanEqual(p, vec2(0.0))) && all(lessThanEqual(p, vec2(1.0))); }\n"
"float lum(vec3 c){ return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
"void main(){\n"
"  vec3 col; float conf = 1.0;\n"
"  if (hasV == 0) {\n"
"    col = hasA != 0 ? mix(texture(A, uv).rgb, texture(B, uv).rgb, clamp(t, 0.0, 1.0)) : texture(B, uv).rgb;\n"
"  } else {\n"
"    vec2 v = vnear(uv).xy;\n"
"    for (int i = 0; i < 3; i++) v = vnear(uv + kb * v).xy;\n"     // x_B = y + (1-t) v(x_B)
"    vec2 pB = uv + kb * v; vec2 vB = vnear(pB).xy;\n"
"    vec2 px = vec2(textureSize(B, 0));\n"
"    bool conv = length((vB - v) * px) * abs(kb) < 1.0 + 0.05 * length(v * px);\n"
"    if (conv && inside(pB)) {\n"
"      vec3 cB = texture(B, pB).rgb;\n"
"      vec2 pA = uv - t * v;\n"
"      if (hasA != 0 && t < 1.0 && inside(pA)) {\n"
"        vec3 cA = texture(A, pA).rgb;\n"
"        float d = abs(lum(cA) - lum(cB));\n"
"        col = mix(cA, cB, t);\n"
"      } else col = cB;\n"
"    } else {\n"                                                   // hidden in B (or off-screen): background motion
"      conf = 0.0;\n"
"      vec3 bg = vfar(uv); vec2 vb = bg.xy;\n"
"      vec2 pB2 = uv + kb * vb; vec3 atB = vd(pB2);\n"
"      vec2 pA2 = uv - t * vb;\n"
"      bool bgInB = inside(pB2) && atB.z >= bg.z - 0.0005 * (1.0 - bg.z) - 1e-5;\n"
"      if (bgInB || hasA == 0 || t >= 1.0 || !inside(pA2)) col = texture(B, clamp(pB2, vec2(0.0), vec2(1.0))).rgb;\n"
"      else col = texture(A, pA2).rgb;\n"
"    }\n"
"  }\n"
"  if (hud != 0) {\n"                                              // lay the HUD of B over it, unmoved
"    vec3 f = texture(F, uv).rgb, s = texture(B, uv).rgb, d = f - s;\n"
"    float m = smoothstep(0.03, 0.25, max(max(abs(d.r), abs(d.g)), abs(d.b)));\n"
"    col = mix(clamp(col + d, 0.0, 1.0), f, m);\n"
"  }\n"
"  if (dbg != 0) {\n"
"    vec2 e = min(uv, 1.0 - uv) * vec2(textureSize(B, 0));\n"
"    if (min(e.x, e.y) < 6.0) col = vec3(1.0, 0.0, 1.0);\n"
"    if (dbg == 2 && conf < 0.5) col = mix(col, vec3(1.0, 0.0, 0.0), 0.5);\n"
"  }\n"
"  o = vec4(col, 1.0);\n"
"}\n";

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = gCreateShader(type); gShaderSource(s, 1, &src, NULL); gCompileShader(s);
    char log[2048]; int n = 0; gGetShaderInfoLog(s, sizeof log, &n, log);
    if (n > 1) fprintf(stderr, "[ravenfg] shader log: %s\n", log);
    return s;
}

static int init_gl(void)
{
#define L(v, n) if (!(v = (__typeof__(v))gpa(n))) { fprintf(stderr, "[ravenfg] missing %s, frame generation off\n", n); return 0; }
    L(gGetIntegerv, "glGetIntegerv") L(gGetBooleanv, "glGetBooleanv") L(gIsEnabled, "glIsEnabled") L(gEnable, "glEnable")
    L(gDisable, "glDisable") L(gBindFramebuffer, "glBindFramebuffer") L(gGenFramebuffers, "glGenFramebuffers")
    L(gFramebufferTexture2D, "glFramebufferTexture2D") L(gBlitFramebuffer, "glBlitFramebuffer") L(gGenTextures, "glGenTextures")
    L(gDeleteTextures, "glDeleteTextures") L(gBindTexture, "glBindTexture") L(gTexImage2D, "glTexImage2D")
    L(gTexParameteri, "glTexParameteri") L(gGetTexParameteriv, "glGetTexParameteriv") L(gGetTexLevelParameteriv, "glGetTexLevelParameteriv")
    L(gActiveTexture, "glActiveTexture") L(gBindSampler, "glBindSampler") L(gCreateShader, "glCreateShader")
    L(gShaderSource, "glShaderSource") L(gCompileShader, "glCompileShader") L(gCreateProgram, "glCreateProgram")
    L(gAttachShader, "glAttachShader") L(gLinkProgram, "glLinkProgram") L(gGetProgramiv, "glGetProgramiv")
    L(gGetShaderInfoLog, "glGetShaderInfoLog") L(gGetProgramInfoLog, "glGetProgramInfoLog") L(gUseProgram, "glUseProgram")
    L(gGetUniformLocation, "glGetUniformLocation") L(gUniform1i, "glUniform1i") L(gUniform1f, "glUniform1f")
    L(gUniform2f, "glUniform2f") L(gUniform4f, "glUniform4f") L(gGenVertexArrays, "glGenVertexArrays")
    L(gBindVertexArray, "glBindVertexArray") L(gDrawArrays, "glDrawArrays") L(gViewport, "glViewport")
    L(gColorMask, "glColorMask") L(gDepthMask, "glDepthMask") L(gReadBuffer, "glReadBuffer") L(gDrawBuffer, "glDrawBuffer")
    L(gGetFramebufferAttachmentParameteriv, "glGetFramebufferAttachmentParameteriv") L(gFlush, "glFlush")
#ifdef _WIN32
    xSwap = w_swap; xDpy = w_dpy; xDrw = w_drw;
#else
    L(xSwap, "glXSwapBuffers") L(xDpy, "glXGetCurrentDisplay") L(xDrw, "glXGetCurrentDrawable")
#endif
#undef L
    GLuint v = shader(0x8B31, vs_src), f = shader(0x8B30, fs_src);
    prog = gCreateProgram(); gAttachShader(prog, v); gAttachShader(prog, f); gLinkProgram(prog);
    GLint ok = 0; gGetProgramiv(prog, 0x8B82, &ok);
    if (!ok) { char log[2048]; int n = 0; gGetProgramInfoLog(prog, sizeof log, &n, log); fprintf(stderr, "[ravenfg] link failed: %s\n", log); return 0; }
    u_A = gGetUniformLocation(prog, "A"); u_B = gGetUniformLocation(prog, "B"); u_F = gGetUniformLocation(prog, "F");
    u_V = gGetUniformLocation(prog, "V"); u_D = gGetUniformLocation(prog, "D"); u_t = gGetUniformLocation(prog, "t");
    u_kb = gGetUniformLocation(prog, "kb"); u_hasA = gGetUniformLocation(prog, "hasA"); u_hasV = gGetUniformLocation(prog, "hasV");
    u_dbg = gGetUniformLocation(prog, "dbg"); u_vres = gGetUniformLocation(prog, "vres"); u_hud = gGetUniformLocation(prog, "hud"); u_vs = gGetUniformLocation(prog, "vs");
    GLint ov; gGetIntegerv(0x85B5, &ov); gGenVertexArrays(1, &vao); gBindVertexArray(ov);
    GLint rf, df; gGetIntegerv(0x8CAA, &rf); gGetIntegerv(0x8CA6, &df);
    gGenFramebuffers(1, &fbo_rd); gGenFramebuffers(1, &fbo_wr);
    gBindFramebuffer(0x8CA8, rf); gBindFramebuffer(0x8CA9, df);
    query_refresh(); if (hw_period > 0) period = hw_period;
    fprintf(stderr, "[ravenfg] ready\n");
    return 1;
}

// ---- settings (re-read every 30 frames, so overlay changes are live) ----
static void read_settings(void)
{
    const char *f = ravengfx_get("FRAME_GEN"), *m = ravengfx_get("FG_MODE"), *d = ravengfx_get("FG_DEBUG");
    if (getenv("RAVENFG")) f = getenv("RAVENFG");
    int n = f ? atoi(f) : 1; if (n < 1 || n > 4) n = 1;
    if (n != n_gen) { have_prev = 0; fprintf(stderr, "[ravenfg] frame generation %s\n", n > 1 ? f : "off"); }
    n_gen = n;
    mode_extra = m && !strncasecmp(m, "extra", 5);
    dbg = d ? atoi(d) : 0;
    { const char *nn = ravengfx_get("NEURAL"), *ns = ravengfx_get("NEURAL_STRENGTH"); if (getenv("RAVENNN")) nn = getenv("RAVENNN");
      nn_on = nn && atoi(nn) > 0; nn_str = ns ? atof(ns) : 1.0f;
      const char *nst = ravengfx_get("NEURAL_STABLE"); nn_stable_set = nst ? atof(nst) : 0.6f;
      const char *npr = ravengfx_get("NEURAL_PROTECT"); nn_protect = npr && *npr ? atof(npr) : -1.0f; }   // -1 = what the network was trained with (NN_PROT) eval_mode = getenv("RAVENFG_EVAL") != NULL; force_all = getenv("RAVENFG_FORCE") != NULL;
}
int ravenfg_wanted(void) { read_settings(); return 1; }   // wrappers are always useful (live toggle)

// ---- per-draw hook (ravengltrace.c) ----
struct fb_ent { GLuint fbo; GLuint vel, dep; int w, h; };
static struct fb_ent fbc[64]; static int nfbc;
static GLuint last_fbo = ~0u;

static void check_gbuffer(GLuint fbo)
{
    int i; for (i = 0; i < nfbc && fbc[i].fbo != fbo; i++);
    if (i == nfbc) {
        struct fb_ent e = {fbo, 0, 0, 0, 0};
        GLint prevfb; gGetIntegerv(0x8CA6, &prevfb);   // fbo is the bound draw framebuffer already
        for (int a = 0; a < 8 && !e.vel; a++) {
            GLint type = 0, name = 0; gGetFramebufferAttachmentParameteriv(0x8CA9, 0x8CE0 + a, 0x8CD0, &type);
            if (type != 0x1702) continue;
            gGetFramebufferAttachmentParameteriv(0x8CA9, 0x8CE0 + a, 0x8CD1, &name);
            GLint ot; gGetIntegerv(0x8069, &ot); gBindTexture(0x0DE1, name);
            GLint fmt = 0, w = 0, h = 0; gGetTexLevelParameteriv(0x0DE1, 0, 0x1003, &fmt);
            gGetTexLevelParameteriv(0x0DE1, 0, 0x1000, &w); gGetTexLevelParameteriv(0x0DE1, 0, 0x1001, &h);
            gBindTexture(0x0DE1, ot);
            if (fmt == 0x822F && a >= 3) { e.vel = name; e.w = w; e.h = h; }   // RG16F velocity
        }
        if (e.vel) {
            GLint type = 0, name = 0;
            gGetFramebufferAttachmentParameteriv(0x8CA9, 0x821A, 0x8CD0, &type);   // depth-stencil
            if (type != 0x1702) gGetFramebufferAttachmentParameteriv(0x8CA9, 0x8D00, 0x8CD0, &type);
            if (type == 0x1702) { gGetFramebufferAttachmentParameteriv(0x8CA9, 0x8D00, 0x8CD1, &name); e.dep = name; }
            if (!e.dep) e.vel = 0;
            fprintf(stderr, "[ravenfg] G-buffer fbo %u: velocity tex %u %dx%d, depth tex %u\n", fbo, e.vel, e.w, e.h, e.dep);
        }
        (void)prevfb;
        if (nfbc < 64) fbc[nfbc++] = e; else return;
    }
    if (fbc[i].vel) { vel_tex = fbc[i].vel; dep_tex = fbc[i].dep; vel_w = fbc[i].w; vel_h = fbc[i].h; have_vel = 1; }
}

static void ensure_tex(GLuint *t, int w, int h)
{
    if (*t) return;
    GLint ot; gGetIntegerv(0x8069, &ot);
    gGenTextures(1, t); gBindTexture(0x0DE1, *t);
    gTexImage2D(0x0DE1, 0, 0x8058, w, h, 0, 0x1908, 0x1401, NULL);   // RGBA8
    gTexParameteri(0x0DE1, 0x2801, 0x2601); gTexParameteri(0x0DE1, 0x2800, 0x2601);
    gTexParameteri(0x0DE1, 0x2802, 0x812F); gTexParameteri(0x0DE1, 0x2803, 0x812F);
    gTexParameteri(0x0DE1, 0x813D, 0);
    gBindTexture(0x0DE1, ot);
}
static void resize(int w, int h)
{
    if (w == tw && h == th) return;
    GLuint all[4] = {sc[0], sc[1], sc[2], fin}; gDeleteTextures(4, all); sc[0] = sc[1] = sc[2] = fin = 0;
    ensure_tex(&sc[0], w, h); ensure_tex(&sc[1], w, h); ensure_tex(&sc[2], w, h); ensure_tex(&fin, w, h);
    tw = w; th = h; have_prev = have_old = 0;
}

// copy the window's back buffer into texture t (keeps the game's bindings)
static void copy_window_sz(GLuint t, int w, int h);
static void copy_window(GLuint t) { copy_window_sz(t, tw, th); }
static void copy_window_sz(GLuint t, int w, int h)
{
    GLint rf, df, sci; gGetIntegerv(0x8CAA, &rf); gGetIntegerv(0x8CA6, &df); sci = gIsEnabled(0x0C11);
    GLint rb; gBindFramebuffer(0x8CA8, 0); gGetIntegerv(0x0C02, &rb);
    if (sci) gDisable(0x0C11);
    gBindFramebuffer(0x8CA9, fbo_wr); gFramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, t, 0);
    gReadBuffer(0x0405);   // GL_BACK
    gBlitFramebuffer(0, 0, w, h, 0, 0, w, h, 0x4000, 0x2600);
    gReadBuffer(rb);
    if (sci) gEnable(0x0C11);
    gBindFramebuffer(0x8CA8, rf); gBindFramebuffer(0x8CA9, df);
}

static void nn_apply(int w, int h);
// Bloom fix for supersampling: the engine's blur/downsample program (uKernel + uOffsets) shrinks the
// scene to its first bloom buffer in one step. Above ~2x (e.g. 7680 -> 2048 at 2x supersampled 4K)
// it skips most pixels, and dark menus came out white. Feed it a copy averaged down in 2x steps.
GLuint ravengl_unit_tex(int unit);
static struct { GLuint p; int blur; } bpc[256]; static int nbpc;
static GLuint bl_tex[4], bl_fb[2]; static int bl_w[4], bl_h[4];
static int is_blur(GLuint p)
{
    for (int i = 0; i < nbpc; i++) if (bpc[i].p == p) return bpc[i].blur;
    int k = gGetUniformLocation(p, "uKernel[0]") >= 0 || gGetUniformLocation(p, "uKernel") >= 0, o = gGetUniformLocation(p, "uOffsets[0]") >= 0 || gGetUniformLocation(p, "uOffsets") >= 0;
    if (nbpc < 256) { bpc[nbpc].p = p; bpc[nbpc].blur = k && o; nbpc++; }
    return k && o;
}
static void bloom_fix(GLuint prog_)
{
    static int on = -1; if (on < 0) on = getenv("RAVENGFX_BLOOMFIX") != NULL;   // opt-in: did not fix the white menus
    if (!on || !prog_ || !is_blur(prog_)) return;
    GLuint src = ravengl_unit_tex(0); if (!src) return;
    GLint vp[4]; gGetIntegerv(0x0BA2, vp);
    GLint ot, au; gGetIntegerv(0x84E0, &au); gActiveTexture(0x84C0); gGetIntegerv(0x8069, &ot);
    if ((GLuint)ot != src) { gActiveTexture(au); return; }
    GLint sw = 0, sh = 0, fmt = 0; gGetTexLevelParameteriv(0x0DE1, 0, 0x1000, &sw); gGetTexLevelParameteriv(0x0DE1, 0, 0x1001, &sh); gGetTexLevelParameteriv(0x0DE1, 0, 0x1003, &fmt);
    if (vp[2] <= 0 || sw <= vp[2] * 5 / 2) { gActiveTexture(au); return; }
    static int logged; if (!logged++) fprintf(stderr, "[ravengfx] bloom downsample fix: %dx%d -> %dx%d\n", sw, sh, vp[2], vp[3]);
    GLint rf, df, sci = gIsEnabled(0x0C11); gGetIntegerv(0x8CAA, &rf); gGetIntegerv(0x8CA6, &df);
    if (sci) gDisable(0x0C11);
    if (!bl_fb[0]) gGenFramebuffers(2, bl_fb);
    GLuint cur_t = src; int cw = sw, ch = sh, i = 0;
    while (cw > vp[2] * 2 && i < 4) {
        int nw = (cw + 1) / 2, nh = (ch + 1) / 2;
        if (!bl_tex[i] || bl_w[i] != nw || bl_h[i] != nh) {
            if (bl_tex[i]) gDeleteTextures(1, &bl_tex[i]);
            gGenTextures(1, &bl_tex[i]); gBindTexture(0x0DE1, bl_tex[i]);
            gTexImage2D(0x0DE1, 0, 0x881A, nw, nh, 0, 0x1908, 0x140B, NULL);
            gTexParameteri(0x0DE1, 0x2801, 0x2601); gTexParameteri(0x0DE1, 0x2800, 0x2601); gTexParameteri(0x0DE1, 0x813D, 0);
            gTexParameteri(0x0DE1, 0x2802, 0x812F); gTexParameteri(0x0DE1, 0x2803, 0x812F);
            bl_w[i] = nw; bl_h[i] = nh;
        }
        gBindFramebuffer(0x8CA8, bl_fb[0]); gFramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1, cur_t, 0);
        gBindFramebuffer(0x8CA9, bl_fb[1]); gFramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, bl_tex[i], 0);
        gBlitFramebuffer(0, 0, cw, ch, 0, 0, nw, nh, 0x4000, 0x2601);   // 2x linear = 2x2 average
        cur_t = bl_tex[i]; cw = nw; ch = nh; i++;
    }
    gBindFramebuffer(0x8CA8, bl_fb[0]); gFramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1, 0, 0);
    gBindFramebuffer(0x8CA8, rf); gBindFramebuffer(0x8CA9, df);
    if (sci) gEnable(0x0C11);
    gBindTexture(0x0DE1, cur_t);   // the blur draws from the averaged copy (same UVs)
    gActiveTexture(au);
    (void)fmt;
}
void ravenfg_draw(GLuint prog_, GLuint fbo)
{
    if (gl_ok < 0 && !(gl_ok = init_gl())) return;
    if (fbo) bloom_fix(prog_);
    if ((n_gen <= 1 && !eval_mode && !nn_on) || gl_ok == 0) return;
    if (fbo != last_fbo) { last_fbo = fbo; if (fbo) check_gbuffer(fbo); }
    if (fbo || captured) return;
    // drawing to the window: the first program is the final post pass (tonemap / SMAA); the first
    // draw with a different program starts the HUD. Copy the scene right before it.
    if (!win_draws++) { win_prog = prog_; return; }
    if (prog_ != win_prog) {
        int w = ravengfx_win_w, h = ravengfx_win_h; if (w <= 0) return;
        if (nn_on) { nn_apply(w, h); nn_ran = 1; }
        captured = 1;
        if (n_gen <= 1 && !eval_mode) return;
        resize(w, h);
        cur = (cur + 1) % 3; copy_window(sc[cur]); have_scene = 1; scene_frame = (int)frames;
    }
}

// ---- generated frames ----
struct st { GLint prog, vao, at, tex[8], smp[8], rf, df, vp[4], rb, db; unsigned char cm[4], dm, en[8]; };
static const GLenum caps[8] = {0x0BE2, 0x0B71, 0x0B44, 0x0C11, 0x0B90, 0x8DB9, 0x809D, 0x8642};
static void save(struct st *s)
{
    gGetIntegerv(0x8B8D, &s->prog); gGetIntegerv(0x85B5, &s->vao); gGetIntegerv(0x84E0, &s->at);
    for (int i = 0; i < 8; i++) { gActiveTexture(0x84C0 + i); gGetIntegerv(0x8069, &s->tex[i]); gGetIntegerv(0x8919, &s->smp[i]); }
    gGetIntegerv(0x8CAA, &s->rf); gGetIntegerv(0x8CA6, &s->df); gGetIntegerv(0x0BA2, s->vp);
    gGetBooleanv(0x0C23, s->cm); gGetBooleanv(0x0B72, &s->dm);
    for (int i = 0; i < 8; i++) s->en[i] = gIsEnabled(caps[i]);
}
static void restore(const struct st *s)
{
    for (int i = 0; i < 8; i++) { gActiveTexture(0x84C0 + i); gBindTexture(0x0DE1, s->tex[i]); gBindSampler(i, s->smp[i]); }
    gActiveTexture(s->at); gBindVertexArray(s->vao); gUseProgram(s->prog);
    gBindFramebuffer(0x8CA8, s->rf); gBindFramebuffer(0x8CA9, s->df);
    gViewport(s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    gColorMask(s->cm[0], s->cm[1], s->cm[2], s->cm[3]); gDepthMask(s->dm);
    for (int i = 0; i < 8; i++) (s->en[i] ? gEnable : gDisable)(caps[i]);
}

// engine textures: one level but MAX_LEVEL 1000 (incomplete), and depth may be in stencil/compare mode
static GLint sv_max[2], sv_dsm, sv_cmp;
static void engine_tex_begin(void)
{
    gBindTexture(0x0DE1, vel_tex); gGetTexParameteriv(0x0DE1, 0x813D, &sv_max[0]); gTexParameteri(0x0DE1, 0x813D, 0);
    gBindTexture(0x0DE1, dep_tex); gGetTexParameteriv(0x0DE1, 0x813D, &sv_max[1]); gTexParameteri(0x0DE1, 0x813D, 0);
    gGetTexParameteriv(0x0DE1, 0x90EA, &sv_dsm); gGetTexParameteriv(0x0DE1, 0x884C, &sv_cmp);
    gTexParameteri(0x0DE1, 0x90EA, 0x1902); gTexParameteri(0x0DE1, 0x884C, 0);
}
static void engine_tex_end(void)
{
    gBindTexture(0x0DE1, vel_tex); gTexParameteri(0x0DE1, 0x813D, sv_max[0]);
    gBindTexture(0x0DE1, dep_tex); gTexParameteri(0x0DE1, 0x813D, sv_max[1]);
    gTexParameteri(0x0DE1, 0x90EA, sv_dsm); gTexParameteri(0x0DE1, 0x884C, sv_cmp);
}

static void draw_generated_ab(GLuint a, float t, int use_vel);
static void draw_generated(float t, int use_vel) { draw_generated_ab(sc[PREV], t, use_vel); }
static void draw_generated_ab(GLuint a, float t, int use_vel)
{
    gBindFramebuffer(0x8CA9, 0); gDrawBuffer(0x0405);
    gViewport(0, 0, tw, th);
    for (int i = 0; i < 8; i++) gDisable(caps[i]);
    gColorMask(1, 1, 1, 1); gDepthMask(0);
    gUseProgram(prog); gBindVertexArray(vao);
    GLuint tex[5] = {a, sc[cur], fin, use_vel ? vel_tex : 0, use_vel ? dep_tex : 0};
    for (int i = 0; i < 5; i++) { gActiveTexture(0x84C0 + i); gBindSampler(i, 0); gBindTexture(0x0DE1, tex[i]); }
    gUniform1i(u_A, 0); gUniform1i(u_B, 1); gUniform1i(u_F, 2); gUniform1i(u_V, 3); gUniform1i(u_D, 4);
    gUniform1f(u_t, t); gUniform1f(u_kb, 1 - t); gUniform1f(u_vs, vscale);
    gUniform1i(u_hasA, have_prev && !mode_extra); gUniform1i(u_hasV, use_vel); gUniform1i(u_dbg, dbg);
    gUniform1i(u_hud, have_scene); gUniform2f(u_vres, (float)vel_w, (float)vel_h);
    gDrawArrays(0x0004, 0, 3);
}

static void blit_to_window(GLuint t)
{
    gBindFramebuffer(0x8CA8, fbo_rd); gFramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1, t, 0);
    gBindFramebuffer(0x8CA9, 0); gDrawBuffer(0x0405);
    gBlitFramebuffer(0, 0, tw, th, 0, 0, tw, th, 0x4000, 0x2600);
}

static double slept;   // time spent in pace() this frame: not part of the game's frame time
static void pace(double target)
{
    double t = now_s();
#ifdef _WIN32
    // Sleep() is ~1 ms at best (timeBeginPeriod(1)): sleep until ~1.5 ms before, then spin.
    static int tp; if (!tp++) timeBeginPeriod(1);
    if (target > t + 0.0005) { double r = target - t - 0.0015; if (r > 0.001) Sleep((DWORD)(r * 1000)); while (now_s() < target - 0.0002) YieldProcessor(); slept += now_s() - t; }
    return;
#endif
    if (target > t + 0.0005) { struct timespec ts = {0, (long)((target - t - 0.0003) * 1e9)}; nanosleep(&ts, NULL); slept += now_s() - t; }
}

#include "ravennn.inc"

// RAVENFG_SHOT=1: capture generated frames next to the real-frame shots (one set per shot time)
static int fg_shot_pending; static char fg_shot_base[480];
void ravenfg_request_shot(const char *base) {
    if (nn_on && nn_in && getenv("RAVENNN_DUMP") && nn_ran) {   // only when the pass ran this frame   // save the network's input (frame shown is then replaced)
        char p[512]; snprintf(p, sizeof p, "%s_nnin.ppm", base);
        GLint rf, df; gGetIntegerv(0x8CAA, &rf); gGetIntegerv(0x8CA6, &df);
        gBindFramebuffer(0x8CA8, fbo_rd); gFramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1, nn_pin, 0);
        gBindFramebuffer(0x8CA9, 0); gBlitFramebuffer(0, 0, nn_w, nn_h_, 0, 0, nn_w, nn_h_, 0x4000, 0x2600);
        ravengfx_capture(p); gBindFramebuffer(0x8CA8, rf); gBindFramebuffer(0x8CA9, df);
    } if (n_gen > 1 || eval_mode) { eval_pending = eval_mode; snprintf(fg_shot_base, sizeof fg_shot_base, "%s", base); fg_shot_pending = 1; } }

// RAVENGFX_TEXSTAT=s1,s2,..: at those seconds, log mean/max of the engine's textures 1..60 and the
// float uniforms of the tonemap program (uExposure*), for debugging blown-out frames.
static void texstat(void)
{
    static double ts[16]; static int nts = -1, ti; static double t0;
    if (nts < 0) { nts = 0; const char *e = getenv("RAVENGFX_TEXSTAT"); for (char *x; e && *e && nts < 16; e = *x ? x + 1 : x) ts[nts++] = strtod(e, &x); t0 = now_s(); }
    if (ti >= nts || now_s() - t0 < ts[ti]) return;
    ti++;
    void (*gti)(GLuint, GLint, GLenum, GLenum, int, void *) = gpa("glGetTextureImage");
    void (*gtl)(GLuint, GLint, GLenum, GLint *) = gpa("glGetTextureLevelParameteriv");
    unsigned char (*ist)(GLuint) = gpa("glIsTexture");
    for (GLuint t = 1; t <= 60; t++) {
        if (!ist(t)) continue; GLint w = 0, h = 0, f = 0; gtl(t, 0, 0x1000, &w); gtl(t, 0, 0x1001, &h); gtl(t, 0, 0x1003, &f);
        if (w < 64 || h < 64 || f == 0x88F0 || f == 0x81A6 || f == 0x8CAC || f == 0x81A5) continue;
        size_t n = (size_t)w * h * 4; float *b = malloc(n * 4); if (!b) continue;
        gti(t, 0, 0x1908, 0x1406, (int)(n * 4), b);
        double m = 0, mx = 0; size_t nn = 0; for (size_t i = 0; i < n; i += 4 * 7) { double v = (b[i] + b[i + 1] + b[i + 2]) / 3; if (v != v) continue; m += v; nn++; if (v > mx) mx = v; }
        fprintf(stderr, "[texstat %.0fs] tex %u %dx%d fmt 0x%x mean %.4f max %.3f\n", ts[ti - 1], t, w, h, f, nn ? m / nn : 0, mx);
        free(b);
    }
    void (*gu)(GLuint, GLint, float *) = gpa("glGetUniformfv");
    for (GLuint p = 1; p < 600; p++) {
        if (gGetUniformLocation(p, "uExposure") < 0) continue;
        static const char *nm[] = {"uExposure", "uExposureBias", "uBrightness", "uContrast", "uGamma", "uInBlack", "uOutRange", "uW"};
        fprintf(stderr, "[texstat] prog %u:", p);
        for (int i = 0; i < 8; i++) { GLint l = gGetUniformLocation(p, nm[i]); float v[4] = {0}; if (l >= 0) { gu(p, l, v); fprintf(stderr, " %s=%.4g", nm[i], v[0]); } }
        fprintf(stderr, "\n");
    }
}
// How many frames to show per real frame without slowing the game: with VSync each shown frame takes
// a refresh slot, so N frames only fit when N * real FPS <= refresh (45 FPS at 60 Hz -> 1, not 2:
// 2x would hold the game at 30). With VSync off presents don't wait, so the full factor is used.
static int fg_fit(double hz, double rf)
{
    const char *v = ravengfx_get("VSYNC");
    if (v && !strcmp(v, "0")) return n_gen;
    return (int)floor(hz / rf + 0.05);
}
// Called from my_present instead of the game's present. Returns after the real present.
void ravenfg_present(void (*real_present)(void *), void *ctx)
{
    if (gl_ok < 0) gl_ok = init_gl();
    if (gl_ok > 0) texstat();
    double t0 = now_s();
    if (last_real_t > 0) { double dt = t0 - last_real_t - slept; if (dt > 0.002 && dt < 0.2) real_dt += (dt - real_dt) * 0.1; }
    slept = 0;
    last_real_t = t0;
    if (frames++ % 30 == 0) read_settings();
    int ok = (n_gen > 1 || eval_mode) && gl_ok != 0;
    if (ok && gl_ok < 0) ok = (gl_ok = init_gl());
    int w = ravengfx_win_w, h = ravengfx_win_h;
    if (ok && w > 0) {
        resize(w, h);
        struct st s; save(&s);
        if (gIsEnabled(0x8DB9)) gDisable(0x8DB9);   // FRAMEBUFFER_SRGB off for copies
        if (!captured) { cur = (cur + 1) % 3; copy_window(sc[cur]); have_scene = 0; }   // no HUD split this frame
        copy_window(fin);
        if (eval_pending && have_old && fg_shot_pending) {
            // RAVENFG_EVAL=1: rebuild the previous real frame from the one before it and this one
            // (t = 0.5, motion doubled) and save it next to the real one, our method and a plain blend.
            int uv = have_vel && vel_tex && dep_tex && vel_w > 0; char p[512];
            if (uv) engine_tex_begin();
            int hs = have_scene; have_scene = 0; int hp = have_prev, me = mode_extra; have_prev = 1; mode_extra = 0;
            blit_to_window(sc[PREV]); snprintf(p, sizeof p, "%s_ev_ref.ppm", fg_shot_base); ravengfx_capture(p);
            vscale = 2; draw_generated_ab(sc[OLD], 0.5f, uv); snprintf(p, sizeof p, "%s_ev_mv.ppm", fg_shot_base); ravengfx_capture(p);
            draw_generated_ab(sc[OLD], 0.5f, 0); snprintf(p, sizeof p, "%s_ev_blend.ppm", fg_shot_base); ravengfx_capture(p);
            blit_to_window(sc[cur]); snprintf(p, sizeof p, "%s_ev_cur.ppm", fg_shot_base); ravengfx_capture(p);
            vscale = 1; have_scene = hs; have_prev = hp; mode_extra = me;
            if (uv) engine_tex_end();
            eval_pending = 0; if (n_gen <= 1) fg_shot_pending = 0;
        }
        int use_vel = have_vel && vel_tex && dep_tex && vel_w > 0;
        int interp = !mode_extra;
        if (interp && !have_prev) { have_prev = 1; restore(&s); goto real; }   // need two frames first
        void *dpy = xDpy(); unsigned long drw = xDrw();
        {   // pick how many frames to show per real frame from the game's own frame time (time from our
            // present returning to the next present call: no VSync waits), so FG never holds the game back.
            // A new factor must hold for 2 seconds in a row (a loading hitch does not switch it on).
            static double ws, wsum; static int wn, want = 1, want_n;
            if (!ws) ws = t0;
            if (last_real_ret > 0 && t0 - last_real_ret < 0.5) { wsum += t0 - last_real_ret; wn++; }
            if (t0 - ws >= 1.0 && wn > 0) {
                double cf = wn / wsum, hz = 1.0 / period;   // FPS the game could reach on its own
                int nu = force_all ? n_gen : cf < 0.9 * hz ? fg_fit(hz, cf) : 1;
                if (nu > n_gen) nu = n_gen; if (nu < 1) nu = 1;
                if (nu == want) want_n++; else { want = nu; want_n = 1; }
                if (want != n_use && (want_n >= 2 || force_all)) {
                    fprintf(stderr, "[ravenfg] now %dx (game could do %.1f FPS, screen %.0f Hz)\n", want, cf, hz);
                    n_use = want;
                }
                ws = t0; wn = 0; wsum = 0;
            }
        }
        double step = real_dt / n_use;
        if (use_vel) engine_tex_begin();
        for (int k = 1; k < n_use; k++) {
            float t = interp ? (float)k / n_use : 1.0f + (float)k / n_use;
            if (!interp) {   // extrapolate: show the real frame first, then the predicted ones
                if (k == 1) { if (use_vel) engine_tex_end(); restore(&s); real_present(ctx); swap_done(); last_real_ret = now_s(); save(&s);
                              if (gIsEnabled(0x8DB9)) gDisable(0x8DB9); if (use_vel) engine_tex_begin(); }
            }
            draw_generated(t, use_vel);
            if (fg_shot_pending) { char p[512]; snprintf(p, sizeof p, "%s_gen%d.ppm", fg_shot_base, k); ravengfx_capture(p); }
            pace(last_swap_t + step);
            xSwap(dpy, drw); swap_done();
        }
        if (use_vel) engine_tex_end();
        if (interp) blit_to_window(fin);
        restore(&s);
        if (fg_shot_pending) { fg_shot_pending = 0; }
        have_old = have_prev; have_prev = 1;
        captured = 0; win_draws = 0; have_vel = 0; last_fbo = ~0u; nn_ran = 0;
        if (frames % 600 == 0) { nfbc = 0; }
        if (!interp) return;
        if (n_use > 1) pace(last_swap_t + step);
        real_present(ctx); swap_done(); last_real_ret = now_s();
        return;
    }
    captured = 0; win_draws = 0; have_vel = 0; last_fbo = ~0u; nn_ran = 0;
real:
    captured = 0; win_draws = 0; have_vel = 0; last_fbo = ~0u; nn_ran = 0;
    real_present(ctx); swap_done(); last_real_ret = now_s();
}

int ravenfg_factor(void) { return n_use; }
