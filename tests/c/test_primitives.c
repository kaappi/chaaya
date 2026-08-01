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
    CH_CHECK(ch_test_expect_bool(&vm, "(procedure? +)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(procedure? 1)", false));
    CH_CHECK(ch_test_expect_bool(&vm, "(procedure? (lambda (x) x))", true));

    /* Mutation */
    CH_CHECK(ch_test_expect_fixnum(
        &vm, "(begin (define p (cons 1 2)) (set-car! p 9) (car p))", 9));
    CH_CHECK(ch_test_expect_fixnum(
        &vm, "(begin (define p (cons 1 2)) (set-cdr! p 8) (cdr p))", 8));

    /* Equality */
    CH_CHECK(ch_test_expect_bool(&vm, "(eq? 1 1)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(eqv? 1.0 1.0)", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(equal? \"ab\" \"ab\")", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(equal? (vector 1 2) (vector 1 2))", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(equal? (list 1 (list 2)) (list 1 (list 2)))", true));
    CH_CHECK(ch_test_expect_bool(&vm, "(equal? (list 1) (list 2))", false));

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
