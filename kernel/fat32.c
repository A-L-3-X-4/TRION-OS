#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "blk.h"
#include "vfs.h"
#include "timer.h"

#define FAT_MASK   0x0FFFFFFFu
#define FAT_EOC    0x0FFFFFF8u
#define FAT_BAD    0x0FFFFFF7u
#define FAT_EOC_W  0x0FFFFFFFu
#define DIR_MAX_BYTES (2u * 1024 * 1024)

#define AT_RO   0x01
#define AT_DIR  0x10
#define AT_VOL  0x08
#define AT_LFN  0x0F

struct fatfs {
    struct vfs_sb *sb;
    struct blkdev *dev;
    uint32_t ss, dss;
    uint32_t spc, cbytes, rsvd, nfats, fatsz, data_start, nclusters, root_cl, fsinfo;
    uint32_t active_fat;
    bool     mirror;
    uint32_t free_count, next_free;
    bool     fsinfo_dirty;
};

struct fatnode {
    struct vnode v;
    uint32_t first;
    uint64_t ent_lba;
    uint16_t ent_off;
    uint32_t grp_pos, grp_n;
    uint32_t cur_idx, cur_cl;
    uint32_t rd_idx, rd_pos;
    bool     rd_valid;
};

#define FN(v)  ((struct fatnode *)(v))
#define FSOF(sb) ((struct fatfs *)(sb)->fs)

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static const uint8_t zeros4k[4096];

static int vol_rw(struct fatfs *fs, uint64_t off, void *buf, size_t len, bool wr)
{
    struct blkdev *d = fs->dev;
    uint32_t ds = fs->dss;
    uint8_t *b = buf;
    uint64_t lba = off / ds;
    uint32_t o = (uint32_t)(off % ds);
    int rc;
    if (o || len < ds) {
        size_t c = ds - o < len ? ds - o : len;
        rc = wr ? blk_write_part(d, lba, o, (uint32_t)c, b) : blk_read_part(d, lba, o, (uint32_t)c, b);
        if (rc)
            return rc;
        b += c;
        len -= c;
        lba++;
    }
    size_t full = len / ds;
    if (full) {
        rc = wr ? blk_write(d, lba, (uint32_t)full, b) : blk_read(d, lba, (uint32_t)full, b);
        if (rc)
            return rc;
        b += full * ds;
        len -= full * ds;
        lba += full;
    }
    if (len)
        return wr ? blk_write_part(d, lba, 0, (uint32_t)len, b) : blk_read_part(d, lba, 0, (uint32_t)len, b);
    return 0;
}

static int vol_zero(struct fatfs *fs, uint64_t off, uint64_t len)
{
    while (len) {
        size_t c = len > sizeof(zeros4k) ? sizeof(zeros4k) : (size_t)len;
        int rc = vol_rw(fs, off, (void *)zeros4k, c, true);
        if (rc)
            return rc;
        off += c;
        len -= c;
    }
    return 0;
}

static inline uint64_t sec_off(struct fatfs *fs, uint64_t lba) { return lba * fs->ss; }

static void fat_stamp(uint64_t t, uint16_t *date, uint16_t *time, uint8_t *tenth)
{
    int y, mo, d, h, mi, s;
    unix_to_civil(t, &y, &mo, &d, &h, &mi, &s);
    if (y < 1980) {
        y = 1980; mo = 1; d = 1; h = mi = s = 0;
    }
    if (y > 2107)
        y = 2107;
    *date = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
    *time = (uint16_t)((h << 11) | (mi << 5) | (s / 2));
    if (tenth)
        *tenth = (uint8_t)((s & 1) * 100);
}

static uint64_t fat_unstamp(uint16_t date, uint16_t time)
{
    if (!date)
        return 0;
    int y = 1980 + (date >> 9), mo = (date >> 5) & 15, d = date & 31;
    if (mo < 1 || mo > 12 || d < 1)
        return 0;
    return civil_to_unix(y, mo, d, time >> 11, (time >> 5) & 63, (time & 31) * 2);
}

static inline uint64_t cl_lba(struct fatfs *fs, uint32_t cl)
{
    return fs->data_start + (uint64_t)(cl - 2) * fs->spc;
}
static inline uint64_t cl_off(struct fatfs *fs, uint32_t cl) { return sec_off(fs, cl_lba(fs, cl)); }
static inline uint64_t fat_base(struct fatfs *fs, uint32_t f)
{
    return sec_off(fs, (uint64_t)fs->rsvd + (uint64_t)f * fs->fatsz);
}
static inline bool cl_ok(struct fatfs *fs, uint32_t cl) { return cl >= 2 && cl < fs->nclusters + 2; }

static int fat_get(struct fatfs *fs, uint32_t cl, uint32_t *val)
{
    if (!cl_ok(fs, cl))
        return -E_IO;
    uint8_t b[4];
    int rc = vol_rw(fs, fat_base(fs, fs->active_fat) + (uint64_t)cl * 4, b, 4, false);
    if (rc)
        return rc;
    *val = rd32(b) & FAT_MASK;
    return 0;
}

static int fat_set(struct fatfs *fs, uint32_t cl, uint32_t val)
{
    if (!cl_ok(fs, cl))
        return -E_IO;
    uint8_t b[4];
    uint64_t eoff = (uint64_t)cl * 4;
    int rc = vol_rw(fs, fat_base(fs, fs->active_fat) + eoff, b, 4, false);
    if (rc)
        return rc;
    uint32_t old = rd32(b);
    wr32(b, (old & ~FAT_MASK) | (val & FAT_MASK));
    if (fs->mirror) {
        for (uint32_t f = 0; f < fs->nfats; f++) {
            rc = vol_rw(fs, fat_base(fs, f) + eoff, b, 4, true);
            if (rc)
                return rc;
        }
    } else {
        rc = vol_rw(fs, fat_base(fs, fs->active_fat) + eoff, b, 4, true);
    }
    return rc;
}

static void fsinfo_flush(struct fatfs *fs)
{
    if (!fs->fsinfo_dirty || !fs->fsinfo)
        return;
    uint8_t b[8];
    wr32(b, fs->free_count);
    wr32(b + 4, fs->next_free);
    if (!vol_rw(fs, sec_off(fs, fs->fsinfo) + 488, b, 8, true))
        fs->fsinfo_dirty = false;
}

static int fat_alloc_one(struct fatfs *fs, uint32_t *out)
{
    if (fs->free_count == 0)
        return -E_NOSPC;
    uint32_t total = fs->nclusters;
    uint32_t cl = (fs->next_free >= 2 && fs->next_free < total + 2) ? fs->next_free : 2;
    uint8_t sec[512];
    uint32_t curwin = ~0u;
    for (uint32_t n = 0; n < total; n++) {
        uint32_t s = cl * 4 / sizeof(sec);
        if (s != curwin) {
            int rc = vol_rw(fs, fat_base(fs, fs->active_fat) + (uint64_t)s * sizeof(sec), sec, sizeof(sec), false);
            if (rc)
                return rc;
            curwin = s;
        }
        if ((rd32(sec + cl * 4 % sizeof(sec)) & FAT_MASK) == 0) {
            int rc = fat_set(fs, cl, FAT_EOC_W);
            if (rc)
                return rc;
            fs->free_count--;
            fs->next_free = cl + 1 < total + 2 ? cl + 1 : 2;
            fs->fsinfo_dirty = true;
            *out = cl;
            return 0;
        }
        cl = cl + 1 < total + 2 ? cl + 1 : 2;
    }
    fs->free_count = 0;
    fs->fsinfo_dirty = true;
    return -E_NOSPC;
}

static int zero_cluster(struct fatfs *fs, uint32_t cl)
{
    return vol_zero(fs, cl_off(fs, cl), fs->cbytes);
}

static int fat_free_chain(struct fatfs *fs, uint32_t first)
{
    uint32_t cl = first, guard = 0;
    int result = 0;
    while (cl_ok(fs, cl)) {
        uint32_t next;
        int rc = fat_get(fs, cl, &next);
        if (rc)
            return rc;
        rc = fat_set(fs, cl, 0);
        if (rc)
            return rc;
        fs->free_count++;
        fs->fsinfo_dirty = true;
        if (next >= FAT_EOC)
            break;
        if (!cl_ok(fs, next) || ++guard > fs->nclusters) {
            result = -E_IO;
            break;
        }
        cl = next;
    }
    return result;
}

static int fat_extend(struct fatfs *fs, uint32_t last, uint32_t n, uint32_t *first_new)
{
    uint32_t first = 0, prev = last;
    int rc = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cl;
        rc = fat_alloc_one(fs, &cl);
        if (rc)
            goto rollback;
        rc = zero_cluster(fs, cl);
        if (!rc && prev)
            rc = fat_set(fs, prev, cl);
        if (rc) {
            fat_set(fs, cl, 0);
            fs->free_count++;
            goto rollback;
        }
        if (!first)
            first = cl;
        prev = cl;
    }
    *first_new = first;
    return 0;
rollback:
    if (first) {
        fat_free_chain(fs, first);
        if (last)
            fat_set(fs, last, FAT_EOC_W);
    }
    return rc;
}

static int node_cluster(struct fatnode *n, uint32_t idx, uint32_t *out)
{
    struct fatfs *fs = FSOF(n->v.sb);
    if (!cl_ok(fs, n->first))
        return n->first ? -E_IO : -E_NOENT;
    uint32_t i, cl;
    if (n->cur_cl && idx >= n->cur_idx) {
        i = n->cur_idx;
        cl = n->cur_cl;
    } else {
        i = 0;
        cl = n->first;
    }
    while (i < idx) {
        uint32_t nx;
        int rc = fat_get(fs, cl, &nx);
        if (rc)
            return rc;
        if (nx >= FAT_EOC)
            return -E_NOENT;
        if (!cl_ok(fs, nx))
            return -E_IO;
        cl = nx;
        i++;
    }
    n->cur_idx = idx;
    n->cur_cl = cl;
    *out = cl;
    return 0;
}

#define MAX_RUN_BYTES (64 * 1024)

static int fat_rw(struct fatnode *n, uint64_t off, uint8_t *buf, size_t len, bool wr)
{
    struct fatfs *fs = FSOF(n->v.sb);
    uint32_t cb = fs->cbytes;
    while (len) {
        uint32_t idx = (uint32_t)(off / cb), coff = (uint32_t)(off % cb), cl;
        int rc = node_cluster(n, idx, &cl);
        if (rc)
            return rc == -E_NOENT ? -E_IO : rc;
        uint32_t run = 1;
        while ((uint64_t)run * cb - coff < len && (uint64_t)run * cb < MAX_RUN_BYTES) {
            uint32_t nx;
            rc = fat_get(fs, cl + run - 1, &nx);
            if (rc)
                return rc;
            if (nx != cl + run)
                break;
            run++;
        }
        size_t chunk = (size_t)run * cb - coff;
        if (chunk > len)
            chunk = len;
        rc = vol_rw(fs, cl_off(fs, cl) + coff, buf, chunk, wr);
        if (rc)
            return rc;
        n->cur_idx = idx + run - 1;
        n->cur_cl = cl + run - 1;
        buf += chunk;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

struct dit {
    struct fatnode *dir;
    uint32_t        pos;
    uint8_t         ent[32];
};

static void dit_init(struct dit *it, struct fatnode *dir, uint32_t pos)
{
    it->dir = dir;
    it->pos = pos;
}

static int dit_ent(struct dit *it, uint32_t pos, uint8_t **e, uint64_t *lba, uint16_t *off)
{
    struct fatfs *fs = FSOF(it->dir->v.sb);
    uint32_t cl;
    int rc = node_cluster(it->dir, pos / fs->cbytes, &cl);
    if (rc == -E_NOENT)
        return 0;
    if (rc)
        return rc;
    uint64_t boff = cl_off(fs, cl) + pos % fs->cbytes;
    rc = vol_rw(fs, boff, it->ent, 32, false);
    if (rc)
        return rc;
    *e = it->ent;
    if (lba)
        *lba = boff / fs->ss;
    if (off)
        *off = (uint16_t)(boff % fs->ss);
    return 1;
}

static int u8_dec(const char *s, uint32_t *cp)
{
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if (p[0] >= 0xC2 && p[0] < 0xE0 && (p[1] & 0xC0) == 0x80) {
        *cp = (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
        return 2;
    }
    if (p[0] >= 0xE0 && p[0] < 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        uint32_t c = (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
        if (c < 0x800 || (c >= 0xD800 && c < 0xE000))
            return 0;
        *cp = c;
        return 3;
    }
    if (p[0] >= 0xF0 && p[0] < 0xF5 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        uint32_t c = (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 |
                     (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
        if (c < 0x10000 || c > 0x10FFFF)
            return 0;
        *cp = c;
        return 4;
    }
    return 0;
}

static int u8_enc(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static uint32_t cp_fold(uint32_t c)
{
    if (c < 0x80)
        return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 32;
    if (c >= 0x391 && c <= 0x3A9)
        return c + 32;
    if (c >= 0x410 && c <= 0x42F)
        return c + 32;
    if (c >= 0x400 && c <= 0x40F)
        return c + 80;
    return c;
}

static bool name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        uint32_t ca, cb;
        int na = u8_dec(a, &ca), nb = u8_dec(b, &cb);
        if (!na || !nb) {
            if (*a != *b)
                return false;
            a++;
            b++;
            continue;
        }
        if (cp_fold(ca) != cp_fold(cb))
            return false;
        a += na;
        b += nb;
    }
    return !*a && !*b;
}

static int name_to_utf16(const char *name, uint16_t *u, uint32_t *cps, int *ncp_out)
{
    int nu = 0, ncp = 0;
    size_t len = strlen(name);
    if (!len)
        return -E_INVAL;
    if (len > VFS_NAME_MAX)
        return -E_NAMETOOLONG;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return -E_INVAL;
    for (const char *p = name; *p;) {
        uint32_t c;
        int n = u8_dec(p, &c);
        if (!n)
            return -E_INVAL;
        if (c < 0x20 || c == 0x7F || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
            c == '>' || c == '?' || c == '\\' || c == '|')
            return -E_INVAL;
        if (nu + (c >= 0x10000 ? 2 : 1) > 255)
            return -E_NAMETOOLONG;
        if (c >= 0x10000) {
            c -= 0x10000;
            u[nu++] = (uint16_t)(0xD800 | (c >> 10));
            u[nu++] = (uint16_t)(0xDC00 | (c & 0x3FF));
            cps[ncp++] = c + 0x10000;
        } else {
            u[nu++] = (uint16_t)c;
            cps[ncp++] = c;
        }
        p += n;
    }
    if (name[len - 1] == '.' || name[len - 1] == ' ')
        return -E_INVAL;
    *ncp_out = ncp;
    return nu;
}

static uint8_t sfn_checksum(const uint8_t *s)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        // The checksum every LFN entry carries for its 8.3 name: rotate right by 1 and add the next byte, 11 times, and if it doesn't match the LFN is orphaned. Seriously why is FAT like this
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + s[i]);
    return sum;
}

static bool sfn_char_ok(uint32_t c)
{
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    for (const char *p = "!#$%&'()-@^_`{}~"; *p; p++)
        if ((uint32_t)*p == c)
            return true;
    return false;
}

static void sfn_text(const uint8_t *sfn, uint8_t ntres, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && sfn[i] != ' '; i++) {
        uint8_t c = sfn[i];
        if (i == 0 && c == 0x05)
            c = 0xE5;
        if ((ntres & 0x08) && c >= 'A' && c <= 'Z')
            c += 32;
        n += u8_enc(c, out + n);
    }
    if (sfn[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && sfn[i] != ' '; i++) {
            uint8_t c = sfn[i];
            if ((ntres & 0x10) && c >= 'A' && c <= 'Z')
                c += 32;
            n += u8_enc(c, out + n);
        }
    }
    out[n] = 0;
}

struct dinfo {
    char     name[768];
    char     sname[40];
    uint8_t  sfn[11];
    uint8_t  attr, ntres;
    uint32_t cluster, size;
    uint16_t cdate, ctime, wdate, wtime, adate;
    uint32_t pos;
    uint32_t gpos, gn;
    uint64_t lba;
    uint16_t eoff;
    uint16_t u[272];
};

static int dir_next(struct dit *it, struct dinfo *di)
{
    bool have = false;
    uint8_t cks = 0;
    int expect = 0, total = 0;
    uint32_t lfn_start = 0;
    for (;;) {
        uint8_t *e;
        uint64_t lba;
        uint16_t eo;
        int r = dit_ent(it, it->pos, &e, &lba, &eo);
        if (r <= 0)
            return r;
        uint32_t here = it->pos;
        it->pos += 32;
        if (e[0] == 0x00) {
            it->pos = here;
            return 0;
        }
        if (e[0] == 0xE5) {
            have = false;
            continue;
        }
        if ((e[11] & 0x3F) == AT_LFN) {
            uint8_t ord = e[0] & 0x3F;
            if (e[0] & 0x40) {
                if (!ord || ord > 20) {
                    have = false;
                    continue;
                }
                have = true;
                total = expect = ord;
                cks = e[13];
                lfn_start = here;
            } else if (!have || ord != expect || e[13] != cks) {
                have = false;
                continue;
            }
            if (ord != expect) {
                have = false;
                continue;
            }
            uint16_t *u = di->u + (ord - 1) * 13;
            for (int k = 0; k < 5; k++)  u[k] = rd16(e + 1 + k * 2);
            for (int k = 0; k < 6; k++)  u[5 + k] = rd16(e + 14 + k * 2);
            for (int k = 0; k < 2; k++)  u[11 + k] = rd16(e + 28 + k * 2);
            expect--;
            continue;
        }
        if (e[11] & AT_VOL) {
            have = false;
            continue;
        }
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) {
            have = false;
            continue;
        }

        memcpy(di->sfn, e, 11);
        di->attr = e[11];
        di->ntres = e[12];
        di->cdate = rd16(e + 16);
        di->ctime = rd16(e + 14);
        di->adate = rd16(e + 18);
        di->wtime = rd16(e + 22);
        di->wdate = rd16(e + 24);
        di->cluster = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
        di->size = rd32(e + 28);
        di->pos = here;
        di->lba = lba;
        di->eoff = eo;
        sfn_text(di->sfn, di->ntres, di->sname);

        if (have && expect == 0 && cks == sfn_checksum(di->sfn)) {
            int n = 0, units = total * 13;
            for (int i = 0; i < units && di->u[i]; i++) {
                uint32_t c = di->u[i];
                if (c >= 0xD800 && c < 0xDC00 && i + 1 < units && di->u[i + 1] >= 0xDC00 && di->u[i + 1] < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (di->u[i + 1] - 0xDC00);
                    i++;
                } else if (c >= 0xD800 && c < 0xE000) {
                    c = 0xFFFD;
                }
                n += u8_enc(c, di->name + n);
            }
            di->name[n] = 0;
            di->gpos = lfn_start;
            di->gn = (here - lfn_start) / 32 + 1;
        } else {
            memcpy(di->name, di->sname, sizeof(di->sname));
            di->gpos = here;
            di->gn = 1;
        }
        return 1;
    }
}

static int dir_find(struct fatnode *dir, const char *name, struct dinfo *di)
{
    struct dit it;
    dit_init(&it, dir, 0);
    for (;;) {
        int r = dir_next(&it, di);
        if (r < 0)
            return r;
        if (r == 0)
            return -E_NOENT;
        if (name_eq(di->name, name) || name_eq(di->sname, name))
            return 0;
    }
}

static int dir_has_sfn(struct fatnode *dir, const uint8_t *sfn)
{
    struct dit it;
    dit_init(&it, dir, 0);
    for (;;) {
        uint8_t *e;
        int r = dit_ent(&it, it.pos, &e, NULL, NULL);
        if (r < 0)
            return r;
        if (r == 0 || e[0] == 0x00)
            return 0;
        if (e[0] != 0xE5 && (e[11] & 0x3F) != AT_LFN && !(e[11] & AT_VOL) && !memcmp(e, sfn, 11))
            return 1;
        it.pos += 32;
    }
}

static int node_from(struct vfs_sb *sb, struct dinfo *di, struct vnode **out)
{
    bool fresh;
    uint64_t key = di->lba * FSOF(sb)->ss + di->eoff + 1;
    struct fatnode *n = FN(vnode_get(sb, key, sizeof(*n), &fresh));
    if (!n)
        return -E_NOMEM;
    if (fresh) {
        bool dir = di->attr & AT_DIR;
        n->v.type = dir ? FT_DIR : FT_FILE;
        n->v.attr = di->attr & 0x37;
        n->v.size = dir ? 0 : di->size;
        n->v.ctime = fat_unstamp(di->cdate, di->ctime);
        n->v.mtime = fat_unstamp(di->wdate, di->wtime);
        n->v.atime = fat_unstamp(di->adate, 0);
        n->first = di->cluster;
        n->ent_lba = di->lba;
        n->ent_off = di->eoff;
        n->grp_pos = di->gpos;
        n->grp_n = di->gn;
    }
    *out = &n->v;
    return 0;
}

static int fat_lookup(struct vnode *dv, const char *name, struct vnode **out)
{
    struct dinfo *di = kmalloc(sizeof(*di));
    if (!di)
        return -E_NOMEM;
    int rc = dir_find(FN(dv), name, di);
    if (!rc)
        rc = node_from(dv->sb, di, out);
    kfree(di);
    return rc;
}

static int dir_put(struct fatnode *dir, uint32_t pos, const uint8_t *ent)
{
    struct fatfs *fs = FSOF(dir->v.sb);
    uint32_t cl;
    int rc = node_cluster(dir, pos / fs->cbytes, &cl);
    if (rc)
        return rc == -E_NOENT ? -E_IO : rc;
    return vol_rw(fs, cl_off(fs, cl) + pos % fs->cbytes, (void *)ent, 32, true);
}

static int dir_mark_deleted(struct fatnode *dir, uint32_t pos)
{
    struct fatfs *fs = FSOF(dir->v.sb);
    uint32_t cl;
    int rc = node_cluster(dir, pos / fs->cbytes, &cl);
    if (rc)
        return rc == -E_NOENT ? -E_IO : rc;
    uint8_t del = 0xE5;
    return vol_rw(fs, cl_off(fs, cl) + pos % fs->cbytes, &del, 1, true);
}

static int dir_find_free(struct fatnode *dir, uint32_t need, uint32_t *out)
{
    struct fatfs *fs = FSOF(dir->v.sb);
    struct dit it;
    dit_init(&it, dir, 0);
    uint32_t pos = 0, run = 0, run_start = 0;
    for (;;) {
        uint8_t *e;
        int r = dit_ent(&it, pos, &e, NULL, NULL);
        if (r < 0)
            return r;
        if (r == 0)
            break;
        if (e[0] == 0x00 || e[0] == 0xE5) {
            if (!run)
                run_start = pos;
            if (++run >= need) {
                *out = run_start;
                return 0;
            }
        } else {
            run = 0;
        }
        pos += 32;
    }
    if (!run)
        run_start = pos;
    uint32_t missing = need - run;
    uint32_t add = (missing * 32 + fs->cbytes - 1) / fs->cbytes;
    if (pos + (uint64_t)add * fs->cbytes > DIR_MAX_BYTES)
        return -E_NOSPC;
    uint32_t last, first_new;
    int rc = node_cluster(dir, pos / fs->cbytes - 1, &last);
    if (rc)
        return rc == -E_NOENT ? -E_IO : rc;
    rc = fat_extend(fs, last, add, &first_new);
    if (rc)
        return rc;
    *out = run_start;
    return 0;
}

struct newent {
    uint8_t  attr;
    uint32_t cluster, size;
    uint64_t ctime, mtime, atime;
};

static void lfn_entry(uint8_t *e, uint8_t ord, bool last, const uint16_t *u, int nu, uint8_t cks)
{
    memset(e, 0, 32);
    e[0] = ord | (last ? 0x40 : 0);
    e[11] = AT_LFN;
    e[13] = cks;
    int base = (ord - 1) * 13;
    for (int k = 0; k < 13; k++) {
        int i = base + k;
        uint16_t c = i < nu ? u[i] : (i == nu ? 0x0000 : 0xFFFF);
        int off = k < 5 ? 1 + k * 2 : (k < 11 ? 14 + (k - 5) * 2 : 28 + (k - 11) * 2);
        wr16(e + off, c);
    }
}

static int make_short(struct fatnode *dir, const uint32_t *cps, int ncp, uint8_t sfn[11], uint8_t *ntres,
                      bool *need_lfn)
{
    int lastdot = -1;
    for (int i = 0; i < ncp; i++)
        if (cps[i] == '.')
            lastdot = i;
    int bs = 0, be = ncp, es = ncp, ee = ncp;
    if (lastdot > 0) {
        be = lastdot;
        es = lastdot + 1;
    }

    bool exact = (be - bs) >= 1 && (be - bs) <= 8 && (ee - es) <= 3;
    bool bl = false, bu = false, el = false, eu = false;
    for (int i = bs; i < be && exact; i++) {
        uint32_t c = cps[i];
        if (c >= 'a' && c <= 'z') { bl = true; c -= 32; } else if (c >= 'A' && c <= 'Z') bu = true;
        if (!sfn_char_ok(c))
            exact = false;
    }
    for (int i = es; i < ee && exact; i++) {
        uint32_t c = cps[i];
        if (c >= 'a' && c <= 'z') { el = true; c -= 32; } else if (c >= 'A' && c <= 'Z') eu = true;
        if (!sfn_char_ok(c))
            exact = false;
    }
    if (exact && !(bl && bu) && !(el && eu)) {
        memset(sfn, ' ', 11);
        for (int i = bs; i < be; i++)
            sfn[i - bs] = (uint8_t)(cps[i] >= 'a' && cps[i] <= 'z' ? cps[i] - 32 : cps[i]);
        for (int i = es; i < ee; i++)
            sfn[8 + i - es] = (uint8_t)(cps[i] >= 'a' && cps[i] <= 'z' ? cps[i] - 32 : cps[i]);
        *ntres = (bl ? 0x08 : 0) | (el ? 0x10 : 0);
        *need_lfn = false;
        if (sfn[0] == 0xE5)
            sfn[0] = 0x05;
        int r = dir_has_sfn(dir, sfn);
        if (r < 0)
            return r;
        if (!r)
            return 0;
    }

    *need_lfn = true;
    *ntres = 0;
    if (lastdot == 0) {
        bs = 1;
        es = ee = ncp;
    }
    uint8_t base[8], ext[3];
    int nb = 0, ne = 0;
    bool lossy = (lastdot == 0);
    for (int i = bs; i < be; i++) {
        uint32_t c = cps[i];
        if (c == ' ' || c == '.') { lossy = true; continue; }
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!sfn_char_ok(c)) { c = '_'; lossy = true; }
        if (nb < 8) base[nb++] = (uint8_t)c; else lossy = true;
    }
    for (int i = es; i < ee; i++) {
        uint32_t c = cps[i];
        if (c == ' ' || c == '.') { lossy = true; continue; }
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!sfn_char_ok(c)) { c = '_'; lossy = true; }
        if (ne < 3) ext[ne++] = (uint8_t)c; else lossy = true;
    }
    if (!nb) {
        base[nb++] = '_';
        lossy = true;
    }
    for (uint32_t n = lossy ? 1 : 0; n < 1000000; n++) {
        memset(sfn, ' ', 11);
        int keep = nb;
        char tail[8];
        int tl = 0;
        if (n) {
            tail[tl++] = '~';
            char dig[8];
            int dl = 0;
            for (uint32_t t = n; t; t /= 10)
                dig[dl++] = (char)('0' + t % 10);
            while (dl)
                tail[tl++] = dig[--dl];
            if (keep > 8 - tl)
                keep = 8 - tl;
        }
        memcpy(sfn, base, (size_t)keep);
        memcpy(sfn + keep, tail, (size_t)tl);
        memcpy(sfn + 8, ext, (size_t)ne);
        if (sfn[0] == 0xE5)
            sfn[0] = 0x05;
        int r = dir_has_sfn(dir, sfn);
        if (r < 0)
            return r;
        if (!r)
            return 0;
    }
    return -E_EXIST;
}

static int dir_add(struct fatnode *dir, const char *name, const struct newent *ne, struct dinfo *loc)
{
    struct fatfs *fs = FSOF(dir->v.sb);
    uint16_t u[256];
    uint32_t cps[256];
    int ncp;
    int nu = name_to_utf16(name, u, cps, &ncp);
    if (nu < 0)
        return nu;
    uint8_t sfn[11], ntres;
    bool need_lfn;
    int rc = make_short(dir, cps, ncp, sfn, &ntres, &need_lfn);
    if (rc)
        return rc;
    uint32_t nlfn = need_lfn ? (uint32_t)(nu + 12) / 13 : 0;
    uint32_t pos;
    rc = dir_find_free(dir, nlfn + 1, &pos);
    if (rc)
        return rc;

    uint8_t cks = sfn_checksum(sfn);
    for (uint32_t i = 0; i < nlfn; i++) {
        uint8_t e[32];
        uint8_t ord = (uint8_t)(nlfn - i);
        lfn_entry(e, ord, i == 0, u, nu, cks);
        rc = dir_put(dir, pos + i * 32, e);
        if (rc)
            return rc;
    }
    uint8_t e[32];
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = ne->attr;
    e[12] = ntres;
    uint16_t cd, ct, wd, wt, ad, at;
    uint8_t tenth;
    fat_stamp(ne->ctime, &cd, &ct, &tenth);
    fat_stamp(ne->mtime, &wd, &wt, NULL);
    fat_stamp(ne->atime, &ad, &at, NULL);
    e[13] = tenth;
    wr16(e + 14, ct);
    wr16(e + 16, cd);
    wr16(e + 18, ad);
    wr16(e + 20, (uint16_t)(ne->cluster >> 16));
    wr16(e + 22, wt);
    wr16(e + 24, wd);
    wr16(e + 26, (uint16_t)ne->cluster);
    wr32(e + 28, ne->size);
    rc = dir_put(dir, pos + nlfn * 32, e);
    if (rc)
        return rc;

    uint32_t spos = pos + nlfn * 32, cl;
    rc = node_cluster(dir, spos / fs->cbytes, &cl);
    if (rc)
        return rc == -E_NOENT ? -E_IO : rc;
    uint64_t boff = cl_off(fs, cl) + spos % fs->cbytes;
    loc->lba = boff / fs->ss;
    loc->eoff = (uint16_t)(boff % fs->ss);
    loc->gpos = pos;
    loc->gn = nlfn + 1;
    return 0;
}

static int dir_del_entries(struct fatnode *dir, struct fatnode *n)
{
    struct fatfs *fs = FSOF(dir->v.sb);
    uint8_t del = 0xE5;
    int rc = vol_rw(fs, sec_off(fs, n->ent_lba) + n->ent_off, &del, 1, true);
    if (rc)
        return rc;
    for (uint32_t i = 0; i + 1 < n->grp_n; i++) {
        rc = dir_mark_deleted(dir, n->grp_pos + i * 32);
        if (rc)
            return rc;
    }
    return 0;
}

static int fat_sync_node(struct fatnode *n)
{
    if (!n->ent_lba)
        return 0;
    struct fatfs *fs = FSOF(n->v.sb);
    uint8_t e[32];
    uint64_t eoff = sec_off(fs, n->ent_lba) + n->ent_off;
    int rc = vol_rw(fs, eoff, e, 32, false);
    if (rc)
        return rc;
    uint16_t wd, wt, ad, at;
    fat_stamp(n->v.mtime, &wd, &wt, NULL);
    fat_stamp(n->v.atime ? n->v.atime : n->v.mtime, &ad, &at, NULL);
    e[11] = (uint8_t)((e[11] & ~0x37) | (n->v.attr & 0x37)) | (n->v.type == FT_DIR ? AT_DIR : 0);
    wr16(e + 18, ad);
    wr16(e + 20, (uint16_t)(n->first >> 16));
    wr16(e + 22, wt);
    wr16(e + 24, wd);
    wr16(e + 26, (uint16_t)n->first);
    wr32(e + 28, n->v.type == FT_DIR ? 0 : (uint32_t)n->v.size);
    return vol_rw(fs, eoff, e, 32, true);
}

static int fat_create(struct vnode *dv, const char *name, uint32_t type, struct vnode **out)
{
    struct fatnode *dir = FN(dv);
    struct fatfs *fs = FSOF(dv->sb);
    struct dinfo *di = kmalloc(sizeof(*di));
    if (!di)
        return -E_NOMEM;
    int rc = dir_find(dir, name, di);
    if (rc != -E_NOENT) {
        kfree(di);
        return rc ? rc : -E_EXIST;
    }
    uint64_t now = wall_unix();
    struct newent ne = { .attr = type == FT_DIR ? AT_DIR : 0x20, .cluster = 0, .size = 0,
                         .ctime = now, .mtime = now, .atime = now };
    uint32_t cl = 0;
    if (type == FT_DIR) {
        rc = fat_extend(fs, 0, 1, &cl);
        if (rc)
            goto out;
        uint8_t e[32];
        uint32_t parent = (dir->first == fs->root_cl) ? 0 : dir->first;
        uint16_t d, t;
        uint8_t tenth;
        fat_stamp(now, &d, &t, &tenth);
        for (int i = 0; i < 2; i++) {
            memset(e, 0, 32);
            memset(e, ' ', 11);
            e[0] = '.';
            if (i)
                e[1] = '.';
            e[11] = AT_DIR;
            wr16(e + 14, t); wr16(e + 16, d); wr16(e + 18, d); wr16(e + 22, t); wr16(e + 24, d);
            uint32_t c = i ? parent : cl;
            wr16(e + 20, (uint16_t)(c >> 16));
            wr16(e + 26, (uint16_t)c);
            rc = vol_rw(fs, cl_off(fs, cl) + i * 32, e, 32, true);
            if (rc)
                goto fail_cluster;
        }
        ne.cluster = cl;
    }
    rc = dir_add(dir, name, &ne, di);
    if (rc) {
fail_cluster:
        if (cl)
            fat_free_chain(fs, cl);
        goto out;
    }
    di->attr = ne.attr;
    di->cluster = ne.cluster;
    di->size = 0;
    {
        uint16_t d, t;
        fat_stamp(now, &d, &t, NULL);
        di->cdate = di->wdate = di->adate = d;
        di->ctime = di->wtime = t;
    }
    dir->rd_valid = false;
    rc = node_from(dv->sb, di, out);
out:
    fsinfo_flush(fs);
    kfree(di);
    return rc;
}

static int fat_remove(struct vnode *dv, struct vnode *v)
{
    struct fatnode *dir = FN(dv), *n = FN(v);
    struct fatfs *fs = FSOF(dv->sb);
    int rc = 0;
    if (v->type == FT_DIR) {
        struct dinfo *di = kmalloc(sizeof(*di));
        if (!di)
            return -E_NOMEM;
        struct dit it;
        dit_init(&it, n, 0);
        int r = dir_next(&it, di);
        kfree(di);
        if (r < 0)
            return r;
        if (r > 0)
            return -E_NOTEMPTY;
    }
    rc = dir_del_entries(dir, n);
    if (!rc && n->first)
        rc = fat_free_chain(fs, n->first);
    dir->rd_valid = false;
    n->first = 0;
    n->v.size = 0;
    n->cur_cl = 0;
    fsinfo_flush(fs);
    return rc;
}

static int dotdot_of(struct fatfs *fs, uint32_t c, uint32_t *parent)
{
    uint8_t e[32];
    int rc = vol_rw(fs, cl_off(fs, c) + 32, e, 32, false);
    if (rc)
        return rc;
    if (e[0] != '.' || e[1] != '.')
        return -E_IO;
    *parent = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
    return 0;
}

static int fat_rename(struct vnode *odv, struct vnode *v, struct vnode *ndv, const char *nname)
{
    struct fatnode *odir = FN(odv), *ndir = FN(ndv), *n = FN(v);
    struct fatfs *fs = FSOF(odv->sb);
    struct dinfo *di = kmalloc(sizeof(*di));
    if (!di)
        return -E_NOMEM;

    uint16_t u[256];
    uint32_t cps[256];
    int ncp;
    int rc = name_to_utf16(nname, u, cps, &ncp);
    if (rc < 0)
        goto out;

    bool same = false;
    rc = dir_find(ndir, nname, di);
    if (!rc) {
        same = (di->lba == n->ent_lba && di->eoff == n->ent_off);
        if (!same) {
            rc = -E_EXIST;
            goto out;
        }
    } else if (rc != -E_NOENT) {
        goto out;
    }

    if (v->type == FT_DIR) {
        uint32_t c = ndir->first == fs->root_cl ? 0 : ndir->first, guard = 0;
        while (c) {
            if (c == n->first) {
                rc = -E_INVAL;
                goto out;
            }
            uint32_t p;
            rc = dotdot_of(fs, c, &p);
            if (rc)
                goto out;
            c = p;
            if (++guard > fs->nclusters) {
                rc = -E_IO;
                goto out;
            }
        }
    }

    struct newent ne = { .attr = (uint8_t)(n->v.attr | (v->type == FT_DIR ? AT_DIR : 0)), .cluster = n->first,
                         .size = (uint32_t)n->v.size, .ctime = n->v.ctime, .mtime = n->v.mtime,
                         .atime = n->v.atime };
    if (same) {
        rc = dir_del_entries(odir, n);
        if (rc)
            goto out;
        rc = dir_add(ndir, nname, &ne, di);
    } else {
        rc = dir_add(ndir, nname, &ne, di);
        if (!rc)
            rc = dir_del_entries(odir, n);
    }
    if (rc)
        goto out;

    if (v->type == FT_DIR && odir->first != ndir->first && n->first) {
        uint32_t parent = ndir->first == fs->root_cl ? 0 : ndir->first;
        uint8_t e[32];
        uint64_t doff = cl_off(fs, n->first) + 32;
        rc = vol_rw(fs, doff, e, 32, false);
        if (!rc) {
            wr16(e + 20, (uint16_t)(parent >> 16));
            wr16(e + 26, (uint16_t)parent);
            rc = vol_rw(fs, doff, e, 32, true);
        }
    }
    n->ent_lba = di->lba;
    n->ent_off = di->eoff;
    n->grp_pos = di->gpos;
    n->grp_n = di->gn;
    n->v.key = di->lba * fs->ss + di->eoff + 1;
    odir->rd_valid = ndir->rd_valid = false;
out:
    fsinfo_flush(fs);
    kfree(di);
    return rc;
}

static int fat_grow(struct fatnode *n, uint64_t newsize)
{
    struct fatfs *fs = FSOF(n->v.sb);
    uint32_t cb = fs->cbytes;
    uint64_t old = n->v.size;
    uint32_t have = (uint32_t)((old + cb - 1) / cb), need = (uint32_t)((newsize + cb - 1) / cb);
    uint32_t last = 0;
    int rc;
    if (have) {
        rc = node_cluster(n, have - 1, &last);
        if (rc)
            return rc == -E_NOENT ? -E_IO : rc;
    }
    if (need > have) {
        uint32_t first_new;
        rc = fat_extend(fs, last, need - have, &first_new);
        if (rc)
            return rc;
        if (!have) {
            n->first = first_new;
            n->cur_cl = 0;
        }
    }
    if (old % cb && have) {
        uint64_t end = newsize - (uint64_t)(have - 1) * cb;
        if (end > cb)
            end = cb;
        uint32_t from = (uint32_t)(old % cb);
        uint64_t at = (uint64_t)(have - 1) * cb + from;
        while (end > from) {
            size_t c = end - from > sizeof(zeros4k) ? sizeof(zeros4k) : (size_t)(end - from);
            rc = fat_rw(n, at, (uint8_t *)zeros4k, c, true);
            if (rc)
                return rc;
            at += c;
            from += (uint32_t)c;
        }
    }
    n->v.size = newsize;
    n->v.mtime = wall_unix();
    n->v.attr |= 0x20;
    return fat_sync_node(n);
}

static int fat_truncate(struct vnode *v, uint64_t size)
{
    struct fatnode *n = FN(v);
    struct fatfs *fs = FSOF(v->sb);
    if (v->type != FT_FILE)
        return -E_ISDIR;
    if (size > 0xFFFFFFFFull)
        return -E_FBIG;
    int rc = 0;
    if (size > v->size) {
        rc = fat_grow(n, size);
    } else if (size < v->size) {
        uint32_t cb = fs->cbytes, keep = (uint32_t)((size + cb - 1) / cb);
        if (keep == 0) {
            uint32_t first = n->first;
            n->first = 0;
            n->cur_cl = 0;
            v->size = 0;
            rc = fat_sync_node(n);
            if (!rc && first)
                rc = fat_free_chain(fs, first);
        } else {
            uint32_t cl, next;
            rc = node_cluster(n, keep - 1, &cl);
            if (rc == -E_NOENT)
                rc = -E_IO;
            if (!rc)
                rc = fat_get(fs, cl, &next);
            if (!rc)
                rc = fat_set(fs, cl, FAT_EOC_W);
            if (!rc) {
                n->cur_cl = 0;
                v->size = size;
                v->mtime = wall_unix();
                v->attr |= 0x20;
                rc = fat_sync_node(n);
                if (!rc && cl_ok(fs, next))
                    rc = fat_free_chain(fs, next);
            }
        }
    } else {
        return 0;
    }
    fsinfo_flush(fs);
    return rc;
}

static long fat_read(struct vnode *v, uint64_t off, void *buf, size_t n)
{
    if (v->type != FT_FILE)
        return -E_ISDIR;
    if (off >= v->size || !n)
        return 0;
    if (n > v->size - off)
        n = (size_t)(v->size - off);
    int rc = fat_rw(FN(v), off, buf, n, false);
    return rc ? rc : (long)n;
}

static long fat_write(struct vnode *v, uint64_t off, const void *buf, size_t n)
{
    struct fatnode *fn = FN(v);
    struct fatfs *fs = FSOF(v->sb);
    if (v->type != FT_FILE)
        return -E_ISDIR;
    if (!n)
        return 0;
    if (off > 0xFFFFFFFFull || n > 0xFFFFFFFFull - off)
        return -E_FBIG;
    uint64_t end = off + n;
    int rc = 0;
    if (end > v->size)
        rc = fat_grow(fn, end);
    else {
        uint64_t now = wall_unix();
        if (now / 2 != v->mtime / 2 || !(v->attr & 0x20)) {
            v->mtime = now;
            v->attr |= 0x20;
            rc = fat_sync_node(fn);
        }
    }
    if (!rc)
        rc = fat_rw(fn, off, (uint8_t *)buf, n, true);
    fsinfo_flush(fs);
    return rc ? rc : (long)n;
}

static int fat_readdir(struct vnode *dv, uint32_t index, struct fs_dirent *out)
{
    struct fatnode *dir = FN(dv);
    struct dinfo *di = kmalloc(sizeof(*di));
    if (!di)
        return -E_NOMEM;
    struct dit it;
    uint32_t i = 0, pos = 0;
    if (dir->rd_valid && index >= dir->rd_idx) {
        i = dir->rd_idx;
        pos = dir->rd_pos;
    }
    dit_init(&it, dir, pos);
    int rc;
    for (;;) {
        rc = dir_next(&it, di);
        if (rc < 0)
            break;
        if (rc == 0) {
            rc = -E_NOENT;
            break;
        }
        if (strlen(di->name) > VFS_NAME_MAX)
            continue;
        if (i == index) {
            memset(out, 0, sizeof(*out));
            memcpy(out->name, di->name, strlen(di->name) + 1);
            out->type = (di->attr & AT_DIR) ? FT_DIR : FT_FILE;
            out->attr = di->attr & 0x37;
            out->size = (di->attr & AT_DIR) ? 0 : di->size;
            dir->rd_idx = index + 1;
            dir->rd_pos = it.pos;
            dir->rd_valid = true;
            rc = 0;
            break;
        }
        i++;
    }
    kfree(di);
    return rc;
}

static int fat_statfs(struct vfs_sb *sb, struct fs_statfs *out)
{
    struct fatfs *fs = FSOF(sb);
    out->total_bytes = (uint64_t)fs->nclusters * fs->cbytes;
    out->free_bytes = (uint64_t)fs->free_count * fs->cbytes;
    out->cluster_bytes = fs->cbytes;
    memcpy(out->fs, "FAT32", 6);
    memcpy(out->label, sb->label, sizeof(out->label));
    return 0;
}

static int fat_mount(struct vfs_sb *sb)
{
    struct blkdev *dev = sb->dev;
    uint8_t s[512];
    uint32_t dss = blk_ssize(dev);
    if (blk_read_part(dev, 0, 0, sizeof(s), s))
        return -E_IO;
    if (!((s[0] == 0xEB || s[0] == 0xE9) && s[510] == 0x55 && s[511] == 0xAA))
        return -E_INVAL;
    uint32_t bps = rd16(s + 11);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !s[13] || (s[13] & (s[13] - 1)) || !s[16])
        return -E_INVAL;
    if (rd16(s + 17) != 0 || rd16(s + 22) != 0 || !rd32(s + 36)) {
        klog(LOG_INFO, "fat32: %s looks like FAT12/16, not supported", dev->name);
        return -E_INVAL;
    }
    uint32_t spc = s[13], rsvd = rd16(s + 14), nfats = s[16];
    uint32_t tot = rd16(s + 19) ? rd16(s + 19) : rd32(s + 32), fatsz = rd32(s + 36);
    if (nfats > 2 || rsvd < 1 || !tot || (uint64_t)tot * bps > dev->sectors * dss)
        return -E_INVAL;
    uint64_t data_start = (uint64_t)rsvd + (uint64_t)nfats * fatsz;
    if (data_start >= tot)
        return -E_INVAL;
    uint32_t ncl = (uint32_t)((tot - data_start) / spc);
    if (ncl > (uint64_t)fatsz * (bps / 4) - 2)
        ncl = (uint32_t)((uint64_t)fatsz * (bps / 4) - 2);
    if (ncl > 0x0FFFFFF5)
        ncl = 0x0FFFFFF5;
    if (ncl < 65525) {
        klog(LOG_INFO, "fat32: %s has %u clusters (< 65525): not FAT32", dev->name, ncl);
        return -E_INVAL;
    }
    uint32_t root = rd32(s + 44);
    if (root < 2 || root >= ncl + 2)
        return -E_INVAL;

    struct fatfs *fs = kzalloc(sizeof(*fs));
    if (!fs)
        return -E_NOMEM;
    fs->sb = sb;
    fs->dev = dev;
    fs->spc = spc;
    fs->ss = bps;
    fs->dss = dss;
    fs->cbytes = spc * bps;
    fs->rsvd = rsvd;
    fs->nfats = nfats;
    fs->fatsz = fatsz;
    fs->data_start = (uint32_t)data_start;
    fs->nclusters = ncl;
    fs->root_cl = root;
    uint16_t ext = rd16(s + 40);
    fs->mirror = !(ext & 0x80);
    fs->active_fat = fs->mirror ? 0 : (ext & 0x0F);
    if (fs->active_fat >= nfats)
        fs->active_fat = 0;
    fs->fsinfo = rd16(s + 48);

    if (s[66] == 0x29) {
        int n = 0;
        for (int i = 0; i < 11; i++)
            sb->label[n++] = (char)s[71 + i];
        while (n && sb->label[n - 1] == ' ')
            n--;
        sb->label[n] = 0;
        if (!strcmp(sb->label, "NO NAME"))
            sb->label[0] = 0;
    }

    bool have_info = false;
    if (fs->fsinfo >= 1 && fs->fsinfo < rsvd && !vol_rw(fs, sec_off(fs, fs->fsinfo), s, sizeof(s), false) &&
        rd32(s) == 0x41615252 && rd32(s + 484) == 0x61417272 && rd32(s + 508) == 0xAA550000) {
        uint32_t fc = rd32(s + 488), nf = rd32(s + 492);
        if (fc <= ncl) {
            fs->free_count = fc;
            fs->next_free = (nf >= 2 && nf < ncl + 2) ? nf : 2;
            have_info = true;
        }
    } else {
        fs->fsinfo = 0;
    }
    if (!have_info) {
        uint32_t freec = 0;
        uint32_t scanned = 0;
        uint64_t fat_bytes = (uint64_t)fatsz * bps;
        for (uint64_t pos = 0; pos < fat_bytes && scanned < ncl + 2; pos += sizeof(s)) {
            if (vol_rw(fs, fat_base(fs, fs->active_fat) + pos, s, sizeof(s), false)) {
                kfree(fs);
                return -E_IO;
            }
            for (int k = 0; k < 128 && scanned < ncl + 2; k++, scanned++)
                if (scanned >= 2 && !(rd32(s + k * 4) & FAT_MASK))
                    freec++;
        }
        fs->free_count = freec;
        fs->next_free = 2;
        klog(LOG_INFO, "fat32: %s has no valid FSInfo, counted %u free clusters", dev->name, freec);
    }

    sb->fs = fs;
    bool fresh;
    struct fatnode *rn = FN(vnode_get(sb, 0, sizeof(*rn), &fresh));
    if (!rn) {
        kfree(fs);
        sb->fs = NULL;
        return -E_NOMEM;
    }
    rn->v.type = FT_DIR;
    rn->v.attr = FA_DIR;
    rn->first = root;
    sb->root = &rn->v;
    return 0;
}

static void fat_unmount(struct vfs_sb *sb)
{
    struct fatfs *fs = FSOF(sb);
    if (fs) {
        fs->fsinfo_dirty = true;
        fsinfo_flush(fs);
        kfree(fs);
    }
    sb->fs = NULL;
}

const struct vfs_ops fat32_ops = {
    .name = "FAT32",
    .mount = fat_mount,
    .unmount = fat_unmount,
    .lookup = fat_lookup,
    .create = fat_create,
    .remove = fat_remove,
    .rename = fat_rename,
    .readdir = fat_readdir,
    .read = fat_read,
    .write = fat_write,
    .truncate = fat_truncate,
    .statfs = fat_statfs,
};

int fat32_format(struct blkdev *dev, const char *label, uint32_t spc)
{
    return fat32_format_ss(dev, label, spc, 0);
}

int fat32_format_ss(struct blkdev *dev, const char *label, uint32_t spc, uint32_t ss)
{
    uint32_t dss = blk_ssize(dev);
    if (!ss)
        ss = dss;
    if (ss < 512 || ss > 4096 || (ss & (ss - 1)))
        return -E_INVAL;
    uint64_t total = dev->sectors * dss / ss;
    if (total > 0xFFFFFFFFull)
        total = 0xFFFFFFFFull;
    if (!spc) {
        uint64_t t512 = total * ss / 512;
        uint32_t c512 = t512 <= 532480 ? 1 : t512 <= 16777216 ? 8 : t512 <= 33554432 ? 16 :
                        t512 <= 67108864 ? 32 : 64;
        spc = c512 * 512 / ss;
        if (!spc)
            spc = 1;
    }
    if (spc > 128 || (spc & (spc - 1)))
        return -E_INVAL;
    const uint32_t rsvd = 32, nfats = 2;
    if (total <= rsvd)
        return -E_INVAL;
    uint32_t epf = ss / 4;
    uint32_t tmp2 = (2 * epf * spc + nfats) / 2;
    uint32_t fatsz = (uint32_t)((total - rsvd + tmp2 - 1) / tmp2);
    uint64_t data = total - rsvd - (uint64_t)nfats * fatsz;
    uint32_t ncl = (uint32_t)(data / spc);
    if (ncl < 65525 || ncl > 0x0FFFFFF5)
        return -E_INVAL;
    uint32_t data_start = rsvd + nfats * fatsz;

    struct fatfs f = { .dev = dev, .ss = ss, .dss = dss };
    uint8_t *s = kzalloc(ss);
    if (!s)
        return -E_NOMEM;
    int rc = vol_zero(&f, 0, (uint64_t)rsvd * ss);
    s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
    memcpy(s + 3, "INFINITY", 8);
    wr16(s + 11, (uint16_t)ss);
    s[13] = (uint8_t)spc;
    wr16(s + 14, rsvd);
    s[16] = nfats;
    s[21] = 0xF8;
    wr16(s + 24, 63);
    wr16(s + 26, 255);
    wr32(s + 32, (uint32_t)total);
    wr32(s + 36, fatsz);
    wr32(s + 44, 2);
    wr16(s + 48, 1);
    wr16(s + 50, 6);
    s[64] = 0x80;
    s[66] = 0x29;
    wr32(s + 67, (uint32_t)clock_ns() ^ 0x1F5C0DE5u);
    memset(s + 71, ' ', 11);
    for (int i = 0; i < 11 && label && label[i]; i++)
        s[71 + i] = (uint8_t)(label[i] >= 'a' && label[i] <= 'z' ? label[i] - 32 : label[i]);
    memcpy(s + 82, "FAT32   ", 8);
    s[510] = 0x55; s[511] = 0xAA;
    if (ss > 512) {
        s[ss - 2] = 0x55; s[ss - 1] = 0xAA;
    }
    if (!rc) rc = vol_rw(&f, 0, s, ss, true);
    if (!rc) rc = vol_rw(&f, sec_off(&f, 6), s, ss, true);
    uint8_t label_ent[32];
    memset(label_ent, 0, sizeof(label_ent));
    memcpy(label_ent, s + 71, 11);
    label_ent[11] = AT_VOL;

    memset(s, 0, ss);
    wr32(s, 0x41615252);
    wr32(s + 484, 0x61417272);
    wr32(s + 488, ncl - 1);
    wr32(s + 492, 3);
    wr32(s + 508, 0xAA550000);
    if (!rc) rc = vol_rw(&f, sec_off(&f, 1), s, ss, true);
    if (!rc) rc = vol_rw(&f, sec_off(&f, 7), s, ss, true);
    kfree(s);

    for (uint32_t f_i = 0; f_i < nfats && !rc; f_i++) {
        uint64_t base = sec_off(&f, rsvd + (uint64_t)f_i * fatsz);
        rc = vol_zero(&f, base, (uint64_t)fatsz * ss);
        if (rc)
            break;
        uint8_t head[12];
        wr32(head, 0x0FFFFFF8);
        wr32(head + 4, 0x0FFFFFFF);
        wr32(head + 8, 0x0FFFFFFF);
        rc = vol_rw(&f, base, head, sizeof(head), true);
    }
    if (!rc)
        rc = vol_zero(&f, sec_off(&f, data_start), (uint64_t)spc * ss);
    if (!rc && label && label[0])
        rc = vol_rw(&f, sec_off(&f, data_start), label_ent, sizeof(label_ent), true);
    if (!rc)
        rc = blk_flush(dev);
    return rc;
}
