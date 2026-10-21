#include "mdos.h"
#include <stdarg.h>

enum { SYS_EXIT = 0, SYS_WRITE = 1, SYS_GETCHAR = 2, SYS_OPEN = 3, SYS_CLOSE = 4, SYS_READ = 5,
       SYS_UPTIME = 6, SYS_SLEEP = 7, SYS_SBRK = 8 };

/* x86-64 convention: rax = number, rdi/rsi/rdx = arguments, result in rax. */
static inline long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}

void exit(int code) { sys3(SYS_EXIT, code, 0, 0); for (;;) {} }
long write(int fd, const void *buf, size_t n) { return sys3(SYS_WRITE, fd, (long)buf, (long)n); }
long read(int fd, void *buf, size_t n) { return sys3(SYS_READ, fd, (long)buf, (long)n); }
int getchar(void) { return (int)sys3(SYS_GETCHAR, 0, 0, 0); }
int open(const char *path, int mode) { return (int)sys3(SYS_OPEN, (long)path, mode, 0); }
int close(int fd) { return (int)sys3(SYS_CLOSE, fd, 0, 0); }
u32 uptime_ms(void) { return (u32)sys3(SYS_UPTIME, 0, 0, 0); }
void sleep_ms(u32 ms) { sys3(SYS_SLEEP, (long)ms, 0, 0); }
void *sbrk(long bytes) { return (void *)sys3(SYS_SBRK, bytes, 0, 0); }

void putchar(char c) { write(1, &c, 1); }
void print(const char *s) { write(1, s, strlen(s)); }

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (u8)*a - (u8)*b; }
int atoi(const char *s) {
    int sign = 1, v = 0;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return sign * v;
}
void *memset(void *d, int c, size_t n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }

/* ---- printf ---- */

struct out { char buf[128]; int n; };
static void out_flush(struct out *o) { if (o->n) write(1, o->buf, (size_t)o->n); o->n = 0; }
static void out_char(struct out *o, char c) { if (o->n == (int)sizeof(o->buf)) out_flush(o); o->buf[o->n++] = c; }

static void out_num(struct out *o, u32 v, u32 base, int upper, int width, int zero, int left) {
    char tmp[12];
    int n = 0;
    do { u32 d = v % base; tmp[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= base; } while (v);
    int pad = width > n ? width - n : 0;
    if (!left) while (pad-- > 0) out_char(o, zero ? '0' : ' ');
    while (n) out_char(o, tmp[--n]);
    if (left) while (pad-- > 0) out_char(o, ' ');
}

int printf(const char *fmt, ...) {
    struct out o; o.n = 0;
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_char(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, width = 0;
        while (*fmt == '-' || *fmt == '0') { if (*fmt == '-') left = 1; else zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
            case 'd': case 'i': {
                int v = va_arg(ap, int);
                if (v < 0) { out_char(&o, '-'); out_num(&o, (u32)(-v), 10, 0, width > 0 ? width - 1 : 0, zero, left); }
                else out_num(&o, (u32)v, 10, 0, width, zero, left);
                break;
            }
            case 'u': out_num(&o, va_arg(ap, u32), 10, 0, width, zero, left); break;
            case 'x': out_num(&o, va_arg(ap, u32), 16, 0, width, zero, left); break;
            case 'X': out_num(&o, va_arg(ap, u32), 16, 1, width, zero, left); break;
            case 'c': out_char(&o, (char)va_arg(ap, int)); break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                int len = (int)strlen(s), pad = width > len ? width - len : 0;
                if (!left) while (pad-- > 0) out_char(&o, ' ');
                while (*s) out_char(&o, *s++);
                if (left) while (pad-- > 0) out_char(&o, ' ');
                break;
            }
            case '%': out_char(&o, '%'); break;
            case 0: fmt--; break;
            default: out_char(&o, '%'); out_char(&o, *fmt); break;
        }
    }
    va_end(ap);
    out_flush(&o);
    return 0;
}

int getline(char *buf, int max) {
    int n = 0;
    for (;;) {
        int c = getchar();
        if (c == '\n') { putchar('\n'); break; }
        if (c == '\b') { if (n > 0) { n--; putchar('\b'); } continue; }
        if (c >= 32 && c < 127 && n < max - 1) { buf[n++] = (char)c; putchar((char)c); }
    }
    buf[n] = 0;
    return n;
}
