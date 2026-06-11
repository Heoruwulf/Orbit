#!/bin/bash
set -euo pipefail

# Change to the root of the project securely
cd "$(dirname "$0")/.." || exit 1

BUILD_TYPE="Debug"
DO_CLEAN=false
ENABLE_REDIS="ON"
ENABLE_KAFKA="ON"

# Parse command line arguments
for arg in "$@"; do
    case "${arg,,}" in  # Convert to lowercase
        release)
            BUILD_TYPE="Release"
            ;;
        clean)
            DO_CLEAN=true
            ;;
        no-redis)
            ENABLE_REDIS="OFF"
            ;;
        no-kafka)
            ENABLE_KAFKA="OFF"
            ;;
        *)
            echo "[ERRO] Unknown argument: $arg"
            echo "Usage: ./scripts/build.sh [release] [clean] [no-redis] [no-kafka]"
            exit 1
            ;;
    esac
done

if [[ "$DO_CLEAN" == true ]]; then
    echo "[INFO] Cleaning build directory..."
    rm -rf build/
    echo "[INFO] Clean complete."
    # If the user only passed 'clean' and no build type, we could exit here,
    # but rebuilding after clean is standard when both are requested, or even just 'clean'.
    # If they just want to clean without building, we can check arguments.
    if [[ "$#" -eq 1 ]]; then
        exit 0
    fi
fi

echo "[INFO] Generating CMake configuration (Build Type: $BUILD_TYPE)..."
# -B build defines the build directory
# -DCMAKE_BUILD_TYPE sets Debug or Release optimizations and symbols
cmake -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DENABLE_EVENT_REDIS="$ENABLE_REDIS" -DENABLE_EVENT_KAFKA="$ENABLE_KAFKA"

echo "[INFO] Compiling orbit..."
# Build utilizing all available CPU cores
cmake --build build -j"$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "[INFO] Build completed successfully! Binary is located at ./build/src/orbit"
