#include "chaaya/rational.h"

#include "chaaya/bignum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_zero_int(ChValue v) {
    return ch_is_exact_integer(v) && ch_bignum_compare(v, ch_make_fixnum(0)) == 0;
}

static int is_one_int(ChValue v) {
    return ch_is_exact_integer(v) && ch_bignum_compare(v, ch_make_fixnum(1)) == 0;
}

static int is_neg_int(ChValue v) {
    return ch_is_exact_integer(v) && ch_bignum_compare(v, ch_make_fixnum(0)) < 0;
}

ChValue ch_bignum_gcd(ChGC *gc, ChValue a, ChValue b) {
    ChValue x = ch_bignum_abs(gc, a);
    ch_gc_push(gc, &x);
    ChValue y = ch_bignum_abs(gc, b);
    ch_gc_push(gc, &y);
    while (!is_zero_int(y)) {
        ChValue r = ch_bignum_remainder(gc, x, y);
        if (r == CH_UNDEFINED) {
            ch_gc_pop_n(gc, 2);
            return CH_UNDEFINED;
        }
        r = ch_bignum_abs(gc, r);
        x = y;
        y = r;
    }
    ChValue out = x;
    ch_gc_pop_n(gc, 2);
    return out;
}

void ch_exact_parts(ChValue v, ChValue *num_out, ChValue *den_out) {
    if (ch_is_rational_obj(v)) {
        ChRational *r = ch_as_rational(v);
        *num_out = r->numerator;
        *den_out = r->denominator;
        return;
    }
    *num_out = v;
    *den_out = ch_make_fixnum(1);
}

static ChValue alloc_rational(ChGC *gc, ChValue num, ChValue den) {
    ch_gc_push(gc, &num);
    ch_gc_push(gc, &den);
    ChRational *r = (ChRational *)ch_gc_alloc(gc, sizeof(ChRational), CH_TAG_RATIONAL);
    ch_gc_pop_n(gc, 2);
    r->numerator = num;
    r->denominator = den;
    return ch_make_pointer(&r->header);
}

ChValue ch_make_rational(ChGC *gc, ChValue num, ChValue den) {
    if (!ch_is_exact_integer(num) || !ch_is_exact_integer(den)) {
        return CH_UNDEFINED;
    }
    if (is_zero_int(den)) {
        return CH_UNDEFINED;
    }
    if (is_zero_int(num)) {
        return ch_make_fixnum(0);
    }

    ch_gc_push(gc, &num);
    ch_gc_push(gc, &den);

    if (is_neg_int(den)) {
        num = ch_bignum_negate(gc, num);
        den = ch_bignum_negate(gc, den);
    }

    ChValue g = ch_bignum_gcd(gc, num, den);
    ch_gc_push(gc, &g);
    if (!is_zero_int(g) && !is_one_int(g)) {
        num = ch_bignum_quotient(gc, num, g);
        den = ch_bignum_quotient(gc, den, g);
    }
    ch_gc_pop(gc); /* g */

    if (is_one_int(den)) {
        ch_gc_pop_n(gc, 2);
        return num;
    }

    ChValue out = alloc_rational(gc, num, den);
    ch_gc_pop_n(gc, 2);
    return out;
}

ChValue ch_exact_negate(ChGC *gc, ChValue a) {
    if (ch_is_exact_integer(a)) {
        return ch_bignum_negate(gc, a);
    }
    if (ch_is_rational_obj(a)) {
        ChRational *r = ch_as_rational(a);
        ChValue n = ch_bignum_negate(gc, r->numerator);
        return ch_make_rational(gc, n, r->denominator);
    }
    return CH_UNDEFINED;
}

ChValue ch_exact_abs(ChGC *gc, ChValue a) {
    if (ch_is_exact_integer(a)) {
        return ch_bignum_abs(gc, a);
    }
    if (ch_is_rational_obj(a)) {
        ChRational *r = ch_as_rational(a);
        ChValue n = ch_bignum_abs(gc, r->numerator);
        return ch_make_rational(gc, n, r->denominator);
    }
    return CH_UNDEFINED;
}

ChValue ch_exact_add(ChGC *gc, ChValue a, ChValue b) {
    ChValue an, ad, bn, bd;
    ch_exact_parts(a, &an, &ad);
    ch_exact_parts(b, &bn, &bd);
    ch_gc_push(gc, &an);
    ch_gc_push(gc, &ad);
    ch_gc_push(gc, &bn);
    ch_gc_push(gc, &bd);
    /* (an*bd + bn*ad) / (ad*bd) */
    ChValue t1 = ch_bignum_mul(gc, an, bd);
    ch_gc_push(gc, &t1);
    ChValue t2 = ch_bignum_mul(gc, bn, ad);
    ch_gc_push(gc, &t2);
    ChValue num = ch_bignum_add(gc, t1, t2);
    ch_gc_push(gc, &num);
    ChValue den = ch_bignum_mul(gc, ad, bd);
    ch_gc_push(gc, &den);
    ChValue out = ch_make_rational(gc, num, den);
    ch_gc_pop_n(gc, 8);
    return out;
}

ChValue ch_exact_sub(ChGC *gc, ChValue a, ChValue b) {
    ChValue nb = ch_exact_negate(gc, b);
    ch_gc_push(gc, &a);
    ch_gc_push(gc, &nb);
    ChValue out = ch_exact_add(gc, a, nb);
    ch_gc_pop_n(gc, 2);
    return out;
}

ChValue ch_exact_mul(ChGC *gc, ChValue a, ChValue b) {
    ChValue an, ad, bn, bd;
    ch_exact_parts(a, &an, &ad);
    ch_exact_parts(b, &bn, &bd);
    ch_gc_push(gc, &an);
    ch_gc_push(gc, &ad);
    ch_gc_push(gc, &bn);
    ch_gc_push(gc, &bd);
    ChValue num = ch_bignum_mul(gc, an, bn);
    ch_gc_push(gc, &num);
    ChValue den = ch_bignum_mul(gc, ad, bd);
    ch_gc_push(gc, &den);
    ChValue out = ch_make_rational(gc, num, den);
    ch_gc_pop_n(gc, 6);
    return out;
}

ChValue ch_exact_div(ChGC *gc, ChValue a, ChValue b) {
    ChValue bn, bd;
    ch_exact_parts(b, &bn, &bd);
    if (is_zero_int(bn)) {
        return CH_UNDEFINED;
    }
    /* a / (bn/bd) = a * (bd/bn) */
    ChValue recip = ch_make_rational(gc, bd, bn);
    if (recip == CH_UNDEFINED) {
        return CH_UNDEFINED;
    }
    ch_gc_push(gc, &a);
    ch_gc_push(gc, &recip);
    ChValue out = ch_exact_mul(gc, a, recip);
    ch_gc_pop_n(gc, 2);
    return out;
}

int ch_exact_compare(ChGC *gc, ChValue a, ChValue b) {
    if (ch_is_exact_integer(a) && ch_is_exact_integer(b)) {
        return ch_bignum_compare(a, b);
    }
    ChValue an, ad, bn, bd;
    ch_exact_parts(a, &an, &ad);
    ch_exact_parts(b, &bn, &bd);
    ch_gc_push(gc, &an);
    ch_gc_push(gc, &ad);
    ch_gc_push(gc, &bn);
    ch_gc_push(gc, &bd);
    ChValue left = ch_bignum_mul(gc, an, bd);
    ch_gc_push(gc, &left);
    ChValue right = ch_bignum_mul(gc, bn, ad);
    int c = ch_bignum_compare(left, right);
    ch_gc_pop_n(gc, 5);
    return c;
}

double ch_exact_to_f64(ChValue v) {
    if (ch_is_exact_integer(v)) {
        return ch_bignum_to_f64(v);
    }
    if (ch_is_rational_obj(v)) {
        ChRational *r = ch_as_rational(v);
        return ch_bignum_to_f64(r->numerator) / ch_bignum_to_f64(r->denominator);
    }
    if (ch_is_flonum(v)) {
        return ch_to_flonum(v);
    }
    return 0.0;
}

char *ch_exact_to_string(ChValue v) {
    if (ch_is_exact_integer(v)) {
        return ch_bignum_to_string(v);
    }
    if (!ch_is_rational_obj(v)) {
        return strdup("?");
    }
    ChRational *r = ch_as_rational(v);
    char *ns = ch_bignum_to_string(r->numerator);
    char *ds = ch_bignum_to_string(r->denominator);
    size_t n = strlen(ns) + strlen(ds) + 2;
    char *out = (char *)malloc(n);
    if (!out) {
        abort();
    }
    snprintf(out, n, "%s/%s", ns, ds);
    free(ns);
    free(ds);
    return out;
}
