#include "test_helpers.h"

#include "chaaya/expander.h"
#include "chaaya/ir.h"
#include "chaaya/reader.h"

#include <stdio.h>
#include <string.h>

static int build_ir(ChVM *vm, const char *src, ChCompiler *compiler, ChValue *expr_root,
                    ChValue *expanded_root, ChIrNode **out_ir) {
    *expr_root = CH_NIL;
    *expanded_root = CH_NIL;
    *out_ir = NULL;
    ch_gc_push(&vm->gc, expr_root);
    ch_gc_push(&vm->gc, expanded_root);

    ChReader reader;
    ch_reader_init(&reader, &vm->gc, src, strlen(src));
    if (ch_read_datum(&reader, expr_root) != CH_READ_OK) {
        fprintf(stderr, "read failed: %s\n  in: %s\n", ch_reader_error(&reader), src);
        ch_gc_pop_n(&vm->gc, 2);
        return 0;
    }

    char err[256];
    if (ch_expand_toplevel(vm, *expr_root, expanded_root, err, sizeof(err)) != CH_EXPAND_OK) {
        fprintf(stderr, "expand failed: %s\n  in: %s\n", err, src);
        ch_gc_pop_n(&vm->gc, 2);
        return 0;
    }

    ch_compiler_init(compiler, vm);
    if (ch_ir_lower(compiler, *expanded_root, out_ir) != CH_COMPILE_OK) {
        fprintf(stderr, "ir lower failed: %s\n  in: %s\n", ch_compiler_error(compiler), src);
        ch_gc_pop_n(&vm->gc, 2);
        return 0;
    }
    return 1;
}

static void teardown_ir(ChVM *vm, ChIrNode *ir) {
    ch_ir_free(ir);
    ch_gc_pop_n(&vm->gc, 2);
}

static int test_tail_analysis(ChVM *vm) {
    ChCompiler compiler;
    ChValue expr = CH_NIL;
    ChValue expanded = CH_NIL;
    ChIrNode *ir = NULL;
    if (!build_ir(vm, "(begin (display 1) (display 2))", &compiler, &expr, &expanded, &ir)) {
        return 0;
    }

    ch_ir_analyze(ir);
    if (ir->kind != CH_IR_SEQ || ir->as.seq.count != 2) {
        fprintf(stderr, "tail analysis: expected sequence of two forms\n");
        teardown_ir(vm, ir);
        return 0;
    }
    if (ir->as.seq.items[0]->tail_position) {
        fprintf(stderr, "tail analysis: first form should not be tail-position\n");
        teardown_ir(vm, ir);
        return 0;
    }
    if (!ir->as.seq.items[1]->tail_position) {
        fprintf(stderr, "tail analysis: last form should be tail-position\n");
        teardown_ir(vm, ir);
        return 0;
    }

    teardown_ir(vm, ir);
    return 1;
}

static int test_const_fold(ChVM *vm) {
    ChCompiler compiler;
    ChValue expr = CH_NIL;
    ChValue expanded = CH_NIL;
    ChIrNode *ir = NULL;
    if (!build_ir(vm, "(+ 1 2)", &compiler, &expr, &expanded, &ir)) {
        return 0;
    }

    ch_ir_analyze(ir);
    if (ch_ir_optimize(&compiler, &ir) != CH_COMPILE_OK) {
        fprintf(stderr, "optimize failed: %s\n", ch_compiler_error(&compiler));
        teardown_ir(vm, ir);
        return 0;
    }
    if (ir->kind != CH_IR_LITERAL || !ch_is_fixnum(ir->as.literal) || ch_to_fixnum(ir->as.literal) != 3) {
        fprintf(stderr, "const fold: expected literal fixnum 3\n");
        teardown_ir(vm, ir);
        return 0;
    }

    teardown_ir(vm, ir);
    return 1;
}

static int test_dead_branch(ChVM *vm) {
    ChCompiler compiler;
    ChValue expr = CH_NIL;
    ChValue expanded = CH_NIL;
    ChIrNode *ir = NULL;
    if (!build_ir(vm, "(if #f 11 22)", &compiler, &expr, &expanded, &ir)) {
        return 0;
    }

    ch_ir_analyze(ir);
    if (ch_ir_optimize(&compiler, &ir) != CH_COMPILE_OK) {
        fprintf(stderr, "optimize failed: %s\n", ch_compiler_error(&compiler));
        teardown_ir(vm, ir);
        return 0;
    }
    if (ir->kind != CH_IR_LITERAL || !ch_is_fixnum(ir->as.literal) ||
        ch_to_fixnum(ir->as.literal) != 22) {
        fprintf(stderr, "dead branch: expected literal 22\n");
        teardown_ir(vm, ir);
        return 0;
    }

    teardown_ir(vm, ir);
    return 1;
}

static int test_boolean_simplify(ChVM *vm) {
    ChCompiler compiler;
    ChValue expr = CH_NIL;
    ChValue expanded = CH_NIL;
    ChIrNode *ir = NULL;
    if (!build_ir(vm, "(if (not x) 10 20)", &compiler, &expr, &expanded, &ir)) {
        return 0;
    }

    ch_ir_analyze(ir);
    if (ch_ir_optimize(&compiler, &ir) != CH_COMPILE_OK) {
        fprintf(stderr, "optimize failed: %s\n", ch_compiler_error(&compiler));
        teardown_ir(vm, ir);
        return 0;
    }
    if (ir->kind != CH_IR_IF || ir->as.if_expr.test->kind != CH_IR_VAR) {
        fprintf(stderr, "boolean simplify: expected simplified if with var test\n");
        teardown_ir(vm, ir);
        return 0;
    }
    if (strcmp(ch_symbol_basename(ir->as.if_expr.test->as.var), "x") != 0) {
        fprintf(stderr, "boolean simplify: expected test variable x\n");
        teardown_ir(vm, ir);
        return 0;
    }
    if (ir->as.if_expr.consequent->kind != CH_IR_LITERAL || !ch_is_fixnum(ir->as.if_expr.consequent->as.literal) ||
        ch_to_fixnum(ir->as.if_expr.consequent->as.literal) != 20) {
        fprintf(stderr, "boolean simplify: expected swapped consequent 20\n");
        teardown_ir(vm, ir);
        return 0;
    }
    if (!ir->as.if_expr.has_alternate || ir->as.if_expr.alternate->kind != CH_IR_LITERAL ||
        !ch_is_fixnum(ir->as.if_expr.alternate->as.literal) ||
        ch_to_fixnum(ir->as.if_expr.alternate->as.literal) != 10) {
        fprintf(stderr, "boolean simplify: expected swapped alternate 10\n");
        teardown_ir(vm, ir);
        return 0;
    }

    teardown_ir(vm, ir);
    return 1;
}

static int test_llvm_emittable(ChVM *vm) {
    ChCompiler compiler;
    ChValue expr = CH_NIL;
    ChValue expanded = CH_NIL;
    ChIrNode *ir = NULL;
    if (!build_ir(vm, "(+ 1 2)", &compiler, &expr, &expanded, &ir)) {
        return 0;
    }
    ch_ir_analyze(ir);
    if (ch_ir_optimize(&compiler, &ir) != CH_COMPILE_OK) {
        fprintf(stderr, "llvm emittable: optimize failed\n");
        teardown_ir(vm, ir);
        return 0;
    }
    if (!ch_ir_llvm_emittable(ir)) {
        fprintf(stderr, "llvm emittable: (+ 1 2) should be emittable\n");
        teardown_ir(vm, ir);
        return 0;
    }
    teardown_ir(vm, ir);

    if (!build_ir(vm, "(lambda (x) x)", &compiler, &expr, &expanded, &ir)) {
        return 0;
    }
    if (ch_ir_llvm_emittable(ir) || !ch_ir_llvm_needs_fallback(ir)) {
        fprintf(stderr, "llvm emittable: lambda should require fallback\n");
        teardown_ir(vm, ir);
        return 0;
    }
    teardown_ir(vm, ir);
    return 1;
}

int main(void) {
    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);

    CH_CHECK(test_tail_analysis(&vm));
    CH_CHECK(test_const_fold(&vm));
    CH_CHECK(test_dead_branch(&vm));
    CH_CHECK(test_boolean_simplify(&vm));
    CH_CHECK(test_llvm_emittable(&vm));

    /* IR pipeline stays semantically transparent at runtime. */
    CH_CHECK(ch_test_expect_fixnum(&vm, "(+ 40 2)", 42));
    CH_CHECK(ch_test_expect_equal(
        &vm, "(begin (define (+ a b) 'user-plus) (+ 1 2))", "user-plus"));

    ch_vm_deinit(&vm);
    printf("ok\n");
    return 0;
}
