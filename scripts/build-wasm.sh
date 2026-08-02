#!/usr/bin/env bash
# Build chaaya-wasm. Without a WASI toolchain this produces the host stub binary.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build-wasm}"

cmake -S "$ROOT" -B "$BUILD" -DCHAAYA_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j --target chaaya-wasm
echo "Built: $BUILD/chaaya-wasm (or chaaya_wasm)"
ls -la "$BUILD"/chaaya-wasm "$BUILD"/chaaya_wasm 2>/dev/null || true
