#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <dlfcn.h>
#include "papi.h"
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "monitor.h"

const char *events[] = {
    "INST_RETIRED:ANY_P", "perf::PERF_COUNT_HW_CACHE_MISSES", "ix86arch::UNHALTED_CORE_CYCLES",
    "MEM_INST_RETIRED:ANY", "perf::FAULTS", "CYCLE_ACTIVITY:CYCLES_MEM_ANY", "adl_grt::UOPS_RETIRED"
};

#define SOCKET_PATH "/tmp/scheduler_socket"
#define MONITOR_PRINTF(fmt, ...) \
    printf("\033[32m[MONITOR]\033[0m: " fmt, ##__VA_ARGS__);

#define MONITOR_PERROR(fmt, ...) \
    fprintf(stderr, "\033[31m[MONITOR ERROR]\033[0m: " fmt, ##__VA_ARGS__);

typedef struct {
    int EventSet;
    pid_t tid;
    int active;
    long long initial_values[NUM_EVENTS];
    long long final_values[NUM_EVENTS];
    uint32_t cpu_bitmask;
} ThreadData;

static ThreadData thread_data[MAX_THREADS];
static int thread_count = 0;
static pid_t target_pid = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int results_output = 0;
static ProcessIOStats initial_io, final_io;
static struct timespec start_time;

static void handle_error(const char *msg, int retval, const char *event) {
    //MONITOR_PRINTF("Handling error\n");
    fprintf(stderr, "%s: %s (code: %d), Event: %s\n", msg, 
            retval == PAPI_ESYS ? strerror(errno) : PAPI_strerror(retval), 
            retval, event ? event : "none");
}

static int get_process_io_stats(pid_t pid, ProcessIOStats *stats) {
    //MONITOR_PRINTF("Getting process I/O stats for PID %d\n", pid);
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/io", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    memset(stats, 0, sizeof(ProcessIOStats));
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        sscanf(line, "rchar: %llu", &stats->rchar);
        sscanf(line, "wchar: %llu", &stats->wchar);
        sscanf(line, "syscr: %llu", &stats->syscr);
        sscanf(line, "syscw: %llu", &stats->syscw);
        sscanf(line, "read_bytes: %llu", &stats->read_bytes);
        sscanf(line, "write_bytes: %llu", &stats->write_bytes);
    }
    fclose(fp);
    return 0;
}

static int setup_eventset(pid_t tid, ThreadData *data) {
    //MONITOR_PRINTF("Setting up event set for TID %d\n", tid);
    int EventSet = PAPI_NULL;
    if (PAPI_create_eventset(&EventSet) != PAPI_OK) return -1;
    for (int i = 0; i < NUM_EVENTS; i++) {
        if (PAPI_add_named_event(EventSet, events[i]) != PAPI_OK) {
            handle_error("Failed to add event", PAPI_EINVAL, events[i]);
        }
    }
    if (PAPI_attach(EventSet, tid) != PAPI_OK || PAPI_start(EventSet) != PAPI_OK) {
        PAPI_destroy_eventset(&EventSet);
        return -1;
    }
    PAPI_read(EventSet, data->initial_values);
    return EventSet;
}

static void cleanup_thread(ThreadData *data) {
    //MONITOR_PRINTF("Cleaning up thread TID %d\n", data->tid);
    if (!data->active) return;
    PAPI_read(data->EventSet, data->final_values);
    PAPI_stop(data->EventSet, NULL);
    PAPI_destroy_eventset(&data->EventSet);
    data->active = 0;
}

void calculate_ratios(long long *total_values, ProcessIOStats *io_delta, PerformanceRatios *ratios) {
    //MONITOR_PRINTF("Calculating performance ratios\n");
    long long inst_retired = total_values[0];
    long long cache_misses = total_values[1];
    long long core_cycles = total_values[2];
    long long mem_retired = total_values[3];
    long long faults = total_values[4];
    long long mem_stall_cycles = total_values[5];
    long long uops_retired = total_values[6];

    ratios->IPC = core_cycles ? (double)inst_retired / core_cycles : 0.0;
    ratios->Cache_Miss_Ratio = mem_retired ? (double)cache_misses / mem_retired : 0.0;
    ratios->Uop_per_Cycle = core_cycles ? (double)uops_retired / core_cycles : 0.0;
    ratios->MemStallCycle_per_Mem_Inst = mem_retired ? (double)mem_stall_cycles / mem_retired : 0.0;
    ratios->MemStallCycle_per_Inst = inst_retired ? (double)mem_stall_cycles / inst_retired : 0.0;
    ratios->Fault_Rate_per_mem_instr = mem_retired ? (double)faults / mem_retired : 0.0;
    ratios->RChar_per_Cycle = core_cycles ? (double)io_delta->rchar / core_cycles : 0.0;
    ratios->WChar_per_Cycle = core_cycles ? (double)io_delta->wchar / core_cycles : 0.0;
    ratios->RBytes_per_Cycle = core_cycles ? (double)io_delta->read_bytes / core_cycles : 0.0;
    ratios->WBytes_per_Cycle = core_cycles ? (double)io_delta->write_bytes / core_cycles : 0.0;
}

static void send_to_scheduler(const MonitorData *data, int startup_flag) {
    //MONITOR_PRINTF("Sending %s to scheduler\n", startup_flag ? "startup notification" : "final data");
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        //MONITOR_PERROR("socket\n");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        //MONITOR_PERROR("connect\n");
        close(sock);
        return;
    }
    //MONITOR_PRINTF("Connected to scheduler\n");

    pid_t pid = getpid();
    write(sock, &pid, sizeof(pid));
    write(sock, &startup_flag, sizeof(int));
    write(sock, data, sizeof(MonitorData));

    close(sock);
    //MONITOR_PRINTF("Sent %s to scheduler\n", startup_flag ? "startup notification" : "final data");
}

static void output_results(void) {
    //MONITOR_PRINTF("Outputting results\n");
    if (results_output) return;
    results_output = 1;

    pthread_mutex_lock(&mutex);
    long long total_values[NUM_EVENTS] = {0};
    uint32_t global_cpu_bitmask = 0;

    for (size_t i = 0; i < thread_count; i++) {
        if (thread_data[i].active) {
            cleanup_thread(&thread_data[i]);
        }
        for (int j = 0; j < NUM_EVENTS; j++) {
            total_values[j] += thread_data[i].final_values[j] - thread_data[i].initial_values[j];
        }
        global_cpu_bitmask |= thread_data[i].cpu_bitmask;
    }

    int pcore_count = 0, ecore_count = 0, hw_thread_count = 0, pthread_count = 0;
    uint8_t pcore_used[8] = {0};
    for (int cpu = 0; cpu < 16; cpu++) {
        if (global_cpu_bitmask & (1U << cpu)) {
            int pcore_idx = cpu / 2;
            pcore_used[pcore_idx] = 1;
            pthread_count++;
        }
    }
    for (int i = 0; i < 8; i++) {
        pcore_count += pcore_used[i];
    }
    for (int cpu = 16; cpu < 32; cpu++) {
        if (global_cpu_bitmask & (1U << cpu)) {
            ecore_count++;
        }
    }
    hw_thread_count = pthread_count + ecore_count;
    int total_cores = pcore_count + ecore_count;

    MonitorData data = {0};
    data.thread_count = thread_count;
    data.hw_thread_count = hw_thread_count;
    data.pthread_count = pthread_count;
    data.pcore_count = pcore_count;
    data.ecore_count = ecore_count;
    data.total_cores = total_cores;
    memcpy(data.total_values, total_values, sizeof(total_values));
    data.io_delta = (ProcessIOStats){
        final_io.rchar - initial_io.rchar,
        final_io.wchar - initial_io.wchar,
        final_io.syscr - initial_io.syscr,
        final_io.syscw - initial_io.syscw,
        final_io.read_bytes - initial_io.read_bytes,
        final_io.write_bytes - initial_io.write_bytes
    };
    calculate_ratios(total_values, &data.io_delta, &data.ratios);
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    data.exec_time_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end_time.tv_nsec - start_time.tv_nsec) / 1e6;

    send_to_scheduler(&data, 0);

    pthread_mutex_unlock(&mutex);
}

static void *thread_wrapper(void *arg) {
    //MONITOR_PRINTF("Thread wrapper started\n");
    void *(*start_routine)(void *) = ((void **)arg)[0];
    void *start_arg = ((void **)arg)[1];
    pthread_mutex_lock(&mutex);
    if (thread_count < MAX_THREADS) {
        thread_data[thread_count].tid = syscall(SYS_gettid);
        thread_data[thread_count].active = 1;
        thread_data[thread_count].cpu_bitmask = 0;
        int cpu = sched_getcpu();
        if (cpu >= 0 && cpu < MAX_CPUS) {
            thread_data[thread_count].cpu_bitmask = (1U << cpu);
            //MONITOR_PRINTF("Thread %d assigned to CPU %d\n", thread_data[thread_count].tid, cpu);
        } else {
            fprintf(stderr, "Thread %d: sched_getcpu failed (%s)\n", thread_data[thread_count].tid, strerror(errno));
        }
        thread_data[thread_count].EventSet = setup_eventset(thread_data[thread_count].tid, &thread_data[thread_count]);
        if (thread_data[thread_count].EventSet != -1) {
            thread_count++;
        }
    }
    pthread_mutex_unlock(&mutex);
    void *ret = start_routine(start_arg);
    free(arg);
    return ret;
}

int clone(int (*fn)(void *), void *child_stack, int flags, void *arg, ...) {
    //MONITOR_PRINTF("Clone called\n");
    static int (*real_clone)(int (*)(void *), void *, int, void *, ...) = NULL;
    if (!real_clone) {
        real_clone = dlsym(RTLD_NEXT, "clone");
        if (!real_clone) exit(1);
    }
    int tid = real_clone(fn, child_stack, flags, arg);
    if (tid == 0 && (flags & CLONE_THREAD)) {
        pthread_mutex_lock(&mutex);
        if (thread_count < MAX_THREADS) {
            thread_data[thread_count].tid = syscall(SYS_gettid);
            thread_data[thread_count].active = 1;
            thread_data[thread_count].cpu_bitmask = 0;
            int cpu = sched_getcpu();
            if (cpu >= 0 && cpu < MAX_CPUS) {
                thread_data[thread_count].cpu_bitmask = (1U << cpu);
            }
            thread_data[thread_count].EventSet = setup_eventset(thread_data[thread_count].tid, &thread_data[thread_count]);
            if (thread_count < MAX_THREADS && thread_data[thread_count].EventSet != -1) {
                thread_count++;
            }
        }
        pthread_mutex_unlock(&mutex);
    }
    return tid;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg) {
    //MONITOR_PRINTF("Pthread create called\n");
    static int (*real_pthread_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = NULL;
    if (!real_pthread_create) {
        real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
        if (!real_pthread_create) exit(1);
    }
    void **wrapper_arg = malloc(sizeof(void *) * 2);
    wrapper_arg[0] = start_routine;
    wrapper_arg[1] = arg;
    int ret = real_pthread_create(thread, attr, thread_wrapper, wrapper_arg);
    if (ret != 0) {
        free(wrapper_arg);
    }
    return ret;
}

int pthread_join(pthread_t thread, void **retval) {
    //MONITOR_PRINTF("Pthread join called\n");
    static int (*real_pthread_join)(pthread_t, void **) = NULL;
    if (!real_pthread_join) {
        real_pthread_join = dlsym(RTLD_NEXT, "pthread_join");
        if (!real_pthread_join) exit(1);
    }
    int ret = real_pthread_join(thread, retval);
    if (ret == 0) {
        pthread_mutex_lock(&mutex);
        for (int i = 0; i < thread_count; i++) {
            if (thread_data[i].tid == syscall(SYS_gettid) && thread_data[i].active) {
                cleanup_thread(&thread_data[i]);
                break;
            }
        }
        pthread_mutex_unlock(&mutex);
    }
    return ret;
}

__attribute__((noreturn)) void pthread_exit(void *retval) {
    //MONITOR_PRINTF("Pthread exit called\n");
    static void (*real_pthread_exit)(void *) = NULL;
    if (!real_pthread_exit) {
        real_pthread_exit = dlsym(RTLD_NEXT, "pthread_exit");
        if (!real_pthread_exit) exit(1);
    }
    pthread_mutex_lock(&mutex);
    for (int i = 0; i < thread_count; i++) {
        if (thread_data[i].tid == syscall(SYS_gettid) && thread_data[i].active) {
            cleanup_thread(&thread_data[i]);
            break;
        }
    }
    pthread_mutex_unlock(&mutex);
    real_pthread_exit(retval);
    __builtin_unreachable();
}

__attribute__((constructor))
void init_monitor(void) {
    //MONITOR_PRINTF("Initializing monitor\n");
    static int initialized = 0;
    //MONITOR_PRINTF("Monitor initialized with flag=%d\n", initialized);
    if (initialized) return;
    initialized = 1;

    target_pid = getpid();
    PAPI_library_init(PAPI_VER_CURRENT);
    PAPI_thread_init(pthread_self);
    get_process_io_stats(target_pid, &initial_io);
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    pthread_mutex_lock(&mutex);
    if (thread_count < MAX_THREADS) {
        thread_data[thread_count].tid = syscall(SYS_gettid);
        thread_data[thread_count].active = 1;
        thread_data[thread_count].cpu_bitmask = 0;
        int cpu = sched_getcpu();
        if (cpu >= 0 && cpu < MAX_CPUS) {
            thread_data[thread_count].cpu_bitmask = (1U << cpu);
            //MONITOR_PRINTF("Main thread %d assigned to CPU %d\n", thread_data[thread_count].tid, cpu);
        }
        thread_data[thread_count].EventSet = setup_eventset(thread_data[thread_count].tid, &thread_data[thread_count]);
        if (thread_data[thread_count].EventSet != -1) {
            thread_count++;
        }
    }
    pthread_mutex_unlock(&mutex);

    MonitorData initial_data = {0};
    send_to_scheduler(&initial_data, 1);
}

__attribute__((destructor))
void finish_monitor(void) {
    //MONITOR_PRINTF("Finalizing monitor\n");
    get_process_io_stats(target_pid, &final_io);
    output_results();
    //MONITOR_PRINTF("End\n");
}