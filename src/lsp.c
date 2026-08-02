#include "chaaya/lsp.h"

#include "lsp_internal.h"

#include "chaaya/cli.h"
#include "chaaya/compiler.h"
#include "chaaya/diagnostics.h"
#include "chaaya/expander.h"
#include "chaaya/reader.h"
#include "chaaya/vm.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CH_LSP_SIGNAL_EXIT = 99 };

typedef struct ChLspDocument {
    char *uri;
    char *text;
    struct ChLspDocument *next;
} ChLspDocument;

static ChLspDocument *g_lsp_documents = NULL;

int write_jsonrpc_message(const char *payload) {
    size_t payload_len = strlen(payload);
    if (fprintf(stdout, "Content-Length: %zu\r\n\r\n", payload_len) < 0) {
        return CH_EXIT_ERROR;
    }
    if (fwrite(payload, 1, payload_len, stdout) != payload_len) {
        return CH_EXIT_ERROR;
    }
    if (fflush(stdout) != 0) {
        return CH_EXIT_ERROR;
    }
    return CH_EXIT_OK;
}

static char *ch_lsp_strdup(const char *s) {
    size_t n = strlen(s);
    char *copy = (char *)malloc(n + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, n + 1);
    return copy;
}

static ChLspDocument *find_document_mut(const char *uri) {
    for (ChLspDocument *doc = g_lsp_documents; doc; doc = doc->next) {
        if (strcmp(doc->uri, uri) == 0) {
            return doc;
        }
    }
    return NULL;
}

static const ChLspDocument *find_document(const char *uri) {
    return find_document_mut(uri);
}

static int upsert_document(const char *uri, const char *text) {
    ChLspDocument *doc = find_document_mut(uri);
    if (doc) {
        char *next_text = ch_lsp_strdup(text);
        if (!next_text) {
            return CH_EXIT_ERROR;
        }
        free(doc->text);
        doc->text = next_text;
        return CH_EXIT_OK;
    }

    ChLspDocument *node = (ChLspDocument *)calloc(1, sizeof(*node));
    if (!node) {
        return CH_EXIT_ERROR;
    }
    node->uri = ch_lsp_strdup(uri);
    node->text = ch_lsp_strdup(text);
    if (!node->uri || !node->text) {
        free(node->uri);
        free(node->text);
        free(node);
        return CH_EXIT_ERROR;
    }
    node->next = g_lsp_documents;
    g_lsp_documents = node;
    return CH_EXIT_OK;
}

const char *get_document_text(const char *uri) {
    const ChLspDocument *doc = find_document(uri);
    return doc ? doc->text : NULL;
}

static void remove_document(const char *uri) {
    ChLspDocument *prev = NULL;
    ChLspDocument *cur = g_lsp_documents;
    while (cur) {
        if (strcmp(cur->uri, uri) == 0) {
            if (prev) {
                prev->next = cur->next;
            } else {
                g_lsp_documents = cur->next;
            }
            free(cur->uri);
            free(cur->text);
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static void clear_documents(void) {
    ChLspDocument *cur = g_lsp_documents;
    while (cur) {
        ChLspDocument *next = cur->next;
        free(cur->uri);
        free(cur->text);
        free(cur);
        cur = next;
    }
    g_lsp_documents = NULL;
}

static const char *skip_json_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static bool parse_json_string_value(const char *quoted, char **out_value, const char **out_end) {
    if (!quoted || *quoted != '"') {
        return false;
    }
    size_t cap = 64;
    size_t len = 0;
    char *out = (char *)malloc(cap);
    if (!out) {
        return false;
    }
    const char *p = quoted + 1;
    while (*p) {
        char ch = *p++;
        if (ch == '"') {
            out[len] = '\0';
            *out_value = out;
            if (out_end) {
                *out_end = p;
            }
            return true;
        }
        if (ch == '\\') {
            char esc = *p++;
            if (!esc) {
                free(out);
                return false;
            }
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                ch = esc;
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'u':
                ch = '?';
                for (int i = 0; i < 4; i++) {
                    if (!isxdigit((unsigned char)p[i])) {
                        free(out);
                        return false;
                    }
                }
                p += 4;
                break;
            default:
                ch = esc;
                break;
            }
        }
        if (len + 1 >= cap) {
            size_t next_cap = cap * 2;
            char *next = (char *)realloc(out, next_cap);
            if (!next) {
                free(out);
                return false;
            }
            out = next;
            cap = next_cap;
        }
        out[len++] = ch;
    }
    free(out);
    return false;
}

static bool extract_json_string_field_alloc(const char *json, const char *field, char **out) {
    char key[64];
    int wrote = snprintf(key, sizeof(key), "\"%s\"", field);
    if (wrote < 0 || (size_t)wrote >= sizeof(key)) {
        return false;
    }
    const char *k = strstr(json, key);
    if (!k) {
        return false;
    }
    const char *p = skip_json_ws(k + strlen(key));
    if (*p != ':') {
        return false;
    }
    p = skip_json_ws(p + 1);
    if (*p != '"') {
        return false;
    }
    return parse_json_string_value(p, out, NULL);
}

bool extract_json_string_field_bounded(const char *json, const char *field, char *out,
                                       size_t out_size) {
    char *value = NULL;
    if (!extract_json_string_field_alloc(json, field, &value)) {
        return false;
    }
    size_t len = strlen(value);
    if (len + 1 > out_size) {
        free(value);
        return false;
    }
    memcpy(out, value, len + 1);
    free(value);
    return true;
}

static const char *find_matching_brace(const char *open_brace) {
    if (!open_brace || *open_brace != '{') {
        return NULL;
    }
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char *p = open_brace; *p; p++) {
        char ch = *p;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '{') {
            depth++;
            continue;
        }
        if (ch == '}') {
            depth--;
            if (depth == 0) {
                return p;
            }
            continue;
        }
    }
    return NULL;
}

static const char *find_substring_in_range(const char *start, const char *end, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || end <= start || (size_t)(end - start) < nlen) {
        return NULL;
    }
    const char *limit = end - nlen;
    for (const char *p = start; p <= limit; p++) {
        if (memcmp(p, needle, nlen) == 0) {
            return p;
        }
    }
    return NULL;
}

static bool extract_json_int_field_in_range(const char *start, const char *end, const char *field,
                                            int *out) {
    char key[64];
    int wrote = snprintf(key, sizeof(key), "\"%s\"", field);
    if (wrote < 0 || (size_t)wrote >= sizeof(key)) {
        return false;
    }
    const char *k = find_substring_in_range(start, end, key);
    if (!k) {
        return false;
    }
    const char *p = skip_json_ws(k + strlen(key));
    if (*p != ':') {
        return false;
    }
    p = skip_json_ws(p + 1);
    char *num_end = NULL;
    long v = strtol(p, &num_end, 10);
    if (num_end == p || num_end > end) {
        return false;
    }
    if (v < 0 || v > INT_MAX) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool extract_request_position(const char *json, int *line, int *character) {
    const char *pos_key = strstr(json, "\"position\"");
    if (!pos_key) {
        return false;
    }
    const char *open = strchr(pos_key, '{');
    if (!open) {
        return false;
    }
    const char *close = find_matching_brace(open);
    if (!close) {
        return false;
    }
    if (!extract_json_int_field_in_range(open, close + 1, "line", line)) {
        return false;
    }
    if (!extract_json_int_field_in_range(open, close + 1, "character", character)) {
        return false;
    }
    return true;
}

bool extract_uri_and_position(const char *json, char *uri, size_t uri_size, int *line,
                              int *character) {
    if (!extract_json_string_field_bounded(json, "uri", uri, uri_size)) {
        return false;
    }
    if (!extract_request_position(json, line, character)) {
        return false;
    }
    return true;
}

static int read_content_length(FILE *in, size_t *out_length) {
    char line[256];
    bool saw_header = false;
    bool saw_content_length = false;
    size_t content_length = 0;

    for (;;) {
        if (!fgets(line, sizeof(line), in)) {
            return saw_header ? -1 : 0;
        }

        if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) {
            if (!saw_content_length) {
                return -1;
            }
            *out_length = content_length;
            return 1;
        }

        saw_header = true;
        if (strncmp(line, "Content-Length:", 15) != 0) {
            continue;
        }

        const char *p = line + 15;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            return -1;
        }

        char *end = NULL;
        unsigned long long parsed = strtoull(p, &end, 10);
        if (end == p) {
            return -1;
        }
        while (*end && isspace((unsigned char)*end)) {
            end++;
        }
        if (*end != '\0') {
            return -1;
        }
        if (parsed > (unsigned long long)SIZE_MAX) {
            return -1;
        }

        content_length = (size_t)parsed;
        saw_content_length = true;
    }
}

static bool extract_method(const char *json, char *method, size_t method_size) {
    if (method_size == 0) {
        return false;
    }

    const char *key = strstr(json, "\"method\"");
    if (!key) {
        return false;
    }

    const char *p = key + strlen("\"method\"");
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t len = 0;
    while (p[len] && p[len] != '"') {
        if (len + 1 >= method_size) {
            return false;
        }
        method[len] = p[len];
        len++;
    }
    if (p[len] != '"') {
        return false;
    }
    method[len] = '\0';
    return true;
}

static bool extract_id_slice(const char *json, const char **id_start, size_t *id_len) {
    const char *key = strstr(json, "\"id\"");
    if (!key) {
        return false;
    }

    const char *p = key + strlen("\"id\"");
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        return false;
    }

    const char *start = p;
    const char *end = NULL;

    if (*p == '"') {
        p++;
        bool escaped = false;
        while (*p) {
            if (!escaped && *p == '"') {
                p++;
                break;
            }
            escaped = (!escaped && *p == '\\');
            if (*p != '\\') {
                escaped = false;
            }
            p++;
        }
        if (p == start + 1 || *(p - 1) != '"') {
            return false;
        }
        end = p;
    } else {
        while (*p && *p != ',' && *p != '}' && *p != '\r' && *p != '\n') {
            p++;
        }
        end = p;
    }

    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    if (end <= start) {
        return false;
    }

    *id_start = start;
    *id_len = (size_t)(end - start);
    return true;
}

static bool extract_json_string_field(const char *json, const char *field, char *out, size_t out_size) {
    return extract_json_string_field_bounded(json, field, out, out_size);
}

static int write_initialize_response(const char *id_start, size_t id_len) {
    if (id_len > (size_t)INT_MAX) {
        return CH_EXIT_ERROR;
    }
    char response[1024];
    int n = snprintf(response, sizeof(response),
                     "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":{\"capabilities\":{"
                     "\"textDocumentSync\":1,"
                     "\"completionProvider\":{\"resolveProvider\":false},"
                     "\"hoverProvider\":true,"
                     "\"referencesProvider\":true,"
                     "\"documentSymbolProvider\":true,"
                     "\"definitionProvider\":true"
                     "}}}",
                     (int)id_len, id_start);
    if (n < 0 || (size_t)n >= sizeof(response)) {
        return CH_EXIT_ERROR;
    }
    return write_jsonrpc_message(response);
}

static int write_shutdown_response(const char *id_start, size_t id_len) {
    if (id_len > (size_t)INT_MAX) {
        return CH_EXIT_ERROR;
    }
    char response[256];
    int n = snprintf(response, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":null}",
                     (int)id_len, id_start);
    if (n < 0 || (size_t)n >= sizeof(response)) {
        return CH_EXIT_ERROR;
    }
    return write_jsonrpc_message(response);
}

static int write_method_not_found_response(const char *id_start, size_t id_len) {
    if (id_len > (size_t)INT_MAX) {
        return CH_EXIT_ERROR;
    }
    char response[320];
    int n = snprintf(response, sizeof(response),
                     "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}",
                     (int)id_len, id_start);
    if (n < 0 || (size_t)n >= sizeof(response)) {
        return CH_EXIT_ERROR;
    }
    return write_jsonrpc_message(response);
}

static void json_escape_append(char *buf, size_t *len, size_t cap, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p && *len + 2 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            buf[(*len)++] = '\\';
        }
        if (*p == '\n') {
            if (*len + 2 >= cap) {
                break;
            }
            buf[(*len)++] = '\\';
            buf[(*len)++] = 'n';
            continue;
        }
        buf[(*len)++] = (char)*p;
    }
}

static int publish_diagnostics(const char *uri, const char *text) {
    ChDiagFormat saved = ch_diag_get_format();
    ch_diag_set_format(CH_DIAG_FMT_JSON);

    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);

    char diags[8192];
    size_t dlen = 0;
    diags[0] = '\0';
    int first = 1;

    size_t text_len = strlen(text);
    ChReader reader;
    ch_reader_init(&reader, &vm.gc, text, text_len);
    for (;;) {
        ChValue v = CH_NIL;
        ch_gc_push(&vm.gc, &v);
        size_t form_start = reader.pos;
        int form_line = 0;
        int form_col = 0;
        ch_diag_location_from_offset(text, text_len, form_start, &form_line, &form_col);
        ChReadStatus st = ch_read_datum(&reader, &v);
        if (st == CH_READ_EOF) {
            ch_gc_pop(&vm.gc);
            break;
        }
        if (st != CH_READ_OK) {
            char one[1024];
            FILE *mem = fmemopen(one, sizeof(one), "w");
            if (mem) {
                ch_diag_report_read(mem, uri, text, strlen(text), reader.pos,
                                    ch_reader_error_code(&reader), ch_reader_error(&reader));
                fclose(mem);
                /* strip trailing newline */
                size_t ol = strlen(one);
                while (ol > 0 && (one[ol - 1] == '\n' || one[ol - 1] == '\r')) {
                    one[--ol] = '\0';
                }
                if (!first && dlen + 1 < sizeof(diags)) {
                    diags[dlen++] = ',';
                }
                if (dlen + ol < sizeof(diags)) {
                    memcpy(diags + dlen, one, ol);
                    dlen += ol;
                    diags[dlen] = '\0';
                    first = 0;
                }
            }
            ch_gc_pop(&vm.gc);
            break;
        }

        ChValue expanded = CH_NIL;
        ch_gc_push(&vm.gc, &expanded);
        char err[256];
        if (ch_expand_toplevel(&vm, v, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
            ChDiag d;
            ch_diag_init(&d, ch_diag_classify_message(err, CH_DIAG_STAGE_COMPILE), uri, form_line,
                         form_col, err);
            char one[1024];
            FILE *mem = fmemopen(one, sizeof(one), "w");
            if (mem) {
                ch_diag_report(mem, &d);
                fclose(mem);
                size_t ol = strlen(one);
                while (ol > 0 && (one[ol - 1] == '\n' || one[ol - 1] == '\r')) {
                    one[--ol] = '\0';
                }
                if (!first && dlen + 1 < sizeof(diags)) {
                    diags[dlen++] = ',';
                }
                if (dlen + ol < sizeof(diags)) {
                    memcpy(diags + dlen, one, ol);
                    dlen += ol;
                    diags[dlen] = '\0';
                    first = 0;
                }
            }
            ch_gc_pop_n(&vm.gc, 2);
            break;
        }

        ChCompiler compiler;
        ch_compiler_init(&compiler, &vm);
        ch_compiler_set_location(&compiler, form_line, form_col);
        ChFunction *fn = NULL;
        if (ch_compile_toplevel(&compiler, expanded, &fn) != CH_COMPILE_OK) {
            ChDiag d;
            ch_diag_init(&d, ch_compiler_error_code(&compiler), uri, compiler.error_line,
                         compiler.error_column, ch_compiler_error(&compiler));
            char one[1024];
            FILE *mem = fmemopen(one, sizeof(one), "w");
            if (mem) {
                ch_diag_report(mem, &d);
                fclose(mem);
                size_t ol = strlen(one);
                while (ol > 0 && (one[ol - 1] == '\n' || one[ol - 1] == '\r')) {
                    one[--ol] = '\0';
                }
                if (!first && dlen + 1 < sizeof(diags)) {
                    diags[dlen++] = ',';
                }
                if (dlen + ol < sizeof(diags)) {
                    memcpy(diags + dlen, one, ol);
                    dlen += ol;
                    diags[dlen] = '\0';
                    first = 0;
                }
            }
            ch_gc_pop_n(&vm.gc, 2);
            break;
        }
        (void)fn;
        ch_gc_pop_n(&vm.gc, 2);
    }

    ch_vm_deinit(&vm);
    ch_diag_set_format(saved);

    char msg[16384];
    size_t mlen = 0;
    mlen += (size_t)snprintf(msg + mlen, sizeof(msg) - mlen,
                             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
                             "\"params\":{\"uri\":\"");
    json_escape_append(msg, &mlen, sizeof(msg), uri);
    mlen += (size_t)snprintf(msg + mlen, sizeof(msg) - mlen, "\",\"diagnostics\":[%s]}}", diags);
    return write_jsonrpc_message(msg);
}

static int handle_did_open_or_change(const char *json) {
    char uri[1024];
    if (!extract_json_string_field(json, "uri", uri, sizeof(uri))) {
        return CH_EXIT_OK;
    }
    char *text = NULL;
    if (!extract_json_string_field_alloc(json, "text", &text)) {
        const char *cached = get_document_text(uri);
        return publish_diagnostics(uri, cached ? cached : "");
    }

    int rc = upsert_document(uri, text);
    if (rc != CH_EXIT_OK) {
        free(text);
        return rc;
    }
    rc = publish_diagnostics(uri, text);
    free(text);
    return rc;
}

static int handle_did_close(const char *json) {
    char uri[1024];
    if (!extract_json_string_field(json, "uri", uri, sizeof(uri))) {
        return CH_EXIT_OK;
    }
    remove_document(uri);
    return CH_EXIT_OK;
}

static int handle_request(const char *json) {
    char method[64];
    if (!extract_method(json, method, sizeof(method))) {
        fprintf(stderr, "lsp: invalid JSON-RPC request (missing method)\n");
        return CH_EXIT_ERROR;
    }

    const char *id_start = NULL;
    size_t id_len = 0;
    bool has_id = extract_id_slice(json, &id_start, &id_len);

    if (strcmp(method, "initialize") == 0) {
        return has_id ? write_initialize_response(id_start, id_len) : CH_EXIT_OK;
    }
    if (strcmp(method, "initialized") == 0) {
        return CH_EXIT_OK;
    }
    if (strcmp(method, "shutdown") == 0) {
        return has_id ? write_shutdown_response(id_start, id_len) : CH_EXIT_OK;
    }
    if (strcmp(method, "exit") == 0) {
        return CH_LSP_SIGNAL_EXIT;
    }
    if (strcmp(method, "textDocument/didOpen") == 0 ||
        strcmp(method, "textDocument/didChange") == 0) {
        return handle_did_open_or_change(json);
    }
    if (strcmp(method, "textDocument/didClose") == 0) {
        return handle_did_close(json);
    }
    if (strcmp(method, "textDocument/completion") == 0 && has_id) {
        return write_completion_response(id_start, id_len, json);
    }
    if (strcmp(method, "textDocument/hover") == 0 && has_id) {
        return write_hover_response(id_start, id_len, json);
    }
    if (strcmp(method, "textDocument/documentSymbol") == 0 && has_id) {
        return write_symbols_response(id_start, id_len, json);
    }
    if (strcmp(method, "textDocument/definition") == 0 && has_id) {
        return write_definition_response(id_start, id_len, json);
    }
    if (strcmp(method, "textDocument/references") == 0 && has_id) {
        return write_references_response(id_start, id_len, json);
    }
    if (!has_id) {
        return CH_EXIT_OK;
    }
    return write_method_not_found_response(id_start, id_len);
}

int ch_lsp_run_stdio(void) {
    int final_rc = CH_EXIT_OK;
    for (;;) {
        size_t content_length = 0;
        int header_status = read_content_length(stdin, &content_length);
        if (header_status == 0) {
            final_rc = CH_EXIT_OK;
            break;
        }
        if (header_status < 0) {
            fprintf(stderr, "lsp: failed to parse JSON-RPC headers\n");
            final_rc = CH_EXIT_ERROR;
            break;
        }

        char *payload = (char *)malloc(content_length + 1);
        if (!payload) {
            fprintf(stderr, "lsp: out of memory\n");
            final_rc = CH_EXIT_ERROR;
            break;
        }
        size_t read_count = fread(payload, 1, content_length, stdin);
        if (read_count != content_length) {
            free(payload);
            fprintf(stderr, "lsp: unexpected EOF while reading request body\n");
            final_rc = CH_EXIT_ERROR;
            break;
        }
        payload[content_length] = '\0';

        int rc = handle_request(payload);
        free(payload);
        if (rc == CH_LSP_SIGNAL_EXIT) {
            final_rc = CH_EXIT_OK;
            break;
        }
        if (rc != CH_EXIT_OK) {
            final_rc = rc;
            break;
        }
    }
    clear_documents();
    return final_rc;
}
