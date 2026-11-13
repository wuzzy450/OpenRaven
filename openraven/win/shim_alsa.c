// Fake libasound.so.2 for the game's own Linux FMOD (openraven/extralibs/libfmod.so.10).
// FMOD's ALSA output opens "default", sets format/rate/channels, then pushes audio with
// snd_pcm_writei from its mixer thread. We convert that to float stereo, apply the volume boost
// (F1 "volume", same as the Windows-DLL path) and play it through WASAPI (shared mode).
// Only playback; capture and PulseAudio are reported as unavailable so FMOD picks ALSA.
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"

enum { F_S16 = 2, F_S32 = 10, F_FLOAT = 14 };   // SND_PCM_FORMAT_*
typedef struct { int fmt; unsigned ch, rate; unsigned long buf, per; } hwp;

typedef struct {
    hwp p; int nonblock, started;
    float *ring; unsigned cap, rd, wr;   // stereo frames; rd/wr count frames (mod cap)
    CRITICAL_SECTION cs; HANDLE space;   // signalled when the WASAPI thread consumed frames
    HANDLE thread; volatile int quit;
} pcm_t;

// "boost" is the F1 volume value of the Windows-DLL path (300% = 4.0), where it scaled FMOD's master
// channel group inside the mix. Here it scales the finished mix, so 4.0 maps to 1x (unchanged).
static volatile float g_gain = 1.0f;
void raven_alsa_set_gain(float boost) { g_gain = boost / 4.0f; raven_log("[alsa] output gain %.2f\n", g_gain); }
// Soft limiter: linear up to 0.8, then rounds off toward 1.0 instead of clipping.
static float limit(float x) {
    float a = fabsf(x);
    if (a <= 0.8f) return x;
    float y = 0.8f + 0.2f * tanhf((a - 0.8f) / 0.2f);
    return x < 0 ? -y : y;
}

static unsigned used(pcm_t *p) { return p->wr - p->rd; }

static DWORD WINAPI out_thread(void *arg) {
    pcm_t *p = arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL; IAudioClient *ac = NULL; IAudioRenderClient *rc = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEXTENSIBLE wf = {0};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; wf.Format.nChannels = 2; wf.Format.nSamplesPerSec = p->p.rate;
    wf.Format.wBitsPerSample = 32; wf.Format.nBlockAlign = 8; wf.Format.nAvgBytesPerSec = p->p.rate * 8;
    wf.Format.cbSize = 22; wf.Samples.wValidBitsPerSample = 32; wf.dwChannelMask = 3;
    static const GUID ieee_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    wf.SubFormat = ieee_float;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (SUCCEEDED(hr)) hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            200000 /* 20 ms */, 0, (WAVEFORMATEX *)&wf, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(ac, ev);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    UINT32 bufn = 0;
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(ac, &bufn);
    if (SUCCEEDED(hr)) hr = IAudioClient_Start(ac);
    if (FAILED(hr)) { raven_log("[alsa] WASAPI init failed 0x%08lx, no sound\n", (unsigned long)hr); p->quit = 2; SetEvent(p->space); return 0; }
    raven_log("[alsa] WASAPI output %u Hz stereo float, device buffer %u frames\n", p->p.rate, bufn);
    unsigned long under = 0;
    while (!p->quit) {
        WaitForSingleObject(ev, 100);
        UINT32 pad = 0; if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) break;
        UINT32 n = bufn - pad; if (!n) continue;
        BYTE *d = NULL; if (FAILED(IAudioRenderClient_GetBuffer(rc, n, &d))) continue;
        float *o = (float *)d, g = g_gain;
        EnterCriticalSection(&p->cs);
        unsigned have = used(p), k = have < n ? have : n;
        for (unsigned i = 0; i < k; i++) {
            unsigned j = (p->rd + i) % p->cap;
            float l = p->ring[2 * j] * g, r = p->ring[2 * j + 1] * g;
            o[2 * i] = limit(l); o[2 * i + 1] = limit(r);
        }
        p->rd += k;
        LeaveCriticalSection(&p->cs);
        if (k < n) { memset(o + 2 * k, 0, (n - k) * 8); if (p->started && under++ < 5) raven_log("[alsa] underrun (%u of %u frames)\n", k, n); }
        IAudioRenderClient_ReleaseBuffer(rc, n, 0);
        SetEvent(p->space);
    }
    IAudioClient_Stop(ac);
    IAudioRenderClient_Release(rc); IAudioClient_Release(ac); IMMDevice_Release(dev); IMMDeviceEnumerator_Release(en);
    CloseHandle(ev);
    return 0;
}

// ---- PCM ----
static SV int a_open(pcm_t **out, const char *name, int stream, int mode) {
    raven_log("[alsa] snd_pcm_open(%s, %s, mode %d)\n", name ? name : "?", stream ? "capture" : "playback", mode);
    if (stream != 0) return -2;   // -ENOENT: no recording
    pcm_t *p = calloc(1, sizeof *p);
    p->nonblock = mode & 1; p->p.fmt = F_S16; p->p.ch = 2; p->p.rate = 48000; p->p.buf = 4096; p->p.per = 1024;
    InitializeCriticalSection(&p->cs); p->space = CreateEventA(NULL, FALSE, FALSE, NULL);
    *out = p;
    return 0;
}
static SV int a_close(pcm_t *p) {
    if (!p) return 0;
    if (p->thread) { p->quit = 1; WaitForSingleObject(p->thread, 2000); CloseHandle(p->thread); }
    free(p->ring); CloseHandle(p->space); DeleteCriticalSection(&p->cs); free(p);
    raven_log("[alsa] closed\n");
    return 0;
}
static SV size_t a_hw_sizeof(void) { return 256; }
static SV size_t a_sw_sizeof(void) { return 256; }
static SV int a_hw_any(pcm_t *p, hwp *h) { *h = p->p; return 0; }
static SV int a_ok(void) { return 0; }
static SV int a_set_format(pcm_t *p, hwp *h, int f) { (void)p; if (f != F_S16 && f != F_S32 && f != F_FLOAT) return -22; h->fmt = f; return 0; }
static SV int a_set_channels(pcm_t *p, hwp *h, unsigned c) { (void)p; if (c < 1 || c > 8) return -22; h->ch = c; return 0; }
static SV int a_set_rate(pcm_t *p, hwp *h, unsigned r, int dir) { (void)p; (void)dir; if (r < 8000 || r > 192000) return -22; h->rate = r; return 0; }
static SV int a_test_rate(pcm_t *p, hwp *h, unsigned r, int dir) { (void)p; (void)h; (void)dir; return r >= 8000 && r <= 192000 ? 0 : -22; }
static SV int a_set_buf(pcm_t *p, hwp *h, unsigned long *v) { (void)p; if (*v < 256) *v = 256; if (*v > 65536) *v = 65536; h->buf = *v; return 0; }
static SV int a_set_per(pcm_t *p, hwp *h, unsigned long *v, int *dir) { (void)p; (void)dir; if (*v < 64) *v = 64; h->per = *v; return 0; }
static SV int a_hw_params(pcm_t *p, hwp *h) {
    p->p = *h;
    if (p->p.per > p->p.buf) p->p.per = p->p.buf;
    free(p->ring); p->cap = (unsigned)p->p.buf; p->ring = calloc(p->cap, 8); p->rd = p->wr = 0;
    raven_log("[alsa] hw_params: format %d, %u ch, %u Hz, buffer %lu, period %lu frames\n", p->p.fmt, p->p.ch, p->p.rate, p->p.buf, p->p.per);
    if (!p->thread) p->thread = CreateThread(NULL, 0, out_thread, p, 0, NULL);
    return 0;
}
static SV int a_nonblock(pcm_t *p, int nb) { p->nonblock = nb; return 0; }
static SV int a_start(pcm_t *p) { p->started = 1; return 0; }
static SV long a_avail(pcm_t *p) {
    if (p->quit == 2) return p->cap;   // no device: swallow audio
    EnterCriticalSection(&p->cs); long a = (long)(p->cap - used(p)); LeaveCriticalSection(&p->cs);
    return a;
}
static float sample(const void *b, int fmt, size_t i) {
    if (fmt == F_FLOAT) return ((const float *)b)[i];
    if (fmt == F_S32) return ((const int *)b)[i] / 2147483648.0f;
    return ((const short *)b)[i] / 32768.0f;
}
void streamweb_audio(const float *lr, unsigned frames, unsigned rate);   // streamweb_win.c
static SV long a_writei(pcm_t *p, const void *buf, unsigned long frames) {
    unsigned ch = p->p.ch; unsigned long done = 0;
    p->started = 1;
    while (done < frames) {
        if (p->quit == 2) return (long)frames;
        EnterCriticalSection(&p->cs);
        unsigned room = p->cap - used(p);
        unsigned long n = frames - done; if (n > room) n = room;
        for (unsigned long i = 0; i < n; i++) {
            size_t s = (done + i) * ch; unsigned j = (p->wr + (unsigned)i) % p->cap;
            float l = sample(buf, p->p.fmt, s), r = ch > 1 ? sample(buf, p->p.fmt, s + 1) : l;
            if (ch >= 6) {   // ALSA 5.1 order FL FR RL RR C LFE (FMOD normally gives stereo: see setSoftwareFormat)
                float c = sample(buf, p->p.fmt, s + 4), lf = sample(buf, p->p.fmt, s + 5);
                l = (l + 0.707f * c + 0.707f * sample(buf, p->p.fmt, s + 2) + 0.5f * lf) * 0.5f;
                r = (r + 0.707f * c + 0.707f * sample(buf, p->p.fmt, s + 3) + 0.5f * lf) * 0.5f;
            }
            p->ring[2 * j] = l; p->ring[2 * j + 1] = r;
            float lr[2] = {l, r};
            streamweb_audio(lr, 1, p->p.rate);   // browser stream: the game's own sound only
        }
        p->wr += (unsigned)n; done += n;
        LeaveCriticalSection(&p->cs);
        if (done < frames) {
            if (p->nonblock) return done ? (long)done : -11;   // -EAGAIN
            WaitForSingleObject(p->space, 50);
        }
    }
    return (long)frames;
}
static SV long a_readi(void) { return -5; }   // -EIO
static SV const char *a_strerror(int e) { static char b[32]; snprintf(b, sizeof b, "ALSA error %d", e); return b; }
static SV const char *a_version(void) { return "1.1.3"; }

// ---- device list: one device, "default" ----
static SV int a_hint(int card, const char *iface, void ***hints) {
    (void)card; (void)iface;
    static void *list[2] = {(void *)1, NULL};
    *hints = list;
    return 0;
}
static SV char *a_get_hint(const void *h, const char *id) {
    (void)h;
    if (!strcmp(id, "NAME")) return _strdup("default");
    if (!strcmp(id, "DESC")) return _strdup("Windows audio");
    return NULL;   // IOID NULL = input and output
}
static SV int a_free_hint(void **h) { (void)h; return 0; }

const shim_entry shims_alsa[] = {
    {"snd_pcm_open", a_open}, {"snd_pcm_close", a_close},
    {"snd_pcm_hw_params_sizeof", a_hw_sizeof}, {"snd_pcm_sw_params_sizeof", a_sw_sizeof},
    {"snd_pcm_hw_params_any", a_hw_any}, {"snd_pcm_hw_params_set_access", a_ok},
    {"snd_pcm_hw_params_set_format", a_set_format}, {"snd_pcm_hw_params_set_channels", a_set_channels},
    {"snd_pcm_hw_params_set_rate", a_set_rate}, {"snd_pcm_hw_params_test_rate", a_test_rate},
    {"snd_pcm_hw_params_set_buffer_size_near", a_set_buf}, {"snd_pcm_hw_params_set_period_size_near", a_set_per},
    {"snd_pcm_hw_params", a_hw_params},
    {"snd_pcm_sw_params_current", a_ok}, {"snd_pcm_sw_params_set_avail_min", a_ok},
    {"snd_pcm_sw_params_set_start_threshold", a_ok}, {"snd_pcm_sw_params", a_ok},
    {"snd_pcm_prepare", a_ok}, {"snd_pcm_resume", a_ok}, {"snd_pcm_start", a_start}, {"snd_pcm_nonblock", a_nonblock},
    {"snd_pcm_avail_update", a_avail}, {"snd_pcm_writei", a_writei}, {"snd_pcm_readi", a_readi},
    {"snd_strerror", a_strerror}, {"snd_asoundlib_version", a_version},
    {"snd_device_name_hint", a_hint}, {"snd_device_name_get_hint", a_get_hint}, {"snd_device_name_free_hint", a_free_hint},
    {NULL, NULL},
};
