#include "chaaya/disasm.h"

#include "chaaya/opcode.h"
#include "chaaya/printer.h"
#include "chaaya/vm.h"

#include <stdio.h>

static const char *opcode_name(ChOpCode op) {
    switch (op) {
    case CH_OP_LOAD_CONST:
        return "load-const";
    case CH_OP_LOAD_NIL:
        return "load-nil";
    case CH_OP_LOAD_TRUE:
        return "load-true";
    case CH_OP_LOAD_FALSE:
        return "load-false";
    case CH_OP_LOAD_VOID:
        return "load-void";
    case CH_OP_MOVE:
        return "move";
    case CH_OP_GET_GLOBAL:
        return "get-global";
    case CH_OP_SET_GLOBAL:
        return "set-global";
    case CH_OP_DEFINE_GLOBAL:
        return "define-global";
    case CH_OP_GET_UPVALUE:
        return "get-upvalue";
    case CH_OP_SET_UPVALUE:
        return "set-upvalue";
    case CH_OP_CALL:
        return "call";
    case CH_OP_TAIL_CALL:
        return "tail-call";
    case CH_OP_RETURN:
        return "return";
    case CH_OP_JUMP:
        return "jump";
    case CH_OP_JUMP_FALSE:
        return "jump-false";
    case CH_OP_JUMP_TRUE:
        return "jump-true";
    case CH_OP_CLOSURE:
        return "closure";
    case CH_OP_CONS:
        return "cons";
    case CH_OP_HALT:
        return "halt";
    default:
        return "?";
    }
}

static uint8_t read_u8(const uint8_t *code, size_t *ip) {
    return code[(*ip)++];
}

static uint16_t read_u16(const uint8_t *code, size_t *ip) {
    uint16_t v = (uint16_t)code[(*ip)++];
    v |= (uint16_t)code[(*ip)++] << 8;
    return v;
}

static int16_t read_i16(const uint8_t *code, size_t *ip) {
    return (int16_t)read_u16(code, ip);
}

void ch_disassemble_function(FILE *out, ChFunction *fn) {
    if (!fn) {
        fputs("; (null function)\n", out);
        return;
    }
    fprintf(out, "; arity=%u regs=%u upvalues=%u%s\n", fn->arity, fn->num_regs, fn->num_upvalues,
            fn->variadic ? " variadic" : "");
    size_t ip = 0;
    while (ip < fn->code_len) {
        size_t start = ip;
        ChOpCode op = (ChOpCode)read_u8(fn->code, &ip);
        fprintf(out, "%4zu  %s", start, opcode_name(op));
        switch (op) {
        case CH_OP_LOAD_CONST: {
            uint8_t dst = read_u8(fn->code, &ip);
            uint16_t idx = read_u16(fn->code, &ip);
            fprintf(out, " r%u const[%u] ", dst, idx);
            if (idx < fn->const_count) {
                ch_print_value(out, fn->constants[idx], false);
            }
            break;
        }
        case CH_OP_LOAD_NIL:
        case CH_OP_LOAD_TRUE:
        case CH_OP_LOAD_FALSE:
        case CH_OP_LOAD_VOID:
        case CH_OP_RETURN:
            fprintf(out, " r%u", read_u8(fn->code, &ip));
            break;
        case CH_OP_MOVE: {
            uint8_t dst = read_u8(fn->code, &ip);
            uint8_t src = read_u8(fn->code, &ip);
            fprintf(out, " r%u r%u", dst, src);
            break;
        }
        case CH_OP_GET_GLOBAL:
        case CH_OP_DEFINE_GLOBAL: {
            uint8_t dst = read_u8(fn->code, &ip);
            uint16_t idx = read_u16(fn->code, &ip);
            fprintf(out, " r%u global[%u]", dst, idx);
            break;
        }
        case CH_OP_SET_GLOBAL: {
            uint16_t idx = read_u16(fn->code, &ip);
            uint8_t src = read_u8(fn->code, &ip);
            fprintf(out, " global[%u] r%u", idx, src);
            break;
        }
        case CH_OP_GET_UPVALUE:
        case CH_OP_SET_UPVALUE: {
            uint8_t a = read_u8(fn->code, &ip);
            uint8_t b = read_u8(fn->code, &ip);
            if (op == CH_OP_GET_UPVALUE) {
                fprintf(out, " r%u uv[%u]", a, b);
            } else {
                fprintf(out, " uv[%u] r%u", a, b);
            }
            break;
        }
        case CH_OP_CALL:
        case CH_OP_TAIL_CALL: {
            uint8_t base = read_u8(fn->code, &ip);
            uint8_t nargs = read_u8(fn->code, &ip);
            fprintf(out, " base=%u nargs=%u", base, nargs);
            break;
        }
        case CH_OP_JUMP:
            fprintf(out, " %+d", (int)read_i16(fn->code, &ip));
            break;
        case CH_OP_JUMP_FALSE:
        case CH_OP_JUMP_TRUE: {
            uint8_t test = read_u8(fn->code, &ip);
            int16_t off = read_i16(fn->code, &ip);
            fprintf(out, " r%u %+d", test, (int)off);
            break;
        }
        case CH_OP_CLOSURE: {
            uint8_t dst = read_u8(fn->code, &ip);
            uint16_t idx = read_u16(fn->code, &ip);
            fprintf(out, " r%u fn[%u]", dst, idx);
            break;
        }
        case CH_OP_CONS: {
            uint8_t dst = read_u8(fn->code, &ip);
            uint8_t car = read_u8(fn->code, &ip);
            uint8_t cdr = read_u8(fn->code, &ip);
            fprintf(out, " r%u r%u r%u", dst, car, cdr);
            break;
        }
        case CH_OP_HALT:
            break;
        default:
            fprintf(out, " (unknown operands, stopping)");
            ip = fn->code_len;
            break;
        }
        fputc('\n', out);
    }
}
