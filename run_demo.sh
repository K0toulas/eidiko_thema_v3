#!/usr/bin/env bash
set -euo pipefail

SCHED_CORESET="${1:-0-15}"
MON_CORESET="${2:-0-15}"
WORKLOAD="${3:-./highmiss_loop}"

echo "[DEMO] scheduler coreset: ${SCHED_CORESET}"
echo "[DEMO] monitor CORESET:   ${MON_CORESET}"
echo "[DEMO] workload:          ${WORKLOAD}"

CC="${CC:-gcc}"
CFLAGS=(-O2 -g -Wall -Wextra -fno-omit-frame-pointer)
PICFLAGS=(-fPIC)

echo "[DEMO] building perf backend + monitor..."
"${CC}" "${CFLAGS[@]}" "${PICFLAGS[@]}" -c perf_backend.c -o perf_backend.o
"${CC}" "${CFLAGS[@]}" "${PICFLAGS[@]}" -DQUIET_MONITOR -DMONITOR_SPLIT_DEBUG -shared -o libmonitor.so libmonitor.c perf_backend.o -ldl -lpthread

echo "[DEMO] building scheduler..."
"${CC}" "${CFLAGS[@]}" -I. -o scheduler scheduler.c libclassifier.c cJSON.c -lpthread -lm

# Build workload if source exists and binary is missing/outdated
if [[ -f "${WORKLOAD}.c" ]]; then
  echo "[DEMO] building workload from ${WORKLOAD}.c ..."
  "${CC}" "${CFLAGS[@]}" -o "${WORKLOAD}" "${WORKLOAD}.c" -lpthread
else
  if [[ ! -x "${WORKLOAD}" ]]; then
    echo "[DEMO] ERROR: workload ${WORKLOAD} not executable and no ${WORKLOAD}.c found"
    exit 1
  fi
fi

# Clean stale socket
if [ -S /tmp/scheduler_socket ]; then
  echo "[DEMO] removing stale /tmp/scheduler_socket"
  rm -f /tmp/scheduler_socket
fi

: > classifier_val.csv
: > core_allocation.csv

echo "[DEMO] starting scheduler..."
./scheduler "${SCHED_CORESET}" > scheduler_stdout.log 2> scheduler_stderr.log &
SCHED_PID=$!
echo "[DEMO] scheduler PID: ${SCHED_PID}"

echo -n "[DEMO] waiting for /tmp/scheduler_socket"
for i in $(seq 1 50); do
  if [ -S /tmp/scheduler_socket ]; then
    echo
    echo "[DEMO] socket ready"
    break
  fi
  echo -n "."
  sleep 0.1
done

if [ ! -S /tmp/scheduler_socket ]; then
  echo
  echo "[DEMO] ERROR: scheduler socket did not appear."
  echo "[DEMO] scheduler_stdout.log:"
  tail -n 50 scheduler_stdout.log || true
  echo "[DEMO] scheduler_stderr.log:"
  tail -n 50 scheduler_stderr.log || true
  kill "${SCHED_PID}" 2>/dev/null || true
  exit 1
fi

echo "[DEMO] tailing classifier_val.csv and core_allocation.csv (Ctrl+C will stop script)"
tail -n 0 -f classifier_val.csv &
TAIL1=$!
tail -n 0 -f core_allocation.csv &
TAIL2=$!

cleanup() {
  echo
  echo "[DEMO] cleaning up..."

  kill "${TAIL1}" 2>/dev/null || true
  kill "${TAIL2}" 2>/dev/null || true

  if [ -x ./shutdown_scheduler ]; then
    echo "[DEMO] shutting down scheduler via ./shutdown_scheduler"
    ./shutdown_scheduler || true
  else
    echo "[DEMO] shutdown_scheduler not found; killing scheduler PID ${SCHED_PID}"
    kill "${SCHED_PID}" 2>/dev/null || true
  fi

  wait "${SCHED_PID}" 2>/dev/null || true
  echo "[DEMO] done."
}
trap cleanup EXIT INT TERM

echo "[DEMO] running workload under monitor..."
LD_PRELOAD=./libmonitor.so CORESET="${MON_CORESET}" "${WORKLOAD}"

echo "[DEMO] workload finished"