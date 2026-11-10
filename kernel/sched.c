#include "kernel.h"
#include "sched.h"
#include "sched_asm.h"
#include "mm.h"
#include "timer.h"

volatile int  g_preempt_count;
volatile int  g_irq_depth;
volatile bool g_need_resched;

#define SCHED_TICK_NS 1000000ULL
#define IF_FLAG       (1ULL << 9)

static const uint32_t nice_weight_tbl[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
     9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
     1024,   820,   655,   526,   423,   335,   272,   215,   172,   137,
      110,    87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static struct {
    struct list_node list;
    unsigned         nr;
    int64_t          v_ref;
} rq;

static struct thread  idle_thread;
static struct thread *cur;
static bool           sched_ready;

static struct list_node all_threads;
static struct list_node timer_list;
static struct list_node zombie_list;
static struct sem       reap_sem;
static uint32_t         next_tid = 1;
static unsigned         nr_threads;

static uint32_t nice_to_weight(int nice)
{
    if (nice < NICE_MIN) nice = NICE_MIN;
    if (nice > NICE_MAX) nice = NICE_MAX;
    // Clamp nice to NICE_MIN..NICE_MAX then look up the weight in the table, nicer means smaller weight means less CPU, so being nice gets you fucked
    return nice_weight_tbl[nice - NICE_MIN];
}

static inline int64_t calc_vdelta(uint64_t delta, uint32_t weight)
{
    return (int64_t)(delta * NICE_0_WEIGHT / weight);
}

static inline int64_t vslice(const struct thread *t)
{
    return calc_vdelta(t->slice_ns, t->weight);
}

static inline int64_t lag_limit(const struct thread *t)
{
    uint64_t s = 2 * t->slice_ns;
    if (s < SCHED_TICK_NS)
        s = SCHED_TICK_NS;
    return calc_vdelta(s, t->weight);
}

static inline void resched_point(void)
{
    if (g_need_resched && !g_preempt_count && !g_irq_depth && !irqs_disabled())
        schedule();
}

struct thread *thread_current(void) { return cur; }

struct rq_avg {
    int64_t  base;
    int64_t  sum;
    uint64_t load;
    int64_t  V;
};

static void rq_avg_calc(struct rq_avg *a)
{
    if (!rq.nr) {
        a->base = a->V = rq.v_ref;
        a->sum = 0;
        a->load = 0;
        return;
    }
    int64_t base = INT64_MAX;
    list_for_each(n, &rq.list) {
        struct thread *t = container_of(n, struct thread, rq_node);
        if (t->v < base)
            base = t->v;
    }
    int64_t sum = 0;
    uint64_t load = 0;
    list_for_each(n, &rq.list) {
        struct thread *t = container_of(n, struct thread, rq_node);
        sum += (t->v - base) * (int64_t)t->weight;
        load += t->weight;
    }
    a->base = base;
    a->sum = sum;
    a->load = load;
    a->V = base + sum / (int64_t)load;
}

static inline bool eligible(const struct thread *t, const struct rq_avg *a)
{
    return (t->v - a->base) * (int64_t)a->load <= a->sum;
}

static struct thread *pick_next(void)
{
    if (!rq.nr)
        return NULL;
    struct rq_avg a;
    rq_avg_calc(&a);

    // O(n) walk over the whole runqueue to find the eligible thread with the earliest deadline, if nobody's eligible just take the lowest v. A tree would be better but n is tiny so I'm not bothering
    struct thread *best = NULL, *min_v = NULL;
    list_for_each(n, &rq.list) {
        struct thread *t = container_of(n, struct thread, rq_node);
        if (!min_v || t->v < min_v->v)
            min_v = t;
        if (eligible(t, &a) && (!best || t->deadline < best->deadline))
            best = t;
    }
    return best ? best : min_v;
}

static void rq_add(struct thread *t)
{
    list_add_tail(&rq.list, &t->rq_node);
    rq.nr++;
}

static void rq_del(struct thread *t)
{
    list_del(&t->rq_node);
    rq.nr--;
}

static void update_curr(void)
{
    struct thread *t = cur;
    uint64_t now = clock_ns();
    uint64_t delta = now > t->exec_start ? now - t->exec_start : 0;
    t->exec_start = now;
    t->sum_exec_ns += delta;
    if (t != &idle_thread)
        t->v += calc_vdelta(delta, t->weight);
}

static void rq_remove_lag(struct thread *t)
{
    struct rq_avg a;
    rq_avg_calc(&a);
    int64_t lag = a.V - t->v;
    int64_t lim = lag_limit(t);
    if (lag > lim) lag = lim;
    if (lag < -lim) lag = -lim;
    t->vlag = lag;
    rq_del(t);
    if (!rq.nr)
        rq.v_ref = a.V;
}

static void dequeue_sleep(struct thread *t)
{
    update_curr();
    rq_remove_lag(t);
}

static void enqueue_wake(struct thread *t)
{
    struct rq_avg a;
    rq_avg_calc(&a);

    int64_t lag = t->vlag;
    if (rq.nr && lag)
        lag = lag * (int64_t)(a.load + t->weight) / (int64_t)a.load;
    t->v = a.V - lag;

    int64_t vs = vslice(t);
    if (t->fresh) {
        vs /= 2;
        t->fresh = false;
    }
    t->deadline = t->v + vs;
    t->state = T_RUNNABLE;
    rq_add(t);
}

static void check_preempt_wakeup(struct thread *t)
{
    if (cur == &idle_thread || pick_next() == t)
        g_need_resched = true;
}

static void wake_locked(struct thread *t)
{
    update_curr();
    enqueue_wake(t);
    check_preempt_wakeup(t);
}

static void __schedule(void)
{
    if (g_preempt_count)
        kpanic("scheduling while atomic (preempt_count=%d) in thread %s",
               g_preempt_count, cur->name);
    if (g_irq_depth)
        kpanic("schedule() from interrupt handler context");

    struct thread *prev = cur;
    update_curr();
    g_need_resched = false;

    struct thread *next = pick_next();
    if (!next)
        next = &idle_thread;

    if (prev->state == T_RUNNING)
        prev->state = T_RUNNABLE;
    next->state = T_RUNNING;

    if (next != prev) {
        next->exec_start = clock_ns();
        next->nr_switches++;
        cur = next;
        sched_arch_switch(prev, next);
        ctx_switch(&prev->rsp, next->rsp);
    }
}

void schedule(void)
{
    uint64_t f = irq_save();
    __schedule();
    irq_restore(f);
}

void preempt_enable(void)
{
    __asm__ volatile("" : : : "memory");
    if (g_preempt_count <= 0)
        kpanic("preempt_enable: count underflow");
    if (--g_preempt_count == 0 && g_need_resched && g_irq_depth == 0 &&
        sched_ready && !irqs_disabled())
        schedule();
}

void thread_yield(void)
{
    if (!sched_ready || g_preempt_count || g_irq_depth)
        return;
    uint64_t f = irq_save();
    if (cur != &idle_thread) {
        update_curr();
        cur->deadline += vslice(cur);
    }
    __schedule();
    irq_restore(f);
}

static void timer_insert(struct thread *t)
{
    struct list_node *pos = &timer_list;
    list_for_each(n, &timer_list) {
        struct thread *o = container_of(n, struct thread, timer_node);
        if (o->wake_tick > t->wake_tick) {
            pos = n;
            break;
        }
    }
    list_insert_before(pos, &t->timer_node);
}

void sched_tick(void)
{
    if (!sched_ready)
        return;

    uint64_t now = timer_ticks();
    while (!list_empty(&timer_list)) {
        struct thread *t = container_of(timer_list.next, struct thread, timer_node);
        if (t->wake_tick > now)
            break;
        list_del(&t->timer_node);
        if (t->waiting_on) {
            list_del(&t->wait_node);
            t->waiting_on = NULL;
            t->wait_result = SEM_TIMEOUT;
        } else {
            t->wait_result = SEM_OK;
        }
        wake_locked(t);
    }

    if (cur == &idle_thread) {
        if (rq.nr)
            g_need_resched = true;
        return;
    }
    update_curr();
    if (cur->v >= cur->deadline) {
        cur->deadline = cur->v + vslice(cur);
        if (rq.nr > 1)
            g_need_resched = true;
    }
}

static void assert_can_block(uint64_t flags, const char *who)
{
    if (!sched_ready || cur == &idle_thread || g_irq_depth || g_preempt_count ||
        !(flags & IF_FLAG))
        kpanic("%s: cannot block here (idle=%d irq_depth=%d preempt=%d IF=%d)", who,
               cur == &idle_thread, g_irq_depth, g_preempt_count, !!(flags & IF_FLAG));
}

static int block_current(struct sem *s, uint64_t timeout_ms, enum thread_state st)
{
    struct thread *t = cur;
    t->wait_result = SEM_OK;
    if (s) {
        t->waiting_on = s;
        list_add_tail(&s->waiters, &t->wait_node);
    }
    if (timeout_ms != SEM_FOREVER) {
        t->wake_tick = timer_ticks() + timer_ms_to_ticks(timeout_ms);
        timer_insert(t);
    }
    t->state = st;
    dequeue_sleep(t);
    __schedule();
    return t->wait_result;
}

void thread_sleep_ms(uint64_t ms)
{
    if (ms == 0) {
        thread_yield();
        return;
    }
    uint64_t f = irq_save();
    assert_can_block(f, "thread_sleep_ms");
    block_current(NULL, ms, T_SLEEPING);
    irq_restore(f);
}

void sem_init(struct sem *s, int initial)
{
    s->count = initial;
    list_init(&s->waiters);
}

bool sem_trydown(struct sem *s)
{
    uint64_t f = irq_save();
    bool ok = s->count > 0;
    if (ok)
        s->count--;
    irq_restore(f);
    return ok;
}

int sem_down_timeout(struct sem *s, uint64_t ms)
{
    uint64_t f = irq_save();
    int r;
    if (s->count > 0) {
        s->count--;
        r = SEM_OK;
    } else if (ms == 0) {
        r = SEM_TIMEOUT;
    } else {
        assert_can_block(f, "sem_down");
        r = block_current(s, ms, T_BLOCKED);
    }
    irq_restore(f);
    return r;
}

void sem_down(struct sem *s)
{
    sem_down_timeout(s, SEM_FOREVER);
}

void sem_up(struct sem *s)
{
    uint64_t f = irq_save();
    if (list_empty(&s->waiters)) {
        s->count++;
    } else {
        struct thread *w = container_of(s->waiters.next, struct thread, wait_node);
        list_del(&w->wait_node);
        w->waiting_on = NULL;
        w->wait_result = SEM_OK;
        if (list_linked(&w->timer_node))
            list_del(&w->timer_node);
        wake_locked(w);
    }
    irq_restore(f);
    resched_point();
}

#define SLOT_PAGES (KSTACK_PAGES + 1)
#define SLOT_SIZE  (SLOT_PAGES * PAGE_SIZE)

static uint64_t       kstack_bitmap[KSTACK_SLOTS / 64];
static struct thread *kstack_owner[KSTACK_SLOTS];

static inline uint64_t slot_base(int slot) { return KSTACK_BASE + (uint64_t)slot * SLOT_SIZE; }

static int kstack_alloc(struct thread *owner, uint64_t *lo_out)
{
    int slot = -1;
    uint64_t f = irq_save();
    for (int i = 0; i < KSTACK_SLOTS; i++) {
        if (!(kstack_bitmap[i / 64] & (1ULL << (i % 64)))) {
            kstack_bitmap[i / 64] |= 1ULL << (i % 64);
            kstack_owner[i] = owner;
            slot = i;
            break;
        }
    }
    irq_restore(f);
    if (slot < 0)
        return -1;

    uint64_t lo = slot_base(slot) + PAGE_SIZE;
    int mapped = 0;
    for (; mapped < KSTACK_PAGES; mapped++) {
        uint64_t fr = pmm_alloc_frame();
        if (!fr)
            break;
        if (vmm_map(lo + (uint64_t)mapped * PAGE_SIZE, fr, VMM_WRITE | VMM_NX) != VMM_OK) {
            pmm_free_frame(fr);
            break;
        }
    }
    if (mapped < KSTACK_PAGES) {
        for (int i = 0; i < mapped; i++) {
            uint64_t fr = vmm_unmap(lo + (uint64_t)i * PAGE_SIZE);
            if (fr)
                pmm_free_frame(fr);
        }
        f = irq_save();
        kstack_bitmap[slot / 64] &= ~(1ULL << (slot % 64));
        kstack_owner[slot] = NULL;
        irq_restore(f);
        return -1;
    }
    *lo_out = lo;
    return slot;
}

static void kstack_free(int slot)
{
    uint64_t lo = slot_base(slot) + PAGE_SIZE;
    for (int i = 0; i < KSTACK_PAGES; i++) {
        uint64_t fr = vmm_unmap(lo + (uint64_t)i * PAGE_SIZE);
        if (fr)
            pmm_free_frame(fr);
    }
    uint64_t f = irq_save();
    kstack_bitmap[slot / 64] &= ~(1ULL << (slot % 64));
    kstack_owner[slot] = NULL;
    irq_restore(f);
}

const char *kstack_owner_of_fault(uint64_t addr, uint32_t *tid)
{
    if (addr < KSTACK_BASE || addr >= KSTACK_BASE + (uint64_t)KSTACK_SLOTS * SLOT_SIZE)
        return NULL;
    uint64_t off = addr - KSTACK_BASE;
    int slot = (int)(off / SLOT_SIZE);
    struct thread *o = kstack_owner[slot];
    if (!o)
        return NULL;
    if (off % SLOT_SIZE < PAGE_SIZE + 1024)
        return *tid = o->tid, o->name;
    return NULL;
}

bool thread_stack_range(struct thread *t, uint64_t *lo, uint64_t *hi, uint64_t *guard)
{
    if (!t || t->stack_slot < 0)
        return false;
    uint64_t g = slot_base(t->stack_slot);
    if (guard) *guard = g;
    if (lo) *lo = g + PAGE_SIZE;
    if (hi) *hi = g + PAGE_SIZE + KSTACK_SIZE;
    return true;
}

static __attribute__((noreturn)) void thread_start(void)
{
    irq_enable();
    int rc = cur->fn(cur->arg);
    thread_exit(rc);
}

static void init_thread_common(struct thread *t, const char *name, int nice)
{
    size_t i = 0;
    for (; name && name[i] && i < sizeof(t->name) - 1; i++)
        t->name[i] = name[i];
    t->name[i] = 0;

    if (nice < NICE_MIN) nice = NICE_MIN;
    if (nice > NICE_MAX) nice = NICE_MAX;
    t->nice = nice;
    t->weight = nice_to_weight(nice);
    t->slice_ns = SCHED_SLICE_NS_DEFAULT;
    t->fresh = true;
    t->stack_slot = -1;
    list_init(&t->rq_node);
    list_init(&t->wait_node);
    list_init(&t->timer_node);
    list_init(&t->all_node);
    sem_init(&t->exit_sem, 0);
}

struct thread *thread_create(const char *name, int (*fn)(void *), void *arg, int nice)
{
    return thread_create_ex(name, fn, arg, nice, NULL);
}

struct thread *thread_create_ex(const char *name, int (*fn)(void *), void *arg, int nice,
                                struct task *task)
{
    struct thread *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    uint64_t lo;
    int slot = kstack_alloc(t, &lo);
    if (slot < 0) {
        kfree(t);
        return NULL;
    }
    init_thread_common(t, name, nice);
    t->stack_slot = slot;
    t->kstack_top = lo + KSTACK_SIZE;
    t->task = task;
    t->fn = fn;
    t->arg = arg;

    uint64_t *sp = (uint64_t *)(lo + KSTACK_SIZE);
    *--sp = 0;
    *--sp = (uint64_t)thread_start;
    for (int i = 0; i < 6; i++)
        *--sp = 0;
    t->rsp = (uint64_t)sp;

    uint64_t f = irq_save();
    t->tid = next_tid++;
    list_add_tail(&all_threads, &t->all_node);
    nr_threads++;
    wake_locked(t);
    irq_restore(f);
    resched_point();
    return t;
}

static void thread_free(struct thread *t)
{
    uint64_t f = irq_save();
    list_del(&t->all_node);
    nr_threads--;
    irq_restore(f);
    kstack_free(t->stack_slot);
    kfree(t);
}

void thread_exit(int code)
{
    if (cur == &idle_thread)
        kpanic("the idle thread cannot exit");
    if (cur->task)
        task_exit_hook(cur->task);
    uint64_t f = irq_save();
    (void)f;
    struct thread *t = cur;
    t->exit_code = code;
    dequeue_sleep(t);
    t->state = T_ZOMBIE;
    if (t->detached) {
        list_add_tail(&zombie_list, &t->wait_node);
        sem_up(&reap_sem);
    } else {
        sem_up(&t->exit_sem);
    }
    __schedule();
    kpanic("zombie thread %s was scheduled again", t->name);
}

int thread_join(struct thread *t)
{
    if (!t || t == cur || t->detached)
        return -1;
    sem_down(&t->exit_sem);
    int rc = t->exit_code;
    thread_free(t);
    return rc;
}

void thread_detach(struct thread *t)
{
    uint64_t f = irq_save();
    if (!t->detached) {
        t->detached = true;
        if (t->state == T_ZOMBIE) {
            list_add_tail(&zombie_list, &t->wait_node);
            sem_up(&reap_sem);
        }
    }
    irq_restore(f);
}

static int reaper_main(void *arg)
{
    (void)arg;
    for (;;) {
        sem_down(&reap_sem);
        uint64_t f = irq_save();
        struct thread *t = NULL;
        if (!list_empty(&zombie_list)) {
            t = container_of(zombie_list.next, struct thread, wait_node);
            list_del(&t->wait_node);
        }
        irq_restore(f);
        if (t)
            thread_free(t);
    }
    return 0;
}

void sched_set_nice(struct thread *t, int nice)
{
    if (nice < NICE_MIN) nice = NICE_MIN;
    if (nice > NICE_MAX) nice = NICE_MAX;

    uint64_t f = irq_save();
    uint32_t old_w = t->weight, new_w = nice_to_weight(nice);
    t->nice = nice;
    if (new_w != old_w) {
        if (list_linked(&t->rq_node)) {
            update_curr();
            struct rq_avg a;
            rq_avg_calc(&a);
            int64_t lag = a.V - t->v;
            int64_t rel = t->deadline - t->v;
            t->weight = new_w;
            t->v = a.V - lag * (int64_t)old_w / (int64_t)new_w;
            t->deadline = t->v + rel * (int64_t)old_w / (int64_t)new_w;
            g_need_resched = true;
        } else {
            t->vlag = t->vlag * (int64_t)old_w / (int64_t)new_w;
            t->weight = new_w;
        }
    }
    irq_restore(f);
    resched_point();
}

void sched_set_slice(struct thread *t, uint64_t slice_ns)
{
    if (slice_ns < 100000)
        slice_ns = 100000;
    uint64_t f = irq_save();
    t->slice_ns = slice_ns;
    if (list_linked(&t->rq_node)) {
        update_curr();
        t->deadline = t->v + vslice(t);
        g_need_resched = true;
    }
    irq_restore(f);
    resched_point();
}

uint64_t thread_cpu_ns(struct thread *t)
{
    uint64_t f = irq_save();
    if (t == cur)
        update_curr();
    uint64_t r = t->sum_exec_ns;
    irq_restore(f);
    return r;
}

uint64_t sched_idle_ns(void) { return thread_cpu_ns(&idle_thread); }

unsigned sched_thread_count(void)
{
    uint64_t f = irq_save();
    unsigned n = nr_threads;
    irq_restore(f);
    return n;
}

static const char *state_name(enum thread_state s)
{
    switch (s) {
    case T_RUNNING:  return "run";
    case T_RUNNABLE: return "ready";
    case T_BLOCKED:  return "blocked";
    case T_SLEEPING: return "sleep";
    case T_ZOMBIE:   return "zombie";
    }
    return "?";
}

void sched_dump(void)
{
    uint64_t f = irq_save();
    update_curr();
    kprintf("  tid name             state   nice  cpu(ms)  switches\n");
    list_for_each(n, &all_threads) {
        struct thread *t = container_of(n, struct thread, all_node);
        kprintf("  %3u %-16s %-7s %4d %8lu %9lu\n", t->tid, t->name, state_name(t->state),
                t->nice, t->sum_exec_ns / 1000000, t->nr_switches);
    }
    irq_restore(f);
}

void sched_init(void)
{
    list_init(&rq.list);
    rq.nr = 0;
    rq.v_ref = 0;
    list_init(&all_threads);
    list_init(&timer_list);
    list_init(&zombie_list);
    sem_init(&reap_sem, 0);

    memset(&idle_thread, 0, sizeof(idle_thread));
    init_thread_common(&idle_thread, "idle", 0);
    idle_thread.tid = 0;
    idle_thread.fresh = false;
    idle_thread.state = T_RUNNING;
    idle_thread.exec_start = clock_ns();
    list_add_tail(&all_threads, &idle_thread.all_node);
    nr_threads = 1;
    cur = &idle_thread;
    sched_ready = true;

    struct thread *r = thread_create("reaper", reaper_main, NULL, 0);
    if (!r)
        kpanic("sched: cannot create reaper thread");
}

void sched_idle_loop(void)
{
    for (;;) {
        irq_disable();
        if (g_need_resched) {
            irq_enable();
            schedule();
        } else {
            // sti only takes effect after the NEXT instruction so sti; hlt can't lose a wakeup between the need_resched check and the halt, which is the one genuinely clever thing x86 does here
            __asm__ volatile("sti; hlt" : : : "memory");
        }
    }
}

unsigned sched_selftest_eevdf(unsigned *checks)
{
    unsigned fails = 0, nchk = 0;
#define T(c) do { nchk++; if (!(c)) { klog(LOG_FAIL, "sched.c:%d: %s", __LINE__, #c); fails++; } } while (0)

    T(nice_to_weight(0) == 1024);
    T(nice_to_weight(-20) == 88761);
    T(nice_to_weight(19) == 15);
    T(nice_to_weight(100) == 15 && nice_to_weight(-100) == 88761);
    for (int n = NICE_MIN; n < NICE_MAX; n++)
        T(nice_to_weight(n) > nice_to_weight(n + 1));
    T(calc_vdelta(1000000, 1024) == 1000000);
    T(calc_vdelta(1000000, 335) == 3056716);

    static struct thread d1, d2, d3;
    struct thread *D[3] = { &d1, &d2, &d3 };

    uint64_t f = irq_save();
    update_curr();
    struct rq_avg a0;
    rq_avg_calc(&a0);
    T(rq.nr >= 1);

    for (int i = 0; i < 3; i++) {
        memset(D[i], 0, sizeof(*D[i]));
        init_thread_common(D[i], "dummy", 0);
        D[i]->fresh = false;
    }

    int64_t big = 50 * 1000000LL;
    d1.v = a0.V + big;  d1.deadline = INT64_MIN / 4;
    d2.v = a0.V - big;  d2.deadline = INT64_MIN / 2 + 1;
    rq_add(&d1);
    rq_add(&d2);
    struct rq_avg a1;
    rq_avg_calc(&a1);
    T(a1.V > a0.V - 1000 && a1.V < a0.V + 1000);
    T(!eligible(&d1, &a1));
    T(eligible(&d2, &a1));
    T(pick_next() == &d2);
    rq_del(&d1);
    rq_del(&d2);

    d2.v = a0.V - big;
    rq_add(&d2);
    rq_remove_lag(&d2);
    T(d2.vlag == lag_limit(&d2));
    T(d2.vlag == 6 * 1000000LL);

    d3.weight = 335;
    d3.v = a0.V - 2 * 1000000LL;
    rq_add(&d3);
    struct rq_avg b;
    rq_avg_calc(&b);
    int64_t expect = b.V - d3.v;
    rq_remove_lag(&d3);
    T(d3.vlag == expect);
    T(d3.vlag > 0 && d3.vlag < lag_limit(&d3));
    enqueue_wake(&d3);
    struct rq_avg c;
    rq_avg_calc(&c);
    int64_t got = c.V - d3.v;
    T(got >= d3.vlag - 4 && got <= d3.vlag + 4);
    T(eligible(&d3, &c));
    rq_del(&d3);

    d3.vlag = -3 * 1000000LL;
    enqueue_wake(&d3);
    rq_avg_calc(&c);
    got = c.V - d3.v;
    T(got >= d3.vlag - 4 && got <= d3.vlag + 4);
    T(!eligible(&d3, &c));
    rq_del(&d3);

    d1.fresh = true;
    d1.vlag = 0;
    enqueue_wake(&d1);
    T(d1.deadline - d1.v == vslice(&d1) / 2);
    T(!d1.fresh);
    rq_del(&d1);

    irq_restore(f);
#undef T
    *checks = nchk;
    return fails;
}
