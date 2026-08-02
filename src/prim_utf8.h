/* Private UTF-8 helpers shared by char/string/vector primitives. */
#ifndef CHAAYA_PRIM_UTF8_H
#define CHAAYA_PRIM_UTF8_H

#include "chaaya/vm.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int ch_parse_nonnegative_index(ChVM *vm, ChValue v, size_t *out, const char *who) {
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

static inline int ch_parse_optional_range(ChVM *vm, ChValue *args, int nargs, int start_arg,
                                         size_t len, const char *who, size_t *start_out,
                                         size_t *end_out) {
    size_t start = 0;
    size_t end = len;
    if (nargs > start_arg) {
        if (ch_parse_nonnegative_index(vm, args[start_arg], &start, who) != 0) {
            return -1;
        }
    }
    if (nargs > start_arg + 1) {
        if (ch_parse_nonnegative_index(vm, args[start_arg + 1], &end, who) != 0) {
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

static inline bool ch_utf8_decode_next(const char *bytes, size_t len, size_t pos, uint32_t *cp_out,
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

static inline bool ch_utf8_encode_codepoint(uint32_t cp, char out[4], size_t *len_out) {
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

int ch_utf8_count_codepoints(ChVM *vm, const ChString *s, const char *who, size_t *count_out);
int ch_utf8_find_codepoint(ChVM *vm, const ChString *s, size_t index, const char *who,
                           size_t *start_out, size_t *end_out, uint32_t *cp_out);
int ch_utf8_offset_for_index(ChVM *vm, const ChString *s, size_t index, const char *who,
                             size_t *offset_out);
bool ch_utf8_append_codepoint(char **buf, size_t *len, size_t *cap, uint32_t cp);

#endif /* CHAAYA_PRIM_UTF8_H */
