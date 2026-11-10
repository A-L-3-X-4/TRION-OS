#pragma once
#include <stdint.h>
#include <stdbool.h>

struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vec, err;
    uint64_t rip, cs, rflags, rsp, ss;
};

#define SYSCALL_VECTOR 0x80

struct percpu {
    uint64_t kstack_top;
    uint64_t user_rsp;
};

void arch_set_kstack(uint64_t top);

void syscall_init(void);
void syscall_handler(struct regs *r);

extern void syscall_entry(void);
__attribute__((noreturn)) void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg);

extern struct percpu g_percpu;

const char *exception_name(unsigned vec);
