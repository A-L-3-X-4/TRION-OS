#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "abi.h"
#include "ipc.h"
#include "timer.h"

static unsigned n_checks, n_fail;

static void check_(bool ok, const char *expr, int line)
{
    n_checks++;
    if (!ok) {
        n_fail++;
        klog(LOG_FAIL, "ipctest.c:%d: %s", line, expr);
    }
}
#define CHECK(c) check_((c), #c, __LINE__)

#define IMG_SIZE ((size_t)(user_tests_end - user_tests_start))
#define FAULT(vec) (-(TASK_FAULT_BASE + (vec)))

struct mbuf { uint8_t b[IPC_MSG_MAX]; };

static size_t build(struct mbuf *m, uint32_t rname, uint32_t rdisp, uint32_t lname, uint32_t ldisp,
                    uint32_t id, const struct msg_desc *d, unsigned nd, uint64_t val)
{
    struct msg_hdr h = { MSG_BITS(rdisp, ldisp), 0, rname, lname, id, nd };
    size_t off = sizeof(h) + nd * sizeof(*d);
    h.size = (uint32_t)(off + sizeof(val));
    memcpy(m->b, &h, sizeof(h));
    if (nd)
        memcpy(m->b + sizeof(h), d, nd * sizeof(*d));
    memcpy(m->b + off, &val, sizeof(val));
    return h.size;
}

static int send_val(struct ipc_space *s, uint32_t to, uint32_t rdisp, uint32_t id, uint64_t val,
                    uint64_t tmo)
{
    struct mbuf m;
    size_t n = build(&m, to, rdisp, 0, 0, id, NULL, 0, val);
    return ipc_send(s, m.b, n, tmo);
}

static int send_full(struct ipc_space *s, uint32_t to, uint32_t rdisp, uint32_t lname,
                     uint32_t ldisp, uint32_t id, const struct msg_desc *d, unsigned nd)
{
    struct mbuf m;
    size_t n = build(&m, to, rdisp, lname, ldisp, id, d, nd, 0);
    return ipc_send(s, m.b, n, 0);
}

struct got {
    struct msg_hdr h;
    struct msg_desc d[IPC_MAX_DESC];
    uint64_t val;
    long n;
};

static struct got recv_(struct ipc_space *s, uint32_t name, uint64_t tmo)
{
    struct mbuf m;
    struct got g;
    memset(&g, 0, sizeof(g));
    g.n = ipc_recv(s, name, m.b, sizeof(m.b), tmo);
    if (g.n >= (long)sizeof(g.h)) {
        memcpy(&g.h, m.b, sizeof(g.h));
        memcpy(g.d, m.b + sizeof(g.h), g.h.ndesc * sizeof(g.d[0]));
        memcpy(&g.val, m.b + g.n - sizeof(g.val), sizeof(g.val));
    }
    return g;
}

static uint32_t new_port(struct ipc_space *s)
{
    uint32_t n = 0;
    if (ipc_port_alloc(s, &n))
        return 0;
    return n;
}

static uint32_t grant(struct ipc_space *dst, struct ipc_space *src, uint32_t name, uint32_t disp)
{
    uint32_t n = 0;
    if (ipc_grant(dst, src, name, disp, &n))
        return 0;
    return n;
}

static uint32_t ptype(struct ipc_space *s, uint32_t name)
{
    struct port_info pi;
    return ipc_port_info(s, name, &pi) ? 0xFFFF : pi.type;
}

static uint32_t purefs(struct ipc_space *s, uint32_t name)
{
    struct port_info pi;
    return ipc_port_info(s, name, &pi) ? 0xFFFF : pi.urefs;
}

struct sendjob {
    struct ipc_space *s;
    struct mbuf       m;
    size_t            n;
    uint64_t          tmo;
    volatile int      rc;
    volatile bool     done;
};

static int sender_main(void *arg)
{
    struct sendjob *j = arg;
    j->rc = ipc_send(j->s, j->m.b, j->n, j->tmo);
    j->done = true;
    return 0;
}

static struct thread *start_send(struct sendjob *j, struct ipc_space *s, uint32_t to,
                                 uint32_t rdisp, uint32_t id, uint64_t tmo)
{
    j->s = s;
    j->n = build(&j->m, to, rdisp, 0, 0, id, NULL, 0, 0xB10C);
    j->tmo = tmo;
    j->rc = 12345;
    j->done = false;
    return thread_create("sender", sender_main, j, 0);
}

struct recvjob {
    struct ipc_space *s;
    uint32_t          name;
    volatile long     n;
    volatile bool     done;
};

static int receiver_main(void *arg)
{
    struct recvjob *j = arg;
    j->n = recv_(j->s, j->name, SEM_FOREVER).n;
    j->done = true;
    return 0;
}

static void test_names(void)
{
    struct ipc_space *a = ipc_space_create();
    CHECK(a != NULL);
    struct port_info pi;

    uint32_t p = new_port(a);
    CHECK(p != 0 && p != IPC_NAME_DEAD);
    CHECK(ipc_port_info(a, p, &pi) == 0 && pi.type == PT_RECV && pi.urefs == 0 &&
          pi.qlen == 0 && pi.qlimit == 16);
    CHECK(ipc_port_info(a, 0, &pi) == -E_BADF);
    CHECK(ipc_port_info(a, IPC_NAME_DEAD, &pi) == -E_BADF);
    CHECK(ipc_port_info(a, p + 1, &pi) == -E_BADF);

    CHECK(ipc_port_destroy(a, p) == 0);
    CHECK(ipc_port_info(a, p, &pi) == -E_BADF);
    CHECK(ipc_port_destroy(a, p) == -E_BADF);
    uint32_t q = new_port(a);
    CHECK(q != 0 && q != p);
    CHECK(ipc_port_info(a, p, &pi) == -E_BADF);

    unsigned made = 1;
    uint32_t extra = 0;
    int rc = 0;
    while ((rc = ipc_port_alloc(a, &extra)) == 0)
        made++;
    CHECK(rc == -E_NOSPC && made == 256);
    CHECK(ipc_live_ports() >= 256);
    ipc_space_destroy(a);
    klog(LOG_INFO, "ipctest: names, generations, namespace limit ok");
}

static void test_basic(void)
{
    struct ipc_space *a = ipc_space_create();
    uint32_t p = new_port(a);
    CHECK(ipc_port_insert(a, p, MSG_MAKE_SEND) == 0);
    CHECK(purefs(a, p) == 1 && ptype(a, p) == (PT_RECV | PT_SEND));
    CHECK(ipc_port_insert(a, p, MSG_COPY_SEND) == 0);
    CHECK(purefs(a, p) == 2);
    CHECK(ipc_port_insert(a, p, MSG_MOVE_SEND) == -E_INVAL);
    CHECK(ipc_port_dealloc(a, p) == 0 && purefs(a, p) == 1);

    for (uint32_t i = 1; i <= 3; i++)
        CHECK(send_val(a, p, MSG_COPY_SEND, i, 1000 + i, 0) == 0);
    struct port_info pi;
    CHECK(ipc_port_info(a, p, &pi) == 0 && pi.qlen == 3);
    for (uint32_t i = 1; i <= 3; i++) {
        struct got g = recv_(a, p, 0);
        CHECK(g.n == 32 && g.h.id == i && g.val == 1000 + i && g.h.local == p &&
              g.h.remote == 0 && g.h.bits == 0 && g.h.size == 32 && g.h.ndesc == 0);
    }
    CHECK(recv_(a, p, 0).n == -E_AGAIN);

    uint64_t t0 = clock_ns();
    CHECK(recv_(a, p, 20).n == -E_TIMEDOUT);
    uint64_t dt = clock_ns() - t0;
    CHECK(dt >= 19000000ULL && dt < 40000000ULL);

    CHECK(send_val(a, p, MSG_COPY_SEND, 9, 99, 0) == 0);
    uint8_t small[31];
    CHECK(ipc_recv(a, p, small, sizeof(small), 0) == -E_MSGSIZE);
    CHECK(ipc_port_info(a, p, &pi) == 0 && pi.qlen == 1);
    CHECK(recv_(a, p, 0).val == 99);

    struct mbuf m;
    size_t n = build(&m, p, MSG_COPY_SEND, 0, 0, 1, NULL, 0, 0);
    CHECK(ipc_send(a, m.b, sizeof(struct msg_hdr) - 1, 0) == -E_INVAL);
    CHECK(ipc_send(a, m.b, IPC_MSG_MAX + 1, 0) == -E_MSGSIZE);
    CHECK(ipc_send(a, m.b, n - 1, 0) == -E_INVAL);
    CHECK(send_val(a, 0, MSG_COPY_SEND, 1, 0, 0) == -E_BADF);
    CHECK(send_val(a, p, 99, 1, 0, 0) == -E_INVAL);
    CHECK(send_val(a, p, MSG_MOVE_RECEIVE, 1, 0, 0) == -E_INVAL);
    CHECK(send_full(a, p, MSG_COPY_SEND, 777, 0, 1, NULL, 0) == -E_INVAL);
    CHECK(send_full(a, p, MSG_COPY_SEND, 0, MSG_COPY_SEND, 1, NULL, 0) == -E_BADF);
    {
        struct msg_desc many[IPC_MAX_DESC + 1];
        memset(many, 0, sizeof(many));
        CHECK(send_full(a, p, MSG_COPY_SEND, 0, 0, 1, many, IPC_MAX_DESC + 1) == -E_INVAL);
    }
    CHECK(recv_(a, 0, 0).n == -E_BADF);
    CHECK(ipc_recv(a, p, m.b, 8, 0) == -E_MSGSIZE);

    CHECK(send_val(a, p, MSG_MOVE_SEND, 5, 5, 0) == 0);
    CHECK(ptype(a, p) == PT_RECV);
    CHECK(send_val(a, p, MSG_COPY_SEND, 5, 5, 0) == -E_PERM);
    CHECK(recv_(a, p, 0).val == 5);
    CHECK(ipc_port_dealloc(a, p) == -E_PERM);

    ipc_space_destroy(a);
    klog(LOG_INFO, "ipctest: send/recv, FIFO, timeouts, too-small buffer, error paths ok");
}

static void test_queue(void)
{
    struct ipc_space *a = ipc_space_create();
    uint32_t p = new_port(a);
    ipc_port_insert(a, p, MSG_MAKE_SEND);

    CHECK(ipc_port_set_qlimit(a, p, 0) == -E_INVAL);
    CHECK(ipc_port_set_qlimit(a, p, 33) == -E_INVAL);
    CHECK(ipc_port_set_qlimit(a, 0x999, 2) == -E_BADF);
    CHECK(ipc_port_set_qlimit(a, p, 2) == 0);

    CHECK(send_val(a, p, MSG_COPY_SEND, 1, 1, 0) == 0);
    CHECK(send_val(a, p, MSG_COPY_SEND, 2, 2, 0) == 0);
    CHECK(send_val(a, p, MSG_COPY_SEND, 3, 3, 0) == -E_AGAIN);
    uint64_t t0 = clock_ns();
    CHECK(send_val(a, p, MSG_COPY_SEND, 3, 3, 15) == -E_TIMEDOUT);
    CHECK(clock_ns() - t0 >= 14000000ULL);

    uint32_t so = grant(a, a, p, MSG_MAKE_SEND_ONCE);
    CHECK(so != 0 && ptype(a, so) == PT_SONCE);
    CHECK(send_val(a, so, MSG_MOVE_SEND_ONCE, 4, 4, 0) == 0);
    CHECK(ptype(a, so) == 0xFFFF);
    struct port_info pi;
    CHECK(ipc_port_info(a, p, &pi) == 0 && pi.qlen == 3 && pi.qlimit == 2);

    struct sendjob j;
    struct thread *t = start_send(&j, a, p, MSG_COPY_SEND, 5, SEM_FOREVER);
    thread_sleep_ms(20);
    CHECK(!j.done);
    CHECK(recv_(a, p, 0).h.id == 1);
    thread_sleep_ms(5);
    CHECK(!j.done);
    CHECK(recv_(a, p, 0).h.id == 2);
    thread_join(t);
    CHECK(j.done && j.rc == 0);
    CHECK(recv_(a, p, 0).h.id == 4);
    CHECK(recv_(a, p, 0).h.id == 5);
    CHECK(recv_(a, p, 0).n == -E_AGAIN);

    ipc_port_set_qlimit(a, p, 1);
    CHECK(send_val(a, p, MSG_COPY_SEND, 6, 6, 0) == 0);
    t = start_send(&j, a, p, MSG_COPY_SEND, 7, SEM_FOREVER);
    thread_sleep_ms(10);
    CHECK(!j.done);
    CHECK(ipc_port_set_qlimit(a, p, 4) == 0);
    thread_join(t);
    CHECK(j.done && j.rc == 0);
    CHECK(ipc_port_info(a, p, &pi) == 0 && pi.qlen == 2);

    recv_(a, p, 0); recv_(a, p, 0);
    struct recvjob rj = { a, p, 0, false };
    t = thread_create("receiver", receiver_main, &rj, 0);
    thread_sleep_ms(15);
    CHECK(!rj.done);
    CHECK(send_val(a, p, MSG_COPY_SEND, 8, 8, 0) == 0);
    thread_join(t);
    CHECK(rj.done && rj.n == 32);

    ipc_space_destroy(a);
    klog(LOG_INFO, "ipctest: queue limit, blocked sender/receiver wake-ups, send-once bypass ok");
}

static void test_transfer(void)
{
    struct ipc_space *a = ipc_space_create(), *b = ipc_space_create();
    uint32_t pa = new_port(a);
    uint32_t pb = new_port(b);
    uint32_t b_to_a = grant(b, a, pa, MSG_MAKE_SEND);
    CHECK(pa && pb && b_to_a && ptype(b, b_to_a) == PT_SEND);

    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, pb, MSG_MAKE_SEND_ONCE, 5, NULL, 0) == 0);
    struct got rq = recv_(a, pa, 0);
    CHECK(rq.n == 32 && rq.h.id == 5 && rq.h.remote != 0 && rq.h.local == pa);
    CHECK(rq.h.bits == MSG_BITS(0, MSG_MOVE_SEND_ONCE));
    CHECK(ptype(a, rq.h.remote) == PT_SONCE);
    CHECK(send_val(a, rq.h.remote, MSG_MOVE_SEND_ONCE, 6, 66, 0) == 0);
    CHECK(ptype(a, rq.h.remote) == 0xFFFF);
    CHECK(send_val(a, rq.h.remote, MSG_MOVE_SEND_ONCE, 6, 66, 0) == -E_BADF);
    struct got rp = recv_(b, pb, 0);
    CHECK(rp.n == 32 && rp.h.id == 6 && rp.val == 66);

    uint32_t sb = new_port(b);
    struct msg_desc d = { sb, MSG_MAKE_SEND };
    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, 0, 0, 10, &d, 1) == 0);
    struct got g = recv_(a, pa, 0);
    CHECK(g.n == 24 + 8 + 8 && g.h.ndesc == 1 && g.d[0].disp == MSG_MOVE_SEND && g.d[0].name != 0);
    uint32_t a_to_sb = g.d[0].name;
    CHECK(ptype(a, a_to_sb) == PT_SEND && purefs(a, a_to_sb) == 1);
    CHECK(send_val(a, a_to_sb, MSG_COPY_SEND, 11, 111, 0) == 0);
    CHECK(recv_(b, sb, 0).val == 111);

    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, 0, 0, 12, &d, 1) == 0);
    g = recv_(a, pa, 0);
    CHECK(g.d[0].name == a_to_sb && purefs(a, a_to_sb) == 2);
    CHECK(ipc_port_dealloc(a, a_to_sb) == 0 && purefs(a, a_to_sb) == 1);
    CHECK(ipc_port_dealloc(a, a_to_sb) == 0);
    CHECK(ptype(a, a_to_sb) == 0xFFFF);

    CHECK(ipc_port_insert(b, sb, MSG_MAKE_SEND) == 0);
    CHECK(send_val(b, sb, MSG_COPY_SEND, 20, 200, 0) == 0);
    struct msg_desc mv = { sb, MSG_MOVE_RECEIVE };
    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, 0, 0, 21, &mv, 1) == 0);
    CHECK(ptype(b, sb) == PT_SEND);
    g = recv_(a, pa, 0);
    CHECK(g.h.ndesc == 1 && g.d[0].disp == MSG_MOVE_RECEIVE);
    uint32_t a_sb = g.d[0].name;
    CHECK(ptype(a, a_sb) == PT_RECV);
    CHECK(recv_(a, a_sb, 0).val == 200);
    CHECK(send_val(b, sb, MSG_COPY_SEND, 22, 222, 0) == 0);
    CHECK(recv_(a, a_sb, 0).val == 222);

    uint32_t x = new_port(b);
    ipc_port_insert(b, x, MSG_MAKE_SEND);
    ipc_port_insert(b, x, MSG_COPY_SEND);
    struct msg_desc two[2] = { { x, MSG_MOVE_SEND }, { 0x777, MSG_COPY_SEND } };
    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, 0, 0, 30, two, 2) == -E_BADF);
    CHECK(purefs(b, x) == 2);
    struct port_info pi;
    CHECK(ipc_port_info(a, pa, &pi) == 0 && pi.qlen == 0);

    struct msg_desc dup[2] = { { x, MSG_MOVE_SEND }, { x, MSG_COPY_SEND } };
    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, 0, 0, 31, dup, 2) == -E_INVAL);
    struct msg_desc cc[2] = { { x, MSG_COPY_SEND }, { x, MSG_MAKE_SEND } };
    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, 0, 0, 32, cc, 2) == 0);
    CHECK(purefs(b, x) == 2);
    g = recv_(a, pa, 0);
    CHECK(g.h.ndesc == 2 && g.d[0].name == g.d[1].name && purefs(a, g.d[0].name) == 2);

    uint32_t ax = g.d[0].name;
    struct msg_desc mvs = { x, MSG_MOVE_SEND };
    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, 0, 0, 36, &mvs, 1) == 0);
    CHECK(purefs(b, x) == 1);
    g = recv_(a, pa, 0);
    CHECK(g.d[0].name == ax && purefs(a, ax) == 3);
    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, 0, 0, 37, &mvs, 1) == 0);
    CHECK(ptype(b, x) == PT_RECV);
    g = recv_(a, pa, 0);
    CHECK(purefs(a, ax) == 4);

    uint32_t bare = new_port(b);
    struct msg_desc wrong = { bare, MSG_MOVE_SEND };
    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, 0, 0, 33, &wrong, 1) == -E_PERM);
    struct msg_desc right2 = { x, MSG_MAKE_SEND };
    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, 0, 0, 34, &right2, 1) == 0);
    recv_(a, pa, 0);
    struct msg_desc badd = { x, 77 };
    CHECK(send_full(b, b_to_a, MSG_COPY_SEND, 0, 0, 35, &badd, 1) == -E_INVAL);

    uint32_t so = grant(a, a, pa, MSG_MAKE_SEND_ONCE);
    struct msg_desc self = { pa, MSG_MOVE_RECEIVE };
    CHECK(send_full(a, so, MSG_MOVE_SEND_ONCE, 0, 0, 40, &self, 1) == -E_INVAL);
    CHECK(ptype(a, so) == PT_SONCE && ptype(a, pa) == PT_RECV);

    ipc_space_destroy(a);
    ipc_space_destroy(b);
    klog(LOG_INFO, "ipctest: reply rights, descriptors, coalescing, receive-right move, atomic validation ok");
}

static void test_death(void)
{
    struct ipc_space *a = ipc_space_create(), *b = ipc_space_create();
    uint32_t pa = new_port(a);
    uint32_t bp = grant(b, a, pa, MSG_MAKE_SEND);
    uint32_t bp2 = grant(b, a, pa, MSG_MAKE_SEND);
    CHECK(bp == bp2 && purefs(b, bp) == 2);
    uint32_t bso = grant(b, a, pa, MSG_MAKE_SEND_ONCE);
    CHECK(bso != 0 && bso != bp);

    uint32_t other = new_port(a);
    struct msg_desc d = { other, MSG_MAKE_SEND };
    struct msg_desc bogus = { 0x555, MSG_MAKE_SEND };
    CHECK(send_full(b, bp, MSG_COPY_SEND, 0, 0, 1, &bogus, 1) == -E_BADF);
    uint32_t a_to_pa = grant(a, a, pa, MSG_MAKE_SEND);
    CHECK(send_full(a, a_to_pa, MSG_COPY_SEND, 0, 0, 1, &d, 1) == 0);
    unsigned msgs = ipc_live_msgs();
    CHECK(msgs >= 1);

    CHECK(ipc_port_destroy(a, pa) == 0);
    CHECK(ipc_live_msgs() == msgs - 1);
    CHECK(send_val(b, bp, MSG_COPY_SEND, 1, 1, 0) == -E_PIPE);
    CHECK(ptype(b, bp) == PT_DEAD);
    CHECK(send_val(b, bp, MSG_COPY_SEND, 1, 1, 0) == -E_PIPE);
    CHECK(ptype(b, bso) == PT_DEAD);
    CHECK(send_val(b, bso, MSG_MOVE_SEND_ONCE, 1, 1, 0) == -E_PIPE);
    CHECK(purefs(b, bp) == 2);
    CHECK(ipc_port_dealloc(b, bp) == 0 && ptype(b, bp) == PT_DEAD);
    CHECK(ipc_port_dealloc(b, bp) == 0 && ptype(b, bp) == 0xFFFF);
    CHECK(ipc_port_dealloc(b, bso) == 0 && ptype(b, bso) == 0xFFFF);
    CHECK(ipc_port_insert(b, 0x999, MSG_COPY_SEND) == -E_BADF);

    uint32_t q = new_port(a);
    uint32_t bq = grant(b, a, q, MSG_MAKE_SEND);
    ipc_port_set_qlimit(a, q, 1);
    CHECK(send_val(b, bq, MSG_COPY_SEND, 1, 1, 0) == 0);
    struct sendjob j;
    struct thread *t = start_send(&j, b, bq, MSG_COPY_SEND, 2, SEM_FOREVER);
    thread_sleep_ms(15);
    CHECK(!j.done);
    CHECK(ipc_port_destroy(a, q) == 0);
    thread_join(t);
    CHECK(j.done && j.rc == -E_PIPE);
    CHECK(ptype(b, bq) == PT_DEAD);

    struct ipc_space *c = ipc_space_create();
    uint32_t pc = new_port(c);
    uint32_t bc = grant(b, c, pc, MSG_MAKE_SEND);
    CHECK(send_val(b, bc, MSG_COPY_SEND, 1, 1, 0) == 0);
    ipc_space_destroy(c);
    CHECK(send_val(b, bc, MSG_COPY_SEND, 1, 1, 0) == -E_PIPE);

    ipc_space_destroy(a);
    ipc_space_destroy(b);
    klog(LOG_INFO, "ipctest: dead names, queued-message cleanup, blocked-sender wake on death ok");
}

static void test_chain(void)
{
    enum { N = 240 };
    struct ipc_space *a = ipc_space_create();
    uint32_t pn[N];
    unsigned base_ports = ipc_live_ports();
    unsigned base_msgs = ipc_live_msgs();

    for (int i = 0; i < N; i++) {
        pn[i] = new_port(a);
        CHECK(pn[i] != 0);
        ipc_port_insert(a, pn[i], MSG_MAKE_SEND);
    }
    for (int i = 0; i + 1 < N; i++) {
        struct msg_desc d = { pn[i + 1], MSG_MOVE_RECEIVE };
        CHECK(send_full(a, pn[i], MSG_COPY_SEND, 0, 0, 1, &d, 1) == 0);
    }
    CHECK(ipc_live_ports() == base_ports + N);
    CHECK(ipc_live_msgs() == base_msgs + N - 1);

    CHECK(ipc_port_destroy(a, pn[0]) == 0);
    CHECK(ipc_live_msgs() == base_msgs);
    CHECK(ptype(a, pn[N - 1]) == PT_DEAD);
    CHECK(send_val(a, pn[N - 1], MSG_COPY_SEND, 1, 1, 0) == -E_PIPE);

    ipc_space_destroy(a);
    CHECK(ipc_live_ports() == base_ports);
    klog(LOG_INFO, "ipctest: %d-deep receive-right chain torn down iteratively", N);
}

static int run_user(const char *name, uint64_t off)
{
    struct task *t = task_create(name, user_tests_start, IMG_SIZE, off, 0);
    return t ? task_wait(t) : (int)0x80000000;
}

static void test_user(void)
{
    unsigned ports0 = ipc_live_ports(), msgs0 = ipc_live_msgs();

    int rc = run_user("port-basic", UT_PORT_BASIC);
    if (rc != 0)
        klog(LOG_FAIL, "port-basic exited with 0x%x (the check number that failed)", rc);
    CHECK(rc == 0);
    CHECK(ipc_live_ports() == ports0 && ipc_live_msgs() == msgs0);

    struct ipc_space *k = ipc_space_create();
    uint32_t sp = new_port(k);
    struct task *srv = task_new("rpc-srv", user_tests_start, IMG_SIZE, UT_RPC_SERVER, 0);
    struct task *cli = task_new("rpc-cli", user_tests_start, IMG_SIZE, UT_RPC_CLIENT, 0);
    CHECK(srv && cli && sp);
    if (srv && cli && sp) {
        uint32_t cn = 0, sn = 0;
        CHECK(ipc_grant(cli->ipc, k, sp, MSG_MAKE_SEND, &cn) == 0);
        CHECK(ipc_grant(srv->ipc, k, sp, MSG_MOVE_RECEIVE, &sn) == 0);
        cli->arg = cn;
        srv->arg = sn;
        task_start(cli);
        task_start(srv);
        int rs = task_wait(srv);
        int rcl = task_wait(cli);
        if (rs || rcl)
            klog(LOG_FAIL, "rpc: server exit 0x%x, client exit 0x%x", rs, rcl);
        CHECK(rs == 0 && rcl == 0);
    }
    ipc_space_destroy(k);
    CHECK(ipc_live_ports() == ports0 && ipc_live_msgs() == msgs0);

    k = ipc_space_create();
    uint32_t xp = new_port(k);
    ipc_port_insert(k, xp, MSG_MAKE_SEND);
    struct task *v = task_new("victim", user_tests_start, IMG_SIZE, UT_FAULT_K, 0);
    CHECK(v && xp);
    if (v && xp) {
        uint32_t vn;
        CHECK(ipc_grant(v->ipc, k, xp, MSG_MOVE_RECEIVE, &vn) == 0);
        CHECK(send_val(k, xp, MSG_COPY_SEND, 1, 1, 0) == 0);
        task_start(v);
        CHECK(task_wait(v) == FAULT(14));
        CHECK(send_val(k, xp, MSG_COPY_SEND, 2, 2, 0) == -E_PIPE);
        CHECK(ptype(k, xp) == PT_DEAD);
        CHECK(ipc_live_msgs() == msgs0);
    }
    ipc_space_destroy(k);

    k = ipc_space_create();
    xp = new_port(k);
    ipc_port_insert(k, xp, MSG_MAKE_SEND);
    ipc_port_set_qlimit(k, xp, 1);
    v = task_new("owner", user_tests_start, IMG_SIZE, UT_SPIN, 0);
    CHECK(v && xp);
    if (v && xp) {
        uint32_t vn;
        CHECK(ipc_grant(v->ipc, k, xp, MSG_MOVE_RECEIVE, &vn) == 0);
        CHECK(send_val(k, xp, MSG_COPY_SEND, 1, 1, 0) == 0);
        struct sendjob j;
        task_start(v);
        struct thread *t = start_send(&j, k, xp, MSG_COPY_SEND, 2, SEM_FOREVER);
        thread_sleep_ms(30);
        CHECK(!j.done);
        CHECK(task_wait(v) == 0);
        thread_join(t);
        CHECK(j.done && j.rc == -E_PIPE);
    }
    ipc_space_destroy(k);

    CHECK(ipc_live_ports() == ports0 && ipc_live_msgs() == msgs0);
    klog(LOG_INFO, "ipctest: ring-3 syscalls, cross-task RPC with a send-once reply, task death cleanup ok");
}

static void test_leaks(void)
{
    uint64_t fr = pmm_free_frames();
    unsigned th = sched_thread_count();
    unsigned ports0 = ipc_live_ports(), msgs0 = ipc_live_msgs();

    for (int i = 0; i < 20; i++) {
        struct ipc_space *a = ipc_space_create(), *b = ipc_space_create();
        uint32_t p = new_port(a), q = new_port(b);
        uint32_t bp = grant(b, a, p, MSG_MAKE_SEND);
        send_full(b, bp, MSG_COPY_SEND, q, MSG_MAKE_SEND_ONCE, 1, NULL, 0);
        struct msg_desc d = { q, MSG_MAKE_SEND };
        send_full(b, bp, MSG_COPY_SEND, 0, 0, 2, &d, 1);
        if (i & 1)
            recv_(a, p, 0);
        ipc_space_destroy(i & 1 ? a : b);
        ipc_space_destroy(i & 1 ? b : a);
    }
    CHECK(ipc_live_ports() == ports0 && ipc_live_msgs() == msgs0);
    CHECK(sched_thread_count() == th);
    CHECK(pmm_free_frames() + 2 >= fr);
    CHECK(heap_check());
    klog(LOG_INFO, "ipctest: 20 space pairs created and torn down, no ports/messages/frames leaked");
}

unsigned ipctest_run(void)
{
    n_checks = n_fail = 0;
    unsigned ports0 = ipc_live_ports(), msgs0 = ipc_live_msgs();
    uint64_t sent0 = ipc_msgs_sent();

    test_names();
    test_basic();
    test_queue();
    test_transfer();
    test_death();
    test_chain();
    test_user();
    test_leaks();

    CHECK(ipc_live_ports() == ports0 && ipc_live_msgs() == msgs0);
    klog(n_fail ? LOG_FAIL : LOG_OK, "IPCTEST %s (%u checks, %u failed; %lu messages sent)",
         n_fail ? "FAIL" : "PASS", n_checks, n_fail, ipc_msgs_sent() - sent0);
    console_set_color(n_fail ? C_LRED : C_LGREEN, C_BLACK);
    kprintf("\nStep 3b (Mach ports + rights) %s.\n",
            n_fail ? "has FAILURES" : "complete");
    console_set_color(C_LGRAY, C_BLACK);
    return n_fail;
}
