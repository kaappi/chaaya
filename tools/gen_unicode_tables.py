#!/usr/bin/env python3
"""Generate C Unicode case/property tables for Chaaya from Unicode 15.1 data.

Uses Kaappi's cached UCD files when available (../kaappi/tools/.cache/).
"""

import os
import sys
import urllib.request

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
KAAPI_CACHE = os.path.join(REPO_ROOT, "..", "kaappi", "tools", ".cache")
LOCAL_CACHE = os.path.join(SCRIPT_DIR, ".cache")

UNICODE_DATA_URL = "https://www.unicode.org/Public/15.1.0/ucd/UnicodeData.txt"
CASE_FOLDING_URL = "https://www.unicode.org/Public/15.1.0/ucd/CaseFolding.txt"
DERIVED_PROPS_URL = "https://www.unicode.org/Public/15.1.0/ucd/DerivedCoreProperties.txt"


def cache_dir():
    for d in (KAAPI_CACHE, LOCAL_CACHE):
        if os.path.isdir(d):
            return d
    os.makedirs(LOCAL_CACHE, exist_ok=True)
    return LOCAL_CACHE


def download(url, path):
    if os.path.exists(path):
        return
    print(f"Downloading {url}...", file=sys.stderr)
    urllib.request.urlretrieve(url, path)


def parse_unicode_data(path):
    upcase_map = {}
    downcase_map = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            fields = line.strip().split(";")
            if len(fields) < 15:
                continue
            cp = int(fields[0], 16)
            simple_upper = fields[12].strip()
            simple_lower = fields[13].strip()
            if simple_upper:
                upcase_map[cp] = int(simple_upper, 16)
            if simple_lower:
                downcase_map[cp] = int(simple_lower, 16)
    return upcase_map, downcase_map


def parse_case_folding(path):
    fold_map = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split(";")
            if len(fields) < 3:
                continue
            status = fields[1].strip()
            if status not in ("C", "S"):
                continue
            cp = int(fields[0].strip(), 16)
            folded = int(fields[2].strip().split()[0], 16)
            fold_map[cp] = folded
    return fold_map


def parse_derived_core_properties(path):
    props = {"Uppercase": [], "Lowercase": [], "Alphabetic": [], "Cased": []}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split(";")
            if len(fields) < 2:
                continue
            prop = fields[1].split("#")[0].strip()
            if prop not in props:
                continue
            cp_field = fields[0].strip()
            if ".." in cp_field:
                lo, hi = cp_field.split("..")
                props[prop].append((int(lo, 16), int(hi, 16)))
            else:
                cp = int(cp_field, 16)
                props[prop].append((cp, cp))
    for prop in props:
        ranges = sorted(props[prop])
        merged = []
        for lo, hi in ranges:
            if merged and lo <= merged[-1][1] + 1:
                merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
            else:
                merged.append((lo, hi))
        props[prop] = merged
    return props


def fmt(cp):
    return f"0x{cp:04X}u"


def write_case_table(out, name, entries):
    out.append(f"static const ChUnicodeCaseEntry {name}[] = {{")
    for key, val in entries:
        out.append(f"    {{ {fmt(key)}, {fmt(val)} }},")
    out.append("};")
    out.append(f"static const size_t {name}_len = sizeof({name}) / sizeof({name}[0]);")
    out.append("")


def write_range_table(out, name, ranges):
    out.append(f"static const ChUnicodeRange {name}[] = {{")
    for lo, hi in ranges:
        out.append(f"    {{ {fmt(lo)}, {fmt(hi)} }},")
    out.append("};")
    out.append(f"static const size_t {name}_len = sizeof({name}) / sizeof({name}[0]);")
    out.append("")


def write_search_fn(out, fn_name, table_name):
    out.append(f"uint32_t {fn_name}(uint32_t cp) {{")
    out.append(f"    size_t lo = 0;")
    out.append(f"    size_t hi = {table_name}_len;")
    out.append(f"    while (lo < hi) {{")
    out.append(f"        size_t mid = lo + (hi - lo) / 2;")
    out.append(f"        if ({table_name}[mid].from_cp == cp) {{")
    out.append(f"            return {table_name}[mid].to_cp;")
    out.append(f"        }}")
    out.append(f"        if ({table_name}[mid].from_cp < cp) {{")
    out.append(f"            lo = mid + 1;")
    out.append(f"        }} else {{")
    out.append(f"            hi = mid;")
    out.append(f"        }}")
    out.append(f"    }}")
    out.append(f"    return cp;")
    out.append(f"}}")
    out.append("")


def write_in_ranges(out):
    out.append("bool ch_unicode_in_ranges(const ChUnicodeRange *table, size_t len, uint32_t cp) {")
    out.append("    size_t lo = 0;")
    out.append("    size_t hi = len;")
    out.append("    while (lo < hi) {")
    out.append("        size_t mid = lo + (hi - lo) / 2;")
    out.append("        if (cp < table[mid].lo) {")
    out.append("            hi = mid;")
    out.append("        } else if (cp > table[mid].hi) {")
    out.append("            lo = mid + 1;")
    out.append("        } else {")
    out.append("            return true;")
    out.append("        }")
    out.append("    }")
    out.append("    return false;")
    out.append("}")
    out.append("")


def generate_c(upcase_map, downcase_map, fold_map, derived_props):
    lines = []
    lines.append("/* Auto-generated from Unicode 15.1 — do not edit by hand. */")
    lines.append("#include \"chaaya/unicode.h\"")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    uint32_t from_cp;")
    lines.append("    uint32_t to_cp;")
    lines.append("} ChUnicodeCaseEntry;")
    lines.append("")

    upcase_sorted = sorted(upcase_map.items())
    downcase_sorted = sorted(downcase_map.items())

    fold_extra = {}
    for cp, folded in sorted(fold_map.items()):
        if cp <= 0x7F:
            continue
        if cp in downcase_map and downcase_map[cp] == folded:
            continue
        fold_extra[cp] = folded
    fold_sorted = sorted(fold_extra.items())

    write_case_table(lines, "upcase_table", upcase_sorted)
    write_case_table(lines, "downcase_table", downcase_sorted)
    write_case_table(lines, "fold_table", fold_sorted)

    write_range_table(lines, "uppercase_ranges", derived_props["Uppercase"])
    write_range_table(lines, "lowercase_ranges", derived_props["Lowercase"])
    write_range_table(lines, "alphabetic_ranges", derived_props["Alphabetic"])
    write_range_table(lines, "cased_ranges", derived_props["Cased"])

    lines.append("uint32_t ch_unicode_upcase(uint32_t cp) {")
    lines.append("    if (cp <= 0x7Fu) {")
    lines.append("        return (uint32_t)((cp >= 'a' && cp <= 'z') ? cp - 32u : cp);")
    lines.append("    }")
    lines.append("    return ch_unicode_lookup_upcase(cp);")
    lines.append("}")
    lines.append("")

    lines.append("uint32_t ch_unicode_downcase(uint32_t cp) {")
    lines.append("    if (cp <= 0x7Fu) {")
    lines.append("        return (uint32_t)((cp >= 'A' && cp <= 'Z') ? cp + 32u : cp);")
    lines.append("    }")
    lines.append("    return ch_unicode_lookup_downcase(cp);")
    lines.append("}")
    lines.append("")

    lines.append("uint32_t ch_unicode_foldcase(uint32_t cp) {")
    lines.append("    if (cp <= 0x7Fu) {")
    lines.append("        return (uint32_t)((cp >= 'A' && cp <= 'Z') ? cp + 32u : cp);")
    lines.append("    }")
    lines.append("    {")
    lines.append("        uint32_t folded = ch_unicode_lookup_fold(cp);")
    lines.append("        if (folded != cp) {")
    lines.append("            return folded;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return ch_unicode_lookup_downcase(cp);")
    lines.append("}")
    lines.append("")

    write_search_fn(lines, "ch_unicode_lookup_upcase", "upcase_table")
    write_search_fn(lines, "ch_unicode_lookup_downcase", "downcase_table")
    write_search_fn(lines, "ch_unicode_lookup_fold", "fold_table")
    write_in_ranges(lines)

    lines.append("bool ch_unicode_is_uppercase(uint32_t cp) {")
    lines.append("    if (cp <= 0x7Fu) {")
    lines.append("        return cp >= 'A' && cp <= 'Z';")
    lines.append("    }")
    lines.append("    return ch_unicode_in_ranges(uppercase_ranges, uppercase_ranges_len, cp);")
    lines.append("}")
    lines.append("")

    lines.append("bool ch_unicode_is_lowercase(uint32_t cp) {")
    lines.append("    if (cp <= 0x7Fu) {")
    lines.append("        return cp >= 'a' && cp <= 'z';")
    lines.append("    }")
    lines.append("    return ch_unicode_in_ranges(lowercase_ranges, lowercase_ranges_len, cp);")
    lines.append("}")
    lines.append("")

    lines.append("bool ch_unicode_is_alphabetic(uint32_t cp) {")
    lines.append("    if (cp <= 0x7Fu) {")
    lines.append("        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');")
    lines.append("    }")
    lines.append("    return ch_unicode_in_ranges(alphabetic_ranges, alphabetic_ranges_len, cp);")
    lines.append("}")
    lines.append("")

    lines.append("bool ch_unicode_is_cased(uint32_t cp) {")
    lines.append("    if (cp <= 0x7Fu) {")
    lines.append("        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');")
    lines.append("    }")
    lines.append("    return ch_unicode_in_ranges(cased_ranges, cased_ranges_len, cp);")
    lines.append("}")
    lines.append("")

    lines.append("bool ch_unicode_is_whitespace(uint32_t cp) {")
    lines.append("    switch (cp) {")
    lines.append("    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:")
    lines.append("        return true;")
    lines.append("    case 0x85: case 0xA0: case 0x1680:")
    lines.append("    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:")
    lines.append("    case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:")
    lines.append("    case 0x200A: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:")
    lines.append("        return true;")
    lines.append("    default:")
    lines.append("        return false;")
    lines.append("    }")
    lines.append("}")
    lines.append("")

    lines.append("bool ch_unicode_is_numeric(uint32_t cp) {")
    lines.append("    if (cp >= '0' && cp <= '9') {")
    lines.append("        return true;")
    lines.append("    }")
    lines.append("    static const uint32_t digit_zeros[] = {")
    zeros = [
        0x0660, 0x06F0, 0x07C0, 0x0966, 0x09E6, 0x0A66, 0x0AE6, 0x0B66,
        0x0BE6, 0x0C66, 0x0CE6, 0x0D66, 0x0DE6, 0x0E50, 0x0ED0, 0x0F20,
        0x1040, 0x1090, 0x17E0, 0x1810, 0x1946, 0x19D0, 0x1A80, 0x1A90,
        0x1B50, 0x1BB0, 0x1C40, 0x1C50, 0xA620, 0xA8D0, 0xA900, 0xA9D0,
        0xA9F0, 0xAA50, 0xABF0, 0xFF10,
    ]
    for z in zeros:
        lines.append(f"        {fmt(z)},")
    lines.append("    };")
    lines.append("    for (size_t i = 0; i < sizeof(digit_zeros) / sizeof(digit_zeros[0]); i++) {")
    lines.append("        uint32_t zero = digit_zeros[i];")
    lines.append("        if (cp >= zero && cp <= zero + 9u) {")
    lines.append("            return true;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")

    lines.append("int ch_unicode_digit_value(uint32_t cp) {")
    lines.append("    if (cp >= '0' && cp <= '9') {")
    lines.append("        return (int)(cp - '0');")
    lines.append("    }")
    lines.append("    static const uint32_t digit_zeros[] = {")
    for z in zeros:
        lines.append(f"        {fmt(z)},")
    lines.append("    };")
    lines.append("    for (size_t i = 0; i < sizeof(digit_zeros) / sizeof(digit_zeros[0]); i++) {")
    lines.append("        uint32_t zero = digit_zeros[i];")
    lines.append("        if (cp >= zero && cp <= zero + 9u) {")
    lines.append("            return (int)(cp - zero);")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return -1;")
    lines.append("}")
    lines.append("")

    return "\n".join(lines) + "\n"


def main():
    cdir = cache_dir()
    udata_path = os.path.join(cdir, "UnicodeData.txt")
    cfold_path = os.path.join(cdir, "CaseFolding.txt")
    dprops_path = os.path.join(cdir, "DerivedCoreProperties.txt")

    download(UNICODE_DATA_URL, udata_path)
    download(CASE_FOLDING_URL, cfold_path)
    download(DERIVED_PROPS_URL, dprops_path)

    upcase_map, downcase_map = parse_unicode_data(udata_path)
    fold_map = parse_case_folding(cfold_path)
    derived_props = parse_derived_core_properties(dprops_path)

    out_path = os.path.join(REPO_ROOT, "src", "unicode_tables.c")
    header_path = os.path.join(REPO_ROOT, "include", "chaaya", "unicode.h")

    c_src = generate_c(upcase_map, downcase_map, fold_map, derived_props)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(c_src)

    header = """/* Unicode case/property helpers for R7RS char/string procedures. */
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
"""
    with open(header_path, "w", encoding="utf-8") as f:
        f.write(header)

    print(f"Wrote {out_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
