#include "mdos.h"
int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 3;
    for (int i = 1; i <= n; i++) { sleep_ms(1000); printf("%d... ", i); }
    print("done\n");
    return 0;
}
