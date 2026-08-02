#include "chaaya/expander.h"

#include "expander_internal.h"

#include "chaaya/eval.h"
#include "chaaya/features.h"
#include "chaaya/library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int is_well_known(const char *base) {
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

void capture_transformer_templates(ChVM *vm, ChTransformer *tr) {
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

static ChValue list1(ChGC *gc, ChValue a) {
    return ch_gc_cons(gc, a, CH_NIL);
}

/* list2/list3/list4 build their tail via a nested allocating call, so the
 * leading argument(s) must be rooted first: ch_gc_cons only protects the two
 * arguments passed directly to it, not values still waiting in an outer
 * frame while a nested list{1,2,3} call runs (and may trigger a minor GC). */
static ChValue list2(ChGC *gc, ChValue a, ChValue b) {
    ch_gc_push(gc, &a);
    ChValue rest = list1(gc, b);
    ch_gc_pop(gc);
    return ch_gc_cons(gc, a, rest);
}

static ChValue list3(ChGC *gc, ChValue a, ChValue b, ChValue c) {
    ch_gc_push(gc, &a);
    ChValue rest = list2(gc, b, c);
    ch_gc_pop(gc);
    return ch_gc_cons(gc, a, rest);
}

static ChValue list4(ChGC *gc, ChValue a, ChValue b, ChValue c, ChValue d) {
    ch_gc_push(gc, &a);
    ChValue rest = list3(gc, b, c, d);
    ch_gc_pop(gc);
    return ch_gc_cons(gc, a, rest);
}

static ChValue append_one(ChGC *gc, ChValue list, ChValue item) {
    if (ch_is_nil(list)) {
        return list1(gc, item);
    }
    ChValue list_root = list;
    ChValue head = CH_NIL;
    ChValue tail = CH_NIL;
    ch_gc_push(gc, &list_root);
    ch_gc_push(gc, &head);
    ch_gc_push(gc, &tail);
    ch_gc_push(gc, &item);
    for (ChValue p = list_root; ch_is_pair(p); p = ch_cdr(p)) {
        ChValue cell = ch_gc_cons(gc, ch_car(p), CH_NIL);
        if (ch_is_nil(head)) {
            head = cell;
            tail = cell;
        } else {
            ch_set_cdr(tail, cell);
            ch_gc_write_barrier(gc, ch_to_object(tail), cell);
            tail = cell;
        }
    }
    ChValue cell = ch_gc_cons(gc, item, CH_NIL);
    if (ch_is_nil(head)) {
        head = cell;
    } else {
        ch_set_cdr(tail, cell);
        ch_gc_write_barrier(gc, ch_to_object(tail), cell);
    }
    ch_gc_pop_n(gc, 4);
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
    if (snprintf(iname, sizeof(iname), " __record_type_%s", type_name) >= (int)sizeof(iname)) {
        snprintf(err, err_len, "define-record-type: type name too long");
        return CH_EXPAND_ERROR;
    }

    uint16_t parent_n = 0;
    ChValue parent_rt_name = CH_FALSE;
    if (parent_sym) {
        char pname[256];
        if (snprintf(pname, sizeof(pname), " __record_type_%s", parent_sym->name) >=
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
        ch_gc_push(gc, &body);
        ChValue lam = list3(gc, lambda_sym, params, body);
        ch_gc_push(gc, &lam);
        ChValue let_expr = list3(gc, let_sym, list1(gc, list2(gc, rt_local, rt_name)), lam);
        forms = append_one(gc, forms, list3(gc, define_sym, ch_make_pointer(&ctor_sym->header), let_expr));
        ch_gc_pop_n(gc, 4); /* body, lam, params, body_args */
    }

    /* predicate */
    {
        ChValue v_sym = ch_gc_intern_symbol_cstr(gc, "v");
        ch_gc_push(gc, &v_sym);
        ChValue body = list3(gc, rp_sym, v_sym, rt_local);
        ch_gc_push(gc, &body);
        ChValue lam = list3(gc, lambda_sym, list1(gc, v_sym), body);
        ch_gc_push(gc, &lam);
        ChValue let_expr = list3(gc, let_sym, list1(gc, list2(gc, rt_local, rt_name)), lam);
        forms = append_one(gc, forms, list3(gc, define_sym, ch_make_pointer(&pred_sym->header), let_expr));
        ch_gc_pop_n(gc, 3); /* v_sym, body, lam */
    }

    for (size_t fi = 0; fi < nfields; fi++) {
        ChValue p_sym = ch_gc_intern_symbol_cstr(gc, "p");
        ch_gc_push(gc, &p_sym);
        ChValue idx = ch_make_fixnum((int64_t)parent_n + (int64_t)fi);
        ch_gc_push(gc, &idx);
        ChValue body = list4(gc, rr_sym, p_sym, idx, rt_local);
        ch_gc_push(gc, &body);
        ChValue lam = list3(gc, lambda_sym, list1(gc, p_sym), body);
        ch_gc_push(gc, &lam);
        ChValue let_expr = list3(gc, let_sym, list1(gc, list2(gc, rt_local, rt_name)), lam);
        forms = append_one(gc, forms, list3(gc, define_sym, ch_make_pointer(&acc_syms[fi]->header), let_expr));
        if (mut_syms[fi]) {
            ChValue v_sym = ch_gc_intern_symbol_cstr(gc, "v");
            ch_gc_push(gc, &v_sym);
            ChValue set_body = list1(gc, p_sym);
            ch_gc_push(gc, &set_body);
            set_body = append_one(gc, set_body, idx);
            set_body = append_one(gc, set_body, v_sym);
            set_body = append_one(gc, set_body, rt_local);
            set_body = ch_gc_cons(gc, rs_sym, set_body);
            ChValue set_lam = list3(gc, lambda_sym, list2(gc, p_sym, v_sym), set_body);
            ch_gc_push(gc, &set_lam);
            ChValue set_let = list3(gc, let_sym, list1(gc, list2(gc, rt_local, rt_name)), set_lam);
            forms = append_one(gc, forms, list3(gc, define_sym, ch_make_pointer(&mut_syms[fi]->header), set_let));
            ch_gc_pop_n(gc, 3); /* v_sym, set_body, set_lam */
        }
        ch_gc_pop_n(gc, 4); /* p_sym, idx, body, lam */
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
    if (snprintf(iname, sizeof(iname), " __record_type_%s", type_sym->name) >= (int)sizeof(iname)) {
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
        ch_gc_push(gc, &body);
        ChValue lam = list3(gc, lambda_sym, params, body);
        ch_gc_push(gc, &lam);
        ChValue binding = list2(gc, rt_local, rt_name);
        ch_gc_push(gc, &binding);
        ChValue bindings = list1(gc, binding);
        ch_gc_push(gc, &bindings);
        ChValue let_expr = list3(gc, let_sym, bindings, lam);
        ch_gc_push(gc, &let_expr);
        ChValue ctor_name = ch_make_pointer(&ctor_sym->header);
        ChValue def = list3(gc, define_sym, ctor_name, let_expr);
        forms = append_one(gc, forms, def);
        ch_gc_pop_n(gc, 6); /* body, lam, binding, bindings, let_expr, body_args */
    }

    /* predicate */
    {
        ChValue v_sym = ch_gc_intern_symbol_cstr(gc, "v");
        ch_gc_push(gc, &v_sym);
        ChValue body = list3(gc, rp_sym, v_sym, rt_local);
        ch_gc_push(gc, &body);
        ChValue lam = list3(gc, lambda_sym, list1(gc, v_sym), body);
        ch_gc_push(gc, &lam);
        ChValue binding = list2(gc, rt_local, rt_name);
        ch_gc_push(gc, &binding);
        ChValue let_expr = list3(gc, let_sym, list1(gc, binding), lam);
        ch_gc_push(gc, &let_expr);
        ChValue def = list3(gc, define_sym, ch_make_pointer(&pred_sym->header), let_expr);
        forms = append_one(gc, forms, def);
        ch_gc_pop_n(gc, 5); /* v_sym, body, lam, binding, let_expr */
    }

    for (size_t fi = 0; fi < nfields; fi++) {
        ChValue p_sym = ch_gc_intern_symbol_cstr(gc, "p");
        ch_gc_push(gc, &p_sym);
        ChValue idx = ch_make_fixnum((int64_t)fi);
        ch_gc_push(gc, &idx);
        ChValue body = list4(gc, rr_sym, p_sym, idx, rt_local);
        ch_gc_push(gc, &body);
        ChValue lam = list3(gc, lambda_sym, list1(gc, p_sym), body);
        ch_gc_push(gc, &lam);
        ChValue binding = list2(gc, rt_local, rt_name);
        ch_gc_push(gc, &binding);
        ChValue let_expr = list3(gc, let_sym, list1(gc, binding), lam);
        ch_gc_push(gc, &let_expr);
        ChValue def = list3(gc, define_sym, ch_make_pointer(&acc_syms[fi]->header), let_expr);
        forms = append_one(gc, forms, def);

        if (mut_syms[fi]) {
            ChValue v_sym = ch_gc_intern_symbol_cstr(gc, "v");
            ch_gc_push(gc, &v_sym);
            ChValue set_body = list1(gc, p_sym);
            ch_gc_push(gc, &set_body);
            set_body = append_one(gc, set_body, idx);
            set_body = append_one(gc, set_body, v_sym);
            set_body = append_one(gc, set_body, rt_local);
            set_body = ch_gc_cons(gc, rs_sym, set_body);
            ChValue params = list2(gc, p_sym, v_sym);
            ch_gc_push(gc, &params);
            ChValue set_lam = list3(gc, lambda_sym, params, set_body);
            ch_gc_push(gc, &set_lam);
            ChValue set_let = list3(gc, let_sym, list1(gc, list2(gc, rt_local, rt_name)), set_lam);
            ch_gc_push(gc, &set_let);
            ChValue set_def =
                list3(gc, define_sym, ch_make_pointer(&mut_syms[fi]->header), set_let);
            forms = append_one(gc, forms, set_def);
            ch_gc_pop_n(gc, 5); /* v_sym, set_body, params, set_lam, set_let */
        }
        ch_gc_pop_n(gc, 6); /* p_sym, idx, body, lam, binding, let_expr */
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

ChExpandStatus expand_form_no_macros(ChVM *vm, ChValue expr, ChValue *out, char *err,
                                     size_t err_len, int depth) {
    bool prev = vm->suppress_macro_expand;
    vm->suppress_macro_expand = true;
    ChExpandStatus st = expand_form(vm, expr, out, err, err_len, depth);
    vm->suppress_macro_expand = prev;
    return st;
}

ChExpandStatus expand_form(ChVM *vm, ChValue expr, ChValue *out, char *err, size_t err_len,
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
            ChExpandStatus st =
                expand_form_no_macros(vm, expanded, out, err, err_len, depth + 1);
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
            /* An environment created by (environment ...), (null-environment ...),
             * or (scheme-report-environment ...) is immutable (R7RS 6.14): eval'ing
             * a top-level define-syntax into it must error, not register the macro
             * in the VM's global table (which would leak it to every other user of
             * that name). Mirrors compile_define/compile_set's eval_env_immutable
             * check; active_eval_env is only set for the eval-environment argument,
             * never for ordinary library/body compilation, so local define-syntax
             * inside lambda/let bodies is unaffected. */
            if (vm->active_eval_env) {
                snprintf(err, err_len, "define-syntax: environment is not mutable");
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
        ChTransformer *tr =
            vm->suppress_macro_expand ? NULL : ch_vm_lookup_macro(vm, ch_as_symbol(head));
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
