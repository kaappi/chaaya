#include "chaaya/expander.h"

#include "expander_internal.h"

#include "chaaya/gc.h"
#include "chaaya/library.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static thread_local const ChUseSiteBindingCheck *active_use_check = NULL;

/* Pattern bindings live in a stack ChExpandCtx and are not otherwise scanned.
 * Keep each assigned value alive until the rule attempt finishes. */
static void protect_bind_value(ChExpandCtx *ctx, ChValue v) {
    if (ch_is_pointer(v)) {
        (void)ch_gc_add_extra_root(&ctx->vm->gc, v);
    }
}

static uint32_t global_binding_slot(ChVM *vm, ChSymbol *sym) {
    const char *base = ch_symbol_basename(sym);
    for (size_t i = 0; i < vm->global_count; i++) {
        if (vm->globals[i].defined &&
            strcmp(ch_symbol_basename(vm->globals[i].name), base) == 0) {
            return CH_LITERAL_GLOBAL_BASE | (uint32_t)i;
        }
    }
    if (vm->active_lib_env) {
        for (size_t i = 0; i < vm->active_lib_env->count; i++) {
            if (vm->active_lib_env->bindings[i].defined &&
                strcmp(ch_symbol_basename(vm->active_lib_env->bindings[i].name), base) == 0) {
                return CH_LITERAL_GLOBAL_BASE | (uint32_t)(vm->global_count + i);
            }
        }
    }
    return CH_LITERAL_UNBOUND;
}

static uint32_t resolve_expand_binding_slot(ChVM *vm, ChSymbol *sym) {
    return global_binding_slot(vm, sym);
}

int literal_index(ChTransformer *tr, ChSymbol *s) {
    const char *base = ch_symbol_basename(s);
    for (size_t i = 0; i < tr->literal_count; i++) {
        if (tr->literals[i] == s || strcmp(ch_symbol_basename(tr->literals[i]), base) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int literal_matches(ChExpandCtx *ctx, ChSymbol *pat_lit, ChValue use) {
    if (!ch_is_symbol(use)) {
        return 0;
    }
    ChSymbol *use_sym = ch_as_symbol(use);
    int idx = literal_index(ctx->tr, pat_lit);
    if (idx < 0) {
        return 0;
    }
    uint32_t def_slot = ctx->tr->literal_bound[(size_t)idx];
    uint32_t use_slot = CH_LITERAL_UNBOUND;
    if (ctx->use_check && ctx->use_check->resolve) {
        use_slot = ctx->use_check->resolve(ctx->use_check->ctx, ch_symbol_basename(use_sym));
    }
    if (use_slot == CH_LITERAL_UNBOUND) {
        use_slot = resolve_expand_binding_slot(ctx->vm, use_sym);
    }
    if (def_slot == CH_LITERAL_UNBOUND && use_slot == CH_LITERAL_UNBOUND) {
        return strcmp(ch_symbol_basename(pat_lit), ch_symbol_basename(use_sym)) == 0;
    }
    if (def_slot == CH_LITERAL_UNBOUND || use_slot == CH_LITERAL_UNBOUND) {
        return 0;
    }
    return def_slot == use_slot;
}

static int is_ellipsis_id(ChExpandCtx *ctx, ChSymbol *s) {
    const char *base = ch_symbol_basename(s);
    if (ctx->tr->ellipsis_id) {
        return strcmp(ch_symbol_basename(ctx->tr->ellipsis_id), base) == 0;
    }
    return strcmp(base, "...") == 0;
}

static int is_ellipsis_sym(ChSymbol *s) {
    return strcmp(ch_symbol_basename(s), "...") == 0;
}

static int is_literal(ChExpandCtx *ctx, ChSymbol *s);

static int is_underscore_pat(ChExpandCtx *ctx, ChSymbol *s) {
    return strcmp(ch_symbol_basename(s), "_") == 0 && !is_literal(ctx, s);
}

static int take_pattern_escape(ChValue v, ChValue *inner) {
    if (!ch_is_pair(v) || !ch_is_symbol(ch_car(v))) {
        return 0;
    }
    if (!is_ellipsis_sym(ch_as_symbol(ch_car(v)))) {
        return 0;
    }
    ChValue rest = ch_cdr(v);
    if (ch_is_pair(rest) && ch_is_nil(ch_cdr(rest))) {
        *inner = ch_car(rest);
        return 1;
    }
    return 0;
}

static int expand_err(ChExpandCtx *ctx, const char *msg) {
    if (ctx->err && ctx->err_len) {
        snprintf(ctx->err, ctx->err_len, "%s", msg);
    }
    return 0;
}

static int is_literal(ChExpandCtx *ctx, ChSymbol *s) {
    const char *base = ch_symbol_basename(s);
    for (size_t i = 0; i < ctx->tr->literal_count; i++) {
        if (strcmp(ch_symbol_basename(ctx->tr->literals[i]), base) == 0) {
            return 1;
        }
    }
    return 0;
}

static ChBinding *find_bind(ChExpandCtx *ctx, ChSymbol *var) {
    const char *base = ch_symbol_basename(var);
    for (int i = 0; i < ctx->nbinds; i++) {
        if (strcmp(ch_symbol_basename(ctx->binds[i].var), base) == 0) {
            return &ctx->binds[i];
        }
    }
    return NULL;
}

static int list_length(ChValue v) {
    int n = 0;
    for (; ch_is_pair(v); v = ch_cdr(v)) {
        n++;
    }
    return ch_is_nil(v) ? n : -1;
}

static ChValue list_reverse(ChGC *gc, ChValue lst) {
    ChValue lst_root = lst;
    ChValue rev = CH_NIL;
    ch_gc_push(gc, &lst_root);
    ch_gc_push(gc, &rev);
    while (ch_is_pair(lst_root)) {
        ChValue item = ch_car(lst_root);
        ch_gc_push(gc, &item);
        rev = ch_gc_cons(gc, item, rev);
        ch_gc_pop(gc);
        lst_root = ch_cdr(lst_root);
    }
    ch_gc_pop_n(gc, 2);
    return rev;
}

static int bind_var(ChExpandCtx *ctx, ChSymbol *var, ChValue val, int under_ellipsis) {
    const char *base = ch_symbol_basename(var);
    if (strcmp(base, "_") == 0) {
        return 1;
    }
    ChBinding *b = find_bind(ctx, var);
    if (under_ellipsis) {
        if (b) {
            if (!b->ellipsis) {
                return expand_err(ctx, "syntax-rules: pattern variable used at mixed depths");
            }
            ChValue item = val;
            ch_gc_push(&ctx->vm->gc, &b->value);
            ch_gc_push(&ctx->vm->gc, &item);
            b->value = ch_gc_cons(&ctx->vm->gc, item, b->value);
            protect_bind_value(ctx, b->value);
            ch_gc_pop_n(&ctx->vm->gc, 2);
            return 1;
        }
        if (ctx->nbinds >= CH_BIND_MAX) {
            return expand_err(ctx, "syntax-rules: too many pattern variables");
        }
        ch_gc_push(&ctx->vm->gc, &val);
        ctx->binds[ctx->nbinds].var = var;
        ctx->binds[ctx->nbinds].value = ch_gc_cons(&ctx->vm->gc, val, CH_NIL);
        ctx->binds[ctx->nbinds].ellipsis = 1;
        protect_bind_value(ctx, ctx->binds[ctx->nbinds].value);
        ctx->nbinds++;
        ch_gc_pop(&ctx->vm->gc);
        return 1;
    }
    if (b) {
        return expand_err(ctx, "syntax-rules: duplicate pattern variable");
    }
    if (ctx->nbinds >= CH_BIND_MAX) {
        return expand_err(ctx, "syntax-rules: too many pattern variables");
    }
    ctx->binds[ctx->nbinds].var = var;
    ctx->binds[ctx->nbinds].value = val;
    ctx->binds[ctx->nbinds].ellipsis = 0;
    protect_bind_value(ctx, val);
    ctx->nbinds++;
    return 1;
}

static int match_pattern(ChExpandCtx *ctx, ChValue pat, ChValue use, int under_ellipsis);
static int match_list(ChExpandCtx *ctx, ChValue pat, ChValue use, int under_ellipsis);

/* Collect pattern-variable names from an ellipsis element pattern. */
static void collect_ellipsis_vars(ChExpandCtx *ctx, ChValue pat, ChSymbol **names, int *nnames) {
    if (ch_is_symbol(pat)) {
        ChSymbol *s = ch_as_symbol(pat);
        const char *base = ch_symbol_basename(s);
        if (strcmp(base, "_") == 0 || is_ellipsis_id(ctx, s) || is_literal(ctx, s)) {
            return;
        }
        for (int i = 0; i < *nnames; i++) {
            if (strcmp(ch_symbol_basename(names[i]), base) == 0) {
                return;
            }
        }
        if (*nnames < CH_BIND_MAX) {
            names[(*nnames)++] = s;
        }
        return;
    }
    if (ch_is_pair(pat)) {
        ChValue esc;
        if (take_pattern_escape(pat, &esc)) {
            collect_ellipsis_vars(ctx, esc, names, nnames);
            return;
        }
        collect_ellipsis_vars(ctx, ch_car(pat), names, nnames);
        collect_ellipsis_vars(ctx, ch_cdr(pat), names, nnames);
        return;
    }
    if (ch_is_vector(pat)) {
        ChVector *vec = ch_as_vector(pat);
        for (size_t i = 0; i < vec->len; i++) {
            collect_ellipsis_vars(ctx, vec->items[i], names, nnames);
        }
    }
}

/* Match `patt ...` by matching each element into a fresh binding set, then
 * appending (wrapping nested ellipsis lists) onto outer ellipsis bindings. */
static int match_ellipsis(ChExpandCtx *ctx, ChValue pcar, ChValue after, ChValue use) {
    int need = list_length(after);
    int ulen = list_length(use);
    if (need < 0 || ulen < 0 || ulen < need) {
        return 0;
    }
    int nrep = ulen - need;

    ChSymbol *var_names[CH_BIND_MAX];
    int nvars = 0;
    collect_ellipsis_vars(ctx, pcar, var_names, &nvars);

    int base = ctx->nbinds;
    for (int vi = 0; vi < nvars; vi++) {
        if (ctx->nbinds >= CH_BIND_MAX) {
            return expand_err(ctx, "syntax-rules: too many pattern variables");
        }
        /* Skip if already bound (shouldn't happen for well-formed patterns). */
        if (find_bind(ctx, var_names[vi])) {
            return expand_err(ctx, "syntax-rules: duplicate pattern variable");
        }
        ctx->binds[ctx->nbinds].var = var_names[vi];
        ctx->binds[ctx->nbinds].value = CH_NIL;
        ctx->binds[ctx->nbinds].ellipsis = 1;
        ctx->nbinds++;
    }

    ChValue u = use;
    for (int i = 0; i < nrep; i++) {
        if (!ch_is_pair(u)) {
            return 0;
        }
        /* Fresh sub-context bindings for this element. */
        ChExpandCtx sub = *ctx;
        sub.nbinds = 0;
        memset(sub.binds, 0, sizeof(sub.binds));
        if (!match_pattern(&sub, pcar, ch_car(u), 0)) {
            if (sub.err && sub.err[0]) {
                snprintf(ctx->err, ctx->err_len, "%s", sub.err);
            }
            return 0;
        }
        for (int si = 0; si < sub.nbinds; si++) {
            for (int bi = base; bi < ctx->nbinds; bi++) {
                if (strcmp(ch_symbol_basename(ctx->binds[bi].var),
                           ch_symbol_basename(sub.binds[si].var)) != 0) {
                    continue;
                }
                ChValue piece = sub.binds[si].value;
                if (sub.binds[si].ellipsis) {
                    /* Nested ellipsis: sub match_ellipsis already left piece in
                     * document order — wrap as one outer element. */
                    if (ctx->binds[bi].ellipsis < sub.binds[si].ellipsis + 1) {
                        ctx->binds[bi].ellipsis = sub.binds[si].ellipsis + 1;
                    }
                }
                ch_gc_push(&ctx->vm->gc, &ctx->binds[bi].value);
                ch_gc_push(&ctx->vm->gc, &piece);
                ctx->binds[bi].value = ch_gc_cons(&ctx->vm->gc, piece, ctx->binds[bi].value);
                protect_bind_value(ctx, ctx->binds[bi].value);
                ch_gc_pop_n(&ctx->vm->gc, 2);
                break;
            }
        }
        /* Copy hyg renames from sub if any were added (none for match). */
        u = ch_cdr(u);
    }

    /* Reverse outer ellipsis lists to document order. */
    for (int bi = base; bi < ctx->nbinds; bi++) {
        if (ctx->binds[bi].ellipsis) {
            ctx->binds[bi].value = list_reverse(&ctx->vm->gc, ctx->binds[bi].value);
            protect_bind_value(ctx, ctx->binds[bi].value);
        }
    }
    return match_list(ctx, after, u, 0);
}

static int match_list(ChExpandCtx *ctx, ChValue pat, ChValue use, int under_ellipsis) {
    ChValue whole_esc;
    if (take_pattern_escape(pat, &whole_esc)) {
        int ulen = list_length(use);
        if (ulen < 0) {
            return 0;
        }
        int nrep = ulen;
        int start_binds = ctx->nbinds;
        ChValue u = use;
        for (int i = 0; i < nrep; i++) {
            if (!match_pattern(ctx, whole_esc, ch_car(u), 1)) {
                return 0;
            }
            u = ch_cdr(u);
        }
        for (int bi = start_binds; bi < ctx->nbinds; bi++) {
            if (ctx->binds[bi].ellipsis) {
                ctx->binds[bi].value = list_reverse(&ctx->vm->gc, ctx->binds[bi].value);
                protect_bind_value(ctx, ctx->binds[bi].value);
            }
        }
        return ch_is_nil(u);
    }
    while (ch_is_pair(pat)) {
        ChValue pcar = ch_car(pat);
        ChValue pcdr = ch_cdr(pat);
        /* Ellipsis must win over underscore greediness so `(_ ... y)` works. */
        if (ch_is_pair(pcdr) && ch_is_symbol(ch_car(pcdr)) &&
            is_ellipsis_id(ctx, ch_as_symbol(ch_car(pcdr)))) {
            return match_ellipsis(ctx, pcar, ch_cdr(pcdr), use);
        }
        if (ch_is_nil(use) && ch_is_nil(pcdr) && ch_is_symbol(pcar) &&
            is_underscore_pat(ctx, ch_as_symbol(pcar))) {
            return 1;
        }
        if (ch_is_symbol(pcar) && is_underscore_pat(ctx, ch_as_symbol(pcar)) &&
            !ch_is_nil(pcdr)) {
            int saved_binds = ctx->nbinds;
            if (match_list(ctx, pcdr, use, under_ellipsis)) {
                return 1;
            }
            ctx->nbinds = saved_binds;
            if (ch_is_pair(use) && match_list(ctx, pcdr, ch_cdr(use), under_ellipsis)) {
                return 1;
            }
            ctx->nbinds = saved_binds;
            return 0;
        }
        ChValue esc_inner;
        if (take_pattern_escape(pcar, &esc_inner)) {
            int need = list_length(pcdr);
            int ulen = list_length(use);
            if (need < 0 || ulen < 0 || ulen < need) {
                return 0;
            }
            int nrep = ulen - need;
            int start_binds = ctx->nbinds;
            ChValue u = use;
            for (int i = 0; i < nrep; i++) {
                if (!match_pattern(ctx, esc_inner, ch_car(u), 1)) {
                    return 0;
                }
                u = ch_cdr(u);
            }
            for (int bi = start_binds; bi < ctx->nbinds; bi++) {
                if (ctx->binds[bi].ellipsis) {
                    ctx->binds[bi].value = list_reverse(&ctx->vm->gc, ctx->binds[bi].value);
                    protect_bind_value(ctx, ctx->binds[bi].value);
                }
            }
            return match_list(ctx, pcdr, u, under_ellipsis);
        }
        if (!ch_is_pair(use)) {
            return 0;
        }
        if (!match_pattern(ctx, pcar, ch_car(use), under_ellipsis)) {
            return 0;
        }
        pat = pcdr;
        use = ch_cdr(use);
    }
    if (ch_is_symbol(pat)) {
        ChSymbol *s = ch_as_symbol(pat);
        if (is_literal(ctx, s)) {
            return 0;
        }
        return bind_var(ctx, s, use, under_ellipsis);
    }
    return ch_is_nil(pat) && ch_is_nil(use);
}

static ChValue vector_to_list(ChGC *gc, ChVector *vec) {
    ChValue list = CH_NIL;
    ch_gc_push(gc, &list);
    for (size_t i = vec->len; i > 0; i--) {
        ChValue item = vec->items[i - 1];
        ch_gc_push(gc, &item);
        list = ch_gc_cons(gc, item, list);
        ch_gc_pop(gc);
    }
    ch_gc_pop(gc);
    return list;
}

static int match_pattern(ChExpandCtx *ctx, ChValue pat, ChValue use, int under_ellipsis) {
    if (ch_is_symbol(pat)) {
        ChSymbol *s = ch_as_symbol(pat);
        if (is_literal(ctx, s)) {
            return literal_matches(ctx, s, use);
        }
        return bind_var(ctx, s, use, under_ellipsis);
    }
    if (ch_is_nil(pat)) {
        return ch_is_nil(use);
    }
    if (ch_is_pair(pat)) {
        return match_list(ctx, pat, use, under_ellipsis);
    }
    if (ch_is_vector(pat)) {
        if (!ch_is_vector(use)) {
            return 0;
        }
        ChValue plist = vector_to_list(&ctx->vm->gc, ch_as_vector(pat));
        ch_gc_push(&ctx->vm->gc, &plist);
        ChValue ulist = vector_to_list(&ctx->vm->gc, ch_as_vector(use));
        ch_gc_push(&ctx->vm->gc, &ulist);
        int ok = match_list(ctx, plist, ulist, under_ellipsis);
        ch_gc_pop_n(&ctx->vm->gc, 2);
        return ok;
    }
    return ch_equal(pat, use);
}

static void reverse_all_ellipsis(ChExpandCtx *ctx) {
    /* After matching, ellipsis bindings are newest-first from nested appends.
     * match_list already reverses per-ellipsis-segment for newly created binds.
     * For vars that existed and got more conses, reverse once here only if still
     * front-built — we reverse in match_list for the segment; for re-used vars
     * within one ellipsis loop, bind_var conses so the loop's matches are reversed
     * relative to input order. Fix: reverse in the ellipsis loop after all nrep. */
    (void)ctx;
}

static ChSymbol *hyg_rename(ChExpandCtx *ctx, ChSymbol *from) {
    const char *base = ch_symbol_basename(from);
    for (int i = 0; i < ctx->nrenames; i++) {
        if (strcmp(ch_symbol_basename(ctx->renames[i].from), base) == 0) {
            return ctx->renames[i].to;
        }
    }
    if (ctx->nrenames >= CH_HYG_MAX) {
        return from;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "__hyg_%u_%s", ctx->vm->hyg_counter++, base);
    ChValue sym = ch_gc_intern_symbol_cstr(&ctx->vm->gc, buf);
    ctx->renames[ctx->nrenames].from = from;
    ctx->renames[ctx->nrenames].to = ch_as_symbol(sym);
    ctx->nrenames++;
    return ctx->renames[ctx->nrenames - 1].to;
}

static int lib_env_binding(ChVM *vm, ChSymbol *sym, ChValue *out) {
    if (!vm->active_lib_env) {
        return 0;
    }
    for (size_t i = 0; i < vm->active_lib_env->count; i++) {
        if (vm->active_lib_env->bindings[i].defined &&
            vm->active_lib_env->bindings[i].name == sym) {
            *out = vm->active_lib_env->bindings[i].value;
            return 1;
        }
    }
    const char *base = ch_symbol_basename(sym);
    for (size_t i = 0; i < vm->active_lib_env->count; i++) {
        if (vm->active_lib_env->bindings[i].defined &&
            strcmp(ch_symbol_basename(vm->active_lib_env->bindings[i].name), base) == 0) {
            *out = vm->active_lib_env->bindings[i].value;
            return 1;
        }
    }
    return 0;
}

static ChValue bind_lib_ref(ChExpandCtx *ctx, ChSymbol *sym) {
    ChValue val = CH_UNDEFINED;
    if (!lib_env_binding(ctx->vm, sym, &val)) {
        return ch_make_pointer(&sym->header);
    }
    char buf[256];
    unsigned env_tag = (unsigned)(uintptr_t)ctx->vm->active_lib_env;
    snprintf(buf, sizeof(buf), "__hyg_lib_%u_%s", env_tag, ch_symbol_basename(sym));
    ChSymbol *ren = ch_as_symbol(ch_gc_intern_symbol_cstr(&ctx->vm->gc, buf));
    int g = ch_vm_intern_global(ctx->vm, ren);
    ch_vm_define_global(ctx->vm, g, val);
    return ch_make_pointer(&ren->header);
}

static ChValue instantiate(ChExpandCtx *ctx, ChValue tmpl);

static int ellipsis_count_in_tmpl(ChExpandCtx *ctx, ChValue tmpl) {
    if (ch_is_symbol(tmpl)) {
        ChBinding *b = find_bind(ctx, ch_as_symbol(tmpl));
        if (b && b->ellipsis) {
            return list_length(b->value);
        }
        return -1;
    }
    if (ch_is_pair(tmpl)) {
        int best = -1;
        for (ChValue p = tmpl; ch_is_pair(p); p = ch_cdr(p)) {
            int n = ellipsis_count_in_tmpl(ctx, ch_car(p));
            if (n > best) {
                best = n;
            }
        }
        if (!ch_is_nil(tmpl) && !ch_is_pair(tmpl)) {
            int n = ellipsis_count_in_tmpl(ctx, tmpl);
            if (n > best) {
                best = n;
            }
        }
        return best;
    }
    return -1;
}

static ChValue nth_of(ChValue lst, int n) {
    for (int i = 0; i < n && ch_is_pair(lst); i++) {
        lst = ch_cdr(lst);
    }
    return ch_is_pair(lst) ? ch_car(lst) : CH_NIL;
}

static ChValue deep_copy_instantiate(ChExpandCtx *ctx, ChValue v) {
    ChValue v_root = v;
    ch_gc_push(&ctx->vm->gc, &v_root);
    if (ch_is_pair(v_root)) {
        ChValue car = deep_copy_instantiate(ctx, ch_car(v_root));
        ch_gc_push(&ctx->vm->gc, &car);
        ChValue cdr = deep_copy_instantiate(ctx, ch_cdr(v_root));
        ch_gc_push(&ctx->vm->gc, &cdr);
        ChValue out = ch_gc_cons(&ctx->vm->gc, car, cdr);
        ch_gc_pop_n(&ctx->vm->gc, 3);
        return out;
    }
    if (ch_is_vector(v_root)) {
        ChVector *src = ch_as_vector(v_root);
        ChValue out = ch_gc_make_vector(&ctx->vm->gc, src->len, CH_UNDEFINED);
        ChVector *dst = ch_as_vector(out);
        for (size_t i = 0; i < src->len; i++) {
            dst->items[i] = deep_copy_instantiate(ctx, src->items[i]);
        }
        ch_gc_pop(&ctx->vm->gc);
        return out;
    }
    ch_gc_pop(&ctx->vm->gc);
    return v_root;
}

static ChValue instantiate_with_index(ChExpandCtx *ctx, ChValue tmpl, int index) {
    /* Peel one ellipsis level so nested `(b ...)` sees the index-th group. */
    ChBinding saved[CH_BIND_MAX];
    int nsaved = ctx->nbinds;
    if (nsaved > CH_BIND_MAX) {
        nsaved = CH_BIND_MAX;
    }
    memcpy(saved, ctx->binds, (size_t)nsaved * sizeof(ChBinding));
    /* Keep saved binding values alive across instantiate (binds[] is not a GC root). */
    for (int i = 0; i < nsaved; i++) {
        protect_bind_value(ctx, saved[i].value);
    }
    for (int i = 0; i < ctx->nbinds; i++) {
        if (!ctx->binds[i].ellipsis) {
            continue;
        }
        ChValue group = nth_of(ctx->binds[i].value, index);
        if (ctx->binds[i].ellipsis > 1) {
            ctx->binds[i].value = group;
            ctx->binds[i].ellipsis -= 1;
        } else {
            ctx->binds[i].value = group;
            ctx->binds[i].ellipsis = 0;
        }
        protect_bind_value(ctx, ctx->binds[i].value);
    }
    ChValue out = instantiate(ctx, tmpl);
    memcpy(ctx->binds, saved, (size_t)nsaved * sizeof(ChBinding));
    ctx->nbinds = nsaved;
    return out;
}

static ChValue splice_values(ChGC *gc, ChValue vals, ChValue rest) {
    if (!ch_is_pair(vals)) {
        ch_gc_push(gc, &vals);
        ch_gc_push(gc, &rest);
        ChValue out = ch_gc_cons(gc, vals, rest);
        ch_gc_pop_n(gc, 2);
        return out;
    }
    ChValue result = rest;
    ChValue items[CH_ELLIPSIS_MAX];
    int n = 0;
    for (ChValue p = vals; ch_is_pair(p) && n < CH_ELLIPSIS_MAX; p = ch_cdr(p)) {
        items[n++] = ch_car(p);
    }
    for (int i = n - 1; i >= 0; i--) {
        ChValue item = items[i];
        ch_gc_push(gc, &item);
        result = ch_gc_cons(gc, item, result);
        ch_gc_pop(gc);
    }
    return result;
}

static ChValue instantiate_list(ChExpandCtx *ctx, ChValue tmpl) {
    if (ch_is_nil(tmpl)) {
        return CH_NIL;
    }
    if (!ch_is_pair(tmpl)) {
        return instantiate(ctx, tmpl);
    }
    ChValue whole_esc;
    if (!ctx->escape && take_pattern_escape(tmpl, &whole_esc)) {
        ctx->escape = 1;
        ChValue inner;
        if (ch_is_symbol(whole_esc)) {
            ChBinding *b = find_bind(ctx, ch_as_symbol(whole_esc));
            if (b && b->ellipsis) {
                inner = b->value;
            } else if (is_ellipsis_id(ctx, ch_as_symbol(whole_esc))) {
                inner = whole_esc;
            } else {
                inner = instantiate(ctx, whole_esc);
            }
        } else {
            inner = instantiate(ctx, whole_esc);
        }
        ctx->escape = 0;
        if (ch_is_symbol(whole_esc)) {
            ChBinding *b = find_bind(ctx, ch_as_symbol(whole_esc));
            if (b && b->ellipsis) {
                return inner;
            }
        }
        return inner;
    }
    if (ch_is_symbol(ch_car(tmpl)) &&
        strcmp(ch_symbol_basename(ch_as_symbol(ch_car(tmpl))), "quote") == 0) {
        ChValue qrest = ch_cdr(tmpl);
        if (ch_is_pair(qrest) && ch_is_nil(ch_cdr(qrest))) {
            int saved_quote = ctx->in_quote;
            ctx->in_quote = 1;
            ChValue inner = instantiate(ctx, ch_car(qrest));
            ctx->in_quote = saved_quote;
            ch_gc_push(&ctx->vm->gc, &inner);
            ChValue tail = ch_gc_cons(&ctx->vm->gc, inner, CH_NIL);
            ch_gc_push(&ctx->vm->gc, &tail);
            ChValue out = ch_gc_cons(&ctx->vm->gc, ch_car(tmpl), tail);
            ch_gc_pop_n(&ctx->vm->gc, 2);
            return out;
        }
    }
    ChValue tcar = ch_car(tmpl);
    ChValue tcdr = ch_cdr(tmpl);
    ChValue esc_inner;
    if (!ctx->escape && take_pattern_escape(tcar, &esc_inner)) {
        ctx->escape = 1;
        ChValue inner;
        if (ch_is_symbol(esc_inner)) {
            ChBinding *b = find_bind(ctx, ch_as_symbol(esc_inner));
            if (b && b->ellipsis) {
                inner = b->value;
            } else if (is_ellipsis_id(ctx, ch_as_symbol(esc_inner))) {
                inner = esc_inner;
            } else {
                inner = instantiate(ctx, esc_inner);
            }
        } else {
            int saved = ctx->escape;
            ctx->escape = 1;
            inner = instantiate(ctx, esc_inner);
            ctx->escape = saved;
        }
        ctx->escape = 0;
        ChValue tail = instantiate_list(ctx, tcdr);
        if (ch_is_symbol(esc_inner)) {
            ChBinding *b = find_bind(ctx, ch_as_symbol(esc_inner));
            if (b && b->ellipsis) {
                return splice_values(&ctx->vm->gc, inner, tail);
            }
        }
        ch_gc_push(&ctx->vm->gc, &inner);
        ch_gc_push(&ctx->vm->gc, &tail);
        ChValue out = ch_gc_cons(&ctx->vm->gc, inner, tail);
        ch_gc_pop_n(&ctx->vm->gc, 2);
        return out;
    }
    if (ch_is_pair(tcdr) && ch_is_symbol(ch_car(tcdr)) &&
        is_ellipsis_id(ctx, ch_as_symbol(ch_car(tcdr))) && !ctx->escape) {
        ChValue after = ch_cdr(tcdr);
        /* Consecutive ellipses `(x ... ...)` flatten one nesting level. */
        int extra_ellipsis = 0;
        while (ch_is_pair(after) && ch_is_symbol(ch_car(after)) &&
               is_ellipsis_id(ctx, ch_as_symbol(ch_car(after)))) {
            extra_ellipsis++;
            after = ch_cdr(after);
        }
        int nrep = ellipsis_count_in_tmpl(ctx, tcar);
        if (nrep < 0) {
            nrep = 0;
        }
        ChValue pieces[CH_ELLIPSIS_MAX];
        int np = 0;
        size_t pieces_root = ctx->vm->gc.root_count;
        for (int i = 0; i < nrep && np < CH_ELLIPSIS_MAX; i++) {
            pieces[np] = instantiate_with_index(ctx, tcar, i);
            ch_gc_push(&ctx->vm->gc, &pieces[np]);
            np++;
        }
        ChValue rest = instantiate_list(ctx, after);
        ch_gc_push(&ctx->vm->gc, &rest);
        ChValue result = rest;
        for (int i = np - 1; i >= 0; i--) {
            ChValue item = pieces[i];
            ch_gc_push(&ctx->vm->gc, &item);
            if (extra_ellipsis > 0 && ch_is_pair(item)) {
                result = splice_values(&ctx->vm->gc, item, result);
            } else if (extra_ellipsis > 0 && ch_is_nil(item)) {
                /* empty group contributes nothing */
            } else {
                result = ch_gc_cons(&ctx->vm->gc, item, result);
            }
            ch_gc_pop(&ctx->vm->gc);
        }
        ch_gc_pop_to(&ctx->vm->gc, pieces_root);
        return result;
    }

    ChValue car_v = instantiate(ctx, tcar);
    ch_gc_push(&ctx->vm->gc, &car_v);
    ChValue cdr_v = instantiate_list(ctx, tcdr);
    ch_gc_push(&ctx->vm->gc, &cdr_v);
    ChValue out = ch_gc_cons(&ctx->vm->gc, car_v, cdr_v);
    ch_gc_pop_n(&ctx->vm->gc, 2);
    return out;
}

static ChValue instantiate(ChExpandCtx *ctx, ChValue tmpl) {
    if (ch_is_symbol(tmpl)) {
        ChSymbol *s = ch_as_symbol(tmpl);
        ChBinding *b = find_bind(ctx, s);
        if (b) {
            return deep_copy_instantiate(ctx, b->value);
        }
        if (ctx->in_quote) {
            return tmpl;
        }
        if (is_literal(ctx, s) || is_well_known(ch_symbol_basename(s))) {
            return tmpl;
        }
        {
            ChValue lib_val = CH_UNDEFINED;
            if (lib_env_binding(ctx->vm, s, &lib_val)) {
                if (!ch_is_transformer(lib_val)) {
                    return bind_lib_ref(ctx, s);
                }
                return tmpl;
            }
        }
        if (!ctx->escape && is_ellipsis_id(ctx, s)) {
            return tmpl;
        }
        if (ctx->escape && is_ellipsis_id(ctx, s)) {
            return tmpl;
        }
        /* Free identifiers that already resolve to a top-level binding keep
         * their name (so templates can refer to user helpers like `pass`).
         * Unbound free identifiers still get hygienic renames. */
        if (global_binding_slot(ctx->vm, s) != CH_LITERAL_UNBOUND) {
            return tmpl;
        }
        ChSymbol *ren = hyg_rename(ctx, s);
        return ch_make_pointer(&ren->header);
    }
    if (ch_is_pair(tmpl)) {
        if (ch_is_symbol(ch_car(tmpl)) &&
            strcmp(ch_symbol_basename(ch_as_symbol(ch_car(tmpl))), "syntax-rules") == 0) {
            ChValue car_v = ch_car(tmpl);
            ch_gc_push(&ctx->vm->gc, &car_v);
            ChValue cdr_v = instantiate_list(ctx, ch_cdr(tmpl));
            ch_gc_push(&ctx->vm->gc, &cdr_v);
            ChValue out = ch_gc_cons(&ctx->vm->gc, car_v, cdr_v);
            ch_gc_pop_n(&ctx->vm->gc, 2);
            return out;
        }
        return instantiate_list(ctx, tmpl);
    }
    return tmpl;
}

ChExpandStatus ch_parse_syntax_rules(ChVM *vm, ChValue spec, ChTransformer **out, char *err,
                                     size_t err_len) {
    if (!ch_is_pair(spec) || !ch_is_symbol(ch_car(spec)) ||
        strcmp(ch_symbol_basename(ch_as_symbol(ch_car(spec))), "syntax-rules") != 0) {
        snprintf(err, err_len, "define-syntax: expected syntax-rules");
        return CH_EXPAND_ERROR;
    }
    ChValue rest = ch_cdr(spec);
    if (!ch_is_pair(rest)) {
        snprintf(err, err_len, "syntax-rules: bad syntax");
        return CH_EXPAND_ERROR;
    }
    ChValue after_ellipsis = rest;
    ChSymbol *custom_ellipsis = NULL;
    ChValue first = ch_car(rest);
    if (ch_is_symbol(first) && strcmp(ch_symbol_basename(ch_as_symbol(first)), "_") != 0) {
        custom_ellipsis = ch_as_symbol(first);
        after_ellipsis = ch_cdr(rest);
        if (!ch_is_pair(after_ellipsis)) {
            snprintf(err, err_len, "syntax-rules: bad syntax");
            return CH_EXPAND_ERROR;
        }
    }
    ChValue lits_v = ch_car(after_ellipsis);
    ChValue rules = ch_cdr(after_ellipsis);

    ChValue tr_v = ch_gc_make_transformer(&vm->gc);
    ch_gc_push(&vm->gc, &tr_v);
    ChTransformer *tr = ch_as_transformer(tr_v);
    tr->literal_count = 0;
    tr->ellipsis_id = custom_ellipsis;
    tr->rule_count = 0;

    for (ChValue L = lits_v; ch_is_pair(L); L = ch_cdr(L)) {
        if (!ch_is_symbol(ch_car(L))) {
            ch_gc_pop(&vm->gc);
            snprintf(err, err_len, "syntax-rules: literal must be a symbol");
            return CH_EXPAND_ERROR;
        }
        if (tr->literal_count >= CH_TRANSFORMER_MAX_LITERALS) {
            ch_gc_pop(&vm->gc);
            snprintf(err, err_len, "syntax-rules: too many literals");
            return CH_EXPAND_ERROR;
        }
        tr->literals[tr->literal_count] = ch_as_symbol(ch_car(L));
        tr->literal_bound[tr->literal_count] =
            resolve_expand_binding_slot(vm, tr->literals[tr->literal_count]);
        tr->literal_count++;
    }
    if (!ch_is_nil(lits_v) && !ch_is_pair(lits_v)) {
        ch_gc_pop(&vm->gc);
        snprintf(err, err_len, "syntax-rules: literals must be a list");
        return CH_EXPAND_ERROR;
    }

    for (ChValue R = rules; ch_is_pair(R); R = ch_cdr(R)) {
        ChValue rule = ch_car(R);
        if (!ch_is_pair(rule) || !ch_is_pair(ch_cdr(rule))) {
            ch_gc_pop(&vm->gc);
            snprintf(err, err_len, "syntax-rules: bad rule");
            return CH_EXPAND_ERROR;
        }
        if (tr->rule_count >= CH_TRANSFORMER_MAX_RULES) {
            ch_gc_pop(&vm->gc);
            snprintf(err, err_len, "syntax-rules: too many rules");
            return CH_EXPAND_ERROR;
        }
        tr->patterns[tr->rule_count] = ch_car(rule);
        tr->templates[tr->rule_count] = ch_car(ch_cdr(rule));
        tr->rule_count++;
    }

    ch_gc_pop(&vm->gc);
    *out = tr;
    return CH_EXPAND_OK;
}

ChExpandStatus ch_expand_macro_checked(ChVM *vm, ChTransformer *tr, ChValue use,
                                       const ChUseSiteBindingCheck *use_check, ChValue *out,
                                       char *err, size_t err_len) {
    const ChUseSiteBindingCheck *saved = active_use_check;
    active_use_check = use_check;
    ChExpandStatus st = ch_expand_macro(vm, tr, use, out, err, err_len);
    active_use_check = saved;
    return st;
}

ChExpandStatus ch_expand_macro(ChVM *vm, ChTransformer *tr, ChValue use, ChValue *out, char *err,
                               size_t err_len) {
    ChLibEnv *saved_env = vm->active_lib_env;
    ChLibEnv *home_env = NULL;
    ChValue use_root = use;
    ch_gc_push(&vm->gc, &use_root);
    ChSymbol *use_name = NULL;
    if (ch_is_pair(use_root) && ch_is_symbol(ch_car(use_root))) {
        use_name = ch_as_symbol(ch_car(use_root));
    }
    for (size_t i = 0; i < vm->macro_count; i++) {
        if (use_name &&
            strcmp(ch_symbol_basename(vm->macros[i].name), ch_symbol_basename(use_name)) == 0) {
            home_env = vm->macros[i].home_env;
            break;
        }
        if (ch_as_transformer(vm->macros[i].transformer) == tr) {
            home_env = vm->macros[i].home_env;
            break;
        }
    }
    if (home_env) {
        vm->active_lib_env = home_env;
    }
    for (size_t i = 0; i < tr->rule_count; i++) {
        ChExpandCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.vm = vm;
        ctx.tr = tr;
        ctx.use_check = active_use_check;
        ctx.err = err;
        ctx.err_len = err_len;

        ChValue pat = tr->patterns[i];
        if (!ch_is_pair(pat) || !ch_is_pair(use_root)) {
            continue;
        }
        size_t extra_base = vm->gc.extra_root_count;
        if (!match_list(&ctx, ch_cdr(pat), ch_cdr(use_root), 0)) {
            vm->gc.extra_root_count = extra_base;
            continue;
        }
        (void)reverse_all_ellipsis;
        size_t roots_before = vm->gc.root_count;
        *out = instantiate(&ctx, tr->templates[i]);
        if (vm->gc.root_count > roots_before) {
            ch_gc_pop_n(&vm->gc, vm->gc.root_count - roots_before);
        }
        vm->gc.extra_root_count = extra_base;
        ch_gc_push(&vm->gc, out);
        vm->active_lib_env = saved_env;
        ch_gc_pop_n(&vm->gc, 2);
        return CH_EXPAND_OK;
    }
    vm->active_lib_env = saved_env;
    ch_gc_pop(&vm->gc);
    snprintf(err, err_len, "macro: no matching syntax-rules pattern");
    return CH_EXPAND_ERROR;
}

ChTransformer *ch_vm_lookup_macro(ChVM *vm, ChSymbol *name) {
    const char *base = ch_symbol_basename(name);
    for (size_t i = 0; i < vm->macro_count; i++) {
        if (strcmp(ch_symbol_basename(vm->macros[i].name), base) == 0) {
            return ch_as_transformer(vm->macros[i].transformer);
        }
    }
    return NULL;
}

int ch_vm_define_macro(ChVM *vm, ChSymbol *name, ChTransformer *tr) {
    ChValue tr_v = ch_make_pointer(&tr->header);
    ChLibEnv *home = vm->active_lib_env;
    for (size_t i = 0; i < vm->macro_count; i++) {
        if (strcmp(ch_symbol_basename(vm->macros[i].name), ch_symbol_basename(name)) == 0) {
            vm->macros[i].transformer = tr_v;
            if (home) {
                vm->macros[i].home_env = home;
            }
            return 0;
        }
    }
    if (vm->macro_count >= CH_VM_MAX_MACROS) {
        return -1;
    }
    vm->macros[vm->macro_count].name = name;
    vm->macros[vm->macro_count].transformer = tr_v;
    vm->macros[vm->macro_count].home_env = home;
    vm->macro_count++;
    return 0;
}
