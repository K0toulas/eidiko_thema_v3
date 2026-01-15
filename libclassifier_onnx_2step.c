#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "libclassifier.h"

static const OrtApi *g_ort = NULL;
static OrtEnv *g_env = NULL;
static OrtSession *g_session_step1 = NULL;
static OrtSession *g_session_step2 = NULL;
static OrtSessionOptions *g_session_options = NULL;
static char *g_input_name_step1 = NULL;
static char *g_prob_name_step1 = NULL;
static char *g_input_name_step2 = NULL;
static char *g_prob_name_step2 = NULL;
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

int init_classifier_onnx_2step(const char *model_path) {
    char filename_step1[256];
    char filename_step2[256];
    snprintf(filename_step1, sizeof(filename_step1), "%s_compute_step1.onnx", model_path);
    snprintf(filename_step2, sizeof(filename_step2), "%s_compute_step2.onnx", model_path);
    
    printf("Initializing ONNX two-step classifier with models:\n");
    printf("  Step 1: %s\n", filename_step1);
    printf("  Step 2: %s\n", filename_step2);

    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!g_ort) {
        fprintf(stderr, "Failed to initialize ONNX Runtime API\n");
        return -1;
    }

    check_ort_status(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "workload_classifier_onnx", &g_env),
                     "Failed to create ONNX environment");

    check_ort_status(g_ort->CreateSessionOptions(&g_session_options),
                     "Failed to create session options");

#ifdef USE_OPENVINO
    OrtStatus *status = g_ort->SessionOptionsAppendExecutionProvider_OpenVINO(g_session_options, NULL);
    if (status) {
        fprintf(stderr, "Failed to enable OpenVINO: %s\n", g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
    } else {
        printf("Enabled OpenVINO execution provider\n");
    }
#endif

    // Initialize Step 1 model (Compute vs Non-Compute)
    check_ort_status(g_ort->CreateSession(g_env, filename_step1, g_session_options, &g_session_step1),
                     "Failed to create ONNX session for Step 1");

    // Initialize Step 2 model (I/O vs Memory)
    check_ort_status(g_ort->CreateSession(g_env, filename_step2, g_session_options, &g_session_step2),
                     "Failed to create ONNX session for Step 2");

    OrtMemoryInfo *memory_info;
    check_ort_status(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info),
                     "Failed to create memory info");
    check_ort_status(g_ort->CreateAllocator(g_session_step1, memory_info, &g_allocator),
                     "Failed to create allocator");
    g_ort->ReleaseMemoryInfo(memory_info);

    // Get input/output names for Step 1
    check_ort_status(g_ort->SessionGetInputName(g_session_step1, 0, g_allocator, &g_input_name_step1),
                     "Failed to get input name for Step 1");
    check_ort_status(g_ort->SessionGetOutputName(g_session_step1, 1, g_allocator, &g_prob_name_step1),
                     "Failed to get probability output name for Step 1");

    // Get input/output names for Step 2
    check_ort_status(g_ort->SessionGetInputName(g_session_step2, 0, g_allocator, &g_input_name_step2),
                     "Failed to get input name for Step 2");
    check_ort_status(g_ort->SessionGetOutputName(g_session_step2, 1, g_allocator, &g_prob_name_step2),
                     "Failed to get probability output name for Step 2");

    printf("ONNX two-step classifier initialized successfully\n");
    return 0;
}

void classify_workload_onnx_2step(MonitorData *data) {
    if (!g_session_step1 || !g_session_step2 || !g_ort) {
        fprintf(stderr, "ONNX 2 step classifier not initialized\n");
        data->compute_prob_onnx_2step = 0.0;
        data->io_prob_onnx_2step = 0.0;
        data->memory_prob_onnx_2step = 0.0;
        return;
    }

    float features[NUM_FEATURES] = {
        (float)data->pthread_count, (float)data->pcore_count, (float)data->ecore_count,
        (float)data->ratios.IPC, (float)data->ratios.Cache_Miss_Ratio, (float)data->ratios.Uop_per_Cycle,
        (float)data->ratios.MemStallCycle_per_Mem_Inst, (float)data->ratios.MemStallCycle_per_Inst,
        (float)data->ratios.Fault_Rate_per_mem_instr, (float)data->ratios.RChar_per_Cycle,
        (float)data->ratios.WChar_per_Cycle, (float)data->ratios.RBytes_per_Cycle,
        (float)data->ratios.WBytes_per_Cycle
    };

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

    // Step 1: Compute vs Non-Compute classification
    const char *input_names_step1[] = {g_input_name_step1};
    const char *output_names_step1[] = {g_prob_name_step1};
    OrtValue *output_tensor_step1 = NULL;
    check_ort_status(g_ort->Run(g_session_step1, NULL, input_names_step1, 
                               (const OrtValue *const *)&input_tensor, 1,
                               output_names_step1, 1, &output_tensor_step1),
                     "Failed to run inference for Step 1");

    float *output_probs_step1;
    check_ort_status(g_ort->GetTensorMutableData(output_tensor_step1, (void **)&output_probs_step1),
                     "Failed to get output tensor data for Step 1");

    float prob_compute = output_probs_step1[1];  // Probability of being Compute
    float probs[3] = {0.0, 0.0, 0.0};

    if (prob_compute > 0.5) {
        // If Compute probability > 50%, assign remaining probability equally to I/O and Memory
        probs[0] = prob_compute;
        probs[1] = (1.0 - prob_compute) / 2.0;
        probs[2] = (1.0 - prob_compute) / 2.0;
    } else {
        // Step 2: I/O vs Memory classification
        const char *input_names_step2[] = {g_input_name_step2};
        const char *output_names_step2[] = {g_prob_name_step2};
        OrtValue *output_tensor_step2 = NULL;
        
        check_ort_status(g_ort->Run(g_session_step2, NULL, input_names_step2,
                                   (const OrtValue *const *)&input_tensor, 1,
                                   output_names_step2, 1, &output_tensor_step2),
                         "Failed to run inference for Step 2");

        float *output_probs_step2;
        check_ort_status(g_ort->GetTensorMutableData(output_tensor_step2, (void **)&output_probs_step2),
                         "Failed to get output tensor data for Step 2");

        probs[0] = 0.0;  // Compute probability is 0
        probs[1] = output_probs_step2[0];  // I/O probability
        probs[2] = output_probs_step2[1];  // Memory probability

        g_ort->ReleaseValue(output_tensor_step2);
    }

    // Normalize probabilities to sum to 1
    float prob_sum = probs[0] + probs[1] + probs[2];
    if (prob_sum > 0) {
        probs[0] /= prob_sum;
        probs[1] /= prob_sum;
        probs[2] /= prob_sum;
    }

    // Store results
    data->compute_prob_onnx_2step = probs[0];
    data->io_prob_onnx_2step = probs[1];
    data->memory_prob_onnx_2step = probs[2];

    // Determine predicted class
    int pred_class = 0;
    if (probs[1] > probs[0] && probs[1] > probs[2]) {
        pred_class = 1;
    } else if (probs[2] > probs[0] && probs[2] > probs[1]) {
        pred_class = 2;
    }

    printf("\n--- Workload Classification (ONNX Two-Step) ---\n");
    printf("  Predicted Class: %s\n", class_names[pred_class]);
    printf("  Prob_Compute: %.4f\n", probs[0]);
    printf("  Prob_I/O: %.4f\n", probs[1]);
    printf("  Prob_Memory: %.4f\n", probs[2]);

    // Clean up
    g_ort->ReleaseValue(output_tensor_step1);
    g_ort->ReleaseValue(input_tensor);
}

void cleanup_classifier_onnx_2step(void) {
    if (g_ort) {
        if (g_input_name_step1) g_ort->AllocatorFree(g_allocator, g_input_name_step1);
        if (g_prob_name_step1) g_ort->AllocatorFree(g_allocator, g_prob_name_step1);
        if (g_input_name_step2) g_ort->AllocatorFree(g_allocator, g_input_name_step2);
        if (g_prob_name_step2) g_ort->AllocatorFree(g_allocator, g_prob_name_step2);
        if (g_allocator) g_ort->ReleaseAllocator(g_allocator);
        if (g_session_step1) g_ort->ReleaseSession(g_session_step1);
        if (g_session_step2) g_ort->ReleaseSession(g_session_step2);
        if (g_session_options) g_ort->ReleaseSessionOptions(g_session_options);
        if (g_env) g_ort->ReleaseEnv(g_env);
    }
    g_ort = NULL;
    g_env = NULL;
    g_session_step1 = NULL;
    g_session_step2 = NULL;
    g_session_options = NULL;
    g_allocator = NULL;
    g_input_name_step1 = NULL;
    g_prob_name_step1 = NULL;
    g_input_name_step2 = NULL;
    g_prob_name_step2 = NULL;
    printf("ONNX two-step classifier resources cleaned up\n");
}