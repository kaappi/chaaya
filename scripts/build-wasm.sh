#!/usr/bin/env bash
# Build chaaya-wasm. Without a WASI toolchain this builds the host stub binary.
set -euo pipefail
IFS=$'\n\t'

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build-wasm}"
CMAKE_BIN="${CMAKE_BIN:-cmake}"
TOOLCHAIN_FILE="${CHAAYA_WASI_TOOLCHAIN:-}"

if [[ -z "${TOOLCHAIN_FILE}" && -n "${WASI_SDK_PATH:-}" ]]; then
  TOOLCHAIN_FILE="${WASI_SDK_PATH}/share/cmake/wasi-sdk.cmake"
fi

if ! command -v "${CMAKE_BIN}" >/dev/null 2>&1; then
  echo "build-wasm: cmake not found (CMAKE_BIN=${CMAKE_BIN})" >&2
  exit 1
fi

if [[ -n "${TOOLCHAIN_FILE}" && ! -f "${TOOLCHAIN_FILE}" ]]; then
  echo "build-wasm: toolchain file not found: ${TOOLCHAIN_FILE}" >&2
  exit 1
fi

mkdir -p "${BUILD}"

declare -a CMAKE_ARGS
CMAKE_ARGS=(
  -S "${ROOT}"
  -B "${BUILD}"
  -DCHAAYA_WASM=ON
  -DCMAKE_BUILD_TYPE=Release
)

if [[ -n "${TOOLCHAIN_FILE}" ]]; then
  CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}")
  echo "build-wasm: using toolchain ${TOOLCHAIN_FILE}"
else
  echo "build-wasm: no WASI toolchain configured; building host stub target"
fi

"${CMAKE_BIN}" "${CMAKE_ARGS[@]}"
"${CMAKE_BIN}" --build "${BUILD}" -j --target chaaya-wasm

OUTPUT_A="${BUILD}/chaaya-wasm"
OUTPUT_B="${BUILD}/chaaya_wasm"
if [[ -e "${OUTPUT_A}" || -e "${OUTPUT_B}" ]]; then
  echo "build-wasm: built ${OUTPUT_A} (or ${OUTPUT_B})"
else
  echo "build-wasm: target built but output binary was not found" >&2
  exit 1
fi

ls -la "${OUTPUT_A}" "${OUTPUT_B}" 2>/dev/null || true
