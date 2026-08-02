# Backends (MVP)

This note documents the current backend integration surface for Phase 11.

## Backend status

| Backend | Entry point | Current behavior |
|---------|-------------|------------------|
| Bytecode VM | default `chaaya` run path | Implemented and primary execution path |
| LLVM native | `chaaya compile` / `--native` / `--emit-llvm` | Emits real LLVM IR `@main`; returns constant exits for simple fixnum programs, otherwise calls runtime `@ch_rt_main` |
| WASM | `chaaya wasm` / CMake option `CHAAYA_WASM` | `chaaya wasm` runs `scripts/build-wasm.sh` when present; script supports both host-stub and WASI toolchain builds |
| LSP | stdin/stdout JSON-RPC | `initialize`, `didOpen`/`didChange` → `publishDiagnostics`, `completion`, `hover`, `definition`, `references`, `documentSymbol` |
| Compile cache | `$CHAAYA_HOME/cache/*.chbc` | Auto read/write on plain file runs (skipped when source mentions `import`, or when `CHAAYA_NO_CACHE` is set). Blob includes bytecode plus the global name table so absolute global indices stay valid on cold load. |

## LLVM native

- **`--emit-llvm [-o out.ll]`:** lowers expanded forms through post-opt IR and emits valid LLVM IR text.
- **Constant-exit MVP lowering:** when the final top-level form is a fixnum expression (including simple `+`, `-`, `*` fixnum trees) or when `(define (main) <const-expr>)` can be proven constant, generated `@main` returns that constant as `i32`.
- **Runtime fallback:** for other programs, generated `@main` calls `@ch_rt_main` from `src/ch_rt_main.c` (built as `chaaya_rt` in CMake).
- **`compile` / `--native`:** emits `.ll` and links it with `src/ch_rt_main.c` using `clang`/`cc` (or `CHAAYA_LLVM_CC`).
- **Current scope:** lowering is intentionally partial; the native path is still MVP-oriented.

## WASM (wasm32-wasi)

`chaaya wasm` is now a build helper:

- If `scripts/build-wasm.sh` exists, it runs that script and returns its status.
- If the script is missing, it prints manual CMake/WASI instructions and exits successfully (no build attempted).
- It exits non-zero only when a build was attempted and failed.

```bash
# Preferred helper (auto-detects optional toolchain)
./scripts/build-wasm.sh [build-dir]

# Optional: point helper at a WASI SDK toolchain
CHAAYA_WASI_TOOLCHAIN=/path/to/wasi-sdk.cmake ./scripts/build-wasm.sh

# Manual host-stub build (when no WASI toolchain is configured)
cmake -S . -B build-wasm -DCHAAYA_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm -j --target chaaya-wasm

# Manual WASI build
cmake -S . -B build-wasi -DCHAAYA_WASM=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/wasi-sdk.cmake
cmake --build build-wasi -j --target chaaya-wasm
```

See also [`scripts/build-wasm.sh`](../../scripts/build-wasm.sh).

## LSP

- Diagnostics use the shared [diagnostics.md](diagnostics.md) funnel (`CH` codes, JSON Diagnostic shape).
- In-memory URI → text storage is updated on `didOpen`/`didChange` and used for symbol lookup requests.
- Completion combines built-in Scheme forms/procedures with document-local `define`/`define-syntax` names.
- Hover/definition/references currently resolve within the active document text only.

## Compile cache

- Format: `CHBC` magic, version 1, source/compiler hashes, serialized top-level functions.
- CLI: `chaaya cache status|clear`.
- Auto-cache on `chaaya file.scm` when the source does not mention `import`.
