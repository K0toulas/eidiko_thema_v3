#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <papi.h>

#define MAX_EVENTS 10

int add_event_by_name(int EventSet, const char *event_name) {
    int event_code;
    int ret = PAPI_event_name_to_code((char *)event_name, &event_code);
    if (ret != PAPI_OK) {
        fprintf(stderr, "Not found: %s\n", event_name);
        return -1;
    }
    ret = PAPI_add_event(EventSet, event_code);
    if (ret != PAPI_OK) {
        fprintf(stderr, "Add failed: %s\n", event_name);
        return -1;
    }
    return 0;
}

int main() {
    int ret, EventSet = PAPI_NULL;
    long long values[MAX_EVENTS];
    int event_count = 0;

    ret = PAPI_library_init(PAPI_VER_CURRENT);
    if (ret != PAPI_VER_CURRENT) {
        fprintf(stderr, "PAPI init error\n");
        return 1;
    }

    ret = PAPI_create_eventset(&EventSet);
    if (ret != PAPI_OK) {
        fprintf(stderr, "create_eventset error\n");
        return 1;
    }

    const char *events_to_add[] = {
        "INST_RETIRED:ANY_P",
        "perf::PERF_COUNT_HW_CACHE_MISSES",
        "ix86arch::UNHALTED_CORE_CYCLES",
        "MEM_INST_RETIRED:ANY",
        "perf::FAULTS",
        "CYCLE_ACTIVITY:CYCLES_MEM_ANY",
        "adl_grt::UOPS_RETIRED.ALL",
        "adl_grt::UOPS_RETIRED",
        NULL
    };
    int num_events = sizeof(events_to_add) / sizeof(events_to_add[0]);

    int added_flags[MAX_EVENTS] = {0};

    for (int i = 0; i < num_events && events_to_add[i] != NULL; i++) {
        // an uop_retired failarei sinexise
        if (strcmp(events_to_add[i], "adl_grt::UOPS_RETIRED.ALL") == 0) {
            if (add_event_by_name(EventSet, events_to_add[i]) != 0) {
                printf("Skipped: %s\n", events_to_add[i]);
                continue;
            }
        } else {
            if (add_event_by_name(EventSet, events_to_add[i]) != 0) {
                fprintf(stderr, "Error adding %s\n", events_to_add[i]);
                PAPI_cleanup_eventset(EventSet);
                PAPI_destroy_eventset(&EventSet);
                return 1;
            }
        }
        added_flags[i] = 1;
        event_count++;
        printf("Added: %s\n", events_to_add[i]);
    }

    if (event_count == 0) {
        printf("No events added. Exiting.\n");
        PAPI_destroy_eventset(&EventSet);
        PAPI_shutdown();
        return 0;
    }

    ret = PAPI_start(EventSet);
    if (ret != PAPI_OK) {
        fprintf(stderr, "PAPI_start error\n");
        return 1;
    }

    printf("Running workload for 1 second...\n");
    sleep(1);

    ret = PAPI_stop(EventSet, values);
    if (ret != PAPI_OK) {
        fprintf(stderr, "PAPI_stop error\n");
        return 1;
    }

    printf("Results:\n");
    int j = 0;
    for (int i = 0; i < num_events && events_to_add[i] != NULL; ++i) {
        if (!added_flags[i]) continue;
        printf("%s : %lld\n", events_to_add[i], values[j]);
        ++j;
    }

    PAPI_cleanup_eventset(EventSet);
    PAPI_destroy_eventset(&EventSet);
    PAPI_shutdown();
    return 0;
}