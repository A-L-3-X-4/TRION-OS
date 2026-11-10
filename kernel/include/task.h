#pragma once
#include "kernel.h"
#include "sched.h"
#include "arch.h"
#include "abi.h"

struct ipc_space;
struct handle_table;
struct lx;

struct task {
    uint32_t          id;
    char              name[16];
    uint64_t          as;
    struct handle_table *ht;
    struct token      tok;
    struct ipc_space *ipc;
    struct thread    *main;
    uint64_t          entry;
    uint64_t          arg;
    uint64_t          usp;
    struct lx        *lx;
    struct sem        start;
};

struct task *task_create(const char *name, const void *image, size_t size,
                         uint64_t entry_off, uint64_t arg);

struct task *task_new(const char *name, const void *image, size_t size,
                      uint64_t entry_off, uint64_t arg);
void         task_start(struct task *t);

int  task_wait(struct task *t);

struct task *task_alloc(const char *name);
bool         task_launch(struct task *t, uint64_t entry, uint64_t usp, uint64_t arg);
void         task_discard(struct task *t);
bool         task_map_zeroed(uint64_t as, uint64_t va, uint64_t flags);

struct task *task_current(void);

int  user_copy_in(struct task *t, void *dst, uint64_t uaddr, size_t n);

int  user_copy_out(struct task *t, uint64_t uaddr, const void *src, size_t n);
int  user_check_out(struct task *t, uint64_t uaddr, size_t n);

void task_exit_hook(struct task *t);

__attribute__((noreturn)) void task_fault(struct regs *r);

unsigned task_live_count(void);

extern const uint8_t user_tests_start[], user_tests_end[];

unsigned usertest_run(void);
