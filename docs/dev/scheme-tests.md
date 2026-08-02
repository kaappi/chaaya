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
| `tests/scheme/kaappi-deferred/` | Full Kaappi smoke/compliance/probe copies | **partial** — 37 compliance + 62 smoke files in CTest (see below) |

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
   (compliance: 37 files; smoke: 62 files — see `CMakeLists.txt`)

## kaappi-deferred compliance

Wired in CTest as `kaappi_deferred_*` (numeric/reader, unicode/strings, R7RS gaps,
time/process-context, bugfix sweeps). Enable with:

```bash
ctest --output-on-failure -R kaappi_deferred -E smoke
```

**Recently added (batch 1B–1E + 2, green):** `r7rs-datatypes-gaps`, `time`,
`process-context`, `scheme-repl`, `correctness-fixes`, `final-gaps`, `bugfixes`,
`deferred-bugfixes`, `deferred-final`, `r7rs-thin-forms-gaps`, `printer-gaps`,
`r7rs-tail-position-gaps`, `r7rs-import-macro-gaps`, `r7rs-libraries-gaps`,
`reader-exactness-gaps`, `srfi-completeness`.

**Deferred (not wired — known blockers):**

| File | Reason |
|------|--------|
| `reader-port-refill-gaps.scm` | port buffer refill at split boundaries (22 failures) |
| `r7rs-expressions-gaps.scm` | literal identifier binding at use site (29/30) |
| `r7rs-tail-procedures-gaps.scm` | let-values TCO at N=40000 (register growth via `call-with-values`) |
| `r7rs-control-io-gaps.scm` | immutable environment / `eval` (4 failures) |
| `r7rs-continuation-gaps.scm` | MV through continuations; `guard`/`raise` (2 failures) |
| `r7rs-hygiene-gaps.scm` | ellipsis distribution in `syntax-rules` (2 failures) |
| `record-ctor-clause-keyword-1882.scm` | R6RS clause-syntax records NYI |
| `include-lib-decls.scm` | library declaration edge cases (in progress) |

## kaappi-deferred smoke

Wired in CTest as `kaappi_deferred_smoke_*` (themes: core, macros, expander, GC,
bignum, fibers, ports). Enable with:

```bash
ctest --output-on-failure -R kaappi_deferred_smoke
```

**Green (62):** batch 1 — `basic`, `numeric`, `macros`, `expander-fixes`,
`expander-many-patvars`, `gc-mark-contents-types`, `bignum-rational-arith`,
`bignum-division-multi-arg`, `bignum-rational-normalization`, `not-not-non-boolean`,
`zero-pred-type-error`, `literal-immutability`, `apply-shadowing`,
`call-with-values-arity`, `binding-form-validation`, `memv-assv-numeric`,
`char-folding-fixes`, `expt-rational`, `tail-calls`; batch 2 —
`bignum-expt-gc`, `closure-upvalue-gc`, `fiber-channel-gc`, `promise-gc`,
`mutation-write-barrier`, `upvalue-write-barrier`, `ellipsis-mismatch`,
`import-composed`, `bytevector-port-fixes`, `guard-continuable-845`,
`arith-overflow`, `bare-lambda-self-tail-call`, `bignum-gc-alias-1414`,
`callwithargs-bounds`, `case-arrow-register`, `continuation-wind-870`,
`define-values-gc`, `dynamic-wind-double-875`, `exact-inexact-842-848`,
`fiber-round-robin`, `hash-table-walk-rehash`, `inexact-to-exact`,
`rational-rounding`, `string-trim-whitespace-826`; batch 3 —
`bignum-toF64-833`, `call-global-continuation`, `case-empty-datum-854`,
`command-line-o-flag`, `complex-accessors-types`, `cond-expand-empty`,
`constant-fold-shadowing`, `continuation-gc-size`, `create-temp-file-error`,
`datum-label-vector`, `deep-mark-864`, `define-values-lambda-body`, `derived`,
`do-closure-capture-803`, `dotted-pair-datum-comment`, `ellipsis-depth-mismatch`,
`exact-integer-sqrt-export`, `exact-large-float`, `exactness-prefix`.

**Batch triage (239 unwired scanned):** ~123 pass locally with 15s timeout;
116 fail/hang (SRFI-18 threads NYI, missing primitives, fixture libs, 3 hangs).
See `/tmp/chaaya-smoke-final.txt` from batch run for per-file reasons.

**Skipped in this batch (not green or Chaaya-deferred):**

| File | Reason |
|------|--------|
| `jit-*.scm`, `llvm-*.scm` | JIT/LLVM backend not in Chaaya — do not wire |
| `circular-list-terminate.scm` | hangs (timeout) — needs printer/GC fix |
| `equal-dag.scm` | `equal?` on DAGs |
| `case-lambda-fixes.scm` | case-lambda edge cases |
| `expt-negative-base-1725.scm` | negative-base fractional/complex `expt` |

Add the next batch (ports, fibers, expander) only after local green + `make test`.
