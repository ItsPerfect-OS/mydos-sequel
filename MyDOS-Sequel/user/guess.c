#include "mdos.h"
int main(void) {
    int secret = (int)(((uptime_ms() * 2654435761u) >> 16) % 100) + 1;
    print("I'm thinking of a number from 1 to 100. Type q to give up.\n");
    for (int tries = 1; ; tries++) {
        char line[16];
        printf("guess #%d> ", tries);
        getline(line, sizeof(line));
        if (line[0] == 'q') { printf("It was %d.\n", secret); return 1; }
        int g = atoi(line);
        if (g == secret) { printf("Correct, in %d tries!\n", tries); return 0; }
        print(g < secret ? "higher\n" : "lower\n");
    }
}
