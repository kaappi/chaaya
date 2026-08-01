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

int main(void) {
    ChGC gc;
    ch_gc_init(&gc);

    if (!check_roundtrip(&gc, "42", "42")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "#t", "#t")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "#f", "#f")) {
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
    if (!check_roundtrip(&gc, "\"hi\"", "\"hi\"")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "#(1 2)", "#(1 2)")) {
        return 1;
    }
    if (!check_roundtrip(&gc, "(1 . 2)", "(1 . 2)")) {
        return 1;
    }

    ch_gc_deinit(&gc);
    printf("ok\n");
    return 0;
}
