#ifndef CHAAYA_RATIONAL_H
#define CHAAYA_RATIONAL_H

#include "chaaya/gc.h"
#include "chaaya/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reduced rational or integer. den==0 → CH_UNDEFINED. */
ChValue ch_make_rational(ChGC *gc, ChValue num, ChValue den);

/* Split exact number into numerator/denominator (den=1 for integers). */
void ch_exact_parts(ChValue v, ChValue *num_out, ChValue *den_out);

/* IEEE-754 flonum → exact integer or rational; CH_UNDEFINED if non-finite. */
ChValue ch_exact_from_flonum(ChGC *gc, double f);

ChValue ch_exact_add(ChGC *gc, ChValue a, ChValue b);
ChValue ch_exact_sub(ChGC *gc, ChValue a, ChValue b);
ChValue ch_exact_mul(ChGC *gc, ChValue a, ChValue b);
ChValue ch_exact_div(ChGC *gc, ChValue a, ChValue b); /* CH_UNDEFINED on /0 */
ChValue ch_exact_negate(ChGC *gc, ChValue a);
ChValue ch_exact_abs(ChGC *gc, ChValue a);

/* Compare exact numbers: -1, 0, 1. */
int ch_exact_compare(ChGC *gc, ChValue a, ChValue b);

double ch_exact_to_f64(ChValue v);

/* Heap-allocated "n/d" or integer string; caller frees. */
char *ch_exact_to_string(ChValue v);

/* GCD of exact integers (always non-negative). */
ChValue ch_bignum_gcd(ChGC *gc, ChValue a, ChValue b);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_RATIONAL_H */
