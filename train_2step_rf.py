import pandas as pd
import numpy as np
from sklearn.ensemble import RandomForestClassifier
from sklearn.preprocessing import LabelEncoder
from sklearn.metrics import classification_report
from sklearn.tree import _tree
import json
import joblib
import onnx
from skl2onnx import convert_sklearn
from skl2onnx.common.data_types import FloatTensorType
import onnxruntime as rt
import pickle

def create_ratios(df):
    df = df.copy()
    df['IPC'] = df['INST_RETIRED:ANY_P'] / df['ix86arch::UNHALTED_CORE_CYCLES']
    df['Cache_Miss_Ratio'] = df['perf::PERF_COUNT_HW_CACHE_MISSES'] / df['MEM_INST_RETIRED:ANY']
    df['Uop_per_Cycle'] = df['adl_grt::UOPS_RETIRED'] / df['ix86arch::UNHALTED_CORE_CYCLES']
    df['MemStallCycle_per_Mem_Inst'] = df['CYCLE_ACTIVITY:CYCLES_MEM_ANY'] / df['MEM_INST_RETIRED:ANY']
    df['MemStallCycle_per_Inst'] = df['CYCLE_ACTIVITY:CYCLES_MEM_ANY'] / df['INST_RETIRED:ANY_P']
    df['Fault_Rate_per_mem_instr'] = df['perf::FAULTS'] / df['MEM_INST_RETIRED:ANY']
    df['RChar_per_Cycle'] = df['rchar'] / df['ix86arch::UNHALTED_CORE_CYCLES']
    df['WChar_per_Cycle'] = df['wchar'] / df['ix86arch::UNHALTED_CORE_CYCLES']
    df['RBytes_per_Cycle'] = df['read_bytes'] / df['ix86arch::UNHALTED_CORE_CYCLES']
    df['WBytes_per_Cycle'] = df['write_bytes'] / df['ix86arch::UNHALTED_CORE_CYCLES']
    df.replace([np.inf, -np.inf], np.nan, inplace=True)
    df.fillna(0, inplace=True)
    return df

def tree_to_code(tree, feature_names):
    tree_ = tree.tree_
    feature_name = [
        feature_names[i] if i != _tree.TREE_UNDEFINED else "undefined!"
        for i in tree_.feature
    ]
    
    nodes = []
    def recurse(node, depth):
        if tree_.feature[node] != _tree.TREE_UNDEFINED:
            name = feature_name[node]
            threshold = tree_.threshold[node]
            nodes.append({
                'type': 'node',
                'feature': name,
                'threshold': threshold,
                'left': recurse(tree_.children_left[node], depth + 1),
                'right': recurse(tree_.children_right[node], depth + 1)
            })
        else:
            value = tree_.value[node][0]
            total = np.sum(value)
            nodes.append({
                'type': 'leaf',
                'value': (value / total).tolist()
            })
        return len(nodes) - 1
    
    root = recurse(0, 1)
    return {'nodes': nodes, 'root': root}

def export_model(rf_model, feature_names, filename):
    model_data = {
        'n_estimators': len(rf_model.estimators_),
        'n_classes': rf_model.n_classes_,
        'n_features': rf_model.n_features_in_,
        'feature_names': feature_names,
        'trees': []
    }
    
    for tree in rf_model.estimators_:
        model_data['trees'].append(tree_to_code(tree, feature_names))
    
    with open(filename, 'w') as f:
        json.dump(model_data, f, indent=2)

def train_two_step_rf(X_train, X_test, y_train, y_test, positive_class, features, label_encoder):
    # Step 1: Binary classification (Compute vs. Non-Compute)
    y_train_step1 = y_train.apply(lambda x: 1 if x == positive_class else 0)
    y_test_step1 = y_test.apply(lambda x: 1 if x == positive_class else 0)
    
    rf_step1 = RandomForestClassifier(n_estimators=300, max_depth=15, random_state=42, class_weight='balanced')
    rf_step1.fit(X_train, y_train_step1)
    
    y_pred_step1 = rf_step1.predict(X_test)
    print(f"Step 1 ({positive_class} vs. Non-{positive_class}) Classification Report:")
    print(classification_report(y_test_step1, y_pred_step1, target_names=[f'Non-{positive_class}', positive_class]))
    
    # Step 2: Binary classification (I/O vs. Memory)
    other_classes = ['I/O', 'Memory']
    X_train_step2 = X_train[y_train != positive_class]
    y_train_step2 = y_train[y_train != positive_class]
    X_test_step2 = X_test[y_pred_step1 == 0]
    
    y_pred_final = pd.Series([positive_class if pred == 1 else 'unknown' for pred in y_pred_step1], index=y_test.index)
    
    rf_step2 = None
    le_step2 = None
    if not X_train_step2.empty and not X_test_step2.empty:
        le_step2 = LabelEncoder()
        y_train_step2_encoded = le_step2.fit_transform(y_train_step2)
        
        rf_step2 = RandomForestClassifier(n_estimators=300, max_depth=15, random_state=42, class_weight='balanced')
        rf_step2.fit(X_train_step2, y_train_step2_encoded)
        
        y_pred_step2_encoded = rf_step2.predict(X_test_step2)
        y_pred_step2 = le_step2.inverse_transform(y_pred_step2_encoded)
        
        print(f"Step 2 ({other_classes[0]} vs. {other_classes[1]}) Classification Report:")
        print(classification_report(y_test[X_test_step2.index], y_pred_step2, target_names=other_classes, labels=other_classes))
        
        y_pred_final.loc[X_test_step2.index] = y_pred_step2
    else:
        y_pred_step2 = np.array([])
        print("No samples for Step 2 classification.")
    
    print("Final Combined Classification Report:")
    print(classification_report(y_test, y_pred_final, target_names=['Compute', 'I/O', 'Memory']))
    
    return rf_step1, rf_step2, le_step2, y_test_step1

if __name__ == "__main__":
    path = 'data/'
    df_train_un = pd.read_csv(path + 'train_dataset_new_eventset_initial_benchmarks.csv')
    df_test_un = pd.read_csv(path + 'val_real_app_sim.csv')
    
    performance_events = [
        'INST_RETIRED:ANY_P', 'perf::PERF_COUNT_HW_CACHE_MISSES', 'ix86arch::UNHALTED_CORE_CYCLES',
        'MEM_INST_RETIRED:ANY', 'perf::FAULTS', 'CYCLE_ACTIVITY:CYCLES_MEM_ANY', 'adl_grt::UOPS_RETIRED'
    ]
    df_train_ratios = create_ratios(df_train_un)
    df_test_ratios = create_ratios(df_test_un)
    df_train_ratios = df_train_ratios.drop(performance_events, axis=1)
    df_test_ratios = df_test_ratios.drop(performance_events, axis=1)
    
    features = [
        'P-Threads', 'P-Cores', 'E-Cores', 'IPC', 'Cache_Miss_Ratio', 'Uop_per_Cycle',
        'MemStallCycle_per_Mem_Inst', 'MemStallCycle_per_Inst', 'Fault_Rate_per_mem_instr',
        'RChar_per_Cycle', 'WChar_per_Cycle', 'RBytes_per_Cycle', 'WBytes_per_Cycle'
    ]
    
    target = ['Compute', 'I/O', 'Memory']
    
    X_train = df_train_ratios[features]
    X_test = df_test_ratios[features]
    y_train = df_train_ratios.apply(lambda row: 'Compute' if row['Compute'] == 1 else ('I/O' if row['I/O'] == 1 else 'Memory'), axis=1)
    y_test = df_test_ratios.apply(lambda row: 'Compute' if row['Compute'] == 1 else ('I/O' if row['I/O'] == 1 else 'Memory'), axis=1)
    
    label_encoder = LabelEncoder()
    label_encoder.fit(target)
    with open('/srv/homes/ggantsios/eidiko/dynamic-librari-interposition-for-monintoring/label_encoder.pkl', 'wb') as f:
        pickle.dump(label_encoder, f)
    
    positive_class = 'Compute'
    name = 'compute'
    print(f"\nTraining two-step RF for {positive_class} vs. Non-{positive_class}")
    
    # Train for both ONNX and JSON
    rf_step1, rf_step2, le_step2, y_test_step1 = train_two_step_rf(X_train, X_test, y_train, y_test, positive_class, features, label_encoder)
    
    # Export Step 1 to ONNX
    initial_type = [('input', FloatTensorType([None, len(features)]))]
    options = {RandomForestClassifier: {'zipmap': False}}
    onnx_step1 = convert_sklearn(rf_step1, initial_types=initial_type, options=options, target_opset=15)
    onnx_step1.ir_version = 9
    onnx_path_step1 = f"/srv/homes/ggantsios/eidiko/dynamic-librari-interposition-for-monintoring/workload_classifier_onnx_{name}_step1.onnx"
    with open(onnx_path_step1, "wb") as f:
        f.write(onnx_step1.SerializeToString())
    
    # Export Step 2 to ONNX
    if rf_step2:
        onnx_step2 = convert_sklearn(rf_step2, initial_types=initial_type, options=options, target_opset=15)
        onnx_step2.ir_version = 9
        onnx_path_step2 = f"/srv/homes/ggantsios/eidiko/dynamic-librari-interposition-for-monintoring/workload_classifier_onnx_{name}_step2.onnx"
        with open(onnx_path_step2, "wb") as f:
            f.write(onnx_step2.SerializeToString())
    
    # Export Step 1 to JSON
    export_model(rf_step1, features, f"/srv/homes/ggantsios/eidiko/dynamic-librari-interposition-for-monintoring/workload_classifier_cjson_{name}_step1.json")
    if rf_step2:
        export_model(rf_step2, features, f"/srv/homes/ggantsios/eidiko/dynamic-librari-interposition-for-monintoring/workload_classifier_cjson_{name}_step2.json")
    
    # Validate ONNX models
    sess_options = rt.SessionOptions()
    sess_options.intra_op_num_threads = 32
    sess_step1 = rt.InferenceSession(onnx_path_step1, providers=["CPUExecutionProvider"])
    input_name = sess_step1.get_inputs()[0].name
    prob_name = sess_step1.get_outputs()[1].name
    input_data = X_test.to_numpy().astype(np.float32)
    pred_step1_probs = sess_step1.run([prob_name], {input_name: input_data})[0]
    y_pred_step1_onnx = (pred_step1_probs[:, 1] > 0.5).astype(int)
    
    print(f"ONNX Step 1 ({positive_class} vs. Non-{positive_class}) Classification Report:")
    print(classification_report(y_test_step1, y_pred_step1_onnx, target_names=[f'Non-{positive_class}', positive_class]))
    
    y_pred_final_onnx = pd.Series([positive_class if pred == 1 else 'unknown' for pred in y_pred_step1_onnx], index=y_test.index)
    
    if rf_step2 and len(X_test[y_pred_step1_onnx == 0]) > 0:
        sess_step2 = rt.InferenceSession(onnx_path_step2, providers=["CPUExecutionProvider"])
        input_data_step2 = X_test[y_pred_step1_onnx == 0].to_numpy().astype(np.float32)
        pred_step2_probs = sess_step2.run([prob_name], {input_name: input_data_step2})[0]
        y_pred_step2_onnx = le_step2.inverse_transform((pred_step2_probs[:, 1] > 0.5).astype(int))
        other_classes = ['I/O', 'Memory']
        print(f"ONNX Step 2 ({other_classes[0]} vs. {other_classes[1]}) Classification Report:")
        print(classification_report(y_test[X_test.index[y_pred_step1_onnx == 0]], y_pred_step2_onnx, target_names=other_classes, labels=other_classes))
        
        y_pred_final_onnx.loc[X_test.index[y_pred_step1_onnx == 0]] = y_pred_step2_onnx
    
    print("ONNX Final Combined Classification Report:")
    print(classification_report(y_test, y_pred_final_onnx, target_names=['Compute', 'I/O', 'Memory']))
    
    joblib.dump(rf_step1, f"/srv/homes/ggantsios/eidiko/dynamic-librari-interposition-for-monintoring/rf_{name}_step1.pkl")
    if rf_step2:
        joblib.dump(rf_step2, f"/srv/homes/ggantsios/eidiko/dynamic-librari-interposition-for-monintoring/rf_{name}_step2.pkl")