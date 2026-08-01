/* Unicode case/property helpers for R7RS char/string procedures. */
#ifndef CHAAYA_UNICODE_H
#define CHAAYA_UNICODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t lo;
    uint32_t hi;
} ChUnicodeRange;

uint32_t ch_unicode_upcase(uint32_t cp);
uint32_t ch_unicode_downcase(uint32_t cp);
uint32_t ch_unicode_foldcase(uint32_t cp);
uint32_t ch_unicode_lookup_upcase(uint32_t cp);
uint32_t ch_unicode_lookup_downcase(uint32_t cp);
uint32_t ch_unicode_lookup_fold(uint32_t cp);
bool ch_unicode_in_ranges(const ChUnicodeRange *table, size_t len, uint32_t cp);
bool ch_unicode_is_uppercase(uint32_t cp);
bool ch_unicode_is_lowercase(uint32_t cp);
bool ch_unicode_is_alphabetic(uint32_t cp);
bool ch_unicode_is_cased(uint32_t cp);
bool ch_unicode_is_whitespace(uint32_t cp);
bool ch_unicode_is_numeric(uint32_t cp);
int ch_unicode_digit_value(uint32_t cp);

typedef struct {
    uint32_t cps[3];
    size_t len;
} ChUnicodeFoldExpansion;

ChUnicodeFoldExpansion ch_unicode_fold_expand(uint32_t cp);
int ch_unicode_fold_compare_strings(const char *a, size_t alen, const char *b, size_t blen);

#endif
