// Core libc shims: memory, strings, errno, stdio, fd I/O, paths, time, mmap,
// shared memory, process info. Linux paths are remapped under the game root.
#include "shim.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
void raven_time_trace(const char *fn, void *ra);
#define TT(name) raven_time_trace(name, __builtin_return_address(0))
#include <time.h>
#include <ctype.h>

// ---- logging, errno, paths ---------------------------------------------------

// Raw Win32 handle, so the game's close()/fclose() on CRT fds can never take the log down.
static HANDLE g_log = INVALID_HANDLE_VALUE;
void raven_log(const char *fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE)
        g_log = CreateFileA(getenv("RAVEN_LOG") ? getenv("RAVEN_LOG") : "ravenloader.log", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    DWORD wr;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, buf, (DWORD)n, &wr, NULL);
    else fwrite(buf, 1, (size_t)n, stderr);
}

static __thread int t_errno;
int *lx_errno_ptr(void) { return &t_errno; }
int lx_errno_from_crt(int e) {
    switch (e) {
    case ENOSYS: return LX_ENOSYS;
    case ETIMEDOUT: return LX_ETIMEDOUT;
    case EWOULDBLOCK: return LX_EAGAIN;
    default: return e;  // 1..34 match between UCRT and Linux
    }
}
void lx_set_errno_from_crt(void) { t_errno = lx_errno_from_crt(errno); }
static SV int *s_errno_location(void) { return &t_errno; }

char g_root[MAX_PATH];
void raven_set_root(const char *root) {
    GetFullPathNameA(root, MAX_PATH, g_root, NULL);
    for (char *c = g_root; *c; c++) if (*c == '\\') *c = '/';
    size_t n = strlen(g_root);
    if (n && g_root[n - 1] == '/') g_root[n - 1] = 0;
}

// Absolute Linux paths map under the game root (where pm/ lives).
// /proc/cpuinfo: written once at startup into <root>/ravenloader_proc/.
static void write_cpuinfo(void) {
    char dir[MAX_PATH * 2], path[MAX_PATH * 2], name[128] = "Unknown CPU";
    DWORD mhz = 3000, sz = sizeof name, sz2 = sizeof mhz;
    HKEY k;
    if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k)) {
        RegQueryValueExA(k, "ProcessorNameString", NULL, NULL, (BYTE *)name, &sz);
        RegQueryValueExA(k, "~MHz", NULL, NULL, (BYTE *)&mhz, &sz2);
        RegCloseKey(k);
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    snprintf(dir, sizeof dir, "%s/ravenloader_proc", g_root);
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s/cpuinfo", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (DWORD i = 0; i < si.dwNumberOfProcessors; i++)
        fprintf(f, "processor\t: %lu\nvendor_id\t: %s\nmodel name\t: %s\ncpu MHz\t\t: %lu.000\n"
                   "cache size\t: 512 KB\ncpu cores\t: %lu\nflags\t\t: fpu sse sse2 ssse3 sse4_1 sse4_2 avx avx2\n\n",
                i, strstr(name, "AMD") ? "AuthenticAMD" : "GenuineIntel", name, mhz,
                si.dwNumberOfProcessors);
    fclose(f);
}

// Online coop: every PC must start from the same save files (operator settings, audits, high scores),
// or the game copies drift apart. So in coop the game's save folder pmuser/ is swapped for an empty
// folder <root>/coop_saves/pmuser that is wiped at every start. The player's real saves stay untouched.
static void rm_tree(const char *dir) {
    char pat[MAX_PATH * 2], sub[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s/*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(sub, sizeof sub, "%s/%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(sub);
            else { SetFileAttributesA(sub, FILE_ATTRIBUTE_NORMAL); DeleteFileA(sub); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}
static char g_coop_dir[MAX_PATH * 2];
static int g_coop_saves;
// main.c calls this once graphics.ini has set RAVEN_COOP (ravengfx_install), before the game runs.
void raven_coop_saves_init(void) {
    {
        const char *c = raven_env("RAVEN_COOP"), *k = raven_env("RAVEN_COOP_KEEPSAVES");
        int on = c && *c && strcmp(c, "off") && !(k && *k == '1');
        if (on) {
            // Host and joiners get their own folder, so two copies on one PC do not share one.
            char d[MAX_PATH * 2];
            // RAVEN_COOP_SAVENAME=x: own folder name, for several joiners on one PC (coop_saves_x).
            const char *sn = raven_env("RAVEN_COOP_SAVENAME");
            snprintf(g_coop_dir, sizeof g_coop_dir, "%s/coop_saves_%s", g_root,
                     sn && *sn ? sn : strncmp(c, "host", 4) ? "join" : "host");
            rm_tree(g_coop_dir);
            CreateDirectoryA(g_coop_dir, NULL);
            snprintf(d, sizeof d, "%s/pmuser", g_coop_dir);
            CreateDirectoryA(d, NULL);
            raven_log("[coop] using a fresh save folder %s (your normal saves are not touched)\n", d);
        }
        g_coop_saves = on;
    }
}

const char *lx_path(const char *p, char *buf, size_t cap) {
    if (!p) return NULL;
    if (g_coop_saves) {
        const char *rest = NULL;
        if (!strncmp(p, "/pm/g7/halo/pmuser", 18) && (!p[18] || p[18] == '/')) rest = p + 18;
        else if (!strncmp(p, "pmuser", 6) && (!p[6] || p[6] == '/')) rest = p + 6;
        else if (!strncmp(p, "./pmuser", 8) && (!p[8] || p[8] == '/')) rest = p + 8;
        if (rest) { snprintf(buf, cap, "%s/pmuser%s", g_coop_dir, rest); return buf; }
    }
    if (!strcmp(p, "/proc/cpuinfo")) snprintf(buf, cap, "%s/ravenloader_proc/cpuinfo", g_root);
    else if (p[0] == '/') snprintf(buf, cap, "%s%s", g_root, p);
    else snprintf(buf, cap, "%s", p);
    return buf;
}
#define PATH(p) char _pb_##p[MAX_PATH * 2]; const char *w_##p = lx_path(p, _pb_##p, sizeof _pb_##p)

// ---- missing-function stubs --------------------------------------------------

static SV void missing_called(const char *name) {
    raven_log("FATAL: unimplemented function called: %s\n", name);
    fprintf(stderr, "ravenloader: unimplemented function called: %s\n", name);
    ExitProcess(3);
}

static unsigned char *g_stub_pool;
static size_t g_stub_used;
void *shim_missing_stub(const char *name) {
    if (!g_stub_pool || g_stub_used + 32 > 0x10000) {
        g_stub_pool = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        g_stub_used = 0;
    }
    unsigned char *c = g_stub_pool + g_stub_used;
    g_stub_used += 32;
    char *nm = _strdup(name);
    c[0] = 0x48; c[1] = 0xbf; memcpy(c + 2, &nm, 8);                // mov rdi, name
    void *fn = (void *)missing_called;
    c[10] = 0x48; c[11] = 0xb8; memcpy(c + 12, &fn, 8);             // mov rax, fn
    c[20] = 0xff; c[21] = 0xe0;                                     // jmp rax
    return c;
}

// Simple failing stubs for things the arcade build can live without.
static SV long long fail_neg(void) { t_errno = LX_ENOSYS; return -1; }
static SV void *fail_null(void) { t_errno = LX_ENOSYS; return NULL; }
static SV int ok_zero(void) { return 0; }
static SV void nop_void(void) {}

// ---- memory / strings ----------------------------------------------------------

static SV void *s_malloc(size_t n) { return malloc(n); }
static SV void *s_calloc(size_t a, size_t b) { return calloc(a, b); }
static SV void *s_realloc(void *p, size_t n) { return realloc(p, n); }
static SV void s_free(void *p) { free(p); }
static SV void *s_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static SV void *s_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static SV void *s_memcpy_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memcpy(d, s, n); }
static SV int s_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static SV size_t s_strlen(const char *s) { return strlen(s); }
static SV int s_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static SV int s_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static SV int s_strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
static SV char *s_strcpy(char *d, const char *s) { return strcpy(d, s); }
static SV char *s_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static SV char *s_strcat(char *d, const char *s) { return strcat(d, s); }
static SV char *s_strchr(const char *s, int c) { return strchr(s, c); }
static SV char *s_strstr(const char *a, const char *b) { return strstr(a, b); }
static SV char *s_strdup(const char *s) { return _strdup(s); }
static SV char *s_strtok_r(char *s, const char *d, char **sv) { return strtok_s(s, d, sv); }
static SV char *s_strerror(int e) { return strerror(e); }
static SV long long s_strtol(const char *s, char **e, int b) { return strtoll(s, e, b); }
static SV unsigned long long s_strtoul(const char *s, char **e, int b) { return strtoull(s, e, b); }
static SV long long s_atol(const char *s) { return atoll(s); }

// glibc ctype table: 384 entries, pointer aimed at index 128 (so -128..255 work).
static unsigned short g_ctype[384];
static const unsigned short *g_ctype_ptr = g_ctype + 128;
static void init_ctype(void) {
    for (int c = -128; c < 256; c++) {
        unsigned short f = 0;
        if (c >= 0 && c < 128) {
            if (isupper(c)) f |= 0x100;
            if (islower(c)) f |= 0x200;
            if (isalpha(c)) f |= 0x400;
            if (isdigit(c)) f |= 0x800;
            if (isxdigit(c)) f |= 0x1000;
            if (isspace(c)) f |= 0x2000;
            if (isprint(c)) f |= 0x4000;
            if (isgraph(c)) f |= 0x8000;
            if (c == ' ' || c == '\t') f |= 0x1;
            if (iscntrl(c)) f |= 0x2;
            if (ispunct(c)) f |= 0x4;
            if (isalnum(c)) f |= 0x8;
        }
        g_ctype[c + 128] = f;
    }
}
static SV const unsigned short **s_ctype_b_loc(void) { return &g_ctype_ptr; }

static SV char *s_getenv(const char *n) { return getenv(n); }
static SV void s_stack_chk_fail(void) {
    raven_log("FATAL: stack smashing detected\n");
    ExitProcess(4);
}

// ---- stdio ---------------------------------------------------------------------

// Hybrid 4-player mode (RAVEN_WIDE4=hybrid): the game runs at the screen's 16:9 shape, so play
// the 16:9 cutscene movies the 2-player cabinet uses (X_1080.webm, 1920x1080; X_wide.webm -> X.webm)
// instead of the 32:9 ones. raven_movie_open tells the presenter a movie is on screen.
volatile int raven_movie_open;
static FILE *movie_f;
static int hybrid16(void) {
    static int on = -1;
    if (on < 0) { const char *e = raven_env("RAVEN_WIDE4"); on = e && !strcmp(e, "hybrid"); }
    return on;
}
static const char *movie16(const char *p, char *buf, size_t cap) {
    size_t n = strlen(p);
    if (!hybrid16() || n < 6 || n + 8 > cap || !strstr(p, "/webm/") || strcmp(p + n - 5, ".webm")) return p;
    if (n > 10 && !strcmp(p + n - 10, "_wide.webm")) snprintf(buf, cap, "%.*s.webm", (int)(n - 10), p);
    else if (n > 10 && !strcmp(p + n - 10, "_1080.webm")) return p;
    else snprintf(buf, cap, "%.*s_1080.webm", (int)(n - 5), p);
    char wb[MAX_PATH * 2];
    const char *w = lx_path(buf, wb, sizeof wb);
    return GetFileAttributesA(w) != INVALID_FILE_ATTRIBUTES ? buf : p;
}
static SV FILE *s_fopen(const char *p, const char *m) {
    char mb[1024];
    p = movie16(p, mb, sizeof mb);
    PATH(p);
    char mode[16];
    snprintf(mode, sizeof mode, "%s%s", m, strchr(m, 'b') ? "" : "b");
    FILE *f = fopen(w_p, mode);
    if (!f) lx_set_errno_from_crt();
    raven_log("fopen(%s, %s) -> %p\n", p, m, (void *)f);
    if (f && strchr(m, 'b') && strstr(p, "/webm/")) { movie_f = f; raven_movie_open = 1; }
    return f;
}
static SV int s_fclose(FILE *f) {
    if (f && f == movie_f) { movie_f = NULL; raven_movie_open = 0; }
    return fclose(f);
}
static SV size_t s_fread(void *b, size_t s, size_t n, FILE *f) { return fread(b, s, n, f); }
static SV size_t s_fwrite(const void *b, size_t s, size_t n, FILE *f) { return fwrite(b, s, n, f); }
static SV int s_fseek(FILE *f, long long o, int w) { return _fseeki64(f, o, w); }
static SV long long s_ftell(FILE *f) { return _ftelli64(f); }
static SV int s_fgetc(FILE *f) { return fgetc(f); }
static SV int s_fputc(int c, FILE *f) { return fputc(c, f); }
static SV char *s_fgets(char *b, int n, FILE *f) { return fgets(b, n, f); }
static SV int s_fileno(FILE *f) { return _fileno(f); }
static SV int s_setvbuf(FILE *f, char *b, int mode, size_t n) {
    int m = mode == 2 ? _IONBF : mode == 1 ? _IOLBF : _IOFBF;
    if (m == _IOLBF) m = _IOFBF;  // UCRT treats _IOLBF as full buffering anyway
    return setvbuf(f, b, m, n ? n : 512);
}
static SV void s_perror(const char *s) { fprintf(stderr, "%s: %s\n", s ? s : "", strerror(t_errno)); }

// ---- fd I/O --------------------------------------------------------------------

static int open_flags(int lf) {
    int f = _O_BINARY;
    switch (lf & 3) { case 0: f |= _O_RDONLY; break; case 1: f |= _O_WRONLY; break; default: f |= _O_RDWR; }
    if (lf & 0100) f |= _O_CREAT;
    if (lf & 0200) f |= _O_EXCL;
    if (lf & 01000) f |= _O_TRUNC;
    if (lf & 02000) f |= _O_APPEND;
    return f;
}
static SV int s_open(const char *p, int flags, int mode) {
    PATH(p);
    int fd = _open(w_p, open_flags(flags), (mode & 0200) ? _S_IREAD | _S_IWRITE : _S_IREAD);
    if (fd < 0) lx_set_errno_from_crt();
    raven_log("open(%s, 0%o) -> %d\n", p, flags, fd);
    return fd;
}
static SV long long s_read(int fd, void *b, size_t n) { int r = _read(fd, b, (unsigned)n); if (r < 0) lx_set_errno_from_crt(); return r; }
static SV long long s_write(int fd, const void *b, size_t n) { int r = _write(fd, b, (unsigned)n); if (r < 0) lx_set_errno_from_crt(); return r; }
static SV int s_close(int fd) { if (fd <= 4) raven_log("close(%d)\n", fd); return _close(fd); }
static SV long long s_lseek(int fd, long long o, int w) { return _lseeki64(fd, o, w); }
static SV int s_ftruncate(int fd, long long n) { return _chsize_s(fd, n) ? -1 : 0; }
static SV int s_fdatasync(int fd) { return _commit(fd); }
static SV int s_pipe(int fds[2]) { return _pipe(fds, 65536, _O_BINARY); }
static SV int s_access(const char *p, int m) {
    PATH(p);
    int r = _access(w_p, m & 6);
    if (r) lx_set_errno_from_crt();
    raven_log("access(%s) -> %d\n", p, r);
    return r;
}
static SV int s_mkdir(const char *p, int m) { (void)m; PATH(p); int r = _mkdir(w_p); if (r) lx_set_errno_from_crt(); return r; }
static SV int s_rmdir(const char *p) { PATH(p); return _rmdir(w_p); }
static SV int s_unlink(const char *p) { PATH(p); int r = _unlink(w_p); if (r) lx_set_errno_from_crt(); return r; }
static SV int s_rename(const char *a, const char *b) {
    PATH(a); PATH(b);
    return MoveFileExA(w_a, w_b, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
static SV int s_umask(int m) { (void)m; return 022; }
static SV long long s_readlink(const char *p, char *buf, size_t n) {
    const char *r = NULL;
    if (!strcmp(p, "/proc/self/exe")) r = "/pm/g7/halo/game";
    raven_log("readlink(%s)\n", p);
    if (!r) { t_errno = LX_ENOENT; return -1; }
    size_t l = strlen(r);
    if (l > n) l = n;
    memcpy(buf, r, l);
    return (long long)l;
}

// struct stat on x86-64 Linux (144 bytes).
typedef struct {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime_, ctime_ns;
    int64_t reserved[3];
} lx_stat;
static void fill_stat(lx_stat *o, const struct _stat64 *s) {
    memset(o, 0, sizeof *o);
    o->dev = s->st_dev;
    o->ino = s->st_ino;
    o->nlink = s->st_nlink;
    o->mode = s->st_mode;  // S_IFDIR/S_IFREG bits match Linux octal values
    if (o->mode & 0400) o->mode |= 0044;
    if (o->mode & 0100000) o->mode |= 0;
    o->size = s->st_size;
    o->blksize = 4096;
    o->blocks = (s->st_size + 511) / 512;
    o->atime = s->st_atime;
    o->mtime = s->st_mtime;
    o->ctime_ = s->st_ctime;
}
static SV int s_xstat(int ver, const char *p, lx_stat *o) {
    (void)ver;
    PATH(p);
    struct _stat64 s;
    if (_stat64(w_p, &s)) { lx_set_errno_from_crt(); raven_log("stat(%s) -> ENOENT\n", p); return -1; }
    fill_stat(o, &s);
    return 0;
}
static SV int s_fxstat(int ver, int fd, lx_stat *o) {
    (void)ver;
    struct _stat64 s;
    if (_fstat64(fd, &s)) { lx_set_errno_from_crt(); return -1; }
    fill_stat(o, &s);
    return 0;
}

// Linux struct dirent: ino, off, reclen(u16), type(u8), name[256].
typedef struct { uint64_t ino; int64_t off; uint16_t reclen; uint8_t type; char name[256]; } lx_dirent;
typedef struct { HANDLE h; WIN32_FIND_DATAA fd; int first; lx_dirent ent; } lx_dir;
static SV lx_dir *s_opendir(const char *p) {
    PATH(p);
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s/*", w_p);
    lx_dir *d = calloc(1, sizeof *d);
    d->h = FindFirstFileA(pat, &d->fd);
    raven_log("opendir(%s) -> %s\n", p, d->h == INVALID_HANDLE_VALUE ? "fail" : "ok");
    if (d->h == INVALID_HANDLE_VALUE) { free(d); t_errno = LX_ENOENT; return NULL; }
    d->first = 1;
    return d;
}
static SV lx_dirent *s_readdir(lx_dir *d) {
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return NULL;
    d->first = 0;
    memset(&d->ent, 0, sizeof d->ent);
    d->ent.ino = 1;
    d->ent.reclen = sizeof d->ent;
    d->ent.type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;
    snprintf(d->ent.name, 256, "%s", d->fd.cFileName);
    return &d->ent;
}
static SV int s_closedir(lx_dir *d) {
    if (!d) { lx_errno = LX_EBADF; return -1; }  // game closes failed /dev scans
    FindClose(d->h);
    free(d);
    return 0;
}

// ---- memory mapping ------------------------------------------------------------

static DWORD prot_win(int p) {
    if (p & 4) return (p & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (p & 2) return PAGE_READWRITE;
    if (p & 1) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
static SV void *s_mmap(void *addr, size_t len, int prot, int flags, int fd, long long off) {
    void *p = VirtualAlloc((flags & 0x10) ? addr : NULL, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) { t_errno = LX_ENOMEM; return (void *)-1; }
    if (!(flags & 0x20) && fd >= 0) {  // file-backed: copy contents (private map)
        long long cur = _lseeki64(fd, 0, SEEK_CUR);
        _lseeki64(fd, off, SEEK_SET);
        _read(fd, p, (unsigned)len);
        _lseeki64(fd, cur, SEEK_SET);
    }
    DWORD old;
    VirtualProtect(p, len, prot_win(prot), &old);
    raven_log("mmap(%p, 0x%zx, prot=%d, flags=0x%x, fd=%d) -> %p\n", addr, len, prot, flags, fd, p);
    return p;
}
static SV int s_mprotect(void *a, size_t n, int prot) {
    DWORD old;
    return VirtualProtect(a, n, prot_win(prot), &old) ? 0 : -1;
}
// munmap: only whole regions from our mmap can be released; partial unmaps are decommitted.
static SV int s_munmap(void *a, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(a, &mi, sizeof mi) && mi.AllocationBase == a && mi.RegionSize >= n)
        if (VirtualFree(a, 0, MEM_RELEASE)) return 0;
    VirtualFree(a, n, MEM_DECOMMIT);
    return 0;
}
// mincore: Linux returns ENOMEM if any page is unmapped; else 1 per page.
static SV int s_mincore(void *a, size_t n, unsigned char *vec) {
    uintptr_t p = (uintptr_t)a & ~(uintptr_t)0xfff, end = (uintptr_t)a + n;
    for (size_t i = 0; p < end; p += 0x1000, i++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)p, &mi, sizeof mi) || mi.State != MEM_COMMIT) {
            lx_errno = LX_ENOMEM;
            return -1;
        }
        vec[i] = 1;
    }
    return 0;
}

// SysV shared memory, process-local (the arcade IO uses it between threads).
typedef struct { int key; size_t size; void *mem; } shm_seg;
static shm_seg g_shm[64];
static int g_nshm;
static SV int s_shmget(int key, size_t size, int flags) {
    (void)flags;
    for (int i = 0; i < g_nshm; i++) if (g_shm[i].key == key && key) return i;
    if (g_nshm == 64) return -1;
    g_shm[g_nshm] = (shm_seg){key, size, calloc(1, size)};
    raven_log("shmget(0x%x, %zu) -> %d\n", key, size, g_nshm);
    return g_nshm++;
}
static SV void *s_shmat(int id, const void *a, int f) { (void)a; (void)f; return id >= 0 && id < g_nshm ? g_shm[id].mem : (void *)-1; }

// ---- time ----------------------------------------------------------------------

typedef struct { int sec, min, hour, mday, mon, year, wday, yday, isdst, pad; long long gmtoff; const char *zone; } lx_tm;
static void tm_out(lx_tm *o, const struct tm *t) {
    memset(o, 0, sizeof *o);
    memcpy(o, t, 9 * sizeof(int));
    o->zone = "UTC";
}
int coop_vtime_us(uint64_t *us);   // coop_win.c: virtual wall clock, 0 = off
static SV long long s_time(long long *t) { TT("time"); uint64_t vu; long long v = coop_vtime_us(&vu) ? (long long)(vu / 1000000) : _time64(NULL); if (t) *t = v; return v; }
static SV lx_tm *s_localtime_r(const long long *t, lx_tm *o) { struct tm w; __time64_t v = *t; _localtime64_s(&w, &v); tm_out(o, &w); return o; }
static SV lx_tm *s_gmtime_r(const long long *t, lx_tm *o) { struct tm w; __time64_t v = *t; _gmtime64_s(&w, &v); tm_out(o, &w); return o; }
static lx_tm g_tm;
static SV lx_tm *s_gmtime(const long long *t) { return s_gmtime_r(t, &g_tm); }
static SV long long s_mktime(lx_tm *o) {
    struct tm w;
    memcpy(&w, o, 9 * sizeof(int));
    long long r = _mktime64(&w);
    memcpy(o, &w, 9 * sizeof(int));
    return r;
}
// RAVEN_TIMETRACE=1: count time-function calls per caller, dump every 10 s (coop determinism work).
#include "elfload.h"
#define TT_N 512
static struct { const char *fn; void *ra; volatile long n; } g_tt[TT_N];
static int g_tt_on = -1;
static SRWLOCK g_tt_lock = SRWLOCK_INIT;
void raven_time_trace_enable(int on) { g_tt_on = on; }
void raven_time_trace(const char *fn, void *ra) {
    if (g_tt_on <= 0) return;
    static ULONGLONG last;
    AcquireSRWLockExclusive(&g_tt_lock);
    unsigned h = (unsigned)(((uintptr_t)ra >> 2) ^ (uintptr_t)fn) % TT_N;
    for (int k = 0; k < TT_N; k++, h = (h + 1) % TT_N) {
        if (!g_tt[h].ra) { g_tt[h].fn = fn; g_tt[h].ra = ra; }
        if (g_tt[h].ra == ra && g_tt[h].fn == fn) { g_tt[h].n++; break; }
    }
    ULONGLONG now = GetTickCount64();
    if (!last) last = now;
    if (now - last >= 10000) {
        last = now;
        raven_log("[timetrace] --- per 10s ---\n");
        for (int i = 0; i < TT_N; i++)
            if (g_tt[i].ra && g_tt[i].n) {
                raven_log("[timetrace] %-14s %6ld  %s\n", g_tt[i].fn, g_tt[i].n, elf_module_name(g_tt[i].ra));
                g_tt[i].n = 0;
            }
    }
    ReleaseSRWLockExclusive(&g_tt_lock);
}

static SV long long s_clock(void) { TT("clock"); return (long long)clock() * (1000000 / CLOCKS_PER_SEC); }

typedef struct { long long tv_sec, tv_usec; } lx_timeval;
static SV int s_gettimeofday(lx_timeval *tv, void *tz) {
    (void)tz; TT("gettimeofday");
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10 - 11644473600000000ULL;
    uint64_t vu;
    if (coop_vtime_us(&vu)) t = vu;
    if (tv) { tv->tv_sec = t / 1000000; tv->tv_usec = t % 1000000; }
    return 0;
}

typedef struct { long long sec, nsec; } lx_timespec;
static SV int s_nanosleep(const lx_timespec *r, lx_timespec *rem) {
    TT("nanosleep");
    if (rem) rem->sec = rem->nsec = 0;
    long long ms = r->sec * 1000 + r->nsec / 1000000;
    extern void raven_thread_blk(int kind);
    extern int raven_gate_sleep(unsigned us);
    extern void raven_gate_main_sleep(void);
    if (raven_gate_sleep(r->sec ? 1000000u : (unsigned)(r->nsec / 1000))) return 0;
    raven_gate_main_sleep();
    { extern void raven_gate_main_sleep_us(unsigned); raven_gate_main_sleep_us(r->sec ? 1000000u * (unsigned)r->sec : (unsigned)(r->nsec / 1000)); }
    raven_thread_blk(4);
    Sleep(ms > 0 ? (DWORD)ms : (r->nsec ? 1 : 0));
    raven_thread_blk(0);
    return 0;
}
void raven_thread_blk(int kind);   // shim_thread.c
int raven_gate_sleep(unsigned us);   // shim_thread.c: coop gate
void raven_gate_main_sleep(void);
static SV int s_usleep(unsigned us) {
    TT("usleep");
    if (raven_gate_sleep(us)) return 0;
    raven_gate_main_sleep();
    { extern void raven_gate_main_sleep_us(unsigned); raven_gate_main_sleep_us(us); }
    raven_thread_blk(4); Sleep(us >= 1000 ? us / 1000 : (us ? 1 : 0)); raven_thread_blk(0);
    return 0;
}
static SV unsigned s_sleep(unsigned s) { Sleep(s * 1000); return 0; }
static SV int s_sched_yield(void) { SwitchToThread(); return 0; }

// ---- process / system info ------------------------------------------------------

static SV int s_getpid(void) { return (int)GetCurrentProcessId(); }
static SV int s_getppid(void) { return 1; }
static SV int s_getuid(void) { return 0; }
// atexit / __cxa_atexit handlers, run in reverse order by exit().
typedef struct { SV void (*fn)(void *); void *arg; } exit_handler;
static exit_handler g_atexit[1024];
static volatile LONG g_natexit;
static SV int s_cxa_atexit(SV void (*fn)(void *), void *arg, void *dso) {
    (void)dso;
    LONG i = InterlockedIncrement(&g_natexit) - 1;
    if (i >= 1024) return -1;
    g_atexit[i].fn = fn;
    g_atexit[i].arg = arg;
    return 0;
}
static SV int s_atexit(SV void (*fn)(void)) { return s_cxa_atexit((SV void (*)(void *))fn, NULL, NULL); }
static SV void s_cxa_finalize(void *dso) { (void)dso; }
static SV void s_exit(int c) {
    raven_log("exit(%d)\n", c);
    for (LONG i = g_natexit - 1; i >= 0; i--)
        if (i < 1024 && g_atexit[i].fn) { void (*f)(void *) SV = g_atexit[i].fn; g_atexit[i].fn = NULL; f(g_atexit[i].arg); }
    extern void coopnet_shutdown(void);
    coopnet_shutdown();   // tell the other PCs, close the router port
    fflush(NULL);
    ExitProcess(c);
}
static SV void s__exit(int c) { extern void coopnet_shutdown(void); raven_log("_exit(%d)\n", c); coopnet_shutdown(); fflush(NULL); ExitProcess(c); }
static SV long long s_sysconf(int n) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (n) {
    case 2: return 100;                       // _SC_CLK_TCK
    case 30: return 4096;                     // _SC_PAGESIZE
    case 83: case 84: return si.dwNumberOfProcessors;  // NPROCESSORS_CONF/ONLN
    }
    raven_log("sysconf(%d) unknown\n", n);
    return -1;
}
static SV int s_uname(char *u) {
    memset(u, 0, 65 * 6);
    strcpy(u, "Linux");
    strcpy(u + 65, "raven");
    strcpy(u + 130, "4.15.0");
    strcpy(u + 195, "#1 SMP");
    strcpy(u + 260, "x86_64");
    return 0;
}
static SV int s_gethostname(char *b, size_t n) { snprintf(b, n, "raven"); return 0; }
static SV int s_getlogin_r(char *b, size_t n) { snprintf(b, n, "raven"); return 0; }
static SV int s_ttyname_r(int fd, char *b, size_t n) { (void)fd; (void)b; (void)n; return LX_ENOTTY; }

typedef struct { char *name, *passwd; uint32_t uid, gid; char *gecos, *dir, *shell; } lx_passwd;
static lx_passwd g_pw = {"raven", "x", 0, 0, "raven", "/pm/g7/halo", "/bin/sh"};
static SV lx_passwd *s_getpwuid(unsigned uid) { (void)uid; return &g_pw; }
static SV int s_getpwuid_r(unsigned uid, lx_passwd *pw, char *buf, size_t n, lx_passwd **res) {
    (void)uid; (void)buf; (void)n;
    *pw = g_pw;
    *res = pw;
    return 0;
}
static SV long long s_syscall(long long nr, long long a, long long b) {
    (void)a; (void)b;
    if (nr == 186) return GetCurrentThreadId();  // gettid
    raven_log("syscall(%lld) unsupported\n", nr);
    t_errno = LX_ENOSYS;
    return -1;
}
static SV int s_ioctl(int fd, unsigned long long req, void *arg) {
    (void)arg;
    raven_log("ioctl(%d, 0x%llx) unsupported\n", fd, req);
    t_errno = LX_ENOTTY;
    return -1;
}
static SV int s_fcntl(int fd, int cmd, long long arg) {
    (void)arg;
    if (cmd == 1 || cmd == 3) return 0;  // F_GETFD / F_GETFL
    return 0;                            // F_SETFD / F_SETFL: accept silently
    (void)fd;
}
static SV int s_prctl(int op) { (void)op; return 0; }
static SV long long s_ptrace(int req) { (void)req; return 0; }  // "not being traced"
static SV unsigned s_dev_major(unsigned long long d) { return (unsigned)((d >> 8) & 0xfff); }
static SV unsigned s_dev_minor(unsigned long long d) { return (unsigned)((d & 0xff) | ((d >> 12) & 0xfff00)); }
static SV unsigned long long s_dev_makedev(unsigned ma, unsigned mi) { return ((unsigned long long)ma << 8) | mi; }
static SV int s_sigrtmin(void) { return 34; }

void shim_core_init(void) { init_ctype(); write_cpuinfo(); }

const shim_entry shims_core[] = {
    {"__errno_location", s_errno_location},
    {"malloc", s_malloc}, {"calloc", s_calloc}, {"realloc", s_realloc}, {"free", s_free},
    {"memset", s_memset}, {"memmove", s_memmove}, {"memcpy", s_memmove}, {"__memcpy_chk", s_memcpy_chk},
    {"memcmp", s_memcmp}, {"strlen", s_strlen}, {"strcmp", s_strcmp}, {"strncmp", s_strncmp},
    {"strcasecmp", s_strcasecmp}, {"strcpy", s_strcpy}, {"strncpy", s_strncpy}, {"strcat", s_strcat},
    {"strchr", s_strchr}, {"strstr", s_strstr}, {"strdup", s_strdup}, {"strtok_r", s_strtok_r},
    {"strerror", s_strerror}, {"strtol", s_strtol}, {"strtoul", s_strtoul}, {"strtoull", s_strtoul},
    {"atol", s_atol}, {"__ctype_b_loc", s_ctype_b_loc}, {"getenv", s_getenv},
    {"__stack_chk_fail", s_stack_chk_fail},
    {"fopen", s_fopen}, {"fclose", s_fclose}, {"fread", s_fread}, {"fwrite", s_fwrite},
    {"fseek", s_fseek}, {"ftell", s_ftell}, {"fgetc", s_fgetc}, {"fputc", s_fputc}, {"fgets", s_fgets},
    {"fileno", s_fileno}, {"setvbuf", s_setvbuf}, {"perror", s_perror},
    {"open", s_open}, {"read", s_read}, {"write", s_write}, {"close", s_close}, {"lseek", s_lseek},
    {"ftruncate", s_ftruncate}, {"fdatasync", s_fdatasync}, {"pipe", s_pipe}, {"access", s_access},
    {"mkdir", s_mkdir}, {"rmdir", s_rmdir}, {"unlink", s_unlink}, {"rename", s_rename},
    {"umask", s_umask}, {"readlink", s_readlink},
    {"__xstat", s_xstat}, {"__lxstat64", s_xstat}, {"__fxstat", s_fxstat}, {"__fxstat64", s_fxstat},
    {"opendir", s_opendir}, {"readdir", s_readdir}, {"closedir", s_closedir},
    {"mmap", s_mmap}, {"mprotect", s_mprotect}, {"mincore", s_mincore},
    {"mmap64", s_mmap}, {"munmap", s_munmap}, {"lseek64", s_lseek}, {"open64", s_open},
    {"__open_2", s_open}, {"__xstat64", s_xstat}, {"__lxstat", s_xstat}, {"fopen64", s_fopen},
    {"shmget", s_shmget}, {"shmat", s_shmat}, {"shmdt", ok_zero}, {"shmctl", ok_zero},
    {"time", s_time}, {"localtime_r", s_localtime_r}, {"gmtime_r", s_gmtime_r}, {"gmtime", s_gmtime},
    {"mktime", s_mktime}, {"clock", s_clock}, {"gettimeofday", s_gettimeofday},
    {"nanosleep", s_nanosleep}, {"usleep", s_usleep}, {"sleep", s_sleep}, {"sched_yield", s_sched_yield},
    {"getpid", s_getpid}, {"getppid", s_getppid}, {"getuid", s_getuid}, {"geteuid", s_getuid},
    {"getgid", s_getuid}, {"exit", s_exit}, {"__cxa_atexit", s_cxa_atexit}, {"atexit", s_atexit},
    {"__cxa_finalize", s_cxa_finalize},{"_exit", s__exit}, {"sysconf", s_sysconf},
    {"uname", s_uname}, {"gethostname", s_gethostname}, {"getlogin_r", s_getlogin_r},
    {"ttyname_r", s_ttyname_r}, {"getpwuid", s_getpwuid}, {"getpwuid_r", s_getpwuid_r},
    {"syscall", s_syscall}, {"ioctl", s_ioctl}, {"fcntl", s_fcntl}, {"prctl", s_prctl},
    {"ptrace", s_ptrace}, {"gnu_dev_major", s_dev_major}, {"gnu_dev_minor", s_dev_minor},
    {"gnu_dev_makedev", s_dev_makedev}, {"__libc_current_sigrtmin", s_sigrtmin},
    // Signals: no-ops (the arcade build only installs crash handlers).
    {"signal", ok_zero}, {"sigaction", ok_zero}, {"sigfillset", ok_zero}, {"sigdelset", ok_zero},
    {"pthread_sigmask", ok_zero}, {"kill", ok_zero}, {"sigqueue", ok_zero}, {"setrlimit", ok_zero},
    // Not available on Windows; fail like a locked-down Linux box would.
    {"fork", fail_neg}, {"waitpid", fail_neg},
    {"setmntent", fail_null}, {"getmntent", fail_null}, {"endmntent", ok_zero},
    {"setutent", nop_void}, {"getutent_r", fail_neg}, {"endutent", nop_void},
    {"getifaddrs", fail_neg}, {"freeifaddrs", nop_void},
    // Networking: stubbed until a later milestone (arcade link play only).
    {"socket", fail_neg}, {"bind", fail_neg}, {"listen", fail_neg}, {"accept", fail_neg},
    {"connect", fail_neg}, {"send", fail_neg}, {"recv", fail_neg}, {"sendto", fail_neg},
    {"recvfrom", fail_neg}, {"setsockopt", fail_neg}, {"getsockopt", fail_neg},
    {"getsockname", fail_neg}, {"getpeername", fail_neg}, {"shutdown", fail_neg},
    {"select", fail_neg}, {"poll", fail_neg}, {"getaddrinfo", fail_neg}, {"freeaddrinfo", nop_void},
    {"getnameinfo", fail_neg}, {"gethostbyname", fail_null}, {"getservbyname", fail_null},
    {"inet_addr", fail_neg}, {"inet_ntoa", fail_null}, {"inet_ntop", fail_null},
    {NULL, NULL},
};

const char *raven_env(const char *name) {
    static char buf[16][256];
    static volatile LONG slot;
    char *b = buf[InterlockedIncrement(&slot) & 15];
    DWORD n = GetEnvironmentVariableA(name, b, 256);
    return n && n < 256 ? b : NULL;
}
