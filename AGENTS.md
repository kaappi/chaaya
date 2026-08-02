# Chaaya — R7RS Scheme in C

Bootstrap R7RS Scheme interpreter (C23, CMake). Inspired by
[Kaappi](https://github.com/kaappi/kaappi) as a behavioral reference — not a
line-by-line port. Bytecode, IR, and ABI are independent.

Current release: **v0.1.0** (see `include/chaaya/version.h`).

## Build

Requires CMake 3.21+, a C23-capable compiler, and libc. POSIX REPL uses vendored
[linenoise](third_party/linenoise/).

```bash
make            # configure + build → build/chaaya
make test       # build + ctest (329 tests)
make bootstrap-scheme   # Kaappi-shaped bootstrap suites (17 files)
make run        # REPL
```

Or with CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

```bash
./build/chaaya program.scm
./build/chaaya --lib-path ./lib program.scm
./build/chaaya features
./build/chaaya doctor
./build/chaaya ast program.scm
```

Environment: `CHAAYA_HOME` (default `~/.chaaya`) for REPL history.

## Architecture

```text
Source → Reader → Expander → Compiler → VM → GC
```

| Stage | Key files |
|-------|-----------|
| Reader | `src/reader.c`, `src/reader_number.c`, `include/chaaya/reader.h` |
| Expander | `src/expander_syntax_rules.c`, `src/expander_toplevel.c` — `syntax-rules`, hygiene |
| Compiler | `src/compiler.c`, `src/compiler_bind.c`, `src/compiler_control.c` — AST → register bytecode |
| VM | `src/vm.c`, `include/chaaya/opcode.h` |
| GC / values | `src/gc.c`, `src/value.c` — NaN-boxed `ChValue` |
| Primitives | `src/prim_*.c` — one file per domain |
| Libraries | `src/library.c`, `src/library_builtin.c` — R7RS `(scheme …)` exports |
| Numeric tower | `src/bignum.c`, `src/rational.c`, `src/complex.c` |
| CLI / REPL | `src/cli.c`, `src/cli_parse.c`, `src/cli_cmds.c`, `src/repl.c`, `src/main.c` |

Full detail: [docs/dev/architecture.md](docs/dev/architecture.md). C23 policy and
features: [docs/dev/c23.md](docs/dev/c23.md).

## Source layout

```text
include/chaaya/   Public headers (ch_ / Ch prefixes)
src/              One concern per .c file
tests/c/          CTest unit tests
tests/scheme/     Scheme smoke, bootstrap, probes
lib/              Portable .sld trees (via --lib-path)
docs/dev/         Contributor docs
third_party/      Vendored deps (linenoise)
```

Logic `src/*.c` files: soft limit ~1500 lines, hard limit 1800. Split along
domain seams before crossing the hard limit. Exempt generated data (e.g.
`unicode_tables.c`) and `third_party/`.

## Adding primitives

1. Implement `static ChValue prim_foo(ChVM *vm, ChValue *args, int nargs)` in the
   appropriate `src/prim_*.c` (or add a new file and list it in `CMakeLists.txt`).
2. Register in that file's `ch_register_*_primitives()` via `define_prim`.
3. Call the registrar from `ch_vm_register_primitives()` in `src/vm.c`.
4. Declare the registrar in `include/chaaya/prim.h`.

Arity: pass `-1` for variadic rest args; `min_arity` is the minimum required.

On error, set `vm->error` and return `CH_UNDEFINED`.

## Adding R7RS libraries

Built-in `(scheme …)` libraries are registered in
`ch_register_builtin_libraries()` in `src/library_builtin.c`. Each library lists export
names; bindings are resolved from VM globals (soft-skip if a name is missing).

`(scheme base)` auto-exports all defined globals except `%`-prefixed internals.

Portable user libraries live under `lib/` and load via `--lib-path`.

## GC rule (critical)

Any `ChValue` that must survive a `ch_gc_*` allocation must be on the root stack
(`ch_gc_push` / `ch_gc_pop`) before the allocation runs. The collector is
stop-the-world mark-and-sweep with no write barrier.

## Testing

| Suite | Command | Notes |
|-------|---------|-------|
| C + Scheme (CTest) | `make test` | 329 tests; CI runs this |
| Bootstrap | `make bootstrap-scheme` | Uses `run-bootstrap.sh`; needs suite-dir cwd for `include` fixtures |
| Single bootstrap file | See `tests/scheme/run_bootstrap.cmake` | CTest prepends `harness.scm` |

Bootstrap harness (`tests/scheme/bootstrap/harness.scm`): `check-equal`,
`check-eqv`, `check-assert`, `check-finish`.

When adding surface area, extend the relevant bootstrap suite
(`numbers.scm`, `libraries.scm`, etc.) — see
[docs/dev/scheme-tests.md](docs/dev/scheme-tests.md).

`tests/scheme/r7rs/` (canonical R7RS-small suite) and most of
`tests/scheme/kaappi-deferred/` (compliance + smoke) are wired into CTest —
see [docs/dev/scheme-tests.md](docs/dev/scheme-tests.md) for current
wired/unwired counts. `jit-*.scm`/`llvm-*.scm` smoke files stay permanently
unwired (no JIT/LLVM backend in Chaaya); a smaller remainder is gated on
fiber/thread scheduler races, macro-expander depth/hygiene gaps, and missing
SRFI-170 POSIX primitives (tracked in that doc, not in CTest).

## Conventions

- **C**: C23 (`CMAKE_C_STANDARD 23`), `-Wall -Wextra`; warnings-as-errors on by default (`CHAAYA_WARNINGS_AS_ERRORS`). When writing C, prefer C23 features over C17/C11 equivalents where they improve clarity or safety — see [docs/dev/c23.md](docs/dev/c23.md).
- **Naming**: `ch_` for functions, `Ch` for types, `CH_` for constants/macros.
- **Scheme tests**: 2-space indentation, R7RS style.
- **Commits**: short imperative subject; body explains *why*.
- **Docs**: contributor material in `docs/dev/`; keep paths and file references current.

## Relationship to Kaappi

Use Kaappi for behavioral and architectural reference (CLI shape, test harness,
library naming, control-flow patterns). Do **not** assume bytecode, IR, or ABI
compatibility. Kaappi's built-in `(scheme …)` libraries are C/Zig natives; Chaaya
registers them similarly. Portable `.sld` files are for SRFIs and user code under
`lib/`.

### Package manager (thottam)

**Do not re-implement thottam in Chaaya.** When ecosystem packaging is needed,
use Kaappi's existing **thottam** implementation (Zig) in the sibling repo
[`../kaappi/`](../kaappi/) — sources under `src/thottam*.zig` (e.g.
`thottam.zig`, `thottam_proc.zig`, `thottam_fs.zig`). Build with `zig build`
in that repo; the `thottam` binary handles `kaappi.pkg` manifests, install paths,
lockfiles, and native `build:` steps for ecosystem libraries. Chaaya only needs
`--lib-path` (and compatible `.sld` / `.scm` layouts) to consume what thottam
installs; wiring Chaaya into thottam's toolchain is a separate integration task,
not a greenfield package manager in C.

## Roadmap (phase table)

| Phase | Status |
|------:|--------|
| 0–6 | Scaffold through R7RS libraries — **done** |
| 7A | R7RS-small surface (eval, exceptions, bytevectors, hashtables, base audit) — **done** |
| 7B | Numeric tower + I/O hardening — **done** |
| 8 | IR/opts + generational GC + CLI tooling (`check`, `expand`, `fmt`, cache, REPL debugger MVP) — **MVP done** |
| 9 | Portable SRFI subset + import resolution — **partial** (~192 vendored under `lib/srfi/`; see `docs/dev/srfi-import-audit.md`) |
| 10 | Fibers + FFI + reactor — **MVP done** (cooperative fibers/channels, dlopen FFI; SRFI-18 partial (timeouts, specifics, terminate; mutex/cond MVP)) |
| 11 | LLVM/WASM/LSP/thottam — **MVP done** (stubs + docs; no C thottam reimplementation) |

See [README.md](README.md) for the full phase table.

## Agent guidelines

**Do**

- Run `make test` before finishing; add bootstrap checks for new surface.
- Read surrounding code and match existing patterns before adding primitives or compiler support.
- Keep diffs focused — one concern per change when possible.
- Consult `docs/dev/` before guessing at CLI, REPL, or test conventions.
- Prefer **C23 standard features** over C17 workarounds or compiler extensions when touching C code; follow [docs/dev/c23.md](docs/dev/c23.md).
- Split logic sources before they exceed 1800 lines (soft target ~1500).

**Don't**

- Port Kaappi Zig sources line-for-line; redesign for C is intentional.
- Add `%`-prefixed names to `(scheme base)` exports (internals only).
- Skip GC root pushes around allocations that can trigger collection.
- Enable deferred R7RS/Kaappi suites without checking the gate list in
  `docs/dev/scheme-tests.md`.
- Expand scope into IR, FFI, fibers, or LLVM unless explicitly requested.
- Re-implement **thottam** or a Chaaya-native package manager — use Kaappi's thottam in `../kaappi/` instead (see above).
- Introduce C17-only patterns or non-standard extensions when an equivalent C23 feature exists (see [docs/dev/c23.md](docs/dev/c23.md)).
