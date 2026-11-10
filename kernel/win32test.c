#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "obj.h"
#include "vfs.h"
#include "blk.h"
#include "timer.h"
#include "linux.h"
#include "win32.h"

extern const uint8_t w32min_start[], w32min_end[], w32fix_start[], w32fix_end[], w32norel_start[], w32norel_end[],
                     w32crt_start[], w32crt_end[], lxmin_start[], lxmin_end[];

static unsigned n_checks, n_fail;
static void check_(bool ok, const char *expr, int line)
{
    n_checks++;
    if (!ok) { n_fail++; klog(LOG_FAIL, "win32test.c:%d: %s", line, expr); }
}
#define CHECK(c) check_((c), #c, __LINE__)
#define FAULT(vec) (-(TASK_FAULT_BASE + (vec)))
#define LEN(s, e) ((size_t)((e) - (s)))

static struct volume *V;
static const char *const ENV[] = { "W32TEST=1", "HOME=\\", NULL };
static char cap[8192];

static int run_img(const void *img, size_t n, const char *const *argv, bool capture)
{
    int err = 0;
    struct task *t = win32_task_new(argv[0], img, n, argv, ENV, "\\Volumes\\w32", NULL, &err);
    if (!t)
        return 0x7FFF0000 + (-err);
    if (capture)
        console_capture_begin(cap, sizeof(cap));
    task_start(t);
    int rc = task_wait(t);
    if (capture)
        console_capture_end();
    return rc;
}
static int run_min(const char *mode, const char *arg, bool capture)
{
    const char *argv[] = { "w32min", mode, arg, NULL };
    if (!arg) argv[2] = NULL;
    return run_img(w32min_start, LEN(w32min_start, w32min_end), argv, capture);
}
static char *sstr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++)
        if (!memcmp(h, n, nl))
            return (char *)h;
    return NULL;
}
static void scopy(char *d, const char *s) { while ((*d++ = *s++)) ; }
static bool has(const char *s) { return sstr(cap, s) != NULL; }

static bool put_file(const char *rel, const void *d, size_t n)
{
    struct file *f;
    int rc = vfs_open(V, rel, FD_OVERWRITE_IF, FILE_READ_DATA | FILE_WRITE_DATA, 0, true, &f);
    if (rc) {
        klog(LOG_WARN, "put_file(%s): vfs_open -> %d", rel, rc);
        return false;
    }
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
static bool is_dir(const char *rel)
{
    struct file_stat st;
    return vfs_stat(V, rel, &st) == 0 && st.type == FT_DIR;
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
static void rm_tree(const char *rel)
{
    struct file_stat st;
    if (vfs_stat(V, rel, &st))
        return;
    if (st.type == FT_DIR) {
        struct file *f;
        if (vfs_open(V, rel, FD_OPEN, FILE_READ_DATA, FO_DIRECTORY, false, &f) == 0) {
            struct fs_dirent *de = kmalloc(sizeof(*de));
            char names[32][64];
            int n = 0;
            while (n < 32 && file_readdir(f, (uint32_t)n, de) == 0) {
                scopy(names[n], de->name);
                n++;
            }
            kfree(de);
            obj_unref(&f->o);
            for (int i = 0; i < n; i++) {
                char child[200];
                size_t l = strlen(rel);
                memcpy(child, rel, l);
                child[l] = '/';
                scopy(child + l + 1, names[i]);
                rm_tree(child);
            }
        }
    }
    vfs_remove(V, rel);
}
static void fresh_volume(void)
{
    rm_tree("out");
    rm_tree("out2");
    rm_tree("crt.txt");
    rm_tree("crt2.txt");
    rm_tree("tm.txt");
    rm_tree("tm.bin");
    rm_tree("bin");
    vfs_mkdir(V, "out2");
}

static const uint8_t *g_pristine;
static size_t g_psize;
static uint8_t *g_work;
static unsigned g_mut_fail;

static uint32_t rd32(const uint8_t *b, size_t o) { uint32_t v; memcpy(&v, b + o, 4); return v; }
static uint64_t rd64(const uint8_t *b, size_t o) { uint64_t v; memcpy(&v, b + o, 8); return v; }
static void wr(uint8_t *b, size_t off, uint64_t v, int len) { memcpy(b + off, &v, (size_t)len); }

static int mut(const char *name, size_t off, uint64_t val, int len)
{
    memcpy(g_work, g_pristine, g_psize);
    wr(g_work, off, val, len);
    int rc = win32_pe_probe(g_work, g_psize);
    if (rc >= 0) {
        g_mut_fail++;
        klog(LOG_FAIL, "win32test: mutation '%s' was accepted", name);
    }
    check_(rc < 0, name, __LINE__);
    return rc;
}
static int mut_trunc(const char *name, size_t n)
{
    int rc = win32_pe_probe(g_pristine, n);
    check_(rc < 0, name, __LINE__);
    return rc;
}
static size_t rva2off(const uint8_t *b, uint32_t rva)
{
    uint32_t lf = rd32(b, 0x3C);
    unsigned nsec = b[lf + 6] | (b[lf + 7] << 8);
    size_t secoff = lf + 24 + (b[lf + 20] | (b[lf + 21] << 8));
    for (unsigned i = 0; i < nsec; i++) {
        uint32_t va = rd32(b, secoff + 40 * i + 12), raw = rd32(b, secoff + 40 * i + 20), rs = rd32(b, secoff + 40 * i + 16);
        if (rva >= va && rva - va < rs)
            return raw + (rva - va);
    }
    return 0;
}

static void test_loader(void)
{
    size_t mx = LEN(w32min_start, w32min_end);
    if (LEN(w32crt_start, w32crt_end) > mx) mx = LEN(w32crt_start, w32crt_end);
    if (LEN(w32fix_start, w32fix_end) > mx) mx = LEN(w32fix_start, w32fix_end);
    g_work = kmalloc(mx + 4096);
    CHECK(g_work != NULL);
    if (!g_work)
        return;
    uint64_t fr0 = pmm_free_frames();
    unsigned ob0 = obj_live(), tk0 = task_live_count();

    CHECK(win32_pe_probe(w32min_start, LEN(w32min_start, w32min_end)) == 0);
    CHECK(win32_pe_probe(w32fix_start, LEN(w32fix_start, w32fix_end)) == 0);
    CHECK(win32_pe_probe(w32crt_start, LEN(w32crt_start, w32crt_end)) == 0);
    CHECK(win32_pe_probe(w32norel_start, LEN(w32norel_start, w32norel_end)) == -E_NOEXEC);
    CHECK(win32_pe_probe(lxmin_start, LEN(lxmin_start, lxmin_end)) == -E_NOEXEC);
    CHECK(linux_elf_probe(w32min_start, LEN(w32min_start, w32min_end)) < 0);

    g_pristine = w32min_start;
    g_psize = LEN(w32min_start, w32min_end);
    uint32_t lf = rd32(g_pristine, 0x3C);
    size_t co = lf + 4, op = lf + 24, so = op + (g_pristine[lf + 20] | (g_pristine[lf + 21] << 8));
    size_t dir = op + 112;
    #define M(name, off, val, len) mut(name, off, val, len)
    mut_trunc("empty", 0);
    mut_trunc("1 byte", 1);
    mut_trunc("63 bytes", 63);
    mut_trunc("cut inside the PE signature", lf + 3);
    mut_trunc("cut inside the COFF header", lf + 15);
    mut_trunc("cut inside the optional header", op + 50);
    mut_trunc("cut inside the section table", so + 50);
    mut_trunc("headers only", rd32(g_pristine, op + 60));
    {
        size_t end = 0;
        unsigned nsec = g_pristine[co + 2] | (g_pristine[co + 3] << 8);
        for (unsigned i = 0; i < nsec; i++) {
            size_t e = (size_t)rd32(g_pristine, so + 40 * i + 20) + rd32(g_pristine, so + 40 * i + 16);
            if (e > end) end = e;
        }
        mut_trunc("last section's data cut", end - 1);
    }
    M("MZ", 0, 0x4142, 2);
    M("e_lfanew = 0", 0x3C, 0, 4);
    M("e_lfanew past the end", 0x3C, g_psize - 1, 4);
    M("e_lfanew = 0xFFFFFFF0", 0x3C, 0xFFFFFFF0u, 4);
    M("PE signature", lf + 2, 1, 1);
    M("machine i386", co, 0x14C, 2);
    M("machine 0", co, 0, 2);
    M("no sections", co + 2, 0, 2);
    M("97 sections", co + 2, 97, 2);
    M("65535 sections", co + 2, 0xFFFF, 2);
    M("optional header size 0", co + 16, 0, 2);
    M("optional header too small", co + 16, 100, 2);
    M("optional header size 65535", co + 16, 0xFFFF, 2);
    M("not executable", co + 18, 0x0224, 2);
    M("is a DLL", co + 18, 0x2226, 2);
    M("PE32 magic", op, 0x10B, 2);
    M("ROM magic", op, 0x107, 2);
    M("17 data directories", op + 108, 17, 4);
    M("0xFFFFFFFF data directories", op + 108, 0xFFFFFFFFu, 4);
    M("SectionAlignment 0", op + 32, 0, 4);
    M("SectionAlignment 0x800", op + 32, 0x800, 4);
    M("SectionAlignment not a power of two", op + 32, 0x3000, 4);
    M("SectionAlignment 0x20000", op + 32, 0x20000, 4);
    M("FileAlignment 0", op + 36, 0, 4);
    M("FileAlignment 100", op + 36, 100, 4);
    M("FileAlignment > SectionAlignment", op + 36, 0x2000, 4);
    M("SizeOfImage 0", op + 56, 0, 4);
    M("SizeOfImage not aligned", op + 56, 0x4001, 4);
    M("SizeOfImage 512 MiB", op + 56, 0x20000000, 4);
    M("SizeOfImage smaller than the sections", op + 56, 0x1000, 4);
    M("SizeOfHeaders 0", op + 60, 0, 4);
    M("SizeOfHeaders > SizeOfImage", op + 60, 0x100000, 4);
    M("SizeOfHeaders < the section table", op + 60, 0x50, 4);
    M("subsystem native", op + 68, 1, 2);
    M("subsystem 0", op + 68, 0, 2);
    M("subsystem EFI", op + 68, 9, 2);
    M("entry 0", op + 16, 0, 4);
    M("entry past the image", op + 16, 0x10000000, 4);
    M("entry in a non-executable section", op + 16, rd32(g_pristine, so + 40 + 12), 4);
    M("first section overlaps the headers", so + 12, 0, 4);
    M("first section unaligned", so + 12, 0x1001, 4);
    M("sections out of order", so + 80 + 12, 0x1000, 4);
    M("section overlaps the next", so + 8, 0x2000, 4);
    M("section VirtualSize 256 MiB", so + 8, 0x10000000, 4);
    M("section data beyond the file", so + 20, 0x7FFFFFFF, 4);
    M("section data size beyond the file", so + 16, 0x7FFFFFFF, 4);
    M("import directory outside the image", dir + 8, 0x10000000, 4);
    M("import directory in a gap", dir + 8, 0x3800, 4);
    M("import directory unaligned garbage", dir + 8, 0x1001, 4);

    {
        uint8_t *w = g_work;
        memcpy(w, g_pristine, g_psize);
        size_t at = 0;
        for (size_t i = 0; i + 12 < g_psize; i++)
            if (!memcmp(w + i, "KERNEL32.dll", 13)) { at = i; break; }
        CHECK(at != 0);
        if (at) {
            CHECK(win32_pe_probe(w, g_psize) == 0);
            memcpy(w + at, "kernel32.dll", 13);
            CHECK(win32_pe_probe(w, g_psize) == 0);
            memcpy(w, g_pristine, g_psize);
            w[at] = 'x';
            int nrc = win32_pe_probe(w, g_psize);
            if (nrc != -E_NOENT) klog(LOG_FAIL, "win32test: unknown DLL gave %d", nrc);
            CHECK(nrc == -E_NOENT);
            memcpy(w, g_pristine, g_psize);
            memcpy(w + at, "user32.dll\0\0", 12);
            CHECK(win32_pe_probe(w, g_psize) == 0);
        }
        size_t fn = 0;
        for (size_t i = 0; i + 12 < g_psize; i++)
            if (!memcmp(w + i, "ExitProcess", 12)) { fn = i; break; }
        memcpy(w, g_pristine, g_psize);
        CHECK(fn != 0);
        if (fn) {
            w[fn + 10] = 'z';
            CHECK(win32_pe_probe(w, g_psize) == 0);
        }
    }

    g_pristine = w32crt_start;
    g_psize = LEN(w32crt_start, w32crt_end);
    lf = rd32(g_pristine, 0x3C);
    co = lf + 4; op = lf + 24;
    so = op + (g_pristine[lf + 20] | (g_pristine[lf + 21] << 8));
    dir = op + 112;
    uint32_t reloc_rva = rd32(g_pristine, dir + 5 * 8), reloc_size = rd32(g_pristine, dir + 5 * 8 + 4);
    size_t ro = rva2off(g_pristine, reloc_rva);
    uint32_t tls_rva = rd32(g_pristine, dir + 9 * 8);
    size_t to = rva2off(g_pristine, tls_rva);
    CHECK(reloc_size > 16 && ro != 0 && to != 0);
    if (ro && to) {
        M("reloc block size 0", ro + 4, 0, 4);
        M("reloc block size 7", ro + 4, 7, 4);
        M("reloc block size odd", ro + 4, 9, 4);
        M("reloc block larger than the directory", ro + 4, reloc_size + 8, 4);
        M("reloc block page beyond the image", ro, 0x10000000, 4);
        M("reloc directory too large", dir + 5 * 8 + 4, 0x2000000, 4);
        M("reloc directory beyond the file", dir + 5 * 8, 0x10000000, 4);
        M("reloc type HIGH", ro + 8 + 1, ((uint64_t)(rd32(g_pristine, ro + 8) & 0xFFF) | 0x1000) >> 8, 1);
        for (int ty = 1; ty <= 9; ty++) {
            if (ty == 3) continue;
            memcpy(g_work, g_pristine, g_psize);
            uint16_t e;
            memcpy(&e, g_work + ro + 8, 2);
            e = (uint16_t)((ty << 12) | (e & 0xFFF));
            memcpy(g_work + ro + 8, &e, 2);
            check_(win32_pe_probe(g_work, g_psize) < 0, "reloc type not supported", __LINE__);
        }
        M("reloc target in the discarded .reloc section", ro, rd32(g_pristine, dir + 5 * 8), 4);
        M("TLS directory outside the image", dir + 9 * 8, 0x10000000, 4);
        M("TLS start > end", to, rd64(g_pristine, to + 8) + 16, 8);
        M("TLS end beyond the image", to + 8, 0x7FFFFFFFFFFFFFFFull, 8);
        M("TLS index outside the image", to + 16, 0x1000, 8);
        M("TLS callbacks outside the image", to + 24, 1, 8);
        M("TLS zero fill 256 MiB", to + 32, 0x10000000, 4);
        M("TLS raw data 64 MiB", to + 8, rd64(g_pristine, to) + 0x4000000, 8);
        uint64_t cbtab = rd64(g_pristine, to + 24);
        uint64_t imagebase = rd64(g_pristine, op + 24);
        size_t cbo = rva2off(g_pristine, (uint32_t)(cbtab - imagebase));
        CHECK(cbo != 0);
        if (cbo)
            M("TLS callback not in executable code", cbo, imagebase + 0x40, 8);
    }
    memcpy(g_work, g_pristine, g_psize);
    wr(g_work, co + 18, rd32(g_pristine, co + 18) | 1, 2);
    CHECK(win32_pe_probe(g_work, g_psize) == -E_NOEXEC);

    g_pristine = w32fix_start;
    g_psize = LEN(w32fix_start, w32fix_end);
    lf = rd32(g_pristine, 0x3C);
    op = lf + 24;
    M("fixed-base image on top of the runtime, no relocations", op + 24, 0x8000010000ull, 8);
    M("fixed-base image over the stack", op + 24, 0xFFFFFF0000ull, 8);
    #undef M

    CHECK(g_mut_fail == 0);
    kfree(g_work);
    g_work = NULL;
    CHECK(pmm_free_frames() == fr0);
    CHECK(obj_live() == ob0 && task_live_count() == tk0);
    klog(LOG_INFO, "win32test: loader refused every malformed PE and leaked nothing");
}

static void test_api(void)
{
    uint64_t fr0 = pmm_free_frames();
    fresh_volume();

    CHECK(run_min("base", NULL, false) == 0x80004);
    CHECK(run_min("exit", "77", false) == 77);
    CHECK(run_min("exit", "0", false) == 0);

    int rc = run_min("api", NULL, true);
    if (rc != 0)
        klog(LOG_FAIL, "w32min api: %d failure(s)\n%s", rc, cap);
    CHECK(rc == 0);
    CHECK(has("console-ok\n") && has("wide \xE2\x82\xAC\n"));
    CHECK(has("msvcrt printf: 7 ok 0.5\nmsvcrt puts\n"));
    CHECK(has("w32min: debug string"));
    CHECK(has("unimplemented import called: GetComputerNameA"));
    CHECK(!has("FAIL"));

    CHECK(file_size("data/in.txt") == 22);
    CHECK(is_dir("out") && is_dir("out/sub"));
    CHECK(file_size("out/new.txt") == 0 && file_size("out/x1.dat") == 0 && file_size("out/x22.dat") == 0);
    CHECK(file_size("out/big.bin") == 300 * 1024 + 123);
    CHECK(file_size("out/A Long File Name, with spaces.and.dots.txt") == 0);
    CHECK(file_size("out/a.txt") < 0 && file_size("out/b.txt") < 0);
    CHECK(dir_count("out") == 6);

    fresh_volume();
    const char *argv[] = { "w32fix", "base", NULL };
    CHECK(run_img(w32fix_start, LEN(w32fix_start, w32fix_end), argv, false) == 0x80008);
    const char *argv2[] = { "w32fix", "api", NULL };
    rc = run_img(w32fix_start, LEN(w32fix_start, w32fix_end), argv2, true);
    if (rc != 0)
        klog(LOG_FAIL, "w32fix api: %d failure(s)\n%s", rc, cap);
    CHECK(rc == 0);
    {
        int err = 0;
        const char *a3[] = { "w32norel", NULL };
        CHECK(win32_task_new("w32norel", w32norel_start, LEN(w32norel_start, w32norel_end), a3, ENV, NULL, NULL, &err) == NULL &&
              err == -E_NOEXEC);
    }

    const char *a4[] = { "w32min", "args", "with space", "last", NULL };
    CHECK(run_img(w32min_start, LEN(w32min_start, w32min_end), a4, false) == 0);
    const char *a5[] = { "w32min", "exit", "5", NULL };
    CHECK(run_img(w32min_start, LEN(w32min_start, w32min_end), a5, false) == 5);

    CHECK(pmm_free_frames() >= fr0 - 4);
    klog(LOG_INFO, "win32test: w32min ran relocated and in place, ~400 Win32 checks each");
}

static void test_crt(void)
{
    fresh_volume();
    const char *argv[] = { "w32crt", NULL };
    int rc = run_img(w32crt_start, LEN(w32crt_start, w32crt_end), argv, true);
    if (rc != 0)
        klog(LOG_FAIL, "w32crt: %d failure(s)\n%s", rc, cap);
    CHECK(rc == 0);
    CHECK(has("hello 1 w32crt\n"));
    CHECK(has("pi=3.141593 sqrt2=1.414214\n"));
    CHECK(has("fputs line\np\n"));
    CHECK(has("to stderr 1\n"));
    CHECK(has("w32crt done: 0 failure(s)\natexit 2\natexit 1\n"));
    CHECK(file_size("crt.txt") < 0);

    const char *a2[] = { "w32crt", "exit", "9", NULL };
    CHECK(run_img(w32crt_start, LEN(w32crt_start, w32crt_end), a2, true) == 9 && !has("atexit"));
    const char *a3[] = { "w32crt", "atexit", NULL };
    CHECK(run_img(w32crt_start, LEN(w32crt_start, w32crt_end), a3, true) == 3 && has("main done\natexit 3\natexit 2\natexit 1\n"));
    const char *a4[] = { "w32crt", "abort", NULL };
    CHECK(run_img(w32crt_start, LEN(w32crt_start, w32crt_end), a4, true) == 3 && has("abort() called"));
    klog(LOG_INFO, "win32test: an unmodified MinGW program runs (CRT startup, TLS directory, atexit, stdio, math)");
}

static void test_from_file(void)
{
    fresh_volume();
    CHECK(vfs_mkdir(V, "bin") == 0);
    struct file *f;
    CHECK(vfs_open(V, "bin/w32min.exe", FD_CREATE, FILE_WRITE_DATA, 0, true, &f) == 0);
    if (f) {
        size_t n = LEN(w32min_start, w32min_end);
        CHECK(file_write(f, w32min_start, n) == (long)n);
        obj_unref(&f->o);
    }
    CHECK(put_file("bin/notpe.txt", "just some text, definitely not an executable........................................", 80));
    CHECK(put_file("bin/empty.exe", "", 0));
    CHECK(vfs_open(V, "bin/lxmin", FD_CREATE, FILE_WRITE_DATA, 0, true, &f) == 0);
    if (f) { file_write(f, lxmin_start, LEN(lxmin_start, lxmin_end)); obj_unref(&f->o); }

    int err = 0;
    const char *argv[] = { "w32min", "modname", NULL };
    struct task *t = win32_task_new_from_file("\\Volumes\\w32\\bin\\w32min.exe", argv, ENV, "\\Volumes\\w32", &err);
    CHECK(t != NULL && err == 0);
    if (t) {
        task_start(t);
        int rc = task_wait(t);
        CHECK(rc == 0);
    }
    t = win32_task_new_from_file("\\Volumes\\w32\\bin\\w32min.exe", (const char *[]){ "w32min", "exit", "4", NULL }, ENV, NULL, &err);
    CHECK(t != NULL);
    if (t) { task_start(t); CHECK(task_wait(t) == 4); }

    CHECK(win32_task_new_from_file("\\Volumes\\w32\\bin\\nope", argv, ENV, NULL, &err) == NULL && err == -E_NOENT);
    CHECK(win32_task_new_from_file("\\Volumes\\w32\\bin\\notpe.txt", argv, ENV, NULL, &err) == NULL && err == -E_NOEXEC);
    CHECK(win32_task_new_from_file("\\Volumes\\w32\\bin\\empty.exe", argv, ENV, NULL, &err) == NULL && err != 0);
    CHECK(win32_task_new_from_file("\\Volumes\\w32\\bin\\lxmin", argv, ENV, NULL, &err) == NULL && err == -E_NOEXEC);
    CHECK(win32_task_new_from_file("\\Volumes\\w32\\bin", argv, ENV, NULL, &err) == NULL && err != 0);
    CHECK(win32_task_new_from_file("\\Volumes\\nodisk\\X", argv, ENV, NULL, &err) == NULL && err == -E_NOENT);
    CHECK(win32_task_new_from_file("\\elsewhere", argv, ENV, NULL, &err) == NULL && err == -E_NOENT);

    static char huge[40000];
    memset(huge, 'a', sizeof(huge) - 1);
    const char *big[] = { "w32min", huge, NULL };
    CHECK(win32_task_new("big", w32min_start, LEN(w32min_start, w32min_end), big, ENV, NULL, NULL, &err) == NULL && err == -E_2BIG);

    uint64_t fr = pmm_free_frames();
    t = linux_task_new("lx", w32min_start, LEN(w32min_start, w32min_end), argv, ENV, &err);
    CHECK(t == NULL && err != 0);
    CHECK(pmm_free_frames() == fr);
    const char *lxa[] = { "lx", "spin_fpu", "1", NULL };
    t = linux_task_new("lx", lxmin_start, LEN(lxmin_start, lxmin_end), lxa, ENV, &err);
    CHECK(t != NULL);
    if (t) {
        struct vm_info vi;
        CHECK(wvm_alloc(t, 0, 4096, VM_RESERVE | VM_COMMIT | VM_PROT_R) == -E_NOSYS);
        CHECK(sys_vm_dispatch(t, SYS_VM_QUERY, 0x1000, (uint64_t)&vi, 0) == -E_NOSYS);
        task_start(t);
        CHECK(task_wait(t) == 0);
    }
    CHECK(vfs_remove(V, "bin/w32min.exe") == 0 && vfs_remove(V, "bin/notpe.txt") == 0 &&
          vfs_remove(V, "bin/empty.exe") == 0 && vfs_remove(V, "bin/lxmin") == 0 && vfs_remove(V, "bin") == 0);
    klog(LOG_INFO, "win32test: executing a PE from a VFS path; the personalities refuse each other's binaries");
}

static void test_faults(void)
{
    uint64_t fr0 = pmm_free_frames();
    CHECK(run_min("fault-null", NULL, false) == FAULT(14));
    CHECK(run_min("fault-write-ro", NULL, false) == FAULT(14));
    CHECK(run_min("fault-write-text", NULL, false) == FAULT(14));
    CHECK(run_min("fault-exec-data", NULL, false) == FAULT(14));
    CHECK(run_min("fault-exec-heap", NULL, false) == FAULT(14));
    CHECK(run_min("fault-ud", NULL, false) == FAULT(6));
    CHECK(run_min("fault-div", NULL, false) == FAULT(0));
    CHECK(run_min("fault-priv", NULL, false) == FAULT(13));
    CHECK(run_min("fault-hlt", NULL, false) == FAULT(13));
    CHECK(run_min("fault-stack", NULL, false) == FAULT(14));
    CHECK(run_min("fault-kernel", NULL, false) == FAULT(14));
    CHECK(run_min("fault-reserved", NULL, false) == FAULT(14));
    CHECK(run_min("fault-noaccess", NULL, false) == FAULT(14));
    CHECK(run_min("fault-readonly", NULL, false) == FAULT(14));
    CHECK(run_min("fault-decommitted", NULL, false) == FAULT(14));
    CHECK(run_min("fault-released", NULL, false) == FAULT(14));
    CHECK(run_min("fault-syscall", NULL, false) == 0);
    CHECK(run_min("no-such-mode", NULL, false) == 0x7FFF);
    CHECK(pmm_free_frames() >= fr0 - 4);
    klog(LOG_INFO, "win32test: bad accesses kill the task, not the kernel; reserved / released / decommitted memory faults");
}

static void test_concurrent(void)
{
    struct task *t[5];
    const char *argv[4][4];
    const char *seeds[] = { "1", "2", "3", "4" };
    int err;
    for (int i = 0; i < 4; i++) {
        argv[i][0] = "spin"; argv[i][1] = "fpu"; argv[i][2] = seeds[i]; argv[i][3] = NULL;
        t[i] = win32_task_new("spin", w32min_start, LEN(w32min_start, w32min_end), argv[i], ENV, NULL, NULL, &err);
        CHECK(t[i] != NULL);
    }
    const char *lxargv[] = { "spin", "spin_fpu", "9", NULL };
    t[4] = linux_task_new("lxspin", lxmin_start, LEN(lxmin_start, lxmin_end), lxargv, ENV, &err);
    CHECK(t[4] != NULL);
    for (int i = 0; i < 5; i++)
        if (t[i]) task_start(t[i]);
    for (int i = 0; i < 5; i++)
        if (t[i]) CHECK(task_wait(t[i]) == 0);
    struct task *n = task_create("native-fpu", user_tests_start, (size_t)(user_tests_end - user_tests_start), UT_FPU, 0);
    CHECK(n != NULL);
    if (n)
        CHECK(task_wait(n) == FAULT(6));
    klog(LOG_INFO, "win32test: SSE/x87 + GS (TEB) private per task under time slicing next to a Linux task; native tasks still get #UD");

    fresh_volume();
    const char *sa[3][4];
    const char *ids[] = { "1", "2", "3" };
    struct task *s[3];
    for (int i = 0; i < 3; i++) {
        sa[i][0] = "stress"; sa[i][1] = "stress"; sa[i][2] = ids[i]; sa[i][3] = NULL;
        s[i] = win32_task_new("stress", w32min_start, LEN(w32min_start, w32min_end), sa[i], ENV, "\\Volumes\\w32", NULL, &err);
        CHECK(s[i] != NULL);
    }
    for (int i = 0; i < 3; i++)
        if (s[i]) task_start(s[i]);
    for (int i = 0; i < 3; i++)
        if (s[i]) CHECK(task_wait(s[i]) == 0);
    CHECK(dir_count("out2") == 0);
    klog(LOG_INFO, "win32test: three processes hammering files and VM at once (syscalls into never-touched demand-zero buffers)");
}

static void scat(char *d, const char *s)
{
    d += strlen(d);
    while ((*d++ = *s++))
        ;
}

// C:\example\dir becomes /Volumes/<vol>/example/dir, this is what we do for a living now
static void test_alias(void)
{
    char c[VFS_VOL_NAME_MAX + 1], o[160], want[160];
    bool ch;
    CHECK(vfs_dos_volume('C', c) == 0 && c[0]);
    CHECK(vfs_dos_volume('c', o) == 0 && !strcmp(o, c));
    CHECK(vfs_dos_volume('A', o) == -E_NOENT && vfs_dos_volume('B', o) == -E_NOENT && vfs_dos_volume('1', o) == -E_NOENT);
    CHECK(vfs_dos_volume('Z', o) == -E_NOENT);
    int letter = 0;
    for (int l = 'C'; l <= 'Z'; l++)
        if (vfs_dos_volume(l, o) == 0 && !strcmp(o, "w32"))
            letter = l;
    CHECK(letter != 0);

    want[0] = 0;
    scat(want, "/Volumes/");
    scat(want, c);
    size_t wl = strlen(want);

    CHECK(vfs_alias_path("C:\\example\\dir", o, sizeof(o), &ch) == 0 && ch && !strcmp(o + wl, "/example/dir") && !memcmp(o, want, wl));
    CHECK(vfs_alias_path("c:/example/dir", o, sizeof(o), &ch) == 0 && ch && !strcmp(o + wl, "/example/dir"));
    CHECK(vfs_alias_path("C:\\", o, sizeof(o), &ch) == 0 && ch && !strcmp(o + wl, "/") && !memcmp(o, want, wl));
    CHECK(vfs_alias_path("C:", o, sizeof(o), &ch) == 0 && ch && !strcmp(o, want));
    CHECK(vfs_alias_path("\\a\\b", o, sizeof(o), &ch) == 0 && ch && !strcmp(o + wl, "/a/b"));
    CHECK(vfs_alias_path("C:\\a/b\\c", o, sizeof(o), &ch) == 0 && ch && !strcmp(o + wl, "/a/b/c"));
    CHECK(vfs_alias_path("Z:\\x", o, sizeof(o), &ch) == -E_NOENT && !ch);
    {
        char ww[32] = { 0 };
        ww[0] = (char)letter; ww[1] = 0; scat(ww, ":\\data\\in.txt");
        CHECK(vfs_alias_path(ww, o, sizeof(o), &ch) == 0 && ch && !strcmp(o, "/Volumes/w32/data/in.txt"));
    }
    CHECK(vfs_alias_path("/example/dir", o, sizeof(o), &ch) == 0 && !ch && !strcmp(o, "/example/dir"));
    CHECK(vfs_alias_path("relative\\x", o, sizeof(o), &ch) == 0 && !ch && !strcmp(o, "relative\\x"));
    CHECK(vfs_alias_path("\\\\?\\C:\\x", o, sizeof(o), &ch) == 0 && !ch);
    CHECK(vfs_alias_path("1:\\x", o, sizeof(o), &ch) == 0 && !ch);
    char tiny[8];
    CHECK(vfs_alias_path("C:\\example\\dir", tiny, sizeof(tiny), &ch) == -E_NAMETOOLONG);

    char nt[2 * 4096 + 4];
    char wnt[160];
    wnt[0] = 0;
    scat(wnt, "\\Volumes\\");
    scat(wnt, c);
    CHECK(linux_path_probe("\\", "C:\\example\\dir", nt, sizeof(nt)) == 0 && !strcmp(nt + 9 + strlen(c), "\\example\\dir") && !memcmp(nt, wnt, 9 + strlen(c)));
    CHECK(linux_path_probe("\\", "/example/dir", nt, sizeof(nt)) == 0 && !strcmp(nt + 9 + strlen(c), "\\example\\dir"));
    CHECK(linux_path_probe("\\", "\\example\\dir", nt, sizeof(nt)) == 0 && !strcmp(nt + 9 + strlen(c), "\\example\\dir"));
    CHECK(linux_path_probe("\\", "C:\\", nt, sizeof(nt)) == 0 && !strcmp(nt, wnt));
    CHECK(linux_path_probe("\\", "C:\\a\\..\\b\\.\\c", nt, sizeof(nt)) == 0 && !strcmp(nt + 9 + strlen(c), "\\b\\c"));
    CHECK(linux_path_probe("\\", "/Volumes/w32/data", nt, sizeof(nt)) == 0 && !strcmp(nt, "\\Volumes\\w32\\data"));
    CHECK(linux_path_probe("\\", "/Volumes", nt, sizeof(nt)) == 0 && !strcmp(nt, "\\Volumes"));
    CHECK(linux_path_probe("\\", "/", nt, sizeof(nt)) == 0 && !strcmp(nt, "\\"));
    CHECK(linux_path_probe("\\Volumes\\w32", "data/in.txt", nt, sizeof(nt)) == 0 && !strcmp(nt, "\\Volumes\\w32\\data\\in.txt"));
    CHECK(linux_path_probe("\\", "Z:\\x", nt, sizeof(nt)) == -E_NOENT);
    klog(LOG_INFO, "win32test: C:\\dir == /dir: drive letters and rooted paths map onto the first volume, same as the Win32 runtime");
}

unsigned win32test_run(void)
{
    n_checks = n_fail = 0;
    unsigned vn0 = vfs_live_vnodes(), ob0 = obj_live(), tk0 = task_live_count();
    uint64_t t0 = clock_ns();
    klog(LOG_INFO, "win32: %u runtime exports", win32_export_count());

    struct blkdev *rd = blk_ramdisk_create("w32ram", 150000);
    CHECK(rd != NULL);
    if (rd && fat32_format(rd, "W32TEST", 1) == 0 && vfs_mount("w32", rd, false, &V) == 0) {
        CHECK(vfs_mkdir(V, "data") == 0);
        CHECK(put_file("data/in.txt", "hello from the kernel\n", 22));
        test_loader();
        test_alias();
        uint64_t free0 = pmm_free_frames();
        test_api();
        test_crt();
        test_from_file();
        test_faults();
        test_concurrent();
        CHECK(pmm_free_frames() >= free0 - 4);
        CHECK(vfs_unmount("w32") == 0);
        obj_unref(&V->o);
        V = NULL;
    } else {
        CHECK(false);
    }
    if (rd)
        blk_unregister(rd);

    CHECK(vfs_live_vnodes() == vn0 && obj_live() == ob0 && task_live_count() == tk0);
    if (vfs_live_vnodes() != vn0 || obj_live() != ob0 || task_live_count() != tk0)
        klog(LOG_FAIL, "win32test: leak: vnodes %u->%u objects %u->%u tasks %u->%u", vn0, vfs_live_vnodes(), ob0,
             obj_live(), tk0, task_live_count());
    CHECK(heap_check());
    klog(n_fail ? LOG_FAIL : LOG_OK, "W32TEST %s (%u checks, %u failed, %lu ms)", n_fail ? "FAIL" : "PASS", n_checks, n_fail,
         (uint64_t)((clock_ns() - t0) / 1000000));
    console_set_color(n_fail ? C_LRED : C_LGREEN, C_BLACK);
    kprintf("\nStep 9 (PE loader + Win32 personality) %s.\n", n_fail ? "has FAILURES" : "complete");
    console_set_color(C_LGRAY, C_BLACK);
    return n_fail;
}
