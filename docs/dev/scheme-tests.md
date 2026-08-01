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
| `tests/scheme/r7rs/` | Canonical R7RS-small suite (vendored) | **partial** (`r7rs_suite` in CTest; §4.1–5 and much of §6 green; full run stops in §6.2 numeric tower — `denominator` / flonum gaps) |
| `tests/scheme/kaappi-deferred/` | Full Kaappi smoke/compliance/probe copies | **partial** (Tier-1 + three Tier-2 compliance files wired in CTest; SRFI-64 macros green) |

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
   (Tier-1: `sqrt-exact`, `lists`, `vectors` in CTest; Tier-2: `macro-export-scope`, `chars`, `bytevectors`; `eval`/`lazy` still blocked)
