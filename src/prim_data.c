#include "chaaya/prim.h"

#include "chaaya/bignum.h"
#include "chaaya/complex.h"
#include "chaaya/rational.h"
#include "chaaya/unicode.h"

#include <math.h>
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

static int parse_nonnegative_index(ChVM *vm, ChValue v, size_t *out, const char *who) {
    if (!ch_is_fixnum(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected exact integer", who);
        return -1;
    }
    int64_t n = ch_to_fixnum(v);
    if (n < 0) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected non-negative integer", who);
        return -1;
    }
    *out = (size_t)n;
    return 0;
}

static int parse_optional_range(ChVM *vm, ChValue *args, int nargs, int start_arg, size_t len,
                                const char *who, size_t *start_out, size_t *end_out) {
    size_t start = 0;
    size_t end = len;
    if (nargs > start_arg) {
        if (parse_nonnegative_index(vm, args[start_arg], &start, who) != 0) {
            return -1;
        }
    }
    if (nargs > start_arg + 1) {
        if (parse_nonnegative_index(vm, args[start_arg + 1], &end, who) != 0) {
            return -1;
        }
    }
    if (start > end || end > len) {
        snprintf(vm->error, sizeof(vm->error), "%s: range out of bounds", who);
        return -1;
    }
    *start_out = start;
    *end_out = end;
    return 0;
}

static bool utf8_decode_next(const char *bytes, size_t len, size_t pos, uint32_t *cp_out,
                             size_t *next_out) {
    if (pos >= len) {
        return false;
    }
    const uint8_t b0 = (uint8_t)bytes[pos];
    if (b0 < 0x80) {
        *cp_out = b0;
        *next_out = pos + 1;
        return true;
    }
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (pos + 1 >= len) {
            return false;
        }
        const uint8_t b1 = (uint8_t)bytes[pos + 1];
        if ((b1 & 0xC0) != 0x80) {
            return false;
        }
        *cp_out = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
        *next_out = pos + 2;
        return true;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (pos + 2 >= len) {
            return false;
        }
        const uint8_t b1 = (uint8_t)bytes[pos + 1];
        const uint8_t b2 = (uint8_t)bytes[pos + 2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
            return false;
        }
        if ((b0 == 0xE0 && b1 < 0xA0) || (b0 == 0xED && b1 >= 0xA0)) {
            return false; /* overlong or surrogate range */
        }
        *cp_out = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) |
                  (uint32_t)(b2 & 0x3F);
        *next_out = pos + 3;
        return true;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (pos + 3 >= len) {
            return false;
        }
        const uint8_t b1 = (uint8_t)bytes[pos + 1];
        const uint8_t b2 = (uint8_t)bytes[pos + 2];
        const uint8_t b3 = (uint8_t)bytes[pos + 3];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
            return false;
        }
        if ((b0 == 0xF0 && b1 < 0x90) || (b0 == 0xF4 && b1 >= 0x90)) {
            return false; /* overlong or > U+10FFFF */
        }
        *cp_out = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(b1 & 0x3F) << 12) |
                  ((uint32_t)(b2 & 0x3F) << 6) | (uint32_t)(b3 & 0x3F);
        *next_out = pos + 4;
        return true;
    }
    return false;
}

static bool utf8_encode_codepoint(uint32_t cp, char out[4], size_t *len_out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        *len_out = 1;
        return true;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        *len_out = 2;
        return true;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        return false;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        *len_out = 3;
        return true;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        *len_out = 4;
        return true;
    }
    return false;
}

static int utf8_count_codepoints(ChVM *vm, const ChString *s, const char *who, size_t *count_out) {
    size_t pos = 0;
    size_t count = 0;
    while (pos < s->len) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            snprintf(vm->error, sizeof(vm->error), "%s: invalid UTF-8 sequence", who);
            return -1;
        }
        (void)cp;
        pos = next;
        count++;
    }
    *count_out = count;
    return 0;
}

static int utf8_find_codepoint(ChVM *vm, const ChString *s, size_t index, const char *who,
                               size_t *start_out, size_t *end_out, uint32_t *cp_out) {
    size_t pos = 0;
    size_t i = 0;
    while (pos < s->len) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            snprintf(vm->error, sizeof(vm->error), "%s: invalid UTF-8 sequence", who);
            return -1;
        }
        if (i == index) {
            if (start_out) {
                *start_out = pos;
            }
            if (end_out) {
                *end_out = next;
            }
            if (cp_out) {
                *cp_out = cp;
            }
            return 0;
        }
        pos = next;
        i++;
    }
    snprintf(vm->error, sizeof(vm->error), "%s: index out of range", who);
    return -1;
}

static int utf8_offset_for_index(ChVM *vm, const ChString *s, size_t index, const char *who,
                                 size_t *offset_out) {
    size_t pos = 0;
    size_t i = 0;
    while (pos < s->len) {
        if (i == index) {
            *offset_out = pos;
            return 0;
        }
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            snprintf(vm->error, sizeof(vm->error), "%s: invalid UTF-8 sequence", who);
            return -1;
        }
        (void)cp;
        pos = next;
        i++;
    }
    if (i == index) {
        *offset_out = s->len;
        return 0;
    }
    snprintf(vm->error, sizeof(vm->error), "%s: index out of range", who);
    return -1;
}

static ChValue prim_values(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 1) {
        return args[0];
    }
    return ch_gc_make_values(&vm->gc, args, (size_t)nargs);
}

static ChValue prim_call_with_values(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue producer = args[0];
    ChValue consumer = args[1];
    if (!ch_is_procedure(producer) || !ch_is_procedure(consumer)) {
        snprintf(vm->error, sizeof(vm->error), "call-with-values: not a procedure");
        return CH_UNDEFINED;
    }
    /* Capture before nested applies (producer may call values) clear the flag. */
    bool was_tail = vm->native_was_tail;
    ChValue produced = CH_VOID;
    ChVMStatus st = ch_vm_apply(vm, producer, NULL, 0, &produced);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    /* Producer delivered values (possibly via continuation barrier landing). */
    vm->continuation_invoked = false;

    ChValue cargs[CH_VM_MAX_PENDING_ARGS];
    int cnargs = 0;
    if (ch_is_values(produced)) {
        ChValues *vs = ch_as_values(produced);
        if (vs->count > (size_t)CH_VM_MAX_PENDING_ARGS) {
            snprintf(vm->error, sizeof(vm->error), "call-with-values: too many values");
            return CH_UNDEFINED;
        }
        cnargs = (int)vs->count;
        for (int i = 0; i < cnargs; i++) {
            cargs[i] = vs->items[i];
        }
    } else {
        cnargs = 1;
        cargs[0] = produced;
    }

    /* Tail position: request a follow-up call so the consumer is not nested
     * under another run_until (proper TCO for loop-cwv / let-values). */
    if (was_tail) {
        vm->has_pending_call = true;
        vm->pending_call_tail = true;
        vm->pending_proc = consumer;
        vm->pending_nargs = cnargs;
        for (int i = 0; i < cnargs; i++) {
            vm->pending_args[i] = cargs[i];
        }
        return CH_UNDEFINED;
    }

    ChValue result = CH_VOID;
    st = ch_vm_apply(vm, consumer, cargs, cnargs, &result);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    return ch_coerce_single(result);
}

static ChValue prim_char_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_char(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_char_eq(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "char=?: needs at least 2 arguments");
        return CH_UNDEFINED;
    }
    for (int i = 0; i < nargs; i++) {
        if (!ch_is_char(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "char=?: not a char");
            return CH_UNDEFINED;
        }
    }
    uint32_t c0 = ch_to_char(args[0]);
    for (int i = 1; i < nargs; i++) {
        if (ch_to_char(args[i]) != c0) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static ChValue compare_chars(ChVM *vm, ChValue *args, int nargs, const char *who,
                             int (*cmp)(uint32_t, uint32_t)) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "%s: needs at least 2 arguments", who);
        return CH_UNDEFINED;
    }
    for (int i = 0; i < nargs; i++) {
        if (!ch_is_char(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "%s: not a char", who);
            return CH_UNDEFINED;
        }
    }
    for (int i = 1; i < nargs; i++) {
        if (!cmp(ch_to_char(args[i - 1]), ch_to_char(args[i]))) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static int ch_lt(uint32_t a, uint32_t b) { return a < b; }
static int ch_le(uint32_t a, uint32_t b) { return a <= b; }
static int ch_gt(uint32_t a, uint32_t b) { return a > b; }
static int ch_ge(uint32_t a, uint32_t b) { return a >= b; }

static ChValue prim_char_lt(ChVM *vm, ChValue *args, int nargs) {
    return compare_chars(vm, args, nargs, "char<?", ch_lt);
}
static ChValue prim_char_le(ChVM *vm, ChValue *args, int nargs) {
    return compare_chars(vm, args, nargs, "char<=?", ch_le);
}
static ChValue prim_char_gt(ChVM *vm, ChValue *args, int nargs) {
    return compare_chars(vm, args, nargs, "char>?", ch_gt);
}
static ChValue prim_char_ge(ChVM *vm, ChValue *args, int nargs) {
    return compare_chars(vm, args, nargs, "char>=?", ch_ge);
}

static ChValue prim_char_to_integer(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "char->integer: not a char");
        return CH_UNDEFINED;
    }
    return ch_make_fixnum((int64_t)ch_to_char(args[0]));
}

static int require_char(ChVM *vm, ChValue v, const char *who) {
    if (!ch_is_char(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: not a char", who);
        return -1;
    }
    return 0;
}

static ChValue compare_ci_chars(ChVM *vm, ChValue *args, int nargs, const char *who,
                                int (*cmp)(uint32_t, uint32_t)) {
    if (nargs < 2) {
        return CH_TRUE;
    }
    for (int i = 0; i < nargs; i++) {
        if (require_char(vm, args[i], who) != 0) {
            return CH_UNDEFINED;
        }
    }
    for (int i = 1; i < nargs; i++) {
        uint32_t a = ch_unicode_foldcase(ch_to_char(args[i - 1]));
        uint32_t b = ch_unicode_foldcase(ch_to_char(args[i]));
        if (!cmp(a, b)) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static int ci_lt(uint32_t a, uint32_t b) { return a < b; }
static int ci_le(uint32_t a, uint32_t b) { return a <= b; }
static int ci_eq(uint32_t a, uint32_t b) { return a == b; }
static int ci_ge(uint32_t a, uint32_t b) { return a >= b; }
static int ci_gt(uint32_t a, uint32_t b) { return a > b; }

static ChValue prim_char_ci_eq(ChVM *vm, ChValue *args, int nargs) {
    return compare_ci_chars(vm, args, nargs, "char-ci=?", ci_eq);
}
static ChValue prim_char_ci_lt(ChVM *vm, ChValue *args, int nargs) {
    return compare_ci_chars(vm, args, nargs, "char-ci<?", ci_lt);
}
static ChValue prim_char_ci_le(ChVM *vm, ChValue *args, int nargs) {
    return compare_ci_chars(vm, args, nargs, "char-ci<=?", ci_le);
}
static ChValue prim_char_ci_gt(ChVM *vm, ChValue *args, int nargs) {
    return compare_ci_chars(vm, args, nargs, "char-ci>?", ci_gt);
}
static ChValue prim_char_ci_ge(ChVM *vm, ChValue *args, int nargs) {
    return compare_ci_chars(vm, args, nargs, "char-ci>=?", ci_ge);
}

static ChValue prim_char_alphabetic_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "char-alphabetic?: not a char");
        return CH_UNDEFINED;
    }
    return ch_unicode_is_alphabetic(ch_to_char(args[0])) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_char_numeric_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "char-numeric?: not a char");
        return CH_UNDEFINED;
    }
    return ch_unicode_is_numeric(ch_to_char(args[0])) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_char_whitespace_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "char-whitespace?: not a char");
        return CH_UNDEFINED;
    }
    return ch_unicode_is_whitespace(ch_to_char(args[0])) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_char_upper_case_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "char-upper-case?: not a char");
        return CH_UNDEFINED;
    }
    return ch_unicode_is_uppercase(ch_to_char(args[0])) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_char_lower_case_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "char-lower-case?: not a char");
        return CH_UNDEFINED;
    }
    return ch_unicode_is_lowercase(ch_to_char(args[0])) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_char_upcase(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "char-upcase: not a char");
        return CH_UNDEFINED;
    }
    return ch_make_char(ch_unicode_upcase(ch_to_char(args[0])));
}

static ChValue prim_char_downcase(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "char-downcase: not a char");
        return CH_UNDEFINED;
    }
    return ch_make_char(ch_unicode_downcase(ch_to_char(args[0])));
}

static ChValue prim_char_foldcase(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "char-foldcase: not a char");
        return CH_UNDEFINED;
    }
    return ch_make_char(ch_unicode_foldcase(ch_to_char(args[0])));
}

static ChValue prim_digit_value(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "digit-value: not a char");
        return CH_UNDEFINED;
    }
    int dv = ch_unicode_digit_value(ch_to_char(args[0]));
    return dv < 0 ? CH_FALSE : ch_make_fixnum(dv);
}

static ChValue prim_integer_to_char(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_fixnum(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "integer->char: not an integer");
        return CH_UNDEFINED;
    }
    int64_t n = ch_to_fixnum(args[0]);
    if (n < 0 || n > 0x10FFFF) {
        snprintf(vm->error, sizeof(vm->error), "integer->char: out of range");
        return CH_UNDEFINED;
    }
    return ch_make_char((uint32_t)n);
}

static ChValue prim_abs(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact(args[0])) {
        return ch_exact_abs(&vm->gc, args[0]);
    }
    if (ch_is_flonum(args[0])) {
        double d = ch_to_flonum(args[0]);
        return ch_make_flonum(d < 0 ? -d : d);
    }
    snprintf(vm->error, sizeof(vm->error), "abs: not a number");
    return CH_UNDEFINED;
}

static ChValue prim_integer_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (ch_is_exact_integer(args[0])) {
        return CH_TRUE;
    }
    if (ch_is_flonum(args[0])) {
        double d = ch_to_flonum(args[0]);
        return (d == (double)(int64_t)d) ? CH_TRUE : CH_FALSE;
    }
    return CH_FALSE;
}

static ChValue prim_zero_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact_integer(args[0])) {
        return ch_bignum_compare(args[0], ch_make_fixnum(0)) == 0 ? CH_TRUE : CH_FALSE;
    }
    if (ch_is_rational_obj(args[0])) {
        return ch_bignum_compare(ch_as_rational(args[0])->numerator, ch_make_fixnum(0)) == 0
                   ? CH_TRUE
                   : CH_FALSE;
    }
    if (ch_is_flonum(args[0])) {
        return ch_to_flonum(args[0]) == 0.0 ? CH_TRUE : CH_FALSE;
    }
    if (ch_is_complex_obj(args[0])) {
        ChComplex *c = ch_as_complex(args[0]);
        return (c->real == 0.0 && c->imag == 0.0) ? CH_TRUE : CH_FALSE;
    }
    snprintf(vm->error, sizeof(vm->error), "zero?: not a number");
    return CH_UNDEFINED;
}

static ChValue prim_string_length(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-length: not a string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t count = 0;
    if (utf8_count_codepoints(vm, s, "string-length", &count) != 0) {
        return CH_UNDEFINED;
    }
    return ch_make_fixnum((int64_t)count);
}

static ChValue prim_string_ref(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-ref: bad arguments");
        return CH_UNDEFINED;
    }
    size_t index = 0;
    if (parse_nonnegative_index(vm, args[1], &index, "string-ref") != 0) {
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    uint32_t cp = 0;
    if (utf8_find_codepoint(vm, s, index, "string-ref", NULL, NULL, &cp) != 0) {
        return CH_UNDEFINED;
    }
    return ch_make_char(cp);
}

static int string_splice_bytes(ChString *s, size_t byte_start, size_t byte_end, const char *rep,
                               size_t rep_len) {
    size_t old_len = byte_end - byte_start;
    if (rep_len == old_len) {
        if (rep_len > 0) {
            memmove(s->data + byte_start, rep, rep_len);
        }
        return 0;
    }
    size_t new_total = s->len - old_len + rep_len;
    char *nb = (char *)malloc(new_total + 1);
    if (!nb) {
        return -1;
    }
    memcpy(nb, s->data, byte_start);
    if (rep_len > 0) {
        memcpy(nb + byte_start, rep, rep_len);
    }
    memcpy(nb + byte_start + rep_len, s->data + byte_end, s->len - byte_end);
    nb[new_total] = '\0';
    free(s->data);
    s->data = nb;
    s->len = new_total;
    return 0;
}

static ChValue prim_string_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0]) || !ch_is_char(args[2])) {
        snprintf(vm->error, sizeof(vm->error), "string-set!: bad arguments");
        return CH_UNDEFINED;
    }
    size_t index = 0;
    if (parse_nonnegative_index(vm, args[1], &index, "string-set!") != 0) {
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t start = 0;
    size_t end = 0;
    if (utf8_find_codepoint(vm, s, index, "string-set!", &start, &end, NULL) != 0) {
        return CH_UNDEFINED;
    }
    char encoded[4];
    size_t encoded_len = 0;
    if (!utf8_encode_codepoint(ch_to_char(args[2]), encoded, &encoded_len)) {
        snprintf(vm->error, sizeof(vm->error), "string-set!: invalid character");
        return CH_UNDEFINED;
    }
    if (string_splice_bytes(s, start, end, encoded, encoded_len) != 0) {
        abort();
    }
    return CH_VOID;
}

static ChValue prim_string(ChVM *vm, ChValue *args, int nargs) {
    size_t total = 0;
    for (int i = 0; i < nargs; i++) {
        if (!ch_is_char(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "string: not a char");
            return CH_UNDEFINED;
        }
        char encoded[4];
        size_t encoded_len = 0;
        if (!utf8_encode_codepoint(ch_to_char(args[i]), encoded, &encoded_len)) {
            snprintf(vm->error, sizeof(vm->error), "string: invalid character");
            return CH_UNDEFINED;
        }
        if (total > SIZE_MAX - encoded_len) {
            snprintf(vm->error, sizeof(vm->error), "string: output too large");
            return CH_UNDEFINED;
        }
        total += encoded_len;
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        abort();
    }
    size_t pos = 0;
    for (int i = 0; i < nargs; i++) {
        char encoded[4];
        size_t encoded_len = 0;
        (void)utf8_encode_codepoint(ch_to_char(args[i]), encoded, &encoded_len);
        memcpy(buf + pos, encoded, encoded_len);
        pos += encoded_len;
    }
    buf[total] = '\0';
    ChValue out = ch_gc_make_string(&vm->gc, buf, total);
    free(buf);
    return out;
}

static ChValue prim_make_string(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_fixnum(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "make-string: bad length");
        return CH_UNDEFINED;
    }
    int64_t n = ch_to_fixnum(args[0]);
    if (n < 0 || n > 1000000) {
        snprintf(vm->error, sizeof(vm->error), "make-string: bad length");
        return CH_UNDEFINED;
    }
    char fill_bytes[4] = {' '};
    size_t fill_len = 1;
    if (nargs >= 2) {
        if (!ch_is_char(args[1])) {
            snprintf(vm->error, sizeof(vm->error), "make-string: fill not a char");
            return CH_UNDEFINED;
        }
        if (!utf8_encode_codepoint(ch_to_char(args[1]), fill_bytes, &fill_len)) {
            snprintf(vm->error, sizeof(vm->error), "make-string: fill not a valid character");
            return CH_UNDEFINED;
        }
    }
    size_t count = (size_t)n;
    if (fill_len > 0 && count > (SIZE_MAX - 1) / fill_len) {
        snprintf(vm->error, sizeof(vm->error), "make-string: length too large");
        return CH_UNDEFINED;
    }
    size_t total = count * fill_len;
    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        abort();
    }
    for (size_t i = 0; i < count; i++) {
        memcpy(buf + (i * fill_len), fill_bytes, fill_len);
    }
    buf[total] = '\0';
    ChValue s = ch_gc_make_string(&vm->gc, buf, total);
    free(buf);
    return s;
}

static ChValue prim_string_append(ChVM *vm, ChValue *args, int nargs) {
    size_t total = 0;
    for (int i = 0; i < nargs; i++) {
        if (!ch_is_string(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "string-append: not a string");
            return CH_UNDEFINED;
        }
        total += ch_as_string(args[i])->len;
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        abort();
    }
    size_t pos = 0;
    for (int i = 0; i < nargs; i++) {
        ChString *s = ch_as_string(args[i]);
        memcpy(buf + pos, s->data, s->len);
        pos += s->len;
    }
    buf[total] = '\0';
    ChValue out = ch_gc_make_string(&vm->gc, buf, total);
    free(buf);
    return out;
}

static ChValue prim_substring(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "substring: bad arguments");
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = 0;
    if (parse_nonnegative_index(vm, args[1], &start, "substring") != 0 ||
        parse_nonnegative_index(vm, args[2], &end, "substring") != 0) {
        return CH_UNDEFINED;
    }
    if (end < start) {
        snprintf(vm->error, sizeof(vm->error), "substring: out of range");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t start_byte = 0;
    size_t end_byte = 0;
    if (utf8_offset_for_index(vm, s, start, "substring", &start_byte) != 0 ||
        utf8_offset_for_index(vm, s, end, "substring", &end_byte) != 0) {
        return CH_UNDEFINED;
    }
    return ch_gc_make_string(&vm->gc, s->data + start_byte, end_byte - start_byte);
}

static bool append_codepoint(char **buf, size_t *len, size_t *cap, uint32_t cp);

static ChValue prim_string_map(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "string-map: expected procedure and at least one string");
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-map: not a procedure");
        return CH_UNDEFINED;
    }
    int string_count = nargs - 1;
    if (string_count > 64) {
        snprintf(vm->error, sizeof(vm->error), "string-map: too many string arguments");
        return CH_UNDEFINED;
    }
    ChString *strings[64];
    size_t len = 0;
    for (int i = 0; i < string_count; i++) {
        if (!ch_is_string(args[i + 1])) {
            snprintf(vm->error, sizeof(vm->error), "string-map: not a string");
            return CH_UNDEFINED;
        }
        strings[i] = ch_as_string(args[i + 1]);
        size_t count = 0;
        if (utf8_count_codepoints(vm, strings[i], "string-map", &count) != 0) {
            return CH_UNDEFINED;
        }
        if (i == 0 || count < len) {
            len = count;
        }
    }
    char *buf = NULL;
    size_t out_len = 0;
    size_t out_cap = 0;
    ChValue call_args[64];
    for (size_t i = 0; i < len; i++) {
        for (int j = 0; j < string_count; j++) {
            uint32_t cp = 0;
            if (utf8_find_codepoint(vm, strings[j], i, "string-map", NULL, NULL, &cp) != 0) {
                free(buf);
                return CH_UNDEFINED;
            }
            call_args[j] = ch_make_char(cp);
        }
        ChValue mapped = CH_UNDEFINED;
        ChVMStatus st = ch_vm_apply(vm, args[0], call_args, string_count, &mapped);
        if (st == CH_VM_CONTINUATION_INVOKED) {
            free(buf);
            vm->continuation_invoked = true;
            return CH_UNDEFINED;
        }
        if (st != CH_VM_OK) {
            free(buf);
            return CH_UNDEFINED;
        }
        mapped = ch_coerce_single(mapped);
        if (!ch_is_char(mapped)) {
            free(buf);
            snprintf(vm->error, sizeof(vm->error), "string-map: procedure must return a character");
            return CH_UNDEFINED;
        }
        if (!append_codepoint(&buf, &out_len, &out_cap, ch_to_char(mapped))) {
            free(buf);
            abort();
        }
    }
    if (buf) {
        buf[out_len] = '\0';
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", out_len);
    free(buf);
    return out;
}

static int string_cmp_utf8(const ChString *a, const ChString *b) {
    size_t ap = 0;
    size_t bp = 0;
    while (ap < a->len && bp < b->len) {
        uint32_t ac = 0;
        uint32_t bc = 0;
        size_t an = 0;
        size_t bn = 0;
        if (!utf8_decode_next(a->data, a->len, ap, &ac, &an) ||
            !utf8_decode_next(b->data, b->len, bp, &bc, &bn)) {
            break;
        }
        if (ac < bc) {
            return -1;
        }
        if (ac > bc) {
            return 1;
        }
        ap = an;
        bp = bn;
    }
    if (ap < a->len) {
        return 1;
    }
    if (bp < b->len) {
        return -1;
    }
    return 0;
}

static ChValue compare_strings(ChVM *vm, ChValue *args, int nargs, const char *who, int mode) {
    if (nargs < 2) {
        return CH_TRUE;
    }
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "%s: not a string", who);
        return CH_UNDEFINED;
    }
    ChString *s0 = ch_as_string(args[0]);
    for (int i = 1; i < nargs; i++) {
        if (!ch_is_string(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "%s: not a string", who);
            return CH_UNDEFINED;
        }
        ChString *si = ch_as_string(args[i]);
        int cmp = string_cmp_utf8(s0, si);
        int pass = 0;
        switch (mode) {
        case 0:
            pass = cmp < 0;
            break;
        case 1:
            pass = cmp <= 0;
            break;
        case 2:
            pass = cmp == 0;
            break;
        case 3:
            pass = cmp >= 0;
            break;
        case 4:
            pass = cmp > 0;
            break;
        }
        if (!pass) {
            return CH_FALSE;
        }
        s0 = si;
    }
    return CH_TRUE;
}

static ChValue prim_string_eq(ChVM *vm, ChValue *args, int nargs) {
    return compare_strings(vm, args, nargs, "string=?", 2);
}
static ChValue prim_string_lt(ChVM *vm, ChValue *args, int nargs) {
    return compare_strings(vm, args, nargs, "string<?", 0);
}
static ChValue prim_string_le(ChVM *vm, ChValue *args, int nargs) {
    return compare_strings(vm, args, nargs, "string<=?", 1);
}
static ChValue prim_string_gt(ChVM *vm, ChValue *args, int nargs) {
    return compare_strings(vm, args, nargs, "string>?", 4);
}
static ChValue prim_string_ge(ChVM *vm, ChValue *args, int nargs) {
    return compare_strings(vm, args, nargs, "string>=?", 3);
}

static ChValue compare_ci_strings(ChVM *vm, ChValue *args, int nargs, const char *who, int mode) {
    if (nargs < 2) {
        return CH_TRUE;
    }
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "%s: not a string", who);
        return CH_UNDEFINED;
    }
    ChString *s0 = ch_as_string(args[0]);
    for (int i = 1; i < nargs; i++) {
        if (!ch_is_string(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "%s: not a string", who);
            return CH_UNDEFINED;
        }
        ChString *si = ch_as_string(args[i]);
        int cmp = ch_unicode_fold_compare_strings(s0->data, s0->len, si->data, si->len);
        int pass = 0;
        switch (mode) {
        case 0:
            pass = cmp < 0;
            break;
        case 1:
            pass = cmp <= 0;
            break;
        case 2:
            pass = cmp == 0;
            break;
        case 3:
            pass = cmp >= 0;
            break;
        case 4:
            pass = cmp > 0;
            break;
        }
        if (!pass) {
            return CH_FALSE;
        }
        s0 = si;
    }
    return CH_TRUE;
}

static ChValue prim_string_ci_eq(ChVM *vm, ChValue *args, int nargs) {
    return compare_ci_strings(vm, args, nargs, "string-ci=?", 2);
}
static ChValue prim_string_ci_lt(ChVM *vm, ChValue *args, int nargs) {
    return compare_ci_strings(vm, args, nargs, "string-ci<?", 0);
}
static ChValue prim_string_ci_le(ChVM *vm, ChValue *args, int nargs) {
    return compare_ci_strings(vm, args, nargs, "string-ci<=?", 1);
}
static ChValue prim_string_ci_gt(ChVM *vm, ChValue *args, int nargs) {
    return compare_ci_strings(vm, args, nargs, "string-ci>?", 4);
}
static ChValue prim_string_ci_ge(ChVM *vm, ChValue *args, int nargs) {
    return compare_ci_strings(vm, args, nargs, "string-ci>=?", 3);
}

static bool append_codepoint(char **buf, size_t *len, size_t *cap, uint32_t cp) {
    char encoded[4];
    size_t encoded_len = 0;
    if (cp <= 0x7Fu) {
        encoded[0] = (char)cp;
        encoded_len = 1;
    } else if (cp <= 0x7FFu) {
        encoded[0] = (char)(0xC0u | (cp >> 6));
        encoded[1] = (char)(0x80u | (cp & 0x3Fu));
        encoded_len = 2;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
        return false;
    } else if (cp <= 0xFFFFu) {
        encoded[0] = (char)(0xE0u | (cp >> 12));
        encoded[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[2] = (char)(0x80u | (cp & 0x3Fu));
        encoded_len = 3;
    } else if (cp <= 0x10FFFFu) {
        encoded[0] = (char)(0xF0u | (cp >> 18));
        encoded[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        encoded[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[3] = (char)(0x80u | (cp & 0x3Fu));
        encoded_len = 4;
    } else {
        return false;
    }
    if (*len + encoded_len > *cap) {
        size_t ncap = *cap ? *cap : 64;
        while (ncap < *len + encoded_len) {
            ncap *= 2;
        }
        char *nb = (char *)realloc(*buf, ncap + 1);
        if (!nb) {
            abort();
        }
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, encoded, encoded_len);
    *len += encoded_len;
    return true;
}

typedef enum { CH_CASE_UP, CH_CASE_DOWN, CH_CASE_FOLD } ChCaseMode;

static bool append_case_codepoint(char **buf, size_t *len, size_t *cap, uint32_t cp, ChCaseMode mode) {
    if (mode == CH_CASE_FOLD) {
        ChUnicodeFoldExpansion exp = ch_unicode_fold_expand(cp);
        for (size_t i = 0; i < exp.len; i++) {
            if (!append_codepoint(buf, len, cap, exp.cps[i])) {
                return false;
            }
        }
        return true;
    }
    if (mode == CH_CASE_UP) {
        switch (cp) {
        case 0x00DF:
            return append_codepoint(buf, len, cap, 'S') && append_codepoint(buf, len, cap, 'S');
        case 0x01F0:
            return append_codepoint(buf, len, cap, 'J') && append_codepoint(buf, len, cap, 0x030C);
        case 0x0390:
            return append_codepoint(buf, len, cap, 0x0399) &&
                   append_codepoint(buf, len, cap, 0x0308) &&
                   append_codepoint(buf, len, cap, 0x0301);
        case 0x03B0:
            return append_codepoint(buf, len, cap, 0x03A5) &&
                   append_codepoint(buf, len, cap, 0x0308) &&
                   append_codepoint(buf, len, cap, 0x0301);
        case 0xFB00:
            return append_codepoint(buf, len, cap, 'F') && append_codepoint(buf, len, cap, 'F');
        case 0xFB01:
            return append_codepoint(buf, len, cap, 'F') && append_codepoint(buf, len, cap, 'I');
        case 0xFB02:
            return append_codepoint(buf, len, cap, 'F') && append_codepoint(buf, len, cap, 'L');
        case 0xFB03:
            return append_codepoint(buf, len, cap, 'F') && append_codepoint(buf, len, cap, 'F') &&
                   append_codepoint(buf, len, cap, 'I');
        case 0xFB04:
            return append_codepoint(buf, len, cap, 'F') && append_codepoint(buf, len, cap, 'F') &&
                   append_codepoint(buf, len, cap, 'L');
        default:
            return append_codepoint(buf, len, cap, ch_unicode_upcase(cp));
        }
    }
    if (cp == 0x0130) {
        return append_codepoint(buf, len, cap, 0x0069) && append_codepoint(buf, len, cap, 0x0307);
    }
    return append_codepoint(buf, len, cap, ch_unicode_downcase(cp));
}

static ChValue string_case_map(ChVM *vm, ChValue arg, const char *who, ChCaseMode mode) {
    if (!ch_is_string(arg)) {
        snprintf(vm->error, sizeof(vm->error), "%s: not a string", who);
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(arg);
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    size_t pos = 0;
    bool prev_cased = false;
    while (pos < s->len) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            free(buf);
            snprintf(vm->error, sizeof(vm->error), "%s: invalid UTF-8 sequence", who);
            return CH_UNDEFINED;
        }
        if (mode == CH_CASE_DOWN && cp == 0x03A3) {
            uint32_t next_cp = 0;
            if (next < s->len) {
                size_t nn = 0;
                (void)utf8_decode_next(s->data, s->len, next, &next_cp, &nn);
            }
            bool next_is_cased = ch_unicode_is_cased(next_cp);
            if (prev_cased && !next_is_cased) {
                if (!append_codepoint(&buf, &len, &cap, 0x03C2)) {
                    free(buf);
                    abort();
                }
            } else if (!append_codepoint(&buf, &len, &cap, 0x03C3)) {
                free(buf);
                abort();
            }
        } else if (!append_case_codepoint(&buf, &len, &cap, cp, mode)) {
            free(buf);
            abort();
        }
        prev_cased = ch_unicode_is_cased(cp);
        pos = next;
    }
    if (buf) {
        buf[len] = '\0';
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", len);
    free(buf);
    return out;
}

static ChValue prim_string_upcase(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return string_case_map(vm, args[0], "string-upcase", CH_CASE_UP);
}

static ChValue prim_string_downcase(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return string_case_map(vm, args[0], "string-downcase", CH_CASE_DOWN);
}

static ChValue prim_string_foldcase(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return string_case_map(vm, args[0], "string-foldcase", CH_CASE_FOLD);
}

static ChValue prim_string_copy(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-copy: expected string [start [end]]");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t count = 0;
    if (utf8_count_codepoints(vm, s, "string-copy", &count) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = count;
    if (parse_optional_range(vm, args, nargs, 1, count, "string-copy", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t start_byte = 0;
    size_t end_byte = 0;
    if (utf8_offset_for_index(vm, s, start, "string-copy", &start_byte) != 0 ||
        utf8_offset_for_index(vm, s, end, "string-copy", &end_byte) != 0) {
        return CH_UNDEFINED;
    }
    return ch_gc_make_string(&vm->gc, s->data + start_byte, end_byte - start_byte);
}

static ChValue prim_string_copy_bang(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 3 || !ch_is_string(args[0]) || !ch_is_string(args[2])) {
        snprintf(vm->error, sizeof(vm->error), "string-copy!: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *to = ch_as_string(args[0]);
    if (ch_object_is_immutable(&to->header)) {
        snprintf(vm->error, sizeof(vm->error), "string-copy!: immutable string");
        return CH_UNDEFINED;
    }
    size_t at = 0;
    if (parse_nonnegative_index(vm, args[1], &at, "string-copy!") != 0) {
        return CH_UNDEFINED;
    }
    ChString *from = ch_as_string(args[2]);
    size_t from_count = 0;
    if (utf8_count_codepoints(vm, from, "string-copy!", &from_count) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = from_count;
    if (parse_optional_range(vm, args, nargs, 3, from_count, "string-copy!", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t to_count = 0;
    if (utf8_count_codepoints(vm, to, "string-copy!", &to_count) != 0) {
        return CH_UNDEFINED;
    }
    size_t copy_cps = end - start;
    if (at + copy_cps > to_count) {
        snprintf(vm->error, sizeof(vm->error), "string-copy!: range out of bounds");
        return CH_UNDEFINED;
    }
    size_t from_byte_start = 0;
    size_t from_byte_end = 0;
    size_t to_byte_start = 0;
    size_t to_byte_end = 0;
    if (utf8_offset_for_index(vm, from, start, "string-copy!", &from_byte_start) != 0 ||
        utf8_offset_for_index(vm, from, end, "string-copy!", &from_byte_end) != 0 ||
        utf8_offset_for_index(vm, to, at, "string-copy!", &to_byte_start) != 0 ||
        utf8_offset_for_index(vm, to, at + copy_cps, "string-copy!", &to_byte_end) != 0) {
        return CH_UNDEFINED;
    }
    /* Copy source bytes first when src/dst may overlap after a resize. */
    size_t src_len = from_byte_end - from_byte_start;
    char *src_copy = (char *)malloc(src_len == 0 ? 1 : src_len);
    if (!src_copy) {
        abort();
    }
    if (src_len > 0) {
        memcpy(src_copy, from->data + from_byte_start, src_len);
    }
    if (string_splice_bytes(to, to_byte_start, to_byte_end, src_copy, src_len) != 0) {
        free(src_copy);
        abort();
    }
    free(src_copy);
    return CH_VOID;
}

static ChValue prim_string_for_each(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "string-for-each: expected procedure and at least one string");
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-for-each: not a procedure");
        return CH_UNDEFINED;
    }
    int string_count = nargs - 1;
    if (string_count > 64) {
        snprintf(vm->error, sizeof(vm->error), "string-for-each: too many string arguments");
        return CH_UNDEFINED;
    }
    ChString *strings[64];
    size_t len = 0;
    for (int i = 0; i < string_count; i++) {
        if (!ch_is_string(args[i + 1])) {
            snprintf(vm->error, sizeof(vm->error), "string-for-each: not a string");
            return CH_UNDEFINED;
        }
        strings[i] = ch_as_string(args[i + 1]);
        size_t count = 0;
        if (utf8_count_codepoints(vm, strings[i], "string-for-each", &count) != 0) {
            return CH_UNDEFINED;
        }
        if (i == 0 || count < len) {
            len = count;
        }
    }
    ChValue call_args[64];
    for (size_t i = 0; i < len; i++) {
        for (int j = 0; j < string_count; j++) {
            uint32_t cp = 0;
            if (utf8_find_codepoint(vm, strings[j], i, "string-for-each", NULL, NULL, &cp) != 0) {
                return CH_UNDEFINED;
            }
            call_args[j] = ch_make_char(cp);
        }
        ChValue r = CH_VOID;
        ChVMStatus st = ch_vm_apply(vm, args[0], call_args, string_count, &r);
        if (st == CH_VM_CONTINUATION_INVOKED) {
            vm->continuation_invoked = true;
            return CH_UNDEFINED;
        }
        if (st != CH_VM_OK) {
            return CH_UNDEFINED;
        }
    }
    return CH_VOID;
}

static ChValue prim_symbol_to_string(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_symbol(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "symbol->string: not a symbol");
        return CH_UNDEFINED;
    }
    ChSymbol *s = ch_as_symbol(args[0]);
    return ch_gc_make_string(&vm->gc, s->name, s->len);
}

static ChValue prim_string_to_symbol(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string->symbol: not a string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    return ch_gc_intern_symbol(&vm->gc, s->data, s->len);
}

static ChValue prim_vector_length(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_vector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-length: not a vector");
        return CH_UNDEFINED;
    }
    return ch_make_fixnum((int64_t)ch_as_vector(args[0])->len);
}

static ChValue prim_vector_ref(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_vector(args[0]) || !ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "vector-ref: bad arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = ch_as_vector(args[0]);
    int64_t i = ch_to_fixnum(args[1]);
    if (i < 0 || (size_t)i >= v->len) {
        snprintf(vm->error, sizeof(vm->error), "vector-ref: index out of range");
        return CH_UNDEFINED;
    }
    return v->items[i];
}

static ChValue prim_vector_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_vector(args[0]) || !ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "vector-set!: bad arguments");
        return CH_UNDEFINED;
    }
    ChVector *v = ch_as_vector(args[0]);
    if (ch_object_is_immutable(&v->header)) {
        snprintf(vm->error, sizeof(vm->error), "vector-set!: immutable vector");
        return CH_UNDEFINED;
    }
    int64_t i = ch_to_fixnum(args[1]);
    if (i < 0 || (size_t)i >= v->len) {
        snprintf(vm->error, sizeof(vm->error), "vector-set!: index out of range");
        return CH_UNDEFINED;
    }
    v->items[i] = args[2];
    ch_gc_write_barrier(&vm->gc, &v->header, args[2]);
    return CH_VOID;
}

static ChValue prim_vector_fill(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || nargs > 4 || !ch_is_vector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-fill!: expected vector fill [start [end]]");
        return CH_UNDEFINED;
    }
    ChVector *v = ch_as_vector(args[0]);
    if (ch_object_is_immutable(&v->header)) {
        snprintf(vm->error, sizeof(vm->error), "vector-fill!: immutable vector");
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = v->len;
    if (parse_optional_range(vm, args, nargs, 2, v->len, "vector-fill!", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    for (size_t i = start; i < end; i++) {
        v->items[i] = args[1];
        ch_gc_write_barrier(&vm->gc, &v->header, args[1]);
    }
    return CH_VOID;
}

static ChValue prim_vector_copy(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || nargs > 3 || !ch_is_vector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-copy: expected vector [start [end]]");
        return CH_UNDEFINED;
    }
    ChVector *src = ch_as_vector(args[0]);
    size_t start = 0;
    size_t end = src->len;
    if (parse_optional_range(vm, args, nargs, 1, src->len, "vector-copy", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t count = end - start;
    ChValue out = ch_gc_make_vector(&vm->gc, count, CH_FALSE);
    ChVector *dst = ch_as_vector(out);
    for (size_t i = 0; i < count; i++) {
        dst->items[i] = src->items[start + i];
    }
    return out;
}

static ChValue prim_vector_append(ChVM *vm, ChValue *args, int nargs) {
    size_t total = 0;
    for (int i = 0; i < nargs; i++) {
        if (!ch_is_vector(args[i])) {
            snprintf(vm->error, sizeof(vm->error), "vector-append: not a vector");
            return CH_UNDEFINED;
        }
        total += ch_as_vector(args[i])->len;
    }
    ChValue out = ch_gc_make_vector(&vm->gc, total, CH_FALSE);
    ChVector *dst = ch_as_vector(out);
    size_t pos = 0;
    for (int i = 0; i < nargs; i++) {
        ChVector *src = ch_as_vector(args[i]);
        for (size_t j = 0; j < src->len; j++) {
            dst->items[pos++] = src->items[j];
        }
    }
    return out;
}

static ChValue prim_vector_map(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-map: expected procedure and at least one vector");
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-map: not a procedure");
        return CH_UNDEFINED;
    }
    int vec_count = nargs - 1;
    if (vec_count > 64) {
        snprintf(vm->error, sizeof(vm->error), "vector-map: too many vector arguments");
        return CH_UNDEFINED;
    }
    ChVector *vecs[64];
    size_t len = 0;
    for (int i = 0; i < vec_count; i++) {
        if (!ch_is_vector(args[i + 1])) {
            snprintf(vm->error, sizeof(vm->error), "vector-map: not a vector");
            return CH_UNDEFINED;
        }
        vecs[i] = ch_as_vector(args[i + 1]);
        if (i == 0 || vecs[i]->len < len) {
            len = vecs[i]->len;
        }
    }
    ChValue out = ch_gc_make_vector(&vm->gc, len, CH_FALSE);
    ch_gc_push(&vm->gc, &out);
    ChVector *dst = ch_as_vector(out);
    ChValue call_args[64];
    for (size_t i = 0; i < len; i++) {
        for (int j = 0; j < vec_count; j++) {
            call_args[j] = vecs[j]->items[i];
        }
        ChValue mapped = CH_UNDEFINED;
        ChVMStatus st = ch_vm_apply(vm, args[0], call_args, vec_count, &mapped);
        if (st == CH_VM_CONTINUATION_INVOKED) {
            vm->continuation_invoked = true;
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        if (st != CH_VM_OK) {
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        dst->items[i] = ch_coerce_single(mapped);
    }
    ch_gc_pop(&vm->gc);
    return out;
}

static ChValue prim_vector_for_each(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        snprintf(vm->error, sizeof(vm->error), "vector-for-each: expected procedure and at least one vector");
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector-for-each: not a procedure");
        return CH_UNDEFINED;
    }
    int vec_count = nargs - 1;
    if (vec_count > 64) {
        snprintf(vm->error, sizeof(vm->error), "vector-for-each: too many vector arguments");
        return CH_UNDEFINED;
    }
    ChVector *vecs[64];
    size_t len = 0;
    for (int i = 0; i < vec_count; i++) {
        if (!ch_is_vector(args[i + 1])) {
            snprintf(vm->error, sizeof(vm->error), "vector-for-each: not a vector");
            return CH_UNDEFINED;
        }
        vecs[i] = ch_as_vector(args[i + 1]);
        if (i == 0 || vecs[i]->len < len) {
            len = vecs[i]->len;
        }
    }
    ChValue call_args[64];
    for (size_t i = 0; i < len; i++) {
        for (int j = 0; j < vec_count; j++) {
            call_args[j] = vecs[j]->items[i];
        }
        ChValue r = CH_VOID;
        ChVMStatus st = ch_vm_apply(vm, args[0], call_args, vec_count, &r);
        if (st == CH_VM_CONTINUATION_INVOKED) {
            vm->continuation_invoked = true;
            return CH_UNDEFINED;
        }
        if (st != CH_VM_OK) {
            return CH_UNDEFINED;
        }
    }
    return CH_VOID;
}

static ChValue prim_make_vector(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_fixnum(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "make-vector: bad length");
        return CH_UNDEFINED;
    }
    int64_t n = ch_to_fixnum(args[0]);
    if (n < 0 || n > 1000000) {
        snprintf(vm->error, sizeof(vm->error), "make-vector: bad length");
        return CH_UNDEFINED;
    }
    ChValue fill = nargs >= 2 ? args[1] : CH_FALSE;
    return ch_gc_make_vector(&vm->gc, (size_t)n, fill);
}

static ChValue prim_vector_to_list(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || nargs > 3) {
        snprintf(vm->error, sizeof(vm->error), "vector->list: expected 1 to 3 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_vector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector->list: not a vector");
        return CH_UNDEFINED;
    }
    ChVector *v = ch_as_vector(args[0]);
    size_t start = 0;
    size_t end = v->len;
    if (parse_optional_range(vm, args, nargs, 1, v->len, "vector->list", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (size_t i = end; i > start; i--) {
        ChValue item = v->items[i - 1];
        ch_gc_push(&vm->gc, &item);
        list = ch_gc_cons(&vm->gc, item, list);
        ch_gc_pop(&vm->gc);
    }
    ch_gc_pop(&vm->gc);
    return list;
}

static ChValue prim_list_to_vector(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue lst = args[0];
    size_t n = 0;
    for (ChValue p = lst; ch_is_pair(p); p = ch_cdr(p)) {
        n++;
    }
    ChValue vec = ch_gc_make_vector(&vm->gc, n, CH_FALSE);
    ChVector *v = ch_as_vector(vec);
    size_t i = 0;
    for (ChValue p = lst; ch_is_pair(p); p = ch_cdr(p)) {
        v->items[i++] = ch_car(p);
    }
    return vec;
}

static ChValue prim_vector(ChVM *vm, ChValue *args, int nargs) {
    ChValue vec = ch_gc_make_vector(&vm->gc, (size_t)nargs, CH_FALSE);
    ChVector *v = ch_as_vector(vec);
    for (int i = 0; i < nargs; i++) {
        v->items[i] = args[i];
        ch_gc_write_barrier(&vm->gc, &v->header, args[i]);
    }
    return vec;
}

static ChValue prim_vector_copy_bang(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 3 || !ch_is_vector(args[0]) || !ch_is_vector(args[2])) {
        snprintf(vm->error, sizeof(vm->error), "vector-copy!: expected to at from [start [end]]");
        return CH_UNDEFINED;
    }
    ChVector *to = ch_as_vector(args[0]);
    if (ch_object_is_immutable(&to->header)) {
        snprintf(vm->error, sizeof(vm->error), "vector-copy!: immutable vector");
        return CH_UNDEFINED;
    }
    size_t at = 0;
    if (parse_nonnegative_index(vm, args[1], &at, "vector-copy!") != 0) {
        return CH_UNDEFINED;
    }
    ChVector *from = ch_as_vector(args[2]);
    size_t start = 0;
    size_t end = from->len;
    if (parse_optional_range(vm, args, nargs, 3, from->len, "vector-copy!", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t count = end - start;
    if (at + count > to->len) {
        snprintf(vm->error, sizeof(vm->error), "vector-copy!: range out of bounds");
        return CH_UNDEFINED;
    }
    if (to == from && at > start) {
        for (size_t i = count; i > 0; i--) {
            to->items[at + i - 1] = from->items[start + i - 1];
            ch_gc_write_barrier(&vm->gc, &to->header, to->items[at + i - 1]);
        }
    } else {
        for (size_t i = 0; i < count; i++) {
            to->items[at + i] = from->items[start + i];
            ch_gc_write_barrier(&vm->gc, &to->header, to->items[at + i]);
        }
    }
    return CH_VOID;
}

static ChValue prim_string_to_vector(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string->vector: not a string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t count = 0;
    if (utf8_count_codepoints(vm, s, "string->vector", &count) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = count;
    if (parse_optional_range(vm, args, nargs, 1, count, "string->vector", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t n = end - start;
    ChValue vec = ch_gc_make_vector(&vm->gc, n, CH_FALSE);
    ChVector *v = ch_as_vector(vec);
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = 0;
        if (utf8_find_codepoint(vm, s, start + i, "string->vector", NULL, NULL, &cp) != 0) {
            return CH_UNDEFINED;
        }
        v->items[i] = ch_make_char(cp);
    }
    return vec;
}

static ChValue prim_vector_to_string(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_vector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "vector->string: not a vector");
        return CH_UNDEFINED;
    }
    ChVector *v = ch_as_vector(args[0]);
    size_t start = 0;
    size_t end = v->len;
    if (parse_optional_range(vm, args, nargs, 1, v->len, "vector->string", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    for (size_t i = start; i < end; i++) {
        if (!ch_is_char(v->items[i])) {
            free(buf);
            snprintf(vm->error, sizeof(vm->error), "vector->string: not a char");
            return CH_UNDEFINED;
        }
        if (!append_codepoint(&buf, &len, &cap, ch_to_char(v->items[i]))) {
            free(buf);
            snprintf(vm->error, sizeof(vm->error), "vector->string: invalid character");
            return CH_UNDEFINED;
        }
    }
    if (buf) {
        buf[len] = '\0';
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", len);
    free(buf);
    return out;
}

static ChValue prim_string_to_list(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string->list: not a string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t count = 0;
    if (utf8_count_codepoints(vm, s, "string->list", &count) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = count;
    if (parse_optional_range(vm, args, nargs, 1, count, "string->list", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t n = end - start;
    ChValue *items = (ChValue *)malloc((n == 0 ? 1 : n) * sizeof(ChValue));
    if (!items) {
        abort();
    }
    size_t pos = 0;
    size_t idx = 0;
    size_t out_i = 0;
    while (pos < s->len && idx < end) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            free(items);
            snprintf(vm->error, sizeof(vm->error), "string->list: invalid UTF-8 sequence");
            return CH_UNDEFINED;
        }
        if (idx >= start) {
            items[out_i++] = ch_make_char(cp);
        }
        idx++;
        pos = next;
    }
    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (size_t j = n; j > 0; j--) {
        ChValue ch = items[j - 1];
        list = ch_gc_cons(&vm->gc, ch, list);
    }
    ch_gc_pop(&vm->gc);
    free(items);
    return list;
}

static ChValue prim_string_fill(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || !ch_is_string(args[0]) || !ch_is_char(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-fill!: expected string and char");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t count = 0;
    if (utf8_count_codepoints(vm, s, "string-fill!", &count) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = count;
    if (parse_optional_range(vm, args, nargs, 2, count, "string-fill!", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    char fill_bytes[4];
    size_t fill_len = 0;
    if (!utf8_encode_codepoint(ch_to_char(args[1]), fill_bytes, &fill_len)) {
        snprintf(vm->error, sizeof(vm->error), "string-fill!: invalid character");
        return CH_UNDEFINED;
    }
    size_t byte_start = 0;
    size_t byte_end = 0;
    if (utf8_offset_for_index(vm, s, start, "string-fill!", &byte_start) != 0 ||
        utf8_offset_for_index(vm, s, end, "string-fill!", &byte_end) != 0) {
        return CH_UNDEFINED;
    }
    size_t fill_cps = end - start;
    if (fill_cps > 0 && fill_len > (SIZE_MAX - 1) / fill_cps) {
        snprintf(vm->error, sizeof(vm->error), "string-fill!: output too large");
        return CH_UNDEFINED;
    }
    size_t rep_len = fill_cps * fill_len;
    char *rep = (char *)malloc(rep_len == 0 ? 1 : rep_len);
    if (!rep) {
        abort();
    }
    for (size_t i = 0; i < fill_cps; i++) {
        memcpy(rep + i * fill_len, fill_bytes, fill_len);
    }
    if (string_splice_bytes(s, byte_start, byte_end, rep, rep_len) != 0) {
        free(rep);
        abort();
    }
    free(rep);
    return CH_VOID;
}

static ChValue prim_list_to_string(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    size_t total = 0;
    for (ChValue p = args[0]; ch_is_pair(p); p = ch_cdr(p)) {
        if (!ch_is_char(ch_car(p))) {
            snprintf(vm->error, sizeof(vm->error), "list->string: not a char");
            return CH_UNDEFINED;
        }
        char encoded[4];
        size_t encoded_len = 0;
        if (!utf8_encode_codepoint(ch_to_char(ch_car(p)), encoded, &encoded_len)) {
            snprintf(vm->error, sizeof(vm->error), "list->string: invalid character");
            return CH_UNDEFINED;
        }
        if (total > SIZE_MAX - encoded_len) {
            snprintf(vm->error, sizeof(vm->error), "list->string: output too large");
            return CH_UNDEFINED;
        }
        total += encoded_len;
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        abort();
    }
    size_t pos = 0;
    for (ChValue p = args[0]; ch_is_pair(p); p = ch_cdr(p)) {
        char encoded[4];
        size_t encoded_len = 0;
        (void)utf8_encode_codepoint(ch_to_char(ch_car(p)), encoded, &encoded_len);
        memcpy(buf + pos, encoded, encoded_len);
        pos += encoded_len;
    }
    buf[total] = '\0';
    ChValue s = ch_gc_make_string(&vm->gc, buf, total);
    free(buf);
    return s;
}

void ch_register_data_primitives(ChVM *vm) {
    define_prim(vm, "values", prim_values, -1, 0);
    define_prim(vm, "call-with-values", prim_call_with_values, 2, 2);
    define_prim(vm, "char?", prim_char_p, 1, 1);
    define_prim(vm, "char=?", prim_char_eq, -1, 2);
    define_prim(vm, "char<?", prim_char_lt, -1, 2);
    define_prim(vm, "char<=?", prim_char_le, -1, 2);
    define_prim(vm, "char>?", prim_char_gt, -1, 2);
    define_prim(vm, "char>=?", prim_char_ge, -1, 2);
    define_prim(vm, "char-ci=?", prim_char_ci_eq, -1, 2);
    define_prim(vm, "char-ci<?", prim_char_ci_lt, -1, 2);
    define_prim(vm, "char-ci<=?", prim_char_ci_le, -1, 2);
    define_prim(vm, "char-ci>?", prim_char_ci_gt, -1, 2);
    define_prim(vm, "char-ci>=?", prim_char_ci_ge, -1, 2);
    define_prim(vm, "char-alphabetic?", prim_char_alphabetic_p, 1, 1);
    define_prim(vm, "char-numeric?", prim_char_numeric_p, 1, 1);
    define_prim(vm, "char-whitespace?", prim_char_whitespace_p, 1, 1);
    define_prim(vm, "char-upper-case?", prim_char_upper_case_p, 1, 1);
    define_prim(vm, "char-lower-case?", prim_char_lower_case_p, 1, 1);
    define_prim(vm, "char-upcase", prim_char_upcase, 1, 1);
    define_prim(vm, "char-downcase", prim_char_downcase, 1, 1);
    define_prim(vm, "char-foldcase", prim_char_foldcase, 1, 1);
    define_prim(vm, "digit-value", prim_digit_value, 1, 1);
    define_prim(vm, "char->integer", prim_char_to_integer, 1, 1);
    define_prim(vm, "integer->char", prim_integer_to_char, 1, 1);
    define_prim(vm, "abs", prim_abs, 1, 1);
    define_prim(vm, "integer?", prim_integer_p, 1, 1);
    define_prim(vm, "zero?", prim_zero_p, 1, 1);
    define_prim(vm, "string-length", prim_string_length, 1, 1);
    define_prim(vm, "string-ref", prim_string_ref, 2, 2);
    define_prim(vm, "string-set!", prim_string_set, 3, 3);
    define_prim(vm, "string", prim_string, -1, 0);
    define_prim(vm, "make-string", prim_make_string, -1, 1);
    define_prim(vm, "string-append", prim_string_append, -1, 0);
    define_prim(vm, "substring", prim_substring, 3, 3);
    define_prim(vm, "string-map", prim_string_map, -1, 2);
    define_prim(vm, "string=?", prim_string_eq, -1, 2);
    define_prim(vm, "string<?", prim_string_lt, -1, 2);
    define_prim(vm, "string<=?", prim_string_le, -1, 2);
    define_prim(vm, "string>?", prim_string_gt, -1, 2);
    define_prim(vm, "string>=?", prim_string_ge, -1, 2);
    define_prim(vm, "string-ci=?", prim_string_ci_eq, -1, 2);
    define_prim(vm, "string-ci<?", prim_string_ci_lt, -1, 2);
    define_prim(vm, "string-ci<=?", prim_string_ci_le, -1, 2);
    define_prim(vm, "string-ci>?", prim_string_ci_gt, -1, 2);
    define_prim(vm, "string-ci>=?", prim_string_ci_ge, -1, 2);
    define_prim(vm, "string-copy", prim_string_copy, -1, 1);
    define_prim(vm, "string-copy!", prim_string_copy_bang, -1, 3);
    define_prim(vm, "string-fill!", prim_string_fill, -1, 2);
    define_prim(vm, "string-upcase", prim_string_upcase, 1, 1);
    define_prim(vm, "string-downcase", prim_string_downcase, 1, 1);
    define_prim(vm, "string-foldcase", prim_string_foldcase, 1, 1);
    define_prim(vm, "string-for-each", prim_string_for_each, -1, 2);
    define_prim(vm, "symbol->string", prim_symbol_to_string, 1, 1);
    define_prim(vm, "string->symbol", prim_string_to_symbol, 1, 1);
    define_prim(vm, "vector", prim_vector, -1, 0);
    define_prim(vm, "vector-length", prim_vector_length, 1, 1);
    define_prim(vm, "vector-ref", prim_vector_ref, 2, 2);
    define_prim(vm, "vector-set!", prim_vector_set, 3, 3);
    define_prim(vm, "vector-fill!", prim_vector_fill, -1, 2);
    define_prim(vm, "vector-copy", prim_vector_copy, -1, 1);
    define_prim(vm, "vector-copy!", prim_vector_copy_bang, -1, 3);
    define_prim(vm, "vector-append", prim_vector_append, -1, 0);
    define_prim(vm, "vector-map", prim_vector_map, -1, 2);
    define_prim(vm, "vector-for-each", prim_vector_for_each, -1, 2);
    define_prim(vm, "make-vector", prim_make_vector, -1, 1);
    define_prim(vm, "vector->list", prim_vector_to_list, -1, 1);
    define_prim(vm, "list->vector", prim_list_to_vector, 1, 1);
    define_prim(vm, "string->vector", prim_string_to_vector, -1, 1);
    define_prim(vm, "vector->string", prim_vector_to_string, -1, 1);
    define_prim(vm, "string->list", prim_string_to_list, -1, 1);
    define_prim(vm, "list->string", prim_list_to_string, 1, 1);
}
