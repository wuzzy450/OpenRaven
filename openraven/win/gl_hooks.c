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
    {"glDrawElements", (void *)h_glDrawElements},
    {"glDrawArrays", (void *)h_glDrawArrays},
    {"glDrawElementsInstanced", (void *)h_glDrawElementsInstanced},
    {"glDrawRangeElements", (void *)h_glDrawRangeElements},
    {NULL, NULL},
};
