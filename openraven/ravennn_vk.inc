// ravennn_vk.inc - neural detail pass, Vulkan network (vk/ravenvk.c) + the GL final pass. Included by
// ravennn.inc when the weights header defines NN_UNET (U-Net).
// Per frame: window -> nn_in (GL) -> GL-shared Vulkan buffer (RGBA16F) -> Vulkan U-Net (matrix cores where
// the GPU has them, plain kernels otherwise) -> shared buffer: residual + the input's 3x3 contrast in
// alpha -> nn_full (mip levels: 16x16 mean of the residual at level 4, 8x8 contrast mean at level 3) ->
// final pass (guard, pattern guard, temporal stabilising) -> window. GL and Vulkan share memory and
// semaphores (Linux: GL_EXT_memory_object_fd / semaphore_fd; Windows: the _win32 versions), else host staging.
// NEURAL_BACKEND = auto | plain | matrix (restart): auto = matrix cores when present (+ autotune per layer).
#include "vk/ravenvk.h"

static int nn_ok = -1; static float nn_stable = 0.6f;
static GLuint nn_prog_final, nn_fb[2], nn_in, nn_pin, nn_res[2], nn_out, nn_full;
static int nn_w, nn_h_, nn_ri, nn_have_prev;
static GLuint nn_glbuf[2], nn_glmem[2], nn_glsem[2];
static int nn_vk_ok;   // 1 = zero-copy GL/Vulkan sharing, 2 = host-staging fallback
static void *nn_host_buf;
G(void, gDrawBuffers, (int, const GLenum *)) G(void, gUniform2i, (GLint, GLint, GLint)) G(void, gGenerateMipmap, (GLenum))
G(void, gGetTexImage, (GLenum, GLint, GLenum, GLenum, void *)) G(void, gTexSubImage2D, (GLenum, GLint, GLint, GLint, int, int, GLenum, GLenum, const void *))
G(void, gGenBuffers, (int, GLuint *)) G(void, gDeleteBuffers, (int, const GLuint *)) G(void, gBindBuffer, (GLenum, GLuint))
G(void, gPixelStorei, (GLenum, GLint)) G(void, gGetUnsignedBytevEXT, (GLenum, unsigned char *))
G(void, gCreateMemoryObjectsEXT, (int, GLuint *)) G(void, gDeleteMemoryObjectsEXT, (int, const GLuint *))
G(void, gImportMemoryFdEXT, (GLuint, uint64_t, GLenum, GLint)) G(void, gImportMemoryWin32HandleEXT, (GLuint, uint64_t, GLenum, void *)) G(void, gBufferStorageMemEXT, (GLenum, intptr_t, GLuint, uint64_t))
G(void, gGenSemaphoresEXT, (int, GLuint *)) G(void, gDeleteSemaphoresEXT, (int, const GLuint *)) G(void, gImportSemaphoreFdEXT, (GLuint, GLenum, GLint)) G(void, gImportSemaphoreWin32HandleEXT, (GLuint, GLenum, void *))
G(void, gSignalSemaphoreEXT, (GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *))
G(void, gWaitSemaphoreEXT, (GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *))
G(GLenum, gGetError, (void)) G(void, gReadPixels, (GLint, GLint, int, int, GLenum, GLenum, void *))
static int nn_has_ext_interop;

static char *nn_src; static size_t nn_len, nn_cap;
static void ap(const char *f, ...) { va_list v; va_start(v, f); char b[8192]; int n = vsnprintf(b, sizeof b, f, v); va_end(v);
    if (nn_len + n + 1 > nn_cap) { nn_cap = (nn_cap + n + 1) * 2; nn_src = realloc(nn_src, nn_cap); } memcpy(nn_src + nn_len, b, n + 1); nn_len += n; }
#ifndef NN_PROT
#define NN_PROT 0.9f   // networks from before the header recorded it (v5-v7)
#endif
#ifndef NN_PB0
#define NN_PB0 0.12f   // pattern guard: 8x8-mean local contrast where damping starts ...
#define NN_PB1 0.10f   // ... and how much more until it is full
#endif
static void gen_final(void)
{
    nn_len = 0;
    ap("#version 330 core\nuniform ivec2 full; uniform float str;\n"
       "uniform sampler2D X, RF, P, PR, V; uniform int hasV, hasP; uniform float stab, prot;\n"
       "layout(location=0) out vec4 oc; layout(location=1) out vec4 orr;\n"
       "void main(){ ivec2 g = ivec2(gl_FragCoord.xy); ivec2 tq = ivec2(g.x, full.y - 1 - g.y);\n"
       "  vec3 r = texelFetch(RF, tq, 0).rgb - textureLod(RF, (vec2(tq) + 0.5) / vec2(full), 4.0).rgb;\n"
       "  vec3 v = texelFetch(X, g, 0).rgb;\n"
       "  if (hasP != 0 && stab > 0.0) {\n"
       "    vec2 uv = (vec2(g) + 0.5) / vec2(full);\n"
       "    vec2 pv = uv - (hasV != 0 ? texture(V, uv).xy : vec2(0.0));\n"
       "    if (all(greaterThanEqual(pv, vec2(0.0))) && all(lessThanEqual(pv, vec2(1.0)))) {\n"
       "      vec3 pin = texture(P, pv).rgb, pr = texture(PR, pv).rgb;\n"
       "      float d = length(pin - v); float w = stab * (1.0 - smoothstep(0.02, 0.08, d));\n"
       "      r = mix(r, pr, w); } }\n"
       "  vec3 lo = v, hi = v;\n"
       "  for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) { vec3 n = texelFetch(X, clamp(g + ivec2(i, j), ivec2(0), full - 1), 0).rgb; lo = min(lo, n); hi = max(hi, n); }\n"
       "  float busy = textureLod(RF, (vec2(tq) + 0.5) / vec2(full), 3.0).a;\n"
       "  float k = 1.0 - prot * clamp((busy - %.6f) / %.6f, 0.0, 1.0);\n"   // dense patterns (grates): change less
       "  vec3 mg = %.6f * (hi - lo) * k + %.6f;\n"
       "  oc = vec4(clamp(clamp(v + str * k * r, lo - mg, hi + mg), 0.0, 1.0), 1.0); orr = vec4(r, 1.0); }\n", NN_PB0, NN_PB1, NN_GM, NN_GA);
}
static GLuint nn_link(void)
{
    GLuint v = shader(0x8B31, vs_src), f = shader(0x8B30, nn_src), p = gCreateProgram();
    gAttachShader(p, v); gAttachShader(p, f); gLinkProgram(p);
    GLint ok = 0; gGetProgramiv(p, 0x8B82, &ok);
    if (!ok) { char log[2048]; int l = 0; gGetProgramInfoLog(p, sizeof log, &l, log); fprintf(stderr, "[ravennn] link failed: %s\n", log); return 0; }
    return p;
}
static int nn_init(void)
{
#define L(v, n) if (!(v = (__typeof__(v))gpa(n))) { fprintf(stderr, "[ravennn] missing %s, neural pass off\n", n); return 0; }
    L(gDrawBuffers, "glDrawBuffers") L(gUniform2i, "glUniform2i") L(gGenerateMipmap, "glGenerateMipmap")
    L(gGetTexImage, "glGetTexImage") L(gTexSubImage2D, "glTexSubImage2D") L(gGenBuffers, "glGenBuffers")
    L(gDeleteBuffers, "glDeleteBuffers") L(gBindBuffer, "glBindBuffer") L(gPixelStorei, "glPixelStorei") L(gGetError, "glGetError") L(gReadPixels, "glReadPixels")
#undef L
#define O(v, n) v = (__typeof__(v))gpa(n);
    O(gGetUnsignedBytevEXT, "glGetUnsignedBytevEXT") O(gCreateMemoryObjectsEXT, "glCreateMemoryObjectsEXT")
    O(gDeleteMemoryObjectsEXT, "glDeleteMemoryObjectsEXT") O(gImportMemoryFdEXT, "glImportMemoryFdEXT")
    O(gBufferStorageMemEXT, "glBufferStorageMemEXT") O(gGenSemaphoresEXT, "glGenSemaphoresEXT")
    O(gDeleteSemaphoresEXT, "glDeleteSemaphoresEXT") O(gImportSemaphoreFdEXT, "glImportSemaphoreFdEXT")
    O(gSignalSemaphoreEXT, "glSignalSemaphoreEXT") O(gWaitSemaphoreEXT, "glWaitSemaphoreEXT")
    O(gImportMemoryWin32HandleEXT, "glImportMemoryWin32HandleEXT") O(gImportSemaphoreWin32HandleEXT, "glImportSemaphoreWin32HandleEXT")
#undef O
#ifdef _WIN32
    nn_has_ext_interop = gCreateMemoryObjectsEXT && gImportMemoryWin32HandleEXT && gBufferStorageMemEXT && gGenSemaphoresEXT && gImportSemaphoreWin32HandleEXT && gSignalSemaphoreEXT && gWaitSemaphoreEXT;
#else
    nn_has_ext_interop = gCreateMemoryObjectsEXT && gImportMemoryFdEXT && gBufferStorageMemEXT && gGenSemaphoresEXT && gImportSemaphoreFdEXT && gSignalSemaphoreEXT && gWaitSemaphoreEXT;
#endif
    gen_final(); if (!(nn_prog_final = nn_link())) return 0;
    free(nn_src); nn_src = NULL; nn_cap = 0;
    gGenFramebuffers(2, nn_fb);
    return 1;
}
static void nn_tex(GLuint *t, int w, int h, GLint ifmt, GLenum fmt, GLenum type)
{
    gGenTextures(1, t); gBindTexture(0x0DE1, *t); gTexImage2D(0x0DE1, 0, ifmt, w, h, 0, fmt, type, NULL);
    gTexParameteri(0x0DE1, 0x2801, 0x2601); gTexParameteri(0x0DE1, 0x2800, 0x2601); gTexParameteri(0x0DE1, 0x813D, 0);
    gTexParameteri(0x0DE1, 0x2802, 0x812F); gTexParameteri(0x0DE1, 0x2803, 0x812F);
}
// (re)create the Vulkan net and its GL-shared buffers for this frame size
static void nn_vk_free(void)
{
    if (nn_glbuf[0]) { gDeleteBuffers(2, nn_glbuf); gDeleteMemoryObjectsEXT(2, nn_glmem); gDeleteSemaphoresEXT(2, nn_glsem); }
    nn_glbuf[0] = nn_glbuf[1] = 0;
    free(nn_host_buf); nn_host_buf = NULL;
    rvk_destroy(); nn_vk_ok = 0;
}
static const char *nn_acc_desc(const rvk_info *in)   // e.g. " fp16 accumulate (21/21 layers)"
{
    static char b[64];
    if (!in->coopmat) return "";
    if (in->n_acc32 == 0 || in->n_acc16 == 0) snprintf(b, sizeof b, " %s accumulate (%d/%d layers)", in->n_acc16 ? "fp16" : "fp32", in->n_acc16 + in->n_acc32, in->n_conv);
    else snprintf(b, sizeof b, " mixed accumulate (fp16 %d, fp32 %d of %d layers)", in->n_acc16, in->n_acc32, in->n_conv);
    return b;
}
static int nn_vk_setup(int w, int h)
{
    nn_vk_free();
    unsigned char uuid[16] = { 0 };
    if (gGetUnsignedBytevEXT) gGetUnsignedBytevEXT(0x9597, uuid);   // GL_DEVICE_UUID_EXT
    const char *be = ravengfx_get("NEURAL_BACKEND"); if (getenv("RAVENNN_BACKEND")) be = getenv("RAVENNN_BACKEND");
    int mode = be && !strcasecmp(be, "plain") ? 1 : be && !strcasecmp(be, "matrix") ? 2 : 0;
    const char *ac = ravengfx_get("NEURAL_ACCUMULATE"); if (getenv("RAVENNN_ACCUMULATE")) ac = getenv("RAVENNN_ACCUMULATE");
    rvk_acc_pref = ac && !strcasecmp(ac, "fp16") ? 1 : ac && !strcasecmp(ac, "fp32") ? 2 : 0;   // auto = speed test at start
    static const rvk_net net = { NN_LAYERS, nn_src0, nn_mode0, nn_src1, nn_cin, nn_cout, nn_W, nn_B };
    rvk_info in; double t = now_s();
    // 1. zero-copy sharing (GL_EXT_memory_object_fd/_win32 + GL_EXT_semaphore_fd/_win32)
    int try_export = nn_has_ext_interop && !getenv("RAVENNN_HOSTSTAGE");
    if (try_export && rvk_init(&net, w, h, uuid, mode, 1, &in)) {
        while (gGetError()) ;
        gCreateMemoryObjectsEXT(2, nn_glmem); gGenBuffers(2, nn_glbuf); gGenSemaphoresEXT(2, nn_glsem);
        uint64_t sz[2] = { in.in_size, in.out_size };
#ifndef _WIN32
        int fds[2] = { in.in_fd, in.out_fd };
#endif
        for (int i = 0; i < 2; i++) {
#ifdef _WIN32
            gImportMemoryWin32HandleEXT(nn_glmem[i], sz[i], 0x9587, i ? in.out_h : in.in_h);   // GL_HANDLE_TYPE_OPAQUE_WIN32_EXT
#else
            gImportMemoryFdEXT(nn_glmem[i], sz[i], 0x9586, fds[i]);              // GL_HANDLE_TYPE_OPAQUE_FD_EXT (GL owns the fd now)
#endif
            gBindBuffer(0x88EB, nn_glbuf[i]); gBufferStorageMemEXT(0x88EB, (intptr_t)w * h * 8, nn_glmem[i], 0);
        }
        gBindBuffer(0x88EB, 0);
#ifdef _WIN32
        gImportSemaphoreWin32HandleEXT(nn_glsem[0], 0x9587, in.sem_in_h); gImportSemaphoreWin32HandleEXT(nn_glsem[1], 0x9587, in.sem_out_h);
#else
        gImportSemaphoreFdEXT(nn_glsem[0], 0x9586, in.sem_in_fd); gImportSemaphoreFdEXT(nn_glsem[1], 0x9586, in.sem_out_fd);
#endif
        GLenum e = gGetError();
        if (!e) {
            fprintf(stderr, "[ravennn] Vulkan network on %s: %s%s, %d layers, %dx%d, %.2f ms per frame (GPU zero-copy), set-up %.1f s\n",
                    in.gpu, in.coopmat ? "matrix cores" : "plain kernels", nn_acc_desc(&in),
                    NN_LAYERS, w, h, in.ms, now_s() - t);
            nn_vk_ok = 1;
            return 1;
        }
        fprintf(stderr, "[ravennn] GL import of Vulkan memory failed (0x%x), falling back to host-staged Vulkan path\n", e);
        nn_vk_free();
    }
    // 3. Host-staged in-process Vulkan fallback
    t = now_s();
    if (!rvk_init(&net, w, h, uuid, mode, 0, &in)) {
        fprintf(stderr, "[ravennn] Vulkan network off: %s (GPU %s)\n", in.why, in.gpu[0] ? in.gpu : "?");
        rvk_destroy(); return 0;
    }
    nn_host_buf = malloc((size_t)w * h * 8);
    if (!nn_host_buf) { rvk_destroy(); return 0; }
    fprintf(stderr, "[ravennn] Vulkan network on %s: %s%s, %d layers, %dx%d, %.2f ms per frame (host-staged fallback), set-up %.1f s\n",
            in.gpu, in.coopmat ? "matrix cores" : "plain kernels", nn_acc_desc(&in),
            NN_LAYERS, w, h, in.ms, now_s() - t);
    nn_vk_ok = 2;
    return 1;
}
static void nn_resize(int w, int h)
{
    if (w == nn_w && h == nn_h_) return;
    if (nn_in) { gDeleteTextures(1, &nn_in); gDeleteTextures(1, &nn_pin); gDeleteTextures(2, nn_res); gDeleteTextures(1, &nn_out); gDeleteTextures(1, &nn_full); }
    // RGBA16F like the shared buffer, so the read-back into it is a plain copy (no 8-bit -> half conversion)
    nn_tex(&nn_in, w, h, 0x881A, 0x1908, 0x140B); nn_tex(&nn_pin, w, h, 0x881A, 0x1908, 0x140B); nn_tex(&nn_out, w, h, 0x8058, 0x1908, 0x1401);
    nn_tex(&nn_res[0], w, h, 0x881A, 0x1908, 0x140B); nn_tex(&nn_res[1], w, h, 0x881A, 0x1908, 0x140B);
    // full-res residual (+ contrast in alpha) with mip levels 0..4 (level 4 = the 16x16 mean, 3 = 8x8)
    gGenTextures(1, &nn_full); gBindTexture(0x0DE1, nn_full);
    for (int m = 0, mw = w, mh = h; m <= 4; m++, mw = mw > 1 ? mw / 2 : 1, mh = mh > 1 ? mh / 2 : 1) gTexImage2D(0x0DE1, m, 0x881A, mw, mh, 0, 0x1908, 0x140B, NULL);
    gTexParameteri(0x0DE1, 0x2801, 0x2701); gTexParameteri(0x0DE1, 0x2800, 0x2601); gTexParameteri(0x0DE1, 0x813C, 0); gTexParameteri(0x0DE1, 0x813D, 4);
    gTexParameteri(0x0DE1, 0x2802, 0x812F); gTexParameteri(0x0DE1, 0x2803, 0x812F);
    nn_w = w; nn_h_ = h; nn_have_prev = 0;
    nn_vk_setup(w & ~1, h & ~1);
}
// nn_in -> dst (pack buffer offset or host memory), RGBA16F, vw x vh. Odd sizes (the Windows window is
// 1 px taller): drop the bottom row / right column, so GL row y = buffer row y - (h - vh) and the
// top-down output row r = h - 1 - y, which is what the final pass reads.
static void nn_read_in(int w, int h, int vw, int vh, void *dst)
{
    if (vw == w && vh == h) { gBindTexture(0x0DE1, nn_in); gGetTexImage(0x0DE1, 0, 0x1908, 0x140B, dst); return; }
    gBindFramebuffer(0x8CA8, nn_fb[0]); gFramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1, nn_in, 0); gReadBuffer(0x8CE0);
    gReadPixels(0, h - vh, vw, vh, 0x1908, 0x140B, dst);
    gFramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1, 0, 0); gBindFramebuffer(0x8CA8, 0);
}
static void nn_bind(GLuint pr, const char *nm, int unit, GLuint tex)
{
    gActiveTexture(0x84C0 + unit); gBindSampler(unit, 0); gBindTexture(0x0DE1, tex); gUniform1i(gGetUniformLocation(pr, nm), unit);
}
#ifdef _WIN32
// RAVENNN_PAIR_EVERY=N: every N neural frames, save this frame before (nn_in) and after (nn_out) the pass
// as nnpair/NNNNNN_off.ppm / _on.ppm (same frame, HUD not drawn yet), for quality comparisons.
static void nn_save_ppm(GLuint tex, int w, int h, const char *path)
{
    unsigned char *b = malloc((size_t)w * h * 3); if (!b) return;
    gBindFramebuffer(0x8CA8, nn_fb[0]); gFramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1, tex, 0); gReadBuffer(0x8CE0);
    gPixelStorei(0x0D05, 1); gPixelStorei(0x0D02, 0);
    gReadPixels(0, 0, w, h, 0x1907, 0x1401, b);   // RGB, UNSIGNED_BYTE
    gFramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1, 0, 0);
    FILE *f = fopen(path, "wb");
    if (f) { char hd[48]; int hl = snprintf(hd, sizeof hd, "P6\n%d %d\n255\n", w, h); fwrite(hd, 1, hl, f); for (int y = h - 1; y >= 0; y--) fwrite(b + (size_t)y * w * 3, 1, (size_t)w * 3, f); fclose(f); }
    free(b);
}
static void nn_pair(int w, int h)
{
    // RAVENNN_PAIR_KEY=1: F9 toggles saving on/off (every RAVENNN_PAIR_EVERY frames, default 15)
    static int every = -1, n, keymode, on, was;
    if (every < 0) { keymode = getenv("RAVENNN_PAIR_KEY") && atoi(getenv("RAVENNN_PAIR_KEY")) > 0;
        every = getenv("RAVENNN_PAIR_EVERY") ? atoi(getenv("RAVENNN_PAIR_EVERY")) : keymode ? 15 : 0; }
    ++n;
    if (keymode) {
        SHORT ks = GetAsyncKeyState(VK_F9);   // bit 0 = pressed since last call (a tap during a slow save frame)
        int down = (ks & 0x8000) != 0;
        if ((down && !was) || (!down && (ks & 1))) { on = !on; raven_log("[ravennn] F9: pair capture %s (frame %d)\n", on ? "ON" : "OFF", n); MessageBeep(on ? MB_OK : MB_ICONHAND); }
        was = down;
        if (!on) return;
    }
    if (every <= 0 || n % every) return;
    char p[64]; CreateDirectoryA("nnpair", NULL);
    snprintf(p, sizeof p, "nnpair/%06d_off.ppm", n); nn_save_ppm(nn_in, w, h, p);
    snprintf(p, sizeof p, "nnpair/%06d_on.ppm", n); nn_save_ppm(nn_out, w, h, p);
    raven_log("[ravennn] pair %d saved\n", n);
}
#endif
// Run the network on the window's back buffer, in place. Called right before the HUD is drawn.
static void nn_apply(int w, int h)
{
    if (nn_ok < 0) nn_ok = nn_init();
    if (!nn_ok || w < 32 || h < 32) return;
    struct st s; save(&s);
    GLint srgb = gIsEnabled(0x8DB9);
    GLint db[8]; for (int i = 0; i < 8; i++) gGetIntegerv(0x8825 + i, &db[i]);
    gActiveTexture(0x84C0); nn_resize(w, h);
    if (!nn_vk_ok) { restore(&s); return; }
    if (srgb) gDisable(0x8DB9);
    copy_window_sz(nn_in, w, h);
    for (int i = 0; i < 8; i++) gDisable(caps[i]);
    gColorMask(1, 1, 1, 1); gDepthMask(0); gBindVertexArray(vao);
    static const GLenum mrt[8] = {0x8CE0, 0x8CE1, 0x8CE2, 0x8CE3, 0x8CE4, 0x8CE5, 0x8CE6, 0x8CE7};
    int vw = w & ~1, vh = h & ~1;
    // nn_in -> shared input buffer (RGBA16F), Vulkan net, shared output buffer -> nn_full level 0
    GLint pk = 0, upk = 0, pa = 4, ua = 4, prl = 0, url = 0;
    gGetIntegerv(0x88ED, &pk); gGetIntegerv(0x88EF, &upk);                 // PIXEL_PACK / UNPACK_BUFFER_BINDING
    gGetIntegerv(0x0D05, &pa); gGetIntegerv(0x0CF5, &ua); gGetIntegerv(0x0D02, &prl); gGetIntegerv(0x0CF2, &url);
    gPixelStorei(0x0D05, 8); gPixelStorei(0x0CF5, 8); gPixelStorei(0x0D02, vw); gPixelStorei(0x0CF2, vw);
    static int vt = -1; if (vt < 0) vt = getenv("RAVENNN_VKTEST") ? atoi(getenv("RAVENNN_VKTEST")) : 0;   // timing tests: 1 = no Vulkan run, 2 = no copies either
    if (nn_vk_ok == 1) {
        gBindBuffer(0x88EB, nn_glbuf[0]);
        if (vt < 2) nn_read_in(w, h, vw, vh, 0);    // RGBA, HALF_FLOAT
        gBindBuffer(0x88EB, 0);
        if (!vt) {
            gSignalSemaphoreEXT(nn_glsem[0], 1, &nn_glbuf[0], 0, NULL, NULL);
            gFlush();   // the signal must reach the GPU before Vulkan waits on it (else Vulkan stalls until GL flushes at swap)
            rvk_run();
            gWaitSemaphoreEXT(nn_glsem[1], 1, &nn_glbuf[1], 0, NULL, NULL);
        }
        gBindTexture(0x0DE1, nn_full); gBindBuffer(0x88EC, nn_glbuf[1]);
        if (vt < 2) gTexSubImage2D(0x0DE1, 0, 0, 0, vw, vh, 0x1908, 0x140B, 0);
        gBindBuffer(0x88EC, 0); gGenerateMipmap(0x0DE1); gBindTexture(0x0DE1, 0);
    } else if (nn_vk_ok == 2 && nn_host_buf) {
        gBindBuffer(0x88EB, 0); gBindBuffer(0x88EC, 0);
        if (vt < 2) nn_read_in(w, h, vw, vh, nn_host_buf);
        if (!vt) {
            {
                rvk_upload(nn_host_buf);
                rvk_run();
                rvk_download(nn_host_buf);
            }
        }
        gBindTexture(0x0DE1, nn_full);
        if (vt < 2) gTexSubImage2D(0x0DE1, 0, 0, 0, vw, vh, 0x1908, 0x140B, nn_host_buf);
        gGenerateMipmap(0x0DE1); gBindTexture(0x0DE1, 0);
    }
    gPixelStorei(0x0D05, pa); gPixelStorei(0x0CF5, ua); gPixelStorei(0x0D02, prl); gPixelStorei(0x0CF2, url);
    gBindBuffer(0x88EB, pk); gBindBuffer(0x88EC, upk);
    gBindFramebuffer(0x8CA9, nn_fb[0]);
    // final: guard + temporal, into nn_out + nn_res[nn_ri]; then copy nn_out to the window
    GLuint pr = nn_prog_final; gUseProgram(pr);
    GLint vmax = 0;
    if (have_vel) { gBindTexture(0x0DE1, vel_tex); gGetTexParameteriv(0x0DE1, 0x813D, &vmax); gTexParameteri(0x0DE1, 0x813D, 0); }
    nn_bind(pr, "X", 0, nn_in); nn_bind(pr, "RF", 1, nn_full); nn_bind(pr, "P", 2, nn_pin); nn_bind(pr, "PR", 3, nn_res[nn_ri ^ 1]); nn_bind(pr, "V", 4, have_vel ? vel_tex : 0);
    gUniform2i(gGetUniformLocation(pr, "full"), w, h); gUniform1f(gGetUniformLocation(pr, "str"), nn_str);
    gUniform1i(gGetUniformLocation(pr, "hasV"), have_vel); gUniform1i(gGetUniformLocation(pr, "hasP"), nn_have_prev);
    nn_stable = nn_stable_set; gUniform1f(gGetUniformLocation(pr, "stab"), nn_stable); gUniform1f(gGetUniformLocation(pr, "prot"), nn_protect < 0 ? NN_PROT : nn_protect);
    gBindFramebuffer(0x8CA9, nn_fb[1]);
    gFramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, nn_out, 0); gFramebufferTexture2D(0x8CA9, 0x8CE1, 0x0DE1, nn_res[nn_ri], 0);
    gDrawBuffers(2, mrt); gViewport(0, 0, w, h);
    gDrawArrays(0x0004, 0, 3);
    for (int i = 0; i < 5; i++) { gActiveTexture(0x84C0 + i); gBindTexture(0x0DE1, 0); }
    if (have_vel) { gActiveTexture(0x84C0); gBindTexture(0x0DE1, vel_tex); gTexParameteri(0x0DE1, 0x813D, vmax); }
    gBindFramebuffer(0x8CA8, nn_fb[1]); gReadBuffer(0x8CE0);
    gBindFramebuffer(0x8CA9, 0); gDrawBuffer(0x0405);
    gBlitFramebuffer(0, 0, w, h, 0, 0, w, h, 0x4000, 0x2600);
#ifdef _WIN32
    { GLint pa2 = 4, prl2 = 0; gGetIntegerv(0x0D05, &pa2); gGetIntegerv(0x0D02, &prl2); nn_pair(w, h); gPixelStorei(0x0D05, pa2); gPixelStorei(0x0D02, prl2); }
#endif
    { GLuint t = nn_pin; nn_pin = nn_in; nn_in = t; }    // this frame's input becomes "previous"
    nn_ri ^= 1; nn_have_prev = 1;
    restore(&s);
    if (s.df) gDrawBuffers(8, (const GLenum *)db); else gDrawBuffer(db[0]);
    if (srgb) gEnable(0x8DB9);
}
