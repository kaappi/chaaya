# Architecture

Chaaya implements Scheme as a bytecode-compiled language with a register-based
VM written in C23. This document describes the bootstrap subsystems (v0.1) and
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
  → IR              (lower → analyze → optimize → reify; see [ir.md](ir.md))
  → Compiler        (reified AST → bytecode; derived forms desugared in C)
  → VM              (register bytecode)
  → GC              (stop-the-world generational mark-sweep)
```

Kaappi’s pipeline is Reader → Expander → IR → Analysis → Optim → Bytecode → VM.
Chaaya mirrors that shape with its own tree IR (`ChIrNode`). Emit currently
reifies Scheme datums and calls the legacy `compile_expr` path, so derived forms
(`cond`, `let*`, `letrec`, `when`, `unless`, `quasiquote`, …) still desugar in
the compiler via `CH_IR_RAW` passthrough. Details: [ir.md](ir.md).

| Stage | Files | Role |
|-------|-------|------|
| **Reader** | [`src/reader.c`](../../src/reader.c), [`include/chaaya/reader.h`](../../include/chaaya/reader.h) | Recursive-descent datum reader: lists, vectors, quote/quasiquote abbrevs, strings, numbers, `#t`/`#f`, characters |
| **Expander** | [`src/expander.c`](../../src/expander.c), [`include/chaaya/expander.h`](../../include/chaaya/expander.h) | `syntax-rules` match/instantiate, macro table, `chaaya expand` |
| **IR** | [`src/ir_*.c`](../../src/ir_lower.c), [`include/chaaya/ir.h`](../../include/chaaya/ir.h) | Tree IR: lower, analyze, optimize, reify; see [ir.md](ir.md) |
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

Allocation may trigger a **minor** collection when `alloc_count` crosses a
threshold (periodic **major** collections sweep both generations). New objects
start in the young generation and promote by survival age. `ch_gc_write_barrier`
records old→young stores on a remembered set; minor GC marks from roots plus
remembered-set object contents, then prunes entries that no longer reference
young objects.

**Fibers / reactor:** cooperative fibers (`src/fiber.c`) park on channels, timers,
and fds. The reactor (`src/reactor.c`) owns a timer heap plus a kqueue (Darwin)
or epoll (Linux) fd multiplexer; `thread-sleep!` schedules a timer and parks the
current fiber so siblings keep running. Feature ids: `chaaya-reactor` /
`kaappi-reactor`.

**SRFI-18 / shared channels / FFI callbacks:** OS threads (`src/thread.c`) use a
per-thread GC/VM, `ch_gc_deep_copy` at start/join, and owner checks on fibers and
local channels. Captured channels promote to `SharedChannel` (`src/shared_channel.c`)
with envelope deep-copy — including a freshly made, not-yet-promoted channel handed
off *as a message payload* (e.g. a reply channel), which the envelope's transient
heap promotes by borrowing the sending gc's identity. FFI callbacks
(`src/ffi_callback.c`) provide a small trampoline slot pool with deferred exception
re-raise and NUL-safe string args. `thread-join!`/`mutex-lock!`/`fiber-join` accept
an optional timeout (seconds or an SRFI-18 time object, an absolute deadline)
driven through the same reactor/scheduler step used by channel timeouts.
SRFI-18 coverage: `thread-specific[-set!]`, `thread-terminate!` (cooperative
marker — a live OS thread cannot be force-killed from C, so it runs to
completion but `thread-join!` reports `terminated-thread-exception?` instead of
its real outcome), `join-timeout-exception?`, `uncaught-exception?`/
`-reason` (reason is the printed form of the original condition — the raw
value generally cannot cross the OS-thread boundary safely), and a mutex/condvar
MVP (`mutex-name`/`-specific[-set!]`/`-state`, `condition-variable-name`/
`-specific[-set!]`, abandoned-mutex detection when a lock's owner fiber/thread
has already finished). Feature ids: `chaaya-threads` / `kaappi-threads`.

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

Build: CMake 3.21+ produces `chaaya` (C23) and links tests against static
`chaaya_core`. Language policy: [c23.md](c23.md).

---

## Roadmap seams

| Next capability | Likely touch points |
|-----------------|---------------------|
| Full R7RS-small surface | More `(scheme …)` libs; SRFI surface |
| Ports / numeric tower | Richer port I/O; exact complexes |
| Richer IR + direct bytecode / LLVM | Grow `ChIrNode` coverage; emit opcodes (or LLVM) without reify; see [ir.md](ir.md) |
| FFI / fibers / LSP / WASM | After R7RS-small sequential interpreter is solid |

Done: `call/cc`, `dynamic-wind`, exceptions; hygienic `syntax-rules`; R7RS library
system; `include` / `cond-expand` / `(features)`; R7RS `define-record-type`;
`case-lambda`; `delay`/`force` (`(scheme lazy)`); file ports (`(scheme file)`);
bignum exact integers (`src/bignum.c`); exact rationals
(`src/rational.c`); inexact complexes (`src/complex.c`); built-in
`(scheme base|write|read|cxr|char|process-context|lazy|file|complex|inexact|exact|case-lambda)`.

See the root [README](../../README.md) for the phase table. Conformance /
compatibility tests borrowed from Kaappi are documented in
[scheme-tests.md](scheme-tests.md).
