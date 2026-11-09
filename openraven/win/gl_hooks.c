// GL calls the graphics features need to see (Windows version of the FSR part of ravengltrace.c).
// shim_lookup checks this table before the generated gl_thunks.c, so the game gets these wrappers
// from dlsym and glXGetProcAddress. With FSR 2 off they only forward (plus texture-unit tracking).
//   glUseProgram / glUniformMatrix4fv -> ravenfsr_use_program / ravenfsr_matrix (jitter, projection)
//   glDraw* -> ravenfsr_draw (skip the engine's TAA pass, run FSR 2 instead)
//   glActiveTexture / glBindTexture -> which texture is on which unit; tonemap gets the FSR output
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "shim.h"

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

static SV void h_glBindFramebuffer(GLenum t, GLuint f) {
    REAL(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC);
    r_glBindFramebuffer(t, f);
    if (t != GL_READ_FRAMEBUFFER) cur_fbo = f;
}

// Called once per presented frame (ravengfx_frame).
void ravengl_frame_end(void) {
    if (g_fsr > 0) ravenfsr_frame_end();
}

const shim_entry shims_glhook[] = {
    {"glUseProgram", (void *)h_glUseProgram},
    {"glUniformMatrix4fv", (void *)h_glUniformMatrix4fv},
    {"glActiveTexture", (void *)h_glActiveTexture},
    {"glBindTexture", (void *)h_glBindTexture},
    {"glBindFramebuffer", (void *)h_glBindFramebuffer},
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
