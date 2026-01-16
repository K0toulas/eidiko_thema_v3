#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int tidx;
    int pin_cpu;     // -1 = no pinning
    int seconds;     // runtime
    volatile uint64_t *sink;
} thread_arg_t;

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
    }
}

// Simple xorshift64*
static inline uint64_t xorshift64(uint64_t *x) {
    uint64_t z = *x;
    z ^= z >> 12;
    z ^= z << 25;
    z ^= z >> 27;
    *x = z;
    return z * 2685821657736338717ull;
}

static void *worker(void *p) {
    thread_arg_t *a = (thread_arg_t *)p;

    if (a->pin_cpu >= 0) {
        pin_to_cpu(a->pin_cpu);
    }

    // Hot loop: lots of ALU + some FP, tiny working set (in registers)
    uint64_t start = now_ns();
    uint64_t end = start + (uint64_t)a->seconds * 1000000000ull;

    uint64_t s = 0x9e3779b97f4a7c15ull ^ (uint64_t)(a->tidx + 1) * 0xD1B54A32D192ED03ull;
    double d = 1.0000001 + (double)a->tidx * 1e-9;

    volatile uint64_t acc = 0;

    while (now_ns() < end) {
        // Unrolled-ish compute
        for (int i = 0; i < 200000; i++) {
            uint64_t r = xorshift64(&s);
            // integer mix
            r ^= (r << 13);
            r ^= (r >> 7);
            r *= 0x2545F4914F6CDD1Dull;
            acc += (r ^ (r >> 33));

            // small FP work (kept in registers)
            d = d * 1.0000000003 + 0.0000000001;
            d = d - 0.00000000005;
        }
    }

    // fold FP into acc so compiler can't drop it
    acc ^= (uint64_t)(d * 1e9);
    *(a->sink) ^= acc;
    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-t threads] [-s seconds] [--pin]\n"
        "  -t threads   number of threads (1..20), default 8\n"
        "  -s seconds   runtime seconds, default 10\n"
        "  --pin        pin threads round-robin to CPUs 0..15 (default off)\n",
        prog);
}

int main(int argc, char **argv) {
    int threads = 8;
    int seconds = 10;
    int pin = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--pin")) {
            pin = 1;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (threads < 1) threads = 1;
    if (threads > 20) threads = 20;

    printf("mt_compute: threads=%d seconds=%d pin=%d\n", threads, seconds, pin);

    pthread_t ths[20];
    thread_arg_t args[20];
    volatile uint64_t sink = 0;

    for (int i = 0; i < threads; i++) {
        args[i].tidx = i;
        args[i].seconds = seconds;
        args[i].sink = &sink;
        args[i].pin_cpu = pin ? (i % 16) : -1;

        if (pthread_create(&ths[i], NULL, worker, &args[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    for (int i = 0; i < threads; i++) {
        pthread_join(ths[i], NULL);
    }

    printf("done (sink=%llu)\n", (unsigned long long)sink);
    return 0;
}