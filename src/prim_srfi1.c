#include "chaaya/prim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SRFI1_MAX_LISTS 64
#define SRFI1_MAX_ELEMS 4096

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static bool is_truthy(ChValue v) {
    return v != CH_FALSE && !ch_is_false(v);
}

static int parse_nonneg(ChVM *vm, ChValue v, int64_t *out, const char *who) {
    if (!ch_is_fixnum(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected exact integer", who);
        return -1;
    }
    int64_t n = ch_to_fixnum(v);
    if (n < 0) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected non-negative integer", who);
        return -1;
    }
    *out = n;
    return 0;
}

static ChValue build_list(ChVM *vm, ChValue *items, size_t count, ChValue tail) {
    ChValue result = tail;
    ch_gc_push(&vm->gc, &result);
    for (size_t i = count; i > 0; i--) {
        result = ch_gc_cons(&vm->gc, items[i - 1], result);
    }
    ch_gc_pop(&vm->gc);
    return result;
}

static int vm_call1(ChVM *vm, ChValue proc, ChValue arg, ChValue *out) {
    ChVMStatus st = ch_vm_apply(vm, proc, &arg, 1, out);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return -1;
    }
    return st == CH_VM_OK ? 0 : -1;
}

static ChValue prim_iota(ChVM *vm, ChValue *args, int nargs) {
    int64_t count = 0;
    int64_t start = 0;
    int64_t step = 1;
    if (nargs < 1 || parse_nonneg(vm, args[0], &count, "iota") != 0) {
        return CH_UNDEFINED;
    }
    if (nargs > 1 && !ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "iota: expected exact integer");
        return CH_UNDEFINED;
    }
    if (nargs > 2 && !ch_is_fixnum(args[2])) {
        snprintf(vm->error, sizeof(vm->error), "iota: expected exact integer");
        return CH_UNDEFINED;
    }
    if (nargs > 1) {
        start = ch_to_fixnum(args[1]);
    }
    if (nargs > 2) {
        step = ch_to_fixnum(args[2]);
    }
    if (count > SRFI1_MAX_ELEMS) {
        snprintf(vm->error, sizeof(vm->error), "iota: list too long");
        return CH_UNDEFINED;
    }
    ChValue items[SRFI1_MAX_ELEMS];
    for (int64_t i = 0; i < count; i++) {
        items[(size_t)i] = ch_make_fixnum(start + i * step);
    }
    return build_list(vm, items, (size_t)count, CH_NIL);
}

static ChValue nth_elem(ChVM *vm, ChValue lst, int n, const char *who) {
    for (int i = 0; i < n; i++) {
        if (!ch_is_pair(lst)) {
            snprintf(vm->error, sizeof(vm->error), "%s: list too short", who);
            return CH_UNDEFINED;
        }
        lst = ch_cdr(lst);
    }
    if (!ch_is_pair(lst)) {
        snprintf(vm->error, sizeof(vm->error), "%s: list too short", who);
        return CH_UNDEFINED;
    }
    return ch_car(lst);
}

static ChValue prim_first(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return nth_elem(vm, args[0], 0, "first");
}

static ChValue prim_second(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return nth_elem(vm, args[0], 1, "second");
}

static ChValue prim_third(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return nth_elem(vm, args[0], 2, "third");
}

static ChValue prim_fourth(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return nth_elem(vm, args[0], 3, "fourth");
}

static ChValue prim_fifth(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return nth_elem(vm, args[0], 4, "fifth");
}

static ChValue prim_sixth(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return nth_elem(vm, args[0], 5, "sixth");
}

static ChValue prim_seventh(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return nth_elem(vm, args[0], 6, "seventh");
}

static ChValue prim_eighth(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return nth_elem(vm, args[0], 7, "eighth");
}

static ChValue prim_ninth(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return nth_elem(vm, args[0], 8, "ninth");
}

static ChValue prim_tenth(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return nth_elem(vm, args[0], 9, "tenth");
}

static ChValue prim_not_pair_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_pair(args[0]) ? CH_FALSE : CH_TRUE;
}

static ChValue prim_null_list_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_nil(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_proper_list_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)vm;
    (void)nargs;
    ChValue slow = args[0];
    ChValue fast = args[0];
    while (ch_is_pair(fast)) {
        fast = ch_cdr(fast);
        if (!ch_is_pair(fast)) {
            return ch_is_nil(fast) ? CH_TRUE : CH_FALSE;
        }
        fast = ch_cdr(fast);
        slow = ch_cdr(slow);
        if (slow == fast) {
            return CH_FALSE;
        }
    }
    return ch_is_nil(slow) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_dotted_list_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    ChValue p = args[0];
    if (ch_is_nil(p)) {
        return CH_FALSE;
    }
    while (ch_is_pair(p)) {
        p = ch_cdr(p);
    }
    return ch_is_nil(p) ? CH_FALSE : CH_TRUE;
}

static ChValue prim_xcons(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return ch_gc_cons(&vm->gc, args[1], args[0]);
}

static ChValue prim_cons_star(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1) {
        snprintf(vm->error, sizeof(vm->error), "cons*: expected at least 1 argument");
        return CH_UNDEFINED;
    }
    ChValue result = args[nargs - 1];
    ch_gc_push(&vm->gc, &result);
    for (int i = nargs - 2; i >= 0; i--) {
        result = ch_gc_cons(&vm->gc, args[i], result);
    }
    ch_gc_pop(&vm->gc);
    return result;
}

static ChValue prim_concatenate(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_pair(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "concatenate: expected list of lists");
        return CH_UNDEFINED;
    }
    ChValue result = CH_NIL;
    ch_gc_push(&vm->gc, &result);
    for (ChValue outer = args[0]; ch_is_pair(outer); outer = ch_cdr(outer)) {
        ChValue lst = ch_car(outer);
        for (; ch_is_pair(lst); lst = ch_cdr(lst)) {
            result = ch_gc_cons(&vm->gc, ch_car(lst), result);
        }
        if (!ch_is_nil(lst)) {
            ch_gc_pop(&vm->gc);
            snprintf(vm->error, sizeof(vm->error), "concatenate: not a proper list");
            return CH_UNDEFINED;
        }
    }
    ChValue rev = CH_NIL;
    ch_gc_push(&vm->gc, &rev);
    for (ChValue p = result; ch_is_pair(p); p = ch_cdr(p)) {
        rev = ch_gc_cons(&vm->gc, ch_car(p), rev);
    }
    ch_gc_pop_n(&vm->gc, 2);
    return rev;
}

static ChValue prim_take(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    int64_t k = 0;
    if (parse_nonneg(vm, args[1], &k, "take") != 0) {
        return CH_UNDEFINED;
    }
    ChValue items[SRFI1_MAX_ELEMS];
    ChValue cur = args[0];
    for (int64_t i = 0; i < k; i++) {
        if (!ch_is_pair(cur)) {
            snprintf(vm->error, sizeof(vm->error), "take: list too short");
            return CH_UNDEFINED;
        }
        items[(size_t)i] = ch_car(cur);
        cur = ch_cdr(cur);
    }
    return build_list(vm, items, (size_t)k, CH_NIL);
}

static ChValue prim_drop(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    int64_t k = 0;
    if (parse_nonneg(vm, args[1], &k, "drop") != 0) {
        return CH_UNDEFINED;
    }
    ChValue cur = args[0];
    for (int64_t i = 0; i < k; i++) {
        if (!ch_is_pair(cur)) {
            snprintf(vm->error, sizeof(vm->error), "drop: list too short");
            return CH_UNDEFINED;
        }
        cur = ch_cdr(cur);
    }
    return cur;
}

static ChValue prim_fold(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 3) {
        snprintf(vm->error, sizeof(vm->error), "fold: expected at least 3 arguments");
        return CH_UNDEFINED;
    }
    ChValue proc = args[0];
    ChValue acc = args[1];
    ch_gc_push(&vm->gc, &acc);
    int list_count = nargs - 2;
    ChValue currents[SRFI1_MAX_LISTS];
    for (int i = 0; i < list_count; i++) {
        currents[i] = args[2 + i];
    }
    while (true) {
        ChValue call_args[SRFI1_MAX_LISTS + 1];
        for (int i = 0; i < list_count; i++) {
            if (!ch_is_pair(currents[i])) {
                ch_gc_pop(&vm->gc);
                return acc;
            }
            call_args[i] = ch_car(currents[i]);
        }
        call_args[list_count] = acc;
        ChValue next = CH_UNDEFINED;
        ChVMStatus st = ch_vm_apply(vm, proc, call_args, list_count + 1, &next);
        if (st != CH_VM_OK) {
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        acc = ch_coerce_single(next);
        for (int i = 0; i < list_count; i++) {
            currents[i] = ch_cdr(currents[i]);
        }
    }
}

static ChValue prim_filter(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue proc = args[0];
    ChValue cur = args[1];
    ChValue items[SRFI1_MAX_ELEMS];
    size_t n = 0;
    while (ch_is_pair(cur)) {
        ChValue elem = ch_car(cur);
        ChValue ok = CH_FALSE;
        if (vm_call1(vm, proc, elem, &ok) != 0) {
            return CH_UNDEFINED;
        }
        if (is_truthy(ok)) {
            if (n >= SRFI1_MAX_ELEMS) {
                snprintf(vm->error, sizeof(vm->error), "filter: list too long");
                return CH_UNDEFINED;
            }
            items[n++] = elem;
        }
        cur = ch_cdr(cur);
    }
    if (!ch_is_nil(cur)) {
        snprintf(vm->error, sizeof(vm->error), "filter: not a proper list");
        return CH_UNDEFINED;
    }
    return build_list(vm, items, n, CH_NIL);
}

static ChValue prim_list_tabulate(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    int64_t n = 0;
    if (parse_nonneg(vm, args[0], &n, "list-tabulate") != 0) {
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "list-tabulate: expected procedure");
        return CH_UNDEFINED;
    }
    if (n > SRFI1_MAX_ELEMS) {
        snprintf(vm->error, sizeof(vm->error), "list-tabulate: list too long");
        return CH_UNDEFINED;
    }
    ChValue items[SRFI1_MAX_ELEMS];
    for (int64_t i = 0; i < n; i++) {
        ChValue idx = ch_make_fixnum(i);
        if (vm_call1(vm, args[1], idx, &items[(size_t)i]) != 0) {
            return CH_UNDEFINED;
        }
    }
    return build_list(vm, items, (size_t)n, CH_NIL);
}

static ChValue prim_length_plus(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!ch_is_pair(args[0])) {
        return ch_make_fixnum(0);
    }
    int64_t n = 0;
    ChValue cur = args[0];
    while (ch_is_pair(cur)) {
        n++;
        cur = ch_cdr(cur);
    }
    return ch_make_fixnum(n);
}

static ChValue prim_unfold(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 4) {
        snprintf(vm->error, sizeof(vm->error), "unfold: expected at least 4 arguments");
        return CH_UNDEFINED;
    }
    ChValue items[SRFI1_MAX_ELEMS];
    size_t n = 0;
    ChValue seed = args[3];
    ch_gc_push(&vm->gc, &seed);
    while (n < SRFI1_MAX_ELEMS) {
        ChValue stop = CH_FALSE;
        if (vm_call1(vm, args[0], seed, &stop) != 0) {
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        if (is_truthy(stop)) {
            break;
        }
        ChValue elem = CH_UNDEFINED;
        if (vm_call1(vm, args[1], seed, &elem) != 0) {
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        items[n++] = elem;
        ChValue next = CH_UNDEFINED;
        if (vm_call1(vm, args[2], seed, &next) != 0) {
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        seed = next;
    }
    ChValue tail = CH_NIL;
    if (nargs > 4) {
        if (vm_call1(vm, args[4], seed, &tail) != 0) {
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
    }
    ChValue out = build_list(vm, items, n, tail);
    ch_gc_pop(&vm->gc);
    return out;
}

void ch_register_srfi1_primitives(ChVM *vm) {
    define_prim(vm, "iota", prim_iota, -1, 1);
    define_prim(vm, "first", prim_first, 1, 1);
    define_prim(vm, "second", prim_second, 1, 1);
    define_prim(vm, "third", prim_third, 1, 1);
    define_prim(vm, "fourth", prim_fourth, 1, 1);
    define_prim(vm, "fifth", prim_fifth, 1, 1);
    define_prim(vm, "sixth", prim_sixth, 1, 1);
    define_prim(vm, "seventh", prim_seventh, 1, 1);
    define_prim(vm, "eighth", prim_eighth, 1, 1);
    define_prim(vm, "ninth", prim_ninth, 1, 1);
    define_prim(vm, "tenth", prim_tenth, 1, 1);
    define_prim(vm, "not-pair?", prim_not_pair_p, 1, 1);
    define_prim(vm, "null-list?", prim_null_list_p, 1, 1);
    define_prim(vm, "proper-list?", prim_proper_list_p, 1, 1);
    define_prim(vm, "dotted-list?", prim_dotted_list_p, 1, 1);
    define_prim(vm, "xcons", prim_xcons, 2, 2);
    define_prim(vm, "cons*", prim_cons_star, -1, 1);
    define_prim(vm, "concatenate", prim_concatenate, 1, 1);
    define_prim(vm, "take", prim_take, 2, 2);
    define_prim(vm, "drop", prim_drop, 2, 2);
    define_prim(vm, "fold", prim_fold, -1, 3);
    define_prim(vm, "filter", prim_filter, 2, 2);
    define_prim(vm, "list-tabulate", prim_list_tabulate, 2, 2);
    define_prim(vm, "length+", prim_length_plus, 1, 1);
    define_prim(vm, "unfold", prim_unfold, -1, 4);
}
