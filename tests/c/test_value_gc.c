#include "chaaya/gc.h"
#include "chaaya/value.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    return 0;
}

static int test_fixnums(void) {
    if (!ch_is_fixnum(ch_make_fixnum(0))) {
        return fail("0 not fixnum");
    }
    if (ch_to_fixnum(ch_make_fixnum(-1)) != -1) {
        return fail("fixnum -1");
    }
    if (ch_to_fixnum(ch_make_fixnum(140737488355327LL)) != 140737488355327LL) { /* 2^47-1 */
        return fail("fixnum max");
    }
    if (ch_to_fixnum(ch_make_fixnum(-140737488355328LL)) != -140737488355328LL) {
        return fail("fixnum min");
    }
    return 1;
}

static int test_flonums(void) {
    ChValue v = ch_make_flonum(3.5);
    if (!ch_is_flonum(v) || ch_to_flonum(v) != 3.5) {
        return fail("flonum round-trip");
    }
    if (ch_is_fixnum(v) || ch_is_immediate(v) || ch_is_pointer(v)) {
        return fail("flonum exclusive tags");
    }
    return 1;
}

static int test_immediates(void) {
    if (!ch_is_nil(CH_NIL) || !ch_is_immediate(CH_NIL)) {
        return fail("nil");
    }
    if (!ch_is_false(CH_FALSE) || ch_is_true_value(CH_FALSE)) {
        return fail("false");
    }
    if (!ch_is_true_value(CH_TRUE) || !ch_is_true_value(ch_make_fixnum(0))) {
        return fail("true-value");
    }
    ChValue ch = ch_make_char('A');
    if (!ch_is_char(ch) || ch_to_char(ch) != 'A') {
        return fail("char");
    }
    return 1;
}

static int test_equality(ChGC *gc) {
    if (!ch_eq(ch_make_fixnum(1), ch_make_fixnum(1))) {
        return fail("eq fixnum");
    }
    if (!ch_eqv(ch_make_flonum(1.0), ch_make_flonum(1.0))) {
        return fail("eqv flonum");
    }
    ChValue s1 = ch_gc_make_string_cstr(gc, "ab");
    ChValue s2 = ch_gc_make_string_cstr(gc, "ab");
    ch_gc_push(gc, &s1);
    ch_gc_push(gc, &s2);
    if (ch_eq(s1, s2)) {
        ch_gc_pop_n(gc, 2);
        return fail("distinct strings should not be eq?");
    }
    if (!ch_equal(s1, s2)) {
        ch_gc_pop_n(gc, 2);
        return fail("equal strings");
    }
    ChValue p1 = ch_gc_cons(gc, ch_make_fixnum(1), ch_gc_cons(gc, ch_make_fixnum(2), CH_NIL));
    ChValue p2 = ch_gc_cons(gc, ch_make_fixnum(1), ch_gc_cons(gc, ch_make_fixnum(2), CH_NIL));
    ch_gc_push(gc, &p1);
    ch_gc_push(gc, &p2);
    if (!ch_equal(p1, p2)) {
        ch_gc_pop_n(gc, 4);
        return fail("equal lists");
    }
    ChValue v1 = ch_gc_make_vector(gc, 2, CH_FALSE);
    ChValue v2 = ch_gc_make_vector(gc, 2, CH_FALSE);
    ch_as_vector(v1)->items[0] = ch_make_fixnum(9);
    ch_as_vector(v1)->items[1] = ch_make_fixnum(8);
    ch_as_vector(v2)->items[0] = ch_make_fixnum(9);
    ch_as_vector(v2)->items[1] = ch_make_fixnum(8);
    ch_gc_push(gc, &v1);
    ch_gc_push(gc, &v2);
    if (!ch_equal(v1, v2)) {
        ch_gc_pop_n(gc, 6);
        return fail("equal vectors");
    }
    ch_gc_pop_n(gc, 6);
    return 1;
}

static int test_gc_keeps_vector(ChGC *gc) {
    ChValue vec = ch_gc_make_vector(gc, 3, ch_make_fixnum(7));
    ch_gc_push(gc, &vec);
    for (int i = 0; i < 3000; i++) {
        (void)ch_gc_make_string_cstr(gc, "garbage");
    }
    ch_gc_collect(gc);
    if (!ch_is_vector(vec) || ch_as_vector(vec)->len != 3 ||
        ch_to_fixnum(ch_as_vector(vec)->items[0]) != 7) {
        ch_gc_pop(gc);
        return fail("gc dropped vector");
    }
    ch_gc_pop(gc);
    return 1;
}

static int test_symbol_distinct(ChGC *gc) {
    ChValue a = ch_gc_intern_symbol_cstr(gc, "alpha");
    ChValue b = ch_gc_intern_symbol_cstr(gc, "beta");
    if (ch_eq(a, b)) {
        return fail("different symbols eq");
    }
    if (strcmp(ch_as_symbol(a)->name, "alpha") != 0) {
        return fail("symbol name");
    }
    return 1;
}

int main(void) {
    ChGC gc;
    ch_gc_init(&gc);

    if (!test_fixnums() || !test_flonums() || !test_immediates()) {
        return 1;
    }
    if (!test_equality(&gc) || !test_gc_keeps_vector(&gc) || !test_symbol_distinct(&gc)) {
        return 1;
    }

    /* original cons/intern smoke */
    ChValue a = ch_make_fixnum(1);
    ChValue b = ch_make_fixnum(2);
    ch_gc_push(&gc, &a);
    ch_gc_push(&gc, &b);
    ChValue p = ch_gc_cons(&gc, a, b);
    ch_gc_push(&gc, &p);
    if (!ch_is_pair(p) || ch_to_fixnum(ch_car(p)) != 1) {
        fprintf(stderr, "cons failed\n");
        return 1;
    }
    for (int i = 0; i < 2000; i++) {
        (void)ch_gc_cons(&gc, ch_make_fixnum(i), CH_NIL);
    }
    ch_gc_collect(&gc);
    if (!ch_is_pair(p) || ch_to_fixnum(ch_car(p)) != 1) {
        fprintf(stderr, "gc collected live pair\n");
        return 1;
    }
    ch_gc_pop_n(&gc, 3);

    ch_gc_deinit(&gc);
    printf("ok\n");
    return 0;
}
