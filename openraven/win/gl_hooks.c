// GL calls the graphics features need to see (Windows version of the FSR part of ravengltrace.c).
// shim_lookup checks this table before the generated gl_thunks.c, so the game gets these wrappers
// from dlsym and glXGetProcAddress. With FSR 2 off they only forward (plus texture-unit tracking).
//   glUseProgram / glUniformMatrix4fv -> ravenfsr_use_program / ravenfsr_matrix (jitter, projection)
//   glDraw* -> ravenfsr_draw (skip the engine's TAA pass, run FSR 2 instead)
//   glActiveTexture / glBindTexture -> which texture is on which unit; tonemap gets the FSR output
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "shim.h"
#include "../glsl_compat.h"   // shared with Linux (ravengltrace.c)

void *gl_resolve(const char *name);
int ravenfsr_wanted(void);
void ravenfsr_use_program(GLuint p);
void ravenfsr_matrix(GLint loc, const float *m);
int ravenfsr_draw(void);
GLuint ravenfsr_bind_texture(GLuint tex);
void ravenfsr_frame_end(void);
int ravenfg_wanted(void);
void ravenfg_draw(GLuint prog, GLuint fbo);
static int g_fg = -1;
static GLuint cur_prog, cur_fbo;
static void fgdraw(void) {
    if (g_fg < 0) g_fg = !raven_env("RAVENGFX_NOGLHOOK") && ravenfg_wanted();
    if (g_fg) ravenfg_draw(cur_prog, cur_fbo);
}

extern int ravenhdr_float_fb;   // shim_glx.c: native HDR (float back buffer)
int ravenhdr_on(void) { return ravenhdr_float_fb; }   // shared code (neural pass): the frame may go above 1.0

static int g_fsr = -1;
static int fsr(void) {
    if (g_fsr < 0) g_fsr = ravenfsr_wanted();
    return g_fsr;
}
static GLenum cur_unit = GL_TEXTURE0;
static GLuint unit_tex[32];
unsigned ravengl_unit_tex(int unit) { return unit >= 0 && unit < 32 ? unit_tex[unit] : 0; }

typedef void (APIENTRY *t_bindtex)(GLenum, GLuint);
typedef void (APIENTRY *t_drawel)(GLenum, GLsizei, GLenum, const void *);
typedef void (APIENTRY *t_drawarr)(GLenum, GLint, GLsizei);
#define REAL(name, type) static type r_##name; if (!r_##name) r_##name = (type)gl_resolve(#name)

static SV void h_glUseProgram(GLuint p) {
    REAL(glUseProgram, PFNGLUSEPROGRAMPROC);
    if (fsr()) ravenfsr_use_program(p);
    cur_prog = p;
    r_glUseProgram(p);
}
static SV void h_glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *m) {
    REAL(glUniformMatrix4fv, PFNGLUNIFORMMATRIX4FVPROC);
    if (fsr() && n == 1 && !tr) ravenfsr_matrix(loc, m);
    r_glUniformMatrix4fv(loc, n, tr, m);
}
static SV void h_glActiveTexture(GLenum u) {
    REAL(glActiveTexture, PFNGLACTIVETEXTUREPROC);
    cur_unit = u;
    r_glActiveTexture(u);
}
static SV void h_glBindTexture(GLenum t, GLuint x) {
    REAL(glBindTexture, t_bindtex);
    if (t == GL_TEXTURE_2D) {
        unsigned u = cur_unit - GL_TEXTURE0;
        if (fsr()) x = ravenfsr_bind_texture(x);
        if (u < 32) unit_tex[u] = x;
    }
    r_glBindTexture(t, x);
}
static SV void h_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) {
    REAL(glDrawElements, t_drawel);
    fgdraw();
    if (fsr() && ravenfsr_draw()) return;
    r_glDrawElements(m, c, t, i);
}
static SV void h_glDrawArrays(GLenum m, GLint f, GLsizei c) {
    REAL(glDrawArrays, t_drawarr);
    fgdraw();
    if (fsr() && ravenfsr_draw()) return;
    r_glDrawArrays(m, f, c);
}
static SV void h_glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n) {
    REAL(glDrawElementsInstanced, PFNGLDRAWELEMENTSINSTANCEDPROC);
    fgdraw();
    if (fsr() && ravenfsr_draw()) return;
    r_glDrawElementsInstanced(m, c, t, i, n);
}
static SV void h_glDrawRangeElements(GLenum m, GLuint a, GLuint b, GLsizei c, GLenum t, const void *i) {
    REAL(glDrawRangeElements, PFNGLDRAWRANGEELEMENTSPROC);
    fgdraw();
    if (fsr() && ravenfsr_draw()) return;
    r_glDrawRangeElements(m, a, b, c, t, i);
}

// HUD/menu sprites (the Ramos/Lang JOIN portraits): the engine leaves them on GL_REPEAT, and their
// bottom row is opaque (body, gun), so the top edge of the quad blends that row in = a 1 px line above
// the head (plain game too). Textures with an alpha format get clamp-to-edge right after upload,
// unless the engine set a wrap mode on that texture before the upload (it would override ours anyway
// if it sets one after). RAVEN_SPRITECLAMP=0 = engine behaviour.
#define MAXTEX 65536
static unsigned char tex_flags[MAXTEX];   // 2 = engine set a wrap mode
static int sprite_clamp(void) {
    static int on = -1;
    if (on < 0) { const char *e = raven_env("RAVEN_SPRITECLAMP"); on = !(e && *e == '0'); }
    return on;
}
static GLuint bound2d(void) { unsigned u = cur_unit - GL_TEXTURE0; return u < 32 ? unit_tex[u] : 0; }
typedef void (APIENTRY *t_texpi)(GLenum, GLenum, GLint);
typedef void (APIENTRY *t_texpf)(GLenum, GLenum, GLfloat);
typedef void (APIENTRY *t_texpiv)(GLenum, GLenum, const GLint *);
typedef void (APIENTRY *t_texpfv)(GLenum, GLenum, const GLfloat *);
static void note_wrap(GLenum target, GLenum pname) {
    GLuint t = bound2d();
    if (target == GL_TEXTURE_2D && (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) && t && t < MAXTEX)
        tex_flags[t] |= 2;
}
static SV void h_glTexParameteri(GLenum t, GLenum p, GLint v) { REAL(glTexParameteri, t_texpi); note_wrap(t, p); r_glTexParameteri(t, p, v); }
static SV void h_glTexParameterf(GLenum t, GLenum p, GLfloat v) { REAL(glTexParameterf, t_texpf); note_wrap(t, p); r_glTexParameterf(t, p, v); }
static SV void h_glTexParameteriv(GLenum t, GLenum p, const GLint *v) { REAL(glTexParameteriv, t_texpiv); note_wrap(t, p); r_glTexParameteriv(t, p, v); }
static SV void h_glTexParameterfv(GLenum t, GLenum p, const GLfloat *v) { REAL(glTexParameterfv, t_texpfv); note_wrap(t, p); r_glTexParameterfv(t, p, v); }
static void sprite_uploaded(GLenum target, GLint level, int alpha) {   // called with the texture bound
    GLuint t = bound2d();
    if (target != GL_TEXTURE_2D || level != 0 || !alpha || !t || t >= MAXTEX || (tex_flags[t] & 2) || !sprite_clamp()) return;
    static t_texpi tp;
    if (!tp) tp = (t_texpi)gl_resolve("glTexParameteri");   // the real one: not counted as an engine call
    tp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    tp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
// Texture names are reused after a delete (new attract scene): forget the old texture's flags.
typedef void (APIENTRY *t_deltex)(GLsizei, const GLuint *);
static SV void h_glDeleteTextures(GLsizei n, const GLuint *ids) {
    REAL(glDeleteTextures, t_deltex);
    for (GLsizei i = 0; ids && i < n; i++) if (ids[i] < MAXTEX) tex_flags[ids[i]] = 0;
    r_glDeleteTextures(n, ids);
}
typedef void (APIENTRY *t_teximg)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static SV void h_glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *p) {
    REAL(glTexImage2D, t_teximg);
    // Native HDR: the engine lights the scene in RGBA16F but resolves it into screen-size 8-bit RGB
    // targets before colour correction, cutting everything above white. Make those float too.
    if (ravenhdr_float_fb && !p && !l && (i == GL_RGB || i == GL_RGB8) && w >= 1280) {
        GLint tex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        raven_log("[hdr] render target %d (%dx%d) RGB8 -> RGBA16F\n", tex, w, h);
        i = GL_RGBA16F;
    }
    r_glTexImage2D(t, l, i, w, h, b, f, ty, p);
    sprite_uploaded(t, l, f == GL_RGBA || f == GL_BGRA || f == GL_LUMINANCE_ALPHA || f == GL_ALPHA);
}
// The game's art (HUD sprites included) arrives as S3TC/BPTC compressed textures.
typedef void (APIENTRY *t_ctex)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
static SV void h_glCompressedTexImage2D(GLenum t, GLint l, GLenum fmt, GLsizei w, GLsizei h, GLint b, GLsizei sz, const void *p) {
    REAL(glCompressedTexImage2D, t_ctex);
    r_glCompressedTexImage2D(t, l, fmt, w, h, b, sz, p);
    sprite_uploaded(t, l, fmt == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT || fmt == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ||
                          fmt == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT || fmt == GL_COMPRESSED_RGBA_BPTC_UNORM ||
                          fmt == GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM || fmt == 0x8C4D || fmt == 0x8C4E || fmt == 0x8C4F);
}

// Native HDR: blending HUD sprites onto the float *window* buffer clamps the whole picture to 1.0
// (NVIDIA), so the game's screen (framebuffer 0) becomes our own RGBA16F framebuffer at the game's
// resolution. ravenhdr_win.c presents from it, the F1 overlay draws into it.
extern int ravengfx_res_w, ravengfx_res_h;
static GLuint scr_fbo, scr_tex, scr_ds; static HGLRC scr_ctx;
unsigned ravengl_screen_fbo(void) { return scr_fbo && wglGetCurrentContext() == scr_ctx ? scr_fbo : 0; }
void ravengl_screen_size(int *w, int *h) { *w = ravengfx_res_w; *h = ravengfx_res_h; }
static GLuint screen_fbo(void) {
    if (!ravenhdr_float_fb || ravengfx_res_w <= 0 || ravengfx_res_h <= 0) return 0;
    HGLRC c = wglGetCurrentContext();
    if (scr_fbo || !c) return c == scr_ctx ? scr_fbo : 0;
    REAL(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC);
    PFNGLGENFRAMEBUFFERSPROC gf = (PFNGLGENFRAMEBUFFERSPROC)gl_resolve("glGenFramebuffers");
    PFNGLGENRENDERBUFFERSPROC gr = (PFNGLGENRENDERBUFFERSPROC)gl_resolve("glGenRenderbuffers");
    PFNGLBINDRENDERBUFFERPROC br = (PFNGLBINDRENDERBUFFERPROC)gl_resolve("glBindRenderbuffer");
    PFNGLRENDERBUFFERSTORAGEPROC rs = (PFNGLRENDERBUFFERSTORAGEPROC)gl_resolve("glRenderbufferStorage");
    PFNGLFRAMEBUFFERTEXTURE2DPROC ft = (PFNGLFRAMEBUFFERTEXTURE2DPROC)gl_resolve("glFramebufferTexture2D");
    PFNGLFRAMEBUFFERRENDERBUFFERPROC fr = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)gl_resolve("glFramebufferRenderbuffer");
    PFNGLCHECKFRAMEBUFFERSTATUSPROC cs = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)gl_resolve("glCheckFramebufferStatus");
    if (!gf || !gr || !br || !rs || !ft || !fr || !cs) return 0;
    GLint ot = 0, orb = 0, odf = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &ot); glGetIntegerv(GL_RENDERBUFFER_BINDING, &orb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &odf);
    int w = ravengfx_res_w, h = ravengfx_res_h;
    glGenTextures(1, &scr_tex); glBindTexture(GL_TEXTURE_2D, scr_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gr(1, &scr_ds); br(GL_RENDERBUFFER, scr_ds); rs(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    gf(1, &scr_fbo); r_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scr_fbo);
    ft(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scr_tex, 0);
    fr(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, scr_ds);
    GLenum st = cs(GL_DRAW_FRAMEBUFFER);
    r_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, odf);
    glBindTexture(GL_TEXTURE_2D, ot); br(GL_RENDERBUFFER, orb);
    if (st != GL_FRAMEBUFFER_COMPLETE) { raven_log("[hdr] screen framebuffer incomplete (%#x), native HDR highlights will clip\n", st); scr_fbo = 0; return 0; }
    scr_ctx = c;
    raven_log("[hdr] game screen -> RGBA16F framebuffer %u (%dx%d)\n", scr_fbo, w, h);
    return scr_fbo;
}
static SV void h_glBindFramebuffer(GLenum t, GLuint f) {
    REAL(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC);
    GLuint real = f;
    if (!f) { GLuint s = screen_fbo(); if (s) real = s; }
    r_glBindFramebuffer(t, real);
    if (t != GL_READ_FRAMEBUFFER) cur_fbo = f;
}
// GL_BACK / GL_FRONT are not valid on our stand-in framebuffer: map them to its colour attachment.
static GLenum map_buf(GLenum b, GLenum which) {
    if (!scr_fbo || (b != GL_BACK && b != GL_BACK_LEFT && b != GL_FRONT && b != GL_FRONT_LEFT && b != GL_FRONT_AND_BACK)) return b;
    GLint fb = 0; glGetIntegerv(which, &fb);
    return (GLuint)fb == scr_fbo ? GL_COLOR_ATTACHMENT0 : b;
}
typedef void (APIENTRY *t_buf)(GLenum);
static SV void h_glDrawBuffer(GLenum b) { REAL(glDrawBuffer, t_buf); r_glDrawBuffer(map_buf(b, GL_DRAW_FRAMEBUFFER_BINDING)); }
static SV void h_glReadBuffer(GLenum b) { REAL(glReadBuffer, t_buf); r_glReadBuffer(map_buf(b, GL_READ_FRAMEBUFFER_BINDING)); }

// Native HDR: the game's colour-correction shader (shader105) ends with an Uncharted 2 filmic tonemap
// (the game runs it with exposure 1 and white point 1). Its input used to be 8-bit and is clamped to
// 0..1, so nothing was brighter than white. With float scene targets (h_glTexImage2D) and a float
// back buffer (shim_glx.c) we drop that clamp and swap in a tonemap that is identical up to SDR
// white. Above white the filmic curve keeps rising (up to ~2.9x white); HDR_HIGHLIGHTS scales that
// extra range (0.5 = the curve's own shoulder, 0 = SDR, 1 = twice as far), worked out on luminance
// so saturated effects (blue shields, plasma) stay calm, and capped at the display peak
// (P = HDR_PEAK / HDR_PAPERWHITE). Output stays encoded like before, so the HUD drawn afterwards
// still lines up; ravenhdr_win.c maps 1.0 to paper white. Changes need a restart.
const char *ravengfx_get(const char *key);
static const char tonemap_hdr[] =
    "vec3 tone_mapping(in vec3 hdr)\n{\n"
    "  vec3 x = max(hdr * uExposure * uExposureBias, vec3(0.0));\n"
    "  vec3 s = max(Uncharted2Tonemap(x) / Uncharted2Tonemap(vec3(uW)), vec3(0.0));\n"
    "  float L = dot(s, vec3(0.2126, 0.7152, 0.0722));\n"          // 1.0 = SDR white
    "  if (L > 1.0) s *= (1.0 + (L - 1.0) * RAVEN_HDR_GAIN) / L;\n"
    "  float m = max(max(s.r, s.g), s.b);\n"
    "  if (m > RAVEN_HDR_P) s *= RAVEN_HDR_P / m;\n"
    "  return pow(s, vec3(uGamma));\n}\n";

static char *rewrite_tonemap(const char *src) {
    const char *a = strstr(src, "vec3 tone_mapping(in vec3 hdr)");
    const char *b = a ? strstr(a, "void main") : NULL;
    const char *v = strstr(src, "#version");
    if (!a || !b || !v || !strstr(src, "Uncharted2Tonemap") || !strstr(src, "uExposureBias")) return NULL;
    const char *pv = ravengfx_get("HDR_PEAK"), *wv = ravengfx_get("HDR_PAPERWHITE");
    float peak = pv && atof(pv) > 100 ? (float)atof(pv) : 1000, paper = wv && atof(wv) >= 40 ? (float)atof(wv) : 203;
    float P = peak / paper; if (P < 1) P = 1;
    const char *nl = strchr(v, '\n'); if (!nl) return NULL;
    const char *sv = ravengfx_get("HDR_HIGHLIGHTS");   // 0 = SDR look, 0.5 = the filmic curve's own range
    float k = sv ? (float)atof(sv) : 0.5f; if (k < 0) k = 0; if (k > 1) k = 1;
    char def[96]; snprintf(def, sizeof def, "#define RAVEN_HDR_P %.4f\n#define RAVEN_HDR_GAIN %.3f\n", P, 2 * k);
    size_t n = strlen(src) + sizeof tonemap_hdr + sizeof def + 16;
    char *o = malloc(n), *p = o;
    memcpy(p, src, nl + 1 - src); p += nl + 1 - src;     // keep #version first
    p += sprintf(p, "%s", def);
    memcpy(p, nl + 1, a - nl - 1); p += a - nl - 1;
    p += sprintf(p, "%s", tonemap_hdr);
    strcpy(p, b);
    // colour correction clamps to 0..1 before the tonemap: keep values above white
    static const char cl[] = "pow( clamp(base.rgb, 0, 1), gamma_vec )", nc[] = "pow(max(base.rgb,vec3(0)),gamma_vec)";
    char *c = strstr(p, cl);
    if (c) { memset(c, ' ', sizeof cl - 1); memcpy(c, nc, sizeof nc - 1); }   // shorter, padded with spaces
    else raven_log("[hdr] colour-correction clamp not found, highlights stay clipped\n");
    raven_log("[hdr] native HDR tonemap installed in the game's colour-correction shader (peak %.0f / paper %.0f nits = %.2fx white)\n", peak, paper, P);
    return o;
}

// Every shader goes through raven_glsl_compat: some effect shaders ask for "#version ... core" but use
// texture2D/textureCube, which strict compilers (AMD, Intel, Mesa) reject, so Elite/Jackal shields, the
// energy sword and Spirit effects would be missing. NVIDIA accepts them either way.
// graphics.ini DUMP_SHADERS = 1: append every shader source the game compiles to shader_dump.txt (debug).
static SV void h_glShaderSource(GLuint sh, GLsizei n, const GLchar *const *str, const GLint *len) {
    REAL(glShaderSource, PFNGLSHADERSOURCEPROC);
    const char *dv = ravengfx_get("DUMP_SHADERS");
    int dump = dv && atoi(dv);
    size_t tot = 0;
    for (GLsizei i = 0; i < n; i++) tot += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    char *all = malloc(tot + 1), *p = all;
    if (!all) { r_glShaderSource(sh, n, str, len); return; }
    for (GLsizei i = 0; i < n; i++) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(p, str[i], l); p += l; }
    *p = 0;
    int fixes = raven_glsl_compat(all);
    if (fixes) raven_log("[glsl-compat] shader %u: modernized %d legacy sampler call(s)\n", sh, fixes);
    if (dump) {
        char dp[MAX_PATH * 2]; snprintf(dp, sizeof dp, "%s/shader_dump.txt", g_root);
        FILE *f = fopen(dp, "ab");
        if (f) { fprintf(f, "==== shader %u\n%s\n", sh, all); fclose(f); }
    }
    char *hd = ravenhdr_float_fb ? rewrite_tonemap(all) : NULL;
    const GLchar *one = hd ? hd : all;
    r_glShaderSource(sh, 1, &one, NULL);
    free(hd); free(all);
}

// Called once per presented frame (ravengfx_frame).
void ravengl_frame_end(void) {
    if (g_fsr > 0) ravenfsr_frame_end();
}

const shim_entry shims_glhook[] = {
    {"glUseProgram", (void *)h_glUseProgram},
    {"glShaderSource", (void *)h_glShaderSource},
    {"glUniformMatrix4fv", (void *)h_glUniformMatrix4fv},
    {"glActiveTexture", (void *)h_glActiveTexture},
    {"glBindTexture", (void *)h_glBindTexture},
    {"glBindFramebuffer", (void *)h_glBindFramebuffer},
    {"glDrawBuffer", (void *)h_glDrawBuffer},
    {"glReadBuffer", (void *)h_glReadBuffer},
    {"glTexImage2D", (void *)h_glTexImage2D},
    {"glCompressedTexImage2D", (void *)h_glCompressedTexImage2D},
    {"glDeleteTextures", (void *)h_glDeleteTextures},
    {"glTexParameteri", (void *)h_glTexParameteri},
    {"glTexParameterf", (void *)h_glTexParameterf},
    {"glTexParameteriv", (void *)h_glTexParameteriv},
    {"glTexParameterfv", (void *)h_glTexParameterfv},
    {"glDrawElements", (void *)h_glDrawElements},
    {"glDrawArrays", (void *)h_glDrawArrays},
    {"glDrawElementsInstanced", (void *)h_glDrawElementsInstanced},
    {"glDrawRangeElements", (void *)h_glDrawRangeElements},
    {NULL, NULL},
};
