#include <stdio.h>

int main(void) {
    fprintf(stderr,
            "chaaya-wasm: this binary is a host stub (no WASI toolchain configured).\n");
    fprintf(stderr,
            "Use the helper script:\n"
            "  ./scripts/build-wasm.sh\n"
            "Or configure CMake manually:\n"
            "  cmake -S . -B build-wasm -DCHAAYA_WASM=ON "
            "-DCMAKE_TOOLCHAIN_FILE=<wasi-sdk.cmake>\n"
            "  cmake --build build-wasm -j --target chaaya-wasm\n");
    return 1;
}
