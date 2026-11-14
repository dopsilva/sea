/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_vm.h"
#include "sea_show.h"
#include "sea_bigint.h"
#include "sea_chunk.h"
#include "sea_string.h"
#include "sea_rope.h"
#include "sea_array.h"
#include "sea_table.h"
#include "sea_map.h"
#include "sea_struct.h"
#include "sea_typed_array.h"
#include "sea_memory.h"
#include "sea_channel.h"
#include "sea_goroutine.h"
#include "sea_sched.h"
#include "sea_jit.h"
#include "sea_gc.h"
#include <math.h>
#include <stdarg.h>
#include <setjmp.h>

// Forward declaration: the dispatch loop, taking base frame level
static int vm_run(SeaVM *vm, int base_frame);
static int vm_run_ex(SeaVM *vm, int base_frame, bool top, bool unwind);

// ---- VM construction ----

SeaVM* sea_vm_new(void) {
    SeaVM *vm = SEA_ALLOC(SeaVM);
    memset(vm, 0, sizeof(*vm));
    vm->shared           = sea_shared_new();
    vm->owns_shared      = true;
    vm->frame_count      = 0;
    vm->panicking        = false;
    vm->has_exec_result  = false;
    vm->exec_result      = SEA_NIL_VAL;
    return vm;
}

SeaVM* sea_vm_new_child(SeaShared *shared) {
    SeaVM *vm = SEA_ALLOC(SeaVM);
    memset(vm, 0, sizeof(*vm));
    vm->shared          = shared;
    vm->owns_shared     = false;
    vm->frame_count     = 0;
    vm->panicking       = false;
    vm->has_exec_result = false;
    vm->exec_result     = SEA_NIL_VAL;
    return vm;
}

void sea_vm_free(SeaVM *vm) {
    if (!vm) return;
    if (sea_gc_vm == vm) sea_gc_vm = NULL;
    if (vm->gc_roots) sea_free(vm->gc_roots, sizeof(SeaObject*) * vm->gc_cap);

    if (vm->owns_shared) {   /* with the required modules' chunks */
        sea_shared_free(vm->shared);
    }
    free(vm->panic_trace);
    SEA_FREE(vm, SeaVM);
}

void sea_vm_reset_child(SeaVM *vm) {
    if (!vm) return;
    if (sea_gc_vm == vm) sea_gc_vm = NULL;
    if (vm->gc_roots) sea_free(vm->gc_roots, sizeof(SeaObject*) * vm->gc_cap);
    free(vm->panic_trace);
    memset(vm, 0, sizeof(*vm));
    /* frame_count == 0 signals "needs init" to worker_thread */
}

// ---- Error handling ----

/* Walk the call stack and print file:line info for each frame to stderr.
 * Assumes frame->ip was saved by the DISPATCH macro before each instruction. */
static void vm_write_traceback(SeaVM *vm, FILE *out) {
    if (vm->frame_count == 0) return;
    fprintf(out, "stack traceback:\n");
    for (int i = vm->frame_count - 1; i >= 0; i--) {
        SeaCallFrame *f = &vm->frames[i];
        if (!f->closure || !f->closure->chunk) continue;
        SeaChunk *c = f->closure->chunk;
        const char *name = c->name ? c->name : "?";
        /* ip saved by DISPATCH() points to the next instruction; -1 = current */
        int idx  = (int)(f->ip - 1 - c->code);
        int line = (idx >= 0 && idx < c->lines_len) ? c->lines[idx] : -1;
        /* A root chunk (main script, module) is its own source; prototypes
         * point at their root (sea_chunk_set_source). */
        int is_file = c->source ? c->source == c->name : (name && strchr(name, '/'));
        if (line > 0) {
            const char *src = c->source ? c->source : "[?]";
            if (is_file)
                fprintf(out, "\t%s:%d: in main chunk\n", name, line);
            else if (name[0])
                fprintf(out, "\t%s:%d: in function '%s'\n", src, line, name);
            else
                fprintf(out, "\t%s:%d: in anonymous function\n", src, line);
        } else {
            fprintf(out, "\t%s: (no line info)\n", name);
        }
    }
}

static void sea_vm_print_traceback(SeaVM *vm) {
    vm_write_traceback(vm, stderr);
}

void sea_vm_error(SeaVM *vm, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(vm->err_msg, sizeof(vm->err_msg), fmt, args);
    va_end(args);
}

void sea_vm_error_noreturn(SeaVM *vm, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char msg[sizeof vm->err_msg];
    vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    /* "file:line: message", as in Lua: where the error happened */
    snprintf(vm->err_msg, sizeof vm->err_msg, "%s", msg);
    if (vm->frame_count > 0) {
        SeaCallFrame *f = &vm->frames[vm->frame_count - 1];
        SeaChunk *c = f->closure ? f->closure->chunk : NULL;
        int idx = c ? (int)(f->ip - 1 - c->code) : -1;
        if (c && idx >= 0 && idx < c->lines_len && c->lines[idx] > 0) {
            const char *src = c->source ? c->source : (c->name ? c->name : "?");
            snprintf(vm->err_msg, sizeof vm->err_msg, "%.180s:%d: %.300s", src, c->lines[idx], msg);
        }
    }
    sea_vm_raise(vm, sea_make_error_str(vm, vm->err_msg));
}

/* ---- Naming the operand of a failed call/index ----------------------------
 * "attempt to call a nil value" does not say which value. The failing
 * instruction's register is traced back to the instruction that loaded it
 * (LOAD_GLOBAL, GET_FIELD, LOAD_UP) to give "(global 'pcall')" or
 * "(field 'gsub' of global 'string')", and a table of names that other
 * languages have and Sea does not adds what to use instead. */

static bool op_is_jump(int op) {
    return op == OP_JMP || op == OP_JMP_TRUE || op == OP_JMP_FALSE || op == OP_JMP_NIL ||
           op == OP_JMP_NOT_ERR || op == OP_FOR_PREP || op == OP_FOR_LOOP ||
           op == OP_FOR_LOOP_STEP1 || op == OP_FOR_LOOP_INT || op == OP_FOR_IN_INIT ||
           op == OP_FOR_IN_NEXT;
}

static const char *const_name(SeaChunk *c, int k) {
    if (k < 0 || k >= c->const_len || !sea_is_string(c->constants[k])) return NULL;
    return SEA_AS_STRING(c->constants[k])->data;
}

/* What loaded register `reg` just before instruction `idx`: kind 'g' global,
 * 'f' field, 'u' upvalue (name in *name; *owner is the global the field was
 * read from, if any). 0: unknown. */
static int trace_reg(SeaChunk *c, int idx, int reg, const char **name, const char **owner) {
    *name = NULL;
    *owner = NULL;
    for (int j = idx - 1; j >= 0 && j >= idx - 16; j--) {
        uint32_t ins = c->code[j];
        int op = (int)(ins & 0xff), a = (int)((ins >> 8) & 0xff);
        if (op_is_jump(op)) return 0;                 /* a branch may join here */
        if (op == OP_STORE_GLOBAL || op == OP_STORE_UP || op == OP_SET_FIELD ||
            op == OP_SET_TABLE || op == OP_SET_ARRAY || op == OP_ARRAY_PUSH) continue;
        if (a != reg) continue;
        if (op == OP_LOAD_GLOBAL) {
            *name = const_name(c, (int)(ins >> 16));
            return *name ? 'g' : 0;
        }
        if (op == OP_GET_FIELD) {
            *name = const_name(c, (int)((ins >> 24) & 0xff));
            if (!*name) return 0;
            const char *n2, *o2;
            if (trace_reg(c, j, (int)((ins >> 16) & 0xff), &n2, &o2) == 'g') *owner = n2;
            return 'f';
        }
        if (op == OP_LOAD_UP) {
            int u = (int)((ins >> 16) & 0xff);
            if (u < c->upval_count && c->upvals[u].name) { *name = c->upvals[u].name; return 'u'; }
        }
        return 0;                                     /* some other writer */
    }
    return 0;
}

/* What Sea has instead of a name from Lua, Python or JavaScript. */
static const char *name_hint(const char *owner, const char *name) {
    static const struct { const char *key, *hint; } table[] = {
        { "table",         "there is no 'table' library: sort with sort.sort(list), append with list[] = v, remove with del list[i], length with len(list)" },
        { "pcall",         "there is no pcall: use `defer function() recover() end` (recover works like Go's)" },
        { "xpcall",        "there is no xpcall: use `defer function() recover() end`" },
        { "ipairs",        "iterate directly: for i, v in list do ... end" },
        { "pairs",         "iterate directly: for k, v in map do ... end" },
        { "unpack",        "assign directly: local a, b = list[0], list[1]" },
        { "setmetatable",  "there are no metatables: use tables or struct literals with function fields" },
        { "min",           "use math.min" }, { "max", "use math.max" }, { "abs", "use math.abs" },
        { "format",        "use string.format" }, { "sprintf", "use string.format" },
        { "join",          "use string.join(list, sep)" }, { "split", "use string.split(s, sep)" },
        { "trim",          "use string.trim(s)" }, { "contains", "use string.contains(s, sub)" },
        { "sum",           "use list.sum" }, { "range", "use list.range" },
        { "sleep",         "use time.sleep(n * time.Second)" }, { "exit", "use os.exit(code)" },
        { "int",           "use tonumber(x) (it returns an int for integer text)" },
        { "float",         "use tonumber(x)" }, { "str", "use tostring(x)" },
        { "string.gsub",   "use string.replace(s, old, new) for plain text, or re.gsub(pattern, s, repl)" },
        { "string.gmatch", "use re.all(pattern, s) or string.split" },
        { "string.match",  "use re.match(pattern, s)" },
        { "string.startswith", "use string.starts_with" }, { "string.endswith", "use string.ends_with" },
        { "string.strip",  "use string.trim" }, { "string.lstrip", "use string.trim" }, { "string.rstrip", "use string.trim" },
        { "string.lines",  "use string.splitlines" },
        { "list.sort",     "use sort.sort(list)" }, { "list.insert", "append with list[] = v" },
        { "list.push",     "append with list[] = v" }, { "list.append", "append with list[] = v" },
        { "list.pop",      "remove with del list[i]" }, { "list.remove", "remove with del list[i]" },
        { "list.reverse",  "use list.reversed(list) or sort.reverse" }, { "list.join", "use string.join(list, sep)" },
        { "os.clock",      "use time.clock()" }, { "os.date", "use time.now() and its methods (see time)" },
        { "os.sleep",      "use time.sleep(n * time.Second)" },
        { "math.sign",     "no math.sign: write `if x > 0 then ... elseif x < 0 then ...`" },
        { "math.clamp",    "use math.max(lo, math.min(hi, x))" },
        { "math.tointeger","use tonumber(x) or x // 1" },
        { "math.inf", "use math.huge" },
    };
    char key[96];
    if (owner) snprintf(key, sizeof key, "%s.%s", owner, name);
    else snprintf(key, sizeof key, "%s", name);
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(table[i].key, key) == 0) return table[i].hint;
    return NULL;
}

/* Raise "attempt to <verb> a <type> value (<what>) - <hint>". `callee`: the
 * failing register is the called one (A); otherwise it is the indexed object. */
static void vm_operand_error(SeaVM *vm, const char *verb, SeaValue v, bool callee) {
    char what[160] = "", hint[260] = "";
    if (vm->frame_count > 0) {
        SeaCallFrame *f = &vm->frames[vm->frame_count - 1];
        SeaChunk *c = f->closure ? f->closure->chunk : NULL;
        int idx = c ? (int)(f->ip - 1 - c->code) : -1;
        if (c && idx >= 0 && idx < c->code_len) {
            uint32_t ins = c->code[idx];
            int op = (int)(ins & 0xff), a = (int)((ins >> 8) & 0xff), b = (int)((ins >> 16) & 0xff);
            int reg = -1;
            if (op == OP_CALL || op == OP_CALL_SELF || op == OP_CALL_MULTI) reg = a;
            else if (op == OP_GET_FIELD || op == OP_GET_TABLE || op == OP_GET_TABLE2 || op == OP_GET_ARRAY) reg = b;
            else if (op == OP_SET_FIELD || op == OP_SET_TABLE || op == OP_SET_ARRAY) reg = a;
            const char *name, *owner;
            int kind = reg >= 0 ? trace_reg(c, idx, reg, &name, &owner) : 0;
            if (kind == 'g') snprintf(what, sizeof what, " (global '%s')", name);
            else if (kind == 'f' && owner) snprintf(what, sizeof what, " (field '%s' of global '%s')", name, owner);
            else if (kind == 'f') snprintf(what, sizeof what, " (field '%s')", name);
            else if (kind == 'u') snprintf(what, sizeof what, " (upvalue '%s')", name);
            if (kind == 'g' || kind == 'f') {
                const char *h = name_hint(kind == 'f' ? owner : NULL, name);
                /* indexing a missing global library: the hint is about the library */
                if (h) snprintf(hint, sizeof hint, ": %s", h);
            }
            (void)callee;
        }
    }
    sea_vm_error_noreturn(vm, "attempt to %s a %s value%s%s", verb, sea_type_name(v), what, hint);
}

/* ---- Panics ----
 * A panic (panic(v) or a runtime error) unwinds the call stack frame by frame,
 * running each frame's deferred calls (panic_unwind in vm_run_inner). A
 * deferred call that calls recover() stops it: that frame then returns
 * normally, with nil results, after its remaining deferred calls. A panic
 * that reaches the root of a nested vm_run (sea_vm_call from a native, a
 * module body, a JIT helper) sets panic_propagating and returns; the caller's
 * VM loop continues the unwinding when control gets back to it. A panic that
 * reaches the root of the program or of a goroutine ends the process.
 *
 * Runtime errors raised from C code inside the loop longjmp to it (vm_run).
 * Frames that are unwound do not release their locals (a leak on that path,
 * like the values held by deferred-call arguments of an aborted native). */

typedef struct {
    jmp_buf buf;
} SeaPanicJmp;

static void vm_capture_trace(SeaVM *vm) {
    free(vm->panic_trace);
    vm->panic_trace = NULL;
    char *buf = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    if (!f) return;
    vm_write_traceback(vm, f);
    fclose(f);
    vm->panic_trace = buf;
}

void sea_vm_raise(SeaVM *vm, SeaValue v) {
    if (vm->panicking) { if (sea_is_obj(vm->panic_val)) sea_obj_release(sea_as_obj(vm->panic_val)); }   /* a new panic replaces it */
    vm->panicking = true;
    vm->panic_propagating = false;
    vm->panic_val = v;
    vm_capture_trace(vm);
    if (vm->panic_jmp) longjmp(((SeaPanicJmp *)vm->panic_jmp)->buf, 1);
    sea_vm_panic_exit(vm);
}

void sea_vm_panic_exit(SeaVM *vm) {
    if (vm->panic_trace) fputs(vm->panic_trace, stderr);
    SeaValue v = vm->panic_val;
    if (sea_is_error(v)) {
        SeaString *s = sea_vm_tostring(vm, SEA_AS_ERROR(v)->message);
        fprintf(stderr, "sea runtime error: %s\n", s->data);
        sea_obj_release(&s->obj);
    } else {
        SeaString *s = sea_vm_tostring(vm, v);
        fprintf(stderr, "sea panic: %s\n", s->data);
        sea_obj_release(&s->obj);
    }
    fflush(stdout);
    exit(1);
}

void sea_vm_budget_exceeded(SeaVM *vm) {
    vm->killed = true;
    sea_vm_error_noreturn(vm, "instruction budget exceeded");
}

SeaValue sea_vm_recover(SeaVM *vm) {
    if (!vm->panicking) return SEA_NIL_VAL;
    if (vm->killed) return SEA_NIL_VAL;   /* the budget panic cannot be recovered */
    SeaValue v = vm->panic_val;      /* owned: moves to the caller */
    vm->panicking = false;
    vm->panic_val = SEA_NIL_VAL;
    free(vm->panic_trace);
    vm->panic_trace = NULL;
    return v;
}

// ---- Native closure creation ----

SeaClosure* sea_native_closure(const char *name, SeaValue (*fn)(SeaVM*, int, SeaValue*)) {
    SeaClosure *cl = SEA_ALLOC(SeaClosure);
    cl->obj.type = OBJ_CLOSURE;
    cl->obj.gc = 0;
    cl->obj.refcount = 1;
    cl->chunk = NULL;
    cl->is_native = true;
    cl->native = fn;
    cl->native_name = name;
    cl->upvalues = NULL;
    cl->upval_count = 0;
    return cl;
}

void sea_globals_set(SeaVM *vm, SeaValue key, SeaValue val) {
    /* With no goroutine alive, the main thread is the only one that can touch
     * the globals (a spawn counts the goroutine before it can run). */
    if (!vm->goroutine &&
        (!sea_global_sched || atomic_load(&sea_global_sched->goroutine_count) == 0)) {
        sea_table_set(vm_globals(vm), key, val);
        return;
    }
    sea_table_set_sync(vm_globals(vm), &vm->shared->globals_sync, key, val);
}

bool sea_globals_get(SeaVM *vm, SeaValue key, SeaValue *out) {
    return sea_table_get_sync(vm_globals(vm), &vm->shared->globals_sync, key, NULL, out);
}

bool sea_globals_get_str(SeaVM *vm, const char *name, SeaValue *out) {
    SeaString *s = sea_string_intern(name, strlen(name));
    bool found = sea_globals_get(vm, sea_obj_val(&s->obj), out);
    sea_obj_release(&s->obj);
    return found;
}

void sea_vm_register_native(SeaVM *vm, const char *name,
                              SeaValue (*fn)(SeaVM*, int, SeaValue*)) {
    SeaClosure *cl = sea_native_closure(name, fn);   /* cl rc=1 */
    SeaString *key = sea_string_intern(name, strlen(name)); /* key rc=2 */
    sea_globals_set(vm, sea_obj_val(&key->obj), sea_obj_val(&cl->obj));
    /* table_set retained both: cl rc=2, key rc=3 */
    sea_obj_release(&key->obj); /* key rc=2 (intern=1 + table=1) */
    sea_obj_release(&cl->obj);  /* cl  rc=1 (table=1) */
}

void sea_vm_register_module(SeaVM *vm, const char *name, SeaTable *mod) {
    sea_table_add_lowercase_aliases(mod);
    SeaString *key = sea_string_intern(name, strlen(name)); /* key rc=2 */
    sea_globals_set(vm, sea_obj_val(&key->obj), sea_obj_val(&mod->obj));
    /* table_set retained both */
    sea_obj_release(&key->obj);  /* caller releases their ref */
    /* mod: caller passes their ref; they should release after this call if desired */
}

// ---- Upvalue management ----

static SeaUpvalue* capture_upvalue(SeaVM *vm, SeaValue *location) {
    // Check if we already have an open upvalue for this location
    SeaUpvalue *prev = NULL;
    SeaUpvalue *up = vm->open_upvalues;
    while (up && up->location > location) {
        prev = up;
        up = up->next;
    }
    if (up && up->location == location) {
        sea_obj_retain(&up->obj);
        return up;
    }
    // Create new open upvalue: one reference for the capturing closure, one
    // for the open list (dropped by close_upvalues), so a closure freed while
    // its upvalue is still open cannot leave a dangling list entry.
    SeaUpvalue *new_up = SEA_ALLOC(SeaUpvalue);
    new_up->obj.type = OBJ_UPVALUE;
    new_up->obj.gc = 0;
    new_up->obj.refcount = 2;
    new_up->location = location;
    new_up->closed = SEA_NIL_VAL;
    new_up->lock = 0;
    new_up->next = up;
    if (prev) prev->next = new_up;
    else vm->open_upvalues = new_up;
    return new_up;
}

/* OP_MARK_CAPTURED: the local at `slot` was just assigned and some closure of
 * this function captures it. If that closure was handed to a goroutine, its
 * open upvalue is shared, and the goroutine can now reach the new value
 * through it, so the value must be shared too (the owner thread marks it, as
 * for channel sends). Locals no goroutine sees stay with plain RC. */
static inline void mark_captured(SeaVM *vm, SeaValue *slot) {
    SeaValue v = *slot;
    if (!sea_is_obj(v) ||
        (__atomic_load_n(&sea_as_obj(v)->refcount, __ATOMIC_RELAXED) & SEA_RC_SHARED_BIT))
        return;
    SeaUpvalue *up = vm->open_upvalues;      /* sorted by location, descending */
    while (up && up->location > slot) up = up->next;
    if (up && up->location == slot &&
        (__atomic_load_n(&up->obj.refcount, __ATOMIC_RELAXED) & SEA_RC_SHARED_BIT))
        sea_val_mark_shared(v);
}

static void close_upvalues(SeaVM *vm, SeaValue *from) {
    while (vm->open_upvalues && vm->open_upvalues->location >= from) {
        SeaUpvalue *up = vm->open_upvalues;
        /* Copy the live value into the upvalue's own storage, then publish the
         * new location with release semantics: a goroutine on another thread
         * reading through this upvalue (acquire) then sees the copied value. */
        /* The register's reference moves into the upvalue (docs/memory.md),
         * and the register is cleared so nothing releases it a second time
         * (the loop variable of the next iteration, a return's cleanup).
         * A shared upvalue may be stored into by another goroutine meanwhile:
         * the lock keeps the move whole, and readers retry if the location
         * changed under them (sea_upvalue_get). */
        bool shared = __atomic_load_n(&up->obj.refcount, __ATOMIC_RELAXED) & SEA_RC_SHARED_BIT;
        if (shared) sea_upvalue_lock(up);
        SeaValue *slot = up->location;
        up->closed = *slot;
        __atomic_store_n(&up->location, &up->closed, __ATOMIC_RELEASE);
        __atomic_store_n(slot, SEA_NIL_VAL, __ATOMIC_RELEASE);
        if (shared) sea_upvalue_unlock(up);
        /* Remove from the open chain and drop the list's reference (frees the
         * upvalue, and the value it now owns, if no closure holds it). */
        vm->open_upvalues = up->next;
        up->next = NULL;
        sea_obj_release(&up->obj);
    }
}

/* Release a value an owner (local, temporary) held — the body of OP_RELEASE.
 * (An arena object reaching zero tears down its contents: sea_obj_release.) */
static inline __attribute__((always_inline)) void vm_release_val(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}
static inline __attribute__((always_inline)) void vm_retain_val(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_retain(sea_as_obj(v));
}

// ---- Value operations ----

/* The operand named in an arithmetic error; a bigint gets a pointer to the
 * functions that do its arithmetic (the operators take int and number). */
static const char *arith_what(SeaValue v) {
    if (sea_is_bigint(v))
        return "bigint (operators take int and number; use bigint.add, sub, mul, div, mod, pow)";
    return sea_type_name(v);
}

static SeaValue val_add(SeaVM *vm, SeaValue a, SeaValue b) {
    if (sea_is_int(a) && sea_is_int(b)) {
        return sea_int_val(sea_as_int(a) + sea_as_int(b));
    }
    double na = sea_is_number(a) ? sea_as_number(a) : (sea_is_int(a) ? (double)sea_as_int(a) : 0);
    double nb = sea_is_number(b) ? sea_as_number(b) : (sea_is_int(b) ? (double)sea_as_int(b) : 0);
    if (!sea_is_number(a) && !sea_is_int(a)) {
        sea_vm_error_noreturn(vm, "attempt to perform arithmetic on %s", arith_what(a));
    }
    if (!sea_is_number(b) && !sea_is_int(b)) {
        sea_vm_error_noreturn(vm, "attempt to perform arithmetic on %s", arith_what(b));
    }
    return sea_number_val(na + nb);
}

static SeaValue val_sub(SeaVM *vm, SeaValue a, SeaValue b) {
    if (sea_is_int(a) && sea_is_int(b)) {
        return sea_int_val(sea_as_int(a) - sea_as_int(b));
    }
    double na = sea_is_number(a) ? sea_as_number(a) : (sea_is_int(a) ? (double)sea_as_int(a) : 0);
    double nb = sea_is_number(b) ? sea_as_number(b) : (sea_is_int(b) ? (double)sea_as_int(b) : 0);
    if (!sea_is_number(a) && !sea_is_int(a))
        sea_vm_error_noreturn(vm, "attempt to perform arithmetic on %s", arith_what(a));
    if (!sea_is_number(b) && !sea_is_int(b))
        sea_vm_error_noreturn(vm, "attempt to perform arithmetic on %s", arith_what(b));
    return sea_number_val(na - nb);
}

static SeaValue val_mul(SeaVM *vm, SeaValue a, SeaValue b) {
    if (sea_is_int(a) && sea_is_int(b)) {
        return sea_int_val(sea_as_int(a) * sea_as_int(b));
    }
    double na = sea_is_number(a) ? sea_as_number(a) : (sea_is_int(a) ? (double)sea_as_int(a) : 0);
    double nb = sea_is_number(b) ? sea_as_number(b) : (sea_is_int(b) ? (double)sea_as_int(b) : 0);
    if (!sea_is_number(a) && !sea_is_int(a))
        sea_vm_error_noreturn(vm, "attempt to perform arithmetic on %s", arith_what(a));
    if (!sea_is_number(b) && !sea_is_int(b))
        sea_vm_error_noreturn(vm, "attempt to perform arithmetic on %s", arith_what(b));
    return sea_number_val(na * nb);
}

/* Integer // and % round toward minus infinity, like their float versions:
 * a == (a // b) * b + a % b, and a % b has the sign of b. */
static inline int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a ^ b) < 0)) q--;
    return q;
}

static double to_number(SeaVM *vm, SeaValue v) {
    if (sea_is_number(v)) return sea_as_number(v);
    if (sea_is_int(v)) return (double)sea_as_int(v);
    sea_vm_error_noreturn(vm, "attempt to perform arithmetic on %s", arith_what(v));
}

static int64_t to_int(SeaVM *vm, SeaValue v) {
    if (sea_is_int(v)) return sea_as_int(v);
    if (sea_is_number(v)) return (int64_t)sea_as_number(v);
    sea_vm_error_noreturn(vm, "attempt to perform bitwise on %s", sea_type_name(v));
}

// ---- String conversion ----

SeaString* sea_vm_tostring(SeaVM *vm, SeaValue v) {
    (void)vm;
    if (sea_is_nil(v))    return sea_string_from_cstr("nil");
    if (sea_is_bool(v))   return sea_string_from_cstr(sea_as_bool(v) ? "true" : "false");
    if (sea_is_int(v))    return sea_int_to_string(sea_as_int(v));
    if (sea_is_number(v)) return sea_number_to_string(sea_as_number(v));
    if (sea_is_string(v)) {
        SeaString *s = SEA_AS_STRING(v);
        sea_obj_retain(&s->obj);
        return s;
    }
    if (sea_is_rope(v)) {
        return sea_rope_flatten(SEA_AS_ROPE(v));  /* RC=1 */
    }
    if (sea_is_obj(v)) {
        SeaObject *obj = sea_as_obj(v);
        switch (obj->type) {
            case OBJ_ARRAY: case OBJ_TABLE: case OBJ_MAP: case OBJ_STRUCT:
            case OBJ_INT_ARRAY: case OBJ_FLOAT_ARRAY: case OBJ_FLOAT32_ARRAY:
                /* the contents: [1, "a"], {k = 1} (src/sea_show.c) */
                return sea_show(vm, v, SEA_SHOW_DEPTH);
            case OBJ_CLOSURE:
            case OBJ_FUNCTION: {
                const char *name = obj->type == OBJ_CLOSURE ?
                    (((SeaClosure*)obj)->is_native ? ((SeaClosure*)obj)->native_name :
                     (((SeaClosure*)obj)->chunk ? ((SeaClosure*)obj)->chunk->name : "?")) :
                    ((SeaFunction*)obj)->name;
                char buf[128];
                snprintf(buf, sizeof(buf), "function(%s)", name ? name : "?");
                return sea_string_from_cstr(buf);
            }
            case OBJ_ERROR: {
                SeaError *err = (SeaError*)obj;
                SeaString *msg = sea_vm_tostring(vm, err->message);
                char buf[256];
                snprintf(buf, sizeof(buf), "error(%s)", msg->data);
                sea_obj_release(&msg->obj);
                return sea_string_from_cstr(buf);
            }
            case OBJ_CHANNEL:
                return sea_string_from_cstr("channel");
            case OBJ_BIGINT:
                return sea_bigint_to_string((SeaBigInt*)obj, 10);
            default: {
                char buf[64];
                snprintf(buf, sizeof(buf), "object(%p)", (void*)obj);
                return sea_string_from_cstr(buf);
            }
        }
    }
    return sea_string_from_cstr("<unknown>");
}

// ---- Error values ----

SeaValue sea_make_error(SeaVM *vm, SeaValue msg) {
    (void)vm;
    SeaError *err = SEA_ALLOC(SeaError);
    err->obj.type = OBJ_ERROR;
    err->obj.gc = 0;
    err->obj.refcount = 1;
    err->message = msg;          /* the error owns its message */
    if (sea_is_obj(msg)) sea_obj_retain(sea_as_obj(msg));
    return sea_obj_val(&err->obj);
}

SeaValue sea_make_error_str(SeaVM *vm, const char *msg) {
    SeaString *s = sea_string_from_cstr(msg);
    SeaValue sv = sea_obj_val(&s->obj);
    SeaValue err = sea_make_error(vm, sv);
    sea_obj_release(&s->obj);
    return err;
}

// ---- Table/array access ----

static SeaValue table_get(SeaVM *vm, SeaValue obj_val, SeaValue key) {
    /* a rope key means its string content (struct fields, "length", ...) */
    if (SEA_UNLIKELY(sea_is_rope(key))) key = sea_rope_key(key);
    if (sea_is_array(obj_val)) {
        SeaArray *arr = SEA_AS_ARRAY(obj_val);
        if (sea_is_int(key)) {
            return sea_array_get(arr, sea_as_int(key));
        }
        if (sea_is_number(key)) {
            return sea_array_get(arr, (int64_t)sea_as_number(key));
        }
        // Check for method on string "length"
        if (sea_is_string(key)) {
            SeaString *s = SEA_AS_STRING(key);
            if (strcmp(s->data, "length") == 0 || strcmp(s->data, "len") == 0) {
                return sea_int_val(arr->len);
            }
        }
        return SEA_NIL_VAL;
    }
    if (sea_is_table(obj_val)) {
        SeaTable *t = SEA_AS_TABLE(obj_val);
        SeaValue result = SEA_NIL_VAL;
        sea_table_get(t, key, &result);
        return result;
    }
    if (sea_is_map(obj_val)) {
        SeaValue result = SEA_NIL_VAL;
        sea_map_get(SEA_AS_MAP(obj_val), key, &result);
        return result;
    }
    if (sea_is_struct(obj_val)) {
        if (sea_is_string(key)) {
            return sea_struct_get(SEA_AS_STRUCT(obj_val), SEA_AS_STRING(key));
        }
        return SEA_NIL_VAL;
    }
    if (sea_is_int_array(obj_val)) {
        int64_t idx = sea_is_int(key) ? sea_as_int(key) : (int64_t)sea_as_number(key);
        return sea_int_val(sea_int_array_get(SEA_AS_INT_ARRAY(obj_val), idx));
    }
    if (sea_is_float_array(obj_val)) {
        int64_t idx = sea_is_int(key) ? sea_as_int(key) : (int64_t)sea_as_number(key);
        return sea_number_val(sea_float_array_get(SEA_AS_FLOAT_ARRAY(obj_val), idx));
    }
    if (sea_is_float32_array(obj_val)) {
        int64_t idx = sea_is_int(key) ? sea_as_int(key) : (int64_t)sea_as_number(key);
        return sea_number_val(sea_float32_array_get(SEA_AS_FLOAT32_ARRAY(obj_val), idx));
    }
    if (sea_is_string(obj_val)) {
        // String method access
        SeaValue str_mod;
        if (sea_globals_get_str(vm, "string", &str_mod) && sea_is_table(str_mod)) {
            SeaValue fn = SEA_NIL_VAL;
            sea_table_get(SEA_AS_TABLE(str_mod), key, &fn);
            return fn;
        }
        return SEA_NIL_VAL;
    }
    if (sea_is_error(obj_val)) {
        // Allow accessing .message on error objects
        if (sea_is_string(key)) {
            SeaString *ks = SEA_AS_STRING(key);
            if (strcmp(ks->data, "message") == 0) {
                return ((SeaError*)sea_as_obj(obj_val))->message;
            }
        }
        return SEA_NIL_VAL;
    }
    vm_operand_error(vm, "index", obj_val, false);
}

static void table_set(SeaVM *vm, SeaValue obj_val, SeaValue key, SeaValue value) {
    /* a rope key means its string content (struct fields, "length", ...) */
    if (SEA_UNLIKELY(sea_is_rope(key))) key = sea_rope_key(key);
    if (sea_is_array(obj_val)) {
        SeaArray *arr = SEA_AS_ARRAY(obj_val);
        if (sea_is_int(key)) {
            int64_t idx = sea_as_int(key);
            // Grow array if needed
            while (idx >= (int64_t)arr->len) {
                sea_array_push(arr, SEA_NIL_VAL);
            }
            sea_array_set(arr, idx, value);
            return;
        }
        if (sea_is_number(key)) {
            sea_array_set(arr, (int64_t)sea_as_number(key), value);
            return;
        }
        sea_vm_error_noreturn(vm, "array index must be integer");
    }
    if (sea_is_table(obj_val)) {
        sea_table_set(SEA_AS_TABLE(obj_val), key, value);
        return;
    }
    if (sea_is_map(obj_val)) {
        sea_map_set(SEA_AS_MAP(obj_val), key, value);
        return;
    }
    if (sea_is_struct(obj_val)) {
        if (sea_is_string(key)) {
            SeaStruct *st = SEA_AS_STRUCT(obj_val);
            if (st->building_remaining > 0) {
                sea_struct_init_field(st, SEA_AS_STRING(key), value);
                return;
            }
            if (!sea_struct_set(st, SEA_AS_STRING(key), value)) {
                sea_vm_error_noreturn(vm, "attempt to add field '%s' to sealed struct",
                                     SEA_AS_STRING(key)->data);
            }
            return;
        }
        sea_vm_error_noreturn(vm, "struct field key must be a string");
    }
    if (sea_is_int_array(obj_val)) {
        int64_t idx = sea_is_int(key) ? sea_as_int(key) : (int64_t)sea_as_number(key);
        int64_t v = sea_is_int(value) ? sea_as_int(value) : (int64_t)sea_as_number(value);
        sea_int_array_set(SEA_AS_INT_ARRAY(obj_val), idx, v);
        return;
    }
    if (sea_is_float_array(obj_val)) {
        int64_t idx = sea_is_int(key) ? sea_as_int(key) : (int64_t)sea_as_number(key);
        double v = sea_is_number(value) ? sea_as_number(value) : (double)sea_as_int(value);
        sea_float_array_set(SEA_AS_FLOAT_ARRAY(obj_val), idx, v);
        return;
    }
    if (sea_is_float32_array(obj_val)) {
        int64_t idx = sea_is_int(key) ? sea_as_int(key) : (int64_t)sea_as_number(key);
        double v = sea_is_number(value) ? sea_as_number(value) : (double)sea_as_int(value);
        sea_float32_array_set(SEA_AS_FLOAT32_ARRAY(obj_val), idx, v);
        return;
    }
    vm_operand_error(vm, "index", obj_val, false);
}

/* ---- Frame arena ---- */

SEA_INLINE void *sea_arena_alloc(SeaVM *vm, size_t size) {
    size = (size + 7u) & ~7u;  /* align to 8 bytes */
    if (SEA_UNLIKELY(vm->frame_arena_used + (uint32_t)size > SEA_FRAME_ARENA_SIZE))
        return NULL;
    void *ptr = vm->frame_arena + vm->frame_arena_used;
    vm->frame_arena_used += (uint32_t)size;
    return ptr;
}

/* ---- M:N goroutine helpers (Fase 4) --------------------------------------- */

/* Varargs get a block of their own (see release_varargs). */
static inline SeaValue *varargs_new(const SeaValue *src, int n) {
    SeaValue *v = SEA_ALLOC_N(SeaValue, (size_t)n);
    memcpy(v, src, (size_t)n * sizeof(SeaValue));
    return v;
}

/*
 * Push the initial call frame for a goroutine without running it.
 * Called once per goroutine the first time a worker picks it up.
 * Subsequent resumptions go straight to sea_vm_resume().
 */
static inline SeaValue call_native(SeaVM *vm, SeaClosure *cl, int argc, SeaValue *args);

bool sea_vm_goroutine_init(SeaVM *vm, SeaValue fn, SeaValue *args, int argc) {
    if (!sea_is_obj(fn)) return false;
    SeaObject *obj = sea_as_obj(fn);
    if (obj->type != OBJ_CLOSURE) return false;
    SeaClosure *cl = (SeaClosure*)obj;
    if (cl->is_native) {
        /* `go print("x")`: no bytecode frame — just run the C function here;
         * it borrows the (owned) arguments, released after. No frame to
         * resume, so it cannot park: native_depth makes I/O block. */
        vm->native_depth++;
        vm->regs[0] = call_native(vm, cl, argc, args);
        vm->native_depth--;
        if (vm->panic_propagating) sea_vm_panic_exit(vm);
        for (int i = 0; i < argc; i++) vm_release_val(args[i]);
        return false;
    }

    SeaChunk *chunk = cl->chunk;
    SeaCallFrame *frame = &vm->frames[vm->frame_count++];
    frame->closure    = cl;
    frame->ip         = chunk->code;
    frame->base       = vm->regs;   /* goroutine root frame at register 0 */
    frame->expected_ret = -1;
    frame->defer_base = 0;
    frame->arena_mark = vm->frame_arena_used;

    /* The arguments are owned (+1): the parameters take them over; the
     * varargs go past the registers like a call's (released by RETURN);
     * surplus arguments of a fixed-arity function are released. */
    frame->vararg_count = 0;
    frame->varargs = NULL;
    for (int i = 0; i < argc && i < chunk->arity; i++)
        frame->base[i] = args[i];
    for (int i = argc; i < chunk->arity; i++)
        frame->base[i] = SEA_NIL_VAL;
    if (argc > chunk->arity) {
        if (chunk->is_vararg) {
            frame->vararg_count = argc - chunk->arity;
            frame->varargs = varargs_new(&args[chunk->arity], frame->vararg_count);
        } else {
            for (int i = chunk->arity; i < argc; i++) vm_release_val(args[i]);
        }
    }
    return true;
}

/* Run (or resume) the goroutine until it finishes or parks on a channel.
 * Returns SEA_OK on normal return, SEA_SUSPENDED if parked. */
int sea_vm_resume(SeaVM *vm) {
    return vm_run_ex(vm, 0, true, false);
}

/* ---- Call dispatch ----*/

/* Body of OP_FOR_IN_NEXT, shared with the JIT (sea_jit_for_in_next).
 * A=iter_reg, var_count=B: R[A] = container (array/table/map/struct/channel),
 * R[A+1] = cursor, R[A+2..A+1+var_count] = loop variables. Ownership
 * (docs/memory.md): R[A] holds a reference to the container, and the loop
 * variables own their values — each step releases the previous ones (nil
 * after OP_FOR_IN_INIT, or after CLOSE_UP moved them into a closure) and
 * retains the new ones (a channel hands over its reference). When exhausted,
 * releases all of them and sets R[A]=nil so the following OP_JMP_NIL exits.
 * Returns 1 when the goroutine parked on a channel (frame->ip = resume_ip, the
 * caller must suspend), else 0. always_inline: the interpreter's handler stays
 * as fast as when inline. */
static inline __attribute__((always_inline))
void for_in_set(SeaValue *slot, SeaValue v, bool retain) {
    SeaValue old = *slot;
    if (retain) vm_retain_val(v);
    *slot = v;
    vm_release_val(old);
}
static inline __attribute__((always_inline))
int vm_for_in_next(SeaVM *vm, SeaCallFrame *frame, SeaValue *base, uint32_t *resume_ip,
                   int a, int var_count) {
    int first_var = a + 2;

    SeaValue target_v = base[a];
    int64_t  pos      = sea_as_int(base[a + 1]);
    bool done = false;

    if (sea_is_array(target_v)) {
        SeaArray *arr = SEA_AS_ARRAY(target_v);
        if (pos >= (int64_t)arr->len) {
            done = true;
        } else {
            if (var_count >= 1) for_in_set(&base[first_var], sea_int_val(pos), false);
            if (var_count >= 2) for_in_set(&base[first_var + 1], arr->data[pos], true);
            base[a + 1] = sea_int_val(pos + 1);
        }
    } else if (sea_is_table(target_v) || sea_is_map(target_v) || sea_is_struct(target_v)) {
        uint32_t iter_pos = (uint32_t)pos;
        SeaValue k = SEA_NIL_VAL, v = SEA_NIL_VAL;
        bool more = sea_is_table(target_v)  ? sea_table_next(SEA_AS_TABLE(target_v), &iter_pos, &k, &v)
                  : sea_is_map(target_v)    ? sea_map_next(SEA_AS_MAP(target_v), &iter_pos, &k, &v)
                  :                           sea_struct_next(SEA_AS_STRUCT(target_v), &iter_pos, &k, &v);
        if (!more) {
            done = true;
        } else {
            if (var_count >= 1) for_in_set(&base[first_var], k, true);
            if (var_count >= 2) for_in_set(&base[first_var + 1], v, true);
            base[a + 1] = sea_int_val((int64_t)iter_pos);
        }
    } else if (sea_is_channel(target_v)) {
        SeaChannel *ch = SEA_AS_CHANNEL(target_v);
        SeaGoroutine *g = vm->goroutine;
        SeaValue recv_val = SEA_NIL_VAL;     /* +1 from the channel */
        if (g && g->has_pending_recv) {
            recv_val = g->block_val;
            g->block_val = SEA_NIL_VAL;
            g->has_pending_recv = false;
        } else if (g) {
            /* Resume point must be in place before the goroutine becomes
             * visible in the channel's wait queue (a waker may requeue it
             * immediately); ip itself is untouched if we don't park. */
            frame->ip = resume_ip;
            SeaChanStatus st = sea_chan_recv_nb(ch, &recv_val, g);
            if (st == CHAN_PARKED) return 1;
            if (st == CHAN_CLOSED) done = true;
        } else {
            if (!sea_chan_recv(ch, &recv_val)) done = true;
        }
        if (!done) {
            if (var_count >= 1) for_in_set(&base[first_var], recv_val, false);
            else vm_release_val(recv_val);
        }
    } else {
        done = true;
    }

    if (done) {
        for (int j = 0; j < var_count; j++) {
            SeaValue old = base[first_var + j];
            base[first_var + j] = SEA_NIL_VAL;
            vm_release_val(old);
        }
        SeaValue cont = base[a];
        base[a] = SEA_NIL_VAL;
        vm_release_val(cont);
    }
    return 0;
}

/* Body of OP_DEL: del R[a][R[b]] (array with shift, map or table). Shared
 * with the JIT (sea_jit_del). */
static inline __attribute__((always_inline))
void vm_del(SeaVM *vm, SeaValue *base, int a, int b) {
    (void)vm;
    SeaValue obj_v = base[a];
    SeaValue key_v = base[b];
    if (sea_is_array(obj_v)) {
        int64_t idx = sea_is_int(key_v) ? sea_as_int(key_v) : (int64_t)sea_as_number(key_v);
        sea_array_del(SEA_AS_ARRAY(obj_v), idx);
    } else if (sea_is_map(obj_v)) {
        sea_map_delete(SEA_AS_MAP(obj_v), key_v);
    } else if (sea_is_table(obj_v)) {
        sea_table_delete(SEA_AS_TABLE(obj_v), key_v);
    }
}

/* Body of OP_GET_TABLE2: R[a] = R[b][R[c]], R[a+1] = found. Shared with the
 * JIT (sea_jit_get_table2). */
static inline __attribute__((always_inline))
void vm_get_table2(SeaValue *base, int a, int b, int c) {
    SeaValue obj2 = base[b], key2 = base[c];
    if (SEA_UNLIKELY(sea_is_rope(key2))) key2 = sea_rope_key(key2);   /* struct branch */
    SeaValue val2 = SEA_NIL_VAL;
    bool ok2;
    if (sea_is_map(obj2)) {
        ok2 = sea_map_get(SEA_AS_MAP(obj2), key2, &val2);
    } else if (sea_is_table(obj2)) {
        ok2 = sea_table_get(SEA_AS_TABLE(obj2), key2, &val2);
    } else if (sea_is_struct(obj2)) {
        if (sea_is_string(key2)) {
            val2 = sea_struct_get(SEA_AS_STRUCT(obj2), SEA_AS_STRING(key2));
            ok2  = !sea_is_nil(val2);
        } else {
            ok2 = false;
        }
    } else if (sea_is_array(obj2)) {
        int64_t idx2 = sea_is_int(key2) ? sea_as_int(key2) : (int64_t)sea_as_number(key2);
        val2 = sea_array_get(SEA_AS_ARRAY(obj2), idx2);
        ok2  = !sea_is_nil(val2);
    } else {
        ok2 = false;
    }
    base[a]   = val2;
    base[a+1] = sea_bool_val(ok2);
}

/* Record type feedback for the GET_TABLE/SET_TABLE at instruction pc
 * (interpreter slow path only): the JIT emits an inline typed-array path or a
 * table_get/set helper only at sites that needed one (SEA_FB_* in sea_chunk.h),
 * keeping generic-array loops lean. */
static void sea_chunk_feedback(SeaChunk *c, int pc, SeaValue container) {
    uint32_t f = (sea_is_obj(container) &&
                  (sea_as_obj(container)->type == OBJ_INT_ARRAY ||
                   sea_as_obj(container)->type == OBJ_FLOAT_ARRAY))
               ? SEA_FB_TYPED_ARRAY : SEA_FB_OTHER;
    struct SeaTableICSlot *slot = &sea_chunk_ics(c)[pc];
    if (!(__atomic_load_n(&slot->idx, __ATOMIC_RELAXED) & f))
        __atomic_fetch_or(&slot->idx, f, __ATOMIC_RELAXED);
}

/* The same feedback from the interpreter's typed-array fast path: one
 * relaxed load once the bit is set. */
static inline void sea_chunk_feedback_typed(SeaChunk *c, int pc) {
    struct SeaTableICSlot *slot = &sea_chunk_ics(c)[pc];
    if (!(__atomic_load_n(&slot->idx, __ATOMIC_RELAXED) & SEA_FB_TYPED_ARRAY))
        __atomic_fetch_or(&slot->idx, SEA_FB_TYPED_ARRAY, __ATOMIC_RELAXED);
}

/* SEA_TYPE_CHECK=1 (diagnostic, `make test-dwe`): IIADD/IISUB — emitted only
 * for operands the compiler proved to be ints — abort if that proof was
 * wrong, instead of silently computing garbage. */
static bool g_type_check;
__attribute__((constructor)) static void init_type_check(void) {
    const char *e = getenv("SEA_TYPE_CHECK");
    g_type_check = e && *e && strcmp(e, "0") != 0;
}
static void type_check_ints(SeaVM *vm, SeaCallFrame *frame, uint32_t *ip, SeaValue b, SeaValue c) {
    if (sea_is_int(b) && sea_is_int(c)) return;
    frame->ip = ip;
    sea_vm_error_noreturn(vm, "SEA_TYPE_CHECK: compiler inferred int operands, got %s and %s "
                          "(unsound int inference — compiler bug)",
                          sea_type_name(b), sea_type_name(c));
}

/* String view of a value: the string itself, a rope's cached flat copy
 * (borrowed), or NULL for non-strings — so comparisons treat ropes (long `..`
 * results) exactly like strings. */
static inline SeaString *str_of(SeaValue v) {
    if (sea_is_string(v)) return SEA_AS_STRING(v);
    if (sea_is_rope(v))   return sea_rope_flat(SEA_AS_ROPE(v));
    return NULL;
}

/* Call a C function. Natives test string arguments with sea_is_string (24 of
 * them in the built-in libs alone), so a rope — what `..` produces past
 * SEA_ROPE_THRESHOLD bytes — used to be rejected silently (Bug 1: os.exec
 * returned nil for long concatenated commands). Ropes are swapped for their
 * cached flat copy in a private argument array; the caller's registers are
 * left alone and the flat copy stays owned by the rope. */
static SeaValue call_native_flat(SeaVM *vm, SeaClosure *cl, int argc, SeaValue *args) {
    SeaValue tmp[argc > 0 ? argc : 1];
    for (int i = 0; i < argc; i++)
        tmp[i] = sea_is_rope(args[i])
               ? sea_obj_val(&sea_rope_flat(SEA_AS_ROPE(args[i]))->obj) : args[i];
    return cl->native(vm, argc, tmp);
}

static inline SeaValue call_native(SeaVM *vm, SeaClosure *cl, int argc, SeaValue *args) {
    for (int i = 0; i < argc; i++)
        if (SEA_UNLIKELY(sea_is_rope(args[i])))
            return call_native_flat(vm, cl, argc, args);
    return cl->native(vm, argc, args);
}

/* Finish a call to a C function made by bytecode (docs/memory.md): the
 * arguments R[a+1..a+argc] were passed owned (+1) and natives only borrow
 * them, so they are released here; the result (+1) goes to R[a] — released at
 * once when the caller discards it (b == 0) — and extra expected results are
 * nil (C functions return one value). */
static inline void native_call_done(SeaValue *base, int a, int b, int argc, SeaValue result) {
    for (int j = 1; j <= argc; j++) vm_release_val(base[a + j]);
    if (b == 0) { vm_release_val(result); return; }
    base[a] = result;
    for (int j = 1; j < b; j++) base[a + j] = SEA_NIL_VAL;
}

/* Copy a returning frame's results R[a..a+nret-1] (each +1) into the caller's
 * R[dst..]: `expected` of them (-1 = all); results nobody takes are released,
 * missing ones are nil. */
static inline __attribute__((always_inline))
void deliver_results(SeaValue *dst, SeaValue *src, int nret, int expected) {
    if (SEA_LIKELY(nret == 1 && expected == 1)) { dst[0] = src[0]; return; }
    if (expected < 0) expected = nret;
    int n = nret < expected ? nret : expected;
    for (int j = 0; j < n; j++) dst[j] = src[j];
    for (int j = n; j < nret; j++) vm_release_val(src[j]);
    for (int j = n; j < expected; j++) dst[j] = SEA_NIL_VAL;
}

/* A returning frame owns the varargs it was passed. They live in a block of
 * their own, not past the frame's registers: a call the function makes puts
 * its callee's frame right above R[a], over that area, and the callee's
 * registers overwrote them (Bug 17 in docs/bugs.md). */
static inline void release_varargs(SeaCallFrame *f) {
    for (int j = 0; j < f->vararg_count; j++) vm_release_val(f->varargs[j]);
    SEA_FREE_N(f->varargs, SeaValue, f->vararg_count);
    f->varargs = NULL;
    f->vararg_count = 0;
}

/* First register above the running frame's window (its registers and the
 * varargs stored past them): where a re-entrant call from C may put a frame
 * without clobbering live values — e.g. the arguments of the native that is
 * calling back. */
static SeaValue *vm_free_base(SeaVM *vm) {
    if (vm->frame_count == 0) return vm->regs;
    SeaCallFrame *f = &vm->frames[vm->frame_count - 1];
    int top = f->closure && f->closure->chunk ? f->closure->chunk->max_stack : SEA_MAX_REGISTERS;
    return f->base + top;
}

/* A defer entry owns its callee and arguments (OP_DEFER). */
static inline void defer_entry_release(SeaDeferEntry *de) {
    vm_release_val(de->fn);
    for (int j = 0; j < de->argc; j++) vm_release_val(de->args[j]);
    de->fn = SEA_NIL_VAL;
    de->argc = 0;
}

/* Slow path of < <= > >= (op 0..3) when an operand is not an int, number or
 * string: a bigint compares with numbers by value; anything else is an
 * error. A NaN makes every comparison false. */
static __attribute__((noinline)) bool big_compare(SeaVM *vm, uint32_t *ip, SeaCallFrame *frame,
                                                  SeaValue a, SeaValue b, int op) {
    int c;
    int k = sea_bigint_compare(a, b, &c);
    if (k < 0 || (!sea_is_bigint(a) && !sea_is_bigint(b))) {
        frame->ip = ip;
        sea_vm_error_noreturn(vm, "attempt to compare %s with %s", sea_type_name(a), sea_type_name(b));
    }
    if (k == 0) return false;
    switch (op) {
        case 0:  return c < 0;
        case 1:  return c <= 0;
        case 2:  return c > 0;
        default: return c >= 0;
    }
}

// ---- Iterator state for for-in ----

typedef struct {
    SeaObject  obj;
    int        type;       // 0=array, 1=table, 2=channel
    uint32_t   iter;       // current position
    SeaValue   target;     // the object being iterated
} SeaIterator;

// ---- Main VM dispatch loop ----
// base_frame: the frame_count value to return at (0 for top-level, N for re-entrant calls)
/* String view of a concat operand: the string itself (borrowed, *owned=false)
 * or a fresh flat string for a rope / number / other value (*owned=true). */
static inline SeaString *concat_part(SeaVM *vm, SeaValue v, bool *owned) {
    if (sea_is_string(v)) { *owned = false; return SEA_AS_STRING(v); }
    *owned = true;
    if (sea_is_rope(v)) return sea_rope_flatten(SEA_AS_ROPE(v));   /* RC=1 */
    return sea_vm_tostring(vm, v);                                 /* RC=1 */
}

/* R[a] = R[b] .. R[b+1] .. ... (c parts) — the body of OP_CONCAT, shared with
 * the JIT's sea_jit_concat. The operands are borrowed; R[a] receives a new
 * value (+1) and its previous content is not released (a is a temporary).
 * Results longer than SEA_ROPE_THRESHOLD bytes are lazy ropes, so a later
 * OP_APPEND can extend them in place. always_inline keeps the interpreter's
 * handler as fast as when this code lived inline in vm_run. */
static inline __attribute__((always_inline))
void vm_concat(SeaVM *vm, SeaValue *base, int a, int b, int c) {
    if (c == 2) {
        bool oa, ob;
        SeaString *sa = concat_part(vm, base[b], &oa);
        SeaString *sb = concat_part(vm, base[b + 1], &ob);
        SeaValue result_val;
        if ((size_t)sa->len + sb->len > SEA_ROPE_THRESHOLD) {
            SeaRope *rope = sea_rope_new();
            sea_rope_append(rope, sa);  /* rope retains sa */
            sea_rope_append(rope, sb);  /* rope retains sb */
            result_val = sea_obj_val(&rope->obj);
        } else {
            result_val = sea_obj_val(&sea_string_concat(sa, sb)->obj);
        }
        if (oa) sea_obj_release(&sa->obj);
        if (ob) sea_obj_release(&sb->obj);
        base[a] = result_val;
        return;
    }
    /* General case: first pass to calculate total length,
     * then allocate once and copy all parts. */
    SeaString *parts[256];
    bool   owned[256];
    size_t total = 0;
    int nc = c < 256 ? c : 255;
    for (int j = 0; j < nc; j++) {
        parts[j] = concat_part(vm, base[b + j], &owned[j]);
        total += parts[j]->len;
    }
    SeaString *result = sea_string_alloc(total);
    char *p = result->data;
    for (int j = 0; j < nc; j++) {
        memcpy(p, parts[j]->data, parts[j]->len);
        p += parts[j]->len;
        if (owned[j]) sea_obj_release(&parts[j]->obj);
    }
    *p = '\0';
    base[a] = sea_obj_val(&result->obj);
}

/* R[a] = R[a] .. R[b] where R[a] is an owning local (`s = s .. x`): the old
 * value is consumed. A rope nothing else references (RC == 1) is extended in
 * place — the O(N) accumulation path; otherwise a new value replaces the old,
 * which is released. Shared with the JIT (sea_jit_append). */
static __attribute__((noinline))
void vm_append(SeaVM *vm, SeaValue *base, int a, int b) {
    SeaValue va = base[a];
    if (sea_is_rope(va) && SEA_AS_ROPE(va)->obj.refcount == 1u) {
        bool ob;
        SeaString *sb = concat_part(vm, base[b], &ob);
        sea_rope_append(SEA_AS_ROPE(va), sb);
        if (ob) sea_obj_release(&sb->obj);
        return;
    }
    SeaValue tmp[2] = { va, base[b] };
    vm_concat(vm, tmp, 0, 0, 2);      /* tmp[0] = new value */
    base[a] = tmp[0];
    vm_release_val(va);
}

/* OP_SET_FIELD_MOVE: R[a].key = R[c], taking over R[c]'s reference (+1). A
 * struct being built stores it as is; otherwise the usual retaining store,
 * then the reference is dropped. Shared with the JIT. */
static void vm_set_field_move(SeaVM *vm, SeaValue *base, int a, int c, SeaValue key) {
    SeaValue obj_v = base[a], v = base[c];
    if (sea_is_obj(obj_v) && sea_as_obj(obj_v)->type == OBJ_STRUCT) {
        SeaStruct *st = SEA_AS_STRUCT(obj_v);
        if (st->building_remaining > 0) {
            sea_struct_init_field_borrow(st, SEA_AS_STRING(key), v);
        } else if (!sea_struct_set_move(st, SEA_AS_STRING(key), v)) {
            vm_release_val(v);
            sea_vm_error_noreturn(vm, "attempt to add field '%s' to sealed struct",
                                  SEA_AS_STRING(key)->data);
        }
    } else if (sea_is_obj(obj_v) && sea_as_obj(obj_v)->type == OBJ_TABLE) {
        sea_table_set_move(SEA_AS_TABLE(obj_v), key, v);
    } else {
        table_set(vm, obj_v, key, v);   /* retains: drop ours */
        vm_release_val(v);
    }
}

/* OP_ARRAY_PUSH_MOVE: append R[b] to the array R[a], taking over its
 * reference (no retain/release pair: that would make every appended
 * container a cycle-collector candidate). Shared with the JIT. */
static inline void vm_array_push_move(SeaValue *base, int a, int b) {
    SeaValue arr_v = base[a], v = base[b];
    if (sea_is_array(arr_v)) {
        sea_array_push_move(SEA_AS_ARRAY(arr_v), v);
        return;
    }
    if (sea_is_int_array(arr_v))
        sea_int_array_push(SEA_AS_INT_ARRAY(arr_v), sea_is_int(v) ? sea_as_int(v) : (int64_t)sea_as_number(v));
    else if (sea_is_float_array(arr_v))
        sea_float_array_push(SEA_AS_FLOAT_ARRAY(arr_v), sea_is_number(v) ? sea_as_number(v) : (double)sea_as_int(v));
    else if (sea_is_float32_array(arr_v))
        sea_float32_array_push(SEA_AS_FLOAT32_ARRAY(arr_v), sea_is_number(v) ? sea_as_number(v) : (double)sea_as_int(v));
    vm_release_val(v);
}

static int vm_run_inner(SeaVM *vm, int base_frame, bool top, bool unwind);

/* Run frames until frame_count drops to base_frame (or the goroutine parks).
 * top: this is the root of the program or of a goroutine (an unrecovered
 * panic ends the process); otherwise a C caller waits for the result.
 * unwind: start by unwinding a panic (see Panics). Runtime errors raised by
 * C code called from the loop come back here through longjmp. */
static int vm_run_ex(SeaVM *vm, int base_frame, bool top, bool unwind) {
    SeaPanicJmp pj;
    void *prev = vm->panic_jmp;
    int depth = vm->native_depth;
    volatile bool vunwind = unwind;
    vm->panic_jmp = &pj;
    if (_setjmp(pj.buf)) {
        vm->panic_jmp = &pj;
        vm->native_depth = depth;     /* raised between a ++ and its -- */
        vm->io_call.active = false;   /* a native died mid-I/O */
        vunwind = true;
    }
    int r = vm_run_inner(vm, base_frame, top, vunwind);
    vm->panic_jmp = prev;
    return r;
}

static int vm_run(SeaVM *vm, int base_frame) {
    return vm_run_ex(vm, base_frame, false, false);
}

/* The dispatch loop's two hottest variables live in fixed callee-saved
 * registers, so GCC never spills them around the handlers' calls. */
#if defined(__x86_64__)
#  define SEA_PIN_REG_BASE __asm__("r13")
#  define SEA_PIN_REG_IP   __asm__("r14")
#elif defined(__aarch64__)
#  define SEA_PIN_REG_BASE __asm__("x19")
#  define SEA_PIN_REG_IP   __asm__("x20")
#else
#  define SEA_PIN_REG_BASE
#  define SEA_PIN_REG_IP
#endif

static int vm_run_inner(SeaVM *vm, int base_frame, bool top, bool unwind) {
    SeaCallFrame *frame = &vm->frames[vm->frame_count - 1];
    register SeaValue *base SEA_PIN_REG_BASE = frame->base;
    register uint32_t *ip SEA_PIN_REG_IP = frame->ip;
    SeaChunk *chunk = frame->closure->chunk;
#ifdef SEA_JIT
    uint32_t jit_ret_instr;   /* synthetic RETURN after an OSR run (jit_osr) */
#endif
    uint32_t synth_ret_instr; /* synthetic RETURN for OP_PROPAGATE's error path */

#define REG(n)   (base[(n)])
#define CONST(n) (chunk->constants[(n)])

/* SAVE_IP: write ip back to frame before any operation that might throw
 * or need the current instruction pointer (calls, errors, channel ops).
 * Pure integer fast-paths (ADD_INT, FOR_LOOP int) skip this. */
#define SAVE_IP() (frame->ip = ip)

#define DISPATCH() do { \
    uint32_t _i = *ip++; \
    goto *dispatch_table[_i & 0xFF]; \
} while(0)

/* JIT fast-call hook (see docs/feature_jit.md). Used inside the OP_CALL /
 * OP_CALL_SELF fast paths AFTER the callee frame `nf` is fully initialized but
 * BEFORE switching the interpreter over to it. If the callee chunk has native
 * code (or becomes hot enough to compile), run it to completion and finish the
 * call right here — teardown mirrors lbl_RETURN. On native completion control
 * never returns to the macro (DISPATCH resumes the caller); on bailout or when
 * no native code is available, execution falls through to the interpreter.
 * `_a` is the caller's result register; `base`/`ip`/`chunk`/`frame` stay the
 * caller's throughout. Only reached when vm->jit_enabled. */
#define JIT_TRY_CALL(_chunk, _nf, _a) do { \
    void *_jf = SEA_JIT_LOAD_FN(_chunk); \
    if (_jf == NULL && ++(_chunk)->exec_count >= (uint32_t)sea_jit_hot_calls) \
        _jf = sea_jit_request(vm, (_chunk)); \
    if (SEA_JIT_IS_CODE(_jf)) { \
        int _nret = ((SeaJittedFn)_jf)(vm, (_nf)); \
        if (_nret == SEA_JIT_SUSPEND) return SEA_SUSPENDED; /* nf->ip = resume point */ \
        /* a panic escaped a call made by the native code: it bailed (spilled \
         * its frame) and the unwinding continues here */ \
        if (SEA_UNLIKELY(_nret == SEA_JIT_BAILOUT && vm->panic_propagating)) goto panic_unwind; \
        if (_nret != SEA_JIT_BAILOUT) { \
            /* native code may have created closures over nf's registers */ \
            if (vm->open_upvalues) close_upvalues(vm, (_nf)->base); \
            vm->frame_arena_used = (_nf)->arena_mark; \
            vm->frame_count--; \
            deliver_results(&base[(_a)], (_nf)->base, _nret, (_nf)->expected_ret); \
            DISPATCH(); \
        } \
    } \
} while(0)

/* Loop back-edge hook, placed right after a backward jump has been taken (ip
 * already points at the loop head). Counts down chunk->jit_hot; when it drops
 * below zero, jit_osr decides whether to queue the chunk for compilation or,
 * once native code exists, to enter it at this loop head (OSR). Compiled out
 * of non-JIT builds. */
#ifdef SEA_JIT
#define JIT_BACKEDGE() do { \
    if (SEA_UNLIKELY(--chunk->jit_hot < 0)) goto jit_osr; \
} while(0)
#else
#define JIT_BACKEDGE() ((void)0)
#endif

/* Sandbox budget: one step per loop back-edge and per Sea call. A normal VM
 * pays one branch on a flag that is never set. */
#define BUDGET_STEP() do { \
    if (SEA_UNLIKELY(vm->sandboxed) && SEA_UNLIKELY(--vm->budget < 0)) { SAVE_IP(); sea_vm_budget_exceeded(vm); } \
} while (0)

/* Cycle-collector safe point (sea_gc.h): between instructions, where no C
 * code holds borrowed pointers. Placed at allocations and returns. */
#define GC_SAFEPOINT() do { \
    if (SEA_UNLIKELY(vm->gc_nroots > vm->gc_threshold)) { SAVE_IP(); sea_gc_collect(vm); } \
} while (0)

/* Run all deferred calls registered for the current frame (LIFO order),
 * then restore the defer stack pointer. Must be called before any RETURN. */
#define RUN_DEFERS() do { \
    while (vm->defer_count > frame->defer_base) { \
        SeaDeferEntry _de = vm->defers[--vm->defer_count]; \
        if (sea_is_obj(_de.fn)) { \
            frame->ip = ip; \
            vm_release_val(sea_vm_call(vm, _de.fn, _de.args, _de.argc)); \
            frame = &vm->frames[vm->frame_count - 1]; \
            base  = frame->base; \
            ip    = frame->ip; \
            chunk = frame->closure->chunk; \
        } \
        defer_entry_release(&_de); \
        /* the deferred call panicked: unwind this frame (its other defers too) */ \
        if (SEA_UNLIKELY(vm->panic_propagating)) goto panic_unwind; \
    } \
} while(0)

    static void *dispatch_table[OP_COUNT] = {
        [OP_LOAD_NIL]     = &&lbl_LOAD_NIL,
        [OP_LOAD_TRUE]    = &&lbl_LOAD_TRUE,
        [OP_LOAD_FALSE]   = &&lbl_LOAD_FALSE,
        [OP_LOAD_INT]     = &&lbl_LOAD_INT,
        [OP_LOAD_K]       = &&lbl_LOAD_K,
        [OP_MOVE]         = &&lbl_MOVE,
        [OP_LOAD_GLOBAL]  = &&lbl_LOAD_GLOBAL,
        [OP_STORE_GLOBAL] = &&lbl_STORE_GLOBAL,
        [OP_LOAD_UP]      = &&lbl_LOAD_UP,
        [OP_STORE_UP]     = &&lbl_STORE_UP,
        [OP_CLOSE_UP]     = &&lbl_CLOSE_UP,
        [OP_ADD]          = &&lbl_ADD,
        [OP_SUB]          = &&lbl_SUB,
        [OP_MUL]          = &&lbl_MUL,
        [OP_DIV]          = &&lbl_DIV,
        [OP_IDIV]         = &&lbl_IDIV,
        [OP_MOD]          = &&lbl_MOD,
        [OP_POW]          = &&lbl_POW,
        [OP_NEG]          = &&lbl_NEG,
        [OP_UNM]          = &&lbl_NEG,
        [OP_ADD_INT]      = &&lbl_ADD_INT,
        [OP_SUB_INT]      = &&lbl_SUB_INT,
        [OP_IIADD]        = &&lbl_IIADD,
        [OP_IISUB]        = &&lbl_IISUB,
        [OP_ADDI]         = &&lbl_ADDI,
        [OP_SUBI]         = &&lbl_SUBI,
        [OP_MUL_INT]      = &&lbl_MUL_INT,
        [OP_IDIV_INT]     = &&lbl_IDIV_INT,
        [OP_MOD_INT]      = &&lbl_MOD_INT,
        [OP_BAND]         = &&lbl_BAND,
        [OP_BOR]          = &&lbl_BOR,
        [OP_BXOR]         = &&lbl_BXOR,
        [OP_BNOT]         = &&lbl_BNOT,
        [OP_SHL]          = &&lbl_SHL,
        [OP_SHR]          = &&lbl_SHR,
        [OP_CONCAT]       = &&lbl_CONCAT,
        [OP_EQ]           = &&lbl_EQ,
        [OP_NEQ]          = &&lbl_NEQ,
        [OP_LT]           = &&lbl_LT,
        [OP_LE]           = &&lbl_LE,
        [OP_GT]           = &&lbl_GT,
        [OP_GE]           = &&lbl_GE,
        [OP_NOT]          = &&lbl_NOT,
        [OP_LTI]          = &&lbl_LTI,
        [OP_LEI]          = &&lbl_LEI,
        [OP_GTI]          = &&lbl_GTI,
        [OP_GEI]          = &&lbl_GEI,
        [OP_JMP]          = &&lbl_JMP,
        [OP_JMP_TRUE]     = &&lbl_JMP_TRUE,
        [OP_JMP_FALSE]    = &&lbl_JMP_FALSE,
        [OP_JMP_NIL]      = &&lbl_JMP_NIL,
        [OP_NEW_TABLE]    = &&lbl_NEW_TABLE,
        [OP_GET_TABLE]    = &&lbl_GET_TABLE,
        [OP_GET_TABLE2]   = &&lbl_GET_TABLE2,
        [OP_SET_TABLE]    = &&lbl_SET_TABLE,
        [OP_GET_FIELD]    = &&lbl_GET_FIELD,
        [OP_SET_FIELD]    = &&lbl_SET_FIELD,
        [OP_NEW_STRUCT]   = &&lbl_NEW_STRUCT,
        [OP_NEW_ARRAY]    = &&lbl_NEW_ARRAY,
        [OP_GET_ARRAY]    = &&lbl_GET_ARRAY,
        [OP_SET_ARRAY]    = &&lbl_SET_ARRAY,
        [OP_ARRAY_PUSH]   = &&lbl_ARRAY_PUSH,
        [OP_GET_IARRAY]   = &&lbl_GET_IARRAY,
        [OP_SET_IARRAY]   = &&lbl_SET_IARRAY,
        [OP_GET_FARRAY]   = &&lbl_GET_FARRAY,
        [OP_SET_FARRAY]   = &&lbl_SET_FARRAY,
        [OP_CLOSURE]      = &&lbl_CLOSURE,
        [OP_CALL]         = &&lbl_CALL,
        [OP_CALL_SELF]    = &&lbl_CALL_SELF,
        [OP_CALL_MULTI]   = &&lbl_CALL_MULTI,
        [OP_RETURN]       = &&lbl_RETURN,
        [OP_RETURN0]      = &&lbl_RETURN0,
        [OP_TAIL_CALL]    = &&lbl_TAIL_CALL,
        [OP_VARARG]       = &&lbl_VARARG,
        [OP_FOR_PREP]        = &&lbl_FOR_PREP,
        [OP_FOR_LOOP]        = &&lbl_FOR_LOOP,
        [OP_FOR_LOOP_STEP1]  = &&lbl_FOR_LOOP_STEP1,
        [OP_FOR_LOOP_INT]    = &&lbl_FOR_LOOP_INT,
        [OP_FOR_IN_INIT]     = &&lbl_FOR_IN_INIT,
        [OP_FOR_IN_NEXT]  = &&lbl_FOR_IN_NEXT,
        [OP_NEW_CHAN]      = &&lbl_NEW_CHAN,
        [OP_CHAN_SEND]     = &&lbl_CHAN_SEND,
        [OP_CHAN_RECV]     = &&lbl_CHAN_RECV,
        [OP_CHAN_CLOSE]    = &&lbl_CHAN_CLOSE,
        [OP_CHAN_TRY_SEND] = &&lbl_CHAN_TRY_SEND,
        [OP_CHAN_TRY_RECV] = &&lbl_CHAN_TRY_RECV,
        [OP_GO]            = &&lbl_GO,
        [OP_YIELD]         = &&lbl_YIELD,
        [OP_SELECT_BEGIN]  = &&lbl_SELECT_BEGIN,
        [OP_SELECT_CASE]   = &&lbl_SELECT_CASE,
        [OP_SELECT_END]    = &&lbl_SELECT_END,
        [OP_NEW_ERROR]     = &&lbl_NEW_ERROR,
        [OP_IS_ERROR]      = &&lbl_IS_ERROR,
        [OP_ERROR_MSG]     = &&lbl_ERROR_MSG,
        [OP_PROPAGATE]     = &&lbl_PROPAGATE,
        [OP_DEFER]         = &&lbl_DEFER,
        [OP_PANIC]         = &&lbl_PANIC,
        [OP_RECOVER]       = &&lbl_RECOVER,
        [OP_TYPE]          = &&lbl_TYPE,
        [OP_TOSTRING]      = &&lbl_TOSTRING,
        [OP_TONUMBER]      = &&lbl_TONUMBER,
        [OP_PRINT]         = &&lbl_PRINT,
        [OP_ASSERT]        = &&lbl_ASSERT,
        [OP_RELEASE]             = &&lbl_RELEASE,
        [OP_NEW_STRUCT_LOCAL]    = &&lbl_NEW_STRUCT_LOCAL,
        [OP_SET_FIELD_BORROW]    = &&lbl_SET_FIELD_BORROW,
        [OP_NEW_ARRAY_LOCAL]     = &&lbl_NEW_ARRAY_LOCAL,
        [OP_NEW_MAP]             = &&lbl_NEW_MAP,
        [OP_DEL]                 = &&lbl_DEL,
        [OP_MARK_CAPTURED]       = &&lbl_MARK_CAPTURED,
        [OP_RETAIN]              = &&lbl_RETAIN,
        [OP_SETLOCAL]            = &&lbl_SETLOCAL,
        [OP_APPEND]              = &&lbl_APPEND,
        [OP_JMP_NOT_ERR]         = &&lbl_JMP_NOT_ERR,
        [OP_LOAD_SELF]           = &&lbl_LOAD_SELF,
        [OP_SET_FIELD_MOVE]      = &&lbl_SET_FIELD_MOVE,
        [OP_ARRAY_PUSH_MOVE]     = &&lbl_ARRAY_PUSH_MOVE,
        [OP_COVER]               = &&lbl_COVER,
    };
    if (SEA_UNLIKELY(g_type_check)) {   /* diagnostic mode: verify int inference */
        dispatch_table[OP_IIADD] = &&lbl_IIADD_CHECKED;
        dispatch_table[OP_IISUB] = &&lbl_IISUB_CHECKED;
    }

    if (SEA_UNLIKELY(unwind)) goto panic_unwind;
    DISPATCH();

    lbl_LOAD_NIL: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        for (int j = 0; j < b; j++) REG(a+j) = SEA_NIL_VAL;
        DISPATCH();
    }
    lbl_LOAD_TRUE: {
        uint32_t i = ip[-1];
        REG(SEA_INSTR_A(i)) = SEA_TRUE_VAL;
        DISPATCH();
    }
    lbl_LOAD_FALSE: {
        uint32_t i = ip[-1];
        REG(SEA_INSTR_A(i)) = SEA_FALSE_VAL;
        DISPATCH();
    }
    lbl_LOAD_INT: {
        uint32_t i = ip[-1];
        int sbx = SEA_INSTR_sBx(i);
        REG(SEA_INSTR_A(i)) = sea_int_val((int64_t)sbx);
        DISPATCH();
    }
    lbl_LOAD_K: {
        uint32_t i = ip[-1];
        REG(SEA_INSTR_A(i)) = CONST(SEA_INSTR_Bx(i));
        DISPATCH();
    }
    lbl_MOVE: {
        uint32_t i = ip[-1];
        REG(SEA_INSTR_A(i)) = REG(SEA_INSTR_B(i));
        DISPATCH();
    }
    lbl_LOAD_GLOBAL: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), bx = SEA_INSTR_Bx(i);
        SeaValue key = CONST(bx);
        SeaValue val = SEA_NIL_VAL;
        sea_table_get_ic_sync(vm_globals(vm), &vm->shared->globals_sync, key,
                              (SeaTableIC*)&sea_chunk_ics(chunk)[ip - 1 - chunk->code], &val);
        REG(a) = val;
        DISPATCH();
    }
    lbl_STORE_GLOBAL: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), bx = SEA_INSTR_Bx(i);
        SeaValue key = CONST(bx);
        sea_val_mark_shared(REG(a));
        sea_globals_set(vm, key, REG(a));
        DISPATCH();
    }
    lbl_LOAD_UP: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        REG(a) = sea_upvalue_get(frame->closure->upvalues[b]);   /* see close_upvalues */
        DISPATCH();
    }
    lbl_STORE_UP: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SeaUpvalue *up = frame->closure->upvalues[b];
        /* A value stored into an upvalue other goroutines can see must be
         * shared too (same rule as channel sends / global writes). */
        /* the upvalue (or, while open, the captured local) owns its value */
        vm_retain_val(REG(a));
        SeaValue old;
        if (SEA_UNLIKELY(__atomic_load_n(&up->obj.refcount, __ATOMIC_RELAXED) & SEA_RC_SHARED_BIT)) {
            sea_val_mark_shared(REG(a));
            /* other goroutines may store into it, or close it, at the same
             * time: under the lock each old value is released exactly once */
            sea_upvalue_lock(up);
            SeaValue *slot = up->location;
            old = *slot;
            __atomic_store_n(slot, REG(a), __ATOMIC_RELEASE);
            sea_upvalue_unlock(up);
        } else {
            SeaValue *slot = up->location;
            old = *slot;
            *slot = REG(a);
        }
        vm_release_val(old);
        DISPATCH();
    }
    lbl_CLOSE_UP: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        close_upvalues(vm, &base[a]);
        DISPATCH();
    }

    // ---- Arithmetic ----

    lbl_ADD: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue vb = REG(b), vc = REG(c);
        if (SEA_LIKELY(sea_is_number(vb) && sea_is_number(vc)))
            REG(a) = sea_number_val(sea_as_number(vb) + sea_as_number(vc));
        else if (sea_is_int(vb) && sea_is_int(vc))   /* as val_add, without the call */
            REG(a) = sea_int_val(sea_as_int(vb) + sea_as_int(vc));
        else { SAVE_IP(); REG(a) = val_add(vm, vb, vc); }
        DISPATCH();
    }
    lbl_SUB: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue vb = REG(b), vc = REG(c);
        if (SEA_LIKELY(sea_is_number(vb) && sea_is_number(vc)))
            REG(a) = sea_number_val(sea_as_number(vb) - sea_as_number(vc));
        else if (sea_is_int(vb) && sea_is_int(vc))   /* as val_sub, without the call */
            REG(a) = sea_int_val(sea_as_int(vb) - sea_as_int(vc));
        else { SAVE_IP(); REG(a) = val_sub(vm, vb, vc); }
        DISPATCH();
    }
    lbl_MUL: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue vb = REG(b), vc = REG(c);
        if (SEA_LIKELY(sea_is_number(vb) && sea_is_number(vc)))
            REG(a) = sea_number_val(sea_as_number(vb) * sea_as_number(vc));
        else if (sea_is_int(vb) && sea_is_int(vc))   /* as val_mul, without the call */
            REG(a) = sea_int_val(sea_as_int(vb) * sea_as_int(vc));
        else { SAVE_IP(); REG(a) = val_mul(vm, vb, vc); }
        DISPATCH();
    }
    lbl_DIV: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SAVE_IP();
        double na = to_number(vm, REG(b));
        double nb = to_number(vm, REG(c));
        REG(a) = sea_number_val(na / nb);
        DISPATCH();
    }
    lbl_IDIV: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SAVE_IP();
        if (sea_is_int(REG(b)) && sea_is_int(REG(c))) {
            int64_t nb = sea_as_int(REG(c));
            if (nb == 0) sea_vm_error_noreturn(vm, "division by zero");
            REG(a) = sea_int_val(floor_div(sea_as_int(REG(b)), nb));
        } else {
            double nb = to_number(vm, REG(c));
            REG(a) = sea_number_val(floor(to_number(vm, REG(b)) / nb));
        }
        DISPATCH();
    }
    lbl_MOD: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SAVE_IP();
        if (sea_is_int(REG(b)) && sea_is_int(REG(c))) {
            int64_t nc = sea_as_int(REG(c));
            if (nc == 0) sea_vm_error_noreturn(vm, "division by zero");
            int64_t r = sea_as_int(REG(b)) % nc;
            if (r != 0 && (r ^ nc) < 0) r += nc;
            REG(a) = sea_int_val(r);
        } else {
            double nb = to_number(vm, REG(b));
            double nc = to_number(vm, REG(c));
            double r = fmod(nb, nc);
            if (r != 0.0 && (r < 0) != (nc < 0)) r += nc;
            REG(a) = sea_number_val(r);
        }
        DISPATCH();
    }
    lbl_POW: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SAVE_IP();
        REG(a) = sea_number_val(pow(to_number(vm, REG(b)), to_number(vm, REG(c))));
        DISPATCH();
    }
    lbl_NEG: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SeaValue v = REG(b);
        if (sea_is_int(v)) REG(a) = sea_int_val(-sea_as_int(v));
        else { SAVE_IP(); REG(a) = sea_number_val(-to_number(vm, v)); }
        DISPATCH();
    }
    lbl_ADD_INT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue vb = REG(b), vc = REG(c);
        if (SEA_LIKELY(sea_is_int(vb) && sea_is_int(vc)))
            REG(a) = sea_int_val(sea_as_int(vb) + sea_as_int(vc));
        else { SAVE_IP(); REG(a) = val_add(vm, vb, vc); }
        DISPATCH();
    }
    lbl_SUB_INT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue vb = REG(b), vc = REG(c);
        if (SEA_LIKELY(sea_is_int(vb) && sea_is_int(vc)))
            REG(a) = sea_int_val(sea_as_int(vb) - sea_as_int(vc));
        else { SAVE_IP(); REG(a) = val_sub(vm, vb, vc); }
        DISPATCH();
    }
    lbl_IIADD: {
        /* Compiler-proven int+int: 48-bit addition via mask — avoids the 4
         * arithmetic shifts of sea_as_int/sea_int_val; result is identical.
         * No type check: the compiler's int inference must be sound (see
         * `is_int` dataflow in sea_compiler.c). SEA_TYPE_CHECK=1 verifies it. */
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        uint64_t bv = REG(b) & SEA_PTR_MASK;
        uint64_t cv = REG(c) & SEA_PTR_MASK;
        REG(a) = (SEA_NAN_BASE | SEA_TAG_INT) | ((bv + cv) & SEA_PTR_MASK);
        DISPATCH();
    }
    /* SEA_TYPE_CHECK=1: the dispatch table routes IIADD/IISUB here first (see
     * vm_run's prologue), so the normal handlers carry no check at all. */
    lbl_IIADD_CHECKED: {
        uint32_t i = ip[-1];
        type_check_ints(vm, frame, ip, REG(SEA_INSTR_B(i)), REG(SEA_INSTR_C(i)));
        goto lbl_IIADD;
    }
    lbl_IISUB_CHECKED: {
        uint32_t i = ip[-1];
        type_check_ints(vm, frame, ip, REG(SEA_INSTR_B(i)), REG(SEA_INSTR_C(i)));
        goto lbl_IISUB;
    }
    lbl_IISUB: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        uint64_t bv = REG(b) & SEA_PTR_MASK;
        uint64_t cv = REG(c) & SEA_PTR_MASK;
        REG(a) = (SEA_NAN_BASE | SEA_TAG_INT) | ((bv - cv) & SEA_PTR_MASK);
        DISPATCH();
    }
    lbl_ADDI: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        int8_t imm = (int8_t)SEA_INSTR_C(i);
        SeaValue vb = REG(b);
        if (SEA_LIKELY(sea_is_int(vb)))
            REG(a) = sea_int_val(sea_as_int(vb) + imm);
        else { SAVE_IP(); REG(a) = val_add(vm, vb, sea_int_val(imm)); }
        DISPATCH();
    }
    lbl_SUBI: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        int8_t imm = (int8_t)SEA_INSTR_C(i);
        SeaValue vb = REG(b);
        if (SEA_LIKELY(sea_is_int(vb)))
            REG(a) = sea_int_val(sea_as_int(vb) - imm);
        else { SAVE_IP(); REG(a) = val_sub(vm, vb, sea_int_val(imm)); }
        DISPATCH();
    }
    lbl_MUL_INT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue vb = REG(b), vc = REG(c);
        if (SEA_LIKELY(sea_is_int(vb) && sea_is_int(vc)))
            REG(a) = sea_int_val(sea_as_int(vb) * sea_as_int(vc));
        else { SAVE_IP(); REG(a) = val_mul(vm, vb, vc); }
        DISPATCH();
    }
    lbl_IDIV_INT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue vb = REG(b), vc = REG(c);
        if (SEA_LIKELY(sea_is_int(vb) && sea_is_int(vc))) {
            int64_t nc = sea_as_int(vc);
            if (SEA_UNLIKELY(nc == 0)) { SAVE_IP(); sea_vm_error_noreturn(vm, "division by zero"); }
            REG(a) = sea_int_val(floor_div(sea_as_int(vb), nc));
        } else {
            SAVE_IP();
            double nb = to_number(vm, vb);
            double nc = to_number(vm, vc);
            REG(a) = sea_number_val(floor(nb / nc));
        }
        DISPATCH();
    }
    lbl_MOD_INT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue vb = REG(b), vc = REG(c);
        if (SEA_LIKELY(sea_is_int(vb) && sea_is_int(vc))) {
            int64_t nc = sea_as_int(vc);
            if (SEA_UNLIKELY(nc == 0)) { SAVE_IP(); sea_vm_error_noreturn(vm, "division by zero"); }
            int64_t r = sea_as_int(vb) % nc;
            if (r != 0 && (r ^ nc) < 0) r += nc;
            REG(a) = sea_int_val(r);
        } else {
            SAVE_IP();
            double nb = to_number(vm, vb);
            double nc = to_number(vm, vc);
            double r = fmod(nb, nc);
            if (r != 0.0 && (r < 0) != (nc < 0)) r += nc;
            REG(a) = sea_number_val(r);
        }
        DISPATCH();
    }

    // ---- Bitwise ----

    lbl_BAND: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SAVE_IP();
        REG(a) = sea_int_val(to_int(vm, REG(b)) & to_int(vm, REG(c)));
        DISPATCH();
    }
    lbl_BOR: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SAVE_IP();
        REG(a) = sea_int_val(to_int(vm, REG(b)) | to_int(vm, REG(c)));
        DISPATCH();
    }
    lbl_BXOR: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SAVE_IP();
        REG(a) = sea_int_val(to_int(vm, REG(b)) ^ to_int(vm, REG(c)));
        DISPATCH();
    }
    lbl_BNOT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SAVE_IP();
        REG(a) = sea_int_val(~to_int(vm, REG(b)));
        DISPATCH();
    }
    lbl_SHL: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SAVE_IP();
        int64_t shift = to_int(vm, REG(c));
        if (shift >= 64 || shift <= -64) { REG(a) = sea_int_val(0); }
        /* left shifts on uint64_t: a signed << that overflows is UB in C */
        else if (shift >= 0) REG(a) = sea_int_val((int64_t)((uint64_t)to_int(vm, REG(b)) << shift));
        else REG(a) = sea_int_val((int64_t)((uint64_t)to_int(vm, REG(b)) >> (-shift)));
        DISPATCH();
    }
    lbl_SHR: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SAVE_IP();
        int64_t shift = to_int(vm, REG(c));
        if (shift >= 64 || shift <= -64) { REG(a) = sea_int_val(0); }
        else if (shift >= 0) REG(a) = sea_int_val((int64_t)((uint64_t)to_int(vm, REG(b)) >> shift));
        else REG(a) = sea_int_val((int64_t)((uint64_t)to_int(vm, REG(b)) << (-shift)));
        DISPATCH();
    }

    // ---- String ----

    lbl_CONCAT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SAVE_IP();
        vm_concat(vm, base, a, b, c);
        DISPATCH();
    }

    // ---- Comparisons ----

    lbl_EQ: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue vb = REG(b), vc = REG(c);
        SeaString *sb = str_of(vb), *sc = sb ? str_of(vc) : NULL;
        bool eq = sc ? sea_string_equal(sb, sc) : sea_val_equal(vb, vc);
        REG(a) = sea_bool_val(eq);
        DISPATCH();
    }
    lbl_NEQ: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue vb = REG(b), vc = REG(c);
        SeaString *sb = str_of(vb), *sc = sb ? str_of(vc) : NULL;
        bool eq = sc ? sea_string_equal(sb, sc) : sea_val_equal(vb, vc);
        REG(a) = sea_bool_val(!eq);
        DISPATCH();
    }
    lbl_LT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue lv = REG(b), rv = REG(c);
        bool res;
        if (sea_is_int(lv) && sea_is_int(rv))
            res = sea_as_int(lv) < sea_as_int(rv);
        else if ((sea_is_number(lv) || sea_is_int(lv)) && (sea_is_number(rv) || sea_is_int(rv)))
            res = to_number(vm, lv) < to_number(vm, rv);
        else if (str_of(lv) && str_of(rv))
            res = strcmp(str_of(lv)->data, str_of(rv)->data) < 0;
        else res = big_compare(vm, ip, frame, lv, rv, 0);
        REG(a) = sea_bool_val(res);
        DISPATCH();
    }
    lbl_LE: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue lv = REG(b), rv = REG(c);
        bool res;
        if (sea_is_int(lv) && sea_is_int(rv))
            res = sea_as_int(lv) <= sea_as_int(rv);
        else if ((sea_is_number(lv) || sea_is_int(lv)) && (sea_is_number(rv) || sea_is_int(rv)))
            res = to_number(vm, lv) <= to_number(vm, rv);
        else if (str_of(lv) && str_of(rv))
            res = strcmp(str_of(lv)->data, str_of(rv)->data) <= 0;
        else res = big_compare(vm, ip, frame, lv, rv, 1);
        REG(a) = sea_bool_val(res);
        DISPATCH();
    }
    lbl_GT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue lv = REG(b), rv = REG(c);
        bool res;
        if (sea_is_int(lv) && sea_is_int(rv))
            res = sea_as_int(lv) > sea_as_int(rv);
        else if ((sea_is_number(lv) || sea_is_int(lv)) && (sea_is_number(rv) || sea_is_int(rv)))
            res = to_number(vm, lv) > to_number(vm, rv);
        else if (str_of(lv) && str_of(rv))
            res = strcmp(str_of(lv)->data, str_of(rv)->data) > 0;
        else res = big_compare(vm, ip, frame, lv, rv, 2);
        REG(a) = sea_bool_val(res);
        DISPATCH();
    }
    lbl_GE: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue lv = REG(b), rv = REG(c);
        bool res;
        if (sea_is_int(lv) && sea_is_int(rv))
            res = sea_as_int(lv) >= sea_as_int(rv);
        else if ((sea_is_number(lv) || sea_is_int(lv)) && (sea_is_number(rv) || sea_is_int(rv)))
            res = to_number(vm, lv) >= to_number(vm, rv);
        else if (str_of(lv) && str_of(rv))
            res = strcmp(str_of(lv)->data, str_of(rv)->data) >= 0;
        else res = big_compare(vm, ip, frame, lv, rv, 3);
        REG(a) = sea_bool_val(res);
        DISPATCH();
    }
    lbl_LTI: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        int8_t imm = (int8_t)SEA_INSTR_C(i);
        SeaValue vb = REG(b);
        bool res;
        if (SEA_LIKELY(sea_is_int(vb))) res = sea_as_int(vb) < imm;
        else if (sea_is_number(vb)) res = sea_as_number(vb) < (double)imm;
        else res = big_compare(vm, ip, frame, vb, sea_int_val(imm), 0);
        REG(a) = sea_bool_val(res);
        DISPATCH();
    }
    lbl_LEI: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        int8_t imm = (int8_t)SEA_INSTR_C(i);
        SeaValue vb = REG(b);
        bool res;
        if (SEA_LIKELY(sea_is_int(vb))) res = sea_as_int(vb) <= imm;
        else if (sea_is_number(vb)) res = sea_as_number(vb) <= (double)imm;
        else res = big_compare(vm, ip, frame, vb, sea_int_val(imm), 1);
        REG(a) = sea_bool_val(res);
        DISPATCH();
    }
    lbl_GTI: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        int8_t imm = (int8_t)SEA_INSTR_C(i);
        SeaValue vb = REG(b);
        bool res;
        if (SEA_LIKELY(sea_is_int(vb))) res = sea_as_int(vb) > imm;
        else if (sea_is_number(vb)) res = sea_as_number(vb) > (double)imm;
        else res = big_compare(vm, ip, frame, vb, sea_int_val(imm), 2);
        REG(a) = sea_bool_val(res);
        DISPATCH();
    }
    lbl_GEI: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        int8_t imm = (int8_t)SEA_INSTR_C(i);
        SeaValue vb = REG(b);
        bool res;
        if (SEA_LIKELY(sea_is_int(vb))) res = sea_as_int(vb) >= imm;
        else if (sea_is_number(vb)) res = sea_as_number(vb) >= (double)imm;
        else res = big_compare(vm, ip, frame, vb, sea_int_val(imm), 3);
        REG(a) = sea_bool_val(res);
        DISPATCH();
    }
    lbl_NOT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        REG(a) = sea_bool_val(!sea_is_truthy(REG(b)));
        DISPATCH();
    }

    // ---- Jumps ----

    lbl_JMP: {
        uint32_t i = ip[-1];
        int sbx = SEA_INSTR_sBx(i);
        ip += sbx;
        if (sbx < 0) { BUDGET_STEP(); JIT_BACKEDGE(); }
        DISPATCH();
    }
    lbl_JMP_TRUE: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), sbx = SEA_INSTR_sBx(i);
        if (sea_is_truthy(REG(a))) ip += sbx;
        DISPATCH();
    }
    lbl_JMP_FALSE: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), sbx = SEA_INSTR_sBx(i);
        if (!sea_is_truthy(REG(a))) ip += sbx;
        DISPATCH();
    }
    lbl_JMP_NIL: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), sbx = SEA_INSTR_sBx(i);
        if (sea_is_nil(REG(a))) ip += sbx;
        DISPATCH();
    }

    // ---- Tables ----

    lbl_NEW_TABLE: {
        GC_SAFEPOINT();
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SeaTable *t = sea_table_new(b ? TABLE_STRUCT : TABLE_MAP);
        REG(a) = sea_obj_val(&t->obj);
        DISPATCH();
    }
    lbl_GET_TABLE: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue _obj = REG(b), _key = REG(c);
        if (SEA_LIKELY(sea_is_array(_obj) && sea_is_int(_key))) {
            SeaArray *_arr = SEA_AS_ARRAY(_obj);
            int64_t  _idx  = sea_as_int(_key);
            if (SEA_LIKELY(_idx >= 0 && (uint64_t)_idx < _arr->len)) {
                REG(a) = _arr->data[_idx];
                DISPATCH();
            }
        } else if (sea_is_obj(_obj) && sea_is_int(_key)) {
            /* int_array / float_array, in range: no refcounting involved */
            SeaObject *_o = sea_as_obj(_obj);
            int64_t _idx = sea_as_int(_key);
            if (_o->type == OBJ_FLOAT_ARRAY) {
                SeaFloatArray *_fa = (SeaFloatArray*)_o;
                if (SEA_LIKELY(_idx >= 0 && (uint64_t)_idx < _fa->len)) {
                    sea_chunk_feedback_typed(chunk, (int)(ip - 1 - chunk->code));
                    REG(a) = sea_number_val(_fa->data[_idx]);
                    DISPATCH();
                }
            } else if (_o->type == OBJ_INT_ARRAY) {
                SeaIntArray *_ia = (SeaIntArray*)_o;
                if (SEA_LIKELY(_idx >= 0 && (uint64_t)_idx < _ia->len)) {
                    sea_chunk_feedback_typed(chunk, (int)(ip - 1 - chunk->code));
                    REG(a) = sea_int_val(_ia->data[_idx]);
                    DISPATCH();
                }
            } else if (_o->type == OBJ_FLOAT32_ARRAY) {
                SeaFloat32Array *_f3 = (SeaFloat32Array*)_o;
                if (SEA_LIKELY(_idx >= 0 && (uint64_t)_idx < _f3->len)) {
                    /* "other" for the JIT: its inline typed path reads doubles */
                    sea_chunk_feedback(chunk, (int)(ip - 1 - chunk->code), _obj);
                    REG(a) = sea_number_val((double)_f3->data[_idx]);
                    DISPATCH();
                }
            }
        }
        SAVE_IP();
        sea_chunk_feedback(chunk, (int)(ip - 1 - chunk->code), _obj);   /* for the JIT */
        REG(a) = table_get(vm, _obj, _key);
        DISPATCH();
    }
    lbl_GET_TABLE2: {
        uint32_t i = ip[-1];
        vm_get_table2(base, SEA_INSTR_A(i), SEA_INSTR_B(i), SEA_INSTR_C(i));
        DISPATCH();
    }
    lbl_SET_TABLE: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue _obj = REG(a), _key = REG(b), _val = REG(c);
        if (SEA_LIKELY(sea_is_array(_obj) && sea_is_int(_key))) {
            SeaArray *_arr = SEA_AS_ARRAY(_obj);
            int64_t  _idx  = sea_as_int(_key);
            if (SEA_LIKELY(_idx >= 0 && (uint64_t)_idx < _arr->len)) {
                SeaValue _old = _arr->data[_idx];
                sea_store_barrier(&_arr->obj, _val);
                vm_retain_val(_val);
                _arr->data[_idx] = _val;
                vm_release_val(_old);
                DISPATCH();
            }
        } else if (sea_is_obj(_obj) && sea_is_int(_key)) {
            SeaObject *_o = sea_as_obj(_obj);
            int64_t _idx = sea_as_int(_key);
            if (_o->type == OBJ_FLOAT_ARRAY && (sea_is_number(_val) || sea_is_int(_val))) {
                SeaFloatArray *_fa = (SeaFloatArray*)_o;
                if (SEA_LIKELY(_idx >= 0 && (uint64_t)_idx < _fa->len)) {
                    sea_chunk_feedback_typed(chunk, (int)(ip - 1 - chunk->code));
                    _fa->data[_idx] = sea_is_number(_val) ? sea_as_number(_val)
                                                          : (double)sea_as_int(_val);
                    DISPATCH();
                }
            } else if (_o->type == OBJ_INT_ARRAY && (sea_is_int(_val) || sea_is_number(_val))) {
                SeaIntArray *_ia = (SeaIntArray*)_o;
                if (SEA_LIKELY(_idx >= 0 && (uint64_t)_idx < _ia->len)) {
                    sea_chunk_feedback_typed(chunk, (int)(ip - 1 - chunk->code));
                    _ia->data[_idx] = sea_is_int(_val) ? sea_as_int(_val)
                                                       : (int64_t)sea_as_number(_val);
                    DISPATCH();
                }
            } else if (_o->type == OBJ_FLOAT32_ARRAY && (sea_is_number(_val) || sea_is_int(_val))) {
                SeaFloat32Array *_f3 = (SeaFloat32Array*)_o;
                if (SEA_LIKELY(_idx >= 0 && (uint64_t)_idx < _f3->len)) {
                    sea_chunk_feedback(chunk, (int)(ip - 1 - chunk->code), _obj);   /* "other" for the JIT */
                    _f3->data[_idx] = (float)(sea_is_number(_val) ? sea_as_number(_val)
                                                                  : (double)sea_as_int(_val));
                    DISPATCH();
                }
            }
        }
        SAVE_IP();
        sea_chunk_feedback(chunk, (int)(ip - 1 - chunk->code), _obj);   /* for the JIT */
        table_set(vm, _obj, _key, _val);
        DISPATCH();
    }
    lbl_GET_FIELD: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue obj_v = REG(b);
        if (sea_is_obj(obj_v) && sea_as_obj(obj_v)->type == OBJ_STRUCT) {
            SeaString *key_str = SEA_AS_STRING(chunk->constants[c]);
            REG(a) = sea_struct_get(SEA_AS_STRUCT(obj_v), key_str);
        } else if (sea_is_obj(obj_v) && sea_as_obj(obj_v)->type == OBJ_TABLE) {
            /* tables (modules, objects): straight lookup — the key is an
             * interned constant, so table_get's generic dispatch is moot */
            SeaValue v = SEA_NIL_VAL;
            sea_table_get_ic(SEA_AS_TABLE(obj_v), chunk->constants[c],
                             (SeaTableIC*)&sea_chunk_ics(chunk)[ip - 1 - chunk->code], &v);
            REG(a) = v;
        } else {
            SAVE_IP();
            REG(a) = table_get(vm, obj_v, chunk->constants[c]);
        }
        DISPATCH();
    }
    lbl_SET_FIELD: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue obj_v = REG(a);
        if (sea_is_obj(obj_v) && sea_as_obj(obj_v)->type == OBJ_STRUCT) {
            SeaString *key_str = SEA_AS_STRING(chunk->constants[b]);
            SeaStruct *st = SEA_AS_STRUCT(obj_v);
            if (st->building_remaining > 0) {
                sea_struct_init_field(st, key_str, REG(c));
            } else if (!sea_struct_set(st, key_str, REG(c))) {
                SAVE_IP();
                sea_vm_error_noreturn(vm, "attempt to add field '%s' to sealed struct",
                                     key_str->data);
            }
        } else {
            SAVE_IP();
            table_set(vm, obj_v, chunk->constants[b], REG(c));
        }
        DISPATCH();
    }
    lbl_NEW_STRUCT: {
        GC_SAFEPOINT();
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SeaStruct *st = sea_struct_new((uint32_t)b);
        REG(a) = sea_obj_val(&st->obj);
        DISPATCH();
    }

    // ---- Arrays ----

    lbl_NEW_ARRAY: {
        GC_SAFEPOINT();
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        uint32_t cap = 0;
        if (b != a) {  // B==A is sentinel "no explicit capacity"
            SeaValue cv = REG(b);
            cap = sea_is_int(cv) ? (uint32_t)sea_as_int(cv) : 0;
        }
        SeaArray *arr = sea_array_new(cap);  // sea_array_new guarantees cap>=4
        REG(a) = sea_obj_val(&arr->obj);
        DISPATCH();
    }
    lbl_GET_ARRAY: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue obj_v = REG(b);
        SAVE_IP();
        if (!sea_is_array(obj_v)) {
            REG(a) = table_get(vm, obj_v, REG(c));
        } else {
            int64_t idx = sea_is_int(REG(c)) ? sea_as_int(REG(c)) : (int64_t)to_number(vm, REG(c));
            REG(a) = sea_array_get(SEA_AS_ARRAY(obj_v), idx);
        }
        DISPATCH();
    }
    lbl_SET_ARRAY: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue obj_v = REG(a);
        SAVE_IP();
        if (!sea_is_array(obj_v)) {
            table_set(vm, obj_v, REG(b), REG(c));
        } else {
            int64_t idx = sea_is_int(REG(b)) ? sea_as_int(REG(b)) : (int64_t)to_number(vm, REG(b));
            sea_array_set(SEA_AS_ARRAY(obj_v), idx, REG(c));
        }
        DISPATCH();
    }
    lbl_ARRAY_PUSH: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SeaValue arr_v = REG(a);
        if (sea_is_array(arr_v)) {
            sea_array_push(SEA_AS_ARRAY(arr_v), REG(b));
        } else if (sea_is_int_array(arr_v)) {
            SeaValue v = REG(b);
            int64_t iv = sea_is_int(v) ? sea_as_int(v) : (int64_t)sea_as_number(v);
            sea_int_array_push(SEA_AS_INT_ARRAY(arr_v), iv);
        } else if (sea_is_float_array(arr_v)) {
            SeaValue v = REG(b);
            double fv = sea_is_number(v) ? sea_as_number(v) : (double)sea_as_int(v);
            sea_float_array_push(SEA_AS_FLOAT_ARRAY(arr_v), fv);
        } else if (sea_is_float32_array(arr_v)) {
            SeaValue v = REG(b);
            double fv = sea_is_number(v) ? sea_as_number(v) : (double)sea_as_int(v);
            sea_float32_array_push(SEA_AS_FLOAT32_ARRAY(arr_v), fv);
        }
        DISPATCH();
    }

    lbl_NEW_MAP: {
        GC_SAFEPOINT();
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        SeaMap *m = sea_map_new();
        REG(a) = sea_obj_val(&m->obj);
        DISPATCH();
    }

    lbl_MARK_CAPTURED: {
        mark_captured(vm, &REG(SEA_INSTR_A(ip[-1])));
        DISPATCH();
    }
    lbl_DEL: {
        uint32_t i = ip[-1];
        SAVE_IP();
        vm_del(vm, base, SEA_INSTR_A(i), SEA_INSTR_B(i));
        DISPATCH();
    }

    // ---- Typed arrays (no per-element RC) ----

    lbl_GET_IARRAY: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue arr_v = REG(b);
        int64_t idx = sea_is_int(REG(c)) ? sea_as_int(REG(c)) : (int64_t)to_number(vm, REG(c));
        REG(a) = sea_int_val(sea_int_array_get(SEA_AS_INT_ARRAY(arr_v), idx));
        DISPATCH();
    }
    lbl_SET_IARRAY: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue arr_v = REG(a);
        int64_t idx = sea_is_int(REG(b)) ? sea_as_int(REG(b)) : (int64_t)to_number(vm, REG(b));
        int64_t val = sea_is_int(REG(c)) ? sea_as_int(REG(c)) : (int64_t)to_number(vm, REG(c));
        sea_int_array_set(SEA_AS_INT_ARRAY(arr_v), idx, val);
        DISPATCH();
    }
    lbl_GET_FARRAY: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue arr_v = REG(b);
        int64_t idx = sea_is_int(REG(c)) ? sea_as_int(REG(c)) : (int64_t)to_number(vm, REG(c));
        REG(a) = sea_number_val(sea_float_array_get(SEA_AS_FLOAT_ARRAY(arr_v), idx));
        DISPATCH();
    }
    lbl_SET_FARRAY: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaValue arr_v = REG(a);
        int64_t idx = sea_is_int(REG(b)) ? sea_as_int(REG(b)) : (int64_t)to_number(vm, REG(b));
        double val = sea_is_number(REG(c)) ? sea_as_number(REG(c)) : (double)to_number(vm, REG(c));
        sea_float_array_set(SEA_AS_FLOAT_ARRAY(arr_v), idx, val);
        DISPATCH();
    }

    // ---- Closures ----

    lbl_CLOSURE: {
        GC_SAFEPOINT();
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        int proto_idx = SEA_INSTR_Bx(i);
        SeaChunk *proto = chunk->protos[proto_idx];

        SeaClosure *cl = SEA_ALLOC(SeaClosure);
        cl->obj.type = OBJ_CLOSURE;
        cl->obj.gc = 0;
        cl->obj.refcount = 1;
        cl->chunk = proto;
        cl->is_native = false;
        cl->native = NULL;
        cl->native_name = NULL;
        cl->upval_count = proto->upval_count;
        cl->upvalues = proto->upval_count > 0 ?
            SEA_ALLOC_N(SeaUpvalue*, proto->upval_count) : NULL;

        // Capture upvalues
        for (int j = 0; j < proto->upval_count; j++) {
            SeaUpvalDesc *desc = &proto->upvals[j];
            if (desc->is_local) {
                // Capture local from current frame
                cl->upvalues[j] = capture_upvalue(vm, &base[desc->idx]);
            } else {
                // Copy upvalue from enclosing closure
                cl->upvalues[j] = frame->closure->upvalues[desc->idx];
                sea_obj_retain(&cl->upvalues[j]->obj);
            }
        }

        REG(a) = sea_obj_val(&cl->obj);
        DISPATCH();
    }

    // ---- Function calls ----

    lbl_CALL: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SAVE_IP();
        BUDGET_STEP();
        // R[a] = fn, R[a+1..a+c] = args
        // Returns B values starting at R[a]
        SeaValue fn_val = REG(a);
        /* C=255: vararg call - argc = (fixed args already pushed) + vararg_count.
         * The compiler emitted OP_VARARG with B=0 before this CALL, expanding varargs
         * into the registers right after the fixed args. */
        int argc;
        if (c == 255) {
            /* Find how many args are actually there:
             * Fixed args + varargs were expanded into base[a+1..].
             * The fixed count = (va_reg - (a+1)) before OP_VARARG.
             * Actually, the OP_VARARG wrote into base[a + 1 + fixed_count .. a + 1 + fixed_count + nva - 1].
             * We need to count how many non-nil args there are.
             * Simpler: store nva in the vararg register before call.
             * Easiest: c = fixed_argc + vararg_count; fixed_argc embedded in the call instr
             * as the value right before va_reg. Actually we don't know without more info.
             * Best approach: just count: argc = fixed_argc + frame->vararg_count.
             * But 'fixed_argc' isn't stored in this instruction.
             *
             * Since the compiler put fixed args at a+1..a+fixed_argc, then varargs at
             * a+1+fixed_argc..a+1+fixed_argc+nva-1, we can compute total as:
             * argc = whatever makes sense.
             *
             * For the simple case print(...): fixed=0, varargs=all. So argc=nva.
             * For f(x, ...): fixed=1, varargs after. argc=1+nva.
             *
             * The compiler always puts the function at R[dst], fixed args at R[dst+1..dst+fixed],
             * then OP_VARARG expands to R[dst+1+fixed..dst+1+fixed+nva-1].
             * The current c in the instruction = fixed_argc (we used 255 just to signal vararg call,
             * but we need the fixed count).
             *
             * FIX: use C = fixed_argc + 128 as the vararg call signal (C < 128 = normal call).
             * But we already emitted 255. Let me just use: count all non-zero registers from a+1 upward
             * up to a reasonable limit. This is ugly.
             *
             * Cleanest: re-encode: use C = (fixed_argc << 1) | 1 as "vararg call" marker.
             * But that requires changing the compiler too.
             *
             * For now, use the frame's vararg_count + the register count before OP_VARARG.
             * The compiler always emits: push fixed args, OP_VARARG, CALL 255.
             * The OP_VARARG start reg = a + 1 + fixed_argc.
             * We don't know fixed_argc here. But in the simple case (print(...)), fixed=0.
             *
             * Let me just use a magic encoding: the previous instruction was OP_VARARG A=va_start.
             * argc = (va_start - (a+1)) + vararg_count.
             */
            uint32_t prev_instr = ip[-2];
            if (SEA_INSTR_OP(prev_instr) == OP_VARARG) {
                int va_start = SEA_INSTR_A(prev_instr);
                int fixed = va_start - (a + 1);
                argc = fixed + frame->vararg_count;
            } else {
                argc = frame->vararg_count;
            }
        } else {
            argc = c;
        }

        if (sea_is_obj(fn_val) && sea_as_obj(fn_val)->type == OBJ_CLOSURE) {
            SeaClosure *cl = (SeaClosure*)sea_as_obj(fn_val);
            if (cl->is_native) {
                SeaValue result = call_native(vm, cl, argc, &REG(a+1));
                if (SEA_UNLIKELY(result == SEA_PARK_VAL)) {
                    /* the native is waiting for I/O (sea_netpoll.h): park,
                     * and run this CALL again when the goroutine resumes —
                     * its arguments stay where they are */
                    frame->ip = ip - 1;
                    return SEA_SUSPENDED;
                }
                native_call_done(base, a, b, argc, result);
                if (SEA_UNLIKELY(vm->panic_propagating)) {   /* from a callback */
                    SAVE_IP();
                    goto panic_unwind;
                }
            } else {
                SeaChunk *callee_chunk = cl->chunk;
                int n_args = argc < 256 ? argc : 255;

                /* Fast path: exact arity, non-vararg, stack not full.
                 * Args are at base[a+1..a+n_args], already in the right position
                 * for the callee — no memmove needed. */
                if (SEA_LIKELY(!callee_chunk->is_vararg &&
                               n_args == callee_chunk->arity &&
                               vm->frame_count < SEA_MAX_CALL_DEPTH)) {
                    SeaCallFrame *nf = &vm->frames[vm->frame_count++];
                    nf->closure = cl;
                    nf->ip = callee_chunk->code;
                    nf->base = &base[a + 1];
                    nf->expected_ret = b;
                    nf->defer_base = vm->defer_count;
                    nf->vararg_count = 0;
                    nf->varargs = NULL;
                    nf->arena_mark = vm->frame_arena_used;
                    if (vm->jit_enabled) JIT_TRY_CALL(callee_chunk, nf, a);
                    frame = nf;
                    base = nf->base;
                    ip = nf->ip;
                    chunk = callee_chunk;
                    DISPATCH();
                }

                /* Slow path: vararg, arity mismatch, or stack overflow */
                if (vm->frame_count >= SEA_MAX_CALL_DEPTH) {
                    if (vm->sandboxed) sea_vm_error_noreturn(vm, "call stack overflow");   /* a panic, not an exit */
                    sea_vm_print_traceback(vm);
                    fprintf(stderr, "sea runtime error: call stack overflow\n");
                    exit(1);
                }
                SeaCallFrame *new_frame = &vm->frames[vm->frame_count++];
                new_frame->closure = cl;
                new_frame->ip = callee_chunk->code;
                /* Base points at arg0: args are already in the right slots. */
                new_frame->base = &base[a + 1];
                new_frame->expected_ret = b;
                new_frame->defer_base = vm->defer_count;
                new_frame->arena_mark = vm->frame_arena_used;

                /* Nil-fill missing args when caller passed fewer than arity */
                for (int j = n_args; j < callee_chunk->arity; j++)
                    new_frame->base[j] = SEA_NIL_VAL;
                /* Surplus args of a fixed-arity callee: owned, never seen */
                if (!callee_chunk->is_vararg)
                    for (int j = callee_chunk->arity; j < n_args; j++) {
                        vm_release_val(new_frame->base[j]);
                        new_frame->base[j] = SEA_NIL_VAL;
                    }

                /* Capture varargs for vararg functions */
                new_frame->vararg_count = 0;
                new_frame->varargs = NULL;
                if (callee_chunk->is_vararg && n_args > callee_chunk->arity) {
                    /* the extra args at base[arity..] move to their own block */
                    int nva = n_args - callee_chunk->arity;
                    new_frame->varargs = varargs_new(&new_frame->base[callee_chunk->arity], nva);
                    new_frame->vararg_count = nva;
                }

                frame = new_frame;
                base = frame->base;
                ip = frame->ip;
                chunk = frame->closure->chunk;
            }
        } else {
            vm_operand_error(vm, "call", fn_val, true);
        }
        DISPATCH();
    }

    lbl_CALL_SELF: {
        /* Self-recursive call: use frame->closure directly, skip upvalue lookup.
         * Same layout as CALL: A=result_reg, B=nret, C=nargs; args at R[A+1..A+C]. */
        SAVE_IP();  /* caller->ip must point past this instruction so RETURN can read it back */
        BUDGET_STEP();
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaClosure *cl = frame->closure;
        SeaChunk *callee_chunk = cl->chunk;
        if (SEA_LIKELY(!callee_chunk->is_vararg &&
                       c == callee_chunk->arity &&
                       vm->frame_count < SEA_MAX_CALL_DEPTH)) {
            SeaCallFrame *nf = &vm->frames[vm->frame_count++];
            nf->closure = cl;
            nf->ip = callee_chunk->code;
            nf->base = &base[a + 1];
            nf->expected_ret = b;
            nf->defer_base = vm->defer_count;
            nf->vararg_count = 0;
            nf->varargs = NULL;
            nf->arena_mark = vm->frame_arena_used;
            if (vm->jit_enabled) JIT_TRY_CALL(callee_chunk, nf, a);
            frame = nf;
            base = nf->base;
            ip = nf->ip;
            chunk = callee_chunk;
            DISPATCH();
        }
        /* Slow path: place closure in R[A] and fall through to lbl_CALL */
        REG(a) = sea_obj_val(&cl->obj);
        goto lbl_CALL;
    }

    lbl_CALL_MULTI: {
        // Same as CALL but with variable returns
        goto lbl_CALL;
    }

#ifdef SEA_JIT
    /* Back-edge slow path (see JIT_BACKEDGE). ip points at a loop head. */
    jit_osr: {
        void *jf = SEA_JIT_LOAD_FN(chunk);
        if (!vm->jit_enabled) { chunk->jit_hot = INT32_MAX; DISPATCH(); }
        if (jf == NULL) {
            if (!chunk->jit_armed) {        /* first back-edge: start counting */
                chunk->jit_armed = 1;
                chunk->jit_hot = sea_jit_hot_loops;
                DISPATCH();
            }
            jf = sea_jit_request(vm, chunk);
        }
        if (jf == SEA_JIT_FAILED)  { chunk->jit_hot = INT32_MAX; DISPATCH(); }
        if (jf == SEA_JIT_PENDING) { chunk->jit_hot = 1024; DISPATCH(); }  /* poll */

        SAVE_IP();
        int nret = ((SeaJittedFn)jf)(vm, frame);
        if (nret == SEA_JIT_SUSPEND) return SEA_SUSPENDED;   /* frame->ip = resume point */
        if (nret == SEA_JIT_BAILOUT) {
            if (SEA_UNLIKELY(vm->panic_propagating)) goto panic_unwind;
            /* Resume where native code stopped (or right here, if this loop
             * head is not an OSR entry). Re-check only every 64 back-edges so
             * a loop that cannot stay native costs little. */
            ip = frame->ip;
            chunk->jit_hot = 64;
            DISPATCH();
        }
        /* Native code ran the function to its RETURN and left the nret results
         * in base[0..]; finish through the interpreter's return path (defers,
         * upvalue closing, copying into the caller) with a synthetic
         * RETURN A=0 B=nret. */
        jit_ret_instr = SEA_MAKE_ABC(OP_RETURN, 0, nret, 0);
        ip = &jit_ret_instr + 1;
        goto lbl_RETURN;
    }
#endif

    lbl_RETURN: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);

        /* Fast path: skip defer/upvalue teardown when nothing is pending */
        if (SEA_UNLIKELY(vm->defer_count > frame->defer_base || vm->open_upvalues != NULL)) {
            RUN_DEFERS();
            close_upvalues(vm, base);
        }
        /* The results R[a..a+b-1] are owned (+1): the compiler released the
         * frame's other locals before this instruction. */
        if (SEA_UNLIKELY(frame->vararg_count)) release_varargs(frame);

        vm->frame_arena_used = frame->arena_mark;  /* bulk-free arena objects */
        vm->frame_count--;
        if (vm->frame_count <= base_frame) {
            /* Back to C (sea_vm_call, a goroutine's root, a module): the
             * first result, +1, goes to base[0]; extra results are dropped. */
            SeaValue r0 = (b > 0) ? base[a] : SEA_NIL_VAL;
            for (int j = 1; j < b; j++) vm_release_val(base[a + j]);
            if (vm->has_exec_result) {
                vm->exec_result = r0;
                vm->has_exec_result = false;
            }
            base[0] = r0;
            return SEA_OK;
        }

        SeaCallFrame *caller = &vm->frames[vm->frame_count - 1];
        // The call site: CALL instruction was at caller->ip[-1]
        // The function was at R[fn_reg] in the caller's frame
        uint32_t call_instr = caller->ip[-1];
        int fn_reg = SEA_INSTR_A(call_instr);
        deliver_results(&caller->base[fn_reg], &base[a], b,
                        vm->frames[vm->frame_count].expected_ret);

        frame = caller;
        base = frame->base;
        ip = frame->ip;
        chunk = frame->closure->chunk;
        GC_SAFEPOINT();
        DISPATCH();
    }

    lbl_RETURN0: {
        if (SEA_UNLIKELY(vm->defer_count > frame->defer_base || vm->open_upvalues != NULL)) {
            RUN_DEFERS();
            close_upvalues(vm, base);
        }
        if (SEA_UNLIKELY(frame->vararg_count)) release_varargs(frame);

        vm->frame_arena_used = frame->arena_mark;  /* bulk-free arena objects */
        vm->frame_count--;
        if (vm->frame_count <= base_frame) {
            if (vm->has_exec_result) {
                vm->exec_result = SEA_NIL_VAL;
                vm->has_exec_result = false;
            }
            base[0] = SEA_NIL_VAL; /* no return value; sea_vm_call reads base[0] */
            return SEA_OK;
        }

        SeaCallFrame *caller = &vm->frames[vm->frame_count - 1];
        uint32_t call_instr = caller->ip[-1];
        int fn_reg = SEA_INSTR_A(call_instr);
        int exp_ret = vm->frames[vm->frame_count].expected_ret;
        if (exp_ret > 0) {
            // Fill with nil
            for (int j = 0; j < exp_ret; j++) {
                caller->base[fn_reg + j] = SEA_NIL_VAL;
            }
        }

        frame = caller;
        base = frame->base;
        ip = frame->ip;
        chunk = frame->closure->chunk;
        GC_SAFEPOINT();
        DISPATCH();
    }

    lbl_TAIL_CALL: {
        goto lbl_CALL;
    }

    lbl_VARARG: {
        /* OP_VARARG A=dst, B=count, C=mode
         * C=0, B>0: copy exactly B varargs into R[A..A+B-1]
         * C=0, B=0: expand ALL varargs into R[A..A+nva-1] (for vararg CALL)
         * C=1, B=0: pack all varargs into the table/array already in R[A]
         */
        uint32_t instr2 = ip[-1];
        int a = SEA_INSTR_A(instr2);
        int b = SEA_INSTR_B(instr2);
        int cmode = SEA_INSTR_C(instr2);
        int nva = frame->vararg_count;

        if (cmode == 1) {
            /* Pack mode: insert all varargs into table/array at R[A] */
            SeaValue tgt = REG(a);
            if (sea_is_table(tgt)) {
                SeaTable *t = SEA_AS_TABLE(tgt);
                for (int j = 0; j < nva; j++) {
                    SeaValue k = sea_int_val(j);
                    SeaValue v = frame->varargs ? frame->varargs[j] : SEA_NIL_VAL;
                    sea_table_set(t, k, v);
                }
            } else if (sea_is_array(tgt)) {          /* [x, ...] */
                SeaArray *arr = SEA_AS_ARRAY(tgt);
                for (int j = 0; j < nva; j++)
                    sea_array_push(arr, frame->varargs ? frame->varargs[j] : SEA_NIL_VAL);
            }
        } else if (b == 0) {
            /* Expand all varargs starting at R[A] — the tail of a call's
             * arguments, which are passed owned: retain each copy */
            for (int j = 0; j < nva; j++) {
                base[a + j] = frame->varargs ? frame->varargs[j] : SEA_NIL_VAL;
                vm_retain_val(base[a + j]);
            }
        } else {
            /* Copy exactly B varargs */
            int ncopy = (b < nva) ? b : nva;
            for (int j = 0; j < ncopy; j++) {
                base[a + j] = frame->varargs ? frame->varargs[j] : SEA_NIL_VAL;
            }
            for (int j = ncopy; j < b; j++) base[a + j] = SEA_NIL_VAL;
        }
        DISPATCH();
    }

    // ---- For loops ----

    lbl_FOR_PREP: {
        /* FOR_PREP: R[A]=idx, R[A+1]=limit, R[A+2]=step, R[A+3]=user var.
         * Copy idx to the user-visible variable, then check the initial
         * condition.  If the loop body would never execute (e.g. start=0,
         * limit=-1, step=1), jump forward by sBx past the whole loop. */
        uint32_t i = ip[-1];
        int a   = SEA_INSTR_A(i);
        int sbx = SEA_INSTR_sBx(i);
        SeaValue idx_v   = REG(a);
        SeaValue limit_v = REG(a+1);
        SeaValue step_v  = REG(a+2);
        REG(a+3) = idx_v;
        bool skip;
        if (SEA_LIKELY(sea_is_int(idx_v) && sea_is_int(limit_v) && sea_is_int(step_v))) {
            int64_t step = sea_as_int(step_v);
            if (step > 0) skip = sea_as_int(idx_v) > sea_as_int(limit_v);
            else           skip = sea_as_int(idx_v) < sea_as_int(limit_v);
        } else {
            SAVE_IP();
            double didx   = to_number(vm, idx_v);
            double dlimit = to_number(vm, limit_v);
            double dstep  = to_number(vm, step_v);
            if (dstep > 0) skip = didx > dlimit;
            else            skip = didx < dlimit;
        }
        if (skip) ip += sbx;
        DISPATCH();
    }

    lbl_FOR_LOOP: {
        /* FOR_LOOP: advance R[A] by step, check vs limit.
         * Layout: R[A]=idx, R[A+1]=limit, R[A+2]=step, R[A+3]=user var. */
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        int sbx = SEA_INSTR_sBx(i);

        SeaValue idx_v   = REG(a);
        SeaValue limit_v = REG(a+1);
        SeaValue step_v  = REG(a+2);

        if (SEA_LIKELY(sea_is_int(idx_v) && sea_is_int(limit_v) && sea_is_int(step_v))) {
            int64_t step = sea_as_int(step_v);
            if (SEA_LIKELY(step > 0)) {
                /* Decoded, not the raw NaN-boxed words: a negative payload
                 * compares above a positive one as raw bits, and -1 + 1
                 * carries into the tag (for i = -1, 1 ran once). */
                int64_t idx   = sea_as_int(idx_v) + step;
                SeaValue new_idx = sea_int_val(idx);
                REG(a) = new_idx;
                if (idx <= sea_as_int(limit_v)) { REG(a+3) = new_idx; ip += sbx; BUDGET_STEP(); JIT_BACKEDGE(); }
            } else {
                int64_t idx   = sea_as_int(idx_v) + step;
                int64_t limit = sea_as_int(limit_v);
                SeaValue new_idx = sea_int_val(idx);
                REG(a) = new_idx;
                if (idx >= limit) { REG(a+3) = new_idx; ip += sbx; BUDGET_STEP(); JIT_BACKEDGE(); }
            }
        } else {
            SAVE_IP();
            double idx   = to_number(vm, idx_v) + to_number(vm, step_v);
            double limit = to_number(vm, limit_v);
            double step  = to_number(vm, step_v);
            REG(a) = sea_number_val(idx);
            bool cont = (step > 0) ? (idx <= limit) : (idx >= limit);
            if (cont) { REG(a+3) = REG(a); ip += sbx; BUDGET_STEP(); }
        }
        DISPATCH();
    }

    lbl_FOR_LOOP_STEP1: {
        /* Specialized FOR_LOOP for step=1, all-int bounds — no type checks. */
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        int sbx = SEA_INSTR_sBx(i);
        int64_t idx = sea_as_int(REG(a)) + 1;  /* decoded: see FOR_LOOP */
        SeaValue new_idx = sea_int_val(idx);
        REG(a) = new_idx;
        if (idx <= sea_as_int(REG(a+1))) { REG(a+3) = new_idx; ip += sbx; BUDGET_STEP(); JIT_BACKEDGE(); }
        DISPATCH();
    }
    lbl_FOR_LOOP_INT: {
        /* Specialized FOR_LOOP for arbitrary int step — no type checks. */
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        int sbx = SEA_INSTR_sBx(i);
        int64_t step = sea_as_int(REG(a+2));
        if (SEA_LIKELY(step > 0)) {
            int64_t idx = sea_as_int(REG(a)) + step;  /* decoded: see FOR_LOOP */
            SeaValue new_idx = sea_int_val(idx);
            REG(a) = new_idx;
            if (idx <= sea_as_int(REG(a+1))) { REG(a+3) = new_idx; ip += sbx; BUDGET_STEP(); JIT_BACKEDGE(); }
        } else {
            int64_t idx = sea_as_int(REG(a)) + step;
            SeaValue new_idx = sea_int_val(idx);
            REG(a) = new_idx;
            if (idx >= sea_as_int(REG(a+1))) { REG(a+3) = new_idx; ip += sbx; BUDGET_STEP(); JIT_BACKEDGE(); }
        }
        DISPATCH();
    }

    lbl_FOR_IN_INIT: {
        // Container is already in R[A]; store cursor=0 in R[A+1] and clear
        // the B loop variables (each FOR_IN_NEXT releases their old values).
        uint32_t instr = ip[-1];
        int a = SEA_INSTR_A(instr), b = SEA_INSTR_B(instr);
        REG(a + 1) = sea_int_val(0);
        for (int j = 0; j < b; j++) REG(a + 2 + j) = SEA_NIL_VAL;
        DISPATCH();
    }

    lbl_FOR_IN_NEXT: {
        SAVE_IP();
        uint32_t instr = ip[-1];
        if (vm_for_in_next(vm, frame, base, ip - 1, SEA_INSTR_A(instr), SEA_INSTR_B(instr)))
            return SEA_SUSPENDED;
        DISPATCH();
    }

    // ---- Channels ----

    lbl_NEW_CHAN: {
        if (SEA_UNLIKELY(vm->sandboxed)) { SAVE_IP(); sea_vm_error_noreturn(vm, "channels are not available in a sandbox"); }
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        uint32_t cap = sea_is_int(REG(b)) ? (uint32_t)sea_as_int(REG(b)) : 0;
        SeaChannel *ch = sea_chan_new(cap);
        REG(a) = sea_obj_val(&ch->obj);
        DISPATCH();
    }
    lbl_CHAN_SEND: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SAVE_IP();
        if (!sea_is_channel(REG(a)))
            sea_vm_error_noreturn(vm, "attempt to send on non-channel");
        sea_val_mark_shared(REG(b));
        SeaChannel *ch = SEA_AS_CHANNEL(REG(a));
        SeaGoroutine *g = vm->goroutine;
        if (g) {
            /* Rewind (retry this instruction on resume) BEFORE parking: once in
             * the wait queue a receiver may hand off and advance frame->ip past
             * this send (sea_chan_recv_nb), or requeue us right away. */
            frame->ip = ip - 1;
            SeaChanStatus st = sea_chan_send_nb(ch, REG(b), g);
            if (st == CHAN_PARKED) return SEA_SUSPENDED;
            if (st == CHAN_CLOSED)
                sea_vm_error_noreturn(vm, "send on closed channel");
        } else {
            sea_chan_send(ch, REG(b));
        }
        DISPATCH();
    }
    lbl_CHAN_RECV: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SAVE_IP();
        if (!sea_is_channel(REG(b)))
            sea_vm_error_noreturn(vm, "attempt to receive from non-channel");
        SeaChannel *ch = SEA_AS_CHANNEL(REG(b));
        SeaGoroutine *g = vm->goroutine;
        if (g) {
            if (g->has_pending_recv) {
                /* Value pre-loaded by direct handoff (unbuffered channel) */
                REG(a) = g->block_val;
                g->has_pending_recv = false;
            } else {
                SeaValue val = SEA_NIL_VAL;
                frame->ip = ip - 1;   /* resume point before becoming visible to wakers */
                SeaChanStatus st = sea_chan_recv_nb(ch, &val, g);
                if (st == CHAN_PARKED) return SEA_SUSPENDED;
                REG(a) = val;  /* CHAN_OK or CHAN_CLOSED → nil */
            }
        } else {
            SeaValue val = SEA_NIL_VAL;
            sea_chan_recv(ch, &val);
            REG(a) = val;
        }
        DISPATCH();
    }
    lbl_CHAN_CLOSE: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        if (sea_is_channel(REG(a)))
            sea_chan_close(SEA_AS_CHANNEL(REG(a)));
        DISPATCH();
    }
    lbl_CHAN_TRY_SEND: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SAVE_IP();
        if (!sea_is_channel(REG(a)))
            sea_vm_error_noreturn(vm, "attempt to send on non-channel");
        sea_val_mark_shared(REG(b));
        bool ok = sea_chan_try_send(SEA_AS_CHANNEL(REG(a)), REG(b));
        REG(a) = sea_bool_val(ok);
        DISPATCH();
    }
    lbl_CHAN_TRY_RECV: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SAVE_IP();
        if (!sea_is_channel(REG(b)))
            sea_vm_error_noreturn(vm, "attempt to receive from non-channel");
        SeaValue val = SEA_NIL_VAL;
        bool ok = sea_chan_try_recv(SEA_AS_CHANNEL(REG(b)), &val);
        REG(a) = val;
        REG(a+1) = sea_bool_val(ok);
        DISPATCH();
    }

    // ---- Goroutines ----

    lbl_GO: {
        if (SEA_UNLIKELY(vm->sandboxed)) { SAVE_IP(); sea_vm_error_noreturn(vm, "goroutines are not available in a sandbox"); }
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        // Spawn goroutine: fn=REG(a), args=REG(b)..REG(b+c-1)
        SeaValue fn_val = REG(a);
        SeaValue *args = &base[b];
        if (!sea_is_closure(fn_val)) {   /* as for a call: an error here, in the spawner */
            SAVE_IP();
            sea_vm_error_noreturn(vm, "attempt to start a goroutine on a %s value",
                                  sea_type_name(fn_val));
        }
        /* the goroutine retains its function; the arguments were passed
         * owned (+1) and move into it, as for a call */
        vm_retain_val(fn_val);
        sea_goroutine_spawn(vm, fn_val, args, c);
        DISPATCH();
    }
    lbl_YIELD: {
        SeaGoroutine *gy = vm->goroutine;
        if (gy) {
            /* Real M:N yield: re-enqueue this goroutine and let the worker
             * pick a different one.  ip already points past YIELD, so on
             * resume we continue with the next instruction. */
            frame->ip = ip;
            gy->status = GORO_RUNNABLE;
            sea_sched_requeue(sea_global_sched, gy);
            return SEA_SUSPENDED;
        }
        sched_yield();
        DISPATCH();
    }

    // ---- Select ----

    lbl_SELECT_BEGIN: { DISPATCH(); }
    lbl_SELECT_CASE:  { DISPATCH(); }

    lbl_SELECT_END: {
        SAVE_IP();
        uint32_t sel_i = ip[-1];
        int winner_reg  = SEA_INSTR_A(sel_i);
        int sel_ncases  = SEA_INSTR_B(sel_i);
        int sel_has_def = SEA_INSTR_C(sel_i);
        /* ip[0] = JMP[0], ip[1] = JMP[1], ...
         * SELECT_CASE[k] is at ip[-(sel_ncases+1)+k] */
        int sel_default_idx = -1;
        int sel_winner      = -1;

        for (int k = 0; k < sel_ncases; k++) {
            uint32_t ci    = ip[-(sel_ncases + 1) + k];
            int ch_reg     = SEA_INSTR_A(ci);
            int val_reg    = SEA_INSTR_B(ci);
            int sel_flags  = SEA_INSTR_C(ci);
            bool is_send   = (sel_flags & 1) != 0;
            bool is_def    = (sel_flags & 2) != 0;

            if (is_def) { sel_default_idx = k; continue; }

            if (!sea_is_channel(REG(ch_reg)))
                sea_vm_error_noreturn(vm, "select: case is not a channel");
            SeaChannel *sch = SEA_AS_CHANNEL(REG(ch_reg));

            if (is_send) {
                /* as OP_CHAN_SEND: whoever receives may run on another thread */
                sea_val_mark_shared(REG(val_reg));
                if (sea_chan_try_send(sch, REG(val_reg))) { sel_winner = k; break; }
            } else {
                SeaValue rv = SEA_NIL_VAL;
                if (sea_chan_try_recv(sch, &rv)) {
                    REG(val_reg) = rv;  /* put received value in dest register */
                    sel_winner = k;
                    break;
                }
            }
        }

        if (sel_winner >= 0) {
            REG(winner_reg) = sea_int_val(sel_winner);
            ip += sel_winner;   /* skip to winner's JMP */
            DISPATCH();
        }
        if (sel_has_def && sel_default_idx >= 0) {
            REG(winner_reg) = sea_int_val(sel_default_idx);
            ip += sel_default_idx;
            DISPATCH();
        }

        /* No case ready, no default */
        SeaGoroutine *sg = vm->goroutine;
        if (sg) {
            /* Goroutine: yield back to scheduler and retry select on next run */
            frame->ip = ip - 1;   /* rewind to SELECT_END */
            sg->status = GORO_RUNNABLE;
            sea_sched_requeue(sea_global_sched, sg);
            return SEA_SUSPENDED;
        }
        /* Main thread: spin */
        ip = ip - 1;   /* rewind to SELECT_END */
        sched_yield();
        DISPATCH();
    }

    // ---- Error handling ----

    lbl_NEW_ERROR: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        REG(a) = sea_make_error(vm, REG(b));
        DISPATCH();
    }
    lbl_IS_ERROR: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        REG(a) = sea_bool_val(sea_is_error(REG(b)));
        DISPATCH();
    }
    lbl_ERROR_MSG: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        if (sea_is_error(REG(b))) {
            REG(a) = SEA_AS_ERROR(REG(b))->message;
        } else {
            REG(a) = SEA_NIL_VAL;
        }
        DISPATCH();
    }
    lbl_PROPAGATE: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        if (sea_is_error(REG(a))) {
            /* Return the error from the current function. Unhandled when no
             * bytecode caller is left (main chunk, goroutine, callback). */
            if (vm->frame_count - 1 <= base_frame) {
                SAVE_IP();
                if (vm->sandboxed) {
                    SeaString *ms = sea_vm_tostring(vm, ((SeaError*)sea_as_obj(REG(a)))->message);
                    char buf[300];
                    snprintf(buf, sizeof buf, "unhandled error: %.250s", ms->data);
                    sea_obj_release(&ms->obj);
                    sea_vm_error_noreturn(vm, "%s", buf);
                }
                sea_vm_print_traceback(vm);
                SeaString *s = sea_vm_tostring(vm, ((SeaError*)sea_as_obj(REG(a)))->message);
                fprintf(stderr, "sea runtime error: unhandled error: %s\n", s->data);
                sea_obj_release(&s->obj);
                exit(1);
            }
            /* The compiler made R[a] owned and released the frame's locals
             * before this instruction: finish as `return R[a]` (defers run). */
            synth_ret_instr = SEA_MAKE_ABC(OP_RETURN, a, 1, 0);
            ip = &synth_ret_instr + 1;
            goto lbl_RETURN;
        }
        // else: fall through, REG(a) = unwrapped value (it's already there)
        DISPATCH();
    }

    // ---- Panic/Defer ----

    lbl_DEFER: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        /* The entry retains the callee; the arguments were passed owned (+1)
         * and move into it. */
        if (vm->defer_count < 256) {
            SeaDeferEntry *de = &vm->defers[vm->defer_count++];
            de->fn = REG(a);
            vm_retain_val(de->fn);
            de->argc = b < SEA_DEFER_MAX_ARGS ? b : SEA_DEFER_MAX_ARGS;
            for (int j = 0; j < de->argc; j++) {
                de->args[j] = base[a+1+j];
            }
        } else {
            for (int j = 0; j < b; j++) vm_release_val(base[a+1+j]);
        }
        DISPATCH();
    }
    lbl_PANIC: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        SAVE_IP();
        SeaValue v = REG(a);
        vm_retain_val(v);                /* owned by the VM until recover() takes it */
        if (vm->panicking) vm_release_val(vm->panic_val);
        vm->panicking = true;
        vm->panic_val = v;
        vm_capture_trace(vm);
        goto panic_unwind;
    }

    /* Unwind a panic (see Panics): run the top frame's deferred calls; if one
     * recovers, that frame returns nil (RETURN0 runs its other defers);
     * otherwise pop it and go on with its caller. */
    panic_unwind: {
        vm->panic_propagating = false;
        vm->io_call.active = false;
        for (;;) {
            frame = &vm->frames[vm->frame_count - 1];
            bool recovered = false;
            while (vm->defer_count > frame->defer_base) {
                SeaDeferEntry de = vm->defers[--vm->defer_count];
                if (sea_is_obj(de.fn)) {
                    vm_release_val(sea_vm_call(vm, de.fn, de.args, de.argc));
                    vm->panic_propagating = false;   /* a new panic replaced it */
                }
                defer_entry_release(&de);
                frame = &vm->frames[vm->frame_count - 1];
                if (!vm->panicking) { recovered = true; break; }
            }
            if (recovered) {
                base  = frame->base;
                ip    = frame->ip;
                chunk = frame->closure->chunk;
                goto lbl_RETURN0;
            }
            if (vm->open_upvalues) close_upvalues(vm, frame->base);
            if (frame->vararg_count) release_varargs(frame);
            vm->frame_arena_used = frame->arena_mark;
            vm->frame_count--;
            if (vm->frame_count <= base_frame) {
                if (top) sea_vm_panic_exit(vm);
                if (vm->has_exec_result) {
                    vm->exec_result = SEA_NIL_VAL;
                    vm->has_exec_result = false;
                }
                frame->base[0] = SEA_NIL_VAL;     /* the C caller reads base[0] */
                vm->panic_propagating = true;
                return SEA_OK;
            }
        }
    }

    lbl_RECOVER: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        REG(a) = sea_vm_recover(vm);
        DISPATCH();
    }

    // ---- Built-in fast dispatch ----

    lbl_TYPE: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        const char *name = sea_type_name(REG(b));
        SeaString *s = sea_string_intern(name, strlen(name));
        REG(a) = sea_obj_val(&s->obj);
        sea_obj_release(&s->obj);
        DISPATCH();
    }
    lbl_TOSTRING: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SeaString *s = sea_vm_tostring(vm, REG(b));
        REG(a) = sea_obj_val(&s->obj);
        sea_obj_release(&s->obj);
        DISPATCH();
    }
    lbl_TONUMBER: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        SeaValue v = REG(b);
        if (sea_is_number(v) || sea_is_int(v)) {
            REG(a) = v;
        } else if (sea_is_string(v)) {
            char *end;
            double n = strtod(SEA_AS_STRING(v)->data, &end);
            REG(a) = (*end == '\0') ? sea_number_val(n) : SEA_NIL_VAL;
        } else if (sea_is_rope(v)) {
            SeaString *s = sea_rope_flatten(SEA_AS_ROPE(v));
            char *end;
            double n = strtod(s->data, &end);
            REG(a) = (*end == '\0') ? sea_number_val(n) : SEA_NIL_VAL;
            sea_obj_release(&s->obj);
        } else {
            REG(a) = SEA_NIL_VAL;
        }
        DISPATCH();
    }
    lbl_PRINT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        for (int j = 0; j < b; j++) {
            if (j > 0) putchar('\t');
            SeaString *s = sea_vm_tostring(vm, REG(a+j));
            fputs(s->data, stdout);
            sea_obj_release(&s->obj);
        }
        putchar('\n');
        DISPATCH();
    }
    lbl_ASSERT: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        if (!sea_is_truthy(REG(a))) {
            if (vm->sandboxed) {
                SAVE_IP();
                sea_vm_error_noreturn(vm, "assertion failed");
            }
            sea_vm_print_traceback(vm);
            if (b > 0 && !sea_is_nil(REG(b))) {
                SeaString *msg = sea_vm_tostring(vm, REG(b));
                fprintf(stderr, "assertion failed: %s\n", msg->data);
                sea_obj_release(&msg->obj);
            } else {
                fprintf(stderr, "assertion failed\n");
            }
            exit(1);
        }
        DISPATCH();
    }
    lbl_RELEASE: {
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        SeaValue v = REG(a);
        REG(a) = SEA_NIL_VAL;
        vm_release_val(v);
        DISPATCH();
    }
    lbl_RETAIN: {
        vm_retain_val(REG(SEA_INSTR_A(ip[-1])));
        DISPATCH();
    }
    lbl_SETLOCAL: {
        /* Assignment to an owning local: the new value is stored (retained
         * first when C, i.e. borrowed), then the old one released — in that
         * order, so `x = x` and `x = x.next` keep what they read alive. */
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i);
        SeaValue nv = REG(SEA_INSTR_B(i)), old = REG(a);
        if (SEA_INSTR_C(i)) vm_retain_val(nv);
        REG(a) = nv;
        vm_release_val(old);
        DISPATCH();
    }
    lbl_APPEND: {
        uint32_t i = ip[-1];
        SAVE_IP();
        vm_append(vm, base, SEA_INSTR_A(i), SEA_INSTR_B(i));
        DISPATCH();
    }
    lbl_JMP_NOT_ERR: {
        uint32_t i = ip[-1];
        if (!sea_is_error(REG(SEA_INSTR_A(i)))) ip += SEA_INSTR_sBx(i);
        DISPATCH();
    }
    lbl_SET_FIELD_MOVE: {
        uint32_t i = ip[-1];
        SAVE_IP();
        vm_set_field_move(vm, base, SEA_INSTR_A(i), SEA_INSTR_C(i), chunk->constants[SEA_INSTR_B(i)]);
        DISPATCH();
    }
    lbl_ARRAY_PUSH_MOVE: {
        uint32_t i = ip[-1];
        vm_array_push_move(base, SEA_INSTR_A(i), SEA_INSTR_B(i));
        DISPATCH();
    }
    lbl_COVER: {
        SeaChunk *cc = frame->closure->chunk;
        uint32_t l = SEA_INSTR_Bx(ip[-1]);
        if (l < (uint32_t)cc->cover_len) cc->cover[l] = 2;
        DISPATCH();
    }
    lbl_LOAD_SELF: {
        REG(SEA_INSTR_A(ip[-1])) = sea_obj_val(&frame->closure->obj);
        DISPATCH();
    }
    lbl_NEW_STRUCT_LOCAL: {
        /* Arena-allocate a struct for a non-escaping local; no RC. */
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        size_t needed = sizeof(SeaStruct) + (size_t)(uint32_t)b * (sizeof(SeaString*) + sizeof(SeaValue));
        SeaStruct *st = (SeaStruct*)sea_arena_alloc(vm, needed);
        if (SEA_UNLIKELY(!st)) {
            /* Arena full: fall back to heap allocation */
            st = sea_struct_new((uint32_t)b);
        } else {
            st->obj.type           = OBJ_STRUCT;
            st->obj.gc = 0;
            st->obj.refcount       = SEA_RC_ARENA_BIT | 1u;  /* arena, owned by the local */
            st->field_count        = 0;
            st->field_cap          = (uint32_t)b;
            st->_pad               = 0;
            st->building_remaining = (b > 0) ? b : -1;
            if (b > 0) {
                SeaString **names  = sea_struct_names(st);
                SeaValue   *values = sea_struct_values(st);
                for (int j = 0; j < b; j++) { names[j] = NULL; values[j] = SEA_NIL_VAL; }
            }
        }
        REG(a) = sea_obj_val(&st->obj);
        DISPATCH();
    }
    lbl_SET_FIELD_BORROW: {
        /* Store field without retaining value (arena struct borrows from owning local). */
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i), c = SEA_INSTR_C(i);
        SeaStruct *st = SEA_AS_STRUCT(REG(a));
        SeaString *key_str = SEA_AS_STRING(chunk->constants[b]);
        sea_struct_init_field(st, key_str, REG(c));   /* fields are owned (vm_release_val) */
        DISPATCH();
    }
    lbl_NEW_ARRAY_LOCAL: {
        /* Non-escaping local array: arena-allocate the SeaArray header (saving one
         * malloc), but keep the data buffer on the heap so push/realloc growth works
         * unchanged. The header is bulk-reclaimed on frame return; the data buffer and
         * element refcounts are freed by the arena-aware OP_RELEASE at scope exit. */
        uint32_t i = ip[-1];
        int a = SEA_INSTR_A(i), b = SEA_INSTR_B(i);
        uint32_t cap = (uint32_t)(int)b;
        SeaArray *arr = (SeaArray*)sea_arena_alloc(vm, sizeof(SeaArray));
        if (SEA_UNLIKELY(!arr)) {
            /* Arena full: fall back to a fully heap-allocated array. */
            arr = sea_array_new(cap);
        } else {
            sea_array_init_header(arr, cap);
            arr->obj.refcount = SEA_RC_ARENA_BIT | 1u;  /* arena, owned by the local */
        }
        REG(a) = sea_obj_val(&arr->obj);
        DISPATCH();
    }

#undef REG
#undef CONST
#undef DISPATCH
#undef JIT_TRY_CALL

    return SEA_OK;
}

#ifdef SEA_JIT
/* ---- JIT call helpers (invoked from JIT-compiled code) --------------------
 * Mirror the interpreter's OP_CALL fast path but as a re-entrant C call: push a
 * frame whose base overlaps the caller at base[a+1] (exactly like the
 * interpreter), run the callee (its own native code if hot, else vm_run), then
 * copy the single return value into base[a]. Runtime errors exit(); panics are
 * handled inline in OP_PANIC — neither unwinds through here — so vm_run only
 * returns on the callee's normal RETURN. */
static int jit_run_callee(SeaVM *vm, SeaClosure *cl, SeaValue *base, int a, int b, int c) {
    SeaChunk *callee = cl->chunk;
    if (callee->is_vararg || c != callee->arity) return 1;       /* slow path: bail */
    if (vm->frame_count >= SEA_MAX_CALL_DEPTH) return 1;

    SeaCallFrame *nf = &vm->frames[vm->frame_count++];
    nf->closure      = cl;
    nf->ip           = callee->code;
    nf->base         = &base[a + 1];
    nf->expected_ret = b;
    nf->defer_base   = vm->defer_count;
    nf->vararg_count = 0;
    nf->varargs      = NULL;
    nf->arena_mark   = vm->frame_arena_used;
    int base_frame   = vm->frame_count - 1;

    if (vm->jit_enabled) {
        void *jf = SEA_JIT_LOAD_FN(callee);
        if (jf == NULL && ++callee->exec_count >= (uint32_t)sea_jit_hot_calls)
            jf = sea_jit_request(vm, callee);
        if (SEA_JIT_IS_CODE(jf)) {
            int nret = ((SeaJittedFn)jf)(vm, nf);
            if (nret == SEA_JIT_SUSPEND) {
                /* cannot happen: callers bail inside goroutines, and only a
                 * goroutine can park */
                fprintf(stderr, "sea: internal error: JIT suspend in nested native call\n");
                abort();
            }
            if (nret != SEA_JIT_BAILOUT) {
                if (vm->open_upvalues) close_upvalues(vm, nf->base);
                vm->frame_arena_used = nf->arena_mark;
                vm->frame_count--;
                deliver_results(&base[a], nf->base, nret, b);
                return 0;
            }
            /* bailout: nf->ip points at the resume instruction; interpret below */
        }
    }
    /* runs nf to completion and pops it; after a bailout caused by a panic in
     * a call nf's native code made, unwinds nf instead */
    vm_run_ex(vm, base_frame, false, vm->panic_propagating);
    if (vm->panic_propagating) return 1;   /* bail: our caller's loop unwinds */
    /* the RETURN left one result (+1) in nf->base[0] (a C-boundary return) */
    if (b > 0) base[a] = nf->base[0];
    else vm_release_val(nf->base[0]);
    return 0;
}

int sea_jit_call(SeaVM *vm, SeaValue *base, int a, int b, int c) {
    sea_gc_maybe_collect(vm);   /* call boundary: a safe point (sea_gc.h) */
    /* Inside a goroutine, a callee may park on a channel and the re-entrant
     * vm_run would return SEA_SUSPENDED — which we cannot resume across the
     * native JIT frame. Bail so the interpreter performs the call inline, where
     * suspension is part of the goroutine's own vm_run. (On the main thread
     * channel ops block the OS thread instead of parking, so this is safe.) */
    if (vm->goroutine != NULL) return 1;
    if (c == 255 || b > 1) return 1;                 /* vararg / multi-return: bail */
    SeaValue fn = base[a];
    if (!sea_is_obj(fn)) return 1;
    SeaObject *o = sea_as_obj(fn);
    if (o->type != OBJ_CLOSURE) return 1;
    SeaClosure *cl = (SeaClosure*)o;
    if (cl->is_native) {
        SeaValue r = call_native(vm, cl, c, &base[a + 1]);
        native_call_done(base, a, b, c, r);
        return vm->panic_propagating ? 1 : 0;
    }
    /* Only run callees that have native code. For an interpreted callee the
     * re-entrant vm_run below is slower than the interpreter's own inline
     * call, so bail and let the interpreter make the call (its call hook also
     * counts the callee towards compilation). Once the callee is compiled, the
     * calling loop stays native on its next OSR entry. */
    if (!SEA_JIT_IS_CODE(SEA_JIT_LOAD_FN(cl->chunk))) return 1;
    return jit_run_callee(vm, cl, base, a, b, c);
}

int sea_jit_call_self(SeaVM *vm, SeaCallFrame *frame, int a, int b, int c) {
    if (vm->goroutine != NULL) return 1;   /* see sea_jit_call: suspension safety */
    if (c == 255 || b > 1) return 1;
    return jit_run_callee(vm, frame->closure, frame->base, a, b, c);
}

/* OP_ARRAY_PUSH for a generic array (R[a].push(R[b])). Bails (returns 1) for
 * typed int/float arrays so the interpreter handles those. */
int sea_jit_apush(SeaVM *vm, SeaValue *base, int a, int b) {
    (void)vm;
    SeaValue arr = base[a];
    if (!sea_is_array(arr)) return 1;
    sea_array_push(SEA_AS_ARRAY(arr), base[b]);
    return 0;
}

/* OP_NEW_ARRAY: R[a] = new array (capacity from R[b] when b != a). */
int sea_jit_new_array(SeaVM *vm, SeaValue *base, int a, int b) {
    (void)vm;
    sea_gc_maybe_collect(vm);   /* allocation: a safe point (sea_gc.h) */
    uint32_t cap = 0;
    if (b != a) {
        SeaValue cv = base[b];
        cap = sea_is_int(cv) ? (uint32_t)sea_as_int(cv) : 0;
    }
    SeaArray *arr = sea_array_new(cap);
    base[a] = sea_obj_val(&arr->obj);
    return 0;
}

/* OP_LOAD_GLOBAL: R[a] = G[key]. `key` is the (stable) constant string value,
 * embedded by the JIT from chunk->constants. */
int sea_jit_load_global(SeaVM *vm, SeaValue *base, int a, SeaValue key, SeaTableIC *ic) {
    SeaValue val = SEA_NIL_VAL;
    sea_table_get_ic_sync(vm_globals(vm), &vm->shared->globals_sync, key, ic, &val);
    base[a] = val;
    return 0;
}

/* OP_RELEASE: drop the local's reference and nil the register (mirrors
 * lbl_RELEASE, including the arena-array teardown). */
int sea_jit_release_reg(SeaVM *vm, SeaValue *base, int a) {
    (void)vm;
    SeaValue v = base[a];
    base[a] = SEA_NIL_VAL;
    vm_release_val(v);
    return 0;
}

/* OP_SET_FIELD_MOVE / OP_ARRAY_PUSH_MOVE (see vm_set_field_move). */
int sea_jit_set_field_move(SeaVM *vm, SeaValue *base, int a, int c, SeaValue key) {
    vm_set_field_move(vm, base, a, c, key);
    return 0;
}
int sea_jit_apush_move(SeaVM *vm, SeaValue *base, int a, int b) {
    (void)vm;
    vm_array_push_move(base, a, b);
    return 0;
}

/* OP_RETAIN / OP_SETLOCAL / OP_APPEND (mirror the interpreter handlers). */
int sea_jit_retain_reg(SeaVM *vm, SeaValue *base, int a) {
    (void)vm;
    vm_retain_val(base[a]);
    return 0;
}
int sea_jit_setlocal(SeaVM *vm, SeaValue *base, int a, int b, int c) {
    (void)vm;
    SeaValue nv = base[b], old = base[a];
    if (c) vm_retain_val(nv);
    base[a] = nv;
    vm_release_val(old);
    return 0;
}
int sea_jit_append(SeaVM *vm, SeaValue *base, int a, int b) {
    vm_append(vm, base, a, b);
    return 0;
}

/* OP_GET_FIELD: R[a] = R[b].key. The generated code saved frame->ip first, so
 * errors raised by table_get report the right line. */
int sea_jit_get_field(SeaVM *vm, SeaValue *base, int a, int b, SeaValue key, SeaTableIC *ic) {
    SeaValue obj = base[b];
    if (sea_is_obj(obj) && sea_as_obj(obj)->type == OBJ_STRUCT) {
        base[a] = sea_struct_get(SEA_AS_STRUCT(obj), SEA_AS_STRING(key));
    } else if (sea_is_obj(obj) && sea_as_obj(obj)->type == OBJ_TABLE) {
        SeaValue v = SEA_NIL_VAL;   /* as lbl_GET_FIELD */
        sea_table_get_ic(SEA_AS_TABLE(obj), key, ic, &v);
        base[a] = v;
    } else
        base[a] = table_get(vm, obj, key);
    return 0;
}

/* OP_SET_FIELD: R[a].key = R[c] (mirrors lbl_SET_FIELD; frame->ip saved). */
int sea_jit_set_field(SeaVM *vm, SeaValue *base, int a, int c, SeaValue key) {
    SeaValue obj = base[a];
    if (sea_is_obj(obj) && sea_as_obj(obj)->type == OBJ_STRUCT) {
        SeaString *key_str = SEA_AS_STRING(key);
        SeaStruct *st = SEA_AS_STRUCT(obj);
        if (st->building_remaining > 0) {
            sea_struct_init_field(st, key_str, base[c]);
        } else if (!sea_struct_set(st, key_str, base[c])) {
            sea_vm_error_noreturn(vm, "attempt to add field '%s' to sealed struct",
                                  key_str->data);
        }
    } else {
        table_set(vm, obj, key, base[c]);
    }
    return 0;
}

/* OP_SET_FIELD_BORROW: R[a].key = R[c] without retaining (arena struct). */
int sea_jit_set_field_borrow(SeaVM *vm, SeaValue *base, int a, int c, SeaValue key) {
    (void)vm;
    sea_struct_init_field(SEA_AS_STRUCT(base[a]), SEA_AS_STRING(key), base[c]);
    return 0;
}

/* Object constructors: R[a] = new NEW_STRUCT(fields=b) / NEW_STRUCT_LOCAL
 * (arena, fields=b) / NEW_TABLE(struct-mode=b) / NEW_MAP — mirroring the
 * interpreter handlers. */
int sea_jit_new_obj(SeaVM *vm, SeaValue *base, int op, int a, int b) {
    sea_gc_maybe_collect(vm);   /* allocation: a safe point (sea_gc.h) */
    switch (op) {
    case OP_NEW_STRUCT: {
        SeaStruct *st = sea_struct_new((uint32_t)b);
        base[a] = sea_obj_val(&st->obj);
        return 0;
    }
    case OP_NEW_STRUCT_LOCAL: {
        size_t needed = sizeof(SeaStruct) + (size_t)(uint32_t)b * (sizeof(SeaString*) + sizeof(SeaValue));
        SeaStruct *st = (SeaStruct*)sea_arena_alloc(vm, needed);
        if (SEA_UNLIKELY(!st)) {
            st = sea_struct_new((uint32_t)b);
        } else {
            st->obj.type           = OBJ_STRUCT;
            st->obj.gc = 0;
            st->obj.refcount       = SEA_RC_ARENA_BIT | 1u;
            st->field_count        = 0;
            st->field_cap          = (uint32_t)b;
            st->_pad               = 0;
            st->building_remaining = (b > 0) ? b : -1;
            if (b > 0) {
                SeaString **names  = sea_struct_names(st);
                SeaValue   *values = sea_struct_values(st);
                for (int j = 0; j < b; j++) { names[j] = NULL; values[j] = SEA_NIL_VAL; }
            }
        }
        base[a] = sea_obj_val(&st->obj);
        return 0;
    }
    case OP_NEW_TABLE: {
        SeaTable *t = sea_table_new(b ? TABLE_STRUCT : TABLE_MAP);
        base[a] = sea_obj_val(&t->obj);
        return 0;
    }
    case OP_NEW_MAP: {
        SeaMap *m = sea_map_new();
        base[a] = sea_obj_val(&m->obj);
        return 0;
    }
    default:
        return 1;
    }
}

/* OP_CONCAT: R[a] = R[b] .. ... (c parts); frame->ip saved by the caller. */
int sea_jit_concat(SeaVM *vm, SeaValue *base, int a, int b, int c) {
    vm_concat(vm, base, a, b, c);
    return 0;
}

/* OP_STORE_GLOBAL: G[key] = R[a] (mirrors lbl_STORE_GLOBAL). */
int sea_jit_store_global(SeaVM *vm, SeaValue *base, int a, SeaValue key) {
    sea_val_mark_shared(base[a]);
    sea_globals_set(vm, key, base[a]);
    return 0;
}

/* OP_FOR_IN_NEXT (see vm_for_in_next). */
int sea_jit_for_in_next(SeaVM *vm, SeaCallFrame *frame, SeaValue *base, int a, int b,
                        uint32_t *resume_ip) {
    return vm_for_in_next(vm, frame, base, resume_ip, a, b) ? SEA_JIT_HELPER_SUSPEND : 0;
}

/* OP_CHAN_SEND: R[a] <- R[b] (mirrors lbl_CHAN_SEND; errors bail so the
 * interpreter re-executes the instruction and raises them). */
int sea_jit_chan_send(SeaVM *vm, SeaCallFrame *frame, SeaValue *base, int a, int b,
                      uint32_t *resume_ip) {
    if (!sea_is_channel(base[a])) return 1;
    SeaChannel *ch = SEA_AS_CHANNEL(base[a]);
    SeaGoroutine *g = vm->goroutine;
    if (g) {
        sea_val_mark_shared(base[b]);
        frame->ip = resume_ip;   /* before becoming visible in a wait queue */
        SeaChanStatus st = sea_chan_send_nb(ch, base[b], g);
        if (st == CHAN_PARKED) return SEA_JIT_HELPER_SUSPEND;
        if (st == CHAN_CLOSED) return 1;
    } else {
        sea_val_mark_shared(base[b]);
        sea_chan_send(ch, base[b]);
    }
    return 0;
}

/* OP_CHAN_RECV: R[a] = <-R[b] (mirrors lbl_CHAN_RECV). */
int sea_jit_chan_recv(SeaVM *vm, SeaCallFrame *frame, SeaValue *base, int a, int b,
                      uint32_t *resume_ip) {
    if (!sea_is_channel(base[b])) return 1;
    SeaChannel *ch = SEA_AS_CHANNEL(base[b]);
    SeaGoroutine *g = vm->goroutine;
    if (g) {
        if (g->has_pending_recv) {
            base[a] = g->block_val;
            g->has_pending_recv = false;
        } else {
            SeaValue val = SEA_NIL_VAL;
            frame->ip = resume_ip;
            SeaChanStatus st = sea_chan_recv_nb(ch, &val, g);
            if (st == CHAN_PARKED) return SEA_JIT_HELPER_SUSPEND;
            base[a] = val;
        }
    } else {
        SeaValue val = SEA_NIL_VAL;
        sea_chan_recv(ch, &val);
        base[a] = val;
    }
    return 0;
}

/* OP_CHAN_CLOSE: close(R[a]). */
int sea_jit_chan_close(SeaVM *vm, SeaValue *base, int a) {
    (void)vm;
    if (sea_is_channel(base[a])) sea_chan_close(SEA_AS_CHANNEL(base[a]));
    return 0;
}

/* OP_GO: spawn R[a](R[b..b+c-1]). */
int sea_jit_go(SeaVM *vm, SeaValue *base, int a, int b, int c) {
    if (!sea_is_closure(base[a])) return 1;   /* bail: the interpreter raises the error */
    vm_retain_val(base[a]);                    /* as lbl_GO */
    sea_goroutine_spawn(vm, base[a], &base[b], c);
    return 0;
}

/* OP_CLOSURE: R[a] = closure(proto[bx]) capturing from this frame (mirrors
 * lbl_CLOSURE). Open upvalues it creates point into frame->base; native
 * returns close them like lbl_RETURN does. */
int sea_jit_closure(SeaVM *vm, SeaCallFrame *frame, SeaValue *base, int a, int bx) {
    sea_gc_maybe_collect(vm);   /* allocation: a safe point (sea_gc.h) */
    SeaChunk *proto = frame->closure->chunk->protos[bx];
    SeaClosure *cl = SEA_ALLOC(SeaClosure);
    cl->obj.type = OBJ_CLOSURE;
    cl->obj.gc = 0;
    cl->obj.refcount = 1;
    cl->chunk = proto;
    cl->is_native = false;
    cl->native = NULL;
    cl->native_name = NULL;
    cl->upval_count = proto->upval_count;
    cl->upvalues = proto->upval_count > 0 ?
        SEA_ALLOC_N(SeaUpvalue*, proto->upval_count) : NULL;
    for (int j = 0; j < proto->upval_count; j++) {
        SeaUpvalDesc *desc = &proto->upvals[j];
        if (desc->is_local) {
            cl->upvalues[j] = capture_upvalue(vm, &base[desc->idx]);
        } else {
            cl->upvalues[j] = frame->closure->upvalues[desc->idx];
            sea_obj_retain(&cl->upvalues[j]->obj);
        }
    }
    base[a] = sea_obj_val(&cl->obj);
    return 0;
}

/* OP_CLOSE_UP: close upvalues from R[a] upward. */
int sea_jit_close_up(SeaVM *vm, SeaValue *base, int a) {
    close_upvalues(vm, &base[a]);
    return 0;
}

/* OP_GET_TABLE / OP_SET_TABLE when the JIT's inline generic-array path does
 * not apply (typed array, table, map, string, out of bounds): exactly the
 * interpreter's fallback. frame->ip is saved by the generated code. */
int sea_jit_get_table(SeaVM *vm, SeaValue *base, int a, int b, int c) {
    base[a] = table_get(vm, base[b], base[c]);
    return 0;
}
int sea_jit_set_table(SeaVM *vm, SeaValue *base, int a, int b, int c) {
    table_set(vm, base[a], base[b], base[c]);
    return 0;
}

/* OP_MARK_CAPTURED (see mark_captured). */
int sea_jit_mark_captured(SeaVM *vm, SeaValue *base, int a) {
    mark_captured(vm, &base[a]);
    return 0;
}

/* OP_DEL / OP_GET_TABLE2 (see vm_del / vm_get_table2). */
int sea_jit_del(SeaVM *vm, SeaValue *base, int a, int b) {
    vm_del(vm, base, a, b);
    return 0;
}
int sea_jit_get_table2(SeaVM *vm, SeaValue *base, int a, int b, int c) {
    (void)vm;
    vm_get_table2(base, a, b, c);
    return 0;
}

/* OP_LOAD_UP / OP_STORE_UP: R[a] = UpV[b] / UpV[b] = R[a]. */
int sea_jit_load_up(SeaCallFrame *frame, SeaValue *base, int a, int b) {
    base[a] = sea_upvalue_get(frame->closure->upvalues[b]);
    return 0;
}
int sea_jit_store_up(SeaCallFrame *frame, SeaValue *base, int a, int b) {
    SeaUpvalue *up = frame->closure->upvalues[b];
    vm_retain_val(base[a]);                                   /* as lbl_STORE_UP */
    SeaValue old;
    if (__atomic_load_n(&up->obj.refcount, __ATOMIC_RELAXED) & SEA_RC_SHARED_BIT) {
        sea_val_mark_shared(base[a]);
        sea_upvalue_lock(up);
        SeaValue *slot = up->location;
        old = *slot;
        __atomic_store_n(slot, base[a], __ATOMIC_RELEASE);
        sea_upvalue_unlock(up);
    } else {
        SeaValue *slot = up->location;
        old = *slot;
        *slot = base[a];
    }
    vm_release_val(old);
    return 0;
}

/* OP_NEW_ARRAY_LOCAL: arena-allocate the header (heap data buffer), mirroring
 * the interpreter; falls back to a heap array if the arena is full. */
int sea_jit_new_array_local(SeaVM *vm, SeaValue *base, int a, int b) {
    sea_gc_maybe_collect(vm);   /* allocation: a safe point (sea_gc.h) */
    uint32_t cap = (uint32_t)(int)b;
    SeaArray *arr = (SeaArray*)sea_arena_alloc(vm, sizeof(SeaArray));
    if (SEA_UNLIKELY(!arr)) {
        arr = sea_array_new(cap);
    } else {
        sea_array_init_header(arr, cap);
        arr->obj.refcount = SEA_RC_ARENA_BIT | 1u;
    }
    base[a] = sea_obj_val(&arr->obj);
    return 0;
}
#endif /* SEA_JIT */

// ---- Public exec API ----

int sea_vm_exec(SeaVM *vm, SeaChunk *chunk) {
    // Create a top-level closure for the chunk
    SeaClosure *cl = SEA_ALLOC(SeaClosure);
    cl->obj.type = OBJ_CLOSURE;
    cl->obj.gc = 0;
    cl->obj.refcount = 1;
    cl->chunk = chunk;
    cl->is_native = false;
    cl->native = NULL;
    cl->native_name = NULL;
    cl->upvalues = NULL;
    cl->upval_count = 0;

    // Set up the first call frame
    SeaCallFrame *frame = &vm->frames[0];
    frame->closure = cl;
    frame->ip = chunk->code;
    frame->base = vm->regs;
    frame->expected_ret = 0;
    frame->arena_mark = 0;
    frame->defer_base = 0;
    frame->vararg_count = 0;
    frame->varargs = NULL;
    vm->frame_count = 1;

    /* The top-level chunk is JIT-compiled like any other once one of its loops
     * gets hot (back-edge counter in vm_run), and entered there via OSR. */
    sea_gc_vm = vm;                  /* cycle buffer of the main goroutine */
    int result = vm_run_ex(vm, 0, true, false);
    sea_obj_release(&cl->obj);
    return result;
}

/*
 * Execute a chunk re-entrantly and capture its top-level return value.
 * Safe to call while the VM is already running (from a native function like
 * require).  The module frame is placed at SEA_MAX_REGISTERS * frame_count
 * so it does not overlap with any currently active frame.
 */
int sea_vm_exec_mod(SeaVM *vm, SeaChunk *chunk, SeaValue *result) {
    *result = SEA_NIL_VAL;

    SeaClosure *cl = SEA_ALLOC(SeaClosure);
    cl->obj.type      = OBJ_CLOSURE;
    cl->obj.gc = 0;
    cl->obj.refcount  = 1;
    cl->chunk         = chunk;
    cl->is_native     = false;
    cl->native        = NULL;
    cl->native_name   = NULL;
    cl->upvalues      = NULL;
    cl->upval_count   = 0;

    int saved_frame  = vm->frame_count;
    int base_reg     = SEA_MAX_REGISTERS * vm->frame_count;

    SeaCallFrame *frame = &vm->frames[vm->frame_count++];
    frame->closure      = cl;
    frame->ip           = chunk->code;
    frame->base         = &vm->regs[base_reg];
    frame->expected_ret = 0;
    frame->varargs      = NULL;
    frame->vararg_count = 0;
    frame->defer_base   = vm->defer_count;
    frame->arena_mark   = vm->frame_arena_used;

    /* Save outer exec-result state so nested sea_vm_exec_mod calls (from
     * require() inside a module body) don't clobber this frame's slot. */
    bool       saved_has    = vm->has_exec_result;
    SeaValue   saved_result = vm->exec_result;

    vm->has_exec_result = true;
    vm->exec_result     = SEA_NIL_VAL;

    int r = vm_run(vm, saved_frame);
    *result             = vm->exec_result;

    /* Restore so the enclosing module's RETURN can still record its value. */
    vm->has_exec_result = saved_has;
    vm->exec_result     = saved_result;

    sea_obj_release(&cl->obj);
    return r;
}

/* Call a Sea function from C. The arguments are borrowed (the caller keeps
 * them); the result is owned (+1): release it or store it. */
SeaValue sea_vm_call(SeaVM *vm, SeaValue fn_val, SeaValue *args, int argc) {
    /* a panic escaped an earlier callback: the native calling us is about to
     * return into the unwinding, so run nothing more */
    if (SEA_UNLIKELY(vm->panic_propagating)) return SEA_NIL_VAL;
    if (!sea_is_obj(fn_val)) {
        fprintf(stderr, "sea: attempt to call a %s value\n", sea_type_name(fn_val));
        return SEA_NIL_VAL;
    }
    SeaObject *obj = sea_as_obj(fn_val);
    if (obj->type != OBJ_CLOSURE) {
        fprintf(stderr, "sea: attempt to call a %s value\n", sea_type_name(fn_val));
        return SEA_NIL_VAL;
    }

    SeaClosure *cl = (SeaClosure*)obj;
    /* Called from C: the C frames cannot be suspended, so natives below this
     * point block instead of parking (sea_netpoll.h). */
    if (cl->is_native) {
        vm->native_depth++;
        SeaValue r = call_native(vm, cl, argc, args);
        vm->native_depth--;
        return r;
    }
    if (vm->frame_count >= SEA_MAX_CALL_DEPTH) {
        sea_vm_error_noreturn(vm, "call stack overflow");
    }

    /* Above every live register — including the arguments of a native that
     * is calling back (they sit in its caller's window). */
    SeaChunk *chunk = cl->chunk;
    SeaValue *fbase = vm_free_base(vm);
    int nva = (chunk->is_vararg && argc > chunk->arity) ? argc - chunk->arity : 0;
    if (fbase + chunk->max_stack + nva > vm->regs + SEA_MAX_REGISTERS * SEA_MAX_CALL_DEPTH) {
        sea_vm_error_noreturn(vm, "call stack overflow");
    }

    SeaCallFrame *frame = &vm->frames[vm->frame_count];
    frame->closure = cl;
    frame->ip = chunk->code;
    frame->base = fbase;
    frame->expected_ret = 1;
    frame->defer_base = vm->defer_count;
    frame->arena_mark = vm->frame_arena_used;
    frame->vararg_count = 0;
    frame->varargs = NULL;
    vm->frame_count++;

    /* The callee owns its parameters (and varargs): retain the borrowed args */
    for (int i = 0; i < argc && i < chunk->arity; i++) {
        frame->base[i] = args[i];
        vm_retain_val(args[i]);
    }
    for (int i = argc; i < chunk->arity; i++) {
        frame->base[i] = SEA_NIL_VAL;
    }
    if (nva > 0) {
        /* on the heap, like every other call: release_varargs frees it */
        frame->varargs = SEA_ALLOC_N(SeaValue, (size_t)nva);
        frame->vararg_count = nva;
        for (int i = 0; i < nva; i++) {
            frame->varargs[i] = args[chunk->arity + i];
            vm_retain_val(frame->varargs[i]);
        }
    }

    int saved_frame = vm->frame_count - 1;  // frame to return to
    /* A module body that calls a native which calls back into Sea
     * (list.map(t, fn) at a module's top level): the callback's RETURN
     * must not take the module's exec-result slot. */
    bool saved_has = vm->has_exec_result;
    vm->has_exec_result = false;
    vm->native_depth++;
    vm_run(vm, saved_frame);
    vm->native_depth--;
    vm->has_exec_result = saved_has;
    return fbase[0];   /* the RETURN left the first result (+1) here */
}
