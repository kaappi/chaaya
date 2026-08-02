#include "chaaya/expander.h"

#include "chaaya/eval.h"
#include "chaaya/features.h"
#include "chaaya/library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_BIND_MAX 64
#define CH_HYG_MAX 4096
#define CH_EXPAND_DEPTH_MAX 256
#define CH_ELLIPSIS_MAX 64

static thread_local const ChUseSiteBindingCheck *active_use_check = NULL;

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

static int literal_index(ChTransformer *tr, ChSymbol *s) {
    const char *base = ch_symbol_basename(s);
    for (size_t i = 0; i < tr->literal_count; i++) {
        if (tr->literals[i] == s || strcmp(ch_symbol_basename(tr->literals[i]), base) == 0) {
            return (int)i;
        }
    }
    return -1;
}

typedef struct ChBinding {
    ChSymbol *var;
    ChValue value; /* single match, or list of matches for ellipsis */
    int ellipsis;  /* 0 = scalar; >0 = ellipsis depth (1 = list, 2 = list-of-lists) */
} ChBinding;

typedef struct ChHygRename {
    ChSymbol *from;
    ChSymbol *to;
} ChHygRename;

typedef struct ChExpandCtx {
    ChVM *vm;
    ChTransformer *tr;
    const ChUseSiteBindingCheck *use_check;
    ChBinding binds[CH_BIND_MAX];
    int nbinds;
    ChHygRename renames[CH_HYG_MAX];
    int nrenames;
    int escape; /* inside (... <template>) ellipsis escape */
    int in_quote;
    char *err;
    size_t err_len;
} ChExpandCtx;

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

static int is_well_known(const char *base) {
    static const char *names[] = {
        "quote",
        "quasiquote",
        "unquote",
        "unquote-splicing",
        "if",
        "lambda",
        "case-lambda",
        "delay",
        "delay-force",
        "define",
        "define-values",
        "set!",
        "begin",
        "and",
        "or",
        "let",
        "let*",
        "letrec",
        "let-values",
        "let*-values",
        "cond",
        "case",
        "when",
        "unless",
        "do",
        "define-syntax",
        "let-syntax",
        "letrec-syntax",
        "define-property",
        "syntax-rules",
        "else",
        "=>",
        "...",
        "call/cc",
        "call-with-current-continuation",
        "dynamic-wind",
        /* Common primitives — free template refs resolve to these bindings. */
        "list",
        "cons",
        "car",
        "cdr",
        "append",
        "null?",
        "pair?",
        "not",
        "eq?",
        "eqv?",
        "equal?",
        "+",
        "-",
        "*",
        "/",
        "=",
        "<",
        ">",
        "<=",
        ">=",
        "display",
        "write",
        "newline",
        "apply",
        "map",
        "values",
        "call-with-values",
        "error",
        "raise",
        "exit",
        "guard",
        "with-exception-handler",
        "force",
        "abs",
        "real?",
        "complex?",
        "inexact?",
        "number?",
        "memv",
        "assv",
        "cadr",
        NULL};
    for (int i = 0; names[i]; i++) {
        if (strcmp(base, names[i]) == 0) {
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
    ChValue rev = CH_NIL;
    ch_gc_push(gc, &rev);
    while (ch_is_pair(lst)) {
        ChValue item = ch_car(lst);
        ch_gc_push(gc, &item);
        rev = ch_gc_cons(gc, item, rev);
        ch_gc_pop(gc);
        lst = ch_cdr(lst);
    }
    ch_gc_pop(gc);
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
        for (int i = 0; i < nrep && np < CH_ELLIPSIS_MAX; i++) {
            pieces[np++] = instantiate_with_index(ctx, tcar, i);
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
        ch_gc_pop(&ctx->vm->gc);
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

static void capture_transformer_templates(ChVM *vm, ChTransformer *tr);

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
        if (!match_list(&ctx, ch_cdr(pat), ch_cdr(use_root), 0)) {
            continue;
        }
        (void)reverse_all_ellipsis;
        size_t roots_before = vm->gc.root_count;
        *out = instantiate(&ctx, tr->templates[i]);
        if (vm->gc.root_count > roots_before) {
            ch_gc_pop_n(&vm->gc, vm->gc.root_count - roots_before);
        }
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

static ChExpandStatus expand_form(ChVM *vm, ChValue expr, ChValue *out, char *err, size_t err_len,
                                  int depth);

typedef struct ChMacroSave {
    ChSymbol *name;
    ChValue old_transformer; /* CH_NIL = unbound */
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

static int tr_is_ellipsis_sym(ChTransformer *tr, ChSymbol *s) {
    const char *base = ch_symbol_basename(s);
    if (tr->ellipsis_id) {
        return strcmp(ch_symbol_basename(tr->ellipsis_id), base) == 0;
    }
    return strcmp(base, "...") == 0;
}

static int pvar_listed(ChSymbol **pvars, int npvars, ChSymbol *s) {
    const char *base = ch_symbol_basename(s);
    for (int i = 0; i < npvars; i++) {
        if (strcmp(ch_symbol_basename(pvars[i]), base) == 0) {
            return 1;
        }
    }
    return 0;
}

static void collect_pattern_vars(ChTransformer *tr, ChValue pat, ChSymbol **pvars, int *npvars) {
    if (ch_is_symbol(pat)) {
        ChSymbol *s = ch_as_symbol(pat);
        const char *base = ch_symbol_basename(s);
        if (strcmp(base, "_") == 0 || tr_is_ellipsis_sym(tr, s) || literal_index(tr, s) >= 0) {
            return;
        }
        if (pvar_listed(pvars, *npvars, s)) {
            return;
        }
        if (*npvars < CH_BIND_MAX) {
            pvars[(*npvars)++] = s;
        }
        return;
    }
    if (ch_is_pair(pat)) {
        collect_pattern_vars(tr, ch_car(pat), pvars, npvars);
        collect_pattern_vars(tr, ch_cdr(pat), pvars, npvars);
        return;
    }
    if (ch_is_vector(pat)) {
        ChVector *vec = ch_as_vector(pat);
        for (size_t i = 0; i < vec->len; i++) {
            collect_pattern_vars(tr, vec->items[i], pvars, npvars);
        }
    }
}

static ChValue capture_template_syms(ChVM *vm, ChTransformer *tr, ChValue tmpl, ChSymbol **pvars,
                                     int npvars) {
    if (ch_is_symbol(tmpl)) {
        ChSymbol *s = ch_as_symbol(tmpl);
        if (is_well_known(ch_symbol_basename(s))) {
            return tmpl;
        }
        /* Pattern variables must stay substitutable — do not rename them even
         * if a same-named macro exists in the ambient environment (e.g. harness
         * `test` colliding with a pattern var `test`). */
        if (pvar_listed(pvars, npvars, s) || strcmp(ch_symbol_basename(s), "_") == 0 ||
            tr_is_ellipsis_sym(tr, s)) {
            return tmpl;
        }
        ChValue macro = lookup_macro_value(vm, s);
        if (macro != CH_NIL) {
            char buf[256];
            snprintf(buf, sizeof(buf), "__hyg_cap_%u_%s", vm->hyg_counter++,
                     ch_symbol_basename(s));
            ChSymbol *cap = ch_as_symbol(ch_gc_intern_symbol_cstr(&vm->gc, buf));
            ch_vm_define_macro(vm, cap, ch_as_transformer(macro));
            return ch_make_pointer(&cap->header);
        }
        return tmpl;
    }
    if (ch_is_pair(tmpl)) {
        if (ch_is_symbol(ch_car(tmpl)) &&
            strcmp(ch_symbol_basename(ch_as_symbol(ch_car(tmpl))), "quote") == 0) {
            return tmpl;
        }
        ChValue car_v = capture_template_syms(vm, tr, ch_car(tmpl), pvars, npvars);
        ch_gc_push(&vm->gc, &car_v);
        ChValue cdr_v = capture_template_syms(vm, tr, ch_cdr(tmpl), pvars, npvars);
        ch_gc_push(&vm->gc, &cdr_v);
        ChValue out = ch_gc_cons(&vm->gc, car_v, cdr_v);
        ch_gc_pop_n(&vm->gc, 2);
        return out;
    }
    return tmpl;
}

static void capture_transformer_templates(ChVM *vm, ChTransformer *tr) {
    for (size_t i = 0; i < tr->rule_count; i++) {
        ChSymbol *pvars[CH_BIND_MAX];
        int npvars = 0;
        /* Skip the pattern keyword (car of list patterns). */
        ChValue pat = tr->patterns[i];
        if (ch_is_pair(pat) && ch_is_symbol(ch_car(pat))) {
            collect_pattern_vars(tr, ch_cdr(pat), pvars, &npvars);
        } else {
            collect_pattern_vars(tr, pat, pvars, &npvars);
        }
        tr->templates[i] = capture_template_syms(vm, tr, tr->templates[i], pvars, npvars);
    }
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

static ChExpandStatus parse_transformer_spec(ChVM *vm, ChValue spec, ChTransformer **out, char *err,
                                             size_t err_len) {
    return ch_parse_syntax_rules(vm, spec, out, err, err_len);
}

static ChExpandStatus expand_define_property(ChVM *vm, ChValue args, ChValue *out, char *err,
                                             size_t err_len) {
    if (!ch_is_pair(args) || !ch_is_symbol(ch_car(args)) || !ch_is_pair(ch_cdr(args))) {
        snprintf(err, err_len, "define-property: bad syntax");
        return CH_EXPAND_ERROR;
    }
    ChSymbol *id = ch_as_symbol(ch_car(args));
    ChValue rest1 = ch_cdr(args);
    if (!ch_is_symbol(ch_car(rest1)) || !ch_is_pair(ch_cdr(rest1)) ||
        !ch_is_nil(ch_cdr(ch_cdr(rest1)))) {
        snprintf(err, err_len, "define-property: bad syntax");
        return CH_EXPAND_ERROR;
    }
    ChSymbol *key = ch_as_symbol(ch_car(rest1));
    ChValue expr = ch_car(ch_cdr(rest1));
    ChValue val = CH_VOID;
    ch_gc_push(&vm->gc, &val);
    if (ch_eval_datum(vm, expr, CH_VOID, &val) != 0) {
        ch_gc_pop(&vm->gc);
        snprintf(err, err_len, "%s", vm->error);
        return CH_EXPAND_ERROR;
    }
    if (ch_vm_syntax_property_set(vm, ch_symbol_basename(id), ch_symbol_basename(key), val) != 0) {
        ch_gc_pop(&vm->gc);
        snprintf(err, err_len, "define-property: property table full");
        return CH_EXPAND_ERROR;
    }
    ch_gc_pop(&vm->gc);
    *out = CH_VOID;
    return CH_EXPAND_OK;
}

static ChExpandStatus expand_let_syntax(ChVM *vm, ChValue bindings, ChValue body, int letrec,
                                        ChValue *out, char *err, size_t err_len, int depth) {
    if (ch_is_nil(body)) {
        snprintf(err, err_len, "let-syntax: missing body");
        return CH_EXPAND_ERROR;
    }

    ChMacroSave saves[CH_BIND_MAX];
    int nsaves = 0;
    ChSymbol *names[CH_BIND_MAX];
    ChTransformer *transformers[CH_BIND_MAX];
    int nbinds = 0;

    if (!letrec) {
        for (ChValue bl = bindings; ch_is_pair(bl); bl = ch_cdr(bl)) {
            ChValue bind = ch_car(bl);
            if (!ch_is_pair(bind) || !ch_is_symbol(ch_car(bind)) || !ch_is_pair(ch_cdr(bind))) {
                snprintf(err, err_len, "let-syntax: bad binding");
                return CH_EXPAND_ERROR;
            }
            if (nbinds >= CH_BIND_MAX) {
                snprintf(err, err_len, "let-syntax: too many bindings");
                return CH_EXPAND_ERROR;
            }
            ChSymbol *kw = ch_as_symbol(ch_car(bind));
            ChValue spec = ch_car(ch_cdr(bind));
            ChTransformer *tr = NULL;
            if (parse_transformer_spec(vm, spec, &tr, err, err_len) != CH_EXPAND_OK) {
                for (int i = 0; i < nsaves; i++) {
                    restore_macro_binding(vm, &saves[i]);
                }
                return CH_EXPAND_ERROR;
            }
            capture_transformer_templates(vm, tr);
            saves[nsaves].name = kw;
            saves[nsaves].old_transformer = lookup_macro_value(vm, kw);
            nsaves++;
            names[nbinds] = kw;
            transformers[nbinds++] = tr;
        }
        if (!ch_is_nil(bindings) && !ch_is_pair(bindings)) {
            snprintf(err, err_len, "let-syntax: bad binding list");
            return CH_EXPAND_ERROR;
        }
        for (int i = 0; i < nbinds; i++) {
            if (ch_vm_define_macro(vm, names[i], transformers[i]) != 0) {
                for (int j = 0; j < nsaves; j++) {
                    restore_macro_binding(vm, &saves[j]);
                }
                snprintf(err, err_len, "let-syntax: too many macros");
                return CH_EXPAND_ERROR;
            }
        }
    } else {
        for (ChValue bl = bindings; ch_is_pair(bl); bl = ch_cdr(bl)) {
            ChValue bind = ch_car(bl);
            if (!ch_is_pair(bind) || !ch_is_symbol(ch_car(bind)) || !ch_is_pair(ch_cdr(bind))) {
                snprintf(err, err_len, "letrec-syntax: bad binding");
                return CH_EXPAND_ERROR;
            }
            if (nsaves >= CH_BIND_MAX) {
                snprintf(err, err_len, "letrec-syntax: too many bindings");
                return CH_EXPAND_ERROR;
            }
            ChSymbol *kw = ch_as_symbol(ch_car(bind));
            ChValue spec = ch_car(ch_cdr(bind));
            saves[nsaves].name = kw;
            saves[nsaves].old_transformer = lookup_macro_value(vm, kw);
            nsaves++;
            ChTransformer *tr = NULL;
            if (parse_transformer_spec(vm, spec, &tr, err, err_len) != CH_EXPAND_OK) {
                for (int i = 0; i < nsaves; i++) {
                    restore_macro_binding(vm, &saves[i]);
                }
                return CH_EXPAND_ERROR;
            }
            if (ch_vm_define_macro(vm, kw, tr) != 0) {
                for (int i = 0; i < nsaves; i++) {
                    restore_macro_binding(vm, &saves[i]);
                }
                snprintf(err, err_len, "letrec-syntax: too many macros");
                return CH_EXPAND_ERROR;
            }
        }
        if (!ch_is_nil(bindings) && !ch_is_pair(bindings)) {
            snprintf(err, err_len, "letrec-syntax: bad binding list");
            return CH_EXPAND_ERROR;
        }
    }

    ChValue begin_sym = ch_gc_intern_symbol_cstr(&vm->gc, "begin");
    ChValue begin_form = ch_gc_cons(&vm->gc, begin_sym, body);
    ChExpandStatus st = expand_form(vm, begin_form, out, err, err_len, depth + 1);
    for (int i = 0; i < nsaves; i++) {
        restore_macro_binding(vm, &saves[i]);
    }
    return st;
}

static ChExpandStatus expand_form(ChVM *vm, ChValue expr, ChValue *out, char *err, size_t err_len,
                                  int depth);

static ChValue list1(ChGC *gc, ChValue a) {
    return ch_gc_cons(gc, a, CH_NIL);
}

static ChValue list2(ChGC *gc, ChValue a, ChValue b) {
    return ch_gc_cons(gc, a, list1(gc, b));
}

static ChValue list3(ChGC *gc, ChValue a, ChValue b, ChValue c) {
    return ch_gc_cons(gc, a, list2(gc, b, c));
}

static ChValue list4(ChGC *gc, ChValue a, ChValue b, ChValue c, ChValue d) {
    return ch_gc_cons(gc, a, list3(gc, b, c, d));
}

static ChValue append_one(ChGC *gc, ChValue list, ChValue item) {
    if (ch_is_nil(list)) {
        return list1(gc, item);
    }
    ChValue head = CH_NIL;
    ChValue tail = CH_NIL;
    ch_gc_push(gc, &head);
    ch_gc_push(gc, &tail);
    ch_gc_push(gc, &item);
    for (ChValue p = list; ch_is_pair(p); p = ch_cdr(p)) {
        ChValue cell = ch_gc_cons(gc, ch_car(p), CH_NIL);
        if (ch_is_nil(head)) {
            head = cell;
            tail = cell;
        } else {
            ch_set_cdr(tail, cell);
            tail = cell;
        }
    }
    ChValue cell = ch_gc_cons(gc, item, CH_NIL);
    if (ch_is_nil(head)) {
        head = cell;
    } else {
        ch_set_cdr(tail, cell);
    }
    ch_gc_pop_n(gc, 3);
    return head;
}

static int is_r6rs_clause_keyword(const char *name) {
    static const char *keywords[] = {"fields", "mutable", "immutable", "parent",
                                     "protocol", "sealed", "opaque", "nongenerative",
                                     "generative", "parent-rtd", NULL};
    for (int i = 0; keywords[i]; i++) {
        if (strcmp(name, keywords[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int looks_like_r6rs_clause_syntax(ChValue args) {
    if (!ch_is_pair(args)) {
        return 0;
    }
    ChValue rest = ch_cdr(args);
    if (!ch_is_pair(rest)) {
        return 0;
    }
    ChValue second = ch_car(rest);
    if (!ch_is_pair(second) || !ch_is_symbol(ch_car(second))) {
        return 0;
    }
    if (!is_r6rs_clause_keyword(ch_symbol_basename(ch_as_symbol(ch_car(second))))) {
        return 0;
    }
    ChValue third_cell = ch_cdr(rest);
    /* R7RS always has a bare-symbol predicate as the 3rd element. */
    if (ch_is_pair(third_cell) && ch_is_symbol(ch_car(third_cell))) {
        return 0;
    }
    return 1;
}

static ChValue lookup_defined_value(ChVM *vm, const char *name) {
    if (vm->active_lib_env) {
        for (size_t i = 0; i < vm->active_lib_env->count; i++) {
            if (vm->active_lib_env->bindings[i].defined &&
                strcmp(ch_symbol_basename(vm->active_lib_env->bindings[i].name), name) == 0) {
                return vm->active_lib_env->bindings[i].value;
            }
        }
    }
    for (size_t i = 0; i < vm->global_count; i++) {
        if (vm->globals[i].defined && strcmp(ch_symbol_basename(vm->globals[i].name), name) == 0) {
            return vm->globals[i].value;
        }
    }
    return CH_UNDEFINED;
}

static ChExpandStatus expand_define_record_type_r6rs(ChVM *vm, ChValue args, ChValue *out, char *err,
                                                    size_t err_len) {
    if (!ch_is_pair(args)) {
        snprintf(err, err_len, "define-record-type: bad R6RS syntax");
        return CH_EXPAND_ERROR;
    }
    ChGC *gc = &vm->gc;
    const char *type_name = NULL;
    ChSymbol *ctor_sym = NULL;
    ChSymbol *pred_sym = NULL;
    ChValue name_spec = ch_car(args);
    if (ch_is_symbol(name_spec)) {
        type_name = ch_as_symbol(name_spec)->name;
        char buf[256];
        if (snprintf(buf, sizeof(buf), "make-%s", type_name) >= (int)sizeof(buf)) {
            snprintf(err, err_len, "define-record-type: type name too long");
            return CH_EXPAND_ERROR;
        }
        ctor_sym = ch_as_symbol(ch_gc_intern_symbol_cstr(gc, buf));
        if (snprintf(buf, sizeof(buf), "%s?", type_name) >= (int)sizeof(buf)) {
            snprintf(err, err_len, "define-record-type: type name too long");
            return CH_EXPAND_ERROR;
        }
        pred_sym = ch_as_symbol(ch_gc_intern_symbol_cstr(gc, buf));
    } else if (ch_is_pair(name_spec) && ch_is_symbol(ch_car(name_spec)) &&
               ch_is_pair(ch_cdr(name_spec)) && ch_is_symbol(ch_car(ch_cdr(name_spec))) &&
               ch_is_pair(ch_cdr(ch_cdr(name_spec))) &&
               ch_is_symbol(ch_car(ch_cdr(ch_cdr(name_spec)))) &&
               ch_is_nil(ch_cdr(ch_cdr(ch_cdr(name_spec))))) {
        type_name = ch_as_symbol(ch_car(name_spec))->name;
        ctor_sym = ch_as_symbol(ch_car(ch_cdr(name_spec)));
        pred_sym = ch_as_symbol(ch_car(ch_cdr(ch_cdr(name_spec))));
    } else {
        snprintf(err, err_len, "define-record-type: bad R6RS name spec");
        return CH_EXPAND_ERROR;
    }

    ChSymbol *parent_sym = NULL;
    ChSymbol *field_names[CH_RECORD_MAX_FIELDS];
    ChSymbol *acc_syms[CH_RECORD_MAX_FIELDS];
    ChSymbol *mut_syms[CH_RECORD_MAX_FIELDS];
    size_t nfields = 0;

    for (ChValue clauses = ch_cdr(args); ch_is_pair(clauses); clauses = ch_cdr(clauses)) {
        ChValue clause = ch_car(clauses);
        if (!ch_is_pair(clause) || !ch_is_symbol(ch_car(clause))) {
            snprintf(err, err_len, "define-record-type: bad R6RS clause");
            return CH_EXPAND_ERROR;
        }
        const char *kw = ch_symbol_basename(ch_as_symbol(ch_car(clause)));
        ChValue crest = ch_cdr(clause);
        if (strcmp(kw, "fields") == 0) {
            for (ChValue fs = crest; ch_is_pair(fs); fs = ch_cdr(fs)) {
                if (nfields >= CH_RECORD_MAX_FIELDS) {
                    snprintf(err, err_len, "define-record-type: too many fields");
                    return CH_EXPAND_ERROR;
                }
                ChValue fspec = ch_car(fs);
                const char *fname = NULL;
                int is_mutable = 0;
                ChSymbol *acc = NULL;
                ChSymbol *mut = NULL;
                if (ch_is_symbol(fspec)) {
                    fname = ch_as_symbol(fspec)->name;
                } else if (ch_is_pair(fspec) && ch_is_symbol(ch_car(fspec))) {
                    const char *kind = ch_symbol_basename(ch_as_symbol(ch_car(fspec)));
                    if (strcmp(kind, "mutable") == 0) {
                        is_mutable = 1;
                    } else if (strcmp(kind, "immutable") != 0) {
                        snprintf(err, err_len, "define-record-type: bad field kind");
                        return CH_EXPAND_ERROR;
                    }
                    ChValue r1 = ch_cdr(fspec);
                    if (!ch_is_pair(r1) || !ch_is_symbol(ch_car(r1))) {
                        snprintf(err, err_len, "define-record-type: bad field spec");
                        return CH_EXPAND_ERROR;
                    }
                    fname = ch_as_symbol(ch_car(r1))->name;
                    ChValue r2 = ch_cdr(r1);
                    if (ch_is_pair(r2) && ch_is_symbol(ch_car(r2))) {
                        acc = ch_as_symbol(ch_car(r2));
                        ChValue r3 = ch_cdr(r2);
                        if (is_mutable && ch_is_pair(r3) && ch_is_symbol(ch_car(r3))) {
                            mut = ch_as_symbol(ch_car(r3));
                        }
                    }
                } else {
                    snprintf(err, err_len, "define-record-type: bad field spec");
                    return CH_EXPAND_ERROR;
                }
                char buf[256];
                field_names[nfields] = ch_as_symbol(ch_gc_intern_symbol_cstr(gc, fname));
                if (!acc) {
                    if (snprintf(buf, sizeof(buf), "%s-%s", type_name, fname) >= (int)sizeof(buf)) {
                        snprintf(err, err_len, "define-record-type: field name too long");
                        return CH_EXPAND_ERROR;
                    }
                    acc = ch_as_symbol(ch_gc_intern_symbol_cstr(gc, buf));
                }
                acc_syms[nfields] = acc;
                if (is_mutable) {
                    if (!mut) {
                        if (snprintf(buf, sizeof(buf), "%s-set!", acc->name) >= (int)sizeof(buf)) {
                            snprintf(err, err_len, "define-record-type: accessor name too long");
                            return CH_EXPAND_ERROR;
                        }
                        mut = ch_as_symbol(ch_gc_intern_symbol_cstr(gc, buf));
                    }
                    mut_syms[nfields] = mut;
                } else {
                    mut_syms[nfields] = NULL;
                }
                nfields++;
            }
        } else if (strcmp(kw, "parent") == 0) {
            if (!ch_is_pair(crest) || !ch_is_symbol(ch_car(crest)) || !ch_is_nil(ch_cdr(crest))) {
                snprintf(err, err_len, "define-record-type: bad parent clause");
                return CH_EXPAND_ERROR;
            }
            parent_sym = ch_as_symbol(ch_car(crest));
        } else if (strcmp(kw, "sealed") == 0 || strcmp(kw, "opaque") == 0 ||
                   strcmp(kw, "nongenerative") == 0 || strcmp(kw, "generative") == 0 ||
                   strcmp(kw, "protocol") == 0 || strcmp(kw, "parent-rtd") == 0) {
            /* Accepted for syntax compatibility; protocol/parent-rtd not fully
             * implemented beyond parent flattening for the compliance suite. */
            if (strcmp(kw, "protocol") == 0 || strcmp(kw, "parent-rtd") == 0) {
                snprintf(err, err_len, "define-record-type: %s not yet supported", kw);
                return CH_EXPAND_ERROR;
            }
        } else {
            snprintf(err, err_len, "define-record-type: unknown R6RS clause");
            return CH_EXPAND_ERROR;
        }
    }

    char iname[256];
    if (snprintf(iname, sizeof(iname), "__record_type_%s", type_name) >= (int)sizeof(iname)) {
        snprintf(err, err_len, "define-record-type: type name too long");
        return CH_EXPAND_ERROR;
    }

    uint16_t parent_n = 0;
    ChValue parent_rt_name = CH_FALSE;
    if (parent_sym) {
        char pname[256];
        if (snprintf(pname, sizeof(pname), "__record_type_%s", parent_sym->name) >=
            (int)sizeof(pname)) {
            snprintf(err, err_len, "define-record-type: parent name too long");
            return CH_EXPAND_ERROR;
        }
        ChValue pval = lookup_defined_value(vm, pname);
        if (!ch_is_record_type(pval)) {
            snprintf(err, err_len, "define-record-type: unknown parent type");
            return CH_EXPAND_ERROR;
        }
        parent_n = ch_as_record_type(pval)->num_fields;
        parent_rt_name = ch_gc_intern_symbol_cstr(gc, pname);
    }

    ChValue begin_sym = ch_gc_intern_symbol_cstr(gc, "begin");
    ChValue define_sym = ch_gc_intern_symbol_cstr(gc, "define");
    ChValue let_sym = ch_gc_intern_symbol_cstr(gc, "let");
    ChValue lambda_sym = ch_gc_intern_symbol_cstr(gc, "lambda");
    ChValue mrt_sym = ch_gc_intern_symbol_cstr(gc, "%make-record-type");
    ChValue mr_sym = ch_gc_intern_symbol_cstr(gc, "%make-record");
    ChValue rp_sym = ch_gc_intern_symbol_cstr(gc, "%record?");
    ChValue rr_sym = ch_gc_intern_symbol_cstr(gc, "%record-ref");
    ChValue rs_sym = ch_gc_intern_symbol_cstr(gc, "%record-set!");
    ChValue rt_name = ch_gc_intern_symbol_cstr(gc, iname);
    ChValue rt_local = ch_gc_intern_symbol_cstr(gc, " __rt");
    ChValue name_str = ch_gc_make_string_cstr(gc, type_name);
    ChValue nfields_v = ch_make_fixnum((int64_t)nfields);

    ChValue forms = CH_NIL;
    ch_gc_push(gc, &forms);
    ch_gc_push(gc, &name_str);
    ch_gc_push(gc, &rt_name);
    ch_gc_push(gc, &rt_local);
    ch_gc_push(gc, &parent_rt_name);

    /* (define __rt (%make-record-type "name" own-n [parent])) */
    {
        ChValue init;
        if (parent_sym) {
            init = list4(gc, mrt_sym, name_str, nfields_v, parent_rt_name);
        } else {
            init = list3(gc, mrt_sym, name_str, nfields_v);
        }
        forms = append_one(gc, forms, list3(gc, define_sym, rt_name, init));
    }

    /* Constructor: (lambda (p0.. p{parent_n-1} f0..) (%make-record __rt ...)) */
    {
        ChValue params = CH_NIL;
        ChValue body_args = list1(gc, rt_local);
        ch_gc_push(gc, &params);
        ch_gc_push(gc, &body_args);
        for (uint16_t i = 0; i < parent_n; i++) {
            char pbuf[32];
            snprintf(pbuf, sizeof(pbuf), " __pf%u", (unsigned)i);
            ChValue ps = ch_gc_intern_symbol_cstr(gc, pbuf);
            params = append_one(gc, params, ps);
            body_args = append_one(gc, body_args, ps);
        }
        for (size_t fi = 0; fi < nfields; fi++) {
            ChValue fs = ch_make_pointer(&field_names[fi]->header);
            params = append_one(gc, params, fs);
            body_args = append_one(gc, body_args, fs);
        }
        ChValue body = ch_gc_cons(gc, mr_sym, body_args);
        ChValue lam = list3(gc, lambda_sym, params, body);
        ChValue let_expr = list3(gc, let_sym, list1(gc, list2(gc, rt_local, rt_name)), lam);
        forms = append_one(gc, forms, list3(gc, define_sym, ch_make_pointer(&ctor_sym->header), let_expr));
        ch_gc_pop_n(gc, 2);
    }

    /* predicate */
    {
        ChValue v_sym = ch_gc_intern_symbol_cstr(gc, "v");
        ChValue body = list3(gc, rp_sym, v_sym, rt_local);
        ChValue lam = list3(gc, lambda_sym, list1(gc, v_sym), body);
        ChValue let_expr = list3(gc, let_sym, list1(gc, list2(gc, rt_local, rt_name)), lam);
        forms = append_one(gc, forms, list3(gc, define_sym, ch_make_pointer(&pred_sym->header), let_expr));
    }

    for (size_t fi = 0; fi < nfields; fi++) {
        ChValue p_sym = ch_gc_intern_symbol_cstr(gc, "p");
        ChValue idx = ch_make_fixnum((int64_t)parent_n + (int64_t)fi);
        ChValue body = list4(gc, rr_sym, p_sym, idx, rt_local);
        ChValue lam = list3(gc, lambda_sym, list1(gc, p_sym), body);
        ChValue let_expr = list3(gc, let_sym, list1(gc, list2(gc, rt_local, rt_name)), lam);
        forms = append_one(gc, forms, list3(gc, define_sym, ch_make_pointer(&acc_syms[fi]->header), let_expr));
        if (mut_syms[fi]) {
            ChValue v_sym = ch_gc_intern_symbol_cstr(gc, "v");
            ChValue set_body = list1(gc, p_sym);
            set_body = append_one(gc, set_body, idx);
            set_body = append_one(gc, set_body, v_sym);
            set_body = append_one(gc, set_body, rt_local);
            set_body = ch_gc_cons(gc, rs_sym, set_body);
            ChValue set_lam = list3(gc, lambda_sym, list2(gc, p_sym, v_sym), set_body);
            ChValue set_let = list3(gc, let_sym, list1(gc, list2(gc, rt_local, rt_name)), set_lam);
            forms = append_one(gc, forms, list3(gc, define_sym, ch_make_pointer(&mut_syms[fi]->header), set_let));
        }
    }

    *out = ch_gc_cons(gc, begin_sym, forms);
    ch_gc_pop_n(gc, 5);
    return CH_EXPAND_OK;
}

static ChExpandStatus expand_define_record_type(ChVM *vm, ChValue args, ChValue *out, char *err,
                                               size_t err_len) {
    if (looks_like_r6rs_clause_syntax(args)) {
        return expand_define_record_type_r6rs(vm, args, out, err, err_len);
    }
    /* (name (ctor f ...) pred (field acc [mut]) ...) */
    if (!ch_is_pair(args) || !ch_is_symbol(ch_car(args))) {
        snprintf(err, err_len, "define-record-type: bad type name");
        return CH_EXPAND_ERROR;
    }
    ChSymbol *type_sym = ch_as_symbol(ch_car(args));
    ChValue rest = ch_cdr(args);
    if (!ch_is_pair(rest) || !ch_is_pair(ch_car(rest)) || !ch_is_symbol(ch_car(ch_car(rest)))) {
        snprintf(err, err_len, "define-record-type: bad constructor");
        return CH_EXPAND_ERROR;
    }
    ChValue ctor_form = ch_car(rest);
    ChSymbol *ctor_sym = ch_as_symbol(ch_car(ctor_form));
    ChValue ctor_fields = ch_cdr(ctor_form);
    rest = ch_cdr(rest);
    if (!ch_is_pair(rest) || !ch_is_symbol(ch_car(rest))) {
        snprintf(err, err_len, "define-record-type: bad predicate");
        return CH_EXPAND_ERROR;
    }
    ChSymbol *pred_sym = ch_as_symbol(ch_car(rest));
    ChValue field_clauses = ch_cdr(rest);

    ChSymbol *field_syms[CH_RECORD_MAX_FIELDS];
    ChSymbol *acc_syms[CH_RECORD_MAX_FIELDS];
    ChSymbol *mut_syms[CH_RECORD_MAX_FIELDS];
    size_t nfields = 0;
    for (ChValue c = field_clauses; ch_is_pair(c); c = ch_cdr(c)) {
        ChValue clause = ch_car(c);
        if (!ch_is_pair(clause) || !ch_is_symbol(ch_car(clause)) || !ch_is_pair(ch_cdr(clause)) ||
            !ch_is_symbol(ch_car(ch_cdr(clause)))) {
            snprintf(err, err_len, "define-record-type: bad field clause");
            return CH_EXPAND_ERROR;
        }
        if (nfields >= CH_RECORD_MAX_FIELDS) {
            snprintf(err, err_len, "define-record-type: too many fields");
            return CH_EXPAND_ERROR;
        }
        field_syms[nfields] = ch_as_symbol(ch_car(clause));
        acc_syms[nfields] = ch_as_symbol(ch_car(ch_cdr(clause)));
        ChValue mut = ch_cdr(ch_cdr(clause));
        if (ch_is_pair(mut) && ch_is_symbol(ch_car(mut))) {
            mut_syms[nfields] = ch_as_symbol(ch_car(mut));
        } else {
            mut_syms[nfields] = NULL;
        }
        nfields++;
    }

    char iname[256];
    if (snprintf(iname, sizeof(iname), "__record_type_%s", type_sym->name) >= (int)sizeof(iname)) {
        snprintf(err, err_len, "define-record-type: type name too long");
        return CH_EXPAND_ERROR;
    }

    ChGC *gc = &vm->gc;
    ChValue begin_sym = ch_gc_intern_symbol_cstr(gc, "begin");
    ChValue define_sym = ch_gc_intern_symbol_cstr(gc, "define");
    ChValue let_sym = ch_gc_intern_symbol_cstr(gc, "let");
    ChValue lambda_sym = ch_gc_intern_symbol_cstr(gc, "lambda");
    ChValue mrt_sym = ch_gc_intern_symbol_cstr(gc, "%make-record-type");
    ChValue mr_sym = ch_gc_intern_symbol_cstr(gc, "%make-record");
    ChValue rp_sym = ch_gc_intern_symbol_cstr(gc, "%record?");
    ChValue rr_sym = ch_gc_intern_symbol_cstr(gc, "%record-ref");
    ChValue rs_sym = ch_gc_intern_symbol_cstr(gc, "%record-set!");
    ChValue rt_name = ch_gc_intern_symbol_cstr(gc, iname);
    ChValue rt_local = ch_gc_intern_symbol_cstr(gc, " __rt");
    ChValue name_str = ch_gc_make_string_cstr(gc, type_sym->name);
    ChValue nfields_v = ch_make_fixnum((int64_t)nfields);

    ChValue forms = CH_NIL;
    ch_gc_push(gc, &forms);
    ch_gc_push(gc, &name_str);
    ch_gc_push(gc, &rt_name);
    ch_gc_push(gc, &rt_local);

    /* (define __rt (%make-record-type "name" n)) */
    {
        ChValue init = list3(gc, mrt_sym, name_str, nfields_v);
        ChValue def = list3(gc, define_sym, rt_name, init);
        forms = append_one(gc, forms, def);
    }

    /* (define ctor (let ((__rt __rtname)) (lambda (fs...) (%make-record __rt ...)))) */
    {
        ChValue params = ctor_fields;
        ChValue body_args = list1(gc, rt_local);
        ch_gc_push(gc, &body_args);
        for (size_t fi = 0; fi < nfields; fi++) {
            ChValue arg = CH_FALSE;
            for (ChValue p = ctor_fields; ch_is_pair(p); p = ch_cdr(p)) {
                if (ch_is_symbol(ch_car(p)) &&
                    strcmp(ch_as_symbol(ch_car(p))->name, field_syms[fi]->name) == 0) {
                    arg = ch_car(p);
                    break;
                }
            }
            /* If field not in ctor, use #f (R7RS allows omitted ctor fields). */
            if (arg == CH_FALSE) {
                /* leave #f */
            }
            body_args = append_one(gc, body_args, arg);
        }
        ChValue body = ch_gc_cons(gc, mr_sym, body_args);
        ChValue lam = list3(gc, lambda_sym, params, body);
        ChValue binding = list2(gc, rt_local, rt_name);
        ChValue bindings = list1(gc, binding);
        ChValue let_expr = list3(gc, let_sym, bindings, lam);
        ChValue ctor_name = ch_make_pointer(&ctor_sym->header);
        ChValue def = list3(gc, define_sym, ctor_name, let_expr);
        forms = append_one(gc, forms, def);
        ch_gc_pop(gc);
    }

    /* predicate */
    {
        ChValue v_sym = ch_gc_intern_symbol_cstr(gc, "v");
        ChValue body = list3(gc, rp_sym, v_sym, rt_local);
        ChValue lam = list3(gc, lambda_sym, list1(gc, v_sym), body);
        ChValue binding = list2(gc, rt_local, rt_name);
        ChValue let_expr = list3(gc, let_sym, list1(gc, binding), lam);
        ChValue def = list3(gc, define_sym, ch_make_pointer(&pred_sym->header), let_expr);
        forms = append_one(gc, forms, def);
    }

    for (size_t fi = 0; fi < nfields; fi++) {
        ChValue p_sym = ch_gc_intern_symbol_cstr(gc, "p");
        ChValue idx = ch_make_fixnum((int64_t)fi);
        ChValue body = list4(gc, rr_sym, p_sym, idx, rt_local);
        ChValue lam = list3(gc, lambda_sym, list1(gc, p_sym), body);
        ChValue binding = list2(gc, rt_local, rt_name);
        ChValue let_expr = list3(gc, let_sym, list1(gc, binding), lam);
        ChValue def = list3(gc, define_sym, ch_make_pointer(&acc_syms[fi]->header), let_expr);
        forms = append_one(gc, forms, def);

        if (mut_syms[fi]) {
            ChValue v_sym = ch_gc_intern_symbol_cstr(gc, "v");
            ChValue set_body = list1(gc, p_sym);
            set_body = append_one(gc, set_body, idx);
            set_body = append_one(gc, set_body, v_sym);
            set_body = append_one(gc, set_body, rt_local);
            set_body = ch_gc_cons(gc, rs_sym, set_body);
            ChValue params = list2(gc, p_sym, v_sym);
            ChValue set_lam = list3(gc, lambda_sym, params, set_body);
            ChValue set_let = list3(gc, let_sym, list1(gc, list2(gc, rt_local, rt_name)), set_lam);
            ChValue set_def =
                list3(gc, define_sym, ch_make_pointer(&mut_syms[fi]->header), set_let);
            forms = append_one(gc, forms, set_def);
        }
    }

    *out = ch_gc_cons(gc, begin_sym, forms);
    ch_gc_pop_n(gc, 4);
    return CH_EXPAND_OK;
}

static ChExpandStatus expand_list(ChVM *vm, ChValue list, ChValue *out, char *err, size_t err_len,
                                  int depth) {
    if (ch_is_nil(list)) {
        *out = CH_NIL;
        return CH_EXPAND_OK;
    }
    if (!ch_is_pair(list)) {
        return expand_form(vm, list, out, err, err_len, depth);
    }
    ChValue car_e = CH_NIL;
    ChValue cdr_e = CH_NIL;
    ch_gc_push(&vm->gc, &car_e);
    ch_gc_push(&vm->gc, &cdr_e);
    if (expand_form(vm, ch_car(list), &car_e, err, err_len, depth) != CH_EXPAND_OK) {
        ch_gc_pop_n(&vm->gc, 2);
        return CH_EXPAND_ERROR;
    }
    if (expand_list(vm, ch_cdr(list), &cdr_e, err, err_len, depth) != CH_EXPAND_OK) {
        ch_gc_pop_n(&vm->gc, 2);
        return CH_EXPAND_ERROR;
    }
    *out = ch_gc_cons(&vm->gc, car_e, cdr_e);
    ch_gc_pop_n(&vm->gc, 2);
    return CH_EXPAND_OK;
}

static ChExpandStatus expand_form(ChVM *vm, ChValue expr, ChValue *out, char *err, size_t err_len,
                                  int depth) {
    if (depth > CH_EXPAND_DEPTH_MAX) {
        snprintf(err, err_len, "macro expansion depth exceeded");
        return CH_EXPAND_ERROR;
    }
    if (!ch_is_pair(expr)) {
        *out = expr;
        return CH_EXPAND_OK;
    }
    ChValue head = ch_car(expr);
    if (ch_is_symbol(head)) {
        const char *base = ch_symbol_basename(ch_as_symbol(head));
        if (strcmp(base, "quote") == 0) {
            *out = expr;
            return CH_EXPAND_OK;
        }
        if (strcmp(base, "quasiquote") == 0) {
            *out = expr;
            return CH_EXPAND_OK;
        }
        if (strcmp(base, "cond-expand") == 0) {
            ChValue body = CH_NIL;
            int sel = ch_cond_expand_select(vm, ch_cdr(expr), &body, err, err_len);
            if (sel < 0) {
                return CH_EXPAND_ERROR;
            }
            if (sel == 1) {
                *out = CH_VOID;
                return CH_EXPAND_OK;
            }
            ChValue begin_sym = ch_gc_intern_symbol_cstr(&vm->gc, "begin");
            ChValue begin_form = ch_gc_cons(&vm->gc, begin_sym, body);
            return expand_form(vm, begin_form, out, err, err_len, depth + 1);
        }
        if (strcmp(base, "define-property") == 0) {
            return expand_define_property(vm, ch_cdr(expr), out, err, err_len);
        }
        if (strcmp(base, "let-syntax") == 0) {
            ChValue rest = ch_cdr(expr);
            if (!ch_is_pair(rest) || !ch_is_pair(ch_cdr(rest))) {
                snprintf(err, err_len, "let-syntax: bad syntax");
                return CH_EXPAND_ERROR;
            }
            return expand_let_syntax(vm, ch_car(rest), ch_cdr(rest), 0, out, err, err_len, depth);
        }
        if (strcmp(base, "letrec-syntax") == 0) {
            ChValue rest = ch_cdr(expr);
            if (!ch_is_pair(rest) || !ch_is_pair(ch_cdr(rest))) {
                snprintf(err, err_len, "letrec-syntax: bad syntax");
                return CH_EXPAND_ERROR;
            }
            return expand_let_syntax(vm, ch_car(rest), ch_cdr(rest), 1, out, err, err_len, depth);
        }
        if (strcmp(base, "define-record-type") == 0) {
            ChValue expanded = CH_NIL;
            ch_gc_push(&vm->gc, &expanded);
            if (expand_define_record_type(vm, ch_cdr(expr), &expanded, err, err_len) !=
                CH_EXPAND_OK) {
                ch_gc_pop(&vm->gc);
                return CH_EXPAND_ERROR;
            }
            ChExpandStatus st = expand_form(vm, expanded, out, err, err_len, depth + 1);
            ch_gc_pop(&vm->gc);
            return st;
        }
        if (strcmp(base, "define-syntax") == 0) {
            /* (define-syntax name transformer-spec) — register immediately so
             * later forms in the same expand pass can see the macro. */
            ChValue rest = ch_cdr(expr);
            if (!ch_is_pair(rest) || !ch_is_symbol(ch_car(rest)) || !ch_is_pair(ch_cdr(rest))) {
                snprintf(err, err_len, "define-syntax: bad syntax");
                return CH_EXPAND_ERROR;
            }
            ChSymbol *name = ch_as_symbol(ch_car(rest));
            ChValue spec = ch_car(ch_cdr(rest));
            ChTransformer *tr = NULL;
            if (ch_parse_syntax_rules(vm, spec, &tr, err, err_len) != CH_EXPAND_OK) {
                return CH_EXPAND_ERROR;
            }
            if (ch_vm_define_macro(vm, name, tr) != 0) {
                snprintf(err, err_len, "define-syntax: too many macros");
                return CH_EXPAND_ERROR;
            }
            if (vm->active_lib_env) {
                int idx = ch_lib_env_intern(vm->active_lib_env, name);
                if (idx < 0) {
                    snprintf(err, err_len, "define-syntax: library environment full");
                    return CH_EXPAND_ERROR;
                }
                ch_lib_env_define(vm->active_lib_env, idx, ch_make_pointer(&tr->header));
            }
            *out = CH_VOID;
            return CH_EXPAND_OK;
        }
        ChTransformer *tr = ch_vm_lookup_macro(vm, ch_as_symbol(head));
        if (tr) {
            /* Macros with literals need use-site lexical binding info
             * (R7RS §4.3.2). Top-level expand has no local contour — leave
             * these uses for the compiler's ch_expand_macro_checked path. */
            if (tr->literal_count > 0) {
                return expand_list(vm, expr, out, err, err_len, depth);
            }
            ChValue expanded = CH_NIL;
            ch_gc_push(&vm->gc, &expanded);
            if (ch_expand_macro(vm, tr, expr, &expanded, err, err_len) != CH_EXPAND_OK) {
                ch_gc_pop(&vm->gc);
                return CH_EXPAND_ERROR;
            }
            ChExpandStatus st = expand_form(vm, expanded, out, err, err_len, depth + 1);
            ch_gc_pop(&vm->gc);
            return st;
        }
    }
    return expand_list(vm, expr, out, err, err_len, depth);
}

ChExpandStatus ch_expand_toplevel(ChVM *vm, ChValue expr, ChValue *out, char *err, size_t err_len) {
    return expand_form(vm, expr, out, err, err_len, 0);
}
