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

Requires CMake 3.20+, a C17 compiler, and libc. On POSIX, the interactive REPL
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
| 7 | Full R7RS-small surface (**in progress**: numeric tower + math libs; more surface next) |
| 8+ | IR/opts, SRFIs, FFI, concurrency, tooling, LLVM, WASM, LSP, packages |

Language-parity target (≈99% Kaappi language surface): R7RS-small + portable
SRFIs. Deferred for later: fibers, FFI, LLVM, WASM, LSP, thottam.

## Relationship to Kaappi

Chaaya reuses Kaappi as a behavioral reference and will vendor portable Scheme
libraries where licenses allow (MIT). Bytecode, IR, and ABI are intentionally
independent. Note that Kaappi’s R7RS `(scheme …)` libraries are built-in (not
`lib/scheme/*.sld`); portable `.sld` is primarily for SRFIs and user code.

## Developer docs

Contributor documentation lives in [docs/dev/](docs/dev/):

- [Architecture](docs/dev/architecture.md) — pipeline, values, GC, VM, layout
- [CLI](docs/dev/cli.md) — help shape, exit codes, stubs
- [REPL](docs/dev/repl.md) — prompts, comma-commands, history
- [Scheme tests](docs/dev/scheme-tests.md) — bootstrap suites, probes, deferred Kaappi/R7RS

## License

MIT — see [LICENSE](LICENSE).
