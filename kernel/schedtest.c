#include "kernel.h"
#include "sched.h"
#include "vfs.h"
#include "ipc.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"
#include "task.h"
#include "mm.h"
#include "timer.h"
#include "irq.h"
#include "linux.h"
#include "win32.h"
#include "drv.h"

static unsigned n_checks, n_fail;

static void check_(bool ok, const char *expr, int line)
{
    n_checks++;
    if (!ok) {
        n_fail++;
        klog(LOG_FAIL, "schedtest.c:%d: %s", line, expr);
    }
}
#define CHECK(c) check_((c), #c, __LINE__)

#define MS(n) ((uint64_t)(n) * 1000000ULL)

static uint64_t uarg(void *p) { return (uint64_t)p; }

static unsigned pct10(uint64_t part, uint64_t whole)
{
    return whole ? (unsigned)(part * 1000 / whole) : 0;
}

static bool wait_state(struct thread *t, enum thread_state s, unsigned timeout_ms)
{
    for (unsigned i = 0; i < timeout_ms; i++) {
        if (*(volatile enum thread_state *)&t->state == s)
            return true;
        thread_sleep_ms(1);
    }
    return false;
}

static int w_return(void *arg) { return (int)uarg(arg) + 1; }

static volatile bool spin_stop;
static int w_hog(void *arg)
{
    (void)arg;
    volatile uint64_t c = 0;
    while (!spin_stop)
        c++;
    return 0;
}

static struct sem park_sem;
static int w_park(void *arg) { (void)arg; sem_down(&park_sem); return 7; }

static volatile unsigned yield_count[2];
static int w_yielder(void *arg)
{
    unsigned id = (unsigned)uarg(arg);
    for (int i = 0; i < 1000; i++) {
        yield_count[id]++;
        thread_yield();
    }
    return (int)yield_count[id];
}

static void test_basic(void)
{
    struct thread *t = thread_create("t-basic", w_return, (void *)41, 0);
    CHECK(t != NULL);
    CHECK(t->tid > 0);
    CHECK(strcmp(t->name, "t-basic") == 0);
    CHECK(thread_join(t) == 42);

    int sum = 0;
    struct thread *ts[20];
    for (int i = 0; i < 20; i++)
        ts[i] = thread_create("t-many", w_return, (void *)(uint64_t)i, 0);
    for (int i = 0; i < 20; i++)
        sum += thread_join(ts[i]);
    CHECK(sum == 20 * 21 / 2);

    struct thread *a = thread_create("yield-a", w_yielder, (void *)0, 0);
    struct thread *b = thread_create("yield-b", w_yielder, (void *)1, 0);
    CHECK(thread_join(a) == 1000);
    CHECK(thread_join(b) == 1000);

    sem_init(&park_sem, 0);
    t = thread_create("t-stack", w_park, NULL, 0);
    uint64_t lo, hi, guard;
    CHECK(thread_stack_range(t, &lo, &hi, &guard));
    CHECK(hi - lo == KSTACK_SIZE);
    CHECK(vmm_translate(guard) == VMM_NOT_MAPPED);
    CHECK(vmm_translate(guard + PAGE_SIZE - 1) == VMM_NOT_MAPPED);
    CHECK(vmm_translate(lo) != VMM_NOT_MAPPED);
    CHECK(vmm_translate(hi - 1) != VMM_NOT_MAPPED);
    CHECK(vmm_translate(hi) == VMM_NOT_MAPPED);
    CHECK(wait_state(t, T_BLOCKED, 200));
    sem_up(&park_sem);
    CHECK(thread_join(t) == 7);

    klog(LOG_INFO, "threads: create/join/exit codes, yield, guard-paged stacks ok");
}

static void test_timer(void)
{
    uint64_t irq0 = irq_count(IRQ_TIMER);
    uint64_t t0 = clock_ns();
    thread_sleep_ms(50);
    uint64_t dt = clock_ns() - t0;
    uint64_t irqs = irq_count(IRQ_TIMER) - irq0;
    CHECK(dt >= MS(49) && dt < MS(75));
    CHECK(irqs >= 40 && irqs <= 60);

    t0 = clock_ns();
    thread_sleep_ms(1);
    CHECK(clock_ns() - t0 >= MS(1) - 100000);

    klog(LOG_INFO, "timer: sleep(50ms) took %lu us, %lu IRQ0 in that window, TSC %lu MHz%s",
         dt / 1000, irqs, timer_tsc_khz() / 1000,
         timer_tsc_calibrated() ? "" : " (uncalibrated: tick clock)");
}

static struct sem fifo_sem;
static volatile int fifo_order[5];
static volatile int fifo_n;
static int w_fifo(void *arg)
{
    sem_down(&fifo_sem);
    fifo_order[fifo_n++] = (int)uarg(arg);
    return 0;
}

static struct sem helper_sem;
static int w_post_later(void *arg)
{
    thread_sleep_ms((uint64_t)uarg(arg));
    sem_up(&helper_sem);
    return 0;
}

enum { RING = 8, PER_PRODUCER = 500 };
static struct {
    int      buf[RING];
    unsigned head, tail;
    struct sem empty, full, lock;
} pc;

static int w_producer(void *arg)
{
    int id = (int)uarg(arg);
    for (int i = 0; i < PER_PRODUCER; i++) {
        sem_down(&pc.empty);
        sem_down(&pc.lock);
        pc.buf[pc.head++ % RING] = id * 1000 + i + 1;
        sem_up(&pc.lock);
        sem_up(&pc.full);
    }
    return 0;
}

static int w_consumer(void *arg)
{
    (void)arg;
    int sum = 0;
    for (int i = 0; i < PER_PRODUCER; i++) {
        sem_down(&pc.full);
        sem_down(&pc.lock);
        int v = pc.buf[pc.tail++ % RING];
        sem_up(&pc.lock);
        sem_up(&pc.empty);
        sum += v;
    }
    return sum;
}

static void test_sem(void)
{
    sem_init(&fifo_sem, 0);
    fifo_n = 0;
    struct thread *w[5];
    for (int i = 0; i < 5; i++) {
        w[i] = thread_create("fifo", w_fifo, (void *)(uint64_t)i, 0);
        CHECK(wait_state(w[i], T_BLOCKED, 200));
    }
    for (int i = 0; i < 5; i++) {
        sem_up(&fifo_sem);
        thread_sleep_ms(5);
        CHECK(fifo_n == i + 1);
    }
    bool in_order = true;
    for (int i = 0; i < 5; i++) {
        in_order &= fifo_order[i] == i;
        thread_join(w[i]);
    }
    CHECK(in_order);

    struct sem s;
    sem_init(&s, 0);
    CHECK(!sem_trydown(&s));
    CHECK(sem_down_timeout(&s, 0) == SEM_TIMEOUT);
    uint64_t t0 = clock_ns();
    CHECK(sem_down_timeout(&s, 30) == SEM_TIMEOUT);
    uint64_t dt = clock_ns() - t0;
    CHECK(dt >= MS(29) && dt < MS(60));
    sem_up(&s);
    CHECK(sem_trydown(&s));
    sem_up(&s);
    CHECK(sem_down_timeout(&s, 10) == SEM_OK);

    sem_init(&helper_sem, 0);
    struct thread *h = thread_create("post-later", w_post_later, (void *)20, 0);
    t0 = clock_ns();
    CHECK(sem_down_timeout(&helper_sem, 500) == SEM_OK);
    dt = clock_ns() - t0;
    CHECK(dt >= MS(19) && dt < MS(80));
    thread_join(h);
    sem_up(&helper_sem);
    thread_sleep_ms(30);
    CHECK(sem_trydown(&helper_sem));

    sem_init(&pc.empty, RING);
    sem_init(&pc.full, 0);
    sem_init(&pc.lock, 1);
    pc.head = pc.tail = 0;
    struct thread *p0 = thread_create("prod0", w_producer, (void *)0, 0);
    struct thread *p1 = thread_create("prod1", w_producer, (void *)1, 0);
    struct thread *c0 = thread_create("cons0", w_consumer, NULL, 0);
    struct thread *c1 = thread_create("cons1", w_consumer, NULL, 0);
    thread_join(p0);
    thread_join(p1);
    int total = thread_join(c0) + thread_join(c1);
    CHECK(total == 125250 + 625250);
    CHECK(pc.head == 2 * PER_PRODUCER && pc.tail == 2 * PER_PRODUCER);

    klog(LOG_INFO, "sem: FIFO hand-off, try/timeout/early-wake, bounded buffer ok");
}

static uint64_t hog_cpu[3];
static int w_hog_report(void *arg)
{
    unsigned i = (unsigned)uarg(arg);
    volatile uint64_t c = 0;
    while (!spin_stop)
        c++;
    hog_cpu[i] = thread_cpu_ns(thread_current());
    return 0;
}

static void test_fairness(void)
{
    spin_stop = false;
    static const int nice[3] = { 0, 0, 5 };
    struct thread *h[3];
    uint64_t t0 = clock_ns();
    for (int i = 0; i < 3; i++)
        h[i] = thread_create("hog", w_hog_report, (void *)(uint64_t)i, nice[i]);
    thread_sleep_ms(1500);
    spin_stop = true;
    uint64_t window = clock_ns() - t0;
    for (int i = 0; i < 3; i++)
        thread_join(h[i]);

    uint64_t total = hog_cpu[0] + hog_cpu[1] + hog_cpu[2];
    CHECK(hog_cpu[0] > 0 && hog_cpu[1] > 0 && hog_cpu[2] > 0);
    CHECK(total > window * 9 / 10);
    CHECK(hog_cpu[0] * 100 >= hog_cpu[1] * 85 && hog_cpu[0] * 100 <= hog_cpu[1] * 115);
    CHECK(hog_cpu[2] * 100 >= hog_cpu[0] * 25 && hog_cpu[2] * 100 <= hog_cpu[0] * 42);

    klog(LOG_INFO, "fairness: nice 0/0/5 got %u.%u%% / %u.%u%% / %u.%u%% of hog CPU (ideal 42.9/42.9/14.0)",
         pct10(hog_cpu[0], total) / 10, pct10(hog_cpu[0], total) % 10,
         pct10(hog_cpu[1], total) / 10, pct10(hog_cpu[1], total) % 10,
         pct10(hog_cpu[2], total) / 10, pct10(hog_cpu[2], total) % 10);
}

static volatile uint64_t lat_max, lat_sum;
static volatile unsigned lat_n;
static int w_interactive(void *arg)
{
    (void)arg;
    for (int i = 0; i < 25; i++) {
        uint64_t t0 = clock_ns();
        thread_sleep_ms(10);
        uint64_t dt = clock_ns() - t0;
        uint64_t lat = dt > MS(10) ? dt - MS(10) : 0;
        if (lat > lat_max)
            lat_max = lat;
        lat_sum += lat;
        lat_n++;
    }
    return 0;
}

static void test_latency(void)
{
    spin_stop = false;
    lat_max = lat_sum = 0;
    lat_n = 0;
    struct thread *h0 = thread_create("hog", w_hog, NULL, 0);
    struct thread *h1 = thread_create("hog", w_hog, NULL, 0);
    struct thread *i = thread_create("interactive", w_interactive, NULL, 0);
    thread_join(i);
    spin_stop = true;
    thread_join(h0);
    thread_join(h1);

    CHECK(lat_n == 25);
    CHECK(lat_n && lat_sum / lat_n < MS(3));
    CHECK(lat_max < MS(25));
    klog(LOG_INFO, "latency: sleeper woke within avg %lu us / max %lu us while 2 hogs ran",
         lat_n ? lat_sum / lat_n / 1000 : 0, lat_max / 1000);
}

static volatile uint64_t s_woke, s_end, s_cpu;
static int w_sleeper(void *arg)
{
    (void)arg;
    thread_sleep_ms(300);
    s_woke = clock_ns();
    while (!spin_stop)
        ;
    s_end = clock_ns();
    s_cpu = thread_cpu_ns(thread_current());
    return 0;
}

static void test_sleeper_credit(void)
{
    spin_stop = false;
    s_woke = s_end = s_cpu = 0;
    struct thread *h0 = thread_create("hog", w_hog, NULL, 0);
    struct thread *h1 = thread_create("hog", w_hog, NULL, 0);
    struct thread *s = thread_create("sleeper", w_sleeper, NULL, 0);
    thread_sleep_ms(650);
    spin_stop = true;
    thread_join(s);
    thread_join(h0);
    thread_join(h1);

    uint64_t window = s_end - s_woke;
    CHECK(window > MS(250));
    CHECK(s_cpu * 100 >= window * 20);
    CHECK(s_cpu * 100 <= window * 45);
    klog(LOG_INFO, "lag: after a 300 ms sleep the thread got %u.%u%% of the next %lu ms (fair: 33.3)",
         pct10(s_cpu, window) / 10, pct10(s_cpu, window) % 10, window / 1000000);
}

static void test_set_nice(void)
{
    spin_stop = false;
    struct thread *a = thread_create("hog-a", w_hog, NULL, 0);
    struct thread *b = thread_create("hog-b", w_hog, NULL, 0);
    thread_sleep_ms(400);
    uint64_t a0 = thread_cpu_ns(a), b0 = thread_cpu_ns(b);
    CHECK(b0 * 100 >= a0 * 80 && b0 * 100 <= a0 * 125);

    sched_set_nice(b, 10);
    CHECK(b->nice == 10 && b->weight == 110);
    uint64_t a1s = thread_cpu_ns(a), b1s = thread_cpu_ns(b);
    thread_sleep_ms(800);
    uint64_t da = thread_cpu_ns(a) - a1s, db = thread_cpu_ns(b) - b1s;
    spin_stop = true;
    thread_join(a);
    thread_join(b);

    CHECK(da > 0 && db > 0);
    CHECK(db * 100 >= da * 5 && db * 100 <= da * 20);
    klog(LOG_INFO, "reweight: nice 0 -> 10 mid-run, share ratio b/a %u.%u%% (ideal 10.7)",
         pct10(db, da) / 10, pct10(db, da) % 10);
}

static struct sem gate;
static int w_gate(void *arg) { (void)arg; sem_down(&gate); return 0; }

static void test_lifecycle(void)
{
    thread_join(thread_create("warm", w_return, NULL, 0));

    uint64_t fr0 = pmm_free_frames(), used0, free0, blk0;
    heap_stats(&used0, &free0, &blk0);
    unsigned n0 = sched_thread_count();

    for (int i = 0; i < 64; i++) {
        struct thread *t = thread_create("cycle", w_return, NULL, 0);
        CHECK(t != NULL);
        if (t)
            thread_join(t);
    }
    uint64_t used1, free1, blk1;
    heap_stats(&used1, &free1, &blk1);
    CHECK(pmm_free_frames() == fr0);
    CHECK(used1 == used0);
    CHECK(sched_thread_count() == n0);

    for (int i = 0; i < 16; i++) {
        struct thread *t = thread_create("detached", w_return, NULL, 0);
        thread_detach(t);
    }
    thread_sleep_ms(100);
    heap_stats(&used1, &free1, &blk1);
    CHECK(sched_thread_count() == n0);
    CHECK(pmm_free_frames() == fr0);
    CHECK(used1 == used0);

    struct thread *z = thread_create("late-detach", w_return, NULL, 0);
    CHECK(wait_state(z, T_ZOMBIE, 200));
    thread_detach(z);
    thread_sleep_ms(30);
    CHECK(sched_thread_count() == n0);
    struct thread *d = thread_create("det-join", w_return, NULL, 0);
    thread_detach(d);
    CHECK(thread_join(d) == -1);
    thread_sleep_ms(30);

    enum { MANY = 200 };
    static struct thread *many[MANY];
    sem_init(&gate, 0);
    int made = 0;
    for (int i = 0; i < MANY; i++) {
        many[i] = thread_create("many", w_gate, NULL, (i % 10) - 5);
        if (many[i])
            made++;
    }
    CHECK(made == MANY);
    CHECK(sched_thread_count() == n0 + MANY);
    for (int i = 0; i < MANY; i++)
        sem_up(&gate);
    for (int i = 0; i < MANY; i++)
        if (many[i])
            thread_join(many[i]);
    CHECK(sched_thread_count() == n0);
    CHECK(fr0 - pmm_free_frames() <= 4);

    klog(LOG_INFO, "lifecycle: 64 create/join, 16 detached, 200 concurrent; frames and heap balanced");
}

static void test_preempt_guard(void)
{
    spin_stop = false;
    struct thread *h = thread_create("hog", w_hog, NULL, 0);
    thread_sleep_ms(10);

    struct thread *me = thread_current();
    uint64_t sw0 = me->nr_switches;
    uint64_t hog0 = thread_cpu_ns(h);

    preempt_disable();
    uint64_t t0 = clock_ns();
    while (clock_ns() - t0 < MS(25))
        ;
    bool pending = g_need_resched;
    uint64_t sw1 = me->nr_switches;
    uint64_t hog1 = thread_cpu_ns(h);
    preempt_enable();
    uint64_t sw2 = me->nr_switches;

    CHECK(pending);
    CHECK(sw1 == sw0 && hog1 == hog0);
    CHECK(sw2 > sw1);

    preempt_disable();
    preempt_disable();
    uint64_t sw3 = me->nr_switches;
    g_need_resched = true;
    preempt_enable();
    CHECK(g_preempt_count == 1 && me->nr_switches == sw3);
    preempt_enable();
    CHECK(g_preempt_count == 0);

    spin_stop = true;
    thread_join(h);
    klog(LOG_INFO, "preempt guard: held 25 ms with a hog waiting; no switch until preempt_enable()");
}

static volatile bool stress_stop;
static volatile int stress_errors;
static volatile unsigned stress_ops[4];

struct live { uint8_t *p; uint32_t sz; uint8_t pat; };

static bool verify(const struct live *l)
{
    for (uint32_t i = 0; i < l->sz; i++)
        if (l->p[i] != l->pat)
            return false;
    return true;
}

static int w_stress(void *arg)
{
    unsigned id = (unsigned)uarg(arg);
    uint32_t rng = 0x1234567u + id * 7919u;
    struct live live[16];
    memset(live, 0, sizeof(live));

    while (!stress_stop) {
        rng = rng * 1664525u + 1013904223u;
        unsigned i = (rng >> 16) & 15;
        if (live[i].p) {
            if (!verify(&live[i]))
                stress_errors++;
            kfree(live[i].p);
            live[i].p = NULL;
        } else {
            uint32_t sz = 16 + (rng >> 8) % (id == 3 ? 70000 : 2000);
            uint8_t *p = kmalloc(sz);
            if (!p) {
                stress_errors++;
                continue;
            }
            uint8_t pat = (uint8_t)(rng >> 24) | 1;
            memset(p, pat, sz);
            live[i].p = p;
            live[i].sz = sz;
            live[i].pat = pat;
        }
        stress_ops[id]++;
    }
    for (int i = 0; i < 16; i++) {
        if (live[i].p) {
            if (!verify(&live[i]))
                stress_errors++;
            kfree(live[i].p);
        }
    }
    return 0;
}

static void test_mm_preempt(void)
{
    uint64_t used0, free0, blk0;
    heap_stats(&used0, &free0, &blk0);

    stress_stop = false;
    stress_errors = 0;
    for (int i = 0; i < 4; i++)
        stress_ops[i] = 0;
    uint64_t sw0 = 0;
    g_mm_torture_ns = 40000;
    struct thread *w[4];
    for (int i = 0; i < 4; i++)
        w[i] = thread_create("mm-stress", w_stress, (void *)(uint64_t)i, 0);
    thread_sleep_ms(700);
    stress_stop = true;
    for (int i = 0; i < 4; i++) {
        sw0 += w[i]->nr_switches;
        thread_join(w[i]);
    }

    g_mm_torture_ns = 0;
    uint64_t used1, free1, blk1;
    heap_stats(&used1, &free1, &blk1);
    CHECK(stress_errors == 0);
    CHECK(heap_check());
    CHECK(used1 == used0);
    unsigned ops = stress_ops[0] + stress_ops[1] + stress_ops[2] + stress_ops[3];
    CHECK(ops > 100);
    klog(LOG_INFO, "mm under preemption (critical sections stretched to 40 us): 4 threads, %u ops, ~%lu switches, heap intact",
         ops, sw0);
}

static void test_eevdf_math(void)
{
    unsigned n = 0;
    unsigned f = sched_selftest_eevdf(&n);
    n_fail += f;
    n_checks += n;
    klog(f ? LOG_FAIL : LOG_INFO,
         "eevdf: weights, eligibility, earliest-deadline pick, lag clamp + preserve, new-thread request: %s",
         f ? "FAILED" : "ok");
}

#ifdef TEST_STACKOVERFLOW
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static int recurse(int n)
{
    volatile char pad[512];
    pad[0] = (char)n;
    return recurse(n + 1) + pad[0];
}
static int w_overflow(void *arg) { (void)arg; return recurse(0); }
#endif

int schedtest_main(void *arg)
{
    (void)arg;
    klog(LOG_INFO, "schedtest: running in thread '%s' (tid %u)", thread_current()->name,
         thread_current()->tid);
    n_checks = n_fail = 0;

#ifndef ONLY_W32
    test_eevdf_math();
    test_basic();
    test_timer();
    test_sem();
    test_fairness();
    test_latency();
    test_sleeper_credit();
    test_set_nice();
    test_lifecycle();
    test_preempt_guard();
    test_mm_preempt();

    uint64_t idle = sched_idle_ns();
    klog(n_fail ? LOG_FAIL : LOG_OK, "SCHEDTEST %s (%u checks, %u failed; idle ran %lu ms)",
         n_fail ? "FAIL" : "PASS", n_checks, n_fail, idle / 1000000);
    kprintf("\n");
    sched_dump();

    console_set_color(n_fail ? C_LRED : C_LGREEN, C_BLACK);
    kprintf("\nStep 2 (scheduler) %s.\n", n_fail ? "has FAILURES" : "complete");
    console_set_color(C_LGRAY, C_BLACK);

    n_fail += usertest_run();
    n_fail += ipctest_run();
    n_fail += objtest_run();
    n_fail += nstest_run();
    n_fail += sectest_run();
    n_fail += fstest_run();
    n_fail += linuxtest_run();
    n_fail += drvtest_run();
#endif
    n_fail += win32test_run();

#ifdef TEST_STACKOVERFLOW
    kprintf("TEST_STACKOVERFLOW: recursing in a thread until the guard page...\n");
    thread_join(thread_create("overflow", w_overflow, NULL, 0));
#endif
#ifdef TEST_FAULT
    kprintf("TEST_FAULT: touching an unmapped address...\n");
    *(volatile uint64_t *)0xDEADBEEF000ULL = 1;
#endif
    return (int)n_fail;
}
