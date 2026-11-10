#include "kernel.h"
#include "mm.h"
#include "io.h"
#include "timer.h"
#include "blk.h"
#include "abi.h"

#define R_DATA   0
#define R_ERR    1
#define R_COUNT  2
#define R_LBA0   3
#define R_LBA1   4
#define R_LBA2   5
#define R_DRIVE  6
#define R_STATUS 7
#define R_CMD    7

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_DRDY 0x40
#define ST_BSY  0x80

#define CMD_READ     0x20
#define CMD_READ_EXT 0x24
#define CMD_WRITE    0x30
#define CMD_WRITE_EXT 0x34
#define CMD_FLUSH    0xE7
#define CMD_FLUSH_EXT 0xEA
#define CMD_IDENTIFY 0xEC

#define ATA_TIMEOUT_MS 5000

bool g_ata_force_lba48;

struct ata {
    uint16_t io, ctl;
    uint8_t  slave;
    bool     lba48;
    uint32_t ssize;
};

static inline void settle(uint16_t ctl)
{
    for (int i = 0; i < 4; i++)
        inb(ctl);
}

static int wait_idle(struct ata *a)
{
    uint64_t end = clock_ns() + ATA_TIMEOUT_MS * 1000000ULL;
    for (uint64_t spin = 0; spin < 50000000ULL; spin++) {
        uint8_t st = inb(a->io + R_STATUS);
        if (!(st & ST_BSY))
            return st;
        if ((spin & 0xFFF) == 0xFFF && clock_ns() > end)
            break;
    }
    return -E_IO;
}

static void select_drive(struct ata *a, uint8_t extra)
{
    outb(a->io + R_DRIVE, 0xA0 | extra | (a->slave << 4));
    settle(a->ctl);
}

static int ata_xfer(struct blkdev *d, bool write, uint64_t lba, uint32_t n, void *buf)
{
    struct ata *a = d->priv;
    uint8_t *b = buf;
    while (n) {
        uint32_t cnt = n > 128 ? 128 : n;
        bool ext = a->lba48 && (g_ata_force_lba48 || lba + cnt > (1ULL << 28));
        if (!ext && lba + cnt > (1ULL << 28))
            return -E_INVAL;

        if (ext)
            select_drive(a, 0x40);
        else
            select_drive(a, 0x40 | ((lba >> 24) & 0x0F));
        int st = wait_idle(a);
        if (st < 0 || !(st & ST_DRDY))
            return -E_IO;

        if (ext) {
            outb(a->io + R_COUNT, (uint8_t)(cnt >> 8));
            outb(a->io + R_LBA0, (uint8_t)(lba >> 24));
            outb(a->io + R_LBA1, (uint8_t)(lba >> 32));
            outb(a->io + R_LBA2, (uint8_t)(lba >> 40));
        }
        outb(a->io + R_COUNT, (uint8_t)cnt);
        outb(a->io + R_LBA0, (uint8_t)lba);
        outb(a->io + R_LBA1, (uint8_t)(lba >> 8));
        outb(a->io + R_LBA2, (uint8_t)(lba >> 16));
        outb(a->io + R_CMD, write ? (ext ? CMD_WRITE_EXT : CMD_WRITE) : (ext ? CMD_READ_EXT : CMD_READ));

        for (uint32_t i = 0; i < cnt; i++) {
            settle(a->ctl);
            st = wait_idle(a);
            if (st < 0 || (st & (ST_ERR | ST_DF)) || !(st & ST_DRQ))
                return -E_IO;
            if (write)
                outsw(a->io + R_DATA, b, a->ssize / 2);
            else
                insw(a->io + R_DATA, b, a->ssize / 2);
            b += a->ssize;
        }
        if (write) {
            settle(a->ctl);
            st = wait_idle(a);
            if (st < 0 || (st & (ST_ERR | ST_DF)))
                return -E_IO;
        }
        lba += cnt;
        n -= cnt;
    }
    return 0;
}

static int ata_rd(struct blkdev *d, uint64_t lba, uint32_t n, void *buf)
{
    return ata_xfer(d, false, lba, n, buf);
}
static int ata_wr(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf)
{
    return ata_xfer(d, true, lba, n, (void *)buf);
}

static int ata_flush(struct blkdev *d)
{
    struct ata *a = d->priv;
    select_drive(a, 0x40);
    int st = wait_idle(a);
    if (st < 0)
        return -E_IO;
    outb(a->io + R_CMD, a->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
    settle(a->ctl);
    st = wait_idle(a);
    return (st < 0 || (st & (ST_ERR | ST_DF))) ? -E_IO : 0;
}

static void ata_release(struct blkdev *d) { kfree(d->priv); }

static int identify(struct ata *a, uint16_t id[256])
{
    select_drive(a, 0);
    outb(a->io + R_COUNT, 0);
    outb(a->io + R_LBA0, 0);
    outb(a->io + R_LBA1, 0);
    outb(a->io + R_LBA2, 0);
    outb(a->io + R_CMD, CMD_IDENTIFY);
    settle(a->ctl);
    uint8_t st = inb(a->io + R_STATUS);
    if (st == 0 || st == 0xFF)
        return 0;
    int w = wait_idle(a);
    if (w < 0)
        return 0;
    if (inb(a->io + R_LBA1) || inb(a->io + R_LBA2))
        return 0;
    for (int i = 0; i < 100000; i++) {
        st = inb(a->io + R_STATUS);
        if (st & ST_ERR)
            return 0;
        if (st & ST_DRQ)
            break;
        if (i == 99999)
            return 0;
    }
    insw(a->io + R_DATA, id, 256);
    return 1;
}

int ata_init(void)
{
    static const struct { uint16_t io, ctl; } chan[2] = { { 0x1F0, 0x3F6 }, { 0x170, 0x376 } };
    int found = 0;
    for (int c = 0; c < 2; c++) {
        if (inb(chan[c].io + R_STATUS) == 0xFF)
            continue;
        outb(chan[c].ctl, 0x02);
        for (int s = 0; s < 2; s++) {
            struct ata probe = { chan[c].io, chan[c].ctl, (uint8_t)s, false, BLK_SECTOR };
            uint16_t id[256];
            if (!identify(&probe, id))
                continue;
            uint64_t sectors = id[60] | ((uint32_t)id[61] << 16);
            bool lba = id[49] & (1 << 9);
            bool l48 = id[83] & (1 << 10);
            if (l48) {
                uint64_t s48 = id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) |
                               ((uint64_t)id[103] << 48);
                if (s48)
                    sectors = s48;
            }
            uint32_t ssize = BLK_SECTOR;
            if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1 << 12))) {
                uint32_t words = id[117] | ((uint32_t)id[118] << 16);
                ssize = words * 2;
            }
            if (ssize < BLK_SECTOR || ssize > BLK_SECTOR_MAX || (ssize & (ssize - 1))) {
                klog(LOG_WARN, "ata: %s %s reports a %u-byte logical sector, ignored", c ? "secondary" : "primary",
                     s ? "slave" : "master", ssize);
                continue;
            }
            if (!lba || !sectors) {
                klog(LOG_WARN, "ata: %s %s has no LBA support, ignored", c ? "secondary" : "primary",
                     s ? "slave" : "master");
                continue;
            }
            struct blkdev *d = kzalloc(sizeof(*d));
            struct ata *a = kzalloc(sizeof(*a));
            if (!d || !a) {
                kfree(d);
                kfree(a);
                return found;
            }
            *a = probe;
            a->lba48 = l48;
            a->ssize = ssize;
            d->name[0] = 'h'; d->name[1] = 'd';
            d->name[2] = (char)('0' + found);
            d->sectors = sectors;
            d->ssize = ssize;
            d->rd = ata_rd;
            d->wr = ata_wr;
            d->flush = ata_flush;
            d->release = ata_release;
            d->priv = a;
            d->cached = true;
            for (int i = 0; i < 20; i++) {
                d->model[i * 2] = (char)(id[27 + i] >> 8);
                d->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
            }
            int e = 40;
            while (e > 0 && (d->model[e - 1] == ' ' || !d->model[e - 1]))
                e--;
            d->model[e] = 0;
            if (blk_register(d)) {
                kfree(d);
                kfree(a);
                continue;
            }
            klog(LOG_OK, "ata: %s = %s %s, \"%s\", %lu MiB, LBA%s, %u-byte sectors", d->name, c ? "secondary" : "primary",
                 s ? "slave" : "master", d->model, (uint64_t)(sectors * ssize >> 20), l48 ? "48" : "28", ssize);
            found++;
        }
    }
    return found;
}
