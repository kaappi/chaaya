# CLI

Contributor notes for Chaaya’s command-line interface ([`src/cli.c`](../../src/cli.c)).

## Shape

The help layout and flag/subcommand names mirror
[Kaappi](https://github.com/kaappi/kaappi)’s `kaappi --help`. Version banner:

```text
Chaaya Scheme v0.1.0
```

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success (`--help`, `--version`, `--completions`, successful run) |
| 1 | Runtime/script error, `check` failure, or unimplemented command |
| 2 | Usage error (unknown option, missing args) |

## Default dispatch

1. No file + TTY stdin → REPL ([`src/repl.c`](../../src/repl.c))
2. No file + non-TTY → evaluate stdin as a script
3. `chaaya file.scm [args…]` → run file; args stored on `ChVM` for `(command-line)`

## Implemented tooling

**Working:** run/REPL/stdin, `features`, `doctor` (grouped checks), `ast`,
`expand`, `ir`, `check`, `explain` / `explain --all`, comment-preserving `fmt`,
`test` (`--json`, `-j`, `--seed`, discovery, `--changed`), bytecode cache
`status|clear` + auto `.chbc`, `--compile` bytecode writer, `--disassemble`,
`--gc-stats`, `--timings[=text|json]`, `--timeout`, `--max-memory`, `--sandbox`,
`--profile` / `--profile-json`, `--coverage` / `--coverage-xml`, `lsp`,
`--emit-llvm` / `compile` / `--native` (MVP).

**Limited:** timeout is POSIX `alarm()`-based; memory limit is best-effort
`RLIMIT_DATA`; LLVM object lowering remains MVP-level. Do not reimplement
thottam — use Kaappi’s Zig binary via `--lib-path`.

## Diagnostics

See [diagnostics.md](diagnostics.md). Prefer `CH` codes; `explain` also accepts `KP` aliases.

## Environment

| Variable | Role |
|----------|------|
| `CHAAYA_HOME` | Override `~/.chaaya` (history; bytecode cache) |
| `CHAAYA_NO_CACHE` | If set and not `0`, disable automatic `.chbc` read/write |
| `CHAAYA_LIB_DIR` | Directory for native runtime libs (`libchaaya_rt.a`) |
| `CHAAYA_TEST_EMIT` | Worker result path (set by `chaaya test` orchestrator) |
| `CHAAYA_TEST_SEED` | Worker SRFI-27 seed |

## Related docs

- [test.md](test.md) — SRFI-64 runner
- [fmt.md](fmt.md) — formatter
- [diagnostics.md](diagnostics.md) — CH codes
- [repl.md](repl.md) — REPL debugger
