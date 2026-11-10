#include "kernel.h"
#include "arch.h"
#include "abi.h"

#define GDT_CODE SEL_KCODE
#define GDT_DATA SEL_KDATA
#define GDT_TSS  SEL_TSS

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint64_t   gdt[8];
static struct tss tss;
struct percpu     g_percpu;
static uint8_t    ist1_stack[16 * 1024] __attribute__((aligned(16)));

void gdt_init(void)
{
    // Long mode ignores base and limit on pretty much all of this but the flags and DPL still count, and the ORDER matters because SYSRET picks selectors by offset from STAR (user code32, user data, user code64), so don't reshuffle it
    gdt[0] = 0;
    gdt[1] = 0x00209A0000000000ULL;
    gdt[2] = 0x0000920000000000ULL;
    gdt[3] = 0x00CFFA000000FFFFULL;
    gdt[4] = 0x0000F20000000000ULL;
    gdt[5] = 0x0020FA0000000000ULL;

    memset(&tss, 0, sizeof(tss));
    tss.ist[0] = (uint64_t)ist1_stack + sizeof(ist1_stack);
    tss.iomap_base = sizeof(tss);

    uint64_t base = (uint64_t)&tss;
    uint64_t limit = sizeof(tss) - 1;
    gdt[6] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | (0x89ULL << 40)
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[7] = base >> 32;

    struct gdt_ptr p = { sizeof(gdt) - 1, (uint64_t)gdt };
    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorl %%eax, %%eax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %3, %%ax\n\t"
        "ltr %%ax\n\t"
        : : "m"(p), "i"((uint64_t)GDT_CODE), "i"((uint16_t)GDT_DATA), "i"((uint16_t)GDT_TSS)
        : "rax", "memory");
}

void arch_set_kstack(uint64_t top)
{
    tss.rsp[0] = top;
    g_percpu.kstack_top = top;
}
