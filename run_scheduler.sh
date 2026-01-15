#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="$SCRIPT_DIR/current_config.txt"
SOCKET_PATH="/tmp/scheduler_socket"
SCHEDULER="$SCRIPT_DIR/scheduler"

# Clear old state
rm -f "$SOCKET_PATH"
> "$CONFIG_FILE"

echo "Scheduler controller started. Waiting for test_executor..."

while true; do
    # Wait for test_executor to write a new config
    while [ ! -s "$CONFIG_FILE" ]; do
        sleep 1
    done

    # Read the config
    CORESET=$(grep '^CORESET=' "$CONFIG_FILE" | cut -d= -f2)
    if [ -z "$CORESET" ]; then
        echo "Error: Invalid CORESET in $CONFIG_FILE"
        sleep 1
        continue
    fi

    echo "🔧 Starting scheduler: CORESET=$CORESET"
    $SCHEDULER $CORESET &> "$SCRIPT_DIR/scheduler_log_${CORESET}.txt" &
    SCHEDULER_PID=$!
    echo "Scheduler PID: $SCHEDULER_PID"

    # Verify scheduler started
    sleep 2
    if ! ps -p $SCHEDULER_PID > /dev/null; then
        echo "Error: Scheduler failed to start for CORESET=$CORESET"
        > "$CONFIG_FILE" # Clear config to avoid deadlock
        continue
    fi

    # Block until shutdown is requested (CONFIG_FILE cleared)
    while [ -s "$CONFIG_FILE" ]; do
        sleep 1
    done

    echo "Terminating scheduler PID $SCHEDULER_PID"
    kill $SCHEDULER_PID 2>/dev/null
    sleep 1
    if ps -p $SCHEDULER_PID > /dev/null; then
        echo "Force-killing scheduler PID $SCHEDULER_PID"
        kill -9 $SCHEDULER_PID 2>/dev/null
    fi
    rm -f "$SOCKET_PATH"
    echo "Scheduler terminated. Waiting for next config..."
done
