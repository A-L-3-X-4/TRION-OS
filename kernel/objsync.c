#include "kernel.h"
#include "mm.h"
#include "irq.h"
#include "sched.h"
#include "task.h"
#include "obj.h"
#include "sec.h"
#include "deadline.h"

struct event {
    struct object o;
    bool          manual, signaled;
    uint32_t      waiters;
    struct sem    hint;
};

struct semobj {
    struct object o;
    uint32_t      count, max, waiters;
    struct sem    hint;
};

static void wake_n(struct sem *s, uint32_t n)
{
    while (n--)
        sem_up(s);
}

static int event_wait(struct object *o, uint64_t tmo)
{
    struct event *e = (struct event *)o;
    struct deadline dl;
    deadline_init(&dl, tmo);
    for (;;) {
        preempt_disable();
        if (e->signaled) {
            if (!e->manual)
                e->signaled = false;
            preempt_enable();
            return 0;
        }
        uint64_t ms;
        if (!deadline_left(&dl, &ms)) {
            preempt_enable();
            return would_block_error(&dl);
        }
        e->waiters++;
        preempt_enable();
        sem_down_timeout(&e->hint, ms);
        preempt_disable();
        e->waiters--;
        preempt_enable();
    }
}

static int sem_wait_obj(struct object *o, uint64_t tmo)
{
    struct semobj *s = (struct semobj *)o;
    struct deadline dl;
    deadline_init(&dl, tmo);
    for (;;) {
        preempt_disable();
        if (s->count) {
            s->count--;
            preempt_enable();
            return 0;
        }
        uint64_t ms;
        if (!deadline_left(&dl, &ms)) {
            preempt_enable();
            return would_block_error(&dl);
        }
        s->waiters++;
        preempt_enable();
        sem_down_timeout(&s->hint, ms);
        preempt_disable();
        s->waiters--;
        preempt_enable();
    }
}

static const struct gmap event_gmap = {
    ACCESS_SYNCHRONIZE | ACCESS_READ_CONTROL,
    EVENT_MODIFY_STATE | ACCESS_READ_CONTROL,
    ACCESS_SYNCHRONIZE | ACCESS_READ_CONTROL,
    EVENT_MODIFY_STATE | ACCESS_SYNCHRONIZE | ACCESS_DELETE | ACCESS_SEC_RIGHTS };
static const struct gmap sem_gmap = {
    ACCESS_SYNCHRONIZE | ACCESS_READ_CONTROL,
    SEMAPHORE_MODIFY_STATE | ACCESS_READ_CONTROL,
    ACCESS_SYNCHRONIZE | ACCESS_READ_CONTROL,
    SEMAPHORE_MODIFY_STATE | ACCESS_SYNCHRONIZE | ACCESS_DELETE | ACCESS_SEC_RIGHTS };

const struct obj_type event_type = {
    "Event", OBJ_EVENT, EVENT_MODIFY_STATE | ACCESS_SYNCHRONIZE | ACCESS_DELETE | ACCESS_SEC_RIGHTS,
    NULL, event_wait, &event_gmap };
const struct obj_type semaphore_type = {
    "Semaphore", OBJ_SEMAPHORE, SEMAPHORE_MODIFY_STATE | ACCESS_SYNCHRONIZE | ACCESS_DELETE | ACCESS_SEC_RIGHTS,
    NULL, sem_wait_obj, &sem_gmap };

int obj_event_create(bool manual, bool signaled, struct object **out)
{
    struct event *e = obj_create(&event_type, sizeof(*e));
    if (!e)
        return -E_NOMEM;
    e->manual = manual;
    e->signaled = signaled;
    sem_init(&e->hint, 0);
    *out = &e->o;
    return 0;
}

int obj_semaphore_create(uint32_t initial, uint32_t max, struct object **out)
{
    if (!max || initial > max)
        return -E_INVAL;
    struct semobj *s = obj_create(&semaphore_type, sizeof(*s));
    if (!s)
        return -E_NOMEM;
    s->count = initial;
    s->max = max;
    sem_init(&s->hint, 0);
    *out = &s->o;
    return 0;
}

int obj_wait(struct object *o, uint64_t tmo)
{
    return o->type->wait ? o->type->wait(o, tmo) : -E_INVAL;
}

static long event_op(struct event *e, uint32_t op)
{
    long prev;
    preempt_disable();
    prev = e->signaled;
    switch (op) {
    case EVENT_SET:
        e->signaled = true;
        wake_n(&e->hint, e->manual ? e->waiters : (e->waiters ? 1 : 0));
        break;
    case EVENT_RESET:
        e->signaled = false;
        break;
    default:
        prev = -E_INVAL;
    }
    preempt_enable();
    return prev;
}

static long sem_release(struct semobj *s, uint32_t n)
{
    long prev;
    preempt_disable();
    if (!n || n > s->max || s->count > s->max - n) {
        prev = -E_INVAL;
    } else {
        prev = s->count;
        s->count += n;
        wake_n(&s->hint, s->waiters < n ? s->waiters : n);
    }
    preempt_enable();
    return prev;
}

int sys_obj_dispatch(uint64_t nr, uint64_t a, uint64_t b)
{
    struct task *t = task_current();
    if (!t || !t->ht)
        return -E_INVAL;
    struct object *o = NULL;
    struct handle_table *ht = t->ht;
    long rc;
    uint32_t h = 0;

    switch (nr) {
    case SYS_CLOSE:
        return ht_close(ht, (uint32_t)a);
    case SYS_DUP:
        rc = ht_dup(ht, ht, (uint32_t)a, (uint32_t)b, &h);
        return rc ? rc : (long)h;
    case SYS_EVENT_CREATE:
        rc = obj_event_create(a & 1, a & 2, &o);
        break;
    case SYS_SEM_CREATE:
        rc = obj_semaphore_create((uint32_t)a, (uint32_t)b, &o);
        break;
    case SYS_EVENT_OP:
        rc = ht_lookup(ht, (uint32_t)a, &event_type, EVENT_MODIFY_STATE, &o, NULL);
        if (rc)
            return rc;
        rc = event_op((struct event *)o, (uint32_t)b);
        obj_unref(o);
        return rc;
    case SYS_SEM_RELEASE:
        rc = ht_lookup(ht, (uint32_t)a, &semaphore_type, SEMAPHORE_MODIFY_STATE, &o, NULL);
        if (rc)
            return rc;
        rc = sem_release((struct semobj *)o, (uint32_t)b);
        obj_unref(o);
        return rc;
    case SYS_WAIT:
        rc = ht_lookup(ht, (uint32_t)a, NULL, ACCESS_SYNCHRONIZE, &o, NULL);
        if (rc)
            return rc;
        rc = obj_wait(o, b);
        obj_unref(o);
        return rc;
    default:
        return -E_NOSYS;
    }
    if (rc)
        return rc;
    rc = sec_obj_init(o, t->tok.user);
    if (rc) {
        obj_unref(o);
        return rc;
    }
    rc = ht_insert(ht, o, o->type->valid_access, &h);
    obj_unref(o);
    return rc ? rc : (long)h;
}

long obj_event_op(struct object *o, uint32_t op) { return event_op((struct event *)o, op); }
long obj_semaphore_release(struct object *o, uint32_t n) { return sem_release((struct semobj *)o, n); }
