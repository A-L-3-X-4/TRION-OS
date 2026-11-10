#include "kernel.h"
#include "io.h"

__attribute__((noreturn)) void kpanic(const char *fmt, ...)
{
    __asm__ volatile("cli");
    // White on red so you know something went very fucking wrong
    console_set_color(C_WHITE, C_RED);
    kprintf("\n*** KERNEL PANIC: ");

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    kprintf("\nSystem halted.\n");
    // Interrupts are already off and nothing runs after this, there is no way back from here
    cpu_halt_forever();
}
