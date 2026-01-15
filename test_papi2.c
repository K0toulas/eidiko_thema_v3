#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <papi.h>

#define MAX_EVENTS 64

static int try_add(int EventSet, const char *name) {
    int code = 0;
    if (PAPI_event_name_to_code((char*)name, &code) != PAPI_OK) return -1;
    if (PAPI_add_event(EventSet, code) != PAPI_OK) return -1;
    return 0;
}

static void busy_work(unsigned seconds) {
    unsigned long long x = 0;
    time_t start = time(NULL);
    while ((unsigned)(time(NULL) - start) < seconds) {
        for (int i = 0; i < 1000; ++i) x += i;
    }
    (void)x;
}

int main(void) {
    const char *candidates[] = {
        "adl_glc::UOPS_RETIRED.SLOTS",
        "adl_glc::UOPS_RETIRED.HEAVY",
        "adl_glc::UOPS_RETIRED.MS",
        "adl_glc::UOPS_RETIRED",
        "adl_grt::UOPS_RETIRED.ALL",
        "UOPS_RETIRED.ALL",
        "MEM_UOPS_RETIRED.ALL_LOADS",
        "MEM_UOPS_RETIRED.ALL_STORES",
        "UOPS_RETIRED.MS",
        "adl_grt::UOPS_RETIRED.SLOTS",
        "adl_grt::UOPS_RETIRED.HEAVY",
        "adl_grt::UOPS_RETIRED.CYCLES",
         "UOPS_RETIRED.SLOTS",
        "UOPS_RETIRED.HEAVY",
        "UOPS_RETIRED.MS",
        "UOPS_RETIRED.CYCLES",
        NULL
    };

    if (PAPI_library_init(PAPI_VER_CURRENT) != PAPI_VER_CURRENT) {
        fprintf(stderr, "PAPI init failed\n");
        return 1;
    }

    int EventSet = PAPI_NULL;
    if (PAPI_create_eventset(&EventSet) != PAPI_OK) {
        fprintf(stderr, "create_eventset failed\n");
        PAPI_shutdown();
        return 1;
    }

    const char *added_names[MAX_EVENTS];
    int added = 0;
    for (int i = 0; candidates[i] != NULL; ++i) {
        if (added >= MAX_EVENTS) break;
        if (try_add(EventSet, candidates[i]) == 0) {
            added_names[added++] = candidates[i];
            printf("Added: %s\n", candidates[i]);
        }
    }

    if (added == 0) {
        printf("No events added. Exiting.\n");
        PAPI_destroy_eventset(&EventSet);
        PAPI_shutdown();
        return 1;
    }

    long long values[MAX_EVENTS] = {0};
    if (PAPI_start(EventSet) != PAPI_OK) {
        fprintf(stderr, "PAPI_start failed\n");
        PAPI_cleanup_eventset(EventSet);
        PAPI_destroy_eventset(&EventSet);
        PAPI_shutdown();
        return 1;
    }

    busy_work(1); 

    if (PAPI_stop(EventSet, values) != PAPI_OK) {
        fprintf(stderr, "PAPI_stop failed\n");
    } else {
        for (int i = 0; i < added; ++i) {
            printf("%s : %lld\n", added_names[i], values[i]);
        }
    }

    PAPI_cleanup_eventset(EventSet);
    PAPI_destroy_eventset(&EventSet);
    PAPI_shutdown();
    return 0;
}