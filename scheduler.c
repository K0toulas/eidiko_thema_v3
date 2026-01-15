#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <math.h>
#include "monitor.h"
#include "libclassifier.h"

#define SOCKET_PATH "/tmp/scheduler_socket"
#define CSV_FILE "classifier_val.csv"
#define CORE_ALLOCATION_CSV "core_allocation.csv"
#define MAX_CORES 16
#define COMPUTE_CORESET "0,1,2,3,4,5,6,7"
#define IO_CORESET "8-15"
#define MEMORY_CORESET "0,1,2,3,4,5,6,7"
#define MAX_QUEUE_SIZE 2048
#define SCHEDULER_SLEEP_MILLISECONDS 100
#ifndef QUIET_SCHEDULER
#define SCHEDULER_PRINTF(fmt, ...) \
    printf("\033[33m[SCHEDULER]\033[0m: " fmt, ##__VA_ARGS__)
#else
#define SCHEDULER_PRINTF(fmt, ...) /* No-op */
#endif
#define SCHEDULER_PERROR(fmt, ...) \
    fprintf(stderr, "\033[31m[SCHEDULER ERROR]\033[0m: " fmt, ##__VA_ARGS__)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))









typedef struct {
    char compute_coreset[256];
    char io_coreset[256];
    char memory_coreset[256];
} DynamicCoreMasks;

typedef struct {
    pid_t pid;
    MonitorData current_data;
    MonitorData *history;
    int history_count;
    int history_capacity;
    MonitorData last_used;
    int has_last_used;
    int startup_flag;
    char predicted_class[16];
} QueueEntry;

static QueueEntry queue[MAX_QUEUE_SIZE];
static int queue_size = 0;
static int compute_threads = 0;
static int io_threads = 0;
static int memory_threads = 0;
static DynamicCoreMasks prev_masks = { {0}, {0}, {0} };

// Safe process existence check
static int is_process_alive(pid_t pid) {
    if (kill(pid, 0) == 0) return 1; // Process exists
    return errno != ESRCH; // Return 0 only if process definitely doesn't exist
}

void get_current_core(pid_t pid, int *core, int *is_pcore) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (sched_getaffinity(pid, sizeof(cpu_set_t), &cpuset) == -1) {
        *core = -1;
        *is_pcore = 0;
        return;
    }
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &cpuset)) {
            *core = i;
            *is_pcore = (i < 16) ? 1 : 0; // Assume 0-15 are P-cores, 16-31 are E-cores
            return;
        }
    }
    *core = -1;
    *is_pcore = 0;
}

void set_thread_priority(pid_t tid, int priority, const char *class, int core, int is_pcore) {
    struct sched_param param;
    param.sched_priority = priority;
    if (sched_setscheduler(tid, SCHED_FIFO, &param) == -1) {
        fprintf(stderr, "Failed to set priority %d for TID %d: %s\n", priority, tid, strerror(errno));
    }
}

static void init_queue_entry(QueueEntry *entry) {
    entry->pid = 0;
    memset(&entry->current_data, 0, sizeof(MonitorData));
    entry->history = NULL;
    entry->history_count = 0;
    entry->history_capacity = 0;
    memset(&entry->last_used, 0, sizeof(MonitorData));
    entry->has_last_used = 0;
    entry->startup_flag = 0;
}

// Safe queue entry removal
static void remove_queue_entry(int index) {
    SCHEDULER_PRINTF("Removing PID %d from queue\n", queue[index].pid);
    if (queue[index].history) {
        free(queue[index].history);
        queue[index].history = NULL;
    }
    for (int i = index; i < queue_size - 1; i++) {
        queue[i] = queue[i + 1];
    }
    init_queue_entry(&queue[queue_size - 1]);
    queue_size--;
}

static void free_queue_entry(QueueEntry *entry) {
    if (entry->history) {
        free(entry->history);
        entry->history = NULL;
    }
    entry->history_count = 0;
    entry->history_capacity = 0;
    entry->pid = 0;
    memset(&entry->current_data, 0, sizeof(MonitorData));
    memset(&entry->last_used, 0, sizeof(MonitorData));
    entry->has_last_used = 0;
    entry->startup_flag = 0;
}

static void parse_coreset(const char *coreset, int *cores, int *core_count) {
    *core_count = 0;
    if (!coreset || !coreset[0]) return;
    char *copy = strdup(coreset);
    if (!copy) {
        SCHEDULER_PERROR("Failed to allocate memory for coreset parsing\n");
        return;
    }
    char *token = strtok(copy, ",");
    while (token) {
        if (strchr(token, '-')) {
            int start, end;
            if (sscanf(token, "%d-%d", &start, &end) == 2) {
                for (int i = start; i <= end && *core_count < MAX_CORES; i++) {
                    if (i >= 0 && i < MAX_CORES) cores[(*core_count)++] = i;
                }
            }
        } else {
            int core = atoi(token);
            if (core >= 0 && core < MAX_CORES && *core_count < MAX_CORES) {
                cores[(*core_count)++] = core;
            }
        }
        token = strtok(NULL, ",");
    }
    free(copy);
}

static int int_compare(const void *a, const void *b) {
    return *(int *)a - *(int *)b;
}

static void cores_to_string(int *cores, int core_count, char *coreset, size_t size) {
    if (core_count == 0) {
        coreset[0] = '\0';
        return;
    }
    qsort(cores, core_count, sizeof(int), int_compare);
    coreset[0] = '\0';
    int i = 0;
    while (i < core_count) {
        int start = cores[i];
        int end = start;
        while (i + 1 < core_count && cores[i + 1] == end + 1) {
            end++;
            i++;
        }
        char temp[32];
        if (start == end) {
            snprintf(temp, sizeof(temp), "%d", start);
        } else {
            snprintf(temp, sizeof(temp), "%d-%d", start, end);
        }
        if (coreset[0]) {
            strncat(coreset, ",", size - strlen(coreset) - 1);
        }
        strncat(coreset, temp, size - strlen(coreset) - 1);
        i++;
    }
}

static int count_cores(const char *coreset) {
    int cores[64];
    int core_count = 0;
    parse_coreset(coreset, cores, &core_count);
    return core_count;
}

static void compute_dynamic_coresets(DynamicCoreMasks *masks) {
    int total_threads = compute_threads + io_threads + memory_threads;

    if (total_threads == 0) {
        strcpy(masks->compute_coreset, COMPUTE_CORESET);
        strcpy(masks->io_coreset, IO_CORESET);
        strcpy(masks->memory_coreset, MEMORY_CORESET);
        SCHEDULER_PRINTF("No threads, reset: Compute=%s, IO=%s, Memory=%s\n",
                         masks->compute_coreset, masks->io_coreset, masks->memory_coreset);
        strcpy(prev_masks.compute_coreset, masks->compute_coreset);
        strcpy(prev_masks.io_coreset, masks->io_coreset);
        strcpy(prev_masks.memory_coreset, masks->memory_coreset);
        return;
    }

    int pcores[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    int ecores[16] = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    int pcore_count = 16, ecore_count = 16;
    int compute_cores[64] = {0}, io_cores[64] = {0}, memory_cores[64] = {0};
    int compute_core_count = 0, io_core_count = 0, memory_core_count = 0;

    int min_cores = 1;
    int active_compute = compute_threads > 0;
    int active_memory = memory_threads > 0;
    int active_io = io_threads > 0;
    int active_classes = active_compute + active_memory + active_io;
    int reserved_total = min_cores * active_classes;
    int remaining_total = MAX_CORES - reserved_total;

    int effective_compute = compute_threads;
    int effective_memory = memory_threads >> 2;  // Reduce weight for memory to avoid saturation and give compute more per thread
    int effective_io = io_threads;
    double total_effective = (double)(effective_compute + effective_memory + effective_io);
    if (total_effective == 0) total_effective = 1.0;  // Avoid division by zero

    int desired_compute = active_compute ? min_cores + (int)(remaining_total * effective_compute / total_effective) : 0;
    int desired_io = active_io ? min_cores + (int)(remaining_total * effective_io / total_effective) : 0;
    int desired_memory = active_memory ? min_cores + (int)(remaining_total * effective_memory / total_effective) : 0;

    // Adjust totals if sum != MAX_CORES (due to rounding)
    int total_desired = desired_compute + desired_io + desired_memory;
    if (total_desired < MAX_CORES) {
        if (active_compute) desired_compute += MAX_CORES - total_desired;
        else if (active_memory) desired_memory += MAX_CORES - total_desired;
        else if (active_io) desired_io += MAX_CORES - total_desired;
    } else if (total_desired > MAX_CORES) {
        if (active_io) {
            desired_io -= total_desired - MAX_CORES;
            desired_io = MAX(desired_io, min_cores);
        } else if (active_memory) {
            desired_memory -= total_desired - MAX_CORES;
            desired_memory = MAX(desired_memory, min_cores);
        } else if (active_compute) {
            desired_compute -= total_desired - MAX_CORES;
            desired_compute = MAX(desired_compute, min_cores);
        }
    }

    // Reserve min 1 P-core for each active class
    int p_idx = 0;  // Index for assigning from pcores array
    // if (active_compute) {
    //     compute_cores[compute_core_count++] = pcores[p_idx++];
    //     desired_compute = MAX(desired_compute - 1, 0);
    //     SCHEDULER_PRINTF("Reserved 1 P-core for Compute\n");
    // }
    // if (active_memory) {
    //     memory_cores[memory_core_count++] = pcores[p_idx++];
    //     desired_memory = MAX(desired_memory - 1, 0);
    //     SCHEDULER_PRINTF("Reserved 1 P-core for Memory\n");
    // }
    // if (active_io) {
    //     io_cores[io_core_count++] = pcores[p_idx++];
    //     desired_io = MAX(desired_io - 1, 0);
    //     SCHEDULER_PRINTF("Reserved 1 P-core for IO\n");
    // }
    // pcore_count -= active_classes;  // Reduce available P-cores

    // Allocate remaining P-cores proportionally to compute and memory (equal priority via effective weights)
    int remaining_p = pcore_count;
    double p_weight = (double)(effective_compute + effective_memory);
    if (p_weight > 0) {
        int p_compute = (int)(remaining_p * effective_compute / p_weight);
        int p_memory = remaining_p - p_compute;

        // Assign to compute
        int cores_needed = MIN(p_compute, pcore_count);
        for (int i = 0; i < cores_needed; i++) {
            compute_cores[compute_core_count++] = pcores[p_idx++];
        }
        desired_compute = MAX(desired_compute - cores_needed, 0);
        pcore_count -= cores_needed;
        SCHEDULER_PRINTF("Compute assigned %d additional P-cores (proportional)\n", cores_needed);

        // Assign to memory
        cores_needed = MIN(p_memory, pcore_count);
        for (int i = 0; i < cores_needed; i++) {
            memory_cores[memory_core_count++] = pcores[p_idx++];
        }
        desired_memory = MAX(desired_memory - cores_needed, 0);
        pcore_count -= cores_needed;
        SCHEDULER_PRINTF("Memory assigned %d additional P-cores (proportional)\n", cores_needed);
    }

    // Assign E-cores to IO first (preferred for IO)
    if (active_io && desired_io > 0) {
        int cores_needed = MIN(desired_io, ecore_count);
        for (int i = 0; i < cores_needed; i++) {
            io_cores[io_core_count++] = ecores[i];
        }
        memmove(ecores, ecores + cores_needed, (ecore_count - cores_needed) * sizeof(int));
        ecore_count -= cores_needed;
        desired_io -= cores_needed;
        SCHEDULER_PRINTF("IO assigned %d E-cores\n", cores_needed);
    }

    // Assign remaining P-cores to IO if still needed (spillover)
    if (active_io && desired_io > 0 && pcore_count > 0) {
        int cores_needed = MIN(desired_io, pcore_count);
        for (int i = 0; i < cores_needed; i++) {
            io_cores[io_core_count++] = pcores[p_idx++];
        }
        pcore_count -= cores_needed;
        desired_io -= cores_needed;
        SCHEDULER_PRINTF("IO assigned %d additional P-cores (spillover)\n", cores_needed);
    }

    // Distribute remaining P-cores to compute and memory if needed
    if (active_compute && desired_compute > 0 && pcore_count > 0) {
        int cores_needed = MIN(desired_compute, pcore_count);
        for (int i = 0; i < cores_needed; i++) {
            compute_cores[compute_core_count++] = pcores[p_idx++];
        }
        pcore_count -= cores_needed;
        desired_compute -= cores_needed;
        SCHEDULER_PRINTF("Compute assigned %d additional P-cores\n", cores_needed);
    }
    if (active_memory && desired_memory > 0 && pcore_count > 0) {
        int cores_needed = MIN(desired_memory, pcore_count);
        for (int i = 0; i < cores_needed; i++) {
            memory_cores[memory_core_count++] = pcores[p_idx++];
        }
        pcore_count -= cores_needed;
        desired_memory -= cores_needed;
        SCHEDULER_PRINTF("Memory assigned %d additional P-cores\n", cores_needed);
    }

    // Distribute remaining E-cores to compute and memory if needed
    if (active_compute && desired_compute > 0 && ecore_count > 0) {
        int cores_needed = MIN(desired_compute, ecore_count);
        for (int i = 0; i < cores_needed; i++) {
            compute_cores[compute_core_count++] = ecores[i];
        }
        memmove(ecores, ecores + cores_needed, (ecore_count - cores_needed) * sizeof(int));
        ecore_count -= cores_needed;
        desired_compute -= cores_needed;
        SCHEDULER_PRINTF("Compute assigned %d E-cores\n", cores_needed);
    }
    if (active_memory && desired_memory > 0 && ecore_count > 0) {
        int cores_needed = MIN(desired_memory, ecore_count);
        for (int i = 0; i < cores_needed; i++) {
            memory_cores[memory_core_count++] = ecores[i];
        }
        memmove(ecores, ecores + cores_needed, (ecore_count - cores_needed) * sizeof(int));
        ecore_count -= cores_needed;
        desired_memory -= cores_needed;
        SCHEDULER_PRINTF("Memory assigned %d E-cores\n", cores_needed);
    }

    // Convert to strings
    cores_to_string(compute_cores, compute_core_count, masks->compute_coreset, sizeof(masks->compute_coreset));
    cores_to_string(io_cores, io_core_count, masks->io_coreset, sizeof(masks->io_coreset));
    cores_to_string(memory_cores, memory_core_count, masks->memory_coreset, sizeof(masks->memory_coreset));

    // Ensure minimum 1 core per class (fallback, though reservations should prevent empty)
    if (masks->compute_coreset[0] == '\0' && active_compute) {
        strcpy(masks->compute_coreset, "0");
        compute_core_count = 1;
        SCHEDULER_PRINTF("Compute coreset fallback to 0\n");
    }
    if (masks->io_coreset[0] == '\0' && active_io) {
        strcpy(masks->io_coreset, "16");
        io_core_count = 1;
        SCHEDULER_PRINTF("IO coreset fallback to 16\n");
    }
    if (masks->memory_coreset[0] == '\0' && active_memory) {
        strcpy(masks->memory_coreset, "1");
        memory_core_count = 1;
        SCHEDULER_PRINTF("Memory coreset fallback to 1\n");
    }

    // Validate no overlaps
    int used_cores[MAX_CORES] = {0};
    for (int i = 0; i < compute_core_count; i++) {
        if (used_cores[compute_cores[i]]) {
            SCHEDULER_PERROR("Core %d assigned multiple times\n", compute_cores[i]);
            strcpy(masks->compute_coreset, prev_masks.compute_coreset);
            strcpy(masks->io_coreset, prev_masks.io_coreset);
            strcpy(masks->memory_coreset, prev_masks.memory_coreset);
            return;
        }
        used_cores[compute_cores[i]] = 1;
    }
    for (int i = 0; i < io_core_count; i++) {
        if (used_cores[io_cores[i]]) {
            SCHEDULER_PERROR("Core %d assigned multiple times\n", io_cores[i]);
            strcpy(masks->compute_coreset, prev_masks.compute_coreset);
            strcpy(masks->io_coreset, prev_masks.io_coreset);
            strcpy(masks->memory_coreset, prev_masks.memory_coreset);
            return;
        }
        used_cores[io_cores[i]] = 1;
    }
    for (int i = 0; i < memory_core_count; i++) {
        if (used_cores[memory_cores[i]]) {
            SCHEDULER_PERROR("Core %d assigned multiple times\n", memory_cores[i]);
            strcpy(masks->compute_coreset, prev_masks.compute_coreset);
            strcpy(masks->io_coreset, prev_masks.io_coreset);
            strcpy(masks->memory_coreset, prev_masks.memory_coreset);
            return;
        }
        used_cores[memory_cores[i]] = 1;
    }

    // Validate total cores
    if (compute_core_count + io_core_count + memory_core_count > MAX_CORES) {
        SCHEDULER_PERROR("Total cores %d exceeds MAX_CORES %d\n",
                         compute_core_count + io_core_count + memory_core_count, MAX_CORES);
        strcpy(masks->compute_coreset, prev_masks.compute_coreset);
        strcpy(masks->io_coreset, prev_masks.io_coreset);
        strcpy(masks->memory_coreset, prev_masks.memory_coreset);
        return;
    }

    SCHEDULER_PRINTF("Updated: Compute=%s (%d), IO=%s (%d), Memory=%s (%d)\n",
                     masks->compute_coreset, compute_core_count,
                     masks->io_coreset, io_core_count,
                     masks->memory_coreset, memory_core_count);

    strcpy(prev_masks.compute_coreset, masks->compute_coreset);
    strcpy(prev_masks.io_coreset, masks->io_coreset);
    strcpy(prev_masks.memory_coreset, masks->memory_coreset);
}

static int init_core_allocation_csv() {
    SCHEDULER_PRINTF("Initializing core allocation CSV file\n");
    FILE *fp = fopen(CORE_ALLOCATION_CSV, "w");
    if (!fp) {
        SCHEDULER_PERROR("Failed to open core allocation CSV file\n");
        return -1;
    }
    fprintf(fp, "Compute Bound Thread Num,I/O Bound Thread Num,Memory Bound Thread Num");
    for (int i = 0; i < MAX_CORES; i++) {
        fprintf(fp, ",Core %d", i);
    }
    fprintf(fp, "\n");
    fclose(fp);
    return 0;
}

static void log_core_allocation(DynamicCoreMasks *masks) {
    SCHEDULER_PRINTF("Logging core allocation to CSV\n");

    int core_assignment[MAX_CORES] = {0};
    char *copy;
    char *token;

    copy = strdup(masks->compute_coreset);
    if (copy) {
        token = strtok(copy, ",");
        while (token) {
            if (strchr(token, '-')) {
                int start, end;
                if (sscanf(token, "%d-%d", &start, &end) == 2) {
                    for (int i = start; i <= end; i++) {
                        if (i >= 0 && i < MAX_CORES) core_assignment[i] = 0;
                    }
                }
            } else {
                int cpu = atoi(token);
                if (cpu >= 0 && cpu < MAX_CORES) core_assignment[cpu] = 0;
            }
            token = strtok(NULL, ",");
        }
        free(copy);
    }

    copy = strdup(masks->io_coreset);
    if (copy) {
        token = strtok(copy, ",");
        while (token) {
            if (strchr(token, '-')) {
                int start, end;
                if (sscanf(token, "%d-%d", &start, &end) == 2) {
                    for (int i = start; i <= end; i++) {
                        if (i >= 0 && i < MAX_CORES) core_assignment[i] = 1;
                    }
                }
            } else {
                int cpu = atoi(token);
                if (cpu >= 0 && cpu < MAX_CORES) core_assignment[cpu] = 1;
            }
            token = strtok(NULL, ",");
        }
        free(copy);
    }

    copy = strdup(masks->memory_coreset);
    if (copy) {
        token = strtok(copy, ",");
        while (token) {
            if (strchr(token, '-')) {
                int start, end;
                if (sscanf(token, "%d-%d", &start, &end) == 2) {
                    for (int i = start; i <= end; i++) {
                        if (i >= 0 && i < MAX_CORES) core_assignment[i] = 2;
                    }
                }
            } else {
                int cpu = atoi(token);
                if (cpu >= 0 && cpu < MAX_CORES) core_assignment[cpu] = 2;
            }
            token = strtok(NULL, ",");
        }
        free(copy);
    }

    FILE *fp = fopen(CORE_ALLOCATION_CSV, "a");
    if (!fp) {
        SCHEDULER_PERROR("Failed to append to core allocation CSV\n");
        return;
    }
    fprintf(fp, "%d,%d,%d", compute_threads, io_threads, memory_threads);
    for (int i = 0; i < MAX_CORES; i++) {
        fprintf(fp, ",%d", core_assignment[i]);
    }
    fprintf(fp, "\n");
    fclose(fp);
}

int init_csv() {
    SCHEDULER_PRINTF("Initializing CSV file\n");
    FILE *fp = fopen(CSV_FILE, "w");
    if (!fp) {
        SCHEDULER_PERROR("Failed to open CSV file\n");
        return -1;
    }
    fprintf(fp, "P-Threads,P-Cores,E-Cores,INST_RETIRED:ANY_P,PERF_COUNT_HW_CACHE_MISSES,UNHALTED_CORE_CYCLES,MEM_INST_RETIRED:ANY,FAULTS,CYCLES_MEM_ANY,UOPS_RETIRED,IPC,Cache_Miss_Ratio,Uop_per_Cycle,MemStallCycle_per_Mem_Inst,MemStallCycle_per_Inst,Fault_Rate_per_mem_instr,RChar_per_Cycle,WChar_per_Cycle,RBytes_per_Cycle,WBytes_per_Cycle,syscr,syscw,Execution Time (ms),rchar,wchar,read_bytes,write_bytes,Compute_Prob_CJSON,IO_Prob_CJSON,Memory_Prob_CJSON,Class_Time_CJSON (us),Expected_Class\n");
    fclose(fp);
    return 0;
}

void set_affinity(pid_t pid, const char *coreset) {
    if (!coreset || !coreset[0]) {
        SCHEDULER_PERROR("Empty coreset for PID %d\n", pid);
        return;
    }
    SCHEDULER_PRINTF("Setting affinity for PID %d to coreset %s\n", pid, coreset);
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    char *copy = strdup(coreset);
    if (!copy) {
        SCHEDULER_PERROR("Failed to allocate memory for coreset\n");
        return;
    }
    char *token = strtok(copy, ",");
    while (token) {
        if (strchr(token, '-')) {
            int start, end;
            if (sscanf(token, "%d-%d", &start, &end) == 2) {
                for (int i = start; i <= end && i < MAX_CORES; i++) {
                    if (i >= 0) CPU_SET(i, &cpuset);
                }
            }
        } else {
            int cpu = atoi(token);
            if (cpu >= 0 && cpu < MAX_CORES) CPU_SET(cpu, &cpuset);
        }
        token = strtok(NULL, ",");
    }
    free(copy);
    
    if (sched_setaffinity(pid, sizeof(cpu_set_t), &cpuset) == -1) {
        SCHEDULER_PERROR("Failed to set affinity for PID %d: %s\n", pid, strerror(errno));
    }
}

void set_affinity_for_all_threads(pid_t pid, const char *coreset) {
    if (!is_process_alive(pid)) {
        SCHEDULER_PRINTF("PID %d not alive, skipping affinity\n", pid);
        return;
    }
    set_affinity(pid, coreset);
    
    char task_path[256];
    snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);
    
    DIR *dir = opendir(task_path);
    if (!dir) {
        SCHEDULER_PERROR("Failed to open task directory for PID %d: %s\n", pid, strerror(errno));
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        pid_t tid = atoi(entry->d_name);
        if (tid > 0 && tid != pid && is_process_alive(tid)) {
            set_affinity(tid, coreset);
        }
    }
    closedir(dir);
}

void verify_affinity(pid_t pid) {
    if (!is_process_alive(pid)) {
        SCHEDULER_PRINTF("PID %d not alive, skipping verification\n", pid);
        return;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ps -mo pid,tid,psr,cmd -p %d", pid);
    SCHEDULER_PRINTF("Verifying affinity for PID %d:\n", pid);
    if (system(cmd) != 0) {
        SCHEDULER_PERROR("Failed to execute ps command for PID %d\n", pid);
    }
}

void schedule_process(pid_t pid, MonitorData data, int startup_flag) {
    SCHEDULER_PRINTF("Scheduling PID %d (startup=%d)\n", pid, startup_flag);
    if (startup_flag) {
        set_affinity_for_all_threads(pid, "0-16");
        return;
    }
}

void write_to_csv(const MonitorData *data, long class_time_cjson, const char *predicted_class) {
    FILE *fp = fopen(CSV_FILE, "a");
    if (!fp) {
        SCHEDULER_PERROR("Failed to append to CSV file\n");
        return;
    }
    fprintf(fp, "%d,%d,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
                "%.15lf,%.15lf,%.15lf,%.15lf,%.15lf,%.15lf,%.15lf,%.15lf,%.15lf,%.15lf,"
                "%llu,%llu,%lf,%llu,%llu,%llu,%llu,"
                "%.15lf,%.15lf,%.15lf,%ld,%s\n",
            data->pthread_count, data->pcore_count, data->ecore_count,
            data->total_values[0], data->total_values[1], data->total_values[2],
            data->total_values[3], data->total_values[4], data->total_values[5],
            data->total_values[6], data->ratios.IPC, data->ratios.Cache_Miss_Ratio,
            data->ratios.Uop_per_Cycle, data->ratios.MemStallCycle_per_Mem_Inst,
            data->ratios.MemStallCycle_per_Inst, data->ratios.Fault_Rate_per_mem_instr,
            data->ratios.RChar_per_Cycle, data->ratios.WChar_per_Cycle,
            data->ratios.RBytes_per_Cycle, data->ratios.WBytes_per_Cycle,
            data->io_delta.syscr, data->io_delta.syscw, data->exec_time_ms,
            data->io_delta.rchar, data->io_delta.wchar, data->io_delta.read_bytes,
            data->io_delta.write_bytes, data->compute_prob_cjson, data->io_prob_cjson,
            data->memory_prob_cjson, class_time_cjson, predicted_class);
    fclose(fp);
}

static int add_to_queue(pid_t pid, MonitorData data, int startup_flag) {
    SCHEDULER_PRINTF("Adding PID %d to queue\n", pid);
    
    if (!is_process_alive(pid)) {
        SCHEDULER_PRINTF("PID %d does not exist, not adding to queue\n", pid);
        return -1;
    }

    for (int i = 0; i < queue_size; i++) {
        if (queue[i].pid == pid) {
            if (!is_process_alive(pid)) {
                SCHEDULER_PRINTF("PID %d found in queue but dead, removing\n", pid);
                remove_queue_entry(i);
                break;
            }
            
            if (queue[i].history_count >= queue[i].history_capacity) {
                int new_capacity = queue[i].history_capacity == 0 ? 4 : queue[i].history_capacity * 2;
                MonitorData *new_history = realloc(queue[i].history, new_capacity * sizeof(MonitorData));
                if (!new_history) {
                    SCHEDULER_PERROR("Failed to allocate history for PID %d\n", pid);
                    return -1;
                }
                queue[i].history = new_history;
                queue[i].history_capacity = new_capacity;
            }
            queue[i].history[queue[i].history_count++] = data;
            queue[i].current_data = data;
            queue[i].startup_flag = startup_flag;
            return 0;
        }
    }

    if (queue_size >= MAX_QUEUE_SIZE) {
        SCHEDULER_PERROR("Queue full, cannot add PID %d\n", pid);
        return -1;
    }
    
    if (!is_process_alive(pid)) {
        SCHEDULER_PRINTF("PID %d died during add operation, not adding\n", pid);
        return -1;
    }

    init_queue_entry(&queue[queue_size]);
    queue[queue_size].pid = pid;
    queue[queue_size].history = malloc(4 * sizeof(MonitorData));
    if (!queue[queue_size].history) {
        SCHEDULER_PERROR("Failed to allocate history for PID %d\n", pid);
        return -1;
    }
    queue[queue_size].history_capacity = 4;
    queue[queue_size].history[0] = data;
    queue[queue_size].history_count = 1;
    queue[queue_size].current_data = data;
    queue[queue_size].startup_flag = startup_flag;
    queue_size++;
    return 0;
}

static void compute_weighted_ratios(pid_t pid, MonitorData *data, MonitorData *history, 
                                   int history_count, MonitorData *last_used, int has_last_used) {
    if (!is_process_alive(pid)) {
        SCHEDULER_PRINTF("PID %d not alive, skipping ratio computation\n", pid);
        return;
    }

    double denominator = 1.0;
    double weights[history_count + has_last_used + 1];
    weights[0] = 1.0;
    for (int i = 0; i < history_count; i++) {
        weights[i + 1] = 1.0 / (1 << (i + 1));
        denominator += weights[i + 1];
    }
    if (has_last_used) {
        weights[history_count + 1] = 1.0 / (1 << (history_count + 1));
        denominator += weights[history_count + 1];
    }

    double ipc = data->ratios.IPC * weights[0];
    double cache_miss = data->ratios.Cache_Miss_Ratio * weights[0];
    double uop_per_cycle = data->ratios.Uop_per_Cycle * weights[0];
    double mem_stall_per_mem = data->ratios.MemStallCycle_per_Mem_Inst * weights[0];
    double mem_stall_per_inst = data->ratios.MemStallCycle_per_Inst * weights[0];
    double fault_rate = data->ratios.Fault_Rate_per_mem_instr * weights[0];
    double rchar_per_cycle = data->ratios.RChar_per_Cycle * weights[0];
    double wchar_per_cycle = data->ratios.WChar_per_Cycle * weights[0];
    double rbytes_per_cycle = data->ratios.RBytes_per_Cycle * weights[0];
    double wbytes_per_cycle = data->ratios.WBytes_per_Cycle * weights[0];

    for (int i = 0; i < history_count; i++) {
        ipc += history[i].ratios.IPC * weights[i + 1];
        cache_miss += history[i].ratios.Cache_Miss_Ratio * weights[i + 1];
        uop_per_cycle += history[i].ratios.Uop_per_Cycle * weights[i + 1];
        mem_stall_per_mem += history[i].ratios.MemStallCycle_per_Mem_Inst * weights[i + 1];
        mem_stall_per_inst += history[i].ratios.MemStallCycle_per_Inst * weights[i + 1];
        fault_rate += history[i].ratios.Fault_Rate_per_mem_instr * weights[i + 1];
        rchar_per_cycle += history[i].ratios.RChar_per_Cycle * weights[i + 1];
        wchar_per_cycle += history[i].ratios.WChar_per_Cycle * weights[i + 1];
        rbytes_per_cycle += history[i].ratios.RBytes_per_Cycle * weights[i + 1];
        wbytes_per_cycle += history[i].ratios.WBytes_per_Cycle * weights[i + 1];
    }

    if (has_last_used) {
        ipc += last_used->ratios.IPC * weights[history_count + 1];
        cache_miss += last_used->ratios.Cache_Miss_Ratio * weights[history_count + 1];
        uop_per_cycle += last_used->ratios.Uop_per_Cycle * weights[history_count + 1];
        mem_stall_per_mem += last_used->ratios.MemStallCycle_per_Mem_Inst * weights[history_count + 1];
        mem_stall_per_inst += last_used->ratios.MemStallCycle_per_Inst * weights[history_count + 1];
        fault_rate += last_used->ratios.Fault_Rate_per_mem_instr * weights[history_count + 1];
        rchar_per_cycle += last_used->ratios.RChar_per_Cycle * weights[history_count + 1];
        wchar_per_cycle += last_used->ratios.WChar_per_Cycle * weights[history_count + 1];
        rbytes_per_cycle += last_used->ratios.RBytes_per_Cycle * weights[history_count + 1];
        wbytes_per_cycle += last_used->ratios.WBytes_per_Cycle * weights[history_count + 1];
    }

    data->ratios.IPC = isnan(ipc) || isinf(ipc) ? 0.0 : ipc / denominator;
    data->ratios.Cache_Miss_Ratio = isnan(cache_miss) || isinf(cache_miss) ? 0.0 : cache_miss / denominator;
    data->ratios.Uop_per_Cycle = isnan(uop_per_cycle) || isinf(uop_per_cycle) ? 0.0 : uop_per_cycle / denominator;
    data->ratios.MemStallCycle_per_Mem_Inst = isnan(mem_stall_per_mem) || isinf(mem_stall_per_mem) ? 0.0 : mem_stall_per_mem / denominator;
    data->ratios.MemStallCycle_per_Inst = isnan(mem_stall_per_inst) || isinf(mem_stall_per_inst) ? 0.0 : mem_stall_per_inst / denominator;
    data->ratios.Fault_Rate_per_mem_instr = isnan(fault_rate) || isinf(fault_rate) ? 0.0 : fault_rate / denominator;
    data->ratios.RChar_per_Cycle = isnan(rchar_per_cycle) || isinf(rchar_per_cycle) ? 0.0 : rchar_per_cycle / denominator;
    data->ratios.WChar_per_Cycle = isnan(wchar_per_cycle) || isinf(wchar_per_cycle) ? 0.0 : wchar_per_cycle / denominator;
    data->ratios.RBytes_per_Cycle = isnan(rbytes_per_cycle) || isinf(rbytes_per_cycle) ? 0.0 : rbytes_per_cycle / denominator;
    data->ratios.WBytes_per_Cycle = isnan(wbytes_per_cycle) || isinf(wbytes_per_cycle) ? 0.0 : wbytes_per_cycle / denominator;

    if (!is_process_alive(pid)) {
        memset(&data->ratios, 0, sizeof(data->ratios));
    }
}

static void process_queue(DynamicCoreMasks *masks) {
    SCHEDULER_PRINTF("Processing queue with %d entries\n", queue_size);
    
    int temp_compute_threads = 0, temp_io_threads = 0, temp_memory_threads = 0;
    int i = 0;

    while (i < queue_size) {
        pid_t pid = queue[i].pid;
        
        if (!is_process_alive(pid)) {
            SCHEDULER_PRINTF("Process PID %d died, removing from queue\n", pid);
            remove_queue_entry(i);
            continue;
        }

        MonitorData data = queue[i].current_data;
        int startup_flag = queue[i].startup_flag;

        if (queue[i].history_count > 0) {
            int latest_idx = queue[i].history_count - 1;
            data.pthread_count = queue[i].history[latest_idx].pthread_count;
            data.pcore_count = queue[i].history[latest_idx].pcore_count;
            data.ecore_count = queue[i].history[latest_idx].ecore_count;
        }

        if (!is_process_alive(pid)) {
            SCHEDULER_PRINTF("Process PID %d died before classification, removing\n", pid);
            remove_queue_entry(i);
            continue;
        }

        if (queue[i].history_count > 0 || queue[i].has_last_used) {
            compute_weighted_ratios(pid, &data, queue[i].history, 
                                   queue[i].history_count, &queue[i].last_used, 
                                   queue[i].has_last_used);
        }

        if (!is_process_alive(pid)) {
            SCHEDULER_PRINTF("Process PID %d died during computation, removing\n", pid);
            remove_queue_entry(i);
            continue;
        }

        struct timespec start_time, end_time;
        long class_time_cjson = 0;

        clock_gettime(CLOCK_MONOTONIC, &start_time);
        classify_workload_cjson(&data);
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        class_time_cjson = (end_time.tv_sec - start_time.tv_sec) * 1000000 +
                           (end_time.tv_nsec - start_time.tv_nsec) / 1000;

        if (!is_process_alive(pid)) {
            SCHEDULER_PRINTF("Process PID %d died before scheduling, removing\n", pid);
            remove_queue_entry(i);
            continue;
        }

        const char *predicted_class;
        if (startup_flag) {
            predicted_class = "Startup";
        } else if (data.compute_prob_cjson > data.io_prob_cjson && 
                   data.compute_prob_cjson > data.memory_prob_cjson) {
            temp_compute_threads += data.thread_count;
            predicted_class = "Compute";
        } else if (data.io_prob_cjson > data.compute_prob_cjson && 
                   data.io_prob_cjson > data.memory_prob_cjson) {
            temp_io_threads += data.thread_count;
            predicted_class = "I/O";
        } else {
            temp_memory_threads += data.thread_count;
            predicted_class = "Memory";
        }
        SCHEDULER_PRINTF("PID %d classified as %s (Compute: %.4f, I/O: %.4f, Memory: %.4f, Threads: %d)\n",
                         pid, predicted_class, data.compute_prob_cjson, data.io_prob_cjson, data.memory_prob_cjson, data.thread_count);

        // Check if class has changed or startup_flag is set
        int class_changed = !queue[i].has_last_used || 
                           strcmp(queue[i].predicted_class, predicted_class) != 0 ||
                           startup_flag;

        write_to_csv(&data, class_time_cjson, predicted_class);

        if (is_process_alive(pid)) {
            // Handle affinity
            if (startup_flag) {
                set_affinity_for_all_threads(pid, "0-31");
                SCHEDULER_PRINTF("PID %d (startup) affinity set to all cores: 0-31\n", pid);
            } else {
                int max_cores = MAX(data.thread_count, 1); // Ensure at least 1 core
                char capped_coreset[256] = "";
                int core_count = 0;
                int cores[64];
                const char *selected_coreset;

                // Select coreset based on workload type
                if (strcmp(predicted_class, "Compute") == 0) {
                    selected_coreset = masks->compute_coreset[0] ? masks->compute_coreset : COMPUTE_CORESET;
                } else if (strcmp(predicted_class, "I/O") == 0) {
                    selected_coreset = masks->io_coreset[0] ? masks->io_coreset : IO_CORESET;
                } else {
                    selected_coreset = masks->memory_coreset[0] ? masks->memory_coreset : MEMORY_CORESET;
                }

                parse_coreset(selected_coreset, cores, &core_count);
                if (core_count == 0) {
                    SCHEDULER_PERROR("Failed to generate capped coreset for PID %d: no valid cores in %s\n", pid, selected_coreset);
                    // Fallback to default coresets
                    if (strcmp(predicted_class, "Compute") == 0) {
                        selected_coreset = COMPUTE_CORESET;
                    } else if (strcmp(predicted_class, "I/O") == 0) {
                        selected_coreset = IO_CORESET;
                    } else {
                        selected_coreset = MEMORY_CORESET;
                    }
                    parse_coreset(selected_coreset, cores, &core_count);
                    if (core_count == 0) {
                        SCHEDULER_PERROR("Fallback coreset %s invalid for PID %d, using core 0\n", selected_coreset, pid);
                        strcpy(capped_coreset, "0");
                        core_count = 1;
                    } else {
                        cores_to_string(cores, MIN(core_count, max_cores), capped_coreset, sizeof(capped_coreset));
                    }
                } else {
                    cores_to_string(cores, MIN(core_count, max_cores), capped_coreset, sizeof(capped_coreset));
                }

                set_affinity_for_all_threads(pid, capped_coreset);
                SCHEDULER_PRINTF("PID %d capped to %d cores: %s\n", pid, core_count, capped_coreset);
                verify_affinity(pid);
            }

            // Commented out priority setting logic
            // if (class_changed) {
            //     // Process main thread
            //     int core, is_pcore;
            //     get_current_core(pid, &core, &is_pcore);
            //     if (core >= 0) {
            //         int priority;
            //         if (startup_flag) {
            //             priority = 20; // High priority for starting processes
            //             SCHEDULER_PRINTF("PID %d is starting, setting high priority %d for main thread on core %d\n", 
            //                              pid, priority, core);
            //         } else {
            //             if (strcmp(predicted_class, "Compute") == 0) {
            //                 priority = is_pcore ? 20 : 10; // High on P-cores, Low on E-cores
            //             } else if (strcmp(predicted_class, "I/O") == 0) {
            //                 priority = is_pcore ? 10 : 20; // Low on P-cores, High on E-cores
            //             } else {
            //                 priority = 15; // Medium on both
            //             }
            //             SCHEDULER_PRINTF("PID %d classified as %s, setting priority %d for main thread on core %d (%s)\n",
            //                              pid, predicted_class, priority, core, is_pcore ? "P-core" : "E-core");
            //         }
            //         set_thread_priority(pid, priority, predicted_class, core, is_pcore);
            //     } else {
            //         SCHEDULER_PRINTF("Skipping priority setting for main thread of PID %d due to core detection failure\n", pid);
            //     }

            //     // Process child threads
            //     char task_path[256];
            //     snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);
            //     DIR *dir = opendir(task_path);
            //     if (!dir) {
            //         SCHEDULER_PERROR("Failed to open task directory for PID %d: %s\n", pid, strerror(errno));
            //     } else {
            //         struct dirent *entry;
            //         while ((entry = readdir(dir)) != NULL) {
            //             if (entry->d_name[0] == '.') continue;
            //             pid_t tid = atoi(entry->d_name);
            //             if (tid > 0 && tid != pid && is_process_alive(tid)) {
            //                 int core, is_pcore;
            //                 get_current_core(tid, &core, &is_pcore);
            //                 if (core >= 0) {
            //                     int priority;
            //                     if (startup_flag) {
            //                         priority = 20; // High priority for starting processes
            //                         SCHEDULER_PRINTF("PID %d is starting, setting high priority %d for TID %d on core %d\n", 
            //                                          pid, priority, tid, core);
            //                     } else {
            //                         if (strcmp(predicted_class, "Compute") == 0) {
            //                             priority = is_pcore ? 20 : 10; // High on P-cores, Low on E-cores
            //                         } else if (strcmp(predicted_class, "I/O") == 0) {
            //                             priority = is_pcore ? 10 : 20; // Low on P-cores, High on E-cores
            //                         } else {
            //                             priority = 15; // Medium on both
            //                         }
            //                         SCHEDULER_PRINTF("PID %d classified as %s, setting priority %d for TID %d on core %d (%s)\n",
            //                                          pid, predicted_class, priority, tid, core, is_pcore ? "P-core" : "E-core");
            //                     }
            //                     set_thread_priority(tid, priority, predicted_class, core, is_pcore);
            //                 } else {
            //                     SCHEDULER_PRINTF("Skipping priority setting for TID %d of PID %d due to core detection failure\n", 
            //                                      tid, pid);
            //                 }
            //             }
            //         }
            //         closedir(dir);
            //     }
            // }
        }

        if (is_process_alive(pid)) {
            queue[i].startup_flag = 0; // Mark as processed after startup
            queue[i].last_used = data;
            queue[i].has_last_used = 1;
            queue[i].current_data = data;
            queue[i].history_count = 0; // Reset history after processing
            strncpy(queue[i].predicted_class, predicted_class, sizeof(queue[i].predicted_class) - 1);
            i++;
        } else {
            SCHEDULER_PRINTF("Process PID %d died after processing, removing\n", pid);
            remove_queue_entry(i);
        }
    }

    compute_threads = temp_compute_threads;
    io_threads = temp_io_threads;
    memory_threads = temp_memory_threads;
    SCHEDULER_PRINTF("Updated thread counts: Compute=%d, I/O=%d, Memory=%d\n",
                     compute_threads, io_threads, memory_threads);
}

void cleanup_scheduler(int server_fd) {
    SCHEDULER_PRINTF("Cleaning up scheduler\n");
    cleanup_classifier_cjson();

    if (server_fd >= 0) {
        close(server_fd);
    }
    unlink(SOCKET_PATH);
    for (int i = 0; i < MAX_QUEUE_SIZE; i++) {
        free_queue_entry(&queue[i]);
    }
    queue_size = 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        SCHEDULER_PERROR("Usage: %s <coreset>\n", argv[0]);
        return 1;
    }

    for (int i = 0; i < MAX_QUEUE_SIZE; i++) {
        init_queue_entry(&queue[i]);
    }

    set_affinity(getpid(), argv[1]);
    SCHEDULER_PRINTF("Scheduler bound to coreset %s\n", argv[1]);

    if (init_classifier_cjson(MODEL_PATH_CJSON) != 0) {
        SCHEDULER_PERROR("Failed to initialize CJSON classifier\n");
        return 1;
    }

    if (init_csv() || init_core_allocation_csv()) {
        SCHEDULER_PERROR("Failed to initialize CSV files\n");
        return 1;
    }

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        SCHEDULER_PERROR("socket: %s\n", strerror(errno));
        cleanup_scheduler(-1);
        return 1;
    }

    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCKET_PATH);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        SCHEDULER_PERROR("bind: %s\n", strerror(errno));
        cleanup_scheduler(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) == -1) {
        SCHEDULER_PERROR("listen: %s\n", strerror(errno));
        cleanup_scheduler(server_fd);
        return 1;
    }

    SCHEDULER_PRINTF("Running, listening on %s\n", SOCKET_PATH);

    while (1) {
        while (1) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                SCHEDULER_PERROR("Error accepting connection: %s\n", strerror(errno));
                continue;
            }

            pid_t pid;
            ssize_t bytes_read = read(client_fd, &pid, sizeof(pid));
            if (bytes_read != sizeof(pid)) {
                SCHEDULER_PERROR("Failed to read PID\n");
                close(client_fd);
                continue;
            }

            if (pid == -1) {
                SCHEDULER_PRINTF("Received shutdown request\n");
                close(client_fd);
                cleanup_scheduler(server_fd);
                return 0;
            }

            int startup_flag;
            MonitorData data;
            bytes_read = read(client_fd, &startup_flag, sizeof(int));
            bytes_read += read(client_fd, &data, sizeof(MonitorData));

            if (bytes_read != sizeof(int) + sizeof(MonitorData)) {
                SCHEDULER_PERROR("Incomplete data received for PID %d\n", pid);
                close(client_fd);
                continue;
            }

            add_to_queue(pid, data, startup_flag);
            close(client_fd);
        }

        DynamicCoreMasks masks;
        compute_dynamic_coresets(&masks);
        SCHEDULER_PRINTF("Computed coresets: Compute=%s, I/O=%s, Memory=%s\n",
                         masks.compute_coreset, masks.io_coreset, masks.memory_coreset);
        log_core_allocation(&masks);
        process_queue(&masks);

        usleep(SCHEDULER_SLEEP_MILLISECONDS * 1000);
    }

    cleanup_scheduler(server_fd);
    return 0;
}