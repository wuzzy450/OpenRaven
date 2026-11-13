// Native HDR10 output for the Windows build (graphics.ini HDR = 1, restart to switch).
// The game draws into a 16-bit float screen framebuffer (gl_hooks.c) and keeps its own highlights
// above 1.0. Here the picture is gamma-decoded (2.2), capped at the display peak, scaled so
// 1.0 = paper-white nits, converted BT.709 -> BT.2020 and PQ-encoded.
// OpenGL cannot present HDR10 itself, so once HDR is turned on we present through a DXGI flip
// swapchain (R10G10B10A2, colour space G2084/P2020). GL draws into a D3D11 texture shared with
// WGL_NV_DX_interop, D3D copies it to the back buffer and presents. Turning HDR off afterwards keeps
// the DXGI path but outputs plain SDR (sRGB colour space).
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"

void *gl_resolve(const char *name);
extern int ravenhdr_float_fb;
unsigned ravengl_screen_fbo(void);          // gl_hooks.c: the game's screen when native HDR is on, else 0
void ravengl_screen_size(int *w, int *h);   // shim_glx.c: back buffer is RGBA16F, the game's tonemap keeps highlights
const char *ravengfx_get(const char *key);

#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002
static HANDLE (WINAPI *wDXOpen)(void *);
static void *(WINAPI *wDXReg)(HANDLE, void *, GLuint, GLenum, GLenum);
static BOOL (WINAPI *wDXUnreg)(HANDLE, HANDLE);
static BOOL (WINAPI *wDXLock)(HANDLE, GLint, HANDLE *);
static BOOL (WINAPI *wDXUnlock)(HANDLE, GLint, HANDLE *);

static int st = 0;            // 0 = not started, 1 = running, -1 = failed (GL presents as before)
static ID3D11Device *dev; static ID3D11DeviceContext *ctx;
static IDXGISwapChain1 *sc; static IDXGISwapChain3 *sc3;
static ID3D11Texture2D *dtex; static HANDLE hdev, hobj;
static GLuint gtex, srct, fb_src, fb_dst, prog, vao;
static int W, H, cs_hdr = -1;
static float out_peak = 1000;  // display peak from DXGI (nits)
static int on, vsync = 1; static float paper = 203, peak = 0;

#define GLF(ret, n, args) static ret (APIENTRY *n) args;
GLF(void, gGenFramebuffers, (GLsizei, GLuint *)) GLF(void, gBindFramebuffer, (GLenum, GLuint))
GLF(void, gFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GLF(void, gBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
GLF(GLuint, gCreateShader, (GLenum)) GLF(void, gShaderSource, (GLuint, GLsizei, const char *const *, const GLint *))
GLF(void, gCompileShader, (GLuint)) GLF(GLuint, gCreateProgram, (void)) GLF(void, gAttachShader, (GLuint, GLuint))
GLF(void, gLinkProgram, (GLuint)) GLF(void, gGetProgramiv, (GLuint, GLenum, GLint *))
GLF(void, gGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *)) GLF(void, gUseProgram, (GLuint))
GLF(GLint, gGetUniformLocation, (GLuint, const char *)) GLF(void, gUniform1i, (GLint, GLint))
GLF(void, gUniform1f, (GLint, GLfloat)) GLF(void, gGenVertexArrays, (GLsizei, GLuint *))
GLF(void, gBindVertexArray, (GLuint)) GLF(void, gActiveTexture, (GLenum)) GLF(void, gBindSampler, (GLuint, GLuint))
GLF(void, gGenTextures, (GLsizei, GLuint *)) GLF(void, gDeleteTextures, (GLsizei, const GLuint *))
GLF(void, gBindTexture, (GLenum, GLuint)) GLF(void, gTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
GLF(void, gTexParameteri, (GLenum, GLenum, GLint)) GLF(void, gGetIntegerv, (GLenum, GLint *))
GLF(GLboolean, gIsEnabled, (GLenum)) GLF(void, gEnable, (GLenum)) GLF(void, gDisable, (GLenum))
GLF(void, gViewport, (GLint, GLint, GLsizei, GLsizei)) GLF(void, gDrawArrays, (GLenum, GLint, GLsizei))
GLF(void, gReadBuffer, (GLenum)) GLF(void, gReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) GLF(void, gColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) GLF(void, gClearBufferfv, (GLenum, GLint, const GLfloat *))

static const char *vs = "#version 330 core\nvoid main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *fs =
"#version 330 core\n"
"uniform sampler2D S; uniform int hdr, h; uniform float paper, peak;\n"
"out vec4 o;\n"
"float avg(vec3 c){ return (c.r + c.g + c.b) / 3.0; }\n"
"vec3 pq(vec3 c){ const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"
"  vec3 y = pow(max(c, 0.0), vec3(m1)); return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2)); }\n"
"void main(){\n"
"  ivec2 q = ivec2(gl_FragCoord.x, h - 1 - int(gl_FragCoord.y));\n"   // D3D rows are top-down
"  vec3 c = texelFetch(S, q, 0).rgb;\n"
"  if (hdr == 0) { o = vec4(c, 1.0); return; }\n"
"  vec3 L = pow(max(c, 0.0), vec3(2.2));\n"
"  L = min(L, vec3(max(peak / paper, 1.0)));\n"   // the game's own highlights (1.0 = paper white), up to the peak
"  L = mat3(0.627402, 0.069095, 0.016394, 0.329292, 0.919544, 0.088028, 0.043306, 0.011360, 0.895578) * L;\n"
"  o = vec4(clamp(pq(L * paper / 10000.0), 0.0, 1.0), 1.0);\n"
"}\n";

static void read_settings(void) {
    const char *e = raven_env("RAVEN_HDR"), *v;
    if (!e) e = ravengfx_get("HDR");
    on = e && atoi(e) > 0;
    v = ravengfx_get("HDR_PAPERWHITE"); paper = v && atof(v) >= 40 ? (float)atof(v) : 203;
    v = ravengfx_get("HDR_PEAK"); peak = v ? (float)atof(v) : 0;
    v = ravengfx_get("VSYNC"); vsync = !v || strcmp(v, "0") != 0;
}

static int load_gl(void) {
#define L(v, n) if (!(v = (__typeof__(v))gl_resolve(n))) return 0;
    L(gGenFramebuffers, "glGenFramebuffers") L(gBindFramebuffer, "glBindFramebuffer") L(gFramebufferTexture2D, "glFramebufferTexture2D")
    L(gBlitFramebuffer, "glBlitFramebuffer") L(gCreateShader, "glCreateShader") L(gShaderSource, "glShaderSource")
    L(gCompileShader, "glCompileShader") L(gCreateProgram, "glCreateProgram") L(gAttachShader, "glAttachShader")
    L(gLinkProgram, "glLinkProgram") L(gGetProgramiv, "glGetProgramiv") L(gGetProgramInfoLog, "glGetProgramInfoLog")
    L(gUseProgram, "glUseProgram") L(gGetUniformLocation, "glGetUniformLocation") L(gUniform1i, "glUniform1i")
    L(gUniform1f, "glUniform1f") L(gGenVertexArrays, "glGenVertexArrays") L(gBindVertexArray, "glBindVertexArray")
    L(gActiveTexture, "glActiveTexture") L(gBindSampler, "glBindSampler") L(gGenTextures, "glGenTextures")
    L(gDeleteTextures, "glDeleteTextures") L(gBindTexture, "glBindTexture") L(gTexImage2D, "glTexImage2D")
    gReadPixels = (__typeof__(gReadPixels))gl_resolve("glReadPixels");
    gClearBufferfv = (__typeof__(gClearBufferfv))gl_resolve("glClearBufferfv");
    L(gTexParameteri, "glTexParameteri") L(gGetIntegerv, "glGetIntegerv") L(gIsEnabled, "glIsEnabled")
    L(gEnable, "glEnable") L(gDisable, "glDisable") L(gViewport, "glViewport") L(gDrawArrays, "glDrawArrays")
    L(gReadBuffer, "glReadBuffer") L(gColorMask, "glColorMask")
#undef L
    wDXOpen = (void *)wglGetProcAddress("wglDXOpenDeviceNV");
    wDXReg = (void *)wglGetProcAddress("wglDXRegisterObjectNV");
    wDXUnreg = (void *)wglGetProcAddress("wglDXUnregisterObjectNV");
    wDXLock = (void *)wglGetProcAddress("wglDXLockObjectsNV");
    wDXUnlock = (void *)wglGetProcAddress("wglDXUnlockObjectsNV");
    if (!wDXOpen || !wDXReg || !wDXUnreg || !wDXLock || !wDXUnlock) { raven_log("[hdr] driver has no WGL_NV_DX_interop, HDR off\n"); return 0; }
    return 1;
}

static void query_output(void) {
    IDXGIOutput *out = NULL; IDXGIOutput6 *o6 = NULL;
    if (FAILED(IDXGISwapChain1_GetContainingOutput(sc, &out)) || !out) return;
    if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput6, (void **)&o6)) && o6) {
        DXGI_OUTPUT_DESC1 d;
        if (SUCCEEDED(IDXGIOutput6_GetDesc1(o6, &d))) {
            cs_hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            if (d.MaxLuminance > 100) out_peak = d.MaxLuminance;
            raven_log("[hdr] display: Windows HDR %s, peak %.0f nits, max full-frame %.0f nits\n",
                      cs_hdr ? "on" : "OFF (turn on 'Use HDR' in Windows display settings)", d.MaxLuminance, d.MaxFullFrameLuminance);
        }
        IDXGIOutput6_Release(o6);
    }
    IDXGIOutput_Release(out);
}

static int make_target(int w, int h) {
    if (hobj) { wDXUnreg(hdev, hobj); hobj = NULL; }
    if (dtex) { ID3D11Texture2D_Release(dtex); dtex = NULL; }
    if (gtex) { gDeleteTextures(1, &gtex); gtex = 0; }
    if (srct) { gDeleteTextures(1, &srct); srct = 0; }
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &dtex))) { raven_log("[hdr] CreateTexture2D failed\n"); return 0; }
    gGenTextures(1, &gtex);
    hobj = wDXReg(hdev, dtex, gtex, GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!hobj) { raven_log("[hdr] wglDXRegisterObjectNV failed (%lu)\n", GetLastError()); return 0; }
    GLint ot; gGetIntegerv(GL_TEXTURE_BINDING_2D, &ot);
    gGenTextures(1, &srct); gBindTexture(GL_TEXTURE_2D, srct);
    gTexImage2D(GL_TEXTURE_2D, 0, ravenhdr_float_fb ? GL_RGBA16F : GL_RGBA8, w, h, 0, GL_RGBA, ravenhdr_float_fb ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, NULL);
    gTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); gTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gBindTexture(GL_TEXTURE_2D, ot);
    W = w; H = h;
    return 1;
}

static int start(HWND hw, int w, int h) {
    if (!load_gl()) return 0;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
        raven_log("[hdr] D3D11CreateDevice failed\n"); return 0;
    }
    IDXGIFactory2 *f = NULL;
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory2, (void **)&f))) { raven_log("[hdr] CreateDXGIFactory2 failed\n"); return 0; }
    DXGI_SWAP_CHAIN_DESC1 d = {0};
    d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_R10G10B10A2_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(f, (IUnknown *)dev, hw, &d, NULL, NULL, &sc);
    if (FAILED(hr)) { raven_log("[hdr] CreateSwapChainForHwnd failed 0x%08lx\n", (unsigned long)hr); IDXGIFactory2_Release(f); return 0; }
    IDXGIFactory2_MakeWindowAssociation(f, hw, DXGI_MWA_NO_ALT_ENTER);
    IDXGIFactory2_Release(f);
    if (FAILED(IDXGISwapChain1_QueryInterface(sc, &IID_IDXGISwapChain3, (void **)&sc3))) { raven_log("[hdr] no IDXGISwapChain3\n"); return 0; }
    query_output();
    if (!(hdev = wDXOpen(dev))) { raven_log("[hdr] wglDXOpenDeviceNV failed\n"); return 0; }
    GLuint v = gCreateShader(GL_VERTEX_SHADER), p = gCreateShader(GL_FRAGMENT_SHADER);
    gShaderSource(v, 1, &vs, NULL); gCompileShader(v); gShaderSource(p, 1, &fs, NULL); gCompileShader(p);
    prog = gCreateProgram(); gAttachShader(prog, v); gAttachShader(prog, p); gLinkProgram(prog);
    GLint ok = 0; gGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char lg[1024]; gGetProgramInfoLog(prog, sizeof lg, NULL, lg); raven_log("[hdr] shader: %s\n", lg); return 0; }
    GLint ov; gGetIntegerv(GL_VERTEX_ARRAY_BINDING, &ov); gGenVertexArrays(1, &vao); gBindVertexArray(ov);
    gGenFramebuffers(1, &fb_src); gGenFramebuffers(1, &fb_dst);
    if (!make_target(w, h)) return 0;
    raven_log("[hdr] HDR10 present path ready (%dx%d, DXGI flip + NV_DX_interop)\n", w, h);
    return 1;
}

static void set_colorspace(int hdr) {
    static int cur = -1;
    if (hdr == cur) return;
    cur = hdr;
    HRESULT r = IDXGISwapChain3_SetColorSpace1(sc3, hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    raven_log("[hdr] output %s%s\n", hdr ? "HDR10 (PQ, BT.2020)" : "SDR", FAILED(r) ? " - SetColorSpace1 failed" : "");
}

static int present(HWND hw);
// Called instead of SwapBuffers. Returns 1 when it presented, 0 = caller does SwapBuffers as before.
// If the game draws into the native-HDR screen framebuffer but we cannot present (no HDR path),
// copy it to the window first so plain SwapBuffers still shows the picture.
int ravenhdr_present(HWND hw) {
    if (present(hw)) return 1;
    unsigned sfb = ravengl_screen_fbo();
    if (sfb && gBlitFramebuffer) {
        GLint rf, df, sc_on = gIsEnabled(GL_SCISSOR_TEST); int sw, sh;
        gGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rf); gGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &df);
        ravengl_screen_size(&sw, &sh); gDisable(GL_SCISSOR_TEST);
        gBindFramebuffer(GL_READ_FRAMEBUFFER, sfb); gBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        gBlitFramebuffer(0, 0, sw, sh, 0, 0, sw, sh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gBindFramebuffer(GL_READ_FRAMEBUFFER, rf); gBindFramebuffer(GL_DRAW_FRAMEBUFFER, df);
        if (sc_on) gEnable(GL_SCISSOR_TEST);
    }
    return 0;
}
static int present(HWND hw) {
    static int n;
    if (n++ % 30 == 0) read_settings();
    if (st < 0 || (!st && !on)) return 0;
    RECT r; GetClientRect(hw, &r);
    int w = r.right, h = r.bottom;
    if (w <= 0 || h <= 0) return 0;
    if (!st) { st = start(hw, w, h) ? 1 : -1; if (st < 0) return 0; }
    if (w != W || h != H) {
        IDXGISwapChain1_ResizeBuffers(sc, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (!make_target(w, h)) { st = -1; return 0; }
    }
    int hdr = on && cs_hdr != 0;
    set_colorspace(hdr);
    // save GL state we touch
    GLint rf, df, pr, va, at, t0, vp[4]; GLint rb;
    gGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rf); gGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &df);
    gGetIntegerv(GL_CURRENT_PROGRAM, &pr); gGetIntegerv(GL_VERTEX_ARRAY_BINDING, &va); gGetIntegerv(GL_ACTIVE_TEXTURE, &at);
    gGetIntegerv(GL_VIEWPORT, vp);
    static const GLenum caps[] = {GL_SCISSOR_TEST, GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_STENCIL_TEST, GL_FRAMEBUFFER_SRGB, GL_RASTERIZER_DISCARD};
    GLboolean en[7]; for (int i = 0; i < 7; i++) { en[i] = gIsEnabled(caps[i]); gDisable(caps[i]); }
    // the game's picture -> srct: from the native-HDR screen framebuffer, else the window's back buffer
    unsigned sfb = ravengl_screen_fbo();
    int sw = w, sh = h;
    if (sfb) ravengl_screen_size(&sw, &sh);
    gBindFramebuffer(GL_READ_FRAMEBUFFER, sfb); gGetIntegerv(GL_READ_BUFFER, &rb); gReadBuffer(sfb ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    gBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_src); gFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srct, 0);
    // Fit the picture into the window keeping its aspect, centred - same placement as the
    // non-HDR present_scaled(). The old 1:1 blit was fine while the picture always equalled
    // the window; with the 4-player 32:9 fullscreen (1080-high picture, 2160-high window) it
    // anchored the picture to the bottom (GL origin is bottom-left) with a black top half.
    extern int raven_view;
    extern int raven_src_w(int);
    extern void raven_fit_rect(int, int, int, int, int *, int *, int *, int *);
    if (sw > 0 && sh > 0 && (sw != w || sh != h)) {
        int dx, dy, dw, dh;
        raven_fit_rect(sw, sh, w, h, &dx, &dy, &dw, &dh);
        static const GLfloat black[4] = {0, 0, 0, 1};
        if (gClearBufferfv) gClearBufferfv(0x1800 /* GL_COLOR */, 0, black);
        gBlitFramebuffer(0, 0, sfb ? raven_src_w(sw) : sw, sh, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else {
        gBlitFramebuffer(0, 0, sw, sh, 0, 0, sw, sh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    gReadBuffer(rb);
    // graphics.ini HDR_STATS = 1: every 300 frames log the brightest pixel in 64 full-width rows and how
    // many go above SDR white (1.0 = paper white), i.e. the highlights SDR would clip.
    static int fr, stats = -1;
    if (stats < 0) { const char *v = ravengfx_get("HDR_STATS"); stats = v && atoi(v) > 0; }
    if (stats && ravenhdr_float_fb && gReadPixels && ++fr % 300 == 0 && w <= 8192) {
        static float *row; if (!row) row = malloc(sizeof(float) * 4 * 8192);
        float mx = 0; long over = 0, tot = 0;
        gBindFramebuffer(GL_READ_FRAMEBUFFER, fb_src);
        gFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srct, 0);
        for (int y = 0; y < 64; y++) {
            gReadPixels(0, h * y / 64 + h / 128, w, 1, GL_RGBA, GL_FLOAT, row);
            for (int x = 0; x < w; x++) {
                float m = fmaxf(row[x * 4], fmaxf(row[x * 4 + 1], row[x * 4 + 2]));
                if (m > mx) mx = m;
                if (m > 1.001f) over++;
                tot++;
            }
        }
        gFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        raven_log("[hdr] frame %d: brightest %.2fx white (~%.0f nits), %.2f%% of pixels above SDR white\n",
                  fr, mx, powf(mx, 2.2f) * paper, tot ? 100.0 * over / tot : 0.0);
    }
    // srct -> shared D3D texture through the shader
    wDXLock(hdev, 1, &hobj);
    gBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_dst); gFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gtex, 0);
    gViewport(0, 0, w, h); gColorMask(1, 1, 1, 1);
    gUseProgram(prog); gBindVertexArray(vao);
    gActiveTexture(GL_TEXTURE0); gGetIntegerv(GL_TEXTURE_BINDING_2D, &t0); gBindSampler(0, 0); gBindTexture(GL_TEXTURE_2D, srct);
    gUniform1i(gGetUniformLocation(prog, "S"), 0); gUniform1i(gGetUniformLocation(prog, "hdr"), hdr);
    gUniform1i(gGetUniformLocation(prog, "h"), h);
    gUniform1f(gGetUniformLocation(prog, "paper"), paper);
    gUniform1f(gGetUniformLocation(prog, "peak"), peak > 0 ? peak : out_peak);
    gDrawArrays(GL_TRIANGLES, 0, 3);
    gFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    wDXUnlock(hdev, 1, &hobj);
    // restore
    gBindTexture(GL_TEXTURE_2D, t0); gActiveTexture(at); gBindVertexArray(va); gUseProgram(pr);
    gBindFramebuffer(GL_READ_FRAMEBUFFER, rf); gBindFramebuffer(GL_DRAW_FRAMEBUFFER, df);
    gViewport(vp[0], vp[1], vp[2], vp[3]);
    for (int i = 0; i < 7; i++) if (en[i]) gEnable(caps[i]);
    // D3D: copy to the back buffer and present
    ID3D11Texture2D *bb = NULL;
    if (SUCCEEDED(IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb)) && bb) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)bb, (ID3D11Resource *)dtex);
        ID3D11Texture2D_Release(bb);
    }
    IDXGISwapChain1_Present(sc, vsync, 0);
    return 1;
}
