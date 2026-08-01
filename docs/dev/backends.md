# Backends (MVP)

This note documents the current backend integration surface for Phase 11 MVP.

## Backend status

| Backend | Entry point | Current behavior |
|---------|-------------|------------------|
| Bytecode VM | default `chaaya` run path | Implemented and primary execution path |
| LLVM native | `chaaya --native <file.scm>` | Stubbed in [`src/llvm_backend.c`](../../src/llvm_backend.c): returns an explicit not-implemented diagnostic |
| WASM | CMake option `CHAAYA_WASM` | Optional target scaffold; on non-WASI toolchains builds a stub binary that prints NYI |

## LLVM native stub

- CLI switch: `--native`
- Current dispatch: handled by [`src/cli.c`](../../src/cli.c), delegated to
  [`src/llvm_backend.c`](../../src/llvm_backend.c)
- MVP contract: fail honestly with a clear message instead of pretending to
  compile/execute native code.

## WASM target scaffold

Enable with CMake:

```bash
cmake -S . -B build -DCHAAYA_WASM=ON
cmake --build build -j
```

Behavior:

- `CMAKE_SYSTEM_NAME=WASI`: builds `chaaya-wasm` from the normal runtime entry
  (`src/main.c` + `chaaya_core`).
- Any other toolchain: builds `chaaya-wasm` from
  [`src/wasm_stub.c`](../../src/wasm_stub.c), which prints an NYI message and
  exits non-zero.

This keeps the build graph ready for wasm32-wasi without claiming interpreter
parity on host toolchains that are not configured for WASI.
