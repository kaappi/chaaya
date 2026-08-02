# Chaaya

Chaaya is an R7RS Scheme implementation written in C. It is a free redesign
inspired by [Kaappi](https://github.com/kaappi/kaappi) (Zig), not a line-by-line
port. Long-term goals match Kaappi’s breadth: R7RS-small, SRFIs, FFI, fibers,
LLVM native backend, WASM, LSP, and packaging — delivered in phases.

## Status (v0.1.0)

Bootstrap interpreter:

- NaN-boxed values + mark-sweep GC
- Datum reader / printer
- Register bytecode VM
- Special forms: `quote`, `if`, `lambda`, `define`, `set!`, `begin`, `and`, `or`, `let`
- Core primitives (lists, arithmetic, equality, I/O)
- Control: `call/cc`, `dynamic-wind`, `with-exception-handler`, `raise` / `raise-continuable`, `error`
- Lists/`apply`/`map`/`for-each`, strings, vectors, `values`/`call-with-values`
- Ports: stdio + string ports; `read` / `display` / `write` take optional ports
- Kaappi-shaped CLI (`features`, `doctor`, `ast`, …) and linenoise REPL
  (`chaaya>`, multiline, `,help`, `_`)

## Build

Requires CMake 3.21+, a C23-capable compiler, and libc. On POSIX, the interactive REPL
uses vendored [linenoise](third_party/linenoise/) (arrow keys, history).

```bash
make            # configure + build → build/chaaya
make test       # build + ctest
make run        # REPL
```

Or with CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

```bash
./build/chaaya                 # REPL
./build/chaaya program.scm     # run a file
./build/chaaya --version       # Chaaya Scheme v0.1.0
./build/chaaya features
./build/chaaya doctor
./build/chaaya ast program.scm
```

REPL history is stored in `~/.chaaya/history` (or `$CHAAYA_HOME/history`).
See [docs/dev/cli.md](docs/dev/cli.md) and [docs/dev/repl.md](docs/dev/repl.md).

## Example

```scheme
(define (fact n)
  (if (= n 0)
      1
      (* n (fact (- n 1)))))
(fact 10)
```

## Roadmap

| Phase | Focus |
|------:|-------|
| 0–3 | Scaffold, values/GC, reader, bytecode REPL (**done**) |
| 4 | `call/cc`, `dynamic-wind`, exceptions (**done**) |
| 5 | Hygienic `syntax-rules` + derived forms (**done**) |
| 6 | R7RS libraries + portable `.sld` reuse from Kaappi (**done**) |
| 7 | Full R7RS-small surface (**R7RS CTest green**; kaappi-deferred compliance/smoke batches in progress) |
| 8 | IR/opts, generational GC, CLI tooling (`check`, `expand`, `fmt`, cache) (**MVP done**) |
| 9 | Portable SRFI subset + import resolution (**partial**: ~192 vendored under `lib/srfi/`) |
| 10 | Fibers + FFI + reactor (**MVP done**; SRFI-18 threads NYI) |
| 11 | LLVM / WASM / LSP / thottam integration (**stubs + docs**; see backlog below) |

### Phase 11+ backlog (not yet implemented)

These items are tracked for future work; current tree ships honest stubs only.

| Item | Current state | Next milestones |
|------|---------------|-----------------|
| **LLVM native** | `chaaya --native` → NYI in [`src/llvm_backend.c`](src/llvm_backend.c) | IR → LLVM IR lowering, C ABI runtime bridge, object link |
| **WASM** | `CHAAYA_WASM` CMake target; host builds use [`src/wasm_stub.c`](src/wasm_stub.c) | wasm32-wasi CI matrix, WASI syscall shims, browser playground binary |
| **LSP** | [`src/lsp.c`](src/lsp.c) MVP (initialize/shutdown) | diagnostics, go-to-def, document symbols |
| **Compile cache** | [`include/chaaya/cache.h`](include/chaaya/cache.h) API stub | `.chaaya/cache` layout, invalidation, CLI `--cache` wiring |
| **thottam** | [`docs/dev/thottam.md`](docs/dev/thottam.md); use Kaappi's Zig thottam, not a C reimplementation | Chaaya `--lib-path` consumer docs, optional `scripts/thottam-smoke.sh` in CI |
| **R7RS CTest gate** | [`tests/scheme/run_r7rs.cmake`](tests/scheme/run_r7rs.cmake) — **green** | Expand [`tests/scheme/kaappi-deferred/`](tests/scheme/kaappi-deferred/) smoke/compliance batches |

Language-parity target (≈99% Kaappi language surface): R7RS-small + portable
SRFIs. Fibers and FFI are **MVP done**; LLVM, WASM, LSP, and thottam integration
remain deferred (see Phase 11).

## Relationship to Kaappi

Chaaya reuses Kaappi as a behavioral reference and will vendor portable Scheme
libraries where licenses allow (MIT). Bytecode, IR, and ABI are intentionally
independent. Note that Kaappi’s R7RS `(scheme …)` libraries are built-in (not
`lib/scheme/*.sld`); portable `.sld` is primarily for SRFIs and user code.

## Developer docs

Contributor documentation lives in [docs/dev/](docs/dev/):

- [Architecture](docs/dev/architecture.md) — pipeline, values, GC, VM, layout
- [Backends](docs/dev/backends.md) — LLVM `--native` + WASM target stub status
- [C23](docs/dev/c23.md) — language standard, C23 vs C17 features to leverage
- [CLI](docs/dev/cli.md) — help shape, exit codes, stubs
- [REPL](docs/dev/repl.md) — prompts, comma-commands, history
- [Scheme tests](docs/dev/scheme-tests.md) — bootstrap suites, probes, deferred Kaappi/R7RS
- [Thottam](docs/dev/thottam.md) — using sibling Kaappi thottam with Chaaya

## License

MIT — see [LICENSE](LICENSE).
