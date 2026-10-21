#ifndef KEYBOARD_H
#define KEYBOARD_H
/* Input sources, all feeding one ring buffer read by keyboard_getc(). */
int  keyboard_init(void);       /* PS/2 keyboard on IRQ1; returns 1 if a controller exists, else 0 */
int  serial_input_init(void);   /* COM1 receive interrupt (IRQ4); returns 1 if a UART exists, else 0 */
char keyboard_getc(void);       /* blocking; sleeps with hlt until a character arrives */
int  keyboard_has_key(void);
void keyboard_flush(void);       /* drop everything typed but not yet read */
#endif
