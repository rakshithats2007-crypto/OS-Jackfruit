#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    size_t total = 0;

    while (1) {
        void *ptr = malloc(5 * 1024 * 1024); // 5 MB
        if (!ptr) {
            printf("malloc failed\n");
            break;
        }

        total += 5;
        printf("Allocated %zu MB\n", total);

        sleep(1);   // 🔥 IMPORTANT: slow down allocation
    }

    return 0;
}
