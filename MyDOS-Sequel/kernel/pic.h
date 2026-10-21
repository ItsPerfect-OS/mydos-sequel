#ifndef PIC_H
#define PIC_H
#include "types.h"

#define IRQ_BASE 32                 /* IRQ0 -> vector 32 (vectors 0-31 are CPU exceptions) */

void pic_init(void);                /* remap both PICs, mask every IRQ */
void pic_mask(u8 irq);
void pic_unmask(u8 irq);
void pic_send_eoi(u8 irq);
int  pic_is_spurious(u8 irq);       /* IRQ7/IRQ15 only; handles the master EOI for a spurious IRQ15 */
#endif
