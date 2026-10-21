# MyDOS Sequel - build with:  make        run in QEMU:  make run
# On the Mac:  brew install nasm llvm        (i686-elf-binutils optional; the Makefile finds whichever tools you have)
# Tool selection: use whatever is installed. Override any of them on the command line, e.g. make LD=...
#   linker/objcopy: i686-elf-binutils if present, otherwise LLVM's (brew install llvm)
#   compiler:       Homebrew's clang if present, otherwise clang from the PATH
LLVM_BIN := $(firstword $(wildcard /usr/local/opt/llvm/bin /opt/homebrew/opt/llvm/bin))
find     = $(firstword $(foreach t,$(1),$(wildcard $(LLVM_BIN)/$(t)) $(shell command -v $(t) 2>/dev/null)))
CC      = $(or $(call find,clang),clang)
LD      = $(or $(call find,i686-elf-ld ld.lld),i686-elf-ld)
OBJCOPY = $(or $(call find,i686-elf-objcopy llvm-objcopy),i686-elf-objcopy)
NASM    ?= nasm
QEMU    ?= qemu-system-x86_64

STAGE2_SECTORS := 4
# 64-bit freestanding kernel: no red zone (interrupt handlers would corrupt it), no SSE/FPU registers.
CFLAGS  := --target=x86_64-elf -m64 -mcmodel=small -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pic -fno-pie -fno-asynchronous-unwind-tables -mno-red-zone -mgeneral-regs-only -nostdlib -O2 -Wall -Wextra \
           -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast

B := build
CFLAGS += $(if $(CRASH),-DCRASH_TEST)

C_SRC   := $(wildcard kernel/*.c)
ASM_SRC := $(wildcard kernel/*.asm)
OBJS    := $(patsubst kernel/%.asm,$(B)/%.asm.o,$(ASM_SRC)) \
           $(patsubst kernel/%.c,$(B)/%.c.o,$(C_SRC))

PYTHON ?= python3
USER_PROGS := hello cat cp primes guess count segv spin brk evil
USER_EXES  := $(patsubst %,$(B)/user/%.exe,$(USER_PROGS))
UCFLAGS    := $(subst -mcmodel=small,-mcmodel=large,$(CFLAGS)) -Iuser

all: $(B)/mydos.img

$(B):
	mkdir -p $(B)

$(B)/%.asm.o: kernel/%.asm | $(B)
	$(NASM) -f elf64 $< -o $@

$(B)/%.c.o: kernel/%.c $(wildcard kernel/*.h) | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- user programs: C -> ELF (linked at USER_BASE) -> .exe -> embedded in the kernel ----
$(B)/user:
	mkdir -p $(B)/user

$(B)/user/crt0.o: user/crt0.asm | $(B)/user
	$(NASM) -f elf64 $< -o $@

$(B)/user/%.o: user/%.c user/mdos.h | $(B)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(B)/user/%.elf: $(B)/user/%.o $(B)/user/mdos.o $(B)/user/crt0.o user/user.ld
	$(LD) -m elf_x86_64 -N -T user/user.ld -o $@ $(B)/user/crt0.o $< $(B)/user/mdos.o

$(B)/user/%.exe: $(B)/user/%.elf tools/mkexe.py
	$(PYTHON) tools/mkexe.py $< $@

$(B)/embedded.c: $(USER_EXES) tools/embed.py | $(B)
	$(PYTHON) tools/embed.py $@ $(foreach p,$(USER_PROGS),/bin/$(p).exe=$(B)/user/$(p).exe)

$(B)/embedded.c.o: $(B)/embedded.c kernel/embedded.h kernel/rpfs.h
	$(CC) $(CFLAGS) -Ikernel -c $< -o $@

OBJS += $(B)/embedded.c.o

.SECONDARY:

$(B)/kernel.elf: $(OBJS) kernel/linker.ld
	$(LD) -m elf_x86_64 -T kernel/linker.ld -o $@ $(OBJS)

$(B)/kernel.bin: $(B)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@test $$(wc -c < $@) -le 450560 || { echo "kernel.bin is bigger than the 440 KB stage 2 can load"; exit 1; }

$(B)/stage1.bin: boot/stage1.asm boot/common16.inc | $(B)
	$(NASM) -f bin -I . -DSTAGE2_SECTORS=$(STAGE2_SECTORS) $< -o $@

$(B)/stage2.bin: boot/stage2.asm boot/common16.inc $(B)/kernel.bin | $(B)
	$(NASM) -f bin -I . -DSTAGE2_SECTORS=$(STAGE2_SECTORS) \
	  -DKERNEL_SECTORS=$$(( ($$(wc -c < $(B)/kernel.bin) + 511) / 512 )) $< -o $@

# 1.44MB floppy: [stage1][stage2 x4][kernel...]
$(B)/mydos.img: $(B)/stage1.bin $(B)/stage2.bin $(B)/kernel.bin
	dd if=/dev/zero of=$@ bs=512 count=2880 2>/dev/null
	dd if=$(B)/stage1.bin of=$@ bs=512 conv=notrunc 2>/dev/null
	dd if=$(B)/stage2.bin of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null
	dd if=$(B)/kernel.bin of=$@ bs=512 seek=$$(( 1 + $(STAGE2_SECTORS) )) conv=notrunc 2>/dev/null
	@echo "Built $@"

# Ready-made RPFS data disk (8 MB) with the demo programs in /bin: import it into UTM as an IDE drive.
disk: $(USER_EXES)
	$(PYTHON) tools/rpfs.py mkfs $(B)/data.img 8 --label MYDISK $(foreach p,$(USER_PROGS),--put $(B)/user/$(p).exe:/bin/$(p).exe)

run: $(B)/mydos.img
	$(QEMU) -drive format=raw,file=$<,if=floppy

clean:
	rm -rf $(B)

.PHONY: all run clean disk
