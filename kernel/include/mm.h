#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void     pmm_init(uint64_t mbi_addr);
uint64_t pmm_alloc_frame(void);
void     pmm_free_frame(uint64_t phys);
uint64_t pmm_total_frames(void);
uint64_t pmm_free_frames(void);

#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITE   (1ULL << 1)
#define VMM_USER    (1ULL << 2)
#define VMM_PWT     (1ULL << 3)
#define VMM_PCD     (1ULL << 4)
#define VMM_HUGE    (1ULL << 7)
#define VMM_NX      (1ULL << 63)

#define VMM_OK        0
#define VMM_EINVAL   (-1)
#define VMM_EEXIST   (-2)
#define VMM_ENOMEM   (-3)

#define VMM_NOT_MAPPED ((uint64_t)-1)

void     vmm_init(void);
int      vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_unmap(uint64_t virt);
uint64_t vmm_translate(uint64_t virt);
bool     vmm_nx_enabled(void);

uint64_t vmm_as_create(void);
int      vmm_as_map(uint64_t as, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_as_lookup(uint64_t as, uint64_t virt, uint64_t *flags);
int      vmm_as_unmap(uint64_t as, uint64_t virt);
int      vmm_as_protect(uint64_t as, uint64_t virt, uint64_t flags);
void     vmm_as_activate(uint64_t as);
void     vmm_as_destroy(uint64_t as);

#define KERNEL_HEAP_BASE 0x0000004000000000ULL
#define KERNEL_HEAP_MAX  (1ULL << 30)

void  heap_init(void);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void *krealloc(void *p, size_t n);
void  kfree(void *p);
void  heap_stats(uint64_t *used, uint64_t *free_bytes, uint64_t *blocks);
bool  heap_check(void);

void mm_lock(void);
void mm_unlock(void);

extern volatile uint64_t g_mm_torture_ns;
void mm_stretch(void);
