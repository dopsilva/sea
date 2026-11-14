/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_jit.h"
#include "sea_chunk.h"

int sea_jit_hot_calls = SEA_JIT_THRESHOLD;
int sea_jit_hot_loops = SEA_JIT_LOOP_THRESHOLD;

/*
 * Method JIT backend (libgccjit).  See docs/feature_jit.md.
 *
 * Phase 1: real codegen for the arithmetic/control opcode subset.  A chunk is
 * compiled to one native function with the ABI:
 *
 *     int  jit(SeaVM *vm, SeaCallFrame *frame);
 *
 * It operates directly on frame->base[...] (the register window).  Each bytecode
 * instruction becomes one basic block; supported ops are inlined with the exact
 * fast path of their interpreter handler, and the slow path (type mismatch,
 * errors) is handed back to the interpreter via "bailout": the generated code
 * writes the resume ip into frame->ip and returns SEA_JIT_BAILOUT, after which
 * the OP_CALL dispatch hook continues interpreting from there.
 *
 * On normal completion the function leaves its return values in frame->base[0..]
 * and returns the count.  Chunks containing any unsupported opcode are refused
 * (jit_fn = SEA_JIT_FAILED) so they keep running fully interpreted.
 */

#ifdef SEA_JIT
#include "sea_compat.h"
#include <libgccjit.h>   /* types and prototypes; symbols come from dlopen */
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#include <sys/socket.h>

/* libgccjit is loaded at run time (load_gccjit) rather than linked: it pulls
 * in ~10 MB of shared objects (GCC, isl, mpfr, gmp) whose relocation costs
 * milliseconds at every startup. With the compile server only the forked
 * server process ever loads it; the VM never does unless it has to compile
 * in-process. Every gcc_jit_* call below goes through these pointers. */
#define GCCJIT_API(X) \
    X(gcc_jit_block_add_assignment)\
    X(gcc_jit_block_add_eval)\
    X(gcc_jit_block_end_with_conditional)\
    X(gcc_jit_block_end_with_jump)\
    X(gcc_jit_block_end_with_return)\
    X(gcc_jit_block_end_with_switch)\
    X(gcc_jit_context_acquire)\
    X(gcc_jit_context_add_driver_option)\
    X(gcc_jit_context_compile)\
    X(gcc_jit_context_compile_to_file)\
    X(gcc_jit_context_get_builtin_function)\
    X(gcc_jit_context_get_first_error)\
    X(gcc_jit_context_get_int_type)\
    X(gcc_jit_context_get_type)\
    X(gcc_jit_context_new_array_access)\
    X(gcc_jit_context_new_binary_op)\
    X(gcc_jit_context_new_bitcast)\
    X(gcc_jit_context_new_call)\
    X(gcc_jit_context_new_case)\
    X(gcc_jit_context_new_cast)\
    X(gcc_jit_context_new_comparison)\
    X(gcc_jit_context_new_field)\
    X(gcc_jit_context_new_function)\
    X(gcc_jit_context_new_param)\
    X(gcc_jit_context_new_rvalue_from_double)\
    X(gcc_jit_context_new_rvalue_from_int)\
    X(gcc_jit_context_new_rvalue_from_long)\
    X(gcc_jit_context_new_rvalue_from_ptr)\
    X(gcc_jit_context_new_struct_type)\
    X(gcc_jit_context_new_unary_op)\
    X(gcc_jit_context_release)\
    X(gcc_jit_context_set_int_option)\
    X(gcc_jit_function_new_block)\
    X(gcc_jit_function_new_local)\
    X(gcc_jit_lvalue_as_rvalue)\
    X(gcc_jit_param_as_rvalue)\
    X(gcc_jit_result_get_code)\
    X(gcc_jit_result_release)\
    X(gcc_jit_rvalue_dereference_field)\
    X(gcc_jit_struct_as_type)\
    X(gcc_jit_type_get_pointer)

#define X(f) static __typeof__(&f) p_##f;
GCCJIT_API(X)
#undef X

static pthread_once_t g_gccjit_once = PTHREAD_ONCE_INIT;
static bool           g_gccjit_ok;

static void load_gccjit_once(void) {
    void *h = dlopen("libgccjit.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libgccjit.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        if (getenv("SEA_JIT_DEBUG")) fprintf(stderr, "[jit] cannot load libgccjit: %s\n", dlerror());
        return;
    }
#define X(f) if (!(p_##f = (__typeof__(&f))dlsym(h, #f))) return;
    GCCJIT_API(X)
#undef X
    g_gccjit_ok = true;
}

static bool load_gccjit(void) {
    pthread_once(&g_gccjit_once, load_gccjit_once);
    return g_gccjit_ok;
}

#define gcc_jit_block_add_assignment p_gcc_jit_block_add_assignment
#define gcc_jit_block_add_eval p_gcc_jit_block_add_eval
#define gcc_jit_block_end_with_conditional p_gcc_jit_block_end_with_conditional
#define gcc_jit_block_end_with_jump p_gcc_jit_block_end_with_jump
#define gcc_jit_block_end_with_return p_gcc_jit_block_end_with_return
#define gcc_jit_block_end_with_switch p_gcc_jit_block_end_with_switch
#define gcc_jit_context_acquire p_gcc_jit_context_acquire
#define gcc_jit_context_add_driver_option p_gcc_jit_context_add_driver_option
#define gcc_jit_context_compile p_gcc_jit_context_compile
#define gcc_jit_context_compile_to_file p_gcc_jit_context_compile_to_file
#define gcc_jit_context_get_builtin_function p_gcc_jit_context_get_builtin_function
#define gcc_jit_context_get_first_error p_gcc_jit_context_get_first_error
#define gcc_jit_context_get_int_type p_gcc_jit_context_get_int_type
#define gcc_jit_context_get_type p_gcc_jit_context_get_type
#define gcc_jit_context_new_array_access p_gcc_jit_context_new_array_access
#define gcc_jit_context_new_binary_op p_gcc_jit_context_new_binary_op
#define gcc_jit_context_new_bitcast p_gcc_jit_context_new_bitcast
#define gcc_jit_context_new_call p_gcc_jit_context_new_call
#define gcc_jit_context_new_case p_gcc_jit_context_new_case
#define gcc_jit_context_new_cast p_gcc_jit_context_new_cast
#define gcc_jit_context_new_comparison p_gcc_jit_context_new_comparison
#define gcc_jit_context_new_field p_gcc_jit_context_new_field
#define gcc_jit_context_new_function p_gcc_jit_context_new_function
#define gcc_jit_context_new_param p_gcc_jit_context_new_param
#define gcc_jit_context_new_rvalue_from_double p_gcc_jit_context_new_rvalue_from_double
#define gcc_jit_context_new_rvalue_from_int p_gcc_jit_context_new_rvalue_from_int
#define gcc_jit_context_new_rvalue_from_long p_gcc_jit_context_new_rvalue_from_long
#define gcc_jit_context_new_rvalue_from_ptr p_gcc_jit_context_new_rvalue_from_ptr
#define gcc_jit_context_new_struct_type p_gcc_jit_context_new_struct_type
#define gcc_jit_context_new_unary_op p_gcc_jit_context_new_unary_op
#define gcc_jit_context_release p_gcc_jit_context_release
#define gcc_jit_context_set_int_option p_gcc_jit_context_set_int_option
#define gcc_jit_function_new_block p_gcc_jit_function_new_block
#define gcc_jit_function_new_local p_gcc_jit_function_new_local
#define gcc_jit_lvalue_as_rvalue p_gcc_jit_lvalue_as_rvalue
#define gcc_jit_param_as_rvalue p_gcc_jit_param_as_rvalue
#define gcc_jit_result_get_code p_gcc_jit_result_get_code
#define gcc_jit_result_release p_gcc_jit_result_release
#define gcc_jit_rvalue_dereference_field p_gcc_jit_rvalue_dereference_field
#define gcc_jit_struct_as_type p_gcc_jit_struct_as_type
#define gcc_jit_type_get_pointer p_gcc_jit_type_get_pointer
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sea_value.h"
#include "sea_memory.h"
#include "sea_object.h"   /* OBJ_ARRAY */

/* ---- Which opcodes Phase 1 can compile ------------------------------------ */

static bool op_supported(SeaOpcode op) {
    switch (op) {
        case OP_LOAD_NIL: case OP_LOAD_TRUE: case OP_LOAD_FALSE:
        case OP_LOAD_INT: case OP_LOAD_K:   case OP_MOVE:
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT:
        case OP_IIADD: case OP_IISUB: case OP_ADDI: case OP_SUBI:
        case OP_NEG: case OP_UNM:
        case OP_EQ: case OP_NEQ:
        case OP_LT: case OP_LE: case OP_GT: case OP_GE:
        case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI:
        case OP_NOT:
        case OP_JMP: case OP_JMP_TRUE: case OP_JMP_FALSE: case OP_JMP_NIL:
        case OP_FOR_PREP: case OP_FOR_LOOP:
        case OP_FOR_LOOP_STEP1: case OP_FOR_LOOP_INT:
        case OP_CALL: case OP_CALL_SELF:
        case OP_GET_TABLE: case OP_SET_TABLE:
        case OP_GET_IARRAY: case OP_SET_IARRAY:
        case OP_GET_FARRAY: case OP_SET_FARRAY:
        case OP_ARRAY_PUSH: case OP_NEW_ARRAY: case OP_NEW_ARRAY_LOCAL:
        case OP_MOD_INT: case OP_MOD: case OP_IDIV: case OP_IDIV_INT: case OP_POW:
        case OP_BAND: case OP_BOR: case OP_BXOR: case OP_BNOT: case OP_SHL: case OP_SHR:
        case OP_LOAD_GLOBAL: case OP_STORE_GLOBAL:
        case OP_CONCAT:
        case OP_LOAD_UP: case OP_STORE_UP:
        case OP_GET_FIELD: case OP_SET_FIELD: case OP_SET_FIELD_BORROW:
        case OP_NEW_STRUCT: case OP_NEW_STRUCT_LOCAL: case OP_NEW_TABLE: case OP_NEW_MAP:
        case OP_RELEASE:
        case OP_FOR_IN_INIT: case OP_FOR_IN_NEXT:
        case OP_CHAN_SEND: case OP_CHAN_RECV: case OP_CHAN_CLOSE: case OP_GO:
        case OP_CLOSURE: case OP_CLOSE_UP:
        case OP_DEL: case OP_GET_TABLE2: case OP_MARK_CAPTURED:
        case OP_RETURN: case OP_RETURN0:
        case OP_RETAIN: case OP_SETLOCAL: case OP_APPEND:
        case OP_SET_FIELD_MOVE: case OP_ARRAY_PUSH_MOVE:
            return true;
        default:
            return false;
    }
}

static bool op_is_jump(SeaOpcode op) {
    switch (op) {
        case OP_JMP: case OP_JMP_TRUE: case OP_JMP_FALSE: case OP_JMP_NIL:
        case OP_FOR_PREP: case OP_FOR_LOOP:
        case OP_FOR_LOOP_STEP1: case OP_FOR_LOOP_INT:
            return true;
        default:
            return false;
    }
}

/* Per-instruction compile plan for a chunk: entry[i] is set when native code
 * may be entered at instruction i (OSR). Entry 0 (the call entry) is always
 * set.
 *
 * Unsupported opcodes compile to a bailout (hand control back to the
 * interpreter at that ip). A loop body (the span [t, i] of a backward jump at i
 * to t) is "clean" when every op in it is supported; only clean loops get an
 * OSR entry at their head t, so the interpreter switches to native code exactly
 * in the loops that can stay native. A clean loop nested in a non-clean one
 * keeps its entry: the inner loop runs natively and bails when control reaches
 * the outer loop's unsupported op.
 *
 * Calls count as supported: sea_jit_call runs the callee natively when it is
 * native (C function or compiled chunk) and otherwise bails so the interpreter
 * makes the call (see sea_jit_call).
 *
 * Returns false when compiling is not worthwhile or not safe: a jump target out
 * of range, or no clean loop and some unsupported op (a straight-line chunk
 * with a bailout gains too little to amortize the native entry). */
static bool plan_chunk(SeaChunk *c, uint8_t *entry) {
    int n = c->code_len;
    if (n <= 0) return false;
    memset(entry, 0, (size_t)n);

    bool all_supported = true;
    bool clean_loop = false;

    for (int idx = 0; idx < n; idx++) {
        uint32_t in = c->code[idx];
        SeaOpcode op = (SeaOpcode)SEA_INSTR_OP(in);
        if (!op_supported(op)) { all_supported = false; continue; }
        if (!op_is_jump(op)) continue;
        int t = idx + 1 + SEA_INSTR_sBx(in);
        if (t < 0 || t >= n) return false;     /* bad target */
        if (t > idx) continue;                 /* forward jump */

        int bad = -1;
        for (int j = t; j <= idx && bad < 0; j++)
            if (!op_supported((SeaOpcode)SEA_INSTR_OP(c->code[j]))) bad = j;
        if (bad < 0) {
            entry[t] = 1;
            clean_loop = true;
        } else if (getenv("SEA_JIT_DEBUG")) {
            fprintf(stderr, "[jit] '%s': loop [%d,%d] stays interpreted:",
                    c->name ? c->name : "?", t, idx);
            for (int j = bad; j <= idx; j++) {
                SeaOpcode bop = (SeaOpcode)SEA_INSTR_OP(c->code[j]);
                if (!op_supported(bop)) fprintf(stderr, " %s", sea_opcode_name(bop));
            }
            fputc('\n', stderr);
        }
    }
    entry[0] = 1;
    return clean_loop || all_supported;
}

static bool chunk_compilable(SeaChunk *c) {
    if (c->code_len <= 0) return false;
    uint8_t *entry = (uint8_t*)malloc((size_t)c->code_len);
    bool ok = plan_chunk(c, entry);
    free(entry);
    return ok;
}

/* ---- Codegen context ------------------------------------------------------ */

typedef struct {
    gcc_jit_context  *ctx;
    gcc_jit_function *fn;
    SeaChunk         *chunk;
    int               n;            /* code_len */
    const uint32_t   *code_addr;    /* chunk->code's address in the VM process */
    uintptr_t         ics_addr;     /* chunk->ics (inline caches) in the VM process */
    bool              has_closure;  /* chunk contains OP_CLOSURE (may capture its locals) */
    const uint8_t    *feedback;     /* per instruction: SEA_FB_* seen by the interpreter (or NULL) */

    /* Register promotion (Fase 4): VM registers 0..nregs-1 that no closure
     * captures live in native locals L[n]; frame->base[n] is only brought up
     * to date at sync points (helper operands, bailout, suspend, return). */
    int               nregs;
    int               arity;        /* parameters: the only registers live at call entry */
    uint8_t           captured[32]; /* bitmap: registers captured by closures */
    gcc_jit_lvalue   *L[256];       /* NULL = not promoted (always in memory) */
    gcc_jit_block    *bail_exit;    /* shared: spill all, frame->ip = bail_ip, BAILOUT */
    gcc_jit_lvalue   *bail_ip;
    gcc_jit_block   **blocks;       /* one per instruction */
    gcc_jit_block    *oob;          /* defensive: out-of-range target -> bailout 0 */
    gcc_jit_lvalue   *base_lv;      /* SeaValue* base, loaded in prologue */
    gcc_jit_rvalue   *frame_rv;     /* the `frame` param */
    gcc_jit_rvalue   *vm_rv;        /* the `vm` param */

    /* imported call helpers (sea_jit_call / sea_jit_call_self) */
    gcc_jit_function *fn_call, *fn_call_self;
    /* imported array helpers (push / new / new_local) */
    gcc_jit_function *fn_apush, *fn_newarr, *fn_newarrlocal, *fn_apushmv, *fn_setfieldmv;
    /* imported global-load helper */
    gcc_jit_function *fn_loadglobal;
    /* imported single-opcode helpers */
    gcc_jit_function *fn_release, *fn_getfield, *fn_setfield, *fn_setfieldb, *fn_newobj;
    gcc_jit_function *fn_retain, *fn_setlocal, *fn_append;
    gcc_jit_function *fn_loadup, *fn_storeup, *fn_concat, *fn_storeglobal;
    /* helpers that may suspend the goroutine / closures */
    gcc_jit_function *fn_forin, *fn_csend, *fn_crecv, *fn_cclose, *fn_go;
    gcc_jit_function *fn_closure, *fn_closeup, *fn_del, *fn_gettable2, *fn_markcap;
    gcc_jit_function *fn_gettable, *fn_settable;
    /* math builtins (floor / fmod / pow) */
    gcc_jit_function *fn_floor, *fn_fmod, *fn_pow;

    /* types */
    gcc_jit_type *u64, *i64, *dbl, *t_int, *t_bool, *u64p, *u32p, *voidptr;
    gcc_jit_type *frame_ptr;
    /* frame struct fields we touch */
    gcc_jit_field *f_ip, *f_base;
    /* SeaArray model: { int type@0; u32 rc@4; u32 len@8; u32 cap@12; u64* data@16 } */
    gcc_jit_type  *arr_ptr;
    gcc_jit_field *f_atype, *f_alen, *f_adata;
} JitCtx;

/* ---- small rvalue/lvalue builders ----------------------------------------- */

static gcc_jit_rvalue *cu64(JitCtx *J, uint64_t v) {
    return gcc_jit_context_new_rvalue_from_long(J->ctx, J->u64, (long)v);
}
static gcc_jit_rvalue *ci64(JitCtx *J, int64_t v) {
    return gcc_jit_context_new_rvalue_from_long(J->ctx, J->i64, (long)v);
}
static gcc_jit_rvalue *cint(JitCtx *J, int v) {
    return gcc_jit_context_new_rvalue_from_int(J->ctx, J->t_int, v);
}
/* frame->base[n] in memory. */
static gcc_jit_lvalue *mem_lv(JitCtx *J, int n) {
    return gcc_jit_context_new_array_access(J->ctx, NULL,
        gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, n));
}
/* Register n as the generated code sees it: its native local when promoted,
 * else the memory slot. */
static gcc_jit_lvalue *reg_lv(JitCtx *J, int n) {
    if (n >= 0 && n < 256 && J->L[n]) return J->L[n];
    return mem_lv(J, n);
}
static gcc_jit_rvalue *reg(JitCtx *J, int n) {
    return gcc_jit_lvalue_as_rvalue(reg_lv(J, n));
}
static void store(JitCtx *J, gcc_jit_block *blk, int n, gcc_jit_rvalue *val) {
    gcc_jit_block_add_assignment(blk, NULL, reg_lv(J, n), val);
}
static gcc_jit_rvalue *bin(JitCtx *J, enum gcc_jit_binary_op op,
                           gcc_jit_type *t, gcc_jit_rvalue *a, gcc_jit_rvalue *b) {
    return gcc_jit_context_new_binary_op(J->ctx, NULL, op, t, a, b);
}
static gcc_jit_rvalue *cmp(JitCtx *J, enum gcc_jit_comparison op,
                           gcc_jit_rvalue *a, gcc_jit_rvalue *b) {
    return gcc_jit_context_new_comparison(J->ctx, NULL, op, a, b);
}

/* ---- NaN-box helpers, mirroring sea_value.h ------------------------------- */

static gcc_jit_rvalue *is_int(JitCtx *J, gcc_jit_rvalue *v) {
    gcc_jit_rvalue *m = bin(J, GCC_JIT_BINARY_OP_BITWISE_AND, J->u64, v,
                            cu64(J, SEA_NAN_BASE | SEA_TAG_MASK));
    return cmp(J, GCC_JIT_COMPARISON_EQ, m, cu64(J, SEA_NAN_BASE | SEA_TAG_INT));
}
static gcc_jit_rvalue *is_number(JitCtx *J, gcc_jit_rvalue *v) {
    gcc_jit_rvalue *m = bin(J, GCC_JIT_BINARY_OP_BITWISE_AND, J->u64, v, cu64(J, SEA_NAN_BASE));
    return cmp(J, GCC_JIT_COMPARISON_NE, m, cu64(J, SEA_NAN_BASE));
}
static gcc_jit_rvalue *is_nil(JitCtx *J, gcc_jit_rvalue *v) {
    return cmp(J, GCC_JIT_COMPARISON_EQ, v, cu64(J, SEA_NIL_VAL));
}
static gcc_jit_rvalue *is_obj(JitCtx *J, gcc_jit_rvalue *v) {
    gcc_jit_rvalue *m = bin(J, GCC_JIT_BINARY_OP_BITWISE_AND, J->u64, v,
                            cu64(J, SEA_NAN_BASE | SEA_TAG_MASK));
    return cmp(J, GCC_JIT_COMPARISON_EQ, m, cu64(J, SEA_NAN_BASE | SEA_TAG_OBJ));
}
/* v is not a heap object: storing or overwriting it needs no refcounting. */
static gcc_jit_rvalue *not_obj(JitCtx *J, gcc_jit_rvalue *v) {
    gcc_jit_rvalue *m = bin(J, GCC_JIT_BINARY_OP_BITWISE_AND, J->u64, v,
                            cu64(J, SEA_NAN_BASE | SEA_TAG_MASK));
    return cmp(J, GCC_JIT_COMPARISON_NE, m, cu64(J, SEA_NAN_BASE | SEA_TAG_OBJ));
}
/* (SeaArray*)(v & PTR_MASK) — only valid after is_obj(v) is established. */
static gcc_jit_rvalue *arr_ptr_of(JitCtx *J, gcc_jit_rvalue *v) {
    gcc_jit_rvalue *p = bin(J, GCC_JIT_BINARY_OP_BITWISE_AND, J->u64, v, cu64(J, SEA_PTR_MASK));
    return gcc_jit_context_new_bitcast(J->ctx, NULL, p, J->arr_ptr);
}
/* The element slot arr->data[ix] (typed as u64 — raw SeaValue / int64 / double bits). */
static gcc_jit_lvalue *arr_slot(JitCtx *J, gcc_jit_rvalue *arrptr, gcc_jit_rvalue *ix) {
    gcc_jit_rvalue *data = gcc_jit_lvalue_as_rvalue(
        gcc_jit_rvalue_dereference_field(arrptr, NULL, J->f_adata));
    return gcc_jit_context_new_array_access(J->ctx, NULL, data, ix);
}
static gcc_jit_rvalue *as_int(JitCtx *J, gcc_jit_rvalue *v) {
    /* libgccjit binary ops require both operands to share a type, so the shift
     * amounts must match the value's type (u64 then i64), not plain int. */
    gcc_jit_rvalue *shl = bin(J, GCC_JIT_BINARY_OP_LSHIFT, J->u64, v, cu64(J, 16));
    gcc_jit_rvalue *sgn = gcc_jit_context_new_cast(J->ctx, NULL, shl, J->i64);
    return bin(J, GCC_JIT_BINARY_OP_RSHIFT, J->i64, sgn, ci64(J, 16));
}
static gcc_jit_rvalue *int_val(JitCtx *J, gcc_jit_rvalue *i /*i64*/) {
    gcc_jit_rvalue *u = gcc_jit_context_new_cast(J->ctx, NULL, i, J->u64);
    gcc_jit_rvalue *masked = bin(J, GCC_JIT_BINARY_OP_BITWISE_AND, J->u64, u, cu64(J, SEA_PTR_MASK));
    return bin(J, GCC_JIT_BINARY_OP_BITWISE_OR, J->u64, masked, cu64(J, SEA_NAN_BASE | SEA_TAG_INT));
}
static gcc_jit_rvalue *as_number(JitCtx *J, gcc_jit_rvalue *v) {
    return gcc_jit_context_new_bitcast(J->ctx, NULL, v, J->dbl);
}
/* a double as a value; a NaN in the tag space (x86's 0/0 is the nil
 * pattern) becomes SEA_CANON_NAN, as sea_number_val does — without a branch:
 * u ^ ((u ^ CANON) & -(tagged)) */
static gcc_jit_rvalue *number_val(JitCtx *J, gcc_jit_rvalue *d) {
    gcc_jit_rvalue *u = gcc_jit_context_new_bitcast(J->ctx, NULL, d, J->u64);
    gcc_jit_rvalue *top = bin(J, GCC_JIT_BINARY_OP_BITWISE_AND, J->u64, u, cu64(J, SEA_NAN_BASE));
    gcc_jit_rvalue *tagged = gcc_jit_context_new_cast(J->ctx, NULL,
        cmp(J, GCC_JIT_COMPARISON_EQ, top, cu64(J, SEA_NAN_BASE)), J->u64);
    gcc_jit_rvalue *mask = bin(J, GCC_JIT_BINARY_OP_MINUS, J->u64, cu64(J, 0), tagged);
    gcc_jit_rvalue *diff = bin(J, GCC_JIT_BINARY_OP_BITWISE_XOR, J->u64, u, cu64(J, SEA_CANON_NAN));
    return bin(J, GCC_JIT_BINARY_OP_BITWISE_XOR, J->u64, u,
               bin(J, GCC_JIT_BINARY_OP_BITWISE_AND, J->u64, diff, mask));
}
static gcc_jit_rvalue *bool_val(JitCtx *J, gcc_jit_rvalue *b /*bool*/) {
    gcc_jit_rvalue *u = gcc_jit_context_new_cast(J->ctx, NULL, b, J->u64);
    return bin(J, GCC_JIT_BINARY_OP_BITWISE_OR, J->u64, u, cu64(J, SEA_FALSE_VAL));
}
static gcc_jit_rvalue *is_truthy(JitCtx *J, gcc_jit_rvalue *v) {
    gcc_jit_rvalue *nn = cmp(J, GCC_JIT_COMPARISON_NE, v, cu64(J, SEA_NIL_VAL));
    gcc_jit_rvalue *nf = cmp(J, GCC_JIT_COMPARISON_NE, v, cu64(J, SEA_FALSE_VAL));
    return bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool, nn, nf);
}

/* ---- control-flow helpers ------------------------------------------------- */

static gcc_jit_block *blk_at(JitCtx *J, int idx) {
    if (idx >= 0 && idx < J->n) return J->blocks[idx];
    /* Out-of-range edge (defensive): lazily create a single bailout block so it
     * is only present when actually referenced (libgccjit rejects unreachable
     * blocks). chunk_compilable() keeps all real targets in range. */
    if (!J->oob) {
        J->oob = gcc_jit_function_new_block(J->fn, "oob");
        gcc_jit_block_end_with_return(J->oob, NULL, cint(J, SEA_JIT_BAILOUT));
    }
    return J->oob;
}
static gcc_jit_block *newblk(JitCtx *J) {
    return gcc_jit_function_new_block(J->fn, NULL);
}
static void go(JitCtx *J, gcc_jit_block *blk, int target) {
    gcc_jit_block_end_with_jump(blk, NULL, blk_at(J, target));
}
static void cgo(JitCtx *J, gcc_jit_block *blk, gcc_jit_rvalue *cond,
                int t_target, int f_target) {
    gcc_jit_block_end_with_conditional(blk, NULL, cond, blk_at(J, t_target), blk_at(J, f_target));
}
/* Sync promoted register n: local -> memory (spill) / memory -> local (reload). */
static void spill(JitCtx *J, gcc_jit_block *blk, int n) {
    if (n >= 0 && n < 256 && J->L[n])
        gcc_jit_block_add_assignment(blk, NULL, mem_lv(J, n), gcc_jit_lvalue_as_rvalue(J->L[n]));
}
static void reload(JitCtx *J, gcc_jit_block *blk, int n) {
    if (n >= 0 && n < 256 && J->L[n])
        gcc_jit_block_add_assignment(blk, NULL, J->L[n], gcc_jit_lvalue_as_rvalue(mem_lv(J, n)));
}
static void spill_all(JitCtx *J, gcc_jit_block *blk) {
    for (int n = 0; n < J->nregs; n++) spill(J, blk, n);
}

/* Registers a helper-backed instruction reads (rd) and writes (wr) through
 * frame->base — kept here, next to each other, so the sync around every helper
 * call follows from the instruction alone. Registers captured by closures are
 * never promoted, so effects through upvalues need no entry. */
typedef struct { uint8_t rd[256], wr[256]; } RegSets;
static void rs_range(uint8_t *set, int lo, int hi) {
    for (int r = lo; r <= hi && r < 256; r++) if (r >= 0) set[r] = 1;
}
static void op_regs(uint32_t in, RegSets *rs) {
    memset(rs, 0, sizeof *rs);
    /* reads: the canonical table derived from the VM handlers (shared with the
     * compiler's dead-write elimination), so the two can't drift apart */
    SeaRegRanges rr;
    sea_op_reads(in, &rr);
    for (int k = 0; k < rr.n; k++) rs_range(rs->rd, rr.lo[k], rr.hi[k]);
    /* writes: what each helper stores into frame->base */
    int a = SEA_INSTR_A(in), b = SEA_INSTR_B(in);
    switch ((SeaOpcode)SEA_INSTR_OP(in)) {
    case OP_LOAD_GLOBAL: case OP_NEW_ARRAY_LOCAL: case OP_CLOSURE: case OP_LOAD_UP:
    case OP_NEW_STRUCT: case OP_NEW_STRUCT_LOCAL: case OP_NEW_TABLE: case OP_NEW_MAP:
    case OP_NEW_ARRAY: case OP_RELEASE: case OP_GET_FIELD: case OP_CHAN_RECV:
    case OP_CONCAT: case OP_GET_TABLE:   /* GET_TABLE: the table_get fallback helper */
    case OP_SETLOCAL: case OP_APPEND:
        rs->wr[a] = 1; break;
    case OP_GET_TABLE2:  rs_range(rs->wr, a, a + 1); break;
    case OP_FOR_IN_NEXT: rs_range(rs->wr, a, a + 1 + b); break;
    case OP_CALL: case OP_CALL_SELF:
        /* the result (b <= 1, else the helper bails); the callee's frame
         * starts at a+1, so registers above a are dead temps */
        if (b > 0) rs->wr[a] = 1;
        break;
    default: break;   /* readers only (stores, sends, ...); CLOSE_UP touches
                         only captured registers, which live in memory */
    }
}
static void spill_reads(JitCtx *J, gcc_jit_block *blk, const RegSets *rs) {
    for (int n = 0; n < J->nregs; n++) if (rs->rd[n]) spill(J, blk, n);
}
static void reload_writes(JitCtx *J, gcc_jit_block *blk, const RegSets *rs) {
    for (int n = 0; n < J->nregs; n++) if (rs->wr[n]) reload(J, blk, n);
}

/* End `blk` by bailing out at instruction idx. The interpreter resumes from
 * frame->base, so every promoted register is written back first — done once,
 * in a single exit block shared by all bail sites (a spill-all per guard
 * bloated the code enough to hurt GCC's register allocation). */
static void emit_bail(JitCtx *J, gcc_jit_block *blk, int idx) {
    if (!J->bail_exit) {
        J->bail_ip = gcc_jit_function_new_local(J->fn, NULL, J->u32p, "bail_ip");
        J->bail_exit = gcc_jit_function_new_block(J->fn, "bail_exit");
        spill_all(J, J->bail_exit);
        gcc_jit_block_add_assignment(J->bail_exit, NULL,
            gcc_jit_rvalue_dereference_field(J->frame_rv, NULL, J->f_ip),
            gcc_jit_lvalue_as_rvalue(J->bail_ip));
        gcc_jit_block_end_with_return(J->bail_exit, NULL, cint(J, SEA_JIT_BAILOUT));
    }
    gcc_jit_block_add_assignment(blk, NULL, J->bail_ip,
        gcc_jit_context_new_rvalue_from_ptr(J->ctx, J->u32p, (void*)(J->code_addr + idx)));
    gcc_jit_block_end_with_jump(blk, NULL, J->bail_exit);
}

/* Address (VM process) of instruction idx's inline-cache slot. */
static gcc_jit_rvalue *ic_slot(JitCtx *J, int idx) {
    return gcc_jit_context_new_rvalue_from_ptr(J->ctx, J->voidptr,
        (void*)(J->ics_addr + (uintptr_t)idx * sizeof(struct SeaTableICSlot)));
}

/* frame->ip = chunk->code + idx — what the interpreter's SAVE_IP() leaves
 * (idx = the instruction after the current one) before a helper that can
 * raise an error or push frames, so tracebacks and error lines are right. */
static void save_ip(JitCtx *J, gcc_jit_block *blk, int idx) {
    gcc_jit_lvalue *ipf = gcc_jit_rvalue_dereference_field(J->frame_rv, NULL, J->f_ip);
    gcc_jit_block_add_assignment(blk, NULL, ipf,
        gcc_jit_context_new_rvalue_from_ptr(J->ctx, J->u32p, (void*)(J->code_addr + idx)));
}

/* Common shape: in `blk`, branch on `ok`; the false edge bails, the true edge
 * is returned to the caller to fill in and terminate. */
static gcc_jit_block *fast_or_bail(JitCtx *J, gcc_jit_block *blk,
                                   gcc_jit_rvalue *ok, int idx) {
    gcc_jit_block *fast = newblk(J);
    gcc_jit_block *bail = newblk(J);
    gcc_jit_block_end_with_conditional(blk, NULL, ok, fast, bail);
    emit_bail(J, bail, idx);
    return fast;
}

/* Emit a call to a helper (sea_jit_call / sea_jit_call_self): if it returns 0
 * the call was handled (continue to `next`); if non-zero, bail to the
 * interpreter at this instruction. The call is assigned to a local so it is
 * evaluated exactly once, for its side effects. */
static void emit_call(JitCtx *J, gcc_jit_block *blk, gcc_jit_function *helper,
                      gcc_jit_rvalue **args, int nargs, int idx, int next) {
    RegSets rs;
    op_regs(J->chunk->code[idx], &rs);
    spill_reads(J, blk, &rs);
    gcc_jit_rvalue *call = gcc_jit_context_new_call(J->ctx, NULL, helper, nargs, args);
    char name[24];
    snprintf(name, sizeof(name), "rc%d", idx);   /* locals require a non-NULL name */
    gcc_jit_lvalue *rc = gcc_jit_function_new_local(J->fn, NULL, J->t_int, name);
    gcc_jit_block_add_assignment(blk, NULL, rc, call);
    gcc_jit_block *cont = newblk(J);
    gcc_jit_block *bail = newblk(J);
    gcc_jit_block_end_with_conditional(blk, NULL,
        cmp(J, GCC_JIT_COMPARISON_EQ, gcc_jit_lvalue_as_rvalue(rc), cint(J, 0)), cont, bail);
    emit_bail(J, bail, idx);   /* helpers bail before any side effect */
    reload_writes(J, cont, &rs);
    go(J, cont, next);
}

/* Like emit_call, for helpers that can also park the goroutine: 0 continues
 * to `next`, SEA_JIT_HELPER_SUSPEND returns SEA_JIT_SUSPEND (the helper set
 * frame->ip to the resume point), anything else bails at `idx`. */
static void emit_call_susp(JitCtx *J, gcc_jit_block *blk, gcc_jit_function *helper,
                           gcc_jit_rvalue **args, int nargs, int idx, int next) {
    RegSets rs;
    op_regs(J->chunk->code[idx], &rs);
    spill_reads(J, blk, &rs);
    gcc_jit_rvalue *call = gcc_jit_context_new_call(J->ctx, NULL, helper, nargs, args);
    char name[24];
    snprintf(name, sizeof(name), "rs%d", idx);
    gcc_jit_lvalue *rc = gcc_jit_function_new_local(J->fn, NULL, J->t_int, name);
    gcc_jit_block_add_assignment(blk, NULL, rc, call);
    gcc_jit_rvalue *rcv = gcc_jit_lvalue_as_rvalue(rc);
    gcc_jit_block *cont = newblk(J), *notok = newblk(J), *susp = newblk(J), *bail = newblk(J);
    gcc_jit_block_end_with_conditional(blk, NULL, cmp(J, GCC_JIT_COMPARISON_EQ, rcv, cint(J, 0)),
                                       cont, notok);
    gcc_jit_block_end_with_conditional(notok, NULL,
        cmp(J, GCC_JIT_COMPARISON_EQ, rcv, cint(J, SEA_JIT_HELPER_SUSPEND)), susp, bail);
    spill_all(J, susp);          /* the interpreter resumes from memory */
    gcc_jit_block_end_with_return(susp, NULL, cint(J, SEA_JIT_SUSPEND));
    emit_bail(J, bail, idx);
    reload_writes(J, cont, &rs);
    go(J, cont, next);
}

/* Emit the guard chain for an array element base[obj_reg][base[key_reg]]:
 * is_obj(obj) && is_int(key), then a type check, then 0 <= idx < len.
 * `type_const` is a specific OBJ_* tag, or -1 to accept any of the three array
 * kinds (generic OBJ_ARRAY, OBJ_INT_ARRAY, OBJ_FLOAT_ARRAY) — they share the
 * { type@0, len@8, data@16 } header. Any failure bails to the interpreter at
 * `idx` (which handles maps/structs/strings/OOB). Bailing — rather than calling
 * a table_get helper and rejoining — matters: a helper that writes base[] on a
 * path back into the loop forces every register to be reloaded from memory
 * each iteration (measured: sort 3.3x -> 2.35x). Returns the in-bounds block;
 * sets *arrptr, *ix, and (if non-NULL) *type_out for element dispatch. */
static gcc_jit_block *emit_arr_access(JitCtx *J, gcc_jit_block *blk,
                                      int obj_reg, int key_reg, int idx, int type_const,
                                      gcc_jit_rvalue **arrptr_out, gcc_jit_rvalue **ix_out,
                                      gcc_jit_rvalue **type_out, gcc_jit_block **type_miss) {
    gcc_jit_rvalue *pre = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                              is_obj(J, reg(J, obj_reg)), is_int(J, reg(J, key_reg)));
    gcc_jit_block *t1 = newblk(J), *b1 = newblk(J);
    gcc_jit_block_end_with_conditional(blk, NULL, pre, t1, b1);
    emit_bail(J, b1, idx);

    gcc_jit_rvalue *arrptr = arr_ptr_of(J, reg(J, obj_reg));
    gcc_jit_rvalue *typ = gcc_jit_context_new_cast(J->ctx, NULL, gcc_jit_lvalue_as_rvalue(
        gcc_jit_rvalue_dereference_field(arrptr, NULL, J->f_atype)), J->t_int);
    gcc_jit_rvalue *typecond;
    if (type_const < 0) {
        typecond = bin(J, GCC_JIT_BINARY_OP_LOGICAL_OR, J->t_bool,
            bin(J, GCC_JIT_BINARY_OP_LOGICAL_OR, J->t_bool,
                cmp(J, GCC_JIT_COMPARISON_EQ, typ, cint(J, OBJ_ARRAY)),
                cmp(J, GCC_JIT_COMPARISON_EQ, typ, cint(J, OBJ_INT_ARRAY))),
            cmp(J, GCC_JIT_COMPARISON_EQ, typ, cint(J, OBJ_FLOAT_ARRAY)));
    } else {
        typecond = cmp(J, GCC_JIT_COMPARISON_EQ, typ, cint(J, type_const));
    }
    gcc_jit_block *t2 = newblk(J), *b2 = newblk(J);
    gcc_jit_block_end_with_conditional(t1, NULL, typecond, t2, b2);
    if (type_miss) *type_miss = b2;   /* caller handles other object types */
    else emit_bail(J, b2, idx);

    gcc_jit_rvalue *ix = as_int(J, reg(J, key_reg));
    gcc_jit_rvalue *len = gcc_jit_lvalue_as_rvalue(
        gcc_jit_rvalue_dereference_field(arrptr, NULL, J->f_alen));
    gcc_jit_rvalue *inb = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
        cmp(J, GCC_JIT_COMPARISON_GE, ix, ci64(J, 0)),
        cmp(J, GCC_JIT_COMPARISON_LT,
            gcc_jit_context_new_cast(J->ctx, NULL, ix, J->u64),
            gcc_jit_context_new_cast(J->ctx, NULL, len, J->u64)));
    gcc_jit_block *t3 = newblk(J), *b3 = newblk(J);
    gcc_jit_block_end_with_conditional(t2, NULL, inb, t3, b3);
    emit_bail(J, b3, idx);

    *arrptr_out = arrptr;
    *ix_out = ix;
    if (type_out) *type_out = typ;
    return t3;
}

/* Generic GET_TABLE / SET_TABLE on a typed array: the object is not a generic
 * array (the inline fast path's guard failed at `miss`), but an int_array or
 * float_array — which the compiler cannot know when the array comes from an
 * upvalue, a parameter or a table field — is still handled inline (same
 * header layout), so such loops don't bail on every access. Anything else
 * bails. For a get, `val_reg` < 0; for a set it is the value register. */
static void emit_typed_fallback(JitCtx *J, gcc_jit_block *miss, gcc_jit_rvalue *arrptr,
                                gcc_jit_rvalue *typ, gcc_jit_rvalue *ix, int dst_reg,
                                int val_reg, int idx, int next) {
    gcc_jit_rvalue *len = gcc_jit_context_new_cast(J->ctx, NULL,
        gcc_jit_lvalue_as_rvalue(gcc_jit_rvalue_dereference_field(arrptr, NULL, J->f_alen)), J->u64);
    gcc_jit_rvalue *inb = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
        cmp(J, GCC_JIT_COMPARISON_GE, ix, ci64(J, 0)),
        cmp(J, GCC_JIT_COMPARISON_LT, gcc_jit_context_new_cast(J->ctx, NULL, ix, J->u64), len));
    gcc_jit_block *isf = newblk(J), *notf = newblk(J), *isi = newblk(J), *bail = newblk(J);
    gcc_jit_block_end_with_conditional(miss, NULL,
        cmp(J, GCC_JIT_COMPARISON_EQ, typ, cint(J, OBJ_FLOAT_ARRAY)), isf, notf);
    gcc_jit_block_end_with_conditional(notf, NULL,
        cmp(J, GCC_JIT_COMPARISON_EQ, typ, cint(J, OBJ_INT_ARRAY)), isi, bail);
    emit_bail(J, bail, idx);
    for (int k = 0; k < 2; k++) {
        bool is_float = (k == 0);
        gcc_jit_block *blk = fast_or_bail(J, is_float ? isf : isi, inb, idx);
        gcc_jit_lvalue *slot = arr_slot(J, arrptr, ix);
        if (val_reg < 0) {
            gcc_jit_rvalue *e = gcc_jit_lvalue_as_rvalue(slot);
            store(J, blk, dst_reg, is_float ? e   /* double bits == number SeaValue */
                : int_val(J, gcc_jit_context_new_cast(J->ctx, NULL, e, J->i64)));
        } else {
            blk = fast_or_bail(J, blk, is_float ? is_number(J, reg(J, val_reg))
                                                : is_int(J, reg(J, val_reg)), idx);
            gcc_jit_block_add_assignment(blk, NULL, slot, is_float ? reg(J, val_reg)
                : gcc_jit_context_new_cast(J->ctx, NULL, as_int(J, reg(J, val_reg)), J->u64));
        }
        go(J, blk, next);
    }
}

/* Coerce R[r] to a double like the interpreter's to_number(): int -> (double),
 * number as is, anything else bails at `idx` (the interpreter raises the
 * error). Returns the continuation block; *out is the value. */
static gcc_jit_block *emit_to_double(JitCtx *J, gcc_jit_block *blk, int r, int idx,
                                     gcc_jit_rvalue **out) {
    char name[32];
    snprintf(name, sizeof name, "d%d_%d", idx, r);
    gcc_jit_lvalue *d = gcc_jit_function_new_local(J->fn, NULL, J->dbl, name);
    gcc_jit_block *ib = newblk(J), *nb, *chk = newblk(J), *done = newblk(J);
    gcc_jit_block_end_with_conditional(blk, NULL, is_int(J, reg(J, r)), ib, chk);
    gcc_jit_block_add_assignment(ib, NULL, d,
        gcc_jit_context_new_cast(J->ctx, NULL, as_int(J, reg(J, r)), J->dbl));
    gcc_jit_block_end_with_jump(ib, NULL, done);
    nb = fast_or_bail(J, chk, is_number(J, reg(J, r)), idx);
    gcc_jit_block_add_assignment(nb, NULL, d, as_number(J, reg(J, r)));
    gcc_jit_block_end_with_jump(nb, NULL, done);
    *out = gcc_jit_lvalue_as_rvalue(d);
    return done;
}

/* Coerce R[r] to an int64 like the interpreter's to_int(): int as is, number
 * truncated, anything else bails. */
static gcc_jit_block *emit_to_int(JitCtx *J, gcc_jit_block *blk, int r, int idx,
                                  gcc_jit_rvalue **out) {
    char name[32];
    snprintf(name, sizeof name, "i%d_%d", idx, r);
    gcc_jit_lvalue *v = gcc_jit_function_new_local(J->fn, NULL, J->i64, name);
    gcc_jit_block *ib = newblk(J), *nb, *chk = newblk(J), *done = newblk(J);
    gcc_jit_block_end_with_conditional(blk, NULL, is_int(J, reg(J, r)), ib, chk);
    gcc_jit_block_add_assignment(ib, NULL, v, as_int(J, reg(J, r)));
    gcc_jit_block_end_with_jump(ib, NULL, done);
    nb = fast_or_bail(J, chk, is_number(J, reg(J, r)), idx);
    gcc_jit_block_add_assignment(nb, NULL, v,
        gcc_jit_context_new_cast(J->ctx, NULL, as_number(J, reg(J, r)), J->i64));
    gcc_jit_block_end_with_jump(nb, NULL, done);
    *out = gcc_jit_lvalue_as_rvalue(v);
    return done;
}

static gcc_jit_rvalue *call1(JitCtx *J, gcc_jit_function *f, gcc_jit_rvalue *x) {
    return gcc_jit_context_new_call(J->ctx, NULL, f, 1, &x);
}
static gcc_jit_rvalue *call2(JitCtx *J, gcc_jit_function *f, gcc_jit_rvalue *x, gcc_jit_rvalue *y) {
    gcc_jit_rvalue *args[2] = { x, y };
    return gcc_jit_context_new_call(J->ctx, NULL, f, 2, args);
}

/* ---- per-instruction emission --------------------------------------------- */

static void emit_instr(JitCtx *J, int idx) {
    uint32_t in = J->chunk->code[idx];
    SeaOpcode op = (SeaOpcode)SEA_INSTR_OP(in);
    int a = SEA_INSTR_A(in), b = SEA_INSTR_B(in), c = SEA_INSTR_C(in);
    gcc_jit_block *blk = J->blocks[idx];
    int next = idx + 1;

    switch (op) {
    case OP_LOAD_NIL: {
        for (int j = 0; j < b; j++) store(J, blk, a + j, cu64(J, SEA_NIL_VAL));
        go(J, blk, next);
        break;
    }
    case OP_LOAD_TRUE:  store(J, blk, a, cu64(J, SEA_TRUE_VAL));  go(J, blk, next); break;
    case OP_LOAD_FALSE: store(J, blk, a, cu64(J, SEA_FALSE_VAL)); go(J, blk, next); break;
    case OP_LOAD_INT: {
        store(J, blk, a, int_val(J, ci64(J, (int64_t)SEA_INSTR_sBx(in))));
        go(J, blk, next);
        break;
    }
    case OP_LOAD_K: {
        store(J, blk, a, cu64(J, (uint64_t)J->chunk->constants[SEA_INSTR_Bx(in)]));
        go(J, blk, next);
        break;
    }
    case OP_MOVE: store(J, blk, a, reg(J, b)); go(J, blk, next); break;
    case OP_LOAD_GLOBAL: {   /* R[a] = G[K[Bx]] (delegated; key is a stable constant) */
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a),
            cu64(J, (uint64_t)J->chunk->constants[SEA_INSTR_Bx(in)]), ic_slot(J, idx)
        };
        emit_call(J, blk, J->fn_loadglobal, args, 5, idx, next);
        break;
    }

    /* generic arithmetic: inline int+int and number+number (mirroring val_add),
     * bail on anything else (mixed int/float, string concat, errors). */
    case OP_ADD: case OP_SUB: case OP_MUL: {
        enum gcc_jit_binary_op aop = (op == OP_ADD) ? GCC_JIT_BINARY_OP_PLUS
                                   : (op == OP_SUB) ? GCC_JIT_BINARY_OP_MINUS
                                                    : GCC_JIT_BINARY_OP_MULT;
        gcc_jit_block *int_blk = newblk(J), *chknum = newblk(J);
        gcc_jit_rvalue *ii = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                                 is_int(J, reg(J, b)), is_int(J, reg(J, c)));
        gcc_jit_block_end_with_conditional(blk, NULL, ii, int_blk, chknum);
        /* int path */
        store(J, int_blk, a, int_val(J, bin(J, aop, J->i64, as_int(J, reg(J, b)), as_int(J, reg(J, c)))));
        go(J, int_blk, next);
        /* number path, else bail */
        gcc_jit_rvalue *nn = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                                 is_number(J, reg(J, b)), is_number(J, reg(J, c)));
        gcc_jit_block *fast = fast_or_bail(J, chknum, nn, idx);
        store(J, fast, a, number_val(J, bin(J, aop, J->dbl, as_number(J, reg(J, b)), as_number(J, reg(J, c)))));
        go(J, fast, next);
        break;
    }
    /* int-typed arithmetic: int fast path, else bail */
    case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT: {
        gcc_jit_rvalue *ok = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                                 is_int(J, reg(J, b)), is_int(J, reg(J, c)));
        gcc_jit_block *fast = fast_or_bail(J, blk, ok, idx);
        enum gcc_jit_binary_op aop = (op == OP_ADD_INT) ? GCC_JIT_BINARY_OP_PLUS
                                   : (op == OP_SUB_INT) ? GCC_JIT_BINARY_OP_MINUS
                                                        : GCC_JIT_BINARY_OP_MULT;
        gcc_jit_rvalue *r = bin(J, aop, J->i64, as_int(J, reg(J, b)), as_int(J, reg(J, c)));
        store(J, fast, a, int_val(J, r));
        go(J, fast, next);
        break;
    }
    /* compiler-proven int+/-: raw 48-bit add/sub, no checks */
    case OP_IIADD: case OP_IISUB: {
        gcc_jit_rvalue *bv = bin(J, GCC_JIT_BINARY_OP_BITWISE_AND, J->u64, reg(J, b), cu64(J, SEA_PTR_MASK));
        gcc_jit_rvalue *cv = bin(J, GCC_JIT_BINARY_OP_BITWISE_AND, J->u64, reg(J, c), cu64(J, SEA_PTR_MASK));
        gcc_jit_rvalue *s  = bin(J, (op == OP_IIADD) ? GCC_JIT_BINARY_OP_PLUS : GCC_JIT_BINARY_OP_MINUS,
                                 J->u64, bv, cv);
        gcc_jit_rvalue *sm = bin(J, GCC_JIT_BINARY_OP_BITWISE_AND, J->u64, s, cu64(J, SEA_PTR_MASK));
        store(J, blk, a, bin(J, GCC_JIT_BINARY_OP_BITWISE_OR, J->u64, sm, cu64(J, SEA_NAN_BASE | SEA_TAG_INT)));
        go(J, blk, next);
        break;
    }
    /* int + immediate (int8): int fast path, else bail */
    case OP_ADDI: case OP_SUBI: {
        int8_t imm = (int8_t)c;
        gcc_jit_block *fast = fast_or_bail(J, blk, is_int(J, reg(J, b)), idx);
        gcc_jit_rvalue *r = bin(J, (op == OP_ADDI) ? GCC_JIT_BINARY_OP_PLUS : GCC_JIT_BINARY_OP_MINUS,
                                J->i64, as_int(J, reg(J, b)), ci64(J, imm));
        store(J, fast, a, int_val(J, r));
        go(J, fast, next);
        break;
    }
    case OP_NEG: case OP_UNM: {
        /* int -> int, number -> number, else bail (interpreter raises) */
        gcc_jit_block *ib = newblk(J), *chk = newblk(J);
        gcc_jit_block_end_with_conditional(blk, NULL, is_int(J, reg(J, b)), ib, chk);
        store(J, ib, a, int_val(J, gcc_jit_context_new_unary_op(J->ctx, NULL,
                                   GCC_JIT_UNARY_OP_MINUS, J->i64, as_int(J, reg(J, b)))));
        go(J, ib, next);
        gcc_jit_block *nb = fast_or_bail(J, chk, is_number(J, reg(J, b)), idx);
        store(J, nb, a, number_val(J, gcc_jit_context_new_unary_op(J->ctx, NULL,
                                   GCC_JIT_UNARY_OP_MINUS, J->dbl, as_number(J, reg(J, b)))));
        go(J, nb, next);
        break;
    }

    /* equality: int (bit) and number (float) fast paths, else bail */
    case OP_EQ: case OP_NEQ: {
        gcc_jit_block *both_int = newblk(J), *chknum = newblk(J);
        gcc_jit_rvalue *ii = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                                 is_int(J, reg(J, b)), is_int(J, reg(J, c)));
        gcc_jit_block_end_with_conditional(blk, NULL, ii, both_int, chknum);

        enum gcc_jit_comparison ceq = (op == OP_EQ) ? GCC_JIT_COMPARISON_EQ : GCC_JIT_COMPARISON_NE;
        /* int path: bitwise equality */
        store(J, both_int, a, bool_val(J, cmp(J, ceq, reg(J, b), reg(J, c))));
        go(J, both_int, next);
        /* number path (else bail) */
        gcc_jit_rvalue *nn = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                                 is_number(J, reg(J, b)), is_number(J, reg(J, c)));
        gcc_jit_block *fast = fast_or_bail(J, chknum, nn, idx);
        store(J, fast, a, bool_val(J, cmp(J, ceq, as_number(J, reg(J, b)), as_number(J, reg(J, c)))));
        go(J, fast, next);
        break;
    }
    /* ordering: int and number (float) fast paths, else bail (mixed/string) */
    case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
        enum gcc_jit_comparison cc = (op == OP_LT) ? GCC_JIT_COMPARISON_LT
                                   : (op == OP_LE) ? GCC_JIT_COMPARISON_LE
                                   : (op == OP_GT) ? GCC_JIT_COMPARISON_GT
                                                   : GCC_JIT_COMPARISON_GE;
        gcc_jit_block *int_blk = newblk(J), *chknum = newblk(J);
        gcc_jit_rvalue *ii = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                                 is_int(J, reg(J, b)), is_int(J, reg(J, c)));
        gcc_jit_block_end_with_conditional(blk, NULL, ii, int_blk, chknum);
        store(J, int_blk, a, bool_val(J, cmp(J, cc, as_int(J, reg(J, b)), as_int(J, reg(J, c)))));
        go(J, int_blk, next);
        gcc_jit_rvalue *nn = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                                 is_number(J, reg(J, b)), is_number(J, reg(J, c)));
        gcc_jit_block *fast = fast_or_bail(J, chknum, nn, idx);
        store(J, fast, a, bool_val(J, cmp(J, cc, as_number(J, reg(J, b)), as_number(J, reg(J, c)))));
        go(J, fast, next);
        break;
    }
    /* float division: like the interpreter, both operands coerce to double
     * (int or number); mixed/non-numeric bails. No zero-check (x/0 = inf). */
    case OP_DIV: {
        gcc_jit_block *int_blk = newblk(J), *chknum = newblk(J);
        gcc_jit_rvalue *ii = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                                 is_int(J, reg(J, b)), is_int(J, reg(J, c)));
        gcc_jit_block_end_with_conditional(blk, NULL, ii, int_blk, chknum);
        gcc_jit_rvalue *da = gcc_jit_context_new_cast(J->ctx, NULL, as_int(J, reg(J, b)), J->dbl);
        gcc_jit_rvalue *db = gcc_jit_context_new_cast(J->ctx, NULL, as_int(J, reg(J, c)), J->dbl);
        store(J, int_blk, a, number_val(J, bin(J, GCC_JIT_BINARY_OP_DIVIDE, J->dbl, da, db)));
        go(J, int_blk, next);
        gcc_jit_rvalue *nn = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                                 is_number(J, reg(J, b)), is_number(J, reg(J, c)));
        gcc_jit_block *fast = fast_or_bail(J, chknum, nn, idx);
        store(J, fast, a, number_val(J, bin(J, GCC_JIT_BINARY_OP_DIVIDE, J->dbl,
                                            as_number(J, reg(J, b)), as_number(J, reg(J, c)))));
        go(J, fast, next);
        break;
    }
    case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI: {
        int8_t imm = (int8_t)c;
        gcc_jit_block *fast = fast_or_bail(J, blk, is_int(J, reg(J, b)), idx);
        enum gcc_jit_comparison cc = (op == OP_LTI) ? GCC_JIT_COMPARISON_LT
                                   : (op == OP_LEI) ? GCC_JIT_COMPARISON_LE
                                   : (op == OP_GTI) ? GCC_JIT_COMPARISON_GT
                                                    : GCC_JIT_COMPARISON_GE;
        store(J, fast, a, bool_val(J, cmp(J, cc, as_int(J, reg(J, b)), ci64(J, imm))));
        go(J, fast, next);
        break;
    }
    case OP_NOT:
        store(J, blk, a, bool_val(J,
            gcc_jit_context_new_unary_op(J->ctx, NULL, GCC_JIT_UNARY_OP_LOGICAL_NEGATE,
                                         J->t_bool, is_truthy(J, reg(J, b)))));
        go(J, blk, next);
        break;

    /* ---- jumps ---- */
    case OP_JMP:
        go(J, blk, idx + 1 + SEA_INSTR_sBx(in));
        break;
    case OP_JMP_TRUE:
        cgo(J, blk, is_truthy(J, reg(J, a)), idx + 1 + SEA_INSTR_sBx(in), next);
        break;
    case OP_JMP_FALSE:
        /* if !truthy jump: truthy => fall through, else => jump */
        cgo(J, blk, is_truthy(J, reg(J, a)), next, idx + 1 + SEA_INSTR_sBx(in));
        break;
    case OP_JMP_NIL:
        cgo(J, blk, is_nil(J, reg(J, a)), idx + 1 + SEA_INSTR_sBx(in), next);
        break;

    /* ---- numeric for ---- */
    case OP_FOR_PREP: {
        int target = idx + 1 + SEA_INSTR_sBx(in);
        gcc_jit_rvalue *allint = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
            bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool, is_int(J, reg(J, a)), is_int(J, reg(J, a + 1))),
            is_int(J, reg(J, a + 2)));
        gcc_jit_block *fast = fast_or_bail(J, blk, allint, idx);
        store(J, fast, a + 3, reg(J, a));   /* user var = idx */
        gcc_jit_block *pos = newblk(J), *neg = newblk(J);
        gcc_jit_block_end_with_conditional(fast, NULL,
            cmp(J, GCC_JIT_COMPARISON_GT, as_int(J, reg(J, a + 2)), ci64(J, 0)), pos, neg);
        /* skip(=jump past loop) when the body would not run */
        cgo(J, pos, cmp(J, GCC_JIT_COMPARISON_GT, as_int(J, reg(J, a)), as_int(J, reg(J, a + 1))), target, next);
        cgo(J, neg, cmp(J, GCC_JIT_COMPARISON_LT, as_int(J, reg(J, a)), as_int(J, reg(J, a + 1))), target, next);
        break;
    }
    case OP_FOR_LOOP: {
        int target = idx + 1 + SEA_INSTR_sBx(in);
        gcc_jit_rvalue *allint = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
            bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool, is_int(J, reg(J, a)), is_int(J, reg(J, a + 1))),
            is_int(J, reg(J, a + 2)));
        gcc_jit_block *fast = fast_or_bail(J, blk, allint, idx);
        gcc_jit_block *pos = newblk(J), *neg = newblk(J);
        gcc_jit_block_end_with_conditional(fast, NULL,
            cmp(J, GCC_JIT_COMPARISON_GT, as_int(J, reg(J, a + 2)), ci64(J, 0)), pos, neg);
        /* positive step: decode, signed compare (the raw NaN-boxed words
         * do not order negative payloads below positive ones, and -1 + 1
         * carries into the tag) */
        {
            gcc_jit_rvalue *idxv = bin(J, GCC_JIT_BINARY_OP_PLUS, J->i64,
                                       as_int(J, reg(J, a)), as_int(J, reg(J, a + 2)));
            store(J, pos, a, int_val(J, idxv));
            gcc_jit_block *cont = newblk(J);
            gcc_jit_block_end_with_conditional(pos, NULL,
                cmp(J, GCC_JIT_COMPARISON_LE, as_int(J, reg(J, a)), as_int(J, reg(J, a + 1))), cont, blk_at(J, next));
            store(J, cont, a + 3, reg(J, a));
            go(J, cont, target);
        }
        /* negative step: decode, signed compare */
        {
            gcc_jit_rvalue *idxv = bin(J, GCC_JIT_BINARY_OP_PLUS, J->i64,
                                       as_int(J, reg(J, a)), as_int(J, reg(J, a + 2)));
            store(J, neg, a, int_val(J, idxv));
            gcc_jit_block *cont = newblk(J);
            gcc_jit_block_end_with_conditional(neg, NULL,
                cmp(J, GCC_JIT_COMPARISON_GE, as_int(J, reg(J, a)), as_int(J, reg(J, a + 1))), cont, blk_at(J, next));
            store(J, cont, a + 3, reg(J, a));
            go(J, cont, target);
        }
        break;
    }
    case OP_FOR_LOOP_STEP1: {
        int target = idx + 1 + SEA_INSTR_sBx(in);
        gcc_jit_rvalue *idxv = bin(J, GCC_JIT_BINARY_OP_PLUS, J->i64, as_int(J, reg(J, a)), ci64(J, 1));
        store(J, blk, a, int_val(J, idxv));
        gcc_jit_block *cont = newblk(J);
        gcc_jit_block_end_with_conditional(blk, NULL,
            cmp(J, GCC_JIT_COMPARISON_LE, as_int(J, reg(J, a)), as_int(J, reg(J, a + 1))), cont, blk_at(J, next));
        store(J, cont, a + 3, reg(J, a));
        go(J, cont, target);
        break;
    }
    case OP_FOR_LOOP_INT: {
        int target = idx + 1 + SEA_INSTR_sBx(in);
        gcc_jit_block *pos = newblk(J), *neg = newblk(J);
        gcc_jit_block_end_with_conditional(blk, NULL,
            cmp(J, GCC_JIT_COMPARISON_GT, as_int(J, reg(J, a + 2)), ci64(J, 0)), pos, neg);
        {
            gcc_jit_rvalue *idxv = bin(J, GCC_JIT_BINARY_OP_PLUS, J->i64,
                                       as_int(J, reg(J, a)), as_int(J, reg(J, a + 2)));
            store(J, pos, a, int_val(J, idxv));
            gcc_jit_block *cont = newblk(J);
            gcc_jit_block_end_with_conditional(pos, NULL,
                cmp(J, GCC_JIT_COMPARISON_LE, as_int(J, reg(J, a)), as_int(J, reg(J, a + 1))), cont, blk_at(J, next));
            store(J, cont, a + 3, reg(J, a));
            go(J, cont, target);
        }
        {
            gcc_jit_rvalue *idxv = bin(J, GCC_JIT_BINARY_OP_PLUS, J->i64,
                                       as_int(J, reg(J, a)), as_int(J, reg(J, a + 2)));
            store(J, neg, a, int_val(J, idxv));
            gcc_jit_block *cont = newblk(J);
            gcc_jit_block_end_with_conditional(neg, NULL,
                cmp(J, GCC_JIT_COMPARISON_GE, as_int(J, reg(J, a)), as_int(J, reg(J, a + 1))), cont, blk_at(J, next));
            store(J, cont, a + 3, reg(J, a));
            go(J, cont, target);
        }
        break;
    }

    /* ---- array element get/set (inline fast path; bail to interp otherwise) ----
     * Generic arrays store SeaValues; int_array/float_array store raw int64/double.
     * The header layout (type@0, len@8, data@16) is identical, so all three reuse
     * the same guard (emit_arr_access) with a different type tag; only the element
     * transform differs. A float's raw bits == its number SeaValue, so float_array
     * get/set is a plain u64 load/store. */
    /* Generic-array fast path first (single type check); only when that check
     * fails are int_array/float_array tried (emit_typed_fallback), so generic
     * array loops pay nothing for it. The dedicated GET/SET_IARRAY/FARRAY
     * opcodes are emitted when the compiler can prove the type. */
    case OP_GET_TABLE:    /* R[a] = R[b][R[c]] (generic array) */
    case OP_GET_IARRAY:   /* int_array (raw int64 element) */
    case OP_GET_FARRAY: { /* float_array (raw double bits == number SeaValue) */
        int tc = (op == OP_GET_TABLE) ? OBJ_ARRAY
               : (op == OP_GET_IARRAY) ? OBJ_INT_ARRAY : OBJ_FLOAT_ARRAY;
        gcc_jit_rvalue *arrptr, *ix, *typ;
        gcc_jit_block *miss = NULL;
        gcc_jit_block *acc = emit_arr_access(J, blk, b, c, idx, tc, &arrptr, &ix, &typ,
                                             op == OP_GET_TABLE ? &miss : NULL);
        if (miss) {
            /* Not a generic array. What happens next depends on what this
             * site has seen in the interpreter (type feedback): an extra path
             * that rejoins the loop costs the generic-array fast path (sort
             * 4.3x -> 3.2x), so only sites that needed one get it. */
            uint8_t fb = J->feedback ? J->feedback[idx] : 0;
            if (fb & SEA_FB_OTHER) {          /* tables/maps/...: table_get helper */
                save_ip(J, miss, next);
                gcc_jit_rvalue *args[5] = {
                    J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b), cint(J, c)
                };
                emit_call(J, miss, J->fn_gettable, args, 5, idx, next);
            } else if (fb & SEA_FB_TYPED_ARRAY) {   /* int/float_array: inline */
                emit_typed_fallback(J, miss, arrptr, typ, ix, a, -1, idx, next);
            } else {
                emit_bail(J, miss, idx);
            }
        }
        gcc_jit_rvalue *elem = gcc_jit_lvalue_as_rvalue(arr_slot(J, arrptr, ix));
        gcc_jit_rvalue *out = (op == OP_GET_IARRAY)
            ? int_val(J, gcc_jit_context_new_cast(J->ctx, NULL, elem, J->i64))
            : elem;
        store(J, acc, a, out);
        go(J, acc, next);
        break;
    }
    case OP_SET_TABLE:    /* R[a][R[b]] = R[c] (generic array) */
    case OP_SET_IARRAY:
    case OP_SET_FARRAY: {
        int tc = (op == OP_SET_TABLE) ? OBJ_ARRAY
               : (op == OP_SET_IARRAY) ? OBJ_INT_ARRAY : OBJ_FLOAT_ARRAY;
        gcc_jit_rvalue *arrptr, *ix, *typ;
        gcc_jit_block *miss = NULL;
        gcc_jit_block *acc = emit_arr_access(J, blk, a, b, idx, tc, &arrptr, &ix, &typ,
                                             op == OP_SET_TABLE ? &miss : NULL);
        if (miss) {   /* as for GET_TABLE: chosen by type feedback */
            uint8_t fb = J->feedback ? J->feedback[idx] : 0;
            if (fb & SEA_FB_OTHER) {
                save_ip(J, miss, next);
                gcc_jit_rvalue *args[5] = {
                    J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b), cint(J, c)
                };
                emit_call(J, miss, J->fn_settable, args, 5, idx, next);
            } else if (fb & SEA_FB_TYPED_ARRAY) {
                emit_typed_fallback(J, miss, arrptr, typ, ix, -1, c, idx, next);
            } else {
                emit_bail(J, miss, idx);
            }
        }
        gcc_jit_rvalue *val;
        if (op == OP_SET_IARRAY) {
            acc = fast_or_bail(J, acc, is_int(J, reg(J, c)), idx);
            val = gcc_jit_context_new_cast(J->ctx, NULL, as_int(J, reg(J, c)), J->u64);
        } else if (op == OP_SET_FARRAY) {
            acc = fast_or_bail(J, acc, is_number(J, reg(J, c)), idx);
            val = reg(J, c);
        } else {
            val = reg(J, c);
            /* the array owns its elements: storing an object or overwriting
             * one needs retain/release — leave that to the helper */
            gcc_jit_block *plain = newblk(J), *rc = newblk(J);
            gcc_jit_rvalue *old = gcc_jit_lvalue_as_rvalue(arr_slot(J, arrptr, ix));
            gcc_jit_block_end_with_conditional(acc, NULL,
                bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool, not_obj(J, val), not_obj(J, old)),
                plain, rc);
            save_ip(J, rc, next);
            gcc_jit_rvalue *args[5] = {
                J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b), cint(J, c)
            };
            emit_call(J, rc, J->fn_settable, args, 5, idx, next);
            acc = plain;
        }
        gcc_jit_block_add_assignment(acc, NULL, arr_slot(J, arrptr, ix), val);
        go(J, acc, next);
        break;
    }

    /* ---- array construction (delegated to C helpers) ---- */
    case OP_ARRAY_PUSH: {   /* R[a].push(R[b]) */
        gcc_jit_rvalue *args[4] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b)
        };
        emit_call(J, blk, J->fn_apush, args, 4, idx, next);
        break;
    }
    case OP_ARRAY_PUSH_MOVE: {  /* R[a].push(R[b]), consuming R[b] */
        gcc_jit_rvalue *args[4] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b)
        };
        emit_call(J, blk, J->fn_apushmv, args, 4, idx, next);
        break;
    }
    case OP_NEW_ARRAY: {    /* R[a] = new array(cap from R[b] if b!=a) */
        gcc_jit_rvalue *args[4] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b)
        };
        emit_call(J, blk, J->fn_newarr, args, 4, idx, next);
        break;
    }
    case OP_NEW_ARRAY_LOCAL: {  /* R[a] = new arena array(cap=B) */
        gcc_jit_rvalue *args[4] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b)
        };
        emit_call(J, blk, J->fn_newarrlocal, args, 4, idx, next);
        break;
    }

    /* ---- modulo (floored) and integer division ----
     * int,int: both floor (toward minus infinity), exactly as the
     * interpreter; a zero divisor bails so the interpreter raises "division by
     * zero". Otherwise both coerce to double: MOD = fmod adjusted to the sign
     * of the divisor, // = floor(b / c). Non-numeric operands bail. */
    case OP_MOD_INT: case OP_MOD:
    case OP_IDIV_INT: case OP_IDIV: {
        bool is_mod = (op == OP_MOD_INT || op == OP_MOD);
        gcc_jit_block *ib = newblk(J), *fb = newblk(J);
        gcc_jit_rvalue *ii = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                                 is_int(J, reg(J, b)), is_int(J, reg(J, c)));
        gcc_jit_block_end_with_conditional(blk, NULL, ii, ib, fb);

        /* int path */
        gcc_jit_rvalue *nc = as_int(J, reg(J, c));
        gcc_jit_block *nz = fast_or_bail(J, ib, cmp(J, GCC_JIT_COMPARISON_NE, nc, ci64(J, 0)), idx);
        if (!is_mod) {
            /* q = b / c; if (b % c != 0 && (b ^ c) < 0) q -= 1 */
            gcc_jit_rvalue *nb = as_int(J, reg(J, b));
            char qn[24]; snprintf(qn, sizeof(qn), "idiv%d", idx);
            gcc_jit_lvalue *q = gcc_jit_function_new_local(J->fn, NULL, J->i64, qn);
            gcc_jit_block_add_assignment(nz, NULL, q, bin(J, GCC_JIT_BINARY_OP_DIVIDE, J->i64, nb, nc));
            gcc_jit_rvalue *need = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                cmp(J, GCC_JIT_COMPARISON_NE, bin(J, GCC_JIT_BINARY_OP_MODULO, J->i64, nb, nc), ci64(J, 0)),
                cmp(J, GCC_JIT_COMPARISON_LT,
                    bin(J, GCC_JIT_BINARY_OP_BITWISE_XOR, J->i64, nb, nc), ci64(J, 0)));
            gcc_jit_block *adj = newblk(J), *done = newblk(J);
            gcc_jit_block_end_with_conditional(nz, NULL, need, adj, done);
            gcc_jit_block_add_assignment(adj, NULL, q,
                bin(J, GCC_JIT_BINARY_OP_MINUS, J->i64, gcc_jit_lvalue_as_rvalue(q), ci64(J, 1)));
            gcc_jit_block_end_with_jump(adj, NULL, done);
            store(J, done, a, int_val(J, gcc_jit_lvalue_as_rvalue(q)));
            go(J, done, next);
        } else {
            /* r = b % c; if (r != 0 && (r ^ c) < 0) r += c */
            gcc_jit_rvalue *r0 = bin(J, GCC_JIT_BINARY_OP_MODULO, J->i64, as_int(J, reg(J, b)), nc);
            char rn[24]; snprintf(rn, sizeof(rn), "mod%d", idx);
            gcc_jit_lvalue *r = gcc_jit_function_new_local(J->fn, NULL, J->i64, rn);
            gcc_jit_block_add_assignment(nz, NULL, r, r0);
            gcc_jit_rvalue *rv = gcc_jit_lvalue_as_rvalue(r);
            gcc_jit_rvalue *need = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                cmp(J, GCC_JIT_COMPARISON_NE, rv, ci64(J, 0)),
                cmp(J, GCC_JIT_COMPARISON_LT,
                    bin(J, GCC_JIT_BINARY_OP_BITWISE_XOR, J->i64, rv, nc), ci64(J, 0)));
            gcc_jit_block *adj = newblk(J), *done = newblk(J);
            gcc_jit_block_end_with_conditional(nz, NULL, need, adj, done);
            gcc_jit_block_add_assignment(adj, NULL, r, bin(J, GCC_JIT_BINARY_OP_PLUS, J->i64, rv, nc));
            gcc_jit_block_end_with_jump(adj, NULL, done);
            store(J, done, a, int_val(J, rv));
            go(J, done, next);
        }

        /* float path */
        gcc_jit_rvalue *db, *dc;
        fb = emit_to_double(J, fb, b, idx, &db);
        fb = emit_to_double(J, fb, c, idx, &dc);
        if (!is_mod) {
            store(J, fb, a, number_val(J, call1(J, J->fn_floor,
                bin(J, GCC_JIT_BINARY_OP_DIVIDE, J->dbl, db, dc))));
            go(J, fb, next);
        } else {
            /* r = fmod(b, c); if (r != 0 && (r < 0) != (c < 0)) r += c */
            char rn[24]; snprintf(rn, sizeof(rn), "fmod%d", idx);
            gcc_jit_lvalue *r = gcc_jit_function_new_local(J->fn, NULL, J->dbl, rn);
            gcc_jit_block_add_assignment(fb, NULL, r, call2(J, J->fn_fmod, db, dc));
            gcc_jit_rvalue *rv = gcc_jit_lvalue_as_rvalue(r);
            gcc_jit_rvalue *zero = gcc_jit_context_new_rvalue_from_double(J->ctx, J->dbl, 0.0);
            gcc_jit_rvalue *need = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool,
                cmp(J, GCC_JIT_COMPARISON_NE, rv, zero),
                cmp(J, GCC_JIT_COMPARISON_NE,
                    cmp(J, GCC_JIT_COMPARISON_LT, rv, zero),
                    cmp(J, GCC_JIT_COMPARISON_LT, dc, zero)));
            gcc_jit_block *adj = newblk(J), *done = newblk(J);
            gcc_jit_block_end_with_conditional(fb, NULL, need, adj, done);
            gcc_jit_block_add_assignment(adj, NULL, r, bin(J, GCC_JIT_BINARY_OP_PLUS, J->dbl, rv, dc));
            gcc_jit_block_end_with_jump(adj, NULL, done);
            store(J, done, a, number_val(J, rv));
            go(J, done, next);
        }
        break;
    }
    case OP_POW: {      /* always a number: pow(to_number(b), to_number(c)) */
        gcc_jit_rvalue *db, *dc;
        blk = emit_to_double(J, blk, b, idx, &db);
        blk = emit_to_double(J, blk, c, idx, &dc);
        store(J, blk, a, number_val(J, call2(J, J->fn_pow, db, dc)));
        go(J, blk, next);
        break;
    }

    /* ---- bitwise: operands coerce like to_int() (int, or truncated number) ---- */
    case OP_BAND: case OP_BOR: case OP_BXOR: {
        gcc_jit_rvalue *ib, *ic;
        blk = emit_to_int(J, blk, b, idx, &ib);
        blk = emit_to_int(J, blk, c, idx, &ic);
        enum gcc_jit_binary_op bop = (op == OP_BAND) ? GCC_JIT_BINARY_OP_BITWISE_AND
                                   : (op == OP_BOR)  ? GCC_JIT_BINARY_OP_BITWISE_OR
                                                     : GCC_JIT_BINARY_OP_BITWISE_XOR;
        store(J, blk, a, int_val(J, bin(J, bop, J->i64, ib, ic)));
        go(J, blk, next);
        break;
    }
    case OP_BNOT: {
        gcc_jit_rvalue *ib;
        blk = emit_to_int(J, blk, b, idx, &ib);
        store(J, blk, a, int_val(J, gcc_jit_context_new_unary_op(J->ctx, NULL,
                                       GCC_JIT_UNARY_OP_BITWISE_NEGATE, J->i64, ib)));
        go(J, blk, next);
        break;
    }
    case OP_SHL: case OP_SHR: {
        /* |shift| >= 64 -> 0; SHL by s>=0 is <<, by s<0 a logical >>; SHR mirrors */
        gcc_jit_rvalue *vb, *sh;
        blk = emit_to_int(J, blk, b, idx, &vb);
        blk = emit_to_int(J, blk, c, idx, &sh);
        gcc_jit_block *big = newblk(J), *small = newblk(J), *pos = newblk(J), *neg = newblk(J);
        gcc_jit_rvalue *out_of_range = bin(J, GCC_JIT_BINARY_OP_LOGICAL_OR, J->t_bool,
            cmp(J, GCC_JIT_COMPARISON_GE, sh, ci64(J, 64)),
            cmp(J, GCC_JIT_COMPARISON_LE, sh, ci64(J, -64)));
        gcc_jit_block_end_with_conditional(blk, NULL, out_of_range, big, small);
        store(J, big, a, int_val(J, ci64(J, 0)));
        go(J, big, next);
        gcc_jit_block_end_with_conditional(small, NULL,
            cmp(J, GCC_JIT_COMPARISON_GE, sh, ci64(J, 0)), pos, neg);
        gcc_jit_rvalue *nsh = gcc_jit_context_new_unary_op(J->ctx, NULL, GCC_JIT_UNARY_OP_MINUS, J->i64, sh);
        /* left shift on u64 (no signed-overflow UB), logical right shift on u64 */
        gcc_jit_rvalue *ub = gcc_jit_context_new_cast(J->ctx, NULL, vb, J->u64);
        gcc_jit_rvalue *lsh_pos = bin(J, GCC_JIT_BINARY_OP_LSHIFT, J->u64, ub,
                                      gcc_jit_context_new_cast(J->ctx, NULL, sh, J->u64));
        gcc_jit_rvalue *rsh_pos = bin(J, GCC_JIT_BINARY_OP_RSHIFT, J->u64, ub,
                                      gcc_jit_context_new_cast(J->ctx, NULL, sh, J->u64));
        gcc_jit_rvalue *lsh_neg = bin(J, GCC_JIT_BINARY_OP_LSHIFT, J->u64, ub,
                                      gcc_jit_context_new_cast(J->ctx, NULL, nsh, J->u64));
        gcc_jit_rvalue *rsh_neg = bin(J, GCC_JIT_BINARY_OP_RSHIFT, J->u64, ub,
                                      gcc_jit_context_new_cast(J->ctx, NULL, nsh, J->u64));
        gcc_jit_rvalue *rp = (op == OP_SHL) ? lsh_pos : rsh_pos;
        gcc_jit_rvalue *rn = (op == OP_SHL) ? rsh_neg : lsh_neg;
        store(J, pos, a, int_val(J, gcc_jit_context_new_cast(J->ctx, NULL, rp, J->i64)));
        go(J, pos, next);
        store(J, neg, a, int_val(J, gcc_jit_context_new_cast(J->ctx, NULL, rn, J->i64)));
        go(J, neg, next);
        break;
    }

    /* ---- for-in, channels, goroutines, closures ---- */
    case OP_FOR_IN_INIT:    /* cursor R[a+1] = 0, loop variables nil (container in R[a]) */
        store(J, blk, a + 1, int_val(J, ci64(J, 0)));
        for (int j = 0; j < b; j++) store(J, blk, a + 2 + j, cu64(J, SEA_NIL_VAL));
        go(J, blk, next);
        break;
    case OP_FOR_IN_NEXT: {  /* advance; R[a] = nil when exhausted (JMP_NIL follows) */
        /* Inline fast path for generic arrays (the common case): cursor in
         * R[a+1], index -> R[a+2], element -> R[a+3]. Other containers
         * (map/table/struct/channel) go through the helper. */
        gcc_jit_block *isarr = newblk(J), *chk = newblk(J), *slow = newblk(J);
        gcc_jit_block_end_with_conditional(blk, NULL, is_obj(J, reg(J, a)), chk, slow);
        gcc_jit_rvalue *arrptr = arr_ptr_of(J, reg(J, a));
        gcc_jit_rvalue *typ = gcc_jit_context_new_cast(J->ctx, NULL, gcc_jit_lvalue_as_rvalue(
            gcc_jit_rvalue_dereference_field(arrptr, NULL, J->f_atype)), J->t_int);
        gcc_jit_block_end_with_conditional(chk, NULL,
            cmp(J, GCC_JIT_COMPARISON_EQ, typ, cint(J, OBJ_ARRAY)), isarr, slow);
        gcc_jit_rvalue *pos = as_int(J, reg(J, a + 1));
        gcc_jit_rvalue *len = gcc_jit_lvalue_as_rvalue(
            gcc_jit_rvalue_dereference_field(arrptr, NULL, J->f_alen));
        gcc_jit_block *more0 = newblk(J), *more = newblk(J);
        gcc_jit_block_end_with_conditional(isarr, NULL,
            cmp(J, GCC_JIT_COMPARISON_LT, pos, gcc_jit_context_new_cast(J->ctx, NULL, len, J->i64)),
            more0, slow);   /* exhaustion releases the container: helper */
        {   /* the loop variables own their values: inline only when neither
             * the old values nor the new element are objects */
            gcc_jit_rvalue *ok = cmp(J, GCC_JIT_COMPARISON_EQ, cint(J, 1), cint(J, 1));
            if (b >= 1) ok = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool, ok, not_obj(J, reg(J, a + 2)));
            if (b >= 2) {
                ok = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool, ok, not_obj(J, reg(J, a + 3)));
                ok = bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool, ok,
                         not_obj(J, gcc_jit_lvalue_as_rvalue(arr_slot(J, arrptr, pos))));
            }
            gcc_jit_block_end_with_conditional(more0, NULL, ok, more, slow);
        }
        if (b >= 1) store(J, more, a + 2, reg(J, a + 1));   /* index == cursor (an int) */
        if (b >= 2) store(J, more, a + 3, gcc_jit_lvalue_as_rvalue(arr_slot(J, arrptr, pos)));
        store(J, more, a + 1, int_val(J, bin(J, GCC_JIT_BINARY_OP_PLUS, J->i64, pos, ci64(J, 1))));
        go(J, more, next);
        save_ip(J, slow, next);
        gcc_jit_rvalue *args[6] = {
            J->vm_rv, J->frame_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b),
            gcc_jit_context_new_rvalue_from_ptr(J->ctx, J->u32p, (void*)(J->code_addr + idx))
        };
        emit_call_susp(J, slow, J->fn_forin, args, 6, idx, next);
        break;
    }
    case OP_CHAN_SEND:      /* R[a] <- R[b] */
    case OP_CHAN_RECV: {    /* R[a] = <-R[b] */
        gcc_jit_function *h = (op == OP_CHAN_SEND) ? J->fn_csend : J->fn_crecv;
        save_ip(J, blk, next);
        gcc_jit_rvalue *args[6] = {
            J->vm_rv, J->frame_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b),
            gcc_jit_context_new_rvalue_from_ptr(J->ctx, J->u32p, (void*)(J->code_addr + idx))
        };
        emit_call_susp(J, blk, h, args, 6, idx, next);
        break;
    }
    case OP_CHAN_CLOSE: {
        gcc_jit_rvalue *args[3] = { J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a) };
        emit_call(J, blk, J->fn_cclose, args, 3, idx, next);
        break;
    }
    case OP_GO: {           /* spawn R[a](R[b..b+c-1]) */
        save_ip(J, blk, next);
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b), cint(J, c)
        };
        emit_call(J, blk, J->fn_go, args, 5, idx, next);
        break;
    }
    case OP_CLOSURE: {      /* R[a] = closure(proto[Bx]) */
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, J->frame_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a),
            cint(J, SEA_INSTR_Bx(in))
        };
        emit_call(J, blk, J->fn_closure, args, 5, idx, next);
        break;
    }
    case OP_DEL: {          /* del R[a][R[b]] */
        save_ip(J, blk, next);
        gcc_jit_rvalue *args[4] = { J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b) };
        emit_call(J, blk, J->fn_del, args, 4, idx, next);
        break;
    }
    case OP_GET_TABLE2: {   /* R[a] = R[b][R[c]]; R[a+1] = found */
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b), cint(J, c)
        };
        emit_call(J, blk, J->fn_gettable2, args, 5, idx, next);
        break;
    }
    case OP_MARK_CAPTURED: {    /* captured local assigned: share if a goroutine sees it */
        gcc_jit_rvalue *args[3] = { J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a) };
        emit_call(J, blk, J->fn_markcap, args, 3, idx, next);
        break;
    }
    case OP_CLOSE_UP: {     /* close upvalues from R[a] up */
        gcc_jit_rvalue *args[3] = { J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a) };
        emit_call(J, blk, J->fn_closeup, args, 3, idx, next);
        break;
    }

    /* ---- ownership (docs/memory.md) ---- */
    case OP_RETAIN: {       /* retain R[a]: nothing to do for a non-object */
        gcc_jit_block *rc = newblk(J);
        gcc_jit_block_end_with_conditional(blk, NULL, not_obj(J, reg(J, a)), blk_at(J, next), rc);
        gcc_jit_rvalue *args[3] = { J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a) };
        emit_call(J, rc, J->fn_retain, args, 3, idx, next);
        break;
    }
    case OP_SETLOCAL: {     /* R[a] = R[b] (retain if C), release the old value */
        gcc_jit_block *plain = newblk(J), *rc = newblk(J);
        gcc_jit_block_end_with_conditional(blk, NULL,
            bin(J, GCC_JIT_BINARY_OP_LOGICAL_AND, J->t_bool, not_obj(J, reg(J, a)), not_obj(J, reg(J, b))),
            plain, rc);
        store(J, plain, a, reg(J, b));
        go(J, plain, next);
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b), cint(J, c)
        };
        emit_call(J, rc, J->fn_setlocal, args, 5, idx, next);
        break;
    }
    case OP_APPEND: {       /* R[a] = R[a] .. R[b] */
        save_ip(J, blk, next);
        gcc_jit_rvalue *args[4] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b)
        };
        emit_call(J, blk, J->fn_append, args, 4, idx, next);
        break;
    }

    /* ---- single-opcode helpers ---- */
    case OP_RELEASE: {      /* a non-object just becomes nil; objects: helper */
        gcc_jit_block *plain = newblk(J), *rc = newblk(J);
        gcc_jit_block_end_with_conditional(blk, NULL, not_obj(J, reg(J, a)), plain, rc);
        store(J, plain, a, cu64(J, SEA_NIL_VAL));
        go(J, plain, next);
        gcc_jit_rvalue *args[3] = { J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a) };
        emit_call(J, rc, J->fn_release, args, 3, idx, next);
        break;
    }
    case OP_GET_FIELD: {    /* R[a] = R[b].K[c] */
        save_ip(J, blk, next);
        gcc_jit_rvalue *args[6] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b),
            cu64(J, (uint64_t)J->chunk->constants[c]), ic_slot(J, idx)
        };
        emit_call(J, blk, J->fn_getfield, args, 6, idx, next);
        break;
    }
    case OP_SET_FIELD: {    /* R[a].K[b] = R[c] */
        save_ip(J, blk, next);
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, c),
            cu64(J, (uint64_t)J->chunk->constants[b])
        };
        emit_call(J, blk, J->fn_setfield, args, 5, idx, next);
        break;
    }
    case OP_SET_FIELD_MOVE: {   /* R[a].K[b] = R[c], consuming R[c] */
        save_ip(J, blk, next);
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, c),
            cu64(J, (uint64_t)J->chunk->constants[b])
        };
        emit_call(J, blk, J->fn_setfieldmv, args, 5, idx, next);
        break;
    }
    case OP_CONCAT: {       /* R[a] = R[b] .. ... (c parts) */
        save_ip(J, blk, next);
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b), cint(J, c)
        };
        emit_call(J, blk, J->fn_concat, args, 5, idx, next);
        break;
    }
    case OP_STORE_GLOBAL: { /* G[K[Bx]] = R[a] */
        gcc_jit_rvalue *args[4] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a),
            cu64(J, (uint64_t)J->chunk->constants[SEA_INSTR_Bx(in)])
        };
        emit_call(J, blk, J->fn_storeglobal, args, 4, idx, next);
        break;
    }
    case OP_SET_FIELD_BORROW: {     /* R[a].K[b] = R[c], no retain */
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, c),
            cu64(J, (uint64_t)J->chunk->constants[b])
        };
        emit_call(J, blk, J->fn_setfieldb, args, 5, idx, next);
        break;
    }
    case OP_NEW_STRUCT: case OP_NEW_STRUCT_LOCAL:
    case OP_NEW_TABLE:  case OP_NEW_MAP: {
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, (int)op), cint(J, a), cint(J, b)
        };
        emit_call(J, blk, J->fn_newobj, args, 5, idx, next);
        break;
    }
    case OP_LOAD_UP:
    case OP_STORE_UP: {     /* R[a] = UpV[b] / UpV[b] = R[a] */
        gcc_jit_rvalue *args[4] = {
            J->frame_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, a), cint(J, b)
        };
        emit_call(J, blk, op == OP_LOAD_UP ? J->fn_loadup : J->fn_storeup, args, 4, idx, next);
        break;
    }

    /* ---- calls (delegated to C helpers) ---- */
    case OP_CALL: {
        save_ip(J, blk, next);
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv),
            cint(J, a), cint(J, b), cint(J, c)
        };
        emit_call(J, blk, J->fn_call, args, 5, idx, next);
        break;
    }
    case OP_CALL_SELF: {
        save_ip(J, blk, next);
        gcc_jit_rvalue *args[5] = {
            J->vm_rv, J->frame_rv, cint(J, a), cint(J, b), cint(J, c)
        };
        emit_call(J, blk, J->fn_call_self, args, 5, idx, next);
        break;
    }

    /* ---- return ---- */
    case OP_RETURN: {
        if (b > 0 && a != 0) {
            /* Copying the results down overwrites base[0..b-1]; any upvalue
             * still open on those registers must be closed first (as
             * lbl_RETURN does) or it would capture the result instead of the
             * local. Only chunks that create closures can have them. */
            if (J->has_closure) {
                gcc_jit_rvalue *args[3] = { J->vm_rv, gcc_jit_lvalue_as_rvalue(J->base_lv), cint(J, 0) };
                gcc_jit_block_add_eval(blk, NULL,
                    gcc_jit_context_new_call(J->ctx, NULL, J->fn_closeup, 3, args));
            }
            for (int j = 0; j < b; j++)
                gcc_jit_block_add_assignment(blk, NULL, mem_lv(J, j), reg(J, a + j));
        } else if (b > 0) {
            /* results already in R[0..b-1]: make sure they are in memory */
            for (int j = 0; j < b; j++) spill(J, blk, j);
        }
        gcc_jit_block_end_with_return(blk, NULL, cint(J, b));
        break;
    }
    case OP_RETURN0:
        gcc_jit_block_end_with_return(blk, NULL, cint(J, 0));
        break;

    default:
        /* Unsupported opcode: hand control back to the interpreter here.
         * chunk_compilable() guarantees this never sits inside a loop body. */
        emit_bail(J, blk, idx);
        break;
    }
}

/* Mark which instructions are reachable in the generated CFG. A bailout (for an
 * unsupported op) terminates its block without falling through, so instructions
 * after it that are not jump targets become dead — libgccjit rejects unreachable
 * blocks, so we must not create them. The successor edges here mirror exactly
 * what emit_instr() emits. */
static void compute_reachable(SeaChunk *c, const uint8_t *entry, uint8_t *reach) {
    int n = c->code_len;
    if (n == 0) return;
    memcpy(reach, entry, (size_t)n);   /* every OSR entry is a root */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int idx = 0; idx < n; idx++) {
            if (!reach[idx]) continue;
            uint32_t in = c->code[idx];
            SeaOpcode op = (SeaOpcode)SEA_INSTR_OP(in);
            int nx = -1, tg = -1;
            if (!op_supported(op) || op == OP_RETURN || op == OP_RETURN0) {
                /* bail or return: no successor */
            } else if (op == OP_JMP) {
                tg = idx + 1 + SEA_INSTR_sBx(in);
            } else if (op == OP_JMP_TRUE || op == OP_JMP_FALSE || op == OP_JMP_NIL ||
                       op == OP_FOR_PREP || op == OP_FOR_LOOP ||
                       op == OP_FOR_LOOP_STEP1 || op == OP_FOR_LOOP_INT) {
                tg = idx + 1 + SEA_INSTR_sBx(in);
                nx = idx + 1;
            } else {
                nx = idx + 1;   /* supported straight-line op falls through */
            }
            if (nx >= 0 && nx < n && !reach[nx]) { reach[nx] = 1; changed = true; }
            if (tg >= 0 && tg < n && !reach[tg]) { reach[tg] = 1; changed = true; }
        }
    }
}

/* ---- driver ---------------------------------------------------------------
 *
 * Compiling costs tens of ms per chunk (GCC init, then external `as` and `ld`
 * runs), so it is kept off the interpreter's critical path:
 *
 *  - sea_jit_init() forks a compile server process at startup. The VM sends it
 *    a chunk's bytecode + constants (plus the bytecode's address, which the
 *    generated code embeds); the server builds the function with libgccjit,
 *    writes it out as a shared object and replies with its path; the VM
 *    dlopen()s it. Since the server is a separate process, the VM can exit at
 *    any moment: the server sees EOF, finishes and removes its temp files.
 *  - A worker thread in the VM feeds the server from a FIFO of hot chunks, so
 *    the interpreter keeps running while a chunk compiles (sea_jit_request).
 *
 * If the server cannot be started (or dies), compilation falls back to
 * libgccjit in-process. SEA_JIT_INPROC=1 forces that mode. */

/* Publish the outcome of a compile. jit_fn may be read concurrently by the
 * interpreter threads, so it is stored with release semantics after
 * jit_result. */
static void *publish(SeaChunk *chunk, void *fn) {
    __atomic_store_n(&chunk->jit_fn, fn, __ATOMIC_RELEASE);
    return fn;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static gcc_jit_context *emit_function(JitCtx *Jp, const uint8_t *entry);

/* Build the native function for `chunk` into a new libgccjit context (not yet
 * compiled). `code_addr` is where the chunk's bytecode lives in the VM process
 * (inside the compile server, chunk->code is a copy). Returns NULL when the
 * chunk is not worth compiling, or when codegen failed (*codegen_err set). */
/* Register-promotion inputs (VM side, where the protos are): the chunk's
 * register count and which registers closures capture (an open upvalue points
 * at frame->base[r], so such registers must always live in memory). */
static void chunk_reg_info(SeaChunk *chunk, int *nregs, int *arity, uint8_t captured[32]) {
    *nregs = chunk->max_stack;
    *arity = chunk->arity;
    memset(captured, 0, 32);
    for (int p = 0; p < chunk->proto_len; p++) {
        SeaChunk *pr = chunk->protos[p];
        if (!pr) continue;
        for (int u = 0; u < pr->upval_count; u++)
            if (pr->upvals[u].is_local)
                captured[pr->upvals[u].idx >> 3] |= (uint8_t)(1u << (pr->upvals[u].idx & 7));
    }
}

/* Type feedback (VM side): one byte per instruction from the chunk's IC slots
 * (GET_TABLE / SET_TABLE record SEA_FB_* there). Caller frees. */
static uint8_t *chunk_feedback(SeaChunk *chunk) {
    uint8_t *fb = (uint8_t*)calloc((size_t)(chunk->code_len > 0 ? chunk->code_len : 1), 1);
    struct SeaTableICSlot *ics = sea_chunk_ics(chunk);
    for (int i = 0; i < chunk->code_len; i++) {
        SeaOpcode op = (SeaOpcode)SEA_INSTR_OP(chunk->code[i]);
        if (op == OP_GET_TABLE || op == OP_SET_TABLE)
            fb[i] = (uint8_t)(__atomic_load_n(&ics[i].idx, __ATOMIC_RELAXED) &
                              (SEA_FB_TYPED_ARRAY | SEA_FB_OTHER));
    }
    return fb;
}

static gcc_jit_context *build_ctx(SeaChunk *chunk, const uint32_t *code_addr, uintptr_t ics_addr,
                                  int nregs, int arity, const uint8_t *captured,
                                  const uint8_t *feedback, bool *codegen_err) {
    *codegen_err = false;
    if (chunk->code_len <= 0) return NULL;
    uint8_t *entry = (uint8_t*)malloc((size_t)chunk->code_len);
    gcc_jit_context *ctx = NULL;
    if (plan_chunk(chunk, entry)) {
        JitCtx J;
        memset(&J, 0, sizeof(J));
        J.chunk = chunk;
        J.n = chunk->code_len;
        J.code_addr = code_addr;
        J.ics_addr = ics_addr;
        /* Promote only in chunks with a loop: in straight-line code each op
         * runs once per call, so the entry/exit syncs are never amortized
         * (measured: fib_recursive ~7% slower with promotion). */
        bool has_loop = false;
        for (int i = 0; i < chunk->code_len && !has_loop; i++) {
            uint32_t in = chunk->code[i];
            if (op_is_jump((SeaOpcode)SEA_INSTR_OP(in)) && SEA_INSTR_sBx(in) < 0) has_loop = true;
        }
        J.nregs = has_loop ? nregs : 0;
        J.arity = arity;
        J.feedback = feedback;
        memcpy(J.captured, captured, 32);
        for (int i = 0; i < chunk->code_len; i++)
            if (SEA_INSTR_OP(chunk->code[i]) == OP_CLOSURE) { J.has_closure = true; break; }
        ctx = emit_function(&J, entry);
        *codegen_err = (ctx == NULL);
    }
    free(entry);
    return ctx;
}

/* Emit the whole function into a fresh context; NULL on codegen error. */
static gcc_jit_context *emit_function(JitCtx *Jp, const uint8_t *entry) {
    JitCtx J = *Jp;
    SeaChunk *chunk = J.chunk;

    J.ctx = gcc_jit_context_acquire();
    if (!J.ctx) return NULL;
    /* Codegen optimization level. Higher = better native code but slower compile;
     * for one-shot chunks (the main chunk) the compile latency can dominate, so
     * allow tuning via SEA_JIT_OPT (default 2). */
    int opt = 1;   /* sweet spot: fast compile + codegen on par with -O2 (measured) */
    { const char *e = getenv("SEA_JIT_OPT"); if (e) { opt = atoi(e); if (opt < 0) opt = 0; if (opt > 3) opt = 3; } }
    gcc_jit_context_set_int_option(J.ctx, GCC_JIT_INT_OPTION_OPTIMIZATION_LEVEL, opt);
    /* The generated code needs nothing from crt*.o / libgcc (its only external
     * references are the sea_jit_* helpers, resolved against the VM at load
     * time); skipping them saves ~10% of every compile. */
    gcc_jit_context_add_driver_option(J.ctx, "-nostdlib");

    J.u64     = gcc_jit_context_get_int_type(J.ctx, 8, 0);
    J.i64     = gcc_jit_context_get_int_type(J.ctx, 8, 1);
    J.dbl     = gcc_jit_context_get_type(J.ctx, GCC_JIT_TYPE_DOUBLE);
    J.t_int   = gcc_jit_context_get_type(J.ctx, GCC_JIT_TYPE_INT);
    J.t_bool  = gcc_jit_context_get_type(J.ctx, GCC_JIT_TYPE_BOOL);
    J.voidptr = gcc_jit_context_get_type(J.ctx, GCC_JIT_TYPE_VOID_PTR);
    J.u64p    = gcc_jit_type_get_pointer(J.u64);
    J.u32p    = gcc_jit_type_get_pointer(gcc_jit_context_get_int_type(J.ctx, 4, 0));

    /* Model SeaCallFrame just enough to reach ->ip and ->base at the right offsets. */
    gcc_jit_field *f_closure = gcc_jit_context_new_field(J.ctx, NULL, J.voidptr, "closure");
    J.f_ip                   = gcc_jit_context_new_field(J.ctx, NULL, J.u32p,    "ip");
    J.f_base                 = gcc_jit_context_new_field(J.ctx, NULL, J.u64p,    "base");
    gcc_jit_field *f_exp     = gcc_jit_context_new_field(J.ctx, NULL, J.t_int,   "expected_ret");
    gcc_jit_field *f_varargs = gcc_jit_context_new_field(J.ctx, NULL, J.u64p,    "varargs");
    gcc_jit_field *f_vc      = gcc_jit_context_new_field(J.ctx, NULL, J.t_int,   "vararg_count");
    gcc_jit_field *f_db      = gcc_jit_context_new_field(J.ctx, NULL, J.t_int,   "defer_base");
    gcc_jit_field *f_am      = gcc_jit_context_new_field(J.ctx, NULL,
                                   gcc_jit_context_get_int_type(J.ctx, 4, 0),    "arena_mark");
    gcc_jit_field *ff[8] = { f_closure, J.f_ip, J.f_base, f_exp, f_varargs, f_vc, f_db, f_am };
    gcc_jit_struct *frame_s = gcc_jit_context_new_struct_type(J.ctx, NULL, "SeaCallFrame", 8, ff);
    J.frame_ptr = gcc_jit_type_get_pointer(gcc_jit_struct_as_type(frame_s));

    /* Model SeaArray for the inline array fast path (offsets: type@0 (one
     * byte; gc flags@1), len@8, data@16 — verified against the C layout). The
     * other fields are modeled only to keep the offsets right. */
    {
        gcc_jit_type *u32 = gcc_jit_context_get_int_type(J.ctx, 4, 0);
        gcc_jit_type *u8  = gcc_jit_context_get_int_type(J.ctx, 1, 0);
        gcc_jit_type *u16 = gcc_jit_context_get_int_type(J.ctx, 2, 0);
        J.f_atype = gcc_jit_context_new_field(J.ctx, NULL, u8, "type");
        gcc_jit_field *f_agc  = gcc_jit_context_new_field(J.ctx, NULL, u8, "gc");
        gcc_jit_field *f_apad = gcc_jit_context_new_field(J.ctx, NULL, u16, "pad");
        gcc_jit_field *f_arc  = gcc_jit_context_new_field(J.ctx, NULL, u32, "rc");
        J.f_alen  = gcc_jit_context_new_field(J.ctx, NULL, u32, "len");
        gcc_jit_field *f_acap = gcc_jit_context_new_field(J.ctx, NULL, u32, "cap");
        J.f_adata = gcc_jit_context_new_field(J.ctx, NULL, J.u64p, "data");
        gcc_jit_field *af[7] = { J.f_atype, f_agc, f_apad, f_arc, J.f_alen, f_acap, J.f_adata };
        gcc_jit_struct *arr_s = gcc_jit_context_new_struct_type(J.ctx, NULL, "SeaArray", 7, af);
        J.arr_ptr = gcc_jit_type_get_pointer(gcc_jit_struct_as_type(arr_s));
    }

    gcc_jit_param *p_vm    = gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm");
    gcc_jit_param *p_frame = gcc_jit_context_new_param(J.ctx, NULL, J.frame_ptr, "frame");
    gcc_jit_param *params[2] = { p_vm, p_frame };
    J.fn = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_EXPORTED,
                                        J.t_int, "seajit", 2, params, 0);
    J.frame_rv = gcc_jit_param_as_rvalue(p_frame);
    J.vm_rv    = gcc_jit_param_as_rvalue(p_vm);

    /* Imported call helpers (resolved at run time via the process symbol table;
     * the binary is linked with -rdynamic).
     *   int sea_jit_call(void *vm, uint64 *base, int a, int b, int c);
     *   int sea_jit_call_self(void *vm, SeaCallFrame *frame, int a, int b, int c); */
    {
        gcc_jit_param *cp[5] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "b"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "c"),
        };
        J.fn_call = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                 J.t_int, "sea_jit_call", 5, cp, 0);
        gcc_jit_param *sp[5] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr,     "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.frame_ptr,   "frame"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,       "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,       "b"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,       "c"),
        };
        J.fn_call_self = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                      J.t_int, "sea_jit_call_self", 5, sp, 0);
    }

    /* Imported array helpers: int fn(void *vm, uint64 *base, int a, int b). */
    {
        const char *names[4] = { "sea_jit_apush", "sea_jit_new_array", "sea_jit_new_array_local",
                                 "sea_jit_apush_move" };
        gcc_jit_function **slots[4] = { &J.fn_apush, &J.fn_newarr, &J.fn_newarrlocal, &J.fn_apushmv };
        for (int k = 0; k < 4; k++) {
            gcc_jit_param *ap[4] = {
                gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
                gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "b"),
            };
            *slots[k] = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                     J.t_int, names[k], 4, ap, 0);
        }
    }

    /* int sea_jit_load_global(void *vm, uint64 *base, int a, uint64 key, void *ic); */
    {
        gcc_jit_param *gp[5] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64,     "key"),
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "ic"),
        };
        J.fn_loadglobal = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                       J.t_int, "sea_jit_load_global", 5, gp, 0);
    }

    /* int h(vm, frame, base, a, b, uint32_t *resume_ip) — may suspend */
    {
        const char *names[3] = { "sea_jit_for_in_next", "sea_jit_chan_send", "sea_jit_chan_recv" };
        gcc_jit_function **slots[3] = { &J.fn_forin, &J.fn_csend, &J.fn_crecv };
        for (int k = 0; k < 3; k++) {
            gcc_jit_param *pp[6] = {
                gcc_jit_context_new_param(J.ctx, NULL, J.voidptr,   "vm"),
                gcc_jit_context_new_param(J.ctx, NULL, J.frame_ptr, "frame"),
                gcc_jit_context_new_param(J.ctx, NULL, J.u64p,      "base"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,     "a"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,     "b"),
                gcc_jit_context_new_param(J.ctx, NULL, J.u32p,      "resume_ip"),
            };
            *slots[k] = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                     J.t_int, names[k], 6, pp, 0);
        }
        const char *n3[3] = { "sea_jit_chan_close", "sea_jit_close_up", "sea_jit_mark_captured" };
        gcc_jit_function **s3[3] = { &J.fn_cclose, &J.fn_closeup, &J.fn_markcap };
        for (int k = 0; k < 3; k++) {
            gcc_jit_param *pp[3] = {
                gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
                gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            };
            *s3[k] = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                  J.t_int, n3[k], 3, pp, 0);
        }
        gcc_jit_param *gp[5] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "b"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "c"),
        };
        J.fn_go = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                               J.t_int, "sea_jit_go", 5, gp, 0);
        gcc_jit_param *cp[5] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr,   "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.frame_ptr, "frame"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,      "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,     "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,     "bx"),
        };
        J.fn_closure = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                    J.t_int, "sea_jit_closure", 5, cp, 0);
        gcc_jit_param *dp[4] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "b"),
        };
        J.fn_del = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                J.t_int, "sea_jit_del", 4, dp, 0);
        gcc_jit_param *tp[5] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "b"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "c"),
        };
        J.fn_gettable2 = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                      J.t_int, "sea_jit_get_table2", 5, tp, 0);
        const char *gtn[2] = { "sea_jit_get_table", "sea_jit_set_table" };
        gcc_jit_function **gts[2] = { &J.fn_gettable, &J.fn_settable };
        for (int k = 0; k < 2; k++) {   /* params can't be shared between functions */
            gcc_jit_param *pp[5] = {
                gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
                gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "b"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "c"),
            };
            *gts[k] = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                   J.t_int, gtn[k], 5, pp, 0);
        }
    }

    J.fn_floor = gcc_jit_context_get_builtin_function(J.ctx, "__builtin_floor");
    J.fn_fmod  = gcc_jit_context_get_builtin_function(J.ctx, "__builtin_fmod");
    J.fn_pow   = gcc_jit_context_get_builtin_function(J.ctx, "__builtin_pow");

    /* Imported single-opcode helpers (see sea_jit.h). */
    {
        gcc_jit_param *rp[3] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
        };
        J.fn_release = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                    J.t_int, "sea_jit_release_reg", 3, rp, 0);
        gcc_jit_param *rtp[3] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
        };
        J.fn_retain = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                   J.t_int, "sea_jit_retain_reg", 3, rtp, 0);
        gcc_jit_param *slp[5] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "b"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "c"),
        };
        J.fn_setlocal = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                     J.t_int, "sea_jit_setlocal", 5, slp, 0);
        gcc_jit_param *app[4] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "b"),
        };
        J.fn_append = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                   J.t_int, "sea_jit_append", 4, app, 0);
        gcc_jit_param *gfp[6] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "b"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64,     "key"),
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "ic"),
        };
        J.fn_getfield = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                     J.t_int, "sea_jit_get_field", 6, gfp, 0);
        const char *fnames[3] = { "sea_jit_set_field", "sea_jit_set_field_borrow",
                                  "sea_jit_set_field_move" };
        gcc_jit_function **fslots[3] = { &J.fn_setfield, &J.fn_setfieldb, &J.fn_setfieldmv };
        for (int k = 0; k < 3; k++) {
            gcc_jit_param *fp[5] = {
                gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
                gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "bc"),
                gcc_jit_context_new_param(J.ctx, NULL, J.u64,     "key"),
            };
            *fslots[k] = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                      J.t_int, fnames[k], 5, fp, 0);
        }
        gcc_jit_param *np[5] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "op"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "b"),
        };
        J.fn_newobj = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                   J.t_int, "sea_jit_new_obj", 5, np, 0);
        gcc_jit_param *cp[5] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "b"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "c"),
        };
        J.fn_concat = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                   J.t_int, "sea_jit_concat", 5, cp, 0);
        gcc_jit_param *gp[4] = {
            gcc_jit_context_new_param(J.ctx, NULL, J.voidptr, "vm"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64p,    "base"),
            gcc_jit_context_new_param(J.ctx, NULL, J.t_int,   "a"),
            gcc_jit_context_new_param(J.ctx, NULL, J.u64,     "key"),
        };
        J.fn_storeglobal = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                        J.t_int, "sea_jit_store_global", 4, gp, 0);
        const char *unames[2] = { "sea_jit_load_up", "sea_jit_store_up" };
        gcc_jit_function **uslots[2] = { &J.fn_loadup, &J.fn_storeup };
        for (int k = 0; k < 2; k++) {
            gcc_jit_param *up[4] = {
                gcc_jit_context_new_param(J.ctx, NULL, J.frame_ptr, "frame"),
                gcc_jit_context_new_param(J.ctx, NULL, J.u64p,      "base"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,     "a"),
                gcc_jit_context_new_param(J.ctx, NULL, J.t_int,     "b"),
            };
            *uslots[k] = gcc_jit_context_new_function(J.ctx, NULL, GCC_JIT_FUNCTION_IMPORTED,
                                                      J.t_int, unames[k], 4, up, 0);
        }
    }

    /* prologue: base = frame->base; then dispatch on frame->ip to the entry
     * block — instruction 0 for a call, a clean loop head for OSR. */
    J.base_lv = gcc_jit_function_new_local(J.fn, NULL, J.u64p, "base");
    gcc_jit_block *prologue = gcc_jit_function_new_block(J.fn, "prologue");
    gcc_jit_block_add_assignment(prologue, NULL, J.base_lv,
        gcc_jit_lvalue_as_rvalue(gcc_jit_rvalue_dereference_field(J.frame_rv, NULL, J.f_base)));

    /* Register promotion: one native local per uncaptured register (loaded
     * on entry, below). */
    for (int n = 0; n < J.nregs && n < 256; n++) {
        if (J.captured[n >> 3] & (1u << (n & 7))) continue;
        char rn[16];
        snprintf(rn, sizeof rn, "R%d", n);
        J.L[n] = gcc_jit_function_new_local(J.fn, NULL, J.u64, rn);
    }

    /* Only create/emit reachable instruction blocks (bailouts leave dead tails). */
    uint8_t *reach = (uint8_t*)malloc((size_t)J.n);
    compute_reachable(chunk, entry, reach);

    J.blocks = (gcc_jit_block**)malloc(sizeof(gcc_jit_block*) * (size_t)J.n);
    for (int i = 0; i < J.n; i++)
        J.blocks[i] = reach[i] ? gcc_jit_function_new_block(J.fn, NULL) : NULL;

    {
        gcc_jit_rvalue *ipv = gcc_jit_lvalue_as_rvalue(
            gcc_jit_rvalue_dereference_field(J.frame_rv, NULL, J.f_ip));
        gcc_jit_rvalue *code0 = gcc_jit_context_new_rvalue_from_ptr(J.ctx, J.u32p, (void*)J.code_addr);
        gcc_jit_block *osr = newblk(&J), *callentry = newblk(&J);
        gcc_jit_block_end_with_conditional(prologue, NULL,
            cmp(&J, GCC_JIT_COMPARISON_EQ, ipv, code0), callentry, osr);
        /* Call entry: only the parameters hold values; every other register is
         * written before it is read, so start it as nil (a constant store GCC
         * drops when unused) instead of loading it. OSR entry: load them all. */
        for (int n = 0; n < J.nregs && n < 256; n++) {
            if (!J.L[n]) continue;
            gcc_jit_block_add_assignment(callentry, NULL, J.L[n],
                n < J.arity ? gcc_jit_lvalue_as_rvalue(mem_lv(&J, n)) : cu64(&J, SEA_NIL_VAL));
            gcc_jit_block_add_assignment(osr, NULL, J.L[n], gcc_jit_lvalue_as_rvalue(mem_lv(&J, n)));
        }
        gcc_jit_block_end_with_jump(callentry, NULL, J.blocks[0]);

        int ncases = 0;
        for (int i = 1; i < J.n; i++) ncases += entry[i];
        if (ncases == 0) {
            /* not an entry point: bail with frame->ip untouched */
            gcc_jit_block_end_with_return(osr, NULL, cint(&J, SEA_JIT_BAILOUT));
        } else {
            gcc_jit_rvalue *off = bin(&J, GCC_JIT_BINARY_OP_RSHIFT, J.u64,
                bin(&J, GCC_JIT_BINARY_OP_MINUS, J.u64,
                    gcc_jit_context_new_bitcast(J.ctx, NULL, ipv, J.u64),
                    gcc_jit_context_new_bitcast(J.ctx, NULL, code0, J.u64)),
                cu64(&J, 2));
            gcc_jit_case **cases = (gcc_jit_case**)malloc(sizeof(gcc_jit_case*) * (size_t)ncases);
            int k = 0;
            for (int i = 1; i < J.n; i++) {
                if (!entry[i]) continue;
                gcc_jit_rvalue *v = gcc_jit_context_new_rvalue_from_long(J.ctx, J.u64, i);
                cases[k++] = gcc_jit_context_new_case(J.ctx, v, v, J.blocks[i]);
            }
            gcc_jit_block *noentry = newblk(&J);
            gcc_jit_block_end_with_return(noentry, NULL, cint(&J, SEA_JIT_BAILOUT));
            gcc_jit_block_end_with_switch(osr, NULL, off, noentry, ncases, cases);
            free(cases);
        }
    }
    for (int i = 0; i < J.n; i++)
        if (reach[i]) emit_instr(&J, i);

    free(reach);
    free(J.blocks);

    if (gcc_jit_context_get_first_error(J.ctx) != NULL) {
        if (getenv("SEA_JIT_DEBUG"))
            fprintf(stderr, "[jit] codegen error in '%s': %s\n",
                    chunk->name ? chunk->name : "?", gcc_jit_context_get_first_error(J.ctx));
        gcc_jit_context_release(J.ctx);
        return NULL;
    }
    return J.ctx;
}

/* chunk->jit_result holds either a gcc_jit_result* (in-process compile) or,
 * tagged with bit 0, a dlopen() handle (compile server). */
#define DL_TAG ((uintptr_t)1)

static void debug_compiled(SeaChunk *chunk, const char *how, double ms) {
    if (getenv("SEA_JIT_DEBUG"))
        fprintf(stderr, "[jit] compiled '%s' (%d instrs, %.1f ms, %s)\n",
                chunk->name ? chunk->name : "?", chunk->code_len, ms, how);
}

static bool g_strict;   /* SEA_JIT_STRICT=1: a JIT bug aborts instead of falling back */

/* A chunk that passed plan_chunk() failed to build or compile: that is a bug
 * in the JIT. Normally the chunk just stays interpreted; under SEA_JIT_STRICT
 * (used by `make test-jit`) it aborts so tests cannot pass by falling back. */
static void jit_bug(SeaChunk *chunk, const char *what) {
    if (!g_strict) return;
    fprintf(stderr, "sea: JIT %s in '%s' (SEA_JIT_STRICT)\n", what,
            chunk->name ? chunk->name : "?");
    /* _exit, not exit: the worker calls this holding g_mu, which the atexit
     * handler (jit_drain_at_exit) would try to take again. */
    fflush(NULL);
    _exit(70);
}

static void *compile_inproc(SeaChunk *chunk) {
    if (!load_gccjit()) return SEA_JIT_FAILED;
    double t0 = now_ms();
    bool cg_err;
    int nregs, arity;
    uint8_t captured[32];
    chunk_reg_info(chunk, &nregs, &arity, captured);
    uint8_t *fb = chunk_feedback(chunk);
    gcc_jit_context *ctx = build_ctx(chunk, chunk->code, (uintptr_t)sea_chunk_ics(chunk),
                                     nregs, arity, captured, fb, &cg_err);
    free(fb);
    if (!ctx) {
        if (cg_err) jit_bug(chunk, "codegen error");
        return SEA_JIT_FAILED;
    }
    gcc_jit_result *res = gcc_jit_context_compile(ctx);
    gcc_jit_context_release(ctx);   /* result is independent of the context */
    if (!res) { jit_bug(chunk, "compile error"); return SEA_JIT_FAILED; }
    void *code = gcc_jit_result_get_code(res, "seajit");
    if (!code) {
        gcc_jit_result_release(res);
        return SEA_JIT_FAILED;
    }
    chunk->jit_result = res;
    debug_compiled(chunk, "in-process", now_ms() - t0);
    return code;
}

/* ---- compile server ---------------------------------------------------------
 * Protocol over a socketpair, one request at a time:
 *   VM -> server: SrvReq, code[code_len] (u32), constants[const_len] (u64), name
 *   server -> VM: SrvRep, then path[path_len] when ok */

typedef struct {
    uint64_t code_addr;            /* chunk->code in the VM process */
    uint64_t ics_addr;             /* chunk->ics (inline caches) in the VM process */
    int32_t  code_len, const_len, name_len;
    int32_t  nregs;                /* register promotion: chunk->max_stack */
    int32_t  arity;                /* chunk->arity */
    uint8_t  captured[32];         /* registers captured by closures (bitmap) */
} SrvReq;

typedef struct {
    int32_t ok;                    /* 1 = compiled (path follows), 0 = refused,
                                      -1 = codegen/compile error (a JIT bug) */
    int32_t path_len;
} SrvRep;

static bool            g_inproc;                 /* SEA_JIT_INPROC=1 */
static bool            g_last_bug;               /* last server reply was ok == -1 */
static int             g_srv_fd = -1;            /* VM end of the socketpair */
static pthread_mutex_t g_srv_mu = PTHREAD_MUTEX_INITIALIZER;

static bool write_all(int fd, const void *p, size_t n) {
    const char *c = (const char*)p;
    while (n > 0) {
        ssize_t w = send(fd, c, n, SEA_MSG_NOSIGNAL);   /* no SIGPIPE if the peer died */
        if (w < 0) { if (errno == EINTR) continue; return false; }
        c += w; n -= (size_t)w;
    }
    return true;
}

static bool read_all(int fd, void *p, size_t n) {
    char *c = (char*)p;
    while (n > 0) {
        ssize_t r = read(fd, c, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        c += r; n -= (size_t)r;
    }
    return true;
}

static void remove_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        char p[PATH_MAX + 256];
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(dir);
}

/* Compile server main loop (forked child; never returns). Serves requests
 * until the VM closes its end, then removes its temp dir and exits. */
static void server_main(int fd) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char dir[PATH_MAX / 2];
    snprintf(dir, sizeof dir, "%s/sea-jit-XXXXXX", tmp);
    bool have_dir = load_gccjit() && mkdtemp(dir) != NULL;
    unsigned seq = 0;
    SrvReq q;

    while (have_dir && read_all(fd, &q, sizeof q)) {
        if (q.code_len <= 0 || q.code_len > (1 << 24) || q.const_len < 0 ||
            q.const_len > (1 << 24) || q.name_len < 0 || q.name_len > 4096 ||
            q.nregs < 0 || q.nregs > 256)
            break;
        SeaChunk c;
        memset(&c, 0, sizeof c);
        c.code_len  = q.code_len;
        c.const_len = q.const_len;
        c.code      = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)q.code_len);
        c.constants = (SeaValue*)malloc(sizeof(SeaValue) * (size_t)(q.const_len + 1));
        c.name      = (char*)malloc((size_t)q.name_len + 1);
        uint8_t *fb = (uint8_t*)malloc((size_t)q.code_len);
        bool ok = read_all(fd, c.code, sizeof(uint32_t) * (size_t)q.code_len) &&
                  read_all(fd, c.constants, sizeof(SeaValue) * (size_t)q.const_len) &&
                  read_all(fd, c.name, (size_t)q.name_len) &&
                  read_all(fd, fb, (size_t)q.code_len);
        if (!ok) { free(c.code); free(c.constants); free(c.name); free(fb); break; }
        c.name[q.name_len] = '\0';

        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/c%u.so", dir, seq++);
        SrvRep r = { 0, 0 };
        double t0 = now_ms();
        bool cg_err;
        gcc_jit_context *ctx = build_ctx(&c, (const uint32_t*)(uintptr_t)q.code_addr,
                                         (uintptr_t)q.ics_addr, q.nregs, q.arity, q.captured,
                                         fb, &cg_err);
        if (ctx) {
            gcc_jit_context_compile_to_file(ctx, GCC_JIT_OUTPUT_KIND_DYNAMIC_LIBRARY, path);
            r.ok = (gcc_jit_context_get_first_error(ctx) == NULL && access(path, R_OK) == 0) ? 1 : -1;
            gcc_jit_context_release(ctx);
        } else if (cg_err) {
            r.ok = -1;
        }
        if (r.ok == 1) {
            debug_compiled(&c, "server", now_ms() - t0);
            r.path_len = (int32_t)strlen(path);
        }
        bool sent = write_all(fd, &r, sizeof r) &&
                    (r.ok != 1 || write_all(fd, path, (size_t)r.path_len));
        free(c.code); free(c.constants); free(c.name); free(fb);
        if (!sent) { if (r.ok == 1) unlink(path); break; }
    }
    /* The VM unlinks each .so once loaded, so normally the dir is empty; a
     * reply the VM never read (it exited) leaves one file behind. */
    if (have_dir) remove_dir(dir);
    _exit(0);
}

/* Fork the compile server. Must run before the VM starts any thread. */
static void start_server(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(sv[0]); close(sv[1]); return; }
    if (pid == 0) {
        close(sv[0]);
        /* Don't hold the VM's stdio open: whoever reads the VM's output until
         * EOF would otherwise wait for the server too. stderr is kept only for
         * SEA_JIT_DEBUG. */
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            if (!getenv("SEA_JIT_DEBUG")) dup2(nul, 2);
            if (nul > 2) close(nul);
        }
        server_main(sv[1]);
    }
    close(sv[1]);
    g_srv_fd = sv[0];
}

/* Send `chunk` to the server. Caller holds g_srv_mu and checked g_srv_fd.
 * After this returns the chunk's memory is no longer needed. On I/O error the
 * server is considered dead and dropped. */
static bool remote_send(SeaChunk *chunk) {
    int name_len = chunk->name ? (int)strlen(chunk->name) : 0;
    if (name_len > 4096) name_len = 4096;
    SrvReq q;
    memset(&q, 0, sizeof q);
    q.code_addr = (uint64_t)(uintptr_t)chunk->code;
    q.ics_addr  = (uint64_t)(uintptr_t)sea_chunk_ics(chunk);
    q.code_len  = chunk->code_len;
    q.const_len = chunk->const_len;
    q.name_len  = name_len;
    chunk_reg_info(chunk, &q.nregs, &q.arity, q.captured);
    bool io = write_all(g_srv_fd, &q, sizeof q) &&
              write_all(g_srv_fd, chunk->code, sizeof(uint32_t) * (size_t)chunk->code_len) &&
              write_all(g_srv_fd, chunk->constants, sizeof(SeaValue) * (size_t)chunk->const_len) &&
              write_all(g_srv_fd, chunk->name, (size_t)name_len);
    if (io) {
        uint8_t *fb = chunk_feedback(chunk);
        io = write_all(g_srv_fd, fb, (size_t)chunk->code_len);
        free(fb);
    }
    if (!io) { close(g_srv_fd); g_srv_fd = -1; }
    return io;
}

/* Receive the server's reply to the last request and load the code. Caller
 * holds g_srv_mu. Returns the code pointer (and its dlopen handle in
 * *handle), SEA_JIT_FAILED when the server refused/failed the chunk, or NULL
 * when the server died. */
static void *remote_recv(SeaChunk *chunk, void **handle) {
    SrvRep r;
    char path[PATH_MAX];
    *handle = NULL;
    bool io = read_all(g_srv_fd, &r, sizeof r) &&
              (r.ok != 1 || (r.path_len > 0 && r.path_len < (int)sizeof path &&
                             read_all(g_srv_fd, path, (size_t)r.path_len)));
    if (!io) { close(g_srv_fd); g_srv_fd = -1; return NULL; }
    g_last_bug = (r.ok == -1);
    if (g_last_bug && chunk) jit_bug(chunk, "codegen/compile error (server)");
    if (r.ok != 1) return SEA_JIT_FAILED;

    path[r.path_len] = '\0';
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    unlink(path);   /* the mapping stays valid */
    if (!h) return SEA_JIT_FAILED;
    void *code = dlsym(h, "seajit");
    if (!code) { dlclose(h); return SEA_JIT_FAILED; }
    *handle = h;
    return code;
}

/* Compile through the server: code pointer, SEA_JIT_FAILED, or NULL when no
 * server is available (the caller then compiles in-process). */
static void *compile_remote(SeaChunk *chunk) {
    void *fn = NULL, *h = NULL;
    pthread_mutex_lock(&g_srv_mu);
    if (g_srv_fd >= 0 && remote_send(chunk)) fn = remote_recv(chunk, &h);
    pthread_mutex_unlock(&g_srv_mu);
    if (h) chunk->jit_result = (void*)((uintptr_t)h | DL_TAG);
    return fn;
}

void *sea_jit_compile(SeaVM *vm, SeaChunk *chunk) {
    (void)vm;
    void *fn = g_inproc ? NULL : compile_remote(chunk);
    if (fn == NULL) fn = compile_inproc(chunk);
    return publish(chunk, fn);
}

/* ---- background compile queue -----------------------------------------------
 * One worker thread drains a FIFO of chunks (linked through chunk->jit_next),
 * so the interpreter never waits for a compile. A queued or in-flight chunk
 * has jit_fn == SEA_JIT_PENDING.
 *
 * The chunk being compiled (g_busy) may be freed meanwhile (sea_jit_release,
 * e.g. at VM teardown). Once its request has been sent to the server
 * (g_sent) the worker no longer reads it, so release() just marks it
 * abandoned instead of waiting for the compile; the worker then discards the
 * result without touching the chunk. */

static bool            g_sync;      /* SEA_JIT_SYNC=1: compile inline */
static pthread_mutex_t g_mu   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_work = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_idle = PTHREAD_COND_INITIALIZER;
static SeaChunk       *g_head, *g_tail;
static SeaChunk       *g_busy;      /* chunk being compiled right now */
static bool            g_sent;      /* g_busy's request is with the server */
static bool            g_abandoned; /* g_busy was released mid-compile */
static bool            g_started;

static void *jit_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (!g_head) pthread_cond_wait(&g_work, &g_mu);
        SeaChunk *c = g_head;
        g_head = c->jit_next;
        if (!g_head) g_tail = NULL;
        c->jit_next = NULL;
        g_busy = c;
        pthread_mutex_unlock(&g_mu);

        void *fn = NULL, *h = NULL;
        if (!g_inproc) {
            pthread_mutex_lock(&g_srv_mu);
            if (g_srv_fd >= 0 && remote_send(c)) {
                pthread_mutex_lock(&g_mu);
                g_sent = true;
                pthread_cond_broadcast(&g_idle);
                pthread_mutex_unlock(&g_mu);
                fn = remote_recv(NULL, &h);   /* c may be freed meanwhile */
                if (fn == SEA_JIT_FAILED && g_last_bug) {
                    pthread_mutex_lock(&g_mu);
                    if (!g_abandoned) jit_bug(c, "codegen/compile error (server)");
                    pthread_mutex_unlock(&g_mu);
                }
            }
            pthread_mutex_unlock(&g_srv_mu);
        }

        pthread_mutex_lock(&g_mu);
        if (g_abandoned) {
            if (h) dlclose(h);
        } else {
            if (fn == NULL) {        /* no server: compile here, reading c throughout */
                g_sent = false;      /* so release() waits for us again */
                pthread_mutex_unlock(&g_mu);
                fn = compile_inproc(c);
                pthread_mutex_lock(&g_mu);
            } else if (h) {
                c->jit_result = (void*)((uintptr_t)h | DL_TAG);
            }
            publish(c, fn);
        }
        g_busy = NULL;
        g_sent = g_abandoned = false;
        pthread_cond_broadcast(&g_idle);
    }
    return NULL;
}

/* At exit, drop whatever is still queued. A compile running in the server
 * needs nothing from us (the server cleans up after itself), but one running
 * in-process must finish: abandoning libgccjit mid-compile would leave its
 * temp dir and the as/ld outputs behind in $TMPDIR. */
static void jit_drain_at_exit(void) {
    pthread_mutex_lock(&g_mu);
    g_head = g_tail = NULL;
    while (g_busy && g_srv_fd < 0) pthread_cond_wait(&g_idle, &g_mu);
    pthread_mutex_unlock(&g_mu);
}

void *sea_jit_request(SeaVM *vm, SeaChunk *chunk) {
    void *jf = SEA_JIT_LOAD_FN(chunk);
    if (jf != NULL) return jf;
    if (g_sync) return sea_jit_compile(vm, chunk);
    /* Refuse up front (cheap) so hopeless chunks never occupy the queue. */
    if (!chunk_compilable(chunk)) return publish(chunk, SEA_JIT_FAILED);

    pthread_mutex_lock(&g_mu);
    jf = chunk->jit_fn;
    if (jf == NULL) {   /* not raced by another interpreter thread */
        if (!g_started) {
            pthread_t th;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            g_started = pthread_create(&th, &attr, jit_worker, NULL) == 0;
            pthread_attr_destroy(&attr);
            if (g_started) atexit(jit_drain_at_exit);
        }
        if (!g_started) {   /* no thread: degrade to inline compile */
            pthread_mutex_unlock(&g_mu);
            return sea_jit_compile(vm, chunk);
        }
        chunk->jit_fn = jf = SEA_JIT_PENDING;
        chunk->jit_next = NULL;
        if (g_tail) g_tail->jit_next = chunk; else g_head = chunk;
        g_tail = chunk;
        pthread_cond_signal(&g_work);
    }
    pthread_mutex_unlock(&g_mu);
    return jf;
}

static bool env_flag(const char *name) {
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0") != 0;
}

static int env_int(const char *name, int dflt) {
    const char *e = getenv(name);
    if (!e || !*e) return dflt;
    int v = atoi(e);
    return v > 0 ? v : 1;
}

void sea_jit_init(void) {
    g_sync   = env_flag("SEA_JIT_SYNC");
    g_inproc = env_flag("SEA_JIT_INPROC");
    g_strict = env_flag("SEA_JIT_STRICT");
    sea_jit_hot_calls = env_int("SEA_JIT_HOT_CALLS", SEA_JIT_THRESHOLD);
    sea_jit_hot_loops = env_int("SEA_JIT_HOT_LOOPS", SEA_JIT_LOOP_THRESHOLD);
    if (!g_inproc) start_server();
}

void sea_jit_release(SeaChunk *chunk) {
    if (SEA_JIT_LOAD_FN(chunk) == SEA_JIT_PENDING) {
        /* Unlink it from the queue, or make sure the worker is done reading it. */
        pthread_mutex_lock(&g_mu);
        SeaChunk **pp = &g_head, *prev = NULL;
        while (*pp && *pp != chunk) { prev = *pp; pp = &(*pp)->jit_next; }
        if (*pp) {
            *pp = chunk->jit_next;
            if (g_tail == chunk) g_tail = prev;
            chunk->jit_next = NULL;
        }
        while (g_busy == chunk && !g_sent) pthread_cond_wait(&g_idle, &g_mu);
        if (g_busy == chunk) g_abandoned = true;   /* worker drops the result */
        pthread_mutex_unlock(&g_mu);
    }
    uintptr_t r = (uintptr_t)chunk->jit_result;
    if (r & DL_TAG)
        dlclose((void*)(r & ~DL_TAG));
    else if (r)
        gcc_jit_result_release((gcc_jit_result*)r);
    chunk->jit_result = NULL;
    chunk->jit_fn = NULL;
}

#else /* !SEA_JIT — JIT not built into this binary */

void *sea_jit_compile(SeaVM *vm, SeaChunk *chunk) {
    (void)vm; (void)chunk;
    return SEA_JIT_FAILED;
}

void *sea_jit_request(SeaVM *vm, SeaChunk *chunk) {
    (void)vm; (void)chunk;
    return SEA_JIT_FAILED;
}

void sea_jit_init(void) {}

void sea_jit_release(SeaChunk *chunk) {
    (void)chunk;
}

#endif
