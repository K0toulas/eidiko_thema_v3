#!/bin/bash

# Script to run workloads with different classifiers and core configurations, collecting accuracy and timing metrics

# Directory setup
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="$SCRIPT_DIR/results"
mkdir -p "$OUTPUT_DIR"
cd "$SCRIPT_DIR" || exit 1

# Classifiers to test
CLASSIFIERS=("cjson" "cjson_2step" "onnx" "onnx_2step")

# Executables with arguments and expected class
EXECUTABLES=(
  # #----------------------------------- TRAINED -----------------------------------#  
  # "$HOME/eidiko/benchmarks/compute_bound/Primes/PrimeC/solution_2/sieve_1of2_epar:compute:1,0,0"
  # "$HOME/eidiko/benchmarks/compute_bound/Primes/PrimeC/solution_2/sieve_8of30_epar:compute:1,0,0"
  # "$HOME/eidiko/benchmarks/compute_bound/Primes/PrimeC/solution_2/sieve_48of210_epar:compute:1,0,0"
  # "$HOME/eidiko/benchmarks/compute_bound/Primes/PrimeC/solution_2/sieve_480of2310_epar:compute:1,0,0"
  # "$HOME/eidiko/benchmarks/compute_bound/Primes/PrimeC/solution_2/sieve_5760of30030_epar:compute:1,0,0"

  # "$HOME/.local/bin/fio --name=test --ioengine=posixaio --runtime=5s --direct=1 --rw=randread --bs=4k --iodepth=32 --filename=fio_temp_file --size=1G --group_reporting=1:io:0,1,0"
  # "$HOME/.local/bin/fio --name=test --ioengine=sync --runtime=5s --direct=1 --rw=write --bs=128k --iodepth=1 --filename=fio_temp_file --size=1G --group_reporting=1:io:0,1,0"
  # "$HOME/.local/bin/fio --name=test --ioengine=posixaio --runtime=5s --direct=1 --rw=randrw --rwmixread=70 --bs=8k --iodepth=16 --filename=fio_temp_file --size=1G --group_reporting=1:io:0,1,0"
  # "$HOME/.local/bin/fio --name=test --ioengine=posixaio --runtime=5s --direct=1 --rw=randwrite --bs=512 --iodepth=64 --filename=fio_temp_file --size=1G --group_reporting=1:io:0,1,0"
  # "$HOME/.local/bin/fio --name=test --ioengine=sync --runtime=5s --direct=1 --rw=read --bs=1M --iodepth=1 --filename=fio_temp_file --size=1G --group_reporting=1:io:0,1,0"

  # "$HOME/eidiko/benchmarks/memory_bound/STREAM/stream_c.exe:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/memory_bound/STREAM/stream_c.exe:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/memory_bound/STREAM/stream_c.exe:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/memory_bound/STREAM/stream_c.exe:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/memory_bound/STREAM/stream_c.exe:memory:0,0,1"

  # #----------------------------------- VALIDATE -----------------------------------#
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/cg.A.x:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/cg.B.x:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/cg.C.x:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/ep.A.x:compute:1,0,0"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/ep.B.x:compute:1,0,0"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/ep.C.x:compute:1,0,0"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/ft.A.x:compute:1,0,0"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/ft.B.x:compute:1,0,0"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/ft.C.x:compute:1,0,0"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/is.A.x:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/is.B.x:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/is.C.x:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/mg.A.x:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/mg.B.x:memory:0,0,1"
  # "$HOME/eidiko/benchmarks/NPB/NPB3.4-OMP/bin/mg.C.x:memory:0,0,1"

  # "$HOME/eidiko/benchmarks/tiobench/tiotest -f 100 -b 4096 -t THREADS -r 200000 -X -d $OUTPUT_DIR/tiobench_data -L:io:0,1,0"
  # "$HOME/eidiko/benchmarks/tiobench/tiotest -f 200 -b 16384 -t THREADS -r 100000 -X -S -W -d $OUTPUT_DIR/tiobench_data -L:io:0,1,0"
  # "$HOME/eidiko/benchmarks/tiobench/tiotest -f 150 -b 8192 -t THREADS -r 150000 -X -S -d $OUTPUT_DIR/tiobench_data -L:io:0,1,0"

  # "/usr/bin/stress-ng --hdd WORKERS --hdd-bytes 1G --hdd-write-size 4k --hdd-opts direct,wr-rnd --taskset CORESET --temp-path $OUTPUT_DIR/stressng_data --timeout 5s --metrics-brief:io:0,1,0"
  # "/usr/bin/stress-ng --iomix WORKERS --iomix-bytes 1G --taskset CORESET --temp-path $OUTPUT_DIR/stressng_data --timeout 5s --metrics-brief:io:0,1,0"
  # "/usr/bin/stress-ng --fallocate WORKERS --fallocate-bytes 1G --taskset CORESET --temp-path $OUTPUT_DIR/stressng_data --timeout 5s --metrics-brief:io:0,1,0"

  # "$HOME/eidiko/benchmarks/io_bound/Benchmark2/iozone3_491/src/current/iozone -t 2 -s 16M -r 4k -i 0 -i 1 -+n:io:0,1,0"

  # "$HOME/eidiko/benchmarks/memory_bound/BabelStream/build/omp-stream:memory:0,0,1"

  # #----------------------------------- CUSTOM TESTS -----------------------------------#

  # "$HOME/eidiko/papi_examples/compute_bound/matrix_mul_mkl_pure 5000 5000 5000:compute:1,0,0"
  # "$HOME/eidiko/papi_examples/compute_bound/matrix_mul_omp_pure_tiled 2000 2000 2000:compute:1,0,0"
  # "$HOME/eidiko/papi_examples/io_bound/io_intense_omp_pure 4 1000:io:0,1,0"
  # "$HOME/eidiko/papi_examples/io_bound/io_intense_pthread_pure 4 1000:io:0,1,0"
  # "$HOME/eidiko/papi_examples/memory_bound/matrix_transpose_mkl_pure 5000:memory:0,0,1"
  # "$HOME/eidiko/papi_examples/memory_bound/matrix_transpose_omp_naive_pure 5000:memory:0,0,1"

  #----------------------------------- REAL APPS -----------------------------------#
  
  # "$HOME/eidiko/benchmarks/john/run/john --format=md5crypt $OUTPUT_DIR/passwords.txt:compute:1,0,0"
  # "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/BMW27.blend -f 1 -- --cycles-device CPU:compute:1,0,0"
  # Blender
  "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/BMW27.blend -f 1 -- --cycles-device CPU --cycles-samples 256:io:0,1,0"
  "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/BMW27.blend -f 1 -- --cycles-device CPU --cycles-samples 512:io:0,1,0"
  # "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/BMW27.blend -f 1 -- --cycles-device CPU --cycles-samples 1024:compute:1,0,0"
  "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/classroom/classroom.blend -f 1 -- --cycles-device CPU --cycles-samples 256:io:0,1,0"
  "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/classroom/classroom.blend -f 1 -- --cycles-device CPU --cycles-samples 512:io:0,1,0"
  # "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/classroom/classroom.blend -f 1 -- --cycles-device CPU --cycles-samples 1024:compute:1,0,0"
  "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/pabellon_barcelona.blend -f 1 -- --cycles-device CPU --cycles-samples 256:io:0,1,0"
  "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/pabellon_barcelona.blend -f 1 -- --cycles-device CPU --cycles-samples 512:io:0,1,0"
  # "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/pabellon_barcelona.blend -f 1 -- --cycles-device CPU --cycles-samples 1024:compute:1,0,0"
  "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/cycles/benchmark/fishy_cat/fishy_cat_cpu.blend -f 1 -- --cycles-device CPU --cycles-tile-size 8:io:0,1,0"
  "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/cycles/benchmark/fishy_cat/fishy_cat_cpu.blend -f 1 -- --cycles-device CPU --cycles-tile-size 16:io:0,1,0"
  # "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/barbershop_interior.blend -f 1 -- --cycles-device CPU --cycles-tile-size 32:compute:1,0,0"
  "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/cycles/benchmark/victor/victor_cpu.blend -f 1 -- --cycles-device CPU --cycles-tile-size 8:io:0,1,0"
  "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/cycles/benchmark/victor/victor_cpu.blend -f 1 -- --cycles-device CPU --cycles-tile-size 16:io:0,1,0"
  # "$HOME/eidiko/benchmarks/blender-4.4.3-linux-x64/blender -b ~/eidiko/benchmarks/blender-4.4.3-linux-x64/junkshop.blend -f 1 -- --cycles-device CPU --cycles-tile-size 32:compute:1,0,0"
  # SQLite
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench_lite.sql:io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench.sql:io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench_heavy.sql:io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench_read_lite.sql:io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench_read.sql:io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench_read_heavy.sql:io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench_blob_lite.sql:io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench_blob.sql:io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench_blob_heavy.sql:io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench_transactional_lite.sql:io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench_transactional.sql:io:0,1,0"
  "$HOME/local/bin/sqlite3 /tmp/test.db < ~/eidiko/benchmarks/sqlite_tests/sqlite_bench_transactional_heavy.sql:io:0,1,0"
  # GnuPG
  "/usr/bin/gpg --batch --yes --passphrase testpass -c ~/eidiko/benchmarks/gnupg/testfile:compute:1,0,0"
  "echo -n "testdata" | /usr/bin/gpg --batch --yes --passphrase testpass -c --cipher-algo AES256:compute:1,0,0"
  "head -c 10240 /dev/urandom | /usr/bin/gpg --batch --yes --passphrase testpass -c --cipher-algo TWOFISH:compute:1,0,0"
  Memtier (Redis)
  Launch Redis on default port 6379 in background
  $HOME/eidiko/benchmarks/redis/src/redis-server --port 6379 &
  "$HOME/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 6379 -c 4 -t 32 --data-size=65536 --key-pattern=R:R:io:0,1,0"
  "$HOME/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 6379 -c 8 -t 32 --data-size=32678 --ratio=10:1 --pipeline=10 --key-pattern=S:S:io:0,1,0"
  "$HOME/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 6379 -c 4 -t 16 --data-size=131072 --ratio=1:10 --key-pattern=G:G:io:0,1,0"
  "$HOME/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 6379 -c 16 -t 32 --data-size=32678 --ratio=1:1 --pipeline=5 --key-pattern=R:R:io:0,1,0"
  "$HOME/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p 6379 -c 4 -t 16 --data-size=4096 --ratio=5:1 --rate-limiting=1000 --key-pattern=S:S:io:0,1,0"
  # Clomp (MPI)
  "$HOME/eidiko/benchmarks/clomp/clomp_mpi -1 1 32 640000 32 1 100:memory:0,0,1"
  "$HOME/eidiko/benchmarks/clomp/clomp_mpi -1 -1 32 1280000 32 1 100:memory:0,0,1"
  "$HOME/eidiko/benchmarks/clomp/clomp_mpi -1 1 32 320000 64 1 50:memory:0,0,1"
  "$HOME/eidiko/benchmarks/clomp/clomp_mpi -1 -1 32 1280000 32 1 50:memory:0,0,1"
  # FluidX3D
  "$HOME/eidiko/benchmarks/FluidX3D/bin/FluidX3D 0:memory:0,0,1"
)

# Core sets
coresets=(
  "0" "16"
  "0,1" "0,2" "0,16" "16,17"
  "0,1,2,3" "0,2,4,6" "0,1,16,17" "0,2,16,17" "16,17,18,19"
  "0,1,2,3,4,5,6,7" "0,2,4,6,8,10,12,14" "16,17,18,19,20,21,22,23"
  "0,1,2,3,16,17,18,19" "0,2,4,6,16,17,18,19"
  "0-15" "16-31"
  "0,1,2,3,4,5,6,7,16,17,18,19,20,21,22,23"
  "0,2,4,6,8,10,12,14,16,17,18,19,20,21,22,23"
  "0-31"
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

# Output files
SUMMARY_CSV="$OUTPUT_DIR/classifier_metrics.csv"

# Function to calculate OMP_NUM_THREADS (P-Threads + E-Cores)
calc_omp_threads() {
  local coreset="$1"
  IFS=',' read -r p_threads p_cores e_cores <<< "${coreset_mapping[$coreset]}"
  echo $((p_threads + e_cores))
}

# Initialize summary CSV
echo "Classifier,Coreset,Accuracy,Certainty,Avg_Classification_Time_us" > "$SUMMARY_CSV"

# Function to process CSV and compute metrics
process_csv() {
  local classifier="$1"
  local coreset="$2"
  local csv_file="train_dataset_dli_test.csv"
  local temp_csv="$OUTPUT_DIR/temp_${classifier}_${coreset//[,-]/_}.csv"
  local total=0
  local correct=0
  local total_time=0
  local total_certainty=0

  # Copy CSV and append expected class
  echo "Processing $csv_file for $classifier on $coreset"
  cp "$csv_file" "$temp_csv"
  for exec_info in "${EXECUTABLES[@]}"; do
    IFS=':' read -r exec_cmd class flags <<< "$exec_info"
    sed -i "/$class/s/unknown/$class/" "$temp_csv"
  done

  # Read CSV and compute metrics
  tail -n +2 "$temp_csv" | while IFS=',' read -r p_threads p_cores e_cores \
    inst_retired cache_misses unhalted_cycles mem_inst faults cycles_mem uops_retired \
    ipc cache_miss_ratio uop_per_cycle memstall_per_mem memstall_per_inst \
    fault_rate rchar_per_cycle wchar_per_cycle rbytes_per_cycle wbytes_per_cycle \
    syscr syscw exec_time_ms rchar wchar read_bytes write_bytes \
    compute_prob io_prob memory_prob class_time_us expected_class; do
    ((total++))
    # Determine predicted class
    if (( $(echo "$compute_prob > $io_prob && $compute_prob > $memory_prob" | bc -l) )); then
      predicted_class="compute"
    elif (( $(echo "$io_prob > $memory_prob" | bc -l) )); then
      predicted_class="io"
    else
      predicted_class="memory"
    fi
    # Check accuracy
    if [[ "$predicted_class" == "$expected_class" ]]; then
      ((correct++))
    fi
    # Add classification time
    total_time=$((total_time + class_time_us))
    # Compute certainty (max probability)
    max_prob=$(echo "$compute_prob $io_prob $memory_prob" | tr ' ' '\n' | sort -nr | head -n1)
    total_certainty=$(echo "$total_certainty + $max_prob" | bc -l)
  done

  # Calculate metrics
  if [ $total -gt 0 ]; then
    accuracy=$(echo "scale=4; $correct / $total" | bc)
    avg_time=$(echo "scale=2; $total_time / $total" | bc)
    avg_certainty=$(echo "scale=4; $total_certainty / $total" | bc)
    echo "$classifier,$coreset,$accuracy,$avg_certainty,$avg_time" >> "$SUMMARY_CSV"
  else
    echo "$classifier,$coreset,0.0,0.0,0.0" >> "$SUMMARY_CSV"
  fi
}

# Run experiments
for classifier in "${CLASSIFIERS[@]}"; do
  echo "Building with CLASSIFIER=$classifier"
  make clean
  make CLASSIFIER="$classifier" || { echo "Build failed for $classifier"; exit 1; }

  for coreset in "${coresets[@]}"; do
    echo "Testing $classifier on coreset $coreset"
    rm -f train_dataset_dli_test.csv
    # Start scheduler
    ./scheduler "8" &
    scheduler_pid=$!
    sleep 2

    for exec_info in "${EXECUTABLES[@]}"; do
      IFS=':' read -r exec_cmd class flags <<< "$exec_info"
      exec_name=$(echo "$exec_cmd" | cut -d' ' -f1)
      exec_name=$(eval echo "$exec_name")
      exec_args=$(echo "$exec_cmd" | cut -s -d' ' -f2-)
      if [[ ! -x "$exec_name" ]]; then
        echo "Error: Executable $exec_name does not exist or is not executable"
        continue
      fi
      omp_threads=$(calc_omp_threads "$coreset")
      IFS=',' read -r p_threads p_cores e_cores <<< "${coreset_mapping[$coreset]}"
      numjobs=$((p_threads + e_cores))
      if [[ "$exec_name" =~ memtier_benchmark$ ]]; then
        modified_args=$(echo "$exec_args" | sed "s/-t THREADS/-t $numjobs/")
      else
        modified_args="$exec_args"
      fi
      echo "Running: LD_PRELOAD=$SCRIPT_DIR/libmonitor.so OMP_NUM_THREADS=$omp_threads taskset -c $coreset $exec_name $modified_args"
      LD_PRELOAD="$SCRIPT_DIR/libmonitor.so" OMP_NUM_THREADS="$omp_threads" taskset -c "$coreset" "$exec_name" $modified_args
      sleep 2
    done

    # Shutdown scheduler
    ./shutdown_scheduler
    wait $scheduler_pid

    # Process results
    if [ -f train_dataset_dli_test.csv ]; then
      process_csv "$classifier" "$coreset"
    else
      echo "No results for $classifier on $coreset"
      echo "$classifier,$coreset,0.0,0.0,0.0" >> "$SUMMARY_CSV"
    fi
    sleep 2
  done
done

echo "Results saved to $SUMMARY_CSV"