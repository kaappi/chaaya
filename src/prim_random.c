#include "chaaya/prim.h"

#include "chaaya/bignum.h"
#include "chaaya/gc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stdlib.h>
#else
#include <fcntl.h>
#endif

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static uint64_t fresh_seed(void) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    uint64_t seed = 0;
    arc4random_buf(&seed, sizeof(seed));
    return seed;
#else
    uint64_t seed = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        (void)read(fd, &seed, sizeof(seed));
        close(fd);
    }
    if (seed == 0) {
        seed = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)&seed;
    }
    return seed;
#endif
}

static ChRandomSource *require_rs(ChVM *vm, ChValue v, const char *who) {
    if (!ch_is_random_source(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected random-source", who);
        return NULL;
    }
    return ch_as_random_source(v);
}

static ChValue type_error(ChVM *vm, const char *who, const char *expected) {
    snprintf(vm->error, sizeof(vm->error), "%s: expected %s", who, expected);
    return CH_UNDEFINED;
}

static int limbs_less_than(const uint64_t *a, const uint64_t *b, size_t len) {
    for (size_t i = len; i > 0; i--) {
        size_t idx = i - 1;
        if (a[idx] < b[idx]) {
            return 1;
        }
        if (a[idx] > b[idx]) {
            return 0;
        }
    }
    return 0;
}

static ChValue random_below(ChVM *vm, ChRandomSource *rs, ChValue bound, const char *who) {
    if (ch_is_fixnum(bound)) {
        int64_t n = ch_to_fixnum(bound);
        if (n <= 0) {
            return type_error(vm, who, "positive integer");
        }
        uint64_t r = ch_random_source_next_u64(rs);
        return ch_make_fixnum((int64_t)(r % (uint64_t)n));
    }
    if (ch_is_bignum(bound)) {
        ChBignum *bn = ch_as_bignum(bound);
        if (!bn->positive || bn->len == 0) {
            return type_error(vm, who, "positive integer");
        }
        size_t n_len = bn->len;
        uint64_t top_limb = bn->limbs[n_len - 1];
        int top_bits = 64 - __builtin_clzll(top_limb);
        uint64_t mask = top_bits >= 64 ? UINT64_MAX : ((1ULL << top_bits) - 1ULL);

        uint64_t stack_buf[16];
        uint64_t *limbs = n_len <= 16 ? stack_buf : (uint64_t *)malloc(n_len * sizeof(uint64_t));
        if (!limbs) {
            snprintf(vm->error, sizeof(vm->error), "%s: out of memory", who);
            return CH_UNDEFINED;
        }

        for (size_t attempt = 0; attempt < 1000; attempt++) {
            for (size_t i = 0; i < n_len; i++) {
                limbs[i] = ch_random_source_next_u64(rs);
            }
            limbs[n_len - 1] &= mask;
            if (limbs_less_than(limbs, bn->limbs, n_len)) {
                size_t actual_len = n_len;
                while (actual_len > 0 && limbs[actual_len - 1] == 0) {
                    actual_len--;
                }
                ChValue result;
                if (actual_len == 0) {
                    result = ch_make_fixnum(0);
                } else if (actual_len == 1 && limbs[0] <= (uint64_t)CH_FIXNUM_MAX) {
                    result = ch_make_fixnum((int64_t)limbs[0]);
                } else {
                    result = ch_gc_make_bignum_from_limbs(&vm->gc, limbs, actual_len, 1);
                }
                if (limbs != stack_buf) {
                    free(limbs);
                }
                return result;
            }
        }
        if (limbs != stack_buf) {
            free(limbs);
        }
        snprintf(vm->error, sizeof(vm->error), "%s: out of memory", who);
        return CH_UNDEFINED;
    }
    return type_error(vm, who, "integer");
}

static double open_unit_real(ChRandomSource *rs) {
    double x = 0.0;
    do {
        uint64_t bits = ch_random_source_next_u64(rs);
        bits &= (1ULL << 53) - 1ULL;
        x = (double)bits / (double)(1ULL << 53);
    } while (x == 0.0);
    return x;
}

static uint64_t int_to_seed_u64(ChVM *vm, const char *who, ChValue v) {
    if (ch_is_fixnum(v)) {
        int64_t n = ch_to_fixnum(v);
        if (n < 0) {
            type_error(vm, who, "non-negative integer");
            return 0;
        }
        return (uint64_t)n;
    }
    if (ch_is_bignum(v)) {
        ChBignum *bn = ch_as_bignum(v);
        if (!bn->positive) {
            type_error(vm, who, "non-negative integer");
            return 0;
        }
        uint64_t result = 0;
        for (size_t i = 0; i < bn->len; i++) {
            result ^= bn->limbs[i];
        }
        return result;
    }
    type_error(vm, who, "integer");
    return 0;
}

static ChValue prim_random_integer(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (vm->default_random_source == CH_UNDEFINED) {
        vm->default_random_source = ch_gc_make_random_source(&vm->gc, fresh_seed());
    }
    ChRandomSource *rs = require_rs(vm, vm->default_random_source, "random-integer");
    if (!rs) {
        return CH_UNDEFINED;
    }
    return random_below(vm, rs, args[0], "random-integer");
}

static ChValue prim_random_real(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    if (vm->default_random_source == CH_UNDEFINED) {
        vm->default_random_source = ch_gc_make_random_source(&vm->gc, fresh_seed());
    }
    ChRandomSource *rs = require_rs(vm, vm->default_random_source, "random-real");
    if (!rs) {
        return CH_UNDEFINED;
    }
    return ch_make_flonum(open_unit_real(rs));
}

static ChValue prim_default_random_source(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    if (vm->default_random_source == CH_UNDEFINED) {
        vm->default_random_source = ch_gc_make_random_source(&vm->gc, fresh_seed());
    }
    return vm->default_random_source;
}

static ChValue prim_random_source_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_random_source(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_make_random_source(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return ch_gc_make_random_source(&vm->gc, 0);
}

static ChValue prim_random_source_randomize(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChRandomSource *rs = require_rs(vm, args[0], "random-source-randomize!");
    if (!rs) {
        return CH_UNDEFINED;
    }
    ch_random_source_seed(rs, fresh_seed());
    return CH_VOID;
}

static ChValue prim_random_source_pseudo_randomize(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChRandomSource *rs = require_rs(vm, args[0], "random-source-pseudo-randomize!");
    if (!rs) {
        return CH_UNDEFINED;
    }
    uint64_t i = int_to_seed_u64(vm, "random-source-pseudo-randomize!", args[1]);
    uint64_t j = int_to_seed_u64(vm, "random-source-pseudo-randomize!", args[2]);
    if (vm->error[0]) {
        return CH_UNDEFINED;
    }
    ch_random_source_seed(rs, i * 2654435761ULL + j * 2246822519ULL);
    return CH_VOID;
}

static int state_word_to_u64(ChValue v, uint64_t *out) {
    if (ch_is_fixnum(v)) {
        *out = (uint64_t)ch_to_fixnum(v);
        return 1;
    }
    if (ch_is_bignum(v)) {
        ChBignum *bn = ch_as_bignum(v);
        if (bn->len == 0) {
            *out = 0;
            return 1;
        }
        if (bn->len > 1) {
            return 0;
        }
        *out = bn->limbs[0];
        return 1;
    }
    return 0;
}

static ChValue prim_random_source_state_ref(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChRandomSource *rs = require_rs(vm, args[0], "random-source-state-ref");
    if (!rs) {
        return CH_UNDEFINED;
    }
    ChValue result = CH_NIL;
    ch_gc_push(&vm->gc, &result);
    for (int i = 3; i >= 0; i--) {
        ChValue word = ch_make_fixnum((int64_t)rs->s[i]);
        ch_gc_push(&vm->gc, &word);
        result = ch_gc_cons(&vm->gc, word, result);
        ch_gc_pop(&vm->gc);
    }
    ch_gc_pop(&vm->gc);
    return result;
}

static ChValue prim_random_source_state_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChRandomSource *rs = require_rs(vm, args[0], "random-source-state-set!");
    if (!rs) {
        return CH_UNDEFINED;
    }
    ChValue state = args[1];
    uint64_t new_state[4];
    for (int i = 0; i < 4; i++) {
        if (!ch_is_pair(state)) {
            return type_error(vm, "random-source-state-set!", "list of 4 integers");
        }
        uint64_t word = 0;
        if (!state_word_to_u64(ch_car(state), &word)) {
            return type_error(vm, "random-source-state-set!", "integer");
        }
        new_state[i] = word;
        state = ch_cdr(state);
    }
    if (new_state[0] == 0 && new_state[1] == 0 && new_state[2] == 0 && new_state[3] == 0) {
        return type_error(vm, "random-source-state-set!", "non-all-zero state");
    }
    memcpy(rs->s, new_state, sizeof(new_state));
    return CH_VOID;
}

static ChValue prim_rs_next_int(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChRandomSource *rs = require_rs(vm, args[0], "%rs-next-int");
    if (!rs) {
        return CH_UNDEFINED;
    }
    return random_below(vm, rs, args[1], "%rs-next-int");
}

static ChValue prim_rs_next_real(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChRandomSource *rs = require_rs(vm, args[0], "%rs-next-real");
    if (!rs) {
        return CH_UNDEFINED;
    }
    return ch_make_flonum(open_unit_real(rs));
}

void ch_random_init_default_source(ChVM *vm) {
    if (vm->default_random_source == CH_UNDEFINED) {
        vm->default_random_source = ch_gc_make_random_source(&vm->gc, fresh_seed());
    }
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, "default-random-source");
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ch_vm_define_global(vm, idx, vm->default_random_source);
}

void ch_register_random_primitives(ChVM *vm) {
    define_prim(vm, "random-integer", prim_random_integer, 1, 1);
    define_prim(vm, "random-real", prim_random_real, 0, 0);
    define_prim(vm, "%default-random-source", prim_default_random_source, 0, 0);
    define_prim(vm, "random-source?", prim_random_source_p, 1, 1);
    define_prim(vm, "make-random-source", prim_make_random_source, 0, 0);
    define_prim(vm, "random-source-randomize!", prim_random_source_randomize, 1, 1);
    define_prim(vm, "random-source-pseudo-randomize!", prim_random_source_pseudo_randomize, 3, 3);
    define_prim(vm, "random-source-state-ref", prim_random_source_state_ref, 1, 1);
    define_prim(vm, "random-source-state-set!", prim_random_source_state_set, 2, 2);
    define_prim(vm, "%rs-next-int", prim_rs_next_int, 2, 2);
    define_prim(vm, "%rs-next-real", prim_rs_next_real, 1, 1);
    ch_random_init_default_source(vm);
}
