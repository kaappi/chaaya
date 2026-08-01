#include "chaaya/value.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    if (!ch_is_fixnum(ch_make_fixnum(42))) {
        fprintf(stderr, "fixnum tag failed\n");
        return 1;
    }
    if (ch_to_fixnum(ch_make_fixnum(-7)) != -7) {
        fprintf(stderr, "fixnum round-trip failed\n");
        return 1;
    }
    if (ch_make_fixnum(1) == CH_NIL) {
        fprintf(stderr, "nil collision\n");
        return 1;
    }
    printf("ok\n");
    return 0;
}
