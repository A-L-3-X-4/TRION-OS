#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "abi.h"
#include "ipc.h"
#include "obj.h"
#include "ns.h"

static unsigned n_checks, n_fail;
static void check_(bool ok, const char *expr, int line)
{
    n_checks++;
    if (!ok) {
        n_fail++;
        klog(LOG_FAIL, "nstest.c:%d: %s", line, expr);
    }
}
#define CHECK(c) check_((c), #c, __LINE__)
#define IMG_SIZE ((size_t)(user_tests_end - user_tests_start))

static struct object *mk_event(void)
{
    struct object *o = NULL;
    return obj_event_create(true, false, &o) ? NULL : o;
}

static void test_paths(void)
{
    unsigned base = obj_live();
    struct object *o = NULL;

    CHECK(ns_mkdir("\\T") == 0);
    CHECK(ns_mkdir("\\T\\Sub") == 0);
    CHECK(ns_mkdir("\\T") == -E_EXIST);
    CHECK(ns_mkdir("\\") == -E_EXIST);
    CHECK(ns_mkdir("\\Nope\\X") == -E_NOENT);
    CHECK(ns_mkdir("T") == -E_INVAL);
    CHECK(ns_mkdir("") == -E_INVAL);
    CHECK(ns_mkdir("\\T\\") == -E_INVAL);
    CHECK(ns_mkdir("\\\\T") == -E_INVAL);
    CHECK(ns_mkdir("\\T\\a b") == -E_INVAL);
    CHECK(ns_mkdir("\\T\\0123456789012345678901234567890") == 0);
    CHECK(ns_mkdir("\\T\\01234567890123456789012345678901") == -E_INVAL);
    char longp[NS_PATH_MAX + 8];
    memset(longp, 'x', sizeof(longp));
    longp[0] = '\\';
    longp[sizeof(longp) - 1] = 0;
    CHECK(ns_mkdir(longp) == -E_INVAL);

    CHECK(ns_lookup("\\", &o) == 0 && o->type == &directory_type);
    obj_unref(o);
    CHECK(ns_lookup("\\T\\Sub", &o) == 0 && o->type == &directory_type);
    obj_unref(o);
    CHECK(ns_lookup("\\t", &o) == -E_NOENT);
    CHECK(ns_lookup("\\Services", &o) == 0);
    obj_unref(o);

    struct object *ev = mk_event();
    CHECK(ev && ns_insert("\\T\\Sub\\ev", ev) == 0);
    CHECK(ev->refs == 2);
    CHECK(ns_insert("\\T\\Sub\\ev", ev) == -E_EXIST);
    CHECK(ns_lookup("\\T\\Sub\\ev", &o) == 0 && o == ev);
    obj_unref(o);
    CHECK(ns_lookup("\\T\\Sub\\ev\\x", &o) == -E_NOTDIR);
    CHECK(ns_insert("\\T\\Sub\\ev\\x", ev) == -E_NOTDIR);
    CHECK(ns_unlink("\\T") == -E_NOTEMPTY);
    CHECK(ns_unlink("\\T\\Sub") == -E_NOTEMPTY);
    CHECK(ns_unlink("\\") == -E_INVAL);
    CHECK(ns_unlink("\\T\\Sub\\nothing") == -E_NOENT);

    CHECK(ns_unlink("\\T\\Sub\\ev") == 0);
    CHECK(ev->refs == 1);
    CHECK(ns_lookup("\\T\\Sub\\ev", &o) == -E_NOENT);
    obj_unref(ev);
    CHECK(ns_unlink("\\T\\Sub") == 0);
    CHECK(ns_unlink("\\T\\0123456789012345678901234567890") == 0);
    CHECK(ns_unlink("\\T") == 0);
    CHECK(obj_live() == base);
    klog(LOG_INFO, "nstest: path rules, nesting, case sensitivity, name-owned refs, unlink ok");
}

static void test_fill(void)
{
    unsigned base = obj_live();
    CHECK(ns_mkdir("\\F") == 0);
    char p[24];
    unsigned made = 0;
    int rc = 0;
    for (unsigned i = 0; rc == 0 && i < NS_DIR_MAX + 5; i++) {
        p[0] = '\\'; p[1] = 'F'; p[2] = '\\';
        p[3] = 'a' + (char)(i / 26);
        p[4] = 'a' + (char)(i % 26);
        p[5] = 0;
        struct object *e = mk_event();
        rc = e ? ns_insert(p, e) : -E_NOMEM;
        if (e)
            obj_unref(e);
        if (rc == 0)
            made++;
    }
    CHECK(rc == -E_NOSPC && made == NS_DIR_MAX);
    for (unsigned i = 0; i < made; i++) {
        p[0] = '\\'; p[1] = 'F'; p[2] = '\\';
        p[3] = 'a' + (char)(i / 26);
        p[4] = 'a' + (char)(i % 26);
        p[5] = 0;
        CHECK(ns_unlink(p) == 0);
    }
    CHECK(ns_unlink("\\F") == 0);
    CHECK(obj_live() == base);
    klog(LOG_INFO, "nstest: %u entries in one directory, limit enforced, all unlinked and freed", made);
}

static int run(const char *name, uint64_t off)
{
    struct task *t = task_create(name, user_tests_start, IMG_SIZE, off, 0);
    return t ? task_wait(t) : -1;
}

static void test_user(void)
{
    unsigned base = obj_live(), ports = ipc_live_ports(), msgs = ipc_live_msgs();

    int rc = run("ns-basic", UT_NS_BASIC);
    if (rc)
        klog(LOG_FAIL, "ns-basic exited with 0x%x (the check that failed)", rc);
    CHECK(rc == 0);
    CHECK(obj_live() == base && ipc_live_ports() == ports && ipc_live_msgs() == msgs);

    struct task *cli = task_create("ns-client", user_tests_start, IMG_SIZE, UT_NS_CLIENT, 0);
    thread_sleep_ms(30);
    struct task *srv = task_create("ns-server", user_tests_start, IMG_SIZE, UT_NS_SERVER, 0);
    int rs = srv ? task_wait(srv) : -1, rcl = cli ? task_wait(cli) : -1;
    if (rs || rcl)
        klog(LOG_FAIL, "ns service: server exit 0x%x, client exit 0x%x", rs, rcl);
    CHECK(rs == 0 && rcl == 0);
    CHECK(obj_live() == base && ipc_live_ports() == ports && ipc_live_msgs() == msgs);

    struct ipc_space *a = ipc_space_create(), *b = ipc_space_create();
    uint32_t pa = 0, pn = 0, n = 0;
    CHECK(a && b && ipc_port_alloc(a, &pa) == 0);
    CHECK(ipc_port_insert(a, pa, MSG_MAKE_SEND) == 0);
    CHECK(ns_publish("\\Services\\orphan", a, pa) == 0);
    CHECK(ns_publish("\\Services\\orphan", a, pa) == -E_EXIST);
    CHECK(ns_publish("\\Services\\orphan2", a, 0x555) == -E_BADF);
    CHECK(ns_lookup_port("\\Services\\orphan", b, &n) == 0);
    CHECK(ipc_port_info(b, n, &(struct port_info){ 0 }) == 0);
    CHECK(ipc_port_alloc(b, &pn) == 0);
    ipc_space_destroy(a);
    CHECK(ns_lookup_port("\\Services\\orphan", b, &pn) == -E_PIPE);
    CHECK(ns_unlink("\\Services\\orphan") == 0);
    CHECK(ns_lookup_port("\\Services\\orphan", b, &pn) == -E_NOENT);
    ipc_space_destroy(b);
    CHECK(obj_live() == base && ipc_live_ports() == ports && ipc_live_msgs() == msgs);
    klog(LOG_INFO, "nstest: ring-3 namespace syscalls, service lookup by name between tasks ok");
}

unsigned nstest_run(void)
{
    n_checks = n_fail = 0;
    unsigned base = obj_live();
    test_paths();
    test_fill();
    test_user();
    CHECK(heap_check());
    (void)base;
    klog(n_fail ? LOG_FAIL : LOG_OK, "NSTEST %s (%u checks, %u failed)",
         n_fail ? "FAIL" : "PASS", n_checks, n_fail);
    console_set_color(n_fail ? C_LRED : C_LGREEN, C_BLACK);
    kprintf("\nStep 4b (namespace + named ports) %s.\n", n_fail ? "has FAILURES" : "complete");
    console_set_color(C_LGRAY, C_BLACK);
    return n_fail;
}
