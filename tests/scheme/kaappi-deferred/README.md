# Deferred Kaappi suites

These trees are copied from [Kaappi](https://github.com/kaappi/kaappi)
`tests/scheme/` for future enablement.

| Tree | Source | CTest / blockers (2026-08) |
|------|--------|----------------------------|
| `smoke/` | `kaappi/tests/scheme/smoke/` | not wired — enable after compliance Tier-1 stabilizes |
| `compliance/` | `kaappi/tests/scheme/compliance/` | **Tier 1 wired (green):** `sqrt-exact`, `lists`, `vectors`. **Tier 2 wired (green):** `macro-export-scope`, `chars`, `bytevectors`, `eval`, `lazy`. |
| `differential-probes/` | `kaappi/tests/scheme/differential/probes/` | not wired — needs full numeric tower + error semantics |

Active ports that already run under `make test`:

- `tests/scheme/bootstrap/` — Kaappi-shaped `check-*` suites (includes `srfi-smoke.scm`)
- `tests/scheme/probes/` — display-based probes
- `tests/scheme/r7rs/r7rs-tests.scm` — `r7rs_suite` (full file green in CTest)

Runner: [`tests/scheme/run_kaappi_deferred.cmake`](../run_kaappi_deferred.cmake).

**Enablement process:** local green with `./build/chaaya --lib-path ./lib <file>.scm`, then add `chaaya_kaappi_deferred_test(...)` in [`CMakeLists.txt`](../../../CMakeLists.txt).

Re-sync from Kaappi periodically:

```bash
rsync -a --delete --include='*/' --include='*.scm' --include='*.sh' --include='*.sbc' --exclude='*' \
  ../kaappi/tests/scheme/smoke/ tests/scheme/kaappi-deferred/smoke/
rsync -a --delete --include='*/' --include='*.scm' --include='*.sh' --include='*.sbc' --exclude='*' \
  ../kaappi/tests/scheme/compliance/ tests/scheme/kaappi-deferred/compliance/
cp ../kaappi/tests/scheme/differential/probes/*.scm tests/scheme/kaappi-deferred/differential-probes/
cp ../kaappi/tests/scheme/r7rs/r7rs-tests.scm tests/scheme/r7rs/
```
