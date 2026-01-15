#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "libclassifier.h"
#include "libclassifier.h"

static const OrtApi *g_ort = NULL;
static OrtEnv *g_env = NULL;
static OrtSession *g_session = NULL;
static OrtSessionOptions *g_session_options = NULL;
static char *g_input_name = NULL;
static char *g_prob_name = NULL;
static OrtAllocator *g_allocator = NULL;

static const char *class_names[] = {"Compute", "I/O", "Memory"};

static void check_ort_status(OrtStatus *status, const char *msg) {
    if (status != NULL) {
        const char *err_msg = g_ort->GetErrorMessage(status);
        fprintf(stderr, "%s: %s\n", msg, err_msg);
        g_ort->ReleaseStatus(status);
        exit(1);
    }
}

int init_classifier_onnx(const char *model_path) {
    char filename[256];
    snprintf(filename, sizeof(filename), "%s.onnx", model_path);
    printf("Initializing ONNX classifier with model %s\n", filename);

    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!g_ort) {
        fprintf(stderr, "Failed to initialize ONNX Runtime API\n");
        return -1;
    }

    check_ort_status(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "workload_classifier_onnx", &g_env),
                     "Failed to create ONNX environment");

    check_ort_status(g_ort->CreateSessionOptions(&g_session_options),
                     "Failed to create session options");

    check_ort_status(g_ort->CreateSession(g_env, filename, g_session_options, &g_session),
                     "Failed to create ONNX session");

    OrtMemoryInfo *memory_info;
    check_ort_status(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info),
                     "Failed to create memory info");
    check_ort_status(g_ort->CreateAllocator(g_session, memory_info, &g_allocator),
                     "Failed to create allocator");
    g_ort->ReleaseMemoryInfo(memory_info);

    check_ort_status(g_ort->SessionGetInputName(g_session, 0, g_allocator, &g_input_name),
                     "Failed to get input name");
    check_ort_status(g_ort->SessionGetOutputName(g_session, 1, g_allocator, &g_prob_name),
                     "Failed to get probability output name");

    printf("ONNX classifier initialized successfully\n");
    return 0;
}

void classify_workload_onnx(MonitorData *data) {
    if (!g_session || !g_ort) {
        fprintf(stderr, "ONNX classifier not initialized\n");
        data->compute_prob_onnx = 0.0;
        data->io_prob_onnx = 0.0;
        data->memory_prob_onnx = 0.0;
        return;
    }

    printf("onnx classify_workload_onnx\n");

    float features[NUM_FEATURES] = {
        (float)data->pthread_count, (float)data->pcore_count, (float)data->ecore_count,
        (float)data->ratios.IPC, (float)data->ratios.Cache_Miss_Ratio, (float)data->ratios.Uop_per_Cycle,
        (float)data->ratios.MemStallCycle_per_Mem_Inst, (float)data->ratios.MemStallCycle_per_Inst,
        (float)data->ratios.Fault_Rate_per_mem_instr, (float)data->ratios.RChar_per_Cycle,
        (float)data->ratios.WChar_per_Cycle, (float)data->ratios.RBytes_per_Cycle,
        (float)data->ratios.WBytes_per_Cycle
    };

    for (int i = 0; i < NUM_FEATURES; i++) {
        printf("Feature %d: %.15f\n", i, features[i]);
    }

    OrtMemoryInfo *memory_info;
    check_ort_status(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info),
                     "Failed to create memory info");

    int64_t input_dims[] = {1, NUM_FEATURES};
    OrtValue *input_tensor;
    check_ort_status(g_ort->CreateTensorWithDataAsOrtValue(memory_info, features, sizeof(features),
                                                            input_dims, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                            &input_tensor),
                     "Failed to create input tensor");

    g_ort->ReleaseMemoryInfo(memory_info);

    const char *input_names[] = {g_input_name};
    const char *output_names[] = {g_prob_name};
    OrtValue *output_tensor = NULL;
    check_ort_status(g_ort->Run(g_session, NULL, input_names, (const OrtValue *const *)&input_tensor, 1,
                                 output_names, 1, &output_tensor),
                     "Failed to run inference");

    float *output_probs;
    check_ort_status(g_ort->GetTensorMutableData(output_tensor, (void **)&output_probs),
                     "Failed to get output tensor data");

    float prob_sum = output_probs[0] + output_probs[1] + output_probs[2];
    printf("Probability sum: %.15f\n", prob_sum);

    data->compute_prob_onnx = output_probs[0];
    data->io_prob_onnx = output_probs[1];
    data->memory_prob_onnx = output_probs[2];

    int pred_class = 0;
    float max_prob = output_probs[0];
    for (int c = 1; c < NUM_CLASSES; c++) {
        if (output_probs[c] > max_prob) {
            max_prob = output_probs[c];
            pred_class = c;
        }
    }

    printf("\n--- Workload Classification (ONNX) ---\n");
    printf("  Predicted Class: %s\n", class_names[pred_class]);
    for (int c = 0; c < NUM_CLASSES; c++) {
        printf("  Prob_%s: %.15f\n", class_names[c], output_probs[c]);
    }

    g_ort->ReleaseValue(output_tensor);
    g_ort->ReleaseValue(input_tensor);
}

void cleanup_classifier_onnx(void) {
    if (g_ort) {
        if (g_input_name) g_ort->AllocatorFree(g_allocator, g_input_name);
        if (g_prob_name) g_ort->AllocatorFree(g_allocator, g_prob_name);
        if (g_allocator) g_ort->ReleaseAllocator(g_allocator);
        if (g_session) g_ort->ReleaseSession(g_session);
        if (g_session_options) g_ort->ReleaseSessionOptions(g_session_options);
        if (g_env) g_ort->ReleaseEnv(g_env);
    }
    g_ort = NULL;
    g_env = NULL;
    g_session = NULL;
    g_session_options = NULL;
    g_allocator = NULL;
    g_input_name = NULL;
    g_prob_name = NULL;
    printf("ONNX classifier resources cleaned up\n");
}