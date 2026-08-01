#include "chaaya/complex.h"

#include "chaaya/rational.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ChValue ch_make_complex(ChGC *gc, double real, double imag) {
    if (imag == 0.0) {
        return ch_make_flonum(real);
    }
    ChComplex *c = (ChComplex *)ch_gc_alloc(gc, sizeof(ChComplex), CH_TAG_COMPLEX);
    c->real = real;
    c->imag = imag;
    return ch_make_pointer(&c->header);
}

int ch_complex_parts(ChValue v, double *real_out, double *imag_out) {
    if (ch_is_complex_obj(v)) {
        ChComplex *c = ch_as_complex(v);
        *real_out = c->real;
        *imag_out = c->imag;
        return 1;
    }
    if (ch_is_fixnum(v)) {
        *real_out = (double)ch_to_fixnum(v);
    } else if (ch_is_flonum(v)) {
        *real_out = ch_to_flonum(v);
    } else if (ch_is_bignum(v) || ch_is_rational_obj(v)) {
        *real_out = ch_exact_to_f64(v);
    } else {
        return 0;
    }
    *imag_out = 0.0;
    return 1;
}

static int both_parts(ChValue a, ChValue b, double *ar, double *ai, double *br, double *bi) {
    return ch_complex_parts(a, ar, ai) && ch_complex_parts(b, br, bi);
}

ChValue ch_complex_add(ChGC *gc, ChValue a, ChValue b) {
    double ar, ai, br, bi;
    if (!both_parts(a, b, &ar, &ai, &br, &bi)) {
        return CH_UNDEFINED;
    }
    return ch_make_complex(gc, ar + br, ai + bi);
}

ChValue ch_complex_sub(ChGC *gc, ChValue a, ChValue b) {
    double ar, ai, br, bi;
    if (!both_parts(a, b, &ar, &ai, &br, &bi)) {
        return CH_UNDEFINED;
    }
    return ch_make_complex(gc, ar - br, ai - bi);
}

ChValue ch_complex_mul(ChGC *gc, ChValue a, ChValue b) {
    double ar, ai, br, bi;
    if (!both_parts(a, b, &ar, &ai, &br, &bi)) {
        return CH_UNDEFINED;
    }
    return ch_make_complex(gc, ar * br - ai * bi, ar * bi + ai * br);
}

ChValue ch_complex_div(ChGC *gc, ChValue a, ChValue b) {
    double ar, ai, br, bi;
    if (!both_parts(a, b, &ar, &ai, &br, &bi)) {
        return CH_UNDEFINED;
    }
    double denom = br * br + bi * bi;
    if (denom == 0.0) {
        return CH_UNDEFINED;
    }
    return ch_make_complex(gc, (ar * br + ai * bi) / denom, (ai * br - ar * bi) / denom);
}

ChValue ch_complex_negate(ChGC *gc, ChValue a) {
    double ar, ai;
    if (!ch_complex_parts(a, &ar, &ai)) {
        return CH_UNDEFINED;
    }
    return ch_make_complex(gc, -ar, -ai);
}

char *ch_complex_to_string(ChValue v) {
    if (!ch_is_complex_obj(v)) {
        return NULL;
    }
    ChComplex *c = ch_as_complex(v);
    char buf[128];
    int n;
    if (c->imag < 0) {
        n = snprintf(buf, sizeof(buf), "%.16g%.16gi", c->real, c->imag);
    } else {
        n = snprintf(buf, sizeof(buf), "%.16g+%.16gi", c->real, c->imag);
    }
    if (n < 0) {
        return NULL;
    }
    char *out = (char *)malloc((size_t)n + 1);
    if (!out) {
        abort();
    }
    memcpy(out, buf, (size_t)n + 1);
    return out;
}
