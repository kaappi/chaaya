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

## Implemented vs stubbed

**Working now:** run/REPL/stdin, `features`, `doctor`, `ast`, `expand`, `ir`,
`check` (expand + compile + CH4xxx lint), `explain`, `fmt` (with readback
round-trip guard), `test` (`--json`, `-j/--jobs`, `--seed`), bytecode cache
`status|clear` + auto `.chbc` read/write on plain runs, `--compile` bytecode
blob writer, optional disassembly dump, `lsp` (init/shutdown + diagnostics +
completion/hover/definition/references/symbols), and wiring for `--sandbox`, `--profile[--profile-json]`,
`--coverage[--coverage-xml]`, `--timings[=text|json]`, `--timeout`,
`--max-memory`, `--gc-stats`.

**Still limited:** timeout is POSIX `alarm()`-based, memory limit is
best-effort process RLIMIT, and LLVM object lowering is still MVP-level.

## Diagnostics

See [diagnostics.md](diagnostics.md). Prefer `CH` codes; `explain` also accepts `KP` aliases.

## Environment

| Variable | Role |
|----------|------|
| `CHAAYA_HOME` | Override `~/.chaaya` (history; bytecode cache) |
| `CHAAYA_NO_CACHE` | If set and not `0`, disable automatic `.chbc` read/write |
| `CHAAYA_LIB_DIR` | Reserved for native runtime libs |
