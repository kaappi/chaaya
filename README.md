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
- REPL and file runner

## Build

Requires CMake 3.20+, a C17 compiler, and libc.

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
./build/chaaya              # REPL
./build/chaaya program.scm  # run a file
./build/chaaya --version
```

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
| 4 | `call/cc`, `dynamic-wind`, exceptions |
| 5 | Hygienic `syntax-rules` |
| 6 | R7RS libraries + portable `.sld` reuse from Kaappi |
| 7 | Full R7RS-small surface |
| 8+ | IR/opts, SRFIs, FFI, concurrency, tooling, LLVM, WASM, LSP, packages |

## Relationship to Kaappi

Chaaya reuses Kaappi as a behavioral reference and will vendor portable Scheme
libraries where licenses allow (MIT). Bytecode, IR, and ABI are intentionally
independent. Note that Kaappi’s R7RS `(scheme …)` libraries are built-in (not
`lib/scheme/*.sld`); portable `.sld` is primarily for SRFIs and user code.

## Developer docs

Contributor documentation lives in [docs/dev/](docs/dev/):

- [Architecture](docs/dev/architecture.md) — pipeline, values, GC, VM, layout

## License

MIT — see [LICENSE](LICENSE).
