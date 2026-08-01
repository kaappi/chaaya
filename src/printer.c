#include "chaaya/printer.h"

#include "chaaya/bignum.h"
#include "chaaya/complex.h"
#include "chaaya/ffi.h"
#include "chaaya/fiber.h"
#include "chaaya/rational.h"

#include <stdio.h>
#include <stdlib.h>

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
    if (ch_is_bignum(v)) {
        char *s = ch_bignum_to_string(v);
        fputs(s, out);
        free(s);
        return;
    }
    if (ch_is_rational_obj(v)) {
        char *s = ch_exact_to_string(v);
        fputs(s, out);
        free(s);
        return;
    }
    if (ch_is_complex_obj(v)) {
        char *s = ch_complex_to_string(v);
        fputs(s, out);
        free(s);
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
    if (ch_is_bytevector(v)) {
        ChBytevector *bv = ch_as_bytevector(v);
        fputs("#u8(", out);
        for (size_t i = 0; i < bv->len; i++) {
            if (i > 0) {
                fputc(' ', out);
            }
            fprintf(out, "%u", (unsigned)bv->data[i]);
        }
        fputc(')', out);
        return;
    }
    if (ch_is_record(v)) {
        ChRecord *r = ch_as_record(v);
        const char *nm = "?";
        if (r->rtype && ch_is_string(r->rtype->name)) {
            nm = ch_as_string(r->rtype->name)->data;
        }
        fprintf(out, "#<%s>", nm);
        return;
    }
    if (ch_is_record_type(v)) {
        ChRecordType *rt = ch_as_record_type(v);
        const char *nm = ch_is_string(rt->name) ? ch_as_string(rt->name)->data : "?";
        fprintf(out, "#<record-type %s>", nm);
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
    if (ch_is_continuation(v)) {
        fputs("#<continuation>", out);
        return;
    }
    if (ch_is_values(v)) {
        fputs("#<values>", out);
        return;
    }
    if (ch_is_port(v)) {
        fputs("#<port>", out);
        return;
    }
    if (ch_is_fiber(v)) {
        ChFiber *fiber = ch_as_fiber(v);
        const char *state = "ready";
        if (fiber->state == CH_FIBER_RUNNING) {
            state = "running";
        } else if (fiber->state == CH_FIBER_DONE) {
            state = "done";
        } else if (fiber->state == CH_FIBER_FAILED) {
            state = "failed";
        }
        fprintf(out, "#<fiber %llu %s>", (unsigned long long)fiber->id, state);
        return;
    }
    if (ch_is_channel(v)) {
        ChChannel *channel = ch_as_channel(v);
        if (channel->capacity == 0) {
            fprintf(out, "#<channel %zu/unbounded>", channel->count);
        } else {
            fprintf(out, "#<channel %zu/%zu>", channel->count, channel->capacity);
        }
        return;
    }
    if (ch_is_foreign_library(v)) {
        ChForeignLibrary *lib = ch_as_foreign_library(v);
        fputs(lib->closed ? "#<foreign-library closed>" : "#<foreign-library>", out);
        return;
    }
    if (ch_is_foreign_procedure(v)) {
        ChForeignProcedure *proc = ch_as_foreign_procedure(v);
        if (ch_is_string(proc->name)) {
            fprintf(out, "#<foreign-procedure %s>", ch_as_string(proc->name)->data);
        } else {
            fputs("#<foreign-procedure>", out);
        }
        return;
    }
    if (ch_is_promise(v)) {
        fputs("#<promise>", out);
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
