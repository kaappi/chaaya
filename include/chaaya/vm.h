#ifndef CHAAYA_VM_H
#define CHAAYA_VM_H

#include "chaaya/diagnostics.h"
#include "chaaya/gc.h"
#include "chaaya/value.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard caps match Kaappi's growable stacks (~32768 frames). Heap-allocated in
 * ch_vm_init so deep Scheme recursion (e.g. map callbacks) does not blow the
 * C stack or a small fixed ChVM on the process stack. */
#define CH_VM_MAX_FRAMES 32768
#define CH_VM_MAX_REGS 262144
#define CH_VM_MAX_GLOBALS 4096
#define CH_VM_MAX_LIB_PATHS 32
#define CH_VM_MAX_SCRIPT_ARGS 64
#define CH_VM_MAX_WINDS 64
#define CH_VM_MAX_HANDLERS 64
#define CH_VM_MAX_PARAMETER_BINDINGS 256
#define CH_VM_MAX_MACROS 256
#define CH_VM_MAX_SYNTAX_PROPS 128
#define CH_VM_MAX_BREAKPOINTS 32
#define CH_VM_MAX_PENDING_ARGS 64
/* Nested native→Scheme→native re-entrancy (map/for-each callbacks). Cap before
 * the C stack overflows; Debug matches Kaappi's 200, Release allows deeper. */
#if defined(NDEBUG)
#define CH_VM_MAX_NATIVE_REENTRY 3000
#else
#define CH_VM_MAX_NATIVE_REENTRY 200
#endif

struct ChLibEnv;
struct ChLibraryRegistry;
struct ChFiberRuntime;

typedef enum ChVMStatus {
    CH_VM_OK = 0,
    CH_VM_RUNTIME_ERROR,
    CH_VM_STACK_OVERFLOW,
    CH_VM_CONTINUATION_INVOKED,
    CH_VM_FIBER_PARKED,
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
    ChValue *regs; /* heap; length CH_VM_MAX_REGS */
    size_t reg_top;
    ChCallFrame *frames; /* heap; length CH_VM_MAX_FRAMES */
    size_t frame_count;
    ChGlobal globals[CH_VM_MAX_GLOBALS];
    size_t global_count;
    ChUpvalue *open_upvalues;
    ChValue result;
    char error[256];
    ChDiagCode error_code;
    int error_line;
    int error_column;

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
    /* When set, expand_form still registers define-syntax but does not
     * expand macro uses (used while expanding record-type boilerplate). */
    bool suppress_macro_expand;
    /* True only for the root form of ch_expand_toplevel — top-level
     * define-syntax registers during expand so `chaaya expand` and the
     * next top-level form see the macro; nested bodies leave registration
     * to the compiler (#651). */
    bool expanding_toplevel_form;

    /* SRFI 213 define-property table (macro-expansion time) */
    ChSyntaxProp syntax_props[CH_VM_MAX_SYNTAX_PROPS];
    size_t syntax_prop_count;

    /* set by call_value before invoking a native; used by call/cc */
    size_t native_result_slot;
    uint16_t native_reentry_depth; /* ch_vm_apply nesting from native callbacks */
    bool continuation_invoked; /* native saw a continuation restore */
    bool native_was_tail;      /* current native was entered via TAIL_CALL */
    /* Native requests a follow-up procedure call without nesting run_until
     * (proper TCO for apply / call-with-values). */
    bool has_pending_call;
    bool pending_call_tail;
    ChValue pending_proc;
    int pending_nargs;
    ChValue pending_args[CH_VM_MAX_PENDING_ARGS];

    /* R7RS libraries */
    struct ChLibraryRegistry *libraries;
    struct ChLibEnv *active_lib_env; /* non-NULL while compiling/running library body */
    struct ChEnvironment *active_eval_env; /* non-NULL during eval in a custom environment */
    size_t eval_depth; /* >0 while running eval'd code; isolates exception handlers */
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
    bool fiber_parked; /* native requested cooperative fiber park */

    /* SRFI-18: current OS-thread handle (fiber with os_state) on this VM. */
    ChValue current_thread;
    bool owns_globals; /* false for child OS-thread VMs sharing parent globals */
    struct ChVM *parent_vm; /* non-NULL for child thread VMs */

    /* SRFI 27 default random source (owned heap object). */
    ChValue default_random_source;

    /* FFI callback trampoline state (Phase F). */
    int ffi_callback_depth;
    bool ffi_callback_deferred;
    ChValue ffi_callback_deferred_value;
    ChValue ffi_callback_raise_result;

    /* REPL debugger. */
    bool debug_mode;
    bool step_trace;
    char breakpoints[CH_VM_MAX_BREAKPOINTS][64];
    size_t breakpoint_count;
    /* Optional hook: return 0 to continue, 1 to abort current eval. */
    int (*debug_break_hook)(struct ChVM *vm, const char *name);
    /* Step modes after a pause: 0=none, 1=step into next call. */
    int debug_step_mode;
    /* Frame inspection cursor for ,up/,down (0 = innermost). */
    size_t debug_inspect_frame;

    /* `chaaya test` worker: swallow (exit) so results can still be emitted. */
    bool suppress_exit;
    bool exit_requested;
    uint8_t exit_code;
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

/* Resume bytecode after a fiber snapshot restore (run down to target_frames). */
ChVMStatus ch_vm_run_fiber_resume(ChVM *vm, size_t target_frames);

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
