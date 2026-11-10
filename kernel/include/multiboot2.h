#pragma once
#include <stdint.h>
#include <stddef.h>

#define MB2_BOOT_MAGIC     0x36D76289u

#define MB2_TAG_END        0
#define MB2_TAG_CMDLINE    1
#define MB2_TAG_BOOTLOADER 2
#define MB2_TAG_MMAP       6
#define MB2_TAG_FB         8
#define MB2_FB_RGB         1

#define MB2_MEM_AVAILABLE  1
#define MB2_MEM_RESERVED   2
#define MB2_MEM_ACPI       3
#define MB2_MEM_NVS        4
#define MB2_MEM_BAD        5

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[];
};

struct mb2_tag_fb {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
    uint16_t reserved;
    uint8_t  red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} __attribute__((packed));

static inline uint32_t mb2_total_size(uint64_t mbi)
{
    return *(const uint32_t *)mbi;
}

static inline const struct mb2_tag *mb2_find_tag(uint64_t mbi, uint32_t type)
{
    const struct mb2_tag *t = (const struct mb2_tag *)(mbi + 8);
    while (t->type != MB2_TAG_END) {
        if (t->type == type)
            return t;
        t = (const struct mb2_tag *)(((uint64_t)t + t->size + 7) & ~7ULL);
    }
    return NULL;
}

#define MB2_FOR_EACH_MMAP(tag, e)                                              \
    for (const struct mb2_mmap_entry *e = (tag)->entries;                      \
         (const uint8_t *)e < (const uint8_t *)(tag) + (tag)->size;            \
         e = (const struct mb2_mmap_entry *)((const uint8_t *)e + (tag)->entry_size))
