#ifndef CHAAYA_GC_H
#define CHAAYA_GC_H

#include "chaaya/value.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_GC_ROOT_MAX 4096
#define CH_GC_DEFAULT_THRESHOLD 1024

typedef struct ChGC {
    ChObject *objects;
    size_t object_count;
    size_t alloc_count;
    size_t threshold;
    size_t collections;
    ChValue *roots[CH_GC_ROOT_MAX];
    size_t root_count;
    /* symbol intern table */
    ChSymbol **symbols;
    size_t symbol_count;
    size_t symbol_cap;
} ChGC;

void ch_gc_init(ChGC *gc);
void ch_gc_deinit(ChGC *gc);

void ch_gc_push(ChGC *gc, ChValue *slot);
void ch_gc_pop(ChGC *gc);
void ch_gc_pop_n(ChGC *gc, size_t n);

void ch_gc_collect(ChGC *gc);
void *ch_gc_alloc(ChGC *gc, size_t size, ChObjectTag tag);

ChValue ch_gc_cons(ChGC *gc, ChValue car, ChValue cdr);
ChValue ch_gc_make_string(ChGC *gc, const char *bytes, size_t len);
ChValue ch_gc_make_string_cstr(ChGC *gc, const char *cstr);
ChValue ch_gc_intern_symbol(ChGC *gc, const char *name, size_t len);
ChValue ch_gc_intern_symbol_cstr(ChGC *gc, const char *name);
ChValue ch_gc_make_vector(ChGC *gc, size_t len, ChValue fill);
ChValue ch_gc_make_function(ChGC *gc);
ChValue ch_gc_make_closure(ChGC *gc, ChFunction *fn, ChUpvalue **upvalues);
ChValue ch_gc_make_native(ChGC *gc, ChNativeFn fn, const char *name, int arity, int min_arity);
ChValue ch_gc_make_continuation(ChGC *gc);
ChValue ch_gc_make_values(ChGC *gc, ChValue *items, size_t count);
ChValue ch_gc_make_stdio_port(ChGC *gc, FILE *file, int input, int output);
ChValue ch_gc_make_string_input_port(ChGC *gc, const char *bytes, size_t len);
ChValue ch_gc_make_string_output_port(ChGC *gc);
ChValue ch_gc_make_transformer(ChGC *gc);
ChValue ch_gc_make_record_type(ChGC *gc, ChValue name, uint16_t num_fields);
ChValue ch_gc_make_record(ChGC *gc, ChRecordType *rtype, ChValue *fields, uint16_t nfields);
ChValue ch_gc_make_promise(ChGC *gc, int forced, ChValue value);
ChValue ch_gc_make_file_port(ChGC *gc, FILE *file, int input, int output);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_GC_H */
