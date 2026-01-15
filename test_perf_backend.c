#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "perf_backend.h"

void high_miss_test(void) {
    size_t N = (size_t)512 * 1024 * 1024 / sizeof(int);
    int *a = malloc(N * sizeof(int));
    if (!a) {
        perror("malloc");
        return;
    }

    for (size_t i = 0; i < N; i++)
        a[i] = (int)i;

    volatile long long sum = 0;
    size_t stride = 64 / sizeof(int); 

    for (int pass = 0; pass < 100; pass++) {   
        for (size_t i = 0; i < N; i += stride) {
            sum += a[i];
        }
    }

    if (sum == -1) {
        printf("sum=%lld\n", sum);
    }

    free(a);
}

static void memory_workload(void) {
    const size_t N = 16 * 1024 * 1024 / sizeof(uint64_t); 
    uint64_t *buf = aligned_alloc(64, N * sizeof(uint64_t));
    if (!buf) {
        perror("aligned_alloc");
        return;
    }
    for (size_t i = 0; i < N; i++) buf[i] = i;

    volatile uint64_t acc = 0;
    for (int reps = 0; reps < 5; reps++) {
        for (size_t i = 0; i < N; i += 16) {
            acc += buf[i];
        }
    }
    printf("Memory workload acc = %llu\n", (unsigned long long)acc);
    free(buf);
}

int main(int argc, char **argv) {
    int cpu;

    if (argc > 1) {
        cpu = atoi(argv[1]);
    } else {
        cpu = sched_getcpu();
    }

    if (cpu < 0) {
        perror("sched_getcpu");
        return 1;
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        return 1;
    }

    printf("Pinned to CPU %d\n", cpu);

    perf_monitor_t mon;
    if (perf_monitor_open(cpu, &mon) < 0) {
        fprintf(stderr, "perf_monitor_open failed on CPU %d\n", cpu);
        return 1;
    }

    if (perf_monitor_start(&mon) < 0) {
        fprintf(stderr, "perf_monitor_start failed\n");
        perf_monitor_close(&mon);
        return 1;
    }

    high_miss_test();
    memory_workload();

    uint64_t values[MEV_NUM_EVENTS] = {0};
    if (perf_monitor_stop_and_read(&mon, values) < 0) {
        fprintf(stderr, "perf_monitor_stop_and_read failed\n");
        perf_monitor_close(&mon);
        return 1;
    }

    perf_monitor_close(&mon);

    printf("\nResults (MEV_* indices in brackets):\n");
    printf("  [%d] INST_RETIRED        : %llu\n",
           MEV_INST_RETIRED,
           (unsigned long long)values[MEV_INST_RETIRED]);

    printf("  [%d] CACHE_MISSES        : %llu\n",
           MEV_CACHE_MISSES,
           (unsigned long long)values[MEV_CACHE_MISSES]);

    printf("  [%d] CORE_CYCLES         : %llu\n",
           MEV_CORE_CYCLES,
           (unsigned long long)values[MEV_CORE_CYCLES]);

    printf("  [%d] MEM_INST_RETIRED    : %llu\n",
           MEV_MEM_INST_RETIRED,
           (unsigned long long)values[MEV_MEM_INST_RETIRED]);

    printf("  [%d] PAGE_FAULTS         : %llu\n",
           MEV_PAGE_FAULTS,
           (unsigned long long)values[MEV_PAGE_FAULTS]);

    printf("  [%d] CYCLE_ACTIVITY_MEM  : %llu\n",
           MEV_CYCLE_ACTIVITY_MEM,
           (unsigned long long)values[MEV_CYCLE_ACTIVITY_MEM]);

    printf("  [%d] UOPS_RETIRED        : %llu\n",
           MEV_UOPS_RETIRED,
           (unsigned long long)values[MEV_UOPS_RETIRED]);

    return 0;
}