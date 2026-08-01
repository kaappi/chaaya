#include "chaaya/prim.h"

#include "chaaya/bignum.h"
#include "chaaya/complex.h"
#include "chaaya/printer.h"
#include "chaaya/rational.h"
#include "chaaya/reader.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static int is_zero_int(ChValue v) {
    return ch_is_exact_integer(v) && ch_bignum_compare(v, ch_make_fixnum(0)) == 0;
}

static int as_real(ChValue v, double *out) {
    double re, im;
    if (!ch_complex_parts(v, &re, &im) || im != 0.0) {
        return 0;
    }
    *out = re;
    return 1;
}

static int require_real(ChVM *vm, ChValue v, const char *who, double *out) {
    if (!as_real(v, out) || ch_is_complex_obj(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected real number", who);
        return 0;
    }
    return 1;
}

/* ---- rounding ---- */

static ChValue rational_trunc_quot(ChGC *gc, ChValue v) {
    ChValue num, den;
    ch_exact_parts(v, &num, &den);
    return ch_bignum_quotient(gc, num, den);
}

static ChValue rational_floor(ChGC *gc, ChValue v) {
    ChValue num, den;
    ch_exact_parts(v, &num, &den);
    ch_gc_push(gc, &num);
    ch_gc_push(gc, &den);
    ChValue q = ch_bignum_quotient(gc, num, den);
    ch_gc_push(gc, &q);
    ChValue r = ch_bignum_remainder(gc, num, den);
    if (!is_zero_int(r) && ch_bignum_compare(num, ch_make_fixnum(0)) < 0) {
        q = ch_bignum_sub(gc, q, ch_make_fixnum(1));
    }
    ch_gc_pop_n(gc, 3);
    return q;
}

static ChValue rational_ceiling(ChGC *gc, ChValue v) {
    ChValue num, den;
    ch_exact_parts(v, &num, &den);
    ch_gc_push(gc, &num);
    ch_gc_push(gc, &den);
    ChValue q = ch_bignum_quotient(gc, num, den);
    ch_gc_push(gc, &q);
    ChValue r = ch_bignum_remainder(gc, num, den);
    if (!is_zero_int(r) && ch_bignum_compare(num, ch_make_fixnum(0)) > 0) {
        q = ch_bignum_add(gc, q, ch_make_fixnum(1));
    }
    ch_gc_pop_n(gc, 3);
    return q;
}

static double bankers_round(double f) {
    double floored = floor(f);
    double frac = fabs(f - floored);
    if (frac == 0.5) {
        /* ties to even */
        if (fmod(floored, 2.0) == 0.0) {
            return floored;
        }
        return ceil(f);
    }
    return round(f);
}

static ChValue prim_floor(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact_integer(args[0])) {
        return args[0];
    }
    if (ch_is_rational_obj(args[0])) {
        return rational_floor(&vm->gc, args[0]);
    }
    double x;
    if (!require_real(vm, args[0], "floor", &x)) {
        return CH_UNDEFINED;
    }
    return ch_make_flonum(floor(x));
}

static ChValue prim_ceiling(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact_integer(args[0])) {
        return args[0];
    }
    if (ch_is_rational_obj(args[0])) {
        return rational_ceiling(&vm->gc, args[0]);
    }
    double x;
    if (!require_real(vm, args[0], "ceiling", &x)) {
        return CH_UNDEFINED;
    }
    return ch_make_flonum(ceil(x));
}

static ChValue prim_truncate(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact_integer(args[0])) {
        return args[0];
    }
    if (ch_is_rational_obj(args[0])) {
        return rational_trunc_quot(&vm->gc, args[0]);
    }
    double x;
    if (!require_real(vm, args[0], "truncate", &x)) {
        return CH_UNDEFINED;
    }
    return ch_make_flonum(trunc(x));
}

static ChValue prim_round(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact_integer(args[0])) {
        return args[0];
    }
    if (ch_is_rational_obj(args[0])) {
        /* floor(x + 1/2) with exact arith */
        ChValue half = ch_make_rational(&vm->gc, ch_make_fixnum(1), ch_make_fixnum(2));
        ch_gc_push(&vm->gc, &half);
        ChValue sum = ch_exact_add(&vm->gc, args[0], half);
        ch_gc_pop(&vm->gc);
        return rational_floor(&vm->gc, sum);
    }
    double x;
    if (!require_real(vm, args[0], "round", &x)) {
        return CH_UNDEFINED;
    }
    return ch_make_flonum(bankers_round(x));
}

/* ---- exactness ---- */

static ChValue exact_from_flonum(ChGC *gc, double f) {
    if (!isfinite(f)) {
        return CH_UNDEFINED;
    }
    if (f == 0.0) {
        return ch_make_fixnum(0);
    }
    if (f == trunc(f)) {
        if (f >= (double)CH_FIXNUM_MIN && f <= (double)CH_FIXNUM_MAX) {
            return ch_make_fixnum((int64_t)f);
        }
        if (f >= (double)INT64_MIN && f <= (double)INT64_MAX) {
            return ch_make_integer(gc, (int64_t)f);
        }
        /* Fall through to IEEE rational for huge integral floats. */
    }

    union {
        double d;
        uint64_t u;
    } bits;
    bits.d = fabs(f);
    int negative = f < 0;
    uint64_t u = bits.u;
    int raw_exp = (int)((u >> 52) & 0x7FF);
    uint64_t mantissa = (raw_exp == 0) ? (u & 0x000FFFFFFFFFFFFFULL)
                                       : ((u & 0x000FFFFFFFFFFFFFULL) | 0x0010000000000000ULL);
    int exp = (raw_exp == 0) ? (1 - 1023 - 52) : (raw_exp - 1023 - 52);

    if (exp >= 0) {
        /* mantissa * 2^exp */
        ChValue num = ch_make_integer(gc, 0);
        /* Build from mantissa via repeated *2^32 chunks if needed — use double path
         * only when mantissa fits, else shift. */
        if (mantissa <= (uint64_t)INT64_MAX) {
            num = ch_make_integer(gc, (int64_t)mantissa);
        } else {
            /* two limbs: lo + hi<<32 conceptually; use mul by 2^32 */
            uint64_t lo = mantissa & 0xFFFFFFFFULL;
            uint64_t hi = mantissa >> 32;
            ChValue hi_v = ch_make_integer(gc, (int64_t)hi);
            ch_gc_push(gc, &hi_v);
            ChValue shift = ch_make_integer(gc, 1LL << 32);
            ch_gc_push(gc, &shift);
            ChValue hi_shifted = ch_bignum_mul(gc, hi_v, shift);
            ch_gc_pop_n(gc, 2);
            ch_gc_push(gc, &hi_shifted);
            ChValue lo_v = ch_make_integer(gc, (int64_t)lo);
            num = ch_bignum_add(gc, hi_shifted, lo_v);
            ch_gc_pop(gc);
        }
        ch_gc_push(gc, &num);
        for (int i = 0; i < exp; i++) {
            num = ch_bignum_add(gc, num, num);
        }
        if (negative) {
            num = ch_bignum_negate(gc, num);
        }
        ch_gc_pop(gc);
        return num;
    }

    /* mantissa / 2^(-exp) */
    unsigned neg_exp = (unsigned)(-exp);
    while ((mantissa & 1ULL) == 0 && neg_exp > 0) {
        mantissa >>= 1;
        neg_exp--;
    }
    ChValue num;
    if (mantissa <= (uint64_t)INT64_MAX) {
        num = ch_make_integer(gc, negative ? -(int64_t)mantissa : (int64_t)mantissa);
    } else {
        uint64_t lo = mantissa & 0xFFFFFFFFULL;
        uint64_t hi = mantissa >> 32;
        ChValue hi_v = ch_make_integer(gc, (int64_t)hi);
        ch_gc_push(gc, &hi_v);
        ChValue shift = ch_make_integer(gc, 1LL << 32);
        ChValue hi_shifted = ch_bignum_mul(gc, hi_v, shift);
        ch_gc_pop(gc);
        ch_gc_push(gc, &hi_shifted);
        num = ch_bignum_add(gc, hi_shifted, ch_make_integer(gc, (int64_t)lo));
        ch_gc_pop(gc);
        if (negative) {
            num = ch_bignum_negate(gc, num);
        }
    }
    ch_gc_push(gc, &num);
    ChValue den = ch_make_fixnum(1);
    ch_gc_push(gc, &den);
    for (unsigned i = 0; i < neg_exp; i++) {
        den = ch_bignum_add(gc, den, den);
    }
    ChValue rat = ch_make_rational(gc, num, den);
    ch_gc_pop_n(gc, 2);
    return rat;
}

static ChValue prim_exact(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact(args[0])) {
        return args[0];
    }
    if (ch_is_flonum(args[0])) {
        ChValue r = exact_from_flonum(&vm->gc, ch_to_flonum(args[0]));
        if (r == CH_UNDEFINED) {
            snprintf(vm->error, sizeof(vm->error), "exact: expected finite number");
        }
        return r;
    }
    if (ch_is_complex_obj(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "exact: inexact complex not supported");
        return CH_UNDEFINED;
    }
    snprintf(vm->error, sizeof(vm->error), "exact: not a number");
    return CH_UNDEFINED;
}

static ChValue prim_inexact(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_flonum(args[0]) || ch_is_complex_obj(args[0])) {
        return args[0];
    }
    double re, im;
    if (!ch_complex_parts(args[0], &re, &im)) {
        snprintf(vm->error, sizeof(vm->error), "inexact: not a number");
        return CH_UNDEFINED;
    }
    return ch_make_complex(&vm->gc, re, im);
}

/* ---- predicates / min-max ---- */

static ChValue prim_positive_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact(args[0])) {
        return ch_exact_compare(&vm->gc, args[0], ch_make_fixnum(0)) > 0 ? CH_TRUE : CH_FALSE;
    }
    double x;
    if (!require_real(vm, args[0], "positive?", &x)) {
        return CH_UNDEFINED;
    }
    return x > 0.0 ? CH_TRUE : CH_FALSE;
}

static ChValue prim_negative_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact(args[0])) {
        return ch_exact_compare(&vm->gc, args[0], ch_make_fixnum(0)) < 0 ? CH_TRUE : CH_FALSE;
    }
    double x;
    if (!require_real(vm, args[0], "negative?", &x)) {
        return CH_UNDEFINED;
    }
    return x < 0.0 ? CH_TRUE : CH_FALSE;
}

static ChValue prim_odd_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_exact_integer(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "odd?: expected exact integer");
        return CH_UNDEFINED;
    }
    ChValue two = ch_make_fixnum(2);
    ChValue r = ch_bignum_remainder(&vm->gc, args[0], two);
    return !is_zero_int(r) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_even_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_exact_integer(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "even?: expected exact integer");
        return CH_UNDEFINED;
    }
    ChValue two = ch_make_fixnum(2);
    ChValue r = ch_bignum_remainder(&vm->gc, args[0], two);
    return is_zero_int(r) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_max(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 0) {
        snprintf(vm->error, sizeof(vm->error), "max: needs at least one argument");
        return CH_UNDEFINED;
    }
    int any_inexact = 0;
    for (int i = 0; i < nargs; i++) {
        if (ch_is_complex_obj(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "max: complex numbers not ordered");
            return CH_UNDEFINED;
        }
        if (!ch_is_number(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "max: not a number");
            return CH_UNDEFINED;
        }
        if (!ch_is_exact(args[i])) {
            any_inexact = 1;
        }
    }
    ChValue best = args[0];
    ch_gc_push(&vm->gc, &best);
    for (int i = 1; i < nargs; i++) {
        int cmp;
        if (ch_is_exact(best) && ch_is_exact(args[i])) {
            cmp = ch_exact_compare(&vm->gc, best, args[i]);
        } else {
            double a, b;
            (void)as_real(best, &a);
            (void)as_real(args[i], &b);
            cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
        }
        if (cmp < 0) {
            best = args[i];
        }
    }
    if (any_inexact && ch_is_exact(best)) {
        best = ch_make_flonum(ch_exact_to_f64(best));
    }
    ch_gc_pop(&vm->gc);
    return best;
}

static ChValue prim_min(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 0) {
        snprintf(vm->error, sizeof(vm->error), "min: needs at least one argument");
        return CH_UNDEFINED;
    }
    int any_inexact = 0;
    for (int i = 0; i < nargs; i++) {
        if (ch_is_complex_obj(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "min: complex numbers not ordered");
            return CH_UNDEFINED;
        }
        if (!ch_is_number(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "min: not a number");
            return CH_UNDEFINED;
        }
        if (!ch_is_exact(args[i])) {
            any_inexact = 1;
        }
    }
    ChValue best = args[0];
    ch_gc_push(&vm->gc, &best);
    for (int i = 1; i < nargs; i++) {
        int cmp;
        if (ch_is_exact(best) && ch_is_exact(args[i])) {
            cmp = ch_exact_compare(&vm->gc, best, args[i]);
        } else {
            double a, b;
            (void)as_real(best, &a);
            (void)as_real(args[i], &b);
            cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
        }
        if (cmp > 0) {
            best = args[i];
        }
    }
    if (any_inexact && ch_is_exact(best)) {
        best = ch_make_flonum(ch_exact_to_f64(best));
    }
    ch_gc_pop(&vm->gc);
    return best;
}

/* ---- integer division helpers ---- */

static ChValue prim_modulo(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_exact_integer(args[0]) || !ch_is_exact_integer(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "modulo: expected exact integers");
        return CH_UNDEFINED;
    }
    if (is_zero_int(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "modulo: division by zero");
        return CH_UNDEFINED;
    }
    /* floor-remainder: a - b * floor(a/b) */
    ChValue a = args[0];
    ChValue b = args[1];
    ch_gc_push(&vm->gc, &a);
    ch_gc_push(&vm->gc, &b);
    ChValue q = ch_bignum_quotient(&vm->gc, a, b);
    ch_gc_push(&vm->gc, &q);
    ChValue r = ch_bignum_remainder(&vm->gc, a, b);
    ch_gc_push(&vm->gc, &r);
    if (!is_zero_int(r)) {
        int a_neg = ch_bignum_compare(a, ch_make_fixnum(0)) < 0;
        int b_neg = ch_bignum_compare(b, ch_make_fixnum(0)) < 0;
        if (a_neg != b_neg) {
            /* floor quot is trunc - 1 when signs differ and rem != 0 */
            q = ch_bignum_sub(&vm->gc, q, ch_make_fixnum(1));
            ChValue qb = ch_bignum_mul(&vm->gc, q, b);
            ch_gc_push(&vm->gc, &qb);
            r = ch_bignum_sub(&vm->gc, a, qb);
            ch_gc_pop(&vm->gc);
        }
    }
    ch_gc_pop_n(&vm->gc, 4);
    return r;
}

static ChValue prim_gcd(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 0) {
        return ch_make_fixnum(0);
    }
    ChValue acc = args[0];
    if (!ch_is_exact_integer(acc)) {
        snprintf(vm->error, sizeof(vm->error), "gcd: expected exact integers");
        return CH_UNDEFINED;
    }
    ch_gc_push(&vm->gc, &acc);
    for (int i = 1; i < nargs; i++) {
        if (!ch_is_exact_integer(args[i])) {
            ch_gc_pop(&vm->gc);
            snprintf(vm->error, sizeof(vm->error), "gcd: expected exact integers");
            return CH_UNDEFINED;
        }
        acc = ch_bignum_gcd(&vm->gc, acc, args[i]);
    }
    acc = ch_bignum_abs(&vm->gc, acc);
    ch_gc_pop(&vm->gc);
    return acc;
}

static ChValue prim_lcm(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 0) {
        return ch_make_fixnum(1);
    }
    ChValue acc = ch_bignum_abs(&vm->gc, args[0]);
    if (!ch_is_exact_integer(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "lcm: expected exact integers");
        return CH_UNDEFINED;
    }
    ch_gc_push(&vm->gc, &acc);
    for (int i = 1; i < nargs; i++) {
        if (!ch_is_exact_integer(args[i])) {
            ch_gc_pop(&vm->gc);
            snprintf(vm->error, sizeof(vm->error), "lcm: expected exact integers");
            return CH_UNDEFINED;
        }
        ChValue b = ch_bignum_abs(&vm->gc, args[i]);
        ch_gc_push(&vm->gc, &b);
        if (is_zero_int(acc) || is_zero_int(b)) {
            acc = ch_make_fixnum(0);
            ch_gc_pop(&vm->gc);
            continue;
        }
        ChValue g = ch_bignum_gcd(&vm->gc, acc, b);
        ch_gc_push(&vm->gc, &g);
        ChValue q = ch_bignum_quotient(&vm->gc, acc, g);
        ch_gc_pop(&vm->gc); /* g */
        ch_gc_push(&vm->gc, &q);
        acc = ch_bignum_mul(&vm->gc, q, b);
        ch_gc_pop_n(&vm->gc, 2); /* q, b */
    }
    ch_gc_pop(&vm->gc);
    return acc;
}

/* ---- powers / square ---- */

static ChValue prim_square(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue a = args[0];
    ch_gc_push(&vm->gc, &a);
    ChValue r;
    if (ch_is_exact(a)) {
        r = ch_exact_mul(&vm->gc, a, a);
    } else if (ch_is_complex_obj(a) || ch_is_flonum(a)) {
        r = ch_complex_mul(&vm->gc, a, a);
    } else {
        ch_gc_pop(&vm->gc);
        snprintf(vm->error, sizeof(vm->error), "square: not a number");
        return CH_UNDEFINED;
    }
    ch_gc_pop(&vm->gc);
    return r;
}

static ChValue expt_nonneg_int(ChGC *gc, ChValue base, ChValue exp) {
    ChValue result = ch_make_fixnum(1);
    ch_gc_push(gc, &result);
    ch_gc_push(gc, &base);
    ChValue e = exp;
    ch_gc_push(gc, &e);
    ChValue two = ch_make_fixnum(2);
    while (ch_bignum_compare(e, ch_make_fixnum(0)) > 0) {
        ChValue rem = ch_bignum_remainder(gc, e, two);
        if (!is_zero_int(rem)) {
            result = ch_bignum_mul(gc, result, base);
        }
        e = ch_bignum_quotient(gc, e, two);
        if (ch_bignum_compare(e, ch_make_fixnum(0)) > 0) {
            base = ch_bignum_mul(gc, base, base);
        }
    }
    ch_gc_pop_n(gc, 3);
    return result;
}

static ChValue prim_expt(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue z = args[0];
    ChValue w = args[1];
    if (ch_is_exact_integer(z) && ch_is_exact_integer(w)) {
        if (ch_bignum_compare(w, ch_make_fixnum(0)) >= 0) {
            return expt_nonneg_int(&vm->gc, z, w);
        }
        /* z^neg = 1 / z^|neg| */
        ChValue pos = ch_bignum_negate(&vm->gc, w);
        ch_gc_push(&vm->gc, &pos);
        ChValue pow = expt_nonneg_int(&vm->gc, z, pos);
        ch_gc_pop(&vm->gc);
        if (is_zero_int(pow)) {
            snprintf(vm->error, sizeof(vm->error), "expt: division by zero");
            return CH_UNDEFINED;
        }
        return ch_make_rational(&vm->gc, ch_make_fixnum(1), pow);
    }
    double a, b;
    if (!as_real(z, &a) || !as_real(w, &b) || ch_is_complex_obj(z) || ch_is_complex_obj(w)) {
        /* Complex expt: defer — error for MVP unless both real */
        snprintf(vm->error, sizeof(vm->error), "expt: complex exponents not supported");
        return CH_UNDEFINED;
    }
    return ch_make_flonum(pow(a, b));
}

/* ---- inexact math ---- */

static ChValue unary_real_math(ChVM *vm, ChValue *args, const char *who, double (*fn)(double)) {
    double x;
    if (!require_real(vm, args[0], who, &x)) {
        return CH_UNDEFINED;
    }
    return ch_make_flonum(fn(x));
}

static ChValue prim_sqrt(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_complex_obj(args[0])) {
        double re, im;
        ch_complex_parts(args[0], &re, &im);
        double r = hypot(re, im);
        double theta = atan2(im, re) / 2.0;
        double sr = sqrt(r);
        return ch_make_complex(&vm->gc, sr * cos(theta), sr * sin(theta));
    }
    double x;
    if (!require_real(vm, args[0], "sqrt", &x)) {
        return CH_UNDEFINED;
    }
    if (x < 0) {
        return ch_make_complex(&vm->gc, 0.0, sqrt(-x));
    }
    return ch_make_flonum(sqrt(x));
}

static ChValue prim_sin(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return unary_real_math(vm, args, "sin", sin);
}
static ChValue prim_cos(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return unary_real_math(vm, args, "cos", cos);
}
static ChValue prim_tan(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return unary_real_math(vm, args, "tan", tan);
}
static ChValue prim_asin(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return unary_real_math(vm, args, "asin", asin);
}
static ChValue prim_acos(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return unary_real_math(vm, args, "acos", acos);
}
static ChValue prim_exp(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return unary_real_math(vm, args, "exp", exp);
}

static ChValue prim_atan(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 1) {
        return unary_real_math(vm, args, "atan", atan);
    }
    double y, x;
    if (!require_real(vm, args[0], "atan", &y) || !require_real(vm, args[1], "atan", &x)) {
        return CH_UNDEFINED;
    }
    return ch_make_flonum(atan2(y, x));
}

static ChValue prim_log(ChVM *vm, ChValue *args, int nargs) {
    double x;
    if (!require_real(vm, args[0], "log", &x)) {
        return CH_UNDEFINED;
    }
    if (nargs == 1) {
        return ch_make_flonum(log(x));
    }
    double b;
    if (!require_real(vm, args[1], "log", &b)) {
        return CH_UNDEFINED;
    }
    return ch_make_flonum(log(x) / log(b));
}

static ChValue prim_finite_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (ch_is_exact(args[0])) {
        return CH_TRUE;
    }
    if (ch_is_flonum(args[0])) {
        return isfinite(ch_to_flonum(args[0])) ? CH_TRUE : CH_FALSE;
    }
    if (ch_is_complex_obj(args[0])) {
        ChComplex *c = ch_as_complex(args[0]);
        return (isfinite(c->real) && isfinite(c->imag)) ? CH_TRUE : CH_FALSE;
    }
    return CH_FALSE;
}

static ChValue prim_infinite_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (ch_is_flonum(args[0])) {
        return isinf(ch_to_flonum(args[0])) ? CH_TRUE : CH_FALSE;
    }
    if (ch_is_complex_obj(args[0])) {
        ChComplex *c = ch_as_complex(args[0]);
        return (isinf(c->real) || isinf(c->imag)) ? CH_TRUE : CH_FALSE;
    }
    return CH_FALSE;
}

static ChValue prim_nan_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (ch_is_flonum(args[0])) {
        double d = ch_to_flonum(args[0]);
        return (d != d) ? CH_TRUE : CH_FALSE;
    }
    if (ch_is_complex_obj(args[0])) {
        ChComplex *c = ch_as_complex(args[0]);
        return (c->real != c->real || c->imag != c->imag) ? CH_TRUE : CH_FALSE;
    }
    return CH_FALSE;
}

/* ---- exact-integer-sqrt ---- */

static ChValue prim_exact_integer_sqrt(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_exact_integer(args[0]) || ch_bignum_compare(args[0], ch_make_fixnum(0)) < 0) {
        snprintf(vm->error, sizeof(vm->error), "exact-integer-sqrt: expected non-negative exact integer");
        return CH_UNDEFINED;
    }
    if (is_zero_int(args[0]) || ch_bignum_compare(args[0], ch_make_fixnum(1)) == 0) {
        ChValue vals[2] = {args[0], ch_make_fixnum(0)};
        return ch_gc_make_values(&vm->gc, vals, 2);
    }
    /* Newton's method on integers: s := (s + n/s) / 2 */
    ChValue n = args[0];
    ch_gc_push(&vm->gc, &n);
    ChValue s = ch_make_fixnum(1);
    /* rough start from f64 when possible */
    double approx = ch_bignum_to_f64(n);
    if (isfinite(approx) && approx > 0) {
        s = ch_make_integer(&vm->gc, (int64_t)sqrt(approx));
        if (is_zero_int(s)) {
            s = ch_make_fixnum(1);
        }
    }
    ch_gc_push(&vm->gc, &s);
    for (;;) {
        ChValue q = ch_bignum_quotient(&vm->gc, n, s);
        ch_gc_push(&vm->gc, &q);
        ChValue sum = ch_bignum_add(&vm->gc, s, q);
        ch_gc_pop(&vm->gc);
        ChValue next = ch_bignum_quotient(&vm->gc, sum, ch_make_fixnum(2));
        if (ch_bignum_compare(next, s) >= 0) {
            break;
        }
        s = next;
    }
    ChValue sq = ch_bignum_mul(&vm->gc, s, s);
    ch_gc_push(&vm->gc, &sq);
    ChValue rem = ch_bignum_sub(&vm->gc, n, sq);
    ch_gc_pop_n(&vm->gc, 3);
    ChValue vals[2] = {s, rem};
    ch_gc_push(&vm->gc, &vals[0]);
    ch_gc_push(&vm->gc, &vals[1]);
    ChValue out = ch_gc_make_values(&vm->gc, vals, 2);
    ch_gc_pop_n(&vm->gc, 2);
    return out;
}

/* ---- string conversion ---- */

static ChValue prim_number_to_string(ChVM *vm, ChValue *args, int nargs) {
    if (!ch_is_number(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "number->string: not a number");
        return CH_UNDEFINED;
    }
    int radix = 10;
    if (nargs >= 2) {
        if (!ch_is_fixnum(args[1])) {
            snprintf(vm->error, sizeof(vm->error), "number->string: bad radix");
            return CH_UNDEFINED;
        }
        radix = (int)ch_to_fixnum(args[1]);
        if (radix != 2 && radix != 8 && radix != 10 && radix != 16) {
            snprintf(vm->error, sizeof(vm->error), "number->string: unsupported radix");
            return CH_UNDEFINED;
        }
    }
    if (radix != 10) {
        if (!ch_is_exact_integer(args[0])) {
            snprintf(vm->error, sizeof(vm->error), "number->string: non-decimal radix needs integer");
            return CH_UNDEFINED;
        }
        /* Simple digit conversion for exact integers. */
        if (is_zero_int(args[0])) {
            return ch_gc_make_string_cstr(&vm->gc, "0");
        }
        ChValue n = ch_bignum_abs(&vm->gc, args[0]);
        ch_gc_push(&vm->gc, &n);
        char buf[512];
        size_t pos = sizeof(buf);
        buf[--pos] = '\0';
        ChValue base = ch_make_fixnum(radix);
        while (!is_zero_int(n)) {
            ChValue dig = ch_bignum_remainder(&vm->gc, n, base);
            int d = (int)ch_to_fixnum(dig);
            char ch = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
            if (pos == 0) {
                ch_gc_pop(&vm->gc);
                snprintf(vm->error, sizeof(vm->error), "number->string: overflow");
                return CH_UNDEFINED;
            }
            buf[--pos] = ch;
            n = ch_bignum_quotient(&vm->gc, n, base);
        }
        if (ch_bignum_compare(args[0], ch_make_fixnum(0)) < 0) {
            if (pos == 0) {
                ch_gc_pop(&vm->gc);
                snprintf(vm->error, sizeof(vm->error), "number->string: overflow");
                return CH_UNDEFINED;
            }
            buf[--pos] = '-';
        }
        ch_gc_pop(&vm->gc);
        return ch_gc_make_string_cstr(&vm->gc, buf + pos);
    }
    char *s = ch_value_to_string(args[0], false);
    ChValue out = ch_gc_make_string_cstr(&vm->gc, s);
    free(s);
    return out;
}

static int digit_value(char c, int radix) {
    if (c >= '0' && c <= '9') {
        int d = c - '0';
        return d < radix ? d : -1;
    }
    if (c >= 'a' && c <= 'z') {
        int d = 10 + (c - 'a');
        return d < radix ? d : -1;
    }
    if (c >= 'A' && c <= 'Z') {
        int d = 10 + (c - 'A');
        return d < radix ? d : -1;
    }
    return -1;
}

static ChValue parse_integer_radix(ChGC *gc, const char *text, size_t len, int radix) {
    size_t i = 0;
    int positive = 1;
    if (i < len && (text[i] == '+' || text[i] == '-')) {
        positive = text[i] != '-';
        i++;
    }
    if (i >= len) {
        return CH_UNDEFINED;
    }
    ChValue acc = ch_make_fixnum(0);
    ch_gc_push(gc, &acc);
    ChValue base = ch_make_fixnum(radix);
    int any = 0;
    for (; i < len; i++) {
        int d = digit_value(text[i], radix);
        if (d < 0) {
            ch_gc_pop(gc);
            return CH_UNDEFINED;
        }
        any = 1;
        acc = ch_bignum_mul(gc, acc, base);
        acc = ch_bignum_add(gc, acc, ch_make_fixnum(d));
    }
    if (!any) {
        ch_gc_pop(gc);
        return CH_UNDEFINED;
    }
    if (!positive) {
        acc = ch_bignum_negate(gc, acc);
    }
    ch_gc_pop(gc);
    return acc;
}

static ChValue prim_string_to_number(ChVM *vm, ChValue *args, int nargs) {
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string->number: expected string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    int radix = 10;
    if (nargs >= 2) {
        if (!ch_is_fixnum(args[1])) {
            return CH_FALSE;
        }
        radix = (int)ch_to_fixnum(args[1]);
        if (radix != 2 && radix != 8 && radix != 10 && radix != 16) {
            return CH_FALSE;
        }
    }
    if (s->len == 0) {
        return CH_FALSE;
    }
    if (radix == 10) {
        ChValue v;
        if (!ch_parse_number(&vm->gc, s->data, s->len, &v)) {
            return CH_FALSE;
        }
        return v;
    }
    /* Non-decimal: integers and a/b rationals only. */
    const char *slash = NULL;
    for (size_t i = 0; i < s->len; i++) {
        if (s->data[i] == '/') {
            slash = s->data + i;
            break;
        }
    }
    if (slash) {
        size_t nlen = (size_t)(slash - s->data);
        size_t dlen = s->len - nlen - 1;
        ChValue num = parse_integer_radix(&vm->gc, s->data, nlen, radix);
        if (num == CH_UNDEFINED) {
            return CH_FALSE;
        }
        ch_gc_push(&vm->gc, &num);
        ChValue den = parse_integer_radix(&vm->gc, slash + 1, dlen, radix);
        if (den == CH_UNDEFINED) {
            ch_gc_pop(&vm->gc);
            return CH_FALSE;
        }
        ch_gc_push(&vm->gc, &den);
        ChValue rat = ch_make_rational(&vm->gc, num, den);
        ch_gc_pop_n(&vm->gc, 2);
        return rat == CH_UNDEFINED ? CH_FALSE : rat;
    }
    ChValue v = parse_integer_radix(&vm->gc, s->data, s->len, radix);
    return v == CH_UNDEFINED ? CH_FALSE : v;
}

void ch_register_math_primitives(ChVM *vm) {
    define_prim(vm, "floor", prim_floor, 1, 1);
    define_prim(vm, "ceiling", prim_ceiling, 1, 1);
    define_prim(vm, "truncate", prim_truncate, 1, 1);
    define_prim(vm, "round", prim_round, 1, 1);
    define_prim(vm, "exact", prim_exact, 1, 1);
    define_prim(vm, "inexact", prim_inexact, 1, 1);
    define_prim(vm, "positive?", prim_positive_p, 1, 1);
    define_prim(vm, "negative?", prim_negative_p, 1, 1);
    define_prim(vm, "odd?", prim_odd_p, 1, 1);
    define_prim(vm, "even?", prim_even_p, 1, 1);
    define_prim(vm, "max", prim_max, -1, 1);
    define_prim(vm, "min", prim_min, -1, 1);
    define_prim(vm, "modulo", prim_modulo, 2, 2);
    define_prim(vm, "gcd", prim_gcd, -1, 0);
    define_prim(vm, "lcm", prim_lcm, -1, 0);
    define_prim(vm, "square", prim_square, 1, 1);
    define_prim(vm, "expt", prim_expt, 2, 2);
    define_prim(vm, "sqrt", prim_sqrt, 1, 1);
    define_prim(vm, "sin", prim_sin, 1, 1);
    define_prim(vm, "cos", prim_cos, 1, 1);
    define_prim(vm, "tan", prim_tan, 1, 1);
    define_prim(vm, "asin", prim_asin, 1, 1);
    define_prim(vm, "acos", prim_acos, 1, 1);
    define_prim(vm, "atan", prim_atan, -1, 1);
    define_prim(vm, "exp", prim_exp, 1, 1);
    define_prim(vm, "log", prim_log, -1, 1);
    define_prim(vm, "finite?", prim_finite_p, 1, 1);
    define_prim(vm, "infinite?", prim_infinite_p, 1, 1);
    define_prim(vm, "nan?", prim_nan_p, 1, 1);
    define_prim(vm, "exact-integer-sqrt", prim_exact_integer_sqrt, 1, 1);
    define_prim(vm, "number->string", prim_number_to_string, -1, 1);
    define_prim(vm, "string->number", prim_string_to_number, -1, 1);
}
