// ravenloader: native Windows loader for the Linux arcade build.
// Loads game + halo.so, links them to our shims, patches the stack-canary
// reads, and calls the game's main() through a __libc_start_main shim.
//
// usage: ravenloader.exe [root]   (root = folder containing pm/, default ".")
#include "elfload.h"
#include "shim.h"
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <windows.h>

// The game is a non-PIE ELF linked at 0x400000. Windows fills low memory with
// heaps and stacks before main() runs, so we relaunch ourselves suspended and
// reserve the game's range in the child before any of its code executes.
#define GAME_LO 0x400000ULL
#define GAME_HI 0x2400000ULL  // game spans 0x400000..0x23d3000

static int relaunch_with_reservation(void) {
    STARTUPINFOW si = {0};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    SetEnvironmentVariableW(L"RAVENLOADER_CHILD", L"1");
    if (!CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, TRUE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "ravenloader: relaunch failed (%lu)\n", GetLastError());
        return 1;
    }
    if (!VirtualAllocEx(pi.hProcess, (void *)GAME_LO, GAME_HI - GAME_LO,
                        MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        fprintf(stderr, "ravenloader: warning, reserve failed (%lu)\n", GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}

// ---- shim lookup -------------------------------------------------------------

static void *gcc_personality_stub;
float raven_fmod_volume_from_pct(const char *pct);   // shim_fmod.c
const char *ravengfx_get(const char *key);
void raven_alsa_set_gain(float g);                   // shim_alsa.c
static SV int fmod_ok(void) { return 0; }
static SV int fmod_fail(void) {
    static int n;
    if (!n++) raven_log("FMOD: audio off, FMOD calls return errors\n");
    return 28;  // FMOD_ERR_INITIALIZATION
}
void *shim_lookup(const char *name) {
    const shim_entry *tabs[] = {shims_x11, shims_glx, shims_glhook, shims_gl, shims_core, shims_fmt, shims_thread, shims_dl, shims_libc2, shims_locale, shims_devices, shims_ctx, NULL};
    for (int t = 0; tabs[t]; t++)
        for (const shim_entry *e = tabs[t]; e->name; e++)
            if (e->fn && !strcmp(e->name, name)) return e->fn;
    for (const shim_entry *e = shims_alsa; e->name; e++)
        if (!strcmp(e->name, name)) return e->fn;
    // Linux FMOD loaded (default): FMOD names bind to its exports, except a few wrappers.
    extern int raven_fmod_linux;
    if (raven_fmod_linux) {
        for (const shim_entry *e = shims_fmodlx; e->name; e++)
            if (!strcmp(e->name, name)) return e->fn;
        if (!strncmp(name, "FMOD_", 5) || strstr(name, "4FMOD")) return NULL;
    }
    // Windows FMOD DLLs (shim_fmod.c). An FMOD name it lacks, or RAVEN_NOAUDIO=1: return an error
    // (FMOD_RESULT != 0) so the game runs silent.
    if (!raven_env("RAVEN_NOAUDIO"))
        for (const shim_entry *e = shims_fmod; e->name; e++)
            if (!strcmp(e->name, name)) return e->fn;
    if (!strcmp(name, "FMOD_Memory_Initialize")) return (void *)fmod_ok;
    if (!strncmp(name, "FMOD_", 5) || strstr(name, "4FMOD")) return (void *)fmod_fail;
    extern void *raven_startup_lookup(const char *name);
    return raven_startup_lookup(name);
    (void)gcc_personality_stub;
}

// ---- stack canary --------------------------------------------------------------
// Linux reads the canary from %fs:0x28. Windows can't set fs, so each such
// instruction is rewritten to read a TLS slot in the TEB via %gs instead.

static DWORD g_canary_slot;

void raven_thread_setup(void) {
    unsigned long long c = __rdtsc() * 0x9e3779b97f4a7c15ULL;
    c &= ~0xffULL;  // glibc keeps the low byte zero
    TlsSetValue(g_canary_slot, (void *)c);
    unsigned short cw = 0x37f;  // Linux x87 default: 64-bit precision
    __asm__ volatile("fldcw %0" ::"m"(cw));
}

static int patch_canary(elf_module *m) {
    uint32_t off = 0x1480 + 8 * g_canary_slot;  // TEB.TlsSlots[idx]
    int n = 0;
    unsigned char *c = m->base, *end = m->base + m->size - 10;
    for (; c < end; c++) {
        if (c[0] != 0x64) continue;
        int i = 1;
        if ((c[i] & 0xf0) == 0x40) i++;              // REX
        unsigned char op = c[i];
        if (op != 0x8b && op != 0x33 && op != 0x2b && op != 0x3b && op != 0x39) continue;
        if ((c[i + 1] & 0xc7) != 0x04 || c[i + 2] != 0x25) continue;  // [disp32]
        if (c[i + 3] != 0x28 || c[i + 4] || c[i + 5] || c[i + 6]) continue;
        c[0] = 0x65;
        memcpy(c + i + 3, &off, 4);
        n++;
    }
    return n;
}

// ---- startup -----------------------------------------------------------------

typedef SV int (*sv_main_fn)(int, char **, char **);
typedef SV void (*sv_init_fn)(int, char **, char **);

static char **g_argv, **g_envp;
static int g_argc;

static SV int s_libc_start_main(sv_main_fn main_fn, int argc, char **argv,
                                sv_init_fn init, void *fini, void *rtld_fini, void *stack_end) {
    (void)fini; (void)rtld_fini; (void)stack_end;
    raven_log("__libc_start_main: running init %p\n", (void *)init);
    if (init) init(argc, argv, g_envp);
    raven_log("__libc_start_main: calling main %p\n", (void *)main_fn);
    int r = main_fn(argc, argv, g_envp);
    raven_log("main returned %d\n", r);
    fflush(NULL);
    exit(r);
}
static SV void s_cxa_finalize(void *d) { (void)d; }

void *raven_startup_lookup(const char *name) {
    if (!strcmp(name, "__libc_start_main")) return (void *)s_libc_start_main;
    if (!strcmp(name, "__cxa_finalize")) return (void *)s_cxa_finalize;
    return NULL;
}

static void run_inits(elf_module *m) {
    if (m->init) ((sv_init_fn)m->init)(g_argc, g_argv, g_envp);
    for (uint64_t i = 0; i < m->init_arraysz / 8; i++) {
        uint64_t f = m->init_array[i];
        if (f && f != (uint64_t)-1) ((sv_init_fn)f)(g_argc, g_argv, g_envp);
    }
}

extern void shim_core_init(void);

static intptr_t g_start[3];
static DWORD WINAPI game_thread(void *p) {
    (void)p;
    raven_thread_setup();
    s_libc_start_main((sv_main_fn)g_start[0], g_argc, g_argv, (sv_init_fn)g_start[1],
                      (void *)g_start[2], NULL, NULL);
    raven_log("game thread: __libc_start_main returned\n");
    return 0;
}

// Log hardware exceptions (access violations etc.) with the faulting module.
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Only real crashes. Everything else (C++ exceptions 0xE06D7363 that Windows DLLs and the GPU driver
    // throw and catch themselves, e.g. when HDR starts Direct3D; debug prints; RPC errors) is left to its
    // own handlers. Logging those as FATAL used to kill a healthy game.
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN */:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT *c = ep->ContextRecord;
    raven_log("FATAL: exception 0x%08lx at rip=%p (%s) addr=%p\n", code, (void *)c->Rip,
              elf_module_name((void *)c->Rip),
              (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    raven_log("  rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p rsp=%p rbp=%p\n",
              (void *)c->Rax, (void *)c->Rbx, (void *)c->Rcx, (void *)c->Rdx,
              (void *)c->Rsi, (void *)c->Rdi, (void *)c->Rsp, (void *)c->Rbp);
    uint64_t *sp = (uint64_t *)c->Rsp;
    for (int i = 0; i < 24; i++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(sp + i, &mi, sizeof mi) || mi.State != MEM_COMMIT) break;
        const char *n = elf_module_name((void *)sp[i]);
        if (strcmp(n, "?")) raven_log("  [rsp+0x%02x] %p %s\n", i * 8, (void *)sp[i], n);
    }
    fflush(NULL);
    ExitProcess(5);
}

int main(int argc, char **argv) {
    if (!getenv("RAVENLOADER_CHILD")) return relaunch_with_reservation();
    // Real pixels, not Windows display scaling (otherwise 4K at 225% reports 1707x960).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // No argument (double-click): the game folder is two levels above the exe (<game>/openraven/win).
    static char exe_root[MAX_PATH * 2];
    if (argc <= 1) {
        GetModuleFileNameA(NULL, exe_root, MAX_PATH);
        for (int up = 0; up < 3; up++) { char *s = strrchr(exe_root, '\\'); if (s) *s = 0; }
        SetCurrentDirectoryA(exe_root);   // ravenloader.log etc. land in the game folder as usual
    }
    raven_set_root(argc > 1 ? argv[1] : exe_root);
    shim_core_init();
    g_canary_slot = TlsAlloc();
    if (g_canary_slot >= 64) { fprintf(stderr, "ravenloader: TLS slot too high\n"); return 1; }
    raven_thread_setup();
    AddVectoredExceptionHandler(0, crash_handler);

    char game[MAX_PATH], halo[MAX_PATH];
    // Like play_halo.sh: the decrypted 3.11-dec game + its halo.so dongle
    // emulator replace pm/g7/halo/{game,halo.so} when present.
    const char *eg = getenv("RAVEN_GAME"), *eh = getenv("RAVEN_HALOSO");
    snprintf(game, sizeof game, "%s/openraven/3.11-dec/pm/g7/halo/game", g_root);
    snprintf(halo, sizeof halo, "%s/openraven/3.11-dec/pm/g7/halo/halo.so", g_root);
    if (GetFileAttributesA(game) == INVALID_FILE_ATTRIBUTES) {
        snprintf(game, sizeof game, "%s/pm/g7/halo/game", g_root);
        snprintf(halo, sizeof halo, "%s/pm/g7/halo/halo.so", g_root);
    }
    if (eg) snprintf(game, sizeof game, "%s", eg);
    if (eh) snprintf(halo, sizeof halo, "%s", eh);
    raven_log("game: %s\nhalo.so: %s\n", game, halo);
    raven_log("settings: RAVEN_WINDOWED=%s RAVEN_SHOT_EVERY=%s (crt: %s)\n",
              raven_env("RAVEN_WINDOWED") ? raven_env("RAVEN_WINDOWED") : "-",
              raven_env("RAVEN_SHOT_EVERY") ? raven_env("RAVEN_SHOT_EVERY") : "-",
              getenv("RAVEN_WINDOWED") ? getenv("RAVEN_WINDOWED") : "-");
    elf_module *gm = elf_load(game);
    elf_module *hm = elf_load(halo);
    // C++ runtime: the game's own Linux libstdc++ and a matching libgcc_s,
    // loaded after halo.so like the Linux search order.
    char cxx[MAX_PATH], gcc[MAX_PATH];
    const char *ec = getenv("RAVEN_LIBSTDCXX"), *eg2 = getenv("RAVEN_LIBGCC");
    // Shipped in the repo (the game archives don't contain it); GCC runtime library, redistributable.
    snprintf(cxx, sizeof cxx, "%s/openraven/cabinet_libs/lib/x86_64-linux-gnu/libstdc++.so.6", g_root);
    snprintf(gcc, sizeof gcc, "%s/openraven/cabinet_libs/lib/x86_64-linux-gnu/libgcc_s.so.1", g_root);
    if (ec) snprintf(cxx, sizeof cxx, "%s", ec);
    if (eg2) snprintf(gcc, sizeof gcc, "%s", eg2);
    raven_log("libstdc++: %s\nlibgcc_s: %s\n", cxx, gcc);
    elf_module *cm = elf_load(cxx);
    elf_module *lm = elf_load(gcc);
    // Sound: the game's own Linux FMOD 1.10 (shipped in openraven/extralibs like the Linux build),
    // output through shim_alsa.c. RAVEN_FMOD=windows uses the Windows DLLs instead (shim_fmod.c).
    elf_module *fm = NULL, *fs = NULL;
    {
        char a[MAX_PATH], b[MAX_PATH];
        snprintf(a, sizeof a, "%s/openraven/extralibs/libfmod.so.10", g_root);
        snprintf(b, sizeof b, "%s/openraven/extralibs/libfmodstudio.so.10", g_root);
        const char *mode = raven_env("RAVEN_FMOD");
        if (!raven_env("RAVEN_NOAUDIO") && !(mode && !strcmp(mode, "windows")) &&
            GetFileAttributesA(a) != INVALID_FILE_ATTRIBUTES && GetFileAttributesA(b) != INVALID_FILE_ATTRIBUTES) {
            fm = elf_load(a); fs = elf_load(b);
            extern int raven_fmod_linux;
            raven_fmod_linux = fm && fs;
            raven_log("FMOD: %s\n", raven_fmod_linux ? "game's Linux FMOD (extralibs) -> WASAPI" : "Linux FMOD failed to load");
            const char *v = raven_env("RAVEN_VOLUME");
            raven_alsa_set_gain(v ? (float)atof(v) : raven_fmod_volume_from_pct(ravengfx_get("volume") ? ravengfx_get("volume") : "0"));
        }
    }

    FILE *rep = fopen("missing_imports.tsv", "w");
    fprintf(rep, "module\tlibrary\tsymbol\n");
    int missing = elf_relocate_all(shim_lookup, rep);
    raven_log("copy-reloc objects restored: %d\n", elf_emulate_copy(gm, rep));
    elf_report(rep);
    fclose(rep);
    raven_log("loaded: %d imports without a shim (stubs installed)\n", missing);
    raven_log("canary patches: game %d, halo.so %d, libstdc++ %d, libgcc_s %d\n",
              patch_canary(gm), patch_canary(hm), patch_canary(cm), patch_canary(lm));

    // Game's own stdout/stderr variables (filled by COPY relocs on Linux).
    FILE **gout = elf_find_export("stdout"), **gerr = elf_find_export("stderr");
    if (gout) *gout = stdout;
    if (gerr) *gerr = stderr;
    FILE **gin = elf_find_export("stdin");
    if (gin) *gin = stdin;

    // Linux-style argv/envp, with the game seeing itself at its arcade path.
    extern char **_environ;
    int ne = 0;
    while (_environ[ne]) ne++;
    // graphics.ini hooks; `resolution` becomes -fWxH -n like play_halo.sh.
    extern void ravengfx_install(void);
    extern int ravengfx_args(char **argv, int argc);
    ravengfx_install();
    g_argv = calloc(ne + 8, sizeof(char *));
    g_argv[0] = "/pm/g7/halo/game";
    g_argc = ravengfx_args(g_argv, 1);
    g_envp = g_argv + g_argc + 1;
    for (int i = 0; i < ne; i++) g_envp[i] = _environ[i];

    char cwd[MAX_PATH];
    snprintf(cwd, sizeof cwd, "%s/pm/g7/halo", g_root);
    _chdir(cwd);

    extern void rio2pad_install(void);
    rio2pad_install();

    run_inits(lm);
    run_inits(cm);
    if (fm) { patch_canary(fm); run_inits(fm); }
    if (fs) { patch_canary(fs); run_inits(fs); }
    run_inits(hm);

    // _start does: mov $fini,%r8; mov $init,%rcx; mov $main,%rdi; call __libc_start_main
    unsigned char *e = (unsigned char *)gm->entry;
    if (memcmp(e + 0x0f, "\x49\xc7\xc0", 3) || memcmp(e + 0x16, "\x48\xc7\xc1", 3) ||
        memcmp(e + 0x1d, "\x48\xc7\xc7", 3)) {
        fprintf(stderr, "ravenloader: unexpected _start layout\n");
        return 1;
    }
    int32_t fini, init, mainp;
    memcpy(&fini, e + 0x12, 4);
    memcpy(&init, e + 0x19, 4);
    memcpy(&mainp, e + 0x20, 4);
    g_start[0] = (intptr_t)mainp;
    g_start[1] = (intptr_t)init;
    g_start[2] = (intptr_t)fini;
    // Linux gives the main thread an 8 MB stack; run the game on a 16 MB one.
    HANDLE t = CreateThread(NULL, 16 << 20, game_thread, NULL,
                            0 /* fully committed, see shim_thread.c */, NULL);
    WaitForSingleObject(t, INFINITE);
    DWORD tc = 0;
    GetExitCodeThread(t, &tc);
    raven_log("game thread ended, exit code 0x%08lx\n", tc);
    return 0;
}
