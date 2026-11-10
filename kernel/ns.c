#include "kernel.h"
#include "mm.h"
#include "irq.h"
#include "sched.h"
#include "task.h"
#include "ipc.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"

static size_t ns_strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

static void ns_strcpy(char *d, const char *s)
{
    while ((*d++ = *s++))
        ;
}

struct dent {
    char           name[NS_NAME_MAX + 1];
    struct object *obj;
};

struct dir {
    struct object o;
    struct dent  *ents;
    uint32_t      n, cap;
};

struct portref {
    struct object o;
    uint32_t      kname;
};

static struct ipc_space *kspace;
static struct dir *root;

struct ipc_space *ns_kernel_space(void) { return kspace; }

int ns_list(const char *path, unsigned idx, char *name, uint32_t *type)
{
    struct object *o;
    int rc = ns_lookup(path, &o);
    if (rc)
        return rc;
    if (o->type != &directory_type) {
        obj_unref(o);
        return -E_NOTDIR;
    }
    struct dir *dd = (struct dir *)o;
    preempt_disable();
    if (idx >= dd->n) {
        rc = -E_NOENT;
    } else {
        ns_strcpy(name, dd->ents[idx].name);
        *type = dd->ents[idx].obj->type->id;
    }
    preempt_enable();
    obj_unref(o);
    return rc;
}

static void dir_destroy(struct object *o)
{
    struct dir *d = (struct dir *)o;
    for (uint32_t i = 0; i < d->n; i++)
        obj_unref(d->ents[i].obj);
    kfree(d->ents);
}

static void portref_destroy(struct object *o)
{
    ipc_port_dealloc(kspace, ((struct portref *)o)->kname);
}

static const struct gmap dir_gmap = {
    DIR_QUERY | ACCESS_READ_CONTROL,
    DIR_CREATE | DIR_DELETE_CHILD | ACCESS_READ_CONTROL,
    DIR_TRAVERSE | ACCESS_READ_CONTROL,
    DIR_QUERY | DIR_TRAVERSE | DIR_CREATE | DIR_DELETE_CHILD | ACCESS_DELETE | ACCESS_SEC_RIGHTS };
static const struct gmap portref_gmap = {
    PORTREF_CONNECT | ACCESS_READ_CONTROL, PORTREF_CONNECT | ACCESS_READ_CONTROL,
    PORTREF_CONNECT | ACCESS_READ_CONTROL, PORTREF_CONNECT | ACCESS_DELETE | ACCESS_SEC_RIGHTS };

const struct obj_type directory_type = {
    "Directory", OBJ_DIR,
    DIR_QUERY | DIR_TRAVERSE | DIR_CREATE | DIR_DELETE_CHILD | ACCESS_DELETE | ACCESS_SEC_RIGHTS,
    dir_destroy, NULL, &dir_gmap };
const struct obj_type portref_type = {
    "PortRef", OBJ_PORTREF, PORTREF_CONNECT | ACCESS_DELETE | ACCESS_SEC_RIGHTS,
    portref_destroy, NULL, &portref_gmap };

static int ns_access(const struct token *tok, struct object *o, uint32_t need)
{
    uint32_t g;
    return sec_check(tok, o, need, &g);
}

int ns_init(void)
{
    kspace = ipc_space_create();
    root = obj_create(&directory_type, sizeof(*root));
    if (!kspace || !root)
        return -E_NOMEM;

    static const struct ace root_acl[] = {
        { ACE_ALLOW, 0, 0, SID_EVERYONE, ACCESS_GENERIC_READ | ACCESS_GENERIC_EXECUTE },
    };
    static const struct ace svc_acl[] = {
        { ACE_ALLOW, 0, 0, SID_EVERYONE, ACCESS_GENERIC_READ | ACCESS_GENERIC_EXECUTE | DIR_CREATE },
        { ACE_ALLOW, ACE_INHERIT | ACE_INHERIT_ONLY, 0, SID_EVERYONE,
          ACCESS_GENERIC_READ | ACCESS_GENERIC_EXECUTE },
        { ACE_ALLOW, ACE_INHERIT | ACE_INHERIT_ONLY, 0, SID_CREATOR_OWNER, ACCESS_GENERIC_ALL },
    };
    int rc = sec_kset(&root->o, SID_SYSTEM, root_acl, 1);
    if (rc)
        return rc;
    rc = ns_mkdir("\\Services");
    struct object *svc;
    if (!rc && !(rc = ns_lookup("\\Services", &svc))) {
        rc = sec_kset(svc, SID_SYSTEM, svc_acl, 3);
        obj_unref(svc);
    }
    return rc;
}

static bool name_ok(const char *s, size_t n)
{
    if (!n || n > NS_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] <= ' ' || s[i] == '\\' || s[i] == 0x7F)
            return false;
    return true;
}

static int next_comp(const char **p, char *out)
{
    const char *s = *p;
    if (!*s)
        return 0;
    if (*s != '\\')
        return -1;
    s++;
    size_t n = 0;
    while (s[n] && s[n] != '\\')
        n++;
    if (!name_ok(s, n))
        return -1;
    memcpy(out, s, n);
    out[n] = 0;
    *p = s + n;
    return (int)n;
}

static struct dent *dir_find(struct dir *d, const char *name)
{
    for (uint32_t i = 0; i < d->n; i++)
        if (strcmp(d->ents[i].name, name) == 0)
            return &d->ents[i];
    return NULL;
}

static bool path_ok(const char *path)
{
    size_t n = ns_strnlen(path, NS_PATH_MAX + 1);
    return n >= 1 && n <= NS_PATH_MAX && path[0] == '\\';
}

static int walk_parent(const struct token *tok, const char *path, struct dir **parent, char *leaf)
{
    if (!path_ok(path))
        return -E_INVAL;
    struct dir *cur = root;
    const char *p = path;
    char comp[NS_NAME_MAX + 1];
    leaf[0] = 0;
    if (path[1] == 0) {
        *parent = NULL;
        return 0;
    }
    for (;;) {
        int n = next_comp(&p, comp);
        if (n < 0)
            return -E_INVAL;
        if (n == 0)
            return -E_INVAL;
        if (*p == 0) {
            ns_strcpy(leaf, comp);
            *parent = cur;
            return 0;
        }
        int arc = ns_access(tok, &cur->o, DIR_TRAVERSE);
        if (arc)
            return arc;
        struct dent *e = dir_find(cur, comp);
        if (!e)
            return -E_NOENT;
        if (e->obj->type != &directory_type)
            return -E_NOTDIR;
        cur = (struct dir *)e->obj;
    }
}

static int dir_add(struct dir *d, const char *name, struct object *o)
{
    if (dir_find(d, name))
        return -E_EXIST;
    if (d->n >= NS_DIR_MAX)
        return -E_NOSPC;
    if (d->n == d->cap) {
        uint32_t nc = d->cap ? d->cap * 2 : 4;
        struct dent *t = krealloc(d->ents, nc * sizeof(*t));
        if (!t)
            return -E_NOMEM;
        d->ents = t;
        d->cap = nc;
    }
    ns_strcpy(d->ents[d->n].name, name);
    d->ents[d->n].obj = o;
    d->n++;
    o->refs++;
    return 0;
}

int ns_insert_as(const struct token *tok, const char *path, struct object *o)
{
    struct dir *parent;
    char leaf[NS_NAME_MAX + 1];
    preempt_disable();
    int rc = walk_parent(tok, path, &parent, leaf);
    if (!rc)
        rc = parent ? ns_access(tok, &parent->o, DIR_CREATE) : -E_EXIST;
    if (!rc)
        rc = dir_add(parent, leaf, o);
    if (!rc && tok) {
        rc = sec_inherit(o, &parent->o, tok);
        if (rc) {
            parent->n--;
            o->refs--;
        }
    }
    preempt_enable();
    return rc;
}

int ns_mkdir_as(const struct token *tok, const char *path)
{
    struct object *d = obj_create(&directory_type, sizeof(struct dir));
    if (!d)
        return -E_NOMEM;
    int rc = ns_insert_as(tok, path, d);
    obj_unref(d);
    return rc;
}

int ns_lookup_as(const struct token *tok, const char *path, struct object **out)
{
    struct dir *parent;
    char leaf[NS_NAME_MAX + 1];
    preempt_disable();
    int rc = walk_parent(tok, path, &parent, leaf);
    if (!rc) {
        struct object *o;
        if (!parent) {
            o = &root->o;
        } else if ((rc = ns_access(tok, &parent->o, DIR_TRAVERSE))) {
            o = NULL;
        } else {
            struct dent *e = dir_find(parent, leaf);
            o = e ? e->obj : NULL;
            if (!o)
                rc = -E_NOENT;
        }
        if (!rc) {
            o->refs++;
            *out = o;
        }
    }
    preempt_enable();
    return rc;
}

int ns_unlink_as(const struct token *tok, const char *path)
{
    struct dir *parent;
    char leaf[NS_NAME_MAX + 1];
    preempt_disable();
    int rc = walk_parent(tok, path, &parent, leaf);
    if (!rc && !parent)
        rc = -E_INVAL;
    if (!rc)
        rc = ns_access(tok, &parent->o, DIR_TRAVERSE);
    if (!rc) {
        struct dent *e = dir_find(parent, leaf);
        if (!e) {
            rc = -E_NOENT;
        } else if (ns_access(tok, e->obj, ACCESS_DELETE) && ns_access(tok, &parent->o, DIR_DELETE_CHILD)) {
            rc = -E_ACCES;
        } else if (e->obj->type == &directory_type && ((struct dir *)e->obj)->n) {
            rc = -E_NOTEMPTY;
        } else {
            struct object *o = e->obj;
            *e = parent->ents[--parent->n];
            obj_unref(o);
        }
    }
    preempt_enable();
    return rc;
}

int ns_publish_as(const struct token *tok, const char *path, struct ipc_space *s, uint32_t name)
{
    struct portref *pr = obj_create(&portref_type, sizeof(*pr));
    if (!pr)
        return -E_NOMEM;
    int rc = ipc_grant(kspace, s, name, MSG_COPY_SEND, &pr->kname);
    if (!rc) {
        rc = ns_insert_as(tok, path, &pr->o);
        if (rc) {
            ipc_port_dealloc(kspace, pr->kname);
            pr->kname = 0;
        }
    }
    obj_unref(&pr->o);
    return rc;
}

int ns_lookup_port_as(const struct token *tok, const char *path, struct ipc_space *s, uint32_t *name)
{
    struct object *o;
    int rc = ns_lookup_as(tok, path, &o);
    if (rc)
        return rc;
    if (o->type != &portref_type)
        rc = -E_INVAL;
    else if (!(rc = ns_access(tok, o, PORTREF_CONNECT)))
        rc = ipc_grant(s, kspace, ((struct portref *)o)->kname, MSG_COPY_SEND, name);
    obj_unref(o);
    return rc;
}

int ns_insert(const char *path, struct object *o)  { return ns_insert_as(NULL, path, o); }
int ns_mkdir(const char *path)                     { return ns_mkdir_as(NULL, path); }
int ns_lookup(const char *path, struct object **o) { return ns_lookup_as(NULL, path, o); }
int ns_unlink(const char *path)                    { return ns_unlink_as(NULL, path); }
int ns_publish(const char *path, struct ipc_space *s, uint32_t name)
{
    return ns_publish_as(NULL, path, s, name);
}
int ns_lookup_port(const char *path, struct ipc_space *s, uint32_t *name)
{
    return ns_lookup_port_as(NULL, path, s, name);
}

static int get_path(struct task *t, char *buf, uint64_t uptr, uint64_t len)
{
    if (len == 0 || len > NS_PATH_MAX)
        return -E_INVAL;
    int rc = user_copy_in(t, buf, uptr, len);
    if (rc)
        return rc;
    buf[len] = 0;
    if (strlen(buf) != len)
        return -E_INVAL;
    return 0;
}

long sys_ns_dispatch(struct task *t, uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    char path[NS_PATH_MAX + 1];
    struct object *o = NULL;
    const struct token *tok = &t->tok;
    long rc = get_path(t, path, a, b);
    if (rc)
        return rc;

    switch (nr) {
    case SYS_NS_MKDIR:
        return ns_mkdir_as(tok, path);

    case SYS_NS_UNLINK:
        return ns_unlink_as(tok, path);

    case SYS_NS_INSERT:
        rc = ht_lookup(t->ht, (uint32_t)c, NULL, 0, &o, NULL);
        if (rc)
            return rc;
        if (o->type->id != OBJ_EVENT && o->type->id != OBJ_SEMAPHORE)
            rc = -E_INVAL;
        else
            rc = ns_insert_as(tok, path, o);
        obj_unref(o);
        return rc;

    case SYS_NS_OPEN: {
        rc = ns_lookup_as(tok, path, &o);
        if (rc)
            return rc;
        uint32_t want = c ? (uint32_t)c : ACCESS_MAXIMUM, granted = 0, h = 0;
        rc = sec_check(tok, o, want, &granted);
        if (!rc)
            rc = ht_insert(t->ht, o, granted, &h);
        obj_unref(o);
        return rc ? rc : (long)h;
    }

    case SYS_NS_PUBLISH:
        return ns_publish_as(tok, path, t->ipc, (uint32_t)c);

    case SYS_NS_LOOKUP_PORT: {
        uint32_t n = 0;
        rc = ns_lookup_port_as(tok, path, t->ipc, &n);
        return rc ? rc : (long)n;
    }

    case SYS_NS_QUERY: {
        rc = ns_lookup_as(tok, path, &o);
        if (rc)
            return rc;
        struct ns_dirent de;
        memset(&de, 0, sizeof(de));
        if (o->type != &directory_type) {
            rc = -E_NOTDIR;
        } else if ((rc = ns_access(tok, o, DIR_QUERY))) {
        } else {
            struct dir *dd = (struct dir *)o;
            preempt_disable();
            if (c >= dd->n) {
                rc = -E_NOENT;
            } else {
                ns_strcpy(de.name, dd->ents[c].name);
                de.type = dd->ents[c].obj->type->id;
            }
            preempt_enable();
        }
        obj_unref(o);
        if (rc)
            return rc;
        return user_copy_out(t, d, &de, sizeof(de));
    }
    }
    return -E_NOSYS;
}
