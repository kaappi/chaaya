#include "chaaya/reader.h"

#include "chaaya/bignum.h"
#include "chaaya/complex.h"
#include "chaaya/rational.h"

#include <ctype.h>
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
            ChValue ignored = CH_NIL;
            ch_gc_push(r->gc, &ignored);
            ChReadStatus st = read_datum(r, &ignored);
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
        /* #!fold-case / #!no-fold-case directives */
        if (c == '#' && peek_n(r, 1) == '!') {
            size_t start = r->pos;
            while (peek(r) >= 0 && peek(r) != '\n' && !isspace((unsigned char)peek(r))) {
                advance(r);
            }
            size_t n = r->pos - start;
            if (n == 12 && strncmp(r->src + start, "#!fold-case", 12) == 0) {
                r->fold_case = 1;
                continue;
            }
            if (n == 15 && strncmp(r->src + start, "#!no-fold-case", 15) == 0) {
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
            int e = advance(r);
            if (e < 0) {
                free(buf);
                return fail(r, "unterminated string escape");
            }
            switch (e) {
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            case 'r':
                c = '\r';
                break;
            case '\\':
                c = '\\';
                break;
            case '"':
                c = '"';
                break;
            default:
                c = e;
                break;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return fail(r, "out of memory");
            }
            buf = nb;
        }
        buf[len++] = (char)c;
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
        *out = ch_make_complex(gc, 0.0, text[0] == '+' ? 1.0 : -1.0);
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
    if (split == 0) {
        /* Pure imaginary: Ni, +Ni, -Ni */
        if (!try_parse_real_f64(gc, text, body, &imag)) {
            return false;
        }
    } else {
        size_t ilen = body - split;
        if (!try_parse_real_f64(gc, text, split, &real)) {
            return false;
        }
        if (ilen == 1) {
            imag = text[split] == '+' ? 1.0 : -1.0;
        } else if (!try_parse_real_f64(gc, text + split, ilen, &imag)) {
            return false;
        }
    }
    *out = ch_make_complex(gc, real, imag);
    return true;
}

static bool try_parse_number(ChGC *gc, const char *text, size_t len, ChValue *out) {
    if (len == 0) {
        return false;
    }
    /* special: + - alone are symbols */
    if ((len == 1 && (text[0] == '+' || text[0] == '-')) ||
        (len == 1 && text[0] == '.')) {
        return false;
    }
    /* Complex before rational so 2/4i is pure-imaginary, not a fraction. */
    if (try_parse_complex(gc, text, len, out)) {
        return true;
    }
    /* Rational N/D with optional signs */
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
        ChValue num = ch_bignum_parse_decimal(gc, text, nlen);
        if (num == CH_UNDEFINED) {
            return false;
        }
        ch_gc_push(gc, &num);
        ChValue den = ch_bignum_parse_decimal(gc, slash + 1, dlen);
        if (den == CH_UNDEFINED) {
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
    /* Exact integer: optional sign + digits only → bignum path (demotes to fixnum). */
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
    char *tmp = (char *)malloc(len + 1);
    if (!tmp) {
        return false;
    }
    memcpy(tmp, text, len);
    tmp[len] = '\0';
    char *end = NULL;
    double dv = strtod(tmp, &end);
    if (end && *end == '\0') {
        *out = ch_make_flonum(dv);
        free(tmp);
        return true;
    }
    free(tmp);
    return false;
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
        if (len == 9 && strncmp(text + 2, "newline", 7) == 0) {
            *out = ch_make_char('\n');
            return CH_READ_OK;
        }
        if (len == 7 && strncmp(text + 2, "space", 5) == 0) {
            *out = ch_make_char(' ');
            return CH_READ_OK;
        }
        if (len == 6 && strncmp(text + 2, "tab", 3) == 0) {
            *out = ch_make_char('\t');
            return CH_READ_OK;
        }
        /* single named char fallback: take first code unit after #\ */
        *out = ch_make_char((unsigned char)text[2]);
        return CH_READ_OK;
    }
    if (len >= 3 && text[0] == '#') {
        int base = 0;
        size_t num_start = 2;
        switch (text[1]) {
        case 'x':
        case 'X':
            base = 16;
            break;
        case 'o':
        case 'O':
            base = 8;
            break;
        case 'b':
        case 'B':
            base = 2;
            break;
        case 'd':
        case 'D':
            base = 10;
            break;
        default:
            break;
        }
        if (base != 0) {
            char *end = NULL;
            unsigned long long uv = strtoull(text + num_start, &end, base);
            if (end && (size_t)(end - text) == len) {
                *out = ch_make_integer(r->gc, (int64_t)uv);
                return CH_READ_OK;
            }
        }
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
    return read_datum(r, out);
}
