#include "chaaya/gc.h"
#include "chaaya/value.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    ChGC gc;
    ch_gc_init(&gc);

    ChValue a = ch_make_fixnum(1);
    ChValue b = ch_make_fixnum(2);
    ch_gc_push(&gc, &a);
    ch_gc_push(&gc, &b);
    ChValue p = ch_gc_cons(&gc, a, b);
    ch_gc_push(&gc, &p);

    if (!ch_is_pair(p) || ch_to_fixnum(ch_car(p)) != 1 || ch_to_fixnum(ch_cdr(p)) != 2) {
        fprintf(stderr, "cons failed\n");
        return 1;
    }

    ChValue sym1 = ch_gc_intern_symbol_cstr(&gc, "foo");
    ChValue sym2 = ch_gc_intern_symbol_cstr(&gc, "foo");
    if (!ch_eq(sym1, sym2)) {
        fprintf(stderr, "intern failed\n");
        return 1;
    }

    ChValue s = ch_gc_make_string_cstr(&gc, "hello");
    if (!ch_is_string(s) || strcmp(ch_as_string(s)->data, "hello") != 0) {
        fprintf(stderr, "string failed\n");
        return 1;
    }

    /* allocate garbage then collect; rooted pair must survive */
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
