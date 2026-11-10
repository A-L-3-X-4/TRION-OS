#include "rt.h"

#define CRT "msvcrt"

extern char **rt_environ(void);
extern int rt_atexit_add(void *fn);
long rt_write_handle(HANDLE h, const void *buf, size_t n);
long rt_read_handle(HANDLE h, void *buf, size_t n);
HANDLE rt_open(const char *p, DWORD access, DWORD disp);
int rt_unlink(const char *p);
int rt_rename(const char *a, const char *b);
int rt_seek(HANDLE h, i64 off, int whence, i64 *out);
int rt_close(HANDLE h);

static int g_fmode, g_commode, g_errno;
static char **g_initenv;
W32DATA(CRT, _fmode, g_fmode);
W32DATA(CRT, _commode, g_commode);
W32DATA(CRT, __initenv, g_initenv);
static int g_argc;
static char **g_argv;

W32FN(CRT, __p__fmode, int *, (void)) { return &g_fmode; }
W32FN(CRT, __p__commode, int *, (void)) { return &g_commode; }
W32FN(CRT, __p___argc, int *, (void)) { return &g_argc; }
W32FN(CRT, __p___argv, char ***, (void)) { return &g_argv; }
W32FN(CRT, __p__environ, char ***, (void)) { return &g_initenv; }
W32FN(CRT, _errno, int *, (void)) { return &g_errno; }
W32FN(CRT, __set_app_type, void, (int t)) { (void)t; }
W32FN(CRT, __setusermatherr, void, (void *h)) { (void)h; }
W32FN(CRT, _set_invalid_parameter_handler, void *, (void *h)) { (void)h; return NULL; }
W32FN(CRT, _lock, void, (int n)) { (void)n; }
W32FN(CRT, _unlock, void, (int n)) { (void)n; }
W32FN(CRT, __lconv_init, int, (void)) { return 0; }
W32FN(CRT, _fpreset, void, (void)) { __asm__ volatile("fninit"); }
W32FN(CRT, ___lc_codepage_func, unsigned, (void)) { return 0; }
W32FN(CRT, ___mb_cur_max_func, int, (void)) { return 1; }

W32FN(CRT, __getmainargs, int, (int *argc, char ***argv, char ***envp, int wild, void *si))
{
    (void)wild; (void)si;
    if (!g_argv) {
        const char *p = (const char *)g_su + g_su->args_off;
        g_argv = rt_malloc((g_su->argc + 1) * sizeof(char *));
        if (!g_argv) return -1;
        for (uint32_t i = 0; i < g_su->argc; i++) {
            size_t n = rt_strlen(p) + 1;
            g_argv[i] = rt_malloc(n);
            if (g_argv[i]) memcpy(g_argv[i], p, n);
            p += n;
        }
        g_argv[g_su->argc] = NULL;
        g_argc = (int)g_su->argc;
    }
    g_initenv = rt_environ();
    *argc = g_argc;
    *argv = g_argv;
    *envp = g_initenv;
    return 0;
}

typedef void (WINAPI *initfn)(void);
typedef int (WINAPI *initfn_e)(void);
W32FN(CRT, _initterm, void, (initfn *s, initfn *e)) { for (; s < e; s++) if (*s) (*s)(); }
W32FN(CRT, _initterm_e, int, (initfn_e *s, initfn_e *e))
{
    for (; s < e; s++)
        if (*s) { int r = (*s)(); if (r) return r; }
    return 0;
}

W32FN(CRT, exit, void, (int code)) { rt_run_atexit(); rt_exit(code); }
W32FN(CRT, _exit, void, (int code)) { rt_exit(code); }
W32FN(CRT, _Exit, void, (int code)) { rt_exit(code); }
W32FN(CRT, _cexit, void, (void)) { rt_run_atexit(); }
W32FN(CRT, _c_exit, void, (void)) { }
W32FN(CRT, abort, void, (void)) { rt_puts("abort() called\n"); rt_exit(3); }
W32FN(CRT, _amsg_exit, void, (int n)) { (void)n; rt_puts("runtime error (_amsg_exit)\n"); rt_exit(255); }
W32FN(CRT, _onexit, void *, (void *fn)) { return rt_atexit_add(fn) ? NULL : fn; }
W32FN(CRT, atexit, int, (void *fn)) { return rt_atexit_add(fn); }
W32FN(CRT, signal, void *, (int sig, void *h)) { (void)sig; (void)h; return NULL; }
W32FN(CRT, raise, int, (int sig)) { if (sig == 22) rt_exit(3); return 0; }

W32FN(CRT, malloc, void *, (size_t n)) { return rt_malloc(n ? n : 1); }
W32FN(CRT, calloc, void *, (size_t a, size_t b))
{
    if (b && a > (size_t)-1 / b) return NULL;
    return rt_heap_alloc(rt_default_heap(), 8, (a * b) ? (a * b) : 1);
}
W32FN(CRT, realloc, void *, (void *p, size_t n))
{
    if (!n) { rt_free(p); return NULL; }
    return rt_heap_realloc(rt_default_heap(), 0, p, n);
}
W32FN(CRT, free, void, (void *p)) { rt_free(p); }
W32FN(CRT, _msize, size_t, (void *p)) { return rt_heap_size(p); }
W32FN(CRT, _aligned_malloc, void *, (size_t n, size_t al))
{
    if (al < 16) al = 16;
    char *raw = rt_malloc(n + al + 8);
    if (!raw) return NULL;
    uintptr_t a = ((uintptr_t)raw + 8 + al - 1) & ~(uintptr_t)(al - 1);
    ((void **)a)[-1] = raw;
    return (void *)a;
}
W32FN(CRT, _aligned_free, void, (void *p)) { if (p) rt_free(((void **)p)[-1]); }

W32FN(CRT, memcpy, void *, (void *d, const void *s, size_t n)) { return memcpy(d, s, n); }
W32FN(CRT, memmove, void *, (void *d, const void *s, size_t n)) { return memmove(d, s, n); }
W32FN(CRT, memset, void *, (void *d, int c, size_t n)) { return memset(d, c, n); }
W32FN(CRT, memcmp, int, (const void *a, const void *b, size_t n)) { return memcmp(a, b, n); }
W32FN(CRT, memchr, void *, (const void *s, int c, size_t n))
{
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}
W32FN(CRT, strlen, size_t, (const char *s)) { return rt_strlen(s); }
W32FN(CRT, wcslen, size_t, (const WCHAR *s)) { size_t n = 0; while (s[n]) n++; return n; }
W32FN(CRT, strcmp, int, (const char *a, const char *b))
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
W32FN(CRT, strncmp, int, (const char *a, const char *b, size_t n))
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
W32FN(CRT, _stricmp, int, (const char *a, const char *b))
{
    while (*a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; }
    return lc((unsigned char)*a) - lc((unsigned char)*b);
}
W32FN(CRT, _strnicmp, int, (const char *a, const char *b, size_t n))
{
    for (; n; n--, a++, b++) {
        if (lc((unsigned char)*a) != lc((unsigned char)*b)) return lc((unsigned char)*a) - lc((unsigned char)*b);
        if (!*a) return 0;
    }
    return 0;
}
W32FN(CRT, strcpy, char *, (char *d, const char *s)) { char *r = d; while ((*d++ = *s++)) ; return r; }
W32FN(CRT, strncpy, char *, (char *d, const char *s, size_t n))
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
W32FN(CRT, strcat, char *, (char *d, const char *s)) { char *r = d; while (*d) d++; while ((*d++ = *s++)) ; return r; }
W32FN(CRT, strncat, char *, (char *d, const char *s, size_t n))
{
    char *r = d;
    while (*d) d++;
    for (; n && *s; n--) *d++ = *s++;
    *d = 0;
    return r;
}
W32FN(CRT, strchr, char *, (const char *s, int c))
{
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; }
}
W32FN(CRT, strrchr, char *, (const char *s, int c))
{
    const char *r = NULL;
    for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; }
}
W32FN(CRT, strstr, char *, (const char *h, const char *n))
{
    size_t nl = rt_strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (!memcmp(h, n, nl)) return (char *)h;
    return NULL;
}
W32FN(CRT, strspn, size_t, (const char *s, const char *a))
{
    size_t n = 0;
    for (; s[n]; n++) { const char *p = a; while (*p && *p != s[n]) p++; if (!*p) break; }
    return n;
}
W32FN(CRT, strcspn, size_t, (const char *s, const char *r))
{
    size_t n = 0;
    for (; s[n]; n++) { const char *p = r; while (*p && *p != s[n]) p++; if (*p) break; }
    return n;
}
W32FN(CRT, _strdup, char *, (const char *s))
{
    size_t n = rt_strlen(s) + 1;
    char *d = rt_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
W32ALIAS(CRT, strdup, w__strdup);

static uint16_t g_ctype[257];
void rt_ctype_init(void)
{
    for (int c = 0; c < 256; c++) {
        uint16_t f = 0;
        if (c >= 'A' && c <= 'Z') f |= 0x1 | 0x100;
        if (c >= 'a' && c <= 'z') f |= 0x2 | 0x100;
        if (c >= '0' && c <= '9') f |= 0x4;
        if (c == ' ' || (c >= 9 && c <= 13)) f |= 0x8;
        if (c > 32 && c < 127 && !(f & (0x100 | 4))) f |= 0x10;
        if (c < 32 || c == 127) f |= 0x20;
        if (c == ' ' || c == '\t') f |= 0x40;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= 0x80;
        g_ctype[c + 1] = f;
    }
}
W32FN(CRT, __pctype_func, const uint16_t *, (void)) { return g_ctype + 1; }
static int ct(int c, uint16_t m) { return c >= 0 && c < 256 && (g_ctype[c + 1] & m); }
W32FN(CRT, isalpha, int, (int c)) { return ct(c, 0x100); }
W32FN(CRT, isdigit, int, (int c)) { return ct(c, 0x4); }
W32FN(CRT, isalnum, int, (int c)) { return ct(c, 0x104); }
W32FN(CRT, isspace, int, (int c)) { return ct(c, 0x8); }
W32FN(CRT, isupper, int, (int c)) { return ct(c, 0x1); }
W32FN(CRT, islower, int, (int c)) { return ct(c, 0x2); }
W32FN(CRT, isxdigit, int, (int c)) { return ct(c, 0x80); }
W32FN(CRT, ispunct, int, (int c)) { return ct(c, 0x10); }
W32FN(CRT, iscntrl, int, (int c)) { return ct(c, 0x20); }
W32FN(CRT, isprint, int, (int c)) { return c >= 32 && c < 127; }
W32FN(CRT, toupper, int, (int c)) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
W32FN(CRT, tolower, int, (int c)) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

struct lconv_t { char *dp, *ts, *gr, *ics, *cs, *mdp, *mts, *mg, *ps, *ns; char fd[8]; WCHAR *wdp, *wts, *wics, *wcs, *wmdp, *wmts, *wps, *wns; };
static WCHAR g_wdot[2] = { '.', 0 }, g_wempty[1] = { 0 };
static struct lconv_t g_lconv;
W32FN(CRT, localeconv, struct lconv_t *, (void))
{
    static char dot[] = ".", empty[] = "";
    if (!g_lconv.dp) {
        g_lconv.dp = dot; g_lconv.ts = g_lconv.gr = g_lconv.ics = g_lconv.cs = empty;
        g_lconv.mdp = dot; g_lconv.mts = g_lconv.mg = g_lconv.ps = g_lconv.ns = empty;
        memset(g_lconv.fd, 127, 8);
        g_lconv.wdp = g_wdot; g_lconv.wts = g_lconv.wics = g_lconv.wcs = g_wempty;
        g_lconv.wmdp = g_wdot; g_lconv.wmts = g_lconv.wps = g_lconv.wns = g_wempty;
    }
    return &g_lconv;
}
W32FN(CRT, strerror, char *, (int e))
{
    switch (e) {
    case 0: return "No error";
    case 2: return "No such file or directory";
    case 9: return "Bad file descriptor";
    case 12: return "Not enough space";
    case 13: return "Permission denied";
    case 17: return "File exists";
    case 22: return "Invalid argument";
    case 28: return "No space left on device";
    default: return "Unknown error";
    }
}

static int digit_of(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}
static u64 parse_int(const char *s, char **end, int base, bool *neg, bool *ovf)
{
    const char *p = s;
    *neg = false; *ovf = false;
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    if (*p == '-') { *neg = true; p++; } else if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && digit_of(p[2]) < 16) { p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    u64 v = 0;
    const char *start = p;
    for (; digit_of(*p) < base; p++) {
        u64 nv = v * (u64)base + (u64)digit_of(*p);
        if (v > (~0ull - (u64)digit_of(*p)) / (u64)base) *ovf = true;
        v = nv;
    }
    if (end) *end = (char *)(p == start ? s : p);
    return v;
}
W32FN(CRT, strtoull, u64, (const char *s, char **e, int b))
{ bool n, o; u64 v = parse_int(s, e, b, &n, &o); return o ? ~0ull : (n ? -v : v); }
W32ALIAS(CRT, _strtoui64, w_strtoull);
W32FN(CRT, strtoll, i64, (const char *s, char **e, int b))
{
    bool n, o;
    u64 v = parse_int(s, e, b, &n, &o);
    if (o || (!n && v > 0x7FFFFFFFFFFFFFFFull)) return (i64)0x7FFFFFFFFFFFFFFFll;
    if (n && v > 0x8000000000000000ull) return (i64)0x8000000000000000ull;
    return n ? (i64)(0 - v) : (i64)v;
}
W32ALIAS(CRT, _strtoi64, w_strtoll);
W32FN(CRT, strtol, int32_t, (const char *s, char **e, int b))
{
    bool n, o;
    u64 v = parse_int(s, e, b, &n, &o);
    if (o || (!n && v > 0x7FFFFFFFull)) return 0x7FFFFFFF;
    if (n && v > 0x80000000ull) return (int32_t)0x80000000u;
    return n ? (int32_t)(0 - v) : (int32_t)v;
}
W32FN(CRT, strtoul, uint32_t, (const char *s, char **e, int b))
{
    bool n, o;
    u64 v = parse_int(s, e, b, &n, &o);
    if (o || v > 0xFFFFFFFFull) return 0xFFFFFFFFu;
    return n ? (uint32_t)(0 - v) : (uint32_t)v;
}
W32FN(CRT, atoi, int, (const char *s)) { return (int)w_strtol(s, NULL, 10); }
W32ALIAS(CRT, atol, w_atoi);
W32FN(CRT, _atoi64, i64, (const char *s)) { return w_strtoll(s, NULL, 10); }
W32ALIAS(CRT, atoll, w__atoi64);
W32FN(CRT, strtod, double, (const char *s, char **end))
{
    const char *p = s;
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    bool neg = false;
    if (*p == '-') { neg = true; p++; } else if (*p == '+') p++;
    long double m = 0;
    int digits = 0, scale = 0;
    for (; *p >= '0' && *p <= '9'; p++, digits++) m = m * 10 + (*p - '0');
    if (*p == '.') {
        p++;
        for (; *p >= '0' && *p <= '9'; p++, digits++, scale--) m = m * 10 + (*p - '0');
    }
    if (!digits) { if (end) *end = (char *)s; return 0; }
    if ((*p | 32) == 'e') {
        const char *q = p + 1;
        bool en = false;
        if (*q == '-') { en = true; q++; } else if (*q == '+') q++;
        if (*q >= '0' && *q <= '9') {
            int e = 0;
            for (; *q >= '0' && *q <= '9'; q++) if (e < 10000) e = e * 10 + (*q - '0');
            scale += en ? -e : e;
            p = q;
        }
    }
    long double f = 1, b = 10;
    for (int n = scale < 0 ? -scale : scale; n; n >>= 1, b *= b)
        if (n & 1) f *= b;
    m = scale < 0 ? m / f : m * f;
    if (end) *end = (char *)p;
    return (double)(neg ? -m : m);
}
W32FN(CRT, atof, double, (const char *s)) { return w_strtod(s, NULL); }
W32FN(CRT, abs, int, (int v)) { return v < 0 ? -v : v; }
W32FN(CRT, labs, int32_t, (int32_t v)) { return v < 0 ? -v : v; }
W32FN(CRT, llabs, i64, (i64 v)) { return v < 0 ? -v : v; }
static u64 g_seed = 1;
W32FN(CRT, srand, void, (unsigned s)) { g_seed = s; }
W32FN(CRT, rand, int, (void)) { g_seed = g_seed * 214013 + 2531011; return (int)((g_seed >> 16) & 0x7FFF); }

typedef int (WINAPI *cmpfn)(const void *, const void *);
W32FN(CRT, qsort, void, (void *base, size_t n, size_t sz, cmpfn cmp))
{
    char *b = base;
    if (!sz) return;
    char tmp[256];
    char *t = sz <= sizeof(tmp) ? tmp : rt_malloc(sz);
    if (!t) return;
    for (size_t gap = n / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < n; i++) {
            memcpy(t, b + i * sz, sz);
            size_t j = i;
            for (; j >= gap && cmp(b + (j - gap) * sz, t) > 0; j -= gap)
                memcpy(b + j * sz, b + (j - gap) * sz, sz);
            memcpy(b + j * sz, t, sz);
        }
    if (t != tmp) rt_free(t);
}
W32FN(CRT, bsearch, void *, (const void *key, const void *base, size_t n, size_t sz, cmpfn cmp))
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, (const char *)base + mid * sz);
        if (!r) return (void *)((const char *)base + mid * sz);
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
W32FN(CRT, getenv, char *, (const char *n)) { return (char *)rt_env_get(n); }
W32FN(CRT, _time64, i64, (i64 *t)) { i64 v = sc1(SYS_TIME_REAL, 0) / 1000000000ll; if (t) *t = v; return v; }
W32ALIAS(CRT, time, w__time64);
W32FN(CRT, clock, int32_t, (void)) { return (int32_t)((u64)sc1(SYS_CLOCK_NS, 0) / 1000000ull); }

struct sink { char *buf; size_t cap, len; HANDLE h; int mode; char st[512]; size_t nst; bool crlf; };
static long write_text(HANDLE h, const char *p, size_t n);
static void sink_flush(struct sink *s)
{
    if (s->mode == 1 && s->nst) {
        if (s->crlf) write_text(s->h, s->st, s->nst);
        else rt_write_handle(s->h, s->st, s->nst);
        s->nst = 0;
    }
}
static void emit(struct sink *s, const char *p, size_t n)
{
    if (s->mode == 0) {
        if (s->cap && s->len + 1 < s->cap) {
            size_t room = s->cap - 1 - s->len;
            memcpy(s->buf + s->len, p, n < room ? n : room);
        }
        s->len += n;
        return;
    }
    s->len += n;
    while (n) {
        size_t c = sizeof(s->st) - s->nst;
        if (c > n) c = n;
        memcpy(s->st + s->nst, p, c);
        s->nst += c; p += c; n -= c;
        if (s->nst == sizeof(s->st)) sink_flush(s);
    }
}
static void pad(struct sink *s, char c, int n) { while (n-- > 0) emit(s, &c, 1); }

static void fmt_num(struct sink *s, u64 v, bool neg, int base, bool upper, int width, int prec, bool left,
                    bool zero, char sign, const char *prefix)
{
    char d[72];
    int n = 0;
    const char *dg = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v) { d[n++] = dg[v % (u64)base]; v /= (u64)base; }
    if (prec < 0) prec = 1;
    while (n < prec) d[n++] = '0';
    if (prec == 0 && !n) { }
    char sg = neg ? '-' : sign;
    int plen = prefix ? (int)rt_strlen(prefix) : 0;
    int total = n + (sg ? 1 : 0) + plen;
    if (!left && !zero) pad(s, ' ', width - total);
    if (sg) emit(s, &sg, 1);
    if (plen) emit(s, prefix, (size_t)plen);
    if (!left && zero) pad(s, '0', width - total);
    while (n) emit(s, &d[--n], 1);
    if (left) pad(s, ' ', width - total);
}

static void fmt_double(struct sink *s, double v, int prec, int width, bool left, bool zero, char sign, bool alt)
{
    bool neg = v < 0 || (v == 0 && 1 / v < 0);
    if (neg) v = -v;
    char out[400];
    int n = 0;
    if (v != v) { memcpy(out, "nan", 3); n = 3; zero = false; neg = false; }
    else if (v > 1.7e308) { memcpy(out, "inf", 3); n = 3; zero = false; }
    else if (v >= 1.8e19) { memcpy(out, "(big)", 5); n = 5; zero = false; }
    else {
        if (prec < 0) prec = 6;
        if (prec > 17) prec = 17;
        double r = 0.5;
        for (int i = 0; i < prec; i++) r /= 10;
        v += r;
        u64 ip = (u64)v;
        double fr = v - (double)ip;
        char t[24];
        int k = 0;
        if (!ip) t[k++] = '0';
        while (ip) { t[k++] = (char)('0' + ip % 10); ip /= 10; }
        while (k) out[n++] = t[--k];
        if (prec || alt) out[n++] = '.';
        for (int i = 0; i < prec; i++) {
            fr *= 10;
            int dgt = (int)fr;
            out[n++] = (char)('0' + dgt);
            fr -= dgt;
        }
    }
    char sg = neg ? '-' : sign;
    int total = n + (sg ? 1 : 0);
    if (!left && !zero) pad(s, ' ', width - total);
    if (sg) emit(s, &sg, 1);
    if (!left && zero) pad(s, '0', width - total);
    emit(s, out, (size_t)n);
    if (left) pad(s, ' ', width - total);
}

static void vformat(struct sink *s, const char *f, __builtin_ms_va_list *ap)
{
    for (; *f; f++) {
        if (*f != '%') {
            const char *e = f;
            while (*e && *e != '%') e++;
            emit(s, f, (size_t)(e - f));
            f = e - 1;
            continue;
        }
        f++;
        bool left = false, plus = false, space = false, alt = false, zero = false;
        for (;; f++) {
            if (*f == '-') left = true; else if (*f == '+') plus = true; else if (*f == ' ') space = true;
            else if (*f == '#') alt = true; else if (*f == '0') zero = true; else break;
        }
        int width = 0, prec = -1;
        if (*f == '*') { width = __builtin_va_arg(*ap, int); if (width < 0) { left = true; width = -width; } f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = __builtin_va_arg(*ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        int lm = 0;
        for (;; f++) {
            if (*f == 'h') lm = lm == 1 ? 2 : 1;
            else if (*f == 'l') lm = lm == 4 ? 8 : 4;
            else if (*f == 'z' || *f == 'j' || *f == 't' || *f == 'L') lm = 8;
            else if (*f == 'I') {
                if (f[1] == '6' && f[2] == '4') { lm = 8; f += 2; }
                else if (f[1] == '3' && f[2] == '2') { lm = 0; f += 2; }
                else lm = 8;
            } else if (*f == 'w') lm = 4;
            else break;
        }
        char sign = plus ? '+' : (space ? ' ' : 0);
        if (prec >= 0) zero = false;
        switch (*f) {
        case 'd': case 'i': {
            i64 v = lm == 8 ? __builtin_va_arg(*ap, i64) : (i64)__builtin_va_arg(*ap, int);
            if (lm == 1) v = (int16_t)v; else if (lm == 2) v = (int8_t)v;
            fmt_num(s, v < 0 ? 0 - (u64)v : (u64)v, v < 0, 10, false, width, prec, left, zero, sign, NULL);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            u64 v = lm == 8 ? __builtin_va_arg(*ap, u64) : (u64)__builtin_va_arg(*ap, unsigned);
            if (lm == 1) v = (uint16_t)v; else if (lm == 2) v = (uint8_t)v;
            int base = *f == 'u' ? 10 : *f == 'o' ? 8 : 16;
            fmt_num(s, v, false, base, *f == 'X', width, prec, left, zero, 0,
                    alt && v && base == 16 ? (*f == 'X' ? "0X" : "0x") : (alt && base == 8 ? "0" : NULL));
            break;
        }
        case 'p': {
            u64 v = __builtin_va_arg(*ap, u64);
            fmt_num(s, v, false, 16, true, width, 16, left, false, 0, NULL);
            break;
        }
        case 'c': {
            int c = __builtin_va_arg(*ap, int);
            char b[4]; int k = 1;
            if (lm == 4 && c > 127) { WCHAR w = (WCHAR)c; k = (int)rt_w2u(&w, 1, b, sizeof(b)); } else b[0] = (char)c;
            if (!left) pad(s, ' ', width - k);
            emit(s, b, (size_t)k);
            if (left) pad(s, ' ', width - k);
            break;
        }
        case 's': case 'S': {
            bool wide = (*f == 's' && lm == 4) || (*f == 'S' && lm != 1);
            char tmp[1024];
            const char *str;
            if (wide) {
                const WCHAR *w = __builtin_va_arg(*ap, const WCHAR *);
                if (!w) str = "(null)";
                else { rt_w2u(w, prec >= 0 ? prec : -1, tmp, sizeof(tmp)); str = tmp; }
            } else {
                str = __builtin_va_arg(*ap, const char *);
                if (!str) str = "(null)";
            }
            size_t n = 0;
            while (str[n] && (prec < 0 || (int)n < prec)) n++;
            if (!left) pad(s, ' ', width - (int)n);
            emit(s, str, n);
            if (left) pad(s, ' ', width - (int)n);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            fmt_double(s, __builtin_va_arg(*ap, double), prec, width, left, zero, sign, alt);
            break;
        case '%': emit(s, "%", 1); break;
        case 0: return;
        default: emit(s, f - 1, 2); break;
        }
    }
}

int rt_vformat(char *buf, size_t cap, const char *fmt, __builtin_ms_va_list *ap)
{
    struct sink s = { .buf = buf, .cap = cap, .mode = 0 };
    vformat(&s, fmt, ap);
    if (cap) buf[s.len < cap ? s.len : cap - 1] = 0;
    return (int)s.len;
}
static int vprint_to(HANDLE h, const char *fmt, __builtin_ms_va_list *ap, bool crlf)
{
    struct sink s = { .h = h, .mode = 1, .crlf = crlf };
    vformat(&s, fmt, ap);
    sink_flush(&s);
    return (int)s.len;
}

#define NFILE 19
static struct rt_file g_iob[NFILE];
static HANDLE g_fh[NFILE];
static uint8_t g_feof[NFILE], g_ferr[NFILE];
static uint8_t g_binary[NFILE];
static int g_unget[NFILE];

#define O_TEXT_   0x4000
#define O_BINARY_ 0x8000
static bool is_text(int i) { return i >= 3 && !g_binary[i]; }
static long write_text(HANDLE h, const char *p, size_t n)
{
    char out[1024];
    size_t used = 0;
    while (used < n) {
        size_t o = 0, k = 0;
        while (used + k < n && o + 2 <= sizeof(out)) {
            char c = p[used + k++];
            // DOS wanted a carriage return AND a line feed for every newline because it was designed around typewriters, and 40 years later I'm the idiot writing a buffer that has to leave room for the extra byte (that's the o + 2 up there)
            if (c == '\n') out[o++] = '\r';
            out[o++] = c;
        }
        long r = rt_write_handle(h, out, o);
        if (r < 0) return used ? (long)used : -1;
        if ((size_t)r < o) return (long)used;
        used += k;
    }
    return (long)used;
}

static int fidx(const struct rt_file *f) { return f >= g_iob && f < g_iob + NFILE ? (int)(f - g_iob) : -1; }
static HANDLE fhandle(int i) { return i < 3 ? HCON(i + 1) : g_fh[i]; }
static void stdio_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (int i = 0; i < NFILE; i++) { g_iob[i]._file = i; g_unget[i] = -1; g_binary[i] = 0; }
    for (int i = 0; i < 3; i++) g_iob[i]._flag = 1;
}
W32FN(CRT, __iob_func, struct rt_file *, (void)) { stdio_init(); return g_iob; }

W32FN(CRT, fwrite, size_t, (const void *p, size_t sz, size_t n, struct rt_file *f))
{
    stdio_init();
    int i = fidx(f);
    if (i < 0 || !sz || !n) return 0;
    long r = is_text(i) ? write_text(fhandle(i), p, sz * n) : rt_write_handle(fhandle(i), p, sz * n);
    if (r < 0) { g_ferr[i] = 1; return 0; }
    return (size_t)r / sz;
}
W32FN(CRT, fputc, int, (int c, struct rt_file *f))
{
    unsigned char b = (unsigned char)c;
    return w_fwrite(&b, 1, 1, f) == 1 ? (int)b : -1;
}
W32ALIAS(CRT, putc, w_fputc);
W32FN(CRT, fputs, int, (const char *s, struct rt_file *f)) { size_t n = rt_strlen(s); return w_fwrite(s, 1, n, f) == n ? 0 : -1; }
W32FN(CRT, puts, int, (const char *s))
{
    stdio_init();
    if (w_fputs(s, &g_iob[1]) < 0 || w_fputc('\n', &g_iob[1]) < 0) return -1;
    return 0;
}
W32FN(CRT, putchar, int, (int c)) { stdio_init(); return w_fputc(c, &g_iob[1]); }
W32FN(CRT, vfprintf, int, (struct rt_file *f, const char *fmt, __builtin_ms_va_list ap))
{
    stdio_init();
    int i = fidx(f);
    return i < 0 ? -1 : vprint_to(fhandle(i), fmt, &ap, is_text(i));
}
W32FN(CRT, fprintf, int, (struct rt_file *f, const char *fmt, ...))
{
    stdio_init();
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int i = fidx(f);
    int r = i < 0 ? -1 : vprint_to(fhandle(i), fmt, &ap, is_text(i));
    __builtin_ms_va_end(ap);
    return r;
}
W32FN(CRT, vprintf, int, (const char *fmt, __builtin_ms_va_list ap)) { stdio_init(); return vprint_to(HCON(2), fmt, &ap, false); }
W32FN(CRT, printf, int, (const char *fmt, ...))
{
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int r = vprint_to(HCON(2), fmt, &ap, false);
    __builtin_ms_va_end(ap);
    return r;
}
W32FN(CRT, vsnprintf, int, (char *b, size_t n, const char *fmt, __builtin_ms_va_list ap)) { return rt_vformat(b, n, fmt, &ap); }
W32ALIAS(CRT, _vsnprintf, w_vsnprintf);
W32FN(CRT, vsprintf, int, (char *b, const char *fmt, __builtin_ms_va_list ap)) { return rt_vformat(b, (size_t)1 << 30, fmt, &ap); }
W32FN(CRT, sprintf, int, (char *b, const char *fmt, ...))
{
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int r = rt_vformat(b, (size_t)1 << 30, fmt, &ap);
    __builtin_ms_va_end(ap);
    return r;
}
W32FN(CRT, snprintf, int, (char *b, size_t n, const char *fmt, ...))
{
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int r = rt_vformat(b, n, fmt, &ap);
    __builtin_ms_va_end(ap);
    return r;
}
W32ALIAS(CRT, _snprintf, w_snprintf);

W32FN(CRT, fopen, struct rt_file *, (const char *path, const char *mode))
{
    stdio_init();
    bool plus = false;
    int bin = (g_fmode & O_BINARY_) != 0;
    char m = mode[0];
    for (const char *p = mode; *p; p++) { if (*p == '+') plus = true; else if (*p == 'b') bin = 1; else if (*p == 't') bin = 0; }
    DWORD acc, disp;
    if (m == 'r') { acc = plus ? 0xC0000000u : 0x80000000u; disp = 3; }
    else if (m == 'w') { acc = plus ? 0xC0000000u : 0x40000000u; disp = 2; }
    else if (m == 'a') { acc = plus ? 0xC0000000u : 0x40000000u; disp = 4; }
    else { g_errno = 22; return NULL; }
    int slot = -1;
    for (int i = 3; i < NFILE; i++) if (!g_iob[i]._flag) { slot = i; break; }
    if (slot < 0) { g_errno = 24; return NULL; }
    HANDLE h = rt_open(path, acc, disp);
    if (h == INVALID_HANDLE_VALUE) { g_errno = get_last_error() == ERROR_FILE_NOT_FOUND || get_last_error() == ERROR_PATH_NOT_FOUND ? 2 : 13; return NULL; }
    if (m == 'a') { i64 o; rt_seek(h, 0, SEEK_END, &o); }
    g_fh[slot] = h;
    g_iob[slot]._flag = 1;
    g_feof[slot] = g_ferr[slot] = 0;
    g_binary[slot] = (uint8_t)bin;
    g_unget[slot] = -1;
    return &g_iob[slot];
}
W32FN(CRT, fclose, int, (struct rt_file *f))
{
    int i = fidx(f);
    if (i < 3) return i < 0 ? -1 : 0;
    int r = rt_close(g_fh[i]);
    g_iob[i]._flag = 0;
    g_fh[i] = NULL;
    return r;
}
W32FN(CRT, fflush, int, (struct rt_file *f)) { (void)f; return 0; }
static int fgetc_raw(int i)
{
    if (g_unget[i] >= 0) { int c = g_unget[i]; g_unget[i] = -1; return c; }
    unsigned char b;
    HANDLE h = fhandle(i);
    long r = rt_read_handle(h, &b, 1);
    if (r != 1) { if (r == 0) g_feof[i] = 1; else g_ferr[i] = 1; return -1; }
    if (!is_text(i)) return b;
    i64 o;
    // Microsoft Employees are a bunch of Psycopaths who love to make their new employees suffer, so Ctrl-Z means the end of a file for some fucking reason instead of something actually normal like linux's "EOF"
    if (b == 0x1A) {
        g_feof[i] = 1;
        rt_seek(h, -1, SEEK_CUR, &o);
        return -1;
    }
    if (b == '\r') {
        unsigned char b2;
        long r2 = rt_read_handle(h, &b2, 1);
        if (r2 == 1) {
            if (b2 == '\n') return '\n';
            rt_seek(h, -1, SEEK_CUR, &o);
        }
    }
    return b;
}
W32FN(CRT, fread, size_t, (void *p, size_t sz, size_t n, struct rt_file *f))
{
    stdio_init();
    int i = fidx(f);
    if (i < 0 || !sz || !n) return 0;
    size_t want = sz * n, got = 0;
    char *d = p;
    if (is_text(i)) {
        while (got < want) {
            int c = fgetc_raw(i);
            if (c < 0) break;
            d[got++] = (char)c;
        }
        return got / sz;
    }
    if (g_unget[i] >= 0 && want) { *d++ = (char)g_unget[i]; g_unget[i] = -1; got = 1; }
    if (got < want) {
        long r = rt_read_handle(fhandle(i), d, want - got);
        if (r < 0) { g_ferr[i] = 1; return got / sz; }
        got += (size_t)r;
        if (got < want) g_feof[i] = 1;
    }
    return got / sz;
}
W32FN(CRT, fgetc, int, (struct rt_file *f))
{
    stdio_init();
    int i = fidx(f);
    if (i < 0) return -1;
    return fgetc_raw(i);
}
W32ALIAS(CRT, getc, w_fgetc);
W32FN(CRT, getchar, int, (void)) { stdio_init(); return w_fgetc(&g_iob[0]); }
W32FN(CRT, ungetc, int, (int c, struct rt_file *f))
{
    int i = fidx(f);
    if (i < 0 || c < 0 || g_unget[i] >= 0) return -1;
    g_unget[i] = c & 255;
    g_feof[i] = 0;
    return c & 255;
}
W32FN(CRT, fgets, char *, (char *buf, int n, struct rt_file *f))
{
    int k = 0;
    while (k + 1 < n) {
        int c = w_fgetc(f);
        if (c < 0) break;
        buf[k++] = (char)c;
        if (c == '\n') break;
    }
    if (!k) return NULL;
    buf[k] = 0;
    return buf;
}
W32FN(CRT, fseek, int, (struct rt_file *f, int32_t off, int whence))
{
    int i = fidx(f);
    if (i < 3) return -1;
    i64 o;
    g_unget[i] = -1;
    g_feof[i] = 0;
    return rt_seek(g_fh[i], off, whence, &o) ? -1 : 0;
}
W32FN(CRT, ftell, int32_t, (struct rt_file *f))
{
    int i = fidx(f);
    if (i < 3) return -1;
    i64 o;
    if (rt_seek(g_fh[i], 0, SEEK_CUR, &o)) return -1;
    return (int32_t)(o - (g_unget[i] >= 0 ? 1 : 0));
}
W32FN(CRT, rewind, void, (struct rt_file *f)) { w_fseek(f, 0, SEEK_SET); }
W32FN(CRT, feof, int, (struct rt_file *f)) { int i = fidx(f); return i >= 0 && g_feof[i]; }
W32FN(CRT, ferror, int, (struct rt_file *f)) { int i = fidx(f); return i >= 0 && g_ferr[i]; }
W32FN(CRT, clearerr, void, (struct rt_file *f)) { int i = fidx(f); if (i >= 0) g_feof[i] = g_ferr[i] = 0; }
W32FN(CRT, setvbuf, int, (struct rt_file *f, char *b, int m, size_t n)) { (void)f; (void)b; (void)m; (void)n; return 0; }
W32FN(CRT, _setmode, int, (int fd, int m))
{
    stdio_init();
    if (fd < 0 || fd >= NFILE || !g_iob[fd]._flag) { g_errno = 9; return -1; }
    if (m != O_TEXT_ && m != O_BINARY_ && m != 0x10000 && m != 0x20000 && m != 0x40000) { g_errno = 22; return -1; }
    int prev = g_binary[fd] ? O_BINARY_ : O_TEXT_;
    g_binary[fd] = m == O_BINARY_;
    return prev;
}
W32FN(CRT, _fileno, int, (struct rt_file *f)) { return fidx(f); }
W32FN(CRT, remove, int, (const char *p)) { return rt_unlink(p); }
W32FN(CRT, rename, int, (const char *a, const char *b)) { return rt_rename(a, b); }
