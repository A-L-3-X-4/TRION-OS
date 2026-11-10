#include "kernel.h"
#include "mm.h"
#include "irq.h"
#include "sched.h"
#include "obj.h"

#define HT_MAX   512
#define HT_INIT  8
#define GEN_MASK 0xFFFFFu

struct hentry {
    struct object *obj;
    uint32_t       gen;
    uint32_t       access;
};

struct handle_table {
    struct hentry *tab;
    uint32_t       cap, used;
};

static unsigned n_objs;
unsigned obj_live(void) { return n_objs; }

static inline void lock(void)
{
    if (g_irq_depth)
        kpanic("obj: used from interrupt context");
    preempt_disable();
}
static inline void unlock(void) { preempt_enable(); }

void *obj_create(const struct obj_type *t, size_t size)
{
    struct object *o = kzalloc(size);
    if (!o)
        return NULL;
    o->refs = 1;
    o->type = t;
    lock();
    n_objs++;
    unlock();
    return o;
}

void obj_ref(struct object *o)
{
    lock();
    o->refs++;
    unlock();
}

static void obj_unref_locked(struct object *o)
{
    // Refcount hit 0 so run the type's destroy hook and free the sd, last one out turns off the lights
    if (--o->refs)
        return;
    n_objs--;
    if (o->type->destroy)
        o->type->destroy(o);
    kfree(o->sd);
    kfree(o);
}

void obj_unref(struct object *o)
{
    lock();
    obj_unref_locked(o);
    unlock();
}

struct handle_table *ht_create(void)
{
    struct handle_table *ht = kzalloc(sizeof(*ht));
    if (!ht)
        return NULL;
    ht->tab = kzalloc(HT_INIT * sizeof(struct hentry));
    if (!ht->tab) {
        kfree(ht);
        return NULL;
    }
    ht->cap = HT_INIT;
    return ht;
}

void ht_destroy(struct handle_table *ht)
{
    if (!ht)
        return;
    lock();
    for (uint32_t i = 0; i < ht->cap; i++)
        if (ht->tab[i].obj) {
            struct object *o = ht->tab[i].obj;
            ht->tab[i].obj = NULL;
            obj_unref_locked(o);
        }
    unlock();
    kfree(ht->tab);
    kfree(ht);
}

static struct hentry *h_lookup(struct handle_table *ht, uint32_t h)
{
    uint32_t idx = h & 0xFFF;
    if (!idx || idx > ht->cap)
        return NULL;
    struct hentry *e = &ht->tab[idx - 1];
    if (!e->obj || e->gen != (h >> 12))
        return NULL;
    return e;
}

static int ht_alloc_slot(struct handle_table *ht, uint32_t *h)
{
    for (;;) {
        for (uint32_t i = 0; i < ht->cap; i++)
            if (!ht->tab[i].obj) {
                *h = (ht->tab[i].gen << 12) | (i + 1);
                return (int)i;
            }
        uint32_t ncap = ht->cap * 2;
        if (ncap > HT_MAX)
            ncap = HT_MAX;
        if (ncap <= ht->cap)
            return -E_NOSPC;
        struct hentry *t = krealloc(ht->tab, ncap * sizeof(*t));
        if (!t)
            return -E_NOMEM;
        memset(t + ht->cap, 0, (ncap - ht->cap) * sizeof(*t));
        ht->tab = t;
        ht->cap = ncap;
    }
}

int ht_insert(struct handle_table *ht, struct object *o, uint32_t access, uint32_t *h)
{
    if (access & ~o->type->valid_access)
        return -E_INVAL;
    lock();
    int i = ht_alloc_slot(ht, h);
    if (i >= 0) {
        ht->tab[i].obj = o;
        ht->tab[i].access = access;
        o->refs++;
        ht->used++;
    }
    unlock();
    return i < 0 ? i : 0;
}

int ht_lookup(struct handle_table *ht, uint32_t h, const struct obj_type *type, uint32_t need,
              struct object **out, uint32_t *granted)
{
    lock();
    struct hentry *e = h_lookup(ht, h);
    int rc = 0;
    if (!e)
        rc = -E_BADF;
    else if (type && e->obj->type != type)
        rc = -E_INVAL;
    else if ((e->access & need) != need)
        rc = -E_ACCES;
    else {
        e->obj->refs++;
        *out = e->obj;
        if (granted)
            *granted = e->access;
    }
    unlock();
    return rc;
}

int ht_close(struct handle_table *ht, uint32_t h)
{
    lock();
    struct hentry *e = h_lookup(ht, h);
    int rc = 0;
    if (!e) {
        rc = -E_BADF;
    } else {
        struct object *o = e->obj;
        e->obj = NULL;
        e->access = 0;
        e->gen = (e->gen + 1) & GEN_MASK;
        ht->used--;
        obj_unref_locked(o);
    }
    unlock();
    return rc;
}

int ht_dup(struct handle_table *dst, struct handle_table *src, uint32_t h, uint32_t access,
           uint32_t *newh)
{
    lock();
    int rc = 0;
    struct hentry *e = h_lookup(src, h);
    if (!e) {
        rc = -E_BADF;
    } else {
        uint32_t have = e->access;
        if (access && (access & ~have)) {
            rc = -E_ACCES;
        } else {
            struct object *o = e->obj;
            uint32_t acc = access ? access : have;
            o->refs++;
            int i = ht_alloc_slot(dst, newh);
            if (i < 0) {
                rc = i;
                obj_unref_locked(o);
            } else {
                dst->tab[i].obj = o;
                dst->tab[i].access = acc;
                dst->used++;
            }
        }
    }
    unlock();
    return rc;
}

unsigned ht_count(struct handle_table *ht) { return ht->used; }
