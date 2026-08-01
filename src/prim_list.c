#include "chaaya/prim.h"

#include <stdio.h>
#include <string.h>

#define CH_APPLY_MAX_ARGS 256

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static int list_length(ChValue lst, int *ok) {
    int n = 0;
    ChValue p = lst;
    while (ch_is_pair(p)) {
        n++;
        p = ch_cdr(p);
        if (n > 1000000) {
            *ok = 0;
            return -1;
        }
    }
    *ok = ch_is_nil(p);
    return n;
}

static ChValue prim_apply(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "apply: expected at least 2 arguments");
        return CH_UNDEFINED;
    }
    ChValue proc = args[0];
    if (!ch_is_procedure(proc)) {
        snprintf(vm->error, sizeof(vm->error), "apply: not a procedure");
        return CH_UNDEFINED;
    }

    ChValue flat[CH_APPLY_MAX_ARGS];
    int nflat = 0;
    for (int i = 1; i < nargs - 1; i++) {
        if (nflat >= CH_APPLY_MAX_ARGS) {
            snprintf(vm->error, sizeof(vm->error), "apply: too many arguments");
            return CH_UNDEFINED;
        }
        flat[nflat++] = args[i];
    }
    ChValue last = args[nargs - 1];
    while (ch_is_pair(last)) {
        if (nflat >= CH_APPLY_MAX_ARGS) {
            snprintf(vm->error, sizeof(vm->error), "apply: too many arguments");
            return CH_UNDEFINED;
        }
        flat[nflat++] = ch_car(last);
        last = ch_cdr(last);
    }
    if (!ch_is_nil(last)) {
        snprintf(vm->error, sizeof(vm->error), "apply: last argument not a proper list");
        return CH_UNDEFINED;
    }

    ChValue result = CH_VOID;
    ChVMStatus st = ch_vm_apply(vm, proc, flat, nflat, &result);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    return ch_coerce_single(result);
}

static ChValue prim_length(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    int ok = 0;
    int n = list_length(args[0], &ok);
    if (!ok) {
        snprintf(vm->error, sizeof(vm->error), "length: not a proper list");
        return CH_UNDEFINED;
    }
    return ch_make_fixnum(n);
}

static ChValue prim_list_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    int ok = 0;
    (void)list_length(args[0], &ok);
    return ok ? CH_TRUE : CH_FALSE;
}

static ChValue prim_append(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 0) {
        return CH_NIL;
    }
    ChValue result = args[nargs - 1];
    ch_gc_push(&vm->gc, &result);
    for (int i = nargs - 2; i >= 0; i--) {
        ChValue lst = args[i];
        /* copy lst onto result */
        ChValue parts[256];
        int n = 0;
        while (ch_is_pair(lst)) {
            if (n >= 256) {
                ch_gc_pop(&vm->gc);
                snprintf(vm->error, sizeof(vm->error), "append: list too long");
                return CH_UNDEFINED;
            }
            parts[n++] = ch_car(lst);
            lst = ch_cdr(lst);
        }
        if (!ch_is_nil(lst)) {
            ch_gc_pop(&vm->gc);
            snprintf(vm->error, sizeof(vm->error), "append: not a proper list");
            return CH_UNDEFINED;
        }
        for (int j = n - 1; j >= 0; j--) {
            ChValue item = parts[j];
            ch_gc_push(&vm->gc, &item);
            result = ch_gc_cons(&vm->gc, item, result);
            ch_gc_pop(&vm->gc);
        }
    }
    ch_gc_pop(&vm->gc);
    return result;
}

static ChValue prim_reverse(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue lst = args[0];
    ChValue acc = CH_NIL;
    ch_gc_push(&vm->gc, &acc);
    while (ch_is_pair(lst)) {
        ChValue item = ch_car(lst);
        ch_gc_push(&vm->gc, &item);
        acc = ch_gc_cons(&vm->gc, item, acc);
        ch_gc_pop(&vm->gc);
        lst = ch_cdr(lst);
    }
    if (!ch_is_nil(lst)) {
        ch_gc_pop(&vm->gc);
        snprintf(vm->error, sizeof(vm->error), "reverse: not a proper list");
        return CH_UNDEFINED;
    }
    ch_gc_pop(&vm->gc);
    return acc;
}

static ChValue prim_list_ref(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "list-ref: index not an integer");
        return CH_UNDEFINED;
    }
    int64_t k = ch_to_fixnum(args[1]);
    if (k < 0) {
        snprintf(vm->error, sizeof(vm->error), "list-ref: negative index");
        return CH_UNDEFINED;
    }
    ChValue lst = args[0];
    for (int64_t i = 0; i < k; i++) {
        if (!ch_is_pair(lst)) {
            snprintf(vm->error, sizeof(vm->error), "list-ref: index out of range");
            return CH_UNDEFINED;
        }
        lst = ch_cdr(lst);
    }
    if (!ch_is_pair(lst)) {
        snprintf(vm->error, sizeof(vm->error), "list-ref: index out of range");
        return CH_UNDEFINED;
    }
    return ch_car(lst);
}

static ChValue prim_list_tail(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "list-tail: index not an integer");
        return CH_UNDEFINED;
    }
    int64_t k = ch_to_fixnum(args[1]);
    if (k < 0) {
        snprintf(vm->error, sizeof(vm->error), "list-tail: negative index");
        return CH_UNDEFINED;
    }
    ChValue lst = args[0];
    for (int64_t i = 0; i < k; i++) {
        if (!ch_is_pair(lst)) {
            snprintf(vm->error, sizeof(vm->error), "list-tail: index out of range");
            return CH_UNDEFINED;
        }
        lst = ch_cdr(lst);
    }
    return lst;
}

static ChValue prim_list_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "list-set!: index not an integer");
        return CH_UNDEFINED;
    }
    int64_t k = ch_to_fixnum(args[1]);
    if (k < 0) {
        snprintf(vm->error, sizeof(vm->error), "list-set!: negative index");
        return CH_UNDEFINED;
    }
    ChValue lst = args[0];
    int64_t i = 0;
    while (ch_is_pair(lst)) {
        if (ch_object_is_immutable(ch_to_object(lst))) {
            snprintf(vm->error, sizeof(vm->error), "list-set!: immutable pair");
            return CH_UNDEFINED;
        }
        if (i == k) {
            ch_set_car(lst, args[2]);
            return CH_VOID;
        }
        i++;
        lst = ch_cdr(lst);
    }
    snprintf(vm->error, sizeof(vm->error), "list-set!: index out of range");
    return CH_UNDEFINED;
}

static ChValue prim_list_copy(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue src = args[0];
    if (ch_is_nil(src)) {
        return CH_NIL;
    }
    if (!ch_is_pair(src)) {
        return src;
    }

    ChValue head = CH_NIL;
    ChValue tail = CH_NIL;
    ch_gc_push(&vm->gc, &head);
    ch_gc_push(&vm->gc, &tail);
    size_t steps = 0;
    while (ch_is_pair(src)) {
        if (steps++ > 1000000) {
            ch_gc_pop_n(&vm->gc, 2);
            snprintf(vm->error, sizeof(vm->error), "list-copy: list too long");
            return CH_UNDEFINED;
        }
        ChValue cell = ch_gc_cons(&vm->gc, ch_car(src), CH_NIL);
        if (ch_is_nil(head)) {
            head = cell;
        } else {
            ch_set_cdr(tail, cell);
        }
        tail = cell;
        src = ch_cdr(src);
    }
    if (!ch_is_nil(src)) {
        ch_set_cdr(tail, src);
    }
    ch_gc_pop_n(&vm->gc, 2);
    return head;
}

static ChValue prim_caar(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_pair(args[0]) || !ch_is_pair(ch_car(args[0]))) {
        snprintf(vm->error, sizeof(vm->error), "caar: bad argument");
        return CH_UNDEFINED;
    }
    return ch_car(ch_car(args[0]));
}

static ChValue prim_cadr(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_pair(args[0]) || !ch_is_pair(ch_cdr(args[0]))) {
        snprintf(vm->error, sizeof(vm->error), "cadr: bad argument");
        return CH_UNDEFINED;
    }
    return ch_car(ch_cdr(args[0]));
}

static ChValue prim_cdar(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_pair(args[0]) || !ch_is_pair(ch_car(args[0]))) {
        snprintf(vm->error, sizeof(vm->error), "cdar: bad argument");
        return CH_UNDEFINED;
    }
    return ch_cdr(ch_car(args[0]));
}

static ChValue prim_cddr(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_pair(args[0]) || !ch_is_pair(ch_cdr(args[0]))) {
        snprintf(vm->error, sizeof(vm->error), "cddr: bad argument");
        return CH_UNDEFINED;
    }
    return ch_cdr(ch_cdr(args[0]));
}

static ChValue mem_common(ChVM *vm, ChValue obj, ChValue lst, int mode) {
    while (ch_is_pair(lst)) {
        ChValue x = ch_car(lst);
        int match = 0;
        if (mode == 0) {
            match = ch_eq(obj, x);
        } else if (mode == 1) {
            match = ch_eqv(obj, x);
        } else {
            match = ch_equal(obj, x);
        }
        if (match) {
            return lst;
        }
        lst = ch_cdr(lst);
    }
    (void)vm;
    return CH_FALSE;
}

static ChValue prim_memq(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return mem_common(vm, args[0], args[1], 0);
}
static ChValue prim_memv(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return mem_common(vm, args[0], args[1], 1);
}
static ChValue prim_member(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return mem_common(vm, args[0], args[1], 2);
}

static ChValue ass_common(ChVM *vm, ChValue obj, ChValue alist, int mode) {
    while (ch_is_pair(alist)) {
        ChValue cell = ch_car(alist);
        if (ch_is_pair(cell)) {
            ChValue key = ch_car(cell);
            int match = mode == 0 ? ch_eq(obj, key) : mode == 1 ? ch_eqv(obj, key) : ch_equal(obj, key);
            if (match) {
                return cell;
            }
        }
        alist = ch_cdr(alist);
    }
    (void)vm;
    return CH_FALSE;
}

static ChValue prim_assq(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return ass_common(vm, args[0], args[1], 0);
}
static ChValue prim_assv(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return ass_common(vm, args[0], args[1], 1);
}
static ChValue prim_assoc(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return ass_common(vm, args[0], args[1], 2);
}

static ChValue prim_map(ChVM *vm, ChValue *args, int nargs) {
    if (nargs != 2) {
        snprintf(vm->error, sizeof(vm->error), "map: expected procedure and one list (bootstrap)");
        return CH_UNDEFINED;
    }
    ChValue proc = args[0];
    ChValue lst = args[1];
    if (!ch_is_procedure(proc)) {
        snprintf(vm->error, sizeof(vm->error), "map: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue acc = CH_NIL;
    ch_gc_push(&vm->gc, &acc);
    ChValue tmp[64];
    int n = 0;
    while (ch_is_pair(lst)) {
        if (n >= 64) {
            ch_gc_pop(&vm->gc);
            snprintf(vm->error, sizeof(vm->error), "map: list too long (bootstrap limit)");
            return CH_UNDEFINED;
        }
        ChValue a = ch_car(lst);
        ChValue r = CH_VOID;
        ChVMStatus st = ch_vm_apply(vm, proc, &a, 1, &r);
        if (st == CH_VM_CONTINUATION_INVOKED) {
            ch_gc_pop(&vm->gc);
            vm->continuation_invoked = true;
            return CH_UNDEFINED;
        }
        if (st != CH_VM_OK) {
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        tmp[n++] = ch_coerce_single(r);
        lst = ch_cdr(lst);
    }
    if (!ch_is_nil(lst)) {
        ch_gc_pop(&vm->gc);
        snprintf(vm->error, sizeof(vm->error), "map: not a proper list");
        return CH_UNDEFINED;
    }
    for (int i = n - 1; i >= 0; i--) {
        ChValue item = tmp[i];
        ch_gc_push(&vm->gc, &item);
        acc = ch_gc_cons(&vm->gc, item, acc);
        ch_gc_pop(&vm->gc);
    }
    ch_gc_pop(&vm->gc);
    return acc;
}

static ChValue prim_for_each(ChVM *vm, ChValue *args, int nargs) {
    if (nargs != 2) {
        snprintf(vm->error, sizeof(vm->error), "for-each: expected procedure and one list (bootstrap)");
        return CH_UNDEFINED;
    }
    ChValue proc = args[0];
    ChValue lst = args[1];
    if (!ch_is_procedure(proc)) {
        snprintf(vm->error, sizeof(vm->error), "for-each: not a procedure");
        return CH_UNDEFINED;
    }
    while (ch_is_pair(lst)) {
        ChValue a = ch_car(lst);
        ChValue r = CH_VOID;
        ChVMStatus st = ch_vm_apply(vm, proc, &a, 1, &r);
        if (st == CH_VM_CONTINUATION_INVOKED) {
            vm->continuation_invoked = true;
            return CH_UNDEFINED;
        }
        if (st != CH_VM_OK) {
            return CH_UNDEFINED;
        }
        lst = ch_cdr(lst);
    }
    if (!ch_is_nil(lst)) {
        snprintf(vm->error, sizeof(vm->error), "for-each: not a proper list");
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

void ch_register_list_primitives(ChVM *vm) {
    define_prim(vm, "apply", prim_apply, -1, 2);
    define_prim(vm, "length", prim_length, 1, 1);
    define_prim(vm, "list?", prim_list_p, 1, 1);
    define_prim(vm, "append", prim_append, -1, 0);
    define_prim(vm, "reverse", prim_reverse, 1, 1);
    define_prim(vm, "list-ref", prim_list_ref, 2, 2);
    define_prim(vm, "list-tail", prim_list_tail, 2, 2);
    define_prim(vm, "list-set!", prim_list_set, 3, 3);
    define_prim(vm, "list-copy", prim_list_copy, 1, 1);
    define_prim(vm, "caar", prim_caar, 1, 1);
    define_prim(vm, "cadr", prim_cadr, 1, 1);
    define_prim(vm, "cdar", prim_cdar, 1, 1);
    define_prim(vm, "cddr", prim_cddr, 1, 1);
    define_prim(vm, "memq", prim_memq, 2, 2);
    define_prim(vm, "memv", prim_memv, 2, 2);
    define_prim(vm, "member", prim_member, 2, 2);
    define_prim(vm, "assq", prim_assq, 2, 2);
    define_prim(vm, "assv", prim_assv, 2, 2);
    define_prim(vm, "assoc", prim_assoc, 2, 2);
    define_prim(vm, "map", prim_map, -1, 2);
    define_prim(vm, "for-each", prim_for_each, -1, 2);
}
