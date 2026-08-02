#include "prim_utf8.h"

int ch_utf8_count_codepoints(ChVM *vm, const ChString *s, const char *who, size_t *count_out) {
    size_t pos = 0;
    size_t count = 0;
    while (pos < s->len) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!ch_utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
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

int ch_utf8_find_codepoint(ChVM *vm, const ChString *s, size_t index, const char *who,
                           size_t *start_out, size_t *end_out, uint32_t *cp_out) {
    size_t pos = 0;
    size_t i = 0;
    while (pos < s->len) {
        uint32_t cp = 0;
        size_t next = 0;
        if (!ch_utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
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

int ch_utf8_offset_for_index(ChVM *vm, const ChString *s, size_t index, const char *who,
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
        if (!ch_utf8_decode_next(s->data, s->len, pos, &cp, &next)) {
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

bool ch_utf8_append_codepoint(char **buf, size_t *len, size_t *cap, uint32_t cp) {
    char encoded[4];
    size_t encoded_len = 0;
    if (!ch_utf8_encode_codepoint(cp, encoded, &encoded_len)) {
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
