; entry.asm - 64-bit kernel entry: stage 2 jumps here in long mode, identity-mapped, interrupts off
[bits 64]
section .text.entry
global _start
extern kernel_main
extern __bss_start
extern __bss_end

_start:
    cli
    cld
    mov rsp, boot_stack_top
    lea rdi, [rel __bss_start]          ; the flat binary has no .bss image: zero it
    lea rcx, [rel __bss_end]
    sub rcx, rdi
    xor eax, eax
    rep stosb
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
boot_stack:  resb 32768
boot_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
