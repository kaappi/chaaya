#ifndef CHAAYA_COMPLEX_H
#define CHAAYA_COMPLEX_H

#include "chaaya/gc.h"
#include "chaaya/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Rectangular complex; imag == 0.0 → flonum(real). */
ChValue ch_make_complex(ChGC *gc, double real, double imag);

/* Always allocate a complex object (used when +0.0i must stay complex). */
ChValue ch_make_complex_raw(ChGC *gc, double real, double imag);

/* Split any number into rectangular parts. Returns 0 if not a number. */
int ch_complex_parts(ChValue v, double *real_out, double *imag_out);

ChValue ch_complex_add(ChGC *gc, ChValue a, ChValue b);
ChValue ch_complex_sub(ChGC *gc, ChValue a, ChValue b);
ChValue ch_complex_mul(ChGC *gc, ChValue a, ChValue b);
ChValue ch_complex_div(ChGC *gc, ChValue a, ChValue b); /* CH_UNDEFINED on /0 */
ChValue ch_complex_negate(ChGC *gc, ChValue a);

/* Heap-allocated print form; caller frees. */
char *ch_complex_to_string(ChValue v);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_COMPLEX_H */
