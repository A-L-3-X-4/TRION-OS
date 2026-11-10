#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#define INFINITY_VERSION "0.0.4"

#define PAGE_SIZE 4096ULL
#define KiB (1024ULL)
#define MiB (1024ULL * KiB)
#define GiB (1024ULL * MiB)

#define ALIGN_UP(x, a)   (((uint64_t)(x) + ((uint64_t)(a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((uint64_t)(x) & ~((uint64_t)(a) - 1))

enum {
    C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LGRAY,
    C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE
};

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);

void serial_init(void);
void serial_puts(const char *s);
void console_init(void);
void console_fb_init(uint64_t mbi_addr);
bool console_fb_active(void);
void console_set_color(uint8_t fg, uint8_t bg);
void console_vga_puts(const char *s);
void kputc(char c);
void kputs(const char *s);
void console_capture_begin(char *buf, size_t max);
size_t console_capture_end(void);

enum klog_level { LOG_OK, LOG_INFO, LOG_WARN, LOG_FAIL };
void kvprintf(const char *fmt, va_list ap);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog(enum klog_level lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

__attribute__((noreturn)) void kpanic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void gdt_init(void);

void idt_init(void);

unsigned selftest_run(void);
