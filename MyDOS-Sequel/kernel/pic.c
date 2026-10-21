/* pic.c - 8259 PIC: remap, mask/unmask, EOI */
#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

static void io_wait(void) { outb(0x80, 0); }   /* unused port: ~1us delay */

void pic_init(void) {
    outb(PIC1_CMD, 0x11);  io_wait();          /* ICW1: init, expect ICW4 */
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, IRQ_BASE);      io_wait(); /* ICW2: vector offsets */
    outb(PIC2_DATA, IRQ_BASE + 8);  io_wait();
    outb(PIC1_DATA, 0x04); io_wait();          /* ICW3: slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();          /*       slave cascade identity */
    outb(PIC1_DATA, 0x01); io_wait();          /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFF);                     /* mask everything for now */
    outb(PIC2_DATA, 0xFF);
}

void pic_mask(u8 irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) | (1 << (irq & 7)));
}

void pic_unmask(u8 irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1 << (irq & 7)));
    if (irq >= 8) pic_unmask(2);               /* slave PIC hangs off IRQ2 */
}

void pic_send_eoi(u8 irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

int pic_is_spurious(u8 irq) {
    if (irq != 7 && irq != 15) return 0;
    u16 cmd = (irq == 7) ? PIC1_CMD : PIC2_CMD;
    outb(cmd, 0x0B);                           /* OCW3: read In-Service Register */
    if (inb(cmd) & 0x80) return 0;             /* real IRQ */
    if (irq == 15) outb(PIC1_CMD, PIC_EOI);    /* master saw IRQ2 for real, slave didn't */
    return 1;
}
