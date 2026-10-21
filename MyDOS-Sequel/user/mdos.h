#ifndef MDOS_H
#define MDOS_H
/* Tiny C library for MyDOS Sequel programs (x86-64, freestanding, system calls via int 0x80). */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
typedef unsigned long  size_t;
#define NULL ((void *)0)

enum { O_READ = 0, O_WRITE = 1, O_APPEND = 2 };      /* O_WRITE creates/truncates; O_APPEND creates/appends */

void exit(int code) __attribute__((noreturn));
long write(int fd, const void *buf, size_t n);           /* fd 1 and 2: screen */
long read(int fd, void *buf, size_t n);                  /* fd 0: keyboard (blocks for at least one key) */
int  getchar(void);                                   /* blocking; Ctrl+C ends the program */
int  open(const char *path, int mode);                /* returns fd >= 3, or -1 */
int  close(int fd);
u32  uptime_ms(void);
void sleep_ms(u32 ms);
void *sbrk(long bytes);                               /* grow the heap; returns the old break, or (void *)-1 */

void putchar(char c);
void print(const char *s);                            /* no newline added */
int  printf(const char *fmt, ...);                    /* %d %i %u %x %X %c %s %% with width / 0 / - flags */
int  getline(char *buf, int max);                     /* echoing line editor; returns the length */

size_t strlen(const char *s);
int  strcmp(const char *a, const char *b);
int  atoi(const char *s);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
#endif
