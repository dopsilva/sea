/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * sea_lib_fs.c — the built-in `fs` (file system) and `path` modules.
 *
 * fs: failures are error values ("fs.read: data.txt: No such file or
 * directory"); fs.exists / isfile / isdir answer with a bool. These calls
 * block their scheduler thread like other file I/O (see CLAUDE.md).
 *
 *   fs.read(p) → string             fs.write(p, data [, opts]) → true
 *   fs.append(p, data) → true       fs.exists(p) / isfile(p) / isdir(p) → bool
 *   fs.stat(p) / lstat(p) → table   fs.list(p) → array   fs.walk(p [, opts]) → array
 *   fs.mkdir(p [, opts]) → true     fs.remove(p [, opts]) → true
 *   fs.rename(a, b) / copy(a, b [, opts]) / symlink(target, link) → true
 *   fs.readlink(p) / realpath(p) → string     fs.chmod(p, mode) / touch(p) → true
 *   fs.tempdir([prefix]) / tempfile([prefix]) → string
 *   fs.cwd() → string   fs.chdir(p) → true   fs.glob(pattern) → array
 *
 * path: string manipulation only (no file system access except path.abs,
 * which reads the working directory). Same rules as Go's path/filepath on
 * Unix: path.normalize("a//b/../c/") is "a/c".
 */

#include "sea_array.h"
#include "sea_map.h"
#include "sea_rope.h"
#include "sea_stdlib.h"
#include "sea_struct.h"
#include "sea_vm.h"
#include "sea_compat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define WALK_MAX_DEPTH 256

/* ---- Helpers ---------------------------------------------------------------- */

static SeaValue new_str(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }
static SeaValue new_cstr(const char *s) { return new_str(s, strlen(s)); }

static void tset(SeaTable *t, const char *k, SeaValue v) {   /* takes v */
    sea_table_set_str(t, k, v);
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

/* "fs.<fn>: <path>: <strerror(err)>" */
static SeaValue fs_err(SeaVM *vm, const char *fn, const char *path, int err) {
    char msg[PATH_MAX + 160];
    if (path) snprintf(msg, sizeof msg, "fs.%s: %s: %s", fn, path, strerror(err));
    else snprintf(msg, sizeof msg, "fs.%s: %s", fn, strerror(err));
    return sea_make_error_str(vm, msg);
}

/* A path argument: a flat string without NUL bytes (+1: release it). */
static SeaString *path_arg(SeaVM *vm, const char *mod, const char *fn, int argc, SeaValue *args, int i) {
    SeaString *s = argc > i ? sea_value_get_flat_string(args[i]) : NULL;
    if (!s)
        sea_vm_error_noreturn(vm, "%s.%s: path (string) expected, got %s", mod, fn,
                              argc > i ? sea_type_name(args[i]) : "nothing");
    if (strlen(s->data) != s->len) {
        sea_obj_release(&s->obj);
        sea_vm_error_noreturn(vm, "%s.%s: a path cannot contain a NUL byte", mod, fn);
    }
    return s;
}

static bool opt_get(SeaValue opts, const char *field, SeaValue *out) {
    *out = SEA_NIL_VAL;
    if (sea_is_table(opts)) return sea_table_get_str(SEA_AS_TABLE(opts), field, out) && !sea_is_nil(*out);
    if (sea_is_struct(opts)) {
        SeaString *k = sea_string_intern(field, strlen(field));
        *out = sea_struct_get(SEA_AS_STRUCT(opts), k);
        sea_obj_release(&k->obj);
        return !sea_is_nil(*out);
    }
    if (sea_is_map(opts)) {
        SeaString *k = sea_string_intern(field, strlen(field));
        bool found = sea_map_get(SEA_AS_MAP(opts), sea_obj_val(&k->obj), out);
        sea_obj_release(&k->obj);
        return found && !sea_is_nil(*out);
    }
    return false;
}

static bool opt_flag(SeaValue opts, const char *field) {
    SeaValue v;
    return opt_get(opts, field, &v) && sea_is_bool(v) && sea_as_bool(v);
}

static int64_t opt_int(SeaValue opts, const char *field, int64_t def) {
    SeaValue v;
    if (!opt_get(opts, field, &v)) return def;
    if (sea_is_int(v)) return sea_as_int(v);
    if (sea_is_number(v)) return (int64_t)sea_as_number(v);
    return def;
}

/* A growable byte buffer (malloc). */
typedef struct { char *data; size_t len, cap; } Buf;

static void buf_push(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->len + n + 1) nc *= 2;
        b->data = realloc(b->data, nc);
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static int write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int cmp_cstr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Names in a directory (no "." / ".."), sorted; NULL with errno on failure. */
static char **list_dir(const char *path, size_t *count) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            names = realloc(names, cap * sizeof *names);
        }
        names[n++] = strdup(nm);
    }
    closedir(d);
    if (n) qsort(names, n, sizeof *names, cmp_cstr);
    if (!names) names = malloc(sizeof *names);    /* empty but not NULL */
    *count = n;
    return names;
}

static void free_names(char **names, size_t n) {
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}

static char *join2(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    bool slash = la > 0 && a[la - 1] != '/';
    char *r = malloc(la + lb + 2);
    memcpy(r, a, la);
    if (slash) r[la++] = '/';
    memcpy(r + la, b, lb + 1);
    return r;
}

/* ======================================================================== */
/* fs                                                                       */
/* ======================================================================== */

static SeaValue fs_read(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "read", argc, args, 0);
    int fd = open(p->data, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { SeaValue e = fs_err(vm, "read", p->data, errno); sea_obj_release(&p->obj); return e; }
    struct stat st;
    size_t cap = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) ? (size_t)st.st_size : 4096;
    char *buf = malloc(cap + 1);
    size_t len = 0;
    for (;;) {
        if (len == cap) { cap *= 2; buf = realloc(buf, cap + 1); }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR) continue;
            int err = errno;
            free(buf);
            close(fd);
            SeaValue e = fs_err(vm, "read", p->data, err);
            sea_obj_release(&p->obj);
            return e;
        }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(fd);
    sea_obj_release(&p->obj);
    SeaValue v = new_str(buf, len);
    free(buf);
    return v;
}

/* Write data to path: flags O_TRUNC or O_APPEND; atomic: a temporary file
 * in the same directory renamed over path. */
static SeaValue write_file(SeaVM *vm, const char *fn, int argc, SeaValue *args, bool append) {
    SeaString *p = path_arg(vm, "fs", fn, argc, args, 0);
    SeaString *data = argc > 1 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!data) {
        sea_obj_release(&p->obj);
        sea_vm_error_noreturn(vm, "fs.%s: data (string) expected, got %s", fn, argc > 1 ? sea_type_name(args[1]) : "nothing");
    }
    SeaValue opts = argc > 2 ? args[2] : SEA_NIL_VAL;
    if (opt_flag(opts, "append")) append = true;
    mode_t mode = (mode_t)opt_int(opts, "mode", 0666);
    bool atomic = !append && opt_flag(opts, "atomic");
    SeaValue r = SEA_TRUE_VAL;
    if (atomic) {
        char *tmp = malloc(p->len + 16);
        snprintf(tmp, p->len + 16, "%s.tmpXXXXXX", p->data);
        int fd = mkstemp(tmp);
        int err = 0;
        if (fd < 0) err = errno;
        else {
            if (fchmod(fd, mode & ~(mode_t)0) != 0 ||
                write_all(fd, data->data, data->len) != 0 || fsync(fd) != 0) err = errno;
            if (close(fd) != 0 && !err) err = errno;
            if (!err && rename(tmp, p->data) != 0) err = errno;
            if (err) unlink(tmp);
        }
        /* mkstemp creates 0600: apply the umask to the requested mode */
        if (!err) {
            mode_t um = umask(0);
            umask(um);
            if (chmod(p->data, mode & ~um) != 0) err = errno;
        }
        free(tmp);
        if (err) r = fs_err(vm, fn, p->data, err);
    } else {
        int fd = open(p->data, O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), mode);
        if (fd < 0) r = fs_err(vm, fn, p->data, errno);
        else {
            int err = write_all(fd, data->data, data->len) != 0 ? errno : 0;
            if (close(fd) != 0 && !err) err = errno;
            if (err) r = fs_err(vm, fn, p->data, err);
        }
    }
    sea_obj_release(&data->obj);
    sea_obj_release(&p->obj);
    return r;
}

static SeaValue fs_write(SeaVM *vm, int argc, SeaValue *args) { return write_file(vm, "write", argc, args, false); }
static SeaValue fs_append(SeaVM *vm, int argc, SeaValue *args) { return write_file(vm, "append", argc, args, true); }

static SeaValue fs_exists(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "exists", argc, args, 0);
    struct stat st;
    bool ok = lstat(p->data, &st) == 0;
    sea_obj_release(&p->obj);
    return sea_bool_val(ok);
}

static SeaValue fs_isfile(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "isfile", argc, args, 0);
    struct stat st;
    bool ok = stat(p->data, &st) == 0 && S_ISREG(st.st_mode);
    sea_obj_release(&p->obj);
    return sea_bool_val(ok);
}

static SeaValue fs_isdir(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "isdir", argc, args, 0);
    struct stat st;
    bool ok = stat(p->data, &st) == 0 && S_ISDIR(st.st_mode);
    sea_obj_release(&p->obj);
    return sea_bool_val(ok);
}

static const char *type_name(mode_t m) {
    if (S_ISREG(m)) return "file";
    if (S_ISDIR(m)) return "dir";
    if (S_ISLNK(m)) return "link";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "socket";
    if (S_ISCHR(m)) return "char";
    if (S_ISBLK(m)) return "block";
    return "other";
}

static double ts(struct timespec t) { return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }

static SeaValue stat_impl(SeaVM *vm, const char *fn, int argc, SeaValue *args, bool follow) {
    SeaString *p = path_arg(vm, "fs", fn, argc, args, 0);
    struct stat st;
    if ((follow ? stat(p->data, &st) : lstat(p->data, &st)) != 0) {
        SeaValue e = fs_err(vm, fn, p->data, errno);
        sea_obj_release(&p->obj);
        return e;
    }
    sea_obj_release(&p->obj);
    SeaTable *t = sea_table_new(TABLE_MAP);
    tset(t, "type", new_cstr(type_name(st.st_mode)));
    tset(t, "size", sea_int_val((int64_t)st.st_size));
    tset(t, "mode", sea_int_val((int64_t)(st.st_mode & 07777)));
    tset(t, "mtime", sea_number_val(ts(SEA_ST_MTIM(st))));
    tset(t, "atime", sea_number_val(ts(SEA_ST_ATIM(st))));
    tset(t, "ctime", sea_number_val(ts(SEA_ST_CTIM(st))));
    tset(t, "uid", sea_int_val((int64_t)st.st_uid));
    tset(t, "gid", sea_int_val((int64_t)st.st_gid));
    tset(t, "ino", sea_int_val((int64_t)(st.st_ino & ((UINT64_C(1) << 47) - 1))));
    tset(t, "nlink", sea_int_val((int64_t)st.st_nlink));
    return sea_obj_val(&t->obj);
}

static SeaValue fs_stat(SeaVM *vm, int argc, SeaValue *args) { return stat_impl(vm, "stat", argc, args, true); }
static SeaValue fs_lstat(SeaVM *vm, int argc, SeaValue *args) { return stat_impl(vm, "lstat", argc, args, false); }

static SeaValue names_array(char **names, size_t n, const char *prefix) {
    SeaArray *a = sea_array_new((uint32_t)(n ? n : 1));
    for (size_t i = 0; i < n; i++) {
        SeaValue v;
        if (prefix) {
            char *full = join2(prefix, names[i]);
            v = new_cstr(full);
            free(full);
        } else {
            v = new_cstr(names[i]);
        }
        sea_array_push(a, v);
        sea_obj_release(sea_as_obj(v));
    }
    return sea_obj_val(&a->obj);
}

static SeaValue fs_list(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "list", argc, args, 0);
    size_t n;
    char **names = list_dir(p->data, &n);
    if (!names) { SeaValue e = fs_err(vm, "list", p->data, errno); sea_obj_release(&p->obj); return e; }
    SeaValue r = names_array(names, n, NULL);
    free_names(names, n);
    sea_obj_release(&p->obj);
    return r;
}

typedef struct {
    SeaArray *out;
    bool      files_only, follow;
    int       max_depth;
    dev_t     dev[WALK_MAX_DEPTH + 1];     /* directories on the path (link loops) */
    ino_t     ino[WALK_MAX_DEPTH + 1];
    int       err;
    char     *err_path;
} Walk;

static void walk_dir(Walk *w, const char *dir, int depth) {
    if (depth >= w->max_depth || w->err) return;
    size_t n;
    char **names = list_dir(dir, &n);
    if (!names) {
        w->err = errno;
        w->err_path = strdup(dir);
        return;
    }
    for (size_t i = 0; i < n && !w->err; i++) {
        char *full = join2(dir, names[i]);
        struct stat st;
        int rc = w->follow ? stat(full, &st) : lstat(full, &st);
        if (rc != 0) rc = lstat(full, &st);        /* a dangling link is still an entry */
        bool is_dir = rc == 0 && S_ISDIR(st.st_mode);
        if (!w->files_only || !is_dir) {
            SeaValue v = new_cstr(full);
            sea_array_push(w->out, v);
            sea_obj_release(sea_as_obj(v));
        }
        if (is_dir && depth + 1 <= WALK_MAX_DEPTH) {
            bool loop = false;
            for (int k = 0; k < depth + 1; k++)
                if (w->dev[k] == st.st_dev && w->ino[k] == st.st_ino) loop = true;
            if (!loop) {
                w->dev[depth + 1] = st.st_dev;
                w->ino[depth + 1] = st.st_ino;
                walk_dir(w, full, depth + 1);
            }
        }
        free(full);
    }
    free_names(names, n);
}

/* fs.walk(root [, opts]): every path under root, depth first, sorted by name
 * in each directory; opts: files (only non-directories), max_depth,
 * follow_links (descend into linked directories) */
static SeaValue fs_walk(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "walk", argc, args, 0);
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    Walk *w = calloc(1, sizeof *w);
    w->out = sea_array_new(16);
    w->files_only = opt_flag(opts, "files");
    w->follow = opt_flag(opts, "follow_links");
    int64_t md = opt_int(opts, "max_depth", WALK_MAX_DEPTH);
    w->max_depth = md < 1 ? 1 : md > WALK_MAX_DEPTH ? WALK_MAX_DEPTH : (int)md;
    struct stat st;
    SeaValue r;
    if (stat(p->data, &st) != 0) {
        r = fs_err(vm, "walk", p->data, errno);
        sea_obj_release(&w->out->obj);
    } else {
        w->dev[0] = st.st_dev;
        w->ino[0] = st.st_ino;
        walk_dir(w, p->data, 0);
        if (w->err) {
            r = fs_err(vm, "walk", w->err_path, w->err);
            sea_obj_release(&w->out->obj);
        } else {
            r = sea_obj_val(&w->out->obj);
        }
    }
    free(w->err_path);
    free(w);
    sea_obj_release(&p->obj);
    return r;
}

static int mkdir_parents(char *path, mode_t mode) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = '\0';
    for (char *s = path + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        if (mkdir(path, 0777) != 0 && errno != EEXIST) { *s = '/'; return -1; }
        *s = '/';
    }
    if (mkdir(path, mode) != 0) {
        if (errno != EEXIST) return -1;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    }
    return 0;
}

/* fs.mkdir(p [, opts]): opts parents (create missing parents; an existing
 * directory is fine), mode (default 0777, less the umask) */
static SeaValue fs_mkdir(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "mkdir", argc, args, 0);
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    mode_t mode = (mode_t)opt_int(opts, "mode", 0777);
    int rc;
    if (opt_flag(opts, "parents")) {
        char *tmp = strdup(p->data);
        rc = mkdir_parents(tmp, mode);
        free(tmp);
    } else {
        rc = mkdir(p->data, mode);
    }
    SeaValue r = rc == 0 ? SEA_TRUE_VAL : fs_err(vm, "mkdir", p->data, errno);
    sea_obj_release(&p->obj);
    return r;
}

/* Remove a tree (links are removed, not followed). 0 or -1 with errno; *where
 * gets the failing path. */
static int remove_tree(const char *path, char **where) {
    struct stat st;
    if (lstat(path, &st) != 0) { *where = strdup(path); return -1; }
    if (S_ISDIR(st.st_mode)) {
        size_t n;
        char **names = list_dir(path, &n);
        if (!names) { *where = strdup(path); return -1; }
        for (size_t i = 0; i < n; i++) {
            char *full = join2(path, names[i]);
            int rc = remove_tree(full, where);
            free(full);
            if (rc != 0) { free_names(names, n); return -1; }
        }
        free_names(names, n);
        if (rmdir(path) != 0) { *where = strdup(path); return -1; }
        return 0;
    }
    if (unlink(path) != 0) { *where = strdup(path); return -1; }
    return 0;
}

/* fs.remove(p [, opts]): a file, link or empty directory; opts recursive
 * (a whole tree), force (a missing path is not an error) */
static SeaValue fs_remove(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "remove", argc, args, 0);
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    bool force = opt_flag(opts, "force");
    SeaValue r = SEA_TRUE_VAL;
    if (opt_flag(opts, "recursive")) {
        char *where = NULL;
        if (remove_tree(p->data, &where) != 0 && !(force && errno == ENOENT && strcmp(where, p->data) == 0))
            r = fs_err(vm, "remove", where, errno);
        free(where);
    } else if (remove(p->data) != 0 && !(force && errno == ENOENT)) {
        r = fs_err(vm, "remove", p->data, errno);
    }
    sea_obj_release(&p->obj);
    return r;
}

static SeaValue fs_rename(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *a = path_arg(vm, "fs", "rename", argc, args, 0);
    SeaString *b = path_arg(vm, "fs", "rename", argc, args, 1);
    SeaValue r = rename(a->data, b->data) == 0 ? SEA_TRUE_VAL : fs_err(vm, "rename", a->data, errno);
    sea_obj_release(&a->obj);
    sea_obj_release(&b->obj);
    return r;
}

static int copy_file(const char *from, const char *to, mode_t mode, bool overwrite) {
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (overwrite ? 0 : O_EXCL), mode);
    if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    char buf[65536];
    int rc = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; rc = -1; break; }
        if (n == 0) break;
        if (write_all(out, buf, (size_t)n) != 0) { rc = -1; break; }
    }
    int e = errno;
    close(in);
    if (close(out) != 0 && rc == 0) { rc = -1; e = errno; }
    if (rc == 0 && chmod(to, mode) != 0) { rc = -1; e = errno; }
    errno = e;
    return rc;
}

static int copy_tree(const char *from, const char *to, bool overwrite, char **where) {
    struct stat st;
    if (lstat(from, &st) != 0) { *where = strdup(from); return -1; }
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(from, target, sizeof target - 1);
        if (n < 0) { *where = strdup(from); return -1; }
        target[n] = '\0';
        if (overwrite) unlink(to);
        if (symlink(target, to) != 0) { *where = strdup(to); return -1; }
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(to, st.st_mode & 07777) != 0 && errno != EEXIST) { *where = strdup(to); return -1; }
        size_t n;
        char **names = list_dir(from, &n);
        if (!names) { *where = strdup(from); return -1; }
        for (size_t i = 0; i < n; i++) {
            char *a = join2(from, names[i]), *b = join2(to, names[i]);
            int rc = copy_tree(a, b, overwrite, where);
            free(a);
            free(b);
            if (rc != 0) { free_names(names, n); return -1; }
        }
        free_names(names, n);
        return 0;
    }
    if (copy_file(from, to, st.st_mode & 07777, overwrite) != 0) { *where = strdup(from); return -1; }
    return 0;
}

/* fs.copy(from, to [, opts]): a file (keeping its mode); opts recursive (a
 * directory tree; links are copied as links), overwrite (default true) */
static SeaValue fs_copy(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *a = path_arg(vm, "fs", "copy", argc, args, 0);
    SeaString *b = path_arg(vm, "fs", "copy", argc, args, 1);
    SeaValue opts = argc > 2 ? args[2] : SEA_NIL_VAL;
    SeaValue ov;
    bool overwrite = !(opt_get(opts, "overwrite", &ov) && sea_is_bool(ov) && !sea_as_bool(ov));
    SeaValue r = SEA_TRUE_VAL;
    struct stat st;
    if (stat(a->data, &st) != 0) {
        r = fs_err(vm, "copy", a->data, errno);
    } else if (S_ISDIR(st.st_mode)) {
        if (!opt_flag(opts, "recursive")) {
            r = fs_err(vm, "copy", a->data, EISDIR);
        } else {
            char *where = NULL;
            if (copy_tree(a->data, b->data, overwrite, &where) != 0) r = fs_err(vm, "copy", where, errno);
            free(where);
        }
    } else if (copy_file(a->data, b->data, st.st_mode & 07777, overwrite) != 0) {
        r = fs_err(vm, "copy", errno == EEXIST ? b->data : a->data, errno);
    }
    sea_obj_release(&a->obj);
    sea_obj_release(&b->obj);
    return r;
}

static SeaValue fs_symlink(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *t = path_arg(vm, "fs", "symlink", argc, args, 0);
    SeaString *l = path_arg(vm, "fs", "symlink", argc, args, 1);
    SeaValue r = symlink(t->data, l->data) == 0 ? SEA_TRUE_VAL : fs_err(vm, "symlink", l->data, errno);
    sea_obj_release(&t->obj);
    sea_obj_release(&l->obj);
    return r;
}

static SeaValue fs_readlink(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "readlink", argc, args, 0);
    char buf[PATH_MAX];
    ssize_t n = readlink(p->data, buf, sizeof buf);
    SeaValue r = n < 0 ? fs_err(vm, "readlink", p->data, errno) : new_str(buf, (size_t)n);
    sea_obj_release(&p->obj);
    return r;
}

static SeaValue fs_realpath(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "realpath", argc, args, 0);
    char buf[PATH_MAX];
    SeaValue r = realpath(p->data, buf) ? new_cstr(buf) : fs_err(vm, "realpath", p->data, errno);
    sea_obj_release(&p->obj);
    return r;
}

static SeaValue fs_chmod(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !sea_is_int(args[1]))
        sea_vm_error_noreturn(vm, "fs.chmod: mode (int) expected, got %s", argc > 1 ? sea_type_name(args[1]) : "nothing");
    SeaString *p = path_arg(vm, "fs", "chmod", argc, args, 0);
    SeaValue r = chmod(p->data, (mode_t)sea_as_int(args[1])) == 0 ? SEA_TRUE_VAL : fs_err(vm, "chmod", p->data, errno);
    sea_obj_release(&p->obj);
    return r;
}

/* fs.touch(p): create an empty file, or set an existing one's times to now */
static SeaValue fs_touch(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "touch", argc, args, 0);
    SeaValue r = SEA_TRUE_VAL;
    int fd = open(p->data, O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) r = fs_err(vm, "touch", p->data, errno);
    else {
        if (futimens(fd, NULL) != 0) r = fs_err(vm, "touch", p->data, errno);
        close(fd);
    }
    sea_obj_release(&p->obj);
    return r;
}

static const char *tmp_root(void) {
    const char *t = getenv("TMPDIR");
    return t && *t ? t : "/tmp";
}

static SeaValue temp_impl(SeaVM *vm, const char *fn, int argc, SeaValue *args, bool dir) {
    SeaString *pre = argc > 0 && !sea_is_nil(args[0]) ? path_arg(vm, "fs", fn, argc, args, 0) : NULL;
    if (pre && strchr(pre->data, '/')) {
        sea_obj_release(&pre->obj);
        sea_vm_error_noreturn(vm, "fs.%s: the prefix cannot contain '/'", fn);
    }
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "%s/%sXXXXXX", tmp_root(), pre ? pre->data : "sea-");
    if (pre) sea_obj_release(&pre->obj);
    SeaValue r;
    if (dir) {
        r = mkdtemp(tmpl) ? new_cstr(tmpl) : fs_err(vm, fn, NULL, errno);
    } else {
        int fd = mkstemp(tmpl);
        if (fd < 0) r = fs_err(vm, fn, NULL, errno);
        else { close(fd); r = new_cstr(tmpl); }
    }
    return r;
}

static SeaValue fs_tempdir(SeaVM *vm, int argc, SeaValue *args) { return temp_impl(vm, "tempdir", argc, args, true); }
static SeaValue fs_tempfile(SeaVM *vm, int argc, SeaValue *args) { return temp_impl(vm, "tempfile", argc, args, false); }

static SeaValue fs_cwd(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    char buf[PATH_MAX];
    return getcwd(buf, sizeof buf) ? new_cstr(buf) : fs_err(vm, "cwd", NULL, errno);
}

static SeaValue fs_chdir(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "chdir", argc, args, 0);
    SeaValue r = chdir(p->data) == 0 ? SEA_TRUE_VAL : fs_err(vm, "chdir", p->data, errno);
    sea_obj_release(&p->obj);
    return r;
}

/* fs.glob(pattern): sorted matching paths (* ? [...] {a,b} ~); none: [] */
static SeaValue fs_glob(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "fs", "glob", argc, args, 0);
    glob_t g;
    memset(&g, 0, sizeof g);
    int rc = glob(p->data, GLOB_BRACE | GLOB_TILDE, NULL, &g);
    SeaValue r;
    if (rc == 0 || rc == GLOB_NOMATCH) {
        r = names_array(g.gl_pathv, rc == 0 ? g.gl_pathc : 0, NULL);
    } else {
        r = fs_err(vm, "glob", p->data, rc == GLOB_NOSPACE ? ENOMEM : EIO);
    }
    globfree(&g);
    sea_obj_release(&p->obj);
    return r;
}

/* ======================================================================== */
/* path                                                                     */
/* ======================================================================== */

/* Lexical clean (Go's filepath.Clean): one slash between elements, no "."
 * elements, ".." removes the element before it (at the root it is dropped;
 * in a relative path leading ".." stay); "" becomes ".". */
static void clean(const char *p, size_t n, Buf *out) {
    bool rooted = n > 0 && p[0] == '/';
    out->len = 0;
    if (rooted) buf_push(out, "/", 1);
    size_t dotdot = out->len;              /* where ".." can no longer back up */
    size_t i = rooted ? 1 : 0;
    while (i < n) {
        if (p[i] == '/') { i++; continue; }
        size_t j = i;
        while (j < n && p[j] != '/') j++;
        size_t len = j - i;
        if (len == 1 && p[i] == '.') {
            /* skip */
        } else if (len == 2 && p[i] == '.' && p[i + 1] == '.') {
            if (out->len > dotdot) {
                /* back up to the previous slash */
                size_t k = out->len;
                while (k > dotdot && out->data[k - 1] != '/') k--;
                out->len = k > dotdot ? k - 1 : dotdot;
                if (out->len < dotdot) out->len = dotdot;
                out->data[out->len] = '\0';
            } else if (!rooted) {
                if (out->len > 0) buf_push(out, "/", 1);
                buf_push(out, "..", 2);
                dotdot = out->len;
            }
        } else {
            if (out->len > (rooted ? 1u : 0u)) buf_push(out, "/", 1);
            buf_push(out, p + i, len);
        }
        i = j;
    }
    if (out->len == 0) buf_push(out, ".", 1);
}

static SeaValue buf_value(Buf *b) {
    SeaValue v = new_str(b->data ? b->data : "", b->len);
    free(b->data);
    b->data = NULL;
    return v;
}

static SeaValue p_normalize(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "path", "normalize", argc, args, 0);
    Buf b = { 0 };
    clean(p->data, p->len, &b);
    sea_obj_release(&p->obj);
    return buf_value(&b);
}

/* path.join(a, b, ...): the parts joined with "/" and normalized; an
 * absolute part starts over; empty parts are skipped */
static SeaValue p_join(SeaVM *vm, int argc, SeaValue *args) {
    Buf raw = { 0 };
    for (int i = 0; i < argc; i++) {
        SeaString *s = path_arg(vm, "path", "join", argc, args, i);
        if (s->len) {
            if (s->data[0] == '/') raw.len = 0;
            else if (raw.len) buf_push(&raw, "/", 1);
            buf_push(&raw, s->data, s->len);
        }
        sea_obj_release(&s->obj);
    }
    if (raw.len == 0) { free(raw.data); return new_str("", 0); }
    Buf b = { 0 };
    clean(raw.data, raw.len, &b);
    free(raw.data);
    return buf_value(&b);
}

/* The last element's range in p (trailing slashes ignored). */
static void last_elem(const char *p, size_t n, size_t *start, size_t *end) {
    while (n > 0 && p[n - 1] == '/') n--;
    size_t s = n;
    while (s > 0 && p[s - 1] != '/') s--;
    *start = s;
    *end = n;
}

/* path.basename(p [, suffix]): the last element ("" → ".", "/" → "/");
 * a matching suffix is removed */
static SeaValue p_basename(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "path", "basename", argc, args, 0);
    SeaValue r;
    if (p->len == 0) r = new_str(".", 1);
    else {
        size_t s, e;
        last_elem(p->data, p->len, &s, &e);
        if (e == 0) r = new_str("/", 1);
        else {
            if (argc > 1 && sea_is_string(args[1])) {
                SeaString *suf = SEA_AS_STRING(args[1]);
                if (suf->len < e - s && memcmp(p->data + e - suf->len, suf->data, suf->len) == 0) e -= suf->len;
            }
            r = new_str(p->data + s, e - s);
        }
    }
    sea_obj_release(&p->obj);
    return r;
}

/* path.dirname(p): everything before the last element, normalized */
static SeaValue p_dirname(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "path", "dirname", argc, args, 0);
    size_t n = p->len;
    size_t k = n;
    while (k > 0 && p->data[k - 1] != '/') k--;
    Buf b = { 0 };
    clean(p->data, k, &b);
    sea_obj_release(&p->obj);
    return buf_value(&b);
}

/* Start of the extension in the last element [s, e) (e for none; leading
 * dots do not start one, so ".bashrc" and ".." have none). */
static size_t ext_start(const char *p, size_t s, size_t e) {
    while (s < e && p[s] == '.') s++;              /* leading dots: "..", ".bashrc" */
    for (size_t i = e; i > s; i--)
        if (p[i - 1] == '.') return i - 1;
    return e;
}

static SeaValue p_ext(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "path", "ext", argc, args, 0);
    size_t s, e;
    last_elem(p->data, p->len, &s, &e);
    size_t x = ext_start(p->data, s, e);
    SeaValue r = new_str(p->data + x, e - x);
    sea_obj_release(&p->obj);
    return r;
}

/* path.stem(p): the last element without its extension */
static SeaValue p_stem(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "path", "stem", argc, args, 0);
    size_t s, e;
    last_elem(p->data, p->len, &s, &e);
    SeaValue r = new_str(p->data + s, ext_start(p->data, s, e) - s);
    sea_obj_release(&p->obj);
    return r;
}

static SeaValue p_isabs(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "path", "isabs", argc, args, 0);
    bool abs = p->len > 0 && p->data[0] == '/';
    sea_obj_release(&p->obj);
    return sea_bool_val(abs);
}

/* path.abs(p): absolute and normalized (relative paths are joined to the
 * working directory; links are not resolved: see fs.realpath) */
static SeaValue p_abs(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "path", "abs", argc, args, 0);
    Buf raw = { 0 };
    if (p->len == 0 || p->data[0] != '/') {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) {
            SeaValue e = fs_err(vm, "cwd", NULL, errno);
            sea_obj_release(&p->obj);
            return e;
        }
        buf_push(&raw, cwd, strlen(cwd));
        buf_push(&raw, "/", 1);
    }
    buf_push(&raw, p->data, p->len);
    Buf b = { 0 };
    clean(raw.data, raw.len, &b);
    free(raw.data);
    sea_obj_release(&p->obj);
    return buf_value(&b);
}

/* Elements of a cleaned path ("." has none), as [start, end) pairs. */
typedef struct { size_t n; size_t s[PATH_MAX / 2], e[PATH_MAX / 2]; } Elems;

static void split_elems(const char *p, Elems *out) {
    out->n = 0;
    if (strcmp(p, ".") == 0) return;
    size_t i = p[0] == '/' ? 1 : 0, len = strlen(p);
    while (i < len && out->n < PATH_MAX / 2) {
        size_t j = i;
        while (j < len && p[j] != '/') j++;
        out->s[out->n] = i;
        out->e[out->n] = j;
        out->n++;
        i = j + 1;
    }
}

/* path.relative(base, target): a path that, joined to base, gives target
 * (both normalized first; both absolute or both relative, else an error) */
static SeaValue p_relative(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *a = path_arg(vm, "path", "relative", argc, args, 0);
    SeaString *t = path_arg(vm, "path", "relative", argc, args, 1);
    Buf ba = { 0 }, bt = { 0 };
    clean(a->data, a->len, &ba);
    clean(t->data, t->len, &bt);
    Elems *ea = malloc(sizeof *ea), *et = malloc(sizeof *et);
    split_elems(ba.data, ea);
    split_elems(bt.data, et);
    size_t k = 0;
    while (k < ea->n && k < et->n && ea->e[k] - ea->s[k] == et->e[k] - et->s[k] &&
           memcmp(ba.data + ea->s[k], bt.data + et->s[k], ea->e[k] - ea->s[k]) == 0)
        k++;
    bool bad = (ba.data[0] == '/') != (bt.data[0] == '/');
    for (size_t i = k; i < ea->n && !bad; i++)       /* base still has ".." past the common part */
        if (ea->e[i] - ea->s[i] == 2 && memcmp(ba.data + ea->s[i], "..", 2) == 0) bad = true;
    SeaValue r;
    if (bad) {
        char msg[PATH_MAX * 2 + 64];
        snprintf(msg, sizeof msg, "path.relative: cannot make %s relative to %s", t->data, a->data);
        r = sea_make_error_str(vm, msg);
    } else {
        Buf out = { 0 };
        for (size_t i = k; i < ea->n; i++) {
            if (out.len) buf_push(&out, "/", 1);
            buf_push(&out, "..", 2);
        }
        for (size_t i = k; i < et->n; i++) {
            if (out.len) buf_push(&out, "/", 1);
            buf_push(&out, bt.data + et->s[i], et->e[i] - et->s[i]);
        }
        if (out.len == 0) buf_push(&out, ".", 1);
        r = buf_value(&out);
    }
    free(ea);
    free(et);
    free(ba.data);
    free(bt.data);
    sea_obj_release(&a->obj);
    sea_obj_release(&t->obj);
    return r;
}

/* path.parts(p): the elements of the normalized path ("/" first when absolute) */
static SeaValue p_parts(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *p = path_arg(vm, "path", "parts", argc, args, 0);
    Buf b = { 0 };
    clean(p->data, p->len, &b);
    sea_obj_release(&p->obj);
    SeaArray *a = sea_array_new(8);
    size_t i = 0;
    if (b.data[0] == '/') {
        SeaValue v = new_str("/", 1);
        sea_array_push(a, v);
        sea_obj_release(sea_as_obj(v));
        i = 1;
    }
    while (i < b.len) {
        size_t j = i;
        while (j < b.len && b.data[j] != '/') j++;
        if (j > i) {
            SeaValue v = new_str(b.data + i, j - i);
            sea_array_push(a, v);
            sea_obj_release(sea_as_obj(v));
        }
        i = j + 1;
    }
    free(b.data);
    return sea_obj_val(&a->obj);
}

/* ---- Registration ------------------------------------------------------------ */

void sea_lib_fs_register(SeaVM *vm) {
    SeaTable *fs = sea_table_new(TABLE_MAP);
    mod_set(vm, fs, "read",     fs_read);
    mod_set(vm, fs, "write",    fs_write);
    mod_set(vm, fs, "append",   fs_append);
    mod_set(vm, fs, "exists",   fs_exists);
    mod_set(vm, fs, "isfile",   fs_isfile);
    mod_set(vm, fs, "isdir",    fs_isdir);
    mod_set(vm, fs, "stat",     fs_stat);
    mod_set(vm, fs, "lstat",    fs_lstat);
    mod_set(vm, fs, "list",     fs_list);
    mod_set(vm, fs, "walk",     fs_walk);
    mod_set(vm, fs, "mkdir",    fs_mkdir);
    mod_set(vm, fs, "remove",   fs_remove);
    mod_set(vm, fs, "rename",   fs_rename);
    mod_set(vm, fs, "copy",     fs_copy);
    mod_set(vm, fs, "symlink",  fs_symlink);
    mod_set(vm, fs, "readlink", fs_readlink);
    mod_set(vm, fs, "realpath", fs_realpath);
    mod_set(vm, fs, "chmod",    fs_chmod);
    mod_set(vm, fs, "touch",    fs_touch);
    mod_set(vm, fs, "tempdir",  fs_tempdir);
    mod_set(vm, fs, "tempfile", fs_tempfile);
    mod_set(vm, fs, "cwd",      fs_cwd);
    mod_set(vm, fs, "chdir",    fs_chdir);
    mod_set(vm, fs, "glob",     fs_glob);
    sea_vm_register_module(vm, "fs", fs);
    sea_obj_release(&fs->obj);

    SeaTable *path = sea_table_new(TABLE_MAP);
    mod_set(vm, path, "join",      p_join);
    mod_set(vm, path, "normalize", p_normalize);
    mod_set(vm, path, "basename",  p_basename);
    mod_set(vm, path, "dirname",   p_dirname);
    mod_set(vm, path, "ext",       p_ext);
    mod_set(vm, path, "stem",      p_stem);
    mod_set(vm, path, "isabs",     p_isabs);
    mod_set(vm, path, "abs",       p_abs);
    mod_set(vm, path, "relative",  p_relative);
    mod_set(vm, path, "parts",     p_parts);
    tset(path, "sep", new_str("/", 1));
    sea_vm_register_module(vm, "path", path);
    sea_obj_release(&path->obj);
}
