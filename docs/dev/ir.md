# Intermediate representation (IR)

Chaaya’s compile path inserts a small tree IR between macro expansion and
bytecode emission. The IR is independent of Kaappi’s Zig IR and of any future
LLVM IR — same *role* (analyze / optimize before codegen), different shape and
ABI.

Header: [`include/chaaya/ir.h`](../../include/chaaya/ir.h).

**See also:** [KEP-0008](https://github.com/kaappi/keps/blob/main/keps/0008-shared-ir-contract.md)
documents the core-form set, optimization set, and shadowing-safety
invariant this IR shares with kaappi's and paal's independent IRs — the
"different shape and ABI" above is deliberate, but the safety gates in
this file's Optimization section are meant to match kaappi's equivalent
guard in spirit.

---

## Pipeline placement

```text
Source
  → Reader
  → Expander          (syntax-rules / hygiene)
  → IR lower          (Scheme datum → ChIrNode tree)
  → IR analyze        (tail marks + constant flags)
  → IR optimize       (fold / simplify; re-analyze)
  → IR emit           (tree → Scheme datum again)
  → Legacy compiler   (compile_expr → register bytecode)
  → VM
```

Top-level compile in [`src/compiler.c`](../../src/compiler.c) runs:

1. `ch_ir_lower`
2. `ch_ir_analyze`
3. `ch_ir_optimize`
4. `ch_ir_emit` with callback `emit_ir_with_legacy` → `compile_expr`

So today the IR does **not** emit opcodes directly. Emit reifies an optimized
Scheme form and hands it to the existing special-form / call compiler. That
keeps derived-form desugaring (`let`, `cond`, `quasiquote`, …) in one place
while still allowing constant folding and simple rewrites earlier.

Derived forms that the lowerer does not model stay as `CH_IR_RAW` nodes (opaque
datums) and pass through emit unchanged.

---

## Source files

| File | Role |
|------|------|
| [`include/chaaya/ir.h`](../../include/chaaya/ir.h) | `ChIrKind`, `ChIrPrim`, `ChIrNode`, public API |
| [`src/ir.c`](../../src/ir.c) | `ch_ir_new_node` / `ch_ir_free` |
| [`src/ir_lower.c`](../../src/ir_lower.c) | Datum → tree |
| [`src/ir_analyze.c`](../../src/ir_analyze.c) | Tail position + constant propagation flags |
| [`src/ir_opt.c`](../../src/ir_opt.c) | Folds, dead branches, identity / `(not)` rewrites |
| [`src/ir_emit.c`](../../src/ir_emit.c) | Tree → datum, then legacy emit callback |
| [`tests/c/test_ir.c`](../../tests/c/test_ir.c) | Unit coverage for analyze / opt + runtime smoke |

Ownership: IR nodes are plain `calloc`/`free` trees (not GC objects). Values
stored inside nodes (`literal`, `quoted`, `params`, `raw.expr`, …) are ordinary
`ChValue`s and must remain rooted by the surrounding compile if GC can run.

---

## Node kinds (`ChIrKind`)

| Kind | Meaning |
|------|---------|
| `CH_IR_VOID` | Unspecified / void result |
| `CH_IR_LITERAL` | Self-evaluating datum (numbers, booleans, strings, …) |
| `CH_IR_QUOTE` | `(quote …)` payload |
| `CH_IR_VAR` | Variable reference (`ChSymbol *`) |
| `CH_IR_IF` | `(if test consequent [alternate])` |
| `CH_IR_LAMBDA` | `(lambda params body…)` — params stay a Scheme list |
| `CH_IR_SEQ` | `(begin …)` |
| `CH_IR_CALL` | General application |
| `CH_IR_SET` | `(set! name value)` |
| `CH_IR_DEFINE` | Simple `(define name value)` only |
| `CH_IR_DEFINE_SYNTAX` | Placeholder; emit yields void (macros already handled) |
| `CH_IR_AND` / `CH_IR_OR` | Short-circuit sequences |
| `CH_IR_PRIM_CALL` | Known arithmetic / comparison / `not` |
| `CH_IR_RAW` | Opaque Scheme form (derived special forms, odd shapes) |

Each `ChIrNode` also carries analysis fields:

- `tail_position` — set by analyze
- `is_constant` / `constant_value` — compile-time known result when safe

### Recognized primitives (`ChIrPrim`)

Lowering turns a symbolic callee into `CH_IR_PRIM_CALL` when the basename is
one of: `+`, `-`, `*`, `<`, `>`, `=`, `<=`, `>=`, `not`.

These are **optimization candidates**, not a separate runtime calling
convention. Emit turns them back into ordinary calls (`(+ …)` etc.).

---

## Lowering (`ch_ir_lower`)

[`src/ir_lower.c`](../../src/ir_lower.c) walks an expanded datum:

- Atoms → `LITERAL` / `VAR` / `VOID`
- Core special forms (`quote`, `if`, `begin`, `lambda`, `set!`, `define`,
  `define-syntax`, `and`, `or`) → structured nodes when shape is valid
- Known prim symbols → `PRIM_CALL`
- Other applications → `CALL`
- Derived / library forms (`let`, `cond`, `quasiquote`, `guard`,
  `define-library`, …) → `RAW` with the original list

Malformed core forms also fall back to `RAW` so the legacy compiler can report
errors with its usual messages.

Symbol matching uses `ch_symbol_basename`, so hygienic `__hyg_N_` renames still
classify as the intended keyword / prim for lowering purposes. Binding identity
for locals remains the compiler’s job after emit.

---

## Analysis (`ch_ir_analyze`)

[`src/ir_analyze.c`](../../src/ir_analyze.c) is a single recursive walk from the
root with `tail_position = true`:

- Marks which subexpressions are in tail position (`begin` / `lambda` body last
  form, `if` arms, last `and`/`or` operand, …).
- Propagates constants for literals, quotes, constant `if`/`and`/`or`/`begin`
  when every relevant child is constant.

Analyze does **not** fold the tree; it only annotates. Optimize may call
analyze again after rewrites.

---

## Optimization (`ch_ir_optimize`)

[`src/ir_opt.c`](../../src/ir_opt.c) is intentionally conservative.

### Safety gates

Before rewriting prims, the pass builds `prim_disabled[]`:

1. If the VM global for that name is not the built-in native with the same
   `native->name`, disable folding for that prim.
2. Walk the IR for top-level `define` / `set!` (and shallow `RAW` inspect) of
   those names and disable them.
3. Inside lambdas (`lambda_depth > 0`), skip const-fold / identity simplify for
   prims (user shadowing is common; keep semantics obvious).

So `(define (+ a b) 'user-plus) (+ 1 2)` still calls the user procedure at
runtime — covered in `test_ir`.

### Rewrites performed today

| Transform | Example |
|-----------|---------|
| Const-fold prims | `(+ 1 2)` → `3` (fixnum/flonum; overflow refuses fold) |
| Identity | `(+ x 0)`, `(* 1 x)`, `(- x 0)` → `x` |
| Dead `if` | `(if #f a b)` → `b` |
| `(if (not x) a b)` | → `(if x b a)` at top level when `not` is safe |
| Flatten / collapse `begin` | Nested / singleton sequences |

After optimize, analyze runs again so annotations match the new tree.

---

## Emit (`ch_ir_emit`)

[`src/ir_emit.c`](../../src/ir_emit.c) rebuilds a Scheme datum from the tree
(`ir_node_to_expr`), then invokes the supplied `ChIrLegacyEmitFn`.

`CH_IR_RAW` and `CH_IR_DEFINE_SYNTAX` are the escape hatches: raw returns the
stored expression; define-syntax emits void (the expander already installed the
transformer).

GC: temporary lists built during reify are rooted around `ch_gc_cons` /
`ch_gc_intern_symbol_cstr` the same way as the rest of the compiler.

---

## Testing

```bash
ctest --test-dir build -R test_ir --output-on-failure
# or via make test
```

[`tests/c/test_ir.c`](../../tests/c/test_ir.c) checks:

- Tail marks on `(begin …)`
- Const fold of `(+ 1 2)`
- Dead-branch elimination
- `(if (not x) …)` boolean simplify
- Runtime transparency, including redefined `+`

---

## Design notes / limits

- **Tree IR, not SSA.** Good enough for local folds and a clean LLVM-entry
  seam later; no CFG or register allocation lives here.
- **Partial coverage.** Most R7RS surface still compiles via `RAW` + legacy
  desugar. Extending lower/opt is incremental: teach lower a form, then add
  safe opts.
- **No direct bytecode emit yet.** When a native / LLVM backend grows, prefer
  consuming post-opt `ChIrNode` (or a later CFG derived from it) instead of
  re-parsing Scheme text — see [backends.md](backends.md).
- **Not Kaappi-compatible.** Do not assume node layouts, pass order, or prim
  IDs match Kaappi’s `ir.zig`.

---

## Related docs

- [architecture.md](architecture.md) — full pipeline and VM
- [backends.md](backends.md) — LLVM / WASM stubs that will eventually consume IR
- [scheme-tests.md](scheme-tests.md) — conformance suites that exercise the
  compile path end-to-end
