#include "chaaya/compiler.h"
#include "chaaya/reader.h"
#include "chaaya/vm.h"

#include <stdio.h>
#include <string.h>

static int eval_expr(ChVM *vm, const char *src, ChValue *out) {
    ChReader reader;
    ch_reader_init(&reader, &vm->gc, src, strlen(src));
    ChValue expr = CH_NIL;
    ch_gc_push(&vm->gc, &expr);
    if (ch_read_datum(&reader, &expr) != CH_READ_OK) {
        fprintf(stderr, "read: %s\n", ch_reader_error(&reader));
        ch_gc_pop(&vm->gc);
        return 0;
    }
    ChCompiler compiler;
    ch_compiler_init(&compiler, vm);
    ChFunction *fn = NULL;
    if (ch_compile_toplevel(&compiler, expr, &fn) != CH_COMPILE_OK) {
        fprintf(stderr, "compile: %s\n", ch_compiler_error(&compiler));
        ch_gc_pop(&vm->gc);
        return 0;
    }
    ChVMStatus st = ch_vm_eval_function(vm, fn, out);
    ch_gc_pop(&vm->gc);
    if (st != CH_VM_OK) {
        fprintf(stderr, "runtime: %s\n", ch_vm_error(vm));
        return 0;
    }
    return 1;
}

static int expect_fixnum(ChVM *vm, const char *src, int64_t want) {
    ChValue v = CH_NIL;
    if (!eval_expr(vm, src, &v)) {
        fprintf(stderr, "failed evaluating: %s\n", src);
        return 0;
    }
    if (!ch_is_fixnum(v) || ch_to_fixnum(v) != want) {
        fprintf(stderr, "expected %lld for %s\n", (long long)want, src);
        return 0;
    }
    return 1;
}

int main(void) {
    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);

    if (!expect_fixnum(&vm, "(+ 1 2 3)", 6)) {
        return 1;
    }
    if (!expect_fixnum(&vm, "(* 6 7)", 42)) {
        return 1;
    }
    if (!expect_fixnum(&vm, "(if #t 1 2)", 1)) {
        return 1;
    }
    if (!expect_fixnum(&vm, "(if #f 1 2)", 2)) {
        return 1;
    }
    if (!expect_fixnum(&vm, "((lambda (x) (+ x 1)) 41)", 42)) {
        return 1;
    }
    if (!expect_fixnum(&vm, "(let ((x 10) (y 32)) (+ x y))", 42)) {
        return 1;
    }
    if (!expect_fixnum(&vm, "(begin (define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (fact 5))",
                       120)) {
        return 1;
    }
    if (!expect_fixnum(&vm,
                       "(begin (define (make-adder n) (lambda (x) (+ x n))) "
                       "((make-adder 40) 2))",
                       42)) {
        return 1;
    }
    if (!expect_fixnum(&vm, "(car (cons 1 2))", 1)) {
        return 1;
    }
    if (!expect_fixnum(&vm, "(cdr (cons 1 2))", 2)) {
        return 1;
    }

    ch_vm_deinit(&vm);
    printf("ok\n");
    return 0;
}
