/* timer.c - 8253/8254 PIT as the system tick */
#include "timer.h"
#include "idt.h"
#include "io.h"

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_BASE  1193182u

static volatile u32 ticks;
static u32 freq;

static void timer_irq(struct regs *r) { (void)r; ticks++; }

void timer_init(u32 hz) {
    freq = hz;
    u32 div = PIT_BASE / hz;
    outb(PIT_CMD, 0x36);                       /* ch0, lobyte/hibyte, mode 3 (square wave) */
    outb(PIT_CH0, div & 0xFF);
    outb(PIT_CH0, (div >> 8) & 0xFF);
    irq_install_handler(0, timer_irq);         /* also unmasks IRQ0 */
}

u32 timer_ticks(void) { return ticks; }
u32 timer_hz(void)    { return freq; }

void timer_sleep_ms(u32 ms) {
    u32 target = ticks + (ms * freq) / 1000;
    while ((int)(ticks - target) < 0) __asm__ volatile("hlt");
}
