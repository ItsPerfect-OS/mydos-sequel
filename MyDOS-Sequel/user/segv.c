#include "mdos.h"
int main(void) {
    print("About to write to address 0...\n");
    *(volatile int *)0 = 1;
    print("unreachable\n");
    return 0;
}
