#pragma once
#include "kernel.h"
#include "abi.h"
#include "w32abi.h"

struct task;

long wvm_alloc(struct task *t, uint64_t addr, uint64_t size, uint32_t flags);
long wvm_free(struct task *t, uint64_t addr, uint64_t size, uint32_t flags);
long wvm_protect(struct task *t, uint64_t addr, uint64_t size, uint32_t prot);
int  wvm_query(struct task *t, uint64_t addr, struct vm_info *out);
long sys_vm_dispatch(struct task *t, uint64_t nr, uint64_t a, uint64_t b, uint64_t c);

struct w32_load {
    struct task *t;
    unsigned     nmissing;
    uint64_t     rt_missing;
};
int  w32_bind_import(struct w32_load *ctx, const char *dll, const char *name, uint32_t ordinal, uint64_t *addr);

struct pe_info {
    uint64_t base, size;
    uint64_t entry;
    uint32_t subsystem;
    uint64_t stack_reserve, stack_commit;
    uint64_t tls_block;
    uint32_t ntls;
    uint64_t tls_cb[W32_MAX_TLS_CB + 1];
    bool     relocated;
};
extern bool pe_verbose;
int pe_load(struct task *t, struct w32_load *ctx, const void *img, size_t size, struct pe_info *out);

struct task *win32_task_new(const char *name, const void *img, size_t size, const char *const *argv,
                            const char *const *envp, const char *cwd, const char *imgpath, int *err);
struct task *win32_task_new_from_file(const char *ntpath, const char *const *argv, const char *const *envp,
                                      const char *cwd, int *err);
int  win32_pe_probe(const void *img, size_t size);
unsigned win32_export_count(void);
uint64_t win32_export_addr(const char *dll, const char *name);

unsigned win32test_run(void);
