#include "kernel.h"
#include "io.h"
#include "irq.h"
#include "mm.h"
#include "arch.h"
#include "task.h"
#include "timer.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"
#include "vfs.h"
#include "blk.h"
#include "linux.h"

static char *strcpy_(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

#define MSR_FS_BASE 0xC0000100
#define CR0_EM (1ULL << 2)

#define AT_FDCWD (-100)
#define LX_O_ACCMODE  03
#define LX_O_CREAT    0100
#define LX_O_EXCL     0200
#define LX_O_TRUNC    01000
#define LX_O_APPEND   02000
#define LX_O_DIRECTORY 0200000
#define LX_O_CLOEXEC  02000000
#define LX_MAP_SHARED 0x01
#define LX_MAP_PRIVATE 0x02
#define LX_MAP_FIXED  0x10
#define LX_MAP_ANON   0x20
#define LX_MAP_FIXED_NOREPLACE 0x100000

struct lx_stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t  size, blksize, blocks;
    uint64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t  unused[3];
};
struct lx_timespec { int64_t sec, nsec; };
struct lx_iovec { uint64_t base, len; };

static bool fpu_on;
static uint64_t cur_fs;
static uint64_t cur_gs;

#define MSR_KERNEL_GS_BASE 0xC0000102

void linux_arch_init(void)
{
    write_cr4(read_cr4() | (1ULL << 9) | (1ULL << 10));
    fpu_on = false;
}

void linux_switch_out(struct task *prev)
{
    if (prev->lx && prev->lx->fpu_valid)
        fxsave_(lx_fpu(prev->lx));
}

void linux_switch_in(struct task *next)
{
    if (!next)
        return;
    if (next->lx) {
        if (!fpu_on) {
            write_cr0(read_cr0() & ~CR0_EM);
            fpu_on = true;
        }
        fxrstor_(lx_fpu(next->lx));
        if (cur_fs != next->lx->fs_base) {
            wrmsr(MSR_FS_BASE, next->lx->fs_base);
            cur_fs = next->lx->fs_base;
        }
        if (cur_gs != next->lx->gs_base) {
            wrmsr(MSR_KERNEL_GS_BASE, next->lx->gs_base);
            cur_gs = next->lx->gs_base;
        }
    } else {
        if (fpu_on) {
            write_cr0(read_cr0() | CR0_EM);
            fpu_on = false;
        }
        if (cur_fs) {
            wrmsr(MSR_FS_BASE, 0);
            cur_fs = 0;
        }
        if (cur_gs) {
            wrmsr(MSR_KERNEL_GS_BASE, 0);
            cur_gs = 0;
        }
    }
}

uint64_t lx_prot_to_vmm(uint32_t prot)
{
    if (!prot)
        return 0;
    return VMM_USER | ((prot & LX_PROT_WRITE) ? VMM_WRITE : 0) | ((prot & LX_PROT_EXEC) ? 0 : VMM_NX);
}

static struct lx_vma *vma_find(struct lx *l, uint64_t addr)
{
    for (unsigned i = 0; i < l->nvma; i++)
        if (addr >= l->vma[i].start && addr < l->vma[i].end)
            return &l->vma[i];
    return NULL;
}

static bool vma_overlaps(struct lx *l, uint64_t s, uint64_t e)
{
    for (unsigned i = 0; i < l->nvma; i++)
        if (l->vma[i].start < e && l->vma[i].end > s)
            return true;
    return false;
}

static void vma_coalesce(struct lx *l)
{
    for (bool again = true; again;) {
        again = false;
        for (unsigned i = 0; i < l->nvma && !again; i++)
            for (unsigned j = 0; j < l->nvma && !again; j++) {
                struct lx_vma *a = &l->vma[i], *b = &l->vma[j];
                if (i != j && a->kind == LXV_ANON && b->kind == LXV_ANON && a->prot == b->prot &&
                    a->end == b->start) {
                    a->end = b->end;
                    l->vma[j] = l->vma[--l->nvma];
                    again = true;
                }
            }
    }
}

int lx_vma_add(struct lx *l, uint64_t start, uint64_t end, uint32_t prot, uint32_t kind)
{
    if (kind == LXV_IMAGE)
        for (unsigned i = 0; i < l->nvma; i++) {
            struct lx_vma *v = &l->vma[i];
            if (v->kind == LXV_IMAGE && v->start < end && v->end > start) {
                if (start < v->start) v->start = start;
                if (end > v->end) v->end = end;
                v->prot |= prot;
                return 0;
            }
        }
    if (l->nvma >= LX_MAX_VMA)
        return -E_NOMEM;
    l->vma[l->nvma++] = (struct lx_vma){ start, end, prot, kind };
    return 0;
}

static int vma_cut(struct task *t, uint64_t s, uint64_t e)
{
    struct lx *l = t->lx;
    unsigned need = 0;
    for (unsigned i = 0; i < l->nvma; i++)
        if (l->vma[i].start < s && l->vma[i].end > e)
            need++;
    if (l->nvma + need > LX_MAX_VMA)
        return -E_NOMEM;
    for (unsigned i = 0; i < l->nvma; i++) {
        struct lx_vma *v = &l->vma[i];
        if (v->end <= s || v->start >= e)
            continue;
        uint64_t os = v->start > s ? v->start : s, oe = v->end < e ? v->end : e;
        for (uint64_t a = os; a < oe; a += PAGE_SIZE)
            vmm_as_unmap(t->as, a);
        if (v->start < s && v->end > e) {
            l->vma[l->nvma++] = (struct lx_vma){ e, v->end, v->prot, v->kind, v->abase, v->aprot, 0 };
            v->end = s;
        } else if (v->start < s) {
            v->end = s;
        } else if (v->end > e) {
            v->start = e;
        } else {
            l->vma[i] = l->vma[--l->nvma];
            i--;
        }
    }
    return 0;
}

static uint64_t vma_find_gap(struct lx *l, uint64_t len)
{
    uint64_t lower = ALIGN_UP(l->brk_cur, PAGE_SIZE) + 1 * MiB;
    uint64_t top = LX_MMAP_TOP;
    for (;;) {
        if (top < lower + len)
            return 0;
        uint64_t s = top - len, hit = 0;
        bool any = false;
        for (unsigned i = 0; i < l->nvma; i++)
            if (l->vma[i].start < top && l->vma[i].end > s && (!any || l->vma[i].start > hit)) {
                hit = l->vma[i].start;
                any = true;
            }
        if (!any)
            return s;
        top = hit;
    }
}

static int lx_prefault(struct task *t, uint64_t addr, size_t n, bool write)
{
    if (!n)
        return 0;
    if (addr < USER_BASE || addr >= USER_TOP || n > USER_TOP - addr)
        return -E_FAULT;
    struct lx *l = t->lx;
    for (uint64_t a = ALIGN_DOWN(addr, PAGE_SIZE); a < addr + n; a += PAGE_SIZE) {
        uint64_t fl;
        if (vmm_as_lookup(t->as, a, &fl) != VMM_NOT_MAPPED) {
            if (!(fl & VMM_USER) || (write && !(fl & VMM_WRITE)))
                return -E_FAULT;
            continue;
        }
        struct lx_vma *v = vma_find(l, a);
        if (!v || !(v->prot & (write ? LX_PROT_WRITE : LX_PROT_READ)))
            return -E_FAULT;
        if (!task_map_zeroed(t->as, a, lx_prot_to_vmm(v->prot)))
            return -E_NOMEM;
    }
    return 0;
}

static int cin(struct task *t, void *dst, uint64_t uaddr, size_t n)
{
    int rc = lx_prefault(t, uaddr, n, false);
    return rc ? rc : user_copy_in(t, dst, uaddr, n);
}

static int cout(struct task *t, uint64_t uaddr, const void *src, size_t n)
{
    int rc = lx_prefault(t, uaddr, n, true);
    return rc ? rc : user_copy_out(t, uaddr, src, n);
}

static long cstr(struct task *t, char *dst, size_t cap, uint64_t uaddr)
{
    size_t len = 0;
    for (;;) {
        size_t chunk = PAGE_SIZE - (size_t)(uaddr & (PAGE_SIZE - 1));
        if (chunk > cap - len)
            chunk = cap - len;
        if (!chunk)
            return -E_NAMETOOLONG;
        int rc = cin(t, dst + len, uaddr, chunk);
        if (rc)
            return rc;
        for (size_t i = 0; i < chunk; i++)
            if (!dst[len + i])
                return (long)(len + i);
        len += chunk;
        uaddr += chunk;
        if (len >= cap)
            return -E_NAMETOOLONG;
    }
}

bool linux_demand_fault(struct regs *r, uint64_t addr)
{
    struct task *t = task_current();
    if (!t || !t->lx)
        return false;
    if ((r->err & (1 | 4 | 8)) != 4)
        return false;
    struct lx_vma *v = vma_find(t->lx, addr);
    if (!v)
        return false;
    uint32_t need = (r->err & 16) ? LX_PROT_EXEC : (r->err & 2) ? LX_PROT_WRITE : LX_PROT_READ;
    if (!(v->prot & need))
        return false;
    uint64_t page = ALIGN_DOWN(addr, PAGE_SIZE);
    if (vmm_as_lookup(t->as, page, NULL) != VMM_NOT_MAPPED)
        return true;
    return task_map_zeroed(t->as, page, lx_prot_to_vmm(v->prot));
}

bool lx_fault_in(struct task *t, uint64_t addr, uint32_t need)
{
    if (!t || !t->lx || addr < USER_BASE || addr >= USER_TOP)
        return false;
    struct lx_vma *v = vma_find(t->lx, addr);
    if (!v || (need && !(v->prot & need)))
        return false;
    uint64_t page = ALIGN_DOWN(addr, PAGE_SIZE);
    if (vmm_as_lookup(t->as, page, NULL) != VMM_NOT_MAPPED)
        return true;
    return task_map_zeroed(t->as, page, lx_prot_to_vmm(v->prot));
}

struct lx_vma *lx_vma_lookup(struct lx *l, uint64_t addr) { return vma_find(l, addr); }
bool lx_vma_busy(struct lx *l, uint64_t start, uint64_t end) { return vma_overlaps(l, start, end); }
int lx_vma_cut(struct task *t, uint64_t start, uint64_t end) { return vma_cut(t, start, end); }

static long lx_brk(struct task *t, uint64_t req)
{
    struct lx *l = t->lx;
    if (req < l->brk_start || req > l->brk_start + LX_BRK_MAX)
        return (long)l->brk_cur;
    uint64_t old_end = ALIGN_UP(l->brk_cur, PAGE_SIZE), new_end = ALIGN_UP(req, PAGE_SIZE);
    if (new_end > old_end) {
        if (vma_overlaps(l, old_end, new_end))
            return (long)l->brk_cur;
        struct lx_vma *h = NULL;
        for (unsigned i = 0; i < l->nvma; i++)
            if (l->vma[i].kind == LXV_HEAP)
                h = &l->vma[i];
        if (h)
            h->end = new_end;
        else if (lx_vma_add(l, l->brk_start, new_end, LX_PROT_READ | LX_PROT_WRITE, LXV_HEAP))
            return (long)l->brk_cur;
    } else if (new_end < old_end) {
        for (uint64_t a = new_end; a < old_end; a += PAGE_SIZE)
            vmm_as_unmap(t->as, a);
        for (unsigned i = 0; i < l->nvma; i++)
            if (l->vma[i].kind == LXV_HEAP) {
                if (new_end <= l->vma[i].start)
                    l->vma[i] = l->vma[--l->nvma];
                else
                    l->vma[i].end = new_end;
                break;
            }
    }
    l->brk_cur = req;
    return (long)req;
}

static long lx_mmap(struct task *t, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, int fd, uint64_t off)
{
    struct lx *l = t->lx;
    if (!len || (off & (PAGE_SIZE - 1)) || (prot & ~7u))
        return -E_INVAL;
    if (len > USER_TOP - USER_BASE)
        return -E_NOMEM;
    len = ALIGN_UP(len, PAGE_SIZE);
    bool anon = flags & LX_MAP_ANON;
    bool shared = flags & LX_MAP_SHARED;
    if (!(flags & (LX_MAP_SHARED | LX_MAP_PRIVATE)) || ((flags & LX_MAP_SHARED) && (flags & LX_MAP_PRIVATE)))
        return -E_INVAL;
    if (!(prot & LX_PROT_READ) && prot)
        prot |= LX_PROT_READ;

    struct file *f = NULL;
    if (!anon) {
        if (fd < 0 || fd >= LX_MAX_FD || l->fd[fd].kind != LXFD_FILE)
            return -E_BADF;
        struct object *o;
        uint32_t granted = 0;
        if (ht_lookup(t->ht, l->fd[fd].handle, &file_type, FILE_READ_DATA, &o, &granted))
            return -E_ACCES;
        f = (struct file *)o;
        if (shared && (prot & LX_PROT_WRITE)) {
            obj_unref(o);
            return (granted & FILE_WRITE_DATA) ? -E_NODEV : -E_ACCES;
        }
    }

    uint64_t start;
    if (flags & (LX_MAP_FIXED | LX_MAP_FIXED_NOREPLACE)) {
        if ((addr & (PAGE_SIZE - 1)) || addr < USER_BASE + PAGE_SIZE || addr >= USER_TOP || len > USER_TOP - addr) {
            if (f) obj_unref(&f->o);
            return -E_INVAL;
        }
        start = addr;
        if ((flags & LX_MAP_FIXED_NOREPLACE) && vma_overlaps(l, start, start + len)) {
            if (f) obj_unref(&f->o);
            return -E_EXIST;
        }
        int rc = vma_cut(t, start, start + len);
        if (rc) {
            if (f) obj_unref(&f->o);
            return rc;
        }
    } else {
        start = vma_find_gap(l, len);
        if (!start) {
            if (f) obj_unref(&f->o);
            return -E_NOMEM;
        }
    }

    int rc = lx_vma_add(l, start, start + len, prot, anon ? LXV_ANON : LXV_FILE);
    if (rc) {
        if (f) obj_unref(&f->o);
        return rc;
    }
    if (f) {
        uint64_t fl = lx_prot_to_vmm(prot);
        struct file_stat st;
        uint64_t fsize = file_stat(f, &st) == 0 ? st.size : 0;
        for (uint64_t a = 0; a < len; a += PAGE_SIZE) {
            if (off + a >= fsize)
                break;
            if (!task_map_zeroed(t->as, start + a, VMM_USER | VMM_WRITE | VMM_NX)) {
                rc = -E_NOMEM;
                break;
            }
            uint64_t pa = vmm_as_lookup(t->as, start + a, NULL);
            long n = file_pread(f, off + a, (void *)pa, PAGE_SIZE);
            if (n < 0) {
                rc = (int)n;
                break;
            }
            vmm_as_protect(t->as, start + a, fl);
        }
        obj_unref(&f->o);
        if (rc) {
            vma_cut(t, start, start + len);
            return rc;
        }
    }
    if (anon)
        vma_coalesce(l);
    return (long)start;
}

static long lx_munmap(struct task *t, uint64_t addr, uint64_t len)
{
    if ((addr & (PAGE_SIZE - 1)) || !len || addr < USER_BASE || addr >= USER_TOP || len > USER_TOP - addr)
        return -E_INVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (addr + len > USER_TOP)
        return -E_INVAL;
    return vma_cut(t, addr, addr + len);
}

static long lx_mprotect(struct task *t, uint64_t addr, uint64_t len, uint32_t prot)
{
    struct lx *l = t->lx;
    if ((addr & (PAGE_SIZE - 1)) || (prot & ~7u) || addr < USER_BASE || addr >= USER_TOP)
        return -E_INVAL;
    if (!len)
        return 0;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (len > USER_TOP - addr)
        return -E_NOMEM;
    uint64_t s = addr, e = addr + len;
    if (!(prot & LX_PROT_READ) && prot)
        prot |= LX_PROT_READ;
    for (uint64_t a = s; a < e;) {
        struct lx_vma *v = vma_find(l, a);
        if (!v)
            return -E_NOMEM;
        a = v->end;
    }
    if (l->nvma + 2 > LX_MAX_VMA)
        return -E_NOMEM;
    for (unsigned i = 0; i < l->nvma; i++) {
        struct lx_vma *v = &l->vma[i];
        if (v->start < s && v->end > s) {
            l->vma[l->nvma++] = (struct lx_vma){ s, v->end, v->prot, v->kind, v->abase, v->aprot, 0 };
            v->end = s;
        }
    }
    for (unsigned i = 0; i < l->nvma; i++) {
        struct lx_vma *v = &l->vma[i];
        if (v->start < e && v->end > e) {
            l->vma[l->nvma++] = (struct lx_vma){ e, v->end, v->prot, v->kind, v->abase, v->aprot, 0 };
            v->end = e;
        }
    }
    for (unsigned i = 0; i < l->nvma; i++)
        if (l->vma[i].start >= s && l->vma[i].end <= e)
            l->vma[i].prot = prot;
    uint64_t fl = lx_prot_to_vmm(prot);
    for (uint64_t a = s; a < e; a += PAGE_SIZE)
        vmm_as_protect(t->as, a, fl);
    vma_coalesce(l);
    return 0;
}

static long lx_madvise(struct task *t, uint64_t addr, uint64_t len, int advice)
{
    if (addr & (PAGE_SIZE - 1))
        return -E_INVAL;
    if (advice == 4  || advice == 8 ) {
        uint64_t e = ALIGN_UP(addr + len, PAGE_SIZE);
        for (uint64_t a = addr; a < e; a += PAGE_SIZE) {
            struct lx_vma *v = vma_find(t->lx, a);
            if (v && v->kind == LXV_ANON)
                vmm_as_unmap(t->as, a);
        }
    }
    return 0;
}

static long lx_path_core(const char *cwd, int dirfd, char *raw, char *out);

static long lx_path_nt(struct task *t, int dirfd, uint64_t upath, char *out)
{
    char raw[LX_PATH_MAX + 2];
    long n = cstr(t, raw, sizeof(raw), upath);
    if (n < 0)
        return n == -E_NAMETOOLONG ? -E_NAMETOOLONG : n;
    if (n == 0)
        return -E_NOENT;
    return lx_path_core(t->lx->cwd, dirfd, raw, out);
}

int linux_path_probe(const char *cwd_nt, const char *path, char *out, size_t cap)
{
    char raw[LX_PATH_MAX + 2], nt[2 * LX_PATH_MAX + 4];
    size_t n = strlen(path);
    if (n == 0)
        return -E_NOENT;
    if (n >= sizeof(raw))
        return -E_NAMETOOLONG;
    memcpy(raw, path, n + 1);
    long rc = lx_path_core(cwd_nt, AT_FDCWD, raw, nt);
    if (rc)
        return (int)rc;
    if (strlen(nt) >= cap)
        return -E_RANGE;
    memcpy(out, nt, strlen(nt) + 1);
    return 0;
}

static long lx_path_core(const char *cwd, int dirfd, char *raw, char *out)
{
    {
        // Somebody's Linux program handed us a Windows path (C:\foo or \foo) so rewrite it to /Volumes/<vol>/... first, yes we support this, no I don't want to talk about it
        char al[LX_PATH_MAX + 2];
        bool changed;
        long arc = vfs_alias_path(raw, al, sizeof(al), &changed);
        if (arc)
            return arc;
        if (changed)
            memcpy(raw, al, strlen(al) + 1);
    }
    if (raw[0] != '/' && dirfd != AT_FDCWD)
        return -E_BADF;

    char comps[LX_PATH_MAX + 2];
    size_t clen = 0;
    comps[0] = 0;
    const char *src = raw;
    char full[2 * LX_PATH_MAX + 4];
    if (raw[0] != '/') {
        size_t k = 0;
        for (const char *c = cwd; *c && k < sizeof(full) - 2; c++)
            full[k++] = *c == '\\' ? '/' : *c;
        if (k && full[k - 1] != '/')
            full[k++] = '/';
        size_t rl = strlen(raw);
        if (k + rl >= sizeof(full))
            return -E_NAMETOOLONG;
        memcpy(full + k, raw, rl + 1);
        src = full;
    }
    const char *p = src;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *c = p;
        while (*p && *p != '/')
            p++;
        size_t cl = (size_t)(p - c);
        if (cl == 1 && c[0] == '.')
            continue;
        if (cl == 2 && c[0] == '.' && c[1] == '.') {
            while (clen && comps[clen - 1] != '/')
                clen--;
            if (clen)
                clen--;
            comps[clen] = 0;
            continue;
        }
        if (clen + 1 + cl >= LX_PATH_MAX)
            return -E_NAMETOOLONG;
        comps[clen++] = '/';
        memcpy(comps + clen, c, cl);
        clen += cl;
        comps[clen] = 0;
    }
    if (!clen) {
        out[0] = '\\';
        out[1] = 0;
        return 0;
    }
    for (size_t i = 0; i <= clen; i++)
        out[i] = comps[i] == '/' ? '\\' : comps[i];
    {
        // If the first component isn't in the object namespace root, put \Volumes\<C: volume> in front of it, so a Linux path like /foo ends up living on C:, which is a sentence I never wanted to write
        char top[NS_NAME_MAX + 2];
        size_t tl = 0;
        top[tl++] = '\\';
        for (size_t i = 1; out[i] && out[i] != '\\' && tl < sizeof(top) - 1; i++)
            top[tl++] = out[i];
        top[tl] = 0;
        struct object *o = NULL;
        bool known = tl <= NS_NAME_MAX && ns_lookup(top, &o) == 0;
        if (o)
            obj_unref(o);
        char vol[VFS_VOL_NAME_MAX + 1];
        if (!known && vfs_dos_volume('C', vol) == 0) {
            size_t vl = strlen(vol);
            if (9 + vl + clen >= 2 * LX_PATH_MAX)
                return -E_NAMETOOLONG;
            memmove(out + 9 + vl, out, clen + 1);
            memcpy(out, "\\Volumes\\", 9);
            memcpy(out + 9, vol, vl);
        }
    }
    return 0;
}

static bool is_ns_dir(const char *nt)
{
    return !strcmp(nt, "\\") || !strcmp(nt, "\\Volumes");
}

static int fd_alloc(struct lx *l, int from)
{
    for (int i = from; i < LX_MAX_FD; i++)
        if (l->fd[i].kind == LXFD_NONE)
            return i;
    return -E_MFILE;
}

static struct lx_fd *fd_get(struct lx *l, int fd)
{
    if (fd < 0 || fd >= LX_MAX_FD || l->fd[fd].kind == LXFD_NONE)
        return NULL;
    return &l->fd[fd];
}

static long fd_file(struct task *t, int fd, uint32_t need, struct file **f)
{
    struct lx_fd *e = fd_get(t->lx, fd);
    if (!e)
        return -E_BADF;
    if (e->kind != LXFD_FILE)
        return -E_INVAL;
    struct object *o;
    int rc = ht_lookup(t->ht, e->handle, &file_type, need, &o, NULL);
    if (rc)
        return rc == -E_ACCES ? -E_BADF : rc;
    *f = (struct file *)o;
    return 0;
}

static long fd_close(struct task *t, int fd)
{
    struct lx_fd *e = fd_get(t->lx, fd);
    if (!e)
        return -E_BADF;
    if (e->kind == LXFD_FILE)
        ht_close(t->ht, e->handle);
    memset(e, 0, sizeof(*e));
    return 0;
}

static long fd_dup_to(struct task *t, int oldfd, int newfd, bool cloexec)
{
    struct lx *l = t->lx;
    struct lx_fd *o = fd_get(l, oldfd);
    if (!o)
        return -E_BADF;
    if (newfd < 0 || newfd >= LX_MAX_FD)
        return newfd < 0 ? -E_INVAL : -E_BADF;
    if (newfd == oldfd)
        return newfd;
    struct lx_fd n = *o;
    n.cloexec = cloexec;
    if (o->kind == LXFD_FILE) {
        uint32_t h;
        int rc = ht_dup(t->ht, t->ht, o->handle, 0, &h);
        if (rc)
            return rc;
        n.handle = h;
    }
    if (l->fd[newfd].kind != LXFD_NONE)
        fd_close(t, newfd);
    l->fd[newfd] = n;
    return newfd;
}

static long console_write(struct task *t, uint64_t ubuf, uint64_t len)
{
    if (len > 64 * KiB)
        len = 64 * KiB;
    char buf[128];
    uint64_t done = 0;
    while (done < len) {
        size_t n = len - done > sizeof(buf) ? sizeof(buf) : (size_t)(len - done);
        int rc = cin(t, buf, ubuf + done, n);
        if (rc)
            return done ? (long)done : rc;
        for (size_t i = 0; i < n; i++)
            kputc(buf[i]);
        done += n;
    }
    return (long)done;
}

#define XFER_CHUNK 4096
#define XFER_MAX   (1u << 20)

static long lx_read_at(struct task *t, struct file *f, uint64_t ubuf, uint64_t len, bool usepos, uint64_t off)
{
    if (len > XFER_MAX)
        len = XFER_MAX;
    int rc = lx_prefault(t, ubuf, len, true);
    if (rc)
        return rc;
    uint8_t *buf = kmalloc(XFER_CHUNK);
    if (!buf)
        return -E_NOMEM;
    long done = 0;
    while ((uint64_t)done < len) {
        size_t c = len - done > XFER_CHUNK ? XFER_CHUNK : (size_t)(len - done);
        long n = usepos ? file_read(f, buf, c) : file_pread(f, off + done, buf, c);
        if (n < 0) {
            rc = (int)n;
            break;
        }
        if (n == 0)
            break;
        user_copy_out(t, ubuf + done, buf, (size_t)n);
        done += n;
        if ((size_t)n < c)
            break;
    }
    kfree(buf);
    return done ? done : rc;
}

static long lx_write_at(struct task *t, struct file *f, uint64_t ubuf, uint64_t len, bool usepos, uint64_t off)
{
    if (len > XFER_MAX)
        len = XFER_MAX;
    uint8_t *buf = kmalloc(XFER_CHUNK);
    if (!buf)
        return -E_NOMEM;
    long done = 0;
    int rc = 0;
    while ((uint64_t)done < len) {
        size_t c = len - done > XFER_CHUNK ? XFER_CHUNK : (size_t)(len - done);
        rc = cin(t, buf, ubuf + done, c);
        if (rc)
            break;
        long n = usepos ? file_write(f, buf, c) : file_pwrite(f, off + done, buf, c);
        if (n < 0) {
            rc = (int)n;
            break;
        }
        done += n;
        if ((size_t)n < c)
            break;
    }
    kfree(buf);
    return done ? done : rc;
}

static long lx_read(struct task *t, int fd, uint64_t ubuf, uint64_t len)
{
    struct lx_fd *e = fd_get(t->lx, fd);
    if (!e)
        return -E_BADF;
    if (e->kind == LXFD_CONSOLE)
        return 0;
    struct file *f;
    long rc = fd_file(t, fd, FILE_READ_DATA, &f);
    if (rc)
        return rc;
    rc = lx_read_at(t, f, ubuf, len, true, 0);
    obj_unref(&f->o);
    return rc;
}

static long lx_write(struct task *t, int fd, uint64_t ubuf, uint64_t len)
{
    struct lx_fd *e = fd_get(t->lx, fd);
    if (!e)
        return -E_BADF;
    if (e->kind == LXFD_CONSOLE)
        return console_write(t, ubuf, len);
    struct file *f;
    long rc = fd_file(t, fd, FILE_WRITE_DATA, &f);
    if (rc)
        return rc;
    if (e->append)
        file_seek(f, 0, SEEK_END);
    rc = lx_write_at(t, f, ubuf, len, true, 0);
    obj_unref(&f->o);
    return rc;
}

static long lx_pio(struct task *t, int fd, uint64_t ubuf, uint64_t len, int64_t off, bool wr)
{
    if (off < 0)
        return -E_INVAL;
    struct lx_fd *e = fd_get(t->lx, fd);
    if (!e)
        return -E_BADF;
    if (e->kind == LXFD_CONSOLE)
        return -E_SPIPE;
    struct file *f;
    long rc = fd_file(t, fd, wr ? FILE_WRITE_DATA : FILE_READ_DATA, &f);
    if (rc)
        return rc;
    rc = wr ? lx_write_at(t, f, ubuf, len, false, (uint64_t)off) : lx_read_at(t, f, ubuf, len, false, (uint64_t)off);
    obj_unref(&f->o);
    return rc;
}

static long lx_vec(struct task *t, int fd, uint64_t uiov, uint64_t cnt, bool wr)
{
    if (cnt > 1024)
        return -E_INVAL;
    long total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        struct lx_iovec v;
        int rc = cin(t, &v, uiov + i * sizeof(v), sizeof(v));
        if (rc)
            return total ? total : rc;
        if (!v.len)
            continue;
        long n = wr ? lx_write(t, fd, v.base, v.len) : lx_read(t, fd, v.base, v.len);
        if (n < 0)
            return total ? total : n;
        total += n;
        if ((uint64_t)n < v.len)
            break;
    }
    return total;
}

static long lx_open_nt(struct task *t, const char *nt, uint32_t flags)
{
    uint32_t acc;
    switch (flags & LX_O_ACCMODE) {
    case 0: acc = FILE_READ_DATA; break;
    case 1: acc = FILE_WRITE_DATA; break;
    case 2: acc = FILE_READ_DATA | FILE_WRITE_DATA; break;
    default: return -E_INVAL;
    }
    if ((flags & LX_O_TRUNC) && !(acc & FILE_WRITE_DATA))
        acc |= FILE_WRITE_DATA;
    uint32_t disp = FD_OPEN;
    if (flags & LX_O_CREAT)
        disp = (flags & LX_O_EXCL) ? FD_CREATE : (flags & LX_O_TRUNC) ? FD_OVERWRITE_IF : FD_OPEN_IF;
    if (flags & LX_O_DIRECTORY)
        disp |= FO_DIRECTORY;
    if (is_ns_dir(nt))
        return -E_NOTSUP;

    int fd = fd_alloc(t->lx, 0);
    if (fd < 0)
        return fd;
    long h = fs_k_open(t, nt, acc, disp);
    if (h < 0)
        return h;
    if ((flags & LX_O_TRUNC) && !(flags & LX_O_CREAT)) {
        struct object *o;
        if (ht_lookup(t->ht, (uint32_t)h, &file_type, FILE_WRITE_DATA, &o, NULL) == 0) {
            file_setsize((struct file *)o, 0);
            obj_unref(o);
        }
    }
    struct lx_fd *e = &t->lx->fd[fd];
    memset(e, 0, sizeof(*e));
    e->kind = LXFD_FILE;
    e->handle = (uint32_t)h;
    e->cloexec = !!(flags & LX_O_CLOEXEC);
    e->append = !!(flags & LX_O_APPEND);
    return fd;
}

static uint64_t fnv(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 1099511628211ULL;
    return h | 1;
}

static uint32_t uid_of(struct task *t)
{
    return t->tok.user == SID_SYSTEM ? 0 : t->tok.user;
}

static void fill_stat(struct task *t, struct lx_stat *s, const struct file_stat *fs, uint64_t ino)
{
    memset(s, 0, sizeof(*s));
    bool dir = fs->type == FT_DIR;
    s->dev = 1;
    s->ino = ino;
    s->nlink = dir ? 2 : 1;
    s->mode = dir ? (040000 | 0755) : (0100000 | ((fs->attr & FA_RDONLY) ? 0444 : 0644));
    s->uid = s->gid = uid_of(t);
    s->size = (int64_t)fs->size;
    s->blksize = 4096;
    s->blocks = (int64_t)((fs->size + 511) / 512);
    s->atime = fs->atime; s->mtime = fs->mtime; s->ctime = fs->ctime;
}

static long lx_stat_nt(struct task *t, const char *nt, uint64_t uout)
{
    struct file_stat fs;
    if (is_ns_dir(nt)) {
        memset(&fs, 0, sizeof(fs));
        fs.type = FT_DIR;
        fs.atime = fs.mtime = fs.ctime = wall_unix();
    } else {
        int rc = fs_k_stat(t, nt, &fs);
        if (rc)
            return rc;
    }
    struct lx_stat s;
    fill_stat(t, &s, &fs, fnv(nt));
    return cout(t, uout, &s, sizeof(s));
}

static long lx_fstat(struct task *t, int fd, uint64_t uout)
{
    struct lx_fd *e = fd_get(t->lx, fd);
    if (!e)
        return -E_BADF;
    struct lx_stat s;
    if (e->kind == LXFD_CONSOLE) {
        memset(&s, 0, sizeof(s));
        s.dev = 1; s.ino = 3 + (uint64_t)fd;
        s.nlink = 1; s.mode = 020620;
        s.rdev = (5 << 8) | 1;
        s.blksize = 1024;
        return cout(t, uout, &s, sizeof(s));
    }
    struct file *f;
    long rc = fd_file(t, fd, 0, &f);
    if (rc)
        return rc;
    struct file_stat fs;
    rc = file_stat(f, &fs);
    if (!rc) {
        fill_stat(t, &s, &fs, (uint64_t)(uintptr_t)f->v->key | 0x100000000ULL);
        rc = cout(t, uout, &s, sizeof(s));
    }
    obj_unref(&f->o);
    return rc;
}

struct lx_dirent64 { uint64_t ino; int64_t off; uint16_t reclen; uint8_t type; char name[]; } __attribute__((packed));

static long lx_getdents(struct task *t, int fd, uint64_t ubuf, uint64_t count)
{
    struct lx_fd *e = fd_get(t->lx, fd);
    if (!e)
        return -E_BADF;
    if (e->kind != LXFD_FILE)
        return -E_NOTDIR;
    if (count < 32)
        return -E_INVAL;
    if (count > 32 * KiB)
        count = 32 * KiB;
    struct file *f;
    long rc = fd_file(t, fd, FILE_READ_DATA, &f);
    if (rc)
        return rc;
    uint8_t *kb = kmalloc(count);
    struct fs_dirent *de = kmalloc(sizeof(*de));
    if (!kb || !de) {
        kfree(kb); kfree(de);
        obj_unref(&f->o);
        return -E_NOMEM;
    }
    size_t pos = 0;
    for (;;) {
        rc = file_readdir(f, e->dir_idx, de);
        if (rc == -E_NOENT) {
            rc = 0;
            break;
        }
        if (rc)
            break;
        size_t nl = strlen(de->name);
        size_t reclen = ALIGN_UP(19 + nl + 1, 8);
        if (pos + reclen > count) {
            if (!pos)
                rc = -E_INVAL;
            break;
        }
        struct lx_dirent64 *d = (struct lx_dirent64 *)(kb + pos);
        memset(d, 0, reclen);
        d->ino = (uint64_t)e->dir_idx + 2;
        d->off = (int64_t)e->dir_idx + 1;
        d->reclen = (uint16_t)reclen;
        d->type = de->type == FT_DIR ? 4 : 8;
        memcpy(d->name, de->name, nl);
        pos += reclen;
        e->dir_idx++;
    }
    obj_unref(&f->o);
    long ret = rc ? rc : (long)pos;
    if (!rc && pos) {
        int c = cout(t, ubuf, kb, pos);
        if (c)
            ret = c;
    }
    kfree(kb);
    kfree(de);
    return ret;
}

static uint64_t rng_next(struct lx *l)
{
    uint64_t x = l->rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    l->rng = x;
    return x * 2685821657736338717ULL;
}

static long lx_getrandom(struct task *t, uint64_t ubuf, uint64_t len)
{
    if (len > 256 * KiB)
        len = 256 * KiB;
    int rc = lx_prefault(t, ubuf, len, true);
    if (rc)
        return rc;
    uint8_t tmp[64];
    uint64_t done = 0;
    while (done < len) {
        size_t n = len - done > sizeof(tmp) ? sizeof(tmp) : (size_t)(len - done);
        for (size_t i = 0; i < n; i += 8) {
            uint64_t r = rng_next(t->lx);
            memcpy(tmp + i, &r, n - i < 8 ? n - i : 8);
        }
        user_copy_out(t, ubuf + done, tmp, n);
        done += n;
    }
    return (long)len;
}

static uint64_t clock_ns_of(struct task *t, uint32_t clk, bool *ok)
{
    (void)t;
    *ok = true;
    switch (clk) {
    case 0:  return wall_ns();
    case 1: case 4: case 7: case 6: case 5: return clock_ns();
    case 2: case 3: {
        struct thread *th = thread_current();
        return th ? thread_cpu_ns(th) : 0;
    }
    default: *ok = false; return 0;
    }
}

static long lx_clock_gettime(struct task *t, uint32_t clk, uint64_t uts)
{
    bool ok;
    uint64_t ns = clock_ns_of(t, clk, &ok);
    if (!ok)
        return -E_INVAL;
    struct lx_timespec ts = { (int64_t)(ns / 1000000000ULL), (int64_t)(ns % 1000000000ULL) };
    return cout(t, uts, &ts, sizeof(ts));
}

static long lx_sleep(struct task *t, uint64_t uts, uint64_t urem, uint32_t clk, bool abs)
{
    struct lx_timespec ts;
    int rc = cin(t, &ts, uts, sizeof(ts));
    if (rc)
        return rc;
    if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000)
        return -E_INVAL;
    uint64_t ns = (uint64_t)ts.sec * 1000000000ULL + (uint64_t)ts.nsec;
    if (abs) {
        bool ok;
        uint64_t now = clock_ns_of(t, clk, &ok);
        ns = ns > now ? ns - now : 0;
    }
    if (ts.sec > 1000000)
        ns = 1000000ULL * 1000000000ULL;
    if (ns)
        thread_sleep_ms((ns + 999999) / 1000000);
    if (urem) {
        struct lx_timespec z = { 0, 0 };
        cout(t, urem, &z, sizeof(z));
    }
    return 0;
}

static long lx_uname(struct task *t, uint64_t ubuf)
{
    char u[6][65];
    memset(u, 0, sizeof(u));
    strcpy_(u[0], "Linux");
    strcpy_(u[1], "infinity");
    strcpy_(u[2], "6.1.0-infinityos");
    strcpy_(u[3], "#1 InfinityOS Linux personality");
    strcpy_(u[4], "x86_64");
    strcpy_(u[5], "(none)");
    return cout(t, ubuf, u, sizeof(u));
}

static long lx_getcwd(struct task *t, uint64_t ubuf, uint64_t size)
{
    char tmp[LX_PATH_MAX + 2];
    size_t k = 0;
    const char *c = t->lx->cwd;
    if (!strcmp(c, "\\")) {
        tmp[k++] = '/';
    } else {
        for (; *c; c++)
            tmp[k++] = *c == '\\' ? '/' : *c;
    }
    tmp[k++] = 0;
    if (k > size)
        return -E_RANGE;
    long rc = cout(t, ubuf, tmp, k);
    return rc ? rc : (long)k;
}

static long lx_chdir(struct task *t, uint64_t upath)
{
    char nt[2 * LX_PATH_MAX + 4];
    long rc = lx_path_nt(t, AT_FDCWD, upath, nt);
    if (rc)
        return rc;
    if (!is_ns_dir(nt)) {
        struct file_stat st;
        rc = fs_k_stat(t, nt, &st);
        if (rc)
            return rc;
        if (st.type != FT_DIR)
            return -E_NOTDIR;
    }
    strcpy_(t->lx->cwd, nt);
    return 0;
}

static long lx_fcntl(struct task *t, int fd, uint32_t cmd, uint64_t arg)
{
    struct lx_fd *e = fd_get(t->lx, fd);
    if (!e)
        return -E_BADF;
    switch (cmd) {
    case 0: case 1030: {
        int nfd = fd_alloc(t->lx, (int)arg);
        if (nfd < 0)
            return nfd;
        return fd_dup_to(t, fd, nfd, cmd == 1030);
    }
    case 1: return e->cloexec;
    case 2: e->cloexec = arg & 1; return 0;
    case 3: {
        long fl = 0;
        if (e->kind == LXFD_CONSOLE)
            return 2;
        struct file *f;
        if (fd_file(t, fd, 0, &f) == 0) {
            bool w = f->access & FILE_WRITE_DATA, r = f->access & FILE_READ_DATA;
            fl = (w && r) ? 2 : w ? 1 : 0;
            obj_unref(&f->o);
        }
        return fl | (e->append ? LX_O_APPEND : 0);
    }
    case 4: e->append = !!(arg & LX_O_APPEND); return 0;
    default: return -E_INVAL;
    }
}

static long lx_ioctl(struct task *t, int fd, uint32_t cmd, uint64_t arg)
{
    struct lx_fd *e = fd_get(t->lx, fd);
    if (!e)
        return -E_BADF;
    if (e->kind != LXFD_CONSOLE)
        return -E_NOTTY;
    switch (cmd) {
    case 0x5401: {
        uint32_t tio[9] = { 0x0500 , 0x0005 , 0x04B2 ,
                            0x8A3B , 0, 0, 0, 0, 0 };
        return cout(t, arg, tio, 36);
    }
    case 0x5413: {
        uint16_t ws[4] = { 25, 80, 0, 0 };
        return cout(t, arg, ws, sizeof(ws));
    }
    case 0x5402: case 0x5403: case 0x5404:
        return 0;
    default:
        return -E_NOTTY;
    }
}

#define LNR_read 0
#define LNR_write 1
#define LNR_open 2
#define LNR_close 3
#define LNR_stat 4
#define LNR_fstat 5
#define LNR_lstat 6
#define LNR_lseek 8
#define LNR_mmap 9
#define LNR_mprotect 10
#define LNR_munmap 11
#define LNR_brk 12
#define LNR_rt_sigaction 13
#define LNR_rt_sigprocmask 14
#define LNR_ioctl 16
#define LNR_pread64 17
#define LNR_pwrite64 18
#define LNR_readv 19
#define LNR_writev 20
#define LNR_access 21
#define LNR_sched_yield 24
#define LNR_mremap 25
#define LNR_madvise 28
#define LNR_dup 32
#define LNR_dup2 33
#define LNR_nanosleep 35
#define LNR_getpid 39
#define LNR_exit 60
#define LNR_kill 62
#define LNR_uname 63
#define LNR_fcntl 72
#define LNR_fsync 74
#define LNR_fdatasync 75
#define LNR_truncate 76
#define LNR_ftruncate 77
#define LNR_getdents 78
#define LNR_getcwd 79
#define LNR_chdir 80
#define LNR_rename 82
#define LNR_mkdir 83
#define LNR_rmdir 84
#define LNR_unlink 87
#define LNR_readlink 89
#define LNR_chmod 90
#define LNR_fchmod 91
#define LNR_chown 92
#define LNR_fchown 93
#define LNR_umask 95
#define LNR_gettimeofday 96
#define LNR_getrlimit 97
#define LNR_getuid 102
#define LNR_getgid 104
#define LNR_geteuid 107
#define LNR_getegid 108
#define LNR_getppid 110
#define LNR_sigaltstack 131
#define LNR_arch_prctl 158
#define LNR_gettid 186
#define LNR_time 201
#define LNR_futex 202
#define LNR_getdents64 217
#define LNR_set_tid_address 218
#define LNR_clock_gettime 228
#define LNR_clock_getres 229
#define LNR_clock_nanosleep 230
#define LNR_exit_group 231
#define LNR_tgkill 234
#define LNR_utimensat 280
#define LNR_openat 257
#define LNR_mkdirat 258
#define LNR_newfstatat 262
#define LNR_unlinkat 263
#define LNR_renameat 264
#define LNR_readlinkat 267
#define LNR_faccessat 269
#define LNR_set_robust_list 273
#define LNR_dup3 292
#define LNR_prlimit64 302
#define LNR_renameat2 316
#define LNR_getrandom 318
#define LNR_faccessat2 439

static const char *sysname_hint(uint64_t nr)
{
    switch (nr) {
    case 56: return "clone"; case 57: return "fork"; case 58: return "vfork"; case 59: return "execve";
    case 61: return "wait4"; case 22: return "pipe"; case 41: return "socket"; case 7: return "poll";
    case 23: return "select"; case 202: return "futex"; case 332: return "statx"; case 334: return "rseq";
    default: return "";
    }
}

static long lx_do(struct task *t, struct regs *r)
{
    struct lx *l = t->lx;
    uint64_t a = r->rdi, b = r->rsi, c = r->rdx, d = r->r10, e5 = r->r8, f6 = r->r9;
    char nt[2 * LX_PATH_MAX + 4], nt2[2 * LX_PATH_MAX + 4];
    long rc;

    switch (r->rax) {
    case LNR_read:   return lx_read(t, (int)a, b, c);
    case LNR_write:  return lx_write(t, (int)a, b, c);
    case LNR_readv:  return lx_vec(t, (int)a, b, c, false);
    case LNR_writev: return lx_vec(t, (int)a, b, c, true);
    case LNR_pread64:  return lx_pio(t, (int)a, b, c, (int64_t)d, false);
    case LNR_pwrite64: return lx_pio(t, (int)a, b, c, (int64_t)d, true);

    case LNR_open:
        rc = lx_path_nt(t, AT_FDCWD, a, nt);
        return rc ? rc : lx_open_nt(t, nt, (uint32_t)b);
    case LNR_openat:
        rc = lx_path_nt(t, (int)(int32_t)a, b, nt);
        return rc ? rc : lx_open_nt(t, nt, (uint32_t)c);
    case LNR_close:  return fd_close(t, (int)a);

    case LNR_stat: case LNR_lstat:
        rc = lx_path_nt(t, AT_FDCWD, a, nt);
        return rc ? rc : lx_stat_nt(t, nt, b);
    case LNR_fstat:  return lx_fstat(t, (int)a, b);
    case LNR_newfstatat: {
        int dirfd = (int)(int32_t)a;
        if ((d & 0x1000) ) {
            char probe[2];
            long n = cstr(t, probe, sizeof(probe), b);
            if (n == 0)
                return lx_fstat(t, dirfd, c);
        }
        rc = lx_path_nt(t, dirfd, b, nt);
        return rc ? rc : lx_stat_nt(t, nt, c);
    }

    case LNR_lseek: {
        struct lx_fd *e = fd_get(l, (int)a);
        if (!e)
            return -E_BADF;
        if (e->kind == LXFD_CONSOLE)
            return -E_SPIPE;
        struct file *f;
        rc = fd_file(t, (int)a, 0, &f);
        if (rc)
            return rc;
        if (c > 2)
            rc = -E_INVAL;
        else {
            rc = file_seek(f, (int64_t)b, (int)c);
            if (rc >= 0 && e->dir_idx && c == SEEK_SET && b == 0)
                e->dir_idx = 0;
        }
        obj_unref(&f->o);
        return rc;
    }

    case LNR_mmap:     return lx_mmap(t, a, b, (uint32_t)c, (uint32_t)d, (int)(int32_t)e5, f6);
    case LNR_munmap:   return lx_munmap(t, a, b);
    case LNR_mprotect: return lx_mprotect(t, a, b, (uint32_t)c);
    case LNR_brk:      return lx_brk(t, a);
    case LNR_madvise:  return lx_madvise(t, a, b, (int)c);
    case LNR_mremap:   return -E_NOMEM;

    case LNR_dup:   { int n = fd_alloc(l, 0); return n < 0 ? n : fd_dup_to(t, (int)a, n, false); }
    case LNR_dup2:  return fd_dup_to(t, (int)a, (int)b, false);
    case LNR_dup3:  return a == b ? -E_INVAL : fd_dup_to(t, (int)a, (int)b, (c & LX_O_CLOEXEC) != 0);
    case LNR_fcntl: return lx_fcntl(t, (int)a, (uint32_t)b, c);
    case LNR_ioctl: return lx_ioctl(t, (int)a, (uint32_t)b, c);

    case LNR_getdents64: case LNR_getdents:
        return r->rax == LNR_getdents64 ? lx_getdents(t, (int)a, b, c) : -E_NOSYS;

    case LNR_fsync: case LNR_fdatasync: {
        struct lx_fd *e = fd_get(l, (int)a);
        if (!e)
            return -E_BADF;
        if (e->kind == LXFD_CONSOLE)
            return -E_INVAL;
        struct file *f;
        rc = fd_file(t, (int)a, 0, &f);
        if (rc)
            return rc;
        rc = vfs_sync(f->vol);
        obj_unref(&f->o);
        return rc;
    }
    case LNR_ftruncate: {
        struct file *f;
        rc = fd_file(t, (int)a, FILE_WRITE_DATA, &f);
        if (rc)
            return rc == -E_INVAL ? -E_INVAL : rc;
        rc = (int64_t)b < 0 ? -E_INVAL : file_setsize(f, b);
        obj_unref(&f->o);
        return rc;
    }
    case LNR_truncate: {
        rc = lx_path_nt(t, AT_FDCWD, a, nt);
        if (rc)
            return rc;
        long fd = lx_open_nt(t, nt, 1);
        if (fd < 0)
            return fd;
        struct file *f;
        rc = fd_file(t, (int)fd, FILE_WRITE_DATA, &f);
        if (!rc) {
            rc = (int64_t)b < 0 ? -E_INVAL : file_setsize(f, b);
            obj_unref(&f->o);
        }
        fd_close(t, (int)fd);
        return rc;
    }

    case LNR_getcwd: return lx_getcwd(t, a, b);
    case LNR_chdir:  return lx_chdir(t, a);

    case LNR_mkdir: case LNR_mkdirat: {
        bool at = r->rax == LNR_mkdirat;
        rc = lx_path_nt(t, at ? (int)(int32_t)a : AT_FDCWD, at ? b : a, nt);
        if (rc)
            return rc;
        return is_ns_dir(nt) ? -E_EXIST : fs_k_mkdir(t, nt);
    }
    case LNR_rmdir: case LNR_unlink: case LNR_unlinkat: {
        bool at = r->rax == LNR_unlinkat;
        bool want_dir = r->rax == LNR_rmdir || (at && (c & 0x200 ));
        rc = lx_path_nt(t, at ? (int)(int32_t)a : AT_FDCWD, at ? b : a, nt);
        if (rc)
            return rc;
        if (is_ns_dir(nt))
            return -E_BUSY;
        struct file_stat st;
        rc = fs_k_stat(t, nt, &st);
        if (rc)
            return rc;
        if (want_dir && st.type != FT_DIR)
            return -E_NOTDIR;
        if (!want_dir && st.type == FT_DIR)
            return -E_ISDIR;
        return fs_k_remove(t, nt);
    }
    case LNR_rename: case LNR_renameat: case LNR_renameat2: {
        bool at = r->rax != LNR_rename;
        if (r->rax == LNR_renameat2 && e5 != 0)
            return -E_INVAL;
        rc = lx_path_nt(t, at ? (int)(int32_t)a : AT_FDCWD, at ? b : a, nt);
        if (rc)
            return rc;
        rc = lx_path_nt(t, at ? (int)(int32_t)c : AT_FDCWD, at ? d : b, nt2);
        if (rc)
            return rc;
        return (is_ns_dir(nt) || is_ns_dir(nt2)) ? -E_BUSY : fs_k_rename(t, nt, nt2);
    }
    case LNR_access: case LNR_faccessat: case LNR_faccessat2: {
        bool at = r->rax != LNR_access;
        rc = lx_path_nt(t, at ? (int)(int32_t)a : AT_FDCWD, at ? b : a, nt);
        if (rc)
            return rc;
        if (is_ns_dir(nt))
            return 0;
        struct file_stat st;
        rc = fs_k_stat(t, nt, &st);
        if (rc)
            return rc;
        uint64_t mode = at ? c : b;
        if ((mode & 2) && (st.attr & FA_RDONLY))
            return -E_ACCES;
        return 0;
    }
    case LNR_readlink: case LNR_readlinkat: {
        bool at = r->rax == LNR_readlinkat;
        rc = lx_path_nt(t, at ? (int)(int32_t)a : AT_FDCWD, at ? b : a, nt);
        if (rc)
            return rc;
        struct file_stat st;
        rc = is_ns_dir(nt) ? 0 : fs_k_stat(t, nt, &st);
        return rc ? rc : -E_INVAL;
    }
    case LNR_chmod: case LNR_fchmod: case LNR_chown: case LNR_fchown: case LNR_utimensat:
        return 0;
    case LNR_umask: return 022;

    case LNR_sched_yield: thread_yield(); return 0;
    case LNR_nanosleep:   return lx_sleep(t, a, b, 1, false);
    case LNR_clock_nanosleep: return lx_sleep(t, c, d, (uint32_t)a, (b & 1) != 0);
    case LNR_clock_gettime: return lx_clock_gettime(t, (uint32_t)a, b);
    case LNR_clock_getres: {
        struct lx_timespec ts = { 0, 1000 };
        return b ? cout(t, b, &ts, sizeof(ts)) : 0;
    }
    case LNR_gettimeofday: {
        if (a) {
            uint64_t ns = wall_ns();
            struct { int64_t sec, usec; } tv = { (int64_t)(ns / 1000000000ULL), (int64_t)(ns % 1000000000ULL / 1000) };
            rc = cout(t, a, &tv, sizeof(tv));
            if (rc)
                return rc;
        }
        return 0;
    }
    case LNR_time: {
        int64_t s = (int64_t)wall_unix();
        if (a) {
            rc = cout(t, a, &s, sizeof(s));
            if (rc)
                return rc;
        }
        return s;
    }

    case LNR_getpid: case LNR_gettid: case LNR_set_tid_address:
        return t->id;
    case LNR_getppid: return 1;
    case LNR_getuid: case LNR_geteuid: case LNR_getgid: case LNR_getegid:
        return uid_of(t);
    case LNR_uname: return lx_uname(t, a);
    case LNR_getrandom: return lx_getrandom(t, a, b);

    case LNR_arch_prctl:
        switch (a) {
        case 0x1002:
            if (b >= USER_TOP)
                return -E_INVAL;
            l->fs_base = b;
            wrmsr(MSR_FS_BASE, b);
            cur_fs = b;
            return 0;
        case 0x1003: {
            uint64_t v = l->fs_base;
            return cout(t, b, &v, sizeof(v));
        }
        default: return -E_INVAL;
        }

    case LNR_rt_sigaction: {
        if (c) {
            uint64_t z[4] = { 0, 0, 0, 0 };
            rc = cout(t, c, z, sizeof(z));
            if (rc)
                return rc;
        }
        return 0;
    }
    case LNR_rt_sigprocmask: {
        if (c) {
            uint64_t z = 0;
            rc = cout(t, c, &z, sizeof(z));
            if (rc)
                return rc;
        }
        return 0;
    }
    case LNR_sigaltstack: return 0;
    case LNR_set_robust_list: return 0;
    case LNR_kill: case LNR_tgkill: {
        uint64_t pid = r->rax == LNR_kill ? a : b;
        uint64_t sig = r->rax == LNR_kill ? b : c;
        if (pid != t->id && pid != 0)
            return -E_SRCH;
        if (sig > 64)
            return -E_INVAL;
        if (sig == 0)
            return 0;
        thread_exit((int)(128 + sig));
    }

    case LNR_futex: {
        uint32_t op = (uint32_t)b & 127;
        if (op == 0 || op == 9) {
            uint32_t cur;
            rc = cin(t, &cur, a, sizeof(cur));
            if (rc)
                return rc;
            if (cur != (uint32_t)c)
                return -E_AGAIN;
            if (d) {
                struct lx_timespec ts;
                rc = cin(t, &ts, d, sizeof(ts));
                if (rc)
                    return rc;
                if (op == 0 && ts.sec >= 0 && ts.nsec >= 0) {
                    thread_sleep_ms((uint64_t)ts.sec * 1000 + (uint64_t)ts.nsec / 1000000 + 1);
                    return -E_TIMEDOUT;
                }
            }
            return -E_DEADLK;
        }
        if (op == 1 || op == 10)
            return 0;
        return -E_NOSYS;
    }

    case LNR_getrlimit: case LNR_prlimit64: {
        bool pr = r->rax == LNR_prlimit64;
        uint64_t res = pr ? b : a, uold = pr ? d : b;
        if (pr && a != 0 && a != t->id)
            return -E_SRCH;
        if (res > 15)
            return -E_INVAL;
        uint64_t lim[2] = { ~0ULL, ~0ULL };
        if (res == 3)  { lim[0] = LX_STACK_MAX; }
        if (res == 7)  { lim[0] = lim[1] = LX_MAX_FD; }
        if (uold) {
            rc = cout(t, uold, lim, sizeof(lim));
            if (rc)
                return rc;
        }
        return 0;
    }

    case LNR_exit: case LNR_exit_group:
        thread_exit((int)(a & 0xFF));
    }

    if (r->rax == 334 || r->rax == 332)
        return -E_NOSYS;
    if (r->rax < 512 && !(l->unsup[r->rax / 64] & (1ULL << (r->rax % 64)))) {
        l->unsup[r->rax / 64] |= 1ULL << (r->rax % 64);
        klog(LOG_WARN, "linux: task '%s' called unimplemented syscall %lu %s-> ENOSYS", t->name, r->rax,
             sysname_hint(r->rax));
    }
    return -E_NOSYS;
}

void linux_syscall(struct regs *r)
{
    struct task *t = task_current();
    r->rax = (uint64_t)lx_do(t, r);
}

void linux_task_free(struct task *t)
{
    if (t->lx) {
        kfree(t->lx);
        t->lx = NULL;
    }
}

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31

static int build_stack(struct task *t, const struct elf_info *ei, const char *const *argv,
                       const char *const *envp, uint64_t *sp_out)
{
    struct lx *l = t->lx;
    size_t argc = 0, envc = 0, strbytes = 0;
    for (; argv && argv[argc]; argc++)
        strbytes += strlen(argv[argc]) + 1;
    for (; envp && envp[envc]; envc++)
        strbytes += strlen(envp[envc]) + 1;
    static const char platform[] = "x86_64";
    const char *execfn = argc ? argv[0] : "";
    strbytes += strlen(execfn) + 1 + sizeof(platform) + 16;
    if (strbytes > LX_ARG_MAX || argc > 4096 || envc > 4096)
        return -E_2BIG;

    enum { NAUX = 18 };
    size_t words = 1 + (argc + 1) + (envc + 1) + 2 * (NAUX + 1);
    size_t total = ALIGN_UP(strbytes, 16) + words * 8 + 16;
    uint64_t str_base = ALIGN_DOWN(LX_STACK_TOP - strbytes, 16);
    uint64_t sp = ALIGN_DOWN(str_base - words * 8, 16);
    uint8_t *kb = kzalloc(LX_STACK_TOP - sp + 16);
    if (!kb)
        return -E_NOMEM;
    uint64_t span = LX_STACK_TOP - sp;
    (void)total;

    size_t soff = (size_t)(str_base - sp);
    uint64_t *argv_va = kmalloc((argc + envc + 2) * sizeof(uint64_t));
    if (!argv_va) {
        kfree(kb);
        return -E_NOMEM;
    }
    uint64_t *env_va = argv_va + argc + 1;
    for (size_t i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        memcpy(kb + soff, argv[i], n);
        argv_va[i] = sp + soff;
        soff += n;
    }
    for (size_t i = 0; i < envc; i++) {
        size_t n = strlen(envp[i]) + 1;
        memcpy(kb + soff, envp[i], n);
        env_va[i] = sp + soff;
        soff += n;
    }
    uint64_t execfn_va = sp + soff;
    memcpy(kb + soff, execfn, strlen(execfn) + 1);
    soff += strlen(execfn) + 1;
    uint64_t plat_va = sp + soff;
    memcpy(kb + soff, platform, sizeof(platform));
    soff += sizeof(platform);
    uint64_t rand_va = sp + soff;
    for (int i = 0; i < 2; i++) {
        uint64_t rv = rng_next(l);
        memcpy(kb + soff + 8 * i, &rv, 8);
    }

    uint32_t a, b, c, dd;
    cpuid(1, &a, &b, &c, &dd);
    uint64_t hwcap = dd;
    uint64_t uid = uid_of(t);

    uint64_t *w = (uint64_t *)kb;
    size_t k = 0;
    w[k++] = argc;
    for (size_t i = 0; i < argc; i++) w[k++] = argv_va[i];
    w[k++] = 0;
    for (size_t i = 0; i < envc; i++) w[k++] = env_va[i];
    w[k++] = 0;
#define AUX(tag, val) do { w[k++] = (tag); w[k++] = (val); } while (0)
    AUX(AT_PHDR, ei->phdr);
    AUX(AT_PHENT, ei->phent);
    AUX(AT_PHNUM, ei->phnum);
    AUX(AT_PAGESZ, PAGE_SIZE);
    AUX(AT_BASE, 0);
    AUX(AT_FLAGS, 0);
    AUX(AT_ENTRY, ei->entry);
    AUX(AT_UID, uid);
    AUX(AT_EUID, uid);
    AUX(AT_GID, uid);
    AUX(AT_EGID, uid);
    AUX(AT_PLATFORM, plat_va);
    AUX(AT_HWCAP, hwcap);
    AUX(AT_CLKTCK, 100);
    AUX(AT_SECURE, 0);
    AUX(AT_RANDOM, rand_va);
    AUX(AT_HWCAP2, 0);
    AUX(AT_EXECFN, execfn_va);
    AUX(AT_NULL, 0);
#undef AUX
    kfree(argv_va);
    if (k > words) {
        kfree(kb);
        return -E_2BIG;
    }

    int rc = 0;
    for (uint64_t pg = ALIGN_DOWN(sp, PAGE_SIZE); pg < LX_STACK_TOP && !rc; pg += PAGE_SIZE)
        if (vmm_as_lookup(t->as, pg, NULL) == VMM_NOT_MAPPED &&
            !task_map_zeroed(t->as, pg, VMM_USER | VMM_WRITE | VMM_NX))
            rc = -E_NOMEM;
    if (!rc)
        rc = user_copy_out(t, sp, kb, span);
    kfree(kb);
    *sp_out = sp;
    return rc;
}

struct task *linux_task_new(const char *name, const void *img, size_t size,
                            const char *const *argv, const char *const *envp, int *err)
{
    int dummy;
    if (!err)
        err = &dummy;
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
    uint32_t fcw = 0x037F, mxcsr = 0x1F80;
    memcpy(lx_fpu(l), &fcw, 4);
    memcpy(lx_fpu(l) + 24, &mxcsr, 4);
    l->fpu_valid = true;
    l->cwd[0] = '\\';
    l->rng = rdtsc() ^ (clock_ns() << 17) ^ 0x9E3779B97F4A7C15ULL;
    if (!l->rng)
        l->rng = 1;
    for (int i = 0; i < 3; i++)
        l->fd[i].kind = LXFD_CONSOLE;

    struct elf_info ei;
    int rc = elf_load(t, img, size, &ei);
    uint64_t sp = 0;
    if (!rc) {
        l->brk_start = l->brk_cur = ei.hi + PAGE_SIZE;
        rc = lx_vma_add(l, LX_STACK_TOP - LX_STACK_MAX, LX_STACK_TOP, LX_PROT_READ | LX_PROT_WRITE, LXV_STACK);
    }
    if (!rc)
        rc = build_stack(t, &ei, argv, envp, &sp);
    if (!rc && !task_launch(t, ei.entry, sp, 0))
        rc = -E_NOMEM;
    if (rc) {
        task_discard(t);
        *err = rc;
        return NULL;
    }
    *err = 0;
    return t;
}

int linux_elf_probe(const void *img, size_t size)
{
    struct task *t = task_alloc("probe");
    if (!t)
        return -E_NOMEM;
    int rc = -E_NOMEM;
    t->lx = kzalloc(sizeof(struct lx));
    if (t->lx) {
        struct elf_info ei;
        rc = elf_load(t, img, size, &ei);
    }
    task_discard(t);
    return rc;
}

#define EXEC_MAX (32 * MiB)

struct task *linux_task_new_from_file(const char *ntpath, const char *const *argv,
                                      const char *const *envp, int *err)
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
    strcpy_(full + sizeof(pre) - 1, vol);
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
    for (const char *q = nm; *q; q++)
        if (*q == '\\')
            nm = q + 1;
    struct task *t = linux_task_new(nm, img, (size_t)st.size, argv, envp, err);
    kfree(img);
    return t;
}
