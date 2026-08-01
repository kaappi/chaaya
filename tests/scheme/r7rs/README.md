# R7RS-small conformance suite (Phase 7A)

`r7rs-tests.scm` is the chibi/Kaappi R7RS-small suite, vendored from
`kaappi/tests/scheme/r7rs/r7rs-tests.scm`.

**Status:** wired into CTest as `r7rs_suite` via `tests/scheme/run_r7rs.cmake`.
Execution is currently partial (section counts are reported even when the
interpreter exits non-zero) while remaining runtime gaps are closed.

It requires:

- R7RS library system and `(import …)`
- Standard libraries: `(scheme base)`, `(scheme char)`, `(scheme lazy)`, …
- `(chibi test)` (or a compatible SRFI-64 runner)
- Full numeric tower, ports, `call/cc`, macros, etc.

Bootstrap coverage that *does* run today lives under
`tests/scheme/bootstrap/` and `tests/scheme/probes/`. See
[docs/dev/scheme-tests.md](../../docs/dev/scheme-tests.md).
