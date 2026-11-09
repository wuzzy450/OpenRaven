// ravenui.c - in-game settings overlay (F1). Built into ravengfx.so (gcc ravengfx.c ravenui.c).
//
// Keys: read on our own X connection with XQueryKeymap (works without touching the game's event
// loop, also under WSLg). F1 (or RAVENGFX_UIKEY=F2 etc.) opens/closes, Up/Down pick, Left/Right
// change, Tab / Shift+Tab switch pages, Enter saves to graphics.ini, Esc closes without saving.
// Mouse: our own pointer is drawn (the game hides the system one); click tabs, < > or a value
// (right-click goes back), wheel or drag the scrollbar to scroll.
// Drawing: the panel is rendered on the CPU into an RGBA buffer, uploaded as one texture and drawn
// as one quad with a GLSL 330 shader just before the frame is presented. All GL state we touch is
// saved and restored. Plain OpenGL 3.3, no vendor extensions.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <X11/Xlib.h>
#include "ravenui_font.h"

// from ravengfx.c
const char *ravengfx_get(const char *key);
void ravengfx_set(const char *key, const char *val);
extern const char *ravengfx_conf_path;
extern float ravengfx_fps;

// ---------------- settings list ----------------
enum { T_CHOICE, T_NUM };
typedef struct {
    const char *section, *label, *key;   // key in graphics.ini
    int type, live;                      // live: takes effect now; else on next launch
    const char *choices;                 // T_CHOICE: "v1|v2|..."  (shown as-is; 0/1 shown as Off/On)
    float lo, hi, step;                  // T_NUM
    const char *def;                     // value when graphics.ini doesn't set it
    const char *help;
    char val[32];
} item_t;

static item_t items[] = {
 {"Display","Resolution","resolution",T_CHOICE,0,"native|1280x720|1920x1080|2560x1440|3840x2160|1440x1080|1600x1200|1920x1200|2560x1600|2560x1080|3440x1440|3840x1080|5120x1440",0,0,0,"1920x1080","Screen size. native = desktop size"},
 {"Display","Render scale","RENDER_SCALE",T_CHOICE,0,"0.5|0.75|1.0|1.25|1.5|2.0",0,0,0,"1.0","Relative to screen, any aspect. Above 1 = supersampling. Ignored when FSR 2 is on"},
 {"Display","VSync","VSYNC",T_CHOICE,1,"0|1|adaptive",0,0,0,"0","Sync to the monitor: no tearing. adaptive = only tears when a frame is late"},
 {"Display","FPS cap","FPS_CAP",T_CHOICE,1,"0|30|40|50|60|90|120|144|165|240",0,0,0,"0","0 = no cap (follows the monitor). Game speed is unaffected"},
 {"Display","Aspect fix (Hor+)","HOR_PLUS",T_CHOICE,1,"0|1",0,0,0,"1","Correct proportions on any screen shape"},
 {"Display","Field of view","FOV_SCALE",T_NUM,1,NULL,0.8f,1.4f,0.05f,"1","1 = arcade view. Higher = wider"},
 {"Display","Show FPS","show_fps",T_CHOICE,1,"0|1",0,0,0,"0","FPS counter in the corner"},
 {"Upscaling","Upscaler","UPSCALER",T_CHOICE,0,"off|fsr2",0,0,0,"off","FSR 2: temporal upscaling + AA (any GPU, GL 4.6)"},
 {"Upscaling","FSR 2 mode","FSR_QUALITY",T_CHOICE,0,"native|quality|balanced|performance|ultra",0,0,0,"quality","Render size: native 100%, quality 67%, balanced 59%, performance 50%, ultra 33%"},
 {"Upscaling","FSR 2 sharpness","FSR_SHARPNESS",T_NUM,1,NULL,0,1,0.05f,"0.3","0 = off. Live"},
 {"Frame generation","Frame generation","FRAME_GEN",T_CHOICE,1,"off|2x|3x|4x",0,0,0,"off","Fills refresh slots the game misses (never lowers real FPS). Max frames per real frame"},
 {"Frame generation","Screen refresh","REFRESH_RATE",T_CHOICE,0,"auto|60|75|100|120|144|165|170|240|360",0,0,0,"auto","auto = read from the display. Set it if auto is wrong"},
 {"Frame generation","Mode","FG_MODE",T_CHOICE,1,"interpolate|extrapolate",0,0,0,"interpolate","Interpolate: best image, ~1 frame more lag. Extrapolate: no extra lag"},
 {"Frame generation","Show generated frames","FG_DEBUG",T_CHOICE,1,"0|1|2",0,0,0,"0","1 = magenta border on generated frames, 2 = also tint guessed areas"},
 {"Upscaling","Neural detail","NEURAL",T_CHOICE,1,"0|1",0,0,0,"0","Learned pass on top of FSR 2: restores texture and face detail, keeps blacks/highlights"},
 {"Upscaling","Neural strength","NEURAL_STRENGTH",T_NUM,1,NULL,0,1.5f,0.1f,"1","Live"},
 {"Upscaling","Neural stability","NEURAL_STABLE",T_NUM,1,NULL,0,0.9f,0.1f,"0.6","Higher = less shimmer on moving detail"},
 {"Upscaling","Neural pattern guard","NEURAL_PROTECT",T_NUM,1,NULL,0,1.0f,0.1f,"","Grates/vents: higher = keeps holes clean. Empty = what the network was trained with"},
 {"Upscaling","Neural backend","NEURAL_BACKEND",T_CHOICE,0,"auto|matrix|plain",0,0,0,"auto","Vulkan. auto = matrix cores if the GPU has them (RTX, RX 7000+), else plain (any GPU)"},
 {"Upscaling","Neural accumulate","NEURAL_ACCUMULATE",T_CHOICE,0,"auto|fp16|fp32",0,0,0,"auto","Matrix cores. auto = quick speed test at start picks the faster per layer"},
 {"Anti-aliasing","SMAA","ENABLE_SMAA",T_CHOICE,0,"0|1",0,0,0,"0","Sharp edge smoothing"},
 {"Anti-aliasing","FXAA","ENABLE_FXAA",T_CHOICE,0,"0|1",0,0,0,"0","Blurs the image; not recommended"},
 {"Anti-aliasing","TAA","ENABLE_TAA",T_CHOICE,0,"0|1",0,0,0,"0","Blurs the image; not recommended"},
 {"Textures","Anisotropic filtering","ANISOTROPY_LEVEL",T_CHOICE,0,"1|2|4|8|16",0,0,0,"8","Sharper textures at angles"},
 {"Shadows","Shadow map size","SHADOW_SIZE",T_CHOICE,0,"1024|2048|4096|8192",0,0,0,"2048",NULL},
 {"Shadows","Static shadow size","STATIC_SHADOW_SIZE",T_CHOICE,0,"1024|2048|4096|8192",0,0,0,"2048",NULL},
 {"Shadows","Shadow cascades","SHADOW_SPLITS",T_CHOICE,0,"1|2|3|4",0,0,0,"2","More = sharper far shadows"},
 {"Shadows","Stable shadows","ENABLE_STABLE_SHADOW",T_CHOICE,0,"0|1",0,0,0,"0","Less shimmering"},
 {"Effects","SSAO","ENABLE_SSAO",T_CHOICE,0,"0|1",0,0,0,"1","Ambient occlusion"},
 {"Effects","Reflections resolution","SSR_BUFFER_SCALE",T_CHOICE,0,"0.25|0.5|0.75|1.0",0,0,0,"0.5",NULL},
// Enhanced bloom removed from the menu for now: engine bug turns some scenes white (ENHANCED_BLOOM stays 0 in graphics.ini)
 {"Effects","Bloom strength","bloomscale",T_NUM,1,NULL,0,3,0.1f,"1","Live"},
 {"Effects","Glow strength","glowscale",T_NUM,1,NULL,0,3,0.1f,"1","Live"},
 {"Effects","Lens flare","ENABLE_LENS_FLARE",T_CHOICE,0,"0|1",0,0,0,"0",NULL},
 {"Audio","Volume boost","volume",T_CHOICE,1,"100%|125%|150%|200%|250%|300%",0,0,0,"100%","Audio stream volume"},
 {"Input","Aim mode","aim",T_CHOICE,1,"cursor|abs",0,0,0,"cursor","cursor: stick moves aim. abs: stick = aim"},
 {"Input","Stick speed","stick_speed",T_NUM,1,NULL,0.1f,4,0.05f,"0.25","Screen widths per second (cursor mode)"},
 {"Overlay","Overlay size","ui_scale",T_CHOICE,1,"0.75|0.9|1.0|1.1|1.25|1.5|2.0",0,0,0,"1.0","Size of this menu. It always shrinks to fit the screen"},
 {"Cabinet","Free play","free_play",T_CHOICE,0,"0|1",0,0,0,"1",NULL},
 {"Cabinet","Skip boot checks","skip_boot",T_CHOICE,0,"0|1",0,0,0,"1","Skip file test + gun calibration"},
};
#define NITEMS ((int)(sizeof items / sizeof *items))

static volatile int ui_open, ui_dirty, ui_restart;
static char ui_msg[96];
static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static int show_fps;
static float ui_user_scale = 1.0f;   // "Overlay size" setting

// ---------------- tabs ----------------
// Each tab shows the sections it lists, in that order. To add a tab, add a line here.
static const struct { const char *name, *sections; } tabs[] = {
    {"Display",       "Display|Textures"},
    {"Upscaling",     "Upscaling|Frame generation|Anti-aliasing"},
    {"Quality",       "Shadows|Effects"},
    {"Audio & Input", "Audio|Input|Overlay"},
    {"Cabinet",       "Cabinet"},
};
#define NTABS ((int)(sizeof tabs / sizeof *tabs))

typedef struct { int item; const char *hdr; } row_t;   // item >= 0, or -1 for a section header
static row_t rows[NITEMS * 2];
static int nrows, ui_tab, ui_sel = -1, ui_scroll;     // ui_sel = row index; ui_scroll in layout units

// Layout, in units of a 1080p screen (scaled to the real screen when drawn).
enum { PW = 980, PH = 660, RH = 40, TAB_Y = 64, TAB_H = 44, CT_X = 24, CT_Y = 124, SB_W = 10,
       VAL_X = 500, VAL_W = 300 };
#define CT_H (PH - 136 - CT_Y)
#define SB_X (PW - 24 - SB_W)

static void build_rows(void)
{
    char buf[256], *sp; snprintf(buf, sizeof buf, "%s", tabs[ui_tab].sections);
    nrows = 0;
    for (char *s = strtok_r(buf, "|", &sp); s; s = strtok_r(NULL, "|", &sp)) {
        int hdr = 0;
        for (int i = 0; i < NITEMS; i++) if (!strcmp(items[i].section, s)) {
            if (!hdr++) rows[nrows++] = (row_t){-1, items[i].section};
            rows[nrows++] = (row_t){i, NULL};
        }
    }
}
static int max_scroll(void) { int m = nrows * RH - CT_H; return m > 0 ? m : 0; }
static void clamp_scroll(void) { if (ui_scroll > max_scroll()) ui_scroll = max_scroll(); if (ui_scroll < 0) ui_scroll = 0; }
static void reveal(int r)
{
    int top = (r > 0 && rows[r - 1].item < 0 ? r - 1 : r) * RH;   // keep the section header in view too
    if (top < ui_scroll) ui_scroll = top;
    if ((r + 1) * RH > ui_scroll + CT_H) ui_scroll = (r + 1) * RH - CT_H;
    clamp_scroll();
}
static void move_sel(int dir)
{
    if (!nrows) return;
    int r = ui_sel;
    for (int n = 0; n < nrows; n++) { r = ((r + dir) % nrows + nrows) % nrows; if (rows[r].item >= 0) break; }
    ui_sel = r; reveal(r);
}
static void set_tab(int t) { ui_tab = (t % NTABS + NTABS) % NTABS; build_rows(); ui_scroll = 0; ui_sel = -1; move_sel(1); }

static void apply_live(item_t *it);

static void load_values(void)
{
    for (int i = 0; i < NITEMS; i++) {
        const char *v = ravengfx_get(items[i].key);
        snprintf(items[i].val, sizeof items[i].val, "%s", v ? v : items[i].def);
        if (items[i].live && v) apply_live(&items[i]);   // only what graphics.ini sets
    }
}

static void change(item_t *it, int dir)
{
    if (it->type == T_NUM) {
        float v = strtof(it->val, NULL) + dir * it->step;
        if (v < it->lo) v = it->lo;
        if (v > it->hi) v = it->hi;
        snprintf(it->val, sizeof it->val, "%.2g", v);
    } else {
        char buf[256]; snprintf(buf, sizeof buf, "%s", it->choices);
        char *opt[16]; int n = 0, cur = -1;
        for (char *t = strtok(buf, "|"); t && n < 16; t = strtok(NULL, "|")) opt[n++] = t;
        for (int i = 0; i < n; i++) if (!strcmp(opt[i], it->val)) cur = i;
        cur = cur < 0 ? 0 : (cur + dir + n) % n;
        snprintf(it->val, sizeof it->val, "%s", opt[cur]);
    }
    ui_dirty = 1;
    if (it->live) apply_live(it); else ui_restart = 1;
}

// ---- live effects ----
static void set_volume(const char *pct)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "id=$(pactl list sink-inputs 2>/dev/null | awk '/^Sink Input #/{id=substr($3,2)} "
             "/PipeWire ALSA \\[game\\]|ALSA plug-in \\[game\\]|application.process.binary = \"game\"/{print id; exit}'); [ -n \"$id\" ] && pactl set-sink-input-volume \"$id\" %s "
             ">/dev/null 2>&1 &", pct);
    if (system(cmd)) {}
}
static void apply_live(item_t *it)
{
    const char *k = it->key;
    if (!strcmp(k, "show_fps")) show_fps = atoi(it->val);
    else if (!strcmp(k, "volume")) { if (ui_open) set_volume(it->val); }   // launcher applies it at start
    else if (!strcmp(k, "aim")) { int *p = dlsym(RTLD_DEFAULT, "rio2pad_aim_mode"); if (p) *p = !strcmp(it->val, "abs"); }
    else if (!strcmp(k, "stick_speed")) { float *p = dlsym(RTLD_DEFAULT, "rio2pad_stick_speed"); if (p) *p = strtof(it->val, NULL); }
    else if (!strcmp(k, "ui_scale")) { ui_user_scale = strtof(it->val, NULL); if (ui_user_scale < 0.5f) ui_user_scale = 0.5f; }
    else ravengfx_set(k, it->val);   // engine keys read every frame (bloomscale, glowscale)
}

// Rewrite graphics.ini: replace existing `key = value` lines, append new keys, keep comments.
static void save(void)
{
    const char *path = ravengfx_conf_path;
    if (!path) { snprintf(ui_msg, sizeof ui_msg, "No graphics.ini path - not saved"); return; }
    char *lines[512]; int n = 0; char buf[512];
    FILE *f = fopen(path, "r");
    if (f) { while (n < 512 && fgets(buf, sizeof buf, f)) lines[n++] = strdup(buf); fclose(f); }
    int done[NITEMS] = {0};
    for (int l = 0; l < n; l++) {
        char *s = lines[l]; while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == ';') continue;
        for (int i = 0; i < NITEMS; i++) {
            size_t kl = strlen(items[i].key);
            if (!strncmp(s, items[i].key, kl) && (s[kl] == ' ' || s[kl] == '=' || s[kl] == '\t')) {
                free(lines[l]); snprintf(buf, sizeof buf, "%s = %s\n", items[i].key, items[i].val);
                lines[l] = strdup(buf); done[i] = 1;
            }
        }
    }
    f = fopen(path, "w");
    if (!f) { snprintf(ui_msg, sizeof ui_msg, "Can't write %s", path); goto out; }
    for (int l = 0; l < n; l++) fputs(lines[l], f);
    int hdr = 0;
    for (int i = 0; i < NITEMS; i++) if (!done[i] && strcmp(items[i].val, items[i].def)) {
        if (!hdr++) fputs("\n# Added by the in-game overlay (F1)\n", f);
        fprintf(f, "%s = %s\n", items[i].key, items[i].val);
    }
    fclose(f);
    snprintf(ui_msg, sizeof ui_msg, ui_restart ? "Saved. Restart the game for (restart) items." : "Saved.");
    ui_dirty = 0;
out:
    for (int l = 0; l < n; l++) free(lines[l]);
}

// ---------------- CPU panel rendering ----------------
// Everything is drawn into pix at the final screen size (layout units * S), so text stays sharp.
static uint8_t *pix; static int pw, ph;
static float S = 1;                       // screen pixels per layout unit
static int cx0, cy0, cx1, cy1;            // clip rectangle, pixels
#define SC(v) ((int)((v) * S + 0.5f))
#define RGBA(r,g,b,a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)
#define RGB(r,g,b) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16)
#define ACCENT RGBA(90, 200, 120, 255)

static void canvas(int w, int h) { pw = w; ph = h; pix = realloc(pix, (size_t)pw * ph * 4); cx0 = cy0 = 0; cx1 = pw; cy1 = ph; }
static void noclip(void) { cx0 = cy0 = 0; cx1 = pw; cy1 = ph; }
static void fill(int x, int y, int w, int h, uint32_t rgba)   // pixels
{
    int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y, x1 = x + w > cx1 ? cx1 : x + w, y1 = y + h > cy1 ? cy1 : y + h;
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) memcpy(pix + ((size_t)j * pw + i) * 4, &rgba, 4);
}
static void box(float x, float y, float w, float h, uint32_t c) { fill(SC(x), SC(y), SC(x + w) - SC(x), SC(y + h) - SC(y), c); }
static int gpx(const unsigned char *g, int x, int y) { return x < 0 || y < 0 || x >= FONT_W || y >= FONT_H ? 0 : g[y * FONT_W + x]; }
static float text(float x, float y, const char *s, uint32_t rgb)   // layout units; returns end x
{
    for (; *s; s++, x += FONT_W) {
        int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?';
        if (c == ' ') continue;
        const unsigned char *g = font_px[c - 32];
        int ox = SC(x), oy = SC(y), w = SC(x + FONT_W) - ox, h = SC(y + FONT_H) - oy;
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            int px = ox + i, py = oy + j;
            if (px < cx0 || py < cy0 || px >= cx1 || py >= cy1) continue;
            float u = (i + 0.5f) / S - 0.5f, v = (j + 0.5f) / S - 0.5f, fu = floorf(u), fv = floorf(v), tu = u - fu, tv = v - fv;
            int iu = (int)fu, iv = (int)fv;
            float a = (gpx(g, iu, iv) * (1 - tu) + gpx(g, iu + 1, iv) * tu) * (1 - tv)
                    + (gpx(g, iu, iv + 1) * (1 - tu) + gpx(g, iu + 1, iv + 1) * tu) * tv;
            if (a < 1) continue;
            uint8_t *p = pix + ((size_t)py * pw + px) * 4;
            for (int k = 0; k < 3; k++) p[k] = (uint8_t)((((rgb >> (8 * k)) & 255) * a + p[k] * (255 - a)) / 255);
            if (p[3] < a) p[3] = (uint8_t)a;
        }
    }
    return x;
}
static int text_w(const char *s) { return (int)strlen(s) * FONT_W; }
static void text_c(float x, float y, float w, float h, const char *s, uint32_t c) { text(x + (w - text_w(s)) / 2, y + (h - FONT_H) / 2, s, c); }

// Clickable areas, rebuilt with every panel (pixels, relative to the panel).
enum { H_ROW, H_DEC, H_INC, H_TAB, H_SAVE, H_CLOSE, H_TRACK };
typedef struct { int x, y, w, h, act, arg; } hit_t;
static hit_t hits[256]; static int nhits;
static int g_mx = -1, g_my = -1;                    // mouse, pixels relative to the panel
static int g_ox, g_oy, g_wx = -1, g_wy = -1;        // panel origin and mouse, window pixels
static float g_thumb_y, g_thumb_h; static int drag_thumb; static float drag_off;

static void hit(float x, float y, float w, float h, int act, int arg)
{
    if (act <= H_INC) {                                   // list rows: clip to the visible list
        if (y < CT_Y) { h -= CT_Y - y; y = CT_Y; }
        if (y + h > CT_Y + CT_H) h = CT_Y + CT_H - y;
        if (h <= 0) return;
    }
    if (nhits < 256) hits[nhits++] = (hit_t){SC(x), SC(y), SC(x + w) - SC(x), SC(y + h) - SC(y), act, arg};
}
static int over(float x, float y, float w, float h) { return g_mx >= SC(x) && g_mx < SC(x + w) && g_my >= SC(y) && g_my < SC(y + h); }
static void button(float x, float y, float w, float h, const char *label, int act, int arg)
{
    int hv = over(x, y, w, h);
    box(x, y, w, h, hv ? RGBA(70, 130, 95, 255) : RGBA(32, 44, 56, 255));
    text_c(x, y, w, h, label, hv ? RGB(255, 255, 255) : RGB(200, 210, 215));
    hit(x, y, w, h, act, arg);
}

static const char *shown(item_t *it, char *b)
{
    if (it->type == T_CHOICE && (!strcmp(it->choices, "0|1"))) return atoi(it->val) ? "On" : "Off";
    snprintf(b, 40, "%s", it->val); return b;
}
static void wrapped(float x, float y, int maxw, const char *s, uint32_t c, int maxl)
{
    int per = maxw / FONT_W; if (per > 120) per = 120;
    char line[128];
    for (int l = 0; l < maxl && *s; l++) {
        int n = (int)strlen(s);
        if (n > per) { n = per; while (n > 0 && s[n] != ' ') n--; if (!n) n = per; }
        memcpy(line, s, n); line[n] = 0;
        text(x, y + l * (FONT_H + 4), line, c);
        s += n; while (*s == ' ') s++;
    }
}

static void build_panel(void)
{
    canvas(SC(PW), SC(PH)); nhits = 0;
    box(0, 0, PW, PH, RGBA(8, 12, 18, 235));
    box(0, 0, PW, 3, ACCENT);
    text(28, 20, "OpenRaven settings", RGB(140, 230, 160));
    button(PW - 24 - 40, 14, 40, 36, "X", H_CLOSE, 0);

    float tx = 24;                                          // tab bar
    for (int t = 0; t < NTABS; t++) {
        float w = text_w(tabs[t].name) + 36; int act = t == ui_tab, hv = over(tx, TAB_Y, w, TAB_H);
        box(tx, TAB_Y, w, TAB_H, act ? RGBA(26, 46, 38, 255) : hv ? RGBA(28, 38, 50, 255) : RGBA(16, 22, 30, 255));
        if (act) box(tx, TAB_Y + TAB_H - 3, w, 3, ACCENT);
        text_c(tx, TAB_Y, w, TAB_H, tabs[t].name, act ? RGB(255, 255, 255) : RGB(165, 175, 185));
        hit(tx, TAB_Y, w, TAB_H, H_TAB, t);
        tx += w + 6;
    }
    box(24, TAB_Y + TAB_H, PW - 48, 1, RGBA(60, 80, 90, 255));

    clamp_scroll();                                         // list
    cx0 = SC(CT_X); cy0 = SC(CT_Y); cx1 = SC(SB_X - 8); cy1 = SC(CT_Y + CT_H);
    for (int r = 0; r < nrows; r++) {
        float y = CT_Y + r * RH - ui_scroll;
        if (y + RH <= CT_Y || y >= CT_Y + CT_H) continue;
        if (rows[r].item < 0) {
            float e = text(CT_X + 8, y + 12, rows[r].hdr, RGB(110, 170, 230));
            box(e + 12, y + 25, SB_X - 16 - e - 12, 1, RGBA(45, 65, 85, 255));
            continue;
        }
        item_t *it = &items[rows[r].item]; char b[40];
        int sel = r == ui_sel;
        if (sel) box(CT_X, y + 2, SB_X - 8 - CT_X, RH - 4, RGBA(34, 62, 50, 240));
        hit(CT_X, y, SB_X - 8 - CT_X, RH, H_ROW, r);
        text(CT_X + 24, y + 8, it->label, sel ? RGB(255, 255, 255) : RGB(200, 205, 210));
        button(VAL_X, y + 4, 36, RH - 8, "<", H_DEC, r);
        button(VAL_X + VAL_W - 36, y + 4, 36, RH - 8, ">", H_INC, r);
        const char *v = shown(it, b);
        uint32_t vc = !strcmp(v, "On") ? RGB(140, 230, 160) : !strcmp(v, "Off") ? RGB(150, 155, 160) : RGB(255, 230, 140);
        box(VAL_X + 40, y + 4, VAL_W - 80, RH - 8, RGBA(14, 20, 28, 255));
        text_c(VAL_X + 40, y, VAL_W - 80, RH, v, vc);
        hit(VAL_X + 40, y, VAL_W - 80, RH, H_INC, r);
        if (!it->live) text(VAL_X + VAL_W + 14, y + 8, "(restart)", RGB(115, 120, 125));
    }
    noclip();
    if (max_scroll() > 0) {                                 // scrollbar
        g_thumb_h = (float)CT_H * CT_H / (nrows * RH); if (g_thumb_h < 30) g_thumb_h = 30;
        g_thumb_y = CT_Y + (CT_H - g_thumb_h) * ui_scroll / max_scroll();
        box(SB_X, CT_Y, SB_W, CT_H, RGBA(22, 30, 40, 255));
        int hv = drag_thumb || over(SB_X - 6, g_thumb_y, SB_W + 12, g_thumb_h);
        box(SB_X, g_thumb_y, SB_W, g_thumb_h, hv ? RGBA(140, 230, 160, 255) : RGBA(80, 110, 120, 255));
        hit(SB_X - 6, CT_Y, SB_W + 12, CT_H, H_TRACK, 0);
    }

    float fy = PH - 128;                                    // footer
    box(24, fy, PW - 48, 1, RGBA(60, 80, 90, 255));
    const char *h = ui_sel >= 0 && rows[ui_sel].item >= 0 ? items[rows[ui_sel].item].help : NULL;
    if (h) wrapped(28, fy + 12, PW - 56, h, RGB(170, 175, 180), 2);
    float by = PH - 60;
    if (ui_msg[0]) text(28, by + 8, ui_msg, RGB(140, 230, 160));
    else if (ui_dirty) text(28, by + 8, "Unsaved changes", RGB(240, 190, 110));
    else text(28, by + 8, "Tab: next page   Enter: save   Esc: close", RGB(110, 120, 130));
    button(PW - 24 - 110 - 10 - 110, by, 110, 40, "Save", H_SAVE, 0);
    button(PW - 24 - 110, by, 110, 40, "Close", H_CLOSE, 0);
}

// Mouse pointer (the game hides the system one). Classic arrow, X = outline, . = fill.
static const char *arrow[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
    "X.....XXXXX", "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X", "      XX" };
static void build_cursor(void)
{
    int cs = SC(1.5f); if (cs < 1) cs = 1;
    canvas(11 * cs, 17 * cs); memset(pix, 0, (size_t)pw * ph * 4);
    for (int j = 0; j < 17; j++) for (int i = 0; arrow[j][i]; i++)
        if (arrow[j][i] != ' ') fill(i * cs, j * cs, cs, cs, arrow[j][i] == 'X' ? RGBA(0, 0, 0, 255) : RGBA(255, 255, 255, 255));
}

// ---------------- input ----------------
static void ui_key(int k)   // 0 toggle, 1 up, 2 down, 3 left, 4 right, 5 enter, 6 esc, 7 next tab, 8 previous tab
{
    pthread_mutex_lock(&ui_lock);
    if (k == 0) { ui_open = !ui_open; if (ui_open) { ui_msg[0] = 0; if (!nrows) set_tab(ui_tab); } }
    else if (ui_open) switch (k) {
        case 1: move_sel(-1); break;
        case 2: move_sel(+1); break;
        case 3: case 4: if (ui_sel >= 0 && rows[ui_sel].item >= 0) change(&items[rows[ui_sel].item], k == 3 ? -1 : 1); break;
        case 5: save(); break;
        case 6: ui_open = 0; break;
        case 7: set_tab(ui_tab + 1); break;
        case 8: set_tab(ui_tab - 1); break;
    }
    pthread_mutex_unlock(&ui_lock);
}

static hit_t *hit_at(void)
{
    for (int i = nhits - 1; i >= 0; i--) {
        hit_t *h = &hits[i];
        if (g_mx >= h->x && g_mx < h->x + h->w && g_my >= h->y && g_my < h->y + h->h) return h;
    }
    return NULL;
}
static void mouse_button(int b)   // under ui_lock. 1 left, 3 right, 4/5 wheel
{
    if (b == 4 || b == 5) { ui_scroll += b == 4 ? -RH : RH; clamp_scroll(); return; }
    hit_t *h = hit_at();
    if (!h || (b != 1 && b != 3)) return;
    int dir = b == 3 ? -1 : 1;
    switch (h->act) {
        case H_ROW: ui_sel = h->arg; break;
        case H_DEC: ui_sel = h->arg; change(&items[rows[h->arg].item], -1); break;
        case H_INC: ui_sel = h->arg; change(&items[rows[h->arg].item], dir); break;
        case H_TAB: if (b == 1) set_tab(h->arg); break;
        case H_SAVE: if (b == 1) save(); break;
        case H_CLOSE: if (b == 1) ui_open = 0; break;
        case H_TRACK: {
            float my = g_my / S;
            if (my >= g_thumb_y && my < g_thumb_y + g_thumb_h) { drag_thumb = 1; drag_off = my - g_thumb_y; }
            else { ui_scroll += my < g_thumb_y ? -CT_H : CT_H; clamp_scroll(); }
        } break;
    }
}
static void mouse_moved(void)   // under ui_lock
{
    if (drag_thumb) {
        float span = CT_H - g_thumb_h;
        if (span > 0) { ui_scroll = (int)((g_my / S - drag_off - CT_Y) / span * max_scroll() + 0.5f); clamp_scroll(); }
        return;
    }
    hit_t *h = hit_at();
    if (h && h->act <= H_INC) ui_sel = h->arg;
}

// Minimal XInput2 declarations (the -dev headers aren't needed; libXi is loaded at run time).
typedef struct { int deviceid; int mask_len; unsigned char *mask; } XIEventMask_;
typedef struct { int type; unsigned long serial; Bool send_event; Display *display; int extension, evtype;
                 Time time; int deviceid, sourceid, detail, flags; } XIRawEvent_;
enum { XI_RAWBUTTONPRESS = 15, XI_RAWBUTTONRELEASE = 16 };

// Keys come from XQueryKeymap and the pointer from XQueryPointer on our own X connection; clicks
// and the wheel come from XInput2 raw events on the root window, which arrive even while the
// game holds the pointer. Nothing here touches the game's own event loop.
static void *key_thread(void *a)
{
    (void)a;
    void *x = dlopen("libX11.so.6", RTLD_NOW);
    if (!x) return NULL;
#define XS(n) __typeof__(n) *p##n = (__typeof__(n) *)dlsym(x, #n)
    XS(XOpenDisplay); XS(XQueryKeymap); XS(XKeysymToKeycode); XS(XStringToKeysym); XS(XQueryPointer);
    XS(XGetInputFocus); XS(XTranslateCoordinates); XS(XQueryExtension); XS(XPending); XS(XNextEvent);
    XS(XGetEventData); XS(XFreeEventData); XS(XFlush); XS(XDefaultRootWindow);
#undef XS
    Display *d = NULL;
    for (int i = 0; i < 100 && !(d = pXOpenDisplay(NULL)); i++) usleep(200000);
    if (!d) { fprintf(stderr, "[ravenui] no X display, overlay keys off\n"); return NULL; }
    Window root = pXDefaultRootWindow(d);
    const char *tk = getenv("RAVENGFX_UIKEY") ? getenv("RAVENGFX_UIKEY") : "F1";
    enum { NK = 10 };
    const char *names[NK] = {tk, "Up", "Down", "Left", "Right", "Return", "Escape", "Tab", "Prior", "Next"};
    int kc[NK], shift_l = pXKeysymToKeycode(d, pXStringToKeysym("Shift_L")), shift_r = pXKeysymToKeycode(d, pXStringToKeysym("Shift_R"));
    for (int i = 0; i < NK; i++) kc[i] = pXKeysymToKeycode(d, pXStringToKeysym(names[i]));

    int xi_op = -1, ev_, er_;
    void *xi = dlopen("libXi.so.6", RTLD_NOW);
    int (*xiqv)(Display *, int *, int *) = xi ? dlsym(xi, "XIQueryVersion") : NULL;
    int (*xise)(Display *, Window, XIEventMask_ *, int) = xi ? dlsym(xi, "XISelectEvents") : NULL;
    if (xiqv && xise && pXQueryExtension(d, "XInputExtension", &xi_op, &ev_, &er_)) {
        int maj = 2, min = 2;
        if (xiqv(d, &maj, &min) == Success && (maj > 2 || min >= 1)) {
            unsigned char m[3] = {0};
            m[XI_RAWBUTTONPRESS >> 3] |= 1 << (XI_RAWBUTTONPRESS & 7);
            m[XI_RAWBUTTONRELEASE >> 3] |= 1 << (XI_RAWBUTTONRELEASE & 7);
            XIEventMask_ em = {1 /* all master devices */, 3, m};
            xise(d, root, &em, 1); pXFlush(d);
        } else xi_op = -1;
    } else xi_op = -1;
    if (xi_op < 0) fprintf(stderr, "[ravenui] no XInput2: mouse clicks off (keys still work)\n");

    char prev[32] = {0}, km[32];
    double held[NK] = {0};
    int focus_t = 0, wox = 0, woy = 0, lx = -1 << 30, ly = 0;
    fprintf(stderr, "[ravenui] overlay ready (press %s)\n", tk);
    for (;;) {
        usleep(16000);
        pXQueryKeymap(d, km);
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec * 1e-9;
        int shift = (km[shift_l >> 3] >> (shift_l & 7) & 1) || (km[shift_r >> 3] >> (shift_r & 7) & 1);
        for (int i = 0; i < NK; i++) {
            int b = kc[i]; if (!b) continue;
            int down = (km[b >> 3] >> (b & 7)) & 1, was = (prev[b >> 3] >> (b & 7)) & 1;
            int k = i == 7 ? (shift ? 8 : 7) : i == 8 ? 8 : i == 9 ? 7 : i;   // Tab / Shift+Tab / PgUp / PgDn
            if (down && !was) { ui_key(k); held[i] = now + 0.4; }
            else if (down && i >= 1 && i <= 4 && now > held[i]) { ui_key(k); held[i] = now + 0.08; }   // auto-repeat
        }
        memcpy(prev, km, 32);

        if (!ui_open) {
            while (pXPending(d)) { XEvent e; pXNextEvent(d, &e); }
            g_wx = g_wy = -1; drag_thumb = 0; focus_t = 0;
            continue;
        }
        if (focus_t-- <= 0) {                     // game window origin, refreshed twice a second
            focus_t = 30; wox = woy = 0;
            Window f, ch; int rv;
            pXGetInputFocus(d, &f, &rv);
            if (f > PointerRoot) pXTranslateCoordinates(d, f, root, 0, 0, &wox, &woy, &ch);
        }
        Window r, ch; int rx, ry, wx, wy; unsigned mask = 0;
        pXQueryPointer(d, root, &r, &ch, &rx, &ry, &wx, &wy, &mask);
        pthread_mutex_lock(&ui_lock);
        g_wx = rx - wox; g_wy = ry - woy; g_mx = g_wx - g_ox; g_my = g_wy - g_oy;
        if (!(mask & Button1Mask)) drag_thumb = 0;
        if (rx != lx || ry != ly || drag_thumb) { mouse_moved(); lx = rx; ly = ry; }
        while (pXPending(d)) {
            XEvent e; pXNextEvent(d, &e);
            if (e.xcookie.type == GenericEvent && e.xcookie.extension == xi_op && pXGetEventData(d, &e.xcookie)) {
                XIRawEvent_ *re = e.xcookie.data;
                if (e.xcookie.evtype == XI_RAWBUTTONPRESS) mouse_button(re->detail);
                else if (e.xcookie.evtype == XI_RAWBUTTONRELEASE && re->detail == 1) drag_thumb = 0;
                pXFreeEventData(d, &e.xcookie);
            }
        }
        pthread_mutex_unlock(&ui_lock);
    }
    return NULL;
}

// Scripted keys for unattended tests: RAVENGFX_UISCRIPT="t:keys,..." e.g. "60:o,62:dddr,64:e"
// (o toggle, u/d/l/r arrows, e enter, x esc, t next tab), t = seconds after the first frame.
static void *script_thread(void *a)
{
    char *s = strdup((const char *)a), *tok, *sp;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (tok = strtok_r(s, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp)) {
        double at = strtod(tok, NULL); char *k = strchr(tok, ':'); if (!k) continue;
        for (;;) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
                   if (t.tv_sec - t0.tv_sec + (t.tv_nsec - t0.tv_nsec) * 1e-9 >= at) break; usleep(50000); }
        for (k++; *k; k++) {
            const char *m = "oudlrext"; const char *p = strchr(m, *k);
            if (p) { ui_key((int)(p - m)); fprintf(stderr, "[ravenui] script key %c\n", *k); usleep(150000); }
        }
    }
    free(s);
    return NULL;
}


// ---------------- GL ----------------
#define GLF(ret, name, args) static ret (*name) args
GLF(unsigned, glCreateShader_, (unsigned)); GLF(void, glShaderSource_, (unsigned, int, const char **, const int *));
GLF(void, glCompileShader_, (unsigned)); GLF(unsigned, glCreateProgram_, (void)); GLF(void, glAttachShader_, (unsigned, unsigned));
GLF(void, glLinkProgram_, (unsigned)); GLF(void, glUseProgram_, (unsigned)); GLF(int, glGetUniformLocation_, (unsigned, const char *));
GLF(void, glUniform4f_, (int, float, float, float, float)); GLF(void, glUniform1i_, (int, int));
GLF(void, glGenVertexArrays_, (int, unsigned *)); GLF(void, glBindVertexArray_, (unsigned));
GLF(void, glGenTextures_, (int, unsigned *)); GLF(void, glBindTexture_, (unsigned, unsigned));
GLF(void, glTexImage2D_, (unsigned, int, int, int, int, int, unsigned, unsigned, const void *));
GLF(void, glTexParameteri_, (unsigned, unsigned, int)); GLF(void, glActiveTexture_, (unsigned));
GLF(void, glEnable_, (unsigned)); GLF(void, glDisable_, (unsigned)); GLF(unsigned char, glIsEnabled_, (unsigned));
GLF(void, glBlendFuncSeparate_, (unsigned, unsigned, unsigned, unsigned)); GLF(void, glBlendEquationSeparate_, (unsigned, unsigned));
GLF(void, glGetIntegerv_, (unsigned, int *)); GLF(void, glViewport_, (int, int, int, int));
GLF(void, glDrawArrays_, (unsigned, int, int)); GLF(void, glBindFramebuffer_, (unsigned, unsigned));
GLF(void, glColorMask_, (unsigned char, unsigned char, unsigned char, unsigned char)); GLF(void, glDepthMask_, (unsigned char));
GLF(void, glBindSampler_, (unsigned, unsigned)); GLF(void, glBindBuffer_, (unsigned, unsigned));
GLF(void, glPixelStorei_, (unsigned, int)); GLF(void, glGetBooleanv_, (unsigned, unsigned char *));
GLF(void, glPolygonMode_, (unsigned, unsigned));

static void *glsym2(const char *n)
{
    void *f = dlsym(RTLD_DEFAULT, n);
    if (!f) { void *(*gpa)(const char *) = dlsym(RTLD_DEFAULT, "glXGetProcAddressARB"); if (gpa) f = gpa(n); }
    return f;
}
static int gl_ok = -1; static unsigned prog, vao, tex; static int u_rect, u_tex;

static const char *vs = "#version 330 core\nuniform vec4 r; out vec2 uv;\n"
    "void main(){ vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); uv = vec2(p.x, 1.0 - p.y);\n"
    " gl_Position = vec4(r.xy + p * r.zw, 0.0, 1.0); }\n";
static const char *fs = "#version 330 core\nuniform sampler2D t; in vec2 uv; out vec4 c;\n"
    "void main(){ c = texture(t, uv); }\n";

static int gl_init(void)
{
#define L(n) if (!(n##_ = glsym2(#n))) { fprintf(stderr, "[ravenui] missing %s, overlay off\n", #n); return 0; }
    L(glCreateShader) L(glShaderSource) L(glCompileShader) L(glCreateProgram) L(glAttachShader) L(glLinkProgram)
    L(glUseProgram) L(glGetUniformLocation) L(glUniform4f) L(glUniform1i) L(glGenVertexArrays) L(glBindVertexArray)
    L(glGenTextures) L(glBindTexture) L(glTexImage2D) L(glTexParameteri) L(glActiveTexture) L(glEnable) L(glDisable)
    L(glIsEnabled) L(glBlendFuncSeparate) L(glBlendEquationSeparate) L(glGetIntegerv) L(glViewport) L(glDrawArrays)
    L(glBindFramebuffer) L(glColorMask) L(glDepthMask) L(glBindSampler) L(glBindBuffer) L(glPixelStorei) L(glGetBooleanv)
#undef L
    glPolygonMode_ = glsym2("glPolygonMode");
    unsigned v = glCreateShader_(0x8B31), f = glCreateShader_(0x8B30);
    glShaderSource_(v, 1, &vs, NULL); glCompileShader_(v);
    glShaderSource_(f, 1, &fs, NULL); glCompileShader_(f);
    prog = glCreateProgram_(); glAttachShader_(prog, v); glAttachShader_(prog, f); glLinkProgram_(prog);
    u_rect = glGetUniformLocation_(prog, "r"); u_tex = glGetUniformLocation_(prog, "t");
    if (u_rect < 0) { fprintf(stderr, "[ravenui] shader failed, overlay off\n"); return 0; }
    int ov; glGetIntegerv_(0x85B5, &ov);   // GL_VERTEX_ARRAY_BINDING
    glGenVertexArrays_(1, &vao);
    glBindVertexArray_(ov);
    int ot; glGetIntegerv_(0x8069, &ot);   // GL_TEXTURE_BINDING_2D
    glGenTextures_(1, &tex); glBindTexture_(0x0DE1, tex);
    glTexParameteri_(0x0DE1, 0x2801, 0x2601); glTexParameteri_(0x0DE1, 0x2800, 0x2601);   // LINEAR
    glTexParameteri_(0x0DE1, 0x2802, 0x812F); glTexParameteri_(0x0DE1, 0x2803, 0x812F);   // CLAMP_TO_EDGE
    glBindTexture_(0x0DE1, ot);
    return 1;
}

static void draw_image(int sw, int sh, int x, int y, float scale)
{
    // save state
    int prog0, vao0, at0, tex0, samp0, fbo0, vp[4], bsr, bdr, bsa, bda, ber, bea, ua, url, upbo, pm[2];
    unsigned char cm[4], dm;
    glGetIntegerv_(0x8B8D, &prog0); glGetIntegerv_(0x85B5, &vao0); glGetIntegerv_(0x84E0, &at0);
    glActiveTexture_(0x84C0);
    glGetIntegerv_(0x8069, &tex0); glGetIntegerv_(0x8919, &samp0); glGetIntegerv_(0x8CA6, &fbo0);
    glGetIntegerv_(0x0BA2, vp); glGetIntegerv_(0x80C9, &bsr); glGetIntegerv_(0x80C8, &bdr);
    glGetIntegerv_(0x80CB, &bsa); glGetIntegerv_(0x80CA, &bda); glGetIntegerv_(0x8009, &ber); glGetIntegerv_(0x883D, &bea);
    glGetIntegerv_(0x0CF5, &ua); glGetIntegerv_(0x0CF2, &url); glGetIntegerv_(0x88EF, &upbo);
    glGetBooleanv_(0x0C23, cm); glGetBooleanv_(0x0B72, &dm);
    pm[0] = 0x1B02; if (glPolygonMode_) glGetIntegerv_(0x0B40, pm);
    static const unsigned caps[] = {0x0BE2, 0x0B71, 0x0B44, 0x0C11, 0x0B90, 0x8DB9, 0x809D, 0x8642};
    unsigned char en[8];
    for (int i = 0; i < 8; i++) en[i] = glIsEnabled_(caps[i]);   // blend depth cull scissor stencil srgb msaa progsize

    glBindFramebuffer_(0x8CA9, 0);   // GL_DRAW_FRAMEBUFFER: the window
    glViewport_(0, 0, sw, sh);
    for (int i = 0; i < 8; i++) glDisable_(caps[i]);
    glEnable_(0x0BE2);
    glBlendFuncSeparate_(0x0302, 0x0303, 1, 0x0303);   // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    glBlendEquationSeparate_(0x8006, 0x8006);
    glColorMask_(1, 1, 1, 1); glDepthMask_(0);
    if (glPolygonMode_) glPolygonMode_(0x0408, 0x1B02);
    glUseProgram_(prog); glBindVertexArray_(vao);
    glBindSampler_(0, 0); glBindTexture_(0x0DE1, tex);
    glBindBuffer_(0x88EC, 0); glPixelStorei_(0x0CF5, 4); glPixelStorei_(0x0CF2, 0);
    glTexImage2D_(0x0DE1, 0, 0x8058, pw, ph, 0, 0x1908, 0x1401, pix);   // RGBA8
    glUniform1i_(u_tex, 0);
    float w = pw * scale, h = ph * scale;
    glUniform4f_(u_rect, 2.0f * x / sw - 1, 1 - 2.0f * (y + h) / sh, 2.0f * w / sw, 2.0f * h / sh);
    glDrawArrays_(0x0005, 0, 4);   // TRIANGLE_STRIP

    // restore
    if (glPolygonMode_) glPolygonMode_(0x0408, pm[0]);
    glBindBuffer_(0x88EC, upbo); glPixelStorei_(0x0CF5, ua); glPixelStorei_(0x0CF2, url);
    glBindTexture_(0x0DE1, tex0); glBindSampler_(0, samp0); glActiveTexture_(at0);
    glBindVertexArray_(vao0); glUseProgram_(prog0);
    glColorMask_(cm[0], cm[1], cm[2], cm[3]); glDepthMask_(dm);
    glBlendFuncSeparate_(bsr, bdr, bsa, bda); glBlendEquationSeparate_(ber, bea);
    for (int i = 0; i < 8; i++) (en[i] ? glEnable_ : glDisable_)(caps[i]);
    glViewport_(vp[0], vp[1], vp[2], vp[3]);
    glBindFramebuffer_(0x8CA9, fbo0);
}

void ravengfx_overlay_draw(int sw, int sh)
{
    if (sw <= 0 || sh <= 0) return;
    if (!ui_open && !show_fps) return;
    if (gl_ok < 0) gl_ok = gl_init();
    if (!gl_ok) return;
    pthread_mutex_lock(&ui_lock);
    // Size: 1 layout unit = 1 px on a 1080p screen, times the "Overlay size" setting, but always
    // small enough to fit the screen.
    float base = fminf(sw / 1920.0f, sh / 1080.0f) * ui_user_scale, fit = fminf(0.96f * sw / PW, 0.96f * sh / PH);
    if (ui_open) {
        S = base < fit ? base : fit; if (S < 0.35f) S = 0.35f;
        build_panel();
        g_ox = (sw - pw) / 2; g_oy = (sh - ph) / 2;
        draw_image(sw, sh, g_ox, g_oy, 1);
        if (g_wx >= 0 && g_wy >= 0 && g_wx < sw && g_wy < sh) { build_cursor(); draw_image(sw, sh, g_wx, g_wy, 1); }
    }
    if (show_fps) {
        char b[32]; snprintf(b, sizeof b, "%.0f FPS", ravengfx_fps);
        S = fminf(sw / 1920.0f, sh / 1080.0f); if (S < 0.6f) S = 0.6f;
        canvas(SC(text_w(b) + 16), SC(FONT_H + 8));
        box(0, 0, text_w(b) + 16, FONT_H + 8, RGBA(0, 0, 0, 150)); text(8, 4, b, RGB(140, 230, 160));
        draw_image(sw, sh, sw - pw - SC(12), SC(12), 1);
    }
    pthread_mutex_unlock(&ui_lock);
}


void ravenui_start(void)
{
    load_values();
    pthread_t t;
    pthread_create(&t, NULL, key_thread, NULL); pthread_detach(t);
    if (getenv("RAVENGFX_UISCRIPT")) { pthread_create(&t, NULL, script_thread, (void *)getenv("RAVENGFX_UISCRIPT")); pthread_detach(t); }
}
