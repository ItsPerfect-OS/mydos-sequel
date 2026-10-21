; stage1.asm - MyDOS Sequel boot sector (512 bytes, loaded by BIOS at 0x7C00)
; Loads STAGE2_SECTORS sectors (LBA 1..) to 0x0000:0x7E00 and jumps there.
[bits 16]
[org 0x7C00]

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 4
%endif
STAGE2_LOAD equ 0x7E00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl
    call get_geometry

    mov si, msg_stage1
    call print

    mov word [lba], 1
    mov cx, STAGE2_SECTORS
    mov bx, STAGE2_LOAD
.load:
    push cx
    mov ax, [lba]
    call read_sector
    pop cx
    add bx, 512
    inc word [lba]
    loop .load

    mov dl, [boot_drive]
    jmp 0x0000:STAGE2_LOAD

%include "boot/common16.inc"

msg_stage1 db "MyDOS Sequel: stage 1", 13, 10, 0
boot_drive db 0
lba        dw 0
drive_spt   dw 18          ; defaults = 1.44MB floppy; get_geometry overrides
drive_heads dw 2

times 510-($-$$) db 0
dw 0xAA55
