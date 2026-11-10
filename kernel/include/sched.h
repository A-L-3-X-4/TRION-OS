#pragma once
#include "kernel.h"
#include "list.h"
#include "irq.h"

#define NICE_MIN    (-20)
#define NICE_MAX    19
#define NICE_0_WEIGHT 1024

#define SCHED_SLICE_NS_DEFAULT 3000000ULL

#define KSTACK_BASE   0x0000006000000000ULL
#define KSTACK_PAGES  4
#define KSTACK_SIZE   (KSTACK_PAGES * PAGE_SIZE)
#define KSTACK_SLOTS  512

struct task;

enum thread_state { T_RUNNING, T_RUNNABLE, T_BLOCKED, T_SLEEPING, T_ZOMBIE };

#define SEM_OK       0
#define SEM_TIMEOUT  1
#define SEM_FOREVER  ((uint64_t)-1)

struct sem {
    int              count;
    struct list_node waiters;
};

struct thread {
    uint64_t          rsp;

    uint32_t          tid;
    char              name[16];
    enum thread_state state;
    bool              detached;

    int               nice;
    uint32_t          weight;
    uint64_t          slice_ns;
    int64_t           v;
    int64_t           deadline;
    int64_t           vlag;
    bool              fresh;

    uint64_t          exec_start;
    uint64_t          sum_exec_ns;
    uint64_t          nr_switches;

    uint64_t          wake_tick;
    struct sem       *waiting_on;
    int               wait_result;

    int             (*fn)(void *);
    void             *arg;
    int               exit_code;
    struct sem        exit_sem;
    int               stack_slot;
    uint64_t          kstack_top;
    struct task      *task;

    struct list_node  rq_node;
    struct list_node  wait_node;
    struct list_node  timer_node;
    struct list_node  all_node;
};

extern volatile int  g_preempt_count;
extern volatile int  g_irq_depth;
extern volatile bool g_need_resched;

static inline void preempt_disable(void)
{
    g_preempt_count++;
    __asm__ volatile("" : : : "memory");
}
void preempt_enable(void);

void            sched_init(void);
__attribute__((noreturn)) void sched_idle_loop(void);
void            sched_tick(void);
void            schedule(void);
void            thread_yield(void);
struct thread  *thread_current(void);

struct thread  *thread_create(const char *name, int (*fn)(void *), void *arg, int nice);
struct thread  *thread_create_ex(const char *name, int (*fn)(void *), void *arg, int nice,
                                 struct task *task);
void            sched_arch_switch(struct thread *prev, struct thread *next);
void            task_exit_hook(struct task *t);
__attribute__((noreturn)) void thread_exit(int code);
int             thread_join(struct thread *t);
void            thread_detach(struct thread *t);
void            thread_sleep_ms(uint64_t ms);

void            sched_set_nice(struct thread *t, int nice);
void            sched_set_slice(struct thread *t, uint64_t slice_ns);

void sem_init(struct sem *s, int initial);
void sem_down(struct sem *s);
int  sem_down_timeout(struct sem *s, uint64_t ms);
bool sem_trydown(struct sem *s);
void sem_up(struct sem *s);

uint64_t    thread_cpu_ns(struct thread *t);
uint64_t    sched_idle_ns(void);
unsigned    sched_thread_count(void);
void        sched_dump(void);
bool        thread_stack_range(struct thread *t, uint64_t *lo, uint64_t *hi, uint64_t *guard);
const char *kstack_owner_of_fault(uint64_t addr, uint32_t *tid);

unsigned sched_selftest_eevdf(unsigned *checks);
int      schedtest_main(void *arg);
