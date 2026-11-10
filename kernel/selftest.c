#include "kernel.h"
#include "mm.h"

static unsigned n_checks, n_fail;

static void check_(bool ok, const char *expr, int line)
{
    n_checks++;
    if (!ok) {
        n_fail++;
        klog(LOG_FAIL, "selftest.c:%d: %s", line, expr);
    }
}
#define CHECK(c) check_((c), #c, __LINE__)

static bool filled_with(const void *p, uint8_t v, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        if (b[i] != v)
            return false;
    return true;
}

static void test_pmm(void)
{
    enum { N = 512 };
    static uint64_t frames[N];
    uint64_t before = pmm_free_frames();

    for (int i = 0; i < N; i++) {
        frames[i] = pmm_alloc_frame();
        CHECK(frames[i] != 0);
        CHECK((frames[i] & (PAGE_SIZE - 1)) == 0);
    }
    bool unique = true;
    for (int i = 0; i < N && unique; i++)
        for (int j = i + 1; j < N; j++)
            if (frames[i] == frames[j]) { unique = false; break; }
    CHECK(unique);
    CHECK(pmm_free_frames() == before - N);

    for (int i = 0; i < N; i++)
        pmm_free_frame(frames[i]);
    CHECK(pmm_free_frames() == before);

    klog(LOG_INFO, "pmm: %d frames allocated, unique, freed, counts match", N);
}

static void test_vmm(void)
{
    CHECK(vmm_translate(0xB8000) == 0xB8000);
    CHECK(vmm_translate(0x00123456) == 0x00123456);

    uint64_t va = 0x0000005000000000ULL;
    uint64_t f = pmm_alloc_frame();
    CHECK(f != 0);
    CHECK(vmm_map(va, f, VMM_WRITE) == VMM_OK);
    CHECK(vmm_translate(va + 0x123) == f + 0x123);
    *(volatile uint64_t *)va = 0xDEADBEEFCAFEF00DULL;
    CHECK(*(volatile uint64_t *)f == 0xDEADBEEFCAFEF00DULL);
    CHECK(vmm_map(va, f, VMM_WRITE) == VMM_EEXIST);
    CHECK(vmm_map(va + 1, f, VMM_WRITE) == VMM_EINVAL);
    CHECK(vmm_unmap(va) == f);
    CHECK(vmm_translate(va) == VMM_NOT_MAPPED);
    CHECK(vmm_unmap(va) == 0);
    pmm_free_frame(f);

    uint64_t base = va + 0x200000 - 4 * PAGE_SIZE;
    uint64_t fr[8];
    for (int i = 0; i < 8; i++) {
        fr[i] = pmm_alloc_frame();
        CHECK(vmm_map(base + i * PAGE_SIZE, fr[i], VMM_WRITE) == VMM_OK);
        memset((void *)(base + i * PAGE_SIZE), 0x40 + i, PAGE_SIZE);
    }
    bool ok = true;
    for (int i = 0; i < 8; i++) {
        ok &= vmm_translate(base + i * PAGE_SIZE) == fr[i];
        ok &= filled_with((void *)fr[i], (uint8_t)(0x40 + i), PAGE_SIZE);
    }
    CHECK(ok);
    for (int i = 0; i < 8; i++) {
        vmm_unmap(base + i * PAGE_SIZE);
        pmm_free_frame(fr[i]);
    }

    klog(LOG_INFO, "vmm: identity map ok, map/translate/unmap ok, NX %s",
         vmm_nx_enabled() ? "on" : "unsupported");
}

static void test_heap(void)
{
    enum { N = 128 };
    static void *p[N];
    static size_t sz[N];
    uint64_t used, freeb, blocks;

    CHECK(heap_check());

    for (int i = 0; i < N; i++) {
        sz[i] = 8 + (size_t)(i * 37) % 3000;
        p[i] = kmalloc(sz[i]);
        CHECK(p[i] != NULL);
        CHECK(((uint64_t)p[i] & 15) == 0);
        memset(p[i], i, sz[i]);
    }
    CHECK(heap_check());

    for (int i = 1; i < N; i += 2)
        kfree(p[i]);
    CHECK(heap_check());

    static void *q[64];
    for (int i = 0; i < 64; i++) {
        q[i] = kmalloc(24);
        CHECK(q[i] != NULL);
        memset(q[i], 0xEE, 24);
    }
    bool intact = true;
    for (int i = 0; i < N; i += 2)
        intact &= filled_with(p[i], (uint8_t)i, sz[i]);
    CHECK(intact);

    for (int i = 0; i < 64; i++) kfree(q[i]);
    for (int i = 0; i < N; i += 2) kfree(p[i]);
    CHECK(heap_check());
    heap_stats(&used, &freeb, &blocks);
    CHECK(used == 0);
    CHECK(blocks == 1);

    uint64_t free_before = pmm_free_frames();
    size_t big = 4 * MiB;
    uint8_t *b = kmalloc(big);
    CHECK(b != NULL);
    for (size_t i = 0; i < big; i += 4096)
        b[i] = (uint8_t)(i >> 12);
    bool big_ok = true;
    for (size_t i = 0; i < big; i += 4096)
        big_ok &= b[i] == (uint8_t)(i >> 12);
    CHECK(big_ok);
    CHECK(pmm_free_frames() < free_before);
    kfree(b);
    CHECK(heap_check());
    heap_stats(&used, &freeb, &blocks);
    CHECK(used == 0 && blocks == 1);

    void *d = kmalloc(1000);
    memset(d, 0xAA, 1000);
    kfree(d);
    uint8_t *z = kzalloc(1000);
    CHECK(z && filled_with(z, 0, 1000));
    kfree(z);

    uint8_t *r = kmalloc(100);
    memset(r, 7, 100);
    r = krealloc(r, 5000);
    CHECK(r && filled_with(r, 7, 100));
    CHECK(krealloc(r, 0) == NULL);
    CHECK(heap_check());

    heap_stats(&used, &freeb, &blocks);
    klog(LOG_INFO, "heap: alloc/free/coalesce/grow/realloc ok (%lu KiB mapped, %lu block)",
         (uint64_t)(used + freeb) / 1024, blocks);
}

unsigned selftest_run(void)
{
    n_checks = n_fail = 0;
    test_pmm();
    test_vmm();
    test_heap();
    klog(n_fail ? LOG_FAIL : LOG_OK, "SELFTEST %s (%u checks, %u failed)",
         n_fail ? "FAIL" : "PASS", n_checks, n_fail);
    return n_fail;
}
