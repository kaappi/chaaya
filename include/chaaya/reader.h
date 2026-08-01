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

struct ChReader;
typedef bool (*ChReaderRefillFn)(struct ChReader *reader, void *ctx);

#define CH_READER_MAX_LABELS 256

typedef struct ChReader {
    ChGC *gc;
    const char *src;
    size_t len;
    size_t pos;
    int fold_case; /* ASCII fold for identifiers when non-zero */
    ChReaderRefillFn refill;
    void *refill_ctx;
    char error[256];
    /* Datum labels (#n= / #n#) for the current top-level read. */
    ChValue labels[CH_READER_MAX_LABELS];
    uint8_t label_set[CH_READER_MAX_LABELS];
} ChReader;

void ch_reader_init(ChReader *r, ChGC *gc, const char *src, size_t len);
void ch_reader_set_refill(ChReader *r, ChReaderRefillFn refill, void *ctx);
ChReadStatus ch_read_datum(ChReader *r, ChValue *out);
const char *ch_reader_error(const ChReader *r);

/* Parse a full numeric literal (no surrounding whitespace). Returns false on failure. */
bool ch_parse_number(ChGC *gc, const char *text, size_t len, ChValue *out);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_READER_H */
