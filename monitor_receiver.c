#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <time.h>
#include "monitor.h" 

#define SOCKET_PATH "/tmp/scheduler_socket"
#define OUTFILE_PREFIX "monitor_data"

static void timestamp_now(char *buf, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03ld",
             t.tm_year+1900, t.tm_mon+1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec/1000000);
}

int main(int argc, char **argv) {
    const char *outdir = "."; 
    if (argc > 1) outdir = argv[1];
    char socket_path[] = SOCKET_PATH;
    unlink(socket_path); 

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket failed to initialize"); exit(1); }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(s); exit(1); }
    if (listen(s, 8) < 0) { perror("listen"); close(s); exit(1); }
    printf("[receiver] listening on %s, writing CSV to %s\n", socket_path, outdir);

    char csvpath[4096];
    snprintf(csvpath, sizeof(csvpath), "%s/%s.csv", outdir, OUTFILE_PREFIX);
    FILE *csv = fopen(csvpath, "a");
    if (!csv) { perror("fopen"); close(s); exit(1); }

    //write header
    fseek(csv, 0, SEEK_END);
    long sz = ftell(csv);
    if (sz == 0) {
        fprintf(csv,
            "timestamp,pid,startup_flag,thread_count,hw_thread_count,pthread_count,pcore_count,ecore_count,total_cores,exec_time_ms,"
            "INST_RETIRED,CACHE_MISSES,UNHALTED_CORE_CYCLES,MEM_INST_RETIRED,FAULTS,CYCLE_ACTIVITY_CYCLES_MEM_ANY,UOPS_RETIRED,"
            "rchar,wchar,syscr,syscw,read_bytes,write_bytes,IPC,Cache_Miss_Ratio,Uop_per_Cycle,MemStallCycle_per_Mem_Inst,MemStallCycle_per_Inst,Fault_Rate_per_mem_instr,RChar_per_Cycle,WChar_per_Cycle,RBytes_per_Cycle,WBytes_per_Cycle\n");
        fflush(csv);
    }

    // accept loop
    for (;;) {
        int cfd = accept(s, NULL, NULL);
        if (cfd < 0) { perror("accept"); break; }
        // read pid
        pid_t pid;
        ssize_t r = read(cfd, &pid, sizeof(pid));
        if (r != sizeof(pid)) { close(cfd); continue; }
        int startup_flag;
        r = read(cfd, &startup_flag, sizeof(startup_flag));
        if (r != sizeof(startup_flag)) { close(cfd); continue; }
        MonitorData data;
        ssize_t expected = sizeof(MonitorData);
        ssize_t got = 0;
        char *buf = (char*)&data;
        while (got < expected) {
            ssize_t n = read(cfd, buf + got, expected - got);
            if (n <= 0) break;
            got += n;
        }
        if (got != expected) { // partial read
            close(cfd);
            continue;
        }
        char ts[64];
        timestamp_now(ts, sizeof(ts));

        //  CSV row
        fprintf(csv, "%s,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,",
            ts,
            (int)pid,
            startup_flag,
            data.thread_count,
            data.hw_thread_count,
            data.pthread_count,
            data.pcore_count,
            data.ecore_count,
            data.total_cores,
            data.exec_time_ms
        );
        //   CSV events
        for (int i = 0; i < NUM_EVENTS; ++i) {
            fprintf(csv, "%lld", (long long)data.total_values[i]);
            if (i < NUM_EVENTS - 1) fprintf(csv, ",");
            else fprintf(csv, ",");
        }
        // IO delta
        fprintf(csv, "%llu,%llu,%llu,%llu,%llu,%llu,",
            (unsigned long long)data.io_delta.rchar,
            (unsigned long long)data.io_delta.wchar,
            (unsigned long long)data.io_delta.syscr,
            (unsigned long long)data.io_delta.syscw,
            (unsigned long long)data.io_delta.read_bytes,
            (unsigned long long)data.io_delta.write_bytes
        );
        // ratios
        fprintf(csv, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f\n",
            data.ratios.IPC,
            data.ratios.Cache_Miss_Ratio,
            data.ratios.Uop_per_Cycle,
            data.ratios.MemStallCycle_per_Mem_Inst,
            data.ratios.MemStallCycle_per_Inst,
            data.ratios.Fault_Rate_per_mem_instr,
            data.ratios.RChar_per_Cycle,
            data.ratios.WChar_per_Cycle,
            data.ratios.RBytes_per_Cycle,
            data.ratios.WBytes_per_Cycle
        );
        fflush(csv);
        close(cfd);
    }

    fclose(csv);
    close(s);
    unlink(socket_path);
    return 0;
    ///vale gia to tourbostat
}
