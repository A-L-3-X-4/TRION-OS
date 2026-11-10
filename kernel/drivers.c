#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"
#include "blk.h"
#include "drv.h"

static inline void lock(void)   { preempt_disable(); }
static inline void unlock(void) { preempt_enable(); }

static void filter_defaults(struct driver *d)
{
    for (unsigned i = 0; i < IRP_MJ_MAX; i++)
        d->dispatch[i] = io_pass_down;
}

static const struct ace acl_all_rw[] = {
    { ACE_ALLOW, 0, 0, SID_EVERYONE, DEV_READ_DATA | DEV_WRITE_DATA | DEV_IOCTL | ACCESS_READ_CONTROL },
};

static int null_read(struct device *dev, struct irp *irp)
{
    (void)dev;
    irp->info = 0;
    return io_complete_request(irp, 0);
}

static int zero_read(struct device *dev, struct irp *irp)
{
    (void)dev;
    memset(irp->buf, 0, io_cur_sp(irp)->len);
    irp->info = io_cur_sp(irp)->len;
    return io_complete_request(irp, 0);
}

static int sink_write(struct device *dev, struct irp *irp)
{
    (void)dev;
    irp->info = io_cur_sp(irp)->len;
    return io_complete_request(irp, 0);
}

static int no_ioctl(struct device *dev, struct irp *irp)
{
    (void)dev;
    return io_complete_request(irp, -E_NOTTY);
}

struct mem_ext {
    uint8_t           *data;
    uint32_t           fail_next;
    struct mem_counters c;
};

static struct driver *mem_drv;

static int mem_rw(struct device *dev, struct irp *irp)
{
    struct mem_ext *m = dev->ext;
    struct irp_sp *sp = io_cur_sp(irp);
    bool wr = sp->major == IRP_MJ_WRITE;
    lock();
    if (wr) m->c.writes++; else m->c.reads++;
    bool fail = m->fail_next > 0;
    if (fail) {
        m->fail_next--;
        m->c.failed++;
    }
    unlock();
    if (fail) {
        irp->info = 0;
        return io_complete_request(irp, -E_IO);
    }
    uint64_t size = dev->size;
    if (sp->off >= size) {
        irp->info = 0;
        return io_complete_request(irp, wr && sp->len ? -E_NOSPC : 0);
    }
    uint64_t n = size - sp->off < sp->len ? size - sp->off : sp->len;
    if (wr)
        memcpy(m->data + sp->off, irp->buf, (size_t)n);
    else
        memcpy(irp->buf, m->data + sp->off, (size_t)n);
    irp->info = n;
    return io_complete_request(irp, 0);
}

static int mem_ioctl(struct device *dev, struct irp *irp)
{
    struct mem_ext *m = dev->ext;
    struct irp_sp *sp = io_cur_sp(irp);
    int rc = 0;
    irp->info = 0;
    switch (sp->code) {
    case DEVCTL_GETSIZE:
        if (sp->outlen < 8) { rc = -E_INVAL; break; }
        memcpy(irp->buf, &dev->size, 8);
        irp->info = 8;
        break;
    case MEM_IOCTL_FAIL_NEXT:
        if (sp->len < 4) { rc = -E_INVAL; break; }
        lock();
        memcpy(&m->fail_next, irp->buf, 4);
        unlock();
        break;
    case MEM_IOCTL_COUNTERS:
        if (sp->outlen < sizeof(m->c)) { rc = -E_INVAL; break; }
        lock();
        m->c.ioctls++;
        memcpy(irp->buf, &m->c, sizeof(m->c));
        unlock();
        irp->info = sizeof(m->c);
        break;
    default:
        rc = -E_NOTTY;
    }
    return io_complete_request(irp, rc);
}

static void mem_remove(struct device *dev)
{
    kfree(((struct mem_ext *)dev->ext)->data);
}

struct device *mem_device_create(const char *name, uint64_t size)
{
    if (!size || size > 16 * MiB)
        return NULL;
    struct device *d;
    if (io_create_device(mem_drv, sizeof(struct mem_ext), DEVF_SEEKABLE, &d))
        return NULL;
    struct mem_ext *m = d->ext;
    m->data = kzalloc((size_t)size);
    if (!m->data) {
        io_delete_device(d);
        return NULL;
    }
    d->size = size;
    if (name) {
        sec_kset(&d->o, SID_SYSTEM, acl_all_rw, 1);
        if (io_publish_device(d, name)) {
            io_delete_device(d);
            return NULL;
        }
    }
    return d;
}

static int blkdisk_rw(struct device *dev, struct irp *irp)
{
    struct blkdev *b = dev->ext;
    struct irp_sp *sp = io_cur_sp(irp);
    bool wr = sp->major == IRP_MJ_WRITE;
    uint32_t ss = blk_ssize(b);
    uint64_t size = b->sectors * ss;
    irp->info = 0;
    if (wr && b->ro)
        return io_complete_request(irp, -E_ROFS);
    if (sp->off >= size)
        return io_complete_request(irp, wr && sp->len ? -E_NOSPC : 0);
    uint64_t n = size - sp->off < sp->len ? size - sp->off : sp->len;
    uint8_t *p = irp->buf;
    uint64_t off = sp->off, left = n;
    int rc = 0;
    while (left && !rc) {
        uint64_t lba = off / ss;
        uint32_t o = (uint32_t)(off % ss);
        if (o || left < ss) {
            uint32_t c = ss - o < left ? ss - o : (uint32_t)left;
            rc = wr ? blk_write_part(b, lba, o, c, p) : blk_read_part(b, lba, o, c, p);
            p += c; off += c; left -= c;
        } else {
            uint32_t cnt = (uint32_t)(left / ss);
            if (cnt > 128)
                cnt = 128;
            rc = wr ? blk_write(b, lba, cnt, p) : blk_read(b, lba, cnt, p);
            p += (uint64_t)cnt * ss; off += (uint64_t)cnt * ss; left -= (uint64_t)cnt * ss;
        }
    }
    irp->info = n - left;
    return io_complete_request(irp, rc);
}

static int blkdisk_flush(struct device *dev, struct irp *irp)
{
    return io_complete_request(irp, blk_flush(dev->ext));
}

static int blkdisk_ioctl(struct device *dev, struct irp *irp)
{
    struct blkdev *b = dev->ext;
    struct irp_sp *sp = io_cur_sp(irp);
    irp->info = 0;
    if (sp->code == DEVCTL_GETSIZE && sp->outlen >= 8) {
        memcpy(irp->buf, &dev->size, 8);
        irp->info = 8;
        return io_complete_request(irp, 0);
    }
    if (sp->code == DEVCTL_SECTOR && sp->outlen >= 4) {
        uint32_t ss = blk_ssize(b);
        memcpy(irp->buf, &ss, 4);
        irp->info = 4;
        return io_complete_request(irp, 0);
    }
    return io_complete_request(irp, -E_NOTTY);
}

static struct driver *blkdisk_drv;

struct device *blkdisk_attach(struct blkdev *b)
{
    struct device *d;
    if (io_create_device(blkdisk_drv, 0, DEVF_SEEKABLE, &d))
        return NULL;
    d->ext = b;
    d->size = b->sectors * blk_ssize(b);
    sec_kset(&d->o, SID_SYSTEM, NULL, 0);
    if (io_publish_device(d, b->name)) {
        d->ext = NULL;
        io_delete_device(d);
        return NULL;
    }
    return d;
}

static void blkdisk_remove(struct device *d) { d->ext = NULL; }

void blkdisk_detach(struct device *d) { io_delete_device(d); }

void blkdisk_scan(void)
{
    for (unsigned i = 0; i < blk_count(); i++) {
        struct blkdev *b = blk_nth(i);
        if (b && !blkdisk_attach(b))
            klog(LOG_WARN, "io: cannot expose %s as a device", b ? b->name : "?");
    }
}

struct xor_ext { uint8_t key; };
struct xor_ctx { void *orig, *tmp; uint32_t len; uint8_t key; };

static struct driver *xor_drv;

static int xor_done_write(struct device *above, struct irp *irp, void *ctx)
{
    (void)above;
    struct xor_ctx *c = ctx;
    irp->buf = c->orig;
    kfree(c->tmp);
    kfree(c);
    return IO_CONTINUE;
}

static int xor_done_read(struct device *above, struct irp *irp, void *ctx)
{
    (void)above;
    struct xor_ctx *c = ctx;
    uint8_t *p = irp->buf;
    if (irp->status == 0)
        for (uint64_t i = 0; i < irp->info; i++)
            p[i] ^= c->key;
    kfree(c);
    return IO_CONTINUE;
}

static int xor_write(struct device *dev, struct irp *irp)
{
    struct xor_ext *x = dev->ext;
    struct irp_sp *sp = io_cur_sp(irp);
    struct xor_ctx *c = kzalloc(sizeof(*c));
    void *tmp = sp->len ? kmalloc(sp->len) : NULL;
    if (!c || (sp->len && !tmp)) {
        kfree(c);
        kfree(tmp);
        return io_complete_request(irp, -E_NOMEM);
    }
    const uint8_t *s = irp->buf;
    for (uint32_t i = 0; i < sp->len; i++)
        ((uint8_t *)tmp)[i] = s[i] ^ x->key;
    c->orig = irp->buf;
    c->tmp = tmp;
    irp->buf = tmp;
    io_skip_sp(irp);
    io_set_completion(irp, xor_done_write, c);
    return io_call_driver(dev->below, irp);
}

static int xor_read(struct device *dev, struct irp *irp)
{
    struct xor_ext *x = dev->ext;
    struct xor_ctx *c = kzalloc(sizeof(*c));
    if (!c)
        return io_complete_request(irp, -E_NOMEM);
    c->key = x->key;
    io_skip_sp(irp);
    io_set_completion(irp, xor_done_read, c);
    return io_call_driver(dev->below, irp);
}

static int xor_ioctl(struct device *dev, struct irp *irp)
{
    struct irp_sp *sp = io_cur_sp(irp);
    if (sp->code == XOR_IOCTL_KEY) {
        if (sp->len < 1)
            return io_complete_request(irp, -E_INVAL);
        ((struct xor_ext *)dev->ext)->key = *(uint8_t *)irp->buf;
        irp->info = 0;
        return io_complete_request(irp, 0);
    }
    return io_pass_down(dev, irp);
}

struct device *xor_filter_create(uint8_t key)
{
    struct device *d;
    if (io_create_device(xor_drv, sizeof(struct xor_ext), 0, &d))
        return NULL;
    ((struct xor_ext *)d->ext)->key = key;
    return d;
}

struct cnt_ext { struct cnt_counters c; uint32_t rofs; };
static struct driver *cnt_drv;

static int cnt_dispatch(struct device *dev, struct irp *irp)
{
    struct cnt_ext *x = dev->ext;
    struct irp_sp *sp = io_cur_sp(irp);
    lock();
    x->c.n[sp->major]++;
    bool ro = x->rofs;
    unlock();
    if (sp->major == IRP_MJ_WRITE && ro) {
        irp->info = 0;
        return io_complete_request(irp, -E_ROFS);
    }
    if (sp->major == IRP_MJ_CREATE && ro && (sp->len & DEV_WRITE_DATA))
        return io_complete_request(irp, -E_ROFS);
    if (sp->major == IRP_MJ_IOCTL && sp->code == CNT_IOCTL_GET) {
        if (sp->outlen < sizeof(x->c))
            return io_complete_request(irp, -E_INVAL);
        lock();
        memcpy(irp->buf, &x->c, sizeof(x->c));
        unlock();
        irp->info = sizeof(x->c);
        return io_complete_request(irp, 0);
    }
    if (sp->major == IRP_MJ_IOCTL && sp->code == CNT_IOCTL_ROFS) {
        if (sp->len < 4)
            return io_complete_request(irp, -E_INVAL);
        uint32_t v;
        memcpy(&v, irp->buf, 4);
        lock();
        x->rofs = v != 0;
        unlock();
        irp->info = 0;
        return io_complete_request(irp, 0);
    }
    return io_pass_down(dev, irp);
}

struct device *cnt_filter_create(void)
{
    struct device *d;
    if (io_create_device(cnt_drv, sizeof(struct cnt_ext), 0, &d))
        return NULL;
    return d;
}

struct slow_ext {
    struct irp   *head, *tail;
    struct sem    wake;
    struct thread *th;
    bool          stop;
    uint32_t      delay_ms, started;
};
static struct driver *slow_drv;

static void slow_cancel(struct device *dev, struct irp *irp)
{
    struct slow_ext *s = dev->ext;
    lock();
    struct irp **pp = &s->head, *prev = NULL;
    while (*pp && *pp != irp) {
        prev = *pp;
        pp = &(*pp)->qnext;
    }
    if (*pp) {
        *pp = irp->qnext;
        if (s->tail == irp)
            s->tail = prev;
    }
    unlock();
    irp->info = 0;
    io_complete_request(irp, -E_CANCELED);
}

static int slow_queue(struct device *dev, struct irp *irp)
{
    struct slow_ext *s = dev->ext;
    io_set_cancel(irp, dev, slow_cancel);
    lock();
    irp->qnext = NULL;
    if (s->tail)
        s->tail->qnext = irp;
    else
        s->head = irp;
    s->tail = irp;
    unlock();
    sem_up(&s->wake);
    return IO_PENDING;
}

static int slow_ioctl(struct device *dev, struct irp *irp)
{
    struct slow_ext *s = dev->ext;
    struct irp_sp *sp = io_cur_sp(irp);
    if (sp->code != SLOW_IOCTL_DELAY || sp->len < 4)
        return io_complete_request(irp, -E_NOTTY);
    uint32_t v;
    memcpy(&v, irp->buf, 4);
    s->delay_ms = v > 5000 ? 5000 : v;
    irp->info = 0;
    return io_complete_request(irp, 0);
}

static int slow_worker(void *arg)
{
    struct device *dev = arg;
    struct slow_ext *s = dev->ext;
    for (;;) {
        sem_down(&s->wake);
        lock();
        struct irp *irp = s->head;
        if (irp) {
            s->head = irp->qnext;
            if (!s->head)
                s->tail = NULL;
        }
        bool stop = s->stop;
        unlock();
        if (!irp) {
            if (stop)
                return 0;
            continue;
        }
        if (!io_clear_cancel(irp))
            continue;
        if (stop) {
            irp->info = 0;
            io_complete_request(irp, -E_NODEV);
            continue;
        }
        lock();
        s->started++;
        unlock();
        if (s->delay_ms)
            thread_sleep_ms(s->delay_ms);
        struct irp_sp *sp = io_cur_sp(irp);
        if (sp->major == IRP_MJ_READ)
            memset(irp->buf, 'S', sp->len);
        irp->info = sp->len;
        io_complete_request(irp, 0);
    }
}

static void slow_remove(struct device *dev)
{
    struct slow_ext *s = dev->ext;
    if (!s->th)
        return;
    lock();
    s->stop = true;
    unlock();
    sem_up(&s->wake);
    thread_join(s->th);
}

uint32_t slow_started(struct device *d)
{
    struct slow_ext *s = d->ext;
    lock();
    uint32_t v = s->started;
    unlock();
    return v;
}

struct device *slow_device_create(const char *name)
{
    struct device *d;
    if (io_create_device(slow_drv, sizeof(struct slow_ext), 0, &d))
        return NULL;
    struct slow_ext *s = d->ext;
    sem_init(&s->wake, 0);
    s->delay_ms = 20;
    s->th = thread_create("slowdev", slow_worker, d, 0);
    if (!s->th) {
        io_delete_device(d);
        return NULL;
    }
    if (name) {
        sec_kset(&d->o, SID_SYSTEM, acl_all_rw, 1);
        if (io_publish_device(d, name)) {
            io_delete_device(d);
            return NULL;
        }
    }
    return d;
}

void drivers_init(void)
{
    struct driver *nd = io_driver_create("null"), *zd = io_driver_create("zero");
    mem_drv = io_driver_create("mem");
    blkdisk_drv = io_driver_create("blkdisk");
    xor_drv = io_driver_create("xor");
    cnt_drv = io_driver_create("count");
    slow_drv = io_driver_create("slow");
    if (!nd || !zd || !mem_drv || !blkdisk_drv || !xor_drv || !cnt_drv || !slow_drv)
        kpanic("drivers: cannot register");

    nd->dispatch[IRP_MJ_READ] = null_read;
    nd->dispatch[IRP_MJ_WRITE] = sink_write;
    nd->dispatch[IRP_MJ_IOCTL] = no_ioctl;
    zd->dispatch[IRP_MJ_READ] = zero_read;
    zd->dispatch[IRP_MJ_WRITE] = sink_write;
    zd->dispatch[IRP_MJ_IOCTL] = no_ioctl;

    mem_drv->dispatch[IRP_MJ_READ] = mem_rw;
    mem_drv->dispatch[IRP_MJ_WRITE] = mem_rw;
    mem_drv->dispatch[IRP_MJ_IOCTL] = mem_ioctl;
    mem_drv->remove = mem_remove;

    blkdisk_drv->dispatch[IRP_MJ_READ] = blkdisk_rw;
    blkdisk_drv->dispatch[IRP_MJ_WRITE] = blkdisk_rw;
    blkdisk_drv->dispatch[IRP_MJ_IOCTL] = blkdisk_ioctl;
    blkdisk_drv->dispatch[IRP_MJ_FLUSH] = blkdisk_flush;
    blkdisk_drv->remove = blkdisk_remove;

    filter_defaults(xor_drv);
    xor_drv->dispatch[IRP_MJ_READ] = xor_read;
    xor_drv->dispatch[IRP_MJ_WRITE] = xor_write;
    xor_drv->dispatch[IRP_MJ_IOCTL] = xor_ioctl;

    for (unsigned i = 0; i < IRP_MJ_MAX; i++)
        cnt_drv->dispatch[i] = cnt_dispatch;

    slow_drv->dispatch[IRP_MJ_READ] = slow_queue;
    slow_drv->dispatch[IRP_MJ_WRITE] = slow_queue;
    slow_drv->dispatch[IRP_MJ_IOCTL] = slow_ioctl;
    slow_drv->remove = slow_remove;

    struct device *n, *z;
    if (io_create_device(nd, 0, 0, &n) || io_create_device(zd, 0, 0, &z))
        kpanic("drivers: cannot create \\Device\\Null");
    sec_kset(&n->o, SID_SYSTEM, acl_all_rw, 1);
    sec_kset(&z->o, SID_SYSTEM, acl_all_rw, 1);
    if (io_publish_device(n, "Null") || io_publish_device(z, "Zero"))
        kpanic("drivers: cannot publish \\Device\\Null");
}
