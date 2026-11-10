#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "timer.h"

// Test knob, when it's nonzero mm_stretch() burns this many ns inside the allocator's critical sections, so a missing lock shows up as heap corruption instead of working on my machine
volatile uint64_t g_mm_torture_ns;

// On one CPU a lock is just turning preemption off. Calling this from an interrupt panics the entire kernel on purpose, as a warning to whoever writes the next one
void mm_lock(void)
{
    if (g_irq_depth)
        kpanic("mm: allocator used from interrupt context");
    preempt_disable();
}

// Preemption back on, the scheduler can do whatever it wants again
void mm_unlock(void)
{
    preempt_enable();
}

// Spins for g_mm_torture_ns while the lock is held, a deliberate stall whose only job is to make race conditions hurt
void mm_stretch(void)
{
    uint64_t ns = g_mm_torture_ns;
    if (!ns)
        return;
    uint64_t t0 = clock_ns();
    while (clock_ns() - t0 < ns)
        ;
}
