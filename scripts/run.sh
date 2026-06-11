#!/bin/bash
set -euo pipefail

# Change to the root of the project
cd "$(dirname "$0")/.." || exit 1

# Export environment variables from .env.local file securely
if [[ -f ".env.local" ]]; then
    echo "[INFO] Loading .env.local file..."
    set -a
    # shellcheck disable=SC1091
    source .env.local
    set +a
else
    echo "[WARN] No .env.local file found. Relying on system/default configuration."
fi

# Ensure the logs directory exists
mkdir -p logs

# Ensure the binary is built
BIN="./build/src/orbit"
if [[ ! -f "$BIN" ]]; then
    echo "[ERRO] Executable not found at $BIN. Please run 'cmake --build build' first."
    exit 1
fi

if [[ ! -x "$BIN" ]]; then
    echo "[ERRO] Binary at $BIN is not executable."
    exit 1
fi

# Generate timestamped log filename
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
LOG_FILE="logs/${TIMESTAMP}_orbit.log"

echo "[INFO] Starting orbit server..."
echo "[INFO] Redirecting stdout and stderr to $LOG_FILE"
echo "[INFO] Tail the log file to view output: tail -f $LOG_FILE"

# Execute the server, replacing the current shell process, and redirect output
exec "$BIN" > "$LOG_FILE" 2>&1
