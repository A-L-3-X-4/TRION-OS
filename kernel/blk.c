#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "blk.h"
#include "abi.h"

static struct blkdev *devs;
static unsigned ndevs;

#define CACHE_LINES 128
struct blk_cline {
    uint64_t lba;
    bool     valid;
};

static inline uint8_t *line_data(struct blkdev *d, const struct blk_cline *l)
{
    return d->cache_data + (size_t)(l - d->cache) * d->ssize;
}

static bool ssize_ok(uint32_t s) { return s >= BLK_SECTOR && s <= BLK_SECTOR_MAX && !(s & (s - 1)); }

int blk_register(struct blkdev *d)
{
    if (!d->rd || !d->wr)
        return -E_INVAL;
    if (!d->ssize)
        d->ssize = BLK_SECTOR;
    if (!ssize_ok(d->ssize))
        return -E_INVAL;
    sem_init(&d->lock, 1);
    d->bounce = kmalloc(d->ssize);
    if (!d->bounce)
        return -E_NOMEM;
    if (d->cached) {
        d->cache = kzalloc(sizeof(struct blk_cline) * CACHE_LINES);
        d->cache_data = kzalloc((size_t)d->ssize * CACHE_LINES);
        if (!d->cache || !d->cache_data) {
            kfree(d->cache);
            kfree(d->cache_data);
            kfree(d->bounce);
            d->cache = NULL;
            d->cache_data = NULL;
            d->bounce = NULL;
            return -E_NOMEM;
        }
    }
    preempt_disable();
    struct blkdev **pp = &devs;
    while (*pp)
        pp = &(*pp)->next;
    *pp = d;
    d->next = NULL;
    ndevs++;
    preempt_enable();
    return 0;
}

void blk_unregister(struct blkdev *d)
{
    preempt_disable();
    for (struct blkdev **pp = &devs; *pp; pp = &(*pp)->next)
        if (*pp == d) {
            *pp = d->next;
            ndevs--;
            break;
        }
    preempt_enable();
    if (d->release)
        d->release(d);
    kfree(d->cache);
    kfree(d->cache_data);
    kfree(d->bounce);
    kfree(d);
}

struct blkdev *blk_find(const char *name)
{
    struct blkdev *r = NULL;
    preempt_disable();
    for (struct blkdev *d = devs; d; d = d->next)
        if (!strcmp(d->name, name)) {
            r = d;
            break;
        }
    preempt_enable();
    return r;
}

struct blkdev *blk_nth(unsigned i)
{
    struct blkdev *d;
    preempt_disable();
    for (d = devs; d && i; d = d->next)
        i--;
    preempt_enable();
    return d;
}

unsigned blk_count(void) { return ndevs; }

static inline struct blk_cline *line_of(struct blkdev *d, uint64_t lba)
{
    return &d->cache[lba % CACHE_LINES];
}

int blk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf)
{
    if (!n)
        return 0;
    if (lba >= d->sectors || n > d->sectors - lba)
        return -E_INVAL;
    sem_down(&d->lock);
    int rc;
    d->st_reads += n;
    if (d->cache && n == 1) {
        struct blk_cline *l = line_of(d, lba);
        if (l->valid && l->lba == lba) {
            memcpy(buf, line_data(d, l), d->ssize);
            d->st_hits++;
            rc = 0;
        } else {
            l->valid = false;
            rc = d->rd(d, lba, 1, line_data(d, l));
            if (!rc) {
                l->lba = lba;
                l->valid = true;
                memcpy(buf, line_data(d, l), d->ssize);
            }
        }
    } else {
        rc = d->rd(d, lba, n, buf);
    }
    sem_up(&d->lock);
    return rc;
}

int blk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf)
{
    if (!n)
        return 0;
    if (lba >= d->sectors || n > d->sectors - lba)
        return -E_INVAL;
    if (d->ro)
        return -E_ROFS;
    sem_down(&d->lock);
    d->st_writes += n;
    int rc = d->wr(d, lba, n, buf);
    if (d->cache) {
        if (rc) {
            for (uint32_t i = 0; i < n && i < CACHE_LINES; i++)
                line_of(d, lba + i)->valid = false;
        } else if (n == 1) {
            struct blk_cline *l = line_of(d, lba);
            l->lba = lba;
            l->valid = true;
            memcpy(line_data(d, l), buf, d->ssize);
        } else if (n >= CACHE_LINES) {
            for (uint32_t k = 0; k < CACHE_LINES; k++) {
                struct blk_cline *c = &d->cache[k];
                if (c->valid && c->lba >= lba && c->lba < lba + n)
                    c->valid = false;
            }
        } else {
            for (uint32_t i = 0; i < n; i++) {
                struct blk_cline *l = line_of(d, lba + i);
                if (l->valid && l->lba == lba + i)
                    l->valid = false;
            }
        }
    }
    sem_up(&d->lock);
    return rc;
}

static int part_io(struct blkdev *d, uint64_t lba, uint32_t off, uint32_t len, void *rbuf, const void *wbuf)
{
    if (!len)
        return 0;
    if (lba >= d->sectors || off >= d->ssize || len > d->ssize - off)
        return -E_INVAL;
    if (wbuf && d->ro)
        return -E_ROFS;
    sem_down(&d->lock);
    struct blk_cline *l = d->cache ? line_of(d, lba) : NULL;
    uint8_t *sec = l ? line_data(d, l) : d->bounce;
    int rc = 0;
    d->st_reads++;
    if (l && l->valid && l->lba == lba) {
        d->st_hits++;
    } else {
        if (l)
            l->valid = false;
        rc = d->rd(d, lba, 1, sec);
        if (!rc && l) {
            l->lba = lba;
            l->valid = true;
        }
    }
    if (!rc) {
        if (!wbuf) {
            memcpy(rbuf, sec + off, len);
        } else {
            memcpy(sec + off, wbuf, len);
            d->st_writes++;
            rc = d->wr(d, lba, 1, sec);
            if (rc && l)
                l->valid = false;
        }
    }
    sem_up(&d->lock);
    return rc;
}

int blk_read_part(struct blkdev *d, uint64_t lba, uint32_t off, uint32_t len, void *buf)
{
    return part_io(d, lba, off, len, buf, NULL);
}

int blk_write_part(struct blkdev *d, uint64_t lba, uint32_t off, uint32_t len, const void *buf)
{
    return part_io(d, lba, off, len, NULL, buf);
}

int blk_flush(struct blkdev *d)
{
    int rc = 0;
    sem_down(&d->lock);
    if (d->flush)
        rc = d->flush(d);
    sem_up(&d->lock);
    return rc;
}

#define RD_CHUNK_BYTES 4096
struct ramdisk {
    uint8_t **chunk;
    uint64_t  nchunks;
    unsigned  used;
    uint32_t  ssize;
    uint32_t  per;
};

static int rd_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf)
{
    struct ramdisk *r = d->priv;
    uint8_t *out = buf;
    while (n--) {
        uint8_t *c = r->chunk[lba / r->per];
        if (c)
            memcpy(out, c + (lba % r->per) * r->ssize, r->ssize);
        else
            memset(out, 0, r->ssize);
        out += r->ssize;
        lba++;
    }
    return 0;
}

static int rd_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf)
{
    struct ramdisk *r = d->priv;
    const uint8_t *in = buf;
    while (n--) {
        uint8_t **cp = &r->chunk[lba / r->per];
        if (!*cp) {
            *cp = kzalloc(RD_CHUNK_BYTES);
            if (!*cp)
                return -E_NOMEM;
            r->used++;
        }
        memcpy(*cp + (lba % r->per) * r->ssize, in, r->ssize);
        in += r->ssize;
        lba++;
    }
    return 0;
}

static void rd_release(struct blkdev *d)
{
    struct ramdisk *r = d->priv;
    for (uint64_t i = 0; i < r->nchunks; i++)
        kfree(r->chunk[i]);
    kfree(r->chunk);
    kfree(r);
}

struct blkdev *blk_ramdisk_create(const char *name, uint64_t sectors)
{
    return blk_ramdisk_create_ss(name, sectors, BLK_SECTOR);
}

struct blkdev *blk_ramdisk_create_ss(const char *name, uint64_t sectors, uint32_t ssize)
{
    struct blkdev *d = NULL;
    struct ramdisk *r = NULL;
    if (!ssize_ok(ssize) || !sectors)
        return NULL;
    d = kzalloc(sizeof(*d));
    r = kzalloc(sizeof(*r));
    if (!d || !r)
        goto fail;
    r->ssize = ssize;
    r->per = RD_CHUNK_BYTES / ssize;
    d->ssize = ssize;
    r->nchunks = (sectors + r->per - 1) / r->per;
    r->chunk = kzalloc(r->nchunks * sizeof(uint8_t *));
    if (!r->chunk)
        goto fail;
    size_t i = 0;
    while (name[i] && i < BLK_NAME_MAX - 1) {
        d->name[i] = name[i];
        i++;
    }
    d->sectors = sectors;
    d->rd = rd_read;
    d->wr = rd_write;
    d->release = rd_release;
    d->priv = r;
    d->cached = false;
    for (const char *m = "RAM disk"; *m; m++)
        d->model[m - "RAM disk"] = *m;
    if (blk_register(d))
        goto fail;
    return d;
fail:
    if (r) {
        kfree(r->chunk);
        kfree(r);
    }
    kfree(d);
    return NULL;
}

unsigned blk_ramdisk_chunks(struct blkdev *d) { return ((struct ramdisk *)d->priv)->used; }

struct part {
    struct blkdev *parent;
    uint64_t       first;
};

static int part_rd(struct blkdev *d, uint64_t lba, uint32_t n, void *buf)
{
    struct part *p = d->priv;
    return blk_read(p->parent, p->first + lba, n, buf);
}
static int part_wr(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf)
{
    struct part *p = d->priv;
    return blk_write(p->parent, p->first + lba, n, buf);
}
static int part_flush(struct blkdev *d)
{
    return blk_flush(((struct part *)d->priv)->parent);
}
static void part_release(struct blkdev *d) { kfree(d->priv); }

struct blkdev *blk_partition_create(struct blkdev *parent, const char *name,
                                    uint64_t first, uint64_t sectors)
{
    if (!sectors || first >= parent->sectors || sectors > parent->sectors - first)
        return NULL;
    struct blkdev *d = kzalloc(sizeof(*d));
    struct part *p = kzalloc(sizeof(*p));
    if (!d || !p) {
        kfree(d);
        kfree(p);
        return NULL;
    }
    size_t i = 0;
    while (name[i] && i < BLK_NAME_MAX - 1) {
        d->name[i] = name[i];
        i++;
    }
    p->parent = parent;
    p->first = first;
    d->sectors = sectors;
    d->ssize = blk_ssize(parent);
    d->ro = parent->ro;
    d->rd = part_rd;
    d->wr = part_wr;
    d->flush = part_flush;
    d->release = part_release;
    d->priv = p;
    if (blk_register(d)) {
        kfree(d);
        kfree(p);
        return NULL;
    }
    return d;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool is_fat_bpb(const uint8_t *s)
{
    return (s[0] == 0xEB || s[0] == 0xE9) && s[510] == 0x55 && s[511] == 0xAA &&
           ssize_ok((uint32_t)(s[11] | s[12] << 8)) && s[13] != 0 && s[16] != 0;
}

int blk_scan_partitions(struct blkdev *d)
{
    uint8_t *s = kmalloc(blk_ssize(d));
    if (!s)
        return 0;
    if (d->sectors < 1 || blk_read(d, 0, 1, s) || s[510] != 0x55 || s[511] != 0xAA || is_fat_bpb(s)) {
        kfree(s);
        return 0;
    }
    int found = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = s + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t first = le32(e + 8), count = le32(e + 12);
        if (!type || !count || type == 0xEE || type == 0x05 || type == 0x0F)
            continue;
        char name[BLK_NAME_MAX];
        size_t k = 0;
        for (const char *p = d->name; *p && k < BLK_NAME_MAX - 4; p++)
            name[k++] = *p;
        name[k++] = 'p';
        name[k++] = (char)('1' + i);
        name[k] = 0;
        if (blk_partition_create(d, name, first, count))
            found++;
        else
            klog(LOG_WARN, "blk: %s partition %d (type 0x%x, %u sectors at %u) is out of range, skipped",
                 d->name, i + 1, type, count, first);
    }
    kfree(s);
    return found;
}
