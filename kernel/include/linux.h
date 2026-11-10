#pragma once
#include "kernel.h"
#include "abi.h"
#include "arch.h"

#define LX_STACK_TOP    (USER_TOP - PAGE_SIZE)
#define LX_STACK_MAX    (8 * MiB)
#define LX_MMAP_TOP     (LX_STACK_TOP - LX_STACK_MAX - 64 * MiB)
#define LX_PIE_BASE     0x8000400000ULL
#define LX_BRK_MAX      (1ULL * GiB)

#define LX_MAX_FD    64
#define LX_MAX_VMA   128
#define LX_PATH_MAX  300
#define LX_ARG_MAX   (64 * KiB)

#define LX_PROT_READ  1
#define LX_PROT_WRITE 2
#define LX_PROT_EXEC  4

enum { LXV_ANON = 1, LXV_IMAGE, LXV_HEAP, LXV_STACK, LXV_FILE,
       LXV_WRESERVE, LXV_WCOMMIT, LXV_WIMAGE };
struct lx_vma { uint64_t start, end; uint32_t prot, kind; uint64_t abase; uint32_t aprot, rsvd; };

enum { LXFD_NONE = 0, LXFD_CONSOLE, LXFD_FILE };
struct lx_fd { uint8_t kind; uint8_t cloexec; uint8_t append; uint8_t rsvd; uint32_t handle; uint32_t dir_idx; };

enum { LXF_LINUX = 0, LXF_WIN32 = 1 };

struct lx {
    uint32_t      flavor;
    uint64_t      gs_base;
    struct lx_vma vma[LX_MAX_VMA];
    unsigned      nvma;
    uint64_t      brk_start, brk_cur;
    struct lx_fd  fd[LX_MAX_FD];
    char          cwd[LX_PATH_MAX + 1];
    uint64_t      fs_base;
    uint64_t      rng;
    uint64_t      unsup[8];
    uint8_t       fpu_raw[512 + 16];
    bool          fpu_valid;
};

static inline uint8_t *lx_fpu(struct lx *l) { return (uint8_t *)ALIGN_UP((uint64_t)l->fpu_raw, 16); }

struct elf_info {
    uint64_t entry;
    uint64_t phdr, phnum, phent;
    uint64_t lo, hi;
    uint64_t bias;
    bool     pie;
};

int elf_load(struct task *t, const void *img, size_t size, struct elf_info *out);

struct task;
struct task *linux_task_new(const char *name, const void *img, size_t size,
                            const char *const *argv, const char *const *envp, int *err);
int linux_elf_probe(const void *img, size_t size);
struct task *linux_task_new_from_file(const char *ntpath, const char *const *argv,
                                      const char *const *envp, int *err);
void linux_task_free(struct task *t);

bool linux_demand_fault(struct regs *r, uint64_t addr);
bool lx_fault_in(struct task *t, uint64_t addr, uint32_t need);
struct lx_vma *lx_vma_lookup(struct lx *l, uint64_t addr);
bool lx_vma_busy(struct lx *l, uint64_t start, uint64_t end);
int  lx_vma_cut(struct task *t, uint64_t start, uint64_t end);
uint64_t lx_prot_to_vmm(uint32_t prot);
int  lx_vma_add(struct lx *l, uint64_t start, uint64_t end, uint32_t prot, uint32_t kind);
void linux_syscall(struct regs *r);

void linux_switch_out(struct task *prev);
void linux_switch_in(struct task *next);
void linux_arch_init(void);

unsigned linuxtest_run(void);

int linux_path_probe(const char *cwd_nt, const char *path, char *out, size_t cap);
