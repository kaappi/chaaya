#include "chaaya/prim.h"

#include "chaaya/unicode.h"

#include <stdio.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
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

void ch_register_char_primitives(ChVM *vm) {
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
}
