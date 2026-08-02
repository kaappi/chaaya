#include "chaaya/diagnostics.h"

#include "chaaya/cli.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ChDiagFormat g_diag_format = CH_DIAG_FMT_TEXT;

static const ChDiagInfo g_registry[] = {
    {CH_DIAG_UNEXPECTED_EOF, "unexpected-eof", CH_DIAG_SEVERITY_ERROR,
     "unexpected end of input",
     "The reader reached end of file while still expecting tokens.",
     "(define x  ; missing closing paren"},
    {CH_DIAG_UNEXPECTED_CHAR, "unexpected-char", CH_DIAG_SEVERITY_ERROR, "unexpected character",
     "A character was found that is not valid in this position.", "(@ 1)"},
    {CH_DIAG_UNEXPECTED_RIGHT_PAREN, "unexpected-right-paren", CH_DIAG_SEVERITY_ERROR,
     "unexpected ')'", "A closing parenthesis appeared without a matching open paren.", ")"},
    {CH_DIAG_INVALID_NUMBER, "invalid-number", CH_DIAG_SEVERITY_ERROR, "invalid number literal",
     "The numeric token could not be parsed.", "#e1.2.3"},
    {CH_DIAG_INVALID_CHARACTER_NAME, "invalid-character-name", CH_DIAG_SEVERITY_ERROR,
     "invalid character name", "An unrecognized #\\name was used.", "#\\nota-char"},
    {CH_DIAG_UNTERMINATED_STRING, "unterminated-string", CH_DIAG_SEVERITY_ERROR,
     "unterminated string", "A string literal was not closed before end of input.", "\"hello"},
    {CH_DIAG_INVALID_ESCAPE, "invalid-escape", CH_DIAG_SEVERITY_ERROR, "invalid escape sequence",
     "A string or identifier escape is not valid.", "\"\\q\""},
    {CH_DIAG_DOT_OUTSIDE_LIST, "dot-outside-list", CH_DIAG_SEVERITY_ERROR, "dot outside list",
     "A dotted pair marker '.' was used outside a list.", "."},
    {CH_DIAG_NESTING_TOO_DEEP, "nesting-too-deep", CH_DIAG_SEVERITY_ERROR, "nesting too deep",
     "Datum nesting exceeded the implementation limit.", "(((((((( ..."},
    {CH_DIAG_TOKEN_TOO_LONG, "token-too-long", CH_DIAG_SEVERITY_ERROR, "token too long",
     "A single token exceeded the implementation size limit.", "; very long identifier"},

    {CH_DIAG_INVALID_SYNTAX, "invalid-syntax", CH_DIAG_SEVERITY_ERROR, "invalid syntax",
     "The form is not valid Scheme at this stage of expansion/compilation.", "(if)"},
    {CH_DIAG_SYNTAX_ERROR, "syntax-error", CH_DIAG_SEVERITY_ERROR, "syntax error",
     "Macro expansion or compilation rejected this form.", "(define)"},
    {CH_DIAG_MACRO_EXPANSION_LIMIT, "macro-expansion-limit", CH_DIAG_SEVERITY_ERROR,
     "macro expansion limit exceeded",
     "Macro expansion recursed beyond the implementation limit.",
     "(define-syntax loop (syntax-rules () ((_) (loop))))"},

    {CH_DIAG_UNCAUGHT_EXCEPTION, "uncaught-exception", CH_DIAG_SEVERITY_ERROR,
     "uncaught exception", "An exception escaped all exception handlers.", "(raise 'boom)"},
    {CH_DIAG_UNDEFINED_VARIABLE, "undefined-variable", CH_DIAG_SEVERITY_ERROR,
     "undefined variable", "A variable was referenced but is not bound.", "undefined-name"},
    {CH_DIAG_TYPE_ERROR, "type-error", CH_DIAG_SEVERITY_ERROR, "type error",
     "A value of the wrong type was passed to a procedure.", "(+ 'a 1)"},
    {CH_DIAG_ARITY_MISMATCH, "arity-mismatch", CH_DIAG_SEVERITY_ERROR, "arity mismatch",
     "The wrong number of arguments was passed.", "(cons 1)"},
    {CH_DIAG_DIVISION_BY_ZERO, "division-by-zero", CH_DIAG_SEVERITY_ERROR, "division by zero",
     "An exact or inexact division by zero was attempted.", "(/ 1 0)"},
    {CH_DIAG_NOT_A_PROCEDURE, "not-a-procedure", CH_DIAG_SEVERITY_ERROR, "not a procedure",
     "An attempt was made to call a non-procedure value.", "(1 2)"},
    {CH_DIAG_INDEX_OUT_OF_BOUNDS, "index-out-of-bounds", CH_DIAG_SEVERITY_ERROR,
     "index out of bounds", "A sequence index was outside the valid range.",
     "(vector-ref #(1) 9)"},
    {CH_DIAG_INVALID_ARGUMENT, "invalid-argument", CH_DIAG_SEVERITY_ERROR, "invalid argument",
     "An argument was invalid for the called procedure.", "(make-vector -1)"},
    {CH_DIAG_STACK_OVERFLOW, "stack-overflow", CH_DIAG_SEVERITY_ERROR, "stack overflow",
     "The VM call stack exceeded its maximum depth.",
     "(let loop () (loop)) ; without TCO in a non-tail position"},
    {CH_DIAG_EXECUTION_TIMEOUT, "execution-timeout", CH_DIAG_SEVERITY_ERROR, "execution timeout",
     "Execution exceeded a configured time limit.", "; chaaya --timeout 1 long-running.scm"},

    {CH_DIAG_UNKNOWN_TOPLEVEL_VARIABLE, "unknown-toplevel-variable", CH_DIAG_SEVERITY_WARNING,
     "unknown top-level variable",
     "`chaaya check` found a top-level reference that is not defined or imported.",
     "(define (f) missing-name)"},
    {CH_DIAG_PRIMITIVE_ARITY_MISMATCH, "primitive-arity-mismatch", CH_DIAG_SEVERITY_ERROR,
     "primitive arity mismatch",
     "`chaaya check` detected a call to a known primitive with the wrong arity.", "(cons 1)"},
    {CH_DIAG_PRIMITIVE_TYPE_MISMATCH, "primitive-type-mismatch", CH_DIAG_SEVERITY_ERROR,
     "primitive type mismatch",
     "`chaaya check` detected a literal argument of an incompatible type.", "(car 1)"},

    {CH_DIAG_UNCATEGORIZED, "uncategorized", CH_DIAG_SEVERITY_ERROR, "error",
     "An error without a more specific stable code.", "; generic failure"},
    {CH_DIAG_INTERNAL_ERROR, "internal-error", CH_DIAG_SEVERITY_ERROR, "internal error",
     "An unexpected internal failure in the implementation.", "; should not happen"},
    {CH_DIAG_OUT_OF_MEMORY, "out-of-memory", CH_DIAG_SEVERITY_ERROR, "out of memory",
     "A memory allocation failed.", "; allocate until OOM"},
};

enum { CH_DIAG_REGISTRY_LEN = (int)(sizeof(g_registry) / sizeof(g_registry[0])) };

void ch_diag_set_format(ChDiagFormat fmt) {
    g_diag_format = fmt;
}

ChDiagFormat ch_diag_get_format(void) {
    return g_diag_format;
}

ChDiagStage ch_diag_stage(ChDiagCode code) {
    int n = (int)code / 1000;
    switch (n) {
    case 1:
        return CH_DIAG_STAGE_READ;
    case 2:
        return CH_DIAG_STAGE_COMPILE;
    case 3:
        return CH_DIAG_STAGE_RUNTIME;
    case 4:
        return CH_DIAG_STAGE_STATIC_ANALYSIS;
    default:
        return CH_DIAG_STAGE_INTERNAL;
    }
}

const char *ch_diag_stage_label(ChDiagStage stage) {
    switch (stage) {
    case CH_DIAG_STAGE_READ:
        return "read";
    case CH_DIAG_STAGE_COMPILE:
        return "compile";
    case CH_DIAG_STAGE_RUNTIME:
        return "runtime";
    case CH_DIAG_STAGE_STATIC_ANALYSIS:
        return "static-analysis";
    case CH_DIAG_STAGE_INTERNAL:
    default:
        return "internal";
    }
}

const ChDiagInfo *ch_diag_lookup(ChDiagCode code) {
    for (int i = 0; i < CH_DIAG_REGISTRY_LEN; i++) {
        if (g_registry[i].code == code) {
            return &g_registry[i];
        }
    }
    return NULL;
}

static int is_all_digits(const char *s) {
    if (!s || !*s) {
        return 0;
    }
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
}

ChDiagCode ch_diag_parse_code(const char *s) {
    if (!s || !*s) {
        return (ChDiagCode)0;
    }
    const char *digits = s;
    if ((digits[0] == 'C' || digits[0] == 'c') && (digits[1] == 'H' || digits[1] == 'h')) {
        digits += 2;
    } else if ((digits[0] == 'K' || digits[0] == 'k') && (digits[1] == 'P' || digits[1] == 'p')) {
        /* Accept KP aliases for Kaappi corpus familiarity. */
        digits += 2;
    }
    if (is_all_digits(digits)) {
        int n = atoi(digits);
        if (ch_diag_lookup((ChDiagCode)n)) {
            return (ChDiagCode)n;
        }
        return (ChDiagCode)0;
    }
    for (int i = 0; i < CH_DIAG_REGISTRY_LEN; i++) {
        if (strcmp(g_registry[i].name, s) == 0) {
            return g_registry[i].code;
        }
    }
    return (ChDiagCode)0;
}

void ch_diag_location_from_offset(const char *src, size_t len, size_t offset, int *line_out,
                                  int *col_out) {
    int line = 1;
    int col = 1;
    if (!src) {
        if (line_out) {
            *line_out = 0;
        }
        if (col_out) {
            *col_out = 0;
        }
        return;
    }
    size_t lim = offset < len ? offset : len;
    for (size_t i = 0; i < lim; i++) {
        if (src[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    if (line_out) {
        *line_out = line;
    }
    if (col_out) {
        *col_out = col;
    }
}

void ch_diag_init(ChDiag *d, ChDiagCode code, const char *file, int line, int column,
                  const char *message) {
    memset(d, 0, sizeof(*d));
    d->code = code;
    const ChDiagInfo *info = ch_diag_lookup(code);
    d->severity = info ? info->severity : CH_DIAG_SEVERITY_ERROR;
    d->file = file;
    d->line = line;
    d->column = column;
    if (message && message[0]) {
        snprintf(d->message, sizeof(d->message), "%s", message);
    } else if (info) {
        snprintf(d->message, sizeof(d->message), "%s", info->message);
    } else {
        snprintf(d->message, sizeof(d->message), "error");
    }
}

static void json_escape_to(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (*p < 0x20) {
                fprintf(out, "\\u%04x", *p);
            } else {
                fputc(*p, out);
            }
            break;
        }
    }
    fputc('"', out);
}

void ch_diag_report(FILE *out, const ChDiag *d) {
    if (!out || !d) {
        return;
    }
    char codebuf[16];
    snprintf(codebuf, sizeof(codebuf), "CH%04d", (int)d->code);
    const char *sev =
        d->severity == CH_DIAG_SEVERITY_WARNING ? "warning" : "error";
    const char *stage = ch_diag_stage_label(ch_diag_stage(d->code));
    int line0 = d->line > 0 ? d->line - 1 : 0;
    int col0 = d->column > 0 ? d->column - 1 : 0;
    int end_col0 = col0 + 1;

    if (g_diag_format == CH_DIAG_FMT_JSON) {
        fputs("{\"range\":{\"start\":{\"line\":", out);
        fprintf(out, "%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}},", line0, col0,
                line0, end_col0);
        fprintf(out, "\"severity\":%d,\"code\":", d->severity == CH_DIAG_SEVERITY_WARNING ? 2 : 1);
        json_escape_to(out, codebuf);
        fputs(",\"source\":\"chaaya\",\"message\":", out);
        json_escape_to(out, d->message);
        fputs(",\"data\":{\"stage\":", out);
        json_escape_to(out, stage);
        if (d->file && d->file[0]) {
            fputs(",\"file\":", out);
            json_escape_to(out, d->file);
        }
        fputs("}}\n", out);
        return;
    }

    if (d->file && d->file[0] && d->line > 0) {
        fprintf(out, "%s:%d:%d: ", d->file, d->line, d->column > 0 ? d->column : 1);
    } else if (d->file && d->file[0]) {
        fprintf(out, "%s: ", d->file);
    }
    /* Keep a literal "error:" / "warning:" so existing test harnesses that
     * scrape `error:` continue to match (see run_kaappi_deferred.cmake). */
    fprintf(out, "%s %s: [%s] %s\n", stage, sev, codebuf, d->message);
}

void ch_diag_report_read(FILE *out, const char *file, const char *src, size_t src_len, size_t pos,
                         ChDiagCode code, const char *message) {
    int line = 0;
    int col = 0;
    ch_diag_location_from_offset(src, src_len, pos, &line, &col);
    ChDiag d;
    ch_diag_init(&d, code, file, line, col, message);
    ch_diag_report(out, &d);
}

void ch_diag_report_simple(FILE *out, const char *file, int line, int column, ChDiagCode code,
                           const char *stage_override, const char *message) {
    (void)stage_override;
    ChDiag d;
    ch_diag_init(&d, code, file, line, column, message);
    ch_diag_report(out, &d);
}

ChDiagCode ch_diag_classify_message(const char *msg, ChDiagStage fallback_stage) {
    if (!msg) {
        return fallback_stage == CH_DIAG_STAGE_READ   ? CH_DIAG_UNEXPECTED_EOF
               : fallback_stage == CH_DIAG_STAGE_COMPILE ? CH_DIAG_SYNTAX_ERROR
               : fallback_stage == CH_DIAG_STAGE_RUNTIME ? CH_DIAG_UNCATEGORIZED
                                                         : CH_DIAG_UNCATEGORIZED;
    }
    if (strstr(msg, "undefined") || strstr(msg, "unbound")) {
        return CH_DIAG_UNDEFINED_VARIABLE;
    }
    if (strstr(msg, "arity") || strstr(msg, "wrong number") || strstr(msg, "too few") ||
        strstr(msg, "too many")) {
        return CH_DIAG_ARITY_MISMATCH;
    }
    if (strstr(msg, "not a procedure") || strstr(msg, "not applicable")) {
        return CH_DIAG_NOT_A_PROCEDURE;
    }
    if (strstr(msg, "division by zero") || strstr(msg, "divide by zero")) {
        return CH_DIAG_DIVISION_BY_ZERO;
    }
    if (strstr(msg, "out of bounds") || strstr(msg, "index out of")) {
        return CH_DIAG_INDEX_OUT_OF_BOUNDS;
    }
    if (strstr(msg, "stack overflow")) {
        return CH_DIAG_STACK_OVERFLOW;
    }
    if (strstr(msg, "unterminated string")) {
        return CH_DIAG_UNTERMINATED_STRING;
    }
    if (strstr(msg, "unexpected EOF") || strstr(msg, "unexpected end")) {
        return CH_DIAG_UNEXPECTED_EOF;
    }
    if (strstr(msg, "unexpected character") || strstr(msg, "unexpected ')'") ||
        strstr(msg, "unexpected )")) {
        return CH_DIAG_UNEXPECTED_CHAR;
    }
    if (strstr(msg, "nesting")) {
        return CH_DIAG_NESTING_TOO_DEEP;
    }
    if (strstr(msg, "invalid number")) {
        return CH_DIAG_INVALID_NUMBER;
    }
    if (strstr(msg, "type error") || strstr(msg, "type-error")) {
        return CH_DIAG_TYPE_ERROR;
    }
    if (strstr(msg, "syntax") || strstr(msg, "invalid") || strstr(msg, "bad syntax") ||
        strstr(msg, "expected")) {
        return fallback_stage == CH_DIAG_STAGE_READ ? CH_DIAG_UNEXPECTED_CHAR : CH_DIAG_SYNTAX_ERROR;
    }
    switch (fallback_stage) {
    case CH_DIAG_STAGE_READ:
        return CH_DIAG_UNEXPECTED_CHAR;
    case CH_DIAG_STAGE_COMPILE:
        return CH_DIAG_SYNTAX_ERROR;
    case CH_DIAG_STAGE_RUNTIME:
        return CH_DIAG_UNCATEGORIZED;
    case CH_DIAG_STAGE_STATIC_ANALYSIS:
        return CH_DIAG_UNKNOWN_TOPLEVEL_VARIABLE;
    default:
        return CH_DIAG_INTERNAL_ERROR;
    }
}

int ch_diag_explain(const char *code_or_name, int json) {
    if (!code_or_name || !code_or_name[0]) {
        fprintf(stderr, "explain: missing diagnostic code\n");
        return CH_EXIT_USAGE;
    }
    ChDiagCode code = ch_diag_parse_code(code_or_name);
    if (!code) {
        fprintf(stderr, "explain: unknown diagnostic code '%s'\n", code_or_name);
        return CH_EXIT_ERROR;
    }
    const ChDiagInfo *info = ch_diag_lookup(code);
    if (!info) {
        fprintf(stderr, "explain: unknown diagnostic code '%s'\n", code_or_name);
        return CH_EXIT_ERROR;
    }
    char codebuf[16];
    snprintf(codebuf, sizeof(codebuf), "CH%04d", (int)code);
    const char *stage = ch_diag_stage_label(ch_diag_stage(code));
    const char *sev = info->severity == CH_DIAG_SEVERITY_WARNING ? "warning" : "error";

    if (json) {
        printf("{\"code\":\"%s\",\"name\":\"%s\",\"stage\":\"%s\",\"severity\":\"%s\","
               "\"message\":",
               codebuf, info->name, stage, sev);
        /* minimal JSON escaping for fixed registry strings */
        fputc('"', stdout);
        for (const char *p = info->message; *p; p++) {
            if (*p == '"' || *p == '\\') {
                fputc('\\', stdout);
            }
            fputc(*p, stdout);
        }
        fputs("\",\"explanation\":\"", stdout);
        for (const char *p = info->explanation; *p; p++) {
            if (*p == '"' || *p == '\\') {
                fputc('\\', stdout);
            }
            if (*p == '\n') {
                fputs("\\n", stdout);
            } else {
                fputc(*p, stdout);
            }
        }
        fputs("\",\"example\":\"", stdout);
        for (const char *p = info->example; *p; p++) {
            if (*p == '"' || *p == '\\') {
                fputc('\\', stdout);
            }
            if (*p == '\n') {
                fputs("\\n", stdout);
            } else {
                fputc(*p, stdout);
            }
        }
        fputs("\"}\n", stdout);
        return CH_EXIT_OK;
    }

    printf("%s (%s)\n", codebuf, info->name);
    printf("  stage:      %s\n", stage);
    printf("  severity:   %s\n", sev);
    printf("  message:    %s\n", info->message);
    printf("  explanation:%s%s\n", info->explanation[0] ? " " : "", info->explanation);
    printf("  example:    %s\n", info->example);
    return CH_EXIT_OK;
}
