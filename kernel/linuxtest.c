#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "obj.h"
#include "vfs.h"
#include "blk.h"
#include "timer.h"
#include "linux.h"

extern const uint8_t lxmin_start[], lxmin_end[], lxexec_start[], lxexec_end[], lxglibc_start[], lxglibc_end[];

static unsigned n_checks, n_fail;
static void check_(bool ok, const char *expr, int line)
{
    n_checks++;
    if (!ok) {
        n_fail++;
        klog(LOG_FAIL, "linuxtest.c:%d: %s", line, expr);
    }
}
#define CHECK(c) check_((c), #c, __LINE__)
#define FAULT(vec) (-(TASK_FAULT_BASE + (vec)))
#define LEN(s, e) ((size_t)((e) - (s)))

static struct volume *V;

static const char *const ENV[] = { "LXTEST=1", "HOME=/", NULL };

static int run_img(const void *img, size_t n, const char *const *argv)
{
    int err = 0;
    struct task *t = linux_task_new(argv[0], img, n, argv, ENV, &err);
    if (!t)
        return 0x7FFF0000 + (-err);
    task_start(t);
    return task_wait(t);
}

static int run_min(const char *mode, const char *arg)
{
    const char *argv[] = { "lxmin", mode, arg, NULL };
    return run_img(lxmin_start, LEN(lxmin_start, lxmin_end), argv);
}

static bool put_file(const char *rel, const void *d, size_t n)
{
    struct file *f;
    if (vfs_open(V, rel, FD_OVERWRITE_IF, FILE_READ_DATA | FILE_WRITE_DATA, 0, true, &f))
        return false;
    bool ok = file_write(f, d, n) == (long)n;
    obj_unref(&f->o);
    return ok;
}

static long file_size(const char *rel)
{
    struct file_stat st;
    int rc = vfs_stat(V, rel, &st);
    return rc ? rc : (long)st.size;
}

static int dir_count(const char *rel)
{
    struct file *f;
    if (vfs_open(V, rel, FD_OPEN, FILE_READ_DATA, FO_DIRECTORY, false, &f))
        return -1;
    struct fs_dirent *de = kmalloc(sizeof(*de));
    int n = 0;
    while (file_readdir(f, (uint32_t)n, de) == 0)
        n++;
    kfree(de);
    obj_unref(&f->o);
    return n;
}

static int try_load(const uint8_t *img, size_t n) { return linux_elf_probe(img, n); }

static void u16w(uint8_t *b, size_t off, uint16_t v) { memcpy(b + off, &v, 2); }
static void u64w(uint8_t *b, size_t off, uint64_t v) { memcpy(b + off, &v, 8); }
static uint64_t u64r(const uint8_t *b, size_t off) { uint64_t v; memcpy(&v, b + off, 8); return v; }

static void test_loader(void)
{
    size_t n = LEN(lxmin_start, lxmin_end);
    uint8_t *img = kmalloc(n);
    CHECK(img != NULL);
    if (!img)
        return;

    memcpy(img, lxmin_start, n);
    CHECK(try_load(img, n) == 0);

    uint64_t phoff = u64r(img, 32);
    unsigned phnum = img[56] | (img[57] << 8);
    int load0 = -1, load_x = -1, note = -1;
    for (unsigned i = 0; i < phnum; i++) {
        uint32_t type;
        uint32_t fl;
        memcpy(&type, img + phoff + i * 56, 4);
        memcpy(&fl, img + phoff + i * 56 + 4, 4);
        if (type == 1 && load0 < 0) load0 = (int)i;
        if (type == 1 && (fl & 1)) load_x = (int)i;
        if (type == 4 && note < 0) note = (int)i;
    }
    CHECK(load0 >= 0 && load_x >= 0);
    size_t ph0 = phoff + (size_t)load0 * 56, phx = phoff + (size_t)load_x * 56;

    uint64_t free0 = pmm_free_frames();
    unsigned tasks0 = task_live_count();

    #define MUT(code, want) do { memcpy(img, lxmin_start, n); code; CHECK(try_load(img, n) == (want)); } while (0)
    MUT(img[0] = 0x7E, -E_NOEXEC);
    MUT(img[4] = 1, -E_NOEXEC);
    MUT(img[5] = 2, -E_NOEXEC);
    MUT(u16w(img, 18, 3), -E_NOEXEC);
    MUT(u16w(img, 16, 1), -E_NOEXEC);
    MUT(u16w(img, 16, 4), -E_NOEXEC);
    MUT(u16w(img, 56, 0), -E_NOEXEC);
    MUT(u16w(img, 56, 65), -E_NOEXEC);
    MUT(u16w(img, 54, 32), -E_NOEXEC);
    MUT(u64w(img, 32, n - 10), -E_NOEXEC);
    MUT(u64w(img, 32, ~0ULL - 20), -E_NOEXEC);
    MUT(u64w(img, ph0 + 32, u64r(img, ph0 + 40) + 1), -E_NOEXEC);
    MUT(u64w(img, ph0 + 8, n), -E_NOEXEC);
    MUT(u64w(img, ph0 + 8, ~0ULL - 100), -E_NOEXEC);
    MUT(u64w(img, ph0 + 16, 0xFFFFFFFFFFFFF000ULL); u64w(img, ph0 + 24, 0), -E_NOEXEC);
    MUT(u64w(img, ph0 + 16, u64r(img, ph0 + 16) + 1), -E_NOEXEC);
    MUT(u64w(img, phx + 40, 1ULL << 40); u64w(img, phx + 32, 4096), -E_NOEXEC);
    MUT(u64w(img, phx + 40, ~0ULL - 4096), -E_NOEXEC);
    MUT(u64w(img, 24, 0), -E_NOEXEC);
    MUT(u64w(img, 24, 0xFFFFFFFFFFFFFFF0ULL), -E_NOEXEC);
    MUT(u16w(img, 16, 2), -E_NOEXEC);
    if (note >= 0)
        MUT(memcpy(img + phoff + (size_t)note * 56, &(uint32_t){ 3 }, 4), -E_NOEXEC);
    MUT(memset(img + phoff, 0, 56 * phnum), -E_NOEXEC);
    #undef MUT
    CHECK(try_load(img, 20) == -E_NOEXEC);
    CHECK(try_load(img, 0) == -E_NOEXEC);
    CHECK(try_load(lxmin_start, 64) == -E_NOEXEC);
    CHECK(try_load(lxglibc_start + 0, LEN(lxglibc_start, lxglibc_end)) == 0);

    {
        size_t big = 70 * 1024;
        char *s = kmalloc(big);
        memset(s, 'a', big - 1);
        s[big - 1] = 0;
        const char *argv[] = { s, NULL };
        int err = 0;
        struct task *t = linux_task_new("big", lxmin_start, n, argv, ENV, &err);
        CHECK(t == NULL && err == -E_2BIG);
        kfree(s);
    }
    kfree(img);
    CHECK(pmm_free_frames() == free0);
    CHECK(task_live_count() == tasks0);
    klog(LOG_INFO, "linuxtest: ELF loader rejects malformed / dynamic / out-of-window files, no leaks");
}

static void test_programs(void)
{
    const char *argv_m[] = { "lxmin", "main", "/Volumes/lx", NULL };
    CHECK(put_file("IN.TXT", "hello from the kernel\n", 22));

    int rc = run_img(lxmin_start, LEN(lxmin_start, lxmin_end), argv_m);
    CHECK(rc == 0);
    CHECK(file_size("OUT.TXT") == 26);
    CHECK(file_size("t1.txt") == -E_NOENT && file_size("big.bin") == -E_NOENT);
    CHECK(dir_count("") == 2);
    {
        char b[32];
        struct file *f;
        CHECK(vfs_open(V, "OUT.TXT", FD_OPEN, FILE_READ_DATA, 0, false, &f) == 0);
        if (!f) return;
        CHECK(file_read(f, b, 26) == 26 && !memcmp(b, "written by a Linux binary\n", 26));
        obj_unref(&f->o);
    }
    vfs_remove(V, "OUT.TXT");
    klog(LOG_INFO, "linuxtest: lxmin (static-PIE, freestanding): startup state, files, mmap/brk, TLS, SSE ok");

    rc = run_img(lxexec_start, LEN(lxexec_start, lxexec_end), argv_m);
    CHECK(rc == 0);
    CHECK(file_size("OUT.TXT") == 26);
    vfs_remove(V, "OUT.TXT");
    klog(LOG_INFO, "linuxtest: lxexec (ET_EXEC linked at 0x8000400000) ok");

    {
        char dl[VFS_VOL_NAME_MAX + 1], base1[8], base2[8];
        int letter = 0;
        for (int l = 'C'; l <= 'Z'; l++)
            if (vfs_dos_volume(l, dl) == 0 && !strcmp(dl, "lx"))
                letter = l;
        CHECK(letter != 0);
        base1[0] = base2[0] = (char)letter;
        base1[1] = base2[1] = ':';
        base1[2] = '\\'; base1[3] = 0;
        base2[2] = '/';  base2[3] = 0;
        const char *argv_w[] = { "lxglibc", base1, NULL };
        CHECK(run_img(lxglibc_start, LEN(lxglibc_start, lxglibc_end), argv_w) == 0);
        CHECK(file_size("GLIBC.TXT") == -E_NOENT && file_size("GLIBC2.TXT") == -E_NOENT);
        argv_w[1] = base2;
        CHECK(run_img(lxglibc_start, LEN(lxglibc_start, lxglibc_end), argv_w) == 0);
        CHECK(file_size("GLIBC.TXT") == -E_NOENT && file_size("GLIBC2.TXT") == -E_NOENT);
        klog(LOG_INFO, "linuxtest: the glibc program run with a Windows-style directory (L:\\ and L:/) reaches the same volume");
    }

    const char *argv_g[] = { "lxglibc", "/Volumes/lx", NULL };
    rc = run_img(lxglibc_start, LEN(lxglibc_start, lxglibc_end), argv_g);
    CHECK(rc == 0);
    CHECK(file_size("GLIBC.TXT") == -E_NOENT && file_size("GLIBC2.TXT") == -E_NOENT);
    klog(LOG_INFO, "linuxtest: static glibc binary (stdio, malloc, mremap fallback, qsort, libm, readdir) ok");

    CHECK(run_min("exit42", "") == 42);
    CHECK(run_min("tty", "") == 0);
    CHECK(run_min("fixed_low", "") == 0);
    CHECK(run_min("stack_ok", "") == 0);
}

static void test_from_file(void)
{
    CHECK(vfs_mkdir(V, "BIN") == 0);
    struct file *f;
    CHECK(vfs_open(V, "BIN/LXMIN", FD_CREATE, FILE_WRITE_DATA, 0, true, &f) == 0);
    if (f) {
        size_t n = LEN(lxmin_start, lxmin_end);
        CHECK(file_write(f, lxmin_start, n) == (long)n);
        obj_unref(&f->o);
    }
    CHECK(put_file("BIN/NOTELF.TXT", "just some text, definitely not an executable........................................", 80));

    int err = 0;
    const char *argv[] = { "lxmin", "exit42", NULL };
    struct task *t = linux_task_new_from_file("\\Volumes\\lx\\BIN\\LXMIN", argv, ENV, &err);
    CHECK(t != NULL && err == 0);
    if (t) {
        task_start(t);
        CHECK(task_wait(t) == 42);
    }
    CHECK(linux_task_new_from_file("\\Volumes\\lx\\BIN\\NOPE", argv, ENV, &err) == NULL && err == -E_NOENT);
    CHECK(linux_task_new_from_file("\\Volumes\\lx\\BIN\\NOTELF.TXT", argv, ENV, &err) == NULL && err == -E_NOEXEC);
    CHECK(linux_task_new_from_file("\\Volumes\\lx\\BIN", argv, ENV, &err) == NULL && err != 0);
    CHECK(linux_task_new_from_file("\\Volumes\\nodisk\\X", argv, ENV, &err) == NULL && err == -E_NOENT);
    CHECK(linux_task_new_from_file("\\elsewhere", argv, ENV, &err) == NULL && err == -E_NOENT);
    CHECK(vfs_remove(V, "BIN/LXMIN") == 0 && vfs_remove(V, "BIN/NOTELF.TXT") == 0 && vfs_remove(V, "BIN") == 0);
    klog(LOG_INFO, "linuxtest: executing a Linux binary from a VFS path ok");
}

static void test_faults(void)
{
    CHECK(run_min("fault_null", "") == FAULT(14));
    CHECK(run_min("fault_ro", "") == FAULT(14));
    CHECK(run_min("fault_none", "") == FAULT(14));
    CHECK(run_min("fault_nx", "") == FAULT(14));
    CHECK(run_min("fault_unmapped", "") == FAULT(14));
    CHECK(run_min("fault_kernel", "") == FAULT(14));
    CHECK(run_min("fault_priv", "") == FAULT(13));
    CHECK(run_min("stack_overflow", "") == FAULT(14));
    klog(LOG_INFO, "linuxtest: bad accesses kill the task, not the kernel");
}

static void test_fpu_fs(void)
{
    const char *seeds[] = { "1", "2", "3", "4" };
    struct task *t[4];
    const char *argv[4][4];
    for (int i = 0; i < 4; i++) {
        argv[i][0] = "spin"; argv[i][1] = "spin_fpu"; argv[i][2] = seeds[i]; argv[i][3] = NULL;
        int err;
        t[i] = linux_task_new("spin", lxmin_start, LEN(lxmin_start, lxmin_end), argv[i], ENV, &err);
        CHECK(t[i] != NULL);
    }
    for (int i = 0; i < 4; i++)
        if (t[i]) task_start(t[i]);
    for (int i = 0; i < 4; i++)
        if (t[i]) CHECK(task_wait(t[i]) == 0);

    struct task *n = task_create("native-fpu", user_tests_start, (size_t)(user_tests_end - user_tests_start), UT_FPU, 0);
    CHECK(n != NULL);
    if (n)
        CHECK(task_wait(n) == FAULT(6));
    klog(LOG_INFO, "linuxtest: SSE + FS base private per task under time slicing; native tasks still get #UD");
}

unsigned linuxtest_run(void)
{
    n_checks = n_fail = 0;
    unsigned vn0 = vfs_live_vnodes(), ob0 = obj_live(), tk0 = task_live_count();
    uint64_t t0 = clock_ns();

    struct blkdev *rd = blk_ramdisk_create("lxram", 150000);
    CHECK(rd != NULL);
    if (rd && fat32_format(rd, "LXTEST", 1) == 0 && vfs_mount("lx", rd, false, &V) == 0) {
        test_loader();
        uint64_t free0 = pmm_free_frames();
        test_programs();
        test_from_file();
        test_faults();
        test_fpu_fs();
        CHECK(pmm_free_frames() >= free0 - 4);
        CHECK(vfs_unmount("lx") == 0);
        obj_unref(&V->o);
        V = NULL;
    } else {
        CHECK(false);
    }
    if (rd)
        blk_unregister(rd);

    CHECK(vfs_live_vnodes() == vn0 && obj_live() == ob0 && task_live_count() == tk0);
    if (vfs_live_vnodes() != vn0 || obj_live() != ob0 || task_live_count() != tk0)
        klog(LOG_FAIL, "linuxtest: leak: vnodes %u->%u objects %u->%u tasks %u->%u", vn0, vfs_live_vnodes(), ob0,
             obj_live(), tk0, task_live_count());
    CHECK(heap_check());
    klog(n_fail ? LOG_FAIL : LOG_OK, "LXTEST %s (%u checks, %u failed, %lu ms)", n_fail ? "FAIL" : "PASS",
         n_checks, n_fail, (uint64_t)((clock_ns() - t0) / 1000000));
    console_set_color(n_fail ? C_LRED : C_LGREEN, C_BLACK);
    kprintf("\nStep 6 (ELF loader + Linux syscall shim) %s.\n", n_fail ? "has FAILURES" : "complete");
    console_set_color(C_LGRAY, C_BLACK);
    return n_fail;
}
