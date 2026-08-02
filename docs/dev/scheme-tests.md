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
6. Complexes (inexact + exact rectangular) — **done** (`exact-complex` feature)
7. `(scheme inexact)` / `(scheme exact)` + math prims — **done** (MVP)
8. SRFI-64 or `(chibi test)` — **done** for wired deferred suites
9. Re-run Kaappi’s `tests/scheme/run-all.sh` corpus against `chaaya` — **in
   progress**. `run-all.sh` hardcodes `KAAPPI=zig-out/bin/kaappi` with no
   override hook, and running it as-is would mean either editing that script
   in the sibling `../kaappi/` repo or clobbering the developer's real
   `zig-out/bin/kaappi` binary — both out of scope here. As a substitute,
   Kaappi's own unmodified `tests/scheme/r7rs/r7rs-tests.scm` (the same
   canonical suite Chaaya vendors a lightly-patched copy of, at
   `tests/scheme/r7rs/r7rs-tests.scm`) was run directly against
   `./build/chaaya --lib-path ./lib`. It gets through `4.1`/`4.2` clean (27 +
   74 pass) then hits two real gaps in `4.3 Macros` before aborting: a
   hygiene miss (`let-syntax` template referencing an outer `x` returns the
   inner shadowed binding instead — expected `outer`, got `inner`) and a
   `letrec-syntax`-scoped `if` immediately after that reports `if: bad
   syntax` at compile time. Both are expander bugs worth their own follow-up;
   neither is a quick fix. Full-corpus reconciliation stays deferred.

## kaappi-deferred compliance

Wired in CTest as `kaappi_deferred_*` (see `CMakeLists.txt`). Enable with:

```bash
ctest --output-on-failure -R kaappi_deferred -E smoke
```

**Wired (language-parity track, mostly green):** includes
`include-lib-decls`, `r7rs-import-macro-gaps`, `r7rs-libraries-gaps`,
`r7rs-control-io-gaps`, `reader-port-refill-gaps`, `r7rs-expressions-gaps`,
`r7rs-hygiene-gaps`, `r7rs-continuation-gaps`, `r7rs-tail-procedures-gaps`,
`printer-gaps`, `record-ctor-clause-keyword-1882`, plus earlier
numeric/reader/unicode/string/datatype/time suites.

As of 2026-08-02, the previously red
`kaappi_deferred_r7rs_continuation_gaps` multi-value `call/cc` check is green
(call-with-values drains its consumer after a continuation barrier landing).

**Still deferred (not wired):** none on the language-parity track above.
Remaining Kaappi compliance files stay out until separately triaged.

## kaappi-deferred smoke

Wired in CTest as `kaappi_deferred_smoke_*`. Enable with:

```bash
ctest --output-on-failure -R kaappi_deferred_smoke
```

**Status as of 2026-08-02 (Batch 11):** 230 of 257 non-backend smoke
files wired (**230/257**, ~89%). "Non-backend" excludes the 8 permanently
skipped `jit-*.scm`/`llvm-*.scm` files (see below); 27 remain unwired.

**Wired:** language-surface fixes (`case-lambda-fixes`, `expt-negative-base-1725`,
`equal-dag`, `circular-list-terminate`) plus a large batch of locally green
smoke files (see `CMakeLists.txt` `smoke_*` entries), including
`smoke_portable_srfi_import` for explicit `--lib-path` portable SRFI import
coverage. Expander campaign wired `lambda-param-shadows-keyword-788`,
`expander-vector-patterns`, `internal-define-syntax-scope`,
`library-macro-leak-877`, `record-macro-scope-1718`, and `macro-chains`.
Batch 9 added `file-info-blocks` (missing `file-info:blocks`
accessor, now implemented) and `mutex-lock-false-owner` (`mutex-lock!`'s `#f`
timeout argument was inverted — it polled instead of blocking indefinitely,
and the optional explicit-owner-thread argument was never read; both fixed
in `src/thread.c`/`src/prim_fiber.c`).
Batch 10 wired six former hang/platform blockers: `exact-integer-sqrt-851`
(Newton now uses `ch_bignum_quotient`), `peek-char-malformed-utf8` (truncated
UTF-8 no longer spins the refill loop), `bignum-rational-ffi-793` (`ffi-open
"libm"` probes `libm.dylib`/`libm.so`), `gc-root-growth` (growable root buffer
+ catchable native re-entrancy cap), `filesystem-intcast` (`set-file-mode` /
`umask` / `set-umask!` / `nice`), and `filesystem-nul-path-805` (embedded NUL
rejected in filesystem path arguments).
Batch 11 wired the remaining SRFI-170 POSIX gaps: `group-info-by-name-1161`
(`user-uid` / `user-gid` / `user-effective-uid` / `user-effective-gid` /
`user-supplementary-gids` / `group-info` + `group-info?` / `:name` / `:gid`)
and `srfi170-time-objects` (`posix-time` / `monotonic-time` as SRFI-19
`time-utc` / `time-monotonic` objects).

`lib/kaappi/parallel.sld` loads cleanly under `--lib-path`; all three of its
smoke covers are wired and green: `kaappi-parallel-map`,
`kaappi-parallel-pool`, `kaappi-parallel-shutdown`.

**Permanently skipped:**

| File | Reason |
|------|--------|
| `jit-*.scm`, `llvm-*.scm` | JIT/LLVM backend not in Chaaya |

**Remaining unwired (27), grouped by blocker:**

| Blocker | Files | Notes |
|---------|------:|-------|
| Fiber/thread scheduler races & cross-thread capacity limits | 15 | `fiber-blocked-exit`, `fiber-channel-receive-waits-out-peer-sleep`, `fiber-channel-rendezvous`, `fiber-dispatch-blocked-siblings`, `fiber-error-handling`, `fiber-many-waiters-one-object-1530`, `fiber-pipeline`, `fiber-thread-join-deadline-cleared-after-resolve`, `fiber-timed-mutex-lock-not-starved-by-busy-sibling`, `mutex-nested-dispatch-dirty-snapshot-1487`, `mutex-timeout`, `nested-wait-under-sleep-dirty-snapshot-1490`, `thread-foreign-owner-1484`, `thread-port-isolation`, `deep-copy-list-801`. Deep scheduler/dispatch work, not quick fixes; `nested-wait-under-sleep-dirty-snapshot-1490` in particular passes in isolation but times out under parallel `ctest -j` load — a genuine contention-sensitive race, left unwired rather than wired flaky. |
| Fixed compiler limits (register file is `uint8_t`-indexed) | 5 | `apply-large-arglist`, `call-arg-limit`, `case-large-clauses`, `large-form-body-791`, `vector-large-arglist` — all hit an intentional ~200–256 argument/clause/register ceiling; raising it is a register-file width change, not a quick fix. |
| Global/library rebinding internals | 3 | `percent-name-user-library-1856`, `library-redefine-closure-820`, `define-values-letrec-1719`. |
| Continuation / dynamic-wind / exception-handler depth | 2 | `handler-wind-depth-1886` (deep nested wind/handler stacks), `gc-rooting-safety` (needs `call/ec` as a distinct escape-only, extent-checked continuation — aliasing it to the existing re-entrant `call/cc` primitive was tried and segfaults on invocation outside its extent instead of raising a catchable error, so the alias was reverted). |
| Library primitive closures (SRFI-133 etc.) | 1 | `trampoline-polish-1375` — `vector-map`/`string-map`/`string-for-each` as Scheme closures don't raise the expected arity/type errors, and `%push-wind`/`%pop-wind` are unexpectedly unbound in one path. |
| GC / heap-layout sensitivity (latent) | — | Batch 11: registering the new SRFI-170 natives shifted heap layout enough to break `bootstrap_macros`, `r7rs-thin-forms-gaps`, and `exact-prefix-856` depending on exact native count. Expanding the same SRFI-170 user/group cluster (`user-uid` / `user-effective-*` / `user-supplementary-gids`) stabilized the suite; root cause still looks like a GC/expander marking bug worth a dedicated investigation. |

See `docs/dev/srfi-import-audit.md` for the separate, lower-level audit of
which `lib/srfi/*.sld` files import cleanly (165 probed, 122 pass, 43 fail) —
that tracks portable-SRFI import health, not the kaappi-deferred smoke/compliance
corpus described here.

**Runtime/platform smokes wired:** `fiber-gc-remembered-set`, `fiber-channel-close`,
`fiber-sleep-does-not-stall-sibling`, `thread-sleep-876`, `thread-self-join`,
`fiber-channel-rendezvous-thread-lite`, `fiber-channel-rendezvous-thread`,
`fiber-channel-lost-wakeup-1489`, `fiber-channel-timeout`, `thread-join-timeout-878`,
`kaappi-parallel-pool`, `kaappi-parallel-shutdown`.

Add further smoke batches only after local green + `make test`.
