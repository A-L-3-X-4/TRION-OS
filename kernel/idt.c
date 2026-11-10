#include "kernel.h"
#include "io.h"
#include "irq.h"
#include "sched.h"
#include "arch.h"
#include "task.h"
#include "linux.h"

// A 64-bit IDT gate, the handler address is chopped into three pieces because this used to be 16-bit and Intel has never thrown anything away
struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// 32 CPU exceptions plus 16 PIC IRQs, anything past that is unpopulated
#define IDT_VECTORS 48
#define IST_DOUBLE_FAULT 1

extern void *isr_stub_table[IDT_VECTORS];
static struct idt_entry idt[256];

// Names for the 32 exception vectors so the crash screen can say General Protection Fault instead of making you remember what vector 13 is
static const char *const exc_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow",
    "BOUND Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 FP Exception", "Alignment Check",
    "Machine Check", "SIMD FP Exception", "Virtualization Exception",
    "Control Protection Exception", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Hypervisor Injection",
    "VMM Communication", "Security Exception", "Reserved",
};

const char *exception_name(unsigned vec)
{
    return exc_names[vec & 31];
}

// One gate per vector from the stub table in isr.asm, kernel code segment, interrupt gate type 0x8E (clears IF on entry), and the double fault gets its own IST stack so it can still run when the normal stack is what exploded
void idt_init(void)
{
    for (int i = 0; i < IDT_VECTORS; i++) {
        uint64_t h = (uint64_t)isr_stub_table[i];
        idt[i].off_lo  = (uint16_t)(h & 0xFFFF);
        idt[i].sel     = 0x08;
        idt[i].ist     = (i == 8) ? IST_DOUBLE_FAULT : 0;
        idt[i].attr    = 0x8E;
        idt[i].off_mid = (uint16_t)((h >> 16) & 0xFFFF);
        idt[i].off_hi  = (uint32_t)(h >> 32);
        idt[i].zero    = 0;
    }
    struct idt_ptr p = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" : : "m"(p));
}

// The C half of every interrupt and exception. Hardware IRQs go to irq_dispatch, page faults from user mode get a shot at being demand paged, any other fault from user mode gets that task shot, and anything that survives all of that is a kernel bug, which prints the crash screen and halts
void isr_handler(struct regs *r)
{
    if (r->vec >= PIC_VECTOR_BASE) {
        irq_dispatch((unsigned)r->vec);
        return;
    }

    // Page fault from ring 3, interrupts back on while we try to fault the page in (that can block), then off again before going back to the stub
    if ((r->cs & 3) && r->vec == 14) {
        uint64_t addr = read_cr2();
        irq_enable();
        bool ok = linux_demand_fault(r, addr);
        irq_disable();
        if (ok)
            return;
    }
    if ((r->cs & 3) && r->vec < 32)
        task_fault(r);

    // Everything below is the crash screen and nobody comes back from it, so make it loud
    console_set_color(C_WHITE, C_RED);
    kprintf("\n*** KERNEL FAULT: %s (vector %lu, error 0x%lx)\n",
            exc_names[r->vec & 31], r->vec, r->err);

    if (r->vec == 14) {
        kprintf("Page fault at %p: %s, %s, %s%s%s\n", (void *)read_cr2(),
                (r->err & 1) ? "protection violation" : "page not present",
                (r->err & 2) ? "write" : "read",
                (r->err & 4) ? "user" : "supervisor",
                (r->err & 8) ? ", reserved bit set" : "",
                (r->err & 16) ? ", instruction fetch" : "");
    }

    struct thread *cur = thread_current();
    if (cur)
        kprintf("Thread: '%s' (tid %u)\n", cur->name, cur->tid);
    // A page fault or double fault next to a thread's stack means it ran off the end into the guard page, so check the faulting address and RSP against every kernel stack and name the thread that did it instead of leaving you with a hex dump
    if (r->vec == 14 || r->vec == 8) {
        uint32_t tid;
        const char *who = kstack_owner_of_fault(read_cr2(), &tid);
        if (!who)
            who = kstack_owner_of_fault(r->rsp, &tid);
        if (who)
            kprintf("KERNEL STACK OVERFLOW in thread '%s' (tid %u): hit the guard page\n", who, tid);
    }

    kprintf("RIP=%016lx CS=%04lx RFLAGS=%08lx\n", r->rip, r->cs, r->rflags);
    kprintf("RSP=%016lx SS=%04lx CR3=%016lx\n", r->rsp, r->ss, read_cr3());
    kprintf("RAX=%016lx RBX=%016lx RCX=%016lx\n", r->rax, r->rbx, r->rcx);
    kprintf("RDX=%016lx RSI=%016lx RDI=%016lx\n", r->rdx, r->rsi, r->rdi);
    kprintf("RBP=%016lx R8 =%016lx R9 =%016lx\n", r->rbp, r->r8, r->r9);
    kprintf("System halted.\n");
    // Nothing to return to, so sit here forever and let you read the screen
    cpu_halt_forever();
}
