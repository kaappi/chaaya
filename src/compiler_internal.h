#ifndef CHAAYA_COMPILER_INTERNAL_H
#define CHAAYA_COMPILER_INTERNAL_H

#include "chaaya/compiler.h"

#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/ir.h"
#include "chaaya/library.h"
#include "chaaya/opcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_MAX_LOCALS 256
#define CH_MAX_UPVALUES 64
#define CH_MAX_CONSTS 512
#define CH_CODE_INIT 256
#define CH_MAX_DERIVED_BINDINGS 64
#define CH_MAX_GUARD_CLAUSES 128

typedef struct ChLocal {
    ChSymbol *name;
    uint8_t depth;
    bool is_captured;
    uint32_t binding_id;
} ChLocal;

typedef struct ChCompUpvalue {
    uint8_t index;
    bool is_local;
} ChCompUpvalue;

typedef struct ChBodyMacroSave {
    ChSymbol *name;
    ChValue old_transformer;
    int depth;
} ChBodyMacroSave;

typedef struct ChFuncCompiler {
    struct ChFuncCompiler *enclosing;
    ChFunction *fn;
    ChValue fn_root;
    size_t compiling_fn_slot;
    ChLocal locals[CH_MAX_LOCALS];
    int local_count;
    int scope_depth;
    ChCompUpvalue upvalues[CH_MAX_UPVALUES];
    int upvalue_count;
    /* Scoped define-syntax bindings to restore when leaving a body (#651). */
    ChBodyMacroSave body_macros[CH_MAX_DERIVED_BINDINGS];
    int n_body_macros;
    uint8_t next_reg;
    uint8_t max_regs;
    /* growable code buffer before freeze into fn */
    uint8_t *code;
    size_t code_len;
    size_t code_cap;
    ChValue constants[CH_MAX_CONSTS];
    size_t const_count;
    size_t const_roots; /* GC roots pushed for constants[] slots */
    bool is_toplevel;
} ChFuncCompiler;

/* --- infrastructure (compiler.c) --- */
ChCompileStatus fail(ChCompiler *c, const char *msg);
void fc_init(ChCompiler *c, ChFuncCompiler *fc, ChFuncCompiler *enclosing, ChFunction *fn,
             bool toplevel);
void pop_const_roots(ChCompiler *c, ChFuncCompiler *fc);
void pop_root_at(ChGC *gc, size_t base);
void fc_end_compile(ChCompiler *c, ChFuncCompiler *fc);
void fc_discard(ChCompiler *c, ChFuncCompiler *fc);

void emit_byte(ChFuncCompiler *fc, uint8_t b);
void emit_u16(ChFuncCompiler *fc, uint16_t v);
void emit_i16(ChFuncCompiler *fc, int16_t v);
size_t emit_jump(ChFuncCompiler *fc, ChOpCode op);
size_t emit_jump_test(ChFuncCompiler *fc, ChOpCode op, uint8_t test);
void patch_jump(ChFuncCompiler *fc, size_t at);

uint8_t alloc_reg(ChFuncCompiler *fc);
void reset_regs(ChFuncCompiler *fc, uint8_t saved);
int add_constant(ChCompiler *c, ChFuncCompiler *fc, ChValue v);

int resolve_local(ChFuncCompiler *fc, ChSymbol *name);
int resolve_upvalue(ChFuncCompiler *fc, ChSymbol *name);
void begin_scope(ChFuncCompiler *fc);
void end_scope(ChCompiler *c, ChFuncCompiler *fc);
int add_local(ChCompiler *c, ChFuncCompiler *fc, ChSymbol *name);
uint8_t local_reg(ChFuncCompiler *fc, int local_index);
void ensure_temps_from(ChFuncCompiler *fc);

int in_restricted_eval_env(ChCompiler *c);
int eval_env_immutable(ChCompiler *c);

size_t list_length(ChValue v);
bool is_symbol_named(ChValue v, const char *name);

ChCompileStatus compile_begin(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                              bool tail);
ChCompileStatus finish_function(ChCompiler *c, ChFuncCompiler *fc);
ChCompileStatus compile_expr(ChCompiler *c, ChFuncCompiler *fc, ChValue expr, uint8_t dst,
                             bool tail);

/* --- binding forms (compiler_bind.c) --- */
ChCompileStatus compile_set(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst);
ChCompileStatus compile_define(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst);
ChCompileStatus compile_lambda(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst);
ChCompileStatus compile_case_lambda(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                    bool tail);
ChCompileStatus compile_delay(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                              bool tail);
ChCompileStatus compile_delay_force(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                    bool tail);
ChCompileStatus compile_define_values(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst);
ChCompileStatus compile_let_values(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                   bool tail);
ChCompileStatus compile_let_star_values(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                        bool tail);
ChCompileStatus compile_let(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst, bool tail);
ChCompileStatus compile_let_star(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                 bool tail);
ChCompileStatus compile_letrec(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                               bool tail);

/* --- control / syntax forms (compiler_control.c) --- */
ChCompileStatus compile_cond_expand(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                    bool tail);
ChCompileStatus compile_define_property(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst);
ChCompileStatus compile_define_syntax(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst);
ChCompileStatus compile_let_syntax(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                   bool tail, int letrec);
ChCompileStatus compile_and(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst, bool tail);
ChCompileStatus compile_or(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst, bool tail);
ChCompileStatus compile_cond(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst, bool tail);
ChCompileStatus compile_case(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst, bool tail);
ChCompileStatus compile_when_unless(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                    bool tail, int is_when);
ChCompileStatus compile_do(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst, bool tail);
ChCompileStatus compile_guard(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                              bool tail);
ChCompileStatus compile_parameterize(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                     bool tail);
ChCompileStatus compile_quasiquote_real(ChCompiler *c, ChFuncCompiler *fc, ChValue args, uint8_t dst,
                                        bool tail);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_COMPILER_INTERNAL_H */
