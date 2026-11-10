#include "kernel.h"
#include "mm.h"
#include "task.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"
#include "vfs.h"

#define FS_FULL_MAX 300
#define XFER_MAX    (1u << 20)
#define CHUNK       4096

static int get_path(struct task *t, char *buf, uint64_t uptr, uint64_t len)
{
    if (len == 0 || len > FS_FULL_MAX)
        return -E_INVAL;
    int rc = user_copy_in(t, buf, uptr, len);
    if (rc)
        return rc;
    buf[len] = 0;
    if (strlen(buf) != len)
        return -E_INVAL;
    return 0;
}

static int resolve(struct task *t, const char *full, uint32_t need, struct volume **vol, const char **rel)
{
    const char *p = full;
    static const char pre[] = "\\Volumes\\";
    for (const char *q = pre; *q; q++, p++)
        if (*p != *q)
            return -E_NOENT;
    char path[sizeof(pre) + VFS_VOL_NAME_MAX + 1];
    size_t n = sizeof(pre) - 1;
    memcpy(path, pre, n);
    size_t k = 0;
    while (p[k] && p[k] != '\\' && p[k] != '/') {
        if (k >= VFS_VOL_NAME_MAX)
            return -E_NOENT;
        path[n + k] = p[k];
        k++;
    }
    if (!k)
        return -E_NOENT;
    path[n + k] = 0;

    struct object *o;
    int rc = ns_lookup_as(&t->tok, path, &o);
    if (rc)
        return rc;
    if (o->type != &volume_type) {
        obj_unref(o);
        return -E_NOENT;
    }
    uint32_t g;
    rc = sec_check(&t->tok, o, need, &g);
    if (rc) {
        obj_unref(o);
        return rc;
    }
    *vol = (struct volume *)o;
    *rel = p + k;
    return 0;
}

long fs_k_open(struct task *t, const char *full, uint32_t access, uint32_t dispflags)
{
    if (!access || (access & ~(uint32_t)FILE_VALID_HANDLE))
        return -E_INVAL;
    struct volume *vol;
    const char *rel;
    long rc = resolve(t, full, access, &vol, &rel);
    if (rc)
        return rc;
    uint32_t g;
    bool may_create = sec_check(&t->tok, &vol->o, FILE_CREATE, &g) == 0;
    struct file *f;
    rc = vfs_open(vol, rel, dispflags & FD_MASK, access, dispflags & ~(uint32_t)FD_MASK, may_create, &f);
    obj_unref(&vol->o);
    if (rc)
        return rc;
    uint32_t h = 0;
    rc = ht_insert(t->ht, &f->o, f->access, &h);
    obj_unref(&f->o);
    return rc ? rc : (long)h;
}

int fs_k_stat(struct task *t, const char *full, struct file_stat *st)
{
    struct volume *vol;
    const char *rel;
    memset(st, 0, sizeof(*st));
    int rc = resolve(t, full, FILE_READ_DATA, &vol, &rel);
    if (!rc) {
        rc = vfs_stat(vol, rel, st);
        obj_unref(&vol->o);
    }
    return rc;
}

int fs_k_mkdir(struct task *t, const char *full)
{
    struct volume *vol;
    const char *rel;
    int rc = resolve(t, full, FILE_CREATE, &vol, &rel);
    if (!rc) {
        rc = vfs_mkdir(vol, rel);
        obj_unref(&vol->o);
    }
    return rc;
}

int fs_k_remove(struct task *t, const char *full)
{
    struct volume *vol;
    const char *rel;
    int rc = resolve(t, full, FILE_DELETE_CHILD, &vol, &rel);
    if (!rc) {
        rc = vfs_remove(vol, rel);
        obj_unref(&vol->o);
    }
    return rc;
}

int fs_k_rename(struct task *t, const char *from, const char *to)
{
    struct volume *vol, *vol2;
    const char *rel, *rel2;
    int rc = resolve(t, from, FILE_DELETE_CHILD, &vol, &rel);
    if (rc)
        return rc;
    rc = resolve(t, to, FILE_CREATE, &vol2, &rel2);
    if (!rc) {
        rc = vol == vol2 ? vfs_rename(vol, rel, rel2) : -E_XDEV;
        obj_unref(&vol2->o);
    }
    obj_unref(&vol->o);
    return rc;
}

static long sys_open(struct task *t, uint64_t upath, uint64_t len, uint64_t access, uint64_t dispflags)
{
    char full[FS_FULL_MAX + 1];
    long rc = get_path(t, full, upath, len);
    if (rc)
        return rc;
    if (!access || (access & ~(uint64_t)FILE_VALID_HANDLE))
        return -E_INVAL;
    return fs_k_open(t, full, (uint32_t)access, (uint32_t)dispflags);
}

static long sys_read(struct task *t, uint32_t h, uint64_t ubuf, uint64_t len)
{
    struct object *o;
    int rc = ht_lookup(t->ht, h, &file_type, FILE_READ_DATA, &o, NULL);
    if (rc)
        return rc;
    struct file *f = (struct file *)o;
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
        long n = file_read(f, buf, c);
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
    int rc = ht_lookup(t->ht, h, &file_type, FILE_WRITE_DATA, &o, NULL);
    if (rc)
        return rc;
    struct file *f = (struct file *)o;
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
        long n = file_write(f, buf, c);
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

long sys_fs_dispatch(struct task *t, uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    struct object *o;
    long rc;
    switch (nr) {
    case SYS_FILE_OPEN:  return sys_open(t, a, b, c, d);
    case SYS_FILE_READ:  return sys_read(t, (uint32_t)a, b, c);
    case SYS_FILE_WRITE: return sys_write(t, (uint32_t)a, b, c);

    case SYS_FILE_SEEK:
        rc = ht_lookup(t->ht, (uint32_t)a, &file_type, 0, &o, NULL);
        if (rc)
            return rc;
        rc = file_seek((struct file *)o, (int64_t)b, (int)c);
        obj_unref(o);
        return rc;

    case SYS_FILE_STAT: {
        struct file_stat st;
        memset(&st, 0, sizeof(st));
        rc = user_check_out(t, b, sizeof(st));
        if (rc)
            return rc;
        rc = ht_lookup(t->ht, (uint32_t)a, &file_type, 0, &o, NULL);
        if (rc)
            return rc;
        rc = file_stat((struct file *)o, &st);
        obj_unref(o);
        return rc ? rc : user_copy_out(t, b, &st, sizeof(st));
    }

    case SYS_FILE_SETSIZE:
        rc = ht_lookup(t->ht, (uint32_t)a, &file_type, FILE_WRITE_DATA, &o, NULL);
        if (rc)
            return rc;
        rc = file_setsize((struct file *)o, b);
        obj_unref(o);
        return rc;

    case SYS_FILE_FLUSH:
        rc = ht_lookup(t->ht, (uint32_t)a, &file_type, 0, &o, NULL);
        if (rc)
            return rc;
        rc = vfs_sync(((struct file *)o)->vol);
        obj_unref(o);
        return rc;

    case SYS_FILE_READDIR: {
        struct fs_dirent de;
        rc = user_check_out(t, c, sizeof(de));
        if (rc)
            return rc;
        rc = ht_lookup(t->ht, (uint32_t)a, &file_type, FILE_READ_DATA, &o, NULL);
        if (rc)
            return rc;
        rc = file_readdir((struct file *)o, (uint32_t)b, &de);
        obj_unref(o);
        return rc ? rc : user_copy_out(t, c, &de, sizeof(de));
    }
    }

    char full[FS_FULL_MAX + 1];
    struct volume *vol;
    const char *rel;
    rc = get_path(t, full, a, b);
    if (rc)
        return rc;
    switch (nr) {
    case SYS_FS_MKDIR:
        rc = resolve(t, full, FILE_CREATE, &vol, &rel);
        if (!rc) {
            rc = vfs_mkdir(vol, rel);
            obj_unref(&vol->o);
        }
        return rc;
    case SYS_FS_REMOVE:
        rc = resolve(t, full, FILE_DELETE_CHILD, &vol, &rel);
        if (!rc) {
            rc = vfs_remove(vol, rel);
            obj_unref(&vol->o);
        }
        return rc;
    case SYS_FS_STAT: {
        struct file_stat st;
        memset(&st, 0, sizeof(st));
        rc = user_check_out(t, c, sizeof(st));
        if (rc)
            return rc;
        rc = resolve(t, full, FILE_READ_DATA, &vol, &rel);
        if (!rc) {
            rc = vfs_stat(vol, rel, &st);
            obj_unref(&vol->o);
        }
        return rc ? rc : user_copy_out(t, c, &st, sizeof(st));
    }
    case SYS_FS_STATFS: {
        struct fs_statfs sf;
        rc = user_check_out(t, c, sizeof(sf));
        if (rc)
            return rc;
        rc = resolve(t, full, FILE_READ_DATA, &vol, &rel);
        if (!rc) {
            rc = vfs_statfs(vol, &sf);
            obj_unref(&vol->o);
        }
        return rc ? rc : user_copy_out(t, c, &sf, sizeof(sf));
    }
    case SYS_FS_RENAME: {
        char full2[FS_FULL_MAX + 1];
        struct volume *vol2;
        const char *rel2;
        rc = get_path(t, full2, c, d);
        if (rc)
            return rc;
        rc = resolve(t, full, FILE_DELETE_CHILD, &vol, &rel);
        if (rc)
            return rc;
        rc = resolve(t, full2, FILE_CREATE, &vol2, &rel2);
        if (!rc) {
            rc = vol == vol2 ? vfs_rename(vol, rel, rel2) : -E_XDEV;
            obj_unref(&vol2->o);
        }
        obj_unref(&vol->o);
        return rc;
    }
    }
    return -E_NOSYS;
}
