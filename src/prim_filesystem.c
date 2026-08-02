#include "chaaya/prim.h"
#include "chaaya/sandbox.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static const char *require_path(ChVM *vm, ChValue v, const char *who) {
    if (!ch_is_string(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected string path", who);
        return NULL;
    }
    const char *path = ch_as_string(v)->data;
    if (ch_sandbox_deny_fs(path)) {
        snprintf(vm->error, sizeof(vm->error),
                 "%s: denied by sandbox for path '%s'", who, path);
        return NULL;
    }
    return path;
}

static ChValue raise_file_error(ChVM *vm, const char *message, ChValue irritant) {
    ChValue msg = ch_gc_make_string_cstr(&vm->gc, message);
    ChValue irritants = CH_NIL;
    ch_gc_push(&vm->gc, &msg);
    ch_gc_push(&vm->gc, &irritants);
    if (irritant != CH_UNDEFINED) {
        ChValue item = irritant;
        ch_gc_push(&vm->gc, &item);
        irritants = ch_gc_cons(&vm->gc, item, CH_NIL);
        ch_gc_pop(&vm->gc);
    }
    ChValue err = ch_gc_make_error_object(&vm->gc, msg, irritants, 1);
    ch_gc_pop_n(&vm->gc, 2);
    return ch_vm_raise(vm, err, 0);
}

static ChFileInfo *require_file_info(ChVM *vm, ChValue v, const char *who) {
    if (!ch_is_file_info(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected file-info", who);
        return NULL;
    }
    return ch_as_file_info(v);
}

static int do_stat(const char *path, int follow, struct stat *st) {
    if (follow) {
        return stat(path, st);
    }
    return lstat(path, st);
}

static ChValue make_file_info_from_stat(ChVM *vm, struct stat *st) {
    ChFileInfo info = {
        .mode = (uint32_t)st->st_mode,
        .size = (int64_t)st->st_size,
        .mtime_sec = (int64_t)st->st_mtime,
        .atime_sec = (int64_t)st->st_atime,
        .ctime_sec = (int64_t)st->st_ctime,
        .dev = (uint64_t)st->st_dev,
        .ino = (uint64_t)st->st_ino,
        .nlinks = (uint64_t)st->st_nlink,
        .rdev = (uint64_t)st->st_rdev,
        .blksize = (int64_t)st->st_blksize,
        .blocks = (int64_t)st->st_blocks,
        .uid = (uint32_t)st->st_uid,
        .gid = (uint32_t)st->st_gid,
    };
    return ch_gc_make_file_info(&vm->gc, &info);
}

static ChValue prim_directory_files(ChVM *vm, ChValue *args, int nargs) {
    const char *path = require_path(vm, args[0], "directory-files");
    if (!path) {
        return CH_UNDEFINED;
    }
    int include_dotfiles = 0;
    if (nargs > 1) {
        include_dotfiles = ch_is_true_value(args[1]);
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return raise_file_error(vm, "directory-files: cannot open directory", args[0]);
    }
    ChValue result = CH_NIL;
    ch_gc_push(&vm->gc, &result);
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        if (!include_dotfiles && name[0] == '.') {
            continue;
        }
        ChValue str = ch_gc_make_string_cstr(&vm->gc, name);
        ch_gc_push(&vm->gc, &str);
        result = ch_gc_cons(&vm->gc, str, result);
        ch_gc_pop(&vm->gc);
    }
    closedir(dir);
    ch_gc_pop(&vm->gc);
    return result;
}

static ChValue prim_file_info(ChVM *vm, ChValue *args, int nargs) {
    const char *path = require_path(vm, args[0], "file-info");
    if (!path) {
        return CH_UNDEFINED;
    }
    int follow = 1;
    if (nargs > 1) {
        follow = ch_is_true_value(args[1]);
    }
    struct stat st;
    if (do_stat(path, follow, &st) != 0) {
        return raise_file_error(vm, "file-info: cannot stat file", args[0]);
    }
    return make_file_info_from_stat(vm, &st);
}

static ChValue prim_file_info_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_file_info(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_file_info_directory_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChFileInfo *fi = require_file_info(vm, args[0], "file-info-directory?");
    if (!fi) {
        return CH_UNDEFINED;
    }
    return S_ISDIR(fi->mode) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_file_info_regular_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChFileInfo *fi = require_file_info(vm, args[0], "file-info-regular?");
    if (!fi) {
        return CH_UNDEFINED;
    }
    return S_ISREG(fi->mode) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_file_info_symlink_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChFileInfo *fi = require_file_info(vm, args[0], "file-info-symlink?");
    if (!fi) {
        return CH_UNDEFINED;
    }
    return S_ISLNK(fi->mode) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_file_info_size(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChFileInfo *fi = require_file_info(vm, args[0], "file-info:size");
    if (!fi) {
        return CH_UNDEFINED;
    }
    return ch_make_fixnum(fi->size);
}

static ChValue prim_file_info_mtime(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChFileInfo *fi = require_file_info(vm, args[0], "file-info:mtime");
    if (!fi) {
        return CH_UNDEFINED;
    }
    return ch_make_fixnum(fi->mtime_sec);
}

static ChValue prim_file_info_mode(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChFileInfo *fi = require_file_info(vm, args[0], "file-info:mode");
    if (!fi) {
        return CH_UNDEFINED;
    }
    return ch_make_fixnum((int64_t)(fi->mode & 07777));
}

static ChValue prim_file_info_blocks(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChFileInfo *fi = require_file_info(vm, args[0], "file-info:blocks");
    if (!fi) {
        return CH_UNDEFINED;
    }
    return ch_make_fixnum(fi->blocks);
}

static ChValue prim_create_directory(ChVM *vm, ChValue *args, int nargs) {
    const char *path = require_path(vm, args[0], "create-directory");
    if (!path) {
        return CH_UNDEFINED;
    }
    mode_t mode = 0755;
    if (nargs > 1 && ch_is_fixnum(args[1])) {
        int64_t m = ch_to_fixnum(args[1]);
        if (m < 0 || m > 07777) {
            snprintf(vm->error, sizeof(vm->error), "create-directory: mode out of range");
            return CH_UNDEFINED;
        }
        mode = (mode_t)m;
    }
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        return raise_file_error(vm, "create-directory: cannot create directory", args[0]);
    }
    return CH_VOID;
}

static ChValue prim_delete_directory(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "delete-directory");
    if (!path) {
        return CH_UNDEFINED;
    }
    if (rmdir(path) != 0) {
        return raise_file_error(vm, "delete-directory: cannot delete directory", args[0]);
    }
    return CH_VOID;
}

static ChValue prim_rename_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *from = require_path(vm, args[0], "rename-file");
    if (!from) {
        return CH_UNDEFINED;
    }
    const char *to = require_path(vm, args[1], "rename-file");
    if (!to) {
        return CH_UNDEFINED;
    }
    if (rename(from, to) != 0) {
        return raise_file_error(vm, "rename-file: cannot rename file", args[0]);
    }
    return CH_VOID;
}

static ChValue prim_real_path(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "real-path");
    if (!path) {
        return CH_UNDEFINED;
    }
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        return raise_file_error(vm, "real-path: cannot resolve path", args[0]);
    }
    return ch_gc_make_string_cstr(&vm->gc, resolved);
}

static ChValue prim_current_directory(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        snprintf(vm->error, sizeof(vm->error), "current-directory: cannot read cwd");
        return CH_UNDEFINED;
    }
    return ch_gc_make_string_cstr(&vm->gc, cwd);
}

static ChValue prim_set_current_directory(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "set-current-directory!");
    if (!path) {
        return CH_UNDEFINED;
    }
    if (chdir(path) != 0) {
        return raise_file_error(vm, "set-current-directory!: cannot change directory", args[0]);
    }
    return CH_VOID;
}

static ChSymbol *file_type_symbol(ChVM *vm, mode_t mode) {
    if (S_ISREG(mode)) {
        return ch_as_symbol(ch_gc_intern_symbol_cstr(&vm->gc, "regular"));
    }
    if (S_ISDIR(mode)) {
        return ch_as_symbol(ch_gc_intern_symbol_cstr(&vm->gc, "directory"));
    }
    if (S_ISLNK(mode)) {
        return ch_as_symbol(ch_gc_intern_symbol_cstr(&vm->gc, "symlink"));
    }
    if (S_ISFIFO(mode)) {
        return ch_as_symbol(ch_gc_intern_symbol_cstr(&vm->gc, "fifo"));
    }
    if (S_ISSOCK(mode)) {
        return ch_as_symbol(ch_gc_intern_symbol_cstr(&vm->gc, "socket"));
    }
    if (S_ISCHR(mode)) {
        return ch_as_symbol(ch_gc_intern_symbol_cstr(&vm->gc, "char-special"));
    }
    if (S_ISBLK(mode)) {
        return ch_as_symbol(ch_gc_intern_symbol_cstr(&vm->gc, "block-special"));
    }
    return ch_as_symbol(ch_gc_intern_symbol_cstr(&vm->gc, "unknown"));
}

static ChValue prim_file_info_type(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChFileInfo *fi = require_file_info(vm, args[0], "file-info-type");
    if (!fi) {
        return CH_UNDEFINED;
    }
    return ch_make_pointer(&file_type_symbol(vm, fi->mode)->header);
}

static const char *default_temp_prefix(char *buf, size_t cap) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0') {
        tmpdir = "/tmp/";
    }
    size_t n = strlen(tmpdir);
    if (n + 1 >= cap) {
        return "/tmp/";
    }
    memcpy(buf, tmpdir, n + 1);
    if (n > 0 && tmpdir[n - 1] != '/') {
        if (n + 2 >= cap) {
            return "/tmp/";
        }
        buf[n] = '/';
        buf[n + 1] = '\0';
    }
    return buf;
}

static ChValue prim_temp_file_prefix(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    char buf[512];
    return ch_gc_make_string_cstr(&vm->gc, default_temp_prefix(buf, sizeof(buf)));
}

static ChValue prim_create_temp_file(ChVM *vm, ChValue *args, int nargs) {
    char prefix_buf[512];
    const char *prefix = default_temp_prefix(prefix_buf, sizeof(prefix_buf));
    if (nargs > 0) {
        prefix = require_path(vm, args[0], "create-temp-file");
        if (!prefix) {
            return CH_UNDEFINED;
        }
    }
    char template_buf[256];
    size_t plen = strlen(prefix);
    if (plen + 7 > sizeof(template_buf)) {
        return raise_file_error(vm, "temp file prefix too long", nargs > 0 ? args[0] : CH_FALSE);
    }
    memcpy(template_buf, prefix, plen);
    memcpy(template_buf + plen, "XXXXXX", 6);
    template_buf[plen + 6] = '\0';
    int fd = mkstemp(template_buf);
    if (fd < 0) {
        return raise_file_error(vm, "cannot create temp file", nargs > 0 ? args[0] : CH_FALSE);
    }
    close(fd);
    return ch_gc_make_string_cstr(&vm->gc, template_buf);
}

void ch_register_filesystem_primitives(ChVM *vm) {
    define_prim(vm, "directory-files", prim_directory_files, -1, 1);
    define_prim(vm, "file-info", prim_file_info, -1, 1);
    define_prim(vm, "file-info?", prim_file_info_p, 1, 1);
    define_prim(vm, "file-info-directory?", prim_file_info_directory_p, 1, 1);
    define_prim(vm, "file-info-regular?", prim_file_info_regular_p, 1, 1);
    define_prim(vm, "file-info-symlink?", prim_file_info_symlink_p, 1, 1);
    define_prim(vm, "file-info:size", prim_file_info_size, 1, 1);
    define_prim(vm, "file-info:mtime", prim_file_info_mtime, 1, 1);
    define_prim(vm, "file-info:mode", prim_file_info_mode, 1, 1);
    define_prim(vm, "file-info:blocks", prim_file_info_blocks, 1, 1);
    define_prim(vm, "create-directory", prim_create_directory, -1, 1);
    define_prim(vm, "delete-directory", prim_delete_directory, 1, 1);
    define_prim(vm, "rename-file", prim_rename_file, 2, 2);
    define_prim(vm, "real-path", prim_real_path, 1, 1);
    define_prim(vm, "current-directory", prim_current_directory, 0, 0);
    define_prim(vm, "set-current-directory!", prim_set_current_directory, 1, 1);
    define_prim(vm, "file-info-type", prim_file_info_type, 1, 1);
    define_prim(vm, "temp-file-prefix", prim_temp_file_prefix, 0, 0);
    define_prim(vm, "create-temp-file", prim_create_temp_file, -1, 0);
}
