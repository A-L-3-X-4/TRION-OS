#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

static int fails;
#define CK(c) do { if (!(c)) { printf("lxglibc: FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int cmp(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

int main(int argc, char **argv)
{
    const char *base = argc > 1 ? argv[1] : "/Volumes/lx";
    char p[256], line[128];
    printf("lxglibc: hello from static glibc, argc=%d pid>0=%d\n", argc, getpid() > 0);

    snprintf(line, sizeof line, "%d|%5.2f|%s|%lx|%e", -42, 3.14159, "str", 0xBEEFUL, 12345.678);
    CK(!strcmp(line, "-42| 3.14|str|beef|1.234568e+04"));
    CK(strtod("2.5e3", NULL) == 2500.0);
    CK(fabs(sqrt(2.0) - 1.41421356237) < 1e-9);
    CK(fabs(sin(1.0) - 0.8414709848) < 1e-9);
    CK(atoi("1234") == 1234);

    char *blocks[200];
    for (int i = 0; i < 200; i++) {
        blocks[i] = malloc(1 + i * 37);
        CK(blocks[i] != NULL);
        memset(blocks[i], i, 1 + i * 37);
    }
    for (int i = 0; i < 200; i++) {
        CK(blocks[i][0] == (char)i && blocks[i][i * 37] == (char)i);
        free(blocks[i]);
    }
    size_t big = 6u << 20;
    unsigned char *bm = malloc(big);
    CK(bm != NULL);
    memset(bm, 0xA5, big);
    CK(bm[0] == 0xA5 && bm[big - 1] == 0xA5);
    bm = realloc(bm, big * 2);
    CK(bm != NULL && bm[big - 1] == 0xA5);
    free(bm);
    char *z = calloc(1000, 1000);
    CK(z && z[500000] == 0);
    free(z);

    int arr[500];
    for (int i = 0; i < 500; i++) arr[i] = (i * 7919) % 501;
    qsort(arr, 500, sizeof(int), cmp);
    for (int i = 1; i < 500; i++) CK(arr[i - 1] <= arr[i]);
    char src[1000], dst[1000];
    for (int i = 0; i < 999; i++) src[i] = 'a' + i % 26;
    src[999] = 0;
    memcpy(dst, src, 1000);
    CK(strlen(dst) == 999 && !strcmp(src, dst) && memchr(dst, 'z', 999) == dst + 25);

    snprintf(p, sizeof p, "%s/IN.TXT", base);
    FILE *f = fopen(p, "r");
    CK(f != NULL);
    if (f) {
        CK(fgets(line, sizeof line, f) && !strcmp(line, "hello from the kernel\n"));
        CK(fgets(line, sizeof line, f) == NULL && feof(f));
        fclose(f);
    }
    snprintf(p, sizeof p, "%s/GLIBC.TXT", base);
    f = fopen(p, "w");
    CK(f != NULL);
    if (f) {
        for (int i = 0; i < 1000; i++) fprintf(f, "line %d %.3f\n", i, i / 8.0);
        CK(fclose(f) == 0);
    }
    struct stat st;
    CK(stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 10000);
    f = fopen(p, "r");
    int n = 0;
    while (f && fgets(line, sizeof line, f)) {
        int idx; double v;
        CK(sscanf(line, "line %d %lf", &idx, &v) == 2 && idx == n && v == n / 8.0);
        n++;
    }
    CK(n == 1000);
    if (f) fclose(f);
    char p2[256];
    snprintf(p2, sizeof p2, "%s/GLIBC2.TXT", base);
    CK(rename(p, p2) == 0);
    CK(stat(p, &st) == -1 && errno == ENOENT);
    CK(fopen(p, "r") == NULL && errno == ENOENT);

    DIR *d = opendir(base);
    CK(d != NULL);
    int saw = 0;
    struct dirent *de;
    while (d && (de = readdir(d)))
        if (!strcasecmp(de->d_name, "GLIBC2.TXT")) saw = 1;
    CK(saw);
    if (d) closedir(d);
    CK(remove(p2) == 0);

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    struct timespec nap = { 0, 20 * 1000 * 1000 };
    nanosleep(&nap, NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    CK((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) >= 19 * 1000000L);
    CK(time(NULL) > 1700000000);
    CK(getenv("LXTEST") != NULL || argc > 2);

    printf("lxglibc: %s (%d failures)\n", fails ? "FAILED" : "all checks passed", fails);
    fflush(stdout);
    return fails ? 1 : 0;
}
