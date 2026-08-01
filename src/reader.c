#include "chaaya/reader.h"

#include "chaaya/bignum.h"
#include "chaaya/complex.h"
#include "chaaya/rational.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ch_reader_init(ChReader *r, ChGC *gc, const char *src, size_t len) {
    r->gc = gc;
    r->src = src;
    r->len = len;
    r->pos = 0;
    r->fold_case = 0;
    r->refill = NULL;
    r->refill_ctx = NULL;
    r->error[0] = '\0';
    memset(r->label_set, 0, sizeof(r->label_set));
    for (size_t i = 0; i < CH_READER_MAX_LABELS; i++) {
        r->labels[i] = CH_UNDEFINED;
    }
}

void ch_reader_set_refill(ChReader *r, ChReaderRefillFn refill, void *ctx) {
    r->refill = refill;
    r->refill_ctx = ctx;
}

const char *ch_reader_error(const ChReader *r) {
    return r->error;
}

static bool try_refill(ChReader *r) {
    if (!r->refill) {
        return false;
    }
    const char *old_src = r->src;
    size_t old_len = r->len;
    size_t old_pos = r->pos;
    if (!r->refill(r, r->refill_ctx)) {
        return false;
    }
    if (r->pos < r->len) {
        return true;
    }
    return r->src != old_src || r->len != old_len || r->pos != old_pos;
}

static int peek(ChReader *r) {
    while (r->pos >= r->len) {
        if (!try_refill(r)) {
            return -1;
        }
    }
    return (unsigned char)r->src[r->pos];
}

static int peek_n(ChReader *r, size_t ahead) {
    size_t want = r->pos + ahead;
    while (want >= r->len) {
        if (!try_refill(r)) {
            return -1;
        }
    }
    return (unsigned char)r->src[want];
}

static int advance(ChReader *r) {
    int c = peek(r);
    if (c < 0) {
        return -1;
    }
    r->pos++;
    return c;
}

static ChReadStatus read_datum(ChReader *r, ChValue *out);
static ChReadStatus fail(ChReader *r, const char *msg);
static bool is_delim(int c);

static bool is_utf8_lead_byte(int c) {
    unsigned char u = (unsigned char)c;
    return u >= 0xC2 && u <= 0xF4;
}

static bool is_utf8_continuation_byte(int c) {
    unsigned char u = (unsigned char)c;
    return u >= 0x80 && u <= 0xBF;
}

static ChReadStatus skip_ws_and_comments(ChReader *r) {
    for (;;) {
        int c = peek(r);
        if (c < 0) {
            return CH_READ_OK;
        }
        if (c == ';') {
            while (peek(r) >= 0 && peek(r) != '\n') {
                advance(r);
            }
            continue;
        }
        if (isspace((unsigned char)c)) {
            advance(r);
            continue;
        }
        if (c == '#' && peek_n(r, 1) == ';') {
            advance(r);
            advance(r);
            /* Nested #; / whitespace first; commenting the dotted-list marker is an
             * error (R7RS 7.1.1). */
            ChReadStatus st = skip_ws_and_comments(r);
            if (st != CH_READ_OK) {
                return st;
            }
            if (peek(r) == '.' && is_delim(peek_n(r, 1))) {
                return fail(r, "#;: cannot comment dotted-list marker");
            }
            ChValue ignored = CH_NIL;
            ch_gc_push(r->gc, &ignored);
            st = read_datum(r, &ignored);
            ch_gc_pop(r->gc);
            if (st == CH_READ_EOF) {
                snprintf(r->error, sizeof(r->error), "#;: expected datum");
                return CH_READ_ERROR;
            }
            if (st != CH_READ_OK) {
                return st;
            }
            continue;
        }
        /* Nested block comments #| ... |# */
        if (c == '#' && peek_n(r, 1) == '|') {
            advance(r);
            advance(r);
            int depth = 1;
            while (depth > 0) {
                int ch = advance(r);
                if (ch < 0) {
                    return fail(r, "unterminated block comment");
                }
                if (ch == '#' && peek(r) == '|') {
                    advance(r);
                    depth++;
                } else if (ch == '|' && peek(r) == '#') {
                    advance(r);
                    depth--;
                }
            }
            continue;
        }
        /* #!fold-case / #!no-fold-case directives (lengths exclude trailing ws) */
        if (c == '#' && peek_n(r, 1) == '!') {
            size_t start = r->pos;
            while (peek(r) >= 0 && peek(r) != '\n' && !isspace((unsigned char)peek(r))) {
                advance(r);
            }
            size_t n = r->pos - start;
            if (n == 11 && strncmp(r->src + start, "#!fold-case", 11) == 0) {
                r->fold_case = 1;
                continue;
            }
            if (n == 14 && strncmp(r->src + start, "#!no-fold-case", 14) == 0) {
                r->fold_case = 0;
                continue;
            }
            /* Unknown #! — treat as comment to EOL */
            while (peek(r) >= 0 && peek(r) != '\n') {
                advance(r);
            }
            continue;
        }
        return CH_READ_OK;
    }
}

static bool is_delim(int c) {
    return c < 0 || isspace(c) || c == '(' || c == ')' || c == '"' || c == ';' || c == '\'' ||
           c == '`' || c == ',' || c == '[' || c == ']';
}

static bool is_ident_start(int c) {
    if (c < 0) {
        return false;
    }
    if (isalpha((unsigned char)c) || is_utf8_lead_byte(c)) {
        return true;
    }
    return strchr("!$%&*/:<=>?^_~+-.@", c) != NULL;
}

static bool is_ident_subsequent(int c) {
    if (is_ident_start(c) || isdigit((unsigned char)c)) {
        return true;
    }
    if (is_utf8_continuation_byte(c)) {
        return true;
    }
    return strchr("+-.@", c) != NULL;
}

static ChReadStatus fail(ChReader *r, const char *msg) {
    snprintf(r->error, sizeof(r->error), "%s", msg);
    return CH_READ_ERROR;
}

static bool string_buf_append_bytes(char **buf, size_t *len, size_t *cap, const char *bytes,
                                    size_t n) {
    if (*len + n >= *cap) {
        size_t ncap = *cap ? *cap : 64;
        while (*len + n >= ncap) {
            ncap *= 2;
        }
        char *nb = (char *)realloc(*buf, ncap);
        if (!nb) {
            return false;
        }
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, bytes, n);
    *len += n;
    return true;
}

static bool string_buf_append_cp(char **buf, size_t *len, size_t *cap, uint32_t cp) {
    char encoded[4];
    size_t n = 0;
    if (cp <= 0x7Fu) {
        encoded[0] = (char)cp;
        n = 1;
    } else if (cp <= 0x7FFu) {
        encoded[0] = (char)(0xC0u | (cp >> 6));
        encoded[1] = (char)(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
        return false;
    } else if (cp <= 0xFFFFu) {
        encoded[0] = (char)(0xE0u | (cp >> 12));
        encoded[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[2] = (char)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else if (cp <= 0x10FFFFu) {
        encoded[0] = (char)(0xF0u | (cp >> 18));
        encoded[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        encoded[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[3] = (char)(0x80u | (cp & 0x3Fu));
        n = 4;
    } else {
        return false;
    }
    return string_buf_append_bytes(buf, len, cap, encoded, n);
}

/* Decode a \ escape shared by strings and |delimited| identifiers.
 * On success appends into buf (unless use_cp ends up 0 for intraline ws). */
static ChReadStatus read_escape_into(ChReader *r, char **buf, size_t *len, size_t *cap,
                                     const char *what) {
    int e = advance(r);
    if (e < 0) {
        return fail(r, "unterminated escape");
    }
    uint32_t cp = 0;
    int use_cp = 1;
    switch (e) {
    case 'a':
        cp = 0x07;
        break;
    case 'b':
        cp = 0x08;
        break;
    case 't':
        cp = '\t';
        break;
    case 'n':
        cp = '\n';
        break;
    case 'r':
        cp = '\r';
        break;
    case '|':
        cp = '|';
        break;
    case '\\':
        cp = '\\';
        break;
    case '"':
        cp = '"';
        break;
    case 'x':
    case 'X': {
        uint32_t hex = 0;
        int digits = 0;
        for (;;) {
            int dch = peek(r);
            if (dch < 0) {
                return fail(r, "unterminated hex escape");
            }
            if (dch == ';') {
                advance(r);
                break;
            }
            int d = -1;
            if (dch >= '0' && dch <= '9') {
                d = dch - '0';
            } else if (dch >= 'a' && dch <= 'f') {
                d = dch - 'a' + 10;
            } else if (dch >= 'A' && dch <= 'F') {
                d = dch - 'A' + 10;
            }
            if (d < 0) {
                return fail(r, "bad hex escape");
            }
            advance(r);
            if (digits >= 8 || hex > 0x10FFFF) {
                return fail(r, "hex escape out of range");
            }
            hex = (hex << 4) | (uint32_t)d;
            digits++;
        }
        if (digits == 0 || hex > 0x10FFFF) {
            return fail(r, "bad hex escape");
        }
        cp = hex;
        break;
    }
    default:
        /* Intraline whitespace: \<ws>*<line ending><ws>* (strings only, but harmless in idents) */
        if (e == ' ' || e == '\t' || e == '\n' || e == '\r') {
            int ch = e;
            while (ch == ' ' || ch == '\t') {
                ch = advance(r);
                if (ch < 0) {
                    return fail(r, "unterminated escape");
                }
            }
            if (ch == '\r') {
                if (peek(r) == '\n') {
                    advance(r);
                }
            } else if (ch != '\n') {
                (void)what;
                return fail(r, "bad escape");
            }
            while (peek(r) == ' ' || peek(r) == '\t') {
                advance(r);
            }
            use_cp = 0;
            break;
        }
        (void)what;
        return fail(r, "bad escape");
    }
    if (use_cp) {
        if (!string_buf_append_cp(buf, len, cap, cp)) {
            return fail(r, "bad escape");
        }
    }
    return CH_READ_OK;
}

static ChReadStatus read_string(ChReader *r, ChValue *out) {
    advance(r); /* skip " */
    size_t cap = 64;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        return fail(r, "out of memory");
    }
    while (peek(r) >= 0 && peek(r) != '"') {
        int c = advance(r);
        if (c == '\\') {
            ChReadStatus st = read_escape_into(r, &buf, &len, &cap, "string");
            if (st != CH_READ_OK) {
                free(buf);
                return st;
            }
            continue;
        }
        /* Copy raw UTF-8 lead/continuation as bytes. */
        char raw = (char)c;
        if (!string_buf_append_bytes(&buf, &len, &cap, &raw, 1)) {
            free(buf);
            return fail(r, "out of memory");
        }
    }
    if (peek(r) != '"') {
        free(buf);
        return fail(r, "unterminated string");
    }
    advance(r);
    *out = ch_gc_make_string(r->gc, buf, len);
    free(buf);
    return CH_READ_OK;
}

/* R7RS |...| identifiers with the same \ escapes as strings. */
static ChReadStatus read_delimited_identifier(ChReader *r, ChValue *out) {
    advance(r); /* skip opening | */
    size_t cap = 64;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        return fail(r, "out of memory");
    }
    while (peek(r) >= 0 && peek(r) != '|') {
        int c = advance(r);
        if (c == '\\') {
            ChReadStatus st = read_escape_into(r, &buf, &len, &cap, "identifier");
            if (st != CH_READ_OK) {
                free(buf);
                return st;
            }
            continue;
        }
        char raw = (char)c;
        if (!string_buf_append_bytes(&buf, &len, &cap, &raw, 1)) {
            free(buf);
            return fail(r, "out of memory");
        }
    }
    if (peek(r) != '|') {
        free(buf);
        return fail(r, "unterminated identifier");
    }
    advance(r);
    if (r->fold_case) {
        for (size_t i = 0; i < len; i++) {
            unsigned char ch = (unsigned char)buf[i];
            if (ch < 0x80) {
                buf[i] = (char)tolower(ch);
            }
        }
    }
    *out = ch_gc_intern_symbol(r->gc, buf, len);
    free(buf);
    return CH_READ_OK;
}

static bool try_parse_number(ChGC *gc, const char *text, size_t len, ChValue *out);

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

static int eq_ci_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
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

/* Parse unsigned integer digits in base into a ChValue integer. */
static bool parse_uint_base(ChGC *gc, const char *text, size_t len, int base, ChValue *out) {
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (digit_in_base((unsigned char)text[i], base) < 0) {
            return false;
        }
    }
    if (base == 10) {
        ChValue v = ch_bignum_parse_decimal(gc, text, len);
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
    for (size_t i = 0; i < len; i++) {
        int d = digit_in_base((unsigned char)text[i], base);
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
    char *tmp = (char *)malloc(len + 1);
    if (!tmp) {
        return false;
    }
    memcpy(tmp, text, len);
    tmp[len] = '\0';
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

static bool try_parse_number(ChGC *gc, const char *text, size_t len, ChValue *out) {
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
    if (!try_parse_number_in_base(gc, text + pos, len - pos, base, out)) {
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
        size_t i = 0;
        if (text[0] == '+' || text[0] == '-') {
            i = 1;
        }
        int all_digits = (i < len);
        for (size_t j = i; j < len; j++) {
            if (text[j] < '0' || text[j] > '9') {
                all_digits = 0;
                break;
            }
        }
        if (all_digits) {
            ChValue v = ch_bignum_parse_decimal(gc, text, len);
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

static ChReadStatus read_atom(ChReader *r, ChValue *out) {
    size_t start = r->pos;
    bool ident_mode = is_ident_start(peek(r));
    while (!is_delim(peek(r))) {
        if (ident_mode && !is_ident_subsequent(peek(r))) {
            break;
        }
        advance(r);
    }
    size_t len = r->pos - start;
    const char *text = r->src + start;
    if (len == 0) {
        return fail(r, "empty atom");
    }
    if (len == 2 && text[0] == '#' && (text[1] == 't' || text[1] == 'T')) {
        *out = CH_TRUE;
        return CH_READ_OK;
    }
    if (len == 2 && text[0] == '#' && (text[1] == 'f' || text[1] == 'F')) {
        *out = CH_FALSE;
        return CH_READ_OK;
    }
    if (len == 5 && strncmp(text, "#true", 5) == 0) {
        *out = CH_TRUE;
        return CH_READ_OK;
    }
    if (len == 6 && strncmp(text, "#false", 6) == 0) {
        *out = CH_FALSE;
        return CH_READ_OK;
    }
    if (len >= 3 && text[0] == '#' && text[1] == '\\') {
        if (len == 3) {
            *out = ch_make_char((unsigned char)text[2]);
            return CH_READ_OK;
        }
        /* #\x⟨hex⟩ or #\x⟨hex⟩; (semicolon optional for Kaappi/Chibi parity) */
        if (text[2] == 'x' || text[2] == 'X') {
            size_t i = 3;
            uint32_t cp = 0;
            int digits = 0;
            while (i < len && text[i] != ';') {
                int d = -1;
                char c = text[i];
                if (c >= '0' && c <= '9') {
                    d = c - '0';
                } else if (c >= 'a' && c <= 'f') {
                    d = c - 'a' + 10;
                } else if (c >= 'A' && c <= 'F') {
                    d = c - 'A' + 10;
                }
                if (d < 0) {
                    break;
                }
                if (digits >= 8 || cp > 0x10FFFF) {
                    return fail(r, "hex character out of range");
                }
                cp = (cp << 4) | (uint32_t)d;
                digits++;
                i++;
            }
            if (digits == 0) {
                return fail(r, "empty hex character");
            }
            if (i < len && text[i] == ';') {
                i++;
            }
            if (i != len || cp > 0x10FFFF) {
                return fail(r, "bad hex character");
            }
            *out = ch_make_char(cp);
            return CH_READ_OK;
        }
        /* Named characters: len is 2 ("#\") plus name length. */
        if (len == 9 && strncmp(text + 2, "newline", 7) == 0) {
            *out = ch_make_char('\n');
            return CH_READ_OK;
        }
        if (len == 7 && strncmp(text + 2, "space", 5) == 0) {
            *out = ch_make_char(' ');
            return CH_READ_OK;
        }
        if (len == 5 && strncmp(text + 2, "tab", 3) == 0) {
            *out = ch_make_char('\t');
            return CH_READ_OK;
        }
        if (len == 6 && strncmp(text + 2, "null", 4) == 0) {
            *out = ch_make_char(0);
            return CH_READ_OK;
        }
        if (len == 7 && strncmp(text + 2, "alarm", 5) == 0) {
            *out = ch_make_char(0x07);
            return CH_READ_OK;
        }
        if (len == 11 && strncmp(text + 2, "backspace", 9) == 0) {
            *out = ch_make_char(0x08);
            return CH_READ_OK;
        }
        if (len == 8 && strncmp(text + 2, "return", 6) == 0) {
            *out = ch_make_char(0x0D);
            return CH_READ_OK;
        }
        if (len == 8 && strncmp(text + 2, "escape", 6) == 0) {
            *out = ch_make_char(0x1B);
            return CH_READ_OK;
        }
        if (len == 8 && strncmp(text + 2, "delete", 6) == 0) {
            *out = ch_make_char(0x7F);
            return CH_READ_OK;
        }
        /* Multi-byte UTF-8 or named char: take first Unicode scalar after #\ */
        {
            const unsigned char *p = (const unsigned char *)(text + 2);
            size_t rem = len - 2;
            uint32_t cp = 0;
            size_t used = 0;
            if (rem >= 1 && p[0] < 0x80) {
                cp = p[0];
                used = 1;
            } else if (rem >= 2 && (p[0] & 0xE0) == 0xC0) {
                cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
                used = 2;
            } else if (rem >= 3 && (p[0] & 0xF0) == 0xE0) {
                cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                     (p[2] & 0x3F);
                used = 3;
            } else if (rem >= 4 && (p[0] & 0xF8) == 0xF0) {
                cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                     ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
                used = 4;
            }
            if (used == rem && cp <= 0x10FFFF) {
                *out = ch_make_char(cp);
                return CH_READ_OK;
            }
        }
        return fail(r, "unknown character name");
    }
    ChValue num;
    if (try_parse_number(r->gc, text, len, &num)) {
        *out = num;
        return CH_READ_OK;
    }
    if (r->fold_case) {
        char *folded = (char *)malloc(len + 1);
        if (!folded) {
            return fail(r, "out of memory");
        }
        for (size_t i = 0; i < len; i++) {
            unsigned char ch = (unsigned char)text[i];
            folded[i] = (char)((ch < 0x80) ? tolower(ch) : ch);
        }
        folded[len] = '\0';
        *out = ch_gc_intern_symbol(r->gc, folded, len);
        free(folded);
        return CH_READ_OK;
    }
    *out = ch_gc_intern_symbol(r->gc, text, len);
    return CH_READ_OK;
}

static ChReadStatus read_list(ChReader *r, ChValue *out) {
    advance(r); /* ( */
    ChReadStatus ws = skip_ws_and_comments(r);
    if (ws != CH_READ_OK) {
        return ws;
    }
    if (peek(r) == ')') {
        advance(r);
        *out = CH_NIL;
        return CH_READ_OK;
    }

    ChValue head = CH_NIL;
    ChValue tail = CH_NIL;
    ch_gc_push(r->gc, &head);
    ch_gc_push(r->gc, &tail);

    while (peek(r) >= 0 && peek(r) != ')') {
        ws = skip_ws_and_comments(r);
        if (ws != CH_READ_OK) {
            ch_gc_pop_n(r->gc, 2);
            return ws;
        }
        if (peek(r) == ')') {
            break;
        }
        if (peek(r) == '.') {
            advance(r);
            if (!is_delim(peek(r))) {
                /* not a dotted pair — put back conceptually by reading as atom starting with . */
                r->pos--;
            } else {
                ws = skip_ws_and_comments(r);
                if (ws != CH_READ_OK) {
                    ch_gc_pop_n(r->gc, 2);
                    return ws;
                }
                ChValue rest = CH_NIL;
                ch_gc_push(r->gc, &rest);
                ChReadStatus st = read_datum(r, &rest);
                if (st != CH_READ_OK) {
                    ch_gc_pop_n(r->gc, 3);
                    return st;
                }
                ws = skip_ws_and_comments(r);
                if (ws != CH_READ_OK) {
                    ch_gc_pop_n(r->gc, 3);
                    return ws;
                }
                if (peek(r) != ')') {
                    ch_gc_pop_n(r->gc, 3);
                    return fail(r, "expected ) after dotted pair");
                }
                advance(r);
                if (ch_is_nil(head)) {
                    ch_gc_pop_n(r->gc, 3);
                    return fail(r, "dotted pair with empty head");
                }
                ch_set_cdr(tail, rest);
                ch_gc_pop_n(r->gc, 3);
                *out = head;
                return CH_READ_OK;
            }
        }

        ChValue item = CH_NIL;
        ch_gc_push(r->gc, &item);
        ChReadStatus st = read_datum(r, &item);
        if (st != CH_READ_OK) {
            ch_gc_pop_n(r->gc, 3);
            return st;
        }
        ChValue cell = ch_gc_cons(r->gc, item, CH_NIL);
        ch_gc_pop(r->gc); /* item */
        if (ch_is_nil(head)) {
            head = cell;
            tail = cell;
        } else {
            ch_set_cdr(tail, cell);
            tail = cell;
        }
        ws = skip_ws_and_comments(r);
        if (ws != CH_READ_OK) {
            ch_gc_pop_n(r->gc, 2);
            return ws;
        }
    }
    if (peek(r) != ')') {
        ch_gc_pop_n(r->gc, 2);
        return fail(r, "unterminated list");
    }
    advance(r);
    *out = head;
    ch_gc_pop_n(r->gc, 2);
    return CH_READ_OK;
}

static ChReadStatus read_vector(ChReader *r, ChValue *out) {
    /* already consumed #, next is ( */
    advance(r); /* ( */
    ChValue items[256];
    size_t n = 0;
    ChReadStatus ws = skip_ws_and_comments(r);
    if (ws != CH_READ_OK) {
        return ws;
    }
    while (peek(r) >= 0 && peek(r) != ')') {
        if (n >= 256) {
            return fail(r, "vector too large");
        }
        ChReadStatus st = read_datum(r, &items[n]);
        if (st != CH_READ_OK) {
            return st;
        }
        n++;
        ws = skip_ws_and_comments(r);
        if (ws != CH_READ_OK) {
            return ws;
        }
    }
    if (peek(r) != ')') {
        return fail(r, "unterminated vector");
    }
    advance(r);
    ChValue vec = ch_gc_make_vector(r->gc, n, CH_FALSE);
    ChVector *v = ch_as_vector(vec);
    for (size_t i = 0; i < n; i++) {
        v->items[i] = items[i];
    }
    *out = vec;
    return CH_READ_OK;
}

static ChReadStatus read_bytevector(ChReader *r, ChValue *out) {
    /* already consumed #, next is u/U */
    int u = advance(r);
    if (u != 'u' && u != 'U') {
        return fail(r, "expected u8 bytevector literal");
    }
    if (advance(r) != '8') {
        return fail(r, "expected u8 bytevector literal");
    }
    if (peek(r) != '(') {
        return fail(r, "expected ( after #u8");
    }
    advance(r); /* ( */

    size_t cap = 16;
    size_t len = 0;
    uint8_t *bytes = (uint8_t *)malloc(cap);
    if (!bytes) {
        return fail(r, "out of memory");
    }

    ChReadStatus ws = skip_ws_and_comments(r);
    if (ws != CH_READ_OK) {
        free(bytes);
        return ws;
    }
    while (peek(r) >= 0 && peek(r) != ')') {
        ChValue item = CH_NIL;
        ch_gc_push(r->gc, &item);
        ChReadStatus st = read_datum(r, &item);
        ch_gc_pop(r->gc);
        if (st != CH_READ_OK) {
            free(bytes);
            return st;
        }
        if (!ch_is_fixnum(item)) {
            free(bytes);
            return fail(r, "bytevector element is not an exact integer");
        }
        int64_t n = ch_to_fixnum(item);
        if (n < 0 || n > 255) {
            free(bytes);
            return fail(r, "bytevector element out of range");
        }
        if (len >= cap) {
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(bytes, cap);
            if (!nb) {
                free(bytes);
                return fail(r, "out of memory");
            }
            bytes = nb;
        }
        bytes[len++] = (uint8_t)n;
        ws = skip_ws_and_comments(r);
        if (ws != CH_READ_OK) {
            free(bytes);
            return ws;
        }
    }
    if (peek(r) != ')') {
        free(bytes);
        return fail(r, "unterminated bytevector");
    }
    advance(r);
    ChValue bv = ch_gc_make_bytevector(r->gc, len, 0);
    if (len > 0) {
        memcpy(ch_as_bytevector(bv)->data, bytes, len);
    }
    free(bytes);
    *out = bv;
    return CH_READ_OK;
}

static ChReadStatus read_abbrev(ChReader *r, const char *name, ChValue *out) {
    ChValue inner = CH_NIL;
    ch_gc_push(r->gc, &inner);
    ChReadStatus st = read_datum(r, &inner);
    if (st != CH_READ_OK) {
        ch_gc_pop(r->gc);
        return st;
    }
    ChValue sym = ch_gc_intern_symbol_cstr(r->gc, name);
    ChValue rest = ch_gc_cons(r->gc, inner, CH_NIL);
    ch_gc_push(r->gc, &rest);
    ch_gc_push(r->gc, &sym);
    *out = ch_gc_cons(r->gc, sym, rest);
    ch_gc_pop_n(r->gc, 3);
    return CH_READ_OK;
}

#define CH_DATUM_PATCH_DEPTH 1024

/* Patch leftover placeholder pairs when a label is bound to a non-pair. */
static void patch_placeholder(ChValue root, ChValue placeholder, ChValue replacement, int depth) {
    if (depth > CH_DATUM_PATCH_DEPTH || (!ch_is_pair(root) && !ch_is_vector(root))) {
        return;
    }
    if (ch_is_pair(root)) {
        if (ch_car(root) == placeholder) {
            ch_set_car(root, replacement);
        } else {
            patch_placeholder(ch_car(root), placeholder, replacement, depth + 1);
        }
        if (ch_cdr(root) == placeholder) {
            ch_set_cdr(root, replacement);
        } else {
            patch_placeholder(ch_cdr(root), placeholder, replacement, depth + 1);
        }
        return;
    }
    if (ch_is_vector(root)) {
        ChVector *vec = ch_as_vector(root);
        for (size_t i = 0; i < vec->len; i++) {
            if (vec->items[i] == placeholder) {
                vec->items[i] = replacement;
            } else {
                patch_placeholder(vec->items[i], placeholder, replacement, depth + 1);
            }
        }
    }
}

static ChReadStatus read_datum_label(ChReader *r, ChValue *out) {
    /* Caller consumed '#'; next chars are digits then '=' or '#'. */
    unsigned label = 0;
    int digits = 0;
    while (peek(r) >= '0' && peek(r) <= '9') {
        int d = advance(r) - '0';
        if (label > (CH_READER_MAX_LABELS - 1) / 10) {
            return fail(r, "datum label out of range");
        }
        label = label * 10u + (unsigned)d;
        digits++;
    }
    if (digits == 0 || label >= CH_READER_MAX_LABELS) {
        return fail(r, "bad datum label");
    }
    int mark = peek(r);
    if (mark == '#') {
        advance(r);
        if (!r->label_set[label]) {
            return fail(r, "undefined datum label");
        }
        *out = r->labels[label];
        return CH_READ_OK;
    }
    if (mark != '=') {
        return fail(r, "expected = or # after datum label");
    }
    advance(r);

    ChValue placeholder = ch_gc_cons(r->gc, CH_VOID, CH_NIL);
    ch_gc_push(r->gc, &placeholder);
    r->labels[label] = placeholder;
    r->label_set[label] = 1;

    ChValue datum = CH_UNDEFINED;
    ch_gc_push(r->gc, &datum);
    ChReadStatus st = read_datum(r, &datum);
    if (st != CH_READ_OK) {
        ch_gc_pop_n(r->gc, 2);
        r->label_set[label] = 0;
        r->labels[label] = CH_UNDEFINED;
        return st;
    }

    if (ch_is_pair(datum)) {
        ch_set_car(placeholder, ch_car(datum));
        ch_set_cdr(placeholder, ch_cdr(datum));
        r->labels[label] = placeholder;
        *out = placeholder;
        ch_gc_pop_n(r->gc, 2);
        return CH_READ_OK;
    }

    r->labels[label] = datum;
    patch_placeholder(datum, placeholder, datum, 0);
    *out = datum;
    ch_gc_pop_n(r->gc, 2);
    return CH_READ_OK;
}

static ChReadStatus read_datum(ChReader *r, ChValue *out) {
    ChReadStatus ws = skip_ws_and_comments(r);
    if (ws != CH_READ_OK) {
        return ws;
    }
    int c = peek(r);
    if (c < 0) {
        return CH_READ_EOF;
    }
    if (c == '(') {
        return read_list(r, out);
    }
    if (c == '\'') {
        advance(r);
        return read_abbrev(r, "quote", out);
    }
    if (c == '`') {
        advance(r);
        return read_abbrev(r, "quasiquote", out);
    }
    if (c == ',') {
        advance(r);
        if (peek(r) == '@') {
            advance(r);
            return read_abbrev(r, "unquote-splicing", out);
        }
        return read_abbrev(r, "unquote", out);
    }
    if (c == '"') {
        return read_string(r, out);
    }
    if (c == '|') {
        return read_delimited_identifier(r, out);
    }
    if (c == '#') {
        size_t save = r->pos;
        advance(r);
        int n = peek(r);
        if (n == '(') {
            return read_vector(r, out);
        }
        if ((n == 'u' || n == 'U') && peek_n(r, 1) == '8' && peek_n(r, 2) == '(') {
            return read_bytevector(r, out);
        }
        if (n >= '0' && n <= '9') {
            return read_datum_label(r, out);
        }
        r->pos = save;
        return read_atom(r, out);
    }
    if (c == ')') {
        return fail(r, "unexpected )");
    }
    if (is_ident_start(c) || isdigit(c) || c == '#' || c == '.' || c == '+' || c == '-') {
        return read_atom(r, out);
    }
    return fail(r, "unexpected character");
}

ChReadStatus ch_read_datum(ChReader *r, ChValue *out) {
    ChReadStatus ws = skip_ws_and_comments(r);
    if (ws != CH_READ_OK) {
        return ws;
    }
    if (peek(r) < 0) {
        return CH_READ_EOF;
    }
    memset(r->label_set, 0, sizeof(r->label_set));
    for (size_t i = 0; i < CH_READER_MAX_LABELS; i++) {
        r->labels[i] = CH_UNDEFINED;
    }
    return read_datum(r, out);
}
