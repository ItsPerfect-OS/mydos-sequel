#include "mdos.h"
int main(int argc, char **argv) {
    int status = 0;
    if (argc < 2) { print("usage: cat <file>...\n"); return 2; }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_READ);
        if (fd < 0) { printf("cat: cannot open %s\n", argv[i]); status = 1; continue; }
        char buf[256];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, (u32)n);
        close(fd);
    }
    return status;
}
