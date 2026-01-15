#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static pid_t gettid_linux(void) {
    return (pid_t)syscall(SYS_gettid);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

typedef struct {
    int idx;
    volatile int *stop_flag;
    int max_ms;
} worker_arg_t;

static void *worker_fn(void *argp) {
    worker_arg_t *a = (worker_arg_t *)argp;
    pid_t tid = gettid_linux();
    fprintf(stderr, "[worker %02d] start tid=%d\n", a->idx, tid);

    volatile uint64_t x = 0;
    uint64_t start = now_ns();

    while (!*(a->stop_flag)) {
        // Do some work to generate perf activity
        for (int i = 0; i < 300000; i++) x += (uint64_t)i ^ (x << 1);

        // allow migration / scheduling decisions
        sched_yield();

        if ((int)((now_ns() - start) / 1000000ull) >= a->max_ms) break;
    }

    fprintf(stderr, "[worker %02d] exit  tid=%d x=%llu\n",
            a->idx, tid, (unsigned long long)x);
    return NULL;
}

int main(int argc, char **argv) {
    int nthreads = 10;
    int stop_after_ms = 30;
    int keep_running_ms = 200;

    if (argc >= 2) nthreads = atoi(argv[1]);
    if (argc >= 3) stop_after_ms = atoi(argv[2]);
    if (argc >= 4) keep_running_ms = atoi(argv[3]);

    fprintf(stderr,
            "[main] pid=%d tid=%d nthreads=%d stop_after_ms=%d keep_running_ms=%d\n",
            getpid(), gettid_linux(), nthreads, stop_after_ms, keep_running_ms);

    pthread_t *ths = calloc((size_t)nthreads, sizeof(*ths));
    worker_arg_t *args = calloc((size_t)nthreads, sizeof(*args));
    volatile int *stop = calloc((size_t)nthreads, sizeof(*stop));
    if (!ths || !args || !stop) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }

    for (int i = 0; i < nthreads; i++) {
        args[i].idx = i;
        args[i].stop_flag = &stop[i];
        args[i].max_ms = keep_running_ms + 200; // safety bound

        int rc = pthread_create(&ths[i], NULL, worker_fn, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create(%d) failed\n", i);
            return 1;
        }
    }

    sleep_ms(stop_after_ms);

    int first_batch = nthreads / 2;
    fprintf(stderr, "[main] stopping first batch: %d threads\n", first_batch);
    for (int i = 0; i < first_batch; i++) stop[i] = 1;

    for (int i = 0; i < first_batch; i++) {
        pthread_join(ths[i], NULL);
        fprintf(stderr, "[main] joined worker %d\n", i);
    }

    sleep_ms(keep_running_ms);

    fprintf(stderr, "[main] stopping remaining threads\n");
    for (int i = first_batch; i < nthreads; i++) stop[i] = 1;

    for (int i = first_batch; i < nthreads; i++) {
        pthread_join(ths[i], NULL);
        fprintf(stderr, "[main] joined worker %d\n", i);
    }

    fprintf(stderr, "[main] done\n");
    return 0;
}