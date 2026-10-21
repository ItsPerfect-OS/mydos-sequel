/* kernel.c - MyDOS Sequel, milestone 5a: user-mode programs */
#include "console.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "shell.h"
#include "memmap.h"
#include "pmm.h"
#include "heap.h"
#include "paging.h"
#include "fscmds.h"
#include "gdt.h"

void kernel_main(void) {
    console_init();
    console_set_color(COL_CYAN);  puts("MyDOS Sequel\n");
    console_set_color(COL_GREY);  puts("Booted via BIOS -> 64-bit long mode.\n");

    gdt_init();                               /* own GDT: adds ring-3 segments and the TSS */
    idt_init();
    console_set_color(COL_GREEN); puts("IDT loaded: 32 CPU exception handlers installed.\n");

    __asm__ volatile("int3");               /* breakpoint test: handler must return */

#ifdef CRASH_TEST
    console_set_color(COL_GREY);  puts("CRASH_TEST: dividing by zero...\n");
    __asm__ volatile("xor %%edx, %%edx; mov $1, %%eax; xor %%ecx, %%ecx; div %%ecx"
                     ::: "eax", "ecx", "edx");
#endif

    memmap_init();
    console_set_color(memmap_is_fallback() ? COL_YELLOW : COL_GREEN);
    puts(memmap_is_fallback() ? "Memory: BIOS gave no E820 map, assuming 15 MB.\n" : "Memory map: ");
    if (!memmap_is_fallback()) {
        put_dec((u32)memmap_count()); puts(" E820 entries, ");
        put_dec((u32)(memmap_usable_bytes() >> 20)); puts(" MB usable.\n");
    }

    pmm_init();
    console_set_color(COL_GREEN);
    puts("PMM: "); put_dec(pmm_free_frames()); puts(" free frames ("); put_dec(pmm_free_frames() / 256);
    puts(" MB) above 1 MB.\n");

    if (heap_init()) {
        console_set_color(COL_GREEN);
        puts("Heap: kmalloc/kfree ready (256 KB initial chunk).\n");
    } else {
        console_set_color(COL_YELLOW);
        puts("Heap: could not get 256 KB of contiguous RAM - kmalloc will return NULL.\n");
    }

    if (paging_init()) {
        console_set_color(COL_GREEN);
        puts("Paging: enabled (4-level, identity map, NULL page unmapped, kernel code read-only).\n");
    } else {
        console_set_color(COL_YELLOW);
        puts("Paging: could not be enabled (not enough memory for page tables).\n");
    }

    pic_init();
    timer_init(100);
    int ps2 = keyboard_init();
    int com = serial_input_init();
    __asm__ volatile("sti");
    console_set_color(COL_GREEN); puts("PIC and PIT (100 Hz) ready.\n");
    console_set_color(ps2 ? COL_GREEN : COL_YELLOW);
    puts(ps2 ? "Input: PS/2 keyboard found.\n" : "Input: no PS/2 keyboard controller on this machine.\n");
    console_set_color(com ? COL_GREEN : COL_YELLOW);
    puts(com ? "Input: COM1 serial console ready.\n" : "Input: no COM1 serial port.\n");
    if (!ps2 && com) {
        console_set_color(COL_YELLOW);
        puts("  -> Type in the serial console (UTM: add a Serial device, Mode: Built-In Terminal).\n");
    }
    putc('\n');

    fs_autostart();
    putc('\n');

    console_set_color(COL_GREY);  puts("Type 'help' for a list of commands.\n");
    shell_run();
}
