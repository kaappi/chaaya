#include "chaaya/compiler.h"

#include "chaaya/expander.h"
#include "chaaya/library.h"
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

static void pop_const_roots(ChCompiler *c, ChFuncCompiler *fc) {
    if (fc->const_roots > 0) {
        ch_gc_pop_n(&c->vm->gc, fc->const_roots);
        fc->const_roots = 0;
    }
}

static void fc_free_buf(ChCompiler *c, ChFuncCompiler *fc) {
    pop_const_roots(c, fc);
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
    ch_gc_push(&c->vm->gc, &fn_v);

    ChFuncCompiler child;
    fc_init(&child, fc, fn, false);
    begin_scope(&child);

    /* bind parameters as locals 0.. */
    bool variadic = false;
    ChValue p = params;
    while (ch_is_pair(p)) {
        if (!ch_is_symbol(ch_car(p))) {
            fc_free_buf(c, &child);
            ch_gc_pop(&c->vm->gc);
            return fail(c, "lambda: parameter must be a symbol");
        }
        if (add_local(c, &child, ch_as_symbol(ch_car(p))) < 0) {
            fc_free_buf(c, &child);
            ch_gc_pop(&c->vm->gc);
            return CH_COMPILE_ERROR;
        }
        p = ch_cdr(p);
    }
    if (ch_is_symbol(p)) {
        variadic = true;
        if (add_local(c, &child, ch_as_symbol(p)) < 0) {
            fc_free_buf(c, &child);
            ch_gc_pop(&c->vm->gc);
            return CH_COMPILE_ERROR;
        }
    } else if (!ch_is_nil(p)) {
        fc_free_buf(c, &child);
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
        fc_free_buf(c, &child);
        ch_gc_pop(&c->vm->gc);
        return CH_COMPILE_ERROR;
    }
    emit_byte(&child, CH_OP_RETURN);
    emit_byte(&child, body_dst);
    end_scope(&child);

    if (finish_function(c, &child) != CH_COMPILE_OK) {
        fc_free_buf(c, &child);
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
    pop_const_roots(c, fc);
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
        if (lidx >= 0) {
            emit_byte(fc, CH_OP_GET_GLOBAL);
            emit_byte(fc, dst);
            emit_u16(fc, (uint16_t)(CH_ENV_LIB_BIT | (unsigned)lidx));
            return CH_COMPILE_OK;
        }
    }
    /* Hygienic renames of free ids still resolve to the basename global. */
    const char *base = ch_symbol_basename(name);
    ChValue base_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, base);
    int g = ch_vm_intern_global(c->vm, ch_as_symbol(base_sym));
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

/* (let* ((v e) ...) body...) → nested lets */
static ChCompileStatus compile_let_star(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                        uint8_t dst, bool tail) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "let*: bad syntax");
    }
    ChValue bindings = ch_car(args);
    ChValue body = ch_cdr(args);
    if (ch_is_nil(bindings)) {
        ChValue begin_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "begin");
        ChValue form = ch_gc_cons(&c->vm->gc, begin_sym, body);
        return compile_expr(c, fc, form, dst, tail);
    }
    if (!ch_is_pair(bindings)) {
        return fail(c, "let*: bad bindings");
    }
    ChValue first = ch_car(bindings);
    ChValue rest = ch_cdr(bindings);
    ChValue let_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let");
    ChValue let_star = ch_gc_intern_symbol_cstr(&c->vm->gc, "let*");
    ChValue inner = ch_gc_cons(&c->vm->gc, let_star, ch_gc_cons(&c->vm->gc, rest, body));
    ChValue binds1 = ch_gc_cons(&c->vm->gc, first, CH_NIL);
    ChValue form =
        ch_gc_cons(&c->vm->gc, let_sym, ch_gc_cons(&c->vm->gc, binds1, ch_gc_cons(&c->vm->gc, inner, CH_NIL)));
    return compile_expr(c, fc, form, dst, tail);
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
    ch_gc_pop_n(&c->vm->gc, 9);
    return compile_expr(c, fc, form, dst, tail);
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
    /* (=> recipient) */
    if (ch_is_pair(exprs) && is_symbol_named(ch_car(exprs), "=>")) {
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

static ChValue qq_expand(ChCompiler *c, ChValue x, int level);

static ChValue qq_expand_list(ChCompiler *c, ChValue xs, int level) {
    if (ch_is_nil(xs)) {
        return ch_gc_cons(&c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "quote"),
                          ch_gc_cons(&c->vm->gc, CH_NIL, CH_NIL));
    }
    if (!ch_is_pair(xs)) {
        return qq_expand(c, xs, level);
    }
    ChValue head = ch_car(xs);
    if (level == 0 && ch_is_pair(head) && is_symbol_named(ch_car(head), "unquote-splicing")) {
        ChValue uargs = ch_cdr(head);
        if (!ch_is_pair(uargs)) {
            return CH_FALSE; /* signal error via false — caller checks */
        }
        ChValue rest = qq_expand_list(c, ch_cdr(xs), level);
        ch_gc_push(&c->vm->gc, &rest);
        ChValue app = ch_gc_cons(
            &c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "append"),
            ch_gc_cons(&c->vm->gc, ch_car(uargs), ch_gc_cons(&c->vm->gc, rest, CH_NIL)));
        ch_gc_pop(&c->vm->gc);
        return app;
    }
    ChValue car_e = qq_expand(c, head, level);
    ch_gc_push(&c->vm->gc, &car_e);
    ChValue cdr_e = qq_expand_list(c, ch_cdr(xs), level);
    ch_gc_push(&c->vm->gc, &cdr_e);
    ChValue out = ch_gc_cons(
        &c->vm->gc, ch_gc_intern_symbol_cstr(&c->vm->gc, "cons"),
        ch_gc_cons(&c->vm->gc, car_e, ch_gc_cons(&c->vm->gc, cdr_e, CH_NIL)));
    ch_gc_pop_n(&c->vm->gc, 2);
    return out;
}

static ChValue qq_expand(ChCompiler *c, ChValue x, int level) {
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
    ChValue expanded = qq_expand(c, ch_car(args), 0);
    if (expanded == CH_FALSE) {
        return fail(c, "quasiquote: bad syntax");
    }
    return compile_expr(c, fc, expanded, dst, tail);
}

static ChCompileStatus compile_expr_impl(ChCompiler *c, ChFuncCompiler *fc, ChValue expr,
                                         uint8_t dst, bool tail);

static ChCompileStatus compile_expr(ChCompiler *c, ChFuncCompiler *fc, ChValue expr, uint8_t dst,
                                    bool tail) {
    /* Keep heap exprs rooted across desugaring allocations inside compile. */
    if (!ch_is_pointer(expr)) {
        return compile_expr_impl(c, fc, expr, dst, tail);
    }
    ChValue rooted = expr;
    ch_gc_push(&c->vm->gc, &rooted);
    ChCompileStatus st = compile_expr_impl(c, fc, rooted, dst, tail);
    ch_gc_pop(&c->vm->gc);
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
            if (ch_expand_macro(c->vm, tr, expr, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
                ch_gc_pop(&c->vm->gc);
                return fail(c, err);
            }
            ChCompileStatus st = compile_expr(c, fc, expanded, dst, tail);
            ch_gc_pop(&c->vm->gc);
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
    if (is_symbol_named(head, "lambda")) {
        return compile_lambda(c, fc, args, dst);
    }
    if (is_symbol_named(head, "case-lambda")) {
        return compile_case_lambda(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "delay")) {
        return compile_delay(c, fc, args, dst, tail);
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
    if (is_symbol_named(head, "cond")) {
        return compile_cond(c, fc, args, dst, tail);
    }
    if (is_symbol_named(head, "when")) {
        return compile_when_unless(c, fc, args, dst, tail, 1);
    }
    if (is_symbol_named(head, "unless")) {
        return compile_when_unless(c, fc, args, dst, tail, 0);
    }
    return compile_call(c, fc, expr, dst, tail);
}

ChCompileStatus ch_compile_toplevel(ChCompiler *c, ChValue expr, ChFunction **out_fn) {
    c->error[0] = '\0';

    /* Root globals/macros before any allocation — first GC must see them. */
    for (size_t i = 0; i < c->vm->global_count; i++) {
        ch_gc_push(&c->vm->gc, &c->vm->globals[i].value);
    }
    for (size_t i = 0; i < c->vm->macro_count; i++) {
        ch_gc_push(&c->vm->gc, &c->vm->macros[i].transformer);
    }
    size_t sticky_roots = c->vm->global_count + c->vm->macro_count;

    ChValue fn_v = ch_gc_make_function(&c->vm->gc);
    ch_gc_push(&c->vm->gc, &fn_v);
    ChValue expr_r = expr;
    ch_gc_push(&c->vm->gc, &expr_r);

    {
        ChValue expanded = CH_NIL;
        ch_gc_push(&c->vm->gc, &expanded);
        char err[256];
        if (ch_expand_toplevel(c->vm, expr_r, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
            snprintf(c->error, sizeof(c->error), "%s", err);
            ch_gc_pop_n(&c->vm->gc, 3 + sticky_roots);
            return CH_COMPILE_ERROR;
        }
        expr_r = expanded;
        ch_gc_pop(&c->vm->gc);
    }

    /* Expansion / define-syntax may grow globals or macros — re-root the full set. */
    ch_gc_pop_n(&c->vm->gc, sticky_roots);
    for (size_t i = 0; i < c->vm->global_count; i++) {
        ch_gc_push(&c->vm->gc, &c->vm->globals[i].value);
    }
    for (size_t i = 0; i < c->vm->macro_count; i++) {
        ch_gc_push(&c->vm->gc, &c->vm->macros[i].transformer);
    }
    sticky_roots = c->vm->global_count + c->vm->macro_count;

    ChFunction *fn = ch_as_function(fn_v);
    ChFuncCompiler fc;
    fc_init(&fc, NULL, fn, true);
    fn->arity = 0;
    fn->variadic = 0;

    uint8_t dst = alloc_reg(&fc);
    if (compile_expr(c, &fc, expr_r, dst, false) != CH_COMPILE_OK) {
        fc_free_buf(c, &fc);
        ch_gc_pop_n(&c->vm->gc, 2 + sticky_roots);
        return CH_COMPILE_ERROR;
    }
    emit_byte(&fc, CH_OP_HALT);
    if (finish_function(c, &fc) != CH_COMPILE_OK) {
        fc_free_buf(c, &fc);
        ch_gc_pop_n(&c->vm->gc, 2 + sticky_roots);
        return CH_COMPILE_ERROR;
    }
    *out_fn = fn;
    ch_gc_pop_n(&c->vm->gc, 2 + sticky_roots);
    return CH_COMPILE_OK;
}
