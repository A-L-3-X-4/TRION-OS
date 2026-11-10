#include "kernel.h"
#include "io.h"
#include "mm.h"
#include "task.h"
#include "timer.h"
#include "obj.h"
#include "ns.h"
#include "vfs.h"
#include "linux.h"
#include "win32.h"

// The Win32 runtime is an ELF blob baked into the kernel, it plays kernel32, ntdll and msvcrt for every exe we load, because we don't have real DLLs and Microsoft isn't about to give us theirs
extern const uint8_t w32rt_start[], w32rt_end[];

// ELF64 header layouts so the kernel can dig through its own embedded runtime
struct e64_eh { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } __attribute__((packed));
struct e64_ph { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } __attribute__((packed));
struct e64_sh { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t align, entsize; } __attribute__((packed));
struct e64_sym { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } __attribute__((packed));

// Turns an address inside the runtime into a pointer to its bytes in the blob by walking the PT_LOAD segments, NULL if the file doesn't back it, so .bss is a no
static const uint8_t *rt_va(uint64_t va, uint64_t n)
{
    size_t size = (size_t)(w32rt_end - w32rt_start);
    struct e64_eh eh;
    memcpy(&eh, w32rt_start, sizeof(eh));
    for (unsigned i = 0; i < eh.phnum; i++) {
        struct e64_ph ph;
        memcpy(&ph, w32rt_start + eh.phoff + i * sizeof(ph), sizeof(ph));
        if (ph.type == 1 && va >= ph.vaddr && va - ph.vaddr < ph.filesz && n <= ph.filesz - (va - ph.vaddr) &&
            ph.offset + (va - ph.vaddr) + n <= size)
            return w32rt_start + ph.offset + (va - ph.vaddr);
    }
    return NULL;
}

// Finds a symbol by name with a dumb linear scan of every symbol, which is fine because it only gets called a few times, once
static bool rt_symbol(const char *want, uint64_t *value)
{
    size_t size = (size_t)(w32rt_end - w32rt_start);
    struct e64_eh eh;
    if (size < sizeof(eh))
        return false;
    memcpy(&eh, w32rt_start, sizeof(eh));
    for (unsigned i = 0; i < eh.shnum; i++) {
        struct e64_sh sh, str;
        memcpy(&sh, w32rt_start + eh.shoff + i * sizeof(sh), sizeof(sh));
        if (sh.type != 2  || sh.link >= eh.shnum)
            continue;
        memcpy(&str, w32rt_start + eh.shoff + sh.link * sizeof(str), sizeof(str));
        for (uint64_t k = 0; k + sizeof(struct e64_sym) <= sh.size; k += sizeof(struct e64_sym)) {
            struct e64_sym sy;
            memcpy(&sy, w32rt_start + sh.offset + k, sizeof(sy));
            const char *nm = (const char *)w32rt_start + str.offset + sy.name;
            if (sy.name < str.size && !strcmp(nm, want)) {
                *value = sy.value;
                return true;
            }
        }
    }
    return false;
}

// One resolved export: the fake DLL it pretends to live in, its name, where it is in the runtime, and what kind of thing it is
struct kexp { const char *dll, *name; uint64_t addr; uint32_t kind; };
static struct kexp *kx;
static unsigned nkx;
static bool kx_built, kx_ok;
static uint64_t rt_entry, rt_missing_va;

// Builds the export table the first time anybody needs it and finds the 'missing' hook that unimplemented imports get sent to, and if any of that isn't there the whole Win32 personality refuses to work
static void build_index(void)
{
    if (kx_built)
        return;
    kx_built = true;
    uint64_t a, b;
    if (!rt_symbol("__start_w32exp", &a) || !rt_symbol("__stop_w32exp", &b) || b < a ||
        (b - a) % sizeof(struct w32_export)) {
        klog(LOG_FAIL, "win32: runtime has no export table");
        return;
    }
    unsigned n = (unsigned)((b - a) / sizeof(struct w32_export));
    kx = kzalloc((n + 1) * sizeof(*kx));
    if (!kx)
        return;
    for (unsigned i = 0; i < n; i++) {
        struct w32_export e;
        const uint8_t *p = rt_va(a + i * sizeof(e), sizeof(e));
        if (!p)
            continue;
        memcpy(&e, p, sizeof(e));
        const uint8_t *d = rt_va(e.dll, 1), *nm = rt_va(e.name, 1);
        if (!d || !nm)
            continue;
        kx[nkx++] = (struct kexp){ (const char *)d, (const char *)nm, e.addr, (uint32_t)e.kind };
    }
    struct e64_eh eh;
    memcpy(&eh, w32rt_start, sizeof(eh));
    rt_entry = eh.entry;
    rt_missing_va = 0;
    for (unsigned i = 0; i < nkx; i++)
        if (!strcmp(kx[i].dll, W32_RT_DLL) && !strcmp(kx[i].name, "missing"))
            rt_missing_va = kx[i].addr;
    kx_ok = rt_missing_va != 0;
    if (!kx_ok)
        klog(LOG_FAIL, "win32: runtime lacks its 'missing' hook");
}

unsigned win32_export_count(void) { build_index(); return nkx; }

// Linear search by (dll, name), 0 if it doesn't exist, no hashing because the table is small and a hash map is more code than this is worth
uint64_t win32_export_addr(const char *dll, const char *name)
{
    build_index();
    for (unsigned i = 0; i < nkx; i++)
        if (!strcmp(kx[i].dll, dll) && !strcmp(kx[i].name, name))
            return kx[i].addr;
    return 0;
}

// Does s start with p, the startswith() that C won't give us
static bool has_prefix(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return false;
    return true;
}

// Decides which fake DLL an import really belongs to. kernel32, ntdll or msvcrt for the ones we implement (every api-ms-win-core alias and msvcr flavor back to crtdll included), "" for DLLs we know but implement nothing from so everything in them gets stubbed, and NULL for ones we've never heard of, which fails the load
static const char *dll_class(const char *in)
{
    char n[80];
    size_t k = 0;
    for (; in[k] && k < sizeof(n) - 1; k++)
        n[k] = (in[k] >= 'A' && in[k] <= 'Z') ? (char)(in[k] + 32) : in[k];
    n[k] = 0;
    if (k > 4 && !strcmp(n + k - 4, ".dll"))
        n[k - 4] = 0;
    if (!strcmp(n, "kernel32") || !strcmp(n, "kernelbase") || has_prefix(n, "api-ms-win-core-"))
        return "kernel32";
    if (!strcmp(n, "ntdll"))
        return "ntdll";
    if (!strcmp(n, "msvcrt") || !strcmp(n, "ucrtbase") || !strcmp(n, "crtdll") || has_prefix(n, "msvcr") ||
        has_prefix(n, "api-ms-win-crt-") || has_prefix(n, "vcruntime"))
        return "msvcrt";
    static const char *const known[] = {
        "user32", "gdi32", "advapi32", "shell32", "ole32", "oleaut32", "ws2_32", "wsock32", "comctl32", "comdlg32",
        "shlwapi", "version", "winmm", "imm32", "secur32", "bcrypt", "crypt32", "rpcrt4", "setupapi", "psapi",
        "iphlpapi", "userenv", "netapi32", "dbghelp", "msimg32", "winspool", "mswsock", "normaliz", "ncrypt", NULL };
    for (int i = 0; known[i]; i++)
        if (!strcmp(n, known[i]))
            return "";
    if (has_prefix(n, "api-ms-win-"))
        return "";
    return NULL;
}

// Each missing-import stub gets 64 bytes, 32 for the code and 32 for its own name (cut off at 31 chars), so 128 of them fit in two pages
#define STUB_SIZE 64
#define STUB_NAME 32

// Makes a stub for an import we don't implement, a few bytes of x86 that put a pointer to the function's own name in rcx and jump to the runtime's 'missing' hook, so calling it tells you which function you wanted and the exe still loads
static int make_stub(struct w32_load *ctx, const char *name, uint64_t *addr)
{
    struct task *t = ctx->t;
    if (ctx->nmissing >= W32_MAX_MISSING)
        return -E_NOMEM;
    if (ctx->nmissing == 0) {
        uint64_t end = W32_STUB_BASE + W32_STUB_PAGES * PAGE_SIZE;
        if (lx_vma_busy(t->lx, W32_STUB_BASE, end))
            return -E_NOEXEC;
        for (unsigned i = 0; i < W32_STUB_PAGES; i++)
            if (!task_map_zeroed(t->as, W32_STUB_BASE + i * PAGE_SIZE, VMM_USER))
                return -E_NOMEM;
        int rc = lx_vma_add(t->lx, W32_STUB_BASE, end, VM_PROT_R | VM_PROT_X, LXV_WIMAGE);
        if (rc)
            return rc;
        t->lx->vma[t->lx->nvma - 1].abase = W32_STUB_BASE;
    }
    uint8_t *page = (uint8_t *)vmm_as_lookup(t->as, W32_STUB_BASE + (ctx->nmissing / (PAGE_SIZE / STUB_SIZE)) * PAGE_SIZE, NULL);
    uint8_t *s = page + (size_t)(ctx->nmissing % (PAGE_SIZE / STUB_SIZE)) * STUB_SIZE;
    // lea rcx, [rip + name]; mov rax, <missing hook>; jmp rax, hand assembled into bytes because there's no assembler in the loop
    uint8_t code[19] = { 0x48, 0x8D, 0x0D, STUB_NAME - 7, 0, 0, 0,
                         0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
                         0xFF, 0xE0 };
    memcpy(code + 9, &ctx->rt_missing, 8);
    memcpy(s, code, sizeof(code));
    size_t i = 0;
    for (; name[i] && i < STUB_NAME - 1; i++)
        s[STUB_NAME + i] = (uint8_t)name[i];
    s[STUB_NAME + i] = 0;
    *addr = W32_STUB_BASE + (uint64_t)ctx->nmissing * STUB_SIZE;
    ctx->nmissing++;
    return 0;
}

// Resolves one import. Unknown DLL fails the load, a DLL we implement gets the real export if we have it, and everything else (missing function, ordinal, DLL we only pretend to know) gets a stub, with ordinals turned into the name "#123" so the stub can at least complain in readable English
int w32_bind_import(struct w32_load *ctx, const char *dll, const char *name, uint32_t ordinal, uint64_t *addr)
{
    build_index();
    if (!kx_ok)
        return -E_NOEXEC;
    const char *cls = dll_class(dll);
    if (!cls) {
        klog(LOG_WARN, "win32: '%s' imports from unknown DLL '%s'", ctx->t->name, dll);
        return -E_NOENT;
    }
    char ord[16];
    if (!name) {
        ord[0] = '#';
        int n = 1;
        char tmp[8];
        int k = 0;
        uint32_t v = ordinal;
        do { tmp[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k)
            ord[n++] = tmp[--k];
        ord[n] = 0;
        name = ord;
    } else if (*cls) {
        for (unsigned i = 0; i < nkx; i++)
            if (!strcmp(kx[i].dll, cls) && !strcmp(kx[i].name, name)) {
                *addr = kx[i].addr;
                return 0;
            }
    }
    return make_stub(ctx, name, addr);
}

// Writes into another task's memory by physical frame a page at a time, the same loop as img_wr in pe.c, which means this exists twice
static int kput(struct task *t, uint64_t va, const void *src, size_t n)
{
    const uint8_t *s = src;
    while (n) {
        uint64_t pa = vmm_as_lookup(t->as, ALIGN_DOWN(va, PAGE_SIZE), NULL);
        if (pa == VMM_NOT_MAPPED)
            return -E_FAULT;
        size_t off = (size_t)(va & (PAGE_SIZE - 1)), chunk = PAGE_SIZE - off;
        if (chunk > n)
            chunk = n;
        memcpy((void *)(pa + off), s, chunk);
        s += chunk; va += chunk; n -= chunk;
    }
    return 0;
}

// Loads the runtime into the new task as a normal ELF and rejects it if it doesn't land exactly at W32_RT_BASE, everything else assumes that address
static int load_runtime(struct task *t)
{
    struct elf_info ei;
    int rc = elf_load(t, w32rt_start, (size_t)(w32rt_end - w32rt_start), &ei);
    if (!rc && ei.lo != W32_RT_BASE)
        rc = -E_NOEXEC;
    return rc;
}

// Zeroed read/write/no-execute user pages, used for the TEB, PEB and startup block
static int map_rw(struct task *t, uint64_t base, unsigned pages)
{
    for (unsigned i = 0; i < pages; i++)
        if (!task_map_zeroed(t->as, base + i * PAGE_SIZE, VMM_USER | VMM_WRITE | VMM_NX))
            return -E_NOMEM;
    return 0;
}

// Packs a string list back to back as NUL-separated strings, that's how argv and envp get to the runtime, returns the new offset
static size_t pack(uint8_t *dst, size_t off, const char *const *list, uint32_t *count)
{
    *count = 0;
    for (; list && list[*count]; (*count)++) {
        size_t n = strlen(list[*count]) + 1;
        memcpy(dst + off, list[*count], n);
        off += n;
    }
    return off;
}

// Builds a whole Windows process from a PE image. It launches at the RUNTIME's entry, not the exe's, because the runtime has to set up Windows first and then jump to the exe. If anything fails the task gets thrown away and *err says why
struct task *win32_task_new(const char *name, const void *img, size_t size, const char *const *argv,
                            const char *const *envp, const char *cwd, const char *imgpath, int *err)
{
    int dummy;
    if (!err)
        err = &dummy;
    build_index();
    if (!kx_ok) {
        *err = -E_NOEXEC;
        return NULL;
    }
    size_t total = 0;
    for (int i = 0; argv && argv[i]; i++) total += strlen(argv[i]) + 1;
    for (int i = 0; envp && envp[i]; i++) total += strlen(envp[i]) + 1;
    total += (imgpath ? strlen(imgpath) : 0) + 1 + (cwd ? strlen(cwd) : 0) + 1;
    if (total > W32_STRINGS_MAX - 64 || sizeof(struct w32_startup) + total > W32_START_PAGES * PAGE_SIZE) {
        *err = -E_2BIG;
        return NULL;
    }

    struct task *t = task_alloc(name);
    if (!t) {
        *err = -E_NOMEM;
        return NULL;
    }
    struct lx *l = kzalloc(sizeof(*l));
    if (!l) {
        task_discard(t);
        *err = -E_NOMEM;
        return NULL;
    }
    t->lx = l;
    // This is what makes it a Windows process, same Linux task underneath, different flavor, and the VM syscalls check it before they'll talk to you
    l->flavor = LXF_WIN32;
    l->gs_base = W32_TEB_BASE;
    // 0x027F is 53-bit x87 precision, which is what Windows gives you and what MSVC code expects (Linux defaults to 64-bit), and 0x1F80 is the normal SSE state with every exception masked
    uint16_t fcw = 0x027F;
    uint32_t mxcsr = 0x1F80;
    memcpy(lx_fpu(l), &fcw, 2);
    memcpy(lx_fpu(l) + 24, &mxcsr, 4);
    l->fpu_valid = true;
    // Random seed is the CPU's cycle counter, the clock, and the golden ratio constant, which is random enough for a hobby OS and useless for anything that matters
    l->rng = rdtsc() ^ (clock_ns() << 17) ^ 0x9E3779B97F4A7C15ULL;
    // The working directory starts as a lone backslash, the root of the NT namespace, so relative paths have somewhere to start
    l->cwd[0] = '\\';

    struct w32_load ctx = { t, 0, rt_missing_va };
    struct pe_info pi;
    memset(&pi, 0, sizeof(pi));
    int rc = load_runtime(t);
    if (rc)
        klog(LOG_WARN, "win32: '%s': runtime image rejected (%d)", name, rc);
    if (!rc) {
        rc = pe_load(t, &ctx, img, size, &pi);
        if (rc)
            klog(LOG_WARN, "win32: '%s': PE image rejected (%d)", name, rc);
    }

    uint64_t blk_end = W32_START_BASE + W32_START_PAGES * PAGE_SIZE;
    if (!rc && lx_vma_busy(l, W32_TEB_BASE, blk_end))
        rc = -E_NOEXEC;
    if (!rc)
        rc = map_rw(t, W32_TEB_BASE, (unsigned)((blk_end - W32_TEB_BASE) / PAGE_SIZE));
    if (!rc) {
        rc = lx_vma_add(l, W32_TEB_BASE, blk_end, VM_PROT_R | VM_PROT_W, LXV_WCOMMIT);
        if (!rc) {
            l->vma[l->nvma - 1].abase = W32_TEB_BASE;
            l->vma[l->nvma - 1].aprot = VM_PROT_R | VM_PROT_W;
        }
    }
    // Stack is whatever the exe asked for (1 MiB if it didn't say) clamped between 256 KiB and the max, because that number comes from the exe's header and we trust those about as far as we can throw them
    uint64_t ssize = pi.stack_reserve ? pi.stack_reserve : 1 * MiB;
    if (ssize < 256 * KiB) ssize = 256 * KiB;
    if (ssize > LX_STACK_MAX) ssize = LX_STACK_MAX;
    ssize = ALIGN_UP(ssize, PAGE_SIZE);
    uint64_t stack_low = LX_STACK_TOP - ssize;
    if (!rc) {
        rc = lx_vma_add(l, stack_low, LX_STACK_TOP, VM_PROT_R | VM_PROT_W, LXV_WCOMMIT);
        if (!rc) {
            l->vma[l->nvma - 1].abase = stack_low;
            l->vma[l->nvma - 1].aprot = VM_PROT_R | VM_PROT_W;
        }
    }
    if (!rc) {
        uint64_t top = pi.base + pi.size;
        if (blk_end > top) top = blk_end;
        // Heap break starts above the image and the TEB block rounded to 64K, and find_gap in winvm.c treats it as the don't-allocate-below-this line
        l->brk_start = l->brk_cur = ALIGN_UP(top, VM_GRANULARITY);
    }
    // Launches at the runtime's entry with the startup block address as its argument, that's how the runtime finds the exe, the TEB, the PEB and everything else
    if (!rc && !task_launch(t, rt_entry, LX_STACK_TOP - 0x28, W32_START_BASE))
        rc = -E_NOMEM;
    if (rc) {
        task_discard(t);
        *err = rc;
        return NULL;
    }

    // The startup block (args, env, image info), the TEB and the PEB, built in kernel memory first and copied into the process after
    uint8_t *su = kzalloc(W32_START_PAGES * PAGE_SIZE);
    uint8_t *teb = kzalloc(W32_TEB_PAGES * PAGE_SIZE);
    uint8_t *peb = kzalloc(PAGE_SIZE);
    if (!su || !teb || !peb) {
        kfree(su); kfree(teb); kfree(peb);
        task_start(t);
        task_wait(t);
        *err = -E_NOMEM;
        return NULL;
    }
    struct w32_startup *s = (struct w32_startup *)su;
    s->magic = W32_START_MAGIC;
    s->version = W32_START_VERSION;
    s->image_base = pi.base; s->image_size = pi.size; s->entry = pi.entry;
    s->peb = W32_PEB_BASE; s->teb = W32_TEB_BASE;
    s->stack_top = LX_STACK_TOP; s->stack_low = stack_low;
    s->tls_block = pi.tls_block;
    s->subsystem = pi.subsystem;
    s->ntls = pi.ntls;
    memcpy(s->tls_callbacks, pi.tls_cb, sizeof(pi.tls_cb));
    s->nmissing = ctx.nmissing;
    size_t off = sizeof(*s);
    s->args_off = (uint32_t)off;
    size_t e = pack(su, off, argv, &s->argc);
    s->args_len = (uint32_t)(e - off);
    off = e;
    s->env_off = (uint32_t)off;
    e = pack(su, off, envp, &s->envc);
    s->env_len = (uint32_t)(e - off);
    off = e;
    s->path_off = (uint32_t)off;
    if (imgpath) { memcpy(su + off, imgpath, strlen(imgpath)); off += strlen(imgpath); }
    su[off++] = 0;
    s->cwd_off = (uint32_t)off;
    if (cwd) { memcpy(su + off, cwd, strlen(cwd)); off += strlen(cwd); }
    su[off++] = 0;

    uint64_t v;
    #define P64(b, o, x) do { uint64_t _v = (x); memcpy((b) + (o), &_v, 8); } while (0)
    #define P32(b, o, x) do { uint32_t _v = (x); memcpy((b) + (o), &_v, 4); } while (0)
    P64(teb, TEB_STACKBASE, LX_STACK_TOP);
    P64(teb, TEB_STACKLIMIT, stack_low);
    P64(teb, TEB_SELF, W32_TEB_BASE);
    P64(teb, TEB_PID, t->id * 4ULL);
    P64(teb, TEB_TID, t->main->tid * 4ULL);
    P64(teb, TEB_PEB, W32_PEB_BASE);
    P64(teb, TEB_TLSARRAY, pi.tls_block);
    v = pi.tls_block ? W32_TEB_BASE + TEB_TLSARRAY : 0;
    P64(teb, TEB_TLSPTR, v);
    // Zeroed even though kzalloc already did it, because some exes check if they're being debugged and I'm not about to find out what they do if the answer is yes
    peb[PEB_BEINGDEBUGGED] = 0;
    P64(peb, PEB_IMAGEBASE, pi.base);
    P64(peb, PEB_PROCESSPARAMS, W32_START_BASE);
    P32(peb, PEB_NCPU, 1);
    // Every program gets told it's on Windows 10 build 19045 (22H2), which is a lie, but it's the lie that makes version checks pass
    P32(peb, PEB_OSMAJOR, 10);
    P32(peb, PEB_OSMINOR, 0);
    P32(peb, PEB_OSBUILD, 19045);
    P32(peb, PEB_OSPLATFORM, 2);
    #undef P64
    #undef P32
    kput(t, W32_TEB_BASE, teb, W32_TEB_PAGES * PAGE_SIZE);
    kput(t, W32_PEB_BASE, peb, PAGE_SIZE);
    kput(t, W32_START_BASE, su, W32_START_PAGES * PAGE_SIZE);
    kfree(su); kfree(teb); kfree(peb);
    *err = 0;
    return t;
}

// Loads the PE into a throwaway task just to see if the loader would take it, then kills the task, so you can ask without actually running the exe
int win32_pe_probe(const void *img, size_t size)
{
    build_index();
    if (!kx_ok)
        return -E_NOEXEC;
    struct task *t = task_alloc("probe");
    if (!t)
        return -E_NOMEM;
    int rc = -E_NOMEM;
    t->lx = kzalloc(sizeof(struct lx));
    if (t->lx) {
        t->lx->flavor = LXF_WIN32;
        struct w32_load ctx = { t, 0, rt_missing_va };
        struct pe_info pi;
        memset(&pi, 0, sizeof(pi));
        rc = pe_load(t, &ctx, img, size, &pi);
    }
    task_discard(t);
    return rc;
}

// Biggest exe we'll read in, because the whole file gets kmalloc'd in one go
#define EXEC_MAX (32 * MiB)

// Starts a process from an NT path like \Volumes\C\foo.exe by finding the volume, reading the whole exe into a buffer and handing it to win32_task_new, and anything that isn't under \Volumes\ is -E_NOENT
struct task *win32_task_new_from_file(const char *ntpath, const char *const *argv, const char *const *envp,
                                      const char *cwd, int *err)
{
    int dummy;
    if (!err)
        err = &dummy;
    static const char pre[] = "\\Volumes\\";
    if (strlen(ntpath) <= sizeof(pre) - 1 || memcmp(ntpath, pre, sizeof(pre) - 1)) {
        *err = -E_NOENT;
        return NULL;
    }
    const char *p = ntpath + sizeof(pre) - 1;
    char vol[VFS_VOL_NAME_MAX + 1], full[VFS_VOL_NAME_MAX + 16];
    size_t k = 0;
    while (p[k] && p[k] != '\\' && k < VFS_VOL_NAME_MAX)
        vol[k] = p[k], k++;
    vol[k] = 0;
    memcpy(full, pre, sizeof(pre) - 1);
    memcpy(full + sizeof(pre) - 1, vol, k + 1);
    struct object *o;
    int rc = ns_lookup(full, &o);
    if (rc || o->type != &volume_type) {
        if (!rc)
            obj_unref(o);
        *err = -E_NOENT;
        return NULL;
    }
    struct file *f;
    rc = vfs_open((struct volume *)o, p + k, FD_OPEN, FILE_READ_DATA, 0, false, &f);
    obj_unref(o);
    if (rc) {
        *err = rc;
        return NULL;
    }
    struct file_stat st;
    rc = file_stat(f, &st);
    if (!rc && (st.type != FT_FILE || st.size == 0 || st.size > EXEC_MAX))
        rc = st.type == FT_DIR ? -E_ISDIR : -E_NOEXEC;
    uint8_t *img = rc ? NULL : kmalloc((size_t)st.size);
    if (!rc && !img)
        rc = -E_NOMEM;
    for (uint64_t off = 0; !rc && off < st.size;) {
        long n = file_pread(f, off, img + off, (size_t)(st.size - off > 65536 ? 65536 : st.size - off));
        if (n <= 0)
            rc = n < 0 ? (int)n : -E_IO;
        else
            off += (uint64_t)n;
    }
    obj_unref(&f->o);
    if (rc) {
        kfree(img);
        *err = rc;
        return NULL;
    }
    const char *nm = p + k;
    // The process name is whatever comes after the last backslash, there's no basename() in a kernel
    for (const char *q = nm; *q; q++)
        if (*q == '\\')
            nm = q + 1;
    struct task *t = win32_task_new(nm, img, (size_t)st.size, argv, envp, cwd, ntpath, err);
    kfree(img);
    return t;
}
