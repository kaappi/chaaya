#include "chaaya/repl.h"

#include "chaaya/disasm.h"
#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/library.h"
#include "chaaya/printer.h"
#include "chaaya/profile.h"
#include "chaaya/reader.h"
#include "chaaya/version.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef CHAAYA_HAS_LINENOISE
#include "linenoise.h"
#endif

#define CH_HISTORY_MAX 1000
#define CH_REPL_MAX_WATCHES 32
#define CH_REPL_MAX_WATCH_EXPR 256
#define CH_REPL_MAX_BREAK_COND 256

static ChVM *g_repl_vm = NULL; /* for completion callback */

static ChReadStatus read_repl_datum(ChVM *vm, ChReader *reader, ChValue *out);

int ch_repl_paren_depth(const char *s) {
    int depth = 0;
    int in_string = 0;
    int in_line_comment = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = 0;
            }
            continue;
        }
        if (in_string) {
            if (c == '\\' && s[i + 1]) {
                i++;
                continue;
            }
            if (c == '"') {
                in_string = 0;
            }
            continue;
        }
        if (c == ';') {
            in_line_comment = 1;
            continue;
        }
        if (c == '"') {
            in_string = 1;
            continue;
        }
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth < 0) {
                depth = 0;
            }
        }
    }
    return depth;
}

static char *trim_left(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        s++;
    }
    return s;
}

static int mkdir_p(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return mkdir(dir, 0700);
}

static int history_path(char *buf, size_t buflen) {
    char home_buf[512];
    const char *env_home = getenv("CHAAYA_HOME");
    if (env_home && env_home[0] != '\0') {
        if (snprintf(home_buf, sizeof(home_buf), "%s", env_home) >= (int)sizeof(home_buf)) {
            return -1;
        }
    } else {
        const char *h = getenv("HOME");
        if (!h || h[0] == '\0') {
            return -1;
        }
        if (snprintf(home_buf, sizeof(home_buf), "%s/.chaaya", h) >= (int)sizeof(home_buf)) {
            return -1;
        }
    }
    if (mkdir_p(home_buf) != 0) {
        return -1;
    }
    if (snprintf(buf, buflen, "%s/history", home_buf) >= (int)buflen) {
        return -1;
    }
    return 0;
}

static void print_comma_help(void) {
    puts("Commands:");
    puts("  ,help             Show this message");
    puts("  ,quit / ,exit     Exit the REPL");
    puts("");
    puts(" -- Evaluation:");
    puts("  ,time <expr>      Measure execution time");
    puts("  ,type <expr>      Show result type");
    puts("  ,expand <expr>    Show macro expansion");
    puts("  ,profile <expr>   Profile timing and calls");
    puts("  ,dis <expr>       Disassemble a procedure");
    puts("");
    puts(" -- Inspection:");
    puts("  ,describe <sym>   Show procedure arity and type");
    puts("  ,apropos <str>    Search bindings by substring");
    puts("  ,env [prefix]     List bindings (optionally filtered by prefix)");
    puts("");
    puts(" -- Debugging:");
    puts("  ,break <name> [if <expr>]  Break on call; optional condition");
    puts("  ,breakpoints      List active breakpoints");
    puts("  ,delete <name|all>  Remove a breakpoint (or all)");
    puts("  ,step <expr>      Evaluate with single-stepping");
    puts("  ,condition <id> <expr>  Set breakpoint condition");
    puts("  ,watch <expr>     Add a debugger watch expression");
    puts("  ,watch            List watch expressions");
    puts("  ,unwatch [n|expr] Remove watch by index/expression (none = all)");
    puts("  At debug>: ,continue ,next ,finish ,backtrace ,locals ,up ,down");
    puts("");
    puts(" -- System:");
    puts("  ,gc               Show GC statistics");
    puts("  ,version          Show Chaaya version");
    puts("  ,load <file>      Load and run a Scheme file");
    puts("  ,import <lib>     Import a library (e.g. ,import (srfi 1))");
    puts("");
    puts("The variable _ holds the last result.");
}

static int g_debug_continue = 0;
static int g_debug_abort = 0;
static char g_debug_watches[CH_REPL_MAX_WATCHES][CH_REPL_MAX_WATCH_EXPR];
static size_t g_debug_watch_count = 0;
static char g_break_conditions[CH_VM_MAX_BREAKPOINTS][CH_REPL_MAX_BREAK_COND];

static void debug_print_backtrace(ChVM *vm) {
    printf("; backtrace (%zu frame%s):\n", vm->frame_count, vm->frame_count == 1 ? "" : "s");
    for (size_t i = 0; i < vm->frame_count; i++) {
        size_t idx = vm->frame_count - 1 - i;
        ChCallFrame *fr = &vm->frames[idx];
        const char *name = "<thunk>";
        if (fr->closure && fr->closure->fn) {
            /* Try match against globals */
            ChValue clv = ch_make_pointer(&fr->closure->header);
            for (size_t g = 0; g < vm->global_count; g++) {
                if (vm->globals[g].defined && ch_eqv(vm->globals[g].value, clv)) {
                    name = vm->globals[g].name->name;
                    break;
                }
            }
        }
        printf(";   #%zu %s regs=%u\n", i, name, (unsigned)fr->num_regs);
    }
}

static int vm_lookup_global_value(ChVM *vm, const char *name, ChValue *out) {
    if (!vm || !name) {
        return 0;
    }
    for (size_t i = 0; i < vm->global_count; i++) {
        if (!vm->globals[i].defined) {
            continue;
        }
        if (strcmp(vm->globals[i].name->name, name) == 0) {
            if (out) {
                *out = vm->globals[i].value;
            }
            return 1;
        }
    }
    return 0;
}

static int repl_eval_expr_quiet(ChVM *vm, const char *expr, ChValue *out) {
    bool saved_debug = vm->debug_mode;
    bool saved_step_trace = vm->step_trace;
    int saved_step_mode = vm->debug_step_mode;
    int (*saved_hook)(ChVM *, const char *) = vm->debug_break_hook;

    vm->debug_mode = false;
    vm->step_trace = false;
    vm->debug_step_mode = 0;
    vm->debug_break_hook = NULL;

    int rc = ch_eval_source(vm, expr, strlen(expr), 0);

    vm->debug_break_hook = saved_hook;
    vm->debug_mode = saved_debug;
    vm->step_trace = saved_step_trace;
    vm->debug_step_mode = saved_step_mode;

    if (rc != 0) {
        return -1;
    }
    if (out) {
        if (!vm_lookup_global_value(vm, "_", out)) {
            *out = CH_VOID;
        }
    }
    return 0;
}

static int repl_strcasestr(const char *haystack, const char *needle) {
    if (!needle || !needle[0]) {
        return 1;
    }
    if (!haystack) {
        return 0;
    }
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) {
        return 0;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen) {
            unsigned char hc = (unsigned char)haystack[i + j];
            unsigned char nc = (unsigned char)needle[j];
            if (tolower(hc) != tolower(nc)) {
                break;
            }
            j++;
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

static int debug_add_watch_expr(const char *expr) {
    if (!expr || !expr[0]) {
        return -1;
    }
    for (size_t i = 0; i < g_debug_watch_count; i++) {
        if (strcmp(g_debug_watches[i], expr) == 0) {
            return 1; /* duplicate */
        }
    }
    if (g_debug_watch_count >= CH_REPL_MAX_WATCHES) {
        return -2;
    }
    if (snprintf(g_debug_watches[g_debug_watch_count], CH_REPL_MAX_WATCH_EXPR, "%s", expr) >=
        CH_REPL_MAX_WATCH_EXPR) {
        return -3;
    }
    g_debug_watch_count++;
    return 0;
}

static void debug_list_watches(void) {
    if (g_debug_watch_count == 0) {
        puts("; no watches");
        return;
    }
    for (size_t i = 0; i < g_debug_watch_count; i++) {
        printf("; watch %zu: %s\n", i + 1, g_debug_watches[i]);
    }
}

static int debug_remove_watch_index(size_t idx) {
    if (idx >= g_debug_watch_count) {
        return -1;
    }
    memmove(&g_debug_watches[idx], &g_debug_watches[idx + 1],
            (g_debug_watch_count - idx - 1) * sizeof(g_debug_watches[0]));
    g_debug_watch_count--;
    return 0;
}

static int debug_remove_watch_expr(const char *expr) {
    for (size_t i = 0; i < g_debug_watch_count; i++) {
        if (strcmp(g_debug_watches[i], expr) == 0) {
            return debug_remove_watch_index(i);
        }
    }
    return -1;
}

static int debug_should_pause_for_name(ChVM *vm, const char *name) {
    if (!name) {
        return 1;
    }
    int matched = 0;
    int has_condition = 0;
    for (size_t i = 0; i < vm->breakpoint_count; i++) {
        if (strcmp(vm->breakpoints[i], name) != 0) {
            continue;
        }
        matched = 1;
        const char *cond = g_break_conditions[i];
        if (!cond[0]) {
            return 1;
        }
        has_condition = 1;
        ChValue cond_value = CH_FALSE;
        if (repl_eval_expr_quiet(vm, cond, &cond_value) != 0) {
            fprintf(stderr, "; breakpoint condition error for %s: %s\n", name,
                    vm->error[0] ? vm->error : "evaluation failed");
            vm->error[0] = '\0';
            return 1;
        }
        if (ch_is_true_value(cond_value)) {
            return 1;
        }
    }
    if (!matched) {
        return 1;
    }
    if (has_condition) {
        return 0;
    }
    return 1;
}

static void debug_print_watches(ChVM *vm) {
    if (g_debug_watch_count == 0) {
        return;
    }
    puts("; watches:");
    for (size_t i = 0; i < g_debug_watch_count; i++) {
        ChValue value = CH_UNDEFINED;
        int rc = repl_eval_expr_quiet(vm, g_debug_watches[i], &value);
        printf(";   [%zu] %s => ", i + 1, g_debug_watches[i]);
        if (rc != 0) {
            printf("<error: %s>\n", vm->error[0] ? vm->error : "evaluation failed");
            vm->error[0] = '\0';
            continue;
        }
        ch_print_value(stdout, value, false);
        fputc('\n', stdout);
    }
}

static int debug_break_hook(ChVM *vm, const char *name) {
    if (!debug_should_pause_for_name(vm, name)) {
        return 0;
    }

    vm->debug_inspect_frame = 0;
    if (name && name[0]) {
        if (vm->script_path && vm->error_line > 0) {
            printf("; paused at %s (%s:%d)\n", name, vm->script_path, vm->error_line);
        } else {
            printf("; paused at %s\n", name);
        }
    }
    debug_print_watches(vm);

    g_debug_continue = 0;
    g_debug_abort = 0;
    while (!g_debug_continue && !g_debug_abort) {
        fputs("debug> ", stdout);
        fflush(stdout);
        char line[1024];
        if (!fgets(line, sizeof(line), stdin)) {
            g_debug_abort = 1;
            break;
        }
        char *cmd = trim_left(line);
        size_t L = strlen(cmd);
        while (L > 0 && (cmd[L - 1] == '\n' || cmd[L - 1] == '\r')) {
            cmd[--L] = '\0';
        }
        if (cmd[0] == '\0' || strcmp(cmd, ",continue") == 0 || strcmp(cmd, ",c") == 0) {
            g_debug_continue = 1;
            vm->debug_step_mode = 0;
            break;
        }
        if (strcmp(cmd, ",step") == 0 || strcmp(cmd, ",s") == 0) {
            vm->debug_step_mode = 1;
            g_debug_continue = 1;
            break;
        }
        if (strcmp(cmd, ",next") == 0 || strcmp(cmd, ",n") == 0) {
            vm->debug_step_mode = 1;
            g_debug_continue = 1;
            break;
        }
        if (strcmp(cmd, ",finish") == 0 || strcmp(cmd, ",out") == 0) {
            g_debug_continue = 1;
            vm->debug_step_mode = 0;
            break;
        }
        if (strcmp(cmd, ",backtrace") == 0 || strcmp(cmd, ",bt") == 0) {
            debug_print_backtrace(vm);
            continue;
        }
        if (strcmp(cmd, ",watch") == 0 || strcmp(cmd, ",watches") == 0) {
            debug_list_watches();
            continue;
        }
        if (strncmp(cmd, ",watch ", 7) == 0) {
            const char *expr = trim_left(cmd + 7);
            int rc = debug_add_watch_expr(expr);
            if (rc == 0) {
                printf("; watch added: %s\n", expr);
            } else if (rc == 1) {
                printf("; watch already exists: %s\n", expr);
            } else if (rc == -2) {
                fprintf(stderr, "; watch limit reached (%d)\n", CH_REPL_MAX_WATCHES);
            } else if (rc == -3) {
                fprintf(stderr, "; watch expression too long\n");
            } else {
                fprintf(stderr, "; watch: missing expression\n");
            }
            continue;
        }
        if (strcmp(cmd, ",unwatch") == 0) {
            g_debug_watch_count = 0;
            puts("; cleared watches");
            continue;
        }
        if (strncmp(cmd, ",unwatch ", 9) == 0) {
            char *arg = trim_left(cmd + 9);
            char *endptr = NULL;
            long idx = strtol(arg, &endptr, 10);
            if (endptr && *trim_left(endptr) == '\0' && idx > 0) {
                if (debug_remove_watch_index((size_t)(idx - 1)) == 0) {
                    printf("; removed watch %ld\n", idx);
                } else {
                    fprintf(stderr, "; unwatch: index out of range\n");
                }
            } else if (debug_remove_watch_expr(arg) == 0) {
                printf("; removed watch: %s\n", arg);
            } else {
                fprintf(stderr, "; unwatch: no such watch\n");
            }
            continue;
        }
        if (strcmp(cmd, ",locals") == 0) {
            if (vm->frame_count == 0) {
                puts("; no frames");
            } else {
                if (vm->debug_inspect_frame >= vm->frame_count) {
                    vm->debug_inspect_frame = 0;
                }
                size_t idx = vm->frame_count - 1 - vm->debug_inspect_frame;
                ChCallFrame *fr = &vm->frames[idx];
                const char *fname = "<thunk>";
                if (fr->closure && fr->closure->fn) {
                    ChValue clv = ch_make_pointer(&fr->closure->header);
                    for (size_t g = 0; g < vm->global_count; g++) {
                        if (vm->globals[g].defined && ch_eqv(vm->globals[g].value, clv)) {
                            fname = vm->globals[g].name->name;
                            break;
                        }
                    }
                }
                printf("; frame #%zu %s regs=%u base=%zu\n", vm->debug_inspect_frame, fname,
                       (unsigned)fr->num_regs, fr->reg_base);
                for (uint8_t r = 0; r < fr->num_regs && r < 16; r++) {
                    printf(";   r%u = ", (unsigned)r);
                    ch_print_value(stdout, vm->regs[fr->reg_base + r], false);
                    fputc('\n', stdout);
                }
            }
            continue;
        }
        if (strcmp(cmd, ",up") == 0) {
            if (vm->frame_count == 0) {
                puts("; no frames");
            } else if (vm->debug_inspect_frame + 1 >= vm->frame_count) {
                puts("; already at outermost frame");
            } else {
                vm->debug_inspect_frame++;
                printf("; frame #%zu\n", vm->debug_inspect_frame);
            }
            continue;
        }
        if (strcmp(cmd, ",down") == 0) {
            if (vm->frame_count == 0) {
                puts("; no frames");
            } else if (vm->debug_inspect_frame == 0) {
                puts("; already at innermost frame");
            } else {
                vm->debug_inspect_frame--;
                printf("; frame #%zu\n", vm->debug_inspect_frame);
            }
            continue;
        }
        if (strcmp(cmd, ",quit") == 0 || strcmp(cmd, ",abort") == 0) {
            g_debug_abort = 1;
            break;
        }
        if (strcmp(cmd, ",help") == 0) {
            puts("Debug commands:");
            puts(" -- Continue / Step:");
            puts("  ,continue / ,c    Resume execution");
            puts("  ,step             Step into next call");
            puts("  ,next / ,n        Step over");
            puts("  ,finish / ,out    Run until current frame returns");
            puts(" -- Inspect:");
            puts("  ,backtrace        Show call frames");
            puts("  ,locals           Show current frame registers");
            puts("  ,up / ,down       Navigate call frames");
            puts(" -- Watches:");
            puts("  ,watch <expr>     Add watch; ,watch lists them");
            puts("  ,unwatch [n|expr] Remove watch (none = all)");
            puts("  ,quit / ,abort    Abort debugging");
            continue;
        }
        fprintf(stderr, "debug: unknown command (try ,help)\n");
    }
    return g_debug_abort ? 1 : 0;
}

static int repl_import_spec(ChVM *vm, const char *spec_src) {
    ChReader reader;
    ch_reader_init(&reader, &vm->gc, spec_src, strlen(spec_src));

    ChValue import_root = CH_NIL;
    ChValue import_tail = CH_NIL;
    ch_gc_push(&vm->gc, &import_root);
    ch_gc_push(&vm->gc, &import_tail);

    for (;;) {
        ChValue datum = CH_NIL;
        ch_gc_push(&vm->gc, &datum);
        ChReadStatus st = read_repl_datum(vm, &reader, &datum);
        if (st == CH_READ_EOF) {
            ch_gc_pop(&vm->gc);
            break;
        }
        if (st != CH_READ_OK) {
            fprintf(stderr, "read error: %s\n", ch_reader_error(&reader));
            ch_gc_pop_n(&vm->gc, 3);
            return -1;
        }
        ChValue cell = ch_gc_cons(&vm->gc, datum, CH_NIL);
        if (import_root == CH_NIL) {
            import_root = cell;
            import_tail = cell;
        } else {
            ch_as_pair(import_tail)->cdr = cell;
            import_tail = cell;
        }
        ch_gc_pop(&vm->gc);
    }

    if (import_root == CH_NIL) {
        fprintf(stderr, ",import: missing library spec\n");
        ch_gc_pop_n(&vm->gc, 2);
        return -1;
    }

    if (ch_handle_import(vm, import_root) != 0) {
        if (vm->error[0]) {
            fprintf(stderr, "import error: %s\n", vm->error);
        } else {
            fprintf(stderr, "import error\n");
        }
        ch_gc_pop_n(&vm->gc, 2);
        return -1;
    }
    ch_gc_pop_n(&vm->gc, 2);
    return 0;
}

static int repl_dis_expr(ChVM *vm, const char *expr_src) {
    char wrap[8192];
    int n = snprintf(wrap, sizeof(wrap), "(define __dis %s)", expr_src);
    if (n < 0 || n >= (int)sizeof(wrap)) {
        fprintf(stderr, ",dis: expression too long\n");
        return -1;
    }
    if (ch_eval_source(vm, wrap, (size_t)n, 0) != 0) {
        return -1;
    }
    for (size_t i = 0; i < vm->global_count; i++) {
        if (!vm->globals[i].defined || strcmp(vm->globals[i].name->name, "__dis") != 0) {
            continue;
        }
        ChValue v = vm->globals[i].value;
        if (ch_is_closure(v)) {
            ch_disassemble_function(stdout, ch_as_closure(v)->fn);
            return 0;
        }
        if (ch_is_native(v)) {
            ChNative *nat = ch_as_native(v);
            fprintf(stderr, ",dis: native procedure %s (arity %d..%d)\n", nat->name, nat->min_arity,
                    nat->arity);
            return 0;
        }
        fprintf(stderr, ",dis: not a procedure\n");
        return -1;
    }
    fprintf(stderr, ",dis: no result\n");
    return -1;
}

static ChReadStatus read_repl_datum(ChVM *vm, ChReader *reader, ChValue *out) {
    /* Globals/macros/libraries are marked during GC; no sticky root flood. */
    (void)vm;
    return ch_read_datum(reader, out);
}

static int repl_expand_expr(ChVM *vm, const char *expr_src) {
    ChReader reader;
    ch_reader_init(&reader, &vm->gc, expr_src, strlen(expr_src));

    int saw_form = 0;
    for (;;) {
        ChValue form = CH_NIL;
        ch_gc_push(&vm->gc, &form);
        ChReadStatus st = read_repl_datum(vm, &reader, &form);
        if (st == CH_READ_EOF) {
            ch_gc_pop(&vm->gc);
            break;
        }
        if (st != CH_READ_OK) {
            fprintf(stderr, "read error: %s\n", ch_reader_error(&reader));
            ch_gc_pop(&vm->gc);
            return -1;
        }

        ChValue expanded = CH_NIL;
        ch_gc_push(&vm->gc, &expanded);
        char err[256];
        if (ch_expand_toplevel(vm, form, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
            fprintf(stderr, "expand error: %s\n", err);
            ch_gc_pop_n(&vm->gc, 2);
            return -1;
        }
        if (expanded != CH_VOID) {
            ch_print_value(stdout, expanded, false);
            fputc('\n', stdout);
        }
        saw_form = 1;
        ch_gc_pop_n(&vm->gc, 2);
    }

    if (!saw_form) {
        fprintf(stderr, ",expand: missing expression\n");
        return -1;
    }
    return 0;
}

static void repl_describe_binding(ChVM *vm, const char *name) {
    ChValue value = CH_UNDEFINED;
    if (!vm_lookup_global_value(vm, name, &value)) {
        fprintf(stderr, ",describe: no binding named '%s'\n", name);
        return;
    }
    printf("%s\n", name);
    printf("  type: %s\n", ch_value_type_name(value));
    if (ch_is_native(value)) {
        ChNative *nat = ch_as_native(value);
        if (nat->arity >= 0) {
            printf("  procedure: native (%d args)\n", nat->arity);
        } else {
            printf("  procedure: native (variadic, min %d)\n", nat->min_arity);
        }
    } else if (ch_is_closure(value)) {
        ChFunction *fn = ch_as_closure(value)->fn;
        printf("  procedure: closure (%u arg%s%s)\n", (unsigned)fn->arity,
               fn->arity == 1 ? "" : "s", fn->variadic ? "+rest" : "");
    }
    printf("  value: ");
    ch_print_value(stdout, value, false);
    fputc('\n', stdout);
}

static void repl_apropos(ChVM *vm, const char *needle) {
    size_t hits = 0;
    for (size_t i = 0; i < vm->global_count; i++) {
        if (!vm->globals[i].defined) {
            continue;
        }
        const char *name = vm->globals[i].name->name;
        if (!repl_strcasestr(name, needle)) {
            continue;
        }
        puts(name);
        hits++;
    }
    if (hits == 0) {
        puts("; no matches");
    } else {
        printf("; %zu match%s\n", hits, hits == 1 ? "" : "es");
    }
}

static int handle_comma(ChVM *vm, char *line, int *should_exit) {
    char *cmd = trim_left(line);
    *should_exit = 0;

    if (strcmp(cmd, ",help") == 0) {
        print_comma_help();
        return 0;
    }
    if (strcmp(cmd, ",quit") == 0 || strcmp(cmd, ",exit") == 0) {
        *should_exit = 1;
        return 0;
    }
    if (strcmp(cmd, ",version") == 0) {
        printf("%s\n", CHAAYA_VERSION_BANNER);
        return 0;
    }
    if (strcmp(cmd, ",gc") == 0) {
        printf("GC: %zu objects, %zu collections, threshold %zu\n", vm->gc.object_count,
               vm->gc.collections, vm->gc.threshold);
        return 0;
    }
    if (strncmp(cmd, ",env", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        const char *prefix = trim_left(cmd + 4);
        for (size_t i = 0; i < vm->global_count; i++) {
            if (!vm->globals[i].defined) {
                continue;
            }
            const char *name = vm->globals[i].name->name;
            if (prefix[0] && strncmp(name, prefix, strlen(prefix)) != 0) {
                continue;
            }
            puts(name);
        }
        return 0;
    }
    if (strcmp(cmd, ",watch") == 0 || strcmp(cmd, ",watches") == 0) {
        debug_list_watches();
        return 0;
    }
    if (strncmp(cmd, ",watch ", 7) == 0) {
        const char *expr = trim_left(cmd + 7);
        int rc = debug_add_watch_expr(expr);
        if (rc == 0) {
            printf("; watch added: %s\n", expr);
        } else if (rc == 1) {
            printf("; watch already exists: %s\n", expr);
        } else if (rc == -2) {
            fprintf(stderr, ",watch: limit reached (%d)\n", CH_REPL_MAX_WATCHES);
        } else if (rc == -3) {
            fprintf(stderr, ",watch: expression too long\n");
        } else {
            fprintf(stderr, ",watch: missing expression\n");
        }
        return 0;
    }
    if (strcmp(cmd, ",unwatch") == 0) {
        g_debug_watch_count = 0;
        puts("; cleared watches");
        return 0;
    }
    if (strncmp(cmd, ",unwatch ", 9) == 0) {
        char *arg = trim_left(cmd + 9);
        char *endptr = NULL;
        long idx = strtol(arg, &endptr, 10);
        if (endptr && *trim_left(endptr) == '\0' && idx > 0) {
            if (debug_remove_watch_index((size_t)(idx - 1)) == 0) {
                printf("; removed watch %ld\n", idx);
            } else {
                fprintf(stderr, ",unwatch: index out of range\n");
            }
            return 0;
        }
        if (debug_remove_watch_expr(arg) == 0) {
            printf("; removed watch: %s\n", arg);
            return 0;
        }
        fprintf(stderr, ",unwatch: no such watch\n");
        return 0;
    }
    if (strncmp(cmd, ",load ", 6) == 0) {
        const char *path = trim_left(cmd + 6);
        if (!path[0]) {
            fprintf(stderr, ",load: missing file\n");
            return 0;
        }
        size_t len = 0;
        char *src = ch_read_file(path, &len);
        if (!src) {
            fprintf(stderr, "Error opening file '%s'\n", path);
            return 0;
        }
        (void)ch_eval_source(vm, src, len, 1);
        free(src);
        return 0;
    }
    if (strncmp(cmd, ",expand", 7) == 0 && (cmd[7] == '\0' || cmd[7] == ' ')) {
        const char *expr = trim_left(cmd + 7);
        if (!expr[0]) {
            fprintf(stderr, ",expand: missing expression\n");
            return 0;
        }
        (void)repl_expand_expr(vm, expr);
        return 0;
    }
    if (strncmp(cmd, ",import ", 8) == 0) {
        const char *spec = trim_left(cmd + 8);
        if (!spec[0]) {
            fprintf(stderr, ",import: missing library spec\n");
            return 0;
        }
        (void)repl_import_spec(vm, spec);
        return 0;
    }
    if (strncmp(cmd, ",dis ", 5) == 0) {
        const char *expr = trim_left(cmd + 5);
        if (!expr[0]) {
            fprintf(stderr, ",dis: missing expression\n");
            return 0;
        }
        (void)repl_dis_expr(vm, expr);
        return 0;
    }
    if (strncmp(cmd, ",break ", 7) == 0) {
        char *spec = trim_left(cmd + 7);
        if (!spec[0]) {
            fprintf(stderr, ",break: missing name\n");
            return 0;
        }
        char *if_kw = strstr(spec, " if ");
        char *cond = NULL;
        if (if_kw) {
            *if_kw = '\0';
            cond = trim_left(if_kw + 4);
            if (!cond[0]) {
                fprintf(stderr, ",break: missing condition after 'if'\n");
                return 0;
            }
        }
        char *name = spec;
        size_t name_len = strlen(name);
        while (name_len > 0 && (name[name_len - 1] == ' ' || name[name_len - 1] == '\t')) {
            name[--name_len] = '\0';
        }
        if (name_len == 0) {
            fprintf(stderr, ",break: missing name\n");
            return 0;
        }
        if (vm->breakpoint_count >= CH_VM_MAX_BREAKPOINTS) {
            fprintf(stderr, ",break: too many breakpoints\n");
            return 0;
        }
        if (snprintf(vm->breakpoints[vm->breakpoint_count], sizeof(vm->breakpoints[0]), "%s",
                     name) >= (int)sizeof(vm->breakpoints[0])) {
            fprintf(stderr, ",break: name too long\n");
            return 0;
        }
        if (cond) {
            if (snprintf(g_break_conditions[vm->breakpoint_count], CH_REPL_MAX_BREAK_COND, "%s", cond) >=
                CH_REPL_MAX_BREAK_COND) {
                fprintf(stderr, ",break: condition too long\n");
                return 0;
            }
        } else {
            g_break_conditions[vm->breakpoint_count][0] = '\0';
        }
        vm->breakpoint_count++;
        vm->debug_mode = true;
        if (cond) {
            printf("; breakpoint set on %s if %s\n", name, cond);
        } else {
            printf("; breakpoint set on %s\n", name);
        }
        return 0;
    }
    if (strcmp(cmd, ",breakpoints") == 0) {
        if (vm->breakpoint_count == 0) {
            puts("; no breakpoints");
            return 0;
        }
        for (size_t i = 0; i < vm->breakpoint_count; i++) {
            if (g_break_conditions[i][0]) {
                printf("; breakpoint %zu: %s if %s\n", i + 1, vm->breakpoints[i],
                       g_break_conditions[i]);
            } else {
                printf("; breakpoint %zu: %s\n", i + 1, vm->breakpoints[i]);
            }
        }
        return 0;
    }
    if (strncmp(cmd, ",delete ", 8) == 0) {
        const char *name = trim_left(cmd + 8);
        if (!name[0]) {
            fprintf(stderr, ",delete: missing name\n");
            return 0;
        }
        if (strcmp(name, "all") == 0) {
            vm->breakpoint_count = 0;
            vm->debug_mode = false;
            for (size_t i = 0; i < CH_VM_MAX_BREAKPOINTS; i++) {
                g_break_conditions[i][0] = '\0';
            }
            puts("; cleared all breakpoints");
            return 0;
        }
        for (size_t i = 0; i < vm->breakpoint_count; i++) {
            if (strcmp(vm->breakpoints[i], name) == 0) {
                memmove(&vm->breakpoints[i], &vm->breakpoints[i + 1],
                        (vm->breakpoint_count - i - 1) * sizeof(vm->breakpoints[0]));
                memmove(&g_break_conditions[i], &g_break_conditions[i + 1],
                        (vm->breakpoint_count - i - 1) * sizeof(g_break_conditions[0]));
                vm->breakpoint_count--;
                if (vm->breakpoint_count < CH_VM_MAX_BREAKPOINTS) {
                    g_break_conditions[vm->breakpoint_count][0] = '\0';
                }
                printf("; deleted breakpoint %s\n", name);
                if (vm->breakpoint_count == 0) {
                    vm->debug_mode = false;
                }
                return 0;
            }
        }
        fprintf(stderr, ",delete: no breakpoint named '%s'\n", name);
        return 0;
    }
    if (strncmp(cmd, ",profile", 8) == 0 && (cmd[8] == '\0' || cmd[8] == ' ')) {
        const char *expr = trim_left(cmd + 8);
        if (!expr[0]) {
            fprintf(stderr, ",profile: missing expression\n");
            return 0;
        }
        ch_profile_enable();
        (void)ch_eval_source(vm, expr, strlen(expr), 1);
        ch_profile_report_text(stdout);
        ch_profile_disable();
        return 0;
    }
    if (strncmp(cmd, ",step ", 6) == 0) {
        const char *expr = trim_left(cmd + 6);
        if (!expr[0]) {
            fprintf(stderr, ",step: missing expression\n");
            return 0;
        }
        bool saved_debug = vm->debug_mode;
        vm->step_trace = true;
        (void)ch_eval_source(vm, expr, strlen(expr), 1);
        vm->debug_mode = saved_debug;
        return 0;
    }
    if (strncmp(cmd, ",time ", 6) == 0) {
        const char *expr = trim_left(cmd + 6);
        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        int rc = ch_eval_source(vm, expr, strlen(expr), 1);
        gettimeofday(&t1, NULL);
        double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                    (double)(t1.tv_usec - t0.tv_usec) / 1000.0;
        if (rc == 0) {
            printf("; %.3f ms\n", ms);
        }
        return 0;
    }
    if (strncmp(cmd, ",type ", 6) == 0) {
        const char *expr = trim_left(cmd + 6);
        /* Eval without printing; read last `_` or evaluate into temp */
        int rc = ch_eval_source(vm, expr, strlen(expr), 0);
        if (rc != 0) {
            return 0;
        }
        ChValue value = CH_VOID;
        if (vm_lookup_global_value(vm, "_", &value)) {
            printf("%s\n", ch_value_type_name(value));
            return 0;
        }
        puts("void");
        return 0;
    }
    if (strncmp(cmd, ",apropos", 8) == 0 && (cmd[8] == '\0' || cmd[8] == ' ')) {
        const char *needle = trim_left(cmd + 8);
        if (!needle[0]) {
            fprintf(stderr, ",apropos: missing search text\n");
            return 0;
        }
        repl_apropos(vm, needle);
        return 0;
    }
    if (strncmp(cmd, ",describe", 9) == 0 && (cmd[9] == '\0' || cmd[9] == ' ')) {
        const char *name = trim_left(cmd + 9);
        if (!name[0]) {
            fprintf(stderr, ",describe: missing symbol name\n");
            return 0;
        }
        repl_describe_binding(vm, name);
        return 0;
    }

    if (strncmp(cmd, ",condition", 10) == 0 && (cmd[10] == '\0' || cmd[10] == ' ')) {
        char *rest = trim_left(cmd + 10);
        if (!rest[0]) {
            fprintf(stderr, ",condition: usage ,condition <id> <expr>\n");
            return 0;
        }
        char *endptr = NULL;
        long id = strtol(rest, &endptr, 10);
        if (!endptr || endptr == rest || id <= 0) {
            fprintf(stderr, ",condition: expected breakpoint id\n");
            return 0;
        }
        char *expr = trim_left(endptr);
        if (!expr[0]) {
            fprintf(stderr, ",condition: missing expression\n");
            return 0;
        }
        size_t idx = (size_t)(id - 1);
        if (idx >= vm->breakpoint_count) {
            fprintf(stderr, ",condition: no breakpoint %ld\n", id);
            return 0;
        }
        if (snprintf(g_break_conditions[idx], CH_REPL_MAX_BREAK_COND, "%s", expr) >=
            CH_REPL_MAX_BREAK_COND) {
            fprintf(stderr, ",condition: expression too long\n");
            return 0;
        }
        printf("; breakpoint %ld condition: %s\n", id, expr);
        return 0;
    }

    fprintf(stderr, "unknown command: %s\nType ,help for available commands.\n", cmd);
    return 0;
}

#ifdef CHAAYA_HAS_LINENOISE
static void completion_callback(const char *buf, linenoiseCompletions *lc) {
    static const char *cmds[] = {
        ",help",       ",quit",        ",exit",      ",version",   ",load ",
        ",expand ",    ",import ",     ",dis ",      ",profile ",  ",break ",
        ",breakpoints", ",delete ",    ",condition ", ",watch ",   ",unwatch ",
        ",step ",      ",continue",    ",next",      ",finish",    ",backtrace",
        ",locals",     ",up",          ",down",      ",gc",        ",env",
        ",time ",      ",type ",       ",apropos ",  ",describe ", NULL};
    if (buf[0] == ',') {
        for (int i = 0; cmds[i]; i++) {
            if (strncmp(cmds[i], buf, strlen(buf)) == 0) {
                linenoiseAddCompletion(lc, cmds[i]);
            }
        }
        return;
    }
    if (!g_repl_vm) {
        return;
    }
    for (size_t i = 0; i < g_repl_vm->global_count; i++) {
        if (!g_repl_vm->globals[i].defined) {
            continue;
        }
        const char *name = g_repl_vm->globals[i].name->name;
        if (strncmp(name, buf, strlen(buf)) == 0) {
            linenoiseAddCompletion(lc, name);
        }
    }
}
#endif

static int process_input(ChVM *vm, char *text, int *should_exit) {
    char *t = trim_left(text);
    if (t[0] == '\0') {
        return 0;
    }
    if (t[0] == ',') {
        return handle_comma(vm, t, should_exit);
    }
    return ch_eval_source(vm, t, strlen(t), 1);
}

#ifdef CHAAYA_HAS_LINENOISE
static int run_linenoise(ChVM *vm) {
    printf("%s\n", CHAAYA_VERSION_BANNER);
    printf("Type ,help for commands, ,quit to exit.\n\n");

    vm->debug_break_hook = debug_break_hook;
    g_repl_vm = vm;
    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(CH_HISTORY_MAX);
    linenoiseSetCompletionCallback(completion_callback);

    char hist[1024];
    int have_hist = (history_path(hist, sizeof(hist)) == 0);
    if (have_hist) {
        linenoiseHistoryLoad(hist);
    }

    char *acc = NULL;
    size_t acc_len = 0;
    int should_exit = 0;

    while (!should_exit) {
        const char *prompt = (acc_len > 0) ? "  ... " : "chaaya> ";
        char *line = linenoise(prompt);
        if (!line) {
            fputc('\n', stdout);
            break;
        }
        if (line[0] == '\0' && acc_len == 0) {
            linenoiseFree(line);
            continue;
        }

        size_t line_len = strlen(line);
        size_t need = acc_len + line_len + 2;
        char *nacc = (char *)realloc(acc, need);
        if (!nacc) {
            linenoiseFree(line);
            free(acc);
            return 1;
        }
        acc = nacc;
        if (acc_len > 0) {
            acc[acc_len++] = '\n';
        }
        memcpy(acc + acc_len, line, line_len + 1);
        acc_len += line_len;

        linenoiseHistoryAdd(line);
        linenoiseFree(line);

        if (ch_repl_paren_depth(acc) > 0) {
            continue;
        }

        process_input(vm, acc, &should_exit);
        free(acc);
        acc = NULL;
        acc_len = 0;
        vm->error[0] = '\0';
    }

    free(acc);
    if (have_hist) {
        linenoiseHistorySave(hist);
    }
    g_repl_vm = NULL;
    return 0;
}
#endif

static int run_plain(ChVM *vm) {
    int interactive = isatty(STDIN_FILENO);
    vm->debug_break_hook = debug_break_hook;
    if (interactive) {
        printf("%s\n", CHAAYA_VERSION_BANNER);
        printf("Type ,help for commands, ,quit to exit.\n\n");
    }

    char *acc = NULL;
    size_t acc_len = 0;
    char line[4096];
    int should_exit = 0;

    while (!should_exit) {
        if (interactive) {
            fputs(acc_len > 0 ? "  ... " : "chaaya> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof(line), stdin)) {
            if (interactive) {
                fputc('\n', stdout);
            }
            break;
        }
        /* strip trailing newline */
        size_t line_len = strlen(line);
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line[--line_len] = '\0';
        }
        if (line_len == 0 && acc_len == 0) {
            continue;
        }

        size_t need = acc_len + line_len + 2;
        char *nacc = (char *)realloc(acc, need);
        if (!nacc) {
            free(acc);
            return 1;
        }
        acc = nacc;
        if (acc_len > 0) {
            acc[acc_len++] = '\n';
        }
        memcpy(acc + acc_len, line, line_len + 1);
        acc_len += line_len;

        if (ch_repl_paren_depth(acc) > 0) {
            continue;
        }

        process_input(vm, acc, &should_exit);
        free(acc);
        acc = NULL;
        acc_len = 0;
        vm->error[0] = '\0';
    }
    free(acc);
    return 0;
}

int ch_repl_run(ChVM *vm) {
#ifdef CHAAYA_HAS_LINENOISE
    if (isatty(STDIN_FILENO)) {
        return run_linenoise(vm);
    }
#endif
    return run_plain(vm);
}
