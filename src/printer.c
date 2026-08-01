#include "chaaya/printer.h"

#include <stdio.h>

static void print_string_write(FILE *out, const ChString *s) {
    fputc('"', out);
    for (size_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        switch (c) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        default:
            fputc(c, out);
            break;
        }
    }
    fputc('"', out);
}

static void print_value_rec(FILE *out, ChValue v, bool display);

static void print_list(FILE *out, ChValue v, bool display) {
    fputc('(', out);
    bool first = true;
    while (ch_is_pair(v)) {
        if (!first) {
            fputc(' ', out);
        }
        first = false;
        print_value_rec(out, ch_car(v), display);
        v = ch_cdr(v);
    }
    if (!ch_is_nil(v)) {
        fputs(" . ", out);
        print_value_rec(out, v, display);
    }
    fputc(')', out);
}

static void print_value_rec(FILE *out, ChValue v, bool display) {
    if (ch_is_nil(v)) {
        fputs("()", out);
        return;
    }
    if (v == CH_TRUE) {
        fputs("#t", out);
        return;
    }
    if (v == CH_FALSE) {
        fputs("#f", out);
        return;
    }
    if (v == CH_VOID) {
        fputs("#<void>", out);
        return;
    }
    if (v == CH_EOF_OBJ) {
        fputs("#<eof>", out);
        return;
    }
    if (v == CH_UNDEFINED) {
        fputs("#<undefined>", out);
        return;
    }
    if (ch_is_char(v)) {
        uint32_t cp = ch_to_char(v);
        if (display) {
            if (cp < 128) {
                fputc((int)cp, out);
            } else {
                fprintf(out, "\\u%04X", cp);
            }
        } else {
            if (cp == '\n') {
                fputs("#\\newline", out);
            } else if (cp == ' ') {
                fputs("#\\space", out);
            } else if (cp == '\t') {
                fputs("#\\tab", out);
            } else if (cp < 128) {
                fprintf(out, "#\\%c", (char)cp);
            } else {
                fprintf(out, "#\\x%X", cp);
            }
        }
        return;
    }
    if (ch_is_fixnum(v)) {
        fprintf(out, "%lld", (long long)ch_to_fixnum(v));
        return;
    }
    if (ch_is_flonum(v)) {
        fprintf(out, "%.16g", ch_to_flonum(v));
        return;
    }
    if (ch_is_pair(v)) {
        print_list(out, v, display);
        return;
    }
    if (ch_is_symbol(v)) {
        fputs(ch_as_symbol(v)->name, out);
        return;
    }
    if (ch_is_string(v)) {
        ChString *s = ch_as_string(v);
        if (display) {
            fwrite(s->data, 1, s->len, out);
        } else {
            print_string_write(out, s);
        }
        return;
    }
    if (ch_is_vector(v)) {
        ChVector *vec = ch_as_vector(v);
        fputs("#(", out);
        for (size_t i = 0; i < vec->len; i++) {
            if (i > 0) {
                fputc(' ', out);
            }
            print_value_rec(out, vec->items[i], display);
        }
        fputc(')', out);
        return;
    }
    if (ch_is_closure(v)) {
        fputs("#<closure>", out);
        return;
    }
    if (ch_is_native(v)) {
        fprintf(out, "#<native %s>", ch_as_native(v)->name);
        return;
    }
    if (ch_is_function(v)) {
        fputs("#<function>", out);
        return;
    }
    fputs("#<unknown>", out);
}

void ch_print_value(FILE *out, ChValue v, bool display) {
    print_value_rec(out, v, display);
}

char *ch_value_to_string(ChValue v, bool display) {
    char *buf = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&buf, &size);
    if (!mem) {
        return NULL;
    }
    print_value_rec(mem, v, display);
    fclose(mem);
    return buf;
}
