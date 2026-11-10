#include "kernel.h"
#include "mm.h"
#include "irq.h"
#include "sched.h"
#include "timer.h"
#include "list.h"
#include "ipc.h"
#include "deadline.h"

#define SPACE_MAX       256
#define SPACE_INIT      8
#define QLIMIT_DEFAULT  16
#define QLIMIT_MAX      32
#define NMSGS_MAX       4096
#define UREFS_MAX       0xFFFF

#define NAME_IDX(n)     ((n) & 0xFFFu)
#define NAME_GEN(n)     ((n) >> 12)
#define GEN_MASK        0xFFFFFu

struct port {
    uint32_t         refs;
    bool             dead;
    bool             killing;
    struct list_node queue;
    uint32_t         qlen, qlimit;
    struct sem       recv_sem;
    struct sem       send_sem;
    uint32_t         recv_waiters, send_waiters;
    struct list_node kill_node;
};

struct kright {
    struct port *port;
    uint8_t      type;
};

struct kmsg {
    struct list_node node;
    uint32_t         id, ndesc, datalen;
    struct kright    reply;
    struct kright    desc[IPC_MAX_DESC];
    uint8_t          data[];
};

struct ipc_entry {
    struct port *port;
    uint32_t     gen;
    uint32_t     urefs;
    uint16_t     bits;
};

struct ipc_space {
    struct ipc_entry *tab;
    uint32_t          cap, used;
};

static unsigned n_ports, n_msgs;
static uint64_t n_sent, n_received;

unsigned ipc_live_ports(void) { return n_ports; }
unsigned ipc_live_msgs(void)  { return n_msgs; }
uint64_t ipc_msgs_sent(void)  { return n_sent; }

static inline void lock(void)
{
    if (g_irq_depth)
        kpanic("ipc: used from interrupt context");
    preempt_disable();
}
static inline void unlock(void) { preempt_enable(); }

static void kill_run(struct list_node *killq);

static struct port *port_new(void)
{
    struct port *p = kzalloc(sizeof(*p));
    if (!p)
        return NULL;
    p->refs = 1;
    p->qlimit = QLIMIT_DEFAULT;
    list_init(&p->queue);
    list_init(&p->kill_node);
    sem_init(&p->recv_sem, 0);
    sem_init(&p->send_sem, 0);
    n_ports++;
    return p;
}

static void port_ref(struct port *p) { p->refs++; }

static void port_unref(struct port *p)
{
    if (--p->refs)
        return;
    if (!p->dead || !list_empty(&p->queue) || p->recv_waiters || p->send_waiters ||
        p->killing)
        kpanic("ipc: freeing a live port (dead=%d qlen=%u waiters=%u/%u)", p->dead,
               p->qlen, p->recv_waiters, p->send_waiters);
    n_ports--;
    kfree(p);
}

static void right_drop(struct kright *r, struct list_node *killq)
{
    struct port *p = r->port;
    if (!p)
        return;
    r->port = NULL;
    if (r->type == PT_RECV && !p->dead && !p->killing) {
        p->killing = true;
        list_add_tail(killq, &p->kill_node);
        return;
    }
    port_unref(p);
}

static void kmsg_destroy(struct kmsg *m, struct list_node *killq)
{
    right_drop(&m->reply, killq);
    for (uint32_t i = 0; i < IPC_MAX_DESC; i++)
        right_drop(&m->desc[i], killq);
    n_msgs--;
    kfree(m);
}

static void wake_all(struct sem *s, uint32_t waiters)
{
    for (uint32_t i = 0; i < waiters; i++)
        sem_up(s);
}

static void kill_run(struct list_node *killq)
{
    while (!list_empty(killq)) {
        struct port *p = container_of(killq->next, struct port, kill_node);
        list_del(&p->kill_node);
        p->dead = true;
        p->killing = false;
        wake_all(&p->send_sem, p->send_waiters);
        wake_all(&p->recv_sem, p->recv_waiters);
        while (!list_empty(&p->queue)) {
            struct kmsg *m = container_of(p->queue.next, struct kmsg, node);
            list_del(&m->node);
            kmsg_destroy(m, killq);
        }
        p->qlen = 0;
        port_unref(p);
    }
}

struct ipc_space *ipc_space_create(void)
{
    struct ipc_space *s = kzalloc(sizeof(*s));
    if (!s)
        return NULL;
    s->tab = kzalloc(SPACE_INIT * sizeof(struct ipc_entry));
    if (!s->tab) {
        kfree(s);
        return NULL;
    }
    s->cap = SPACE_INIT;
    return s;
}

static bool space_grow(struct ipc_space *s)
{
    uint32_t ncap = s->cap * 2;
    if (ncap > SPACE_MAX)
        ncap = SPACE_MAX;
    if (ncap <= s->cap)
        return false;
    struct ipc_entry *t = krealloc(s->tab, ncap * sizeof(struct ipc_entry));
    if (!t)
        return false;
    memset(t + s->cap, 0, (ncap - s->cap) * sizeof(struct ipc_entry));
    s->tab = t;
    s->cap = ncap;
    return true;
}

static bool space_reserve(struct ipc_space *s, uint32_t n)
{
    while (s->cap - s->used < n)
        if (!space_grow(s))
            return false;
    return true;
}

static struct ipc_entry *entry_lookup(struct ipc_space *s, uint32_t name)
{
    uint32_t idx = NAME_IDX(name);
    if (!idx || idx > s->cap)
        return NULL;
    struct ipc_entry *e = &s->tab[idx - 1];
    if (!e->bits || e->gen != NAME_GEN(name))
        return NULL;
    return e;
}

static struct ipc_entry *entry_alloc(struct ipc_space *s, uint32_t *name)
{
    for (uint32_t i = 0; i < s->cap; i++) {
        struct ipc_entry *e = &s->tab[i];
        if (!e->bits) {
            s->used++;
            *name = (e->gen << 12) | (i + 1);
            return e;
        }
    }
    return NULL;
}

static void entry_free(struct ipc_space *s, struct ipc_entry *e)
{
    if (e->bits & PT_RECV)
        kpanic("ipc: freeing an entry that still holds a receive right");
    if (e->port)
        port_unref(e->port);
    e->port = NULL;
    e->bits = 0;
    e->urefs = 0;
    e->gen = (e->gen + 1) & GEN_MASK;
    s->used--;
}

static void entry_check_dead(struct ipc_entry *e)
{
    if (!e->port || !e->port->dead)
        return;
    if (e->bits & PT_RECV)
        kpanic("ipc: receive right on a dead port");
    struct port *p = e->port;
    e->port = NULL;
    e->bits = PT_DEAD;
    if (!e->urefs)
        e->urefs = 1;
    port_unref(p);
}

static struct ipc_entry *entry_find_port(struct ipc_space *s, struct port *p)
{
    for (uint32_t i = 0; i < s->cap; i++) {
        struct ipc_entry *e = &s->tab[i];
        if ((e->bits & (PT_RECV | PT_SEND)) && e->port == p)
            return e;
    }
    return NULL;
}

static uint32_t space_insert(struct ipc_space *s, struct kright *r)
{
    struct port *p = r->port;
    r->port = NULL;

    if (r->type == PT_RECV || r->type == PT_SEND) {
        struct ipc_entry *e = entry_find_port(s, p);
        if (e) {
            uint32_t name = (e->gen << 12) | (uint32_t)(e - s->tab + 1);
            if (r->type == PT_RECV) {
                e->bits |= PT_RECV;
            } else if (e->urefs < UREFS_MAX) {
                e->urefs++;
                e->bits |= PT_SEND;
            }
            port_unref(p);
            return name;
        }
    }
    uint32_t name;
    struct ipc_entry *e = entry_alloc(s, &name);
    if (!e)
        kpanic("ipc: space_insert without a reservation");
    e->port = p;
    e->bits = r->type;
    e->urefs = (r->type == PT_SEND) ? 1 : 0;
    return name;
}

struct rt {
    struct ipc_entry *e;
    uint32_t          disp;
    uint8_t           type;
};

static bool disp_is_move(uint32_t d)
{
    return d == MSG_MOVE_RECEIVE || d == MSG_MOVE_SEND || d == MSG_MOVE_SEND_ONCE;
}

static int right_check(struct ipc_space *s, uint32_t name, uint32_t disp, struct rt *out)
{
    uint32_t need, type;
    switch (disp) {
    case MSG_MOVE_RECEIVE:   need = PT_RECV;  type = PT_RECV;  break;
    case MSG_MOVE_SEND:
    case MSG_COPY_SEND:      need = PT_SEND;  type = PT_SEND;  break;
    case MSG_MOVE_SEND_ONCE: need = PT_SONCE; type = PT_SONCE; break;
    case MSG_MAKE_SEND:      need = PT_RECV;  type = PT_SEND;  break;
    case MSG_MAKE_SEND_ONCE: need = PT_RECV;  type = PT_SONCE; break;
    default:                 return -E_INVAL;
    }
    struct ipc_entry *e = entry_lookup(s, name);
    if (!e)
        return -E_BADF;
    entry_check_dead(e);
    if (e->bits & PT_DEAD)
        return -E_PIPE;
    if (!(e->bits & need))
        return -E_PERM;
    out->e = e;
    out->disp = disp;
    out->type = (uint8_t)type;
    return 0;
}

static void right_commit(struct ipc_space *s, const struct rt *rt, struct kright *out)
{
    struct ipc_entry *e = rt->e;
    out->port = e->port;
    out->type = rt->type;
    port_ref(e->port);

    switch (rt->disp) {
    case MSG_MOVE_RECEIVE:
        e->bits &= (uint16_t)~PT_RECV;
        if (!e->bits)
            entry_free(s, e);
        break;
    case MSG_MOVE_SEND:
        if (--e->urefs == 0)
            e->bits &= (uint16_t)~PT_SEND;
        if (!e->bits)
            entry_free(s, e);
        break;
    case MSG_MOVE_SEND_ONCE:
        e->bits &= (uint16_t)~PT_SONCE;
        entry_free(s, e);
        break;
    default:
        break;
    }
}

void ipc_space_destroy(struct ipc_space *s)
{
    if (!s)
        return;
    struct list_node killq;
    list_init(&killq);

    lock();
    for (uint32_t i = 0; i < s->cap; i++) {
        struct ipc_entry *e = &s->tab[i];
        if (!e->bits)
            continue;
        if (e->port) {
            struct kright r = { e->port, (e->bits & PT_RECV) ? PT_RECV : PT_SEND };
            e->port = NULL;
            e->bits = 0;
            right_drop(&r, &killq);
        }
        e->bits = 0;
    }
    kill_run(&killq);
    unlock();

    kfree(s->tab);
    kfree(s);
}

int ipc_port_alloc(struct ipc_space *s, uint32_t *name)
{
    lock();
    int rc = 0;
    if (!space_reserve(s, 1)) {
        rc = -E_NOSPC;
    } else {
        struct port *p = port_new();
        if (!p) {
            rc = -E_NOMEM;
        } else {
            struct ipc_entry *e = entry_alloc(s, name);
            e->port = p;
            e->bits = PT_RECV;
        }
    }
    unlock();
    return rc;
}

int ipc_port_destroy(struct ipc_space *s, uint32_t name)
{
    struct list_node killq;
    list_init(&killq);

    lock();
    struct ipc_entry *e = entry_lookup(s, name);
    int rc = 0;
    if (!e) {
        rc = -E_BADF;
    } else if (e->bits & PT_RECV) {
        struct kright r = { e->port, PT_RECV };
        port_ref(e->port);
        e->bits &= (uint16_t)~PT_RECV;
        entry_free(s, e);
        right_drop(&r, &killq);
        kill_run(&killq);
    } else {
        entry_free(s, e);
    }
    unlock();
    return rc;
}

int ipc_port_dealloc(struct ipc_space *s, uint32_t name)
{
    lock();
    struct ipc_entry *e = entry_lookup(s, name);
    int rc = 0;
    if (!e) {
        rc = -E_BADF;
    } else {
        entry_check_dead(e);
        if (e->bits & PT_DEAD) {
            if (--e->urefs == 0) {
                e->bits = 0;
                entry_free(s, e);
            }
        } else if (e->bits & PT_SONCE) {
            e->bits = 0;
            entry_free(s, e);
        } else if (e->bits & PT_SEND) {
            if (--e->urefs == 0)
                e->bits &= (uint16_t)~PT_SEND;
            if (!e->bits)
                entry_free(s, e);
        } else {
            rc = -E_PERM;
        }
    }
    unlock();
    return rc;
}

int ipc_port_insert(struct ipc_space *s, uint32_t name, uint32_t disp)
{
    lock();
    struct ipc_entry *e = entry_lookup(s, name);
    int rc = 0;
    if (!e) {
        rc = -E_BADF;
    } else {
        entry_check_dead(e);
        if (e->bits & PT_DEAD)
            rc = -E_PIPE;
        else if (disp == MSG_MAKE_SEND && (e->bits & PT_RECV))
            ;
        else if (disp == MSG_COPY_SEND && (e->bits & PT_SEND))
            ;
        else
            rc = (disp == MSG_MAKE_SEND || disp == MSG_COPY_SEND) ? -E_PERM : -E_INVAL;
        if (!rc) {
            if (e->urefs >= UREFS_MAX) {
                rc = -E_NOSPC;
            } else {
                e->urefs++;
                e->bits |= PT_SEND;
            }
        }
    }
    unlock();
    return rc;
}

int ipc_port_info(struct ipc_space *s, uint32_t name, struct port_info *out)
{
    lock();
    struct ipc_entry *e = entry_lookup(s, name);
    int rc = 0;
    if (!e) {
        rc = -E_BADF;
    } else {
        entry_check_dead(e);
        out->type = e->bits;
        out->urefs = e->urefs;
        out->qlen = e->port ? e->port->qlen : 0;
        out->qlimit = e->port ? e->port->qlimit : 0;
    }
    unlock();
    return rc;
}

int ipc_port_set_qlimit(struct ipc_space *s, uint32_t name, uint32_t limit)
{
    if (limit < 1 || limit > QLIMIT_MAX)
        return -E_INVAL;
    lock();
    struct ipc_entry *e = entry_lookup(s, name);
    int rc = 0;
    if (!e)
        rc = -E_BADF;
    else if (!(e->bits & PT_RECV))
        rc = -E_PERM;
    else {
        e->port->qlimit = limit;
        wake_all(&e->port->send_sem, e->port->send_waiters);
    }
    unlock();
    return rc;
}

int ipc_grant(struct ipc_space *dst, struct ipc_space *src, uint32_t src_name,
              uint32_t disp, uint32_t *dst_name)
{
    lock();
    int rc;
    struct rt rt;
    if (!space_reserve(dst, 1)) {
        rc = -E_NOSPC;
    } else if ((rc = right_check(src, src_name, disp, &rt)) == 0) {
        struct kright r;
        right_commit(src, &rt, &r);
        *dst_name = space_insert(dst, &r);
    }
    unlock();
    return rc;
}

static bool dest_disp_ok(uint32_t d)
{
    return d == MSG_COPY_SEND || d == MSG_MOVE_SEND || d == MSG_MAKE_SEND ||
           d == MSG_MAKE_SEND_ONCE || d == MSG_MOVE_SEND_ONCE;
}

int ipc_send(struct ipc_space *s, const void *msg, size_t size, uint64_t timeout_ms)
{
    struct msg_hdr h;
    if (size < sizeof(h))
        return -E_INVAL;
    if (size > IPC_MSG_MAX)
        return -E_MSGSIZE;
    memcpy(&h, msg, sizeof(h));
    if (h.size != size || h.ndesc > IPC_MAX_DESC || (h.bits >> 16))
        return -E_INVAL;
    size_t off = sizeof(h) + (size_t)h.ndesc * sizeof(struct msg_desc);
    if (off > size)
        return -E_INVAL;
    uint32_t rdisp = h.bits & 0xFF, ldisp = (h.bits >> 8) & 0xFF;
    if (!dest_disp_ok(rdisp))
        return -E_INVAL;
    if (!ldisp && h.local)
        return -E_INVAL;

    struct msg_desc d[IPC_MAX_DESC];
    memcpy(d, (const uint8_t *)msg + sizeof(h), (size_t)h.ndesc * sizeof(struct msg_desc));

    struct { uint32_t name, disp; } use[2 + IPC_MAX_DESC];
    unsigned nuse = 0;
    use[nuse].name = h.remote; use[nuse++].disp = rdisp;
    if (ldisp) { use[nuse].name = h.local; use[nuse++].disp = ldisp; }
    for (uint32_t i = 0; i < h.ndesc; i++) {
        use[nuse].name = d[i].name; use[nuse++].disp = d[i].disp;
    }
    for (unsigned i = 0; i < nuse; i++)
        for (unsigned j = i + 1; j < nuse; j++)
            if (use[i].name == use[j].name && (disp_is_move(use[i].disp) || disp_is_move(use[j].disp)))
                return -E_INVAL;

    size_t dl = size - off;
    struct kmsg *m = kzalloc(sizeof(*m) + dl);
    if (!m)
        return -E_NOMEM;
    lock();
    if (n_msgs >= NMSGS_MAX) {
        unlock();
        kfree(m);
        return -E_NOMEM;
    }
    n_msgs++;
    unlock();
    list_init(&m->node);
    m->id = h.id;
    m->ndesc = h.ndesc;
    m->datalen = (uint32_t)dl;
    if (dl)
        memcpy(m->data, (const uint8_t *)msg + off, dl);

    struct deadline dl_t;
    deadline_init(&dl_t, timeout_ms);
    bool sonce = (rdisp == MSG_MOVE_SEND_ONCE || rdisp == MSG_MAKE_SEND_ONCE);
    int rc = 0;
    struct port *p = NULL;
    struct rt rdest, rrep, rdesc[IPC_MAX_DESC];

    for (;;) {
        lock();
        rc = right_check(s, h.remote, rdisp, &rdest);
        if (rc)
            break;
        p = rdest.e->port;
        if (sonce || p->qlen < p->qlimit)
            break;

        uint64_t ms;
        if (!deadline_left(&dl_t, &ms)) {
            rc = would_block_error(&dl_t);
            break;
        }
        port_ref(p);
        p->send_waiters++;
        unlock();
        int r = sem_down_timeout(&p->send_sem, ms);
        lock();
        p->send_waiters--;
        port_unref(p);
        unlock();
        if (r == SEM_TIMEOUT) {
            rc = would_block_error(&dl_t);
            goto fail_unlocked;
        }
    }
    if (rc) {
        unlock();
        goto fail_unlocked;
    }

    if (ldisp) {
        rc = right_check(s, h.local, ldisp, &rrep);
        if (rc)
            goto fail_locked;
        if (rrep.type == PT_RECV && rrep.e->port == p) {
            rc = -E_INVAL;
            goto fail_locked;
        }
    }
    for (uint32_t i = 0; i < h.ndesc; i++) {
        rc = right_check(s, d[i].name, d[i].disp, &rdesc[i]);
        if (rc)
            goto fail_locked;
        if (rdesc[i].type == PT_RECV && rdesc[i].e->port == p) {
            rc = -E_INVAL;
            goto fail_locked;
        }
    }

    port_ref(p);
    if (ldisp)
        right_commit(s, &rrep, &m->reply);
    for (uint32_t i = 0; i < h.ndesc; i++)
        right_commit(s, &rdesc[i], &m->desc[i]);

    if (rdisp == MSG_MOVE_SEND) {
        if (--rdest.e->urefs == 0)
            rdest.e->bits &= (uint16_t)~PT_SEND;
        if (!rdest.e->bits)
            entry_free(s, rdest.e);
    } else if (rdisp == MSG_MOVE_SEND_ONCE) {
        rdest.e->bits &= (uint16_t)~PT_SONCE;
        entry_free(s, rdest.e);
    }

    list_add_tail(&p->queue, &m->node);
    p->qlen++;
    n_sent++;
    if (p->recv_waiters)
        sem_up(&p->recv_sem);
    port_unref(p);
    unlock();
    return 0;

fail_locked:
    unlock();
fail_unlocked:
    lock();
    n_msgs--;
    unlock();
    kfree(m);
    return rc;
}

long ipc_recv(struct ipc_space *s, uint32_t name, void *buf, size_t cap, uint64_t timeout_ms)
{
    if (cap < sizeof(struct msg_hdr))
        return -E_MSGSIZE;

    struct deadline dl;
    deadline_init(&dl, timeout_ms);
    struct port *p = NULL;
    long rc;

    lock();
    struct ipc_entry *e = entry_lookup(s, name);
    if (!e) {
        unlock();
        return -E_BADF;
    }
    entry_check_dead(e);
    if (e->bits & PT_DEAD) {
        unlock();
        return -E_PIPE;
    }
    if (!(e->bits & PT_RECV)) {
        unlock();
        return -E_PERM;
    }
    p = e->port;
    port_ref(p);

    for (;;) {
        if (p->dead) {
            rc = -E_PIPE;
            break;
        }
        e = entry_lookup(s, name);
        if (!e || e->port != p || !(e->bits & PT_RECV)) {
            rc = -E_BADF;
            break;
        }

        if (p->qlen) {
            struct kmsg *m = container_of(p->queue.next, struct kmsg, node);
            size_t total = sizeof(struct msg_hdr) + (size_t)m->ndesc * sizeof(struct msg_desc) +
                           m->datalen;
            if (total > cap) {
                rc = -E_MSGSIZE;
                break;
            }
            uint32_t need = m->ndesc + (m->reply.port ? 1 : 0);
            if (need && !space_reserve(s, need)) {
                rc = -E_NOSPC;
                break;
            }
            list_del(&m->node);
            p->qlen--;
            if (p->send_waiters)
                sem_up(&p->send_sem);

            uint8_t *out = buf;
            struct msg_desc od[IPC_MAX_DESC];
            struct msg_hdr oh = { 0, (uint32_t)total, 0, name, m->id, m->ndesc };
            if (m->reply.port) {
                uint8_t t = m->reply.type;
                oh.remote = space_insert(s, &m->reply);
                oh.bits = MSG_BITS(0, t == PT_RECV ? MSG_MOVE_RECEIVE :
                                      t == PT_SEND ? MSG_MOVE_SEND : MSG_MOVE_SEND_ONCE);
            }
            for (uint32_t i = 0; i < m->ndesc; i++) {
                uint8_t t = m->desc[i].type;
                od[i].disp = t == PT_RECV ? MSG_MOVE_RECEIVE :
                             t == PT_SEND ? MSG_MOVE_SEND : MSG_MOVE_SEND_ONCE;
                od[i].name = space_insert(s, &m->desc[i]);
            }
            memcpy(out, &oh, sizeof(oh));
            memcpy(out + sizeof(oh), od, (size_t)m->ndesc * sizeof(od[0]));
            memcpy(out + sizeof(oh) + (size_t)m->ndesc * sizeof(od[0]), m->data, m->datalen);

            n_msgs--;
            n_received++;
            kfree(m);
            rc = (long)total;
            break;
        }

        uint64_t ms;
        if (!deadline_left(&dl, &ms)) {
            rc = would_block_error(&dl);
            break;
        }
        p->recv_waiters++;
        unlock();
        int r = sem_down_timeout(&p->recv_sem, ms);
        lock();
        p->recv_waiters--;
        if (r == SEM_TIMEOUT && !p->qlen && !p->dead) {
            rc = would_block_error(&dl);
            break;
        }
    }
    port_unref(p);
    unlock();
    return rc;
}
