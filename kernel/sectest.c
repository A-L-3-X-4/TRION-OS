#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "abi.h"
#include "ipc.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"

static unsigned n_checks, n_fail;
static void check_(bool ok, const char *expr, int line)
{
    n_checks++;
    if (!ok) {
        n_fail++;
        klog(LOG_FAIL, "sectest.c:%d: %s", line, expr);
    }
}
#define CHECK(c) check_((c), #c, __LINE__)
#define IMG_SIZE ((size_t)(user_tests_end - user_tests_start))

#define ALICE 1000
#define BOB   1001
#define CAROL 1002
#define STAFF 2000
#define MODIFY EVENT_MODIFY_STATE
#define SYNC   ACCESS_SYNCHRONIZE
#define RC     ACCESS_READ_CONTROL
#define WDAC   ACCESS_WRITE_DAC
#define WOWN   ACCESS_WRITE_OWNER
#define DEL    ACCESS_DELETE

static struct token alice, bob, carol, sys;

static void tokens_init(void)
{
    static const uint32_t staff[] = { STAFF };
    sec_token_user(&alice, ALICE, staff, 1, 0);
    sec_token_user(&bob, BOB, NULL, 0, 0);
    sec_token_user(&carol, CAROL, NULL, 0, PRIV_TAKE_OWNERSHIP);
    sec_token_system(&sys);
}

static struct object *mk_event(void)
{
    struct object *o = NULL;
    return obj_event_create(true, false, &o) ? NULL : o;
}

#define ACE(t, f, sid, m) ((struct ace){ (t), (f), 0, (sid), (m) })

static uint32_t grant_of(const struct token *t, struct object *o, uint32_t want, int *rc)
{
    uint32_t g = 0xDEAD;
    *rc = sec_check(t, o, want, &g);
    return g;
}

static void test_algorithm(void)
{
    unsigned base = obj_live();
    struct object *e = mk_event();
    CHECK(e != NULL);
    uint32_t all = event_type.valid_access;
    int rc;
    uint32_t g;

    g = grant_of(&alice, e, ACCESS_MAXIMUM, &rc);
    CHECK(rc == 0 && g == all);
    g = grant_of(&alice, e, MODIFY | SYNC, &rc);
    CHECK(rc == 0 && g == (MODIFY | SYNC));
    g = grant_of(NULL, e, ACCESS_MAXIMUM, &rc);
    CHECK(rc == 0 && g == all);
    grant_of(&alice, e, 0x4, &rc);
    CHECK(rc == -E_INVAL);

    CHECK(sec_kset(e, BOB, NULL, 0) == 0);
    grant_of(&alice, e, SYNC, &rc);
    CHECK(rc == -E_ACCES);
    grant_of(&alice, e, ACCESS_MAXIMUM, &rc);
    CHECK(rc == -E_ACCES);
    g = grant_of(&bob, e, ACCESS_MAXIMUM, &rc);
    CHECK(rc == 0 && g == (RC | WDAC));
    grant_of(&bob, e, SYNC, &rc);
    CHECK(rc == -E_ACCES);
    grant_of(&bob, e, RC, &rc);
    CHECK(rc == 0);

    struct ace a1[] = { ACE(ACE_ALLOW, 0, STAFF, SYNC) };
    CHECK(sec_kset(e, BOB, a1, 1) == 0);
    grant_of(&alice, e, SYNC, &rc);
    CHECK(rc == 0);
    grant_of(&alice, e, SYNC | MODIFY, &rc);
    CHECK(rc == -E_ACCES);
    grant_of(&carol, e, SYNC, &rc);
    CHECK(rc == -E_ACCES);
    struct ace a2[] = { ACE(ACE_ALLOW, 0, SID_EVERYONE, SYNC) };
    CHECK(sec_kset(e, BOB, a2, 1) == 0);
    grant_of(&carol, e, SYNC, &rc);
    CHECK(rc == 0);

    struct ace d1[] = { ACE(ACE_DENY, 0, ALICE, MODIFY), ACE(ACE_ALLOW, 0, SID_EVERYONE, all) };
    CHECK(sec_kset(e, BOB, d1, 2) == 0);
    grant_of(&alice, e, SYNC, &rc);
    CHECK(rc == 0);
    grant_of(&alice, e, MODIFY, &rc);
    CHECK(rc == -E_ACCES);
    grant_of(&bob, e, MODIFY, &rc);
    CHECK(rc == 0);
    g = grant_of(&alice, e, ACCESS_MAXIMUM, &rc);
    CHECK(rc == 0 && g == (all & ~MODIFY));
    struct ace d2[] = { ACE(ACE_ALLOW, 0, SID_EVERYONE, all), ACE(ACE_DENY, 0, ALICE, MODIFY) };
    CHECK(sec_kset(e, BOB, d2, 2) == 0);
    grant_of(&alice, e, MODIFY, &rc);
    CHECK(rc == 0);

    struct ace d3[] = { ACE(ACE_DENY, 0, SID_EVERYONE, all) };
    CHECK(sec_kset(e, ALICE, d3, 1) == 0);
    g = grant_of(&alice, e, ACCESS_MAXIMUM, &rc);
    CHECK(rc == 0 && g == (RC | WDAC));
    grant_of(&bob, e, ACCESS_MAXIMUM, &rc);
    CHECK(rc == -E_ACCES);

    struct ace io[] = { ACE(ACE_ALLOW, ACE_INHERIT | ACE_INHERIT_ONLY, SID_EVERYONE, all) };
    CHECK(sec_kset(e, BOB, io, 1) == 0);
    grant_of(&alice, e, SYNC, &rc);
    CHECK(rc == -E_ACCES);

    struct ace gr[] = { ACE(ACE_ALLOW, 0, SID_EVERYONE, ACCESS_GENERIC_READ) };
    CHECK(sec_kset(e, BOB, gr, 1) == 0);
    g = grant_of(&alice, e, ACCESS_MAXIMUM, &rc);
    CHECK(rc == 0 && g == (SYNC | RC));
    g = grant_of(&alice, e, ACCESS_GENERIC_READ, &rc);
    CHECK(rc == 0 && g == (SYNC | RC));
    grant_of(&alice, e, ACCESS_GENERIC_WRITE, &rc);
    CHECK(rc == -E_ACCES);
    CHECK(sec_map_generic(&event_type, ACCESS_GENERIC_ALL) == all);

    CHECK(sec_kset(e, BOB, NULL, 0) == 0);
    g = grant_of(&sys, e, ACCESS_MAXIMUM, &rc);
    CHECK(rc == 0 && g == all);
    g = grant_of(&carol, e, ACCESS_MAXIMUM, &rc);
    CHECK(rc == 0 && g == WOWN);
    grant_of(&carol, e, DEL, &rc);
    CHECK(rc == -E_ACCES);

    CHECK(sec_kset(e, BOB, NULL, -1) == 0);
    grant_of(&alice, e, all, &rc);
    CHECK(rc == 0);
    CHECK(sec_kset(e, BOB, NULL, SD_ACE_MAX + 1) == -E_INVAL);

    obj_unref(e);
    CHECK(obj_live() == base);
    klog(LOG_INFO, "sectest: access check: NULL/empty DACL, owner, groups, ACE order, MAXIMUM, generic, privileges ok");
}

static uint32_t owner_of(struct object *o) { return o->sd ? o->sd->owner : 0; }
static uint32_t nace_of(struct object *o)  { return o->sd ? o->sd->nace : 0xFFFF; }

static int open_as(const struct token *t, const char *path, uint32_t want, uint32_t *granted)
{
    struct object *o;
    int rc = ns_lookup_as(t, path, &o);
    if (rc)
        return rc;
    rc = sec_check(t, o, want, granted);
    obj_unref(o);
    return rc;
}

static void test_namespace(void)
{
    unsigned base = obj_live(), ports = ipc_live_ports(), msgs = ipc_live_msgs();
    struct object *o = NULL;
    uint32_t g;

    CHECK(ns_mkdir_as(&alice, "\\A") == -E_ACCES);
    CHECK(ns_mkdir_as(&sys, "\\Sys") == 0);
    CHECK(ns_unlink_as(&alice, "\\Sys") == -E_ACCES);
    CHECK(ns_unlink_as(&sys, "\\Sys") == 0);
    CHECK(ns_lookup_as(&alice, "\\Services", &o) == 0);
    obj_unref(o);
    CHECK(ns_lookup_as(&alice, "\\Nope", &o) == -E_NOENT);

    CHECK(ns_mkdir_as(&alice, "\\Services\\alice") == 0);
    CHECK(ns_lookup("\\Services\\alice", &o) == 0);
    CHECK(owner_of(o) == ALICE);
    CHECK(nace_of(o) == 2);
    CHECK(o->sd->aces[0].sid == SID_EVERYONE && o->sd->aces[1].sid == ALICE);
    CHECK((o->sd->aces[0].flags & ACE_INHERIT) && !(o->sd->aces[0].flags & ACE_INHERIT_ONLY));
    obj_unref(o);

    CHECK(ns_mkdir_as(&bob, "\\Services\\alice\\b") == -E_ACCES);
    CHECK(ns_mkdir_as(&alice, "\\Services\\alice\\sub") == 0);
    CHECK(ns_lookup_as(&bob, "\\Services\\alice\\sub", &o) == 0);
    obj_unref(o);
    CHECK(ns_lookup_as(&bob, "\\Services\\alice\\nothing", &o) == -E_NOENT);

    struct object *ev = mk_event();
    CHECK(ev != NULL);
    CHECK(sec_obj_init(ev, ALICE) == 0);
    CHECK(ns_insert_as(&alice, "\\Services\\alice\\ev", ev) == 0);
    CHECK(ns_insert_as(&alice, "\\Services\\alice\\ev", ev) == -E_EXIST);
    CHECK(ns_insert_as(&bob, "\\Services\\alice\\ev2", ev) == -E_ACCES);
    CHECK(nace_of(ev) == 2 && !(ev->sd->aces[0].flags));
    CHECK(open_as(&bob, "\\Services\\alice\\ev", SYNC, &g) == 0 && g == SYNC);
    CHECK(open_as(&bob, "\\Services\\alice\\ev", MODIFY, &g) == -E_ACCES);
    CHECK(open_as(&bob, "\\Services\\alice\\ev", ACCESS_MAXIMUM, &g) == 0 && g == (SYNC | RC));
    CHECK(open_as(&alice, "\\Services\\alice\\ev", ACCESS_MAXIMUM, &g) == 0 && g == event_type.valid_access);
    CHECK(open_as(&alice, "\\Services\\alice\\ev", 0x4, &g) == -E_INVAL);
    CHECK(ns_unlink_as(&bob, "\\Services\\alice\\ev") == -E_ACCES);
    CHECK(ns_unlink_as(&carol, "\\Services\\alice\\ev") == -E_ACCES);

    struct ace deny_bob[] = { ACE(ACE_DENY, 0, BOB, SYNC), ACE(ACE_ALLOW, 0, SID_EVERYONE, SYNC | RC) };
    CHECK(sec_kset(ev, ALICE, deny_bob, 2) == 0);
    CHECK(open_as(&bob, "\\Services\\alice\\ev", SYNC, &g) == -E_ACCES);
    CHECK(open_as(&carol, "\\Services\\alice\\ev", SYNC, &g) == 0);
    CHECK(open_as(&alice, "\\Services\\alice\\ev", SYNC, &g) == 0);
    CHECK(sec_kset(ev, ALICE, NULL, -1) == 0);
    obj_unref(ev);

    struct object *ev2 = mk_event();
    struct ace only_carol[] = { ACE(ACE_ALLOW, 0, CAROL, SYNC) };
    CHECK(ev2 && sec_kset(ev2, ALICE, only_carol, 1) == 0);
    CHECK(ns_insert_as(&alice, "\\Services\\alice\\ev2", ev2) == 0);
    CHECK(nace_of(ev2) == 1 && ev2->sd->aces[0].sid == CAROL);
    CHECK(open_as(&carol, "\\Services\\alice\\ev2", SYNC, &g) == 0);
    CHECK(open_as(&bob, "\\Services\\alice\\ev2", SYNC, &g) == -E_ACCES);
    obj_unref(ev2);

    struct object *ev3 = mk_event();
    CHECK(ev3 && ns_insert_as(&bob, "\\Services\\alice\\ev3", ev3) == -E_ACCES);
    CHECK(ev3->refs == 1 && !ev3->sd);
    obj_unref(ev3);

    CHECK(ns_mkdir("\\Plain") == 0);
    struct object *ev4 = mk_event();
    CHECK(ev4 && sec_obj_init(ev4, ALICE) == 0);
    CHECK(ns_insert_as(&alice, "\\Plain\\ev4", ev4) == 0);
    CHECK(nace_of(ev4) == 1 && ev4->sd->aces[0].sid == ALICE);
    CHECK(open_as(&bob, "\\Plain\\ev4", SYNC, &g) == -E_ACCES);
    CHECK(open_as(&alice, "\\Plain\\ev4", ACCESS_MAXIMUM, &g) == 0 && g == event_type.valid_access);
    obj_unref(ev4);
    CHECK(ns_unlink_as(&alice, "\\Plain\\ev4") == 0);
    CHECK(ns_unlink("\\Plain") == 0);

    struct object *d, *tmp;
    CHECK(ns_lookup("\\Services\\alice", &d) == 0);
    struct ace hide[] = { ACE(ACE_DENY, 0, BOB, DIR_TRAVERSE | DIR_QUERY),
                          ACE(ACE_ALLOW, 0, SID_EVERYONE, ACCESS_GENERIC_READ | ACCESS_GENERIC_EXECUTE) };
    CHECK(sec_kset(d, ALICE, hide, 2) == 0);
    CHECK(ns_lookup_as(&bob, "\\Services\\alice\\sub", &tmp) == -E_ACCES);
    CHECK(ns_lookup_as(&alice, "\\Services\\alice\\sub", &tmp) == 0);
    obj_unref(tmp);
    CHECK(ns_lookup_as(&bob, "\\Services\\alice", &tmp) == 0);
    CHECK(sec_check(&bob, tmp, DIR_QUERY, &g) == -E_ACCES);
    CHECK(sec_check(&alice, tmp, DIR_QUERY, &g) == 0);
    obj_unref(tmp);

    struct ace none[] = { ACE(ACE_DENY, 0, SID_EVERYONE, DIR_QUERY | DIR_TRAVERSE | DIR_CREATE) };
    CHECK(sec_kset(d, ALICE, none, 1) == 0);
    CHECK(ns_mkdir_as(&alice, "\\Services\\alice\\x") == -E_ACCES);
    CHECK(sec_check(&alice, d, WDAC, &g) == 0);
    CHECK(sec_check(&alice, d, DIR_QUERY, &g) == -E_ACCES);
    CHECK(sec_check(&sys, d, DIR_CREATE, &g) == 0);
    obj_unref(d);

    struct ipc_space *sa = ipc_space_create(), *sb = ipc_space_create();
    uint32_t pa = 0, nb = 0;
    CHECK(sa && sb && ipc_port_alloc(sa, &pa) == 0);
    CHECK(ipc_port_insert(sa, pa, MSG_MAKE_SEND) == 0);
    CHECK(ns_publish_as(&alice, "\\Services\\pub", sa, pa) == 0);
    CHECK(ns_lookup("\\Services\\pub", &o) == 0 && owner_of(o) == ALICE);
    obj_unref(o);
    CHECK(ns_lookup_port_as(&bob, "\\Services\\pub", sb, &nb) == 0);
    CHECK(ns_lookup_as(&alice, "\\Services\\pub", &o) == 0);
    struct ace noconn[] = { ACE(ACE_DENY, 0, BOB, PORTREF_CONNECT | DEL),
                            ACE(ACE_ALLOW, 0, SID_EVERYONE, ACCESS_GENERIC_ALL) };
    CHECK(sec_kset(o, ALICE, noconn, 2) == 0);
    obj_unref(o);
    CHECK(ns_lookup_port_as(&bob, "\\Services\\pub", sb, &nb) == -E_ACCES);
    CHECK(ns_lookup_port_as(&carol, "\\Services\\pub", sb, &nb) == 0);
    CHECK(ns_publish_as(&bob, "\\Services\\pub2", sb, nb) == 0);
    CHECK(ns_unlink_as(&bob, "\\Services\\pub") == -E_ACCES);
    CHECK(ns_unlink_as(&bob, "\\Services\\pub2") == 0);
    ipc_space_destroy(sa);
    ipc_space_destroy(sb);

    CHECK(ns_unlink("\\Services\\pub") == 0);
    CHECK(ns_unlink("\\Services\\alice\\ev") == 0);
    CHECK(ns_unlink("\\Services\\alice\\ev2") == 0);
    CHECK(ns_unlink("\\Services\\alice\\sub") == 0);
    CHECK(ns_unlink("\\Services\\alice") == 0);
    CHECK(obj_live() == base && ipc_live_ports() == ports && ipc_live_msgs() == msgs);
    klog(LOG_INFO, "sectest: namespace: traverse/create/delete/query/connect rules, ownership, inheritance ok");
}

static int run_as(const char *name, uint64_t off, const struct token *tok)
{
    struct task *t = task_new(name, user_tests_start, IMG_SIZE, off, 0);
    if (!t)
        return -1;
    t->tok = *tok;
    task_start(t);
    return task_wait(t);
}

static void test_user(void)
{
    unsigned base = obj_live(), ports = ipc_live_ports(), msgs = ipc_live_msgs();

    int rc = run_as("sec-alice", UT_SEC_ALICE, &alice);
    if (rc)
        klog(LOG_FAIL, "sec-alice exited with 0x%x (the check that failed)", rc);
    CHECK(rc == 0);
    struct object *o = NULL;
    if (ns_lookup("\\Services\\sec\\ev", &o) == 0) {
        CHECK(owner_of(o) == ALICE);
        obj_unref(o);
    } else {
        CHECK(!"alice's event is not in the namespace");
    }

    rc = run_as("sec-bob", UT_SEC_BOB, &bob);
    if (rc)
        klog(LOG_FAIL, "sec-bob exited with 0x%x (the check that failed)", rc);
    CHECK(rc == 0);

    CHECK(ns_unlink("\\Services\\sec\\ev") == 0);
    CHECK(ns_unlink("\\Services\\sec") == 0);
    CHECK(obj_live() == base && ipc_live_ports() == ports && ipc_live_msgs() == msgs);

    struct task *t = task_new("sec-default", user_tests_start, IMG_SIZE, UT_CPL, 0);
    CHECK(t && t->tok.user == SID_SYSTEM && t->tok.privs == (PRIV_BYPASS_ACL | PRIV_TAKE_OWNERSHIP));
    if (t) {
        task_start(t);
        task_wait(t);
    }
    klog(LOG_INFO, "sectest: ring-3 tasks as alice and bob: tokens, GETSEC/SETSEC, denial, inheritance ok");
}

unsigned sectest_run(void)
{
    n_checks = n_fail = 0;
    tokens_init();
    test_algorithm();
    test_namespace();
    test_user();
    CHECK(heap_check());
    klog(n_fail ? LOG_FAIL : LOG_OK, "SECTEST %s (%u checks, %u failed)",
         n_fail ? "FAIL" : "PASS", n_checks, n_fail);
    console_set_color(n_fail ? C_LRED : C_LGREEN, C_BLACK);
    kprintf("\nStep 4c (ACLs + access checks) %s.\n", n_fail ? "has FAILURES" : "complete");
    console_set_color(C_LGRAY, C_BLACK);
    return n_fail;
}
