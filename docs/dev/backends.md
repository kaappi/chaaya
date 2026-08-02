# Backends (MVP)

This note documents the current backend integration surface for Phase 11.

## Backend status

| Backend | Entry point | Current behavior |
|---------|-------------|------------------|
| Bytecode VM | default `chaaya` run path | Implemented and primary execution path |
| LLVM native | `chaaya compile` / `--native` / `--emit-llvm` | Real `.ll` emission; constant exits, native prim lowering, or eval-fallback via `libchaaya_rt.a` |
| WASM | `chaaya wasm` / CMake option `CHAAYA_WASM` | Build helper + WASI/host stub; FFI/threads gated under `__wasi__` |
| LSP | stdin/stdout JSON-RPC | Diagnostics, document symbols, completion, hover, definition, references |
| Compile cache | `$CHAAYA_HOME/cache/*.chbc` | Auto read/write on plain file runs (skipped when source mentions `import`, or when `CHAAYA_NO_CACHE` is set) |

## LLVM native

- **`--emit-llvm [-o out.ll]`:** lower expand → IR → optimize → emit LLVM IR text.
- **Constant-exit path:** fixnum trees / proven `(define (main) …)` → `@main` returns `i32` constant (no runtime link).
- **Native prim path:** emittable IR (`PRIM_CALL`, `IF`, `SEQ`, simple `DEFINE`) calls `ch_rt_*` helpers.
- **Eval fallback:** other programs embed source and call `ch_rt_eval` after `ch_rt_init`.
- **Runtime:** [`src/runtime_exports.c`](../../src/runtime_exports.c) → `libchaaya_rt.a` (CMake target `chaaya_rt`). Discover via `CHAAYA_LIB_DIR` or the build directory.
- **Gates:** aarch64/x86_64 only; refuses sources containing `import`.
- **Doctor:** smoke-links a tiny C program against `ch_rt_fixnum_add`.

## WASM (wasm32-wasi)

```bash
./scripts/build-wasm.sh [build-dir]
# Optional WASI SDK:
CHAAYA_WASI_TOOLCHAIN=/path/to/wasi-sdk.cmake ./scripts/build-wasm.sh
```

Under `__wasi__`, FFI/`dlopen` and OS threads are unavailable; `wasm32`/`wasi` cond-expand features are registered. Cooperative fibers remain.

## LSP

- Shared diagnostics funnel ([diagnostics.md](diagnostics.md)).
- Per-URI document store; symbols/definition/references from buffer text.
- Completion/hover from builtins + document defines.

## Compile cache

- Format: `CHBC` magic, source/compiler hashes, serialized top-level functions + global name table.
- CLI: `chaaya cache status|clear`.
