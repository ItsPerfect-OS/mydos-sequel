#ifndef CONSOLE_H
#define CONSOLE_H
#include "types.h"

enum {
    COL_WHITE = 0x0F, COL_GREY = 0x07, COL_CYAN = 0x0B,
    COL_GREEN = 0x0A, COL_RED = 0x0C, COL_YELLOW = 0x0E,
    COL_WHITE_ON_RED = 0x4F
};

void console_init(void);          /* serial + VGA text, clears screen */
void console_clear(void);
void console_set_color(u8 color);
void console_capture_start(char *buf, u32 max, int mute);   /* record output (optionally hiding it) - used by self-tests */
u32  console_capture_stop(void);                              /* returns the number of bytes captured; buf is NUL-terminated */
void putc(char c);                /* handles \n, \b, \t */
void puts(const char *s);
void put_dec(u32 v);              /* unsigned decimal */
void put_hex(u32 v);
void put_hex64(u64 v);            /* prints 0x%016x */              /* prints 0x%08x */
#endif
