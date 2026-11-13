// Online coop, host side (Linux): ask the router to forward the coop UDP port to this PC (UPnP IGD),
// with plain sockets: SSDP search, fetch the device description, SOAP AddPortMapping on the
// WANIPConnection / WANPPPConnection service. Same messages to the player as win/coopupnp_win.c.
//
//   RAVEN_COOP_UPNP=0   do not touch the router (default 1)
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static char g_upnp[300];
static int g_mapped, g_port;
static char g_ctl_host[64], g_ctl_path[512], g_svc[96];
static int g_ctl_port;

const char *coopupnp_status(void) { return g_upnp; }

static int local_ip(char *out, size_t cap)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);   // connect on UDP sends nothing, it only picks a route
    socklen_t l = sizeof a;
    int ok = !connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &l);
    close(s);
    if (ok) inet_ntop(AF_INET, &a.sin_addr, out, (socklen_t)cap);
    return ok;
}
int coopupnp_local_ip(char *out, size_t cap) { return local_ip(out, cap); }

// http://host:port/path -> parts
static int split_url(const char *u, char *host, size_t hc, int *port, char *path, size_t pc)
{
    if (strncmp(u, "http://", 7)) return 0;
    u += 7;
    const char *slash = strchr(u, '/'), *colon = strchr(u, ':');
    if (!slash) slash = u + strlen(u);
    *port = 80;
    if (colon && colon < slash) { snprintf(host, hc, "%.*s", (int)(colon - u), u); *port = atoi(colon + 1); }
    else snprintf(host, hc, "%.*s", (int)(slash - u), u);
    snprintf(path, pc, "%s", *slash ? slash : "/");
    return 1;
}

// One HTTP request, whole answer into buf (headers included). Returns its length or -1.
static int http(const char *host, int port, const char *req, char *buf, int cap)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port);
    struct timeval tv = {3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1 || connect(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
    send(s, req, strlen(req), 0);
    int n = 0, r;
    while (n < cap - 1 && (r = recv(s, buf + n, cap - 1 - n, 0)) > 0) n += r;
    buf[n] = 0;
    close(s);
    return n;
}

static int soap(const char *action, const char *args, char *out, int cap)
{
    char body[1500], req[2500];
    snprintf(body, sizeof body,
             "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>",
             action, g_svc, args, action);
    snprintf(req, sizeof req,
             "POST %s HTTP/1.0\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
             "SOAPAction: \"%s#%s\"\r\nContent-Length: %zu\r\n\r\n%s",
             g_ctl_path, g_ctl_host, g_ctl_port, g_svc, action, strlen(body), body);
    int n = http(g_ctl_host, g_ctl_port, req, out, cap);
    return n > 12 && !strncmp(out + 9, "200", 3);
}

// SSDP search, then find the WAN connection service's control URL in the device description.
static int find_router(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = {2, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in m = {0};
    m.sin_family = AF_INET; m.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &m.sin_addr);
    const char *q = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                    "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    char loc[300] = "";
    for (int tries = 0; tries < 3 && !loc[0]; tries++) {
        sendto(s, q, strlen(q), 0, (struct sockaddr *)&m, sizeof m);
        char buf[1500];
        int n;
        while ((n = recv(s, buf, sizeof buf - 1, 0)) > 0) {
            buf[n] = 0;
            char *l = strcasestr(buf, "\nLOCATION:");
            if (!l) continue;
            l += 10;
            while (*l == ' ') l++;
            snprintf(loc, sizeof loc, "%.*s", (int)strcspn(l, "\r\n"), l);
            break;
        }
    }
    close(s);
    if (!loc[0]) return 0;
    char host[64], path[256], req[600];
    int port;
    if (!split_url(loc, host, sizeof host, &port, path, sizeof path)) return 0;
    static char desc[65536];
    snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s:%d\r\n\r\n", path, host, port);
    if (http(host, port, req, desc, sizeof desc) <= 0) return 0;
    const char *svcs[] = {"urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANIPConnection:2",
                          "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    for (unsigned i = 0; i < 3; i++) {
        char *p = strstr(desc, svcs[i]);
        if (!p) continue;
        char *c = strstr(p, "<controlURL>");
        if (!c) continue;
        c += 12;
        char ctl[512];
        snprintf(ctl, sizeof ctl, "%.*s", (int)(strcspn(c, "<") & 255), c);
        snprintf(g_svc, sizeof g_svc, "%s", svcs[i]);
        snprintf(g_ctl_host, sizeof g_ctl_host, "%s", host);
        g_ctl_port = port;
        if (!strncmp(ctl, "http://", 7)) split_url(ctl, g_ctl_host, sizeof g_ctl_host, &g_ctl_port, g_ctl_path, sizeof g_ctl_path);
        else snprintf(g_ctl_path, sizeof g_ctl_path, "%s%s", ctl[0] == '/' ? "" : "/", ctl);
        return 1;
    }
    return 0;
}

static void *upnp_thread(void *arg)
{
    int port = (int)(intptr_t)arg;
    char ip[64] = "";
    if (!local_ip(ip, sizeof ip)) {
        snprintf(g_upnp, sizeof g_upnp, "No network found. Players on the internet need UDP port %d forwarded to this PC", port);
        fprintf(stderr, "[coop] %s\n", g_upnp);
        return NULL;
    }
    snprintf(g_upnp, sizeof g_upnp, "Asking the router to open UDP port %d...", port);
    if (!find_router()) {
        snprintf(g_upnp, sizeof g_upnp, "The router did not answer (UPnP off?). Forward UDP port %d to %s by hand. Friends join: your public IPv4 (https://www.whatismyip.com/):%d", port, ip, port);
        fprintf(stderr, "[coop] %s\n", g_upnp);
        return NULL;
    }
    char args[700], out[4096];
    snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>", port);
    soap("DeletePortMapping", args, out, sizeof out);   // an old mapping from a crashed session, maybe to another PC
    snprintf(args, sizeof args,
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>"
             "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
             "<NewPortMappingDescription>OpenRaven coop</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>", port, port, ip);
    if (soap("AddPortMapping", args, out, sizeof out)) {
        g_mapped = 1; g_port = port;
        char ext[64] = "";
        if (soap("GetExternalIPAddress", "", out, sizeof out)) {
            char *e = strstr(out, "<NewExternalIPAddress>");
            if (e) { e += 22; snprintf(ext, sizeof ext, "%.*s", (int)strcspn(e, "<"), e); }
        }
        if (ext[0]) snprintf(g_upnp, sizeof g_upnp, "Router port opened. Friends join: %s:%d", ext, port);
        else snprintf(g_upnp, sizeof g_upnp, "Router port %d opened. Friends join: your public IPv4 (find it at https://www.whatismyip.com/):%d", port, port);
    } else
        snprintf(g_upnp, sizeof g_upnp, "The router refused. Forward UDP port %d to %s by hand. Friends join: your public IPv4 (https://www.whatismyip.com/):%d", port, ip, port);
    fprintf(stderr, "[coop] %s\n", g_upnp);
    return NULL;
}

void coopupnp_start(int port)
{
    const char *e = getenv("RAVEN_COOP_UPNP");
    if (e && *e == '0') { snprintf(g_upnp, sizeof g_upnp, "Players on the internet need UDP port %d forwarded to this PC, then join your public IPv4 (https://www.whatismyip.com/):%d", port, port); return; }
    pthread_t t;
    pthread_create(&t, NULL, upnp_thread, (void *)(intptr_t)port);
    pthread_detach(t);
}

// Game end: take the mapping out again (best effort).
void coopupnp_stop(void)
{
    if (!g_mapped) return;
    g_mapped = 0;
    char args[300], out[2048];
    snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>", g_port);
    soap("DeletePortMapping", args, out, sizeof out);
}
