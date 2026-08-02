#ifndef CHAAYA_IR_H
#define CHAAYA_IR_H

#include "chaaya/compiler.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ChIrKind {
    CH_IR_VOID = 0,
    CH_IR_LITERAL,
    CH_IR_QUOTE,
    CH_IR_VAR,
    CH_IR_IF,
    CH_IR_LAMBDA,
    CH_IR_SEQ,
    CH_IR_CALL,
    CH_IR_SET,
    CH_IR_DEFINE,
    CH_IR_DEFINE_SYNTAX,
    CH_IR_AND,
    CH_IR_OR,
    CH_IR_PRIM_CALL,
    CH_IR_RAW,
} ChIrKind;

typedef enum ChIrPrim {
    CH_IR_PRIM_ADD = 0,
    CH_IR_PRIM_SUB,
    CH_IR_PRIM_MUL,
    CH_IR_PRIM_LT,
    CH_IR_PRIM_GT,
    CH_IR_PRIM_NUM_EQ,
    CH_IR_PRIM_LE,
    CH_IR_PRIM_GE,
    CH_IR_PRIM_NOT,
    CH_IR_PRIM_COUNT
} ChIrPrim;

typedef struct ChIrNode ChIrNode;

typedef struct ChIrNodeArray {
    ChIrNode **items;
    size_t count;
} ChIrNodeArray;

typedef struct ChIrIfExpr {
    ChIrNode *test;
    ChIrNode *consequent;
    ChIrNode *alternate;
    bool has_alternate;
} ChIrIfExpr;

typedef struct ChIrLambdaExpr {
    ChValue params;
    ChIrNode **body;
    size_t body_count;
} ChIrLambdaExpr;

typedef struct ChIrCallExpr {
    ChIrNode *callee;
    ChIrNode **args;
    size_t arg_count;
} ChIrCallExpr;

typedef struct ChIrSetExpr {
    ChSymbol *name;
    ChIrNode *value;
} ChIrSetExpr;

typedef struct ChIrDefineExpr {
    ChValue target;
    ChIrNode *value;
} ChIrDefineExpr;

typedef struct ChIrPrimCallExpr {
    ChIrPrim prim;
    ChSymbol *symbol;
    ChIrNode **args;
    size_t arg_count;
} ChIrPrimCallExpr;

typedef struct ChIrRawExpr {
    ChValue expr;
} ChIrRawExpr;

struct ChIrNode {
    ChIrKind kind;
    bool tail_position;
    bool is_constant;
    ChValue constant_value;
    union {
        ChValue literal;
        ChValue quoted;
        ChSymbol *var;
        ChIrIfExpr if_expr;
        ChIrLambdaExpr lambda;
        ChIrNodeArray seq;
        ChIrCallExpr call;
        ChIrSetExpr set_expr;
        ChIrDefineExpr define_expr;
        ChIrNodeArray and_expr;
        ChIrNodeArray or_expr;
        ChIrPrimCallExpr prim_call;
        ChIrRawExpr raw;
    } as;
};

typedef ChCompileStatus (*ChIrLegacyEmitFn)(void *ctx, ChValue expr, uint8_t dst, bool tail);

ChIrNode *ch_ir_new_node(ChIrKind kind);
void ch_ir_free(ChIrNode *node);

ChCompileStatus ch_ir_lower(ChCompiler *c, ChValue expr, ChIrNode **out_root);
void ch_ir_analyze(ChIrNode *root);
ChCompileStatus ch_ir_optimize(ChCompiler *c, ChIrNode **root_slot);

ChCompileStatus ch_ir_emit(ChCompiler *c, ChIrNode *root, ChIrLegacyEmitFn emit_fn, void *emit_ctx,
                           uint8_t dst, bool tail);

/* Pretty-print IR tree (used by `chaaya ir`). */
void ch_ir_print(FILE *out, const ChIrNode *node, int indent);

/* True when the node (and all children) can be lowered to native LLVM IR
 * without eval-fallback. CH_IR_RAW and unsupported forms return false. */
bool ch_ir_llvm_emittable(const ChIrNode *node);

/* True when any descendant requires eval-fallback (whole-scope abandon). */
bool ch_ir_llvm_needs_fallback(const ChIrNode *node);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_IR_H */
