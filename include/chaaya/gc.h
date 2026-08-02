#ifndef CHAAYA_GC_H
#define CHAAYA_GC_H

#include "chaaya/value.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deep reader nesting (1023) and nested C helpers push many temporary roots. */
#define CH_GC_ROOT_MAX 16384
#define CH_GC_DEFAULT_THRESHOLD 1024
#define CH_GC_DEFAULT_PROMOTION_AGE 2
#define CH_GC_DEFAULT_MAJOR_INTERVAL 8

struct ChVM;

typedef struct ChGC {
    ChObject *young_objects;
    ChObject *old_objects;
    size_t young_count;
    size_t old_count;
    size_t object_count;
    size_t alloc_count;
    size_t threshold;
    size_t collections;
    size_t minor_collections;
    size_t major_collections;
    uint8_t promotion_age;
    uint8_t major_interval;
    uint16_t id;           /* unique heap id for owner checks */
    uint8_t owns_symbols;  /* 0 => symbol table shared with parent */
    ChValue *roots[CH_GC_ROOT_MAX];
    size_t root_count;
    /* Ephemerons pending weak-key fixpoint during collection. */
    ChValue *pending_ephemerons;
    size_t pending_ephem_count;
    size_t pending_ephem_cap;
    /* Old containers that may point at young objects (generational). */
    ChObject **remembered_set;
    size_t remembered_count;
    size_t remembered_cap;
    /* symbol intern table */
    ChSymbol **symbols;
    size_t symbol_count;
    size_t symbol_cap;
    struct ChVM *vm; /* live interpreter roots during collection */
    struct ChFunction *compiling_fns[32];
    size_t compiling_fn_depth;
} ChGC;

void ch_gc_init(ChGC *gc);
/* Child heap for an OS thread: private nursery; shares parent's symbol table. */
void ch_gc_init_for_thread(ChGC *gc, ChGC *parent);
void ch_gc_deinit(ChGC *gc);

void ch_gc_push(ChGC *gc, ChValue *slot);
void ch_gc_pop(ChGC *gc);
void ch_gc_pop_n(ChGC *gc, size_t n);
void ch_gc_pop_to(ChGC *gc, size_t target);

void ch_gc_collect(ChGC *gc);
void ch_gc_collect_minor(ChGC *gc);
void ch_gc_collect_major(ChGC *gc);
void ch_gc_mark_value(ChValue v);
void ch_gc_write_barrier(ChGC *gc, ChObject *owner, ChValue value);
void ch_gc_promote_to_old(ChGC *gc, ChObject *obj);
void *ch_gc_alloc(ChGC *gc, size_t size, ChObjectTag tag);

ChValue ch_gc_cons(ChGC *gc, ChValue car, ChValue cdr);
ChValue ch_gc_make_string(ChGC *gc, const char *bytes, size_t len);
ChValue ch_gc_make_string_cstr(ChGC *gc, const char *cstr);
ChValue ch_gc_intern_symbol(ChGC *gc, const char *name, size_t len);
ChValue ch_gc_intern_symbol_cstr(ChGC *gc, const char *name);
ChValue ch_gc_alloc_uninterned_symbol(ChGC *gc, const char *name, size_t len);
ChValue ch_gc_alloc_uninterned_symbol_cstr(ChGC *gc, const char *name);
ChValue ch_gc_make_vector(ChGC *gc, size_t len, ChValue fill);
ChValue ch_gc_make_function(ChGC *gc);
ChValue ch_gc_make_closure(ChGC *gc, ChFunction *fn, ChUpvalue **upvalues);
ChValue ch_gc_make_native(ChGC *gc, ChNativeFn fn, const char *name, int arity, int min_arity);
ChValue ch_gc_make_continuation(ChGC *gc);
ChValue ch_gc_make_values(ChGC *gc, ChValue *items, size_t count);
ChValue ch_gc_make_stdio_port(ChGC *gc, FILE *file, int input, int output);
ChValue ch_gc_make_string_input_port(ChGC *gc, const char *bytes, size_t len);
ChValue ch_gc_make_string_output_port(ChGC *gc);
ChValue ch_gc_make_bytevector_input_port(ChGC *gc, const uint8_t *bytes, size_t len);
ChValue ch_gc_make_bytevector_output_port(ChGC *gc);
ChValue ch_gc_make_transformer(ChGC *gc);
ChValue ch_gc_make_record_type(ChGC *gc, ChValue name, uint16_t num_fields);
ChValue ch_gc_make_record_type_ext(ChGC *gc, ChValue name, uint16_t own_fields, ChRecordType *parent);
ChValue ch_gc_make_record(ChGC *gc, ChRecordType *rtype, ChValue *fields, uint16_t nfields);
ChValue ch_gc_make_promise(ChGC *gc, int forced, ChValue value);
ChValue ch_gc_make_file_port(ChGC *gc, FILE *file, int input, int output, int binary);
ChValue ch_gc_make_error_object(ChGC *gc, ChValue message, ChValue irritants, uint8_t error_type);
ChValue ch_gc_make_parameter(ChGC *gc, ChValue init, ChValue converter);
ChValue ch_gc_make_hashtable(ChGC *gc, size_t capacity);
ChValue ch_gc_make_bytevector(ChGC *gc, size_t len, uint8_t fill);
ChValue ch_gc_make_time(ChGC *gc, int64_t seconds, int32_t nanoseconds, ChValue type_sym);
ChValue ch_gc_make_random_source(ChGC *gc, uint64_t seed);
void ch_random_source_seed(ChRandomSource *rs, uint64_t seed);
uint64_t ch_random_source_next_u64(ChRandomSource *rs);
ChValue ch_gc_make_ephemeron(ChGC *gc, ChValue key, ChValue value);
ChValue ch_gc_make_file_info(ChGC *gc, const ChFileInfo *info);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_GC_H */
