#include "kernel.h"
#include "mm.h"
#include "task.h"
#include "linux.h"
#include "win32.h"

// Caps on everything a PE file gets to claim, because the file is attacker controlled and trusting the header is how you end up allocating 4 billion sections
#define PE_MAX_SECTIONS   96
#define PE_IMAGE_MAX      (128 * MiB)
#define PE_MAX_IMPORT_DLL 128
#define PE_MAX_THUNKS     16384
#define PE_RELOC_MAX      (16 * MiB)
#define PE_TLS_MAX        (4 * MiB)

// The PE constants we actually use, written out by hand because dragging winnt.h into a kernel would be a war crime
#define IMAGE_FILE_RELOCS_STRIPPED 0x0001
#define IMAGE_FILE_EXECUTABLE      0x0002
#define IMAGE_FILE_DLL             0x2000
#define IMAGE_FILE_MACHINE_AMD64   0x8664
#define SCN_MEM_DISCARDABLE 0x02000000u
#define SCN_MEM_EXECUTE     0x20000000u
#define SCN_MEM_READ        0x40000000u
#define SCN_MEM_WRITE       0x80000000u
#define DIR_IMPORT 1
#define DIR_BASERELOC 5
#define DIR_TLS 9

// On-disk PE headers, packed because the file format doesn't give a shit about C alignment and the compiler would pad them and break everything
struct pe_coff { uint16_t machine, nsec; uint32_t stamp, symtab, nsyms; uint16_t optsize, chars; } __attribute__((packed));
struct pe_opt {
    uint16_t magic; uint8_t maj, min; uint32_t codesz, initsz, uninitsz, entry, codebase;
    uint64_t imagebase; uint32_t secalign, filealign;
    uint16_t majos, minos, majimg, minimg, majsub, minsub;
    uint32_t win32ver, imgsize, hdrsize, checksum;
    uint16_t subsys, dllchars;
    uint64_t stkres, stkcommit, heapres, heapcommit;
    uint32_t loaderflags, nrva;
} __attribute__((packed));
struct pe_dir { uint32_t rva, size; };
struct pe_sec {
    char name[8]; uint32_t vsize, va, rawsize, rawptr, relocptr, lineptr; uint16_t nreloc, nline; uint32_t chars;
} __attribute__((packed));
struct pe_imp { uint32_t oft, stamp, fwd, name, ft; } __attribute__((packed));
struct pe_tls { uint64_t start, end, index, callbacks; uint32_t zerofill, chars; } __attribute__((packed));

// Everything we know about the image while loading it, parsed once up front so nothing later has to trust raw file bytes again
struct pe_ctx {
    const uint8_t *f;  size_t fsize;
    struct task *t;
    uint64_t base, imgsize;
    uint32_t hdrsize;
    struct pe_dir dir[16];
    unsigned nsec;
    struct pe_sec sec[PE_MAX_SECTIONS];
    uint32_t prot[PE_MAX_SECTIONS];
    bool mapped[PE_MAX_SECTIONS];
};

// Every check in here goes through BAD(), which logs the pe.c line number when this is on, so when an exe gets refused you can see which of the many checks hated it instead of getting a useless -E_NOEXEC
bool pe_verbose;
static int pe_bad(int line)
{
    if (pe_verbose)
        klog(LOG_WARN, "pe: refused at pe.c:%d", line);
    return -E_NOEXEC;
}
#define BAD() pe_bad(__LINE__)

// One bit set and only one, v & (v - 1) clears the lowest bit so if anything is left it wasn't a power of two, and PE alignment fields have to be
static bool pow2(uint32_t v) { return v && !(v & (v - 1)); }

// Turns an RVA (where it lives in memory) into a pointer into the file (where it lives on disk), NULL if the file doesn't back it, and mixing those two up is how you write a loader that works on exactly one exe
static const uint8_t *file_at(const struct pe_ctx *c, uint64_t rva, uint64_t n)
{
    if (rva > c->imgsize || n > c->imgsize - rva)
        return NULL;
    if (rva + n <= c->hdrsize)
        return (rva + n <= c->fsize) ? c->f + rva : NULL;
    for (unsigned i = 0; i < c->nsec; i++) {
        const struct pe_sec *s = &c->sec[i];
        uint64_t len = s->vsize && s->vsize < s->rawsize ? s->vsize : s->rawsize;
        if (rva >= s->va && rva - s->va < len && n <= len - (rva - s->va)) {
            uint64_t off = (uint64_t)s->rawptr + (rva - s->va);
            return off + n <= c->fsize ? c->f + off : NULL;
        }
    }
    return NULL;
}

// Reads a string out of the file one byte at a time because the file doesn't have to terminate it, returns the length or -1 if it runs off the end
static int file_str(const struct pe_ctx *c, uint64_t rva, char *out, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        const uint8_t *p = file_at(c, rva + i, 1);
        if (!p)
            return -1;
        out[i] = (char)*p;
        if (!*p)
            return (int)i;
    }
    return -1;
}

// Finds the physical frame behind a user address so we can poke another task's memory directly (it's all identity mapped so physical is just a pointer), faulting the page in first if it isn't there yet
static uint8_t *frame_ptr(struct task *t, uint64_t va)
{
    uint64_t page = ALIGN_DOWN(va, PAGE_SIZE);
    uint64_t pa = vmm_as_lookup(t->as, page, NULL);
    if (pa == VMM_NOT_MAPPED) {
        if (!lx_fault_in(t, page, 0))
            return NULL;
        pa = vmm_as_lookup(t->as, page, NULL);
        if (pa == VMM_NOT_MAPPED)
            return NULL;
    }
    return (uint8_t *)pa + (va - page);
}

// Copies out of the loaded image one page at a time, because the frames aren't contiguous and one big memcpy would walk straight into somebody else's memory
static int img_rd(struct task *t, uint64_t va, void *dst, size_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint8_t *p = frame_ptr(t, va);
        if (!p)
            return BAD();
        size_t chunk = PAGE_SIZE - (size_t)(va & (PAGE_SIZE - 1));
        if (chunk > n)
            chunk = n;
        memcpy(d, p, chunk);
        d += chunk; va += chunk; n -= chunk;
    }
    return 0;
}

// img_rd but the other way, for patching relocs, imports and TLS straight into the image
static int img_wr(struct task *t, uint64_t va, const void *src, size_t n)
{
    const uint8_t *s = src;
    while (n) {
        uint8_t *p = frame_ptr(t, va);
        if (!p)
            return BAD();
        size_t chunk = PAGE_SIZE - (size_t)(va & (PAGE_SIZE - 1));
        if (chunk > n)
            chunk = n;
        memcpy(p, s, chunk);
        s += chunk; va += chunk; n -= chunk;
    }
    return 0;
}

// Checks the DOS header, PE signature, COFF header, optional header and section table, and only lets in 64-bit x86 console or GUI exes, so no DLLs, no 32-bit, no ARM, no drivers, no "trust me bro"
static int parse_headers(struct pe_ctx *c, const uint8_t *f, size_t size, struct pe_opt *oh, struct pe_coff *ch)
{
    if (size < 64 || f[0] != 'M' || f[1] != 'Z')
        return BAD();
    uint32_t lfanew;
    // e_lfanew at 0x3C says where the real PE header is, because every Windows exe is still secretly a DOS program that prints "This program cannot be run in DOS mode" and quits
    memcpy(&lfanew, f + 0x3C, 4);
    if (lfanew < 64 || lfanew > size || size - lfanew < 4 + sizeof(*ch))
        return BAD();
    if (memcmp(f + lfanew, "PE\0\0", 4))
        return BAD();
    memcpy(ch, f + lfanew + 4, sizeof(*ch));
    // x86_64 executables only, and anything marked as a DLL gets thrown out right here
    if (ch->machine != IMAGE_FILE_MACHINE_AMD64 || !(ch->chars & IMAGE_FILE_EXECUTABLE) ||
        (ch->chars & IMAGE_FILE_DLL))
        return BAD();
    if (ch->nsec == 0 || ch->nsec > PE_MAX_SECTIONS || ch->optsize < sizeof(*oh))
        return BAD();
    uint64_t optoff = (uint64_t)lfanew + 4 + sizeof(*ch);
    if (optoff > size || ch->optsize > size - optoff)
        return BAD();
    memcpy(oh, f + optoff, sizeof(*oh));
    if (oh->magic != 0x20B || oh->nrva > 16 || ch->optsize < sizeof(*oh) + (uint64_t)oh->nrva * 8)
        return BAD();
    for (unsigned i = 0; i < 16; i++)
        c->dir[i] = (struct pe_dir){ 0, 0 };
    if (oh->nrva)
        memcpy(c->dir, f + optoff + sizeof(*oh), (size_t)oh->nrva * 8);

    uint64_t secoff = optoff + ch->optsize;
    if (secoff > size || (uint64_t)ch->nsec * sizeof(struct pe_sec) > size - secoff)
        return BAD();
    c->nsec = ch->nsec;
    memcpy(c->sec, f + secoff, (size_t)ch->nsec * sizeof(struct pe_sec));

    // Sections have to be at least page aligned and file alignment can't be bigger than that, we map whole pages and everything below assumes it
    if (!pow2(oh->secalign) || oh->secalign < PAGE_SIZE || oh->secalign > 0x10000)
        return BAD();
    if (!pow2(oh->filealign) || oh->filealign < 512 || oh->filealign > 0x10000 || oh->filealign > oh->secalign)
        return BAD();
    if (oh->imgsize == 0 || oh->imgsize > PE_IMAGE_MAX || (oh->imgsize & (oh->secalign - 1)))
        return BAD();
    if (oh->hdrsize < secoff + (uint64_t)ch->nsec * sizeof(struct pe_sec) || oh->hdrsize > size ||
        oh->hdrsize > oh->imgsize)
        return BAD();
    if (oh->subsys != 2 && oh->subsys != 3)
        return BAD();
    c->imgsize = oh->imgsize;
    c->hdrsize = oh->hdrsize;
    c->f = f;
    c->fsize = size;
    return 0;
}

// Every section has to be page aligned, in order, not overlapping, and inside both the image and the file, then its flags become VM protections. A section table that lies is the easiest way to make a loader scribble over memory it shouldn't touch
static int check_sections(struct pe_ctx *c, const struct pe_opt *oh)
{
    uint64_t prev_end = ALIGN_UP(c->hdrsize, oh->secalign);
    for (unsigned i = 0; i < c->nsec; i++) {
        struct pe_sec *s = &c->sec[i];
        uint32_t vsize = s->vsize ? s->vsize : s->rawsize;
        if (s->va & (oh->secalign - 1))
            return BAD();
        if (s->va < prev_end)
            return BAD();
        uint64_t end = ALIGN_UP((uint64_t)s->va + vsize, oh->secalign);
        if (end > c->imgsize)
            return BAD();
        if (vsize)
            prev_end = end;
        if (s->rawsize && ((uint64_t)s->rawptr > c->fsize || s->rawsize > c->fsize - s->rawptr))
            return BAD();
        uint32_t ch = s->chars, p = 0;
        if (ch & SCN_MEM_READ)    p |= VM_PROT_R;
        if (ch & SCN_MEM_WRITE)   p |= VM_PROT_W;
        if (ch & SCN_MEM_EXECUTE) p |= VM_PROT_X;
        // x86 can't do write-only pages so anything writable or executable gets read for free
        if (p && !(p & VM_PROT_R))
            p |= VM_PROT_R;
        c->prot[i] = p;
        // Discardable and empty sections (debug info and shit) live in the file but never in memory, so they don't get mapped
        c->mapped[i] = vsize && !(ch & SCN_MEM_DISCARDABLE);
    }
    return 0;
}

// Is this RVA inside a section that's mapped and executable, so the entry point and TLS callbacks have to point at real code and not, say, .data
static bool exec_rva(const struct pe_ctx *c, uint64_t rva)
{
    for (unsigned i = 0; i < c->nsec; i++) {
        const struct pe_sec *s = &c->sec[i];
        uint32_t vsize = s->vsize ? s->vsize : s->rawsize;
        if (c->mapped[i] && (c->prot[i] & VM_PROT_X) && rva >= s->va && rva - s->va < vsize)
            return true;
    }
    return false;
}

// Our runtime and import stubs live in this range, so an exe that wanted to be loaded on top of it gets moved instead
static bool overlaps_reserved(uint64_t base, uint64_t size)
{
    return base < W32_STUB_BASE + W32_STUB_PAGES * PAGE_SIZE && base + size > W32_RT_BASE;
}

// Writes the mapping into the task's Linux VMA list, because yes, Windows exes run on top of the Linux personality's memory tracking and mmap/mprotect need to know they exist. Neighbors get merged so one chatty exe doesn't fill the 128 entry table
static int add_image_vma(struct task *t, uint64_t s, uint64_t e, uint32_t prot, uint64_t abase)
{
    struct lx *l = t->lx;
    if (l->nvma) {
        struct lx_vma *p = &l->vma[l->nvma - 1];
        if (p->kind == LXV_WIMAGE && p->abase == abase && p->prot == prot && p->end == s) {
            p->end = e;
            return 0;
        }
    }
    int rc = lx_vma_add(l, s, e, prot, LXV_WIMAGE);
    if (rc)
        return rc;
    l->vma[l->nvma - 1].abase = abase;
    l->vma[l->nvma - 1].aprot = prot;
    return 0;
}

// Backs a range with zeroed pages, and if we run out of memory halfway the ones already mapped just stay mapped, there's no undo in here
static int map_page_range(struct task *t, uint64_t s, uint64_t e, uint32_t prot)
{
    uint64_t fl = lx_prot_to_vmm(prot);
    for (uint64_t a = s; a < e; a += PAGE_SIZE)
        if (!task_map_zeroed(t->as, a, fl))
            return -E_NOMEM;
    return 0;
}

// Headers go in read-only at the base, then each section at base + its RVA, copying min(rawsize, vsize) bytes from the file and leaving the rest zero, which is all .bss is: a section that's a promise to be zeros
static int map_image(struct pe_ctx *c)
{
    struct task *t = c->t;
    uint64_t hdr_end = c->base + ALIGN_UP(c->hdrsize, PAGE_SIZE);
    int rc = add_image_vma(t, c->base, hdr_end, VM_PROT_R, c->base);
    if (!rc)
        rc = map_page_range(t, c->base, hdr_end, VM_PROT_R);
    if (rc)
        return rc;
    memcpy((void *)vmm_as_lookup(t->as, c->base, NULL), c->f, c->hdrsize < PAGE_SIZE ? c->hdrsize : PAGE_SIZE);
    for (uint64_t off = PAGE_SIZE; off < c->hdrsize; off += PAGE_SIZE)
        memcpy((void *)vmm_as_lookup(t->as, c->base + off, NULL), c->f + off,
               c->hdrsize - off < PAGE_SIZE ? c->hdrsize - off : PAGE_SIZE);

    for (unsigned i = 0; i < c->nsec && !rc; i++) {
        if (!c->mapped[i])
            continue;
        const struct pe_sec *s = &c->sec[i];
        uint32_t vsize = s->vsize ? s->vsize : s->rawsize;
        uint64_t va = c->base + s->va, end = va + ALIGN_UP(vsize, PAGE_SIZE);
        uint64_t copy = s->rawsize < vsize ? s->rawsize : vsize;
        rc = add_image_vma(t, va, end, c->prot[i], c->base);
        if (rc)
            break;
        rc = map_page_range(t, va, va + ALIGN_UP(copy, PAGE_SIZE), c->prot[i]);
        for (uint64_t done = 0; !rc && done < copy;) {
            uint64_t pa = vmm_as_lookup(t->as, va + done, NULL);
            size_t chunk = PAGE_SIZE - (size_t)((va + done) & (PAGE_SIZE - 1));
            if (chunk > copy - done)
                chunk = (size_t)(copy - done);
            memcpy((void *)(pa + ((va + done) & (PAGE_SIZE - 1))), c->f + s->rawptr + done, chunk);
            done += chunk;
        }
    }
    return rc;
}

// Only runs if the exe couldn't load at the address it wanted. Adds the load delta to every fixup in .reloc, and only HIGHLOW and DIR64 are allowed, anything else and the whole load gets refused
static int apply_relocs(struct pe_ctx *c, uint64_t imagebase)
{
    uint64_t delta = c->base - imagebase;
    uint32_t rva = c->dir[DIR_BASERELOC].rva, total = c->dir[DIR_BASERELOC].size;
    if (!total || total > PE_RELOC_MAX)
        return BAD();
    uint64_t pos = 0;
    while (pos < total) {
        if (total - pos < 8)
            return BAD();
        const uint8_t *b = file_at(c, (uint64_t)rva + pos, 8);
        if (!b)
            return BAD();
        uint32_t page, bsize;
        memcpy(&page, b, 4);
        memcpy(&bsize, b + 4, 4);
        if (bsize < 8 || (bsize & 1) || bsize > total - pos || page >= c->imgsize)
            return BAD();
        const uint8_t *e = file_at(c, (uint64_t)rva + pos + 8, bsize - 8);
        if (bsize > 8 && !e)
            return BAD();
        for (uint32_t i = 0; i < (bsize - 8) / 2; i++) {
            uint16_t ent;
            memcpy(&ent, e + 2 * i, 2);
            uint32_t type = ent >> 12, off = ent & 0xFFF;
            uint64_t va = c->base + page + off;
            // Type 0 is padding to keep the blocks 4-byte aligned, it does nothing
            if (type == 0)
                continue;
            if (type == 10) {
                uint64_t v;
                if (img_rd(c->t, va, &v, 8))
                    return BAD();
                v += delta;
                if (img_wr(c->t, va, &v, 8))
                    return BAD();
            } else if (type == 3) {
                uint32_t v;
                if (img_rd(c->t, va, &v, 4))
                    return BAD();
                v += (uint32_t)delta;
                if (img_wr(c->t, va, &v, 4))
                    return BAD();
            } else {
                return BAD();
            }
        }
        pos += bsize;
    }
    return 0;
}

// Fills every IAT slot with an address from w32_bind_import, by name or ordinal. There is no real kernel32.dll anywhere in here, they all point into our own runtime, and whatever it doesn't implement gets a stub that complains when it's called
static int bind_imports(struct pe_ctx *c, struct w32_load *ctx)
{
    uint32_t rva = c->dir[DIR_IMPORT].rva, size = c->dir[DIR_IMPORT].size;
    if (!rva || !size)
        return 0;
    for (unsigned d = 0;; d++) {
        if (d >= PE_MAX_IMPORT_DLL)
            return BAD();
        const uint8_t *p = file_at(c, (uint64_t)rva + (uint64_t)d * sizeof(struct pe_imp), sizeof(struct pe_imp));
        if (!p)
            return BAD();
        struct pe_imp im;
        memcpy(&im, p, sizeof(im));
        if (!im.name && !im.oft && !im.ft)
            break;
        char dll[80];
        if (!im.name || !im.ft || file_str(c, im.name, dll, sizeof(dll)) <= 0)
            return BAD();
        // The OFT is the pristine name list and the IAT is the copy we overwrite, and some linkers skip the OFT so fall back to the IAT
        uint32_t lookup = im.oft ? im.oft : im.ft;
        for (unsigned i = 0;; i++) {
            if (i >= PE_MAX_THUNKS)
                return BAD();
            const uint8_t *tp = file_at(c, (uint64_t)lookup + 8ull * i, 8);
            if (!tp)
                return BAD();
            uint64_t th;
            memcpy(&th, tp, 8);
            if (!th)
                break;
            uint64_t addr = 0;
            int rc;
            // Top bit set means import by ordinal, otherwise it's an RVA to a hint/name entry, and the +2 below skips the hint, which is a guess at the export index that we ignore
            if (th >> 63) {
                rc = w32_bind_import(ctx, dll, NULL, (uint32_t)(th & 0xFFFF), &addr);
            } else {
                char name[128];
                if (th > 0x7FFFFFFF || file_str(c, th + 2, name, sizeof(name)) <= 0)
                    return BAD();
                rc = w32_bind_import(ctx, dll, name, 0, &addr);
            }
            if (rc)
                return rc;
            if (img_wr(c->t, c->base + im.ft + 8ull * i, &addr, 8))
                return BAD();
        }
    }
    return 0;
}

// Copies the .tls template into a fresh block with room for the zero-fill, zeroes the TLS index, and collects the callback list, anything with a thread_local variable ends up here
static int setup_tls(struct pe_ctx *c, struct pe_info *out)
{
    out->tls_block = 0;
    out->ntls = 0;
    out->tls_cb[0] = 0;
    if (c->dir[DIR_TLS].size < sizeof(struct pe_tls))
        return 0;
    struct pe_tls tl;
    if (c->dir[DIR_TLS].rva > c->imgsize || img_rd(c->t, c->base + c->dir[DIR_TLS].rva, &tl, sizeof(tl)))
        return BAD();
    uint64_t lo = c->base, hi = c->base + c->imgsize;
    if (tl.start > tl.end || tl.start < lo || tl.end > hi || tl.index < lo || tl.index > hi - 4)
        return BAD();
    uint64_t rawlen = tl.end - tl.start;
    if (rawlen > PE_TLS_MAX || tl.zerofill > PE_TLS_MAX)
        return BAD();
    uint64_t total = ALIGN_UP(rawlen + tl.zerofill, 16);

    if (tl.callbacks) {
        if (tl.callbacks < lo || tl.callbacks > hi - 8)
            return BAD();
        unsigned n = 0;
        for (;; n++) {
            if (n > W32_MAX_TLS_CB)
                return BAD();
            uint64_t cb;
            if (img_rd(c->t, tl.callbacks + 8ull * n, &cb, 8))
                return BAD();
            if (!cb)
                break;
            // TLS callbacks run before the entry point, so each one has to land in executable code inside the image, otherwise a malicious exe gets to start running wherever it feels like
            if (cb < lo || cb >= hi || !exec_rva(c, cb - lo))
                return BAD();
            out->tls_cb[n] = cb;
        }
        out->tls_cb[n] = 0;
        out->ntls = n;
    }
    long blk = wvm_alloc(c->t, 0, total ? total : PAGE_SIZE, VM_RESERVE | VM_COMMIT | VM_PROT_R | VM_PROT_W);
    if (blk < 0)
        return (int)blk;
    uint8_t *tmp = rawlen ? kmalloc((size_t)rawlen) : NULL;
    if (rawlen && !tmp)
        return -E_NOMEM;
    int rc = rawlen ? img_rd(c->t, tl.start, tmp, (size_t)rawlen) : 0;
    if (!rc && rawlen)
        rc = img_wr(c->t, (uint64_t)blk, tmp, (size_t)rawlen);
    kfree(tmp);
    uint32_t zero = 0;
    if (!rc)
        rc = img_wr(c->t, tl.index, &zero, 4);
    if (rc)
        return rc;
    out->tls_block = (uint64_t)blk;
    return 0;
}

// The whole loader: validate, pick a base, map, relocate if needed, bind imports, set up TLS, and every failure jumps to out: so the context gets freed exactly once
int pe_load(struct task *t, struct w32_load *ctx, const void *img, size_t size, struct pe_info *out)
{
    struct pe_ctx *c = kzalloc(sizeof(*c));
    if (!c)
        return -E_NOMEM;
    struct pe_opt oh;
    struct pe_coff ch;
    int rc = parse_headers(c, img, size, &oh, &ch);
    if (!rc)
        rc = check_sections(c, &oh);
    if (rc)
        goto out;
    c->t = t;

    if (!oh.entry || !exec_rva(c, oh.entry)) {
        rc = BAD();
        goto out;
    }

    // Load at the address the exe asked for if it's sane, 64K aligned, in the allowed window, not on top of our runtime and not already taken, otherwise move it to W32_IMAGE_BASE and hope it kept its relocations
    bool fits = oh.imagebase >= USER_BASE + PAGE_SIZE && !(oh.imagebase & 0xFFFF) &&
                oh.imagebase <= LX_MMAP_TOP - 64 * MiB &&
                c->imgsize <= LX_MMAP_TOP - 64 * MiB - oh.imagebase &&
                !overlaps_reserved(oh.imagebase, c->imgsize) &&
                !lx_vma_busy(t->lx, oh.imagebase, oh.imagebase + c->imgsize);
    bool reloc = !fits;
    c->base = fits ? oh.imagebase : W32_IMAGE_BASE;
    // No relocations means it can't move, so if its address is taken we have to refuse, the linker's way of saying this address or nothing
    if (reloc && (ch.chars & IMAGE_FILE_RELOCS_STRIPPED)) {
        rc = BAD();
        goto out;
    }
    if (lx_vma_busy(t->lx, c->base, c->base + c->imgsize)) {
        rc = BAD();
        goto out;
    }

    rc = map_image(c);
    if (!rc && reloc && c->dir[DIR_BASERELOC].size)
        rc = apply_relocs(c, oh.imagebase);
    if (!rc)
        rc = bind_imports(c, ctx);
    if (!rc)
        rc = setup_tls(c, out);
    if (rc)
        goto out;

    out->base = c->base;
    out->size = c->imgsize;
    out->entry = c->base + oh.entry;
    out->subsystem = oh.subsys;
    out->stack_reserve = oh.stkres;
    out->stack_commit = oh.stkcommit;
    out->relocated = reloc;
// One exit so the context gets freed on every path, including the failed ones
out:
    kfree(c);
    return rc;
}
