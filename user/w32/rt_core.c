#include "rt.h"

struct w32_startup *g_su;

__asm__(".text\n.globl w32rt_start\nw32rt_start:\n xor %ebp, %ebp\n and $-16, %rsp\n call w32rt_main\n ud2\n");
void w32rt_main(struct w32_startup *su) __attribute__((noreturn, used));

void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}
void *memmove(void *d, const void *s, size_t n)
{
    void *r = d;
    if ((uintptr_t)d <= (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + n) {
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        d = (char *)d + n - 1;
        s = (const char *)s + n - 1;
        __asm__ volatile("std\n rep movsb\n cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return r;
}
void *memset(void *d, int c, size_t n)
{
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}
size_t rt_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

void rt_puts(const char *s)
{
    size_t n = rt_strlen(s);
    while (n) {
        size_t c = n > 4000 ? 4000 : n;
        long r = sc2(SYS_WRITE, s, c);
        if (r <= 0)
            break;
        s += r; n -= (size_t)r;
    }
}
void rt_log(const char *s) { rt_puts(s); }

DWORD kerr_to_win(long e)
{
    if (e < 0) e = -e;
    switch (e) {
    case E_NOENT: return ERROR_FILE_NOT_FOUND;
    case E_ACCES: case E_PERM: case E_ISDIR: return ERROR_ACCESS_DENIED;
    case E_BADF: return ERROR_INVALID_HANDLE;
    case E_NOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case E_EXIST: return ERROR_FILE_EXISTS;
    case E_NOTDIR: return ERROR_DIRECTORY;
    case E_NOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case E_BUSY: return ERROR_SHARING_VIOLATION;
    case E_INVAL: return ERROR_INVALID_PARAMETER;
    case E_NOSPC: return ERROR_DISK_FULL;
    case E_ROFS: return ERROR_WRITE_PROTECT;
    case E_NAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case E_XDEV: return ERROR_NOT_SAME_DEVICE;
    case E_IO: return ERROR_IO_DEVICE;
    case E_FAULT: return ERROR_NOACCESS;
    case E_NOSYS: case E_NOTSUP: return ERROR_CALL_NOT_IMPLEMENTED;
    case E_FBIG: return 223;
    default: return ERROR_GEN_FAILURE;
    }
}

size_t rt_w2u(const WCHAR *w, long n, char *out, size_t cap)
{
    size_t o = 0;
    for (long i = 0; (n < 0 || i < n) && w[i]; i++) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && (n < 0 || i + 1 < n) && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        uint8_t b[4];
        int k;
        if (c < 0x80) { b[0] = (uint8_t)c; k = 1; }
        else if (c < 0x800) { b[0] = 0xC0 | (c >> 6); b[1] = 0x80 | (c & 63); k = 2; }
        else if (c < 0x10000) { b[0] = 0xE0 | (c >> 12); b[1] = 0x80 | ((c >> 6) & 63); b[2] = 0x80 | (c & 63); k = 3; }
        else { b[0] = 0xF0 | (c >> 18); b[1] = 0x80 | ((c >> 12) & 63); b[2] = 0x80 | ((c >> 6) & 63); b[3] = 0x80 | (c & 63); k = 4; }
        if (o + k >= cap)
            break;
        for (int j = 0; j < k; j++)
            out[o++] = (char)b[j];
    }
    if (cap)
        out[o] = 0;
    return o;
}

size_t rt_u2w(const char *u, long n, WCHAR *out, size_t cap)
{
    size_t o = 0;
    for (long i = 0; (n < 0 || i < n) && u[i];) {
        uint8_t c = (uint8_t)u[i];
        uint32_t cp;
        int k = 1;
        if (c < 0x80) cp = c;
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; k = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; k = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; k = 4; }
        else { cp = 0xFFFD; }
        if (k > 1) {
            for (int j = 1; j < k; j++) {
                if ((n >= 0 && i + j >= n) || ((uint8_t)u[i + j] & 0xC0) != 0x80) { cp = 0xFFFD; k = j; break; }
                cp = (cp << 6) | ((uint8_t)u[i + j] & 63);
            }
        }
        i += k;
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            if (o + 2 >= cap) break;
            cp -= 0x10000;
            out[o++] = (WCHAR)(0xD800 + (cp >> 10));
            out[o++] = (WCHAR)(0xDC00 + (cp & 0x3FF));
        } else {
            if (o + 1 >= cap) break;
            out[o++] = (WCHAR)(cp > 0xFFFF ? 0xFFFD : cp);
        }
    }
    if (cap)
        out[o] = 0;
    return o;
}

long rt_vm_alloc(u64 addr, u64 size, DWORD flags) { return sc3(SYS_VM_ALLOC, addr, size, flags); }

#define LARGE_MIN (256 * 1024)
#define HB_USED 1
#define HB_LARGE 2
struct hblk { u64 size; u64 prev; };
struct hfree { struct hblk h; struct hfree *next, *prevf; };
struct heap;
struct hseg { struct hseg *next; u64 size; struct heap *heap; u64 pad; };
struct heap { u64 magic; struct hfree *freelist; struct hseg *segs; u64 flags; u64 pad[4]; };
#define HEAP_MAGIC 0x50414548u

static void fl_remove(struct heap *h, struct hfree *f)
{
    if (f->prevf) f->prevf->next = f->next; else h->freelist = f->next;
    if (f->next) f->next->prevf = f->prevf;
}
static void fl_insert(struct heap *h, struct hfree *f)
{
    f->prevf = NULL;
    f->next = h->freelist;
    if (h->freelist) h->freelist->prevf = f;
    h->freelist = f;
}
static inline u64 bsize(const struct hblk *b) { return b->size & ~(u64)15; }
static inline struct hblk *bnext(struct hblk *b) { return (struct hblk *)((char *)b + bsize(b)); }

static struct heap *heap_new_segment(struct heap *h, u64 need)
{
    u64 segsz = 1 * 1024 * 1024;
    if (need + 512 > segsz)
        segsz = (need + 512 + 0xFFFF) & ~0xFFFFull;
    long base = rt_vm_alloc(0, segsz, VM_RESERVE | VM_COMMIT | VM_PROT_R | VM_PROT_W);
    if (base < 0)
        return NULL;
    struct hseg *s = (struct hseg *)base;
    s->size = segsz;
    struct hblk *b = (struct hblk *)((char *)s + 32);
    u64 usable = (segsz - 32 - 16) & ~(u64)15;
    struct hblk *first = b;
    u64 used0 = 0;
    if (!h) {
        used0 = 16 + sizeof(struct heap);
        first->size = used0 | HB_USED;
        first->prev = 0;
        h = (struct heap *)((char *)first + 16);
        h->magic = HEAP_MAGIC; h->freelist = NULL; h->segs = NULL; h->flags = 0;
        b = (struct hblk *)((char *)first + used0);
        b->prev = used0;
    } else {
        b->prev = 0;
    }
    b->size = usable - used0;
    s->heap = h;
    s->next = h->segs;
    h->segs = s;
    struct hblk *end = bnext(b);
    end->size = 16 | HB_USED;
    end->prev = bsize(b);
    fl_insert(h, (struct hfree *)b);
    return h;
}

void *rt_heap_create(void) { return heap_new_segment(NULL, 0); }

void *rt_heap_alloc(void *hv, DWORD flags, size_t n)
{
    struct heap *h = hv;
    if (!h || h->magic != HEAP_MAGIC || n > (1ull << 40))
        return NULL;
    u64 need = ((u64)n + 15) & ~(u64)15;
    need += 16;
    if (need < 32) need = 32;
    void *res = NULL;
    if (need >= LARGE_MIN) {
        u64 total = (need + 16 + 0xFFF) & ~0xFFFull;
        long base = rt_vm_alloc(0, total, VM_RESERVE | VM_COMMIT | VM_PROT_R | VM_PROT_W);
        if (base < 0)
            return NULL;
        struct hblk *b = (struct hblk *)(base + 16);
        b->size = (total - 16) | HB_USED | HB_LARGE;
        b->prev = 0;
        return (char *)b + 16;
    }
    for (int pass = 0; pass < 2 && !res; pass++) {
        for (struct hfree *f = h->freelist; f; f = f->next) {
            u64 sz = bsize(&f->h);
            if (sz < need)
                continue;
            fl_remove(h, f);
            if (sz - need >= 32) {
                struct hblk *rest = (struct hblk *)((char *)f + need);
                rest->size = sz - need;
                rest->prev = need;
                bnext(rest)->prev = sz - need;
                fl_insert(h, (struct hfree *)rest);
                f->h.size = need | HB_USED;
            } else {
                f->h.size = sz | HB_USED;
            }
            res = (char *)f + 16;
            break;
        }
        if (!res && pass == 0 && !heap_new_segment(h, need))
            return NULL;
    }
    if (res && (flags & 8 ))
        memset(res, 0, bsize((struct hblk *)((char *)res - 16)) - 16);
    return res;
}

void rt_heap_free(void *p)
{
    if (!p)
        return;
    struct hblk *b = (struct hblk *)((char *)p - 16);
    if (b->size & HB_LARGE) {
        sc3(SYS_VM_FREE, (char *)b - 16, 0, VM_RELEASE);
        return;
    }
    struct hblk *c = b;
    while (c->prev)
        c = (struct hblk *)((char *)c - c->prev);
    struct heap *hp = ((struct hseg *)((char *)c - 32))->heap;
    u64 sz = bsize(b);
    struct hblk *nx = bnext(b);
    if (!(nx->size & HB_USED)) {
        fl_remove(hp, (struct hfree *)nx);
        sz += bsize(nx);
    }
    if (b->prev) {
        struct hblk *pv = (struct hblk *)((char *)b - b->prev);
        if (!(pv->size & HB_USED)) {
            fl_remove(hp, (struct hfree *)pv);
            sz += bsize(pv);
            b = pv;
        }
    }
    b->size = sz;
    bnext(b)->prev = sz;
    fl_insert(hp, (struct hfree *)b);
}

size_t rt_heap_size(void *p)
{
    if (!p) return (size_t)-1;
    struct hblk *b = (struct hblk *)((char *)p - 16);
    return (size_t)(bsize(b) - 16);
}

void *rt_heap_realloc(void *h, DWORD flags, void *p, size_t n)
{
    if (!p)
        return rt_heap_alloc(h, flags, n);
    size_t old = rt_heap_size(p);
    if (n <= old) {
        return p;
    }
    void *q = rt_heap_alloc(h, flags & ~8u, n);
    if (!q)
        return NULL;
    memcpy(q, p, old);
    if (flags & 8)
        memset((char *)q + old, 0, n - old);
    rt_heap_free(p);
    return q;
}

static struct heap *g_process_heap;
void *rt_default_heap(void) { return g_process_heap; }
void *rt_malloc(size_t n) { return rt_heap_alloc(g_process_heap, 0, n); }
void rt_free(void *p) { rt_heap_free(p); }

#define VOLN 16
static char g_vol[26][VOLN];
static int g_cur = -1;
static char g_cwd[RT_PATH] = "\\";

static int drive_of_letter(int c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 'C' || c > 'Z') return -1;
    int i = c - 'A';
    return g_vol[i][0] ? i : -1;
}

static void sapp(char *dst, size_t cap, size_t *o, const char *s)
{
    while (*s && *o + 1 < cap)
        dst[(*o)++] = *s++;
    dst[*o] = 0;
}

static int drives_init(void)
{
    int letter = 'C' - 'A';
    for (unsigned idx = 0; letter < 26; idx++) {
        struct ns_dirent de;
        long rc = sc4(SYS_NS_QUERY, "\\Volumes", 8, idx, &de);
        if (rc)
            break;
        if (de.type != OBJ_VOLUME)
            continue;
        size_t i = 0;
        for (; de.name[i] && i < VOLN - 1; i++)
            g_vol[letter][i] = de.name[i];
        g_vol[letter][i] = 0;
        if (g_cur < 0)
            g_cur = letter;
        letter++;
    }
    return 0;
}

static int norm_rooted(const char *in, char *out, size_t cap)
{
    size_t o = 1, starts[96];
    int depth = 0;
    out[0] = '\\';
    out[1] = 0;
    for (const char *p = in; *p;) {
        while (*p == '\\') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '\\') e++;
        size_t len = (size_t)(e - p);
        if (len == 1 && p[0] == '.') {
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (depth) o = starts[--depth];
        } else {
            if (depth >= 96 || o + len + 2 > cap) return -E_NAMETOOLONG;
            starts[depth++] = o;
            if (o > 1) out[o++] = '\\';
            for (size_t i = 0; i < len; i++) out[o++] = p[i];
        }
        out[o] = 0;
        p = e;
    }
    out[o] = 0;
    return 0;
}

int path_to_nt(const char *win, char *out)
{
    char t[RT_PATH + 4], joined[2 * RT_PATH], rooted[RT_PATH];
    size_t n = rt_strlen(win);
    if (!n) return -E_NOENT;
    if (n >= RT_PATH) return -E_NAMETOOLONG;
    for (size_t i = 0; i <= n; i++) t[i] = win[i] == '/' ? '\\' : win[i];
    const char *p = t;
    bool raw = false;
    if (!memcmp(p, "\\\\?\\", 4) || !memcmp(p, "\\??\\", 4)) { p += 4; raw = true; }

    int drive = -1;
    size_t j = 0;
    joined[0] = 0;
    if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':') {
        drive = drive_of_letter(p[0]);
        if (drive < 0) return -E_NOENT;
        p += 2;
        if (*p != '\\') {
            if (drive == g_cur) { sapp(joined, sizeof(joined), &j, g_cwd); sapp(joined, sizeof(joined), &j, "\\"); }
            else sapp(joined, sizeof(joined), &j, "\\");
        }
    } else if (raw) {
        if (*p == '\\') p++;
        if (memcmp(p, "Volumes\\", 8)) return -E_NOENT;
        char nt[RT_PATH];
        size_t o = 0;
        sapp(nt, sizeof(nt), &o, "\\");
        sapp(nt, sizeof(nt), &o, p);
        if (norm_rooted(nt, rooted, sizeof(rooted))) return -E_NAMETOOLONG;
        if (rt_strlen(rooted) >= RT_PATH - 1) return -E_NAMETOOLONG;
        size_t k = 0;
        sapp(out, RT_PATH, &k, rooted);
        return 0;
    } else if (*p == '\\') {
        drive = g_cur;
    } else {
        drive = g_cur;
        sapp(joined, sizeof(joined), &j, g_cwd);
        sapp(joined, sizeof(joined), &j, "\\");
    }
    if (drive < 0) return -E_NOENT;
    sapp(joined, sizeof(joined), &j, p);
    int rc = norm_rooted(joined, rooted, sizeof(rooted));
    if (rc) return rc;
    size_t o = 0;
    sapp(out, RT_PATH, &o, "\\Volumes\\");
    sapp(out, RT_PATH, &o, g_vol[drive]);
    if (rooted[1])
        sapp(out, RT_PATH, &o, rooted);
    return o >= RT_PATH - 1 ? -E_NAMETOOLONG : 0;
}

static int nt_to_win(const char *nt, char *out, size_t cap)
{
    if (memcmp(nt, "\\Volumes\\", 9))
        return -E_NOENT;
    const char *v = nt + 9, *e = v;
    while (*e && *e != '\\') e++;
    for (int i = 0; i < 26; i++) {
        size_t l = rt_strlen(g_vol[i]);
        if (g_vol[i][0] && l == (size_t)(e - v) && !memcmp(g_vol[i], v, l)) {
            size_t o = 0;
            char dl[3] = { (char)('A' + i), ':', 0 };
            sapp(out, cap, &o, dl);
            sapp(out, cap, &o, *e ? e : "\\");
            return 0;
        }
    }
    return -E_NOENT;
}

int path_cwd(char *out, size_t cap)
{
    if (g_cur < 0) return -E_NOENT;
    char dl[3] = { (char)('A' + g_cur), ':', 0 };
    size_t o = 0;
    sapp(out, cap, &o, dl);
    sapp(out, cap, &o, g_cwd);
    return 0;
}

int path_full(const char *win, char *out, size_t cap)
{
    char nt[RT_PATH];
    int rc = path_to_nt(win, nt);
    return rc ? rc : nt_to_win(nt, out, cap);
}

int path_set_cwd(const char *win)
{
    char nt[RT_PATH];
    int rc = path_to_nt(win, nt);
    if (rc) return rc;
    const char *v = nt + 9, *e = v;
    while (*e && *e != '\\') e++;
    for (int i = 0; i < 26; i++)
        if (g_vol[i][0] && rt_strlen(g_vol[i]) == (size_t)(e - v) && !memcmp(g_vol[i], v, e - v)) {
            g_cur = i;
            size_t o = 0;
            g_cwd[0] = 0;
            sapp(g_cwd, sizeof(g_cwd), &o, *e ? e : "\\");
            return 0;
        }
    return -E_NOENT;
}

#define ENV_MAX 128
static char *g_env[ENV_MAX + 1];
static int g_nenv;

static void env_init(void)
{
    const char *p = (const char *)g_su + g_su->env_off;
    for (uint32_t i = 0; i < g_su->envc && g_nenv < ENV_MAX; i++) {
        size_t n = rt_strlen(p) + 1;
        char *c = rt_malloc(n);
        if (c) { memcpy(c, p, n); g_env[g_nenv++] = c; }
        p += n;
    }
    g_env[g_nenv] = NULL;
}
char **rt_environ(void) { return g_env; }

static bool env_match(const char *e, const char *name, size_t nl)
{
    for (size_t i = 0; i < nl; i++)
        if (((e[i] | 32) != (name[i] | 32)) || !e[i]) return false;
    return e[nl] == '=';
}
const char *rt_env_get(const char *name)
{
    size_t nl = rt_strlen(name);
    for (int i = 0; i < g_nenv; i++)
        if (env_match(g_env[i], name, nl))
            return g_env[i] + nl + 1;
    return NULL;
}
int rt_env_set(const char *name, const char *val)
{
    size_t nl = rt_strlen(name);
    int idx = -1;
    for (int i = 0; i < g_nenv; i++)
        if (env_match(g_env[i], name, nl)) { idx = i; break; }
    if (!val) {
        if (idx >= 0) { rt_free(g_env[idx]); g_env[idx] = g_env[--g_nenv]; g_env[g_nenv] = NULL; }
        return 0;
    }
    if (idx < 0 && g_nenv >= ENV_MAX)
        return -E_NOMEM;
    size_t vl = rt_strlen(val);
    char *c = rt_malloc(nl + vl + 2);
    if (!c) return -E_NOMEM;
    memcpy(c, name, nl); c[nl] = '='; memcpy(c + nl + 1, val, vl + 1);
    if (idx >= 0) { rt_free(g_env[idx]); g_env[idx] = c; }
    else { g_env[g_nenv++] = c; g_env[g_nenv] = NULL; }
    return 0;
}

#define ATEXIT_MAX 64
typedef void (*atexit_fn)(void);
static atexit_fn g_atexit[ATEXIT_MAX];
static int g_natexit;
int rt_atexit_add(void *fn)
{
    if (g_natexit >= ATEXIT_MAX) return -1;
    g_atexit[g_natexit++] = (atexit_fn)fn;
    return 0;
}
void rt_run_atexit(void)
{
    while (g_natexit > 0) {
        atexit_fn f = g_atexit[--g_natexit];
        ((void (WINAPI *)(void))f)();
    }
}

__attribute__((noreturn)) void rt_exit(int code)
{
    for (;;)
        sc1(SYS_EXIT, code);
}

void w32rt_main(struct w32_startup *su)
{
    g_su = su;
    if (su->magic != W32_START_MAGIC)
        rt_exit(0x7FFF0001);
    g_process_heap = rt_heap_create();
    if (!g_process_heap) {
        rt_puts("w32rt: cannot create the process heap\n");
        rt_exit(0x7FFF0002);
    }
    *(u64 *)(peb() + PEB_PROCESSHEAP) = (u64)g_process_heap;
    drives_init();
    env_init();
    rt_ctype_init();
    {
        const char *cwd = (const char *)su + su->cwd_off;
        if (cwd[0]) {
            char w[RT_PATH];
            if (!nt_to_win(cwd, w, sizeof(w)))
                path_set_cwd(w);
        }
    }
    for (uint32_t i = 0; i < su->ntls; i++)
        ((void (WINAPI *)(void *, DWORD, void *))su->tls_callbacks[i])((void *)su->image_base, 1 , NULL);
    int rc = ((int (WINAPI *)(void *))su->entry)(peb());
    rt_exit(rc);
}

// Which unimplemented imports we've already complained about, a 128 slot bitmap indexed by a hash of the name, so if two names collide the second one fails in total silence
static uint64_t g_missing_seen[2];
// Every stub for an import we don't implement lands here, prints its name once so you know what to go implement, sets ERROR_CALL_NOT_IMPLEMENTED, returns 0, and hopes the exe checks return values (it won't, they never fucking do)
static DWORD WINAPI rt_missing(const char *name)
{
    u64 h = 1469598103934665603ULL;
    for (const char *p = name; *p; p++) h = (h ^ (u64)(unsigned char)*p) * 1099511628211ULL;
    int slot = (int)(h & 127);
    if (!(g_missing_seen[slot >> 6] & (1ULL << (slot & 63)))) {
        g_missing_seen[slot >> 6] |= 1ULL << (slot & 63);
        rt_puts("w32: unimplemented import called: ");
        rt_puts(name);
        rt_puts("\n");
    }
    set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}
static const struct w32_export xf_missing __attribute__((used, section("w32exp"))) =
    { (u64)W32_RT_DLL, (u64)"missing", (u64)&rt_missing, W32X_FUNC };
