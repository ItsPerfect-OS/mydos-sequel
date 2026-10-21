/* console.c - VGA text console (with hardware cursor) mirrored to COM1 serial */
#include "console.h"
#include "io.h"

#define VGA_MEM  ((volatile u16 *)0xB8000)
#define VGA_COLS 80
#define VGA_ROWS 25
#define COM1     0x3F8

static int cur_x, cur_y;
static u8  color = COL_WHITE;

static void serial_init(void) {
    outb(COM1 + 1, 0x00);  outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);  outb(COM1 + 1, 0x00);   /* 38400 baud */
    outb(COM1 + 3, 0x03);  outb(COM1 + 2, 0x07);
    outb(COM1 + 4, 0x0B);
}
static void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20)) {}
    outb(COM1, (u8)c);
}

static void update_cursor(void) {
    u16 pos = (u16)(cur_y * VGA_COLS + cur_x);
    outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E); outb(0x3D5, (pos >> 8) & 0xFF);
}

static void vga_scroll(void) {
    for (int i = 0; i < VGA_COLS * (VGA_ROWS - 1); i++) VGA_MEM[i] = VGA_MEM[i + VGA_COLS];
    for (int i = 0; i < VGA_COLS; i++)
        VGA_MEM[VGA_COLS * (VGA_ROWS - 1) + i] = ((u16)color << 8) | ' ';
    cur_y = VGA_ROWS - 1;
}

void console_clear(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) VGA_MEM[i] = ((u16)color << 8) | ' ';
    cur_x = cur_y = 0;
    update_cursor();
}

void console_init(void) {
    serial_init();
    color = COL_WHITE;
    console_clear();
}

void console_set_color(u8 c) { color = c; }

static char *cap_buf;
static u32 cap_len, cap_max;
static int cap_mute;

void console_capture_start(char *buf, u32 max, int mute) { cap_buf = buf; cap_max = max; cap_len = 0; cap_mute = mute; if (max) buf[0] = 0; }
u32 console_capture_stop(void) { u32 n = cap_len; if (cap_buf && cap_max) cap_buf[cap_len < cap_max ? cap_len : cap_max - 1] = 0; cap_buf = 0; cap_mute = 0; return n; }

void putc(char c) {
    if (cap_buf && cap_len + 1 < cap_max) { cap_buf[cap_len++] = c; cap_buf[cap_len] = 0; }
    if (cap_buf && cap_mute) return;
    if (c == '\b') {
        serial_putc('\b'); serial_putc(' '); serial_putc('\b');
        if (cur_x > 0) cur_x--;
        else if (cur_y > 0) { cur_y--; cur_x = VGA_COLS - 1; }
        VGA_MEM[cur_y * VGA_COLS + cur_x] = ((u16)color << 8) | ' ';
        update_cursor();
        return;
    }
    if (c == '\t') {
        do { putc(' '); } while (cur_x % 4);
        return;
    }
    if (c == '\n') {
        serial_putc('\r'); serial_putc('\n');
        cur_x = 0; cur_y++;
    } else {
        serial_putc(c);
        VGA_MEM[cur_y * VGA_COLS + cur_x] = ((u16)color << 8) | (u8)c;
        if (++cur_x >= VGA_COLS) { cur_x = 0; cur_y++; }
    }
    if (cur_y >= VGA_ROWS) vga_scroll();
    update_cursor();
}

void puts(const char *s) { while (*s) putc(*s++); }

void put_dec(u32 v) {
    char buf[11];
    int n = 0;
    do { buf[n++] = '0' + (v % 10); v /= 10; } while (v);
    while (n) putc(buf[--n]);
}

void put_hex(u32 v) {
    static const char d[] = "0123456789ABCDEF";
    puts("0x");
    for (int i = 28; i >= 0; i -= 4) putc(d[(v >> i) & 0xF]);
}

void put_hex64(u64 v) {
    static const char d[] = "0123456789ABCDEF";
    u32 hi = (u32)(v >> 32), lo = (u32)v;
    puts("0x");
    for (int i = 28; i >= 0; i -= 4) putc(d[(hi >> i) & 0xF]);
    for (int i = 28; i >= 0; i -= 4) putc(d[(lo >> i) & 0xF]);
}
