#ifndef IDT_H
#define IDT_H
#include "types.h"

/* Register frame built by isr.asm (lowest address first). In long mode the CPU always pushes
 * rip, cs, rflags, rsp and ss, whether or not the privilege level changes. */
struct regs {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 int_no, err_code;                    /* pushed by the stub / by the CPU */
    u64 rip, cs, rflags, rsp, ss;            /* pushed by the CPU */
};

typedef void (*irq_handler_t)(struct regs *);

void idt_init(void);
const char *idt_exception_name(u64 vector);
void irq_install_handler(int irq, irq_handler_t handler);   /* registers + unmasks the IRQ */
#endif
