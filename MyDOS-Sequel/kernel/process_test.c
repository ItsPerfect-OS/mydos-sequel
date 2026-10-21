/* process_test.c - `proctest`: runs the demo programs (and hostile ones) on a private RAM disk */
#include "process.h"
#include "embedded.h"
#include "rpfs.h"
#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "console.h"
#include "string.h"
#include "blockdev.h"

static int step(const char *name, int ok) {
    console_set_color(COL_GREY); puts("  "); puts(name); puts(": ");
    console_set_color(ok ? COL_GREEN : COL_RED); puts(ok ? "ok\n" : "FAILED\n");
    return ok;
}

static int contains(const char *hay, const char *needle) {
    u32 n = (u32)strlen(needle);
    for (; *hay; hay++) if (!strncmp(hay, needle, n)) return 1;
    return 0;
}

static char out[2048];

/* Run a program with its output captured (and hidden). */
static int run(struct rpfs *fs, int argc, char **argv, struct proc_result *res) {
    char path[48] = "/bin/";
    strcpy(path + 5, argv[0]);
    strcpy(path + 5 + strlen(argv[0]), ".exe");
    console_capture_start(out, sizeof(out), 1);
    int r = process_run(fs, path, argc, argv, res);
    console_capture_stop();
    return r;
}

static u64 cr3(void) { u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }

int process_selftest(void) {
    int pass = 1, ok;
    struct blockdev *dev = ramdisk_create("ptest", 2048, 0);
    struct rpfs *fs = kmalloc(sizeof(*fs));
    if (!dev || !fs) { puts("  could not allocate a test RAM disk\n"); if (dev) ramdisk_destroy(dev); kfree(fs); return 0; }
    struct proc_result res;

    ok = rpfs_format(dev, "PTEST") == 0 && rpfs_mount(fs, dev) == 0 && embedded_install(fs) == (int)embedded_count;
    pass &= step("format a RAM disk and install the demo programs", ok);

    {   char *a[] = { "hello" };
        ok = run(fs, 1, a, &res) == 0 && res.exit_code == 0 && !res.crashed && !res.killed
          && contains(out, "Hello from MyDOS Sequel user mode!") && contains(out, "argc=1");
        pass &= step("hello runs in ring 3 and prints through system calls", ok); }

    {   char *a[] = { "hello", "alpha", "beta" };
        ok = run(fs, 3, a, &res) == 0 && contains(out, "argc=3") && contains(out, "argv[1] = alpha") && contains(out, "argv[2] = beta");
        pass &= step("arguments arrive in main(argc, argv)", ok); }

    {   char *a[] = { "primes", "30" };
        ok = run(fs, 2, a, &res) == 0 && res.exit_code == 0
          && contains(out, "2 3 5 7 11 13 17 19 23 29") && contains(out, "10 primes up to 30") && !contains(out, "31");
        pass &= step("a program with a 100 KB zero-initialised array (.bss) computes correctly", ok); }

    {   char *a[] = { "cat", "/nope" };
        ok = run(fs, 2, a, &res) == 0 && res.exit_code == 1 && !res.crashed && contains(out, "cannot open");
        pass &= step("exit codes and error paths work (cat of a missing file exits 1)", ok); }

    {   char text[1600];
        u32 n = 0;
        for (u32 i = 0; i < 150; i++) {                                  /* 150 lines of 10 bytes: "line 0001\n" */
            text[n++] = 'l'; text[n++] = 'i'; text[n++] = 'n'; text[n++] = 'e'; text[n++] = ' ';
            text[n++] = (char)('0' + (i + 1) / 1000 % 10); text[n++] = (char)('0' + (i + 1) / 100 % 10);
            text[n++] = (char)('0' + (i + 1) / 10 % 10);   text[n++] = (char)('0' + (i + 1) % 10); text[n++] = '\n';
        }
        char *cp[] = { "cp", "/src.txt", "/dst.txt" };
        char *ct[] = { "cat", "/dst.txt" };
        u8 back[1600]; u32 got = 0;
        ok = rpfs_write(fs, "/src.txt", text, n, 0) == 0
          && run(fs, 3, cp, &res) == 0 && res.exit_code == 0 && contains(out, "copied 1500 bytes")
          && rpfs_read(fs, "/dst.txt", 0, back, sizeof(back), &got) == 0 && got == n && !memcmp(back, text, n)
          && run(fs, 2, ct, &res) == 0 && res.exit_code == 0 && !strncmp(out, text, n);
        pass &= step("file system calls: a program copies a file (cp) and reads it back (cat)", ok); }

    {   char *a[] = { "brk" };
        ok = run(fs, 1, a, &res) == 0 && res.exit_code == 0 && contains(out, "1 MB heap ok");
        pass &= step("sbrk grows the heap (zeroed pages) and refuses an oversize request", ok); }

    {   char *a[] = { "segv" };
        ok = run(fs, 1, a, &res) == 0 && res.crashed && contains(out, "Program crashed: #PF Page Fault")
          && contains(out, "page not present, write");
        struct rpfs_stat st;
        ok &= rpfs_stat(fs, "/bin/hello.exe", &st) == 0;                  /* the kernel and filesystem are fine */
        pass &= step("a crashing program (NULL write) is stopped; the kernel carries on", ok); }

    {   char *a[] = { "evil", "kptr" };
        ok = run(fs, 2, a, &res) == 0 && res.exit_code == 0 && !res.crashed && contains(out, "kptr results: -1 -1 -1 -1 -1");
        pass &= step("hostile system-call pointers (kernel memory, wrap-around, unmapped) are refused", ok); }

    {   static char *modes[] = { "priv", "port", "kmem", "wild", "stack" };
        ok = 1;
        for (int i = 0; i < 5; i++) {
            char *a[] = { "evil", modes[i] };
            if (run(fs, 2, a, &res) != 0 || !res.crashed || !contains(out, "Program crashed")) ok = 0;
        }
        pass &= step("privileged instructions, port I/O, kernel memory, wild jumps and stack overflow each just kill the program", ok); }

    {   /* malformed executables */
        u8 *good = kmalloc(RPFS_MAX_FILE);
        u32 size = 0;
        ok = good && rpfs_read(fs, "/bin/hello.exe", 0, good, RPFS_MAX_FILE, &size) == 0 && size > 64;
        struct rpfs_stat st; (void)st;
        if (ok) {
            u8 junk[100];
            for (u32 i = 0; i < sizeof(junk); i++) junk[i] = (u8)(i * 37 + 11);
            rpfs_write(fs, "/junk.exe", junk, sizeof(junk), 0);
            rpfs_write(fs, "/short.exe", good, 40, 0);                                   /* header ok, image cut off */
            good[40] ^= 0x55; rpfs_write(fs, "/sum.exe", good, size, 0); good[40] ^= 0x55; /* checksum mismatch */
            *(u32 *)(good + 28) = 0x10000000u; rpfs_write(fs, "/huge.exe", good, size, 0);  /* absurd mem_size */
            rpfs_read(fs, "/bin/hello.exe", 0, good, RPFS_MAX_FILE, &size);
            *(u64 *)(good + 8) = 0x1000; rpfs_write(fs, "/low.exe", good, size, 0);          /* load address in kernel space */
            rpfs_read(fs, "/bin/hello.exe", 0, good, RPFS_MAX_FILE, &size);
            *(u64 *)(good + 16) = 0x1000; rpfs_write(fs, "/entry.exe", good, size, 0);       /* entry point outside the image */
            const char *bad[] = { "/junk.exe", "/short.exe", "/sum.exe", "/huge.exe", "/low.exe", "/entry.exe" };
            for (int i = 0; i < 6; i++) {
                char *a[] = { (char *)bad[i] };
                if (process_run(fs, bad[i], 1, a, &res) != PROC_ENOEXEC) ok = 0;
            }
            char *a[] = { "x" };
            ok &= process_run(fs, "/bin", 1, a, &res) == RPFS_EISDIR && process_run(fs, "/missing.exe", 1, a, &res) == RPFS_ENOENT;
        }
        kfree(good);
        pass &= step("corrupt, truncated, oversized and mis-addressed executables are rejected before running", ok); }

    {   u64 frames0 = pmm_free_frames(); u32 heap0 = heap_used_bytes();
        ok = 1;
        char *h[] = { "hello" }, *c[] = { "segv" }, *e[] = { "evil", "kptr" }, *s[] = { "primes", "50" }, *b[] = { "brk" };
        for (int i = 0; i < 20; i++) if (run(fs, 1, h, &res) != 0 || res.exit_code != 0) ok = 0;
        for (int i = 0; i < 3; i++) { if (run(fs, 1, c, &res) != 0 || !res.crashed) ok = 0; }
        for (int i = 0; i < 3; i++) { if (run(fs, 2, e, &res) != 0) ok = 0; run(fs, 2, s, &res); run(fs, 1, b, &res); }
        ok &= pmm_free_frames() == frames0 && heap_used_bytes() == heap0 && cr3() == paging_kernel_pd();
        pass &= step("30+ runs (normal, crashing, heap-hungry) leak no frames or heap, and CR3 is back on the kernel", ok); }

    {   u64 frames0 = pmm_free_frames();
        char *sp[] = { "spin" }, *ct[] = { "count", "30" };
        process_set_watchdog(30);                                          /* the kernel stands in for the Ctrl+C key */
        ok = run(fs, 1, sp, &res) == 0 && res.killed && !res.crashed;
        process_set_watchdog(30);
        ok &= run(fs, 2, ct, &res) == 0 && res.killed && !res.crashed;     /* killed while asleep inside a system call */
        ok &= pmm_free_frames() == frames0 && cr3() == paging_kernel_pd();
        pass &= step("Ctrl+C-style kill stops a spinning program and a sleeping one, freeing everything", ok); }

    console_set_color(COL_GREY);
    kfree(fs);
    ramdisk_destroy(dev);
    console_set_color(pass ? COL_GREEN : COL_RED);
    puts(pass ? "Program loader self-test PASSED\n" : "Program loader self-test FAILED\n");
    return pass;
}
