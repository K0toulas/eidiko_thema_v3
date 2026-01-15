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
import time
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
    y_train_encoded = label_encoder.transform(y_train)
    y_test_encoded = label_encoder.transform(y_test)

    # Save label encoder
    with open('/srv/homes/ggantsios/eidiko/dynamic-librari-interposition-for-monintoring/label_encoder.pkl', 'wb') as f:
        pickle.dump(label_encoder, f)

    rf_simple = RandomForestClassifier(n_estimators=300, max_depth=15, random_state=42)
    rf_simple.fit(X_train, y_train_encoded)

    y_pred_simple = rf_simple.predict(X_test)
    print("Scikit-learn Random Forest Classification Report:")
    print(classification_report(y_test_encoded, y_pred_simple, target_names=target))

    pd.DataFrame({
        'true_class': y_test,
        'predicted_class': label_encoder.inverse_transform(y_pred_simple),
        'prob_Compute': rf_simple.predict_proba(X_test)[:, 0],
        'prob_IO': rf_simple.predict_proba(X_test)[:, 1],
        'prob_Memory': rf_simple.predict_proba(X_test)[:, 2]
    }).to_csv('rf_simple_predictions.csv', index=False)

    export_model(rf_simple, features, '/srv/homes/ggantsios/eidiko/dynamic-librari-interposition-for-monintoring/workload_classifier_cjson.json')
    joblib.dump(rf_simple, "/srv/homes/ggantsios/eidiko/dynamic-librari-interposition-for-monintoring/rf_simple.pkl")
    print("Model successfully exported to workload_classifier_cjson.json")

    # Export to ONNX
    initial_type = [('input', FloatTensorType([None, len(features)]))]
    options = {RandomForestClassifier: {'zipmap': False}}
    onx = convert_sklearn(rf_simple, initial_types=initial_type, options=options, target_opset=15)
    onx.ir_version = 9

    onnx_path = "/srv/homes/ggantsios/eidiko/dynamic-librari-interposition-for-monintoring/workload_classifier_onnx.onnx"
    with open(onnx_path, "wb") as f:
        f.write(onx.SerializeToString())

    loaded_model = onnx.load(onnx_path)
    print(f"Model IR version: {loaded_model.ir_version}")
    print(f"Input shape: {loaded_model.graph.input[0]}")
    print(f"Output shapes: {[out for out in loaded_model.graph.output]}")
    onnx.checker.check_model(loaded_model)

    sess_options = rt.SessionOptions()
    sess_options.intra_op_num_threads = 32
    sess_options.inter_op_num_threads = 32
    sess = rt.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    print("Available providers:", rt.get_available_providers())
    print("Session providers:", sess.get_providers())

    input_name = sess.get_inputs()[0].name
    label_name = sess.get_outputs()[0].name  # Label output
    prob_name = sess.get_outputs()[1].name   # Probability output
    print("ONNX input name:", input_name)
    print("ONNX label output name:", label_name)
    print("ONNX probability output name:", prob_name)

    input_data = X_test.to_numpy().astype(np.float32)
    input_feed = {input_name: input_data}

    start_onnx = time.time()
    pred_onx_probs, pred_onx_labels = sess.run([prob_name, label_name], input_feed)
    end_onnx = time.time()
    onnx_duration = end_onnx - start_onnx
    print(f"ONNX Runtime predict time: {onnx_duration:.6f} seconds")

    start_sklearn = time.time()
    sk_probas = rf_simple.predict_proba(X_test)
    end_sklearn = time.time()
    sklearn_duration = end_sklearn - start_sklearn
    print(f"Sklearn predict time: {sklearn_duration:.6f} seconds")

    print("Are ONNX and sklearn probabilities close? ", np.allclose(pred_onx_probs, sk_probas, atol=1e-5))
    print("ONNX Random Forest Classification Report:")
    print(classification_report(y_test_encoded, pred_onx_labels, target_names=target))
    pred_sk_labels = np.argmax(sk_probas, axis=1)
    print("Are ONNX and sklearn predictions equal? ", np.array_equal(pred_onx_labels, pred_sk_labels))