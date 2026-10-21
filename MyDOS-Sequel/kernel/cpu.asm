; cpu.asm - low-level CPU helpers (x86-64): GDT reload, kernel context save/restore, ring-3 entry
[bits 64]
section .text

; void gdt_flush(struct gdt_ptr *p): load the new GDT, reload segment registers, load the TSS
global gdt_flush
gdt_flush:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    push qword 0x08                 ; far return reloads CS (64-bit code segment)
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    mov ax, 0x28                    ; TSS selector
    ltr ax
    ret

; int kctx_save(struct kctx *c): like setjmp. Returns 0 now, and again with the value given to kctx_restore.
; struct kctx { u64 rbx, rbp, r12, r13, r14, r15, rsp, rip; }
global kctx_save
kctx_save:
    mov [rdi + 0],  rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rax, [rsp + 8]              ; rsp as it will be after our `ret`
    mov [rdi + 48], rax
    mov rax, [rsp]                  ; return address
    mov [rdi + 56], rax
    xor eax, eax
    ret

; void kctx_restore(struct kctx *c, int value): like longjmp
global kctx_restore
kctx_restore:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    mov eax, esi
    jmp qword [rdi + 56]

; void enter_user(u64 rip, u64 rsp, u64 argc, u64 argv): drop to ring 3 and never come back directly.
; Programs start with rdi = argc and rsi = argv (the System V convention for main).
global enter_user
enter_user:
    cli
    mov ax, 0x1B                    ; user data selector (ring 3)
    mov ds, ax
    mov es, ax
    push qword 0x1B                 ; ss
    push rsi                        ; rsp
    push qword 0x202                ; rflags: IF set
    push qword 0x23                 ; cs: 64-bit user code selector (ring 3)
    push rdi                        ; rip
    mov rdi, rdx                    ; argc
    mov rsi, rcx                    ; argv
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
