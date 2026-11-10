#pragma once
#include "kernel.h"
#include "abi.h"
#include "obj.h"
#include "sched.h"
#include "blk.h"

enum {
    IRP_MJ_CREATE = 0,
    IRP_MJ_CLOSE,
    IRP_MJ_READ,
    IRP_MJ_WRITE,
    IRP_MJ_IOCTL,
    IRP_MJ_FLUSH,
    IRP_MJ_MAX
};

#define IO_PENDING          1
#define IO_CONTINUE         0
#define IO_MORE_PROCESSING  1

#define IRP_STACK_MAX   8
#define IO_NAME_MAX     32

struct driver;
struct device;
struct irp;

typedef int (*io_dispatch_fn)(struct device *dev, struct irp *irp);
typedef int (*io_completion_fn)(struct device *dev, struct irp *irp, void *ctx);
typedef void (*io_cancel_fn)(struct device *dev, struct irp *irp);
typedef void (*io_done_fn)(struct irp *irp, void *ctx);

struct irp_sp {
    uint8_t           major;
    struct device    *dev;
    uint64_t          off;
    uint32_t          len;
    uint32_t          code;
    uint32_t          outlen;
    io_completion_fn  completion;
    void             *ctx;
};

struct irp {
    int               status;
    uint64_t          info;
    void             *buf;
    uint32_t          buflen;
    bool              own_buf;
    struct devfile   *file;
    void             *fctx;
    int               cur;
    uint32_t          nsp;
    bool              completed;
    bool              sync;
    struct sem        done;
    io_done_fn        cb;
    void             *cb_ctx;
    io_cancel_fn      cancel;
    struct device    *cancel_dev;
    struct irp       *qnext;
    uint64_t          qdata;
    struct device    *top;
    struct irp_sp     sp[IRP_STACK_MAX + 1];
};

struct driver {
    struct object  o;
    char           name[IO_NAME_MAX];
    io_dispatch_fn dispatch[IRP_MJ_MAX];
    void         (*remove)(struct device *dev);
    unsigned       ndevices;
};

#define DEVF_SEEKABLE   0x1

struct device {
    struct object  o;
    struct driver *drv;
    char           name[IO_NAME_MAX];
    struct device *below;
    struct device *above;
    uint32_t       stack_size;
    uint32_t       flags;
    uint64_t       size;
    unsigned       opens;
    void          *ext;
    bool           deleted;
};

struct devfile {
    struct object  o;
    struct device *dev;
    uint64_t       pos;
    uint32_t       access;
    void          *fctx;
};

extern const struct obj_type driver_type, device_type, devfile_type;

struct driver *io_driver_create(const char *name);
int  io_driver_unload(struct driver *d);

int  io_create_device(struct driver *d, size_t ext_size, uint32_t flags, struct device **out);
int  io_publish_device(struct device *dev, const char *name);
int  io_attach(struct device *filter, struct device *target);
int  io_delete_device(struct device *dev);
struct device *io_top(struct device *d);
struct device *io_base(struct device *d);
struct device *io_find_device(const char *name);

struct irp *io_irp_alloc(struct device *top, uint8_t major);
void        io_irp_free(struct irp *irp);
static inline struct irp_sp *io_cur_sp(struct irp *irp)  { return &irp->sp[irp->cur]; }
static inline struct irp_sp *io_next_sp(struct irp *irp) { return &irp->sp[irp->cur + 1]; }
void        io_skip_sp(struct irp *irp);
int         io_call_driver(struct device *dev, struct irp *irp);
void        io_set_completion(struct irp *irp, io_completion_fn fn, void *ctx);
int         io_complete_request(struct irp *irp, int status);
int         io_pass_down(struct device *dev, struct irp *irp);
int         io_call_sync(struct device *dev, struct irp *irp);

void        io_set_cancel(struct irp *irp, struct device *dev, io_cancel_fn fn);
bool        io_clear_cancel(struct irp *irp);
bool        io_cancel_irp(struct irp *irp);

int io_rw(struct device *d, bool write, uint64_t off, void *buf, uint32_t len, uint64_t *bytes);
int io_ioctl(struct device *d, uint32_t code, const void *in, uint32_t inlen, void *out, uint32_t outcap,
             uint64_t *outlen);
int io_flush(struct device *d);

int io_open(const struct token *tok, const char *path, uint32_t access, struct devfile **out);
long io_file_read(struct devfile *f, void *buf, size_t len);
long io_file_write(struct devfile *f, const void *buf, size_t len);
long io_file_ioctl(struct devfile *f, uint32_t code, const void *in, uint32_t inlen, void *out,
                   uint32_t outcap);
long io_file_seek(struct devfile *f, int64_t off, int whence);
void io_files_init(void);
void io_close_drain(void);

long sys_dev_dispatch(struct task *t, uint64_t nr, uint64_t a, uint64_t b, uint64_t c);

void io_init(void);
void drivers_init(void);
struct device *blkdisk_attach(struct blkdev *b);
void           blkdisk_detach(struct device *d);
void blkdisk_scan(void);

struct device *mem_device_create(const char *name, uint64_t size);
struct device *xor_filter_create(uint8_t key);
struct device *cnt_filter_create(void);
struct device *slow_device_create(const char *name);
uint32_t       slow_started(struct device *d);

unsigned drvtest_run(void);

struct mem_counters { uint32_t reads, writes, failed, ioctls; };
struct cnt_counters { uint32_t n[IRP_MJ_MAX]; };
