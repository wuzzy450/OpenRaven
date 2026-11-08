// Linux FMOD mode (default): the game links to its own Linux FMOD 1.10 (openraven/extralibs/
// libfmod.so.10 + libfmodstudio.so.10, loaded by main.c). Sound goes out through shim_alsa.c.
// Only three calls are wrapped, for the same fixes the Windows-DLL path (shim_fmod.c) has:
//   System::setSoftwareFormat  -> stereo (FMOD does the 5.1 downmix; RAVEN_SURROUND=1 keeps 5.1)
//   Studio::System::getBus     -> remember which handle is the master bus "bus:/"
//   Studio::Bus::setVolume     -> ignore the game muting the master bus in attract mode
//                                 (RAVEN_ATTRACTMUTE=1 = game's way)
#include <stdlib.h>
#include <string.h>
#include "shim.h"
#include "elfload.h"

typedef int R; typedef void *H;
int raven_fmod_linux;   // set by main.c when the Linux FMOD libraries are loaded

static void *real(const char *n) {
    void *p = elf_find_export(n);
    if (!p) raven_log("FMOD (Linux): %s not exported\n", n);
    return p;
}

static SV R w_setformat(H s, int rate, int mode, int raw) {
    static SV R (*f)(H, int, int, int);
    if (!f && !(f = real("_ZN4FMOD6System17setSoftwareFormatEi16FMOD_SPEAKERMODEi"))) return 30;
    int m = raven_env("RAVEN_SURROUND") ? mode : 3;   // FMOD_SPEAKERMODE_STEREO
    raven_log("FMOD (Linux): setSoftwareFormat(%d, speakermode %d -> %d, %d)\n", rate, mode, m, raw);
    return f(s, rate, m, raw);
}

static H g_masterbus;
static SV R w_getbus(H sys, const char *path, H *out) {
    static SV R (*f)(H, const char *, H *);
    if (!f && !(f = real("_ZNK4FMOD6Studio6System6getBusEPKcPPNS0_3BusE"))) return 30;
    R r = f(sys, path, out);
    if (!r && out && *out && path && !strcmp(path, "bus:/")) g_masterbus = *out;
    return r;
}

static SV R w_bus_setvolume(H b, float v) {
    static SV R (*f)(H, float);
    if (!f && !(f = real("_ZN4FMOD6Studio3Bus9setVolumeEf"))) return 30;
    static float last = 1.0f;
    if (b && b == g_masterbus && !raven_env("RAVEN_ATTRACTMUTE")) {
        if (v > 0.001f) last = v;
        else { static int said; if (!said++) raven_log("FMOD: master bus mute blocked, keeping %.2f\n", last); v = last; }
    }
    return f(b, v);
}

const shim_entry shims_fmodlx[] = {
    {"_ZN4FMOD6System17setSoftwareFormatEi16FMOD_SPEAKERMODEi", w_setformat},
    {"_ZNK4FMOD6Studio6System6getBusEPKcPPNS0_3BusE", w_getbus},
    {"_ZN4FMOD6Studio3Bus9setVolumeEf", w_bus_setvolume},
    {NULL, NULL},
};
