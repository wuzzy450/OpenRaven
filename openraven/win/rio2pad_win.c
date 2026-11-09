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
static int g_coin_ids[MAX_PADS] = {570, 571, -1, -1};
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
    *x = g_aim[g][0];
    *y = 1.0f - g_aim[g][1];
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

static int trigger_pressed(int p)
{
    static int block[MAX_PADS];
    int down = g_pads[p].active && g_pads[p].axis[5] > 0;
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

// Gun buttons as the cabinet I/O board reported them to the game's switch dispatcher (0x4ad80c):
// record +4 = new presses, +8 = new releases, +0xc = held. Reporting the level in +4 instead made the
// game see a fresh press every frame and never "held", so full-auto weapons (needler, plasma rifle)
// never got their automatic fire. One physical button can feed two logical ids, so the dispatcher may
// ask twice in one poll: edges are worked out once per poll (calls < 3 ms apart share them).
#define SW_DISPATCH_RET 0x4ad811ULL
static void gun_report(int id, int level, unsigned char *f)
{
    static int last[10], snap_p[10], snap_r[10];
    static double snap_t[10];
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    double t = (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
    int k = id - 793;
    static int said;
    if (!said) { said = 1; raven_log("[rio2pad] gun buttons: edge + held reporting (RIO2PAD_GUNLEVEL=1 = old way)\n"); }
    if (t - snap_t[k] >= 3.0) {
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
    for (int i = 0; i < 4; i++)
        if (G_START_IDS[i] && id == G_START_IDS[i]) pressed = start_pressed(i) || (i == 0 && boot_start_pulse());
    if (id >= 793 && id <= 802) {   // gun buttons, 5 per gun: +2 trigger, +3 reload, +4 grenade
        int p = (id - 793) / 5, k = (id - 793) % 5;
        pressed = 0;
        if (k == 2) pressed = trigger_pressed(p);
        if (k == 3) pressed = btn(p, PAD_RB);
        if (k == 4) pressed = g_pads[p].active && g_pads[p].axis[2] > 0;
    }
    if (id == 581) pressed = start_pressed(0);   // gun cabinet player START
    if (id == 582) pressed = start_pressed(1);
    for (int i = 0; i < 3; i++)
        if (G_TEST_IDS[i] && id == G_TEST_IDS[i]) pressed = test_pressed();
    for (int i = 0; i < MAX_PADS; i++)
        if (g_coin_ids[i] >= 0 && id == g_coin_ids[i]) pressed = coin_pulse(i);
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
    if (id >= 793 && id <= 802 && caller == SW_DISPATCH_RET && !g_gunlevel) gun_report(id, pressed, f);
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
    unsigned char *f = (unsigned char *)IRT_SET_CABTYPE;   // always 2 guns
    if (!memcmp(f, "\x8d\x47\xf9", 3)) { make_writable(IRT_SET_CABTYPE, 3); memcpy(f, "\x31\xc0\x90", 3); }
    else raven_log("[rio2pad] unexpected irt cabinet-type code, not patching\n");

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
            g_aim[0][0] = (c.x + 0.5f) / rc.right;
            g_aim[0][1] = (c.y + 0.5f) / rc.bottom;
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
    for (int p = 0; p < MAX_PADS; p++) {
        struct pad *pd = &g_pads[p];
        XINPUT_STATE st;
        int ok = p_XInputGetState && p_XInputGetState(p, &st) == ERROR_SUCCESS;
        if (ok != was_active[p]) {
            raven_log("[rio2pad] player %d %s\n", p + 1, ok ? "= XInput pad connected" : "pad disconnected");
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
        pd->active = ok ? 1 : (p == 0 || kb) ? 2 : 0;   // keyboard always drives player 1
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
    for (int p = 0; p < MAX_PADS; p++) {
        float sx = g_pads[p].axis[3] / 32767.0f, sy = g_pads[p].axis[4] / 32767.0f;
        float lx = g_pads[p].axis[0] / 32767.0f, ly = g_pads[p].axis[1] / 32767.0f;
        if (lx * lx + ly * ly > sx * sx + sy * sy) { sx = lx; sy = ly; }
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

static DWORD WINAPI pad_thread(void *arg)
{
    (void)arg;
    timeBeginPeriod(1);
    long long last_log = now_ms(), last_proc = 0;
    for (;;) {
        long long t = now_ms();
        if (g_swlog && t - last_log >= 5000) { dump_swlog(); last_log = t; }
        if (g_proclog && t - last_proc >= 500) { log_procs(); last_proc = t; }
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
    g_freeplay = env_int("RIO2PAD_FREEPLAY", 1) != 0;
    g_skipboot = env_int("RIO2PAD_SKIPBOOT", 1) != 0;
    g_gamevol = env_int("RIO2PAD_GAMEVOL", -1);
    g_probe = env_int("RIO2PAD_PROBE", -1);
    static char swlog[256];
    if (raven_env("RIO2PAD_SWLOG")) { snprintf(swlog, sizeof swlog, "%s", raven_env("RIO2PAD_SWLOG")); g_swlog = swlog; }
    if (raven_env("RIO2PAD_INPUTLOG")) g_inputlog = fopen(raven_env("RIO2PAD_INPUTLOG"), "a");
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

    *G_CABINET_TYPE = env_int("RIO2PAD_CABTYPE", 8);
    install_sw_hook();
    install_adj_hook();
    patch_misc();
    if (g_skipboot) patch_skip_boot_checks();
    for (int p = 0; p < MAX_PADS; p++) g_aim[p][0] = g_aim[p][1] = 0.5f;

    HMODULE x = LoadLibraryA("xinput1_4.dll");
    if (!x) x = LoadLibraryA("xinput9_1_0.dll");
    if (x) p_XInputGetState = (xinput_get_state_fn)(void *)GetProcAddress(x, "XInputGetState");
    raven_log("[rio2pad] XInput %s\n", p_XInputGetState ? "loaded" : "NOT available (keyboard only)");
    CloseHandle(CreateThread(NULL, 0, pad_thread, NULL, 0, NULL));
}
