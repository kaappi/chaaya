# REPL

Reference for the interactive REPL ([`src/repl.c`](../../src/repl.c)).

## Overview

On a TTY (POSIX build), the REPL uses vendored linenoise for line editing,
history (`~/.chaaya/history`, 1000 entries), and tab completion (comma-commands
and defined globals). Non-TTY stdin is handled by the CLI as a **script**, not
the REPL.

Banner:

```text
Chaaya Scheme v0.1.0
Type ,help for commands, ,quit to exit.
```

Prompts: `chaaya> ` and continuation `  ... ` while paren depth > 0.
Multi-line input tracks `(`/`)` outside strings and `;` line comments.
The variable `_` holds the last non-void result.

## Comma commands

| Command | Effect |
|---------|--------|
| `,help` | Show command list |
| `,quit` / `,exit` | Leave the REPL |
| `,version` | Print version banner |
| `,load <file>` | Evaluate a Scheme file |
| `,gc` | Object count / collections |
| `,env [prefix]` | List defined globals |
| `,time <expr>` | Eval and print elapsed ms |
| `,type <expr>` | Print value type name |
| `,expand <expr>` | Macro-expand and print |
| `,import <lib>` | Import a library (e.g. `,import (srfi 64)`) |
| `,dis <expr>` | Disassemble a procedure's bytecode |
| `,apropos <text>` | Search global bindings |
| `,describe <name>` | Describe a global binding |
| `,break <name> [if <expr>]` | Break on named procedure (optional condition) |
| `,condition <id> <expr>` | Set/update breakpoint condition by id |
| `,breakpoints` | List breakpoints |
| `,delete <name>` | Remove breakpoint |
| `,watch <expr>` / `,unwatch` | Debugger watch expressions |
| `,step <expr>` | Eval; pause on next call |
| `,continue` / `,backtrace` / `,locals` | Debugger commands at `debug>` |
| `,up` / `,down` | Navigate call frames at `debug>` |

At a breakpoint the REPL enters a nested `debug>` prompt (`,continue`, `,step`,
`,next`, `,finish`, `,backtrace`, `,locals`, `,up`, `,down`, `,watch`, `,quit`).
Pause banners include `source:line` when available.

`,profile` is still not implemented (use CLI `--profile`).

## Key files

| Component | Location |
|-----------|----------|
| REPL loop, comma dispatch, paren depth | `src/repl.c` |
| Eval / `_` binding | `src/eval.c` |
| CLI entry / when to start REPL | `src/cli.c` |
| Linenoise | `third_party/linenoise/` |
