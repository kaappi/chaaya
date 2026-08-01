#include "chaaya/value.h"

#include "chaaya/bignum.h"
#include "chaaya/rational.h"

#include <string.h>

ChValue ch_make_fixnum(int64_t n) {
    uint64_t u = (uint64_t)n;
    return CH_NANBOX_FIX | (u & CH_NANBOX_PAYLOAD);
}

int64_t ch_to_fixnum(ChValue v) {
    uint64_t payload = v & CH_NANBOX_PAYLOAD;
    /* sign-extend from 48 bits */
    if (payload & (1ULL << 47)) {
        payload |= 0xFFFF000000000000ULL;
    }
    return (int64_t)payload;
}

ChValue ch_make_flonum(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
        (bits & 0x000FFFFFFFFFFFFFULL) != 0) {
        return CH_CANONICAL_NAN;
    }
    return bits;
}

double ch_to_flonum(ChValue v) {
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

ChValue ch_make_char(uint32_t codepoint) {
    return CH_NANBOX_IMM | (0x100ULL + (codepoint & 0x1FFFFFULL));
}

bool ch_is_char(ChValue v) {
    if (!ch_is_immediate(v)) {
        return false;
    }
    uint64_t payload = v & CH_NANBOX_PAYLOAD;
    return payload >= 0x100 && payload <= 0x100 + 0x1FFFFF;
}

uint32_t ch_to_char(ChValue v) {
    return (uint32_t)((v & CH_NANBOX_PAYLOAD) - 0x100);
}

ChValue ch_make_pointer(ChObject *obj) {
    return CH_NANBOX_PTR | ((uintptr_t)obj & CH_NANBOX_PAYLOAD);
}

ChObject *ch_to_object(ChValue v) {
    return (ChObject *)(uintptr_t)(v & CH_NANBOX_PAYLOAD);
}

#define CH_IS_TAG(v, obj_tag) (ch_is_pointer(v) && ch_to_object(v)->tag == (obj_tag))

bool ch_is_pair(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_PAIR);
}

bool ch_is_symbol(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_SYMBOL);
}

bool ch_is_string(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_STRING);
}

bool ch_is_vector(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_VECTOR);
}

bool ch_is_closure(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_CLOSURE);
}

bool ch_is_native(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_NATIVE);
}

bool ch_is_function(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_FUNCTION);
}

bool ch_is_continuation(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_CONTINUATION);
}

bool ch_is_values(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_VALUES);
}

bool ch_is_port(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_PORT);
}

bool ch_is_transformer(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_TRANSFORMER);
}

bool ch_is_record_type(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_RECORD_TYPE);
}

bool ch_is_record(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_RECORD);
}

bool ch_is_promise(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_PROMISE);
}

bool ch_is_bignum(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_BIGNUM);
}

bool ch_is_rational_obj(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_RATIONAL);
}

bool ch_is_complex_obj(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_COMPLEX);
}

bool ch_is_error_object(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_ERROR_OBJ);
}

bool ch_is_parameter(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_PARAMETER);
}

bool ch_is_hashtable(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_HASHTABLE);
}

bool ch_is_bytevector(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_BYTEVECTOR);
}

bool ch_is_time(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_TIME);
}

bool ch_is_fiber(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_FIBER);
}

bool ch_is_channel(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_CHANNEL);
}

bool ch_is_foreign_library(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_FOREIGN_LIBRARY);
}

bool ch_is_foreign_procedure(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_FOREIGN_PROC);
}

bool ch_is_random_source(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_RANDOM_SOURCE);
}

bool ch_is_ephemeron(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_EPHEMERON);
}

bool ch_is_file_info(ChValue v) {
    return CH_IS_TAG(v, CH_TAG_FILE_INFO);
}

bool ch_is_exact_integer(ChValue v) {
    return ch_is_fixnum(v) || ch_is_bignum(v);
}

bool ch_is_exact(ChValue v) {
    return ch_is_exact_integer(v) || ch_is_rational_obj(v);
}

bool ch_is_number(ChValue v) {
    return ch_is_fixnum(v) || ch_is_flonum(v) || ch_is_bignum(v) || ch_is_rational_obj(v) ||
           ch_is_complex_obj(v);
}

bool ch_is_procedure(ChValue v) {
    return ch_is_closure(v) || ch_is_native(v) || ch_is_continuation(v) || ch_is_parameter(v) ||
           ch_is_foreign_procedure(v);
}

ChPair *ch_as_pair(ChValue v) {
    return (ChPair *)ch_to_object(v);
}

ChSymbol *ch_as_symbol(ChValue v) {
    return (ChSymbol *)ch_to_object(v);
}

ChString *ch_as_string(ChValue v) {
    return (ChString *)ch_to_object(v);
}

ChVector *ch_as_vector(ChValue v) {
    return (ChVector *)ch_to_object(v);
}

ChFunction *ch_as_function(ChValue v) {
    return (ChFunction *)ch_to_object(v);
}

ChClosure *ch_as_closure(ChValue v) {
    return (ChClosure *)ch_to_object(v);
}

ChNative *ch_as_native(ChValue v) {
    return (ChNative *)ch_to_object(v);
}

ChContinuation *ch_as_continuation(ChValue v) {
    return (ChContinuation *)ch_to_object(v);
}

ChValues *ch_as_values(ChValue v) {
    return (ChValues *)ch_to_object(v);
}

ChPort *ch_as_port(ChValue v) {
    return (ChPort *)ch_to_object(v);
}

ChTransformer *ch_as_transformer(ChValue v) {
    return (ChTransformer *)ch_to_object(v);
}

ChRecordType *ch_as_record_type(ChValue v) {
    return (ChRecordType *)ch_to_object(v);
}

ChRecord *ch_as_record(ChValue v) {
    return (ChRecord *)ch_to_object(v);
}

ChPromise *ch_as_promise(ChValue v) {
    return (ChPromise *)ch_to_object(v);
}

ChBignum *ch_as_bignum(ChValue v) {
    return (ChBignum *)ch_to_object(v);
}

ChRational *ch_as_rational(ChValue v) {
    return (ChRational *)ch_to_object(v);
}

ChComplex *ch_as_complex(ChValue v) {
    return (ChComplex *)ch_to_object(v);
}

ChErrorObject *ch_as_error_object(ChValue v) {
    return (ChErrorObject *)ch_to_object(v);
}

ChParameter *ch_as_parameter(ChValue v) {
    return (ChParameter *)ch_to_object(v);
}

ChHashtable *ch_as_hashtable(ChValue v) {
    return (ChHashtable *)ch_to_object(v);
}

ChBytevector *ch_as_bytevector(ChValue v) {
    return (ChBytevector *)ch_to_object(v);
}

ChTime *ch_as_time(ChValue v) {
    return (ChTime *)ch_to_object(v);
}

ChFiber *ch_as_fiber(ChValue v) {
    return (ChFiber *)ch_to_object(v);
}

ChChannel *ch_as_channel(ChValue v) {
    return (ChChannel *)ch_to_object(v);
}

ChForeignLibrary *ch_as_foreign_library(ChValue v) {
    return (ChForeignLibrary *)ch_to_object(v);
}

ChForeignProcedure *ch_as_foreign_procedure(ChValue v) {
    return (ChForeignProcedure *)ch_to_object(v);
}

ChRandomSource *ch_as_random_source(ChValue v) {
    return (ChRandomSource *)ch_to_object(v);
}

ChEphemeron *ch_as_ephemeron(ChValue v) {
    return (ChEphemeron *)ch_to_object(v);
}

ChFileInfo *ch_as_file_info(ChValue v) {
    return (ChFileInfo *)ch_to_object(v);
}

const char *ch_symbol_basename(ChSymbol *sym) {
    const char *name = sym->name;
    if (strncmp(name, "__hyg_", 6) != 0) {
        return name;
    }
    const char *p = name + 6;
    while (*p >= '0' && *p <= '9') {
        p++;
    }
    if (*p == '_' && p[1] != '\0') {
        return p + 1;
    }
    return name;
}

ChValue ch_coerce_single(ChValue v) {
    if (!ch_is_values(v)) {
        return v;
    }
    ChValues *vs = ch_as_values(v);
    if (vs->count == 0) {
        return CH_VOID;
    }
    return vs->items[0];
}

ChValue ch_car(ChValue v) {
    return ch_as_pair(v)->car;
}

ChValue ch_cdr(ChValue v) {
    return ch_as_pair(v)->cdr;
}

void ch_set_car(ChValue pair, ChValue v) {
    ch_as_pair(pair)->car = v;
}

void ch_set_cdr(ChValue pair, ChValue v) {
    ch_as_pair(pair)->cdr = v;
}

bool ch_eq(ChValue a, ChValue b) {
    return a == b;
}

static bool inexact_bits_equal(double x, double y) {
    uint64_t xb = 0;
    uint64_t yb = 0;
    memcpy(&xb, &x, sizeof(xb));
    memcpy(&yb, &y, sizeof(yb));
    return xb == yb;
}

bool ch_eqv(ChValue a, ChValue b) {
    if (a == b) {
        return true;
    }
    if (ch_is_fixnum(a) && ch_is_fixnum(b)) {
        return ch_to_fixnum(a) == ch_to_fixnum(b);
    }
    if (ch_is_exact_integer(a) && ch_is_exact_integer(b)) {
        return ch_bignum_compare(a, b) == 0;
    }
    if (ch_is_exact(a) && ch_is_exact(b)) {
        /* Cross-multiply without a VM — use string/f64 only if we lack GC here.
         * eqv? for rationals: same reduced num and den. */
        if (ch_is_rational_obj(a) && ch_is_rational_obj(b)) {
            ChRational *ra = ch_as_rational(a);
            ChRational *rb = ch_as_rational(b);
            return ch_bignum_compare(ra->numerator, rb->numerator) == 0 &&
                   ch_bignum_compare(ra->denominator, rb->denominator) == 0;
        }
        if (ch_is_rational_obj(a) || ch_is_rational_obj(b)) {
            return false; /* integer vs non-reduced shouldn't happen; unequal types */
        }
    }
    if (ch_is_flonum(a) && ch_is_flonum(b)) {
        /* Preserve IEEE distinctions such as +0.0 vs -0.0. */
        return inexact_bits_equal(ch_to_flonum(a), ch_to_flonum(b));
    }
    if (ch_is_complex_obj(a) && ch_is_complex_obj(b)) {
        ChComplex *ca = ch_as_complex(a);
        ChComplex *cb = ch_as_complex(b);
        return inexact_bits_equal(ca->real, cb->real) && inexact_bits_equal(ca->imag, cb->imag);
    }
    if (ch_is_char(a) && ch_is_char(b)) {
        return ch_to_char(a) == ch_to_char(b);
    }
    return false;
}

static bool equal_list(ChValue a, ChValue b);

bool ch_equal(ChValue a, ChValue b) {
    if (ch_eqv(a, b)) {
        return true;
    }
    if (ch_is_pair(a) && ch_is_pair(b)) {
        return equal_list(a, b);
    }
    if (ch_is_string(a) && ch_is_string(b)) {
        ChString *sa = ch_as_string(a);
        ChString *sb = ch_as_string(b);
        if (sa->len != sb->len) {
            return false;
        }
        return memcmp(sa->data, sb->data, sa->len) == 0;
    }
    if (ch_is_vector(a) && ch_is_vector(b)) {
        ChVector *va = ch_as_vector(a);
        ChVector *vb = ch_as_vector(b);
        if (va->len != vb->len) {
            return false;
        }
        for (size_t i = 0; i < va->len; i++) {
            if (!ch_equal(va->items[i], vb->items[i])) {
                return false;
            }
        }
        return true;
    }
    if (ch_is_bytevector(a) && ch_is_bytevector(b)) {
        ChBytevector *ba = ch_as_bytevector(a);
        ChBytevector *bb = ch_as_bytevector(b);
        if (ba->len != bb->len) {
            return false;
        }
        if (ba->len == 0) {
            return true;
        }
        return memcmp(ba->data, bb->data, ba->len) == 0;
    }
    return false;
}

static bool equal_list(ChValue a, ChValue b) {
    while (ch_is_pair(a) && ch_is_pair(b)) {
        if (!ch_equal(ch_car(a), ch_car(b))) {
            return false;
        }
        a = ch_cdr(a);
        b = ch_cdr(b);
    }
    return ch_equal(a, b);
}
