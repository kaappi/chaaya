#ifndef CHAAYA_TEST_HELPERS_H
#define CHAAYA_TEST_HELPERS_H

#include "chaaya/compiler.h"
#include "chaaya/reader.h"
#include "chaaya/vm.h"

#include <stdio.h>
#include <string.h>

static inline int ch_test_eval(ChVM *vm, const char *src, ChValue *out) {
    ChReader reader;
    ch_reader_init(&reader, &vm->gc, src, strlen(src));
    ChValue expr = CH_NIL;
    ch_gc_push(&vm->gc, &expr);
    if (ch_read_datum(&reader, &expr) != CH_READ_OK) {
        fprintf(stderr, "read: %s\n  in: %s\n", ch_reader_error(&reader), src);
        ch_gc_pop(&vm->gc);
        return 0;
    }
    ChCompiler compiler;
    ch_compiler_init(&compiler, vm);
    ChFunction *fn = NULL;
    if (ch_compile_toplevel(&compiler, expr, &fn) != CH_COMPILE_OK) {
        fprintf(stderr, "compile: %s\n  in: %s\n", ch_compiler_error(&compiler), src);
        ch_gc_pop(&vm->gc);
        return 0;
    }
    vm->error[0] = '\0';
    ChVMStatus st = ch_vm_eval_function(vm, fn, out);
    ch_gc_pop(&vm->gc);
    if (st != CH_VM_OK) {
        fprintf(stderr, "runtime: %s\n  in: %s\n", ch_vm_error(vm), src);
        return 0;
    }
    if (vm->error[0] != '\0' && *out == CH_UNDEFINED) {
        fprintf(stderr, "runtime: %s\n  in: %s\n", ch_vm_error(vm), src);
        return 0;
    }
    return 1;
}

static inline int ch_test_eval_all(ChVM *vm, const char *src, ChValue *out) {
    ChReader reader;
    ch_reader_init(&reader, &vm->gc, src, strlen(src));
    ChValue last = CH_VOID;
    ch_gc_push(&vm->gc, &last);
    for (;;) {
        ChValue expr = CH_NIL;
        ch_gc_push(&vm->gc, &expr);
        ChReadStatus rs = ch_read_datum(&reader, &expr);
        if (rs == CH_READ_EOF) {
            ch_gc_pop(&vm->gc); /* expr */
            break;
        }
        if (rs != CH_READ_OK) {
            fprintf(stderr, "read: %s\n  in: %s\n", ch_reader_error(&reader), src);
            ch_gc_pop_n(&vm->gc, 2);
            return 0;
        }
        ChCompiler compiler;
        ch_compiler_init(&compiler, vm);
        ChFunction *fn = NULL;
        if (ch_compile_toplevel(&compiler, expr, &fn) != CH_COMPILE_OK) {
            fprintf(stderr, "compile: %s\n  in: %s\n", ch_compiler_error(&compiler), src);
            ch_gc_pop_n(&vm->gc, 2);
            return 0;
        }
        vm->error[0] = '\0';
        ChVMStatus st = ch_vm_eval_function(vm, fn, &last);
        ch_gc_pop(&vm->gc); /* expr */
        if (st != CH_VM_OK || (vm->error[0] != '\0' && last == CH_UNDEFINED)) {
            fprintf(stderr, "runtime: %s\n  in: %s\n", ch_vm_error(vm), src);
            ch_gc_pop(&vm->gc);
            return 0;
        }
    }
    *out = last;
    ch_gc_pop(&vm->gc);
    return 1;
}

static inline int ch_test_expect_fixnum(ChVM *vm, const char *src, int64_t want) {
    ChValue v = CH_NIL;
    if (!ch_test_eval(vm, src, &v)) {
        return 0;
    }
    if (!ch_is_fixnum(v) || ch_to_fixnum(v) != want) {
        fprintf(stderr, "expected fixnum %lld, got something else\n  in: %s\n", (long long)want,
                src);
        return 0;
    }
    return 1;
}

static inline int ch_test_expect_bool(ChVM *vm, const char *src, bool want_true) {
    ChValue v = CH_NIL;
    if (!ch_test_eval(vm, src, &v)) {
        return 0;
    }
    bool got = ch_is_true_value(v);
    /* For predicates we want exact #t / #f */
    if (want_true) {
        if (v != CH_TRUE) {
            fprintf(stderr, "expected #t\n  in: %s\n", src);
            return 0;
        }
    } else if (v != CH_FALSE) {
        fprintf(stderr, "expected #f\n  in: %s\n", src);
        return 0;
    }
    (void)got;
    return 1;
}

static inline int ch_test_expect_flonum(ChVM *vm, const char *src, double want, double eps) {
    ChValue v = CH_NIL;
    if (!ch_test_eval(vm, src, &v)) {
        return 0;
    }
    if (!ch_is_flonum(v)) {
        fprintf(stderr, "expected flonum\n  in: %s\n", src);
        return 0;
    }
    double got = ch_to_flonum(v);
    double d = got - want;
    if (d < 0) {
        d = -d;
    }
    if (d > eps) {
        fprintf(stderr, "expected %g (±%g), got %g\n  in: %s\n", want, eps, got, src);
        return 0;
    }
    return 1;
}

static inline int ch_test_expect_equal(ChVM *vm, const char *src, const char *want_src) {
    ChValue got = CH_NIL;
    ChValue want = CH_NIL;
    ch_gc_push(&vm->gc, &got);
    ch_gc_push(&vm->gc, &want);
    if (!ch_test_eval(vm, src, &got)) {
        ch_gc_pop_n(&vm->gc, 2);
        return 0;
    }
    ChReader r;
    ch_reader_init(&r, &vm->gc, want_src, strlen(want_src));
    if (ch_read_datum(&r, &want) != CH_READ_OK) {
        fprintf(stderr, "want read failed: %s\n", want_src);
        ch_gc_pop_n(&vm->gc, 2);
        return 0;
    }
    if (!ch_equal(got, want)) {
        fprintf(stderr, "equal? failed\n  got src: %s\n  want: %s\n", src, want_src);
        ch_gc_pop_n(&vm->gc, 2);
        return 0;
    }
    ch_gc_pop_n(&vm->gc, 2);
    return 1;
}

#define CH_CHECK(expr)                                                                             \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#endif /* CHAAYA_TEST_HELPERS_H */
