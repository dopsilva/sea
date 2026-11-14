/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * sea_lib_process.c — the built-in `process` module: run and talk to child
 * processes without blocking the scheduler.
 *
 *   process.run(cmd [, opts]) → {code, signal, ok, stdout, stderr, timed_out} | error
 *   process.spawn(cmd [, opts]) → proc | error
 *       proc:write(data [, timeout_ms]) → bytes | error   proc:close_stdin()
 *       proc:read([max [, timeout_ms]]) → string ("" at end) | error
 *       proc:read_stderr([max [, timeout_ms]]) → string | error
 *       proc:read_line([timeout_ms]) → string | nil (end) | error
 *       proc:wait([timeout_ms]) → {code, signal, ok} | error   proc:kill([sig]) → true | error
 *   process.pid() → int    process.kill(pid [, sig]) → true | error
 *   process.signals → {HUP = 1, INT = 2, KILL = 9, TERM = 15, ...}
 *
 * cmd is an array of arguments (run directly, searched in PATH) or a string
 * (run with /bin/sh -c). Pipes are non-blocking and waited on through the
 * netpoller (sea_netpoll.h): in a goroutine, a wait parks only that goroutine.
 * process.run keeps its state across parks in vm->io_call (aux[0]).
 */

#include "sea_array.h"
#include "sea_map.h"
#include "sea_netpoll.h"
#include "sea_rope.h"
#include "sea_stdlib.h"
#include "sea_struct.h"
#include "sea_vm.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sea_compat.h"
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* ---- Helpers ---------------------------------------------------------------- */

static SeaValue new_str(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }

static void tset(SeaTable *t, const char *k, SeaValue v) {
    sea_table_set_str(t, k, v);
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
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

static int64_t ms_arg(int argc, SeaValue *args, int i) {
    if (argc <= i) return -1;
    if (sea_is_int(args[i])) return sea_as_int(args[i]) >= 0 ? sea_as_int(args[i]) : -1;
    if (sea_is_number(args[i])) return sea_as_number(args[i]) >= 0 ? (int64_t)sea_as_number(args[i]) : -1;
    return -1;
}

static SeaValue perr(SeaVM *vm, const char *fn, const char *what, int err) {
    char msg[600];
    if (what) snprintf(msg, sizeof msg, "process.%s: %s: %s", fn, what, strerror(err));
    else snprintf(msg, sizeof msg, "process.%s: %s", fn, strerror(err));
    return sea_make_error_str(vm, msg);
}

typedef struct { char *d; size_t len, cap; } Buf;

static void buf_push(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        b->d = realloc(b->d, nc);
        b->cap = nc;
    }
    memcpy(b->d + b->len, s, n);
    b->len += n;
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ---- Spawning ------------------------------------------------------------------- */

enum { IO_PIPE, IO_INHERIT, IO_NULL, IO_STDOUT };

typedef struct {
    char  **argv;        /* NULL-terminated, malloc'd strings */
    char  **envp;        /* NULL: inherit */
    char   *cwd;
    int     mode[3];     /* stdin, stdout, stderr */
} Spec;

static void spec_free(Spec *s) {
    if (s->argv) { for (char **a = s->argv; *a; a++) free(*a); free(s->argv); }
    if (s->envp) { for (char **e = s->envp; *e; e++) free(*e); free(s->envp); }
    free(s->cwd);
}

static char *cstr_of(SeaValue v) {
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return NULL;
    char *r = NULL;
    if (strlen(s->data) == s->len) r = strdup(s->data);     /* no NUL bytes inside */
    sea_obj_release(&s->obj);
    return r;
}

static int io_mode(SeaValue opts, const char *field, int def, bool allow_stdout) {
    SeaValue v;
    if (!opt_get(opts, field, &v)) return def;
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return -1;
    int m = -1;
    if (!strcmp(s->data, "pipe")) m = IO_PIPE;
    else if (!strcmp(s->data, "inherit")) m = IO_INHERIT;
    else if (!strcmp(s->data, "null")) m = IO_NULL;
    else if (allow_stdout && !strcmp(s->data, "stdout")) m = IO_STDOUT;
    sea_obj_release(&s->obj);
    return m;
}

/* Build the spec from (cmd, opts); NULL or an error message. */
static const char *spec_build(Spec *sp, SeaValue cmd, SeaValue opts, int def_stdin) {
    memset(sp, 0, sizeof *sp);
    if (sea_is_string(cmd) || sea_is_rope(cmd)) {
        char *c = cstr_of(cmd);
        if (!c) return "the command contains a NUL byte";
        sp->argv = calloc(4, sizeof *sp->argv);
        sp->argv[0] = strdup("/bin/sh");
        sp->argv[1] = strdup("-c");
        sp->argv[2] = c;
    } else if (sea_is_array(cmd) || sea_is_table(cmd)) {
        uint32_t n = sea_is_array(cmd) ? sea_array_len(SEA_AS_ARRAY(cmd)) : sea_table_count(SEA_AS_TABLE(cmd));
        if (n == 0) return "an empty command";
        sp->argv = calloc(n + 1, sizeof *sp->argv);
        for (uint32_t i = 0; i < n; i++) {
            SeaValue a;
            if (sea_is_array(cmd)) a = sea_array_get(SEA_AS_ARRAY(cmd), (int64_t)i);
            else if (!sea_table_get(SEA_AS_TABLE(cmd), sea_int_val((int64_t)i), &a)) return "the command table must hold the arguments at 0..n-1";
            char *s = NULL;
            if (sea_is_string(a) || sea_is_rope(a)) s = cstr_of(a);
            else if (sea_is_int(a) || sea_is_number(a)) {
                char t[40];
                if (sea_is_int(a)) snprintf(t, sizeof t, "%lld", (long long)sea_as_int(a));
                else snprintf(t, sizeof t, "%.17g", sea_as_number(a));
                s = strdup(t);
            }
            if (!s) return "every argument must be a string (without NUL bytes) or a number";
            sp->argv[i] = s;
        }
    } else {
        return "the command must be a string or an array of arguments";
    }
    SeaValue v;
    if (opt_get(opts, "cwd", &v)) {
        sp->cwd = cstr_of(v);
        if (!sp->cwd) return "cwd must be a string";
    }
    bool clear = opt_get(opts, "clear_env", &v) && sea_is_bool(v) && sea_as_bool(v);
    SeaValue env;
    bool has_env = opt_get(opts, "env", &env);
    if (clear || has_env) {
        /* the current environment (unless cleared), then the overrides */
        size_t cap = 16, n = 0;
        char **e = calloc(cap, sizeof *e);
        if (!clear) for (char **p = environ; *p; p++) {
            if (n + 2 > cap) { cap *= 2; e = realloc(e, cap * sizeof *e); }
            e[n++] = strdup(*p);
        }
        if (has_env && (sea_is_table(env) || sea_is_map(env) || sea_is_struct(env))) {
            uint32_t it = 0;
            SeaValue k, val;
            for (;;) {
                bool more = sea_is_table(env) ? sea_table_next(SEA_AS_TABLE(env), &it, &k, &val)
                          : sea_is_map(env) ? sea_map_next(SEA_AS_MAP(env), &it, &k, &val)
                          : sea_struct_next(SEA_AS_STRUCT(env), &it, &k, &val);
                if (!more) break;
                char *name = cstr_of(k);
                if (!name || !*name || strchr(name, '=')) { free(name); continue; }
                size_t nl = strlen(name);
                /* drop an inherited entry with the same name */
                for (size_t i = 0; i < n; i++) {
                    if (strncmp(e[i], name, nl) == 0 && e[i][nl] == '=') {
                        free(e[i]);
                        e[i] = e[--n];
                        break;
                    }
                }
                bool unset = sea_is_nil(val) || (sea_is_bool(val) && !sea_as_bool(val));
                if (!unset) {
                    char *value = NULL;
                    if (sea_is_string(val) || sea_is_rope(val)) value = cstr_of(val);
                    else {
                        char t[40] = "";
                        if (sea_is_int(val)) snprintf(t, sizeof t, "%lld", (long long)sea_as_int(val));
                        else if (sea_is_number(val)) snprintf(t, sizeof t, "%.17g", sea_as_number(val));
                        else if (sea_is_bool(val)) snprintf(t, sizeof t, "true");
                        if (t[0]) value = strdup(t);
                    }
                    if (value) {
                        if (n + 2 > cap) { cap *= 2; e = realloc(e, cap * sizeof *e); }
                        size_t len = nl + strlen(value) + 2;
                        e[n] = malloc(len);
                        snprintf(e[n], len, "%s=%s", name, value);
                        n++;
                        free(value);
                    }
                }
                free(name);
            }
        }
        e[n] = NULL;
        sp->envp = e;
    }
    sp->mode[0] = io_mode(opts, "stdin", def_stdin, false);
    sp->mode[1] = io_mode(opts, "stdout", IO_PIPE, false);
    sp->mode[2] = io_mode(opts, "stderr", IO_PIPE, true);
    if (sp->mode[0] < 0 || sp->mode[1] < 0 || sp->mode[2] < 0)
        return "stdin, stdout and stderr take \"pipe\", \"inherit\" or \"null\" (stderr also \"stdout\")";
    return NULL;
}

/* Fork and exec; fds[i] gets the parent's end of each pipe (or -1). 0, or an
 * errno (the exec's own failure is reported through a close-on-exec pipe). */
static int spawn(Spec *sp, pid_t *pid_out, int fds[3]) {
    int p[3][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 } };
    int devnull = -1, err = 0;
    fds[0] = fds[1] = fds[2] = -1;
    for (int i = 0; i < 3; i++) {
        if (sp->mode[i] == IO_PIPE && sea_pipe_cloexec(p[i]) != 0) { err = errno; goto fail; }
        if (sp->mode[i] == IO_NULL && devnull < 0 && (devnull = open("/dev/null", O_RDWR | O_CLOEXEC)) < 0) { err = errno; goto fail; }
    }
    int ep[2];
    if (sea_pipe_cloexec(ep) != 0) { err = errno; goto fail; }
    pid_t pid = fork();
    if (pid < 0) { err = errno; close(ep[0]); close(ep[1]); goto fail; }
    if (pid == 0) {
        /* child: only async-signal-safe calls until exec */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        for (int i = 0; i < 3; i++) {
            int target = i;
            int src = -1;
            if (sp->mode[i] == IO_PIPE) src = i == 0 ? p[0][0] : p[i][1];
            else if (sp->mode[i] == IO_NULL) src = devnull;
            else if (sp->mode[i] == IO_STDOUT) src = 1;
            if (src >= 0 && dup2(src, target) < 0) goto child_fail;
        }
        if (sp->cwd && chdir(sp->cwd) != 0) goto child_fail;
        if (sp->envp) {
            /* execvpe is a GNU extension: the child is about to exec, so it can
             * simply replace its own environment and use execvp */
            environ = sp->envp;
            execvp(sp->argv[0], sp->argv);
        } else execvp(sp->argv[0], sp->argv);
    child_fail: {
            int e = errno;
            ssize_t w = write(ep[1], &e, sizeof e);
            (void)w;
            _exit(127);
        }
    }
    close(ep[1]);
    for (int i = 0; i < 3; i++) {
        if (sp->mode[i] != IO_PIPE) continue;
        if (i == 0) { close(p[0][0]); fds[0] = p[0][1]; }
        else { close(p[i][1]); fds[i] = p[i][0]; }
        p[i][0] = p[i][1] = -1;
        set_nonblock(fds[i]);
    }
    if (devnull >= 0) close(devnull);
    int child_err = 0;
    ssize_t r;
    do r = read(ep[0], &child_err, sizeof child_err); while (r < 0 && errno == EINTR);
    close(ep[0]);
    if (r == (ssize_t)sizeof child_err) {
        int st;
        waitpid(pid, &st, 0);
        for (int i = 0; i < 3; i++) if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; }
        return child_err;
    }
    *pid_out = pid;
    return 0;
fail:
    for (int i = 0; i < 3; i++) { if (p[i][0] >= 0) close(p[i][0]); if (p[i][1] >= 0) close(p[i][1]); }
    if (devnull >= 0) close(devnull);
    return err;
}

static void status_fields(SeaTable *t, int status) {
    if (WIFEXITED(status)) {
        tset(t, "code", sea_int_val(WEXITSTATUS(status)));
        tset(t, "ok", sea_bool_val(WEXITSTATUS(status) == 0));
    } else {
        if (WIFSIGNALED(status)) tset(t, "signal", sea_int_val(WTERMSIG(status)));
        tset(t, "ok", SEA_FALSE_VAL);
    }
}

/* ======================================================================== */
/* process.run                                                              */
/* ======================================================================== */

typedef struct {
    pid_t  pid;
    int    in, out, err, ep, pidfd;
    bool   armed;
    Buf    o, e;
    char  *input;
    size_t in_len, in_off;
    bool   exited, timed_out;
    int    status;
} Run;

static void run_free(Run *r) {
    if (r->in >= 0) close(r->in);
    if (r->out >= 0) close(r->out);
    if (r->err >= 0) close(r->err);
    if (r->pidfd >= 0) sea_mux_del_pid(r->ep, r->pidfd, r->pid);
    if (r->ep >= 0) sea_netpoll_close(r->ep);
    free(r->o.d);
    free(r->e.d);
    free(r->input);
    free(r);
}

/* Read what is available from *fd into b; close it at end of stream. */
static void drain(Run *r, int *fd, Buf *b) {
    char chunk[65536];
    while (*fd >= 0) {
        ssize_t n = read(*fd, chunk, sizeof chunk);
        if (n > 0) { buf_push(b, chunk, (size_t)n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        sea_mux_del(r->ep, *fd);
        close(*fd);
        *fd = -1;
    }
}

/* Put what process.run waits for into its multiplexer. With epoll (Linux) the
 * interest is level-triggered and stays registered, so this only has to run
 * once. With kqueue the multiplexer is itself waited on as a file descriptor
 * but nobody reads its events, so each interest is one-shot and is registered
 * again on every turn of the loop (a registration that finds the fd already
 * ready fires at once, so nothing is lost); events left over from the last
 * turn are discarded first. */
static void run_arm(Run *r) {
    if (r->ep < 0) return;
#ifdef SEA_MUX_KQUEUE
    int stale[16];
    while (sea_mux_wait(r->ep, stale, 16, 0) > 0) {}
    bool oneshot = true;
#else
    if (r->armed) return;
    r->armed = true;
    bool oneshot = false;
#endif
    if (r->out >= 0) sea_mux_add(r->ep, r->out, SEA_MUX_READ, oneshot);
    if (r->err >= 0) sea_mux_add(r->ep, r->err, SEA_MUX_READ, oneshot);
    if (r->in >= 0) sea_mux_add(r->ep, r->in, SEA_MUX_WRITE, oneshot);
    if (!r->exited) {
        if (r->pidfd < 0 || oneshot) {
            int h = sea_mux_add_pid(r->ep, r->pid);
            if (h >= 0) r->pidfd = h;
        }
    }
}

/* Push input into the child's stdin; close it when done (or the child left). */
static void feed(Run *r) {
    while (r->in >= 0 && r->in_off < r->in_len) {
        ssize_t n = write(r->in, r->input + r->in_off, r->in_len - r->in_off);
        if (n > 0) { r->in_off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        break;                                     /* EPIPE: the child does not read */
    }
    if (r->in >= 0) {
        sea_mux_del(r->ep, r->in);
        close(r->in);
        r->in = -1;
    }
}

static void reap(Run *r, bool block) {
    if (r->exited) return;
    int st;
    pid_t w;
    do w = waitpid(r->pid, &st, block ? 0 : WNOHANG); while (w < 0 && errno == EINTR);
    if (w == r->pid) {
        r->exited = true;
        r->status = st;
        if (r->pidfd >= 0) {
            sea_mux_del_pid(r->ep, r->pidfd, r->pid);
            r->pidfd = -1;
        }
    }
}

static SeaValue run_result(SeaVM *vm, Run *r) {
    SeaTable *t = sea_table_new(TABLE_MAP);
    status_fields(t, r->status);
    tset(t, "stdout", new_str(r->o.d ? r->o.d : "", r->o.len));
    tset(t, "stderr", new_str(r->e.d ? r->e.d : "", r->e.len));
    tset(t, "timed_out", sea_bool_val(r->timed_out));
    tset(t, "pid", sea_int_val(r->pid));
    (void)vm;
    return sea_obj_val(&t->obj);
}

/* process.run(cmd [, opts]): run to completion, collecting stdout and stderr.
 * opts: cwd, env (table of overrides; false removes), clear_env, stdin
 * (a string to feed, or "inherit"/"null"), stdout/stderr ("pipe", "inherit",
 * "null"; stderr also "stdout"), timeout (ms: then the child is killed) */
static SeaValue proc_run(SeaVM *vm, int argc, SeaValue *args) {
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    SeaIoCall *io = vm->io_call.active ? &vm->io_call : NULL;
    Run *r;
    if (!io || io->progress == 0) {
        /* first attempt: build and start everything before the first wait */
        if (argc < 1) return sea_make_error_str(vm, "process.run: command expected");
        SeaValue to;
        int64_t timeout = -1;
        if (opt_get(opts, "timeout", &to)) {
            if (sea_is_int(to)) timeout = sea_as_int(to);
            else if (sea_is_number(to)) timeout = (int64_t)sea_as_number(to);
        }
        SeaValue input = SEA_NIL_VAL;
        bool feed_input = false;
        Spec sp;
        SeaValue sv;
        int def_stdin = IO_NULL;
        if (opt_get(opts, "stdin", &sv) && (sea_is_string(sv) || sea_is_rope(sv))) {
            SeaString *s = sea_value_get_flat_string(sv);
            bool is_mode = !strcmp(s->data, "inherit") || !strcmp(s->data, "null") || !strcmp(s->data, "pipe");
            sea_obj_release(&s->obj);
            if (!is_mode) { input = sv; feed_input = true; def_stdin = IO_PIPE; }
        }
        SeaValue opts2 = opts;
        const char *bad;
        if (feed_input) {
            /* "stdin" holds data: build the spec with a pipe for it */
            SeaTable *copy = sea_table_new(TABLE_MAP);
            const char *keys[] = { "cwd", "env", "clear_env", "stdout", "stderr" };
            for (int i = 0; i < 5; i++) {
                SeaValue v;
                if (opt_get(opts, keys[i], &v)) sea_table_set_str(copy, keys[i], v);
            }
            bad = spec_build(&sp, args[0], sea_obj_val(&copy->obj), IO_PIPE);
            sea_obj_release(&copy->obj);
        } else {
            bad = spec_build(&sp, args[0], opts2, def_stdin);
        }
        if (bad) {
            spec_free(&sp);
            char msg[200];
            snprintf(msg, sizeof msg, "process.run: %s", bad);
            return sea_make_error_str(vm, msg);
        }
        r = calloc(1, sizeof *r);
        r->in = r->out = r->err = r->ep = r->pidfd = -1;
        if (feed_input) {
            SeaString *s = sea_value_get_flat_string(input);
            r->input = malloc(s->len + 1);
            memcpy(r->input, s->data, s->len);
            r->in_len = s->len;
            sea_obj_release(&s->obj);
        }
        int fds[3];
        int e = spawn(&sp, &r->pid, fds);
        if (e) {
            SeaValue err = perr(vm, "run", sp.argv[0], e);
            spec_free(&sp);
            run_free(r);
            return err;
        }
        spec_free(&sp);
        r->in = fds[0];
        r->out = fds[1];
        r->err = fds[2];
        r->ep = sea_mux_create();
        run_arm(r);
        io = sea_io_begin(vm, timeout);
        io->aux[0] = (int64_t)(intptr_t)r;
        io->progress = 1;
    } else {
        io = sea_io_begin(vm, -1);                 /* resume: the deadline is kept */
        r = (Run *)(intptr_t)io->aux[0];
    }
    for (;;) {
        run_arm(r);
        feed(r);
        drain(r, &r->out, &r->o);
        drain(r, &r->err, &r->e);
        reap(r, false);
        if (r->out < 0 && r->err < 0) {
            if (!r->exited && r->pidfd < 0) reap(r, true);   /* no pidfd: the child is closing down */
            if (r->exited) break;
        }
        int w = sea_io_wait(vm, r->ep, SEA_IO_READ);
        if (w == SEA_IO_PARK) return SEA_PARK_VAL;
        if (w == SEA_IO_TIMEOUT) {
            kill(r->pid, SIGKILL);
            r->timed_out = true;
            reap(r, true);
            drain(r, &r->out, &r->o);
            drain(r, &r->err, &r->e);
            break;
        }
        if (w == SEA_IO_CLOSED) break;
    }
    if (!r->exited) reap(r, true);
    sea_io_end(vm);
    SeaValue res = run_result(vm, r);
    run_free(r);
    return res;
}

/* ======================================================================== */
/* process.spawn and its handle                                             */
/* ======================================================================== */

static bool field_int(SeaValue t, const char *k, int64_t *out) {
    SeaValue v;
    if (!sea_is_table(t) || !sea_table_get_str(SEA_AS_TABLE(t), k, &v) || !sea_is_int(v)) return false;
    *out = sea_as_int(v);
    return true;
}

static int handle_fd(SeaVM *vm, const char *fn, int argc, SeaValue *args, const char *which) {
    int64_t fd;
    if (argc < 1 || !field_int(args[0], which, &fd) || fd < 0) return -1;
    (void)vm; (void)fn;
    return (int)fd;
}

static void set_field(SeaValue t, const char *k, SeaValue v) {
    if (sea_is_table(t)) tset(SEA_AS_TABLE(t), k, v);
}

/* Read up to max bytes from fd (waiting through the netpoller). */
static SeaValue read_fd(SeaVM *vm, const char *fn, int fd, int64_t max, int64_t timeout) {
    if (fd < 0) return sea_make_error_str(vm, "process.read: that stream is not a pipe (or is closed)");
    SeaIoCall *io = sea_io_begin(vm, timeout);
    if (io->closed) { sea_io_end(vm); return perr(vm, fn, NULL, EBADF); }
    char *buf = malloc((size_t)max);
    for (;;) {
        ssize_t n = read(fd, buf, (size_t)max);
        if (n >= 0) {
            sea_io_end(vm);
            SeaValue v = new_str(buf, (size_t)n);
            free(buf);
            return v;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            int e = errno;
            free(buf);
            sea_io_end(vm);
            return perr(vm, fn, NULL, e);
        }
        int w = sea_io_wait(vm, fd, SEA_IO_READ);
        if (w == SEA_IO_PARK) { free(buf); return SEA_PARK_VAL; }
        if (w != SEA_IO_READY) {
            free(buf);
            sea_io_end(vm);
            return w == SEA_IO_TIMEOUT ? sea_make_error_str(vm, "timeout") : perr(vm, fn, NULL, EBADF);
        }
    }
}

static int64_t max_arg(int argc, SeaValue *args, int i) {
    if (argc > i && sea_is_int(args[i]) && sea_as_int(args[i]) > 0) {
        int64_t m = sea_as_int(args[i]);
        return m > (1 << 24) ? (1 << 24) : m;
    }
    return 65536;
}

static SeaValue h_read(SeaVM *vm, int argc, SeaValue *args) {
    return read_fd(vm, "read", handle_fd(vm, "read", argc, args, "stdout"), max_arg(argc, args, 1), ms_arg(argc, args, 2));
}

static SeaValue h_read_stderr(SeaVM *vm, int argc, SeaValue *args) {
    return read_fd(vm, "read_stderr", handle_fd(vm, "read_stderr", argc, args, "stderr"), max_arg(argc, args, 1), ms_arg(argc, args, 2));
}

/* proc:read_line([timeout]): the next line of stdout without its "\n"; nil
 * at the end. Bytes read past the line are kept in the handle (__rest). */
static SeaValue h_read_line(SeaVM *vm, int argc, SeaValue *args) {
    int fd = handle_fd(vm, "read_line", argc, args, "stdout");
    SeaValue rest = SEA_NIL_VAL;
    if (argc > 0 && sea_is_table(args[0])) sea_table_get_str(SEA_AS_TABLE(args[0]), "__rest", &rest);
    SeaString *rs = sea_is_string(rest) ? SEA_AS_STRING(rest) : NULL;
    if (rs) {
        const char *nl = memchr(rs->data, '\n', rs->len);
        if (nl) {
            size_t k = (size_t)(nl - rs->data);
            SeaValue line = new_str(rs->data, k > 0 && rs->data[k - 1] == '\r' ? k - 1 : k);
            set_field(args[0], "__rest", new_str(nl + 1, rs->len - k - 1));
            return line;
        }
    }
    SeaValue chunk = read_fd(vm, "read_line", fd, 65536, ms_arg(argc, args, 1));
    if (chunk == SEA_PARK_VAL || sea_is_error(chunk)) return chunk;
    SeaString *c = SEA_AS_STRING(chunk);
    if (c->len == 0) {                                   /* end of stream */
        sea_obj_release(&c->obj);
        if (rs && rs->len) {
            SeaValue line = sea_val_ref(rest);
            set_field(args[0], "__rest", new_str("", 0));
            return line;
        }
        return SEA_NIL_VAL;
    }
    /* append and retry: the line may now be complete */
    size_t old = rs ? rs->len : 0;
    SeaString *joined = sea_string_alloc(old + c->len);
    if (old) memcpy(joined->data, rs->data, old);
    memcpy(joined->data + old, c->data, c->len);
    joined->data[old + c->len] = '\0';
    sea_obj_release(&c->obj);
    set_field(args[0], "__rest", sea_obj_val(&joined->obj));
    return h_read_line(vm, argc, args);
}

static SeaValue h_write(SeaVM *vm, int argc, SeaValue *args) {
    int fd = handle_fd(vm, "write", argc, args, "stdin");
    if (fd < 0) return sea_make_error_str(vm, "process.write: stdin is not a pipe (or is closed)");
    SeaString *data = argc > 1 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!data) sea_vm_error_noreturn(vm, "process.write: data (string) expected");
    SeaIoCall *io = sea_io_begin(vm, ms_arg(argc, args, 2));
    if (io->closed) { sea_obj_release(&data->obj); sea_io_end(vm); return perr(vm, "write", NULL, EBADF); }
    while ((size_t)io->progress < data->len) {
        ssize_t n = write(fd, data->data + io->progress, data->len - (size_t)io->progress);
        if (n > 0) { io->progress += n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int w = sea_io_wait(vm, fd, SEA_IO_WRITE);
            if (w == SEA_IO_PARK) { sea_obj_release(&data->obj); return SEA_PARK_VAL; }
            if (w == SEA_IO_READY) continue;
            sea_obj_release(&data->obj);
            sea_io_end(vm);
            return w == SEA_IO_TIMEOUT ? sea_make_error_str(vm, "timeout") : perr(vm, "write", NULL, EBADF);
        }
        int e = errno;
        sea_obj_release(&data->obj);
        sea_io_end(vm);
        return perr(vm, "write", NULL, e);
    }
    int64_t written = io->progress;
    sea_obj_release(&data->obj);
    sea_io_end(vm);
    return sea_int_val(written);
}

static void close_field(SeaValue t, const char *k) {
    int64_t fd;
    if (field_int(t, k, &fd) && fd >= 0) {
        sea_netpoll_close((int)fd);
        set_field(t, k, sea_int_val(-1));
    }
}

static SeaValue h_close_stdin(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc > 0) close_field(args[0], "stdin");
    return SEA_TRUE_VAL;
}

static int sig_arg(SeaVM *vm, const char *fn, int argc, SeaValue *args, int i) {
    if (argc <= i || sea_is_nil(args[i])) return SIGTERM;
    if (sea_is_int(args[i])) return (int)sea_as_int(args[i]);
    sea_vm_error_noreturn(vm, "process.%s: signal number expected (see process.signals)", fn);
    return SIGTERM;
}

static SeaValue h_kill(SeaVM *vm, int argc, SeaValue *args) {
    int64_t pid;
    if (argc < 1 || !field_int(args[0], "pid", &pid)) return sea_make_error_str(vm, "process.kill: not a process handle");
    int64_t done;
    if (field_int(args[0], "__done", &done) && done) return perr(vm, "kill", NULL, ESRCH);
    int sig = sig_arg(vm, "kill", argc, args, 1);
    return kill((pid_t)pid, sig) == 0 ? SEA_TRUE_VAL : perr(vm, "kill", NULL, errno);
}

/* proc:wait([timeout]): the exit status; closes the handle's pipes.
 * Waits through a pidfd on the netpoller. */
static SeaValue h_wait(SeaVM *vm, int argc, SeaValue *args) {
    int64_t pid;
    if (argc < 1 || !field_int(args[0], "pid", &pid)) return sea_make_error_str(vm, "process.wait: not a process handle");
    SeaValue cached;
    if (sea_table_get_str(SEA_AS_TABLE(args[0]), "__status", &cached) && sea_is_table(cached)) return sea_val_ref(cached);
    SeaIoCall *io = sea_io_begin(vm, ms_arg(argc, args, 1));
    int pidfd = (int)io->aux[1] - 1;
    if (io->progress == 0) {
        pidfd = sea_pid_waitfd((pid_t)pid);
        io->aux[1] = pidfd + 1;
        io->progress = 1;
    }
    int st = 0;
    for (;;) {
        pid_t w = waitpid((pid_t)pid, &st, pidfd >= 0 ? WNOHANG : 0);
        if (w == (pid_t)pid) break;
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) {
            int e = errno;
            if (pidfd >= 0) sea_netpoll_close(pidfd);
            sea_io_end(vm);
            return perr(vm, "wait", NULL, e);
        }
        int r = sea_io_wait(vm, pidfd, SEA_IO_READ);
        if (r == SEA_IO_PARK) return SEA_PARK_VAL;
        if (r != SEA_IO_READY) {
            if (pidfd >= 0) sea_netpoll_close(pidfd);
            sea_io_end(vm);
            return r == SEA_IO_TIMEOUT ? sea_make_error_str(vm, "timeout") : perr(vm, "wait", NULL, EBADF);
        }
    }
    if (pidfd >= 0) sea_netpoll_close(pidfd);
    sea_io_end(vm);
    SeaTable *t = sea_table_new(TABLE_MAP);
    status_fields(t, st);
    SeaValue res = sea_obj_val(&t->obj);
    sea_table_set_str(SEA_AS_TABLE(args[0]), "__status", res);
    set_field(args[0], "__done", sea_int_val(1));
    close_field(args[0], "stdin");
    close_field(args[0], "stdout");
    close_field(args[0], "stderr");
    return res;
}

/* process.spawn(cmd [, opts]) → handle with pid, stdin/stdout/stderr fds
 * (-1 when not a pipe) and the methods above */
static SeaValue proc_spawn(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) return sea_make_error_str(vm, "process.spawn: command expected");
    Spec sp;
    const char *bad = spec_build(&sp, args[0], argc > 1 ? args[1] : SEA_NIL_VAL, IO_PIPE);
    if (bad) {
        spec_free(&sp);
        char msg[200];
        snprintf(msg, sizeof msg, "process.spawn: %s", bad);
        return sea_make_error_str(vm, msg);
    }
    pid_t pid;
    int fds[3];
    int e = spawn(&sp, &pid, fds);
    if (e) {
        SeaValue err = perr(vm, "spawn", sp.argv[0], e);
        spec_free(&sp);
        return err;
    }
    spec_free(&sp);
    SeaTable *t = sea_table_new(TABLE_MAP);
    tset(t, "pid", sea_int_val(pid));
    tset(t, "stdin", sea_int_val(fds[0]));
    tset(t, "stdout", sea_int_val(fds[1]));
    tset(t, "stderr", sea_int_val(fds[2]));
    mod_set(vm, t, "write",       h_write);
    mod_set(vm, t, "close_stdin", h_close_stdin);
    mod_set(vm, t, "read",        h_read);
    mod_set(vm, t, "read_stderr", h_read_stderr);
    mod_set(vm, t, "read_line",   h_read_line);
    mod_set(vm, t, "wait",        h_wait);
    mod_set(vm, t, "kill",        h_kill);
    return sea_obj_val(&t->obj);
}

/* ---- module-level -------------------------------------------------------------- */

static SeaValue proc_pid(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    return sea_int_val(getpid());
}

static SeaValue proc_kill(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_int(args[0])) sea_vm_error_noreturn(vm, "process.kill: pid (int) expected");
    int sig = sig_arg(vm, "kill", argc, args, 1);
    return kill((pid_t)sea_as_int(args[0]), sig) == 0 ? SEA_TRUE_VAL : perr(vm, "kill", NULL, errno);
}

void sea_lib_process_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);
    mod_set(vm, mod, "run",   proc_run);
    mod_set(vm, mod, "spawn", proc_spawn);
    mod_set(vm, mod, "pid",   proc_pid);
    mod_set(vm, mod, "kill",  proc_kill);
    SeaTable *sigs = sea_table_new(TABLE_MAP);
    static const struct { const char *name; int sig; } S[] = {
        { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "KILL", SIGKILL }, { "USR1", SIGUSR1 },
        { "USR2", SIGUSR2 }, { "PIPE", SIGPIPE }, { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "CHLD", SIGCHLD },
        { "CONT", SIGCONT }, { "STOP", SIGSTOP }, { "TSTP", SIGTSTP },
    };
    for (size_t i = 0; i < sizeof S / sizeof *S; i++) tset(sigs, S[i].name, sea_int_val(S[i].sig));
    tset(mod, "signals", sea_obj_val(&sigs->obj));
    sea_vm_register_module(vm, "process", mod);
    sea_obj_release(&mod->obj);
}
