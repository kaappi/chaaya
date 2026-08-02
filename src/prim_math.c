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

static double gcd_double(double a, double b) {
    a = fabs(a);
    b = fabs(b);
    while (b != 0.0) {
        double t = fmod(a, b);
        a = b;
        b = t;
    }
    return a;
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

static int is_even_int(ChValue v) {
    if (ch_is_fixnum(v)) {
        return (ch_to_fixnum(v) & 1) == 0;
    }
    if (ch_is_bignum(v)) {
        ChBignum *bn = ch_as_bignum(v);
        return bn->len == 0 || (bn->limbs[0] & 1ULL) == 0;
    }
    return 0;
}

static ChValue rational_round(ChGC *gc, ChValue v) {
    ChValue num, den;
    ch_exact_parts(v, &num, &den);
    ch_gc_push(gc, &num);
    ch_gc_push(gc, &den);
    ChValue q = ch_bignum_quotient(gc, num, den);
    ch_gc_push(gc, &q);
    ChValue rem = ch_bignum_remainder(gc, num, den);
    if (is_zero_int(rem)) {
        ch_gc_pop_n(gc, 3);
        return q;
    }
    ch_gc_push(gc, &rem);
    ChValue abs_rem = ch_bignum_abs(gc, rem);
    ch_gc_push(gc, &abs_rem);
    ChValue double_rem = ch_bignum_mul(gc, abs_rem, ch_make_fixnum(2));
    int cmp = ch_bignum_compare(double_rem, den);
    ChValue out = q;
    if (cmp < 0) {
        out = q;
    } else if (cmp > 0) {
        if (ch_bignum_compare(rem, ch_make_fixnum(0)) < 0) {
            out = ch_bignum_sub(gc, q, ch_make_fixnum(1));
        } else {
            out = ch_bignum_add(gc, q, ch_make_fixnum(1));
        }
    } else {
        /* Exact half — ties to even */
        if (!is_even_int(q)) {
            if (ch_bignum_compare(rem, ch_make_fixnum(0)) < 0) {
                out = ch_bignum_sub(gc, q, ch_make_fixnum(1));
            } else {
                out = ch_bignum_add(gc, q, ch_make_fixnum(1));
            }
        }
    }
    ch_gc_pop_n(gc, 5);
    return out;
}

static ChValue prim_round(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact_integer(args[0])) {
        return args[0];
    }
    if (ch_is_rational_obj(args[0])) {
        return rational_round(&vm->gc, args[0]);
    }
    double x;
    if (!require_real(vm, args[0], "round", &x)) {
        return CH_UNDEFINED;
    }
    return ch_make_flonum(bankers_round(x));
}

static ChValue prim_rationalize(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    double x, y;
    if (!require_real(vm, args[0], "rationalize", &x) ||
        !require_real(vm, args[1], "rationalize", &y)) {
        return CH_UNDEFINED;
    }
    if (!isfinite(x)) {
        return args[0];
    }
    double lo = x - fabs(y);
    double hi = x + fabs(y);
    int64_t best_num = (int64_t)llround(x);
    int64_t best_den = 1;
    double bv = (double)best_num;
    if (!(bv >= lo && bv <= hi)) {
        int found = 0;
        for (int64_t den = 2; den <= 1000000; den++) {
            double fden = (double)den;
            int64_t lo_num = (int64_t)ceil(lo * fden);
            int64_t hi_num = (int64_t)floor(hi * fden);
            if (lo_num <= hi_num) {
                best_num = lo_num;
                best_den = den;
                found = 1;
                break;
            }
        }
        if (!found) {
            best_num = (int64_t)llround(x * 1000000.0);
            best_den = 1000000;
        }
    }
    {
        int64_t an = best_num < 0 ? -best_num : best_num;
        int64_t ad = best_den;
        while (ad != 0) {
            int64_t t = an % ad;
            an = ad;
            ad = t;
        }
        if (an > 1) {
            best_num /= an;
            best_den /= an;
        }
    }
    if (ch_is_exact(args[0])) {
        if (best_den == 1) {
            return ch_make_integer(&vm->gc, best_num);
        }
        return ch_make_rational(&vm->gc, ch_make_integer(&vm->gc, best_num),
                                ch_make_integer(&vm->gc, best_den));
    }
    return ch_make_flonum((double)best_num / (double)best_den);
}

/* ---- exactness ---- */

static ChValue prim_exact(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact(args[0])) {
        return args[0];
    }
    if (ch_is_flonum(args[0])) {
        ChValue r = ch_exact_from_flonum(&vm->gc, ch_to_flonum(args[0]));
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
    bool any_inexact = ch_is_flonum(args[0]);
    for (int i = 1; i < nargs; i++) {
        if (ch_is_flonum(args[i])) {
            any_inexact = true;
            break;
        }
    }
    if (any_inexact) {
        double acc;
        if (!require_real(vm, args[0], "lcm", &acc)) {
            return CH_UNDEFINED;
        }
        acc = fabs(acc);
        for (int i = 1; i < nargs; i++) {
            double b;
            if (!require_real(vm, args[i], "lcm", &b)) {
                return CH_UNDEFINED;
            }
            b = fabs(b);
            if (acc == 0.0 || b == 0.0) {
                acc = 0.0;
                continue;
            }
            double g = gcd_double(acc, b);
            acc = (acc / g) * b;
        }
        return ch_make_flonum(acc);
    }
    if (!ch_is_exact_integer(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "lcm: expected exact integers");
        return CH_UNDEFINED;
    }
    ChValue acc = ch_bignum_abs(&vm->gc, args[0]);
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

static uint32_t exact_int_bit_length(ChValue v) {
    if (ch_is_fixnum(v)) {
        uint64_t n = (uint64_t)ch_to_fixnum(v);
        if (n == 0) {
            return 0;
        }
        return 64 - (uint32_t)__builtin_clzll(n);
    }
    ChBignum *bn = ch_as_bignum(v);
    if (bn->len == 0) {
        return 0;
    }
    uint64_t top = bn->limbs[bn->len - 1];
    uint32_t top_bits = top == 0 ? 0 : 64 - (uint32_t)__builtin_clzll(top);
    return (uint32_t)((bn->len - 1) * 64 + top_bits);
}

typedef struct {
    ChValue root;
    ChValue rem;
} IsqrtResult;

/* Quotient via binary search using multiply/compare (for exact isqrt). */
static ChValue bignum_quotient_bs(ChGC *gc, ChValue a, ChValue b) {
    if (is_zero_int(b)) {
        return CH_UNDEFINED;
    }
    ChValue lo = ch_make_fixnum(0);
    ChValue hi = ch_make_fixnum(1);
    ch_gc_push(gc, &a);
    ch_gc_push(gc, &b);
    ch_gc_push(gc, &lo);
    ch_gc_push(gc, &hi);
    ChValue two = ch_make_fixnum(2);

    for (int guard = 0; guard < 256; guard++) {
        ChValue prod = ch_bignum_mul(gc, b, hi);
        ch_gc_push(gc, &prod);
        if (ch_bignum_compare(prod, a) > 0) {
            ch_gc_pop(gc);
            break;
        }
        ch_gc_pop(gc);
        lo = hi;
        hi = ch_bignum_mul(gc, hi, two);
    }

    while (ch_bignum_compare(lo, hi) < 0) {
        ChValue sum = ch_bignum_add(gc, lo, hi);
        ch_gc_push(gc, &sum);
        ChValue sum1 = ch_bignum_add(gc, sum, ch_make_fixnum(1));
        ch_gc_pop(gc);
        ch_gc_push(gc, &sum1);
        ChValue mid = ch_bignum_quotient(gc, sum1, two);
        ch_gc_pop(gc);
        ch_gc_push(gc, &mid);
        ChValue prod = ch_bignum_mul(gc, b, mid);
        ch_gc_push(gc, &prod);
        if (ch_bignum_compare(prod, a) <= 0) {
            lo = mid;
        } else {
            hi = ch_bignum_sub(gc, mid, ch_make_fixnum(1));
        }
        ch_gc_pop(gc);
        ch_gc_pop(gc);
    }

    ChValue out = lo;
    ch_gc_pop_n(gc, 4);
    return out;
}

static IsqrtResult isqrt_non_negative(ChVM *vm, ChValue n) {
    ChGC *gc = &vm->gc;
    if (ch_is_fixnum(n)) {
        int64_t ni = ch_to_fixnum(n);
        double f = (double)ni;
        int64_t s = (int64_t)sqrt(f);
        while (s * s > ni) {
            s--;
        }
        while ((s + 1) * (s + 1) <= ni) {
            s++;
        }
        return (IsqrtResult){ch_make_fixnum(s), ch_make_fixnum(ni - s * s)};
    }

    ch_gc_push(gc, &n);
    ChValue s;
    double f64_val = ch_bignum_to_f64(n);
    if (!isfinite(f64_val)) {
        uint32_t bit_len = exact_int_bit_length(n);
        uint32_t shift = ((bit_len - 52) + 1u) & ~1u;
        ChValue pow2 = ch_make_fixnum(1);
        ch_gc_push(gc, &pow2);
        for (uint32_t i = 0; i < shift; i++) {
            pow2 = ch_bignum_mul(gc, pow2, ch_make_fixnum(2));
        }
        ChValue shifted = ch_bignum_quotient(gc, n, pow2);
        ch_gc_pop(gc);
        ch_gc_push(gc, &shifted);
        double approx = sqrt(ch_bignum_to_f64(shifted));
        int64_t approx_i = approx < 1.0 ? 1 : (int64_t)approx;
        s = ch_make_integer(gc, approx_i);
        ch_gc_pop(gc);
        ch_gc_push(gc, &s);
        for (uint32_t j = 0; j < shift / 2; j++) {
            s = ch_bignum_mul(gc, s, ch_make_fixnum(2));
        }
    } else {
        double approx = sqrt(f64_val);
        int64_t approx_i;
        if (approx < 1.0) {
            approx_i = 1;
        } else if (approx >= (double)INT64_MAX) {
            approx_i = INT64_MAX;
        } else {
            approx_i = (int64_t)approx;
        }
        s = ch_make_integer(gc, approx_i);
        ch_gc_push(gc, &s);
    }

    ChValue two = ch_make_fixnum(2);
    for (int iters = 0; iters < 500; iters++) {
        if (is_zero_int(s)) {
            break;
        }
        ChValue q = bignum_quotient_bs(gc, n, s);
        ch_gc_push(gc, &q);
        ChValue sum = ch_bignum_add(gc, s, q);
        ch_gc_pop(gc);
        ChValue next = ch_bignum_quotient(gc, sum, two);
        if (ch_bignum_compare(next, s) == 0) {
            s = next;
            break;
        }
        s = next;
    }

    ChValue s2 = ch_bignum_mul(gc, s, s);
    ch_gc_push(gc, &s2);
    while (ch_bignum_compare(s2, n) > 0) {
        s = ch_bignum_sub(gc, s, ch_make_fixnum(1));
        s2 = ch_bignum_mul(gc, s, s);
    }

    ChValue one = ch_make_fixnum(1);
    ChValue s1 = ch_bignum_add(gc, s, one);
    ch_gc_push(gc, &s1);
    ChValue s1_sq = ch_bignum_mul(gc, s1, s1);
    ch_gc_push(gc, &s1_sq);
    while (ch_bignum_compare(s1_sq, n) <= 0) {
        s = s1;
        s2 = s1_sq;
        s1 = ch_bignum_add(gc, s, one);
        s1_sq = ch_bignum_mul(gc, s1, s1);
    }
    ch_gc_pop(gc);
    ch_gc_pop(gc);
    ch_gc_pop(gc);

    ChValue rem = ch_bignum_sub(gc, n, s2);
    IsqrtResult out = {ch_bignum_normalize(gc, s), ch_bignum_normalize(gc, rem)};
    ch_gc_pop_n(gc, 2);
    return out;
}

static bool isqrt_perfect(const IsqrtResult *r) {
    return ch_bignum_compare(r->rem, ch_make_fixnum(0)) == 0;
}

static double rational_to_f64(ChValue num, ChValue den) {
    return ch_bignum_to_f64(num) / ch_bignum_to_f64(den);
}

static ChValue prim_sqrt(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue arg = args[0];
    if (ch_is_complex_obj(arg)) {
        double re, im;
        ch_complex_parts(arg, &re, &im);
        /* Negative real with ±0 imag: principal sqrt is +i * sqrt(|re|). */
        if (im == 0.0 && re < 0.0) {
            return ch_make_complex(&vm->gc, 0.0, sqrt(-re));
        }
        double r = hypot(re, im);
        double theta = atan2(im, re) / 2.0;
        double sr = sqrt(r);
        return ch_make_complex(&vm->gc, sr * cos(theta), sr * sin(theta));
    }
    if (ch_is_flonum(arg)) {
        double x = ch_to_flonum(arg);
        if (x < 0) {
            return ch_make_complex(&vm->gc, 0.0, sqrt(-x));
        }
        return ch_make_flonum(sqrt(x));
    }
    if (ch_is_exact_integer(arg)) {
        if (ch_bignum_compare(arg, ch_make_fixnum(0)) < 0) {
            double x = ch_bignum_to_f64(arg);
            return ch_make_complex(&vm->gc, 0.0, sqrt(-x));
        }
        IsqrtResult r = isqrt_non_negative(vm, arg);
        if (isqrt_perfect(&r)) {
            return r.root;
        }
        return ch_make_flonum(sqrt(ch_bignum_to_f64(arg)));
    }
    if (ch_is_rational_obj(arg)) {
        ChRational *rat = ch_as_rational(arg);
        double mag = rational_to_f64(rat->numerator, rat->denominator);
        if (mag < 0) {
            return ch_make_complex(&vm->gc, 0.0, sqrt(-mag));
        }
        IsqrtResult num_r = isqrt_non_negative(vm, rat->numerator);
        IsqrtResult den_r = isqrt_non_negative(vm, rat->denominator);
        if (isqrt_perfect(&num_r) && isqrt_perfect(&den_r)) {
            return ch_make_rational(&vm->gc, num_r.root, den_r.root);
        }
        double x = rational_to_f64(rat->numerator, rat->denominator);
        return ch_make_flonum(sqrt(x));
    }
    double x;
    if (!require_real(vm, arg, "sqrt", &x)) {
        return CH_UNDEFINED;
    }
    if (x < 0) {
        return ch_make_complex(&vm->gc, 0.0, sqrt(-x));
    }
    return ch_make_flonum(sqrt(x));
}

static ChValue prim_exact_integer_sqrt(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_exact_integer(args[0]) || ch_bignum_compare(args[0], ch_make_fixnum(0)) < 0) {
        ChValue msg =
            ch_gc_make_string_cstr(&vm->gc, "exact-integer-sqrt: expected non-negative exact integer");
        ChValue err = ch_gc_make_error_object(&vm->gc, msg, CH_NIL, 0);
        return ch_vm_raise(vm, err, 0);
    }
    if (is_zero_int(args[0]) || ch_bignum_compare(args[0], ch_make_fixnum(1)) == 0) {
        ChValue vals[2] = {args[0], ch_make_fixnum(0)};
        return ch_gc_make_values(&vm->gc, vals, 2);
    }
    IsqrtResult r = isqrt_non_negative(vm, args[0]);
    ChValue vals[2] = {r.root, r.rem};
    ch_gc_push(&vm->gc, &vals[0]);
    ch_gc_push(&vm->gc, &vals[1]);
    ChValue out = ch_gc_make_values(&vm->gc, vals, 2);
    ch_gc_pop_n(&vm->gc, 2);
    return out;
}

/* ---- floor/ and truncate/ ---- */

static ChValue exact_div_pair(ChVM *vm, ChValue a, ChValue b, const char *who, int use_floor) {
    if (ch_is_flonum(a) || ch_is_flonum(b)) {
        double da, db;
        if (!require_real(vm, a, who, &da) || !require_real(vm, b, who, &db)) {
            return CH_UNDEFINED;
        }
        if (db == 0.0) {
            snprintf(vm->error, sizeof(vm->error), "%s: division by zero", who);
            return CH_UNDEFINED;
        }
        double q = use_floor ? floor(da / db) : trunc(da / db);
        ChValue vals[2] = {ch_make_flonum(q), ch_make_flonum(da - q * db)};
        return ch_gc_make_values(&vm->gc, vals, 2);
    }
    if (!ch_is_exact_integer(a) || !ch_is_exact_integer(b)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected exact integers", who);
        return CH_UNDEFINED;
    }
    if (is_zero_int(b)) {
        snprintf(vm->error, sizeof(vm->error), "%s: division by zero", who);
        return CH_UNDEFINED;
    }
    ChGC *gc = &vm->gc;
    ch_gc_push(gc, &a);
    ch_gc_push(gc, &b);
    ChValue q = ch_bignum_quotient(gc, a, b);
    ChValue r = ch_bignum_remainder(gc, a, b);
    if (use_floor && !is_zero_int(r) &&
        ((ch_bignum_compare(a, ch_make_fixnum(0)) < 0) != (ch_bignum_compare(b, ch_make_fixnum(0)) < 0))) {
        q = ch_bignum_sub(gc, q, ch_make_fixnum(1));
        r = ch_bignum_add(gc, r, b);
    }
    ch_gc_pop_n(gc, 2);
    ChValue vals[2] = {q, r};
    return ch_gc_make_values(gc, vals, 2);
}

static ChValue prim_floor_div(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return exact_div_pair(vm, args[0], args[1], "floor/", 1);
}

static ChValue prim_truncate_div(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return exact_div_pair(vm, args[0], args[1], "truncate/", 0);
}

static ChValue exact_div_quotient(ChVM *vm, ChValue a, ChValue b, const char *who, int use_floor) {
    if (ch_is_flonum(a) || ch_is_flonum(b)) {
        double da, db;
        if (!require_real(vm, a, who, &da) || !require_real(vm, b, who, &db)) {
            return CH_UNDEFINED;
        }
        if (db == 0.0) {
            snprintf(vm->error, sizeof(vm->error), "%s: division by zero", who);
            return CH_UNDEFINED;
        }
        return ch_make_flonum(use_floor ? floor(da / db) : trunc(da / db));
    }
    if (!ch_is_exact_integer(a) || !ch_is_exact_integer(b)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected exact integers", who);
        return CH_UNDEFINED;
    }
    if (is_zero_int(b)) {
        snprintf(vm->error, sizeof(vm->error), "%s: division by zero", who);
        return CH_UNDEFINED;
    }
    ChGC *gc = &vm->gc;
    ch_gc_push(gc, &a);
    ch_gc_push(gc, &b);
    ChValue q = ch_bignum_quotient(gc, a, b);
    ChValue r = ch_bignum_remainder(gc, a, b);
    if (use_floor && !is_zero_int(r) &&
        ((ch_bignum_compare(a, ch_make_fixnum(0)) < 0) != (ch_bignum_compare(b, ch_make_fixnum(0)) < 0))) {
        q = ch_bignum_sub(gc, q, ch_make_fixnum(1));
    }
    ch_gc_pop_n(gc, 2);
    return q;
}

static ChValue exact_div_remainder(ChVM *vm, ChValue a, ChValue b, const char *who, int use_floor) {
    if (ch_is_flonum(a) || ch_is_flonum(b)) {
        double da, db;
        if (!require_real(vm, a, who, &da) || !require_real(vm, b, who, &db)) {
            return CH_UNDEFINED;
        }
        if (db == 0.0) {
            snprintf(vm->error, sizeof(vm->error), "%s: division by zero", who);
            return CH_UNDEFINED;
        }
        double q = use_floor ? floor(da / db) : trunc(da / db);
        return ch_make_flonum(da - q * db);
    }
    if (!ch_is_exact_integer(a) || !ch_is_exact_integer(b)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected exact integers", who);
        return CH_UNDEFINED;
    }
    if (is_zero_int(b)) {
        snprintf(vm->error, sizeof(vm->error), "%s: division by zero", who);
        return CH_UNDEFINED;
    }
    ChGC *gc = &vm->gc;
    ch_gc_push(gc, &a);
    ch_gc_push(gc, &b);
    ChValue r = ch_bignum_remainder(gc, a, b);
    if (use_floor && !is_zero_int(r) &&
        ((ch_bignum_compare(a, ch_make_fixnum(0)) < 0) != (ch_bignum_compare(b, ch_make_fixnum(0)) < 0))) {
        r = ch_bignum_add(gc, r, b);
    }
    ch_gc_pop_n(gc, 2);
    return r;
}

static ChValue prim_floor_quotient(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return exact_div_quotient(vm, args[0], args[1], "floor-quotient", 1);
}

static ChValue prim_floor_remainder(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return exact_div_remainder(vm, args[0], args[1], "floor-remainder", 1);
}

static ChValue prim_truncate_quotient(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return exact_div_quotient(vm, args[0], args[1], "truncate-quotient", 0);
}

static ChValue prim_truncate_remainder(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return exact_div_remainder(vm, args[0], args[1], "truncate-remainder", 0);
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
    define_prim(vm, "rationalize", prim_rationalize, 2, 2);
    define_prim(vm, "exact", prim_exact, 1, 1);
    define_prim(vm, "inexact", prim_inexact, 1, 1);
    define_prim(vm, "exact->inexact", prim_inexact, 1, 1);
    define_prim(vm, "inexact->exact", prim_exact, 1, 1);
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
    define_prim(vm, "floor/", prim_floor_div, 2, 2);
    define_prim(vm, "truncate/", prim_truncate_div, 2, 2);
    define_prim(vm, "floor-quotient", prim_floor_quotient, 2, 2);
    define_prim(vm, "floor-remainder", prim_floor_remainder, 2, 2);
    define_prim(vm, "truncate-quotient", prim_truncate_quotient, 2, 2);
    define_prim(vm, "truncate-remainder", prim_truncate_remainder, 2, 2);
    define_prim(vm, "number->string", prim_number_to_string, -1, 1);
    define_prim(vm, "string->number", prim_string_to_number, -1, 1);
}
