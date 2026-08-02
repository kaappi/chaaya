/* Comment-preserving Scheme CST formatter (Kaappi fmt.zig / fmt_print.zig shaped). */

#include "chaaya/fmt.h"

#include "chaaya/eval.h"
#include "chaaya/gc.h"
#include "chaaya/reader.h"
#include "chaaya/value.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    FMT_MAX_WIDTH = 80,
    FMT_INDENT_STEP = 2,
    FMT_MAX_NESTING = 1024,
    FMT_MAX_FILE_BYTES = 8 * 1024 * 1024,
    FMT_ARENA_SLAB = 65536,
};

typedef enum FmtNodeKind {
    FMT_ATOM,
    FMT_LIST,
    FMT_PREFIX,
    FMT_DATUM_COMMENT,
    FMT_LINE_COMMENT,
    FMT_BLOCK_COMMENT,
} FmtNodeKind;

typedef struct FmtNode {
    FmtNodeKind kind;
    const char *text;
    size_t text_len;
    struct FmtNode *children;
    size_t child_count;
    uint32_t newlines_before;
    bool is_data;
    bool width_computed;
    size_t inline_width; /* valid when width_computed; SIZE_MAX => no inline width */
} FmtNode;

typedef enum FmtTokKind {
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LIST_OPEN,
    TOK_ATOM,
    TOK_PREFIX,
    TOK_DATUM_COMMENT,
    TOK_LINE_COMMENT,
    TOK_BLOCK_COMMENT,
    TOK_EOF,
} FmtTokKind;

typedef struct FmtTok {
    FmtTokKind kind;
    const char *text;
    size_t text_len;
    uint32_t newlines_before;
} FmtTok;

typedef struct FmtArenaBlock {
    struct FmtArenaBlock *next;
    size_t used;
    char data[FMT_ARENA_SLAB];
} FmtArenaBlock;

typedef struct FmtArena {
    FmtArenaBlock *head;
    FmtArenaBlock *cur;
} FmtArena;

typedef struct FmtBuf {
    char *data;
    size_t len;
    size_t cap;
} FmtBuf;

typedef struct FmtLexer {
    const char *src;
    size_t len;
    size_t pos;
} FmtLexer;

typedef struct FmtParser {
    FmtLexer lex;
    FmtArena *arena;
    uint32_t depth;
} FmtParser;

typedef struct FmtPrinter {
    FmtBuf *out;
    size_t col;
    bool line_comment_open;
} FmtPrinter;

typedef enum FmtParseErr {
    FMT_PARSE_OK = 0,
    FMT_PARSE_UNTERMINATED_LIST,
    FMT_PARSE_UNTERMINATED_STRING,
    FMT_PARSE_UNTERMINATED_BLOCK_COMMENT,
    FMT_PARSE_UNEXPECTED_RPAREN,
    FMT_PARSE_DANGLING_PREFIX,
    FMT_PARSE_NESTING_TOO_DEEP,
    FMT_PARSE_OOM,
} FmtParseErr;

typedef enum FmtStyleKind {
    FMT_STYLE_CALL,
    FMT_STYLE_BODY,
} FmtStyleKind;

typedef struct FmtStyle {
    FmtStyleKind kind;
    size_t body_n;
} FmtStyle;

/* ── Arena ─────────────────────────────────────────────────────────────── */

static FmtArenaBlock *fmt_arena_new_block(void) {
    FmtArenaBlock *b = (FmtArenaBlock *)malloc(sizeof(FmtArenaBlock));
    if (!b) {
        return NULL;
    }
    b->next = NULL;
    b->used = 0;
    return b;
}

static void fmt_arena_init(FmtArena *a) {
    a->head = fmt_arena_new_block();
    a->cur = a->head;
}

static void fmt_arena_free(FmtArena *a) {
    FmtArenaBlock *b = a->head;
    while (b) {
        FmtArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
    a->cur = NULL;
}

static void *fmt_arena_alloc(FmtArena *a, size_t n) {
    n = (n + 7u) & ~7u;
    if (!a->cur || a->cur->used + n > FMT_ARENA_SLAB) {
        FmtArenaBlock *b = fmt_arena_new_block();
        if (!b) {
            return NULL;
        }
        b->next = a->cur;
        a->cur = b;
    }
    void *p = a->cur->data + a->cur->used;
    a->cur->used += n;
    return p;
}

static char *fmt_arena_dup(FmtArena *a, const char *s, size_t n) {
    char *d = (char *)fmt_arena_alloc(a, n + 1);
    if (!d) {
        return NULL;
    }
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* ── Output buffer ─────────────────────────────────────────────────────── */

static bool fmt_buf_reserve(FmtBuf *b, size_t need) {
    if (b->len + need <= b->cap) {
        return true;
    }
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need) {
        cap *= 2;
    }
    char *next = (char *)realloc(b->data, cap);
    if (!next) {
        return false;
    }
    b->data = next;
    b->cap = cap;
    return true;
}

static bool fmt_buf_append(FmtBuf *b, const char *s, size_t n) {
    if (!fmt_buf_reserve(b, n + 1)) {
        return false;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

static bool fmt_buf_append_c(FmtBuf *b, char c) {
    return fmt_buf_append(b, &c, 1);
}

static void fmt_buf_free(FmtBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* ── Lexer helpers ─────────────────────────────────────────────────────── */

static bool fmt_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool fmt_is_delim(char c) {
    return fmt_is_space(c) || c == '(' || c == ')' || c == '"' || c == ';' || c == '|';
}

static uint32_t fmt_skip_space(FmtLexer *lx) {
    uint32_t newlines = 0;
    while (lx->pos < lx->len && fmt_is_space(lx->src[lx->pos])) {
        char c = lx->src[lx->pos++];
        if (c == '\n' ||
            (c == '\r' && !(lx->pos < lx->len && lx->src[lx->pos] == '\n'))) {
            newlines++;
        }
    }
    return newlines;
}

static FmtParseErr fmt_scan_string(FmtLexer *lx) {
    lx->pos++;
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == '\\') {
            lx->pos += 2;
            continue;
        }
        lx->pos++;
        if (c == '"') {
            return FMT_PARSE_OK;
        }
    }
    return FMT_PARSE_UNTERMINATED_STRING;
}

static FmtParseErr fmt_scan_pipe(FmtLexer *lx) {
    lx->pos++;
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == '\\') {
            lx->pos += 2;
            continue;
        }
        lx->pos++;
        if (c == '|') {
            return FMT_PARSE_OK;
        }
    }
    return FMT_PARSE_UNTERMINATED_STRING;
}

static void fmt_scan_char(FmtLexer *lx) {
    lx->pos += 2;
    if (lx->pos >= lx->len) {
        return;
    }
    unsigned char first = (unsigned char)lx->src[lx->pos++];
    if (first < 0x80) {
        /* one byte */
    } else {
        size_t seq = 1;
        if ((first & 0xE0) == 0xC0) {
            seq = 2;
        } else if ((first & 0xF0) == 0xE0) {
            seq = 3;
        } else if ((first & 0xF8) == 0xF0) {
            seq = 4;
        }
        while (seq > 1 && lx->pos < lx->len) {
            lx->pos++;
            seq--;
        }
    }
    if (isalpha(first)) {
        while (lx->pos < lx->len && isalnum((unsigned char)lx->src[lx->pos])) {
            lx->pos++;
        }
    }
}

static FmtParseErr fmt_scan_block_comment(FmtLexer *lx) {
    lx->pos += 2;
    size_t depth = 1;
    while (lx->pos + 1 < lx->len) {
        if (lx->src[lx->pos] == '#' && lx->src[lx->pos + 1] == '|') {
            depth++;
            lx->pos += 2;
        } else if (lx->src[lx->pos] == '|' && lx->src[lx->pos + 1] == '#') {
            depth--;
            lx->pos += 2;
            if (depth == 0) {
                return FMT_PARSE_OK;
            }
        } else {
            lx->pos++;
        }
    }
    return FMT_PARSE_UNTERMINATED_BLOCK_COMMENT;
}

static FmtParseErr fmt_scan_raw_string(FmtLexer *lx) {
    lx->pos += 2;
    size_t delim_start = lx->pos;
    while (lx->pos < lx->len && lx->src[lx->pos] != '"') {
        lx->pos++;
    }
    if (lx->pos >= lx->len) {
        return FMT_PARSE_UNTERMINATED_STRING;
    }
    size_t delim_len = lx->pos - delim_start;
    lx->pos++;
    while (lx->pos < lx->len) {
        if (lx->src[lx->pos] == '"' && lx->pos + delim_len + 2 <= lx->len &&
            memcmp(lx->src + lx->pos + 1, lx->src + delim_start, delim_len) == 0 &&
            lx->src[lx->pos + 1 + delim_len] == '"') {
            lx->pos += delim_len + 2;
            return FMT_PARSE_OK;
        }
        lx->pos++;
    }
    return FMT_PARSE_UNTERMINATED_STRING;
}

static void fmt_scan_atom(FmtLexer *lx) {
    lx->pos++;
    while (lx->pos < lx->len && !fmt_is_delim(lx->src[lx->pos])) {
        lx->pos++;
    }
}

static FmtParseErr fmt_scan_hash(FmtLexer *lx, FmtTokKind *out_kind) {
    const char *rest = lx->src + lx->pos;
    size_t rest_len = lx->len - lx->pos;

    if (rest_len >= 2 && rest[1] == '|') {
        FmtParseErr err = fmt_scan_block_comment(lx);
        if (err != FMT_PARSE_OK) {
            return err;
        }
        *out_kind = TOK_BLOCK_COMMENT;
        return FMT_PARSE_OK;
    }
    if (rest_len >= 2 && rest[1] == ';') {
        lx->pos += 2;
        *out_kind = TOK_DATUM_COMMENT;
        return FMT_PARSE_OK;
    }
    if (rest_len >= 2 && rest[1] == '(') {
        lx->pos += 2;
        *out_kind = TOK_LIST_OPEN;
        return FMT_PARSE_OK;
    }
    if (rest_len >= 4 && memcmp(rest, "#u8(", 4) == 0) {
        lx->pos += 4;
        *out_kind = TOK_LIST_OPEN;
        return FMT_PARSE_OK;
    }
    if (rest_len >= 4 && memcmp(rest, "#u8", 3) == 0 && rest[3] == '"') {
        lx->pos += 3;
        FmtParseErr err = fmt_scan_string(lx);
        if (err != FMT_PARSE_OK) {
            return err;
        }
        *out_kind = TOK_ATOM;
        return FMT_PARSE_OK;
    }
    if (rest_len >= 2 && rest[1] == '"') {
        FmtParseErr err = fmt_scan_raw_string(lx);
        if (err != FMT_PARSE_OK) {
            return err;
        }
        *out_kind = TOK_ATOM;
        return FMT_PARSE_OK;
    }
    if (rest_len >= 2 && rest[1] == '\\') {
        fmt_scan_char(lx);
        *out_kind = TOK_ATOM;
        return FMT_PARSE_OK;
    }
    if (rest_len >= 2 && isdigit((unsigned char)rest[1])) {
        size_t i = 1;
        while (i < rest_len && isdigit((unsigned char)rest[i])) {
            i++;
        }
        if (i < rest_len && rest[i] == '=') {
            lx->pos += i + 1;
            *out_kind = TOK_PREFIX;
            return FMT_PARSE_OK;
        }
    }
    fmt_scan_atom(lx);
    *out_kind = TOK_ATOM;
    return FMT_PARSE_OK;
}

static FmtParseErr fmt_lexer_next(FmtLexer *lx, FmtTok *tok) {
    uint32_t nl = fmt_skip_space(lx);
    if (lx->pos >= lx->len) {
        *tok = (FmtTok){.kind = TOK_EOF, .text = "", .text_len = 0, .newlines_before = nl};
        return FMT_PARSE_OK;
    }

    size_t start = lx->pos;
    char c = lx->src[lx->pos];
    FmtTokKind kind;
    FmtParseErr err = FMT_PARSE_OK;

    switch (c) {
    case '(':
        lx->pos++;
        kind = TOK_LPAREN;
        break;
    case ')':
        lx->pos++;
        kind = TOK_RPAREN;
        break;
    case '\'':
    case '`':
        lx->pos++;
        kind = TOK_PREFIX;
        break;
    case ',':
        lx->pos++;
        if (lx->pos < lx->len && lx->src[lx->pos] == '@') {
            lx->pos++;
        }
        kind = TOK_PREFIX;
        break;
    case ';':
        while (lx->pos < lx->len && lx->src[lx->pos] != '\n') {
            lx->pos++;
        }
        kind = TOK_LINE_COMMENT;
        break;
    case '"':
        err = fmt_scan_string(lx);
        kind = TOK_ATOM;
        break;
    case '|':
        err = fmt_scan_pipe(lx);
        kind = TOK_ATOM;
        break;
    case '#':
        err = fmt_scan_hash(lx, &kind);
        break;
    default:
        fmt_scan_atom(lx);
        kind = TOK_ATOM;
        break;
    }

    if (err != FMT_PARSE_OK) {
        return err;
    }

    *tok = (FmtTok){
        .kind = kind,
        .text = lx->src + start,
        .text_len = lx->pos - start,
        .newlines_before = nl,
    };
    return FMT_PARSE_OK;
}

/* ── EOL normalisation (block comments only) ───────────────────────────── */

static char *fmt_normalize_eol(FmtArena *arena, const char *text, size_t text_len, size_t *out_len) {
    const char *cr = memchr(text, '\r', text_len);
    if (!cr) {
        *out_len = text_len;
        return (char *)text;
    }
    char *buf = (char *)fmt_arena_alloc(arena, text_len + 1);
    if (!buf) {
        return NULL;
    }
    size_t n = 0;
    for (size_t i = 0; i < text_len; i++) {
        char c = text[i];
        if (c == '\r') {
            if (i + 1 < text_len && text[i + 1] == '\n') {
                continue;
            }
            buf[n++] = '\n';
        } else {
            buf[n++] = c;
        }
    }
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

static char *fmt_trim_line_comment(FmtArena *arena, const char *text, size_t text_len, size_t *out_len) {
    while (text_len > 0 && (text[text_len - 1] == ' ' || text[text_len - 1] == '\t' ||
                            text[text_len - 1] == '\r')) {
        text_len--;
    }
    *out_len = text_len;
    return fmt_arena_dup(arena, text, text_len);
}

/* ── Parser ────────────────────────────────────────────────────────────── */

static FmtParseErr fmt_parser_take(FmtParser *p, FmtTok *tok) {
    return fmt_lexer_next(&p->lex, tok);
}

static FmtParseErr fmt_parse_prefix_target(FmtParser *p, FmtNode *out);
static FmtParseErr fmt_node_from_tok(FmtParser *p, const FmtTok *tok, FmtNode *out);
static FmtParseErr fmt_parse_list(FmtParser *p, const FmtTok *open, bool is_data, FmtNode *out);

static FmtParseErr fmt_parse_list(FmtParser *p, const FmtTok *open, bool is_data, FmtNode *out) {
    if (p->depth >= FMT_MAX_NESTING) {
        return FMT_PARSE_NESTING_TOO_DEEP;
    }
    p->depth++;

    FmtNode *items = NULL;
    size_t count = 0;
    size_t cap = 0;

    for (;;) {
        FmtTok tok;
        FmtParseErr err = fmt_parser_take(p, &tok);
        if (err != FMT_PARSE_OK) {
            p->depth--;
            return err;
        }
        if (tok.kind == TOK_EOF) {
            p->depth--;
            return FMT_PARSE_UNTERMINATED_LIST;
        }
        if (tok.kind == TOK_RPAREN) {
            break;
        }
        if (count >= cap) {
            size_t next_cap = cap ? cap * 2 : 8;
            FmtNode *next = (FmtNode *)fmt_arena_alloc(p->arena, next_cap * sizeof(FmtNode));
            if (!next) {
                p->depth--;
                return FMT_PARSE_OOM;
            }
            if (items && count > 0) {
                memcpy(next, items, count * sizeof(FmtNode));
            }
            items = next;
            cap = next_cap;
        }
        err = fmt_node_from_tok(p, &tok, &items[count]);
        if (err != FMT_PARSE_OK) {
            p->depth--;
            return err;
        }
        count++;
    }

    p->depth--;
    *out = (FmtNode){
        .kind = FMT_LIST,
        .text = open->text,
        .text_len = open->text_len,
        .children = items,
        .child_count = count,
        .newlines_before = open->newlines_before,
        .is_data = is_data,
    };
    return FMT_PARSE_OK;
}

static FmtParseErr fmt_parse_prefix_target(FmtParser *p, FmtNode *out) {
    FmtTok tok;
    FmtParseErr err = fmt_parser_take(p, &tok);
    if (err != FMT_PARSE_OK) {
        return err;
    }
    if (tok.kind == TOK_EOF || tok.kind == TOK_RPAREN) {
        return FMT_PARSE_DANGLING_PREFIX;
    }
    return fmt_node_from_tok(p, &tok, out);
}

static FmtParseErr fmt_node_from_tok(FmtParser *p, const FmtTok *tok, FmtNode *out) {
    switch (tok->kind) {
    case TOK_LPAREN:
        return fmt_parse_list(p, tok, false, out);
    case TOK_LIST_OPEN:
        return fmt_parse_list(p, tok, true, out);
    case TOK_ATOM:
        *out = (FmtNode){
            .kind = FMT_ATOM,
            .text = tok->text,
            .text_len = tok->text_len,
            .newlines_before = tok->newlines_before,
        };
        return FMT_PARSE_OK;
    case TOK_LINE_COMMENT: {
        size_t n = 0;
        char *text = fmt_trim_line_comment(p->arena, tok->text, tok->text_len, &n);
        if (!text) {
            return FMT_PARSE_OOM;
        }
        *out = (FmtNode){
            .kind = FMT_LINE_COMMENT,
            .text = text,
            .text_len = n,
            .newlines_before = tok->newlines_before,
        };
        return FMT_PARSE_OK;
    }
    case TOK_BLOCK_COMMENT: {
        size_t n = 0;
        char *text = fmt_normalize_eol(p->arena, tok->text, tok->text_len, &n);
        if (!text) {
            return FMT_PARSE_OOM;
        }
        *out = (FmtNode){
            .kind = FMT_BLOCK_COMMENT,
            .text = text,
            .text_len = n,
            .newlines_before = tok->newlines_before,
        };
        return FMT_PARSE_OK;
    }
    case TOK_PREFIX:
    case TOK_DATUM_COMMENT: {
        FmtNode child;
        FmtParseErr err = fmt_parse_prefix_target(p, &child);
        if (err != FMT_PARSE_OK) {
            return err;
        }
        FmtNode *children = (FmtNode *)fmt_arena_alloc(p->arena, sizeof(FmtNode));
        if (!children) {
            return FMT_PARSE_OOM;
        }
        children[0] = child;
        *out = (FmtNode){
            .kind = (tok->kind == TOK_PREFIX) ? FMT_PREFIX : FMT_DATUM_COMMENT,
            .text = (tok->kind == TOK_DATUM_COMMENT) ? "#;" : tok->text,
            .text_len = (tok->kind == TOK_DATUM_COMMENT) ? 2 : tok->text_len,
            .children = children,
            .child_count = 1,
            .newlines_before = tok->newlines_before,
        };
        return FMT_PARSE_OK;
    }
    default:
        return FMT_PARSE_UNEXPECTED_RPAREN;
    }
}

static FmtParseErr fmt_parse_program(FmtParser *p, FmtNode **out_nodes, size_t *out_count) {
    FmtNode *nodes = NULL;
    size_t count = 0;
    size_t cap = 0;

    for (;;) {
        FmtTok tok;
        FmtParseErr err = fmt_parser_take(p, &tok);
        if (err != FMT_PARSE_OK) {
            return err;
        }
        if (tok.kind == TOK_EOF) {
            break;
        }
        if (tok.kind == TOK_RPAREN) {
            return FMT_PARSE_UNEXPECTED_RPAREN;
        }
        if (count >= cap) {
            size_t next_cap = cap ? cap * 2 : 8;
            FmtNode *next = (FmtNode *)fmt_arena_alloc(p->arena, next_cap * sizeof(FmtNode));
            if (!next) {
                return FMT_PARSE_OOM;
            }
            if (nodes && count > 0) {
                memcpy(next, nodes, count * sizeof(FmtNode));
            }
            nodes = next;
            cap = next_cap;
        }
        err = fmt_node_from_tok(p, &tok, &nodes[count]);
        if (err != FMT_PARSE_OK) {
            return err;
        }
        count++;
    }

    *out_nodes = nodes;
    *out_count = count;
    return FMT_PARSE_OK;
}

static const char *fmt_parse_err_msg(FmtParseErr err) {
    switch (err) {
    case FMT_PARSE_UNTERMINATED_LIST:
        return "syntax error: unterminated list";
    case FMT_PARSE_UNTERMINATED_STRING:
        return "syntax error: unterminated string or |symbol|";
    case FMT_PARSE_UNTERMINATED_BLOCK_COMMENT:
        return "syntax error: unterminated block comment";
    case FMT_PARSE_UNEXPECTED_RPAREN:
        return "syntax error: unexpected ')'";
    case FMT_PARSE_DANGLING_PREFIX:
        return "syntax error: quote/unquote with no datum";
    case FMT_PARSE_NESTING_TOO_DEEP:
        return "syntax error: nesting too deep";
    case FMT_PARSE_OOM:
        return "out of memory";
    default:
        return "syntax error";
    }
}

/* ── Layout helpers ────────────────────────────────────────────────────── */

static bool fmt_is_comment(FmtNodeKind k) {
    return k == FMT_LINE_COMMENT || k == FMT_BLOCK_COMMENT;
}

static size_t fmt_first_code_index(const FmtNode *children, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!fmt_is_comment(children[i].kind)) {
            return i;
        }
    }
    return SIZE_MAX;
}

static bool fmt_atom_eq(const FmtNode *op, const char *name) {
    return op->kind == FMT_ATOM && op->text_len == strlen(name) &&
           memcmp(op->text, name, op->text_len) == 0;
}

static int fmt_body_distinguished(const char *name, size_t name_len) {
    static const struct {
        const char *name;
        int n;
    } table[] = {
        {"lambda", 1},           {"define", 1},           {"define-values", 1},
        {"define-syntax", 1},      {"define-record-type", 1}, {"define-library", 1},
        {"let*", 1},               {"letrec", 1},           {"letrec*", 1},
        {"let-values", 1},         {"let*-values", 1},      {"let-syntax", 1},
        {"letrec-syntax", 1},      {"when", 1},             {"unless", 1},
        {"begin", 0},              {"if", 1},               {"cond", 1},
        {"case", 1},               {"do", 2},               {"parameterize", 1},
        {"guard", 1},              {"syntax-rules", 1},     {"case-lambda", 0},
        {"receive", 2},            {"test-group", 1},       {"test-group-with-cleanup", 1},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strlen(table[i].name) == name_len && memcmp(table[i].name, name, name_len) == 0) {
            return table[i].n;
        }
    }
    return -1;
}

static FmtStyle fmt_style_of(const FmtNode *node) {
    if (node->child_count == 0) {
        return (FmtStyle){.kind = FMT_STYLE_CALL, .body_n = 0};
    }
    const FmtNode *op = &node->children[0];
    if (op->kind != FMT_ATOM) {
        return (FmtStyle){.kind = FMT_STYLE_CALL, .body_n = 0};
    }
    if (fmt_atom_eq(op, "let")) {
        if (node->child_count >= 2 && node->children[1].kind == FMT_ATOM) {
            return (FmtStyle){.kind = FMT_STYLE_BODY, .body_n = 2};
        }
        return (FmtStyle){.kind = FMT_STYLE_BODY, .body_n = 1};
    }
    int n = fmt_body_distinguished(op->text, op->text_len);
    if (n >= 0) {
        return (FmtStyle){.kind = FMT_STYLE_BODY, .body_n = (size_t)n};
    }
    return (FmtStyle){.kind = FMT_STYLE_CALL, .body_n = 0};
}

static size_t fmt_compute_measure(FmtNode *node);

static size_t fmt_measure(FmtNode *node) {
    if (node->width_computed) {
        return node->inline_width;
    }
    node->inline_width = fmt_compute_measure(node);
    node->width_computed = true;
    return node->inline_width;
}

static size_t fmt_compute_measure(FmtNode *node) {
    switch (node->kind) {
    case FMT_LINE_COMMENT:
        return SIZE_MAX;
    case FMT_ATOM:
    case FMT_BLOCK_COMMENT:
        if (memchr(node->text, '\n', node->text_len)) {
            return SIZE_MAX;
        }
        return node->text_len;
    case FMT_PREFIX: {
        size_t cw = fmt_measure(&node->children[0]);
        if (cw == SIZE_MAX) {
            return SIZE_MAX;
        }
        return node->text_len + cw;
    }
    case FMT_DATUM_COMMENT: {
        size_t cw = fmt_measure(&node->children[0]);
        if (cw == SIZE_MAX) {
            return SIZE_MAX;
        }
        return 2 + cw;
    }
    case FMT_LIST: {
        size_t total = node->text_len + 1;
        for (size_t i = 0; i < node->child_count; i++) {
            size_t w = fmt_measure(&node->children[i]);
            if (w == SIZE_MAX) {
                return SIZE_MAX;
            }
            total += w;
            if (i > 0) {
                total++;
            }
        }
        return total;
    }
    default:
        return SIZE_MAX;
    }
}

static bool fmt_has_body_blank(const FmtNode *node) {
    if (node->kind != FMT_LIST) {
        return false;
    }
    size_t op_idx = fmt_first_code_index(node->children, node->child_count);
    size_t first_body;
    if (op_idx == SIZE_MAX || op_idx != 0 || node->is_data) {
        first_body = 1;
    } else {
        FmtStyle st = fmt_style_of(node);
        if (st.kind == FMT_STYLE_BODY) {
            first_body = st.body_n + 1;
        } else {
            first_body = 2;
        }
    }
    if (first_body >= node->child_count) {
        return false;
    }
    for (size_t i = first_body; i < node->child_count; i++) {
        if (node->children[i].newlines_before >= 2) {
            return true;
        }
    }
    return false;
}

/* ── Printer ───────────────────────────────────────────────────────────── */

static bool fmt_printer_raw(FmtPrinter *p, const char *s, size_t n) {
    if (!fmt_buf_append(p->out, s, n)) {
        return false;
    }
    const char *nl = NULL;
    for (size_t i = n; i > 0; i--) {
        if (s[i - 1] == '\n') {
            nl = s + i - 1;
            break;
        }
    }
    if (nl) {
        p->col = n - (size_t)(nl - s) - 1;
    } else {
        p->col += n;
    }
    p->line_comment_open = false;
    return true;
}

static bool fmt_printer_spaces(FmtPrinter *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!fmt_buf_append_c(p->out, ' ')) {
            return false;
        }
    }
    p->col += n;
    return true;
}

static bool fmt_printer_newline_to(FmtPrinter *p, size_t indent) {
    if (!fmt_buf_append_c(p->out, '\n')) {
        return false;
    }
    if (!fmt_printer_spaces(p, indent)) {
        return false;
    }
    p->line_comment_open = false;
    return true;
}

static bool fmt_printer_blank_line(FmtPrinter *p) {
    if (!fmt_buf_append_c(p->out, '\n')) {
        return false;
    }
    return true;
}

static bool fmt_printer_emit_node(FmtPrinter *p, FmtNode *node);
static bool fmt_printer_emit_inline(FmtPrinter *p, FmtNode *node);
static bool fmt_printer_emit_body_from(FmtPrinter *p, FmtNode *children, size_t n, size_t indent,
                                       bool first_inline);
static bool fmt_printer_emit_broken_list(FmtPrinter *p, FmtNode *node, size_t start_col);
static bool fmt_printer_close_paren(FmtPrinter *p, size_t paren_col);

static bool fmt_printer_emit_list(FmtPrinter *p, FmtNode *node) {
    size_t start_col = p->col;
    size_t w = fmt_measure(node);
    if (w != SIZE_MAX && start_col + w <= FMT_MAX_WIDTH && !fmt_has_body_blank(node)) {
        return fmt_printer_emit_inline(p, node);
    }
    return fmt_printer_emit_broken_list(p, node, start_col);
}

static bool fmt_printer_emit_node(FmtPrinter *p, FmtNode *node) {
    switch (node->kind) {
    case FMT_ATOM:
    case FMT_BLOCK_COMMENT:
        return fmt_printer_raw(p, node->text, node->text_len);
    case FMT_LINE_COMMENT:
        if (!fmt_printer_raw(p, node->text, node->text_len)) {
            return false;
        }
        p->line_comment_open = true;
        return true;
    case FMT_PREFIX:
        if (!fmt_printer_raw(p, node->text, node->text_len)) {
            return false;
        }
        return fmt_printer_emit_node(p, &node->children[0]);
    case FMT_DATUM_COMMENT:
        if (!fmt_printer_raw(p, "#;", 2)) {
            return false;
        }
        return fmt_printer_emit_node(p, &node->children[0]);
    case FMT_LIST:
        return fmt_printer_emit_list(p, node);
    default:
        return false;
    }
}

static bool fmt_printer_emit_inline(FmtPrinter *p, FmtNode *node) {
    switch (node->kind) {
    case FMT_ATOM:
    case FMT_BLOCK_COMMENT:
    case FMT_LINE_COMMENT:
        return fmt_printer_raw(p, node->text, node->text_len);
    case FMT_PREFIX:
        if (!fmt_printer_raw(p, node->text, node->text_len)) {
            return false;
        }
        return fmt_printer_emit_inline(p, &node->children[0]);
    case FMT_DATUM_COMMENT:
        if (!fmt_printer_raw(p, "#;", 2)) {
            return false;
        }
        return fmt_printer_emit_inline(p, &node->children[0]);
    case FMT_LIST:
        if (!fmt_printer_raw(p, node->text, node->text_len)) {
            return false;
        }
        for (size_t i = 0; i < node->child_count; i++) {
            if (i > 0 && !fmt_printer_spaces(p, 1)) {
                return false;
            }
            if (!fmt_printer_emit_inline(p, &node->children[i])) {
                return false;
            }
        }
        return fmt_printer_raw(p, ")", 1);
    default:
        return false;
    }
}

static bool fmt_printer_emit_body_from(FmtPrinter *p, FmtNode *children, size_t n, size_t indent,
                                       bool first_inline) {
    bool started = false;
    for (size_t i = 0; i < n; i++) {
        FmtNode *child = &children[i];
        bool on_own_line = !(first_inline && !started);
        if (fmt_is_comment(child->kind) && child->newlines_before == 0 && started &&
            !p->line_comment_open) {
            if (!fmt_printer_spaces(p, 1)) {
                return false;
            }
            if (!fmt_printer_emit_node(p, child)) {
                return false;
            }
            continue;
        }
        if (on_own_line) {
            if (child->newlines_before >= 2 && !fmt_printer_blank_line(p)) {
                return false;
            }
            if (!fmt_printer_newline_to(p, indent)) {
                return false;
            }
        }
        if (!fmt_printer_emit_node(p, child)) {
            return false;
        }
        started = true;
    }
    return true;
}

static bool fmt_printer_emit_body(FmtPrinter *p, FmtNode *children, size_t n, size_t indent) {
    return fmt_printer_emit_body_from(p, children, n, indent, false);
}

static bool fmt_printer_close_paren(FmtPrinter *p, size_t paren_col) {
    if (p->line_comment_open) {
        if (!fmt_printer_newline_to(p, paren_col)) {
            return false;
        }
    }
    return fmt_printer_raw(p, ")", 1);
}

static bool fmt_printer_emit_body_style(FmtPrinter *p, FmtNode *node, size_t start_col, size_t n) {
    FmtNode *children = node->children;
    size_t count = node->child_count;
    size_t placed = 0;
    size_t i = 0;
    while (i < count && placed <= n) {
        if (fmt_is_comment(children[i].kind)) {
            break;
        }
        if (placed > 0 && !fmt_printer_spaces(p, 1)) {
            return false;
        }
        if (!fmt_printer_emit_node(p, &children[i])) {
            return false;
        }
        placed++;
        i++;
        while (i < count && fmt_is_comment(children[i].kind) && children[i].newlines_before == 0) {
            if (!fmt_printer_spaces(p, 1)) {
                return false;
            }
            if (!fmt_printer_emit_node(p, &children[i])) {
                return false;
            }
            bool was_line = children[i].kind == FMT_LINE_COMMENT;
            i++;
            if (was_line) {
                return fmt_printer_emit_body(p, &children[i], count - i, start_col + FMT_INDENT_STEP);
            }
        }
    }
    return fmt_printer_emit_body(p, &children[i], count - i, start_col + FMT_INDENT_STEP);
}

static bool fmt_printer_emit_call_style(FmtPrinter *p, FmtNode *node, size_t open_col) {
    FmtNode *children = node->children;
    size_t count = node->child_count;
    if (!fmt_printer_emit_node(p, &children[0])) {
        return false;
    }
    size_t i = 1;
    while (i < count && fmt_is_comment(children[i].kind) && children[i].newlines_before == 0) {
        if (!fmt_printer_spaces(p, 1)) {
            return false;
        }
        if (!fmt_printer_emit_node(p, &children[i])) {
            return false;
        }
        if (children[i].kind == FMT_LINE_COMMENT) {
            i++;
            return fmt_printer_emit_body(p, &children[i], count - i, open_col);
        }
        i++;
    }
    if (i < count && !fmt_is_comment(children[i].kind)) {
        if (!fmt_printer_spaces(p, 1)) {
            return false;
        }
        size_t align_col = p->col;
        if (!fmt_printer_emit_node(p, &children[i])) {
            return false;
        }
        i++;
        while (i < count && fmt_is_comment(children[i].kind) && children[i].newlines_before == 0) {
            if (!fmt_printer_spaces(p, 1)) {
                return false;
            }
            if (!fmt_printer_emit_node(p, &children[i])) {
                return false;
            }
            if (children[i].kind == FMT_LINE_COMMENT) {
                i++;
                return fmt_printer_emit_body(p, &children[i], count - i, align_col);
            }
            i++;
        }
        return fmt_printer_emit_body(p, &children[i], count - i, align_col);
    }
    return fmt_printer_emit_body(p, &children[i], count - i, open_col);
}

static bool fmt_printer_emit_broken_list(FmtPrinter *p, FmtNode *node, size_t start_col) {
    if (!fmt_printer_raw(p, node->text, node->text_len)) {
        return false;
    }
    size_t open_col = p->col;
    size_t op_idx = fmt_first_code_index(node->children, node->child_count);
    if (op_idx == SIZE_MAX || op_idx != 0 || node->is_data) {
        if (!fmt_printer_emit_body_from(p, node->children, node->child_count, open_col, true)) {
            return false;
        }
        return fmt_printer_close_paren(p, start_col);
    }
    FmtStyle st = fmt_style_of(node);
    if (st.kind == FMT_STYLE_BODY) {
        if (!fmt_printer_emit_body_style(p, node, start_col, st.body_n)) {
            return false;
        }
    } else if (!fmt_printer_emit_call_style(p, node, open_col)) {
        return false;
    }
    return fmt_printer_close_paren(p, start_col);
}

static bool fmt_printer_emit_top_level(FmtPrinter *p, FmtNode *nodes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        FmtNode *node = &nodes[i];
        if (i > 0) {
            if (fmt_is_comment(node->kind) && node->newlines_before == 0 && !p->line_comment_open) {
                if (!fmt_printer_spaces(p, 1)) {
                    return false;
                }
                if (!fmt_printer_emit_node(p, node)) {
                    return false;
                }
                continue;
            }
            if (node->newlines_before >= 2 && !fmt_printer_blank_line(p)) {
                return false;
            }
            if (!fmt_printer_newline_to(p, 0)) {
                return false;
            }
        }
        if (!fmt_printer_emit_node(p, node)) {
            return false;
        }
    }
    return true;
}

static bool fmt_print_nodes(FmtNode *nodes, size_t count, FmtBuf *out) {
    FmtPrinter p = {.out = out, .col = 0, .line_comment_open = false};
    if (!fmt_printer_emit_top_level(&p, nodes, count)) {
        return false;
    }
    if (out->len == 0 || out->data[out->len - 1] != '\n') {
        if (!fmt_buf_append_c(out, '\n')) {
            return false;
        }
    }
    return true;
}

/* ── Round-trip verification ───────────────────────────────────────────── */

typedef struct FmtValueList {
    ChValue *items;
    size_t count;
    size_t cap;
} FmtValueList;

static bool fmt_value_list_append(FmtValueList *list, ChValue v) {
    if (list->count >= list->cap) {
        size_t next_cap = list->cap ? list->cap * 2 : 16;
        ChValue *next = (ChValue *)realloc(list->items, next_cap * sizeof(ChValue));
        if (!next) {
            return false;
        }
        list->items = next;
        list->cap = next_cap;
    }
    list->items[list->count++] = v;
    return true;
}

static bool fmt_read_all(ChGC *gc, const char *src, size_t len, FmtValueList *out) {
    ChReader reader;
    ch_reader_init(&reader, gc, src, len);
    while (true) {
        ChValue v = CH_NIL;
        ch_gc_push(gc, &v);
        ChReadStatus st = ch_read_datum(&reader, &v);
        if (st == CH_READ_EOF) {
            ch_gc_pop(gc);
            break;
        }
        if (st != CH_READ_OK) {
            ch_gc_pop(gc);
            return false;
        }
        if (!fmt_value_list_append(out, v)) {
            ch_gc_pop(gc);
            return false;
        }
        ChValue keep = v;
        ch_gc_push(gc, &keep);
        ch_gc_pop(gc);
        ch_gc_pop(gc);
    }
    return true;
}

static bool fmt_verify_round_trip(ChGC *gc, const char *original, size_t orig_len,
                                  const char *formatted, size_t fmt_len) {
    FmtValueList orig = {0};
    FmtValueList fmtv = {0};
    ChValue orig_roots = CH_NIL;
    ChValue fmt_roots = CH_NIL;
    ch_gc_push(gc, &orig_roots);
    ch_gc_push(gc, &fmt_roots);

    bool ok = fmt_read_all(gc, original, orig_len, &orig);
    if (ok) {
        for (size_t i = 0; i < orig.count; i++) {
            ChValue keep = orig.items[i];
            ch_gc_push(gc, &keep);
            orig_roots = ch_gc_cons(gc, keep, orig_roots);
            ch_gc_pop(gc);
        }
        ok = fmt_read_all(gc, formatted, fmt_len, &fmtv);
    }
    if (ok) {
        for (size_t i = 0; i < fmtv.count; i++) {
            ChValue keep = fmtv.items[i];
            ch_gc_push(gc, &keep);
            fmt_roots = ch_gc_cons(gc, keep, fmt_roots);
            ch_gc_pop(gc);
        }
        if (orig.count != fmtv.count) {
            ok = false;
        } else {
            for (size_t i = 0; i < orig.count; i++) {
                if (!ch_equal(orig.items[i], fmtv.items[i])) {
                    ok = false;
                    break;
                }
            }
        }
    }

    ch_gc_pop(gc);
    ch_gc_pop(gc);
    free(orig.items);
    free(fmtv.items);
    return ok;
}

static bool fmt_same_text_ignore_trailing_nl(const char *a, size_t a_len, const char *b, size_t b_len) {
    while (a_len > 0 && (a[a_len - 1] == '\n' || a[a_len - 1] == '\r')) {
        a_len--;
    }
    while (b_len > 0 && (b[b_len - 1] == '\n' || b[b_len - 1] == '\r')) {
        b_len--;
    }
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static bool fmt_write_file(const char *path, const char *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    if (len > 0 && fwrite(buf, 1, len, f) != len) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

/* ── Public API ────────────────────────────────────────────────────────── */

int ch_fmt_source(const char *src, size_t len, char **out_text, size_t *out_len, char *err_out,
                  size_t err_len) {
    if (!src || !out_text || !out_len) {
        if (err_out && err_len > 0) {
            snprintf(err_out, err_len, "invalid arguments");
        }
        return -1;
    }

    FmtArena arena;
    fmt_arena_init(&arena);
    if (!arena.head) {
        if (err_out && err_len > 0) {
            snprintf(err_out, err_len, "out of memory");
        }
        return -1;
    }

    FmtParser parser = {
        .lex = {.src = src, .len = len, .pos = 0},
        .arena = &arena,
        .depth = 0,
    };
    FmtNode *nodes = NULL;
    size_t node_count = 0;
    FmtParseErr perr = fmt_parse_program(&parser, &nodes, &node_count);
    if (perr != FMT_PARSE_OK) {
        if (err_out && err_len > 0) {
            snprintf(err_out, err_len, "%s", fmt_parse_err_msg(perr));
        }
        fmt_arena_free(&arena);
        return -1;
    }

    FmtBuf out = {0};
    if (!fmt_print_nodes(nodes, node_count, &out)) {
        fmt_buf_free(&out);
        fmt_arena_free(&arena);
        if (err_out && err_len > 0) {
            snprintf(err_out, err_len, "out of memory");
        }
        return -1;
    }

    char *text = (char *)malloc(out.len + 1);
    if (!text) {
        fmt_buf_free(&out);
        fmt_arena_free(&arena);
        if (err_out && err_len > 0) {
            snprintf(err_out, err_len, "out of memory");
        }
        return -1;
    }
    memcpy(text, out.data, out.len);
    text[out.len] = '\0';

    *out_text = text;
    *out_len = out.len;
    fmt_buf_free(&out);
    fmt_arena_free(&arena);
    return 0;
}

int ch_fmt_file(const char *path, int check_only, const char *output_path) {
    if (!path) {
        fprintf(stderr, "fmt: missing file\n");
        return 1;
    }

    size_t src_len = 0;
    char *src = ch_read_file(path, &src_len);
    if (!src) {
        fprintf(stderr, "fmt: cannot read '%s'\n", path);
        return 1;
    }
    if (src_len > FMT_MAX_FILE_BYTES) {
        fprintf(stderr, "fmt: %s: file too large\n", path);
        free(src);
        return 1;
    }

    char *formatted = NULL;
    size_t fmt_len = 0;
    char err[256];
    if (ch_fmt_source(src, src_len, &formatted, &fmt_len, err, sizeof(err)) != 0) {
        fprintf(stderr, "fmt: %s: %s\n", path, err);
        free(src);
        return 1;
    }

    ChGC gc;
    ch_gc_init(&gc);
    if (!fmt_verify_round_trip(&gc, src, src_len, formatted, fmt_len)) {
        fprintf(stderr,
                "fmt: %s: internal error: formatting would change the program; file left unchanged\n",
                path);
        ch_gc_deinit(&gc);
        free(formatted);
        free(src);
        return 1;
    }
    ch_gc_deinit(&gc);

    if (fmt_same_text_ignore_trailing_nl(src, src_len, formatted, fmt_len)) {
        if (check_only) {
            printf("fmt: ok %s\n", path);
            free(formatted);
            free(src);
            return 0;
        }
        if (!output_path) {
            free(formatted);
            free(src);
            return 0;
        }
        /* Explicit -o: write even when already canonical. */
    } else if (check_only) {
        fprintf(stderr, "fmt: %s would be reformatted\n", path);
        free(formatted);
        free(src);
        return 1;
    }

    const char *dest = output_path ? output_path : path;
    if (!fmt_write_file(dest, formatted, fmt_len)) {
        fprintf(stderr, "fmt: cannot write '%s'\n", dest);
        free(formatted);
        free(src);
        return 1;
    }

    printf("fmt: wrote %s\n", dest);
    free(formatted);
    free(src);
    return 0;
}
