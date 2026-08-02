#ifndef CHAAYA_VALUE_H
#define CHAAYA_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t ChValue;

/* NaN-boxing (same layout family as Kaappi):
 *   v < 0xFFFC000000000000  → flonum (raw IEEE f64 bits)
 *   (v >> 48) == 0xFFFC     → heap pointer
 *   (v >> 48) == 0xFFFD     → fixnum (i48)
 *   (v >> 48) == 0xFFFE     → immediate
 */
#define CH_NANBOX_PTR 0xFFFC000000000000ULL
#define CH_NANBOX_FIX 0xFFFD000000000000ULL
#define CH_NANBOX_IMM 0xFFFE000000000000ULL
#define CH_NANBOX_THRESHOLD CH_NANBOX_PTR
#define CH_NANBOX_PAYLOAD 0x0000FFFFFFFFFFFFULL
#define CH_CANONICAL_NAN 0x7FF8000000000000ULL

#define CH_NIL ((ChValue)(CH_NANBOX_IMM | 0))
#define CH_FALSE ((ChValue)(CH_NANBOX_IMM | 1))
#define CH_TRUE ((ChValue)(CH_NANBOX_IMM | 2))
#define CH_VOID ((ChValue)(CH_NANBOX_IMM | 3))
#define CH_EOF_OBJ ((ChValue)(CH_NANBOX_IMM | 4))
#define CH_UNDEFINED ((ChValue)(CH_NANBOX_IMM | 5))

typedef enum ChObjectTag {
    CH_TAG_PAIR = 0,
    CH_TAG_SYMBOL = 1,
    CH_TAG_STRING = 2,
    CH_TAG_VECTOR = 3,
    CH_TAG_FUNCTION = 4,
    CH_TAG_CLOSURE = 5,
    CH_TAG_NATIVE = 6,
    CH_TAG_CONTINUATION = 7,
    CH_TAG_VALUES = 8,
    CH_TAG_PORT = 9,
    CH_TAG_TRANSFORMER = 10,
    CH_TAG_RECORD_TYPE = 11,
    CH_TAG_RECORD = 12,
    CH_TAG_PROMISE = 13,
    CH_TAG_BIGNUM = 14,
    CH_TAG_RATIONAL = 15,
    CH_TAG_COMPLEX = 16,
    CH_TAG_ENVIRONMENT = 17,
    CH_TAG_ERROR_OBJ = 18,
    CH_TAG_PARAMETER = 19,
    CH_TAG_HASHTABLE = 20,
    CH_TAG_BYTEVECTOR = 21,
    CH_TAG_TIME = 22,
    CH_TAG_FIBER = 23,
    CH_TAG_CHANNEL = 24,
    CH_TAG_FOREIGN_LIBRARY = 25,
    CH_TAG_FOREIGN_PROC = 26,
    CH_TAG_RANDOM_SOURCE = 27,
    CH_TAG_EPHEMERON = 28,
    CH_TAG_FILE_INFO = 29,
} ChObjectTag;

typedef struct ChObject {
    uint8_t tag;
    uint8_t marked;
    uint8_t generation;
    uint8_t age;
    uint16_t reserved;
    struct ChObject *next; /* GC heap list */
} ChObject;

#define CH_OBJ_GEN_YOUNG 0u
#define CH_OBJ_GEN_OLD 1u
#define CH_OBJ_FLAG_IMMUTABLE 0x0001u
#define CH_OBJ_FLAG_UNINTERNED 0x0002u

static inline bool ch_object_is_immutable(const ChObject *obj) {
    return obj && (obj->reserved & CH_OBJ_FLAG_IMMUTABLE) != 0;
}

static inline bool ch_object_is_old(const ChObject *obj) {
    return obj && obj->generation == CH_OBJ_GEN_OLD;
}

static inline void ch_object_set_immutable(ChObject *obj, bool immutable) {
    if (!obj) {
        return;
    }
    if (immutable) {
        obj->reserved = (uint16_t)(obj->reserved | CH_OBJ_FLAG_IMMUTABLE);
    } else {
        obj->reserved = (uint16_t)(obj->reserved & (uint16_t)~CH_OBJ_FLAG_IMMUTABLE);
    }
}

typedef struct ChPair {
    ChObject header;
    ChValue car;
    ChValue cdr;
} ChPair;

typedef struct ChSymbol {
    ChObject header;
    size_t len;
    char name[]; /* flexible array, null-terminated */
} ChSymbol;

static inline bool ch_symbol_is_interned(const ChSymbol *sym) {
    return sym && (sym->header.reserved & CH_OBJ_FLAG_UNINTERNED) == 0;
}

typedef struct ChString {
    ChObject header;
    size_t len; /* byte length */
    char *data; /* heap buffer, null-terminated; freed with the object */
} ChString;

typedef struct ChVector {
    ChObject header;
    size_t len;
    ChValue *items;
} ChVector;

struct ChVM;
struct ChFunction;
typedef struct ChFiber ChFiber;
typedef struct ChChannel ChChannel;
typedef struct ChForeignLibrary ChForeignLibrary;
typedef struct ChForeignProcedure ChForeignProcedure;

typedef ChValue (*ChNativeFn)(struct ChVM *vm, ChValue *args, int nargs);

typedef struct ChNative {
    ChObject header;
    ChNativeFn fn;
    const char *name;
    int arity; /* -1 = variadic (at least 0) */
    int min_arity;
} ChNative;

typedef struct ChUpvalue {
    ChValue *location; /* points into registers or closed_value */
    ChValue closed_value;
    bool is_closed;
    struct ChUpvalue *next;
} ChUpvalue;

typedef struct ChFunction {
    ChObject header;
    uint8_t *code;
    size_t code_len;
    ChValue *constants;
    size_t const_count;
    uint8_t arity;
    uint8_t num_regs;
    uint8_t num_upvalues;
    uint8_t variadic; /* 1 if rest arg */
    uint8_t *uv_is_local;
    uint8_t *uv_index;
} ChFunction;

struct ChLibEnv;

typedef struct ChClosure {
    ChObject header;
    ChFunction *fn;
    ChUpvalue **upvalues;
    struct ChLibEnv *home_env; /* library env for CH_ENV_LIB_BIT global ops */
} ChClosure;

/* Saved call frame inside a continuation snapshot (R7RS 6.10). */
typedef struct ChSavedFrame {
    ChClosure *closure;
    size_t ip_offset; /* offset from closure->fn->code */
    size_t reg_base;
    uint8_t num_regs;
} ChSavedFrame;

typedef struct ChWindRecord {
    ChValue before;
    ChValue after;
} ChWindRecord;

typedef struct ChExceptionHandler {
    ChValue handler;
    size_t frame_count; /* frames present when handler was installed */
    size_t wind_count;
} ChExceptionHandler;

typedef struct ChParameterBinding {
    ChValue parameter; /* CH_TAG_PARAMETER object */
    ChValue value;
} ChParameterBinding;

typedef struct ChSavedUpvalue {
    ChUpvalue *uv;
    size_t reg_index; /* absolute index into vm->regs at capture */
} ChSavedUpvalue;

typedef struct ChContinuation {
    ChObject header;
    ChValue *registers;
    size_t register_count;
    ChSavedFrame *frames;
    size_t frame_count;
    ChWindRecord *winds;
    size_t wind_count;
    ChExceptionHandler *handlers;
    size_t handler_count;
    ChParameterBinding *parameter_bindings;
    size_t parameter_binding_count;
    ChSavedUpvalue *open_uvs;
    size_t open_uv_count;
    size_t result_slot; /* absolute register index for call/cc result */
} ChContinuation;

typedef struct ChValues {
    ChObject header;
    size_t count;
    ChValue *items;
} ChValues;

typedef enum ChPortKind {
    CH_PORT_STDIO = 0,
    CH_PORT_STRING_IN = 1,
    CH_PORT_STRING_OUT = 2,
    CH_PORT_FILE = 3, /* owned FILE* — fclose on close */
    CH_PORT_BYTEVECTOR = 4,
} ChPortKind;

typedef struct ChPort {
    ChObject header;
    ChPortKind kind;
    uint8_t input;
    uint8_t output;
    uint8_t closed;
    uint8_t binary; /* file ports: #t => binary-port?, #f => textual-port? */
    FILE *file; /* CH_PORT_STDIO */
    char *buf;  /* string/bytevector ports (owned) */
    size_t len;
    size_t cap;
    size_t pos;
} ChPort;

#define CH_TRANSFORMER_MAX_RULES 32
#define CH_TRANSFORMER_MAX_LITERALS 16

typedef struct ChTransformer {
    ChObject header;
    ChSymbol *literals[CH_TRANSFORMER_MAX_LITERALS];
    size_t literal_count;
    ChSymbol *ellipsis_id; /* NULL => "..." */
    ChValue patterns[CH_TRANSFORMER_MAX_RULES];
    ChValue templates[CH_TRANSFORMER_MAX_RULES];
    size_t rule_count;
} ChTransformer;

#define CH_RECORD_MAX_FIELDS 64

typedef struct ChRecordType {
    ChObject header;
    ChValue name; /* string */
    uint16_t num_fields;
} ChRecordType;

typedef struct ChRecord {
    ChObject header;
    ChRecordType *rtype;
    uint16_t num_fields;
    ChValue fields[]; /* flexible array */
} ChRecord;

typedef struct ChPromise {
    ChObject header;
    uint8_t forced;
    uint8_t forcing;
    ChValue value; /* thunk if lazy; result if forced */
} ChPromise;

/* Sign-magnitude little-endian limbs (Kaappi-compatible layout). */
typedef struct ChBignum {
    ChObject header;
    uint64_t *limbs;
    size_t len;       /* active limb count; 0 = zero */
    uint8_t positive; /* 1 = non-negative */
} ChBignum;

/* Exact fraction p/q in lowest terms; q always positive and > 1 when heap-allocated. */
typedef struct ChRational {
    ChObject header;
    ChValue numerator;   /* fixnum or bignum */
    ChValue denominator; /* fixnum or bignum, always > 0 */
} ChRational;

/* Inexact rectangular complex (MVP). Imag == 0 demotes to flonum. */
typedef struct ChComplex {
    ChObject header;
    double real;
    double imag;
    bool exact_real;
    bool exact_imag;
} ChComplex;

typedef struct ChErrorObject {
    ChObject header;
    ChValue message;   /* string */
    ChValue irritants; /* list */
    uint8_t error_type; /* 0=generic 1=file 2=read */
} ChErrorObject;

typedef struct ChParameter {
    ChObject header;
    ChValue init;
    ChValue converter; /* procedure or NIL */
    ChValue value;
} ChParameter;

typedef enum ChHashtableMode {
    CH_HASHTABLE_EQ = 0,
    CH_HASHTABLE_EQV = 1,
    CH_HASHTABLE_EQUAL = 2,
} ChHashtableMode;

typedef struct ChHashtable {
    ChObject header;
    size_t count;
    size_t cap;
    ChHashtableMode mode;
    ChValue *keys;
    ChValue *vals;
    bool *used;
} ChHashtable;

typedef struct ChBytevector {
    ChObject header;
    size_t len;
    uint8_t data[]; /* flexible array */
} ChBytevector;

typedef struct ChTime {
    ChObject header;
    int64_t seconds;
    int32_t nanoseconds;
    ChValue type_sym; /* time-utc, time-monotonic, etc. */
} ChTime;

typedef struct ChRandomSource {
    ChObject header;
    uint64_t s[4]; /* xoshiro256** state */
} ChRandomSource;

typedef struct ChEphemeron {
    ChObject header;
    ChValue key;
    ChValue value;
    uint8_t broken;
} ChEphemeron;

typedef struct ChFileInfo {
    ChObject header;
    uint32_t mode;
    int64_t size;
    int64_t mtime_sec;
    int64_t atime_sec;
    int64_t ctime_sec;
    uint64_t dev;
    uint64_t ino;
    uint64_t nlinks;
    uint64_t rdev;
    int64_t blksize;
    int64_t blocks;
    uint32_t uid;
    uint32_t gid;
} ChFileInfo;

/* Signed i48 fixnum range. */
#define CH_FIXNUM_MIN (-((int64_t)1 << 47))
#define CH_FIXNUM_MAX (((int64_t)1 << 47) - 1)

static inline bool ch_is_flonum(ChValue v) {
    return v < CH_NANBOX_THRESHOLD;
}

static inline bool ch_is_fixnum(ChValue v) {
    return (v >> 48) == 0xFFFD;
}

static inline bool ch_is_pointer(ChValue v) {
    return (v >> 48) == 0xFFFC;
}

static inline bool ch_is_immediate(ChValue v) {
    return (v >> 48) == 0xFFFE;
}

static inline bool ch_is_nil(ChValue v) {
    return v == CH_NIL;
}

static inline bool ch_is_false(ChValue v) {
    return v == CH_FALSE;
}

static inline bool ch_is_true_value(ChValue v) {
    return v != CH_FALSE;
}

ChValue ch_make_fixnum(int64_t n);
int64_t ch_to_fixnum(ChValue v);
ChValue ch_make_flonum(double d);
double ch_to_flonum(ChValue v);
ChValue ch_make_char(uint32_t codepoint);
bool ch_is_char(ChValue v);
uint32_t ch_to_char(ChValue v);

ChValue ch_make_pointer(ChObject *obj);
ChObject *ch_to_object(ChValue v);

bool ch_is_pair(ChValue v);
bool ch_is_symbol(ChValue v);
bool ch_is_string(ChValue v);
bool ch_is_vector(ChValue v);
bool ch_is_closure(ChValue v);
bool ch_is_native(ChValue v);
bool ch_is_function(ChValue v);
bool ch_is_continuation(ChValue v);
bool ch_is_values(ChValue v);
bool ch_is_port(ChValue v);
bool ch_is_transformer(ChValue v);
bool ch_is_record_type(ChValue v);
bool ch_is_record(ChValue v);
bool ch_is_promise(ChValue v);
bool ch_is_bignum(ChValue v);
bool ch_is_rational_obj(ChValue v); /* heap rational only */
bool ch_is_complex_obj(ChValue v);  /* heap complex only */
bool ch_is_error_object(ChValue v);
bool ch_is_parameter(ChValue v);
bool ch_is_hashtable(ChValue v);
bool ch_is_bytevector(ChValue v);
bool ch_is_time(ChValue v);
bool ch_is_fiber(ChValue v);
bool ch_is_channel(ChValue v);
bool ch_is_foreign_library(ChValue v);
bool ch_is_foreign_procedure(ChValue v);
bool ch_is_random_source(ChValue v);
bool ch_is_ephemeron(ChValue v);
bool ch_is_file_info(ChValue v);
bool ch_is_exact_integer(ChValue v);
bool ch_is_exact(ChValue v); /* integer or rational */
bool ch_is_number(ChValue v);
bool ch_is_procedure(ChValue v);

ChPair *ch_as_pair(ChValue v);
ChSymbol *ch_as_symbol(ChValue v);
ChString *ch_as_string(ChValue v);
ChVector *ch_as_vector(ChValue v);
ChFunction *ch_as_function(ChValue v);
ChClosure *ch_as_closure(ChValue v);
ChNative *ch_as_native(ChValue v);
ChContinuation *ch_as_continuation(ChValue v);
ChValues *ch_as_values(ChValue v);
ChPort *ch_as_port(ChValue v);
ChTransformer *ch_as_transformer(ChValue v);
ChRecordType *ch_as_record_type(ChValue v);
ChRecord *ch_as_record(ChValue v);
ChPromise *ch_as_promise(ChValue v);
ChBignum *ch_as_bignum(ChValue v);
ChRational *ch_as_rational(ChValue v);
ChComplex *ch_as_complex(ChValue v);
ChErrorObject *ch_as_error_object(ChValue v);
ChParameter *ch_as_parameter(ChValue v);
ChHashtable *ch_as_hashtable(ChValue v);
ChBytevector *ch_as_bytevector(ChValue v);
ChTime *ch_as_time(ChValue v);
ChFiber *ch_as_fiber(ChValue v);
ChChannel *ch_as_channel(ChValue v);
ChForeignLibrary *ch_as_foreign_library(ChValue v);
ChForeignProcedure *ch_as_foreign_procedure(ChValue v);
ChRandomSource *ch_as_random_source(ChValue v);
ChEphemeron *ch_as_ephemeron(ChValue v);
ChFileInfo *ch_as_file_info(ChValue v);

/* Collapse multiple values to the first (or void if none). */
ChValue ch_coerce_single(ChValue v);

/* Strip __hyg_N_ prefix for special-form recognition. */
const char *ch_symbol_basename(ChSymbol *sym);

ChValue ch_car(ChValue v);
ChValue ch_cdr(ChValue v);
void ch_set_car(ChValue pair, ChValue v);
void ch_set_cdr(ChValue pair, ChValue v);

bool ch_eq(ChValue a, ChValue b);
bool ch_eqv(ChValue a, ChValue b);
bool ch_equal(ChValue a, ChValue b);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_VALUE_H */
