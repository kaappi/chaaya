# R7RS-small conformance suite (deferred)

`r7rs-tests.scm` is the chibi/Kaappi R7RS-small suite, vendored from
`kaappi/tests/scheme/r7rs/r7rs-tests.scm`.

**Status:** not executed by Chaaya CI yet.

It requires:

- R7RS library system and `(import …)`
- Standard libraries: `(scheme base)`, `(scheme char)`, `(scheme lazy)`, …
- `(chibi test)` (or a compatible SRFI-64 runner)
- Full numeric tower, ports, `call/cc`, macros, etc.

Bootstrap coverage that *does* run today lives under
`tests/scheme/bootstrap/` and `tests/scheme/probes/`. See
[docs/dev/scheme-tests.md](../../docs/dev/scheme-tests.md).
