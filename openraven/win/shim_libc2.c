// More libc/libm shims: math, strings, 32-bit wchar_t, stdio extras, time,
// environment, and "not available" stubs (termios, sockets, timerfd, ...).
#include "shim.h"
#include <windows.h>
#include <io.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

typedef uint32_t lwchar;  // Linux wchar_t
typedef struct { long long sec, nsec; } lx_timespec;

// ---- math ----------------------------------------------------------------
#define M1(n) static SV double s_##n(double x) { return n(x); }
#define M1F(n) static SV float s_##n(float x) { return n(x); }
#define M2(n) static SV double s_##n(double x, double y) { return n(x, y); }
#define M2F(n) static SV float s_##n(float x, float y) { return n(x, y); }
M1(sin) M1(cos) M1(tan) M1(asin) M1(acos) M1(atan) M1(exp) M1(log) M1(log10)
M1(sqrt) M1(floor) M1(ceil) M1(trunc) M1(rint)
M1F(sinf) M1F(cosf) M1F(tanf) M1F(asinf) M1F(acosf) M1F(atanf) M1F(expf) M1F(logf)
M1F(sqrtf) M1F(floorf) M1F(ceilf) M1F(roundf) M1F(log10f)
static SV double s_ldexp(double x, int n) { return ldexp(x, n); }
static SV float s_ldexpf(float x, int n) { return ldexpf(x, n); }
M2(atan2) M2(pow) M2(fmod) M2F(atan2f) M2F(powf) M2F(fmodf)
static SV double s_modf(double x, double *i) { return modf(x, i); }
static SV float s_modff(float x, float *i) { return modff(x, i); }
static SV double s_scalbn(double x, int n) { return scalbn(x, n); }
static SV void s_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
static SV void s_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
// glibc values: FP_NAN 0, FP_INFINITE 1, FP_ZERO 2, FP_SUBNORMAL 3, FP_NORMAL 4
static SV int s_fpclassify(double x) {
    switch (fpclassify(x)) {
    case FP_NAN: return 0; case FP_INFINITE: return 1; case FP_ZERO: return 2;
    case FP_SUBNORMAL: return 3; default: return 4;
    }
}
static SV int s_isinf(double x) { return isinf(x) ? (x > 0 ? 1 : -1) : 0; }
static SV double s_powidf2(double a, int b) {
    int neg = b < 0;
    double r = 1;
    for (unsigned n = neg ? -(unsigned)b : (unsigned)b; n; n >>= 1, a *= a)
        if (n & 1) r *= a;
    return neg ? 1 / r : r;
}
static SV unsigned __int128 s_udivti3(unsigned __int128 a, unsigned __int128 b) { return a / b; }

// ---- strings -------------------------------------------------------------
static SV void *s_memchr(const void *s, int c, size_t n) { return memchr(s, c, n); }
static SV char *s_strrchr(const char *s, int c) { return strrchr(s, c); }
static SV int s_strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }
static SV char *s_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static SV size_t s_strnlen(const char *s, size_t n) { return strnlen(s, n); }
static SV char *s_strpbrk(const char *s, const char *a) { return strpbrk(s, a); }
static SV char *s_strcasestr(const char *h, const char *n) {
    size_t l = strlen(n);
    for (; *h; h++) if (!_strnicmp(h, n, l)) return (char *)h;
    return l ? NULL : (char *)h;
}
static SV char *s_stpcpy(char *d, const char *s) { size_t l = strlen(s); memcpy(d, s, l + 1); return d + l; }
static SV char *s_strtok(char *s, const char *d) { return strtok(s, d); }
static SV char *s_strtok_r(char *s, const char *d, char **save) { return strtok_s(s, d, save); }
static SV double s_strtod(const char *s, char **e) { return strtod(s, e); }
static SV long long s_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
static SV int s_tolower(int c) { return tolower(c); }
static SV int s_toupper(int c) { return toupper(c); }
// glibc tables are int32, indexable from -128 to 255.
static int32_t g_lower[384], g_upper[384];
static const int32_t *g_lower_p, *g_upper_p;
static void init_case(void) {
    if (g_lower_p) return;
    for (int i = -128; i < 256; i++) {
        int c = i & 0xff;
        g_lower[i + 128] = (c >= 'A' && c <= 'Z') ? c + 32 : (i < 0 ? i : c);
        g_upper[i + 128] = (c >= 'a' && c <= 'z') ? c - 32 : (i < 0 ? i : c);
    }
    g_lower_p = g_lower + 128;
    g_upper_p = g_upper + 128;
}
static SV const int32_t **s_ctype_tolower_loc(void) { init_case(); return &g_lower_p; }
static SV const int32_t **s_ctype_toupper_loc(void) { init_case(); return &g_upper_p; }
// _chk variants: just do the work.
static SV void *s_memmove_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memmove(d, s, n); }
static SV void *s_memset_chk(void *d, int c, size_t n, size_t dl) { (void)dl; return memset(d, c, n); }
static SV char *s_strcpy_chk(char *d, const char *s, size_t dl) { (void)dl; return strcpy(d, s); }
static SV char *s_stpcpy_chk(char *d, const char *s, size_t dl) { (void)dl; return s_stpcpy(d, s); }
static SV char *s_strcat_chk(char *d, const char *s, size_t dl) { (void)dl; return strcat(d, s); }
static SV char *s_strncpy_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncpy(d, s, n); }
static SV char *s_strncat_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncat(d, s, n); }
static SV long long s_fdelt_chk(long long d) { return d / 64; }

// ---- 32-bit wchar_t ------------------------------------------------------
static SV size_t s_wcslen(const lwchar *s) { size_t n = 0; while (s[n]) n++; return n; }
static SV int s_wcscmp(const lwchar *a, const lwchar *b) {
    while (*a && *a == *b) a++, b++;
    return *a < *b ? -1 : *a > *b;
}
static SV int s_wcsncmp(const lwchar *a, const lwchar *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
    return 0;
}
static lwchar wlow(lwchar c) { return c < 0x10000 ? (lwchar)towlower((wint_t)c) : c; }
static lwchar wup(lwchar c) { return c < 0x10000 ? (lwchar)towupper((wint_t)c) : c; }
static SV int s_wcscasecmp(const lwchar *a, const lwchar *b) {
    while (*a && wlow(*a) == wlow(*b)) a++, b++;
    lwchar x = wlow(*a), y = wlow(*b);
    return x < y ? -1 : x > y;
}
static SV lwchar *s_wcscpy(lwchar *d, const lwchar *s) { lwchar *r = d; while ((*d++ = *s++)); return r; }
static SV lwchar *s_wcsncpy(lwchar *d, const lwchar *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
static SV lwchar *s_wcscat(lwchar *d, const lwchar *s) { s_wcscpy(d + s_wcslen(d), s); return d; }
static SV lwchar *s_wcsncat(lwchar *d, const lwchar *s, size_t n) {
    lwchar *p = d + s_wcslen(d);
    while (n-- && *s) *p++ = *s++;
    *p = 0;
    return d;
}
static SV lwchar *s_wcschr(const lwchar *s, lwchar c) {
    for (;; s++) { if (*s == c) return (lwchar *)s; if (!*s) return NULL; }
}
static SV lwchar *s_wcsstr(const lwchar *h, const lwchar *n) {
    size_t l = s_wcslen(n);
    for (; *h; h++) if (!s_wcsncmp(h, n, l)) return (lwchar *)h;
    return l ? NULL : (lwchar *)h;
}
static SV lwchar *s_wcstok(lwchar *s, const lwchar *d, lwchar **save) {
    if (!s) s = *save;
    while (*s && s_wcschr(d, *s)) s++;
    if (!*s) { *save = s; return NULL; }
    lwchar *t = s;
    while (*s && !s_wcschr(d, *s)) s++;
    if (*s) *s++ = 0;
    *save = s;
    return t;
}
static SV lwchar *s_wcscpy_chk(lwchar *d, const lwchar *s, size_t dl) { (void)dl; return s_wcscpy(d, s); }
static SV lwchar *s_wcscat_chk(lwchar *d, const lwchar *s, size_t dl) { (void)dl; return s_wcscat(d, s); }
static SV lwchar *s_wcsncat_chk(lwchar *d, const lwchar *s, size_t n, size_t dl) { (void)dl; return s_wcsncat(d, s, n); }
static SV unsigned s_towlower(unsigned c) { return wlow(c); }
static SV unsigned s_towupper(unsigned c) { return wup(c); }
static SV int s_iswalpha(unsigned c) { return c < 0x10000 ? iswalpha((wint_t)c) : 1; }
static SV int s_iswdigit(unsigned c) { return c >= '0' && c <= '9'; }
// Numeric parsing: narrow to ASCII, parse, map end pointer back.
static size_t narrow(const lwchar *s, char *buf, size_t cap) {
    size_t i = 0;
    for (; i + 1 < cap && s[i] && s[i] < 128; i++) buf[i] = (char)s[i];
    buf[i] = 0;
    return i;
}
#define WNUM(name, type, fn, ...)                                                   \
    static SV type s_##name(const lwchar *s, lwchar **e, ##__VA_ARGS__) {             \
        char b[128], *ne;                                                            \
        narrow(s, b, sizeof b);                                                      \
        type r = fn;                                                                 \
        if (e) *e = (lwchar *)s + (ne - b);                                          \
        return r;                                                                    \
    }
WNUM(wcstol, long long, strtoll(b, &ne, base), int base)
WNUM(wcstoll, long long, strtoll(b, &ne, base), int base)
WNUM(wcstoul, unsigned long long, strtoull(b, &ne, base), int base)
WNUM(wcstoull, unsigned long long, strtoull(b, &ne, base), int base)
WNUM(wcstod, double, strtod(b, &ne))
// Multibyte <-> wide: UTF-8.
static SV size_t s_mbstowcs(lwchar *d, const char *s, size_t n) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && (!d || o < n)) {
        lwchar c = *p++;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra) c &= 0x3f >> extra;
        while (extra-- && (*p & 0xc0) == 0x80) c = c << 6 | (*p++ & 0x3f);
        if (d) d[o] = c;
        o++;
    }
    if (d && o < n) d[o] = 0;
    return o;
}
static SV size_t s_wcstombs(char *d, const lwchar *s, size_t n) {
    size_t o = 0;
    for (; *s; s++) {
        char t[4];
        int k;
        lwchar c = *s;
        if (c < 0x80) { t[0] = (char)c; k = 1; }
        else if (c < 0x800) { t[0] = (char)(0xc0 | c >> 6); t[1] = (char)(0x80 | (c & 0x3f)); k = 2; }
        else if (c < 0x10000) { t[0] = (char)(0xe0 | c >> 12); t[1] = (char)(0x80 | (c >> 6 & 0x3f)); t[2] = (char)(0x80 | (c & 0x3f)); k = 3; }
        else { t[0] = (char)(0xf0 | c >> 18); t[1] = (char)(0x80 | (c >> 12 & 0x3f)); t[2] = (char)(0x80 | (c >> 6 & 0x3f)); t[3] = (char)(0x80 | (c & 0x3f)); k = 4; }
        if (d && o + k > n) break;
        if (d) memcpy(d + o, t, k);
        o += k;
    }
    if (d && o < n) d[o] = 0;
    return o;
}

// ---- stdio extras --------------------------------------------------------
static SV int s_feof(FILE *f) { return feof(f); }
static SV int s_ferror(FILE *f) { return ferror(f); }
static SV int s_fflush(FILE *f) { return fflush(f); }
static SV int s_fputs(const char *s, FILE *f) { return fputs(s, f); }
static SV int s_puts(const char *s) { return puts(s); }
static SV int s_putchar(int c) { return putchar(c); }
static SV int s_ungetc(int c, FILE *f) { return ungetc(c, f); }
static SV void s_rewind(FILE *f) { rewind(f); }
static SV int s_getc(FILE *f) { return fgetc(f); }
static SV FILE *s_fdopen(int fd, const char *m) {
    char mode[16];
    snprintf(mode, sizeof mode, "%s%s", m, strchr(m, 'b') ? "" : "b");
    return _fdopen(fd, mode);
}
// Linux fpos_t is 16 bytes; we keep the offset in the first 8.
static SV int s_fgetpos(FILE *f, long long *pos) { *pos = _ftelli64(f); return *pos < 0 ? -1 : 0; }
static SV int s_fsetpos(FILE *f, const long long *pos) { return _fseeki64(f, *pos, SEEK_SET); }
static SV int s_fseeko(FILE *f, long long o, int w) { return _fseeki64(f, o, w); }
static SV long long s_ftello(FILE *f) { return _ftelli64(f); }
static SV size_t s_fread_chk(void *b, size_t bl, size_t s, size_t n, FILE *f) { (void)bl; return fread(b, s, n, f); }
static SV long long s_getline(char **line, size_t *cap, FILE *f) {
    size_t n = 0;
    int c;
    if (!*line || !*cap) { *cap = 128; *line = realloc(*line, *cap); }
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap) { *cap *= 2; *line = realloc(*line, *cap); }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (!n) return -1;
    (*line)[n] = 0;
    return (long long)n;
}

// ---- sort / random -------------------------------------------------------
typedef SV int (*sv_cmp)(const void *, const void *);
static __thread sv_cmp t_cmp;
static int qsort_tramp(const void *a, const void *b) { return t_cmp(a, b); }
static SV void s_qsort(void *b, size_t n, size_t sz, sv_cmp cmp) {
    sv_cmp prev = t_cmp;
    t_cmp = cmp;
    qsort(b, n, sz, qsort_tramp);
    t_cmp = prev;
}
// glibc RAND_MAX is 2^31-1.
static unsigned long long g_rand = 1;
static SV void s_srand(unsigned s) { g_rand = s; }
static SV int s_rand(void) {
    g_rand = g_rand * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)(g_rand >> 33);
}
static SV int s_rand_r(unsigned *seed) {
    *seed = *seed * 1103515245 + 12345;
    return (int)((*seed >> 1) & 0x7fffffff);
}

// ---- environment / process -----------------------------------------------
static SV int s_setenv(const char *n, const char *v, int over) {
    raven_log("setenv(%s=%s, %d)\n", n, v, over);
    if (!over && getenv(n)) return 0;
    return _putenv_s(n, v) ? -1 : 0;
}
static SV int s_unsetenv(const char *n) { raven_log("unsetenv(%s)\n", n); return _putenv_s(n, "") ? -1 : 0; }
static SV char *s_secure_getenv(const char *n) { return getenv(n); }
// The game thinks it runs in /pm/g7/halo (start scripts cd there).
static SV char *s_getcwd(char *b, size_t n) {
    const char *cwd = "/pm/g7/halo";
    if (!b) return _strdup(cwd);
    if (n < strlen(cwd) + 1) { lx_errno = 34; return NULL; }
    strcpy(b, cwd);
    return b;
}
static SV int s_chdir(const char *p) { raven_log("chdir(%s) ignored\n", p); return 0; }
static SV int s_dup(int fd) { return _dup(fd); }
static SV void s_abort(void) { raven_log("FATAL: abort() called\n"); ExitProcess(6); }
static SV void s_assert_fail(const char *e, const char *file, unsigned line, const char *fn) {
    raven_log("FATAL: assert %s failed (%s:%u %s)\n", e, file, line, fn ? fn : "");
    ExitProcess(6);
}
static SV int s_system(const char *c) { raven_log("system(%s) ignored\n", c ? c : "(null)"); return c ? -1 : 0; }
static SV void *s_popen(const char *c, const char *m) { (void)m; raven_log("popen(%s) refused\n", c); lx_errno = LX_ENOSYS; return NULL; }
static SV int s_pclose(void *f) { (void)f; return -1; }
static SV int s_execlp(const char *f) { raven_log("execlp(%s) refused\n", f); lx_errno = LX_ENOSYS; return -1; }
static SV int s_zero(void) { return 0; }
static SV int s_enosys(void) { lx_errno = LX_ENOSYS; return -1; }
static SV int s_enotty(void) { lx_errno = LX_ENOTTY; return -1; }
static SV void *s_null(void) { return NULL; }
static SV int s_backtrace(void **b, int n) { (void)b; (void)n; return 0; }
static int g_h_errno;
static SV int *s_h_errno_location(void) { return &g_h_errno; }
static SV long long s_iconv_open(void) { lx_errno = LX_EINVAL; return -1; }
static SV const char *s_setlocale(int c, const char *l) { (void)c; (void)l; return "C"; }
static SV int s_sigemptyset(uint64_t *s) { memset(s, 0, 128); return 0; }
static SV int s_sigaddset(uint64_t *s, int n) { if (n > 0 && n <= 1024) s[(n - 1) / 64] |= 1ULL << ((n - 1) % 64); return 0; }
// struct statvfs (112 bytes): report lots of free space.
static SV int s_statvfs(const char *p, uint64_t *o) {
    (void)p;
    memset(o, 0, 112);
    o[0] = o[1] = 4096;                  // bsize, frsize
    o[2] = o[3] = o[4] = 1ULL << 28;     // blocks, bfree, bavail
    o[5] = o[6] = o[7] = 1 << 20;        // files, ffree, favail
    o[10] = 255;                         // namemax
    return 0;
}
static SV void *s_aligned_alloc(size_t al, size_t n) {
    if (al > 16) raven_log("aligned_alloc(%zu) > 16 not supported, using malloc\n", al);
    return malloc(n);
}

// ---- time ----------------------------------------------------------------
static SV int s_clock_gettime(int clk, lx_timespec *ts) {
    if (clk == 0 || clk == 8) {  // REALTIME, REALTIME_ALARM
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL;
        ts->sec = (long long)(t / 10000000);
        ts->nsec = (long long)(t % 10000000) * 100;
    } else {  // MONOTONIC and friends
        static LARGE_INTEGER freq;
        LARGE_INTEGER c;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&c);
        ts->sec = c.QuadPart / freq.QuadPart;
        ts->nsec = (c.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart;
    }
    return 0;
}
static SV int s_clock_getres(int clk, lx_timespec *ts) { (void)clk; if (ts) { ts->sec = 0; ts->nsec = 100; } return 0; }
static SV int s_timespec_get(lx_timespec *ts, int base) { s_clock_gettime(0, ts); return base; }
// Linux struct tm = 9 ints + long gmtoff + char *zone (56 bytes).
typedef struct { int v[9]; long long gmtoff; const char *zone; } lx_tm;
static __thread lx_tm t_tm;
static SV lx_tm *s_localtime(const long long *t) {
    struct tm w;
    time_t tt = (time_t)*t;
    if (localtime_s(&w, &tt)) return NULL;
    memcpy(t_tm.v, &w, sizeof t_tm.v);
    t_tm.gmtoff = 0;
    t_tm.zone = "UTC";
    return &t_tm;
}
static SV char *s_asctime_r(const lx_tm *tm, char *buf) {
    struct tm w;
    memcpy(&w, tm->v, sizeof tm->v);
    asctime_s(buf, 26, &w);
    return buf;
}
static SV size_t s_strftime(char *b, size_t n, const char *f, const lx_tm *tm) {
    struct tm w;
    memcpy(&w, tm->v, sizeof tm->v);
    return strftime(b, n, f, &w);
}
static SV void s_tzset(void) { _tzset(); }

const shim_entry shims_libc2[] = {
#define E(n) {#n, s_##n}
    E(sin), E(cos), E(tan), E(asin), E(acos), E(atan), E(exp), E(log), E(log10),
    E(sqrt), E(floor), E(ceil), E(trunc), E(rint),
    E(sinf), E(cosf), E(tanf), E(asinf), E(acosf), E(atanf), E(expf), E(logf),
    E(sqrtf), E(floorf), E(ceilf), E(roundf), E(log10f), E(ldexp), E(ldexpf),
    E(atan2), E(pow), E(fmod), E(atan2f), E(powf), E(fmodf),
    E(modf), E(modff), E(scalbn), E(sincos), E(sincosf),
    {"__fpclassify", s_fpclassify}, {"__isinf", s_isinf},
    {"__powidf2", s_powidf2}, {"__udivti3", s_udivti3},
    E(memchr), E(strrchr), E(strncasecmp), E(strncat), E(strnlen), E(strpbrk),
    E(strcasestr), E(stpcpy), E(strtok), {"__strtok_r", s_strtok_r}, E(strtod), E(strtoll),
    E(tolower), E(toupper),
    {"__ctype_tolower_loc", s_ctype_tolower_loc}, {"__ctype_toupper_loc", s_ctype_toupper_loc},
    {"__memmove_chk", s_memmove_chk}, {"__memset_chk", s_memset_chk},
    {"__strcpy_chk", s_strcpy_chk}, {"__stpcpy_chk", s_stpcpy_chk},
    {"__strcat_chk", s_strcat_chk}, {"__strncpy_chk", s_strncpy_chk},
    {"__strncat_chk", s_strncat_chk}, {"__fdelt_chk", s_fdelt_chk},
    E(wcslen), E(wcscmp), E(wcsncmp), E(wcscasecmp), E(wcscpy), E(wcsncpy), E(wcscat),
    E(wcsncat), E(wcschr), E(wcsstr), E(wcstok),
    {"__wcscpy_chk", s_wcscpy_chk}, {"__wcscat_chk", s_wcscat_chk}, {"__wcsncat_chk", s_wcsncat_chk},
    E(towlower), E(towupper), E(iswalpha), E(iswdigit),
    E(wcstol), E(wcstoll), E(wcstoul), E(wcstoull), E(wcstod), E(mbstowcs), E(wcstombs),
    E(feof), E(ferror), E(fflush), E(fputs), E(puts), E(putchar), E(ungetc), E(rewind),
    {"_IO_getc", s_getc}, E(fdopen), E(fgetpos), E(fsetpos),
    {"fseeko64", s_fseeko}, {"ftello64", s_ftello}, {"__fread_chk", s_fread_chk}, E(getline),
    E(qsort), E(srand), E(rand), E(rand_r),
    E(setenv), E(unsetenv), E(secure_getenv), E(getcwd), E(chdir), E(dup), E(abort),
    {"__assert_fail", s_assert_fail}, E(system), E(popen), E(pclose), E(execlp),
    {"nice", s_zero}, {"setpriority", s_zero}, {"sync", s_zero}, {"flock", s_zero},
    {"symlink", s_enosys}, {"settimeofday", s_zero},
    E(backtrace), {"backtrace_symbols", s_null}, {"__h_errno_location", s_h_errno_location},
    E(iconv_open), {"iconv", s_iconv_open}, {"iconv_close", s_zero}, E(setlocale),
    {"gethostbyname_r", s_enosys}, {"inet_pton", s_zero},
    {"timerfd_create", s_enosys}, {"timerfd_settime", s_enosys},
    {"tcgetattr", s_enotty}, {"tcsetattr", s_enotty}, {"tcflush", s_enotty}, {"tcdrain", s_enotty},
    {"cfgetispeed", s_zero}, {"cfsetispeed", s_zero}, {"cfsetospeed", s_zero},
    E(sigemptyset), E(sigaddset), {"statvfs64", s_statvfs}, E(aligned_alloc),
    E(clock_gettime), E(clock_getres), E(timespec_get), E(localtime), E(asctime_r),
    E(strftime), E(tzset),
#undef E
    {NULL, NULL},
};
