#include "chaaya/prim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static int parse_nonneg(ChVM *vm, ChValue v, size_t *out, const char *who) {
    if (!ch_is_fixnum(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected exact integer", who);
        return -1;
    }
    int64_t n = ch_to_fixnum(v);
    if (n < 0) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected non-negative integer", who);
        return -1;
    }
    *out = (size_t)n;
    return 0;
}

static int parse_optional_range(ChVM *vm, ChValue *args, int nargs, int start_arg, size_t len,
                                const char *who, size_t *start_out, size_t *end_out) {
    size_t start = 0;
    size_t end = len;
    if (nargs > start_arg) {
        if (parse_nonneg(vm, args[start_arg], &start, who) != 0) {
            return -1;
        }
    }
    if (nargs > start_arg + 1) {
        if (parse_nonneg(vm, args[start_arg + 1], &end, who) != 0) {
            return -1;
        }
    }
    if (start > end || end > len) {
        snprintf(vm->error, sizeof(vm->error), "%s: range out of bounds", who);
        return -1;
    }
    *start_out = start;
    *end_out = end;
    return 0;
}

static ChVector *require_vector(ChVM *vm, ChValue v, const char *who) {
    if (!ch_is_vector(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected vector", who);
        return NULL;
    }
    return ch_as_vector(v);
}

static bool call_pred(ChVM *vm, ChValue proc, ChValue arg, bool *out) {
    ChValue result = CH_FALSE;
    ChVMStatus st = ch_vm_apply(vm, proc, &arg, 1, &result);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return false;
    }
    if (st != CH_VM_OK) {
        return false;
    }
    *out = result != CH_FALSE && !ch_is_false(result);
    return true;
}

static ChValue prim_vector_empty_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChVector *v = require_vector(vm, args[0], "vector-empty?");
    if (!v) {
        return CH_UNDEFINED;
    }
    return v->len == 0 ? CH_TRUE : CH_FALSE;
}

static ChValue prim_vector_index(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-index: bad arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = require_vector(vm, args[1], "vector-index");
    if (!v || !ch_is_procedure(args[0])) {
        if (v) {
            snprintf(vm->error, sizeof(vm->error), "vector-index: expected procedure");
        }
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = v->len;
    if (parse_optional_range(vm, args, nargs, 2, v->len, "vector-index", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    for (size_t i = start; i < end; i++) {
        bool match = false;
        if (!call_pred(vm, args[0], v->items[i], &match)) {
            return CH_UNDEFINED;
        }
        if (match) {
            return ch_make_fixnum((int64_t)i);
        }
    }
    return CH_FALSE;
}

static ChValue prim_vector_index_right(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-index-right: bad arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = require_vector(vm, args[1], "vector-index-right");
    if (!v || !ch_is_procedure(args[0])) {
        if (v) {
            snprintf(vm->error, sizeof(vm->error), "vector-index-right: expected procedure");
        }
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = v->len;
    if (parse_optional_range(vm, args, nargs, 2, v->len, "vector-index-right", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    ssize_t last = -1;
    for (size_t i = start; i < end; i++) {
        bool match = false;
        if (!call_pred(vm, args[0], v->items[i], &match)) {
            return CH_UNDEFINED;
        }
        if (match) {
            last = (ssize_t)i;
        }
    }
    return last >= 0 ? ch_make_fixnum(last) : CH_FALSE;
}

static ChValue prim_vector_skip(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-skip: bad arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = require_vector(vm, args[1], "vector-skip");
    if (!v || !ch_is_procedure(args[0])) {
        if (v) {
            snprintf(vm->error, sizeof(vm->error), "vector-skip: expected procedure");
        }
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = v->len;
    if (parse_optional_range(vm, args, nargs, 2, v->len, "vector-skip", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    for (size_t i = start; i < end; i++) {
        bool match = false;
        if (!call_pred(vm, args[0], v->items[i], &match)) {
            return CH_UNDEFINED;
        }
        if (!match) {
            return ch_make_fixnum((int64_t)i);
        }
    }
    return CH_FALSE;
}

static ChValue prim_vector_skip_right(ChVM *vm, ChValue *args, int nargs) {
    return prim_vector_index_right(vm, args, nargs);
}

static ChValue prim_vector_count(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-count: bad arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = require_vector(vm, args[1], "vector-count");
    if (!v || !ch_is_procedure(args[0])) {
        if (v) {
            snprintf(vm->error, sizeof(vm->error), "vector-count: expected procedure");
        }
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = v->len;
    if (parse_optional_range(vm, args, nargs, 2, v->len, "vector-count", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    int64_t count = 0;
    for (size_t i = start; i < end; i++) {
        bool match = false;
        if (!call_pred(vm, args[0], v->items[i], &match)) {
            return CH_UNDEFINED;
        }
        if (match) {
            count++;
        }
    }
    return ch_make_fixnum(count);
}

static ChValue prim_vector_cumulate(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 3) {
        snprintf(vm->error, sizeof(vm->error), "vector-cumulate: expected at least 3 arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = require_vector(vm, args[2], "vector-cumulate");
    if (!v || !ch_is_procedure(args[0])) {
        if (v) {
            snprintf(vm->error, sizeof(vm->error), "vector-cumulate: expected procedure");
        }
        return CH_UNDEFINED;
    }
    ChValue out = ch_gc_make_vector(&vm->gc, v->len, CH_FALSE);
    ch_gc_push(&vm->gc, &out);
    ChVector *vec = ch_as_vector(out);
    ChValue acc = args[1];
    ch_gc_push(&vm->gc, &acc);
    for (size_t i = 0; i < v->len; i++) {
        ChValue call_args[2] = {acc, v->items[i]};
        ChValue next = CH_UNDEFINED;
        ChVMStatus st = ch_vm_apply(vm, args[0], call_args, 2, &next);
        if (st != CH_VM_OK) {
            ch_gc_pop_n(&vm->gc, 2);
            return CH_UNDEFINED;
        }
        acc = ch_coerce_single(next);
        vec->items[i] = acc;
    }
    ch_gc_pop_n(&vm->gc, 2);
    return out;
}

static ChValue prim_vector_fold(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 3) {
        snprintf(vm->error, sizeof(vm->error), "vector-fold: expected at least 3 arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = require_vector(vm, args[2], "vector-fold");
    if (!v || !ch_is_procedure(args[0])) {
        if (v) {
            snprintf(vm->error, sizeof(vm->error), "vector-fold: expected procedure");
        }
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = v->len;
    if (parse_optional_range(vm, args, nargs, 3, v->len, "vector-fold", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    ChValue acc = args[1];
    ch_gc_push(&vm->gc, &acc);
    for (size_t i = start; i < end; i++) {
        ChValue call_args[2] = {v->items[i], acc};
        ChValue next = CH_UNDEFINED;
        ChVMStatus st = ch_vm_apply(vm, args[0], call_args, 2, &next);
        if (st != CH_VM_OK) {
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        acc = ch_coerce_single(next);
    }
    ch_gc_pop(&vm->gc);
    return acc;
}

static ChValue prim_vector_fold_right(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 3) {
        snprintf(vm->error, sizeof(vm->error), "vector-fold-right: expected at least 3 arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = require_vector(vm, args[2], "vector-fold-right");
    if (!v || !ch_is_procedure(args[0])) {
        if (v) {
            snprintf(vm->error, sizeof(vm->error), "vector-fold-right: expected procedure");
        }
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = v->len;
    if (parse_optional_range(vm, args, nargs, 3, v->len, "vector-fold-right", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    ChValue acc = args[1];
    ch_gc_push(&vm->gc, &acc);
    for (size_t ii = end; ii > start; ii--) {
        size_t i = ii - 1;
        ChValue call_args[2] = {v->items[i], acc};
        ChValue next = CH_UNDEFINED;
        ChVMStatus st = ch_vm_apply(vm, args[0], call_args, 2, &next);
        if (st != CH_VM_OK) {
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        acc = ch_coerce_single(next);
    }
    ch_gc_pop(&vm->gc);
    return acc;
}

static ChValue prim_vector_reverse_bang(ChVM *vm, ChValue *args, int nargs) {
    ChVector *v = require_vector(vm, args[0], "vector-reverse!");
    if (!v) {
        return CH_UNDEFINED;
    }
    if (ch_object_is_immutable(&v->header)) {
        snprintf(vm->error, sizeof(vm->error), "vector-reverse!: immutable vector");
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = v->len;
    if (parse_optional_range(vm, args, nargs, 1, v->len, "vector-reverse!", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t lo = start;
    size_t hi = end;
    while (lo < hi) {
        hi--;
        ChValue tmp = v->items[lo];
        v->items[lo] = v->items[hi];
        v->items[hi] = tmp;
        lo++;
    }
    return CH_VOID;
}

static ChValue apply_unfold_step(ChVM *vm, ChValue proc, ChValue *call_args, int ncall,
                                  ChValue *elem_out, ChValue *seeds, int nseeds) {
    ChValue result = CH_UNDEFINED;
    ChVMStatus st = ch_vm_apply(vm, proc, call_args, ncall, &result);
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    if (ch_is_values(result)) {
        ChValues *vs = ch_as_values(result);
        if (vs->count == 0) {
            snprintf(vm->error, sizeof(vm->error), "vector-unfold: empty values from step procedure");
            return CH_UNDEFINED;
        }
        *elem_out = vs->items[0];
        for (int j = 0; j < nseeds; j++) {
            if ((size_t)(j + 1) < vs->count) {
                seeds[j] = vs->items[j + 1];
            }
        }
        return result;
    }
    *elem_out = result;
    return result;
}

static ChValue prim_vector_unfold(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-unfold: expected at least 2 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_fixnum(args[1]) || ch_to_fixnum(args[1]) < 0) {
        snprintf(vm->error, sizeof(vm->error), "vector-unfold: expected non-negative length");
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-unfold: expected procedure");
        return CH_UNDEFINED;
    }
    size_t len = (size_t)ch_to_fixnum(args[1]);
    int nseeds = nargs - 2;
    ChValue out = ch_gc_make_vector(&vm->gc, len, CH_FALSE);
    ch_gc_push(&vm->gc, &out);
    ChVector *vec = ch_as_vector(out);
    ChValue seeds[64];
    if (nseeds > 64) {
        ch_gc_pop(&vm->gc);
        snprintf(vm->error, sizeof(vm->error), "vector-unfold: too many seeds");
        return CH_UNDEFINED;
    }
    for (int i = 0; i < nseeds; i++) {
        seeds[i] = args[2 + i];
        ch_gc_push(&vm->gc, &seeds[i]);
    }
    ChValue call_args[65];
    for (size_t i = 0; i < len; i++) {
        call_args[0] = ch_make_fixnum((int64_t)i);
        for (int j = 0; j < nseeds; j++) {
            call_args[1 + j] = seeds[j];
        }
        ChValue elem = CH_UNDEFINED;
        if (apply_unfold_step(vm, args[0], call_args, 1 + nseeds, &elem, seeds, nseeds) ==
            CH_UNDEFINED) {
            ch_gc_pop_n(&vm->gc, 1 + (size_t)nseeds);
            return CH_UNDEFINED;
        }
        vec->items[i] = elem;
        ch_gc_push(&vm->gc, &elem);
        for (int j = 0; j < nseeds; j++) {
            ch_gc_push(&vm->gc, &seeds[j]);
        }
    }
    ch_gc_pop_n(&vm->gc, 1 + (size_t)nseeds);
    return out;
}

static ChValue prim_vector_partition(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-partition: expected at least 2 arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = require_vector(vm, args[1], "vector-partition");
    if (!v || !ch_is_procedure(args[0])) {
        if (v) {
            snprintf(vm->error, sizeof(vm->error), "vector-partition: expected procedure");
        }
        return CH_UNDEFINED;
    }
    ChValue yes_buf = ch_gc_make_vector(&vm->gc, v->len, CH_FALSE);
    ChValue no_buf = ch_gc_make_vector(&vm->gc, v->len, CH_FALSE);
    ch_gc_push(&vm->gc, &yes_buf);
    ch_gc_push(&vm->gc, &no_buf);
    ChVector *yes = ch_as_vector(yes_buf);
    ChVector *no = ch_as_vector(no_buf);
    size_t yes_n = 0;
    size_t no_n = 0;
    for (size_t i = 0; i < v->len; i++) {
        ChValue elem = v->items[i];
        bool match = false;
        if (!call_pred(vm, args[0], elem, &match)) {
            ch_gc_pop_n(&vm->gc, 2);
            return CH_UNDEFINED;
        }
        if (match) {
            yes->items[yes_n++] = elem;
        } else {
            no->items[no_n++] = elem;
        }
    }
    ChValue out = ch_gc_make_vector(&vm->gc, v->len, CH_FALSE);
    ch_gc_push(&vm->gc, &out);
    ChVector *result = ch_as_vector(out);
    for (size_t i = 0; i < yes_n; i++) {
        result->items[i] = yes->items[i];
    }
    for (size_t i = 0; i < no_n; i++) {
        result->items[yes_n + i] = no->items[i];
    }
    ChValue count = ch_make_fixnum((int64_t)yes_n);
    ChValue vals[2] = {out, count};
    ch_gc_pop_n(&vm->gc, 3);
    return ch_gc_make_values(&vm->gc, vals, 2);
}

static ChValue prim_vector_unfold_right(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-unfold-right: expected at least 2 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_fixnum(args[1]) || ch_to_fixnum(args[1]) < 0) {
        snprintf(vm->error, sizeof(vm->error), "vector-unfold-right: expected non-negative length");
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-unfold-right: expected procedure");
        return CH_UNDEFINED;
    }
    size_t len = (size_t)ch_to_fixnum(args[1]);
    int nseeds = nargs - 2;
    ChValue out = ch_gc_make_vector(&vm->gc, len, CH_FALSE);
    ch_gc_push(&vm->gc, &out);
    ChVector *vec = ch_as_vector(out);
    ChValue seeds[64];
    if (nseeds > 64) {
        ch_gc_pop(&vm->gc);
        snprintf(vm->error, sizeof(vm->error), "vector-unfold-right: too many seeds");
        return CH_UNDEFINED;
    }
    for (int i = 0; i < nseeds; i++) {
        seeds[i] = args[2 + i];
        ch_gc_push(&vm->gc, &seeds[i]);
    }
    ChValue call_args[65];
    for (size_t ii = len; ii > 0; ii--) {
        size_t i = ii - 1;
        call_args[0] = ch_make_fixnum((int64_t)i);
        for (int j = 0; j < nseeds; j++) {
            call_args[1 + j] = seeds[j];
        }
        ChValue elem = CH_UNDEFINED;
        if (apply_unfold_step(vm, args[0], call_args, 1 + nseeds, &elem, seeds, nseeds) ==
            CH_UNDEFINED) {
            ch_gc_pop_n(&vm->gc, 1 + (size_t)nseeds);
            return CH_UNDEFINED;
        }
        vec->items[i] = elem;
        ch_gc_push(&vm->gc, &elem);
        for (int j = 0; j < nseeds; j++) {
            ch_gc_push(&vm->gc, &seeds[j]);
        }
    }
    ch_gc_pop_n(&vm->gc, 1 + (size_t)nseeds);
    return out;
}

static ChValue prim_vector_swap_bang(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChVector *v = require_vector(vm, args[0], "vector-swap!");
    if (!v) {
        return CH_UNDEFINED;
    }
    if (ch_object_is_immutable(&v->header)) {
        snprintf(vm->error, sizeof(vm->error), "vector-swap!: immutable vector");
        return CH_UNDEFINED;
    }
    if (!ch_is_fixnum(args[1]) || !ch_is_fixnum(args[2])) {
        snprintf(vm->error, sizeof(vm->error), "vector-swap!: expected integer indices");
        return CH_UNDEFINED;
    }
    int64_t i = ch_to_fixnum(args[1]);
    int64_t j = ch_to_fixnum(args[2]);
    if (i < 0 || j < 0 || (size_t)i >= v->len || (size_t)j >= v->len) {
        snprintf(vm->error, sizeof(vm->error), "vector-swap!: index out of range");
        return CH_UNDEFINED;
    }
    ChValue tmp = v->items[i];
    v->items[i] = v->items[j];
    v->items[j] = tmp;
    return CH_VOID;
}

static ChValue prim_vector_reverse_copy(ChVM *vm, ChValue *args, int nargs) {
    ChVector *v = require_vector(vm, args[0], "vector-reverse-copy");
    if (!v) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = v->len;
    if (parse_optional_range(vm, args, nargs, 1, v->len, "vector-reverse-copy", &start, &end) !=
        0) {
        return CH_UNDEFINED;
    }
    size_t n = end - start;
    ChValue out = ch_gc_make_vector(&vm->gc, n, CH_FALSE);
    ch_gc_push(&vm->gc, &out);
    ChVector *dst = ch_as_vector(out);
    for (size_t i = 0; i < n; i++) {
        dst->items[i] = v->items[end - 1 - i];
    }
    ch_gc_pop(&vm->gc);
    return out;
}

/* (vector-map! f vec1 vec2 ...) mutates vec1 in place with f applied
   element-wise, stopping at the shortest vector (SRFI-133). */
static ChValue prim_vector_map_bang(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-map!: expected at least 2 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-map!: expected procedure");
        return CH_UNDEFINED;
    }
    ChVector *v0 = require_vector(vm, args[1], "vector-map!");
    if (!v0) {
        return CH_UNDEFINED;
    }
    if (ch_object_is_immutable(&v0->header)) {
        snprintf(vm->error, sizeof(vm->error), "vector-map!: immutable vector");
        return CH_UNDEFINED;
    }
    size_t n = v0->len;
    int vec_count = nargs - 1;
    if (vec_count > 16) {
        snprintf(vm->error, sizeof(vm->error), "vector-map!: too many vectors");
        return CH_UNDEFINED;
    }
    for (int i = 1; i < vec_count; i++) {
        ChVector *vi = require_vector(vm, args[1 + i], "vector-map!");
        if (!vi) {
            return CH_UNDEFINED;
        }
        if (vi->len < n) {
            n = vi->len;
        }
    }
    for (size_t i = 0; i < n; i++) {
        ChValue call_args[16];
        for (int j = 0; j < vec_count; j++) {
            call_args[j] = ch_as_vector(args[1 + j])->items[i];
        }
        ChValue result = CH_UNDEFINED;
        ChVMStatus st = ch_vm_apply(vm, args[0], call_args, vec_count, &result);
        if (st != CH_VM_OK) {
            return CH_UNDEFINED;
        }
        v0->items[i] = ch_coerce_single(result);
    }
    return CH_VOID;
}

void ch_register_srfi133_primitives(ChVM *vm) {
    define_prim(vm, "vector-empty?", prim_vector_empty_p, 1, 1);
    define_prim(vm, "vector-index", prim_vector_index, -1, 2);
    define_prim(vm, "vector-index-right", prim_vector_index_right, -1, 2);
    define_prim(vm, "vector-skip", prim_vector_skip, -1, 2);
    define_prim(vm, "vector-skip-right", prim_vector_skip_right, -1, 2);
    define_prim(vm, "vector-count", prim_vector_count, -1, 2);
    define_prim(vm, "vector-fold", prim_vector_fold, -1, 3);
    define_prim(vm, "vector-fold-right", prim_vector_fold_right, -1, 3);
    define_prim(vm, "vector-cumulate", prim_vector_cumulate, -1, 3);
    define_prim(vm, "vector-reverse!", prim_vector_reverse_bang, -1, 1);
    define_prim(vm, "vector-swap!", prim_vector_swap_bang, 3, 3);
    define_prim(vm, "vector-reverse-copy", prim_vector_reverse_copy, -1, 1);
    define_prim(vm, "vector-unfold", prim_vector_unfold, -1, 2);
    define_prim(vm, "vector-unfold-right", prim_vector_unfold_right, -1, 2);
    define_prim(vm, "vector-partition", prim_vector_partition, 2, 2);
    define_prim(vm, "vector-map!", prim_vector_map_bang, -1, 2);
}
