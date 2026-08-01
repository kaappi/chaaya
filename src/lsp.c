#include "chaaya/lsp.h"

#include "chaaya/cli.h"

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

static int write_initialize_response(const char *id_start, size_t id_len) {
    if (id_len > (size_t)INT_MAX) {
        return CH_EXIT_ERROR;
    }
    char response[512];
    int n = snprintf(response, sizeof(response),
                     "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":{\"capabilities\":{}}}",
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
    if (strcmp(method, "shutdown") == 0) {
        return has_id ? write_shutdown_response(id_start, id_len) : CH_EXIT_OK;
    }
    if (strcmp(method, "exit") == 0) {
        return CH_LSP_SIGNAL_EXIT;
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
