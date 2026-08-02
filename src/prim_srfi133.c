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

static ChValue prim_vector_unfold(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 4) {
        snprintf(vm->error, sizeof(vm->error), "vector-unfold: expected at least 4 arguments");
        return CH_UNDEFINED;
    }
    int64_t len = 0;
    if (!ch_is_fixnum(args[0]) || ch_to_fixnum(args[0]) < 0) {
        snprintf(vm->error, sizeof(vm->error), "vector-unfold: expected non-negative length");
        return CH_UNDEFINED;
    }
    len = ch_to_fixnum(args[0]);
    ChValue out = ch_gc_make_vector(&vm->gc, (size_t)len, CH_FALSE);
    ch_gc_push(&vm->gc, &out);
    ChVector *vec = ch_as_vector(out);
    ChValue seed = args[3];
    ch_gc_push(&vm->gc, &seed);
    for (int64_t i = 0; i < len; i++) {
        ChValue stop = CH_FALSE;
        ChVMStatus st = ch_vm_apply(vm, args[1], &seed, 1, &stop);
        if (st != CH_VM_OK) {
            ch_gc_pop_n(&vm->gc, 2);
            return CH_UNDEFINED;
        }
        if (stop != CH_FALSE && !ch_is_false(stop)) {
            ch_gc_pop_n(&vm->gc, 2);
            snprintf(vm->error, sizeof(vm->error), "vector-unfold: stopped early");
            return CH_UNDEFINED;
        }
        ChValue elem = CH_UNDEFINED;
        st = ch_vm_apply(vm, args[2], &seed, 1, &elem);
        if (st != CH_VM_OK) {
            ch_gc_pop_n(&vm->gc, 2);
            return CH_UNDEFINED;
        }
        vec->items[(size_t)i] = elem;
        ChValue next = CH_UNDEFINED;
        st = ch_vm_apply(vm, args[4], &seed, 1, &next);
        if (st != CH_VM_OK) {
            ch_gc_pop_n(&vm->gc, 2);
            return CH_UNDEFINED;
        }
        seed = next;
    }
    ch_gc_pop_n(&vm->gc, 2);
    return out;
}

void ch_register_srfi133_primitives(ChVM *vm) {
    define_prim(vm, "vector-empty?", prim_vector_empty_p, 1, 1);
    define_prim(vm, "vector-index", prim_vector_index, -1, 2);
    define_prim(vm, "vector-index-right", prim_vector_index_right, -1, 2);
    define_prim(vm, "vector-skip", prim_vector_skip, -1, 2);
    define_prim(vm, "vector-skip-right", prim_vector_skip_right, -1, 2);
    define_prim(vm, "vector-count", prim_vector_count, -1, 2);
    define_prim(vm, "vector-fold", prim_vector_fold, -1, 3);
    define_prim(vm, "vector-reverse!", prim_vector_reverse_bang, -1, 1);
    define_prim(vm, "vector-unfold", prim_vector_unfold, -1, 4);
}
