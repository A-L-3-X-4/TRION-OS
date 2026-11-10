#include "rt.h"

#define K32 "kernel32"
#define GENERIC_READ  0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_ALL   0x10000000u
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u

extern char **rt_environ(void);
extern int rt_env_set(const char *name, const char *val);
extern int path_set_cwd(const char *win);

#define WPATH(w, u)  char u[RT_PATH * 2]; do { if (!(w)) { u[0] = 0; } else rt_w2u((w), -1, u, sizeof(u)); } while (0)
#define NTPATH(win, nt, retval) char nt[RT_PATH]; do { int _rc = (win) ? path_to_nt((win), nt) : -E_INVAL; if (_rc) { set_last_error(_rc == -E_NOENT ? ERROR_PATH_NOT_FOUND : kerr_to_win(_rc)); return retval; } } while (0)

W32FN(K32, GetLastError, DWORD, (void)) { return get_last_error(); }
W32FN(K32, SetLastError, void, (DWORD e)) { set_last_error(e); }
W32FN(K32, ExitProcess, void, (DWORD code)) { rt_exit((int)code); }
W32FN(K32, TerminateProcess, BOOL, (HANDLE h, DWORD code))
{
    if (h == (HANDLE)(i64)-1)
        rt_exit((int)code);
    return fail_w(ERROR_INVALID_HANDLE);
}
W32FN(K32, GetCurrentProcess, HANDLE, (void)) { return (HANDLE)(i64)-1; }
W32FN(K32, GetCurrentThread, HANDLE, (void)) { return (HANDLE)(i64)-2; }
W32FN(K32, GetCurrentProcessId, DWORD, (void)) { return (DWORD)sc1(SYS_GETPID, 0) * 4; }
W32FN(K32, GetCurrentThreadId, DWORD, (void)) { return (DWORD)sc1(SYS_GETTID, 0) * 4; }
W32FN(K32, CreateThread, HANDLE, (void *a, size_t s, void *f, void *p, DWORD fl, DWORD *id))
{ (void)a; (void)s; (void)f; (void)p; (void)fl; (void)id; set_last_error(ERROR_NOT_SUPPORTED); return NULL; }
W32FN(K32, ExitThread, void, (DWORD code)) { rt_exit((int)code); }
W32FN(K32, SwitchToThread, BOOL, (void)) { sc1(SYS_YIELD, 0); return TRUE; }
W32FN(K32, Sleep, void, (DWORD ms)) { if (ms) sc1(SYS_SLEEP_MS, ms); else sc1(SYS_YIELD, 0); }
W32FN(K32, SleepEx, DWORD, (DWORD ms, BOOL alertable)) { (void)alertable; if (ms) sc1(SYS_SLEEP_MS, ms); else sc1(SYS_YIELD, 0); return 0; }
W32FN(K32, IsDebuggerPresent, BOOL, (void)) { return FALSE; }
W32FN(K32, SetErrorMode, DWORD, (DWORD m)) { (void)m; return 0; }
W32FN(K32, DisableThreadLibraryCalls, BOOL, (void *m)) { (void)m; return TRUE; }
W32FN(K32, IsProcessorFeaturePresent, BOOL, (DWORD f)) { return f == 6 || f == 10; }
W32FN(K32, GetVersion, DWORD, (void)) { return (19045u << 16) | 10; }
W32FN(K32, GetACP, DWORD, (void)) { return 65001; }
W32FN(K32, GetOEMCP, DWORD, (void)) { return 65001; }
W32FN(K32, HeapSetInformation, BOOL, (void *h, DWORD c, void *i, size_t n)) { (void)h; (void)c; (void)i; (void)n; return TRUE; }
W32FN(K32, GetVersionExA, BOOL, (DWORD *v))
{
    if (v[0] < 148) return fail_w(ERROR_INSUFFICIENT_BUFFER);
    v[1] = 10; v[2] = 0; v[3] = 19045; v[4] = 2;
    ((char *)v)[20] = 0;
    return TRUE;
}
W32FN(K32, GetSystemInfo, void, (uint8_t *si))
{
    memset(si, 0, 48);
    *(uint16_t *)si = 9;
    *(DWORD *)(si + 4) = 4096;
    *(u64 *)(si + 8) = 0x10000;
    *(u64 *)(si + 16) = USER_TOP - 0x10000;
    *(u64 *)(si + 24) = 1;
    *(DWORD *)(si + 32) = 1;
    *(DWORD *)(si + 36) = 8664;
    *(DWORD *)(si + 40) = 0x10000;
    *(uint16_t *)(si + 44) = 6;
}
W32ALIAS(K32, GetNativeSystemInfo, w_GetSystemInfo);
W32FN(K32, OutputDebugStringA, void, (const char *s)) { rt_puts(s); }
W32FN(K32, SetUnhandledExceptionFilter, void *, (void *f))
{
    static void *cur;
    void *old = cur;
    cur = f;
    return old;
}
W32FN(K32, UnhandledExceptionFilter, DWORD, (void *ep)) { (void)ep; return 0; }
W32FN(K32, RaiseException, void, (DWORD code, DWORD fl, DWORD n, const u64 *a))
{
    (void)fl; (void)n; (void)a;
    rt_puts("w32: unhandled exception (no SEH in this runtime): process terminated\n");
    rt_exit((int)code);
}

W32FN(K32, GetTickCount64, u64, (void)) { return (u64)sc1(SYS_CLOCK_NS, 0) / 1000000ull; }
W32FN(K32, GetTickCount, DWORD, (void)) { return (DWORD)((u64)sc1(SYS_CLOCK_NS, 0) / 1000000ull); }
W32FN(K32, QueryPerformanceCounter, BOOL, (i64 *c)) { *c = sc1(SYS_CLOCK_NS, 0); return TRUE; }
W32FN(K32, QueryPerformanceFrequency, BOOL, (i64 *f)) { *f = 1000000000; return TRUE; }
static u64 filetime_now(void) { return (u64)sc1(SYS_TIME_REAL, 0) / 100 + 116444736000000000ull; }
W32FN(K32, GetSystemTimeAsFileTime, void, (u64 *ft)) { *ft = filetime_now(); }
W32ALIAS(K32, GetSystemTimePreciseAsFileTime, w_GetSystemTimeAsFileTime);

static void unix_to_systemtime(u64 secs, u64 ns, uint16_t *st)
{
    u64 days = secs / 86400, rem = secs % 86400;
    i64 z = (i64)days + 719468;
    i64 era = z / 146097;
    i64 doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = yoe + era * 400;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153;
    i64 d = doy - (153 * mp + 2) / 5 + 1;
    i64 m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    st[0] = (uint16_t)y; st[1] = (uint16_t)m;
    st[2] = (uint16_t)((days + 4) % 7);
    st[3] = (uint16_t)d; st[4] = (uint16_t)(rem / 3600); st[5] = (uint16_t)(rem % 3600 / 60);
    st[6] = (uint16_t)(rem % 60); st[7] = (uint16_t)(ns / 1000000);
}
W32FN(K32, GetSystemTime, void, (uint16_t *st))
{
    u64 ns = (u64)sc1(SYS_TIME_REAL, 0);
    unix_to_systemtime(ns / 1000000000ull, ns % 1000000000ull, st);
}
W32ALIAS(K32, GetLocalTime, w_GetSystemTime);

W32FN(K32, CloseHandle, BOOL, (HANDLE h))
{
    if (con_index(h))
        return TRUE;
    uint32_t k = w_to_kh(h);
    if (!k)
        return fail_w(ERROR_INVALID_HANDLE);
    long r = sc1(SYS_CLOSE, k);
    return r ? fail_k(r) : TRUE;
}
W32FN(K32, DuplicateHandle, BOOL, (HANDLE sp, HANDLE sh, HANDLE tp, HANDLE *out, DWORD acc, BOOL inh, DWORD opt))
{
    (void)sp; (void)tp; (void)acc; (void)inh;
    if (con_index(sh)) { *out = sh; return TRUE; }
    uint32_t k = w_to_kh(sh);
    if (!k) return fail_w(ERROR_INVALID_HANDLE);
    long r = sc2(SYS_DUP, k, 0);
    if (r < 0) return fail_k(r);
    *out = kh_to_w((uint32_t)r);
    if (opt & 1) sc1(SYS_CLOSE, k);
    return TRUE;
}
W32FN(K32, CreateEventW, HANDLE, (void *sa, BOOL manual, BOOL initial, const WCHAR *name))
{
    (void)sa; (void)name;
    long r = sc1(SYS_EVENT_CREATE, (manual ? 1 : 0) | (initial ? 2 : 0));
    if (r < 0) { fail_k(r); return NULL; }
    return kh_to_w((uint32_t)r);
}
W32FN(K32, CreateEventA, HANDLE, (void *sa, BOOL manual, BOOL initial, const char *name))
{ (void)name; return w_CreateEventW(sa, manual, initial, NULL); }
static BOOL ev_op(HANDLE h, int op)
{
    uint32_t k = w_to_kh(h);
    if (!k) return fail_w(ERROR_INVALID_HANDLE);
    long r = sc2(SYS_EVENT_OP, k, op);
    return r < 0 ? fail_k(r) : TRUE;
}
W32FN(K32, SetEvent, BOOL, (HANDLE h)) { return ev_op(h, EVENT_SET); }
W32FN(K32, ResetEvent, BOOL, (HANDLE h)) { return ev_op(h, EVENT_RESET); }
W32FN(K32, CreateSemaphoreW, HANDLE, (void *sa, i64 initial, i64 max, const WCHAR *name))
{
    (void)sa; (void)name;
    long r = sc2(SYS_SEM_CREATE, (int32_t)initial, (int32_t)max);
    if (r < 0) { fail_k(r); return NULL; }
    return kh_to_w((uint32_t)r);
}
W32FN(K32, CreateSemaphoreA, HANDLE, (void *sa, i64 initial, i64 max, const char *name))
{ (void)name; return w_CreateSemaphoreW(sa, initial, max, NULL); }
W32FN(K32, ReleaseSemaphore, BOOL, (HANDLE h, i64 n, int32_t *prev))
{
    uint32_t k = w_to_kh(h);
    if (!k) return fail_w(ERROR_INVALID_HANDLE);
    long r = sc2(SYS_SEM_RELEASE, k, (int32_t)n);
    if (r < 0) return fail_k(r);
    if (prev) *prev = (int32_t)r;
    return TRUE;
}
W32FN(K32, WaitForSingleObject, DWORD, (HANDLE h, DWORD ms))
{
    uint32_t k = w_to_kh(h);
    if (!k) { set_last_error(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    long r = sc2(SYS_WAIT, k, ms == INFINITE ? (long)IPC_TIMEOUT_INF : (long)ms);
    if (!r) return WAIT_OBJECT_0;
    if (r == -E_AGAIN || r == -E_TIMEDOUT) return WAIT_TIMEOUT;
    set_last_error(kerr_to_win(r));
    return WAIT_FAILED;
}
W32FN(K32, WaitForSingleObjectEx, DWORD, (HANDLE h, DWORD ms, BOOL a)) { (void)a; return w_WaitForSingleObject(h, ms); }

W32FN(K32, InitializeCriticalSection, void, (uint8_t *cs)) { memset(cs, 0, 40); *(int32_t *)(cs + 8) = -1; }
W32FN(K32, InitializeCriticalSectionAndSpinCount, BOOL, (uint8_t *cs, DWORD n)) { (void)n; w_InitializeCriticalSection(cs); return TRUE; }
W32FN(K32, InitializeCriticalSectionEx, BOOL, (uint8_t *cs, DWORD n, DWORD f)) { (void)n; (void)f; w_InitializeCriticalSection(cs); return TRUE; }
W32FN(K32, EnterCriticalSection, void, (uint8_t *cs)) { (*(int32_t *)(cs + 12))++; (*(int32_t *)(cs + 8))++; }
W32FN(K32, TryEnterCriticalSection, BOOL, (uint8_t *cs)) { w_EnterCriticalSection(cs); return TRUE; }
W32FN(K32, LeaveCriticalSection, void, (uint8_t *cs)) { (*(int32_t *)(cs + 12))--; (*(int32_t *)(cs + 8))--; }
W32FN(K32, DeleteCriticalSection, void, (uint8_t *cs)) { memset(cs, 0, 40); }

W32FN(K32, InterlockedIncrement, int32_t, (volatile int32_t *p)) { return __sync_add_and_fetch(p, 1); }
W32FN(K32, InterlockedDecrement, int32_t, (volatile int32_t *p)) { return __sync_sub_and_fetch(p, 1); }
W32FN(K32, InterlockedExchange, int32_t, (volatile int32_t *p, int32_t v)) { return __sync_lock_test_and_set(p, v); }
W32FN(K32, InterlockedExchangeAdd, int32_t, (volatile int32_t *p, int32_t v)) { return __sync_fetch_and_add(p, v); }
W32FN(K32, InterlockedCompareExchange, int32_t, (volatile int32_t *p, int32_t x, int32_t c)) { return __sync_val_compare_and_swap(p, c, x); }

static u64 g_tls_used;
W32FN(K32, TlsAlloc, DWORD, (void))
{
    for (int i = 0; i < 64; i++)
        if (!(g_tls_used & (1ull << i))) {
            g_tls_used |= 1ull << i;
            *(u64 *)(teb() + TEB_TLSSLOTS + 8 * i) = 0;
            return (DWORD)i;
        }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0xFFFFFFFFu;
}
W32FN(K32, TlsFree, BOOL, (DWORD i))
{
    if (i >= 64 || !(g_tls_used & (1ull << i))) return fail_w(ERROR_INVALID_PARAMETER);
    g_tls_used &= ~(1ull << i);
    return TRUE;
}
W32FN(K32, TlsGetValue, void *, (DWORD i))
{
    if (i >= 64) { set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    set_last_error(0);
    return (void *)*(u64 *)(teb() + TEB_TLSSLOTS + 8 * i);
}
W32FN(K32, TlsSetValue, BOOL, (DWORD i, void *v))
{
    if (i >= 64) return fail_w(ERROR_INVALID_PARAMETER);
    *(u64 *)(teb() + TEB_TLSSLOTS + 8 * i) = (u64)v;
    return TRUE;
}
W32FN(K32, FlsAlloc, DWORD, (void *cb)) { (void)cb; return w_TlsAlloc(); }
W32FN(K32, FlsFree, BOOL, (DWORD i)) { return w_TlsFree(i); }
W32FN(K32, FlsGetValue, void *, (DWORD i)) { return w_TlsGetValue(i); }
W32FN(K32, FlsSetValue, BOOL, (DWORD i, void *v)) { return w_TlsSetValue(i, v); }

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_FREE 0x10000
#define MEM_PRIVATE 0x20000
#define MEM_IMAGE 0x1000000

static int prot_from_page(DWORD p)
{
    switch (p) {
    case 0x01: return 0;
    case 0x02: return VM_PROT_R;
    case 0x04: case 0x08: return VM_PROT_R | VM_PROT_W;
    case 0x10: case 0x20: return VM_PROT_R | VM_PROT_X;
    case 0x40: case 0x80: return VM_PROT_R | VM_PROT_W | VM_PROT_X;
    default: return -1;
    }
}
static DWORD page_from_prot(uint32_t p)
{
    p &= 7;
    if (!p) return 0x01;
    if (p & VM_PROT_X) return (p & VM_PROT_W) ? 0x40 : 0x20;
    return (p & VM_PROT_W) ? 0x04 : 0x02;
}
static DWORD vm_err(long r)
{
    if (r == -E_EXIST) return ERROR_INVALID_ADDRESS;
    if (r == -E_NOMEM) return ERROR_NOT_ENOUGH_MEMORY;
    return ERROR_INVALID_PARAMETER;
}

W32FN(K32, VirtualAlloc, void *, (void *addr, size_t size, DWORD type, DWORD protect))
{
    int pr = prot_from_page(protect);
    if (pr < 0 || !(type & (MEM_COMMIT | MEM_RESERVE)) || (type & ~(DWORD)(MEM_COMMIT | MEM_RESERVE | 0x100000 ))) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    DWORD fl = (DWORD)pr | ((type & MEM_RESERVE) ? VM_RESERVE : 0) | ((type & MEM_COMMIT) ? VM_COMMIT : 0);
    if (!addr && (type & MEM_COMMIT))
        fl |= VM_RESERVE;
    long r = rt_vm_alloc((u64)addr, size, fl);
    if (r < 0) { set_last_error(vm_err(r)); return NULL; }
    return (void *)r;
}
W32FN(K32, VirtualFree, BOOL, (void *addr, size_t size, DWORD type))
{
    long r;
    if (type == MEM_RELEASE) {
        if (size) return fail_w(ERROR_INVALID_PARAMETER);
        r = sc3(SYS_VM_FREE, addr, 0, VM_RELEASE);
    } else if (type == MEM_DECOMMIT) {
        r = sc3(SYS_VM_FREE, addr, size, VM_DECOMMIT);
    } else {
        return fail_w(ERROR_INVALID_PARAMETER);
    }
    if (r < 0) return fail_w(vm_err(r));
    return TRUE;
}
W32FN(K32, VirtualProtect, BOOL, (void *addr, size_t size, DWORD np, DWORD *old))
{
    int pr = prot_from_page(np);
    if (pr < 0 || !old) return fail_w(ERROR_INVALID_PARAMETER);
    long r = sc3(SYS_VM_PROTECT, addr, size, pr);
    if (r < 0) return fail_w(r == -E_INVAL ? ERROR_INVALID_ADDRESS : vm_err(r));
    *old = page_from_prot((uint32_t)r);
    return TRUE;
}
struct mbi { u64 base, abase; DWORD aprot, pad1; u64 size; DWORD state, prot, type, pad2; };
W32FN(K32, VirtualQuery, size_t, (const void *addr, struct mbi *mbi, size_t len))
{
    if (len < sizeof(*mbi)) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    struct vm_info vi;
    long r = sc2(SYS_VM_QUERY, addr, &vi);
    if (r < 0) { set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    memset(mbi, 0, sizeof(*mbi));
    mbi->base = vi.base;
    mbi->size = vi.size;
    if (vi.state == VM_ST_FREE) {
        mbi->state = MEM_FREE; mbi->prot = 0x01;
    } else {
        mbi->abase = vi.abase;
        mbi->aprot = page_from_prot(vi.aprot);
        mbi->state = vi.state == VM_ST_COMMITTED ? MEM_COMMIT : MEM_RESERVE;
        mbi->prot = vi.state == VM_ST_COMMITTED ? page_from_prot(vi.prot) : 0;
        mbi->type = vi.type == VM_TY_IMAGE ? MEM_IMAGE : MEM_PRIVATE;
    }
    return sizeof(*mbi);
}

void *rt_heap_create(void);
W32FN(K32, GetProcessHeap, void *, (void)) { return rt_default_heap(); }
W32FN(K32, HeapCreate, void *, (DWORD opt, size_t init, size_t max)) { (void)opt; (void)init; (void)max; return rt_heap_create(); }
W32FN(K32, HeapDestroy, BOOL, (void *h)) { (void)h; return TRUE; }
W32FN(K32, HeapAlloc, void *, (void *h, DWORD fl, size_t n))
{
    void *p = rt_heap_alloc(h, fl, n);
    if (!p) set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}
W32FN(K32, HeapReAlloc, void *, (void *h, DWORD fl, void *p, size_t n))
{
    void *q = rt_heap_realloc(h, fl, p, n);
    if (!q) set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return q;
}
W32FN(K32, HeapFree, BOOL, (void *h, DWORD fl, void *p)) { (void)h; (void)fl; rt_heap_free(p); return TRUE; }
W32FN(K32, HeapSize, size_t, (void *h, DWORD fl, const void *p)) { (void)h; (void)fl; return rt_heap_size((void *)p); }
W32FN(K32, HeapValidate, BOOL, (void *h, DWORD fl, const void *p)) { (void)h; (void)fl; (void)p; return TRUE; }
W32FN(K32, LocalAlloc, void *, (DWORD fl, size_t n)) { return w_HeapAlloc(rt_default_heap(), (fl & 0x40) ? 8 : 0, n); }
W32FN(K32, LocalFree, void *, (void *p)) { rt_heap_free(p); return NULL; }
W32FN(K32, GlobalAlloc, void *, (DWORD fl, size_t n)) { return w_HeapAlloc(rt_default_heap(), (fl & 0x40) ? 8 : 0, n); }
W32FN(K32, GlobalFree, void *, (void *p)) { rt_heap_free(p); return NULL; }

static HANDLE g_std[4] = { 0, HCON(1), HCON(2), HCON(3) };
W32FN(K32, GetStdHandle, HANDLE, (DWORD n))
{
    switch (n) {
    case (DWORD)-10: return g_std[1];
    case (DWORD)-11: return g_std[2];
    case (DWORD)-12: return g_std[3];
    }
    set_last_error(ERROR_INVALID_HANDLE);
    return INVALID_HANDLE_VALUE;
}
W32FN(K32, SetStdHandle, BOOL, (DWORD n, HANDLE h))
{
    if (n == (DWORD)-10) g_std[1] = h; else if (n == (DWORD)-11) g_std[2] = h; else if (n == (DWORD)-12) g_std[3] = h;
    else return fail_w(ERROR_INVALID_HANDLE);
    return TRUE;
}
W32FN(K32, GetConsoleMode, BOOL, (HANDLE h, DWORD *m)) { if (!con_index(h)) return fail_w(ERROR_INVALID_HANDLE); *m = 3; return TRUE; }
W32FN(K32, SetConsoleMode, BOOL, (HANDLE h, DWORD m)) { (void)m; return con_index(h) ? TRUE : fail_w(ERROR_INVALID_HANDLE); }
W32FN(K32, GetConsoleCP, DWORD, (void)) { return 65001; }
W32FN(K32, GetConsoleOutputCP, DWORD, (void)) { return 65001; }
W32FN(K32, SetConsoleOutputCP, BOOL, (DWORD cp)) { (void)cp; return TRUE; }
W32FN(K32, SetConsoleCtrlHandler, BOOL, (void *h, BOOL a)) { (void)h; (void)a; return TRUE; }
W32FN(K32, GetFileType, DWORD, (HANDLE h))
{
    if (con_index(h)) return 2;
    return w_to_kh(h) ? 1 : (set_last_error(ERROR_INVALID_HANDLE), 0);
}

static long con_write(const void *buf, size_t n)
{
    const char *p = buf;
    size_t done = 0;
    while (done < n) {
        size_t c = n - done > 4000 ? 4000 : n - done;
        long r = sc2(SYS_WRITE, p + done, c);
        if (r <= 0) return done ? (long)done : r;
        done += (size_t)r;
    }
    return (long)done;
}
long rt_write_handle(HANDLE h, const void *buf, size_t n)
{
    int ci = con_index(h);
    if (ci) return ci == 1 ? -E_BADF : con_write(buf, n);
    uint32_t k = w_to_kh(h);
    if (!k) return -E_BADF;
    size_t done = 0;
    while (done < n) {
        size_t c = n - done > 65536 ? 65536 : n - done;
        long r = sc3(SYS_FILE_WRITE, k, (const char *)buf + done, c);
        if (r < 0) return done ? (long)done : r;
        done += (size_t)r;
        if ((size_t)r < c) break;
    }
    return (long)done;
}
long rt_read_handle(HANDLE h, void *buf, size_t n)
{
    int ci = con_index(h);
    if (ci) return ci == 1 ? 0 : -E_BADF;
    uint32_t k = w_to_kh(h);
    if (!k) return -E_BADF;
    size_t done = 0;
    while (done < n) {
        size_t c = n - done > 65536 ? 65536 : n - done;
        long r = sc3(SYS_FILE_READ, k, (char *)buf + done, c);
        if (r < 0) return done ? (long)done : r;
        if (!r) break;
        done += (size_t)r;
        if ((size_t)r < c) break;
    }
    return (long)done;
}

W32FN(K32, WriteFile, BOOL, (HANDLE h, const void *buf, DWORD n, DWORD *wrote, void *ovl))
{
    if (wrote) *wrote = 0;
    if (ovl) return fail_w(ERROR_NOT_SUPPORTED);
    long r = rt_write_handle(h, buf, n);
    if (r < 0) return fail_k(r);
    if (wrote) *wrote = (DWORD)r;
    return TRUE;
}
W32FN(K32, ReadFile, BOOL, (HANDLE h, void *buf, DWORD n, DWORD *got, void *ovl))
{
    if (got) *got = 0;
    if (ovl) return fail_w(ERROR_NOT_SUPPORTED);
    long r = rt_read_handle(h, buf, n);
    if (r < 0) return fail_k(r);
    if (got) *got = (DWORD)r;
    return TRUE;
}
W32FN(K32, WriteConsoleA, BOOL, (HANDLE h, const void *b, DWORD n, DWORD *w, void *r))
{ (void)r; if (!con_index(h)) return fail_w(ERROR_INVALID_HANDLE); return w_WriteFile(h, b, n, w, NULL); }
W32FN(K32, WriteConsoleW, BOOL, (HANDLE h, const WCHAR *b, DWORD n, DWORD *w, void *r))
{
    (void)r;
    if (!con_index(h)) return fail_w(ERROR_INVALID_HANDLE);
    char u[1024];
    DWORD done = 0;
    while (done < n) {
        DWORD c = n - done > 250 ? 250 : n - done;
        size_t k = rt_w2u(b + done, c, u, sizeof(u));
        con_write(u, k);
        done += c;
    }
    if (w) *w = n;
    return TRUE;
}
W32FN(K32, ReadConsoleA, BOOL, (HANDLE h, void *b, DWORD n, DWORD *g, void *r))
{ (void)b; (void)n; (void)r; if (!con_index(h)) return fail_w(ERROR_INVALID_HANDLE); *g = 0; return TRUE; }
W32FN(K32, ReadConsoleW, BOOL, (HANDLE h, void *b, DWORD n, DWORD *g, void *r))
{ (void)b; (void)n; (void)r; if (!con_index(h)) return fail_w(ERROR_INVALID_HANDLE); *g = 0; return TRUE; }
W32FN(K32, FlushFileBuffers, BOOL, (HANDLE h))
{
    if (con_index(h)) return TRUE;
    uint32_t k = w_to_kh(h);
    if (!k) return fail_w(ERROR_INVALID_HANDLE);
    long r = sc1(SYS_FILE_FLUSH, k);
    return r ? fail_k(r) : TRUE;
}

static HANDLE create_file(const char *wp, DWORD access, DWORD disp, DWORD flags)
{
    NTPATH(wp, nt, INVALID_HANDLE_VALUE);
    bool wantw = access & (GENERIC_WRITE | GENERIC_ALL | 2 | 4 | 0x10000 );
    uint32_t acc = FILE_READ_DATA;
    if (wantw) acc = (access & (GENERIC_READ | GENERIC_ALL | 1 | 0x80)) ? (FILE_READ_DATA | FILE_WRITE_DATA) : FILE_WRITE_DATA;
    uint32_t kd;
    switch (disp) {
    case 1: kd = FD_CREATE; break;
    case 2: kd = FD_OVERWRITE_IF; break;
    case 3: case 5: kd = FD_OPEN; break;
    case 4: kd = FD_OPEN_IF; break;
    default: set_last_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
    }
    if (disp == 5 && !wantw) { set_last_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    struct file_stat st;
    bool exists = sc3(SYS_FS_STAT, nt, rt_strlen(nt), &st) == 0;
    if (exists && st.type == FT_DIR) {
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS) || wantw || disp == 1 || disp == 2 || disp == 5) {
            set_last_error(disp == 1 ? ERROR_FILE_EXISTS : ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        kd |= FO_DIRECTORY;
        acc = FILE_READ_DATA;
    }
    long h = sc4(SYS_FILE_OPEN, nt, rt_strlen(nt), acc, kd);
    if (h < 0) { fail_k(h); return INVALID_HANDLE_VALUE; }
    if (disp == 5)
        sc2(SYS_FILE_SETSIZE, h, 0);
    set_last_error((exists && (disp == 2 || disp == 4)) ? ERROR_ALREADY_EXISTS : 0);
    return kh_to_w((uint32_t)h);
}
W32FN(K32, CreateFileA, HANDLE, (const char *p, DWORD acc, DWORD share, void *sa, DWORD disp, DWORD fl, HANDLE tmpl))
{ (void)share; (void)sa; (void)tmpl; return create_file(p, acc, disp, fl); }
W32FN(K32, CreateFileW, HANDLE, (const WCHAR *p, DWORD acc, DWORD share, void *sa, DWORD disp, DWORD fl, HANDLE tmpl))
{ (void)share; (void)sa; (void)tmpl; WPATH(p, u); return create_file(u, acc, disp, fl); }

static BOOL seek_k(HANDLE h, i64 off, int whence, i64 *out)
{
    if (con_index(h)) return fail_w(ERROR_INVALID_HANDLE);
    uint32_t k = w_to_kh(h);
    if (!k) return fail_w(ERROR_INVALID_HANDLE);
    long r = sc3(SYS_FILE_SEEK, k, off, whence);
    if (r < 0) return fail_w(r == -E_INVAL ? ERROR_NEGATIVE_SEEK : kerr_to_win(r));
    *out = r;
    return TRUE;
}
W32FN(K32, SetFilePointerEx, BOOL, (HANDLE h, i64 d, i64 *np, DWORD m))
{
    i64 o;
    if (m > 2) return fail_w(ERROR_INVALID_PARAMETER);
    if (!seek_k(h, d, (int)m, &o)) return FALSE;
    if (np) *np = o;
    return TRUE;
}
W32FN(K32, SetFilePointer, DWORD, (HANDLE h, int32_t lo, int32_t *hi, DWORD m))
{
    i64 d = hi ? ((i64)*hi << 32) | (uint32_t)lo : (i64)lo, o;
    if (m > 2) { set_last_error(ERROR_INVALID_PARAMETER); return 0xFFFFFFFFu; }
    if (!seek_k(h, d, (int)m, &o)) return 0xFFFFFFFFu;
    if (hi) *hi = (int32_t)(o >> 32);
    set_last_error(0);
    return (DWORD)o;
}
static BOOL fsize(HANDLE h, u64 *sz)
{
    uint32_t k = w_to_kh(h);
    if (!k) return fail_w(ERROR_INVALID_HANDLE);
    struct file_stat st;
    long r = sc2(SYS_FILE_STAT, k, &st);
    if (r) return fail_k(r);
    *sz = st.size;
    return TRUE;
}
W32FN(K32, GetFileSizeEx, BOOL, (HANDLE h, i64 *out)) { u64 s; if (!fsize(h, &s)) return FALSE; *out = (i64)s; return TRUE; }
W32FN(K32, GetFileSize, DWORD, (HANDLE h, DWORD *hi))
{
    u64 s;
    if (!fsize(h, &s)) return 0xFFFFFFFFu;
    if (hi) *hi = (DWORD)(s >> 32);
    set_last_error(0);
    return (DWORD)s;
}
W32FN(K32, SetEndOfFile, BOOL, (HANDLE h))
{
    i64 pos;
    if (!seek_k(h, 0, SEEK_CUR, &pos)) return FALSE;
    long r = sc2(SYS_FILE_SETSIZE, w_to_kh(h), pos);
    return r ? fail_k(r) : TRUE;
}

static DWORD win_attr(uint32_t a, uint32_t type)
{
    DWORD w = a & (FA_RDONLY | FA_HIDDEN | FA_SYSTEM | FA_ARCHIVE);
    if (type == FT_DIR) w |= FILE_ATTRIBUTE_DIRECTORY;
    return w ? w : FILE_ATTRIBUTE_NORMAL;
}
static u64 ft_from_unix(u64 s) { return (s + 11644473600ull) * 10000000ull; }

static DWORD get_attr(const char *wp)
{
    char nt[RT_PATH];
    int rc = path_to_nt(wp, nt);
    if (rc) { set_last_error(rc == -E_NOENT ? ERROR_PATH_NOT_FOUND : kerr_to_win(rc)); return INVALID_FILE_ATTRIBUTES; }
    struct file_stat st;
    long r = sc3(SYS_FS_STAT, nt, rt_strlen(nt), &st);
    if (r) { fail_k(r); return INVALID_FILE_ATTRIBUTES; }
    return win_attr(st.attr, st.type);
}
W32FN(K32, GetFileAttributesA, DWORD, (const char *p)) { return get_attr(p); }
W32FN(K32, GetFileAttributesW, DWORD, (const WCHAR *p)) { WPATH(p, u); return get_attr(u); }
static BOOL attr_ex(const char *wp, void *out)
{
    NTPATH(wp, nt, FALSE);
    struct file_stat st;
    long r = sc3(SYS_FS_STAT, nt, rt_strlen(nt), &st);
    if (r) return fail_k(r);
    uint8_t *o = out;
    *(DWORD *)o = win_attr(st.attr, st.type);
    *(u64 *)(o + 4) = ft_from_unix(st.ctime);
    *(u64 *)(o + 12) = ft_from_unix(st.atime);
    *(u64 *)(o + 20) = ft_from_unix(st.mtime);
    *(DWORD *)(o + 28) = (DWORD)(st.size >> 32);
    *(DWORD *)(o + 32) = (DWORD)st.size;
    return TRUE;
}
W32FN(K32, GetFileAttributesExA, BOOL, (const char *p, DWORD lvl, void *out)) { if (lvl) return fail_w(ERROR_INVALID_PARAMETER); return attr_ex(p, out); }
W32FN(K32, GetFileAttributesExW, BOOL, (const WCHAR *p, DWORD lvl, void *out)) { WPATH(p, u); if (lvl) return fail_w(ERROR_INVALID_PARAMETER); return attr_ex(u, out); }

static BOOL del_file(const char *wp)
{
    NTPATH(wp, nt, FALSE);
    struct file_stat st;
    long r = sc3(SYS_FS_STAT, nt, rt_strlen(nt), &st);
    if (r) return fail_k(r);
    if (st.type == FT_DIR) return fail_w(ERROR_ACCESS_DENIED);
    r = sc2(SYS_FS_REMOVE, nt, rt_strlen(nt));
    return r ? fail_k(r) : TRUE;
}
W32FN(K32, DeleteFileA, BOOL, (const char *p)) { return del_file(p); }
W32FN(K32, DeleteFileW, BOOL, (const WCHAR *p)) { WPATH(p, u); return del_file(u); }
static BOOL rm_dir(const char *wp)
{
    NTPATH(wp, nt, FALSE);
    struct file_stat st;
    long r = sc3(SYS_FS_STAT, nt, rt_strlen(nt), &st);
    if (r) return fail_k(r);
    if (st.type != FT_DIR) return fail_w(ERROR_DIRECTORY);
    r = sc2(SYS_FS_REMOVE, nt, rt_strlen(nt));
    return r ? fail_k(r) : TRUE;
}
W32FN(K32, RemoveDirectoryA, BOOL, (const char *p)) { return rm_dir(p); }
W32FN(K32, RemoveDirectoryW, BOOL, (const WCHAR *p)) { WPATH(p, u); return rm_dir(u); }
static BOOL mk_dir(const char *wp)
{
    NTPATH(wp, nt, FALSE);
    long r = sc2(SYS_FS_MKDIR, nt, rt_strlen(nt));
    if (r == -E_EXIST) return fail_w(ERROR_ALREADY_EXISTS);
    return r ? fail_k(r) : TRUE;
}
W32FN(K32, CreateDirectoryA, BOOL, (const char *p, void *sa)) { (void)sa; return mk_dir(p); }
W32FN(K32, CreateDirectoryW, BOOL, (const WCHAR *p, void *sa)) { (void)sa; WPATH(p, u); return mk_dir(u); }
static BOOL move_file(const char *a, const char *b, DWORD fl)
{
    NTPATH(a, na, FALSE);
    NTPATH(b, nb, FALSE);
    if (fl & 1) {
        struct file_stat st;
        if (sc3(SYS_FS_STAT, nb, rt_strlen(nb), &st) == 0 && st.type == FT_FILE)
            sc2(SYS_FS_REMOVE, nb, rt_strlen(nb));
    }
    long r = sc4(SYS_FS_RENAME, na, rt_strlen(na), nb, rt_strlen(nb));
    if (r == -E_EXIST) return fail_w(ERROR_ALREADY_EXISTS);
    return r ? fail_k(r) : TRUE;
}
W32FN(K32, MoveFileA, BOOL, (const char *a, const char *b)) { return move_file(a, b, 0); }
W32FN(K32, MoveFileW, BOOL, (const WCHAR *a, const WCHAR *b)) { WPATH(a, ua); WPATH(b, ub); return move_file(ua, ub, 0); }
W32FN(K32, MoveFileExA, BOOL, (const char *a, const char *b, DWORD fl)) { return move_file(a, b, fl); }
W32FN(K32, MoveFileExW, BOOL, (const WCHAR *a, const WCHAR *b, DWORD fl)) { WPATH(a, ua); WPATH(b, ub); return move_file(ua, ub, fl); }

W32FN(K32, GetCurrentDirectoryA, DWORD, (DWORD n, char *buf))
{
    char c[RT_PATH];
    if (path_cwd(c, sizeof(c))) { set_last_error(ERROR_PATH_NOT_FOUND); return 0; }
    DWORD l = (DWORD)rt_strlen(c);
    if (n <= l) return l + 1;
    memcpy(buf, c, l + 1);
    return l;
}
W32FN(K32, GetCurrentDirectoryW, DWORD, (DWORD n, WCHAR *buf))
{
    char c[RT_PATH];
    if (path_cwd(c, sizeof(c))) { set_last_error(ERROR_PATH_NOT_FOUND); return 0; }
    DWORD l = (DWORD)rt_strlen(c);
    WCHAR w[RT_PATH];
    size_t wl = rt_u2w(c, -1, w, RT_PATH);
    (void)l;
    if (n <= wl) return (DWORD)wl + 1;
    memcpy(buf, w, (wl + 1) * 2);
    return (DWORD)wl;
}
static BOOL set_cwd(const char *wp)
{
    NTPATH(wp, nt, FALSE);
    struct file_stat st;
    long r = sc3(SYS_FS_STAT, nt, rt_strlen(nt), &st);
    if (r) return fail_k(r);
    if (st.type != FT_DIR) return fail_w(ERROR_DIRECTORY);
    int rc = path_set_cwd(wp);
    return rc ? fail_k(rc) : TRUE;
}
W32FN(K32, SetCurrentDirectoryA, BOOL, (const char *p)) { return set_cwd(p); }
W32FN(K32, SetCurrentDirectoryW, BOOL, (const WCHAR *p)) { WPATH(p, u); return set_cwd(u); }
W32FN(K32, GetFullPathNameA, DWORD, (const char *p, DWORD n, char *buf, char **fp))
{
    char o[RT_PATH];
    int rc = path_full(p, o, sizeof(o));
    if (rc) { set_last_error(kerr_to_win(rc)); return 0; }
    DWORD l = (DWORD)rt_strlen(o);
    if (n <= l) return l + 1;
    memcpy(buf, o, l + 1);
    if (fp) { char *s = buf + l; while (s > buf && s[-1] != '\\') s--; *fp = s; }
    return l;
}
W32FN(K32, GetFullPathNameW, DWORD, (const WCHAR *p, DWORD n, WCHAR *buf, WCHAR **fp))
{
    WPATH(p, u);
    char o[RT_PATH];
    int rc = path_full(u, o, sizeof(o));
    if (rc) { set_last_error(kerr_to_win(rc)); return 0; }
    WCHAR w[RT_PATH];
    size_t wl = rt_u2w(o, -1, w, RT_PATH);
    if (n <= wl) return (DWORD)wl + 1;
    memcpy(buf, w, (wl + 1) * 2);
    if (fp) { WCHAR *s = buf + wl; while (s > buf && s[-1] != '\\') s--; *fp = s; }
    return (DWORD)wl;
}
W32FN(K32, GetTempPathA, DWORD, (DWORD n, char *buf))
{
    char c[RT_PATH];
    if (path_cwd(c, sizeof(c))) { set_last_error(ERROR_PATH_NOT_FOUND); return 0; }
    c[2] = '\\'; c[3] = 0;
    DWORD l = 3;
    if (n <= l) return l + 1;
    memcpy(buf, c, l + 1);
    return l;
}

struct find { bool used, wide; uint32_t h, idx; char pat[RT_PATH]; };
static struct find g_find[8];

static bool wild(const char *p, const char *s)
{
    for (;; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return true;
            for (; *s; s++) if (wild(p + 1, s)) return true;
            return wild(p + 1, s);
        }
        if (!*s || !*p) return !*s && !*p;
        char a = *p, b = *s;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (*p != '?' && a != b) return false;
    }
}

static void fill_find(const struct fs_dirent *de, bool wide, void *out)
{
    uint8_t *o = out;
    memset(o, 0, wide ? 592 : 320);
    *(DWORD *)o = win_attr(de->attr, de->type);
    *(DWORD *)(o + 28) = (DWORD)(de->size >> 32);
    *(DWORD *)(o + 32) = (DWORD)de->size;
    if (wide) rt_u2w(de->name, -1, (WCHAR *)(o + 44), 260);
    else { size_t i = 0; for (; de->name[i] && i < 259; i++) o[44 + i] = (uint8_t)de->name[i]; }
}
static bool find_next(struct find *f, void *out)
{
    struct fs_dirent *de = (struct fs_dirent *)rt_malloc(sizeof(*de));
    if (!de) return false;
    bool got = false;
    while (sc3(SYS_FILE_READDIR, f->h, f->idx++, de) == 0)
        if (wild(f->pat, de->name)) { fill_find(de, f->wide, out); got = true; break; }
    rt_free(de);
    return got;
}
static HANDLE find_first(const char *wp, bool wide, void *out)
{
    char dir[RT_PATH];
    size_t n = rt_strlen(wp);
    if (!n || n >= RT_PATH) { set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    size_t cut = n;
    while (cut > 0 && wp[cut - 1] != '\\' && wp[cut - 1] != '/' && wp[cut - 1] != ':') cut--;
    memcpy(dir, wp, cut);
    dir[cut] = 0;
    if (!cut) { dir[0] = '.'; dir[1] = 0; }
    int slot = -1;
    for (int i = 0; i < 8; i++) if (!g_find[i].used) { slot = i; break; }
    if (slot < 0) { set_last_error(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    const char *dirp = dir;
    NTPATH(dirp, nt, INVALID_HANDLE_VALUE);
    long h = sc4(SYS_FILE_OPEN, nt, rt_strlen(nt), FILE_READ_DATA, FD_OPEN | FO_DIRECTORY);
    if (h < 0) { set_last_error(h == -E_NOENT || h == -E_NOTDIR ? ERROR_PATH_NOT_FOUND : kerr_to_win(h)); return INVALID_HANDLE_VALUE; }
    struct find *f = &g_find[slot];
    f->used = true; f->wide = wide; f->h = (uint32_t)h; f->idx = 0;
    size_t k = 0;
    for (size_t i = cut; i <= n && k < sizeof(f->pat); i++) f->pat[k++] = wp[i];
    if (!f->pat[0]) { f->pat[0] = '*'; f->pat[1] = 0; }
    if (!find_next(f, out)) {
        sc1(SYS_CLOSE, f->h);
        f->used = false;
        set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return HFIND(slot);
}
W32FN(K32, FindFirstFileA, HANDLE, (const char *p, void *out)) { return find_first(p, false, out); }
W32FN(K32, FindFirstFileW, HANDLE, (const WCHAR *p, void *out)) { WPATH(p, u); return find_first(u, true, out); }
static struct find *find_of(HANDLE h)
{
    u64 v = (u64)h - 0xA000000000000000ULL;
    if ((v & 3) || v >= 32) return NULL;
    struct find *f = &g_find[v / 4];
    return f->used ? f : NULL;
}
W32FN(K32, FindNextFileA, BOOL, (HANDLE h, void *out))
{
    struct find *f = find_of(h);
    if (!f) return fail_w(ERROR_INVALID_HANDLE);
    if (!find_next(f, out)) return fail_w(ERROR_NO_MORE_FILES);
    return TRUE;
}
W32ALIAS(K32, FindNextFileW, w_FindNextFileA);
W32FN(K32, FindClose, BOOL, (HANDLE h))
{
    struct find *f = find_of(h);
    if (!f) return fail_w(ERROR_INVALID_HANDLE);
    sc1(SYS_CLOSE, f->h);
    f->used = false;
    return TRUE;
}

W32FN(K32, MultiByteToWideChar, int, (DWORD cp, DWORD fl, const char *s, int n, WCHAR *d, int cap))
{
    (void)cp; (void)fl;
    if (!s) { set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    long len = n < 0 ? (long)rt_strlen(s) + 1 : n;
    if (!cap) {
        WCHAR tmp[256];
        int total = 0;
        for (long i = 0; i < len;) {
            long c = len - i > 200 ? 200 : len - i;
            while (i + c < len && ((uint8_t)s[i + c] & 0xC0) == 0x80) c++;
            total += (int)rt_u2w(s + i, c, tmp, 256);
            i += c;
        }
        return total + (n < 0 ? 1 : 0);
    }
    size_t w = rt_u2w(s, len, d, (size_t)cap);
    if (n < 0 && w < (size_t)cap) { d[w] = 0; w++; }
    return (int)w;
}
W32FN(K32, WideCharToMultiByte, int, (DWORD cp, DWORD fl, const WCHAR *w, int n, char *d, int cap, const char *def, BOOL *used))
{
    (void)cp; (void)fl; (void)def;
    if (used) *used = FALSE;
    if (!w) { set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    long len = n;
    if (n < 0) { len = 0; while (w[len]) len++; }
    if (!cap) {
        char tmp[1024];
        int total = 0;
        for (long i = 0; i < len;) {
            long c = len - i > 200 ? 200 : len - i;
            total += (int)rt_w2u(w + i, c, tmp, sizeof(tmp));
            i += c;
        }
        return total + (n < 0 ? 1 : 0);
    }
    size_t o = rt_w2u(w, len, d, (size_t)cap);
    if (n < 0 && o < (size_t)cap) { d[o] = 0; o++; }
    return (int)o;
}
W32FN(K32, IsDBCSLeadByteEx, BOOL, (DWORD cp, uint8_t b)) { (void)cp; (void)b; return FALSE; }
W32FN(K32, lstrlenA, int, (const char *s)) { return s ? (int)rt_strlen(s) : 0; }
W32FN(K32, lstrlenW, int, (const WCHAR *s)) { int n = 0; if (s) while (s[n]) n++; return n; }
W32FN(K32, lstrcpyA, char *, (char *d, const char *s)) { char *r = d; while ((*d++ = *s++)) ; return r; }

static char g_cmdline[8192];
static WCHAR g_cmdline_w[8192];
static void build_cmdline(void)
{
    if (g_cmdline[0] || !g_su) return;
    const char *p = (const char *)g_su + g_su->args_off;
    size_t o = 0;
    for (uint32_t i = 0; i < g_su->argc; i++) {
        size_t n = rt_strlen(p);
        bool q = !n;
        for (size_t k = 0; k < n; k++) if (p[k] == ' ' || p[k] == '\t') q = true;
        if (i && o + 1 < sizeof(g_cmdline)) g_cmdline[o++] = ' ';
        if (q && o + 1 < sizeof(g_cmdline)) g_cmdline[o++] = '"';
        for (size_t k = 0; k < n && o + 2 < sizeof(g_cmdline); k++) g_cmdline[o++] = p[k];
        if (q && o + 1 < sizeof(g_cmdline)) g_cmdline[o++] = '"';
        p += n + 1;
    }
    g_cmdline[o] = 0;
    rt_u2w(g_cmdline, -1, g_cmdline_w, 8192);
}
W32FN(K32, GetCommandLineA, char *, (void)) { build_cmdline(); return g_cmdline; }
W32FN(K32, GetCommandLineW, WCHAR *, (void)) { build_cmdline(); return g_cmdline_w; }
W32FN(K32, GetStartupInfoA, void, (DWORD *si)) { memset(si, 0, 104); si[0] = 104; }
W32FN(K32, GetStartupInfoW, void, (DWORD *si)) { memset(si, 0, 104); si[0] = 104; }

W32FN(K32, GetEnvironmentVariableA, DWORD, (const char *n, char *buf, DWORD cap))
{
    const char *v = rt_env_get(n);
    if (!v) { set_last_error(203 ); return 0; }
    DWORD l = (DWORD)rt_strlen(v);
    if (cap <= l) return l + 1;
    memcpy(buf, v, l + 1);
    return l;
}
W32FN(K32, GetEnvironmentVariableW, DWORD, (const WCHAR *n, WCHAR *buf, DWORD cap))
{
    WPATH(n, un);
    const char *v = rt_env_get(un);
    if (!v) { set_last_error(203); return 0; }
    WCHAR w[2048];
    size_t wl = rt_u2w(v, -1, w, 2048);
    if (cap <= wl) return (DWORD)wl + 1;
    memcpy(buf, w, (wl + 1) * 2);
    return (DWORD)wl;
}
W32FN(K32, SetEnvironmentVariableA, BOOL, (const char *n, const char *v))
{ int rc = rt_env_set(n, v); return rc ? fail_k(rc) : TRUE; }
W32FN(K32, SetEnvironmentVariableW, BOOL, (const WCHAR *n, const WCHAR *v))
{
    WPATH(n, un);
    char uv[2048];
    if (v) rt_w2u(v, -1, uv, sizeof(uv));
    int rc = rt_env_set(un, v ? uv : NULL);
    return rc ? fail_k(rc) : TRUE;
}
W32FN(K32, GetEnvironmentStringsW, WCHAR *, (void))
{
    char **e = rt_environ();
    size_t total = 2;
    for (int i = 0; e[i]; i++) total += rt_strlen(e[i]) + 1;
    WCHAR *blk = rt_malloc(total * 2 + 4);
    if (!blk) return NULL;
    size_t o = 0;
    for (int i = 0; e[i]; i++) o += rt_u2w(e[i], -1, blk + o, total + 1 - o) + 1;
    blk[o] = 0;
    return blk;
}
W32FN(K32, FreeEnvironmentStringsW, BOOL, (WCHAR *p)) { rt_free(p); return TRUE; }
W32FN(K32, GetEnvironmentStringsA, char *, (void))
{
    char **e = rt_environ();
    size_t total = 2;
    for (int i = 0; e[i]; i++) total += rt_strlen(e[i]) + 1;
    char *blk = rt_malloc(total);
    if (!blk) return NULL;
    size_t o = 0;
    for (int i = 0; e[i]; i++) { size_t n = rt_strlen(e[i]) + 1; memcpy(blk + o, e[i], n); o += n; }
    blk[o] = 0;
    return blk;
}
W32FN(K32, FreeEnvironmentStringsA, BOOL, (char *p)) { rt_free(p); return TRUE; }
W32ALIAS(K32, GetEnvironmentStrings, w_GetEnvironmentStringsA);
W32ALIAS(K32, FreeEnvironmentStrings, w_FreeEnvironmentStringsA);

extern const struct w32_export __start_w32exp[], __stop_w32exp[];
static const char *const g_mods[] = { "kernel32", "ntdll", "msvcrt" };
static int module_index(const char *name)
{
    char n[64];
    const char *b = name, *p = name;
    for (; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    size_t k = 0;
    for (; b[k] && k < 63; k++) n[k] = (b[k] >= 'A' && b[k] <= 'Z') ? (char)(b[k] + 32) : b[k];
    n[k] = 0;
    if (k > 4 && !memcmp(n + k - 4, ".dll", 4)) n[k - 4] = 0;
    if (!memcmp(n, "kernel32", 9) || !memcmp(n, "kernelbase", 11)) return 0;
    if (!memcmp(n, "ntdll", 6)) return 1;
    if (!memcmp(n, "msvcrt", 7) || !memcmp(n, "ucrtbase", 9)) return 2;
    return -1;
}
#define MOD_HANDLE(i) ((HANDLE)(W32_RT_BASE + 0x1000ull * ((i) + 1)))
static HANDLE get_module(const char *name)
{
    if (!name) return (HANDLE)g_su->image_base;
    int i = module_index(name);
    if (i < 0) { set_last_error(ERROR_MOD_NOT_FOUND); return NULL; }
    return MOD_HANDLE(i);
}
W32FN(K32, GetModuleHandleA, HANDLE, (const char *n)) { return get_module(n); }
W32FN(K32, GetModuleHandleW, HANDLE, (const WCHAR *n)) { if (!n) return get_module(NULL); WPATH(n, u); return get_module(u); }
W32FN(K32, LoadLibraryA, HANDLE, (const char *n)) { return get_module(n); }
W32FN(K32, LoadLibraryW, HANDLE, (const WCHAR *n)) { WPATH(n, u); return get_module(u); }
W32FN(K32, LoadLibraryExA, HANDLE, (const char *n, HANDLE f, DWORD fl)) { (void)f; (void)fl; return get_module(n); }
W32FN(K32, FreeLibrary, BOOL, (HANDLE m)) { (void)m; return TRUE; }
W32FN(K32, GetProcAddress, void *, (HANDLE m, const char *name))
{
    for (int i = 0; i < 3; i++)
        if (m == MOD_HANDLE(i)) {
            if ((u64)name < 0x10000) break;
            for (const struct w32_export *e = __start_w32exp; e < __stop_w32exp; e++) {
                const char *d = (const char *)e->dll, *nm = (const char *)e->name;
                if (!memcmp(d, g_mods[i], rt_strlen(g_mods[i]) + 1) && !memcmp(nm, name, rt_strlen(name) + 1))
                    return (void *)e->addr;
            }
        }
    set_last_error(ERROR_PROC_NOT_FOUND);
    return NULL;
}
// This is the one export real Windows doesn't have, and w32min uses it to figure out if it's running on my kernel or on actual Windows, so if you "clean this up" you're breaking the whole detection you dumbass
W32FN(K32, InfinityOsVersion, DWORD, (void)) { return 0x00010000; }
static DWORD module_file(HANDLE m, char *out, DWORD cap)
{
    if (m && m != (HANDLE)g_su->image_base) { set_last_error(ERROR_MOD_NOT_FOUND); return 0; }
    const char *nt = (const char *)g_su + g_su->path_off;
    char w[RT_PATH];
    char raw[RT_PATH + 8];
    raw[0] = '\\'; raw[1] = '?'; raw[2] = '?';
    { size_t i = 0; for (; nt[i] && i < RT_PATH; i++) raw[3 + i] = nt[i]; raw[3 + i] = 0; }
    if (!nt[0] || path_full(raw, w, sizeof(w)) != 0) {
        size_t o = 0;
        const char *a = (const char *)g_su + g_su->args_off;
        for (; a[o] && o < sizeof(w) - 1; o++) w[o] = a[o];
        w[o] = 0;
    }
    DWORD l = (DWORD)rt_strlen(w);
    if (cap <= l) { set_last_error(ERROR_INSUFFICIENT_BUFFER); if (cap) { memcpy(out, w, cap - 1); out[cap - 1] = 0; } return cap; }
    memcpy(out, w, l + 1);
    return l;
}
W32FN(K32, GetModuleFileNameA, DWORD, (HANDLE m, char *out, DWORD cap)) { return module_file(m, out, cap); }
W32FN(K32, GetModuleFileNameW, DWORD, (HANDLE m, WCHAR *out, DWORD cap))
{
    char u[RT_PATH];
    DWORD l = module_file(m, u, sizeof(u));
    if (!l) return 0;
    WCHAR w[RT_PATH];
    size_t wl = rt_u2w(u, -1, w, RT_PATH);
    if (cap <= wl) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return cap; }
    memcpy(out, w, (wl + 1) * 2);
    return (DWORD)wl;
}

HANDLE rt_open(const char *p, DWORD access, DWORD disp) { return create_file(p, access, disp, 0); }
int rt_unlink(const char *p) { return del_file(p) ? 0 : -1; }
int rt_rename(const char *a, const char *b) { return move_file(a, b, 0) ? 0 : -1; }
int rt_seek(HANDLE h, i64 off, int whence, i64 *out) { return seek_k(h, off, whence, out) ? 0 : -1; }
int rt_close(HANDLE h) { return w_CloseHandle(h) ? 0 : -1; }
