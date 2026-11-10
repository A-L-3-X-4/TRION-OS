#pragma once
#include "kernel.h"
#include "obj.h"
#include "blk.h"
#include "list.h"
#include "sched.h"
#include "abi.h"

#define VFS_NAME_MAX 255
#define VFS_PATH_MAX 260
#define VFS_VOL_NAME_MAX 15

struct vfs_sb;

struct vnode {
    struct list_node link;
    struct vfs_sb   *sb;
    uint64_t         key;
    uint32_t         refs;
    uint32_t         pending;
    struct vnode    *dnext;
    uint32_t         type;
    uint32_t         attr;
    uint64_t         size;
    uint64_t         ctime, mtime, atime;
};

struct vfs_ops {
    const char *name;
    int  (*mount)(struct vfs_sb *sb);
    void (*unmount)(struct vfs_sb *sb);
    int  (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    int  (*create)(struct vnode *dir, const char *name, uint32_t type, struct vnode **out);
    int  (*remove)(struct vnode *dir, struct vnode *v);
    int  (*rename)(struct vnode *odir, struct vnode *v, struct vnode *ndir, const char *nname);
    int  (*readdir)(struct vnode *dir, uint32_t index, struct fs_dirent *out);
    long (*read)(struct vnode *v, uint64_t off, void *buf, size_t n);
    long (*write)(struct vnode *v, uint64_t off, const void *buf, size_t n);
    int  (*truncate)(struct vnode *v, uint64_t size);
    int  (*statfs)(struct vfs_sb *sb, struct fs_statfs *out);
};

struct vfs_sb {
    const struct vfs_ops *ops;
    struct blkdev   *dev;
    struct sem       lock;
    struct list_node vnodes;
    unsigned         nvnodes;
    struct vnode    *root;
    struct vnode    *deferred;
    bool             ro, dead;
    void            *fs;
    char             label[12];
};

struct vnode *vnode_get(struct vfs_sb *sb, uint64_t key, size_t size, bool *fresh);
void          vnode_ref(struct vnode *v);
void          vnode_put(struct vnode *v);
void          vfs_register_fs(const struct vfs_ops *ops);
extern const struct vfs_ops fat32_ops;

int  fat32_format(struct blkdev *dev, const char *label, uint32_t spc);
int  fat32_format_ss(struct blkdev *dev, const char *label, uint32_t spc, uint32_t ss);

struct volume {
    struct object   o;
    struct vfs_sb  *sb;
    char            name[VFS_VOL_NAME_MAX + 1];
};

struct file {
    struct object   o;
    struct volume  *vol;
    struct vnode   *v;
    uint64_t        pos;
    uint32_t        access;
};

extern const struct obj_type volume_type, file_type;

int  vfs_dos_volume(int letter, char out[VFS_VOL_NAME_MAX + 1]);
int  vfs_alias_path(const char *in, char *out, size_t cap, bool *changed);

void vfs_init(void);
int  vfs_mount(const char *name, struct blkdev *dev, bool ro, struct volume **out);
int  vfs_unmount(const char *name);
int  vfs_automount(void);
struct volume *vfs_volume_for_dev(struct blkdev *dev);

int  vfs_open(struct volume *v, const char *rel, uint32_t disp, uint32_t access, uint32_t flags,
              bool may_create, struct file **out);
int  vfs_mkdir(struct volume *v, const char *rel);
int  vfs_remove(struct volume *v, const char *rel);
int  vfs_rename(struct volume *v, const char *from, const char *to);
int  vfs_stat(struct volume *v, const char *rel, struct file_stat *st);
int  vfs_statfs(struct volume *v, struct fs_statfs *out);
int  vfs_sync(struct volume *v);

long file_read(struct file *f, void *buf, size_t n);
long file_write(struct file *f, const void *buf, size_t n);
long file_pread(struct file *f, uint64_t off, void *buf, size_t n);
long file_pwrite(struct file *f, uint64_t off, const void *buf, size_t n);
long file_seek(struct file *f, int64_t off, int whence);
int  file_stat(struct file *f, struct file_stat *st);
int  file_setsize(struct file *f, uint64_t size);
int  file_readdir(struct file *f, uint32_t index, struct fs_dirent *out);

unsigned vfs_live_vnodes(void);
unsigned fstest_run(void);

struct task;
long fs_k_open(struct task *t, const char *full, uint32_t access, uint32_t dispflags);
int  fs_k_stat(struct task *t, const char *full, struct file_stat *st);
int  fs_k_mkdir(struct task *t, const char *full);
int  fs_k_remove(struct task *t, const char *full);
int  fs_k_rename(struct task *t, const char *from, const char *to);
long sys_fs_dispatch(struct task *t, uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
