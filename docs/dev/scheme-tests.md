# Scheme tests

Chaaya borrows Kaappi’s conformance posture: smoke → compliance → R7RS suite,
plus display probes for optimizer-sensitive shapes. Bootstrap covers macros and
an R7RS library MVP; fuller Kaappi/R7RS suites stay deferred.

## Layout

| Path | Role | In `make test`? |
|------|------|-----------------|
| `tests/scheme/*.scm` + `*.expected` | Early smoke (fact, lists, …) | yes |
| `tests/scheme/bootstrap/` | Kaappi-shaped `check-*` suites (`libraries.scm` uses `import`) | yes |
| `tests/scheme/probes/` | Adapted Kaappi differential probes (stdout golden) | yes |
| `tests/scheme/r7rs/` | Canonical R7RS-small suite (vendored) | **in CTest** (`r7rs_suite`; full file green) |
| `tests/scheme/kaappi-deferred/` | Full Kaappi smoke/compliance/probe copies | **partial** — 30 compliance + 16 smoke files in CTest (see below) |

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
8. SRFI-64 or `(chibi test)` — **done** for wired deferred suites (`bind_lib_ref` skips transformers; per-env hoist globals). `(chibi test)` drives `r7rs_suite`.
9. Re-run Kaappi’s `tests/scheme/run-all.sh` corpus against `chaaya` — **in progress**
   (compliance: 30 files; smoke batch 1: 16 files — see `CMakeLists.txt`)

## kaappi-deferred compliance

Wired in CTest as `kaappi_deferred_*` (numeric/reader, unicode/strings, R7RS gaps,
time/process-context, bugfix sweeps). Enable with:

```bash
ctest --output-on-failure -R kaappi_deferred -E smoke
```

**Recently added (batch 1B–1E, green):** `r7rs-datatypes-gaps`, `time`,
`process-context`, `scheme-repl`, `include-lib-decls`, `correctness-fixes`,
`final-gaps`, `bugfixes`, `deferred-bugfixes`, `deferred-final`.

**Deferred (not wired — known blockers):**

| File | Reason |
|------|--------|
| `reader-exactness-gaps.scm` | SIGABRT on section 10 round-trip matrix (sections 1–9 pass) |
| `printer-gaps.scm` | hangs on cyclic `write-simple` (SRFI 258 import OK) |
| `reader-port-refill-gaps.scm` | port buffer refill at split boundaries (22 failures) |
| `r7rs-expressions-gaps.scm` | literal identifier binding at use site (29/30) |
| `r7rs-thin-forms-gaps.scm`, `r7rs-tail-position-gaps.scm`, `r7rs-tail-procedures-gaps.scm`, `r7rs-import-macro-gaps.scm`, `r7rs-libraries-gaps.scm` | SIGABRT during run |
| `r7rs-control-io-gaps.scm` | immutable environment / `eval` (4 failures) |
| `r7rs-continuation-gaps.scm` | MV through continuations; `guard`/`raise` (2 failures) |
| `r7rs-hygiene-gaps.scm` | ellipsis distribution in `syntax-rules` (2 failures) |
| `record-ctor-clause-keyword-1882.scm` | R6RS clause-syntax records NYI |
| `bugfixes.scm` | missing `hash-table-merge!` |
| `deferred-bugfixes.scm` | missing `open-binary-input-file` |
| `deferred-final.scm` | missing `open-binary-output-file` |
| `srfi-completeness.scm` | multiple missing native SRFI libraries |

## kaappi-deferred smoke (batch 1)

Wired in CTest as `kaappi_deferred_smoke_*` (themes: core, macros, expander, GC,
bignum). Enable with:

```bash
ctest --output-on-failure -R kaappi_deferred_smoke
```

**Green (16):** `basic`, `numeric`, `macros`, `expander-fixes`,
`expander-many-patvars`, `gc-mark-contents-types`, `bignum-rational-arith`,
`bignum-division-multi-arg`, `bignum-rational-normalization`, `not-not-non-boolean`,
`zero-pred-type-error`, `literal-immutability`, `apply-shadowing`,
`call-with-values-arity`, `binding-form-validation`, `memv-assv-numeric`.

**Skipped in this batch (not green or Chaaya-deferred):**

| File | Reason |
|------|--------|
| `jit-*.scm`, `llvm-*.scm` | JIT/LLVM backend not in Chaaya — do not wire |
| `circular-list-terminate.scm` | hangs (timeout) — needs printer/GC fix |
| `equal-dag.scm` | `equal?` on DAGs |
| `case-lambda-fixes.scm` | case-lambda edge cases |
| `expt-negative-base-1725.scm` | negative-base fractional/complex `expt` |

Add the next batch (ports, fibers, expander) only after local green + `make test`.
