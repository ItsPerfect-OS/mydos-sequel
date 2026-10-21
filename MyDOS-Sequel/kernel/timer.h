#ifndef TIMER_H
#define TIMER_H
#include "types.h"
void timer_init(u32 hz);            /* PIT channel 0 periodic interrupt (IRQ0) */
u32  timer_ticks(void);
u32  timer_hz(void);
void timer_sleep_ms(u32 ms);        /* needs interrupts enabled */
#endif
