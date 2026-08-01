#include "chaaya/prim.h"

#include <stdio.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static bool parse_nonnegative_index(ChVM *vm, ChValue v, size_t *out, const char *who) {
    if (!ch_is_fixnum(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected exact integer", who);
        return false;
    }
    int64_t n = ch_to_fixnum(v);
    if (n < 0) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected non-negative integer", who);
        return false;
    }
    *out = (size_t)n;
    return true;
}

static bool parse_u8(ChVM *vm, ChValue v, uint8_t *out, const char *who) {
    if (!ch_is_fixnum(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected exact integer", who);
        return false;
    }
    int64_t n = ch_to_fixnum(v);
    if (n < 0 || n > 255) {
        snprintf(vm->error, sizeof(vm->error), "%s: byte out of range", who);
        return false;
    }
    *out = (uint8_t)n;
    return true;
}

static bool parse_slice(ChVM *vm, ChValue *args, int nargs, int start_arg, size_t len, const char *who,
                        size_t *start_out, size_t *end_out) {
    size_t start = 0;
    size_t end = len;
    if (nargs > start_arg) {
        if (!parse_nonnegative_index(vm, args[start_arg], &start, who)) {
            return false;
        }
    }
    if (nargs > start_arg + 1) {
        if (!parse_nonnegative_index(vm, args[start_arg + 1], &end, who)) {
            return false;
        }
    }
    if (start > end || end > len) {
        snprintf(vm->error, sizeof(vm->error), "%s: slice out of range", who);
        return false;
    }
    *start_out = start;
    *end_out = end;
    return true;
}

static bool require_mutable_bytevector(ChVM *vm, ChBytevector *bv, const char *who) {
    if (ch_object_is_immutable(&bv->header)) {
        snprintf(vm->error, sizeof(vm->error), "%s: immutable bytevector", who);
        return false;
    }
    return true;
}

static ChValue prim_bytevector_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_bytevector(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_bytevector(ChVM *vm, ChValue *args, int nargs) {
    ChValue out = ch_gc_make_bytevector(&vm->gc, (size_t)nargs, 0);
    ChBytevector *bv = ch_as_bytevector(out);
    for (int i = 0; i < nargs; i++) {
        if (!parse_u8(vm, args[i], &bv->data[i], "bytevector")) {
            return CH_UNDEFINED;
        }
    }
    return out;
}

static ChValue prim_make_bytevector(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 2) {
        snprintf(vm->error, sizeof(vm->error), "make-bytevector: expected 1 or 2 arguments");
        return CH_UNDEFINED;
    }
    size_t len = 0;
    if (!parse_nonnegative_index(vm, args[0], &len, "make-bytevector")) {
        return CH_UNDEFINED;
    }
    uint8_t fill = 0;
    if (nargs > 1 && !parse_u8(vm, args[1], &fill, "make-bytevector")) {
        return CH_UNDEFINED;
    }
    return ch_gc_make_bytevector(&vm->gc, len, fill);
}

static ChValue prim_bytevector_length(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "bytevector-length: not a bytevector");
        return CH_UNDEFINED;
    }
    return ch_make_fixnum((int64_t)ch_as_bytevector(args[0])->len);
}

static ChValue prim_bytevector_u8_ref(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "bytevector-u8-ref: not a bytevector");
        return CH_UNDEFINED;
    }
    size_t idx = 0;
    if (!parse_nonnegative_index(vm, args[1], &idx, "bytevector-u8-ref")) {
        return CH_UNDEFINED;
    }
    ChBytevector *bv = ch_as_bytevector(args[0]);
    if (idx >= bv->len) {
        snprintf(vm->error, sizeof(vm->error), "bytevector-u8-ref: index out of range");
        return CH_UNDEFINED;
    }
    return ch_make_fixnum((int64_t)bv->data[idx]);
}

static ChValue prim_bytevector_u8_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "bytevector-u8-set!: not a bytevector");
        return CH_UNDEFINED;
    }
    ChBytevector *bv = ch_as_bytevector(args[0]);
    if (!require_mutable_bytevector(vm, bv, "bytevector-u8-set!")) {
        return CH_UNDEFINED;
    }
    size_t idx = 0;
    if (!parse_nonnegative_index(vm, args[1], &idx, "bytevector-u8-set!")) {
        return CH_UNDEFINED;
    }
    if (idx >= bv->len) {
        snprintf(vm->error, sizeof(vm->error), "bytevector-u8-set!: index out of range");
        return CH_UNDEFINED;
    }
    uint8_t byte = 0;
    if (!parse_u8(vm, args[2], &byte, "bytevector-u8-set!")) {
        return CH_UNDEFINED;
    }
    bv->data[idx] = byte;
    return CH_VOID;
}

static ChValue prim_bytevector_copy(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 3) {
        snprintf(vm->error, sizeof(vm->error), "bytevector-copy: expected 1 to 3 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "bytevector-copy: not a bytevector");
        return CH_UNDEFINED;
    }
    ChBytevector *src = ch_as_bytevector(args[0]);
    size_t start = 0;
    size_t end = src->len;
    if (!parse_slice(vm, args, nargs, 1, src->len, "bytevector-copy", &start, &end)) {
        return CH_UNDEFINED;
    }
    ChValue out = ch_gc_make_bytevector(&vm->gc, end - start, 0);
    ChBytevector *dst = ch_as_bytevector(out);
    if (end > start) {
        memcpy(dst->data, src->data + start, end - start);
    }
    return out;
}

static ChValue prim_bytevector_copy_bang(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 5) {
        snprintf(vm->error, sizeof(vm->error), "bytevector-copy!: expected 3 to 5 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_bytevector(args[0]) || !ch_is_bytevector(args[2])) {
        snprintf(vm->error, sizeof(vm->error), "bytevector-copy!: expected bytevectors");
        return CH_UNDEFINED;
    }
    ChBytevector *to = ch_as_bytevector(args[0]);
    ChBytevector *from = ch_as_bytevector(args[2]);
    if (!require_mutable_bytevector(vm, to, "bytevector-copy!")) {
        return CH_UNDEFINED;
    }
    size_t at = 0;
    if (!parse_nonnegative_index(vm, args[1], &at, "bytevector-copy!")) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = from->len;
    if (!parse_slice(vm, args, nargs, 3, from->len, "bytevector-copy!", &start, &end)) {
        return CH_UNDEFINED;
    }
    size_t count = end - start;
    if (at > to->len || count > to->len - at) {
        snprintf(vm->error, sizeof(vm->error), "bytevector-copy!: destination out of range");
        return CH_UNDEFINED;
    }
    if (count > 0) {
        memmove(to->data + at, from->data + start, count);
    }
    return CH_VOID;
}

static ChValue prim_bytevector_append(ChVM *vm, ChValue *args, int nargs) {
    size_t total = 0;
    for (int i = 0; i < nargs; i++) {
        if (!ch_is_bytevector(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "bytevector-append: not a bytevector");
            return CH_UNDEFINED;
        }
        total += ch_as_bytevector(args[i])->len;
    }
    ChValue out = ch_gc_make_bytevector(&vm->gc, total, 0);
    ChBytevector *dst = ch_as_bytevector(out);
    size_t pos = 0;
    for (int i = 0; i < nargs; i++) {
        ChBytevector *src = ch_as_bytevector(args[i]);
        if (src->len > 0) {
            memcpy(dst->data + pos, src->data, src->len);
            pos += src->len;
        }
    }
    return out;
}

static ChValue prim_list_to_bytevector(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue lst = args[0];
    size_t len = 0;
    for (ChValue p = lst; ch_is_pair(p); p = ch_cdr(p)) {
        uint8_t b = 0;
        if (!parse_u8(vm, ch_car(p), &b, "list->bytevector")) {
            return CH_UNDEFINED;
        }
        (void)b;
        len++;
    }
    if (!ch_is_nil(lst)) {
        ChValue p = lst;
        while (ch_is_pair(p)) {
            p = ch_cdr(p);
        }
        if (!ch_is_nil(p)) {
            snprintf(vm->error, sizeof(vm->error), "list->bytevector: improper list");
            return CH_UNDEFINED;
        }
    }
    ChValue out = ch_gc_make_bytevector(&vm->gc, len, 0);
    ChBytevector *bv = ch_as_bytevector(out);
    size_t i = 0;
    for (ChValue p = lst; ch_is_pair(p); p = ch_cdr(p)) {
        uint8_t b = 0;
        if (!parse_u8(vm, ch_car(p), &b, "list->bytevector")) {
            return CH_UNDEFINED;
        }
        bv->data[i++] = b;
    }
    return out;
}

static ChValue prim_bytevector_to_list(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 3) {
        snprintf(vm->error, sizeof(vm->error), "bytevector->list: expected 1 to 3 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "bytevector->list: not a bytevector");
        return CH_UNDEFINED;
    }
    ChBytevector *bv = ch_as_bytevector(args[0]);
    size_t start = 0;
    size_t end = bv->len;
    if (!parse_slice(vm, args, nargs, 1, bv->len, "bytevector->list", &start, &end)) {
        return CH_UNDEFINED;
    }
    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (size_t i = end; i > start; i--) {
        ChValue item = ch_make_fixnum((int64_t)bv->data[i - 1]);
        list = ch_gc_cons(&vm->gc, item, list);
    }
    ch_gc_pop(&vm->gc);
    return list;
}

static ChValue prim_utf8_to_string(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 3) {
        snprintf(vm->error, sizeof(vm->error), "utf8->string: expected 1 to 3 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "utf8->string: not a bytevector");
        return CH_UNDEFINED;
    }
    ChBytevector *bv = ch_as_bytevector(args[0]);
    size_t start = 0;
    size_t end = bv->len;
    if (!parse_slice(vm, args, nargs, 1, bv->len, "utf8->string", &start, &end)) {
        return CH_UNDEFINED;
    }
    return ch_gc_make_string(&vm->gc, (const char *)(bv->data + start), end - start);
}

static ChValue prim_string_to_utf8(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 3) {
        snprintf(vm->error, sizeof(vm->error), "string->utf8: expected 1 to 3 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string->utf8: not a string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t start = 0;
    size_t end = s->len;
    if (!parse_slice(vm, args, nargs, 1, s->len, "string->utf8", &start, &end)) {
        return CH_UNDEFINED;
    }
    ChValue out = ch_gc_make_bytevector(&vm->gc, end - start, 0);
    ChBytevector *bv = ch_as_bytevector(out);
    if (end > start) {
        memcpy(bv->data, (const uint8_t *)s->data + start, end - start);
    }
    return out;
}

void ch_register_bytevector_primitives(ChVM *vm) {
    define_prim(vm, "bytevector?", prim_bytevector_p, 1, 1);
    define_prim(vm, "bytevector", prim_bytevector, -1, 0);
    define_prim(vm, "make-bytevector", prim_make_bytevector, -1, 1);
    define_prim(vm, "bytevector-length", prim_bytevector_length, 1, 1);
    define_prim(vm, "bytevector-u8-ref", prim_bytevector_u8_ref, 2, 2);
    define_prim(vm, "bytevector-u8-set!", prim_bytevector_u8_set, 3, 3);
    define_prim(vm, "bytevector-copy", prim_bytevector_copy, -1, 1);
    define_prim(vm, "bytevector-copy!", prim_bytevector_copy_bang, -1, 3);
    define_prim(vm, "bytevector-append", prim_bytevector_append, -1, 0);
    define_prim(vm, "list->bytevector", prim_list_to_bytevector, 1, 1);
    define_prim(vm, "bytevector->list", prim_bytevector_to_list, -1, 1);
    define_prim(vm, "utf8->string", prim_utf8_to_string, -1, 1);
    define_prim(vm, "string->utf8", prim_string_to_utf8, -1, 1);
}
