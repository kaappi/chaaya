#ifndef CHAAYA_DIAGNOSTICS_H
#define CHAAYA_DIAGNOSTICS_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable CH codes (Kaappi KP taxonomy mirrored with CH prefix).
 * Leading digit = pipeline stage. Codes are never renumbered once shipped. */
typedef enum ChDiagCode {
    /* CH1xxx — read / lexical */
    CH_DIAG_UNEXPECTED_EOF = 1001,
    CH_DIAG_UNEXPECTED_CHAR = 1002,
    CH_DIAG_UNEXPECTED_RIGHT_PAREN = 1003,
    CH_DIAG_INVALID_NUMBER = 1004,
    CH_DIAG_INVALID_CHARACTER_NAME = 1005,
    CH_DIAG_UNTERMINATED_STRING = 1006,
    CH_DIAG_INVALID_ESCAPE = 1007,
    CH_DIAG_DOT_OUTSIDE_LIST = 1008,
    CH_DIAG_NESTING_TOO_DEEP = 1009,
    CH_DIAG_TOKEN_TOO_LONG = 1010,

    /* CH2xxx — expand / compile */
    CH_DIAG_INVALID_SYNTAX = 2001,
    CH_DIAG_SYNTAX_ERROR = 2002,
    CH_DIAG_MACRO_EXPANSION_LIMIT = 2003,

    /* CH3xxx — runtime */
    CH_DIAG_UNCAUGHT_EXCEPTION = 3000,
    CH_DIAG_UNDEFINED_VARIABLE = 3001,
    CH_DIAG_TYPE_ERROR = 3002,
    CH_DIAG_ARITY_MISMATCH = 3003,
    CH_DIAG_DIVISION_BY_ZERO = 3004,
    CH_DIAG_NOT_A_PROCEDURE = 3005,
    CH_DIAG_INDEX_OUT_OF_BOUNDS = 3006,
    CH_DIAG_INVALID_ARGUMENT = 3007,
    CH_DIAG_STACK_OVERFLOW = 3008,
    CH_DIAG_EXECUTION_TIMEOUT = 3009,

    /* CH4xxx — static analysis (check lint) */
    CH_DIAG_UNKNOWN_TOPLEVEL_VARIABLE = 4001,
    CH_DIAG_PRIMITIVE_ARITY_MISMATCH = 4002,
    CH_DIAG_PRIMITIVE_TYPE_MISMATCH = 4003,

    /* CH9xxx — internal */
    CH_DIAG_UNCATEGORIZED = 9000,
    CH_DIAG_INTERNAL_ERROR = 9001,
    CH_DIAG_OUT_OF_MEMORY = 9002,
} ChDiagCode;

typedef enum ChDiagSeverity {
    CH_DIAG_SEVERITY_ERROR = 0,
    CH_DIAG_SEVERITY_WARNING = 1,
} ChDiagSeverity;

typedef enum ChDiagStage {
    CH_DIAG_STAGE_READ = 0,
    CH_DIAG_STAGE_COMPILE,
    CH_DIAG_STAGE_RUNTIME,
    CH_DIAG_STAGE_STATIC_ANALYSIS,
    CH_DIAG_STAGE_INTERNAL,
} ChDiagStage;

typedef enum ChDiagFormat {
    CH_DIAG_FMT_TEXT = 0,
    CH_DIAG_FMT_JSON = 1,
} ChDiagFormat;

typedef struct ChDiagInfo {
    ChDiagCode code;
    const char *name; /* kebab-case, e.g. "undefined-variable" */
    ChDiagSeverity severity;
    const char *message; /* default template */
    const char *explanation;
    const char *example;
} ChDiagInfo;

typedef struct ChDiag {
    ChDiagCode code;
    ChDiagSeverity severity;
    const char *file; /* optional; may be NULL */
    int line;         /* 1-based; 0 = unknown */
    int column;       /* 1-based; 0 = unknown */
    char message[256];
} ChDiag;

void ch_diag_set_format(ChDiagFormat fmt);
ChDiagFormat ch_diag_get_format(void);

const ChDiagInfo *ch_diag_lookup(ChDiagCode code);
ChDiagCode ch_diag_parse_code(const char *s); /* 0 if unknown */
ChDiagStage ch_diag_stage(ChDiagCode code);
const char *ch_diag_stage_label(ChDiagStage stage);

void ch_diag_location_from_offset(const char *src, size_t len, size_t offset, int *line_out,
                                  int *col_out);

void ch_diag_init(ChDiag *d, ChDiagCode code, const char *file, int line, int column,
                  const char *message);

/* Render to stream. Text: file:line:col: stage error[CHxxxx]: msg
 * JSON: one LSP Diagnostic-shaped object per line. */
void ch_diag_report(FILE *out, const ChDiag *d);

/* Convenience wrappers used by CLI / eval. */
void ch_diag_report_read(FILE *out, const char *file, const char *src, size_t src_len, size_t pos,
                         ChDiagCode code, const char *message);
void ch_diag_report_simple(FILE *out, const char *file, int line, int column, ChDiagCode code,
                           const char *stage_override, const char *message);

/* chaaya explain <code> — static, no VM. Returns process exit code. */
int ch_diag_explain(const char *code_or_name, int json);

/* chaaya explain --all [--json] — dump the full diagnostic registry. */
int ch_diag_explain_all(int json);

/* Classify a free-form runtime/compiler message into a best-effort code. */
ChDiagCode ch_diag_classify_message(const char *msg, ChDiagStage fallback_stage);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_DIAGNOSTICS_H */
