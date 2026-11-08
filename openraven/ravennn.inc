// ravennn.inc - neural detail pass v2 (included by ravenfg.c, shares its GL helpers).
// NEURAL = 0|1 (live), NEURAL_STRENGTH = 0..1.5, NEURAL_STABLE = 0..0.9 (temporal smoothing of the
// generated detail, default 0.6). Runs on the finished scene right before the HUD (after FSR 2 and
// tonemap), i.e. on display colours, like the training frames.
// Network (neural/train2.py, weights in neural/raven_nn_weights.h): 2x2 pixel-unshuffle, NN_LAYERS
// 3x3 convs (ReLU between), pixel-shuffle residual. All layers run at half resolution; shaders are
// generated at startup with the weights written in as constants (4-wide matrix steps).
// Final pass: brightness guard (residual minus its 5x5 mean; output within the input's 3x3 range
// +-25%, and 0..1, as in training), then temporal stabilising: the residual is blended with last
// frame's residual, moved by the engine's motion vectors, where the input looks the same as last
// frame (so generated texture sticks to surfaces instead of shimmering).
// Torch row 0 = image top, so passes index in "torch" rows (flipped from GL).
#include "neural/raven_nn_weights.h"
#ifdef NN_UNET
#include "ravennn_vk.inc"   // was ravennn5.inc (GL network, session 82-83)
#else
static int nn_ok = -1; static float nn_stable = 0.6f;
#define NN_MAXG 8
static GLuint nn_prog[NN_LAYERS + 1], nn_fb[3], nn_h[3][NN_MAXG], nn_in, nn_pin, nn_res[2], nn_out; static int nn_w, nn_h_, nn_ri, nn_have_prev;
G(void, gDrawBuffers, (int, const GLenum *)) G(void, gUniform2i, (GLint, GLint, GLint))

static char *nn_src; static size_t nn_len, nn_cap;
static void ap(const char *f, ...) { va_list v; va_start(v, f); char b[8192]; int n = vsnprintf(b, sizeof b, f, v); va_end(v);
    if (nn_len + n + 1 > nn_cap) { nn_cap = (nn_cap + n + 1) * 2; nn_src = realloc(nn_src, nn_cap); } memcpy(nn_src + nn_len, b, n + 1); nn_len += n; }
static const char *NN_COMMON = "#version 330 core\nuniform ivec2 full; uniform float str;\n"
    "ivec2 hf(){ return (full + 1) / 2; }\n"
    "bool okh(int x, int y){ ivec2 h = hf(); return x >= 0 && y >= 0 && x < h.x && y < h.y; }\n";
#define WL(l, o, i, k) nn_W[l][((o) * nn_cin[l] + (i)) * 9 + (k)]

static void gen_layer(int l)
{
    int gi = nn_cin[l] / 4, go = nn_cout[l] / 4, last = l == NN_LAYERS - 1;
    nn_len = 0; ap("%s", NN_COMMON);
    if (l == 0) ap("uniform sampler2D X;\nvec3 px(int x, int y){ return (x < full.x && y < full.y) ? texelFetch(X, ivec2(x, full.y - 1 - y), 0).rgb : vec3(0.0); }\n");
    else for (int q = 0; q < gi; q++) ap("uniform sampler2D H%d;\n", q);
    for (int g = 0; g < go; g++) ap("layout(location=%d) out vec4 o%d;\n", g, g);
    ap("void main(){\n  ivec2 g = ivec2(gl_FragCoord.xy); int Xh = g.x, Yh = hf().y - 1 - g.y; int hx, hy; ivec2 q; vec3 v;\n");
    for (int g = 0; g < go; g++) { const float *b = nn_B[l]; ap("  vec4 a%d = vec4(%.9g, %.9g, %.9g, %.9g);\n", g, b[g*4], b[g*4+1], b[g*4+2], b[g*4+3]); }
    if (l) { ap("  vec4 t0"); for (int q = 1; q < gi; q++) ap(", t%d", q); ap(";\n"); }
    for (int k = 0; k < 9; k++) {
        ap("  hx = Xh + %d; hy = Yh + %d; q = ivec2(hx, hf().y - 1 - hy);\n  if (okh(hx, hy)) {\n", k % 3 - 1, k / 3 - 1);
        if (l == 0) {
            for (int sp = 0; sp < 4; sp++) { ap("    v = px(2 * hx + %d, 2 * hy + %d);\n", sp & 1, sp >> 1);
                for (int g = 0; g < go; g++) { ap("    a%d += mat3x4(", g);
                    for (int c = 0; c < 3; c++) for (int r = 0; r < 4; r++) ap("%.9g%s", WL(0, g*4+r, c*4+sp, k), c == 2 && r == 3 ? "" : ",");
                    ap(") * v;\n"); } }
        } else {
            ap("   "); for (int q = 0; q < gi; q++) ap(" t%d = texelFetch(H%d, q, 0);", q, q); ap("\n");
            for (int g = 0; g < go; g++) for (int qq = 0; qq < gi; qq++) { ap("    a%d += mat4(", g);
                for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) ap("%.9g%s", WL(l, g*4+r, qq*4+c, k), c == 3 && r == 3 ? "" : ",");
                ap(") * t%d;\n", qq); }
        }
        ap("  }\n");
    }
    for (int g = 0; g < go; g++) ap(last ? "  o%d = a%d;\n" : "  o%d = max(a%d, 0.0);\n", g, g);
    ap("}\n");
}
// final pass: residual at full res -> guard -> temporal -> out (colour) + res (residual for next frame)
static void gen_final(void)
{
    nn_len = 0; ap("%s", NN_COMMON);
    ap("uniform sampler2D X, R0, R1, R2, P, PR, V; uniform int hasV, hasP; uniform float stab;\n"
       "layout(location=0) out vec4 oc; layout(location=1) out vec4 orr;\n"
       "vec3 res(ivec2 g){ g = clamp(g, ivec2(0), full - 1); int x = g.x, y = full.y - 1 - g.y; ivec2 q = ivec2(x >> 1, hf().y - 1 - (y >> 1)); int s = (y & 1) * 2 + (x & 1);\n"
       "  return vec3(texelFetch(R0, q, 0)[s], texelFetch(R1, q, 0)[s], texelFetch(R2, q, 0)[s]); }\n"
       "void main(){ ivec2 g = ivec2(gl_FragCoord.xy);\n"
       "  vec3 m = vec3(0.0); for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) m += res(g + ivec2(i, j));\n"
       "  vec3 r = res(g) - m / 25.0;\n"
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
       "  vec3 mg = 0.25 * (hi - lo);\n"
       "  oc = vec4(clamp(clamp(v + str * r, lo - mg, hi + mg), 0.0, 1.0), 1.0); orr = vec4(r, 1.0); }\n");
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
    L(gDrawBuffers, "glDrawBuffers") L(gUniform2i, "glUniform2i")
#undef L
    GLint mdb = 0; gGetIntegerv(0x8824, &mdb);   // MAX_DRAW_BUFFERS
    if (mdb < NN_C / 4) { fprintf(stderr, "[ravennn] needs %d draw buffers (GPU has %d), neural pass off\n", NN_C / 4, mdb); return 0; }
    double t = now_s();
    for (int l = 0; l < NN_LAYERS; l++) { gen_layer(l); if (!(nn_prog[l] = nn_link())) return 0; }
    gen_final(); if (!(nn_prog[NN_LAYERS] = nn_link())) return 0;
    free(nn_src); nn_src = NULL; nn_cap = 0;
    gGenFramebuffers(3, nn_fb);
    fprintf(stderr, "[ravennn] ready: %d layers, %d channels (shaders built in %.1f s)\n", NN_LAYERS, NN_C, now_s() - t);
    return 1;
}
static void nn_tex(GLuint *t, int w, int h, GLint ifmt, GLenum fmt, GLenum type)
{
    gGenTextures(1, t); gBindTexture(0x0DE1, *t); gTexImage2D(0x0DE1, 0, ifmt, w, h, 0, fmt, type, NULL);
    gTexParameteri(0x0DE1, 0x2801, 0x2601); gTexParameteri(0x0DE1, 0x2800, 0x2601); gTexParameteri(0x0DE1, 0x813D, 0);
    gTexParameteri(0x0DE1, 0x2802, 0x812F); gTexParameteri(0x0DE1, 0x2803, 0x812F);
}
static void nn_resize(int w, int h)
{
    if (w == nn_w && h == nn_h_) return;
    if (nn_in) { gDeleteTextures(1, &nn_in); gDeleteTextures(1, &nn_pin); gDeleteTextures(2, nn_res); gDeleteTextures(1, &nn_out);
                 for (int s = 0; s < 3; s++) gDeleteTextures(NN_MAXG, nn_h[s]); }
    memset(nn_h, 0, sizeof nn_h);
    nn_tex(&nn_in, w, h, 0x8058, 0x1908, 0x1401); nn_tex(&nn_pin, w, h, 0x8058, 0x1908, 0x1401); nn_tex(&nn_out, w, h, 0x8058, 0x1908, 0x1401);
    nn_tex(&nn_res[0], w, h, 0x881A, 0x1908, 0x140B); nn_tex(&nn_res[1], w, h, 0x881A, 0x1908, 0x140B);
    for (int s = 0; s < 2; s++) {          // ping-pong hidden layers (NN_C channels)
        gBindFramebuffer(0x8CA9, nn_fb[s]);
        for (int i = 0; i < NN_C / 4; i++) { nn_tex(&nn_h[s][i], (w + 1) / 2, (h + 1) / 2, 0x881A, 0x1908, 0x140B); gFramebufferTexture2D(0x8CA9, 0x8CE0 + i, 0x0DE1, nn_h[s][i], 0); }
    }
    nn_w = w; nn_h_ = h; nn_have_prev = 0;
}
// Run the network on the window's back buffer, in place. Called right before the HUD is drawn.
static void nn_apply(int w, int h)
{
    if (nn_ok < 0) nn_ok = nn_init();
    if (!nn_ok || w < 4 || h < 4) return;
    struct st s; save(&s);
    GLint srgb = gIsEnabled(0x8DB9);
    GLint db[8]; for (int i = 0; i < 8; i++) gGetIntegerv(0x8825 + i, &db[i]);
    gActiveTexture(0x84C0); nn_resize(w, h);
    if (srgb) gDisable(0x8DB9);
    copy_window_sz(nn_in, w, h);
    for (int i = 0; i < 8; i++) gDisable(caps[i]);
    gColorMask(1, 1, 1, 1); gDepthMask(0); gBindVertexArray(vao);
    static const GLenum mrt[8] = {0x8CE0, 0x8CE1, 0x8CE2, 0x8CE3, 0x8CE4, 0x8CE5, 0x8CE6, 0x8CE7};
    int hw = (w + 1) / 2, hh = (h + 1) / 2;
    for (int l = 0; l < NN_LAYERS; l++) {      // layer l reads set (l+1)&1, writes set l&1
        GLuint pr = nn_prog[l]; gUseProgram(pr);
        gUniform2i(gGetUniformLocation(pr, "full"), w, h);
        if (l == 0) { gActiveTexture(0x84C0); gBindSampler(0, 0); gBindTexture(0x0DE1, nn_in); gUniform1i(gGetUniformLocation(pr, "X"), 0); }
        else for (int q = 0; q < nn_cin[l] / 4; q++) { char nm[8]; snprintf(nm, sizeof nm, "H%d", q);
            gActiveTexture(0x84C0 + q); gBindSampler(q, 0); gBindTexture(0x0DE1, nn_h[(l + 1) & 1][q]); gUniform1i(gGetUniformLocation(pr, nm), q); }
        gBindFramebuffer(0x8CA9, nn_fb[l & 1]); gDrawBuffers(nn_cout[l] / 4, mrt); gViewport(0, 0, hw, hh);
        gDrawArrays(0x0004, 0, 3);
    }
    // final: guard + temporal, into nn_out + nn_res[nn_ri]; then copy nn_out to the window
    GLuint pr = nn_prog[NN_LAYERS]; gUseProgram(pr);
    int lastset = (NN_LAYERS - 1) & 1;
    GLuint in[7] = {nn_in, nn_h[lastset][0], nn_h[lastset][1], nn_h[lastset][2], nn_pin, nn_res[nn_ri ^ 1], have_vel ? vel_tex : 0};
    static const char *nm[7] = {"X", "R0", "R1", "R2", "P", "PR", "V"};
    GLint vmax = 0;
    if (have_vel) { gBindTexture(0x0DE1, vel_tex); gGetTexParameteriv(0x0DE1, 0x813D, &vmax); gTexParameteri(0x0DE1, 0x813D, 0); }
    for (int i = 0; i < 7; i++) { gActiveTexture(0x84C0 + i); gBindSampler(i, 0); gBindTexture(0x0DE1, in[i]); gUniform1i(gGetUniformLocation(pr, nm[i]), i); }
    gUniform2i(gGetUniformLocation(pr, "full"), w, h); gUniform1f(gGetUniformLocation(pr, "str"), nn_str);
    gUniform1i(gGetUniformLocation(pr, "hasV"), have_vel); gUniform1i(gGetUniformLocation(pr, "hasP"), nn_have_prev);
    nn_stable = nn_stable_set; gUniform1f(gGetUniformLocation(pr, "stab"), nn_stable);
    gBindFramebuffer(0x8CA9, nn_fb[2]);
    gFramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, nn_out, 0); gFramebufferTexture2D(0x8CA9, 0x8CE1, 0x0DE1, nn_res[nn_ri], 0);
    gDrawBuffers(2, mrt); gViewport(0, 0, w, h);
    gDrawArrays(0x0004, 0, 3);
    for (int i = 5; i < 7; i++) { gActiveTexture(0x84C0 + i); gBindTexture(0x0DE1, 0); }
    if (have_vel) { gActiveTexture(0x84C0); gBindTexture(0x0DE1, vel_tex); gTexParameteri(0x0DE1, 0x813D, vmax); }
    gBindFramebuffer(0x8CA8, nn_fb[2]); gReadBuffer(0x8CE0);
    gBindFramebuffer(0x8CA9, 0); gDrawBuffer(0x0405);
    gBlitFramebuffer(0, 0, w, h, 0, 0, w, h, 0x4000, 0x2600);
    { GLuint t = nn_pin; nn_pin = nn_in; nn_in = t; }    // this frame's input becomes "previous"
    nn_ri ^= 1; nn_have_prev = 1;
    restore(&s);
    if (s.df) gDrawBuffers(8, (const GLenum *)db); else gDrawBuffer(db[0]);
    if (srgb) gEnable(0x8DB9);
}
#endif
