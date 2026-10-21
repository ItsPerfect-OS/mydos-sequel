/* string.c - minimal freestanding libc bits (the compiler may call these) */
#include "string.h"

void *memset(void *d, int c, size_t n) {
    u8 *p = d; while (n--) *p++ = (u8)c; return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    u8 *dp = d; const u8 *sp = s; while (n--) *dp++ = *sp++; return d;
}
void *memmove(void *d, const void *s, size_t n) {
    u8 *dp = d; const u8 *sp = s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (int)((u8)*a - (u8)*b) : 0;
}
char *strcpy(char *d, const char *s) {
    char *r = d; while ((*d++ = *s++)) {} return r;
}
int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
