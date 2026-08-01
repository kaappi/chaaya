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
| `,type <expr>` | Eval and print type name |

Other Kaappi comma-commands (`,break`, `,step`, `,expand`, `,import`, …) print
a not-implemented message.

## Key files

| Component | Location |
|-----------|----------|
| REPL loop, comma dispatch, paren depth | `src/repl.c` |
| Eval / `_` binding | `src/eval.c` |
| CLI entry / when to start REPL | `src/cli.c` |
| Linenoise | `third_party/linenoise/` |
