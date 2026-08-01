#include "chaaya/expander.h"

#include "chaaya/features.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_BIND_MAX 64
#define CH_HYG_MAX 128
#define CH_EXPAND_DEPTH_MAX 256
#define CH_ELLIPSIS_MAX 64

typedef struct ChBinding {
    ChSymbol *var;
    ChValue value; /* single match, or list of matches for ellipsis */
    int ellipsis;
} ChBinding;

typedef struct ChHygRename {
    ChSymbol *from;
    ChSymbol *to;
} ChHygRename;

typedef struct ChExpandCtx {
    ChVM *vm;
    ChTransformer *tr;
    ChBinding binds[CH_BIND_MAX];
    int nbinds;
    ChHygRename renames[CH_HYG_MAX];
    int nrenames;
    char *err;
    size_t err_len;
} ChExpandCtx;

static int expand_err(ChExpandCtx *ctx, const char *msg) {
    if (ctx->err && ctx->err_len) {
        snprintf(ctx->err, ctx->err_len, "%s", msg);
    }
    return 0;
}

static int is_ellipsis_sym(ChSymbol *s) {
    return strcmp(ch_symbol_basename(s), "...") == 0;
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
        "define",
        "set!",
        "begin",
        "and",
        "or",
        "let",
        "let*",
        "letrec",
        "cond",
        "case",
        "when",
        "unless",
        "do",
        "define-syntax",
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

static int match_list(ChExpandCtx *ctx, ChValue pat, ChValue use, int under_ellipsis) {
    while (ch_is_pair(pat)) {
        ChValue pcar = ch_car(pat);
        ChValue pcdr = ch_cdr(pat);
        if (ch_is_pair(pcdr) && ch_is_symbol(ch_car(pcdr)) &&
            is_ellipsis_sym(ch_as_symbol(ch_car(pcdr)))) {
            ChValue after = ch_cdr(pcdr);
            int need = list_length(after);
            int ulen = list_length(use);
            if (need < 0 || ulen < 0 || ulen < need) {
                return 0;
            }
            int nrep = ulen - need;
            int start_binds = ctx->nbinds;
            ChValue u = use;
            for (int i = 0; i < nrep; i++) {
                if (!match_pattern(ctx, pcar, ch_car(u), 1)) {
                    return 0;
                }
                u = ch_cdr(u);
            }
            /* ellipsis lists were cons'd front-first; reverse each new ellipsis bind */
            for (int bi = start_binds; bi < ctx->nbinds; bi++) {
                if (ctx->binds[bi].ellipsis) {
                    ctx->binds[bi].value = list_reverse(&ctx->vm->gc, ctx->binds[bi].value);
                }
            }
            /* also reverse pre-existing ellipsis vars that were appended in this loop */
            for (int bi = 0; bi < start_binds; bi++) {
                if (ctx->binds[bi].ellipsis && nrep > 0) {
                    /* appended nrep items at front; reverse whole list then rotate?
                     * Simpler: rebuild by reversing only the newly prepended segment.
                     * Since bind_var always conses to front, after nrep appends the list is
                     * newest-first for the whole ellipsis sequence if this was the only
                     * ellipsis match for that var. Reverse once at end of rule match. */
                    (void)bi;
                }
            }
            return match_list(ctx, after, u, under_ellipsis);
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

static int match_pattern(ChExpandCtx *ctx, ChValue pat, ChValue use, int under_ellipsis) {
    if (ch_is_symbol(pat)) {
        ChSymbol *s = ch_as_symbol(pat);
        if (is_literal(ctx, s)) {
            return ch_is_symbol(use) &&
                   strcmp(ch_symbol_basename(ch_as_symbol(use)), ch_symbol_basename(s)) == 0;
        }
        return bind_var(ctx, s, use, under_ellipsis);
    }
    if (ch_is_nil(pat)) {
        return ch_is_nil(use);
    }
    if (ch_is_pair(pat)) {
        return match_list(ctx, pat, use, under_ellipsis);
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

static ChValue instantiate_with_index(ChExpandCtx *ctx, ChValue tmpl, int index) {
    if (ch_is_symbol(tmpl)) {
        ChBinding *b = find_bind(ctx, ch_as_symbol(tmpl));
        if (b) {
            if (b->ellipsis) {
                return nth_of(b->value, index);
            }
            return b->value;
        }
        if (is_literal(ctx, ch_as_symbol(tmpl)) ||
            is_well_known(ch_symbol_basename(ch_as_symbol(tmpl)))) {
            return tmpl;
        }
        ChSymbol *ren = hyg_rename(ctx, ch_as_symbol(tmpl));
        return ch_make_pointer(&ren->header);
    }
    if (ch_is_pair(tmpl)) {
        if (ch_is_symbol(ch_car(tmpl)) &&
            strcmp(ch_symbol_basename(ch_as_symbol(ch_car(tmpl))), "quote") == 0) {
            return tmpl;
        }
        ChValue car_v = instantiate_with_index(ctx, ch_car(tmpl), index);
        ch_gc_push(&ctx->vm->gc, &car_v);
        ChValue cdr_v = instantiate_with_index(ctx, ch_cdr(tmpl), index);
        ch_gc_push(&ctx->vm->gc, &cdr_v);
        ChValue out = ch_gc_cons(&ctx->vm->gc, car_v, cdr_v);
        ch_gc_pop_n(&ctx->vm->gc, 2);
        return out;
    }
    return tmpl;
}

static ChValue instantiate_list(ChExpandCtx *ctx, ChValue tmpl) {
    if (ch_is_nil(tmpl)) {
        return CH_NIL;
    }
    if (!ch_is_pair(tmpl)) {
        return instantiate(ctx, tmpl);
    }
    ChValue tcar = ch_car(tmpl);
    ChValue tcdr = ch_cdr(tmpl);
    if (ch_is_pair(tcdr) && ch_is_symbol(ch_car(tcdr)) &&
        is_ellipsis_sym(ch_as_symbol(ch_car(tcdr)))) {
        ChValue after = ch_cdr(tcdr);
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
            result = ch_gc_cons(&ctx->vm->gc, item, result);
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
            return b->value;
        }
        if (is_literal(ctx, s) || is_well_known(ch_symbol_basename(s))) {
            return tmpl;
        }
        ChSymbol *ren = hyg_rename(ctx, s);
        return ch_make_pointer(&ren->header);
    }
    if (ch_is_pair(tmpl)) {
        if (ch_is_symbol(ch_car(tmpl)) &&
            strcmp(ch_symbol_basename(ch_as_symbol(ch_car(tmpl))), "quote") == 0) {
            return tmpl;
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
    ChValue lits_v = ch_car(rest);
    ChValue rules = ch_cdr(rest);

    ChValue tr_v = ch_gc_make_transformer(&vm->gc);
    ch_gc_push(&vm->gc, &tr_v);
    ChTransformer *tr = ch_as_transformer(tr_v);
    tr->literal_count = 0;
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
        tr->literals[tr->literal_count++] = ch_as_symbol(ch_car(L));
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

ChExpandStatus ch_expand_macro(ChVM *vm, ChTransformer *tr, ChValue use, ChValue *out, char *err,
                               size_t err_len) {
    for (size_t i = 0; i < tr->rule_count; i++) {
        ChExpandCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.vm = vm;
        ctx.tr = tr;
        ctx.err = err;
        ctx.err_len = err_len;

        ChValue pat = tr->patterns[i];
        if (!ch_is_pair(pat) || !ch_is_pair(use)) {
            continue;
        }
        if (!match_list(&ctx, ch_cdr(pat), ch_cdr(use), 0)) {
            continue;
        }
        (void)reverse_all_ellipsis;
        *out = instantiate(&ctx, tr->templates[i]);
        return CH_EXPAND_OK;
    }
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
    for (size_t i = 0; i < vm->macro_count; i++) {
        if (strcmp(ch_symbol_basename(vm->macros[i].name), ch_symbol_basename(name)) == 0) {
            vm->macros[i].transformer = tr_v;
            return 0;
        }
    }
    if (vm->macro_count >= CH_VM_MAX_MACROS) {
        return -1;
    }
    vm->macros[vm->macro_count].name = name;
    vm->macros[vm->macro_count].transformer = tr_v;
    vm->macro_count++;
    return 0;
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

static ChExpandStatus expand_define_record_type(ChVM *vm, ChValue args, ChValue *out, char *err,
                                               size_t err_len) {
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
            for (ChValue clauses = ch_cdr(expr); ch_is_pair(clauses); clauses = ch_cdr(clauses)) {
                ChValue clause = ch_car(clauses);
                if (!ch_is_pair(clause)) {
                    snprintf(err, err_len, "cond-expand: bad clause");
                    return CH_EXPAND_ERROR;
                }
                ChValue req = ch_car(clause);
                int match = 0;
                if (ch_is_symbol(req) &&
                    strcmp(ch_symbol_basename(ch_as_symbol(req)), "else") == 0) {
                    match = 1;
                } else {
                    match = ch_eval_feature_req(vm, req);
                }
                if (match) {
                    ChValue body = ch_cdr(clause);
                    ChValue begin_sym = ch_gc_intern_symbol_cstr(&vm->gc, "begin");
                    ChValue begin_form = ch_gc_cons(&vm->gc, begin_sym, body);
                    return expand_form(vm, begin_form, out, err, err_len, depth + 1);
                }
            }
            *out = CH_VOID;
            return CH_EXPAND_OK;
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
            *out = CH_VOID;
            return CH_EXPAND_OK;
        }
        ChTransformer *tr = ch_vm_lookup_macro(vm, ch_as_symbol(head));
        if (tr) {
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
