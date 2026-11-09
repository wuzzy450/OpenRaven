// X11 over Win32, enough for the game's static SDL 2.0.2 X11 backend.
// One fake Display with Xlib's LP64 struct layout (SDL reads fields through
// Xlib macros), one Screen, one TrueColor visual. X windows are Win32
// windows; Win32 messages become XEvents in a queue. Keycodes are Linux
// evdev codes + 8 so SDL's keymap detection picks its evdev table.
#include "shim.h"
#include "x11_shim.h"
#include <windows.h>
#include <windowsx.h>
#include <stdlib.h>
#include <string.h>

// ---- display / screen / visual ---------------------------------------------
static unsigned char g_dpy[4096];   // Display (Xlib _XPrivDisplay layout)
static unsigned char g_screen[128]; // Screen
static unsigned char g_visual[56];  // Visual
static int g_scr_w, g_scr_h;
#define DPY_FD 900                  // ConnectionNumber; select() knows it
#define VISUAL_ID 0x21
#define ROOT_WIN 1

#define SET(base, off, type, v) (*(type *)((base) + (off)) = (v))

void *x11_visual(void) { return g_visual; }
int x11_screen_w(void) { return g_scr_w; }
int x11_screen_h(void) { return g_scr_h; }

static void init_display(void) {
    if (g_scr_w) return;
    g_scr_w = GetSystemMetrics(SM_CXSCREEN);
    g_scr_h = GetSystemMetrics(SM_CYSCREEN);
    // Visual
    SET(g_visual, 8, uint64_t, VISUAL_ID);
    SET(g_visual, 16, int, 4);  // TrueColor
    SET(g_visual, 24, uint64_t, 0xff0000);
    SET(g_visual, 32, uint64_t, 0x00ff00);
    SET(g_visual, 40, uint64_t, 0x0000ff);
    SET(g_visual, 48, int, 8);
    SET(g_visual, 52, int, 256);
    // Screen
    SET(g_screen, 8, void *, g_dpy);
    SET(g_screen, 16, uint64_t, ROOT_WIN);
    SET(g_screen, 24, int, g_scr_w);
    SET(g_screen, 28, int, g_scr_h);
    SET(g_screen, 32, int, g_scr_w * 254 / 960);  // mm at 96 dpi
    SET(g_screen, 36, int, g_scr_h * 254 / 960);
    SET(g_screen, 56, int, 24);
    SET(g_screen, 64, void *, g_visual);
    SET(g_screen, 80, uint64_t, 1);           // default colormap
    SET(g_screen, 88, uint64_t, 0xffffff);    // white
    SET(g_screen, 96, uint64_t, 0);           // black
    // Display
    SET(g_dpy, 16, int, DPY_FD);
    SET(g_dpy, 24, int, 11);
    SET(g_dpy, 32, const char *, "ravenloader");
    SET(g_dpy, 84, int, 32);   // bitmap_unit
    SET(g_dpy, 88, int, 32);   // bitmap_pad
    SET(g_dpy, 116, int, 11804000);  // release
    SET(g_dpy, 192, unsigned, 65535);
    SET(g_dpy, 216, const char *, ":0");
    SET(g_dpy, 224, int, 0);   // default_screen
    SET(g_dpy, 228, int, 1);   // nscreens
    SET(g_dpy, 232, void *, g_screen);
    SET(g_dpy, 256, int, 8);   // min_keycode
    SET(g_dpy, 260, int, 255); // max_keycode
}

// ---- atoms -------------------------------------------------------------------
static char *g_atoms[512];
static int g_natoms = 69;  // predefined atoms occupy 1..68
static SV uint64_t s_XInternAtom(void *d, const char *name, int only_if_exists) {
    (void)d; (void)only_if_exists;
    static const char *pre[] = {"PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL"};
    for (int i = 0; i < 6; i++) if (!strcmp(name, pre[i])) return (uint64_t)i + 1;
    if (!strcmp(name, "STRING")) return 31;
    if (!strcmp(name, "WM_NAME")) return 39;
    for (int i = 69; i < g_natoms; i++) if (!strcmp(g_atoms[i], name)) return (uint64_t)i;
    if (g_natoms >= 512) return 0;
    g_atoms[g_natoms] = _strdup(name);
    return (uint64_t)g_natoms++;
}
static const char *atom_name(uint64_t a) { return a >= 69 && a < (uint64_t)g_natoms ? g_atoms[a] : ""; }
static SV char *s_XGetAtomName(void *d, uint64_t a) { (void)d; return _strdup(a < 69 ? "PREDEFINED" : atom_name(a)); }

// ---- windows -----------------------------------------------------------------
typedef struct {
    HWND hwnd;
    int x, y, w, h, mapped, override, fullscreen;
    int req_w, req_h;   // size the game asked for (its render size)
    long mask;
    uint64_t cursor;
    RECT saved;
    DWORD saved_style;
} xwin;
static xwin g_win[64];  // id = index; 0 unused, 1 = root
static int g_nwin = 2;
static uint64_t g_focus;
static uint64_t g_atom_protocols, g_atom_delete;

static xwin *W(uint64_t id) { return id >= 2 && id < (uint64_t)g_nwin ? &g_win[id] : NULL; }
HWND x11_hwnd(uint64_t id) { xwin *w = W(id); return w ? w->hwnd : NULL; }
int x11_req_size(uint64_t id, int *rw, int *rh) {
    xwin *w = W(id);
    if (!w || w->req_w <= 0 || w->req_h <= 0) return 0;
    *rw = w->req_w; *rh = w->req_h;
    return 1;
}
static uint64_t id_of(HWND h) {
    for (int i = 2; i < g_nwin; i++) if (g_win[i].hwnd == h) return (uint64_t)i;
    return 0;
}

// ---- event queue (XEvent = 192 bytes) ----------------------------------------
typedef struct { unsigned char b[192]; } xev;
static xev g_q[1024];
static int g_qh, g_qt;
static unsigned long g_serial;
static int q_len(void) { return (g_qt - g_qh + 1024) % 1024; }
static unsigned char *q_new(int type, uint64_t win) {
    if (q_len() == 1023) g_qh = (g_qh + 1) % 1024;  // drop oldest
    unsigned char *e = g_q[g_qt].b;
    g_qt = (g_qt + 1) % 1024;
    memset(e, 0, 192);
    SET(e, 0, int, type);
    SET(e, 8, uint64_t, ++g_serial);
    SET(e, 24, void *, g_dpy);
    SET(e, 32, uint64_t, win);
    return e;
}
enum { KeyPress = 2, KeyRelease, ButtonPress, ButtonRelease, MotionNotify, EnterNotify, LeaveNotify,
       FocusIn, FocusOut, Expose = 12, UnmapNotify = 18, MapNotify = 19, ConfigureNotify = 22,
       ClientMessage = 33 };

static unsigned mod_state(void) {
    unsigned s = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) s |= 1;
    if (GetKeyState(VK_CAPITAL) & 1) s |= 2;
    if (GetKeyState(VK_CONTROL) & 0x8000) s |= 4;
    if (GetKeyState(VK_MENU) & 0x8000) s |= 8;
    if (GetKeyState(VK_LBUTTON) & 0x8000) s |= 0x100;
    if (GetKeyState(VK_MBUTTON) & 0x8000) s |= 0x200;
    if (GetKeyState(VK_RBUTTON) & 0x8000) s |= 0x400;
    return s;
}
static void fill_pointer(unsigned char *e, int x, int y) {
    POINT p = {x, y};
    SET(e, 40, uint64_t, ROOT_WIN);
    SET(e, 56, uint64_t, (uint64_t)GetMessageTime());
    SET(e, 64, int, x);
    SET(e, 68, int, y);
    HWND h = x11_hwnd(*(uint64_t *)(e + 32));
    if (h) ClientToScreen(h, &p);
    SET(e, 72, int, p.x);
    SET(e, 76, int, p.y);
    SET(e, 80, unsigned, mod_state());
    SET(e, 88, int, 1);
}
static void push_configure(uint64_t id) {
    xwin *w = W(id);
    unsigned char *e = q_new(ConfigureNotify, id);
    SET(e, 40, uint64_t, id);
    SET(e, 48, int, w->x);
    SET(e, 52, int, w->y);
    SET(e, 56, int, w->w);
    SET(e, 60, int, w->h);
}

// Windows scancode (with extended bit 0x100) -> evdev keycode.
static int evdev_code(unsigned sc) {
    if (!(sc & 0x100)) return sc <= 88 ? (int)sc : 0;
    switch (sc & 0xff) {
    case 0x1c: return 96;  case 0x1d: return 97;  case 0x35: return 98;  case 0x37: return 99;
    case 0x38: return 100; case 0x47: return 102; case 0x48: return 103; case 0x49: return 104;
    case 0x4b: return 105; case 0x4d: return 106; case 0x4f: return 107; case 0x50: return 108;
    case 0x51: return 109; case 0x52: return 110; case 0x53: return 111; case 0x5b: return 125;
    case 0x5c: return 126; case 0x5d: return 127;
    }
    return 0;
}
// US-layout keysyms indexed by evdev code.
static const uint32_t g_keysym[128] = {
    0, 0xff1b, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0xff08, 0xff09,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0xff0d, 0xffe3, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0xffe1, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0xffe2, 0xffaa, 0xffe9, ' ', 0xffe5, 0xffbe, 0xffbf, 0xffc0, 0xffc1, 0xffc2,
    0xffc3, 0xffc4, 0xffc5, 0xffc6, 0xffc7, 0xff7f, 0xff14, 0xff95, 0xff97, 0xff9a, 0xffad, 0xff96, 0xff9d, 0xff98, 0xffab, 0xff9c,
    0xff99, 0xff9b, 0xff9e, 0xff9f, 0, 0, 0, 0xffc8, 0xffc9, 0, 0, 0, 0, 0, 0, 0,
    0xff8d, 0xffe4, 0xffaf, 0xff61, 0xffea, 0, 0xff50, 0xff52, 0xff55, 0xff51, 0xff53, 0xff57, 0xff54, 0xff56, 0xff63, 0xffff,
    0, 0, 0, 0, 0, 0, 0, 0xff13, 0, 0, 0, 0, 0, 0xffeb, 0xffec, 0xff67,
};
static uint32_t keysym_for(unsigned kc, int shift) {
    if (kc < 8 || kc >= 136) return 0;
    uint32_t k = g_keysym[kc - 8];
    if (shift && k >= 'a' && k <= 'z') k -= 32;
    if (shift) {
        static const char *from = "1234567890-=[];'`\\,./", *to = "!@#$%^&*()_+{}:\"~|<>?";
        const char *p = k < 128 && k ? strchr(from, (int)k) : NULL;
        if (p) k = (uint32_t)(unsigned char)to[p - from];
    }
    return k;
}

static DWORD WINAPI close_watchdog(void *arg) {
    (void)arg;
    Sleep(1500);
    raven_log("WM_CLOSE: game did not exit, ending the process\n");
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    uint64_t id = id_of(h);
    xwin *w = W(id);
    if (!w) return DefWindowProcA(h, msg, wp, lp);
    {   // flicker hunt: window-state messages that can drop the fullscreen/flip present path
        static int nl;
        if ((msg == WM_ACTIVATEAPP || msg == WM_ACTIVATE || msg == WM_DISPLAYCHANGE || msg == WM_WINDOWPOSCHANGED ||
             msg == WM_DWMCOMPOSITIONCHANGED || msg == WM_SHOWWINDOW || msg == WM_NCACTIVATE) && nl++ < 300) {
            if (msg == WM_WINDOWPOSCHANGED) {
                WINDOWPOS *p = (WINDOWPOS *)lp;
                raven_log("[flicker] win %llu WM_WINDOWPOSCHANGED %d,%d %dx%d flags 0x%x\n", (unsigned long long)id,
                          p->x, p->y, p->cx, p->cy, p->flags);
            } else
                raven_log("[flicker] win %llu msg 0x%x wp 0x%llx lp 0x%llx\n", (unsigned long long)id, msg,
                          (unsigned long long)wp, (unsigned long long)lp);
        }
    }
    switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        unsigned sc = (unsigned)((lp >> 16) & 0x1ff);
        int code = evdev_code(sc);
        if (!code) break;
        int up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
        unsigned char *e = q_new(up ? KeyRelease : KeyPress, id);
        fill_pointer(e, 0, 0);
        SET(e, 84, unsigned, (unsigned)code + 8);
        if (msg == WM_SYSKEYDOWN && wp == VK_F4 && !(lp & (1 << 30))) PostMessageA(h, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_MOUSEMOVE: {
        unsigned char *e = q_new(MotionNotify, id);
        fill_pointer(e, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
        int up = msg == WM_LBUTTONUP || msg == WM_MBUTTONUP || msg == WM_RBUTTONUP;
        int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 1 : (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) ? 2 : 3;
        unsigned char *e = q_new(up ? ButtonRelease : ButtonPress, id);
        fill_pointer(e, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        SET(e, 84, unsigned, (unsigned)b);
        if (up) ReleaseCapture(); else SetCapture(h);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int b = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 4 : 5;
        extern void ravenui_wheel(int dir);
        ravenui_wheel(b == 4 ? 1 : -1);  // F1 overlay scrolling (ignored while it is closed)
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        for (int t = ButtonPress; t <= ButtonRelease; t++) {
            unsigned char *e = q_new(t, id);
            fill_pointer(e, p.x, p.y);
            SET(e, 84, unsigned, (unsigned)b);
        }
        return 0;
    }
    case WM_SIZE:
        w->w = LOWORD(lp);
        w->h = HIWORD(lp);
        push_configure(id);
        return 0;
    case WM_MOVE:
        w->x = (short)LOWORD(lp);
        w->y = (short)HIWORD(lp);
        push_configure(id);
        return 0;
    case WM_SETFOCUS: g_focus = id; q_new(FocusIn, id); return 0;
    case WM_KILLFOCUS: q_new(FocusOut, id); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        unsigned char *e = q_new(Expose, id);
        SET(e, 48, int, w->w);
        SET(e, 52, int, w->h);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(w->cursor == 2 ? NULL : LoadCursor(NULL, IDC_ARROW)); return 1; }
        break;
    case WM_CLOSE: {
        unsigned char *e = q_new(ClientMessage, id);
        SET(e, 40, uint64_t, g_atom_protocols);
        SET(e, 48, int, 32);
        SET(e, 56, uint64_t, g_atom_delete);
        // The arcade game ignores WM_DELETE_WINDOW, so quit for it if it is still running shortly after.
        static LONG closing;
        if (!InterlockedExchange(&closing, 1)) {
            raven_log("WM_CLOSE: closing the game\n");
            HANDLE t = CreateThread(NULL, 0, close_watchdog, NULL, 0, NULL);
            if (t) CloseHandle(t); else close_watchdog(NULL);
        }
        return 0;
    }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}
int x11_events_pending(void) { pump(); return q_len(); }

// ---- display functions -------------------------------------------------------
static SV void *s_XOpenDisplay(const char *name) {
    init_display();
    g_atom_protocols = s_XInternAtom(NULL, "WM_PROTOCOLS", 0);
    g_atom_delete = s_XInternAtom(NULL, "WM_DELETE_WINDOW", 0);
    raven_log("XOpenDisplay(%s) -> %p (%dx%d)\n", name ? name : "NULL", (void *)g_dpy, g_scr_w, g_scr_h);
    return g_dpy;
}
static SV int s_zero(void) { return 0; }
static SV int s_one(void) { return 1; }
static SV void *s_null(void) { return NULL; }
static SV int s_XFree(void *p) { free(p); return 1; }
static SV const char *s_XDisplayName(const char *n) { return n ? n : ":0"; }
static SV const char *s_XDisplayString(void *d) { (void)d; return ":0"; }
static SV void *s_XSetErrorHandler(void *h) { (void)h; return NULL; }
static SV int s_XQueryExtension(void *d, const char *name, int *op, int *ev, int *err) {
    (void)d;
    raven_log("XQueryExtension(%s) -> no\n", name);
    if (op) *op = 0;
    if (ev) *ev = 0;
    if (err) *err = 0;
    return 0;
}
static SV int s_XDisplayKeycodes(void *d, int *mn, int *mx) { (void)d; *mn = 8; *mx = 255; return 1; }
static SV uint64_t s_XKeycodeToKeysym(void *d, unsigned char kc, int idx) { (void)d; return keysym_for(kc, idx & 1); }
static SV uint64_t s_XkbKeycodeToKeysym(void *d, unsigned char kc, int group, int level) { (void)d; (void)group; return keysym_for(kc, level & 1); }
static SV unsigned char s_XKeysymToKeycode(void *d, uint64_t ks) {
    (void)d;
    for (int i = 1; i < 128; i++) if (g_keysym[i] == ks) return (unsigned char)(i + 8);
    if (ks >= 'A' && ks <= 'Z') return s_XKeysymToKeycode(d, ks + 32);
    return 0;
}
static SV const char *s_XKeysymToString(uint64_t ks) {
    static char b[8][16];
    static int i;
    char *r = b[i++ & 7];
    if (ks >= 0x20 && ks < 0x7f) snprintf(r, 16, "%c", (int)ks);
    else snprintf(r, 16, "0x%llx", (unsigned long long)ks);
    return r;
}
static SV uint64_t s_XLookupKeysym(unsigned char *ev, int idx) { return keysym_for(*(unsigned *)(ev + 84), idx & 1); }
static SV int s_XLookupString(unsigned char *ev, char *buf, int n, uint64_t *ks, void *status) {
    (void)status;
    unsigned st = *(unsigned *)(ev + 80);
    uint32_t k = keysym_for(*(unsigned *)(ev + 84), (st & 1) != 0);
    if ((st & 2) && k >= 'a' && k <= 'z') k -= 32;
    if (ks) *ks = k;
    char c = 0;
    if (k >= 0x20 && k < 0x7f) c = (char)k;
    else if (k == 0xff0d) c = '\r';
    else if (k == 0xff08) c = 8;
    else if (k == 0xff09) c = 9;
    else if (k == 0xff1b) c = 27;
    if (!c || n < 1) return 0;
    buf[0] = c;
    if (n > 1) buf[1] = 0;
    return 1;
}
typedef struct { int max_keypermod; unsigned char *map; } xmodmap;
static SV xmodmap *s_XGetModifierMapping(void *d) {
    (void)d;
    xmodmap *m = calloc(1, sizeof *m);
    m->max_keypermod = 2;
    m->map = calloc(16, 1);
    unsigned char codes[16] = {50, 62, 66, 0, 37, 105, 64, 108, 77, 0, 0, 0, 133, 134, 0, 0};
    memcpy(m->map, codes, 16);  // shift, lock, control, mod1(alt), mod2(num), -, mod4(super)
    return m;
}
static SV int s_XFreeModifiermap(xmodmap *m) { if (m) { free(m->map); free(m); } return 0; }
static SV int s_XQueryKeymap(void *d, char *keys) { (void)d; memset(keys, 0, 32); return 1; }

// Visuals: XVisualInfo is 64 bytes.
static void fill_vinfo(unsigned char *vi) {
    memset(vi, 0, 64);
    SET(vi, 0, void *, g_visual);
    SET(vi, 8, uint64_t, VISUAL_ID);
    SET(vi, 16, int, 0);
    SET(vi, 20, int, 24);
    SET(vi, 24, int, 4);
    SET(vi, 32, uint64_t, 0xff0000);
    SET(vi, 40, uint64_t, 0x00ff00);
    SET(vi, 48, uint64_t, 0x0000ff);
    SET(vi, 56, int, 256);
    SET(vi, 60, int, 8);
}
void *x11_new_vinfo(void) { unsigned char *v = malloc(64); fill_vinfo(v); return v; }
static SV int s_XMatchVisualInfo(void *d, int scr, int depth, int cls, unsigned char *vi) {
    (void)d; (void)scr;
    if ((depth != 24 && depth != 32) || cls != 4) return 0;
    fill_vinfo(vi);
    return 1;
}
static SV void *s_XGetVisualInfo(void *d, long mask, void *tmpl, int *n) {
    (void)d; (void)mask; (void)tmpl;
    *n = 1;
    return x11_new_vinfo();
}
static SV uint64_t s_XVisualIDFromVisual(unsigned char *v) { return *(uint64_t *)(v + 8); }
static SV int *s_XListPixmapFormats(void *d, int *n) {
    (void)d;
    static const int f[9] = {1, 1, 32, 24, 32, 32, 32, 32, 32};
    int *r = malloc(sizeof f);
    memcpy(r, f, sizeof f);
    *n = 3;
    return r;
}
static SV uint64_t s_XCreateColormap(void *d, uint64_t w, void *v, int alloc) { (void)d; (void)w; (void)v; (void)alloc; return 2; }

// ---- window functions --------------------------------------------------------
static ATOM g_class;
static SV uint64_t s_XCreateWindow(void *d, uint64_t parent, int x, int y, unsigned w, unsigned h,
                                   unsigned bw, int depth, unsigned cls, void *vis, unsigned long mask,
                                   unsigned char *attrs) {
    (void)d; (void)bw; (void)depth; (void)cls; (void)vis;
    if (!g_class) {
        WNDCLASSA wc = {0};
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "RavenX11";
        g_class = RegisterClassA(&wc);
    }
    if (g_nwin >= 64) return 0;
    uint64_t id = (uint64_t)g_nwin++;
    xwin *xw = &g_win[id];
    memset(xw, 0, sizeof *xw);
    // XSetWindowAttributes: override_redirect at offset 88 (CWOverrideRedirect = 1<<9),
    // event_mask at 72 (CWEventMask = 1<<11).
    if (attrs && (mask & (1 << 9))) xw->override = *(int *)(attrs + 88);
    if (attrs && (mask & (1 << 11))) xw->mask = *(long long *)(attrs + 72);
    DWORD style = xw->override ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT r = {x, y, x + (int)w, y + (int)h};
    AdjustWindowRect(&r, style, FALSE);
    xw->hwnd = CreateWindowExA(0, "RavenX11", "Halo: Fireteam Raven", style | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                               xw->override ? x : CW_USEDEFAULT, xw->override ? y : CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top,
                               (parent > ROOT_WIN && W(parent)) ? W(parent)->hwnd : NULL, NULL,
                               GetModuleHandleA(NULL), NULL);
    xw->x = x; xw->y = y; xw->w = (int)w; xw->h = (int)h; xw->req_w = (int)w; xw->req_h = (int)h;
    raven_log("XCreateWindow(%dx%d at %d,%d, override=%d) -> %llu hwnd=%p\n", w, h, x, y, xw->override,
              (unsigned long long)id, (void *)xw->hwnd);
    return id;
}
static SV int s_XDestroyWindow(void *d, uint64_t id) {
    (void)d;
    xwin *w = W(id);
    if (w && w->hwnd) { DestroyWindow(w->hwnd); w->hwnd = NULL; }
    return 1;
}
static void set_fullscreen(xwin *w, int on) {
    if (!w || !w->hwnd || w->fullscreen == on) return;
    HMONITOR mon = MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoA(mon, &mi);
    if (on) {
        w->saved_style = (DWORD)GetWindowLongA(w->hwnd, GWL_STYLE);
        GetWindowRect(w->hwnd, &w->saved);
        SetWindowLongA(w->hwnd, GWL_STYLE, (LONG)((w->saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP));
        RECT r = mi.rcMonitor;
        // Default: one pixel taller than the monitor (top row off-screen), so Windows always composes
        // the window. An exact-monitor window flickered black at scene changes (independent-flip /
        // G-SYNC switches; user confirmed fixed 2026-11-08). RAVEN_COMPOSE=0 = exact size.
        const char *cp = raven_env("RAVEN_COMPOSE");
        if (!(cp && *cp == '0')) { r.top -= 1; raven_log("RAVEN_COMPOSE: window 1 px taller than the monitor\n"); }
        SetWindowPos(w->hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongA(w->hwnd, GWL_STYLE, (LONG)w->saved_style);
        RECT r = w->saved;
        SetWindowPos(w->hwnd, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER);
    }
    w->fullscreen = on;
    RECT cr;
    GetClientRect(w->hwnd, &cr);
    raven_log("fullscreen %s: monitor %ldx%ld, client %ldx%ld, window dpi %u, awareness %d\n", on ? "on" : "off",
              mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, cr.right, cr.bottom,
              GetDpiForWindow(w->hwnd), (int)GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()));
}
static SV int s_XMapWindow(void *d, uint64_t id) {
    (void)d;
    xwin *w = W(id);
    if (!w) return 1;
    // RAVEN_WINDOWED=1 keeps a normal window; otherwise go fullscreen on map,
    // like the arcade cabinet.
    const char *wm = raven_env("RAVEN_WINDOWED");
    raven_log("XMapWindow(%llu) %dx%d windowed=%s\n", (unsigned long long)id, w->w, w->h, wm ? wm : "(unset)");
    if (!w->override && !(wm && *wm == '1')) set_fullscreen(w, 1);
    ShowWindow(w->hwnd, SW_SHOW);
    SetForegroundWindow(w->hwnd);
    SetFocus(w->hwnd);
    w->mapped = 1;
    unsigned char *e = q_new(MapNotify, id);
    SET(e, 40, uint64_t, id);
    pump();
    return 1;
}
static SV int s_XUnmapWindow(void *d, uint64_t id) {
    (void)d;
    xwin *w = W(id);
    if (!w) return 1;
    ShowWindow(w->hwnd, SW_HIDE);
    w->mapped = 0;
    unsigned char *e = q_new(UnmapNotify, id);
    SET(e, 40, uint64_t, id);
    return 1;
}
// SDL's legacy fullscreen maps a screen-sized override window, reparents the game window into it
// and waits for the game window's MapNotify (X unmaps + remaps a mapped window on reparent).
// We keep the game window as the top-level fullscreen window and hide the cover window instead.
static SV int s_XReparentWindow(void *d, uint64_t id, uint64_t parent, int x, int y) {
    (void)d;
    xwin *w = W(id), *p = W(parent);
    if (!w) return 1;
    raven_log("XReparentWindow(%llu -> %llu at %d,%d)\n", (unsigned long long)id, (unsigned long long)parent, x, y);
    if (p && p->override) ShowWindow(p->hwnd, SW_HIDE);
    unsigned char *e;
    if (w->mapped) { e = q_new(UnmapNotify, id); SET(e, 40, uint64_t, id); }
    e = q_new(21, id);  // ReparentNotify: event, window, parent, x, y
    SET(e, 40, uint64_t, id);
    SET(e, 48, uint64_t, parent);
    SET(e, 56, int, x);
    SET(e, 60, int, y);
    if (w->mapped) {
        SetWindowPos(w->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(w->hwnd);
        SetFocus(w->hwnd);
        e = q_new(MapNotify, id);
        SET(e, 40, uint64_t, id);
    }
    return 1;
}
static SV int s_XIconifyWindow(void *d, uint64_t id, int s) { (void)d; (void)s; xwin *w = W(id); if (w) ShowWindow(w->hwnd, SW_MINIMIZE); return 1; }
static SV int s_XRaiseWindow(void *d, uint64_t id) { (void)d; xwin *w = W(id); if (w) SetWindowPos(w->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE); return 1; }
static SV int s_XMoveWindow(void *d, uint64_t id, int x, int y) {
    (void)d;
    xwin *w = W(id);
    if (w && !w->fullscreen) SetWindowPos(w->hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    return 1;
}
static SV int s_XResizeWindow(void *d, uint64_t id, unsigned cw, unsigned ch) {
    (void)d;
    xwin *w = W(id);
    if (!w) return 1;
    raven_log("XResizeWindow(%u x %u)\n", cw, ch);
    if (w->fullscreen) return 1;
    RECT r = {0, 0, (int)cw, (int)ch};
    AdjustWindowRect(&r, (DWORD)GetWindowLongA(w->hwnd, GWL_STYLE), FALSE);
    SetWindowPos(w->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    return 1;
}
static SV int s_XStoreName(void *d, uint64_t id, const char *n) { (void)d; xwin *w = W(id); if (w && n) SetWindowTextA(w->hwnd, n); return 1; }
static SV int s_XGetWindowAttributes(void *d, uint64_t id, unsigned char *a) {
    (void)d;
    memset(a, 0, 136);
    xwin *w = W(id);
    SET(a, 0, int, w ? w->x : 0);
    SET(a, 4, int, w ? w->y : 0);
    SET(a, 8, int, w ? w->w : g_scr_w);
    SET(a, 12, int, w ? w->h : g_scr_h);
    SET(a, 20, int, 24);
    SET(a, 24, void *, g_visual);
    SET(a, 32, uint64_t, ROOT_WIN);
    SET(a, 40, int, 1);  // InputOutput
    SET(a, 80, uint64_t, 2);
    SET(a, 92, int, w ? (w->mapped ? 2 : 0) : 2);  // IsViewable / IsUnmapped
    SET(a, 104, long long, w ? w->mask : 0);
    SET(a, 120, int, w ? w->override : 0);
    SET(a, 128, void *, g_screen);
    return 1;
}
static SV int s_XTranslateCoordinates(void *d, uint64_t src, uint64_t dst, int x, int y, int *ox, int *oy, uint64_t *child) {
    (void)d;
    POINT p = {x, y};
    if (W(src)) ClientToScreen(W(src)->hwnd, &p);
    if (W(dst)) ScreenToClient(W(dst)->hwnd, &p);
    *ox = p.x;
    *oy = p.y;
    if (child) *child = 0;
    return 1;
}
static SV int s_XQueryPointer(void *d, uint64_t id, uint64_t *root, uint64_t *child, int *rx, int *ry,
                              int *wx, int *wy, unsigned *mask) {
    (void)d;
    POINT p;
    GetCursorPos(&p);
    *rx = p.x; *ry = p.y;
    if (W(id)) ScreenToClient(W(id)->hwnd, &p);
    *wx = p.x; *wy = p.y;
    if (root) *root = ROOT_WIN;
    if (child) *child = 0;
    if (mask) *mask = mod_state();
    return 1;
}
static SV int s_XWarpPointer(void *d, uint64_t src, uint64_t dst, int sx, int sy, unsigned sw, unsigned sh, int x, int y) {
    (void)d; (void)src; (void)sx; (void)sy; (void)sw; (void)sh;
    POINT p = {x, y};
    if (W(dst)) ClientToScreen(W(dst)->hwnd, &p);
    SetCursorPos(p.x, p.y);
    return 1;
}
static SV int s_XGetInputFocus(void *d, uint64_t *w, int *rev) { (void)d; *w = g_focus ? g_focus : ROOT_WIN; *rev = 0; return 1; }
static SV int s_XSetInputFocus(void *d, uint64_t id, int rev, uint64_t t) { (void)d; (void)rev; (void)t; if (W(id)) SetFocus(W(id)->hwnd); return 1; }
static SV int s_XSelectInput(void *d, uint64_t id, long long m) { (void)d; if (W(id)) W(id)->mask = m; return 1; }
static SV int s_XGetWindowProperty(void *d, uint64_t w, uint64_t prop, long long off, long long len, int del,
                                   uint64_t req, uint64_t *atype, int *afmt, unsigned long long *n,
                                   unsigned long long *after, unsigned char **ret) {
    (void)d; (void)w; (void)prop; (void)off; (void)len; (void)del; (void)req;
    *atype = 0; *afmt = 0; *n = 0; *after = 0; *ret = NULL;
    return 0;
}
static SV int s_XChangeProperty(void *d, uint64_t id, uint64_t prop, uint64_t type, int fmt, int mode,
                                const unsigned char *data, int n) {
    (void)d; (void)type; (void)mode;
    const char *pn = atom_name(prop);
    if (fmt == 8 && W(id) && (!strcmp(pn, "_NET_WM_NAME") || prop == 39)) {
        char t[256];
        snprintf(t, sizeof t, "%.*s", n, (const char *)data);
        SetWindowTextA(W(id)->hwnd, t);
    }
    return 1;
}
// XSendEvent: SDL asks the window manager for fullscreen with a
// _NET_WM_STATE client message (data.l[0] = 0 remove / 1 add / 2 toggle).
static SV int s_XSendEvent(void *d, uint64_t dst, int prop, long long mask, unsigned char *ev) {
    (void)d; (void)dst; (void)prop; (void)mask;
    if (*(int *)ev == ClientMessage && !strcmp(atom_name(*(uint64_t *)(ev + 40)), "_NET_WM_STATE")) {
        uint64_t *l = (uint64_t *)(ev + 56);
        int fs = !strcmp(atom_name(l[1]), "_NET_WM_STATE_FULLSCREEN") || !strcmp(atom_name(l[2]), "_NET_WM_STATE_FULLSCREEN");
        xwin *w = W(*(uint64_t *)(ev + 32));
        if (fs && w) set_fullscreen(w, l[0] == 2 ? !w->fullscreen : (int)l[0]);
    }
    return 1;
}
static SV uint64_t s_XCreatePixmapCursor(void) { return 2; }  // SDL's blank cursor
static SV uint64_t s_XCreateFontCursor(void *d, unsigned shape) { (void)d; (void)shape; return 3; }
static SV int s_XDefineCursor(void *d, uint64_t id, uint64_t c) {
    (void)d;
    if (W(id)) W(id)->cursor = c;
    if (c == 2) SetCursor(NULL);
    return 1;
}
static SV int s_XUndefineCursor(void *d, uint64_t id) { (void)d; if (W(id)) W(id)->cursor = 0; return 1; }
static SV void *s_XAllocSizeHints(void) { return calloc(1, 80); }
static SV void *s_XAllocWMHints(void) { return calloc(1, 56); }
static SV void *s_XAllocClassHint(void) { return calloc(1, 16); }
// XTextProperty {value, encoding, format, nitems}
static SV int s_XStringListToTextProperty(char **list, int n, unsigned char *tp) {
    size_t len = 0;
    for (int i = 0; i < n; i++) len += strlen(list[i]) + 1;
    char *v = calloc(1, len + 1), *p = v;
    for (int i = 0; i < n; i++) { strcpy(p, list[i]); p += strlen(list[i]) + 1; }
    SET(tp, 0, char *, v);
    SET(tp, 8, uint64_t, 31);
    SET(tp, 16, int, 8);
    SET(tp, 24, uint64_t, len ? len - 1 : 0);
    return 1;
}
static SV int s_Xutf8TextListToTextProperty(void *d, char **list, int n, int style, unsigned char *tp) {
    (void)d; (void)style;
    s_XStringListToTextProperty(list, n, tp);
    return 0;  // Success
}
static SV void s_XSetTextProperty(void *d, uint64_t id, unsigned char *tp, uint64_t prop) {
    s_XChangeProperty(d, id, prop, 0, 8, 0, *(unsigned char **)tp, (int)*(uint64_t *)(tp + 24));
}
static SV void s_XSetWMProperties(void *d, uint64_t id, unsigned char *name, unsigned char *icon, char **argv, int argc,
                                  void *nh, void *wmh, void *ch) {
    (void)icon; (void)argv; (void)argc; (void)nh; (void)wmh; (void)ch;
    if (name) s_XSetTextProperty(d, id, name, 39);
}
static SV int s_XGrabSuccess(void) { return 0; }  // GrabSuccess
static SV void *s_XSynchronize(void) { return NULL; }
static SV int s_XInitThreads(void) { return 1; }
static SV int s_XGetPointerControl(void *d, int *n, int *dn, int *t) { (void)d; *n = 1; *dn = 1; *t = 0; return 1; }

// ---- events ------------------------------------------------------------------
typedef SV int (*xpred)(void *, unsigned char *, void *);
static SV int s_XPending(void *d) { (void)d; return x11_events_pending(); }
static SV int s_XEventsQueued(void *d, int mode) { (void)d; if (mode) pump(); return q_len(); }
static void wait_event(void) {
    while (!x11_events_pending()) MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
}
static SV int s_XNextEvent(void *d, unsigned char *ev) {
    (void)d;
    wait_event();
    memcpy(ev, g_q[g_qh].b, 192);
    g_qh = (g_qh + 1) % 1024;
    return 0;
}
static SV int s_XPeekEvent(void *d, unsigned char *ev) { (void)d; wait_event(); memcpy(ev, g_q[g_qh].b, 192); return 0; }
// Remove queue entry i (relative to head), keeping order.
static void q_remove(int i) {
    for (int k = i; k > 0; k--) g_q[(g_qh + k) % 1024] = g_q[(g_qh + k - 1) % 1024];
    g_qh = (g_qh + 1) % 1024;
}
static int find_pred(xpred p, void *arg, unsigned char *ev) {
    int n = x11_events_pending();
    for (int i = 0; i < n; i++) {
        unsigned char *e = g_q[(g_qh + i) % 1024].b;
        if (p(g_dpy, e, arg)) { memcpy(ev, e, 192); q_remove(i); return 1; }
    }
    return 0;
}
static SV int s_XCheckIfEvent(void *d, unsigned char *ev, xpred p, void *arg) { (void)d; return find_pred(p, arg, ev); }
static SV int s_XIfEvent(void *d, unsigned char *ev, xpred p, void *arg) {
    (void)d;
    for (int tries = 0; !find_pred(p, arg, ev); tries++) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
        if (tries == 300) {
            // SDL's predicates get a pointer to the Window they wait for.
            raven_log("XIfEvent: still waiting after 3 s (pred %p, arg window %llu, %d queued:",
                      (void *)p, arg ? (unsigned long long)*(uint64_t *)arg : 0ULL, x11_events_pending());
            for (int i = 0; i < x11_events_pending() && i < 16; i++) {
                unsigned char *e = g_q[(g_qh + i) % 1024].b;
                raven_log(" %d/w%llu", *(int *)e, (unsigned long long)*(uint64_t *)(e + 32));
            }
            raven_log(")\n");
        }
    }
    return 0;
}
static SV int s_XWindowEvent(void *d, uint64_t id, long long mask, unsigned char *ev) {
    (void)d; (void)mask;
    for (;;) {
        int n = x11_events_pending();
        for (int i = 0; i < n; i++) {
            unsigned char *e = g_q[(g_qh + i) % 1024].b;
            if (*(uint64_t *)(e + 32) == id) { memcpy(ev, e, 192); q_remove(i); return 0; }
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
    }
}

// select(): the fake X connection fd is readable when events are queued.
typedef struct { long long sec, usec; } lx_timeval;
static SV int s_select(int nfds, uint64_t *rd, uint64_t *wr, uint64_t *ex, lx_timeval *tv) {
    int want_x = rd && nfds > DPY_FD && (rd[DPY_FD / 64] >> (DPY_FD % 64) & 1);
    DWORD ms = tv ? (DWORD)(tv->sec * 1000 + tv->usec / 1000) : INFINITE;
    int words = (nfds + 63) / 64;
    if (rd) memset(rd, 0, (size_t)words * 8);
    if (wr) memset(wr, 0, (size_t)words * 8);
    if (ex) memset(ex, 0, (size_t)words * 8);
    if (want_x) {
        DWORD start = GetTickCount();
        for (;;) {
            if (x11_events_pending()) { rd[DPY_FD / 64] |= 1ULL << (DPY_FD % 64); return 1; }
            DWORD el = GetTickCount() - start;
            if (ms != INFINITE && el >= ms) return 0;
            MsgWaitForMultipleObjects(0, NULL, FALSE, ms == INFINITE ? 10 : (ms - el < 10 ? ms - el : 10), QS_ALLINPUT);
        }
    }
    if (ms) Sleep(ms == INFINITE ? 1000 : ms);
    return 0;
}

const shim_entry shims_x11[] = {
    {"XOpenDisplay", s_XOpenDisplay}, {"XCloseDisplay", s_zero}, {"XFree", s_XFree},
    {"XDisplayName", s_XDisplayName}, {"XDisplayString", s_XDisplayString},
    {"XSetErrorHandler", s_XSetErrorHandler}, {"XSetIOErrorHandler", s_XSetErrorHandler},
    {"XSynchronize", s_XSynchronize}, {"XInitThreads", s_XInitThreads},
    {"XQueryExtension", s_XQueryExtension}, {"XMissingExtension", s_zero},
    {"XInternAtom", s_XInternAtom}, {"XGetAtomName", s_XGetAtomName},
    {"XDisplayKeycodes", s_XDisplayKeycodes}, {"XKeycodeToKeysym", s_XKeycodeToKeysym},
    {"XkbKeycodeToKeysym", s_XkbKeycodeToKeysym}, {"XKeysymToKeycode", s_XKeysymToKeycode},
    {"XKeysymToString", s_XKeysymToString}, {"XLookupKeysym", s_XLookupKeysym},
    {"XLookupString", s_XLookupString}, {"XGetModifierMapping", s_XGetModifierMapping},
    {"XFreeModifiermap", s_XFreeModifiermap}, {"XQueryKeymap", s_XQueryKeymap},
    {"XMatchVisualInfo", s_XMatchVisualInfo}, {"XGetVisualInfo", s_XGetVisualInfo},
    {"XVisualIDFromVisual", s_XVisualIDFromVisual}, {"XListPixmapFormats", s_XListPixmapFormats},
    {"XCreateColormap", s_XCreateColormap}, {"XFreeColormap", s_zero},
    {"XInstallColormap", s_zero}, {"XUninstallColormap", s_zero}, {"XStoreColors", s_zero},
    {"XCreateWindow", s_XCreateWindow}, {"XDestroyWindow", s_XDestroyWindow},
    {"XMapWindow", s_XMapWindow}, {"XMapRaised", s_XMapWindow}, {"XReparentWindow", s_XReparentWindow}, {"XUnmapWindow", s_XUnmapWindow},
    {"XWithdrawWindow", s_XUnmapWindow}, {"XIconifyWindow", s_XIconifyWindow},
    {"XRaiseWindow", s_XRaiseWindow}, {"XMoveWindow", s_XMoveWindow}, {"XResizeWindow", s_XResizeWindow},
    {"XStoreName", s_XStoreName}, {"XGetWindowAttributes", s_XGetWindowAttributes},
    {"XTranslateCoordinates", s_XTranslateCoordinates}, {"XQueryPointer", s_XQueryPointer},
    {"XWarpPointer", s_XWarpPointer}, {"XGetInputFocus", s_XGetInputFocus},
    {"XSetInputFocus", s_XSetInputFocus}, {"XSelectInput", s_XSelectInput},
    {"XGetWindowProperty", s_XGetWindowProperty}, {"XChangeProperty", s_XChangeProperty},
    {"XDeleteProperty", s_one}, {"XSendEvent", s_XSendEvent},
    {"XCreatePixmapCursor", s_XCreatePixmapCursor}, {"XCreateFontCursor", s_XCreateFontCursor},
    {"XCreateBitmapFromData", s_XCreatePixmapCursor}, {"XFreePixmap", s_zero}, {"XFreeCursor", s_zero},
    {"XDefineCursor", s_XDefineCursor}, {"XUndefineCursor", s_XUndefineCursor},
    {"XAllocSizeHints", s_XAllocSizeHints}, {"XAllocWMHints", s_XAllocWMHints},
    {"XAllocClassHint", s_XAllocClassHint}, {"XStringListToTextProperty", s_XStringListToTextProperty},
    {"Xutf8TextListToTextProperty", s_Xutf8TextListToTextProperty},
    {"XSetTextProperty", s_XSetTextProperty}, {"XSetWMProperties", s_XSetWMProperties},
    {"XSetWMNormalHints", s_one}, {"XSetWMProtocols", s_one}, {"XSetTransientForHint", s_one},
    {"XGetWMHints", s_null}, {"XGetWMNormalHints", s_zero}, {"XSetWindowBackground", s_one},
    {"XClearWindow", s_one}, {"XShapeCombineMask", s_zero},
    {"XGrabPointer", s_XGrabSuccess}, {"XGrabKeyboard", s_XGrabSuccess},
    {"XUngrabPointer", s_one}, {"XUngrabKeyboard", s_one}, {"XGrabServer", s_one}, {"XUngrabServer", s_one},
    {"XFlush", s_one}, {"XSync", s_one}, {"_XFlush", s_one}, {"XResetScreenSaver", s_one},
    {"XGetPointerControl", s_XGetPointerControl}, {"XChangePointerControl", s_one},
    {"XAutoRepeatOn", s_one}, {"XAutoRepeatOff", s_one}, {"XFreeStringList", s_zero},
    {"XGetSelectionOwner", s_zero}, {"XSetSelectionOwner", s_one}, {"XConvertSelection", s_one},
    {"XOpenIM", s_null}, {"XCloseIM", s_zero}, {"XCreateIC", s_null}, {"XDestroyIC", s_zero},
    {"XSetICFocus", s_zero}, {"XUnsetICFocus", s_zero}, {"XFilterEvent", s_zero},
    {"XGetEventData", s_zero}, {"XFreeEventData", s_zero},
    {"XShmQueryExtension", s_zero},
    {"XRRQueryVersion", s_zero}, {"XF86VidModeQueryExtension", s_zero}, {"XF86VidModeQueryVersion", s_zero},
    {"XPending", s_XPending}, {"XEventsQueued", s_XEventsQueued}, {"XNextEvent", s_XNextEvent},
    {"XPeekEvent", s_XPeekEvent}, {"XCheckIfEvent", s_XCheckIfEvent}, {"XIfEvent", s_XIfEvent},
    {"XWindowEvent", s_XWindowEvent},
    {"select", s_select},
    {NULL, NULL},
};
