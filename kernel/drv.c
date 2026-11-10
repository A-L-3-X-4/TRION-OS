#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"
#include "drv.h"

static inline void lock(void)   { preempt_disable(); }
static inline void unlock(void) { preempt_enable(); }

static void driver_destroy(struct object *o)
{
    (void)o;
}

static void device_destroy(struct object *o)
{
    struct device *d = (struct device *)o;
    if (d->below) {
        d->below->above = NULL;
        obj_unref(&d->below->o);
        d->below = NULL;
    }
    kfree(d->ext);
    if (d->drv) {
        d->drv->ndevices--;
        obj_unref(&d->drv->o);
    }
}

static const struct gmap device_gmap = {
    DEV_READ_DATA | ACCESS_READ_CONTROL,
    DEV_WRITE_DATA | ACCESS_READ_CONTROL,
    ACCESS_READ_CONTROL,
    DEV_VALID_HANDLE | ACCESS_DELETE | ACCESS_SEC_RIGHTS };

const struct obj_type driver_type = {
    "Driver", OBJ_DRIVER, ACCESS_DELETE | ACCESS_SEC_RIGHTS, driver_destroy, NULL, NULL };
const struct obj_type device_type = {
    "Device", OBJ_DEVICE, DEV_VALID_HANDLE | ACCESS_DELETE | ACCESS_SEC_RIGHTS, device_destroy, NULL,
    &device_gmap };

static int join_path(char *out, size_t cap, const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    if (!nl || nl >= IO_NAME_MAX || dl + nl + 1 >= cap)
        return -E_INVAL;
    memcpy(out, dir, dl);
    memcpy(out + dl, name, nl + 1);
    return 0;
}

struct driver *io_driver_create(const char *name)
{
    char path[64];
    if (join_path(path, sizeof(path), "\\Driver\\", name))
        return NULL;
    struct driver *d = obj_create(&driver_type, sizeof(*d));
    if (!d)
        return NULL;
    memcpy(d->name, name, strlen(name) + 1);
    static const struct ace acl[] = {
        { ACE_ALLOW, 0, 0, SID_EVERYONE, ACCESS_READ_CONTROL },
    };
    if (sec_kset(&d->o, SID_SYSTEM, acl, 1) || ns_insert(path, &d->o)) {
        obj_unref(&d->o);
        return NULL;
    }
    return d;
}

int io_driver_unload(struct driver *d)
{
    char path[64];
    lock();
    bool busy = d->ndevices != 0;
    unlock();
    if (busy)
        return -E_BUSY;
    int rc = join_path(path, sizeof(path), "\\Driver\\", d->name);
    if (!rc)
        rc = ns_unlink(path);
    obj_unref(&d->o);
    return rc;
}

int io_create_device(struct driver *drv, size_t ext_size, uint32_t flags, struct device **out)
{
    struct device *d = obj_create(&device_type, sizeof(*d));
    if (!d)
        return -E_NOMEM;
    if (ext_size) {
        d->ext = kzalloc(ext_size);
        if (!d->ext) {
            obj_unref(&d->o);
            return -E_NOMEM;
        }
    }
    d->flags = flags;
    d->stack_size = 1;
    obj_ref(&drv->o);
    lock();
    d->drv = drv;
    drv->ndevices++;
    unlock();
    *out = d;
    return 0;
}

int io_publish_device(struct device *dev, const char *name)
{
    char path[64];
    int rc = join_path(path, sizeof(path), "\\Device\\", name);
    if (rc)
        return rc;
    if (dev->name[0] || dev->below)
        return -E_INVAL;
    rc = ns_insert(path, &dev->o);
    if (!rc)
        memcpy(dev->name, name, strlen(name) + 1);
    return rc;
}

struct device *io_find_device(const char *name)
{
    char path[64];
    struct object *o;
    if (join_path(path, sizeof(path), "\\Device\\", name) || ns_lookup(path, &o))
        return NULL;
    if (o->type != &device_type) {
        obj_unref(o);
        return NULL;
    }
    return (struct device *)o;
}

static struct device *top_locked(struct device *d)
{
    while (d->above)
        d = d->above;
    return d;
}

struct device *io_top(struct device *d)
{
    lock();
    struct device *t = top_locked(d);
    unlock();
    return t;
}

struct device *io_base(struct device *d)
{
    lock();
    while (d->below)
        d = d->below;
    unlock();
    return d;
}

int io_attach(struct device *filter, struct device *target)
{
    int rc = 0;
    lock();
    struct device *t = top_locked(target);
    if (filter == t || filter->below || filter->above || filter->name[0] || t->deleted || filter->deleted)
        rc = -E_INVAL;
    else if (t->stack_size >= IRP_STACK_MAX)
        rc = -E_NOSPC;
    else {
        filter->below = t;
        t->above = filter;
        filter->stack_size = t->stack_size + 1;
        obj_ref(&t->o);
    }
    unlock();
    return rc;
}

int io_delete_device(struct device *dev)
{
    io_close_drain();
    lock();
    int rc = 0;
    if (dev->deleted)
        rc = -E_INVAL;
    else if (dev->above || dev->opens)
        rc = -E_BUSY;
    else
        dev->deleted = true;
    unlock();
    if (rc)
        return rc;

    if (dev->name[0]) {
        char path[64];
        if (!join_path(path, sizeof(path), "\\Device\\", dev->name))
            ns_unlink(path);
        dev->name[0] = 0;
    }
    lock();
    struct device *b = dev->below;
    if (b) {
        b->above = NULL;
        dev->below = NULL;
    }
    unlock();
    if (b)
        obj_unref(&b->o);
    if (dev->drv->remove)
        dev->drv->remove(dev);
    obj_unref(&dev->o);
    return 0;
}

struct irp *io_irp_alloc(struct device *top, uint8_t major)
{
    struct irp *irp = kzalloc(sizeof(*irp));
    if (!irp)
        return NULL;
    irp->nsp = top->stack_size;
    irp->cur = -1;
    irp->sp[0].major = major;
    sem_init(&irp->done, 0);
    irp->top = top;
    obj_ref(&top->o);
    return irp;
}

void io_irp_free(struct irp *irp)
{
    if (!irp)
        return;
    if (irp->own_buf)
        kfree(irp->buf);
    obj_unref(&irp->top->o);
    kfree(irp);
}

void io_skip_sp(struct irp *irp)
{
    struct irp_sp *n = io_next_sp(irp);
    *n = *io_cur_sp(irp);
    n->completion = NULL;
    n->ctx = NULL;
    n->dev = NULL;
}

void io_set_completion(struct irp *irp, io_completion_fn fn, void *ctx)
{
    struct irp_sp *n = io_next_sp(irp);
    n->completion = fn;
    n->ctx = ctx;
}

int io_call_driver(struct device *dev, struct irp *irp)
{
    if (!dev)
        return io_complete_request(irp, -E_NODEV);
    if (irp->cur + 1 >= (int)irp->nsp)
        return io_complete_request(irp, -E_INVAL);
    irp->cur++;
    struct irp_sp *sp = &irp->sp[irp->cur];
    sp->dev = dev;
    if (sp->major >= IRP_MJ_MAX)
        return io_complete_request(irp, -E_INVAL);
    io_dispatch_fn fn = dev->drv->dispatch[sp->major];
    if (!fn)
        return io_complete_request(irp, sp->major <= IRP_MJ_CLOSE ? 0 : -E_NOTSUP);
    return fn(dev, irp);
}

int io_pass_down(struct device *dev, struct irp *irp)
{
    io_skip_sp(irp);
    return io_call_driver(dev->below, irp);
}

int io_complete_request(struct irp *irp, int status)
{
    irp->status = status;
    irp->cancel = NULL;
    for (;;) {
        int i = irp->cur;
        if (i < 0)
            break;
        struct irp_sp *sp = &irp->sp[i];
        irp->cur = i - 1;
        io_completion_fn fn = sp->completion;
        if (fn) {
            sp->completion = NULL;
            struct device *above = i > 0 ? irp->sp[i - 1].dev : NULL;
            if (fn(above, irp, sp->ctx) == IO_MORE_PROCESSING)
                return IO_PENDING;
        }
    }
    int st = irp->status;
    irp->completed = true;
    io_done_fn cb = irp->cb;
    void *cx = irp->cb_ctx;
    bool sync = irp->sync;
    if (cb)
        cb(irp, cx);
    else if (sync)
        sem_up(&irp->done);
    return st;
}

int io_call_sync(struct device *dev, struct irp *irp)
{
    irp->sync = true;
    int st = io_call_driver(dev, irp);
    if (st != IO_PENDING && !irp->completed) {
        klog(LOG_WARN, "io: driver '%s' returned %d without completing the IRP",
             dev->drv->name, st);
        io_complete_request(irp, st);
    }
    sem_down(&irp->done);
    return irp->status;
}

void io_set_cancel(struct irp *irp, struct device *dev, io_cancel_fn fn)
{
    lock();
    irp->cancel = fn;
    irp->cancel_dev = dev;
    unlock();
}

bool io_clear_cancel(struct irp *irp)
{
    lock();
    io_cancel_fn fn = irp->cancel;
    irp->cancel = NULL;
    unlock();
    return fn != NULL;
}

bool io_cancel_irp(struct irp *irp)
{
    lock();
    io_cancel_fn fn = irp->cancel;
    struct device *dev = irp->cancel_dev;
    irp->cancel = NULL;
    unlock();
    if (!fn)
        return false;
    fn(dev, irp);
    return true;
}

int io_rw(struct device *d, bool write, uint64_t off, void *buf, uint32_t len, uint64_t *bytes)
{
    struct irp *irp = io_irp_alloc(d, write ? IRP_MJ_WRITE : IRP_MJ_READ);
    if (!irp)
        return -E_NOMEM;
    irp->buf = buf;
    irp->buflen = len;
    irp->sp[0].off = off;
    irp->sp[0].len = len;
    int rc = io_call_sync(d, irp);
    if (bytes)
        *bytes = irp->info;
    io_irp_free(irp);
    return rc;
}

int io_ioctl(struct device *d, uint32_t code, const void *in, uint32_t inlen, void *out, uint32_t outcap,
             uint64_t *outlen)
{
    uint32_t n = inlen > outcap ? inlen : outcap;
    struct irp *irp = io_irp_alloc(d, IRP_MJ_IOCTL);
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
    irp->sp[0].code = code;
    irp->sp[0].len = inlen;
    irp->sp[0].outlen = outcap;
    int rc = io_call_sync(d, irp);
    if (!rc && out && irp->info)
        memcpy(out, irp->buf, irp->info > outcap ? outcap : (size_t)irp->info);
    if (outlen)
        *outlen = irp->info;
    io_irp_free(irp);
    return rc;
}

int io_flush(struct device *d)
{
    struct irp *irp = io_irp_alloc(d, IRP_MJ_FLUSH);
    if (!irp)
        return -E_NOMEM;
    int rc = io_call_sync(d, irp);
    io_irp_free(irp);
    return rc;
}

void io_init(void)
{
    static const struct ace acl[] = {
        { ACE_ALLOW, 0, 0, SID_EVERYONE, ACCESS_GENERIC_READ | ACCESS_GENERIC_EXECUTE },
    };
    static const char *const dirs[] = { "\\Device", "\\Driver" };
    for (unsigned i = 0; i < 2; i++) {
        struct object *d;
        if (ns_mkdir(dirs[i]) || ns_lookup(dirs[i], &d))
            kpanic("io: cannot create %s", dirs[i]);
        sec_kset(d, SID_SYSTEM, acl, 1);
        obj_unref(d);
    }
    io_files_init();
}
