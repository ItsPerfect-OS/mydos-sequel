/* idt.c - Interrupt Descriptor Table + CPU exception handler */
#include "idt.h"
#include "console.h"
#include "pic.h"
#include "paging.h"
#include "process.h"

/* A long-mode gate is 16 bytes. */
struct idt_entry {
    u16 offset_lo;
    u16 selector;
    u8  ist;                                  /* interrupt stack table slot (0 = none) */
    u8  flags;                                /* 0x8E: present, ring 0, interrupt gate; 0xEE: callable from ring 3 */
    u16 offset_mid;
    u32 offset_hi;
    u32 zero;
} __attribute__((packed));
typedef char idt_entry_size_check[(sizeof(struct idt_entry) == 16) ? 1 : -1];

struct idt_ptr { u16 limit; u64 base; } __attribute__((packed));

static struct idt_entry idt[256] __attribute__((aligned(16)));
extern u64 isr_stub_table[32];
extern u64 irq_stub_table[16];
extern char isr_syscall_stub[];
static irq_handler_t irq_handlers[16];

static void idt_set_gate(int n, u64 handler, u8 flags, u8 ist) {
    idt[n].offset_lo  = handler & 0xFFFF;
    idt[n].selector   = 0x08;                 /* kernel code segment */
    idt[n].ist        = ist;
    idt[n].flags      = flags;
    idt[n].offset_mid = (handler >> 16) & 0xFFFF;
    idt[n].offset_hi  = (u32)(handler >> 32);
    idt[n].zero       = 0;
}

void idt_init(void) {
    for (int i = 0; i < 32; i++) idt_set_gate(i, isr_stub_table[i], 0x8E, i == 8 ? 1 : 0);   /* #DF runs on its own stack */
    for (int i = 0; i < 16; i++) idt_set_gate(IRQ_BASE + i, irq_stub_table[i], 0x8E, 0);
    idt_set_gate(0x80, (u64)isr_syscall_stub, 0xEE, 0);           /* system calls: DPL 3 so ring 3 may use `int 0x80` */
    static struct idt_ptr p;
    p.limit = sizeof(idt) - 1;
    p.base  = (u64)&idt;
    __asm__ volatile("lidt %0" :: "m"(p));
}

void irq_install_handler(int irq, irq_handler_t handler) {
    irq_handlers[irq] = handler;
    pic_unmask(irq);
}

static const char *exception_names[32] = {
    "#DE Divide Error",           "#DB Debug",
    "NMI Interrupt",              "#BP Breakpoint",
    "#OF Overflow",               "#BR Bound Range Exceeded",
    "#UD Invalid Opcode",         "#NM Device Not Available",
    "#DF Double Fault",           "Coprocessor Segment Overrun",
    "#TS Invalid TSS",            "#NP Segment Not Present",
    "#SS Stack-Segment Fault",    "#GP General Protection Fault",
    "#PF Page Fault",             "Reserved",
    "#MF x87 FP Exception",       "#AC Alignment Check",
    "#MC Machine Check",          "#XM SIMD FP Exception",
    "#VE Virtualization",         "#CP Control Protection",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "#HV Hypervisor Injection",   "#VC VMM Communication",
    "#SX Security Exception",     "Reserved"
};

const char *idt_exception_name(u64 vector) { return exception_names[vector & 31]; }

static void reg(const char *name, u64 v) {
    puts(name); puts("="); put_hex64(v); puts("  ");
}

void isr_handler(struct regs *r) {
    if (r->int_no == 0x80) { syscall_dispatch(r); return; }

    /* Hardware interrupts (remapped PIC): dispatch, then acknowledge. */
    if (r->int_no >= IRQ_BASE && r->int_no < IRQ_BASE + 16) {
        u8 irq = (u8)(r->int_no - IRQ_BASE);
        if (pic_is_spurious(irq)) return;           /* no EOI for a spurious IRQ */
        if (irq_handlers[irq]) irq_handlers[irq](r);
        pic_send_eoi(irq);
        if ((r->cs & 3) == 3 && process_kill_pending()) process_terminate(130, 0, 1);   /* Ctrl+C while a program spins */
        return;
    }

    /* Anything the CPU raises while ring 3 is running is the program's fault, not the kernel's. */
    if ((r->cs & 3) == 3 && process_running()) process_user_fault(r);

    /* Breakpoint is a trap: report it and carry on. */
    if (r->int_no == 3) {
        console_set_color(COL_YELLOW);
        puts("[#BP] breakpoint at RIP="); put_hex64(r->rip); puts(" - continuing\n");
        return;
    }

    /* Page faults may be legitimate (demand paging): fix them and let the CPU retry. */
    if (r->int_no == 14) {
        u64 addr;
        __asm__ volatile("mov %%cr2, %0" : "=r"(addr));
        if (paging_handle_fault(addr, r->err_code)) return;
    }

    console_set_color(COL_WHITE_ON_RED);
    puts("\n*** CPU EXCEPTION ***\n");
    console_set_color(COL_RED);
    puts(exception_names[r->int_no & 31]);
    puts("  (vector "); put_hex(r->int_no); puts(", error code "); put_hex(r->err_code); puts(")\n");

    console_set_color(COL_GREY);
    reg("RIP", r->rip); reg("CS", r->cs); reg("RFLAGS", r->rflags); puts("\n");
    reg("RAX", r->rax); reg("RBX", r->rbx); reg("RCX", r->rcx); reg("RDX", r->rdx); puts("\n");
    reg("RSI", r->rsi); reg("RDI", r->rdi); reg("RBP", r->rbp); reg("RSP", r->rsp); puts("\n");

    if (r->int_no == 14) {                      /* page fault: faulting address is in CR2 */
        u64 cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        reg("CR2", cr2); puts("\n");
        console_set_color(COL_YELLOW);
        puts((r->err_code & 1) ? "protection violation" : "page not present");
        puts(", while ");
        puts((r->err_code & 16) ? "fetching an instruction" : (r->err_code & 2) ? "writing" : "reading");
        puts((r->err_code & 4) ? " (user mode)" : " (kernel mode)");
        if (r->err_code & 8) puts(", reserved bit set");
        puts("\n");
    }

    console_set_color(COL_RED);
    puts("System halted.\n");
    for (;;) __asm__ volatile("cli; hlt");
}
