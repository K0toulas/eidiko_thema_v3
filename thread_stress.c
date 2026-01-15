#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

typedef struct {
    int tidx;
    int work_us;
} Args;

static void *worker(void *p) {
    Args *a = (Args*)p;

    // small compute + memory touch
    const size_t N = 1 << 20; // 1M bytes
    uint8_t *buf = (uint8_t*)malloc(N);
    if (!buf) return NULL;

    // touch memory to generate loads/stores
    for (size_t i = 0; i < N; i += 64) {
        buf[i] = (uint8_t)(a->tidx + i);
    }

    // some integer work
    volatile uint64_t x = 0;
    for (int i = 0; i < 200000; i++) x += (uint64_t)i * 2654435761u;

    usleep(a->work_us);

    free(buf);
    return NULL;
}

int main(int argc, char **argv) {
    int waves = (argc > 1) ? atoi(argv[1]) : 20;
    int nthreads = (argc > 2) ? atoi(argv[2]) : 32;
    int work_us = (argc > 3) ? atoi(argv[3]) : 20000;

    printf("thread_stress: waves=%d nthreads=%d work_us=%d\n", waves, nthreads, work_us);

    for (int w = 0; w < waves; w++) {
        pthread_t *ths = (pthread_t*)calloc(nthreads, sizeof(pthread_t));
        Args *args = (Args*)calloc(nthreads, sizeof(Args));

        for (int i = 0; i < nthreads; i++) {
            args[i].tidx = i;
            args[i].work_us = work_us;
            int rc = pthread_create(&ths[i], NULL, worker, &args[i]);
            if (rc != 0) {
                fprintf(stderr, "pthread_create failed rc=%d at i=%d\n", rc, i);
                exit(1);
            }
        }

        for (int i = 0; i < nthreads; i++) {
            pthread_join(ths[i], NULL);
        }

        free(args);
        free(ths);

        // short pause between waves
        usleep(10000);
    }

    return 0;
}