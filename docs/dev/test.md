# Test runner

`chaaya test` runs SRFI-64 suites in isolated worker subprocesses
([`src/test_runner.c`](../../src/test_runner.c)).

## Usage

```bash
chaaya test [--json] [-j N] [--seed N] [--changed] [--since REV] [--list-affected] [paths...]
```

With no paths, discovers `*.scm` under `./tests`.

## Worker protocol

Each worker is a plain `chaaya <file>` with:

| Env | Role |
|-----|------|
| `CHAAYA_TEST_EMIT` | Absolute path for the worker's one JSON result object |
| `CHAAYA_TEST_SEED` | Optional SRFI-27 seed |

Workers install a quiet collecting SRFI-64 runner, suppress `(exit)` so results
can still be emitted, then write Kaappi-shaped JSON:

```json
{"type":"file","file":"...","suite":"...","tests":N,"pass":N,"fail":N,
 "xpass":N,"xfail":N,"skip":N,"error":false,"error_message":null,
 "duration_ms":1.23,"failures":[{"name":"...","kind":"fail","expected":"...","actual":"..."}]}
```

Orchestrator exit is nonzero iff any `fail`, `xpass`, or file `error`.

## Selection

`--changed` / `--list-affected` use `git diff --name-only` against `--since`
(default `HEAD`). Native/`src/` changes force a full run (safety over precision).
