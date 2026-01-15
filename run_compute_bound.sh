#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(pwd)"
RESULT_DIR="${PROJECT_ROOT}/test_results/compute_bound"
mkdir -p "$RESULT_DIR"

# Ensure libmonitor.so exists
if [ ! -f "${PROJECT_ROOT}/libmonitor.so" ]; then
  echo "libmonitor.so not found. Build first:"
  echo "  make CORESET=\"0-15\" PAPI_DIR=\"/home/thanos/src/papi-7.2.0\""
  exit 1
fi

# Ensure monitor_receiver exists
if [ ! -x "${PROJECT_ROOT}/monitor_receiver" ]; then
  echo "monitor_receiver not found or not executable. Compile: gcc -O2 -o monitor_receiver monitor_receiver.c"
  exit 1
fi

# Core sets to test (P-cores, E-cores)
CORESETS=("0-7" "8-15")
THREADS=(1 2 4 8)
REPEATS=5

# Find sieve binaries / source
SIEVE_DIR="${PROJECT_ROOT}/tests/compute_bound"
cd "$SIEVE_DIR"
# If .c exist but not binary, compile them
for c in *.c; do
  [ -f "$c" ] || continue
  base="${c%.c}"
  if [ ! -x "${PROJECT_ROOT}/${base}" ]; then
    echo "Compiling $c -> ${base}"
    gcc -O3 -std=gnu11 -march=native -fopenmp "$c" -o "${PROJECT_ROOT}/${base}" -lm
  fi
done

# list of binaries to run (copy to array)
BINS=()
for f in "${PROJECT_ROOT}"/sieve_*of*_epar "${PROJECT_ROOT}"/sieve_1of2; do
  [ -x "$f" ] || continue
  BINS+=("$f")
done

if [ ${#BINS[@]} -eq 0 ]; then
  echo "No compute binaries found. Check tests/compute_bound or compiled binaries in project root."
  exit 1
fi

# Run the matrix
for bin in "${BINS[@]}"; do
  echo "===> Compute binary: $bin"
  for cs in "${CORESETS[@]}"; do
    for t in "${THREADS[@]}"; do
      for r in $(seq 1 $REPEATS); do
        RUN_NAME="$(basename "$bin")_coreset-${cs}_th-${t}_run-${r}_$(date +%Y%m%dT%H%M%S)"
        OUTDIR="${RESULT_DIR}/${RUN_NAME}"
        mkdir -p "$OUTDIR"
        # start receiver
        echo "[run] starting receiver (writing to ${OUTDIR}/monitor_data.csv)"
        "${PROJECT_ROOT}/monitor_receiver" "$OUTDIR" &
        RCV_PID=$!
        sleep 0.2
        # start turbostat (requires sudo) - sample every 1s
        echo "[run] starting turbostat (sudo) ..."
        sudo turbostat --interval 1 > "${OUTDIR}/turbostat.txt" 2>&1 &
        TURBO_PID=$!

        # run workload pinned and preloaded
        echo "[run] running workload: $bin pins=$cs threads=$t"
        export OMP_NUM_THREADS="$t"
        LD_PRELOAD="${PROJECT_ROOT}/libmonitor.so" taskset -c "${cs}" "$bin" > "${OUTDIR}/workload.stdout" 2> "${OUTDIR}/workload.stderr" || true

        # wait small grace period then stop turbostat & receiver
        sleep 1
        echo "[run] killing turbostat (pid $TURBO_PID) and receiver (pid $RCV_PID)"
        sudo kill -INT "$TURBO_PID" 2>/dev/null || sudo kill -TERM "$TURBO_PID" 2>/dev/null || true
        sleep 0.2
        kill "$RCV_PID" 2>/dev/null || true
        # cleanup socket
        rm -f /tmp/scheduler_socket

        # record metadata
        echo "binary,coreset,threads,run,timestamp,workload_stdout,workload_stderr,monitor_csv,turbostat" > "${OUTDIR}/metadata.txt"
        echo "$(basename "$bin"),\"$cs\",$t,$r,$(date +%Y-%m-%dT%H:%M:%S),workload.stdout,workload.stderr,monitor_data.csv,turbostat.txt" >> "${OUTDIR}/metadata.txt"

        # small cooldown
        sleep 2
      done
    done
  done
done

echo "Compute runs finished. Results under ${RESULT_DIR}"
