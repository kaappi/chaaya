#include "chaaya/bignum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned __int128 u128;

static void normalize_limbs(uint64_t *limbs, size_t *len) {
    while (*len > 0 && limbs[*len - 1] == 0) {
        (*len)--;
    }
}

static int fits_fixnum_mag(const uint64_t *limbs, size_t len, int positive) {
    if (len == 0) {
        return 1;
    }
    if (len > 1) {
        return 0;
    }
    uint64_t m = limbs[0];
    if (positive) {
        return m <= (uint64_t)CH_FIXNUM_MAX;
    }
    /* magnitude of CH_FIXNUM_MIN is 2^47 */
    return m <= ((uint64_t)1 << 47);
}

static int64_t limbs_to_i64(const uint64_t *limbs, size_t len, int positive) {
    if (len == 0) {
        return 0;
    }
    int64_t m = (int64_t)limbs[0];
    return positive ? m : -m;
}

ChValue ch_gc_make_bignum_from_limbs(ChGC *gc, const uint64_t *limbs, size_t len, int positive) {
    uint64_t *copy = NULL;
    size_t nlen = len;
    if (len > 0) {
        copy = (uint64_t *)malloc(sizeof(uint64_t) * len);
        if (!copy) {
            abort();
        }
        memcpy(copy, limbs, sizeof(uint64_t) * len);
        normalize_limbs(copy, &nlen);
    }
    if (nlen == 0) {
        free(copy);
        positive = 1;
    }
    ChBignum *bn = (ChBignum *)ch_gc_alloc(gc, sizeof(ChBignum), CH_TAG_BIGNUM);
    bn->limbs = copy;
    bn->len = nlen;
    bn->positive = positive ? 1 : 0;
    return ch_make_pointer(&bn->header);
}

ChValue ch_gc_make_bignum_from_i64(ChGC *gc, int64_t n) {
    if (n == 0) {
        return ch_gc_make_bignum_from_limbs(gc, NULL, 0, 1);
    }
    int positive = n > 0;
    uint64_t mag = positive ? (uint64_t)n : (uint64_t)(-(u128)n);
    return ch_gc_make_bignum_from_limbs(gc, &mag, 1, positive);
}

ChValue ch_make_integer(ChGC *gc, int64_t n) {
    if (n >= CH_FIXNUM_MIN && n <= CH_FIXNUM_MAX) {
        return ch_make_fixnum(n);
    }
    return ch_gc_make_bignum_from_i64(gc, n);
}

ChValue ch_bignum_normalize(ChGC *gc, ChValue v) {
    if (ch_is_fixnum(v)) {
        return v;
    }
    if (!ch_is_bignum(v)) {
        return v;
    }
    ChBignum *bn = ch_as_bignum(v);
    normalize_limbs(bn->limbs, &bn->len);
    if (bn->len == 0) {
        bn->positive = 1;
        return ch_make_fixnum(0);
    }
    if (fits_fixnum_mag(bn->limbs, bn->len, bn->positive)) {
        return ch_make_fixnum(limbs_to_i64(bn->limbs, bn->len, bn->positive));
    }
    (void)gc;
    return v;
}

typedef struct {
    const uint64_t *limbs;
    size_t len;
    int positive;
    uint64_t scratch;
} MagView;

static MagView view_of(ChValue v) {
    MagView m = {0};
    if (ch_is_fixnum(v)) {
        int64_t n = ch_to_fixnum(v);
        if (n == 0) {
            m.positive = 1;
            return m;
        }
        m.positive = n > 0;
        m.scratch = m.positive ? (uint64_t)n : (uint64_t)(-n);
        m.limbs = &m.scratch;
        m.len = 1;
        return m;
    }
    ChBignum *bn = ch_as_bignum(v);
    m.limbs = bn->limbs;
    m.len = bn->len;
    m.positive = bn->positive;
    return m;
}

static int cmp_mag(const uint64_t *a, size_t al, const uint64_t *b, size_t bl) {
    if (al != bl) {
        return al > bl ? 1 : -1;
    }
    for (size_t i = al; i > 0; i--) {
        if (a[i - 1] != b[i - 1]) {
            return a[i - 1] > b[i - 1] ? 1 : -1;
        }
    }
    return 0;
}

int ch_bignum_compare(ChValue a, ChValue b) {
    MagView va = view_of(a);
    MagView vb = view_of(b);
    if (va.len == 0 && vb.len == 0) {
        return 0;
    }
    if (va.positive != vb.positive) {
        return va.positive ? 1 : -1;
    }
    int c = cmp_mag(va.limbs, va.len, vb.limbs, vb.len);
    return va.positive ? c : -c;
}

static ChValue make_from_mag(ChGC *gc, uint64_t *limbs, size_t len, int positive) {
    normalize_limbs(limbs, &len);
    if (len == 0) {
        free(limbs);
        return ch_make_fixnum(0);
    }
    if (fits_fixnum_mag(limbs, len, positive)) {
        int64_t n = limbs_to_i64(limbs, len, positive);
        free(limbs);
        return ch_make_fixnum(n);
    }
    ChBignum *bn = (ChBignum *)ch_gc_alloc(gc, sizeof(ChBignum), CH_TAG_BIGNUM);
    bn->limbs = limbs;
    bn->len = len;
    bn->positive = positive ? 1 : 0;
    return ch_make_pointer(&bn->header);
}

static uint64_t *add_mag(const uint64_t *a, size_t al, const uint64_t *b, size_t bl, size_t *out_len) {
    size_t max = al > bl ? al : bl;
    uint64_t *r = (uint64_t *)calloc(max + 1, sizeof(uint64_t));
    if (!r) {
        abort();
    }
    u128 carry = 0;
    for (size_t i = 0; i < max; i++) {
        u128 sum = carry;
        if (i < al) {
            sum += a[i];
        }
        if (i < bl) {
            sum += b[i];
        }
        r[i] = (uint64_t)sum;
        carry = sum >> 64;
    }
    size_t len = max;
    if (carry) {
        r[max] = (uint64_t)carry;
        len = max + 1;
    }
    *out_len = len;
    return r;
}

static uint64_t *sub_mag(const uint64_t *a, size_t al, const uint64_t *b, size_t bl, size_t *out_len) {
    /* assume |a| >= |b| */
    uint64_t *r = (uint64_t *)calloc(al, sizeof(uint64_t));
    if (!r) {
        abort();
    }
    u128 borrow = 0;
    for (size_t i = 0; i < al; i++) {
        u128 av = a[i];
        u128 bv = (i < bl ? b[i] : 0) + borrow;
        if (av >= bv) {
            r[i] = (uint64_t)(av - bv);
            borrow = 0;
        } else {
            r[i] = (uint64_t)(((u128)1 << 64) + av - bv);
            borrow = 1;
        }
    }
    *out_len = al;
    normalize_limbs(r, out_len);
    return r;
}

static uint64_t *mul_mag(const uint64_t *a, size_t al, const uint64_t *b, size_t bl, size_t *out_len) {
    if (al == 0 || bl == 0) {
        *out_len = 0;
        return NULL;
    }
    size_t max = al + bl;
    uint64_t *r = (uint64_t *)calloc(max, sizeof(uint64_t));
    if (!r) {
        abort();
    }
    for (size_t i = 0; i < al; i++) {
        u128 carry = 0;
        for (size_t j = 0; j < bl; j++) {
            u128 prod = (u128)a[i] * (u128)b[j] + (u128)r[i + j] + carry;
            r[i + j] = (uint64_t)prod;
            carry = prod >> 64;
        }
        if (carry) {
            r[i + bl] += (uint64_t)carry;
        }
    }
    *out_len = max;
    normalize_limbs(r, out_len);
    return r;
}

ChValue ch_bignum_add(ChGC *gc, ChValue a, ChValue b) {
    MagView va = view_of(a);
    MagView vb = view_of(b);
    size_t len = 0;
    uint64_t *limbs;
    int positive;
    if (va.positive == vb.positive) {
        limbs = add_mag(va.limbs, va.len, vb.limbs, vb.len, &len);
        positive = va.positive;
    } else {
        int c = cmp_mag(va.limbs, va.len, vb.limbs, vb.len);
        if (c == 0) {
            return ch_make_fixnum(0);
        }
        if (c > 0) {
            limbs = sub_mag(va.limbs, va.len, vb.limbs, vb.len, &len);
            positive = va.positive;
        } else {
            limbs = sub_mag(vb.limbs, vb.len, va.limbs, va.len, &len);
            positive = vb.positive;
        }
    }
    return make_from_mag(gc, limbs, len, positive);
}

ChValue ch_bignum_sub(ChGC *gc, ChValue a, ChValue b) {
    ChValue nb = ch_bignum_negate(gc, b);
    ch_gc_push(gc, &a);
    ch_gc_push(gc, &nb);
    ChValue r = ch_bignum_add(gc, a, nb);
    ch_gc_pop_n(gc, 2);
    return r;
}

ChValue ch_bignum_mul(ChGC *gc, ChValue a, ChValue b) {
    MagView va = view_of(a);
    MagView vb = view_of(b);
    size_t len = 0;
    uint64_t *limbs = mul_mag(va.limbs, va.len, vb.limbs, vb.len, &len);
    int positive = (va.positive == vb.positive) || len == 0;
    if (len == 0) {
        free(limbs);
        return ch_make_fixnum(0);
    }
    return make_from_mag(gc, limbs, len, positive);
}

ChValue ch_bignum_negate(ChGC *gc, ChValue a) {
    if (ch_is_fixnum(a)) {
        int64_t n = ch_to_fixnum(a);
        if (n == CH_FIXNUM_MIN) {
            /* -(-2^47) = 2^47, needs bignum */
            uint64_t mag = (uint64_t)1 << 47;
            return ch_gc_make_bignum_from_limbs(gc, &mag, 1, 1);
        }
        return ch_make_fixnum(-n);
    }
    ChBignum *bn = ch_as_bignum(a);
    if (bn->len == 0) {
        return ch_make_fixnum(0);
    }
    return ch_gc_make_bignum_from_limbs(gc, bn->limbs, bn->len, !bn->positive);
}

ChValue ch_bignum_abs(ChGC *gc, ChValue a) {
    if (ch_is_fixnum(a)) {
        int64_t n = ch_to_fixnum(a);
        if (n == CH_FIXNUM_MIN) {
            uint64_t mag = (uint64_t)1 << 47;
            return ch_gc_make_bignum_from_limbs(gc, &mag, 1, 1);
        }
        return ch_make_fixnum(n < 0 ? -n : n);
    }
    ChBignum *bn = ch_as_bignum(a);
    if (bn->positive || bn->len == 0) {
        return a;
    }
    return ch_gc_make_bignum_from_limbs(gc, bn->limbs, bn->len, 1);
}

/* Division of magnitudes: Knuth-light for small cases; schoolbook long div. */
static int div_mag(const uint64_t *num, size_t nl, const uint64_t *den, size_t dl, uint64_t **q_out,
                   size_t *ql_out, uint64_t **r_out, size_t *rl_out) {
    if (dl == 0) {
        return -1; /* div by zero */
    }
    if (cmp_mag(num, nl, den, dl) < 0) {
        *q_out = NULL;
        *ql_out = 0;
        if (nl == 0) {
            *r_out = NULL;
            *rl_out = 0;
        } else {
            *r_out = (uint64_t *)malloc(sizeof(uint64_t) * nl);
            if (!*r_out) {
                abort();
            }
            memcpy(*r_out, num, sizeof(uint64_t) * nl);
            *rl_out = nl;
        }
        return 0;
    }
    /* Single-limb divisor fast path. */
    if (dl == 1) {
        uint64_t d = den[0];
        uint64_t *q = (uint64_t *)calloc(nl, sizeof(uint64_t));
        if (!q) {
            abort();
        }
        u128 rem = 0;
        for (size_t i = nl; i > 0; i--) {
            rem = (rem << 64) | num[i - 1];
            q[i - 1] = (uint64_t)(rem / d);
            rem = rem % d;
        }
        size_t ql = nl;
        normalize_limbs(q, &ql);
        *q_out = q;
        *ql_out = ql;
        if (rem == 0) {
            *r_out = NULL;
            *rl_out = 0;
        } else {
            uint64_t *r = (uint64_t *)malloc(sizeof(uint64_t));
            if (!r) {
                abort();
            }
            r[0] = (uint64_t)rem;
            *r_out = r;
            *rl_out = 1;
        }
        return 0;
    }
    /* Multi-limb: binary long division (simple, not fastest). */
    uint64_t *rem = NULL;
    size_t rl = 0;
    if (nl > 0) {
        rem = (uint64_t *)malloc(sizeof(uint64_t) * nl);
        if (!rem) {
            abort();
        }
        memcpy(rem, num, sizeof(uint64_t) * nl);
        rl = nl;
    }
    size_t max_q = nl - dl + 1;
    uint64_t *q = (uint64_t *)calloc(max_q, sizeof(uint64_t));
    if (!q) {
        abort();
    }
    for (size_t shift = max_q; shift > 0; shift--) {
        size_t s = shift - 1;
        /* While rem >= den << (s*64), subtract. Use bit binary search per limb. */
        for (;;) {
            /* Compare rem with den shifted by s limbs */
            size_t need = dl + s;
            if (rl < need) {
                break;
            }
            if (rl == need) {
                int c = cmp_mag(rem + s, rl - s, den, dl);
                if (c < 0) {
                    break;
                }
            }
            /* rem -= den << s */
            u128 borrow = 0;
            for (size_t i = 0; i < dl; i++) {
                u128 av = rem[i + s];
                u128 bv = (u128)den[i] + borrow;
                if (av >= bv) {
                    rem[i + s] = (uint64_t)(av - bv);
                    borrow = 0;
                } else {
                    rem[i + s] = (uint64_t)(((u128)1 << 64) + av - bv);
                    borrow = 1;
                }
            }
            size_t j = dl + s;
            while (borrow && j < rl) {
                if (rem[j] >= borrow) {
                    rem[j] -= (uint64_t)borrow;
                    borrow = 0;
                } else {
                    rem[j] = (uint64_t)(((u128)1 << 64) + rem[j] - borrow);
                    borrow = 1;
                    j++;
                }
            }
            normalize_limbs(rem, &rl);
            q[s]++;
        }
    }
    size_t ql = max_q;
    normalize_limbs(q, &ql);
    *q_out = q;
    *ql_out = ql;
    normalize_limbs(rem, &rl);
    *r_out = rem;
    *rl_out = rl;
    return 0;
}

ChValue ch_bignum_quotient(ChGC *gc, ChValue a, ChValue b) {
    MagView va = view_of(a);
    MagView vb = view_of(b);
    if (vb.len == 0) {
        return CH_UNDEFINED;
    }
    uint64_t *q = NULL, *r = NULL;
    size_t ql = 0, rl = 0;
    if (div_mag(va.limbs, va.len, vb.limbs, vb.len, &q, &ql, &r, &rl) != 0) {
        return CH_UNDEFINED;
    }
    free(r);
    int positive = (va.positive == vb.positive) || ql == 0;
    if (ql == 0) {
        free(q);
        return ch_make_fixnum(0);
    }
    return make_from_mag(gc, q, ql, positive);
}

ChValue ch_bignum_remainder(ChGC *gc, ChValue a, ChValue b) {
    MagView va = view_of(a);
    MagView vb = view_of(b);
    if (vb.len == 0) {
        return CH_UNDEFINED;
    }
    uint64_t *q = NULL, *r = NULL;
    size_t ql = 0, rl = 0;
    if (div_mag(va.limbs, va.len, vb.limbs, vb.len, &q, &ql, &r, &rl) != 0) {
        return CH_UNDEFINED;
    }
    free(q);
    /* Remainder sign follows dividend (C99 / Scheme trunc toward 0). */
    if (rl == 0) {
        free(r);
        return ch_make_fixnum(0);
    }
    return make_from_mag(gc, r, rl, va.positive);
}

ChValue ch_bignum_parse_decimal(ChGC *gc, const char *text, size_t len) {
    if (!text || len == 0) {
        return CH_UNDEFINED;
    }
    size_t i = 0;
    int positive = 1;
    if (text[0] == '+') {
        i = 1;
    } else if (text[0] == '-') {
        positive = 0;
        i = 1;
    }
    if (i >= len) {
        return CH_UNDEFINED;
    }
    for (size_t j = i; j < len; j++) {
        if (text[j] < '0' || text[j] > '9') {
            return CH_UNDEFINED;
        }
    }
    uint64_t *limbs = NULL;
    size_t llen = 0;
    uint64_t zero = 0;
    for (; i < len; i++) {
        uint64_t digit = (uint64_t)(text[i] - '0');
        uint64_t ten = 10;
        const uint64_t *base_limbs = limbs ? limbs : &zero;
        size_t base_len = limbs ? llen : 0;
        size_t nlen = 0;
        uint64_t *scaled = mul_mag(base_limbs, base_len, &ten, 1, &nlen);
        size_t alen = 0;
        uint64_t *added =
            add_mag(scaled ? scaled : &zero, scaled ? nlen : 0, &digit, digit || 1 ? 1 : 0, &alen);
        /* always add one-limb digit (even 0) */
        free(scaled);
        free(limbs);
        limbs = added;
        llen = alen;
        normalize_limbs(limbs, &llen);
    }
    if (llen == 0) {
        free(limbs);
        return ch_make_fixnum(0);
    }
    return make_from_mag(gc, limbs, llen, positive);
}

char *ch_bignum_to_string(ChValue v) {
    if (ch_is_fixnum(v)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)ch_to_fixnum(v));
        return strdup(buf);
    }
    MagView m = view_of(v);
    if (m.len == 0) {
        return strdup("0");
    }
    /* Copy magnitude for destructive division by 10^9 chunks. */
    uint64_t *work = (uint64_t *)malloc(sizeof(uint64_t) * m.len);
    if (!work) {
        abort();
    }
    memcpy(work, m.limbs, sizeof(uint64_t) * m.len);
    size_t wlen = m.len;

    char *digits = (char *)malloc(m.len * 20 + 4);
    if (!digits) {
        abort();
    }
    size_t dpos = 0;
    const uint64_t base = 1000000000ULL; /* 10^9 */
    while (wlen > 0) {
        u128 rem = 0;
        for (size_t i = wlen; i > 0; i--) {
            rem = (rem << 64) | work[i - 1];
            work[i - 1] = (uint64_t)(rem / base);
            rem = rem % base;
        }
        normalize_limbs(work, &wlen);
        char chunk[16];
        if (wlen == 0) {
            snprintf(chunk, sizeof(chunk), "%llu", (unsigned long long)rem);
        } else {
            snprintf(chunk, sizeof(chunk), "%09llu", (unsigned long long)rem);
        }
        size_t clen = strlen(chunk);
        for (size_t i = clen; i > 0; i--) {
            digits[dpos++] = chunk[i - 1];
        }
    }
    free(work);
    /* digits are reversed */
    size_t out_len = dpos + (m.positive ? 0 : 1);
    char *out = (char *)malloc(out_len + 1);
    if (!out) {
        abort();
    }
    size_t o = 0;
    if (!m.positive) {
        out[o++] = '-';
    }
    for (size_t i = dpos; i > 0; i--) {
        out[o++] = digits[i - 1];
    }
    out[o] = '\0';
    free(digits);
    return out;
}

double ch_bignum_to_f64(ChValue v) {
    if (ch_is_fixnum(v)) {
        return (double)ch_to_fixnum(v);
    }
    MagView m = view_of(v);
    if (m.len == 0) {
        return 0.0;
    }
    double r = 0.0;
    double scale = 1.0;
    for (size_t i = 0; i < m.len; i++) {
        r += (double)m.limbs[i] * scale;
        scale *= 18446744073709551616.0; /* 2^64 */
    }
    return m.positive ? r : -r;
}
