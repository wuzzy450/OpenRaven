// ravenloader milestone 1: load Linux x86-64 ELF files into this Windows
// process, apply relocations, and report every import we can't resolve yet.
#include "elfload.h"
#include "shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>


static elf_module *g_mods[16];
static int g_nmods;

static void die(const char *m, const char *p) {
    fprintf(stderr, "ravenloader: %s: %s\n", m, p);
    exit(1);
}

static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open", path);
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) die("short read", path);
    fclose(f);
    return b;
}

elf_module *elf_load(const char *path) {
    size_t len;
    unsigned char *file = read_file(path, &len);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_machine != EM_X86_64)
        die("not an x86-64 ELF", path);

    Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff);
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t s = ph[i].p_vaddr & ~0xfffULL;
        uint64_t e = (ph[i].p_vaddr + ph[i].p_memsz + 0xfff) & ~0xfffULL;
        if (s < lo) lo = s;
        if (e > hi) hi = e;
    }

    // ET_EXEC must sit at its link address; ET_DYN can go anywhere.
    void *want = eh->e_type == ET_EXEC ? (void *)lo : NULL;
    // The parent process may have pre-reserved this range for us (see main.c),
    // so try committing inside an existing reservation first.
    unsigned char *mem = want ? VirtualAlloc(want, hi - lo, MEM_COMMIT,
                                             PAGE_EXECUTE_READWRITE) : NULL;
    if (!mem)
        mem = VirtualAlloc(want, hi - lo, MEM_RESERVE | MEM_COMMIT,
                           PAGE_EXECUTE_READWRITE);
    if (!mem || (want && mem != want)) {
        fprintf(stderr, "ravenloader: cannot reserve %p..%p (err %lu)\n",
                (void *)lo, (void *)hi, GetLastError());
        exit(1);
    }

    elf_module *m = calloc(1, sizeof *m);
    m->path = path;
    m->bias = eh->e_type == ET_EXEC ? 0 : (uint64_t)mem - lo;
    m->base = mem;
    m->size = hi - lo;
    m->entry = eh->e_entry + m->bias;

    for (int i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *p = &ph[i];
        if (p->p_type == PT_LOAD)
            memcpy((void *)(p->p_vaddr + m->bias), file + p->p_offset, p->p_filesz);
        else if (p->p_type == PT_DYNAMIC)
            m->dyn = (Elf64_Dyn *)(p->p_vaddr + m->bias);
        else if (p->p_type == PT_TLS)
            m->tls = *p;
        else if (p->p_type == PT_INTERP)
            ;
    }

    // Count executable bytes that need attention: %fs: prefix and raw syscalls.
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        unsigned char *c = file + ph[i].p_offset;
        for (uint64_t j = 0; j + 1 < ph[i].p_filesz; j++) {
            if (c[j] == 0x64 && (c[j + 1] == 0x48 || c[j + 1] == 0x8b ||
                                 c[j + 1] == 0x4c || c[j + 1] == 0x89))
                m->fs_hits++;
            if (c[j] == 0x0f && c[j + 1] == 0x05) m->syscall_hits++;
        }
    }

    for (Elf64_Dyn *d = m->dyn; d && d->d_tag != DT_NULL; d++) {
        uint64_t v = d->d_un.d_ptr + m->bias;
        switch (d->d_tag) {
        case DT_STRTAB: m->strtab = (char *)v; break;
        case DT_SYMTAB: m->symtab = (Elf64_Sym *)v; break;
        case DT_HASH: m->hash = (uint32_t *)v; break;
        case DT_GNU_HASH: m->gnuhash = (uint32_t *)v; break;
        case DT_RELA: m->rela = (Elf64_Rela *)v; break;
        case DT_RELASZ: m->relasz = d->d_un.d_val; break;
        case DT_JMPREL: m->jmprel = (Elf64_Rela *)v; break;
        case DT_PLTRELSZ: m->pltrelsz = d->d_un.d_val; break;
        case DT_VERSYM: m->versym = (uint16_t *)v; break;
        case DT_VERNEED: m->verneed = (Elf64_Verneed *)v; break;
        case DT_INIT_ARRAY: m->init_array = (uint64_t *)v; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->d_un.d_val; break;
        case DT_INIT: m->init = v; break;
        }
    }
    m->phnum = eh->e_phnum;
    m->phdr = malloc(sizeof *ph * eh->e_phnum);
    memcpy(m->phdr, ph, sizeof *ph * eh->e_phnum);
    free(file);
    if (g_nmods >= 16) die("too many modules", path);
    g_mods[g_nmods++] = m;
    return m;
}

static uint32_t nsyms(elf_module *m) {
    if (m->hash) return m->hash[1];
    if (!m->gnuhash) return 0;
    uint32_t nb = m->gnuhash[0], so = m->gnuhash[1], bw = m->gnuhash[2];
    uint32_t *buckets = m->gnuhash + 4 + bw * 2;
    uint32_t *chain = buckets + nb;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nb; i++)
        if (buckets[i] > last) last = buckets[i];
    if (last < so) return so;
    while (!(chain[last - so] & 1)) last++;
    return last + 1;
}

// Library that a symbol's version requirement points at (for reporting).
static const char *sym_lib(elf_module *m, uint32_t idx) {
    if (!m->versym || !m->verneed) return "?";
    uint16_t want = m->versym[idx] & 0x7fff;
    Elf64_Verneed *vn = m->verneed;
    for (;;) {
        Elf64_Vernaux *va = (Elf64_Vernaux *)((char *)vn + vn->vn_aux);
        for (int i = 0; i < vn->vn_cnt; i++) {
            if (va->vna_other == want) return m->strtab + vn->vn_file;
            va = (Elf64_Vernaux *)((char *)va + va->vna_next);
        }
        if (!vn->vn_next) break;
        vn = (Elf64_Verneed *)((char *)vn + vn->vn_next);
    }
    return "?";
}

void *elf_find_export(const char *name) {
    for (int k = 0; k < g_nmods; k++) {
        elf_module *m = g_mods[k];
        uint32_t n = nsyms(m);
        for (uint32_t i = 1; i < n; i++) {
            Elf64_Sym *s = &m->symtab[i];
            if (s->st_shndx == SHN_UNDEF || !s->st_value) continue;
            if (ELF64_ST_BIND(s->st_info) == STB_LOCAL) continue;
            if ((s->st_info & 0xf) == 6) continue;          // STT_TLS: not an address
            if (m->versym && (m->versym[i] & 0x8000)) continue;   // hidden (old) version
            if (!strcmp(m->strtab + s->st_name, name))
                return (void *)(s->st_value + m->bias);
        }
    }
    return NULL;
}

// The dumped game keeps its own copies of library data objects (vtables,
// typeinfo, ...) that were filled by R_X86_64_COPY on Linux; the relocs are
// gone and the bytes are stale. Redo the copy from the library that defines
// each object. Call after elf_relocate_all.
int elf_emulate_copy(elf_module *exe, FILE *log) {
    int n = 0;
    uint32_t ns = nsyms(exe);
    for (uint32_t i = 1; i < ns; i++) {
        Elf64_Sym *s = &exe->symtab[i];
        if ((s->st_info & 0xf) != 1 || !s->st_size || s->st_shndx == SHN_UNDEF) continue;  // STT_OBJECT
        const char *name = exe->strtab + s->st_name;
        for (int k = 0; k < g_nmods; k++) {
            elf_module *m = g_mods[k];
            if (m == exe) continue;
            uint32_t nm = nsyms(m);
            for (uint32_t j = 1; j < nm; j++) {
                Elf64_Sym *t = &m->symtab[j];
                if (t->st_shndx == SHN_UNDEF || !t->st_value || (t->st_info & 0xf) != 1) continue;
                if (m->versym && (m->versym[j] & 0x8000)) continue;
                if (strcmp(m->strtab + t->st_name, name)) continue;
                size_t sz = t->st_size < s->st_size ? t->st_size : s->st_size;
                memcpy((void *)(s->st_value + exe->bias), (void *)(t->st_value + m->bias), sz);
                if (log) fprintf(log, "# copy %s (%zu bytes) from %s\n", name, sz, m->path);
                n++;
                goto next;
            }
        }
    next:;
    }
    return n;
}

// "path+0xoffset" for an address inside a loaded module (for crash logs).
const char *elf_module_name(void *addr) {
    static char buf[MAX_PATH + 32];
    for (int k = 0; k < g_nmods; k++) {
        elf_module *m = g_mods[k];
        if ((unsigned char *)addr >= m->base && (unsigned char *)addr < m->base + m->size) {
            const char *n = strrchr(m->path, '/');
            snprintf(buf, sizeof buf, "%s+0x%llx", n ? n + 1 : m->path,
                     (unsigned long long)((uint64_t)addr - m->bias));
            return buf;
        }
    }
    return "?";
}

// dl_iterate_phdr: libgcc's unwinder walks this to find .eh_frame_hdr.
typedef struct {
    uint64_t addr;
    const char *name;
    const Elf64_Phdr *phdr;
    uint16_t phnum;
    unsigned long long adds, subs;
    size_t tls_modid;
    void *tls_data;
} lx_dl_phdr_info;
typedef __attribute__((sysv_abi)) int (*lx_phdr_cb)(lx_dl_phdr_info *, size_t, void *);
__attribute__((sysv_abi)) int elf_dl_iterate_phdr(void *cbp, void *data) {
    lx_phdr_cb cb = (lx_phdr_cb)cbp;
    for (int k = 0; k < g_nmods; k++) {
        elf_module *m = g_mods[k];
        lx_dl_phdr_info info = {m->bias, k ? m->path : "", m->phdr, m->phnum,
                                (unsigned long long)g_nmods, 0, (size_t)m->tls_id, NULL};
        int r = cb(&info, sizeof info, data);
        if (r) return r;
    }
    return 0;
}

// Placeholder for unresolved imports: crash loudly with the name if called.
#define MAX_MISSING 8192
static const char *g_missing[MAX_MISSING];
static int g_nmissing;

static void *resolve(elf_module *m, uint32_t si, shim_lookup_fn shims, FILE *rep,
                     int want_stub) {
    Elf64_Sym *s = &m->symtab[si];
    const char *name = m->strtab + s->st_name;
    void *p = shims ? shims(name) : NULL;
    if (!p) p = elf_find_export(name);
    // Defined in this module under a non-default version: bind to our own copy.
    if (!p && s->st_shndx != SHN_UNDEF && s->st_value) p = (void *)(s->st_value + m->bias);
    if (!p && ELF64_ST_BIND(s->st_info) == STB_WEAK) return NULL;
    if (!p) {
        int seen = 0;
        for (int i = 0; i < g_nmissing; i++)
            if (!strcmp(g_missing[i], name)) seen = 1;
        if (!seen && g_nmissing < MAX_MISSING) {
            g_missing[g_nmissing++] = name;
            fprintf(rep, "%s\t%s\t%s\n", m->path, sym_lib(m, si), name);
        }
        if (want_stub) p = shim_missing_stub(name);
    }
    return p;
}

static void do_relocs(elf_module *m, Elf64_Rela *r, uint64_t sz,
                      shim_lookup_fn shims, FILE *rep) {
    for (uint64_t i = 0; i < sz / sizeof *r; i++, r++) {
        uint64_t *where = (uint64_t *)(r->r_offset + m->bias);
        uint32_t type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
        switch (type) {
        case R_X86_64_NONE: break;
        case R_X86_64_RELATIVE: *where = m->bias + r->r_addend; break;
        case R_X86_64_64:
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: {
            void *p = resolve(m, si, shims, rep, 1);
            *where = (uint64_t)p + (type == R_X86_64_64 ? r->r_addend : 0);
            break;
        }
        case R_X86_64_COPY: {
            void *p = resolve(m, si, shims, rep, 0);
            if (p && p != where) memcpy(where, p, m->symtab[si].st_size);
            break;
        }
        case R_X86_64_DTPMOD64:
            // Symbols are defined in this module (checked for halo.so), so the id is ours.
            m->tls_relocs++;
            *where = (uint64_t)m->tls_id;
            break;
        case R_X86_64_DTPOFF64:
            m->tls_relocs++;
            *where = (si ? m->symtab[si].st_value : 0) + r->r_addend;
            break;
        case R_X86_64_TPOFF64:
            // Initial-exec TLS through %fs: not supported (none in game/halo.so).
            m->tls_relocs++;
            fprintf(rep, "%s\tRELOC\tTPOFF64 unsupported\n", m->path);
            break;
        default:
            fprintf(rep, "%s\tRELOC\tunknown type %u\n", m->path, type);
        }
    }
}

// --- TLS (general-dynamic model) ---
// Each module with PT_TLS gets an id 1..MAX_TLS_MODS. Each thread keeps a
// small pointer array (in a Win32 TLS slot); blocks are made on first use
// from the module's PT_TLS image.
#define MAX_TLS_MODS 16
static elf_module *g_tls_mods[MAX_TLS_MODS + 1];
static int g_ntls;
static DWORD g_tls_slot = TLS_OUT_OF_INDEXES;

static void tls_assign_ids(void) {
    if (g_tls_slot == TLS_OUT_OF_INDEXES) g_tls_slot = TlsAlloc();
    for (int k = 0; k < g_nmods; k++) {
        elf_module *m = g_mods[k];
        if (m->tls.p_type != PT_TLS || m->tls_id) continue;
        if (g_ntls >= MAX_TLS_MODS) { fprintf(stderr, "ravenloader: too many TLS modules\n"); exit(1); }
        m->tls_id = ++g_ntls;
        g_tls_mods[m->tls_id] = m;
    }
}

typedef struct { uint64_t module, offset; } tls_index;

__attribute__((sysv_abi)) void *elf_tls_get_addr(void *arg) {
    tls_index *ti = arg;
    void **blocks = TlsGetValue(g_tls_slot);
    if (!blocks) {
        blocks = calloc(MAX_TLS_MODS + 1, sizeof *blocks);
        TlsSetValue(g_tls_slot, blocks);
    }
    uint64_t id = ti->module;
    if (id == 0 || id > (uint64_t)g_ntls) {
        fprintf(stderr, "ravenloader: bad TLS module id %llu\n", (unsigned long long)id);
        exit(3);
    }
    if (!blocks[id]) {
        elf_module *m = g_tls_mods[id];
        size_t align = m->tls.p_align > 16 ? m->tls.p_align : 16;
        unsigned char *b = _aligned_malloc(m->tls.p_memsz ? m->tls.p_memsz : 1, align);
        memcpy(b, (void *)(m->tls.p_vaddr + m->bias), m->tls.p_filesz);
        memset(b + m->tls.p_filesz, 0, m->tls.p_memsz - m->tls.p_filesz);
        blocks[id] = b;
    }
    return (unsigned char *)blocks[id] + ti->offset;
}

int elf_relocate_all(shim_lookup_fn shims, FILE *rep) {
    tls_assign_ids();
    for (int k = 0; k < g_nmods; k++) {
        elf_module *m = g_mods[k];
        do_relocs(m, m->rela, m->relasz, shims, rep);
        do_relocs(m, m->jmprel, m->pltrelsz, shims, rep);
    }
    return g_nmissing;
}

void elf_report(FILE *f) {
    for (int k = 0; k < g_nmods; k++) {
        elf_module *m = g_mods[k];
        fprintf(f, "# %s base=%p size=0x%llx entry=%p tls=%s(0x%llx) "
                   "tls_relocs=%d fs_prefix_hits~%d syscall_hits~%d\n",
                m->path, m->base, (unsigned long long)m->size, (void *)m->entry,
                m->tls.p_type ? "yes" : "no", (unsigned long long)m->tls.p_memsz,
                m->tls_relocs, m->fs_hits, m->syscall_hits);
        for (Elf64_Dyn *d = m->dyn; d && d->d_tag != DT_NULL; d++)
            if (d->d_tag == DT_NEEDED)
                fprintf(f, "#   needs %s\n", m->strtab + d->d_un.d_val);
    }
}
