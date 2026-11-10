#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "task.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"
#include "vfs.h"
#include "timer.h"

static unsigned n_checks, n_fail;
static void check_(bool ok, const char *expr, int line)
{
    n_checks++;
    if (!ok) {
        n_fail++;
        klog(LOG_FAIL, "fstest.c:%d: %s", line, expr);
    }
}
#define CHECK(c) check_((c), #c, __LINE__)
#define IMG_SIZE ((size_t)(user_tests_end - user_tests_start))

static struct volume *V;

static int open_p(const char *p, uint32_t disp, uint32_t acc, struct file **f)
{
    return vfs_open(V, p, disp, acc, 0, true, f);
}
static void close_f(struct file *f) { obj_unref(&f->o); }

static uint8_t pat(uint64_t i, uint32_t seed)
{
    uint64_t j = i + seed;
    return (uint8_t)(((j ^ (j >> 8) ^ (j >> 16)) * 167 + 13) & 0xFF);
}

static bool put_file(const char *p, const void *d, size_t n)
{
    struct file *f;
    if (open_p(p, FD_OVERWRITE_IF, FILE_READ_DATA | FILE_WRITE_DATA, &f))
        return false;
    bool ok = file_write(f, d, n) == (long)n;
    close_f(f);
    return ok;
}

static long get_file(const char *p, void *buf, size_t cap)
{
    struct file *f;
    int rc = open_p(p, FD_OPEN, FILE_READ_DATA, &f);
    if (rc)
        return rc;
    long n = file_read(f, buf, cap);
    close_f(f);
    return n;
}

static bool is_str(const char *p, const char *want)
{
    char buf[128];
    long n = get_file(p, buf, sizeof(buf));
    return n == (long)strlen(want) && !memcmp(buf, want, (size_t)n);
}

static bool write_pat(const char *p, uint64_t size, uint32_t seed, size_t chunk)
{
    struct file *f;
    uint8_t *b = kmalloc(chunk);
    if (!b || open_p(p, FD_OVERWRITE_IF, FILE_READ_DATA | FILE_WRITE_DATA, &f)) {
        kfree(b);
        return false;
    }
    bool ok = true;
    for (uint64_t off = 0; off < size && ok;) {
        size_t c = size - off < chunk ? (size_t)(size - off) : chunk;
        for (size_t i = 0; i < c; i++)
            b[i] = pat(off + i, seed);
        ok = file_write(f, b, c) == (long)c;
        off += c;
    }
    close_f(f);
    kfree(b);
    return ok;
}

static bool verify_pat(const char *p, uint64_t size, uint32_t seed, size_t chunk)
{
    struct file *f;
    uint8_t *b = kmalloc(chunk);
    if (!b || open_p(p, FD_OPEN, FILE_READ_DATA, &f)) {
        kfree(b);
        return false;
    }
    struct file_stat st;
    bool ok = !file_stat(f, &st) && st.size == size && st.type == FT_FILE;
    uint64_t off = 0;
    while (ok && off < size) {
        long n = file_read(f, b, chunk);
        if (n <= 0) {
            ok = false;
            break;
        }
        for (long i = 0; i < n && ok; i++)
            ok = b[i] == pat(off + (uint64_t)i, seed);
        off += (uint64_t)n;
    }
    ok = ok && file_read(f, b, chunk) == 0;
    close_f(f);
    kfree(b);
    return ok;
}

static bool is_zero(const char *p, uint64_t from, uint64_t to)
{
    struct file *f;
    if (open_p(p, FD_OPEN, FILE_READ_DATA, &f))
        return false;
    uint8_t b[512];
    bool ok = true;
    for (uint64_t off = from; off < to && ok; ) {
        size_t c = to - off < sizeof(b) ? (size_t)(to - off) : sizeof(b);
        ok = file_pread(f, off, b, c) == (long)c;
        for (size_t i = 0; i < c && ok; i++)
            ok = b[i] == 0;
        off += c;
    }
    close_f(f);
    return ok;
}

#define MAXN 200
static char names[MAXN][72];
static uint32_t types[MAXN];

static int list_dir(const char *p)
{
    struct file *f;
    int rc = open_p(p, FD_OPEN, FILE_READ_DATA, &f);
    if (rc)
        return rc;
    int n = 0;
    struct fs_dirent de;
    while (n < MAXN && (rc = file_readdir(f, (uint32_t)n, &de)) == 0) {
        size_t l = strlen(de.name);
        if (l > 71)
            l = 71;
        memcpy(names[n], de.name, l);
        names[n][l] = 0;
        types[n] = de.type;
        n++;
    }
    close_f(f);
    return rc == -E_NOENT ? n : rc;
}

static bool listed(int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], name))
            return true;
    return false;
}

static uint64_t free_bytes(void)
{
    struct fs_statfs sf;
    return vfs_statfs(V, &sf) ? 0 : sf.free_bytes;
}

static struct volume *mount_dev(const char *name, struct blkdev *d, bool ro)
{
    struct volume *v = NULL;
    int rc = vfs_mount(name, d, ro, &v);
    if (rc)
        klog(LOG_FAIL, "fstest: mount %s failed: %d", name, rc);
    return rc ? NULL : v;
}

static void test_blk(void)
{
    struct blkdev *d = blk_ramdisk_create("rtest", 1000);
    CHECK(d != NULL);
    if (!d)
        return;
    uint8_t a[1024], b[1024];
    for (int i = 0; i < 1024; i++)
        a[i] = (uint8_t)(i * 7);
    CHECK(blk_read(d, 5, 2, b) == 0);
    bool zero = true;
    for (int i = 0; i < 1024; i++)
        zero &= b[i] == 0;
    CHECK(zero);
    CHECK(blk_ramdisk_chunks(d) == 0);
    CHECK(blk_write(d, 7, 2, a) == 0 && blk_read(d, 7, 2, b) == 0 && !memcmp(a, b, 1024));
    CHECK(blk_ramdisk_chunks(d) == 2);
    CHECK(blk_write(d, 8, 2, a) == 0);
    CHECK(blk_ramdisk_chunks(d) == 2);
    CHECK(blk_read(d, 999, 1, b) == 0);
    CHECK(blk_read(d, 1000, 1, b) == -E_INVAL);
    CHECK(blk_read(d, 999, 2, b) == -E_INVAL);
    CHECK(blk_write(d, 1000, 1, a) == -E_INVAL);
    CHECK(blk_read(d, 0, 0, b) == 0);
    CHECK(blk_find("rtest") == d && blk_find("nope") == NULL);

    struct blkdev *ro = blk_partition_create(d, "rtest-p", 100, 50);
    CHECK(ro != NULL);
    if (ro) {
        CHECK(blk_write(ro, 3, 1, a) == 0 && blk_read(d, 103, 1, b) == 0 && !memcmp(a, b, 512));
        CHECK(blk_read(ro, 50, 1, b) == -E_INVAL);
        blk_unregister(ro);
    }
    CHECK(blk_partition_create(d, "bad", 990, 20) == NULL);
    d->ro = true;
    CHECK(blk_write(d, 1, 1, a) == -E_ROFS);
    blk_unregister(d);
    CHECK(blk_find("rtest") == NULL);
    klog(LOG_INFO, "fstest: block layer: ramdisk (sparse), bounds, read-only, partitions ok");
}

static void test_core_ss(const char *name, uint64_t sectors, uint32_t spc, bool fill_test, uint32_t dss, uint32_t fss)
{
    unsigned vn0 = vfs_live_vnodes(), ob0 = obj_live();
    struct blkdev *rd = blk_ramdisk_create_ss(name, sectors, dss);
    CHECK(rd != NULL);
    if (!rd)
        return;
    CHECK(blk_ssize(rd) == dss);
    CHECK(fat32_format_ss(rd, "coretest", spc, fss) == 0);
    {
        uint8_t bpb[2];
        CHECK(blk_read_part(rd, 0, 11, 2, bpb) == 0 && (bpb[0] | bpb[1] << 8) == (int)fss);
    }
    V = mount_dev(name, rd, false);
    CHECK(V != NULL);
    if (!V) {
        blk_unregister(rd);
        return;
    }
    struct fs_statfs sf;
    uint32_t cb;
    CHECK(vfs_statfs(V, &sf) == 0);
    cb = sf.cluster_bytes;
    CHECK(cb == (spc ? spc : 1) * fss);
    CHECK(!strcmp(sf.fs, "FAT32") && !strcmp(sf.label, "CORETEST"));
    CHECK(sf.total_bytes > 30 * MiB && sf.free_bytes < sf.total_bytes && sf.free_bytes > sf.total_bytes - 4 * cb);
    uint64_t free0 = sf.free_bytes;
    CHECK(list_dir("") == 0);

    struct file *f;
    CHECK(open_p("hello.txt", FD_OPEN, FILE_READ_DATA, &f) == -E_NOENT);
    CHECK(open_p("hello.txt", FD_CREATE, FILE_READ_DATA | FILE_WRITE_DATA, &f) == 0);
    struct file_stat st;
    CHECK(file_stat(f, &st) == 0 && st.size == 0 && st.type == FT_FILE && st.mtime > 1700000000);
    CHECK(file_write(f, "hello", 5) == 5 && file_write(f, " world", 6) == 6);
    CHECK(file_stat(f, &st) == 0 && st.size == 11);
    CHECK(file_seek(f, 0, SEEK_SET) == 0);
    char buf[64];
    CHECK(file_read(f, buf, 64) == 11 && !memcmp(buf, "hello world", 11));
    CHECK(file_read(f, buf, 64) == 0);
    CHECK(file_pread(f, 6, buf, 64) == 5 && !memcmp(buf, "world", 5));
    CHECK(file_pread(f, 99, buf, 4) == 0);
    CHECK(file_seek(f, -5, SEEK_END) == 6 && file_read(f, buf, 2) == 2 && !memcmp(buf, "wo", 2));
    CHECK(file_seek(f, 1, SEEK_CUR) == 9);
    CHECK(file_seek(f, -100, SEEK_CUR) == -E_INVAL && file_seek(f, 0, 77) == -E_INVAL);
    close_f(f);
    CHECK(open_p("hello.txt", FD_CREATE, FILE_READ_DATA, &f) == -E_EXIST);
    CHECK(open_p("HELLO.TXT", FD_OPEN, FILE_READ_DATA, &f) == 0);
    CHECK(file_write(f, "x", 1) == -E_ACCES);
    close_f(f);
    CHECK(open_p("hello.txt", FD_OPEN_IF, FILE_READ_DATA, &f) == 0);
    close_f(f);
    int n = list_dir("");
    CHECK(n == 1 && !strcmp(names[0], "hello.txt") && types[0] == FT_FILE);

    CHECK(write_pat("exact.bin", 3 * cb, 11, 1000) && verify_pat("exact.bin", 3 * cb, 11, 777));
    CHECK(write_pat("one.bin", 1, 12, 1) && verify_pat("one.bin", 1, 12, 64));
    CHECK(write_pat("edge.bin", cb + 1, 13, cb) && verify_pat("edge.bin", cb + 1, 13, cb - 1));
    CHECK(write_pat("big.bin", 300 * KiB + 77, 14, 3333) && verify_pat("big.bin", 300 * KiB + 77, 14, 8192));
    CHECK(verify_pat("exact.bin", 3 * cb, 11, 4096));
    CHECK(free_bytes() < free0);

    CHECK(open_p("gap.bin", FD_CREATE, FILE_READ_DATA | FILE_WRITE_DATA, &f) == 0);
    CHECK(file_pwrite(f, 3 * cb + 10, "X", 1) == 1);
    CHECK(file_stat(f, &st) == 0 && st.size == 3 * cb + 11);
    close_f(f);
    CHECK(is_zero("gap.bin", 0, 3 * cb + 10));
    CHECK(write_pat("stale.bin", 2 * cb, 15, 512));
    CHECK(open_p("stale.bin", FD_OPEN, FILE_READ_DATA | FILE_WRITE_DATA, &f) == 0);
    CHECK(file_setsize(f, cb + 5) == 0);
    CHECK(file_setsize(f, 2 * cb + 100) == 0);
    close_f(f);
    CHECK(is_zero("stale.bin", cb + 5, 2 * cb + 100));
    {
        uint8_t b[16];
        long got = get_file("stale.bin", b, 16);
        CHECK(got == 16 && b[0] == pat(0, 15) && b[15] == pat(15, 15));
    }

    uint64_t before = free_bytes();
    CHECK(write_pat("tmp.bin", 10 * cb, 16, 4096));
    CHECK(free_bytes() == before - 10 * cb);
    CHECK(open_p("tmp.bin", FD_OPEN, FILE_WRITE_DATA, &f) == 0);
    CHECK(file_setsize(f, 4 * cb) == 0 && free_bytes() == before - 4 * cb);
    CHECK(file_setsize(f, 0) == 0 && free_bytes() == before);
    close_f(f);
    CHECK(vfs_remove(V, "tmp.bin") == 0 && free_bytes() == before);
    CHECK(write_pat("tmp.bin", 5 * cb, 17, 4096) && vfs_remove(V, "tmp.bin") == 0 && free_bytes() == before);
    CHECK(open_p("tmp.bin", FD_OPEN, FILE_READ_DATA, &f) == -E_NOENT);

    CHECK(vfs_mkdir(V, "Docs") == 0 && vfs_mkdir(V, "Docs\\Sub") == 0);
    CHECK(vfs_mkdir(V, "Docs") == -E_EXIST && vfs_mkdir(V, "") == -E_EXIST);
    CHECK(vfs_mkdir(V, "Nope\\X") == -E_NOENT);
    CHECK(put_file("Docs\\Sub\\deep.txt", "deep", 4));
    CHECK(is_str("DOCS\\sub\\DEEP.TXT", "deep") && is_str("docs/sub/deep.txt", "deep"));
    CHECK(vfs_remove(V, "Docs") == -E_NOTEMPTY && vfs_remove(V, "Docs\\Sub") == -E_NOTEMPTY);
    CHECK(open_p("hello.txt\\x", FD_OPEN, FILE_READ_DATA, &f) == -E_NOTDIR);
    CHECK(open_p("Docs\\nope\\x", FD_OPEN, FILE_READ_DATA, &f) == -E_NOENT);
    CHECK(open_p("Docs\\..\\hello.txt", FD_OPEN, FILE_READ_DATA, &f) == -E_INVAL);
    CHECK(open_p("Docs\\Sub", FD_OPEN, FILE_READ_DATA | FILE_WRITE_DATA, &f) == -E_ISDIR);
    CHECK(open_p("Docs\\Sub", FD_OPEN | 0, FILE_READ_DATA, &f) == 0);
    CHECK(file_read(f, buf, 4) == -E_ISDIR && file_write(f, buf, 4) == -E_ACCES);
    struct fs_dirent de;
    CHECK(file_readdir(f, 0, &de) == 0 && !strcmp(de.name, "deep.txt") && de.size == 4);
    CHECK(file_readdir(f, 1, &de) == -E_NOENT);
    close_f(f);
    CHECK(vfs_open(V, "hello.txt", FD_OPEN, FILE_READ_DATA, FO_DIRECTORY, true, &f) == -E_NOTDIR);
    CHECK(vfs_stat(V, "Docs\\Sub", &st) == 0 && st.type == FT_DIR && st.size == 0);
    CHECK(vfs_stat(V, "", &st) == 0 && st.type == FT_DIR);

    CHECK(put_file("Long Name One.txt", "1", 1) && put_file("Long Name Two.txt", "2", 1) &&
          put_file("Long Name Three.txt", "3", 1));
    CHECK(is_str("long name two.TXT", "2"));
    CHECK(is_str("LONGNA~1.TXT", "1") && is_str("LONGNA~2.TXT", "2") && is_str("LONGNA~3.TXT", "3"));
    CHECK(put_file("Readme", "r", 1) && is_str("README", "r"));
    n = list_dir("");
    CHECK(listed(n, "Long Name One.txt") && listed(n, "Long Name Three.txt") && listed(n, "Readme"));
    CHECK(put_file("na\xC3\xAFve caf\xC3\xA9.txt", "u", 1) && is_str("NA\xC3\x8FVE CAF\xC3\x89.TXT", "u"));
    CHECK(put_file("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt", "j", 1));
    CHECK(put_file("emoji \xF0\x9F\x98\x80.txt", "e", 1) && is_str("emoji \xF0\x9F\x98\x80.txt", "e"));
    n = list_dir("");
    CHECK(listed(n, "na\xC3\xAFve caf\xC3\xA9.txt") && listed(n, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt") &&
          listed(n, "emoji \xF0\x9F\x98\x80.txt"));
    CHECK(open_p("\xC3\x89" "COLE.txt", FD_CREATE, FILE_WRITE_DATA, &f) == 0);
    close_f(f);
    CHECK(open_p("\xC3\xA9" "cole.TXT", FD_CREATE, FILE_WRITE_DATA, &f) == -E_EXIST);
    CHECK(open_p("a:b.txt", FD_CREATE, FILE_WRITE_DATA, &f) == -E_INVAL);
    CHECK(open_p("q?.txt", FD_CREATE, FILE_WRITE_DATA, &f) == -E_INVAL);
    CHECK(open_p("trail.", FD_CREATE, FILE_WRITE_DATA, &f) == -E_INVAL);
    CHECK(open_p("trail ", FD_CREATE, FILE_WRITE_DATA, &f) == -E_INVAL);
    CHECK(open_p("tab\there", FD_CREATE, FILE_WRITE_DATA, &f) == -E_INVAL);
    CHECK(open_p("bad\xC3(utf8", FD_CREATE, FILE_WRITE_DATA, &f) == -E_INVAL);
    {
        char longn[300];
        memset(longn, 'x', 256);
        longn[256] = 0;
        CHECK(open_p(longn, FD_CREATE, FILE_WRITE_DATA, &f) == -E_NAMETOOLONG);
        longn[255] = 0;
        CHECK(open_p(longn, FD_CREATE, FILE_WRITE_DATA, &f) == 0);
        close_f(f);
        CHECK(vfs_stat(V, longn, &st) == 0 && vfs_remove(V, longn) == 0);
    }

    CHECK(vfs_mkdir(V, "Many") == 0);
    char nm[96];
    bool ok = true;
    for (int i = 0; i < 120 && ok; i++) {
        nm[0] = 0;
        char *p = nm;
        for (const char *s = "Many\\file-number-"; *s; s++) *p++ = *s;
        *p++ = (char)('0' + i / 100); *p++ = (char)('0' + i / 10 % 10); *p++ = (char)('0' + i % 10);
        for (const char *s = "-with-a-long-name.dat"; *s; s++) *p++ = *s;
        *p = 0;
        ok = put_file(nm, &i, sizeof(i));
    }
    CHECK(ok);
    n = list_dir("Many");
    CHECK(n == 120);
    CHECK(listed(n, "file-number-000-with-a-long-name.dat") && listed(n, "file-number-119-with-a-long-name.dat"));
    for (int i = 0; i < 120; i += 2) {
        nm[0] = 0;
        char *p = nm;
        for (const char *s = "Many\\file-number-"; *s; s++) *p++ = *s;
        *p++ = (char)('0' + i / 100); *p++ = (char)('0' + i / 10 % 10); *p++ = (char)('0' + i % 10);
        for (const char *s = "-with-a-long-name.dat"; *s; s++) *p++ = *s;
        *p = 0;
        ok &= vfs_remove(V, nm) == 0;
    }
    CHECK(ok && list_dir("Many") == 60);
    for (int i = 0; i < 60; i++) {
        nm[0] = 0;
        char *p = nm;
        for (const char *s = "Many\\again-"; *s; s++) *p++ = *s;
        *p++ = (char)('0' + i / 10); *p++ = (char)('0' + i % 10);
        for (const char *s = "-also-long-enough.dat"; *s; s++) *p++ = *s;
        *p = 0;
        ok &= put_file(nm, &i, sizeof(i));
    }
    CHECK(ok && list_dir("Many") == 120);
    {
        int v = -1;
        CHECK(get_file("Many\\file-number-001-with-a-long-name.dat", &v, 4) == 4 && v == 1);
        CHECK(get_file("Many\\again-59-also-long-enough.dat", &v, 4) == 4 && v == 59);
    }

    CHECK(vfs_rename(V, "hello.txt", "hi.txt") == 0);
    CHECK(is_str("hi.txt", "hello world") && get_file("hello.txt", buf, 4) == -E_NOENT);
    CHECK(vfs_rename(V, "hi.txt", "HI.txt") == 0);
    n = list_dir("");
    CHECK(listed(n, "HI.txt") && !listed(n, "hi.txt"));
    CHECK(vfs_rename(V, "HI.txt", "Readme") == -E_EXIST);
    CHECK(vfs_rename(V, "HI.txt", "Docs\\Sub\\moved.txt") == 0 && is_str("Docs\\Sub\\moved.txt", "hello world"));
    CHECK(vfs_rename(V, "Nope", "x") == -E_NOENT);
    CHECK(vfs_rename(V, "Docs\\Sub", "Docs\\Sub2") == 0 && is_str("Docs\\Sub2\\deep.txt", "deep"));
    CHECK(vfs_rename(V, "Docs\\Sub2", "Moved") == 0 && is_str("Moved\\moved.txt", "hello world"));
    CHECK(put_file("Moved\\new.txt", "n", 1));
    CHECK(vfs_rename(V, "Moved", "Docs\\Moved") == 0);
    CHECK(vfs_rename(V, "Docs", "Docs\\Moved\\Docs") == -E_INVAL);
    CHECK(is_str("Docs\\Moved\\deep.txt", "deep") && list_dir("Docs\\Moved") == 3);
    CHECK(vfs_rename(V, "", "x") == -E_INVAL);

    struct file *a, *b;
    CHECK(open_p("share.bin", FD_CREATE, FILE_READ_DATA | FILE_WRITE_DATA, &a) == 0);
    CHECK(open_p("SHARE.BIN", FD_OPEN, FILE_READ_DATA, &b) == 0);
    CHECK(file_write(a, "0123456789", 10) == 10);
    CHECK(file_stat(b, &st) == 0 && st.size == 10 && file_read(b, buf, 64) == 10 && !memcmp(buf, "0123456789", 10));
    CHECK(vfs_remove(V, "share.bin") == -E_BUSY && vfs_rename(V, "share.bin", "other.bin") == -E_BUSY);
    CHECK(vfs_unmount(name) == -E_BUSY);
    close_f(a);
    CHECK(vfs_remove(V, "share.bin") == -E_BUSY);
    close_f(b);
    CHECK(vfs_remove(V, "share.bin") == 0);
    CHECK(vfs_remove(V, "share.bin") == -E_NOENT);

    if (fill_test) {
        uint64_t fb = free_bytes();
        CHECK(open_p("huge.bin", FD_CREATE, FILE_WRITE_DATA, &f) == 0);
        CHECK(file_setsize(f, fb + 10 * cb) == -E_NOSPC);
        CHECK(file_stat(f, &st) == 0 && st.size == 0 && free_bytes() == fb);
        CHECK(file_pwrite(f, fb, "x", 1) == -E_NOSPC && free_bytes() == fb);
        CHECK(file_setsize(f, 0xFFFFFFFFull + 1) == -E_FBIG);
        close_f(f);
        CHECK(vfs_remove(V, "huge.bin") == 0 && free_bytes() == fb);
        CHECK(write_pat("last.bin", 100 * cb, 18, 8192) && verify_pat("last.bin", 100 * cb, 18, 8192));
        CHECK(vfs_remove(V, "last.bin") == 0);
    }

    uint64_t freeA = free_bytes();
    CHECK(vfs_unmount(name) == 0);
    obj_unref(&V->o);
    V = NULL;
    {
        struct object *o;
        char path[32] = "\\Volumes\\";
        size_t l = strlen(path);
        memcpy(path + l, name, strlen(name) + 1);
        CHECK(ns_lookup(path, &o) == -E_NOENT);
    }
    V = mount_dev(name, rd, false);
    CHECK(V != NULL);
    if (V) {
        CHECK(free_bytes() == freeA);
        CHECK(verify_pat("big.bin", 300 * KiB + 77, 14, 5000) && verify_pat("exact.bin", 3 * cb, 11, 512));
        CHECK(is_str("Docs\\Moved\\deep.txt", "deep") && is_str("Readme", "r"));
        CHECK(list_dir("Many") == 120 && list_dir("") >= 12);
        CHECK(is_str("long name one.txt", "1"));
        CHECK(vfs_unmount(name) == 0);
        obj_unref(&V->o);
        V = mount_dev(name, rd, true);
        CHECK(V != NULL);
        if (V) {
            CHECK(is_str("Readme", "r"));
            CHECK(open_p("Readme", FD_OPEN, FILE_WRITE_DATA, &f) == -E_ROFS);
            CHECK(open_p("new.txt", FD_CREATE, FILE_WRITE_DATA, &f) == -E_ROFS);
            CHECK(vfs_mkdir(V, "d") == -E_ROFS && vfs_remove(V, "Readme") == -E_ROFS &&
                  vfs_rename(V, "Readme", "R2") == -E_ROFS);
            CHECK(vfs_unmount(name) == 0);
            obj_unref(&V->o);
        }
    }
    V = NULL;
    blk_unregister(rd);
    CHECK(vfs_live_vnodes() == vn0 && obj_live() == ob0);
    CHECK(heap_check());
}

static void test_core(const char *name, uint64_t sectors, uint32_t spc, bool fill_test)
{
    test_core_ss(name, sectors, spc, fill_test, 512, 512);
}

static void test_blk_ss(void)
{
    CHECK(blk_ramdisk_create_ss("bad", 100, 256) == NULL && blk_ramdisk_create_ss("bad", 100, 1000) == NULL &&
          blk_ramdisk_create_ss("bad", 100, 8192) == NULL && blk_ramdisk_create_ss("bad", 100, 0) == NULL);
    CHECK(blk_find("bad") == NULL);
    static const uint32_t sizes[] = { 512, 1024, 2048, 4096 };
    for (unsigned k = 0; k < 4; k++) {
        uint32_t ss = sizes[k];
        struct blkdev *d = blk_ramdisk_create_ss("rss", 64, ss);
        CHECK(d != NULL && blk_ssize(d) == ss);
        if (!d)
            continue;
        uint8_t *a = kmalloc(3 * ss), *b = kmalloc(3 * ss);
        if (!a || !b) {
            CHECK(false);
            blk_unregister(d);
            kfree(a);
            kfree(b);
            continue;
        }
        for (uint32_t i = 0; i < 3 * ss; i++)
            a[i] = (uint8_t)(i * 13 + ss);
        CHECK(blk_write(d, 10, 3, a) == 0 && blk_read(d, 10, 3, b) == 0 && !memcmp(a, b, 3 * ss));
        CHECK(blk_read(d, 63, 1, b) == 0 && blk_read(d, 63, 2, b) == -E_INVAL);
        uint8_t patch[7] = { 1, 2, 3, 4, 5, 6, 7 };
        CHECK(blk_write_part(d, 11, ss - 3, 3, patch) == 0 && blk_write_part(d, 12, 0, 4, patch + 3) == 0);
        CHECK(blk_read(d, 10, 3, b) == 0);
        memcpy(a + ss + ss - 3, patch, 3);
        memcpy(a + 2 * ss, patch + 3, 4);
        CHECK(!memcmp(a, b, 3 * ss));
        uint8_t t[5];
        CHECK(blk_read_part(d, 11, ss - 3, 3, t) == 0 && !memcmp(t, patch, 3));
        CHECK(blk_read_part(d, 11, ss - 2, 3, t) == -E_INVAL && blk_read_part(d, 11, ss, 1, t) == -E_INVAL);
        CHECK(blk_read_part(d, 64, 0, 1, t) == -E_INVAL);
        struct blkdev *p = blk_partition_create(d, "rss-p", 8, 20);
        CHECK(p != NULL && blk_ssize(p) == ss);
        if (p) {
            CHECK(blk_read(p, 2, 3, b) == 0 && !memcmp(a, b, 3 * ss));
            CHECK(blk_write_part(p, 2, 100, 5, patch) == 0 && blk_read_part(d, 10, 100, 5, t) == 0 &&
                  !memcmp(t, patch, 5));
            blk_unregister(p);
        }
        kfree(a);
        kfree(b);
        blk_unregister(d);
    }
    klog(LOG_INFO, "fstest: block layer: 512/1024/2048/4096-byte sectors, partial-sector RMW, partitions inherit, bad sizes refused");
}

static int pt_rd(struct blkdev *d, uint64_t lba, uint32_t n, void *buf) { return blk_read(d->priv, lba, n, buf); }
static int pt_wr(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) { return blk_write(d->priv, lba, n, buf); }

static void test_blk_cache_ss(void)
{
    struct blkdev *ram = blk_ramdisk_create_ss("rcache-ram", 32, 4096);
    struct blkdev *d = kzalloc(sizeof(*d));
    uint8_t *a = kmalloc(4096), *b = kmalloc(4096);
    CHECK(ram != NULL && d != NULL && a != NULL && b != NULL);
    if (!ram || !d || !a || !b) {
        kfree(d);
        kfree(a);
        kfree(b);
        if (ram)
            blk_unregister(ram);
        return;
    }
    memcpy(d->name, "rcache", 7);
    d->sectors = 32;
    d->ssize = 4096;
    d->cached = true;
    d->rd = pt_rd;
    d->wr = pt_wr;
    d->priv = ram;
    CHECK(blk_register(d) == 0);
    for (int i = 0; i < 4096; i++)
        a[i] = (uint8_t)(i ^ (i >> 7));
    CHECK(blk_write(d, 5, 1, a) == 0);
    uint64_t h0 = d->st_hits;
    CHECK(blk_read(d, 5, 1, b) == 0 && !memcmp(a, b, 4096) && d->st_hits == h0 + 1);
    uint8_t patch[3] = { 9, 8, 7 };
    CHECK(blk_write_part(d, 5, 4090, 3, patch) == 0);
    memcpy(a + 4090, patch, 3);
    CHECK(blk_read(d, 5, 1, b) == 0 && !memcmp(a, b, 4096));
    CHECK(blk_read(ram, 5, 1, b) == 0 && !memcmp(a, b, 4096));
    CHECK(blk_read(d, 6, 1, b) == 0 && blk_read(d, 6, 1, b) == 0 && d->st_hits >= h0 + 3);
    memset(patch, 0, sizeof(patch));
    CHECK(blk_read_part(d, 5, 4090, 3, patch) == 0 && patch[0] == 9 && patch[2] == 7);
    blk_unregister(d);
    blk_unregister(ram);
    kfree(a);
    kfree(b);
    CHECK(heap_check());
}

static void test_ss_validation(void)
{
    struct blkdev *rd = blk_ramdisk_create_ss("rfv", 150000, 512);
    CHECK(rd != NULL);
    if (!rd)
        return;
    CHECK(fat32_format_ss(rd, "X", 1, 256) == -E_INVAL);
    CHECK(fat32_format_ss(rd, "X", 1, 1000) == -E_INVAL);
    CHECK(fat32_format_ss(rd, "X", 1, 8192) == -E_INVAL);
    CHECK(fat32_format_ss(rd, "X", 1, 4096) == -E_INVAL);
    CHECK(fat32_format_ss(rd, "X", 3, 1024) == -E_INVAL);
    CHECK(fat32_format_ss(rd, "X", 1, 1024) == 0);
    struct volume *v = NULL;
    CHECK(vfs_mount("rfv", rd, false, &v) == 0 && v);
    if (v) {
        CHECK(vfs_unmount("rfv") == 0);
        obj_unref(&v->o);
    }
    static const uint16_t bad[] = { 256, 1000, 8192, 0 };
    for (unsigned i = 0; i < 4; i++) {
        uint8_t w[2] = { (uint8_t)bad[i], (uint8_t)(bad[i] >> 8) };
        v = NULL;
        CHECK(blk_write_part(rd, 0, 11, 2, w) == 0);
        CHECK(vfs_mount("rfv", rd, false, &v) == -E_INVAL && !v);
    }
    blk_unregister(rd);
    CHECK(heap_check());
}

static void test_ss_partitions(void)
{
    static const uint32_t dsz[] = { 2048, 4096 };
    for (unsigned k = 0; k < 2; k++) {
        uint32_t dss = dsz[k];
        uint64_t nsec = 600ULL * 1024 * 1024 / dss;
        struct blkdev *rd = blk_ramdisk_create_ss("rpq", nsec, dss);
        CHECK(rd != NULL);
        if (!rd)
            continue;
        uint8_t *mbr = kzalloc(dss);
        if (!mbr) {
            CHECK(false);
            blk_unregister(rd);
            continue;
        }
        #define PE(i, type, first, count) do { uint8_t *e = mbr + 446 + (i) * 16; e[4] = (type); \
            for (int q = 0; q < 4; q++) { e[8 + q] = (uint8_t)((uint32_t)(first) >> (8 * q)); \
                                          e[12 + q] = (uint8_t)((uint32_t)(count) >> (8 * q)); } } while (0)
        PE(0, 0x0C, 256, nsec / 2);
        PE(1, 0x83, nsec / 2 + 256, nsec / 4);
        mbr[510] = 0x55; mbr[511] = 0xAA;
        CHECK(blk_write(rd, 0, 1, mbr) == 0);
        kfree(mbr);
        #undef PE
        CHECK(blk_scan_partitions(rd) == 2);
        struct blkdev *p1 = blk_find("rpqp1"), *p2 = blk_find("rpqp2");
        CHECK(p1 && blk_ssize(p1) == dss && p1->sectors == nsec / 2);
        if (p1) {
            CHECK(fat32_format(p1, "PQ", 1) == 0);
            struct volume *v = V = mount_dev("rpq1", p1, false);
            CHECK(v != NULL);
            if (v) {
                CHECK(write_pat("pq.bin", 300 * KiB + 5, 3, 5000) && verify_pat("pq.bin", 300 * KiB + 5, 3, 7777));
                CHECK(vfs_sync(V) == 0);
                CHECK(vfs_unmount("rpq1") == 0);
                obj_unref(&V->o);
                V = NULL;
            }
        }
        if (p2)
            blk_unregister(p2);
        if (p1)
            blk_unregister(p1);
        blk_unregister(rd);
    }
    struct blkdev *sf = blk_ramdisk_create_ss("rsf4", 75000, 4096);
    CHECK(sf && fat32_format(sf, "SF", 1) == 0 && blk_scan_partitions(sf) == 0 && !blk_find("rsf4p1"));
    if (sf)
        blk_unregister(sf);
    struct blkdev *sg = blk_ramdisk_create_ss("rsf5", 40000, 4096);
    CHECK(sg && fat32_format_ss(sg, "SG", 1, 2048) == 0 && blk_scan_partitions(sg) == 0);
    if (sg)
        blk_unregister(sg);
    CHECK(heap_check());
    klog(LOG_INFO, "fstest: sector sizes: bad sizes refused by mkfs and mount, MBR + FAT on 2K/4K disks, superfloppies ok");
}

static void test_partitions(void)
{
    struct blkdev *rd = blk_ramdisk_create("rp", 300000);
    CHECK(rd != NULL);
    if (!rd)
        return;
    uint8_t mbr[512];
    memset(mbr, 0, sizeof(mbr));
    #define PENT(i, type, first, count) do { uint8_t *e = mbr + 446 + (i) * 16; e[4] = (type); \
        e[8] = (uint8_t)(first); e[9] = (uint8_t)((first) >> 8); e[10] = (uint8_t)((first) >> 16); e[11] = (uint8_t)((first) >> 24); \
        e[12] = (uint8_t)(count); e[13] = (uint8_t)((count) >> 8); e[14] = (uint8_t)((count) >> 16); e[15] = (uint8_t)((count) >> 24); } while (0)
    PENT(0, 0x0C, 2048, 100000);
    PENT(1, 0x83, 102048, 80000);
    PENT(2, 0x0C, 290000, 50000);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    CHECK(blk_write(rd, 0, 1, mbr) == 0);
    CHECK(blk_scan_partitions(rd) == 2);
    struct blkdev *p1 = blk_find("rpp1"), *p2 = blk_find("rpp2");
    CHECK(p1 && p2 && p1->sectors == 100000 && p2->sectors == 80000 && !blk_find("rpp3"));
    if (p1 && p2) {
        CHECK(fat32_format(p1, "PART", 0) == 0);
        uint8_t s[512];
        CHECK(blk_read(rd, 2048, 1, s) == 0 && s[0] == 0xEB && !memcmp(s + 3, "INFINITY", 8));
        CHECK(blk_read(rd, 2047, 1, s) == 0 && s[0] == 0);
        struct volume *v = mount_dev("rpart", p1, false);
        CHECK(v != NULL);
        if (v) {
            struct volume *save = V;
            V = v;
            CHECK(put_file("on-partition.txt", "yes", 3) && is_str("on-partition.txt", "yes"));
            V = save;
            CHECK(vfs_unmount("rpart") == 0);
            obj_unref(&v->o);
        }
        struct volume *bad = NULL;
        CHECK(vfs_mount("rbad", p2, false, &bad) == -E_INVAL);
        blk_unregister(p1);
        blk_unregister(p2);
    }
    struct blkdev *sf = blk_ramdisk_create("rsf", 80000);
    CHECK(sf && fat32_format(sf, "SF", 0) == 0 && blk_scan_partitions(sf) == 0 && blk_count() >= 1);
    if (sf)
        blk_unregister(sf);
    struct blkdev *tiny = blk_ramdisk_create("rtiny", 20000);
    CHECK(tiny && fat32_format(tiny, "T", 0) == -E_INVAL);
    if (tiny)
        blk_unregister(tiny);
    blk_unregister(rd);
    klog(LOG_INFO, "fstest: MBR partitions (+ out-of-range entry skipped), superfloppy, mkfs size limits ok");
}

static int run_fs(const char *name, uint64_t arg, uint32_t uid)
{
    struct task *t = task_new(name, user_tests_start, IMG_SIZE, UT_FS, arg);
    if (!t)
        return -1;
    sec_token_user(&t->tok, uid, NULL, 0, 0);
    task_start(t);
    return task_wait(t);
}

static void test_user(void)
{
    unsigned vn0 = vfs_live_vnodes(), ob0 = obj_live();
    struct blkdev *rd = blk_ramdisk_create("t0", 80000);
    CHECK(rd && fat32_format(rd, "T0", 0) == 0);
    if (!rd)
        return;
    struct volume *v = mount_dev("t0", rd, false);
    CHECK(v != NULL);
    if (!v) {
        blk_unregister(rd);
        return;
    }
    V = v;
    CHECK(put_file("hello.txt", "hello ramdisk", 13));
    static const struct ace acl[] = {
        { ACE_ALLOW, 0, 0, 1000, ACCESS_GENERIC_ALL },
        { ACE_ALLOW, 0, 0, SID_EVERYONE, ACCESS_GENERIC_READ | ACCESS_GENERIC_EXECUTE },
    };
    CHECK(sec_kset(&v->o, SID_SYSTEM, acl, 2) == 0);

    struct token alice, bob;
    sec_token_user(&alice, 1000, NULL, 0, 0);
    sec_token_user(&bob, 1001, NULL, 0, 0);
    uint32_t g;
    CHECK(sec_check(&alice, &v->o, FILE_WRITE_DATA | FILE_CREATE | FILE_DELETE_CHILD, &g) == 0);
    CHECK(sec_check(&bob, &v->o, FILE_READ_DATA, &g) == 0);
    CHECK(sec_check(&bob, &v->o, FILE_WRITE_DATA, &g) == -E_ACCES);
    CHECK(sec_check(&bob, &v->o, FILE_CREATE, &g) == -E_ACCES);
    CHECK(sec_check(&bob, &v->o, 0x100, &g) == -E_INVAL);

    int rc = run_fs("fs-alice", 1, 1000);
    if (rc)
        klog(LOG_FAIL, "fs-alice exited with 0x%x (the check that failed)", rc);
    CHECK(rc == 0);
    rc = run_fs("fs-bob", 0, 1001);
    if (rc)
        klog(LOG_FAIL, "fs-bob exited with 0x%x (the check that failed)", rc);
    CHECK(rc == 0);

    struct file_stat st;
    CHECK(vfs_stat(v, "dir", &st) == -E_NOENT && vfs_stat(v, "new.txt", &st) == -E_NOENT);
    CHECK(vfs_stat(v, "hello.txt", &st) == 0 && st.size == 13);
    CHECK(vfs_live_vnodes() == vn0 + 1);
    V = NULL;
    CHECK(vfs_unmount("t0") == 0);
    obj_unref(&v->o);
    blk_unregister(rd);
    CHECK(vfs_live_vnodes() == vn0 && obj_live() == ob0);
    klog(LOG_INFO, "fstest: ring-3 file syscalls: open/read/write/seek/stat/readdir/rename/remove, ACL denial, no leaks ok");
}

static bool vol_open(const char *name)
{
    struct object *o;
    if (ns_lookup(name, &o))
        return false;
    bool ok = o->type == &volume_type;
    if (ok)
        V = (struct volume *)o;
    else
        obj_unref(o);
    return ok;
}

static void test_hostdisk(void)
{
    if (!vol_open("\\Volumes\\hd0")) {
        klog(LOG_INFO, "fstest: no disk attached (run tools/mkdisk.sh): skipping the host-image tests");
        return;
    }
    struct volume *vol = V;
    struct file_stat st;
    struct file *f;
    if (vfs_stat(V, "HELLO.TXT", &st) || st.size != 20) {
        klog(LOG_INFO, "fstest: \\Volumes\\hd0 is not the test image: skipping");
        obj_unref(&vol->o);
        V = NULL;
        return;
    }
    CHECK(is_str("HELLO.TXT", "hello from the host\n"));
    CHECK(is_str("long file name example.txt", "long names, with spaces\n"));
    CHECK(is_str("LONGFI~1.TXT", "long names, with spaces\n"));
    CHECK(is_str("DOCS\\NESTED\\DEEP.TXT", "deep\n"));
    CHECK(verify_pat("BIG.BIN", 3 * MiB + 777, 0, 4096));
    CHECK(verify_pat("BIG.BIN", 3 * MiB + 777, 0, 65536));
    int n = list_dir("");
    CHECK(n == 7 && listed(n, "KERNEL.TST") && listed(n, "HELLO.TXT") && listed(n, "Long File Name Example.txt") &&
          listed(n, "LOCKED.TXT") && listed(n, "lower.txt") && listed(n, "BIG.BIN") && listed(n, "DOCS"));
    CHECK(vfs_stat(V, "LOCKED.TXT", &st) == 0 && (st.attr & FA_RDONLY));
    CHECK(open_p("LOCKED.TXT", FD_OPEN, FILE_WRITE_DATA, &f) == -E_ACCES);
    CHECK(vfs_remove(V, "LOCKED.TXT") == -E_ACCES);
    CHECK(is_str("LOCKED.TXT", "read only\n"));
    CHECK(vfs_stat(V, "HELLO.TXT", &st) == 0 && st.mtime > 1700000000 && st.mtime < wall_unix() + 86400);
    struct fs_statfs sf;
    CHECK(vfs_statfs(V, &sf) == 0 && !strcmp(sf.label, "INFDISK") && sf.cluster_bytes == 512 &&
          sf.total_bytes == 129022ULL * 512);

    {
        uint8_t *a = kmalloc(8 * 512), *b = kmalloc(8 * 512), *c = kmalloc(512), *e = kmalloc(512);
        struct blkdev *d = V->sb->dev;
        if (a && b && c && e) {
            CHECK(blk_read(d, 100, 8, a) == 0);
            g_ata_force_lba48 = true;
            CHECK(blk_read(d, 100, 8, b) == 0);
            memset(c, 0x5A, 512);
            CHECK(blk_write(d, d->sectors - 1, 1, c) == 0);
            g_ata_force_lba48 = false;
            CHECK(blk_read(d, d->sectors - 1, 1, e) == 0 && !memcmp(c, e, 512));
            CHECK(!memcmp(a, b, 8 * 512));
            memset(c, 0, 512);
            CHECK(blk_write(d, d->sectors - 1, 1, c) == 0);
        } else {
            CHECK(false);
        }
        kfree(a); kfree(b); kfree(c); kfree(e);
    }

    if (vfs_stat(V, "KERNEL.TST", &st)) {
        klog(LOG_INFO, "fstest: host image reads ok (no KERNEL.TST marker: not writing to it)");
        obj_unref(&vol->o);
        V = NULL;
        return;
    }
    CHECK(vfs_remove(V, "KERNEL.TST") == 0);
    CHECK(vfs_mkdir(V, "KERNEL") == 0 && vfs_mkdir(V, "KERNEL\\Sub Dir") == 0);
    CHECK(write_pat("KERNEL\\OUT.BIN", 2 * MiB + 4321, 5, 7000));
    CHECK(put_file("KERNEL\\Sub Dir\\na\xC3\xAFve caf\xC3\xA9.txt", "unicode name\n", 13));
    CHECK(put_file("KERNEL\\Lower Case.txt", "mixed case\n", 11));
    CHECK(put_file("KERNEL\\short.txt", "all lower 8.3\n", 14));
    CHECK(put_file("KERNEL\\UPPER.TXT", "all upper 8.3\n", 14));
    CHECK(open_p("HELLO.TXT", FD_OPEN, FILE_WRITE_DATA, &f) == 0);
    CHECK(file_seek(f, 0, SEEK_END) == 20 && file_write(f, " + kernel\n", 10) == 10);
    close_f(f);
    CHECK(open_p("BIG.BIN", FD_OPEN, FILE_WRITE_DATA, &f) == 0);
    {
        uint8_t z[5000];
        memset(z, 0xAA, sizeof(z));
        CHECK(file_pwrite(f, 1000000, z, sizeof(z)) == (long)sizeof(z));
    }
    close_f(f);
    CHECK(vfs_remove(V, "Long File Name Example.txt") == 0);
    CHECK(vfs_rename(V, "lower.txt", "Renamed Lower.txt") == 0);
    CHECK(vfs_rename(V, "DOCS\\NESTED\\DEEP.TXT", "DOCS\\moved deep.txt") == 0);
    CHECK(vfs_remove(V, "DOCS\\NESTED") == 0);
    CHECK(is_str("HELLO.TXT", "hello from the host\n + kernel\n"));
    CHECK(vfs_sync(V) == 0);
    struct blkdev *d = V->sb->dev;
    klog(LOG_INFO, "fstest: host image: read mkfs.fat/mtools data (incl. LFN, RO attr, 3 MiB file), then wrote "
         "files/dirs/renames/deletes; block cache %lu/%lu sector reads hit, %lu written",
         (uint64_t)d->st_hits, (uint64_t)d->st_reads, (uint64_t)d->st_writes);
    obj_unref(&vol->o);
    V = NULL;
}

static void test_hostdisk_4k(void)
{
    if (!vol_open("\\Volumes\\hd2")) {
        klog(LOG_INFO, "fstest: no 4 KiB-sector disk attached (run tools/mkdisk.sh): skipping");
        return;
    }
    struct volume *vol = V;
    struct file_stat st;
    struct file *f;
    struct fs_statfs sf;
    if (vfs_statfs(V, &sf) || strcmp(sf.label, "DISK4K")) {
        klog(LOG_INFO, "fstest: \\Volumes\\hd2 is not the 4 KiB test image: skipping");
        obj_unref(&vol->o);
        V = NULL;
        return;
    }
    CHECK(sf.cluster_bytes == 4096);
    CHECK(is_str("HELLO.TXT", "hello from a 4096-byte-sector volume\n"));
    CHECK(is_str("long file name example.txt", "long names, with spaces\n"));
    CHECK(is_str("DOCS\\NESTED\\DEEP.TXT", "deep\n"));
    CHECK(verify_pat("BIG.BIN", 3 * MiB + 777, 0, 4096) && verify_pat("BIG.BIN", 3 * MiB + 777, 0, 777));
    int n = list_dir("");
    CHECK(n == 5 && listed(n, "HELLO.TXT") && listed(n, "Long File Name Example.txt") && listed(n, "BIG.BIN") &&
          listed(n, "DOCS") && listed(n, "KERNEL.TST"));
    CHECK(vfs_stat(V, "HELLO.TXT", &st) == 0 && st.mtime > 1700000000);
    CHECK(blk_ssize(V->sb->dev) == 512);

    if (vfs_stat(V, "KERNEL.TST", &st)) {
        klog(LOG_INFO, "fstest: 4 KiB host image reads ok (no KERNEL.TST marker: not writing to it)");
        obj_unref(&vol->o);
        V = NULL;
        return;
    }
    CHECK(vfs_remove(V, "KERNEL.TST") == 0);
    CHECK(vfs_mkdir(V, "KERNEL") == 0 && vfs_mkdir(V, "KERNEL\\MANY") == 0);
    CHECK(write_pat("KERNEL\\OUT.BIN", 1 * MiB + 4321, 5, 6000));
    CHECK(put_file("KERNEL\\Lower Case.txt", "mixed case\n", 11));
    CHECK(put_file("KERNEL\\UPPER.TXT", "all upper 8.3\n", 14));
    for (int i = 0; i < 70; i++) {
        char p[64] = "KERNEL\\MANY\\Many Files With Long Names ";
        size_t l = strlen(p);
        p[l++] = (char)('0' + i / 10);
        p[l++] = (char)('0' + i % 10);
        memcpy(p + l, ".txt", 5);
        char c[16] = "file #";
        c[6] = (char)('0' + i / 10);
        c[7] = (char)('0' + i % 10);
        c[8] = '\n';
        if (!put_file(p, c, 9)) {
            CHECK(false);
            break;
        }
    }
    n = list_dir("KERNEL\\MANY");
    CHECK(n == 70 && listed(n, "Many Files With Long Names 00.txt") && listed(n, "Many Files With Long Names 69.txt"));
    CHECK(is_str("KERNEL\\MANY\\Many Files With Long Names 42.txt", "file #42\n"));
    CHECK(vfs_remove(V, "KERNEL\\MANY\\Many Files With Long Names 13.txt") == 0);
    CHECK(open_p("HELLO.TXT", FD_OPEN, FILE_WRITE_DATA, &f) == 0);
    CHECK(file_seek(f, 0, SEEK_END) == 37 && file_write(f, " + kernel\n", 10) == 10);
    close_f(f);
    CHECK(open_p("BIG.BIN", FD_OPEN, FILE_WRITE_DATA, &f) == 0);
    {
        uint8_t z[9000];
        memset(z, 0xAA, sizeof(z));
        CHECK(file_pwrite(f, 4090, z, sizeof(z)) == (long)sizeof(z));
    }
    close_f(f);
    CHECK(vfs_remove(V, "Long File Name Example.txt") == 0);
    CHECK(vfs_rename(V, "DOCS\\NESTED\\DEEP.TXT", "DOCS\\moved deep.txt") == 0);
    CHECK(vfs_remove(V, "DOCS\\NESTED") == 0);
    CHECK(is_str("HELLO.TXT", "hello from a 4096-byte-sector volume\n + kernel\n"));
    CHECK(vfs_sync(V) == 0);
    klog(LOG_INFO, "fstest: 4 KiB-sector host image (on a 512 B disk): read mkfs.fat data, then wrote a 70-file "
         "long-name directory, a 1 MiB file, appends, an overwrite across sectors, rename, move, deletes");
    obj_unref(&vol->o);
    V = NULL;
}

static char mf[4096];
static size_t mfn;
static void mf_str(const char *s) { while (*s && mfn < sizeof(mf) - 1) mf[mfn++] = *s++; }
static void mf_num(uint64_t v)
{
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && mfn < sizeof(mf) - 1) mf[mfn++] = t[--n];
}
static void mf_entry(const char *path, uint64_t size, uint32_t seed)
{
    mf_str(path); mf_str(" "); mf_num(size); mf_str(" "); mf_num(seed); mf_str("\n");
}

static void test_scratch_dev(const char *devname, const char *mount, uint32_t ss)
{
    struct blkdev *d = blk_find(devname);
    uint8_t s0[32];
    if (!d || blk_read_part(d, 0, 0, sizeof(s0), s0) || memcmp(s0, "INFINITYOS-FSTEST-SCRATCH", 25)) {
        klog(LOG_INFO, "fstest: no scratch disk with the test marker: skipping the format-and-fill test");
        return;
    }
    CHECK(vfs_volume_for_dev(d) == NULL);
    CHECK(fat32_format_ss(d, "scratch", 0, ss) == 0);
    V = mount_dev(mount, d, false);
    CHECK(V != NULL);
    if (!V)
        return;
    mfn = 0;
    CHECK(vfs_mkdir(V, "alpha") == 0 && vfs_mkdir(V, "beta gamma") == 0 && vfs_mkdir(V, "alpha\\deeper") == 0);
    static const uint64_t sizes[] = { 0, 1, 511, 512, 513, 4095, 4096, 4097, 20000, 65536, 100001 };
    char p[96];
    int idx = 0;
    for (int dir = 0; dir < 3; dir++) {
        const char *dn = dir == 0 ? "alpha" : dir == 1 ? "beta gamma" : "alpha\\deeper";
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++, idx++) {
            char *q = p;
            for (const char *t = dn; *t; t++) *q++ = *t;
            *q++ = '\\';
            const char *base = (i % 3 == 0) ? "file-" : (i % 3 == 1) ? "FILE" : "Mixed Case File ";
            for (const char *t = base; *t; t++) *q++ = *t;
            *q++ = (char)('0' + i / 10); *q++ = (char)('0' + i % 10);
            for (const char *t = ".dat"; *t; t++) *q++ = *t;
            *q = 0;
            CHECK(write_pat(p, sizes[i], (uint32_t)(idx * 37), 1000 + i * 111));
        }
    }
    CHECK(write_pat("big file.bin", 6 * MiB + 13, 99, 16384));
    CHECK(vfs_rename(V, "alpha\\deeper", "beta gamma\\deeper") == 0);
    mfn = 0;
    {
        int idx2 = 0;
        for (int dir = 0; dir < 3; dir++) {
            const char *dn = dir == 0 ? "alpha" : dir == 1 ? "beta gamma" : "beta gamma\\deeper";
            for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++, idx2++) {
                char *q = p;
                for (const char *t = dn; *t; t++) *q++ = *t;
                *q++ = '\\';
                const char *base = (i % 3 == 0) ? "file-" : (i % 3 == 1) ? "FILE" : "Mixed Case File ";
                for (const char *t = base; *t; t++) *q++ = *t;
                *q++ = (char)('0' + i / 10); *q++ = (char)('0' + i % 10);
                for (const char *t = ".dat"; *t; t++) *q++ = *t;
                *q = 0;
                mf_entry(p, sizes[i], (uint32_t)(idx2 * 37));
            }
        }
        mf_entry("big file.bin", 6 * MiB + 13, 99);
    }
    CHECK(vfs_remove(V, "alpha\\file-00.dat") == 0 && vfs_remove(V, "beta gamma\\FILE04.dat") == 0);
    {
        char *w = mf, *r = mf;
        while (*r) {
            char *e = r;
            while (*e && *e != '\n') e++;
            size_t l = (size_t)(e - r) + (*e ? 1 : 0);
            bool drop = (!memcmp(r, "alpha\\file-00.dat ", 17)) || (!memcmp(r, "beta gamma\\FILE04.dat ", 22));
            if (!drop) {
                memmove(w, r, l);
                w += l;
            }
            r += l;
        }
        mfn = (size_t)(w - mf);
        mf[mfn] = 0;
    }
    CHECK(put_file("MANIFEST.TXT", mf, mfn));

    uint64_t free_before = free_bytes();
    struct volume *v = V;
    CHECK(vfs_unmount(mount) == 0);
    obj_unref(&v->o);
    V = mount_dev(mount, d, false);
    CHECK(V != NULL);
    if (V) {
        CHECK(free_bytes() == free_before);
        CHECK(verify_pat("big file.bin", 6 * MiB + 13, 99, 4096));
        CHECK(verify_pat("beta gamma\\deeper\\Mixed Case File 02.dat", sizes[2], (uint32_t)((22 + 2) * 37), 4096));
        CHECK(vfs_unmount(mount) == 0);
        obj_unref(&V->o);
    }
    V = NULL;
    klog(LOG_INFO, "fstest: scratch disk: own mkfs, 33 files in 4 dirs + 6 MiB file, directory move, deletes, remount ok");
}

static void test_scratch(void)
{
    test_scratch_dev("hd1", "scratch", 0);
    test_scratch_dev("hd3", "scratch2k", 2048);
}

unsigned fstest_run(void)
{
    n_checks = n_fail = 0;
    unsigned vn0 = vfs_live_vnodes(), ob0 = obj_live();
    uint64_t t0 = clock_ns();

    test_blk();
    test_core("rc1", 73728, 1, true);
    klog(LOG_INFO, "fstest: FAT32 core, 512 B clusters: files, holes, LFN/unicode, dir growth, rename, sharing, ENOSPC, remount ok");
    test_core("rc4", 270000, 4, false);
    klog(LOG_INFO, "fstest: FAT32 core, 2 KiB clusters ok");
    test_blk_ss();
    test_blk_cache_ss();
    test_ss_validation();
    test_core_ss("rs11", 150000, 1, false, 512, 1024);
    test_core_ss("rs12", 290000, 1, false, 512, 2048);
    test_core_ss("rs14", 600000, 1, false, 512, 4096);
    test_core_ss("rs41", 75000, 1, false, 4096, 4096);
    test_core_ss("rs42", 10000, 1, false, 4096, 512);
    test_core_ss("rs44", 560000, 4, false, 512, 1024);
    klog(LOG_INFO, "fstest: FAT32 core on 1K/2K/4K sector volumes over 512/2K/4K devices ok");
    test_ss_partitions();
    test_partitions();
    test_user();
    test_hostdisk();
    test_hostdisk_4k();
    test_scratch();

    CHECK(vfs_live_vnodes() == vn0 && obj_live() == ob0);
    CHECK(heap_check());
    klog(n_fail ? LOG_FAIL : LOG_OK, "FSTEST %s (%u checks, %u failed, %lu ms)", n_fail ? "FAIL" : "PASS",
         n_checks, n_fail, (uint64_t)((clock_ns() - t0) / 1000000));
    console_set_color(n_fail ? C_LRED : C_LGREEN, C_BLACK);
    kprintf("\nStep 5 (FAT32 + VFS) %s.\n", n_fail ? "has FAILURES" : "complete");
    console_set_color(C_LGRAY, C_BLACK);
    return n_fail;
}
