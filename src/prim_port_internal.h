#ifndef CHAAYA_PRIM_PORT_INTERNAL_H
#define CHAAYA_PRIM_PORT_INTERNAL_H

#include "chaaya/prim.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_PORT_INPUT_CHUNK 4096

void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity);
ChValue get_global(ChVM *vm, const char *name);

FILE *port_file(ChPort *p);
void compact_port_input_buffer(ChPort *p);
int ensure_port_capacity(ChPort *p, size_t extra);

/* Returns 0 on success, -1 on error, -2 if a fiber parked waiting for writable. */
int port_write_bytes_vm(ChVM *vm, ChPort *p, const char *data, size_t len, int may_park);
int port_write_bytes(ChPort *p, const char *data, size_t len);

/* Returns bytes appended (>0), 0 at EOF, -1 on error, -2 if fiber parked on fd. */
int append_file_input_bytes(ChVM *vm, ChPort *p, int may_park);
int ensure_port_input_byte(ChVM *vm, ChPort *p, int may_park);

int parse_nonnegative_fixnum(ChVM *vm, ChValue v, size_t *out, const char *who);
int parse_u8(ChVM *vm, ChValue v, uint8_t *out, const char *who);

ChPort *require_output_port(ChVM *vm, ChValue *args, int nargs, int idx);
ChPort *require_input_port(ChVM *vm, ChValue *args, int nargs, int idx);

void close_port_impl(ChVM *vm, ChPort *p);

ChValue raise_typed_error(ChVM *vm, const char *message, int error_type, ChValue irritant);
ChValue raise_file_error(ChVM *vm, const char *message, ChValue irritant);
ChValue raise_read_error(ChVM *vm, const char *message, ChValue irritant);

/* --- registered from prim_port_io.c --- */
void ch_register_port_io_primitives(ChVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_PRIM_PORT_INTERNAL_H */
