// Locale + wide-char functions libstdc++ needs. Only the "C" locale exists:
// every locale_t is the same glibc-layout struct, and the *_l variants
// ignore their locale argument.
#include "shim.h"
#include <windows.h>
#include <io.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

typedef uint32_t lwchar;

// glibc struct __locale_struct: 13 category pointers, then the ctype tables
// libstdc++ reads directly, then 13 name pointers.
typedef struct {
    void *locales[13];
    const unsigned short *ctype_b;
    const int32_t *ctype_tolower, *ctype_toupper;
    const char *names[13];
} lx_locale;
static lx_locale g_cloc;

static void init_cloc(void) {
    if (g_cloc.ctype_b) return;
    typedef SV const void **(*loc_fn)(void);
    g_cloc.ctype_b = *(const unsigned short **)((loc_fn)shim_lookup("__ctype_b_loc"))();
    g_cloc.ctype_tolower = *(const int32_t **)((loc_fn)shim_lookup("__ctype_tolower_loc"))();
    g_cloc.ctype_toupper = *(const int32_t **)((loc_fn)shim_lookup("__ctype_toupper_loc"))();
    for (int i = 0; i < 13; i++) g_cloc.names[i] = "C";
}
static SV lx_locale *s_newlocale(int mask, const char *name, lx_locale *base) {
    (void)mask; (void)base;
    init_cloc();
    if (name && *name && strcmp(name, "C") && strcmp(name, "POSIX"))
        raven_log("newlocale(%s): only C locale available\n", name);
    return &g_cloc;
}
static SV lx_locale *s_duplocale(lx_locale *l) { (void)l; init_cloc(); return &g_cloc; }
static SV void s_freelocale(lx_locale *l) { (void)l; }
static __thread lx_locale *t_loc = (lx_locale *)-1;  // LC_GLOBAL_LOCALE
static SV lx_locale *s_uselocale(lx_locale *l) {
    lx_locale *prev = t_loc;
    if (l) t_loc = l;
    return prev;
}
// nl_langinfo items are (category << 16) | index.
static SV const char *s_nl_langinfo_l(int item, void *l) {
    (void)l;
    switch (item) {
    case 0x10000: return ".";               // RADIXCHAR
    case 0x10001: return "";                // THOUSEP
    case 14: return "ANSI_X3.4-1968";       // CODESET
    }
    return "";
}
static SV double s_strtod_l(const char *s, char **e, void *l) { (void)l; return strtod(s, e); }
static SV float s_strtof_l(const char *s, char **e, void *l) { (void)l; return strtof(s, e); }
static SV long double s_strtold_l(const char *s, char **e, void *l) { (void)l; return strtold(s, e); }
static SV long double s_frexpl(long double x, int *e) { return frexpl(x, e); }
static SV int s_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcmp(a, b); }
static SV size_t s_strxfrm_l(char *d, const char *s, size_t n, void *l) {
    (void)l;
    size_t len = strlen(s);
    if (n) { size_t m = len < n ? len + 1 : n; memcpy(d, s, m); if (m == n) d[n - 1] = 0; }
    return len;
}
typedef struct { int v[9]; long long gmtoff; const char *zone; } lx_tm;
static SV size_t s_strftime_l(char *b, size_t n, const char *f, const lx_tm *tm, void *l) {
    (void)l;
    struct tm w;
    memcpy(&w, tm->v, sizeof tm->v);
    return strftime(b, n, f, &w);
}
static SV size_t s_wcsftime_l(lwchar *b, size_t n, const lwchar *f, const lx_tm *tm, void *l) {
    char nf[256], out[512];
    size_t i = 0;
    for (; f[i] && i + 1 < sizeof nf; i++) nf[i] = f[i] < 128 ? (char)f[i] : '?';
    nf[i] = 0;
    size_t r = s_strftime_l(out, sizeof out, nf, tm, l);
    if (r + 1 > n) return 0;
    for (size_t k = 0; k <= r; k++) b[k] = (unsigned char)out[k];
    return r;
}
static SV int s_wcscoll_l(const lwchar *a, const lwchar *b, void *l) {
    (void)l;
    while (*a && *a == *b) a++, b++;
    return *a < *b ? -1 : *a > *b;
}
static SV size_t s_wcsxfrm_l(lwchar *d, const lwchar *s, size_t n, void *l) {
    (void)l;
    size_t len = 0;
    while (s[len]) len++;
    for (size_t i = 0; i < n && i <= len; i++) d[i] = s[i];
    return len;
}
static SV unsigned s_towupper_l(unsigned c, void *l) { (void)l; return c < 0x10000 ? (unsigned)towupper((wint_t)c) : c; }
static SV unsigned s_towlower_l(unsigned c, void *l) { (void)l; return c < 0x10000 ? (unsigned)towlower((wint_t)c) : c; }
// wctype: class ids 1..12 in this order.
static const char *g_wct[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph",
                              "lower", "print", "punct", "space", "upper", "xdigit", NULL};
static SV unsigned long long s_wctype_l(const char *name, void *l) {
    (void)l;
    for (int i = 0; g_wct[i]; i++) if (!strcmp(name, g_wct[i])) return (unsigned long long)i + 1;
    return 0;
}
static SV int s_iswctype_l(unsigned c, unsigned long long d, void *l) {
    (void)l;
    if (c >= 0x10000) return d == 1 || d == 2 || d == 6 || d == 8;
    wint_t w = (wint_t)c;
    switch (d) {
    case 1: return iswalnum(w); case 2: return iswalpha(w); case 3: return c == ' ' || c == '\t';
    case 4: return iswcntrl(w); case 5: return c >= '0' && c <= '9'; case 6: return iswgraph(w);
    case 7: return iswlower(w); case 8: return iswprint(w); case 9: return iswpunct(w);
    case 10: return iswspace(w); case 11: return iswupper(w); case 12: return iswxdigit(w);
    }
    return 0;
}
static SV size_t s_mb_cur_max(void) { return 1; }

// ---- C-locale multibyte: one byte = one wide char ----
static SV size_t s_mbrtowc(lwchar *pwc, const char *s, size_t n, void *ps) {
    (void)ps;
    if (!s) return 0;
    if (!n) return (size_t)-2;
    if (pwc) *pwc = (unsigned char)*s;
    return *s ? 1 : 0;
}
static SV size_t s_wcrtomb(char *s, lwchar wc, void *ps) {
    (void)ps;
    if (!s) return 1;
    if (wc > 0xff) { lx_errno = 84; return (size_t)-1; }  // EILSEQ
    *s = (char)wc;
    return 1;
}
static SV unsigned s_btowc(int c) { return c == -1 ? 0xffffffffu : (unsigned char)c; }
static SV int s_wctob(unsigned c) { return c <= 0xff ? (int)c : -1; }
static SV size_t s_mbsnrtowcs(lwchar *d, const char **src, size_t nms, size_t len, void *ps) {
    (void)ps;
    const unsigned char *s = (const unsigned char *)*src;
    size_t i = 0;
    for (; i < nms && (!d || i < len); i++) {
        if (d) d[i] = s[i];
        if (!s[i]) { if (d) *src = NULL; return i; }
    }
    if (d) *src = (const char *)s + i;
    return i;
}
static SV size_t s_mbsrtowcs(lwchar *d, const char **src, size_t len, void *ps) {
    return s_mbsnrtowcs(d, src, (size_t)-1, len, ps);
}
static SV size_t s_wcsnrtombs(char *d, const lwchar **src, size_t nwc, size_t len, void *ps) {
    (void)ps;
    const lwchar *s = *src;
    size_t i = 0;
    for (; i < nwc && (!d || i < len); i++) {
        if (s[i] > 0xff) { lx_errno = 84; return (size_t)-1; }
        if (d) d[i] = (char)s[i];
        if (!s[i]) { if (d) *src = NULL; return i; }
    }
    if (d) *src = s + i;
    return i;
}
static SV lwchar *s_wmemcpy(lwchar *d, const lwchar *s, size_t n) { return memcpy(d, s, n * 4); }
static SV lwchar *s_wmemmove(lwchar *d, const lwchar *s, size_t n) { return memmove(d, s, n * 4); }
static SV lwchar *s_wmemset(lwchar *d, lwchar c, size_t n) { for (size_t i = 0; i < n; i++) d[i] = c; return d; }
static SV int s_wmemcmp(const lwchar *a, const lwchar *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static SV lwchar *s_wmemchr(const lwchar *s, lwchar c, size_t n) {
    for (size_t i = 0; i < n; i++) if (s[i] == c) return (lwchar *)s + i;
    return NULL;
}
static SV unsigned s_putwc(unsigned c, FILE *f) { return fputc(c <= 0xff ? (int)c : '?', f) == EOF ? 0xffffffffu : c; }
static SV unsigned s_getwc(FILE *f) { int c = fgetc(f); return c == EOF ? 0xffffffffu : (unsigned)c; }
static SV unsigned s_ungetwc(unsigned c, FILE *f) { return ungetc((int)c, f) == EOF ? 0xffffffffu : c; }
static SV int s_putc(int c, FILE *f) { return fputc(c, f); }
static SV int s_getc(FILE *f) { return fgetc(f); }
static SV int s_isspace(int c) { return c >= -128 && c < 256 && isspace(c & 0xff); }

// ---- gettext: no translations ----
static SV const char *s_gettext(const char *m) { return m; }
static SV const char *s_dgettext(const char *d, const char *m) { (void)d; return m; }
static SV const char *s_bindtextdomain(const char *d, const char *dir) { (void)d; return dir ? dir : "/usr/share/locale"; }
static SV const char *s_bind_codeset(const char *d, const char *c) { (void)d; return c; }

// ---- misc ----
static SV int s_get_nprocs(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
typedef struct { void *base; size_t len; } lx_iovec;
static SV long long s_writev(int fd, const lx_iovec *iov, int n) {
    long long t = 0;
    for (int i = 0; i < n; i++) {
        int r = _write(fd, iov[i].base, (unsigned)iov[i].len);
        if (r < 0) return t ? t : -1;
        t += r;
    }
    return t;
}
// Thread-local destructors: run at process exit (good enough for the game's threads).
static SV int s_cxa_thread_atexit_impl(void *fn, void *obj, void *dso) {
    typedef SV int (*cxa_fn)(void *, void *, void *);
    return ((cxa_fn)shim_lookup("__cxa_atexit"))(fn, obj, dso);
}

const shim_entry shims_locale[] = {
    {"__newlocale", s_newlocale}, {"newlocale", s_newlocale},
    {"__duplocale", s_duplocale}, {"__freelocale", s_freelocale},
    {"__uselocale", s_uselocale}, {"uselocale", s_uselocale},
    {"__nl_langinfo_l", s_nl_langinfo_l},
    {"__strtod_l", s_strtod_l}, {"__strtof_l", s_strtof_l}, {"strtold_l", s_strtold_l},
    {"__strtold_l", s_strtold_l}, {"frexpl", s_frexpl},
    {"__strcoll_l", s_strcoll_l}, {"__strxfrm_l", s_strxfrm_l},
    {"__strftime_l", s_strftime_l}, {"__wcsftime_l", s_wcsftime_l},
    {"__wcscoll_l", s_wcscoll_l}, {"__wcsxfrm_l", s_wcsxfrm_l},
    {"__towupper_l", s_towupper_l}, {"__towlower_l", s_towlower_l},
    {"__wctype_l", s_wctype_l}, {"__iswctype_l", s_iswctype_l},
    {"__ctype_get_mb_cur_max", s_mb_cur_max},
    {"mbrtowc", s_mbrtowc}, {"wcrtomb", s_wcrtomb}, {"btowc", s_btowc}, {"wctob", s_wctob},
    {"mbsrtowcs", s_mbsrtowcs}, {"mbsnrtowcs", s_mbsnrtowcs}, {"wcsnrtombs", s_wcsnrtombs},
    {"wmemcpy", s_wmemcpy}, {"wmemmove", s_wmemmove}, {"wmemset", s_wmemset},
    {"wmemcmp", s_wmemcmp}, {"wmemchr", s_wmemchr},
    {"putwc", s_putwc}, {"getwc", s_getwc}, {"ungetwc", s_ungetwc},
    {"putc", s_putc}, {"getc", s_getc}, {"isspace", s_isspace},
    {"gettext", s_gettext}, {"dgettext", s_dgettext},
    {"bindtextdomain", s_bindtextdomain}, {"bind_textdomain_codeset", s_bind_codeset},
    {"get_nprocs", s_get_nprocs}, {"writev", s_writev},
    {"__cxa_thread_atexit_impl", s_cxa_thread_atexit_impl},
    {NULL, NULL},
};
