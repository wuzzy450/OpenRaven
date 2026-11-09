// ravenfsr.cpp - AMD FSR 2.2 temporal upscaling for the engine (OpenGL 4.6 compute, any GPU vendor).
//
// graphics.ini: UPSCALER = off | fsr2, FSR_QUALITY = native | quality | balanced | performance | ultra,
// FSR_SHARPNESS = 0..1. With fsr2 the engine renders at 1/ratio of the screen with its own TAA jitter
// and velocity buffer (ENABLE_TAA forced on). We skip the engine's TAA draw (program with
// uHistoryTexture + uVelocityMap), run FSR 2 on its inputs (colour unit 0, velocity unit 2, depth
// unit 3) into a screen-size texture, and hand that to the tonemap program (uExposureBias) in place of
// the TAA result. The HUD is drawn after tonemap, so it stays sharp and unaffected.
// GL calls are intercepted in ravengltrace.c, which calls the ravenfsr_* hooks below.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <strings.h>
#include <unordered_map>
#include "fsr2/src/ffx_fsr2.h"
#include "fsr2/src/gl/ffx_fsr2_gl.h"

extern "C" {
const char *ravengfx_get(const char *key);
void ravengfx_set(const char *key, const char *val);
extern int ravengfx_win_w, ravengfx_win_h;
unsigned ravengl_unit_tex(int unit);
}

typedef unsigned GLuint; typedef int GLint; typedef unsigned GLenum;
#ifdef _WIN32
// Windows loader (openraven/win): GL comes from gl_resolve (real Windows functions), settings from
// the Win32 environment, messages go to ravenloader.log. Built by win/Makefile.
extern "C" { void *gl_resolve(const char *name); void raven_log(const char *fmt, ...); const char *raven_env(const char *name); }
static void *gpa(const char *n) { return gl_resolve(n); }
#define getenv raven_env
#define fprintf(f, ...) raven_log(__VA_ARGS__)
#define clock_gettime(clk, ts) timespec_get(ts, TIME_UTC)  // frame time delta only
#else
#include <dlfcn.h>
static void *gpa(const char *n) {
    static void *(*g)(const char *);
    if (!g) g = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    void *f = g ? g(n) : nullptr;
    return f ? f : dlsym(RTLD_DEFAULT, n);
}
#endif
static ffx_glProc ffx_gpa(const char *n) { return (ffx_glProc)gpa(n); }

#define F(ret, name, args) static ret (*name) args;
F(void, gGetProgramiv, (GLuint, GLenum, GLint *))
F(void, gGetActiveUniform, (GLuint, GLuint, int, int *, GLint *, GLenum *, char *))
F(GLint, gGetUniformLocation, (GLuint, const char *))
F(void, gGetTextureLevelParameteriv, (GLuint, GLint, GLenum, GLint *))
F(void, gCreateTextures, (GLenum, int, GLuint *))
F(void, gTextureStorage2D, (GLuint, int, GLenum, int, int))
F(void, gDeleteTextures, (int, const GLuint *))
F(void, gGetIntegerv, (GLenum, GLint *))
F(void, gGetIntegeri_v, (GLenum, GLuint, GLint *))
F(void, gUseProgram, (GLuint))
F(void, gActiveTexture, (GLenum))
F(void, gBindTexture, (GLenum, GLuint))
F(void, gBindSampler, (GLuint, GLuint))
F(void, gBindBufferBase, (GLenum, GLuint, GLuint))
F(void, gBindBufferRange, (GLenum, GLuint, GLuint, long long, long long))
F(void, gBindBuffer, (GLenum, GLuint))
F(void, gMemoryBarrier, (unsigned))
F(void, gGetIntegeri_v64, (GLenum, GLuint, long long *))

enum { K_OTHER = 1, K_TAA, K_TONE, K_GBUF };
struct ProgInfo { int kind; GLint loc_mvp = -1, loc_nj = -1, loc_invp = -1; };
static std::unordered_map<GLuint, ProgInfo> progs;
static ProgInfo *cur;
static GLuint cur_prog_id;

static int want = -1;                 // UPSCALER=fsr2 in graphics.ini
static bool gl_ok;
static FfxFsr2Context ctx; static bool ctx_ok; static void *scratch;
static int ctx_rw, ctx_rh, ctx_dw, ctx_dh, ctx_flags;
static GLuint out_tex; static int out_w, out_h;
static GLuint taa_out_tex;            // texture the engine's TAA would have written (FBO c0), replaced in tonemap
static bool dispatched;               // this frame
static float mvp[16], nj[16]; static bool have_mvp, have_nj, have_jit;
static float jit_ndc[2];
static float invp[16]; static bool have_invp;
static double last_t;
static long nframes;

extern "C" int ravenfsr_wanted(void)
{
    if (want < 0) { const char *u = ravengfx_get("UPSCALER"); want = u && !strcasecmp(u, "fsr2"); }
    return want;
}

// Called once at startup (from ravengfx.c, after graphics.ini is loaded): force the engine settings
// FSR 2 needs, and pick the render scale from the quality mode.
extern "C" void ravenfsr_config(void)
{
    if (!ravenfsr_wanted()) return;
    const char *q = ravengfx_get("FSR_QUALITY"); if (!q) q = "quality";
    float ratio = !strcasecmp(q, "native") ? 1.0f : !strcasecmp(q, "balanced") ? 1.7f :
                  !strcasecmp(q, "performance") ? 2.0f : !strcasecmp(q, "ultra") ? 3.0f : 1.5f;
    char buf[32]; snprintf(buf, sizeof buf, "%.4f", 1.0f / ratio);
    ravengfx_set("RENDER_SCALE", buf); ravengfx_set("RENDER_SIZE", "");
    ravengfx_set("ENABLE_TAA", "1"); ravengfx_set("ENABLE_SMAA", "0"); ravengfx_set("ENABLE_FXAA", "0");
    fprintf(stderr, "[ravenfsr] FSR 2 %s (render scale %s)\n", q, buf);
}

static bool init_gl(void)
{
    if (gl_ok) return true;
#define L(v, n) v = (decltype(v))gpa(n); if (!v) { fprintf(stderr, "[ravenfsr] missing %s, FSR 2 off\n", n); want = 0; return false; }
    L(gGetProgramiv, "glGetProgramiv") L(gGetActiveUniform, "glGetActiveUniform") L(gGetUniformLocation, "glGetUniformLocation")
    L(gGetTextureLevelParameteriv, "glGetTextureLevelParameteriv") L(gCreateTextures, "glCreateTextures")
    L(gTextureStorage2D, "glTextureStorage2D") L(gDeleteTextures, "glDeleteTextures") L(gGetIntegerv, "glGetIntegerv")
    L(gGetIntegeri_v, "glGetIntegeri_v") L(gUseProgram, "glUseProgram") L(gActiveTexture, "glActiveTexture")
    L(gBindTexture, "glBindTexture") L(gBindSampler, "glBindSampler") L(gBindBufferBase, "glBindBufferBase")
    L(gBindBufferRange, "glBindBufferRange") L(gBindBuffer, "glBindBuffer") L(gMemoryBarrier, "glMemoryBarrier")
    L(gGetIntegeri_v64, "glGetInteger64i_v")
    if (!gpa("glSpecializeShader")) { fprintf(stderr, "[ravenfsr] no GL 4.6 SPIR-V support, FSR 2 off\n"); want = 0; return false; }
    gl_ok = true; return true;
}

static ProgInfo classify(GLuint p)
{
    ProgInfo pi; pi.kind = K_OTHER;
    GLint n = 0; gGetProgramiv(p, 0x8B86, &n);
    bool hist = false, vel = false, expb = false, nojit = false;
    for (GLint i = 0; i < n; i++) {
        char nm[128]; int l; GLint sz; GLenum ty; gGetActiveUniform(p, i, sizeof nm, &l, &sz, &ty, nm);
        if (!strcmp(nm, "uHistoryTexture")) hist = true;
        if (!strcmp(nm, "uVelocityMap")) vel = true;
        if (!strcmp(nm, "uExposureBias")) expb = true;
        if (!strcmp(nm, "uNoJitterModelViewProjectionMatrix")) nojit = true;
    }
    if (hist && vel) { pi.kind = K_TAA; pi.loc_invp = gGetUniformLocation(p, "uInvProjectionMatrix"); }
    else if (expb) pi.kind = K_TONE;
    else if (nojit) { pi.kind = K_GBUF; pi.loc_mvp = gGetUniformLocation(p, "uModelViewProjectionMatrix"); pi.loc_nj = gGetUniformLocation(p, "uNoJitterModelViewProjectionMatrix"); }
    return pi;
}

extern "C" void ravenfsr_use_program(GLuint p)
{
    cur_prog_id = p; cur = nullptr;
    if (!p || !init_gl()) return;
    auto it = progs.find(p);
    if (it == progs.end()) it = progs.emplace(p, classify(p)).first;
    cur = &it->second;
}

extern "C" void ravenfsr_matrix(GLint loc, const float *m)
{
    if (!cur) return;
    if (cur->kind == K_GBUF && !have_jit) {
        if (loc == cur->loc_mvp) { memcpy(mvp, m, 64); have_mvp = true; }
        else if (loc == cur->loc_nj) { memcpy(nj, m, 64); have_nj = true; }
        if (have_mvp && have_nj) {
            // jittered = T * unjittered with T adding j*w to x/y: row0' = row0 + jx*row3. Use the column
            // with the largest w term.
            int c = 0; for (int k = 1; k < 4; k++) if (fabsf(nj[k * 4 + 3]) > fabsf(nj[c * 4 + 3])) c = k;
            if (fabsf(nj[c * 4 + 3]) > 1e-6f) {
                jit_ndc[0] = (mvp[c * 4 + 0] - nj[c * 4 + 0]) / nj[c * 4 + 3];
                jit_ndc[1] = (mvp[c * 4 + 1] - nj[c * 4 + 1]) / nj[c * 4 + 3];
                have_jit = true;
            }
            have_mvp = have_nj = false;
        }
    } else if (cur->kind == K_TAA && loc == cur->loc_invp) { memcpy(invp, m, 64); have_invp = true; }
}

static int texfmt(GLuint t, int *w, int *h)
{
    GLint f = 0; gGetTextureLevelParameteriv(t, 0, 0x1000, w); gGetTextureLevelParameteriv(t, 0, 0x1001, h);
    gGetTextureLevelParameteriv(t, 0, 0x1003, &f); return f;
}

// 4x4 inverse (column-major), for the projection from uInvProjectionMatrix
static bool inv4(const float *m, float *o)
{
    float inv[16];
    inv[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    float det = m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
    if (fabsf(det) < 1e-20f) return false;
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return true;
}

static bool make_ctx(int rw, int rh, int dw, int dh, int flags)
{
    if (ctx_ok && rw == ctx_rw && rh == ctx_rh && dw == ctx_dw && dh == ctx_dh && flags == ctx_flags) return true;
    if (ctx_ok) { ffxFsr2ContextDestroy(&ctx); ctx_ok = false; }
    size_t ss = ffxFsr2GetScratchMemorySizeGL();
    free(scratch); scratch = calloc(1, ss);
    FfxFsr2ContextDescription d; memset(&d, 0, sizeof d);
    if (ffxFsr2GetInterfaceGL(&d.callbacks, scratch, ss, ffx_gpa) != FFX_OK) { fprintf(stderr, "[ravenfsr] interface failed\n"); want = 0; return false; }
    d.flags = flags | FFX_FSR2_ALLOW_NULL_DEVICE_AND_COMMAND_LIST; d.maxRenderSize = {(uint32_t)rw, (uint32_t)rh}; d.displaySize = {(uint32_t)dw, (uint32_t)dh};
    FfxErrorCode e = ffxFsr2ContextCreate(&ctx, &d);
    if (e != FFX_OK) { fprintf(stderr, "[ravenfsr] context create failed (%d), FSR 2 off\n", (int)e); want = 0; return false; }
    ctx_ok = true; ctx_rw = rw; ctx_rh = rh; ctx_dw = dw; ctx_dh = dh; ctx_flags = flags;
    if (out_tex && (out_w != dw || out_h != dh)) { gDeleteTextures(1, &out_tex); out_tex = 0; }
    if (!out_tex) { gCreateTextures(0x0DE1, 1, &out_tex); gTextureStorage2D(out_tex, 1, 0x881A, dw, dh); out_w = dw; out_h = dh; }
    fprintf(stderr, "[ravenfsr] context %dx%d -> %dx%d flags 0x%x\n", rw, rh, dw, dh, flags);
    return true;
}

// Save/restore the GL state the FSR backend touches.
struct Saved { GLint prog, active, tex[32], smp[32], ubo[24]; long long uoff[24], usz[24]; GLint ubo_generic; };
static void save(Saved &s)
{
    gGetIntegerv(0x8B8D, &s.prog); gGetIntegerv(0x84E0, &s.active);
    for (int i = 0; i < 32; i++) { gActiveTexture(0x84C0 + i); gGetIntegerv(0x8069, &s.tex[i]); gGetIntegerv(0x8919, &s.smp[i]); }
    for (int i = 0; i < 24; i++) { gGetIntegeri_v(0x8A28, i, &s.ubo[i]); gGetIntegeri_v64(0x8A29, i, &s.uoff[i]); gGetIntegeri_v64(0x8A2A, i, &s.usz[i]); }
    gGetIntegerv(0x8A28, &s.ubo_generic);
}
static void restore(const Saved &s)
{
    for (int i = 0; i < 32; i++) { gActiveTexture(0x84C0 + i); gBindTexture(0x0DE1, s.tex[i]); gBindSampler(i, s.smp[i]); }
    for (int i = 0; i < 24; i++) {
        if (s.usz[i] > 0) gBindBufferRange(0x8A11, i, s.ubo[i], s.uoff[i], s.usz[i]); else gBindBufferBase(0x8A11, i, s.ubo[i]);
    }
    gBindBuffer(0x8A11, s.ubo_generic);
    gActiveTexture(s.active); gUseProgram(s.prog);
}

extern "C++" { int ravenfsr_joblog; }
static int dbg_on;
static void dbg_cb(GLenum src, GLenum type, GLuint id, GLenum sev, int len, const char *msg, const void *)
{
    static int n; if (dbg_on && type != 0x8251 && n < 60) { n++; fprintf(stderr, "[ravenfsr] GL debug type 0x%x sev 0x%x: %s\n", type, sev, msg); }
}
static void dispatch(void)
{
    static bool dbg_init;
    if (!dbg_init) { dbg_init = true; void (*cb)(void *, const void *) = (void (*)(void *, const void *))gpa("glDebugMessageCallback");
        void (*en)(GLenum) = (void (*)(GLenum))gpa("glEnable"); if (cb && getenv("RAVENFSR_GLDEBUG")) { en(0x92E0); en(0x8242); cb((void *)dbg_cb, nullptr); } }
    dbg_on = 1; ravenfsr_joblog = nframes > 200;
    GLuint color = ravengl_unit_tex(0), vel = ravengl_unit_tex(2), depth = ravengl_unit_tex(3);
    int rw, rh, w2, h2, w3, h3;
    int fc = texfmt(color, &rw, &rh), fv = texfmt(vel, &w2, &h2), fd = texfmt(depth, &w3, &h3);
    int dw = ravengfx_win_w, dh = ravengfx_win_h;
    if (!color || !vel || !depth || rw <= 0 || dw <= 0 || w2 != rw || w3 != rw) return;
    bool hdr = fc == 0x881A || fc == 0x8C3A || fc == 0x881B;   // RGBA16F / R11G11B10F / RGB16F
    int flags = (hdr ? FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE : 0);
    if (getenv("RAVENFSR_FLAGS")) flags = strtol(getenv("RAVENFSR_FLAGS"), NULL, 0);
    Saved s; save(s);
    bool reset = !ctx_ok || rw != ctx_rw || dw != ctx_dw;
    if (!make_ctx(rw, rh, dw, dh, flags)) { restore(s); return; }
    float proj[16]; float n = 1.0f, f = 1000.0f, fov = 0.81f;
    if (have_invp && inv4(invp, proj) && fabsf(proj[10] - 1) > 1e-6f && fabsf(proj[10] + 1) > 1e-6f) {
        n = proj[14] / (proj[10] - 1); f = proj[14] / (proj[10] + 1); fov = 2 * atanf(1 / proj[5]);
        if (n < 0) n = -n; if (f < 0) f = -f;
    }
    static float jsx = getenv("RAVENFSR_JSX") ? atof(getenv("RAVENFSR_JSX")) : 1, jsy = getenv("RAVENFSR_JSY") ? atof(getenv("RAVENFSR_JSY")) : 1;
    static float msx = getenv("RAVENFSR_MSX") ? atof(getenv("RAVENFSR_MSX")) : -1, msy = getenv("RAVENFSR_MSY") ? atof(getenv("RAVENFSR_MSY")) : -1;
    FfxFsr2DispatchDescription d; memset(&d, 0, sizeof d);
    d.color = ffxGetTextureResourceGL(color, rw, rh, fc, L"FSR2_InputColor");
    d.depth = ffxGetTextureResourceGL(depth, rw, rh, fd, L"FSR2_InputDepth");
    d.motionVectors = ffxGetTextureResourceGL(vel, rw, rh, fv, L"FSR2_InputMotionVectors");
    d.output = ffxGetTextureResourceGL(out_tex, dw, dh, 0x881A, L"FSR2_Output");
    // engine jitter is in NDC; FSR wants pixels (x right, y as in the sample's convention, sign tunable)
    d.jitterOffset.x = have_jit ? jsx * jit_ndc[0] * rw * 0.5f : 0;
    d.jitterOffset.y = have_jit ? jsy * jit_ndc[1] * rh * 0.5f : 0;
    // engine velocity = uv_now - uv_prev (UV units); FSR: prev = now + mv * scale / renderSize
    d.motionVectorScale.x = msx * rw; d.motionVectorScale.y = msy * rh;
    d.renderSize = {(uint32_t)rw, (uint32_t)rh};
    const char *sh = ravengfx_get("FSR_SHARPNESS");
    d.sharpness = sh ? atof(sh) : 0.3f; d.enableSharpening = d.sharpness > 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double t = ts.tv_sec + ts.tv_nsec * 1e-9;
    d.frameTimeDelta = last_t > 0 ? (float)((t - last_t) * 1000) : 16.7f; last_t = t;
    d.preExposure = 1; d.reset = reset;
    d.cameraNear = n; d.cameraFar = f; d.cameraFovAngleVertical = fov;
    d.viewSpaceToMetersFactor = 1; d.deviceDepthNegativeOneToOne = true;
    // the engine's depth texture may be set to stencil mode / compare mode: read plain depth meanwhile
    static void (*gtp)(GLuint, GLenum, GLint *) = (void (*)(GLuint, GLenum, GLint *))gpa("glGetTextureParameteriv");
    static void (*stp)(GLuint, GLenum, GLint) = (void (*)(GLuint, GLenum, GLint))gpa("glTextureParameteri");
    GLint dsmode = 0, cmpmode = 0; gtp(depth, 0x90EA, &dsmode); gtp(depth, 0x884C, &cmpmode);
    stp(depth, 0x90EA, 0x1902); stp(depth, 0x884C, 0);
    // FSR samples with a mipmapped sampler; the engine's textures have one level but MAX_LEVEL 1000,
    // which makes them incomplete (reads return 0). Clamp to level 0 while FSR runs.
    GLuint ins[3] = {color, vel, depth}; GLint maxl[3];
    for (int i = 0; i < 3; i++) { gtp(ins[i], 0x813D, &maxl[i]); stp(ins[i], 0x813D, 0); }
    FfxErrorCode e = ffxFsr2ContextDispatch(&ctx, &d);
    gMemoryBarrier(0xFFFFFFFF);
    stp(depth, 0x90EA, dsmode); stp(depth, 0x884C, cmpmode);
    for (int i = 0; i < 3; i++) stp(ins[i], 0x813D, maxl[i]);
    restore(s); dbg_on = 0;
    if (e != FFX_OK) { fprintf(stderr, "[ravenfsr] dispatch failed (%d)\n", (int)e); return; }
    if (nframes++ == 300) fprintf(stderr, "[ravenfsr] dispatch: colour %u %dx%d fmt 0x%x, vel %u fmt 0x%x, depth %u fmt 0x%x, near %.3g far %.4g fov %.1f deg, jitter %.3f,%.3f px\n",
        color, rw, rh, fc, vel, fv, depth, fd, n, f, fov * 57.2958f, d.jitterOffset.x, d.jitterOffset.y);
    dispatched = true;
}

// ---- colour guard ----
// The FSR 2 GL path sometimes (stretches of 100-300 frames, depending on the scene) writes its output with
// the negative YCoCg chroma clipped to 0: bluish / purple turn grey, reds turn orange. Its input is fine
// and the engine's TAA is fine (attract scene with Lang and Ramos flickered dull <-> vivid). Not found
// inside FSR yet (not history, FP16, sharpening, exposure, deringing, motion vectors, unit leaks).
// So every frame two 16x9 thumbnails (FSR input,
// FSR output) are read back asynchronously; when the output has lost most of the input's colour the
// tonemap shows the engine's TAA result (it then runs too) until FSR is right again for 300 frames (it tends to relapse). RAVENFSR_GUARD=0 turns it off.
static int guard = -1;
static bool fsr_bad;
static int taa_hold;                   // frames the engine TAA keeps running after a bad stretch
static GLuint in_tex;                  // FSR colour input this frame
static bool guard_on(void)
{
    if (guard < 0) { const char *e = getenv("RAVENFSR_GUARD"); guard = !(e && *e == '0'); }
    return guard > 0;
}
static void guard_check(void)
{
    if (!guard_on() || !dispatched || !out_tex || !in_tex) return;
    typedef void (*gen_t)(GLsizei, GLuint *); typedef void (*bind_t)(GLenum, GLuint);
    static gen_t gfb, grb, gbuf; static bind_t bfb, brb, bbuf;
    static void (*rbs)(GLenum, GLenum, GLsizei, GLsizei), (*fbrb)(GLenum, GLenum, GLenum, GLuint);
    static void (*fbt)(GLenum, GLenum, GLenum, GLuint, GLint), (*bdata)(GLenum, long long, const void *, GLenum);
    static void (*blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    static void (*rp)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    static void *(*mapr)(GLenum, long long, long long, GLbitfield); static unsigned char (*unmap)(GLenum);
    static unsigned char (*isen)(GLenum); static void (*en)(GLenum), (*dis)(GLenum);
    static void (*gPixelStorei)(GLenum, GLint);
    typedef struct __GLsync *sync_t;
    static sync_t (*fence)(GLenum, GLbitfield); static GLenum (*cws)(sync_t, GLbitfield, unsigned long long); static void (*delsync)(sync_t);
    static sync_t fs[2];
    static GLuint fb[2], rb, pbo[2]; static int slot, filled[2], good;
    if (!fb[0]) {
        gfb = (gen_t)gpa("glGenFramebuffers"); grb = (gen_t)gpa("glGenRenderbuffers"); gbuf = (gen_t)gpa("glGenBuffers");
        bfb = (bind_t)gpa("glBindFramebuffer"); brb = (bind_t)gpa("glBindRenderbuffer"); bbuf = (bind_t)gpa("glBindBuffer");
        rbs = (decltype(rbs))gpa("glRenderbufferStorage"); fbrb = (decltype(fbrb))gpa("glFramebufferRenderbuffer");
        fbt = (decltype(fbt))gpa("glFramebufferTexture2D"); bdata = (decltype(bdata))gpa("glBufferData");
        blit = (decltype(blit))gpa("glBlitFramebuffer"); rp = (decltype(rp))gpa("glReadPixels");
        mapr = (decltype(mapr))gpa("glMapBufferRange"); unmap = (decltype(unmap))gpa("glUnmapBuffer");
        gPixelStorei = (decltype(gPixelStorei))gpa("glPixelStorei");
        fence = (decltype(fence))gpa("glFenceSync"); cws = (decltype(cws))gpa("glClientWaitSync"); delsync = (decltype(delsync))gpa("glDeleteSync");
        if (!fence || !cws || !delsync) { guard = 0; return; }
        isen = (decltype(isen))gpa("glIsEnabled"); en = (decltype(en))gpa("glEnable"); dis = (decltype(dis))gpa("glDisable");
        if (!gfb || !grb || !gbuf || !mapr || !blit) { guard = 0; return; }
        gfb(2, fb); grb(1, &rb); gbuf(2, pbo);
        brb(0x8D41, rb); rbs(0x8D41, 0x881A, 32, 9); brb(0x8D41, 0);
        GLint df0; gGetIntegerv(0x8CA6, &df0);
        bfb(0x8CA9, fb[1]); fbrb(0x8CA9, 0x8CE0, 0x8D41, rb); bfb(0x8CA9, df0);
        GLint pb0; gGetIntegerv(0x88ED, &pb0);
        for (int i = 0; i < 2; i++) { bbuf(0x88EB, pbo[i]); bdata(0x88EB, 32 * 9 * 16, nullptr, 0x88E1 /*STREAM_READ*/); }
        bbuf(0x88EB, pb0);
    }
    GLint rf, df, pb, align; gGetIntegerv(0x8CAA, &rf); gGetIntegerv(0x8CA6, &df); gGetIntegerv(0x88ED, &pb); gGetIntegerv(0x0D05, &align);
    unsigned char sc = isen(0x0C11), srgb = isen(0x8DB9);
    if (sc) dis(0x0C11); if (srgb) dis(0x8DB9);
    // 1) result of the readback from two frames ago, only once the GPU is done with it (never wait)
    int s = slot;
    if (filled[s] && cws(fs[s], 0, 0) == 0x911B /*TIMEOUT_EXPIRED*/) { bfb(0x8CA8, rf); bfb(0x8CA9, df); if (sc) en(0x0C11); if (srgb) en(0x8DB9); return; }
    if (filled[s]) {
        delsync(fs[s]); fs[s] = nullptr;
        bbuf(0x88EB, pbo[s]);
        const float *px = (const float *)mapr(0x88EB, 0, 32 * 9 * 16, 0x0001 /*READ*/);
        if (px) {
            double sat[2] = {0, 0};
            for (int y = 0; y < 9; y++) for (int x = 0; x < 32; x++) {
                const float *c = px + (y * 32 + x) * 4;
                sat[x >= 16] += fmaxf(c[0], fmaxf(c[1], c[2])) - fminf(c[0], fminf(c[1], c[2]));
            }
            unmap(0x88EB);
            double si = sat[0] / 144, so = sat[1] / 144;
            bool bad_now = si > 6.0 / 255 && so < 0.6 * si;
            if (bad_now) taa_hold = 1800;
            if (bad_now) { if (!fsr_bad) fprintf(stderr, "[ravenfsr] guard: FSR output lost its colour (%.3f vs input %.3f), showing TAA\n", so, si); fsr_bad = true; good = 0; }
            else if (fsr_bad && ++good >= 300) { fsr_bad = false; fprintf(stderr, "[ravenfsr] guard: FSR output fine again\n"); }
        }
        filled[s] = 0;
    }
    // 2) this frame's thumbnails -> PBO (read two frames later)
    int w, h;
    bfb(0x8CA9, fb[1]);
    bfb(0x8CA8, fb[0]); texfmt(in_tex, &w, &h); fbt(0x8CA8, 0x8CE0, 0x0DE1, in_tex, 0); blit(0, 0, w, h, 0, 0, 16, 9, 0x4000, 0x2601);
    texfmt(out_tex, &w, &h); fbt(0x8CA8, 0x8CE0, 0x0DE1, out_tex, 0); blit(0, 0, w, h, 16, 0, 32, 9, 0x4000, 0x2601);
    fbt(0x8CA8, 0x8CE0, 0x0DE1, 0, 0);
    bfb(0x8CA8, fb[1]); bbuf(0x88EB, pbo[s]); gPixelStorei(0x0D05, 4);
    rp(0, 0, 32, 9, 0x1908, 0x1406, nullptr);
    fs[s] = fence(0x9117 /*SYNC_GPU_COMMANDS_COMPLETE*/, 0);
    filled[s] = 1; slot = s ^ 1;
    gPixelStorei(0x0D05, align); bbuf(0x88EB, pb);
    bfb(0x8CA8, rf); bfb(0x8CA9, df);
    if (sc) en(0x0C11); if (srgb) en(0x8DB9);
}

// Returns 1 if the engine's draw call must be skipped.
extern "C" int ravenfsr_draw(void)
{
    if (!cur || cur->kind != K_TAA || !want) return 0;
    if (!dispatched) {
        GLint fbo = 0; gGetIntegerv(0x8CA6, &fbo);
        GLint tex = 0; void (*q)(GLuint, GLenum, GLenum, GLint *) = (void (*)(GLuint, GLenum, GLenum, GLint *))gpa("glGetNamedFramebufferAttachmentParameteriv");
        if (fbo && q) q(fbo, 0x8CE0, 0x8CD1, &tex);
        taa_out_tex = tex;
        in_tex = ravengl_unit_tex(0);
        dispatch();
        if (!dispatched) return 0;   // FSR failed: let the engine's TAA run
    }
    // guard: the engine's TAA also runs while FSR is bad and for a while after (it relapses), so the
    // fallback picture is ready; its neighbourhood clamp keeps the first frame after a pause clean.
    return guard_on() && (fsr_bad || taa_hold > 0) ? 0 : 1;
}

// Tonemap reads the TAA result: give it the FSR output instead (unless the guard caught a bad FSR frame).
extern "C" GLuint ravenfsr_bind_texture(GLuint tex)
{
    if (dispatched && !fsr_bad && cur && cur->kind == K_TONE && tex && tex == taa_out_tex && out_tex) return out_tex;
    return tex;
}

extern "C" int ravenfsr_dispatched(void) { return dispatched; }   // debug: FSR ran this frame
extern "C" void ravenfsr_frame_end(void) { guard_check(); if (taa_hold > 0) taa_hold--; dispatched = false; have_jit = false; have_mvp = have_nj = false; }
