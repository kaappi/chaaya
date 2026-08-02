#include "chaaya/hashtable.h"

#include "chaaya/vm.h"

#include <stdint.h>
#include <stdlib.h>

#define CH_HASHTABLE_MIN_CAP 8
#define CH_HASHTABLE_LOAD_NUM 3
#define CH_HASHTABLE_LOAD_DEN 4

static uint64_t mix_u64(uint64_t x) {
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return x;
}

static size_t hash_bytes(const char *bytes, size_t len) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)bytes[i];
        h *= UINT64_C(1099511628211);
    }
    return (size_t)mix_u64(h);
}

static bool slot_occupied(const ChHashtable *ht, size_t idx) {
    return ht->used[idx] && ht->keys[idx] != CH_UNDEFINED;
}

static bool call_equiv(ChVM *vm, ChValue equiv_fn, ChValue a, ChValue b) {
    ChValue args[2] = {a, b};
    ChValue out = CH_FALSE;
    if (ch_vm_apply(vm, equiv_fn, args, 2, &out) != CH_VM_OK) {
        return false;
    }
    return out != CH_FALSE && !ch_is_false(out);
}

static bool key_equal(ChVM *vm, const ChHashtable *ht, ChValue a, ChValue b) {
    if (ch_is_procedure(ht->equiv_fn)) {
        return call_equiv(vm, ht->equiv_fn, a, b);
    }
    switch (ht->mode) {
    case CH_HASHTABLE_EQ:
        return ch_eq(a, b);
    case CH_HASHTABLE_EQV:
        return ch_eqv(a, b);
    case CH_HASHTABLE_EQUAL:
        return ch_equal(a, b);
    }
    return false;
}

bool ch_hashtable_key_supported(ChValue key) {
    (void)key;
    return true;
}

static size_t identity_hash(ChValue key) {
    return (size_t)mix_u64((uint64_t)(uintptr_t)ch_to_object(key));
}

static size_t value_hash(ChValue key) {
    if (ch_is_fixnum(key)) {
        return (size_t)mix_u64((uint64_t)ch_to_fixnum(key));
    }
    if (ch_is_symbol(key)) {
        ChSymbol *sym = ch_as_symbol(key);
        return hash_bytes(sym->name, sym->len);
    }
    if (ch_is_string(key)) {
        ChString *s = ch_as_string(key);
        return hash_bytes(s->data, s->len);
    }
    if (ch_is_char(key)) {
        return (size_t)mix_u64((uint64_t)ch_to_char(key));
    }
    if (key == CH_TRUE) {
        return 1;
    }
    if (key == CH_FALSE) {
        return 0;
    }
    if (ch_is_nil(key)) {
        return 2;
    }
    return identity_hash(key);
}

static size_t hash_key(ChVM *vm, const ChHashtable *ht, ChValue key) {
    if (ch_is_procedure(ht->hash_fn)) {
        ChValue out = CH_UNDEFINED;
        if (ch_vm_apply(vm, ht->hash_fn, &key, 1, &out) == CH_VM_OK) {
            if (ch_is_fixnum(out)) {
                int64_t n = ch_to_fixnum(out);
                uint64_t abs_n = (uint64_t)(n < 0 ? -n : n);
                return (size_t)mix_u64(abs_n);
            }
            return value_hash(out);
        }
    }
    if (ht->mode == CH_HASHTABLE_EQ) {
        return identity_hash(key);
    }
    return value_hash(key);
}

static void init_slots(ChValue *keys, ChValue *vals, size_t cap) {
    for (size_t i = 0; i < cap; i++) {
        keys[i] = CH_UNDEFINED;
        vals[i] = CH_UNDEFINED;
    }
}

static bool find_entry(ChVM *vm, const ChHashtable *ht, ChValue key, size_t *out_idx) {
    if (!ht->keys || !ht->vals || !ht->used || ht->cap == 0) {
        return false;
    }
    size_t idx = hash_key(vm, ht, key) % ht->cap;
    for (size_t probes = 0; probes < ht->cap; probes++) {
        if (!ht->used[idx]) {
            return false;
        }
        if (slot_occupied(ht, idx) && key_equal(vm, ht, ht->keys[idx], key)) {
            *out_idx = idx;
            return true;
        }
        idx = (idx + 1) % ht->cap;
    }
    return false;
}

static bool find_insert_slot(ChVM *vm, const ChHashtable *ht, ChValue key, size_t *out_idx,
                             bool *out_found) {
    if (!ht->keys || !ht->vals || !ht->used || ht->cap == 0) {
        return false;
    }
    size_t idx = hash_key(vm, ht, key) % ht->cap;
    size_t first_tombstone = ht->cap;
    for (size_t probes = 0; probes < ht->cap; probes++) {
        if (!ht->used[idx]) {
            *out_idx = (first_tombstone < ht->cap) ? first_tombstone : idx;
            *out_found = false;
            return true;
        }
        if (!slot_occupied(ht, idx)) {
            if (first_tombstone == ht->cap) {
                first_tombstone = idx;
            }
        } else if (key_equal(vm, ht, ht->keys[idx], key)) {
            *out_idx = idx;
            *out_found = true;
            return true;
        }
        idx = (idx + 1) % ht->cap;
    }
    if (first_tombstone < ht->cap) {
        *out_idx = first_tombstone;
        *out_found = false;
        return true;
    }
    return false;
}

static void insert_entry_no_resize(ChVM *vm, ChHashtable *ht, ChValue key, ChValue value) {
    size_t idx = hash_key(vm, ht, key) % ht->cap;
    for (size_t probes = 0; probes < ht->cap; probes++) {
        if (!ht->used[idx] || !slot_occupied(ht, idx)) {
            ht->used[idx] = true;
            ht->keys[idx] = key;
            ht->vals[idx] = value;
            ht->count++;
            return;
        }
        idx = (idx + 1) % ht->cap;
    }
}

static ChHashtableStatus rehash_table(ChVM *vm, ChHashtable *ht, size_t target_cap) {
    size_t new_cap = target_cap < CH_HASHTABLE_MIN_CAP ? CH_HASHTABLE_MIN_CAP : target_cap;
    ChValue *new_keys = (ChValue *)calloc(new_cap, sizeof(ChValue));
    ChValue *new_vals = (ChValue *)calloc(new_cap, sizeof(ChValue));
    bool *new_used = (bool *)calloc(new_cap, sizeof(bool));
    if (!new_keys || !new_vals || !new_used) {
        free(new_keys);
        free(new_vals);
        free(new_used);
        return CH_HASHTABLE_OOM;
    }
    init_slots(new_keys, new_vals, new_cap);

    ChValue *old_keys = ht->keys;
    ChValue *old_vals = ht->vals;
    bool *old_used = ht->used;
    size_t old_cap = ht->cap;

    ht->keys = new_keys;
    ht->vals = new_vals;
    ht->used = new_used;
    ht->cap = new_cap;
    ht->count = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old_used && old_used[i] && old_keys[i] != CH_UNDEFINED) {
            insert_entry_no_resize(vm, ht, old_keys[i], old_vals[i]);
        }
    }

    free(old_keys);
    free(old_vals);
    free(old_used);
    return CH_HASHTABLE_OK;
}

static ChHashtableStatus maybe_grow(ChVM *vm, ChHashtable *ht) {
    if (ht->cap == 0 || !ht->keys || !ht->vals || !ht->used) {
        return rehash_table(vm, ht, CH_HASHTABLE_MIN_CAP);
    }
    if ((ht->count + 1) > ((ht->cap * CH_HASHTABLE_LOAD_NUM) / CH_HASHTABLE_LOAD_DEN)) {
        if (ht->cap > (SIZE_MAX / 2)) {
            return CH_HASHTABLE_OOM;
        }
        return rehash_table(vm, ht, ht->cap * 2);
    }
    return CH_HASHTABLE_OK;
}

ChHashtableStatus ch_hashtable_get(ChVM *vm, const ChHashtable *ht, ChValue key,
                                   ChValue *out_value) {
    if (!ch_hashtable_key_supported(key)) {
        return CH_HASHTABLE_BAD_KEY;
    }
    size_t idx = 0;
    if (!find_entry(vm, ht, key, &idx)) {
        return CH_HASHTABLE_NOT_FOUND;
    }
    if (out_value) {
        *out_value = ht->vals[idx];
    }
    return CH_HASHTABLE_OK;
}

ChHashtableStatus ch_hashtable_set(ChVM *vm, ChHashtable *ht, ChValue key, ChValue value) {
    if (!ch_hashtable_key_supported(key)) {
        return CH_HASHTABLE_BAD_KEY;
    }

    ChHashtableStatus st = maybe_grow(vm, ht);
    if (st != CH_HASHTABLE_OK) {
        return st;
    }

    for (;;) {
        size_t idx = 0;
        bool found = false;
        if (!find_insert_slot(vm, ht, key, &idx, &found)) {
            if (ht->cap > (SIZE_MAX / 2)) {
                return CH_HASHTABLE_OOM;
            }
            st = rehash_table(vm, ht, ht->cap * 2);
            if (st != CH_HASHTABLE_OK) {
                return st;
            }
            continue;
        }

        if (found) {
            ht->vals[idx] = value;
            return CH_HASHTABLE_OK;
        }

        ht->used[idx] = true;
        ht->keys[idx] = key;
        ht->vals[idx] = value;
        ht->count++;
        return CH_HASHTABLE_OK;
    }
}

ChHashtableStatus ch_hashtable_delete(ChVM *vm, ChHashtable *ht, ChValue key) {
    if (!ch_hashtable_key_supported(key)) {
        return CH_HASHTABLE_BAD_KEY;
    }

    size_t idx = 0;
    if (!find_entry(vm, ht, key, &idx)) {
        return CH_HASHTABLE_NOT_FOUND;
    }

    ht->used[idx] = true;
    ht->keys[idx] = CH_UNDEFINED;
    ht->vals[idx] = CH_UNDEFINED;
    if (ht->count > 0) {
        ht->count--;
    }
    return CH_HASHTABLE_OK;
}
