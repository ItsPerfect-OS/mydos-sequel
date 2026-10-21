/* process.c - program loader, user/kernel boundary and system calls
 *
 * One program runs at a time (like DOS): the shell blocks inside process_run() until the program
 * exits, crashes or is killed with Ctrl+C. process_run() remembers where it was with kctx_save();
 * ending a program (process_terminate) frees its address space and jumps back there with kctx_restore().
 */
#include "process.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "timer.h"
#include "keyboard.h"
#include "fscmds.h"
#include "gdt.h"

#define MAX_IMAGE      (4u * 1024 * 1024)
#define MIN_STACK      (16u * 1024)
#define MAX_STACK      (1024u * 1024)
#define STACK_TOP      USER_STACK_TOP
#define HEAP_LIMIT     (USER_BASE + 64ULL * 1024 * 1024)     /* sbrk may grow the image up to here */
#define USER_END       0x0000800000000000ULL                  /* end of the lower canonical half */
#define MAX_FDS        8
#define MAX_ARGS       32

struct fdent { int used; int mode; u32 off; char path[RPFS_PATH_MAX]; };

static struct {
    int running;
    u64 pd;
    struct rpfs *fs;
    struct kctx ctx;
    int exit_code, crashed, killed;
    u64 brk;
    volatile int kill_pending;
    u32 deadline;                                  /* test watchdog: kill the program at this tick (0 = off) */
    struct fdent fd[MAX_FDS];
} proc;

const char *process_strerror(int e) {
    switch (e) {
        case PROC_ENOEXEC: return "not a valid MyDOS executable";
        case PROC_ENOMEM:  return "not enough memory";
        case PROC_EBUSY:   return "a program is already running";
        default:           return rpfs_strerror(e);
    }
}

int process_running(void) { return proc.running; }
void process_request_kill(void) { proc.kill_pending = 1; }
int process_kill_pending(void) {
    if (proc.deadline && (int)(timer_ticks() - proc.deadline) >= 0) proc.kill_pending = 1;
    return proc.kill_pending;
}
void process_set_watchdog(u32 ticks) { proc.deadline = ticks ? timer_ticks() + ticks : 0; }

/* ======================= ending a program ======================= */

void process_terminate(int code, int crashed, int killed) {
    proc.exit_code = code;
    proc.crashed = crashed;
    proc.killed = killed;
    paging_switch(paging_kernel_pd());                  /* leave the program's address space first... */
    paging_destroy_space(proc.pd);                      /* ...then free it */
    proc.pd = 0;
    proc.running = 0;
    proc.kill_pending = 0;
    proc.deadline = 0;
    keyboard_flush();
    kctx_restore(&proc.ctx, 1);                         /* resume inside process_run() */
}

void process_user_fault(struct regs *r) {
    u64 cr2 = 0;
    if (r->int_no == 14) __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    console_set_color(COL_RED);
    puts("\nProgram crashed: "); puts(idt_exception_name(r->int_no));
    puts(" at RIP="); put_hex64(r->rip);
    if (r->int_no == 14) {
        puts(", address "); put_hex64(cr2);
        puts((r->err_code & 1) ? " (protection violation, " : " (page not present, ");
        puts((r->err_code & 2) ? "write)" : "read)");
    }
    putc('\n');
    process_terminate(-1, 1, 0);
}

/* ======================= user memory access ======================= */

/* Is [addr, addr+len) entirely mapped for ring 3 in the current program (and writable if asked)? */
static int user_ok(u64 addr, u64 len, int write) {
    if (len == 0) return 1;
    if (addr < USER_BASE || addr + len < addr || addr + len > USER_END) return 0;
    for (u64 p = addr & ~(PAGE_SIZE - 1); p < addr + len; p += PAGE_SIZE) {
        u64 flags;
        if (!paging_translate_in(proc.pd, p, 0, &flags)) return 0;
        if (!(flags & PAGE_USER) || (write && !(flags & PAGE_WRITE))) return 0;
    }
    return 1;
}

static int user_string(char *dst, u64 ua, u32 max) {              /* returns length, or -1 */
    for (u32 i = 0; i < max; i++) {
        if (!user_ok(ua + i, 1, 0)) return -1;
        dst[i] = *(const char *)(ua + i);
        if (!dst[i]) return (int)i;
    }
    return -1;
}

/* Write into a program's memory from the kernel while its address space is NOT the current one. */
static int poke_user(u64 pd, u64 va, const void *src, u64 n) {
    const u8 *s = src;
    while (n) {
        u64 phys, chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > n) chunk = n;
        if (!paging_translate_in(pd, va, &phys, 0)) return 0;
        memcpy((void *)phys, s, chunk);                           /* frames are identity-mapped in the kernel */
        va += chunk; s += chunk; n -= chunk;
    }
    return 1;
}

/* ======================= system calls ======================= */

static long sys_open(struct regs *r) {
    char path[RPFS_PATH_MAX];
    if (user_string(path, r->rdi, sizeof(path)) < 0) return -1;
    int mode = (int)r->rsi;
    if (!proc.fs || mode < 0 || mode > 2) return -1;
    struct rpfs_stat st;
    int e = rpfs_stat(proc.fs, path, &st);
    if (mode == 0) { if (e || st.mode != RPFS_FILE) return -1; }
    else {
        if (!e && st.mode != RPFS_FILE) return -1;
        if (rpfs_write(proc.fs, path, "", 0, mode == 2)) return -1;   /* create, or truncate for mode 1 */
    }
    for (int i = 3; i < MAX_FDS; i++) {
        if (proc.fd[i].used) continue;
        proc.fd[i].used = 1; proc.fd[i].mode = mode; proc.fd[i].off = 0;
        strcpy(proc.fd[i].path, path);
        return i;
    }
    return -1;
}

static long sys_read(struct regs *r) {
    int fd = (int)r->rdi;
    u64 buf = r->rsi, n = r->rdx;
    if (n > 65536) n = 65536;
    if (!user_ok(buf, n, 1)) return -1;
    if (fd == 0) {                                                   /* stdin: block for one key, then take what's there */
        u64 got = 0;
        char c = keyboard_getc();
        if (c == 3) process_terminate(130, 0, 1);
        ((char *)buf)[got++] = c;
        while (got < n && keyboard_has_key()) ((char *)buf)[got++] = keyboard_getc();
        return (long)got;
    }
    if (fd < 3 || fd >= MAX_FDS || !proc.fd[fd].used || proc.fd[fd].mode != 0) return -1;
    u8 tmp[512];
    u64 total = 0;
    while (total < n) {
        u32 want = n - total < sizeof(tmp) ? (u32)(n - total) : (u32)sizeof(tmp), got = 0;
        if (rpfs_read(proc.fs, proc.fd[fd].path, proc.fd[fd].off, tmp, want, &got)) return -1;
        if (got == 0) break;
        memcpy((void *)(buf + total), tmp, got);
        proc.fd[fd].off += got;
        total += got;
        if (got < want) break;
    }
    return (long)total;
}

static long sys_write(struct regs *r) {
    int fd = (int)r->rdi;
    u64 buf = r->rsi, n = r->rdx;
    if (n > 65536) n = 65536;
    if (!user_ok(buf, n, 0)) return -1;
    if (fd == 1 || fd == 2) {
        for (u64 i = 0; i < n; i++) putc(((const char *)buf)[i]);
        return (long)n;
    }
    if (fd < 3 || fd >= MAX_FDS || !proc.fd[fd].used || proc.fd[fd].mode == 0) return -1;
    u8 tmp[512];
    u64 total = 0;
    while (total < n) {
        u32 chunk = n - total < sizeof(tmp) ? (u32)(n - total) : (u32)sizeof(tmp);
        memcpy(tmp, (const void *)(buf + total), chunk);
        if (rpfs_write(proc.fs, proc.fd[fd].path, tmp, chunk, 1)) return total ? (long)total : -1;
        total += chunk;
    }
    return (long)total;
}

static long sys_sbrk(struct regs *r) {
    long incr = (long)r->rdi;
    u64 old = proc.brk;
    if (incr == 0) return (long)old;
    if (incr < 0) return -1;                                          /* shrinking isn't supported */
    u64 want = old + (u64)incr;
    if (want < old || want > HEAP_LIMIT) return -1;
    for (u64 p = (old + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); p < want; p += PAGE_SIZE) {
        if (paging_translate_in(proc.pd, p, 0, 0)) continue;
        u64 f = pmm_alloc_frame();
        if (!f) return -1;
        memset((void *)f, 0, PAGE_SIZE);
        if (!paging_map_in(proc.pd, p, f, PAGE_USER | PAGE_WRITE)) { pmm_free_frame(f); return -1; }
    }
    proc.brk = want;
    return (long)old;
}

void syscall_dispatch(struct regs *r) {
    __asm__ volatile("sti");                                          /* syscalls may block; keep interrupts flowing */
    long ret = -1;
    switch (r->rax) {
        case SYS_EXIT:    process_terminate((int)r->rdi, 0, 0);
        case SYS_WRITE:   ret = sys_write(r); break;
        case SYS_GETCHAR: {
            char c = keyboard_getc();
            if (c == 3) process_terminate(130, 0, 1);                 /* Ctrl+C */
            ret = (unsigned char)c;
            break;
        }
        case SYS_OPEN:    ret = sys_open(r); break;
        case SYS_CLOSE: {
            int fd = (int)r->rdi;
            if (fd >= 3 && fd < MAX_FDS && proc.fd[fd].used) { proc.fd[fd].used = 0; ret = 0; }
            break;
        }
        case SYS_READ:    ret = sys_read(r); break;
        case SYS_UPTIME:  ret = (long)(timer_ticks() * (1000 / timer_hz())); break;
        case SYS_SLEEP: {
            u32 target = timer_ticks() + ((u32)r->rdi * timer_hz()) / 1000;
            while ((int)(timer_ticks() - target) < 0) {
                if (process_kill_pending()) process_terminate(130, 0, 1);
                __asm__ volatile("sti; hlt");
            }
            ret = 0;
            break;
        }
        case SYS_SBRK:    ret = sys_sbrk(r); break;
        default:          ret = -1; break;
    }
    __asm__ volatile("cli");
    r->rax = (u64)ret;
    if (process_kill_pending()) process_terminate(130, 0, 1);         /* Ctrl+C that arrived during the call */
}

/* ======================= loading and running ======================= */

static int map_fresh_page(u64 pd, u64 va, const void *src, u32 n) {
    u64 f = pmm_alloc_frame();
    if (!f) return 0;
    memset((void *)f, 0, PAGE_SIZE);
    if (src && n) memcpy((void *)f, src, n);
    if (!paging_map_in(pd, va, f, PAGE_USER | PAGE_WRITE)) { pmm_free_frame(f); return 0; }
    return 1;
}

int process_run(struct rpfs *fs, const char *path, int argc, char **argv, struct proc_result *res) {
    if (proc.running) return PROC_EBUSY;
    if (!fs || !fs->mounted) return RPFS_ENOFS;
    if (argc > MAX_ARGS) return RPFS_EINVAL;

    struct rpfs_stat st;
    int r = rpfs_stat(fs, path, &st);
    if (r) return r;
    if (st.mode != RPFS_FILE) return RPFS_EISDIR;
    if (st.size < MDXE_HEADER || st.size > RPFS_MAX_FILE) return PROC_ENOEXEC;
    u8 *file = kmalloc(st.size);
    if (!file) return PROC_ENOMEM;
    u32 got = 0;
    r = rpfs_read(fs, path, 0, file, st.size, &got);
    if (r || got != st.size) { kfree(file); return r ? r : RPFS_EIO; }

    /* ---- validate the header: never trust a file ---- */
    struct mdxe_header h;
    memcpy(&h, file, sizeof(h));
    u32 sum = 0;
    for (u32 i = 0; i < h.file_size && MDXE_HEADER + i < st.size; i++) sum += file[MDXE_HEADER + i];
    int ok = h.magic == MDXE_MAGIC && h.version == MDXE_VERSION && h.flags == 0
          && h.load_addr == USER_BASE
          && h.file_size > 0 && h.file_size <= h.mem_size && h.mem_size <= MAX_IMAGE
          && MDXE_HEADER + h.file_size == st.size
          && h.entry >= h.load_addr && h.entry < h.load_addr + h.file_size
          && h.stack_size >= MIN_STACK && h.stack_size <= MAX_STACK
          && sum == h.checksum;
    if (!ok) { kfree(file); return PROC_ENOEXEC; }

    /* ---- build the address space ---- */
    u64 pd = paging_create_space();
    if (!pd) { kfree(file); return PROC_ENOMEM; }
    int built = 1;
    u32 pages = (h.mem_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u32 k = 0; k < pages && built; k++) {
        u32 off = k * PAGE_SIZE, n = 0;
        if (off < h.file_size) n = h.file_size - off < PAGE_SIZE ? h.file_size - off : PAGE_SIZE;
        built = map_fresh_page(pd, h.load_addr + off, file + MDXE_HEADER + off, n);
    }
    u32 stack_pages = (h.stack_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u32 k = 1; k <= stack_pages && built; k++) built = map_fresh_page(pd, STACK_TOP - k * PAGE_SIZE, 0, 0);

    /* ---- arguments: strings at the top of the stack, then argv[], then argc/argv in registers ---- */
    u64 sp = STACK_TOP, argv_user[MAX_ARGS + 1];
    for (int i = argc - 1; i >= 0 && built; i--) {
        u32 len = (u32)strlen(argv[i]) + 1;
        if (len > 256) len = 256;
        sp -= len;
        char tmp[256];
        memcpy(tmp, argv[i], len - 1);
        tmp[len - 1] = 0;
        built = poke_user(pd, sp, tmp, len);
        argv_user[i] = sp;
    }
    if (built) {
        argv_user[argc] = 0;
        sp &= ~7ULL;
        sp -= (u64)(argc + 1) * 8;
        built = poke_user(pd, sp, argv_user, (u64)(argc + 1) * 8);
    }
    u64 argv_ptr = sp;
    sp = (sp & ~15ULL) - 16;
    kfree(file);
    if (!built) { paging_destroy_space(pd); return PROC_ENOMEM; }

    /* ---- go ---- */
    memset(&proc.fd, 0, sizeof(proc.fd));
    proc.fd[0].used = proc.fd[1].used = proc.fd[2].used = 1;
    proc.pd = pd;
    proc.fs = fs;
    proc.brk = (h.load_addr + h.mem_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    proc.kill_pending = 0;
    proc.running = 1;
    proc.exit_code = proc.crashed = proc.killed = 0;
    u64 entry = h.entry;

    if (kctx_save(&proc.ctx) == 0) {
        paging_switch(pd);
        enter_user(entry, sp, (u64)argc, argv_ptr);               /* does not return here */
    }
    /* back from process_terminate() */
    __asm__ volatile("sti");
    if (res) { res->exit_code = proc.exit_code; res->crashed = proc.crashed; res->killed = proc.killed; }
    return 0;
}

/* ======================= shell integration ======================= */

static int is_file(struct rpfs *fs, const char *path) {
    struct rpfs_stat st;
    return rpfs_stat(fs, path, &st) == 0 && st.mode == RPFS_FILE;
}

int process_shell_run(int argc, char **argv) {
    struct rpfs *fs = fs_current();
    if (!fs || argc < 1) return 0;
    const char *name = argv[0];
    u32 len = (u32)strlen(name);
    if (len == 0 || len > 100) return 0;

    char cand[4][RPFS_PATH_MAX];
    int n = 0;
    int has_ext = len > 4 && !strcmp(name + len - 4, ".exe");
    int has_slash = 0;
    for (u32 i = 0; i < len; i++) if (name[i] == '/') has_slash = 1;
    if (has_slash) {
        strcpy(cand[n++], name);
        if (!has_ext) { strcpy(cand[n], name); strcpy(cand[n] + len, ".exe"); n++; }
    } else {
        strcpy(cand[n++], name);
        if (!has_ext) { strcpy(cand[n], name); strcpy(cand[n] + len, ".exe"); n++; }
        strcpy(cand[n], "/bin/"); strcpy(cand[n] + 5, name); n++;
        if (!has_ext) { strcpy(cand[n], "/bin/"); strcpy(cand[n] + 5, name); strcpy(cand[n] + 5 + len, ".exe"); n++; }
    }
    for (int i = 0; i < n; i++) {
        if (!is_file(fs, cand[i])) continue;
        struct proc_result res;
        console_set_color(COL_WHITE);
        int r = process_run(fs, cand[i], argc, argv, &res);
        console_set_color(COL_RED);
        if (r) { puts(cand[i]); puts(": "); puts(process_strerror(r)); putc('\n'); }
        else if (res.killed) { puts("^C\n"); }
        else if (!res.crashed && res.exit_code != 0) { puts("[exit code "); put_dec((u32)res.exit_code); puts("]\n"); }
        return 1;
    }
    return 0;
}
