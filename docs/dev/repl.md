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

Type `,help` in the REPL for the authoritative list.

**General:** `,help`, `,quit` (also `,exit`)

**Evaluation:**

| Command | Effect |
|---------|--------|
| `,time <expr>` | Measure execution time |
| `,type <expr>` | Show result type |
| `,expand <expr>` | Show macro expansion without evaluating |
| `,profile <expr>` | Profile timing and calls |
| `,dis <expr>` | Disassemble a procedure's bytecode |

**Inspection:**

| Command | Effect |
|---------|--------|
| `,describe <sym>` | Show procedure arity and type |
| `,apropos <str>` | Search bindings by substring |
| `,env [prefix]` | List bindings, optionally filtered by prefix |

**Debugging:**

| Command | Effect |
|---------|--------|
| `,break <name> [if <expr>]` | Break on named procedure (optional condition) |
| `,breakpoints` | List breakpoints |
| `,delete <name>` / `,delete all` | Remove a breakpoint, or clear all |
| `,condition <id> <expr>` | Set/update breakpoint condition by id |
| `,step <expr>` | Eval; pause on next call |
| `,watch <expr>` / `,unwatch` | Debugger watch expressions |

At a breakpoint the REPL enters a nested `debug>` prompt (`,continue`, `,step`,
`,next`, `,finish`, `,backtrace`, `,locals`, `,up`, `,down`, `,watch`, `,quit`).
Pause banners include `source:line` when available.

**System:**

| Command | Effect |
|---------|--------|
| `,gc` | Show GC statistics |
| `,version` | Show Chaaya version |
| `,load <file>` | Load and run a Scheme file |
| `,import <lib>` | Import a library (e.g. `,import (srfi 1)`) |

## Key files

| Component | Location |
|-----------|----------|
| REPL loop, comma dispatch, paren depth | `src/repl.c` |
| Eval / `_` binding | `src/eval.c` |
| CLI entry / when to start REPL | `src/cli.c` |
| Linenoise | `third_party/linenoise/` |
