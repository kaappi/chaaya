#include "compiler_internal.h"

#include "expander_internal.h"

static size_t guard_gensym_counter = 0;
static size_t parameterize_gensym_counter = 0;
static size_t do_gensym_counter = 0;
static size_t case_gensym_counter = 0;

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

ChCompileStatus compile_cond_expand(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
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

static void let_syntax_setup_captures(ChCompiler *c, ChFuncCompiler *fc, ChTransformer *tr);

ChCompileStatus compile_define_property(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
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

ChCompileStatus compile_define_syntax(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
                                             uint8_t dst) {
    if (!ch_is_pair(args) || !ch_is_symbol(ch_car(args)) || !ch_is_pair(ch_cdr(args))) {
        return fail(c, "define-syntax: bad syntax");
    }
    if (!ch_is_nil(ch_cdr(ch_cdr(args)))) {
        return fail(c, "define-syntax: bad syntax");
    }
    if (eval_env_immutable(c)) {
        return fail(c, "define-syntax: environment is not mutable");
    }

    ChSymbol *name = ch_as_symbol(ch_car(args));
    ChValue spec = ch_car(ch_cdr(args));
    ChTransformer *tr = NULL;
    char err[256];
    if (ch_parse_syntax_rules(c->vm, spec, &tr, err, sizeof(err)) != CH_EXPAND_OK) {
        return fail(c, err);
    }
    capture_transformer_templates(c->vm, tr);
    let_syntax_setup_captures(c, fc, tr);

    /* Snapshot definition-site locals for free template refs (#1644). */
    for (size_t i = 0; i < tr->capture_count; i++) {
        int to_idx = resolve_local(fc, tr->capture_to[i]);
        if (to_idx < 0) {
            if (add_local(c, fc, tr->capture_to[i]) < 0) {
                return CH_COMPILE_ERROR;
            }
            ensure_temps_from(fc);
            to_idx = resolve_local(fc, tr->capture_to[i]);
        }
        uint8_t to_reg = local_reg(fc, to_idx);
        int from_idx = resolve_local(fc, tr->capture_from[i]);
        if (from_idx < 0) {
            const char *base = ch_symbol_basename(tr->capture_from[i]);
            for (int li = fc->local_count - 1; li >= 0; li--) {
                if (li == to_idx) {
                    continue;
                }
                if (strcmp(ch_symbol_basename(fc->locals[li].name), base) == 0) {
                    from_idx = li;
                    break;
                }
            }
        }
        if (from_idx >= 0) {
            emit_byte(fc, CH_OP_MOVE);
            emit_byte(fc, to_reg);
            emit_byte(fc, local_reg(fc, from_idx));
        } else {
            int up = resolve_upvalue(fc, tr->capture_from[i]);
            if (up < 0) {
                return fail(c, "define-syntax: capture of unbound local");
            }
            emit_byte(fc, CH_OP_GET_UPVALUE);
            emit_byte(fc, to_reg);
            emit_byte(fc, (uint8_t)up);
        }
    }

    /* begin's shared dst is allocated before capture locals and may alias them;
     * writing void there would clobber the snapshot (same rule as compile_define). */
    int emit_void = dst >= (uint8_t)fc->local_count;

    /* Library body: keep the transformer in the library env only (#877). */
    if (c->vm->active_lib_env) {
        int idx = ch_lib_env_intern(c->vm->active_lib_env, name);
        if (idx < 0) {
            return fail(c, "define-syntax: library environment full");
        }
        ch_lib_env_define(c->vm->active_lib_env, idx, ch_make_pointer(&tr->header));
        if (emit_void) {
            emit_byte(fc, CH_OP_LOAD_VOID);
            emit_byte(fc, dst);
        }
        return CH_COMPILE_OK;
    }

    /* Internal body: scoped registration restored by end_scope (#651). */
    bool in_body = !fc->is_toplevel || fc->scope_depth > 0;
    if (in_body) {
        if (fc->n_body_macros >= CH_MAX_DERIVED_BINDINGS) {
            return fail(c, "define-syntax: too many body macros");
        }
        ChBodyMacroSave *save = &fc->body_macros[fc->n_body_macros++];
        save->name = name;
        save->old_transformer = lookup_macro_value(c->vm, name);
        save->depth = fc->scope_depth;
        if (ch_vm_define_macro(c->vm, name, tr) != 0) {
            fc->n_body_macros--;
            return fail(c, "define-syntax: too many macros");
        }
        if (emit_void) {
            emit_byte(fc, CH_OP_LOAD_VOID);
            emit_byte(fc, dst);
        }
        return CH_COMPILE_OK;
    }

    /* Program top-level: permanent. */
    if (ch_vm_define_macro(c->vm, name, tr) != 0) {
        return fail(c, "define-syntax: too many macros");
    }
    if (emit_void) {
        emit_byte(fc, CH_OP_LOAD_VOID);
        emit_byte(fc, dst);
    }
    return CH_COMPILE_OK;
}

static int let_syntax_is_ellipsis(ChTransformer *tr, ChSymbol *s) {
    const char *base = ch_symbol_basename(s);
    if (tr->ellipsis_id) {
        return strcmp(ch_symbol_basename(tr->ellipsis_id), base) == 0;
    }
    return strcmp(base, "...") == 0;
}

static int let_syntax_is_literal(ChTransformer *tr, ChSymbol *s) {
    const char *base = ch_symbol_basename(s);
    for (size_t i = 0; i < tr->literal_count; i++) {
        if (strcmp(ch_symbol_basename(tr->literals[i]), base) == 0) {
            return 1;
        }
    }
    return 0;
}

static int let_syntax_name_listed(ChSymbol **names, int n, ChSymbol *s) {
    const char *base = ch_symbol_basename(s);
    for (int i = 0; i < n; i++) {
        if (strcmp(ch_symbol_basename(names[i]), base) == 0) {
            return 1;
        }
    }
    return 0;
}

static void let_syntax_collect_pvars(ChTransformer *tr, ChValue pat, ChSymbol **pvars, int *npvars) {
    if (ch_is_symbol(pat)) {
        ChSymbol *s = ch_as_symbol(pat);
        const char *base = ch_symbol_basename(s);
        if (strcmp(base, "_") == 0 || let_syntax_is_ellipsis(tr, s) || let_syntax_is_literal(tr, s)) {
            return;
        }
        if (!let_syntax_name_listed(pvars, *npvars, s) && *npvars < CH_BIND_MAX) {
            pvars[(*npvars)++] = s;
        }
        return;
    }
    if (ch_is_pair(pat)) {
        let_syntax_collect_pvars(tr, ch_car(pat), pvars, npvars);
        let_syntax_collect_pvars(tr, ch_cdr(pat), pvars, npvars);
        return;
    }
    if (ch_is_vector(pat)) {
        ChVector *vec = ch_as_vector(pat);
        for (size_t i = 0; i < vec->len; i++) {
            let_syntax_collect_pvars(tr, vec->items[i], pvars, npvars);
        }
    }
}

static void let_syntax_collect_free(ChTransformer *tr, ChValue tmpl, ChSymbol **pvars, int npvars,
                                    ChSymbol **frees, int *nfrees) {
    if (ch_is_symbol(tmpl)) {
        ChSymbol *s = ch_as_symbol(tmpl);
        const char *base = ch_symbol_basename(s);
        if (strcmp(base, "_") == 0 || let_syntax_is_ellipsis(tr, s) || let_syntax_is_literal(tr, s) ||
            let_syntax_name_listed(pvars, npvars, s) || is_well_known(base)) {
            return;
        }
        if (!let_syntax_name_listed(frees, *nfrees, s) && *nfrees < CH_TRANSFORMER_MAX_CAPTURES) {
            frees[(*nfrees)++] = s;
        }
        return;
    }
    if (ch_is_pair(tmpl)) {
        if (ch_is_symbol(ch_car(tmpl)) &&
            strcmp(ch_symbol_basename(ch_as_symbol(ch_car(tmpl))), "quote") == 0) {
            return;
        }
        let_syntax_collect_free(tr, ch_car(tmpl), pvars, npvars, frees, nfrees);
        let_syntax_collect_free(tr, ch_cdr(tmpl), pvars, npvars, frees, nfrees);
        return;
    }
    if (ch_is_vector(tmpl)) {
        ChVector *vec = ch_as_vector(tmpl);
        for (size_t i = 0; i < vec->len; i++) {
            let_syntax_collect_free(tr, vec->items[i], pvars, npvars, frees, nfrees);
        }
    }
}

static int let_syntax_local_bound(ChFuncCompiler *fc, ChSymbol *name) {
    const char *base = ch_symbol_basename(name);
    for (ChFuncCompiler *p = fc; p; p = p->enclosing) {
        for (int i = 0; i < p->local_count; i++) {
            if (strcmp(ch_symbol_basename(p->locals[i].name), base) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static void let_syntax_setup_captures(ChCompiler *c, ChFuncCompiler *fc, ChTransformer *tr) {
    ChSymbol *pvars[CH_BIND_MAX];
    int npvars = 0;
    for (size_t r = 0; r < tr->rule_count; r++) {
        let_syntax_collect_pvars(tr, tr->patterns[r], pvars, &npvars);
    }
    ChSymbol *frees[CH_TRANSFORMER_MAX_CAPTURES];
    int nfrees = 0;

    for (size_t r = 0; r < tr->rule_count; r++) {
        let_syntax_collect_free(tr, tr->templates[r], pvars, npvars, frees, &nfrees);
    }
    tr->capture_count = 0;
    for (int i = 0; i < nfrees; i++) {
        if (!let_syntax_local_bound(fc, frees[i])) {
            continue;
        }
        if (tr->capture_count >= CH_TRANSFORMER_MAX_CAPTURES) {
            break;
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "__cap_%u_%s", c->vm->hyg_counter++,
                 ch_symbol_basename(frees[i]));
        tr->capture_from[tr->capture_count] = frees[i];
        tr->capture_to[tr->capture_count] =
            ch_as_symbol(ch_gc_intern_symbol_cstr(&c->vm->gc, buf));

        tr->capture_count++;
    }

}

ChCompileStatus compile_let_syntax(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
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

    for (ChValue bl = bindings; ch_is_pair(bl); bl = ch_cdr(bl)) {
        ChValue bind = ch_car(bl);
        if (!ch_is_pair(bind) || !ch_is_symbol(ch_car(bind)) || !ch_is_pair(ch_cdr(bind))) {
            return fail(c, letrec ? "letrec-syntax: bad binding" : "let-syntax: bad binding");
        }
        if (nbinds >= CH_MAX_DERIVED_BINDINGS) {
            return fail(c, letrec ? "letrec-syntax: too many bindings" : "let-syntax: too many bindings");
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
        capture_transformer_templates(c->vm, tr);
        let_syntax_setup_captures(c, fc, tr);
        saves[nsaves].name = kw;
        saves[nsaves].old_transformer = lookup_macro_value(c->vm, kw);
        nsaves++;
        names[nbinds] = kw;
        transformers[nbinds++] = tr;
        if (letrec) {
            if (ch_vm_define_macro(c->vm, kw, tr) != 0) {
                for (int i = 0; i < nsaves; i++) {
                    restore_macro_binding(c->vm, &saves[i]);
                }
                return fail(c, "letrec-syntax: too many macros");
            }
        }
    }
    if (!ch_is_nil(bindings) && !ch_is_pair(bindings)) {
        return fail(c, letrec ? "letrec-syntax: bad binding list" : "let-syntax: bad binding list");
    }
    if (!letrec) {
        for (int i = 0; i < nbinds; i++) {
            if (ch_vm_define_macro(c->vm, names[i], transformers[i]) != 0) {
                for (int j = 0; j < nsaves; j++) {
                    restore_macro_binding(c->vm, &saves[j]);
                }
                return fail(c, "let-syntax: too many macros");
            }
        }
    }

    /* Union capture parameters and inject locals that snapshot definition-site
     * values so use-site shadows cannot steal them (#1644). */
    ChSymbol *cap_from[CH_TRANSFORMER_MAX_CAPTURES];
    ChSymbol *cap_to[CH_TRANSFORMER_MAX_CAPTURES];
    int ncaps = 0;
    for (int i = 0; i < nbinds; i++) {
        ChTransformer *tr = transformers[i];
        for (size_t j = 0; j < tr->capture_count; j++) {
            int idx = -1;
            for (int k = 0; k < ncaps; k++) {
                if (strcmp(ch_symbol_basename(cap_from[k]),
                           ch_symbol_basename(tr->capture_from[j])) == 0) {
                    idx = k;
                    break;
                }
            }
            if (idx < 0) {
                if (ncaps >= CH_TRANSFORMER_MAX_CAPTURES) {
                    break;
                }
                cap_from[ncaps] = tr->capture_from[j];
                cap_to[ncaps] = tr->capture_to[j];
                idx = ncaps++;
            } else {
                tr->capture_to[j] = cap_to[idx];
            }
        }
    }

    for (int i = 0; i < ncaps; i++) {
        int to_idx = resolve_local(fc, cap_to[i]);
        if (to_idx < 0) {
            if (add_local(c, fc, cap_to[i]) < 0) {
                for (int j = 0; j < nsaves; j++) {
                    restore_macro_binding(c->vm, &saves[j]);
                }
                return CH_COMPILE_ERROR;
            }
            ensure_temps_from(fc);
            to_idx = resolve_local(fc, cap_to[i]);
        }
        uint8_t to_reg = local_reg(fc, to_idx);
        int from_idx = resolve_local(fc, cap_from[i]);
        if (from_idx < 0) {
            const char *base = ch_symbol_basename(cap_from[i]);
            for (int li = fc->local_count - 1; li >= 0; li--) {
                if (li == to_idx) {
                    continue;
                }
                if (strcmp(ch_symbol_basename(fc->locals[li].name), base) == 0) {
                    from_idx = li;
                    break;
                }
            }
        }
        if (from_idx >= 0) {
            emit_byte(fc, CH_OP_MOVE);
            emit_byte(fc, to_reg);
            emit_byte(fc, local_reg(fc, from_idx));
        } else {
            int up = resolve_upvalue(fc, cap_from[i]);
            if (up < 0) {
                for (int j = 0; j < nsaves; j++) {
                    restore_macro_binding(c->vm, &saves[j]);
                }
                return fail(c, "let-syntax: capture of unbound local");
            }
            emit_byte(fc, CH_OP_GET_UPVALUE);
            emit_byte(fc, to_reg);
            emit_byte(fc, (uint8_t)up);
        }
    }

    ChCompileStatus st = compile_begin(c, fc, body, dst, tail);
    for (int i = 0; i < nsaves; i++) {
        restore_macro_binding(c->vm, &saves[i]);
    }
    return st;
}

ChCompileStatus compile_and(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
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

ChCompileStatus compile_or(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
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

ChCompileStatus compile_cond(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
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

static ChCompileStatus build_case_chain(ChCompiler *c, ChFuncCompiler *fc, ChValue key_sym,
                                        ChValue clauses, ChValue *out_form) {
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
        if (ch_is_pair(body) && is_symbol_named(ch_car(body), "=>") &&
            resolve_local(fc, ch_as_symbol(ch_car(body))) < 0 &&
            resolve_upvalue(fc, ch_as_symbol(ch_car(body))) < 0) {
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

    if (build_case_chain(c, fc, key_sym, rest, &alternate) != CH_COMPILE_OK) {
        ch_gc_pop_n(&c->vm->gc, 4);
        return CH_COMPILE_ERROR;
    }

    if (ch_is_pair(body) && is_symbol_named(ch_car(body), "=>") &&
        resolve_local(fc, ch_as_symbol(ch_car(body))) < 0 &&
        resolve_upvalue(fc, ch_as_symbol(ch_car(body))) < 0) {
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

ChCompileStatus compile_case(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
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

    if (build_case_chain(c, fc, key_sym, clauses, &chain) != CH_COMPILE_OK) {
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

ChCompileStatus compile_when_unless(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
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
ChCompileStatus compile_do(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
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

ChCompileStatus compile_guard(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
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

ChCompileStatus compile_parameterize(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
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
    /* Dotted unquote tail (#852): (unquote expr) as the final cdr is `. ,expr`. */
    if (level == 0 && ch_is_symbol(ch_car(xs_root)) &&
        is_symbol_named(ch_car(xs_root), "unquote")) {
        ChValue uargs = ch_cdr(xs_root);
        if (ch_is_pair(uargs) && ch_is_nil(ch_cdr(uargs))) {
            ChValue out = ch_car(uargs);
            ch_gc_pop(&c->vm->gc);
            return out;
        }
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

ChCompileStatus compile_quasiquote_real(ChCompiler *c, ChFuncCompiler *fc, ChValue args,
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

