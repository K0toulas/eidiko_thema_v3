#!/bin/bash

# Simple test executor to run 9 processes (3 compute, 3 IO, 3 memory) with OMP_NUM_THREADS=16
# Can run with or without scheduler based on LD_PRELOAD argument

# Usage: ./scheduler_test1.sh [LD_PRELOAD_PATH]
# If LD_PRELOAD_PATH is provided, use scheduler mode
# If no argument, run in CFS mode

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="scheduler_test1"
LOG_DIR="$TEST_DIR/logs"
RESULT_FILE="$TEST_DIR/scheduler_results.csv"
DEBUG_LOG="$LOG_DIR/debug.log"
TIMING_LOG="$LOG_DIR/timing.log"

# Database and file paths
DB="/tmp/test.db"
DUMMYFILE="$HOME/eidiko/benchmarks/dummyfile1"
REDIS_PORT=7779

# Hardcoded workload executables
COMPUTE_EXECS=(
    "/srv/homes/ggantsios/eidiko/papi_examples/compute_bound/matrix_mul_mkl_pure 5000 5000 5000"
    "/srv/homes/ggantsios/eidiko/papi_examples/compute_bound/matrix_mul_mkl_pure 20000 20000 20000"
    "/srv/homes/ggantsios/eidiko/papi_examples/compute_bound/matrix_mul_mkl_pure 5000 5000 5000"
)
IO_EXECS=(
    "/srv/homes/ggantsios/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p $REDIS_PORT -c 8 -t 16 --data-size=32678 --ratio=10:1 --pipeline=10 --key-pattern=S:S"
    "/srv/homes/ggantsios/eidiko/benchmarks/memtier_benchmark/memtier_benchmark -s 127.0.0.1 -p $REDIS_PORT -c 4 -t 16 --data-size=131072 --ratio=1:10 --key-pattern=G:G"
    "/srv/homes/ggantsios/eidiko/papi_examples/io_bound/io_intense_omp_pure 4 10000"
)
MEMORY_EXECS=(
    "/srv/homes/ggantsios/eidiko/benchmarks/clomp/clomp_mpi -1 -1 32 640000 32 1 100"
    "/srv/homes/ggantsios/eidiko/benchmarks/clomp/clomp_mpi -1 -1 32 1280000 32 1 100"
    "/srv/homes/ggantsios/eidiko/papi_examples/memory_bound/matrix_transpose_omp_pure 15000"
)

# Handle LD_PRELOAD argument
SCHEDULER_LIB="$1"
if [[ -n "$SCHEDULER_LIB" ]] && [[ -f "$SCHEDULER_LIB" ]]; then
    echo "Using scheduler mode with LD_PRELOAD=$SCHEDULER_LIB" | tee -a "$DEBUG_LOG"
    export LD_PRELOAD="$SCHEDULER_LIB"
    MODE="scheduler"
else
    echo "Using CFS mode (no LD_PRELOAD)" | tee -a "$DEBUG_LOG"
    unset LD_PRELOAD
    MODE="cfs"
fi

# Function to check if a file exists and is executable
check_executable() {
    local executable="$1"
    # Extract just the command path (remove arguments)
    local cmd_path=$(echo "$executable" | awk '{print $1}')
    
    if [[ ! -f "$cmd_path" ]]; then
        echo "Error: File not found: $cmd_path" | tee -a "$DEBUG_LOG"
        return 1
    fi
    
    if [[ ! -x "$cmd_path" ]]; then
        echo "Error: File not executable: $cmd_path" | tee -a "$DEBUG_LOG"
        return 1
    fi
    
    echo "Found executable: $cmd_path" | tee -a "$DEBUG_LOG"
    return 0
}

# Check dependencies
check_dependencies() {
    echo "Checking dependencies..." | tee -a "$DEBUG_LOG"
    
    # Check compute executables
    for cmd in "${COMPUTE_EXECS[@]}"; do
        check_executable "$cmd" || exit 1
    done
    
    # Check IO executables
    for cmd in "${IO_EXECS[@]}"; do
        check_executable "$cmd" || exit 1
    done
    
    # Check memory executables
    for cmd in "${MEMORY_EXECS[@]}"; do
        check_executable "$cmd" || exit 1
    done
    
    echo "All dependencies found and executable" | tee -a "$DEBUG_LOG"
}

# Setup test environment
setup() {
    echo "Setting up test environment" | tee -a "$DEBUG_LOG"
    mkdir -p "$TEST_DIR" "$LOG_DIR"
    rm -f "$DB" "$DUMMYFILE" "$RESULT_FILE" "$DEBUG_LOG" "$TIMING_LOG" "$TEST_DIR"/pid_* "$TEST_DIR"/time_*
    touch "$RESULT_FILE" "$DEBUG_LOG" "$TIMING_LOG"
    echo "Mode,Overall_Time_s,Max_Latency_s,Max_Starvation_s,Avg_Execution_s" > "$RESULT_FILE"
    
    # Create dummy file for tests
    dd if=/dev/zero of="$DUMMYFILE" bs=1M count=100 status=none 2>/dev/null
    
    # Create empty database for SQLite
    touch "$DB"
}

# Cleanup
cleanup() {
    echo "Cleaning up test environment" | tee -a "$DEBUG_LOG"
    # Kill any remaining processes
    pkill -f "matrix_mul_omp_pure_tiled" 2>/dev/null || true
    pkill -f "matrix_mul_mkl_pure" 2>/dev/null || true
    pkill -f "clomp_mpi" 2>/dev/null || true
    pkill -f "memtier_benchmark" 2>/dev/null || true
    pkill -f "io_intense_omp_pure" 2>/dev/null || true
    pkill -f "matrix_transpose_omp_pure" 2>/dev/null || true
    
    # Remove temporary files
    rm -f "$DB" "$DUMMYFILE" "$TEST_DIR"/pid_* "$TEST_DIR"/time_*
}

# Function to get current time in milliseconds
get_time_ms() {
    echo $(date +%s%3N)
}

# Run a single process and measure execution time with timestamps
run_process() {
    local cmd="$1"
    local type="$2"
    local index="$3"
    local omp_threads=16
    
    echo "Starting $type process $index: $cmd" | tee -a "$DEBUG_LOG"
    export OMP_NUM_THREADS="$omp_threads"
    
    local start_time=$(get_time_ms)
    echo "START,$type,$index,$start_time" >> "$TIMING_LOG"
    
    # Run the command in background but capture its PID
    # Use a subshell to isolate the environment
    (
        # LD_PRELOAD will be inherited from parent environment
        eval "$cmd" >/dev/null 2>&1
    ) &
    local pid=$!
    echo "$pid" > "$TEST_DIR/pid_${type}_${index}"
    
    # Wait for the process to complete and get its exit status
    wait $pid
    local status=$?
    
    local end_time=$(get_time_ms)
    echo "END,$type,$index,$end_time" >> "$TIMING_LOG"
    
    local runtime=$(echo "scale=3; ($end_time - $start_time) / 1000" | bc)
    echo "$runtime" > "$TEST_DIR/time_${type}_${index}.time"
    
    echo "Completed $type process $index in $runtime seconds with status $status" | tee -a "$DEBUG_LOG"
    return $status
}

# Calculate starvation time from timing log
calculate_starvation() {
    local timing_log="$1"
    local max_starvation=0
    
    if [[ ! -f "$timing_log" ]]; then
        echo "Warning: Timing log $timing_log not found" | tee -a "$DEBUG_LOG"
        echo "0"
        return
    fi
    
    # Parse all start and end events into arrays
    declare -A start_times
    declare -A end_times
    
    while IFS=',' read -r event type index timestamp; do
        if [[ "$event" == "START" ]]; then
            start_times["${type}_${index}"]=$timestamp
        elif [[ "$event" == "END" ]]; then
            end_times["${type}_${index}"]=$timestamp
        fi
    done < "$timing_log"
    
    # Create a timeline of all events
    declare -a events
    for key in "${!start_times[@]}"; do
        events+=("${start_times[$key]},start,$key")
    done
    for key in "${!end_times[@]}"; do
        events+=("${end_times[$key]},end,$key")
    done
    
    # Sort events by timestamp
    IFS=$'\n' sorted_events=($(sort -n <<<"${events[*]}"))
    unset IFS
    
    # Track running processes and find the longest period with no processes running
    local running_count=0
    local last_event_time=0
    local current_starvation=0
    
    for event in "${sorted_events[@]}"; do
        IFS=',' read timestamp action key <<< "$event"
        
        if [[ $running_count -eq 0 ]] && [[ $last_event_time -gt 0 ]]; then
            # No processes were running between last_event_time and timestamp
            current_starvation=$(echo "scale=3; ($timestamp - $last_event_time) / 1000" | bc)
            if (( $(echo "$current_starvation > $max_starvation" | bc -l) )); then
                max_starvation=$current_starvation
            fi
        fi
        
        if [[ "$action" == "start" ]]; then
            ((running_count++))
        elif [[ "$action" == "end" ]]; then
            ((running_count--))
        fi
        
        last_event_time=$timestamp
    done
    
    echo "$max_starvation"
}

# Run the test
run_test() {
    local total_start=$(get_time_ms)
    local pids=()

    echo "Running in $MODE mode, LD_PRELOAD=$LD_PRELOAD" | tee -a "$DEBUG_LOG"

    # Start all processes in parallel
    for i in $(seq 1 3); do
        run_process "${COMPUTE_EXECS[$((i-1))]}" "compute" "$i" &
        pids+=($!)
    done

    for i in $(seq 1 3); do
        run_process "${IO_EXECS[$((i-1))]}" "io" "$i" &
        pids+=($!)
    done

    for i in $(seq 1 3); do
        run_process "${MEMORY_EXECS[$((i-1))]}" "memory" "$i" &
        pids+=($!)
    done

    # Wait for all background processes to complete
    echo "Waiting for all processes to complete..." | tee -a "$DEBUG_LOG"
    for pid in "${pids[@]}"; do
        wait $pid
    done

    local total_end=$(get_time_ms)
    local total_time=$(echo "scale=3; ($total_end - $total_start) / 1000" | bc)

    # Calculate metrics
    local max_latency=0
    local sum_times=0
    local count_times=0
    for time_file in "$TEST_DIR"/time_*.time; do
        if [[ -f "$time_file" ]]; then
            local runtime=$(cat "$time_file" 2>/dev/null)
            if [[ -n "$runtime" ]] && [[ "$runtime" =~ ^[0-9.]+$ ]]; then
                count_times=$((count_times + 1))
                sum_times=$(echo "$sum_times + $runtime" | bc -l)
                if (( $(echo "$runtime > $max_latency" | bc -l) )); then
                    max_latency=$runtime
                fi
            fi
        fi
    done

    local avg_time=0
    if (( count_times > 0 )); then
        avg_time=$(echo "scale=3; $sum_times / $count_times" | bc -l)
    fi

    local starvation_time=$(calculate_starvation "$TIMING_LOG")

    echo "$MODE,$total_time,$max_latency,$starvation_time,$avg_time" >> "$RESULT_FILE"
    
    echo "Test completed:" | tee -a "$DEBUG_LOG"
    echo "  Mode: $MODE" | tee -a "$DEBUG_LOG"
    echo "  Total time: $total_time seconds" | tee -a "$DEBUG_LOG"
    echo "  Max latency: $max_latency seconds" | tee -a "$DEBUG_LOG"
    echo "  Max starvation: $starvation_time seconds" | tee -a "$DEBUG_LOG"
    echo "  Average execution: $avg_time seconds" | tee -a "$DEBUG_LOG"
}

# Main execution
check_dependencies
setup
trap cleanup EXIT

run_test

# Summarize results
echo "" | tee -a "$DEBUG_LOG"
echo "Results:" | tee -a "$DEBUG_LOG"
cat "$RESULT_FILE" | tee -a "$DEBUG_LOG"