#include "mdos.h"
static char composite[100001];                 /* lives in .bss: the loader must hand us zeroed memory */
int main(int argc, char **argv) {
    int limit = argc > 1 ? atoi(argv[1]) : 100;
    if (limit < 2 || limit > 100000) { print("usage: primes [2..100000]\n"); return 2; }
    int count = 0;
    for (int i = 2; i <= limit; i++) {
        if (composite[i]) continue;
        printf(count ? " %d" : "%d", i);
        count++;
        for (int j = i * 2; j <= limit; j += i) composite[j] = 1;
    }
    printf("\n%d primes up to %d\n", count, limit);
    return 0;
}
