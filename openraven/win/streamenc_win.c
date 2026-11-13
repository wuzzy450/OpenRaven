// Browser stream, video: the game's own picture (read from the back buffer before the F1 overlay, so
// never the desktop or other windows) -> H.264 (Windows Media Foundation encoder, low-latency, no
// B-frames) -> fragmented MP4, one fragment per frame -> streamweb_win.c -> the browser's
// MediaSource player. Only runs while a browser is watching.
//
//   RAVEN_STREAM_HEIGHT=720    picture height sent (width follows the game's shape)
//   RAVEN_STREAM_FPS=60        frames per second sent
//   RAVEN_STREAM_KBPS=8000     video bit rate
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <initguid.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <strmif.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "shim.h"

void *gl_resolve(const char *name);
// codecapi.h names these GUIDs with __uuidof (C++ only); same values, defined for C:
#define DG(n, ...) DEFINE_GUID(n, __VA_ARGS__)   // expands the STATIC_ value list first
DG(K_AVLowLatencyMode, STATIC_CODECAPI_AVLowLatencyMode);
DG(K_AVEncCommonRateControlMode, STATIC_CODECAPI_AVEncCommonRateControlMode);
DG(K_AVEncMPVGOPSize, STATIC_CODECAPI_AVEncMPVGOPSize);
DG(K_AVEncMPVDefaultBPictureCount, STATIC_CODECAPI_AVEncMPVDefaultBPictureCount);
DG(K_AVEncVideoForceKeyFrame, STATIC_CODECAPI_AVEncVideoForceKeyFrame);
int streamweb_active(void);
int streamweb_viewers(void);
int streamweb_take_key_request(void);
void streamweb_video(const unsigned char *init, size_t ninit, const unsigned char *frag, size_t n, int key);

#define NB 3
static int W, H, g_fps = 60, g_kbps = 8000;
static unsigned char *g_bgra; static size_t g_fsize;
static LONGLONG g_frame_t;                      // capture time of the frame in g_bgra (100 ns)
static HANDLE g_ready; static volatile LONG g_busy;

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

// ---------------- fragmented MP4 ----------------
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

// ---------------- encoder thread ----------------
// Any graphics card: every hardware H.264 encoder Windows knows (NVIDIA NVENC, AMD AMF/VCE, Intel Quick
// Sync) is listed; the card that draws the game is tried first, then the others, then Windows' software
// encoder. An encoder that refuses the settings is retried with a D3D11 device (some AMD/Intel ones want
// it); one that takes frames but gives no picture within 3 s is dropped for the next one.
//   RAVEN_STREAM_ENCODER=auto|nvidia|amd|intel|software   (RAVEN_STREAM_HW=0 = software, older name)
static IMFTransform *g_t;
static ICodecAPI *g_api;

static int g_async;                      // hardware encoders work by events (want input / have output)
static IMFMediaEventGenerator *g_ev;
static char g_encname[96] = "starting";
static char g_glvendor[64];              // GL_VENDOR of the game's card (read on the GL thread)

// Types + low-latency settings on an encoder; 0 = it refused.
static int enc_setup(IMFTransform *t, int profile)
{
    IMFMediaType *ot, *it;
    MFCreateMediaType(&ot);
    IMFMediaType_SetGUID(ot, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(ot, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    IMFMediaType_SetUINT32(ot, &MF_MT_AVG_BITRATE, (UINT32)g_kbps * 1000);
    IMFMediaType_SetUINT64(ot, &MF_MT_FRAME_SIZE, (UINT64)W << 32 | (UINT32)H);
    IMFMediaType_SetUINT64(ot, &MF_MT_FRAME_RATE, (UINT64)g_fps << 32 | 1);
    IMFMediaType_SetUINT32(ot, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT32(ot, &MF_MT_MPEG2_PROFILE, (UINT32)profile);
    HRESULT hr = IMFTransform_SetOutputType(t, 0, ot, 0);
    MFCreateMediaType(&it);
    IMFMediaType_SetGUID(it, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(it, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    IMFMediaType_SetUINT64(it, &MF_MT_FRAME_SIZE, (UINT64)W << 32 | (UINT32)H);
    IMFMediaType_SetUINT64(it, &MF_MT_FRAME_RATE, (UINT64)g_fps << 32 | 1);
    IMFMediaType_SetUINT32(it, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = IMFTransform_SetInputType(t, 0, it, 0);
    IMFMediaType_Release(ot); IMFMediaType_Release(it);
    if (FAILED(hr)) return 0;
    ICodecAPI *api = NULL;
    if (SUCCEEDED(IMFTransform_QueryInterface(t, &IID_ICodecAPI, (void **)&api))) {
        VARIANT v; VariantInit(&v);
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE; ICodecAPI_SetValue(api, &K_AVLowLatencyMode, &v);
        v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR; ICodecAPI_SetValue(api, &K_AVEncCommonRateControlMode, &v);
        v.vt = VT_UI4; v.ulVal = (ULONG)g_fps * 2; ICodecAPI_SetValue(api, &K_AVEncMPVGOPSize, &v);
        v.vt = VT_UI4; v.ulVal = 0; ICodecAPI_SetValue(api, &K_AVEncMPVDefaultBPictureCount, &v);
    }
    g_api = api;
    return 1;
}

#define MAXC 12
typedef struct { IMFActivate *act; unsigned vendor; char name[64]; } cand_t;
static cand_t g_c[MAXC + 1];             // the last one = Windows software (act NULL)
static int g_nc;
static IMFActivate *g_act;
static IMFDXGIDeviceManager *g_dxm; static ID3D11Device *g_d3d;

static unsigned vendor_of(const char *s)
{
    char l[96]; int i = 0;
    for (; s[i] && i < 95; i++) l[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
    l[i] = 0;
    if (strstr(l, "nvidia")) return 0x10de;
    if (strstr(l, "amd") || strstr(l, "ati ") || strstr(l, "ati_") || strstr(l, "radeon")) return 0x1002;
    if (strstr(l, "intel")) return 0x8086;
    return 0;
}
static const char *vname(unsigned v) { return v == 0x10de ? "NVIDIA" : v == 0x1002 ? "AMD" : v == 0x8086 ? "Intel" : v ? "other" : "unknown"; }

static void list_encoders(void)
{
    const char *want = raven_env("RAVEN_STREAM_ENCODER"), *hw = raven_env("RAVEN_STREAM_HW");
    unsigned force = 0; int sw_only = hw && *hw == '0';
    if (want && *want) {
        if (!_stricmp(want, "software")) sw_only = 1;
        else if (_stricmp(want, "auto")) force = vendor_of(want);
    }
    unsigned gpu = vendor_of(g_glvendor);
    raven_log("[stream] game runs on: %s (%s); encoder choice: %s\n", g_glvendor[0] ? g_glvendor : "?", vname(gpu), want && *want ? want : "auto");
    if (!sw_only) {
        MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_NV12}, out = {MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate **acts = NULL; UINT32 n = 0;
        if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, &n))) {
            cand_t tmp[MAXC]; int nt = 0;
            for (UINT32 i = 0; i < n; i++) {
                cand_t c = {acts[i], 0, "graphics card"};
                WCHAR wn[64] = L""; UINT32 l = 0;
                if (SUCCEEDED(IMFActivate_GetString(acts[i], &MFT_FRIENDLY_NAME_Attribute, wn, 64, &l)))
                    WideCharToMultiByte(CP_UTF8, 0, wn, -1, c.name, sizeof c.name, NULL, NULL);
                WCHAR wv[64] = L""; char v[64] = "";
                if (SUCCEEDED(IMFActivate_GetString(acts[i], &MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, wv, 64, &l))) {
                    WideCharToMultiByte(CP_UTF8, 0, wv, -1, v, sizeof v, NULL, NULL);
                    const char *p = strstr(v, "VEN_"); if (p) c.vendor = (unsigned)strtoul(p + 4, NULL, 16);
                }
                if (!c.vendor) c.vendor = vendor_of(c.name);
                raven_log("[stream] hardware encoder %u: %s (%s, %s)\n", i, c.name, v[0] ? v : "no vendor id", vname(c.vendor));
                if ((force && c.vendor != force) || nt >= MAXC) { IMFActivate_Release(acts[i]); continue; }
                tmp[nt++] = c;
            }
            CoTaskMemFree(acts);
            // the game's card first, then the rest in Windows' order
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < nt; i++) if ((gpu && tmp[i].vendor == gpu) == !pass) g_c[g_nc++] = tmp[i];
        }
        if (force && !g_nc) raven_log("[stream] no %s encoder found, using software\n", want);
    }
    g_c[g_nc].act = NULL; g_c[g_nc].vendor = 0; snprintf(g_c[g_nc].name, sizeof g_c[g_nc].name, "Windows software encoder");
    g_nc++;
}

// A D3D11 device on the encoder's own card, for encoders that refuse to work without one.
static IMFDXGIDeviceManager *make_dxm(unsigned vendor)
{
    IDXGIFactory1 *f = NULL; IDXGIAdapter1 *ad = NULL, *pick = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) return NULL;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d; IDXGIAdapter1_GetDesc1(ad, &d);
        if (!pick && d.VendorId == vendor) pick = ad; else IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory1_Release(f);
    ID3D11Device *dev = NULL;
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter *)pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, NULL);
    if (pick) IDXGIAdapter1_Release(pick);
    if (FAILED(hr)) return NULL;
    ID3D10Multithread *mt = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D10Multithread, (void **)&mt))) { ID3D10Multithread_SetMultithreadProtected(mt, TRUE); ID3D10Multithread_Release(mt); }
    UINT tok = 0; IMFDXGIDeviceManager *m = NULL;
    if (FAILED(MFCreateDXGIDeviceManager(&tok, &m)) || FAILED(IMFDXGIDeviceManager_ResetDevice(m, (IUnknown *)dev, tok))) {
        if (m) IMFDXGIDeviceManager_Release(m);
        ID3D11Device_Release(dev); return NULL;
    }
    g_d3d = dev;
    return m;
}

static void enc_close(void)
{
    if (g_api) { ICodecAPI_Release(g_api); g_api = NULL; }
    if (g_ev) { IMFMediaEventGenerator_Release(g_ev); g_ev = NULL; }
    if (g_t) {
        IMFTransform_ProcessMessage(g_t, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        if (g_dxm) IMFTransform_ProcessMessage(g_t, MFT_MESSAGE_SET_D3D_MANAGER, 0);
        IMFTransform_Release(g_t); g_t = NULL;
    }
    if (g_act) { IMFActivate_ShutdownObject(g_act); g_act = NULL; }
    if (g_dxm) { IMFDXGIDeviceManager_Release(g_dxm); g_dxm = NULL; }
    if (g_d3d) { ID3D11Device_Release(g_d3d); g_d3d = NULL; }
    g_async = 0;
}

static int try_hw(cand_t *c, int with_d3d)
{
    IMFTransform *t = NULL;
    if (FAILED(IMFActivate_ActivateObject(c->act, &IID_IMFTransform, (void **)&t))) return 0;
    g_t = t; g_act = c->act;
    IMFAttributes *at = NULL;
    if (SUCCEEDED(IMFTransform_GetAttributes(t, &at)) && at) {
        IMFAttributes_SetUINT32(at, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        IMFAttributes_SetUINT32(at, &MF_LOW_LATENCY, TRUE);
        IMFAttributes_Release(at);
    }
    if (with_d3d) {
        g_dxm = make_dxm(c->vendor);
        if (!g_dxm || FAILED(IMFTransform_ProcessMessage(t, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)g_dxm))) {
            if (g_dxm) { IMFDXGIDeviceManager_Release(g_dxm); g_dxm = NULL; }
            enc_close(); return 0;
        }
    }
    if ((enc_setup(t, eAVEncH264VProfile_Base) || enc_setup(t, eAVEncH264VProfile_Main)) &&
        SUCCEEDED(IMFTransform_QueryInterface(t, &IID_IMFMediaEventGenerator, (void **)&g_ev))) {
        g_async = 1;
        snprintf(g_encname, sizeof g_encname, "%s (%s%s)", c->name, vname(c->vendor), with_d3d ? ", D3D11" : "");
        return 1;
    }
    enc_close();
    return 0;
}

// Opens candidate number `from`, or the next one that works; returns its number or -1.
static int enc_open_from(int from)
{
    for (int i = from; i < g_nc; i++) {
        cand_t *c = &g_c[i];
        if (c->act) {
            if (try_hw(c, 0) || try_hw(c, 1)) goto ok;
            raven_log("[stream] %s refused the settings, trying the next encoder\n", c->name);
            continue;
        }
        IMFTransform *t;
        if (FAILED(CoCreateInstance(&CLSID_CMSH264EncoderMFT, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&t))) continue;
        g_t = t;
        if (!enc_setup(t, eAVEncH264VProfile_Base)) { raven_log("[stream] H.264 encoder refused %dx%d\n", W, H); enc_close(); continue; }
        snprintf(g_encname, sizeof g_encname, "%s (uses the processor)", c->name);
    ok:
        IMFTransform_ProcessMessage(g_t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        IMFTransform_ProcessMessage(g_t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        raven_log("[stream] H.264 encoder ready (%s): %dx%d, %d fps, %d kbit/s\n", g_encname, W, H, g_fps, g_kbps);
        return i;
    }
    snprintf(g_encname, sizeof g_encname, "none worked - no browser video");
    return -1;
}

// For the F1 Status row.
const char *streamenc_status(void) { return g_encname; }

// BGRA, bottom-up rows (glReadPixels) -> NV12 top-down, BT.709 limited range.
static void to_nv12(const unsigned char *src, unsigned char *dst)
{
    unsigned char *uv = dst + (size_t)W * H;
    for (int y = 0; y < H; y++) {
        const unsigned char *s = src + (size_t)(H - 1 - y) * W * 4;
        unsigned char *d = dst + (size_t)y * W;
        for (int x = 0; x < W; x++) { int b = s[4 * x], g = s[4 * x + 1], r = s[4 * x + 2]; d[x] = (unsigned char)(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16); }
        if (y & 1) continue;
        const unsigned char *s2 = src + (size_t)(H - 2 - y) * W * 4;
        unsigned char *u = uv + (size_t)(y / 2) * W;
        for (int x = 0; x < W; x += 2) {
            int b = (s[4 * x] + s[4 * x + 4] + s2[4 * x] + s2[4 * x + 4]) / 4;
            int g = (s[4 * x + 1] + s[4 * x + 5] + s2[4 * x + 1] + s2[4 * x + 5]) / 4;
            int r = (s[4 * x + 2] + s[4 * x + 6] + s2[4 * x + 2] + s2[4 * x + 6]) / 4;
            u[x] = (unsigned char)(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128);
            u[x + 1] = (unsigned char)(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
        }
    }
}

// Annex-B -> AVCC (4-byte lengths), pulling out SPS/PPS; drops AUD/SPS/PPS/SEI from the sample.
static size_t to_avcc(const unsigned char *a, size_t n, unsigned char *out, int *key,
                      unsigned char *sps, int *nsps, unsigned char *pps, int *npps)
{
    size_t o = 0, i = 0;
    *key = 0;
    // NAL units start after 00 00 01 (a 4-byte 00 00 00 01 is the same with one more zero in front).
    while (i + 3 <= n && !(a[i] == 0 && a[i + 1] == 0 && a[i + 2] == 1)) i++;
    while (i + 3 <= n) {
        size_t s = i + 3, e = s;
        while (e + 3 <= n && !(a[e] == 0 && a[e + 1] == 0 && a[e + 2] == 1)) e++;
        size_t next = e + 3 <= n ? e : n;
        if (e + 3 > n) e = n;
        while (e > s && a[e - 1] == 0) e--;   // the zero in front of a 4-byte start code (and trailing zeros)
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

// Encoder output -> fMP4 fragment -> browsers.
static unsigned char *g_avcc, g_sps[256], g_pps[256];
static int g_nsps, g_npps;
static bw g_initseg;
static uint32_t g_seq;
static LONGLONG g_last_ts;
static volatile LONG g_outs;             // pictures this encoder has given (0 = maybe a dud)
static void emit(IMFSample *os)
{
    IMFMediaBuffer *ob = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(os, &ob)) || !ob) return;
    BYTE *d; DWORD n = 0;
    IMFMediaBuffer_Lock(ob, &d, NULL, &n);
    int key = 0;
    size_t an = n < (4u << 20) - 64 ? to_avcc(d, n, g_avcc, &key, g_sps, &g_nsps, g_pps, &g_npps) : 0;
    IMFMediaBuffer_Unlock(ob);
    IMFMediaBuffer_Release(ob);
    LONGLONG ts = 0; IMFSample_GetSampleTime(os, &ts);
    if (g_nsps && g_npps && !g_initseg.p) {
        g_initseg = make_init(g_sps, g_nsps, g_pps, g_npps);
        raven_log("[stream] video: H.264 profile %d level %d.%d\n", g_sps[1], g_sps[3] / 10, g_sps[3] % 10);
    }
    if (g_initseg.p && an) {
        uint32_t dur = (uint32_t)((ts - g_last_ts) * 9 / 1000); if (!g_seq || dur == 0 || dur > 90000) dur = 90000 / g_fps;
        bw f = make_frag(++g_seq, (uint64_t)ts * 9 / 1000, dur, g_avcc, an, key);
        streamweb_video(g_initseg.p, g_initseg.n, f.p, f.n, key);
        free(f.p);
    }
    g_last_ts = ts;
    g_outs++;
}

// Next captured frame as an NV12 sample (waits for one).
static IMFSample *next_frame(unsigned char *nv, size_t nvsize)
{
    static LONGLONG t0 = -1;
    WaitForSingleObject(g_ready, INFINITE);
    to_nv12(g_bgra, nv);
    LONGLONG t = g_frame_t;
    InterlockedExchange(&g_busy, 0);
    if (t0 < 0) t0 = t;
    if (streamweb_take_key_request() && g_api) {
        VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = 1;
        ICodecAPI_SetValue(g_api, &K_AVEncVideoForceKeyFrame, &v);
    }
    IMFMediaBuffer *mb; IMFSample *s;
    MFCreateMemoryBuffer((DWORD)nvsize, &mb);
    BYTE *p; IMFMediaBuffer_Lock(mb, &p, NULL, NULL); memcpy(p, nv, nvsize); IMFMediaBuffer_Unlock(mb);
    IMFMediaBuffer_SetCurrentLength(mb, (DWORD)nvsize);
    MFCreateSample(&s); IMFSample_AddBuffer(s, mb); IMFMediaBuffer_Release(mb);
    IMFSample_SetSampleTime(s, t - t0); IMFSample_SetSampleDuration(s, 10000000 / g_fps);
    return s;
}

static DWORD WINAPI enc_thread(void *arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 0;
    list_encoders();
    int ci = enc_open_from(0);
    if (ci < 0) { raven_log("[stream] no H.264 encoder: browser video off\n"); return 0; }
    size_t nvsize = (size_t)W * H * 3 / 2;
    unsigned char *nv = malloc(nvsize);
    g_avcc = malloc(4 << 20);
    MFT_OUTPUT_STREAM_INFO si; IMFTransform_GetOutputStreamInfo(g_t, 0, &si);
    int own = !(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
    DWORD t_open = GetTickCount(), t_in = 0;   // t_in: first frame given to this encoder
    for (;;) {
        if (g_async) {   // hardware: the encoder says what it wants next
            IMFMediaEvent *ev = NULL; MediaEventType et = 0;
            HRESULT hr = IMFMediaEventGenerator_GetEvent(g_ev, MF_EVENT_FLAG_NO_WAIT, &ev);
            const char *why = NULL;
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                DWORD now = GetTickCount();
                if (!g_outs && ((t_in && now - t_in > 3000) || (!t_in && now - t_open > 5000))) why = "gave no picture";
                else { Sleep(1); continue; }
            } else if (FAILED(hr)) why = "stopped";
            if (why) {   // this card's encoder does not work here: next one (finally the software one)
                raven_log("[stream] %s %s, trying the next encoder\n", g_encname, why);
                enc_close();
                if (g_outs) { free(g_initseg.p); memset(&g_initseg, 0, sizeof g_initseg); g_nsps = g_npps = 0; g_seq = 0; g_outs = 0; }
                if ((ci = enc_open_from(ci + 1)) < 0) { raven_log("[stream] no H.264 encoder left: browser video off\n"); return 0; }
                IMFTransform_GetOutputStreamInfo(g_t, 0, &si);
                own = !(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
                t_open = GetTickCount(); t_in = 0;
                continue;
            }
            IMFMediaEvent_GetType(ev, &et); IMFMediaEvent_Release(ev);
            if (et == METransformNeedInput) {
                IMFSample *s = next_frame(nv, nvsize);
                IMFTransform_ProcessInput(g_t, 0, s, 0);
                IMFSample_Release(s);
                if (!t_in) t_in = GetTickCount();
                continue;
            }
            if (et != METransformHaveOutput) continue;
        } else {          // software: one frame in, then take all output
            IMFSample *s = next_frame(nv, nvsize);
            HRESULT hr = IMFTransform_ProcessInput(g_t, 0, s, 0);
            IMFSample_Release(s);
            if (FAILED(hr)) continue;
        }
        for (;;) {
            IMFSample *os = NULL;
            if (own) {
                IMFTransform_GetOutputStreamInfo(g_t, 0, &si);
                IMFMediaBuffer *ob;
                MFCreateMemoryBuffer(si.cbSize > 4096 ? si.cbSize : 2 << 20, &ob);
                MFCreateSample(&os); IMFSample_AddBuffer(os, ob); IMFMediaBuffer_Release(ob);
            }
            MFT_OUTPUT_DATA_BUFFER od = {0}; od.pSample = os;
            DWORD st = 0;
            HRESULT hr = IMFTransform_ProcessOutput(g_t, 0, 1, &od, &st);
            if (SUCCEEDED(hr) && od.pSample) emit(od.pSample);
            if (od.pEvents) IMFCollection_Release(od.pEvents);
            if (od.pSample) IMFSample_Release(od.pSample);
            else if (os) IMFSample_Release(os);
            if (FAILED(hr) || g_async) break;   // software: MF_E_TRANSFORM_NEED_MORE_INPUT; hardware: one per event
        }
    }
    return 0;
}

static int cap_init(int sw, int sh)
{
#define L(v, n) if (!(v = (__typeof__(v))gl_resolve(n))) return 0;
    L(gGenFramebuffers, "glGenFramebuffers") L(gBindFramebuffer, "glBindFramebuffer") L(gFramebufferTexture2D, "glFramebufferTexture2D")
    L(gBlitFramebuffer, "glBlitFramebuffer") L(gGenTextures, "glGenTextures") L(gBindTexture, "glBindTexture")
    L(gTexImage2D, "glTexImage2D") L(gGetIntegerv, "glGetIntegerv") L(gIsEnabled, "glIsEnabled") L(gEnable, "glEnable")
    L(gDisable, "glDisable") L(gReadBuffer, "glReadBuffer") L(gGenBuffers, "glGenBuffers") L(gBindBuffer, "glBindBuffer")
    L(gBufferData, "glBufferData") L(gMapBufferRange, "glMapBufferRange") L(gUnmapBuffer, "glUnmapBuffer")
    L(gReadPixels, "glReadPixels") L(gPixelStorei, "glPixelStorei") L(gFenceSync, "glFenceSync")
    L(gClientWaitSync, "glClientWaitSync") L(gDeleteSync, "glDeleteSync")
#undef L
    typedef const GLubyte *(APIENTRY *gs_t)(GLenum);
    gs_t gs = (gs_t)gl_resolve("glGetString");
    const char *gv = gs ? (const char *)gs(GL_VENDOR) : NULL, *gr = gs ? (const char *)gs(GL_RENDERER) : NULL;
    snprintf(g_glvendor, sizeof g_glvendor, "%s %s", gv ? gv : "", gr ? gr : "");
    const char *e;
    int hh = (e = raven_env("RAVEN_STREAM_HEIGHT")) ? atoi(e) : 720;
    if ((e = raven_env("RAVEN_STREAM_FPS"))) g_fps = atoi(e);
    if ((e = raven_env("RAVEN_STREAM_KBPS"))) g_kbps = atoi(e);
    if (g_fps < 10) g_fps = 10;
    if (g_fps > 60) g_fps = 60;
    if (hh < 240) hh = 240;
    H = sh < hh ? sh : hh; W = (int)((long long)sw * H / sh);
    W &= ~15; H &= ~1;   // encoder-friendly sizes
    g_fsize = (size_t)W * H * 4;
    g_bgra = malloc(g_fsize);
    g_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, enc_thread, NULL, 0, NULL));
    return 1;
}

// Called once per presented frame from ravengfx_frame (game picture in the back buffer, no overlay yet).
void streamenc_frame(int sw, int sh)
{
    static int st;   // 0 = not started, 1 = ok, -1 = off
    static GLuint fbo_d, tex, pbo[NB]; static GLsync fence[NB]; static int slot, inflight[NB];
    static LONGLONG at[NB];
    static LARGE_INTEGER qf, last;
    if (st < 0 || sw <= 0 || sh <= 0 || !streamweb_active()) return;
    if (!st) { st = cap_init(sw, sh) ? 1 : -1; if (st < 0) return; }
    if (!streamweb_viewers()) { for (int i = 0; i < NB; i++) inflight[i] = 0; return; }
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    if (!tex) {
        GLint ot; gGetIntegerv(GL_TEXTURE_BINDING_2D, &ot);
        gGenTextures(1, &tex); gBindTexture(GL_TEXTURE_2D, tex);
        gTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        gBindTexture(GL_TEXTURE_2D, ot);
        gGenFramebuffers(1, &fbo_d); gGenBuffers(NB, pbo);
        GLint opb; gGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &opb);
        for (int i = 0; i < NB; i++) { gBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]); gBufferData(GL_PIXEL_PACK_BUFFER, g_fsize, NULL, GL_STREAM_READ); }
        gBindBuffer(GL_PIXEL_PACK_BUFFER, opb);
    }
    GLint rf, df, opb, pa, rb; GLboolean sci = gIsEnabled(GL_SCISSOR_TEST), srgb = gIsEnabled(GL_FRAMEBUFFER_SRGB);
    gGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rf); gGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &df);
    gGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &opb); gGetIntegerv(GL_PACK_ALIGNMENT, &pa);
    if (sci) gDisable(GL_SCISSOR_TEST);
    if (srgb) gDisable(GL_FRAMEBUFFER_SRGB);
    // finished readbacks -> encoder thread (dropped if it is still busy with the last one)
    for (int k = 0; k < NB; k++) {
        int i = (slot + 1 + k) % NB;
        if (!inflight[i] || gClientWaitSync(fence[i], 0, 0) == GL_TIMEOUT_EXPIRED) continue;
        gDeleteSync(fence[i]); inflight[i] = 0;
        if (!InterlockedCompareExchange(&g_busy, 1, 0)) {
            gBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]);
            void *m = gMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, g_fsize, GL_MAP_READ_BIT);
            if (m) { memcpy(g_bgra, m, g_fsize); gUnmapBuffer(GL_PIXEL_PACK_BUFFER); g_frame_t = at[i]; SetEvent(g_ready); }
            else InterlockedExchange(&g_busy, 0);
        }
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if ((!last.QuadPart || (now.QuadPart - last.QuadPart) * (g_fps + 2) >= qf.QuadPart) && !inflight[slot]) {
        last = now;
        // Native HDR draws the game into its own float framebuffer (gl_hooks.c), not the window's.
        extern unsigned ravengl_screen_fbo(void);
        unsigned sfb = ravengl_screen_fbo();
        gBindFramebuffer(GL_READ_FRAMEBUFFER, sfb); gGetIntegerv(GL_READ_BUFFER, &rb); gReadBuffer(sfb ? GL_COLOR_ATTACHMENT0 : GL_BACK);
        gBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_d); gFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        gBlitFramebuffer(0, 0, sw, sh, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        gReadBuffer(rb);
        gBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_d);
        gBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]); gPixelStorei(GL_PACK_ALIGNMENT, 4);
        gReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        fence[slot] = gFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); inflight[slot] = 1;
        at[slot] = now.QuadPart / qf.QuadPart * 10000000 + now.QuadPart % qf.QuadPart * 10000000 / qf.QuadPart;
        slot = (slot + 1) % NB;
    }
    gBindBuffer(GL_PIXEL_PACK_BUFFER, opb); gPixelStorei(GL_PACK_ALIGNMENT, pa);
    gBindFramebuffer(GL_READ_FRAMEBUFFER, rf); gBindFramebuffer(GL_DRAW_FRAMEBUFFER, df);
    if (sci) gEnable(GL_SCISSOR_TEST);
    if (srgb) gEnable(GL_FRAMEBUFFER_SRGB);
}
