#include "chaaya/repl.h"

#include "chaaya/disasm.h"
#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/library.h"
#include "chaaya/printer.h"
#include "chaaya/reader.h"
#include "chaaya/version.h"

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
    puts("Comma commands:");
    puts("  ,help             Show this message");
    puts("  ,quit / ,exit     Exit the REPL");
    puts("  ,version          Show Chaaya version");
    puts("  ,load <file>      Load and evaluate a Scheme file");
    puts("  ,expand <expr>    Expand expression and print transformed form");
    puts("  ,import <lib>     Import a library (e.g. ,import (srfi 64))");
    puts("  ,dis <expr>       Disassemble a procedure");
    puts("  ,break <name>     Break when calling the named procedure");
    puts("  ,breakpoints      List active breakpoints");
    puts("  ,delete <name>    Remove a breakpoint");
    puts("  ,step <expr>      Evaluate; pause on next call (interactive debugger)");
    puts("  ,continue         Resume from a breakpoint (debug> prompt)");
    puts("  ,next / ,finish   Step/finish from a breakpoint");
    puts("  ,backtrace        Show VM call frames at a breakpoint");
    puts("  ,locals           Show current frame register count");
    puts("  ,gc               Show GC statistics");
    puts("  ,env [prefix]     List defined globals");
    puts("  ,time <expr>      Evaluate and print elapsed milliseconds");
    puts("  ,type <expr>      Evaluate and print result type");
    puts("");
    puts("Other Kaappi comma-commands (,profile, ,apropos, …) are not implemented yet.");
}

static int g_debug_continue = 0;
static int g_debug_abort = 0;

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

static int debug_break_hook(ChVM *vm, const char *name) {
    (void)name;
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
        if (strcmp(cmd, ",locals") == 0) {
            if (vm->frame_count == 0) {
                puts("; no frames");
            } else {
                ChCallFrame *fr = &vm->frames[vm->frame_count - 1];
                printf("; frame regs=%u base=%zu\n", (unsigned)fr->num_regs, fr->reg_base);
                for (uint8_t r = 0; r < fr->num_regs && r < 16; r++) {
                    printf(";   r%u = ", (unsigned)r);
                    ch_print_value(stdout, vm->regs[fr->reg_base + r], false);
                    fputc('\n', stdout);
                }
            }
            continue;
        }
        if (strcmp(cmd, ",up") == 0 || strcmp(cmd, ",down") == 0) {
            puts("; frame navigation: use ,backtrace (single-frame focus MVP)");
            continue;
        }
        if (strcmp(cmd, ",quit") == 0 || strcmp(cmd, ",abort") == 0) {
            g_debug_abort = 1;
            break;
        }
        if (strcmp(cmd, ",help") == 0) {
            puts("debug commands: ,continue ,step ,next ,finish ,backtrace ,locals ,quit");
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
        const char *name = trim_left(cmd + 7);
        if (!name[0]) {
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
        vm->breakpoint_count++;
        vm->debug_mode = true;
        printf("; breakpoint set on %s\n", name);
        return 0;
    }
    if (strcmp(cmd, ",breakpoints") == 0) {
        if (vm->breakpoint_count == 0) {
            puts("; no breakpoints");
            return 0;
        }
        for (size_t i = 0; i < vm->breakpoint_count; i++) {
            printf("; breakpoint %zu: %s\n", i + 1, vm->breakpoints[i]);
        }
        return 0;
    }
    if (strncmp(cmd, ",delete ", 8) == 0) {
        const char *name = trim_left(cmd + 8);
        if (!name[0]) {
            fprintf(stderr, ",delete: missing name\n");
            return 0;
        }
        for (size_t i = 0; i < vm->breakpoint_count; i++) {
            if (strcmp(vm->breakpoints[i], name) == 0) {
                memmove(&vm->breakpoints[i], &vm->breakpoints[i + 1],
                        (vm->breakpoint_count - i - 1) * sizeof(vm->breakpoints[0]));
                vm->breakpoint_count--;
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
        size_t before = vm->global_count;
        (void)before;
        int rc = ch_eval_source(vm, expr, strlen(expr), 0);
        if (rc != 0) {
            return 0;
        }
        /* Find `_` */
        for (size_t i = 0; i < vm->global_count; i++) {
            if (vm->globals[i].defined && strcmp(vm->globals[i].name->name, "_") == 0) {
                printf("%s\n", ch_value_type_name(vm->globals[i].value));
                return 0;
            }
        }
        puts("void");
        return 0;
    }

    /* Known Kaappi commands not yet supported */
    static const char *nyi[] = {",condition", ",profile", ",describe", ",apropos", NULL};
    for (int i = 0; nyi[i]; i++) {
        size_t n = strlen(nyi[i]);
        if (strncmp(cmd, nyi[i], n) == 0 && (cmd[n] == '\0' || cmd[n] == ' ')) {
            fprintf(stderr, "chaaya: '%s' is not implemented yet (bootstrap)\n", nyi[i]);
            return 0;
        }
    }

    fprintf(stderr, "unknown command: %s\nType ,help for available commands.\n", cmd);
    return 0;
}

#ifdef CHAAYA_HAS_LINENOISE
static void completion_callback(const char *buf, linenoiseCompletions *lc) {
    static const char *cmds[] = {",help",     ",quit", ",exit", ",version", ",load ", ",expand ",
                                 ",import ", ",dis ",   ",break ",   ",breakpoints", ",delete ",
                                 ",step ", ",continue", ",backtrace", ",locals", ",gc", ",env",
                                 ",time ", ",type ", NULL};
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
