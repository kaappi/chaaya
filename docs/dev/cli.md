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
`check` (expand + compile + CH4xxx lint), `explain`, `fmt`, `test` (fork/exec),
`cache status|clear` + auto `.chbc` writer on plain runs,
`lsp` (init/shutdown + didOpen diagnostics + symbols),
`--lib-path`, `--completions`, `--diagnostics=text|json`, `--deny-warnings`,
`--no-ir-opt` / `--no-opt`, `--emit-llvm`, `compile` / `--native` (MVP stub runtime).

**Still NYI / limited:** `--sandbox`, `--profile`, `--coverage`, `--timings`,
`--timeout`, `--max-memory`, `--disassemble`, full LLVM object lowering.

## Diagnostics

See [diagnostics.md](diagnostics.md). Prefer `CH` codes; `explain` also accepts `KP` aliases.

## Environment

| Variable | Role |
|----------|------|
| `CHAAYA_HOME` | Override `~/.chaaya` (history; bytecode cache) |
| `CHAAYA_NO_CACHE` | If set and not `0`, disable automatic `.chbc` read/write |
| `CHAAYA_LIB_DIR` | Reserved for native runtime libs |
