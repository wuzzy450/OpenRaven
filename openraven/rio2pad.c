// rio2pad: feed normal gamepads into Halo: Fireteam Raven in place of the RIO2 cabinet I/O board.
//
// The game reads every cabinet switch through one lookup function, sw_get(id) @0x61e890, which
// returns a pointer to a switch record whose int at +4 is "pressed". Player START switch ids live
// in g_start_ids[4] @0x16acc60 and TEST/DIAG ids in g_test_ids[3] @0x16acc70. We inline-hook
// sw_get and, for those ids, return a record marked pressed while the mapped pad button is held.
//
// Up to 4 pads: the Nth non-mouse /dev/input/js* device found is player N+1.
//   Start or A = that player's START     Back = TEST (any pad)     Y = insert coin (only with RIO2PAD_FREEPLAY=0)
//
// Build: gcc -O2 -shared -fPIC -o rio2pad.so rio2pad.c -lpthread -ldl
// Env:   RIO2PAD_DEBUG=1      print button presses and pressed switch ids
//        RIO2PAD_SWLOG=file   every 5 s, write each switch id the game polled (with counts)
//        RIO2PAD_PROBE=id     hold switch <id> pressed 1 s out of every 4 s (to identify ids)
//        RIO2PAD_COIN=a,b,..  switch ids for player 1..4 coin (default: see g_coin_ids)
//        RIO2PAD_AUTOSTART=1  pulse P1 START every other second (testing)
//        RIO2PAD_CABTYPE=n    cabinet type to report (default 8 = 2-player IR guns; 3 = 4-player, needs 2 RIO2 boards)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/joystick.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define SW_GET        0x61e890UL
#define SW_GET_JS_TGT 0x61ee68UL   // target of the "js" at the function entry
#define SW_GET_RESUME 0x61e89eUL   // first instruction after the 14 bytes we overwrite
#define G_START_IDS   ((volatile int *)0x16acc60UL)
#define G_TEST_IDS    ((volatile int *)0x16acc70UL)
#define SHUTDOWN_JOIN_CALL 0x46a34aUL  // "call thread_join_ms(&g_worker, 10000)" in the shutdown path
#define THREAD_JOIN_MS     0x91cd30UL
#define ENVELOPE_DLCLOSE   0x1f5cd80UL   // protection wrapper's own dlclose PLT stub

#define G_CABINET_TYPE ((volatile int *)0x1800e60UL)  // cached SysInfoGameCabinetType (1..13)

#define IRT_SET_CABTYPE 0xdd4bc0UL  // irt_systemSetCabinetType(t): t 7..11 sets gun count; we get t=0

#define IRT_GUN_CONNECTED 0xdd4b10UL  // int  irt_gun_connected(gun)            (16-byte stub)
#define IRT_GUN_AIM       0xdd6c60UL  // void irt_gun_aim(gun, float *x, float *y)  (80 bytes)

#define MAX_PADS 4
#define MAX_SW   0x800

// Xbox-layout button numbers (8BitDo, Xbox and most pads under xpad/hid-generic).
enum { PAD_A = 0, PAD_B = 1, PAD_X = 2, PAD_Y = 3, PAD_LB = 4, PAD_RB = 5, PAD_BACK = 6, PAD_START = 7 };

struct pad {
    int fd;
    char path[64];
    volatile int btn[32];
    volatile int axis[8];
    volatile long long coin_t;   // ms timestamp of the last Y press (coin pulse start)
};
static struct pad g_pads[MAX_PADS] = {{-1}, {-1}, {-1}, {-1}};

static int g_debug, g_autostart, g_probe = -1, g_dumprec = -1;
static int g_probe_list[64], g_probe_n;   // RIO2PAD_PROBELIST="delay,id,id,...": press each id in turn
static time_t g_probe_t0;
static int g_probe_step = 6;              // RIO2PAD_PROBESTEP=s
static int g_coin_ids[MAX_PADS] = {570, 571, -1, -1};   // coin mech switch ids (probed: 570/571 add money)
static volatile unsigned g_swcount[MAX_SW];
static signed char g_swstate[MAX_SW];
static FILE *g_inputlog;   // RIO2PAD_INPUTLOG=file: timestamped switch press/release edges
static const char *g_swlog;
static unsigned char g_fake[8][64];   // rotating fake switch records
static int g_fake_i;

typedef void *(*sw_get_fn)(int);
static sw_get_fn sw_get_orig;          // trampoline

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

// Gun buttons as the cabinet I/O board reported them to the game's switch dispatcher (0x4ad80c):
// record +4 = new presses, +8 = new releases, +0xc = held. Reporting the level in +4 instead made the
// game see a fresh press every frame and never "held", so full-auto weapons (needler, plasma rifle)
// never got their automatic fire. One physical button can feed two logical ids, so the dispatcher may
// ask twice in one poll: edges are worked out once per poll (calls < 3 ms apart share them).
// RIO2PAD_GUNLEVEL=1 = old way.
#define SW_DISPATCH_RET 0x4ad811UL
static void gun_report(int id, int level, unsigned char *f)
{
    static int last[10], snap_p[10], snap_r[10], said;
    static double snap_t[10];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
    int k = id - 793;
    if (!said) { said = 1; fprintf(stderr, "[rio2pad] gun buttons: edge + held reporting (RIO2PAD_GUNLEVEL=1 = old way)\n"); }
    if (t - snap_t[k] >= 3.0) {
        snap_p[k] = level && !last[k];
        snap_r[k] = !level && last[k];
        last[k] = level;
        snap_t[k] = t;
    }
    ((int *)f)[1] = snap_p[k];
    ((int *)f)[2] = snap_r[k];
    ((int *)f)[3] = last[k];
}

// A coin mech gives one short pulse per coin; the game counts every poll that sees it closed.
static int coin_pulse(int p) { return g_pads[p].fd != -1 && now_ms() - g_pads[p].coin_t < 60; }

static float g_aim[MAX_PADS][2];   // per-player aim point, 0..1 across the screen (0,0 = top-left)
int rio2pad_aim_mode;               // exported: the overlay (ravenui.c) changes it live
float rio2pad_stick_speed = 0.25f;  // RIO2PAD_STICKSPEED; also changed live by the overlay
#define g_aim_mode rio2pad_aim_mode
// Mouse drives player 1: 0 off, 1 screen (pointer position = aim), 2 relative (pointer kept at the
// window centre, movement pushes the aim). RIO2PAD_MOUSE / RIO2PAD_MOUSESENS; overlay changes them live.
int rio2pad_mouse_mode = 1;
float rio2pad_mouse_sens = 1.0f;
static int g_mouse_owns;           // mouse moved last: it has player 1's aim
static volatile int g_mouse_btn[3]; // left fire, middle grenade, right reload

// Always report the guns as present (aim stays centred until that player's pad moves it), so the
// game never shows "gun missing" while a wireless pad is asleep.
static int gun_connected(int g) { return g >= 0 && g < MAX_PADS; }

static void gun_aim(int g, float *x, float *y)
{
    if (!x || !y) return;
    if (!gun_connected(g)) { *x = *y = -1.0f; return; }   // -1,-1 = "not aiming at the screen"
    *x = g_aim[g][0];
    *y = 1.0f - g_aim[g][1];   // game's y runs bottom-to-top; ours (and the stick's) top-to-bottom
}

static int btn(int p, int b) { return g_pads[p].fd != -1 && g_pads[p].btn[b]; }

static int g_skipboot;
static time_t g_t_start;
// Same test as the game's proc_running(name) @0x5aa050, done here without calling game code (calling
// it this early hung boot): 64 proc slots at 0x1dd78c0, 0x448 bytes each, name at +0, bit 0 of +0x28 = live.
static int PROC_RUNNING(const char *name)
{
    for (int i = 0; i < 64; i++) {
        const char *slot = (const char *)(0x1dd78c0UL + i * 0x448UL);
        if ((slot[0x28] & 1) && !strncmp(slot, name, 0x28)) return 1;
    }
    return 0;
}

// RIO2PAD_SKIPBOOT: boot screens (file test, "Press START or DIAG to skip") run before the attract
// proc exists. Pulse the file-test START id until attract (AtProc) is first seen, then never again.
static int boot_start_pulse(void)
{
    static int done;
    if (!g_skipboot || done) return 0;
    if (PROC_RUNNING("AtProc")) { done = 1; fprintf(stderr, "[rio2pad] skipboot: attract reached\n"); return 0; }
    return (now_ms() / 250) & 1;
}

static int start_pressed(int p)
{
    if (p == 0 && g_autostart && (time(NULL) & 1) && !(g_probe_n && time(NULL) >= g_probe_t0)) return 1;
    return btn(p, PAD_START);
}

// RT: xpad reports it as axis 5 (-32767 released .. 32767 full); some modes give button 9/11 instead.
// Player records: 0x1c0c7c0 + slot * 0x250 (see 0x418bc0); byte 0 is the state, 2 = "press START to
// continue" countdown (set @0x4271ec). Gun 1 is slot 0, gun 2 is slot 3.
#define PLAYER_STATE(slot) (*(volatile unsigned char *)(0x1c0c7c0UL + (slot) * 0x250UL))
static int in_continue(int p)
{
    int s = PLAYER_STATE(p == 0 ? 0 : 3);
    static int prev[MAX_PADS] = {-1, -1, -1, -1};
    if (p < MAX_PADS && s != prev[p]) { fprintf(stderr, "[rio2pad] P%d state %d\n", p + 1, s); prev[p] = s; }
    return s == 2;
}

// Shooting during the continue countdown speeds it up, so a held trigger at the moment of death
// skipped straight to game over. Ignore the trigger until the trigger is let go after the countdown.
static int trigger_pressed(int p)
{
    static int block[MAX_PADS];
    int down = (g_pads[p].fd != -1 && g_pads[p].axis[5] > 0) || (p == 0 && g_mouse_btn[0]);
    if (getenv("RIO2PAD_CONTINUESHOOT")) return down;
    if (in_continue(p)) { block[p] = 1; return 0; }
    if (block[p] && down) return 0;   // still held from the countdown
    block[p] = 0;
    return down;
}

static int test_pressed(void)
{
    for (int p = 0; p < MAX_PADS; p++)
        if (btn(p, PAD_BACK)) return 1;
    return 0;
}

static void *sw_get_hook(int id)
{
    unsigned long caller = (unsigned long)__builtin_return_address(0);
    void *rec = sw_get_orig(id);
    int pressed = -1;

    if (id >= 0 && id < MAX_SW) g_swcount[id]++;
    if (g_dumprec >= 0 && id == g_dumprec && rec) {     // RIO2PAD_DUMPREC=id: show the raw record
        static time_t last;
        if (time(NULL) != last) {
            last = time(NULL);
            fprintf(stderr, "[rio2pad] rec %d @%p:", id, rec);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", ((unsigned *)rec)[i]);
            fprintf(stderr, "\n");
        }
    }
    for (int i = 0; i < 4; i++)
        if (G_START_IDS[i] && id == G_START_IDS[i]) pressed = start_pressed(i) || (i == 0 && boot_start_pulse());
    // Gun cabinet (type 8) candidates, all driven from pad 1 while we narrow them down:
    // Button-test build: one candidate switch per pad-1 button.
    // Gun buttons, 5 per gun (gun 1 = 793..797, gun 2 = 798..802). Decoded from the type-8 remap
    // table built @0x41c8a0 and the per-player handlers registered @0x42cde0..0x42ce94:
    //   +2 -> logical 0x208+12p: trigger (fire, menu select, calibration)
    //   +3 -> logical 0x209+12p: reload button (handler 0x42c030, "gun%i_reload"/"reload_held")
    //   +4 -> logical 0x20a+12p: grenade (handler 0x42ef10, "gun%dGrenadeKick")
    //   +0/+1 have no physical record in sw_get and no gameplay handler; keep them released.
    if (id >= 793 && id <= 802) {
        int p = (id - 793) / 5, k = (id - 793) % 5;
        pressed = 0;
        if (k == 2) pressed = trigger_pressed(p);                                       // RT
        if (k == 3) pressed = btn(p, PAD_RB) || (p == 0 && g_mouse_btn[2]);            // RB / right click = reload
        if (k == 4) pressed = (g_pads[p].fd != -1 && g_pads[p].axis[2] > 0) || (p == 0 && g_mouse_btn[1]);   // LT / middle = grenade
    }
    // Player START in the gun cabinet: the type-8 remap sends 581 -> logical 0x24e and 582 -> 0x251,
    // and the game reads "Player N Start" as logical 0x24e + slot (gun 2 is slot 3). Same switch
    // also skips cutscenes and confirms calibration.
    if (id == 581) pressed = start_pressed(0);
    if (id == 582) pressed = start_pressed(1);
    for (int i = 0; i < 3; i++)
        if (G_TEST_IDS[i] && id == G_TEST_IDS[i]) pressed = test_pressed();
    for (int i = 0; i < MAX_PADS; i++)
        if (g_coin_ids[i] >= 0 && id == g_coin_ids[i]) pressed = coin_pulse(i);
    if (id == g_probe) pressed = (time(NULL) % 4) == 0;
    if (g_probe_n && time(NULL) >= g_probe_t0) {           // cycle: each listed id gets g_probe_step s, pressed in 1st s
        long k = (time(NULL) - g_probe_t0) / g_probe_step;
        if (k < g_probe_n && id == g_probe_list[k]) pressed = ((time(NULL) - g_probe_t0) % g_probe_step) == 0;
    }
    if (pressed < 0)
        return rec;
    if (id >= 0 && id < MAX_SW && pressed != g_swstate[id]) {   // log every press/release edge
        g_swstate[id] = pressed;
        if (g_inputlog) {
            time_t t = time(NULL);
            char ts[16];
            strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
            fprintf(g_inputlog, "%s switch %d %s\n", ts, id, pressed ? "DOWN" : "up");
            fflush(g_inputlog);
        }
    }

    unsigned char *f = g_fake[g_fake_i++ & 7];
    if (rec) memcpy(f, rec, sizeof g_fake[0]);
    else     memset(f, 0, sizeof g_fake[0]);
    if (id >= 793 && id <= 802 && caller == SW_DISPATCH_RET && !getenv("RIO2PAD_GUNLEVEL")) gun_report(id, pressed, f);
    else ((int *)f)[1] = pressed;
    if (g_debug && pressed) fprintf(stderr, "[rio2pad] switch %d -> pressed\n", id);
    return f;
}

static void write_abs_jmp(unsigned char *p, uint64_t target)
{
    p[0] = 0xff; p[1] = 0x25; memset(p + 2, 0, 4);   // jmp *0(%rip)
    memcpy(p + 6, &target, 8);
}

// Called from the pad thread every ~4 ms: integrate the left stick into the aim point.
static void update_aim(void)
{
    static long long last, last_sh;
    long long t = now_ms();
    float dt = last ? (t - last) / 1000.0f : 0;
    last = t;
    const char *shpath = getenv("RIO2PAD_STATE_FILE");
    if (shpath && *shpath && t - last_sh >= 8) {
        last_sh = t;
        FILE *sf = fopen(shpath, "rb");
        if (sf) {
            struct { uint32_t magic; int32_t active[4]; int32_t btn[4][32]; int32_t axis[4][8]; } st;
            if (fread(&st, sizeof st, 1, sf) == 1 && st.magic == 0x52494F32) {
                for (int p = 0; p < MAX_PADS; p++) if (st.active[p] && g_pads[p].fd == -1) {
                    g_pads[p].fd = -2;   // virtual pad from shared state file
                    snprintf(g_pads[p].path, sizeof g_pads[p].path, "winpad%d", p);
                    fprintf(stderr, "[rio2pad] player %d = Windows XInput Gamepad %d\n", p + 1, p + 1);
                }
                for (int p = 0; p < MAX_PADS; p++) if (g_pads[p].fd == -2) {
                    if (!st.active[p]) { g_pads[p].fd = -1; continue; }
                    if (getenv("RIO2PAD_FREEPLAY") && !atoi(getenv("RIO2PAD_FREEPLAY")) && st.btn[p][PAD_Y] && !g_pads[p].btn[PAD_Y])
                        g_pads[p].coin_t = t;
                    for (int b = 0; b < 32; b++) g_pads[p].btn[b] = st.btn[p][b];
                    for (int a = 0; a < 8; a++) g_pads[p].axis[a] = st.axis[p][a];
                }
            }
            fclose(sf);
        }
    }
    for (int p = 0; p < MAX_PADS; p++) {
        float sx = g_pads[p].axis[3] / 32767.0f, sy = g_pads[p].axis[4] / 32767.0f;   // right stick (or left stick if larger)
        float lx = g_pads[p].axis[0] / 32767.0f, ly = g_pads[p].axis[1] / 32767.0f;
        if (lx * lx + ly * ly > sx * sx + sy * sy) { sx = lx; sy = ly; }
        if (p == 0 && g_mouse_owns) {           // mouse keeps P1's aim until a stick is pushed
            if (sx * sx + sy * sy < 0.3f * 0.3f) continue;
            g_mouse_owns = 0;
        }
        if (g_aim_mode) {                       // absolute: centre of stick = centre of screen
            g_aim[p][0] = 0.5f + 0.5f * sx;
            g_aim[p][1] = 0.5f + 0.5f * sy;
            continue;
        }
        float dz = 0.15f;                       // deadzone, then a curve for fine aiming
        if (sx > -dz && sx < dz) sx = 0;
        if (sy > -dz && sy < dz) sy = 0;
        float speed = rio2pad_stick_speed;                   // screen widths per second at full tilt
        g_aim[p][0] += sx * (sx < 0 ? -sx : sx) * speed * dt;
        g_aim[p][1] += sy * (sy < 0 ? -sy : sy) * speed * dt;
        for (int k = 0; k < 2; k++) {
            if (g_aim[p][k] < 0) g_aim[p][k] = 0;
            if (g_aim[p][k] > 1) g_aim[p][k] = 1;
        }
    }
}

static void install_hook(void)
{
    // Trampoline: the 14 overwritten bytes are "test %edi,%edi; js rel32; cmp $0x1ff,%edi".
    unsigned char *t = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *p = t;
    *p++ = 0x85; *p++ = 0xff;                          // test %edi,%edi
    *p++ = 0x79; *p++ = 14;                            // jns +14 (skip the abs jmp)
    write_abs_jmp(p, SW_GET_JS_TGT); p += 14;          // js taken -> original target
    *p++ = 0x81; *p++ = 0xff; memcpy(p, "\xff\x01\x00\x00", 4); p += 4;   // cmp $0x1ff,%edi
    write_abs_jmp(p, SW_GET_RESUME);                   // flags survive the jmp
    sw_get_orig = (sw_get_fn)t;

    unsigned char *f = (unsigned char *)SW_GET;
    static const unsigned char expect[14] = {0x85,0xff,0x0f,0x88,0xd0,0x05,0x00,0x00,0x81,0xff,0xff,0x01,0x00,0x00};
    if (memcmp(f, expect, 14) != 0) {
        fprintf(stderr, "[rio2pad] unexpected bytes at sw_get, not hooking (different game build?)\n");
        return;
    }
    mprotect((void *)(SW_GET & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    write_abs_jmp(f, (uint64_t)sw_get_hook);
}

// Cabinet adjustments are read with adj_get(section, key) @0x53f270. "sound"/"gameVolume" is the
// in-game and cutscene volume (attract uses "attractVolume"); on this install it is 0, so gameplay
// and cutscenes were silent while attract played. Report 100 instead of 0 (RIO2PAD_GAMEVOL=n forces n).
#define ADJ_GET 0x53f270UL
typedef int (*adj_get_fn)(const char *, const char *);
static adj_get_fn adj_get_orig;
static int adj_get_hook(const char *sec, const char *key)
{
    int v = adj_get_orig(sec, key);
    // Free play: no coins needed to start (RIO2PAD_FREEPLAY=0 restores the cabinet's setting).
    if (key && !strcmp(key, "freeplay") && !(getenv("RIO2PAD_FREEPLAY") && !atoi(getenv("RIO2PAD_FREEPLAY")))) {
        static int said;
        if (!said++) fprintf(stderr, "[rio2pad] adjustment freeplay = %d -> 1\n", v);
        return 1;
    }
    if (key && strstr(key, "olume")) {
        static char seen[8][32];
        int r = v;
        if (!strcmp(key, "gameVolume")) {
            if (getenv("RIO2PAD_GAMEVOL")) r = atoi(getenv("RIO2PAD_GAMEVOL"));
            else if (v == 0) r = 100;
        }
        for (int i = 0; i < 8; i++) {
            if (!strcmp(seen[i], key)) break;
            if (!seen[i][0]) {
                snprintf(seen[i], sizeof seen[i], "%s", key);
                fprintf(stderr, "[rio2pad] adjustment %s/%s = %d%s\n", sec ? sec : "?", key, v,
                        r != v ? " (overridden)" : "");
                break;
            }
        }
        v = r;
    }
    return v;
}

static void hook_adj_get(void)
{
    static const unsigned char expect[17] = {0x48,0x83,0xec,0x08,0x48,0x89,0xf2,0x31,0xc9,0x48,0x89,0xfe,
                                             0xbf,0xc0,0x3c,0xcc,0x01};
    unsigned char *f = (unsigned char *)ADJ_GET;
    if (memcmp(f, expect, 17)) { fprintf(stderr, "[rio2pad] unexpected bytes at adj_get, not hooking\n"); return; }
    unsigned char *t = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) return;
    memcpy(t, f, 17);                      // all position-independent
    write_abs_jmp(t + 17, ADJ_GET + 17);   // resume at the call @0x53f281
    adj_get_orig = (adj_get_fn)t;
    // Free play: blank the "Money In $n" amount (saved coins from testing). MoneyInCallback
    // @0x447bc0 (buf in rsi) becomes: movb $0,(%rsi); ret.
    if (!(getenv("RIO2PAD_FREEPLAY") && !atoi(getenv("RIO2PAD_FREEPLAY")))) {
        unsigned char *m = (unsigned char *)0x447bc0UL;
        mprotect((void *)(0x447bc0UL & ~0xfffUL), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
        if (!memcmp(m, "\x41\x55\x41\x54\x49\x89\xfd", 7)) memcpy(m, "\xc6\x06\x00\xc3", 4);
        else fprintf(stderr, "[rio2pad] unexpected bytes at MoneyInCallback, left alone\n");
    }
    mprotect((void *)(ADJ_GET & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    write_abs_jmp(f, (uint64_t)adj_get_hook);
}

// Open any newly plugged-in, non-mouse joystick into the first free player slot.
static void scan_pads(void)
{
    const char *dirs[2] = {"/dev/input", getenv("RIO2PAD_JSDIR")};
    for (int d = 0; d < 2; d++) {
        if (!dirs[d] || !*dirs[d]) continue;
        for (int n = 0; n < 16; n++) {
            char path[64], name[128] = "Gamepad";
            snprintf(path, sizeof path, "%s/js%d", dirs[d], n);
            int taken = 0, slot = -1;
            for (int p = 0; p < MAX_PADS; p++) {
                if (g_pads[p].fd >= 0 && !strcmp(g_pads[p].path, path)) taken = 1;
                if (g_pads[p].fd == -1 && slot < 0) slot = p;
            }
            if (taken || slot < 0) continue;
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            ioctl(fd, JSIOCGNAME(sizeof name), name);
            if (strcasestr(name, "mouse")) { close(fd); continue; }
            memset((void *)g_pads[slot].btn, 0, sizeof g_pads[slot].btn);
            memset((void *)g_pads[slot].axis, 0, sizeof g_pads[slot].axis);
            snprintf(g_pads[slot].path, sizeof g_pads[slot].path, "%s", path);
            g_pads[slot].fd = fd;
            fprintf(stderr, "[rio2pad] player %d = %s (%s)\n", slot + 1, path, name);
        }
    }
}

// ---- mouse, through our own X11 connection (libX11 loaded at run time) ----
typedef unsigned long XID_;
static struct {
    void *dpy;
    void *(*open)(const char *);
    int (*focus)(void *, XID_ *, int *);
    int (*query)(void *, XID_, XID_ *, XID_ *, int *, int *, int *, int *, unsigned *);
    int (*geom)(void *, XID_, XID_ *, int *, int *, unsigned *, unsigned *, unsigned *, unsigned *);
    int (*warp)(void *, XID_, XID_, int, int, unsigned, unsigned, int, int);
    int (*flush)(void *);
    XID_ (*atom)(void *, const char *, int);
    int (*prop)(void *, XID_, XID_, long, long, int, XID_, XID_ *, int *, unsigned long *, unsigned long *, unsigned char **);
    int (*xfree)(void *);
    int (*tree)(void *, XID_, XID_ *, XID_ *, XID_ **, unsigned *);
} X;

static int x_init(void)
{
    static int tried;
    if (X.dpy || tried) return X.dpy != NULL;
    tried = 1;
    void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "[rio2pad] mouse: no libX11, mouse off\n"); return 0; }
    X.open = dlsym(h, "XOpenDisplay");          X.focus = dlsym(h, "XGetInputFocus");
    X.query = dlsym(h, "XQueryPointer");        X.geom = dlsym(h, "XGetGeometry");
    X.warp = dlsym(h, "XWarpPointer");          X.flush = dlsym(h, "XFlush");
    X.atom = dlsym(h, "XInternAtom");           X.prop = dlsym(h, "XGetWindowProperty");
    X.xfree = dlsym(h, "XFree");                X.tree = dlsym(h, "XQueryTree");
    if (!X.open || !X.focus || !X.query || !X.geom || !X.warp || !X.flush || !X.atom || !X.prop || !X.xfree || !X.tree) return 0;
    X.dpy = X.open(NULL);
    fprintf(stderr, "[rio2pad] mouse: %s\n", X.dpy ? "X11 connected" : "XOpenDisplay failed, mouse off");
    return X.dpy != NULL;
}

// Is window w (or one of its parents) a window of this process? (_NET_WM_PID; cached for the last window)
static int x_ours(XID_ w)
{
    static XID_ last; static int last_ok;
    if (w == last) return last_ok;
    last = w; last_ok = 0;
    XID_ pid_atom = X.atom(X.dpy, "_NET_WM_PID", 0), cur = w;
    for (int depth = 0; cur && depth < 8; depth++) {
        XID_ type, root, parent, *kids = NULL; int fmt; unsigned long n, after; unsigned char *data = NULL; unsigned nk;
        if (X.prop(X.dpy, cur, pid_atom, 0, 1, 0, 0 /*AnyPropertyType*/, &type, &fmt, &n, &after, &data) == 0 && data) {
            if (n == 1 && fmt == 32) last_ok = (pid_t)*(unsigned long *)data == getpid();
            X.xfree(data);
            if (n == 1) return last_ok;
        }
        if (!X.tree(X.dpy, cur, &root, &parent, &kids, &nk)) break;
        if (kids) X.xfree(kids);
        if (parent == root) break;
        cur = parent;
    }
    return last_ok;
}

static void poll_mouse(void)
{
    static int have_last, lx, ly;
    g_mouse_btn[0] = g_mouse_btn[1] = g_mouse_btn[2] = 0;
    static int (*ui_is_open)(void); static int ui_looked;
    if (!ui_looked) { ui_looked = 1; ui_is_open = (int (*)(void))dlsym(RTLD_DEFAULT, "ravenui_is_open"); }
    if (!rio2pad_mouse_mode || !x_init() || (ui_is_open && ui_is_open())) { have_last = 0; return; }
    XID_ w; int rev;
    X.focus(X.dpy, &w, &rev);
    if (w <= 1 /*None, PointerRoot*/ || !x_ours(w)) { have_last = 0; return; }
    XID_ root, child; int rx, ry, wx, wy, gx, gy; unsigned mask, ww, wh, bw, dep;
    if (!X.geom(X.dpy, w, &root, &gx, &gy, &ww, &wh, &bw, &dep) || !ww || !wh) return;
    if (!X.query(X.dpy, w, &root, &child, &rx, &ry, &wx, &wy, &mask)) { have_last = 0; return; }   // other screen
    int inside = wx >= 0 && wy >= 0 && wx < (int)ww && wy < (int)wh;
    if (rio2pad_mouse_mode == 1) {
        if (have_last && (wx != lx || wy != ly) && inside) {
            g_mouse_owns = 1;
            g_aim[0][0] = (wx + 0.5f) / ww;
            g_aim[0][1] = (wy + 0.5f) / wh;
        }
        lx = wx; ly = wy; have_last = 1;
    } else {
        int mx = ww / 2, my = wh / 2;
        if (have_last && (wx != mx || wy != my)) {
            g_mouse_owns = 1;
            g_aim[0][0] += (float)(wx - mx) / ww * rio2pad_mouse_sens;
            g_aim[0][1] += (float)(wy - my) / wh * rio2pad_mouse_sens;
            for (int k = 0; k < 2; k++) {
                if (g_aim[0][k] < 0) g_aim[0][k] = 0;
                if (g_aim[0][k] > 1) g_aim[0][k] = 1;
            }
        }
        X.warp(X.dpy, 0, w, 0, 0, 0, 0, mx, my);
        X.flush(X.dpy);
        have_last = 1; inside = 1;
    }
    if (inside) {
        g_mouse_btn[0] = !!(mask & (1u << 8));    // Button1Mask
        g_mouse_btn[1] = !!(mask & (1u << 9));    // Button2Mask (middle)
        g_mouse_btn[2] = !!(mask & (1u << 10));   // Button3Mask (right)
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
    fclose(f);
}

static void *pad_thread(void *arg)
{
    (void)arg;
    time_t last_scan = 0, last_log = time(NULL);
    for (;;) {
        time_t now = time(NULL);
        if (now != last_scan) { scan_pads(); last_scan = now; }
        if (g_swlog && now - last_log >= 5) { dump_swlog(); last_log = now; }
        static time_t last_proc;
        if (getenv("RIO2PAD_PROCLOG") && now - last_proc >= 2) {   // live game procs = current screen/state
            static char prev[2048];
            char cur[2048] = "";
            last_proc = now;
            for (int i = 0; i < 64; i++) {
                const char *slot = (const char *)(0x1dd78c0UL + i * 0x448UL);
                if ((slot[0x28] & 1) && slot[0] && strlen(cur) + 42 < sizeof cur) {
                    strncat(cur, " ", sizeof cur - strlen(cur) - 1);
                    strncat(cur, slot, 0x28);
                }
            }
            if (strcmp(cur, prev)) { fprintf(stderr, "[rio2pad] t=%ld procs:%s\n", (long)(now - g_t_start), cur); strcpy(prev, cur); }
        }

        struct pollfd pfd[MAX_PADS];
        int np = 0, map[MAX_PADS];
        for (int p = 0; p < MAX_PADS; p++)
            if (g_pads[p].fd >= 0) { pfd[np].fd = g_pads[p].fd; pfd[np].events = POLLIN; map[np++] = p; }
        poll_mouse();
        update_aim();
        if (poll(pfd, np, 4) <= 0) continue;

        for (int i = 0; i < np; i++) {
            struct pad *pd = &g_pads[map[i]];
            if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "[rio2pad] player %d disconnected\n", map[i] + 1);
                close(pd->fd); pd->fd = -1;       // wireless pad slept/unplugged: slot frees, rescan
                continue;
            }
            struct js_event e;
            while (read(pd->fd, &e, sizeof e) == sizeof e) {
                int type = e.type & ~JS_EVENT_INIT;
                if (type == JS_EVENT_BUTTON && e.number < 32) {
                    if (getenv("RIO2PAD_FREEPLAY") && !atoi(getenv("RIO2PAD_FREEPLAY")) && e.number == PAD_Y && e.value && !pd->btn[PAD_Y] && !(e.type & JS_EVENT_INIT))
                        pd->coin_t = now_ms();
                    pd->btn[e.number] = e.value;
                    if (g_debug) fprintf(stderr, "[rio2pad] P%d button %d = %d\n", map[i] + 1, e.number, e.value);
                } else if (type == JS_EVENT_AXIS && e.number < 8) {
                    pd->axis[e.number] = e.value;
                }
            }
        }
    }
    return NULL;
}

// On shutdown/restart the game joins worker threads it may never have started (handle 0).
// Old glibc tolerated that; current glibc dereferences it and segfaults.
int pthread_timedjoin_np(pthread_t th, void **ret, const struct timespec *abstime)
{
    static int (*real)(pthread_t, void **, const struct timespec *);
    if (!th) return ESRCH;
    if (!real) real = dlsym(RTLD_NEXT, "pthread_timedjoin_np");
    return real(th, ret, abstime);
}

int pthread_join(pthread_t th, void **ret)
{
    static int (*real)(pthread_t, void **);
    if (!th) return ESRCH;
    if (!real) real = dlsym(RTLD_NEXT, "pthread_join");
    return real(th, ret);
}

// thread_join_ms(&handle, ms) wrapper: skip the join when the worker was never started.
static void safe_join_ms(pthread_t *h, int ms)
{
    if (!h || !*h) return;
    ((void (*)(pthread_t *, int))THREAD_JOIN_MS)(h, ms);
}

static void patch_shutdown_join(void)
{
    unsigned char *call = (unsigned char *)SHUTDOWN_JOIN_CALL;
    if (call[0] != 0xe8 || (uintptr_t)(call + 5) + *(int32_t *)(call + 1) != THREAD_JOIN_MS) {
        fprintf(stderr, "[rio2pad] unexpected shutdown join call, not patching\n");
        return;
    }
    // rel32 must reach the stub, so place it in low memory near the game image.
    unsigned char *stub = mmap((void *)0x30000000UL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (stub == MAP_FAILED) { fprintf(stderr, "[rio2pad] no stub memory, not patching\n"); return; }
    write_abs_jmp(stub, (uint64_t)safe_join_ms);
    mprotect((void *)(SHUTDOWN_JOIN_CALL & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(int32_t *)(call + 1) = (int32_t)((uintptr_t)stub - (uintptr_t)(call + 5));
}

// The protection wrapper dlclose()s library handles that are no longer valid on current glibc,
// which crashes inside ld.so. Make its dlclose a no-op (the handles just stay loaded).
static void patch_envelope_dlclose(void)
{
    unsigned char *f = (unsigned char *)ENVELOPE_DLCLOSE;
    if (f[0] != 0xff || f[1] != 0x25) {
        fprintf(stderr, "[rio2pad] unexpected dlclose stub, not patching\n");
        return;
    }
    mprotect((void *)(ENVELOPE_DLCLOSE & ~0xfffUL), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
    memcpy(f, "\x31\xc0\xc3", 3);   // xor %eax,%eax; ret
}

// irt_systemSetCabinetType() starts with "lea -0x7(%rdi),%eax" to index a 5-entry jump table
// (types 7..11). The I/O layer passes 0 here, which leaves the gun count at 0. Replacing the lea
// with "xor %eax,%eax; nop" always takes the type-7 case: 2 guns.
// RIO2PAD_SKIPBOOT=1 (testing): never start the boot-time File Test or Gun Calibration procs.
// Each site is "call proc_start(name, 0)" @0x5aa0b0; the code after it waits while the proc runs,
// so with the call removed the wait ends at once and boot continues to attract.
static void patch_skip_boot_checks(void)
{
    static const uintptr_t sites[] = {
        // filetest_proc (0x49e8c6) is left running: its menu shows anyway, and START skips it.
        0x417edd,   // proc_start("GunCalProc")
        0x417f08,   // proc_start("GunCalProc"), second path
    };
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
        unsigned char *c = (unsigned char *)sites[i];
        mprotect((void *)(sites[i] & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
        if (c[0] != 0xe8 || sites[i] + 5 + *(int32_t *)(c + 1) != 0x5aa0b0UL) {
            fprintf(stderr, "[rio2pad] skipboot: unexpected code at %#lx, left alone\n", (unsigned long)sites[i]);
            continue;
        }
        memset(c, 0x90, 5);
    }
    // Boot also calls guns_need_cal() @0x417aa0, which calibrates each gun inline (0x4168f0) when its
    // saved "gun%iCalibration" is missing. Make it return 0 ("all calibrated").
    unsigned char *nc = (unsigned char *)0x417aa0UL;
    mprotect((void *)(0x417aa0UL & ~0xfffUL), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (!memcmp(nc, "\x53\x48\x83\xec\x50", 5)) memcpy(nc, "\x31\xc0\xc3", 3);   // xor eax,eax; ret
    else fprintf(stderr, "[rio2pad] skipboot: unexpected code at 0x417aa0, left alone\n");
    fprintf(stderr, "[rio2pad] skipboot: file test and gun calibration disabled\n");
}

static void patch_gun_count(void)
{
    unsigned char *f = (unsigned char *)IRT_SET_CABTYPE;
    if (memcmp(f, "\x8d\x47\xf9", 3) != 0) {
        fprintf(stderr, "[rio2pad] unexpected irt cabinet-type code, not patching\n");
        return;
    }
    mprotect((void *)(IRT_SET_CABTYPE & ~0xfffUL), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
    memcpy(f, "\x31\xc0\x90", 3);
}

__attribute__((constructor)) static void rio2pad_init(void)
{
    // Only act inside the game itself, not in helper processes it may spawn.
    char exe[256] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0 || !strstr(exe, "/game")) return;
    g_t_start = time(NULL);
    g_debug = getenv("RIO2PAD_DEBUG") != NULL;
    g_autostart = getenv("RIO2PAD_AUTOSTART") != NULL;
    g_swlog = getenv("RIO2PAD_SWLOG");
    if (getenv("RIO2PAD_INPUTLOG")) g_inputlog = fopen(getenv("RIO2PAD_INPUTLOG"), "a");
    if (getenv("RIO2PAD_PROBE")) g_probe = atoi(getenv("RIO2PAD_PROBE"));
    if (getenv("RIO2PAD_DUMPREC")) g_dumprec = atoi(getenv("RIO2PAD_DUMPREC"));
    if (getenv("RIO2PAD_PROBESTEP")) g_probe_step = atoi(getenv("RIO2PAD_PROBESTEP"));
    if (getenv("RIO2PAD_PROBELIST")) {
        char *s = strdup(getenv("RIO2PAD_PROBELIST")), *tok;
        g_probe_t0 = time(NULL) + strtol(s, &tok, 10);
        while (*tok && g_probe_n < 64) g_probe_list[g_probe_n++] = strtol(tok + 1, &tok, 10);
    }
    if (getenv("RIO2PAD_COIN"))
        sscanf(getenv("RIO2PAD_COIN"), "%d,%d,%d,%d", &g_coin_ids[0], &g_coin_ids[1], &g_coin_ids[2], &g_coin_ids[3]);
    // Cabinet type decides which I/O modules start; 3 (4P) and 8 (2P) enable the IR gun module (bit 0x8).
    // The game reads the cached value first, so seeding it overrides the stored setting.
    *G_CABINET_TYPE = getenv("RIO2PAD_CABTYPE") ? atoi(getenv("RIO2PAD_CABTYPE")) : 8;
    install_hook();
    hook_adj_get();
    patch_shutdown_join();
    patch_envelope_dlclose();
    patch_gun_count();
    {   // Hide the attract "Factory Setup Has Not Been Completed!" banner: attract @0x448e4f does
        // `je banner` when factory_setup_done() @0x444a80 is 0. NOP that jump (RIO2PAD_FACTORYBANNER=1 keeps it).
        unsigned char *j = (unsigned char *)0x448e4fUL;
        if (!getenv("RIO2PAD_FACTORYBANNER")) {
            mprotect((void *)(0x448e4fUL & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
            if (!memcmp(j, "\x0f\x84\x4b\x03\x00\x00", 6)) memset(j, 0x90, 6);
            else fprintf(stderr, "[rio2pad] unexpected bytes at factory banner check, left alone\n");
        }
    }
    if (getenv("RIO2PAD_SKIPBOOT") && atoi(getenv("RIO2PAD_SKIPBOOT"))) { g_skipboot = 1; patch_skip_boot_checks(); }
    for (int p = 0; p < MAX_PADS; p++) g_aim[p][0] = g_aim[p][1] = 0.5f;
    if (getenv("RIO2PAD_STICKSPEED")) rio2pad_stick_speed = strtof(getenv("RIO2PAD_STICKSPEED"), NULL);
    g_aim_mode = getenv("RIO2PAD_AIM") && !strcmp(getenv("RIO2PAD_AIM"), "abs");
    if (getenv("RIO2PAD_MOUSE")) {
        const char *m = getenv("RIO2PAD_MOUSE");
        rio2pad_mouse_mode = !strcmp(m, "relative") ? 2 : (!strcmp(m, "0") || !strcmp(m, "off")) ? 0 : 1;
    }
    if (getenv("RIO2PAD_MOUSESENS")) rio2pad_mouse_sens = strtof(getenv("RIO2PAD_MOUSESENS"), NULL);
    mprotect((void *)(IRT_GUN_CONNECTED & ~0xfffUL), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
    mprotect((void *)(IRT_GUN_AIM & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (*(unsigned char *)IRT_GUN_CONNECTED == 0xe9 && *(unsigned char *)IRT_GUN_AIM == 0x48) {
        write_abs_jmp((unsigned char *)IRT_GUN_CONNECTED, (uint64_t)gun_connected);
        write_abs_jmp((unsigned char *)IRT_GUN_AIM, (uint64_t)gun_aim);
    } else {
        fprintf(stderr, "[rio2pad] unexpected gun getter code, not hooking guns\n");
    }
    pthread_t th;
    pthread_create(&th, NULL, pad_thread, NULL);
    pthread_detach(th);
}
