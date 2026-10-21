/* gdt.c - kernel GDT with user-mode segments and a 64-bit TSS
 *
 * In long mode segmentation is mostly flat and ignored; what matters is the privilege level in the code
 * and data selectors, and the TSS: when ring 3 is interrupted or makes a system call, the CPU switches to
 * the kernel stack in tss.rsp0. The double-fault handler gets its own stack (IST1) so even a blown kernel
 * stack produces a readable crash screen instead of a silent reset.
 */
#include "gdt.h"
#include "string.h"

struct tss64 {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3, iomap;
} __attribute__((packed));
typedef char tss_size_check[(sizeof(struct tss64) == 104) ? 1 : -1];

struct gdt_ptr { u16 limit; u64 base; } __attribute__((packed));

extern void gdt_flush(struct gdt_ptr *p);

static u64 gdt[7] __attribute__((aligned(16)));            /* null, kcode, kdata, udata, ucode, TSS (2 slots) */
static struct tss64 tss;
static u8 kstack[16384] __attribute__((aligned(16)));      /* stack used while serving a ring-3 program */
static u8 dfstack[8192] __attribute__((aligned(16)));      /* dedicated double-fault stack (IST1) */

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;                         /* kernel code: ring 0, 64-bit (L=1) */
    gdt[2] = 0x00CF92000000FFFFULL;                         /* kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL;                         /* user data: ring 3 */
    gdt[4] = 0x00AFFA000000FFFFULL;                         /* user code: ring 3, 64-bit */

    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = (u64)(kstack + sizeof(kstack));
    tss.ist[0] = (u64)(dfstack + sizeof(dfstack));
    tss.iomap = sizeof(tss);                                /* no I/O permission bitmap: ring 3 gets no ports */

    u64 base = (u64)&tss, limit = sizeof(tss) - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40)
           | (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);   /* available 64-bit TSS */
    gdt[6] = base >> 32;

    static struct gdt_ptr gp;
    gp.limit = sizeof(gdt) - 1;
    gp.base = (u64)gdt;
    gdt_flush(&gp);
}
