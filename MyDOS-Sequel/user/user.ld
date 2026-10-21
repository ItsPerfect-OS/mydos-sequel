/* Linker script for MyDOS Sequel programs: one contiguous image at USER_BASE (512 GB). */
ENTRY(_start)
SECTIONS
{
    . = 0x8000000000;
    .text   : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
