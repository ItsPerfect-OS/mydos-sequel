; stage2.asm - MyDOS Sequel second stage (loaded at 0x7E00)
; 1. checks that the CPU has 64-bit mode (halts with a message if not)
; 2. loads the kernel (flat binary) to 0x10000 with the BIOS, one sector at a time
; 3. collects the E820 memory map
; 4. enables A20, enters 32-bit protected mode, copies the kernel up to 1 MB
; 5. builds a minimal identity map (first 1 GB, 2 MB pages), enables PAE + long mode + paging
; 6. jumps into the 64-bit kernel at 0x100000
[bits 16]
[org 0x7E00]

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 4
%endif
%ifndef KERNEL_SECTORS
%error "KERNEL_SECTORS must be defined (-DKERNEL_SECTORS=n)"
%endif

; E820 memory map handed to the kernel in low memory (free conventional RAM):
;   0x500        dword  entry count
;   0x504 + 24*i entries: u64 base, u64 length, u32 type, u32 ACPI ext attrs
E820_COUNT   equ 0x500
E820_ENTRIES equ 0x504
E820_MAX     equ 32

KERNEL_SEG  equ 0x1000              ; 0x1000:0000 = physical 0x10000: where the BIOS can put it
KERNEL_LBA  equ 1 + STAGE2_SECTORS
KERNEL_LOAD equ 0x10000
KERNEL_RUN  equ 0x100000            ; where the 64-bit kernel is linked and runs

; boot page tables (replaced by the kernel's own once it is running): 0x1000 PML4, 0x2000 PDPT, 0x3000 PD
BOOT_PML4   equ 0x1000
BOOT_PDPT   equ 0x2000
BOOT_PD     equ 0x3000

stage2_start:
    mov [boot_drive], dl
    xor ax, ax
    mov ds, ax
    call get_geometry

    mov si, msg_stage2
    call print
    call check_long_mode            ; does not return on a 32-bit-only CPU

    ; ---- load kernel ----
    mov ax, KERNEL_SEG
    mov es, ax
    xor bx, bx
    mov word [lba], KERNEL_LBA
    mov cx, KERNEL_SECTORS
.load:
    push cx
    mov ax, [lba]
    call read_sector
    pop cx
    inc word [lba]
    mov ax, es
    add ax, 0x20                    ; advance 512 bytes via the segment
    mov es, ax
    loop .load
    xor ax, ax
    mov es, ax

    call detect_memory

    mov si, msg_pm
    call print

    ; ---- A20 (fast gate) ----
    in al, 0x92
    or al, 2
    and al, 0xFE
    out 0x92, al

    ; ---- protected mode ----
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

%include "boot/common16.inc"

; check_long_mode: CPUID must exist and report the "long mode" bit, else print why and stop.
check_long_mode:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 0x200000               ; try to flip the ID flag: only CPUID-capable CPUs allow it
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    jz .no
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29               ; LM bit
    jz .no
    ret
.no:
    mov si, msg_no64
    call print
.hang:
    cli
    hlt
    jmp .hang

; detect_memory: INT 15h EAX=E820h loop -> table at E820_COUNT / E820_ENTRIES.
; On a BIOS without E820 the count stays 0 and the kernel falls back.
detect_memory:
    pushad
    xor ax, ax
    mov es, ax
    mov dword [E820_COUNT], 0
    mov di, E820_ENTRIES
    xor ebx, ebx                    ; continuation value, 0 = start
    xor bp, bp                      ; entries stored so far
.next:
    mov dword [es:di + 20], 1       ; pre-set "ACPI 3.0 enabled" in case the BIOS only writes 20 bytes
    mov eax, 0xE820
    mov edx, 0x534D4150             ; 'SMAP'
    mov ecx, 24
    int 0x15
    jc .done                        ; CF = unsupported, or end of list reached
    cmp eax, 0x534D4150
    jne .done
    jcxz .skip                      ; BIOS returned 0 bytes
    mov eax, [es:di + 8]
    or  eax, [es:di + 12]           ; skip zero-length entries
    jz .skip
    inc bp
    add di, 24
    cmp bp, E820_MAX
    jae .done
.skip:
    test ebx, ebx                   ; ebx = 0 -> that was the last entry
    jnz .next
.done:
    movzx eax, bp
    mov [E820_COUNT], eax
    popad
    ret

[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7000

    ; copy the kernel from the BIOS-reachable area up to 1 MB
    cld
    mov esi, KERNEL_LOAD
    mov edi, KERNEL_RUN
    mov ecx, (KERNEL_SECTORS * 512 + 3) / 4
    rep movsd

    ; boot page tables: identity-map the first 1 GB with 2 MB pages
    mov edi, BOOT_PML4
    xor eax, eax
    mov ecx, 0x3000 / 4
    rep stosd
    mov dword [BOOT_PML4], BOOT_PDPT | 3
    mov dword [BOOT_PDPT], BOOT_PD | 3
    mov edi, BOOT_PD
    mov eax, 0x83                   ; present | writable | 2 MB page
    mov ecx, 512
.fill:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .fill

    ; PAE -> page tables -> long mode enable -> paging
    mov eax, cr4
    or eax, 0x20                    ; PAE
    mov cr4, eax
    mov eax, BOOT_PML4
    mov cr3, eax
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or eax, 0x100                   ; LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000000              ; PG
    mov cr0, eax
    jmp 0x18:long_entry             ; far jump into the 64-bit code segment

[bits 64]
long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rax, KERNEL_RUN
    jmp rax

[bits 16]
align 8
gdt_start:
    dq 0                            ; null
    dq 0x00CF9A000000FFFF           ; 0x08 code: base 0, 4GB, ring 0
    dq 0x00CF92000000FFFF           ; 0x10 data: base 0, 4GB, ring 0
    dq 0x00AF9A000000FFFF           ; 0x18 64-bit code (L bit): ring 0
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

msg_stage2 db "Stage 2: loading kernel...", 13, 10, 0
msg_pm     db "Entering 64-bit mode...", 13, 10, 0
msg_no64   db "This CPU has no 64-bit mode. In UTM use an x86_64 VM, not i386.", 13, 10, 0
boot_drive db 0
lba        dw 0
drive_spt   dw 18
drive_heads dw 2

times (STAGE2_SECTORS*512)-($-$$) db 0   ; errors out if stage 2 outgrows its slot
