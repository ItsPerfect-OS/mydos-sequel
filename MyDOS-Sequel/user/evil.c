#include "mdos.h"
/* Hostile behaviour on purpose: the kernel must stay up and stop us. */
#pragma clang diagnostic ignored "-Winfinite-recursion"
static void deeper(int n) { volatile char pad[512]; pad[0] = (char)n; deeper(n + 1); pad[1] = pad[0]; }
int main(int argc, char **argv) {
    if (argc < 2) { print("usage: evil kptr|priv|port|kmem|wild|stack\n"); return 2; }
    const char *m = argv[1];
    if (!strcmp(m, "kptr")) {                         /* hand the kernel bad pointers: every call must fail cleanly */
        int fd = open("/bin/hello.exe", O_READ);
        long r1 = write(1, (void *)0x100000, 64);      /* kernel memory */
        long r2 = write(1, (void *)0xDEAD0000, 16);    /* nothing there */
        long r3 = read(fd, (void *)0x100000, 64);      /* would overwrite kernel code */
        int r4 = open((const char *)0x100000, O_READ); /* path in kernel memory */
        long r5 = write(1, (void *)0xFFFFFFFFFFFFFFF0UL, 64); /* wraps around the address space */
        printf("kptr results: %d %d %d %d %d\n", (int)r1, (int)r2, (int)r3, r4, (int)r5);
        return (r1 < 0 && r2 < 0 && r3 < 0 && r4 < 0 && r5 < 0) ? 0 : 1;
    }
    if (!strcmp(m, "priv"))  { __asm__ volatile("cli"); print("survived cli?!\n"); return 1; }
    if (!strcmp(m, "port"))  { __asm__ volatile("outb %%al, $0x80" :: "a"(0)); print("survived out?!\n"); return 1; }
    if (!strcmp(m, "kmem"))  { volatile u32 v = *(volatile u32 *)0x100000; printf("read kernel memory: %x\n", v); return 1; }
    if (!strcmp(m, "wild"))  { ((void (*)(void))0x1234)(); return 1; }
    if (!strcmp(m, "stack")) { deeper(0); return 1; }
    print("unknown mode\n");
    return 2;
}
