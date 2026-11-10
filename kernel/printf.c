#include "kernel.h"

static int utoa(char *out, uint64_t v, unsigned base, bool upper)
{
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = digs[v % base];
        v /= base;
    } while (v);
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

static void put_n(char c, int n)
{
    while (n-- > 0)
        kputc(c);
}

static void emit(const char *prefix, const char *body, int blen, int width, bool left, bool zero)
{
    int plen = (int)strlen(prefix);
    int pad = width - plen - blen;
    if (pad < 0)
        pad = 0;

    if (left) {
        kputs(prefix);
        for (int i = 0; i < blen; i++) kputc(body[i]);
        put_n(' ', pad);
    } else if (zero) {
        kputs(prefix);
        put_n('0', pad);
        for (int i = 0; i < blen; i++) kputc(body[i]);
    } else {
        put_n(' ', pad);
        kputs(prefix);
        for (int i = 0; i < blen; i++) kputc(body[i]);
    }
}

void kvprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            kputc(*fmt);
            continue;
        }
        fmt++;

        bool left = false, zero = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        int lng = 0;
        while (*fmt == 'l' || *fmt == 'z') {
            lng++;
            fmt++;
        }

        char num[24];
        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            uint64_t u = v < 0 ? -(uint64_t)v : (uint64_t)v;
            int n = utoa(num, u, 10, false);
            emit(v < 0 ? "-" : "", num, n, width, left, zero);
            break;
        }
        case 'u': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            int n = utoa(num, v, 10, false);
            emit("", num, n, width, left, zero);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            int n = utoa(num, v, 16, *fmt == 'X');
            emit("", num, n, width, left, zero);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)va_arg(ap, void *);
            int n = utoa(num, v, 16, false);
            emit("0x", num, n, 16 + 2, left, true);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            emit("", s, (int)strlen(s), width, left, false);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            emit("", &c, 1, width, left, false);
            break;
        }
        case '%':
            kputc('%');
            break;
        case '\0':
            return;
        default:
            kputc('%');
            kputc(*fmt);
            break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kputs("");
}

void klog(enum klog_level lvl, const char *fmt, ...)
{
    static const struct { const char *tag; uint8_t color; } L[] = {
        [LOG_OK]   = { " OK ", C_LGREEN },
        [LOG_INFO] = { "INFO", C_LCYAN  },
        [LOG_WARN] = { "WARN", C_YELLOW },
        [LOG_FAIL] = { "FAIL", C_LRED   },
    };

    console_set_color(C_LGRAY, C_BLACK);
    kputc('[');
    console_set_color(L[lvl].color, C_BLACK);
    kputs(L[lvl].tag);
    console_set_color(C_LGRAY, C_BLACK);
    kputs("] ");

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kputc('\n');
    kputs("");
}
