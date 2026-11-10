#pragma once
#include "kernel.h"
#include "sched.h"

#define BLK_SECTOR      512
#define BLK_SECTOR_MAX  4096
#define BLK_NAME_MAX 16

struct blk_cline;
struct blkdev {
    char      name[BLK_NAME_MAX];
    uint64_t  sectors;
    uint32_t  ssize;
    bool      ro;
    bool      cached;
    int     (*rd)(struct blkdev *d, uint64_t lba, uint32_t n, void *buf);
    int     (*wr)(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf);
    int     (*flush)(struct blkdev *d);
    void    (*release)(struct blkdev *d);
    void     *priv;
    char      model[41];

    struct sem        lock;
    struct blk_cline *cache;
    uint8_t          *bounce;
    uint8_t          *cache_data;
    uint64_t  st_reads, st_writes, st_hits;
    struct blkdev *next;
};

int  blk_register(struct blkdev *d);
void blk_unregister(struct blkdev *d);
struct blkdev *blk_find(const char *name);
struct blkdev *blk_nth(unsigned i);
unsigned blk_count(void);

static inline uint32_t blk_ssize(const struct blkdev *d) { return d->ssize ? d->ssize : BLK_SECTOR; }

int  blk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf);
int  blk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf);
int  blk_flush(struct blkdev *d);

int  blk_read_part(struct blkdev *d, uint64_t lba, uint32_t off, uint32_t len, void *buf);
int  blk_write_part(struct blkdev *d, uint64_t lba, uint32_t off, uint32_t len, const void *buf);

struct blkdev *blk_ramdisk_create(const char *name, uint64_t sectors);
struct blkdev *blk_ramdisk_create_ss(const char *name, uint64_t sectors, uint32_t ssize);
unsigned       blk_ramdisk_chunks(struct blkdev *d);

struct blkdev *blk_partition_create(struct blkdev *parent, const char *name,
                                    uint64_t first_lba, uint64_t sectors);
int  blk_scan_partitions(struct blkdev *d);

int  ata_init(void);
extern bool g_ata_force_lba48;

void     rtc_init(void);
uint64_t wall_unix(void);
uint64_t wall_ns(void);
void     unix_to_civil(uint64_t t, int *y, int *mo, int *d, int *h, int *mi, int *s);
uint64_t civil_to_unix(int y, int mo, int d, int h, int mi, int s);

unsigned blktest_run(void);
