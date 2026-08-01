#include "chaaya/prim.h"

#include "chaaya/hashtable.h"

#include <stdio.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChHashtable *require_hashtable(ChVM *vm, const char *who, ChValue v) {
    if (!ch_is_hashtable(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected hash-table", who);
        return NULL;
    }
    return ch_as_hashtable(v);
}

static ChValue fail_hashtable_status(ChVM *vm, const char *who, ChHashtableStatus st) {
    if (st == CH_HASHTABLE_BAD_KEY) {
        snprintf(vm->error, sizeof(vm->error),
                 "%s: expected symbol, fixnum, or string key", who);
    } else if (st == CH_HASHTABLE_OOM) {
        snprintf(vm->error, sizeof(vm->error), "%s: out of memory", who);
    } else {
        snprintf(vm->error, sizeof(vm->error), "%s: internal hash-table error", who);
    }
    return CH_UNDEFINED;
}

static int parse_comparator_mode(ChValue comparator, ChHashtableMode *out_mode) {
    if (!ch_is_native(comparator)) {
        return 0;
    }
    const char *name = ch_as_native(comparator)->name;
    if (strcmp(name, "eq?") == 0) {
        *out_mode = CH_HASHTABLE_EQ;
        return 1;
    }
    if (strcmp(name, "eqv?") == 0) {
        *out_mode = CH_HASHTABLE_EQV;
        return 1;
    }
    return 0;
}

static ChValue prim_make_hash_table(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 1) {
        snprintf(vm->error, sizeof(vm->error),
                 "make-hash-table: expected 0 or 1 arguments");
        return CH_UNDEFINED;
    }

    ChHashtableMode mode = CH_HASHTABLE_EQV;
    if (nargs == 1 && !parse_comparator_mode(args[0], &mode)) {
        snprintf(vm->error, sizeof(vm->error),
                 "make-hash-table: comparator must be eq? or eqv?");
        return CH_UNDEFINED;
    }

    ChValue ht_v = ch_gc_make_hashtable(&vm->gc, 8);
    ch_as_hashtable(ht_v)->mode = mode;
    return ht_v;
}

static ChValue prim_hash_table_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_hashtable(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_hash_table_ref(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 3) {
        snprintf(vm->error, sizeof(vm->error),
                 "hash-table-ref: expected 2 or 3 arguments");
        return CH_UNDEFINED;
    }

    ChHashtable *ht = require_hashtable(vm, "hash-table-ref", args[0]);
    if (!ht) {
        return CH_UNDEFINED;
    }

    ChValue value = CH_UNDEFINED;
    ChHashtableStatus st = ch_hashtable_get(ht, args[1], &value);
    if (st == CH_HASHTABLE_OK) {
        return value;
    }
    if (st == CH_HASHTABLE_BAD_KEY) {
        return fail_hashtable_status(vm, "hash-table-ref", st);
    }

    if (nargs == 3) {
        if (ch_is_procedure(args[2])) {
            ChValue out = CH_VOID;
            ChVMStatus call_st = ch_vm_apply(vm, args[2], NULL, 0, &out);
            if (call_st == CH_VM_CONTINUATION_INVOKED) {
                vm->continuation_invoked = true;
                return CH_UNDEFINED;
            }
            if (call_st != CH_VM_OK) {
                return CH_UNDEFINED;
            }
            return ch_coerce_single(out);
        }
        return args[2];
    }

    snprintf(vm->error, sizeof(vm->error), "hash-table-ref: key not found");
    return CH_UNDEFINED;
}

static ChValue prim_hash_table_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChHashtable *ht = require_hashtable(vm, "hash-table-set!", args[0]);
    if (!ht) {
        return CH_UNDEFINED;
    }

    ChHashtableStatus st = ch_hashtable_set(ht, args[1], args[2]);
    if (st != CH_HASHTABLE_OK) {
        return fail_hashtable_status(vm, "hash-table-set!", st);
    }
    ch_gc_write_barrier(&vm->gc, &ht->header, args[1]);
    ch_gc_write_barrier(&vm->gc, &ht->header, args[2]);
    return CH_VOID;
}

static ChValue prim_hash_table_delete(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChHashtable *ht = require_hashtable(vm, "hash-table-delete!", args[0]);
    if (!ht) {
        return CH_UNDEFINED;
    }

    ChHashtableStatus st = ch_hashtable_delete(ht, args[1]);
    if (st == CH_HASHTABLE_BAD_KEY) {
        return fail_hashtable_status(vm, "hash-table-delete!", st);
    }
    return CH_VOID;
}

static ChValue prim_hash_table_size(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChHashtable *ht = require_hashtable(vm, "hash-table-size", args[0]);
    if (!ht) {
        return CH_UNDEFINED;
    }
    if (ht->count > (size_t)CH_FIXNUM_MAX) {
        snprintf(vm->error, sizeof(vm->error), "hash-table-size: size out of fixnum range");
        return CH_UNDEFINED;
    }
    return ch_make_fixnum((int64_t)ht->count);
}

static ChValue prim_hash_table_keys(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChHashtable *ht = require_hashtable(vm, "hash-table-keys", args[0]);
    if (!ht) {
        return CH_UNDEFINED;
    }

    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (size_t i = 0; i < ht->cap; i++) {
        if (!ht->used[i] || ht->keys[i] == CH_UNDEFINED) {
            continue;
        }
        ChValue key = ht->keys[i];
        ch_gc_push(&vm->gc, &key);
        list = ch_gc_cons(&vm->gc, key, list);
        ch_gc_pop(&vm->gc);
    }
    ch_gc_pop(&vm->gc);
    return list;
}

static ChValue prim_hash_table_values(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChHashtable *ht = require_hashtable(vm, "hash-table-values", args[0]);
    if (!ht) {
        return CH_UNDEFINED;
    }

    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (size_t i = 0; i < ht->cap; i++) {
        if (!ht->used[i] || ht->keys[i] == CH_UNDEFINED) {
            continue;
        }
        ChValue val = ht->vals[i];
        ch_gc_push(&vm->gc, &val);
        list = ch_gc_cons(&vm->gc, val, list);
        ch_gc_pop(&vm->gc);
    }
    ch_gc_pop(&vm->gc);
    return list;
}

static void snapshot_entries(ChVM *vm, ChHashtable *ht, ChValue *keys_vec, ChValue *vals_vec,
                             size_t *len_out) {
    *keys_vec = ch_gc_make_vector(&vm->gc, ht->count, CH_UNDEFINED);
    ch_gc_push(&vm->gc, keys_vec);
    *vals_vec = ch_gc_make_vector(&vm->gc, ht->count, CH_UNDEFINED);
    ch_gc_push(&vm->gc, vals_vec);

    ChVector *ks = ch_as_vector(*keys_vec);
    ChVector *vs = ch_as_vector(*vals_vec);
    size_t n = 0;
    for (size_t i = 0; i < ht->cap; i++) {
        if (!ht->used[i] || ht->keys[i] == CH_UNDEFINED) {
            continue;
        }
        if (n >= ks->len) {
            break;
        }
        ks->items[n] = ht->keys[i];
        vs->items[n] = ht->vals[i];
        n++;
    }
    *len_out = n;
}

static ChValue prim_hash_table_walk(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChHashtable *ht = require_hashtable(vm, "hash-table-walk", args[0]);
    if (!ht) {
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "hash-table-walk: expected procedure");
        return CH_UNDEFINED;
    }

    ChValue keys_vec = CH_NIL;
    ChValue vals_vec = CH_NIL;
    size_t len = 0;
    snapshot_entries(vm, ht, &keys_vec, &vals_vec, &len);

    ChVector *ks = ch_as_vector(keys_vec);
    ChVector *vs = ch_as_vector(vals_vec);
    for (size_t i = 0; i < len; i++) {
        ChValue call_args[2] = {ks->items[i], vs->items[i]};
        ChValue ignored = CH_VOID;
        ChVMStatus st = ch_vm_apply(vm, args[1], call_args, 2, &ignored);
        if (st == CH_VM_CONTINUATION_INVOKED) {
            ch_gc_pop_n(&vm->gc, 2);
            vm->continuation_invoked = true;
            return CH_UNDEFINED;
        }
        if (st != CH_VM_OK) {
            ch_gc_pop_n(&vm->gc, 2);
            return CH_UNDEFINED;
        }
    }

    ch_gc_pop_n(&vm->gc, 2);
    return CH_VOID;
}

static ChValue prim_hash_table_fold(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChHashtable *ht = require_hashtable(vm, "hash-table-fold", args[0]);
    if (!ht) {
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "hash-table-fold: expected procedure");
        return CH_UNDEFINED;
    }

    ChValue acc = args[2];
    ch_gc_push(&vm->gc, &acc);

    ChValue keys_vec = CH_NIL;
    ChValue vals_vec = CH_NIL;
    size_t len = 0;
    snapshot_entries(vm, ht, &keys_vec, &vals_vec, &len);

    ChVector *ks = ch_as_vector(keys_vec);
    ChVector *vs = ch_as_vector(vals_vec);
    for (size_t i = 0; i < len; i++) {
        ChValue call_args[3] = {ks->items[i], vs->items[i], acc};
        ChValue next = CH_VOID;
        ChVMStatus st = ch_vm_apply(vm, args[1], call_args, 3, &next);
        if (st == CH_VM_CONTINUATION_INVOKED) {
            ch_gc_pop_n(&vm->gc, 3);
            vm->continuation_invoked = true;
            return CH_UNDEFINED;
        }
        if (st != CH_VM_OK) {
            ch_gc_pop_n(&vm->gc, 3);
            return CH_UNDEFINED;
        }
        acc = ch_coerce_single(next);
    }

    ch_gc_pop_n(&vm->gc, 2);
    ch_gc_pop(&vm->gc);
    return acc;
}

void ch_register_hashtable_primitives(ChVM *vm) {
    define_prim(vm, "make-hash-table", prim_make_hash_table, -1, 0);
    define_prim(vm, "hash-table?", prim_hash_table_p, 1, 1);
    define_prim(vm, "hash-table-ref", prim_hash_table_ref, -1, 2);
    define_prim(vm, "hash-table-set!", prim_hash_table_set, 3, 3);
    define_prim(vm, "hash-table-delete!", prim_hash_table_delete, 2, 2);
    define_prim(vm, "hash-table-size", prim_hash_table_size, 1, 1);
    define_prim(vm, "hash-table-keys", prim_hash_table_keys, 1, 1);
    define_prim(vm, "hash-table-values", prim_hash_table_values, 1, 1);
    define_prim(vm, "hash-table-walk", prim_hash_table_walk, 2, 2);
    define_prim(vm, "hash-table-fold", prim_hash_table_fold, 3, 3);
}
