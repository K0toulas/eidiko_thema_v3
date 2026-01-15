#!/bin/bash

# Script to run tests for each core set, recompiling scheduler for each coreset
# Assumes Redis is already running on another terminal
# Appends metrics, probabilities, classification times, and Expected_Class to train_dataset_dli.csv

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="$SCRIPT_DIR/results"
CONFIG_FILE="$SCRIPT_DIR/current_config.txt"
mkdir -p "$OUTPUT_DIR"
cd "$SCRIPT_DIR" || exit 1

# Core sets
coresets=(
  "0" "16"
  "0,1" "0,2" "0,16" "16,17" 
  "0,1,2,3" "0,2,4,6" "0,1,16,17" "0,2,16,17"
  "16,17,18,19" "0,1,2,3,4,5,6,7" "0,2,4,6,8,10,12,14" "16,17,18,19,20,21,22,23"
  "0,1,2,3,16,17,18,19" "0,2,4,6,16,17,18,19" 
  "0-15" "16-31"
  "0,1,2,3,4,5,6,7,16,17,18,19,20,21,22,23"
  "0,2,4,6,8,10,12,14,16,17,18,19,20,21,22,23" "0-31"
)

# Core set mapping to P-Threads,P-Cores,E-Cores
declare -A coreset_mapping=(
  ["0"]="1,1,0"
  ["16"]="0,0,1"
  ["0,1"]="2,1,0"
  ["0,2"]="2,2,0"
  ["0,16"]="1,1,1"
  ["16,17"]="0,0,2"
  ["0,1,2,3"]="4,2,0"
  ["0,2,4,6"]="4,4,0"
  ["0,1,16,17"]="2,1,2"
  ["0,2,16,17"]="2,2,2"
  ["16,17,18,19"]="0,0,4"
  ["0,1,2,3,4,5,6,7"]="8,4,0"
  ["0,2,4,6,8,10,12,14"]="8,8,0"
  ["16,17,18,19,20,21,22,23"]="0,0,8"
  ["0,1,2,3,16,17,18,19"]="4,2,4"
  ["0,2,4,6,16,17,18,19"]="4,4,4"
  ["0-15"]="16,8,0"
  ["16-31"]="0,0,16"
  ["0,1,2,3,4,5,6,7,16,17,18,19,20,21,22,23"]="8,4,8"
  ["0,2,4,6,8,10,12,14,16,17,18,19,20,21,22,23"]="8,8,8"
  ["0-31"]="16,8,16"
)

# Executables with arguments and expected class
EXECUTABLES=(
  "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b $HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/BMW27.blend -f 1 -- --cycles-device CPU --cycles-samples 256 --cycles-denoising=OPENIMAGEDENOISE::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench_lite.sql::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench.sql::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench_heavy.sql::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench_read_lite.sql::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench_read.sql::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench_read_heavy.sql::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench_blob_lite.sql::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench_blob.sql::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench_blob_heavy.sql::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench_transactional_lite.sql::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench_transactional.sql::io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < $HOME/eidiko/benchmarks/sqlite_tests/sqlite_bench_transactional_heavy.sql::io:0,1,0"
  "/usr/bin/gpg --batch --yes --passphrase testpass -c $HOME/eidiko/benchmarks/gnupg/testfile::compute:1,0,0"
  "openssl speed -elapsed -bytes 16384 sha256::compute:1,0,0"
  "openssl speed -elapsed -bytes 16384 aes-128-cbc::compute:1,0,0"
  "openssl speed -seconds 5 aes-128-cbc::compute:1,0,0"
  "openssl speed -seconds 5 sha256::compute:1,0,0"
  "openssl speed -elapsed -seconds 3 aes-128-cbc::compute:1,0,0"
  "openssl speed -async_jobs 32 -seconds 3 aes-128-cbc::compute:1,0,0"
  "openssl speed -async_jobs 32 -seconds 3 sha256::compute:1,0,0"
  "pigz -p 32 -k -f dummyfile1::io:0,1,0"
  "pigz -p 32 -k -f -9 dummyfile1::io:0,1,0"
  "pigz -p 32 -k -f -1 dummyfile1::io:0,1,0"
  "pigz -p 32 -k -f -H dummyfile1::io:0,1,0"
  "pigz -p 32 -k -f -i dummyfile1::io:0,1,0"
  "$HOME/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 6382 -c 4 -t 32 --data-size=65536 --key-pattern=R:R::io:0,1,0"
  "$HOME/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 6382 -c 8 -t 32 --data-size=32678 --ratio=10:1 --pipeline=10 --key-pattern=S:S::io:0,1,0"
  "$HOME/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 6382 -c 4 -t 32 --data-size=131072 --ratio=1:10 --key-pattern=G:G::io:0,1,0"
  "$HOME/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 6382 -c 16 -t 32 --data-size=32678 --ratio=1:1 --pipeline=5 --key-pattern=R:R::io:0,1,0"
  "$HOME/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 6382 -c 4 -t 32 --data-size=4096 --ratio=5:1 --rate-limiting=1000 --key-pattern=S:S::io:0,1,0"
  "$HOME/eidiko/benchmarks/clomp/clomp_mpi -1 -1 32 640000 32 1 100::memory:0,0,1"
  "$HOME/eidiko/benchmarks/clomp/clomp_mpi -1 -1 32 1280000 32 1 100::memory:0,0,1"
  "$HOME/eidiko/benchmarks/clomp/clomp_mpi -1 -1 32 320000 64 1 50::memory:0,0,1"
  "$HOME/eidiko/benchmarks/clomp/clomp_mpi -1 -1 32 1280000 32 1 50::memory:0,0,1"
  "$HOME/eidiko/benchmarks/FluidX3D/bin/FluidX3D 0::compute:1,0,0"
)

# Function to calculate OMP_NUM_THREADS (P-Threads + E-Cores)
calc_omp_threads() {
  local coreset="$1"
  IFS=',' read -r p_threads p_cores e_cores <<< "${coreset_mapping[$coreset]}"
  echo $((p_threads + e_cores))
}

# Function to get Predicted_Class for a classifier based on probabilities
get_predicted_class() {
  local compute_prob=$1
  local io_prob=$2
  local memory_prob=$3
  predicted=$(awk -v c="$compute_prob" -v i="$io_prob" -v m="$memory_prob" '
      BEGIN {
          max = c; class = "compute";
          if (i > max) { max = i; class = "io" }
          if (m > max) { class = "memory" }
          print class
      }')
  echo "$predicted"
}

# Initialize CSV file
echo "P-Threads,P-Cores,E-Cores,INST_RETIRED:ANY_P,PERF_COUNT_HW_CACHE_MISSES,UNHALTED_CORE_CYCLES,MEM_INST_RETIRED:ANY,FAULTS,CYCLES_MEM_ANY,UOPS_RETIRED,IPC,Cache_Miss_Ratio,Uop_per_Cycle,MemStallCycle_per_Mem_Inst,MemStallCycle_per_Inst,Fault_Rate_per_mem_instr,RChar_per_Cycle,WChar_per_Cycle,RBytes_per_Cycle,WBytes_per_Cycle,syscr,syscw,Execution Time (ms),rchar,wchar,read_bytes,write_bytes,Compute_Prob_CJSON,IO_Prob_CJSON,Memory_Prob_CJSON,Class_Time_CJSON (us),Compute_Prob_CJSON_2STEP,IO_Prob_CJSON_2STEP,Memory_Prob_CJSON_2STEP,Class_Time_CJSON_2STEP (us),Compute_Prob_ONNX,IO_Prob_ONNX,Memory_Prob_ONNX,Class_Time_ONNX (us),Compute_Prob_ONNX_2STEP,IO_Prob_ONNX_2STEP,Memory_Prob_ONNX_2STEP,Class_Time_ONNX_2STEP (us),Expected_Class" > train_dataset_dli.csv
chmod u+rw train_dataset_dli.csv

for coreset in "${coresets[@]}"; do
  echo "Compiling: CORESET=$coreset"
  make clean && make CORESET="$coreset" >> "$OUTPUT_DIR/build_log.txt" 2>&1
  if [ $? -ne 0 ]; then
    echo "Error: Compilation failed for CORESET=$coreset" >> "$OUTPUT_DIR/build_log.txt"
    continue
  fi
  # Check if libmonitor.so exists
  if [ ! -f "$SCRIPT_DIR/libmonitor.so" ]; then
    echo "Error: libmonitor.so not found after compilation for CORESET=$coreset" >> "$OUTPUT_DIR/build_log.txt"
    continue
  fi

  # Signal run_scheduler.sh to launch the new binary
  echo "CORESET=$coreset" > "$CONFIG_FILE"

  # Wait for scheduler to start (verify PID exists)
  while ! pgrep -f "scheduler 15" > /dev/null; do
    sleep 1
  done

  # Run benchmarks
  for exec_info in "${EXECUTABLES[@]}"; do
    # Split on LAST occurrence of ::
    exec_cmd="${exec_info%::*}"
    class_flags="${exec_info##*::}"

    # Now split class_flags on regular :
    IFS=':' read -r class flags <<< "$class_flags"

    echo "Running: $exec_cmd (Expected Class: $class)" >> "$OUTPUT_DIR/exec_log.txt" 2>&1

    # Run with environment in a subshell
    (
      export LD_PRELOAD="$SCRIPT_DIR/libmonitor.so"
      export OMP_NUM_THREADS=$(calc_omp_threads "$coreset")
      export OMP_PROC_BIND="spread"
      export OMP_PLACES="cores"

      # Execute directly without timeout/mpirun
      $exec_cmd >> "$OUTPUT_DIR/exec_log.txt" 2>&1
    )

    # Process results
    last_line=$(tail -n 1 train_dataset_dli_test.csv)
    IFS=',' read -r -a metrics <<< "$last_line"
    # Append all metrics with comma separator and Expected_Class
    ( IFS=','; echo "${metrics[*]},$class" ) >> train_dataset_dli.csv

    # Log predicted classes for debugging
    cjson_pred=$(get_predicted_class "${metrics[27]}" "${metrics[28]}" "${metrics[29]}")
    cjson_2step_pred=$(get_predicted_class "${metrics[31]}" "${metrics[32]}" "${metrics[33]}")
    onnx_pred=$(get_predicted_class "${metrics[35]}" "${metrics[36]}" "${metrics[37]}")
    onnx_2step_pred=$(get_predicted_class "${metrics[39]}" "${metrics[40]}" "${metrics[41]}")
    echo "DEBUG: CJSON=$cjson_pred, CJSON_2STEP=$cjson_2step_pred, ONNX=$onnx_pred, ONNX_2STEP=$onnx_2step_pred, Expected=$class" >> "$OUTPUT_DIR/classification_debug.log"

    sleep 2  # Short pause between runs
  done

  # Graceful shutdown
  echo "Requesting scheduler shutdown..." >> "$OUTPUT_DIR/exec_log.txt"
  ./shutdown_scheduler
  > "$CONFIG_FILE"  # Clear config to signal shutdown completion
  sleep 1  # Allow scheduler to exit
done

echo "Results saved to $OUTPUT_DIR/train_dataset_dli.csv"