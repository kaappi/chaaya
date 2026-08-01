#ifndef CHAAYA_READER_H
#define CHAAYA_READER_H

#include "chaaya/gc.h"
#include "chaaya/value.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ChReadStatus {
    CH_READ_OK = 0,
    CH_READ_EOF,
    CH_READ_ERROR,
} ChReadStatus;

typedef struct ChReader {
    ChGC *gc;
    const char *src;
    size_t len;
    size_t pos;
    int fold_case; /* ASCII fold for identifiers when non-zero */
    char error[256];
} ChReader;

void ch_reader_init(ChReader *r, ChGC *gc, const char *src, size_t len);
ChReadStatus ch_read_datum(ChReader *r, ChValue *out);
const char *ch_reader_error(const ChReader *r);

/* Parse a full numeric literal (no surrounding whitespace). Returns false on failure. */
bool ch_parse_number(ChGC *gc, const char *text, size_t len, ChValue *out);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_READER_H */
