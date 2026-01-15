PAPI_DIR = /home/thanos/src/papi-7.2.0
# point to PAPI 7.2 install lib (not the build lib)
PAPI_INSTALL_LIB = $(PAPI_DIR)/src/install/lib
PAPI_INCLUDE = $(PAPI_DIR)/include
# full path to the specific .so file we want linked (adjust if different)
PAPI_SO = $(PAPI_INSTALL_LIB)/libpapi.so.7.2.0.0

ONNX_DIR = /home/thanos/onnxruntime
ONNX_INCLUDE = $(ONNX_DIR)/include
ONNX_LIB = $(ONNX_DIR)/lib

CC = gcc
CFLAGS = -g -O2 -Wall -I$(PAPI_INCLUDE) -I$(ONNX_INCLUDE) -D_GNU_SOURCE -fPIC -DUSE_CJSON -DUSE_ONNX

# Add CORESET macro if provided via `make CORESET="0-15"`
ifdef CORESET
CFLAGS += -DCORESET=\"$(CORESET)\"
endif
ifdef QUIET_MONITOR
CFLAGS += -DQUIET_MONITOR
endif
ifdef QUIET_SCHEDULER
CFLAGS += -DQUIET_SCHEDULER
endif
ifdef QUIET_CLASSIFIER
CFLAGS += -DQUIET_CLASSIFIER
endif

# LDFLAGS: link the exact PAPI .so and embed rpath for both PAPI and ONNX
LDFLAGS = $(PAPI_SO) -L$(ONNX_LIB) -lonnxruntime -ldl -pthread -Wl,-rpath,$(PAPI_INSTALL_LIB):$(ONNX_LIB) -Wl,--enable-new-dtags

LIB_SRC = libmonitor.c
LIB = libmonitor.so

SCHEDULER_SRC = scheduler.c libclassifier.c cJSON.c libclassifier_2step.c libclassifier_onnx.c libclassifier_onnx_2step.c
SCHEDULER = scheduler

SHUTDOWN_SCHEDULER_SRC = shutdown_scheduler.c
SHUTDOWN_SCHEDULER = shutdown_scheduler

TEST_SRC = scheduler_quality_test1.c
TEST = scheduler_quality_test1

all: $(LIB) $(SCHEDULER) $(SHUTDOWN_SCHEDULER) $(TEST)

$(LIB): $(LIB_SRC)
	$(CC) -fPIC -shared -o $@ $^ $(CFLAGS) $(LDFLAGS)

$(SCHEDULER): $(SCHEDULER_SRC) libclassifier.h monitor.h
	$(CC) -o $@ $(SCHEDULER_SRC) $(CFLAGS) $(LDFLAGS)

$(SHUTDOWN_SCHEDULER): $(SHUTDOWN_SCHEDULER_SRC)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

$(TEST): $(TEST_SRC)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(LIB) $(SCHEDULER) $(SHUTDOWN_SCHEDULER) $(TEST)

.PHONY: all clean