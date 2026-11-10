#include "kernel.h"
#include "io.h"
#include "timer.h"
#include "blk.h"

static uint64_t base_unix, base_ns;

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static bool rtc_updating(void) { return cmos(0x0A) & 0x80; }

uint64_t civil_to_unix(int y, int m, int d, int hh, int mm, int ss)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return (uint64_t)(days * 86400 + hh * 3600 + mm * 60 + ss);
}

void unix_to_civil(uint64_t t, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    uint64_t days = t / 86400, rem = t % 86400;
    *h = (int)(rem / 3600);
    *mi = (int)(rem % 3600 / 60);
    *s = (int)(rem % 60);
    int64_t z = (int64_t)days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *mo = m;
    *y = (int)(yy + (m <= 2));
}

struct rtc_raw { uint8_t s, mi, h, d, mo, y, c, b; };

static void rtc_read_raw(struct rtc_raw *r)
{
    for (int spin = 0; spin < 1000000 && rtc_updating(); spin++)
        ;
    r->s = cmos(0x00); r->mi = cmos(0x02); r->h = cmos(0x04);
    r->d = cmos(0x07); r->mo = cmos(0x08); r->y = cmos(0x09);
    r->c = cmos(0x32); r->b = cmos(0x0B);
}

void rtc_init(void)
{
    struct rtc_raw a, b;
    rtc_read_raw(&a);
    for (int i = 0; i < 5; i++) {
        rtc_read_raw(&b);
        if (!__builtin_memcmp(&a, &b, sizeof(a)))
            break;
        a = b;
    }
    bool bin = a.b & 0x04;
    #define BCD(x) (bin ? (x) : (uint8_t)(((x) & 0x0F) + ((x) >> 4) * 10))
    bool pm = !(a.b & 0x02) && (a.h & 0x80);
    int hour = BCD((uint8_t)(a.h & 0x7F));
    if (!(a.b & 0x02)) {
        hour %= 12;
        if (pm)
            hour += 12;
    }
    int year = BCD(a.y);
    int cent = BCD(a.c);
    year += (cent >= 19 && cent <= 21) ? cent * 100 : 2000;
    int mon = BCD(a.mo), day = BCD(a.d);
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || year < 1980) {
        year = 2026; mon = 1; day = 1; hour = 0;
        a.mi = a.s = 0;
    }
    base_unix = civil_to_unix(year, mon, day, hour, BCD(a.mi), BCD(a.s));
    base_ns = clock_ns();
    #undef BCD
}

uint64_t wall_unix(void)
{
    return base_unix + (clock_ns() - base_ns) / 1000000000ULL;
}

uint64_t wall_ns(void)
{
    return base_unix * 1000000000ULL + (clock_ns() - base_ns);
}
