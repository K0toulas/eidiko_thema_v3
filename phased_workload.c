#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static inline uint64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void compute_phase(double *a, double *b, double *c, size_t n, uint64_t dur_ns) {
    uint64_t end = nsec_now() + dur_ns;
    size_t i = 0;
    // FMA-ish loop (portable)
    while (nsec_now() < end) {
        for (size_t k = 0; k < n; k++) {
            a[k] = a[k] * 1.0000001 + b[k] * 0.9999999 + c[k];
        }
        i++;
    }
    // Prevent optimizer from removing work
    volatile double sink = a[(i + 7) % n];
    (void)sink;
}

static void memory_phase(uint32_t *next, uint32_t *buf, size_t n, uint64_t dur_ns) {
    uint64_t end = nsec_now() + dur_ns;

    // 1) Pointer chasing: defeats prefetch, increases stalls/misses
    uint32_t idx = 0;
    while (nsec_now() < end) {
        for (size_t k = 0; k < n; k++) {
            idx = next[idx];
            buf[idx] += 1;
        }
    }

    // 2) Streaming sweep: bandwidth
    for (size_t k = 0; k < n; k++) {
        buf[k] ^= (uint32_t)k;
    }

    volatile uint32_t sink = buf[idx];
    (void)sink;
}

static void io_phase(int fd, uint8_t *io_buf, size_t io_size, uint64_t dur_ns, int do_fsync) {
    uint64_t end = nsec_now() + dur_ns;

    // Alternate write and read within file
    off_t off = 0;
    while (nsec_now() < end) {
        // Write
        if (lseek(fd, off, SEEK_SET) == (off_t)-1) break;
        ssize_t w = write(fd, io_buf, io_size);
        if (w < 0) break;

        if (do_fsync) fsync(fd);

        // Read back
        if (lseek(fd, off, SEEK_SET) == (off_t)-1) break;
        ssize_t r = read(fd, io_buf, io_size);
        if (r < 0) break;

        off += (off_t)io_size;
        if (off > (off_t)(256 * 1024 * 1024)) off = 0; // wrap within 256MB
    }
}

int main(int argc, char **argv) {
    // Parameters
    int seconds = (argc > 1) ? atoi(argv[1]) : 30;
    int phase_ms = (argc > 2) ? atoi(argv[2]) : 300;      // phase duration
    int do_fsync = (argc > 3) ? atoi(argv[3]) : 0;        // 1 forces disk-ish behavior

    fprintf(stderr, "phased_workload: seconds=%d phase_ms=%d fsync=%d\n", seconds, phase_ms, do_fsync);

    // Compute buffers
    size_t n_d = 1u << 20; // ~1M doubles (~8MB each array)
    double *a = aligned_alloc(64, n_d * sizeof(double));
    double *b = aligned_alloc(64, n_d * sizeof(double));
    double *c = aligned_alloc(64, n_d * sizeof(double));
    if (!a || !b || !c) { perror("alloc"); return 1; }
    for (size_t i = 0; i < n_d; i++) { a[i] = 1.0; b[i] = 2.0; c[i] = 3.0; }

    // Memory buffers
    size_t n_u = 1u << 20; // 1M uint32
    uint32_t *buf  = aligned_alloc(64, n_u * sizeof(uint32_t));
    uint32_t *next = aligned_alloc(64, n_u * sizeof(uint32_t));
    if (!buf || !next) { perror("alloc"); return 1; }
    for (size_t i = 0; i < n_u; i++) buf[i] = (uint32_t)i;

    // Build a random-ish permutation for pointer chasing
    for (size_t i = 0; i < n_u; i++) next[i] = (uint32_t)((i * 48271u) % n_u);

    // I/O setup
    //const char *path = "io_workload.bin";
    //int fd = open(path, O_CREAT | O_RDWR, 0644);
    //if (fd < 0) { perror("open"); return 1; }

   // size_t io_size = 1 << 20; // 1MB chunks
    //uint8_t *io_buf = aligned_alloc(4096, io_size);
    //if (!io_buf) { perror("io alloc"); return 1; }
    //memset(io_buf, 0xA5, io_size);

    uint64_t t0 = nsec_now();
    uint64_t end_all = t0 + (uint64_t)seconds * 1000000000ull;
    uint64_t phase_ns = (uint64_t)phase_ms * 1000000ull;

    int phase = 0;
    while (nsec_now() < end_all) {
    fprintf(stderr, "PHASE compute\n");
    compute_phase(a, b, c, n_d, phase_ns);
    }
   // close(fd);
    fprintf(stderr, "done\n");
    return 0;
}