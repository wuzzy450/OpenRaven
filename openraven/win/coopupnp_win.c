// Online coop, host side: ask the router to forward the coop UDP port to this PC (UPnP), through the
// Windows NAT API (the COM object "HNetCfg.NATUPnP"). If the router has UPnP off or none answers, the
// status tells the player which one port to forward by hand. The mapping is removed when the game ends.
//
//   RAVEN_COOP_UPNP=0   do not touch the router (default 1)
//
// mingw has no natupnp.h, so the three interfaces used are declared here (vtables from the Windows SDK).
#define COBJMACROS
#include "shim.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <stdio.h>
#include <string.h>

typedef struct IStaticPortMapping IStaticPortMapping;
typedef struct IStaticPortMappingCollection IStaticPortMappingCollection;
typedef struct IUPnPNAT IUPnPNAT;

#define DISPATCH_SLOTS(T)                                                                            \
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(T *, REFIID, void **);                                \
    ULONG(STDMETHODCALLTYPE *AddRef)(T *);                                                           \
    ULONG(STDMETHODCALLTYPE *Release)(T *);                                                          \
    void *GetTypeInfoCount, *GetTypeInfo, *GetIDsOfNames, *Invoke;

struct IStaticPortMappingVtbl {
    DISPATCH_SLOTS(IStaticPortMapping)
    HRESULT(STDMETHODCALLTYPE *get_ExternalIPAddress)(IStaticPortMapping *, BSTR *);
};
struct IStaticPortMapping { struct IStaticPortMappingVtbl *v; };

struct IStaticPortMappingCollectionVtbl {
    DISPATCH_SLOTS(IStaticPortMappingCollection)
    void *get__NewEnum, *get_Item, *get_Count;
    HRESULT(STDMETHODCALLTYPE *Remove)(IStaticPortMappingCollection *, long, BSTR);
    HRESULT(STDMETHODCALLTYPE *Add)(IStaticPortMappingCollection *, long, BSTR, long, BSTR, VARIANT_BOOL, BSTR, IStaticPortMapping **);
};
struct IStaticPortMappingCollection { struct IStaticPortMappingCollectionVtbl *v; };

struct IUPnPNATVtbl {
    DISPATCH_SLOTS(IUPnPNAT)
    HRESULT(STDMETHODCALLTYPE *get_StaticPortMappingCollection)(IUPnPNAT *, IStaticPortMappingCollection **);
};
struct IUPnPNAT { struct IUPnPNATVtbl *v; };

static const CLSID CLSID_UPnPNAT = {0xAE1E00AA, 0x3FD5, 0x403C, {0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1}};
static const IID IID_IUPnPNAT = {0xB171C812, 0xCC76, 0x485A, {0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99}};

static char g_upnp[320] = "";
static volatile int g_mapped, g_port;
const char *coopupnp_status(void) { return g_upnp; }

// This PC's address on the home network: the one Windows would use to reach the internet.
static int local_ip(char *out, size_t cap)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);   // connect on UDP sends nothing, it only picks a route
    int ok = !connect(s, (struct sockaddr *)&a, sizeof a);
    int l = sizeof a;
    if (ok) ok = !getsockname(s, (struct sockaddr *)&a, &l);
    closesocket(s);
    if (ok) inet_ntop(AF_INET, &a.sin_addr, out, (socklen_t)cap);
    return ok;
}

static DWORD WINAPI upnp_thread(void *arg)
{
    int port = (int)(intptr_t)arg;
    char ip[64] = "";
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    if (!local_ip(ip, sizeof ip)) {
        snprintf(g_upnp, sizeof g_upnp, "No network found. Players on the internet need UDP port %d forwarded to this PC", port);
        raven_log("[coop] %s\n", g_upnp);
        return 0;
    }
    snprintf(g_upnp, sizeof g_upnp, "Asking the router to open UDP port %d...", port);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUPnPNAT *nat = NULL;
    IStaticPortMappingCollection *col = NULL;
    // The collection can take a few seconds to show up while Windows finds the router.
    if (SUCCEEDED(CoCreateInstance(&CLSID_UPnPNAT, NULL, CLSCTX_ALL, &IID_IUPnPNAT, (void **)&nat)) && nat)
        for (int i = 0; i < 6 && !col; i++) {
            if (FAILED(nat->v->get_StaticPortMappingCollection(nat, &col)) || !col) { col = NULL; Sleep(1000); }
        }
    if (!col) {
        snprintf(g_upnp, sizeof g_upnp, "The router did not answer (UPnP off?). Forward UDP port %d to %s by hand. Friends join: your public IPv4 (https://www.whatismyip.com/):%d", port, ip, port);
        raven_log("[coop] %s\n", g_upnp);
        if (nat) nat->v->Release(nat);
        return 0;
    }
    wchar_t wip[64];
    MultiByteToWideChar(CP_ACP, 0, ip, -1, wip, 64);
    BSTR udp = SysAllocString(L"UDP"), client = SysAllocString(wip), desc = SysAllocString(L"OpenRaven coop");
    IStaticPortMapping *m = NULL;
    col->v->Remove(col, port, udp);   // an old mapping from a crashed session, maybe to another PC
    HRESULT hr = col->v->Add(col, port, udp, port, client, VARIANT_TRUE, desc, &m);
    if (SUCCEEDED(hr) && m) {
        char ext[64] = "";
        BSTR e = NULL;
        if (SUCCEEDED(m->v->get_ExternalIPAddress(m, &e)) && e) { WideCharToMultiByte(CP_ACP, 0, e, -1, ext, sizeof ext, NULL, NULL); SysFreeString(e); }
        g_mapped = 1; g_port = port;
        if (ext[0]) snprintf(g_upnp, sizeof g_upnp, "Router port opened. Friends join: %s:%d", ext, port);
        else snprintf(g_upnp, sizeof g_upnp, "Router port %d opened. Friends join: your public IPv4 (find it at https://www.whatismyip.com/):%d", port, port);
        m->v->Release(m);
    } else
        snprintf(g_upnp, sizeof g_upnp, "The router refused (error %08lx). Forward UDP port %d to %s by hand. Friends join: your public IPv4 (https://www.whatismyip.com/):%d", (unsigned long)hr, port, ip, port);
    raven_log("[coop] %s\n", g_upnp);
    SysFreeString(udp); SysFreeString(client); SysFreeString(desc);
    col->v->Release(col);
    nat->v->Release(nat);
    return 0;
}

void coopupnp_start(int port)
{
    const char *e = raven_env("RAVEN_COOP_UPNP");
    if (e && *e == '0') { snprintf(g_upnp, sizeof g_upnp, "Players on the internet need UDP port %d forwarded to this PC, then join your public IPv4 (https://www.whatismyip.com/):%d", port, port); return; }
    CloseHandle(CreateThread(NULL, 0, upnp_thread, (void *)(intptr_t)port, 0, NULL));
}

// Game end: take the mapping out again (best effort, a few seconds at most).
void coopupnp_stop(void)
{
    if (!g_mapped) return;
    g_mapped = 0;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUPnPNAT *nat = NULL;
    IStaticPortMappingCollection *col = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_UPnPNAT, NULL, CLSCTX_ALL, &IID_IUPnPNAT, (void **)&nat)) && nat) {
        if (SUCCEEDED(nat->v->get_StaticPortMappingCollection(nat, &col)) && col) {
            BSTR udp = SysAllocString(L"UDP");
            col->v->Remove(col, g_port, udp);
            SysFreeString(udp);
            col->v->Release(col);
        }
        nat->v->Release(nat);
    }
}

int coopupnp_local_ip(char *out, size_t cap)
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    return local_ip(out, cap);
}
