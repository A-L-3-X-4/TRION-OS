#include "kernel.h"
#include "mm.h"
#include "task.h"
#include "linux.h"

#define EI_NIDENT 16
struct elf64_ehdr {
    uint8_t  ident[EI_NIDENT];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

#define ET_EXEC 2
#define ET_DYN  3
#define EM_X86_64 62
#define PT_LOAD   1
#define PT_INTERP 3
#define PT_PHDR   6
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define ELF_PHNUM_MAX   64
#define ELF_IMAGE_MAX   (128 * MiB)
#define ELF_LOAD_MAX    32

static uint32_t pf_to_prot(uint32_t pf)
{
    return ((pf & PF_R) ? LX_PROT_READ : 0) | ((pf & PF_W) ? LX_PROT_WRITE : 0) |
           ((pf & PF_X) ? LX_PROT_EXEC : 0);
}

uint64_t lx_prot_to_vmm(uint32_t prot);
int      lx_vma_add(struct lx *l, uint64_t start, uint64_t end, uint32_t prot, uint32_t kind);

int elf_load(struct task *t, const void *img, size_t size, struct elf_info *out)
{
    const uint8_t *base = img;
    struct elf64_ehdr eh;
    if (size < sizeof(eh))
        return -E_NOEXEC;
    memcpy(&eh, base, sizeof(eh));

    if (eh.ident[0] != 0x7F || eh.ident[1] != 'E' || eh.ident[2] != 'L' || eh.ident[3] != 'F')
        return -E_NOEXEC;
    if (eh.ident[4] != 2 || eh.ident[5] != 1 || eh.ident[6] != 1)
        return -E_NOEXEC;
    if (eh.machine != EM_X86_64 || (eh.type != ET_EXEC && eh.type != ET_DYN))
        return -E_NOEXEC;
    if (eh.phentsize != sizeof(struct elf64_phdr) || eh.phnum == 0 || eh.phnum > ELF_PHNUM_MAX)
        return -E_NOEXEC;
    uint64_t phbytes = (uint64_t)eh.phnum * sizeof(struct elf64_phdr);
    if (eh.phoff > size || phbytes > size - eh.phoff)
        return -E_NOEXEC;

    struct elf64_phdr ph[ELF_PHNUM_MAX];
    memcpy(ph, base + eh.phoff, phbytes);

    bool pie = eh.type == ET_DYN;
    uint64_t bias = pie ? LX_PIE_BASE : 0;

    unsigned nload = 0;
    uint64_t lo = ~0ULL, hi = 0, total = 0;
    for (unsigned i = 0; i < eh.phnum; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->type == PT_INTERP)
            return -E_NOEXEC;
        if (p->type != PT_LOAD)
            continue;
        if (++nload > ELF_LOAD_MAX)
            return -E_NOEXEC;
        if (p->filesz > p->memsz)
            return -E_NOEXEC;
        if (p->offset > size || p->filesz > size - p->offset)
            return -E_NOEXEC;
        if ((p->vaddr & (PAGE_SIZE - 1)) != (p->offset & (PAGE_SIZE - 1)))
            return -E_NOEXEC;
        if (p->memsz == 0)
            continue;
        if (p->vaddr > ~0ULL - bias || p->memsz > ~0ULL - bias - p->vaddr - PAGE_SIZE)
            return -E_NOEXEC;
        uint64_t s = ALIGN_DOWN(p->vaddr + bias, PAGE_SIZE);
        uint64_t e = ALIGN_UP(p->vaddr + bias + p->memsz, PAGE_SIZE);
        if (s < USER_BASE + PAGE_SIZE || e > LX_MMAP_TOP - 64 * MiB || e <= s)
            return -E_NOEXEC;
        total += e - s;
        if (total > ELF_IMAGE_MAX)
            return -E_NOEXEC;
        if (s < lo) lo = s;
        if (e > hi) hi = e;
    }
    if (!nload || lo >= hi)
        return -E_NOEXEC;
    if (eh.entry > ~0ULL - bias)
        return -E_NOEXEC;
    uint64_t entry = eh.entry + bias;

    bool entry_ok = false;
    for (unsigned i = 0; i < eh.phnum; i++)
        if (ph[i].type == PT_LOAD && (ph[i].flags & PF_X) && entry >= ph[i].vaddr + bias &&
            entry - (ph[i].vaddr + bias) < ph[i].memsz)
            entry_ok = true;
    if (!entry_ok)
        return -E_NOEXEC;

    uint64_t phdr_va = 0;
    for (unsigned i = 0; i < eh.phnum; i++)
        if (ph[i].type == PT_PHDR)
            phdr_va = ph[i].vaddr + bias;
    if (!phdr_va)
        for (unsigned i = 0; i < eh.phnum; i++) {
            const struct elf64_phdr *p = &ph[i];
            if (p->type == PT_LOAD && eh.phoff >= p->offset && eh.phoff - p->offset + phbytes <= p->filesz) {
                phdr_va = p->vaddr + bias + (eh.phoff - p->offset);
                break;
            }
        }
    if (!phdr_va)
        return -E_NOEXEC;

    int rc = 0;
    for (unsigned i = 0; i < eh.phnum && !rc; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->type != PT_LOAD || p->memsz == 0)
            continue;
        uint64_t s = ALIGN_DOWN(p->vaddr + bias, PAGE_SIZE);
        uint64_t e = ALIGN_UP(p->vaddr + bias + p->memsz, PAGE_SIZE);
        uint32_t prot = pf_to_prot(p->flags);
        if (!(prot & LX_PROT_READ) && prot)
            prot |= LX_PROT_READ;
        uint64_t want = lx_prot_to_vmm(prot);

        for (uint64_t va = s; va < e; va += PAGE_SIZE) {
            uint64_t fl, pa = vmm_as_lookup(t->as, va, &fl);
            if (pa == VMM_NOT_MAPPED) {
                if (!task_map_zeroed(t->as, va, want)) {
                    rc = -E_NOMEM;
                    break;
                }
            } else {
                uint64_t nf = (fl & ~VMM_NX) | (want & ~VMM_NX) | ((fl & VMM_NX) && (want & VMM_NX) ? VMM_NX : 0);
                vmm_as_protect(t->as, va, nf | VMM_USER);
            }
        }
        if (rc)
            break;
        uint64_t va = p->vaddr + bias, n = p->filesz, off = p->offset;
        while (n) {
            uint64_t pa = vmm_as_lookup(t->as, va, NULL);
            size_t chunk = PAGE_SIZE - (size_t)(va & (PAGE_SIZE - 1));
            if (chunk > n)
                chunk = (size_t)n;
            memcpy((void *)pa, base + off, chunk);
            va += chunk; off += chunk; n -= chunk;
        }
        rc = lx_vma_add(t->lx, s, e, prot, LXV_IMAGE);
    }
    if (rc)
        return rc;

    out->entry = entry;
    out->phdr = phdr_va;
    out->phnum = eh.phnum;
    out->phent = sizeof(struct elf64_phdr);
    out->lo = lo;
    out->hi = hi;
    out->bias = bias;
    out->pie = pie;
    return 0;
}
