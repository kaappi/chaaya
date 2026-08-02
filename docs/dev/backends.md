# Backends (MVP)

This note documents the current backend integration surface for Phase 11.

## Backend status

| Backend | Entry point | Current behavior |
|---------|-------------|------------------|
| Bytecode VM | default `chaaya` run path | Implemented and primary execution path |
| LLVM native | `chaaya compile` / `--native` / `--emit-llvm` | Emits LLVM IR text from post-opt IR; links a tiny `ch_rt_main` stub via `cc` |
| WASM | CMake option `CHAAYA_WASM` | Optional target; WASI toolchain builds interpreter; host builds stub |
| LSP | stdin/stdout JSON-RPC | `initialize`, `didOpen`/`didChange` → `publishDiagnostics`, document symbols, definition stub |
| Compile cache | `$CHAAYA_HOME/cache/*.chbc` | Auto read/write on plain file runs (skipped when source mentions `import`, or when `CHAAYA_NO_CACHE` is set). Blob includes bytecode plus the global name table so absolute global indices stay valid on cold load. |

## LLVM native

- **`--emit-llvm [-o out.ll]`:** lower expand → IR → comments-as-IR MVP module with `main` calling `@ch_rt_main`.
- **`compile` / `--native`:** emit IR to a temp file, compile a stub C runtime with `cc`, optionally run it.
- **Next:** real LLVM IR for fixnums/calls, `libchaaya_rt` C-ABI bridge, aarch64/x86_64 only.

## WASM (wasm32-wasi)

```bash
# Host stub (default when CHAAYA_WASM=ON without WASI toolchain)
cmake -S . -B build-wasm -DCHAAYA_WASM=ON
cmake --build build-wasm -j --target chaaya-wasm

# Real WASI build (requires WASI SDK / wasi-sdk toolchain file)
cmake -S . -B build-wasi -DCHAAYA_WASM=ON -DCMAKE_SYSTEM_NAME=WASI \
  -DCMAKE_C_COMPILER=clang --toolchain path/to/wasi-sdk.cmake
```

See also [`scripts/build-wasm.sh`](../../scripts/build-wasm.sh).

## LSP

- Diagnostics use the shared [diagnostics.md](diagnostics.md) funnel (`CH` codes, JSON Diagnostic shape).
- Next: completion, hover, references.

## Compile cache

- Format: `CHBC` magic, version 1, source/compiler hashes, serialized top-level functions.
- CLI: `chaaya cache status|clear`.
- Auto-cache on `chaaya file.scm` when the source does not mention `import`.
