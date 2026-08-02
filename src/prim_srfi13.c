#include "chaaya/prim.h"
#include "chaaya/unicode.h"

#include "prim_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
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
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        *len_out = 2;
        return true;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        *len_out = 3;
        return true;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        *len_out = 4;
        return true;
    }
    return false;
}

static int utf8_count_codepoints(ChVM *vm, ChString *s, const char *who, size_t *count_out) {
    size_t count = 0;
    size_t pos = 0;
    while (pos < s->len) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            snprintf(vm->error, sizeof(vm->error), "%s: invalid UTF-8", who);
            return -1;
        }
        pos = next;
        count++;
    }
    *count_out = count;
    return 0;
}

static int utf8_offset_for_index(ChVM *vm, ChString *s, size_t index, const char *who,
                                 size_t *byte_out) {
    size_t cp = 0;
    size_t pos = 0;
    while (pos < s->len) {
        if (cp == index) {
            *byte_out = pos;
            return 0;
        }
        uint32_t code = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &code, &next)) {
            snprintf(vm->error, sizeof(vm->error), "%s: invalid UTF-8", who);
            return -1;
        }
        pos = next;
        cp++;
    }
    if (cp == index) {
        *byte_out = pos;
        return 0;
    }
    snprintf(vm->error, sizeof(vm->error), "%s: index out of range", who);
    return -1;
}

static int call_pred_char(ChVM *vm, ChValue pred, uint32_t cp, bool *out, const char *who) {
    ChValue arg = ch_make_char(cp);
    ChValue result = CH_UNDEFINED;
    ChVMStatus st = ch_vm_apply(vm, pred, &arg, 1, &result);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return -1;
    }
    if (st != CH_VM_OK) {
        return -1;
    }
    (void)who;
    *out = result != CH_FALSE && !ch_is_false(result);
    return 0;
}

static bool append_bytes(char **buf, size_t *len, size_t *cap, const char *bytes, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t ncap = (*cap == 0) ? 64 : *cap * 2;
        while (ncap < *len + n + 1) {
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
    (*buf)[*len] = '\0';
    return true;
}

static ChValue prim_string_contains(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || !ch_is_string(args[0]) || !ch_is_string(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-contains: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    ChString *pat = ch_as_string(args[1]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, "string-contains", &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = cp_len;
    if (parse_optional_range(vm, args, nargs, 2, cp_len, "string-contains", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t pat_cp_len = 0;
    if (utf8_count_codepoints(vm, pat, "string-contains", &pat_cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t pat_start = 0;
    size_t pat_end = pat_cp_len;
    if (parse_optional_range(vm, args, nargs, 4, pat_cp_len, "string-contains", &pat_start,
                             &pat_end) != 0) {
        return CH_UNDEFINED;
    }
    size_t start_byte = 0;
    size_t end_byte = 0;
    if (utf8_offset_for_index(vm, s, start, "string-contains", &start_byte) != 0 ||
        utf8_offset_for_index(vm, s, end, "string-contains", &end_byte) != 0) {
        return CH_UNDEFINED;
    }
    size_t pat_start_byte = 0;
    size_t pat_end_byte = 0;
    if (utf8_offset_for_index(vm, pat, pat_start, "string-contains", &pat_start_byte) != 0 ||
        utf8_offset_for_index(vm, pat, pat_end, "string-contains", &pat_end_byte) != 0) {
        return CH_UNDEFINED;
    }
    const char *pat_slice = pat->data + pat_start_byte;
    size_t pat_slice_len = pat_end_byte - pat_start_byte;
    if (pat_slice_len == 0) {
        return ch_make_fixnum((int64_t)start);
    }
    if (pat_slice_len > (end_byte - start_byte)) {
        return CH_FALSE;
    }
    size_t cp_idx = start;
    for (size_t pos = start_byte; pos + pat_slice_len <= end_byte;) {
        if (memcmp(s->data + pos, pat_slice, pat_slice_len) == 0) {
            return ch_make_fixnum((int64_t)cp_idx);
        }
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            snprintf(vm->error, sizeof(vm->error), "string-contains: invalid UTF-8");
            return CH_UNDEFINED;
        }
        (void)cp;
        pos = next;
        cp_idx++;
    }
    return CH_FALSE;
}

static ChValue prim_string_prefix_p(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || !ch_is_string(args[0]) || !ch_is_string(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-prefix?: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *prefix = ch_as_string(args[0]);
    ChString *s = ch_as_string(args[1]);
    if (prefix->len > s->len) {
        return CH_FALSE;
    }
    return memcmp(prefix->data, s->data, prefix->len) == 0 ? CH_TRUE : CH_FALSE;
}

static ChValue prim_string_suffix_p(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || !ch_is_string(args[0]) || !ch_is_string(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-suffix?: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *suffix = ch_as_string(args[0]);
    ChString *s = ch_as_string(args[1]);
    if (suffix->len > s->len) {
        return CH_FALSE;
    }
    return memcmp(suffix->data, s->data + s->len - suffix->len, suffix->len) == 0 ? CH_TRUE
                                                                                  : CH_FALSE;
}

static ChValue prim_string_concatenate(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_pair(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-concatenate: expected list");
        return CH_UNDEFINED;
    }
    size_t total = 0;
    for (ChValue p = args[0]; ch_is_pair(p); p = ch_cdr(p)) {
        ChValue v = ch_car(p);
        if (!ch_is_string(v)) {
            snprintf(vm->error, sizeof(vm->error), "string-concatenate: not a string");
            return CH_UNDEFINED;
        }
        total += ch_as_string(v)->len;
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        abort();
    }
    size_t pos = 0;
    for (ChValue p = args[0]; ch_is_pair(p); p = ch_cdr(p)) {
        ChString *s = ch_as_string(ch_car(p));
        memcpy(buf + pos, s->data, s->len);
        pos += s->len;
    }
    buf[total] = '\0';
    ChValue out = ch_gc_make_string(&vm->gc, buf, total);
    free(buf);
    return out;
}

static ChValue prim_string_unfold(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 4) {
        snprintf(vm->error, sizeof(vm->error), "string-unfold: expected at least 4 arguments");
        return CH_UNDEFINED;
    }
    ChValue p = args[0];
    ChValue f = args[1];
    ChValue g = args[2];
    ChValue seed = args[3];
    ch_gc_push(&vm->gc, &seed);

    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    if (nargs > 4 && ch_is_string(args[4])) {
        ChString *base = ch_as_string(args[4]);
        if (!append_bytes(&buf, &len, &cap, base->data, base->len)) {
            ch_gc_pop(&vm->gc);
            abort();
        }
    }

    while (true) {
        ChValue stop = CH_UNDEFINED;
        if (ch_vm_apply(vm, p, &seed, 1, &stop) != CH_VM_OK) {
            free(buf);
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        if (stop != CH_FALSE && !ch_is_false(stop)) {
            break;
        }
        ChValue chv = CH_UNDEFINED;
        if (ch_vm_apply(vm, f, &seed, 1, &chv) != CH_VM_OK) {
            free(buf);
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        if (!ch_is_char(chv)) {
            free(buf);
            ch_gc_pop(&vm->gc);
            snprintf(vm->error, sizeof(vm->error), "string-unfold: procedure must return char");
            return CH_UNDEFINED;
        }
        char enc[4];
        size_t elen = 0;
        if (!utf8_encode_codepoint(ch_to_char(chv), enc, &elen) ||
            !append_bytes(&buf, &len, &cap, enc, elen)) {
            free(buf);
            ch_gc_pop(&vm->gc);
            abort();
        }
        ChValue next = CH_UNDEFINED;
        if (ch_vm_apply(vm, g, &seed, 1, &next) != CH_VM_OK) {
            free(buf);
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        seed = next;
    }

    if (nargs > 5) {
        ChValue finalv = CH_UNDEFINED;
        if (ch_vm_apply(vm, args[5], &seed, 1, &finalv) != CH_VM_OK) {
            free(buf);
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        if (!ch_is_string(finalv)) {
            free(buf);
            ch_gc_pop(&vm->gc);
            snprintf(vm->error, sizeof(vm->error), "string-unfold: final must be string");
            return CH_UNDEFINED;
        }
        ChString *fs = ch_as_string(finalv);
        if (!append_bytes(&buf, &len, &cap, fs->data, fs->len)) {
            free(buf);
            ch_gc_pop(&vm->gc);
            abort();
        }
    }

    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", len);
    free(buf);
    ch_gc_pop(&vm->gc);
    return out;
}

static ChValue prim_string_unfold_right(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 4) {
        snprintf(vm->error, sizeof(vm->error), "string-unfold-right: expected at least 4 arguments");
        return CH_UNDEFINED;
    }
    ChValue p = args[0];
    ChValue f = args[1];
    ChValue g = args[2];
    ChValue seed = args[3];
    ch_gc_push(&vm->gc, &seed);

    uint32_t *cps = NULL;
    size_t cp_count = 0;
    size_t cp_cap = 0;

    while (true) {
        ChValue stop = CH_UNDEFINED;
        if (ch_vm_apply(vm, p, &seed, 1, &stop) != CH_VM_OK) {
            free(cps);
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        if (stop != CH_FALSE && !ch_is_false(stop)) {
            break;
        }
        ChValue chv = CH_UNDEFINED;
        if (ch_vm_apply(vm, f, &seed, 1, &chv) != CH_VM_OK) {
            free(cps);
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        if (!ch_is_char(chv)) {
            free(cps);
            ch_gc_pop(&vm->gc);
            snprintf(vm->error, sizeof(vm->error), "string-unfold-right: must return char");
            return CH_UNDEFINED;
        }
        if (cp_count + 1 > cp_cap) {
            cp_cap = cp_cap ? cp_cap * 2 : 16;
            uint32_t *nc = (uint32_t *)realloc(cps, cp_cap * sizeof(uint32_t));
            if (!nc) {
                free(cps);
                ch_gc_pop(&vm->gc);
                abort();
            }
            cps = nc;
        }
        cps[cp_count++] = ch_to_char(chv);
        ChValue next = CH_UNDEFINED;
        if (ch_vm_apply(vm, g, &seed, 1, &next) != CH_VM_OK) {
            free(cps);
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        seed = next;
    }

    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    if (nargs > 5) {
        ChValue finalv = CH_UNDEFINED;
        if (ch_vm_apply(vm, args[5], &seed, 1, &finalv) != CH_VM_OK) {
            free(cps);
            ch_gc_pop(&vm->gc);
            return CH_UNDEFINED;
        }
        if (!ch_is_string(finalv)) {
            free(cps);
            ch_gc_pop(&vm->gc);
            snprintf(vm->error, sizeof(vm->error), "string-unfold-right: final must be string");
            return CH_UNDEFINED;
        }
        ChString *fs = ch_as_string(finalv);
        if (!append_bytes(&buf, &len, &cap, fs->data, fs->len)) {
            free(cps);
            ch_gc_pop(&vm->gc);
            abort();
        }
    }

    for (size_t i = cp_count; i > 0; i--) {
        char enc[4];
        size_t elen = 0;
        if (!utf8_encode_codepoint(cps[i - 1], enc, &elen) ||
            !append_bytes(&buf, &len, &cap, enc, elen)) {
            free(cps);
            free(buf);
            ch_gc_pop(&vm->gc);
            abort();
        }
    }

    if (nargs > 4 && ch_is_string(args[4])) {
        ChString *base = ch_as_string(args[4]);
        if (!append_bytes(&buf, &len, &cap, base->data, base->len)) {
            free(cps);
            free(buf);
            ch_gc_pop(&vm->gc);
            abort();
        }
    }

    free(cps);
    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", len);
    free(buf);
    ch_gc_pop(&vm->gc);
    return out;
}

static ChValue string_index_right_impl(ChVM *vm, ChValue *args, int nargs, const char *who,
                                       bool skip_mode) {
    if (nargs < 2 || !ch_is_string(args[0]) || !ch_is_procedure(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "%s: bad arguments", who);
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, who, &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = cp_len;
    if (parse_optional_range(vm, args, nargs, 2, cp_len, who, &start, &end) != 0) {
        return CH_UNDEFINED;
    }

    ssize_t last = -1;
    size_t cp_idx = 0;
    size_t pos = 0;
    while (pos < s->len && cp_idx < end) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            pos++;
            cp_idx++;
            continue;
        }
        if (cp_idx >= start) {
            bool match = false;
            if (call_pred_char(vm, args[1], cp, &match, who) != 0) {
                return CH_UNDEFINED;
            }
            if (skip_mode) {
                if (!match) {
                    last = (ssize_t)cp_idx;
                }
            } else if (match) {
                last = (ssize_t)cp_idx;
            }
        }
        pos = next;
        cp_idx++;
    }
    return last >= 0 ? ch_make_fixnum(last) : CH_FALSE;
}

static ChValue prim_string_index_right(ChVM *vm, ChValue *args, int nargs) {
    return string_index_right_impl(vm, args, nargs, "string-index-right", false);
}

static ChValue prim_string_skip(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || !ch_is_string(args[0]) || !ch_is_procedure(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-skip: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, "string-skip", &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = cp_len;
    if (parse_optional_range(vm, args, nargs, 2, cp_len, "string-skip", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t cp_idx = 0;
    size_t pos = 0;
    while (pos < s->len && cp_idx < end) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            pos++;
            cp_idx++;
            continue;
        }
        if (cp_idx >= start) {
            bool match = false;
            if (call_pred_char(vm, args[1], cp, &match, "string-skip") != 0) {
                return CH_UNDEFINED;
            }
            if (!match) {
                return ch_make_fixnum((int64_t)cp_idx);
            }
        }
        pos = next;
        cp_idx++;
    }
    return CH_FALSE;
}

static ChValue prim_string_skip_right(ChVM *vm, ChValue *args, int nargs) {
    return string_index_right_impl(vm, args, nargs, "string-skip-right", true);
}

static ChValue prim_string_index(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || !ch_is_string(args[0]) || !ch_is_procedure(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-index: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, "string-index", &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = cp_len;
    if (parse_optional_range(vm, args, nargs, 2, cp_len, "string-index", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t cp_idx = 0;
    size_t pos = 0;
    while (pos < s->len && cp_idx < end) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            pos++;
            cp_idx++;
            continue;
        }
        if (cp_idx >= start) {
            bool match = false;
            if (call_pred_char(vm, args[1], cp, &match, "string-index") != 0) {
                return CH_UNDEFINED;
            }
            if (match) {
                return ch_make_fixnum((int64_t)cp_idx);
            }
        }
        pos = next;
        cp_idx++;
    }
    return CH_FALSE;
}

static ChValue prim_string_take(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-take: expected string");
        return CH_UNDEFINED;
    }
    size_t n = 0;
    if (parse_nonnegative_index(vm, args[1], &n, "string-take") != 0) {
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t end_byte = 0;
    if (utf8_offset_for_index(vm, s, n, "string-take", &end_byte) != 0) {
        return CH_UNDEFINED;
    }
    return ch_gc_make_string(&vm->gc, s->data, end_byte);
}

static ChValue prim_string_drop(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-drop: expected string");
        return CH_UNDEFINED;
    }
    size_t n = 0;
    if (parse_nonnegative_index(vm, args[1], &n, "string-drop") != 0) {
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t start_byte = 0;
    if (utf8_offset_for_index(vm, s, n, "string-drop", &start_byte) != 0) {
        return CH_UNDEFINED;
    }
    return ch_gc_make_string(&vm->gc, s->data + start_byte, s->len - start_byte);
}

static size_t find_prev_cp_start(const char *data, size_t end) {
    size_t p = end;
    while (p > 0) {
        p--;
        if (((unsigned char)data[p] & 0xC0) != 0x80) {
            return p;
        }
    }
    return 0;
}

static int trim_pred_match(ChVM *vm, ChValue pred, uint32_t cp, bool *out, const char *who) {
    if (ch_is_procedure(pred)) {
        return call_pred_char(vm, pred, cp, out, who);
    }
    *out = ch_unicode_is_whitespace(cp);
    return 0;
}

static ChValue string_trim_impl(ChVM *vm, ChValue *args, int nargs, const char *who, int mode) {
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected string", who);
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    ChValue pred = (nargs > 1) ? args[1] : CH_NIL;
    size_t start = 0;
    size_t end = s->len;

    if (mode == 0 || mode == 2) {
        size_t pos = 0;
        while (pos < end) {
            uint32_t cp = 0;
            size_t next = 0;
            if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
                bool match = false;
                if (nargs > 1) {
                    if (trim_pred_match(vm, pred, (uint32_t)(unsigned char)s->data[pos], &match, who) !=
                        0) {
                        return CH_UNDEFINED;
                    }
                } else {
                    match = ch_unicode_is_whitespace((uint32_t)(unsigned char)s->data[pos]);
                }
                if (!match) {
                    break;
                }
                pos++;
                continue;
            }
            bool match = false;
            if (trim_pred_match(vm, pred, cp, &match, who) != 0) {
                return CH_UNDEFINED;
            }
            if (!match) {
                break;
            }
            pos = next;
        }
        start = pos;
    }

    if (mode == 1 || mode == 2) {
        while (end > start) {
            size_t cp_start = find_prev_cp_start(s->data, end);
            uint32_t cp = 0;
            size_t next = 0;
            if (!utf8_decode_next(s->data, s->len, cp_start, &cp, &next)) {
                bool match = false;
                if (nargs > 1) {
                    if (trim_pred_match(vm, pred, (uint32_t)(unsigned char)s->data[cp_start], &match,
                                        who) != 0) {
                        return CH_UNDEFINED;
                    }
                } else {
                    match = ch_unicode_is_whitespace((uint32_t)(unsigned char)s->data[cp_start]);
                }
                if (!match) {
                    break;
                }
                end = cp_start;
                continue;
            }
            bool match = false;
            if (trim_pred_match(vm, pred, cp, &match, who) != 0) {
                return CH_UNDEFINED;
            }
            if (!match) {
                break;
            }
            end = cp_start;
        }
    }

    return ch_gc_make_string(&vm->gc, s->data + start, end - start);
}

static ChValue prim_string_trim(ChVM *vm, ChValue *args, int nargs) {
    return string_trim_impl(vm, args, nargs, "string-trim", 0);
}

static ChValue prim_string_trim_right(ChVM *vm, ChValue *args, int nargs) {
    return string_trim_impl(vm, args, nargs, "string-trim-right", 1);
}

static ChValue prim_string_trim_both(ChVM *vm, ChValue *args, int nargs) {
    return string_trim_impl(vm, args, nargs, "string-trim-both", 2);
}

static ChValue prim_string_null_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-null?: expected string");
        return CH_UNDEFINED;
    }
    return ch_as_string(args[0])->len == 0 ? CH_TRUE : CH_FALSE;
}

static ChValue prim_string_take_right(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-take-right: expected string");
        return CH_UNDEFINED;
    }
    size_t n = 0;
    if (parse_nonnegative_index(vm, args[1], &n, "string-take-right") != 0) {
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, "string-take-right", &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    if (n > cp_len) {
        snprintf(vm->error, sizeof(vm->error), "string-take-right: index out of range");
        return CH_UNDEFINED;
    }
    if (n == cp_len) {
        return ch_gc_make_string(&vm->gc, s->data, s->len);
    }
    size_t start_byte = 0;
    if (utf8_offset_for_index(vm, s, cp_len - n, "string-take-right", &start_byte) != 0) {
        return CH_UNDEFINED;
    }
    return ch_gc_make_string(&vm->gc, s->data + start_byte, s->len - start_byte);
}

static ChValue prim_string_drop_right(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-drop-right: expected string");
        return CH_UNDEFINED;
    }
    size_t n = 0;
    if (parse_nonnegative_index(vm, args[1], &n, "string-drop-right") != 0) {
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, "string-drop-right", &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    if (n > cp_len) {
        snprintf(vm->error, sizeof(vm->error), "string-drop-right: index out of range");
        return CH_UNDEFINED;
    }
    if (n == cp_len) {
        return ch_gc_make_string(&vm->gc, "", 0);
    }
    size_t end_byte = 0;
    if (utf8_offset_for_index(vm, s, cp_len - n, "string-drop-right", &end_byte) != 0) {
        return CH_UNDEFINED;
    }
    return ch_gc_make_string(&vm->gc, s->data, end_byte);
}

static int pad_char_bytes(ChVM *vm, ChValue padv, const char *who, char out[4], size_t *len_out) {
    if (!ch_is_char(padv)) {
        snprintf(vm->error, sizeof(vm->error), "%s: pad character must be char", who);
        return -1;
    }
    if (!utf8_encode_codepoint(ch_to_char(padv), out, len_out)) {
        snprintf(vm->error, sizeof(vm->error), "%s: invalid pad character", who);
        return -1;
    }
    return 0;
}

static ChValue string_pad_impl(ChVM *vm, ChValue *args, int nargs, const char *who, bool right) {
    if (nargs < 2 || !ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected string and length", who);
        return CH_UNDEFINED;
    }
    size_t target_len = 0;
    if (parse_nonnegative_index(vm, args[1], &target_len, who) != 0) {
        return CH_UNDEFINED;
    }
    char pad_bytes[4] = {' '};
    size_t pad_len = 1;
    if (nargs > 2) {
        if (pad_char_bytes(vm, args[2], who, pad_bytes, &pad_len) != 0) {
            return CH_UNDEFINED;
        }
    }
    ChString *s = ch_as_string(args[0]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, who, &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = cp_len;
    if (parse_optional_range(vm, args, nargs, 3, cp_len, who, &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t start_byte = 0;
    size_t end_byte = 0;
    if (utf8_offset_for_index(vm, s, start, who, &start_byte) != 0 ||
        utf8_offset_for_index(vm, s, end, who, &end_byte) != 0) {
        return CH_UNDEFINED;
    }
    const char *data = s->data + start_byte;
    size_t data_len = end_byte - start_byte;
    size_t current_len = end - start;
    if (current_len >= target_len) {
        if (right) {
            size_t trunc_end = 0;
            if (utf8_offset_for_index(vm, s, start + target_len, who, &trunc_end) != 0) {
                return CH_UNDEFINED;
            }
            return ch_gc_make_string(&vm->gc, s->data + start_byte, trunc_end - start_byte);
        }
        size_t trunc_start = 0;
        if (utf8_offset_for_index(vm, s, end - target_len, who, &trunc_start) != 0) {
            return CH_UNDEFINED;
        }
        return ch_gc_make_string(&vm->gc, s->data + trunc_start, end_byte - trunc_start);
    }
    size_t pad_count = target_len - current_len;
    size_t out_len = data_len + pad_count * pad_len;
    char *buf = (char *)malloc(out_len + 1);
    if (!buf) {
        abort();
    }
    size_t pos = 0;
    if (!right) {
        for (size_t i = 0; i < pad_count; i++) {
            memcpy(buf + pos, pad_bytes, pad_len);
            pos += pad_len;
        }
    }
    memcpy(buf + pos, data, data_len);
    pos += data_len;
    if (right) {
        for (size_t i = 0; i < pad_count; i++) {
            memcpy(buf + pos, pad_bytes, pad_len);
            pos += pad_len;
        }
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf, out_len);
    free(buf);
    return out;
}

static ChValue prim_string_pad(ChVM *vm, ChValue *args, int nargs) {
    return string_pad_impl(vm, args, nargs, "string-pad", false);
}

static ChValue prim_string_pad_right(ChVM *vm, ChValue *args, int nargs) {
    return string_pad_impl(vm, args, nargs, "string-pad-right", true);
}

static ChValue prim_string_replace(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 4 || !ch_is_string(args[0]) || !ch_is_string(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-replace: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *s1 = ch_as_string(args[0]);
    ChString *s2 = ch_as_string(args[1]);
    size_t start = 0;
    size_t end = 0;
    if (parse_nonnegative_index(vm, args[2], &start, "string-replace") != 0 ||
        parse_nonnegative_index(vm, args[3], &end, "string-replace") != 0) {
        return CH_UNDEFINED;
    }
    if (start > end) {
        snprintf(vm->error, sizeof(vm->error), "string-replace: range out of bounds");
        return CH_UNDEFINED;
    }
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s1, "string-replace", &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    if (end > cp_len) {
        snprintf(vm->error, sizeof(vm->error), "string-replace: range out of bounds");
        return CH_UNDEFINED;
    }
    size_t start_byte = 0;
    size_t end_byte = 0;
    if (utf8_offset_for_index(vm, s1, start, "string-replace", &start_byte) != 0 ||
        utf8_offset_for_index(vm, s1, end, "string-replace", &end_byte) != 0) {
        return CH_UNDEFINED;
    }
    size_t s2_cp_len = 0;
    if (utf8_count_codepoints(vm, s2, "string-replace", &s2_cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t s2_start = 0;
    size_t s2_end = s2_cp_len;
    if (parse_optional_range(vm, args, nargs, 4, s2_cp_len, "string-replace", &s2_start,
                             &s2_end) != 0) {
        return CH_UNDEFINED;
    }
    size_t s2_start_byte = 0;
    size_t s2_end_byte = 0;
    if (utf8_offset_for_index(vm, s2, s2_start, "string-replace", &s2_start_byte) != 0 ||
        utf8_offset_for_index(vm, s2, s2_end, "string-replace", &s2_end_byte) != 0) {
        return CH_UNDEFINED;
    }
    const char *ins = s2->data + s2_start_byte;
    size_t ins_len = s2_end_byte - s2_start_byte;
    size_t out_len = start_byte + ins_len + (s1->len - end_byte);
    char *buf = (char *)malloc(out_len + 1);
    if (!buf) {
        abort();
    }
    memcpy(buf, s1->data, start_byte);
    memcpy(buf + start_byte, ins, ins_len);
    memcpy(buf + start_byte + ins_len, s1->data + end_byte, s1->len - end_byte);
    ChValue out = ch_gc_make_string(&vm->gc, buf, out_len);
    free(buf);
    return out;
}

static bool titlecase_word_boundary(uint32_t cp) {
    return !ch_unicode_is_cased(cp);
}

static int append_codepoint(char **buf, size_t *len, size_t *cap, uint32_t cp) {
    char enc[4];
    size_t elen = 0;
    if (!utf8_encode_codepoint(cp, enc, &elen)) {
        return -1;
    }
    return append_bytes(buf, len, cap, enc, elen) ? 0 : -1;
}

static ChValue titlecase_range(ChVM *vm, const char *data, size_t len) {
    char *buf = NULL;
    size_t blen = 0;
    size_t cap = 0;
    bool word_start = true;
    size_t pos = 0;
    while (pos < len) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(data, len, pos, &cp, &next)) {
            if (!append_bytes(&buf, &blen, &cap, data + pos, 1)) {
                free(buf);
                abort();
            }
            pos++;
            word_start = true;
            continue;
        }
        if (titlecase_word_boundary(cp)) {
            if (!append_bytes(&buf, &blen, &cap, data + pos, next - pos)) {
                free(buf);
                abort();
            }
            word_start = true;
        } else if (word_start) {
            if (append_codepoint(&buf, &blen, &cap, ch_unicode_upcase(cp)) != 0) {
                free(buf);
                abort();
            }
            word_start = false;
        } else {
            if (append_codepoint(&buf, &blen, &cap, ch_unicode_downcase(cp)) != 0) {
                free(buf);
                abort();
            }
        }
        pos = next;
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", blen);
    free(buf);
    return out;
}

static ChValue prim_string_titlecase(ChVM *vm, ChValue *args, int nargs) {
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-titlecase: expected string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, "string-titlecase", &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = cp_len;
    if (parse_optional_range(vm, args, nargs, 1, cp_len, "string-titlecase", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t start_byte = 0;
    size_t end_byte = 0;
    if (utf8_offset_for_index(vm, s, start, "string-titlecase", &start_byte) != 0 ||
        utf8_offset_for_index(vm, s, end, "string-titlecase", &end_byte) != 0) {
        return CH_UNDEFINED;
    }
    return titlecase_range(vm, s->data + start_byte, end_byte - start_byte);
}

typedef enum {
    JOIN_INFIX,
    JOIN_STRICT_INFIX,
    JOIN_PREFIX,
    JOIN_SUFFIX,
} JoinGrammar;

static int parse_join_grammar(ChVM *vm, ChValue v, JoinGrammar *out) {
    if (!ch_is_symbol(v)) {
        snprintf(vm->error, sizeof(vm->error),
                 "string-join: grammar must be infix, strict-infix, prefix, or suffix");
        return -1;
    }
    const char *name = ch_as_symbol(v)->name;
    if (strcmp(name, "infix") == 0) {
        *out = JOIN_INFIX;
        return 0;
    }
    if (strcmp(name, "strict-infix") == 0) {
        *out = JOIN_STRICT_INFIX;
        return 0;
    }
    if (strcmp(name, "prefix") == 0) {
        *out = JOIN_PREFIX;
        return 0;
    }
    if (strcmp(name, "suffix") == 0) {
        *out = JOIN_SUFFIX;
        return 0;
    }
    snprintf(vm->error, sizeof(vm->error),
             "string-join: grammar must be infix, strict-infix, prefix, or suffix");
    return -1;
}

static ChValue prim_string_join(ChVM *vm, ChValue *args, int nargs) {
    if (!ch_is_pair(args[0]) && !ch_is_nil(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-join: expected list");
        return CH_UNDEFINED;
    }
    const char *delim = " ";
    size_t delim_len = 1;
    if (nargs > 1) {
        if (!ch_is_string(args[1])) {
            snprintf(vm->error, sizeof(vm->error), "string-join: delimiter must be string");
            return CH_UNDEFINED;
        }
        ChString *ds = ch_as_string(args[1]);
        delim = ds->data;
        delim_len = ds->len;
    }
    JoinGrammar grammar = JOIN_INFIX;
    if (nargs > 2) {
        if (parse_join_grammar(vm, args[2], &grammar) != 0) {
            return CH_UNDEFINED;
        }
    }

    size_t count = 0;
    size_t total = 0;
    for (ChValue p = args[0]; ch_is_pair(p); p = ch_cdr(p)) {
        ChValue v = ch_car(p);
        if (!ch_is_string(v)) {
            snprintf(vm->error, sizeof(vm->error), "string-join: not a string");
            return CH_UNDEFINED;
        }
        total += ch_as_string(v)->len;
        count++;
    }
    if (!ch_is_nil(args[0]) && !ch_is_pair(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-join: expected list");
        return CH_UNDEFINED;
    }
    if (count == 0) {
        if (grammar == JOIN_STRICT_INFIX) {
            snprintf(vm->error, sizeof(vm->error),
                     "string-join: strict-infix grammar requires a non-empty list");
            return CH_UNDEFINED;
        }
        return ch_gc_make_string(&vm->gc, "", 0);
    }

    switch (grammar) {
    case JOIN_INFIX:
    case JOIN_STRICT_INFIX:
        total += (count - 1) * delim_len;
        break;
    case JOIN_PREFIX:
    case JOIN_SUFFIX:
        total += count * delim_len;
        break;
    }

    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        abort();
    }
    size_t pos = 0;
    bool first = true;
    for (ChValue p = args[0]; ch_is_pair(p); p = ch_cdr(p)) {
        ChString *s = ch_as_string(ch_car(p));
        if (grammar == JOIN_PREFIX && delim_len > 0) {
            memcpy(buf + pos, delim, delim_len);
            pos += delim_len;
        } else if ((grammar == JOIN_INFIX || grammar == JOIN_STRICT_INFIX) && !first &&
                   delim_len > 0) {
            memcpy(buf + pos, delim, delim_len);
            pos += delim_len;
        }
        first = false;
        memcpy(buf + pos, s->data, s->len);
        pos += s->len;
        if (grammar == JOIN_SUFFIX && delim_len > 0) {
            memcpy(buf + pos, delim, delim_len);
            pos += delim_len;
        }
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf, total);
    free(buf);
    return out;
}

static ChValue prim_string_split(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0]) || !ch_is_string(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-split: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    ChString *delim = ch_as_string(args[1]);

    ChValue result = CH_NIL;
    ch_gc_push(&vm->gc, &result);

    if (delim->len == 0) {
        size_t pos = s->len;
        while (pos > 0) {
            size_t start = find_prev_cp_start(s->data, pos);
            ChValue part = ch_gc_make_string(&vm->gc, s->data + start, pos - start);
            ch_gc_push(&vm->gc, &part);
            result = ch_gc_cons(&vm->gc, part, result);
            ch_gc_pop(&vm->gc);
            pos = start;
        }
        ch_gc_pop(&vm->gc);
        return result;
    }

    size_t splits[512][2];
    size_t nsplits = 0;
    size_t scan = 0;
    while (scan <= s->len) {
        if (nsplits >= 512) {
            ch_gc_pop(&vm->gc);
            snprintf(vm->error, sizeof(vm->error), "string-split: too many parts");
            return CH_UNDEFINED;
        }
        const char *found = NULL;
        if (scan < s->len) {
            for (size_t i = scan; i + delim->len <= s->len; i++) {
                if (memcmp(s->data + i, delim->data, delim->len) == 0) {
                    found = s->data + i;
                    break;
                }
            }
        }
        if (found) {
            size_t fpos = (size_t)(found - s->data);
            splits[nsplits][0] = scan;
            splits[nsplits][1] = fpos;
            nsplits++;
            scan = fpos + delim->len;
        } else {
            splits[nsplits][0] = scan;
            splits[nsplits][1] = s->len;
            nsplits++;
            break;
        }
    }

    for (size_t i = nsplits; i > 0; i--) {
        ChValue part =
            ch_gc_make_string(&vm->gc, s->data + splits[i - 1][0], splits[i - 1][1] - splits[i - 1][0]);
        ch_gc_push(&vm->gc, &part);
        result = ch_gc_cons(&vm->gc, part, result);
        ch_gc_pop(&vm->gc);
    }
    ch_gc_pop(&vm->gc);
    return result;
}

static ChValue prim_string_tabulate(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_procedure(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string-tabulate: expected procedure");
        return CH_UNDEFINED;
    }
    if (!ch_is_fixnum(args[1]) || ch_to_fixnum(args[1]) < 0) {
        snprintf(vm->error, sizeof(vm->error), "string-tabulate: expected non-negative integer");
        return CH_UNDEFINED;
    }
    size_t len = (size_t)ch_to_fixnum(args[1]);
    /* Build via a growable UTF-8 buffer of chars produced by the proc. */
    size_t cap = len * 4 + 1;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        snprintf(vm->error, sizeof(vm->error), "string-tabulate: out of memory");
        return CH_UNDEFINED;
    }
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        ChValue idx = ch_make_fixnum((int64_t)i);
        ChValue chv = CH_UNDEFINED;
        ChVMStatus st = ch_vm_apply(vm, args[0], &idx, 1, &chv);
        if (st != CH_VM_OK) {
            free(buf);
            return CH_UNDEFINED;
        }
        chv = ch_coerce_single(chv);
        if (!ch_is_char(chv)) {
            free(buf);
            snprintf(vm->error, sizeof(vm->error), "string-tabulate: expected char");
            return CH_UNDEFINED;
        }
        char tmp[4];
        size_t n = 0;
        if (!ch_utf8_encode_codepoint(ch_to_char(chv), tmp, &n)) {
            free(buf);
            snprintf(vm->error, sizeof(vm->error), "string-tabulate: invalid character");
            return CH_UNDEFINED;
        }
        if (pos + n >= cap) {
            cap = (cap + n) * 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                snprintf(vm->error, sizeof(vm->error), "string-tabulate: out of memory");
                return CH_UNDEFINED;
            }
            buf = nb;
        }
        memcpy(buf + pos, tmp, n);
        pos += n;
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf, pos);
    free(buf);
    return out;
}

/* string-every char/char-set/pred s [start end] -- only procedure predicates
   are supported, matching string-index/string-skip's existing simplification
   in this file (no char-set type yet). Returns the last non-#f predicate
   result, or #t for an empty range. */
static ChValue prim_string_every(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || !ch_is_procedure(args[0]) || !ch_is_string(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-every: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[1]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, "string-every", &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = cp_len;
    if (parse_optional_range(vm, args, nargs, 2, cp_len, "string-every", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    ChValue last = CH_TRUE;
    size_t cp_idx = 0;
    size_t pos = 0;
    while (pos < s->len && cp_idx < end) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            snprintf(vm->error, sizeof(vm->error), "string-every: invalid UTF-8");
            return CH_UNDEFINED;
        }
        if (cp_idx >= start) {
            ChValue arg = ch_make_char(cp);
            ChValue result = CH_UNDEFINED;
            ChVMStatus st = ch_vm_apply(vm, args[0], &arg, 1, &result);
            if (st == CH_VM_CONTINUATION_INVOKED) {
                vm->continuation_invoked = true;
                return CH_UNDEFINED;
            }
            if (st != CH_VM_OK) {
                return CH_UNDEFINED;
            }
            if (result == CH_FALSE || ch_is_false(result)) {
                return CH_FALSE;
            }
            last = result;
        }
        pos = next;
        cp_idx++;
    }
    return last;
}

/* string-any char/char-set/pred s [start end] -- procedure predicates only,
   same simplification as string-every above. */
static ChValue prim_string_any(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || !ch_is_procedure(args[0]) || !ch_is_string(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-any: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[1]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, "string-any", &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = cp_len;
    if (parse_optional_range(vm, args, nargs, 2, cp_len, "string-any", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t cp_idx = 0;
    size_t pos = 0;
    while (pos < s->len && cp_idx < end) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            snprintf(vm->error, sizeof(vm->error), "string-any: invalid UTF-8");
            return CH_UNDEFINED;
        }
        if (cp_idx >= start) {
            ChValue arg = ch_make_char(cp);
            ChValue result = CH_UNDEFINED;
            ChVMStatus st = ch_vm_apply(vm, args[0], &arg, 1, &result);
            if (st == CH_VM_CONTINUATION_INVOKED) {
                vm->continuation_invoked = true;
                return CH_UNDEFINED;
            }
            if (st != CH_VM_OK) {
                return CH_UNDEFINED;
            }
            if (result != CH_FALSE && !ch_is_false(result)) {
                return result;
            }
        }
        pos = next;
        cp_idx++;
    }
    return CH_FALSE;
}

/* string-count s char/char-set/pred [start end] -- note the string comes
   first here, unlike string-every/any/filter/delete (per SRFI-13). */
static ChValue prim_string_count(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || !ch_is_string(args[0]) || !ch_is_procedure(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "string-count: bad arguments");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, "string-count", &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = cp_len;
    if (parse_optional_range(vm, args, nargs, 2, cp_len, "string-count", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    int64_t count = 0;
    size_t cp_idx = 0;
    size_t pos = 0;
    while (pos < s->len && cp_idx < end) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            snprintf(vm->error, sizeof(vm->error), "string-count: invalid UTF-8");
            return CH_UNDEFINED;
        }
        if (cp_idx >= start) {
            bool match = false;
            if (call_pred_char(vm, args[1], cp, &match, "string-count") != 0) {
                return CH_UNDEFINED;
            }
            if (match) {
                count++;
            }
        }
        pos = next;
        cp_idx++;
    }
    return ch_make_fixnum(count);
}

/* string-filter/string-delete share this walk: keep (or drop) codepoints
   for which `pred` is true. */
static ChValue string_filter_impl(ChVM *vm, ChValue *args, int nargs, const char *who,
                                  bool keep_on_match) {
    if (nargs < 2 || !ch_is_procedure(args[0]) || !ch_is_string(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "%s: bad arguments", who);
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[1]);
    size_t cp_len = 0;
    if (utf8_count_codepoints(vm, s, who, &cp_len) != 0) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = cp_len;
    if (parse_optional_range(vm, args, nargs, 2, cp_len, who, &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    size_t cp_idx = 0;
    size_t pos = 0;
    while (pos < s->len && cp_idx < end) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
            free(buf);
            snprintf(vm->error, sizeof(vm->error), "%s: invalid UTF-8", who);
            return CH_UNDEFINED;
        }
        if (cp_idx >= start) {
            bool match = false;
            if (call_pred_char(vm, args[0], cp, &match, who) != 0) {
                free(buf);
                return CH_UNDEFINED;
            }
            if (match == keep_on_match) {
                if (!append_bytes(&buf, &len, &cap, s->data + pos, next - pos)) {
                    free(buf);
                    abort();
                }
            }
        }
        pos = next;
        cp_idx++;
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", len);
    free(buf);
    return out;
}

static ChValue prim_string_filter(ChVM *vm, ChValue *args, int nargs) {
    return string_filter_impl(vm, args, nargs, "string-filter", true);
}

static ChValue prim_string_delete(ChVM *vm, ChValue *args, int nargs) {
    return string_filter_impl(vm, args, nargs, "string-delete", false);
}

void ch_register_srfi13_primitives(ChVM *vm) {
    define_prim(vm, "string-null?", prim_string_null_p, 1, 1);
    define_prim(vm, "string-contains", prim_string_contains, -1, 2);
    define_prim(vm, "string-prefix?", prim_string_prefix_p, -1, 2);
    define_prim(vm, "string-suffix?", prim_string_suffix_p, -1, 2);
    define_prim(vm, "string-concatenate", prim_string_concatenate, 1, 1);
    define_prim(vm, "string-unfold", prim_string_unfold, -1, 4);
    define_prim(vm, "string-unfold-right", prim_string_unfold_right, -1, 4);
    define_prim(vm, "string-index-right", prim_string_index_right, -1, 2);
    define_prim(vm, "string-skip", prim_string_skip, -1, 2);
    define_prim(vm, "string-skip-right", prim_string_skip_right, -1, 2);
    define_prim(vm, "string-index", prim_string_index, -1, 2);
    define_prim(vm, "string-take", prim_string_take, 2, 2);
    define_prim(vm, "string-drop", prim_string_drop, 2, 2);
    define_prim(vm, "string-trim", prim_string_trim, -1, 1);
    define_prim(vm, "string-trim-right", prim_string_trim_right, -1, 1);
    define_prim(vm, "string-trim-both", prim_string_trim_both, -1, 1);
    define_prim(vm, "string-take-right", prim_string_take_right, 2, 2);
    define_prim(vm, "string-drop-right", prim_string_drop_right, 2, 2);
    define_prim(vm, "string-pad", prim_string_pad, -1, 2);
    define_prim(vm, "string-pad-right", prim_string_pad_right, -1, 2);
    define_prim(vm, "string-replace", prim_string_replace, -1, 4);
    define_prim(vm, "string-titlecase", prim_string_titlecase, -1, 1);
    define_prim(vm, "string-join", prim_string_join, -1, 1);
    define_prim(vm, "string-split", prim_string_split, 2, 2);
    define_prim(vm, "string-tabulate", prim_string_tabulate, 2, 2);
    define_prim(vm, "string-every", prim_string_every, -1, 2);
    define_prim(vm, "string-any", prim_string_any, -1, 2);
    define_prim(vm, "string-count", prim_string_count, -1, 2);
    define_prim(vm, "string-filter", prim_string_filter, -1, 2);
    define_prim(vm, "string-delete", prim_string_delete, -1, 2);
}
