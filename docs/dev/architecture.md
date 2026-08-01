# Architecture

Chaaya implements Scheme as a bytecode-compiled language with a register-based
VM written in C17. This document describes the bootstrap subsystems (v0.1) and
where later R7RS / Kaappi-class features are expected to plug in.

Chaaya is a free redesign inspired by
[Kaappi](https://github.com/kaappi/kaappi), not a translation of its Zig sources.
Bytecode, IR, and ABI are independent.

---

## Pipeline (current)

```text
Source
  → Reader          (UTF-8 datum parser)
  → Expander        (`define-syntax` / `syntax-rules`; hygiene via `__hyg_N_` renames)
  → Compiler        (AST → bytecode; derived forms desugared in C)
  → VM              (register bytecode)
  → GC              (stop-the-world mark-sweep)
```

Kaappi’s full pipeline is Reader → Expander → IR → Analysis → Optim → Bytecode →
VM. Chaaya has a hygienic expander MVP and still collapses IR; special forms and
derived forms (`cond`, `let*`, `letrec`, `when`, `unless`, `quasiquote`) are
handled in the compiler.

| Stage | Files | Role |
|-------|-------|------|
| **Reader** | [`src/reader.c`](../../src/reader.c), [`include/chaaya/reader.h`](../../include/chaaya/reader.h) | Recursive-descent datum reader: lists, vectors, quote/quasiquote abbrevs, strings, numbers, `#t`/`#f`, characters |
| **Expander** | [`src/expander.c`](../../src/expander.c), [`include/chaaya/expander.h`](../../include/chaaya/expander.h) | `syntax-rules` match/instantiate, macro table, `chaaya expand` |
| **Printer** | [`src/printer.c`](../../src/printer.c), [`include/chaaya/printer.h`](../../include/chaaya/printer.h) | `write` / `display` rendering |
| **Compiler** | [`src/compiler.c`](../../src/compiler.c), [`include/chaaya/compiler.h`](../../include/chaaya/compiler.h) | Top-level AST → `ChFunction` bytecode; lexical locals + upvalues |
| **VM** | [`src/vm.c`](../../src/vm.c), [`include/chaaya/vm.h`](../../include/chaaya/vm.h) | Dispatch loop, calls/tail-calls, globals, upvalue close-over |
| **GC** | [`src/gc.c`](../../src/gc.c), [`include/chaaya/gc.h`](../../include/chaaya/gc.h) | Mark-sweep heap, root stack, symbol intern table |
| **Values** | [`src/value.c`](../../src/value.c), [`include/chaaya/value.h`](../../include/chaaya/value.h) | NaN-boxed `ChValue`, heap object tags, equality |
| **Primitives** | [`src/prim_core.c`](../../src/prim_core.c), [`src/prim_control.c`](../../src/prim_control.c) | Core natives + `call/cc` / wind / exceptions |
| **Eval** | [`src/eval.c`](../../src/eval.c) | Shared top-level eval + `_` binding |
| **CLI** | [`src/cli.c`](../../src/cli.c), [`include/chaaya/cli.h`](../../include/chaaya/cli.h) | Kaappi-shaped help/flags/subcommands; see [cli.md](cli.md) |
| **REPL** | [`src/repl.c`](../../src/repl.c), [`include/chaaya/repl.h`](../../include/chaaya/repl.h) | Interactive loop; see [repl.md](repl.md) |
| **Driver** | [`src/main.c`](../../src/main.c) | `main` → parse → dispatch |
| **linenoise** | [`third_party/linenoise/`](../../third_party/linenoise/) | Vendored BSD line editor (history in `~/.chaaya/history`) |

---

## Value representation

`ChValue` is a `uint64_t` using the same NaN-boxing family as Kaappi:

| Condition | Meaning |
|-----------|---------|
| `v < 0xFFFC000000000000` | Flonum (raw IEEE-754 bits; NaNs canonicalized) |
| `(v >> 48) == 0xFFFC` | Heap pointer (48-bit address of `ChObject` header) |
| `(v >> 48) == 0xFFFD` | Fixnum (signed i48) |
| `(v >> 48) == 0xFFFE` | Immediate (`nil`, `#f`, `#t`, void, eof, undefined) or character |

Heap objects begin with `ChObject { tag, marked, next }` and are threaded on the
GC’s intrusive list. Bootstrap tags:

| Tag | Type |
|-----|------|
| `CH_TAG_PAIR` | Cons cell |
| `CH_TAG_SYMBOL` | Interned symbol (flexible name) |
| `CH_TAG_STRING` | UTF-8 byte string |
| `CH_TAG_VECTOR` | Heterogeneous vector |
| `CH_TAG_FUNCTION` | Bytecode prototype (code, constants, upvalue descriptors) |
| `CH_TAG_CLOSURE` | Function + captured upvalues |
| `CH_TAG_NATIVE` | C primitive (`ChNativeFn`) |
| `CH_TAG_CONTINUATION` | `call/cc` snapshot (regs, frames, winds, handlers) |
| `CH_TAG_VALUES` | Multiple return values |
| `CH_TAG_PORT` | Text ports (stdio or string) |
| `CH_TAG_TRANSFORMER` | `syntax-rules` macro transformer |
| `CH_TAG_RECORD_TYPE` | R7RS record type descriptor |
| `CH_TAG_RECORD` | Record instance (fields[]) |

Control stacks on the VM (not heap tags): dynamic-wind records and exception
handlers. `dynamic-wind` is Scheme over `%push-wind` / `%pop-wind` (Kaappi
pattern) so continuation restores re-enter `after` correctly.

---

## Garbage collection

Stop-the-world **mark-and-sweep**:

1. Explicit roots via `ch_gc_push` / `ch_gc_pop` (and interned symbols as roots).
2. Mark reachable heap objects from those roots.
3. Sweep unmarked objects from the intrusive list.

Allocation may trigger collection when `alloc_count` crosses a threshold.
There is **no** write barrier or generation split yet; a generational collector
is planned when allocation pressure demands it.

**Contributor rule:** any `ChValue` that must survive a `ch_gc_*` allocation must
be on the root stack (or reachable from something that is) before the
allocation runs.

---

## Bytecode and VM

Opcodes live in [`include/chaaya/opcode.h`](../../include/chaaya/opcode.h). The
bootstrap ISA is a compact register machine (~20 ops): loads, globals,
upvalues, call / tail-call / return, jumps, closure, `cons`, `halt`.

**Call convention:** at call site register `base` holds the callee; `base+1 …`
hold arguments. Entering a Scheme closure shifts arguments down so local `0`
is the first parameter (the callee slot is overwritten). Tail calls reuse the
current frame’s register base.

**Closures:** `CH_OP_CLOSURE` builds a `ChClosure` from a `ChFunction` constant,
capturing open upvalues from the current frame (or linking to the enclosing
closure’s upvalues). Leaving a frame closes upvalues whose locations fall in
that frame’s register window.

Special forms compiled today: `quote`, `if`, `lambda`, `define`, `set!`,
`begin`, `and`, `or`, `let` (desugared to lambda + call).

---

## Source layout

```text
include/chaaya/   Public-ish headers (ch_ / Ch prefixes)
src/              One concern per .c file (cli, repl, eval, vm, …)
tests/c/          CTest unit tests
tests/scheme/     Scheme smoke programs
tests/cli/        CLI integration helpers
docs/dev/         Contributor documentation
third_party/      Vendored C deps (linenoise)
lib/              Portable .sld trees (search via --lib-path / ./lib)
```

Build: CMake 3.20+ produces `chaaya` and links tests against static
`chaaya_core`.

---

## Roadmap seams

| Next capability | Likely touch points |
|-----------------|---------------------|
| Full R7RS-small surface | Numeric tower; more `(scheme …)` libs; SRFI surface |
| Ports / numeric tower | Bignum/rational/complex; richer port I/O |
| IR + opts | Insert between expander and bytecode; prep for LLVM |
| FFI / fibers / LSP / WASM | After R7RS-small sequential interpreter is solid |

Done: `call/cc`, `dynamic-wind`, exceptions; hygienic `syntax-rules`; R7RS library
system; `include` / `cond-expand` / `(features)`; R7RS `define-record-type`;
`case-lambda`; `delay`/`force` (`(scheme lazy)`); file ports (`(scheme file)`);
built-in `(scheme base|write|read|cxr|char|process-context|lazy|file|case-lambda)`.

See the root [README](../../README.md) for the phase table. Conformance /
compatibility tests borrowed from Kaappi are documented in
[scheme-tests.md](scheme-tests.md).
