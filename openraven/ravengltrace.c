// ravengltrace.c - GL call tracer for reverse-engineering the engine's frame (FBOs, programs, matrices).
// RAVENGFX_GLTRACE=<secs>  : at the first present after <secs>, log every traced GL call of one frame
//                            to stderr ("[gltrace] ..."). Function pointers are found by scanning the
//                            game's writable memory for the real GL entry points and replacing them.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include "glsl_compat.h"

typedef unsigned GLenum, GLuint, GLbitfield; typedef int GLint, GLsizei; typedef unsigned char GLboolean;

// FSR 2 hooks (ravenfsr.cpp)
int ravenfsr_wanted(void); void ravenfsr_use_program(GLuint p); void ravenfsr_matrix(GLint loc, const float *m);
int ravenfsr_draw(void); GLuint ravenfsr_bind_texture(GLuint tex); void ravenfsr_frame_end(void);
static int fsr;                           // FSR 2 active: wrappers feed it
int ravenfg_wanted(void); void ravenfg_draw(GLuint prog, GLuint fbo);
static int fg;                            // frame generation hooks
static GLuint unit_tex[32];
GLuint ravengl_unit_tex(int u) { return u >= 0 && u < 32 ? unit_tex[u] : 0; }

int ravenhdr_on(void);
static int tracing;                       // 1 while the trace frame is being logged
static long ndraw, ndraw_seg;
static GLuint cur_prog, cur_fbo;

static void *gpa(const char *n){
    static void *(*g)(const char *);
    if (!g) g = dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    void *f = g ? g(n) : NULL;
    return f ? f : dlsym(RTLD_DEFAULT, n);
}

// GL 4.5 DSA queries (diagnostics only; fine on the dev box)
static void (*q_fbattach)(GLuint, GLenum, GLenum, GLint *);
static void (*q_texlevel)(GLuint, GLint, GLenum, GLint *);
static void (*q_rbparam)(GLuint, GLenum, GLint *);
static void (*q_progiv)(GLuint, GLenum, GLint *);
static void (*q_activeuni)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *);
static GLint (*q_uniloc)(GLuint, const char *);

static void describe_fbo(GLuint fbo){
    if (!fbo) { fprintf(stderr, "[gltrace]   fbo 0 = default\n"); return; }
    static const GLenum att[] = {0x8CE0,0x8CE1,0x8CE2,0x8CE3,0x8CE4,0x8CE5,0x8D00,0x8D20,0x821A};
    static const char *an[] = {"c0","c1","c2","c3","c4","c5","depth","stencil","ds"};
    for (int i = 0; i < 9; i++) {
        GLint type = 0, name = 0, w = 0, h = 0, fmt = 0;
        q_fbattach(fbo, att[i], 0x8CD0, &type);
        if (!type) continue;
        q_fbattach(fbo, att[i], 0x8CD1, &name);
        if (type == 0x1702) { q_texlevel(name, 0, 0x1000, &w); q_texlevel(name, 0, 0x1001, &h); q_texlevel(name, 0, 0x1003, &fmt); }
        else { q_rbparam(name, 0x8D42, &w); q_rbparam(name, 0x8D43, &h); q_rbparam(name, 0x8D44, &fmt); }
        fprintf(stderr, "[gltrace]   %s %s %d %dx%d fmt 0x%x\n", an[i], type == 0x1702 ? "tex" : "rb", name, w, h, fmt);
    }
}

static const char *uni_name(GLuint prog, GLint loc){
    static char buf[128]; GLint n = 0; buf[0] = 0;
    q_progiv(prog, 0x8B86, &n);   // GL_ACTIVE_UNIFORMS
    for (GLint i = 0; i < n; i++) {
        GLsizei l; GLint sz; GLenum ty; char nm[128];
        q_activeuni(prog, i, sizeof nm, &l, &sz, &ty, nm);
        if (q_uniloc(prog, nm) == loc) { snprintf(buf, sizeof buf, "%s", nm); break; }
    }
    return buf;
}
static void prog_uniforms(GLuint prog){
    static GLuint seen[4096]; static int ns;
    for (int i = 0; i < ns; i++) if (seen[i] == prog) return;
    if (ns < 4096) seen[ns++] = prog;
    GLint n = 0; q_progiv(prog, 0x8B86, &n);
    char line[2048]; int p = snprintf(line, sizeof line, "[gltrace]   prog %u uniforms:", prog);
    for (GLint i = 0; i < n && p < (int)sizeof line - 80; i++) {
        GLsizei l; GLint sz; GLenum ty; char nm[64];
        q_activeuni(prog, i, sizeof nm, &l, &sz, &ty, nm);
        p += snprintf(line + p, sizeof line - p, " %s", nm);
    }
    fprintf(stderr, "%s\n", line);
}

#define R(name, ret, args) static ret (*r_##name) args;
R(glBindFramebuffer, void, (GLenum, GLuint))
R(glUseProgram, void, (GLuint))
R(glUniformMatrix4fv, void, (GLint, GLsizei, GLboolean, const float *))
R(glUniform1i, void, (GLint, GLint))
R(glDrawElements, void, (GLenum, GLsizei, GLenum, const void *))
R(glDrawArrays, void, (GLenum, GLint, GLsizei))
R(glDrawElementsInstanced, void, (GLenum, GLsizei, GLenum, const void *, GLsizei))
R(glDrawRangeElements, void, (GLenum, GLuint, GLuint, GLsizei, GLenum, const void *))
R(glBlitFramebuffer, void, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
R(glClear, void, (GLbitfield))
R(glViewport, void, (GLint, GLint, GLsizei, GLsizei))
R(glDrawBuffers, void, (GLsizei, const GLenum *))
R(glBindTexture, void, (GLenum, GLuint))
R(glActiveTexture, void, (GLenum))
R(glClearColor, void, (float, float, float, float))
R(glClearBufferfv, void, (GLenum, GLint, const float *))
R(glTexParameteri, void, (GLenum, GLenum, GLint))
R(glTexParameterf, void, (GLenum, GLenum, float))
R(glTexParameteriv, void, (GLenum, GLenum, const GLint *))
R(glTexParameterfv, void, (GLenum, GLenum, const float *))
R(glDeleteTextures, void, (GLsizei, const GLuint *))
R(glTexImage2D, void, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
R(glCompressedTexImage2D, void, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *))
R(glShaderSource, void, (GLuint, GLsizei, const char *const *, const GLint *))
R(glDrawBuffer, void, (GLenum))
R(glReadBuffer, void, (GLenum))

static void seg_end(void){ if (tracing && ndraw_seg) fprintf(stderr, "[gltrace]   ... %ld draws\n", ndraw_seg); ndraw_seg = 0; }
static GLuint screen_fbo(void);
static void w_glBindFramebuffer(GLenum t, GLuint f){
    GLuint real = f;
    if (!f) { GLuint s = screen_fbo(); if (s) real = s; }
    r_glBindFramebuffer(t, real);
    if (t != 0x8CA8) cur_fbo = f;
    if (!tracing) return;
    if (t == 0x8CA8) { fprintf(stderr, "[gltrace] bind READ fbo %u\n", f); return; }
    seg_end(); fprintf(stderr, "[gltrace] bind fbo %u (0x%x)\n", f, t); describe_fbo(f);
}
static void dump_src(GLuint p){
    static GLuint done_[64]; static int nd; const char *want = getenv("RAVENGFX_PROGSRC");
    if (!want || !p) return;
    char key[16]; snprintf(key, sizeof key, ",%u,", p); char w2[512]; snprintf(w2, sizeof w2, ",%s,", want);
    if (!strstr(w2, key)) return;
    for (int i = 0; i < nd; i++) if (done_[i] == p) return;
    if (nd < 64) done_[nd++] = p;
    void (*att)(GLuint, GLsizei, GLsizei *, GLuint *) = gpa("glGetAttachedShaders");
    void (*src)(GLuint, GLsizei, GLsizei *, char *) = gpa("glGetShaderSource");
    GLuint sh[8]; GLsizei n = 0; att(p, 8, &n, sh);
    char *buf = malloc(1 << 20);
    for (int i = 0; i < n; i++) { GLsizei l = 0; src(sh[i], 1 << 20, &l, buf); fprintf(stderr, "[progsrc %u.%d]\n%s\n[/progsrc]\n", p, i, buf); }
    free(buf);
}
static void w_glUseProgram(GLuint p){
    r_glUseProgram(p); cur_prog = p;
    if (fsr) ravenfsr_use_program(p);
    if (tracing) { fprintf(stderr, "[gltrace] use prog %u\n", p); if (p) { prog_uniforms(p); dump_src(p); } }
}
static void w_glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const float *m){
    r_glUniformMatrix4fv(loc, n, tr, m);
    if (fsr && n == 1 && !tr) ravenfsr_matrix(loc, m);
    if (!tracing || !cur_prog) return;
    const char *nm = uni_name(cur_prog, loc);
    if (!strstr(nm, "roj") && !strstr(nm, "View")) return;
    fprintf(stderr, "[gltrace]   mat %s x%d%s:", nm, n, tr ? " T" : "");
    for (int i = 0; i < 16; i++) fprintf(stderr, " %.5g", m[i]);
    fprintf(stderr, "\n");
}
static void w_glUniform1i(GLint loc, GLint v){
    r_glUniform1i(loc, v);
    if (tracing && cur_prog) fprintf(stderr, "[gltrace]   int %s = %d\n", uni_name(cur_prog, loc), v);
}
static void w_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i){ if (fg) ravenfg_draw(cur_prog, cur_fbo); if (fsr && ravenfsr_draw()) return; r_glDrawElements(m,c,t,i); ndraw++; ndraw_seg++; }
static void w_glDrawArrays(GLenum m, GLint f, GLsizei c){ if (fg) ravenfg_draw(cur_prog, cur_fbo); if (fsr && ravenfsr_draw()) return; r_glDrawArrays(m,f,c); ndraw++; ndraw_seg++; }
static void w_glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n){ if (fg) ravenfg_draw(cur_prog, cur_fbo); if (fsr && ravenfsr_draw()) return; r_glDrawElementsInstanced(m,c,t,i,n); ndraw++; ndraw_seg++; }
static void w_glDrawRangeElements(GLenum m, GLuint a, GLuint b, GLsizei c, GLenum t, const void *i){ if (fg) ravenfg_draw(cur_prog, cur_fbo); if (fsr && ravenfsr_draw()) return; r_glDrawRangeElements(m,a,b,c,t,i); ndraw++; ndraw_seg++; }
static void w_glBlitFramebuffer(GLint a,GLint b,GLint c,GLint d,GLint e,GLint f,GLint g,GLint h,GLbitfield m,GLenum fl){
    r_glBlitFramebuffer(a,b,c,d,e,f,g,h,m,fl);
    if (tracing) fprintf(stderr, "[gltrace] blit %d,%d-%d,%d -> %d,%d-%d,%d mask 0x%x\n", a,b,c,d,e,f,g,h,m);
}
static void w_glClearColor(float r, float g, float b, float a){ r_glClearColor(r,g,b,a); if (tracing) fprintf(stderr, "[gltrace]   clearcolor %g %g %g %g\n", r,g,b,a); }
static void w_glClearBufferfv(GLenum bf, GLint i, const float *v){ r_glClearBufferfv(bf,i,v); if (tracing) fprintf(stderr, "[gltrace]   clearbuffer 0x%x %d = %g %g %g %g\n", bf, i, v[0], bf == 0x1800 ? v[1] : 0, bf == 0x1800 ? v[2] : 0, bf == 0x1800 ? v[3] : 0); }
static void w_glClear(GLbitfield m){ r_glClear(m); if (tracing) fprintf(stderr, "[gltrace]   clear 0x%x\n", m); }
static void w_glViewport(GLint x, GLint y, GLsizei w, GLsizei h){ r_glViewport(x,y,w,h); if (tracing) fprintf(stderr, "[gltrace]   viewport %d,%d %dx%d\n", x,y,w,h); }
static void w_glDrawBuffers(GLsizei n, const GLenum *b){
    r_glDrawBuffers(n, b);
    if (tracing) { fprintf(stderr, "[gltrace]   drawbuffers"); for (int i = 0; i < n; i++) fprintf(stderr, " 0x%x", b[i]); fprintf(stderr, "\n"); }
}
static GLenum cur_unit;
static void w_glActiveTexture(GLenum u){ r_glActiveTexture(u); cur_unit = u; }
static void w_glBindTexture(GLenum t, GLuint x){
    if (t == 0x0DE1) { unsigned u = cur_unit ? cur_unit - 0x84C0 : 0; if (fsr) x = ravenfsr_bind_texture(x); if (u < 32) unit_tex[u] = x; }
    r_glBindTexture(t, x);
    if (tracing) { GLint w = 0, h = 0, f = 0; if (x) { q_texlevel(x, 0, 0x1000, &w); q_texlevel(x, 0, 0x1001, &h); q_texlevel(x, 0, 0x1003, &f); }
        fprintf(stderr, "[gltrace]   tex unit %u <- %u (%dx%d 0x%x)\n", cur_unit - 0x84C0, x, w, h, f); } }

// HUD/menu sprites (the Ramos/Lang JOIN portraits): the engine leaves them on GL_REPEAT, and their
// bottom row is opaque, so the top edge of the quad blends that row in = a 1 px line above the head
// (plain game too). Same fix as win/gl_hooks.c: textures with an alpha format get clamp-to-edge right
// after upload, unless the engine set a wrap mode on that texture first. RAVEN_SPRITECLAMP=0 = off.
#define MAXTEX 65536
static unsigned char tex_flags[MAXTEX];   // 2 = engine set a wrap mode
static int sprite_clamp(void){ static int on = -1; if (on < 0) { const char *e = getenv("RAVEN_SPRITECLAMP"); on = !(e && *e == '0'); } return on; }
static GLuint bound2d(void){ unsigned u = cur_unit ? cur_unit - 0x84C0 : 0; return u < 32 ? unit_tex[u] : 0; }
static void note_wrap(GLenum t, GLenum p){ GLuint x = bound2d(); if (t == 0x0DE1 && (p == 0x2802 || p == 0x2803) && x && x < MAXTEX) tex_flags[x] |= 2; }
static void w_glTexParameteri(GLenum t, GLenum p, GLint v){ note_wrap(t, p); r_glTexParameteri(t, p, v); }
static void w_glTexParameterf(GLenum t, GLenum p, float v){ note_wrap(t, p); r_glTexParameterf(t, p, v); }
static void w_glTexParameteriv(GLenum t, GLenum p, const GLint *v){ note_wrap(t, p); r_glTexParameteriv(t, p, v); }
static void w_glTexParameterfv(GLenum t, GLenum p, const float *v){ note_wrap(t, p); r_glTexParameterfv(t, p, v); }
static void sprite_uploaded(GLenum t, GLint l, int alpha){
    GLuint x = bound2d();
    if (t != 0x0DE1 || l != 0 || !alpha || !x || x >= MAXTEX || (tex_flags[x] & 2) || !sprite_clamp()) return;
    static void (*tp)(GLenum, GLenum, GLint); if (!tp) tp = gpa("glTexParameteri");
    tp(0x0DE1, 0x2802, 0x812F); tp(0x0DE1, 0x2803, 0x812F);
}
static void w_glDeleteTextures(GLsizei n, const GLuint *ids){ for (GLsizei i = 0; ids && i < n; i++) if (ids[i] < MAXTEX) tex_flags[ids[i]] = 0; r_glDeleteTextures(n, ids); }
static void w_glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *p){
    // Native HDR: the engine lights the scene in RGBA16F but resolves it into screen-size 8-bit RGB
    // targets before colour correction, cutting everything above white. Make those float too.
    if (ravenhdr_on() && t == 0x0DE1 && !p && !l && (i == 0x1907 || i == 0x8051) && w >= 1280) {
        fprintf(stderr, "[hdr] render target %u (%dx%d) RGB8 -> RGBA16F\n", bound2d(), w, h);
        i = 0x881A;
    }
    r_glTexImage2D(t, l, i, w, h, b, f, ty, p);
    sprite_uploaded(t, l, f == 0x1908 || f == 0x80E1 || f == 0x190A || f == 0x1906);
}
static void w_glCompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei sz, const void *p){
    r_glCompressedTexImage2D(t, l, f, w, h, b, sz, p);
    sprite_uploaded(t, l, f == 0x83F1 || f == 0x83F2 || f == 0x83F3 || f == 0x8E8C || f == 0x8E8D || f == 0x8C4D || f == 0x8C4E || f == 0x8C4F);
}

// ---- Native HDR (graphics.ini HDR = 1), same as win/gl_hooks.c ----
// Blending HUD sprites onto a float *window* buffer clamps the whole picture to 1.0 (NVIDIA), and the
// X11 window is 8-bit anyway, so the game's screen (framebuffer 0) becomes our own RGBA16F framebuffer
// at the game's resolution. ravenhdr.c presents from it (HDR10 through Vulkan, or a plain blit), and
// the overlay, frame generation, the neural pass and screenshots use it through ravengl_screen_fbo().
const char *ravengfx_get(const char *key);
extern int ravengfx_win_w, ravengfx_win_h;
int ravenhdr_on(void) { static int on = -1; if (on < 0) { const char *v = ravengfx_get("HDR"); on = v && atoi(v) == 1; } return on; }
static GLuint scr_fbo, scr_tex, scr_ds; static void *scr_ctx; static int scr_failed;
static void *cur_ctx(void){ static void *(*g)(void); if (!g) g = dlsym(RTLD_DEFAULT, "glXGetCurrentContext"); return g ? g() : NULL; }
unsigned ravengl_screen_fbo(void) { return scr_fbo && cur_ctx() == scr_ctx ? scr_fbo : 0; }
unsigned ravengl_screen_tex(void) { return scr_tex; }
void ravengl_screen_size(int *w, int *h) { *w = ravengfx_win_w; *h = ravengfx_win_h; }
static GLuint screen_fbo(void){
    if (!ravenhdr_on() || scr_failed || ravengfx_win_w <= 0 || ravengfx_win_h <= 0) return 0;
    void *c = cur_ctx();
    if (scr_fbo || !c) return c == scr_ctx ? scr_fbo : 0;
    void (*gf)(GLsizei, GLuint *) = gpa("glGenFramebuffers"), (*gr)(GLsizei, GLuint *) = gpa("glGenRenderbuffers"), (*gt)(GLsizei, GLuint *) = gpa("glGenTextures");
    void (*br)(GLenum, GLuint) = gpa("glBindRenderbuffer"), (*rs)(GLenum, GLenum, GLsizei, GLsizei) = gpa("glRenderbufferStorage");
    void (*ft)(GLenum, GLenum, GLenum, GLuint, GLint) = gpa("glFramebufferTexture2D"), (*fr)(GLenum, GLenum, GLenum, GLuint) = gpa("glFramebufferRenderbuffer");
    GLenum (*cs)(GLenum) = gpa("glCheckFramebufferStatus"); void (*gi)(GLenum, GLint *) = gpa("glGetIntegerv");
    void (*tp)(GLenum, GLenum, GLint) = gpa("glTexParameteri"), (*bt)(GLenum, GLuint) = gpa("glBindTexture");
    void (*ti)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) = gpa("glTexImage2D");
    if (!gf || !gr || !gt || !br || !rs || !ft || !fr || !cs || !gi || !tp || !bt || !ti || !r_glBindFramebuffer) { scr_failed = 1; return 0; }
    GLint ot = 0, orb = 0, odf = 0; gi(0x8069, &ot); gi(0x8CA7, &orb); gi(0x8CA6, &odf);
    int w = ravengfx_win_w, h = ravengfx_win_h;
    gt(1, &scr_tex); bt(0x0DE1, scr_tex);
    ti(0x0DE1, 0, 0x881A, w, h, 0, 0x1908, 0x140B, NULL);              // RGBA16F
    tp(0x0DE1, 0x2801, 0x2600); tp(0x0DE1, 0x2800, 0x2600); tp(0x0DE1, 0x813D, 0);
    gr(1, &scr_ds); br(0x8D41, scr_ds); rs(0x8D41, 0x88F0, w, h);       // DEPTH24_STENCIL8
    gf(1, &scr_fbo); r_glBindFramebuffer(0x8CA9, scr_fbo);
    ft(0x8CA9, 0x8CE0, 0x0DE1, scr_tex, 0); fr(0x8CA9, 0x821A, 0x8D41, scr_ds);
    GLenum st = cs(0x8CA9);
    r_glBindFramebuffer(0x8CA9, odf); bt(0x0DE1, ot); br(0x8D41, orb);
    if (st != 0x8CD5) { fprintf(stderr, "[hdr] screen framebuffer incomplete (%#x), HDR off\n", st); scr_fbo = 0; scr_failed = 1; return 0; }
    scr_ctx = c;
    fprintf(stderr, "[hdr] game screen -> RGBA16F framebuffer %u (%dx%d)\n", scr_fbo, w, h);
    return scr_fbo;
}
// GL_BACK / GL_FRONT are not valid on our stand-in framebuffer: map them to its colour attachment.
GLenum ravengl_map_buf(GLenum b, int read){
    if (!scr_fbo || (b != 0x0405 && b != 0x0402 && b != 0x0404 && b != 0x0400 && b != 0x0408)) return b;
    static void (*gi)(GLenum, GLint *); if (!gi) gi = gpa("glGetIntegerv");
    GLint fb = 0; gi(read ? 0x8CAA : 0x8CA6, &fb);
    return (GLuint)fb == scr_fbo ? 0x8CE0 : b;
}
static void w_glDrawBuffer(GLenum b){ r_glDrawBuffer(ravengl_map_buf(b, 0)); }
static void w_glReadBuffer(GLenum b){ r_glReadBuffer(ravengl_map_buf(b, 1)); }
// For our own modules (frame generation, neural pass, overlay, capture): the same redirect, called
// with the real GL entry points they resolved themselves.
void ravengl_bind_window(void (*bind)(GLenum, GLuint), GLenum target){ bind(target, ravengl_screen_fbo()); }

// The game's colour-correction shader (shader105) ends with an Uncharted 2 filmic tonemap (exposure 1,
// white point 1). Its input used to be 8-bit and is clamped to 0..1, so nothing was brighter than
// white. With float scene targets we drop that clamp and swap in a tonemap that is identical up to SDR
// white. Above white, HDR_HIGHLIGHTS scales the curve's extra range (0.5 = its own shoulder, 0 = SDR),
// worked out on luminance so saturated effects stay calm, capped at HDR_PEAK / HDR_PAPERWHITE.
// Output stays encoded like before, so the HUD drawn afterwards still lines up. Restart to change.
static const char tonemap_hdr[] =
    "vec3 tone_mapping(in vec3 hdr)\n{\n"
    "  vec3 x = max(hdr * uExposure * uExposureBias, vec3(0.0));\n"
    "  vec3 s = max(Uncharted2Tonemap(x) / Uncharted2Tonemap(vec3(uW)), vec3(0.0));\n"
    "  float L = dot(s, vec3(0.2126, 0.7152, 0.0722));\n"
    "  if (L > 1.0) s *= (1.0 + (L - 1.0) * RAVEN_HDR_GAIN) / L;\n"
    "  float m = max(max(s.r, s.g), s.b);\n"
    "  if (m > RAVEN_HDR_P) s *= RAVEN_HDR_P / m;\n"
    "  return pow(s, vec3(uGamma));\n}\n";
static char *rewrite_tonemap(const char *src){
    const char *a = strstr(src, "vec3 tone_mapping(in vec3 hdr)");
    const char *b = a ? strstr(a, "void main") : NULL;
    const char *v = strstr(src, "#version");
    if (!a || !b || !v || !strstr(src, "Uncharted2Tonemap") || !strstr(src, "uExposureBias")) return NULL;
    const char *pv = ravengfx_get("HDR_PEAK"), *wv = ravengfx_get("HDR_PAPERWHITE");
    float peak = pv && atof(pv) > 100 ? (float)atof(pv) : 1000, paper = wv && atof(wv) >= 40 ? (float)atof(wv) : 203;
    float P = peak / paper; if (P < 1) P = 1;
    const char *nl = strchr(v, '\n'); if (!nl) return NULL;
    const char *sv = ravengfx_get("HDR_HIGHLIGHTS");
    float k = sv ? (float)atof(sv) : 0.5f; if (k < 0) k = 0; if (k > 1) k = 1;
    char def[96]; snprintf(def, sizeof def, "#define RAVEN_HDR_P %.4f\n#define RAVEN_HDR_GAIN %.3f\n", P, 2 * k);
    size_t n = strlen(src) + sizeof tonemap_hdr + sizeof def + 16;
    char *o = malloc(n), *p = o;
    memcpy(p, src, nl + 1 - src); p += nl + 1 - src;
    p += sprintf(p, "%s", def);
    memcpy(p, nl + 1, a - nl - 1); p += a - nl - 1;
    p += sprintf(p, "%s", tonemap_hdr);
    strcpy(p, b);
    static const char cl[] = "pow( clamp(base.rgb, 0, 1), gamma_vec )", nc[] = "pow(max(base.rgb,vec3(0)),gamma_vec)";
    char *c = strstr(p, cl);
    if (c) { memset(c, ' ', sizeof cl - 1); memcpy(c, nc, sizeof nc - 1); }
    else fprintf(stderr, "[hdr] colour-correction clamp not found, highlights stay clipped\n");
    fprintf(stderr, "[hdr] native HDR tonemap installed in the game's colour-correction shader (peak %.0f / paper %.0f nits = %.2fx white)\n", peak, paper, P);
    return o;
}
// graphics.ini DUMP_SHADERS = 1: append every shader source the game compiles to shader_dump.txt (debug).
static void w_glShaderSource(GLuint sh, GLsizei n, const char *const *str, const GLint *len){
    const char *dv = ravengfx_get("DUMP_SHADERS"); int dump = dv && atoi(dv);
    size_t tot = 0;
    for (GLsizei i = 0; i < n; i++) tot += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    char *all = malloc(tot + 1), *p = all;
    if (!all) { r_glShaderSource(sh, n, str, len); return; }
    for (GLsizei i = 0; i < n; i++) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(p, str[i], l); p += l; }
    *p = 0;
    int fixes = raven_glsl_compat(all);
    if (fixes) fprintf(stderr, "[glsl-compat] shader %u: modernized %d legacy sampler call(s)\n", sh, fixes);
    if (dump) { FILE *f = fopen("/pm/g7/dump/shader_dump.txt", "ab"); if (f) { fprintf(f, "==== shader %u\n%s\n", sh, all); fclose(f); } }
    char *hd = ravenhdr_on() ? rewrite_tonemap(all) : NULL;
    const char *one = hd ? hd : all;
    r_glShaderSource(sh, 1, &one, NULL);
    free(hd); free(all);
}

struct ent { const char *n; void **real; void *wrap; };
#define E(name) { #name, (void **)&r_##name, (void *)w_##name }
static struct ent ents[] = {
    E(glBindFramebuffer), E(glUseProgram), E(glUniformMatrix4fv), E(glUniform1i), E(glDrawElements),
    E(glDrawArrays), E(glDrawElementsInstanced), E(glDrawRangeElements), E(glBlitFramebuffer),
    E(glClear), E(glViewport), E(glDrawBuffers), E(glBindTexture), E(glActiveTexture), E(glClearColor), E(glClearBufferfv),
    E(glTexParameteri), E(glTexParameterf), E(glTexParameteriv), E(glTexParameterfv), E(glDeleteTextures),
    E(glTexImage2D), E(glCompressedTexImage2D), E(glShaderSource), E(glDrawBuffer), E(glReadBuffer),
};

static double trace_at = -1; static int installed, done;

void ravengl_trace_frame(double t)     // called from the present hook, before the real swap
{
    if (!installed) {
        installed = 1;
        const char *e = getenv("RAVENGFX_GLTRACE");
        fsr = ravenfsr_wanted(); fg = !getenv("RAVENGFX_NOGLHOOK") && ravenfg_wanted();
        // Shader compatibility also needs the source hook when enhancements are off.
        if (getenv("RAVENGFX_NOGLHOOK") && !e) return;
        trace_at = e ? atof(e) : -1;
        q_fbattach = gpa("glGetNamedFramebufferAttachmentParameteriv"); q_texlevel = gpa("glGetTextureLevelParameteriv");
        q_rbparam = gpa("glGetNamedRenderbufferParameteriv"); q_progiv = gpa("glGetProgramiv");
        q_activeuni = gpa("glGetActiveUniform"); q_uniloc = gpa("glGetUniformLocation");
        const unsigned NE = sizeof ents / sizeof *ents;
        uint64_t ra[64], rb[64];
        for (unsigned k = 0; k < NE; k++) { *ents[k].real = gpa(ents[k].n); ra[k] = (uint64_t)*ents[k].real; rb[k] = (uint64_t)dlsym(RTLD_DEFAULT, ents[k].n); }
        FILE *m = fopen("/proc/self/maps", "r"); char line[512];
        int total = 0;
        while (m && fgets(line, sizeof line, m)) {
            uint64_t a, b; char perm[5];
            if (sscanf(line, "%lx-%lx %4s", &a, &b, perm) != 3 || perm[0] != 'r' || perm[1] != 'w' || a >= 0x10000000UL) continue;
            for (uint64_t *q = (uint64_t *)a; q < (uint64_t *)b; q++) {
                if (*q < 0x10000000UL) continue;
                for (unsigned k = 0; k < NE; k++)
                    if (*q == ra[k] || (rb[k] && *q == rb[k])) {
                        fprintf(stderr, "[gltrace] %s slot %p%s\n", ents[k].n, (void *)q, *q == ra[k] ? "" : " (dlsym addr)");
                        *ents[k].real = (void *)*q; *q = (uint64_t)ents[k].wrap; total++; break;
                    }
            }
        }
        if (m) fclose(m);
        fprintf(stderr, "[gltrace] %d slots patched, trace at %.0fs\n", total, trace_at);
        return;
    }
    if (fsr) ravenfsr_frame_end();
    if (trace_at < 0 || done) return;
    if (tracing) { seg_end(); fprintf(stderr, "[gltrace] ==== end of frame, %ld draws\n", ndraw); tracing = 0; done = 1; return; }
    if (t >= trace_at) { tracing = 1; ndraw = 0; fprintf(stderr, "[gltrace] ==== frame start (fbo %u prog %u)\n", cur_fbo, cur_prog); }
}
