#include "mdos.h"
int main(void) {
    char *p = sbrk(1024 * 1024);
    if (p == (char *)-1) { print("sbrk failed\n"); return 1; }
    for (int i = 0; i < 1024 * 1024; i += 4096) if (p[i] != 0) { print("heap not zeroed\n"); return 2; }
    for (int i = 0; i < 1024 * 1024; i++) p[i] = (char)(i * 7);
    for (int i = 0; i < 1024 * 1024; i++) if (p[i] != (char)(i * 7)) { print("heap corrupted\n"); return 3; }
    if (sbrk(256 * 1024 * 1024) != (void *)-1) { print("sbrk limit not enforced\n"); return 4; }
    print("1 MB heap ok; oversize request refused\n");
    return 0;
}
