// PlayStation controllers without DS4Windows/Steam: DualShock 4 (v1, v2, USB wireless adapter),
// DualSense and DualSense Edge, over USB or Bluetooth, read straight from Windows HID.
// rio2pad_win.c asks pspad_get() for pads beyond the XInput ones, as an XInput-shaped state.
//
// DS4Windows (and similar: DS4Windows.exe, DSX.exe) already turns these into virtual Xbox pads that
// XInput reads; reading the real pad too would give that player double input, so while one of
// them runs the direct path stays off. RAVEN_PSPAD=0 turns it off by hand.
#include "shim.h"
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <tlhelp32.h>
#include <xinput.h>
#include <string.h>
#include <stdlib.h>

#define MAXPS 4
struct ps {
    HANDLE h;
    int kind;                  // 1 = DualShock 4, 2 = DualSense
    char path[512];
    volatile LONG alive;
    CRITICAL_SECTION cs;
    XINPUT_GAMEPAD g;
};
static struct ps g_ps[MAXPS];
static volatile LONG g_off;    // 1 = DS4Windows-like tool running (or disabled)
static int g_inited;

static int pid_kind(USHORT vid, USHORT pid)
{
    if (vid != 0x054c) return 0;
    if (pid == 0x05c4 || pid == 0x09cc || pid == 0x0ba0) return 1;   // DS4 v1, v2, wireless adapter
    if (pid == 0x0ce6 || pid == 0x0df2) return 2;                    // DualSense, DualSense Edge
    return 0;
}

static SHORT stick(BYTE v) { int x = ((int)v - 128) * 256; return (SHORT)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }
static SHORT stick_y(BYTE v) { int x = (128 - (int)v) * 256; return (SHORT)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }

// One input report -> XInput layout. d points at the left-stick X byte.
// DS4 (and DualSense's short Bluetooth report): LX LY RX RY | hat+face | L1 R1 L2 R2 share options L3 R3 | PS | L2 R2
// DualSense full report:                        LX LY RX RY L2 R2 counter | hat+face | shoulders.. | PS
static void decode(struct ps *p, const BYTE *d, int full_ds)
{
    BYTE face, sh, ps, l2, r2;
    if (full_ds) { l2 = d[4]; r2 = d[5]; face = d[7]; sh = d[8]; ps = d[9]; }
    else { face = d[4]; sh = d[5]; ps = d[6]; l2 = d[7]; r2 = d[8]; }
    XINPUT_GAMEPAD g = {0};
    static const WORD hat[8] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP};
    if ((face & 15) < 8) g.wButtons |= hat[face & 15];
    if (face & 0x10) g.wButtons |= XINPUT_GAMEPAD_X;   // square
    if (face & 0x20) g.wButtons |= XINPUT_GAMEPAD_A;   // cross
    if (face & 0x40) g.wButtons |= XINPUT_GAMEPAD_B;   // circle
    if (face & 0x80) g.wButtons |= XINPUT_GAMEPAD_Y;   // triangle
    if (sh & 0x01) g.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (sh & 0x02) g.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (sh & 0x10) g.wButtons |= XINPUT_GAMEPAD_BACK;  // share / create
    if (sh & 0x20) g.wButtons |= XINPUT_GAMEPAD_START; // options
    if (sh & 0x40) g.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (sh & 0x80) g.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (ps & 0x02) g.wButtons |= XINPUT_GAMEPAD_BACK;  // touchpad click: also Back (test menu)
    g.sThumbLX = stick(d[0]); g.sThumbLY = stick_y(d[1]);
    g.sThumbRX = stick(d[2]); g.sThumbRY = stick_y(d[3]);
    g.bLeftTrigger = l2; g.bRightTrigger = r2;
    EnterCriticalSection(&p->cs);
    p->g = g;
    LeaveCriticalSection(&p->cs);
}

static DWORD WINAPI reader(void *arg)
{
    struct ps *p = arg;
    BYTE buf[600];
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (;;) {
        DWORD n = 0;
        ResetEvent(ov.hEvent);
        if (!ReadFile(p->h, buf, sizeof buf, NULL, &ov) && GetLastError() != ERROR_IO_PENDING) break;
        if (WaitForSingleObject(ov.hEvent, 3000) != WAIT_OBJECT_0) { CancelIo(p->h); continue; }   // idle pad: keep waiting
        if (!GetOverlappedResult(p->h, &ov, &n, FALSE)) break;
        if (g_off || n < 10) continue;
        // Report ids: 0x01 = USB (both) or short Bluetooth, 0x11 = DS4 Bluetooth, 0x31 = DualSense Bluetooth.
        if (buf[0] == 0x01) decode(p, buf + 1, p->kind == 2 && n >= 60);
        else if (buf[0] == 0x11 && p->kind == 1) decode(p, buf + 3, 0);
        else if (buf[0] == 0x31 && p->kind == 2) decode(p, buf + 2, 1);
    }
    raven_log("[pspad] %s controller unplugged\n", p->kind == 2 ? "DualSense" : "DualShock 4");
    CloseHandle(ov.hEvent);
    CloseHandle(p->h);
    InterlockedExchange(&p->alive, 0);
    return 0;
}

static int known(const char *path)
{
    for (int i = 0; i < MAXPS; i++) if (g_ps[i].alive && !strcmp(g_ps[i].path, path)) return 1;
    return 0;
}

static void scan(void)
{
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsA(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof ifd};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        char raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 512];
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)raw;
        det->cbSize = sizeof *det;
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, sizeof raw, NULL, NULL)) continue;
        if (known(det->DevicePath)) continue;
        HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at = {sizeof at};
        int kind = HidD_GetAttributes(h, &at) ? pid_kind(at.VendorID, at.ProductID) : 0;
        int slot = -1;
        for (int k = 0; k < MAXPS && slot < 0; k++) if (!g_ps[k].alive) slot = k;
        if (!kind || slot < 0) { CloseHandle(h); continue; }
        struct ps *p = &g_ps[slot];
        if (kind == 1) {   // DS4 over Bluetooth sends full reports (0x11) only after this feature read
            BYTE f[64] = {0x02};
            HidD_GetFeature(h, f, sizeof f);
        }
        p->h = h; p->kind = kind;
        snprintf(p->path, sizeof p->path, "%s", det->DevicePath);
        memset(&p->g, 0, sizeof p->g);
        InterlockedExchange(&p->alive, 1);
        CloseHandle(CreateThread(NULL, 0, reader, p, 0, NULL));
        raven_log("[pspad] %s controller found (%04x:%04x)\n", kind == 2 ? "DualSense" : "DualShock 4", at.VendorID, at.ProductID);
    }
    SetupDiDestroyDeviceInfoList(set);
}

static int tool_running(void)
{
    static const char *names[] = {"DS4Windows.exe", "DSX.exe", "DS4Tool.exe"};
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 e = {sizeof e};
    int found = 0;
    for (BOOL ok = Process32First(s, &e); ok && !found; ok = Process32Next(s, &e))
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++) if (!_stricmp(e.szExeFile, names[i])) found = 1;
    CloseHandle(s);
    return found;
}

// Every 2 s: new pads (hot-plug) and whether DS4Windows is up.
static DWORD WINAPI watcher(void *arg)
{
    (void)arg;
    int was = -1;
    for (;;) {
        int off = tool_running();
        if (off != was) {
            raven_log(off ? "[pspad] DS4Windows (or similar) is running: PlayStation pads come in through it as Xbox pads\n"
                          : "[pspad] reading PlayStation controllers directly\n");
            was = off;
        }
        InterlockedExchange(&g_off, off);
        if (!off) scan();
        Sleep(2000);
    }
    return 0;
}

void pspad_start(void)
{
    const char *e = raven_env("RAVEN_PSPAD");
    if (e && *e == '0') { raven_log("[pspad] off (RAVEN_PSPAD=0)\n"); return; }
    for (int i = 0; i < MAXPS; i++) InitializeCriticalSection(&g_ps[i].cs);
    g_inited = 1;
    CloseHandle(CreateThread(NULL, 0, watcher, NULL, 0, NULL));
}

// n-th connected PlayStation pad (0-based) as an XInput state; 0 = none.
int pspad_get(int n, XINPUT_STATE *st)
{
    if (!g_inited || g_off) return 0;
    for (int i = 0; i < MAXPS; i++) {
        if (!g_ps[i].alive) continue;
        if (n-- == 0) {
            memset(st, 0, sizeof *st);
            EnterCriticalSection(&g_ps[i].cs);
            st->Gamepad = g_ps[i].g;
            LeaveCriticalSection(&g_ps[i].cs);
            return 1;
        }
    }
    return 0;
}
