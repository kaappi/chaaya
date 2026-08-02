#ifndef CHAAYA_READER_INTERNAL_H
#define CHAAYA_READER_INTERNAL_H

#include "chaaya/gc.h"
#include "chaaya/value.h"

#include <ctype.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared by atom lexing (booleans) and numeric tower parsing. */
static inline int eq_ci_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

/* Numeric tower parse used by read_atom / read_bytevector. */
bool try_parse_number(ChGC *gc, const char *text, size_t len, ChValue *out);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_READER_INTERNAL_H */
