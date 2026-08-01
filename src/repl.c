#include "chaaya/repl.h"

#include "chaaya/eval.h"
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
    puts("  ,gc               Show GC statistics");
    puts("  ,env [prefix]     List defined globals");
    puts("  ,time <expr>      Evaluate and print elapsed milliseconds");
    puts("  ,type <expr>      Evaluate and print result type");
    puts("");
    puts("Other Kaappi comma-commands are not implemented yet in the bootstrap.");
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
    static const char *nyi[] = {",break",  ",breakpoints", ",delete", ",step",   ",condition",
                                ",expand", ",profile",     ",dis",    ",describe", ",apropos",
                                ",import", NULL};
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
    static const char *cmds[] = {",help", ",quit", ",exit", ",version", ",load ",
                                 ",gc",   ",env",  ",time ", ",type ",   NULL};
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
