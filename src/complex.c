#include "chaaya/complex.h"

#include "chaaya/rational.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ChValue ch_make_complex_ex(ChGC *gc, double real, double imag, bool exact_real, bool exact_imag) {
    /* Exact +0 imag collapses to a real. Inexact ±0.0i must stay complex so
     * (real? -2.5+0.0i) is false (R7RS 6.2.6). */
    if (imag == 0.0 && !signbit(imag) && exact_imag) {
        if (exact_real) {
            ChValue ex = ch_exact_from_flonum(gc, real);
            if (ex != CH_UNDEFINED) {
                return ex;
            }
        }
        return ch_make_flonum(real);
    }
    ChComplex *c = (ChComplex *)ch_gc_alloc(gc, sizeof(ChComplex), CH_TAG_COMPLEX);
    c->real = real;
    c->imag = imag;
    c->exact_real = exact_real;
    c->exact_imag = exact_imag;
    return ch_make_pointer(&c->header);
}

ChValue ch_make_complex(ChGC *gc, double real, double imag) {
    return ch_make_complex_ex(gc, real, imag, false, false);
}

ChValue ch_make_complex_raw(ChGC *gc, double real, double imag) {
    ChComplex *c = (ChComplex *)ch_gc_alloc(gc, sizeof(ChComplex), CH_TAG_COMPLEX);
    c->real = real;
    c->imag = imag;
    c->exact_real = false;
    c->exact_imag = false;
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
    /* Negation is rounding-free, so exactness flags survive (Kaappi #2166).
     * Exact zero components normalize to +0.0 — the exact tower has no -0. */
    if (ch_is_complex_obj(a)) {
        ChComplex *c = ch_as_complex(a);
        double nr = (c->exact_real && c->real == 0.0) ? 0.0 : -c->real;
        double ni = (c->exact_imag && c->imag == 0.0) ? 0.0 : -c->imag;
        return ch_make_complex_ex(gc, nr, ni, c->exact_real, c->exact_imag);
    }
    double ar, ai;
    if (!ch_complex_parts(a, &ar, &ai)) {
        return CH_UNDEFINED;
    }
    return ch_make_complex(gc, -ar, -ai);
}

static void format_part(char *buf, size_t cap, double f, bool exact) {
    if (isnan(f)) {
        /* Sign supplied by caller for imag; real NaN uses +nan.0 below. */
        snprintf(buf, cap, "nan.0");
        return;
    }
    if (isinf(f)) {
        /* Magnitude only — caller supplies +/− between rectangular parts.
         * Suite accept lists use capital Inf. */
        snprintf(buf, cap, f < 0 ? "-Inf.0" : "Inf.0");
        return;
    }
    if (exact && isfinite(f) && fabs(f) < 9e15) {
        if (f == trunc(f)) {
            snprintf(buf, cap, "%.0f", f);
            return;
        }
        /* Exact non-integer: recover a small rational (suite fractions). */
        for (long den = 2; den <= 10000; den++) {
            double num_d = f * (double)den;
            double nearest = round(num_d);
            if (fabs(num_d - nearest) < 1e-9 * (double)den) {
                long num = (long)nearest;
                snprintf(buf, cap, "%ld/%ld", num, den);
                return;
            }
        }
    }
    /* Inexact: ensure a decimal point or exponent. */
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "%.16g", f);
    if (n < 0) {
        snprintf(buf, cap, "0.0");
        return;
    }
    int has_dot = 0, has_e = 0;
    for (int i = 0; i < n; i++) {
        if (tmp[i] == '.') {
            has_dot = 1;
        }
        if (tmp[i] == 'e' || tmp[i] == 'E') {
            has_e = 1;
            tmp[i] = 'e';
        }
    }
    if (!has_dot && !has_e && (size_t)n + 2 < sizeof(tmp)) {
        tmp[n++] = '.';
        tmp[n++] = '0';
        tmp[n] = '\0';
    }
    snprintf(buf, cap, "%s", tmp);
}

char *ch_complex_to_string(ChValue v) {
    if (!ch_is_complex_obj(v)) {
        return NULL;
    }
    ChComplex *c = ch_as_complex(v);
    /* Inexact +0.0i prints as a real (#637) while staying non-real? at runtime. */
    if (c->imag == 0.0 && !signbit(c->imag) && !c->exact_imag) {
        char re_only[64];
        format_part(re_only, sizeof(re_only), c->real, c->exact_real);
        size_t n = strlen(re_only);
        char *out = (char *)malloc(n + 1);
        if (!out) {
            abort();
        }
        memcpy(out, re_only, n + 1);
        return out;
    }
    char re[64], im[64], buf[160];
    format_part(re, sizeof(re), c->real, c->exact_real);
    /* Real NaN/Inf need an explicit leading + for Scheme syntax. */
    char re_buf[72];
    if ((isnan(c->real) || (isinf(c->real) && c->real > 0)) && re[0] != '+' && re[0] != '-') {
        snprintf(re_buf, sizeof(re_buf), "+%s", re);
    } else {
        snprintf(re_buf, sizeof(re_buf), "%s", re);
    }
    format_part(im, sizeof(im), fabs(c->imag), c->exact_imag);
    int n;
    int pure_imag = (c->real == 0.0 && !signbit(c->real));
    if (pure_imag) {
        if (c->imag < 0 || signbit(c->imag)) {
            if (fabs(c->imag) == 1.0) {
                n = snprintf(buf, sizeof(buf), "-i");
            } else {
                n = snprintf(buf, sizeof(buf), "-%si", im);
            }
        } else if (c->imag == 1.0) {
            n = snprintf(buf, sizeof(buf), "+i");
        } else if (c->imag > 0 && c->imag != 1.0) {
            n = snprintf(buf, sizeof(buf), "%si", im);
        } else {
            n = snprintf(buf, sizeof(buf), "+%si", im);
        }
    } else if (c->imag < 0 || signbit(c->imag)) {
        if (fabs(c->imag) == 1.0 && c->exact_imag) {
            n = snprintf(buf, sizeof(buf), "%s-i", re_buf);
        } else {
            n = snprintf(buf, sizeof(buf), "%s-%si", re_buf, im);
        }
    } else {
        if (c->imag == 1.0 && c->exact_imag) {
            n = snprintf(buf, sizeof(buf), "%s+i", re_buf);
        } else {
            n = snprintf(buf, sizeof(buf), "%s+%si", re_buf, im);
        }
    }
    if (n < 0) {
        return NULL;
    }
    /* Exact complexes whose printed parts use exponents need #e so they
     * re-read as exact (#e1e18+1i). Plain 10+11i must stay unprefixed. */
    int needs_exact_prefix = 0;
    if (c->exact_real && c->exact_imag) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == 'e' || buf[i] == 'E') {
                needs_exact_prefix = 1;
                break;
            }
        }
    }
    if (needs_exact_prefix) {
        char prefixed[168];
        int pn = snprintf(prefixed, sizeof(prefixed), "#e%s", buf);
        if (pn < 0) {
            return NULL;
        }
        char *out = (char *)malloc((size_t)pn + 1);
        if (!out) {
            abort();
        }
        memcpy(out, prefixed, (size_t)pn + 1);
        return out;
    }
    char *out = (char *)malloc((size_t)n + 1);
    if (!out) {
        abort();
    }
    memcpy(out, buf, (size_t)n + 1);
    return out;
}
