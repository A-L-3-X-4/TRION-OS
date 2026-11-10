#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "abi.h"
#include "obj.h"
#include "timer.h"

static unsigned n_checks, n_fail;
static void check_(bool ok, const char *expr, int line)
{
    n_checks++;
    if (!ok) {
        n_fail++;
        klog(LOG_FAIL, "objtest.c:%d: %s", line, expr);
    }
}
#define CHECK(c) check_((c), #c, __LINE__)
#define IMG_SIZE ((size_t)(user_tests_end - user_tests_start))

static struct object *mk_event(bool manual, bool sig)
{
    struct object *o = NULL;
    return obj_event_create(manual, sig, &o) ? NULL : o;
}

static void test_table(void)
{
    unsigned base = obj_live();
    struct handle_table *a = ht_create(), *b = ht_create();
    struct object *ev = mk_event(true, false), *sm = NULL, *got = NULL;
    CHECK(a && b && ev && obj_semaphore_create(1, 2, &sm) == 0);
    uint32_t full = event_type.valid_access, he, hs, h2, hd;
    uint32_t g;

    CHECK(ht_insert(a, ev, full, &he) == 0 && he != 0);
    CHECK(ht_insert(a, sm, semaphore_type.valid_access, &hs) == 0 && hs != he);
    CHECK(ht_insert(a, ev, 0x4, &h2) == -E_INVAL);
    obj_unref(ev);
    obj_unref(sm);
    CHECK(obj_live() == base + 2);

    CHECK(ht_lookup(a, he, &event_type, EVENT_MODIFY_STATE, &got, &g) == 0 && got == ev && g == full);
    obj_unref(got);
    CHECK(ht_lookup(a, he, &semaphore_type, 0, &got, NULL) == -E_INVAL);
    CHECK(ht_lookup(a, 0, NULL, 0, &got, NULL) == -E_BADF);
    CHECK(ht_lookup(a, he + 0x40, NULL, 0, &got, NULL) == -E_BADF);
    CHECK(ht_lookup(b, he, NULL, 0, &got, NULL) == -E_BADF);

    CHECK(ht_dup(b, a, he, ACCESS_SYNCHRONIZE, &hd) == 0);
    CHECK(ht_lookup(b, hd, NULL, ACCESS_SYNCHRONIZE, &got, NULL) == 0 && got == ev);
    obj_unref(got);
    CHECK(ht_lookup(b, hd, NULL, EVENT_MODIFY_STATE, &got, NULL) == -E_ACCES);
    CHECK(ht_dup(b, b, hd, EVENT_MODIFY_STATE, &h2) == -E_ACCES);
    CHECK(ht_dup(b, b, hd, 0, &h2) == 0 && h2 != hd);
    CHECK(ht_dup(b, a, 0x777, 0, &h2) == -E_BADF);
    CHECK(obj_live() == base + 2);

    CHECK(ht_close(a, he) == 0);
    CHECK(ht_close(a, he) == -E_BADF);
    CHECK(ht_lookup(b, hd, NULL, 0, &got, NULL) == 0);
    obj_unref(got);
    uint32_t he2;
    struct object *ev2 = mk_event(false, false);
    CHECK(ht_insert(a, ev2, event_type.valid_access, &he2) == 0 && he2 != he);
    obj_unref(ev2);

    unsigned made = 0;
    uint32_t hh;
    int rc;
    for (;;) {
        struct object *e = mk_event(false, false);
        if (!e) { rc = -E_NOMEM; break; }
        rc = ht_insert(b, e, ACCESS_SYNCHRONIZE, &hh);
        obj_unref(e);
        if (rc)
            break;
        made++;
    }
    CHECK(rc == -E_NOSPC && made > 400);
    ht_destroy(a);
    ht_destroy(b);
    CHECK(obj_live() == base);
    klog(LOG_INFO, "objtest: handle tables: types, access, dup narrowing, generations, limit, teardown ok");
}

struct wjob {
    struct object *o;
    volatile int   rc;
    volatile bool  done;
};

static int waiter_main(void *arg)
{
    struct wjob *j = arg;
    j->rc = obj_wait(j->o, SEM_FOREVER);
    j->done = true;
    return 0;
}

static void test_wait(void)
{
    unsigned base = obj_live();
    struct object *ev = mk_event(false, false), *me = mk_event(true, false), *sm = NULL;
    CHECK(obj_semaphore_create(0, 10, &sm) == 0);

    CHECK(obj_wait(ev, 0) == -E_AGAIN);
    uint64_t t0 = clock_ns();
    CHECK(obj_wait(ev, 20) == -E_TIMEDOUT);
    CHECK(clock_ns() - t0 >= 19000000ULL);

    struct wjob j[3] = { { ev, 0, false }, { ev, 0, false }, { me, 0, false } };
    struct thread *t0_ = thread_create("w0", waiter_main, &j[0], 0);
    struct thread *t1_ = thread_create("w1", waiter_main, &j[1], 0);
    thread_sleep_ms(20);
    CHECK(!j[0].done && !j[1].done);
    CHECK(obj_event_op(ev, EVENT_SET) == 0);
    thread_sleep_ms(20);
    CHECK(j[0].done != j[1].done);
    CHECK(obj_wait(ev, 0) == -E_AGAIN);
    CHECK(obj_event_op(ev, EVENT_SET) == 0);
    thread_join(t0_);
    thread_join(t1_);
    CHECK(j[0].done && j[1].done && j[0].rc == 0 && j[1].rc == 0);

    struct wjob m[2] = { { me, 0, false }, { me, 0, false } };
    struct thread *a = thread_create("m0", waiter_main, &m[0], 0);
    struct thread *b = thread_create("m1", waiter_main, &m[1], 0);
    thread_sleep_ms(20);
    CHECK(obj_event_op(me, EVENT_SET) == 0);
    thread_join(a);
    thread_join(b);
    CHECK(m[0].rc == 0 && m[1].rc == 0 && obj_wait(me, 0) == 0);
    CHECK(obj_event_op(me, EVENT_RESET) == 1 && obj_wait(me, 0) == -E_AGAIN);
    CHECK(obj_event_op(me, 99) == -E_INVAL);

    struct wjob s[3] = { { sm, 0, false }, { sm, 0, false }, { sm, 0, false } };
    struct thread *st[3];
    for (int i = 0; i < 3; i++)
        st[i] = thread_create("s", waiter_main, &s[i], 0);
    thread_sleep_ms(20);
    CHECK(obj_semaphore_release(sm, 2) == 0);
    thread_sleep_ms(30);
    CHECK(s[0].done + s[1].done + s[2].done == 2);
    CHECK(obj_semaphore_release(sm, 1) == 0);
    for (int i = 0; i < 3; i++)
        thread_join(st[i]);
    CHECK(s[0].rc == 0 && s[1].rc == 0 && s[2].rc == 0);
    CHECK(obj_semaphore_release(sm, 11) == -E_INVAL);

    obj_unref(ev);
    obj_unref(me);
    obj_unref(sm);
    CHECK(obj_live() == base);
    klog(LOG_INFO, "objtest: events (auto/manual) and semaphores block, wake the right number, time out ok");
}

static void test_user(void)
{
    unsigned base = obj_live();
    struct task *t = task_create("obj-basic", user_tests_start, IMG_SIZE, UT_OBJ_BASIC, 0);
    int rc = t ? task_wait(t) : -1;
    if (rc)
        klog(LOG_FAIL, "obj-basic exited with 0x%x (the check that failed)", rc);
    CHECK(rc == 0);
    CHECK(obj_live() == base);

    struct object *ev = mk_event(true, false);
    struct task *w = task_new("obj-waiter", user_tests_start, IMG_SIZE, UT_OBJ_WAITER, 0);
    CHECK(ev && w);
    if (ev && w) {
        uint32_t h;
        CHECK(ht_insert(w->ht, ev, ACCESS_SYNCHRONIZE, &h) == 0);
        w->arg = h;
        task_start(w);
        thread_sleep_ms(40);
        CHECK(obj_event_op(ev, EVENT_SET) == 0);
        rc = task_wait(w);
        CHECK(rc == 0);
    }
    if (ev)
        obj_unref(ev);
    CHECK(obj_live() == base);

    ev = mk_event(false, false);
    struct task *v = task_new("obj-victim", user_tests_start, IMG_SIZE, UT_FAULT_K, 0);
    if (ev && v) {
        uint32_t h;
        ht_insert(v->ht, ev, ACCESS_SYNCHRONIZE, &h);
        obj_unref(ev);
        task_start(v);
        CHECK(task_wait(v) == -(TASK_FAULT_BASE + 14));
    }
    CHECK(obj_live() == base);
    klog(LOG_INFO, "objtest: ring-3 handle syscalls, blocked task woken by the kernel, cleanup on exit/fault ok");
}

unsigned objtest_run(void)
{
    n_checks = n_fail = 0;
    unsigned base = obj_live();
    test_table();
    test_wait();
    test_user();
    CHECK(obj_live() == base);
    CHECK(heap_check());
    klog(n_fail ? LOG_FAIL : LOG_OK, "OBJTEST %s (%u checks, %u failed)",
         n_fail ? "FAIL" : "PASS", n_checks, n_fail);
    console_set_color(n_fail ? C_LRED : C_LGREEN, C_BLACK);
    kprintf("\nStep 4a (object core + handles + events/semaphores) %s.\n",
            n_fail ? "has FAILURES" : "complete");
    console_set_color(C_LGRAY, C_BLACK);
    return n_fail;
}
