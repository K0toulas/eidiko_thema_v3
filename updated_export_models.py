import pandas as pd
import numpy as np
from sklearn.ensemble import RandomForestClassifier
from sklearn.preprocessing import StandardScaler, LabelEncoder
from sklearn.metrics import classification_report
import treelite
import tl2cgen
import pickle
import os
import subprocess
import shutil
import glob

# Load data
path = 'data/'
df_train_un = pd.read_csv(path + 'train_dataset_new_eventset_initial_benchmarks.csv')
df_test_un = pd.read_csv(path + 'val_dataset_new_eventset_benchmarks.csv')

# Define features and target
features = [
    'P-Threads', 'P-Cores', 'E-Cores', 'INST_RETIRED:ANY_P', 'perf::PERF_COUNT_HW_CACHE_MISSES',
    'ix86arch::UNHALTED_CORE_CYCLES', 'MEM_INST_RETIRED:ANY', 'perf::FAULTS',
    'CYCLE_ACTIVITY:CYCLES_MEM_ANY', 'adl_grt::UOPS_RETIRED', 'rchar', 'wchar',
    'syscr', 'syscw', 'read_bytes', 'write_bytes'
]
target = ['Compute', 'I/O', 'Memory']

# Prepare data
X_train = df_train_un[features]
X_test = df_test_un[features]
y_train = df_train_un.apply(lambda row: 'Compute' if row['Compute'] == 1 else ('I/O' if row['I/O'] == 1 else 'Memory'), axis=1)
y_test = df_test_un.apply(lambda row: 'Compute' if row['Compute'] == 1 else ('I/O' if row['I/O'] == 1 else 'Memory'), axis=1)

# Label encoding
label_encoder = LabelEncoder()
label_encoder.fit(target)
y_train_encoded = label_encoder.transform(y_train)
y_test_encoded = label_encoder.transform(y_test)

# StandardScaler for Two-Step Random Forest
scaler = StandardScaler()
X_train_scaled = scaler.fit_transform(X_train)
X_test_scaled = scaler.transform(X_test)

# Save scaler parameters
scaler_params = {'mean': scaler.mean_, 'std': scaler.scale_}
with open('scaler_params.pkl', 'wb') as f:
    pickle.dump(scaler_params, f)

# Train Simple Random Forest
rf_simple = RandomForestClassifier(n_estimators=300, max_depth=15, random_state=42)
rf_simple.fit(X_train, y_train_encoded)

# Train Two-Step Random Forest
# Step 1: Compute vs Non-Compute
y_train_step1 = y_train.apply(lambda x: 1 if x == 'Compute' else 0)
y_test_step1 = y_test.apply(lambda x: 1 if x == 'Compute' else 0)
rf_step1 = RandomForestClassifier(n_estimators=500, max_depth=15, random_state=42, class_weight='balanced')
rf_step1.fit(X_train_scaled, y_train_step1)

# Step 2: I/O vs Memory
X_train_step2 = X_train_scaled[y_train != 'Compute']
y_train_step2 = y_train[y_train != 'Compute']
le_step2 = LabelEncoder()
y_train_step2_encoded = le_step2.fit_transform(y_train_step2)
rf_step2 = RandomForestClassifier(n_estimators=500, max_depth=15, random_state=42, class_weight='balanced')
rf_step2.fit(X_train_step2, y_train_step2_encoded)

# Export models to TL2cgen
def export_to_treelite(model, model_name, features, class_names):
    try:
        # Import scikit-learn model into Treelite
        model_treelite = treelite.sklearn.import_model(model)
        # Generate C code using TL2cgen
        tl2cgen.generate_c_code(model_treelite, dirpath=f'./{model_name}', params={'parallel_comp': os.cpu_count() or 1})
        # Use user-specific tl2cgen paths
        lib_path = os.path.expanduser('~/gantsios_venv/lib/python3.10/site-packages/tl2cgen/lib')  # REPLACE WITH YOUR libtl2cgen.so PATH
        include_path = os.path.expanduser('~/gantsios_venv/lib/python3.10/site-packages/tl2cgen/include')  # REPLACE WITH YOUR tl2cgen.h PATH
        # Compile the generated C code to a shared library
        compile_cmd = [
            'gcc', '-shared', '-o', f'{model_name}.so', *glob.glob(f'{model_name}/*.c'),
            f'-I{include_path}', f'-L{lib_path}', '-ltl2cgen', '-fPIC'
        ]
        result = subprocess.run(compile_cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"Compilation failed for {model_name}.so:\n{result.stderr}\n{result.stdout}")
        if not os.path.exists(f'{model_name}.so'):
            raise FileNotFoundError(f"Shared library {model_name}.so was not created")
        # Verify the library
        with tl2cgen.Predictor(f'{model_name}.so', n_jobs=os.cpu_count() or 1) as predictor:
            pass  # Auto-closes
        # Clean up
        shutil.rmtree(f'./{model_name}', ignore_errors=True)
        return model_treelite
    except Exception as e:
        print(f"Error compiling {model_name} for TL2cgen: {e}")
        raise

# Export models
export_to_treelite(rf_simple, 'rf_simple', features, target)
export_to_treelite(rf_step1, 'rf_step1', features, ['Not Compute', 'Compute'])
export_to_treelite(rf_step2, 'rf_step2', features, ['I/O', 'Memory'])

# Save label encoders
with open('label_encoder.pkl', 'wb') as f:
    pickle.dump(label_encoder, f)
with open('label_encoder_step2.pkl', 'wb') as f:
    pickle.dump(le_step2, f)

# C interface for inference
c_code = """
#include <tl2cgen/predictor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Scaler parameters (from scaler_params.pkl)
double scaler_mean[] = {""" + ', '.join(f'{x:.6f}' for x in scaler_params['mean']) + """};
double scaler_std[] = {""" + ', '.join(f'{x:.6f}' for x in scaler_params['std']) + """};

// Function to scale input features
void scale_features(double* input, double* output, int n_features) {
    for (int i = 0; i < n_features; i++) {
        output[i] = (input[i] - scaler_mean[i]) / scaler_std[i];
        if (isnan(output[i]) || isinf(output[i])) {
            output[i] = 0.0; // Handle NaN or Inf
        }
    }
}

// Function to run inference on Simple Random Forest
void predict_rf_simple(double* features, double* probabilities, int n_features) {
    TL2cgenPredictor* predictor;
    TL2cgenErrorCode status = TL2cgenPredictorLoad("./rf_simple.so", &predictor);
    if (status != TL2CGEN_NO_ERROR) {
        printf("Error loading rf_simple.so: %s\\n", TL2cgenGetLastError());
        return;
    }
    
    float input[n_features];
    for (int i = 0; i < n_features; i++) {
        input[i] = (float)features[i];
    }
    
    float output[3]; // 3 classes: Compute, I/O, Memory
    size_t out_size;
    TL2cgenPredictorPredict(predictor, input, 0, output, &out_size);
    
    for (int i = 0; i < 3; i++) {
        probabilities[i] = (double)output[i];
    }
    
    TL2cgenPredictorFree(predictor);
}

// Function to run inference on Two-Step Random Forest
void predict_rf_twostep(double* features, double* probabilities, int n_features) {
    TL2cgenPredictor* predictor_step1;
    TL2cgenErrorCode status = TL2cgenPredictorLoad("./rf_step1.so", &predictor_step1);
    if (status != TL2CGEN_NO_ERROR) {
        printf("Error loading rf_step1.so: %s\\n", TL2cgenGetLastError());
        return;
    }
    
    // Scale features
    double scaled_features[n_features];
    scale_features(features, scaled_features, n_features);
    float input[n_features];
    for (int i = 0; i < n_features; i++) {
        input[i] = (float)scaled_features[i];
    }
    
    // Step 1: Compute vs Non-Compute
    float output_step1[2];
    size_t out_size;
    TL2cgenPredictorPredict(predictor_step1, input, 0, output_step1, &out_size);
    
    // Initialize probabilities
    probabilities[0] = 0.0; // Compute
    probabilities[1] = 0.0; // I/O
    probabilities[2] = 0.0; // Memory
    
    if (output_step1[1] > output_step1[0]) {
        // Predicted as Compute
        probabilities[0] = (double)output_step1[1];
    } else {
        // Step 2: I/O vs Memory
        TL2cgenPredictor* predictor_step2;
        status = TL2cgenPredictorLoad("./rf_step2.so", &predictor_step2);
        if (status != TL2CGEN_NO_ERROR) {
            printf("Error loading rf_step2.so: %s\\n", TL2cgenGetLastError());
            TL2cgenPredictorFree(predictor_step1);
            return;
        }
        float output_step2[2];
        TL2cgenPredictorPredict(predictor_step2, input, 0, output_step2, &out_size);
        probabilities[1] = (double)output_step2[0]; // I/O
        probabilities[2] = (double)output_step2[1]; // Memory
        TL2cgenPredictorFree(predictor_step2);
    }
    
    TL2cgenPredictorFree(predictor_step1);
}

// Example usage
int main() {
    // Example feature vector from monitoring (replace with actual data)
    double features[] = {""" + ', '.join(['0.0'] * len(features)) + """};
    double probabilities[3];
    
    printf("Simple Random Forest Probabilities:\\n");
    predict_rf_simple(features, probabilities, """ + str(len(features)) + """);
    printf("Compute: %.3f, I/O: %.3f, Memory: %.3f\\n", probabilities[0], probabilities[1], probabilities[2]);
    
    printf("Two-Step Random Forest Probabilities:\\n");
    predict_rf_twostep(features, probabilities, """ + str(len(features)) + """);
    printf("Compute: %.3f, I/O: %.3f, Memory: %.3f\\n", probabilities[0], probabilities[1], probabilities[2]);
    
    return 0;
}
"""
with open('inference.c', 'w') as f:
    f.write(c_code)

# Test models on test data
# Simple Random Forest
y_pred_simple = rf_simple.predict(X_test)
y_proba_simple = rf_simple.predict_proba(X_test)
print("Simple Random Forest Classification Report:")
print(classification_report(y_test_encoded, y_pred_simple, target_names=target))

# Two-Step Random Forest
y_pred_step1 = rf_step1.predict(X_test_scaled)
y_pred_final = pd.Series(['unknown'] * len(y_test), index=y_test.index)
y_proba_final = np.zeros((len(y_test), 3))
y_pred_final[y_pred_step1 == 1] = 'Compute'
y_proba_final[y_pred_step1 == 1, 0] = rf_step1.predict_proba(X_test_scaled)[y_pred_step1 == 1, 1]
if np.any(y_pred_step1 == 0):
    X_test_step2 = X_test_scaled[y_pred_step1 == 0]
    test_indices_step2 = X_test.index[y_pred_step1 == 0]
    y_pred_step2_encoded = rf_step2.predict(X_test_step2)
    y_pred_step2 = le_step2.inverse_transform(y_pred_step2_encoded)
    y_proba_step2 = rf_step2.predict_proba(X_test_step2)
    y_pred_final.loc[test_indices_step2] = y_pred_step2
    for idx, test_idx in enumerate(test_indices_step2):
        y_proba_final[test_idx, 1] = y_proba_step2[idx, 0]  # I/O
        y_proba_final[test_idx, 2] = y_proba_step2[idx, 1]  # Memory
print("Two-Step Random Forest Classification Report:")
print(classification_report(y_test, y_pred_final, target_names=target))

# Save predictions
pd.DataFrame({
    'true_class': y_test,
    'predicted_class': label_encoder.inverse_transform(y_pred_simple),
    'prob_Compute': y_proba_simple[:, 0],
    'prob_IO': y_proba_simple[:, 1],
    'prob_Memory': y_proba_simple[:, 2]
}).to_csv('rf_simple_predictions.csv', index=False)
pd.DataFrame({
    'true_class': y_test,
    'predicted_class': y_pred_final,
    'prob_Compute': y_proba_final[:, 0],
    'prob_IO': y_proba_final[:, 1],
    'prob_Memory': y_proba_final[:, 2]
}).to_csv('rf_twostep_predictions.csv', index=False)