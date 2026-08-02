#include "chaaya/prim.h"
#include "chaaya/unicode.h"

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
    if (pat->len == 0) {
        return ch_make_fixnum(0);
    }
    if (pat->len > s->len) {
        return CH_FALSE;
    }
    for (size_t i = 0; i + pat->len <= s->len; i++) {
        if (memcmp(s->data + i, pat->data, pat->len) == 0) {
            size_t cp_idx = 0;
            size_t pos = 0;
            while (pos < i) {
                uint32_t cp = 0;
                size_t next = 0;
                if (!utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
                    break;
                }
                pos = next;
                cp_idx++;
            }
            return ch_make_fixnum((int64_t)cp_idx);
        }
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
}
