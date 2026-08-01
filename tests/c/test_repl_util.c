#include "chaaya/repl.h"

#include <stdio.h>

int main(void) {
    if (ch_repl_paren_depth("(define (f x)") != 1) {
        fprintf(stderr, "expected depth 1\n");
        return 1;
    }
    if (ch_repl_paren_depth("(define (f x) (+ x 1))") != 0) {
        fprintf(stderr, "expected depth 0\n");
        return 1;
    }
    if (ch_repl_paren_depth("\"(\"") != 0) {
        fprintf(stderr, "string paren should not count\n");
        return 1;
    }
    if (ch_repl_paren_depth("; (\n(+ 1 2)") != 0) {
        fprintf(stderr, "comment paren should not count\n");
        return 1;
    }
    if (ch_repl_paren_depth("((a) (b)") != 1) {
        fprintf(stderr, "nested depth\n");
        return 1;
    }
    printf("ok\n");
    return 0;
}
