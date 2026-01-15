#!/usr/bin/env bash
set -euo pipefail

CC=${CC:-gcc}
CFLAGS="-O2 -g -fPIC -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE"
LDFLAGS_SO="-shared"
LDLIBS="-ldl -lpthread"

echo "[1/4] Build perf_backend.o"
$CC $CFLAGS -c perf_backend.c -o perf_backend.o

echo "[2/4] Build libmonitor.so"
$CC $CFLAGS $LDFLAGS_SO -o libmonitor.so libmonitor.c perf_backend.o $LDLIBS

echo "[3/4] Build a tiny pthread test workload"
cat > test_workload.c <<'EOF'
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
EOF

$CC -O2 -g -Wall -Wextra test_workload.c -o test_workload -lpthread

echo "[4/4] Run with LD_PRELOAD (scheduler socket optional)"
echo "  If /tmp/scheduler_socket is NOT running, you will see connect errors (that's OK)."
LD_PRELOAD=./libmonitor.so ./test_workload