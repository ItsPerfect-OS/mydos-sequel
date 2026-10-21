#include "mdos.h"
int main(int argc, char **argv) {
    printf("Hello from MyDOS Sequel user mode!\n");
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d] = %s\n", i, argv[i]);
    printf("uptime: %u ms\n", uptime_ms());
    return 0;
}
