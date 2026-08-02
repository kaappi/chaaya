#include "chaaya/lsp.h"

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

static int write_jsonrpc_message(const char *payload) {
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
    char key[64];
    snprintf(key, sizeof(key), "\"%s\"", field);
    const char *k = strstr(json, key);
    if (!k) {
        return false;
    }
    const char *p = k + strlen(key);
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
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
        }
        if (n + 1 >= out_size) {
            return false;
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    return true;
}

static int write_initialize_response(const char *id_start, size_t id_len) {
    if (id_len > (size_t)INT_MAX) {
        return CH_EXIT_ERROR;
    }
    char response[768];
    int n = snprintf(response, sizeof(response),
                     "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":{\"capabilities\":{"
                     "\"textDocumentSync\":1,"
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

    ChReader reader;
    ch_reader_init(&reader, &vm.gc, text, strlen(text));
    for (;;) {
        ChValue v = CH_NIL;
        ch_gc_push(&vm.gc, &v);
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
            ch_diag_init(&d, ch_diag_classify_message(err, CH_DIAG_STAGE_COMPILE), uri, 0, 0, err);
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
    char *text = NULL;
    if (!extract_json_string_field(json, "uri", uri, sizeof(uri))) {
        return CH_EXIT_OK;
    }
    /* text is large — find "text":"..." */
    const char *key = strstr(json, "\"text\"");
    if (!key) {
        return publish_diagnostics(uri, "");
    }
    const char *p = strchr(key + 6, '"');
    if (!p) {
        return publish_diagnostics(uri, "");
    }
    p++;
    size_t cap = 4096;
    size_t n = 0;
    text = (char *)malloc(cap);
    if (!text) {
        return CH_EXIT_ERROR;
    }
    while (*p && *p != '"') {
        char ch = *p++;
        if (ch == '\\' && *p) {
            char e = *p++;
            if (e == 'n') {
                ch = '\n';
            } else if (e == 't') {
                ch = '\t';
            } else if (e == 'r') {
                ch = '\r';
            } else {
                ch = e;
            }
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *nt = (char *)realloc(text, cap);
            if (!nt) {
                free(text);
                return CH_EXIT_ERROR;
            }
            text = nt;
        }
        text[n++] = ch;
    }
    text[n] = '\0';
    int rc = publish_diagnostics(uri, text);
    free(text);
    return rc;
}

static int write_symbols_response(const char *id_start, size_t id_len, ChVM *vm) {
    char body[8192];
    size_t blen = 0;
    body[0] = '[';
    blen = 1;
    int first = 1;
    for (size_t i = 0; i < vm->global_count && blen + 128 < sizeof(body); i++) {
        if (!vm->globals[i].defined || !vm->globals[i].name) {
            continue;
        }
        if (!first) {
            body[blen++] = ',';
        }
        first = 0;
        int n = snprintf(body + blen, sizeof(body) - blen,
                         "{\"name\":\"%s\",\"kind\":12,\"location\":{\"uri\":\"\",\"range\":{"
                         "\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}"
                         "}}}",
                         vm->globals[i].name->name);
        if (n < 0 || (size_t)n >= sizeof(body) - blen) {
            break;
        }
        blen += (size_t)n;
    }
    if (blen + 1 < sizeof(body)) {
        body[blen++] = ']';
        body[blen] = '\0';
    }
    char response[16384];
    int n = snprintf(response, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":%s}",
                     (int)id_len, id_start, body);
    if (n < 0 || (size_t)n >= sizeof(response)) {
        return CH_EXIT_ERROR;
    }
    return write_jsonrpc_message(response);
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
    if (strcmp(method, "textDocument/documentSymbol") == 0 && has_id) {
        ChVM vm;
        ch_vm_init(&vm);
        ch_vm_register_primitives(&vm);
        int rc = write_symbols_response(id_start, id_len, &vm);
        ch_vm_deinit(&vm);
        return rc;
    }
    if (strcmp(method, "textDocument/definition") == 0 && has_id) {
        /* MVP: empty locations */
        char response[256];
        snprintf(response, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":[]}",
                 (int)id_len, id_start);
        return write_jsonrpc_message(response);
    }
    if (!has_id) {
        return CH_EXIT_OK;
    }
    return write_method_not_found_response(id_start, id_len);
}

int ch_lsp_run_stdio(void) {
    for (;;) {
        size_t content_length = 0;
        int header_status = read_content_length(stdin, &content_length);
        if (header_status == 0) {
            return CH_EXIT_OK;
        }
        if (header_status < 0) {
            fprintf(stderr, "lsp: failed to parse JSON-RPC headers\n");
            return CH_EXIT_ERROR;
        }

        char *payload = (char *)malloc(content_length + 1);
        if (!payload) {
            fprintf(stderr, "lsp: out of memory\n");
            return CH_EXIT_ERROR;
        }
        size_t read_count = fread(payload, 1, content_length, stdin);
        if (read_count != content_length) {
            free(payload);
            fprintf(stderr, "lsp: unexpected EOF while reading request body\n");
            return CH_EXIT_ERROR;
        }
        payload[content_length] = '\0';

        int rc = handle_request(payload);
        free(payload);
        if (rc == CH_LSP_SIGNAL_EXIT) {
            return CH_EXIT_OK;
        }
        if (rc != CH_EXIT_OK) {
            return rc;
        }
    }
}
