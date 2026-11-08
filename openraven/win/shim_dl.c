// dlopen/dlsym: the game reaches GL, X11, FMOD and ALSA through these, so for
// now every request is logged. Unknown symbols get a stub that names itself
// if called; later milestones plug real Windows implementations in here.
#include "shim.h"
#include "elfload.h"
#include <stdlib.h>
#include <string.h>

typedef struct { char name[256]; } lx_lib;

static SV void *s_dlopen(const char *name, int flags) {
    lx_lib *l = calloc(1, sizeof *l);
    snprintf(l->name, sizeof l->name, "%s", name ? name : "<self>");
    raven_log("dlopen(%s, 0x%x) -> %p\n", l->name, flags, (void *)l);
    return l;
}

static void *lookup(void *h, const char *sym) {
    void *p = shim_lookup(sym);
    if (!p) p = elf_find_export(sym);
    const char *lib = (h && h != (void *)-1) ? ((lx_lib *)h)->name : "<default>";
    raven_log("dlsym(%s, %s) -> %s\n", lib, sym, p ? "found" : "stub");
    // Optional hooks that are absent on a normal Linux system: report "not found".
    static const char *absent[] = {"__gmon_start__", "_ITM_registerTMCloneTable",
                                   "_ITM_deregisterTMCloneTable", "_Jv_RegisterClasses", NULL};
    for (int i = 0; !p && absent[i]; i++)
        if (!strcmp(sym, absent[i])) return NULL;
    if (!p) {
        char n[512];
        snprintf(n, sizeof n, "%s:%s", lib, sym);
        p = shim_missing_stub(n);
    }
    return p;
}
static SV void *s_dlsym(void *h, const char *sym) { return lookup(h, sym); }
static SV void *s_dlvsym(void *h, const char *sym, const char *ver) { (void)ver; return lookup(h, sym); }
static SV int s_dlclose(void *h) { (void)h; return 0; }
static SV char *s_dlerror(void) { return NULL; }

typedef struct { const char *fname; void *fbase; const char *sname; void *saddr; } lx_dlinfo;
static SV int s_dladdr(const void *addr, lx_dlinfo *info) {
    memset(info, 0, sizeof *info);
    info->fname = "/pm/g7/halo/game";
    info->fbase = (void *)0x400000;
    (void)addr;
    return 1;
}

const shim_entry shims_dl[] = {
    {"dlopen", s_dlopen}, {"dlsym", s_dlsym}, {"dlvsym", s_dlvsym},
    {"dlclose", s_dlclose}, {"dlerror", s_dlerror}, {"dladdr", s_dladdr},
    {"__tls_get_addr", elf_tls_get_addr}, {"dl_iterate_phdr", elf_dl_iterate_phdr},
    {NULL, NULL},
};
