#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RFLAGS_IF (1ULL << 9)

static inline uint64_t irq_save(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
    return f;
}

static inline void irq_restore(uint64_t f)
{
    __asm__ volatile("pushq %0; popfq" : : "r"(f) : "memory", "cc");
}

static inline void irq_enable(void)  { __asm__ volatile("sti" : : : "memory"); }
static inline void irq_disable(void) { __asm__ volatile("cli" : : : "memory"); }

static inline bool irqs_disabled(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f));
    return !(f & RFLAGS_IF);
}

#define PIC_VECTOR_BASE 32
#define IRQ_TIMER       0
#define IRQ_KEYBOARD    1
#define IRQ_COM1        4
#define IRQ_COUNT       16

typedef void (*irq_handler_t)(void);

void     pic_init(void);
void     irq_register(unsigned irq, irq_handler_t h);
void     irq_unmask(unsigned irq);
void     irq_mask(unsigned irq);
uint64_t irq_count(unsigned irq);
uint64_t irq_spurious_count(void);

void     irq_dispatch(unsigned vec);
