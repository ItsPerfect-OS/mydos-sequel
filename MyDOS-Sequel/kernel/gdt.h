#ifndef GDT_H
#define GDT_H
#include "types.h"

/* Selectors: 0x08 kernel code (64-bit), 0x10 kernel data, 0x18/0x20 user data/code (RPL 3 -> 0x1B/0x23), 0x28 TSS (16 bytes) */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x1B
#define SEL_UCODE 0x23

void gdt_init(void);                 /* replace stage 2's GDT with the kernel's own (ring 3 segments + a 64-bit TSS) */
#endif
