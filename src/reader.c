#include "chaaya/reader.h"

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
    r->error[0] = '\0';
}

const char *ch_reader_error(const ChReader *r) {
    return r->error;
}

static int peek(ChReader *r) {
    if (r->pos >= r->len) {
        return -1;
    }
    return (unsigned char)r->src[r->pos];
}

static int advance(ChReader *r) {
    if (r->pos >= r->len) {
        return -1;
    }
    return (unsigned char)r->src[r->pos++];
}

static void skip_ws_and_comments(ChReader *r) {
    for (;;) {
        int c = peek(r);
        if (c < 0) {
            return;
        }
        if (c == ';') {
            while (peek(r) >= 0 && peek(r) != '\n') {
                advance(r);
            }
            continue;
        }
        if (isspace(c)) {
            advance(r);
            continue;
        }
        /* #!fold-case / #!no-fold-case directives */
        if (c == '#' && r->pos + 1 < r->len && r->src[r->pos + 1] == '!') {
            size_t start = r->pos;
            while (peek(r) >= 0 && peek(r) != '\n' && !isspace(peek(r))) {
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
        return;
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
    if (isalpha(c) || (unsigned char)c >= 0x80) {
        return true;
    }
    return strchr("!$%&*/:<=>?^_~+-.@", c) != NULL;
}

static ChReadStatus read_datum(ChReader *r, ChValue *out);

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

static bool try_parse_number(const char *text, size_t len, ChValue *out) {
    if (len == 0) {
        return false;
    }
    /* special: + - alone are symbols */
    if ((len == 1 && (text[0] == '+' || text[0] == '-')) ||
        (len == 1 && text[0] == '.')) {
        return false;
    }
    char *tmp = (char *)malloc(len + 1);
    if (!tmp) {
        return false;
    }
    memcpy(tmp, text, len);
    tmp[len] = '\0';
    char *end = NULL;
    /* try integer */
    long long iv = strtoll(tmp, &end, 10);
    if (end && *end == '\0') {
        *out = ch_make_fixnum((int64_t)iv);
        free(tmp);
        return true;
    }
    end = NULL;
    double dv = strtod(tmp, &end);
    if (end && *end == '\0') {
        *out = ch_make_flonum(dv);
        free(tmp);
        return true;
    }
    free(tmp);
    return false;
}

static ChReadStatus read_atom(ChReader *r, ChValue *out) {
    size_t start = r->pos;
    while (!is_delim(peek(r))) {
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
    ChValue num;
    if (try_parse_number(text, len, &num)) {
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
    skip_ws_and_comments(r);
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
        skip_ws_and_comments(r);
        if (peek(r) == ')') {
            break;
        }
        if (peek(r) == '.') {
            advance(r);
            if (!is_delim(peek(r))) {
                /* not a dotted pair — put back conceptually by reading as atom starting with . */
                r->pos--;
            } else {
                skip_ws_and_comments(r);
                ChValue rest = CH_NIL;
                ch_gc_push(r->gc, &rest);
                ChReadStatus st = read_datum(r, &rest);
                if (st != CH_READ_OK) {
                    ch_gc_pop_n(r->gc, 3);
                    return st;
                }
                skip_ws_and_comments(r);
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
        skip_ws_and_comments(r);
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
    skip_ws_and_comments(r);
    while (peek(r) >= 0 && peek(r) != ')') {
        if (n >= 256) {
            return fail(r, "vector too large");
        }
        ChReadStatus st = read_datum(r, &items[n]);
        if (st != CH_READ_OK) {
            return st;
        }
        n++;
        skip_ws_and_comments(r);
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
    skip_ws_and_comments(r);
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
    skip_ws_and_comments(r);
    if (peek(r) < 0) {
        return CH_READ_EOF;
    }
    return read_datum(r, out);
}
