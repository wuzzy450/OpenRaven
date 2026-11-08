// Game-only video for the recorder (C:\Users\...\HaloRecorder): each presented game frame (before the
// F1 overlay and before HDR output) is scaled on the GPU to 1080 lines, read back asynchronously and
// written to the named pipe \\.\pipe\openraven_video as raw BGRA (bottom-up rows). The recorder's ffmpeg
// reads that pipe, so it can only ever see the game's own picture, never the desktop or other windows.
// The frame size is written to <game folder>\ravenrec.txt ("W H") when a client connects.
// Nothing is copied while no recorder is connected. RAVEN_NOREC=1 turns the pipe off.
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"

void *gl_resolve(const char *name);


#define NB 3
static HANDLE pipe_h = INVALID_HANDLE_VALUE, thr, ready_ev;
static volatile LONG connected, busy;
static unsigned char *buf; static int W, H; static size_t fsize;

#define GLF(ret, n, args) static ret (APIENTRY *n) args;
GLF(void, gGenFramebuffers, (GLsizei, GLuint *)) GLF(void, gBindFramebuffer, (GLenum, GLuint))
GLF(void, gFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GLF(void, gBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
GLF(void, gGenTextures, (GLsizei, GLuint *)) GLF(void, gBindTexture, (GLenum, GLuint))
GLF(void, gTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
GLF(void, gGetIntegerv, (GLenum, GLint *)) GLF(GLboolean, gIsEnabled, (GLenum))
GLF(void, gEnable, (GLenum)) GLF(void, gDisable, (GLenum)) GLF(void, gReadBuffer, (GLenum))
GLF(void, gGenBuffers, (GLsizei, GLuint *)) GLF(void, gBindBuffer, (GLenum, GLuint))
GLF(void, gBufferData, (GLenum, GLsizeiptr, const void *, GLenum))
GLF(void *, gMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) GLF(GLboolean, gUnmapBuffer, (GLenum))
GLF(void, gReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) GLF(void, gPixelStorei, (GLenum, GLint))
GLF(GLsync, gFenceSync, (GLenum, GLbitfield)) GLF(GLenum, gClientWaitSync, (GLsync, GLbitfield, GLuint64))
GLF(void, gDeleteSync, (GLsync))

static DWORD WINAPI writer(void *a) {
    (void)a;
    for (;;) {
        if (!ConnectNamedPipe(pipe_h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) { Sleep(500); continue; }
        raven_log("[rec] recorder connected (%dx%d)\n", W, H);
        InterlockedExchange(&connected, 1);
        for (;;) {
            WaitForSingleObject(ready_ev, INFINITE);
            DWORD n = 0;
            BOOL ok = WriteFile(pipe_h, buf, (DWORD)fsize, &n, NULL);
            InterlockedExchange(&busy, 0);
            if (!ok || n != fsize) break;
        }
        InterlockedExchange(&connected, 0);
        DisconnectNamedPipe(pipe_h);
        raven_log("[rec] recorder disconnected\n");
    }
    return 0;
}

static int init(int sw, int sh) {
    if (raven_env("RAVEN_NOREC")) return 0;
#define L(v, n) if (!(v = (__typeof__(v))gl_resolve(n))) return 0;
    L(gGenFramebuffers, "glGenFramebuffers") L(gBindFramebuffer, "glBindFramebuffer") L(gFramebufferTexture2D, "glFramebufferTexture2D")
    L(gBlitFramebuffer, "glBlitFramebuffer") L(gGenTextures, "glGenTextures") L(gBindTexture, "glBindTexture")
    L(gTexImage2D, "glTexImage2D") L(gGetIntegerv, "glGetIntegerv") L(gIsEnabled, "glIsEnabled") L(gEnable, "glEnable")
    L(gDisable, "glDisable") L(gReadBuffer, "glReadBuffer") L(gGenBuffers, "glGenBuffers") L(gBindBuffer, "glBindBuffer")
    L(gBufferData, "glBufferData") L(gMapBufferRange, "glMapBufferRange") L(gUnmapBuffer, "glUnmapBuffer")
    L(gReadPixels, "glReadPixels") L(gPixelStorei, "glPixelStorei") L(gFenceSync, "glFenceSync")
    L(gClientWaitSync, "glClientWaitSync") L(gDeleteSync, "glDeleteSync")
#undef L
    H = sh > 1080 ? 1080 : sh; W = (int)((long long)sw * H / sh); W -= W % 2; H -= H % 2;
    fsize = (size_t)W * H * 4;
    buf = malloc(fsize);
    ready_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    pipe_h = CreateNamedPipeA("\\\\.\\pipe\\openraven_video", PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT,
                              1, (DWORD)fsize, 0, 0, NULL);
    if (pipe_h == INVALID_HANDLE_VALUE) { raven_log("[rec] pipe failed (%lu)\n", GetLastError()); return 0; }
    // frame size for the recorder's ffmpeg (it must know it before it opens the pipe)
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s/ravenrec.txt", g_root[0] ? g_root : ".");
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%d %d\n", W, H); fclose(f); }
    thr = CreateThread(NULL, 0, writer, NULL, 0, NULL);
    raven_log("[rec] game-only video pipe ready (%dx%d)\n", W, H);
    return 1;
}

// Called once per frame from ravengfx_frame (game picture in the back buffer, overlay not drawn yet).
void ravenrec_frame(int sw, int sh) {
    static int st = 0;   // 0 = not started, 1 = ok, -1 = off
    static GLuint fbo_s, fbo_d, tex, pbo[NB]; static GLsync fence[NB]; static int slot, inflight[NB];
    static LARGE_INTEGER qf, last;
    if (st < 0 || sw <= 0 || sh <= 0) return;
    if (!st) { st = init(sw, sh) ? 1 : -1; if (st < 0) return; }
    if (!connected) { for (int i = 0; i < NB; i++) inflight[i] = 0; return; }
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    if (!tex) {
        GLint ot; gGetIntegerv(GL_TEXTURE_BINDING_2D, &ot);
        gGenTextures(1, &tex); gBindTexture(GL_TEXTURE_2D, tex);
        gTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        gBindTexture(GL_TEXTURE_2D, ot);
        gGenFramebuffers(1, &fbo_s); gGenFramebuffers(1, &fbo_d); gGenBuffers(NB, pbo);
        GLint opb; gGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &opb);
        for (int i = 0; i < NB; i++) { gBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]); gBufferData(GL_PIXEL_PACK_BUFFER, fsize, NULL, GL_STREAM_READ); }
        gBindBuffer(GL_PIXEL_PACK_BUFFER, opb);
    }
    GLint rf, df, opb, pa, rb; GLboolean sci = gIsEnabled(GL_SCISSOR_TEST), srgb = gIsEnabled(GL_FRAMEBUFFER_SRGB);
    gGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rf); gGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &df);
    gGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &opb); gGetIntegerv(GL_PACK_ALIGNMENT, &pa);
    if (sci) gDisable(GL_SCISSOR_TEST);
    if (srgb) gDisable(GL_FRAMEBUFFER_SRGB);
    // 1) hand the oldest finished readback to the writer thread (drop it if the pipe is still busy)
    for (int k = 0; k < NB; k++) {
        int i = (slot + 1 + k) % NB;
        if (!inflight[i]) continue;
        if (gClientWaitSync(fence[i], 0, 0) == GL_TIMEOUT_EXPIRED) continue;
        gDeleteSync(fence[i]); inflight[i] = 0;
        if (!InterlockedCompareExchange(&busy, 1, 0)) {
            gBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]);
            void *m = gMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, fsize, GL_MAP_READ_BIT);
            if (m) { memcpy(buf, m, fsize); gUnmapBuffer(GL_PIXEL_PACK_BUFFER); SetEvent(ready_ev); }
            else InterlockedExchange(&busy, 0);
        }
    }
    // 2) at most 60 copies per second (the arcade game runs at 60): scale the back buffer and start an async readback
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!last.QuadPart || (now.QuadPart - last.QuadPart) * 62 >= qf.QuadPart) {
        if (!inflight[slot]) {
            last = now;
            gBindFramebuffer(GL_READ_FRAMEBUFFER, 0); gGetIntegerv(GL_READ_BUFFER, &rb); gReadBuffer(GL_BACK);
            gBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_d); gFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            gBlitFramebuffer(0, 0, sw, sh, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            gReadBuffer(rb);
            gBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_d);
            gBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]); gPixelStorei(GL_PACK_ALIGNMENT, 4);
            gReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
            fence[slot] = gFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); inflight[slot] = 1;
            slot = (slot + 1) % NB;
        }
    }
    gBindBuffer(GL_PIXEL_PACK_BUFFER, opb); gPixelStorei(GL_PACK_ALIGNMENT, pa);
    gBindFramebuffer(GL_READ_FRAMEBUFFER, rf); gBindFramebuffer(GL_DRAW_FRAMEBUFFER, df);
    if (sci) gEnable(GL_SCISSOR_TEST);
    if (srgb) gEnable(GL_FRAMEBUFFER_SRGB);
    (void)fbo_s;
}
