/* keyboard.c - input drivers: PS/2 keyboard (IRQ1) and COM1 serial (IRQ4) -> ring buffer */
#include "keyboard.h"
#include "idt.h"
#include "io.h"
#include "process.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define COM1       0x3F8
#define BUF_SIZE   128

/* Scancode set 1 (what the 8042 delivers by default), indexed by scancode. */
static const char map_normal[] =
    "\0\033" "1234567890-=" "\b\t" "qwertyuiop[]" "\n\0" "asdfghjkl;'`" "\0\\"
    "zxcvbnm,./" "\0*\0 \0";
static const char map_shift[] =
    "\0\033" "!@#$%^&*()_+" "\b\t" "QWERTYUIOP{}" "\n\0" "ASDFGHJKL:\"~" "\0|"
    "ZXCVBNM<>?" "\0*\0 \0";

static volatile char buf[BUF_SIZE];
static volatile u32 head, tail;
static int shift, ctrl, caps, extended;

static void push(char c) {
    if (c == 3 && process_running()) process_request_kill();      /* Ctrl+C stops the running program */
    u32 next = (head + 1) % BUF_SIZE;
    if (next == tail) return;               /* buffer full: drop the key */
    buf[head] = c;
    head = next;
}

/* ---------------- PS/2 keyboard ---------------- */

static void kbd_irq(struct regs *r) {
    (void)r;
    u8 sc = inb(KBD_DATA);

    if (sc == 0xE0) { extended = 1; return; }
    int release = sc & 0x80;
    sc &= 0x7F;
    if (extended) { extended = 0; return; } /* arrows, Del, etc.: not handled yet */

    switch (sc) {
        case 0x2A: case 0x36: shift = !release; return;
        case 0x1D:            ctrl  = !release; return;
        case 0x3A:            if (!release) caps ^= 1; return;
    }
    if (release || sc >= sizeof(map_normal) - 1) return;

    char c = map_normal[sc];
    if (c >= 'a' && c <= 'z') {
        if (ctrl) c = c - 'a' + 1;          /* Ctrl+A..Z -> 1..26 (Ctrl+C = 3) */
        else if (shift ^ caps) c -= 32;
    } else if (shift) {
        c = map_shift[sc];
    }
    if (c) push(c);
}

int keyboard_init(void) {
    /* A missing controller reads back 0xFF on every port: don't touch it (and never
       spin on its status bits, which would be "full" forever). */
    if (inb(KBD_STATUS) == 0xFF) return 0;
    for (int i = 0; i < 16 && (inb(KBD_STATUS) & 1); i++) (void)inb(KBD_DATA);   /* drain, bounded */
    irq_install_handler(1, kbd_irq);                                              /* also unmasks IRQ1 */
    return 1;
}

/* ---------------- COM1 serial input ---------------- */

static void serial_irq(struct regs *r) {
    static int esc, prev_cr;
    (void)r;
    for (int n = 0; n < 16 && (inb(COM1 + 5) & 1); n++) {
        u8 c = inb(COM1);

        if (esc == 1) { esc = (c == '[' || c == 'O') ? 2 : 0; continue; }   /* swallow ESC [ x sequences */
        if (esc == 2) { if (c >= 0x40 && c <= 0x7E) esc = 0; continue; }
        if (c == 0x1B) { esc = 1; continue; }

        if (c == '\r') { prev_cr = 1; push('\n'); continue; }
        if (c == '\n') { if (!prev_cr) push('\n'); prev_cr = 0; continue; }
        prev_cr = 0;
        if (c == 0x7F) c = '\b';
        if (c == '\b' || c == 3 || (c >= 32 && c < 127)) push((char)c);
    }
}

int serial_input_init(void) {
    outb(COM1 + 7, 0xAE);                                    /* scratch register: is a UART really there? */
    if (inb(COM1 + 7) != 0xAE) return 0;
    for (int i = 0; i < 16 && (inb(COM1 + 5) & 1); i++) (void)inb(COM1);
    outb(COM1 + 1, 0x01);                                    /* IER: interrupt on received data */
    irq_install_handler(4, serial_irq);
    return 1;
}

/* ---------------- consumer side ---------------- */

int keyboard_has_key(void) { return head != tail; }

void keyboard_flush(void) { __asm__ volatile("cli"); tail = head; __asm__ volatile("sti"); }

char keyboard_getc(void) {
    for (;;) {
        __asm__ volatile("cli");
        if (head != tail) {
            char c = buf[tail];
            tail = (tail + 1) % BUF_SIZE;
            __asm__ volatile("sti");
            return c;
        }
        __asm__ volatile("sti; hlt");       /* sti takes effect after hlt starts: no lost wakeup */
    }
}
