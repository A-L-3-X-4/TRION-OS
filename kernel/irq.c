#include "kernel.h"
#include "io.h"
#include "irq.h"
#include "sched.h"

// 8259 PIC ports, command and data for the master (0x20) and slave (0xA0), a chip from the 70s that every PC still pretends to have, and we're not on the APIC yet
#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1
#define PIC_EOI  0x20
#define PIC_READ_ISR 0x0B

// Software copy of the PIC masks (1 means masked, backwards from what you'd hope) so single bits can be flipped without reading the chip
static uint8_t mask1 = 0xFF, mask2 = 0xFF;
static irq_handler_t handlers[IRQ_COUNT];
static volatile uint64_t counts[IRQ_COUNT];
static volatile uint64_t spurious;

// A write to port 0x80 takes about a microsecond, the traditional way to give a slow ancient PIC time to catch up between commands
static inline void io_wait(void) { outb(0x80, 0); }

// Remaps IRQs to vectors 32-47 instead of 0-15, because the default collides with the CPU exceptions and the first timer tick would show up as a Double Fault. The outb pairs are the four init words, and everything ends up masked except IRQ2, the cascade line the slave hangs off
void pic_init(void)
{
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DAT, PIC_VECTOR_BASE);     io_wait();
    outb(PIC2_DAT, PIC_VECTOR_BASE + 8); io_wait();
    outb(PIC1_DAT, 0x04); io_wait();
    outb(PIC2_DAT, 0x02); io_wait();
    outb(PIC1_DAT, 0x01); io_wait();
    outb(PIC2_DAT, 0x01); io_wait();

    mask1 = 0xFB;
    mask2 = 0xFF;
    outb(PIC1_DAT, mask1);
    outb(PIC2_DAT, mask2);
}

// One handler per line, last one to register wins, out of range lines get ignored
void irq_register(unsigned irq, irq_handler_t h)
{
    if (irq < IRQ_COUNT)
        handlers[irq] = h;
}

// Clears the line's mask bit with interrupts off so a half written mask can't get interrupted
void irq_unmask(unsigned irq)
{
    uint64_t f = irq_save();
    if (irq < 8) {
        mask1 &= (uint8_t)~(1u << irq);
        outb(PIC1_DAT, mask1);
    } else if (irq < 16) {
        mask2 &= (uint8_t)~(1u << (irq - 8));
        outb(PIC2_DAT, mask2);
    }
    irq_restore(f);
}

// Same thing, other direction
void irq_mask(unsigned irq)
{
    uint64_t f = irq_save();
    if (irq < 8) {
        mask1 |= (uint8_t)(1u << irq);
        outb(PIC1_DAT, mask1);
    } else if (irq < 16) {
        mask2 |= (uint8_t)(1u << (irq - 8));
        outb(PIC2_DAT, mask2);
    }
    irq_restore(f);
}

uint64_t irq_count(unsigned irq)        { return irq < IRQ_COUNT ? counts[irq] : 0; }
uint64_t irq_spurious_count(void)       { return spurious; }

// Tells the PIC we're done. Lines 8 and up come through the slave so it needs an EOI too, and then the master gets one because the slave is plugged into it
static void eoi(unsigned irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

// The PIC sometimes fires IRQ 7 or 15 when nothing's there (noise, a line that dropped mid-ack), so ask its in-service register if it's real. A fake 15 still needs an EOI to the master since the master did see the cascade go off
static bool is_spurious(unsigned irq)
{
    if (irq == 7) {
        outb(PIC1_CMD, PIC_READ_ISR);
        return !(inb(PIC1_CMD) & 0x80);
    }
    if (irq == 15) {
        outb(PIC2_CMD, PIC_READ_ISR);
        if (!(inb(PIC2_CMD) & 0x80)) {
            outb(PIC1_CMD, PIC_EOI);
            return true;
        }
    }
    return false;
}

// Every hardware interrupt comes through here: drop spurious ones, count it, run the handler, send the EOI, and only then, if we're not nested and preemption is on, let the scheduler switch away. EOI has to come BEFORE the switch or the PIC stays blocked on this line for as long as the next thread runs
void irq_dispatch(unsigned vec)
{
    unsigned irq = vec - PIC_VECTOR_BASE;
    if (irq >= IRQ_COUNT)
        return;

    g_irq_depth++;
    if (is_spurious(irq)) {
        spurious++;
        g_irq_depth--;
        return;
    }
    counts[irq]++;
    if (handlers[irq])
        handlers[irq]();

    eoi(irq);
    g_irq_depth--;

    if (g_irq_depth == 0 && g_preempt_count == 0 && g_need_resched)
        schedule();
}
