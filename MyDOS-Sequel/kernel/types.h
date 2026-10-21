#ifndef TYPES_H
#define TYPES_H
/* 64-bit kernel (LP64): int is 32 bits, long and pointers are 64. */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
typedef long           s64;
typedef unsigned long  uptr;                 /* an address held as an integer */
typedef unsigned long  size_t;
#endif
