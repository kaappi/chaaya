#include "chaaya/prim.h"

#include "prim_port_internal.h"

#include "chaaya/printer.h"
#include "chaaya/reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_to_port(ChPort *p, ChValue v, ChPrintMode mode) {
    FILE *f = port_file(p);
    if (f) {
        ch_print_value_mode(f, v, mode);
        return;
    }
    char *s = ch_value_to_string_mode(v, mode);
    if (s) {
        port_write_bytes(p, s, strlen(s));
        free(s);
    }
}

static ChValue prim_display(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    print_to_port(p, args[0], CH_PRINT_DISPLAY);
    return CH_VOID;
}

static ChValue prim_write(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    print_to_port(p, args[0], CH_PRINT_WRITE);
    return CH_VOID;
}

static ChValue prim_write_shared(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    print_to_port(p, args[0], CH_PRINT_SHARED);
    return CH_VOID;
}

static ChValue prim_write_simple(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    print_to_port(p, args[0], CH_PRINT_SIMPLE);
    return CH_VOID;
}

static ChValue prim_newline(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_output_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    int wrc = port_write_bytes_vm(vm, p, "\n", 1, 1);
    if (wrc == -2) {
        return CH_UNDEFINED;
    }
    if (wrc != 0) {
        snprintf(vm->error, sizeof(vm->error), "newline: write failed");
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static int port_peek_byte(ChVM *vm, ChPort *p) {
    int ready = ensure_port_input_byte(vm, p, 1);
    if (ready == -2) {
        return -2;
    }
    if (ready <= 0) {
        return -1;
    }
    return (unsigned char)p->buf[p->pos];
}

static int port_read_byte(ChVM *vm, ChPort *p) {
    int ready = ensure_port_input_byte(vm, p, 1);
    if (ready == -2) {
        return -2;
    }
    if (ready <= 0) {
        return -1;
    }
    return (unsigned char)p->buf[p->pos++];
}

static int utf8_seq_len(unsigned char b0) {
    if (b0 < 0x80) {
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        return 2;
    }
    if ((b0 & 0xF0) == 0xE0) {
        return 3;
    }
    if ((b0 & 0xF8) == 0xF0) {
        return 4;
    }
    return -1;
}

static int port_decode_utf8_at(ChVM *vm, ChPort *p, size_t pos, uint32_t *cp_out, size_t *next_out) {
    int ready0 = ensure_port_input_byte(vm, p, 1);
    if (ready0 == -2) {
        return -2;
    }
    if (ready0 <= 0 && pos >= p->len) {
        return -1;
    }
    if (pos >= p->len) {
        return -1;
    }
    unsigned char b0 = (unsigned char)p->buf[pos];
    int n = utf8_seq_len(b0);
    if (n < 0) {
        /* Invalid lead byte: expose it as a single Latin-1 code unit. */
        *cp_out = b0;
        *next_out = pos + 1;
        return 0;
    }
    while (pos + (size_t)n > p->len) {
        size_t old_len = p->len;
        int ready = ensure_port_input_byte(vm, p, 1);
        if (ready == -2) {
            return -2;
        }
        if (ready <= 0) {
            break;
        }
        /* Bytevector/string ports report a ready byte at `pos` even when the
         * remaining sequence is truncated — stop if the buffer did not grow. */
        if (p->len == old_len) {
            break;
        }
    }
    if (pos + (size_t)n > p->len) {
        /* Truncated multi-byte sequence (#518): return the lead byte so a
         * subsequent read-u8 sees the original stream bytes, not EOF. */
        *cp_out = b0;
        *next_out = pos + 1;
        return 0;
    }
    uint32_t cp = 0;
    if (n == 1) {
        cp = b0;
    } else if (n == 2) {
        cp = ((uint32_t)(b0 & 0x1F) << 6) | ((unsigned char)p->buf[pos + 1] & 0x3F);
    } else if (n == 3) {
        cp = ((uint32_t)(b0 & 0x0F) << 12) | (((unsigned char)p->buf[pos + 1] & 0x3F) << 6) |
             ((unsigned char)p->buf[pos + 2] & 0x3F);
    } else {
        cp = ((uint32_t)(b0 & 0x07) << 18) | (((unsigned char)p->buf[pos + 1] & 0x3F) << 12) |
             (((unsigned char)p->buf[pos + 2] & 0x3F) << 6) | ((unsigned char)p->buf[pos + 3] & 0x3F);
    }
    *cp_out = cp;
    *next_out = pos + (size_t)n;
    return 0;
}

static ChValue prim_read_char(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    uint32_t cp = 0;
    size_t next = 0;
    int rc = port_decode_utf8_at(vm, p, p->pos, &cp, &next);
    if (rc == -2) {
        return CH_UNDEFINED;
    }
    if (rc != 0) {
        return CH_EOF_OBJ;
    }
    p->pos = next;
    return ch_make_char(cp);
}

static ChValue prim_peek_char(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    uint32_t cp = 0;
    size_t next = 0;
    int rc = port_decode_utf8_at(vm, p, p->pos, &cp, &next);
    if (rc == -2) {
        return CH_UNDEFINED;
    }
    if (rc != 0) {
        return CH_EOF_OBJ;
    }
    return ch_make_char(cp);
}

static ChValue prim_char_ready_p(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    int ready = ensure_port_input_byte(vm, p, 0);
    if (ready < 0) {
        return CH_UNDEFINED;
    }
    return ready > 0 ? CH_TRUE : CH_FALSE;
}

static ChValue prim_write_char(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "write-char: expected char");
        return CH_UNDEFINED;
    }
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    uint32_t cp = ch_to_char(args[0]);
    char encoded[4];
    size_t n = 0;
    if (cp <= 0x7Fu) {
        encoded[0] = (char)cp;
        n = 1;
    } else if (cp <= 0x7FFu) {
        encoded[0] = (char)(0xC0u | (cp >> 6));
        encoded[1] = (char)(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp <= 0xFFFFu) {
        encoded[0] = (char)(0xE0u | (cp >> 12));
        encoded[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[2] = (char)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else if (cp <= 0x10FFFFu) {
        encoded[0] = (char)(0xF0u | (cp >> 18));
        encoded[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        encoded[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[3] = (char)(0x80u | (cp & 0x3Fu));
        n = 4;
    } else {
        snprintf(vm->error, sizeof(vm->error), "write-char: invalid character");
        return CH_UNDEFINED;
    }
    int wrc = port_write_bytes_vm(vm, p, encoded, n, 1);
    if (wrc == -2) {
        return CH_UNDEFINED;
    }
    if (wrc != 0) {
        snprintf(vm->error, sizeof(vm->error), "write-char: write failed");
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_write_string(ChVM *vm, ChValue *args, int nargs) {
    /* (write-string string [port [start [end]]]) — start/end are byte offsets. */
    if (nargs < 1 || !ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "write-string: expected string");
        return CH_UNDEFINED;
    }
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t start = 0;
    size_t end = s->len;
    int range_arg = (nargs > 1 && ch_is_port(args[1])) ? 2 : 1;
    if (nargs > range_arg) {
        if (!ch_is_fixnum(args[range_arg])) {
            snprintf(vm->error, sizeof(vm->error), "write-string: bad start");
            return CH_UNDEFINED;
        }
        int64_t st = ch_to_fixnum(args[range_arg]);
        if (st < 0 || (size_t)st > s->len) {
            snprintf(vm->error, sizeof(vm->error), "write-string: start out of range");
            return CH_UNDEFINED;
        }
        start = (size_t)st;
    }
    if (nargs > range_arg + 1) {
        if (!ch_is_fixnum(args[range_arg + 1])) {
            snprintf(vm->error, sizeof(vm->error), "write-string: bad end");
            return CH_UNDEFINED;
        }
        int64_t en = ch_to_fixnum(args[range_arg + 1]);
        if (en < (int64_t)start || (size_t)en > s->len) {
            snprintf(vm->error, sizeof(vm->error), "write-string: end out of range");
            return CH_UNDEFINED;
        }
        end = (size_t)en;
    }
    if (end > start) {
        int wrc = port_write_bytes_vm(vm, p, s->data + start, end - start, 1);
        if (wrc == -2) {
            return CH_UNDEFINED;
        }
        if (wrc != 0) {
            snprintf(vm->error, sizeof(vm->error), "write-string: write failed");
            return CH_UNDEFINED;
        }
    }
    return CH_VOID;
}

static ChValue prim_read_line(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    for (;;) {
        int c = port_read_byte(vm, p);
        if (c == -2) {
            free(buf);
            return CH_UNDEFINED;
        }
        if (c < 0) {
            if (len == 0) {
                free(buf);
                return CH_EOF_OBJ;
            }
            break;
        }
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            int peek = port_peek_byte(vm, p);
            if (peek == -2) {
                free(buf);
                return CH_UNDEFINED;
            }
            if (peek == '\n') {
                (void)port_read_byte(vm, p);
            }
            break;
        }
        if (len + 1 >= cap) {
            size_t ncap = cap ? cap * 2 : 64;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                abort();
            }
            buf = nb;
            cap = ncap;
        }
        buf[len++] = (char)c;
    }
    if (buf) {
        buf[len] = '\0';
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", len);
    free(buf);
    return out;
}

static ChValue prim_read_string(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_fixnum(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "read-string: expected k");
        return CH_UNDEFINED;
    }
    int64_t k = ch_to_fixnum(args[0]);
    if (k < 0) {
        snprintf(vm->error, sizeof(vm->error), "read-string: negative length");
        return CH_UNDEFINED;
    }
    ChPort *p = require_input_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    if (k == 0) {
        return ch_gc_make_string(&vm->gc, "", 0);
    }
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    for (int64_t i = 0; i < k; i++) {
        uint32_t cp = 0;
        size_t next = 0;
        int rc = port_decode_utf8_at(vm, p, p->pos, &cp, &next);
        if (rc == -2) {
            free(buf);
            return CH_UNDEFINED;
        }
        if (rc != 0) {
            if (i == 0) {
                free(buf);
                return CH_EOF_OBJ;
            }
            break;
        }
        p->pos = next;
        char encoded[4];
        size_t n = next; /* placeholder */
        /* re-encode from cp */
        if (cp <= 0x7Fu) {
            encoded[0] = (char)cp;
            n = 1;
        } else if (cp <= 0x7FFu) {
            encoded[0] = (char)(0xC0u | (cp >> 6));
            encoded[1] = (char)(0x80u | (cp & 0x3Fu));
            n = 2;
        } else if (cp <= 0xFFFFu) {
            encoded[0] = (char)(0xE0u | (cp >> 12));
            encoded[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            encoded[2] = (char)(0x80u | (cp & 0x3Fu));
            n = 3;
        } else {
            encoded[0] = (char)(0xF0u | (cp >> 18));
            encoded[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            encoded[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            encoded[3] = (char)(0x80u | (cp & 0x3Fu));
            n = 4;
        }
        if (len + n >= cap) {
            size_t ncap = cap ? cap * 2 : 64;
            while (len + n >= ncap) {
                ncap *= 2;
            }
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                abort();
            }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, encoded, n);
        len += n;
    }
    if (buf) {
        buf[len] = '\0';
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", len);
    free(buf);
    return out;
}

static int parse_port_slice(ChVM *vm, ChValue *args, int nargs, int start_arg, size_t len, const char *who,
                            size_t *start_out, size_t *end_out) {
    size_t start = 0;
    size_t end = len;
    if (nargs > start_arg) {
        if (parse_nonnegative_fixnum(vm, args[start_arg], &start, who) != 0) {
            return -1;
        }
    }
    if (nargs > start_arg + 1) {
        if (parse_nonnegative_fixnum(vm, args[start_arg + 1], &end, who) != 0) {
            return -1;
        }
    }
    if (start > end || end > len) {
        snprintf(vm->error, sizeof(vm->error), "%s: slice out of range", who);
        return -1;
    }
    *start_out = start;
    *end_out = end;
    return 0;
}

static ChValue prim_read_u8(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    int c = port_read_byte(vm, p);
    if (c == -2) {
        return CH_UNDEFINED;
    }
    if (c < 0) {
        return CH_EOF_OBJ;
    }
    return ch_make_fixnum(c);
}

static ChValue prim_peek_u8(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    int c = port_peek_byte(vm, p);
    if (c == -2) {
        return CH_UNDEFINED;
    }
    if (c < 0) {
        return CH_EOF_OBJ;
    }
    return ch_make_fixnum(c);
}

static ChValue prim_write_u8(ChVM *vm, ChValue *args, int nargs) {
    uint8_t b = 0;
    if (parse_u8(vm, args[0], &b, "write-u8") != 0) {
        return CH_UNDEFINED;
    }
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    char byte = (char)b;
    int wrc = port_write_bytes_vm(vm, p, &byte, 1, 1);
    if (wrc == -2) {
        return CH_UNDEFINED;
    }
    if (wrc != 0) {
        snprintf(vm->error, sizeof(vm->error), "write-u8: write failed");
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_u8_ready_p(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    (void)p;
    return CH_TRUE;
}

static ChValue prim_read_bytevector(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 2) {
        snprintf(vm->error, sizeof(vm->error), "read-bytevector: expected 1 or 2 arguments");
        return CH_UNDEFINED;
    }
    size_t k = 0;
    if (parse_nonnegative_fixnum(vm, args[0], &k, "read-bytevector") != 0) {
        return CH_UNDEFINED;
    }
    ChPort *p = require_input_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    if (k == 0) {
        return ch_gc_make_bytevector(&vm->gc, 0, 0);
    }
    size_t cap = k < 256 ? k : 256;
    if (cap == 0) {
        cap = 1;
    }
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        abort();
    }
    size_t n = 0;
    while (n < k) {
        int c = port_read_byte(vm, p);
        if (c == -2) {
            free(buf);
            return CH_UNDEFINED;
        }
        if (c < 0) {
            break;
        }
        if (n == cap) {
            size_t ncap = cap * 2;
            if (ncap > k) {
                ncap = k;
            }
            uint8_t *nb = (uint8_t *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                abort();
            }
            buf = nb;
            cap = ncap;
        }
        buf[n++] = (uint8_t)c;
    }
    if (n == 0) {
        free(buf);
        return CH_EOF_OBJ;
    }
    ChValue out = ch_gc_make_bytevector(&vm->gc, n, 0);
    ChBytevector *bv = ch_as_bytevector(out);
    memcpy(bv->data, buf, n);
    free(buf);
    return out;
}

static ChValue prim_read_bytevector_bang(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 4) {
        snprintf(vm->error, sizeof(vm->error), "read-bytevector!: expected 1 to 4 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "read-bytevector!: not a bytevector");
        return CH_UNDEFINED;
    }
    ChBytevector *target = ch_as_bytevector(args[0]);
    if (ch_object_is_immutable(&target->header)) {
        snprintf(vm->error, sizeof(vm->error), "read-bytevector!: immutable bytevector");
        return CH_UNDEFINED;
    }
    ChPort *p = require_input_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = target->len;
    if (parse_port_slice(vm, args, nargs, 2, target->len, "read-bytevector!", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t count = end - start;
    if (count == 0) {
        return ch_make_fixnum(0);
    }
    size_t n = 0;
    while (n < count) {
        int c = port_read_byte(vm, p);
        if (c == -2) {
            return CH_UNDEFINED;
        }
        if (c < 0) {
            break;
        }
        target->data[start + n] = (uint8_t)c;
        n++;
    }
    if (n == 0) {
        return CH_EOF_OBJ;
    }
    return ch_make_fixnum((int64_t)n);
}

static ChValue prim_write_bytevector(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 4) {
        snprintf(vm->error, sizeof(vm->error), "write-bytevector: expected 1 to 4 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "write-bytevector: not a bytevector");
        return CH_UNDEFINED;
    }
    ChBytevector *bv = ch_as_bytevector(args[0]);
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = bv->len;
    if (parse_port_slice(vm, args, nargs, 2, bv->len, "write-bytevector", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    if (end > start) {
        int wrc =
            port_write_bytes_vm(vm, p, (const char *)bv->data + start, end - start, 1);
        if (wrc == -2) {
            return CH_UNDEFINED;
        }
        if (wrc != 0) {
            snprintf(vm->error, sizeof(vm->error), "write-bytevector: write failed");
            return CH_UNDEFINED;
        }
    }
    return CH_VOID;
}

static bool reader_refill_from_port(ChReader *r, void *ctx) {
    ChPort *p = (ChPort *)ctx;
    if (!p || p->closed || !p->input || (p->kind != CH_PORT_STDIO && p->kind != CH_PORT_FILE)) {
        return false;
    }
    int appended = append_file_input_bytes(NULL, p, 0);
    r->src = p->buf ? p->buf + p->pos : "";
    r->len = p->len - p->pos;
    return appended > 0;
}

static ChValue prim_read(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        compact_port_input_buffer(p);
    }

    ChReader r;
    ch_reader_init(&r, &vm->gc, p->buf ? p->buf + p->pos : "", p->len - p->pos);
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        ch_reader_set_refill(&r, reader_refill_from_port, p);
    }

    ChValue out = CH_NIL;
    ch_gc_push(&vm->gc, &out);
    ChReadStatus st = ch_read_datum(&r, &out);
    ch_gc_pop(&vm->gc);
    if (st == CH_READ_EOF) {
        p->pos += r.pos;
        return CH_EOF_OBJ;
    }
    if (st != CH_READ_OK) {
        return raise_read_error(vm, ch_reader_error(&r),
                                (nargs > 0) ? args[0] : get_global(vm, "current-input-port"));
    }
    p->pos += r.pos;
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        compact_port_input_buffer(p);
    }
    return out;
}

static bool port_can_position(ChPort *port) {
    if (port->closed) {
        return false;
    }
    switch (port->kind) {
    case CH_PORT_STRING_IN:
    case CH_PORT_STRING_OUT:
    case CH_PORT_BYTEVECTOR:
        return true;
    case CH_PORT_FILE:
        if (port->file) {
            long cur = ftell(port->file);
            return cur >= 0;
        }
        return false;
    default:
        return false;
    }
}

static ChValue prim_port_position(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_port(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "port-position: expected port");
        return CH_UNDEFINED;
    }
    ChPort *port = ch_as_port(args[0]);
    if (port->closed) {
        snprintf(vm->error, sizeof(vm->error), "port-position: port is closed");
        return CH_UNDEFINED;
    }
    if (!port_can_position(port)) {
        snprintf(vm->error, sizeof(vm->error), "port-position: port does not support positioning");
        return CH_UNDEFINED;
    }
    if (port->kind == CH_PORT_FILE && port->file) {
        long pos = ftell(port->file);
        if (pos < 0) {
            snprintf(vm->error, sizeof(vm->error), "port-position: port does not support positioning");
            return CH_UNDEFINED;
        }
        return ch_make_fixnum(pos);
    }
    return ch_make_fixnum((int64_t)port->pos);
}

static ChValue prim_set_port_position_bang(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_port(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "set-port-position!: expected port");
        return CH_UNDEFINED;
    }
    if (!ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "set-port-position!: expected exact integer");
        return CH_UNDEFINED;
    }
    int64_t pos = ch_to_fixnum(args[1]);
    if (pos < 0) {
        snprintf(vm->error, sizeof(vm->error), "set-port-position!: expected non-negative integer");
        return CH_UNDEFINED;
    }
    ChPort *port = ch_as_port(args[0]);
    if (port->closed) {
        snprintf(vm->error, sizeof(vm->error), "set-port-position!: port is closed");
        return CH_UNDEFINED;
    }
    if (port->output && port->kind == CH_PORT_FILE && port->file) {
        fflush(port->file);
    }
    if (port->kind == CH_PORT_FILE && port->file) {
        if (fseek(port->file, pos, SEEK_SET) != 0) {
            snprintf(vm->error, sizeof(vm->error),
                     "set-port-position!: invalid position or port does not support positioning");
            return CH_UNDEFINED;
        }
        return CH_VOID;
    }
    if (port->kind == CH_PORT_STRING_IN || port->kind == CH_PORT_STRING_OUT ||
        port->kind == CH_PORT_BYTEVECTOR) {
        if ((size_t)pos > port->len) {
            snprintf(vm->error, sizeof(vm->error), "set-port-position!: index out of range");
            return CH_UNDEFINED;
        }
        port->pos = (size_t)pos;
        return CH_VOID;
    }
    snprintf(vm->error, sizeof(vm->error),
             "set-port-position!: invalid position or port does not support positioning");
    return CH_UNDEFINED;
}

static ChValue prim_port_has_port_position_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_port(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "port-has-port-position?: expected port");
        return CH_UNDEFINED;
    }
    return port_can_position(ch_as_port(args[0])) ? CH_TRUE : CH_FALSE;
}

void ch_register_port_io_primitives(ChVM *vm) {
    define_prim(vm, "read-char", prim_read_char, -1, 0);
    define_prim(vm, "peek-char", prim_peek_char, -1, 0);
    define_prim(vm, "char-ready?", prim_char_ready_p, -1, 0);
    define_prim(vm, "write-char", prim_write_char, -1, 1);
    define_prim(vm, "write-string", prim_write_string, -1, 1);
    define_prim(vm, "read-line", prim_read_line, -1, 0);
    define_prim(vm, "read-string", prim_read_string, -1, 1);
    define_prim(vm, "read-u8", prim_read_u8, -1, 0);
    define_prim(vm, "peek-u8", prim_peek_u8, -1, 0);
    define_prim(vm, "write-u8", prim_write_u8, -1, 1);
    define_prim(vm, "u8-ready?", prim_u8_ready_p, -1, 0);
    define_prim(vm, "read-bytevector", prim_read_bytevector, -1, 1);
    define_prim(vm, "read-bytevector!", prim_read_bytevector_bang, -1, 1);
    define_prim(vm, "write-bytevector", prim_write_bytevector, -1, 1);
    define_prim(vm, "read", prim_read, -1, 0);

    /* Replace core display/write/newline with port-aware versions. */
    define_prim(vm, "display", prim_display, -1, 1);
    define_prim(vm, "write", prim_write, -1, 1);
    define_prim(vm, "write-shared", prim_write_shared, -1, 1);
    define_prim(vm, "write-simple", prim_write_simple, -1, 1);
    define_prim(vm, "newline", prim_newline, -1, 0);
    define_prim(vm, "port-position", prim_port_position, 1, 1);
    define_prim(vm, "set-port-position!", prim_set_port_position_bang, 2, 2);
    define_prim(vm, "port-has-port-position?", prim_port_has_port_position_p, 1, 1);
    define_prim(vm, "port-has-set-port-position!?", prim_port_has_port_position_p, 1, 1);
}
