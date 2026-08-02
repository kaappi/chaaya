# Chaaya — remaining work

Tracked gaps after the expander bug campaign (2026-08-02). Detail and
repro context live in the linked docs; this file is the checklist.

Sources: [scheme-tests.md](dev/scheme-tests.md),
[srfi-import-audit.md](dev/srfi-import-audit.md),
[README.md](../README.md) Phase 11+ backlog.

---

## Unwired kaappi-deferred smokes (35)

Non-backend corpus is **222/257** wired (~86%). Wire only after local green +
`make test`. Permanently skipped: `jit-*.scm` / `llvm-*.scm` (no JIT/LLVM
backend in Chaaya).

### Fiber / thread scheduler races & capacity (15)

Deep scheduler/dispatch work — not quick fixes. Prefer isolation green before
CTest; `nested-wait-under-sleep-dirty-snapshot-1490` passes alone but times
out under parallel `ctest -j`.

- [ ] `fiber-blocked-exit`
- [ ] `fiber-channel-receive-waits-out-peer-sleep`
- [ ] `fiber-channel-rendezvous`
- [ ] `fiber-dispatch-blocked-siblings`
- [ ] `fiber-error-handling`
- [ ] `fiber-many-waiters-one-object-1530`
- [ ] `fiber-pipeline`
- [ ] `fiber-thread-join-deadline-cleared-after-resolve`
- [ ] `fiber-timed-mutex-lock-not-starved-by-busy-sibling`
- [ ] `mutex-nested-dispatch-dirty-snapshot-1487`
- [ ] `mutex-timeout`
- [ ] `nested-wait-under-sleep-dirty-snapshot-1490`
- [ ] `thread-foreign-owner-1484`
- [ ] `thread-port-isolation`
- [ ] `deep-copy-list-801`

### SRFI-170 POSIX primitives (4)

- [ ] `filesystem-intcast` — `set-file-mode` / `set-umask!` / `umask` unimplemented
- [ ] `group-info-by-name-1161` — `user-gid` / `group-info` unimplemented
- [ ] `srfi170-time-objects` — `posix-time` / `monotonic-time` gaps
- [ ] `filesystem-nul-path-805`

### Fixed compiler limits (`uint8_t` register file) (5)

Intentional ~200–256 argument/clause/register ceiling; raising needs a
register-file width change.

- [ ] `apply-large-arglist`
- [ ] `call-arg-limit`
- [ ] `case-large-clauses`
- [ ] `large-form-body-791`
- [ ] `vector-large-arglist`

### Global / library rebinding (3)

- [ ] `percent-name-user-library-1856`
- [ ] `library-redefine-closure-820`
- [ ] `define-values-letrec-1719`

### Continuations / dynamic-wind / handlers (2)

- [ ] `handler-wind-depth-1886` — deep nested wind/handler stacks
- [ ] `gc-rooting-safety` — needs distinct escape-only `call/ec` (extent-checked);
  aliasing to re-entrant `call/cc` was tried and reverted (segfault outside extent)

### Library primitive closures (1)

- [ ] `trampoline-polish-1375` — `vector-map` / `string-map` / `string-for-each`
  as Scheme closures miss expected arity/type errors; `%push-wind` / `%pop-wind`
  unbound on one path

### Numeric tower (1)

- [ ] `exact-integer-sqrt-851` — hangs (not just slow) on ~2^3000-bit bignums

### Reader / ports (1)

- [ ] `peek-char-malformed-utf8` — hangs on truncated multi-byte UTF-8;
  suspected stream-desync retry loop

### GC (1)

- [ ] `gc-root-growth` — 2000-deep native re-entrancy aborts instead of growing
  the root buffer or hitting the documented re-entrancy cap cleanly

### FFI / platform (1)

- [ ] `bignum-rational-ffi-793` — `dlopen("liblibm.so")` fails on macOS
  (needs per-platform libm path probe)

---

## Portable SRFI import failures (43)

Probe: `scripts/audit-srfi-imports.sh` → [srfi-import-audit.md](dev/srfi-import-audit.md)
(165 top-level `lib/srfi/N.sld`; 122 pass / 43 fail as of 2026-08-02).

### Native sub-libraries not ported from Kaappi

Need C ports of Kaappi Zig natives (`srfi.NNN.primitives`, NumericVector, etc.).

- [ ] Record-type / descriptor engine: SRFI **57**, **131**, **136**, **137**,
  **150**, **237**, **240** (`srfi.237.primitives` / `srfi.211.primitives`)
- [ ] Homogeneous numeric vectors (SRFI **160** stack): **4**, **63**, **66**,
  **74**, **231**
- [ ] Custom ports: SRFI **181**
- [ ] Syntax-parameter / ER macros: SRFI **211** (via **150**)

### Kaappi-only helper libraries

- [ ] `kaappi.sysinfo` consumers: SRFI **59**, **112**, **193**
- [ ] `kaappi.primitives` consumer: SRFI **271**

### Syntax / special-form export & import

Compiler forms are not ordinary globals; needs syntax-export support in the
library/expander layer.

- [ ] Export special forms: SRFI **0** (`cond-expand`), **11** / **234**
  (`let-values`), **16** (`case-lambda`), **34** (`guard`), **45** (`delay`),
  **189** (`either`), **213** (`define-property`), **257** (`_`), **43**
  (`vector-any` — export gap)
- [ ] Import special forms as values: SRFI **46**, **71**, **139**, **147**,
  **148**, **149**, **244**, **248**

### Deeper load-time triage

- [ ] Wrong arity at library load: SRFI **44**, **113**, **146**, **162**,
  **167**, **168**, **228**
- [ ] Bare runtime `error` during load: SRFI **36**

---

## Phase 11+ tooling / backends

From the README roadmap — MVP exists; harden toward Kaappi parity.

- [ ] **LLVM native** — broader form lowering; full C ABI runtime bridge
  ([`src/llvm_backend.c`](../src/llvm_backend.c))
- [ ] **WASM** — wasm32-wasi CI matrix; browser playground binary
  ([`scripts/build-wasm.sh`](../scripts/build-wasm.sh))
- [ ] **LSP** — workspace/project indexing; richer docs
  ([`src/lsp.c`](../src/lsp.c), [`src/lsp_analysis.c`](../src/lsp_analysis.c))
- [ ] **Compile cache** — import-aware caching; richer per-entry status
  ([`src/cache.c`](../src/cache.c))
- [ ] **CLI observability** — deeper library-export coverage; parallel
  `chaaya test --changed`
- [ ] **thottam** — consumer docs / smoke wiring only (use Kaappi Zig thottam;
  do not reimplement in C — [thottam.md](dev/thottam.md))
- [ ] **Kaappi `run-all.sh` corpus** — full reconciliation still open once
  remaining smoke/SRFI blockers shrink

---

## Out of scope / permanent skips

| Item | Reason |
|------|--------|
| `jit-*.scm`, `llvm-*.scm` smokes | No JIT/LLVM interpreter-tier backend in Chaaya |
| C reimplementation of thottam | Use sibling [`../kaappi/`](../../kaappi/) thottam |
