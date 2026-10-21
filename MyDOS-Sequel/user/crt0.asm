; crt0.asm - program entry (x86-64): the kernel starts us with rdi = argc, rsi = argv
[bits 64]
section .text.start
global _start
extern main
extern exit
_start:
    xor ebp, ebp
    and rsp, -16            ; the ABI wants 16-byte alignment at the call
    call main               ; main(argc, argv): rdi and rsi are already set
    mov edi, eax
    call exit
.hang:
    jmp .hang
section .note.GNU-stack noalloc noexec nowrite progbits
