# Backends (MVP)

This note documents the current backend integration surface for Phase 11 MVP.

## Backend status

| Backend | Entry point | Current behavior |
|---------|-------------|------------------|
| Bytecode VM | default `chaaya` run path | Implemented and primary execution path |
| LLVM native | `chaaya --native <file.scm>` | Stubbed in [`src/llvm_backend.c`](../../src/llvm_backend.c): returns an explicit not-implemented diagnostic |
| WASM | CMake option `CHAAYA_WASM` | Optional target scaffold; on non-WASI toolchains builds a stub binary that prints NYI |
| LSP | stdin/stdout JSON-RPC | MVP in [`src/lsp.c`](../../src/lsp.c): `initialize` / `shutdown` only |
| Compile cache | [`include/chaaya/cache.h`](../../include/chaaya/cache.h) | Header + CLI hooks; no persistent cache yet |

## Phase 11+ backlog

Work tracked here intentionally stays **documentation and stubs** until a dedicated track lands implementation.

### LLVM native

- **Today:** `--native` fails with a clear NYI message ([`src/llvm_backend.c`](../../src/llvm_backend.c)).
- **Scope when implemented:** lower Chaaya IR (post-opts) to LLVM IR for `aarch64` / `x86_64`, link against a small C-ABI runtime exporting GC + procedure call helpers.
- **Out of scope for first cut:** full separate compilation of user `.sld` trees, cross-compilation matrix beyond macOS/Linux host dev.

### WASM (wasm32-wasi)

- **Today:** `CHAAYA_WASM=ON` builds `chaaya-wasm`; non-WASI toolchains link [`src/wasm_stub.c`](../../src/wasm_stub.c).
- **Next:** WASI SDK CI job, `wasi_snapshot_preview1` I/O for `scheme.base` ports, release artifact alongside Kaappi playground updates.
- **Deferred:** browser JS glue (Kaappi.github.io owns playground integration).

### LSP

- **Today:** enough for CTest `cli_lsp_initialize_shutdown`.
- **Next:** `textDocument/didOpen`, publish diagnostics from expand/check, symbol at point for built-in libraries.

### Compile cache

- **Today:** [`include/chaaya/cache.h`](../../include/chaaya/cache.h) defines keying hooks; CLI accepts cache flags but does not persist bytecode.
- **Next:** content-addressed keys (source + lib-path + feature set), atomic write under `$CHAAYA_HOME/cache`.

### thottam / packages

- **Policy:** do **not** re-implement thottam in C — use Kaappi's Zig thottam in the sibling [`kaappi`](../../kaappi) repo ([`docs/dev/thottam.md`](thottam.md)).
- **Chaaya integration milestones:** document `--lib-path` layout for thottam installs; optional [`scripts/thottam-smoke.sh`](../../scripts/thottam-smoke.sh) as a SKIP-friendly CI probe.

### kaappi-deferred compliance in CTest

- Runner: [`tests/scheme/run_kaappi_deferred.cmake`](../scheme/run_kaappi_deferred.cmake) (mirrors R7RS runner).
- **Tier 1 enabled (CTest, green):** `sqrt-exact`, `lists`, `vectors`
- **Tier 2 enabled (CTest, green):** `macro-export-scope`, `chars`, `bytevectors`
- **Tier 2 blocked (local):** `eval` (eval arity), `lazy` (reentrant `force`)
- **Next engine work:** nested quasiquote (`,,`) segfault in full `r7rs-tests.scm` (~line 354); SRFI-64 `test-equal` / `guard` interaction in deferred compliance.

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
