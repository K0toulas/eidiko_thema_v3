#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static void* worker(void* arg) {
    (void)arg;
    volatile uint64_t x = 1;
    for (uint64_t i = 0; i < 200000000ULL; i++) {
        x = x * 1664525ULL + 1013904223ULL;
        x ^= (x >> 13);
    }
    return NULL;
}

int main() {
    const int n = 4;
    pthread_t th[n];

    for (int i = 0; i < n; i++) {
        if (pthread_create(&th[i], NULL, worker, NULL) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    for (int i = 0; i < n; i++) {
        pthread_join(th[i], NULL);
    }

    puts("test_workload done");
    return 0;
}
