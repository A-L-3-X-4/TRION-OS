#include "kernel.h"
#include "mm.h"

#define BLK_MAGIC     0xB10CB10Cu
#define HEAP_MIN_GROW (64 * KiB)
#define MIN_SPLIT     16

struct blk {
    uint64_t    size;
    struct blk *prev;
    struct blk *next;
    uint32_t    magic;
    uint32_t    free;
};

static struct blk *head, *tail;
static uint64_t heap_end = KERNEL_HEAP_BASE;

static void unmap_range(uint64_t start, uint64_t end)
{
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t f = vmm_unmap(a);
        if (f)
            pmm_free_frame(f);
    }
}

static struct blk *heap_grow(uint64_t need)
{
    uint64_t bytes = ALIGN_UP(need + sizeof(struct blk), PAGE_SIZE);
    if (bytes < HEAP_MIN_GROW)
        bytes = HEAP_MIN_GROW;
    if (heap_end + bytes > KERNEL_HEAP_BASE + KERNEL_HEAP_MAX)
        return NULL;

    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t f = pmm_alloc_frame();
        if (!f || vmm_map(heap_end + off, f, VMM_WRITE) != VMM_OK) {
            if (f && vmm_translate(heap_end + off) != f)
                pmm_free_frame(f);
            unmap_range(heap_end, heap_end + off);
            return NULL;
        }
    }

    struct blk *b;
    if (tail && tail->free) {
        b = tail;
        b->size += bytes;
    } else {
        b = (struct blk *)heap_end;
        b->size = bytes - sizeof(struct blk);
        b->free = 1;
        b->magic = BLK_MAGIC;
        b->prev = tail;
        b->next = NULL;
        if (tail)
            tail->next = b;
        else
            head = b;
        tail = b;
    }
    heap_end += bytes;
    return b;
}

void heap_init(void)
{
    head = tail = NULL;
    heap_end = KERNEL_HEAP_BASE;
    if (!heap_grow(0))
        kpanic("heap: initial grow failed");
}

static struct blk *find_fit(uint64_t n)
{
    for (struct blk *b = head; b; b = b->next)
        // First fit: walk the list from the head and take the first free block that's big enough, not best fit, not good fit, I don't care
        if (b->free && b->size >= n)
            return b;
    return NULL;
}

static void split(struct blk *b, uint64_t n)
{
    if (b->size < n + sizeof(struct blk) + MIN_SPLIT)
        return;
    struct blk *r = (struct blk *)((uint8_t *)(b + 1) + n);
    r->size = b->size - n - sizeof(struct blk);
    r->free = 1;
    r->magic = BLK_MAGIC;
    r->prev = b;
    r->next = b->next;
    if (b->next)
        b->next->prev = r;
    else
        tail = r;
    b->next = r;
    b->size = n;
}

static void *kmalloc_locked(size_t n)
{
    if (n == 0)
        n = 1;
    if (n > KERNEL_HEAP_MAX)
        return NULL;
    n = ALIGN_UP(n, 16);

    struct blk *b = find_fit(n);
    if (!b) {
        if (!heap_grow(n))
            return NULL;
        b = find_fit(n);
        if (!b)
            return NULL;
    }
    mm_stretch();
    split(b, n);
    b->free = 0;
    return b + 1;
}

void *kmalloc(size_t n)
{
    mm_lock();
    void *p = kmalloc_locked(n);
    mm_unlock();
    return p;
}

void *kzalloc(size_t n)
{
    void *p = kmalloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

static void kfree_locked(void *p)
{
    if (!p)
        return;
    struct blk *b = (struct blk *)p - 1;
    if (b->magic != BLK_MAGIC)
        kpanic("heap: kfree(%p) bad magic (corruption or bad pointer)", p);
    if (b->free)
        kpanic("heap: double free of %p", p);
    b->free = 1;
    mm_stretch();

    if (b->next && b->next->free) {
        struct blk *n = b->next;
        b->size += sizeof(struct blk) + n->size;
        b->next = n->next;
        if (n->next)
            n->next->prev = b;
        else
            tail = b;
        n->magic = 0;
    }
    if (b->prev && b->prev->free) {
        struct blk *pv = b->prev;
        pv->size += sizeof(struct blk) + b->size;
        pv->next = b->next;
        if (b->next)
            b->next->prev = pv;
        else
            tail = pv;
        b->magic = 0;
    }
}

void kfree(void *p)
{
    mm_lock();
    kfree_locked(p);
    mm_unlock();
}

void *krealloc(void *p, size_t n)
{
    if (!p)
        return kmalloc(n);
    if (n == 0) {
        kfree(p);
        return NULL;
    }
    struct blk *b = (struct blk *)p - 1;
    if (b->magic != BLK_MAGIC)
        kpanic("heap: krealloc(%p) bad magic", p);
    if (b->size >= ALIGN_UP(n, 16))
        return p;
    void *q = kmalloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, b->size);
    kfree(p);
    return q;
}

void heap_stats(uint64_t *used, uint64_t *free_bytes, uint64_t *blocks)
{
    mm_lock();
    uint64_t u = 0, f = 0, n = 0;
    for (struct blk *b = head; b; b = b->next) {
        n++;
        if (b->free)
            f += b->size;
        else
            u += b->size;
    }
    if (used) *used = u;
    if (free_bytes) *free_bytes = f;
    if (blocks) *blocks = n;
    mm_unlock();
}

static bool heap_check_locked(void)
{
    uint64_t expect = KERNEL_HEAP_BASE;
    struct blk *prev = NULL;
    for (struct blk *b = head; b; b = b->next) {
        if ((uint64_t)b != expect || b->magic != BLK_MAGIC || b->prev != prev)
            return false;
        if (prev && prev->free && b->free)
            return false;
        expect += sizeof(struct blk) + b->size;
        prev = b;
    }
    return expect == heap_end && prev == tail;
}

bool heap_check(void)
{
    mm_lock();
    bool ok = heap_check_locked();
    mm_unlock();
    return ok;
}
