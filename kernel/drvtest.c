#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"
#include "blk.h"
#include "timer.h"
#include "drv.h"

static unsigned n_checks, n_fail;
static void check_(bool ok, const char *expr, int line)
{
    n_checks++;
    if (!ok) {
        n_fail++;
        klog(LOG_FAIL, "drvtest.c:%d: %s", line, expr);
    }
}
#define CHECK(c) check_((c), #c, __LINE__)
#define IMG_SIZE ((size_t)(user_tests_end - user_tests_start))

static inline void lock(void)   { preempt_disable(); }
static inline void unlock(void) { preempt_enable(); }

static struct devfile *open_k(const char *path, uint32_t access)
{
    struct devfile *f = NULL;
    int rc = io_open(NULL, path, access, &f);
    if (rc)
        klog(LOG_FAIL, "drvtest: open %s -> %d", path, rc);
    return f;
}

static void close_k(struct devfile *f)
{
    if (f)
        obj_unref(&f->o);
}

#define RW (DEV_READ_DATA | DEV_WRITE_DATA | DEV_IOCTL)

static void test_registry(void)
{
    struct object *o;
    CHECK(ns_lookup("\\Device", &o) == 0 && o->type->id == OBJ_DIR);
    obj_unref(o);
    CHECK(ns_lookup("\\Driver\\null", &o) == 0 && o->type->id == OBJ_DRIVER);
    obj_unref(o);
    CHECK(ns_lookup("\\Device\\Null", &o) == 0 && o->type->id == OBJ_DEVICE);
    obj_unref(o);
    CHECK(ns_lookup("\\Device\\Zero", &o) == 0);
    obj_unref(o);

    struct devfile *f = open_k("\\Device\\Null", RW);
    uint8_t b[16];
    CHECK(f && io_file_write(f, "abcdef", 6) == 6);
    CHECK(f && io_file_read(f, b, 16) == 0);
    CHECK(f && io_file_seek(f, 0, SEEK_SET) == -E_SPIPE);
    CHECK(f && io_file_ioctl(f, DEVCTL_USER + 9, NULL, 0, NULL, 0) == -E_NOTTY);
    close_k(f);

    f = open_k("\\Device\\Zero", DEV_READ_DATA);
    memset(b, 0xEE, sizeof(b));
    CHECK(f && io_file_read(f, b, 16) == 16);
    bool z = true;
    for (unsigned i = 0; i < 16; i++)
        z &= b[i] == 0;
    CHECK(z);
    close_k(f);

    struct devfile *x;
    CHECK(io_open(NULL, "\\Device\\Nope", DEV_READ_DATA, &x) == -E_NOENT);
    CHECK(io_open(NULL, "\\Volumes\\x", DEV_READ_DATA, &x) == -E_NOENT);
    CHECK(io_open(NULL, "\\Device", DEV_READ_DATA, &x) == -E_NOENT);
    CHECK(io_open(NULL, "\\Device\\Null", 0, &x) == -E_INVAL);
    CHECK(io_open(NULL, "\\Device\\Null", 0x100, &x) == -E_INVAL);
    CHECK(io_open(NULL, "\\Driver\\null", DEV_READ_DATA, &x) == -E_NOENT);
    io_close_drain();
    klog(LOG_INFO, "drvtest: \\Device and \\Driver, null/zero, open path and its errors ok");
}

static void test_mem(void)
{
    struct device *m = mem_device_create("m0", 65536);
    CHECK(m != NULL);
    if (!m)
        return;
    struct devfile *f = open_k("\\Device\\m0", RW);
    uint8_t w[100], r[100];
    for (unsigned i = 0; i < sizeof(w); i++)
        w[i] = (uint8_t)(i * 7 + 1);
    CHECK(f && io_file_write(f, w, 100) == 100);
    CHECK(f && f->pos == 100);
    CHECK(f && io_file_seek(f, 0, SEEK_SET) == 0);
    memset(r, 0, sizeof(r));
    CHECK(f && io_file_read(f, r, 100) == 100 && memcmp(r, w, 100) == 0);
    CHECK(f && io_file_seek(f, -10, SEEK_END) == 65526);
    CHECK(f && io_file_read(f, r, 100) == 10);
    CHECK(f && io_file_read(f, r, 100) == 0);
    CHECK(f && io_file_write(f, w, 4) == -E_NOSPC);
    CHECK(f && io_file_seek(f, -1, SEEK_SET) == -E_INVAL);
    CHECK(f && io_file_seek(f, 0, 9) == -E_INVAL);
    CHECK(f && io_file_seek(f, 60000, SEEK_SET) == 60000);
    CHECK(f && io_file_read(f, r, 8) == 8);
    bool z = true;
    for (unsigned i = 0; i < 8; i++)
        z &= r[i] == 0;
    CHECK(z);

    uint64_t sz = 0;
    CHECK(f && io_file_ioctl(f, DEVCTL_GETSIZE, NULL, 0, &sz, 8) == 8 && sz == 65536);
    CHECK(f && io_file_ioctl(f, DEVCTL_GETSIZE, NULL, 0, &sz, 4) == -E_INVAL);
    CHECK(f && io_file_ioctl(f, 0x777, NULL, 0, NULL, 0) == -E_NOTTY);
    CHECK(f && io_file_ioctl(f, DEVCTL_FLUSH, NULL, 0, NULL, 0) == -E_NOTSUP);

    CHECK(io_delete_device(m) == -E_BUSY);
    close_k(f);
    io_close_drain();
    CHECK(m->opens == 0);
    CHECK(io_delete_device(m) == 0);
    CHECK(io_find_device("m0") == NULL);
    struct devfile *x;
    CHECK(io_open(NULL, "\\Device\\m0", DEV_READ_DATA, &x) == -E_NOENT);
    klog(LOG_INFO, "drvtest: mem device: transfers, short transfers, seek, ioctls, busy-delete ok");
}

static char trace[160];
static unsigned tn;
static void tr(const char *s)
{
    lock();
    while (*s && tn < sizeof(trace) - 1)
        trace[tn++] = *s++;
    trace[tn] = 0;
    unlock();
}
static void tr_reset(void) { lock(); tn = 0; trace[0] = 0; unlock(); }

struct tr_ext { char id; };

static int tr_up(struct device *above, struct irp *irp, void *ctx)
{
    (void)irp;
    struct tr_ext *e = above->ext;
    char s[3] = { 'u', e->id, 0 };
    tr(s);
    (void)ctx;
    return IO_CONTINUE;
}

static int tr_down(struct device *dev, struct irp *irp)
{
    struct tr_ext *e = dev->ext;
    char s[3] = { 'd', e->id, 0 };
    tr(s);
    io_skip_sp(irp);
    io_set_completion(irp, tr_up, NULL);
    return io_call_driver(dev->below, irp);
}

static int tbase_read(struct device *dev, struct irp *irp)
{
    (void)dev;
    tr("B");
    memset(irp->buf, 'x', io_cur_sp(irp)->len);
    irp->info = io_cur_sp(irp)->len;
    return io_complete_request(irp, 0);
}

static void test_stacks(void)
{
    struct driver *td = io_driver_create("trtest"), *bd = io_driver_create("tbase");
    CHECK(td && bd);
    if (!td || !bd)
        return;
    td->dispatch[IRP_MJ_READ] = tr_down;
    bd->dispatch[IRP_MJ_READ] = tbase_read;
    CHECK(io_driver_create("trtest") == NULL);

    struct device *base, *f[3];
    CHECK(io_create_device(bd, 0, 0, &base) == 0);
    CHECK(base->stack_size == 1 && io_top(base) == base && io_base(base) == base);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(io_create_device(td, sizeof(struct tr_ext), 0, &f[i]) == 0);
        ((struct tr_ext *)f[i]->ext)->id = (char)('1' + i);
        CHECK(io_attach(f[i], base) == 0);
        CHECK(f[i]->stack_size == i + 2);
    }
    CHECK(io_top(base) == f[2] && io_top(f[0]) == f[2] && io_base(f[2]) == base);
    CHECK(f[0]->below == base && f[1]->below == f[0] && f[2]->below == f[1] && base->above == f[0]);

    char buf[8];
    tr_reset();
    uint64_t n = 0;
    CHECK(io_rw(f[2], false, 0, buf, 8, &n) == 0 && n == 8 && buf[0] == 'x');
    CHECK(strcmp(trace, "d3d2d1Bu1u2u3") == 0);
    if (strcmp(trace, "d3d2d1Bu1u2u3"))
        klog(LOG_FAIL, "drvtest: trace was %s", trace);
    tr_reset();
    CHECK(io_rw(f[0], false, 0, buf, 8, &n) == 0 && strcmp(trace, "d1Bu1") == 0);
    tr_reset();
    CHECK(io_rw(base, false, 0, buf, 8, &n) == 0 && strcmp(trace, "B") == 0);
    CHECK(io_rw(base, true, 0, buf, 8, &n) == -E_NOTSUP);

    CHECK(io_attach(f[1], base) == -E_INVAL);
    CHECK(io_publish_device(f[1], "nope") == -E_INVAL);
    CHECK(io_delete_device(base) == -E_BUSY);
    CHECK(io_delete_device(f[0]) == -E_BUSY);
    CHECK(io_delete_device(f[1]) == -E_BUSY);
    CHECK(io_driver_unload(td) == -E_BUSY);
    struct device *extra[IRP_STACK_MAX], *spare = NULL;
    unsigned ne = 0;
    int rc = 0;
    while (!rc && ne < IRP_STACK_MAX) {
        if (io_create_device(td, sizeof(struct tr_ext), 0, &spare))
            break;
        rc = io_attach(spare, base);
        if (!rc)
            extra[ne++] = spare;
    }
    CHECK(rc == -E_NOSPC && ne == IRP_STACK_MAX - 4 && io_top(base)->stack_size == IRP_STACK_MAX);
    if (rc && spare)
        CHECK(io_delete_device(spare) == 0);
    for (unsigned i = ne; i-- > 0;)
        CHECK(io_delete_device(extra[i]) == 0);
    CHECK(io_delete_device(f[2]) == 0 && io_delete_device(f[1]) == 0 && io_delete_device(f[0]) == 0);
    CHECK(base->above == NULL && io_top(base) == base);
    CHECK(io_delete_device(f[0]) == -E_INVAL);
    CHECK(io_delete_device(base) == 0);
    CHECK(io_driver_unload(td) == 0 && io_driver_unload(bd) == 0);
    struct object *o;
    CHECK(ns_lookup("\\Driver\\trtest", &o) != 0);
    klog(LOG_INFO, "drvtest: stacks: attach/delete rules, depth limit, dispatch down + completion up in order ok");
}

static void test_filters(void)
{
    struct device *m = mem_device_create("m1", 4096);
    struct device *x = xor_filter_create(0x5A), *c = cnt_filter_create();
    CHECK(m && x && c);
    if (!m || !x || !c)
        return;
    CHECK(io_attach(x, m) == 0 && io_attach(c, m) == 0);
    CHECK(c->stack_size == 3 && io_top(m) == c);

    struct devfile *f = open_k("\\Device\\m1", RW);
    CHECK(f && f->dev == c);
    const char *msg = "hello, filters";
    char plain[32], raw[32], back[32];
    memcpy(plain, msg, 15);
    CHECK(f && io_file_write(f, plain, 15) == 15);
    CHECK(memcmp(plain, msg, 15) == 0);
    CHECK(io_rw(m, false, 0, raw, 15, NULL) == 0);
    bool enc = true;
    for (unsigned i = 0; i < 15; i++)
        enc &= raw[i] == (char)(msg[i] ^ 0x5A);
    CHECK(enc && memcmp(raw, msg, 15) != 0);
    CHECK(f && io_file_seek(f, 0, SEEK_SET) == 0);
    memset(back, 0, sizeof(back));
    CHECK(f && io_file_read(f, back, 15) == 15 && memcmp(back, msg, 15) == 0);
    CHECK(f && io_file_seek(f, 4090, SEEK_SET) == 4090);
    memset(back, 0x11, sizeof(back));
    CHECK(f && io_file_read(f, back, 20) == 6 && back[6] == 0x11);

    uint8_t key = 0x33;
    CHECK(f && io_file_ioctl(f, XOR_IOCTL_KEY, &key, 1, NULL, 0) == 0);
    CHECK(f && io_file_seek(f, 100, SEEK_SET) == 100 && io_file_write(f, plain, 4) == 4);
    CHECK(io_rw(m, false, 100, raw, 4, NULL) == 0 && raw[0] == (char)('h' ^ 0x33));

    struct mem_counters mc;
    CHECK(f && io_file_ioctl(f, MEM_IOCTL_COUNTERS, NULL, 0, &mc, sizeof(mc)) == sizeof(mc));
    CHECK(mc.writes == 2 && mc.failed == 0);
    uint64_t sz = 0;
    CHECK(f && io_file_ioctl(f, DEVCTL_GETSIZE, NULL, 0, &sz, 8) == 8 && sz == 4096);

    struct cnt_counters cc;
    CHECK(f && io_file_ioctl(f, CNT_IOCTL_GET, NULL, 0, &cc, sizeof(cc)) == sizeof(cc));
    CHECK(cc.n[IRP_MJ_CREATE] == 1 && cc.n[IRP_MJ_WRITE] == 2 && cc.n[IRP_MJ_READ] == 2);

    uint32_t one = 1, zero = 0;
    CHECK(f && io_file_ioctl(f, CNT_IOCTL_ROFS, &one, 4, NULL, 0) == 0);
    CHECK(f && io_file_write(f, plain, 4) == -E_ROFS);
    CHECK(io_rw(m, false, 100, raw, 4, NULL) == 0 && raw[0] == (char)('h' ^ 0x33));
    CHECK(f && io_file_seek(f, 0, SEEK_SET) == 0 && io_file_read(f, back, 4) == 4);
    struct devfile *g;
    CHECK(io_open(NULL, "\\Device\\m1", DEV_WRITE_DATA, &g) == -E_ROFS);
    g = open_k("\\Device\\m1", DEV_READ_DATA);
    CHECK(g != NULL);
    close_k(g);
    CHECK(f && io_file_ioctl(f, CNT_IOCTL_ROFS, &zero, 4, NULL, 0) == 0);
    CHECK(f && io_file_write(f, plain, 4) == 4);

    CHECK(io_flush(c) == -E_NOTSUP);

    CHECK(io_delete_device(c) == -E_BUSY);
    close_k(f);
    io_close_drain();
    CHECK(c->opens == 0 && m->opens == 0);
    CHECK(io_delete_device(m) == -E_BUSY);
    CHECK(io_delete_device(c) == 0 && io_delete_device(x) == 0 && io_delete_device(m) == 0);
    klog(LOG_INFO, "drvtest: xor filter (private buffer + completion routine), count filter, ioctl routing ok");
}

struct retry_ctx { unsigned tries; };

static int retry_done(struct device *above, struct irp *irp, void *ctx)
{
    struct retry_ctx *c = ctx;
    if (irp->status == -E_IO && c->tries < 3) {
        c->tries++;
        io_skip_sp(irp);
        io_set_completion(irp, retry_done, c);
        io_call_driver(above->below, irp);
        return IO_MORE_PROCESSING;
    }
    kfree(c);
    return IO_CONTINUE;
}

static int retry_dispatch(struct device *dev, struct irp *irp)
{
    struct retry_ctx *c = kzalloc(sizeof(*c));
    if (!c)
        return io_complete_request(irp, -E_NOMEM);
    io_skip_sp(irp);
    io_set_completion(irp, retry_done, c);
    return io_call_driver(dev->below, irp);
}

static void test_retry(void)
{
    struct driver *rd = io_driver_create("retry");
    CHECK(rd != NULL);
    if (!rd)
        return;
    rd->dispatch[IRP_MJ_READ] = retry_dispatch;
    rd->dispatch[IRP_MJ_WRITE] = retry_dispatch;
    struct device *m = mem_device_create(NULL, 4096), *r;
    CHECK(m && io_create_device(rd, 0, 0, &r) == 0 && io_attach(r, m) == 0);
    if (!m)
        return;
    struct mem_counters mc;
    uint32_t n = 2;
    char b[16] = "retry me";
    CHECK(io_ioctl(m, MEM_IOCTL_FAIL_NEXT, &n, 4, NULL, 0, NULL) == 0);
    CHECK(io_rw(r, true, 0, b, 9, NULL) == 0);
    CHECK(io_ioctl(m, MEM_IOCTL_COUNTERS, NULL, 0, &mc, sizeof(mc), NULL) == 0);
    CHECK(mc.writes == 3 && mc.failed == 2);
    char back[16] = { 0 };
    CHECK(io_rw(m, false, 0, back, 9, NULL) == 0 && strcmp(back, "retry me") == 0);

    n = 5;
    CHECK(io_ioctl(m, MEM_IOCTL_FAIL_NEXT, &n, 4, NULL, 0, NULL) == 0);
    CHECK(io_rw(r, false, 0, back, 9, NULL) == -E_IO);
    CHECK(io_ioctl(m, MEM_IOCTL_COUNTERS, NULL, 0, &mc, sizeof(mc), NULL) == 0);
    CHECK(mc.failed == 6 && mc.reads == 5);
    CHECK(io_rw(r, false, 0, back, 9, NULL) == 0);
    CHECK(io_delete_device(r) == 0 && io_delete_device(m) == 0 && io_driver_unload(rd) == 0);
    klog(LOG_INFO, "drvtest: retry filter: MORE_PROCESSING + resend, bounded retries ok");
}

struct arec {
    struct sem done;
    int        status;
    uint64_t   info;
    unsigned   order;
};
static unsigned arec_seq;

static void arec_cb(struct irp *irp, void *ctx)
{
    struct arec *a = ctx;
    a->status = irp->status;
    a->info = irp->info;
    lock();
    a->order = ++arec_seq;
    unlock();
    sem_up(&a->done);
}

static struct irp *async_write(struct device *d, struct arec *a, const void *buf, uint32_t len)
{
    struct irp *irp = io_irp_alloc(d, IRP_MJ_WRITE);
    if (!irp)
        return NULL;
    sem_init(&a->done, 0);
    irp->buf = (void *)buf;
    irp->buflen = len;
    irp->sp[0].len = len;
    irp->cb = arec_cb;
    irp->cb_ctx = a;
    io_call_driver(d, irp);
    return irp;
}

static bool wait_started(struct device *s, uint32_t n)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (slow_started(s) >= n)
            return true;
        thread_sleep_ms(1);
    }
    return false;
}

static void test_async(void)
{
    unsigned th0 = sched_thread_count();
    struct device *s = slow_device_create("s0");
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(sched_thread_count() == th0 + 1);
    struct arec A, B, C, D, E;
    char buf[8] = "slow";
    uint32_t delay = 60;
    CHECK(io_ioctl(s, SLOW_IOCTL_DELAY, &delay, 4, NULL, 0, NULL) == 0);

    struct irp *ia = async_write(s, &A, buf, 8);
    struct irp *ib = async_write(s, &B, buf, 8);
    struct irp *ic = async_write(s, &C, buf, 8);
    CHECK(ia && ib && ic);
    if (!ia || !ib || !ic)
        return;
    CHECK(wait_started(s, 1));
    CHECK(io_cancel_irp(ib) == true);
    CHECK(sem_trydown(&B.done) && B.status == -E_CANCELED && B.info == 0);
    CHECK(io_cancel_irp(ib) == false);
    CHECK(io_cancel_irp(ia) == false);
    CHECK(sem_down_timeout(&A.done, 2000) == SEM_OK);
    CHECK(A.status == 0 && A.info == 8);
    CHECK(sem_down_timeout(&C.done, 2000) == SEM_OK);
    CHECK(C.status == 0 && C.info == 8);
    CHECK(B.order < A.order && A.order < C.order);
    CHECK(io_cancel_irp(ic) == false);
    CHECK(slow_started(s) == 2);
    io_irp_free(ia); io_irp_free(ib); io_irp_free(ic);

    char r[8];
    uint64_t t0 = clock_ns();
    CHECK(io_rw(s, false, 0, r, 8, NULL) == 0 && r[0] == 'S' && r[7] == 'S');
    CHECK(clock_ns() - t0 >= 50ull * 1000 * 1000);

    delay = 150;
    CHECK(io_ioctl(s, SLOW_IOCTL_DELAY, &delay, 4, NULL, 0, NULL) == 0);
    struct irp *id_ = async_write(s, &D, buf, 8);
    struct irp *ie = async_write(s, &E, buf, 8);
    CHECK(id_ && ie);
    CHECK(wait_started(s, 4));
    CHECK(io_delete_device(s) == 0);
    CHECK(sem_trydown(&D.done) && D.status == 0);
    CHECK(sem_trydown(&E.done) && E.status == -E_NODEV);
    io_irp_free(id_);
    io_irp_free(ie);
    CHECK(sched_thread_count() == th0);
    klog(LOG_INFO, "drvtest: async IRPs: worker completion, cancel (queued/started/done), removal with a queue ok");
}

static uint8_t patt(uint64_t i) { return (uint8_t)((i * 131 + (i >> 7) * 17 + 5) & 0xFF); }

static void blk_case(const char *name, uint32_t ssize, uint64_t nsec)
{
    struct blkdev *b = blk_ramdisk_create_ss(name, nsec, ssize);
    CHECK(b != NULL);
    if (!b)
        return;
    struct device *d = blkdisk_attach(b);
    CHECK(d != NULL && d->size == nsec * ssize && (d->flags & DEVF_SEEKABLE));
    char path[40] = "\\Device\\";
    memcpy(path + 8, name, strlen(name) + 1);
    struct devfile *f = d ? open_k(path, RW) : NULL;

    struct token user, root;
    struct devfile *x;
    sec_token_user(&user, 1000, NULL, 0, 0);
    sec_token_user(&root, 1000, NULL, 0, PRIV_BYPASS_ACL);
    CHECK(io_open(&user, path, DEV_READ_DATA, &x) == -E_ACCES);
    CHECK(io_open(&user, path, DEV_READ_DATA | DEV_WRITE_DATA, &x) == -E_ACCES);
    if (io_open(&root, path, DEV_READ_DATA, &x) == 0)
        close_k(x);
    else
        CHECK(!"privileged open");

    uint64_t total = nsec * ssize;
    uint8_t *w = kmalloc(total), *r = kmalloc(total), *ref = kzalloc(total);
    CHECK(w && r && ref);
    if (!w || !r || !ref || !f)
        goto out;
    for (uint64_t i = 0; i < total; i++)
        w[i] = patt(i);

    uint64_t off = ssize - 37, len = 2ull * ssize + 100;
    uint64_t n = 0;
    CHECK(io_rw(d, true, off, w, (uint32_t)len, &n) == 0 && n == len);
    memcpy(ref + off, w, len);
    CHECK(io_rw(d, false, 0, r, (uint32_t)total, &n) == 0 && n == total && memcmp(r, ref, total) == 0);
    uint8_t *sec = kmalloc(ssize);
    CHECK(sec && blk_read(b, 1, 1, sec) == 0 && memcmp(sec, ref + ssize, ssize) == 0);
    kfree(sec);

    CHECK(io_rw(d, true, 0, w, (uint32_t)total, &n) == 0 && n == total);
    CHECK(io_rw(d, false, 0, r, (uint32_t)total, &n) == 0 && n == total && memcmp(r, w, total) == 0);
    CHECK(io_rw(d, false, total - 10, r, 100, &n) == 0 && n == 10 && memcmp(r, w + total - 10, 10) == 0);
    CHECK(io_rw(d, false, total, r, 10, &n) == 0 && n == 0);
    CHECK(io_rw(d, true, total, w, 10, &n) == -E_NOSPC);
    CHECK(io_rw(d, true, total - 3, w, 10, &n) == 0 && n == 3);

    uint64_t sz = 0;
    uint32_t ss = 0;
    CHECK(io_file_ioctl(f, DEVCTL_GETSIZE, NULL, 0, &sz, 8) == 8 && sz == total);
    CHECK(io_file_ioctl(f, DEVCTL_SECTOR, NULL, 0, &ss, 4) == 4 && ss == ssize);
    CHECK(io_file_ioctl(f, DEVCTL_FLUSH, NULL, 0, NULL, 0) == 0);
    CHECK(io_file_seek(f, 0, SEEK_END) == (long)total);
out:
    kfree(w); kfree(r); kfree(ref);
    close_k(f);
    io_close_drain();
    if (d)
        blkdisk_detach(d);
    CHECK(io_find_device(name) == NULL);
    blk_unregister(b);
}

static void test_blkdisk(void)
{
    unsigned ob = obj_live();
    blk_case("bd0", 512, 400);
    blk_case("bd4", 4096, 64);
    CHECK(obj_live() == ob);
    klog(LOG_INFO, "drvtest: blkdisk: unaligned / multi-sector / end-of-disk I/O on 512 B and 4 KiB sectors, root-only ACL ok");
}

static int run_dev(const char *name, uint64_t arg, uint32_t uid)
{
    struct task *t = task_new(name, user_tests_start, IMG_SIZE, UT_DEV, arg);
    if (!t)
        return -1;
    sec_token_user(&t->tok, uid, NULL, 0, 0);
    task_start(t);
    return task_wait(t);
}

static void test_user(void)
{
    struct device *m = mem_device_create("Mem0", 4096);
    struct device *sec = mem_device_create("Secret", 512);
    struct device *x = xor_filter_create(0x5A), *c = cnt_filter_create();
    CHECK(m && sec && x && c);
    if (!m || !sec || !x || !c)
        return;
    static const struct ace mem_acl[] = {
        { ACE_ALLOW, 0, 0, 1000, ACCESS_GENERIC_ALL },
        { ACE_ALLOW, 0, 0, SID_EVERYONE, DEV_READ_DATA },
    };
    static const struct ace sec_acl[] = {
        { ACE_ALLOW, 0, 0, 1000, ACCESS_GENERIC_ALL },
    };
    CHECK(sec_kset(&m->o, SID_SYSTEM, mem_acl, 2) == 0);
    CHECK(sec_kset(&sec->o, SID_SYSTEM, sec_acl, 1) == 0);

    struct token alice, bob;
    sec_token_user(&alice, 1000, NULL, 0, 0);
    sec_token_user(&bob, 1001, NULL, 0, 0);
    struct devfile *f;
    CHECK(io_open(&alice, "\\Device\\Mem0", RW, &f) == 0);
    close_k(f);
    CHECK(io_open(&bob, "\\Device\\Mem0", DEV_READ_DATA, &f) == 0);
    close_k(f);
    CHECK(io_open(&bob, "\\Device\\Mem0", DEV_WRITE_DATA, &f) == -E_ACCES);
    CHECK(io_open(&bob, "\\Device\\Mem0", DEV_IOCTL, &f) == -E_ACCES);
    CHECK(io_open(&bob, "\\Device\\Secret", DEV_READ_DATA, &f) == -E_ACCES);
    CHECK(io_open(&alice, "\\Device\\Secret", DEV_READ_DATA, &f) == 0);
    close_k(f);
    io_close_drain();
    CHECK(io_attach(x, m) == 0 && io_attach(c, m) == 0);
    struct cnt_counters cc;

    int rc = run_dev("dev-alice", 1, 1000);
    if (rc)
        klog(LOG_FAIL, "dev-alice exited with 0x%x (the check that failed)", rc);
    CHECK(rc == 0);
    rc = run_dev("dev-bob", 0, 1001);
    if (rc)
        klog(LOG_FAIL, "dev-bob exited with 0x%x (the check that failed)", rc);
    CHECK(rc == 0);
    io_close_drain();

    uint8_t raw[12];
    static const char msg[] = "hello device";
    CHECK(io_rw(m, false, 0, raw, 12, NULL) == 0);
    bool enc = true;
    for (unsigned i = 0; i < 12; i++)
        enc &= raw[i] == (uint8_t)(msg[i] ^ 0x5A);
    CHECK(enc);
    CHECK(io_ioctl(c, CNT_IOCTL_GET, NULL, 0, &cc, sizeof(cc), NULL) == 0);
    CHECK(cc.n[IRP_MJ_CREATE] == 4 && cc.n[IRP_MJ_CLOSE] == 3);
    CHECK(m->opens == 0 && c->opens == 0 && sec->opens == 0);
    CHECK(io_delete_device(c) == 0 && io_delete_device(x) == 0 && io_delete_device(m) == 0);
    CHECK(io_delete_device(sec) == 0);
    klog(LOG_INFO, "drvtest: ring-3 device syscalls: ACLs, 3-layer stack, errors, close-on-exit ok");
}

unsigned drvtest_run(void)
{
    n_checks = n_fail = 0;
    unsigned ob = obj_live(), th = sched_thread_count();
    test_registry();
    test_mem();
    test_stacks();
    test_filters();
    test_retry();
    test_async();
    test_blkdisk();
    test_user();
    io_close_drain();
    CHECK(obj_live() == ob);
    CHECK(sched_thread_count() == th);
    if (n_fail)
        klog(LOG_FAIL, "DRVTEST FAIL (%u of %u checks failed)", n_fail, n_checks);
    else
        klog(LOG_OK, "DRVTEST PASS (%u checks)", n_checks);
    console_set_color(n_fail ? C_LRED : C_LGREEN, C_BLACK);
    kprintf("\nStep 8 (driver framework) %s.\n", n_fail ? "has FAILURES" : "complete");
    console_set_color(C_LGRAY, C_BLACK);
    return n_fail;
}
