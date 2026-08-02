#include "chaaya/lsp.h"

#include "lsp_internal.h"

#include "chaaya/cli.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int write_completion_response(const char *id_start, size_t id_len, const char *json) {
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

int write_hover_response(const char *id_start, size_t id_len, const char *json) {
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

int write_definition_response(const char *id_start, size_t id_len, const char *json) {
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

int write_references_response(const char *id_start, size_t id_len, const char *json) {
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

int write_symbols_response(const char *id_start, size_t id_len, const char *json) {
    char uri[1024];
    if (!extract_json_string_field_bounded(json, "uri", uri, sizeof(uri))) {
        /* textDocument.uri may be nested; try full extract path used elsewhere */
        int line = 0;
        int character = 0;
        if (!extract_uri_and_position(json, uri, sizeof(uri), &line, &character)) {
            return write_empty_array_response(id_start, id_len);
        }
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

    char response[16384];
    size_t len = 0;
    bool ok = buf_append_fmt(response, &len, sizeof(response),
                             "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":[", (int)id_len, id_start);
    bool first = true;
    for (size_t i = 0; i < bindings.count && ok; i++) {
        const ChLspBinding *b = &bindings.items[i];
        if (b->kind == CH_LSP_BIND_SET) {
            continue;
        }
        if (!first) {
            ok = buf_append_char(response, &len, sizeof(response), ',');
        }
        first = false;
        int kind = b->kind == CH_LSP_BIND_DEFINE_SYNTAX ? 14 : 12;
        size_t name_len = b->symbol.end > b->symbol.start ? b->symbol.end - b->symbol.start : 0;
        ok = ok && buf_append_fmt(response, &len, sizeof(response),
                                  "{\"name\":\"%.*s\",\"kind\":%d,\"location\":", (int)name_len,
                                  text + b->symbol.start, kind) &&
             append_location_json(response, &len, sizeof(response), uri, &b->symbol) &&
             buf_append(response, &len, sizeof(response), "}");
    }
    ok = ok && buf_append(response, &len, sizeof(response), "]}");
    token_vec_free(&tokens);
    binding_vec_free(&bindings);
    if (!ok) {
        return CH_EXIT_ERROR;
    }
    return write_jsonrpc_message(response);
}
