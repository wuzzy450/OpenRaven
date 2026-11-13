// Browser stream, video (Linux, part of ravengfx.so): the game's own picture (read from the back buffer
// before the F1 overlay) -> H.264 -> fragmented MP4, one fragment per frame -> streamweb.c (rio2pad.so)
// -> the browser's MediaSource player. Only runs while a browser is watching. Same fMP4 as
// win/streamenc_win.c, so the page is shared.
//
// The encoder is the ffmpeg program (fed raw frames through a pipe, H.264 Annex-B back): the game card's
// own encoder first (NVIDIA: h264_nvenc; AMD / Intel: h264_vaapi on that card's /dev/dri/renderD*), then
// the other cards, then software (libx264). One that gives no picture within 3 s of its first frame
// (5 s if it never asked for one) is closed and the next one opened.
//   RAVEN_STREAM_ENCODER=auto|nvidia|amd|intel|software
//   RAVEN_STREAM_HEIGHT=720   RAVEN_STREAM_FPS=60   RAVEN_STREAM_KBPS=8000
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
typedef unsigned GLenum, GLuint, GLbitfield; typedef int GLint, GLsizei; typedef unsigned char GLboolean;
typedef long GLsizeiptr, GLintptr; typedef void *GLsync; typedef unsigned long long GLuint64;

// streamweb.c lives in rio2pad.so: found by name
static int (*sw_active)(void), (*sw_viewers)(void), (*sw_take_key)(void);
static void (*sw_video)(const unsigned char *, size_t, const unsigned char *, size_t, int);

static int W, H, g_fps = 60, g_kbps = 8000;
static unsigned char *g_bgra; static size_t g_fsize;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static volatile int g_ready, g_busy;
static char g_glvendor[160];
static char g_encname[96] = "not started (no browser yet)";
const char *streamenc_status(void) { return g_encname; }

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

// ---------------- fragmented MP4 (as win/streamenc_win.c) ----------------
typedef struct { unsigned char *p; size_t n, c; } bw;
static void put(bw *b, const void *d, size_t n)
{
    if (b->n + n > b->c) { b->c = (b->n + n) * 2 + 4096; b->p = realloc(b->p, b->c); }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void u8(bw *b, unsigned v) { unsigned char c = (unsigned char)v; put(b, &c, 1); }
static void u16(bw *b, unsigned v) { u8(b, v >> 8); u8(b, v); }
static void u32(bw *b, uint32_t v) { u16(b, v >> 16); u16(b, v & 0xffff); }
static void u64(bw *b, uint64_t v) { u32(b, (uint32_t)(v >> 32)); u32(b, (uint32_t)v); }
static size_t box(bw *b, const char *t) { size_t at = b->n; u32(b, 0); put(b, t, 4); return at; }
static void end(bw *b, size_t at) { uint32_t n = (uint32_t)(b->n - at); b->p[at] = n >> 24; b->p[at + 1] = n >> 16; b->p[at + 2] = n >> 8; b->p[at + 3] = n; }
static size_t fbox(bw *b, const char *t, unsigned ver, unsigned flags) { size_t at = box(b, t); u32(b, ver << 24 | flags); return at; }
static void matrix(bw *b) { static const uint32_t m[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000}; for (int i = 0; i < 9; i++) u32(b, m[i]); }

static bw make_init(const unsigned char *sps, int nsps, const unsigned char *pps, int npps)
{
    bw b = {0};
    size_t a = box(&b, "ftyp"); put(&b, "isom", 4); u32(&b, 512); put(&b, "isomiso6avc1mp41", 16); end(&b, a);
    size_t moov = box(&b, "moov");
    a = fbox(&b, "mvhd", 0, 0); u32(&b, 0); u32(&b, 0); u32(&b, 1000); u32(&b, 0); u32(&b, 0x10000); u16(&b, 0x100); u16(&b, 0);
    u32(&b, 0); u32(&b, 0); matrix(&b); for (int i = 0; i < 6; i++) u32(&b, 0); u32(&b, 2); end(&b, a);
    size_t trak = box(&b, "trak");
    a = fbox(&b, "tkhd", 0, 3); u32(&b, 0); u32(&b, 0); u32(&b, 1); u32(&b, 0); u32(&b, 0); u32(&b, 0); u32(&b, 0);
    u16(&b, 0); u16(&b, 0); u16(&b, 0); u16(&b, 0); matrix(&b); u32(&b, (uint32_t)W << 16); u32(&b, (uint32_t)H << 16); end(&b, a);
    size_t mdia = box(&b, "mdia");
    a = fbox(&b, "mdhd", 0, 0); u32(&b, 0); u32(&b, 0); u32(&b, 90000); u32(&b, 0); u16(&b, 0x55c4); u16(&b, 0); end(&b, a);
    a = fbox(&b, "hdlr", 0, 0); u32(&b, 0); put(&b, "vide", 4); u32(&b, 0); u32(&b, 0); u32(&b, 0); put(&b, "Game\0", 5); end(&b, a);
    size_t minf = box(&b, "minf");
    a = fbox(&b, "vmhd", 0, 1); u16(&b, 0); u16(&b, 0); u16(&b, 0); u16(&b, 0); end(&b, a);
    size_t dinf = box(&b, "dinf"); size_t dref = fbox(&b, "dref", 0, 0); u32(&b, 1); a = fbox(&b, "url ", 0, 1); end(&b, a); end(&b, dref); end(&b, dinf);
    size_t stbl = box(&b, "stbl");
    size_t stsd = fbox(&b, "stsd", 0, 0); u32(&b, 1);
    size_t avc1 = box(&b, "avc1"); for (int i = 0; i < 6; i++) u8(&b, 0); u16(&b, 1); for (int i = 0; i < 16; i++) u8(&b, 0);
    u16(&b, W); u16(&b, H); u32(&b, 0x480000); u32(&b, 0x480000); u32(&b, 0); u16(&b, 1); for (int i = 0; i < 32; i++) u8(&b, 0);
    u16(&b, 0x18); u16(&b, 0xffff);
    a = box(&b, "avcC"); u8(&b, 1); u8(&b, sps[1]); u8(&b, sps[2]); u8(&b, sps[3]); u8(&b, 0xff); u8(&b, 0xe1);
    u16(&b, nsps); put(&b, sps, nsps); u8(&b, 1); u16(&b, npps); put(&b, pps, npps); end(&b, a);
    end(&b, avc1); end(&b, stsd);
    a = fbox(&b, "stts", 0, 0); u32(&b, 0); end(&b, a);
    a = fbox(&b, "stsc", 0, 0); u32(&b, 0); end(&b, a);
    a = fbox(&b, "stsz", 0, 0); u32(&b, 0); u32(&b, 0); end(&b, a);
    a = fbox(&b, "stco", 0, 0); u32(&b, 0); end(&b, a);
    end(&b, stbl); end(&b, minf); end(&b, mdia); end(&b, trak);
    size_t mvex = box(&b, "mvex"); a = fbox(&b, "trex", 0, 0); u32(&b, 1); u32(&b, 1); u32(&b, 0); u32(&b, 0); u32(&b, 0); end(&b, a); end(&b, mvex);
    end(&b, moov);
    return b;
}

static bw make_frag(uint32_t seq, uint64_t t90, uint32_t dur90, const unsigned char *avcc, size_t n, int key)
{
    bw b = {0};
    size_t moof = box(&b, "moof");
    size_t a = fbox(&b, "mfhd", 0, 0); u32(&b, seq); end(&b, a);
    size_t traf = box(&b, "traf");
    a = fbox(&b, "tfhd", 0, 0x020000); u32(&b, 1); end(&b, a);
    a = fbox(&b, "tfdt", 1, 0); u64(&b, t90); end(&b, a);
    a = fbox(&b, "trun", 0, 0x000701); u32(&b, 1); size_t off_at = b.n; u32(&b, 0);
    u32(&b, dur90); u32(&b, (uint32_t)n); u32(&b, key ? 0x02000000 : 0x01010000); end(&b, a);
    end(&b, traf); end(&b, moof);
    uint32_t off = (uint32_t)(b.n + 8);
    b.p[off_at] = off >> 24; b.p[off_at + 1] = off >> 16; b.p[off_at + 2] = off >> 8; b.p[off_at + 3] = off;
    a = box(&b, "mdat"); put(&b, avcc, n); end(&b, a);
    return b;
}

// ---------------- encoders ----------------
typedef struct { char name[64]; unsigned vendor; char dev[64]; int sw; } cand_t;
#define MAXC 8
static cand_t g_c[MAXC]; static int g_nc;
static unsigned vendor_of(const char *s)
{
    if (strcasestr(s, "nvidia")) return 0x10de;
    if (strcasestr(s, "amd") || strcasestr(s, "ati ") || strcasestr(s, "radeon")) return 0x1002;
    if (strcasestr(s, "intel")) return 0x8086;
    return 0;
}
static const char *vname(unsigned v) { return v == 0x10de ? "NVIDIA" : v == 0x1002 ? "AMD" : v == 0x8086 ? "Intel" : "other"; }

// Every card in /dev/dri (vendor from sysfs) -> one candidate each; the game's card first, software last.
static void list_encoders(void)
{
    const char *want = getenv("RAVEN_STREAM_ENCODER");
    if (!want || !*want) want = "auto";
    unsigned game = vendor_of(g_glvendor);
    cand_t all[MAXC]; int n = 0;
    DIR *d = opendir("/dev/dri");
    struct dirent *e;
    while (d && (e = readdir(d)) && n < MAXC - 1) {
        if (strncmp(e->d_name, "renderD", 7)) continue;
        char p[300]; unsigned v = 0;
        snprintf(p, sizeof p, "/sys/class/drm/%s/device/vendor", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (fscanf(f, "%x", &v) != 1) v = 0; fclose(f); }
        if (v != 0x10de && v != 0x1002 && v != 0x8086) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (v == 0x10de && all[i].vendor == v) dup = 1;   // NVENC picks the card itself
        if (dup) continue;
        cand_t c = {{0}, v, {0}, 0};
        snprintf(c.dev, sizeof c.dev, "/dev/dri/%s", e->d_name);
        snprintf(c.name, sizeof c.name, "%s %s", vname(v), v == 0x10de ? "NVENC" : "VA-API");
        all[n++] = c;
        fprintf(stderr, "[stream] encoder found: %s (%s)\n", c.name, c.dev);
    }
    if (d) closedir(d);
    g_nc = 0;
    for (int pass = 0; pass < 2; pass++)   // the game's card first
        for (int i = 0; i < n; i++) {
            if ((pass == 0) != (all[i].vendor == game)) continue;
            if (!strcmp(want, "software")) continue;
            if (!strcmp(want, "nvidia") && all[i].vendor != 0x10de) continue;
            if (!strcmp(want, "amd") && all[i].vendor != 0x1002) continue;
            if (!strcmp(want, "intel") && all[i].vendor != 0x8086) continue;
            g_c[g_nc++] = all[i];
        }
    cand_t s = {"software (x264)", 0, "", 1};
    g_c[g_nc++] = s;
    fprintf(stderr, "[stream] game card: %s; encoder order (RAVEN_STREAM_ENCODER=%s):", g_glvendor, want);
    for (int i = 0; i < g_nc; i++) fprintf(stderr, " %d. %s", i + 1, g_c[i].name);
    fprintf(stderr, "\n");
}

static pid_t g_pid; static int g_in = -1, g_out = -1;
static volatile int g_outs;   // pictures this encoder gave (0 = maybe a dud)
static volatile int g_cur = -1;

static void enc_close(void)
{
    if (g_in >= 0) close(g_in);
    if (g_out >= 0) close(g_out);
    g_in = g_out = -1;
    if (g_pid > 0) { kill(g_pid, SIGKILL); waitpid(g_pid, NULL, 0); }
    g_pid = 0;
}

// ffmpeg: raw BGRA frames (bottom-up rows) in, H.264 Annex-B with an AUD before every picture out.
static int enc_open(cand_t *c)
{
    char sz[32], fps[16], br[16], gop[16], buf2[16];
    snprintf(sz, sizeof sz, "%dx%d", W, H); snprintf(fps, sizeof fps, "%d", g_fps);
    snprintf(br, sizeof br, "%dk", g_kbps); snprintf(gop, sizeof gop, "%d", g_fps); snprintf(buf2, sizeof buf2, "%dk", g_kbps / 4);
    const char *av[64]; int n = 0;
    av[n++] = "ffmpeg"; av[n++] = "-hide_banner"; av[n++] = "-loglevel"; av[n++] = "error"; av[n++] = "-nostdin";
    if (c->vendor == 0x1002 || c->vendor == 0x8086) { av[n++] = "-vaapi_device"; av[n++] = c->dev; }
    av[n++] = "-f"; av[n++] = "rawvideo"; av[n++] = "-pix_fmt"; av[n++] = "bgra"; av[n++] = "-s"; av[n++] = sz;
    av[n++] = "-fflags"; av[n++] = "nobuffer"; av[n++] = "-flags"; av[n++] = "low_delay";   // no input queue: each frame goes straight in
    av[n++] = "-probesize"; av[n++] = "32"; av[n++] = "-analyzeduration"; av[n++] = "0";
    av[n++] = "-r"; av[n++] = fps; av[n++] = "-i"; av[n++] = "-";
    if (c->vendor == 0x10de) {
        av[n++] = "-vf"; av[n++] = "vflip,format=nv12";
        av[n++] = "-c:v"; av[n++] = "h264_nvenc"; av[n++] = "-preset"; av[n++] = "p1"; av[n++] = "-tune"; av[n++] = "ull";
        av[n++] = "-zerolatency"; av[n++] = "1"; av[n++] = "-delay"; av[n++] = "0"; av[n++] = "-rc"; av[n++] = "cbr"; av[n++] = "-aud"; av[n++] = "1";
        av[n++] = "-profile:v"; av[n++] = "high";
    } else if (c->vendor) {
        av[n++] = "-vf"; av[n++] = "vflip,format=nv12,hwupload";
        av[n++] = "-c:v"; av[n++] = "h264_vaapi"; av[n++] = "-aud"; av[n++] = "1"; av[n++] = "-rc_mode"; av[n++] = "CBR";
        av[n++] = "-profile:v"; av[n++] = "high";
    } else {
        av[n++] = "-vf"; av[n++] = "vflip,format=yuv420p";
        av[n++] = "-c:v"; av[n++] = "libx264"; av[n++] = "-preset"; av[n++] = "ultrafast"; av[n++] = "-tune"; av[n++] = "zerolatency";
        av[n++] = "-x264-params"; av[n++] = "aud=1";
    }
    av[n++] = "-bf"; av[n++] = "0"; av[n++] = "-g"; av[n++] = gop; av[n++] = "-b:v"; av[n++] = br;
    av[n++] = "-maxrate"; av[n++] = br; av[n++] = "-bufsize"; av[n++] = buf2;
    av[n++] = "-flush_packets"; av[n++] = "1"; av[n++] = "-f"; av[n++] = "h264"; av[n++] = "-"; av[n] = NULL;
    int pin[2], pout[2];
    if (pipe2(pin, O_CLOEXEC) || pipe2(pout, O_CLOEXEC)) return 0;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pin[0], 0);
    posix_spawn_file_actions_adddup2(&fa, pout[1], 1);
    // ffmpeg must not get our preloads (halo.so, rio2pad.so, ravengfx.so)
    static char *env[256]; int ne = 0;
    for (char **e = environ; *e && ne < 254; e++) if (strncmp(*e, "LD_PRELOAD=", 11)) env[ne++] = *e;
    env[ne] = NULL;
    int r = posix_spawnp(&g_pid, "ffmpeg", &fa, NULL, (char **)av, env);
    posix_spawn_file_actions_destroy(&fa);
    close(pin[0]); close(pout[1]);
    if (r) { close(pin[1]); close(pout[0]); g_pid = 0; fprintf(stderr, "[stream] cannot start ffmpeg (%s): is it installed?\n", strerror(r)); return 0; }
    g_in = pin[1]; g_out = pout[0];
    fcntl(g_in, F_SETPIPE_SZ, 1 << 20);
    return 1;
}

// ---- encoder output -> fMP4 fragments (reader thread) ----
// Annex-B -> AVCC (4-byte lengths), pulling out SPS/PPS; drops AUD/SPS/PPS/SEI from the sample.
static size_t to_avcc(const unsigned char *a, size_t n, unsigned char *out, int *key,
                      unsigned char *sps, int *nsps, unsigned char *pps, int *npps)
{
    size_t o = 0, i = 0;
    *key = 0;
    while (i + 3 <= n && !(a[i] == 0 && a[i + 1] == 0 && a[i + 2] == 1)) i++;
    while (i + 3 <= n) {
        size_t s = i + 3, e = s;
        while (e + 3 <= n && !(a[e] == 0 && a[e + 1] == 0 && a[e + 2] == 1)) e++;
        size_t next = e + 3 <= n ? e : n;
        if (e + 3 > n) e = n;
        while (e > s && a[e - 1] == 0) e--;
        size_t len = e - s;
        i = next;
        if (!len) continue;
        int t = a[s] & 31;
        if (t == 7 && len < 256) { memcpy(sps, a + s, len); *nsps = (int)len; }
        else if (t == 8 && len < 256) { memcpy(pps, a + s, len); *npps = (int)len; }
        else if (t != 9 && t != 6) {
            if (t == 5) *key = 1;
            out[o] = (unsigned char)(len >> 24); out[o + 1] = (unsigned char)(len >> 16); out[o + 2] = (unsigned char)(len >> 8); out[o + 3] = (unsigned char)len;
            memcpy(out + o + 4, a + s, len); o += 4 + len;
        }
    }
    return o;
}

static unsigned char g_sps[256], g_pps[256]; static int g_nsps, g_npps;
static bw g_initseg; static uint32_t g_seq;
static void emit(const unsigned char *au, size_t n)
{
    static unsigned char *avcc; static size_t cap;
    if (cap < n + 64) { cap = n * 2 + 65536; avcc = realloc(avcc, cap); }
    int key = 0;
    size_t an = to_avcc(au, n, avcc, &key, g_sps, &g_nsps, g_pps, &g_npps);
    if (g_nsps && g_npps && !g_initseg.p) {
        g_initseg = make_init(g_sps, g_nsps, g_pps, g_npps);
        fprintf(stderr, "[stream] video: H.264 profile %d level %d.%d\n", g_sps[1], g_sps[3] / 10, g_sps[3] % 10);
    }
    if (!g_initseg.p || !an) return;
    uint32_t dur = 90000 / g_fps;
    g_seq++;
    bw f = make_frag(g_seq, (uint64_t)g_seq * dur, dur, avcc, an, key);
    sw_video(g_initseg.p, g_initseg.n, f.p, f.n, key);
    free(f.p);
    g_outs++;
}
// Splits ffmpeg's output into pictures at each access unit delimiter (NAL type 9).
static void *reader(void *arg)
{
    int fd = (int)(intptr_t)arg;
    size_t cap = 4 << 20, n = 0;
    unsigned char *b = malloc(cap);
    for (;;) {
        if (n == cap) { cap *= 2; b = realloc(b, cap); }
        ssize_t r = read(fd, b + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
        // emit every complete picture: from one AUD up to the next one
        for (;;) {
            size_t first = (size_t)-1, second = (size_t)-1;
            for (size_t i = 0; i + 3 < n; i++)
                if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && (b[i + 3] & 31) == 9) {
                    if (first == (size_t)-1) { first = i; i += 3; }
                    else { second = i; break; }
                }
            if (second == (size_t)-1) break;
            emit(b + first, second - first);
            memmove(b, b + second, n - second);
            n -= second;
        }
    }
    free(b);
    close(fd);
    return NULL;
}

// Encoder thread: hands captured frames to ffmpeg; switches encoder when one gives no picture.
static void *enc_thread(void *arg)
{
    (void)arg;
    list_encoders();
    int from = 0;
    for (;;) {
        int ok = 0;
        while (!ok && from < g_nc) {
            if (enc_open(&g_c[from])) { ok = 1; break; }
            from++;
        }
        if (!ok) { snprintf(g_encname, sizeof g_encname, "none works (is ffmpeg installed?)"); fprintf(stderr, "[stream] no video encoder works: stream has no picture\n"); return NULL; }
        g_cur = from; g_outs = 0;
        snprintf(g_encname, sizeof g_encname, "%s", g_c[from].name);
        fprintf(stderr, "[stream] video encoder: %s, %dx%d %d fps %d kbit/s\n", g_c[from].name, W, H, g_fps, g_kbps);
        pthread_t rt;
        pthread_create(&rt, NULL, reader, (void *)(intptr_t)g_out);
        g_out = -1;   // the reader closes it
        double t_open = now_s(), t_first = 0;
        int dud = 0;
        for (;;) {
            pthread_mutex_lock(&g_mx);
            while (!g_ready) {
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 200000000;
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&g_cv, &g_mx, &ts);
                if (!g_ready) break;
            }
            int have = g_ready; g_ready = 0;
            pthread_mutex_unlock(&g_mx);
            if (have) {
                if (!t_first) t_first = now_s();
                if (sw_take_key) sw_take_key();   // keyframes come every second (-g = fps)
                size_t o = 0;
                while (o < g_fsize) {
                    ssize_t w = write(g_in, g_bgra + o, g_fsize - o);
                    if (w <= 0) { if (errno == EINTR) continue; dud = 1; break; }
                    o += (size_t)w;
                }
                __atomic_store_n(&g_busy, 0, __ATOMIC_SEQ_CST);
            }
            if (dud) break;
            double t = now_s();
            if (!g_outs && ((t_first && t - t_first > 3) || (!t_first && t - t_open > 5 && sw_viewers && sw_viewers()))) { dud = 1; break; }
        }
        fprintf(stderr, "[stream] encoder %s gave no picture: trying the next one\n", g_c[from].name);
        int had = g_outs;
        enc_close();
        pthread_join(rt, NULL);
        if (had) { free(g_initseg.p); g_initseg.p = NULL; g_initseg.n = 0; g_nsps = g_npps = 0; }   // new init segment for the next one
        from++;
    }
    return NULL;
}

// ---------------- capture (GL thread, from the present hook) ----------------
#define NB 3
static void *glsym(const char *n)
{
    void *(*gpa)(const char *) = dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    void *f = gpa ? gpa(n) : NULL;
    return f ? f : dlsym(RTLD_DEFAULT, n);
}
#define GLF(ret, n, args) static ret (*n) args;
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
GLF(void, gDeleteSync, (GLsync)) GLF(const unsigned char *, gGetString, (GLenum))

static int cap_init(int sw, int sh)
{
    sw_active = dlsym(RTLD_DEFAULT, "streamweb_active"); sw_viewers = dlsym(RTLD_DEFAULT, "streamweb_viewers");
    sw_take_key = dlsym(RTLD_DEFAULT, "streamweb_take_key_request"); sw_video = dlsym(RTLD_DEFAULT, "streamweb_video");
    if (!sw_active || !sw_viewers || !sw_video) return 0;
#define L(v, n) if (!(v = (__typeof__(v))glsym(n))) return 0;
    L(gGenFramebuffers, "glGenFramebuffers") L(gBindFramebuffer, "glBindFramebuffer") L(gFramebufferTexture2D, "glFramebufferTexture2D")
    L(gBlitFramebuffer, "glBlitFramebuffer") L(gGenTextures, "glGenTextures") L(gBindTexture, "glBindTexture")
    L(gTexImage2D, "glTexImage2D") L(gGetIntegerv, "glGetIntegerv") L(gIsEnabled, "glIsEnabled") L(gEnable, "glEnable")
    L(gDisable, "glDisable") L(gReadBuffer, "glReadBuffer") L(gGenBuffers, "glGenBuffers") L(gBindBuffer, "glBindBuffer")
    L(gBufferData, "glBufferData") L(gMapBufferRange, "glMapBufferRange") L(gUnmapBuffer, "glUnmapBuffer")
    L(gReadPixels, "glReadPixels") L(gPixelStorei, "glPixelStorei") L(gFenceSync, "glFenceSync")
    L(gClientWaitSync, "glClientWaitSync") L(gDeleteSync, "glDeleteSync") L(gGetString, "glGetString")
#undef L
    const char *gv = (const char *)gGetString(0x1F00), *gr = (const char *)gGetString(0x1F01);   // GL_VENDOR, GL_RENDERER
    snprintf(g_glvendor, sizeof g_glvendor, "%s %s", gv ? gv : "", gr ? gr : "");
    const char *e;
    int hh = (e = getenv("RAVEN_STREAM_HEIGHT")) ? atoi(e) : 720;
    if ((e = getenv("RAVEN_STREAM_FPS"))) g_fps = atoi(e);
    if ((e = getenv("RAVEN_STREAM_KBPS"))) g_kbps = atoi(e);
    if (g_fps < 10) g_fps = 10;
    if (g_fps > 60) g_fps = 60;
    if (hh < 240) hh = 240;
    H = sh < hh ? sh : hh; W = (int)((long long)sw * H / sh);
    W &= ~15; H &= ~1;
    g_fsize = (size_t)W * H * 4;
    g_bgra = malloc(g_fsize);
    signal(SIGPIPE, SIG_IGN);   // a dead ffmpeg must not end the game
    pthread_t t;
    pthread_create(&t, NULL, enc_thread, NULL);
    pthread_detach(t);
    return 1;
}

// Called once per presented frame (game picture in the back buffer, no overlay yet).
void streamenc_frame(int sw, int sh)
{
    static int st;   // 0 = not started, 1 = ok, -1 = off
    static GLuint fbo_d, tex, pbo[NB]; static GLsync fence[NB]; static int slot, inflight[NB];
    static double last;
    if (st < 0 || sw <= 0 || sh <= 0) return;
    if (!st) {
        int (*act)(void) = dlsym(RTLD_DEFAULT, "streamweb_active");
        if (!act || !act()) return;
        st = cap_init(sw, sh) ? 1 : -1;
        if (st < 0) return;
    }
    if (!sw_viewers()) { for (int i = 0; i < NB; i++) inflight[i] = 0; return; }
    if (!tex) {
        GLint ot; gGetIntegerv(0x8069, &ot);
        gGenTextures(1, &tex); gBindTexture(0x0DE1, tex);
        gTexImage2D(0x0DE1, 0, 0x8058, W, H, 0, 0x80E1, 0x1401, NULL);   // RGBA8, BGRA
        gBindTexture(0x0DE1, (GLuint)ot);
        gGenFramebuffers(1, &fbo_d); gGenBuffers(NB, pbo);
        GLint opb; gGetIntegerv(0x88ED, &opb);
        for (int i = 0; i < NB; i++) { gBindBuffer(0x88EB, pbo[i]); gBufferData(0x88EB, (GLsizeiptr)g_fsize, NULL, 0x88E9); }
        gBindBuffer(0x88EB, (GLuint)opb);
    }
    GLint rf, df, opb, pa, rb; GLboolean sci = gIsEnabled(0x0C11), srgb = gIsEnabled(0x8DB9);
    gGetIntegerv(0x8CAA, &rf); gGetIntegerv(0x8CA6, &df);
    gGetIntegerv(0x88ED, &opb); gGetIntegerv(0x0D05, &pa);
    if (sci) gDisable(0x0C11);
    if (srgb) gDisable(0x8DB9);
    // finished readbacks -> encoder thread (dropped if it is still busy with the last one)
    for (int k = 0; k < NB; k++) {
        int i = (slot + 1 + k) % NB;
        if (!inflight[i] || gClientWaitSync(fence[i], 0, 0) == 0x911B) continue;   // TIMEOUT_EXPIRED
        gDeleteSync(fence[i]); inflight[i] = 0;
        int z = 0;
        if (__atomic_compare_exchange_n(&g_busy, &z, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            gBindBuffer(0x88EB, pbo[i]);
            void *m = gMapBufferRange(0x88EB, 0, (GLsizeiptr)g_fsize, 0x0001);
            if (m) {
                memcpy(g_bgra, m, g_fsize); gUnmapBuffer(0x88EB);
                pthread_mutex_lock(&g_mx); g_ready = 1; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mx);
            } else __atomic_store_n(&g_busy, 0, __ATOMIC_SEQ_CST);
        }
    }
    double now = now_s();
    if ((!last || (now - last) * (g_fps + 2) >= 1.0) && !inflight[slot]) {
        last = now;
        // Native HDR draws the game into its own float framebuffer, not the window's.
        unsigned ravengl_screen_fbo(void);
        unsigned sfb = ravengl_screen_fbo();
        gBindFramebuffer(0x8CA8, sfb); gGetIntegerv(0x0C02, &rb); gReadBuffer(sfb ? 0x8CE0 : 0x0405);
        gBindFramebuffer(0x8CA9, fbo_d); gFramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, tex, 0);
        gBlitFramebuffer(0, 0, sw, sh, 0, 0, W, H, 0x4000, 0x2601);
        gReadBuffer((GLenum)rb);
        gBindFramebuffer(0x8CA8, fbo_d);
        gBindBuffer(0x88EB, pbo[slot]); gPixelStorei(0x0D05, 4);
        gReadPixels(0, 0, W, H, 0x80E1, 0x1401, NULL);
        fence[slot] = gFenceSync(0x9117, 0); inflight[slot] = 1;
        slot = (slot + 1) % NB;
    }
    gBindBuffer(0x88EB, (GLuint)opb); gPixelStorei(0x0D05, pa);
    gBindFramebuffer(0x8CA8, (GLuint)rf); gBindFramebuffer(0x8CA9, (GLuint)df);
    if (sci) gEnable(0x0C11);
    if (srgb) gEnable(0x8DB9);
}
