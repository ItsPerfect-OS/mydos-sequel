ENTRY(_start)
SECTIONS
{
    . = 0x100000;
    __ro_start = .;
    .text   : { *(.text.entry) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    __ro_end = .;
    .data   : { *(.data .data.*) }
    __bss_start = .;
    .bss    : { *(.bss .bss.*) *(COMMON) }
    __bss_end = .;
    __kernel_end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
