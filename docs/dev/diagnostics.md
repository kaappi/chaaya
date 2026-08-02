# Diagnostics

Chaaya emits stable `CH`-prefixed diagnostic codes so tools (editors, CI, agents)
can match on identifiers rather than scraping prose.

## Taxonomy

| Range | Stage |
|-------|-------|
| `CH1xxx` | Read / lexical |
| `CH2xxx` | Expand / compile / IR |
| `CH3xxx` | Runtime |
| `CH4xxx` | Static analysis (`chaaya check` lint) |
| `CH9xxx` | Internal / resource |

Codes are never renumbered once shipped. Message text may change freely.

Registry: [`src/diagnostics.c`](../../src/diagnostics.c),
[`include/chaaya/diagnostics.h`](../../include/chaaya/diagnostics.h).

## CLI

```bash
chaaya explain CH3001
chaaya explain undefined-variable --json
chaaya check file.scm --diagnostics=json
chaaya file.scm --diagnostics=text
```

Text shape:

```text
path.scm:3:5: compile error: [CH2002] syntax error
```

JSON shape (one LSP `Diagnostic` object per line on stderr):
`range`, `severity`, `code`, `source` (`"chaaya"`), `message`, plus `data.stage`.

Kaappi `KP` aliases are accepted by `explain` for corpus familiarity
(`chaaya explain KP3001` → `CH3001`).

## Related

- [cli.md](cli.md) — command surface (`explain`, `check --diagnostics=json`, `ir`)
- [backends.md](backends.md) — LSP `publishDiagnostics` uses the same funnel; `.chbc` cache
