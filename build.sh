#!/usr/bin/env bash
set -euo pipefail

echo "[1/4] Configuring CMake..."
mkdir -p build

TOOLCHAIN_ARG=""
if [ -n "${VCPKG_ROOT:-}" ]; then
    TOOLCHAIN_ARG="-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
fi

cmake -B build -S . $TOOLCHAIN_ARG -DCMAKE_BUILD_TYPE=Release

echo "[2/4] Building..."
cmake --build build --config Release -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo "[3/4] Setting up output directory..."
mkdir -p bin

echo "[4/4] Done."
echo "Run ./bin/moreno_text to launch."
