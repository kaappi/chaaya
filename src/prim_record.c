#include "chaaya/prim.h"

#include <stdio.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChValue prim_make_record_type(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "%%make-record-type: expected string");
        return CH_UNDEFINED;
    }
    if (!ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "%%make-record-type: expected fixnum");
        return CH_UNDEFINED;
    }
    int64_t n = ch_to_fixnum(args[1]);
    if (n < 0 || n > CH_RECORD_MAX_FIELDS) {
        snprintf(vm->error, sizeof(vm->error), "%%make-record-type: bad field count");
        return CH_UNDEFINED;
    }
    return ch_gc_make_record_type(&vm->gc, args[0], (uint16_t)n);
}

static ChValue prim_make_record(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_record_type(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "%%make-record: expected record type");
        return CH_UNDEFINED;
    }
    ChRecordType *rt = ch_as_record_type(args[0]);
    int nfields = nargs - 1;
    if (nfields != (int)rt->num_fields) {
        snprintf(vm->error, sizeof(vm->error), "%%make-record: wrong number of fields");
        return CH_UNDEFINED;
    }
    return ch_gc_make_record(&vm->gc, rt, args + 1, (uint16_t)nfields);
}

static ChValue prim_record_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_record_type(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "%%record?: expected record type");
        return CH_UNDEFINED;
    }
    if (!ch_is_record(args[0])) {
        return CH_FALSE;
    }
    return ch_as_record(args[0])->rtype == ch_as_record_type(args[1]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_record_ref(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_record_type(args[2])) {
        snprintf(vm->error, sizeof(vm->error), "%%record-ref: expected record type");
        return CH_UNDEFINED;
    }
    ChRecordType *rt = ch_as_record_type(args[2]);
    if (!ch_is_record(args[0]) || ch_as_record(args[0])->rtype != rt) {
        snprintf(vm->error, sizeof(vm->error), "%%record-ref: wrong record type");
        return CH_UNDEFINED;
    }
    if (!ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "%%record-ref: expected fixnum index");
        return CH_UNDEFINED;
    }
    int64_t idx = ch_to_fixnum(args[1]);
    ChRecord *r = ch_as_record(args[0]);
    if (idx < 0 || idx >= (int64_t)r->num_fields) {
        snprintf(vm->error, sizeof(vm->error), "%%record-ref: index out of range");
        return CH_UNDEFINED;
    }
    return r->fields[idx];
}

static ChValue prim_record_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_record_type(args[3])) {
        snprintf(vm->error, sizeof(vm->error), "%%record-set!: expected record type");
        return CH_UNDEFINED;
    }
    ChRecordType *rt = ch_as_record_type(args[3]);
    if (!ch_is_record(args[0]) || ch_as_record(args[0])->rtype != rt) {
        snprintf(vm->error, sizeof(vm->error), "%%record-set!: wrong record type");
        return CH_UNDEFINED;
    }
    if (!ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "%%record-set!: expected fixnum index");
        return CH_UNDEFINED;
    }
    int64_t idx = ch_to_fixnum(args[1]);
    ChRecord *r = ch_as_record(args[0]);
    if (idx < 0 || idx >= (int64_t)r->num_fields) {
        snprintf(vm->error, sizeof(vm->error), "%%record-set!: index out of range");
        return CH_UNDEFINED;
    }
    r->fields[idx] = args[2];
    return CH_VOID;
}

void ch_register_record_primitives(ChVM *vm) {
    define_prim(vm, "%make-record-type", prim_make_record_type, 2, 2);
    define_prim(vm, "%make-record", prim_make_record, -1, 1);
    define_prim(vm, "%record?", prim_record_p, 2, 2);
    define_prim(vm, "%record-ref", prim_record_ref, 3, 3);
    define_prim(vm, "%record-set!", prim_record_set, 4, 4);
}
