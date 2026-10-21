; common16.inc - shared 16-bit helpers for stage 1 and stage 2
; Requires the including file to define: boot_drive (db), drive_spt (dw), drive_heads (dw)

; print: DS:SI = zero-terminated string
print:
    pusha
    mov ah, 0x0E
    xor bh, bh
.next:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .next
.done:
    popa
    ret

; get_geometry: ask the BIOS for the boot drive's CHS geometry (INT 13h AH=08).
; Floppies are 18 sectors x 2 heads, but a hard disk / USB disk gets a different
; translation (e.g. 63 sectors x 16 heads). Falls back to the defaults on failure.
; Requires: boot_drive (db), drive_spt (dw, default 18), drive_heads (dw, default 2), DS=0
get_geometry:
    pusha
    push es
    mov dl, [boot_drive]
    xor di, di
    mov es, di                  ; guard against BIOS bugs (ES:DI is clobbered)
    mov ah, 0x08
    int 0x13
    jc .done
    and cx, 0x003F              ; CL bits 0-5 = sectors per track
    jz .done
    mov [drive_spt], cx
    mov al, dh                  ; DH = highest head number
    xor ah, ah
    inc ax
    mov [drive_heads], ax
.done:
    pop es
    popa
    ret

; read_sector: AX = LBA, ES:BX = destination (one 512-byte sector)
; Converts LBA -> CHS using the geometry from get_geometry. One sector per call,
; so we never cross a track boundary (INT 13h can't).
read_sector:
    pusha
    xor dx, dx
    div word [drive_spt]        ; AX = LBA / spt,  DX = LBA % spt
    mov cl, dl
    inc cl                      ; CL = sector (1-based)
    xor dx, dx
    div word [drive_heads]      ; AX = cylinder,  DX = head
    mov dh, dl                  ; DH = head
    mov ch, al                  ; CH = cylinder low 8 bits
    shl ah, 6
    or  cl, ah                  ; CL bits 6-7 = cylinder bits 8-9
    mov dl, [boot_drive]
    mov si, 3                   ; retries
.retry:
    push cx
    push dx
    mov ax, 0x0201          ; read 1 sector
    int 0x13
    pop dx
    pop cx
    jnc .ok
    push cx
    push dx
    xor ax, ax              ; reset disk
    int 0x13
    pop dx
    pop cx
    dec si
    jnz .retry
    jmp disk_error
.ok:
    popa
    ret

disk_error:
    mov si, msg_disk_error
    call print
.hang:
    cli
    hlt
    jmp .hang

msg_disk_error db "Disk read error!", 13, 10, 0
