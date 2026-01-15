#define _GNU_SOURCE
#include <errno.h>
#include <linux/sched.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static pid_t gettid_linux(void) {
    return (pid_t)syscall(SYS_gettid);
}

static void write_str(const char *s) {
    syscall(SYS_write, 2, s, (size_t)strlen(s)); // stderr
}

static void write_tid(const char *prefix, pid_t tid) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s%d\n", prefix, tid);
    if (n > 0) syscall(SYS_write, 2, buf, (size_t)n);
}

// Busy loop using only CPU (no libc)
static void burn_cycles(volatile uint64_t *x, uint64_t iters) {
    for (uint64_t i = 0; i < iters; i++) {
        *x = (*x * 1103515245u + 12345u) ^ i;
    }
}

typedef struct {
    int idx;
    int iterations;
} child_arg_t;

static int child_fn(void *argp) {
    // IMPORTANT: avoid printf/malloc/etc. in CLONE_THREAD mode.
    child_arg_t *a = (child_arg_t *)argp;
    pid_t tid = gettid_linux();
    write_tid("[clone-child] tid=", tid);

    volatile uint64_t x = (uint64_t)(a->idx + 1);
    for (int i = 0; i < a->iterations; i++) {
        burn_cycles(&x, 200000);
        syscall(SYS_sched_yield);
    }

    // Exit the thread cleanly
    syscall(SYS_exit, 0);
    __builtin_unreachable();
}

int main(int argc, char **argv) {
    int n = 5;
    int iterations = 30;

    if (argc >= 2) n = atoi(argv[1]);
    if (argc >= 3) iterations = atoi(argv[2]);

    fprintf(stderr, "[main] pid=%d tid=%d n=%d iterations=%d\n",
            getpid(), gettid_linux(), n, iterations);

    // Allocate a separate stack per clone thread
    const size_t stack_sz = 1024 * 1024;

    for (int i = 0; i < n; i++) {
        void *stack = mmap(NULL, stack_sz, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (stack == MAP_FAILED) {
            fprintf(stderr, "mmap failed: %s\n", strerror(errno));
            return 1;
        }
        void *child_stack = (char *)stack + stack_sz;

        // Heap arg is okay in parent; child will only read it
        child_arg_t *a = calloc(1, sizeof(*a));
        if (!a) {
            fprintf(stderr, "calloc failed\n");
            return 1;
        }
        a->idx = i;
        a->iterations = iterations;

        // Thread-like clone flags
        int flags = CLONE_THREAD | CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_SYSVSEM;

        // This calls glibc clone(), which your interposer wraps.
        int tid = clone(child_fn, child_stack, flags, a);
        if (tid == -1) {
            fprintf(stderr, "clone failed: %s\n", strerror(errno));
            return 1;
        }

        fprintf(stderr, "[main] clone returned tid=%d\n", tid);
        // Intentionally not freeing stack/arg for this test binary.
    }

    // Keep process alive so your monitor has time to sample clone threads.
    sleep(1);

    fprintf(stderr, "[main] done\n");
    return 0;
}