#include "kernel.h"
#include "io.h"
#include "irq.h"
#include "mm.h"
#include "arch.h"
#include "abi.h"
#include "task.h"
#include "ipc.h"
#include "obj.h"
#include "sec.h"
#include "linux.h"

// Biggest flat user binary task_new will take
#define IMAGE_MAX (512 * KiB)

// Task IDs start at 1 and only go up, never reused, so a stale ID can't end up pointing at some other task later
static uint32_t next_task_id = 1;
static unsigned live_tasks;

// The task the running thread belongs to, NULL for pure kernel threads
struct task *task_current(void)
{
    struct thread *t = thread_current();
    return t ? t->task : NULL;
}

unsigned task_live_count(void) { return live_tasks; }

// Runs on every context switch. Points the kernel stack at the next thread's (that's where the next syscall or interrupt from user mode lands), switches page tables if it belongs to a task, and lets the Linux layer save and restore the FPU
void sched_arch_switch(struct thread *prev, struct thread *next)
{
    if (next->kstack_top)
        arch_set_kstack(next->kstack_top);
    if (next->task)
        vmm_as_activate(next->task->as);
    if (prev->task)
        linux_switch_out(prev->task);
    linux_switch_in(next->task);
}

// Every task's main thread starts here and blocks on the start semaphore, which is what lets task_new + ipc_grant set everything up before the program runs a single instruction. enter_user never returns
static int task_thread_main(void *arg)
{
    struct task *t = arg;
    sem_down(&t->start);
    enter_user(t->entry, t->usp, t->arg);
}

// Grabs a frame, zeroes it, copies up to a page of data in if there is any, maps it, and gives the frame back if the mapping fails so we don't leak it
static bool map_zeroed(uint64_t as, uint64_t va, const void *src, size_t n, uint64_t flags)
{
    uint64_t f = pmm_alloc_frame();
    if (!f)
        return false;
    memset((void *)f, 0, PAGE_SIZE);
    if (src)
        memcpy((void *)f, src, n);
    if (vmm_as_map(as, va, f, flags) != VMM_OK) {
        pmm_free_frame(f);
        return false;
    }
    return true;
}

bool task_map_zeroed(uint64_t as, uint64_t va, uint64_t flags)
{
    return map_zeroed(as, va, NULL, 0, flags);
}

// A bare task with a name, security token, IPC space, handle table and empty address space, and if any of it fails the rest gets torn back down
struct task *task_alloc(const char *name)
{
    struct task *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    for (size_t i = 0; name && name[i] && i < sizeof(t->name) - 1; i++)
        t->name[i] = name[i];
    struct task *parent = task_current();
    // Children start with their creator's token, so you can't get out of your own permissions by spawning a kid
    if (parent)
        t->tok = parent->tok;
    else
        sec_token_system(&t->tok);
    sem_init(&t->start, 0);

    t->ipc = ipc_space_create();
    t->ht = ht_create();
    t->as = vmm_as_create();
    if (!t->ipc || !t->ht || !t->as) {
        task_discard(t);
        return NULL;
    }
    return t;
}

// Evidence disposal for a task that never got to run, address space, IPC space, handle table and Linux state all gone like it never existed
void task_discard(struct task *t)
{
    if (t->as)
        vmm_as_destroy(t->as);
    if (t->ipc)
        ipc_space_destroy(t->ipc);
    if (t->ht)
        ht_destroy(t->ht);
    linux_task_free(t);
    kfree(t);
}

// Gives the task its ID, counts it as alive and creates its main thread, which blocks until task_start. If thread creation fails the live count gets undone and we return false
bool task_launch(struct task *t, uint64_t entry, uint64_t usp, uint64_t arg)
{
    t->entry = entry;
    t->usp = usp;
    t->arg = arg;
    uint64_t f = irq_save();
    t->id = next_task_id++;
    live_tasks++;
    irq_restore(f);

    t->main = thread_create_ex(t->name, task_thread_main, t, 0, t);
    if (!t->main) {
        f = irq_save();
        live_tasks--;
        irq_restore(f);
        return false;
    }
    return true;
}

// Builds a flat binary task. Copies the image page by page to USER_CODE, maps the stack writable and no-execute, launches it, and any failure jumps to fail and throws it all away
struct task *task_new(const char *name, const void *image, size_t size,
                      uint64_t entry_off, uint64_t arg)
{
    if (!size || size > IMAGE_MAX || entry_off >= size)
        return NULL;
    struct task *t = task_alloc(name);
    if (!t)
        return NULL;

    const uint8_t *src = image;
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        size_t n = size - off < PAGE_SIZE ? size - off : PAGE_SIZE;
        if (!map_zeroed(t->as, USER_CODE + off, src + off, n, VMM_USER))
            goto fail;
    }
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t va = USER_STACK_TOP - (uint64_t)(i + 1) * PAGE_SIZE;
        if (!map_zeroed(t->as, va, NULL, 0, VMM_USER | VMM_WRITE | VMM_NX))
            goto fail;
    }
    if (!task_launch(t, USER_CODE + entry_off, USER_STACK_TOP, arg))
        goto fail;
    return t;

fail:
    task_discard(t);
    return NULL;
}

// Lets the task run, by bumping the semaphore its main thread has been sitting on
void task_start(struct task *t)
{
    sem_up(&t->start);
}

// task_new + task_start for when you don't need to set anything up in between
struct task *task_create(const char *name, const void *image, size_t size,
                         uint64_t entry_off, uint64_t arg)
{
    struct task *t = task_new(name, image, size, entry_off, arg);
    if (t)
        task_start(t);
    return t;
}

// A dead task's rights get seized the instant it dies, not whenever someone gets around to the paperwork, and the pointers get NULLed first so task_wait doesn't free them twice
void task_exit_hook(struct task *t)
{
    struct ipc_space *s = t->ipc;
    struct handle_table *h = t->ht;
    t->ipc = NULL;
    t->ht = NULL;
    if (s)
        ipc_space_destroy(s);
    if (h)
        ht_destroy(h);
}

// Joins the main thread, cleans up what's left, drops the live count, frees the task and returns its exit code. After this the struct is gone, don't touch t
int task_wait(struct task *t)
{
    int rc = thread_join(t->main);
    if (t->ht)
        ht_destroy(t->ht);
    if (t->ipc)
        ipc_space_destroy(t->ipc);
    vmm_as_destroy(t->as);
    linux_task_free(t);
    uint64_t f = irq_save();
    live_tasks--;
    irq_restore(f);
    kfree(t);
    return rc;
}

// Finds the physical page behind a user address, faulting it in through the Linux layer if it isn't there, so the copy functions below work on lazily mapped memory too
static uint64_t user_page(struct task *t, uint64_t a, bool write, uint64_t *fl)
{
    uint64_t pa = vmm_as_lookup(t->as, a, fl);
    if (pa == VMM_NOT_MAPPED && t->lx && lx_fault_in(t, a, write ? LX_PROT_WRITE : LX_PROT_READ))
        pa = vmm_as_lookup(t->as, a, fl);
    return pa;
}

// Every pointer from user mode is assumed to be lying until proven otherwise, so the range and every page's USER bit get checked as we go, and a bad one returns -E_FAULT instead of taking the kernel down with it
int user_copy_in(struct task *t, void *dst, uint64_t uaddr, size_t n)
{
    if (!n)
        return 0;
    if (uaddr < USER_BASE || uaddr >= USER_TOP || n > USER_TOP - uaddr)
        return -E_FAULT;
    uint8_t *d = dst;
    while (n) {
        uint64_t fl;
        uint64_t pa = user_page(t, uaddr, false, &fl);
        if (pa == VMM_NOT_MAPPED || !(fl & VMM_USER))
            return -E_FAULT;
        size_t chunk = PAGE_SIZE - (size_t)(uaddr & (PAGE_SIZE - 1));
        if (chunk > n)
            chunk = n;
        memcpy(d, (const void *)pa, chunk);
        d += chunk;
        uaddr += chunk;
        n -= chunk;
    }
    return 0;
}

// Checks a user buffer is fully mapped and writable BEFORE we do anything we can't undo, so a syscall fails cleanly up front instead of half finishing
int user_check_out(struct task *t, uint64_t uaddr, size_t n)
{
    if (!n)
        return 0;
    if (uaddr < USER_BASE || uaddr >= USER_TOP || n > USER_TOP - uaddr)
        return -E_FAULT;
    for (uint64_t a = ALIGN_DOWN(uaddr, PAGE_SIZE); a < uaddr + n; a += PAGE_SIZE) {
        uint64_t fl;
        if (user_page(t, a, true, &fl) == VMM_NOT_MAPPED ||
            (fl & (VMM_USER | VMM_WRITE)) != (VMM_USER | VMM_WRITE))
            return -E_FAULT;
    }
    return 0;
}

// Writes to user memory only after user_check_out says the whole range is good, so it either all works or nothing gets written
int user_copy_out(struct task *t, uint64_t uaddr, const void *src, size_t n)
{
    int rc = user_check_out(t, uaddr, n);
    if (rc || !n)
        return rc;
    const uint8_t *s = src;
    while (n) {
        uint64_t pa = vmm_as_lookup(t->as, uaddr, NULL);
        size_t chunk = PAGE_SIZE - (size_t)(uaddr & (PAGE_SIZE - 1));
        if (chunk > n)
            chunk = n;
        memcpy((void *)pa, s, chunk);
        s += chunk;
        uaddr += chunk;
        n -= chunk;
    }
    return 0;
}

// User code did something illegal, so write it up (that's the klog), then kill it on the spot with exit code -(TASK_FAULT_BASE + vector). No trial, no appeal, the kernel keeps running and the program doesn't
void task_fault(struct regs *r)
{
    // Interrupts back on, this path can block and the CPU turned them off when it took the exception
    irq_enable();
    struct thread *th = thread_current();
    struct task *t = th ? th->task : NULL;

    klog(LOG_WARN, "task '%s' (id %u) killed: %s (vector %lu, err 0x%lx) at rip=%lx",
         t ? t->name : "?", t ? t->id : 0, exception_name((unsigned)r->vec),
         r->vec, r->err, r->rip);
    if (r->vec == 14)
        kprintf("         page fault at %p (%s, %s%s)\n", (void *)read_cr2(),
                (r->err & 1) ? "protection violation" : "not present",
                (r->err & 2) ? "write" : "read",
                (r->err & 16) ? ", instruction fetch" : "");

    thread_exit(-(TASK_FAULT_BASE + (int)r->vec));
}
