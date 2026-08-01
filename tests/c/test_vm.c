#include "test_helpers.h"

#include <stdio.h>

int main(void) {
    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);

    CH_CHECK(ch_test_expect_fixnum(&vm, "(+ 1 2 3)", 6));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(+)", 0));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(* 6 7)", 42));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(*)", 1));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(- 10 3)", 7));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(- 5)", -5));
    CH_CHECK(ch_test_expect_flonum(&vm, "(/ 1 2)", 0.5, 1e-12));

    CH_CHECK(ch_test_expect_fixnum(&vm, "(if #t 1 2)", 1));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(if #f 1 2)", 2));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(if 0 1 2)", 1));
    {
        ChValue v = CH_NIL;
        CH_CHECK(ch_test_eval(&vm, "(if #f 1)", &v));
        if (v != CH_FALSE) {
            fprintf(stderr, "(if #f 1) should be #f\n");
            return 1;
        }
    }

    CH_CHECK(ch_test_expect_fixnum(&vm, "(and 1 2 3)", 3));
    CH_CHECK(ch_test_expect_bool(&vm, "(and 1 #f 3)", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(and)", true));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(or #f #f 9)", 9));
    CH_CHECK(ch_test_expect_bool(&vm, "(or)", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(or #f #f)", false));

    CH_CHECK(ch_test_expect_fixnum(&vm, "((lambda (x) (+ x 1)) 41)", 42));
    CH_CHECK(ch_test_expect_fixnum(&vm, "((lambda (a b) (* a b)) 6 7)", 42));
    CH_CHECK(ch_test_expect_fixnum(&vm, "((lambda x (car x)) 1 2 3)", 1));
    CH_CHECK(ch_test_expect_fixnum(&vm, "((lambda (a . rest) (car rest)) 1 2 3)", 2));

    CH_CHECK(ch_test_expect_fixnum(&vm, "(let ((x 10) (y 32)) (+ x y))", 42));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(let ((x 1)) (let ((x 2)) x))", 2));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(let ((x 1)) (let ((y 2)) (+ x y)))", 3));

    CH_CHECK(ch_test_expect_fixnum(
        &vm, "(begin (define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (fact 5))", 120));
    CH_CHECK(ch_test_expect_fixnum(
        &vm,
        "(begin (define (make-adder n) (lambda (x) (+ x n))) ((make-adder 40) 2))", 42));
    CH_CHECK(ch_test_expect_fixnum(
        &vm,
        "(begin (define (make-counter) (let ((n 0)) (lambda () (set! n (+ n 1)) n))) "
        "(define c (make-counter)) (c) (c) (c))",
        3));

    CH_CHECK(ch_test_expect_fixnum(
        &vm,
        "(begin "
        "(define (even? n) (if (= n 0) #t (odd? (- n 1)))) "
        "(define (odd? n) (if (= n 0) #f (even? (- n 1)))) "
        "(if (even? 10) 1 0))",
        1));

    CH_CHECK(ch_test_expect_fixnum(
        &vm, "(begin (define (sum n) (if (= n 0) 0 (+ n (sum (- n 1))))) (sum 20))", 210));

    CH_CHECK(ch_test_expect_fixnum(
        &vm,
        "(begin (define (loop n acc) (if (= n 0) acc (loop (- n 1) (+ acc 1)))) (loop 500 0))",
        500));

    CH_CHECK(ch_test_expect_fixnum(&vm, "(car (cons 1 2))", 1));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(cdr (cons 1 2))", 2));
    CH_CHECK(ch_test_expect_equal(&vm, "(list 1 2 3)", "(1 2 3)"));
    CH_CHECK(ch_test_expect_equal(&vm, "(quote (a b))", "(a b)"));
    CH_CHECK(ch_test_expect_bool(&vm, "(equal? (list 1 2) (list 1 2))", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(eq? (quote a) (quote a))", true));

    CH_CHECK(ch_test_expect_fixnum(&vm, "(begin (define x 10) (set! x (+ x 1)) x)", 11));

    CH_CHECK(ch_test_expect_bool(&vm, "(not #f)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(not 1)", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(< 1 2 3)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(< 1 3 2)", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(= 2 2 2)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(>= 3 2 2)", true));

    ch_vm_deinit(&vm);
    printf("ok\n");
    return 0;
}
