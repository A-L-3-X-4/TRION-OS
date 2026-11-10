#pragma once
#include "kernel.h"
#include "abi.h"

struct object;
struct gmap { uint32_t read, write, exec, all; };
struct obj_type {
    const char *name;
    uint32_t    id;
    uint32_t    valid_access;
    void      (*destroy)(struct object *o);
    int       (*wait)(struct object *o, uint64_t timeout_ms);
    const struct gmap *gmap;
};

struct object {
    uint32_t               refs;
    const struct obj_type *type;
    struct secdesc        *sd;
};

void *obj_create(const struct obj_type *t, size_t size);
void  obj_ref(struct object *o);
void  obj_unref(struct object *o);
unsigned obj_live(void);

struct handle_table;
struct handle_table *ht_create(void);
void ht_destroy(struct handle_table *ht);

int ht_insert(struct handle_table *ht, struct object *o, uint32_t access, uint32_t *h);
int ht_lookup(struct handle_table *ht, uint32_t h, const struct obj_type *type,
              uint32_t need, struct object **out, uint32_t *granted);
int ht_close(struct handle_table *ht, uint32_t h);
int ht_dup(struct handle_table *dst, struct handle_table *src, uint32_t h, uint32_t access,
           uint32_t *newh);
unsigned ht_count(struct handle_table *ht);

extern const struct obj_type event_type, semaphore_type;
int obj_event_create(bool manual_reset, bool signaled, struct object **out);
int obj_semaphore_create(uint32_t initial, uint32_t max, struct object **out);
int obj_wait(struct object *o, uint64_t timeout_ms);
long obj_event_op(struct object *o, uint32_t op);
long obj_semaphore_release(struct object *o, uint32_t n);

int  sys_obj_dispatch(uint64_t nr, uint64_t a, uint64_t b);

unsigned objtest_run(void);
