#include "chaaya/printer.h"

#include "chaaya/bignum.h"
#include "chaaya/complex.h"
#include "chaaya/ffi.h"
#include "chaaya/fiber.h"
#include "chaaya/rational.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_MAX_PRINT_DEPTH 1024

typedef struct ChSharedState {
    ChValue *seen;
    size_t seen_count;
    size_t seen_cap;
    ChValue *shared;
    int32_t *labels;
    size_t shared_count;
    size_t shared_cap;
    int32_t next_label;
    uint32_t depth;
    ChValue *on_stack;
    size_t on_stack_count;
    size_t on_stack_cap;
    ChValue *done;
    size_t done_count;
    size_t done_cap;
    int oom;
} ChSharedState;

static int ensure_values_cap(ChValue **arr, size_t *cap, size_t need) {
    if (need <= *cap) {
        return 0;
    }
    size_t ncap = *cap ? *cap * 2 : 64;
    while (ncap < need) {
        ncap *= 2;
    }
    ChValue *n = (ChValue *)realloc(*arr, ncap * sizeof(ChValue));
    if (!n) {
        return -1;
    }
    *arr = n;
    *cap = ncap;
    return 0;
}

static int ensure_labels_cap(ChSharedState *st, size_t need) {
    if (need <= st->shared_cap) {
        return 0;
    }
    size_t ncap = st->shared_cap ? st->shared_cap * 2 : 64;
    while (ncap < need) {
        ncap *= 2;
    }
    ChValue *ns = (ChValue *)realloc(st->shared, ncap * sizeof(ChValue));
    int32_t *nl = (int32_t *)realloc(st->labels, ncap * sizeof(int32_t));
    if (!ns || !nl) {
        free(ns);
        free(nl);
        return -1;
    }
    st->shared = ns;
    st->labels = nl;
    st->shared_cap = ncap;
    return 0;
}

static void shared_state_free(ChSharedState *st) {
    free(st->seen);
    free(st->shared);
    free(st->labels);
    free(st->on_stack);
    free(st->done);
    memset(st, 0, sizeof(*st));
}

static void print_value_atom(FILE *out, ChValue v, bool display);
static void print_value_shared(FILE *out, ChValue v, ChSharedState *st, bool display);

static int starts_with_ci(const char *s, size_t n, const char *prefix) {
    size_t plen = strlen(prefix);
    if (n < plen) {
        return 0;
    }
    for (size_t i = 0; i < plen; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i])) {
            return 0;
        }
    }
    return 1;
}

/* Ensure Scheme inexact syntax: a '.' or 'e', and 'e+' for positive exponents. */
static int ensure_inexact_syntax(char *buf, int n, size_t cap) {
    int has_dot = 0;
    int e_pos = -1;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '.') {
            has_dot = 1;
        }
        if (buf[i] == 'e' || buf[i] == 'E') {
            e_pos = i;
            buf[i] = 'e';
        }
    }
    if (e_pos >= 0) {
        if (!has_dot && (size_t)n + 2 < cap) {
            memmove(buf + e_pos + 2, buf + e_pos, (size_t)(n - e_pos) + 1);
            buf[e_pos] = '.';
            buf[e_pos + 1] = '0';
            n += 2;
            e_pos += 2;
        }
        if (e_pos + 1 < n && buf[e_pos + 1] != '+' && buf[e_pos + 1] != '-' &&
            (size_t)n + 1 < cap) {
            memmove(buf + e_pos + 2, buf + e_pos + 1, (size_t)(n - e_pos));
            buf[e_pos + 1] = '+';
            n += 1;
        }
        return n;
    }
    if (!has_dot && (size_t)n + 2 < cap) {
        buf[n++] = '.';
        buf[n++] = '0';
        buf[n] = '\0';
    }
    return n;
}

/* R7RS: flonums must write with '.' or 'e' so they read back as inexact.
 * Prefer the shortest round-trip decimal (matches common Scheme printers). */
static void print_flonum(FILE *out, double f) {
    if (isnan(f)) {
        fputs("+nan.0", out);
        return;
    }
    if (isinf(f)) {
        fputs(f > 0 ? "+inf.0" : "-inf.0", out);
        return;
    }
    char best[128];
    int best_n = -1;
    double absf = fabs(f);
    int prefer_sci = (absf != 0.0 && (absf < 1e-10 || absf >= 1e21));

    /* Fixed-point first when in normal range: low-precision %g may emit sci
     * forms like 1e+02 for 100.0, which fail suite accept lists. */
    for (int pass = 0; pass < 2; pass++) {
        int use_sci = prefer_sci ? (pass == 0) : (pass == 1);
        /* prec=0 matters for subnormals: %e with prec 0 yields "5e-324"
         * (→ "5.0e-324") while prec 1 yields "4.9e-324", which is round-trip
         * correct but rejected by R7RS suite accept lists. */
        for (int prec = 0; prec <= 17; prec++) {
            char buf[128];
            int n = use_sci ? snprintf(buf, sizeof(buf), "%.*e", prec, f)
                            : snprintf(buf, sizeof(buf), "%.*g", prec, f);
            if (n < 0 || (size_t)n >= sizeof(buf)) {
                continue;
            }
            if (!use_sci) {
                int has_e = 0;
                for (int i = 0; i < n; i++) {
                    if (buf[i] == 'e' || buf[i] == 'E') {
                        has_e = 1;
                        break;
                    }
                }
                if (has_e) {
                    continue;
                }
            }
            char *end = NULL;
            double back = strtod(buf, &end);
            if (!end || *end != '\0') {
                continue;
            }
            if (memcmp(&back, &f, sizeof(double)) != 0) {
                continue;
            }
            n = ensure_inexact_syntax(buf, n, sizeof(buf));
            if (best_n < 0 || n < best_n) {
                memcpy(best, buf, (size_t)n + 1);
                best_n = n;
            }
            /* Shortest at this style: keep scanning for shorter higher prec? No —
             * higher prec only grows. Take first hit. */
            break;
        }
        /* If the preferred style succeeded, do not fall back to the other. */
        if (best_n >= 0 && ((prefer_sci && use_sci) || (!prefer_sci && !use_sci))) {
            break;
        }
    }
    if (best_n < 0) {
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "%.17g", f);
        if (n < 0 || (size_t)n >= sizeof(buf)) {
            fputs("+nan.0", out);
            return;
        }
        n = ensure_inexact_syntax(buf, n, sizeof(buf));
        fwrite(buf, 1, (size_t)n, out);
        return;
    }
    fwrite(best, 1, (size_t)best_n, out);
}

static int symbol_needs_bars(const char *name, size_t len) {
    if (len == 0) {
        return 1;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c <= ' ' || c == 0x7F || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
            c == '}' || c == '"' || c == ';' || c == '|' || c == '\\' || c == '\'' || c == '`' ||
            c == ',' || c == '#') {
            return 1;
        }
    }
    if (len == 1 && name[0] == '.') {
        return 1;
    }
    unsigned char c0 = (unsigned char)name[0];
    if (isdigit(c0)) {
        return 1;
    }
    if (c0 == '+' || c0 == '-') {
        if (len == 1) {
            return 0;
        }
        unsigned char c1 = (unsigned char)name[1];
        if (isdigit(c1)) {
            return 1;
        }
        if (c1 == '.' && len > 2 && isdigit((unsigned char)name[2])) {
            return 1;
        }
        if (len == 2 && (c1 == 'i' || c1 == 'I')) {
            return 1;
        }
        if (starts_with_ci(name + 1, len - 1, "inf.0") ||
            starts_with_ci(name + 1, len - 1, "nan.0")) {
            return 1;
        }
        return 0;
    }
    if (c0 == '.' && len > 1 && isdigit((unsigned char)name[1])) {
        return 1;
    }
    return 0;
}

static void print_symbol_write(FILE *out, const char *name, size_t len) {
    if (!symbol_needs_bars(name, len)) {
        fwrite(name, 1, len, out);
        return;
    }
    fputc('|', out);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '|' || c == '\\') {
            fputc('\\', out);
            fputc((int)c, out);
        } else if (c < 0x20 || c == 0x7F) {
            fprintf(out, "\\x%02x;", (unsigned)c);
        } else {
            fputc((int)c, out);
        }
    }
    fputc('|', out);
}

static int contains_value(const ChValue *arr, size_t n, ChValue v) {
    for (size_t i = 0; i < n; i++) {
        if (arr[i] == v) {
            return 1;
        }
    }
    return 0;
}

static int is_labelable(ChValue v) {
    return ch_is_pair(v) || ch_is_vector(v) || ch_is_record(v);
}

static int shared_index(ChSharedState *st, ChValue v) {
    for (size_t i = 0; i < st->shared_count; i++) {
        if (st->shared[i] == v) {
            return (int)i;
        }
    }
    return -1;
}

static void record_shared(ChSharedState *st, ChValue v) {
    if (shared_index(st, v) >= 0) {
        return;
    }
    if (ensure_labels_cap(st, st->shared_count + 1) != 0) {
        st->oom = 1;
        return;
    }
    st->shared[st->shared_count] = v;
    st->labels[st->shared_count] = -1;
    st->shared_count++;
}

static int32_t get_or_assign_label(ChSharedState *st, ChValue v) {
    int idx = shared_index(st, v);
    if (idx < 0) {
        return -1;
    }
    if (st->labels[idx] < 0) {
        st->labels[idx] = st->next_label++;
    }
    return st->labels[idx];
}

static int32_t peek_label(ChSharedState *st, ChValue v) {
    int idx = shared_index(st, v);
    if (idx < 0 || st->labels[idx] < 0) {
        return -1;
    }
    return st->labels[idx];
}

/* write-shared: mark every heap object referenced more than once. */
static void mark_shared(ChValue v, ChSharedState *st) {
    if (!is_labelable(v) || st->depth >= CH_MAX_PRINT_DEPTH || st->oom) {
        return;
    }
    st->depth++;
    if (contains_value(st->seen, st->seen_count, v)) {
        record_shared(st, v);
        st->depth--;
        return;
    }
    if (ensure_values_cap(&st->seen, &st->seen_cap, st->seen_count + 1) != 0) {
        st->oom = 1;
        st->depth--;
        return;
    }
    st->seen[st->seen_count++] = v;
    if (ch_is_pair(v)) {
        mark_shared(ch_car(v), st);
        mark_shared(ch_cdr(v), st);
    } else if (ch_is_vector(v)) {
        ChVector *vec = ch_as_vector(v);
        for (size_t i = 0; i < vec->len; i++) {
            mark_shared(vec->items[i], st);
        }
    } else if (ch_is_record(v)) {
        ChRecord *r = ch_as_record(v);
        for (size_t i = 0; i < r->num_fields; i++) {
            mark_shared(r->fields[i], st);
        }
    }
    st->depth--;
}

/* write/display: mark only objects on a cycle (DFS back-edges).
 * Pair spines are walked iteratively so flat lists of any length do not blow
 * the C stack (Kaappi markCycles). */
static void mark_cycles(ChValue v, ChSharedState *st);

static int push_value(ChValue **arr, size_t *count, size_t *cap, ChValue v, ChSharedState *st) {
    if (ensure_values_cap(arr, cap, *count + 1) != 0) {
        st->oom = 1;
        return -1;
    }
    (*arr)[(*count)++] = v;
    return 0;
}

static void mark_cycles(ChValue v, ChSharedState *st) {
    if (st->depth >= CH_MAX_PRINT_DEPTH || st->oom) {
        return;
    }
    if (!ch_is_pointer(v)) {
        return;
    }

    if (ch_is_vector(v) || ch_is_record(v)) {
        if (contains_value(st->on_stack, st->on_stack_count, v)) {
            record_shared(st, v);
            return;
        }
        if (contains_value(st->done, st->done_count, v)) {
            return;
        }
        if (push_value(&st->on_stack, &st->on_stack_count, &st->on_stack_cap, v, st) != 0) {
            return;
        }
        st->depth++;
        if (ch_is_vector(v)) {
            ChVector *vec = ch_as_vector(v);
            for (size_t i = 0; i < vec->len; i++) {
                mark_cycles(vec->items[i], st);
            }
        } else {
            ChRecord *r = ch_as_record(v);
            for (size_t i = 0; i < r->num_fields; i++) {
                mark_cycles(r->fields[i], st);
            }
        }
        st->on_stack_count--;
        (void)push_value(&st->done, &st->done_count, &st->done_cap, v, st);
        st->depth--;
        return;
    }

    if (!ch_is_pair(v)) {
        return;
    }

    /* Walk cdr spine iteratively; recurse only into cars / dotted tails. */
    size_t spine_start = st->on_stack_count;
    ChValue cur = v;
    while (ch_is_pair(cur)) {
        if (contains_value(st->on_stack, st->on_stack_count, cur)) {
            record_shared(st, cur);
            break;
        }
        if (contains_value(st->done, st->done_count, cur)) {
            break;
        }
        if (push_value(&st->on_stack, &st->on_stack_count, &st->on_stack_cap, cur, st) != 0) {
            break;
        }
        if (st->depth < CH_MAX_PRINT_DEPTH) {
            st->depth++;
            mark_cycles(ch_car(cur), st);
            st->depth--;
        }
        cur = ch_cdr(cur);
    }
    if (!ch_is_nil(cur) && !ch_is_pair(cur)) {
        mark_cycles(cur, st);
    }
    while (st->on_stack_count > spine_start) {
        ChValue node = st->on_stack[--st->on_stack_count];
        (void)push_value(&st->done, &st->done_count, &st->done_cap, node, st);
    }
}

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

static void print_list_shared(FILE *out, ChValue v, ChSharedState *st, bool display) {
    fputc('(', out);
    print_value_shared(out, ch_car(v), st, display);
    ChValue rest = ch_cdr(v);
    while (!ch_is_nil(rest)) {
        if (ch_is_pair(rest)) {
            if (shared_index(st, rest) >= 0) {
                fputs(" . ", out);
                print_value_shared(out, rest, st, display);
                break;
            }
            fputc(' ', out);
            print_value_shared(out, ch_car(rest), st, display);
            rest = ch_cdr(rest);
        } else {
            fputs(" . ", out);
            print_value_shared(out, rest, st, display);
            break;
        }
    }
    fputc(')', out);
}

static void print_value_with_depth(FILE *out, ChValue v, bool display, uint32_t depth);

static void print_list_simple(FILE *out, ChValue v, bool display, uint32_t depth) {
    fputc('(', out);
    bool first = true;
    while (ch_is_pair(v)) {
        if (!first) {
            fputc(' ', out);
        }
        first = false;
        print_value_with_depth(out, ch_car(v), display, depth + 1);
        v = ch_cdr(v);
    }
    if (!ch_is_nil(v)) {
        fputs(" . ", out);
        print_value_with_depth(out, v, display, depth + 1);
    }
    fputc(')', out);
}

static void print_value_atom(FILE *out, ChValue v, bool display) {
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
        if (display) {
            fprintf(out, "%.16g", ch_to_flonum(v));
        } else {
            print_flonum(out, ch_to_flonum(v));
        }
        return;
    }
    if (ch_is_pair(v) || ch_is_vector(v)) {
        /* Prefer print_value_with_depth for compound data. */
        print_value_with_depth(out, v, display, 0);
        return;
    }
    if (ch_is_symbol(v)) {
        ChSymbol *sym = ch_as_symbol(v);
        if (display) {
            fputs(sym->name, out);
        } else if (!ch_symbol_is_interned(sym)) {
            fprintf(out, "#<uninterned-symbol %s>", sym->name);
        } else {
            print_symbol_write(out, sym->name, strlen(sym->name));
        }
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
    if (ch_is_error_object(v)) {
        ChErrorObject *err = ch_as_error_object(v);
        fputs("#<error ", out);
        print_value_with_depth(out, err->message, display, 0);
        ChValue irr = err->irritants;
        while (!ch_is_nil(irr)) {
            fputc(' ', out);
            if (ch_is_pair(irr)) {
                print_value_with_depth(out, ch_car(irr), display, 0);
                irr = ch_cdr(irr);
            } else {
                print_value_with_depth(out, irr, display, 0);
                break;
            }
        }
        fputc('>', out);
        return;
    }
    if (ch_is_function(v)) {
        fputs("#<function>", out);
        return;
    }
    fputs("#<unknown>", out);
}

/* write/display without datum labels: truncate with "..." past MAX_PRINT_DEPTH. */
static void print_value_with_depth(FILE *out, ChValue v, bool display, uint32_t depth) {
    if (depth >= CH_MAX_PRINT_DEPTH) {
        fputs("...", out);
        return;
    }
    if (ch_is_pair(v)) {
        print_list_simple(out, v, display, depth);
        return;
    }
    if (ch_is_vector(v)) {
        ChVector *vec = ch_as_vector(v);
        fputs("#(", out);
        for (size_t i = 0; i < vec->len; i++) {
            if (i > 0) {
                fputc(' ', out);
            }
            print_value_with_depth(out, vec->items[i], display, depth + 1);
        }
        fputc(')', out);
        return;
    }
    print_value_atom(out, v, display);
}

static void print_value_shared(FILE *out, ChValue v, ChSharedState *st, bool display) {
    if (st->depth >= CH_MAX_PRINT_DEPTH) {
        fputs("#<deep>", out);
        return;
    }
    st->depth++;

    if (is_labelable(v) && shared_index(st, v) >= 0) {
        int32_t existing = peek_label(st, v);
        if (existing >= 0) {
            fprintf(out, "#%d#", existing);
            st->depth--;
            return;
        }
        int32_t label = get_or_assign_label(st, v);
        if (label >= 0) {
            fprintf(out, "#%d=", label);
        }
    }

    if (ch_is_pair(v)) {
        print_list_shared(out, v, st, display);
    } else if (ch_is_vector(v)) {
        ChVector *vec = ch_as_vector(v);
        fputs("#(", out);
        for (size_t i = 0; i < vec->len; i++) {
            if (i > 0) {
                fputc(' ', out);
            }
            print_value_shared(out, vec->items[i], st, display);
        }
        fputc(')', out);
    } else if (ch_is_record(v)) {
        ChRecord *r = ch_as_record(v);
        const char *nm = "?";
        if (r->rtype && ch_is_string(r->rtype->name)) {
            nm = ch_as_string(r->rtype->name)->data;
        }
        fprintf(out, "#<%s", nm);
        for (size_t i = 0; i < r->num_fields; i++) {
            fputc(' ', out);
            print_value_shared(out, r->fields[i], st, display);
        }
        fputc('>', out);
    } else {
        print_value_atom(out, v, display);
    }
    st->depth--;
}

void ch_print_value_mode(FILE *out, ChValue v, ChPrintMode mode) {
    bool display = (mode == CH_PRINT_DISPLAY);
    if (mode == CH_PRINT_SIMPLE) {
        print_value_with_depth(out, v, false, 0);
        return;
    }

    ChSharedState st;
    memset(&st, 0, sizeof(st));
    if (mode == CH_PRINT_SHARED) {
        mark_shared(v, &st);
    } else {
        mark_cycles(v, &st);
    }
    st.depth = 0;
    if (st.shared_count == 0) {
        print_value_with_depth(out, v, display, 0);
    } else {
        print_value_shared(out, v, &st, display);
    }
    shared_state_free(&st);
}

char *ch_value_to_string_mode(ChValue v, ChPrintMode mode) {
    char *buf = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&buf, &size);
    if (!mem) {
        return NULL;
    }
    ch_print_value_mode(mem, v, mode);
    fclose(mem);
    return buf;
}

void ch_print_value(FILE *out, ChValue v, bool display) {
    ch_print_value_mode(out, v, display ? CH_PRINT_DISPLAY : CH_PRINT_WRITE);
}

char *ch_value_to_string(ChValue v, bool display) {
    return ch_value_to_string_mode(v, display ? CH_PRINT_DISPLAY : CH_PRINT_WRITE);
}
