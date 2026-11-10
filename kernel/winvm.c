#include "kernel.h"
#include "mm.h"
#include "task.h"
#include "linux.h"
#include "win32.h"

// Top of the allocatable window rounded down to 64K, because Windows allocations are 64K aligned and some programs notice
#define MMAP_TOP  ALIGN_DOWN(LX_MMAP_TOP, VM_GRANULARITY)

// Gives back the Linux-side state only for Win32 tasks and NULL for everything else, so everything in here refuses to run on Linux processes
static struct lx *wl(struct task *t)
{
    return t && t->lx && t->lx->flavor == LXF_WIN32 ? t->lx : NULL;
}

// Reserved or committed, the two kinds VirtualAlloc itself makes
static bool alloc_kind(uint32_t k) { return k == LXV_WRESERVE || k == LXV_WCOMMIT; }
// Regions that count as real memory, the exe's own image included
static bool committed_kind(uint32_t k) { return k == LXV_WCOMMIT || k == LXV_WIMAGE; }

// Keeps the R/W/X bits and adds R to write-only or exec-only combos, x86 has no write-only pages and nobody asked it to
static uint32_t norm_prot(uint32_t prot)
{
    prot &= 7;
    return prot && !(prot & VM_PROT_R) ? prot | VM_PROT_R : prot;
}

// Cuts a region in two at address a if a lands inside one, because you can commit or protect half a reservation and that needs two entries. Fails if the 128 entry VMA table is full
static int split_at(struct lx *l, uint64_t a)
{
    for (unsigned i = 0; i < l->nvma; i++) {
        struct lx_vma *v = &l->vma[i];
        if (v->start < a && v->end > a) {
            if (l->nvma >= LX_MAX_VMA)
                return -E_NOMEM;
            l->vma[l->nvma++] = (struct lx_vma){ a, v->end, v->prot, v->kind, v->abase, v->aprot, 0 };
            v->end = a;
            return 0;
        }
    }
    return 0;
}

// Glues identical neighbors back together after every change so the table doesn't shatter into a hundred pieces, it's an O(n^2) loop that repeats until nothing merges and n is at most 128 so who gives a shit
static void coalesce(struct lx *l)
{
    for (bool again = true; again;) {
        again = false;
        for (unsigned i = 0; i < l->nvma && !again; i++)
            for (unsigned j = 0; j < l->nvma && !again; j++) {
                struct lx_vma *a = &l->vma[i], *b = &l->vma[j];
                if (i != j && alloc_kind(a->kind) && a->kind == b->kind && a->prot == b->prot &&
                    a->abase == b->abase && a->aprot == b->aprot && a->end == b->start) {
                    a->end = b->end;
                    l->vma[j] = l->vma[--l->nvma];
                    again = true;
                }
            }
    }
}

// Walks a range region by region and checks every piece is an allowed kind with no holes, which is how we refuse to commit or protect memory that was never reserved
static bool range_ok(struct lx *l, uint64_t s, uint64_t e, bool (*ok)(uint32_t))
{
    for (uint64_t a = s; a < e;) {
        struct lx_vma *v = lx_vma_lookup(l, a);
        if (!v || !ok(v->kind))
            return false;
        a = v->end;
    }
    return true;
}

// Finds free address space top down, starts at the top of the window and slides down past anything in the way until a 64K aligned hole fits, never below the heap break, 0 if there's nowhere
static uint64_t find_gap(struct lx *l, uint64_t len)
{
    uint64_t lower = ALIGN_UP(l->brk_cur, VM_GRANULARITY);
    uint64_t top = MMAP_TOP;
    for (;;) {
        if (top < lower + len)
            return 0;
        uint64_t s = ALIGN_DOWN(top - len, VM_GRANULARITY);
        if (s < lower)
            return 0;
        uint64_t hit = 0;
        bool any = false;
        for (unsigned i = 0; i < l->nvma; i++)
            if (l->vma[i].start < s + len && l->vma[i].end > s && (!any || l->vma[i].start > hit)) {
                hit = l->vma[i].start;
                any = true;
            }
        if (!any)
            return s;
        top = ALIGN_DOWN(hit, VM_GRANULARITY);
    }
}

// Pushes the new protections into the real page tables a page at a time
static void reprotect(struct task *t, uint64_t s, uint64_t e, uint32_t prot)
{
    uint64_t fl = lx_prot_to_vmm(prot);
    for (uint64_t a = s; a < e; a += PAGE_SIZE)
        vmm_as_protect(t->as, a, fl);
}

// VirtualAlloc. RESERVE claims address space with nothing behind it (and picks a gap if you don't give an address), COMMIT turns reserved space into usable memory, and both together does it in one go. Returns the base or a negative error
long wvm_alloc(struct task *t, uint64_t addr, uint64_t size, uint32_t flags)
{
    struct lx *l = wl(t);
    if (!l)
        return -E_NOSYS;
    if (!size || size > USER_TOP - USER_BASE)
        return size ? -E_NOMEM : -E_INVAL;
    uint32_t prot = norm_prot(flags);
    if (!(flags & (VM_RESERVE | VM_COMMIT)) || (flags & ~(7u | VM_RESERVE | VM_COMMIT)))
        return -E_INVAL;

    // Reserve path: a fixed address gets checked against the user window and anything already there, otherwise find_gap picks one, then the region just gets written down, nothing is mapped here
    if (flags & VM_RESERVE) {
        uint64_t base, end;
        if (addr) {
            if (addr >= USER_TOP || size > USER_TOP - addr)
                return -E_INVAL;
            base = ALIGN_DOWN(addr, VM_GRANULARITY);
            end = ALIGN_UP(addr + size, PAGE_SIZE);
            if (base < USER_BASE + PAGE_SIZE || end > USER_TOP - PAGE_SIZE)
                return -E_INVAL;
            if (lx_vma_busy(l, base, end))
                return -E_EXIST;
        } else {
            uint64_t len = ALIGN_UP(size, PAGE_SIZE);
            base = find_gap(l, len);
            if (!base)
                return -E_NOMEM;
            end = base + len;
        }
        if (l->nvma >= LX_MAX_VMA)
            return -E_NOMEM;
        bool commit = flags & VM_COMMIT;
        l->vma[l->nvma++] = (struct lx_vma){ base, end, commit ? prot : 0,
                                             commit ? LXV_WCOMMIT : LXV_WRESERVE, base, prot, 0 };
        coalesce(l);
        return (long)base;
    }

    // Commit only: the range has to already be inside a reservation, and all this does is flip it to committed and apply the protection
    if (!addr || addr >= USER_TOP || size > USER_TOP - addr)
        return -E_INVAL;
    uint64_t s = ALIGN_DOWN(addr, PAGE_SIZE), e = ALIGN_UP(addr + size, PAGE_SIZE);
    if (!range_ok(l, s, e, alloc_kind))
        return -E_INVAL;
    if (split_at(l, s) || split_at(l, e))
        return -E_NOMEM;
    for (unsigned i = 0; i < l->nvma; i++) {
        struct lx_vma *v = &l->vma[i];
        if (v->start >= s && v->end <= e) {
            v->kind = LXV_WCOMMIT;
            v->prot = prot;
            reprotect(t, v->start, v->end, prot);
        }
    }
    coalesce(l);
    return (long)s;
}

// VirtualFree. RELEASE gives back the whole original reservation (size has to be 0 and the address has to be the exact base, Windows insists) and DECOMMIT unmaps the pages but keeps the address space
long wvm_free(struct task *t, uint64_t addr, uint64_t size, uint32_t flags)
{
    struct lx *l = wl(t);
    if (!l)
        return -E_NOSYS;
    if (addr < USER_BASE || addr >= USER_TOP)
        return -E_INVAL;

    if (flags == VM_RELEASE) {
        if (size)
            return -E_INVAL;
        struct lx_vma *v = lx_vma_lookup(l, addr);
        if (!v || !alloc_kind(v->kind) || v->abase != addr)
            return -E_INVAL;
        // Cuts out every region belonging to this allocation, restarting after each cut because the cut changes the table under us
        for (bool again = true; again;) {
            again = false;
            for (unsigned i = 0; i < l->nvma; i++)
                if (l->vma[i].abase == addr && alloc_kind(l->vma[i].kind)) {
                    uint64_t s = l->vma[i].start, e = l->vma[i].end;
                    int rc = lx_vma_cut(t, s, e);
                    if (rc)
                        return rc;
                    again = true;
                    break;
                }
        }
        return 0;
    }

    if (flags == VM_DECOMMIT) {
        if (!size || size > USER_TOP - addr)
            return -E_INVAL;
        uint64_t s = ALIGN_DOWN(addr, PAGE_SIZE), e = ALIGN_UP(addr + size, PAGE_SIZE);
        if (!range_ok(l, s, e, alloc_kind))
            return -E_INVAL;
        if (split_at(l, s) || split_at(l, e))
            return -E_NOMEM;
        for (unsigned i = 0; i < l->nvma; i++) {
            struct lx_vma *v = &l->vma[i];
            if (v->start >= s && v->end <= e) {
                for (uint64_t a = v->start; a < v->end; a += PAGE_SIZE)
                    vmm_as_unmap(t->as, a);
                v->kind = LXV_WRESERVE;
                v->prot = 0;
            }
        }
        coalesce(l);
        return 0;
    }
    return -E_INVAL;
}

// VirtualProtect. Committed memory only, and it returns the OLD protection like Windows does so programs can put it back later
long wvm_protect(struct task *t, uint64_t addr, uint64_t size, uint32_t prot)
{
    struct lx *l = wl(t);
    if (!l)
        return -E_NOSYS;
    if (!size || addr < USER_BASE || addr >= USER_TOP || size > USER_TOP - addr || (prot & ~7u))
        return -E_INVAL;
    prot = norm_prot(prot);
    uint64_t s = ALIGN_DOWN(addr, PAGE_SIZE), e = ALIGN_UP(addr + size, PAGE_SIZE);
    if (!range_ok(l, s, e, committed_kind))
        return -E_INVAL;
    long old = (long)(lx_vma_lookup(l, s)->prot & 7);
    if (split_at(l, s) || split_at(l, e))
        return -E_NOMEM;
    for (unsigned i = 0; i < l->nvma; i++) {
        struct lx_vma *v = &l->vma[i];
        if (v->start >= s && v->end <= e) {
            v->prot = prot;
            reprotect(t, v->start, v->end, prot);
        }
    }
    coalesce(l);
    return old;
}

// Same allocation, kind and protection? wvm_query uses it to report neighbors as one big block
static bool same_region(const struct lx_vma *a, const struct lx_vma *b)
{
    uint64_t ab = a->abase ? a->abase : a->start, bb = b->abase ? b->abase : b->start;
    return a->kind == b->kind && (a->prot & 7) == (b->prot & 7) && ab == bb;
}

// VirtualQuery. Reports base, size, state, protection and image-or-private for the region around an address, merging identical neighbors. Below USER_BASE it says "reserved" so nothing tries to allocate the null page area
int wvm_query(struct task *t, uint64_t addr, struct vm_info *out)
{
    struct lx *l = wl(t);
    if (!l)
        return -E_NOSYS;
    if (addr >= USER_TOP)
        return -E_INVAL;
    uint64_t a = ALIGN_DOWN(addr, PAGE_SIZE);
    memset(out, 0, sizeof(*out));
    out->base = a;
    struct lx_vma *v = lx_vma_lookup(l, a);
    if (!v) {
        if (a < USER_BASE) {
            out->size = USER_BASE - a;
            out->state = VM_ST_RESERVED;
            out->type = VM_TY_PRIVATE;
            return 0;
        }
        uint64_t next = USER_TOP;
        for (unsigned i = 0; i < l->nvma; i++)
            if (l->vma[i].start > a && l->vma[i].start < next)
                next = l->vma[i].start;
        out->size = next - a;
        out->state = VM_ST_FREE;
        return 0;
    }
    uint64_t hi = v->end;
    for (bool again = true; again;) {
        again = false;
        for (unsigned i = 0; i < l->nvma; i++)
            if (l->vma[i].start == hi && same_region(v, &l->vma[i])) {
                hi = l->vma[i].end;
                again = true;
                break;
            }
    }
    out->size = hi - a;
    out->abase = v->abase ? v->abase : v->start;
    out->aprot = v->aprot ? v->aprot : v->prot & 7;
    out->prot = v->kind == LXV_WRESERVE ? 0 : v->prot & 7;
    out->state = v->kind == LXV_WRESERVE ? VM_ST_RESERVED : VM_ST_COMMITTED;
    out->type = (v->kind == LXV_WIMAGE || v->kind == LXV_IMAGE) ? VM_TY_IMAGE : VM_TY_PRIVATE;
    return 0;
}

// Syscall front door, Win32 tasks only, routes ALLOC/FREE/PROTECT/QUERY to the functions above, and QUERY checks the user's output buffer is writable before doing any work
long sys_vm_dispatch(struct task *t, uint64_t nr, uint64_t a, uint64_t b, uint64_t c)
{
    if (!wl(t))
        return -E_NOSYS;
    switch (nr) {
    case SYS_VM_ALLOC:   return wvm_alloc(t, a, b, (uint32_t)c);
    case SYS_VM_FREE:    return wvm_free(t, a, b, (uint32_t)c);
    case SYS_VM_PROTECT: return wvm_protect(t, a, b, (uint32_t)c);
    case SYS_VM_QUERY: {
        struct vm_info vi;
        int rc = user_check_out(t, b, sizeof(vi));
        if (rc)
            return rc;
        rc = wvm_query(t, a, &vi);
        return rc ? rc : user_copy_out(t, b, &vi, sizeof(vi));
    }
    default:             return -E_NOSYS;
    }
}
