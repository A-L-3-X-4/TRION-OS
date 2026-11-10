#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("w32crt FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static __thread int tls_int = 5;
static __thread char tls_buf[300];
static __thread long long tls_ll = -7;

static void ae1(void) { puts("atexit 1"); }
static void ae2(void) { puts("atexit 2"); }
static void ae3(void) { puts("atexit 3"); }

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "exit")) return atoi(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "abort")) abort();
    if (argc >= 2 && !strcmp(argv[1], "atexit")) {
        atexit(ae1); atexit(ae2); atexit(ae3);
        puts("main done");
        return 3;
    }

    printf("hello %d %s\n", argc, argv[0]);
    CHECK(argc >= 1 && argv[argc] == NULL);
    CHECK(getenv("W32TEST") && !strcmp(getenv("W32TEST"), "1") && getenv("NO_SUCH_VAR") == NULL);

    char b[128];
    CHECK(snprintf(b, sizeof(b), "%d|%5d|%-5d|%05d|%+d|%x|%X|%#x|%o", 42, 42, 42, 42, 42, 255, 255, 255, 8) == 38 &&
          !strcmp(b, "42|   42|42   |00042|+42|ff|FF|0xff|10"));
    CHECK(snprintf(b, sizeof(b), "%s|%10s|%-10s|%.3s|%c|%%", "abc", "abc", "abc", "abcdef", 'z') == 33 &&
          !strcmp(b, "abc|       abc|abc       |abc|z|%"));
    CHECK(snprintf(b, sizeof(b), "%lld %llu %zu %ld", -1234567890123LL, 18446744073709551615ULL, (size_t)77, -5L) > 0 &&
          !strcmp(b, "-1234567890123 18446744073709551615 77 -5"));
    CHECK(snprintf(b, sizeof(b), "%.2f %8.3f %e", 3.14159, 2.5, 1234.5) > 0 && !strncmp(b, "3.14    2.500 ", 14));
    CHECK(snprintf(b, 8, "0123456789") == 10 && !strcmp(b, "0123456"));
    CHECK(sprintf(b, "%s=%d", "x", 9) == 3 && !strcmp(b, "x=9"));
    char *as = NULL;
    CHECK(snprintf(NULL, 0, "%d%d", 12, 34) == 4);
    (void)as;
    fprintf(stderr, "to stderr %d\n", 1);
    fputs("fputs line\n", stdout);
    putchar('p'); putchar('\n');
    printf("pi=%.6f sqrt2=%.6f\n", 3.14159265358979, sqrt(2.0));

    char *p = malloc(100);
    CHECK(p != NULL);
    memset(p, 0x5A, 100);
    p = realloc(p, 100000);
    CHECK(p && p[0] == 0x5A && p[99] == 0x5A);
    free(p);
    int *z = calloc(1000, sizeof(int));
    CHECK(z != NULL);
    int sum = 0; for (int i = 0; i < 1000; i++) sum |= z[i];
    CHECK(sum == 0);
    free(z);
    void *many[300];
    for (int i = 0; i < 300; i++) { many[i] = malloc(1 + i * 7); memset(many[i], i, 1 + i * 7); }
    int bad = 0;
    for (int i = 0; i < 300; i++) { for (int k = 0; k < 1 + i * 7; k++) if (((unsigned char *)many[i])[k] != (unsigned char)i) bad++; free(many[i]); }
    CHECK(bad == 0);

    CHECK(strlen("hello") == 5 && strcmp("a", "b") < 0 && strncmp("abcd", "abcf", 3) == 0 && strchr("hello", 'l') && strrchr("hello", 'l') - "hello" == 3);
    CHECK(strstr("find the needle in", "needle") != NULL && strstr("abc", "zz") == NULL);
    strcpy(b, "foo"); strcat(b, "bar"); CHECK(!strcmp(b, "foobar"));
    char *d = strdup("dup"); CHECK(d && !strcmp(d, "dup")); free(d);
    CHECK(memcmp("abc", "abd", 3) < 0 && memchr("abc", 'c', 3) != NULL);
    CHECK(atoi("  -123abc") == -123 && strtol("0x1F", NULL, 16) == 31 && strtol("777", NULL, 8) == 511 && strtoul("4294967295", NULL, 10) == 4294967295UL);
    char *end; long v = strtol("12345xyz", &end, 10); CHECK(v == 12345 && !strcmp(end, "xyz"));
    CHECK(strtoll("-9223372036854775808", NULL, 10) == (-9223372036854775807LL - 1));
    CHECK(toupper('a') == 'A' && tolower('Q') == 'q' && isdigit('7') && !isdigit('x') && isalpha('k') && isspace('\t') && isalnum('_') == 0 && isxdigit('f') && ispunct('!'));
    int arr[200];
    unsigned r = 7;
    for (int i = 0; i < 200; i++) { r = r * 1103515245 + 12345; arr[i] = (int)((r >> 8) % 1000); }
    qsort(arr, 200, sizeof(int), cmp_int);
    int sorted = 1; for (int i = 1; i < 200; i++) if (arr[i - 1] > arr[i]) sorted = 0;
    CHECK(sorted);
    int key = arr[77];
    CHECK(bsearch(&key, arr, 200, sizeof(int), cmp_int) != NULL);
    srand(1); int r1 = rand(); srand(1); CHECK(rand() == r1 && r1 >= 0 && r1 <= 32767);

    CHECK(fabs(sqrt(2.0) - 1.4142135623730951) < 1e-14);
    CHECK(fabs(sin(1.0) - 0.8414709848078965) < 1e-12 && fabs(cos(1.0) - 0.5403023058681398) < 1e-12);
    CHECK(fabs(pow(2.0, 10.0) - 1024.0) < 1e-9 && fabs(pow(2.0, 0.5) - 1.4142135623730951) < 1e-12);
    CHECK(fabs(exp(1.0) - 2.718281828459045) < 1e-12 && fabs(log(10.0) - 2.302585092994046) < 1e-12);
    CHECK(floor(-2.5) == -3.0 && ceil(-2.5) == -2.0 && floor(2.5) == 2.0 && ceil(2.5) == 3.0 && fmod(7.5, 2.0) == 1.5);
    CHECK(fabs(atan2(1.0, 1.0) - 0.7853981633974483) < 1e-12 && fabs(tan(0.5) - 0.5463024898437905) < 1e-12);
    CHECK(fabs(strtod("3.25e2", NULL) - 325.0) < 1e-9 && fabs(atof("-0.5") + 0.5) < 1e-12);

    CHECK(tls_int == 5 && tls_ll == -7);
    int tz = 0; for (int i = 0; i < 300; i++) tz |= tls_buf[i];
    CHECK(tz == 0);
    tls_int = 99; tls_buf[299] = 'q'; tls_ll *= 3;
    CHECK(tls_int == 99 && tls_buf[299] == 'q' && tls_ll == -21);

    FILE *f = fopen("crt.txt", "w");
    CHECK(f != NULL);
    if (f) {
        CHECK(fprintf(f, "line one\nline %d\n", 2) == 16);
        CHECK(fwrite("tail", 1, 4, f) == 4);
        CHECK(fclose(f) == 0);
    }
    f = fopen("crt.txt", "r");
    CHECK(f != NULL);
    if (f) {
        char l[64];
        CHECK(fgets(l, sizeof(l), f) && !strcmp(l, "line one\n"));
        CHECK(ftell(f) == 10);
        CHECK(fgetc(f) == 'l');
        CHECK(ungetc('L', f) == 'L' && fgetc(f) == 'L');
        CHECK(fseek(f, -4, SEEK_END) == 0 && fread(l, 1, 10, f) == 4 && !memcmp(l, "tail", 4) && feof(f));
        rewind(f);
        CHECK(fgetc(f) == 'l' && !feof(f));
        fclose(f);
    }
    f = fopen("crt.txt", "a"); CHECK(f != NULL); if (f) { fputs("+app", f); fclose(f); }
    f = fopen("crt.txt", "r+");
    CHECK(f != NULL);
    if (f) { char l[64]; size_t n = fread(l, 1, 63, f); CHECK(n == 24 && !memcmp(l + 20, "+app", 4)); fclose(f); }

    {
        char l[64];
        size_t n;
        // In text mode every single \n gets written to disk as \r\n because apparently one byte for a newline was too efficient for Microsoft, so this 9 char string ends up as 13 bytes on disk, I fucking hate it here
        f = fopen("tm.txt", "w");
        CHECK(f != NULL);
        if (f) { CHECK(fputs("a\nb\n", f) >= 0 && fprintf(f, "c%d\n", 3) == 3 && fwrite("d\n", 1, 2, f) == 2); fclose(f); }
        f = fopen("tm.txt", "rb");
        CHECK(f != NULL);
        if (f) { n = fread(l, 1, sizeof(l), f); CHECK(n == 13 && !memcmp(l, "a\r\nb\r\nc3\r\nd\r\n", 13)); fclose(f); }
        f = fopen("tm.txt", "r");
        CHECK(f != NULL);
        if (f) {
            n = fread(l, 1, sizeof(l), f);
            CHECK(n == 9 && !memcmp(l, "a\nb\nc3\nd\n", 9) && feof(f));
            CHECK(ftell(f) == 13);
            rewind(f);
            CHECK(fgets(l, sizeof(l), f) && !strcmp(l, "a\n") && ftell(f) == 3);
            CHECK(fgetc(f) == 'b' && fgetc(f) == '\n' && fgets(l, sizeof(l), f) && !strcmp(l, "c3\n"));
            fclose(f);
        }
        f = fopen("tm.txt", "wt"); CHECK(f != NULL); if (f) { fputs("t\n", f); fclose(f); }
        f = fopen("tm.txt", "rb"); CHECK(f != NULL); if (f) { n = fread(l, 1, sizeof(l), f); CHECK(n == 3 && !memcmp(l, "t\r\n", 3)); fclose(f); }

        f = fopen("tm.bin", "wb");
        CHECK(f != NULL);
        if (f) { CHECK(fwrite("x\ry\r\r\nz\n\x1a" "end", 1, 12, f) == 12); fclose(f); }
        f = fopen("tm.bin", "rb");
        CHECK(f != NULL);
        if (f) { n = fread(l, 1, sizeof(l), f); CHECK(n == 12 && !memcmp(l, "x\ry\r\r\nz\n\x1a" "end", 12)); fclose(f); }
        f = fopen("tm.bin", "r");
        CHECK(f != NULL);
        if (f) { n = fread(l, 1, sizeof(l), f); CHECK(n == 7 && !memcmp(l, "x\ry\r\nz\n", 7) && feof(f) && fgetc(f) == EOF); fclose(f); }

        f = fopen("tm.bin", "r");
        CHECK(f != NULL);
        if (f) {
            int fd = _fileno(f);
            CHECK(_setmode(fd, _O_BINARY) == _O_TEXT && _setmode(fd, _O_TEXT) == _O_BINARY);
            CHECK(_setmode(fd, _O_BINARY) == _O_TEXT);
            n = fread(l, 1, sizeof(l), f); CHECK(n == 12);
            fclose(f);
        }
        f = fopen("tm.bin", "ab"); CHECK(f != NULL); if (f) { fputs("\n", f); fclose(f); }
        f = fopen("tm.bin", "rb"); CHECK(f != NULL); if (f) { n = fread(l, 1, sizeof(l), f); CHECK(n == 13 && l[12] == '\n'); fclose(f); }
        f = fopen("tm.bin", "a"); CHECK(f != NULL); if (f) { fputs("\n", f); fclose(f); }
        f = fopen("tm.bin", "rb"); CHECK(f != NULL); if (f) { n = fread(l, 1, sizeof(l), f); CHECK(n == 15 && l[13] == '\r' && l[14] == '\n'); fclose(f); }
        CHECK(remove("tm.txt") == 0 && remove("tm.bin") == 0);
    }

    CHECK(fopen("no/such/dir/x.txt", "r") == NULL && errno == ENOENT);
    CHECK(fopen("crt.txt", "z") == NULL);
    CHECK(rename("crt.txt", "crt2.txt") == 0 && remove("crt2.txt") == 0 && remove("crt2.txt") != 0);

    atexit(ae1);
    atexit(ae2);
    printf("w32crt done: %d failure(s)\n", fails);
    return fails;
}
