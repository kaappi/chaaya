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
  → Compiler        (AST → bytecode; no IR yet)
  → VM              (register bytecode)
  → GC              (stop-the-world mark-sweep)
```

Kaappi’s full pipeline is Reader → Expander → IR → Analysis → Optim → Bytecode →
VM. Chaaya collapses expander and IR until later phases; special forms are
handled directly in the compiler today.

| Stage | Files | Role |
|-------|-------|------|
| **Reader** | [`src/reader.c`](../../src/reader.c), [`include/chaaya/reader.h`](../../include/chaaya/reader.h) | Recursive-descent datum reader: lists, vectors, quote/quasiquote abbrevs, strings, numbers, `#t`/`#f`, characters |
| **Printer** | [`src/printer.c`](../../src/printer.c), [`include/chaaya/printer.h`](../../include/chaaya/printer.h) | `write` / `display` rendering |
| **Compiler** | [`src/compiler.c`](../../src/compiler.c), [`include/chaaya/compiler.h`](../../include/chaaya/compiler.h) | Top-level AST → `ChFunction` bytecode; lexical locals + upvalues |
| **VM** | [`src/vm.c`](../../src/vm.c), [`include/chaaya/vm.h`](../../include/chaaya/vm.h) | Dispatch loop, calls/tail-calls, globals, upvalue close-over |
| **GC** | [`src/gc.c`](../../src/gc.c), [`include/chaaya/gc.h`](../../include/chaaya/gc.h) | Mark-sweep heap, root stack, symbol intern table |
| **Values** | [`src/value.c`](../../src/value.c), [`include/chaaya/value.h`](../../include/chaaya/value.h) | NaN-boxed `ChValue`, heap object tags, equality |
| **Primitives** | [`src/prim_core.c`](../../src/prim_core.c), [`include/chaaya/prim.h`](../../include/chaaya/prim.h) | Native procedures registered into VM globals |
| **Driver** | [`src/main.c`](../../src/main.c) | REPL (linenoise on POSIX TTYs; plain `fgets` otherwise) and file runner |
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

Later R7RS work will add ports, bytevectors, records, continuations, promises,
parameters, numeric tower heap types, etc.

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
src/              One concern per .c file
tests/c/          CTest unit tests
tests/scheme/     Scheme smoke programs
docs/dev/         This contributor documentation
third_party/      Vendored C deps (linenoise)
lib/              Portable .sld trees (reserved; after library system)
```

Build: CMake 3.20+ produces `chaaya` and links tests against static
`chaaya_core`.

---

## Roadmap seams

| Next capability | Likely touch points |
|-----------------|---------------------|
| `call/cc`, `dynamic-wind`, exceptions | New heap tags; VM frame/wind stacks; control primitives |
| `syntax-rules` | Expander stage before compile; hygiene |
| R7RS libraries | Library registry + `.sld` loader; built-in `(scheme …)` like Kaappi (core is not portable `.sld`) |
| IR + opts | Insert between expander and bytecode; prep for LLVM |
| FFI / fibers / LSP / WASM | After R7RS-small sequential interpreter is solid |

See the root [README](../../README.md) for the phase table.
