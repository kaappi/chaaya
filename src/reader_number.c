#include "chaaya/reader.h"

#include "reader_internal.h"

#include "chaaya/bignum.h"
#include "chaaya/complex.h"
#include "chaaya/rational.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool try_parse_real_f64(ChGC *gc, const char *text, size_t len, double *out) {
    if (len == 0) {
        return false;
    }
    ChValue v;
    if (!try_parse_number(gc, text, len, &v) || ch_is_complex_obj(v)) {
        return false;
    }
    double re, im;
    if (!ch_complex_parts(v, &re, &im)) {
        return false;
    }
    *out = re;
    return true;
}

static bool imag_literal_is_inexact(const char *text, size_t split, size_t body) {
    size_t ilen = body - split;
    if (ilen == 0) {
        return false;
    }
    if (ilen == 1) {
        return false;
    }
    for (size_t j = split; j < body; j++) {
        if (text[j] == '.' || text[j] == 'e' || text[j] == 'E') {
            return true;
        }
    }
    return false;
}

static bool part_text_is_inexact(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '.' || c == 'e' || c == 'E' || c == 's' || c == 'S' || c == 'f' || c == 'F' ||
            c == 'd' || c == 'D' || c == 'l' || c == 'L') {
            return true;
        }
    }
    return false;
}

static bool try_parse_complex(ChGC *gc, const char *text, size_t len, ChValue *out) {
    if (len < 2) {
        return false;
    }
    char last = text[len - 1];
    if (last != 'i' && last != 'I') {
        return false;
    }
    /* +i / -i */
    if (len == 2 && (text[0] == '+' || text[0] == '-')) {
        *out = ch_make_complex_ex(gc, 0.0, text[0] == '+' ? 1.0 : -1.0, true, true);
        return true;
    }
    size_t body = len - 1; /* strip trailing i */
    /* Last +/− after index 0 separates real and imag parts. */
    size_t split = 0;
    for (size_t j = 1; j < body; j++) {
        if (text[j] == '+' || text[j] == '-') {
            split = j;
        }
    }
    double real = 0.0;
    double imag = 0.0;
    bool exact_re = true;
    bool exact_im = true;
    if (split == 0) {
        /* Pure imaginary: Ni, +Ni, -Ni */
        if (!try_parse_real_f64(gc, text, body, &imag)) {
            return false;
        }
        exact_im = !part_text_is_inexact(text, body);
        exact_re = true;
    } else {
        size_t ilen = body - split;
        if (!try_parse_real_f64(gc, text, split, &real)) {
            return false;
        }
        exact_re = !part_text_is_inexact(text, split);
        if (ilen == 1) {
            imag = text[split] == '+' ? 1.0 : -1.0;
            exact_im = true;
        } else if (!try_parse_real_f64(gc, text + split, ilen, &imag)) {
            return false;
        } else {
            exact_im = !part_text_is_inexact(text + split, ilen);
        }
    }
    if (imag == 0.0 && imag_literal_is_inexact(text, split, body)) {
        *out = ch_make_complex_raw(gc, real, imag);
    } else {
        *out = ch_make_complex_ex(gc, real, imag, exact_re, exact_im);
    }
    return true;
}

static bool try_parse_special_flonum(const char *text, size_t len, ChValue *out) {
    if (len == 6 && text[0] == '+' && eq_ci_n(text + 1, "inf.0", 5)) {
        *out = ch_make_flonum(INFINITY);
        return true;
    }
    if (len == 6 && text[0] == '-' && eq_ci_n(text + 1, "inf.0", 5)) {
        *out = ch_make_flonum(-INFINITY);
        return true;
    }
    if (len == 6 && text[0] == '+' && eq_ci_n(text + 1, "nan.0", 5)) {
        *out = ch_make_flonum(NAN);
        return true;
    }
    if (len == 6 && text[0] == '-' && eq_ci_n(text + 1, "nan.0", 5)) {
        *out = ch_make_flonum(NAN);
        return true;
    }
    return false;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int digit_in_base(int c, int base) {
    int d = hex_digit(c);
    if (d < 0 || d >= base) {
        return -1;
    }
    return d;
}

/* SRFI 169: strip digit-separator underscores; null if misplaced. */
static const char *strip_underscores(const char *text, size_t len, int base, char *buf, size_t buf_cap,
                                     size_t *out_len) {
    if (memchr(text, '_', len) == NULL) {
        *out_len = len;
        return text;
    }
    if (buf_cap < len) {
        return NULL;
    }
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] != '_') {
            buf[n++] = text[i];
            continue;
        }
        int prev = (i > 0) ? digit_in_base((unsigned char)text[i - 1], base) : -1;
        int next = (i + 1 < len) ? digit_in_base((unsigned char)text[i + 1], base) : -1;
        if (prev < 0 || next < 0) {
            return NULL;
        }
    }
    *out_len = n;
    return buf;
}

/* SRFI 270: [sign] hex[.hex] [p[sign]decimal-digits] */
static bool try_parse_hex_float(const char *text, size_t len, ChValue *out) {
    char strip_buf[512];
    size_t slen = 0;
    const char *s = strip_underscores(text, len, 16, strip_buf, sizeof(strip_buf), &slen);
    if (!s) {
        return false;
    }
    size_t i = 0;
    int neg = 0;
    if (i < slen && (s[i] == '+' || s[i] == '-')) {
        neg = (s[i] == '-');
        i++;
    }
    double mantissa = 0.0;
    int any_digit = 0;
    while (i < slen && hex_digit((unsigned char)s[i]) >= 0) {
        mantissa = mantissa * 16.0 + (double)hex_digit((unsigned char)s[i]);
        any_digit = 1;
        i++;
    }
    if (i < slen && s[i] == '.') {
        i++;
        double frac_scale = 1.0 / 16.0;
        while (i < slen && hex_digit((unsigned char)s[i]) >= 0) {
            mantissa += (double)hex_digit((unsigned char)s[i]) * frac_scale;
            frac_scale /= 16.0;
            any_digit = 1;
            i++;
        }
    }
    if (!any_digit) {
        return false;
    }
    int32_t exp = 0;
    if (i < slen && (s[i] == 'p' || s[i] == 'P')) {
        i++;
        int exp_neg = 0;
        if (i < slen && (s[i] == '+' || s[i] == '-')) {
            exp_neg = (s[i] == '-');
            i++;
        }
        int32_t exp_val = 0;
        int exp_any = 0;
        while (i < slen && isdigit((unsigned char)s[i])) {
            exp_any = 1;
            if (exp_val < 1000000) {
                exp_val = exp_val * 10 + (s[i] - '0');
            }
            i++;
        }
        if (!exp_any) {
            return false;
        }
        exp = exp_neg ? -exp_val : exp_val;
    }
    if (i != slen) {
        return false;
    }
    double result = mantissa * pow(2.0, (double)exp);
    if (neg) {
        result = -result;
    }
    *out = ch_make_flonum(result);
    return true;
}

static bool looks_like_hex_float(const char *text, size_t len) {
    size_t i = 0;
    if (i < len && (text[i] == '+' || text[i] == '-')) {
        i++;
    }
    while (i < len && (hex_digit((unsigned char)text[i]) >= 0 || text[i] == '_')) {
        i++;
    }
    if (i < len && text[i] == '.') {
        return true;
    }
    if (i < len && (text[i] == 'p' || text[i] == 'P')) {
        return true;
    }
    return false;
}
/* Parse unsigned integer digits in base into a ChValue integer. */
static bool parse_uint_base(ChGC *gc, const char *text, size_t len, int base, ChValue *out) {
    char strip_buf[512];
    size_t slen = 0;
    const char *digits = strip_underscores(text, len, base, strip_buf, sizeof(strip_buf), &slen);
    if (!digits) {
        return false;
    }
    if (slen == 0) {
        return false;
    }
    for (size_t i = 0; i < slen; i++) {
        if (digit_in_base((unsigned char)digits[i], base) < 0) {
            return false;
        }
    }
    if (base == 10) {
        ChValue v = ch_bignum_parse_decimal(gc, digits, slen);
        if (v == CH_UNDEFINED) {
            return false;
        }
        *out = v;
        return true;
    }
    /* Accumulate via repeated *base + digit (ok for suite-sized literals). */
    ChValue acc = ch_make_fixnum(0);
    ChValue b = ch_make_fixnum(base);
    ch_gc_push(gc, &acc);
    for (size_t i = 0; i < slen; i++) {
        int d = digit_in_base((unsigned char)digits[i], base);
        acc = ch_bignum_mul(gc, acc, b);
        acc = ch_bignum_add(gc, acc, ch_make_fixnum(d));
    }
    ch_gc_pop(gc);
    *out = acc;
    return true;
}

static bool parse_signed_int_base(ChGC *gc, const char *text, size_t len, int base, ChValue *out) {
    if (len == 0) {
        return false;
    }
    int neg = 0;
    size_t i = 0;
    if (text[0] == '+' || text[0] == '-') {
        neg = (text[0] == '-');
        i = 1;
    }
    if (i >= len) {
        return false;
    }
    ChValue mag;
    if (!parse_uint_base(gc, text + i, len - i, base, &mag)) {
        return false;
    }
    if (neg) {
        mag = ch_bignum_negate(gc, mag);
    }
    *out = mag;
    return true;
}

/* Decimal real with optional exponent markers e/s/f/d/l (any case). */
static bool parse_decimal_real(ChGC *gc, const char *text, size_t len, ChValue *out) {
    (void)gc;
    if (len == 0) {
        return false;
    }
    if (try_parse_special_flonum(text, len, out)) {
        return true;
    }
    char strip_buf[512];
    size_t slen = 0;
    const char *body = strip_underscores(text, len, 10, strip_buf, sizeof(strip_buf), &slen);
    if (!body) {
        return false;
    }
    char *tmp = (char *)malloc(slen + 1);
    if (!tmp) {
        return false;
    }
    memcpy(tmp, body, slen);
    tmp[slen] = '\0';
    len = slen;
    /* Normalize Scheme exponent markers to 'e' for strtod. */
    for (size_t i = 0; i < len; i++) {
        char c = tmp[i];
        if (c == 's' || c == 'S' || c == 'f' || c == 'F' || c == 'd' || c == 'D' || c == 'l' ||
            c == 'L' || c == 'e' || c == 'E') {
            /* Only treat as exponent if followed by optional sign + digit. */
            size_t j = i + 1;
            if (j < len && (tmp[j] == '+' || tmp[j] == '-')) {
                j++;
            }
            if (j < len && isdigit((unsigned char)tmp[j])) {
                tmp[i] = 'e';
            }
        }
    }
    char *end = NULL;
    double dv = strtod(tmp, &end);
    int ok = (end && *end == '\0');
    free(tmp);
    if (!ok) {
        return false;
    }
    *out = ch_make_flonum(dv);
    return true;
}

static bool try_parse_number_in_base(ChGC *gc, const char *text, size_t len, int base, ChValue *out);

/* Parse decimal/text scientific notation as an exact rational (for #e). */
static bool try_parse_exact_decimal(ChGC *gc, const char *text, size_t len, ChValue *out) {
    char strip_buf[512];
    size_t slen = 0;
    const char *body = strip_underscores(text, len, 10, strip_buf, sizeof(strip_buf), &slen);
    if (!body) {
        return false;
    }
    text = body;
    len = slen;
    if (len == 0) {
        return false;
    }
    size_t i = 0;
    int neg = 0;
    if (text[0] == '+' || text[0] == '-') {
        neg = (text[0] == '-');
        i = 1;
    }
    if (i >= len) {
        return false;
    }
    size_t exp_pos = len;
    for (size_t j = i; j < len; j++) {
        if (text[j] == 'e' || text[j] == 'E') {
            exp_pos = j;
            break;
        }
    }
    int64_t exp = 0;
    if (exp_pos < len) {
        size_t k = exp_pos + 1;
        int eneg = 0;
        if (k < len && (text[k] == '+' || text[k] == '-')) {
            eneg = (text[k] == '-');
            k++;
        }
        if (k >= len) {
            return false;
        }
        while (k < len) {
            if (!isdigit((unsigned char)text[k])) {
                return false;
            }
            exp = exp * 10 + (text[k] - '0');
            if (exp > 1000) {
                return false;
            }
            k++;
        }
        if (eneg) {
            exp = -exp;
        }
    }
    size_t mant_end = exp_pos;
    size_t dot = mant_end;
    for (size_t j = i; j < mant_end; j++) {
        if (text[j] == '.') {
            if (dot != mant_end) {
                return false;
            }
            dot = j;
        } else if (!isdigit((unsigned char)text[j])) {
            return false;
        }
    }
    if (dot == i && (dot + 1 >= mant_end || !isdigit((unsigned char)text[dot + 1]))) {
        return false;
    }
    ChValue num = ch_make_fixnum(0);
    ChValue den = ch_make_fixnum(1);
    ch_gc_push(gc, &num);
    ch_gc_push(gc, &den);
    size_t frac_digits = 0;
    for (size_t j = i; j < mant_end; j++) {
        if (text[j] == '.') {
            continue;
        }
        num = ch_bignum_mul(gc, num, ch_make_fixnum(10));
        num = ch_bignum_add(gc, num, ch_make_fixnum(text[j] - '0'));
        if (j > dot) {
            frac_digits++;
        }
    }
    int64_t scale = exp - (int64_t)frac_digits;
    if (scale > 0) {
        ChValue mul = ch_make_fixnum(1);
        ch_gc_push(gc, &mul);
        for (int64_t e = 0; e < scale; e++) {
            mul = ch_bignum_mul(gc, mul, ch_make_fixnum(10));
        }
        num = ch_bignum_mul(gc, num, mul);
        ch_gc_pop(gc);
    } else if (scale < 0) {
        ChValue mul = ch_make_fixnum(1);
        ch_gc_push(gc, &mul);
        for (int64_t e = 0; e > scale; e--) {
            mul = ch_bignum_mul(gc, mul, ch_make_fixnum(10));
        }
        den = mul;
        ch_gc_pop(gc);
    }
    if (neg) {
        num = ch_bignum_negate(gc, num);
    }
    ChValue rat = ch_make_rational(gc, num, den);
    ch_gc_pop_n(gc, 2);
    if (rat == CH_UNDEFINED) {
        return false;
    }
    *out = rat;
    return true;
}

bool try_parse_number(ChGC *gc, const char *text, size_t len, ChValue *out) {
    if (len == 0) {
        return false;
    }
    /* Strip stacked R7RS numeric prefixes. */
    int force_exact = 0;
    int force_inexact = 0;
    int base = 10;
    size_t pos = 0;
    while (pos + 2 <= len && text[pos] == '#') {
        char p = text[pos + 1];
        if (p == 'e' || p == 'E') {
            force_exact = 1;
            force_inexact = 0;
            pos += 2;
        } else if (p == 'i' || p == 'I') {
            force_inexact = 1;
            force_exact = 0;
            pos += 2;
        } else if (p == 'b' || p == 'B') {
            base = 2;
            pos += 2;
        } else if (p == 'o' || p == 'O') {
            base = 8;
            pos += 2;
        } else if (p == 'd' || p == 'D') {
            base = 10;
            pos += 2;
        } else if (p == 'x' || p == 'X') {
            base = 16;
            pos += 2;
        } else {
            break;
        }
    }
    if (pos >= len) {
        return false;
    }
    const char *body = text + pos;
    size_t blen = len - pos;
    if (force_exact && base == 10 && try_parse_exact_decimal(gc, body, blen, out)) {
        return true;
    }
    if (!try_parse_number_in_base(gc, body, blen, base, out)) {
        return false;
    }
    if (force_inexact && ch_is_exact(*out)) {
        *out = ch_make_flonum(ch_exact_to_f64(*out));
    } else if (force_exact && ch_is_flonum(*out)) {
        double dv = ch_to_flonum(*out);
        if (!isfinite(dv)) {
            return true; /* leave nan/inf as-is under #e (implementation-defined) */
        }
        ChValue ex = ch_exact_from_flonum(gc, dv);
        if (ex != CH_UNDEFINED) {
            *out = ex;
        }
    }
    return true;
}

static bool try_parse_number_in_base(ChGC *gc, const char *text, size_t len, int base, ChValue *out) {
    if (len == 0) {
        return false;
    }
    if (try_parse_special_flonum(text, len, out)) {
        return true;
    }
    /* special: + - alone are symbols */
    if ((len == 1 && (text[0] == '+' || text[0] == '-')) || (len == 1 && text[0] == '.')) {
        return false;
    }
    /* Complex before rational so 2/4i is pure-imaginary, not a fraction. */
    if (base == 10 && try_parse_complex(gc, text, len, out)) {
        return true;
    }
    if (base == 16 && looks_like_hex_float(text, len)) {
        return try_parse_hex_float(text, len, out);
    }
    /* Rational N/D */
    const char *slash = NULL;
    for (size_t j = 0; j < len; j++) {
        if (text[j] == '/') {
            slash = text + j;
            break;
        }
    }
    if (slash) {
        size_t nlen = (size_t)(slash - text);
        size_t dlen = len - nlen - 1;
        if (nlen == 0 || dlen == 0) {
            return false;
        }
        ChValue num;
        ChValue den;
        if (!parse_signed_int_base(gc, text, nlen, base, &num)) {
            return false;
        }
        ch_gc_push(gc, &num);
        if (!parse_signed_int_base(gc, slash + 1, dlen, base, &den)) {
            ch_gc_pop(gc);
            return false;
        }
        ch_gc_push(gc, &den);
        ChValue rat = ch_make_rational(gc, num, den);
        ch_gc_pop_n(gc, 2);
        if (rat == CH_UNDEFINED) {
            return false;
        }
        *out = rat;
        return true;
    }
    if (base == 10) {
        /* Exact integer: optional sign + digits only */
        char strip_buf[512];
        size_t slen = 0;
        const char *body = strip_underscores(text, len, 10, strip_buf, sizeof(strip_buf), &slen);
        if (!body) {
            return false;
        }
        size_t bi = 0;
        if (body[0] == '+' || body[0] == '-') {
            bi = 1;
        }
        int all_digits = (bi < slen);
        for (size_t j = bi; j < slen; j++) {
            if (body[j] < '0' || body[j] > '9') {
                all_digits = 0;
                break;
            }
        }
        if (all_digits) {
            ChValue v = ch_bignum_parse_decimal(gc, body, slen);
            if (v != CH_UNDEFINED) {
                *out = v;
                return true;
            }
            return false;
        }
        return parse_decimal_real(gc, text, len, out);
    }
    /* Non-decimal integer (optional sign). */
    return parse_signed_int_base(gc, text, len, base, out);
}

bool ch_parse_number(ChGC *gc, const char *text, size_t len, ChValue *out) {
    return try_parse_number(gc, text, len, out);
}
