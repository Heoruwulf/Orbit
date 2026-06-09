#!/bin/bash

DURATION=${1:-120}

# Find the PID of the running orbit process
PID=$(pidof orbit)

if [ -z "$PID" ]; then
    echo "Error: orbit is not running."
    exit 1
fi

echo "Found orbit running with PID: $PID"
echo "Starting hardware profiling for ${DURATION} seconds..."

perf stat \
    -e cycles,instructions,cache-references,cache-misses,branches,branch-misses,page-faults,minor-faults,major-faults \
    -p "$PID" \
    -- sleep "$DURATION"

echo "Profiling complete!"
