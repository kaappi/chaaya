# Deferred Kaappi suites

These trees are copied from [Kaappi](https://github.com/kaappi/kaappi)
`tests/scheme/` for future enablement. They are **not** run by `make test`
yet.

| Tree | Source | Blockers on Chaaya v0.1 |
|------|--------|-------------------------|
| `smoke/` | `kaappi/tests/scheme/smoke/` | `(import …)`, SRFI-64 / chibi-test, macros, most R7RS libraries |
| `compliance/` | `kaappi/tests/scheme/compliance/` | Same + full R7RS procedure surface |
| `differential-probes/` | `kaappi/tests/scheme/differential/probes/` | Full numeric tower, error mid-stream, `zero?`/`exact?`/`expt`, … |

Active ports that already run:

- `tests/scheme/bootstrap/` — SRFI-64-style checks without `import` (harness prepended)
- `tests/scheme/probes/` — display-based probes adapted from Kaappi’s differential probes
- `tests/scheme/r7rs/r7rs-tests.scm` — canonical R7RS-small suite (also deferred; needs libraries + `(chibi test)`)

When Phase 6+ lands (libraries, `syntax-rules`, SRFI-64), prefer enabling these
files in place over rewriting them. Re-sync from Kaappi periodically:

```bash
rsync -a --delete --include='*/' --include='*.scm' --include='*.sh' --include='*.sbc' --exclude='*' \
  ../kaappi/tests/scheme/smoke/ tests/scheme/kaappi-deferred/smoke/
rsync -a --delete --include='*/' --include='*.scm' --include='*.sh' --include='*.sbc' --exclude='*' \
  ../kaappi/tests/scheme/compliance/ tests/scheme/kaappi-deferred/compliance/
cp ../kaappi/tests/scheme/differential/probes/*.scm tests/scheme/kaappi-deferred/differential-probes/
cp ../kaappi/tests/scheme/r7rs/r7rs-tests.scm tests/scheme/r7rs/
```
