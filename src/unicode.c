#include "chaaya/unicode.h"

#include <ctype.h>
#include <string.h>

ChUnicodeFoldExpansion ch_unicode_fold_expand(uint32_t cp) {
    ChUnicodeFoldExpansion out = {.cps = {0, 0, 0}, .len = 1};
    switch (cp) {
    case 0x00DF:
        out.cps[0] = 's';
        out.cps[1] = 's';
        out.len = 2;
        return out;
    case 0x0130:
        out.cps[0] = 0x0069;
        out.cps[1] = 0x0307;
        out.len = 2;
        return out;
    case 0x01F0:
        out.cps[0] = 'j';
        out.cps[1] = 0x030C;
        out.len = 2;
        return out;
    case 0x0390:
        out.cps[0] = 0x03B9;
        out.cps[1] = 0x0308;
        out.cps[2] = 0x0301;
        out.len = 3;
        return out;
    case 0x03B0:
        out.cps[0] = 0x03C5;
        out.cps[1] = 0x0308;
        out.cps[2] = 0x0301;
        out.len = 3;
        return out;
    case 0xFB00:
        out.cps[0] = 'f';
        out.cps[1] = 'f';
        out.len = 2;
        return out;
    case 0xFB01:
        out.cps[0] = 'f';
        out.cps[1] = 'i';
        out.len = 2;
        return out;
    case 0xFB02:
        out.cps[0] = 'f';
        out.cps[1] = 'l';
        out.len = 2;
        return out;
    case 0xFB03:
        out.cps[0] = 'f';
        out.cps[1] = 'f';
        out.cps[2] = 'i';
        out.len = 3;
        return out;
    case 0xFB04:
        out.cps[0] = 'f';
        out.cps[1] = 'f';
        out.cps[2] = 'l';
        out.len = 3;
        return out;
    case 0xFB05:
    case 0xFB06:
        out.cps[0] = 's';
        out.cps[1] = 't';
        out.len = 2;
        return out;
    default:
        out.cps[0] = ch_unicode_foldcase(cp);
        out.len = 1;
        return out;
    }
}

static bool utf8_decode(const char *bytes, size_t len, size_t pos, uint32_t *cp_out,
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
    *cp_out = b0;
    *next_out = pos + 1;
    return true;
}

int ch_unicode_fold_compare_strings(const char *a, size_t alen, const char *b, size_t blen) {
    size_t ai = 0;
    size_t bi = 0;
    ChUnicodeFoldExpansion a_buf = {0};
    ChUnicodeFoldExpansion b_buf = {0};
    size_t a_idx = 0;
    size_t b_idx = 0;

    for (;;) {
        if (a_idx >= a_buf.len) {
            if (ai >= alen) {
                if (b_idx >= b_buf.len && bi >= blen) {
                    return 0;
                }
                return -1;
            }
            uint32_t cp = 0;
            size_t next = 0;
            (void)utf8_decode(a, alen, ai, &cp, &next);
            a_buf = ch_unicode_fold_expand(cp);
            a_idx = 0;
            ai = next;
        }
        if (b_idx >= b_buf.len) {
            if (bi >= blen) {
                return 1;
            }
            uint32_t cp = 0;
            size_t next = 0;
            (void)utf8_decode(b, blen, bi, &cp, &next);
            b_buf = ch_unicode_fold_expand(cp);
            b_idx = 0;
            bi = next;
        }
        uint32_t fa = a_buf.cps[a_idx++];
        uint32_t fb = b_buf.cps[b_idx++];
        if (fa < fb) {
            return -1;
        }
        if (fa > fb) {
            return 1;
        }
    }
}
