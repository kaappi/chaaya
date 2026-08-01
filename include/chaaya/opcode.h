#ifndef CHAAYA_OPCODE_H
#define CHAAYA_OPCODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ChOpCode {
    CH_OP_LOAD_CONST = 0, /* dst:u8, idx:u16 */
    CH_OP_LOAD_NIL,       /* dst:u8 */
    CH_OP_LOAD_TRUE,      /* dst:u8 */
    CH_OP_LOAD_FALSE,     /* dst:u8 */
    CH_OP_LOAD_VOID,      /* dst:u8 */
    CH_OP_MOVE,           /* dst:u8, src:u8 */
    CH_OP_GET_GLOBAL,     /* dst:u8, sym_idx:u16 */
    CH_OP_SET_GLOBAL,     /* sym_idx:u16, src:u8 */
    CH_OP_DEFINE_GLOBAL,  /* sym_idx:u16, src:u8 */
    CH_OP_GET_UPVALUE,    /* dst:u8, idx:u8 */
    CH_OP_SET_UPVALUE,    /* idx:u8, src:u8 */
    CH_OP_CALL,           /* base:u8, nargs:u8 */
    CH_OP_TAIL_CALL,      /* base:u8, nargs:u8 */
    CH_OP_RETURN,         /* src:u8 */
    CH_OP_JUMP,           /* offset:i16 */
    CH_OP_JUMP_FALSE,     /* test:u8, offset:i16 */
    CH_OP_JUMP_TRUE,      /* test:u8, offset:i16 */
    CH_OP_CLOSURE,        /* dst:u8, idx:u16 */
    CH_OP_CONS,           /* dst:u8, car:u8, cdr:u8 */
    CH_OP_HALT,           /* (none) */
} ChOpCode;

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_OPCODE_H */
