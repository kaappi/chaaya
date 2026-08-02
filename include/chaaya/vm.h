#ifndef CHAAYA_VM_H
#define CHAAYA_VM_H

#include "chaaya/gc.h"
#include "chaaya/value.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_VM_MAX_FRAMES 256
#define CH_VM_MAX_REGS 4096
#define CH_VM_MAX_GLOBALS 4096
#define CH_VM_MAX_LIB_PATHS 32
#define CH_VM_MAX_SCRIPT_ARGS 64
#define CH_VM_MAX_WINDS 64
#define CH_VM_MAX_HANDLERS 64
#define CH_VM_MAX_PARAMETER_BINDINGS 256
#define CH_VM_MAX_MACROS 256
#define CH_VM_MAX_SYNTAX_PROPS 128
#define CH_VM_MAX_BREAKPOINTS 32

struct ChLibEnv;
struct ChLibraryRegistry;
struct ChFiberRuntime;

typedef enum ChVMStatus {
    CH_VM_OK = 0,
    CH_VM_RUNTIME_ERROR,
    CH_VM_STACK_OVERFLOW,
    CH_VM_CONTINUATION_INVOKED,
} ChVMStatus;

typedef struct ChCallFrame {
    ChClosure *closure;
    uint8_t *ip;
    size_t reg_base; /* index into vm->regs */
    uint8_t num_regs;
} ChCallFrame;

typedef struct ChGlobal {
    ChSymbol *name;
    ChValue value;
    bool defined;
} ChGlobal;

typedef struct ChMacroEntry {
    ChSymbol *name;
    ChValue transformer; /* CH_TAG_TRANSFORMER — rooted for GC */
    struct ChLibEnv *home_env; /* library env for template free-identifier resolution */
} ChMacroEntry;

typedef struct ChSyntaxProp {
    char *key; /* owned composite "<id>\x1f<key>" */
    ChValue value;
} ChSyntaxProp;

typedef struct ChVM {
    ChGC gc;
    ChValue regs[CH_VM_MAX_REGS];
    size_t reg_top;
    ChCallFrame frames[CH_VM_MAX_FRAMES];
    size_t frame_count;
    ChGlobal globals[CH_VM_MAX_GLOBALS];
    size_t global_count;
    ChUpvalue *open_upvalues;
    ChValue result;
    char error[256];

    /* dynamic-wind + exception handlers */
    ChWindRecord wind_stack[CH_VM_MAX_WINDS];
    size_t wind_count;
    ChExceptionHandler handler_stack[CH_VM_MAX_HANDLERS];
    size_t handler_count;
    ChParameterBinding parameter_stack[CH_VM_MAX_PARAMETER_BINDINGS];
    size_t parameter_count;

    /* compile-time macros (define-syntax) */
    ChMacroEntry macros[CH_VM_MAX_MACROS];
    size_t macro_count;
    uint32_t hyg_counter;

    /* SRFI 213 define-property table (macro-expansion time) */
    ChSyntaxProp syntax_props[CH_VM_MAX_SYNTAX_PROPS];
    size_t syntax_prop_count;

    /* set by call_value before invoking a native; used by call/cc */
    size_t native_result_slot;
    bool continuation_invoked; /* native saw a continuation restore */

    /* R7RS libraries */
    struct ChLibraryRegistry *libraries;
    struct ChLibEnv *active_lib_env; /* non-NULL while compiling/running library body */
    struct ChEnvironment *active_eval_env; /* non-NULL during eval in a custom environment */
    char *loading_libs[32];
    size_t loading_lib_count;
    char *current_lib_dir; /* owned; directory of .sld being loaded */

    /* CLI / script context (owned strings are not strdup'd — point at argv) */
    const char *lib_paths[CH_VM_MAX_LIB_PATHS];
    size_t lib_path_count;
    const char *script_path;
    const char *script_args[CH_VM_MAX_SCRIPT_ARGS];
    size_t script_arg_count;

    /* Phase 10: cooperative fibers + timer reactor. */
    struct ChFiberRuntime *fiber_runtime;

    /* SRFI 27 default random source (owned heap object). */
    ChValue default_random_source;

    /* REPL debugger (Phase 8 MVP). */
    bool debug_mode;
    bool step_trace;
    char breakpoints[CH_VM_MAX_BREAKPOINTS][64];
    size_t breakpoint_count;
} ChVM;

void ch_vm_init(ChVM *vm);
void ch_vm_deinit(ChVM *vm);

int ch_vm_intern_global(ChVM *vm, ChSymbol *sym);
void ch_vm_define_global(ChVM *vm, int idx, ChValue v);

void ch_vm_register_primitives(ChVM *vm);

/* Push live VM slots onto the GC root stack; returns count for ch_gc_pop_n. */
size_t ch_vm_push_gc_roots(ChVM *vm);

/* Mark live VM slots during collection without using the root stack. */
void ch_vm_mark_gc_roots(ChVM *vm);

ChVMStatus ch_vm_call_closure(ChVM *vm, ChValue closure, ChValue *args, int nargs, ChValue *out);
ChVMStatus ch_vm_eval_function(ChVM *vm, ChFunction *fn, ChValue *out);

/* Call a procedure without clearing the existing stack (for dynamic-wind etc.). */
ChVMStatus ch_vm_apply(ChVM *vm, ChValue proc, ChValue *args, int nargs, ChValue *out);

/* Parameter object dynamic binding helpers. */
ChValue ch_vm_parameter_ref(ChVM *vm, ChValue parameter);
int ch_vm_parameter_set(ChVM *vm, ChValue parameter, ChValue value);
int ch_vm_parameter_push(ChVM *vm, ChValue parameter, ChValue value);
int ch_vm_parameter_pop(ChVM *vm, ChValue parameter);

/* Raise a Scheme condition object through the handler stack. */
ChValue ch_vm_raise(ChVM *vm, ChValue obj, int continuable);
void ch_vm_reclaim_regs(ChVM *vm);

ChValue ch_vm_capture_continuation(ChVM *vm, size_t result_slot);
ChVMStatus ch_vm_invoke_continuation(ChVM *vm, ChContinuation *cont, ChValue value);
ChVMStatus ch_vm_wind_transition(ChVM *vm, const ChWindRecord *target, size_t target_count);

const char *ch_vm_error(const ChVM *vm);

/* SRFI 213 syntax properties keyed by identifier and property name. */
int ch_vm_syntax_property_set(ChVM *vm, const char *id, const char *key, ChValue val);
ChValue ch_vm_syntax_property_get(ChVM *vm, const char *id, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_VM_H */
