; isr.asm (x86-64) - CPU exception stubs, hardware IRQ stubs, the int 0x80 stub, and the common handler
; Exceptions 8, 10-14, 17, 21, 30 push an error code themselves; for the rest we push a dummy 0 so the
; frame layout (struct regs) is always the same. In long mode the CPU always pushes rip, cs, rflags, rsp, ss.
[bits 64]
extern isr_handler

section .text
%assign i 0
%rep 32
isr %+ i:
%if (i == 8) || (i >= 10 && i <= 14) || (i == 17) || (i == 21) || (i == 30)
    ; CPU already pushed the error code
%else
    push qword 0
%endif
    push qword i
    jmp isr_common
%assign i i+1
%endrep

; ---- 16 hardware IRQ stubs: vectors 32..47 (no error code) ----
%assign i 0
%rep 16
irq %+ i:
    push qword 0
    push qword (32 + i)
    jmp isr_common
%assign i i+1
%endrep

; ---- int 0x80: system calls from user programs ----
global isr_syscall_stub
isr_syscall_stub:
    push qword 0
    push qword 0x80
    jmp isr_common

isr_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp                    ; argument: struct regs *
    cld
    call isr_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16                     ; drop int_no + err_code
    iretq

section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 32
    dq isr %+ i
%assign i i+1
%endrep

global irq_stub_table
irq_stub_table:
%assign i 0
%rep 16
    dq irq %+ i
%assign i i+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
