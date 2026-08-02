#include "chaaya/test_runner.h"

#include "chaaya/eval.h"
#include "chaaya/printer.h"
#include "chaaya/value.h"

#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CH_TEST_MAX_PATHS 512
#define CH_TEST_MAX_JSON (1024 * 1024)

static const char *collector_prelude =
    "(import (scheme base) (scheme write) (srfi 64))\n"
    "(define %kt-pass 0)\n"
    "(define %kt-fail 0)\n"
    "(define %kt-xpass 0)\n"
    "(define %kt-xfail 0)\n"
    "(define %kt-skip 0)\n"
    "(define %kt-suite #f)\n"
    "(define %kt-failures '())\n"
    "(define (%kt-render x)\n"
    "  (let ((p (open-output-string)))\n"
    "    (write x p)\n"
    "    (get-output-string p)))\n"
    "(define (%kt-name->string x)\n"
    "  (cond ((eq? x #f) #f)\n"
    "        ((string? x) x)\n"
    "        (else (%kt-render x))))\n"
    "(define (%kt-on-group-begin r name count)\n"
    "  (if (and (not %kt-suite) (null? (test-runner-group-stack r)))\n"
    "      (set! %kt-suite (%kt-name->string name))))\n"
    "(define (%kt-on-test-end r)\n"
    "  (let ((kind (test-result-kind r))\n"
    "        (alist (test-result-alist r)))\n"
    "    (cond\n"
    "     ((eq? kind 'pass) (set! %kt-pass (+ %kt-pass 1)))\n"
    "     ((eq? kind 'xfail) (set! %kt-xfail (+ %kt-xfail 1)))\n"
    "     ((eq? kind 'skip) (set! %kt-skip (+ %kt-skip 1)))\n"
    "     ((or (eq? kind 'fail) (eq? kind 'xpass))\n"
    "      (if (eq? kind 'fail)\n"
    "          (set! %kt-fail (+ %kt-fail 1))\n"
    "          (set! %kt-xpass (+ %kt-xpass 1)))\n"
    "      (let ((nm (assq 'test-name alist))\n"
    "            (ev (assq 'expected-value alist))\n"
    "            (av (assq 'actual-value alist))\n"
    "            (sf (assq 'source-file alist))\n"
    "            (sl (assq 'source-line alist)))\n"
    "        (set! %kt-failures\n"
    "              (cons (vector (if nm (%kt-name->string (cdr nm)) #f)\n"
    "                            (symbol->string kind)\n"
    "                            (if ev (%kt-render (cdr ev)) #f)\n"
    "                            (if av (%kt-render (cdr av)) #f)\n"
    "                            (if (and sf (string? (cdr sf))) (cdr sf) #f)\n"
    "                            (if (and sl (exact-integer? (cdr sl))) (cdr sl) #f))\n"
    "                    %kt-failures))))\n"
    "     (else (set! %kt-skip (+ %kt-skip 1))))))\n"
    "(define (%kt-factory)\n"
    "  (let ((r (test-runner-null)))\n"
    "    (test-runner-on-test-end! r %kt-on-test-end)\n"
    "    (test-runner-on-group-begin! r %kt-on-group-begin)\n"
    "    r))\n"
    "(test-runner-factory %kt-factory)\n"
    "(define (%kt-collect)\n"
    "  (vector %kt-pass %kt-fail %kt-xpass %kt-xfail %kt-skip %kt-suite\n"
    "          (reverse %kt-failures)))\n";

const char *ch_test_worker_emit_path(void) {
    const char *p = getenv(CH_TEST_EMIT_ENV);
    return (p && p[0]) ? p : NULL;
}

static void json_escape(FILE *out, const char *s) {
    if (!s) {
        fputs("null", out);
        return;
    }
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (*p < 0x20) {
                fprintf(out, "\\u%04x", (unsigned)*p);
            } else {
                fputc((int)*p, out);
            }
            break;
        }
    }
    fputc('"', out);
}

static int64_t fix_or_0(ChValue v) {
    return ch_is_fixnum(v) ? ch_to_fixnum(v) : 0;
}

static const char *string_or_null(ChValue v, char *buf, size_t buflen) {
    if (!ch_is_string(v)) {
        return NULL;
    }
    ChString *s = ch_as_string(v);
    size_t n = s->len < buflen - 1 ? s->len : buflen - 1;
    memcpy(buf, s->data, n);
    buf[n] = '\0';
    return buf;
}

int ch_test_install_collector(ChVM *vm) {
    if (ch_eval_source(vm, collector_prelude, strlen(collector_prelude), 0) != 0) {
        return -1;
    }
    const char *seed_str = getenv(CH_TEST_SEED_ENV);
    if (seed_str && seed_str[0]) {
        char prog[256];
        snprintf(prog, sizeof(prog),
                 "(import (srfi 27)) (random-source-pseudo-randomize! "
                 "default-random-source 0 %s)",
                 seed_str);
        (void)ch_eval_source(vm, prog, strlen(prog), 0);
        vm->error[0] = '\0';
    }
    return 0;
}

static int resolve_verdict(int toplevel_error, int exit_requested, uint8_t exit_code,
                           int counters_failed, const char **note_out) {
    *note_out = NULL;
    if (!exit_requested) {
        return toplevel_error;
    }
    if (exit_code == 0) {
        if (toplevel_error) {
            *note_out = "uncaught top-level error, acknowledged by an explicit (exit 0)";
        }
        return 0;
    }
    if (counters_failed) {
        return 0;
    }
    *note_out = "file requested a nonzero exit with no failing test to explain it";
    return 1;
}

void ch_test_emit_result(ChVM *vm, const char *emit_path, const char *file_path,
                         int toplevel_error, const char *err_msg, double duration_ms) {
    if (!emit_path || !emit_path[0]) {
        return;
    }
    FILE *f = fopen(emit_path, "w");
    if (!f) {
        return;
    }

    int64_t pass = 0, fail = 0, xpass = 0, xfail = 0, skip = 0;
    char suite_buf[256];
    const char *suite = NULL;
    ChValue failures = CH_NIL;

    ChValue collected = CH_FALSE;
    {
        const char *collect_src = "(define %kt-emit-tmp (%kt-collect))";
        if (ch_eval_source(vm, collect_src, strlen(collect_src), 0) == 0) {
            for (size_t i = 0; i < vm->global_count; i++) {
                if (vm->globals[i].defined && vm->globals[i].name &&
                    strcmp(vm->globals[i].name->name, "%kt-emit-tmp") == 0) {
                    collected = vm->globals[i].value;
                    break;
                }
            }
        }
    }
    vm->error[0] = '\0';

    if (ch_is_vector(collected)) {
        ChVector *vec = ch_as_vector(collected);
        if (vec->len >= 7) {
            pass = fix_or_0(vec->items[0]);
            fail = fix_or_0(vec->items[1]);
            xpass = fix_or_0(vec->items[2]);
            xfail = fix_or_0(vec->items[3]);
            skip = fix_or_0(vec->items[4]);
            suite = string_or_null(vec->items[5], suite_buf, sizeof(suite_buf));
            failures = vec->items[6];
        }
    }

    int64_t tests = pass + fail + xpass + xfail + skip;
    const char *note = NULL;
    int errored = resolve_verdict(toplevel_error, vm->exit_requested, vm->exit_code,
                                  (fail > 0 || xpass > 0), &note);
    const char *error_message = err_msg ? err_msg : note;

    fputs("{\"type\":\"file\",\"file\":", f);
    json_escape(f, file_path);
    fputs(",\"suite\":", f);
    if (suite) {
        json_escape(f, suite);
    } else {
        fputs("null", f);
    }
    fprintf(f,
            ",\"tests\":%lld,\"pass\":%lld,\"fail\":%lld,\"xpass\":%lld,\"xfail\":%lld,"
            "\"skip\":%lld,\"error\":%s,\"error_message\":",
            (long long)tests, (long long)pass, (long long)fail, (long long)xpass, (long long)xfail,
            (long long)skip, errored ? "true" : "false");
    if (error_message) {
        json_escape(f, error_message);
    } else {
        fputs("null", f);
    }
    fprintf(f, ",\"duration_ms\":%.3f,\"failures\":[", duration_ms);

    int first = 1;
    ChValue cur = failures;
    while (ch_is_pair(cur)) {
        ChValue rec = ch_car(cur);
        cur = ch_cdr(cur);
        if (!ch_is_vector(rec)) {
            continue;
        }
        ChVector *rv = ch_as_vector(rec);
        if (rv->len < 6) {
            continue;
        }
        if (!first) {
            fputc(',', f);
        }
        first = 0;
        char nb[256], kb[64], eb[512], ab[512], sfb[256];
        const char *nm = string_or_null(rv->items[0], nb, sizeof(nb));
        const char *kind = string_or_null(rv->items[1], kb, sizeof(kb));
        const char *ev = string_or_null(rv->items[2], eb, sizeof(eb));
        const char *av = string_or_null(rv->items[3], ab, sizeof(ab));
        const char *sf = string_or_null(rv->items[4], sfb, sizeof(sfb));
        fputs("{\"name\":", f);
        if (nm) {
            json_escape(f, nm);
        } else {
            fputs("null", f);
        }
        fputs(",\"kind\":", f);
        if (kind) {
            json_escape(f, kind);
        } else {
            fputs("null", f);
        }
        fputs(",\"expected\":", f);
        if (ev) {
            json_escape(f, ev);
        } else {
            fputs("null", f);
        }
        fputs(",\"actual\":", f);
        if (av) {
            json_escape(f, av);
        } else {
            fputs("null", f);
        }
        fputs(",\"source_file\":", f);
        if (sf) {
            json_escape(f, sf);
        } else {
            fputs("null", f);
        }
        fputs(",\"source_line\":", f);
        if (ch_is_fixnum(rv->items[5])) {
            fprintf(f, "%lld", (long long)ch_to_fixnum(rv->items[5]));
        } else {
            fputs("null", f);
        }
        fputc('}', f);
    }
    fputs("]}\n", f);
    fclose(f);
}

int ch_test_run_worker_file(ChVM *vm, const char *path, const char *emit_path) {
    vm->suppress_exit = true;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (ch_test_install_collector(vm) != 0) {
        ch_test_emit_result(vm, emit_path, path, 1, "test collector setup failed", 0.0);
        return CH_EXIT_OK;
    }

    size_t len = 0;
    char *src = ch_read_file(path, &len);
    int toplevel_error = 0;
    if (!src) {
        toplevel_error = 1;
        snprintf(vm->error, sizeof(vm->error), "Error opening file '%s'", path);
    } else {
        if (ch_eval_source(vm, src, len, 0) != 0) {
            toplevel_error = 1;
        }
        free(src);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    ch_test_emit_result(vm, emit_path, path, toplevel_error, NULL, ms);
    return CH_EXIT_OK;
}

/* ── Discovery / selection ───────────────────────────────────────────── */

static int is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_scm(const char *name) {
    size_t n = strlen(name);
    return n >= 4 && strcmp(name + n - 4, ".scm") == 0;
}

static int discover_dir(const char *dir_path, char **out, size_t *count, size_t depth) {
    if (depth > 32 || *count >= CH_TEST_MAX_PATHS) {
        return 0;
    }
    DIR *d = opendir(dir_path);
    if (!d) {
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && *count < CH_TEST_MAX_PATHS) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name) >= (int)sizeof(full)) {
            continue;
        }
        if (is_dir(full)) {
            if (discover_dir(full, out, count, depth + 1) != 0) {
                closedir(d);
                return -1;
            }
        } else if (is_scm(ent->d_name)) {
            out[*count] = strdup(full);
            if (!out[*count]) {
                closedir(d);
                return -1;
            }
            (*count)++;
        }
    }
    closedir(d);
    return 0;
}

static int path_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static size_t discover_paths(ChCliOptions *opts, char **out) {
    size_t count = 0;
    if (!opts->file && opts->script_arg_count == 0) {
        if (is_dir("tests")) {
            (void)discover_dir("tests", out, &count, 0);
        }
        qsort(out, count, sizeof(char *), path_cmp);
        return count;
    }
    if (opts->file) {
        if (is_dir(opts->file)) {
            (void)discover_dir(opts->file, out, &count, 0);
        } else {
            out[count++] = strdup(opts->file);
        }
    }
    for (size_t i = 0; i < opts->script_arg_count && count < CH_TEST_MAX_PATHS; i++) {
        const char *p = opts->script_args[i];
        if (is_dir(p)) {
            (void)discover_dir(p, out, &count, 0);
        } else {
            out[count++] = strdup(p);
        }
    }
    qsort(out, count, sizeof(char *), path_cmp);
    return count;
}

static int git_changed_files(const char *since, char **changed, size_t *changed_count) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "git diff --name-only %s 2>/dev/null", since ? since : "HEAD");
    FILE *p = popen(cmd, "r");
    if (!p) {
        return -1;
    }
    char line[PATH_MAX];
    *changed_count = 0;
    while (fgets(line, sizeof(line), p) && *changed_count < CH_TEST_MAX_PATHS) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0) {
            continue;
        }
        changed[*changed_count] = strdup(line);
        if (!changed[*changed_count]) {
            pclose(p);
            return -1;
        }
        (*changed_count)++;
    }
    int st = pclose(p);
    return (st == 0) ? 0 : -1;
}

static int looks_native(const char *path) {
    return strstr(path, "csrc/") != NULL || strstr(path, ".dylib") != NULL ||
           strstr(path, ".so") != NULL || strstr(path, "src/") != NULL;
}

static int path_affected(const char *suite, char **changed, size_t changed_count) {
    for (size_t i = 0; i < changed_count; i++) {
        if (strcmp(suite, changed[i]) == 0) {
            return 1;
        }
        /* Same directory prefix or basename match — safety-over-precision. */
        if (strstr(suite, changed[i]) != NULL || strstr(changed[i], suite) != NULL) {
            return 1;
        }
        const char *base = strrchr(changed[i], '/');
        base = base ? base + 1 : changed[i];
        if (strstr(suite, base) != NULL) {
            return 1;
        }
    }
    return 0;
}

static size_t select_changed(char **all, size_t all_count, const char *since, char **out,
                             int *full_run, char *note, size_t note_len) {
    char *changed[CH_TEST_MAX_PATHS];
    size_t changed_count = 0;
    if (git_changed_files(since, changed, &changed_count) != 0) {
        *full_run = 1;
        snprintf(note, note_len, "git unavailable or bad revision; running all %zu suite(s)",
                 all_count);
        for (size_t i = 0; i < all_count; i++) {
            out[i] = all[i];
        }
        return all_count;
    }
    for (size_t i = 0; i < changed_count; i++) {
        if (looks_native(changed[i])) {
            *full_run = 1;
            snprintf(note, note_len, "native/source change (%s); running all suites", changed[i]);
            for (size_t j = 0; j < changed_count; j++) {
                free(changed[j]);
            }
            for (size_t j = 0; j < all_count; j++) {
                out[j] = all[j];
            }
            return all_count;
        }
    }
    size_t n = 0;
    for (size_t i = 0; i < all_count; i++) {
        if (path_affected(all[i], changed, changed_count)) {
            out[n++] = all[i];
        }
    }
    for (size_t i = 0; i < changed_count; i++) {
        free(changed[i]);
    }
    *full_run = 0;
    snprintf(note, note_len, "selected %zu/%zu suite(s) affected since %s", n, all_count,
             since ? since : "HEAD");
    return n;
}

/* ── Orchestrator ────────────────────────────────────────────────────── */

typedef struct {
    uint64_t files;
    uint64_t files_failed;
    uint64_t errors;
    uint64_t pass;
    uint64_t fail;
    uint64_t xpass;
    uint64_t xfail;
    uint64_t skip;
} ChTestTotals;

static void print_json_escaped_stdout(const char *s) {
    json_escape(stdout, s);
}

static char *read_file_alloc(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) {
        *out_len = n;
    }
    return buf;
}

static int parse_file_json_counts(const char *json, ChTestTotals *t, int *errored, int *file_failed) {
    *errored = 0;
    *file_failed = 0;
    if (!json) {
        *errored = 1;
        *file_failed = 1;
        return -1;
    }
    long pass = 0, fail = 0, xpass = 0, xfail = 0, skip = 0;
    const char *p;
    if ((p = strstr(json, "\"pass\":"))) {
        pass = strtol(p + 7, NULL, 10);
    }
    if ((p = strstr(json, "\"fail\":"))) {
        fail = strtol(p + 7, NULL, 10);
    }
    if ((p = strstr(json, "\"xpass\":"))) {
        xpass = strtol(p + 8, NULL, 10);
    }
    if ((p = strstr(json, "\"xfail\":"))) {
        xfail = strtol(p + 8, NULL, 10);
    }
    if ((p = strstr(json, "\"skip\":"))) {
        skip = strtol(p + 7, NULL, 10);
    }
    if ((p = strstr(json, "\"error\":"))) {
        *errored = (strncmp(p + 8, "true", 4) == 0);
    }
    t->pass += (uint64_t)pass;
    t->fail += (uint64_t)fail;
    t->xpass += (uint64_t)xpass;
    t->xfail += (uint64_t)xfail;
    t->skip += (uint64_t)skip;
    if (fail > 0 || xpass > 0 || *errored) {
        *file_failed = 1;
    }
    return 0;
}

static pid_t spawn_worker(const char *argv0, ChCliOptions *opts, const char *path,
                          const char *emit_path, unsigned seed, int seed_set) {
    char exe[PATH_MAX];
    if (realpath(argv0, exe) == NULL) {
        snprintf(exe, sizeof(exe), "%s", argv0);
    }

    char dir_copy[PATH_MAX];
    char file_copy[PATH_MAX];
    snprintf(dir_copy, sizeof(dir_copy), "%s", path);
    snprintf(file_copy, sizeof(file_copy), "%s", path);
    char *dir = dirname(dir_copy);
    const char *file = basename(file_copy);

    char lib_abs[CH_VM_MAX_LIB_PATHS][PATH_MAX];
    const char *resolved_libs[CH_VM_MAX_LIB_PATHS];
    size_t lib_count = opts->lib_path_count;
    for (size_t i = 0; i < lib_count; i++) {
        if (realpath(opts->lib_paths[i], lib_abs[i]) != NULL) {
            resolved_libs[i] = lib_abs[i];
        } else {
            resolved_libs[i] = opts->lib_paths[i];
        }
    }
#ifndef CHAAYA_SOURCE_DIR
#define CHAAYA_SOURCE_DIR "."
#endif
    char bundled_lib[PATH_MAX];
    if (lib_count == 0) {
        snprintf(bundled_lib, sizeof(bundled_lib), "%s/lib", CHAAYA_SOURCE_DIR);
        if (realpath(bundled_lib, lib_abs[0]) != NULL) {
            resolved_libs[0] = lib_abs[0];
        } else {
            resolved_libs[0] = bundled_lib;
        }
        lib_count = 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        if (chdir(dir) != 0) {
            _exit(127);
        }
        setenv(CH_TEST_EMIT_ENV, emit_path, 1);
        if (seed_set) {
            char seedbuf[32];
            snprintf(seedbuf, sizeof(seedbuf), "%u", seed);
            setenv(CH_TEST_SEED_ENV, seedbuf, 1);
        }
        if (opts->test_json) {
            (void)freopen("/dev/null", "w", stdout);
            (void)freopen("/dev/null", "w", stderr);
        }
        char *child_argv[4 + CH_VM_MAX_LIB_PATHS * 2];
        size_t argc = 0;
        child_argv[argc++] = exe;
        for (size_t i = 0; i < lib_count; i++) {
            child_argv[argc++] = (char *)"--lib-path";
            child_argv[argc++] = (char *)resolved_libs[i];
        }
        child_argv[argc++] = (char *)file;
        child_argv[argc] = NULL;
        execv(exe, child_argv);
        _exit(127);
    }
    return pid;
}

static void report_human(const char *path, const char *json, int file_failed, int errored) {
    if (file_failed) {
        fprintf(stderr, "FAIL %s", path);
        if (errored) {
            fprintf(stderr, " (error)");
        }
        fputc('\n', stderr);
        if (json) {
            const char *fail = strstr(json, "\"failures\":[");
            if (fail) {
                /* Print a short excerpt of failure names if present. */
                const char *nm = strstr(fail, "\"name\":");
                if (nm) {
                    nm += 7;
                    if (*nm == '"') {
                        nm++;
                        const char *end = strchr(nm, '"');
                        if (end) {
                            fprintf(stderr, "  first failure: %.*s\n", (int)(end - nm), nm);
                        }
                    }
                }
            }
        }
    } else {
        printf("PASS %s\n", path);
    }
}

static int run_one(const char *argv0, ChCliOptions *opts, const char *path, size_t index,
                   unsigned seed, int seed_set, ChTestTotals *totals) {
    char emit_path[PATH_MAX];
    snprintf(emit_path, sizeof(emit_path), "/tmp/chaaya-test-%d-%zu.json", (int)getpid(), index);

    pid_t pid = spawn_worker(argv0, opts, path, emit_path, seed, seed_set);
    totals->files++;
    if (pid < 0) {
        totals->errors++;
        totals->files_failed++;
        if (opts->test_json) {
            fputs("{\"type\":\"file\",\"file\":", stdout);
            print_json_escaped_stdout(path);
            fputs(",\"error\":true,\"error_message\":\"spawn failed\",\"failures\":[]}\n", stdout);
        } else {
            fprintf(stderr, "FAIL %s (spawn failed)\n", path);
        }
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        totals->errors++;
        totals->files_failed++;
        unlink(emit_path);
        return -1;
    }

    size_t jlen = 0;
    char *json = read_file_alloc(emit_path, &jlen);
    unlink(emit_path);

    int errored = 0;
    int file_failed = 0;
    if (!json || !WIFEXITED(status)) {
        errored = 1;
        file_failed = 1;
        totals->errors++;
    } else {
        parse_file_json_counts(json, totals, &errored, &file_failed);
        if (errored) {
            totals->errors++;
        }
    }
    if (file_failed) {
        totals->files_failed++;
    }

    if (opts->test_json) {
        if (json && json[0]) {
            fputs(json, stdout);
            if (json[strlen(json) - 1] != '\n') {
                fputc('\n', stdout);
            }
        } else {
            fputs("{\"type\":\"file\",\"file\":", stdout);
            print_json_escaped_stdout(path);
            fputs(",\"error\":true,\"error_message\":\"missing worker result\",\"failures\":[]}\n",
                  stdout);
        }
    } else {
        report_human(path, json, file_failed, errored);
    }
    free(json);
    return file_failed ? -1 : 0;
}

int ch_test_run(const char *argv0, ChCliOptions *opts) {
    char *all[CH_TEST_MAX_PATHS];
    size_t all_count = discover_paths(opts, all);
    if (all_count == 0) {
        fprintf(stderr,
                "test: no suites found (pass .scm paths, or run from a repo with ./tests)\n");
        fprintf(stderr, "Usage: %s test [--json] [-j N] [--seed N] [--changed] [paths...]\n",
                argv0);
        return CH_EXIT_USAGE;
    }

    unsigned seed = opts->test_seed_set ? opts->test_seed : (unsigned)time(NULL);
    fprintf(stderr, "chaaya test: seed %u (reproduce with: chaaya test --seed %u)\n", seed, seed);

    char *selected[CH_TEST_MAX_PATHS];
    size_t path_count = all_count;
    char **paths = all;
    if (opts->test_changed || opts->test_list_affected) {
        int full_run = 0;
        char note[256];
        path_count = select_changed(all, all_count, opts->test_since, selected, &full_run, note,
                                    sizeof(note));
        paths = selected;
        fprintf(stderr, "chaaya test: %s\n", note);
        if (opts->test_list_affected) {
            for (size_t i = 0; i < path_count; i++) {
                if (opts->test_json) {
                    fputs("{\"type\":\"affected\",\"file\":", stdout);
                    print_json_escaped_stdout(paths[i]);
                    fputs("}\n", stdout);
                } else {
                    printf("%s\n", paths[i]);
                }
            }
            for (size_t i = 0; i < all_count; i++) {
                free(all[i]);
            }
            return CH_EXIT_OK;
        }
    }

    ChTestTotals totals = {0};
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int jobs = opts->test_jobs > 0 ? opts->test_jobs : 1;
    if ((size_t)jobs > path_count) {
        jobs = (int)path_count;
    }

    if (jobs <= 1 || path_count <= 1) {
        for (size_t i = 0; i < path_count; i++) {
            (void)run_one(argv0, opts, paths[i], i, seed, 1, &totals);
        }
    } else {
        typedef struct {
            pid_t pid;
            const char *path;
            size_t index;
            char emit_path[PATH_MAX];
        } Running;
        Running *running = (Running *)calloc((size_t)jobs, sizeof(Running));
        if (!running) {
            for (size_t i = 0; i < path_count; i++) {
                (void)run_one(argv0, opts, paths[i], i, seed, 1, &totals);
            }
        } else {
            size_t next = 0;
            size_t active = 0;
            while (next < path_count || active > 0) {
                while (next < path_count && active < (size_t)jobs) {
                    snprintf(running[active].emit_path, sizeof(running[active].emit_path),
                             "/tmp/chaaya-test-%d-%zu.json", (int)getpid(), next);
                    pid_t pid = spawn_worker(argv0, opts, paths[next], running[active].emit_path,
                                             seed, 1);
                    if (pid < 0) {
                        totals.files++;
                        totals.errors++;
                        totals.files_failed++;
                        if (opts->test_json) {
                            fputs("{\"type\":\"file\",\"file\":", stdout);
                            print_json_escaped_stdout(paths[next]);
                            fputs(",\"error\":true,\"error_message\":\"spawn failed\",\"failures\":[]}\n",
                                  stdout);
                        } else {
                            fprintf(stderr, "FAIL %s (spawn failed)\n", paths[next]);
                        }
                        next++;
                        continue;
                    }
                    running[active].pid = pid;
                    running[active].path = paths[next];
                    running[active].index = next;
                    active++;
                    next++;
                }
                if (active == 0) {
                    continue;
                }
                int status = 0;
                pid_t done = waitpid(-1, &status, 0);
                if (done < 0) {
                    break;
                }
                size_t idx = 0;
                while (idx < active && running[idx].pid != done) {
                    idx++;
                }
                if (idx == active) {
                    continue;
                }
                totals.files++;
                size_t jlen = 0;
                char *json = read_file_alloc(running[idx].emit_path, &jlen);
                unlink(running[idx].emit_path);
                int errored = 0, file_failed = 0;
                if (!json || !WIFEXITED(status)) {
                    errored = 1;
                    file_failed = 1;
                    totals.errors++;
                } else {
                    parse_file_json_counts(json, &totals, &errored, &file_failed);
                    if (errored) {
                        totals.errors++;
                    }
                }
                if (file_failed) {
                    totals.files_failed++;
                }
                if (opts->test_json) {
                    if (json && json[0]) {
                        fputs(json, stdout);
                        if (json[strlen(json) - 1] != '\n') {
                            fputc('\n', stdout);
                        }
                    }
                } else {
                    report_human(running[idx].path, json, file_failed, errored);
                }
                free(json);
                running[idx] = running[active - 1];
                active--;
            }
            free(running);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double total_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                      (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;

    if (opts->test_json) {
        printf("{\"type\":\"summary\",\"files\":%llu,\"files_failed\":%llu,\"errors\":%llu,"
               "\"pass\":%llu,\"fail\":%llu,\"xpass\":%llu,\"xfail\":%llu,\"skip\":%llu,"
               "\"seed\":%u,\"duration_ms\":%.3f}\n",
               (unsigned long long)totals.files, (unsigned long long)totals.files_failed,
               (unsigned long long)totals.errors, (unsigned long long)totals.pass,
               (unsigned long long)totals.fail, (unsigned long long)totals.xpass,
               (unsigned long long)totals.xfail, (unsigned long long)totals.skip, seed, total_ms);
    } else {
        printf("test: %llu file(s), pass=%llu fail=%llu xpass=%llu xfail=%llu skip=%llu "
               "errors=%llu (%.1f ms)\n",
               (unsigned long long)totals.files, (unsigned long long)totals.pass,
               (unsigned long long)totals.fail, (unsigned long long)totals.xpass,
               (unsigned long long)totals.xfail, (unsigned long long)totals.skip,
               (unsigned long long)totals.errors, total_ms);
    }

    for (size_t i = 0; i < all_count; i++) {
        free(all[i]);
    }

    if (totals.fail > 0 || totals.xpass > 0 || totals.errors > 0) {
        return CH_EXIT_ERROR;
    }
    return CH_EXIT_OK;
}
