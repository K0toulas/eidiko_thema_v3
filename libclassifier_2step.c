#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include <errno.h>
#include "libclassifier.h"

static cJSON *g_model_step1 = NULL;
static cJSON *g_model_step2 = NULL;
static int g_n_classes_step1 = 0;
static int g_n_classes_step2 = 0;
static int g_n_features = 0;
static char **g_feature_names = NULL;
static const char *class_names[] = {"Compute", "I/O", "Memory"};
static const char *positive_class = "Compute";
static const char *other_classes[] = {"I/O", "Memory"};

static float evaluate_tree(cJSON *tree, float *features, int n_features) {
    cJSON *nodes = cJSON_GetObjectItem(tree, "nodes");
    int root = cJSON_GetObjectItem(tree, "root")->valueint;
    int current = root;
    
    while (1) {
        cJSON *node = cJSON_GetArrayItem(nodes, current);
        if (strcmp(cJSON_GetObjectItem(node, "type")->valuestring, "leaf") == 0) {
            cJSON *value = cJSON_GetObjectItem(node, "value");
            float max_prob = 0.0;
            int max_class = 0;
            for (int i = 0; i < cJSON_GetArraySize(value); i++) {
                float prob = (float)cJSON_GetArrayItem(value, i)->valuedouble;
                if (prob > max_prob) {
                    max_prob = prob;
                    max_class = i;
                }
            }
            return (float)max_class;
        }
        
        cJSON *feature = cJSON_GetObjectItem(node, "feature");
        float threshold = (float)cJSON_GetObjectItem(node, "threshold")->valuedouble;
        int feature_idx = -1;
        for (int i = 0; i < n_features; i++) {
            if (strcmp(g_feature_names[i], feature->valuestring) == 0) {
                feature_idx = i;
                break;
            }
        }
        
        int next = features[feature_idx] <= threshold ? 
                   cJSON_GetObjectItem(node, "left")->valueint :
                   cJSON_GetObjectItem(node, "right")->valueint;
        current = next;
    }
}

int init_classifier_cjson_2step(const char *model_path) {
    printf("Initializing CJSON two-step classifier\n");
    
    // Construct paths for Step 1 and Step 2
    char model_path_step1[256], model_path_step2[256];
    snprintf(model_path_step1, sizeof(model_path_step1), "%s_compute_step1.json", model_path);
    snprintf(model_path_step2, sizeof(model_path_step2), "%s_compute_step2.json", model_path);
    
    // Load Step 1 model
    FILE *file = fopen(model_path_step1, "r");
    if (!file) {
        fprintf(stderr, "Failed to open model file %s: %s\n", model_path_step1, strerror(errno));
        return -1;
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = malloc(length + 1);
    fread(data, 1, length, file);
    data[length] = '\0';
    fclose(file);
    
    g_model_step1 = cJSON_Parse(data);
    free(data);
    if (!g_model_step1) {
        fprintf(stderr, "Failed to parse JSON: %s\n", cJSON_GetErrorPtr());
        return -1;
    }
    
    // Load Step 2 model
    file = fopen(model_path_step2, "r");
    if (!file) {
        fprintf(stderr, "Failed to open model file %s: %s\n", model_path_step2, strerror(errno));
        cJSON_Delete(g_model_step1);
        g_model_step1 = NULL;
        return -1;
    }
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc(length + 1);
    fread(data, 1, length, file);
    data[length] = '\0';
    fclose(file);
    
    g_model_step2 = cJSON_Parse(data);
    free(data);
    if (!g_model_step2) {
        fprintf(stderr, "Failed to parse JSON: %s\n", cJSON_GetErrorPtr());
        cJSON_Delete(g_model_step1);
        g_model_step1 = NULL;
        return -1;
    }
    
    g_n_classes_step1 = cJSON_GetObjectItem(g_model_step1, "n_classes")->valueint;
    g_n_classes_step2 = cJSON_GetObjectItem(g_model_step2, "n_classes")->valueint;
    g_n_features = cJSON_GetObjectItem(g_model_step1, "n_features")->valueint;
    
    cJSON *feature_names = cJSON_GetObjectItem(g_model_step1, "feature_names");
    g_feature_names = malloc(g_n_features * sizeof(char *));
    for (int i = 0; i < g_n_features; i++) {
        g_feature_names[i] = strdup(cJSON_GetArrayItem(feature_names, i)->valuestring);
    }
    
    printf("CJSON two-step classifier initialized successfully with %d trees\n", 
           cJSON_GetObjectItem(g_model_step1, "n_estimators")->valueint);
    return 0;
}

void classify_workload_cjson_2step(MonitorData *data) {
    if (!g_model_step1 || !g_model_step2) {
        fprintf(stderr, "CJSON classifier not initialized\n");
        data->compute_prob_cjson_2step = 0.0;
        data->io_prob_cjson_2step = 0.0;
        data->memory_prob_cjson_2step = 0.0;
        return;
    }
    
    printf("cjson classify_workload_cjson_2step\n");
    
    // Prepare features (13 features)
    float features[13] = {
        (float)data->thread_count, (float)data->pcore_count, (float)data->ecore_count,
        (float)data->ratios.IPC, (float)data->ratios.Cache_Miss_Ratio, (float)data->ratios.Uop_per_Cycle,
        (float)data->ratios.MemStallCycle_per_Mem_Inst, (float)data->ratios.MemStallCycle_per_Inst,
        (float)data->ratios.Fault_Rate_per_mem_instr, (float)data->ratios.RChar_per_Cycle,
        (float)data->ratios.WChar_per_Cycle, (float)data->ratios.RBytes_per_Cycle,
        (float)data->ratios.WBytes_per_Cycle
    };
    
    // Step 1: Predict positive class (Compute)
    cJSON *trees = cJSON_GetObjectItem(g_model_step1, "trees");
    float votes[2] = {0.0, 0.0};
    for (int i = 0; i < cJSON_GetArraySize(trees); i++) {
        float pred = evaluate_tree(cJSON_GetArrayItem(trees, i), features, g_n_features);
        votes[(int)pred]++;
    }
    float prob_positive = votes[1] / (votes[0] + votes[1]);
    
    float probs[3] = {0.0, 0.0, 0.0};
    
if (prob_positive > 0.5) {
        probs[0] = prob_positive; // Compute
        float remaining_prob = 1.0 - prob_positive; // Remaining probability
        probs[1] = remaining_prob / 2.0; // I/O
        probs[2] = remaining_prob / 2.0; // Memory
        data->compute_prob_cjson_2step = probs[0];
        data->io_prob_cjson_2step = probs[1];
        data->memory_prob_cjson_2step = probs[2];
        printf("\n--- Workload Classification (CJSON Two-Step) ---\n");
        printf("  Predicted Class: %s\n", class_names[0]);
        printf("  Prob_Compute: %.15f\n", probs[0]);
        printf("  Prob_I/O: %.15f\n", probs[1]);
        printf("  Prob_Memory: %.15f\n", probs[2]);
        return;
    }
    
    // Step 2: Predict non-positive classes (I/O vs. Memory)
    trees = cJSON_GetObjectItem(g_model_step2, "trees");
    votes[0] = votes[1] = 0.0;
    for (int i = 0; i < cJSON_GetArraySize(trees); i++) {
        float pred = evaluate_tree(cJSON_GetArrayItem(trees, i), features, g_n_features);
        votes[(int)pred]++;
    }
    probs[1] = votes[0] / (votes[0] + votes[1]); // I/O
    probs[2] = votes[1] / (votes[0] + votes[1]); // Memory
    
    data->compute_prob_cjson_2step = probs[0];
    data->io_prob_cjson_2step = probs[1];
    data->memory_prob_cjson_2step = probs[2];
    
    int pred_class = probs[1] > probs[2] ? 1 : 2;
    printf("\n--- Workload Classification (CJSON Two-Step) ---\n");
    printf("  Predicted Class: %s\n", class_names[pred_class]);
    printf("  Prob_Compute: %.15f\n", probs[0]);
    printf("  Prob_I/O: %.15f\n", probs[1]);
    printf("  Prob_Memory: %.15f\n", probs[2]);
}

void cleanup_classifier_cjson_2step(void) {
    if (g_model_step1) {
        cJSON_Delete(g_model_step1);
        g_model_step1 = NULL;
    }
    if (g_model_step2) {
        cJSON_Delete(g_model_step2);
        g_model_step2 = NULL;
    }
    if (g_feature_names) {
        for (int i = 0; i < g_n_features; i++) {
            free(g_feature_names[i]);
        }
        free(g_feature_names);
        g_feature_names = NULL;
    }
    g_n_classes_step1 = 0;
    g_n_classes_step2 = 0;
    g_n_features = 0;
    printf("CJSON two-step classifier resources cleaned up\n");
}