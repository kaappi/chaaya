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
3. `chaaya file.scm [args…]` → run file; args stored on `ChVM` for a future `(command-line)`

## Implemented vs stubbed

**Working now:** run/REPL/stdin, `features`, `doctor`, `ast`, `expand`, `check`, `fmt`,
`cache status|clear` (filesystem-backed status + clear; cache writer still NYI),
`lsp` (minimal initialize/shutdown JSON-RPC), `--lib-path` (stored),
`--completions bash|zsh|fish`.

**Accepted in help, exit 1 if invoked:** `compile`, `explain`, `test`, `ir`,
`wasm`, and engine flags (`--compile`, `--emit-llvm`, `--sandbox`,
`--profile`, …). `--native` is routed to an explicit LLVM backend stub.

## Environment

| Variable | Role |
|----------|------|
| `CHAAYA_HOME` | Override `~/.chaaya` (history; future cache/lib) |
| `CHAAYA_LIB_DIR` | Reserved for native runtime libs |
