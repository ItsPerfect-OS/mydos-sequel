#ifndef IO_H
#define IO_H
#include "types.h"
static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline u8   inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void insw(u16 port, void *addr, u64 count) {
    __asm__ volatile("cld; rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}
static inline void outsw(u16 port, const void *addr, u64 count) {
    __asm__ volatile("cld; rep outsw" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}
#endif
