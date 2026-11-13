// Browser stream coop: only the host has the game. Friends open http://HOST:27600 in a browser and get
// the game's own picture (H.264, streamenc_win.c) and the game's own sound (from shim_alsa.c, nothing
// else from the PC), and their mouse / keyboard / pad in the browser drives player 2, 3 or 4.
//
//   RAVEN_STREAM=1            turn it on (F1 Coop tab: "Browser stream")
//   RAVEN_STREAM_PORT=27600   TCP port (internet play: forward this TCP port to the host)
//
// One thread serves the page and the WebSocket (/ws). Messages to the browser (binary, first byte):
//   'I' video init segment (fMP4), 'V' video fragment, 'A' audio (s16 stereo 48 kHz), 'S' status text.
// From the browser: 8 bytes {1, buttons, aim x u16, aim y u16, has_aim, 0}. Buttons: 1 fire, 2 reload,
// 4 grenade, 8 start, 16 back.
#include "shim.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <xinput.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAXC 8
#define QMAX (4 << 20)          // bytes queued for one browser before video frames are dropped
struct client {
    SOCKET s;
    int ws;                     // 0 = HTTP request being read, 1 = WebSocket
    int player;                 // game player index 1..3, or -1 = watching only
    int need_key;               // waiting for a keyframe before sending video
    unsigned sent, skipped;     // video frames, for the 5 s log line
    char in[8192]; int nin;
    unsigned char *q; size_t nq, cq;   // outgoing bytes
    // input
    uint8_t buttons; float ax, ay; int has_aim; ULONGLONG seen;
};
static struct client g_c[MAXC];
static CRITICAL_SECTION g_cs;
static int g_on, g_port = 27600;
static unsigned char *g_init; static size_t g_ninit;   // current video init segment
static volatile LONG g_viewers, g_want_key;

int streamweb_active(void) { return g_on; }
int streamweb_viewers(void) { return g_viewers; }
int streamweb_take_key_request(void) { return InterlockedExchange(&g_want_key, 0); }

static const char *page(void);

// ---- small helpers ----
static void b64(const unsigned char *in, int n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        unsigned v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63]; out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}
static int sha1(const void *d, ULONG n, unsigned char out[20])
{
    BCRYPT_ALG_HANDLE a;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA1_ALGORITHM, NULL, 0)) return 0;
    BCRYPT_HASH_HANDLE h;
    int ok = !BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0) && !BCryptHashData(h, (PUCHAR)d, n, 0) && !BCryptFinishHash(h, out, 20, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

// Append bytes to a client's queue (caller holds g_cs).
static int queue(struct client *c, const void *d, size_t n)
{
    if (c->nq + n > c->cq) {
        size_t nc = c->cq ? c->cq : 65536;
        while (nc < c->nq + n) nc *= 2;
        unsigned char *q = realloc(c->q, nc);
        if (!q) return 0;
        c->q = q; c->cq = nc;
    }
    memcpy(c->q + c->nq, d, n); c->nq += n;
    return 1;
}
// One WebSocket binary message: type byte + payload.
static void ws_send(struct client *c, char type, const void *d, size_t n)
{
    unsigned char h[10]; int hn;
    size_t len = n + 1;
    h[0] = 0x82;
    if (len < 126) { h[1] = (unsigned char)len; hn = 2; }
    else if (len < 65536) { h[1] = 126; h[2] = (unsigned char)(len >> 8); h[3] = (unsigned char)len; hn = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (unsigned char)(len >> (56 - 8 * i)); hn = 10; }
    queue(c, h, hn); queue(c, &type, 1); queue(c, d, n);
}
static void status_to(struct client *c)
{
    char s[160];
    if (c->player > 0) snprintf(s, sizeof s, "You are player %d. Press START (Enter) to join the game.", c->player + 1);
    else snprintf(s, sizeof s, "All player spots are taken: you are watching. (The host can set F1 Cabinet Players to 4 for up to 3 friends.)");
    ws_send(c, 'S', s, strlen(s));
}

static void drop(struct client *c)
{
    if (c->ws) {
        InterlockedDecrement(&g_viewers);
        raven_log("[stream] %s left\n", c->player > 0 ? "a player" : "a viewer");
        if (c->player > 0) raven_log("[stream] player %d's spot is free again\n", c->player + 1);
    }
    closesocket(c->s);
    free(c->q);
    memset(c, 0, sizeof *c);
    c->s = INVALID_SOCKET;
}

// ---- requests ----
static void http(struct client *c)
{
    c->in[c->nin] = 0;
    char *end = strstr(c->in, "\r\n\r\n");
    if (!end) { if (c->nin >= (int)sizeof c->in - 1) drop(c); return; }
    char *key = strstr(c->in, "Sec-WebSocket-Key:");
    if (!strncmp(c->in, "GET /ws", 7) && key) {
        key += 18; while (*key == ' ') key++;
        char k[128]; int n = 0;
        while (key[n] && key[n] != '\r' && n < 100) { k[n] = key[n]; n++; }
        k[n] = 0;
        strcat(k, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        unsigned char h[20]; char acc[40];
        sha1(k, (ULONG)strlen(k), h); b64(h, 20, acc);
        char r[256];
        int rn = snprintf(r, sizeof r, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
        queue(c, r, rn);
        c->ws = 1; c->nin = 0; c->need_key = 1; c->seen = GetTickCount64();
        int taken[4] = {1, 0, 0, 0};
        for (int i = 0; i < MAXC; i++) if (&g_c[i] != c && g_c[i].ws && g_c[i].player > 0) taken[g_c[i].player] = 1;
        c->player = -1;
        const char *cab = raven_env("RIO2PAD_PLAYERS");
        int maxp = cab && atoi(cab) >= 3 ? 4 : 2;   // a 2-player cabinet has one spot for a friend
        for (int p = 1; p < maxp && c->player < 0; p++) if (!taken[p]) c->player = p;
        InterlockedIncrement(&g_viewers);
        InterlockedExchange(&g_want_key, 1);
        raven_log("[stream] browser joined: %s\n", c->player > 0 ? (c->player == 1 ? "player 2" : c->player == 2 ? "player 3" : "player 4") : "watching only");
        status_to(c);
        if (g_init) ws_send(c, 'I', g_init, g_ninit);
        return;
    }
    if (!strncmp(c->in, "GET / ", 6) || !strncmp(c->in, "GET /index.html", 15)) {
        char h[200];
        int hn = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", (unsigned)strlen(page()));
        queue(c, h, hn); queue(c, page(), strlen(page()));
    } else {
        static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        queue(c, nf, sizeof nf - 1);
    }
    c->nin = -1;   // close once sent
}

static void ws_read(struct client *c)
{
    for (;;) {
        unsigned char *b = (unsigned char *)c->in;
        if (c->nin < 2) return;
        int op = b[0] & 15, masked = b[1] & 0x80;
        size_t len = b[1] & 127; int hn = 2;
        if (len == 126) { if (c->nin < 4) return; len = (size_t)b[2] << 8 | b[3]; hn = 4; }
        else if (len == 127) { drop(c); return; }
        if (masked) hn += 4;
        if (len > 4096) { drop(c); return; }
        if (c->nin < hn + (int)len) return;
        unsigned char *m = b + hn - 4, *p = b + hn;
        if (masked) for (size_t i = 0; i < len; i++) p[i] ^= m[i & 3];
        if (op == 8) { drop(c); return; }
        if (op == 9) { unsigned char pong[2] = {0x8a, 0}; queue(c, pong, 2); }
        if (op == 2 && len >= 8 && p[0] == 1) {
            c->buttons = p[1];
            c->ax = (p[2] | p[3] << 8) / 65535.0f; c->ay = (p[4] | p[5] << 8) / 65535.0f;
            c->has_aim = p[6];
            c->seen = GetTickCount64();
        }
        memmove(b, b + hn + len, c->nin - hn - len);
        c->nin -= hn + (int)len;
    }
}

static DWORD WINAPI server(void *arg)
{
    SOCKET ls = (SOCKET)(uintptr_t)arg;
    for (;;) {
        fd_set r, w;
        FD_ZERO(&r); FD_ZERO(&w); FD_SET(ls, &r);
        EnterCriticalSection(&g_cs);
        for (int i = 0; i < MAXC; i++) if (g_c[i].s != INVALID_SOCKET) { FD_SET(g_c[i].s, &r); if (g_c[i].nq) FD_SET(g_c[i].s, &w); }
        LeaveCriticalSection(&g_cs);
        struct timeval tv = {0, 4000};
        select(0, &r, &w, NULL, &tv);
        EnterCriticalSection(&g_cs);
        if (FD_ISSET(ls, &r)) {
            SOCKET s = accept(ls, NULL, NULL);
            int i = 0;
            while (i < MAXC && g_c[i].s != INVALID_SOCKET) i++;
            if (s != INVALID_SOCKET && i < MAXC) {
                u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
                int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
                memset(&g_c[i], 0, sizeof g_c[i]); g_c[i].s = s;
            } else if (s != INVALID_SOCKET) closesocket(s);
        }
        for (int i = 0; i < MAXC; i++) {
            struct client *c = &g_c[i];
            if (c->s == INVALID_SOCKET) continue;
            if (FD_ISSET(c->s, &r) && c->nin >= 0) {
                int n = recv(c->s, c->in + c->nin, (int)sizeof c->in - 1 - c->nin, 0);
                if (n <= 0) { drop(c); continue; }
                c->nin += n;
                if (c->ws) ws_read(c); else http(c);
                if (c->s == INVALID_SOCKET) continue;
            }
            if (c->nq) {
                int n = send(c->s, (const char *)c->q, (int)(c->nq > 1 << 20 ? 1 << 20 : c->nq), 0);
                if (n > 0) { memmove(c->q, c->q + n, c->nq - n); c->nq -= n; }
                else if (WSAGetLastError() != WSAEWOULDBLOCK) { drop(c); continue; }
            }
            if (!c->ws && c->nin < 0 && !c->nq) drop(c);
            else if (c->ws && GetTickCount64() - c->seen > 30000) drop(c);   // browser tab gone quiet
        }
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

void streamweb_start(void)
{
    const char *e = raven_env("RAVEN_STREAM");
    if (!e || *e != '1') return;
    if ((e = raven_env("RAVEN_STREAM_PORT"))) g_port = atoi(e);
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    InitializeCriticalSection(&g_cs);
    for (int i = 0; i < MAXC; i++) g_c[i].s = INVALID_SOCKET;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons((u_short)g_port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) {
        raven_log("[stream] cannot open TCP port %d (error %d): browser stream off\n", g_port, WSAGetLastError());
        closesocket(ls);
        return;
    }
    g_on = 1;
    extern int coopupnp_local_ip(char *out, size_t cap);
    char ip[64] = "this PC";
    coopupnp_local_ip(ip, sizeof ip);
    raven_log("[stream] browser stream on: friends open http://%s:%d (internet: forward TCP port %d, then http://<your public IPv4>:%d - find it at https://www.whatismyip.com/)\n",
              ip, g_port, g_port, g_port);
    CloseHandle(CreateThread(NULL, 0, server, (void *)(uintptr_t)ls, 0, NULL));
}

// ---- from the video encoder ----
void streamweb_video(const unsigned char *init, size_t ninit, const unsigned char *frag, size_t n, int key)
{
    if (!g_on) return;
    EnterCriticalSection(&g_cs);
    if (init && (!g_init || g_ninit != ninit || memcmp(g_init, init, ninit))) {
        free(g_init);
        g_init = malloc(ninit); memcpy(g_init, init, ninit); g_ninit = ninit;
        for (int i = 0; i < MAXC; i++) if (g_c[i].ws) { ws_send(&g_c[i], 'I', g_init, g_ninit); g_c[i].need_key = 1; }
    }
    for (int i = 0; i < MAXC; i++) {
        struct client *c = &g_c[i];
        if (!c->ws) continue;
        if (c->nq > QMAX) { c->need_key = 1; c->skipped++; InterlockedExchange(&g_want_key, 1); continue; }   // slow link: skip to the next keyframe
        if (c->need_key && !key) { c->skipped++; continue; }
        c->need_key = 0;
        ws_send(c, 'V', frag, n); c->sent++;
    }
    static ULONGLONG last_log;
    ULONGLONG now = GetTickCount64();
    if (now - last_log >= 5000) {
        last_log = now;
        for (int i = 0; i < MAXC; i++) {
            struct client *c = &g_c[i];
            if (!c->ws) continue;
            raven_log("[stream] browser %d: %u frames sent, %u skipped, %u KB waiting to send\n",
                      c->player > 0 ? c->player + 1 : 0, c->sent, c->skipped, (unsigned)(c->nq / 1024));
            c->sent = c->skipped = 0;
        }
    }
    LeaveCriticalSection(&g_cs);
}

// ---- game sound (shim_alsa.c mixer thread): 48 kHz float stereo -> s16, sent every 20 ms ----
void streamweb_audio(const float *lr, unsigned frames, unsigned rate)
{
    if (!g_on || !g_viewers) return;
    static short buf[960 * 2 * 4]; static unsigned nb;
    static double pos; static unsigned last_rate;
    if (rate != last_rate) { last_rate = rate; pos = 0; nb = 0; }
    // resample to 48 kHz (nearest; FMOD normally already mixes at 48 kHz)
    double step = rate / 48000.0;
    for (;;) {
        unsigned i = (unsigned)pos;
        if (i >= frames) { pos -= frames; break; }
        for (int k = 0; k < 2; k++) {
            float v = lr[2 * i + k] * 32767.0f;
            buf[2 * nb + k] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        nb++;
        pos += step;
        if (nb == 960) {
            EnterCriticalSection(&g_cs);
            for (int c = 0; c < MAXC; c++) if (g_c[c].ws && g_c[c].nq < QMAX) ws_send(&g_c[c], 'A', buf, nb * 4);
            LeaveCriticalSection(&g_cs);
            nb = 0;
        }
    }
}

// ---- to rio2pad_win.c: browser player for game player index p (1..3) ----
// Number of browsers that hold a player spot (not watch-only).
int streamweb_players(void)
{
    if (!g_on) return 0;
    int n = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MAXC; i++) if (g_c[i].ws && g_c[i].player > 0) n++;
    LeaveCriticalSection(&g_cs);
    return n;
}

int streamweb_pad(int p, XINPUT_STATE *st, float *ax, float *ay)
{
    if (!g_on) return 0;
    int ok = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MAXC; i++) {
        struct client *c = &g_c[i];
        if (!c->ws || c->player != p) continue;
        memset(st, 0, sizeof *st);
        if (c->buttons & 1) st->Gamepad.bRightTrigger = 255;
        if (c->buttons & 2) st->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (c->buttons & 4) st->Gamepad.bLeftTrigger = 255;
        if (c->buttons & 8) st->Gamepad.wButtons |= XINPUT_GAMEPAD_START;
        if (c->buttons & 16) st->Gamepad.wButtons |= XINPUT_GAMEPAD_BACK;
        *ax = c->has_aim ? c->ax : -1; *ay = c->ay;
        ok = 1;
    }
    LeaveCriticalSection(&g_cs);
    return ok;
}

// ---- the page ----
static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Halo: Fireteam Raven - stream</title><style>"
"html,body{margin:0;height:100%;background:#000;color:#dde;font:16px system-ui,sans-serif;overflow:hidden}"
"video{position:absolute;inset:0;width:100%;height:100%;object-fit:contain;background:#000;cursor:crosshair}"
"#bar{position:absolute;left:0;right:0;top:0;padding:6px 10px;background:#0009;font-size:14px;pointer-events:none}"
"#go{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;flex-direction:column;background:#000c;cursor:pointer;text-align:center;padding:16px}"
"#go b{font-size:28px;margin-bottom:12px}</style></head><body>"
"<video id=v muted playsinline></video><div id=bar>Connecting...</div>"
"<div id=go><b>Click to play</b><div>Mouse: aim, left = fire, right = reload, middle or G = grenade.<br>"
"Enter = START (join), F = full screen. A game pad works too (if this browser allows it);<br>- and = make the pad's aim slower or faster.</div></div>"
"<script>\n"
"const v=document.getElementById('v'),bar=document.getElementById('bar'),go=document.getElementById('go');\n"
"let ws,ms,sb,q=[],ac,at=0,status='';\n"
"const show=()=>{bar.textContent=status;};\n"
"function feed(){if(sb&&!sb.updating&&q.length){try{sb.appendBuffer(q.shift());}catch(e){q=[];}}}\n"
"function init(seg){ms=new MediaSource();v.src=URL.createObjectURL(ms);q=[seg];\n"
" ms.addEventListener('sourceopen',()=>{const s=new Uint8Array(seg);let c='avc1.42e01f';\n"
"  for(let i=0;i+8<s.length;i++)if(s[i]==0x61&&s[i+1]==0x76&&s[i+2]==0x63&&s[i+3]==0x43){c='avc1.'+[s[i+5],s[i+6],s[i+7]].map(x=>x.toString(16).padStart(2,'0')).join('');break;}\n"
"  sb=ms.addSourceBuffer('video/mp4; codecs=\"'+c+'\"');sb.mode='sequence';sb.addEventListener('updateend',feed);feed();},{once:true});}\n"
"function audio(buf){if(!ac)return;const s=new Int16Array(buf),n=s.length/2,b=ac.createBuffer(2,n,48000);\n"
" const l=b.getChannelData(0),r=b.getChannelData(1);for(let i=0;i<n;i++){l[i]=s[2*i]/32768;r[i]=s[2*i+1]/32768;}\n"
" const src=ac.createBufferSource();src.buffer=b;src.connect(ac.destination);const now=ac.currentTime;\n"
" if(at<now+0.03||at>now+0.3)at=now+0.06;src.start(at);at+=n/48000;}\n"
"function connect(){ws=new WebSocket((location.protocol=='https:'?'wss://':'ws://')+location.host+'/ws');ws.binaryType='arraybuffer';\n"
" ws.onmessage=e=>{const d=new Uint8Array(e.data),t=d[0],p=e.data.slice(1);\n"
"  if(t==73)init(p);else if(t==86){q.push(p);if(q.length>30)q.splice(0,q.length-30);feed();}\n"
"  else if(t==65)audio(p);else if(t==83){status=new TextDecoder().decode(p);show();}};\n"
" ws.onclose=()=>{status='Disconnected - retrying...';show();sb=null;setTimeout(connect,2000);};}\n"
"connect();\n"
"// stay at the live edge: play a bit faster when a little behind, jump when far behind or stuck in a gap;\n"
"// drop old video so memory stays small. The bar shows how far the picture is behind the newest frame.\n"
"let lagms=0,lagt=0;\n"
"setInterval(()=>{const b=v.buffered;if(!b.length)return;const s=b.start(b.length-1),e=b.end(b.length-1),t=v.currentTime,lag=e-t;\n"
" if(lag>0.15||t<s-0.01){v.currentTime=Math.max(s,e-0.02);v.playbackRate=1;}\n"
" else v.playbackRate=lag>0.06?1.25:lag>0.035?1.08:1;\n"
" if(v.paused)v.play().catch(()=>{});\n"
" lagms=lagms*0.8+lag*1000*0.2;const n=performance.now();if(n-lagt>1000){lagt=n;bar.textContent=status+(status?'  |  ':'')+'picture delay '+Math.round(lagms)+' ms';}\n"
" if(sb&&!sb.updating&&!q.length&&b.start(0)<t-20){try{sb.remove(0,t-10);}catch(x){}}},50);\n"
"let btn=0,keys=0,ax=0.5,ay=0.5,has=0;\n"
"function aimAt(x,y){const r=v.getBoundingClientRect(),vw=v.videoWidth||16,vh=v.videoHeight||9,k=Math.min(r.width/vw,r.height/vh),w=vw*k,h=vh*k;\n"
" ax=Math.min(1,Math.max(0,(x-r.left-(r.width-w)/2)/w));ay=Math.min(1,Math.max(0,(y-r.top-(r.height-h)/2)/h));has=1;}\n"
"v.addEventListener('mousemove',e=>aimAt(e.clientX,e.clientY));\n"
"v.addEventListener('mousedown',e=>{btn|=e.button==0?1:e.button==2?2:4;aimAt(e.clientX,e.clientY);e.preventDefault();});\n"
"addEventListener('mouseup',e=>{btn&=~(e.button==0?1:e.button==2?2:4);});\n"
"v.addEventListener('contextmenu',e=>e.preventDefault());\n"
"const KM={Enter:8,Digit1:8,Space:1,KeyR:2,KeyG:4,Backspace:16};\n"
"addEventListener('keydown',e=>{if(KM[e.code]){keys|=KM[e.code];tap|=KM[e.code];e.preventDefault();}if(e.code=='KeyF'){document.fullscreenElement?document.exitFullscreen():document.documentElement.requestFullscreen();}});\n"
"addEventListener('keyup',e=>{if(KM[e.code])keys&=~KM[e.code];});\n"
"let last=performance.now();\n"
"// pad aim speed in screen widths per second (0.43 felt right to the user, 2026-11-13); - and = change it, remembered\n"
"let sens=0.43;try{const s=parseFloat(localStorage.getItem('padsens'));if(s>0)sens=s;}catch(e){}\n"
"addEventListener('keydown',e=>{if(e.code=='Minus'||e.code=='Equal'||e.code=='NumpadSubtract'||e.code=='NumpadAdd'){\n"
" sens=Math.min(2,Math.max(0.05,sens*(e.code=='Minus'||e.code=='NumpadSubtract'?1/1.2:1.2)));try{localStorage.setItem('padsens',sens);}catch(x){}\n"
" status='Pad aim speed '+sens.toFixed(2)+' (- slower, = faster)';show();}});\n"
"function pad(){let b=0;const now=performance.now(),dt=(now-last)/1000;last=now;let gs=[];try{gs=navigator.getGamepads?navigator.getGamepads():[];}catch(e){}\n"
" for(const g of gs){if(!g)continue;const p=i=>g.buttons[i]&&g.buttons[i].pressed;\n"
"  if(p(7)||p(0))b|=1;if(p(5)||p(2))b|=2;if(p(6)||p(4))b|=4;if(p(9))b|=8;if(p(8))b|=16;\n"
"  let sx=g.axes[2]||0,sy=g.axes[3]||0;if(Math.hypot(g.axes[0],g.axes[1])>Math.hypot(sx,sy)){sx=g.axes[0];sy=g.axes[1];}\n"
"  if(Math.hypot(sx,sy)>0.15){ax=Math.min(1,Math.max(0,ax+sx*Math.abs(sx)*sens*dt));ay=Math.min(1,Math.max(0,ay+sy*Math.abs(sy)*sens*dt));has=1;}break;}\n"
" return b;}\n"
"// every press reaches the game: taps between two sends count, and each press lasts at least 120 ms\n"
"const held=[0,0,0,0,0,0,0,0];let tap=0;\n"
"v.addEventListener('mousedown',e=>{tap|=e.button==0?1:e.button==2?2:4;});\n"
"let lastb=0,lasts=0;\n"
"function send(b){if(ws&&ws.readyState==1){const m=new Uint8Array(8),x=Math.round(ax*65535),y=Math.round(ay*65535);\n"
" m[0]=1;m[1]=b;m[2]=x&255;m[3]=x>>8;m[4]=y&255;m[5]=y>>8;m[6]=has;ws.send(m);lasts=performance.now();}}\n"
"setInterval(()=>{const raw=btn|keys|tap|pad(),now=performance.now();tap=0;let b=0;for(let i=0;i<8;i++){if(raw&(1<<i))held[i]=now+120;if(held[i]>now)b|=1<<i;}lastb=b;send(b);},16);\n"
"// aim goes out the moment the mouse moves (at most every 4 ms), not on the next 16 ms tick\n"
"v.addEventListener('mousemove',()=>{if(performance.now()-lasts>4)send(lastb);});\n"
"go.onclick=()=>{go.style.display='none';try{ac=new AudioContext({sampleRate:48000});}catch(e){ac=new AudioContext();}v.play().catch(()=>{});};\n"
"</script></body></html>";

static const char *page(void) { return PAGE; }
