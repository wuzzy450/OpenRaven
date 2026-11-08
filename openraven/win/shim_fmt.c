// printf/scanf family for SysV variadic callers.
// clang can't do va_start in a sysv_abi function on Windows, so each variadic
// entry point is a small asm thunk that spills the SysV argument registers
// into an sv_args block and calls the C implementation with a pointer to it.
#include "shim.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    uint64_t gp[6];   // rdi rsi rdx rcx r8 r9
    uint64_t fp[8];   // xmm0..xmm7 (low 64 bits)
    uint64_t *stk;    // first stack argument
    int gi, fi;
} sv_args;

static uint64_t next_gp(sv_args *a) { return a->gi < 6 ? a->gp[a->gi++] : *a->stk++; }
static double next_fp(sv_args *a) {
    uint64_t v = a->fi < 8 ? a->fp[a->fi++] : *a->stk++;
    double d;
    memcpy(&d, &v, 8);
    return d;
}

// Thunk layout: frame of 0x80 bytes for sv_args + 0x20 shadow space for the
// ms_abi call. sv_args sits at rsp+0x20.
#define VTHUNK(name, impl)                                                   \
    __asm__(".text\n.globl " #name "\n" #name ":\n"                          \
            "push %rbp\n mov %rsp,%rbp\n sub $0xb0,%rsp\n"                   \
            "mov %rdi,0x20(%rsp)\n mov %rsi,0x28(%rsp)\n mov %rdx,0x30(%rsp)\n" \
            "mov %rcx,0x38(%rsp)\n mov %r8,0x40(%rsp)\n mov %r9,0x48(%rsp)\n" \
            "movq %xmm0,0x50(%rsp)\n movq %xmm1,0x58(%rsp)\n"               \
            "movq %xmm2,0x60(%rsp)\n movq %xmm3,0x68(%rsp)\n"               \
            "movq %xmm4,0x70(%rsp)\n movq %xmm5,0x78(%rsp)\n"               \
            "movq %xmm6,0x80(%rsp)\n movq %xmm7,0x88(%rsp)\n"               \
            "lea 0x10(%rbp),%rax\n mov %rax,0x90(%rsp)\n"                    \
            "movl $0,0x98(%rsp)\n movl $0,0x9c(%rsp)\n"                      \
            "lea 0x20(%rsp),%rcx\n call " #impl "\n"                         \
            "leave\n ret\n");                                                \
    extern char name[];

// ---- formatter -------------------------------------------------------------

typedef struct { char *buf; size_t len, cap; } sbuf;
static void sb_put(sbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2 + 64;
        b->buf = realloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

// Linux wchar_t is 32-bit. Convert a UTF-32 string to UTF-8.
static void sb_put_u32(sbuf *b, const uint32_t *w, int maxn) {
    for (int i = 0; w && w[i] && (maxn < 0 || i < maxn); i++) {
        uint32_t c = w[i];
        char o[4];
        int n;
        if (c < 0x80) { o[0] = (char)c; n = 1; }
        else if (c < 0x800) { o[0] = (char)(0xc0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); n = 2; }
        else if (c < 0x10000) { o[0] = (char)(0xe0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); n = 3; }
        else { o[0] = (char)(0xf0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63)); n = 4; }
        sb_put(b, o, n);
    }
}

// Format with Linux printf semantics, pulling arguments from a.
static char *sv_format(sv_args *a, const char *f, size_t *outlen) {
    sbuf b = {0};
    sb_put(&b, "", 0);
    while (*f) {
        if (*f != '%') {
            const char *s = f;
            while (*f && *f != '%') f++;
            sb_put(&b, s, f - s);
            continue;
        }
        const char *start = f++;
        if (*f == '%') { sb_put(&b, "%", 1); f++; continue; }
        char spec[64];
        int sl = 0;
        spec[sl++] = '%';
        while (*f && strchr("-+ #0'", *f)) { if (*f != '\'') spec[sl++] = *f; f++; }
        if (*f == '*') { sl += snprintf(spec + sl, 16, "%d", (int)next_gp(a)); f++; }
        else while (*f >= '0' && *f <= '9') spec[sl++] = *f++;
        int prec = -1;
        if (*f == '.') {
            spec[sl++] = *f++;
            if (*f == '*') { prec = (int)next_gp(a); sl += snprintf(spec + sl, 16, "%d", prec); f++; }
            else { prec = atoi(f); while (*f >= '0' && *f <= '9') spec[sl++] = *f++; }
        }
        int lng = 0;  // 0 int, 1 short, 2 char, 8 64-bit, 16 long double
        int wide = 0;
        for (;;) {
            if (*f == 'h') { lng = lng == 1 ? 2 : 1; f++; }
            else if (*f == 'l') { lng = 8; wide = 1; f++; }
            else if (*f == 'q' || *f == 'j' || *f == 'z' || *f == 't') { lng = 8; f++; }
            else if (*f == 'L') { lng = 16; f++; }
            else break;
        }
        char conv = *f ? *f++ : 0;
        char tmp[512];
        int n = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            uint64_t v = next_gp(a);
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            int signd = conv == 'd' || conv == 'i';
            long long sv;
            if (lng == 8) sv = (long long)v;
            else if (lng == 1) sv = signd ? (short)v : (unsigned short)v;
            else if (lng == 2) sv = signd ? (signed char)v : (unsigned char)v;
            else sv = signd ? (int)v : (unsigned)v;
            n = snprintf(tmp, sizeof tmp, spec, sv);
            break;
        }
        case 'c': {
            spec[sl++] = 'c'; spec[sl] = 0;
            n = snprintf(tmp, sizeof tmp, spec, (int)next_gp(a));
            break;
        }
        case 'p': {
            n = snprintf(tmp, sizeof tmp, "0x%llx", (unsigned long long)next_gp(a));
            break;
        }
        case 's': case 'S': {
            const void *p = (const void *)next_gp(a);
            if (wide || conv == 'S') {
                sbuf w = {0};
                sb_put(&w, "", 0);
                sb_put_u32(&w, p, prec);
                spec[sl++] = 's'; spec[sl] = 0;
                char *o = NULL;
                int m = snprintf(NULL, 0, spec, w.buf);
                o = malloc(m + 1);
                snprintf(o, m + 1, spec, w.buf);
                sb_put(&b, o, m);
                free(o);
                free(w.buf);
                continue;
            }
            spec[sl++] = 's'; spec[sl] = 0;
            const char *s = p ? p : "(null)";
            int m = snprintf(NULL, 0, spec, s);
            char *o = malloc(m + 1);
            snprintf(o, m + 1, spec, s);
            sb_put(&b, o, m);
            free(o);
            continue;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d;
            if (lng == 16) {  // 80-bit long double, passed in memory (2 slots)
                long double ld;
                memcpy(&ld, a->stk, 10);
                a->stk += 2;
                d = (double)ld;
            } else d = next_fp(a);
            spec[sl++] = conv; spec[sl] = 0;
            n = snprintf(tmp, sizeof tmp, spec, d);
            break;
        }
        case 'n': {
            int *p = (int *)next_gp(a);
            if (p) *p = (int)b.len;
            continue;
        }
        case 'm': {
            n = snprintf(tmp, sizeof tmp, "%s", strerror(lx_errno));
            break;
        }
        default:
            sb_put(&b, start, f - start);
            continue;
        }
        if (n > (int)sizeof tmp - 1) n = sizeof tmp - 1;
        if (n > 0) sb_put(&b, tmp, n);
    }
    *outlen = b.len;
    return b.buf;
}

static int put_bounded(char *dst, size_t cap, const char *s, size_t n) {
    if (cap) {
        size_t m = n < cap - 1 ? n : cap - 1;
        memcpy(dst, s, m);
        dst[m] = 0;
    }
    return (int)n;
}

// ---- implementations (called from thunks, MS ABI) --------------------------

int impl_printf_chk(sv_args *a) {  // (int flag, fmt, ...)
    next_gp(a);
    const char *f = (const char *)next_gp(a);
    size_t n;
    char *s = sv_format(a, f, &n);
    fwrite(s, 1, n, stdout);
    free(s);
    return (int)n;
}
int impl_printf(sv_args *a) {
    const char *f = (const char *)next_gp(a);
    size_t n;
    char *s = sv_format(a, f, &n);
    fwrite(s, 1, n, stdout);
    free(s);
    return (int)n;
}
int impl_fprintf(sv_args *a) {  // (FILE*, fmt, ...)
    FILE *fp = (FILE *)next_gp(a);
    const char *f = (const char *)next_gp(a);
    size_t n;
    char *s = sv_format(a, f, &n);
    fwrite(s, 1, n, fp);
    free(s);
    return (int)n;
}
int impl_fwprintf(sv_args *a) {  // (FILE*, const wchar_t32 *fmt, ...)
    FILE *fp = (FILE *)next_gp(a);
    const uint32_t *wf = (const uint32_t *)next_gp(a);
    sbuf fb = {0};
    sb_put(&fb, "", 0);
    sb_put_u32(&fb, wf, -1);
    size_t n;
    char *s = sv_format(a, fb.buf, &n);
    fwrite(s, 1, n, fp);
    free(s);
    free(fb.buf);
    return (int)n;
}
int impl_fwprintf_chk(sv_args *a) {  // (FILE*, int flag, const wchar_t32 *fmt, ...)
    FILE *fp = (FILE *)next_gp(a);
    next_gp(a);
    const uint32_t *wf = (const uint32_t *)next_gp(a);
    sbuf fb = {0};
    sb_put(&fb, "", 0);
    sb_put_u32(&fb, wf, -1);
    size_t n;
    char *s = sv_format(a, fb.buf, &n);
    fwrite(s, 1, n, fp);
    free(s);
    free(fb.buf);
    return (int)n;
}
// Format a 32-bit wide format into dst (cap wide chars). Like glibc: -1 if it does not fit.
static int wformat_into(sv_args *a, uint32_t *dst, size_t cap, const uint32_t *wf) {
    sbuf fb = {0};
    sb_put(&fb, "", 0);
    sb_put_u32(&fb, wf, -1);
    size_t n;
    unsigned char *s = (unsigned char *)sv_format(a, fb.buf, &n);
    free(fb.buf);
    size_t o = 0;
    int fit = 1;
    for (size_t i = 0; i < n;) {   // UTF-8 -> UTF-32
        uint32_t c = s[i];
        int k = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        if (k) c &= 0x3f >> k;
        i++;
        for (int j = 0; j < k && i < n; j++, i++) c = c << 6 | (s[i] & 0x3f);
        if (o + 1 >= cap) { fit = 0; break; }
        dst[o++] = c;
    }
    if (cap) dst[o] = 0;
    free(s);
    return fit ? (int)o : -1;
}
int impl_swprintf(sv_args *a) {  // (wchar_t32 *s, size_t n, fmt, ...)
    uint32_t *d = (uint32_t *)next_gp(a);
    size_t cap = (size_t)next_gp(a);
    return wformat_into(a, d, cap, (const uint32_t *)next_gp(a));
}
int impl_swprintf_chk(sv_args *a) {  // (s, n, flag, slen, fmt, ...)
    uint32_t *d = (uint32_t *)next_gp(a);
    size_t cap = (size_t)next_gp(a);
    next_gp(a);
    next_gp(a);
    return wformat_into(a, d, cap, (const uint32_t *)next_gp(a));
}
int impl_snprintf_chk(sv_args *a) {  // (buf, maxlen, flag, slen, fmt, ...)
    char *buf = (char *)next_gp(a);
    size_t cap = (size_t)next_gp(a);
    next_gp(a);
    next_gp(a);
    const char *f = (const char *)next_gp(a);
    size_t n;
    char *s = sv_format(a, f, &n);
    int r = put_bounded(buf, cap, s, n);
    free(s);
    return r;
}
int impl_snprintf(sv_args *a) {  // (buf, maxlen, fmt, ...)
    char *buf = (char *)next_gp(a);
    size_t cap = (size_t)next_gp(a);
    const char *f = (const char *)next_gp(a);
    size_t n;
    char *s = sv_format(a, f, &n);
    int r = put_bounded(buf, cap, s, n);
    free(s);
    return r;
}
int impl_sprintf_chk(sv_args *a) {  // (buf, flag, slen, fmt, ...)
    char *buf = (char *)next_gp(a);
    next_gp(a);
    next_gp(a);
    const char *f = (const char *)next_gp(a);
    size_t n;
    char *s = sv_format(a, f, &n);
    memcpy(buf, s, n + 1);
    free(s);
    return (int)n;
}
int impl_sprintf(sv_args *a) {
    char *buf = (char *)next_gp(a);
    const char *f = (const char *)next_gp(a);
    size_t n;
    char *s = sv_format(a, f, &n);
    memcpy(buf, s, n + 1);
    free(s);
    return (int)n;
}

// sscanf: arguments are all pointers. Rewrite Linux length modifiers for the
// CRT (l on integers = 64-bit on Linux) and forward up to 32 pointers.
int impl_sscanf(sv_args *a) {
    const char *str = (const char *)next_gp(a);
    const char *f = (const char *)next_gp(a);
    char fmt[1024];
    int o = 0, nptr = 0;
    for (const char *p = f; *p && o < (int)sizeof fmt - 4; p++) {
        if (*p != '%') { fmt[o++] = *p; continue; }
        fmt[o++] = *p++;
        if (*p == '%') { fmt[o++] = '%'; continue; }
        int suppress = 0;
        if (*p == '*') { suppress = 1; fmt[o++] = *p++; }
        while (*p >= '0' && *p <= '9') fmt[o++] = *p++;
        int l = 0, h = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 'q' || *p == 'L') {
            if (*p == 'h') h++; else l++;
            p++;
        }
        char c = *p;
        if (strchr("diuoxXn", c)) {
            if (l) { fmt[o++] = 'l'; fmt[o++] = 'l'; }
            else if (h == 1) fmt[o++] = 'h';
            else if (h >= 2) { fmt[o++] = 'h'; fmt[o++] = 'h'; }
        } else if (strchr("feEgGaA", c)) {
            if (l) fmt[o++] = 'l';
        }
        if (c == '[') {
            fmt[o++] = *p++;
            if (*p == '^') fmt[o++] = *p++;
            if (*p == ']') fmt[o++] = *p++;
            while (*p && *p != ']') fmt[o++] = *p++;
            c = *p;
        }
        fmt[o++] = c;
        if (!suppress && c != '%') nptr++;
        if (!*p) break;
    }
    fmt[o] = 0;
    void *pt[32] = {0};
    for (int i = 0; i < nptr && i < 32; i++) pt[i] = (void *)next_gp(a);
    return sscanf(str, fmt, pt[0], pt[1], pt[2], pt[3], pt[4], pt[5], pt[6], pt[7],
                  pt[8], pt[9], pt[10], pt[11], pt[12], pt[13], pt[14], pt[15],
                  pt[16], pt[17], pt[18], pt[19], pt[20], pt[21], pt[22], pt[23],
                  pt[24], pt[25], pt[26], pt[27], pt[28], pt[29], pt[30], pt[31]);
}

// ---- SysV va_list variants (vsnprintf etc.) -----------------------------------
// SysV va_list: {gp_offset, fp_offset, overflow_arg_area, reg_save_area}.
// reg_save_area holds rdi..r9 (48 bytes) then xmm0..7 (16 bytes each).
typedef struct { uint32_t gp_off, fp_off; uint64_t *overflow; unsigned char *save; } sv_valist;
static void from_valist(sv_args *a, sv_valist *v) {
    memset(a, 0, sizeof *a);
    for (int i = 0; i < 6; i++) memcpy(&a->gp[i], v->save + 8 * i, 8);
    for (int i = 0; i < 8; i++) memcpy(&a->fp[i], v->save + 48 + 16 * i, 8);
    a->gi = v->gp_off / 8;
    a->fi = v->fp_off >= 48 ? (v->fp_off - 48) / 16 : 0;
    a->stk = v->overflow;
}
static char *vfmt(const char *f, sv_valist *v, size_t *n) {
    sv_args a;
    from_valist(&a, v);
    return sv_format(&a, f, n);
}
static SV int s_vsnprintf(char *buf, size_t cap, const char *f, sv_valist *v) {
    size_t n; char *s = vfmt(f, v, &n);
    int r = put_bounded(buf, cap, s, n); free(s); return r;
}
static SV int s_vsnprintf_chk(char *buf, size_t cap, int fl, size_t sl, const char *f, sv_valist *v) {
    (void)fl; (void)sl; return s_vsnprintf(buf, cap, f, v);
}
static SV int s_vsprintf_chk(char *buf, int fl, size_t sl, const char *f, sv_valist *v) {
    (void)fl; (void)sl;
    size_t n; char *s = vfmt(f, v, &n);
    memcpy(buf, s, n + 1); free(s); return (int)n;
}
static SV int s_vfprintf(FILE *fp, const char *f, sv_valist *v) {
    size_t n; char *s = vfmt(f, v, &n);
    fwrite(s, 1, n, fp); free(s); return (int)n;
}
static SV int s_vfprintf_chk(FILE *fp, int fl, const char *f, sv_valist *v) { (void)fl; return s_vfprintf(fp, f, v); }

int impl_fprintf_chk(sv_args *a) {  // (FILE*, flag, fmt, ...)
    FILE *fp = (FILE *)next_gp(a);
    next_gp(a);
    const char *f = (const char *)next_gp(a);
    size_t n;
    char *s = sv_format(a, f, &n);
    fwrite(s, 1, n, fp);
    free(s);
    return (int)n;
}
int impl_asprintf_chk(sv_args *a) {  // (char **out, flag, fmt, ...)
    char **out = (char **)next_gp(a);
    next_gp(a);
    const char *f = (const char *)next_gp(a);
    size_t n;
    *out = sv_format(a, f, &n);  // malloc'd, caller frees with our free
    return (int)n;
}
VTHUNK(th_fprintf_chk, impl_fprintf_chk)
VTHUNK(th_asprintf_chk, impl_asprintf_chk)

VTHUNK(th_printf_chk, impl_printf_chk)
VTHUNK(th_printf, impl_printf)
VTHUNK(th_fprintf, impl_fprintf)
VTHUNK(th_fwprintf, impl_fwprintf)
VTHUNK(th_fwprintf_chk, impl_fwprintf_chk)
VTHUNK(th_swprintf, impl_swprintf)
VTHUNK(th_swprintf_chk, impl_swprintf_chk)
VTHUNK(th_snprintf_chk, impl_snprintf_chk)
VTHUNK(th_snprintf, impl_snprintf)
VTHUNK(th_sprintf_chk, impl_sprintf_chk)
VTHUNK(th_sprintf, impl_sprintf)
VTHUNK(th_sscanf, impl_sscanf)

const shim_entry shims_fmt[] = {
    {"__printf_chk", th_printf_chk},
    {"printf", th_printf},
    {"fprintf", th_fprintf},
    {"fwprintf", th_fwprintf}, {"__fwprintf_chk", th_fwprintf_chk},
    {"swprintf", th_swprintf}, {"__swprintf_chk", th_swprintf_chk},
    {"__snprintf_chk", th_snprintf_chk},
    {"snprintf", th_snprintf},
    {"__sprintf_chk", th_sprintf_chk},
    {"sprintf", th_sprintf},
    {"sscanf", th_sscanf},
    {"__isoc99_sscanf", th_sscanf},
    {"__fprintf_chk", th_fprintf_chk}, {"__asprintf_chk", th_asprintf_chk},
    {"vsnprintf", s_vsnprintf}, {"__vsnprintf_chk", s_vsnprintf_chk},
    {"__vsprintf_chk", s_vsprintf_chk}, {"vfprintf", s_vfprintf},
    {"__vfprintf_chk", s_vfprintf_chk},
    {NULL, NULL},
};
