#include "kernel.h"
#include "mm.h"
#include "multiboot2.h"

extern char kernel_start[], kernel_end[];

#define PMM_LIMIT (4ULL * GiB)

struct range {
    uint64_t start, end;
};

static uint64_t *bitmap;
static uint64_t total_frames;
static uint64_t free_count;
static uint64_t bitmap_words;
static uint64_t search_hint;

// This is the ENTIRE physical memory allocator, one bit per 4 KiB frame where 1 means used, and that's seriously all there is to it
static inline bool bm_test(uint64_t f)  { return bitmap[f >> 6] & (1ULL << (f & 63)); }
static inline void bm_set(uint64_t f)   { bitmap[f >> 6] |= (1ULL << (f & 63)); }
static inline void bm_clear(uint64_t f) { bitmap[f >> 6] &= ~(1ULL << (f & 63)); }

static void mark_used(uint64_t start, uint64_t end)
{
    uint64_t f0 = start / PAGE_SIZE;
    uint64_t f1 = (end + PAGE_SIZE - 1) / PAGE_SIZE;
    if (f1 > total_frames)
        f1 = total_frames;
    for (uint64_t f = f0; f < f1; f++) {
        if (!bm_test(f)) {
            bm_set(f);
            free_count--;
        }
    }
}

static void mark_free(uint64_t start, uint64_t end)
{
    uint64_t f0 = (start + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t f1 = end / PAGE_SIZE;
    if (f1 > total_frames)
        f1 = total_frames;
    for (uint64_t f = f0; f < f1; f++) {
        if (bm_test(f)) {
            bm_clear(f);
            free_count++;
        }
    }
}

static uint64_t find_bitmap_home(const struct mb2_tag_mmap *mm, uint64_t size,
                                 const struct range *res, int nres)
{
    MB2_FOR_EACH_MMAP(mm, e) {
        if (e->type != MB2_MEM_AVAILABLE)
            continue;
        uint64_t s = e->addr > 1 * MiB ? e->addr : 1 * MiB;
        uint64_t end = e->addr + e->len;
        if (end > PMM_LIMIT)
            end = PMM_LIMIT;
        uint64_t cand = ALIGN_UP(s, PAGE_SIZE);
        bool moved;
        do {
            moved = false;
            for (int i = 0; i < nres; i++) {
                if (cand < res[i].end && cand + size > res[i].start) {
                    cand = ALIGN_UP(res[i].end, PAGE_SIZE);
                    moved = true;
                }
            }
        } while (moved);
        if (cand + size <= end)
            return cand;
    }
    return 0;
}

void pmm_init(uint64_t mbi_addr)
{
    const struct mb2_tag_mmap *mm =
        (const struct mb2_tag_mmap *)mb2_find_tag(mbi_addr, MB2_TAG_MMAP);
    if (!mm)
        kpanic("pmm: bootloader gave no memory map");

    uint64_t mbi_size = mb2_total_size(mbi_addr);

    uint64_t top = 0;
    MB2_FOR_EACH_MMAP(mm, e) {
        if (e->type != MB2_MEM_AVAILABLE)
            continue;
        uint64_t end = e->addr + e->len;
        if (end > PMM_LIMIT)
            end = PMM_LIMIT;
        if (end > top)
            top = end;
    }
    if (top == 0)
        kpanic("pmm: no usable RAM below 4 GiB");

    total_frames = top / PAGE_SIZE;
    bitmap_words = (total_frames + 63) / 64;
    uint64_t bitmap_bytes = bitmap_words * 8;

    struct range res[2] = {
        { (uint64_t)kernel_start, (uint64_t)kernel_end },
        { mbi_addr, mbi_addr + mbi_size },
    };
    uint64_t home = find_bitmap_home(mm, bitmap_bytes, res, 2);
    if (!home)
        kpanic("pmm: nowhere to put the frame bitmap");

    bitmap = (uint64_t *)home;
    memset(bitmap, 0xFF, bitmap_bytes);
    free_count = 0;

    MB2_FOR_EACH_MMAP(mm, e) {
        if (e->type == MB2_MEM_AVAILABLE)
            mark_free(e->addr, e->addr + e->len);
    }

    mark_used(0, 1 * MiB);
    mark_used((uint64_t)kernel_start, (uint64_t)kernel_end);
    mark_used(home, home + bitmap_bytes);
    mark_used(mbi_addr, mbi_addr + mbi_size);
}

uint64_t pmm_alloc_frame(void)
{
    mm_lock();
    uint64_t r = 0;
    for (uint64_t n = 0; n < bitmap_words; n++) {
        uint64_t w = search_hint + n;
        if (w >= bitmap_words)
            w -= bitmap_words;
        uint64_t v = bitmap[w];
        if (v != ~0ULL) {
            int b = __builtin_ctzll(~v);
            mm_stretch();
            bitmap[w] |= 1ULL << b;
            free_count--;
            search_hint = w;
            r = (w * 64 + (uint64_t)b) * PAGE_SIZE;
            break;
        }
    }
    mm_unlock();
    return r;
}

void pmm_free_frame(uint64_t phys)
{
    if (phys & (PAGE_SIZE - 1))
        kpanic("pmm: free of unaligned address %p", (void *)phys);
    uint64_t f = phys / PAGE_SIZE;
    mm_lock();
    if (f >= total_frames)
        kpanic("pmm: free of out-of-range frame %p", (void *)phys);
    if (!bm_test(f))
        kpanic("pmm: double free of frame %p", (void *)phys);
    bm_clear(f);
    mm_stretch();
    free_count++;
    if ((f >> 6) < search_hint)
        search_hint = f >> 6;
    mm_unlock();
}

uint64_t pmm_total_frames(void) { return total_frames; }
uint64_t pmm_free_frames(void)  { return free_count; }
