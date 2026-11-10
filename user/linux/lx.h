#pragma once
typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef unsigned char u8;
typedef unsigned short u16;

#define NULL ((void *)0)

static inline i64 sc6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
    register i64 r10 __asm__("r10") = d;
    register i64 r8 __asm__("r8") = e;
    register i64 r9 __asm__("r9") = f;
    i64 ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}
#define sc0(n)              sc6(n, 0, 0, 0, 0, 0, 0)
#define sc1(n, a)           sc6(n, (i64)(a), 0, 0, 0, 0, 0)
#define sc2(n, a, b)        sc6(n, (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sc3(n, a, b, c)     sc6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sc4(n, a, b, c, d)  sc6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), 0, 0)

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_stat 4
#define SYS_fstat 5
#define SYS_lseek 8
#define SYS_mmap 9
#define SYS_mprotect 10
#define SYS_munmap 11
#define SYS_brk 12
#define SYS_ioctl 16
#define SYS_pread64 17
#define SYS_pwrite64 18
#define SYS_readv 19
#define SYS_writev 20
#define SYS_sched_yield 24
#define SYS_madvise 28
#define SYS_dup 32
#define SYS_dup2 33
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_exit 60
#define SYS_uname 63
#define SYS_fcntl 72
#define SYS_ftruncate 77
#define SYS_getcwd 79
#define SYS_chdir 80
#define SYS_rename 82
#define SYS_mkdir 83
#define SYS_rmdir 84
#define SYS_unlink 87
#define SYS_arch_prctl 158
#define SYS_gettid 186
#define SYS_getdents64 217
#define SYS_clock_gettime 228
#define SYS_exit_group 231
#define SYS_getrandom 318

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_DIRECTORY 0200000

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANON 0x20

#define EPERM 1
#define ENOENT 2
#define EBADF 9
#define EFAULT 14
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOTTY 25
#define ENOTEMPTY 39

struct timespec { i64 sec, nsec; };
struct kstat {
    u64 dev, ino, nlink; u32 mode, uid, gid, pad; u64 rdev; i64 size, blksize, blocks;
    u64 at, atn, mt, mtn, ct, ctn; i64 unused[3];
};

static inline i64 sys_write(int fd, const void *b, u64 n) { return sc3(SYS_write, fd, b, n); }
static inline i64 sys_read(int fd, void *b, u64 n) { return sc3(SYS_read, fd, b, n); }
static inline i64 sys_open(const char *p, int fl, int mode) { return sc3(SYS_open, p, fl, mode); }
static inline i64 sys_close(int fd) { return sc1(SYS_close, fd); }
static inline __attribute__((noreturn)) void sys_exit(int c) { for (;;) sc1(SYS_exit_group, c); }

static inline u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static inline int scmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (u8)*a - (u8)*b; }
static inline void *mcpy(void *d, const void *s, u64 n) { u8 *x = d; const u8 *y = s; while (n--) *x++ = *y++; return d; }
static inline void *mset(void *d, int c, u64 n) { u8 *x = d; while (n--) *x++ = (u8)c; return d; }
static inline int mcmp(const void *a, const void *b, u64 n) { const u8 *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; x++, y++; } return 0; }

static inline void puts_(const char *s) { sys_write(1, s, slen(s)); }
static inline void putn(i64 v)
{
    char b[24]; int i = 24; u64 u = v < 0 ? -(u64)v : (u64)v;
    do { b[--i] = '0' + u % 10; u /= 10; } while (u);
    if (v < 0) b[--i] = '-';
    sys_write(1, b + i, 24 - i);
}
