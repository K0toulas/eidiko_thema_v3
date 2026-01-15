#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <dirent.h>

#define LOG_DIR "scheduler_test1/logs"
#define RESULT_FILE "scheduler_test1/scheduler_results.csv"
#define DEBUG_LOG "scheduler_test1/logs/debug.log"
#define TIMING_LOG "scheduler_test1/logs/timing.log"
#define MAX_LINE 1024
#define OMP_NUM_THREADS "16"

const char *compute_cmds[3] = {
    "/srv/homes/ggantsios/eidiko/papi_examples/compute_bound/matrix_mul_omp_pure_tiled 7000 7000 7000",
    "/srv/homes/ggantsios/eidiko/papi_examples/compute_bound/matrix_mul_mkl_pure 20000 20000 20000",
    "/srv/homes/ggantsios/eidiko/papi_examples/compute_bound/matrix_mul_omp_pure_tiled 7000 7000 7000"
};
const char *io_cmds[3] = {
    "/srv/homes/ggantsios/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 7783 -c 8 -t 16 --data-size=32678 --ratio=10:1 --pipeline=10 --key-pattern=S:S",
    "/srv/homes/ggantsios/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 7783 -c 4 -t 16 --data-size=131072 --ratio=1:10 --key-pattern=G:G",
    "/srv/homes/ggantsios/eidiko/papi_examples/io_bound/io_intense_omp_pure 4 10000"
};
const char *memory_cmds[3] = {
    "/srv/homes/ggantsios/eidiko/benchmarks/clomp/clomp_mpi -1 -1 32 640000 32 1 100",
    "/srv/homes/ggantsios/eidiko/benchmarks/clomp/clomp_mpi -1 -1 32 1280000 32 1 100",
    "/srv/homes/ggantsios/eidiko/papi_examples/memory_bound/matrix_transpose_omp_pure 15000"
};

const char *types[3] = {"compute", "io", "memory"};

// Test configurations: [compute_count, io_count, memory_count]
const int test_configs[][3] = {
    {3, 3, 3},    // Original
    {1, 0, 0},    // One compute
    {0, 1, 0},    // One I/O
    {0, 0, 1},    // One memory
    {20, 3, 3},   // Compute-heavy
    {3, 20, 3},   // I/O-heavy
    {3, 3, 20},   // Memory-heavy
    {20, 20, 20}, // Balanced heavy
    {15, 15, 3},  // Compute + I/O heavy
    {15, 3, 15},  // Compute + Memory heavy
    {3, 15, 15}   // I/O + Memory heavy
};
const int num_tests = 11;

double get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

void log_message(const char *file_path, const char *message) {
    FILE *fp = fopen(file_path, "a");
    if (fp) {
        fprintf(fp, "%s\n", message);
        fclose(fp);
    }
}

double calculate_starvation(const char *timing_log, int num_processes) {
    FILE *fp = fopen(timing_log, "r");
    if (!fp) {
        log_message(DEBUG_LOG, "Warning: Timing log not found");
        return 0.0;
    }

    double *starts = (double *)calloc(num_processes, sizeof(double));
    double *ends = (double *)calloc(num_processes, sizeof(double));
    int *indices = (int *)calloc(num_processes, sizeof(int));
    int count = 0;
    char line[MAX_LINE];
    
    while (fgets(line, MAX_LINE, fp) && count < num_processes) {
        char event[10], type[10];
        int index;
        double timestamp;
        if (sscanf(line, "%9[^,],%9[^,],%d,%lf", event, type, &index, &timestamp) == 4) {
            if (strcmp(event, "START") == 0) {
                starts[count] = timestamp;
                indices[count] = index;
            } else if (strcmp(event, "END") == 0) {
                ends[count] = timestamp;
                count++;
            }
        }
    }
    fclose(fp);

    if (count == 0) {
        free(starts);
        free(ends);
        free(indices);
        return 0.0;
    }

    // Sort start and end times
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (starts[i] > starts[j]) {
                double temp = starts[i];
                starts[i] = starts[j];
                starts[j] = temp;
                int temp_idx = indices[i];
                indices[i] = indices[j];
                indices[j] = temp_idx;
            }
            if (ends[i] > ends[j]) {
                double temp = ends[i];
                ends[i] = ends[j];
                ends[j] = temp;
            }
        }
    }

    double max_starvation = 0.0;
    for (int i = 0; i < count - 1; i++) {
        if (starts[i + 1] > ends[i]) {
            double gap = (starts[i + 1] - ends[i]) / 1000.0; // Convert to seconds
            if (gap > max_starvation) max_starvation = gap;
        }
    }

    free(starts);
    free(ends);
    free(indices);
    return max_starvation;
}

int main(int argc, char *argv[]) {
    char *ld_preload = (argc > 1) ? argv[1] : NULL;
    char mode[20] = "cfs";
    if (ld_preload) {
        strcpy(mode, "scheduler");
    }

    // Create directories if needed
    mkdir("scheduler_test1", 0755);
    mkdir(LOG_DIR, 0755);

    // Initialize CSV file
    FILE *fp = fopen(RESULT_FILE, "w");
    if (fp) {
        fprintf(fp, "Test,Mode,Compute_Count,IO_Count,Memory_Count,Overall_Time_s,Max_Latency_s,Max_Starvation_s,Avg_Execution_s\n");
        fclose(fp);
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Starting %d tests in mode: %s", num_tests, mode);
    log_message(DEBUG_LOG, buf);

    // Run each test
    for (int test = 0; test < num_tests; test++) {
        int compute_count = test_configs[test][0];
        int io_count = test_configs[test][1];
        int memory_count = test_configs[test][2];
        int num_processes = compute_count + io_count + memory_count;

        snprintf(buf, sizeof(buf), "Starting test %d: [Compute=%d, IO=%d, Memory=%d]", test + 1, compute_count, io_count, memory_count);
        log_message(DEBUG_LOG, buf);

        // Clear logs for this test
        remove(TIMING_LOG);
        remove(DEBUG_LOG);

        // Create dummyfile and DB
        system("dd if=/dev/zero of=/tmp/dummyfile1 bs=1M count=100 status=none 2>/dev/null");
        system("touch /tmp/test.db");

        double start_time = get_time_ms();
        pid_t *pids = (pid_t *)malloc(num_processes * sizeof(pid_t));
        int *process_indices = (int *)malloc(num_processes * sizeof(int));
        char **process_types = (char **)malloc(num_processes * sizeof(char *));
        int status;

        // Start processes
        int proc_idx = 0;
        for (int i = 0; i < compute_count; i++) {
            pids[proc_idx] = fork();
            if (pids[proc_idx] == 0) {
                // Child process
                if (ld_preload) {
                    setenv("LD_PRELOAD", ld_preload, 1);
                }
                setenv("OMP_NUM_THREADS", OMP_NUM_THREADS, 1);

                // Log start time
                snprintf(buf, sizeof(buf), "START,compute,%d,%.0f", proc_idx + 1, get_time_ms());
                log_message(TIMING_LOG, buf);

                // Select command cyclically
                const char *cmd = compute_cmds[i % 3];
                execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);

                // If execl fails
                perror("execl failed");
                exit(1);
            } else if (pids[proc_idx] < 0) {
                perror("fork failed");
                exit(1);
            }
            process_types[proc_idx] = strdup("compute");
            process_indices[proc_idx] = proc_idx + 1;
            proc_idx++;
            usleep(10000);
        }

        for (int i = 0; i < io_count; i++) {
            pids[proc_idx] = fork();
            if (pids[proc_idx] == 0) {
                if (ld_preload) {
                    setenv("LD_PRELOAD", ld_preload, 1);
                }
                setenv("OMP_NUM_THREADS", OMP_NUM_THREADS, 1);

                snprintf(buf, sizeof(buf), "START,io,%d,%.0f", proc_idx + 1, get_time_ms());
                log_message(TIMING_LOG, buf);

                const char *cmd = io_cmds[i % 3];
                execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);

                perror("execl failed");
                exit(1);
            } else if (pids[proc_idx] < 0) {
                perror("fork failed");
                exit(1);
            }
            process_types[proc_idx] = strdup("io");
            process_indices[proc_idx] = proc_idx + 1;
            proc_idx++;
            usleep(10000);
        }

        for (int i = 0; i < memory_count; i++) {
            pids[proc_idx] = fork();
            if (pids[proc_idx] == 0) {
                if (ld_preload) {
                    setenv("LD_PRELOAD", ld_preload, 1);
                }
                setenv("OMP_NUM_THREADS", OMP_NUM_THREADS, 1);

                snprintf(buf, sizeof(buf), "START,memory,%d,%.0f", proc_idx + 1, get_time_ms());
                log_message(TIMING_LOG, buf);

                const char *cmd = memory_cmds[i % 3];
                execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);

                perror("execl failed");
                exit(1);
            } else if (pids[proc_idx] < 0) {
                perror("fork failed");
                exit(1);
            }
            process_types[proc_idx] = strdup("memory");
            process_indices[proc_idx] = proc_idx + 1;
            proc_idx++;
            usleep(10000);
        }

        // Wait for all processes
        for (int i = 0; i < num_processes; i++) {
            waitpid(pids[i], &status, 0);
            if (WIFEXITED(status)) {
                snprintf(buf, sizeof(buf), "END,%s,%d,%.0f", process_types[i], process_indices[i], get_time_ms());
                log_message(TIMING_LOG, buf);
            }
        }

        double total_time = (get_time_ms() - start_time) / 1000.0; // seconds
        double starvation_time = calculate_starvation(TIMING_LOG, num_processes);
        double max_latency = total_time; // Approximation
        double avg_time = total_time / num_processes;

        // Append results to CSV
        fp = fopen(RESULT_FILE, "a");
        if (fp) {
            fprintf(fp, "%d,%s,%d,%d,%d,%.3f,%.3f,%.3f,%.3f\n", test + 1, mode, compute_count, io_count, memory_count, total_time, max_latency, starvation_time, avg_time);
            fclose(fp);
        }

        snprintf(buf, sizeof(buf), "Test %d completed in %.3f seconds", test + 1, total_time);
        log_message(DEBUG_LOG, buf);

        // Clean up
        for (int i = 0; i < num_processes; i++) {
            free(process_types[i]);
        }
        free(pids);
        free(process_types);
        free(process_indices);
    }

    printf("All tests completed. Results in %s\n", RESULT_FILE);
    return 0;
}