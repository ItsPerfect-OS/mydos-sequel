# MyDOS Sequel

A from-scratch **64-bit (x86-64)** BIOS operating system: two-stage bootloader that switches the CPU into long mode, C kernel,
interrupts, timer, keyboard + serial shell, physical memory manager, heap, 4-level paging, the
**RPFS Sequel** filesystem, and a loader that runs **64-bit .exe programs in user mode (ring 3)**.

## Build (macOS, Intel)

    brew install nasm llvm
    export PATH="/usr/local/opt/llvm/bin:$PATH"
    make clean && make                                      # -> build/mydos.img (1.44 MB floppy image)

The Makefile finds your linker/objcopy/compiler itself (Homebrew LLVM or i686-elf-binutils); override with `make LD=... OBJCOPY=... CC=...`. `make run` needs QEMU.

## UTM setup

* **Architecture: x86_64** (not i386 -- a 32-bit-only VM has no 64-bit mode; the bootloader will say so).
  Boot disk: `build/mydos.img` as a **Floppy** drive. Machine: Standard PC (i440FX + PIIX), UEFI off. Use Emulate.
* Add a **Serial** device (Mode: Built-In Terminal) -- you type in that window if the VM has no PS/2 keyboard, Or go to QEMU settings, and check **Force PS/2 Controller**
* Data disk for RPFS (optional but it is what makes files persist): add a second drive, Interface **IDE**.
  Either create a blank one (tick "Raw Image" if you want to inspect it from the Mac) and `format hda` inside
  MyDOS, or import an image made with `tools/rpfs.py mkfs`. UTM copies imported images into the VM package.
  Without a data disk, MyDOS formats a 2 MB RAM disk (`ram0`) at boot -- fully usable, lost at reboot.
* The ATA driver speaks legacy IDE, so use the i440FX machine. Q35 exposes SATA/AHCI, which it cannot see.

## RPFS Sequel

* 1 KB blocks. Layout: superblock | block bitmap | inode table | data. 64-byte inodes with 12 direct
  pointers + 1 indirect block => files up to 268 KB. Directories are arrays of 32-byte entries
  (`.` and `..` included); names up to 27 bytes. Timestamps come from the CMOS clock (UTC).
* Write-through: every change reaches the disk immediately; `fsck` cross-checks inodes, directories,
  reachability, the block bitmap and the superblock counters.
* Formats: `kernel/rpfs.h` documents the exact on-disk structures.

Shell: `disks format mount umount ls cat write append mkdir rm rmdir mv cp cd pwd df fsck fstest`.
`write notes.txt Hello\nWorld` stores two lines (`\n`, `\t`, `\\` are understood). `format` refuses disks
with a boot signature unless you pass `-f`. `mv` and `cp` also accept a directory as destination;
`mv` moves files only (directories can't be renamed yet).

## Programs (.exe)

MyDOS Sequel has its own executable format (not Windows PE): a 40-byte header plus one flat image (`kernel/process.h`, format version 2 = x86-64;
older 32-bit version-1 files are refused).
Programs are x86-64, run in ring 3 in a private address space (code at `0x8000000000`), talk to the kernel with `int 0x80`, and
cannot touch kernel memory, ports or privileged instructions -- a crash only kills the program. One program runs at a
time; Ctrl+C stops it.

    hello                 run ./hello.exe, or /bin/hello.exe (the .exe is optional)
    run primes 60         same thing, explicitly
    install               copy the built-in demo programs into /bin of the mounted disk

(Built-in shell commands win over programs of the same name; use `run cat` for the program.)
Demo programs in `user/`: `hello cat cp primes guess count segv spin brk evil`. At boot the RAM disk gets all of them in
`/bin`; `make disk` builds `build/data.img`, an RPFS disk with them already installed.

Write your own: add `user/foo.c` (`#include "mdos.h"`; `int main(int argc, char **argv)`), add `foo` to `USER_PROGS`
in the Makefile, and `make`. The library gives you `printf print putchar getchar getline open read write close
sleep_ms uptime_ms sbrk exit` plus a few string helpers. `tools/mkexe.py` converts the linked ELF to `.exe`.

System calls (rax = number; rdi, rsi, rdx = arguments; result in rax): 0 exit, 1 write, 2 getchar, 3 open
(0 read / 1 write+truncate / 2 append), 4 close, 5 read, 6 uptime_ms, 7 sleep, 8 sbrk. Pointers from programs are
validated page by page before the kernel touches them.

## Host tool

    tools/rpfs.py mkfs data.img 16 --label MYDISK --put notes.txt:/docs/notes.txt
    tools/rpfs.py tree|ls|cat|check data.img      # independent implementation of the format

## Self-tests (run them in the shell)

`pmmtest heaptest pagetest fstest proctest` -- frame allocator, heap, paging, filesystem, program loader (includes hostile programs). `crash null|ro|unmapped|div0|ud`
triggers a CPU fault on purpose (the system halts; restart the VM).

## How the 64-bit boot works

Stage 1 (boot sector) loads stage 2, which: checks CPUID for long mode; loads the kernel with the BIOS into 0x10000
(real mode can't reach higher); collects the E820 memory map; enters 32-bit protected mode and copies the kernel to
1 MB; builds a minimal identity map (first 1 GB, 2 MB pages); enables PAE, long mode (EFER.LME) and paging; and jumps to
the 64-bit kernel at 0x100000. The kernel then builds its own page tables covering all RAM, installs a 64-bit GDT/TSS and
IDT (16-byte gates; the double-fault handler has its own stack), and sizes the physical-memory bitmap from the memory map
(up to 64 GB). The kernel binary must stay under 440 KB (the BIOS-loaded area); the Makefile checks.
