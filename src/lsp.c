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
#include <stdarg.h>

enum { CH_LSP_SIGNAL_EXIT = 99 };

typedef struct ChLspDocument {
    char *uri;
    char *text;
    struct ChLspDocument *next;
} ChLspDocument;

typedef enum {
    CH_LSP_TOK_LPAREN,
    CH_LSP_TOK_RPAREN,
    CH_LSP_TOK_SYMBOL
} ChLspTokenKind;

typedef struct {
    ChLspTokenKind kind;
    size_t start;
    size_t end;
    int line;
    int character;
} ChLspToken;

typedef struct {
    ChLspToken *items;
    size_t count;
    size_t capacity;
} ChLspTokenVec;

typedef enum {
    CH_LSP_BIND_DEFINE = 0,
    CH_LSP_BIND_DEFINE_SYNTAX = 1,
    CH_LSP_BIND_SET = 2
} ChLspBindingKind;

typedef struct {
    ChLspToken symbol;
    ChLspBindingKind kind;
} ChLspBinding;

typedef struct {
    ChLspBinding *items;
    size_t count;
    size_t capacity;
} ChLspBindingVec;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} ChLspNameVec;

typedef struct {
    const char *name;
    const char *description;
    int completion_kind;
} ChLspBuiltin;

static ChLspDocument *g_lsp_documents = NULL;

static const ChLspBuiltin g_lsp_builtins[] = {
    {"define", "Bind a variable or procedure.", 14},
    {"define-syntax", "Define a macro transformer.", 14},
    {"lambda", "Create an anonymous procedure.", 14},
    {"if", "Select a branch by predicate value.", 14},
    {"cond", "Evaluate first matching clause.", 14},
    {"case", "Select clause by datum comparison.", 14},
    {"begin", "Evaluate forms in sequence.", 14},
    {"set!", "Mutate an existing binding.", 14},
    {"let", "Introduce local bindings.", 14},
    {"let*", "Introduce sequential local bindings.", 14},
    {"letrec", "Introduce mutually recursive bindings.", 14},
    {"letrec*", "Introduce ordered recursive bindings.", 14},
    {"let-values", "Bind multiple values.", 14},
    {"let*-values", "Bind multiple values sequentially.", 14},
    {"quote", "Return datum without evaluation.", 14},
    {"quasiquote", "Template with selective unquote.", 14},
    {"unquote", "Evaluate within quasiquote.", 14},
    {"unquote-splicing", "Splice list within quasiquote.", 14},
    {"and", "Short-circuit logical conjunction.", 14},
    {"or", "Short-circuit logical disjunction.", 14},
    {"when", "Conditional body when true.", 14},
    {"unless", "Conditional body when false.", 14},
    {"do", "Iterative loop form.", 14},
    {"delay", "Create a promise.", 14},
    {"let-syntax", "Local macro bindings.", 14},
    {"letrec-syntax", "Recursive local macro bindings.", 14},
    {"syntax-rules", "Pattern-based macro transformer.", 14},
    {"guard", "Exception handling form.", 14},
    {"car", "Return first pair element.", 3},
    {"cdr", "Return second pair element.", 3},
    {"cons", "Construct a pair.", 3},
    {"list", "Construct a list.", 3},
    {"pair?", "Test whether value is a pair.", 3},
    {"null?", "Test whether value is the empty list.", 3},
    {"symbol?", "Test whether value is a symbol.", 3},
    {"boolean?", "Test whether value is a boolean.", 3},
    {"number?", "Test whether value is numeric.", 3},
    {"string?", "Test whether value is a string.", 3},
    {"vector?", "Test whether value is a vector.", 3},
    {"eq?", "Pointer/identity comparison.", 3},
    {"eqv?", "R7RS equivalence predicate.", 3},
    {"equal?", "Structural equality predicate.", 3},
    {"not", "Boolean negation.", 3},
    {"+", "Addition procedure.", 3},
    {"-", "Subtraction procedure.", 3},
    {"*", "Multiplication procedure.", 3},
    {"/", "Division procedure.", 3},
    {"=", "Numeric equality predicate.", 3},
    {"<", "Numeric less-than predicate.", 3},
    {"<=", "Numeric less-or-equal predicate.", 3},
    {">", "Numeric greater-than predicate.", 3},
    {">=", "Numeric greater-or-equal predicate.", 3},
    {"apply", "Apply procedure to argument list.", 3},
    {"map", "Map procedure over lists.", 3},
    {"for-each", "Iterate for side effects.", 3},
    {"call-with-current-continuation", "Capture current continuation.", 3},
    {"call/cc", "Alias of call-with-current-continuation.", 3},
    {"values", "Return multiple values.", 3},
    {"call-with-values", "Consume multiple values.", 3},
    {"display", "Write human-readable output.", 3},
    {"write", "Write external representation.", 3},
    {"newline", "Emit newline character.", 3},
    {"read", "Read next datum from input.", 3},
    {"string-length", "Return string length.", 3},
    {"string-append", "Concatenate strings.", 3},
    {"vector-length", "Return vector length.", 3},
    {"make-vector", "Create a vector.", 3},
};

enum { CH_LSP_BUILTIN_COUNT = (int)(sizeof(g_lsp_builtins) / sizeof(g_lsp_builtins[0])) };

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

static char *ch_lsp_strdup(const char *s) {
    size_t n = strlen(s);
    char *copy = (char *)malloc(n + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, n + 1);
    return copy;
}

static bool buf_append_n(char *buf, size_t *len, size_t cap, const char *s, size_t n) {
    if (*len + n >= cap) {
        return false;
    }
    memcpy(buf + *len, s, n);
    *len += n;
    buf[*len] = '\0';
    return true;
}

static bool buf_append(char *buf, size_t *len, size_t cap, const char *s) {
    return buf_append_n(buf, len, cap, s, strlen(s));
}

static bool buf_append_char(char *buf, size_t *len, size_t cap, char ch) {
    if (*len + 1 >= cap) {
        return false;
    }
    buf[(*len)++] = ch;
    buf[*len] = '\0';
    return true;
}

static bool buf_append_fmt(char *buf, size_t *len, size_t cap, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, args);
    va_end(args);
    if (n < 0) {
        return false;
    }
    if ((size_t)n >= cap - *len) {
        return false;
    }
    *len += (size_t)n;
    return true;
}

static bool buf_append_json_escaped_n(char *buf, size_t *len, size_t cap, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '"' || ch == '\\') {
            if (!buf_append_char(buf, len, cap, '\\') || !buf_append_char(buf, len, cap, (char)ch)) {
                return false;
            }
            continue;
        }
        if (ch == '\n') {
            if (!buf_append(buf, len, cap, "\\n")) {
                return false;
            }
            continue;
        }
        if (ch == '\r') {
            if (!buf_append(buf, len, cap, "\\r")) {
                return false;
            }
            continue;
        }
        if (ch == '\t') {
            if (!buf_append(buf, len, cap, "\\t")) {
                return false;
            }
            continue;
        }
        if (ch < 0x20) {
            if (!buf_append_fmt(buf, len, cap, "\\u%04X", (unsigned)ch)) {
                return false;
            }
            continue;
        }
        if (!buf_append_char(buf, len, cap, (char)ch)) {
            return false;
        }
    }
    return true;
}

static bool buf_append_json_escaped(char *buf, size_t *len, size_t cap, const char *s) {
    return buf_append_json_escaped_n(buf, len, cap, s, strlen(s));
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

static const char *get_document_text(const char *uri) {
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

static bool token_vec_push(ChLspTokenVec *vec, ChLspToken token) {
    if (vec->count == vec->capacity) {
        size_t next_capacity = vec->capacity ? vec->capacity * 2 : 64;
        ChLspToken *next_items = (ChLspToken *)realloc(vec->items, next_capacity * sizeof(*next_items));
        if (!next_items) {
            return false;
        }
        vec->items = next_items;
        vec->capacity = next_capacity;
    }
    vec->items[vec->count++] = token;
    return true;
}

static void token_vec_free(ChLspTokenVec *vec) {
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

static bool binding_vec_push(ChLspBindingVec *vec, ChLspBinding binding) {
    if (vec->count == vec->capacity) {
        size_t next_capacity = vec->capacity ? vec->capacity * 2 : 16;
        ChLspBinding *next_items =
            (ChLspBinding *)realloc(vec->items, next_capacity * sizeof(*next_items));
        if (!next_items) {
            return false;
        }
        vec->items = next_items;
        vec->capacity = next_capacity;
    }
    vec->items[vec->count++] = binding;
    return true;
}

static void binding_vec_free(ChLspBindingVec *vec) {
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

static bool name_vec_contains(const ChLspNameVec *vec, const char *name) {
    for (size_t i = 0; i < vec->count; i++) {
        if (strcmp(vec->items[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static bool name_vec_add_unique_slice(ChLspNameVec *vec, const char *text, size_t start, size_t end) {
    if (end <= start) {
        return true;
    }
    size_t n = end - start;
    char *name = (char *)malloc(n + 1);
    if (!name) {
        return false;
    }
    memcpy(name, text + start, n);
    name[n] = '\0';
    if (name_vec_contains(vec, name)) {
        free(name);
        return true;
    }
    if (vec->count == vec->capacity) {
        size_t next_capacity = vec->capacity ? vec->capacity * 2 : 16;
        char **next_items = (char **)realloc(vec->items, next_capacity * sizeof(*next_items));
        if (!next_items) {
            free(name);
            return false;
        }
        vec->items = next_items;
        vec->capacity = next_capacity;
    }
    vec->items[vec->count++] = name;
    return true;
}

static void name_vec_free(ChLspNameVec *vec) {
    for (size_t i = 0; i < vec->count; i++) {
        free(vec->items[i]);
    }
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

static bool is_symbol_delimiter(unsigned char ch) {
    if (isspace(ch)) {
        return true;
    }
    switch (ch) {
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '"':
    case ';':
    case '\'':
    case '`':
    case ',':
        return true;
    default:
        return false;
    }
}

static bool scan_document_tokens(const char *text, ChLspTokenVec *tokens) {
    size_t i = 0;
    int line = 0;
    int character = 0;
    while (text[i]) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\r') {
            i++;
            if (text[i] == '\n') {
                i++;
            }
            line++;
            character = 0;
            continue;
        }
        if (ch == '\n') {
            i++;
            line++;
            character = 0;
            continue;
        }
        if (isspace(ch)) {
            i++;
            character++;
            continue;
        }
        if (ch == ';') {
            while (text[i] && text[i] != '\n' && text[i] != '\r') {
                i++;
                character++;
            }
            continue;
        }
        if (ch == '"') {
            i++;
            character++;
            while (text[i]) {
                unsigned char inner = (unsigned char)text[i];
                if (inner == '\\' && text[i + 1]) {
                    i += 2;
                    character += 2;
                    continue;
                }
                if (inner == '"') {
                    i++;
                    character++;
                    break;
                }
                if (inner == '\r') {
                    i++;
                    if (text[i] == '\n') {
                        i++;
                    }
                    line++;
                    character = 0;
                    continue;
                }
                if (inner == '\n') {
                    i++;
                    line++;
                    character = 0;
                    continue;
                }
                i++;
                character++;
            }
            continue;
        }
        if (ch == '(' || ch == '[' || ch == '{') {
            ChLspToken token = {.kind = CH_LSP_TOK_LPAREN,
                                .start = i,
                                .end = i + 1,
                                .line = line,
                                .character = character};
            if (!token_vec_push(tokens, token)) {
                return false;
            }
            i++;
            character++;
            continue;
        }
        if (ch == ')' || ch == ']' || ch == '}') {
            ChLspToken token = {.kind = CH_LSP_TOK_RPAREN,
                                .start = i,
                                .end = i + 1,
                                .line = line,
                                .character = character};
            if (!token_vec_push(tokens, token)) {
                return false;
            }
            i++;
            character++;
            continue;
        }
        if (ch == '\'' || ch == '`') {
            i++;
            character++;
            continue;
        }
        if (ch == ',') {
            i++;
            character++;
            if (text[i] == '@') {
                i++;
                character++;
            }
            continue;
        }

        size_t start = i;
        int start_line = line;
        int start_character = character;
        while (text[i] && !is_symbol_delimiter((unsigned char)text[i])) {
            i++;
            character++;
        }
        if (i > start) {
            ChLspToken token = {.kind = CH_LSP_TOK_SYMBOL,
                                .start = start,
                                .end = i,
                                .line = start_line,
                                .character = start_character};
            if (!token_vec_push(tokens, token)) {
                return false;
            }
        }
    }
    return true;
}

static bool token_is_symbol_name(const char *text, const ChLspToken *token, const char *name) {
    if (!token || token->kind != CH_LSP_TOK_SYMBOL) {
        return false;
    }
    size_t name_len = strlen(name);
    size_t token_len = token->end - token->start;
    if (name_len != token_len) {
        return false;
    }
    return strncmp(text + token->start, name, token_len) == 0;
}

static bool token_symbol_equals(const char *text, const ChLspToken *lhs, const ChLspToken *rhs) {
    if (!lhs || !rhs || lhs->kind != CH_LSP_TOK_SYMBOL || rhs->kind != CH_LSP_TOK_SYMBOL) {
        return false;
    }
    size_t lhs_len = lhs->end - lhs->start;
    size_t rhs_len = rhs->end - rhs->start;
    if (lhs_len != rhs_len) {
        return false;
    }
    return memcmp(text + lhs->start, text + rhs->start, lhs_len) == 0;
}

static const ChLspToken *find_symbol_at_position(const ChLspTokenVec *tokens, int line, int character) {
    for (size_t i = 0; i < tokens->count; i++) {
        const ChLspToken *token = &tokens->items[i];
        if (token->kind != CH_LSP_TOK_SYMBOL || token->line != line) {
            continue;
        }
        int token_start = token->character;
        int token_end = token_start + (int)(token->end - token->start);
        if (character >= token_start && character <= token_end) {
            return token;
        }
    }
    return NULL;
}

static bool collect_bindings(const char *text, const ChLspTokenVec *tokens, ChLspBindingVec *bindings) {
    for (size_t i = 0; i + 2 < tokens->count; i++) {
        const ChLspToken *open = &tokens->items[i];
        const ChLspToken *keyword = &tokens->items[i + 1];
        if (open->kind != CH_LSP_TOK_LPAREN || keyword->kind != CH_LSP_TOK_SYMBOL) {
            continue;
        }

        ChLspBinding binding = {0};
        bool found = false;
        if (token_is_symbol_name(text, keyword, "define")) {
            const ChLspToken *next = &tokens->items[i + 2];
            if (next->kind == CH_LSP_TOK_SYMBOL) {
                binding.symbol = *next;
                binding.kind = CH_LSP_BIND_DEFINE;
                found = true;
            } else if (next->kind == CH_LSP_TOK_LPAREN && i + 3 < tokens->count &&
                       tokens->items[i + 3].kind == CH_LSP_TOK_SYMBOL) {
                binding.symbol = tokens->items[i + 3];
                binding.kind = CH_LSP_BIND_DEFINE;
                found = true;
            }
        } else if (token_is_symbol_name(text, keyword, "define-syntax")) {
            const ChLspToken *next = &tokens->items[i + 2];
            if (next->kind == CH_LSP_TOK_SYMBOL) {
                binding.symbol = *next;
                binding.kind = CH_LSP_BIND_DEFINE_SYNTAX;
                found = true;
            }
        } else if (token_is_symbol_name(text, keyword, "set!")) {
            const ChLspToken *next = &tokens->items[i + 2];
            if (next->kind == CH_LSP_TOK_SYMBOL) {
                binding.symbol = *next;
                binding.kind = CH_LSP_BIND_SET;
                found = true;
            }
        }

        if (found && !binding_vec_push(bindings, binding)) {
            return false;
        }
    }
    return true;
}

static const ChLspBuiltin *find_builtin_by_name(const char *name, size_t name_len) {
    for (int i = 0; i < CH_LSP_BUILTIN_COUNT; i++) {
        size_t builtin_len = strlen(g_lsp_builtins[i].name);
        if (builtin_len == name_len && strncmp(g_lsp_builtins[i].name, name, name_len) == 0) {
            return &g_lsp_builtins[i];
        }
    }
    return NULL;
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

static bool extract_json_string_field_bounded(const char *json, const char *field, char *out,
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

static bool extract_uri_and_position(const char *json, char *uri, size_t uri_size, int *line,
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

static bool append_location_json(char *buf, size_t *len, size_t cap, const char *uri,
                                 const ChLspToken *token) {
    size_t token_len = token->end - token->start;
    return buf_append(buf, len, cap, "{\"uri\":\"") && buf_append_json_escaped(buf, len, cap, uri) &&
           buf_append_fmt(buf, len, cap,
                          "\",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                          "\"end\":{\"line\":%d,\"character\":%d}}}",
                          token->line, token->character, token->line,
                          token->character + (int)token_len);
}

static int write_empty_array_response(const char *id_start, size_t id_len) {
    if (id_len > (size_t)INT_MAX) {
        return CH_EXIT_ERROR;
    }
    char response[256];
    int n =
        snprintf(response, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":[]}",
                 (int)id_len, id_start);
    if (n < 0 || (size_t)n >= sizeof(response)) {
        return CH_EXIT_ERROR;
    }
    return write_jsonrpc_message(response);
}

static int write_null_response(const char *id_start, size_t id_len) {
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

static int write_completion_response(const char *id_start, size_t id_len, const char *json) {
    char uri[1024];
    int line = 0;
    int character = 0;
    if (!extract_uri_and_position(json, uri, sizeof(uri), &line, &character)) {
        return write_empty_array_response(id_start, id_len);
    }
    (void)line;
    (void)character;

    const char *text = get_document_text(uri);
    if (!text) {
        text = "";
    }

    ChLspTokenVec tokens = {0};
    ChLspBindingVec bindings = {0};
    ChLspNameVec local_names = {0};
    if (!scan_document_tokens(text, &tokens) || !collect_bindings(text, &tokens, &bindings)) {
        token_vec_free(&tokens);
        binding_vec_free(&bindings);
        name_vec_free(&local_names);
        return CH_EXIT_ERROR;
    }
    for (size_t i = 0; i < bindings.count; i++) {
        if (bindings.items[i].kind == CH_LSP_BIND_DEFINE ||
            bindings.items[i].kind == CH_LSP_BIND_DEFINE_SYNTAX) {
            if (!name_vec_add_unique_slice(&local_names, text, bindings.items[i].symbol.start,
                                           bindings.items[i].symbol.end)) {
                token_vec_free(&tokens);
                binding_vec_free(&bindings);
                name_vec_free(&local_names);
                return CH_EXIT_ERROR;
            }
        }
    }

    char response[65536];
    size_t len = 0;
    bool ok = true;
    ok = ok && buf_append_fmt(response, &len, sizeof(response),
                              "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":{\"isIncomplete\":false,"
                              "\"items\":[",
                              (int)id_len, id_start);

    bool first = true;
    for (int i = 0; i < CH_LSP_BUILTIN_COUNT && ok; i++) {
        if (!first) {
            ok = buf_append_char(response, &len, sizeof(response), ',');
        }
        first = false;
        ok = ok && buf_append(response, &len, sizeof(response), "{\"label\":\"") &&
             buf_append_json_escaped(response, &len, sizeof(response), g_lsp_builtins[i].name) &&
             buf_append_fmt(response, &len, sizeof(response), "\",\"kind\":%d}",
                            g_lsp_builtins[i].completion_kind);
    }
    for (size_t i = 0; i < local_names.count && ok; i++) {
        if (find_builtin_by_name(local_names.items[i], strlen(local_names.items[i]))) {
            continue;
        }
        if (!first) {
            ok = buf_append_char(response, &len, sizeof(response), ',');
        }
        first = false;
        ok = ok && buf_append(response, &len, sizeof(response), "{\"label\":\"") &&
             buf_append_json_escaped(response, &len, sizeof(response), local_names.items[i]) &&
             buf_append(response, &len, sizeof(response), "\",\"kind\":6}");
    }
    ok = ok && buf_append(response, &len, sizeof(response), "]}}");

    token_vec_free(&tokens);
    binding_vec_free(&bindings);
    name_vec_free(&local_names);
    if (!ok) {
        return CH_EXIT_ERROR;
    }
    return write_jsonrpc_message(response);
}

static int write_hover_response(const char *id_start, size_t id_len, const char *json) {
    char uri[1024];
    int line = 0;
    int character = 0;
    if (!extract_uri_and_position(json, uri, sizeof(uri), &line, &character)) {
        return write_null_response(id_start, id_len);
    }
    const char *text = get_document_text(uri);
    if (!text) {
        return write_null_response(id_start, id_len);
    }

    ChLspTokenVec tokens = {0};
    if (!scan_document_tokens(text, &tokens)) {
        token_vec_free(&tokens);
        return CH_EXIT_ERROR;
    }
    const ChLspToken *symbol = find_symbol_at_position(&tokens, line, character);
    if (!symbol) {
        token_vec_free(&tokens);
        return write_null_response(id_start, id_len);
    }

    const ChLspBuiltin *builtin =
        find_builtin_by_name(text + symbol->start, symbol->end - symbol->start);
    char response[2048];
    size_t len = 0;
    bool ok = buf_append_fmt(response, &len, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":{",
                             (int)id_len, id_start);
    if (builtin) {
        ok = ok && buf_append(response, &len, sizeof(response), "\"contents\":{\"kind\":\"markdown\",\"value\":\"`") &&
             buf_append_json_escaped(response, &len, sizeof(response), builtin->name) &&
             buf_append(response, &len, sizeof(response), "`: ") &&
             buf_append_json_escaped(response, &len, sizeof(response), builtin->description) &&
             buf_append(response, &len, sizeof(response), "\"}}");
    } else {
        ok = ok && buf_append(response, &len, sizeof(response), "\"contents\":{\"kind\":\"plaintext\",\"value\":\"") &&
             buf_append_json_escaped_n(response, &len, sizeof(response), text + symbol->start,
                                       symbol->end - symbol->start) &&
             buf_append(response, &len, sizeof(response), "\"}}");
    }
    ok = ok && buf_append(response, &len, sizeof(response), "}");
    token_vec_free(&tokens);
    if (!ok) {
        return CH_EXIT_ERROR;
    }
    return write_jsonrpc_message(response);
}

static const ChLspBinding *find_preferred_binding(const char *text, const ChLspBindingVec *bindings,
                                                  const ChLspToken *symbol) {
    const ChLspBinding *best_define = NULL;
    const ChLspBinding *best_set = NULL;
    for (size_t i = 0; i < bindings->count; i++) {
        const ChLspBinding *binding = &bindings->items[i];
        if (!token_symbol_equals(text, &binding->symbol, symbol)) {
            continue;
        }
        if (binding->kind == CH_LSP_BIND_SET) {
            if (!best_set || binding->symbol.start < best_set->symbol.start) {
                best_set = binding;
            }
            continue;
        }
        if (binding->symbol.start <= symbol->start) {
            if (!best_define || binding->symbol.start > best_define->symbol.start) {
                best_define = binding;
            }
        } else if (!best_define) {
            best_define = binding;
        }
    }
    return best_define ? best_define : best_set;
}

static int write_definition_response(const char *id_start, size_t id_len, const char *json) {
    char uri[1024];
    int line = 0;
    int character = 0;
    if (!extract_uri_and_position(json, uri, sizeof(uri), &line, &character)) {
        return write_empty_array_response(id_start, id_len);
    }
    const char *text = get_document_text(uri);
    if (!text) {
        return write_empty_array_response(id_start, id_len);
    }

    ChLspTokenVec tokens = {0};
    ChLspBindingVec bindings = {0};
    if (!scan_document_tokens(text, &tokens) || !collect_bindings(text, &tokens, &bindings)) {
        token_vec_free(&tokens);
        binding_vec_free(&bindings);
        return CH_EXIT_ERROR;
    }
    const ChLspToken *symbol = find_symbol_at_position(&tokens, line, character);
    if (!symbol) {
        token_vec_free(&tokens);
        binding_vec_free(&bindings);
        return write_empty_array_response(id_start, id_len);
    }
    const ChLspBinding *binding = find_preferred_binding(text, &bindings, symbol);
    if (!binding) {
        token_vec_free(&tokens);
        binding_vec_free(&bindings);
        return write_empty_array_response(id_start, id_len);
    }

    char response[2048];
    size_t len = 0;
    bool ok =
        buf_append_fmt(response, &len, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":",
                       (int)id_len, id_start) &&
        append_location_json(response, &len, sizeof(response), uri, &binding->symbol) &&
        buf_append(response, &len, sizeof(response), "}");
    token_vec_free(&tokens);
    binding_vec_free(&bindings);
    if (!ok) {
        return CH_EXIT_ERROR;
    }
    return write_jsonrpc_message(response);
}

static int write_references_response(const char *id_start, size_t id_len, const char *json) {
    char uri[1024];
    int line = 0;
    int character = 0;
    if (!extract_uri_and_position(json, uri, sizeof(uri), &line, &character)) {
        return write_empty_array_response(id_start, id_len);
    }
    const char *text = get_document_text(uri);
    if (!text) {
        return write_empty_array_response(id_start, id_len);
    }

    ChLspTokenVec tokens = {0};
    if (!scan_document_tokens(text, &tokens)) {
        token_vec_free(&tokens);
        return CH_EXIT_ERROR;
    }
    const ChLspToken *symbol = find_symbol_at_position(&tokens, line, character);
    if (!symbol) {
        token_vec_free(&tokens);
        return write_empty_array_response(id_start, id_len);
    }

    char response[65536];
    size_t len = 0;
    bool ok =
        buf_append_fmt(response, &len, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":[",
                       (int)id_len, id_start);
    bool first = true;
    for (size_t i = 0; i < tokens.count && ok; i++) {
        const ChLspToken *candidate = &tokens.items[i];
        if (!token_symbol_equals(text, symbol, candidate)) {
            continue;
        }
        if (!first) {
            ok = buf_append_char(response, &len, sizeof(response), ',');
        }
        first = false;
        ok = ok && append_location_json(response, &len, sizeof(response), uri, candidate);
    }
    ok = ok && buf_append(response, &len, sizeof(response), "]}");
    token_vec_free(&tokens);
    if (!ok) {
        return CH_EXIT_ERROR;
    }
    return write_jsonrpc_message(response);
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
        ChVM vm;
        ch_vm_init(&vm);
        ch_vm_register_primitives(&vm);
        int rc = write_symbols_response(id_start, id_len, &vm);
        ch_vm_deinit(&vm);
        return rc;
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
