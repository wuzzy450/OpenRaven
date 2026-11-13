// rio2pad for the native Windows loader: feeds XInput pads (and a keyboard fallback) into the
// game in place of the RIO2 cabinet I/O board. Port of openraven/rio2pad.c (Linux LD_PRELOAD).
//
// Same game hooks as Linux: sw_get(id) @0x61e890 (all cabinet switches), adj_get @0x53f270
// (free play, volume), gun getters @0xdd4b10/0xdd6c60, cabinet type @0x1800e60, boot skip.
// Every hook is called by game code, so it is SV.
//
// Dropped from the Linux version (not needed here):
//   - shutdown join patch @0x46a34a: our pthread_join/timedjoin shims accept a null thread.
//   - envelope dlclose patch @0x1f5cd80: our dlclose shim is already a no-op.
//   - RIO2PAD_STATE_FILE (the WSL XInput bridge): we read XInput directly.
//
// Pads: XInput user N = player N+1. Start (or A) = START, Back = TEST, Y = coin (FREEPLAY=0),
// RT = fire, RB = reload, LT = grenade, sticks = aim.
// Keyboard (only while a game window has focus): Enter/1 = P1 START, 2 = P2 START, F2 = TEST,
// 5/6 = P1/P2 coin, Space = P1 fire, R = P1 reload, G = P1 grenade, arrows = P1 aim.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "shim.h"

#define SW_GET        0x61e890ULL
#define SW_GET_JS_TGT 0x61ee68ULL
#define SW_GET_RESUME 0x61e89eULL
#define G_START_IDS   ((volatile int *)0x16acc60ULL)
#define G_TEST_IDS    ((volatile int *)0x16acc70ULL)
#define G_CABINET_TYPE ((volatile int *)0x1800e60ULL)
#define IRT_SET_CABTYPE   0xdd4bc0ULL
#define IRT_GUN_CONNECTED 0xdd4b10ULL
#define IRT_GUN_AIM       0xdd6c60ULL
#define ADJ_GET           0x53f270ULL

#define MAX_PADS 4
#define MAX_SW   0x800

// Linux joystick (xpad) layout, kept so the mapping matches rio2pad.c.
enum { PAD_A = 0, PAD_B = 1, PAD_X = 2, PAD_Y = 3, PAD_LB = 4, PAD_RB = 5, PAD_BACK = 6, PAD_START = 7 };

struct pad {
    int active;                  // 1 = XInput pad, 2 = keyboard only (player 1)
    volatile int btn[16];
    volatile int axis[8];        // 0 LX, 1 LY (down +), 2 LT, 3 RX, 4 RY (down +), 5 RT; triggers -32767..32767
    volatile long long coin_t;
};
static struct pad g_pads[MAX_PADS];

static int g_debug, g_autostart, g_probe = -1, g_freeplay = 1, g_continueshoot, g_proclog, g_skipboot, g_gunlevel;
static int g_aimsweep;
static int g_coin_ids[MAX_PADS] = {570, 571, 602, 603};   // 4-player cabinet: 2nd board = ids + 32
static volatile unsigned g_swcount[MAX_SW];
static signed char g_swstate[MAX_SW];
static FILE *g_inputlog;
static const char *g_swlog;
static unsigned char g_fake[8][64];
static volatile LONG g_fake_i;
static int g_gamevol = -1;
static long long g_t_start;

typedef SV void *(*sw_get_fn)(int);
static sw_get_fn sw_get_orig;
typedef SV int (*adj_get_fn)(const char *, const char *);
static adj_get_fn adj_get_orig;

static float g_aim[MAX_PADS][2];
int rio2pad_aim_mode;
float rio2pad_stick_speed = 0.25f;
// Mouse drives player 1: 0 off, 1 screen (cursor position = aim, like a light gun),
// 2 relative (mouse moves the aim, cursor kept in the window centre). Sens only for relative.
int rio2pad_mouse_mode = 1;
float rio2pad_mouse_sens = 1.0f;
static int g_mouse_owns;          // 1 = mouse moved last, so it drives P1's aim
static int g_mouse_btn[3];        // left fire, right reload, middle grenade

static long long now_ms(void) { return (long long)GetTickCount64(); }

static int env_int(const char *n, int def) { const char *v = raven_env(n); return v ? atoi(v) : def; }

static int coin_pulse(int p) { return g_pads[p].active && now_ms() - g_pads[p].coin_t < 60; }
static int btn(int p, int b) { return g_pads[p].active && g_pads[p].btn[b]; }

static SV int gun_connected(int g) { return g >= 0 && g < MAX_PADS; }
static SV void gun_aim(int g, float *x, float *y)
{
    if (!x || !y) return;
    if (!gun_connected(g)) { *x = *y = -1.0f; return; }
    static int trace = -1;
    if (trace < 0) trace = raven_env("RAVEN_AIMTRACE") != NULL;
    if (trace) {   // debug: log every distinct place in the game that asks for the aim
        static void *seen[64]; static int ns;
        void *ra = __builtin_return_address(0);
        int k = 0;
        while (k < ns && seen[k] != ra) k++;
        if (k == ns && !ns) {   // first time: code addresses on the stack = the chain of callers
            uint64_t *sp = (uint64_t *)__builtin_frame_address(0);
            char line[900]; int o = 0;
            for (int i = 0; i < 400 && o < 860; i++) if (sp[i] > 0x400000ULL && sp[i] < 0x1400000ULL) o += snprintf(line + o, sizeof line - o, " %llx", (unsigned long long)sp[i]);
            raven_log("[aimtrace] stack:%s\n", line);
        }
        if (k == ns && ns < 64) { seen[ns++] = ra; raven_log("[aimtrace] gun %d asked from %p (thread %lu)\n", g, ra, GetCurrentThreadId()); }
    }
    extern int rio2pad_coop_mode(void);
    float ax = g_aim[g][0], ay = g_aim[g][1];
    extern unsigned rio2pad_coop_frame_no(void);
    if (rio2pad_coop_mode() && rio2pad_coop_frame_no()) { extern float rio2pad_coop_aim(int g, int k); ax = rio2pad_coop_aim(g, 0); ay = rio2pad_coop_aim(g, 1); }
    // hybrid menu view shows only the left half: keep every cursor inside it (also the stored
    // aim, so a stick does not drift on into the hidden half)
    extern int raven_view;
    if (raven_view == 1 && ax > 0.5f) { ax = 0.5f; if (!rio2pad_coop_mode()) g_aim[g][0] = 0.5f; }
    *x = ax;
    *y = 1.0f - ay;
}

static int proc_running(const char *name)
{
    for (int i = 0; i < 64; i++) {
        const char *slot = (const char *)(0x1dd78c0ULL + i * 0x448ULL);
        if ((slot[0x28] & 1) && !strncmp(slot, name, 0x28)) return 1;
    }
    return 0;
}

static int boot_start_pulse(void)
{
    static int done;
    if (!g_skipboot || done) return 0;
    if (proc_running("AtProc")) {
        done = 1;
        raven_log("[rio2pad] skipboot: attract reached after %lld ms\n", now_ms() - g_t_start);
        return 0;
    }
    return (now_ms() / 250) & 1;
}

static int start_pressed(int p)
{
    if (p == 0 && g_autostart && (time(NULL) & 1)) return 1;
    return btn(p, PAD_START) || btn(p, PAD_A);
}

#define PLAYER_STATE(slot) (*(volatile unsigned char *)(0x1c0c7c0ULL + (slot) * 0x250ULL))
static int in_continue(int p)
{
    static int prev[MAX_PADS] = {-1, -1, -1, -1};
    int s = PLAYER_STATE(p == 0 ? 0 : 3);
    if (s != prev[p]) { if (g_debug) raven_log("[rio2pad] P%d state %d\n", p + 1, s); prev[p] = s; }
    return s == 2;
}

static int in_trig(int p);
static int trigger_pressed(int p)
{
    static int block[MAX_PADS];
    int down = in_trig(p);
    if (g_continueshoot) return down;
    if (in_continue(p)) { block[p] = 1; return 0; }
    if (block[p] && down) return 0;
    block[p] = 0;
    return down;
}

static int test_pressed(void)
{
    for (int p = 0; p < MAX_PADS; p++)
        if (btn(p, PAD_BACK)) return 1;
    return 0;
}

// ---- coop: one input snapshot per simulation frame, recorded or replayed ----
// RIO2PAD_RECORD=file writes every frame's snapshot, RIO2PAD_REPLAY=file feeds it back instead of the
// pads. In either mode sw_get / gun_aim serve the snapshot taken at the frame start (coop_win.c calls
// rio2pad_coop_frame from the game's frame-time update), so input only changes on frame boundaries.
enum { CI_START = 1, CI_TRIG = 2, CI_RELOAD = 4, CI_GREN = 8, CI_TEST = 16, CI_COIN = 32 };
#include "coop.h"
static struct coop_in g_ci;
static int g_coop_mode;            // 0 live, 1 record, 2 replay, 3 online (coopnet_win.c)
static FILE *g_coop_f;
static volatile unsigned g_coop_frame;
static const char REC_MAGIC[8] = "RVREC01";

static int start_pressed(int p);
static int boot_start_pulse(void);
static int test_pressed(void);

static void coop_sample_live(struct coop_in *c)
{
    memset(c, 0, sizeof *c);
    for (int p = 0; p < MAX_PADS; p++) {
        uint8_t b = 0;
        if (start_pressed(p) || (p == 0 && boot_start_pulse())) b |= CI_START;
        if (g_pads[p].active && g_pads[p].axis[5] > 0) b |= CI_TRIG;
        if (btn(p, PAD_RB)) b |= CI_RELOAD;
        if (g_pads[p].active && g_pads[p].axis[2] > 0) b |= CI_GREN;
        if (p == 0 && test_pressed()) b |= CI_TEST;
        if (coin_pulse(p)) b |= CI_COIN;
        c->bits[p] = b;
        c->aim[p][0] = g_aim[p][0];
        c->aim[p][1] = g_aim[p][1];
    }
}

int rio2pad_coop_mode(void) { return g_coop_mode; }
float rio2pad_coop_aim(int g, int k) { return g_ci.aim[g][k]; }
unsigned rio2pad_coop_frame_no(void) { return g_coop_frame; }

void rio2pad_coop_frame(unsigned frame)
{
    struct coop_in c;
    if (g_coop_mode == 2) {
        unsigned f;
        if (fread(&f, 4, 1, g_coop_f) == 1 && fread(&c, sizeof c, 1, g_coop_f) == 1) {
            if (f != frame) raven_log("[coop] replay frame mismatch: file %u, game %u\n", f, frame);
        } else {
            static int said;
            if (!said++) raven_log("[coop] replay file ended at frame %u, inputs released\n", frame);
            memset(&c, 0, sizeof c);
            for (int p = 0; p < MAX_PADS; p++) c.aim[p][0] = c.aim[p][1] = 0.5f;
        }
    } else if (g_coop_mode == 3) {   // online: this PC's first pad / mouse / keyboard is its player
        coop_sample_live(&c);
        coopnet_frame(frame, c.bits[0], c.aim[0][0], c.aim[0][1], &c);
    } else {
        coop_sample_live(&c);
        if (g_coop_mode == 1) {
            fwrite(&frame, 4, 1, g_coop_f);
            fwrite(&c, sizeof c, 1, g_coop_f);
            if ((frame & 63) == 0) fflush(g_coop_f);
        }
    }
    g_ci = c;
    g_coop_frame = frame;
}

static void coop_open(void)
{
    const char *rec = raven_env("RIO2PAD_RECORD"), *rep = raven_env("RIO2PAD_REPLAY");
    coopnet_init();
    if (coopnet_active()) { g_coop_mode = 3; return; }
    if (rep) {
        g_coop_f = fopen(rep, "rb");
        char m[8];
        if (!g_coop_f || fread(m, 8, 1, g_coop_f) != 1 || memcmp(m, REC_MAGIC, 8)) {
            raven_log("[coop] cannot replay %s (missing or not a recording)\n", rep);
            if (g_coop_f) fclose(g_coop_f);
            g_coop_f = NULL;
            return;
        }
        g_coop_mode = 2;
        raven_log("[coop] replaying inputs from %s\n", rep);
    } else if (rec) {
        g_coop_f = fopen(rec, "wb");
        if (!g_coop_f) { raven_log("[coop] cannot write %s\n", rec); return; }
        fwrite(REC_MAGIC, 8, 1, g_coop_f);
        g_coop_mode = 1;
        raven_log("[coop] recording inputs to %s\n", rec);
    }
}

// Inputs as the game hooks see them: the frame snapshot in coop modes, live otherwise.
static int ci(int p, int bit) { return (g_ci.bits[p] & bit) != 0; }
static int in_start(int p) {
    int r = g_coop_mode && g_coop_frame ? ci(p, CI_START) : start_pressed(p) || (p == 0 && boot_start_pulse());
    static int said[4];
    if (r && p > 0 && said[p]++ < 3) raven_log("[rio2pad] DIAG P%d start: coop %d frame %u active %d btnS %d btnA %d ci %02x\n", p + 1, g_coop_mode, g_coop_frame, g_pads[p].active, g_pads[p].btn[PAD_START], g_pads[p].btn[PAD_A], g_ci.bits[p]);
    return r;
}
static int in_trig(int p) { return g_coop_mode && g_coop_frame ? ci(p, CI_TRIG) : g_pads[p].active && g_pads[p].axis[5] > 0; }
static int in_reload(int p) { return g_coop_mode && g_coop_frame ? ci(p, CI_RELOAD) : btn(p, PAD_RB); }
static int in_gren(int p) { return g_coop_mode && g_coop_frame ? ci(p, CI_GREN) : g_pads[p].active && g_pads[p].axis[2] > 0; }
static int in_test(void)   { return g_coop_mode && g_coop_frame ? ci(0, CI_TEST) : test_pressed(); }
static int in_coin(int p) { return g_coop_mode && g_coop_frame ? ci(p, CI_COIN) : coin_pulse(p); }

// Gun buttons as the cabinet I/O board reported them to the game's switch dispatcher (0x4ad80c):
// record +4 = new presses, +8 = new releases, +0xc = held. Reporting the level in +4 instead made the
// game see a fresh press every frame and never "held", so full-auto weapons (needler, plasma rifle)
// never got their automatic fire. One physical button can feed two logical ids, so the dispatcher may
// ask twice in one poll: edges are worked out once per poll (calls < 3 ms apart share them).
#define SW_DISPATCH_RET 0x4ad811ULL
// Gun switch ids -> player * 5 + role (role 2 trigger, 3 reload, 4 grenade; 0/1 unused), or -1.
// 2-player cabinet: guns 1/2 = 793..802, 5 each. 4-player cabinet (type 9): other ids, read from the
// game's dispatcher table (RIO2PAD_SWLOG "table" line): board 1 = 586..596, board 2 = 618..628.
static int g_players4;
static int gun_index(int id)
{
    if (id >= 793 && id <= 802) return id - 793;
    // 4-player cabinet: the dispatcher polls the IR-gun device channels for all 4 guns
    // (793-812, 5 per gun: aim X/Y, trigger, reload, grenade); P3/P4 are 803-812 and
    // were missing here, so their trigger/reload/grenade stayed "never pressed".
    if (g_players4 && id >= 803 && id <= 812) return id - 803 + 10;
    if (!g_players4) return -1;
    static const short m[][2] = {
        {586, 0}, {587, 1}, {590, 2}, {592, 3}, {591, 4},        // player 1
        {588, 5}, {589, 6}, {594, 7}, {596, 8}, {595, 9},        // player 2
        {618, 10}, {619, 11}, {622, 12}, {624, 13}, {623, 14},   // player 3
        {620, 15}, {621, 16}, {626, 17}, {628, 18}, {627, 19}};  // player 4
    for (unsigned i = 0; i < sizeof m / sizeof *m; i++) if (m[i][0] == id) return m[i][1];
    return -1;
}
static void gun_report(int id, int level, unsigned char *f)
{
    static int last[20], snap_p[20], snap_r[20];
    static double snap_t[20];
    static unsigned snap_f[20];
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    double t = (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
    int k = gun_index(id);
    static int said;
    if (!said) { said = 1; raven_log("[rio2pad] gun buttons: edge + held reporting (RIO2PAD_GUNLEVEL=1 = old way)\n"); }
    if (g_coop_mode ? snap_f[k] != g_coop_frame + 1 : t - snap_t[k] >= 3.0) {
        snap_f[k] = g_coop_frame + 1;
        if (g_debug && level != last[k]) raven_log("[rio2pad] gun switch %d %s\n", id, level ? "pressed" : "released");
        snap_p[k] = level && !last[k];
        snap_r[k] = !level && last[k];
        last[k] = level;
        snap_t[k] = t;
    }
    ((int *)f)[1] = snap_p[k];
    ((int *)f)[2] = snap_r[k];
    ((int *)f)[3] = last[k];
}

static SV void *sw_get_hook(int id)
{
    uint64_t caller = (uint64_t)__builtin_return_address(0);
    void *rec = sw_get_orig(id);
    int pressed = -1;

    if (id >= 0 && id < MAX_SW) g_swcount[id]++;
    // 4-player cabinet: the game never runs irt_systemSetCabinetType itself there, so the gun count
    // (0x1f392d0) stays 0 and the per-frame gun poll (aim) does nothing. Run it once (patched: 4 guns).
    if (g_players4 && *(volatile int *)0x1f392d0ULL == 0) {
        { typedef int SV (*irt_fn)(int); ((irt_fn)IRT_SET_CABTYPE)(10); }
        raven_log("[rio2pad] 4-player cabinet: gun count set to %d\n", *(volatile int *)0x1f392d0ULL);
    }
    for (int i = 0; i < 4; i++)
        if (G_START_IDS[i] && id == G_START_IDS[i]) pressed = in_start(i);
    if (gun_index(id) >= 0) {   // gun buttons, 5 per gun: +2 trigger, +3 reload, +4 grenade
        int p = gun_index(id) / 5, k = gun_index(id) % 5;
        pressed = 0;
        if (k == 2) pressed = trigger_pressed(p);
        if (k == 3) pressed = in_reload(p);
        if (k == 4) pressed = in_gren(p);
    }
    if (id == 581) pressed = in_start(0);   // gun cabinet player START
    if (id == 582) pressed = in_start(1);
    for (int i = 0; i < 3; i++)
        if (G_TEST_IDS[i] && id == G_TEST_IDS[i]) pressed = in_test();
    for (int i = 0; i < MAX_PADS; i++)
        if (g_coin_ids[i] >= 0 && id == g_coin_ids[i]) pressed = in_coin(i);
    if (id == g_probe) pressed = (time(NULL) % 4) == 0;
    if (pressed < 0) return rec;

    if (id >= 0 && id < MAX_SW && pressed != g_swstate[id]) {
        g_swstate[id] = (signed char)pressed;
        if (g_inputlog) {
            fprintf(g_inputlog, "%lld switch %d %s\n", now_ms() - g_t_start, id, pressed ? "DOWN" : "up");
            fflush(g_inputlog);
        }
        if (g_debug) raven_log("[rio2pad] switch %d -> %s\n", id, pressed ? "pressed" : "released");
    }
    unsigned char *f = g_fake[InterlockedIncrement(&g_fake_i) & 7];
    if (rec) memcpy(f, rec, sizeof g_fake[0]);
    else     memset(f, 0, sizeof g_fake[0]);
    if (gun_index(id) >= 0 && caller == SW_DISPATCH_RET && !g_gunlevel) gun_report(id, pressed, f);
    else ((int *)f)[1] = pressed;
    return f;
}

static SV int adj_get_hook(const char *sec, const char *key)
{
    int v = adj_get_orig(sec, key);
    if (sec && key && !strcmp(sec, "sound")) {   // log each sound adjustment once (attract audio RE)
        static char seen[16][48]; static int ns;
        int k = 0;
        while (k < ns && strcmp(seen[k], key)) k++;
        if (k == ns && ns < 16) { snprintf(seen[ns++], sizeof seen[0], "%s", key); raven_log("[rio2pad] adjustment sound/%s = %d\n", key, v); }
    }
    // Attract mode sound: this install has attractVolume 25 (quiet) and attractSoundFreq 1 (sound
    // goes off between cycles). RIO2PAD_ATTRACTVOL (default 100) / RIO2PAD_ATTRACTFREQ override.
    if (key && !strcmp(key, "attractVolume")) {
        const char *e = raven_env("RIO2PAD_ATTRACTVOL");
        return e ? atoi(e) : 100;
    }
    if (key && !strcmp(key, "attractSoundFreq") && raven_env("RIO2PAD_ATTRACTFREQ"))
        return atoi(raven_env("RIO2PAD_ATTRACTFREQ"));
    if (key && !strcmp(key, "freeplay") && g_freeplay) {
        static int said;
        if (!said++) raven_log("[rio2pad] adjustment freeplay = %d -> 1\n", v);
        return 1;
    }
    if (key && strstr(key, "olume") && !strcmp(key, "gameVolume")) {
        int r = g_gamevol >= 0 ? g_gamevol : (v == 0 ? 100 : v);
        static int said;
        if (!said++) raven_log("[rio2pad] adjustment %s/gameVolume = %d -> %d\n", sec ? sec : "?", v, r);
        return r;
    }
    return v;
}

// ---- code patching ----

static void write_abs_jmp(unsigned char *p, uint64_t target)
{
    p[0] = 0xff; p[1] = 0x25; memset(p + 2, 0, 4);   // jmp *0(%rip)
    memcpy(p + 6, &target, 8);
}

static void make_writable(uint64_t addr, size_t len)
{
    DWORD old;
    if (!VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &old))
        raven_log("[rio2pad] VirtualProtect(%#llx) failed: %lu\n", (unsigned long long)addr, GetLastError());
}

static unsigned char *exec_page(void)
{
    return VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static void install_sw_hook(void)
{
    static const unsigned char expect[14] = {0x85,0xff,0x0f,0x88,0xd0,0x05,0x00,0x00,0x81,0xff,0xff,0x01,0x00,0x00};
    unsigned char *f = (unsigned char *)SW_GET;
    if (memcmp(f, expect, 14)) { raven_log("[rio2pad] unexpected bytes at sw_get, not hooking\n"); return; }
    unsigned char *t = exec_page(), *p = t;
    *p++ = 0x85; *p++ = 0xff;                          // test %edi,%edi
    *p++ = 0x79; *p++ = 14;                            // jns +14
    write_abs_jmp(p, SW_GET_JS_TGT); p += 14;
    *p++ = 0x81; *p++ = 0xff; memcpy(p, "\xff\x01\x00\x00", 4); p += 4;   // cmp $0x1ff,%edi
    write_abs_jmp(p, SW_GET_RESUME);
    sw_get_orig = (sw_get_fn)t;
    make_writable(SW_GET, 14);
    write_abs_jmp(f, (uint64_t)sw_get_hook);
}

static void install_adj_hook(void)
{
    static const unsigned char expect[17] = {0x48,0x83,0xec,0x08,0x48,0x89,0xf2,0x31,0xc9,0x48,0x89,0xfe,
                                             0xbf,0xc0,0x3c,0xcc,0x01};
    unsigned char *f = (unsigned char *)ADJ_GET;
    if (memcmp(f, expect, 17)) { raven_log("[rio2pad] unexpected bytes at adj_get, not hooking\n"); return; }
    unsigned char *t = exec_page();
    memcpy(t, f, 17);
    write_abs_jmp(t + 17, ADJ_GET + 17);
    adj_get_orig = (adj_get_fn)t;
    if (g_freeplay) {   // MoneyInCallback @0x447bc0 -> movb $0,(%rsi); ret
        unsigned char *m = (unsigned char *)0x447bc0ULL;
        make_writable(0x447bc0ULL, 7);
        if (!memcmp(m, "\x41\x55\x41\x54\x49\x89\xfd", 7)) memcpy(m, "\xc6\x06\x00\xc3", 4);
        else raven_log("[rio2pad] unexpected bytes at MoneyInCallback, left alone\n");
    }
    make_writable(ADJ_GET, 14);
    write_abs_jmp(f, (uint64_t)adj_get_hook);
}

static void patch_skip_boot_checks(void)
{
    static const uint64_t sites[] = {0x417edd, 0x417f08};   // proc_start("GunCalProc")
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
        unsigned char *c = (unsigned char *)sites[i];
        if (c[0] != 0xe8 || sites[i] + 5 + *(int32_t *)(c + 1) != 0x5aa0b0ULL) {
            raven_log("[rio2pad] skipboot: unexpected code at %#llx, left alone\n", (unsigned long long)sites[i]);
            continue;
        }
        make_writable(sites[i], 5);
        memset(c, 0x90, 5);
    }
    unsigned char *nc = (unsigned char *)0x417aa0ULL;   // guns_need_cal() -> 0
    if (!memcmp(nc, "\x53\x48\x83\xec\x50", 5)) { make_writable(0x417aa0ULL, 3); memcpy(nc, "\x31\xc0\xc3", 3); }
    else raven_log("[rio2pad] skipboot: unexpected code at 0x417aa0, left alone\n");
    raven_log("[rio2pad] skipboot: gun calibration disabled, START pulsed until attract\n");
}

static void patch_misc(void)
{
    // irt_systemSetCabinetType: jump table for irt types 7..11; index 0 = 2 guns, index 3 = 4 guns
    // (sets the gun count 0x1f392d0 that the per-frame gun poll 0x624110 loops over).
    unsigned char *f = (unsigned char *)IRT_SET_CABTYPE;
    if (!memcmp(f, "\x8d\x47\xf9", 3)) {
        make_writable(IRT_SET_CABTYPE, 3);
        memcpy(f, env_int("RIO2PAD_GUNS", g_players4 ? 4 : 2) >= 3 ? "\x6a\x03\x58" : "\x31\xc0\x90", 3);   // push 3; pop rax  /  xor eax,eax
    } else raven_log("[rio2pad] unexpected irt cabinet-type code, not patching\n");
    // 4-player cabinet: the game never calls this itself, and the gun device (poll 0x624110) is switched
    // off for good when its first poll finds 0 guns. So set the gun count now, before the game runs.
    if (g_players4) { typedef int SV (*irt_fn)(int); ((irt_fn)IRT_SET_CABTYPE)(10); }
    // I/O device list (0x436400): cabinet types 3 and 8 include the gun device (mask bit 8, poll
    // 0x624110, the one our gun hooks feed); the 4-player cabinet has other gun hardware (bit 0x800)
    // and leaves it out, so nothing reads the aim. Add bit 8 to every mask the other types can get.
    if (g_players4) {
        static const uint64_t imm[] = {0x43642f, 0x436435, 0x43643b, 0x436440, 0x436445, 0x43644a, 0x43644f, 0x436454};
        int n = 0;
        for (unsigned i = 0; i < 8; i++) {
            unsigned char *b = (unsigned char *)imm[i];
            if ((b[0] & ~4) == 0 && (b[1] == 8 || b[1] == 9)) { make_writable(imm[i], 1); b[0] |= 8; n++; }
        }
        raven_log("[rio2pad] 4-player cabinet: gun device added to %d/8 device masks\n", n);
    }

    // 4-player cabinet: the game counts RIO2 I/O boards and stops at "Number of RIO2 boards detected ...
    // Press TEST" (0x4adac0). Our pads stand in for all boards, so skip that stop.
    unsigned char *b = (unsigned char *)0x4adbc8ULL;
    if (env_int("RIO2PAD_BOARDSKIP", g_players4)) {
        if (!memcmp(b, "\x0f\x85\xa1\x00\x00\x00", 6)) { make_writable(0x4adbc8ULL, 6); memset(b, 0x90, 6); }
        else raven_log("[rio2pad] unexpected bytes at the RIO2 board check, left alone\n");
    }

    // 4-player cabinet: the game builds the IR-gun input map (io channels 0x319.. per gun, 5 per gun:
    // aim X, aim Y, trigger, reload, grenade -> game inputs 0x206.. and 0x2a3..) only when launched
    // with "-random" (the option parser sets config byte 0x1749fbf, read via 0x438010 at 0x41baf0).
    // Without it the map falls through to a branch that binds no gun channels: the dispatcher table
    // stays empty and the game never reads our aim - no crosshair, no shooting. Force the branch.
    if (g_players4) {
        unsigned char *j = (unsigned char *)0x41bb1cULL;   // je 0x41c060 (6 bytes)
        if (!memcmp(j, "\x0f\x84\x3e\x05\x00\x00", 6)) { make_writable(0x41bb1cULL, 6); memset(j, 0x90, 6); raven_log("[rio2pad] 4-player cabinet: IR-gun input map forced (the game's -random branch)\n"); }
        else raven_log("[rio2pad] unexpected bytes at the input-map branch, left alone\n");
        // The -random branch also configures the analog axes (0x61f3a0/0x61f3c0), which write through
        // the input-array pointer at 0x180ae78. That pointer is normally set by the stick-gun device
        // init (0x61c290, static array at 0x181c320), which never runs on this cabinet type - so the
        // game crashed (NULL+0x628) right after the map build. Point it at the same static array.
        *(unsigned char **)0x180ae78ULL = (unsigned char *)0x181c320ULL;
        raven_log("[rio2pad] 4-player cabinet: analog input array pointer fixed up\n");
        // The 4-gun IR-channel loop (0x41c74f: io switches 0x319.. per gun, 5 per gun -> game gun
        // inputs) only runs when the game is in 4-cashbox mode (0x418a40 reads the playerCost /
        // numCashboxes4P operator setting). With the default 2-cashbox setting the loop is skipped
        // and the map keeps the stick-gun channel bindings (io 0x394.., a device that does not exist
        // here) - aim dead, and START is bound to the 4-player board's wiring. Force the loop.
        unsigned char *cje = (unsigned char *)0x41c6efULL;   // je 0x41cba0 (6 bytes)
        if (!memcmp(cje, "\x0f\x84\xab\x04\x00\x00", 6)) { make_writable(0x41c6efULL, 6); memset(cje, 0x90, 6); raven_log("[rio2pad] 4-player cabinet: 4-gun IR map loop forced (cashbox check bypassed)\n"); }
        else raven_log("[rio2pad] unexpected bytes at the cashbox branch, left alone\n");

        // The type-9 attract demands the cabinet's four QR-scanner webcams. The boot probe
        // (0x49cdb0, V4L2) finds none on a PC, so the per-station webcam objects (0x1766540+g*8)
        // stay NULL and the ready byte 0x174c100 stays 0 forever. The attract screen script then
        // refuses to run the join flow: it shows "QR scanners need to be configured!" (red banner,
        // condition ATTRACT_WEBCAM_CONFIG) and per station "P%u QR Scanner is not detected!"
        // (condition P%u_WEBCAM), and never sets CASHPLAY_START_P%d when START is pressed - so
        // nobody can join. Patch the three condition setters so the script sees every scanner as
        // present and configured:
        //   0x44b7b2 je 0x44b980 -> nops: the webcam-idle loop in 0x44a870 no longer fires
        //                            WEBCAMS_NOT_READY (it just keeps idling, harmless).
        //   0x4491d1 jne 0x448e78 -> jmp: 0x448a20 never fires ATTRACT_WEBCAM_CONFIG.
        //   0x449039 jne 0x448e88 -> jmp: 0x448a20 never fires P%u_WEBCAM (0x49d730(g) is the
        //                            per-station "webcam present and streaming" check).
        {
            unsigned char *p1 = (unsigned char *)0x44b7b2ULL;   // je 0x44b980 (6 bytes)
            if (!memcmp(p1, "\x0f\x84\xc8\x01\x00\x00", 6)) { make_writable(0x44b7b2ULL, 6); memset(p1, 0x90, 6); raven_log("[rio2pad] 4-player cabinet: WEBCAMS_NOT_READY condition disabled (QR gate 1/3)\n"); }
            else raven_log("[rio2pad] unexpected bytes at the WEBCAMS_NOT_READY branch, left alone\n");
            unsigned char *p2 = (unsigned char *)0x4491d1ULL;   // jne 0x448e78 (6 bytes)
            if (!memcmp(p2, "\x0f\x85\xa1\xfc\xff\xff", 6)) {
                const int rel = 0xfffffca2;                     // jmp 0x448e78
                make_writable(0x4491d1ULL, 6);
                p2[0] = 0xe9; memcpy(p2 + 1, &rel, 4); p2[5] = 0x90;
                raven_log("[rio2pad] 4-player cabinet: ATTRACT_WEBCAM_CONFIG condition disabled (QR gate 2/3)\n");
            }
            else raven_log("[rio2pad] unexpected bytes at the ATTRACT_WEBCAM_CONFIG branch, left alone\n");
            unsigned char *p3 = (unsigned char *)0x449039ULL;   // jne 0x448e88 (6 bytes)
            if (!memcmp(p3, "\x0f\x85\x49\xfe\xff\xff", 6)) {
                const int rel = 0xfffffe4a;                     // jmp 0x448e88
                make_writable(0x449039ULL, 6);
                p3[0] = 0xe9; memcpy(p3 + 1, &rel, 4); p3[5] = 0x90;
                raven_log("[rio2pad] 4-player cabinet: per-station webcam-missing condition disabled (QR gate 3/3)\n");
            }
            else raven_log("[rio2pad] unexpected bytes at the P%u_WEBCAM branch, left alone\n");
        }
    }

    unsigned char *j = (unsigned char *)0x448e4fULL;    // hide "Factory Setup" banner
    if (!raven_env("RIO2PAD_FACTORYBANNER")) {
        if (!memcmp(j, "\x0f\x84\x4b\x03\x00\x00", 6)) { make_writable(0x448e4fULL, 6); memset(j, 0x90, 6); }
        else raven_log("[rio2pad] unexpected bytes at factory banner check, left alone\n");
    }

    if (*(unsigned char *)IRT_GUN_CONNECTED == 0xe9 && *(unsigned char *)IRT_GUN_AIM == 0x48) {
        make_writable(IRT_GUN_CONNECTED, 14);
        make_writable(IRT_GUN_AIM, 14);
        write_abs_jmp((unsigned char *)IRT_GUN_CONNECTED, (uint64_t)gun_connected);
        write_abs_jmp((unsigned char *)IRT_GUN_AIM, (uint64_t)gun_aim);
    } else {
        raven_log("[rio2pad] unexpected gun getter code, not hooking guns\n");
    }
}

// ---- input thread ----

static float g_remote_aim[MAX_PADS][2] = {{-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}};   // browser players' aim, -1 = none
typedef DWORD (WINAPI *xinput_get_state_fn)(DWORD, XINPUT_STATE *);
static xinput_get_state_fn p_XInputGetState;

static int our_window_focused(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

HWND raven_game_hwnd(void);
int ravenui_is_open(void);

// Mouse -> player 1 aim/buttons. Runs on the pad thread.
static void poll_mouse(void)
{
    static POINT last; static int have_last;
    g_mouse_btn[0] = g_mouse_btn[1] = g_mouse_btn[2] = 0;
    HWND gw = raven_game_hwnd();
    if (!rio2pad_mouse_mode || !gw || !our_window_focused() || ravenui_is_open()) { have_last = 0; return; }
    RECT rc; POINT p;
    if (!GetClientRect(gw, &rc) || rc.right <= 0 || rc.bottom <= 0 || !GetCursorPos(&p)) return;
    POINT c = p;
    ScreenToClient(gw, &c);
    int inside = c.x >= 0 && c.y >= 0 && c.x < rc.right && c.y < rc.bottom;
    if (rio2pad_mouse_mode == 1) {
        if (have_last && (p.x != last.x || p.y != last.y) && inside) {
            g_mouse_owns = 1;
            // map against the picture as shown (bars excluded), not the whole window
            extern int ravengfx_res_w, ravengfx_res_h;
            extern void raven_fit_rect(int, int, int, int, int *, int *, int *, int *);
            int px = 0, py = 0, pw = rc.right, ph = rc.bottom;
            if (ravengfx_res_w > 0 && ravengfx_res_h > 0)
                raven_fit_rect(ravengfx_res_w, ravengfx_res_h, rc.right, rc.bottom, &px, &py, &pw, &ph);
            float ax = (c.x - px + 0.5f) / pw, ay = (c.y - py + 0.5f) / ph;
            // hybrid 4-player views (ravengfx_win.c raven_view): menu shows the left half only;
            // mission draws the 2D layer (crosshairs) in the middle 32:9 band
            extern int raven_view;
            if (raven_view == 1) ax *= 0.5f;
            else if (raven_view == 4 && ravengfx_res_h > 0) {   // mission: aim spans the whole screen
                float band = (float)ravengfx_res_w * 9.0f / 32.0f / ravengfx_res_h;
                if (band < 1) ay = (ay - (1 - band) / 2) / band;
            }
            g_aim[0][0] = ax < 0 ? 0 : ax > 1 ? 1 : ax;
            g_aim[0][1] = ay < 0 ? 0 : ay > 1 ? 1 : ay;
        }
        last = p; have_last = 1;
    } else {
        POINT mid = {rc.right / 2, rc.bottom / 2};
        ClientToScreen(gw, &mid);
        if (have_last && (p.x != mid.x || p.y != mid.y)) {
            g_mouse_owns = 1;
            for (int k = 0; k < 2; k++) {
                float d = k ? (float)(p.y - mid.y) / rc.bottom : (float)(p.x - mid.x) / rc.right;
                g_aim[0][k] += d * rio2pad_mouse_sens;
                if (g_aim[0][k] < 0) g_aim[0][k] = 0;
                if (g_aim[0][k] > 1) g_aim[0][k] = 1;
            }
        }
        SetCursorPos(mid.x, mid.y);
        have_last = 1;
        inside = 1;
    }
    if (inside) {
        g_mouse_btn[0] = key(VK_LBUTTON);
        g_mouse_btn[1] = key(VK_RBUTTON);
        g_mouse_btn[2] = key(VK_MBUTTON);
    }
    static long long last_dbg;
    if (g_debug && g_mouse_owns && now_ms() - last_dbg >= 1000) {
        last_dbg = now_ms();
        raven_log("[rio2pad] mouse aim %.3f %.3f buttons %d%d%d\n", g_aim[0][0], g_aim[0][1], g_mouse_btn[0], g_mouse_btn[1], g_mouse_btn[2]);
    }
}

static int trig(BYTE v) { return v * 65534 / 255 - 32767; }
static int neg(SHORT v) { return v == -32768 ? 32767 : -v; }

static void poll_pads(void)
{
    static int was_active[MAX_PADS];
    long long t = now_ms();
    // Xbox (XInput) pads keep their own slot; slots without one take the PlayStation pads in order (pspad_win.c).
    extern int pspad_get(int n, XINPUT_STATE *st);
    int next_ps = 0;
    // A browser player's pad may be plugged into this very PC (testing, or a friend using the host's
    // browser): then Windows gives it to the game too, and one pad drives two players. So while a
    // browser holds a player spot and the game window is not in front, the host's own pads are ignored.
    extern int streamweb_players(void);
    static int pads_muted;
    int mute = streamweb_players() > 0 && !our_window_focused();
    if (mute != pads_muted) { raven_log("[rio2pad] host pads %s\n", mute ? "ignored: a browser player is connected and the game window is in the background" : "back on"); pads_muted = mute; }
    for (int p = 0; p < MAX_PADS; p++) {
        struct pad *pd = &g_pads[p];
        XINPUT_STATE st;
        int ok = !mute && p_XInputGetState && p_XInputGetState(p, &st) == ERROR_SUCCESS;
        if (!ok && !mute && pspad_get(next_ps, &st)) { ok = 2; next_ps++; }
        // Browser stream (streamweb_win.c): a friend in a browser owns this player's slot.
        extern int streamweb_pad(int p, XINPUT_STATE *st, float *ax, float *ay);
        float rx, ry;
        g_remote_aim[p][0] = -1;
        if (p > 0 && streamweb_pad(p, &st, &rx, &ry)) { ok = 3; g_remote_aim[p][0] = rx; g_remote_aim[p][1] = ry; }
        if (ok != was_active[p]) {
            raven_log("[rio2pad] player %d %s\n", p + 1, ok == 3 ? "= browser player" : ok == 2 ? "= PlayStation pad connected" : ok ? "= XInput pad connected" : "pad disconnected");
            was_active[p] = ok;
        }
        int b[16] = {0}, a[8] = {0, 0, -32767, 0, 0, -32767, 0, 0};
        if (ok) {
            WORD w = st.Gamepad.wButtons;
            b[PAD_A] = !!(w & XINPUT_GAMEPAD_A);        b[PAD_B] = !!(w & XINPUT_GAMEPAD_B);
            b[PAD_X] = !!(w & XINPUT_GAMEPAD_X);        b[PAD_Y] = !!(w & XINPUT_GAMEPAD_Y);
            b[PAD_LB] = !!(w & XINPUT_GAMEPAD_LEFT_SHOULDER);
            b[PAD_RB] = !!(w & XINPUT_GAMEPAD_RIGHT_SHOULDER);
            b[PAD_BACK] = !!(w & XINPUT_GAMEPAD_BACK);  b[PAD_START] = !!(w & XINPUT_GAMEPAD_START);
            a[0] = st.Gamepad.sThumbLX; a[1] = neg(st.Gamepad.sThumbLY);
            a[3] = st.Gamepad.sThumbRX; a[4] = neg(st.Gamepad.sThumbRY);
            a[2] = trig(st.Gamepad.bLeftTrigger); a[5] = trig(st.Gamepad.bRightTrigger);
        }
        int kb = 0;
        if (our_window_focused()) {   // keyboard fallback, players 1 and 2
            if (p == 0) {
                if (key(VK_RETURN) || key('1')) b[PAD_START] = kb = 1;
                if (key(VK_F2)) b[PAD_BACK] = kb = 1;
                if (key('5')) b[PAD_Y] = kb = 1;
                if (key(VK_SPACE)) { a[5] = 32767; kb = 1; }
                if (key('R')) b[PAD_RB] = kb = 1;
                if (key('G')) { a[2] = 32767; kb = 1; }
                if (key(VK_LEFT))  { a[0] = -32767; kb = 1; }
                if (key(VK_RIGHT)) { a[0] = 32767; kb = 1; }
                if (key(VK_UP))    { a[1] = -32767; kb = 1; }
                if (key(VK_DOWN))  { a[1] = 32767; kb = 1; }
                if (g_mouse_btn[0]) { a[5] = 32767; kb = 1; }
                if (g_mouse_btn[1]) b[PAD_RB] = kb = 1;
                if (g_mouse_btn[2]) { a[2] = 32767; kb = 1; }
            } else if (p == 1) {
                if (key('2')) b[PAD_START] = kb = 1;
                if (key('6')) b[PAD_Y] = kb = 1;
            }
        }
        pd->active = ok ? 1 : (p == 0 || kb) ? 2 : 0;   // 1 = a real pad (Xbox or PlayStation)   // keyboard always drives player 1
        if (!g_freeplay && b[PAD_Y] && !pd->btn[PAD_Y]) pd->coin_t = t;
        for (int i = 0; i < 16; i++) {
            if (g_debug && b[i] != pd->btn[i]) raven_log("[rio2pad] P%d button %d = %d\n", p + 1, i, b[i]);
            pd->btn[i] = b[i];
        }
        for (int i = 0; i < 8; i++) pd->axis[i] = a[i];
    }
}

static void update_aim(void)
{
    static long long last;
    long long t = now_ms();
    float dt = last ? (t - last) / 1000.0f : 0;
    last = t;
    if (g_aimsweep) {   // debug: drive every gun's aim by script (no OS input needed)
        for (int p = 0; p < MAX_PADS; p++) {
            float a = (t / 1000.0f) * 1.1f + p * 1.9f;
            g_aim[p][0] = 0.5f + 0.4f * (float)sin(a);
            g_aim[p][1] = 0.5f + 0.4f * (float)cos(a * 0.83f);
        }
        return;
    }
    for (int p = 0; p < MAX_PADS; p++) {
        float sx = g_pads[p].axis[3] / 32767.0f, sy = g_pads[p].axis[4] / 32767.0f;
        float lx = g_pads[p].axis[0] / 32767.0f, ly = g_pads[p].axis[1] / 32767.0f;
        if (lx * lx + ly * ly > sx * sx + sy * sy) { sx = lx; sy = ly; }
        if (g_remote_aim[p][0] >= 0) {   // browser player: aims where their mouse is (or their pad moved it)
            // the browser gets the picture as shown, so map it like the local mouse (hybrid 4P views)
            float ax = g_remote_aim[p][0], ay = g_remote_aim[p][1];
            extern int raven_view, ravengfx_res_w, ravengfx_res_h;
            if (raven_view == 1) ax *= 0.5f;
            else if (raven_view == 4 && ravengfx_res_h > 0) {
                float band = (float)ravengfx_res_w * 9.0f / 32.0f / ravengfx_res_h;
                if (band < 1) ay = (ay - (1 - band) / 2) / band;
            }
            g_aim[p][0] = ax < 0 ? 0 : ax > 1 ? 1 : ax;
            g_aim[p][1] = ay < 0 ? 0 : ay > 1 ? 1 : ay;
            continue;
        }
        if (p == 0 && g_mouse_owns) {   // mouse has P1's aim until a stick is pushed
            if (sx * sx + sy * sy < 0.3f * 0.3f) continue;
            g_mouse_owns = 0;
        }
        if (rio2pad_aim_mode) {
            g_aim[p][0] = 0.5f + 0.5f * sx;
            g_aim[p][1] = 0.5f + 0.5f * sy;
            continue;
        }
        float dz = 0.15f;
        if (sx > -dz && sx < dz) sx = 0;
        if (sy > -dz && sy < dz) sy = 0;
        g_aim[p][0] += sx * (sx < 0 ? -sx : sx) * rio2pad_stick_speed * dt;
        g_aim[p][1] += sy * (sy < 0 ? -sy : sy) * rio2pad_stick_speed * dt;
        for (int k = 0; k < 2; k++) {
            if (g_aim[p][k] < 0) g_aim[p][k] = 0;
            if (g_aim[p][k] > 1) g_aim[p][k] = 1;
        }
    }
}

static void dump_swlog(void)
{
    FILE *f = fopen(g_swlog, "w");
    if (!f) return;
    for (int i = 0; i < MAX_SW; i++)
        if (g_swcount[i]) fprintf(f, "%d %u\n", i, g_swcount[i]);
    fprintf(f, "start %d %d %d %d test %d %d %d\n", G_START_IDS[0], G_START_IDS[1], G_START_IDS[2],
            G_START_IDS[3], G_TEST_IDS[0], G_TEST_IDS[1], G_TEST_IDS[2]);
    // The game's switch dispatcher table (0x4ad370): pairs (physical switch id, logical id).
    int n = *(volatile int *)0x1771850ULL;
    const int *t = (const int *)0x176f850ULL;
    fprintf(f, "table %d:", n);
    for (int i = 0; i < n && i < 400; i++) fprintf(f, " %d>%d", t[i * 2], t[i * 2 + 1]);
    fprintf(f, "\n");
    fclose(f);
}

static void log_procs(void)
{
    static char prev[2048];
    char cur[2048] = "";
    for (int i = 0; i < 64; i++) {
        const char *slot = (const char *)(0x1dd78c0ULL + i * 0x448ULL);
        if ((slot[0x28] & 1) && slot[0] && strlen(cur) + 42 < sizeof cur) {
            strcat(cur, " ");
            strncat(cur, slot, 0x28);
        }
    }
    if (strcmp(cur, prev)) { raven_log("[rio2pad] t=%lldms procs:%s\n", now_ms() - g_t_start, cur); strcpy(prev, cur); }
}

// ---- game trace hook (RIO2PAD_TRACE=1): log the game's own init trace lines (fn 0x4fc460) so a
// boot that silently diverts (e.g. the input-map build at 0x437de1 being skipped) shows where it
// stopped. ----
static void (*trace_orig)(const char *);
static SV void trace_hook(const char *name)
{
    if (name) raven_log("[trace] %s\n", name);
    trace_orig(name);
}

static void install_trace_hook(void)
{
    unsigned char *f = (unsigned char *)0x4fc460ULL;
    static const unsigned char expect[16] = {0x55,0x53,0x31,0xf6,0x48,0x89,0xfb,0x31,0xff,
                                             0x48,0x81,0xec,0x58,0x01,0x00,0x00};
    if (memcmp(f, expect, 16)) { raven_log("[rio2pad] unexpected trace fn code, not hooking\n"); return; }
    unsigned char *t = exec_page();
    memcpy(t, f, 16);
    write_abs_jmp(t + 16, 0x4fc470ULL);
    trace_orig = (void (*)(const char *))t;
    make_writable(0x4fc460ULL, 14);
    write_abs_jmp(f, (uint64_t)trace_hook);
    raven_log("[rio2pad] trace hook installed\n");
}


// ---- aim-layer debug dump (RIO2PAD_IRTDUMP=file) ----
// The 4-player cabinet (type 9) shows no gun cursors. This dumps every layer the cursor could
// come from, several times a second, so a run shows which layer carries the aim and which stays
// dead: our aim, the irt per-gun arrays (0x1aa3690 X / 0x1aa37a0 Y) with their state byte and
// counter, the irt pairs (0x1f392a0 device, 0x1e94680 smoothed), the gun-device state banks
// (pointer 0x181f7e0, per-gun 72-byte banks at +g*72 and +288+g*72, 0x10000 = aim-valid bit),
// the analog channels the device registers (X ids 0x319/0x31e/0x323/0x328, Y ids
// 0x31a/0x31f/0x324/0x329, lookup 0x61ee70; +0x14/+0x18 = min/max), the 0x800 gun device state
// (0x180adf0) and the irt board connection table (0x1a7cb20, 32 slots, +0x3c gun, +0xb0 seq).
static FILE *g_irtdump;
typedef SV void *(*chan_fn)(int);
static void irt_dump(void)
{
    if (!g_irtdump) return;
    static chan_fn chan_get;
    if (!chan_get) chan_get = (chan_fn)0x61ee70ULL;
    const unsigned char *st = *(const unsigned char *const *)0x181f7e0ULL;
    fprintf(g_irtdump, "t=%lld cab=%d guns=%d st=%p\n", now_ms() - g_t_start,
            *G_CABINET_TYPE, *(volatile int *)0x1f392d0ULL, (const void *)st);
    fprintf(g_irtdump, " cfg: random(1749fbf)=%d stickgun(1749fa9)=%d type49(1749faa)=%d\n",
            *(const unsigned char *)0x1749fbfULL, *(const unsigned char *)0x1749fa9ULL,
            *(const unsigned char *)0x1749faaULL);
    fprintf(g_irtdump, " map: n=%d\n", *(volatile int *)0x1771850ULL);
    {   // stage probes for the 4-gun IR pairs + join starts (type-9 debug, 2026-11-12)
        static chan_fn desc_get, prov_get, st_get;
        if (!desc_get) {
            desc_get = (chan_fn)0x61e890ULL;   // channel descriptor (counters gate object creation)
            prov_get = (chan_fn)0x4ac220ULL;  // logical-input provision lookup (NULL = dispatcher skips the pair copy)
            st_get   = (chan_fn)0x4ad030ULL;  // logical-input state accumulator
        }
        fprintf(g_irtdump, " pairs:");
        for (int p = 0; p < 6; p++) {
            int phys, logi;
            if (p < 4) { phys = 0x319 + p * 5; logi = 0x206 + p * 0xc; }
            else { phys = (p == 4 ? 577 : 580); logi = (p == 4 ? 718 : 718); }
            fprintf(g_irtdump, " %x/%x c=%p d=%p v=%p s=%p o=%p",
                    phys, logi, (const void *)chan_get(phys), (const void *)desc_get(phys),
                    (const void *)prov_get(logi), (const void *)st_get(logi),
                    (const void *)*(volatile const unsigned char *const *)(0x177d000ULL + 8ULL * (unsigned)logi));
        }
        fprintf(g_irtdump, "\n");
    }
    for (int k = 0; k < 17; k++) {   // game gun inputs 0x206..0x20a (4 guns) and 0x2a3.. menu aim
        int id = (k < 11 ? 0x206 + k : 0x2a0 + k - 11);
        const unsigned char *o = *(const unsigned char *const *)(0x177d000ULL + 8 * (unsigned)id);
        fprintf(g_irtdump, " in %x %p:", id, (const void *)o);
        if (o) for (int i = 0; i < 4; i++) fprintf(g_irtdump, " %.2f", *(const float *)(o + 4 * i));
        fprintf(g_irtdump, "\n");
    }
    for (int g = 0; g < 4; g++) {
        fprintf(g_irtdump, " g%d pad=%d aim=%.3f,%.3f st=%d cnt=%d dev=%.2f,%.2f sm=%.2f,%.2f arr=%.3f,%.3f",
                g, g_pads[g].active, g_aim[g][0], g_aim[g][1],
                *(const unsigned char *)(0x1aa37c0ULL + g), *(const int *)(0x1aa37b0ULL + 4 * g),
                *(const float *)(0x1f392a0ULL + 8 * g), *(const float *)(0x1f392a0ULL + 8 * g + 4),
                *(const float *)(0x1e94680ULL + 8 * g), *(const float *)(0x1e94680ULL + 8 * g + 4),
                *(const float *)(0x1aa3690ULL + 4 * g), *(const float *)(0x1aa37a0ULL + 4 * g));
        if (st) {
            fprintf(g_irtdump, " b1=%06x/%.2f,%.2f b2=%06x/%.2f,%.2f t1=%08x t2=%08x",
                    *(const unsigned *)(st + 72 * g),
                    *(const float *)(st + 72 * g + 4), *(const float *)(st + 72 * g + 8),
                    *(const unsigned *)(st + 288 + 72 * g),
                    *(const float *)(st + 288 + 72 * g + 4), *(const float *)(st + 288 + 72 * g + 8),
                    *(const unsigned *)(st + 0x240 + 48 * g), *(const unsigned *)(st + 0x300 + 48 * g));
        }
        fprintf(g_irtdump, "\n");
        int xid = *(const int *)(0x11bf3a0ULL + 4 * g), yid = *(const int *)(0x11bf3b0ULL + 4 * g);
        const unsigned char *cx = chan_get(xid), *cy = chan_get(yid);
        fprintf(g_irtdump, "   chX %x %p:", xid, (const void *)cx);
        if (cx) for (int i = 0; i < 10; i++) fprintf(g_irtdump, " %08x", *(const unsigned *)(cx + 4 * i));
        fprintf(g_irtdump, "\n   chY %x %p:", yid, (const void *)cy);
        if (cy) for (int i = 0; i < 10; i++) fprintf(g_irtdump, " %08x", *(const unsigned *)(cy + 4 * i));
        fprintf(g_irtdump, "\n");
    }
    fprintf(g_irtdump, " dev800:");
    for (int i = 0; i < 12; i++) fprintf(g_irtdump, " %08x", *(const unsigned *)(0x180adf0ULL + 4 * i));
    fprintf(g_irtdump, "\n board:");
    int nb = 0;
    for (int i = 0; i < 32; i++) {
        const unsigned char *e = (const unsigned char *)(0x1a7cb20ULL + (size_t)i * 0x1348);
        if (*(const unsigned *)e) {
            fprintf(g_irtdump, " [%d]gun=%d seq=%d", i, *(const int *)(e + 0x3c), *(const int *)(e + 0xb0));
            nb++;
        }
    }
    if (!nb) fprintf(g_irtdump, " (none)");
    fprintf(g_irtdump, "\n");
    // io context and the per-frame input snapshot the main loop builds (0x4ad370):
    // memset(0x1766f48, 0x8908) then copy ctx+0x7248..0x7318 and ctx+0x7350..0x7370 into 0x1766f48..
    static unsigned char *ctx;
    typedef SV unsigned char *(*ctx_fn)(void);
    if (!ctx) { static ctx_fn f; if (!f) f = (ctx_fn)0x61cd50ULL; ctx = f(); }
    if (ctx) {
        fprintf(g_irtdump, " ctx=%p c7248:", (void *)ctx);
        for (unsigned o = 0x7248; o < 0x7380; o += 8) {
            unsigned long long q = *(const unsigned long long *)(ctx + o);
            if (q) fprintf(g_irtdump, " +%x=%llx", o, q);
        }
        fprintf(g_irtdump, "\n snap:");
        for (unsigned a = 0x1766f48; a < 0x1767048; a += 8) {
            unsigned long long q = *(const unsigned long long *)(uintptr_t)a;
            if (q) fprintf(g_irtdump, " %x=%llx", a, q);
        }
        fprintf(g_irtdump, "\n");
    }
    fflush(g_irtdump);
}


// ---- 4-player input-map completion ----
// The game's input map (dispatcher table 0x176f840, count 0x1771850, built at boot by 0x41baf0)
// misses the IR-gun channel bindings on this cabinet: the 4-gun loop (0x41c74f, io 0x319.. per gun
// -> game gun inputs 0x206.. and menu aim 0x2a3..) sits behind a cashbox-mode check (0x418a40
// returns 2 with the default operator settings) that our environment never satisfies. Append the
// same 32 mappings ourselves (5 game + 3 menu aim per gun, same order as the loop) once the game's
// own build has finished, so the dispatcher picks them up on the next frame.
static void coop_map_append(void)
{
    typedef SV int (*map_fn)(int, int);
    typedef SV void (*flag_fn)(int, int);
    static int done;
    if (done || *(volatile int *)0x1771850ULL < 40) return;
    done = 1;
    // The 4-player board wires its START buttons to io switches 580/582/612/614 (the game's map
    // binds them to the join inputs). Those channels are never registered in our environment (the
    // polled start switches are 577/578/609/610), so the dispatcher skips the pairs. Register them
    // like the game registers every other switch channel (0x61d240: id, min, max) - here, after the
    // io init has run.
    { typedef SV void (*chan_fn)(int, float, float); chan_fn f = (chan_fn)0x61d240ULL;
      f(580, 0.0f, 1.0f); f(582, 0.0f, 1.0f); f(612, 0.0f, 1.0f); f(614, 0.0f, 1.0f); }
    raven_log("[rio2pad] 4-player cabinet: START channels 580/582/612/614 registered\n");
    for (int g = 0; g < 4; g++) {
        for (int k = 0; k < 5; k++)
            ((map_fn)0x4acdb0ULL)(0x319 + g * 5 + k, 0x206 + g * 12 + k);
        for (int k = 0; k < 3; k++)
            ((map_fn)0x4acdb0ULL)(0x319 + g * 5 + k, 0x2a3 + g * 3 + k);
        for (int k = 0; k < 3; k++)
            ((flag_fn)0x61dee0ULL)(0x31b + g * 5 + k, 1);
    }
    // The game's own type-9 map binds the join inputs to the 4-player board's START wiring
    // (io 580/582/612/614), which no hardware here provides. Bind the start switches that DO
    // exist in this environment (the game's own polled list 577/578/609/610) to the same join
    // inputs (718..721, as the type-3/type-8 maps do), so START joins players again.
    ((map_fn)0x4acdb0ULL)(577, 718);
    ((map_fn)0x4acdb0ULL)(578, 719);
    ((map_fn)0x4acdb0ULL)(609, 720);
    ((map_fn)0x4acdb0ULL)(610, 721);
    // The join state machine (0x4269e0, runs per attract frame per station) polls the per-player
    // START game inputs 0x24e+g (590..593) with 0x4ae2f0 after the screen script sets the
    // CASHPLAY_START_P%d condition. The game's own type-9 map branch binds 577->0x24e, 578->0x24f,
    // 609->0x250, 610->0x251 (0x41c322..0x41c45d), but that branch never runs here (we force the
    // -random branch for the IR-gun map). Append the same four bindings so START reaches it.
    ((map_fn)0x4acdb0ULL)(577, 0x24e);
    ((map_fn)0x4acdb0ULL)(578, 0x24f);
    ((map_fn)0x4acdb0ULL)(609, 0x250);
    ((map_fn)0x4acdb0ULL)(610, 0x251);
    raven_log("[rio2pad] 4-player cabinet: start switches 577/578/609/610 bound to the join inputs\n");
    raven_log("[rio2pad] 4-player cabinet: 32 IR-gun mappings appended (game map had %d)\n",
              *(volatile int *)0x1771850ULL - 32);
}

static DWORD WINAPI pad_thread(void *arg)
{
    (void)arg;
    timeBeginPeriod(1);
    long long last_log = now_ms(), last_proc = 0, last_irt = 0;
    for (;;) {
        long long t = now_ms();
        if (g_swlog && t - last_log >= 5000) { dump_swlog(); last_log = t; }
        if (g_proclog && t - last_proc >= 500) { log_procs(); last_proc = t; }
        if (g_irtdump && t - last_irt >= 250) { irt_dump(); last_irt = t; }
        if (g_players4) coop_map_append();
        poll_mouse();
        poll_pads();
        update_aim();
        Sleep(4);
    }
    return 0;
}

// Called from main.c after relocation and canary patching, before the module inits run.
void rio2pad_install(void)
{
    g_t_start = now_ms();
    g_debug = raven_env("RIO2PAD_DEBUG") != NULL;
    g_autostart = raven_env("RIO2PAD_AUTOSTART") != NULL;
    g_continueshoot = raven_env("RIO2PAD_CONTINUESHOOT") != NULL;
    g_gunlevel = raven_env("RIO2PAD_GUNLEVEL") != NULL;   // old gun-button reporting (press every frame)
    g_proclog = raven_env("RIO2PAD_PROCLOG") != NULL;
    g_aimsweep = raven_env("RIO2PAD_AIMSWEEP") != NULL;
    g_freeplay = env_int("RIO2PAD_FREEPLAY", 1) != 0;
    g_skipboot = env_int("RIO2PAD_SKIPBOOT", 1) != 0;
    g_gamevol = env_int("RIO2PAD_GAMEVOL", -1);
    g_probe = env_int("RIO2PAD_PROBE", -1);
    static char swlog[256];
    if (raven_env("RIO2PAD_SWLOG")) { snprintf(swlog, sizeof swlog, "%s", raven_env("RIO2PAD_SWLOG")); g_swlog = swlog; }
    if (raven_env("RIO2PAD_INPUTLOG")) g_inputlog = fopen(raven_env("RIO2PAD_INPUTLOG"), "a");
    if (raven_env("RIO2PAD_IRTDUMP")) g_irtdump = fopen(raven_env("RIO2PAD_IRTDUMP"), "w");
    if (raven_env("RIO2PAD_TRACE")) install_trace_hook();
    if (raven_env("RIO2PAD_COIN"))
        sscanf(raven_env("RIO2PAD_COIN"), "%d,%d,%d,%d", &g_coin_ids[0], &g_coin_ids[1], &g_coin_ids[2], &g_coin_ids[3]);
    const char *ss = raven_env("RIO2PAD_STICKSPEED");
    if (ss) rio2pad_stick_speed = strtof(ss, NULL);
    const char *am = raven_env("RIO2PAD_AIM");
    rio2pad_aim_mode = am && !strcmp(am, "abs");
    const char *mm = raven_env("RIO2PAD_MOUSE");
    if (mm) rio2pad_mouse_mode = !strcmp(mm, "relative") ? 2 : (!strcmp(mm, "0") || !strcmp(mm, "off")) ? 0 : 1;
    const char *ms = raven_env("RIO2PAD_MOUSESENS");
    if (ms) rio2pad_mouse_sens = strtof(ms, NULL);
    raven_log("[rio2pad] mouse %s, sensitivity %.2f\n", rio2pad_mouse_mode == 2 ? "relative" : rio2pad_mouse_mode ? "screen" : "off", rio2pad_mouse_sens);
    raven_log("[rio2pad] SKIPBOOT=%d FREEPLAY=%d DEBUG=%d\n", g_skipboot, g_freeplay, g_debug);

    // RIO2PAD_PLAYERS=4: the 4-player cabinet (type 9: programs.zip + conf, 4 guns). Default 2 (type 8).
    g_players4 = env_int("RIO2PAD_PLAYERS", 2) >= 3 || (raven_env("RAVEN_COOP") && !strcmp(raven_env("RAVEN_COOP"), "host") && env_int("RAVEN_COOP_PLAYERS", 2) >= 3);   // online with 3-4 PCs needs it too
    *G_CABINET_TYPE = env_int("RIO2PAD_CABTYPE", g_players4 ? 9 : 8);
    if (g_players4) raven_log("[rio2pad] 4-player cabinet (type %d)\n", *G_CABINET_TYPE);
    coop_open();
    install_sw_hook();
    install_adj_hook();
    patch_misc();
    if (g_skipboot) patch_skip_boot_checks();
    for (int p = 0; p < MAX_PADS; p++) g_aim[p][0] = g_aim[p][1] = 0.5f;

    HMODULE x = LoadLibraryA("xinput1_4.dll");
    if (!x) x = LoadLibraryA("xinput9_1_0.dll");
    if (x) p_XInputGetState = (xinput_get_state_fn)(void *)GetProcAddress(x, "XInputGetState");
    raven_log("[rio2pad] XInput %s\n", p_XInputGetState ? "loaded" : "NOT available (keyboard only)");
    extern void pspad_start(void);
    pspad_start();
    extern void streamweb_start(void);
    streamweb_start();
    CloseHandle(CreateThread(NULL, 0, pad_thread, NULL, 0, NULL));
}
