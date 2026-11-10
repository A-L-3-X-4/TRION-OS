#pragma once
#include <stdint.h>

#define W32_RT_BASE       0x8000010000ULL
#define W32_TEB_BASE      0x8000100000ULL
#define W32_TEB_PAGES     2
#define W32_PEB_BASE      0x8000102000ULL
#define W32_START_BASE    0x8000103000ULL
#define W32_START_PAGES   9
#define W32_STUB_BASE     0x8000200000ULL
#define W32_IMAGE_BASE    0x8000400000ULL
#define W32_STRINGS_MAX   (32 * 1024)
#define W32_MAX_TLS_CB    32
#define W32_STUB_PAGES    2
#define W32_MAX_MISSING   128

#define TEB_STACKBASE     0x08
#define TEB_STACKLIMIT    0x10
#define TEB_SELF          0x30
#define TEB_PID           0x40
#define TEB_TID           0x48
#define TEB_TLSPTR        0x58
#define TEB_PEB           0x60
#define TEB_LASTERROR     0x68
#define TEB_TLSSLOTS      0x1480
#define TEB_TLSARRAY      0x1700
#define PEB_BEINGDEBUGGED 0x02
#define PEB_IMAGEBASE     0x10
#define PEB_PROCESSPARAMS 0x20
#define PEB_PROCESSHEAP   0x30
#define PEB_NCPU          0xB8
#define PEB_OSMAJOR       0x118
#define PEB_OSMINOR       0x11C
#define PEB_OSBUILD       0x120
#define PEB_OSPLATFORM    0x124

#define W32_START_MAGIC   0x57333253u
#define W32_START_VERSION 1u

struct w32_startup {
    uint32_t magic, version;
    uint64_t image_base, image_size, entry;
    uint64_t peb, teb;
    uint64_t stack_top, stack_low;
    uint64_t tls_block;
    uint32_t subsystem, ntls;
    uint64_t tls_callbacks[W32_MAX_TLS_CB + 1];
    uint32_t argc, envc;
    uint32_t args_off, args_len;
    uint32_t env_off, env_len;
    uint32_t path_off;
    uint32_t cwd_off;
    uint32_t nmissing;
    uint32_t pad;
};

#define W32X_FUNC 0
#define W32X_DATA 1
struct w32_export { uint64_t dll, name, addr, kind; };

#define W32_RT_DLL "infinityos"
