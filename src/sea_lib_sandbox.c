/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * sea_lib_sandbox.c — the built-in `sandbox` module: run untrusted Sea code in
 * a VM of its own, inside the host program.
 *
 *   local sb = sandbox.new({budget = 1000000, output_limit = 16384})
 *   sb:load(source, "mod.sea")        -- run a chunk; true, or an error value
 *   sb:call("name", a, b)             -- call a global function; its result, or an error value
 *   sb:set("name", value)             -- a global data value
 *   sb:expose("name", fn)             -- what the script can call as host("name", ...)
 *   sb:output()                       -- what the script printed (and clear it)
 *   sb:close()
 *
 * What the script gets (nothing else exists in its VM):
 *   print (collected, capped), type, tostring, tonumber, error, is_error, assert,
 *   len, panic, recover, load, int_array/float_array, math, string, sort, utf8,
 *   list, and `host(name, ...)` for the functions the host exposed.
 * No require, io, os, fs, net, process, time, sync, hash, re, bigint, gc, no
 * goroutines and no channels. The script cannot reach a file, a socket or another
 * program, because the functions that do so are not in its VM at all (and `load`
 * only compiles text into the same restricted VM).
 *
 * Every `load`/`call` gets a fresh budget of steps (a step is a loop iteration or a
 * Sea function call). When it runs out the call stops with an error that the
 * script cannot recover from, and the sandbox is usable again afterwards.
 *
 * Values cross the boundary by copy: nil, booleans, numbers, strings, arrays,
 * tables, maps and structs (as tables), errors (as their message); at most 32
 * levels deep and 100000 values. A function, channel or typed array cannot cross.
 *
 * Limits of this first version: there is no memory cap (a script can still build
 * a huge string or table inside a single native call, which the budget does not
 * count), and the native functions of the allowed libraries run to completion.
 */

#include "sea_array.h"
#include "sea_map.h"
#include "sea_rope.h"
#include "sea_stdlib.h"
#include "sea_struct.h"
#include "sea_vm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SANDBOXES 256
#define COPY_MAX_DEPTH 32
#define COPY_MAX_NODES 100000
#define DEFAULT_BUDGET 1000000
#define DEFAULT_OUTPUT_LIMIT 16384

typedef struct {
    SeaVM    *vm;           /* the script's VM */
    SeaVM    *host;         /* the VM that called us last (for `host(...)`) */
    int64_t   budget;       /* steps per call */
    char     *out;
    size_t    out_len, out_cap, out_max;
    SeaTable *exposed;      /* name -> host function */
    int       depth;        /* nested calls in progress (host fn calling back is refused) */
} Sandbox;

static Sandbox *g_sb[MAX_SANDBOXES];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---- copying values between VMs ------------------------------------------- */

typedef struct {
    int    nodes;
    char   err[160];
    SeaVM *to;           /* the VM the copy is for (errors are made there) */
} Copier;

static SeaValue new_str(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }

static bool copy_value(Copier *c, SeaValue v, int depth, SeaValue *out);

static bool copy_fail(Copier *c, const char *what, SeaValue v) {
    if (!c->err[0]) snprintf(c->err, sizeof c->err, "%s (%s)", what, sea_type_name(v));
    return false;
}

static bool copy_key(Copier *c, SeaValue k, SeaValue *out) {
    if (sea_is_int(k) || sea_is_string(k) || sea_is_bool(k) || sea_is_number(k)) return copy_value(c, k, 1, out);
    return copy_fail(c, "chave que nao pode atravessar", k);
}

static bool copy_value(Copier *c, SeaValue v, int depth, SeaValue *out) {
    if (depth > COPY_MAX_DEPTH) return copy_fail(c, "valor fundo demais", v);
    if (++c->nodes > COPY_MAX_NODES) return copy_fail(c, "valor grande demais", v);
    if (sea_is_nil(v)) { *out = SEA_NIL_VAL; return true; }
    if (sea_is_bool(v)) { *out = sea_bool_val(sea_as_bool(v)); return true; }
    if (sea_is_int(v)) { *out = sea_int_val(sea_as_int(v)); return true; }
    if (sea_is_number(v)) { *out = sea_number_val(sea_as_number(v)); return true; }
    SeaString *flat = sea_value_get_flat_string(v);
    if (flat) {
        *out = new_str(flat->data, flat->len);
        sea_obj_release(&flat->obj);
        return true;
    }
    if (sea_is_error(v)) {
        SeaString *m = sea_value_get_flat_string(SEA_AS_ERROR(v)->message);
        *out = sea_make_error_str(c->to, m ? m->data : "error");
        if (m) sea_obj_release(&m->obj);
        return true;
    }
    if (sea_is_array(v)) {
        SeaArray *src = SEA_AS_ARRAY(v);
        SeaArray *dst = sea_array_new(sea_array_len(src));
        uint32_t n = sea_array_len(src);
        for (uint32_t i = 0; i < n; i++) {
            SeaValue e;
            if (!copy_value(c, sea_array_get(src, i), depth + 1, &e)) {
                sea_obj_release(&dst->obj);
                return false;
            }
            sea_array_push_move(dst, e);
        }
        *out = sea_obj_val(&dst->obj);
        return true;
    }
    if (sea_is_table(v)) {
        SeaTable *src = SEA_AS_TABLE(v);
        SeaTable *dst = sea_table_new(TABLE_MAP);
        uint32_t it = 0;
        SeaValue k, val;
        while (sea_table_next(src, &it, &k, &val)) {
            SeaValue ck, cv;
            if (!copy_key(c, k, &ck)) { sea_obj_release(&dst->obj); return false; }
            if (!copy_value(c, val, depth + 1, &cv)) {
                if (sea_is_obj(ck)) sea_obj_release(sea_as_obj(ck));
                sea_obj_release(&dst->obj);
                return false;
            }
            sea_table_set_move(dst, ck, cv);
            if (sea_is_obj(ck)) sea_obj_release(sea_as_obj(ck));
        }
        *out = sea_obj_val(&dst->obj);
        return true;
    }
    if (sea_is_map(v)) {
        SeaMap *src = SEA_AS_MAP(v);
        SeaMap *dst = sea_map_new();
        uint32_t it = 0;
        SeaValue k, val;
        while (sea_map_next(src, &it, &k, &val)) {
            SeaValue ck, cv;
            if (!copy_key(c, k, &ck)) { sea_obj_release(&dst->obj); return false; }
            if (!copy_value(c, val, depth + 1, &cv)) {
                if (sea_is_obj(ck)) sea_obj_release(sea_as_obj(ck));
                sea_obj_release(&dst->obj);
                return false;
            }
            sea_map_set(dst, ck, cv);
            if (sea_is_obj(ck)) sea_obj_release(sea_as_obj(ck));
            if (sea_is_obj(cv)) sea_obj_release(sea_as_obj(cv));
        }
        *out = sea_obj_val(&dst->obj);
        return true;
    }
    if (sea_is_struct(v)) {            /* a struct crosses as a table */
        SeaStruct *src = SEA_AS_STRUCT(v);
        SeaTable *dst = sea_table_new(TABLE_MAP);
        uint32_t it = 0;
        SeaValue k, val;
        while (sea_struct_next(src, &it, &k, &val)) {
            SeaValue ck, cv;
            if (!copy_key(c, k, &ck)) { sea_obj_release(&dst->obj); return false; }
            if (!copy_value(c, val, depth + 1, &cv)) {
                if (sea_is_obj(ck)) sea_obj_release(sea_as_obj(ck));
                sea_obj_release(&dst->obj);
                return false;
            }
            sea_table_set_move(dst, ck, cv);
            if (sea_is_obj(ck)) sea_obj_release(sea_as_obj(ck));
        }
        *out = sea_obj_val(&dst->obj);
        return true;
    }
    return copy_fail(c, "valor que nao pode atravessar", v);
}

/* ---- registry --------------------------------------------------------------- */

static Sandbox *sb_by_vm(SeaVM *vm) {
    Sandbox *r = NULL;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_SANDBOXES; i++)
        if (g_sb[i] && g_sb[i]->vm == vm) { r = g_sb[i]; break; }
    pthread_mutex_unlock(&g_mu);
    return r;
}

static Sandbox *sb_arg(SeaVM *vm, int argc, SeaValue *args, int *id_out) {
    if (argc < 1 || !sea_is_table(args[0])) sea_vm_error_noreturn(vm, "sandbox: method called without the sandbox (use sb:method())");
    SeaValue idv;
    if (!sea_table_get_str(SEA_AS_TABLE(args[0]), "id", &idv) || !sea_is_int(idv))
        sea_vm_error_noreturn(vm, "sandbox: not a sandbox");
    int id = (int)sea_as_int(idv);
    Sandbox *sb = NULL;
    pthread_mutex_lock(&g_mu);
    if (id >= 0 && id < MAX_SANDBOXES) sb = g_sb[id];
    pthread_mutex_unlock(&g_mu);
    if (!sb) sea_vm_error_noreturn(vm, "sandbox: closed");
    if (id_out) *id_out = id;
    return sb;
}

/* ---- the script's globals ---------------------------------------------------- */

static void out_append(Sandbox *sb, const char *s, size_t n) {
    if (sb->out_len >= sb->out_max) return;
    if (n > sb->out_max - sb->out_len) n = sb->out_max - sb->out_len;
    if (sb->out_len + n + 1 > sb->out_cap) {
        size_t nc = sb->out_cap ? sb->out_cap * 2 : 256;
        while (nc < sb->out_len + n + 1) nc *= 2;
        sb->out = realloc(sb->out, nc);
        sb->out_cap = nc;
    }
    memcpy(sb->out + sb->out_len, s, n);
    sb->out_len += n;
    sb->out[sb->out_len] = 0;
}

static SeaValue sbx_print(SeaVM *vm, int argc, SeaValue *args) {
    Sandbox *sb = sb_by_vm(vm);
    if (!sb) return SEA_NIL_VAL;
    for (int i = 0; i < argc; i++) {
        if (i) out_append(sb, "\t", 1);
        SeaString *s = sea_vm_tostring(vm, args[i]);
        out_append(sb, s->data, s->len);
        sea_obj_release(&s->obj);
    }
    out_append(sb, "\n", 1);
    return SEA_NIL_VAL;
}

/* host(name, ...): call a function the host exposed, arguments and result by copy. */
static SeaValue sbx_host(SeaVM *vm, int argc, SeaValue *args) {
    Sandbox *sb = sb_by_vm(vm);
    SeaString *name = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!sb || !name) sea_vm_error_noreturn(vm, "host: expected the name of a function");
    SeaValue fn;
    bool found = sb->exposed && sea_table_get_str(sb->exposed, name->data, &fn);
    if (!found) {
        char msg[200];
        snprintf(msg, sizeof msg, "host: no function '%.100s' is exposed", name->data);
        sea_obj_release(&name->obj);
        sea_vm_error_noreturn(vm, "%s", msg);
    }
    sea_obj_release(&name->obj);
    int n = argc - 1;
    SeaValue hargs[16];
    if (n > 16) sea_vm_error_noreturn(vm, "host: too many arguments");
    Copier c = {0, {0}, sb->host};
    for (int i = 0; i < n; i++) {
        if (!copy_value(&c, args[i + 1], 1, &hargs[i])) {
            for (int j = 0; j < i; j++) if (sea_is_obj(hargs[j])) sea_obj_release(sea_as_obj(hargs[j]));
            sea_vm_error_noreturn(vm, "host: argument %d: %s", i + 1, c.err);
        }
    }
    SeaValue res = sea_vm_call(sb->host, fn, hargs, n);
    for (int i = 0; i < n; i++) if (sea_is_obj(hargs[i])) sea_obj_release(sea_as_obj(hargs[i]));
    if (sb->host->panic_propagating || sb->host->panicking)
        sea_vm_error_noreturn(vm, "host: the host function failed");
    SeaValue out;
    Copier c2 = {0, {0}, vm};
    bool ok = copy_value(&c2, res, 1, &out);
    if (sea_is_obj(res)) sea_obj_release(sea_as_obj(res));
    if (!ok) sea_vm_error_noreturn(vm, "host: result: %s", c2.err);
    return out;
}

static void sandbox_populate(SeaVM *vm) {
    /* only what a script may use; see the header comment */
    sea_vm_register_native(vm, "print",    sbx_print);
    sea_lib_core_register_safe(vm);
    sea_lib_math_register(vm);
    sea_lib_string_register(vm);
    sea_lib_sort_register(vm);
    sea_lib_utf8_register(vm);
    sea_lib_list_register(vm);
    sea_vm_register_native(vm, "host", sbx_host);
}

/* ---- running ------------------------------------------------------------------ */

/* Take the panic out of the sandbox VM as a message, and make it usable again. */
static void take_panic(SeaVM *vm, char *msg, size_t cap) {
    snprintf(msg, cap, "panic");
    if (vm->panicking) {
        SeaValue v = vm->panic_val;
        if (sea_is_error(v)) {
            SeaString *s = sea_value_get_flat_string(SEA_AS_ERROR(v)->message);
            if (s) { snprintf(msg, cap, "%s", s->data); sea_obj_release(&s->obj); }
        } else {
            SeaString *s = sea_vm_tostring(vm, v);
            snprintf(msg, cap, "%s", s->data);
            sea_obj_release(&s->obj);
        }
        if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
    }
    vm->panicking = false;
    vm->panic_propagating = false;
    vm->panic_val = SEA_NIL_VAL;
    vm->killed = false;
    free(vm->panic_trace);
    vm->panic_trace = NULL;
}

/* Call `fn` in the sandbox with a fresh budget. Returns true with *res owned (in the
 * sandbox's heap); false with the message in msg. */
static bool run_guarded(Sandbox *sb, SeaVM *host, SeaValue fn, SeaValue *args, int argc,
                        SeaValue *res, char *msg, size_t cap) {
    SeaVM *vm = sb->vm;
    int frames = vm->frame_count, defers = vm->defer_count;
    sb->host = host;
    sb->depth++;
    vm->budget = sb->budget;
    SeaValue r = sea_vm_call(vm, fn, args, argc);
    sb->depth--;
    if (vm->panicking || vm->panic_propagating) {
        take_panic(vm, msg, cap);
        vm->frame_count = frames;
        vm->defer_count = defers;
        return false;
    }
    *res = r;
    return true;
}

static SeaValue to_host(SeaVM *host, SeaValue v, bool *ok, char *msg, size_t cap) {
    Copier c = {0, {0}, host};
    SeaValue out;
    *ok = copy_value(&c, v, 1, &out);
    if (!*ok) snprintf(msg, cap, "%s", c.err);
    return *ok ? out : SEA_NIL_VAL;
    (void)host;
}

/* ---- methods -------------------------------------------------------------------- */

static SeaValue m_load(SeaVM *vm, int argc, SeaValue *args) {
    Sandbox *sb = sb_arg(vm, argc, args, NULL);
    SeaString *src = argc > 1 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!src) sea_vm_error_noreturn(vm, "sandbox:load: expected the source text");
    SeaString *nm = argc > 2 ? sea_value_get_flat_string(args[2]) : NULL;
    SeaValue loadfn;
    if (!sea_globals_get_str(sb->vm, "load", &loadfn)) sea_vm_error_noreturn(vm, "sandbox: no load");
    SeaValue largs[2] = { new_str(src->data, src->len), nm ? new_str(nm->data, nm->len) : SEA_NIL_VAL };
    sea_obj_release(&src->obj);
    if (nm) sea_obj_release(&nm->obj);
    SeaValue chunk = sea_vm_call(sb->vm, loadfn, largs, 2);
    sea_obj_release(sea_as_obj(largs[0]));
    if (sea_is_obj(largs[1])) sea_obj_release(sea_as_obj(largs[1]));
    if (sea_is_error(chunk)) {
        SeaString *m = sea_value_get_flat_string(SEA_AS_ERROR(chunk)->message);
        SeaValue e = sea_make_error_str(vm, m ? m->data : "load failed");
        if (m) sea_obj_release(&m->obj);
        sea_obj_release(sea_as_obj(chunk));
        return e;
    }
    char msg[512];
    SeaValue res = SEA_NIL_VAL;
    bool ok = run_guarded(sb, vm, chunk, NULL, 0, &res, msg, sizeof msg);
    sea_obj_release(sea_as_obj(chunk));
    if (!ok) return sea_make_error_str(vm, msg);
    if (sea_is_obj(res)) sea_obj_release(sea_as_obj(res));
    return sea_bool_val(true);
}

static SeaValue m_call(SeaVM *vm, int argc, SeaValue *args) {
    Sandbox *sb = sb_arg(vm, argc, args, NULL);
    SeaString *name = argc > 1 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!name) sea_vm_error_noreturn(vm, "sandbox:call: expected the name of a global function");
    SeaValue fn;
    bool found = sea_globals_get_str(sb->vm, name->data, &fn) && sea_is_closure(fn);
    char msg[512];
    if (!found) {
        snprintf(msg, sizeof msg, "sandbox: no function '%.100s'", name->data);
        sea_obj_release(&name->obj);
        return sea_make_error_str(vm, msg);
    }
    sea_obj_release(&name->obj);
    int n = argc - 2;
    SeaValue sargs[16];
    if (n > 16) sea_vm_error_noreturn(vm, "sandbox:call: too many arguments");
    for (int i = 0; i < n; i++) {
        Copier c = {0, {0}, sb->vm};
        if (!copy_value(&c, args[i + 2], 1, &sargs[i])) {
            for (int j = 0; j < i; j++) if (sea_is_obj(sargs[j])) sea_obj_release(sea_as_obj(sargs[j]));
            snprintf(msg, sizeof msg, "sandbox:call: argument %d: %s", i + 1, c.err);
            return sea_make_error_str(vm, msg);
        }
    }
    SeaValue res = SEA_NIL_VAL;
    bool ok = run_guarded(sb, vm, fn, sargs, n, &res, msg, sizeof msg);
    for (int i = 0; i < n; i++) if (sea_is_obj(sargs[i])) sea_obj_release(sea_as_obj(sargs[i]));
    if (!ok) return sea_make_error_str(vm, msg);
    bool cok;
    SeaValue out = to_host(vm, res, &cok, msg, sizeof msg);
    if (sea_is_obj(res)) sea_obj_release(sea_as_obj(res));
    if (!cok) {
        char full[600];
        snprintf(full, sizeof full, "sandbox:call: result: %s", msg);
        return sea_make_error_str(vm, full);
    }
    return out;
}

static SeaValue m_set(SeaVM *vm, int argc, SeaValue *args) {
    Sandbox *sb = sb_arg(vm, argc, args, NULL);
    SeaString *name = argc > 1 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!name) sea_vm_error_noreturn(vm, "sandbox:set: expected a name");
    Copier c = {0, {0}, sb->vm};
    SeaValue v;
    if (!copy_value(&c, argc > 2 ? args[2] : SEA_NIL_VAL, 1, &v)) {
        sea_obj_release(&name->obj);
        char msg[300];
        snprintf(msg, sizeof msg, "sandbox:set: %s", c.err);
        return sea_make_error_str(vm, msg);
    }
    SeaValue key = new_str(name->data, name->len);
    sea_obj_release(&name->obj);
    sea_globals_set(sb->vm, key, v);
    sea_obj_release(sea_as_obj(key));
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
    return sea_bool_val(true);
}

static SeaValue m_expose(SeaVM *vm, int argc, SeaValue *args) {
    Sandbox *sb = sb_arg(vm, argc, args, NULL);
    SeaString *name = argc > 1 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!name || argc < 3 || !sea_is_closure(args[2])) sea_vm_error_noreturn(vm, "sandbox:expose: expected a name and a function");
    sea_table_set_str(sb->exposed, name->data, args[2]);
    sea_obj_release(&name->obj);
    return sea_bool_val(true);
}

static SeaValue m_output(SeaVM *vm, int argc, SeaValue *args) {
    Sandbox *sb = sb_arg(vm, argc, args, NULL);
    SeaValue s = new_str(sb->out ? sb->out : "", sb->out_len);
    sb->out_len = 0;
    if (sb->out) sb->out[0] = 0;
    return s;
}

static void sb_destroy(Sandbox *sb) {
    if (sb->exposed) sea_obj_release(&sb->exposed->obj);
    sea_vm_free(sb->vm);
    free(sb->out);
    free(sb);
}

static SeaValue m_close(SeaVM *vm, int argc, SeaValue *args) {
    int id;
    Sandbox *sb = sb_arg(vm, argc, args, &id);
    pthread_mutex_lock(&g_mu);
    g_sb[id] = NULL;
    pthread_mutex_unlock(&g_mu);
    sb_destroy(sb);
    return SEA_NIL_VAL;
}

static void set_method(SeaTable *t, const char *name, SeaValue (*fn)(SeaVM *, int, SeaValue *)) {
    SeaClosure *cl = sea_native_closure(name, fn);
    SeaValue v = sea_obj_val(&cl->obj);
    sea_table_set_str(t, name, v);
    sea_obj_release(&cl->obj);
}

/* opts.field from a table, a struct literal or a map. */
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

static SeaValue fn_new(SeaVM *vm, int argc, SeaValue *args) {
    int64_t budget = DEFAULT_BUDGET;
    int64_t out_max = DEFAULT_OUTPUT_LIMIT;
    if (argc > 0) {
        SeaValue v;
        if (opt_get(args[0], "budget", &v) && sea_is_int(v)) budget = sea_as_int(v);
        if (opt_get(args[0], "output_limit", &v) && sea_is_int(v)) out_max = sea_as_int(v);
    }
    if (budget < 1) sea_vm_error_noreturn(vm, "sandbox.new: budget must be positive");
    int id = -1;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_SANDBOXES; i++) if (!g_sb[i]) { id = i; break; }
    if (id >= 0) g_sb[id] = (Sandbox *)1;       /* reserve */
    pthread_mutex_unlock(&g_mu);
    if (id < 0) return sea_make_error_str(vm, "sandbox.new: too many sandboxes");
    Sandbox *sb = calloc(1, sizeof *sb);
    sb->vm = sea_vm_new();
    sb->vm->sandboxed = true;
    sb->vm->budget = budget;
    sb->budget = budget;
    sb->out_max = out_max > 0 ? (size_t)out_max : 0;
    sb->exposed = sea_table_new(TABLE_MAP);
    pthread_mutex_lock(&g_mu);
    g_sb[id] = sb;
    pthread_mutex_unlock(&g_mu);
    sandbox_populate(sb->vm);
    SeaTable *h = sea_table_new(TABLE_MAP);
    sea_table_set_str(h, "id", sea_int_val(id));
    set_method(h, "load", m_load);
    set_method(h, "call", m_call);
    set_method(h, "set", m_set);
    set_method(h, "expose", m_expose);
    set_method(h, "output", m_output);
    set_method(h, "close", m_close);
    return sea_obj_val(&h->obj);
}

void sea_lib_sandbox_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);
    set_method(mod, "new", fn_new);
    sea_vm_register_module(vm, "sandbox", mod);
}
