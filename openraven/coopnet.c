// Online coop: input-delay lockstep over UDP.
//
// Every PC runs its own copy of the game on the virtual clock (coop_win.c). Before a frame runs, all
// copies must hold the same inputs for that frame, so the frame tick waits here until they arrived.
// Star layout: clients talk to the host only, the host relays everybody's inputs. That needs one
// forwarded port (the host's) and lets the host fill in "no input" for a player whose PC went away.
//
//   RAVEN_COOP=host            wait for the other PCs, then start
//   RAVEN_COOP=join:ADDRESS    join a host (name or IP, optional :port)
//   RAVEN_COOP_PORT=27500      UDP port (host: the one port to forward)
//   RAVEN_COOP_PLAYERS=2       host: number of PCs in the session, itself included (2..4)
//   RAVEN_COOP_DELAY=3         host: frames between pressing a button and the game seeing it
//   RAVEN_COOP_BOT=1           test: this PC's player is driven by a script instead of the pad
//
// Local input sampled at frame F is used at frame F + delay on every PC. Each packet repeats the
// sender's last frames, so single lost packets cost nothing.
//
// Linux port of win/coopnet_win.c (part of rio2pad.so). The packets are byte-identical (PROTO), so
// Linux and Windows PCs can play in one session. Differences: BSD sockets, the setup "window" is
// printed to the terminal, and the files fingerprint is the game file + cabinet type only (the same
// on both OSes; the Windows version must hash the same thing for cross-OS play).
#define _GNU_SOURCE
#include "win/coop.h"
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
static int errno_val(void) { return errno; }
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
typedef unsigned long long ULONGLONG;
static ULONGLONG GetTickCount64(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000ULL + t.tv_nsec / 1000000; }
static void Sleep(unsigned ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
#define raven_log(...) fprintf(stderr, __VA_ARGS__)
static const char *raven_env(const char *n) { return getenv(n); }
static void setenv_or_unset(const char *n, const char *v) { if (v) setenv(n, v, 1); else unsetenv(n); }
#define SetEnvironmentVariableA setenv_or_unset
static void nonblock(int s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
static int x11_events_pending(void) { return 0; }
// ravengfx.so values, looked up at run time (it is a separate library)
static int gfx_int(const char *n) { int *p = dlsym(RTLD_DEFAULT, n); return p ? *p : 0; }

#define MAGIC 0x31435652u   // "RVC1"
#define PROTO 4
enum { T_HELLO = 1, T_WELCOME, T_INPUT, T_FULL, T_BYE, T_FILES, T_ASK, T_SETUP };
#define RING 1024           // frames of input kept per player
#define REPEAT 40           // frames repeated in every packet
#define HASH_EVERY 30

#pragma pack(push, 1)
struct hdr { uint32_t magic; uint8_t type, from; };
struct hello { struct hdr h; uint32_t proto; uint64_t files; };
struct welcome { struct hdr h; uint8_t player, nplayers, delay; };
// Host's answer to T_ASK: everything a joiner must copy before its game starts. The camera follows the
// picture shape, FOV and Hor+ and the 4-player picture mode, and aim rays go through the camera, so
// PCs that differ there desync (2026-11-13 test: 16:9 host + 21:9 joiner).
struct setup { struct hdr h; uint8_t nplayers, cab4, delay, horplus, wide4fit; uint16_t res_w, res_h; float fovk; char wide4[8]; };
struct in_e { uint8_t bits; uint16_t ax, ay; };
struct in_hdr { struct hdr h; uint8_t nplayers, delay, nblocks; uint32_t hash_frame; uint64_t hash, deep; uint32_t at_frame; };
struct in_block { uint8_t player; uint32_t first; uint8_t count; };
#pragma pack(pop)

static int g_mode;                      // 0 off, 1 host, 2 client
static SOCKET g_sock = INVALID_SOCKET;
static int g_port = 27500, g_nplayers = 2, g_delay = 3, g_me, g_bot, g_started;
static char g_host_name[256];
static struct sockaddr_in g_peer[COOP_PLAYERS];   // host: client addresses (index = player); client: [0] = host
static int g_peer_ok[COOP_PLAYERS];
static ULONGLONG g_peer_seen[COOP_PLAYERS];
static int g_dropped[COOP_PLAYERS];
static unsigned g_peer_frame[COOP_PLAYERS];        // last frame number a peer reported being at
static struct in_e g_in[COOP_PLAYERS][RING];
static unsigned g_upto[COOP_PLAYERS];              // inputs known for frames 1..g_upto[p]
static uint64_t g_myhash[64], g_mydeep[64];
static long g_deepdiff;
static unsigned g_myhash_frame[64];
static unsigned g_frame_now;
static long g_desyncs, g_waits_ms;
static char g_status[160] = "off";
static uint64_t g_files;                // fingerprint of this PC's loader + game code
static char g_notice[160];              // shown on screen for a while (ravenui_win.c)
static ULONGLONG g_notice_at;

// Short message on top of the game picture, for things a player must know (desync, a PC gone).
static void notice(const char *fmt, ...)
{
    char t[sizeof g_notice];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    ULONGLONG now = GetTickCount64();
    if (strcmp(t, g_notice) || now - g_notice_at > 10000) raven_log("[coop] on screen: %s\n", t);
    memcpy(g_notice, t, sizeof t);
    g_notice_at = now;
}
const char *coopnet_notice(void)
{
    return g_notice[0] && GetTickCount64() - g_notice_at < 10000 ? g_notice : NULL;
}

// Setup message while a session is being set up (Windows shows a small window): printed to the
// terminal / log whenever it changes.
static void wait_window(const char *text)
{
    static char last[1024];
    if (!text) { last[0] = 0; return; }
    if (strcmp(last, text)) { snprintf(last, sizeof last, "%s", text); fprintf(stderr, "[coop] ----\n%s\n[coop] ----\n", text); }
}

uint64_t coop_state_hash(void);        // coop_win.c

int coopnet_active(void) { return g_mode != 0; }
int coopnet_player(void) { return g_me; }
const char *coopnet_status(void) { return g_status; }

static void status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
    raven_log("[coop] %s\n", g_status);
}

// Fingerprint of the loader file and the game's code as loaded (0x400000 up to the data segment).
// Different versions would play differently, so the host only lets in PCs with the same one.
static uint64_t fnv(uint64_t h, const unsigned char *p, size_t n)
{
    while (n--) { h ^= *p++; h *= 0x100000001b3ull; }
    return h;
}
static uint64_t files_fingerprint(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    FILE *f = fopen("/proc/self/exe", "rb");   // the game file itself (the same file on Windows)
    if (f) {
        static unsigned char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) h = fnv(h, buf, n);
        fclose(f);
    }
    h = fnv(h, (const unsigned char *)0x1800e60, 4);   // cabinet type: 2- and 4-player copies play differently
    return h;
}

void coopnet_init(void)
{
    const char *m = raven_env("RAVEN_COOP"), *e;
    if (!m || !*m || !strcmp(m, "off")) return;
    if ((e = raven_env("RAVEN_COOP_PORT"))) g_port = atoi(e);
    if ((e = raven_env("RAVEN_COOP_PLAYERS"))) g_nplayers = atoi(e);
    if ((e = raven_env("RAVEN_COOP_DELAY"))) g_delay = atoi(e);
    if ((e = raven_env("RAVEN_COOP_BOT"))) g_bot = atoi(e);
    if (g_nplayers < 2) g_nplayers = 2;
    if (g_nplayers > COOP_PLAYERS) g_nplayers = COOP_PLAYERS;
    if (g_delay < 1) g_delay = 1;
    if (g_delay > 20) g_delay = 20;
    if (!strcmp(m, "host")) g_mode = 1;
    else if (!strncmp(m, "join:", 5)) {
        g_mode = 2;
        snprintf(g_host_name, sizeof g_host_name, "%s", m + 5);
        char *c = strrchr(g_host_name, ':');
        if (c) { *c = 0; g_port = atoi(c + 1); }
    } else { raven_log("[coop] RAVEN_COOP=%s not understood (host or join:ADDRESS)\n", m); return; }

    g_files = files_fingerprint();
    raven_log("[coop] game files fingerprint %016llx\n", (unsigned long long)g_files);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    nonblock(g_sock);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    if (g_mode == 1) {
        a.sin_port = htons((u_short)g_port);
        if (bind(g_sock, (struct sockaddr *)&a, sizeof a)) {
            raven_log("[coop] cannot open UDP port %d (error %d): coop off\n", g_port, errno_val());
            g_mode = 0;
            return;
        }
        g_me = 0;
        status("hosting on UDP port %d for %d players", g_port, g_nplayers);
        extern void coopupnp_start(int port);
        coopupnp_start(g_port);
    } else {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(g_host_name, NULL, &hints, &res) || !res) {
            raven_log("[coop] cannot find host %s: coop off\n", g_host_name);
            g_mode = 0;
            return;
        }
        g_peer[0] = *(struct sockaddr_in *)res->ai_addr;
        g_peer[0].sin_port = htons((u_short)g_port);
        g_peer_ok[0] = 1;
        freeaddrinfo(res);
        bind(g_sock, (struct sockaddr *)&a, sizeof a);
        status("joining %s port %d", g_host_name, g_port);
    }
}

static void send_to(const struct sockaddr_in *to, const void *buf, int n)
{
    sendto(g_sock, buf, n, 0, (const struct sockaddr *)to, sizeof *to);
}

// Joiner, at loader start (main.c, before the cabinet and the picture size are chosen): ask the host
// how its session is set up and copy that, so a joiner never needs matching Players settings.
// 2- and 4-player cabinets play differently and the host refuses a PC with the other one.
void coopnet_preconnect(void)
{
    const char *m = raven_env("RAVEN_COOP"), *e;
    if (m && !strcmp(m, "host") && (e = raven_env("RAVEN_COOP_PLAYERS")) && atoi(e) >= 3) {
        // 3-4 PCs need the 4-player cabinet. Say so in the one setting everything reads (cabinet, camera,
        // picture), exactly as the joiners get it: a host that differed here desynced (camera, 3P test 2026-11-13).
        SetEnvironmentVariableA("RIO2PAD_PLAYERS", "4");
        raven_log("[coop] hosting %d players: 4-player cabinet\n", atoi(e));
    }
    if (!m || strncmp(m, "join:", 5)) return;
    char host[256];
    int port = (e = raven_env("RAVEN_COOP_PORT")) ? atoi(e) : 27500;
    snprintf(host, sizeof host, "%s", m + 5);
    char *c = strrchr(host, ':');
    if (c) { *c = 0; port = atoi(c + 1); }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    ULONGLONG t0 = GetTickCount64(), last = 0;
    while (getaddrinfo(host, NULL, &hints, &res) || !res) {
        char t[600];
        snprintf(t, sizeof t, "Cannot find the host \"%s\".\n\nCheck the Host address in the F1 Coop tab (e.g. 203.0.113.5 or 192.168.1.20). "
                 "Close this window to quit.", host);
        wait_window(t);
        Sleep(500);
        if (GetTickCount64() - t0 > 3000) { wait_window(NULL); raven_log("[coop] cannot find host %s\n", host); return; }
    }
    struct sockaddr_in to = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    to.sin_port = htons((u_short)port);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    nonblock(g_sock);
    raven_log("[coop] asking %s port %d for its session setup\n", host, port);
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now - last >= 500) {
            last = now;
            struct hdr a = {MAGIC, T_ASK, 0};
            send_to(&to, &a, sizeof a);
        }
        fd_set r; FD_ZERO(&r); FD_SET(g_sock, &r);
        struct timeval tv = {0, 50000};
        select(g_sock + 1, &r, NULL, NULL, &tv);
        struct setup su;
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(g_sock, (char *)&su, sizeof su, 0, (struct sockaddr *)&from, &fl);
        if (n >= (int)sizeof su && su.h.magic == MAGIC && su.h.type == T_SETUP && from.sin_addr.s_addr == to.sin_addr.s_addr) {
            char v[8];
            snprintf(v, sizeof v, "%d", su.nplayers); SetEnvironmentVariableA("RAVEN_COOP_PLAYERS", v);
            SetEnvironmentVariableA("RIO2PAD_PLAYERS", su.cab4 ? "4" : "2");
            su.wide4[sizeof su.wide4 - 1] = 0;
            SetEnvironmentVariableA("RAVEN_WIDE4", su.wide4[0] ? su.wide4 : NULL);
            SetEnvironmentVariableA("RAVEN_WIDE4FIT", su.wide4fit ? "1" : NULL);
            char t[32];
            snprintf(t, sizeof t, "%u:%u", su.res_w, su.res_h); SetEnvironmentVariableA("RAVEN_COOP_ASPECT", t);
            snprintf(t, sizeof t, "%d", su.horplus); SetEnvironmentVariableA("RAVEN_COOP_HORPLUS", t);
            snprintf(t, sizeof t, "%.4f", su.fovk); SetEnvironmentVariableA("RAVEN_COOP_FOV", t);
            raven_log("[coop] host's session: %d players, %d-player cabinet, input delay %d, picture %ux%u, FOV x%.2f, Hor+ %d, 4P picture '%s'\n",
                      su.nplayers, su.cab4 ? 4 : 2, su.delay, su.res_w, su.res_h, su.fovk, su.horplus, su.wide4);
            break;
        }
        if (now - t0 > 1500) {
            char t[800];
            snprintf(t, sizeof t, "Connecting to the host %s, UDP port %d... (%llu s)\n\n"
                     "Is the host's game already running in host mode? Over the internet the host's router must pass "
                     "UDP port %d to the host's PC (the host's F1 Coop tab shows the address and port to use).\n\n"
                     "Close this window to quit.", host, port, (now - t0) / 1000, port);
            wait_window(t);
        }
    }
    closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    wait_window(NULL);
}

static struct in_e neutral(void) { struct in_e e = {0, 32768, 32768}; return e; }

// Inputs packet: host = all players' recent frames, client = its own.
static void send_inputs(void)
{
    unsigned char buf[1400];
    struct in_hdr *h = (struct in_hdr *)buf;
    h->h.magic = MAGIC; h->h.type = T_INPUT; h->h.from = (uint8_t)g_me;
    h->nplayers = (uint8_t)g_nplayers; h->delay = (uint8_t)g_delay; h->nblocks = 0;
    unsigned hf = g_frame_now / HASH_EVERY * HASH_EVERY;
    h->hash_frame = g_myhash_frame[(hf / HASH_EVERY) & 63] == hf ? hf : 0;
    h->hash = g_myhash[(hf / HASH_EVERY) & 63];
    h->deep = g_mydeep[(hf / HASH_EVERY) & 63];
    h->at_frame = g_frame_now;
    int o = sizeof *h;
    for (int p = 0; p < g_nplayers; p++) {
        if (g_mode == 2 && p != g_me) continue;
        unsigned count = g_upto[p] < REPEAT ? g_upto[p] : REPEAT;
        struct in_block b = {(uint8_t)p, g_upto[p] - count + 1, (uint8_t)count};
        memcpy(buf + o, &b, sizeof b); o += sizeof b;
        for (unsigned f = b.first; f < b.first + count; f++) { memcpy(buf + o, &g_in[p][f % RING], sizeof(struct in_e)); o += sizeof(struct in_e); }
        h->nblocks++;
    }
    if (g_mode == 1) { for (int p = 1; p < g_nplayers; p++) if (g_peer_ok[p] && !g_dropped[p]) send_to(&g_peer[p], buf, o); }
    else send_to(&g_peer[0], buf, o);
}

static void on_inputs(const unsigned char *buf, int n, int from)
{
    const struct in_hdr *h = (const struct in_hdr *)buf;
    if (n < (int)sizeof *h) return;
    if (g_mode == 2 && !g_started) {
        g_nplayers = h->nplayers; g_delay = h->delay; g_started = 1;
        for (int p = 0; p < COOP_PLAYERS; p++) g_upto[p] = (unsigned)g_delay;
    }
    g_peer_frame[from] = h->at_frame;
    static unsigned checked[COOP_PLAYERS];   // each peer's hash for a frame is compared once (packets repeat it)
    unsigned k = (h->hash_frame / HASH_EVERY) & 63;
    if (h->hash_frame && h->hash_frame != checked[from] && g_myhash_frame[k] == h->hash_frame) {
        checked[from] = h->hash_frame;
        if (g_myhash[k] != h->hash) {
            if (g_desyncs++ < 5 || g_desyncs % 100 == 0)
                raven_log("[coop] DESYNC: at frame %u this PC and player %d's PC hold different game states (%ld so far)\n", h->hash_frame, from + 1, g_desyncs);
            // Stays on screen while the copies keep differing.
            notice("Coop: OUT OF SYNC with %s - the game copies differ. Please restart the session", g_mode == 1 ? "a player's PC" : "the host");
        }
        if (g_mydeep[k] != h->deep && (g_deepdiff++ < 5 || g_deepdiff % 100 == 0))
            raven_log("[coop] deep check: at frame %u world data differs from player %d's PC (%ld so far; players match: %s)\n",
                      h->hash_frame, from + 1, g_deepdiff, g_myhash[k] == h->hash ? "yes" : "no");
    }
    int o = sizeof *h;
    for (int i = 0; i < h->nblocks; i++) {
        struct in_block b;
        if (o + (int)sizeof b > n) return;
        memcpy(&b, buf + o, sizeof b); o += sizeof b;
        if (o + b.count * (int)sizeof(struct in_e) > n || b.player >= COOP_PLAYERS) return;
        // A client only speaks for itself; the host speaks for everyone but us.
        int ok = g_mode == 1 ? b.player == from : b.player != g_me;
        for (unsigned f = b.first; f < b.first + b.count; f++, o += sizeof(struct in_e))
            if (ok && f == g_upto[b.player] + 1 && !g_dropped[b.player]) {
                memcpy(&g_in[b.player][f % RING], buf + o, sizeof(struct in_e));
                g_upto[b.player] = f;
            }
    }
}

// Read everything waiting on the socket. wait_ms: block that long for the first packet.
static void net_pump(int wait_ms)
{
    if (wait_ms > 0) {
        fd_set r;
        FD_ZERO(&r); FD_SET(g_sock, &r);
        struct timeval tv = {0, wait_ms * 1000};
        select(g_sock + 1, &r, NULL, NULL, &tv);
    }
    for (;;) {
        unsigned char buf[1500];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(g_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < (int)sizeof(struct hdr)) break;
        struct hdr *h = (struct hdr *)buf;
        if (h->magic != MAGIC) continue;
        if (g_mode == 1) {
            if (h->type == T_ASK) {   // a joiner, before its game starts: which cabinet and how many players
                struct setup su = {{MAGIC, T_SETUP, 0}, (uint8_t)g_nplayers, (uint8_t)(*(volatile int *)0x1800e60ULL == 9), (uint8_t)g_delay};
                void (*cam)(float *, int *) = (void (*)(float *, int *))dlsym(RTLD_DEFAULT, "ravengfx_coop_camera");
                int hp = 0;
                su.fovk = 1.0f;
                if (cam) cam(&su.fovk, &hp);
                su.horplus = (uint8_t)hp;
                su.res_w = (uint16_t)gfx_int("ravengfx_res_w"); su.res_h = (uint16_t)gfx_int("ravengfx_res_h");
                const char *w4 = raven_env("RAVEN_WIDE4");
                snprintf(su.wide4, sizeof su.wide4, "%s", w4 ? w4 : "");
                su.wide4fit = raven_env("RAVEN_WIDE4FIT") != NULL;
                send_to(&from, &su, sizeof su);
            } else if (h->type == T_HELLO && n >= (int)sizeof(struct hello)) {
                if (((struct hello *)buf)->proto != PROTO || ((struct hello *)buf)->files != g_files) {
                    raven_log("[coop] %s tried to join with a different loader or game version: refused\n", inet_ntoa(from.sin_addr));
                    struct hdr f = {MAGIC, T_FILES, 0};
                    send_to(&from, &f, sizeof f);
                    continue;
                }
                int p = 0;
                for (int i = 1; i < g_nplayers; i++)
                    if (g_peer_ok[i] && g_peer[i].sin_addr.s_addr == from.sin_addr.s_addr && g_peer[i].sin_port == from.sin_port) p = i;
                if (!p && !g_started)
                    for (int i = 1; i < g_nplayers && !p; i++)
                        if (!g_peer_ok[i]) { p = i; g_peer[i] = from; g_peer_ok[i] = 1; status("player %d joined from %s", i + 1, inet_ntoa(from.sin_addr)); }
                if (!p) { struct hdr f = {MAGIC, T_FULL, 0}; send_to(&from, &f, sizeof f); continue; }
                g_peer_seen[p] = GetTickCount64();
                struct welcome w = {{MAGIC, T_WELCOME, 0}, (uint8_t)p, (uint8_t)g_nplayers, (uint8_t)g_delay};
                send_to(&from, &w, sizeof w);
            } else if (h->type == T_INPUT || h->type == T_BYE) {
                int p = h->from;
                if (p < 1 || p >= g_nplayers || !g_peer_ok[p] || g_peer[p].sin_addr.s_addr != from.sin_addr.s_addr || g_peer[p].sin_port != from.sin_port) continue;
                g_peer_seen[p] = GetTickCount64();
                if (h->type == T_BYE) { if (!g_dropped[p]) { g_dropped[p] = 1; status("player %d left", p + 1); notice("Coop: player %d left the game", p + 1); } }
                else on_inputs(buf, n, p);
            }
        } else {
            if (from.sin_addr.s_addr != g_peer[0].sin_addr.s_addr) continue;
            g_peer_seen[0] = GetTickCount64();
            if (h->type == T_WELCOME && n >= (int)sizeof(struct welcome) && !g_me) {
                struct welcome *w = (struct welcome *)buf;
                g_me = w->player; g_nplayers = w->nplayers; g_delay = w->delay;
                status("joined as player %d of %d, waiting for the host to start", g_me + 1, g_nplayers);
            } else if (h->type == T_FULL) status("the host's session is full or already running");
            else if (h->type == T_FILES) status("refused: the host has a different loader or game version than this PC");
            else if (h->type == T_INPUT && g_me) on_inputs(buf, n, 0);
            else if (h->type == T_BYE && !g_dropped[0]) { g_dropped[0] = 1; status("the host ended the session: playing on alone"); notice("Coop: the host left - playing on alone"); }
        }
    }
}

// First frame tick: hold the game until every PC is here, so all copies start the same frame together.
void coopnet_start(void)
{
    if (!g_mode) return;
    ULONGLONG last = 0, t0 = GetTickCount64(), last_ui = 0;
    extern const char *coopupnp_status(void);
    extern int coopupnp_local_ip(char *out, size_t cap);
    char ip[64] = "this PC's address";
    if (g_mode == 1) coopupnp_local_ip(ip, sizeof ip);
    for (;;) {
        ULONGLONG now = GetTickCount64();
        char w[1024];
        if (g_mode == 1) {
            int have = 1;
            for (int p = 1; p < g_nplayers; p++) have += g_peer_ok[p];
            if (have == g_nplayers) break;
            if (now - last > 5000) { last = now; raven_log("[coop] waiting for players: %d of %d here\n", have, g_nplayers); }
            const char *u = coopupnp_status();
            snprintf(w, sizeof w,
                     "Hosting a %d-player session: %d of %d here. The game starts when everyone is in.\n\n"
                     "Friends at home (same router) type:   %s:%d\n"
                     "Friends over the internet type:   your public IPv4:%d\n"
                     "      Find your public IPv4 at https://www.whatismyip.com/ (or any \"what is my IP\" site).\n\n"
                     "Router: %s\n\n"
                     "Internet play needs UDP port %d passed by your router to this PC (%s). "
                     "If a firewall is on, allow UDP port %d in.",
                     g_nplayers, have, g_nplayers, ip, g_port, g_port,
                     u && u[0] ? u : "checking...", g_port, ip, g_port);
        } else {
            snprintf(w, sizeof w,
                     "%s\n\n%s"
                     "Is the host's game already running? Over the internet the host's router must pass UDP port %d to the host "
                     "(the host's F1 Coop tab shows the address and port to use).",
                     g_status, now - t0 > 15000 && !g_me ? "No answer from the host yet.\n" : "", g_port);
        }
        if (now - t0 > 1500 && now - last_ui > 200) { last_ui = now; wait_window(w); }   // a quick LAN start never flashes it
        if (g_mode == 1) {
        } else {
            if (g_started) break;
            if (now - last > 250) {
                last = now;
                if (!g_me) { struct hello h = {{MAGIC, T_HELLO, 0}, PROTO, g_files}; send_to(&g_peer[0], &h, sizeof h); }
                else { struct hdr h = {MAGIC, T_INPUT, (uint8_t)g_me}; (void)h; send_inputs(); }   // tells the host we are ready
            }
        }
        net_pump(20);
        x11_events_pending();
    }
    wait_window(NULL);
    if (g_mode == 1) { g_started = 1; for (int p = 0; p < COOP_PLAYERS; p++) g_upto[p] = (unsigned)g_delay; }
    for (int p = 0; p < COOP_PLAYERS; p++) for (int f = 0; f < RING; f++) if (f <= g_delay) g_in[p][f] = neutral();
    for (int p = 0; p < COOP_PLAYERS; p++) g_peer_seen[p] = GetTickCount64();
    status("session started: %d players, this PC is player %d, input delay %d frames", g_nplayers, g_me + 1, g_delay);
}

static uint16_t q16(float a) { if (a < 0) a = 0; if (a > 1) a = 1; return (uint16_t)(a * 65535.0f + 0.5f); }

// Test driver: presses START now and then, shoots, moves the aim in a slow figure.
static void bot_input(unsigned frame, uint8_t *bits, float *ax, float *ay)
{
    unsigned f = frame + (unsigned)g_me * 97;
    uint8_t b = 0;
    if ((f / 45) % 2 == 0 && f % 600 < 200) b |= 1;
    if (f % 600 >= 200 && (f / 7) % 5 != 4) b |= 2;
    if (f % 400 == 399) b |= 4;
    if (f % 900 == 450) b |= 8;
    *bits = b;
    *ax = 0.5f + 0.4f * (float)sin(f / 37.0 + g_me * 1.7);
    *ay = 0.5f + 0.3f * (float)sin(f / 23.0 + g_me);
}

// Frame tick: hand in this PC's input (used `delay` frames from now), return everybody's for this frame.
void coopnet_frame(unsigned frame, uint8_t bits, float ax, float ay, struct coop_in *out)
{
    g_frame_now = frame;
    if (frame % HASH_EVERY == 0) {
        unsigned k = (frame / HASH_EVERY) & 63;
        g_myhash[k] = coop_state_hash();
        extern uint64_t coop_deep_hash(void);
        g_mydeep[k] = coop_deep_hash();
        static int fake = -1;   // RAVEN_COOP_FAKEDESYNC=frame (test): pretend this copy went out of sync from then on
        if (fake < 0) { const char *e = raven_env("RAVEN_COOP_FAKEDESYNC"); fake = e ? atoi(e) : 0; }
        if (fake && frame >= (unsigned)fake) g_myhash[k] ^= 1;
        g_myhash_frame[k] = frame;
    }
    if (g_bot) bot_input(frame, &bits, &ax, &ay);
    struct in_e e = {bits, q16(ax), q16(ay)};
    unsigned tf = frame + (unsigned)g_delay;
    if (g_upto[g_me] + 1 == tf) { g_in[g_me][tf % RING] = e; g_upto[g_me] = tf; }
    send_inputs();
    net_pump(0);

    ULONGLONG t0 = GetTickCount64(), last_send = t0, last_log = t0, last_ui = t0;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        int missing = -1;
        for (int p = 0; p < g_nplayers; p++) {
            if (g_upto[p] >= frame) continue;
            // Host decides for a PC that is gone: its player stops giving input.
            int gone = g_dropped[p] || (g_mode == 2 && g_dropped[0]);
            if (!gone && p != g_me && now - g_peer_seen[g_mode == 1 ? p : 0] > 10000) {
                gone = 1;
                if (g_mode == 1) { g_dropped[p] = 1; status("player %d's PC stopped answering: the game goes on without it", p + 1); notice("Coop: lost player %d's PC - the game goes on without them", p + 1); }
                else { g_dropped[0] = 1; status("lost the host: playing on alone"); notice("Coop: lost the host - playing on alone"); }
            }
            if (gone && (g_mode == 1 || g_dropped[0])) {
                while (g_upto[p] < frame + (unsigned)g_delay) { g_upto[p]++; g_in[p][g_upto[p] % RING] = neutral(); }
                continue;
            }
            missing = p;
        }
        if (missing < 0) break;
        net_pump(2);
        now = GetTickCount64();
        if (now - last_send >= 5) { last_send = now; send_inputs(); }
        if (now - last_ui >= 50) { last_ui = now; x11_events_pending(); }
        if (now - last_log >= 3000) { last_log = now; raven_log("[coop] frame %u: waiting for player %d's input\n", frame, missing + 1); }
    }
    g_waits_ms += (long)(GetTickCount64() - t0);
    if (frame % 3600 == 0) { raven_log("[coop] frame %u: waited %ld ms for the network in the last minute, %ld desyncs\n", frame, g_waits_ms, g_desyncs); g_waits_ms = 0; }

    memset(out, 0, sizeof *out);
    for (int p = 0; p < COOP_PLAYERS; p++) {
        struct in_e v = p < g_nplayers ? g_in[p][frame % RING] : neutral();
        out->bits[p] = v.bits;
        out->aim[p][0] = v.ax / 65535.0f;
        out->aim[p][1] = v.ay / 65535.0f;
    }
}

void coopnet_shutdown(void)
{
    static int done;
    if (!g_mode || g_sock == INVALID_SOCKET || done++) return;
    extern void coopupnp_stop(void);
    struct hdr h = {MAGIC, T_BYE, (uint8_t)g_me};
    for (int i = 0; i < 3; i++) {
        if (g_mode == 1) { for (int p = 1; p < g_nplayers; p++) if (g_peer_ok[p]) send_to(&g_peer[p], &h, sizeof h); }
        else send_to(&g_peer[0], &h, sizeof h);
    }
    if (g_mode == 1) coopupnp_stop();
}
