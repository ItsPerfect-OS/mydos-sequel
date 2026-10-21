#ifndef PROCESS_H
#define PROCESS_H
#include "types.h"
#include "idt.h"
#include "rpfs.h"

/* ---------------------------------------------------------------------------
 * MyDOS Sequel executable (.exe, version 2 = x86-64): a 40-byte header followed by one flat image.
 *   0  "MDXE"          magic
 *   4  u16 version (2) 6  u16 flags (0)
 *   8  u64 load_addr   where the image goes in the program's address space (USER_BASE)
 *  16  u64 entry       virtual address of _start
 *  24  u32 file_size   bytes of image stored after the header (code + initialised data)
 *  28  u32 mem_size    bytes the program occupies in memory (file_size + zero-filled .bss)
 *  32  u32 stack_size  requested stack (16 KB .. 1 MB)
 *  36  u32 checksum    sum of all image bytes
 * tools/mkexe.py builds these from an ELF64 file produced by clang + the user linker script.
 *
 * Programs run in ring 3 (64-bit) in their own address space. Entry: rdi = argc, rsi = argv.
 * System calls: int 0x80 with rax = number, rdi/rsi/rdx = arguments, result in rax.
 * ------------------------------------------------------------------------- */
#define MDXE_MAGIC   0x4558444Du          /* "MDXE" little-endian */
#define MDXE_VERSION 2
#define MDXE_HEADER  40u

struct mdxe_header {
    u32 magic;
    u16 version, flags;
    u64 load_addr, entry;
    u32 file_size, mem_size, stack_size, checksum;
} __attribute__((packed));
typedef char mdxe_header_size_check[(sizeof(struct mdxe_header) == 40) ? 1 : -1];

enum { SYS_EXIT = 0, SYS_WRITE = 1, SYS_GETCHAR = 2, SYS_OPEN = 3, SYS_CLOSE = 4, SYS_READ = 5,
       SYS_UPTIME = 6, SYS_SLEEP = 7, SYS_SBRK = 8 };

#define PROC_ENOEXEC (-20)                /* not a valid MyDOS executable */
#define PROC_ENOMEM  (-21)
#define PROC_EBUSY   (-22)                /* a program is already running */

struct proc_result { int exit_code; int crashed; int killed; };

/* Load and run `path` from filesystem `fs`; returns 0 when the program ran (see *res), or an error. */
int  process_run(struct rpfs *fs, const char *path, int argc, char **argv, struct proc_result *res);
const char *process_strerror(int err);

int  process_running(void);
void process_request_kill(void);          /* Ctrl+C arrived while a program runs */
int  process_kill_pending(void);
void process_set_watchdog(u32 ticks);     /* self-tests: kill the next program after this many ticks */
void process_terminate(int code, int crashed, int killed) __attribute__((noreturn));
void process_user_fault(struct regs *r) __attribute__((noreturn));   /* a ring-3 exception: report and kill */
void syscall_dispatch(struct regs *r);

/* Shell helper: run `argv[0]` as a program if a matching .exe exists (cwd, then /bin). 1 = handled. */
int  process_shell_run(int argc, char **argv);
int  process_selftest(void);

/* assembly helpers (cpu.asm) */
struct kctx { u64 rbx, rbp, r12, r13, r14, r15, rsp, rip; };
int  kctx_save(struct kctx *c) __attribute__((returns_twice));
void kctx_restore(struct kctx *c, int value) __attribute__((noreturn));
void enter_user(u64 rip, u64 rsp, u64 argc, u64 argv) __attribute__((noreturn));
#endif
