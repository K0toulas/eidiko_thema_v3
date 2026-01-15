#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Define matrix size - 2048 results in 32MB per matrix (3 matrices = 96MB)
// This is usually large enough to exceed L3 cache on most CPUs.
#define GLOBAL_MATRIX_N 2048
#define TEST_FILE "test_data.bin"

void create_dummy_file(const char* filename) {
    size_t total_elements = (size_t)GLOBAL_MATRIX_N * GLOBAL_MATRIX_N;
    double *data = (double*)malloc(total_elements * sizeof(double));
    if (!data) return;

    for (size_t i = 0; i < total_elements; i++) {
        data[i] = (double)rand() / RAND_MAX;
    }

    FILE *fp = fopen(filename, "wb");
    if (fp) {
        fwrite(data, sizeof(double), total_elements, fp);
        fclose(fp);
        printf("Created dummy file: %s\n", filename);
    }
    free(data);
}

void run_combined_test(const char* filename) {
    size_t total_elements = (size_t)GLOBAL_MATRIX_N * GLOBAL_MATRIX_N;
    
    // Allocate memory
    double *A = (double*)malloc(total_elements * sizeof(double));
    double *B = (double*)malloc(total_elements * sizeof(double));
    double *C = (double*)calloc(total_elements, sizeof(double));

    if (!A || !B || !C) {
        printf("Memory allocation failed!\n");
        if (A) free(A);
        if (B) free(B);
        if (C) free(C);
        return;
    }

    // --- STAGE 1: I/O Bound ---
    // High sleep-time (waiting for disk), low HScore
    printf("Stage 1: I/O Bound - Reading file...\n");
    FILE *fp = fopen(filename, "rb");
    if (fp) {
        fread(A, sizeof(double), total_elements, fp);
        fclose(fp);
    } else {
        printf("Could not open file for reading!\n");
    }

    // --- STAGE 2: Memory Bound ---
    // Strided access causes high Cache/TLB misses. 
    // In HCS, this increases the 'Bias' value, lowering the HScore.
    printf("Stage 2: Memory Bound - Transposing...\n");
    for (int i = 0; i < GLOBAL_MATRIX_N; i++) {
        for (int j = 0; j < GLOBAL_MATRIX_N; j++) {
            B[j * GLOBAL_MATRIX_N + i] = A[i * GLOBAL_MATRIX_N + j];
        }
    }

    // --- STAGE 3: Compute Bound ---
    // Optimized loop order for cache friendliness. 
    // Low Bias, high Interactivity = Highest HScore (Promotes to Big Core).
    printf("Stage 3: Compute Bound - Multiplying...\n");
    for (int i = 0; i < GLOBAL_MATRIX_N; i++) {
        for (int k = 0; k < GLOBAL_MATRIX_N; k++) {
            double temp = A[i * GLOBAL_MATRIX_N + k];
            for (int j = 0; j < GLOBAL_MATRIX_N; j++) {
                C[i * GLOBAL_MATRIX_N + j] += temp * B[k * GLOBAL_MATRIX_N + j];
            }
        }
    }

    printf("Test complete. Sample result: %f\n", C[0]);

    free(A); free(B); free(C);
}

int main(int argc, char* argv[]) {
    srand(time(NULL));

    // Create file if it doesn't exist
    create_dummy_file(TEST_FILE);

    printf("Starting HCS Validation Test (Matrix Size: %d x %d)\n", GLOBAL_MATRIX_N, GLOBAL_MATRIX_N);
    run_combined_test(TEST_FILE);

    // Clean up
    remove(TEST_FILE);
    return 0;
}