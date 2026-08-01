#include "test_helpers.h"

#include <stdio.h>

int main(void) {
    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);

    /* Type predicates */
    CH_CHECK(ch_test_expect_bool(&vm, "(pair? (cons 1 2))", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(pair? 1)", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(null? ())", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(null? (list))", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(null? (list 1))", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(symbol? (quote foo))", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(symbol? \"foo\")", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(string? \"foo\")", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(number? 3)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(number? 3.5)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(number? #t)", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(boolean? #t)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(boolean? 0)", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(vector? (vector 1 2))", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(vector? (list 1 2))", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(bytevector? #u8(1 2))", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(bytevector? #(1 2))", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(procedure? +)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(procedure? 1)", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(procedure? (lambda (x) x))", true));

    /* Mutation */
    CH_CHECK(ch_test_expect_fixnum(
        &vm, "(begin (define p (cons 1 2)) (set-car! p 9) (car p))", 9));
    CH_CHECK(ch_test_expect_fixnum(
        &vm, "(begin (define p (cons 1 2)) (set-cdr! p 8) (cdr p))", 8));
    CH_CHECK(ch_test_expect_equal(
        &vm, "(begin (define v (vector 1 2 3)) (vector-set! v 1 9) v)", "#(1 9 3)"));
    {
        ChValue v = CH_NIL;
        if (ch_test_eval(&vm, "(vector-set! '#(1 2 3) 1 9)", &v)) {
            fprintf(stderr, "expected immutable literal vector set! failure\n");
            return 1;
        }
    }
    {
        ChValue v = CH_NIL;
        if (ch_test_eval(&vm, "(set-car! '(1 2 3) 9)", &v)) {
            fprintf(stderr, "expected immutable literal pair set-car! failure\n");
            return 1;
        }
    }
    {
        ChValue v = CH_NIL;
        if (ch_test_eval(&vm, "(set-cdr! '(1 . 2) 9)", &v)) {
            fprintf(stderr, "expected immutable literal pair set-cdr! failure\n");
            return 1;
        }
    }

    /* Equality */
    CH_CHECK(ch_test_expect_bool(&vm, "(eq? 1 1)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(eqv? 1.0 1.0)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(eqv? 2 2.0)", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(eqv? 0.0 -0.0)", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(equal? \"ab\" \"ab\")", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(equal? (vector 1 2) (vector 1 2))", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(equal? (list 1 (list 2)) (list 1 (list 2)))", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(equal? (list 1) (list 2))", false));
    CH_CHECK(ch_test_expect_fixnum(
        &vm,
        "(char->integer (string-ref (list->string (list (integer->char 945) (integer->char 946))) 1))",
        946));
    CH_CHECK(ch_test_expect_fixnum(
        &vm,
        "(string-length (list->string (list (integer->char 945) (integer->char 946))))",
        2));
    {
        ChValue v = CH_NIL;
        if (ch_test_eval(
                &vm,
                "(string-ref (list->string (list (integer->char 945) (integer->char 946))) 2)",
                &v)) {
            fprintf(stderr, "expected string-ref bounds failure\n");
            return 1;
        }
    }

    /* Arithmetic edge cases */
    CH_CHECK(ch_test_expect_fixnum(&vm, "(+ 100 -40)", 60));
    CH_CHECK(ch_test_expect_fixnum(&vm, "(* -2 -3)", 6));
    CH_CHECK(ch_test_expect_bool(&vm, "(> 5 4 3)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(<= 1 1 2)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(=)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(= 7)", true));

    /* list / vector constructors */
    CH_CHECK(ch_test_expect_equal(&vm, "(list)", "()"));
    CH_CHECK(ch_test_expect_equal(&vm, "(list 'a 'b)", "(a b)"));
    CH_CHECK(ch_test_expect_equal(&vm, "(vector)", "#()"));
    CH_CHECK(ch_test_expect_equal(&vm, "(vector 1 'x)", "#(1 x)"));
    CH_CHECK(ch_test_expect_equal(&vm, "(bytevector 1 2 255)", "#u8(1 2 255)"));
    CH_CHECK(ch_test_expect_equal(&vm, "(make-bytevector 3 7)", "#u8(7 7 7)"));
    CH_CHECK(ch_test_expect_equal(
        &vm, "(begin (define bv (bytevector 1 2 3)) (bytevector-u8-set! bv 1 9) bv)", "#u8(1 9 3)"));
    {
        ChValue v = CH_NIL;
        if (ch_test_eval(&vm, "(bytevector-u8-set! #u8(1 2 3) 0 9)", &v)) {
            fprintf(stderr, "expected immutable literal bytevector set! failure\n");
            return 1;
        }
    }
    {
        ChValue v = CH_NIL;
        if (ch_test_eval(&vm, "(bytevector-copy! #u8(1 2 3) 0 (bytevector 4 5 6))", &v)) {
            fprintf(stderr, "expected immutable literal bytevector copy! failure\n");
            return 1;
        }
    }

    /* begin sequencing */
    CH_CHECK(ch_test_expect_fixnum(&vm, "(begin 1 2 3)", 3));
    {
        ChValue v = CH_NIL;
        CH_CHECK(ch_test_eval(&vm, "(begin)", &v));
        if (v != CH_VOID) {
            fprintf(stderr, "(begin) should be void\n");
            return 1;
        }
    }

    /* Unbound variable should fail */
    {
        ChValue v = CH_NIL;
        if (ch_test_eval(&vm, "not-bound-at-all", &v)) {
            fprintf(stderr, "expected unbound error\n");
            return 1;
        }
    }

    /* Wrong arity */
    {
        ChValue v = CH_NIL;
        if (ch_test_eval(&vm, "(car)", &v)) {
            fprintf(stderr, "expected arity error for (car)\n");
            return 1;
        }
    }

    ch_vm_deinit(&vm);
    printf("ok\n");
    return 0;
}
