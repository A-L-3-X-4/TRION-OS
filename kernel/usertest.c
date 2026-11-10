#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "abi.h"
#include "timer.h"

static unsigned n_checks, n_fail;

static void check_(bool ok, const char *expr, int line)
{
    n_checks++;
    if (!ok) {
        n_fail++;
        klog(LOG_FAIL, "usertest.c:%d: %s", line, expr);
    }
}
#define CHECK(c) check_((c), #c, __LINE__)

#define IMG_SIZE ((size_t)(user_tests_end - user_tests_start))
#define FAULT(vec) (-(TASK_FAULT_BASE + (vec)))

static struct task *start(const char *name, uint64_t off, uint64_t arg)
{
    return task_create(name, user_tests_start, IMG_SIZE, off, arg);
}

static int run(const char *name, uint64_t off, uint64_t arg)
{
    struct task *t = start(name, off, arg);
    return t ? task_wait(t) : (int)0x80000000;
}

static void test_vmm_as(void)
{
    uint64_t before = pmm_free_frames();
    uint64_t as = vmm_as_create();
    CHECK(as != 0);

    uint64_t f = pmm_alloc_frame();
    uint64_t va = USER_BASE + 0x2000, fl = 0;
    CHECK(vmm_as_map(as, va, f, VMM_USER | VMM_WRITE | VMM_NX) == VMM_OK);
    CHECK(vmm_as_map(as, va, f, VMM_USER) == VMM_EEXIST);
    CHECK(vmm_as_map(as, 0x100000, f, VMM_USER) == VMM_EINVAL);
    CHECK(vmm_as_map(as, va + PAGE_SIZE, f, VMM_WRITE) == VMM_EINVAL);
    CHECK(vmm_as_lookup(as, va + 0x123, &fl) == f + 0x123);
    CHECK((fl & VMM_USER) && (fl & VMM_WRITE) && (fl & VMM_NX));
    CHECK(vmm_as_lookup(as, va + PAGE_SIZE, NULL) == VMM_NOT_MAPPED);
    CHECK(vmm_as_lookup(as, 0x100000, NULL) == VMM_NOT_MAPPED);
    CHECK(vmm_translate(va) == VMM_NOT_MAPPED);

    vmm_as_destroy(as);
    CHECK(pmm_free_frames() == before);
    klog(LOG_INFO, "usertest: address-space API ok");
}

static void test_basic(void)
{
    CHECK(run("cpl", UT_CPL, 0) == 3);
    CHECK(run("hello", UT_HELLO, 0) == 42);
    CHECK(run("regs", UT_REGS, 0) == 0);
    CHECK(run("badsys", UT_BADSYS, 0) == 0);
    klog(LOG_INFO, "usertest: ring 3, SYSCALL/SYSRET, write, errors ok");
}

static void test_faults(void)
{
    CHECK(run("fault_w", UT_FAULT_W, 0)       == FAULT(14));
    CHECK(run("fault_k", UT_FAULT_K, 0)       == FAULT(14));
    CHECK(run("fault_priv", UT_FAULT_PRIV, 0) == FAULT(13));
    CHECK(run("fpu", UT_FPU, 0)               == FAULT(6));
    CHECK(run("cpl", UT_CPL, 0) == 3);
    klog(LOG_INFO, "usertest: user faults contained");
}

static void test_isolation(void)
{
    struct task *a = start("iso-a", UT_ISO, 111);
    struct task *b = start("iso-b", UT_ISO, 222);
    CHECK(a && b);
    if (!a || !b)
        return;
    uint64_t pa = vmm_as_lookup(a->as, USER_STACK_TOP - 8, NULL);
    uint64_t pb = vmm_as_lookup(b->as, USER_STACK_TOP - 8, NULL);
    CHECK(pa != VMM_NOT_MAPPED && pb != VMM_NOT_MAPPED && pa != pb);
    CHECK(task_wait(a) == 111);
    CHECK(task_wait(b) == 222);
    klog(LOG_INFO, "usertest: two tasks, same virtual stack, no interference");
}

static void test_preempt(void)
{
    struct task *s = start("spin", UT_SPIN, 0);
    CHECK(s != NULL);
    if (!s)
        return;
    struct thread *th = s->main;

    uint64_t worst = 0;
    for (int i = 0; i < 5; i++) {
        uint64_t a = clock_ns();
        thread_sleep_ms(10);
        uint64_t took = clock_ns() - a;
        uint64_t late = took > 10000000ULL ? took - 10000000ULL : 0;
        if (late > worst)
            worst = late;
    }
    CHECK(worst < 10000000ULL);
    uint64_t cpu = thread_cpu_ns(th);
    CHECK(cpu > 20000000ULL);
    CHECK(task_wait(s) == 0);
    klog(LOG_INFO, "usertest: ring-3 spinner preempted, worst wake latency %lu us", worst / 1000);
}

static void test_leaks(void)
{
    for (int i = 0; i < 3; i++)
        run("warm", UT_CPL, 0);

    uint64_t fr = pmm_free_frames();
    unsigned th = sched_thread_count();
    for (int i = 0; i < 30; i++) {
        CHECK(run("leak", UT_CPL, 0) == 3);
        if (i % 5 == 0)
            CHECK(run("leak-f", UT_FAULT_K, 0) == FAULT(14));
    }
    uint64_t now = pmm_free_frames();
    CHECK(now + 2 >= fr);
    CHECK(sched_thread_count() == th);
    CHECK(task_live_count() == 0);
    CHECK(heap_check());
    klog(LOG_INFO, "usertest: 36 tasks created+destroyed, frames %lu -> %lu", fr, now);
}

unsigned usertest_run(void)
{
    n_checks = n_fail = 0;
    klog(LOG_INFO, "usertest: image %lu bytes at %p", (uint64_t)IMG_SIZE, (void *)user_tests_start);

    test_vmm_as();
    test_basic();
    test_faults();
    test_isolation();
    test_preempt();
    test_leaks();

    klog(n_fail ? LOG_FAIL : LOG_OK, "USERTEST %s (%u checks, %u failed)",
         n_fail ? "FAIL" : "PASS", n_checks, n_fail);
    console_set_color(n_fail ? C_LRED : C_LGREEN, C_BLACK);
    kprintf("\nStep 3a (ring 3 + syscalls + tasks) %s.\n",
            n_fail ? "has FAILURES" : "complete");
    console_set_color(C_LGRAY, C_BLACK);
    return n_fail;
}
