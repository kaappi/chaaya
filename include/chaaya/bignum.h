#ifndef CHAAYA_BIGNUM_H
#define CHAAYA_BIGNUM_H

#include "chaaya/gc.h"
#include "chaaya/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Construct an exact integer: fixnum if in i48 range, else bignum. */
ChValue ch_make_integer(ChGC *gc, int64_t n);
/* Non-negative magnitude that may exceed the fixnum range (radix chunk parse). */
ChValue ch_value_from_u64(ChGC *gc, uint64_t mag);
ChValue ch_gc_make_bignum_from_i64(ChGC *gc, int64_t n);
ChValue ch_gc_make_bignum_from_limbs(ChGC *gc, const uint64_t *limbs, size_t len, int positive);

/* Normalize (trim leading zeros) and demote to fixnum when possible. */
ChValue ch_bignum_normalize(ChGC *gc, ChValue v);

/* Exact integer arithmetic (fixnum or bignum). Flonums rejected by callers. */
ChValue ch_bignum_add(ChGC *gc, ChValue a, ChValue b);
ChValue ch_bignum_sub(ChGC *gc, ChValue a, ChValue b);
ChValue ch_bignum_mul(ChGC *gc, ChValue a, ChValue b);
ChValue ch_bignum_negate(ChGC *gc, ChValue a);
ChValue ch_bignum_abs(ChGC *gc, ChValue a);
ChValue ch_bignum_quotient(ChGC *gc, ChValue a, ChValue b);  /* trunc toward 0 */
ChValue ch_bignum_remainder(ChGC *gc, ChValue a, ChValue b); /* a - quot*b */

/* Compare exact integers: -1, 0, 1. */
int ch_bignum_compare(ChValue a, ChValue b);

/* Parse decimal digit string (optional leading +/-). Returns CH_UNDEFINED on failure. */
ChValue ch_bignum_parse_decimal(ChGC *gc, const char *text, size_t len);

/* Heap-allocated decimal string; caller frees. */
char *ch_bignum_to_string(ChValue v);

/* Convert exact integer to f64 (approximate for large values). */
double ch_bignum_to_f64(ChValue v);

/* If d is an exact integer representable without rounding, return that integer. */
ChValue ch_double_to_exact_if_exact(ChGC *gc, double d);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_BIGNUM_H */
