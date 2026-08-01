# Developer Documentation

Contributor docs for the Chaaya core repo. End-user material (guides, API
reference, cookbook) will live elsewhere when the language is ready for that —
nothing end-user-facing belongs here.

Documents fall into genres. **Guides** (this directory’s top level) are
evergreen and should stay current with the code. **Design decisions**
(`decisions/`) and **postmortems** (`postmortems/`) are point-in-time records:
each carries a status and date; only the status line is updated after the fact.
Open bugs belong in the issue tracker.

## Guides

| Document | Contents |
|----------|----------|
| [architecture.md](architecture.md) | Pipeline, values, GC, bytecode VM, source layout, roadmap seams |
| [backends.md](backends.md) | LLVM `--native` stub, WASM target option (`CHAAYA_WASM`), current backend limits |
| [c23.md](c23.md) | C23 standard policy, features vs C17, patterns to leverage |
| [cli.md](cli.md) | CLI help shape, exit codes, implemented vs stubbed commands |
| [repl.md](repl.md) | REPL prompts, multiline, comma-commands, history |
| [scheme-tests.md](scheme-tests.md) | Bootstrap suites, probes, deferred Kaappi/R7RS |
| [thottam.md](thottam.md) | How to use sibling Kaappi thottam with Chaaya `--lib-path` |

## Design decisions

None yet. Add dated notes under `decisions/` when a non-obvious choice needs a
record (e.g. continuation strategy, IR introduction).

## Postmortems

None yet. Add date-prefixed write-ups under `postmortems/` when an investigation
produces analysis worth keeping.

## Policy

- Prefer short, concrete docs tied to real file paths in this repo.
- Kaappi ([kaappi/kaappi](https://github.com/kaappi/kaappi)) is a *behavioral*
  and *architectural* reference, not a line-by-line source. Cite Kaappi docs
  when comparing designs; do not assume ABI or bytecode compatibility.
- Keep source files under ~1500 lines; split along domain seams when they grow.
