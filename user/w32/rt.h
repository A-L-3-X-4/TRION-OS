#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../kernel/include/w32abi.h"
#include "../../kernel/include/abi.h"

#define WINAPI __attribute__((ms_abi))
typedef uint32_t DWORD;
typedef int32_t  BOOL;
typedef uint16_t WCHAR, WORD;
typedef uint8_t  BYTE;
typedef void    *HANDLE;
typedef int64_t  i64;
typedef uint64_t u64;
#define TRUE  1
#define FALSE 0

static inline long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}
#define sc1(n, a)          sc6(n, (long)(a), 0, 0, 0, 0, 0)
#define sc2(n, a, b)       sc6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define sc3(n, a, b, c)    sc6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define sc4(n, a, b, c, d) sc6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

#define W32FN(dll, name, ret, params)                                                                       \
    static ret WINAPI w_##name params;                                                                       \
    static const struct w32_export xf_##name __attribute__((used, section("w32exp"))) =                      \
        { (u64)dll, (u64)#name, (u64)&w_##name, W32X_FUNC };                                                 \
    static ret WINAPI w_##name params
#define W32DATA(dll, name, var)                                                                             \
    static const struct w32_export xd_##name __attribute__((used, section("w32exp"))) =                      \
        { (u64)dll, (u64)#name, (u64)&(var), W32X_DATA }
#define W32ALIAS(dll, name, fn)                                                                             \
    static const struct w32_export xa_##name __attribute__((used, section("w32exp"))) =                      \
        { (u64)dll, (u64)#name, (u64)&(fn), W32X_FUNC }

extern struct w32_startup *g_su;
static inline uint8_t *teb(void) { u64 v; __asm__("mov %%gs:0x30, %0" : "=r"(v)); return (uint8_t *)v; }
static inline uint8_t *peb(void) { return (uint8_t *)*(u64 *)(teb() + TEB_PEB); }

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_DATA 13
#define ERROR_NOT_SAME_DEVICE 17
#define ERROR_WRITE_PROTECT 19
#define ERROR_GEN_FAILURE 31
#define ERROR_SHARING_VIOLATION 32
#define ERROR_NOT_SUPPORTED 50
#define ERROR_FILE_EXISTS 80
#define ERROR_INVALID_PARAMETER 87
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define ERROR_DISK_FULL 112
#define ERROR_INVALID_NAME 123
#define ERROR_MOD_NOT_FOUND 126
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_DIR_NOT_EMPTY 145
#define ERROR_ALREADY_EXISTS 183
#define ERROR_FILENAME_EXCED_RANGE 206
#define ERROR_NO_MORE_FILES 18
#define ERROR_IO_DEVICE 1117
#define ERROR_NOACCESS 998
#define ERROR_INVALID_ADDRESS 487
#define ERROR_DIRECTORY 267
#define ERROR_NEGATIVE_SEEK 131
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(i64)-1)

static inline void set_last_error(DWORD e) { *(DWORD *)(teb() + TEB_LASTERROR) = e; }
static inline DWORD get_last_error(void) { return *(DWORD *)(teb() + TEB_LASTERROR); }
DWORD kerr_to_win(long neg_errno);
static inline BOOL fail_k(long neg_errno) { set_last_error(kerr_to_win(neg_errno)); return FALSE; }
static inline BOOL fail_w(DWORD e) { set_last_error(e); return FALSE; }

static inline HANDLE kh_to_w(uint32_t k) { return (HANDLE)((u64)k << 2); }
static inline uint32_t w_to_kh(HANDLE h)
{
    u64 v = (u64)h;
    return (v & 3) || (v >> 34) || !v ? 0 : (uint32_t)(v >> 2);
}
#define HCON(i)  ((HANDLE)(0xC000000000000000ULL + 4ULL * (i)))
#define HFIND(i) ((HANDLE)(0xA000000000000000ULL + 4ULL * (i)))
static inline int con_index(HANDLE h)
{
    u64 v = (u64)h - 0xC000000000000000ULL;
    return (v & 3) || v > 12 || v < 4 ? 0 : (int)(v / 4);
}

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t rt_strlen(const char *s);
void *rt_heap_alloc(void *heap, DWORD flags, size_t n);
void  rt_heap_free(void *p);
void *rt_heap_realloc(void *heap, DWORD flags, void *p, size_t n);
size_t rt_heap_size(void *p);
void *rt_default_heap(void);
void *rt_malloc(size_t n);
void  rt_free(void *p);
long  rt_vm_alloc(u64 addr, u64 size, DWORD flags);

size_t rt_w2u(const WCHAR *w, long n, char *out, size_t cap);
size_t rt_u2w(const char *u, long n, WCHAR *out, size_t cap);
void   rt_puts(const char *s);
void   rt_log(const char *s);

#define RT_PATH 300
int  path_to_nt(const char *win, char *out);
int  path_cwd(char *out, size_t cap);
int  path_full(const char *win, char *out, size_t cap);

__attribute__((noreturn)) void rt_exit(int code);
void rt_crt_init(void);
void rt_run_atexit(void);
const char *rt_env_get(const char *name);
void rt_ctype_init(void);

struct rt_file { char *_ptr; int _cnt; char *_base; int _flag; int _file; int _charbuf; int _bufsiz; char *_tmpfname; };
int rt_vformat(char *buf, size_t cap, const char *fmt, __builtin_ms_va_list *ap);
