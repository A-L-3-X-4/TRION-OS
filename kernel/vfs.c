#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"
#include "vfs.h"

#define MAX_FS 4
static const struct vfs_ops *fstypes[MAX_FS];
static unsigned nfs;
static unsigned n_vnodes_live;

#define MAX_MOUNTS 16
static struct volume *mounts[MAX_MOUNTS];

void vfs_register_fs(const struct vfs_ops *ops)
{
    if (nfs < MAX_FS)
        fstypes[nfs++] = ops;
}

unsigned vfs_live_vnodes(void) { return n_vnodes_live; }

struct vnode *vnode_get(struct vfs_sb *sb, uint64_t key, size_t size, bool *fresh)
{
    list_for_each(n, &sb->vnodes) {
        struct vnode *v = container_of(n, struct vnode, link);
        if (v->key == key) {
            v->refs++;
            *fresh = false;
            return v;
        }
    }
    struct vnode *v = kzalloc(size);
    if (!v)
        return NULL;
    v->sb = sb;
    v->key = key;
    v->refs = 1;
    list_add_tail(&sb->vnodes, &v->link);
    sb->nvnodes++;
    preempt_disable();
    n_vnodes_live++;
    preempt_enable();
    *fresh = true;
    return v;
}

void vnode_ref(struct vnode *v) { v->refs++; }

void vnode_put(struct vnode *v)
{
    if (--v->refs)
        return;
    list_del(&v->link);
    v->sb->nvnodes--;
    preempt_disable();
    n_vnodes_live--;
    preempt_enable();
    kfree(v);
}

static void drain(struct vfs_sb *sb)
{
    preempt_disable();
    struct vnode *v = sb->deferred;
    sb->deferred = NULL;
    preempt_enable();
    while (v) {
        struct vnode *next = v->dnext;
        uint32_t n = v->pending;
        v->pending = 0;
        v->dnext = NULL;
        v->refs -= n - 1;
        vnode_put(v);
        v = next;
    }
}

static void sb_lock(struct vfs_sb *sb)
{
    sem_down(&sb->lock);
    drain(sb);
}
static void sb_unlock(struct vfs_sb *sb) { sem_up(&sb->lock); }

static const struct gmap volume_gmap = {
    FILE_READ_DATA | ACCESS_READ_CONTROL,
    FILE_WRITE_DATA | FILE_CREATE | FILE_DELETE_CHILD | ACCESS_READ_CONTROL,
    FILE_READ_DATA | ACCESS_READ_CONTROL,
    FILE_READ_DATA | FILE_WRITE_DATA | FILE_CREATE | FILE_DELETE_CHILD | ACCESS_SEC_RIGHTS };
#define VOLUME_VALID (FILE_READ_DATA | FILE_WRITE_DATA | FILE_CREATE | FILE_DELETE_CHILD | ACCESS_SEC_RIGHTS)

static void volume_destroy(struct object *o)
{
    struct volume *v = (struct volume *)o;
    struct vfs_sb *sb = v->sb;
    if (!sb)
        return;
    drain(sb);
    if (sb->root) {
        vnode_put(sb->root);
        sb->root = NULL;
    }
    if (sb->ops->unmount)
        sb->ops->unmount(sb);
    kfree(sb);
}

static void file_destroy(struct object *o)
{
    struct file *f = (struct file *)o;
    struct vnode *v = f->v;
    if (v) {
        struct vfs_sb *sb = v->sb;
        if (v->pending++ == 0) {
            v->dnext = sb->deferred;
            sb->deferred = v;
        }
    }
    if (f->vol)
        obj_unref(&f->vol->o);
}

const struct obj_type volume_type = {
    "Volume", OBJ_VOLUME, VOLUME_VALID, volume_destroy, NULL, &volume_gmap };
const struct obj_type file_type = {
    "File", OBJ_FILE, FILE_VALID_HANDLE, file_destroy, NULL, NULL };

static void copy_name(char *d, const char *s, size_t cap)
{
    size_t i = 0;
    while (s[i] && i < cap - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

void vfs_init(void)
{
    vfs_register_fs(&fat32_ops);
    if (ns_mkdir("\\Volumes"))
        kpanic("vfs: cannot create \\Volumes");
    static const struct ace acl[] = {
        { ACE_ALLOW, 0, 0, SID_EVERYONE, ACCESS_GENERIC_READ | ACCESS_GENERIC_EXECUTE },
    };
    struct object *d;
    if (!ns_lookup("\\Volumes", &d)) {
        sec_kset(d, SID_SYSTEM, acl, 1);
        obj_unref(d);
    }
}

int vfs_dos_volume(int letter, char out[VFS_VOL_NAME_MAX + 1])
{
    if (letter >= 'a' && letter <= 'z')
        letter -= 32;
    if (letter < 'C' || letter > 'Z')
        return -E_NOENT;
    // There are no real drive letters here, C: is just the first volume under \Volumes, D: is the second and so on, so your drive letters are decided by mount order. Enjoy
    int want = letter - 'C';
    char name[NS_NAME_MAX + 1];
    uint32_t type;
    for (unsigned i = 0; ns_list("\\Volumes", i, name, &type) == 0; i++) {
        if (type != OBJ_VOLUME)
            continue;
        if (want-- == 0) {
            size_t n = strlen(name);
            if (n > VFS_VOL_NAME_MAX)
                return -E_NOENT;
            memcpy(out, name, n + 1);
            return 0;
        }
    }
    return -E_NOENT;
}

static bool alias_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

int vfs_alias_path(const char *in, char *out, size_t cap, bool *changed)
{
    char vol[VFS_VOL_NAME_MAX + 1];
    const char *rest;
    *changed = false;
    if (alias_alpha(in[0]) && in[1] == ':' && (in[2] == '\\' || in[2] == '/' || !in[2])) {
        if (vfs_dos_volume(in[0], vol))
            return -E_NOENT;
        rest = in + 2;
    } else if (in[0] == '\\' && in[1] != '\\') {
        if (vfs_dos_volume('C', vol))
            return -E_NOENT;
        rest = in;
    } else {
        size_t n = strlen(in);
        if (n >= cap)
            return -E_NAMETOOLONG;
        memcpy(out, in, n + 1);
        return 0;
    }
    size_t o = 0;
    static const char pre[] = "/Volumes/";
    if (sizeof(pre) - 1 + strlen(vol) + 1 >= cap)
        return -E_NAMETOOLONG;
    memcpy(out, pre, sizeof(pre) - 1);
    o = sizeof(pre) - 1;
    memcpy(out + o, vol, strlen(vol));
    o += strlen(vol);
    // Copy the rest of the path while turning every backslash into a forward slash, because Microsoft picked \ for paths and now I have to undo it
    for (; *rest; rest++) {
        if (o + 2 >= cap)
            return -E_NAMETOOLONG;
        out[o++] = *rest == '\\' ? '/' : *rest;
    }
    if (!*in || o == 0)
        return -E_INVAL;
    out[o] = 0;
    *changed = true;
    return 0;
}

int vfs_mount(const char *name, struct blkdev *dev, bool ro, struct volume **out)
{
    if (!name[0] || strlen(name) > VFS_VOL_NAME_MAX)
        return -E_INVAL;
    char path[VFS_VOL_NAME_MAX + 16];
    size_t n = 0;
    for (const char *p = "\\Volumes\\"; *p; p++)
        path[n++] = *p;
    for (const char *p = name; *p; p++)
        path[n++] = *p;
    path[n] = 0;

    struct object *dummy;
    if (!ns_lookup(path, &dummy)) {
        obj_unref(dummy);
        return -E_EXIST;
    }
    {
        struct volume *have = vfs_volume_for_dev(dev);
        if (have) {
            obj_unref(&have->o);
            return -E_BUSY;
        }
    }

    struct vfs_sb *sb = kzalloc(sizeof(*sb));
    if (!sb)
        return -E_NOMEM;
    sb->dev = dev;
    sb->ro = ro || dev->ro;
    sem_init(&sb->lock, 1);
    list_init(&sb->vnodes);

    int rc = -E_INVAL;
    for (unsigned i = 0; i < nfs; i++) {
        sb->ops = fstypes[i];
        rc = sb->ops->mount(sb);
        if (rc != -E_INVAL)
            break;
    }
    if (rc) {
        kfree(sb);
        return rc;
    }

    struct volume *v = obj_create(&volume_type, sizeof(*v));
    if (!v) {
        vnode_put(sb->root);
        sb->ops->unmount(sb);
        kfree(sb);
        return -E_NOMEM;
    }
    v->sb = sb;
    copy_name(v->name, name, sizeof(v->name));
    static const struct ace acl[] = {
        { ACE_ALLOW, 0, 0, SID_EVERYONE, ACCESS_GENERIC_READ | ACCESS_GENERIC_EXECUTE },
    };
    rc = sec_kset(&v->o, SID_SYSTEM, acl, 1);
    if (!rc)
        rc = ns_insert(path, &v->o);
    if (!rc) {
        rc = -E_NOSPC;
        preempt_disable();
        for (int i = 0; i < MAX_MOUNTS; i++)
            if (!mounts[i]) {
                mounts[i] = v;
                rc = 0;
                break;
            }
        preempt_enable();
        if (rc)
            ns_unlink(path);
    }
    if (rc) {
        obj_unref(&v->o);
        return rc;
    }
    if (out)
        *out = v;
    else
        obj_unref(&v->o);
    return 0;
}

int vfs_unmount(const char *name)
{
    char path[VFS_VOL_NAME_MAX + 16];
    size_t n = 0;
    for (const char *p = "\\Volumes\\"; *p; p++)
        path[n++] = *p;
    if (strlen(name) > VFS_VOL_NAME_MAX)
        return -E_INVAL;
    for (const char *p = name; *p; p++)
        path[n++] = *p;
    path[n] = 0;

    struct object *o;
    int rc = ns_lookup(path, &o);
    if (rc)
        return rc;
    if (o->type != &volume_type) {
        obj_unref(o);
        return -E_INVAL;
    }
    struct volume *v = (struct volume *)o;
    struct vfs_sb *sb = v->sb;
    sb_lock(sb);
    if (sb->nvnodes > 1) {
        sb_unlock(sb);
        obj_unref(o);
        return -E_BUSY;
    }
    sb->dead = true;
    blk_flush(sb->dev);
    sb_unlock(sb);
    preempt_disable();
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i] == v)
            mounts[i] = NULL;
    preempt_enable();
    ns_unlink(path);
    obj_unref(o);
    return 0;
}

struct volume *vfs_volume_for_dev(struct blkdev *dev)
{
    struct volume *found = NULL;
    preempt_disable();
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i] && mounts[i]->sb->dev == dev && !mounts[i]->sb->dead) {
            found = mounts[i];
            obj_ref(&found->o);
            break;
        }
    preempt_enable();
    return found;
}

int vfs_automount(void)
{
    int mounted = 0;
    for (unsigned i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_nth(i);
        if (!d)
            break;
        struct volume *have = vfs_volume_for_dev(d);
        if (have) {
            obj_unref(&have->o);
            continue;
        }
        struct volume *v;
        int rc = vfs_mount(d->name, d, false, &v);
        if (!rc) {
            klog(LOG_OK, "vfs: \\Volumes\\%s = %s on %s, label \"%s\"", v->name, v->sb->ops->name,
                 d->name, v->sb->label);
            obj_unref(&v->o);
            mounted++;
        } else if (rc != -E_INVAL) {
            klog(LOG_WARN, "vfs: cannot mount %s: error %d", d->name, rc);
        }
    }
    return mounted;
}

static bool is_sep(char c) { return c == '\\' || c == '/'; }

static int next_component(const char **p, char *out)
{
    const char *s = *p;
    while (is_sep(*s))
        s++;
    if (!*s) {
        *p = s;
        return 0;
    }
    size_t n = 0;
    while (s[n] && !is_sep(s[n]))
        n++;
    *p = s + n;
    if (n > VFS_NAME_MAX)
        return -E_NAMETOOLONG;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
        return -E_INVAL;
    memcpy(out, s, n);
    out[n] = 0;
    return (int)n;
}

static int walk_parent(struct vfs_sb *sb, const char *rel, struct vnode **parent, char *leaf)
{
    struct vnode *cur = sb->root;
    vnode_ref(cur);
    leaf[0] = 0;
    const char *p = rel;
    char comp[VFS_NAME_MAX + 1];
    int n = next_component(&p, comp);
    while (n > 0) {
        const char *q = p;
        char nextc[VFS_NAME_MAX + 1];
        int nn = next_component(&q, nextc);
        if (nn < 0) {
            vnode_put(cur);
            return nn;
        }
        if (nn == 0) {
            memcpy(leaf, comp, (size_t)n + 1);
            *parent = cur;
            return 0;
        }
        if (cur->type != FT_DIR) {
            vnode_put(cur);
            return -E_NOTDIR;
        }
        struct vnode *child;
        int rc = sb->ops->lookup(cur, comp, &child);
        vnode_put(cur);
        if (rc)
            return rc;
        cur = child;
        memcpy(comp, nextc, (size_t)nn + 1);
        n = nn;
        p = q;
    }
    if (n < 0) {
        vnode_put(cur);
        return n;
    }
    *parent = cur;
    return 0;
}

static int walk(struct vfs_sb *sb, const char *rel, struct vnode **out)
{
    struct vnode *parent;
    char leaf[VFS_NAME_MAX + 1];
    int rc = walk_parent(sb, rel, &parent, leaf);
    if (rc)
        return rc;
    if (!leaf[0]) {
        *out = parent;
        return 0;
    }
    if (parent->type != FT_DIR) {
        vnode_put(parent);
        return -E_NOTDIR;
    }
    rc = sb->ops->lookup(parent, leaf, out);
    vnode_put(parent);
    return rc;
}

static int check_alive(struct volume *v)
{
    return v->sb->dead ? -E_NOENT : 0;
}

static void stat_of(struct vnode *v, struct file_stat *st)
{
    st->size = v->size;
    st->type = v->type;
    st->attr = v->attr;
    st->ctime = v->ctime;
    st->mtime = v->mtime;
    st->atime = v->atime;
}

int vfs_open(struct volume *vol, const char *rel, uint32_t disp, uint32_t access, uint32_t flags,
             bool may_create, struct file **out)
{
    if (access & ~FILE_VALID_HANDLE)
        return -E_INVAL;
    if (disp > FD_OVERWRITE_IF)
        return -E_INVAL;
    if (disp == FD_OVERWRITE_IF && !(access & FILE_WRITE_DATA))
        return -E_INVAL;
    if ((flags & FO_DIRECTORY) && disp != FD_OPEN)
        return -E_INVAL;
    struct vfs_sb *sb = vol->sb;
    struct file *f = obj_create(&file_type, sizeof(*f));
    if (!f)
        return -E_NOMEM;

    sb_lock(sb);
    int rc = check_alive(vol);
    struct vnode *parent = NULL, *v = NULL;
    char leaf[VFS_NAME_MAX + 1];
    if (!rc)
        rc = walk_parent(sb, rel, &parent, leaf);
    if (!rc) {
        if (!leaf[0]) {
            v = parent;
            parent = NULL;
        } else if (parent->type != FT_DIR) {
            rc = -E_NOTDIR;
        } else {
            rc = sb->ops->lookup(parent, leaf, &v);
        }
    }
    if (!rc) {
        if (disp == FD_CREATE)
            rc = -E_EXIST;
        else if ((flags & FO_DIRECTORY) && v->type != FT_DIR)
            rc = -E_NOTDIR;
        else if (v->type == FT_DIR && (access & FILE_WRITE_DATA))
            rc = -E_ISDIR;
        else if ((access & FILE_WRITE_DATA) && (sb->ro || (v->attr & FA_RDONLY)))
            rc = sb->ro ? -E_ROFS : -E_ACCES;
        else if (disp == FD_OVERWRITE_IF)
            rc = sb->ops->truncate(v, 0);
    } else if (rc == -E_NOENT && parent && disp != FD_OPEN) {
        if (sb->ro)
            rc = -E_ROFS;
        else if (!may_create)
            rc = -E_ACCES;
        else
            rc = sb->ops->create(parent, leaf, FT_FILE, &v);
    }
    if (parent)
        vnode_put(parent);
    if (rc) {
        if (v)
            vnode_put(v);
        sb_unlock(sb);
        f->v = NULL;
        obj_unref(&f->o);
        return rc;
    }
    f->v = v;
    f->vol = vol;
    obj_ref(&vol->o);
    f->access = access;
    sb_unlock(sb);
    *out = f;
    return 0;
}

int vfs_mkdir(struct volume *vol, const char *rel)
{
    struct vfs_sb *sb = vol->sb;
    sb_lock(sb);
    int rc = check_alive(vol);
    struct vnode *parent = NULL, *v = NULL;
    char leaf[VFS_NAME_MAX + 1];
    if (!rc && sb->ro)
        rc = -E_ROFS;
    if (!rc)
        rc = walk_parent(sb, rel, &parent, leaf);
    if (!rc) {
        if (!leaf[0])
            rc = -E_EXIST;
        else if (parent->type != FT_DIR)
            rc = -E_NOTDIR;
        else
            rc = sb->ops->create(parent, leaf, FT_DIR, &v);
        vnode_put(parent);
    }
    if (!rc)
        vnode_put(v);
    sb_unlock(sb);
    return rc;
}

int vfs_remove(struct volume *vol, const char *rel)
{
    struct vfs_sb *sb = vol->sb;
    sb_lock(sb);
    int rc = check_alive(vol);
    struct vnode *parent = NULL, *v = NULL;
    char leaf[VFS_NAME_MAX + 1];
    if (!rc && sb->ro)
        rc = -E_ROFS;
    if (!rc)
        rc = walk_parent(sb, rel, &parent, leaf);
    if (!rc) {
        if (!leaf[0])
            rc = -E_INVAL;
        else if (parent->type != FT_DIR)
            rc = -E_NOTDIR;
        else
            rc = sb->ops->lookup(parent, leaf, &v);
        if (!rc) {
            if (v->refs > 1)
                rc = -E_BUSY;
            else if (v->attr & FA_RDONLY)
                rc = -E_ACCES;
            else
                rc = sb->ops->remove(parent, v);
            vnode_put(v);
        }
        vnode_put(parent);
    }
    sb_unlock(sb);
    return rc;
}

int vfs_rename(struct volume *vol, const char *from, const char *to)
{
    struct vfs_sb *sb = vol->sb;
    sb_lock(sb);
    int rc = check_alive(vol);
    struct vnode *op = NULL, *np = NULL, *v = NULL;
    char oleaf[VFS_NAME_MAX + 1], nleaf[VFS_NAME_MAX + 1];
    if (!rc && sb->ro)
        rc = -E_ROFS;
    if (!rc)
        rc = walk_parent(sb, from, &op, oleaf);
    if (!rc) {
        rc = walk_parent(sb, to, &np, nleaf);
        if (rc) {
            vnode_put(op);
            op = NULL;
        }
    }
    if (!rc) {
        if (!oleaf[0] || !nleaf[0])
            rc = -E_INVAL;
        else if (op->type != FT_DIR || np->type != FT_DIR)
            rc = -E_NOTDIR;
        else
            rc = sb->ops->lookup(op, oleaf, &v);
        if (!rc) {
            if (v->refs > 1)
                rc = -E_BUSY;
            else if (v->attr & FA_RDONLY)
                rc = -E_ACCES;
            else
                rc = sb->ops->rename(op, v, np, nleaf);
            vnode_put(v);
        }
        vnode_put(op);
        vnode_put(np);
    }
    sb_unlock(sb);
    return rc;
}

int vfs_stat(struct volume *vol, const char *rel, struct file_stat *st)
{
    struct vfs_sb *sb = vol->sb;
    sb_lock(sb);
    struct vnode *v;
    int rc = check_alive(vol);
    if (!rc)
        rc = walk(sb, rel, &v);
    if (!rc) {
        stat_of(v, st);
        vnode_put(v);
    }
    sb_unlock(sb);
    return rc;
}

int vfs_statfs(struct volume *vol, struct fs_statfs *out)
{
    struct vfs_sb *sb = vol->sb;
    sb_lock(sb);
    int rc = check_alive(vol);
    if (!rc) {
        memset(out, 0, sizeof(*out));
        rc = sb->ops->statfs(sb, out);
    }
    sb_unlock(sb);
    return rc;
}

int vfs_sync(struct volume *vol)
{
    struct vfs_sb *sb = vol->sb;
    sb_lock(sb);
    int rc = check_alive(vol);
    if (!rc)
        rc = blk_flush(sb->dev);
    sb_unlock(sb);
    return rc;
}

long file_pread(struct file *f, uint64_t off, void *buf, size_t n)
{
    if (!(f->access & FILE_READ_DATA))
        return -E_ACCES;
    struct vfs_sb *sb = f->v->sb;
    sb_lock(sb);
    long rc = check_alive(f->vol);
    if (!rc) {
        if (f->v->type == FT_DIR)
            rc = -E_ISDIR;
        else
            rc = sb->ops->read(f->v, off, buf, n);
    }
    sb_unlock(sb);
    return rc;
}

long file_pwrite(struct file *f, uint64_t off, const void *buf, size_t n)
{
    if (!(f->access & FILE_WRITE_DATA))
        return -E_ACCES;
    struct vfs_sb *sb = f->v->sb;
    sb_lock(sb);
    long rc = check_alive(f->vol);
    if (!rc) {
        if (sb->ro)
            rc = -E_ROFS;
        else if (f->v->type == FT_DIR)
            rc = -E_ISDIR;
        else
            rc = sb->ops->write(f->v, off, buf, n);
    }
    sb_unlock(sb);
    return rc;
}

long file_read(struct file *f, void *buf, size_t n)
{
    if (!(f->access & FILE_READ_DATA))
        return -E_ACCES;
    struct vfs_sb *sb = f->v->sb;
    sb_lock(sb);
    long rc = check_alive(f->vol);
    if (!rc) {
        if (f->v->type == FT_DIR)
            rc = -E_ISDIR;
        else {
            rc = sb->ops->read(f->v, f->pos, buf, n);
            if (rc > 0)
                f->pos += (uint64_t)rc;
        }
    }
    sb_unlock(sb);
    return rc;
}

long file_write(struct file *f, const void *buf, size_t n)
{
    if (!(f->access & FILE_WRITE_DATA))
        return -E_ACCES;
    struct vfs_sb *sb = f->v->sb;
    sb_lock(sb);
    long rc = check_alive(f->vol);
    if (!rc) {
        if (sb->ro)
            rc = -E_ROFS;
        else if (f->v->type == FT_DIR)
            rc = -E_ISDIR;
        else {
            rc = sb->ops->write(f->v, f->pos, buf, n);
            if (rc > 0)
                f->pos += (uint64_t)rc;
        }
    }
    sb_unlock(sb);
    return rc;
}

long file_seek(struct file *f, int64_t off, int whence)
{
    struct vfs_sb *sb = f->v->sb;
    sb_lock(sb);
    long rc = check_alive(f->vol);
    if (!rc) {
        int64_t base;
        switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (int64_t)f->pos; break;
        case SEEK_END: base = (int64_t)f->v->size; break;
        default:       base = -1; rc = -E_INVAL; break;
        }
        if (!rc) {
            int64_t np = base + off;
            if (np < 0 || np > 0xFFFFFFFFLL)
                rc = -E_INVAL;
            else {
                f->pos = (uint64_t)np;
                rc = np;
            }
        }
    }
    sb_unlock(sb);
    return rc;
}

int file_stat(struct file *f, struct file_stat *st)
{
    struct vfs_sb *sb = f->v->sb;
    sb_lock(sb);
    int rc = check_alive(f->vol);
    if (!rc)
        stat_of(f->v, st);
    sb_unlock(sb);
    return rc;
}

int file_setsize(struct file *f, uint64_t size)
{
    if (!(f->access & FILE_WRITE_DATA))
        return -E_ACCES;
    struct vfs_sb *sb = f->v->sb;
    sb_lock(sb);
    int rc = check_alive(f->vol);
    if (!rc) {
        if (sb->ro)
            rc = -E_ROFS;
        else if (f->v->type == FT_DIR)
            rc = -E_ISDIR;
        else
            rc = sb->ops->truncate(f->v, size);
    }
    sb_unlock(sb);
    return rc;
}

int file_readdir(struct file *f, uint32_t index, struct fs_dirent *out)
{
    if (!(f->access & FILE_READ_DATA))
        return -E_ACCES;
    struct vfs_sb *sb = f->v->sb;
    sb_lock(sb);
    int rc = check_alive(f->vol);
    if (!rc) {
        if (f->v->type != FT_DIR)
            rc = -E_NOTDIR;
        else
            rc = sb->ops->readdir(f->v, index, out);
    }
    sb_unlock(sb);
    return rc;
}
