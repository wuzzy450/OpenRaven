// FMOD for the Windows loader. The game calls the Linux FMOD 1.10 C++ API. Each C++ method is
// `this` + args, so it maps 1:1 to the C function of the Windows FMOD 1.10 DLLs (handle first).
// C handles and C++ object pointers are the same pointers in FMOD. C++ bool -> FMOD_BOOL (int).
// Callbacks the game passes are SysV: FMOD gets an MS-ABI trampoline that calls them.
// DLLs: fmodstudio64.dll + fmod64.dll, next to ravenloader.exe or in RAVEN_FMODDIR.
// Like play_halo.sh / halo_asound.conf: output is stereo (FMOD downmixes the game's 5.1) and the
// master volume is boosted (RAVEN_VOLUME, default 4.0; RAVEN_SURROUND=1 keeps the game's format).
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"

typedef int R;  // FMOD_RESULT
#define ERR_INIT 28
#define ERR_INVALID_HANDLE 30

static HMODULE g_core, g_studio;
static int g_loaded;  // 0 = not tried, 1 = ok, -1 = failed
static float g_boost = 4.0f;
static void *g_master;  // low-level master channel group

static int fmod_load(void) {
    if (g_loaded) return g_loaded > 0;
    g_loaded = -1;
    char dir[MAX_PATH * 2], p[MAX_PATH * 2];
    const char *e = raven_env("RAVEN_FMODDIR");
    if (e) snprintf(dir, sizeof dir, "%s", e);
    else {
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        char *s = strrchr(dir, '\\');
        if (s) *s = 0;
    }
    snprintf(p, sizeof p, "%s\\fmod64.dll", dir);
    g_core = LoadLibraryExA(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    snprintf(p, sizeof p, "%s\\fmodstudio64.dll", dir);
    g_studio = LoadLibraryExA(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_core || !g_studio) {
        raven_log("FMOD: could not load fmod64.dll / fmodstudio64.dll from %s (error %lu), no audio\n",
                  dir, GetLastError());
        return 0;
    }
    extern const char *ravengfx_get(const char *key);
    extern float raven_fmod_volume_from_pct(const char *pct);
    if ((e = raven_env("RAVEN_VOLUME"))) g_boost = (float)atof(e);
    else if ((e = ravengfx_get("volume"))) g_boost = raven_fmod_volume_from_pct(e);  // F1 overlay setting
    raven_log("FMOD: loaded Windows FMOD from %s, volume boost %.2f\n", dir, g_boost);
    g_loaded = 1;
    return 1;
}

static void *fsym(const char *n) {
    if (!fmod_load()) return NULL;
    void *f = (void *)GetProcAddress(g_studio, n);
    if (!f) f = (void *)GetProcAddress(g_core, n);
    if (!f) raven_log("FMOD: missing C function %s\n", n);
    return f;
}

// Calls C function `cname` (MS ABI) with type `params`.
#define CALL(cname, params, ...) do { \
        static R (*f) params; \
        if (!f && !(f = (R (*) params)fsym(#cname))) return ERR_INIT; \
        R r_ = f(__VA_ARGS__);         if (r_) fmod_err(#cname, r_);         return r_; } while (0)
// ChannelControl: try the Channel function, then the ChannelGroup one on a group handle.
#define CC(suffix, params, ...) do { \
        static R (*fc) params, (*fg) params; \
        if (!fc && !(fc = (R (*) params)fsym("FMOD_Channel_" #suffix))) return ERR_INIT; \
        R r = fc(__VA_ARGS__); \
        if (r != ERR_INVALID_HANDLE) return r; \
        if (!fg && !(fg = (R (*) params)fsym("FMOD_ChannelGroup_" #suffix))) return r; \
        return fg(__VA_ARGS__); } while (0)

typedef void *H;
// First few failures of each FMOD call go to the log (FMOD_RESULT codes: fmod_common.h).
static void fmod_err(const char *fn, R r) {
    static struct { const char *fn; int n; } seen[96];
    int i = 0;
    while (i < 96 && seen[i].fn && seen[i].fn != fn) i++;
    if (i == 96) return;
    seen[i].fn = fn;
    if (seen[i].n++ < 5) raven_log("FMOD: %s -> error %d\n", fn, r);
}
typedef struct { float x, y, z; } VEC;

static char *bank_path(const char *p, char *buf, size_t cap) {
    lx_path(p, buf, cap);
    if (GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES && p[0] != '/')
        snprintf(buf, cap, "%s/pm/g7/halo/%s", g_root, p);
    return buf;
}

// Callbacks run on FMOD's own threads: give them the canary slot like a game thread.
extern void raven_thread_setup(void);
static __thread int t_setup;
static void cb_thread(void) { if (!t_setup) { t_setup = 1; raven_thread_setup(); } }

typedef SV R (*sv_chcb)(H, int, int, void *, void *);
typedef SV R (*sv_evcb)(unsigned, H, void *);
static sv_chcb g_chcb;
static sv_evcb g_evcb;
static R ms_chcb(H c, int t, int ct, void *d1, void *d2) { cb_thread(); return g_chcb ? g_chcb(c, t, ct, d1, d2) : 0; }
static R ms_evcb(unsigned t, H e, void *p) { cb_thread(); return g_evcb ? g_evcb(t, e, p) : 0; }

// graphics.ini / overlay `volume` (100%..400%, Linux PipeWire scale): 300% = our default 4.0.
float raven_fmod_volume_from_pct(const char *pct) {
    float p = (float)atof(pct);
    return p > 0 ? p / 75.0f : 4.0f;
}
void raven_fmod_set_volume(float boost) {
    extern void raven_alsa_set_gain(float g);
    raven_alsa_set_gain(boost);   // Linux FMOD path
    g_boost = boost;
    if (!g_master) return;
    R (*sv)(H, float) = (R (*)(H, float))fsym("FMOD_ChannelGroup_SetVolume");
    if (sv) sv(g_master, g_boost);
    raven_log("FMOD: volume boost %.2f\n", g_boost);
}

// ---- C API names the game imports ----
static SV R s_Memory_Initialize(void) { return 0; }  // keep FMOD's own allocator
static SV R s_Debug_Initialize(void) { return 0; }

// ---- Studio::System ----
static SV R s_Studio_Create(H *sys, unsigned ver) {
    (void)ver;  // DLLs are 1.10.10; the 1.10 ABI is the same across minor versions
    CALL(FMOD_Studio_System_Create, (H *, unsigned), sys, 0x00011010);
}
static SV R s_Studio_Initialize(H s, int maxch, unsigned sflags, unsigned flags, void *extra) {
    R r;
    { static R (*f)(H, int, unsigned, unsigned, void *);
      if (!f && !(f = (R (*)(H, int, unsigned, unsigned, void *))fsym("FMOD_Studio_System_Initialize"))) return ERR_INIT;
      r = f(s, maxch, sflags, flags, extra); }
    raven_log("FMOD: Studio initialize(maxch %d, studio 0x%x, flags 0x%x) -> %d\n", maxch, sflags, flags, r);
    if (r == 0) {
        H ll = NULL;
        R (*gl)(H, H *) = (R (*)(H, H *))fsym("FMOD_Studio_System_GetLowLevelSystem");
        R (*gm)(H, H *) = (R (*)(H, H *))fsym("FMOD_System_GetMasterChannelGroup");
        R (*sv)(H, float) = (R (*)(H, float))fsym("FMOD_ChannelGroup_SetVolume");
        if (gl && gm && sv && !gl(s, &ll) && ll && !gm(ll, &g_master)) sv(g_master, g_boost);
    }
    return r;
}
static SV R s_Studio_Release(H s) { CALL(FMOD_Studio_System_Release, (H), s); }
static SV R s_Studio_Update(H s) { CALL(FMOD_Studio_System_Update, (H), s); }
static SV R s_Studio_Flush(H s) { CALL(FMOD_Studio_System_FlushCommands, (H), s); }
static SV R s_Studio_LoadBank(H s, const char *path, unsigned flags, H *bank) {
    char buf[MAX_PATH * 2];
    static R (*f)(H, const char *, unsigned, H *);
    if (!f && !(f = (R (*)(H, const char *, unsigned, H *))fsym("FMOD_Studio_System_LoadBankFile"))) return ERR_INIT;
    R r = f(s, bank_path(path, buf, sizeof buf), flags, bank);
    raven_log("FMOD: loadBankFile %s -> %d\n", buf, r);
    return r;
}
static SV R s_Studio_SetListener(H s, int l, const void *a) { CALL(FMOD_Studio_System_SetListenerAttributes, (H, int, const void *), s, l, a); }
static SV R s_Studio_GetLowLevel(H s, H *o) { CALL(FMOD_Studio_System_GetLowLevelSystem, (H, H *), s, o); }
// Bus names + last mute/volume, so the log shows when the game itself turns a bus down
// (RAVEN_FMODLOG=1, or always for the first 200 changes).
static struct { H b; char name[64]; float vol; int mute; } g_bus[32];
static int g_nbus;
static int bus_idx(H b) {
    for (int i = 0; i < g_nbus; i++) if (g_bus[i].b == b) return i;
    return -1;
}
static void bus_log(H b, const char *what, float v) {
    static int n;
    int i = bus_idx(b);
    if (i >= 0) {
        if (!strcmp(what, "volume") ? g_bus[i].vol == v : g_bus[i].mute == (int)v) return;
        if (!strcmp(what, "volume")) g_bus[i].vol = v; else g_bus[i].mute = (int)v;
    }
    if (n++ < 200 || raven_env("RAVEN_FMODLOG"))
        raven_log("FMOD: bus %s %s %.2f (t=%lus)\n", i >= 0 ? g_bus[i].name : "?", what, v, GetTickCount() / 1000);
}
static SV R s_Studio_GetBus(H s, const char *p, H *o) {
    static R (*f)(H, const char *, H *);
    if (!f && !(f = (R (*)(H, const char *, H *))fsym("FMOD_Studio_System_GetBus"))) return ERR_INIT;
    R r = f(s, p, o);
    if (!r && o && *o && bus_idx(*o) < 0 && g_nbus < 32) {
        g_bus[g_nbus].b = *o; g_bus[g_nbus].vol = -1; g_bus[g_nbus].mute = -1;
        snprintf(g_bus[g_nbus].name, sizeof g_bus[0].name, "%s", p ? p : "?");
        g_nbus++;
    }
    return r;
}
static SV R s_Studio_GetEvent(H s, const char *p, H *o) { CALL(FMOD_Studio_System_GetEvent, (H, const char *, H *), s, p, o); }

// ---- Studio::Bank / Bus ----
static SV R s_Bank_LoadSample(H b) { CALL(FMOD_Studio_Bank_LoadSampleData, (H), b); }
static SV R s_Bank_Unload(H b) { raven_log("FMOD: bank unload %p\n", b); CALL(FMOD_Studio_Bank_Unload, (H), b); }
static SV R s_Bus_StopAll(H b, int m) { CALL(FMOD_Studio_Bus_StopAllEvents, (H, int), b, m); }
static SV R s_Bus_SetMute(H b, _Bool m) { bus_log(b, "mute", m); CALL(FMOD_Studio_Bus_SetMute, (H, int), b, m); }
static SV R s_Bus_SetVolume(H b, float v) {
    // The game silences the master bus between attract cycles (cabinet "attract sound" setting,
    // no value of attractSoundFreq keeps it on). Keep the last level instead; RAVEN_ATTRACTMUTE=1 = game's way.
    static float last = 1.0f;
    int i = bus_idx(b);
    if (i >= 0 && !strcmp(g_bus[i].name, "bus:/") && !raven_env("RAVEN_ATTRACTMUTE")) {
        if (v > 0.001f) last = v;
        else { static int said; if (!said++) raven_log("FMOD: master bus mute blocked, keeping %.2f\n", last); v = last; }
    }
    bus_log(b, "volume", v); CALL(FMOD_Studio_Bus_SetVolume, (H, float), b, v); }
static SV R s_Bus_GetCG(H b, H *o) { CALL(FMOD_Studio_Bus_GetChannelGroup, (H, H *), b, o); }

// ---- Studio::EventDescription ----
static SV R s_Desc_Create(H d, H *o) { CALL(FMOD_Studio_EventDescription_CreateInstance, (H, H *), d, o); }
static SV R s_Desc_List(H d, H *a, int cap, int *n) { CALL(FMOD_Studio_EventDescription_GetInstanceList, (H, H *, int, int *), d, a, cap, n); }
static SV R s_Desc_Count(H d, int *n) { CALL(FMOD_Studio_EventDescription_GetInstanceCount, (H, int *), d, n); }
static SV R s_Desc_MaxDist(H d, float *v) { CALL(FMOD_Studio_EventDescription_GetMaximumDistance, (H, float *), d, v); }
static SV R s_Desc_MinDist(H d, float *v) { CALL(FMOD_Studio_EventDescription_GetMinimumDistance, (H, float *), d, v); }
static SV R s_Desc_Path(H d, char *p, int n, int *got) { CALL(FMOD_Studio_EventDescription_GetPath, (H, char *, int, int *), d, p, n, got); }

// ---- Studio::EventInstance ----
static SV R s_Ev_SetCallback(H e, sv_evcb cb, unsigned mask) {
    if (cb && g_evcb && cb != g_evcb) raven_log("FMOD: warning, second event callback %p\n", (void *)cb);
    if (cb) g_evcb = cb;
    CALL(FMOD_Studio_EventInstance_SetCallback, (H, void *, unsigned), e, cb ? (void *)ms_evcb : NULL, mask);
}
static SV R s_Ev_SetUser(H e, void *u) { CALL(FMOD_Studio_EventInstance_SetUserData, (H, void *), e, u); }
static SV R s_Ev_GetUser(H e, void **u) { CALL(FMOD_Studio_EventInstance_GetUserData, (H, void **), e, u); }
static SV R s_Ev_Set3D(H e, const void *a) { CALL(FMOD_Studio_EventInstance_Set3DAttributes, (H, const void *), e, a); }
static SV R s_Ev_Get3D(H e, void *a) { CALL(FMOD_Studio_EventInstance_Get3DAttributes, (H, void *), e, a); }
static SV R s_Ev_SetParam(H e, const char *n, float v) { CALL(FMOD_Studio_EventInstance_SetParameterValue, (H, const char *, float), e, n, v); }
static SV R s_Ev_Stop(H e, int m) { CALL(FMOD_Studio_EventInstance_Stop, (H, int), e, m); }
static SV R s_Ev_Start(H e) { CALL(FMOD_Studio_EventInstance_Start, (H), e); }
static SV R s_Ev_Release(H e) { CALL(FMOD_Studio_EventInstance_Release, (H), e); }
static SV R s_Ev_SetVolume(H e, float v) { CALL(FMOD_Studio_EventInstance_SetVolume, (H, float), e, v); }
static SV R s_Ev_GetDesc(H e, H *o) { CALL(FMOD_Studio_EventInstance_GetDescription, (H, H *), e, o); }
static SV _Bool s_Ev_IsValid(H e) {
    static int (*f)(H);
    if (!f && !(f = (int (*)(H))fsym("FMOD_Studio_EventInstance_IsValid"))) return 0;
    return f(e) != 0;
}

// ---- low-level System ----
static SV R s_Sys_CreateSound(H s, const char *name, unsigned mode, void *ex, H *o) {
    static R (*f)(H, const char *, unsigned, void *, H *);
    if (!f && !(f = (R (*)(H, const char *, unsigned, void *, H *))fsym("FMOD_System_CreateSound"))) return ERR_INIT;
    char buf[MAX_PATH * 2];
    if (name && !(mode & (0x8 | 0x800 | 0x10000000))) name = bank_path(name, buf, sizeof buf);  // not user/memory
    static int warned;
    if (ex && !warned++) raven_log("FMOD: createSound with exinfo (any callbacks in it would be SysV)\n");
    return f(s, name, mode, ex, o);
}
static SV R s_Sys_Get3D(H s, float *a, float *b, float *c) { CALL(FMOD_System_Get3DSettings, (H, float *, float *, float *), s, a, b, c); }
static SV R s_Sys_Set3D(H s, float a, float b, float c) { CALL(FMOD_System_Set3DSettings, (H, float, float, float), s, a, b, c); }
static SV R s_Sys_DriverInfo(H s, int id, char *name, int len, void *guid, int *rate, int *mode, int *ch) {
    CALL(FMOD_System_GetDriverInfo, (H, int, char *, int, void *, int *, int *, int *), s, id, name, len, guid, rate, mode, ch);
}
static SV R s_Sys_NumDrivers(H s, int *n) { CALL(FMOD_System_GetNumDrivers, (H, int *), s, n); }
static SV R s_Sys_GetDriver(H s, int *n) { CALL(FMOD_System_GetDriver, (H, int *), s, n); }
static SV R s_Sys_SetDriver(H s, int n) { CALL(FMOD_System_SetDriver, (H, int), s, n); }
static SV R s_Sys_SetFormat(H s, int rate, int mode, int raw) {
    int m = raven_env("RAVEN_SURROUND") ? mode : 3;  // FMOD_SPEAKERMODE_STEREO
    raven_log("FMOD: setSoftwareFormat(%d, speakermode %d -> %d, %d)\n", rate, mode, m, raw);
    CALL(FMOD_System_SetSoftwareFormat, (H, int, int, int), s, rate, m, raw);
}
static SV R s_Sys_PlaySound(H s, H snd, H cg, _Bool paused, H *ch) { CALL(FMOD_System_PlaySound, (H, H, H, int, H *), s, snd, cg, paused, ch); }
static SV R s_Sound_Release(H s) { CALL(FMOD_Sound_Release, (H), s); }

// ---- Channel / ChannelControl / DSP ----
static SV R s_Ch_SetPos(H c, unsigned p, unsigned u) { CALL(FMOD_Channel_SetPosition, (H, unsigned, unsigned), c, p, u); }
static SV R s_Ch_SetLoop(H c, int n) { CALL(FMOD_Channel_SetLoopCount, (H, int), c, n); }
static SV R s_CC_GetUser(H c, void **u) { CC(GetUserData, (H, void **), c, u); }
static SV R s_CC_SetUser(H c, void *u) { CC(SetUserData, (H, void *), c, u); }
static SV R s_CC_SetCallback(H c, sv_chcb cb) {
    if (cb && g_chcb && cb != g_chcb) raven_log("FMOD: warning, second channel callback %p\n", (void *)cb);
    if (cb) g_chcb = cb;
    CC(SetCallback, (H, void *), c, cb ? (void *)ms_chcb : NULL);
}
static SV R s_CC_Set3D(H c, const VEC *p, const VEC *v, const VEC *a) { CC(Set3DAttributes, (H, const VEC *, const VEC *, const VEC *), c, p, v, a); }
static SV R s_CC_Stop(H c) { CC(Stop, (H), c); }
static SV R s_CC_GetDSP(H c, int i, H *o) { CC(GetDSP, (H, int, H *), c, i, o); }
static SV R s_CC_SetPan(H c, float p) { CC(SetPan, (H, float), c, p); }
static SV R s_CC_SetMode(H c, unsigned m) { CC(SetMode, (H, unsigned), c, m); }
static SV R s_CC_SetPaused(H c, _Bool p) { CC(SetPaused, (H, int), c, p); }
static SV R s_CC_SetVolume(H c, float v) {
    if (c && c == g_master) v *= g_boost;  // keep the boost if the game sets the master level
    CC(SetVolume, (H, float), c, v);
}
static SV R s_DSP_GetMeter(H d, void *in, void *out) { CALL(FMOD_DSP_GetMeteringInfo, (H, void *, void *), d, in, out); }
static SV R s_DSP_SetMeter(H d, _Bool in, _Bool out) { CALL(FMOD_DSP_SetMeteringEnabled, (H, int, int), d, in, out); }

const shim_entry shims_fmod[] = {
    {"FMOD_Memory_Initialize", s_Memory_Initialize},
    {"FMOD_Debug_Initialize", s_Debug_Initialize},
    {"_ZN4FMOD6Studio6System6createEPPS1_j", s_Studio_Create},
    {"_ZN4FMOD6Studio6System10initializeEijjPv", s_Studio_Initialize},
    {"_ZN4FMOD6Studio6System7releaseEv", s_Studio_Release},
    {"_ZN4FMOD6Studio6System6updateEv", s_Studio_Update},
    {"_ZN4FMOD6Studio6System13flushCommandsEv", s_Studio_Flush},
    {"_ZN4FMOD6Studio6System12loadBankFileEPKcjPPNS0_4BankE", s_Studio_LoadBank},
    {"_ZN4FMOD6Studio6System21setListenerAttributesEiPK18FMOD_3D_ATTRIBUTES", s_Studio_SetListener},
    {"_ZNK4FMOD6Studio6System17getLowLevelSystemEPPNS_6SystemE", s_Studio_GetLowLevel},
    {"_ZNK4FMOD6Studio6System6getBusEPKcPPNS0_3BusE", s_Studio_GetBus},
    {"_ZNK4FMOD6Studio6System8getEventEPKcPPNS0_16EventDescriptionE", s_Studio_GetEvent},
    {"_ZN4FMOD6Studio4Bank14loadSampleDataEv", s_Bank_LoadSample},
    {"_ZN4FMOD6Studio4Bank6unloadEv", s_Bank_Unload},
    {"_ZN4FMOD6Studio3Bus13stopAllEventsE21FMOD_STUDIO_STOP_MODE", s_Bus_StopAll},
    {"_ZN4FMOD6Studio3Bus7setMuteEb", s_Bus_SetMute},
    {"_ZN4FMOD6Studio3Bus9setVolumeEf", s_Bus_SetVolume},
    {"_ZNK4FMOD6Studio3Bus15getChannelGroupEPPNS_12ChannelGroupE", s_Bus_GetCG},
    {"_ZNK4FMOD6Studio16EventDescription14createInstanceEPPNS0_13EventInstanceE", s_Desc_Create},
    {"_ZNK4FMOD6Studio16EventDescription15getInstanceListEPPNS0_13EventInstanceEiPi", s_Desc_List},
    {"_ZNK4FMOD6Studio16EventDescription16getInstanceCountEPi", s_Desc_Count},
    {"_ZNK4FMOD6Studio16EventDescription18getMaximumDistanceEPf", s_Desc_MaxDist},
    {"_ZNK4FMOD6Studio16EventDescription18getMinimumDistanceEPf", s_Desc_MinDist},
    {"_ZNK4FMOD6Studio16EventDescription7getPathEPciPi", s_Desc_Path},
    {"_ZN4FMOD6Studio13EventInstance11setCallbackEPF11FMOD_RESULTjP25FMOD_STUDIO_EVENTINSTANCEPvEj", s_Ev_SetCallback},
    {"_ZN4FMOD6Studio13EventInstance11setUserDataEPv", s_Ev_SetUser},
    {"_ZNK4FMOD6Studio13EventInstance11getUserDataEPPv", s_Ev_GetUser},
    {"_ZN4FMOD6Studio13EventInstance15set3DAttributesEPK18FMOD_3D_ATTRIBUTES", s_Ev_Set3D},
    {"_ZNK4FMOD6Studio13EventInstance15get3DAttributesEP18FMOD_3D_ATTRIBUTES", s_Ev_Get3D},
    {"_ZN4FMOD6Studio13EventInstance17setParameterValueEPKcf", s_Ev_SetParam},
    {"_ZN4FMOD6Studio13EventInstance4stopE21FMOD_STUDIO_STOP_MODE", s_Ev_Stop},
    {"_ZN4FMOD6Studio13EventInstance5startEv", s_Ev_Start},
    {"_ZN4FMOD6Studio13EventInstance7releaseEv", s_Ev_Release},
    {"_ZN4FMOD6Studio13EventInstance9setVolumeEf", s_Ev_SetVolume},
    {"_ZNK4FMOD6Studio13EventInstance14getDescriptionEPPNS0_16EventDescriptionE", s_Ev_GetDesc},
    {"_ZNK4FMOD6Studio13EventInstance7isValidEv", s_Ev_IsValid},
    {"_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE", s_Sys_CreateSound},
    {"_ZN4FMOD6System13get3DSettingsEPfS1_S1_", s_Sys_Get3D},
    {"_ZN4FMOD6System13set3DSettingsEfff", s_Sys_Set3D},
    {"_ZN4FMOD6System13getDriverInfoEiPciP9FMOD_GUIDPiP16FMOD_SPEAKERMODES4_", s_Sys_DriverInfo},
    {"_ZN4FMOD6System13getNumDriversEPi", s_Sys_NumDrivers},
    {"_ZN4FMOD6System9getDriverEPi", s_Sys_GetDriver},
    {"_ZN4FMOD6System9setDriverEi", s_Sys_SetDriver},
    {"_ZN4FMOD6System17setSoftwareFormatEi16FMOD_SPEAKERMODEi", s_Sys_SetFormat},
    {"_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE", s_Sys_PlaySound},
    {"_ZN4FMOD5Sound7releaseEv", s_Sound_Release},
    {"_ZN4FMOD7Channel11setPositionEjj", s_Ch_SetPos},
    {"_ZN4FMOD7Channel12setLoopCountEi", s_Ch_SetLoop},
    {"_ZN4FMOD14ChannelControl11getUserDataEPPv", s_CC_GetUser},
    {"_ZN4FMOD14ChannelControl11setUserDataEPv", s_CC_SetUser},
    {"_ZN4FMOD14ChannelControl11setCallbackEPF11FMOD_RESULTP19FMOD_CHANNELCONTROL24FMOD_CHANNELCONTROL_TYPE33FMOD_CHANNELCONTROL_CALLBACK_TYPEPvS6_E", s_CC_SetCallback},
    {"_ZN4FMOD14ChannelControl15set3DAttributesEPK11FMOD_VECTORS3_S3_", s_CC_Set3D},
    {"_ZN4FMOD14ChannelControl4stopEv", s_CC_Stop},
    {"_ZN4FMOD14ChannelControl6getDSPEiPPNS_3DSPE", s_CC_GetDSP},
    {"_ZN4FMOD14ChannelControl6setPanEf", s_CC_SetPan},
    {"_ZN4FMOD14ChannelControl7setModeEj", s_CC_SetMode},
    {"_ZN4FMOD14ChannelControl9setPausedEb", s_CC_SetPaused},
    {"_ZN4FMOD14ChannelControl9setVolumeEf", s_CC_SetVolume},
    {"_ZN4FMOD3DSP15getMeteringInfoEP22FMOD_DSP_METERING_INFOS2_", s_DSP_GetMeter},
    {"_ZN4FMOD3DSP18setMeteringEnabledEbb", s_DSP_SetMeter},
    {NULL, NULL},
};
