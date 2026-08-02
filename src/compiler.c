#include "compiler_internal.h"

#include "expander_internal.h"

void ch_compiler_init(ChCompiler *c, ChVM *vm) {
    c->vm = vm;
    c->error[0] = '\0';
    c->error_code = (ChDiagCode)0;
    c->error_line = 0;
    c->error_column = 0;
    c->next_binding_id = 1;
}

void ch_compiler_set_location(ChCompiler *c, int line, int column) {
    if (!c) {
        return;
    }
    c->error_line = line;
    c->error_column = column;
}

const char *ch_compiler_error(const ChCompiler *c) {
    return c->error;
}

ChDiagCode ch_compiler_error_code(const ChCompiler *c) {
    if (c->error_code) {
        return c->error_code;
    }
    return ch_diag_classify_message(c->error, CH_DIAG_STAGE_COMPILE);
}

ChCompileStatus fail(ChCompiler *c, const char *msg) {
    snprintf(c->error, sizeof(c->error), "%s", msg);
    c->error_code = ch_diag_classify_message(msg, CH_DIAG_STAGE_COMPILE);
    /* Preserve caller-supplied location when set; otherwise leave 0 (unknown). */
    return CH_COMPILE_ERROR;
}

void fc_init(ChCompiler *c, ChFuncCompiler *fc, ChFuncCompiler *enclosing, ChFunction *fn,
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

void pop_const_roots(ChCompiler *c, ChFuncCompiler *fc) {
    if (fc->const_roots > 0) {
        ch_gc_pop_n(&c->vm->gc, fc->const_roots);
        fc->const_roots = 0;
    }
}

void pop_root_at(ChGC *gc, size_t base) {
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

void fc_end_compile(ChCompiler *c, ChFuncCompiler *fc) {
    pop_const_roots(c, fc);
    ch_gc_pop(&c->vm->gc);
    if (c->vm->gc.compiling_fn_depth > fc->compiling_fn_slot) {
        c->vm->gc.compiling_fn_depth = fc->compiling_fn_slot;
    }
}

void fc_discard(ChCompiler *c, ChFuncCompiler *fc) {
    fc_end_compile(c, fc);
    fc_free_buf(c, fc);
}

void emit_byte(ChFuncCompiler *fc, uint8_t b) {
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

void emit_u16(ChFuncCompiler *fc, uint16_t v) {
    emit_byte(fc, (uint8_t)(v & 0xFF));
    emit_byte(fc, (uint8_t)((v >> 8) & 0xFF));
}

void emit_i16(ChFuncCompiler *fc, int16_t v) {
    emit_u16(fc, (uint16_t)v);
}

size_t emit_jump(ChFuncCompiler *fc, ChOpCode op) {
    emit_byte(fc, (uint8_t)op);
    size_t at = fc->code_len;
    emit_i16(fc, 0);
    return at;
}

size_t emit_jump_test(ChFuncCompiler *fc, ChOpCode op, uint8_t test) {
    emit_byte(fc, (uint8_t)op);
    emit_byte(fc, test);
    size_t at = fc->code_len;
    emit_i16(fc, 0);
    return at;
}

void patch_jump(ChFuncCompiler *fc, size_t at) {
    int32_t offset = (int32_t)fc->code_len - (int32_t)(at + 2);
    if (offset < -32768 || offset > 32767) {
        abort();
    }
    fc->code[at] = (uint8_t)(offset & 0xFF);
    fc->code[at + 1] = (uint8_t)((offset >> 8) & 0xFF);
}

uint8_t alloc_reg(ChFuncCompiler *fc) {
    if (fc->next_reg >= 250) {
        abort();
    }
    uint8_t r = fc->next_reg++;
    if (r + 1 > fc->max_regs) {
        fc->max_regs = (uint8_t)(r + 1);
    }
    return r;
}

void reset_regs(ChFuncCompiler *fc, uint8_t saved) {
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

int add_constant(ChCompiler *c, ChFuncCompiler *fc, ChValue v) {
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

int resolve_local(ChFuncCompiler *fc, ChSymbol *name) {
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

int resolve_upvalue(ChFuncCompiler *fc, ChSymbol *name) {
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

/* Last-resort lookup for a hygienically-renamed identifier (__hyg_N_base) that
 * has no binding under its exact renamed identity anywhere in scope. This
 * happens when a syntax-rules template free-references a name that the macro
 * never binds itself (e.g. a record accessor defined earlier in the same
 * body, referenced from a sibling define-syntax) — hyg_rename mints a fresh
 * symbol for any free identifier that isn't already a VM global, since at
 * expansion time it can't see body-local bindings that don't exist yet.
 * Falling back to a basename match only fires when exact resolution already
 * failed, so it never shadows the hygiene renaming does provide: a
 * template-introduced binding and its own internal references always share
 * the exact same renamed symbol and resolve via resolve_local/resolve_upvalue
 * above before this is ever consulted. */
static int resolve_local_by_basename(ChFuncCompiler *fc, const char *base) {
    for (int i = fc->local_count - 1; i >= 0; i--) {
        if (strcmp(ch_symbol_basename(fc->locals[i].name), base) == 0) {
            return i;
        }
    }
    return -1;
}

static int resolve_upvalue_by_basename(ChFuncCompiler *fc, const char *base) {
    if (!fc->enclosing) {
        return -1;
    }
    int local = resolve_local_by_basename(fc->enclosing, base);
    if (local != -1) {
        fc->enclosing->locals[local].is_captured = true;
        return add_upvalue(fc, (uint8_t)local, true);
    }
    int up = resolve_upvalue_by_basename(fc->enclosing, base);
    if (up != -1) {
        return add_upvalue(fc, (uint8_t)up, false);
    }
    return -1;
}

void begin_scope(ChFuncCompiler *fc) {
    fc->scope_depth++;
}

void end_scope(ChCompiler *c, ChFuncCompiler *fc) {
    while (fc->local_count > 0 && fc->locals[fc->local_count - 1].depth == fc->scope_depth) {
        fc->local_count--;
    }
    while (fc->n_body_macros > 0 &&
           fc->body_macros[fc->n_body_macros - 1].depth == fc->scope_depth) {
        fc->n_body_macros--;
        ChBodyMacroSave *save = &fc->body_macros[fc->n_body_macros];
        if (save->old_transformer == CH_NIL) {
            const char *base = ch_symbol_basename(save->name);
            for (size_t i = 0; i < c->vm->macro_count; i++) {
                if (strcmp(ch_symbol_basename(c->vm->macros[i].name), base) == 0) {
                    c->vm->macro_count--;
                    c->vm->macros[i] = c->vm->macros[c->vm->macro_count];
                    break;
                }
            }
        } else {
            ch_vm_define_macro(c->vm, save->name, ch_as_transformer(save->old_transformer));
        }
    }
    fc->scope_depth--;
}

int add_local(ChCompiler *c, ChFuncCompiler *fc, ChSymbol *name) {
    if (fc->local_count >= CH_MAX_LOCALS) {
        fail(c, "too many locals");
        return -1;
    }
    (void)alloc_reg(fc);
    fc->locals[fc->local_count].name = name;
    fc->locals[fc->local_count].depth = (uint8_t)fc->scope_depth;
    fc->locals[fc->local_count].is_captured = false;
    fc->locals[fc->local_count].binding_id = c->next_binding_id++;
    return fc->local_count++;
}

uint8_t local_reg(ChFuncCompiler *fc, int local_index) {
    (void)fc;
    return (uint8_t)local_index;
}

void ensure_temps_from(ChFuncCompiler *fc) {
    if (fc->next_reg < (uint8_t)fc->local_count) {
        fc->next_reg = (uint8_t)fc->local_count;
    }
    if (fc->next_reg > fc->max_regs) {
        fc->max_regs = fc->next_reg;
    }
}

int in_restricted_eval_env(ChCompiler *c) {
    return c->vm->active_eval_env != NULL;
}

int eval_env_immutable(ChCompiler *c) {
    return c->vm->active_eval_env != NULL;
}

static uint32_t resolve_use_site_binding(const void *ctx, const char *name) {
    const ChFuncCompiler *fc = ctx;
    for (const ChFuncCompiler *f = fc; f; f = f->enclosing) {
        for (int i = f->local_count - 1; i >= 0; i--) {
            if (strcmp(ch_symbol_basename(f->locals[i].name), name) == 0) {
                return CH_LITERAL_LOCAL_BASE | f->locals[i].binding_id;
            }
        }
    }
    return CH_LITERAL_UNBOUND;
}

typedef struct ChIrLegacyEmitCtx {
    ChCompiler *compiler;
    ChFuncCompiler *fc;
} ChIrLegacyEmitCtx;

static ChCompileStatus emit_ir_with_legacy(void *ctx, ChValue expr, uint8_t dst, bool tail) {
    ChIrLegacyEmitCtx *emit_ctx = (ChIrLegacyEmitCtx *)ctx;
    return compile_expr(emit_ctx->compiler, emit_ctx->fc, expr, dst, tail);
}

size_t list_length(ChValue v) {
    size_t n = 0;
    while (ch_is_pair(v)) {
        n++;
        v = ch_cdr(v);
    }
    return n;
}

bool is_symbol_named(ChValue v, const char *name) {
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

ChCompileStatus compile_begin(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
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

ChCompileStatus finish_function(ChCompiler *c, ChFuncCompiler *fc) {
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
    /* Compiler-synthesized pristine internal (#1856): load the snapshot value
     * as a constant so library imports / top-level defines of the same bare
     * name cannot redirect desugarings. */
    {
        const char *n = name->name;
        size_t plen = strlen(CH_BASE_BINDING_PREFIX);
        if (strncmp(n, CH_BASE_BINDING_PREFIX, plen) == 0) {
            ChValue prim = CH_UNDEFINED;
            if (!ch_lookup_internal_binding(c->vm, n + plen, &prim)) {
                return fail(c, "unbound internal binding");
            }
            int idx = add_constant(c, fc, prim);
            if (idx < 0) {
                return CH_COMPILE_ERROR;
            }
            emit_byte(fc, CH_OP_LOAD_CONST);
            emit_byte(fc, dst);
            emit_u16(fc, (uint16_t)idx);
            return CH_COMPILE_OK;
        }
    }
    const char *name_base = ch_symbol_basename(name);
    if (name_base != name->name) {
        int blocal = resolve_local_by_basename(fc, name_base);
        if (blocal != -1) {
            emit_byte(fc, CH_OP_MOVE);
            emit_byte(fc, dst);
            emit_byte(fc, local_reg(fc, blocal));
            return CH_COMPILE_OK;
        }
        int bup = resolve_upvalue_by_basename(fc, name_base);
        if (bup != -1) {
            emit_byte(fc, CH_OP_GET_UPVALUE);
            emit_byte(fc, dst);
            emit_byte(fc, (uint8_t)bup);
            return CH_COMPILE_OK;
        }
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
            if (in_restricted_eval_env(c)) {
                return fail(c, "unbound variable");
            }
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
    if (in_restricted_eval_env(c)) {
        return fail(c, "unbound variable");
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

static ChCompileStatus compile_expr_impl(ChCompiler *c, ChFuncCompiler *fc, ChValue expr,
                                         uint8_t dst, bool tail);

ChCompileStatus compile_expr(ChCompiler *c, ChFuncCompiler *fc, ChValue expr, uint8_t dst,
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

    /* Local/upvalue bindings shadow keywords and primitives (#788). Hygienic
     * renames (__hyg_*) still denote the original keyword. Core special forms
     * still beat macro shadows (#1718) when the head is not lexically bound. */
    bool head_shadowed = false;
    if (ch_is_symbol(head)) {
        ChSymbol *hsym = ch_as_symbol(head);
        if (strncmp(hsym->name, "__hyg_", 6) != 0) {
            for (ChFuncCompiler *p = fc; p; p = p->enclosing) {
                for (int i = 0; i < p->local_count; i++) {
                    if (strcmp(p->locals[i].name->name, hsym->name) == 0) {
                        head_shadowed = true;
                        break;
                    }
                }
                if (head_shadowed) {
                    break;
                }
            }
        }
    }

    if (!head_shadowed && is_symbol_named(head, "quote")) {
        return compile_quote(c, fc, args, dst);
    }
    if (!head_shadowed && is_symbol_named(head, "quasiquote")) {
        return compile_quasiquote_real(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "if")) {
        return compile_if(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "begin")) {
        return compile_begin(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "set!")) {
        return compile_set(c, fc, args, dst);
    }
    if (!head_shadowed && is_symbol_named(head, "define")) {
        return compile_define(c, fc, args, dst);
    }
    if (!head_shadowed && is_symbol_named(head, "define-syntax")) {
        return compile_define_syntax(c, fc, args, dst);
    }
    if (!head_shadowed && is_symbol_named(head, "define-property")) {
        return compile_define_property(c, fc, args, dst);
    }
    if (!head_shadowed && is_symbol_named(head, "let-syntax")) {
        return compile_let_syntax(c, fc, args, dst, tail, 0);
    }
    if (!head_shadowed && is_symbol_named(head, "letrec-syntax")) {
        return compile_let_syntax(c, fc, args, dst, tail, 1);
    }
    if (!head_shadowed && is_symbol_named(head, "cond-expand")) {
        return compile_cond_expand(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "lambda")) {
        return compile_lambda(c, fc, args, dst);
    }
    if (!head_shadowed && is_symbol_named(head, "case-lambda")) {
        return compile_case_lambda(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "delay")) {
        return compile_delay(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "delay-force")) {
        return compile_delay_force(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "define-values")) {
        return compile_define_values(c, fc, args, dst);
    }

    /* Macros beat derived special forms (when/let/cond/…) so let-syntax can
     * rebind them; core forms above still beat macros (#1718 / R7RS when+if).
     * Head-position chains trampoline here (#1796) so compileExpr recursion
     * does not grow with chain length. */
    if (ch_is_symbol(head)) {
        ChTransformer *tr = ch_vm_lookup_macro(c->vm, ch_as_symbol(head));
        if (tr) {
            ChValue cur = expr;
            ch_gc_push(&c->vm->gc, &cur);
            char err[256];
            ChLibEnv *saved_lib = c->vm->active_lib_env;
            ChUseSiteBindingCheck use_check = {
                .ctx = fc,
                .resolve = resolve_use_site_binding,
            };
            for (int steps = 0;; steps++) {
                if (steps >= CH_EXPAND_STEP_MAX) {
                    c->vm->active_lib_env = saved_lib;
                    ch_gc_pop(&c->vm->gc);
                    return fail(c, "macro expansion step limit exceeded");
                }
                ChLibEnv *home = NULL;
                const char *base = ch_symbol_basename(ch_as_symbol(ch_car(cur)));
                for (size_t i = 0; i < c->vm->macro_count; i++) {
                    if (strcmp(ch_symbol_basename(c->vm->macros[i].name), base) == 0) {
                        home = c->vm->macros[i].home_env;
                        break;
                    }
                }
                c->vm->active_lib_env = home ? home : saved_lib;
                ChValue expanded = CH_NIL;
                ch_gc_push(&c->vm->gc, &expanded);
                if (ch_expand_macro_checked(c->vm, tr, cur, &use_check, &expanded, err,
                                            sizeof(err)) != CH_EXPAND_OK) {
                    c->vm->active_lib_env = saved_lib;
                    ch_gc_pop_n(&c->vm->gc, 2);
                    return fail(c, err);
                }
                cur = expanded;
                ch_gc_pop(&c->vm->gc);
                if (!ch_is_pair(cur) || !ch_is_symbol(ch_car(cur))) {
                    break;
                }
                tr = ch_vm_lookup_macro(c->vm, ch_as_symbol(ch_car(cur)));
                if (!tr) {
                    break;
                }
            }
            c->vm->active_lib_env = saved_lib;
            size_t root_base = c->vm->gc.root_count - 1;
            ChCompileStatus st = compile_expr(c, fc, cur, dst, tail);
            pop_root_at(&c->vm->gc, root_base);
            return st;
        }
    }

    if (!head_shadowed && is_symbol_named(head, "and")) {
        return compile_and(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "or")) {
        return compile_or(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "let")) {
        return compile_let(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "let*")) {
        return compile_let_star(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "letrec")) {
        return compile_letrec(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "letrec*")) {
        return compile_letrec(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "let-values")) {
        return compile_let_values(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "let*-values")) {
        return compile_let_star_values(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "cond")) {
        return compile_cond(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "case")) {
        return compile_case(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "when")) {
        return compile_when_unless(c, fc, args, dst, tail, 1);
    }
    if (!head_shadowed && is_symbol_named(head, "unless")) {
        return compile_when_unless(c, fc, args, dst, tail, 0);
    }
    if (!head_shadowed && is_symbol_named(head, "do")) {
        return compile_do(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "guard")) {
        return compile_guard(c, fc, args, dst, tail);
    }
    if (!head_shadowed && is_symbol_named(head, "parameterize")) {
        return compile_parameterize(c, fc, args, dst, tail);
    }

    return compile_call(c, fc, expr, dst, tail);
}

ChCompileStatus ch_compile_toplevel(ChCompiler *c, ChValue expr, ChFunction **out_fn) {
    c->error[0] = '\0';
    size_t compile_base = c->vm->gc.root_count;

    /* Globals/macros are marked via ch_vm_mark_gc_roots; only root locals. */
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
