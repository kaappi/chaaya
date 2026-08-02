#include "chaaya/prim.h"

#include "prim_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChValue prim_vector_length(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_vector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-length: not a vector");
        return CH_UNDEFINED;
    }
    return ch_make_fixnum((int64_t)ch_as_vector(args[0])->len);
}

static ChValue prim_vector_ref(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_vector(args[0]) || !ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "vector-ref: bad arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = ch_as_vector(args[0]);
    int64_t i = ch_to_fixnum(args[1]);
    if (i < 0 || (size_t)i >= v->len) {
        snprintf(vm->error, sizeof(vm->error), "vector-ref: index out of range");
        return CH_UNDEFINED;
    }
    return v->items[i];
}

static ChValue prim_vector_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_vector(args[0]) || !ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "vector-set!: bad arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = ch_as_vector(args[0]);
    if (ch_object_is_immutable(&v->header)) {
        snprintf(vm->error, sizeof(vm->error), "vector-set!: immutable vector");
        return CH_UNDEFINED;
    }
    int64_t i = ch_to_fixnum(args[1]);
    if (i < 0 || (size_t)i >= v->len) {
        snprintf(vm->error, sizeof(vm->error), "vector-set!: index out of range");
        return CH_UNDEFINED;
    }
    v->items[i] = args[2];
    ch_gc_write_barrier(&vm->gc, &v->header, args[2]);
    return CH_VOID;
}

static ChValue prim_vector_fill(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || nargs > 4 || !ch_is_vector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-fill!: expected vector fill [start [end]]");
        return CH_UNDEFINED;
    }
    ChVector *v = ch_as_vector(args[0]);
    if (ch_object_is_immutable(&v->header)) {
        snprintf(vm->error, sizeof(vm->error), "vector-fill!: immutable vector");
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = v->len;
    if (ch_parse_optional_range(vm, args, nargs, 2, v->len, "vector-fill!", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    for (size_t i = start; i < end; i++) {
        v->items[i] = args[1];
        ch_gc_write_barrier(&vm->gc, &v->header, args[1]);
    }
    return CH_VOID;
}

static ChValue prim_vector_copy(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || nargs > 3 || !ch_is_vector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-copy: expected vector [start [end]]");
        return CH_UNDEFINED;
    }
    ChVector *src = ch_as_vector(args[0]);
    size_t start = 0;
    size_t end = src->len;
    if (ch_parse_optional_range(vm, args, nargs, 1, src->len, "vector-copy", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t count = end - start;
    ChValue out = ch_gc_make_vector(&vm->gc, count, CH_FALSE);
    ChVector *dst = ch_as_vector(out);
    for (size_t i = 0; i < count; i++) {
        dst->items[i] = src->items[start + i];
    }
    return out;
}

static ChValue prim_vector_append(ChVM *vm, ChValue *args, int nargs) {
    size_t total = 0;
    for (int i = 0; i < nargs; i++) {
        if (!ch_is_vector(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "vector-append: not a vector");
            return CH_UNDEFINED;
        }
        total += ch_as_vector(args[i])->len;
    }
    ChValue out = ch_gc_make_vector(&vm->gc, total, CH_FALSE);
    ChVector *dst = ch_as_vector(out);
    size_t pos = 0;
    for (int i = 0; i < nargs; i++) {
        ChVector *src = ch_as_vector(args[i]);
        for (size_t j = 0; j < src->len; j++) {
            dst->items[pos++] = src->items[j];
        }
    }
    return out;
}

static ChValue prim_vector_map(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-map: expected procedure and at least one vector");
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-map: not a procedure");
        return CH_UNDEFINED;
    }
    int vec_count = nargs - 1;
    if (vec_count > 64) {
        snprintf(vm->error, sizeof(vm->error), "vector-map: too many vector arguments");
        return CH_UNDEFINED;
    }
    ChVector *vecs[64];
    size_t len = 0;
    for (int i = 0; i < vec_count; i++) {
        if (!ch_is_vector(args[i + 1])) {
            snprintf(vm->error, sizeof(vm->error), "vector-map: not a vector");
            return CH_UNDEFINED;
        }
        vecs[i] = ch_as_vector(args[i + 1]);
        if (i == 0 || vecs[i]->len < len) {
            len = vecs[i]->len;
        }
    }
    ChValue out = ch_gc_make_vector(&vm->gc, len, CH_FALSE);
    ch_gc_push(&vm->gc, &out);
    ChVector *dst = ch_as_vector(out);
    ChValue call_args[64];
    for (size_t i = 0; i < len; i++) {
        for (int j = 0; j < vec_count; j++) {
            call_args[j] = vecs[j]->items[i];
        }
        ChValue mapped = CH_UNDEFINED;
        ChVMStatus st = ch_vm_apply(vm, args[0], call_args, vec_count, &mapped);
        if (st == CH_VM_CONTINUATION_INVOKED) {
            vm->continuation_invoked = true;
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        if (st != CH_VM_OK) {
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        dst->items[i] = ch_coerce_single(mapped);
    }
    ch_gc_pop(&vm->gc);
    return out;
}

static ChValue prim_vector_for_each(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-for-each: expected procedure and at least one vector");
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-for-each: not a procedure");
        return CH_UNDEFINED;
    }
    int vec_count = nargs - 1;
    if (vec_count > 64) {
        snprintf(vm->error, sizeof(vm->error), "vector-for-each: too many vector arguments");
        return CH_UNDEFINED;
    }
    ChVector *vecs[64];
    size_t len = 0;
    for (int i = 0; i < vec_count; i++) {
        if (!ch_is_vector(args[i + 1])) {
            snprintf(vm->error, sizeof(vm->error), "vector-for-each: not a vector");
            return CH_UNDEFINED;
        }
        vecs[i] = ch_as_vector(args[i + 1]);
        if (i == 0 || vecs[i]->len < len) {
            len = vecs[i]->len;
        }
    }
    ChValue call_args[64];
    for (size_t i = 0; i < len; i++) {
        for (int j = 0; j < vec_count; j++) {
            call_args[j] = vecs[j]->items[i];
        }
        ChValue r = CH_VOID;
        ChVMStatus st = ch_vm_apply(vm, args[0], call_args, vec_count, &r);
        if (st == CH_VM_CONTINUATION_INVOKED) {
            vm->continuation_invoked = true;
            return CH_UNDEFINED;
        }
        if (st != CH_VM_OK) {
            return CH_UNDEFINED;
        }
    }
    return CH_VOID;
}

static ChValue prim_make_vector(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_fixnum(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "make-vector: bad length");
        return CH_UNDEFINED;
    }
    int64_t n = ch_to_fixnum(args[0]);
    if (n < 0 || n > 1000000) {
        snprintf(vm->error, sizeof(vm->error), "make-vector: bad length");
        return CH_UNDEFINED;
    }
    ChValue fill = nargs >= 2 ? args[1] : CH_FALSE;
    return ch_gc_make_vector(&vm->gc, (size_t)n, fill);
}

static ChValue prim_vector_to_list(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || nargs > 3) {
        snprintf(vm->error, sizeof(vm->error), "vector->list: expected 1 to 3 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_vector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector->list: not a vector");
        return CH_UNDEFINED;
    }
    ChVector *v = ch_as_vector(args[0]);
    size_t start = 0;
    size_t end = v->len;
    if (ch_parse_optional_range(vm, args, nargs, 1, v->len, "vector->list", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (size_t i = end; i > start; i--) {
        ChValue item = v->items[i - 1];
        ch_gc_push(&vm->gc, &item);
        list = ch_gc_cons(&vm->gc, item, list);
        ch_gc_pop(&vm->gc);
    }
    ch_gc_pop(&vm->gc);
    return list;
}

static ChValue prim_list_to_vector(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue lst = args[0];
    size_t n = 0;
    for (ChValue p = lst; ch_is_pair(p); p = ch_cdr(p)) {
        n++;
    }
    ChValue vec = ch_gc_make_vector(&vm->gc, n, CH_FALSE);
    ChVector *v = ch_as_vector(vec);
    size_t i = 0;
    for (ChValue p = lst; ch_is_pair(p); p = ch_cdr(p)) {
        v->items[i++] = ch_car(p);
    }
    return vec;
}

static ChValue prim_vector(ChVM *vm, ChValue *args, int nargs) {
    ChValue vec = ch_gc_make_vector(&vm->gc, (size_t)nargs, CH_FALSE);
    ChVector *v = ch_as_vector(vec);
    for (int i = 0; i < nargs; i++) {
        v->items[i] = args[i];
        ch_gc_write_barrier(&vm->gc, &v->header, args[i]);
    }
    return vec;
}

static ChValue prim_vector_copy_bang(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 3 || !ch_is_vector(args[0]) || !ch_is_vector(args[2])) {
        snprintf(vm->error, sizeof(vm->error), "vector-copy!: expected to at from [start [end]]");
        return CH_UNDEFINED;
    }
    ChVector *to = ch_as_vector(args[0]);
    if (ch_object_is_immutable(&to->header)) {
        snprintf(vm->error, sizeof(vm->error), "vector-copy!: immutable vector");
        return CH_UNDEFINED;
    }
    size_t at = 0;
    if (ch_parse_nonnegative_index(vm, args[1], &at, "vector-copy!") != 0) {
        return CH_UNDEFINED;
    }
    ChVector *from = ch_as_vector(args[2]);
    size_t start = 0;
    size_t end = from->len;
    if (ch_parse_optional_range(vm, args, nargs, 3, from->len, "vector-copy!", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t count = end - start;
    if (at + count > to->len) {
        snprintf(vm->error, sizeof(vm->error), "vector-copy!: range out of bounds");
        return CH_UNDEFINED;
    }
    if (to == from && at > start) {
        for (size_t i = count; i > 0; i--) {
            to->items[at + i - 1] = from->items[start + i - 1];
            ch_gc_write_barrier(&vm->gc, &to->header, to->items[at + i - 1]);
        }
    } else {
        for (size_t i = 0; i < count; i++) {
            to->items[at + i] = from->items[start + i];
            ch_gc_write_barrier(&vm->gc, &to->header, to->items[at + i]);
        }
    }
    return CH_VOID;
}

static ChValue prim_string_to_vector(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string->vector: not a string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t count = 0;
    if (ch_utf8_count_codepoints(vm, s, "string->vector", &count) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = count;
    if (ch_parse_optional_range(vm, args, nargs, 1, count, "string->vector", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t n = end - start;
    ChValue vec = ch_gc_make_vector(&vm->gc, n, CH_FALSE);
    ChVector *v = ch_as_vector(vec);
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = 0;
        if (ch_utf8_find_codepoint(vm, s, start + i, "string->vector", NULL, NULL, &cp) != 0) {
            return CH_UNDEFINED;
        }
        v->items[i] = ch_make_char(cp);
    }
    return vec;
}

static ChValue prim_vector_to_string(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_vector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector->string: not a vector");
        return CH_UNDEFINED;
    }
    ChVector *v = ch_as_vector(args[0]);
    size_t start = 0;
    size_t end = v->len;
    if (ch_parse_optional_range(vm, args, nargs, 1, v->len, "vector->string", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    for (size_t i = start; i < end; i++) {
        if (!ch_is_char(v->items[i])) {
            free(buf);
            snprintf(vm->error, sizeof(vm->error), "vector->string: not a char");
            return CH_UNDEFINED;
        }
        if (!ch_utf8_append_codepoint(&buf, &len, &cap, ch_to_char(v->items[i]))) {
            free(buf);
            snprintf(vm->error, sizeof(vm->error), "vector->string: invalid character");
            return CH_UNDEFINED;
        }
    }
    if (buf) {
        buf[len] = '\0';
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", len);
    free(buf);
    return out;
}

void ch_register_vector_primitives(ChVM *vm) {
    define_prim(vm, "vector", prim_vector, -1, 0);
    define_prim(vm, "vector-length", prim_vector_length, 1, 1);
    define_prim(vm, "vector-ref", prim_vector_ref, 2, 2);
    define_prim(vm, "vector-set!", prim_vector_set, 3, 3);
    define_prim(vm, "vector-fill!", prim_vector_fill, -1, 2);
    define_prim(vm, "vector-copy", prim_vector_copy, -1, 1);
    define_prim(vm, "vector-copy!", prim_vector_copy_bang, -1, 3);
    define_prim(vm, "vector-append", prim_vector_append, -1, 0);
    define_prim(vm, "vector-map", prim_vector_map, -1, 2);
    define_prim(vm, "vector-for-each", prim_vector_for_each, -1, 2);
    define_prim(vm, "make-vector", prim_make_vector, -1, 1);
    define_prim(vm, "vector->list", prim_vector_to_list, -1, 1);
    define_prim(vm, "list->vector", prim_list_to_vector, 1, 1);
    define_prim(vm, "string->vector", prim_string_to_vector, -1, 1);
    define_prim(vm, "vector->string", prim_vector_to_string, -1, 1);
}
