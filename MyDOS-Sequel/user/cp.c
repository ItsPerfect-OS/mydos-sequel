#include "mdos.h"
int main(int argc, char **argv) {
    if (argc != 3) { print("usage: cp <from> <to>\n"); return 2; }
    int in = open(argv[1], O_READ);
    if (in < 0) { printf("cp: cannot read %s\n", argv[1]); return 1; }
    int out = open(argv[2], O_WRITE);
    if (out < 0) { printf("cp: cannot write %s\n", argv[2]); close(in); return 1; }
    char buf[512];
    int n, total = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (u32)n) != n) { print("cp: write failed (disk full?)\n"); close(in); close(out); return 1; }
        total += n;
    }
    close(in); close(out);
    printf("copied %d bytes\n", total);
    return 0;
}
