# Scheme tests

Chaaya borrows Kaappi’s conformance posture: smoke → compliance → R7RS suite,
plus display probes for optimizer-sensitive shapes. Bootstrap covers macros and
an R7RS library MVP; fuller Kaappi/R7RS suites stay deferred where noted.

## Layout

| Path | Role | In `make test`? |
|------|------|-----------------|
| `tests/scheme/*.scm` + `*.expected` | Early smoke (fact, lists, …) | yes |
| `tests/scheme/bootstrap/` | Kaappi-shaped `check-*` suites (`libraries.scm` uses `import`) | yes |
| `tests/scheme/probes/` | Adapted Kaappi differential probes (stdout golden) | yes |
| `tests/scheme/r7rs/` | Canonical R7RS-small suite (vendored) | **in CTest** (`r7rs_suite`; full file green) |
| `tests/scheme/kaappi-deferred/` | Full Kaappi smoke/compliance/probe copies | **partial** — see wired lists below |

## Bootstrap harness

`bootstrap/harness.scm` is prepended to each suite by
`run_bootstrap.cmake` / `run-bootstrap.sh`. Naming mirrors SRFI-64 / Kaappi:

- `check-equal` / `check-eqv` / `check-eq` / `check-assert`
- `check-finish` → `(exit 0)` or `(exit 1)`

```bash
bash tests/scheme/run-bootstrap.sh ./build/chaaya
```

## Probes

Probe files print values with `display`/`write`/`newline`. CTest compares
stdout to `*.expected` (exact match), same as the early smoke files.

## Libraries bootstrap

`bootstrap/libraries.scm` exercises inline `define-library`, `(import (scheme
base))`, and on-disk `.sld` under `lib/` (CTest passes `--lib-path`).

## Enabling deferred suites

See `tests/scheme/kaappi-deferred/README.md`. Rough gates:

1. Hygienic macros (`syntax-rules`) — **done**
2. R7RS libraries + import modifiers + include/cond-expand — **done**
3. `case-lambda`, `delay`/`force`, file ports — **done** (bootstrap suites)
4. Exact integers / bignum — **done** (MVP; demote to fixnum)
5. Rationals + exact `/` — **done** (MVP)
6. Complexes (inexact rectangular) — **done** (MVP)
7. `(scheme inexact)` / `(scheme exact)` + math prims — **done** (MVP)
8. SRFI-64 or `(chibi test)` — **done** for wired deferred suites
9. Re-run Kaappi’s `tests/scheme/run-all.sh` corpus against `chaaya` — **in progress**

## kaappi-deferred compliance

Wired in CTest as `kaappi_deferred_*` (see `CMakeLists.txt`). Enable with:

```bash
ctest --output-on-failure -R kaappi_deferred -E smoke
```

**Wired (language-parity track, green):** includes
`include-lib-decls`, `r7rs-import-macro-gaps`, `r7rs-libraries-gaps`,
`r7rs-control-io-gaps`, `reader-port-refill-gaps`, `r7rs-expressions-gaps`,
`r7rs-hygiene-gaps`, `r7rs-continuation-gaps`, `record-ctor-clause-keyword-1882`,
plus earlier numeric/reader/unicode/string/datatype/time suites.

**Deferred (not wired — known blockers):**

| File | Reason |
|------|--------|
| `r7rs-tail-procedures-gaps.scm` | `let-values` TCO at N=40000 (register growth via `call-with-values`) |
| `printer-gaps.scm` | 300s+ `write-shared` scan — too slow for default CTest |

## kaappi-deferred smoke

Wired in CTest as `kaappi_deferred_smoke_*`. Enable with:

```bash
ctest --output-on-failure -R kaappi_deferred_smoke
```

**Recently wired (language surface):** `case-lambda-fixes`,
`expt-negative-base-1725`, `equal-dag`, `circular-list-terminate`.

**Skipped (do not wire):**

| File | Reason |
|------|--------|
| `jit-*.scm`, `llvm-*.scm` | JIT/LLVM backend not in Chaaya |

Add further smoke batches only after local green + `make test`.
