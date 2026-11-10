#include "kernel.h"
#include "io.h"
#include "multiboot2.h"

#define COM1 0x3F8
static bool serial_ready;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
    serial_ready = true;
}

static void serial_putc(char c)
{
    if (!serial_ready)
        return;
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

#define GLYPH_W 8
#define GLYPH_H 16
extern const uint8_t font8x16[256][16];

static volatile uint16_t *const vga = (volatile uint16_t *)0xB8000;
static int ncols = 80, nrows = 25;
static int cur_x, cur_y;
static uint8_t cur_attr = 0x07;

static struct {
    bool on;
    volatile uint8_t *base;
    uint32_t pitch;
    uint32_t w, h;
    uint32_t pal[16];
} fb;

bool console_fb_active(void) { return fb.on; }

static const uint8_t vga_rgb[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
    {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
    {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
};

static void fb_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t px)
{
    for (uint32_t r = 0; r < h; r++) {
        volatile uint32_t *p = (volatile uint32_t *)(fb.base + (uint64_t)(y + r) * fb.pitch) + x;
        for (uint32_t c = 0; c < w; c++)
            p[c] = px;
    }
}

static void fb_cell(int cx, int cy, uint8_t ch, uint8_t attr)
{
    uint32_t fg = fb.pal[attr & 0x0F], bg = fb.pal[(attr >> 4) & 0x0F];
    for (int r = 0; r < GLYPH_H; r++) {
        uint8_t bits = font8x16[ch][r];
        volatile uint32_t *p = (volatile uint32_t *)(fb.base + (uint64_t)(cy * GLYPH_H + r) * fb.pitch) + cx * GLYPH_W;
        for (int c = 0; c < GLYPH_W; c++)
            p[c] = (bits & (0x80 >> c)) ? fg : bg;
    }
}

static void fb_scroll(void)
{
    uint64_t rowbytes = (uint64_t)GLYPH_H * fb.pitch;
    volatile uint64_t *d = (volatile uint64_t *)fb.base;
    volatile uint64_t *s = (volatile uint64_t *)(fb.base + rowbytes);
    uint64_t n = (uint64_t)(nrows - 1) * rowbytes / 8;
    // Scrolling is copying the whole framebuffer up one text row 8 bytes at a time and reading it back out of VRAM, on every newline at the bottom of the screen, it's slow as shit
    for (uint64_t i = 0; i < n; i++)
        d[i] = s[i];
    fb_fill(0, (uint32_t)(nrows - 1) * GLYPH_H, fb.w, GLYPH_H, fb.pal[(cur_attr >> 4) & 0x0F]);
}

static void vga_update_cursor(void)
{
    if (fb.on)
        return;
    uint16_t pos = (uint16_t)(cur_y * ncols + cur_x);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)(pos >> 8));
}

static void vga_scroll(void)
{
    if (fb.on) {
        fb_scroll();
        return;
    }
    for (int y = 1; y < nrows; y++)
        for (int x = 0; x < ncols; x++)
            vga[(y - 1) * ncols + x] = vga[y * ncols + x];
    for (int x = 0; x < ncols; x++)
        vga[(nrows - 1) * ncols + x] = ((uint16_t)cur_attr << 8) | ' ';
}

static void vga_putc(uint8_t c)
{
    switch (c) {
    case '\n': cur_x = 0; cur_y++; break;
    case '\r': cur_x = 0; break;
    case '\t': cur_x = (cur_x + 8) & ~7; break;
    case '\b': if (cur_x > 0) cur_x--; break;
    default:
        if (fb.on)
            fb_cell(cur_x, cur_y, c, cur_attr);
        else
            vga[cur_y * ncols + cur_x] = ((uint16_t)cur_attr << 8) | c;
        cur_x++;
        break;
    }
    if (cur_x >= ncols) {
        cur_x = 0;
        cur_y++;
    }
    if (cur_y >= nrows) {
        vga_scroll();
        cur_y = nrows - 1;
    }
}

void console_init(void)
{
    cur_attr = 0x07;
    for (int i = 0; i < ncols * nrows; i++)
        vga[i] = ((uint16_t)cur_attr << 8) | ' ';
    cur_x = cur_y = 0;
    vga_update_cursor();
}

void console_fb_init(uint64_t mbi_addr)
{
    const struct mb2_tag_fb *t = (const struct mb2_tag_fb *)mb2_find_tag(mbi_addr, MB2_TAG_FB);
    if (!t || t->fb_type != MB2_FB_RGB || t->bpp != 32 || (t->addr & 3) || (t->pitch & 7))
        return;
    if (t->width < 80 * GLYPH_W || t->height < 25 * GLYPH_H)
        return;
    // boot.asm only identity maps 4 GiB so a framebuffer above that would page fault the second we touched it
    if (t->addr + (uint64_t)t->pitch * t->height > 0x100000000ULL)
        return;
    for (int i = 0; i < 16; i++)
        fb.pal[i] = ((uint32_t)vga_rgb[i][0] << t->red_pos) | ((uint32_t)vga_rgb[i][1] << t->green_pos) |
                    ((uint32_t)vga_rgb[i][2] << t->blue_pos);
    fb.base = (volatile uint8_t *)t->addr;
    fb.pitch = t->pitch;
    fb.w = t->width;
    fb.h = t->height;
    fb.on = true;
    ncols = (int)(fb.w / GLYPH_W);
    nrows = (int)(fb.h / GLYPH_H);
    // Black out the whole screen, whatever GRUB left on it can fuck off
    fb_fill(0, 0, fb.w, fb.h, fb.pal[0]);
    cur_x = cur_y = 0;
}

void console_set_color(uint8_t fg, uint8_t bg)
{
    cur_attr = (uint8_t)((bg << 4) | (fg & 0x0F));
}

void console_vga_puts(const char *s)
{
    while (*s)
        vga_putc((uint8_t)*s++);
    vga_update_cursor();
}

static char *cap_buf;
static size_t cap_len, cap_max;
void console_capture_begin(char *buf, size_t max) { cap_len = 0; cap_max = max; if (max) buf[0] = 0; cap_buf = buf; }
size_t console_capture_end(void)
{
    char *b = cap_buf;
    cap_buf = NULL;
    if (b)
        b[cap_len] = 0;
    return cap_len;
}

void kputc(char c)
{
    if (cap_buf && cap_len + 1 < cap_max)
        cap_buf[cap_len++] = c;
    // Only serial needs this, a bare \n just staircases down the terminal without a \r in front of it
    if (c == '\n')
        serial_putc('\r');
    serial_putc(c);
    vga_putc((uint8_t)c);
}

void kputs(const char *s)
{
    while (*s)
        kputc(*s++);
    vga_update_cursor();
}
