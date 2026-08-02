#include "compiler_internal.h"

static size_t let_values_gensym_counter = 0;

static int body_to_letrec_star(ChCompiler *c, ChValue body, ChValue *out);

ChCompileStatus compile_set(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst) {
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
    if (eval_env_immutable(c)) {
        return fail(c, "set!: environment is not mutable");
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

ChCompileStatus compile_define(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst) {
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
        uint8_t lreg = local_reg(fc, local);
        if (compile_expr(c, fc, val_expr, lreg, false) != CH_COMPILE_OK) {
            return CH_COMPILE_ERROR;
        }
        /* begin's shared body_dst is allocated before locals and often aliases
         * local 0. Copying the define's value there would clobber an earlier
         * local that open upvalues still point at. Define's value is discarded
         * for non-final body forms, so skip the move when dst is another local. */
        if (dst == lreg || dst >= (uint8_t)fc->local_count) {
            emit_byte(fc, CH_OP_MOVE);
            emit_byte(fc, dst);
            emit_byte(fc, lreg);
        }
        return CH_COMPILE_OK;
    }

    if (eval_env_immutable(c)) {
        return fail(c, "define: environment is not mutable");
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

ChCompileStatus compile_lambda(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst) {
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

    /* R7RS internal defines → letrec* (also splices leading begin of defines
     * from define-record-type expansion). */
    {
        ChValue rewritten = CH_NIL;
        ch_gc_push(&c->vm->gc, &rewritten);
        int rw = body_to_letrec_star(c, body, &rewritten);
        if (rw < 0) {
            ch_gc_pop(&c->vm->gc);
            fc_discard(c, &child);
            return CH_COMPILE_ERROR;
        }
        if (rw > 0) {
            ensure_temps_from(&child);
            uint8_t body_dst = alloc_reg(&child);
            ChCompileStatus st = compile_expr(c, &child, rewritten, body_dst, true);
            ch_gc_pop(&c->vm->gc);
            if (st != CH_COMPILE_OK) {
                fc_discard(c, &child);
                return CH_COMPILE_ERROR;
            }
            emit_byte(&child, CH_OP_RETURN);
            emit_byte(&child, body_dst);
            end_scope(c, &child);
            goto lambda_finish;
        }
        ch_gc_pop(&c->vm->gc);
    }

    ensure_temps_from(&child);
    uint8_t body_dst = alloc_reg(&child);
    if (compile_begin(c, &child, body, body_dst, true) != CH_COMPILE_OK) {
        fc_discard(c, &child);
        return CH_COMPILE_ERROR;
    }
    emit_byte(&child, CH_OP_RETURN);
    emit_byte(&child, body_dst);
    end_scope(c, &child);

lambda_finish:
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
 *     (cond ((= %cl-n arity) (let ((formals from list-ref %cl-args)) body)) ...
 *           (else (error "wrong number of arguments"))))) */
ChCompileStatus compile_case_lambda(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                           uint8_t dst, bool tail) {
    ChGC *gc = &c->vm->gc;
    const int nroots = 7;
    ChValue lambda_sym = ch_gc_intern_symbol_cstr(gc, "lambda");
    ChValue let_sym = ch_gc_intern_symbol_cstr(gc, "let");
    ChValue cond_sym = ch_gc_intern_symbol_cstr(gc, "cond");
    ChValue eq_sym = ch_gc_intern_symbol_cstr(gc, "=");
    ChValue ge_sym = ch_gc_intern_symbol_cstr(gc, ">=");
    /* %length is not a (scheme base) export — safe from user shadowing (#1714). */
    ChValue length_sym = ch_gc_intern_symbol_cstr(gc, "%length");
    ChValue list_ref_sym = ch_gc_intern_symbol_cstr(gc, "list-ref");
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
    ChValue clause_body = CH_NIL;
    ChValue cond_clauses = CH_NIL;
    ChValue outer = CH_NIL;
    ch_gc_push(gc, &rev_clauses);
    ch_gc_push(gc, &scratch);
    ch_gc_push(gc, &piece);
    ch_gc_push(gc, &clause_body);
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
        if (has_rest) {
            /* Rest formals still use apply+lambda — list-ref/list-tail let
             * binding did not expose rest names in clause bodies (#1173). */
            piece = ch_gc_cons(gc, formals, body);
            piece = ch_gc_cons(gc, lambda_sym, piece);
            clause_body = ch_gc_cons(gc, args_sym, CH_NIL);
            clause_body = ch_gc_cons(gc, piece, clause_body);
            clause_body = ch_gc_cons(gc, apply_sym, clause_body);
        } else {
            /* Bind fixed formals via list-ref so bodies can tail-call the
             * case-lambda without apply growing the register file. */
            ChValue binds = CH_NIL;
            int idx = 0;
            for (ChValue f = formals; ch_is_pair(f); f = ch_cdr(f)) {
                if (!ch_is_symbol(ch_car(f))) {
                    ch_gc_pop_n(gc, nroots);
                    return fail(c, "case-lambda: bad formals");
                }
                ChValue name = ch_car(f);
                ChValue ref = ch_gc_cons(gc, ch_make_fixnum(idx), CH_NIL);
                ref = ch_gc_cons(gc, args_sym, ref);
                ref = ch_gc_cons(gc, list_ref_sym, ref);
                ChValue bind = ch_gc_cons(gc, name, ch_gc_cons(gc, ref, CH_NIL));
                binds = ch_gc_cons(gc, bind, binds);
                idx++;
            }
            ChValue rb = CH_NIL;
            while (ch_is_pair(binds)) {
                rb = ch_gc_cons(gc, ch_car(binds), rb);
                binds = ch_cdr(binds);
            }
            clause_body = ch_gc_cons(gc, let_sym, ch_gc_cons(gc, rb, body));
        }
        /* piece = (scratch clause_body) */
        piece = ch_gc_cons(gc, clause_body, CH_NIL);
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

ChCompileStatus compile_delay(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
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

ChCompileStatus compile_delay_force(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
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
        ChValue producer = CH_NIL;
        ChValue cwv = CH_NIL;
        ch_gc_push(gc, &producer);
        ch_gc_push(gc, &cwv);
        producer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, expr, CH_NIL)));
        cwv = ch_gc_cons(gc, cwv_sym, ch_gc_cons(gc, producer, ch_gc_cons(gc, list_sym, CH_NIL)));
        *out = ch_gc_cons(gc, set_sym,
                          ch_gc_cons(gc, ch_make_pointer(&rest_name->header), ch_gc_cons(gc, cwv, CH_NIL)));
        ch_gc_pop_n(gc, 2);
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
    ch_gc_push(gc, &consumer_params);
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

    ChValue producer = CH_NIL;
    ChValue consumer = CH_NIL;
    ch_gc_push(gc, &producer);
    ch_gc_push(gc, &consumer);
    consumer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, consumer_params, consumer_body));
    producer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, expr, CH_NIL)));
    *out = ch_gc_cons(gc, cwv_sym, ch_gc_cons(gc, producer, ch_gc_cons(gc, consumer, CH_NIL)));
    ch_gc_pop_n(gc, 4); /* consumer, producer, consumer_params, consumer_body */
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

ChCompileStatus compile_define_values(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
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
ChCompileStatus compile_let_values(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
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

    /* Single-binding tail context: avoid let/apply so body tail calls stay TCO. */
    if (tail && count == 1) {
        ChValue producer = CH_NIL;
        ChValue consumer = CH_NIL;
        ChValue form = CH_NIL;
        ch_gc_push(gc, &producer);
        ch_gc_push(gc, &consumer);
        ch_gc_push(gc, &form);
        producer =
            ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, exprs[0], CH_NIL)));
        consumer =
            ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, formals_arr[0], ch_gc_cons(gc, inner, CH_NIL)));
        form = ch_gc_cons(gc, cwv_sym, ch_gc_cons(gc, producer, ch_gc_cons(gc, consumer, CH_NIL)));
        ChValue form_keep = form;
        ch_gc_pop_n(gc, 4); /* form, consumer, producer, inner */
        return compile_expr(c, fc, form_keep, dst, true);
    }

    for (int i = count - 1; i >= 0; i--) {
        ChValue consumer = CH_NIL;
        ch_gc_push(gc, &consumer);
        ChValue consumer_body = ch_gc_cons(gc, inner, CH_NIL);
        consumer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, formals_arr[i], consumer_body));
        inner = ch_gc_cons(gc, apply_sym, ch_gc_cons(gc, consumer, ch_gc_cons(gc, temp_syms[i], CH_NIL)));
        ch_gc_pop(gc);
    }

    ChValue let_binds = CH_NIL;
    ch_gc_push(gc, &let_binds);
    for (int i = count - 1; i >= 0; i--) {
        ChValue producer = CH_NIL;
        ChValue cwv = CH_NIL;
        ch_gc_push(gc, &producer);
        ch_gc_push(gc, &cwv);
        producer =
            ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, exprs[i], CH_NIL)));
        cwv = ch_gc_cons(gc, cwv_sym, ch_gc_cons(gc, producer, ch_gc_cons(gc, list_sym, CH_NIL)));
        ChValue bind = ch_gc_cons(gc, temp_syms[i], ch_gc_cons(gc, cwv, CH_NIL));
        let_binds = ch_gc_cons(gc, bind, let_binds);
        ch_gc_pop_n(gc, 2);
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
    ChValue producer = CH_NIL;
    ChValue consumer = CH_NIL;
    ch_gc_push(gc, &inner);
    ch_gc_push(gc, &producer);
    ch_gc_push(gc, &consumer);
    if (build_let_star_values(c, rest, body, &inner) != CH_COMPILE_OK) {
        ch_gc_pop_n(gc, 3);
        return CH_COMPILE_ERROR;
    }

    /* Root producer/consumer across nested cons — otherwise a GC during the
     * later allocations can reclaim the producer list and leave a dangling
     * pointer that later looks like a bare function constant (LOAD_CONST). */
    producer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, CH_NIL, ch_gc_cons(gc, expr, CH_NIL)));
    ChValue consumer_body = ch_gc_cons(gc, inner, CH_NIL);
    consumer = ch_gc_cons(gc, lambda_sym, ch_gc_cons(gc, formals, consumer_body));
    *out = ch_gc_cons(gc, cwv_sym, ch_gc_cons(gc, producer, ch_gc_cons(gc, consumer, CH_NIL)));
    ch_gc_pop_n(gc, 3);
    return CH_COMPILE_OK;
}

ChCompileStatus compile_let_star_values(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
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

/* (let ((v e) ...) body...) → ((lambda (v ...) body...) e ...) */
static bool is_define_form(ChValue expr) {
    return ch_is_pair(expr) && ch_is_symbol(ch_car(expr)) &&
           strcmp(ch_symbol_basename(ch_as_symbol(ch_car(expr))), "define") == 0;
}

static bool is_begin_form(ChValue expr) {
    return ch_is_pair(expr) && ch_is_symbol(ch_car(expr)) &&
           strcmp(ch_symbol_basename(ch_as_symbol(ch_car(expr))), "begin") == 0;
}

static int parse_define_binding(ChCompiler *c, ChValue def, ChSymbol **name_out, ChValue *init_out) {
    ChValue drest = ch_cdr(def);
    if (!ch_is_pair(drest)) {
        fail(c, "define: bad syntax");
        return -1;
    }
    ChValue name_form = ch_car(drest);
    ChValue init_forms = ch_cdr(drest);
    if (ch_is_symbol(name_form)) {
        if (!ch_is_pair(init_forms) || !ch_is_nil(ch_cdr(init_forms))) {
            fail(c, "define: bad syntax");
            return -1;
        }
        *name_out = ch_as_symbol(name_form);
        *init_out = ch_car(init_forms);
        return 0;
    }
    if (ch_is_pair(name_form) && ch_is_symbol(ch_car(name_form))) {
        *name_out = ch_as_symbol(ch_car(name_form));
        ChValue lambda_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "lambda");
        *init_out = ch_gc_cons(&c->vm->gc, lambda_sym,
                               ch_gc_cons(&c->vm->gc, ch_cdr(name_form), init_forms));
        return 0;
    }
    fail(c, "define: bad syntax");
    return -1;
}

/* Rewrite leading internal defines (splicing begin) to letrec*. Returns 1 and
 * sets *out when rewritten, 0 when there are no leading defines, -1 on error. */
static int body_to_letrec_star(ChCompiler *c, ChValue body, ChValue *out) {
    ChValue work = body;
    ChValue defs = CH_NIL;
    ch_gc_push(&c->vm->gc, &work);
    ch_gc_push(&c->vm->gc, &defs);

    for (;;) {
        while (ch_is_pair(work) && is_begin_form(ch_car(work))) {
            ChValue bargs = ch_cdr(ch_car(work));
            ChValue rest = ch_cdr(work);
            if (ch_is_nil(bargs)) {
                work = rest;
                continue;
            }
            /* Append rest onto a copy of begin's args. */
            ChValue rev = CH_NIL;
            ch_gc_push(&c->vm->gc, &rev);
            for (ChValue p = bargs; ch_is_pair(p); p = ch_cdr(p)) {
                rev = ch_gc_cons(&c->vm->gc, ch_car(p), rev);
            }
            ChValue spliced = rest;
            ch_gc_push(&c->vm->gc, &spliced);
            while (ch_is_pair(rev)) {
                spliced = ch_gc_cons(&c->vm->gc, ch_car(rev), spliced);
                rev = ch_cdr(rev);
            }
            work = spliced;
            ch_gc_pop_n(&c->vm->gc, 2);
        }
        if (!ch_is_pair(work) || !is_define_form(ch_car(work))) {
            break;
        }
        ChSymbol *name = NULL;
        ChValue init = CH_NIL;
        ch_gc_push(&c->vm->gc, &init);
        if (parse_define_binding(c, ch_car(work), &name, &init) != 0) {
            ch_gc_pop_n(&c->vm->gc, 3);
            return -1;
        }
        ChValue bind = ch_gc_cons(&c->vm->gc, ch_make_pointer(&name->header),
                                  ch_gc_cons(&c->vm->gc, init, CH_NIL));
        defs = ch_gc_cons(&c->vm->gc, bind, defs);
        ch_gc_pop(&c->vm->gc);
        work = ch_cdr(work);
    }

    if (ch_is_nil(defs)) {
        ch_gc_pop_n(&c->vm->gc, 2);
        return 0;
    }

    ChValue rdefs = CH_NIL;
    ch_gc_push(&c->vm->gc, &rdefs);
    while (ch_is_pair(defs)) {
        rdefs = ch_gc_cons(&c->vm->gc, ch_car(defs), rdefs);
        defs = ch_cdr(defs);
    }
    ChValue letrec_star = ch_gc_intern_symbol_cstr(&c->vm->gc, "letrec*");
    *out = ch_gc_cons(&c->vm->gc, letrec_star, ch_gc_cons(&c->vm->gc, rdefs, work));
    ch_gc_pop_n(&c->vm->gc, 3);
    return 1;
}

ChCompileStatus compile_let(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
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
    {
        ChValue inner = CH_NIL;
        ch_gc_push(&c->vm->gc, &inner);
        int rw = body_to_letrec_star(c, body, &inner);
        if (rw < 0) {
            ch_gc_pop_n(&c->vm->gc, 3);
            return CH_COMPILE_ERROR;
        }
        if (rw > 0) {
            ChValue let_sym = ch_gc_intern_symbol_cstr(&c->vm->gc, "let");
            ChValue form = ch_gc_cons(&c->vm->gc, let_sym,
                                      ch_gc_cons(&c->vm->gc, bindings,
                                                 ch_gc_cons(&c->vm->gc, inner, CH_NIL)));
            ch_gc_pop_n(&c->vm->gc, 3);
            return compile_expr(c, fc, form, dst, tail);
        }
        ch_gc_pop(&c->vm->gc);
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
ChCompileStatus compile_let_star(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
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
ChCompileStatus compile_letrec(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
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

