#include "lx.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { puts_("lxmin: FAIL line "); putn(__LINE__); puts_(": " #c "\n"); g_fail = (__LINE__ & 0xFF) ? (__LINE__ & 0xFF) : 1; return g_fail; } } while (0)
#define CHECKEQ(x, y) do { i64 _x = (x), _y = (y); if (_x != _y) { puts_("lxmin: FAIL line "); putn(__LINE__); puts_(": " #x " == "); putn(_x); puts_(" want "); putn(_y); puts_("\n"); g_fail = (__LINE__ & 0xFF) ? (__LINE__ & 0xFF) : 1; return g_fail; } } while (0)

extern char _start[];
static const char *g_base;

static char pathbuf[256];
static const char *P(const char *rel)
{
    u64 n = slen(g_base);
    mcpy(pathbuf, g_base, n);
    pathbuf[n++] = '/';
    u64 m = slen(rel);
    mcpy(pathbuf + n, rel, m + 1);
    return pathbuf;
}

static i64 sys_stat_(const char *p, struct kstat *st) { return sc2(SYS_stat, p, st); }

static int test_startup(u64 *sp)
{
    i64 argc = (i64)sp[0];
    char **argv = (char **)(sp + 1);
    CHECKEQ(argc >= 1, 1);
    char **envp = argv + argc + 1;
    CHECK(argv[argc] == NULL);
    int envc = 0;
    int have_env = 0;
    while (envp[envc]) {
        if (!scmp(envp[envc], "LXTEST=1"))
            have_env = 1;
        envc++;
    }
    (void)have_env;
    u64 *aux = (u64 *)(envp + envc + 1);
    u64 pagesz = 0, phdr = 0, entry = 0, random = 0, phnum = 0, phent = 0, uid = 99, base = 99, execfn = 0, plat = 0;
    for (; aux[0]; aux += 2) {
        switch (aux[0]) {
        case 6: pagesz = aux[1]; break;
        case 3: phdr = aux[1]; break;
        case 9: entry = aux[1]; break;
        case 25: random = aux[1]; break;
        case 5: phnum = aux[1]; break;
        case 4: phent = aux[1]; break;
        case 11: uid = aux[1]; break;
        case 7: base = aux[1]; break;
        case 31: execfn = aux[1]; break;
        case 15: plat = aux[1]; break;
        }
    }
    CHECKEQ(pagesz, 4096);
    CHECKEQ(phent, 56);
    CHECK(phnum >= 1 && phnum < 64);
    CHECK(entry == (u64)_start);
    CHECKEQ(base, 0);
    CHECK(phdr != 0);
    CHECK(*(u32 *)(phdr - 64) == 0x464C457F);
    int nz = 0;
    for (int i = 0; i < 16; i++)
        nz |= ((u8 *)random)[i];
    CHECK(nz != 0);
    CHECK(execfn && !scmp((char *)execfn, argv[0]));
    CHECK(plat && !scmp((char *)plat, "x86_64"));
    CHECK(((u64)sp & 15) == 0);
    (void)uid;
    return 0;
}

static int test_basic(void)
{
    CHECKEQ(sys_write(1, "lxmin: hello from a Linux binary\n", 33), 33);
    CHECKEQ(sys_write(1, (void *)0x10, 4), -EFAULT);
    CHECKEQ(sys_write(1, (void *)0x7FFFFFFFFFF0UL, 4096), -EFAULT);
    CHECKEQ(sys_write(99, "x", 1), -EBADF);
    CHECKEQ(sys_write(-1, "x", 1), -EBADF);
    CHECKEQ(sys_read(77, (void *)0, 0), -EBADF);
    i64 pid = sc0(SYS_getpid);
    CHECK(pid > 0);
    CHECKEQ(sc0(SYS_gettid), pid);
    char un[6][65];
    CHECKEQ(sc1(SYS_uname, un), 0);
    CHECK(!scmp(un[0], "Linux") && !scmp(un[4], "x86_64"));
    CHECKEQ(sc0(9999), -38);
    CHECKEQ(sys_close(55), -EBADF);

    struct timespec a, b;
    CHECKEQ(sc2(SYS_clock_gettime, 1, &a), 0);
    struct timespec ts = { 0, 15 * 1000000 };
    CHECKEQ(sc2(SYS_nanosleep, &ts, 0), 0);
    CHECKEQ(sc2(SYS_clock_gettime, 1, &b), 0);
    i64 dns = (b.sec - a.sec) * 1000000000 + (b.nsec - a.nsec);
    CHECK(dns >= 14 * 1000000 && dns < 2000 * 1000000L);
    CHECKEQ(sc2(SYS_clock_gettime, 0, &a), 0);
    CHECK(a.sec > 1700000000);
    CHECKEQ(sc2(SYS_clock_gettime, 77, &a), -EINVAL);

    u8 r1[32], r2[32];
    CHECKEQ(sc3(SYS_getrandom, r1, 32, 0), 32);
    CHECKEQ(sc3(SYS_getrandom, r2, 32, 0), 32);
    CHECK(mcmp(r1, r2, 32) != 0);
    return 0;
}

static int test_memory(void)
{
    u64 cur = (u64)sc1(SYS_brk, 0);
    CHECK(cur > 0);
    CHECKEQ(sc1(SYS_brk, cur - 4096) == (i64)cur, 1);
    u64 want = cur + 300 * 4096 + 100;
    CHECKEQ(sc1(SYS_brk, want), (i64)want);
    u8 *h = (u8 *)cur;
    for (u64 i = 0; i < 300 * 4096; i += 4096)
        h[i] = (u8)(i >> 12);
    for (u64 i = 0; i < 300 * 4096; i += 4096)
        CHECKEQ(h[i], (u8)(i >> 12));
    CHECKEQ(h[1234], 0);
    CHECKEQ(sc1(SYS_brk, cur + 4096), (i64)(cur + 4096));
    CHECKEQ(sc1(SYS_brk, want), (i64)want);
    CHECKEQ(h[0], 0);
    CHECKEQ(h[200 * 4096], 0);
    CHECKEQ(sc1(SYS_brk, cur), (i64)cur);

    u64 len = 32UL << 20;
    u8 *m = (u8 *)sc6(SYS_mmap, 0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK((i64)m > 0 && ((u64)m & 0xFFF) == 0);
    CHECKEQ(m[0], 0);
    CHECKEQ(m[len - 1], 0);
    m[len / 2] = 0x5A;
    CHECKEQ(m[len / 2], 0x5A);
    CHECKEQ(sc2(SYS_munmap, m + (len / 4), len / 4), 0);
    m[0] = 1;
    m[len - 1] = 2;
    CHECKEQ(m[len / 2], 0x5A);
    CHECKEQ(sc3(SYS_madvise, m + len / 2, 4096, 4), 0);
    CHECKEQ(m[len / 2], 0);
    CHECKEQ(sc2(SYS_munmap, m, len), 0);

    CHECKEQ(sc6(SYS_mmap, 0, 0, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0), -EINVAL);
    CHECKEQ(sc6(SYS_mmap, 0, 4096, PROT_READ, MAP_ANON, -1, 0), -EINVAL);
    CHECKEQ(sc6(SYS_mmap, 0, 1UL << 47, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0), -12);
    CHECKEQ(sc2(SYS_munmap, 0x1234, 4096), -EINVAL);

    u8 *p = (u8 *)sc6(SYS_mmap, 0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK((i64)p > 0);
    p[4096] = 7;
    CHECKEQ(sc3(SYS_mprotect, p + 4096, 4096, PROT_READ), 0);
    CHECKEQ(p[4096], 7);
    int fd = (int)sys_open(P("IN.TXT"), O_RDONLY, 0);
    CHECK(fd >= 0);
    CHECKEQ(sys_read(fd, p + 4096, 8), -EFAULT);
    CHECKEQ(sc3(SYS_mprotect, p + 4096, 4096, PROT_NONE), 0);
    CHECKEQ(sys_read(fd, p + 4096, 8), -EFAULT);
    CHECKEQ(sys_write(1, p + 4096, 1), -EFAULT);
    CHECKEQ(sc3(SYS_mprotect, p + 4096, 4096, PROT_READ | PROT_WRITE), 0);
    CHECKEQ(sys_read(fd, p + 4096, 5), 5);
    CHECKEQ(mcmp(p + 4096, "hello", 5), 0);
    CHECKEQ(sc3(SYS_mprotect, p + 100, 4096, PROT_READ), -EINVAL);
    CHECKEQ(sc2(SYS_munmap, p, 3 * 4096), 0);
    CHECKEQ(sc3(SYS_mprotect, p, 3 * 4096, PROT_READ), -12);

    u8 *fm = (u8 *)sc6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK((i64)fm > 0);
    CHECKEQ(mcmp(fm, "hello from the kernel\n", 22), 0);
    CHECKEQ(fm[22], 0);
    fm[0] = 'J';
    CHECKEQ(sc2(SYS_munmap, fm, 4096), 0);
    char chk[8];
    CHECKEQ(sc4(SYS_pread64, fd, chk, 5, 0), 5);
    CHECKEQ(mcmp(chk, "hello", 5), 0);
    CHECKEQ(sc6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, 1 , fd, 0), -13);
    CHECKEQ(sc6(SYS_mmap, 0, 4096, PROT_READ, MAP_PRIVATE, 60, 0), -EBADF);
    sys_close(fd);

    u8 *a = (u8 *)sc6(SYS_mmap, 0, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK((i64)a > 0);
    a[4096] = 9;
    u8 *b = (u8 *)sc6(SYS_mmap, (i64)(a + 4096), 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    CHECK(b == a + 4096);
    CHECKEQ(b[0], 0);
    CHECKEQ(sc2(SYS_munmap, a, 4 * 4096), 0);

    u8 *first = 0;
    for (int i = 0; i < 400; i++) {
        u8 *q = (u8 *)sc6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK((i64)q > 0);
        *q = (u8)i;
        if (!first) first = q;
    }
    CHECKEQ(*first, 0);
    return 0;
}

static u64 tls_block[8];
static volatile double sink;
static int test_tls_sse(void)
{
    tls_block[0] = (u64)&tls_block[0];
    tls_block[1] = 0xC0FFEE;
    CHECKEQ(sc2(SYS_arch_prctl, 0x1002, tls_block), 0);
    u64 v, w;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(v));
    __asm__ volatile("mov %%fs:8, %0" : "=r"(w));
    CHECK(v == (u64)&tls_block[0]);
    CHECKEQ(w, 0xC0FFEE);
    u64 got = 0;
    CHECKEQ(sc2(SYS_arch_prctl, 0x1003, &got), 0);
    CHECK(got == (u64)tls_block);

    double x = 1.0, y = 0.5;
    for (int i = 0; i < 1000; i++) {
        x = x * 1.0001 + y;
        y = y * 0.999 + 0.001;
        if ((i & 63) == 0)
            sc0(SYS_sched_yield);
    }
    sink = x + y;
    CHECK(x > 500.0 && x < 2000.0);
    float f = 3.0f;
    f = f * f + 0.25f;
    CHECK(f == 9.25f);
    return 0;
}

static int test_files(void)
{
    struct kstat st;
    CHECKEQ(sys_stat_(P("nope.txt"), &st), -ENOENT);
    CHECKEQ(sys_open(P("nope.txt"), O_RDONLY, 0), -ENOENT);

    int fd = (int)sys_open(P("IN.TXT"), O_RDONLY, 0);
    CHECK(fd >= 3);
    char buf[64];
    CHECKEQ(sys_read(fd, buf, 5), 5);
    CHECKEQ(mcmp(buf, "hello", 5), 0);
    int fd2 = (int)sc1(SYS_dup, fd);
    CHECK(fd2 > fd);
    CHECKEQ(sys_read(fd2, buf, 6), 6);
    CHECKEQ(mcmp(buf, " from ", 6), 0);
    CHECKEQ(sys_read(fd, buf, 3), 3);
    CHECKEQ(mcmp(buf, "the", 3), 0);
    CHECKEQ(sys_write(fd, "x", 1), -EBADF);
    CHECKEQ(sc3(SYS_lseek, fd, 0, 2), 22);
    CHECKEQ(sys_read(fd, buf, 10), 0);
    CHECKEQ(sc3(SYS_lseek, fd, 6, 0), 6);
    CHECKEQ(sys_read(fd2, buf, 4), 4);
    CHECKEQ(mcmp(buf, "from", 4), 0);
    CHECKEQ(sc3(SYS_lseek, 1, 0, 0), -29);
    CHECKEQ(sc3(SYS_fcntl, fd, 1, 0), 0);
    CHECKEQ(sc3(SYS_fcntl, fd, 2, 1), 0);
    CHECKEQ(sc3(SYS_fcntl, fd, 1, 0), 1);
    CHECKEQ(sc3(SYS_ioctl, fd, 0x5401, buf), -ENOTTY);
    { i64 r = sc3(SYS_ioctl, 1, 0x5401, buf); CHECK(r == 0 || r == -ENOTTY); }
    CHECKEQ(sc2(SYS_dup2, fd, 40), 40);
    CHECKEQ(sys_close(fd), 0);
    CHECKEQ(sys_read(fd, buf, 1), -EBADF);
    CHECKEQ(sys_read(40, buf, 1), 1);
    sys_close(fd2);
    sys_close(40);

    CHECKEQ(sc2(SYS_fstat, 1, &st), 0);
    CHECK((st.mode & 0170000) == 020000 || (st.mode & 0170000) == 010000);

    fd = (int)sys_open(P("t1.txt"), O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 3);
    CHECKEQ(sys_open(P("t1.txt"), O_WRONLY | O_CREAT | O_EXCL, 0644), -EEXIST);
    CHECKEQ(sys_write(fd, "alpha-", 6), 6);
    struct { u64 base, len; } iov[2] = { { (u64)"beta-", 5 }, { (u64)"gamma", 5 } };
    CHECKEQ(sc3(SYS_writev, fd, iov, 2), 10);
    CHECKEQ(sys_read(fd, buf, 1), -EBADF);
    CHECKEQ(sc2(SYS_fstat, fd, &st), 0);
    CHECKEQ(st.size, 16);
    CHECKEQ(st.mode & 0170000, 0100000);
    CHECK(st.ino != 0);
    CHECKEQ(sys_close(fd), 0);

    fd = (int)sys_open(P("t1.txt"), O_RDWR, 0);
    CHECK(fd >= 3);
    CHECKEQ(sys_read(fd, buf, 64), 16);
    CHECKEQ(mcmp(buf, "alpha-beta-gamma", 16), 0);
    CHECKEQ(sc4(SYS_pwrite64, fd, "ALPHA", 5, 0), 5);
    CHECKEQ(sc4(SYS_pread64, fd, buf, 16, 0), 16);
    CHECKEQ(mcmp(buf, "ALPHA-beta-gamma", 16), 0);
    CHECKEQ(sc2(SYS_ftruncate, fd, 5), 0);
    CHECKEQ(sc2(SYS_fstat, fd, &st), 0);
    CHECKEQ(st.size, 5);
    CHECKEQ(sc2(SYS_ftruncate, fd, 8192), 0);
    CHECKEQ(sc4(SYS_pread64, fd, buf, 4, 5000), 4);
    CHECKEQ(buf[0] | buf[1] | buf[2] | buf[3], 0);
    sys_close(fd);

    fd = (int)sys_open(P("t1.txt"), O_WRONLY | O_TRUNC, 0);
    CHECK(fd >= 3);
    CHECKEQ(sc2(SYS_fstat, fd, &st), 0);
    CHECKEQ(st.size, 0);
    sys_close(fd);
    fd = (int)sys_open(P("t1.txt"), O_WRONLY | O_APPEND, 0);
    CHECKEQ(sys_write(fd, "one", 3), 3);
    CHECKEQ(sc3(SYS_lseek, fd, 0, 0), 0);
    CHECKEQ(sys_write(fd, "two", 3), 3);
    sys_close(fd);
    CHECKEQ(sys_stat_(P("t1.txt"), &st), 0);
    CHECKEQ(st.size, 6);

    CHECKEQ(sc2(SYS_mkdir, P("sub"), 0755), 0);
    CHECKEQ(sc2(SYS_mkdir, P("sub"), 0755), -EEXIST);
    CHECKEQ(sys_stat_(P("sub"), &st), 0);
    CHECKEQ(st.mode & 0170000, 040000);
    return 0;
}

static int test_files2(void)
{
    struct kstat st;
    char buf[512];
    char p1[256], p2[256];
    u64 n = slen(g_base);
    mcpy(p1, g_base, n); mcpy(p1 + n, "/t1.txt", 8);
    mcpy(p2, g_base, n); mcpy(p2 + n, "/sub/t2.txt", 12);
    CHECKEQ(sc2(SYS_rename, p1, p2), 0);
    CHECKEQ(sys_stat_(p1, &st), -ENOENT);
    CHECKEQ(sys_stat_(p2, &st), 0);
    CHECKEQ(st.size, 6);
    char p3[256];
    mcpy(p3, g_base, n); mcpy(p3 + n, "/sub", 5);
    CHECKEQ(sc1(SYS_rmdir, p3), -ENOTEMPTY);
    CHECKEQ(sc1(SYS_unlink, p3), -EISDIR);
    CHECKEQ(sc1(SYS_rmdir, p2), -ENOTDIR);

    int dfd = (int)sys_open(p3, O_RDONLY | O_DIRECTORY, 0);
    CHECK(dfd >= 3);
    CHECKEQ(sys_read(dfd, buf, 8), -EISDIR);
    i64 got = sc3(SYS_getdents64, dfd, buf, sizeof(buf));
    CHECK(got > 0);
    int seen_t2 = 0;
    for (i64 off = 0; off < got;) {
        u16 reclen = *(u16 *)(buf + off + 16);
        const char *name = buf + off + 19;
        CHECK(*(u64 *)(buf + off) != 0);
        if (!scmp(name, "t2.txt"))
            seen_t2 = 1;
        off += reclen;
    }
    CHECK(seen_t2);
    CHECKEQ(sc3(SYS_getdents64, dfd, buf, sizeof(buf)), 0);
    sys_close(dfd);
    CHECKEQ(sys_open(p2, O_RDONLY | O_DIRECTORY, 0), -ENOTDIR);

    CHECKEQ(sc1(SYS_chdir, g_base), 0);
    char cwd[128];
    i64 cl = sc2(SYS_getcwd, cwd, sizeof(cwd));
    CHECK(cl > 0 && !scmp(cwd, g_base));
    CHECKEQ(sc2(SYS_getcwd, cwd, 3), -34);
    int fd = (int)sys_open("sub/../IN.TXT", O_RDONLY, 0);
    CHECK(fd >= 3);
    sys_close(fd);
    fd = (int)sys_open("./sub//./t2.txt", O_RDONLY, 0);
    CHECK(fd >= 3);
    CHECKEQ(sys_read(fd, buf, 16), 6);
    CHECKEQ(mcmp(buf, "onetwo", 6), 0);
    sys_close(fd);
    CHECKEQ(sc1(SYS_chdir, "sub"), 0);
    fd = (int)sys_open("t2.txt", O_RDONLY, 0);
    CHECK(fd >= 3);
    sys_close(fd);
    CHECKEQ(sc1(SYS_chdir, "t2.txt"), -ENOTDIR);
    CHECKEQ(sc1(SYS_chdir, "nowhere"), -ENOENT);
    CHECKEQ(sc1(SYS_chdir, "/"), 0);

    CHECKEQ(sc1(SYS_unlink, p2), 0);
    CHECKEQ(sc1(SYS_rmdir, p3), 0);
    CHECKEQ(sys_stat_(p3, &st), -ENOENT);
    CHECKEQ(sc1(SYS_unlink, p3), -ENOENT);

    u8 *big = (u8 *)sc6(SYS_mmap, 0, 300 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK((i64)big > 0);
    for (u64 i = 0; i < 300 * 1024; i++)
        big[i] = (u8)(i * 7 + (i >> 8));
    int bf = (int)sys_open(P("big.bin"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(bf >= 3);
    CHECKEQ(sys_write(bf, big, 300 * 1024), 300 * 1024);
    mset(big, 0, 300 * 1024);
    CHECKEQ(sc3(SYS_lseek, bf, 0, 0), 0);
    CHECKEQ(sys_read(bf, big, 300 * 1024), 300 * 1024);
    for (u64 i = 0; i < 300 * 1024; i++)
        CHECKEQ(big[i], (u8)(i * 7 + (i >> 8)));
    sys_close(bf);
    CHECKEQ(sc1(SYS_unlink, P("big.bin")), 0);

    int of = (int)sys_open(P("OUT.TXT"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(of >= 3);
    CHECKEQ(sys_write(of, "written by a Linux binary\n", 26), 26);
    sys_close(of);
    return 0;
}

static int recurse(int d)
{
    volatile char pad[1024];
    pad[0] = (char)d;
    if (d == 0)
        return 0;
    return recurse(d - 1) + (int)(pad[0] - (char)d);
}

int lxmain(u64 *sp)
{
    i64 argc = (i64)sp[0];
    char **argv = (char **)(sp + 1);
    const char *mode = argc > 1 ? argv[1] : "main";
    g_base = argc > 2 ? argv[2] : "/Volumes/lx";

    if (!scmp(mode, "fault_null"))   { *(volatile int *)0 = 1; return 0; }
    if (!scmp(mode, "fault_ro")) {
        u8 *p = (u8 *)sc6(SYS_mmap, 0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
        p[0] = 1;
        return 0;
    }
    if (!scmp(mode, "fault_none")) {
        u8 *p = (u8 *)sc6(SYS_mmap, 0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        return *(volatile u8 *)p;
    }
    if (!scmp(mode, "fault_nx")) {
        u8 *p = (u8 *)sc6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        p[0] = 0xC3;
        ((void (*)(void))p)();
        return 0;
    }
    if (!scmp(mode, "fault_unmapped")) {
        u8 *p = (u8 *)sc6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        sc2(SYS_munmap, p, 4096);
        return *(volatile u8 *)p;
    }
    if (!scmp(mode, "fault_kernel")) return *(volatile u8 *)0x100000;
    if (!scmp(mode, "fault_priv"))   { __asm__ volatile("cli"); return 0; }
    if (!scmp(mode, "stack_ok"))     return recurse(3000) == 0 ? 0 : 1;
    if (!scmp(mode, "stack_overflow")) return recurse(100000);
    if (!scmp(mode, "fixed_low")) {
        i64 r = sc6(SYS_mmap, 0x1000, 4096, PROT_READ, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        i64 r2 = sc6(SYS_mmap, 0x200000, 4096, PROT_READ, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        return (r == -EINVAL && r2 == -EINVAL) ? 0 : 1;
    }
    if (!scmp(mode, "tty")) {
        char b[64];
        return (sc3(SYS_ioctl, 1, 0x5401, b) == 0 && sc3(SYS_ioctl, 1, 0x5413, b) == 0) ? 0 : 1;
    }
    if (!scmp(mode, "exit42"))       return 42;
    if (!scmp(mode, "spin_fpu")) {
        double v = (double)(argv[2][0] - '0') + 0.5;
        double acc = 0;
        for (int i = 0; i < 20000; i++) {
            acc += v * (double)(i & 7);
            v = v * 1.0000001 + 0.25;
            if ((i % 500) == 0)
                sc0(SYS_sched_yield);
        }
        double v2 = (double)(argv[2][0] - '0') + 0.5, acc2 = 0;
        for (int i = 0; i < 20000; i++) {
            acc2 += v2 * (double)(i & 7);
            v2 = v2 * 1.0000001 + 0.25;
        }
        return (acc == acc2 && v == v2) ? 0 : 1;
    }

    int rc;
    if ((rc = test_startup(sp))) return rc;
    if ((rc = test_basic())) return rc;
    if ((rc = test_memory())) return rc;
    if ((rc = test_tls_sse())) return rc;
    if ((rc = test_files())) return rc;
    if ((rc = test_files2())) return rc;
    puts_("lxmin: all checks passed\n");
    return 0;
}

__asm__(".text\n.globl _start\n_start:\n"
        "  xor %ebp, %ebp\n"
        "  mov %rsp, %rdi\n"
        "  and $-16, %rsp\n"
        "  call lxmain\n"
        "  mov %eax, %edi\n"
        "  mov $231, %eax\n"
        "  syscall\n"
        "  hlt\n");
