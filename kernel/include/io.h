#pragma once
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t v)
{
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void insw(uint16_t port, void *buf, uint32_t words)
{
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(words) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *buf, uint32_t words)
{
    __asm__ volatile("rep outsw" : "+S"(buf), "+c"(words) : "d"(port) : "memory");
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

static inline void fxsave_(void *area)    { __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory"); }
static inline void fxrstor_(const void *area) { __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory"); }

static inline void invlpg(uint64_t addr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline __attribute__((noreturn)) void cpu_halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}
