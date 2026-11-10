#include "kernel.h"
#include "io.h"
#include "mm.h"
#include "abi.h"

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_ADDR_1G   0x000FFFFFC0000000ULL
#define PTE_ADDR_2M   0x000FFFFFFFE00000ULL
#define LEAF_FLAGS    (VMM_WRITE | VMM_USER | VMM_PWT | VMM_PCD | VMM_NX)

#define IDX4(v) (((v) >> 39) & 0x1FF)
#define IDX3(v) (((v) >> 30) & 0x1FF)
#define IDX2(v) (((v) >> 21) & 0x1FF)
#define IDX1(v) (((v) >> 12) & 0x1FF)

static uint64_t *pml4;
static bool nx_ok;

void vmm_init(void)
{
    pml4 = (uint64_t *)(read_cr3() & PTE_ADDR_MASK);

    uint32_t a, b, c, d;
    cpuid(0x80000001, &a, &b, &c, &d);
    if (d & (1u << 20)) {
        wrmsr(0xC0000080, rdmsr(0xC0000080) | (1ULL << 11));
        nx_ok = true;
    }
}

bool vmm_nx_enabled(void) { return nx_ok; }

static uint64_t *next_table(uint64_t *tbl, uint64_t idx, bool create, uint64_t flags)
{
    uint64_t e = tbl[idx];
    if (e & VMM_PRESENT) {
        if (e & VMM_HUGE)
            return NULL;
        if ((flags & VMM_USER) && !(e & VMM_USER))
            tbl[idx] = e | VMM_USER;
        return (uint64_t *)(e & PTE_ADDR_MASK);
    }
    if (!create)
        return NULL;
    uint64_t f = pmm_alloc_frame();
    if (!f)
        return NULL;
    memset((void *)f, 0, PAGE_SIZE);
    tbl[idx] = f | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
    return (uint64_t *)f;
}

static int map_in(uint64_t *root, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if ((virt | phys) & (PAGE_SIZE - 1))
        return VMM_EINVAL;

    uint64_t leaf = flags & LEAF_FLAGS;
    if (!nx_ok)
        leaf &= ~VMM_NX;

    uint64_t *pdpt = next_table(root, IDX4(virt), true, leaf);
    if (!pdpt) return VMM_ENOMEM;
    uint64_t *pd = next_table(pdpt, IDX3(virt), true, leaf);
    if (!pd) return VMM_ENOMEM;
    uint64_t *pt = next_table(pd, IDX2(virt), true, leaf);
    if (!pt) return VMM_ENOMEM;

    uint64_t *e = &pt[IDX1(virt)];
    if (*e & VMM_PRESENT)
        return VMM_EEXIST;
    *e = phys | leaf | VMM_PRESENT;
    invlpg(virt);
    return VMM_OK;
}

static uint64_t vmm_unmap_locked(uint64_t virt)
{
    uint64_t *pdpt = next_table(pml4, IDX4(virt), false, 0);
    if (!pdpt) return 0;
    uint64_t *pd = next_table(pdpt, IDX3(virt), false, 0);
    if (!pd) return 0;
    uint64_t *pt = next_table(pd, IDX2(virt), false, 0);
    if (!pt) return 0;

    uint64_t e = pt[IDX1(virt)];
    if (!(e & VMM_PRESENT))
        return 0;
    pt[IDX1(virt)] = 0;
    invlpg(virt);
    return e & PTE_ADDR_MASK;
}

static uint64_t lookup_in(uint64_t *root, uint64_t virt, uint64_t *flags)
{
    uint64_t e = root[IDX4(virt)];
    if (!(e & VMM_PRESENT)) return VMM_NOT_MAPPED;

    uint64_t *pdpt = (uint64_t *)(e & PTE_ADDR_MASK);
    e = pdpt[IDX3(virt)];
    if (!(e & VMM_PRESENT)) return VMM_NOT_MAPPED;
    if (e & VMM_HUGE) {
        if (flags) *flags = e & LEAF_FLAGS;
        return (e & PTE_ADDR_1G) | (virt & 0x3FFFFFFFULL);
    }

    uint64_t *pd = (uint64_t *)(e & PTE_ADDR_MASK);
    e = pd[IDX2(virt)];
    if (!(e & VMM_PRESENT)) return VMM_NOT_MAPPED;
    if (e & VMM_HUGE) {
        if (flags) *flags = e & LEAF_FLAGS;
        return (e & PTE_ADDR_2M) | (virt & 0x1FFFFFULL);
    }

    uint64_t *pt = (uint64_t *)(e & PTE_ADDR_MASK);
    e = pt[IDX1(virt)];
    if (!(e & VMM_PRESENT)) return VMM_NOT_MAPPED;
    if (flags) *flags = e & LEAF_FLAGS;
    return (e & PTE_ADDR_MASK) | (virt & 0xFFFULL);
}

static uint64_t vmm_translate_locked(uint64_t virt)
{
    return lookup_in(pml4, virt, NULL);
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (IDX4(virt) != 0)
        return VMM_EINVAL;
    mm_lock();
    int r = map_in(pml4, virt, phys, flags);
    mm_unlock();
    return r;
}

uint64_t vmm_unmap(uint64_t virt)
{
    mm_lock();
    uint64_t r = vmm_unmap_locked(virt);
    mm_unlock();
    return r;
}

uint64_t vmm_translate(uint64_t virt)
{
    mm_lock();
    uint64_t r = vmm_translate_locked(virt);
    mm_unlock();
    return r;
}

uint64_t vmm_as_create(void)
{
    mm_lock();
    uint64_t f = pmm_alloc_frame();
    if (f) {
        memset((void *)f, 0, PAGE_SIZE);
        ((uint64_t *)f)[0] = pml4[0];
    }
    mm_unlock();
    return f;
}

int vmm_as_map(uint64_t as, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!(flags & VMM_USER) || virt < USER_BASE || virt >= USER_TOP)
        return VMM_EINVAL;
    mm_lock();
    int r = map_in((uint64_t *)as, virt, phys, flags);
    mm_unlock();
    return r;
}

uint64_t vmm_as_lookup(uint64_t as, uint64_t virt, uint64_t *flags)
{
    if (virt < USER_BASE || virt >= USER_TOP)
        return VMM_NOT_MAPPED;
    mm_lock();
    uint64_t r = lookup_in((uint64_t *)as, virt, flags);
    mm_unlock();
    return r;
}

static uint64_t *pte_in(uint64_t *root, uint64_t virt)
{
    uint64_t *pdpt = next_table(root, IDX4(virt), false, 0);
    if (!pdpt) return NULL;
    uint64_t *pd = next_table(pdpt, IDX3(virt), false, 0);
    if (!pd) return NULL;
    uint64_t *pt = next_table(pd, IDX2(virt), false, 0);
    if (!pt) return NULL;
    return &pt[IDX1(virt)];
}

int vmm_as_unmap(uint64_t as, uint64_t virt)
{
    if (virt < USER_BASE || virt >= USER_TOP)
        return 0;
    mm_lock();
    uint64_t *e = pte_in((uint64_t *)as, virt);
    int r = 0;
    if (e && (*e & VMM_PRESENT)) {
        uint64_t f = *e & PTE_ADDR_MASK;
        *e = 0;
        invlpg(virt);
        pmm_free_frame(f);
        r = 1;
    }
    mm_unlock();
    return r;
}

int vmm_as_protect(uint64_t as, uint64_t virt, uint64_t flags)
{
    if (virt < USER_BASE || virt >= USER_TOP)
        return -1;
    mm_lock();
    uint64_t *e = pte_in((uint64_t *)as, virt);
    int r = -1;
    if (e && (*e & VMM_PRESENT)) {
        uint64_t leaf = flags & LEAF_FLAGS;
        if (!nx_ok)
            leaf &= ~VMM_NX;
        *e = (*e & PTE_ADDR_MASK) | leaf | VMM_PRESENT;
        invlpg(virt);
        r = 0;
    }
    mm_unlock();
    return r;
}

void vmm_as_activate(uint64_t as)
{
    if ((read_cr3() & PTE_ADDR_MASK) != as)
        write_cr3(as);
}

static void free_tree(uint64_t table, int level)
{
    uint64_t *t = (uint64_t *)table;
    for (int i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & VMM_PRESENT) || (e & VMM_HUGE))
            continue;
        if (level > 1)
            free_tree(e & PTE_ADDR_MASK, level - 1);
        else
            pmm_free_frame(e & PTE_ADDR_MASK);
    }
    pmm_free_frame(table);
}

void vmm_as_destroy(uint64_t as)
{
    mm_lock();
    if ((read_cr3() & PTE_ADDR_MASK) == as)
        write_cr3((uint64_t)pml4);
    uint64_t *root = (uint64_t *)as;
    for (int i = 1; i < 512; i++)
        if (root[i] & VMM_PRESENT)
            free_tree(root[i] & PTE_ADDR_MASK, 3);
    pmm_free_frame(as);
    mm_unlock();
}
