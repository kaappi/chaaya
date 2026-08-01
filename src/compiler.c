#include "chaaya/compiler.h"

#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/ir.h"
#include "chaaya/library.h"
#include "chaaya/opcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_MAX_LOCALS 256
#define CH_MAX_UPVALUES 64
#define CH_MAX_CONSTS 512
#define CH_CODE_INIT 256
#define CH_MAX_DERIVED_BINDINGS 64
#define CH_MAX_GUARD_CLAUSES 128

static size_t guard_gensym_counter = 0;
static size_t parameterize_gensym_counter = 0;
static size_t do_gensym_counter = 0;
static size_t case_gensym_counter = 0;
static size_t let_values_gensym_counter = 0;

typedef struct ChLocal {
    ChSymbol *name;
    uint8_t depth;
    bool is_captured;
} ChLocal;

typedef struct ChCompUpvalue {
    uint8_t index;
    bool is_local;
} ChCompUpvalue;

typedef struct ChFuncCompiler {
    struct ChFuncCompiler *enclosing;
    ChFunction *fn;
    ChValue fn_root;
    size_t compiling_fn_slot;
    ChLocal locals[CH_MAX_LOCALS];
    int local_count;
    int scope_depth;
    ChCompUpvalue upvalues[CH_MAX_UPVALUES];
    int upvalue_count;
    uint8_t next_reg;
    uint8_t max_regs;
    /* growable code buffer before freeze into fn */
    uint8_t *code;
    size_t code_len;
    size_t code_cap;
    ChValue constants[CH_MAX_CONSTS];
    size_t const_count;
    size_t const_roots; /* GC roots pushed for constants[] slots */
    bool is_toplevel;
} ChFuncCompiler;

void ch_compiler_init(ChCompiler *c, ChVM *vm) {
    c->vm = vm;
    c->error[0] = '\0';
}

const char *ch_compiler_error(const ChCompiler *c) {
    return c->error;
}

static ChCompileStatus fail(ChCompiler *c, const char *msg) {
    snprintf(c->error, sizeof(c->error), "%s", msg);
    return CH_COMPILE_ERROR;
}

static void fc_init(ChCompiler *c, ChFuncCompiler *fc, ChFuncCompiler *enclosing, ChFunction *fn,
                    bool toplevel) {
    memset(fc, 0, sizeof(*fc));
    fc->enclosing = enclosing;
    fc->fn = fn;
    fc->is_toplevel = toplevel;
    if (c->vm->gc.compiling_fn_depth >= sizeof(c->vm->gc.compiling_fns) / sizeof(c->vm->gc.compiling_fns[0])) {
        abort();
    }
    fc->compiling_fn_slot = c->vm->gc.compiling_fn_depth;
    c->vm->gc.compiling_fns[c->vm->gc.compiling_fn_depth++] = fn;
    fc->fn_root = ch_make_pointer(&fn->header);
    ch_gc_push(&c->vm->gc, &fc->fn_root);
    fc->code_cap = CH_CODE_INIT;
    fc->code = (uint8_t *)malloc(fc->code_cap);
    if (!fc->code) {
        abort();
    }
    fc->scope_depth = 0;
    fc->local_count = 0;
    fc->next_reg = 0;
    fc->max_regs = 0;
}

static void pop_const_roots(ChCompiler *c, ChFuncCompiler *fc) {
    if (fc->const_roots > 0) {
        ch_gc_pop_n(&c->vm->gc, fc->const_roots);
        fc->const_roots = 0;
    }
}

static void pop_root_at(ChGC *gc, size_t base) {
    size_t after = gc->root_count;
    if (after <= base) {
        abort();
    }
    if (after > base + 1) {
        memmove(&gc->roots[base], &gc->roots[base + 1],
                (after - base - 1) * sizeof(gc->roots[0]));
    }
    gc->root_count = after - 1;
}

static void fc_free_buf(ChCompiler *c, ChFuncCompiler *fc) {
    pop_const_roots(c, fc);
    free(fc->code);
    fc->code = NULL;
}

static void fc_end_compile(ChCompiler *c, ChFuncCompiler *fc) {
    pop_const_roots(c, fc);
    ch_gc_pop(&c->vm->gc);
    if (c->vm->gc.compiling_fn_depth > fc->compiling_fn_slot) {
        c->vm->gc.compiling_fn_depth = fc->compiling_fn_slot;
    }
}

static void fc_discard(ChCompiler *c, ChFuncCompiler *fc) {
    fc_end_compile(c, fc);
    fc_free_buf(c, fc);
}

static void emit_byte(ChFuncCompiler *fc, uint8_t b) {
    if (fc->code_len >= fc->code_cap) {
        fc->code_cap *= 2;
        uint8_t *n = (uint8_t *)realloc(fc->code, fc->code_cap);
        if (!n) {
            abort();
        }
        fc->code = n;
    }
    fc->code[fc->code_len++] = b;
}

static void emit_u16(ChFuncCompiler *fc, uint16_t v) {
    emit_byte(fc, (uint8_t)(v & 0xFF));
    emit_byte(fc, (uint8_t)((v >> 8) & 0xFF));
}

static void emit_i16(ChFuncCompiler *fc, int16_t v) {
    emit_u16(fc, (uint16_t)v);
}

static size_t emit_jump(ChFuncCompiler *fc, ChOpCode op) {
    emit_byte(fc, (uint8_t)op);
    size_t at = fc->code_len;
    emit_i16(fc, 0);
    return at;
}

static size_t emit_jump_test(ChFuncCompiler *fc, ChOpCode op, uint8_t test) {
    emit_byte(fc, (uint8_t)op);
    emit_byte(fc, test);
    size_t at = fc->code_len;
    emit_i16(fc, 0);
    return at;
}

static void patch_jump(ChFuncCompiler *fc, size_t at) {
    int32_t offset = (int32_t)fc->code_len - (int32_t)(at + 2);
    if (offset < -32768 || offset > 32767) {
        abort();
    }
    fc->code[at] = (uint8_t)(offset & 0xFF);
    fc->code[at + 1] = (uint8_t)((offset >> 8) & 0xFF);
}

static uint8_t alloc_reg(ChFuncCompiler *fc) {
    if (fc->next_reg >= 250) {
        abort();
    }
    uint8_t r = fc->next_reg++;
    if (r + 1 > fc->max_regs) {
        fc->max_regs = (uint8_t)(r + 1);
    }
    return r;
}

static void reset_regs(ChFuncCompiler *fc, uint8_t saved) {
    fc->next_reg = saved;
}

static void mark_literal_objects(ChValue v) {
    if (!ch_is_pointer(v)) {
        return;
    }
    ChObject *obj = ch_to_object(v);
    switch ((ChObjectTag)obj->tag) {
    case CH_TAG_BYTEVECTOR:
        ch_object_set_immutable(obj, true);
        return;
    case CH_TAG_PAIR: {
        ch_object_set_immutable(obj, true);
        ChPair *pair = (ChPair *)obj;
        mark_literal_objects(pair->car);
        mark_literal_objects(pair->cdr);
        return;
    }
    case CH_TAG_VECTOR: {
        ch_object_set_immutable(obj, true);
        ChVector *vec = (ChVector *)obj;
        for (size_t i = 0; i < vec->len; i++) {
            mark_literal_objects(vec->items[i]);
        }
        return;
    }
    default:
        return;
    }
}

static int add_constant(ChCompiler *c, ChFuncCompiler *fc, ChValue v) {
    mark_literal_objects(v);
    for (size_t i = 0; i < fc->const_count; i++) {
        if (ch_eq(fc->constants[i], v)) {
            return (int)i;
        }
    }
    if (fc->const_count >= CH_MAX_CONSTS) {
        fail(c, "too many constants");
        return -1;
    }
    fc->constants[fc->const_count] = v;
    ch_gc_push(&c->vm->gc, &fc->constants[fc->const_count]);
    fc->const_roots++;
    return (int)fc->const_count++;
}

static int resolve_local(ChFuncCompiler *fc, ChSymbol *name) {
    for (int i = fc->local_count - 1; i >= 0; i--) {
        if (fc->locals[i].name == name) {
            return i;
        }
    }
    return -1;
}

static int add_upvalue(ChFuncCompiler *fc, uint8_t index, bool is_local) {
    for (int i = 0; i < fc->upvalue_count; i++) {
        if (fc->upvalues[i].index == index && fc->upvalues[i].is_local == is_local) {
            return i;
        }
    }
    if (fc->upvalue_count >= CH_MAX_UPVALUES) {
        return -1;
    }
    fc->upvalues[fc->upvalue_count].index = index;
    fc->upvalues[fc->upvalue_count].is_local = is_local;
    return fc->upvalue_count++;
}

static int resolve_upvalue(ChFuncCompiler *fc, ChSymbol *name) {
    if (!fc->enclosing) {
        return -1;
    }
    int local = resolve_local(fc->enclosing, name);
    if (local != -1) {
        fc->enclosing->locals[local].is_captured = true;
        return add_upvalue(fc, (uint8_t)local, true);
    }
    int up = resolve_upvalue(fc->enclosing, name);
    if (up != -1) {
        return add_upvalue(fc, (uint8_t)up, false);
    }
    return -1;
}

static void begin_scope(ChFuncCompiler *fc) {
    fc->scope_depth++;
}

static void end_scope(ChFuncCompiler *fc) {
    while (fc->local_count > 0 && fc->locals[fc->local_count - 1].depth == fc->scope_depth) {
        fc->local_count--;
    }
    fc->scope_depth--;
}

static int add_local(ChCompiler *c, ChFuncCompiler *fc, ChSymbol *name) {
    if (fc->local_count >= CH_MAX_LOCALS) {
        fail(c, "too many locals");
        return -1;
    }
    (void)alloc_reg(fc);
    fc->locals[fc->local_count].name = name;
    fc->locals[fc->local_count].depth = (uint8_t)fc->scope_depth;
    fc->locals[fc->local_count].is_captured = false;
    return fc->local_count++;
}

static uint8_t local_reg(ChFuncCompiler *fc, int local_index) {
    (void)fc;
    return (uint8_t)local_index;
}

static void ensure_temps_from(ChFuncCompiler *fc) {
    if (fc->next_reg < (uint8_t)fc->local_count) {
        fc->next_reg = (uint8_t)fc->local_count;
    }
    if (fc->next_reg > fc->max_regs) {
        fc->max_regs = fc->next_reg;
    }
}

static ChCompileStatus compile_expr(ChCompiler *c, ChFuncCompiler *fc, ChValue expr, uint8_t dst,
                                    bool tail);

typedef struct ChIrLegacyEmitCtx {
    ChCompiler *compiler;
    ChFuncCompiler *fc;
} ChIrLegacyEmitCtx;

static ChCompileStatus emit_ir_with_legacy(void *ctx, ChValue expr, uint8_t dst, bool tail) {
    ChIrLegacyEmitCtx *emit_ctx = (ChIrLegacyEmitCtx *)ctx;
    return compile_expr(emit_ctx->compiler, emit_ctx->fc, expr, dst, tail);
}

static size_t list_length(ChValue v) {
    size_t n = 0;
    while (ch_is_pair(v)) {
        n++;
        v = ch_cdr(v);
    }
    return n;
}

static bool is_symbol_named(ChValue v, const char *name) {
    return ch_is_symbol(v) && strcmp(ch_symbol_basename(ch_as_symbol(v)), name) == 0;
}

static ChCompileStatus compile_quote(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst) {
    if (!ch_is_pair(args) || !ch_is_nil(ch_cdr(args))) {
        return fail(c, "quote: expected one argument");
    }
    int idx = add_constant(c, fc, ch_car(args));
    if (idx < 0) {
        return CH_COMPILE_ERROR;
    }
    emit_byte(fc, CH_OP_LOAD_CONST);
    emit_byte(fc, dst);
    emit_u16(fc, (uint16_t)idx);
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_if(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                  bool tail) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "if: bad syntax");
    }
    ChValue test = ch_car(args);
    ChValue consequent = ch_car(ch_cdr(args));
    ChValue alternate = CH_FALSE;
    ChValue rest = ch_cdr(ch_cdr(args));
    if (ch_is_pair(rest)) {
        alternate = ch_car(rest);
        if (!ch_is_nil(ch_cdr(rest))) {
            return fail(c, "if: too many arguments");
        }
    } else if (!ch_is_nil(rest)) {
        return fail(c, "if: bad syntax");
    }

    ensure_temps_from(fc);
    uint8_t saved = fc->next_reg;
    uint8_t treg = alloc_reg(fc);
    if (compile_expr(c, fc, test, treg, false) != CH_COMPILE_OK) {
        return CH_COMPILE_ERROR;
    }
    size_t jf = emit_jump_test(fc, CH_OP_JUMP_FALSE, treg);
    reset_regs(fc, saved);
    if (compile_expr(c, fc, consequent, dst, tail) != CH_COMPILE_OK) {
        return CH_COMPILE_ERROR;
    }
    size_t jmp_end = emit_jump(fc, CH_OP_JUMP);
    patch_jump(fc, jf);
    if (compile_expr(c, fc, alternate, dst, tail) != CH_COMPILE_OK) {
        return CH_COMPILE_ERROR;
    }
    patch_jump(fc, jmp_end);
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_begin(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                     bool tail) {
    if (ch_is_nil(args)) {
        emit_byte(fc, CH_OP_LOAD_VOID);
        emit_byte(fc, dst);
        return CH_COMPILE_OK;
    }
    while (ch_is_pair(args)) {
        ChValue expr = ch_car(args);
        ChValue next = ch_cdr(args);
        bool last = ch_is_nil(next);
        if (!last && !ch_is_pair(next)) {
            return fail(c, "begin: improper list");
        }
        if (compile_expr(c, fc, expr, dst, tail && last) != CH_COMPILE_OK) {
            return CH_COMPILE_ERROR;
        }
        args = next;
    }
    if (!ch_is_nil(args)) {
        return fail(c, "begin: improper list");
    }
    return CH_COMPILE_OK;
}

typedef struct ChMacroSave {
    ChSymbol *name;
    ChValue old_transformer;
} ChMacroSave;

static ChValue lookup_macro_value(ChVM *vm, ChSymbol *name) {
    const char *base = ch_symbol_basename(name);
    for (size_t i = 0; i < vm->macro_count; i++) {
        if (strcmp(ch_symbol_basename(vm->macros[i].name), base) == 0) {
            return vm->macros[i].transformer;
        }
    }
    return CH_NIL;
}

static void remove_macro_binding(ChVM *vm, ChSymbol *name) {
    const char *base = ch_symbol_basename(name);
    for (size_t i = 0; i < vm->macro_count; i++) {
        if (strcmp(ch_symbol_basename(vm->macros[i].name), base) == 0) {
            vm->macro_count--;
            vm->macros[i] = vm->macros[vm->macro_count];
            return;
        }
    }
}

static void restore_macro_binding(ChVM *vm, const ChMacroSave *save) {
    if (save->old_transformer == CH_NIL) {
        remove_macro_binding(vm, save->name);
    } else {
        ch_vm_define_macro(vm, save->name, ch_as_transformer(save->old_transformer));
    }
}

static ChCompileStatus compile_cond_expand(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                           uint8_t dst, bool tail) {
    ChValue body = CH_NIL;
    char err[256];
    int sel = ch_cond_expand_select(c->vm, args, &body, err, sizeof(err));
    if (sel < 0) {
        return fail(c, err);
    }
    if (sel == 1) {
        emit_byte(fc, CH_OP_LOAD_VOID);
        emit_byte(fc, dst);
        return CH_COMPILE_OK;
    }
    return compile_begin(c, fc, body, dst, tail);
}

static ChCompileStatus compile_define_property(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                               uint8_t dst) {
    if (!fc->is_toplevel || fc->scope_depth > 0) {
        return fail(c, "define-property: not allowed in body");
    }
    if (!ch_is_pair(args) || !ch_is_symbol(ch_car(args)) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "define-property: bad syntax");
    }
    ChSymbol *id = ch_as_symbol(ch_car(args));
    ChValue rest1 = ch_cdr(args);
    if (!ch_is_symbol(ch_car(rest1)) || !ch_is_pair(ch_cdr(rest1)) ||
        !ch_is_nil(ch_cdr(ch_cdr(rest1)))) {
        return fail(c, "define-property: bad syntax");
    }
    ChSymbol *key = ch_as_symbol(ch_car(rest1));
    ChValue expr = ch_car(ch_cdr(rest1));
    ChValue val = CH_VOID;
    ch_gc_push(&c->vm->gc, &val);
    if (ch_eval_datum(c->vm, expr, CH_VOID, &val) != 0) {
        ch_gc_pop(&c->vm->gc);
        return fail(c, c->vm->error);
    }
    if (ch_vm_syntax_property_set(c->vm, ch_symbol_basename(id), ch_symbol_basename(key), val) !=
        0) {
        ch_gc_pop(&c->vm->gc);
        return fail(c, "define-property: property table full");
    }
    ch_gc_pop(&c->vm->gc);
    emit_byte(fc, CH_OP_LOAD_VOID);
    emit_byte(fc, dst);
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_let_syntax(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                        uint8_t dst, bool tail, int letrec) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, letrec ? "letrec-syntax: bad syntax" : "let-syntax: bad syntax");
    }
    ChValue bindings = ch_car(args);
    ChValue body = ch_cdr(args);

    ChMacroSave saves[CH_MAX_DERIVED_BINDINGS];
    int nsaves = 0;
    ChSymbol *names[CH_MAX_DERIVED_BINDINGS];
    ChTransformer *transformers[CH_MAX_DERIVED_BINDINGS];
    int nbinds = 0;

    if (!letrec) {
        for (ChValue bl = bindings; ch_is_pair(bl); bl = ch_cdr(bl)) {
            ChValue bind = ch_car(bl);
            if (!ch_is_pair(bind) || !ch_is_symbol(ch_car(bind)) || !ch_is_pair(ch_cdr(bind))) {
                return fail(c, "let-syntax: bad binding");
            }
            if (nbinds >= CH_MAX_DERIVED_BINDINGS) {
                return fail(c, "let-syntax: too many bindings");
            }
            ChSymbol *kw = ch_as_symbol(ch_car(bind));
            ChValue spec = ch_car(ch_cdr(bind));
            ChTransformer *tr = NULL;
            char err[256];
            if (ch_parse_syntax_rules(c->vm, spec, &tr, err, sizeof(err)) != CH_EXPAND_OK) {
                for (int i = 0; i < nsaves; i++) {
                    restore_macro_binding(c->vm, &saves[i]);
                }
                return fail(c, err);
            }
            saves[nsaves].name = kw;
            saves[nsaves].old_transformer = lookup_macro_value(c->vm, kw);
            nsaves++;
            names[nbinds] = kw;
            transformers[nbinds++] = tr;
        }
        for (int i = 0; i < nbinds; i++) {
            if (ch_vm_define_macro(c->vm, names[i], transformers[i]) != 0) {
                for (int j = 0; j < nsaves; j++) {
                    restore_macro_binding(c->vm, &saves[j]);
                }
                return fail(c, "let-syntax: too many macros");
            }
        }
    } else {
        for (ChValue bl = bindings; ch_is_pair(bl); bl = ch_cdr(bl)) {
            ChValue bind = ch_car(bl);
            if (!ch_is_pair(bind) || !ch_is_symbol(ch_car(bind)) || !ch_is_pair(ch_cdr(bind))) {
                return fail(c, "letrec-syntax: bad binding");
            }
            if (nsaves >= CH_MAX_DERIVED_BINDINGS) {
                return fail(c, "letrec-syntax: too many bindings");
            }
            ChSymbol *kw = ch_as_symbol(ch_car(bind));
            ChValue spec = ch_car(ch_cdr(bind));
            saves[nsaves].name = kw;
            saves[nsaves].old_transformer = lookup_macro_value(c->vm, kw);
            nsaves++;
            ChTransformer *tr = NULL;
            char err[256];
            if (ch_parse_syntax_rules(c->vm, spec, &tr, err, sizeof(err)) != CH_EXPAND_OK) {
                for (int i = 0; i < nsaves; i++) {
                    restore_macro_binding(c->vm, &saves[i]);
                }
                return fail(c, err);
            }
            if (ch_vm_define_macro(c->vm, kw, tr) != 0) {
                for (int i = 0; i < nsaves; i++) {
                    restore_macro_binding(c->vm, &saves[i]);
                }
                return fail(c, "letrec-syntax: too many macros");
            }
        }
    }

    ChCompileStatus st = compile_begin(c, fc, body, dst, tail);
    for (int i = 0; i < nsaves; i++) {
        restore_macro_binding(c->vm, &saves[i]);
    }
    return st;
}

static ChCompileStatus compile_set(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args)) || !ch_is_nil(ch_cdr(ch_cdr(args)))) {
        return fail(c, "set!: bad syntax");
    }
    if (!ch_is_symbol(ch_car(args))) {
        return fail(c, "set!: target must be a symbol");
    }
    ChSymbol *name = ch_as_symbol(ch_car(args));
    ChValue val_expr = ch_car(ch_cdr(args));
    ensure_temps_from(fc);
    uint8_t saved = fc->next_reg;
    uint8_t vreg = alloc_reg(fc);
    if (compile_expr(c, fc, val_expr, vreg, false) != CH_COMPILE_OK) {
        return CH_COMPILE_ERROR;
    }

    int local = resolve_local(fc, name);
    if (local != -1) {
        emit_byte(fc, CH_OP_MOVE);
        emit_byte(fc, local_reg(fc, local));
        emit_byte(fc, vreg);
        emit_byte(fc, CH_OP_MOVE);
        emit_byte(fc, dst);
        emit_byte(fc, vreg);
        reset_regs(fc, saved);
        return CH_COMPILE_OK;
    }
    int up = resolve_upvalue(fc, name);
    if (up != -1) {
        emit_byte(fc, CH_OP_SET_UPVALUE);
        emit_byte(fc, (uint8_t)up);
        emit_byte(fc, vreg);
        emit_byte(fc, CH_OP_MOVE);
        emit_byte(fc, dst);
        emit_byte(fc, vreg);
        reset_regs(fc, saved);
        return CH_COMPILE_OK;
    }
    uint16_t gidx;
    if (c->vm->active_lib_env) {
        int lidx = ch_lib_env_find(c->vm->active_lib_env, name);
        if (lidx >= 0) {
            gidx = (uint16_t)(CH_ENV_LIB_BIT | (unsigned)lidx);
        } else {
            int g = ch_vm_intern_global(c->vm, name);
            gidx = (uint16_t)g;
        }
    } else {
        int g = ch_vm_intern_global(c->vm, name);
        gidx = (uint16_t)g;
    }
    emit_byte(fc, CH_OP_SET_GLOBAL);
    emit_u16(fc, gidx);
    emit_byte(fc, vreg);
    emit_byte(fc, CH_OP_MOVE);
    emit_byte(fc, dst);
    emit_byte(fc, vreg);
    reset_regs(fc, saved);
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_define(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst) {
    if (!ch_is_pair(args)) {
        return fail(c, "define: bad syntax");
    }
    ChValue head = ch_car(args);
    ChValue rest = ch_cdr(args);

    /* (define (f params...) body...) */
    if (ch_is_pair(head)) {
        if (!ch_is_symbol(ch_car(head))) {
            return fail(c, "define: function name must be a symbol");
        }
        ChSymbol *name = ch_as_symbol(ch_car(head));
        ChValue params = ch_cdr(head);
        ChValue lambda_list = ch_gc_cons(&c->vm->gc, params, rest);
        ChValue lambda_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "lambda");
        ChValue lambda_form = ch_gc_cons(&c->vm->gc, lambda_sym, lambda_list);
        ChValue one = ch_gc_cons(&c->vm->gc, lambda_form, CH_NIL);
        ChValue def_args = ch_gc_cons(&c->vm->gc, ch_make_pointer(&name->header), one);
        return compile_define(c, fc, def_args, dst);
    }

    if (!ch_is_symbol(head)) {
        return fail(c, "define: name must be a symbol");
    }
    if (!ch_is_pair(rest) || !ch_is_nil(ch_cdr(rest))) {
        return fail(c, "define: expected single value expression");
    }
    ChSymbol *name = ch_as_symbol(head);
    ChValue val_expr = ch_car(rest);

    if (!fc->is_toplevel && fc->scope_depth > 0) {
        /* local define: add local then assign */
        if (add_local(c, fc, name) < 0) {
            return CH_COMPILE_ERROR;
        }
        ensure_temps_from(fc);
        int local = resolve_local(fc, name);
        if (compile_expr(c, fc, val_expr, local_reg(fc, local), false) != CH_COMPILE_OK) {
            return CH_COMPILE_ERROR;
        }
        emit_byte(fc, CH_OP_MOVE);
        emit_byte(fc, dst);
        emit_byte(fc, local_reg(fc, local));
        return CH_COMPILE_OK;
    }

    ensure_temps_from(fc);
    uint8_t saved = fc->next_reg;
    uint8_t vreg = alloc_reg(fc);
    if (compile_expr(c, fc, val_expr, vreg, false) != CH_COMPILE_OK) {
        return CH_COMPILE_ERROR;
    }
    uint16_t gidx;
    if (c->vm->active_lib_env) {
        int idx = ch_lib_env_intern(c->vm->active_lib_env, name);
        if (idx < 0) {
            return fail(c, "define: library environment full");
        }
        gidx = (uint16_t)(CH_ENV_LIB_BIT | (unsigned)idx);
    } else {
        int g = ch_vm_intern_global(c->vm, name);
        gidx = (uint16_t)g;
    }
    emit_byte(fc, CH_OP_DEFINE_GLOBAL);
    emit_u16(fc, gidx);
    emit_byte(fc, vreg);
    emit_byte(fc, CH_OP_LOAD_VOID);
    emit_byte(fc, dst);
    reset_regs(fc, saved);
    return CH_COMPILE_OK;
}

static ChCompileStatus finish_function(ChCompiler *c, ChFuncCompiler *fc);

static ChCompileStatus compile_lambda(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "lambda: bad syntax");
    }
    ChValue params = ch_car(args);
    ChValue body = ch_cdr(args);

    ChValue fn_v = ch_gc_make_function(&c->vm->gc);
    ChFunction *fn = ch_as_function(fn_v);
    ChFuncCompiler child;
    fc_init(c, &child, fc, fn, false);
    begin_scope(&child);

    /* bind parameters as locals 0.. */
    bool variadic = false;
    ChValue p = params;
    while (ch_is_pair(p)) {
        if (!ch_is_symbol(ch_car(p))) {
            fc_discard(c, &child);
            return fail(c, "lambda: parameter must be a symbol");
        }
        if (add_local(c, &child, ch_as_symbol(ch_car(p))) < 0) {
            fc_discard(c, &child);
            return CH_COMPILE_ERROR;
        }
        p = ch_cdr(p);
    }
    if (ch_is_symbol(p)) {
        variadic = true;
        if (add_local(c, &child, ch_as_symbol(p)) < 0) {
            fc_discard(c, &child);
            return CH_COMPILE_ERROR;
        }
    } else if (!ch_is_nil(p)) {
        fc_discard(c, &child);
        return fail(c, "lambda: bad parameter list");
    }

    fn->arity = (uint8_t)(variadic ? child.local_count - 1 : child.local_count);
    fn->variadic = variadic ? 1 : 0;
    child.next_reg = (uint8_t)child.local_count;
    child.max_regs = child.next_reg;

    ensure_temps_from(&child);
    uint8_t body_dst = alloc_reg(&child);
    if (compile_begin(c, &child, body, body_dst, true) != CH_COMPILE_OK) {
        fc_discard(c, &child);
        return CH_COMPILE_ERROR;
    }
    emit_byte(&child, CH_OP_RETURN);
    emit_byte(&child, body_dst);
    end_scope(&child);

    if (finish_function(c, &child) != CH_COMPILE_OK) {
        fc_discard(c, &child);
        return CH_COMPILE_ERROR;
    }
    /* Constants now live on child.fn; drop temp roots before parent add_constant
     * pushes, otherwise fc_end_compile's pop_const_roots would steal that root. */
    pop_const_roots(c, &child);

    ChValue fn_cv = ch_make_pointer(&child.fn->header);
    ch_gc_push(&c->vm->gc, &fn_cv);
    int idx = add_constant(c, fc, fn_cv);
    ch_gc_pop(&c->vm->gc);
    fc_end_compile(c, &child);
    if (idx < 0) {
        return CH_COMPILE_ERROR;
    }
    emit_byte(fc, CH_OP_CLOSURE);
    emit_byte(fc, dst);
    emit_u16(fc, (uint16_t)idx);
    return CH_COMPILE_OK;
}

/* (case-lambda (formals body...) ...) →
 * (lambda %cl-args
 *   (let ((%cl-n (length %cl-args)))
 *     (cond ((= %cl-n arity) (apply (lambda formals body...) %cl-args)) ...
 *           (else (error "wrong number of arguments"))))) */
static ChCompileStatus compile_case_lambda(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                           uint8_t dst, bool tail) {
    ChGC *gc = &c->vm->gc;
    const int nroots = 7;
    ChValue lambda_sym = ch_gc_intern_symbol_cstr(gc, "lambda");
    ChValue let_sym = ch_gc_intern_symbol_cstr(gc, "let");
    ChValue cond_sym = ch_gc_intern_symbol_cstr(gc, "cond");
    ChValue eq_sym = ch_gc_intern_symbol_cstr(gc, "=");
    ChValue ge_sym = ch_gc_intern_symbol_cstr(gc, ">=");
    ChValue length_sym = ch_gc_intern_symbol_cstr(gc, "length");
    ChValue apply_sym = ch_gc_intern_symbol_cstr(gc, "apply");
    ChValue else_sym = ch_gc_intern_symbol_cstr(gc, "else");
    ChValue error_sym = ch_gc_intern_symbol_cstr(gc, "error");
    ChValue args_sym = ch_gc_intern_symbol_cstr(gc, "%cl-args");
    ChValue n_sym = ch_gc_intern_symbol_cstr(gc, "%cl-n");

    /* Rooted scratch slots — nested cons would otherwise drop live
     * pointers across GC triggered by later allocations. */
    ChValue rev_clauses = CH_NIL;
    ChValue scratch = CH_NIL;
    ChValue piece = CH_NIL;
    ChValue apply_call = CH_NIL;
    ChValue cond_clauses = CH_NIL;
    ChValue outer = CH_NIL;
    ch_gc_push(gc, &rev_clauses);
    ch_gc_push(gc, &scratch);
    ch_gc_push(gc, &piece);
    ch_gc_push(gc, &apply_call);
    ch_gc_push(gc, &cond_clauses);
    ch_gc_push(gc, &outer);
    ch_gc_push(gc, &args);

    for (ChValue cl = args; ch_is_pair(cl); cl = ch_cdr(cl)) {
        ChValue clause = ch_car(cl);
        if (!ch_is_pair(clause) || !ch_is_pair(ch_cdr(clause))) {
            ch_gc_pop_n(gc, nroots);
            return fail(c, "case-lambda: bad clause");
        }
        ChValue formals = ch_car(clause);
        ChValue body = ch_cdr(clause);
        int arity = 0;
        int has_rest = 0;
        for (ChValue f = formals; !ch_is_nil(f);) {
            if (ch_is_pair(f)) {
                if (!ch_is_symbol(ch_car(f))) {
                    ch_gc_pop_n(gc, nroots);
                    return fail(c, "case-lambda: bad formals");
                }
                arity++;
                f = ch_cdr(f);
            } else if (ch_is_symbol(f)) {
                has_rest = 1;
                break;
            } else {
                ch_gc_pop_n(gc, nroots);
                return fail(c, "case-lambda: bad formals");
            }
        }
        ChValue cmp = has_rest ? ge_sym : eq_sym;
        ChValue arity_v = ch_make_fixnum(arity);
        /* scratch = (cmp n arity) */
        scratch = ch_gc_cons(gc, arity_v, CH_NIL);
        scratch = ch_gc_cons(gc, n_sym, scratch);
        scratch = ch_gc_cons(gc, cmp, scratch);
        /* piece = (lambda formals . body) */
        piece = ch_gc_cons(gc, formals, body);
        piece = ch_gc_cons(gc, lambda_sym, piece);
        /* apply_call = (apply piece args) */
        apply_call = ch_gc_cons(gc, args_sym, CH_NIL);
        apply_call = ch_gc_cons(gc, piece, apply_call);
        apply_call = ch_gc_cons(gc, apply_sym, apply_call);
        /* piece = (scratch apply_call) */
        piece = ch_gc_cons(gc, apply_call, CH_NIL);
        piece = ch_gc_cons(gc, scratch, piece);
        rev_clauses = ch_gc_cons(gc, piece, rev_clauses);
    }

    /* else clause: (else (error "wrong number of arguments")) */
    scratch = ch_gc_make_string_cstr(gc, "wrong number of arguments");
    scratch = ch_gc_cons(gc, scratch, CH_NIL);
    scratch = ch_gc_cons(gc, error_sym, scratch);
    scratch = ch_gc_cons(gc, scratch, CH_NIL);
    scratch = ch_gc_cons(gc, else_sym, scratch);
    cond_clauses = ch_gc_cons(gc, scratch, CH_NIL);
    while (ch_is_pair(rev_clauses)) {
        cond_clauses = ch_gc_cons(gc, ch_car(rev_clauses), cond_clauses);
        rev_clauses = ch_cdr(rev_clauses);
    }

    /* (lambda %cl-args (let ((%cl-n (length %cl-args))) (cond ...))) */
    scratch = ch_gc_cons(gc, cond_sym, cond_clauses);
    piece = ch_gc_cons(gc, args_sym, CH_NIL);
    piece = ch_gc_cons(gc, length_sym, piece);
    piece = ch_gc_cons(gc, piece, CH_NIL);
    piece = ch_gc_cons(gc, n_sym, piece);
    piece = ch_gc_cons(gc, piece, CH_NIL);
    scratch = ch_gc_cons(gc, scratch, CH_NIL);
    scratch = ch_gc_cons(gc, piece, scratch);
    scratch = ch_gc_cons(gc, let_sym, scratch);
    scratch = ch_gc_cons(gc, scratch, CH_NIL);
    scratch = ch_gc_cons(gc, args_sym, scratch);
    outer = ch_gc_cons(gc, lambda_sym, scratch);
    ch_gc_pop_n(gc, nroots);
    return compile_expr(c, fc, outer, dst, tail);
}

static ChCompileStatus compile_delay(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                     bool tail) {
    if (!ch_is_pair(args)) {
        return fail(c, "delay: bad syntax");
    }
    ChGC *gc = &c->vm->gc;
    ChValue lambda_sym = ch_gc_intern_symbol_cstr(gc, "lambda");
    ChValue make_sym = ch_gc_intern_symbol_cstr(gc, "%make-promise");
    /* (delay e1 e2 ...) → (%make-promise (lambda () e1 e2 ...)) */
    ChValue form = CH_NIL;
    ch_gc_push(gc, &form);
    ch_gc_push(gc, &args);
    form = ch_gc_cons(gc, CH_NIL, args);
    form = ch_gc_cons(gc, lambda_sym, form);
    form = ch_gc_cons(gc, form, CH_NIL);
    form = ch_gc_cons(gc, make_sym, form);
    ch_gc_pop_n(gc, 2);
    return compile_expr(c, fc, form, dst, tail);
}

static ChCompileStatus compile_delay_force(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                           uint8_t dst, bool tail) {
    /* delay-force: same lowering as delay; force iterates promises (prim_force). */
    return compile_delay(c, fc, args, dst, tail);
}

static int parse_define_values_formals(ChValue formals, ChSymbol **names, int max_names,
                                       ChSymbol **rest_name) {
    *rest_name = NULL;
    if (ch_is_nil(formals)) {
        return 0;
    }
    if (ch_is_symbol(formals)) {
        *rest_name = ch_as_symbol(formals);
        return 0;
    }
    int count = 0;
    for (ChValue f = formals; !ch_is_nil(f);) {
        if (ch_is_symbol(f)) {
            *rest_name = ch_as_symbol(f);
            break;
        }
        if (!ch_is_pair(f) || !ch_is_symbol(ch_car(f))) {
            return -1;
        }
        if (count >= max_names) {
            return -1;
        }
        names[count++] = ch_as_symbol(ch_car(f));
        f = ch_cdr(f);
    }
    return count;
}

static ChValue build_void_define_args(ChGC *gc, ChSymbol *name) {
    ChValue val = ch_gc_cons(gc, CH_FALSE, CH_NIL);
    val = ch_gc_cons(gc, CH_FALSE, val);
    ChValue if_sym = ch_gc_intern_symbol_cstr(gc, "if");
    val = ch_gc_cons(gc, if_sym, val);
    ChValue one = ch_gc_cons(gc, val, CH_NIL);
    return ch_gc_cons(gc, ch_make_pointer(&name->header), one);
}

static ChValue build_list_ref(ChGC *gc, ChValue list_expr, int index) {
    ChValue cur = list_expr;
    ChValue cdr_sym = ch_gc_intern_symbol_cstr(gc, "cdr");
    for (int j = 0; j < index; j++) {
        cur = ch_gc_cons(gc, cdr_sym, ch_gc_cons(gc, cur, CH_NIL));
    }
    ChValue car_sym = ch_gc_intern_symbol_cstr(gc, "car");
    return ch_gc_cons(gc, car_sym, ch_gc_cons(gc, cur, CH_NIL));
}

/* Build assignment for top-level define-values; names must already be bound. */
static ChCompileStatus build_define_values_assign_form(ChCompiler *c, ChSymbol **names, int name_count,
                                                       ChSymbol *rest_name, ChValue expr,
                                                       ChValue *out) {
    ChGC *gc = &c->vm->gc;
    ChValue lambda_sym = ch_gc_intern_symbol_cstr(gc, "lambda");
    ChValue cwv_sym = ch_gc_intern_symbol_cstr(gc, "call-with-values");
    ChValue list_sym = ch_gc_intern_symbol_cstr(gc, "list");
    ChValue set_sym = ch_gc_intern_symbol_cstr(gc, "set!");

    if (name_count == 0 && rest_name == NULL) {
        *out = expr;
        return CH_COMPILE_OK;
    }

    if (name_count == 0 && rest_name != NULL) {
        /* (define-values x expr) → (set! x (call-with-values (lambda () expr) list)) */
        ChValue producer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, expr, CH_NIL)));
        ChValue cwv = ch_gc_cons(gc, cwv_sym,
                                 ch_gc_cons(gc, producer, ch_gc_cons(gc, list_sym, CH_NIL)));
        *out = ch_gc_cons(gc, set_sym,
                          ch_gc_cons(gc, ch_make_pointer(&rest_name->header), ch_gc_cons(gc, cwv, CH_NIL)));
        return CH_COMPILE_OK;
    }

    ChValue consumer_body = CH_NIL;
    ch_gc_push(gc, &consumer_body);

    if (rest_name != NULL && name_count > 0) {
        ChSymbol *param = ch_as_symbol(ch_gc_intern_symbol_cstr(gc, "__dv_rest"));
        ChValue set_form = ch_gc_cons(
            gc, set_sym,
            ch_gc_cons(gc, ch_make_pointer(&rest_name->header),
                       ch_gc_cons(gc, ch_make_pointer(&param->header), CH_NIL)));
        consumer_body = ch_gc_cons(gc, set_form, consumer_body);
    }

    for (int i = name_count - 1; i >= 0; i--) {
        char buf[32];
        snprintf(buf, sizeof(buf), "__dv_%d", i);
        ChSymbol *param = ch_as_symbol(ch_gc_intern_symbol_cstr(gc, buf));
        ChValue set_form =
            ch_gc_cons(gc, set_sym,
                       ch_gc_cons(gc, ch_make_pointer(&names[i]->header),
                                  ch_gc_cons(gc, ch_make_pointer(&param->header), CH_NIL)));
        consumer_body = ch_gc_cons(gc, set_form, consumer_body);
    }

    ChValue consumer_params = CH_NIL;
    if (rest_name != NULL && name_count > 0) {
        ChSymbol *rest_param = ch_as_symbol(ch_gc_intern_symbol_cstr(gc, "__dv_rest"));
        consumer_params = ch_make_pointer(&rest_param->header);
    }
    for (int i = name_count - 1; i >= 0; i--) {
        char buf[32];
        snprintf(buf, sizeof(buf), "__dv_%d", i);
        ChSymbol *param = ch_as_symbol(ch_gc_intern_symbol_cstr(gc, buf));
        consumer_params = ch_gc_cons(gc, ch_make_pointer(&param->header), consumer_params);
    }

    ChValue consumer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, consumer_params, consumer_body));
    ChValue producer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, expr, CH_NIL)));
    *out = ch_gc_cons(gc, cwv_sym, ch_gc_cons(gc, producer, ch_gc_cons(gc, consumer, CH_NIL)));
    ch_gc_pop(gc);
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_define_values_local_assign(ChCompiler *c, ChFuncCompiler *fc,
                                                          ChSymbol **names, int name_count,
                                                          ChSymbol *rest_name, ChValue expr,
                                                          uint8_t dst) {
    ChGC *gc = &c->vm->gc;

    if (name_count == 0 && rest_name == NULL) {
        return compile_expr(c, fc, expr, dst, false);
    }

    if (name_count == 0 && rest_name != NULL) {
        ChValue lambda_sym = ch_gc_intern_symbol_cstr(gc, "lambda");
        ChValue cwv_sym = ch_gc_intern_symbol_cstr(gc, "call-with-values");
        ChValue list_sym = ch_gc_intern_symbol_cstr(gc, "list");
        ChValue set_sym = ch_gc_intern_symbol_cstr(gc, "set!");
        ChValue producer =
            ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, expr, CH_NIL)));
        ChValue cwv =
            ch_gc_cons(gc, cwv_sym, ch_gc_cons(gc, producer, ch_gc_cons(gc, list_sym, CH_NIL)));
        ChValue assign = ch_gc_cons(
            gc, set_sym,
            ch_gc_cons(gc, ch_make_pointer(&rest_name->header), ch_gc_cons(gc, cwv, CH_NIL)));
        return compile_expr(c, fc, assign, dst, false);
    }

    /* Multi-name locals: evaluate to a temp list, then store into each local
     * register directly (same-scope multi-set! on define locals is broken). */
    ChSymbol *tmp = ch_as_symbol(ch_gc_intern_symbol_cstr(gc, "__dv_tmp"));
    if (add_local(c, fc, tmp) < 0) {
        return CH_COMPILE_ERROR;
    }
    int dv_idx = resolve_local(fc, tmp);

    ChValue lambda_sym = ch_gc_intern_symbol_cstr(gc, "lambda");
    ChValue cwv_sym = ch_gc_intern_symbol_cstr(gc, "call-with-values");
    ChValue list_sym = ch_gc_intern_symbol_cstr(gc, "list");
    ChValue producer =
        ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, expr, CH_NIL)));
    ChValue cwv =
        ch_gc_cons(gc, cwv_sym, ch_gc_cons(gc, producer, ch_gc_cons(gc, list_sym, CH_NIL)));
    if (compile_expr(c, fc, cwv, local_reg(fc, dv_idx), false) != CH_COMPILE_OK) {
        return CH_COMPILE_ERROR;
    }

    ChValue dv_v = ch_make_pointer(&tmp->header);
    for (int i = 0; i < name_count; i++) {
        int li = resolve_local(fc, names[i]);
        if (li < 0) {
            return fail(c, "define-values: internal error");
        }
        ChValue ref = build_list_ref(gc, dv_v, i);
        if (compile_expr(c, fc, ref, local_reg(fc, li), false) != CH_COMPILE_OK) {
            return CH_COMPILE_ERROR;
        }
    }
    if (rest_name != NULL) {
        int ri = resolve_local(fc, rest_name);
        if (ri < 0) {
            return fail(c, "define-values: internal error");
        }
        ChValue tail_sym = ch_gc_intern_symbol_cstr(gc, "list-tail");
        ChValue idx = ch_make_fixnum(name_count);
        ChValue tail_call =
            ch_gc_cons(gc, tail_sym, ch_gc_cons(gc, dv_v, ch_gc_cons(gc, idx, CH_NIL)));
        if (compile_expr(c, fc, tail_call, local_reg(fc, ri), false) != CH_COMPILE_OK) {
            return CH_COMPILE_ERROR;
        }
    }

    /* Avoid clobbering a local slot when begin reuses dst for this form. */
    if ((int)dst >= fc->local_count) {
        emit_byte(fc, CH_OP_LOAD_VOID);
        emit_byte(fc, dst);
    }
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_define_values(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                             uint8_t dst) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args)) || !ch_is_nil(ch_cdr(ch_cdr(args)))) {
        return fail(c, "define-values: bad syntax");
    }
    ChValue formals = ch_car(args);
    ChValue expr = ch_car(ch_cdr(args));

    ChSymbol *names[64];
    ChSymbol *rest_name = NULL;
    int name_count = parse_define_values_formals(formals, names, 64, &rest_name);
    if (name_count < 0) {
        return fail(c, "define-values: bad formals");
    }

    bool local = !fc->is_toplevel && fc->scope_depth > 0;
    for (int i = 0; i < name_count; i++) {
        if (local) {
            if (add_local(c, fc, names[i]) < 0) {
                return CH_COMPILE_ERROR;
            }
        } else {
            ChValue def_args = build_void_define_args(&c->vm->gc, names[i]);
            if (compile_define(c, fc, def_args, dst) != CH_COMPILE_OK) {
                return CH_COMPILE_ERROR;
            }
        }
    }
    if (rest_name != NULL) {
        if (local) {
            if (add_local(c, fc, rest_name) < 0) {
                return CH_COMPILE_ERROR;
            }
        } else {
            ChValue def_args = build_void_define_args(&c->vm->gc, rest_name);
            if (compile_define(c, fc, def_args, dst) != CH_COMPILE_OK) {
                return CH_COMPILE_ERROR;
            }
        }
    }

    if (local) {
        return compile_define_values_local_assign(c, fc, names, name_count, rest_name, expr, dst);
    }

    ChValue assign = CH_NIL;
    ch_gc_push(&c->vm->gc, &assign);
    if (build_define_values_assign_form(c, names, name_count, rest_name, expr, &assign) !=
        CH_COMPILE_OK) {
        ch_gc_pop(&c->vm->gc);
        return CH_COMPILE_ERROR;
    }
    ChCompileStatus st = compile_expr(c, fc, assign, dst, false);
    ch_gc_pop(&c->vm->gc);
    if (st != CH_COMPILE_OK) {
        return st;
    }
    emit_byte(fc, CH_OP_LOAD_VOID);
    emit_byte(fc, dst);
    return CH_COMPILE_OK;
}

/* (let-values (((a b) e) ...) body...) — all producers in outer scope (R7RS §4.2.2). */
static ChCompileStatus compile_let_values(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                          uint8_t dst, bool tail) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "let-values: bad syntax");
    }
    ChValue bindings = ch_car(args);
    ChValue body = ch_cdr(args);

    ChGC *gc = &c->vm->gc;
    ChValue lambda_sym = ch_gc_intern_symbol_cstr(gc, "lambda");
    ChValue apply_sym = ch_gc_intern_symbol_cstr(gc, "apply");
    ChValue let_sym = ch_gc_intern_symbol_cstr(gc, "let");
    ChValue begin_sym = ch_gc_intern_symbol_cstr(gc, "begin");
    ChValue cwv_sym = ch_gc_intern_symbol_cstr(gc, "call-with-values");
    ChValue list_sym = ch_gc_intern_symbol_cstr(gc, "list");

    ChValue formals_arr[64];
    ChValue exprs[64];
    ChValue temp_syms[64];
    int count = 0;

    for (ChValue bl = bindings; ch_is_pair(bl); bl = ch_cdr(bl)) {
        ChValue binding = ch_car(bl);
        if (!ch_is_pair(binding) || !ch_is_pair(ch_cdr(binding))) {
            return fail(c, "let-values: bad binding");
        }
        if (count >= 64) {
            return fail(c, "let-values: too many bindings");
        }
        formals_arr[count] = ch_car(binding);
        exprs[count] = ch_car(ch_cdr(binding));
        let_values_gensym_counter++;
        char buf[64];
        snprintf(buf, sizeof(buf), "__lv_temp_%zu", let_values_gensym_counter);
        temp_syms[count] = ch_gc_intern_symbol_cstr(gc, buf);
        count++;
    }
    if (!ch_is_nil(bindings) && !ch_is_pair(bindings)) {
        return fail(c, "let-values: bad bindings list");
    }

    ChValue inner = CH_NIL;
    ch_gc_push(gc, &inner);
    if (ch_is_nil(body)) {
        inner = CH_VOID;
    } else if (ch_is_nil(ch_cdr(body))) {
        inner = ch_car(body);
    } else {
        inner = ch_gc_cons(gc, begin_sym, body);
    }

    for (int i = count - 1; i >= 0; i--) {
        ChValue consumer_body = ch_gc_cons(gc, inner, CH_NIL);
        ChValue consumer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, formals_arr[i], consumer_body));
        inner = ch_gc_cons(gc, apply_sym, ch_gc_cons(gc, consumer, ch_gc_cons(gc, temp_syms[i], CH_NIL)));
    }

    ChValue let_binds = CH_NIL;
    ch_gc_push(gc, &let_binds);
    for (int i = count - 1; i >= 0; i--) {
        ChValue producer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, exprs[i], CH_NIL)));
        ChValue cwv = ch_gc_cons(gc, cwv_sym, ch_gc_cons(gc, producer, ch_gc_cons(gc, list_sym, CH_NIL)));
        ChValue bind = ch_gc_cons(gc, temp_syms[i], ch_gc_cons(gc, cwv, CH_NIL));
        let_binds = ch_gc_cons(gc, bind, let_binds);
    }

    ChValue form = CH_NIL;
    ch_gc_push(gc, &form);
    if (count == 0) {
        form = ch_gc_cons(gc, let_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, inner, CH_NIL)));
    } else {
        form = ch_gc_cons(gc, let_sym, ch_gc_cons(gc, let_binds, ch_gc_cons(gc, inner, CH_NIL)));
    }
    ChValue form_keep = form;
    ch_gc_pop_n(gc, 3);
    return compile_expr(c, fc, form_keep, dst, tail);
}

/* Build nested call-with-values for sequential let*-values. */
static ChCompileStatus build_let_star_values(ChCompiler *c, ChValue bindings, ChValue body,
                                             ChValue *out) {
    ChGC *gc = &c->vm->gc;
    ChValue lambda_sym = ch_gc_intern_symbol_cstr(gc, "lambda");
    ChValue cwv_sym = ch_gc_intern_symbol_cstr(gc, "call-with-values");
    ChValue begin_sym = ch_gc_intern_symbol_cstr(gc, "begin");

    if (ch_is_nil(bindings)) {
        ChValue let_sym = ch_gc_intern_symbol_cstr(gc, "let");
        ChValue inner;
        if (ch_is_nil(body)) {
            inner = CH_VOID;
        } else if (ch_is_nil(ch_cdr(body))) {
            inner = ch_car(body);
        } else {
            inner = ch_gc_cons(gc, begin_sym, body);
        }
        /* Empty let*-values still introduces a body scope for internal define. */
        *out = ch_gc_cons(gc, let_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, inner, CH_NIL)));
        return CH_COMPILE_OK;
    }
    if (!ch_is_pair(bindings)) {
        return fail(c, "let*-values: bad bindings");
    }

    ChValue binding = ch_car(bindings);
    ChValue rest = ch_cdr(bindings);
    if (!ch_is_pair(binding) || !ch_is_pair(ch_cdr(binding))) {
        return fail(c, "let*-values: bad binding");
    }
    ChValue formals = ch_car(binding);
    ChValue expr = ch_car(ch_cdr(binding));

    ChValue inner = CH_NIL;
    ch_gc_push(gc, &inner);
    if (build_let_star_values(c, rest, body, &inner) != CH_COMPILE_OK) {
        ch_gc_pop(gc);
        return CH_COMPILE_ERROR;
    }

    ChValue producer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, expr, CH_NIL)));
    ChValue consumer_body = ch_gc_cons(gc, inner, CH_NIL);
    ChValue consumer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, formals, consumer_body));
    *out = ch_gc_cons(gc, cwv_sym, ch_gc_cons(gc, producer, ch_gc_cons(gc, consumer, CH_NIL)));
    ch_gc_pop(gc);
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_let_star_values(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                               uint8_t dst, bool tail) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "let*-values: bad syntax");
    }
    ChValue bindings = ch_car(args);
    ChValue body = ch_cdr(args);

    ChValue form = CH_NIL;
    ch_gc_push(&c->vm->gc, &form);
    if (build_let_star_values(c, bindings, body, &form) != CH_COMPILE_OK) {
        ch_gc_pop(&c->vm->gc);
        return CH_COMPILE_ERROR;
    }
    ChCompileStatus st = compile_expr(c, fc, form, dst, tail);
    ch_gc_pop(&c->vm->gc);
    return st;
}

static ChCompileStatus finish_function(ChCompiler *c, ChFuncCompiler *fc) {
    ChFunction *fn = fc->fn;
    fn->code = fc->code;
    fn->code_len = fc->code_len;
    fc->code = NULL;
    fn->num_regs = fc->max_regs;
    fn->num_upvalues = (uint8_t)fc->upvalue_count;
    fn->const_count = fc->const_count;
    if (fc->const_count > 0) {
        fn->constants = (ChValue *)malloc(sizeof(ChValue) * fc->const_count);
        if (!fn->constants) {
            abort();
        }
        memcpy(fn->constants, fc->constants, sizeof(ChValue) * fc->const_count);
    }
    if (fc->upvalue_count > 0) {
        fn->uv_is_local = (uint8_t *)malloc((size_t)fc->upvalue_count);
        fn->uv_index = (uint8_t *)malloc((size_t)fc->upvalue_count);
        if (!fn->uv_is_local || !fn->uv_index) {
            abort();
        }
        for (int i = 0; i < fc->upvalue_count; i++) {
            fn->uv_is_local[i] = fc->upvalues[i].is_local ? 1 : 0;
            fn->uv_index[i] = fc->upvalues[i].index;
        }
    }
    ch_gc_promote_to_old(&c->vm->gc, &fn->header);
    /* const_roots are popped in fc_end_compile once this function is wired into its parent. */
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_call(ChCompiler *c, ChFuncCompiler *fc, ChValue expr, uint8_t dst,
                                    bool tail) {
    ChValue callee = ch_car(expr);
    ChValue args = ch_cdr(expr);
    size_t nargs = list_length(args);
    if (nargs > 200) {
        return fail(c, "too many arguments");
    }

    ensure_temps_from(fc);
    uint8_t saved = fc->next_reg;
    uint8_t base = alloc_reg(fc); /* callee */
    for (size_t i = 0; i < nargs; i++) {
        alloc_reg(fc); /* arg slots */
    }

    if (compile_expr(c, fc, callee, base, false) != CH_COMPILE_OK) {
        return CH_COMPILE_ERROR;
    }
    ChValue a = args;
    for (size_t i = 0; i < nargs; i++) {
        if (compile_expr(c, fc, ch_car(a), (uint8_t)(base + 1 + i), false) != CH_COMPILE_OK) {
            return CH_COMPILE_ERROR;
        }
        a = ch_cdr(a);
    }

    if (tail) {
        emit_byte(fc, CH_OP_TAIL_CALL);
        emit_byte(fc, base);
        emit_byte(fc, (uint8_t)nargs);
    } else {
        emit_byte(fc, CH_OP_CALL);
        emit_byte(fc, base);
        emit_byte(fc, (uint8_t)nargs);
        if (dst != base) {
            emit_byte(fc, CH_OP_MOVE);
            emit_byte(fc, dst);
            emit_byte(fc, base);
        }
    }
    reset_regs(fc, saved);
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_variable(ChCompiler *c, ChFuncCompiler *fc, ChSymbol *name,
                                        uint8_t dst) {
    int local = resolve_local(fc, name);
    if (local != -1) {
        emit_byte(fc, CH_OP_MOVE);
        emit_byte(fc, dst);
        emit_byte(fc, local_reg(fc, local));
        return CH_COMPILE_OK;
    }
    int up = resolve_upvalue(fc, name);
    if (up != -1) {
        emit_byte(fc, CH_OP_GET_UPVALUE);
        emit_byte(fc, dst);
        emit_byte(fc, (uint8_t)up);
        return CH_COMPILE_OK;
    }
    if (c->vm->active_lib_env) {
        int lidx = ch_lib_env_find(c->vm->active_lib_env, name);
        if (lidx < 0) {
            /* try basename match in lib env */
            const char *base = ch_symbol_basename(name);
            for (size_t i = 0; i < c->vm->active_lib_env->count; i++) {
                if (strcmp(ch_symbol_basename(c->vm->active_lib_env->bindings[i].name), base) ==
                    0) {
                    lidx = (int)i;
                    break;
                }
            }
        }
        if (lidx < 0) {
            /* Forward refs within the library body; fall through if this names a VM global. */
            int gchk = ch_vm_intern_global(c->vm, name);
            const char *base = ch_symbol_basename(name);
            ChValue base_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, base);
            int gbase = ch_vm_intern_global(c->vm, ch_as_symbol(base_sym));
            if ((gchk >= 0 && c->vm->globals[gchk].defined) ||
                (gbase >= 0 && c->vm->globals[gbase].defined)) {
                lidx = -1;
            } else {
                lidx = ch_lib_env_intern(c->vm->active_lib_env, name);
                if (lidx < 0) {
                    return fail(c, "compile: library environment full");
                }
            }
        }
        if (lidx >= 0) {
            emit_byte(fc, CH_OP_GET_GLOBAL);
            emit_byte(fc, dst);
            emit_u16(fc, (uint16_t)(CH_ENV_LIB_BIT | (unsigned)lidx));
            return CH_COMPILE_OK;
        }
    }
    /* Hygienic renames: exact symbol global if bound during macro expansion. */
    int g = ch_vm_intern_global(c->vm, name);
    if (g >= 0 && c->vm->globals[g].defined) {
        emit_byte(fc, CH_OP_GET_GLOBAL);
        emit_byte(fc, dst);
        emit_u16(fc, (uint16_t)g);
        return CH_COMPILE_OK;
    }
    /* Fall back to basename global. */
    const char *base = ch_symbol_basename(name);
    ChValue base_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, base);
    g = ch_vm_intern_global(c->vm, ch_as_symbol(base_sym));
    emit_byte(fc, CH_OP_GET_GLOBAL);
    emit_byte(fc, dst);
    emit_u16(fc, (uint16_t)g);
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_and(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                   bool tail) {
    if (ch_is_nil(args)) {
        emit_byte(fc, CH_OP_LOAD_TRUE);
        emit_byte(fc, dst);
        return CH_COMPILE_OK;
    }
    size_t jumps[64];
    size_t nj = 0;
    while (ch_is_pair(args)) {
        ChValue expr = ch_car(args);
        ChValue next = ch_cdr(args);
        bool last = ch_is_nil(next);
        if (compile_expr(c, fc, expr, dst, tail && last) != CH_COMPILE_OK) {
            return CH_COMPILE_ERROR;
        }
        if (!last) {
            if (nj >= 64) {
                return fail(c, "and: too many forms");
            }
            jumps[nj++] = emit_jump_test(fc, CH_OP_JUMP_FALSE, dst);
        }
        args = next;
    }
    for (size_t i = 0; i < nj; i++) {
        patch_jump(fc, jumps[i]);
    }
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_or(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                  bool tail) {
    if (ch_is_nil(args)) {
        emit_byte(fc, CH_OP_LOAD_FALSE);
        emit_byte(fc, dst);
        return CH_COMPILE_OK;
    }
    size_t jumps[64];
    size_t nj = 0;
    while (ch_is_pair(args)) {
        ChValue expr = ch_car(args);
        ChValue next = ch_cdr(args);
        bool last = ch_is_nil(next);
        if (compile_expr(c, fc, expr, dst, tail && last) != CH_COMPILE_OK) {
            return CH_COMPILE_ERROR;
        }
        if (!last) {
            if (nj >= 64) {
                return fail(c, "or: too many forms");
            }
            jumps[nj++] = emit_jump_test(fc, CH_OP_JUMP_TRUE, dst);
        }
        args = next;
    }
    for (size_t i = 0; i < nj; i++) {
        patch_jump(fc, jumps[i]);
    }
    return CH_COMPILE_OK;
}

/* (let ((v e) ...) body...) → ((lambda (v ...) body...) e ...) */
static bool is_define_form(ChValue expr) {
    return ch_is_pair(expr) && ch_is_symbol(ch_car(expr)) &&
           strcmp(ch_symbol_basename(ch_as_symbol(ch_car(expr))), "define") == 0;
}

static ChCompileStatus compile_let(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                   bool tail) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "let: bad syntax");
    }
    /* named let: (let name ((v e)...) body...) */
    if (ch_is_symbol(ch_car(args))) {
        ChValue name = ch_car(args);
        ChValue rest = ch_cdr(args);
        if (!ch_is_pair(rest) || !ch_is_pair(ch_cdr(rest))) {
            return fail(c, "let: bad named let");
        }
        ChValue bindings = ch_car(rest);
        ChValue body = ch_cdr(rest);
        ChValue params = CH_NIL;
        ChValue vals = CH_NIL;
        ch_gc_push(&c->vm->gc, &params);
        ch_gc_push(&c->vm->gc, &vals);
        for (ChValue b = bindings; ch_is_pair(b); b = ch_cdr(b)) {
            ChValue bind = ch_car(b);
            if (!ch_is_pair(bind) || !ch_is_pair(ch_cdr(bind))) {
                ch_gc_pop_n(&c->vm->gc, 2);
                return fail(c, "let: bad binding");
            }
            params = ch_gc_cons(&c->vm->gc, ch_car(bind), params);
            vals = ch_gc_cons(&c->vm->gc, ch_car(ch_cdr(bind)), vals);
        }
        ChValue rp = CH_NIL;
        ChValue rv = CH_NIL;
        ch_gc_push(&c->vm->gc, &rp);
        ch_gc_push(&c->vm->gc, &rv);
        while (ch_is_pair(params)) {
            rp = ch_gc_cons(&c->vm->gc, ch_car(params), rp);
            params = ch_cdr(params);
        }
        while (ch_is_pair(vals)) {
            rv = ch_gc_cons(&c->vm->gc, ch_car(vals), rv);
            vals = ch_cdr(vals);
        }
        /* (letrec ((name (lambda params body...))) (name vals...)) */
        ChValue lambda_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "lambda");
        ChValue letrec_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "letrec");
        ChValue lam = ch_gc_cons(&c->vm->gc, lambda_sym, ch_gc_cons(&c->vm->gc, rp, body));
        ChValue bind1 = ch_gc_cons(&c->vm->gc, name, ch_gc_cons(&c->vm->gc, lam, CH_NIL));
        ChValue binds = ch_gc_cons(&c->vm->gc, bind1, CH_NIL);
        ChValue call = ch_gc_cons(&c->vm->gc, name, rv);
        ChValue body1 = ch_gc_cons(&c->vm->gc, call, CH_NIL);
        ChValue form = ch_gc_cons(&c->vm->gc, letrec_sym,
                                  ch_gc_cons(&c->vm->gc, binds, body1));
        ch_gc_pop_n(&c->vm->gc, 4);
        return compile_expr(c, fc, form, dst, tail);
    }

    ChValue bindings = ch_car(args);
    ChValue body = ch_cdr(args);
    ch_gc_push(&c->vm->gc, &bindings);
    ch_gc_push(&c->vm->gc, &body);

    /* R7RS: leading internal define in let → nested letrec*. */
    if (ch_is_pair(body) && is_define_form(ch_car(body))) {
        ChValue defs = CH_NIL;
        ch_gc_push(&c->vm->gc, &defs);
        while (ch_is_pair(body) && is_define_form(ch_car(body))) {
            ChValue def = ch_car(body);
            ChValue drest = ch_cdr(def);
            if (!ch_is_pair(drest)) {
                ch_gc_pop_n(&c->vm->gc, 3);
                return fail(c, "define: bad syntax");
            }
            ChValue name_form = ch_car(drest);
            ChValue init_forms = ch_cdr(drest);
            ChSymbol *name = NULL;
            ChValue init = CH_NIL;
            if (ch_is_symbol(name_form)) {
                name = ch_as_symbol(name_form);
                if (!ch_is_pair(init_forms) || !ch_is_nil(ch_cdr(init_forms))) {
                    ch_gc_pop(&c->vm->gc);
                    return fail(c, "define: bad syntax");
                }
                init = ch_car(init_forms);
            } else if (ch_is_pair(name_form) && ch_is_symbol(ch_car(name_form))) {
                name = ch_as_symbol(ch_car(name_form));
                ChValue lambda_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "lambda");
                init = ch_gc_cons(&c->vm->gc, lambda_sym,
                                  ch_gc_cons(&c->vm->gc, ch_cdr(name_form), init_forms));
            } else {
                ch_gc_pop_n(&c->vm->gc, 3);
                return fail(c, "define: bad syntax");
            }
            ChValue bind = ch_gc_cons(&c->vm->gc, ch_make_pointer(&name->header),
                                      ch_gc_cons(&c->vm->gc, init, CH_NIL));
            defs = ch_gc_cons(&c->vm->gc, bind, defs);
            body = ch_cdr(body);
        }
        ChValue rdefs = CH_NIL;
        ch_gc_push(&c->vm->gc, &rdefs);
        while (ch_is_pair(defs)) {
            rdefs = ch_gc_cons(&c->vm->gc, ch_car(defs), rdefs);
            defs = ch_cdr(defs);
        }
        ChValue letrec_star = ch_gc_intern_symbol_cstr(&c->vm->gc, "letrec*");
        ChValue inner = ch_gc_cons(&c->vm->gc, letrec_star, ch_gc_cons(&c->vm->gc, rdefs, body));
        ChValue let_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let");
        ChValue form = ch_gc_cons(&c->vm->gc, let_sym,
                                  ch_gc_cons(&c->vm->gc, bindings,
                                             ch_gc_cons(&c->vm->gc, inner, CH_NIL)));
        ch_gc_pop_n(&c->vm->gc, 4);
        return compile_expr(c, fc, form, dst, tail);
    }

    ChValue params = CH_NIL;
    ChValue vals = CH_NIL;
    ch_gc_push(&c->vm->gc, &params);
    ch_gc_push(&c->vm->gc, &vals);

    ChValue b = bindings;
    while (ch_is_pair(b)) {
        ChValue bind = ch_car(b);
        if (!ch_is_pair(bind) || !ch_is_pair(ch_cdr(bind))) {
            ch_gc_pop_n(&c->vm->gc, 4);
            return fail(c, "let: bad binding");
        }
        ChValue name = ch_car(bind);
        ChValue init = ch_car(ch_cdr(bind));
        params = ch_gc_cons(&c->vm->gc, name, params);
        vals = ch_gc_cons(&c->vm->gc, init, vals);
        b = ch_cdr(b);
    }
    if (!ch_is_nil(b)) {
        ch_gc_pop_n(&c->vm->gc, 4);
        return fail(c, "let: bad bindings list");
    }

    ChValue rp = CH_NIL;
    ChValue rv = CH_NIL;
    ch_gc_push(&c->vm->gc, &rp);
    ch_gc_push(&c->vm->gc, &rv);
    while (ch_is_pair(params)) {
        rp = ch_gc_cons(&c->vm->gc, ch_car(params), rp);
        params = ch_cdr(params);
    }
    while (ch_is_pair(vals)) {
        rv = ch_gc_cons(&c->vm->gc, ch_car(vals), rv);
        vals = ch_cdr(vals);
    }

    ChValue call = CH_NIL;
    ChValue lambda = CH_NIL;
    ChValue lambda_args = CH_NIL;
    ch_gc_push(&c->vm->gc, &call);
    ch_gc_push(&c->vm->gc, &lambda);
    ch_gc_push(&c->vm->gc, &lambda_args);
    ChValue lambda_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "lambda");
    lambda_args = ch_gc_cons(&c->vm->gc, rp, body);
    lambda = ch_gc_cons(&c->vm->gc, lambda_sym, lambda_args);
    call = ch_gc_cons(&c->vm->gc, lambda, rv);
    ChValue form_keep = call;
    ch_gc_pop_n(&c->vm->gc, 9);
    return compile_expr(c, fc, form_keep, dst, tail);
}

/* (let* ((v e) ...) body...) → nested lets */
static ChCompileStatus compile_let_star(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                        uint8_t dst, bool tail) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "let*: bad syntax");
    }
    ChValue bindings = ch_car(args);
    ChValue body = ch_cdr(args);
    ch_gc_push(&c->vm->gc, &bindings);
    ch_gc_push(&c->vm->gc, &body);
    if (ch_is_nil(bindings)) {
        ChValue form = CH_NIL;
        ch_gc_push(&c->vm->gc, &form);
        ChValue begin_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "begin");
        form = ch_gc_cons(&c->vm->gc, begin_sym, body);
        ChValue form_keep = form;
        ch_gc_pop_n(&c->vm->gc, 3);
        return compile_expr(c, fc, form_keep, dst, tail);
    }
    if (!ch_is_pair(bindings)) {
        ch_gc_pop_n(&c->vm->gc, 2);
        return fail(c, "let*: bad bindings");
    }
    ChValue first = ch_car(bindings);
    ChValue rest = ch_cdr(bindings);
    ChValue form = CH_NIL;
    ChValue inner = CH_NIL;
    ChValue binds1 = CH_NIL;
    ch_gc_push(&c->vm->gc, &first);
    ch_gc_push(&c->vm->gc, &rest);
    ch_gc_push(&c->vm->gc, &form);
    ch_gc_push(&c->vm->gc, &inner);
    ch_gc_push(&c->vm->gc, &binds1);
    ChValue let_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let");
    ChValue let_star = ch_gc_intern_symbol_cstr(&c->vm->gc, "let*");
    inner = ch_gc_cons(&c->vm->gc, let_star, ch_gc_cons(&c->vm->gc, rest, body));
    binds1 = ch_gc_cons(&c->vm->gc, first, CH_NIL);
    form =
        ch_gc_cons(&c->vm->gc, let_sym, ch_gc_cons(&c->vm->gc, binds1, ch_gc_cons(&c->vm->gc, inner, CH_NIL)));
    ChValue form_keep = form;
    ch_gc_pop_n(&c->vm->gc, 7);
    return compile_expr(c, fc, form_keep, dst, tail);
}

/* (letrec ((v e) ...) body...) → (let ((v <undef>)...) (set! v e)... body...) */
static ChCompileStatus compile_letrec(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                      bool tail) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "letrec: bad syntax");
    }
    ChValue bindings = ch_car(args);
    ChValue body = ch_cdr(args);

    ChValue names = CH_NIL;
    ChValue inits = CH_NIL;
    ch_gc_push(&c->vm->gc, &names);
    ch_gc_push(&c->vm->gc, &inits);
    for (ChValue b = bindings; ch_is_pair(b); b = ch_cdr(b)) {
        ChValue bind = ch_car(b);
        if (!ch_is_pair(bind) || !ch_is_pair(ch_cdr(bind))) {
            ch_gc_pop_n(&c->vm->gc, 2);
            return fail(c, "letrec: bad binding");
        }
        names = ch_gc_cons(&c->vm->gc, ch_car(bind), names);
        inits = ch_gc_cons(&c->vm->gc, ch_car(ch_cdr(bind)), inits);
    }
    ChValue rn = CH_NIL;
    ChValue ri = CH_NIL;
    ch_gc_push(&c->vm->gc, &rn);
    ch_gc_push(&c->vm->gc, &ri);
    while (ch_is_pair(names)) {
        rn = ch_gc_cons(&c->vm->gc, ch_car(names), rn);
        names = ch_cdr(names);
    }
    while (ch_is_pair(inits)) {
        ri = ch_gc_cons(&c->vm->gc, ch_car(inits), ri);
        inits = ch_cdr(inits);
    }

    ChValue undef_binds = CH_NIL;
    ch_gc_push(&c->vm->gc, &undef_binds);
    for (ChValue n = rn; ch_is_pair(n); n = ch_cdr(n)) {
        ChValue bind =
            ch_gc_cons(&c->vm->gc, ch_car(n), ch_gc_cons(&c->vm->gc, CH_FALSE, CH_NIL));
        undef_binds = ch_gc_cons(&c->vm->gc, bind, undef_binds);
    }
    ChValue ub = CH_NIL;
    ch_gc_push(&c->vm->gc, &ub);
    while (ch_is_pair(undef_binds)) {
        ub = ch_gc_cons(&c->vm->gc, ch_car(undef_binds), ub);
        undef_binds = ch_cdr(undef_binds);
    }

    ChValue set_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "set!");
    ChValue sets = body;
    ch_gc_push(&c->vm->gc, &sets);
    /* build set! forms in reverse then reverse onto body */
    ChValue set_forms = CH_NIL;
    ch_gc_push(&c->vm->gc, &set_forms);
    ChValue nn = rn;
    ChValue ii = ri;
    while (ch_is_pair(nn) && ch_is_pair(ii)) {
        ChValue s = ch_gc_cons(&c->vm->gc, set_sym,
                               ch_gc_cons(&c->vm->gc, ch_car(nn),
                                          ch_gc_cons(&c->vm->gc, ch_car(ii), CH_NIL)));
        set_forms = ch_gc_cons(&c->vm->gc, s, set_forms);
        nn = ch_cdr(nn);
        ii = ch_cdr(ii);
    }
    ChValue sf = CH_NIL;
    ch_gc_push(&c->vm->gc, &sf);
    while (ch_is_pair(set_forms)) {
        sf = ch_gc_cons(&c->vm->gc, ch_car(set_forms), sf);
        set_forms = ch_cdr(set_forms);
    }
    /* append sf + body */
    ChValue new_body = body;
    ch_gc_push(&c->vm->gc, &new_body);
    {
        ChValue parts[64];
        int np = 0;
        for (ChValue p = sf; ch_is_pair(p) && np < 64; p = ch_cdr(p)) {
            parts[np++] = ch_car(p);
        }
        for (int i = np - 1; i >= 0; i--) {
            new_body = ch_gc_cons(&c->vm->gc, parts[i], new_body);
        }
    }
    ChValue let_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let");
    ChValue form = ch_gc_cons(&c->vm->gc, let_sym, ch_gc_cons(&c->vm->gc, ub, new_body));
    ChValue form_keep = form;
    ch_gc_pop_n(&c->vm->gc, 9);
    return compile_expr(c, fc, form_keep, dst, tail);
}

static ChCompileStatus compile_cond(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                    bool tail) {
    if (ch_is_nil(args)) {
        emit_byte(fc, CH_OP_LOAD_VOID);
        emit_byte(fc, dst);
        return CH_COMPILE_OK;
    }
    if (!ch_is_pair(args)) {
        return fail(c, "cond: bad syntax");
    }
    ChValue clause = ch_car(args);
    ChValue rest = ch_cdr(args);
    if (!ch_is_pair(clause)) {
        return fail(c, "cond: bad clause");
    }
    ChValue test = ch_car(clause);
    ChValue exprs = ch_cdr(clause);
    if (is_symbol_named(test, "else")) {
        if (!ch_is_nil(rest)) {
            return fail(c, "cond: else not last");
        }
        if (ch_is_nil(exprs)) {
            emit_byte(fc, CH_OP_LOAD_TRUE);
            emit_byte(fc, dst);
            return CH_COMPILE_OK;
        }
        ChValue begin_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "begin");
        ChValue form = ch_gc_cons(&c->vm->gc, begin_sym, exprs);
        return compile_expr(c, fc, form, dst, tail);
    }
    /* (=> recipient) — only when => is not shadowed by a local binding */
    if (ch_is_pair(exprs) && is_symbol_named(ch_car(exprs), "=>")) {
        ChSymbol *arrow = ch_as_symbol(ch_car(exprs));
        if (resolve_local(fc, arrow) < 0 && resolve_upvalue(fc, arrow) < 0) {
        if (!ch_is_pair(ch_cdr(exprs)) || !ch_is_nil(ch_cdr(ch_cdr(exprs)))) {
            return fail(c, "cond: bad => clause");
        }
        ChValue recipient = ch_car(ch_cdr(exprs));
        ChValue t = ch_gc_intern_symbol_cstr(&c->vm->gc, "t");
        ChValue if_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "if");
        ChValue let_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let");
        ChValue cond_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "cond");
        ChValue call = ch_gc_cons(&c->vm->gc, recipient, ch_gc_cons(&c->vm->gc, t, CH_NIL));
        ChValue alt = ch_gc_cons(&c->vm->gc, cond_sym, rest);
        ChValue if_form =
            ch_gc_cons(&c->vm->gc, if_sym, ch_gc_cons(&c->vm->gc, t, ch_gc_cons(&c->vm->gc, call, ch_gc_cons(&c->vm->gc, alt, CH_NIL))));
        ChValue bind = ch_gc_cons(&c->vm->gc, t, ch_gc_cons(&c->vm->gc, test, CH_NIL));
        ChValue binds = ch_gc_cons(&c->vm->gc, bind, CH_NIL);
        ChValue form =
            ch_gc_cons(&c->vm->gc, let_sym, ch_gc_cons(&c->vm->gc, binds, ch_gc_cons(&c->vm->gc, if_form, CH_NIL)));
        return compile_expr(c, fc, form, dst, tail);
        }
    }
    ChValue if_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "if");
    ChValue begin_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "begin");
    ChValue cond_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "cond");
    ChValue consequent;
    if (ch_is_nil(exprs)) {
        consequent = test; /* return test value — need temp; use (if t t alt) via let */
        ChValue t = ch_gc_intern_symbol_cstr(&c->vm->gc, "t");
        ChValue alt = ch_gc_cons(&c->vm->gc, cond_sym, rest);
        ChValue if_form =
            ch_gc_cons(&c->vm->gc, if_sym, ch_gc_cons(&c->vm->gc, t, ch_gc_cons(&c->vm->gc, t, ch_gc_cons(&c->vm->gc, alt, CH_NIL))));
        ChValue let_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let");
        ChValue bind = ch_gc_cons(&c->vm->gc, t, ch_gc_cons(&c->vm->gc, test, CH_NIL));
        ChValue binds = ch_gc_cons(&c->vm->gc, bind, CH_NIL);
        ChValue form =
            ch_gc_cons(&c->vm->gc, let_sym, ch_gc_cons(&c->vm->gc, binds, ch_gc_cons(&c->vm->gc, if_form, CH_NIL)));
        return compile_expr(c, fc, form, dst, tail);
    }
    consequent = ch_gc_cons(&c->vm->gc, begin_sym, exprs);
    ChValue alt = ch_gc_cons(&c->vm->gc, cond_sym, rest);
    ChValue form = ch_gc_cons(
        &c->vm->gc, if_sym,
        ch_gc_cons(&c->vm->gc, test, ch_gc_cons(&c->vm->gc, consequent, ch_gc_cons(&c->vm->gc, alt, CH_NIL))));
    return compile_expr(c, fc, form, dst, tail);
}

static ChCompileStatus build_case_chain(ChCompiler *c, ChValue key_sym, ChValue clauses,
                                        ChValue *out_form) {
    if (ch_is_nil(clauses)) {
        *out_form = CH_VOID;
        return CH_COMPILE_OK;
    }
    if (!ch_is_pair(clauses)) {
        return fail(c, "case: bad syntax");
    }

    ChValue clause = ch_car(clauses);
    ChValue rest = ch_cdr(clauses);
    if (!ch_is_pair(clause)) {
        return fail(c, "case: bad clause");
    }

    ChValue datums = ch_car(clause);
    ChValue body = ch_cdr(clause);
    if (is_symbol_named(datums, "else")) {
        if (!ch_is_nil(rest)) {
            return fail(c, "case: else not last");
        }
        if (ch_is_pair(body) && is_symbol_named(ch_car(body), "=>")) {
            if (!ch_is_pair(ch_cdr(body)) || !ch_is_nil(ch_cdr(ch_cdr(body)))) {
                return fail(c, "case: bad => clause");
            }
            ChValue recipient = ch_car(ch_cdr(body));
            *out_form = ch_gc_cons(&c->vm->gc, recipient, ch_gc_cons(&c->vm->gc, key_sym, CH_NIL));
            return CH_COMPILE_OK;
        }
        ChValue begin_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "begin");
        *out_form = ch_gc_cons(&c->vm->gc, begin_sym, body);
        return CH_COMPILE_OK;
    }

    for (ChValue p = datums; !ch_is_nil(p); p = ch_cdr(p)) {
        if (!ch_is_pair(p)) {
            return fail(c, "case: bad datum list");
        }
    }

    ChValue alternate = CH_VOID;
    ChValue test_expr = CH_NIL;
    ChValue consequent = CH_NIL;
    ChValue if_form = CH_NIL;
    ch_gc_push(&c->vm->gc, &alternate);
    ch_gc_push(&c->vm->gc, &test_expr);
    ch_gc_push(&c->vm->gc, &consequent);
    ch_gc_push(&c->vm->gc, &if_form);

    if (build_case_chain(c, key_sym, rest, &alternate) != CH_COMPILE_OK) {
        ch_gc_pop_n(&c->vm->gc, 4);
        return CH_COMPILE_ERROR;
    }

    if (ch_is_pair(body) && is_symbol_named(ch_car(body), "=>")) {
        if (!ch_is_pair(ch_cdr(body)) || !ch_is_nil(ch_cdr(ch_cdr(body)))) {
            ch_gc_pop_n(&c->vm->gc, 4);
            return fail(c, "case: bad => clause");
        }
        ChValue recipient = ch_car(ch_cdr(body));
        consequent = ch_gc_cons(&c->vm->gc, recipient, ch_gc_cons(&c->vm->gc, key_sym, CH_NIL));
    } else {
        ChValue begin_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "begin");
        consequent = ch_gc_cons(&c->vm->gc, begin_sym, body);
    }

    ChValue memv_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "memv");
    ChValue quote_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "quote");
    ChValue quoted_datums = ch_gc_cons(&c->vm->gc, quote_sym, ch_gc_cons(&c->vm->gc, datums, CH_NIL));
    test_expr =
        ch_gc_cons(&c->vm->gc, memv_sym, ch_gc_cons(&c->vm->gc, key_sym, ch_gc_cons(&c->vm->gc, quoted_datums, CH_NIL)));

    ChValue if_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "if");
    if_form = ch_gc_cons(&c->vm->gc, if_sym,
                         ch_gc_cons(&c->vm->gc, test_expr,
                                    ch_gc_cons(&c->vm->gc, consequent,
                                               ch_gc_cons(&c->vm->gc, alternate, CH_NIL))));
    *out_form = if_form;
    ch_gc_pop_n(&c->vm->gc, 4);
    return CH_COMPILE_OK;
}

static ChCompileStatus compile_case(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                    bool tail) {
    if (!ch_is_pair(args)) {
        return fail(c, "case: bad syntax");
    }

    ChValue key_expr = ch_car(args);
    ChValue clauses = ch_cdr(args);

    case_gensym_counter++;
    char key_name[48];
    snprintf(key_name, sizeof(key_name), "%%case-key%zu", case_gensym_counter);
    ChValue key_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, key_name);

    ChValue chain = CH_VOID;
    ChValue form = CH_NIL;
    ch_gc_push(&c->vm->gc, &chain);
    ch_gc_push(&c->vm->gc, &form);

    if (build_case_chain(c, key_sym, clauses, &chain) != CH_COMPILE_OK) {
        ch_gc_pop_n(&c->vm->gc, 2);
        return CH_COMPILE_ERROR;
    }

    ChValue let_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let");
    ChValue bind = ch_gc_cons(&c->vm->gc, key_sym, ch_gc_cons(&c->vm->gc, key_expr, CH_NIL));
    ChValue binds = ch_gc_cons(&c->vm->gc, bind, CH_NIL);
    form = ch_gc_cons(&c->vm->gc, let_sym, ch_gc_cons(&c->vm->gc, binds, ch_gc_cons(&c->vm->gc, chain, CH_NIL)));

    ChValue form_keep = form;
    ch_gc_pop_n(&c->vm->gc, 2);
    return compile_expr(c, fc, form_keep, dst, tail);
}

static ChCompileStatus compile_when_unless(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                           uint8_t dst, bool tail, int is_when) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, is_when ? "when: bad syntax" : "unless: bad syntax");
    }
    ChValue test = ch_car(args);
    ChValue body = ch_cdr(args);
    ChValue begin_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "begin");
    ChValue if_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "if");
    ChValue body_form = ch_gc_cons(&c->vm->gc, begin_sym, body);
    ChValue form;
    if (is_when) {
        form = ch_gc_cons(
            &c->vm->gc, if_sym,
            ch_gc_cons(&c->vm->gc, test,
                       ch_gc_cons(&c->vm->gc, body_form, ch_gc_cons(&c->vm->gc, CH_VOID, CH_NIL))));
    } else {
        form = ch_gc_cons(
            &c->vm->gc, if_sym,
            ch_gc_cons(&c->vm->gc, test,
                       ch_gc_cons(&c->vm->gc, CH_VOID, ch_gc_cons(&c->vm->gc, body_form, CH_NIL))));
    }
    return compile_expr(c, fc, form, dst, tail);
}

/* (do ((v init step) ...) (test result ...) cmd ...)
 *   ->
 * (let loop ((v init) ...)
 *   (if test
 *       (begin result ...)
 *       (begin cmd ... (loop step-or-v ...)))) */
static ChCompileStatus compile_do(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                  bool tail) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "do: bad syntax");
    }

    ChValue bindings = ch_car(args);
    ChValue rest = ch_cdr(args);
    ChValue test_clause = ch_car(rest);
    ChValue commands = ch_cdr(rest);
    if (!ch_is_pair(test_clause)) {
        return fail(c, "do: bad test clause");
    }
    ChValue test_expr = ch_car(test_clause);
    ChValue result_exprs = ch_cdr(test_clause);

    ChValue var_names[CH_MAX_DERIVED_BINDINGS];
    ChValue init_exprs[CH_MAX_DERIVED_BINDINGS];
    ChValue step_exprs[CH_MAX_DERIVED_BINDINGS];
    size_t nvars = 0;

    ChValue b = bindings;
    while (ch_is_pair(b)) {
        if (nvars >= CH_MAX_DERIVED_BINDINGS) {
            return fail(c, "do: too many bindings");
        }
        ChValue spec = ch_car(b);
        if (!ch_is_pair(spec) || !ch_is_symbol(ch_car(spec))) {
            return fail(c, "do: bad binding");
        }
        ChValue spec_rest = ch_cdr(spec);
        if (!ch_is_pair(spec_rest)) {
            return fail(c, "do: bad binding");
        }
        ChValue init = ch_car(spec_rest);
        ChValue after_init = ch_cdr(spec_rest);
        ChValue step = CH_UNDEFINED;
        if (ch_is_pair(after_init)) {
            step = ch_car(after_init);
            if (!ch_is_nil(ch_cdr(after_init))) {
                return fail(c, "do: bad binding");
            }
        } else if (!ch_is_nil(after_init)) {
            return fail(c, "do: bad binding");
        }
        var_names[nvars] = ch_car(spec);
        init_exprs[nvars] = init;
        step_exprs[nvars] = step;
        nvars++;
        b = ch_cdr(b);
    }
    if (!ch_is_nil(b)) {
        return fail(c, "do: bad bindings");
    }

    ChValue let_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let");
    ChValue if_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "if");
    ChValue begin_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "begin");
    do_gensym_counter++;
    char loop_name[48];
    snprintf(loop_name, sizeof(loop_name), "%%do-loop%zu", do_gensym_counter);
    ChValue loop_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, loop_name);

    ChValue loop_bindings = CH_NIL;
    ChValue recur_args = CH_NIL;
    ChValue recur_call = CH_NIL;
    ChValue rev_cmds = CH_NIL;
    ChValue else_forms = CH_NIL;
    ChValue else_begin = CH_NIL;
    ChValue then_begin = CH_NIL;
    ChValue if_form = CH_NIL;
    ChValue let_body = CH_NIL;
    ChValue let_args = CH_NIL;
    ChValue form = CH_NIL;
    ch_gc_push(&c->vm->gc, &loop_bindings);
    ch_gc_push(&c->vm->gc, &recur_args);
    ch_gc_push(&c->vm->gc, &recur_call);
    ch_gc_push(&c->vm->gc, &rev_cmds);
    ch_gc_push(&c->vm->gc, &else_forms);
    ch_gc_push(&c->vm->gc, &else_begin);
    ch_gc_push(&c->vm->gc, &then_begin);
    ch_gc_push(&c->vm->gc, &if_form);
    ch_gc_push(&c->vm->gc, &let_body);
    ch_gc_push(&c->vm->gc, &let_args);
    ch_gc_push(&c->vm->gc, &form);

    for (size_t i = nvars; i > 0; i--) {
        size_t idx = i - 1;
        ChValue bind =
            ch_gc_cons(&c->vm->gc, var_names[idx], ch_gc_cons(&c->vm->gc, init_exprs[idx], CH_NIL));
        loop_bindings = ch_gc_cons(&c->vm->gc, bind, loop_bindings);
        ChValue next_arg = (step_exprs[idx] == CH_UNDEFINED) ? var_names[idx] : step_exprs[idx];
        recur_args = ch_gc_cons(&c->vm->gc, next_arg, recur_args);
    }
    recur_call = ch_gc_cons(&c->vm->gc, loop_sym, recur_args);

    ChValue cmd = commands;
    while (ch_is_pair(cmd)) {
        rev_cmds = ch_gc_cons(&c->vm->gc, ch_car(cmd), rev_cmds);
        cmd = ch_cdr(cmd);
    }
    if (!ch_is_nil(cmd)) {
        ch_gc_pop_n(&c->vm->gc, 11);
        return fail(c, "do: bad command list");
    }
    else_forms = ch_gc_cons(&c->vm->gc, recur_call, CH_NIL);
    while (ch_is_pair(rev_cmds)) {
        else_forms = ch_gc_cons(&c->vm->gc, ch_car(rev_cmds), else_forms);
        rev_cmds = ch_cdr(rev_cmds);
    }

    then_begin = ch_gc_cons(&c->vm->gc, begin_sym, result_exprs);
    else_begin = ch_gc_cons(&c->vm->gc, begin_sym, else_forms);
    if_form = ch_gc_cons(
        &c->vm->gc, if_sym,
        ch_gc_cons(&c->vm->gc, test_expr,
                   ch_gc_cons(&c->vm->gc, then_begin, ch_gc_cons(&c->vm->gc, else_begin, CH_NIL))));
    let_body = ch_gc_cons(&c->vm->gc, if_form, CH_NIL);
    let_args =
        ch_gc_cons(&c->vm->gc, loop_sym, ch_gc_cons(&c->vm->gc, loop_bindings, let_body));
    form = ch_gc_cons(&c->vm->gc, let_sym, let_args);

    ChValue form_keep = form;
    ch_gc_pop_n(&c->vm->gc, 11);
    return compile_expr(c, fc, form_keep, dst, tail);
}

static ChCompileStatus compile_guard(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                     bool tail) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "guard: bad syntax");
    }
    ChValue clause_spec = ch_car(args);
    ChValue body = ch_cdr(args);
    if (!ch_is_pair(clause_spec)) {
        return fail(c, "guard: bad syntax");
    }
    ChValue var = ch_car(clause_spec);
    if (!ch_is_symbol(var)) {
        return fail(c, "guard: bad syntax");
    }
    ChValue clauses = ch_cdr(clause_spec);

    ChValue clause_vec[CH_MAX_GUARD_CLAUSES];
    size_t clause_count = 0;
    bool has_else = false;
    ChValue it = clauses;
    while (ch_is_pair(it)) {
        if (clause_count >= CH_MAX_GUARD_CLAUSES) {
            return fail(c, "guard: too many clauses");
        }
        ChValue clause = ch_car(it);
        clause_vec[clause_count++] = clause;
        if (ch_is_pair(clause) && is_symbol_named(ch_car(clause), "else")) {
            has_else = true;
        }
        it = ch_cdr(it);
    }
    if (!ch_is_nil(it)) {
        return fail(c, "guard: bad syntax");
    }

    ChValue cond_clauses = CH_NIL;
    ChValue raise_call = CH_NIL;
    ChValue else_clause = CH_NIL;
    ChValue cond_form = CH_NIL;
    ChValue gk_call = CH_NIL;
    ChValue let_bind = CH_NIL;
    ChValue let_binds = CH_NIL;
    ChValue let_form = CH_NIL;
    ChValue handler_formals = CH_NIL;
    ChValue handler_body = CH_NIL;
    ChValue handler = CH_NIL;
    ChValue thunk = CH_NIL;
    ChValue weh = CH_NIL;
    ChValue outer = CH_NIL;
    ChValue form = CH_NIL;
    ch_gc_push(&c->vm->gc, &cond_clauses);
    ch_gc_push(&c->vm->gc, &raise_call);
    ch_gc_push(&c->vm->gc, &else_clause);
    ch_gc_push(&c->vm->gc, &cond_form);
    ch_gc_push(&c->vm->gc, &gk_call);
    ch_gc_push(&c->vm->gc, &let_bind);
    ch_gc_push(&c->vm->gc, &let_binds);
    ch_gc_push(&c->vm->gc, &let_form);
    ch_gc_push(&c->vm->gc, &handler_formals);
    ch_gc_push(&c->vm->gc, &handler_body);
    ch_gc_push(&c->vm->gc, &handler);
    ch_gc_push(&c->vm->gc, &thunk);
    ch_gc_push(&c->vm->gc, &weh);
    ch_gc_push(&c->vm->gc, &outer);
    ch_gc_push(&c->vm->gc, &form);

    if (!has_else) {
        ChValue raise_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "raise");
        ChValue else_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "else");
        raise_call = ch_gc_cons(&c->vm->gc, raise_sym, ch_gc_cons(&c->vm->gc, var, CH_NIL));
        else_clause = ch_gc_cons(&c->vm->gc, else_sym, ch_gc_cons(&c->vm->gc, raise_call, CH_NIL));
        cond_clauses = ch_gc_cons(&c->vm->gc, else_clause, cond_clauses);
    }
    for (size_t i = clause_count; i > 0; i--) {
        cond_clauses = ch_gc_cons(&c->vm->gc, clause_vec[i - 1], cond_clauses);
    }

    guard_gensym_counter++;
    char gk_name[48];
    char value_name[48];
    snprintf(gk_name, sizeof(gk_name), "%%guard-k%zu", guard_gensym_counter);
    snprintf(value_name, sizeof(value_name), "%%guard-v%zu", guard_gensym_counter);
    ChValue gk = ch_gc_intern_symbol_cstr(&c->vm->gc, gk_name);
    ChValue guard_value = ch_gc_intern_symbol_cstr(&c->vm->gc, value_name);

    ChValue cond_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "cond");
    ChValue lambda_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "lambda");
    ChValue weh_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "with-exception-handler");
    ChValue callcc_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "call/cc");
    ChValue let_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let");

    cond_form = ch_gc_cons(&c->vm->gc, cond_sym, cond_clauses);
    gk_call = ch_gc_cons(&c->vm->gc, gk, ch_gc_cons(&c->vm->gc, guard_value, CH_NIL));
    let_bind = ch_gc_cons(&c->vm->gc, guard_value, ch_gc_cons(&c->vm->gc, cond_form, CH_NIL));
    let_binds = ch_gc_cons(&c->vm->gc, let_bind, CH_NIL);
    let_form = ch_gc_cons(&c->vm->gc, let_sym,
                          ch_gc_cons(&c->vm->gc, let_binds, ch_gc_cons(&c->vm->gc, gk_call, CH_NIL)));
    handler_formals = ch_gc_cons(&c->vm->gc, var, CH_NIL);
    handler_body = ch_gc_cons(&c->vm->gc, let_form, CH_NIL);
    handler =
        ch_gc_cons(&c->vm->gc, lambda_sym, ch_gc_cons(&c->vm->gc, handler_formals, handler_body));
    thunk = ch_gc_cons(&c->vm->gc, lambda_sym, ch_gc_cons(&c->vm->gc, CH_NIL, body));
    weh = ch_gc_cons(
        &c->vm->gc, weh_sym,
        ch_gc_cons(&c->vm->gc, handler, ch_gc_cons(&c->vm->gc, thunk, CH_NIL)));
    outer = ch_gc_cons(&c->vm->gc, lambda_sym,
                       ch_gc_cons(&c->vm->gc, ch_gc_cons(&c->vm->gc, gk, CH_NIL),
                                  ch_gc_cons(&c->vm->gc, weh, CH_NIL)));
    form = ch_gc_cons(&c->vm->gc, callcc_sym, ch_gc_cons(&c->vm->gc, outer, CH_NIL));

    ChValue form_keep = form;
    ch_gc_pop_n(&c->vm->gc, 15);
    return compile_expr(c, fc, form_keep, dst, tail);
}

static ChCompileStatus compile_parameterize(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                            uint8_t dst, bool tail) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "parameterize: bad syntax");
    }
    ChValue bindings = ch_car(args);
    ChValue body = ch_cdr(args);

    ChValue param_exprs[CH_MAX_DERIVED_BINDINGS];
    ChValue value_exprs[CH_MAX_DERIVED_BINDINGS];
    ChValue param_syms[CH_MAX_DERIVED_BINDINGS];
    ChValue value_syms[CH_MAX_DERIVED_BINDINGS];
    ChValue converted_syms[CH_MAX_DERIVED_BINDINGS];

    size_t nbindings = 0;
    ChValue b = bindings;
    while (ch_is_pair(b)) {
        if (nbindings >= CH_MAX_DERIVED_BINDINGS) {
            return fail(c, "parameterize: too many bindings");
        }
        ChValue binding = ch_car(b);
        if (!ch_is_pair(binding) || !ch_is_pair(ch_cdr(binding)) ||
            !ch_is_nil(ch_cdr(ch_cdr(binding)))) {
            return fail(c, "parameterize: bad binding");
        }
        param_exprs[nbindings] = ch_car(binding);
        value_exprs[nbindings] = ch_car(ch_cdr(binding));
        nbindings++;
        b = ch_cdr(b);
    }
    if (!ch_is_nil(b)) {
        return fail(c, "parameterize: bad bindings");
    }

    if (nbindings == 0) {
        ChValue begin_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "begin");
        ChValue form = ch_gc_cons(&c->vm->gc, begin_sym, body);
        return compile_expr(c, fc, form, dst, tail);
    }

    parameterize_gensym_counter++;
    size_t gensym = parameterize_gensym_counter;
    for (size_t i = 0; i < nbindings; i++) {
        char name[48];
        snprintf(name, sizeof(name), "%%pp%zu_%zu", gensym, i);
        param_syms[i] = ch_gc_intern_symbol_cstr(&c->vm->gc, name);
        snprintf(name, sizeof(name), "%%pv%zu_%zu", gensym, i);
        value_syms[i] = ch_gc_intern_symbol_cstr(&c->vm->gc, name);
        snprintf(name, sizeof(name), "%%pn%zu_%zu", gensym, i);
        converted_syms[i] = ch_gc_intern_symbol_cstr(&c->vm->gc, name);
    }

    ChValue outer_bindings = CH_NIL;
    ChValue inner_bindings = CH_NIL;
    ChValue before_body = CH_NIL;
    ChValue after_body = CH_NIL;
    ChValue before_thunk = CH_NIL;
    ChValue body_thunk = CH_NIL;
    ChValue after_thunk = CH_NIL;
    ChValue dw_call = CH_NIL;
    ChValue inner_let = CH_NIL;
    ChValue outer_let = CH_NIL;
    ch_gc_push(&c->vm->gc, &outer_bindings);
    ch_gc_push(&c->vm->gc, &inner_bindings);
    ch_gc_push(&c->vm->gc, &before_body);
    ch_gc_push(&c->vm->gc, &after_body);
    ch_gc_push(&c->vm->gc, &before_thunk);
    ch_gc_push(&c->vm->gc, &body_thunk);
    ch_gc_push(&c->vm->gc, &after_thunk);
    ch_gc_push(&c->vm->gc, &dw_call);
    ch_gc_push(&c->vm->gc, &inner_let);
    ch_gc_push(&c->vm->gc, &outer_let);

    for (size_t i = nbindings; i > 0; i--) {
        size_t idx = i - 1;
        ChValue pv_pair = ch_gc_cons(&c->vm->gc, value_syms[idx],
                                     ch_gc_cons(&c->vm->gc, value_exprs[idx], CH_NIL));
        outer_bindings = ch_gc_cons(&c->vm->gc, pv_pair, outer_bindings);
    }
    for (size_t i = nbindings; i > 0; i--) {
        size_t idx = i - 1;
        ChValue pp_pair = ch_gc_cons(&c->vm->gc, param_syms[idx],
                                     ch_gc_cons(&c->vm->gc, param_exprs[idx], CH_NIL));
        outer_bindings = ch_gc_cons(&c->vm->gc, pp_pair, outer_bindings);
    }

    ChValue pconvert_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "%parameter-convert");
    for (size_t i = nbindings; i > 0; i--) {
        size_t idx = i - 1;
        ChValue convert_call =
            ch_gc_cons(&c->vm->gc, pconvert_sym,
                       ch_gc_cons(&c->vm->gc, param_syms[idx],
                                  ch_gc_cons(&c->vm->gc, value_syms[idx], CH_NIL)));
        ChValue new_pair = ch_gc_cons(&c->vm->gc, converted_syms[idx],
                                      ch_gc_cons(&c->vm->gc, convert_call, CH_NIL));
        inner_bindings = ch_gc_cons(&c->vm->gc, new_pair, inner_bindings);
    }

    ChValue ppush_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "%parameter-push!");
    for (size_t i = nbindings; i > 0; i--) {
        size_t idx = i - 1;
        ChValue push_call =
            ch_gc_cons(&c->vm->gc, ppush_sym,
                       ch_gc_cons(&c->vm->gc, param_syms[idx],
                                  ch_gc_cons(&c->vm->gc, converted_syms[idx], CH_NIL)));
        before_body = ch_gc_cons(&c->vm->gc, push_call, before_body);
    }

    ChValue ppop_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "%parameter-pop!");
    for (size_t i = 0; i < nbindings; i++) {
        ChValue pop_call = ch_gc_cons(
            &c->vm->gc, ppop_sym, ch_gc_cons(&c->vm->gc, param_syms[i], CH_NIL));
        after_body = ch_gc_cons(&c->vm->gc, pop_call, after_body);
    }

    ChValue lambda_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "lambda");
    ChValue dynamic_wind_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "dynamic-wind");
    ChValue letstar_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let*");
    ChValue let_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let");

    before_thunk =
        ch_gc_cons(&c->vm->gc, lambda_sym, ch_gc_cons(&c->vm->gc, CH_NIL, before_body));
    body_thunk = ch_gc_cons(&c->vm->gc, lambda_sym, ch_gc_cons(&c->vm->gc, CH_NIL, body));
    after_thunk = ch_gc_cons(&c->vm->gc, lambda_sym, ch_gc_cons(&c->vm->gc, CH_NIL, after_body));
    dw_call = ch_gc_cons(
        &c->vm->gc, dynamic_wind_sym,
        ch_gc_cons(&c->vm->gc, before_thunk,
                   ch_gc_cons(&c->vm->gc, body_thunk,
                              ch_gc_cons(&c->vm->gc, after_thunk, CH_NIL))));
    inner_let = ch_gc_cons(&c->vm->gc, letstar_sym,
                           ch_gc_cons(&c->vm->gc, inner_bindings,
                                      ch_gc_cons(&c->vm->gc, dw_call, CH_NIL)));
    outer_let =
        ch_gc_cons(&c->vm->gc, let_sym,
                   ch_gc_cons(&c->vm->gc, outer_bindings,
                              ch_gc_cons(&c->vm->gc, inner_let, CH_NIL)));

    ChValue form_keep = outer_let;
    ch_gc_pop_n(&c->vm->gc, 10);
    return compile_expr(c, fc, form_keep, dst, tail);
}

static ChValue qq_expand(ChCompiler *c, ChValue x, int level);

static ChValue qq_expand_list(ChCompiler *c, ChValue xs, int level) {
    ChValue xs_root = xs;
    ch_gc_push(&c->vm->gc, &xs_root);
    if (ch_is_nil(xs_root)) {
        ChValue out = ch_gc_cons(&c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "quote"),
                                 ch_gc_cons(&c->vm->gc, CH_NIL, CH_NIL));
        ch_gc_pop(&c->vm->gc);
        return out;
    }
    if (!ch_is_pair(xs_root)) {
        ChValue out = qq_expand(c, xs_root, level);
        ch_gc_pop(&c->vm->gc);
        return out;
    }
    ChValue head = ch_car(xs_root);
    ch_gc_push(&c->vm->gc, &head);
    if (level == 0 && ch_is_pair(head) && is_symbol_named(ch_car(head), "unquote-splicing")) {
        ChValue uargs = ch_cdr(head);
        if (!ch_is_pair(uargs)) {
            ch_gc_pop_n(&c->vm->gc, 2);
            return CH_FALSE;
        }
        ChValue rest = qq_expand_list(c, ch_cdr(xs_root), level);
        ch_gc_push(&c->vm->gc, &rest);
        ChValue app = ch_gc_cons(
            &c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "append"),
            ch_gc_cons(&c->vm->gc, ch_car(uargs), ch_gc_cons(&c->vm->gc, rest, CH_NIL)));
        ch_gc_pop_n(&c->vm->gc, 3);
        return app;
    }
    ChValue car_e = qq_expand(c, head, level);
    ch_gc_push(&c->vm->gc, &car_e);
    ChValue cdr_e = qq_expand_list(c, ch_cdr(xs_root), level);
    ch_gc_push(&c->vm->gc, &cdr_e);
    ChValue out = ch_gc_cons(
        &c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "cons"),
        ch_gc_cons(&c->vm->gc, car_e, ch_gc_cons(&c->vm->gc, cdr_e, CH_NIL)));
    ch_gc_pop_n(&c->vm->gc, 4);
    return out;
}

static ChValue qq_expand(ChCompiler *c, ChValue x, int level) {
    if (ch_is_vector(x)) {
        ChValue x_root = x;
        ch_gc_push(&c->vm->gc, &x_root);
        ChVector *vec = ch_as_vector(x_root);
        ChValue lst = CH_NIL;
        ch_gc_push(&c->vm->gc, &lst);
        for (size_t i = vec->len; i > 0; i--) {
            ChValue item = vec->items[i - 1];
            ch_gc_push(&c->vm->gc, &item);
            lst = ch_gc_cons(&c->vm->gc, item, lst);
            ch_gc_pop(&c->vm->gc);
        }
        ChValue expanded_list = qq_expand_list(c, lst, level);
        if (expanded_list == CH_FALSE) {
            ch_gc_pop_n(&c->vm->gc, 2);
            return CH_FALSE;
        }
        ch_gc_push(&c->vm->gc, &expanded_list);
        ChValue l2v_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "list->vector");
        ChValue out = ch_gc_cons(&c->vm->gc, l2v_sym, ch_gc_cons(&c->vm->gc, expanded_list, CH_NIL));
        ch_gc_pop_n(&c->vm->gc, 3);
        return out;
    }
    if (ch_is_pair(x)) {
        if (is_symbol_named(ch_car(x), "unquote")) {
            ChValue args = ch_cdr(x);
            if (!ch_is_pair(args)) {
                return CH_FALSE;
            }
            if (level == 0) {
                return ch_car(args);
            }
            ChValue inner = qq_expand(c, ch_car(args), level - 1);
            ch_gc_push(&c->vm->gc, &inner);
            ChValue q = ch_gc_cons(
                &c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "list"),
                ch_gc_cons(&c->vm->gc,
                           ch_gc_cons(&c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "quote"),
                                      ch_gc_cons(&c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "unquote"),
                                                 CH_NIL)),
                           ch_gc_cons(&c->vm->gc, inner, CH_NIL)));
            ch_gc_pop(&c->vm->gc);
            return q;
        }
        if (is_symbol_named(ch_car(x), "unquote-splicing")) {
            ChValue args = ch_cdr(x);
            if (!ch_is_pair(args)) {
                return CH_FALSE;
            }
            if (level == 0) {
                return CH_FALSE;
            }
            ChValue inner = qq_expand(c, ch_car(args), level - 1);
            ch_gc_push(&c->vm->gc, &inner);
            ChValue q = ch_gc_cons(
                &c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "list"),
                ch_gc_cons(&c->vm->gc,
                           ch_gc_cons(&c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "quote"),
                                      ch_gc_cons(&c->vm->gc,
                                                 ch_gc_intern_symbol_cstr(&c->vm->gc, "unquote-splicing"),
                                                 CH_NIL)),
                           ch_gc_cons(&c->vm->gc, inner, CH_NIL)));
            ch_gc_pop(&c->vm->gc);
            return q;
        }
        if (is_symbol_named(ch_car(x), "quasiquote")) {
            ChValue args = ch_cdr(x);
            if (!ch_is_pair(args)) {
                return CH_FALSE;
            }
            ChValue inner = qq_expand(c, ch_car(args), level + 1);
            ch_gc_push(&c->vm->gc, &inner);
            ChValue q = ch_gc_cons(
                &c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "list"),
                ch_gc_cons(&c->vm->gc,
                           ch_gc_cons(&c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "quote"),
                                      ch_gc_cons(&c->vm->gc,
                                                 ch_gc_intern_symbol_cstr(&c->vm->gc, "quasiquote"),
                                                 CH_NIL)),
                           ch_gc_cons(&c->vm->gc, inner, CH_NIL)));
            ch_gc_pop(&c->vm->gc);
            return q;
        }
        return qq_expand_list(c, x, level);
    }
    /* atom */
    return ch_gc_cons(&c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "quote"),
                      ch_gc_cons(&c->vm->gc, x, CH_NIL));
}

static ChCompileStatus compile_quasiquote_real(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                               uint8_t dst, bool tail) {
    if (!ch_is_pair(args) || !ch_is_nil(ch_cdr(args))) {
        return fail(c, "quasiquote: bad syntax");
    }
    ChValue tmpl = ch_car(args);
    ch_gc_push(&c->vm->gc, &tmpl);
    ChValue expanded = qq_expand(c, tmpl, 0);
    ch_gc_pop(&c->vm->gc);
    if (expanded == CH_FALSE) {
        return fail(c, "quasiquote: bad syntax");
    }
    size_t root_base = c->vm->gc.root_count;
    ch_gc_push(&c->vm->gc, &expanded);
    ChCompileStatus st = compile_expr(c, fc, expanded, dst, tail);
    pop_root_at(&c->vm->gc, root_base);
    return st;
}

static ChCompileStatus compile_expr_impl(ChCompiler *c, ChFuncCompiler *fc, ChValue expr,
                                         uint8_t dst, bool tail);

static ChCompileStatus compile_expr(ChCompiler *c, ChFuncCompiler *fc, ChValue expr, uint8_t dst,
                                    bool tail) {
    /* Keep heap exprs rooted across desugaring allocations inside compile.
     * add_constant may push long-lived roots above this slot; a plain pop would
     * steal those. Remove only our rooted entry and compact the rest. */
    if (!ch_is_pointer(expr)) {
        return compile_expr_impl(c, fc, expr, dst, tail);
    }
    ChGC *gc = &c->vm->gc;
    size_t base = gc->root_count;
    ChValue rooted = expr;
    ch_gc_push(gc, &rooted);
    ChCompileStatus st = compile_expr_impl(c, fc, rooted, dst, tail);
    pop_root_at(gc, base);
    return st;
}

static ChCompileStatus compile_expr_impl(ChCompiler *c, ChFuncCompiler *fc, ChValue expr,
                                         uint8_t dst, bool tail) {
    if (expr == CH_VOID) {
        emit_byte(fc, CH_OP_LOAD_VOID);
        emit_byte(fc, dst);
        return CH_COMPILE_OK;
    }
    if (ch_is_symbol(expr)) {
        return compile_variable(c, fc, ch_as_symbol(expr), dst);
    }
    if (!ch_is_pair(expr)) {
        if (expr == CH_TRUE) {
            emit_byte(fc, CH_OP_LOAD_TRUE);
            emit_byte(fc, dst);
            return CH_COMPILE_OK;
        }
        if (expr == CH_FALSE) {
            emit_byte(fc, CH_OP_LOAD_FALSE);
            emit_byte(fc, dst);
            return CH_COMPILE_OK;
        }
        if (ch_is_nil(expr)) {
            emit_byte(fc, CH_OP_LOAD_NIL);
            emit_byte(fc, dst);
            return CH_COMPILE_OK;
        }
        int idx = add_constant(c, fc, expr);
        if (idx < 0) {
            return CH_COMPILE_ERROR;
        }
        emit_byte(fc, CH_OP_LOAD_CONST);
        emit_byte(fc, dst);
        emit_u16(fc, (uint16_t)idx);
        return CH_COMPILE_OK;
    }

    ChValue head = ch_car(expr);
    ChValue args = ch_cdr(expr);

    /* Macro uses should already be expanded at toplevel; expand again for safety. */
    if (ch_is_symbol(head)) {
        ChTransformer *tr = ch_vm_lookup_macro(c->vm, ch_as_symbol(head));
        if (tr) {
            ChValue expanded = CH_NIL;
            ch_gc_push(&c->vm->gc, &expanded);
            char err[256];
            ChLibEnv *saved_lib = c->vm->active_lib_env;
            ChLibEnv *home = NULL;
            const char *base = ch_symbol_basename(ch_as_symbol(head));
            for (size_t i = 0; i < c->vm->macro_count; i++) {
                if (strcmp(ch_symbol_basename(c->vm->macros[i].name), base) == 0) {
                    home = c->vm->macros[i].home_env;
                    break;
                }
            }
            if (home) {
                c->vm->active_lib_env = home;
            }
            if (ch_expand_macro(c->vm, tr, expr, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
                c->vm->active_lib_env = saved_lib;
                ch_gc_pop(&c->vm->gc);
                return fail(c, err);
            }
            size_t root_base = c->vm->gc.root_count - 1;
            ChCompileStatus st = compile_expr(c, fc, expanded, dst, tail);
            c->vm->active_lib_env = saved_lib;
            pop_root_at(&c->vm->gc, root_base);
            return st;
        }
    }

    if (is_symbol_named(head, "quote")) {
        return compile_quote(c, fc, args, dst);
    }
    if (is_symbol_named(head, "quasiquote")) {
        return compile_quasiquote_real(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "if")) {
        return compile_if(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "begin")) {
        return compile_begin(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "set!")) {
        return compile_set(c, fc, args, dst);
    }
    if (is_symbol_named(head, "define")) {
        return compile_define(c, fc, args, dst);
    }
    if (is_symbol_named(head, "define-syntax")) {
        /* Should have been handled during expansion; treat as void. */
        emit_byte(fc, CH_OP_LOAD_VOID);
        emit_byte(fc, dst);
        return CH_COMPILE_OK;
    }
    if (is_symbol_named(head, "define-property")) {
        return compile_define_property(c, fc, args, dst);
    }
    if (is_symbol_named(head, "let-syntax")) {
        return compile_let_syntax(c, fc, args, dst, tail, 0);
    }
    if (is_symbol_named(head, "letrec-syntax")) {
        return compile_let_syntax(c, fc, args, dst, tail, 1);
    }
    if (is_symbol_named(head, "cond-expand")) {
        return compile_cond_expand(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "lambda")) {
        return compile_lambda(c, fc, args, dst);
    }
    if (is_symbol_named(head, "case-lambda")) {
        return compile_case_lambda(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "delay")) {
        return compile_delay(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "delay-force")) {
        return compile_delay_force(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "define-values")) {
        return compile_define_values(c, fc, args, dst);
    }
    if (is_symbol_named(head, "and")) {
        return compile_and(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "or")) {
        return compile_or(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "let")) {
        return compile_let(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "let*")) {
        return compile_let_star(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "letrec")) {
        return compile_letrec(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "letrec*")) {
        /* MVP: reuse letrec lowering; semantic gaps are tracked by suite failures. */
        return compile_letrec(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "let-values")) {
        return compile_let_values(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "let*-values")) {
        return compile_let_star_values(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "cond")) {
        return compile_cond(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "case")) {
        return compile_case(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "when")) {
        return compile_when_unless(c, fc, args, dst, tail, 1);
    }
    if (is_symbol_named(head, "unless")) {
        return compile_when_unless(c, fc, args, dst, tail, 0);
    }
    if (is_symbol_named(head, "do")) {
        return compile_do(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "guard")) {
        return compile_guard(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "parameterize")) {
        return compile_parameterize(c, fc, args, dst, tail);
    }
    return compile_call(c, fc, expr, dst, tail);
}

ChCompileStatus ch_compile_toplevel(ChCompiler *c, ChValue expr, ChFunction **out_fn) {
    c->error[0] = '\0';
    size_t compile_base = c->vm->gc.root_count;

    /* Root globals/macros before any allocation — first GC must see them. */
    for (size_t i = 0; i < c->vm->global_count; i++) {
        ch_gc_push(&c->vm->gc, &c->vm->globals[i].value);
    }
    for (size_t i = 0; i < c->vm->macro_count; i++) {
        ch_gc_push(&c->vm->gc, &c->vm->macros[i].transformer);
    }

    ChValue expr_r = expr;
    ch_gc_push(&c->vm->gc, &expr_r);
    {
        ChValue expanded = CH_NIL;
        ch_gc_push(&c->vm->gc, &expanded);
        char err[256];
        if (ch_expand_toplevel(c->vm, expr_r, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
            snprintf(c->error, sizeof(c->error), "%s", err);
            ch_gc_pop_to(&c->vm->gc, compile_base);
            return CH_COMPILE_ERROR;
        }
        expr_r = expanded;
        ch_gc_pop(&c->vm->gc);
    }

    /* Expansion / define-syntax may grow globals or macros — re-root the full set. */
    ch_gc_pop_to(&c->vm->gc, compile_base);
    for (size_t i = 0; i < c->vm->global_count; i++) {
        ch_gc_push(&c->vm->gc, &c->vm->globals[i].value);
    }
    for (size_t i = 0; i < c->vm->macro_count; i++) {
        ch_gc_push(&c->vm->gc, &c->vm->macros[i].transformer);
    }
    ch_gc_push(&c->vm->gc, &expr_r);

    ChValue fn_v = ch_gc_make_function(&c->vm->gc);
    ch_gc_push(&c->vm->gc, &fn_v);
    ChFunction *fn = ch_as_function(fn_v);
    ChFuncCompiler fc;
    fc_init(c, &fc, NULL, fn, true);
    fn->arity = 0;
    fn->variadic = 0;

    uint8_t dst = alloc_reg(&fc);
    ChIrNode *ir_root = NULL;
    if (ch_ir_lower(c, expr_r, &ir_root) != CH_COMPILE_OK) {
        fc_discard(c, &fc);
        ch_gc_pop_to(&c->vm->gc, compile_base);
        return CH_COMPILE_ERROR;
    }
    ch_ir_analyze(ir_root);
    if (ch_ir_optimize(c, &ir_root) != CH_COMPILE_OK) {
        ch_ir_free(ir_root);
        fc_discard(c, &fc);
        ch_gc_pop_to(&c->vm->gc, compile_base);
        return CH_COMPILE_ERROR;
    }
    ChIrLegacyEmitCtx ir_emit_ctx = {
        .compiler = c,
        .fc = &fc,
    };
    if (ch_ir_emit(c, ir_root, emit_ir_with_legacy, &ir_emit_ctx, dst, false) != CH_COMPILE_OK) {
        ch_ir_free(ir_root);
        fc_discard(c, &fc);
        ch_gc_pop_to(&c->vm->gc, compile_base);
        return CH_COMPILE_ERROR;
    }
    ch_ir_free(ir_root);
    emit_byte(&fc, CH_OP_HALT);
    if (finish_function(c, &fc) != CH_COMPILE_OK) {
        fc_discard(c, &fc);
        ch_gc_pop_to(&c->vm->gc, compile_base);
        return CH_COMPILE_ERROR;
    }
    fc_end_compile(c, &fc);
    *out_fn = fn;
    ch_gc_pop_to(&c->vm->gc, compile_base);
    return CH_COMPILE_OK;
}
