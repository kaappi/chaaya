#ifndef CHAAYA_VALUE_H
#define CHAAYA_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

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
} ChObjectTag;

typedef struct ChObject {
    uint8_t tag;
    uint8_t marked;
    uint16_t reserved;
    struct ChObject *next; /* GC heap list */
} ChObject;

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

typedef struct ChString {
    ChObject header;
    size_t len; /* byte length */
    char data[]; /* flexible array, null-terminated */
} ChString;

typedef struct ChVector {
    ChObject header;
    size_t len;
    ChValue *items;
} ChVector;

struct ChVM;
struct ChFunction;

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

typedef struct ChClosure {
    ChObject header;
    ChFunction *fn;
    ChUpvalue **upvalues;
} ChClosure;

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
bool ch_is_procedure(ChValue v);

ChPair *ch_as_pair(ChValue v);
ChSymbol *ch_as_symbol(ChValue v);
ChString *ch_as_string(ChValue v);
ChVector *ch_as_vector(ChValue v);
ChFunction *ch_as_function(ChValue v);
ChClosure *ch_as_closure(ChValue v);
ChNative *ch_as_native(ChValue v);

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
