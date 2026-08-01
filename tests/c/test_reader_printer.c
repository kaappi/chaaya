#include "chaaya/gc.h"
#include "chaaya/printer.h"
#include "chaaya/reader.h"
#include "chaaya/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_roundtrip(ChGC *gc, const char *src, const char *expect_write) {
    ChReader r;
    ch_reader_init(&r, gc, src, strlen(src));
    ChValue v = CH_NIL;
    ch_gc_push(gc, &v);
    if (ch_read_datum(&r, &v) != CH_READ_OK) {
        fprintf(stderr, "read failed for %s: %s\n", src, ch_reader_error(&r));
        ch_gc_pop(gc);
        return 0;
    }
    char *got = ch_value_to_string(v, false);
    if (!got) {
        fprintf(stderr, "print failed\n");
        ch_gc_pop(gc);
        return 0;
    }
    if (strcmp(got, expect_write) != 0) {
        fprintf(stderr, "roundtrip mismatch for %s: got [%s] expect [%s]\n", src, got,
                expect_write);
        free(got);
        ch_gc_pop(gc);
        return 0;
    }
    free(got);
    ch_gc_pop(gc);
    return 1;
}

static int check_read_error(ChGC *gc, const char *src) {
    ChReader r;
    ch_reader_init(&r, gc, src, strlen(src));
    ChValue v = CH_NIL;
    ChReadStatus st = ch_read_datum(&r, &v);
    if (st != CH_READ_ERROR) {
        fprintf(stderr, "expected read error for: %s\n", src);
        return 0;
    }
    return 1;
}

static int check_display(ChGC *gc, const char *src, const char *expect_display) {
    ChReader r;
    ch_reader_init(&r, gc, src, strlen(src));
    ChValue v = CH_NIL;
    ch_gc_push(gc, &v);
    if (ch_read_datum(&r, &v) != CH_READ_OK) {
        fprintf(stderr, "read failed for display %s\n", src);
        ch_gc_pop(gc);
        return 0;
    }
    char *got = ch_value_to_string(v, true);
    if (!got || strcmp(got, expect_display) != 0) {
        fprintf(stderr, "display mismatch for %s: got [%s] expect [%s]\n", src, got ? got : "null",
                expect_display);
        free(got);
        ch_gc_pop(gc);
        return 0;
    }
    free(got);
    ch_gc_pop(gc);
    return 1;
}

static int check_multi(ChGC *gc, const char *src, int expect_count) {
    ChReader r;
    ch_reader_init(&r, gc, src, strlen(src));
    int n = 0;
    for (;;) {
        ChValue v = CH_NIL;
        ChReadStatus st = ch_read_datum(&r, &v);
        if (st == CH_READ_EOF) {
            break;
        }
        if (st != CH_READ_OK) {
            fprintf(stderr, "multi-read failed: %s\n", ch_reader_error(&r));
            return 0;
        }
        n++;
    }
    if (n != expect_count) {
        fprintf(stderr, "expected %d datums, got %d in: %s\n", expect_count, n, src);
        return 0;
    }
    return 1;
}

int main(void) {
    ChGC gc;
    ch_gc_init(&gc);

    if (!check_roundtrip(&gc, "42", "42")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "-17", "-17")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "3.5", "3.5")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "#t", "#t")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "#true", "#t")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "#f", "#f")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "#false", "#f")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "()", "()")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "(1 2 3)", "(1 2 3)")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "'(a b)", "(quote (a b))")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "`x", "(quasiquote x)")) {
        return 1;
    }
    if (!check_roundtrip(&gc, ",x", "(unquote x)")) {
        return 1;
    }
    if (!check_roundtrip(&gc, ",@x", "(unquote-splicing x)")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "\"hi\"", "\"hi\"")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "\"a\\nb\"", "\"a\\nb\"")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "#(1 2)", "#(1 2)")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "(1 . 2)", "(1 . 2)")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "(1 2 . 3)", "(1 2 . 3)")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "#\\a", "#\\a")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "#\\space", "#\\space")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "#\\newline", "#\\newline")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "foo", "foo")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "+", "+")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "((a) (b c))", "((a) (b c))")) {
        return 1;
    }

    /* comments and whitespace */
    if (!check_roundtrip(&gc, "; comment\n42", "42")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "  ( 1   2 )  ", "(1 2)")) {
        return 1;
    }

    if (!check_display(&gc, "\"hi\"", "hi")) {
        return 1;
    }
    if (!check_display(&gc, "#\\A", "A")) {
        return 1;
    }

    if (!check_multi(&gc, "1 2 3", 3)) {
        return 1;
    }
    if (!check_multi(&gc, "(define x 1) x", 2)) {
        return 1;
    }

    if (!check_read_error(&gc, "(")) {
        return 1;
    }
    if (!check_read_error(&gc, ")")) {
        return 1;
    }
    if (!check_read_error(&gc, "\"")) {
        return 1;
    }
    if (!check_read_error(&gc, "(1 . 2 3)")) {
        return 1;
    }

    ch_gc_deinit(&gc);
    printf("ok\n");
    return 0;
}
