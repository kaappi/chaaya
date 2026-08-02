#ifndef CHAAYA_LSP_INTERNAL_H
#define CHAAYA_LSP_INTERNAL_H

#include "chaaya/lsp.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- JSON-RPC framing (lsp.c) --- */
int write_jsonrpc_message(const char *payload);

/* --- document store (lsp.c) --- */
const char *get_document_text(const char *uri);

/* --- shared JSON extraction helpers (lsp.c) --- */
bool extract_json_string_field_bounded(const char *json, const char *field, char *out,
                                       size_t out_size);
bool extract_uri_and_position(const char *json, char *uri, size_t uri_size, int *line,
                              int *character);

/* --- analysis / response writers (lsp_analysis.c) --- */
int write_completion_response(const char *id_start, size_t id_len, const char *json);
int write_hover_response(const char *id_start, size_t id_len, const char *json);
int write_definition_response(const char *id_start, size_t id_len, const char *json);
int write_references_response(const char *id_start, size_t id_len, const char *json);
int write_symbols_response(const char *id_start, size_t id_len, const char *json);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_LSP_INTERNAL_H */
