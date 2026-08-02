#include "chaaya/prim.h"

#include "chaaya/hashtable.h"
#include "chaaya/unicode.h"

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
    if (strcmp(name, "equal?") == 0) {
        *out_mode = CH_HASHTABLE_EQUAL;
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

    ChHashtableMode mode = CH_HASHTABLE_EQUAL;
    if (nargs == 1 && !parse_comparator_mode(args[0], &mode)) {
        snprintf(vm->error, sizeof(vm->error),
                 "make-hash-table: comparator must be eq?, eqv?, or equal?");
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

static ChValue prim_hash_table_exists_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChHashtable *ht = require_hashtable(vm, "hash-table-exists?", args[0]);
    if (!ht) {
        return CH_UNDEFINED;
    }
    ChValue val = CH_UNDEFINED;
    ChHashtableStatus st = ch_hashtable_get(ht, args[1], &val);
    if (st == CH_HASHTABLE_OK) {
        return CH_TRUE;
    }
    if (st == CH_HASHTABLE_NOT_FOUND) {
        return CH_FALSE;
    }
    return fail_hashtable_status(vm, "hash-table-exists?", st);
}

static ChValue prim_hash_table_ref_default(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return prim_hash_table_ref(vm, args, 3);
}

static ChValue prim_hash_table_update_bang(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 3) {
        snprintf(vm->error, sizeof(vm->error), "hash-table-update!: expected at least 3 arguments");
        return CH_UNDEFINED;
    }
    ChValue ref_args[2] = {args[0], args[1]};
    ChValue old = prim_hash_table_ref(vm, ref_args, 2);
    if (old == CH_UNDEFINED) {
        return CH_UNDEFINED;
    }
    ChValue proc_result = CH_UNDEFINED;
    if (ch_vm_apply(vm, args[2], &old, 1, &proc_result) != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    ChValue set_args[3] = {args[0], args[1], ch_coerce_single(proc_result)};
    if (prim_hash_table_set(vm, set_args, 3) == CH_UNDEFINED) {
        return CH_UNDEFINED;
    }
    return args[0];
}

static ChValue prim_hash_table_update_default_bang(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 4) {
        snprintf(vm->error, sizeof(vm->error), "hash-table-update!/default: expected 4 arguments");
        return CH_UNDEFINED;
    }
    ChValue ref_args[3] = {args[0], args[1], args[3]};
    ChValue old = prim_hash_table_ref(vm, ref_args, 3);
    if (old == CH_UNDEFINED) {
        return CH_UNDEFINED;
    }
    ChValue proc_result = CH_UNDEFINED;
    if (ch_vm_apply(vm, args[2], &old, 1, &proc_result) != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    ChValue set_args[3] = {args[0], args[1], ch_coerce_single(proc_result)};
    if (prim_hash_table_set(vm, set_args, 3) == CH_UNDEFINED) {
        return CH_UNDEFINED;
    }
    return args[0];
}

static ChValue prim_hash_table_to_alist(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChHashtable *ht = require_hashtable(vm, "hash-table->alist", args[0]);
    if (!ht) {
        return CH_UNDEFINED;
    }
    ChValue acc = CH_NIL;
    ch_gc_push(&vm->gc, &acc);
    ChValue keys_vec = CH_NIL;
    ChValue vals_vec = CH_NIL;
    size_t len = 0;
    snapshot_entries(vm, ht, &keys_vec, &vals_vec, &len);
    ChVector *ks = ch_as_vector(keys_vec);
    ChVector *vs = ch_as_vector(vals_vec);
    for (size_t i = 0; i < len; i++) {
        ChValue entry = ch_gc_cons(&vm->gc, ks->items[i], vs->items[i]);
        acc = ch_gc_cons(&vm->gc, entry, acc);
    }
    ch_gc_pop_n(&vm->gc, 2);
    ch_gc_pop(&vm->gc);
    return acc;
}

static ChValue prim_alist_to_hash_table(ChVM *vm, ChValue *args, int nargs) {
    ChValue ht = prim_make_hash_table(vm, args, nargs > 1 ? 1 : 0);
    if (ht == CH_UNDEFINED) {
        return CH_UNDEFINED;
    }
    if (!ch_is_pair(args[0])) {
        return ht;
    }
    for (ChValue p = args[0]; ch_is_pair(p); p = ch_cdr(p)) {
        ChValue entry = ch_car(p);
        if (!ch_is_pair(entry)) {
            snprintf(vm->error, sizeof(vm->error), "alist->hash-table: bad alist");
            return CH_UNDEFINED;
        }
        ChValue set_args[3] = {ht, ch_car(entry), ch_cdr(entry)};
        if (prim_hash_table_set(vm, set_args, 3) == CH_UNDEFINED) {
            return CH_UNDEFINED;
        }
    }
    return ht;
}

static uint32_t hash_mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

static int parse_hash_bound(ChVM *vm, ChValue v, size_t *out, const char *who) {
    if (!ch_is_fixnum(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected exact integer", who);
        return -1;
    }
    int64_t n = ch_to_fixnum(v);
    if (n <= 0) {
        snprintf(vm->error, sizeof(vm->error), "%s: bound must be positive", who);
        return -1;
    }
    *out = (size_t)n;
    return 0;
}

static ChValue prim_hash(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    uint32_t h = 0;
    if (ch_is_fixnum(args[0])) {
        int64_t n = ch_to_fixnum(args[0]);
        h = hash_mix((uint32_t)(n ^ (n >> 32)));
    } else if (ch_is_string(args[0])) {
        ChString *s = ch_as_string(args[0]);
        for (size_t i = 0; i < s->len; i++) {
            h = hash_mix(h * 31 + (uint8_t)s->data[i]);
        }
    } else if (ch_is_symbol(args[0])) {
        const char *name = ch_as_symbol(args[0])->name;
        for (size_t i = 0; name[i]; i++) {
            h = hash_mix(h * 31 + (uint8_t)name[i]);
        }
    } else {
        h = hash_mix((uint32_t)(uintptr_t)ch_to_object(args[0]));
    }
    size_t bound = 0;
    if (nargs > 1) {
        if (parse_hash_bound(vm, args[1], &bound, "hash") != 0) {
            return CH_UNDEFINED;
        }
        return ch_make_fixnum((int64_t)(h % (uint32_t)bound));
    }
    return ch_make_fixnum((int64_t)h);
}

static ChValue prim_string_hash(ChVM *vm, ChValue *args, int nargs) {
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-hash: expected string");
        return CH_UNDEFINED;
    }
    return prim_hash(vm, args, nargs);
}

static bool hash_utf8_decode(const char *bytes, size_t len, size_t pos, uint32_t *cp_out,
                             size_t *next_out) {
    if (pos >= len) {
        return false;
    }
    unsigned char b0 = (unsigned char)bytes[pos];
    if (b0 < 0x80) {
        *cp_out = b0;
        *next_out = pos + 1;
        return true;
    }
    int seq = 0;
    if ((b0 & 0xE0) == 0xC0) {
        seq = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
        seq = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
        seq = 4;
    } else {
        return false;
    }
    if (pos + (size_t)seq > len) {
        return false;
    }
    uint32_t cp = b0 & ((1U << (8 - seq)) - 1U);
    for (int i = 1; i < seq; i++) {
        unsigned char b = (unsigned char)bytes[pos + (size_t)i];
        if ((b & 0xC0) != 0x80) {
            return false;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    *cp_out = cp;
    *next_out = pos + (size_t)seq;
    return true;
}

static ChValue prim_string_ci_hash(ChVM *vm, ChValue *args, int nargs) {
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-ci-hash: expected string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    uint64_t h = 0;
    size_t pos = 0;
    while (pos < s->len) {
        uint32_t cp = 0;
        size_t next = pos;
        if (!hash_utf8_decode(s->data, s->len, pos, &cp, &next)) {
            h = h * 31 + (uint8_t)s->data[pos];
            pos++;
            continue;
        }
        h = h * 31 + ch_unicode_foldcase(cp);
        pos = next;
    }
    if (nargs > 1) {
        size_t bound = 0;
        if (parse_hash_bound(vm, args[1], &bound, "string-ci-hash") != 0) {
            return CH_UNDEFINED;
        }
        return ch_make_fixnum((int64_t)(h % bound));
    }
    return ch_make_fixnum((int64_t)(h & 0x3FFFFFFFFFFFFFFFULL));
}

static ChValue prim_hash_by_identity(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    size_t bound = 0;
    uint32_t h = hash_mix((uint32_t)(uintptr_t)ch_to_object(args[0]));
    if (nargs > 1) {
        if (parse_hash_bound(vm, args[1], &bound, "hash-by-identity") != 0) {
            return CH_UNDEFINED;
        }
        return ch_make_fixnum((int64_t)(h % (uint32_t)bound));
    }
    return ch_make_fixnum((int64_t)h);
}

static ChValue prim_hash_table_merge_bang(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChHashtable *ht1 = require_hashtable(vm, "hash-table-merge!", args[0]);
    ChHashtable *ht2 = require_hashtable(vm, "hash-table-merge!", args[1]);
    if (!ht1 || !ht2) {
        return CH_UNDEFINED;
    }
    if (ht1 == ht2) {
        return args[0];
    }

    for (size_t i = 0; i < ht2->cap; i++) {
        if (!ht2->used[i] || ht2->keys[i] == CH_UNDEFINED) {
            continue;
        }
        ChHashtableStatus st = ch_hashtable_set(ht1, ht2->keys[i], ht2->vals[i]);
        if (st != CH_HASHTABLE_OK) {
            return fail_hashtable_status(vm, "hash-table-merge!", st);
        }
        ch_gc_write_barrier(&vm->gc, &ht1->header, ht2->keys[i]);
        ch_gc_write_barrier(&vm->gc, &ht1->header, ht2->vals[i]);
    }
    return args[0];
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
    define_prim(vm, "hash-table-exists?", prim_hash_table_exists_p, 2, 2);
    define_prim(vm, "hash-table-ref/default", prim_hash_table_ref_default, 3, 3);
    define_prim(vm, "hash-table-update!", prim_hash_table_update_bang, -1, 3);
    define_prim(vm, "hash-table-update!/default", prim_hash_table_update_default_bang, 4, 4);
    define_prim(vm, "hash-table->alist", prim_hash_table_to_alist, 1, 1);
    define_prim(vm, "alist->hash-table", prim_alist_to_hash_table, -1, 1);
    define_prim(vm, "hash-table-merge!", prim_hash_table_merge_bang, 2, 2);
    define_prim(vm, "hash", prim_hash, -1, 1);
    define_prim(vm, "string-hash", prim_string_hash, -1, 1);
    define_prim(vm, "string-ci-hash", prim_string_ci_hash, -1, 1);
    define_prim(vm, "hash-by-identity", prim_hash_by_identity, -1, 1);
}
