// GL calls the graphics features need to see (Windows version of the FSR part of ravengltrace.c).
// shim_lookup checks this table before the generated gl_thunks.c, so the game gets these wrappers
// from dlsym and glXGetProcAddress. With FSR 2 off they only forward (plus texture-unit tracking).
//   glUseProgram / glUniformMatrix4fv -> ravenfsr_use_program / ravenfsr_matrix (jitter, projection)
//   glDraw* -> ravenfsr_draw (skip the engine's TAA pass, run FSR 2 instead)
//   glActiveTexture / glBindTexture -> which texture is on which unit; tonemap gets the FSR output
#include <math.h>
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
static void vp_draw(void);
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
    if (n >= 1 && m) { extern float g_last_m[16]; extern int g_last_tr; memcpy(g_last_m, m, 64); g_last_tr = tr; }
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
// Hybrid 4-player mission view (raven_view 2, ravengfx_win.c): the 2D layer (fixed-function
// draws, program 0, to the screen at full size) is authored for 32:9. Draw it into the middle
// 32:9 band of the 16:9 screen so HUDs and crosshairs keep their shape and line up with the
// vert+ 3D view (whose middle band is exactly the cabinet's view).
extern int raven_view;
extern volatile int raven_shadow_seen;
extern int ravengfx_res_w, ravengfx_res_h;
typedef void (APIENTRY *t_vp2d)(GLint, GLint, GLsizei, GLsizei);
static GLint g_vp[4];
static int g_screen_draws;   // draws to the screen this frame (reset in vptrace_frame_end)
static int band2d_begin(void) {
    if (raven_view < 1 || raven_view > 4) return 0;
    static long nd, nf, np, nv, ok;
    if (++nd % 20000 == 0)
        raven_log("[band2d] draws %ld: fbo!=0 %ld, 3D picture %ld, viewport not full %ld, banded %ld (last vp %d %d %d %d prog %u fbo %u)\n",
                  nd, nf, np, nv, ok, g_vp[0], g_vp[1], g_vp[2], g_vp[3], cur_prog, cur_fbo);
    if (cur_fbo != 0) { nf++; return 0; }
    // the first two screen draws each frame are the game's 3D picture (composite + post pass):
    // full screen; everything after them is the 2D layer
    // Some frames have only ONE picture draw: the 2nd screen draw is then already the first HUD
    // item (a join-panel portrait, program 36 - the game's 2D program) and must not be kept full
    // screen, or that portrait is stretched. So the picture ends at the first program-36 draw.
    if (cur_prog == 36) g_screen_draws = 99;
    if (++g_screen_draws <= 2) {
        static int nl;
        if (nl++ < 6) raven_log("[band2d] screen draw %d uses program %u (kept full screen)\n", g_screen_draws, cur_prog);
        np++; return 0;
    }
    int W = ravengfx_res_w, H = ravengfx_res_h;
    if (g_vp[0] != 0 || g_vp[1] != 0 || g_vp[2] != W || g_vp[3] != H) { nv++; return 0; }
    ok++;
    int bh = (int)((long long)W * 9 / 32);
    if (bh >= H) return 0;
    return 1;
}
// Where one 2D item goes (each item moves as a whole, nothing is cut):
//  menu view            - viewport twice as wide: the left cabinet screen fills the screen;
//  mission select view  - the middle 32:9 band;
//  mission view         - the item's position on the cabinet screen (its box through the matrix
//                         the game set for it) decides: near the top -> pinned to the top edge,
//                         near the bottom -> pinned to the bottom edge, full height (damage
//                         tint) -> stretched over the whole screen, else (crosshairs, prompts)
//                         the middle band, where it lines up with the 3D view. Crosshairs
//                         (square ~124-unit quads) always stay in the middle so aim matches.
float g_last_m[16]; int g_last_tr;
volatile int raven_place_req;
static int g_dm_first, g_dm_count, g_dm_indexed;
static int draw_bbox(float *x0, float *y0, float *x1, float *y1);
static GLboolean g_sc_on; static GLint g_sc[4];
static float hud_top = -2, hud_bot;
static int g_bbox_ok;
static int place_class_c(float *cc) {   // 0 follow centre (*cc), 1 top, 2 bottom, 3 full screen
    float x0, y0, x1, y1;
    *cc = 0;
    g_bbox_ok = draw_bbox(&x0, &y0, &x1, &y1);
    if (!g_bbox_ok) {
        if (raven_place_req) raven_log("[place] prog %u n %d box unreadable (vbo/indexed)\n", cur_prog, g_dm_count);
        return 0;
    }
    float w = x1 - x0, h = y1 - y0;
    int xhair = w > 100 && w < 150 && h > 100 && h < 150 && w / h > 0.9f && w / h < 1.1f;
    const float *m = g_last_m;
    float lo = 1e9f, hi = -1e9f;
    for (int c = 0; c < 4; c++) {
        float x = c & 1 ? x1 : x0, y = c & 2 ? y1 : y0, cy, cw;
        if (!g_last_tr) { cy = m[1] * x + m[5] * y + m[13]; cw = m[3] * x + m[7] * y + m[15]; }
        else            { cy = m[4] * x + m[5] * y + m[7];  cw = m[12] * x + m[13] * y + m[15]; }
        if (cw > 1e-6f || cw < -1e-6f) cy /= cw;
        if (cy < lo) lo = cy; if (cy > hi) hi = cy;
    }
    if (hud_top < -1) {
        const char *e = raven_env("RAVEN_HUDSTRIP");   // ndc thresholds: top,bottom (default 0.6,-0.55)
        hud_top = 0.6f; hud_bot = -0.55f;
        if (e) sscanf(e, "%f,%f", &hud_top, &hud_bot);
    }
    float c = (lo + hi) / 2;
    {   // RAVEN_PLACELOG=1: every 30th mission frame (40 times), each item's box and placement
        static int on = -1, dumps; static long last = -1; long vp_frame_now(void);
        if (on < 0) on = raven_env("RAVEN_PLACELOG") != NULL;
        long f = vp_frame_now();
        static long req_fr = -1;   // F12 (shim_glx.c) asks for one frame's dump
        if (raven_place_req && req_fr < 0) req_fr = f + 1;
        if (req_fr >= 0 && f > req_fr) { req_fr = -1; raven_place_req = 0; }
        if (req_fr >= 0 && f == req_fr) {
            if (f != last) { last = f; raven_log("[place] F12 frame %ld view %d\n", f, raven_view); }
            raven_log("[place] prog %u n %d box %.0f,%.0f..%.0f,%.0f ndc y %.2f..%.2f\n",
                      cur_prog, g_dm_count, x0, y0, x1, y1, lo, hi);
        } else
        if (on && raven_view == 2 && dumps < 40 && f % 30 == 0) {
            if (f != last) { last = f; dumps++; raven_log("[place] frame %ld\n", f); }
            raven_log("[place] prog %u n %d box %.0f,%.0f..%.0f,%.0f ndc y %.2f..%.2f tr %d m %.4f %.4f %.4f %.4f\n",
                      cur_prog, g_dm_count, x0, y0, x1, y1, lo, hi, g_last_tr, m[1], m[5], m[13], m[15]);
        }
    }
    // full-screen only for tall items centred on the screen (damage tint); a tall item sitting
    // low/high (a join panel's animated piece) keeps its size and is pinned like the HUD bars
    if (hi - lo > 1.5f && c > -0.3f && c < 0.3f) return 3;
    *cc = c < -1 ? -1 : c > 1 ? 1 : c;
    if (xhair) return 0;   // crosshairs follow the aim over the whole screen
    int cls = c > hud_top ? 1 : c < hud_bot ? 2 : 0;
    return cls;
}
static int band2d_piece(int k) {   // always 1 (draw)
    (void)k;
    REAL(glViewport, t_vp2d);
    int W = ravengfx_res_w, H = ravengfx_res_h, bh = (int)((long long)W * 9 / 32), t = (H - bh) / 2;
    glGetBooleanv(GL_SCISSOR_TEST, &g_sc_on);
    glGetIntegerv(GL_SCISSOR_BOX, g_sc);
    if (raven_view == 1) { r_glViewport(0, 0, 2 * W, H); return 1; }
    if (raven_view == 4) {   // the cabinet picture (any viewport inside it) into the middle band
        r_glViewport(g_vp[0], t + (int)((long long)g_vp[1] * bh / H), g_vp[2], (int)((long long)g_vp[3] * bh / H));
        return 1;
    }
    float c;
    if (raven_view == 3) {   // movie: overlay text ("Press RELOAD to skip") once, like the menu
        if (place_class_c(&c) != 3 && g_bbox_ok) r_glViewport(0, 0, 2 * W, H);
        return 1;
    }
    // mission: the game aims through the vert+ camera, so the aim (0..1) spans the WHOLE screen
    // height. Move each item so its centre lands where the same spot of the cabinet screen lands
    // on the full screen: y = t + c * (H - bh) / 2 (c = item centre, -1..1). Crosshairs then sit
    // exactly over the aim point and HUD bars at c = +-1 end up pinned to the edges.
    // HUD bars near the top/bottom are pinned to the edges; full-height items (damage tint)
    // cover the whole screen.
    switch (place_class_c(&c)) {
    case 1: r_glViewport(0, H - bh, W, bh); break;
    case 2: r_glViewport(0, 0, W, bh); break;
    case 3: r_glViewport(0, 0, W, H); break;
    default: {
        float d = c * (H - bh) / 2;
        r_glViewport(0, t + (int)(d < 0 ? d - 0.5f : d + 0.5f), W, bh);
    }
    }
    return 1;
}
static void band2d_end(int on) {
    if (!on) return;
    REAL(glViewport, t_vp2d);
    REAL(glScissor, t_vp2d);
    r_glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
    (void)r_glScissor; (void)g_sc_on;
}

// RAVEN_HUDDUMP=1: log one mission frame's 2D draws (program, count, first vertices) - debug.

// Bounding box of a 2D draw's positions (attribute 0, client-side float arrays only - the
// game's HUD uses those). Returns 0 when the data is not readable that way.
static const void *g_dm_idx; static GLenum g_dm_itype;
static int draw_bbox(float *x0, float *y0, float *x1, float *y1) {
    typedef void (APIENTRY *gva_t)(GLuint, GLenum, GLint *);
    typedef void (APIENTRY *gvp_t)(GLuint, GLenum, void **);
    static gva_t gva; static gvp_t gvp;
    if (!gva) { gva = (gva_t)gl_resolve("glGetVertexAttribiv"); gvp = (gvp_t)gl_resolve("glGetVertexAttribPointerv"); }
    GLint en = 0, buf = 1, sz = 0, ty = 0, st = 0; void *ptr = NULL;
    gva(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
    if (!en) return 0;
    gva(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
    gva(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &sz); gva(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &ty);
    gva(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &st);
    gvp(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
    if (ty != GL_FLOAT || sz < 2 || g_dm_count <= 0 || g_dm_count > 4096) return 0;
    if (!buf && !ptr) return 0;
    int stride = st ? st : sz * 4;
    // vertex/index data may live in GPU buffers (e.g. the Shade turret cockpit frame): read
    // those back with glGetBufferSubData
    typedef void (APIENTRY *gbs_t)(GLenum, GLintptr, GLsizeiptr, void *);
    typedef void (APIENTRY *bb_t)(GLenum, GLuint);
    static gbs_t gbs; static bb_t bb;
    if (!gbs) { gbs = (gbs_t)gl_resolve("glGetBufferSubData"); bb = (bb_t)gl_resolve("glBindBuffer"); }
    GLint eb = 0, pab = 0;
    if (g_dm_indexed) glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
    if (g_dm_indexed && !eb && !g_dm_idx) return 0;
    if ((buf || eb) && (!gbs || !bb)) return 0;
    static unsigned idxbuf[4096];
    if (g_dm_indexed) {
        int isz = g_dm_itype == GL_UNSIGNED_SHORT ? 2 : g_dm_itype == GL_UNSIGNED_INT ? 4 : 1;
        static unsigned char raw[4096 * 4];
        if (eb) gbs(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)g_dm_idx, (GLsizeiptr)g_dm_count * isz, raw);
        else memcpy(raw, g_dm_idx, (size_t)g_dm_count * isz);
        for (int k = 0; k < g_dm_count; k++)
            idxbuf[k] = isz == 2 ? ((unsigned short *)raw)[k] : isz == 4 ? ((unsigned *)raw)[k] : raw[k];
    } else for (int k = 0; k < g_dm_count; k++) idxbuf[k] = (unsigned)(g_dm_first + k);
    if (buf) { glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &pab); bb(GL_ARRAY_BUFFER, (GLuint)buf); }
    *x0 = *y0 = 1e30f; *x1 = *y1 = -1e30f;
    for (int k = 0; k < g_dm_count; k++) {
        float qv[2]; const float *q = qv;
        size_t off = (size_t)idxbuf[k] * stride;
        if (buf) gbs(GL_ARRAY_BUFFER, (GLintptr)ptr + (GLintptr)off, 8, qv);
        else q = (const float *)((const char *)ptr + off);
        if (q[0] < *x0) *x0 = q[0]; if (q[0] > *x1) *x1 = q[0];
        if (q[1] < *y0) *y0 = q[1]; if (q[1] > *y1) *y1 = q[1];
    }
    if (buf) bb(GL_ARRAY_BUFFER, (GLuint)pab);
    return 1;
}
long vp_frame_now(void);
static void hud_dump(void) {
    static int on = -1, frames, done;
    static long fr_seen = -1;
    if (on < 0) on = raven_env("RAVEN_HUDDUMP") != NULL;
    if (!on || done || raven_view != 2) return;
    long f = vp_frame_now();
    if (f != fr_seen) { fr_seen = f; if (++frames == 1200) raven_log("[huddump] frame %ld\n", f); if (frames > 1200) done = 1; }
    if (frames != 1200) return;
    typedef void (APIENTRY *gva_t)(GLuint, GLenum, GLint *);
    typedef void (APIENTRY *gvp_t)(GLuint, GLenum, void **);
    typedef void (APIENTRY *gbs_t)(GLenum, GLintptr, GLsizeiptr, void *);
    typedef void (APIENTRY *bb_t)(GLenum, GLuint);
    gva_t gva = (gva_t)gl_resolve("glGetVertexAttribiv");
    gvp_t gvp = (gvp_t)gl_resolve("glGetVertexAttribPointerv");
    gbs_t gbs = (gbs_t)gl_resolve("glGetBufferSubData");
    bb_t bb = (bb_t)gl_resolve("glBindBuffer");
    char line[700]; int o = snprintf(line, sizeof line, "[huddump] prog %u %s count %d:", cur_prog, g_dm_indexed ? "idx" : "arr", g_dm_count);
    for (int a = 0; a < 3; a++) {
        GLint en = 0, buf = 0, sz = 0, ty = 0, st = 0; void *ptr = NULL;
        gva(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
        if (!en) continue;
        gva(a, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf); gva(a, GL_VERTEX_ATTRIB_ARRAY_SIZE, &sz);
        gva(a, GL_VERTEX_ATTRIB_ARRAY_TYPE, &ty); gva(a, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &st);
        gvp(a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
        o += snprintf(line + o, sizeof line - o, " a%d(buf %d sz %d ty %x st %d off %p)", a, buf, sz, ty, st, ptr);
        if (a == 0 && ty == GL_FLOAT && sz >= 2 && buf && !g_dm_indexed) {
            int stride = st ? st : sz * 4, n = g_dm_count < 6 ? g_dm_count : 6;
            float v[4]; GLint pb = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &pb);
            bb(GL_ARRAY_BUFFER, buf);
            for (int k = 0; k < n && o < 650; k++) {
                gbs(GL_ARRAY_BUFFER, (GLintptr)ptr + (GLintptr)(g_dm_first + k) * stride, 8, v);
                o += snprintf(line + o, sizeof line - o, " (%.1f,%.1f)", v[0], v[1]);
            }
            bb(GL_ARRAY_BUFFER, pb);
        }
    }
    float bx0, by0, bx1, by1;
    if (draw_bbox(&bx0, &by0, &bx1, &by1)) o += snprintf(line + o, sizeof line - o, " BBOX %.1f %.1f .. %.1f %.1f", bx0, by0, bx1, by1);
    raven_log("%s\n", line);
}
#define BAND2D(call) do { int bd_ = band2d_begin(); if (bd_) hud_dump(); \
    if (bd_) { for (int k_ = 0; k_ < bd_; k_++) { if (band2d_piece(k_)) call; } band2d_end(1); } else call; } while (0)
static SV void h_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) {
    REAL(glDrawElements, t_drawel);
    fgdraw(); vp_draw();
    if (fsr() && ravenfsr_draw()) return;
    g_dm_count = c; g_dm_indexed = 1; g_dm_idx = i; g_dm_itype = t;
    BAND2D(r_glDrawElements(m, c, t, i));
}
static SV void h_glDrawArrays(GLenum m, GLint f, GLsizei c) {
    REAL(glDrawArrays, t_drawarr);
    fgdraw(); vp_draw();
    if (fsr() && ravenfsr_draw()) return;
    g_dm_first = f; g_dm_count = c; g_dm_indexed = 0;
    BAND2D(r_glDrawArrays(m, f, c));
}
static SV void h_glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n) {
    REAL(glDrawElementsInstanced, PFNGLDRAWELEMENTSINSTANCEDPROC);
    fgdraw(); vp_draw();
    if (fsr() && ravenfsr_draw()) return;
    g_dm_count = c; g_dm_indexed = 1; g_dm_idx = i; g_dm_itype = t;
    BAND2D(r_glDrawElementsInstanced(m, c, t, i, n));
}
static SV void h_glDrawRangeElements(GLenum m, GLuint a, GLuint b, GLsizei c, GLenum t, const void *i) {
    REAL(glDrawRangeElements, PFNGLDRAWRANGEELEMENTSPROC);
    fgdraw(); vp_draw();
    if (fsr() && ravenfsr_draw()) return;
    g_dm_count = c; g_dm_indexed = 1; g_dm_idx = i; g_dm_itype = t;
    BAND2D(r_glDrawRangeElements(m, a, b, c, t, i));
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
// RAVEN_VPTRACE=n: log frame n's framebuffer binds, viewports and draw counts (how the game lays out a frame).
static int vp_trace = -1, vp_on, vp_draws; static long vp_frame;
static void vp_flush(void) { if (vp_on && vp_draws) { raven_log("[vptrace]   %d draws\n", vp_draws); vp_draws = 0; } }
static void vp_draw(void) { if (vp_on) vp_draws++; }
static long g_frames_all;
long vp_frame_now(void) { return g_frames_all; }
void vptrace_frame_end(void) {
    g_screen_draws = 0; g_frames_all++;
    static long vp_list[16]; static int vp_n;
    if (vp_trace < 0) {   // RAVEN_VPTRACE=n or n,m,... (up to 16 frames)
        const char *e = raven_env("RAVEN_VPTRACE");
        vp_trace = e ? atoi(e) : 0;
        while (e && *e && vp_n < 16) { vp_list[vp_n++] = strtol(e, (char **)&e, 10); if (*e == ',') e++; else break; }
        if (vp_trace) raven_log("[vptrace] tracing %d frame(s), first %ld\n", vp_n, vp_list[0]);
    }
    if (!vp_trace) return;
    vp_flush();
    vp_frame++;
    vp_on = 0;
    for (int i = 0; i < vp_n; i++) if (vp_list[i] == vp_frame) vp_on = 1;
    if (vp_on) raven_log("[vptrace] frame %ld\n", vp_frame);
}
typedef void (APIENTRY *t_viewport)(GLint, GLint, GLsizei, GLsizei);
static SV void h_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    REAL(glViewport, t_viewport);
    if (vp_on) { vp_flush(); raven_log("[vptrace] viewport %d %d %d %d (fbo %u, program %u)\n", x, y, w, h, cur_fbo, cur_prog); }
    g_vp[0] = x; g_vp[1] = y; g_vp[2] = w; g_vp[3] = h;
    if (w == 8192 && h == 8192) raven_shadow_seen = 1;   // mission shadow map
    r_glViewport(x, y, w, h);
}
static SV void h_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    REAL(glScissor, t_viewport);
    if (vp_on) { vp_flush(); raven_log("[vptrace] scissor %d %d %d %d\n", x, y, w, h); }
    r_glScissor(x, y, w, h);
}
static SV void h_glBindFramebuffer(GLenum t, GLuint f) {
    if (vp_on) { vp_flush(); raven_log("[vptrace] bind fbo %u (%s)\n", f, t == GL_READ_FRAMEBUFFER ? "read" : "draw"); }
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
    vptrace_frame_end();
}

const shim_entry shims_glhook[] = {
    {"glUseProgram", (void *)h_glUseProgram},
    {"glViewport", (void *)h_glViewport},
    {"glScissor", (void *)h_glScissor},
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
