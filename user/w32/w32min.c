#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int wseq(const WCHAR *a, const WCHAR *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static HANDLE out;
static void put(const char *s) { DWORD w; WriteFile(out, s, (DWORD)slen(s), &w, NULL); }
static void putnum(long long v)
{
    char b[24]; int n = 23; b[n] = 0;
    int neg = v < 0; unsigned long long u = neg ? -(unsigned long long)v : (unsigned long long)v;
    do { b[--n] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--n] = '-';
    put(b + n);
}
static int fails;
static void fail(int line, const char *expr)
{
    fails++;
    put("w32min FAIL line "); putnum(line); put(": "); put(expr); put(" (err "); putnum(GetLastError()); put(")\n");
}
#define CHECK(c) do { if (!(c)) fail(__LINE__, #c); } while (0)

static char cwd[MAX_PATH];
static char base[MAX_PATH];
static char drv;
static int native;
#define NATIVE_CHECK(c) do { if (native) CHECK(c); } while (0)

static void cat(char *d, const char *a, const char *b)
{
    size_t n = 0;
    while (*a && n < MAX_PATH - 1) d[n++] = *a++;
    while (*b && n < MAX_PATH - 1) d[n++] = *b++;
    d[n] = 0;
}
static DWORD g_err;
#define LASTERR_IS(a, b) (g_err = GetLastError(), g_err == (a) || g_err == (b))

static char g_orig[MAX_PATH], g_sandbox[MAX_PATH];
static void rm_tree(const char *dir)
{
    char pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAA fd;
    cat(pat, dir, "\\*");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (seq(fd.cFileName, ".") || seq(fd.cFileName, "..")) continue;
            { char t[MAX_PATH]; cat(t, dir, "\\"); cat(sub, t, fd.cFileName); }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(sub);
            else { SetFileAttributesA(sub, FILE_ATTRIBUTE_NORMAL); DeleteFileA(sub); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}
// Only runs on real Windows, makes a scratch folder in %TEMP% so the tests don't shit files all over whatever directory you launched this from, and sandbox_leave puts everything back
static void sandbox_enter(void)
{
    char tmp[MAX_PATH];
    GetCurrentDirectoryA(sizeof(g_orig), g_orig);
    DWORD n = GetTempPathA(sizeof(tmp), tmp);
    if (n && tmp[n - 1] == '\\') tmp[n - 1] = 0;
    cat(g_sandbox, tmp, "\\w32min-test");
    rm_tree(g_sandbox);
    CreateDirectoryA(g_sandbox, NULL);
    SetCurrentDirectoryA(g_sandbox);
    CreateDirectoryA("data", NULL);
    HANDLE f = CreateFileA("data\\in.txt", GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    DWORD w;
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, "hello from the kernel\n", 22, &w, NULL); CloseHandle(f); }
}
// Go back to the original directory and delete the sandbox, which needs a whole recursive function (rm_tree) because Windows refuses to delete a folder with anything in it, thanks Microsoft
static void sandbox_leave(void)
{
    SetCurrentDirectoryA(g_orig);
    rm_tree(g_sandbox);
}
static DWORD WINAPI thread_main(void *arg) { *(volatile LONG *)arg = 42; return 7; }

static void t_teb(void)
{
    char *teb = (char *)NtCurrentTeb();
    int local;
    CHECK(teb != NULL);
    CHECK(*(void **)(teb + 0x30) == teb);
    CHECK(*(char **)(teb + 0x08) > (char *)&local && *(char **)(teb + 0x10) < (char *)&local);
    char *peb = *(char **)(teb + 0x60);
    CHECK(peb && *(void **)(peb + 0x10) == (void *)GetModuleHandleA(NULL));
    CHECK(*(WORD *)GetModuleHandleA(NULL) == 0x5A4D);
    CHECK(*(DWORD *)(peb + 0x118) == 10);
    CHECK(GetCurrentProcessId() != 0 && GetCurrentThreadId() != 0);
    CHECK(GetCurrentProcess() == (HANDLE)-1 && GetCurrentThread() == (HANDLE)-2);

    SetLastError(1234);
    CHECK(GetLastError() == 1234);
    CHECK(*(DWORD *)(teb + 0x68) == 1234);
    SetLastError(0);

    DWORD i = TlsAlloc();
    CHECK(i != TLS_OUT_OF_INDEXES);
    CHECK(TlsGetValue(i) == NULL && GetLastError() == 0);
    CHECK(TlsSetValue(i, (void *)0x1234));
    CHECK(TlsGetValue(i) == (void *)0x1234);
    CHECK(*(void **)(teb + 0x1480 + 8 * i) == (void *)0x1234);
    CHECK(TlsFree(i));
    CHECK(!TlsFree(i) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(TlsGetValue(TLS_OUT_OF_INDEXES) == NULL && GetLastError() == ERROR_INVALID_PARAMETER);
    static DWORD all[1200]; int n = 0;
    while (n < 1200 && (all[n] = TlsAlloc()) != TLS_OUT_OF_INDEXES) n++;
    CHECK(n >= 60 && n < 1200);
    NATIVE_CHECK(n == 64);                                                           // Real Windows gives you 1088 TLS slots (64 + 1024 expansion), we give you 64, if you need more than 64 thread locals you have bigger problems
    CHECK(TlsAlloc() == TLS_OUT_OF_INDEXES);
    for (int k = 0; k < n; k++) TlsFree(all[k]);

    if (native) {
        // CreateThread just returns ERROR_NOT_SUPPORTED on my kernel, it's one thread per process and I'm not dealing with threads yet
        HANDLE th = CreateThread(NULL, 0, NULL, NULL, 0, NULL);
        CHECK(th == NULL && GetLastError() == ERROR_NOT_SUPPORTED);
    } else {
        volatile LONG flag = 0;
        DWORD tid = 0, code = 0;
        HANDLE th = CreateThread(NULL, 0, thread_main, (void *)&flag, 0, &tid);
        CHECK(th != NULL && tid != 0 && tid != GetCurrentThreadId());
        if (th) {
            CHECK(WaitForSingleObject(th, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(th, &code) && code == 7 && flag == 42);
            CloseHandle(th);
        }
    }
}

static const char rdata_probe[] = "read-only";
static int data_probe = 5;

static void t_vm(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    CHECK(si.dwPageSize == 4096 && si.dwAllocationGranularity == 65536 && si.dwNumberOfProcessors >= 1);

    char *p = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE, PAGE_READWRITE);
    CHECK(p && ((ULONG_PTR)p & 0xFFFF) == 0);
    MEMORY_BASIC_INFORMATION m;
    CHECK(VirtualQuery(p, &m, sizeof(m)) == sizeof(m));
    CHECK(m.State == MEM_RESERVE && m.RegionSize == (1 << 20) && m.AllocationBase == p && m.Type == MEM_PRIVATE && m.BaseAddress == p);

    CHECK(VirtualAlloc(p + 4096, 8192, MEM_COMMIT, PAGE_READWRITE) == p + 4096);
    CHECK(VirtualQuery(p, &m, sizeof(m)) && m.State == MEM_RESERVE && m.RegionSize == 4096);
    CHECK(VirtualQuery(p + 4096, &m, sizeof(m)) && m.State == MEM_COMMIT && m.RegionSize == 8192 &&
          m.Protect == PAGE_READWRITE && m.AllocationBase == p);
    CHECK(VirtualQuery(p + 12288, &m, sizeof(m)) && m.State == MEM_RESERVE && m.RegionSize == (1 << 20) - 12288);
    CHECK(VirtualQuery(p + 5000, &m, sizeof(m)) && m.BaseAddress == p + 4096);

    for (int i = 0; i < 8192; i++) CHECK(p[4096 + i] == 0);
    for (int i = 0; i < 8192; i++) p[4096 + i] = (char)(i * 7);
    for (int i = 0; i < 8192; i++) if (p[4096 + i] != (char)(i * 7)) { CHECK(0); break; }

    DWORD old = 0;
    CHECK(VirtualProtect(p + 4096, 4096, PAGE_READONLY, &old) && old == PAGE_READWRITE);
    CHECK(VirtualQuery(p + 4096, &m, sizeof(m)) && m.Protect == PAGE_READONLY && m.RegionSize == 4096);
    CHECK(VirtualQuery(p + 8192, &m, sizeof(m)) && m.Protect == PAGE_READWRITE && m.RegionSize == 4096);
    CHECK(p[4096 + 5] == (char)35);
    CHECK(VirtualProtect(p + 4096, 4096, PAGE_READWRITE, &old) && old == PAGE_READONLY);
    CHECK(!VirtualProtect(p + 20000, 4096, PAGE_READWRITE, &old));
    CHECK(!VirtualProtect(p, 4096, 0x1234, &old) && LASTERR_IS(ERROR_INVALID_PARAMETER, ERROR_INVALID_ADDRESS));

    CHECK(VirtualFree(p + 4096, 8192, MEM_DECOMMIT));
    CHECK(VirtualQuery(p + 4096, &m, sizeof(m)) && m.State == MEM_RESERVE && m.RegionSize == (1 << 20) - 4096);
    CHECK(VirtualAlloc(p + 4096, 4096, MEM_COMMIT, PAGE_READWRITE) == p + 4096);
    CHECK(p[4096 + 5] == 0);
    CHECK(!VirtualFree(p + 4096, 0, MEM_RELEASE) && LASTERR_IS(ERROR_INVALID_PARAMETER, ERROR_INVALID_ADDRESS));
    CHECK(!VirtualFree(p, 4096, MEM_RELEASE));
    CHECK(VirtualFree(p, 0, MEM_RELEASE));
    CHECK(VirtualQuery(p, &m, sizeof(m)) && m.State == MEM_FREE);
    CHECK(!VirtualFree(p, 0, MEM_RELEASE));

    char *q = VirtualAlloc(p, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(q == p);
    q[0] = 1; q[65535] = 2;
    CHECK(VirtualAlloc(p, 4096, MEM_RESERVE, PAGE_READWRITE) == NULL && GetLastError() == ERROR_INVALID_ADDRESS);
    CHECK(VirtualFree(q, 0, MEM_RELEASE));

    CHECK(VirtualAlloc(NULL, 0, MEM_COMMIT, PAGE_READWRITE) == NULL && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(VirtualAlloc(NULL, 4096, 0, PAGE_READWRITE) == NULL);
    CHECK(VirtualAlloc(NULL, 4096, MEM_COMMIT, 0x7777) == NULL);

    char *big = VirtualAlloc(NULL, 256u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(big != NULL);
    if (big) {
        big[0] = 'a'; big[(128u << 20) + 4097] = 'b'; big[(256u << 20) - 1] = 'c';
        CHECK(big[0] == 'a' && big[(128u << 20) + 4097] == 'b' && big[(256u << 20) - 1] == 'c' && big[4096 * 1000] == 0);
        CHECK(VirtualFree(big, 0, MEM_RELEASE));
    }
    CHECK(VirtualAlloc(NULL, (SIZE_T)1 << 41, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) == NULL);

    HMODULE self = GetModuleHandleA(NULL);
    CHECK(VirtualQuery((void *)t_vm, &m, sizeof(m)) && m.Type == MEM_IMAGE && m.State == MEM_COMMIT &&
          m.Protect == PAGE_EXECUTE_READ && m.AllocationBase == (void *)self);
    CHECK(VirtualQuery(rdata_probe, &m, sizeof(m)) && m.Type == MEM_IMAGE && m.Protect == PAGE_READONLY);
    CHECK(VirtualQuery(&data_probe, &m, sizeof(m)) && m.Type == MEM_IMAGE &&
          (m.Protect == PAGE_READWRITE || m.Protect == PAGE_WRITECOPY));
    CHECK(VirtualQuery(self, &m, sizeof(m)) && m.Protect == PAGE_READONLY);
    CHECK(VirtualQuery((void *)0x100000000000000, &m, sizeof(m)) == 0);

    void *v[400]; int n = 0;
    while (n < 400 && (v[n] = VirtualAlloc(NULL, 65536, MEM_RESERVE, PAGE_READWRITE)) != NULL) n++;
    if (native) CHECK(n > 40 && n < 400 && GetLastError() == ERROR_NOT_ENOUGH_MEMORY);
    else CHECK(n == 400);
    for (int k = 0; k < n; k++) CHECK(VirtualFree(v[k], 0, MEM_RELEASE));
    CHECK(VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != NULL);
}

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245 + 12345; return (rng >> 8) & 0xFFFFFF; }

static void t_heap(void)
{
    HANDLE h = GetProcessHeap();
    CHECK(h != NULL);
    enum { N = 600 };
    static unsigned char *blk[N];
    static unsigned sz[N];
    for (int i = 0; i < N; i++) {
        sz[i] = 1 + rnd() % 3000;
        blk[i] = HeapAlloc(h, 0, sz[i]);
        CHECK(blk[i] != NULL && ((ULONG_PTR)blk[i] & 15) == 0 && HeapSize(h, 0, blk[i]) >= sz[i]);
        for (unsigned k = 0; k < sz[i]; k++) blk[i][k] = (unsigned char)(i + k);
    }
    int bad = 0;
    for (int i = 0; i < N; i++)
        for (unsigned k = 0; k < sz[i]; k++) if (blk[i][k] != (unsigned char)(i + k)) bad++;
    CHECK(bad == 0);
    for (int i = 0; i < N; i += 2) { CHECK(HeapFree(h, 0, blk[i])); blk[i] = NULL; }
    unsigned char *g = HeapReAlloc(h, 0, blk[1], 20000);
    CHECK(g != NULL);
    bad = 0;
    for (unsigned k = 0; k < sz[1]; k++) if (g[k] != (unsigned char)(1 + k)) bad++;
    CHECK(bad == 0);
    blk[1] = g;
    unsigned char *z = HeapAlloc(h, HEAP_ZERO_MEMORY, 2500);
    bad = 0;
    for (int k = 0; k < 2500; k++) if (z[k]) bad++;
    CHECK(bad == 0);
    HeapFree(h, 0, z);
    for (int i = 1; i < N; i += 2) HeapFree(h, 0, blk[i]);

    unsigned char *big = HeapAlloc(h, 0, 900 * 1024);
    CHECK(big != NULL);
    if (big) { big[0] = 1; big[900 * 1024 - 1] = 2; CHECK(HeapSize(h, 0, big) >= 900 * 1024); HeapFree(h, 0, big); }
    unsigned char *c = HeapAlloc(h, HEAP_ZERO_MEMORY, 3 << 20);
    CHECK(c && c[(3 << 20) - 1] == 0);
    c = HeapReAlloc(h, HEAP_ZERO_MEMORY, c, 5 << 20);
    CHECK(c && c[(4 << 20)] == 0);
    HeapFree(h, 0, c);
    CHECK(HeapAlloc(h, 0, (SIZE_T)1 << 62) == NULL);                                 // Asking for 4 EiB of heap, which has to fail cleanly instead of taking the entire heap down with it

    HANDLE h2 = HeapCreate(0, 0, 0);
    CHECK(h2 != NULL && h2 != h);
    char *s = HeapAlloc(h2, 0, 100);
    CHECK(s != NULL);
    if (s) { s[0] = 'x'; HeapFree(h2, 0, s); }
    HLOCAL l = LocalAlloc(LMEM_ZEROINIT, 64);
    CHECK(l && ((char *)l)[63] == 0 && LocalFree(l) == NULL);
}

static HANDLE open_r(const char *p) { return CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); }

static int count_find(const char *pat, char names[][64], int max)
{
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return -1;
    int n = 0;
    do {
        if (seq(fd.cFileName, ".") || seq(fd.cFileName, "..")) continue;
        if (n < max) { int k = 0; for (; fd.cFileName[k] && k < 63; k++) names[n][k] = fd.cFileName[k]; names[n][k] = 0; }
        n++;
    } while (FindNextFileA(f, &fd));
    CHECK(GetLastError() == ERROR_NO_MORE_FILES);
    CHECK(FindClose(f));
    return n;
}
static int has_name(char names[][64], int n, const char *w)
{
    for (int i = 0; i < n && i < 16; i++) {
        const char *a = names[i], *b = w; int eq = 1;
        for (; *a || *b; a++, b++) {
            char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32;
            if (x != y) { eq = 0; break; }
        }
        if (eq) return 1;
    }
    return 0;
}

static void t_files(void)
{
    char buf[512], path[MAX_PATH];
    DWORD n;
    DWORD cl = GetCurrentDirectoryA(sizeof(cwd), cwd);
    CHECK(cl == (DWORD)slen(cwd) && cwd[1] == ':' && cwd[2] == '\\');
    NATIVE_CHECK(cl == 3);
    drv = cwd[0];
    cat(base, cwd, "");
    if (slen(base) == 3) base[2] = 0;
    char exp[MAX_PATH], full_in[MAX_PATH];
    cat(full_in, base, "\\data\\in.txt");

    const char *ways[] = { "data\\in.txt", "data/in.txt", ".\\data\\..\\data\\in.txt", "DATA\\IN.TXT", "\\data\\in.txt", NULL };
    if (!native) ways[4] = full_in;
    for (int i = 0; ways[i]; i++) {
        HANDLE h = open_r(ways[i]);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) {
            CHECK(ReadFile(h, buf, sizeof(buf), &n, NULL) && n == 22 && !memcmp(buf, "hello from the kernel\n", 22));
            CHECK(GetFileType(h) == FILE_TYPE_DISK);
            CHECK(ReadFile(h, buf, 10, &n, NULL) && n == 0);
            CHECK(CloseHandle(h));
        }
    }
    HANDLE h = open_r(full_in);
    CHECK(h != INVALID_HANDLE_VALUE);
    CloseHandle(h);
    CHECK(GetFileAttributesA("data\\in.txt") != INVALID_FILE_ATTRIBUTES);
    CHECK((GetFileAttributesA("data") & FILE_ATTRIBUTE_DIRECTORY) != 0);
    CHECK(GetFileAttributesA("data\\nope.txt") == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);
    CHECK(GetFileAttributesA("nodir\\nope.txt") == INVALID_FILE_ATTRIBUTES);
    CHECK(open_r("data\\nope.txt") == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND);
    NATIVE_CHECK(GetFileAttributesA("Z:\\x") == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_PATH_NOT_FOUND);

    WIN32_FILE_ATTRIBUTE_DATA ad;
    CHECK(GetFileAttributesExA("data\\in.txt", GetFileExInfoStandard, &ad) && ad.nFileSizeLow == 22 && ad.nFileSizeHigh == 0);
    CHECK(ad.ftLastWriteTime.dwHighDateTime > 0x01D00000);

    if (GetFileAttributesA("out") == INVALID_FILE_ATTRIBUTES) CHECK(CreateDirectoryA("out", NULL));
    CHECK(!CreateDirectoryA("out", NULL) && GetLastError() == ERROR_ALREADY_EXISTS);
    HANDLE f = CreateFileA("out\\a.txt", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(f != INVALID_HANDLE_VALUE);
    CHECK(CreateFileA("out\\a.txt", GENERIC_READ, 0, NULL, CREATE_NEW, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS);
    for (int i = 0; i < 100; i++) buf[i] = (char)('A' + i % 26);
    CHECK(WriteFile(f, buf, 100, &n, NULL) && n == 100);
    CHECK(GetFileSize(f, NULL) == 100);
    LARGE_INTEGER sz; CHECK(GetFileSizeEx(f, &sz) && sz.QuadPart == 100);
    CHECK(SetFilePointer(f, 0, NULL, FILE_BEGIN) == 0);
    char rb[128];
    CHECK(ReadFile(f, rb, 100, &n, NULL) && n == 100 && !memcmp(rb, buf, 100));
    LARGE_INTEGER d, np; d.QuadPart = -10;
    CHECK(SetFilePointerEx(f, d, &np, FILE_END) && np.QuadPart == 90);
    CHECK(ReadFile(f, rb, 100, &n, NULL) && n == 10 && !memcmp(rb, buf + 90, 10));
    d.QuadPart = -1;
    CHECK(!SetFilePointerEx(f, d, &np, FILE_BEGIN) && GetLastError() == ERROR_NEGATIVE_SEEK);
    CHECK(SetFilePointer(f, 50, NULL, FILE_BEGIN) == 50 && SetEndOfFile(f) && GetFileSize(f, NULL) == 50);
    d.QuadPart = 0; CHECK(SetFilePointerEx(f, d, &np, FILE_END) && np.QuadPart == 50);
    CHECK(FlushFileBuffers(f));
    CHECK(CloseHandle(f));
    CHECK(!CloseHandle(f) && GetLastError() == ERROR_INVALID_HANDLE);

    f = CreateFileA("out\\a.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS && GetFileSize(f, NULL) == 0);
    CloseHandle(f);
    f = CreateFileA("out\\new.txt", GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE && GetLastError() == 0);
    WriteFile(f, "xyz", 3, &n, NULL);
    CloseHandle(f);
    f = CreateFileA("out\\new.txt", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS && GetFileSize(f, NULL) == 3);
    CloseHandle(f);
    f = CreateFileA("out\\new.txt", GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE && GetFileSize(f, NULL) == 0);
    CloseHandle(f);
    CHECK(CreateFileA("out\\none.txt", GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE);
    CHECK(CreateFileA("out\\none.txt", GENERIC_READ, 0, NULL, TRUNCATE_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE &&
          LASTERR_IS(ERROR_INVALID_PARAMETER, ERROR_FILE_NOT_FOUND));
    CHECK(CreateFileA("out\\none.txt", GENERIC_READ, 0, NULL, 99, 0, NULL) == INVALID_HANDLE_VALUE);

    CHECK(CreateFileA("out", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED);
    f = CreateFileA("out", GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    CHECK(f != INVALID_HANDLE_VALUE);
    CloseHandle(f);

    CHECK(MoveFileA("out\\a.txt", "out\\b.txt"));
    CHECK(GetFileAttributesA("out\\a.txt") == INVALID_FILE_ATTRIBUTES && GetFileAttributesA("out\\b.txt") != INVALID_FILE_ATTRIBUTES);
    CHECK(!MoveFileA("out\\b.txt", "out\\new.txt"));
    CHECK(MoveFileExA("out\\b.txt", "out\\new.txt", MOVEFILE_REPLACE_EXISTING));
    CHECK(GetFileAttributesA("out\\b.txt") == INVALID_FILE_ATTRIBUTES);
    CHECK(!MoveFileA("out\\ghost", "out\\x") && GetLastError() == ERROR_FILE_NOT_FOUND);
    CHECK(!DeleteFileA("out") && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(!RemoveDirectoryA("out") && GetLastError() == ERROR_DIR_NOT_EMPTY);
    CHECK(!RemoveDirectoryA("out\\new.txt") && GetLastError() == ERROR_DIRECTORY);

    static char names[24][64];
    CHECK(CreateDirectoryA("out\\sub", NULL));
    HANDLE t1 = CreateFileA("out\\x1.dat", GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL); CloseHandle(t1);
    t1 = CreateFileA("out\\x22.dat", GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL); CloseHandle(t1);
    t1 = CreateFileA("out\\A Long File Name, with spaces.and.dots.txt", GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    CHECK(t1 != INVALID_HANDLE_VALUE); CloseHandle(t1);
    int c = count_find("out\\*", names, 24);
    CHECK(c == 5 && has_name(names, c, "new.txt") && has_name(names, c, "sub") && has_name(names, c, "x1.dat") &&
          has_name(names, c, "A Long File Name, with spaces.and.dots.txt"));
    c = count_find("out\\*.dat", names, 24);
    CHECK(c == 2 && has_name(names, c, "x1.dat") && has_name(names, c, "x22.dat"));
    c = count_find("out\\x?.dat", names, 24);
    CHECK(c == 1 && has_name(names, c, "x1.dat"));
    c = count_find("out\\X1.*", names, 24);
    CHECK(c == 1);
    CHECK(count_find("out\\*.zzz", names, 24) == -1 && GetLastError() == ERROR_FILE_NOT_FOUND);
    CHECK(count_find("nodir\\*", names, 24) == -1 && GetLastError() == ERROR_PATH_NOT_FOUND);
    {
        WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA("out\\x22.dat", &fd);
        CHECK(fh != INVALID_HANDLE_VALUE && seq(fd.cFileName, "x22.dat") && fd.nFileSizeLow == 0 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
        FindClose(fh);
        fh = FindFirstFileA("out\\sub", &fd);
        CHECK(fh != INVALID_HANDLE_VALUE && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
        FindClose(fh);
        if (native) CHECK(!FindClose(fh) && !FindNextFileA(fh, &fd));
    }

    CHECK(SetCurrentDirectoryA("out"));
    cat(exp, base, "\\out");
    CHECK(GetCurrentDirectoryA(sizeof(path), path) == (DWORD)slen(exp) && seq(path, exp));
    f = open_r("new.txt"); CHECK(f != INVALID_HANDLE_VALUE); CloseHandle(f);
    cat(exp, base, "\\out\\x1.dat");
    CHECK(GetFullPathNameA("sub\\..\\x1.dat", sizeof(path), path, NULL) == (DWORD)slen(exp) && seq(path, exp));
    CHECK(SetCurrentDirectoryA(".."));
    CHECK(GetCurrentDirectoryA(sizeof(path), path) == (DWORD)slen(cwd) && seq(path, cwd));
    CHECK(!SetCurrentDirectoryA("out\\new.txt") && !SetCurrentDirectoryA("nonexistent"));
    CHECK(GetFullPathNameA("out\\..\\data\\in.txt", sizeof(path), path, NULL) == (DWORD)slen(full_in) && seq(path, full_in));
    CHECK(GetFullPathNameA("data\\in.txt", 4, path, NULL) == (DWORD)slen(full_in) + 1);

    if (native) f = open_r("\\\\?\\Volumes\\w32\\data\\in.txt");
    else { char raw[MAX_PATH]; cat(raw, "\\\\?\\", full_in); f = open_r(raw); }
    CHECK(f != INVALID_HANDLE_VALUE); CloseHandle(f);
    static const WCHAR uname[] = { 'o','u','t','\\','c','a','f',0x00E9,' ',0x20AC,0xD834,0xDD1E,'.','t','x','t',0 };
    f = CreateFileW(uname, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE); if (f != INVALID_HANDLE_VALUE) { WriteFile(f, "u", 1, &n, NULL); CloseHandle(f); }
    {
        WIN32_FIND_DATAW fw; static const WCHAR pat[] = { 'o','u','t','\\','c','a','f','*',0 };
        HANDLE fh = FindFirstFileW(pat, &fw);
        CHECK(fh != INVALID_HANDLE_VALUE && wseq(fw.cFileName, uname + 4) && fw.nFileSizeLow == 1);
        FindClose(fh);
        CHECK(GetFileAttributesW(uname) != INVALID_FILE_ATTRIBUTES);
        CHECK(DeleteFileW(uname) && GetFileAttributesW(uname) == INVALID_FILE_ATTRIBUTES);
    }

    f = CreateFileA("out\\big.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE);
    enum { BIG = 300 * 1024 + 123 };
    unsigned char *bb = HeapAlloc(GetProcessHeap(), 0, BIG);
    for (int i = 0; i < BIG; i++) bb[i] = (unsigned char)(i * 31 + (i >> 8));
    CHECK(WriteFile(f, bb, BIG, &n, NULL) && n == BIG && GetFileSize(f, NULL) == BIG);
    unsigned char *cc = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, BIG);
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    CHECK(ReadFile(f, cc, BIG, &n, NULL) && n == BIG && !memcmp(bb, cc, BIG));
    for (int k = 0; k < 20; k++) {
        DWORD off = rnd() % (BIG - 100);
        SetFilePointer(f, (LONG)off, NULL, FILE_BEGIN);
        CHECK(ReadFile(f, rb, 100, &n, NULL) && n == 100 && !memcmp(rb, bb + off, 100));
    }
    CloseHandle(f);
    HeapFree(GetProcessHeap(), 0, bb); HeapFree(GetProcessHeap(), 0, cc);

    CHECK(!ReadFile(INVALID_HANDLE_VALUE, rb, 1, &n, NULL) && GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(!WriteFile((HANDLE)0x1234, rb, 1, &n, NULL) && GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(GetFileSize(NULL, NULL) == INVALID_FILE_SIZE);
    CHECK(!CloseHandle((HANDLE)0x0FFFFFFC));
    CHECK(GetFileType(INVALID_HANDLE_VALUE) == FILE_TYPE_UNKNOWN);
}

static void t_sync(void)
{
    HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(e != NULL && ((ULONG_PTR)e & 3) == 0);
    CHECK(WaitForSingleObject(e, 0) == WAIT_TIMEOUT);
    CHECK(SetEvent(e));
    CHECK(WaitForSingleObject(e, 0) == WAIT_OBJECT_0 && WaitForSingleObject(e, INFINITE) == WAIT_OBJECT_0);
    CHECK(ResetEvent(e) && WaitForSingleObject(e, 0) == WAIT_TIMEOUT);
    HANDLE dup = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), e, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS) && dup && dup != e);
    SetEvent(e);
    CHECK(CloseHandle(e));
    CHECK(WaitForSingleObject(dup, 0) == WAIT_OBJECT_0);
    CHECK(CloseHandle(dup));
    HANDLE a = CreateEventA(NULL, FALSE, TRUE, NULL);
    CHECK(WaitForSingleObject(a, 0) == WAIT_OBJECT_0 && WaitForSingleObject(a, 0) == WAIT_TIMEOUT);
    CloseHandle(a);

    HANDLE s = CreateSemaphoreA(NULL, 2, 3, NULL);
    CHECK(s != NULL);
    CHECK(WaitForSingleObject(s, 0) == 0 && WaitForSingleObject(s, 0) == 0 && WaitForSingleObject(s, 0) == WAIT_TIMEOUT);
    LONG prev = -1;
    CHECK(ReleaseSemaphore(s, 1, &prev) && prev == 0);
    CHECK(ReleaseSemaphore(s, 2, &prev) && prev == 1);
    CHECK(!ReleaseSemaphore(s, 1, &prev));
    CloseHandle(s);

    NATIVE_CHECK(WaitForSingleObject(INVALID_HANDLE_VALUE, 0) == WAIT_FAILED);
    CHECK(WaitForSingleObject((HANDLE)0x0FFFFFFC, 0) == WAIT_FAILED);
    NATIVE_CHECK(WaitForSingleObject(GetStdHandle(STD_OUTPUT_HANDLE), 0) == WAIT_FAILED);

    ULONGLONG t0 = GetTickCount64();
    HANDLE w = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(WaitForSingleObject(w, 30) == WAIT_TIMEOUT);
    CHECK(GetTickCount64() - t0 >= (native ? 25u : 14u));
    t0 = GetTickCount64(); Sleep(40);
    CHECK(GetTickCount64() - t0 >= (native ? 35u : 24u) && GetTickCount64() - t0 < 2000);
    Sleep(0);
    NATIVE_CHECK(SwitchToThread());
    CloseHandle(w);

    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs); EnterCriticalSection(&cs);
    CHECK(TryEnterCriticalSection(&cs));
    LeaveCriticalSection(&cs); LeaveCriticalSection(&cs); LeaveCriticalSection(&cs);
    DeleteCriticalSection(&cs);
    CHECK(InitializeCriticalSectionAndSpinCount(&cs, 100));
    DeleteCriticalSection(&cs);
    volatile LONG v = 5;
    CHECK(InterlockedIncrement(&v) == 6 && InterlockedDecrement(&v) == 5 && InterlockedExchange(&v, 9) == 5 &&
          InterlockedExchangeAdd(&v, 3) == 9 && InterlockedCompareExchange(&v, 1, 12) == 12 && v == 1);
}

static void t_time(void)
{
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    CHECK(t > 132223104000000000ull && t < 160000000000000000ull);
    SYSTEMTIME st; GetSystemTime(&st);
    CHECK(st.wYear >= 2024 && st.wYear < 2100 && st.wMonth >= 1 && st.wMonth <= 12 && st.wDay >= 1 && st.wDay <= 31 &&
          st.wHour < 24 && st.wMinute < 60 && st.wSecond < 60 && st.wDayOfWeek < 7);
    GetLocalTime(&st);
    CHECK(st.wYear >= 2024);
    LARGE_INTEGER f, c1, c2;
    CHECK(QueryPerformanceFrequency(&f) && f.QuadPart > 0);
    QueryPerformanceCounter(&c1); Sleep(5); QueryPerformanceCounter(&c2);
    CHECK(c2.QuadPart > c1.QuadPart && (c2.QuadPart - c1.QuadPart) * 1000 / f.QuadPart >= 4);
    DWORD a = GetTickCount(); Sleep(10);
    CHECK(GetTickCount() - a >= (native ? 9u : 0u));
}

static void t_text(void)
{
    static const char u8[] = "h\xC3\xA9llo \xE2\x82\xAC \xF0\x9D\x84\x9E";
    WCHAR w[32];
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, 32);
    CHECK(n == 11 && w[1] == 0x00E9 && w[6] == 0x20AC && w[8] == 0xD834 && w[9] == 0xDD1E && w[10] == 0);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0) == 11);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, "abc", 3, w, 32) == 3 && w[0] == 'a' && w[2] == 'c');
    w[3] = 0;
    char back[32];
    CHECK(WideCharToMultiByte(CP_UTF8, 0, w, -1, back, sizeof(back), NULL, NULL) == 4 && seq(back, "abc"));
    MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, 32);
    CHECK(WideCharToMultiByte(CP_UTF8, 0, w, -1, back, sizeof(back), NULL, NULL) == (int)sizeof(u8) && seq(back, u8));
    CHECK(WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL) == (int)sizeof(u8));
    CHECK(MultiByteToWideChar(CP_UTF8, 0, "\xC3", 1, w, 32) == 1 && w[0] == 0xFFFD);
    CHECK(lstrlenA("four") == 4 && lstrlenW(w) >= 1 && lstrlenA("") == 0);
    NATIVE_CHECK(!IsDBCSLeadByteEx(CP_ACP, 0x81));
}

static void t_env(void)
{
    char b[64];
    if (!native) SetEnvironmentVariableA("W32TEST", "1");
    CHECK(GetEnvironmentVariableA("W32TEST", b, sizeof(b)) == 1 && b[0] == '1');
    CHECK(GetEnvironmentVariableA("w32test", b, sizeof(b)) == 1);
    CHECK(GetEnvironmentVariableA("NOPE_NOT_SET", b, sizeof(b)) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND);
    CHECK(SetEnvironmentVariableA("W32X", "value-1") && GetEnvironmentVariableA("W32X", b, sizeof(b)) == 7 && seq(b, "value-1"));
    CHECK(SetEnvironmentVariableA("w32x", "v2") && GetEnvironmentVariableA("W32X", b, sizeof(b)) == 2);
    CHECK(GetEnvironmentVariableA("W32X", b, 2) == 3);
    CHECK(SetEnvironmentVariableA("W32X", NULL) && GetEnvironmentVariableA("W32X", b, sizeof(b)) == 0);
    static const WCHAR wn[] = { 'W','3','2','T','E','S','T',0 };
    WCHAR wb[16];
    CHECK(GetEnvironmentVariableW(wn, wb, 16) == 1 && wb[0] == '1');
    LPWCH es = GetEnvironmentStringsW();
    int found = 0;
    for (LPWCH p = es; es && *p; p += lstrlenW(p) + 1) if (p[0] == 'W' && p[1] == '3' && p[7] == '=') found = 1;
    CHECK(found && FreeEnvironmentStringsW(es));
    char *as = GetEnvironmentStringsA(); CHECK(as && as[0]); FreeEnvironmentStringsA(as);
}

static void t_modules(void)
{
    HMODULE k = GetModuleHandleA("kernel32.dll");
    CHECK(k != NULL && GetModuleHandleA("KERNEL32") == k && GetModuleHandleW(L"kernel32.dll") == k);
    CHECK(LoadLibraryA("kernel32.dll") == k && FreeLibrary(k));
    typedef ULONGLONG (WINAPI *tick_t)(void);
    tick_t tk = (tick_t)GetProcAddress(k, "GetTickCount64");
    CHECK(tk != NULL && tk() > 0);
    CHECK(GetProcAddress(k, "NoSuchFunction") == NULL && GetLastError() == ERROR_PROC_NOT_FOUND);
    CHECK(GetModuleHandleA("nosuch.dll") == NULL && GetLastError() == ERROR_MOD_NOT_FOUND);
    CHECK(LoadLibraryA("nosuch.dll") == NULL);
    CHECK(GetModuleHandleA("ntdll.dll") != NULL && GetModuleHandleA("msvcrt.dll") != NULL);
    char name[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, name, sizeof(name));
    CHECK(n > 0 && n == (DWORD)slen(name));
    CHECK(GetModuleFileNameA(NULL, name, 3) == 3 && GetLastError() == ERROR_INSUFFICIENT_BUFFER);

    char cn[32]; DWORD cs = sizeof(cn);
    SetLastError(0);
    if (native) CHECK(GetComputerNameA(cn, &cs) == FALSE && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED);
    else CHECK(GetComputerNameA(cn, &cs) && cs > 0 && cs == (DWORD)slen(cn));
}

static void t_console(void)
{
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE), i = GetStdHandle(STD_INPUT_HANDLE), e = GetStdHandle(STD_ERROR_HANDLE);
    CHECK(o != INVALID_HANDLE_VALUE && o != NULL);
    NATIVE_CHECK(i != INVALID_HANDLE_VALUE && e != INVALID_HANDLE_VALUE && o != i && o != e);
    CHECK(GetStdHandle((DWORD)-99) == INVALID_HANDLE_VALUE);
    DWORD n, mode;
    int is_console = GetFileType(o) == FILE_TYPE_CHAR;
    NATIVE_CHECK(is_console);
    if (is_console) CHECK(GetConsoleMode(o, &mode) && SetConsoleMode(o, mode));
    CHECK(WriteFile(o, "console-ok\n", 11, &n, NULL) && n == 11);
    static const WCHAR ws[] = { 'w','i','d','e',' ',0x20AC,'\n',0 };
    if (is_console) CHECK(WriteConsoleW(o, ws, 7, &n, NULL) && n == 7);
    char b[8];
    if (native) {
        CHECK(ReadFile(i, b, 8, &n, NULL) && n == 0);
        CHECK(!WriteFile(i, b, 1, &n, NULL));
        CHECK(GetConsoleOutputCP() != 0);
        CHECK(CloseHandle(o));
        CHECK(WriteFile(o, "", 0, &n, NULL) && n == 0);
    }
    char *cl = GetCommandLineA();
    CHECK(cl && cl[0] && GetCommandLineW()[0] != 0);
    STARTUPINFOA si; GetStartupInfoA(&si); CHECK(si.cb == sizeof(si));
    OutputDebugStringA("w32min: debug string\n");
    CHECK(GetVersion() & 0xFF);
    OSVERSIONINFOA vi; vi.dwOSVersionInfoSize = sizeof(vi);
    CHECK(GetVersionExA(&vi));
    NATIVE_CHECK(vi.dwMajorVersion == 10);
    CHECK(IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE) && !IsDebuggerPresent());
    if (native) CHECK(GetACP() == 65001);
    else CHECK(GetACP() != 0);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    NATIVE_CHECK(SetUnhandledExceptionFilter(NULL) == NULL);
    CHECK(ev != NULL);
    CloseHandle(ev);
}

extern __declspec(dllimport) int sprintf(char *, const char *, ...);
extern __declspec(dllimport) int _snprintf(char *, size_t, const char *, ...);
extern __declspec(dllimport) int printf(const char *, ...);
extern __declspec(dllimport) int puts(const char *);
extern __declspec(dllimport) long strtol(const char *, char **, int);
extern __declspec(dllimport) void *malloc(size_t);
extern __declspec(dllimport) void free(void *);
extern __declspec(dllimport) int atoi(const char *);

static void t_msvcrt(void)
{
    char b[160];
    volatile int i42 = 42, i255 = 255;
    volatile const char *s = "abc";
    CHECK(sprintf(b, "%d|%5d|%-5d|%05d|%+d|%x|%X|%#x|%o", i42, i42, i42, i42, i42, i255, i255, i255, 8) == 38 &&
          seq(b, "42|   42|42   |00042|+42|ff|FF|0xff|10"));
    CHECK(sprintf(b, "%s|%10s|%-10s|%.2s|%c|%%", s, s, s, s, 'z') == 32 && seq(b, "abc|       abc|abc       |ab|z|%"));
    CHECK(sprintf(b, "%lld|%llu|%I64d|%u|%hd", -1234567890123LL, 18446744073709551615ULL, 5LL, 4000000000u, 70000) > 0 &&
          seq(b, "-1234567890123|18446744073709551615|5|4000000000|4464"));
    if (native) CHECK(sprintf(b, "%hhu", 300) > 0 && seq(b, "44"));
    volatile double pi = 3.14159, h = 2.5, neg = -0.125;
    CHECK(sprintf(b, "%.2f|%8.3f|%f|%.0f|%f", pi, h, neg, h, 1e15) > 0 && seq(b, "3.14|   2.500|-0.125000|3|1000000000000000.000000"));
    if (native) {
        CHECK(_snprintf(b, 6, "0123456789") == 10 && seq(b, "01234"));
        CHECK(_snprintf(NULL, 0, "%d", 12345) == 5);
    }
    CHECK(_snprintf(b, sizeof(b), "%d", 12345) == 5 && seq(b, "12345"));
    CHECK(sprintf(b, "%*d|%-*d|%.*s", 6, 42, 4, 7, 2, "xyz") == 14 && seq(b, "    42|7   |xy"));
    CHECK(sprintf(b, "%p", (void *)0x1234) == 16 && seq(b, "0000000000001234"));
    if (native) CHECK(sprintf(b, "%ls|%c", L"w\x00e9", 'q') > 0 && seq(b, "w\xC3\xA9|q"));
    CHECK(sprintf(b, "%ls|%c", L"wx", 'q') == 4 && seq(b, "wx|q"));
    CHECK(sprintf(b, "%s|%d", (char *)0, 1) > 0 && seq(b, "(null)|1"));
    CHECK(strtol("-12abc", NULL, 10) == -12 && atoi("77") == 77);
    char *m = malloc(1000); CHECK(m != NULL); free(m);
    printf("msvcrt printf: %d %s %.1f\n", 7, "ok", 0.5);
    puts("msvcrt puts");
}

static volatile double dsink;
static double mix(double x, int i) { return x * 1.0000001 + (double)(i & 7) * 0.5 - (double)((i * 3) & 15) * 0.25; }

static void t_fpu(void)
{
    double a = 1.5, b = 2.25;
    CHECK(a * b == 3.375 && a / b > 0.6666 && a / b < 0.6667);
    float f = 1.0f / 3.0f;
    CHECK(f * 3.0f > 0.999f && f * 3.0f < 1.001f);
    unsigned short cw; __asm__ volatile("fnstcw %0" : "=m"(cw));
    CHECK((cw & 0x300) == 0x200);
    unsigned mxcsr; __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    CHECK((mxcsr & 0xFFC0) == 0x1F80);
    long double ld = 1.0L; ld /= 3.0L;
    dsink = (double)ld;
    double s = 0; for (int i = 0; i < 1000; i++) s += (double)i * 0.5;
    CHECK(s == 249750.0);
}

static void stress(int id)
{
    char name[] = "out2\\s0.bin";
    name[6] = (char)('0' + id);
    rng = 1000u + (unsigned)id;
    for (int round = 0; round < 12; round++) {
        DWORD size = 20000 + rnd() % 50000, n;
        HANDLE f = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        CHECK(f != INVALID_HANDLE_VALUE);
        unsigned char *a = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        unsigned char *b = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        CHECK(a && b);
        if (!a || !b || f == INVALID_HANDLE_VALUE) break;
        for (DWORD i = 0; i < size; i++) a[i] = (unsigned char)(i * 13 + id + round);
        CHECK(WriteFile(f, a, size, &n, NULL) && n == size);
        SetFilePointer(f, 0, NULL, FILE_BEGIN);
        CHECK(ReadFile(f, b, size, &n, NULL) && n == size && !memcmp(a, b, size));
        void *hp[40];
        for (int i = 0; i < 40; i++) { hp[i] = HeapAlloc(GetProcessHeap(), 0, 16 + rnd() % 4000); if (hp[i]) *(char *)hp[i] = 1; }
        for (int i = 0; i < 40; i++) HeapFree(GetProcessHeap(), 0, hp[i]);
        CloseHandle(f);
        CHECK(VirtualFree(a, 0, MEM_RELEASE) && VirtualFree(b, 0, MEM_RELEASE));
        Sleep(rnd() % 3);
    }
    CHECK(DeleteFileA(name));
    ExitProcess((UINT)fails);
}

static char *g_tok[8];
static int g_ntok;
static void split_cmdline(void)
{
    static char buf[1024];
    const char *p = GetCommandLineA();
    size_t o = 0;
    while (*p && g_ntok < 8) {
        while (*p == ' ') p++;
        if (!*p) break;
        g_tok[g_ntok++] = buf + o;
        int q = 0;
        for (; *p && (q || *p != ' ') && o < sizeof(buf) - 1; p++) { if (*p == '"') q = !q; else buf[o++] = *p; }
        buf[o++] = 0;
    }
}

#pragma GCC diagnostic ignored "-Winfinite-recursion"
static __attribute__((noinline)) int recurse(int n) { volatile char pad[256]; pad[0] = (char)n; return recurse(n + 1) + pad[0]; }
static volatile int zero;
static const char ro_data[16] = "ro";

static int atoi_(const char *s) { int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; return v; }

static void spin_fpu(int seed)
{
    ULONGLONG t0 = GetTickCount64();
    char *teb = (char *)NtCurrentTeb();
    double x = 1.0 + seed, y = 1.0 + seed;
    int bad = 0, iters = 0;
    while (GetTickCount64() - t0 < 250) {
        for (int i = 0; i < 2000; i++) { x = mix(x, i); y = mix(y, i); }
        iters++;
        if (x != y) bad++;
        if (*(void **)(teb + 0x30) != (void *)teb) bad++;
        __asm__ volatile("" ::: "memory");
    }
    double ref = 1.0 + seed;
    for (int n = 0; n < iters; n++) for (int i = 0; i < 2000; i++) ref = mix(ref, i);
    if (ref != x) bad++;
    ExitProcess(bad);
}

void entry(void)
{
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    split_cmdline();
    const char *mode = g_ntok > 1 ? g_tok[1] : "api";
    if (seq(mode, "api")) {
        // Real Windows has never heard of InfinityOsVersion so if kernel32 doesn't export it we're on actual Windows, and if it does we're running on my kernel
        native = GetProcAddress(GetModuleHandleA("kernel32.dll"), "InfinityOsVersion") != NULL;
        if (!native) sandbox_enter();
        t_teb(); t_vm(); t_heap(); t_files(); t_sync(); t_time(); t_text(); t_env(); t_modules(); t_console(); t_msvcrt(); t_fpu();
        if (!native) {
            sandbox_leave();
            put("w32min (Windows): "); putnum(fails); put(" failure(s)\n");
        }
        ExitProcess((UINT)fails);
    }
    if (seq(mode, "exit")) ExitProcess((UINT)atoi_(g_tok[2]));
    if (seq(mode, "base")) ExitProcess((UINT)((ULONG_PTR)GetModuleHandleA(NULL) >> 20));
    if (seq(mode, "stress")) stress(atoi_(g_tok[2]));
    if (seq(mode, "modname")) {
        char nm[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, nm, sizeof(nm));
        GetCurrentDirectoryA(sizeof(cwd), cwd);
        CHECK(n == 17 && nm[0] == cwd[0] && seq(nm + 1, ":\\bin\\w32min.exe"));
        ExitProcess((UINT)fails);
    }
    if (seq(mode, "fpu")) spin_fpu(g_ntok > 2 ? atoi_(g_tok[2]) : 0);
    if (seq(mode, "args")) {
        CHECK(g_ntok == 4 && seq(g_tok[1], "args") && seq(g_tok[2], "with space") && seq(g_tok[3], "last"));
        CHECK(GetEnvironmentVariableA("W32TEST", cwd, sizeof(cwd)) == 1);
        ExitProcess((UINT)fails);
    }
    if (seq(mode, "fault-null")) { zero = *(volatile int *)0; }
    if (seq(mode, "fault-write-ro")) { *(volatile char *)ro_data = 1; }
    if (seq(mode, "fault-write-text")) { *(volatile char *)(void *)entry = 0; }
    if (seq(mode, "fault-exec-data")) { ((void (*)(void))&zero)(); }
    if (seq(mode, "fault-exec-heap")) {
        char *h = HeapAlloc(GetProcessHeap(), 0, 64); h[0] = (char)0xC3; ((void (*)(void))h)();
    }
    if (seq(mode, "fault-ud")) { __asm__ volatile("ud2"); }
    if (seq(mode, "fault-div")) { zero = 100 / zero; }
    if (seq(mode, "fault-priv")) { __asm__ volatile("cli"); }
    if (seq(mode, "fault-hlt")) { __asm__ volatile("hlt"); }
    if (seq(mode, "fault-stack")) { zero = recurse(0); }
    if (seq(mode, "fault-kernel")) { zero = *(volatile int *)0xFFFF800000000000ull; }
    if (seq(mode, "fault-reserved")) {
        char *p = VirtualAlloc(NULL, 65536, MEM_RESERVE, PAGE_READWRITE); p[0] = 1;
    }
    if (seq(mode, "fault-noaccess")) {
        char *p = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS); zero = p[0];
    }
    if (seq(mode, "fault-readonly")) {
        char *p = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY); p[0] = 1;
    }
    if (seq(mode, "fault-decommitted")) {
        char *p = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); p[0] = 1;
        VirtualFree(p, 65536, MEM_DECOMMIT); p[0] = 2;
    }
    if (seq(mode, "fault-released")) {
        char *p = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); p[0] = 1;
        VirtualFree(p, 0, MEM_RELEASE); p[0] = 2;
    }
    if (seq(mode, "fault-syscall")) {
        DWORD w; BOOL ok = WriteFile(out, (void *)0x10, 8, &w, NULL);
        ExitProcess(ok ? 99 : 0);
    }
    ExitProcess(0x7FFF);
}
