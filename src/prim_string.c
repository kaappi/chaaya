#include "chaaya/prim.h"

#include "chaaya/eval.h"
#include "chaaya/unicode.h"
#include "prim_utf8.h"

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

static ChValue prim_string_length(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-length: not a string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t count = 0;
    if (ch_utf8_count_codepoints(vm, s, "string-length", &count) != 0) {
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
    if (ch_parse_nonnegative_index(vm, args[1], &index, "string-ref") != 0) {
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    uint32_t cp = 0;
    if (ch_utf8_find_codepoint(vm, s, index, "string-ref", NULL, NULL, &cp) != 0) {
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
    if (ch_parse_nonnegative_index(vm, args[1], &index, "string-set!") != 0) {
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t start = 0;
    size_t end = 0;
    if (ch_utf8_find_codepoint(vm, s, index, "string-set!", &start, &end, NULL) != 0) {
        return CH_UNDEFINED;
    }
    char encoded[4];
    size_t encoded_len = 0;
    if (!ch_utf8_encode_codepoint(ch_to_char(args[2]), encoded, &encoded_len)) {
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
        if (!ch_utf8_encode_codepoint(ch_to_char(args[i]), encoded, &encoded_len)) {
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
        (void)ch_utf8_encode_codepoint(ch_to_char(args[i]), encoded, &encoded_len);
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
        if (!ch_utf8_encode_codepoint(ch_to_char(args[1]), fill_bytes, &fill_len)) {
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
    if (ch_parse_nonnegative_index(vm, args[1], &start, "substring") != 0 ||
        ch_parse_nonnegative_index(vm, args[2], &end, "substring") != 0) {
        return CH_UNDEFINED;
    }
    if (end < start) {
        snprintf(vm->error, sizeof(vm->error), "substring: out of range");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t start_byte = 0;
    size_t end_byte = 0;
    if (ch_utf8_offset_for_index(vm, s, start, "substring", &start_byte) != 0 ||
        ch_utf8_offset_for_index(vm, s, end, "substring", &end_byte) != 0) {
        return CH_UNDEFINED;
    }
    return ch_gc_make_string(&vm->gc, s->data + start_byte, end_byte - start_byte);
}

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
        if (ch_utf8_count_codepoints(vm, strings[i], "string-map", &count) != 0) {
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
            if (ch_utf8_find_codepoint(vm, strings[j], i, "string-map", NULL, NULL, &cp) != 0) {
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
        if (!ch_utf8_append_codepoint(&buf, &out_len, &out_cap, ch_to_char(mapped))) {
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
        if (!ch_utf8_decode_next(a->data, a->len, ap, &ac, &an) ||
            !ch_utf8_decode_next(b->data, b->len, bp, &bc, &bn)) {
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

typedef enum { CH_CASE_UP, CH_CASE_DOWN, CH_CASE_FOLD } ChCaseMode;

static bool append_case_codepoint(char **buf, size_t *len, size_t *cap, uint32_t cp, ChCaseMode mode) {
    if (mode == CH_CASE_FOLD) {
        ChUnicodeFoldExpansion exp = ch_unicode_fold_expand(cp);
        for (size_t i = 0; i < exp.len; i++) {
            if (!ch_utf8_append_codepoint(buf, len, cap, exp.cps[i])) {
                return false;
            }
        }
        return true;
    }
    if (mode == CH_CASE_UP) {
        switch (cp) {
        case 0x00DF:
            return ch_utf8_append_codepoint(buf, len, cap, 'S') && ch_utf8_append_codepoint(buf, len, cap, 'S');
        case 0x01F0:
            return ch_utf8_append_codepoint(buf, len, cap, 'J') && ch_utf8_append_codepoint(buf, len, cap, 0x030C);
        case 0x0390:
            return ch_utf8_append_codepoint(buf, len, cap, 0x0399) &&
                   ch_utf8_append_codepoint(buf, len, cap, 0x0308) &&
                   ch_utf8_append_codepoint(buf, len, cap, 0x0301);
        case 0x03B0:
            return ch_utf8_append_codepoint(buf, len, cap, 0x03A5) &&
                   ch_utf8_append_codepoint(buf, len, cap, 0x0308) &&
                   ch_utf8_append_codepoint(buf, len, cap, 0x0301);
        case 0xFB00:
            return ch_utf8_append_codepoint(buf, len, cap, 'F') && ch_utf8_append_codepoint(buf, len, cap, 'F');
        case 0xFB01:
            return ch_utf8_append_codepoint(buf, len, cap, 'F') && ch_utf8_append_codepoint(buf, len, cap, 'I');
        case 0xFB02:
            return ch_utf8_append_codepoint(buf, len, cap, 'F') && ch_utf8_append_codepoint(buf, len, cap, 'L');
        case 0xFB03:
            return ch_utf8_append_codepoint(buf, len, cap, 'F') && ch_utf8_append_codepoint(buf, len, cap, 'F') &&
                   ch_utf8_append_codepoint(buf, len, cap, 'I');
        case 0xFB04:
            return ch_utf8_append_codepoint(buf, len, cap, 'F') && ch_utf8_append_codepoint(buf, len, cap, 'F') &&
                   ch_utf8_append_codepoint(buf, len, cap, 'L');
        default:
            return ch_utf8_append_codepoint(buf, len, cap, ch_unicode_upcase(cp));
        }
    }
    if (cp == 0x0130) {
        return ch_utf8_append_codepoint(buf, len, cap, 0x0069) && ch_utf8_append_codepoint(buf, len, cap, 0x0307);
    }
    return ch_utf8_append_codepoint(buf, len, cap, ch_unicode_downcase(cp));
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
        if (!ch_utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            free(buf);
            snprintf(vm->error, sizeof(vm->error), "%s: invalid UTF-8 sequence", who);
            return CH_UNDEFINED;
        }
        if (mode == CH_CASE_DOWN && cp == 0x03A3) {
            uint32_t next_cp = 0;
            if (next < s->len) {
                size_t nn = 0;
                (void)ch_utf8_decode_next(s->data, s->len, next, &next_cp, &nn);
            }
            bool next_is_cased = ch_unicode_is_cased(next_cp);
            if (prev_cased && !next_is_cased) {
                if (!ch_utf8_append_codepoint(&buf, &len, &cap, 0x03C2)) {
                    free(buf);
                    abort();
                }
            } else if (!ch_utf8_append_codepoint(&buf, &len, &cap, 0x03C3)) {
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
    if (ch_utf8_count_codepoints(vm, s, "string-copy", &count) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = count;
    if (ch_parse_optional_range(vm, args, nargs, 1, count, "string-copy", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t start_byte = 0;
    size_t end_byte = 0;
    if (ch_utf8_offset_for_index(vm, s, start, "string-copy", &start_byte) != 0 ||
        ch_utf8_offset_for_index(vm, s, end, "string-copy", &end_byte) != 0) {
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
    if (ch_parse_nonnegative_index(vm, args[1], &at, "string-copy!") != 0) {
        return CH_UNDEFINED;
    }
    ChString *from = ch_as_string(args[2]);
    size_t from_count = 0;
    if (ch_utf8_count_codepoints(vm, from, "string-copy!", &from_count) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = from_count;
    if (ch_parse_optional_range(vm, args, nargs, 3, from_count, "string-copy!", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t to_count = 0;
    if (ch_utf8_count_codepoints(vm, to, "string-copy!", &to_count) != 0) {
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
    if (ch_utf8_offset_for_index(vm, from, start, "string-copy!", &from_byte_start) != 0 ||
        ch_utf8_offset_for_index(vm, from, end, "string-copy!", &from_byte_end) != 0 ||
        ch_utf8_offset_for_index(vm, to, at, "string-copy!", &to_byte_start) != 0 ||
        ch_utf8_offset_for_index(vm, to, at + copy_cps, "string-copy!", &to_byte_end) != 0) {
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
        if (ch_utf8_count_codepoints(vm, strings[i], "string-for-each", &count) != 0) {
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
            if (ch_utf8_find_codepoint(vm, strings[j], i, "string-for-each", NULL, NULL, &cp) != 0) {
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

static ChValue prim_string_to_list(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string->list: not a string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t count = 0;
    if (ch_utf8_count_codepoints(vm, s, "string->list", &count) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = count;
    if (ch_parse_optional_range(vm, args, nargs, 1, count, "string->list", &start, &end) != 0) {
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
        if (!ch_utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
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
    if (ch_utf8_count_codepoints(vm, s, "string-fill!", &count) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = count;
    if (ch_parse_optional_range(vm, args, nargs, 2, count, "string-fill!", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    char fill_bytes[4];
    size_t fill_len = 0;
    if (!ch_utf8_encode_codepoint(ch_to_char(args[1]), fill_bytes, &fill_len)) {
        snprintf(vm->error, sizeof(vm->error), "string-fill!: invalid character");
        return CH_UNDEFINED;
    }
    size_t byte_start = 0;
    size_t byte_end = 0;
    if (ch_utf8_offset_for_index(vm, s, start, "string-fill!", &byte_start) != 0 ||
        ch_utf8_offset_for_index(vm, s, end, "string-fill!", &byte_end) != 0) {
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
        if (!ch_utf8_encode_codepoint(ch_to_char(ch_car(p)), encoded, &encoded_len)) {
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
        (void)ch_utf8_encode_codepoint(ch_to_char(ch_car(p)), encoded, &encoded_len);
        memcpy(buf + pos, encoded, encoded_len);
        pos += encoded_len;
    }
    buf[total] = '\0';
    ChValue s = ch_gc_make_string(&vm->gc, buf, total);
    free(buf);
    return s;
}

void ch_register_string_primitives(ChVM *vm) {
    define_prim(vm, "string-length", prim_string_length, 1, 1);
    define_prim(vm, "string-ref", prim_string_ref, 2, 2);
    define_prim(vm, "string-set!", prim_string_set, 3, 3);
    define_prim(vm, "string", prim_string, -1, 0);
    define_prim(vm, "make-string", prim_make_string, -1, 1);
    define_prim(vm, "string-append", prim_string_append, -1, 0);
    define_prim(vm, "substring", prim_substring, 3, 3);
    /* Temporary native; ch_install_string_bootstrap replaces with Scheme. */
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
    /* Temporary native; ch_install_string_bootstrap replaces with Scheme. */
    define_prim(vm, "string-for-each", prim_string_for_each, -1, 2);
    define_prim(vm, "symbol->string", prim_symbol_to_string, 1, 1);
    define_prim(vm, "string->symbol", prim_string_to_symbol, 1, 1);
    define_prim(vm, "string->list", prim_string_to_list, -1, 1);
    define_prim(vm, "list->string", prim_list_to_string, 1, 1);
}

/* Kaappi-style Scheme string-map / string-for-each. Strings are UTF-8; walking
 * via string->list keeps the multi-string path linear (index-driven string-ref
 * would be O(n^2) because codepoint indexing rescans from byte 0). */
static const char *string_for_each_src =
    "(define string-for-each\n"
    "  (let ((null? null?) (pair? pair?) (car car) (cdr cdr) (cons cons)\n"
    "        (apply apply) (string->list string->list)\n"
    "        (procedure? procedure?) (not not) (error error))\n"
    "    (lambda (proc str1 . strs)\n"
    "      (if (not (procedure? proc))\n"
    "          (error \"type error in 'string-for-each': expected procedure\" proc))\n"
    "      (if (null? strs)\n"
    "          (let loop ((l (string->list str1)))\n"
    "            (if (pair? l)\n"
    "                (begin (proc (car l)) (loop (cdr l)))\n"
    "                (if #f #f)))\n"
    "          (let loop ((ls (let conv ((s (cons str1 strs)))\n"
    "                           (if (null? s) '()\n"
    "                               (cons (string->list (car s)) (conv (cdr s)))))))\n"
    "            (let ((go (let check ((l ls))\n"
    "                        (if (null? l) #t\n"
    "                            (if (null? (car l)) #f (check (cdr l)))))))\n"
    "              (if go\n"
    "                  (begin\n"
    "                    (apply proc\n"
    "                      (let cars ((l ls))\n"
    "                        (if (null? l) '()\n"
    "                            (cons (car (car l)) (cars (cdr l))))))\n"
    "                    (loop\n"
    "                      (let cdrs ((l ls))\n"
    "                        (if (null? l) '()\n"
    "                            (cons (cdr (car l)) (cdrs (cdr l)))))))\n"
    "                  (if #f #f))))))))\n";

static const char *string_map_src =
    "(define string-map\n"
    "  (let ((null? null?) (pair? pair?) (car car) (cdr cdr) (cons cons)\n"
    "        (apply apply) (string->list string->list)\n"
    "        (list->string list->string) (reverse reverse)\n"
    "        (procedure? procedure?) (not not) (error error))\n"
    "    (lambda (proc str1 . strs)\n"
    "      (if (not (procedure? proc))\n"
    "          (error \"type error in 'string-map': expected procedure\" proc))\n"
    "      (if (null? strs)\n"
    "          (let loop ((l (string->list str1)) (acc '()))\n"
    "            (if (pair? l)\n"
    "                (loop (cdr l) (cons (proc (car l)) acc))\n"
    "                (list->string (reverse acc))))\n"
    "          (let loop ((ls (let conv ((s (cons str1 strs)))\n"
    "                           (if (null? s) '()\n"
    "                               (cons (string->list (car s)) (conv (cdr s))))))\n"
    "                     (acc '()))\n"
    "            (let ((go (let check ((l ls))\n"
    "                        (if (null? l) #t\n"
    "                            (if (null? (car l)) #f (check (cdr l)))))))\n"
    "              (if go\n"
    "                  (loop\n"
    "                    (let cdrs ((l ls))\n"
    "                      (if (null? l) '()\n"
    "                          (cons (cdr (car l)) (cdrs (cdr l)))))\n"
    "                    (cons\n"
    "                      (apply proc\n"
    "                        (let cars ((l ls))\n"
    "                          (if (null? l) '()\n"
    "                              (cons (car (car l)) (cars (cdr l))))))\n"
    "                      acc))\n"
    "                  (list->string (reverse acc)))))))))\n";

void ch_install_string_bootstrap(ChVM *vm) {
    if (ch_eval_source(vm, string_for_each_src, strlen(string_for_each_src), 0) != 0) {
        fprintf(stderr, "chaaya: failed to install string-for-each bootstrap\n");
        abort();
    }
    if (ch_eval_source(vm, string_map_src, strlen(string_map_src), 0) != 0) {
        fprintf(stderr, "chaaya: failed to install string-map bootstrap\n");
        abort();
    }
}
