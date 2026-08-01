#include "chaaya/compiler.h"

#include "chaaya/opcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_MAX_LOCALS 256
#define CH_MAX_UPVALUES 64
#define CH_MAX_CONSTS 512
#define CH_CODE_INIT 256

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

static void fc_init(ChFuncCompiler *fc, ChFuncCompiler *enclosing, ChFunction *fn, bool toplevel) {
    memset(fc, 0, sizeof(*fc));
    fc->enclosing = enclosing;
    fc->fn = fn;
    fc->is_toplevel = toplevel;
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

static void fc_free_buf(ChFuncCompiler *fc) {
    free(fc->code);
    fc->code = NULL;
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

static int add_constant(ChCompiler *c, ChFuncCompiler *fc, ChValue v) {
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

static size_t list_length(ChValue v) {
    size_t n = 0;
    while (ch_is_pair(v)) {
        n++;
        v = ch_cdr(v);
    }
    return n;
}

static bool is_symbol_named(ChValue v, const char *name) {
    return ch_is_symbol(v) && strcmp(ch_as_symbol(v)->name, name) == 0;
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
    int g = ch_vm_intern_global(c->vm, name);
    emit_byte(fc, CH_OP_SET_GLOBAL);
    emit_u16(fc, (uint16_t)g);
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
    int g = ch_vm_intern_global(c->vm, name);
    emit_byte(fc, CH_OP_DEFINE_GLOBAL);
    emit_u16(fc, (uint16_t)g);
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
    ch_gc_push(&c->vm->gc, &fn_v);

    ChFuncCompiler child;
    fc_init(&child, fc, fn, false);
    begin_scope(&child);

    /* bind parameters as locals 0.. */
    bool variadic = false;
    ChValue p = params;
    while (ch_is_pair(p)) {
        if (!ch_is_symbol(ch_car(p))) {
            fc_free_buf(&child);
            ch_gc_pop(&c->vm->gc);
            return fail(c, "lambda: parameter must be a symbol");
        }
        if (add_local(c, &child, ch_as_symbol(ch_car(p))) < 0) {
            fc_free_buf(&child);
            ch_gc_pop(&c->vm->gc);
            return CH_COMPILE_ERROR;
        }
        p = ch_cdr(p);
    }
    if (ch_is_symbol(p)) {
        variadic = true;
        if (add_local(c, &child, ch_as_symbol(p)) < 0) {
            fc_free_buf(&child);
            ch_gc_pop(&c->vm->gc);
            return CH_COMPILE_ERROR;
        }
    } else if (!ch_is_nil(p)) {
        fc_free_buf(&child);
        ch_gc_pop(&c->vm->gc);
        return fail(c, "lambda: bad parameter list");
    }

    fn->arity = (uint8_t)(variadic ? child.local_count - 1 : child.local_count);
    fn->variadic = variadic ? 1 : 0;
    child.next_reg = (uint8_t)child.local_count;
    child.max_regs = child.next_reg;

    ensure_temps_from(&child);
    uint8_t body_dst = alloc_reg(&child);
    if (compile_begin(c, &child, body, body_dst, true) != CH_COMPILE_OK) {
        fc_free_buf(&child);
        ch_gc_pop(&c->vm->gc);
        return CH_COMPILE_ERROR;
    }
    emit_byte(&child, CH_OP_RETURN);
    emit_byte(&child, body_dst);
    end_scope(&child);

    if (finish_function(c, &child) != CH_COMPILE_OK) {
        fc_free_buf(&child);
        ch_gc_pop(&c->vm->gc);
        return CH_COMPILE_ERROR;
    }

    int idx = add_constant(c, fc, fn_v);
    ch_gc_pop(&c->vm->gc);
    if (idx < 0) {
        return CH_COMPILE_ERROR;
    }
    emit_byte(fc, CH_OP_CLOSURE);
    emit_byte(fc, dst);
    emit_u16(fc, (uint16_t)idx);
    return CH_COMPILE_OK;
}

static ChCompileStatus finish_function(ChCompiler *c, ChFuncCompiler *fc) {
    (void)c;
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
    int g = ch_vm_intern_global(c->vm, name);
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
static ChCompileStatus compile_let(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                   bool tail) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "let: bad syntax");
    }
    ChValue bindings = ch_car(args);
    ChValue body = ch_cdr(args);

    ChValue params = CH_NIL;
    ChValue vals = CH_NIL;
    ch_gc_push(&c->vm->gc, &params);
    ch_gc_push(&c->vm->gc, &vals);

    ChValue b = bindings;
    while (ch_is_pair(b)) {
        ChValue bind = ch_car(b);
        if (!ch_is_pair(bind) || !ch_is_pair(ch_cdr(bind))) {
            ch_gc_pop_n(&c->vm->gc, 2);
            return fail(c, "let: bad binding");
        }
        ChValue name = ch_car(bind);
        ChValue init = ch_car(ch_cdr(bind));
        params = ch_gc_cons(&c->vm->gc, name, params);
        vals = ch_gc_cons(&c->vm->gc, init, vals);
        b = ch_cdr(b);
    }
    if (!ch_is_nil(b)) {
        ch_gc_pop_n(&c->vm->gc, 2);
        return fail(c, "let: bad bindings list");
    }

    /* reverse params and vals */
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

    ChValue lambda_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "lambda");
    ChValue lambda_args = ch_gc_cons(&c->vm->gc, rp, body);
    ChValue lambda = ch_gc_cons(&c->vm->gc, lambda_sym, lambda_args);
    ChValue call = ch_gc_cons(&c->vm->gc, lambda, rv);
    ch_gc_pop_n(&c->vm->gc, 4);
    return compile_expr(c, fc, call, dst, tail);
}

static ChCompileStatus compile_expr(ChCompiler *c, ChFuncCompiler *fc, ChValue expr, uint8_t dst,
                                    bool tail) {
    if (ch_is_symbol(expr)) {
        return compile_variable(c, fc, ch_as_symbol(expr), dst);
    }
    if (!ch_is_pair(expr)) {
        /* self-evaluating */
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
    if (is_symbol_named(head, "quote")) {
        return compile_quote(c, fc, args, dst);
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
    if (is_symbol_named(head, "lambda")) {
        return compile_lambda(c, fc, args, dst);
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
    return compile_call(c, fc, expr, dst, tail);
}

ChCompileStatus ch_compile_toplevel(ChCompiler *c, ChValue expr, ChFunction **out_fn) {
    c->error[0] = '\0';
    ChValue fn_v = ch_gc_make_function(&c->vm->gc);
    ch_gc_push(&c->vm->gc, &fn_v);
    ChValue expr_r = expr;
    ch_gc_push(&c->vm->gc, &expr_r);

    ChFunction *fn = ch_as_function(fn_v);
    ChFuncCompiler fc;
    fc_init(&fc, NULL, fn, true);
    fn->arity = 0;
    fn->variadic = 0;

    uint8_t dst = alloc_reg(&fc);
    if (compile_expr(c, &fc, expr_r, dst, false) != CH_COMPILE_OK) {
        fc_free_buf(&fc);
        ch_gc_pop_n(&c->vm->gc, 2);
        return CH_COMPILE_ERROR;
    }
    emit_byte(&fc, CH_OP_HALT);
    if (finish_function(c, &fc) != CH_COMPILE_OK) {
        fc_free_buf(&fc);
        ch_gc_pop_n(&c->vm->gc, 2);
        return CH_COMPILE_ERROR;
    }
    *out_fn = fn;
    ch_gc_pop_n(&c->vm->gc, 2);
    return CH_COMPILE_OK;
}
