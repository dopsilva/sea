/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_common.h"
#include "sea_value.h"
#include "sea_object.h"
#include "sea_chunk.h"
#include "sea_table.h"
#include "sea_netpoll.h"

/* Forward declare SeaShared (defined in sea_sched.h) */
typedef struct SeaShared SeaShared;

/*
 * The Sea register-based virtual machine.
 */

// Call frame (one per active function call)
typedef struct {
    SeaClosure *closure;       // function being executed
    uint32_t   *ip;            // instruction pointer
    SeaValue   *base;          // base register for this frame
    int         expected_ret;  // expected number of return values (-1 = any)
    SeaValue   *varargs;       // pointer to vararg values (NULL if none)
    int         vararg_count;  // number of vararg values
    int         defer_base;    // index into vm->defers[] where this frame's defers start
    uint32_t    arena_mark;    // frame_arena watermark at frame entry; restored on return
} SeaCallFrame;

// Open upvalue - captures a local variable by reference
struct SeaUpvalue {
    SeaObject   obj;       // must be first
    SeaValue   *location;  // points into register file while open
    SeaValue    closed;    // value stored here when closed
    SeaUpvalue *next;      // linked list of all open upvalues
    int         lock;      // spinlock: stores and the close of a shared upvalue
};

/* Read an upvalue's value. Another goroutine may close it at the same moment
 * (moving the value from the register into `closed` and clearing the
 * register): if the location changed under the read, read again. (A read
 * that raced with the owner reusing the register after the close is one of
 * those: its value is discarded. TSAN reports that read; it is benign.) */
static inline SeaValue sea_upvalue_get(SeaUpvalue *up) {
    for (;;) {
        SeaValue *loc = __atomic_load_n(&up->location, __ATOMIC_ACQUIRE);
        SeaValue v = __atomic_load_n(loc, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&up->location, __ATOMIC_ACQUIRE) == loc) return v;
    }
}
static inline void sea_upvalue_lock(SeaUpvalue *up) {
    while (__atomic_exchange_n(&up->lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&up->lock, __ATOMIC_RELAXED)) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ __volatile__("yield");
#endif
        }
}
static inline void sea_upvalue_unlock(SeaUpvalue *up) {
    __atomic_store_n(&up->lock, 0, __ATOMIC_RELEASE);
}

// Closure = chunk + captured upvalues
struct SeaClosure {
    SeaObject    obj;        // must be first
    SeaChunk    *chunk;      // bytecode (not owned; owned by proto pool)
    bool         is_native;
    SeaValue   (*native)(SeaVM *vm, int argc, SeaValue *args);
    const char  *native_name;
    SeaUpvalue **upvalues;
    int          upval_count;
};

// Native function wrapper
struct SeaFunction {
    SeaObject   obj;
    const char *name;
    SeaValue  (*fn)(SeaVM *vm, int argc, SeaValue *args);
};

// Error type
struct SeaError {
    SeaObject  obj;
    SeaValue   message;
};

// Deferred call entry
typedef struct {
    SeaValue fn;
    SeaValue args[SEA_DEFER_MAX_ARGS];   // deferred args (compiler enforces the limit)
    int      argc;
} SeaDeferEntry;

struct SeaVM {
    // Register file (large flat array)
    SeaValue    regs[SEA_MAX_REGISTERS * SEA_MAX_CALL_DEPTH];

    // Call stack
    SeaCallFrame frames[SEA_MAX_CALL_DEPTH];
    int          frame_count;
    int          ret_n;          /* results of the last call made with B=255 (return f(x)) */

    // Open upvalue chain (sorted by register address, highest first)
    SeaUpvalue  *open_upvalues;

    // Shared state (globals, loaded modules) — owned by main VM, borrowed by children
    SeaShared   *shared;
    bool         owns_shared;   // true only for the root VM

    // Defer stack (per-vm, cleared on function return)
    SeaDeferEntry defers[256];
    int           defer_count;

    // Panic state (see panic_unwind in vm_run_inner)
    bool      panicking;
    bool      panic_propagating; /* escaped a nested vm_run: the VM loop of the C
                                    caller (native, JIT helper) continues unwinding */
    SeaValue  panic_val;
    char     *panic_trace;       /* traceback captured where the panic started */
    void     *panic_jmp;         /* innermost vm_run's jump buffer (runtime errors) */

    // Error message
    char err_msg[512];

    // Source name for error reporting
    const char *source_name;


    // Captures the top-level return value when sea_vm_exec_mod is running
    SeaValue exec_result;
    bool     has_exec_result;

    /* Frame arena: bump allocator for non-escaping objects (no RC, O(1) free).
     * Each frame saves/restores frame_arena_used as a watermark.
     * _Alignas(8) ensures the base address is 8-byte aligned for SeaStruct etc. */
#define SEA_FRAME_ARENA_SIZE 65536
    _Alignas(8) uint8_t frame_arena[SEA_FRAME_ARENA_SIZE];
    uint32_t frame_arena_used;

    // M:N scheduler: the goroutine currently running in this VM (NULL = main thread)
    SeaGoroutine *goroutine;

    // JIT: per-VM cache of the program-wide SeaShared.jit_enabled, copied at VM
    // init. The hot path (OP_CALL) reads THIS (thread-local) and not the shared
    // copy, to avoid cache contention/false-sharing across goroutine threads.
    bool jit_enabled;

    /* Cycle collector (sea_gc.c): this goroutine's possible cycle roots and
     * statistics. A collection runs at a safe point once gc_nroots reaches
     * gc_threshold. */
    /* Netpoller (sea_netpoll.h): the I/O state of the native call in
     * progress (kept across a park), and how many C-level calls back into
     * the VM are active (sea_vm_call; a native below one cannot park). */
    SeaIoCall   io_call;
    int         native_depth;

    SeaObject **gc_roots;
    uint32_t    gc_nroots, gc_cap, gc_threshold;
    bool        gc_running;
    uint64_t    gc_collections, gc_freed, gc_traversed, gc_buffered;

    /* Sandbox (sea_lib_sandbox.c). A sandboxed VM runs untrusted code: no
     * goroutines or channels, and an instruction budget. The budget is spent
     * at loop back-edges and Sea function calls (the only ways to run
     * forever); when it runs out the VM raises a panic that recover() cannot
     * stop (`killed`), so a script cannot catch it and carry on. Ordinary VMs
     * have `sandboxed` false and the checks cost one predictable branch. */
    bool        sandboxed;
    bool        killed;        /* budget exhausted: the panic in flight is final */
    int64_t     budget;        /* steps left (sandboxed VMs only) */
};

/* Raise the budget panic of a sandboxed VM (does not return). */
void sea_vm_budget_exceeded(SeaVM *vm) SEA_NORETURN;

/* Convenience accessors — keep existing code working with minimal churn */
#define vm_globals(vm)  ((vm)->shared->globals)
#define vm_loaded(vm)   ((vm)->shared->loaded)

/* Globals are read and written by every goroutine: always go through these
 * (never sea_table_set/get on vm_globals directly; see SeaTableSync). */
void sea_globals_set(SeaVM *vm, SeaValue key, SeaValue val);
bool sea_globals_get(SeaVM *vm, SeaValue key, SeaValue *out);
bool sea_globals_get_str(SeaVM *vm, const char *name, SeaValue *out);

// Create/destroy VM
SeaVM*  sea_vm_new(void);
// Create a child VM that shares the parent's SeaShared state.
// Use for goroutines so they see the same globals and modules as the parent.
SeaVM*  sea_vm_new_child(SeaShared *shared);
void    sea_vm_free(SeaVM *vm);
void    sea_vm_reset_child(SeaVM *vm);  /* clean up + zero for pool reuse */

// Execute a compiled chunk in the VM
int     sea_vm_exec(SeaVM *vm, SeaChunk *chunk);

// Execute a chunk and capture its top-level return value (for module loading)
int     sea_vm_exec_mod(SeaVM *vm, SeaChunk *chunk, SeaValue *result);

// Call a Sea function value from C
SeaValue sea_vm_call(SeaVM *vm, SeaValue fn, SeaValue *args, int argc);

/* M:N scheduler helpers (Fase 4):
 *   sea_vm_goroutine_init — push the initial call frame for a goroutine
 *                           without running it yet.
 *   sea_vm_resume         — run vm_run(vm, 0); returns SEA_OK when the
 *                           goroutine's top-level function returns, or
 *                           SEA_SUSPENDED when it parked on a channel. */
/* Returns true when a call frame was pushed (resume with sea_vm_resume), false
 * when there is nothing to resume: a native function, which it has already run
 * to completion. Callers guarantee `fn` is a closure (OP_GO checks). */
bool sea_vm_goroutine_init(SeaVM *vm, SeaValue fn, SeaValue *args, int argc);
int  sea_vm_resume(SeaVM *vm);

// Create a native closure
SeaClosure* sea_native_closure(const char *name, SeaValue (*fn)(SeaVM*, int, SeaValue*));

// Register a native function as a global
void sea_vm_register_native(SeaVM *vm, const char *name,
                             SeaValue (*fn)(SeaVM*, int, SeaValue*));

// Register a module table as a global
void sea_vm_register_module(SeaVM *vm, const char *name, SeaTable *mod);

// Runtime error (longjmp out of VM)
void sea_vm_error(SeaVM *vm, const char *fmt, ...);
void sea_vm_error_noreturn(SeaVM *vm, const char *fmt, ...) SEA_NORETURN;

// Value-to-string (for print/tostring)
SeaString* sea_vm_tostring(SeaVM *vm, SeaValue v);

// Create an error value
SeaValue sea_make_error(SeaVM *vm, SeaValue msg);
SeaValue sea_make_error_str(SeaVM *vm, const char *msg);

/* Start a panic with value v (owned): runtime errors and panic() land here.
 * Unwinds to the innermost running VM loop; recover() in a deferred call stops
 * it. With no VM loop running, prints the traceback and exits. */
SEA_NORETURN void sea_vm_raise(SeaVM *vm, SeaValue v);
/* Print the panic (traceback + value) and exit(1): an unrecovered panic. */
SEA_NORETURN void sea_vm_panic_exit(SeaVM *vm);
/* recover(): take the panic value (nil when not panicking). */
SeaValue sea_vm_recover(SeaVM *vm);
