#include "chaaya/prim.h"

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

static ChValue prim_values(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 1) {
        return args[0];
    }
    return ch_gc_make_values(&vm->gc, args, (size_t)nargs);
}

static ChValue prim_call_with_values(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue producer = args[0];
    ChValue consumer = args[1];
    if (!ch_is_procedure(producer) || !ch_is_procedure(consumer)) {
        snprintf(vm->error, sizeof(vm->error), "call-with-values: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue produced = CH_VOID;
    ChVMStatus st = ch_vm_apply(vm, producer, NULL, 0, &produced);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }

    ChValue result = CH_VOID;
    if (ch_is_values(produced)) {
        ChValues *vs = ch_as_values(produced);
        if (vs->count > 256) {
            snprintf(vm->error, sizeof(vm->error), "call-with-values: too many values");
            return CH_UNDEFINED;
        }
        st = ch_vm_apply(vm, consumer, vs->items, (int)vs->count, &result);
    } else {
        st = ch_vm_apply(vm, consumer, &produced, 1, &result);
    }
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    return ch_coerce_single(result);
}

static ChValue prim_char_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_char(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_char_eq(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "char=?: needs at least 2 arguments");
        return CH_UNDEFINED;
    }
    for (int i = 0; i < nargs; i++) {
        if (!ch_is_char(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "char=?: not a char");
            return CH_UNDEFINED;
        }
    }
    uint32_t c0 = ch_to_char(args[0]);
    for (int i = 1; i < nargs; i++) {
        if (ch_to_char(args[i]) != c0) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static ChValue prim_char_lt(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "char<?: needs at least 2 arguments");
        return CH_UNDEFINED;
    }
    for (int i = 0; i < nargs; i++) {
        if (!ch_is_char(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "char<?: not a char");
            return CH_UNDEFINED;
        }
    }
    for (int i = 1; i < nargs; i++) {
        if (!(ch_to_char(args[i - 1]) < ch_to_char(args[i]))) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static ChValue prim_char_to_integer(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "char->integer: not a char");
        return CH_UNDEFINED;
    }
    return ch_make_fixnum((int64_t)ch_to_char(args[0]));
}

static ChValue prim_integer_to_char(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_fixnum(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "integer->char: not an integer");
        return CH_UNDEFINED;
    }
    int64_t n = ch_to_fixnum(args[0]);
    if (n < 0 || n > 0x10FFFF) {
        snprintf(vm->error, sizeof(vm->error), "integer->char: out of range");
        return CH_UNDEFINED;
    }
    return ch_make_char((uint32_t)n);
}

static ChValue prim_abs(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_fixnum(args[0])) {
        int64_t n = ch_to_fixnum(args[0]);
        return ch_make_fixnum(n < 0 ? -n : n);
    }
    if (ch_is_flonum(args[0])) {
        double d = ch_to_flonum(args[0]);
        return ch_make_flonum(d < 0 ? -d : d);
    }
    snprintf(vm->error, sizeof(vm->error), "abs: not a number");
    return CH_UNDEFINED;
}

static ChValue prim_integer_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (ch_is_fixnum(args[0])) {
        return CH_TRUE;
    }
    if (ch_is_flonum(args[0])) {
        double d = ch_to_flonum(args[0]);
        return (d == (double)(int64_t)d) ? CH_TRUE : CH_FALSE;
    }
    return CH_FALSE;
}

static ChValue prim_zero_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_fixnum(args[0])) {
        return ch_to_fixnum(args[0]) == 0 ? CH_TRUE : CH_FALSE;
    }
    if (ch_is_flonum(args[0])) {
        return ch_to_flonum(args[0]) == 0.0 ? CH_TRUE : CH_FALSE;
    }
    snprintf(vm->error, sizeof(vm->error), "zero?: not a number");
    return CH_UNDEFINED;
}

static ChValue prim_string_length(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-length: not a string");
        return CH_UNDEFINED;
    }
    return ch_make_fixnum((int64_t)ch_as_string(args[0])->len);
}

static ChValue prim_string_ref(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0]) || !ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-ref: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    int64_t i = ch_to_fixnum(args[1]);
    if (i < 0 || (size_t)i >= s->len) {
        snprintf(vm->error, sizeof(vm->error), "string-ref: index out of range");
        return CH_UNDEFINED;
    }
    return ch_make_char((unsigned char)s->data[i]);
}

static ChValue prim_string_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0]) || !ch_is_fixnum(args[1]) || !ch_is_char(args[2])) {
        snprintf(vm->error, sizeof(vm->error), "string-set!: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    int64_t i = ch_to_fixnum(args[1]);
    if (i < 0 || (size_t)i >= s->len) {
        snprintf(vm->error, sizeof(vm->error), "string-set!: index out of range");
        return CH_UNDEFINED;
    }
    s->data[i] = (char)(ch_to_char(args[2]) & 0xFF);
    return CH_VOID;
}

static ChValue prim_make_string(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_fixnum(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "make-string: bad length");
        return CH_UNDEFINED;
    }
    int64_t n = ch_to_fixnum(args[0]);
    if (n < 0 || n > 1000000) {
        snprintf(vm->error, sizeof(vm->error), "make-string: bad length");
        return CH_UNDEFINED;
    }
    char fill = ' ';
    if (nargs >= 2) {
        if (!ch_is_char(args[1])) {
            snprintf(vm->error, sizeof(vm->error), "make-string: fill not a char");
            return CH_UNDEFINED;
        }
        fill = (char)(ch_to_char(args[1]) & 0xFF);
    }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        abort();
    }
    memset(buf, fill, (size_t)n);
    buf[n] = '\0';
    ChValue s = ch_gc_make_string(&vm->gc, buf, (size_t)n);
    free(buf);
    return s;
}

static ChValue prim_string_append(ChVM *vm, ChValue *args, int nargs) {
    size_t total = 0;
    for (int i = 0; i < nargs; i++) {
        if (!ch_is_string(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "string-append: not a string");
            return CH_UNDEFINED;
        }
        total += ch_as_string(args[i])->len;
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        abort();
    }
    size_t pos = 0;
    for (int i = 0; i < nargs; i++) {
        ChString *s = ch_as_string(args[i]);
        memcpy(buf + pos, s->data, s->len);
        pos += s->len;
    }
    buf[total] = '\0';
    ChValue out = ch_gc_make_string(&vm->gc, buf, total);
    free(buf);
    return out;
}

static ChValue prim_substring(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0]) || !ch_is_fixnum(args[1]) || !ch_is_fixnum(args[2])) {
        snprintf(vm->error, sizeof(vm->error), "substring: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    int64_t start = ch_to_fixnum(args[1]);
    int64_t end = ch_to_fixnum(args[2]);
    if (start < 0 || end < start || (size_t)end > s->len) {
        snprintf(vm->error, sizeof(vm->error), "substring: out of range");
        return CH_UNDEFINED;
    }
    return ch_gc_make_string(&vm->gc, s->data + start, (size_t)(end - start));
}

static ChValue prim_string_eq(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    if (nargs < 2) {
        return CH_TRUE;
    }
    if (!ch_is_string(args[0])) {
        return CH_FALSE;
    }
    ChString *a = ch_as_string(args[0]);
    for (int i = 1; i < nargs; i++) {
        if (!ch_is_string(args[i])) {
            return CH_FALSE;
        }
        ChString *b = ch_as_string(args[i]);
        if (a->len != b->len || memcmp(a->data, b->data, a->len) != 0) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static ChValue prim_symbol_to_string(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_symbol(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "symbol->string: not a symbol");
        return CH_UNDEFINED;
    }
    ChSymbol *s = ch_as_symbol(args[0]);
    return ch_gc_make_string(&vm->gc, s->name, s->len);
}

static ChValue prim_string_to_symbol(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string->symbol: not a string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    return ch_gc_intern_symbol(&vm->gc, s->data, s->len);
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
    int64_t i = ch_to_fixnum(args[1]);
    if (i < 0 || (size_t)i >= v->len) {
        snprintf(vm->error, sizeof(vm->error), "vector-set!: index out of range");
        return CH_UNDEFINED;
    }
    v->items[i] = args[2];
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
    (void)nargs;
    if (!ch_is_vector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector->list: not a vector");
        return CH_UNDEFINED;
    }
    ChVector *v = ch_as_vector(args[0]);
    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (size_t i = v->len; i > 0; i--) {
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

static ChValue prim_string_to_list(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string->list: not a string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (size_t i = s->len; i > 0; i--) {
        ChValue ch = ch_make_char((unsigned char)s->data[i - 1]);
        list = ch_gc_cons(&vm->gc, ch, list);
    }
    ch_gc_pop(&vm->gc);
    return list;
}

static ChValue prim_list_to_string(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    size_t n = 0;
    for (ChValue p = args[0]; ch_is_pair(p); p = ch_cdr(p)) {
        if (!ch_is_char(ch_car(p))) {
            snprintf(vm->error, sizeof(vm->error), "list->string: not a char");
            return CH_UNDEFINED;
        }
        n++;
    }
    char *buf = (char *)malloc(n + 1);
    if (!buf) {
        abort();
    }
    size_t i = 0;
    for (ChValue p = args[0]; ch_is_pair(p); p = ch_cdr(p)) {
        buf[i++] = (char)(ch_to_char(ch_car(p)) & 0xFF);
    }
    buf[n] = '\0';
    ChValue s = ch_gc_make_string(&vm->gc, buf, n);
    free(buf);
    return s;
}

void ch_register_data_primitives(ChVM *vm) {
    define_prim(vm, "values", prim_values, -1, 0);
    define_prim(vm, "call-with-values", prim_call_with_values, 2, 2);
    define_prim(vm, "char?", prim_char_p, 1, 1);
    define_prim(vm, "char=?", prim_char_eq, -1, 2);
    define_prim(vm, "char<?", prim_char_lt, -1, 2);
    define_prim(vm, "char->integer", prim_char_to_integer, 1, 1);
    define_prim(vm, "integer->char", prim_integer_to_char, 1, 1);
    define_prim(vm, "abs", prim_abs, 1, 1);
    define_prim(vm, "integer?", prim_integer_p, 1, 1);
    define_prim(vm, "zero?", prim_zero_p, 1, 1);
    define_prim(vm, "string-length", prim_string_length, 1, 1);
    define_prim(vm, "string-ref", prim_string_ref, 2, 2);
    define_prim(vm, "string-set!", prim_string_set, 3, 3);
    define_prim(vm, "make-string", prim_make_string, -1, 1);
    define_prim(vm, "string-append", prim_string_append, -1, 0);
    define_prim(vm, "substring", prim_substring, 3, 3);
    define_prim(vm, "string=?", prim_string_eq, -1, 2);
    define_prim(vm, "symbol->string", prim_symbol_to_string, 1, 1);
    define_prim(vm, "string->symbol", prim_string_to_symbol, 1, 1);
    define_prim(vm, "vector-length", prim_vector_length, 1, 1);
    define_prim(vm, "vector-ref", prim_vector_ref, 2, 2);
    define_prim(vm, "vector-set!", prim_vector_set, 3, 3);
    define_prim(vm, "make-vector", prim_make_vector, -1, 1);
    define_prim(vm, "vector->list", prim_vector_to_list, 1, 1);
    define_prim(vm, "list->vector", prim_list_to_vector, 1, 1);
    define_prim(vm, "string->list", prim_string_to_list, 1, 1);
    define_prim(vm, "list->string", prim_list_to_string, 1, 1);
}
