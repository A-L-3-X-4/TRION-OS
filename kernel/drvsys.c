#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"
#include "drv.h"

#define XFER_MAX   (1u << 20)
#define CHUNK      (16u * 1024)
#define IOCTL_MAX  4096
#define PATH_MAX_  300

struct close_work {
    struct close_work *next;
    struct devfile     snap;
};

static struct close_work *cw_head, *cw_tail;
static struct sem         cw_sem;
static unsigned           cw_pending;

static void devfile_destroy(struct object *o)
{
    struct devfile *f = (struct devfile *)o;
    if (!f->dev)
        return;
    struct close_work *w = kzalloc(sizeof(*w));
    if (!w) {
        f->dev->opens--;
        obj_unref(&f->dev->o);
        klog(LOG_WARN, "io: out of memory closing a device; the driver missed a CLOSE");
        return;
    }
    w->snap = *f;
    cw_pending++;
    if (cw_tail)
        cw_tail->next = w;
    else
        cw_head = w;
    cw_tail = w;
    sem_up(&cw_sem);
}

static const struct gmap devfile_gmap = {
    DEV_READ_DATA, DEV_WRITE_DATA, 0, DEV_VALID_HANDLE };
const struct obj_type devfile_type = {
    "DevFile", OBJ_DEVFILE, DEV_VALID_HANDLE, devfile_destroy, NULL, &devfile_gmap };

static int close_worker(void *arg)
{
    (void)arg;
    for (;;) {
        sem_down(&cw_sem);
        preempt_disable();
        struct close_work *w = cw_head;
        cw_head = w->next;
        if (!cw_head)
            cw_tail = NULL;
        preempt_enable();

        struct device *top = w->snap.dev;
        struct irp *irp = io_irp_alloc(top, IRP_MJ_CLOSE);
        if (irp) {
            irp->file = &w->snap;
            io_call_sync(top, irp);
            io_irp_free(irp);
        }
        preempt_disable();
        top->opens--;
        cw_pending--;
        preempt_enable();
        obj_unref(&top->o);
        kfree(w);
    }
    return 0;
}

void io_files_init(void)
{
    sem_init(&cw_sem, 0);
    struct thread *t = thread_create("io-close", close_worker, NULL, 0);
    if (!t)
        kpanic("io: cannot start the close worker");
    thread_detach(t);
}

void io_close_drain(void)
{
    for (;;) {
        preempt_disable();
        unsigned n = cw_pending;
        preempt_enable();
        if (!n)
            return;
        thread_sleep_ms(1);
    }
}

int io_open(const struct token *tok, const char *path, uint32_t access, struct devfile **out)
{
    static const char pre[] = "\\Device\\";
    if (strlen(path) <= sizeof(pre) - 1 || memcmp(path, pre, sizeof(pre) - 1))
        return -E_NOENT;
    if (!access || (access & ~(uint32_t)DEV_VALID_HANDLE))
        return -E_INVAL;
    struct object *o;
    int rc = ns_lookup_as(tok, path, &o);
    if (rc)
        return rc;
    if (o->type != &device_type) {
        obj_unref(o);
        return -E_NODEV;
    }
    uint32_t granted;
    rc = sec_check(tok, o, access, &granted);
    if (rc) {
        obj_unref(o);
        return rc;
    }

    preempt_disable();
    struct device *top = io_top((struct device *)o);
    bool dead = top->deleted;
    if (!dead) {
        top->opens++;
        obj_ref(&top->o);
    }
    preempt_enable();
    obj_unref(o);
    if (dead)
        return -E_NODEV;

    struct devfile *f = obj_create(&devfile_type, sizeof(*f));
    if (!f) {
        preempt_disable();
        top->opens--;
        preempt_enable();
        obj_unref(&top->o);
        return -E_NOMEM;
    }
    f->dev = top;
    f->access = granted;

    struct irp *irp = io_irp_alloc(top, IRP_MJ_CREATE);
    if (!irp) {
        rc = -E_NOMEM;
    } else {
        irp->sp[0].len = granted;
        irp->file = f;
        rc = io_call_sync(top, irp);
        io_irp_free(irp);
    }
    if (rc) {
        preempt_disable();
        top->opens--;
        preempt_enable();
        obj_unref(&top->o);
        f->dev = NULL;
        obj_unref(&f->o);
        return rc;
    }
    *out = f;
    return 0;
}

static bool seekable(struct devfile *f) { return io_base(f->dev)->flags & DEVF_SEEKABLE; }

long io_file_read(struct devfile *f, void *buf, size_t len)
{
    if (len > CHUNK)
        len = CHUNK;
    struct irp *irp = io_irp_alloc(f->dev, IRP_MJ_READ);
    if (!irp)
        return -E_NOMEM;
    irp->buf = buf;
    irp->buflen = (uint32_t)len;
    irp->file = f;
    irp->sp[0].off = seekable(f) ? f->pos : 0;
    irp->sp[0].len = (uint32_t)len;
    int rc = io_call_sync(f->dev, irp);
    long n = rc ? rc : (long)irp->info;
    io_irp_free(irp);
    if (n > 0 && seekable(f))
        f->pos += (uint64_t)n;
    return n;
}

long io_file_write(struct devfile *f, const void *buf, size_t len)
{
    if (len > CHUNK)
        len = CHUNK;
    struct irp *irp = io_irp_alloc(f->dev, IRP_MJ_WRITE);
    if (!irp)
        return -E_NOMEM;
    irp->buf = (void *)buf;
    irp->buflen = (uint32_t)len;
    irp->file = f;
    irp->sp[0].off = seekable(f) ? f->pos : 0;
    irp->sp[0].len = (uint32_t)len;
    int rc = io_call_sync(f->dev, irp);
    long n = rc ? rc : (long)irp->info;
    io_irp_free(irp);
    if (n > 0 && seekable(f))
        f->pos += (uint64_t)n;
    return n;
}

long io_file_ioctl(struct devfile *f, uint32_t code, const void *in, uint32_t inlen, void *out,
                   uint32_t outcap)
{
    if (inlen > IOCTL_MAX || outcap > IOCTL_MAX)
        return -E_INVAL;
    struct irp *irp;
    if (code == DEVCTL_FLUSH) {
        irp = io_irp_alloc(f->dev, IRP_MJ_FLUSH);
        if (!irp)
            return -E_NOMEM;
        irp->file = f;
        int rc = io_call_sync(f->dev, irp);
        io_irp_free(irp);
        return rc;
    }
    uint32_t n = inlen > outcap ? inlen : outcap;
    irp = io_irp_alloc(f->dev, IRP_MJ_IOCTL);
    if (!irp)
        return -E_NOMEM;
    if (n) {
        irp->buf = kzalloc(n);
        if (!irp->buf) {
            io_irp_free(irp);
            return -E_NOMEM;
        }
        irp->own_buf = true;
        irp->buflen = n;
        if (inlen)
            memcpy(irp->buf, in, inlen);
    }
    irp->file = f;
    irp->sp[0].code = code;
    irp->sp[0].len = inlen;
    irp->sp[0].outlen = outcap;
    int rc = io_call_sync(f->dev, irp);
    long r = rc;
    if (!rc) {
        r = (long)(irp->info > outcap ? outcap : irp->info);
        if (r && out)
            memcpy(out, irp->buf, (size_t)r);
    }
    io_irp_free(irp);
    return r;
}

long io_file_seek(struct devfile *f, int64_t off, int whence)
{
    struct device *base = io_base(f->dev);
    if (!(base->flags & DEVF_SEEKABLE))
        return -E_SPIPE;
    int64_t b;
    switch (whence) {
    case SEEK_SET: b = 0; break;
    case SEEK_CUR: b = (int64_t)f->pos; break;
    case SEEK_END: b = (int64_t)base->size; break;
    default:       return -E_INVAL;
    }
    int64_t np = b + off;
    if (np < 0 || np > (int64_t)0x7FFFFFFFFFFFLL)
        return -E_INVAL;
    f->pos = (uint64_t)np;
    return np;
}

static long sys_open(struct task *t, uint64_t upath, uint64_t len, uint64_t access)
{
    char path[PATH_MAX_ + 1];
    if (len == 0 || len > PATH_MAX_)
        return -E_INVAL;
    int rc = user_copy_in(t, path, upath, len);
    if (rc)
        return rc;
    path[len] = 0;
    if (strlen(path) != len)
        return -E_INVAL;
    struct devfile *f;
    rc = io_open(&t->tok, path, (uint32_t)access, &f);
    if (rc)
        return rc;
    uint32_t h;
    rc = ht_insert(t->ht, &f->o, f->access, &h);
    obj_unref(&f->o);
    return rc ? rc : (long)h;
}

static long sys_read(struct task *t, uint32_t h, uint64_t ubuf, uint64_t len)
{
    struct object *o;
    int rc = ht_lookup(t->ht, h, &devfile_type, DEV_READ_DATA, &o, NULL);
    if (rc)
        return rc;
    if (len > XFER_MAX)
        len = XFER_MAX;
    uint8_t *buf = kmalloc(CHUNK);
    long done = 0;
    if (!buf)
        rc = -E_NOMEM;
    else
        rc = user_check_out(t, ubuf, len);
    while (!rc && (uint64_t)done < len) {
        size_t c = len - done > CHUNK ? CHUNK : (size_t)(len - done);
        long n = io_file_read((struct devfile *)o, buf, c);
        if (n < 0) {
            rc = (int)n;
            break;
        }
        if (n == 0)
            break;
        user_copy_out(t, ubuf + done, buf, (size_t)n);
        done += n;
        if ((size_t)n < c)
            break;
    }
    kfree(buf);
    obj_unref(o);
    return done ? done : rc;
}

static long sys_write(struct task *t, uint32_t h, uint64_t ubuf, uint64_t len)
{
    struct object *o;
    int rc = ht_lookup(t->ht, h, &devfile_type, DEV_WRITE_DATA, &o, NULL);
    if (rc)
        return rc;
    if (len > XFER_MAX)
        len = XFER_MAX;
    uint8_t *buf = kmalloc(CHUNK);
    long done = 0;
    if (!buf)
        rc = -E_NOMEM;
    while (!rc && (uint64_t)done < len) {
        size_t c = len - done > CHUNK ? CHUNK : (size_t)(len - done);
        rc = user_copy_in(t, buf, ubuf + done, c);
        if (rc)
            break;
        long n = io_file_write((struct devfile *)o, buf, c);
        if (n < 0) {
            rc = (int)n;
            break;
        }
        done += n;
        if ((size_t)n < c)
            break;
    }
    kfree(buf);
    obj_unref(o);
    return done ? done : rc;
}

static long sys_ioctl(struct task *t, uint32_t h, uint32_t code, uint64_t uarg)
{
    struct dev_ioctl a;
    int rc = user_copy_in(t, &a, uarg, sizeof(a));
    if (rc)
        return rc;
    if (a.inlen > IOCTL_MAX || a.outlen > IOCTL_MAX)
        return -E_INVAL;
    uint32_t need = DEV_IOCTL | (code == DEVCTL_FLUSH ? DEV_WRITE_DATA : 0);
    struct object *o;
    rc = ht_lookup(t->ht, h, &devfile_type, need, &o, NULL);
    if (rc)
        return rc;
    uint8_t *in = NULL, *out = NULL;
    long r = 0;
    if (a.outlen)
        r = user_check_out(t, a.out, a.outlen);
    if (!r && a.inlen) {
        in = kmalloc(a.inlen);
        if (!in)
            r = -E_NOMEM;
        else
            r = user_copy_in(t, in, a.in, a.inlen);
    }
    if (!r && a.outlen && !(out = kzalloc(a.outlen)))
        r = -E_NOMEM;
    if (!r) {
        r = io_file_ioctl((struct devfile *)o, code, in, (uint32_t)a.inlen, out, (uint32_t)a.outlen);
        if (r > 0 && out)
            user_copy_out(t, a.out, out, (size_t)r);
    }
    kfree(in);
    kfree(out);
    obj_unref(o);
    return r;
}

long sys_dev_dispatch(struct task *t, uint64_t nr, uint64_t a, uint64_t b, uint64_t c)
{
    struct object *o;
    long rc;
    switch (nr) {
    case SYS_DEV_OPEN:  return sys_open(t, a, b, c);
    case SYS_DEV_READ:  return sys_read(t, (uint32_t)a, b, c);
    case SYS_DEV_WRITE: return sys_write(t, (uint32_t)a, b, c);
    case SYS_DEV_IOCTL: return sys_ioctl(t, (uint32_t)a, (uint32_t)b, c);
    case SYS_DEV_SEEK:
        rc = ht_lookup(t->ht, (uint32_t)a, &devfile_type, 0, &o, NULL);
        if (rc)
            return rc;
        rc = io_file_seek((struct devfile *)o, (int64_t)b, (int)c);
        obj_unref(o);
        return rc;
    }
    return -E_NOSYS;
}
