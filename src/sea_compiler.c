/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_compiler.h"
#include "sea_string.h"
#include "sea_table.h"
#include "sea_memory.h"
#include <math.h>
#include <stdarg.h>

/* The first error of the last compilation on this thread (goroutines can
 * require modules concurrently): message without position, and its line. */
static _Thread_local char g_compile_error[512];
static _Thread_local int  g_compile_error_line;

const char* sea_compile_error(void) {
    return g_compile_error[0] ? g_compile_error : NULL;
}

int sea_compile_error_line(void) {
    return g_compile_error_line;
}

// ---- Register metadata helpers (Etapa 2: constant folding) ----

/* reg_clear_int: the register may now hold any value (facts dropped). */
static inline void reg_clear_int(SeaCompiler *c, int r) {
    c->reg_meta[r].is_int = 0;
    c->reg_meta[r].is_const = 0;
    c->reg_meta[r].is_scalar = 0;
}

static inline void reg_set_const_int(SeaCompiler *c, int r, int64_t v) {
    c->reg_meta[r].is_int = 1;
    c->reg_meta[r].is_const = 1;
    c->reg_meta[r].const_int = v;
    c->reg_meta[r].is_scalar = 1;
}

/* A value that needs no refcounting (number, bool, nil, pinned constant). */
static inline void reg_set_scalar(SeaCompiler *c, int r) {
    c->reg_meta[r].is_int = 0;
    c->reg_meta[r].is_const = 0;
    c->reg_meta[r].is_scalar = 1;
}

static inline void reg_copy_int_meta(SeaCompiler *c, int dst, int src) {
    c->reg_meta[dst].is_int = c->reg_meta[src].is_int;
    c->reg_meta[dst].is_const = c->reg_meta[src].is_const;
    c->reg_meta[dst].const_int = c->reg_meta[src].const_int;
    c->reg_meta[dst].is_scalar = c->reg_meta[src].is_scalar;
}

static inline bool reg_scalar(SeaCompiler *c, int r) {
    return c->reg_meta[r].is_int || c->reg_meta[r].is_scalar;
}

/* Forward declarations for emit helpers used by copy-propagation functions below. */
static void emit_abc(SeaCompiler *c, SeaOpcode op, int a, int b, int cc, int line);

/* Follow copy chain: return the ultimate source register for r. */
static int resolve_copy_chain(SeaCompiler *c, int r) {
    while (c->reg_meta[r].has_copy)
        r = c->reg_meta[r].copy_of;
    return r;
}

/* Materialize deferred copies of register r: emit MOVE for every named local
 * that currently aliases r via has_copy, then clear has_copy.
 * Called before r is written to, captured, or goes out of scope. */
static void materialize_copies_of(SeaCompiler *c, int r, int line) {
    for (int i = 0; i < c->local_count; i++) {
        int reg = c->locals[i].reg;
        if (c->reg_meta[reg].has_copy && c->reg_meta[reg].copy_of == r) {
            emit_abc(c, OP_MOVE, reg, r, 0, line);
            c->reg_meta[reg].has_copy = 0;
        }
    }
}

/* At branch merge points and loop exits: clear is_const for all live locals
 * and materialize any deferred copies (copy_of may become stale across branches). */
static void invalidate_locals_at_merge(SeaCompiler *c, int line) {
    for (int i = 0; i < c->local_count; i++) {
        int reg = c->locals[i].reg;
        c->reg_meta[reg].is_const = 0;
        if (c->reg_meta[reg].has_copy) {
            emit_abc(c, OP_MOVE, reg, c->reg_meta[reg].copy_of, 0, line);
            c->reg_meta[reg].has_copy = 0;
        }
    }
}

/* Between mutually exclusive branches (then/else, select cases): constants
 * learned while compiling one branch must not leak into the next — at run
 * time only one of them executes. */
static void clear_local_consts(SeaCompiler *c) {
    for (int i = 0; i < c->local_count; i++)
        c->reg_meta[c->locals[i].reg].is_const = 0;
}

/* ---- Sound int inference across control flow ------------------------------
 * `is_int` on a local's register lets the compiler emit IIADD/IISUB, which do
 * no type check. The fact must hold on every path that reaches a use:
 *   - if/else, select: each alternative starts from the state before the
 *     branch; afterwards only what holds on all paths survives (meet);
 *   - loops: a local assigned in the body keeps is_int at the loop head only if
 *     every assignment to it in the body is provably int given the others
 *     (small fixpoint over the AST); after the loop, meet with the head state
 *     (the body may run zero times or exit early).
 * SEA_TYPE_CHECK=1 makes IIADD/IISUB verify this at run time. */
/* The same machinery tracks `is_scalar` (no refcounting needed), which lets
 * an assignment skip releasing the old value (docs/memory.md). */
typedef struct { uint8_t bits[SEA_MAX_REGISTERS], sbits[SEA_MAX_REGISTERS]; int nlocals; } IntSnap;

static void snap_take(SeaCompiler *c, IntSnap *s) {
    s->nlocals = c->local_count;
    for (int i = 0; i < c->local_count; i++) {
        int r = c->locals[i].reg;
        s->bits[r] = c->reg_meta[r].is_int;
        s->sbits[r] = c->reg_meta[r].is_scalar;
    }
}
static void snap_restore(SeaCompiler *c, const IntSnap *s) {
    for (int i = 0; i < s->nlocals && i < c->local_count; i++) {
        int r = c->locals[i].reg;
        c->reg_meta[r].is_int = s->bits[r];
        c->reg_meta[r].is_scalar = s->sbits[r];
    }
}
/* current := current AND snapshot (for the locals the snapshot covers) */
static void snap_meet(SeaCompiler *c, const IntSnap *s) {
    for (int i = 0; i < s->nlocals && i < c->local_count; i++) {
        int r = c->locals[i].reg;
        c->reg_meta[r].is_int = c->reg_meta[r].is_int && s->bits[r];
        c->reg_meta[r].is_scalar = (c->reg_meta[r].is_scalar || c->reg_meta[r].is_int) &&
                                   (s->sbits[r] || s->bits[r]);
    }
}

/* Assignments `name = expr` inside a loop body (expr NULL = unknown value). */
#define LOOP_MAX_ASSIGNS 512
typedef struct { const char *name[LOOP_MAX_ASSIGNS]; SeaNode *expr[LOOP_MAX_ASSIGNS]; int n; bool overflow; } LoopAssigns;

static void loop_collect(SeaNode *n, LoopAssigns *la) {
    if (!n) return;
    switch (n->type) {
    case NODE_BLOCK: case NODE_DO:
        for (int i = 0; i < n->block.count; i++) loop_collect(n->block.stmts[i], la);
        break;
    case NODE_ASSIGN:
        for (int i = 0; i < n->assign.count; i++) {
            if (n->assign.targets[i]->type != NODE_IDENT) continue;
            if (la->n >= LOOP_MAX_ASSIGNS) { la->overflow = true; return; }
            la->name[la->n] = n->assign.targets[i]->ident;
            la->expr[la->n] = (n->assign.expr_count == n->assign.count) ? n->assign.exprs[i] : NULL;
            la->n++;
        }
        break;
    case NODE_IF:
        loop_collect(n->if_stmt.then_body, la);
        loop_collect(n->if_stmt.else_body, la);
        break;
    case NODE_WHILE:  loop_collect(n->while_loop.body, la); break;
    case NODE_FOR_NUM: loop_collect(n->for_num.body, la); break;
    case NODE_FOR_IN: loop_collect(n->for_in.body, la); break;
    case NODE_SELECT:
        for (int i = 0; i < n->select.count; i++) loop_collect(n->select.cases[i].body, la);
        break;
    default: break;   /* expressions, declarations; closures are not entered */
    }
}

static bool la_assigned(const LoopAssigns *la, const char *name) {
    for (int i = 0; i < la->n; i++) if (strcmp(la->name[i], name) == 0) return true;
    return false;
}

static int resolve_local(SeaCompiler *c, const char *name);

/* Is `e` provably an int, given which assigned names are still believed int
 * (`keep`, indexed like la->name)? Mirrors what compile_expr marks is_int. */
static bool loop_int_expr(SeaCompiler *c, SeaNode *e, const LoopAssigns *la, const uint8_t *keep) {
    if (!e) return false;
    switch (e->type) {
    case NODE_NUMBER: return !e->num_is_float && e->number == (double)(int64_t)e->number;
    case NODE_IDENT: {
        bool assigned = false, all_kept = true;
        for (int i = 0; i < la->n; i++)
            if (strcmp(la->name[i], e->ident) == 0) { assigned = true; if (!keep[i]) all_kept = false; }
        if (assigned) return all_kept;
        int r = resolve_local(c, e->ident);
        return r >= 0 && c->reg_meta[r].is_int;
    }
    case NODE_BINOP:
        switch (e->binop.op) {
        case BINOP_ADD: case BINOP_SUB: case BINOP_MUL: case BINOP_IDIV: case BINOP_MOD:
        case BINOP_BAND: case BINOP_BOR: case BINOP_BXOR: case BINOP_SHL: case BINOP_SHR:
            return loop_int_expr(c, e->binop.left, la, keep) && loop_int_expr(c, e->binop.right, la, keep);
        default: return false;
        }
    case NODE_CALL:   /* len(x) is always an int (as compile_expr assumes) */
        return e->call.fn->type == NODE_IDENT && strcmp(e->call.fn->ident, "len") == 0 &&
               e->call.argc == 1 && resolve_local(c, "len") < 0;
    default: return false;
    }
}

/* Is `e` provably a value that needs no refcounting (see loop_int_expr)?
 * Arithmetic and comparisons yield numbers/bools (or raise), literals are
 * scalars or pinned constants. */
static bool loop_scalar_expr(SeaCompiler *c, SeaNode *e, const LoopAssigns *la, const uint8_t *keep) {
    if (!e) return false;   /* unknown value (e.g. one of several call results) */
    switch (e->type) {
    case NODE_NUMBER: case NODE_BOOL: case NODE_NIL: case NODE_STRING: return true;
    case NODE_IDENT: {
        bool assigned = false, all_kept = true;
        for (int i = 0; i < la->n; i++)
            if (strcmp(la->name[i], e->ident) == 0) { assigned = true; if (!keep[i]) all_kept = false; }
        if (assigned) return all_kept;
        int r = resolve_local(c, e->ident);
        return r >= 0 && reg_scalar(c, r);
    }
    case NODE_UNOP:
        return e->unop.op == UNOP_NEG || e->unop.op == UNOP_NOT || e->unop.op == UNOP_BNOT;
    case NODE_BINOP:
        switch (e->binop.op) {
        case BINOP_CONCAT: case BINOP_CHAN_SEND: return false;
        case BINOP_AND: case BINOP_OR:
            return loop_scalar_expr(c, e->binop.left, la, keep) && loop_scalar_expr(c, e->binop.right, la, keep);
        default: return true;    /* arithmetic, bitwise, comparison */
        }
    case NODE_CALL:
        return e->call.fn->type == NODE_IDENT && strcmp(e->call.fn->ident, "len") == 0 &&
               e->call.argc == 1 && resolve_local(c, "len") < 0;
    default: return false;
    }
}

/* At a loop head: drop is_int for locals assigned in the body unless every
 * assignment keeps them int (fixpoint), and likewise is_scalar. Returns
 * nothing; the caller snapshots the resulting head state for the exit meet. */
static void loop_head_ints(SeaCompiler *c, SeaNode *body) {
    LoopAssigns *L = (LoopAssigns*)sea_malloc(sizeof(LoopAssigns));
    memset(L, 0, sizeof *L);
    loop_collect(body, L);
    uint8_t keep[LOOP_MAX_ASSIGNS];
    for (int i = 0; i < L->n; i++) {
        int r = resolve_local(c, L->name[i]);
        keep[i] = !L->overflow && r >= 0 && c->reg_meta[r].is_int;
    }
    bool changed = true;
    while (changed && !L->overflow) {
        changed = false;
        for (int i = 0; i < L->n; i++) {
            if (!keep[i]) continue;
            if (!loop_int_expr(c, L->expr[i], L, keep)) {
                for (int j = 0; j < L->n; j++)       /* the name loses it everywhere */
                    if (keep[j] && strcmp(L->name[j], L->name[i]) == 0) keep[j] = 0;
                changed = true;
            }
        }
    }
    for (int i = 0; i < L->n; i++) {
        if (keep[i]) continue;
        int r = resolve_local(c, L->name[i]);
        if (r >= 0) c->reg_meta[r].is_int = 0;
    }
    /* the same fixpoint for is_scalar */
    for (int i = 0; i < L->n; i++) {
        int r = resolve_local(c, L->name[i]);
        keep[i] = !L->overflow && r >= 0 && (reg_scalar(c, r) || c->reg_meta[r].is_int);
    }
    changed = true;
    while (changed && !L->overflow) {
        changed = false;
        for (int i = 0; i < L->n; i++) {
            if (!keep[i]) continue;
            if (!loop_scalar_expr(c, L->expr[i], L, keep)) {
                for (int j = 0; j < L->n; j++)
                    if (keep[j] && strcmp(L->name[j], L->name[i]) == 0) keep[j] = 0;
                changed = true;
            }
        }
    }
    for (int i = 0; i < L->n; i++) {
        if (keep[i]) continue;
        int r = resolve_local(c, L->name[i]);
        if (r >= 0) { c->reg_meta[r].is_scalar = 0; c->reg_meta[r].is_int = 0; }
    }
    sea_free(L, sizeof(LoopAssigns));
}

// ---- Etapa 4: Dead Write Elimination (temporaries only) ----

/* Returns true if register r is a named local (never dead-eliminate named locals). */
static inline bool reg_is_named_local(SeaCompiler *c, int r) {
    for (int i = 0; i < c->local_count; i++)
        if (c->locals[i].reg == r) return true;
    return false;
}

/* Record that a pure, side-effect-free instruction is about to be emitted writing to r.
 * Called BEFORE the emit so sea_chunk_pos() returns the instruction's position.
 * Resets dwe_read = 0 so subsequent reads of the SAME temp can be tracked fresh. */
static inline void dwe_pure_write(SeaCompiler *c, int r) {
    if (reg_is_named_local(c, r)) {
        c->reg_meta[r].dwe_write_pos = -1; /* named locals: no DWE */
        return;
    }
    c->reg_meta[r].dwe_write_pos = sea_chunk_pos(c->chunk);
    c->reg_meta[r].dwe_read = 0;
}


/* Called when a temp register is freed.  If it was written by an eliminable pure op
 * and never subsequently read (dwe_read == 0), patch it to OP_LOAD_NIL 0 0 0 (NOP). */
/* Diagnostic modes (read once from the environment at compile time):
 *   SEA_NO_DWE=1    keep every write (rules DWE in or out when chasing a bug);
 *   SEA_DWE_CHECK=1 an eliminated write loads the string "<dwe-eliminated>"
 *                   instead of becoming a NOP, so a write that was wrongly
 *                   judged dead shows up in the program's output/errors
 *                   (`make test-dwe`). */
enum { DWE_ON, DWE_OFF, DWE_CHECK };
static int dwe_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *off = getenv("SEA_NO_DWE"), *chk = getenv("SEA_DWE_CHECK");
        mode = (off && *off && strcmp(off, "0") != 0) ? DWE_OFF
             : (chk && *chk && strcmp(chk, "0") != 0) ? DWE_CHECK : DWE_ON;
    }
    return mode;
}

static int add_const_str(SeaCompiler *c, const char *s);

static void dwe_free(SeaCompiler *c, int r) {
    int pos = c->reg_meta[r].dwe_write_pos;
    if (pos < 0) return;
    if (c->reg_meta[r].dwe_read) return;
    c->reg_meta[r].dwe_write_pos = -1;
    if (dwe_mode() == DWE_OFF) return;
    SeaOpcode op = SEA_INSTR_OP(c->chunk->code[pos]);
    switch (op) {
        case OP_LOAD_INT:
        case OP_LOAD_K:
        case OP_MOVE:
        case OP_IIADD:
        case OP_IISUB:
            if (dwe_mode() == DWE_CHECK) {
                int k = add_const_str(c, "<dwe-eliminated>");
                if (k <= 0xFFFF) {
                    c->chunk->code[pos] = SEA_MAKE_ABx(OP_LOAD_K, r, k);
                    break;
                }
            }
            c->chunk->code[pos] = SEA_MAKE_ABC(OP_LOAD_NIL, 0, 0, 0);
            break;
        default: break;
    }
}

/* Mark every register `instr` reads (sea_op_reads: the table derived from
 * the VM handlers). Called for each emitted instruction. */
static void dwe_mark_instr_reads(SeaCompiler *c, uint32_t instr) {
    SeaRegRanges rr;
    sea_op_reads(instr, &rr);
    for (int i = 0; i < rr.n; i++)
        for (int reg = rr.lo[i] < 0 ? 0 : rr.lo[i]; reg <= rr.hi[i] && reg < SEA_MAX_REGISTERS; reg++)
            c->reg_meta[reg].dwe_read = 1;
}

// ---- Compiler helpers ----

static void compiler_error(SeaCompiler *c, int line, const char *fmt, ...) {
    c->had_error = true;
    va_list args;
    va_start(args, fmt);
    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    snprintf(c->err_msg, sizeof(c->err_msg), "line %d: %s", line, msg);
    if (!g_compile_error[0]) {
        snprintf(g_compile_error, sizeof(g_compile_error), "%s", msg);
        g_compile_error_line = line;
    }
}

static int alloc_reg(SeaCompiler *c) {
    int r = c->reg_top++;
    /* bounds first: reg_meta has SEA_MAX_REGISTERS entries (this used to
     * write reg_meta[r] before checking) */
    if (r < 0 || r >= SEA_MAX_REGISTERS) {
        compiler_error(c, 0, "too many registers");
        return 0;
    }
    c->reg_meta[r] = (SeaRegMeta){0};
    c->reg_meta[r].dwe_write_pos = -1;
    c->reg_meta[r].dwe_read = 0;
    if (c->reg_top > c->chunk->max_stack)
        c->chunk->max_stack = (uint8_t)c->reg_top;
    return r;
}

static void free_reg(SeaCompiler *c, int reg) {
    if (reg + 1 == c->reg_top) {
        dwe_free(c, reg);
        c->reg_top--;
    }
}

static void free_regs_to(SeaCompiler *c, int top) {
    for (int r = top; r < c->reg_top; r++)
        dwe_free(c, r);
    c->reg_top = top;
}

static void emit(SeaCompiler *c, uint32_t instr, int line) {
    sea_chunk_emit(c->chunk, instr, line);
    dwe_mark_instr_reads(c, instr);   /* every emission path goes through here */
}

static void emit_abc(SeaCompiler *c, SeaOpcode op, int a, int b, int cc, int line) {
    if (op == OP_MOVE || op == OP_IIADD || op == OP_IISUB)
        dwe_pure_write(c, a);
    emit(c, SEA_MAKE_ABC(op, a, b, cc), line);   /* reads marked in emit() */
}

static void emit_abx(SeaCompiler *c, SeaOpcode op, int a, int bx, int line) {
    if (op == OP_LOAD_K)
        dwe_pure_write(c, a);
    emit(c, SEA_MAKE_ABx(op, a, bx), line);   /* reads marked in emit() */
}

static void emit_asbx(SeaCompiler *c, SeaOpcode op, int a, int sbx, int line) {
    if (op == OP_LOAD_INT)
        dwe_pure_write(c, a);
    emit(c, SEA_MAKE_AsBx(op, a, sbx), line);
}

static int emit_jmp(SeaCompiler *c, SeaOpcode op, int a, int line) {
    int pos = sea_chunk_emit_jmp(c->chunk, op, a, line);
    /* conditional jumps read R[a], numeric-for ops R[a..a+2] (Bug 14) */
    dwe_mark_instr_reads(c, c->chunk->code[pos]);
    return pos;
}

static void patch_jmp(SeaCompiler *c, int pos) {
    sea_chunk_patch_jmp(c->chunk, pos, sea_chunk_pos(c->chunk));
}

/* Patch the B (nret) field of the most recently emitted CALL or CALL_MULTI instruction.
 * Used when the caller knows how many return values to expect. */
static bool patch_last_call_nret(SeaCompiler *c, int nret, int line) {
    int pos = c->chunk->code_len - 1;
    /* a method call: CALL at base, then MOVE dst <- base (the results land
     * one register above dst): ask for nret results and move each down */
    if (pos > 0 && SEA_INSTR_OP(c->chunk->code[pos]) == OP_MOVE &&
        SEA_INSTR_OP(c->chunk->code[pos - 1]) == OP_CALL &&
        SEA_INSTR_A(c->chunk->code[pos - 1]) == SEA_INSTR_B(c->chunk->code[pos])) {
        uint32_t call = c->chunk->code[pos - 1];
        int mdst = SEA_INSTR_A(c->chunk->code[pos]), msrc = SEA_INSTR_B(c->chunk->code[pos]);
        if (msrc == mdst + 1) {
            c->chunk->code[pos - 1] = (call & 0xFF00FFFF) | ((uint32_t)(nret & 0xFF) << 16);
            for (int k = 1; k < nret; k++) emit_abc(c, OP_MOVE, mdst + k, msrc + k, 0, line);
            return true;
        }
    }
    while (pos >= 0) {
        SeaOpcode op = SEA_INSTR_OP(c->chunk->code[pos]);
        if (op == OP_CALL || op == OP_CALL_MULTI || op == OP_CALL_SELF) {
            /* Patch B field (bits 16..23) */
            c->chunk->code[pos] = (c->chunk->code[pos] & 0xFF00FFFF) | ((uint32_t)(nret & 0xFF) << 16);
            return true;
        }
        /* Stop searching if we hit something other than VARARG (which immediately precedes CALL for vararg calls) */
        if (op != OP_VARARG) break;
        pos--;
    }
    return false;   /* not a plain call at the end (e.g. a held callee): one value */
}

static int add_const(SeaCompiler *c, SeaValue v) {
    return sea_chunk_add_const(c->chunk, v);
}

static int add_const_str(SeaCompiler *c, const char *s) {
    return sea_chunk_add_const_str(c->chunk, s);
}

// ---- Ownership (docs/memory.md) ----
/*
 * Named locals (parameters included) own their value: +1 on an object, from
 * declaration to every exit of their scope (end of block, break, continue,
 * return, `?`). Temporaries carry is_owned when they hold a reference the code
 * must consume. Expression results are scalar (no RC), borrowed (a local's,
 * a field's, a global's value) or owned (calls, constructors, concat, channel
 * receives). Arguments are passed owned and the callee owns its parameters;
 * return values are owned.
 */

static inline bool reg_is_named_local(SeaCompiler *c, int r);

/* Make the value in temporary r owned (+1): retain it unless it already is
 * owned or needs no RC. */
static void own_temp(SeaCompiler *c, int r, int line) {
    if (c->reg_meta[r].is_owned || reg_scalar(c, r)) return;
    emit_abc(c, OP_RETAIN, r, 0, 0, line);
    c->reg_meta[r].is_owned = 1;
}

/* Drop the reference an owned temporary holds (its value was stored in a
 * container, compared, discarded...). Never touches a named local. */
static void drop_temp(SeaCompiler *c, int r, int line) {
    if (!c->reg_meta[r].is_owned || reg_is_named_local(c, r)) return;
    c->reg_meta[r].is_owned = 0;
    if (!reg_scalar(c, r)) emit_abc(c, OP_RELEASE, r, 0, 0, line);
}

/* val_reg is an owned temporary whose reference a store may take over
 * (OP_SET_FIELD_MOVE / OP_ARRAY_PUSH_MOVE) instead of retain + release. */
static bool can_move_from(SeaCompiler *c, int val_reg) {
    return c->reg_meta[val_reg].is_owned && !reg_scalar(c, val_reg) &&
           !reg_is_named_local(c, val_reg);
}

/* Does local i hold a reference that leaving its scope must release? */
static bool local_needs_release(SeaCompiler *c, int i) {
    SeaLocal *loc = &c->locals[i];
    if (loc->captured) return false;          /* CLOSE_UP moves it to the closure */
    int r = loc->reg;
    if (c->reg_meta[r].has_copy) return false;  /* alias of a scalar local */
    return !reg_scalar(c, r);
}

/* A captured local still owns its value when no closure was created for it
 * (the closure sits in a branch that did not run): CLOSE_UP moves the value
 * only into upvalues that exist and nils those registers, so a RELEASE
 * after it frees the rest (and is a no-op for the moved ones). Emitted right
 * after a CLOSE_UP that covers c->locals[from..). Bug 20 in docs/bugs.md. */
static void emit_release_captured(SeaCompiler *c, int from, int keep_reg, int line) {
    for (int i = c->local_count - 1; i >= from && i >= 0; i--) {
        SeaLocal *loc = &c->locals[i];
        if (!loc->captured || loc->reg == keep_reg || reg_scalar(c, loc->reg)) continue;
        emit(c, SEA_MAKE_ABC(OP_RELEASE, loc->reg, 0, 0), line);
    }
}

/* Release the locals c->locals[from..] (innermost first) for a jump out of
 * their scopes, closing the upvalues of the captured ones first (break,
 * continue and return: `close` is kept for the callers, a return used to
 * leave the closing to RETURN). keep_reg is not released (a local whose
 * reference `return` hands over). */
static void emit_locals_cleanup(SeaCompiler *c, int from, bool close, int keep_reg, int line) {
    (void)close;
    int close_from = -1;
    for (int i = c->local_count - 1; i >= from && i >= 0; i--) {
        SeaLocal *loc = &c->locals[i];
        if (loc->captured) {
            if (close_from < 0 || loc->reg < close_from) {
                if (c->reg_meta[loc->reg].has_copy) {
                    emit_abc(c, OP_MOVE, loc->reg, c->reg_meta[loc->reg].copy_of, 0, line);
                    c->reg_meta[loc->reg].has_copy = 0;
                }
                close_from = loc->reg;
            }
            continue;
        }
        if (loc->reg == keep_reg || !local_needs_release(c, i)) continue;
        /* RELEASE nils the register: emitted on a jump path, that must not
         * change what the fall-through code believes about the local */
        emit(c, SEA_MAKE_ABC(OP_RELEASE, loc->reg, 0, 0), line);
    }
    if (close_from >= 0) {
        emit_abx(c, OP_CLOSE_UP, close_from, 0, line);
        emit_release_captured(c, from, keep_reg, line);
    }
}

/* Owned temporaries live at this point (not named locals), e.g. arguments
 * already evaluated when a `?` returns early. */
static void emit_temps_cleanup(SeaCompiler *c, int except, int line) {
    for (int r = 0; r < c->reg_top; r++) {
        if (r == except || !c->reg_meta[r].is_owned || reg_scalar(c, r)) continue;
        if (reg_is_named_local(c, r)) continue;
        emit(c, SEA_MAKE_ABC(OP_RELEASE, r, 0, 0), line);
    }
}

// ---- Scope management ----

static void begin_scope(SeaCompiler *c) {
    c->scope_depth++;
}

static void end_scope(SeaCompiler *c, int line) {
    c->scope_depth--;
    // Close upvalues for locals that go out of scope
    // and free their registers
    int close_from = -1;
    int captured_regs[SEA_MAX_REGISTERS];
    int ncaptured = 0;
    while (c->local_count > 0 &&
           c->locals[c->local_count-1].depth > c->scope_depth) {
        SeaLocal *loc = &c->locals[c->local_count-1];
        /* Before this local's register is freed, materialize any other locals
         * that are currently aliasing it via copy propagation. */
        materialize_copies_of(c, loc->reg, line);
        if (loc->captured) {
            /* If this local is a deferred copy, materialize before close-up. */
            if (c->reg_meta[loc->reg].has_copy) {
                emit_abc(c, OP_MOVE, loc->reg, c->reg_meta[loc->reg].copy_of, 0, line);
                c->reg_meta[loc->reg].has_copy = 0;
            }
            if (close_from < 0 || loc->reg < close_from)
                close_from = loc->reg;
            if (!reg_scalar(c, loc->reg)) captured_regs[ncaptured++] = loc->reg;
        } else if (local_needs_release(c, c->local_count - 1)) {
            emit_abc(c, OP_RELEASE, loc->reg, 0, 0, line);
        }
        c->reg_meta[loc->reg].is_owned = 0;
        if (loc->name) { sea_free(loc->name, strlen(loc->name) + 1); loc->name = NULL; }
        c->local_count--;
    }
    if (close_from >= 0) {
        emit_abx(c, OP_CLOSE_UP, close_from, 0, line);
        /* see emit_release_captured */
        for (int k = 0; k < ncaptured; k++) emit_abc(c, OP_RELEASE, captured_regs[k], 0, 0, line);
    }
    // Reset reg_top to after last local
    if (c->local_count > 0) {
        c->reg_top = c->locals[c->local_count-1].reg + 1;
    } else {
        c->reg_top = 0;
    }
}

/* Per-iteration upvalue closing for loops: the lowest register among the
 * captured locals in [from_local, local_count) — the loop's own variables and
 * the body's locals — materializing deferred copies first; -1 if none is
 * captured. Emitting CLOSE_UP from it at the end of every iteration (on the
 * `continue` path too) gives each iteration its own variables, as in Lua 5.4
 * and Go 1.22: closures and goroutines created in the body capture that
 * iteration's values. */
static int loop_close_from(SeaCompiler *c, int from_local, int line) {
    int close_from = -1;
    for (int i = from_local; i < c->local_count; i++) {
        if (!c->locals[i].captured) continue;
        int reg = c->locals[i].reg;
        if (c->reg_meta[reg].has_copy) {
            emit_abc(c, OP_MOVE, reg, c->reg_meta[reg].copy_of, 0, line);
            c->reg_meta[reg].has_copy = 0;
        }
        if (close_from < 0 || reg < close_from) close_from = reg;
    }
    return close_from;
}

/* Register an existing register as a named local (no alloc_reg). */
static void add_local_at(SeaCompiler *c, const char *name, int reg, int line) {
    if (c->local_count >= SEA_MAX_REGISTERS) {
        compiler_error(c, line, "too many local variables");
        return;
    }
    SeaLocal *loc = &c->locals[c->local_count++];
    loc->name = (char*)sea_malloc(strlen(name) + 1);
    strcpy(loc->name, name);
    loc->depth    = c->scope_depth;
    loc->captured = false;
    loc->reg      = reg;
}

static int add_local(SeaCompiler *c, const char *name, int line) {
    if (c->local_count >= SEA_MAX_REGISTERS) {
        compiler_error(c, line, "too many local variables");
        return -1;
    }
    int reg = alloc_reg(c);
    SeaLocal *loc = &c->locals[c->local_count++];
    loc->name = (char*)sea_malloc(strlen(name)+1);
    strcpy(loc->name, name);
    loc->depth = c->scope_depth;
    loc->captured = false;
    loc->is_const = false;
    loc->reg = reg;
    return reg;
}

static int resolve_local(SeaCompiler *c, const char *name) {
    for (int i = c->local_count - 1; i >= 0; i--) {
        if (strcmp(c->locals[i].name, name) == 0) {
            return c->locals[i].reg;
        }
    }
    return -1;
}

static SeaLocal* find_local(SeaCompiler *c, const char *name) {
    for (int i = c->local_count - 1; i >= 0; i--)
        if (strcmp(c->locals[i].name, name) == 0)
            return &c->locals[i];
    return NULL;
}

static int add_upvalue(SeaCompiler *c, bool is_local, int idx, const char *name) {
    // Check if already captured
    for (int i = 0; i < c->upval_count; i++) {
        if (c->upvalues[i].is_local == is_local && c->upvalues[i].idx == (uint8_t)idx)
            return i;
    }
    if (c->upval_count >= SEA_MAX_UPVALUES) return -1;
    int n = c->upval_count;
    SeaUpvalEntry *up = &c->upvalues[n];
    up->is_local = is_local;
    up->idx = (uint8_t)idx;
    up->name = name ? (char*)sea_malloc(strlen(name)+1) : NULL;
    if (name) strcpy(up->name, name);

    // Sync to chunk descriptor
    c->chunk->upvals = SEA_REALLOC_N(c->chunk->upvals, SeaUpvalDesc, n, n + 1);
    c->chunk->upvals[n].is_local = is_local;
    c->chunk->upvals[n].idx = (uint8_t)idx;
    c->chunk->upvals[n].name = up->name ? (char*)sea_malloc(strlen(up->name)+1) : NULL;
    if (up->name) strcpy(c->chunk->upvals[n].name, up->name);
    c->chunk->upval_count = n + 1;

    return c->upval_count++;
}

static int resolve_upvalue(SeaCompiler *c, const char *name);

static int resolve_upvalue(SeaCompiler *c, const char *name) {
    if (!c->parent) return -1;

    // Check parent's locals
    int local = resolve_local(c->parent, name);
    if (local >= 0) {
        // Mark that local as captured and propagate its is_const flag
        bool src_const = false;
        for (int i = 0; i < c->parent->local_count; i++) {
            if (c->parent->locals[i].reg == local) {
                c->parent->locals[i].captured = true;
                src_const = c->parent->locals[i].is_const;
                /* Materialize deferred copy before capturing: OP_CLOSE_UP will
                 * move the value to the heap, so the register must be live. */
                if (c->parent->reg_meta[local].has_copy) {
                    uint32_t mv = SEA_MAKE_ABC(OP_MOVE, local,
                                               c->parent->reg_meta[local].copy_of, 0);
                    sea_chunk_emit(c->parent->chunk, mv, 0);
                    dwe_mark_instr_reads(c->parent, mv);
                    c->parent->reg_meta[local].has_copy = 0;
                }
                break;
            }
        }
        int uv = add_upvalue(c, true, local, name);
        if (uv >= 0) c->upvalues[uv].is_const = src_const;
        return uv;
    }

    // Check parent's upvalues (transitive capture)
    int upval = resolve_upvalue(c->parent, name);
    if (upval >= 0) {
        bool src_const = (upval < c->parent->upval_count) && c->parent->upvalues[upval].is_const;
        int uv = add_upvalue(c, false, upval, name);
        if (uv >= 0) c->upvalues[uv].is_const = src_const;
        return uv;
    }

    return -1;
}

/* If `node` is a simple local-variable reference, return its register without
 * allocating a fresh temporary.  Returns -1 for anything else (globals, upvals,
 * complex exprs).  Used by BINOP codegen to avoid pointless MOVE instructions
 * when the operands are already sitting in known registers. */
static int expr_local_reg(SeaCompiler *c, SeaNode *node) {
    if (!node || node->type != NODE_IDENT) return -1;
    int r = resolve_local(c, node->ident);
    if (r < 0) return -1;
    return resolve_copy_chain(c, r);
}

// ---- Escape analysis ----

/* Returns true if n is a struct table literal (all-string keys, non-empty). */
static bool esc_is_struct_table(SeaNode *n) {
    if (!n || n->type != NODE_TABLE || n->table.count == 0 || !n->table.keys) return false;
    for (int i = 0; i < n->table.count; i++) {
        SeaNode *k = n->table.keys[i];
        if (!k || k->type != NODE_STRING) return false;
    }
    return true;
}

/* Returns true if n is an empty array literal `[]` (no explicit capacity),
 * the only form eligible for arena-header allocation. */
static bool esc_is_array_lit(SeaNode *n) {
    return n && n->type == NODE_ARRAY_LIT && n->array_lit.count == 0;
}

/* Returns false if any field value would set reg_owned=1 in compile_expr,
 * which would cause OP_RELEASE to free the object before the arena borrow is safe. */
static bool esc_fields_borrow_safe(SeaNode *table_node) {
    for (int i = 0; i < table_node->table.count; i++) {
        SeaNode *v = table_node->table.vals[i];
        if (!v) continue;
        switch (v->type) {
        case NODE_CALL:
        case NODE_METHOD_CALL:
        case NODE_ARRAY_LIT:
        case NODE_MAP_LIT:
            return false;
        case NODE_BINOP:
            if (v->binop.op == BINOP_CONCAT) return false;
            break;
        default: break;
        }
    }
    return true;
}

static void esc_mark(SeaEscapeInfo *ei, const char *name) {
    if (!name) return;
    if (ei->collect) {
        for (int i = 0; i < ei->name_count; i++)
            if (strcmp(ei->names[i], name) == 0) return;
        if (ei->name_count == ei->name_cap) {
            int nc = ei->name_cap ? ei->name_cap * 2 : 16;
            ei->names = (const char**)sea_realloc((void*)ei->names,
                sizeof(char*) * (size_t)ei->name_cap, sizeof(char*) * (size_t)nc);
            ei->name_cap = nc;
        }
        ei->names[ei->name_count++] = name;
        return;
    }
    for (int i = 0; i < ei->count; i++) {
        if (strcmp(ei->cands[i].local_name, name) == 0)
            ei->cands[i].escaped = true;
    }
}


/* Mark only struct candidates with this name (not array candidates).
 * Used for index/field assignment targets: mutating a struct's fields disqualifies
 * arena borrow, but mutating an array (push/index-set) is the normal, safe case. */
static void esc_mark_struct_only(SeaEscapeInfo *ei, const char *name) {
    if (!name) return;
    for (int i = 0; i < ei->count; i++) {
        if (ei->cands[i].table_node->type == NODE_TABLE &&
            strcmp(ei->cands[i].local_name, name) == 0)
            ei->cands[i].escaped = true;
    }
}

/* Recursively walk the whole subtree. Once inside a closure body (mark_idents=true),
 * every identifier reference marks a same-named candidate as escaped: a captured local
 * outlives the frame inside the closure's upvalue, but an arena object is reclaimed on
 * frame return — so any capture must disqualify arena allocation. */
static void esc_walk_captures(SeaEscapeInfo *ei, SeaNode *n, bool mark_idents) {
    if (!n) return;
    switch (n->type) {
    case NODE_IDENT:
        if (mark_idents) esc_mark(ei, n->ident);
        break;
    case NODE_UNOP:
    case NODE_CHAN_RECV:
        esc_walk_captures(ei, n->unop.expr, mark_idents);
        break;
    case NODE_BINOP:
    case NODE_CHAN_SEND:
        esc_walk_captures(ei, n->binop.left, mark_idents);
        esc_walk_captures(ei, n->binop.right, mark_idents);
        break;
    case NODE_INDEX:
        esc_walk_captures(ei, n->index.obj, mark_idents);
        esc_walk_captures(ei, n->index.key, mark_idents);
        break;
    case NODE_FIELD:
        esc_walk_captures(ei, n->field.obj, mark_idents);
        break;
    case NODE_CALL:
        esc_walk_captures(ei, n->call.fn, mark_idents);
        for (int i = 0; i < n->call.argc; i++) esc_walk_captures(ei, n->call.args[i], mark_idents);
        break;
    case NODE_METHOD_CALL:
        esc_walk_captures(ei, n->method.obj, mark_idents);
        for (int i = 0; i < n->method.argc; i++) esc_walk_captures(ei, n->method.args[i], mark_idents);
        break;
    case NODE_FUNC:
        /* Entering a closure body: everything referenced inside is captured. */
        esc_walk_captures(ei, n->func.body, true);
        break;
    case NODE_TABLE:
        for (int i = 0; i < n->table.count; i++) {
            if (n->table.keys) esc_walk_captures(ei, n->table.keys[i], mark_idents);
            esc_walk_captures(ei, n->table.vals[i], mark_idents);
        }
        break;
    case NODE_ARRAY_LIT:
        for (int i = 0; i < n->array_lit.count; i++)
            esc_walk_captures(ei, n->array_lit.elems[i], mark_idents);
        break;
    case NODE_TEMPLATE:
        for (int i = 0; i < n->tmpl.count; i++)
            esc_walk_captures(ei, n->tmpl.exprs[i], mark_idents);
        break;
    case NODE_PROPAGATE:
        esc_walk_captures(ei, n->propagate, mark_idents);
        break;
    case NODE_BLOCK:
    case NODE_DO:
        for (int i = 0; i < n->block.count; i++) esc_walk_captures(ei, n->block.stmts[i], mark_idents);
        break;
    case NODE_LOCAL:
        for (int i = 0; i < n->local.expr_count; i++) esc_walk_captures(ei, n->local.exprs[i], mark_idents);
        break;
    case NODE_ASSIGN:
        for (int i = 0; i < n->assign.count; i++) esc_walk_captures(ei, n->assign.targets[i], mark_idents);
        for (int i = 0; i < n->assign.expr_count; i++) esc_walk_captures(ei, n->assign.exprs[i], mark_idents);
        break;
    case NODE_IF:
        esc_walk_captures(ei, n->if_stmt.cond, mark_idents);
        esc_walk_captures(ei, n->if_stmt.then_body, mark_idents);
        esc_walk_captures(ei, n->if_stmt.else_body, mark_idents);
        break;
    case NODE_WHILE:
        esc_walk_captures(ei, n->while_loop.cond, mark_idents);
        esc_walk_captures(ei, n->while_loop.body, mark_idents);
        break;
    case NODE_FOR_NUM:
        esc_walk_captures(ei, n->for_num.start, mark_idents);
        esc_walk_captures(ei, n->for_num.limit, mark_idents);
        esc_walk_captures(ei, n->for_num.step, mark_idents);
        esc_walk_captures(ei, n->for_num.body, mark_idents);
        break;
    case NODE_FOR_IN:
        esc_walk_captures(ei, n->for_in.iter_expr, mark_idents);
        esc_walk_captures(ei, n->for_in.body, mark_idents);
        break;
    case NODE_RETURN:
        for (int i = 0; i < n->ret.count; i++) esc_walk_captures(ei, n->ret.exprs[i], mark_idents);
        break;
    case NODE_GO:
        esc_walk_captures(ei, n->go_call, mark_idents);
        break;
    case NODE_SELECT:
        for (int i = 0; i < n->select.count; i++) {
            esc_walk_captures(ei, n->select.cases[i].stmt, mark_idents);
            esc_walk_captures(ei, n->select.cases[i].body, mark_idents);
        }
        break;
    case NODE_DEFER:
        esc_walk_captures(ei, n->defer_stmt, mark_idents);
        break;
    case NODE_PANIC:
        esc_walk_captures(ei, n->panic_expr, mark_idents);
        break;
    case NODE_DEL:
        esc_walk_captures(ei, n->del_stmt, mark_idents);
        break;
    case NODE_EXPR_STMT:
        esc_walk_captures(ei, n->expr_stmt, mark_idents);
        break;
    default:
        break;
    }
}

/* Escape scan: a candidate escapes when its name is used anywhere a reference
 * to it could be kept — assigned, passed (except to len), returned, stored in
 * a constructor, sent, captured (esc_walk_captures). Reading a field or
 * element (`p.x`, `a[i]`) and arithmetic/comparison operands do not escape.
 * Name-based, so shadowing only over-approximates. */
static void esc_scan(SeaEscapeInfo *ei, SeaNode *n);

/* An operand that is only read (not kept): a bare name does not escape. */
static void esc_scan_operand(SeaEscapeInfo *ei, SeaNode *n) {
    if (n && n->type == NODE_IDENT) return;
    esc_scan(ei, n);
}

static void esc_scan(SeaEscapeInfo *ei, SeaNode *n) {
    if (!n) return;
    switch (n->type) {
    case NODE_IDENT:
        esc_mark(ei, n->ident);
        break;
    case NODE_FIELD:
        esc_scan_operand(ei, n->field.obj);
        break;
    case NODE_INDEX:
        esc_scan_operand(ei, n->index.obj);
        esc_scan(ei, n->index.key);
        break;
    case NODE_BINOP: case NODE_CHAN_SEND:
        if (n->type == NODE_CHAN_SEND || n->binop.op == BINOP_CHAN_SEND ||
            n->binop.op == BINOP_AND || n->binop.op == BINOP_OR) {
            esc_scan(ei, n->binop.left);
            esc_scan(ei, n->binop.right);
        } else {
            esc_scan_operand(ei, n->binop.left);
            esc_scan_operand(ei, n->binop.right);
        }
        break;
    case NODE_UNOP:
        if (n->unop.op == UNOP_CHAN_RECV) esc_scan(ei, n->unop.expr);
        else esc_scan_operand(ei, n->unop.expr);
        break;
    case NODE_CHAN_RECV:
        esc_scan(ei, n->unop.expr);
        break;
    case NODE_CALL: {
        bool is_len = n->call.fn->type == NODE_IDENT && strcmp(n->call.fn->ident, "len") == 0;
        esc_scan(ei, n->call.fn);
        for (int i = 0; i < n->call.argc; i++) {
            if (is_len) esc_scan_operand(ei, n->call.args[i]);
            else esc_scan(ei, n->call.args[i]);
        }
        break;
    }
    case NODE_METHOD_CALL:
        esc_scan(ei, n->method.obj);          /* passed as self */
        for (int i = 0; i < n->method.argc; i++) esc_scan(ei, n->method.args[i]);
        break;
    case NODE_TABLE:
        for (int i = 0; i < n->table.count; i++) {
            if (n->table.keys) esc_scan(ei, n->table.keys[i]);
            esc_scan(ei, n->table.vals[i]);
        }
        break;
    case NODE_ARRAY_LIT:
        for (int i = 0; i < n->array_lit.count; i++)
            esc_scan(ei, n->array_lit.elems[i]);
        break;
    case NODE_TEMPLATE:
        for (int i = 0; i < n->tmpl.count; i++)
            esc_scan(ei, n->tmpl.exprs[i]);
        break;
    case NODE_PROPAGATE:
        esc_scan(ei, n->propagate);
        break;
    case NODE_FUNC:
        break;   /* captures: esc_walk_captures */
    case NODE_EXPR_STMT:
        esc_scan(ei, n->expr_stmt);
        break;
    case NODE_LOCAL:
        for (int i = 0; i < n->local.expr_count; i++) esc_scan(ei, n->local.exprs[i]);
        break;
    case NODE_ASSIGN:
        for (int i = 0; i < n->assign.expr_count; i++) esc_scan(ei, n->assign.exprs[i]);
        for (int i = 0; i < n->assign.count; i++) {
            SeaNode *t = n->assign.targets[i];
            if (t->type == NODE_FIELD) {
                /* mutating a candidate struct's fields disqualifies it (kept
                 * conservative); arrays may be mutated in the arena */
                if (t->field.obj->type == NODE_IDENT) esc_mark(ei, t->field.obj->ident);
                else esc_scan(ei, t->field.obj);
            } else if (t->type == NODE_INDEX) {
                if (t->index.obj->type == NODE_IDENT) esc_mark_struct_only(ei, t->index.obj->ident);
                else esc_scan(ei, t->index.obj);
                esc_scan(ei, t->index.key);
            }
        }
        break;
    case NODE_BLOCK: case NODE_DO:
        for (int i = 0; i < n->block.count; i++) esc_scan(ei, n->block.stmts[i]);
        break;
    case NODE_IF:
        esc_scan(ei, n->if_stmt.cond);
        esc_scan(ei, n->if_stmt.then_body);
        esc_scan(ei, n->if_stmt.else_body);
        break;
    case NODE_WHILE:
        esc_scan(ei, n->while_loop.cond);
        esc_scan(ei, n->while_loop.body);
        break;
    case NODE_FOR_NUM:
        esc_scan(ei, n->for_num.start);
        esc_scan(ei, n->for_num.limit);
        esc_scan(ei, n->for_num.step);
        esc_scan(ei, n->for_num.body);
        break;
    case NODE_FOR_IN:
        esc_scan(ei, n->for_in.iter_expr);
        esc_scan(ei, n->for_in.body);
        break;
    case NODE_RETURN:
        for (int i = 0; i < n->ret.count; i++) esc_scan(ei, n->ret.exprs[i]);
        break;
    case NODE_GO:     esc_scan(ei, n->go_call); break;
    case NODE_DEFER:  esc_scan(ei, n->defer_stmt); break;
    case NODE_PANIC:  esc_scan(ei, n->panic_expr); break;
    case NODE_DEL:
        if (n->del_stmt && n->del_stmt->type == NODE_INDEX) {
            esc_scan_operand(ei, n->del_stmt->index.obj);
            esc_scan(ei, n->del_stmt->index.key);
        }
        break;
    case NODE_SELECT:
        for (int i = 0; i < n->select.count; i++) {
            esc_scan(ei, n->select.cases[i].stmt);
            esc_scan(ei, n->select.cases[i].body);
        }
        break;
    default:
        break;
    }
}

/* Collect arena candidates in a block (shallow, top-level locals only):
 *   - `local x = {all-string-keys table}` → arena struct (borrow fields)
 *   - `local x = []`                       → arena array header (heap data buffer)
 * Only top-level declarations qualify: an arena object declared inside a loop body
 * would accumulate across iterations (the arena resets per-frame, not per-iteration). */
static void esc_collect_block(SeaNode *block, SeaEscapeInfo *out) {
    if (!block || block->type != NODE_BLOCK) return;
    for (int i = 0; i < block->block.count; i++) {
        SeaNode *s = block->block.stmts[i];
        if (!s || s->type != NODE_LOCAL) continue;
        if (s->local.count != 1 || s->local.expr_count != 1) continue;
        SeaNode *expr = s->local.exprs[0];
        bool is_struct = esc_is_struct_table(expr) && esc_fields_borrow_safe(expr);
        bool is_array  = esc_is_array_lit(expr);
        if (!is_struct && !is_array) continue;
        if (out->count >= SEA_ESCAPE_MAX_CANDS) continue;
        SeaEscapeCand *cand = &out->cands[out->count++];
        cand->table_node  = expr;
        cand->local_name  = s->local.names[0];
        cand->escaped     = false;
    }
}

/* Pre-scan function body and populate ei with non-escaping struct/array candidates. */
/* Names referenced inside any closure in this function body (by name, so
 * shadowing can only over-approximate). An assignment to a local with one of
 * these names is followed by OP_MARK_CAPTURED. */
static void sea_collect_captured(SeaNode *fn_body, SeaEscapeInfo *out) {
    memset(out, 0, sizeof *out);
    out->collect = true;
    esc_walk_captures(out, fn_body, false);
}

static void sea_free_captured(SeaEscapeInfo *ei) {
    if (ei->names) sea_free((void*)ei->names, sizeof(char*) * (size_t)ei->name_cap);
    ei->names = NULL;
    ei->name_count = ei->name_cap = 0;
}

static bool name_is_captured(SeaCompiler *c, const char *name) {
    for (int i = 0; i < c->captured.name_count; i++)
        if (strcmp(c->captured.names[i], name) == 0) return true;
    return false;
}

static void emit_abc(SeaCompiler *c, SeaOpcode op, int a, int b, int cc, int line);

/* After assigning local `name` (register reg): if a closure captures it, a
 * goroutine may see the new value through an open upvalue (OP_MARK_CAPTURED
 * shares it at run time only when that upvalue is shared). */
static void mark_if_captured(SeaCompiler *c, const char *name, int reg, int line) {
    if (name_is_captured(c, name)) {
        emit_abc(c, OP_MARK_CAPTURED, reg, 0, 0, line);
        /* a closure may change it at any call: no constant/int facts */
        c->reg_meta[reg].is_int = 0;
        c->reg_meta[reg].is_const = 0;
        c->reg_meta[reg].is_scalar = 0;
    }
}

static void sea_scan_escapes(SeaNode *fn_body, SeaEscapeInfo *out) {
    out->count = 0;
    esc_collect_block(fn_body, out);
    if (out->count == 0) return;
    esc_scan(out, fn_body);
    /* Capture detection: any candidate referenced inside a closure escapes the frame. */
    esc_walk_captures(out, fn_body, false);
}

static bool esc_is_arena_candidate(SeaEscapeInfo *ei, SeaNode *node) {
    for (int i = 0; i < ei->count; i++) {
        if (ei->cands[i].table_node == node && !ei->cands[i].escaped) return true;
    }
    return false;
}

// ---- Forward declarations ----
static int compile_expr(SeaCompiler *c, SeaNode *node, int dst);
static void compile_stmt(SeaCompiler *c, SeaNode *node);

/* `local v, ok = m[k]` and `local v, ok = m.k` (also in assignments): the
 * two-value lookup, whose ok tells whether the key is there (even holding nil) */
static bool is_two_value_lookup(SeaNode *e) {
    return (e->type == NODE_INDEX && e->index.key != NULL) || e->type == NODE_FIELD;
}

/* the container and the key of a two-value lookup, in fresh registers (the
 * key of m.k is the constant string "k") */
static void compile_lookup_operands(SeaCompiler *c, SeaNode *e, int *obj_reg, int *key_reg, int line) {
    if (e->type == NODE_FIELD) {
        *obj_reg = compile_expr(c, e->field.obj, -1);
        *key_reg = alloc_reg(c);
        emit_abx(c, OP_LOAD_K, *key_reg, add_const_str(c, e->field.field), line);
        return;
    }
    *obj_reg = compile_expr(c, e->index.obj, -1);
    *key_reg = compile_expr(c, e->index.key, -1);
}
static void compile_block(SeaCompiler *c, SeaNode *block);

/* ---- a op= b with a target that has effects (t[f()] += 1) ----
 * Simple targets were turned into a = a op b by the parser. Here the object
 * (and the key) are evaluated once into hidden locals:
 *     do local (o) = obj; local (k) = key; (o)[(k)] = (o)[(k)] op b end
 * The nodes built here borrow the original subtrees and are freed after. */
static char *cc_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char*)sea_malloc(n);
    memcpy(d, s, n);
    return d;
}

static SeaNode *cc_ident(const char *name, int line) {
    SeaNode *n = sea_node_alloc(NODE_IDENT, line);
    n->ident = cc_strdup(name);
    return n;
}

static SeaNode *cc_local(const char *name, SeaNode *value, int line) {
    SeaNode *n = sea_node_alloc(NODE_LOCAL, line);
    n->local.names = (char**)sea_malloc(sizeof(char*));
    n->local.names[0] = cc_strdup(name);
    n->local.count = 1;
    n->local.exprs = (SeaNode**)sea_malloc(sizeof(SeaNode*));
    n->local.exprs[0] = value;
    n->local.expr_count = 1;
    return n;
}

static SeaNode *cc_target(SeaNode *t, bool hidden_key, int line) {
    SeaNode *n = sea_node_alloc(t->type, line);
    if (t->type == NODE_FIELD) {
        n->field.obj = cc_ident("(o)", line);
        n->field.field = cc_strdup(t->field.field);
    } else {
        n->index.obj = cc_ident("(o)", line);
        n->index.key = hidden_key ? cc_ident("(k)", line) : NULL;
    }
    return n;
}

static void compile_stmt(SeaCompiler *c, SeaNode *node);

static void compile_compound_assign(SeaCompiler *c, SeaNode *node) {
    SeaNode *t = node->assign.targets[0];
    SeaNode *rhs = node->assign.exprs[0];
    int line = node->line;
    SeaNode *obj = t->type == NODE_FIELD ? t->field.obj : t->index.obj;
    SeaNode *key = t->type == NODE_INDEX ? t->index.key : NULL;

    SeaNode *stmts[3];
    int ns = 0;
    stmts[ns++] = cc_local("(o)", obj, line);
    if (key) stmts[ns++] = cc_local("(k)", key, line);

    SeaNode *bin = sea_node_alloc(NODE_BINOP, line);
    bin->binop.op = (SeaBinop)(node->assign.op - 1);
    bin->binop.left = cc_target(t, key != NULL, line);
    bin->binop.right = rhs;
    SeaNode *as = sea_node_alloc(NODE_ASSIGN, line);
    as->assign.targets = (SeaNode**)sea_malloc(sizeof(SeaNode*));
    as->assign.targets[0] = cc_target(t, key != NULL, line);
    as->assign.count = 1;
    as->assign.exprs = (SeaNode**)sea_malloc(sizeof(SeaNode*));
    as->assign.exprs[0] = bin;
    as->assign.expr_count = 1;
    stmts[ns++] = as;

    SeaNode *blk = sea_node_alloc(NODE_DO, line);
    blk->block.stmts = (SeaNode**)sea_malloc(sizeof(SeaNode*) * (size_t)ns);
    for (int i = 0; i < ns; i++) blk->block.stmts[i] = stmts[i];
    blk->block.count = ns;
    blk->block.end_line = line;

    compile_stmt(c, blk);

    /* give the borrowed subtrees back, then free what was built */
    stmts[0]->local.exprs[0] = NULL;
    if (key) stmts[1]->local.exprs[0] = NULL;
    bin->binop.right = NULL;
    sea_node_free(blk);
}

/* Is `name` the target of an assignment anywhere in `n` (nested functions
 * included)? Name-based, so shadowing only over-approximates. */
static bool name_assigned_in(SeaNode *n, const char *name) {
    if (!n || !name) return false;
    switch (n->type) {
    case NODE_ASSIGN:
        for (int i = 0; i < n->assign.count; i++) {
            SeaNode *t = n->assign.targets[i];
            if (t->type == NODE_IDENT && strcmp(t->ident, name) == 0) return true;
            if (name_assigned_in(t, name)) return true;
        }
        for (int i = 0; i < n->assign.expr_count; i++)
            if (name_assigned_in(n->assign.exprs[i], name)) return true;
        return false;
    case NODE_BLOCK: case NODE_DO:
        for (int i = 0; i < n->block.count; i++)
            if (name_assigned_in(n->block.stmts[i], name)) return true;
        return false;
    case NODE_LOCAL:
        for (int i = 0; i < n->local.expr_count; i++)
            if (name_assigned_in(n->local.exprs[i], name)) return true;
        return false;
    case NODE_FUNC:   return name_assigned_in(n->func.body, name);
    case NODE_IF:
        return name_assigned_in(n->if_stmt.cond, name) || name_assigned_in(n->if_stmt.then_body, name) ||
               name_assigned_in(n->if_stmt.else_body, name);
    case NODE_WHILE:  return name_assigned_in(n->while_loop.cond, name) || name_assigned_in(n->while_loop.body, name);
    case NODE_FOR_NUM:
        return name_assigned_in(n->for_num.start, name) || name_assigned_in(n->for_num.limit, name) ||
               name_assigned_in(n->for_num.step, name) || name_assigned_in(n->for_num.body, name);
    case NODE_FOR_IN: return name_assigned_in(n->for_in.iter_expr, name) || name_assigned_in(n->for_in.body, name);
    case NODE_SELECT:
        for (int i = 0; i < n->select.count; i++)
            if (name_assigned_in(n->select.cases[i].stmt, name) ||
                name_assigned_in(n->select.cases[i].body, name)) return true;
        return false;
    case NODE_RETURN:
        for (int i = 0; i < n->ret.count; i++)
            if (name_assigned_in(n->ret.exprs[i], name)) return true;
        return false;
    case NODE_CALL:
        if (name_assigned_in(n->call.fn, name)) return true;
        for (int i = 0; i < n->call.argc; i++)
            if (name_assigned_in(n->call.args[i], name)) return true;
        return false;
    case NODE_METHOD_CALL:
        if (name_assigned_in(n->method.obj, name)) return true;
        for (int i = 0; i < n->method.argc; i++)
            if (name_assigned_in(n->method.args[i], name)) return true;
        return false;
    case NODE_TABLE:
        for (int i = 0; i < n->table.count; i++)
            if (name_assigned_in(n->table.vals[i], name)) return true;
        return false;
    case NODE_ARRAY_LIT:
        for (int i = 0; i < n->array_lit.count; i++)
            if (name_assigned_in(n->array_lit.elems[i], name)) return true;
        return false;
    case NODE_TEMPLATE:
        for (int i = 0; i < n->tmpl.count; i++)
            if (name_assigned_in(n->tmpl.exprs[i], name)) return true;
        return false;
    case NODE_BINOP: case NODE_CHAN_SEND:
        return name_assigned_in(n->binop.left, name) || name_assigned_in(n->binop.right, name);
    case NODE_UNOP: case NODE_CHAN_RECV: return name_assigned_in(n->unop.expr, name);
    case NODE_INDEX:  return name_assigned_in(n->index.obj, name) || name_assigned_in(n->index.key, name);
    case NODE_FIELD:  return name_assigned_in(n->field.obj, name);
    case NODE_PROPAGATE: return name_assigned_in(n->propagate, name);
    case NODE_GO:     return name_assigned_in(n->go_call, name);
    case NODE_DEFER:  return name_assigned_in(n->defer_stmt, name);
    case NODE_EXPR_STMT: return name_assigned_in(n->expr_stmt, name);
    default: return false;
    }
}

/* Does every path through `block` end in a return? (Only the simple case is
 * recognised: the last statement is a return.) */
static bool block_ends_with_return(SeaNode *block) {
    if (!block) return false;
    if (block->type == NODE_RETURN) return true;
    if (block->type != NODE_BLOCK || block->block.count == 0) return false;
    return block->block.stmts[block->block.count - 1]->type == NODE_RETURN;
}

/* `e` was just an operand of an arithmetic or bitwise op that succeeded:
 * those raise an error on anything but numbers, so a local named here holds a
 * number from now on (until reassigned) — it needs no release. Not for locals
 * a closure may change. */
static void note_numeric_operand(SeaCompiler *c, SeaNode *e) {
    if (!e || e->type != NODE_IDENT || name_is_captured(c, e->ident)) return;
    int r = resolve_local(c, e->ident);
    if (r < 0) return;
    c->reg_meta[r].is_scalar = 1;
    c->reg_meta[resolve_copy_chain(c, r)].is_scalar = 1;
}

static bool binop_is_arith(int op) {
    switch (op) {
    case BINOP_ADD: case BINOP_SUB: case BINOP_MUL: case BINOP_DIV: case BINOP_IDIV:
    case BINOP_MOD: case BINOP_POW: case BINOP_BAND: case BINOP_BOR: case BINOP_BXOR:
    case BINOP_SHL: case BINOP_SHR:
        return true;
    default:
        return false;
    }
}

// ---- Expression compiler ----

// Compile expr and store result in dst register
// If dst < 0, allocate a new register and return it
// Returns the register holding the result
static int compile_expr(SeaCompiler *c, SeaNode *node, int dst) {
    if (!node || c->had_error) return dst < 0 ? alloc_reg(c) : dst;

    int line = node->line;

    // If dst < 0, allocate one
    if (dst < 0) dst = alloc_reg(c);

    /* Calls lay out callee and arguments from their destination upward, so
     * the destination must be the top register; otherwise (e.g. `x = a or
     * f(y)` compiled into x's register) they would overwrite live registers
     * above it. Compile into a fresh top register and move. */
    if ((node->type == NODE_CALL || node->type == NODE_METHOD_CALL) && dst + 1 != c->reg_top) {
        int saved = c->reg_top;
        int t = alloc_reg(c);
        compile_expr(c, node, t);
        emit_abc(c, OP_MOVE, dst, t, 0, line);
        c->reg_meta[dst] = c->reg_meta[t];
        c->reg_meta[dst].dwe_write_pos = -1;
        c->reg_meta[t].is_owned = 0;
        free_regs_to(c, saved);
        return dst;
    }
    c->reg_meta[dst].is_owned = 0;   /* each case below sets the result's class */

    switch (node->type) {
        case NODE_NIL:
            emit_abc(c, OP_LOAD_NIL, dst, 1, 0, line);
            reg_set_scalar(c, dst);
            break;

        case NODE_BOOL:
            emit_abx(c, node->boolean ? OP_LOAD_TRUE : OP_LOAD_FALSE, dst, 0, line);
            reg_set_scalar(c, dst);
            break;

        case NODE_NUMBER: {
            double n = node->number;
            int64_t iv = (int64_t)n;
            bool is_exact_int = ((double)iv == n) && !node->num_is_float;
            if (is_exact_int && iv >= -32767 && iv <= 32767) {
                emit_asbx(c, OP_LOAD_INT, dst, (int)iv, line);
                reg_set_const_int(c, dst, iv);
            } else if (is_exact_int) {
                SeaValue v = sea_int_val(iv);
                int kidx = add_const(c, v);
                emit_abx(c, OP_LOAD_K, dst, kidx, line);
                reg_set_const_int(c, dst, iv);
            } else {
                SeaValue v = sea_number_val(n);
                int kidx = add_const(c, v);
                emit_abx(c, OP_LOAD_K, dst, kidx, line);
                reg_set_scalar(c, dst);
            }
            break;
        }

        case NODE_STRING: {
            /* a pinned constant: no refcounting (sea_chunk_add_const_str) */
            int kidx = sea_chunk_add_const_strn(c->chunk, node->string.ptr, node->string.len);
            emit_abx(c, OP_LOAD_K, dst, kidx, line);
            reg_set_scalar(c, dst);
            break;
        }

        case NODE_IDENT: {
            const char *name = node->ident;
            int local = resolve_local(c, name);
            if (local >= 0) {
                /* Follow copy chain: use ultimate source register directly. */
                int src = resolve_copy_chain(c, local);
                if (src != dst) {
                    emit_abc(c, OP_MOVE, dst, src, 0, line);
                    reg_copy_int_meta(c, dst, src);
                }
                /* else: dst == src (or same after chain), no MOVE needed */
            } else if (c->self_ok && strcmp(name, c->self_name) == 0) {
                emit_abc(c, OP_LOAD_SELF, dst, 0, 0, line);   /* see SeaCompiler.self_name */
                reg_clear_int(c, dst);
            } else {
                int upval = resolve_upvalue(c, name);
                if (upval >= 0) {
                    emit_abc(c, OP_LOAD_UP, dst, upval, 0, line);
                } else {
                    int kidx = add_const_str(c, name);
                    emit_abx(c, OP_LOAD_GLOBAL, dst, kidx, line);
                }
                reg_clear_int(c, dst);
            }
            break;
        }

        case NODE_VARARG:
            /* OP_VARARG A=dst, B=1 (load one vararg into dst), C=0 */
            emit_abc(c, OP_VARARG, dst, 1, 0, line);
            reg_clear_int(c, dst);
            break;

        case NODE_UNOP: {
            int operand = compile_expr(c, node->unop.expr, -1);
            switch (node->unop.op) {
                case UNOP_NEG:
                    emit_abc(c, OP_NEG, dst, operand, 0, line);
                    if (c->reg_meta[operand].is_const)
                        reg_set_const_int(c, dst, -c->reg_meta[operand].const_int);
                    else
                        reg_copy_int_meta(c, dst, operand);
                    break;
                case UNOP_NOT:
                    emit_abc(c, OP_NOT, dst, operand, 0, line);
                    reg_set_scalar(c, dst);
                    break;
                case UNOP_BNOT:
                    emit_abc(c, OP_BNOT, dst, operand, 0, line);
                    if (c->reg_meta[operand].is_const)
                        reg_set_const_int(c, dst, ~c->reg_meta[operand].const_int);
                    else
                        reg_copy_int_meta(c, dst, operand);
                    break;
                case UNOP_CHAN_RECV:
                    emit_abc(c, OP_CHAN_RECV, dst, operand, 0, line);
                    reg_clear_int(c, dst);
                    c->reg_meta[dst].is_owned = 1;    /* the channel's reference */
                    break;
            }
            if (node->unop.op == UNOP_NEG || node->unop.op == UNOP_BNOT) {
                c->reg_meta[dst].is_scalar = 1;
                c->reg_meta[operand].is_scalar = 1;     /* a number, or it raised */
                note_numeric_operand(c, node->unop.expr);
            }
            drop_temp(c, operand, line);
            free_reg(c, operand);
            break;
        }

        case NODE_BINOP: {
            if (node->binop.op == BINOP_AND) {
                /* Short-circuit: evaluate left, if falsy skip right. A falsy
                 * left (nil/false) needs no RC, so the result has the right
                 * operand's class; a truthy owned left is dropped first. */
                int left = compile_expr(c, node->binop.left, dst);
                int jmp = emit_jmp(c, OP_JMP_FALSE, left, line);
                drop_temp(c, dst, line);
                int right = compile_expr(c, node->binop.right, dst);
                patch_jmp(c, jmp);
                (void)right;
                bool sc = reg_scalar(c, dst), own = c->reg_meta[dst].is_owned;
                reg_clear_int(c, dst);  /* result can be any type */
                c->reg_meta[dst].is_scalar = sc;
                c->reg_meta[dst].is_owned = own;
                break;
            }
            if (node->binop.op == BINOP_OR) {
                /* Short-circuit: evaluate left, if truthy skip right. Both
                 * paths deliver an owned value (retaining a falsy left is a
                 * no-op, so it happens before the jump). */
                int left = compile_expr(c, node->binop.left, dst);
                bool lsc = reg_scalar(c, dst);
                own_temp(c, dst, line);
                int jmp = emit_jmp(c, OP_JMP_TRUE, left, line);
                c->reg_meta[dst].is_owned = 0;   /* falsy left: nothing to drop */
                int right = compile_expr(c, node->binop.right, dst);
                bool rsc = reg_scalar(c, dst);
                own_temp(c, dst, line);
                patch_jmp(c, jmp);
                (void)right;
                reg_clear_int(c, dst);  /* result can be any type */
                c->reg_meta[dst].is_scalar = lsc && rsc;
                c->reg_meta[dst].is_owned = !(lsc && rsc);
                break;
            }
            if (node->binop.op == BINOP_CHAN_SEND) {
                // Channel send expression (the channel retains what it keeps)
                int ch = compile_expr(c, node->binop.left, -1);
                int val = compile_expr(c, node->binop.right, -1);
                emit_abc(c, OP_CHAN_SEND, ch, val, 0, line);
                drop_temp(c, val, line);
                drop_temp(c, ch, line);
                free_reg(c, val);
                free_reg(c, ch);
                emit_abc(c, OP_LOAD_NIL, dst, 1, 0, line);
                reg_set_scalar(c, dst);
                break;
            }
            if (node->binop.op == BINOP_CONCAT) {
                /* a .. b .. c ...: the whole chain at once (it nests to the
                 * right, and a register per level ran out at ~125 operands).
                 * Operands borrowed, in consecutive registers, CONCAT_GROUP at a
                 * time; with more, each group's string goes to a register of
                 * its own and a last CONCAT joins them. A new owned string (or
                 * rope) lands in dst */
                enum { CONCAT_GROUP = 64 };
                SeaNode **ops = NULL;
                int nops = 0, cap = 0;
                SeaNode *stack[CONCAT_GROUP * CONCAT_GROUP];
                int sp = 0;
                stack[sp++] = node;
                while (sp > 0) {
                    SeaNode *n = stack[--sp];
                    if (n->type == NODE_BINOP && n->binop.op == BINOP_CONCAT && sp + 2 <= CONCAT_GROUP * CONCAT_GROUP) {
                        stack[sp++] = n->binop.right;   /* left first out */
                        stack[sp++] = n->binop.left;
                    } else {
                        SEA_DYNARRAY_PUSH(ops, nops, cap, SeaNode *, n);
                    }
                }
                if (nops > CONCAT_GROUP * CONCAT_GROUP) {
                    compiler_error(c, line, "concatenation of more than 4096 parts");
                    SEA_FREE_N(ops, SeaNode *, cap);
                    break;
                }
                int ngroups = (nops + CONCAT_GROUP - 1) / CONCAT_GROUP;
                int groups[CONCAT_GROUP];
                for (int g = 0; g < ngroups; g++) {
                    int first = g * CONCAT_GROUP;
                    int count = nops - first < CONCAT_GROUP ? nops - first : CONCAT_GROUP;
                    int target = ngroups == 1 ? dst : (groups[g] = alloc_reg(c));
                    int regs[CONCAT_GROUP];
                    for (int i = 0; i < count; i++) {
                        regs[i] = alloc_reg(c);
                        compile_expr(c, ops[first + i], regs[i]);
                    }
                    emit_abc(c, OP_CONCAT, target, regs[0], count, line);
                    for (int i = count - 1; i >= 0; i--) {
                        drop_temp(c, regs[i], line);
                        free_reg(c, regs[i]);
                    }
                    reg_clear_int(c, target);
                    c->reg_meta[target].is_owned = 1;
                }
                if (ngroups > 1) {
                    emit_abc(c, OP_CONCAT, dst, groups[0], ngroups, line);
                    for (int g = ngroups - 1; g >= 0; g--) {
                        drop_temp(c, groups[g], line);
                        free_reg(c, groups[g]);
                    }
                    reg_clear_int(c, dst);
                    c->reg_meta[dst].is_owned = 1;
                }
                SEA_FREE_N(ops, SeaNode *, cap);
                break;
            }

            /* [Etapa 2] Constant folding: both operands are compile-time-known integers.
             * Only fires when both sides are locals with is_const=1 (no bytecode emitted
             * for operands). Handles arithmetic and bitwise ops. */
            {
                int ll = expr_local_reg(c, node->binop.left);
                int rl = expr_local_reg(c, node->binop.right);
                if (ll >= 0 && rl >= 0 &&
                    c->reg_meta[ll].is_const && c->reg_meta[rl].is_const) {
                    int64_t lv = c->reg_meta[ll].const_int;
                    int64_t rv = c->reg_meta[rl].const_int;
                    int64_t result = 0; bool folded = true;
                    switch (node->binop.op) {
                        case BINOP_ADD:  result = lv + rv; break;
                        case BINOP_SUB:  result = lv - rv; break;
                        case BINOP_MUL:  result = lv * rv; break;
                        case BINOP_IDIV: if (!rv) { folded=false; break; }      /* floored, as at run time */
                                         result = lv / rv;
                                         if (lv % rv != 0 && (lv ^ rv) < 0) result--;
                                         break;
                        case BINOP_MOD:  if (!rv) { folded=false; break; }
                                         result = lv % rv;
                                         if (result != 0 && (result ^ rv) < 0) result += rv;
                                         break;
                        case BINOP_BAND: result = lv & rv; break;
                        case BINOP_BOR:  result = lv | rv; break;
                        case BINOP_BXOR: result = lv ^ rv; break;
                        case BINOP_SHL:  result = lv << (rv & 63); break;
                        case BINOP_SHR:  result = (int64_t)((uint64_t)lv >> (rv & 63)); break;
                        default: folded = false; break;
                    }
                    if (folded) {
                        if (result >= -32767 && result <= 32767)
                            emit_asbx(c, OP_LOAD_INT, dst, (int)result, line);
                        else {
                            int kidx = add_const(c, sea_int_val(result));
                            emit_abx(c, OP_LOAD_K, dst, kidx, line);
                        }
                        reg_set_const_int(c, dst, result);
                        break;
                    }
                }
            }

            /* Immediate-operand fast paths: fold small integer literals into the opcode.
             * Only applies to integer literals (not float literals like 1.0 or 4.0). */
            {
                SeaNode *rn = node->binop.right;
                if (rn->type == NODE_NUMBER && !rn->num_is_float) {
                    double rv = rn->number;
                    int64_t imm = (int64_t)rv;
                    if ((double)imm == rv && imm >= -128 && imm <= 127) {
                        SeaOpcode iop = OP_ADD; /* sentinel: no match */
                        switch (node->binop.op) {
                            case BINOP_ADD: iop = OP_ADDI; break;
                            case BINOP_SUB: iop = OP_SUBI; break;
                            case BINOP_LT:  iop = OP_LTI;  break;
                            case BINOP_LE:  iop = OP_LEI;  break;
                            case BINOP_GT:  iop = OP_GTI;  break;
                            case BINOP_GE:  iop = OP_GEI;  break;
                            default: break;
                        }
                        if (iop != OP_ADD) {
                            /* Only compile left if opcode matched to avoid double side-effects */
                            int llocal = expr_local_reg(c, node->binop.left);
                            int lreg = (llocal >= 0) ? llocal : compile_expr(c, node->binop.left, -1);
                            bool is_arith = (node->binop.op == BINOP_ADD || node->binop.op == BINOP_SUB);
                            /* If left is a known constant, fold entirely (LOAD_INT instead of ADDI) */
                            if (is_arith && c->reg_meta[lreg].is_const) {
                                int64_t result = (node->binop.op == BINOP_ADD)
                                                 ? c->reg_meta[lreg].const_int + imm
                                                 : c->reg_meta[lreg].const_int - imm;
                                if (result >= -32767 && result <= 32767)
                                    emit_asbx(c, OP_LOAD_INT, dst, (int)result, line);
                                else {
                                    int kidx = add_const(c, sea_int_val(result));
                                    emit_abx(c, OP_LOAD_K, dst, kidx, line);
                                }
                                reg_set_const_int(c, dst, result);
                                if (llocal < 0) free_reg(c, lreg);
                                break;
                            }
                            uint8_t uimm = (uint8_t)(int8_t)imm;
                            emit_abc(c, iop, dst, lreg, uimm, line);
                            if (is_arith) {
                                c->reg_meta[dst].is_int = c->reg_meta[lreg].is_int;
                                c->reg_meta[dst].is_const = 0;
                            } else {
                                reg_clear_int(c, dst);
                            }
                            c->reg_meta[dst].is_scalar = 1;   /* number or bool */
                            if (is_arith) {
                                if (llocal < 0) c->reg_meta[lreg].is_scalar = 1;
                                note_numeric_operand(c, node->binop.left);
                            }
                            if (llocal < 0) { drop_temp(c, lreg, line); free_reg(c, lreg); }
                            break;
                        }
                    }
                }
                /* ADDI is commutative: also handle left being the literal for ADD */
                if (node->binop.op == BINOP_ADD && rn->type != NODE_NUMBER) {
                    SeaNode *ln = node->binop.left;
                    if (ln->type == NODE_NUMBER && !ln->num_is_float) {
                        double lv = ln->number;
                        int64_t imm = (int64_t)lv;
                        if ((double)imm == lv && imm >= -128 && imm <= 127) {
                            int rlocal = expr_local_reg(c, node->binop.right);
                            int rreg = (rlocal >= 0) ? rlocal : compile_expr(c, node->binop.right, -1);
                            /* If right is a known constant, fold entirely */
                            if (c->reg_meta[rreg].is_const) {
                                int64_t result = c->reg_meta[rreg].const_int + imm;
                                if (result >= -32767 && result <= 32767)
                                    emit_asbx(c, OP_LOAD_INT, dst, (int)result, line);
                                else {
                                    int kidx = add_const(c, sea_int_val(result));
                                    emit_abx(c, OP_LOAD_K, dst, kidx, line);
                                }
                                reg_set_const_int(c, dst, result);
                                if (rlocal < 0) free_reg(c, rreg);
                                break;
                            }
                            emit_abc(c, OP_ADDI, dst, rreg, (uint8_t)(int8_t)imm, line);
                            c->reg_meta[dst].is_int = c->reg_meta[rreg].is_int;
                            c->reg_meta[dst].is_const = 0;
                            c->reg_meta[dst].is_scalar = 1;
                            if (rlocal < 0) c->reg_meta[rreg].is_scalar = 1;
                            note_numeric_operand(c, node->binop.right);
                            if (rlocal < 0) { drop_temp(c, rreg, line); free_reg(c, rreg); }
                            break;
                        }
                    }
                }
            }

            /* Borrow local registers directly instead of copying to fresh temps.
             * `left_local` / `right_local` >= 0 means the operand is already in
             * a known local register — no MOVE needed.  We skip free_reg() for
             * borrowed registers because we don't own them. */
            int left_local  = expr_local_reg(c, node->binop.left);
            int right_local = expr_local_reg(c, node->binop.right);
            int left  = (left_local  >= 0) ? left_local
                                           : compile_expr(c, node->binop.left,  -1);
            int right = (right_local >= 0) ? right_local
                                           : compile_expr(c, node->binop.right, -1);

            bool both_int = c->reg_meta[left].is_int && c->reg_meta[right].is_int;
            SeaOpcode op;
            bool result_is_int = false;
            switch (node->binop.op) {
                case BINOP_ADD:
                    op = both_int ? OP_IIADD : OP_ADD;
                    result_is_int = both_int; break;
                case BINOP_SUB:
                    op = both_int ? OP_IISUB : OP_SUB;
                    result_is_int = both_int; break;
                case BINOP_MUL:
                    op = both_int ? OP_MUL_INT  : OP_MUL;
                    result_is_int = both_int; break;
                case BINOP_IDIV:
                    op = both_int ? OP_IDIV_INT : OP_IDIV;
                    result_is_int = both_int; break;
                case BINOP_MOD:
                    op = both_int ? OP_MOD_INT  : OP_MOD;
                    result_is_int = both_int; break;
                case BINOP_DIV:  op = OP_DIV;  break;
                case BINOP_POW:  op = OP_POW;  break;
                case BINOP_EQ:   op = OP_EQ;   break;
                case BINOP_NEQ:  op = OP_NEQ;  break;
                case BINOP_LT:   op = OP_LT;   break;
                case BINOP_LE:   op = OP_LE;   break;
                case BINOP_GT:   op = OP_GT;   break;
                case BINOP_GE:   op = OP_GE;   break;
                case BINOP_BAND: op = OP_BAND; result_is_int = both_int; break;
                case BINOP_BOR:  op = OP_BOR;  result_is_int = both_int; break;
                case BINOP_BXOR: op = OP_BXOR; result_is_int = both_int; break;
                case BINOP_SHL:  op = OP_SHL;  result_is_int = both_int; break;
                case BINOP_SHR:  op = OP_SHR;  result_is_int = both_int; break;
                default: op = OP_ADD; break;
            }
            emit_abc(c, op, dst, left, right, line);
            c->reg_meta[dst].is_int = result_is_int ? 1 : 0;
            c->reg_meta[dst].is_const = 0;
            c->reg_meta[dst].is_scalar = 1;   /* arithmetic/comparison: number or bool */
            if (binop_is_arith(node->binop.op)) {
                /* it succeeded, so both operands were numbers */
                if (left_local < 0)  c->reg_meta[left].is_scalar = 1;
                if (right_local < 0) c->reg_meta[right].is_scalar = 1;
                note_numeric_operand(c, node->binop.left);
                note_numeric_operand(c, node->binop.right);
            }
            if (right_local < 0) { drop_temp(c, right, line); free_reg(c, right); }
            if (left_local  < 0) { drop_temp(c, left, line);  free_reg(c, left); }
            break;
        }

        case NODE_FIELD: {
            int obj_local = expr_local_reg(c, node->field.obj);   /* no MOVE for a local */
            int obj = obj_local >= 0 ? obj_local : compile_expr(c, node->field.obj, -1);
            int kidx = add_const_str(c, node->field.field);
            /* GET_FIELD takes the constant index in C (<= 255); past that,
             * GET_TABLE with the key loaded into a register */
            if (kidx <= 255) {
                emit_abc(c, OP_GET_FIELD, dst, obj, kidx, line);
            } else {
                int key_reg = alloc_reg(c);
                emit_abx(c, OP_LOAD_K, key_reg, kidx, line);
                emit_abc(c, OP_GET_TABLE, dst, obj, key_reg, line);
                free_reg(c, key_reg);
            }
            reg_clear_int(c, dst);
            /* borrowed from obj — unless obj is an owned temporary about to
             * be dropped: then keep the field alive with its own reference */
            if (obj_local < 0 && c->reg_meta[obj].is_owned) {
                own_temp(c, dst, line);
                drop_temp(c, obj, line);
            }
            if (obj_local < 0) free_reg(c, obj);
            break;
        }

        case NODE_INDEX: {
            if (node->index.key == NULL) {
                compiler_error(c, line, "arr[] can only be used as an assignment target");
                if (dst < 0) dst = alloc_reg(c);
                break;
            }
            int obj_local = expr_local_reg(c, node->index.obj);
            int key_local = expr_local_reg(c, node->index.key);
            int obj = (obj_local >= 0) ? obj_local : compile_expr(c, node->index.obj, -1);
            int key = (key_local >= 0) ? key_local : compile_expr(c, node->index.key, -1);
            emit_abc(c, OP_GET_TABLE, dst, obj, key, line);
            reg_clear_int(c, dst);
            if (key_local < 0) drop_temp(c, key, line);
            if (obj_local < 0 && c->reg_meta[obj].is_owned) {   /* see NODE_FIELD */
                own_temp(c, dst, line);
                drop_temp(c, obj, line);
            }
            if (key_local < 0) free_reg(c, key);
            if (obj_local < 0) free_reg(c, obj);
            break;
        }

        case NODE_CALL: {
            // R[fn_reg](args...) -> R[dst..dst+n-1]
            int saved_top = c->reg_top;

            /* Arguments are passed owned (docs/memory.md): each argument
             * register gets +1 (own_temp), which the callee takes over. */

            // Fast path: self-recursive call → OP_CALL_SELF (see self_name)
            if (c->self_ok && node->call.fn->type == NODE_IDENT &&
                strcmp(node->call.fn->ident, c->self_name) == 0 &&
                resolve_local(c, c->self_name) < 0 && node->call.argc <= 254) {
                bool last_is_vararg_cs = (node->call.argc > 0 &&
                    node->call.args[node->call.argc - 1]->type == NODE_VARARG);
                if (!last_is_vararg_cs) {
                    c->reg_top = dst + 1;
                    for (int i = 0; i < node->call.argc; i++) {
                        int arg_reg = alloc_reg(c);
                        compile_expr(c, node->call.args[i], arg_reg);
                        own_temp(c, arg_reg, line);
                    }
                    emit_abc(c, OP_CALL_SELF, dst, 1, node->call.argc, line);
                    free_regs_to(c, saved_top);
                    if (dst >= c->reg_top) c->reg_top = dst + 1;
                    reg_clear_int(c, dst);
                    c->reg_meta[dst].is_owned = 1;
                    break;
                }
            }

            // Put function at dst
            compile_expr(c, node->call.fn, dst);
            c->reg_top = dst + 1;
            /* An owned callee (`f()(x)`, `(function() ... end)()`) must
             * outlive the call, whose result overwrites R[dst]: keep it in
             * dst and call through a copy one register up. */
            int fn_reg = dst;
            bool fn_held = c->reg_meta[dst].is_owned && !reg_scalar(c, dst);
            if (fn_held) {
                fn_reg = alloc_reg(c);
                emit_abc(c, OP_MOVE, fn_reg, dst, 0, line);
            }

            // Check if last arg is vararg (for vararg expansion)
            bool last_is_vararg = (node->call.argc > 0 &&
                node->call.args[node->call.argc - 1]->type == NODE_VARARG);

            // Push args (all except last if last is vararg)
            int fixed_argc = last_is_vararg ? node->call.argc - 1 : node->call.argc;
            for (int i = 0; i < fixed_argc; i++) {
                int arg_reg = alloc_reg(c);
                compile_expr(c, node->call.args[i], arg_reg);
                own_temp(c, arg_reg, line);
            }

            if (last_is_vararg) {
                /* VARARG B=0 expands (and retains) all varargs after the fixed
                 * args; CALL C=255 counts them at run time */
                int va_reg = alloc_reg(c);
                emit_abc(c, OP_VARARG, va_reg, 0, 0, line);
                emit_abc(c, OP_CALL, fn_reg, 1, 255, line);
            } else {
                // CALL R[fn_reg], nret=1, argc
                emit_abc(c, OP_CALL, fn_reg, 1, node->call.argc, line);
            }
            if (fn_held) {
                /* (a multi-value call cannot be patched past these) */
                emit_abc(c, OP_RELEASE, dst, 0, 0, line);
                emit_abc(c, OP_MOVE, dst, fn_reg, 0, line);
            }
            free_regs_to(c, saved_top);
            // dst will hold result
            if (dst >= c->reg_top) c->reg_top = dst + 1;
            reg_clear_int(c, dst);
            c->reg_meta[dst].is_owned = 1;
            // len() is a builtin that always returns an integer
            if (node->call.fn->type == NODE_IDENT &&
                strcmp(node->call.fn->ident, "len") == 0 &&
                node->call.argc == 1 && resolve_local(c, "len") < 0) {
                c->reg_meta[dst].is_int = 1;
                c->reg_meta[dst].is_scalar = 1;
            }
            break;
        }

        case NODE_METHOD_CALL: {
            int saved_top = c->reg_top;

            // Load object
            int obj_reg = dst;
            compile_expr(c, node->method.obj, obj_reg);
            c->reg_top = obj_reg + 1;

            // Load method: field lookup
            int fn_reg = alloc_reg(c);
            int kidx = add_const_str(c, node->method.method);
            if (kidx <= 255) {
                emit_abc(c, OP_GET_FIELD, fn_reg, obj_reg, kidx, line);
            } else {
                int key_reg = alloc_reg(c);
                emit_abx(c, OP_LOAD_K, key_reg, kidx, line);
                emit_abc(c, OP_GET_TABLE, fn_reg, obj_reg, key_reg, line);
                free_reg(c, key_reg);
            }

            // Move fn to call position, push self as first arg
            // We'll use: fn_reg(obj_reg, args...)
            int call_base = fn_reg;
            int self_reg = alloc_reg(c);
            emit_abc(c, OP_MOVE, self_reg, obj_reg, 0, line);
            /* self is an argument (owned): an owned object moves into it,
             * a borrowed one is retained */
            if (c->reg_meta[obj_reg].is_owned) {
                c->reg_meta[obj_reg].is_owned = 0;
                c->reg_meta[self_reg].is_owned = 1;
            } else {
                c->reg_meta[self_reg].is_scalar = c->reg_meta[obj_reg].is_scalar;
                own_temp(c, self_reg, line);
            }

            // Push remaining args
            for (int i = 0; i < node->method.argc; i++) {
                int arg_reg = alloc_reg(c);
                compile_expr(c, node->method.args[i], arg_reg);
                own_temp(c, arg_reg, line);
            }

            // CALL fn_reg(self, args...)
            emit_abc(c, OP_CALL, call_base, 1, node->method.argc + 1, line);

            // Result is in fn_reg, move to dst if different
            if (call_base != dst) {
                emit_abc(c, OP_MOVE, dst, call_base, 0, line);
            }
            free_regs_to(c, saved_top);
            if (dst >= c->reg_top) c->reg_top = dst + 1;
            reg_clear_int(c, dst);
            c->reg_meta[dst].is_owned = 1;
            break;
        }

        case NODE_FUNC: {
            // Compile nested function into a proto
            SeaCompiler child;
            memset(&child, 0, sizeof(child));
            child.parent = c;
            child.chunk = sea_chunk_new(node->func.name);
            child.chunk->line_defined = node->line;
            child.chunk->arity = (uint8_t)node->func.paramc;

            // Add parameters as locals
            for (int i = 0; i < node->func.paramc; i++) {
                if (node->func.params[i].is_vararg) {
                    child.chunk->is_vararg = true;
                    child.chunk->arity--; // vararg not counted
                } else {
                    add_local(&child, node->func.params[i].name, line);
                }
            }

            /* `local function name`: the body refers to itself without
             * capturing the local (see SeaCompiler.self_name) — unless the
             * enclosing function ever reassigns the name. */
            child.loop_body_local = -1;
            child.body = node->func.body;
            if (c->pending_self) {
                child.self_name = c->pending_self;
                child.self_ok = !name_assigned_in(c->body, c->pending_self);
                c->pending_self = NULL;
            }

            // Pre-scan for non-escaping struct literals (arena allocation)
            sea_scan_escapes(node->func.body, &child.escape_info);
            sea_collect_captured(node->func.body, &child.captured);

            // Compile body
            compile_block(&child, node->func.body);
            sea_free_captured(&child.captured);

            /* Falling off the end returns nothing: release the locals
             * (parameters included) first. */
            if (!block_ends_with_return(node->func.body)) {
                emit_locals_cleanup(&child, 0, false, -1, line);
                emit(&child, SEA_MAKE_ABC(OP_RETURN0, 0, 0, 0), line);
            }

            // Free local names
            for (int i = 0; i < child.local_count; i++) {
                if (child.locals[i].name) {
                    sea_free(child.locals[i].name, strlen(child.locals[i].name)+1);
                }
            }
            for (int i = 0; i < child.upval_count; i++) {
                if (child.upvalues[i].name) {
                    sea_free(child.upvalues[i].name, strlen(child.upvalues[i].name)+1);
                }
            }
            if (child.break_list)    sea_free(child.break_list, sizeof(int) * (size_t)child.break_cap);
            if (child.continue_list) sea_free(child.continue_list, sizeof(int) * (size_t)child.continue_cap);

            if (child.had_error) {
                c->had_error = true;
                strncpy(c->err_msg, child.err_msg, sizeof(c->err_msg)-1);
                sea_chunk_free(child.chunk);
                break;
            }

            // Add proto to parent chunk
            int proto_idx = c->chunk->proto_len;
            if (c->chunk->proto_len >= c->chunk->proto_cap) {
                int old = c->chunk->proto_cap;
                c->chunk->proto_cap = old < 4 ? 4 : old * 2;
                c->chunk->protos = SEA_REALLOC_N(c->chunk->protos, SeaChunk*, old, c->chunk->proto_cap);
            }
            c->chunk->protos[c->chunk->proto_len++] = child.chunk;

            // Emit CLOSURE instruction
            emit_abx(c, OP_CLOSURE, dst, proto_idx, line);
            reg_clear_int(c, dst);
            c->reg_meta[dst].is_owned = 1;
            /* Any captured locals may be mutated by the closure: invalidate their
             * const metadata so subsequent uses don't fold stale values. */
            for (int i = 0; i < c->local_count; i++) {
                if (c->locals[i].captured)
                    c->reg_meta[c->locals[i].reg].is_const = 0;
            }
            break;
        }

        case NODE_ARRAY_LIT: {
            if (dst < 0) dst = alloc_reg(c);
            if (node->array_lit.count == 0) {
                /* []: if escape analysis proved this local array never escapes
                 * the frame, arena-allocate its header (saving one malloc); the
                 * heap data buffer is freed by the arena-aware OP_RELEASE at scope
                 * exit. Otherwise use the sentinel B==A form. */
                if (esc_is_arena_candidate(&c->escape_info, node)) {
                    emit_abc(c, OP_NEW_ARRAY_LOCAL, dst, 8, 0, line);  /* B = initial cap hint */
                } else {
                    emit_abc(c, OP_NEW_ARRAY, dst, dst, 0, line);
                }
            } else {
                /* [e1, e2, ...]: a new array with room for them, then one push each
                 * (a call yields one value here, as in any expression list) */
                int n = node->array_lit.count;
                int cap_reg = alloc_reg(c);
                emit_asbx(c, OP_LOAD_INT, cap_reg, n < 30000 ? n : 30000, line);  /* a capacity hint */
                emit_abc(c, OP_NEW_ARRAY, dst, cap_reg, 0, line);
                free_reg(c, cap_reg);
                for (int i = 0; i < n; i++) {
                    if (i == n - 1 && node->array_lit.elems[i]->type == NODE_VARARG) {
                        /* [a, ...]: every vararg, appended (pack mode) */
                        emit_abc(c, OP_VARARG, dst, 0, 1, line);
                        break;
                    }
                    int v = compile_expr(c, node->array_lit.elems[i], -1);
                    if (can_move_from(c, v)) {
                        emit_abc(c, OP_ARRAY_PUSH_MOVE, dst, v, 0, line);
                        c->reg_meta[v].is_owned = 0;
                    } else {
                        emit_abc(c, OP_ARRAY_PUSH, dst, v, 0, line);
                    }
                    drop_temp(c, v, line);
                    if (!reg_is_named_local(c, v)) free_reg(c, v);
                }
            }
            reg_clear_int(c, dst);
            c->reg_meta[dst].is_owned = 1;
            break;
        }

        case NODE_TEMPLATE: {
            /* `a ${x} b`: the pieces in consecutive registers, then one CONCAT
             * (which converts each value as tostring does) */
            if (dst < 0) dst = alloc_reg(c);
            int npieces = 0;
            for (int i = 0; i <= node->tmpl.count; i++) {
                if (node->tmpl.lens[i] > 0) npieces++;
                if (i < node->tmpl.count) npieces++;
            }
            bool lead_empty = npieces == 1 && node->tmpl.count == 1;  /* `${x}`: "" .. x */
            if (npieces == 0 || (npieces == 1 && node->tmpl.count == 0)) {
                int kidx = sea_chunk_add_const_strn(c->chunk, node->tmpl.texts[0], node->tmpl.lens[0]);
                emit_abx(c, OP_LOAD_K, dst, kidx, line);
                reg_set_scalar(c, dst);
                break;
            }
            if (npieces + (lead_empty ? 1 : 0) > 250) {
                compiler_error(c, line, "template string with more than 250 parts");
                break;
            }
            int regs[256];
            int nr = 0;
            if (lead_empty) {
                regs[nr] = alloc_reg(c);
                emit_abx(c, OP_LOAD_K, regs[nr], sea_chunk_add_const_strn(c->chunk, "", 0), line);
                reg_set_scalar(c, regs[nr]);
                nr++;
            }
            for (int i = 0; i <= node->tmpl.count; i++) {
                if (node->tmpl.lens[i] > 0) {
                    regs[nr] = alloc_reg(c);
                    int kidx = sea_chunk_add_const_strn(c->chunk, node->tmpl.texts[i], node->tmpl.lens[i]);
                    emit_abx(c, OP_LOAD_K, regs[nr], kidx, line);
                    reg_set_scalar(c, regs[nr]);
                    nr++;
                }
                if (i < node->tmpl.count) {
                    regs[nr] = alloc_reg(c);
                    compile_expr(c, node->tmpl.exprs[i], regs[nr]);
                    nr++;
                }
            }
            emit_abc(c, OP_CONCAT, dst, regs[0], nr, line);
            for (int i = nr - 1; i >= 0; i--) {
                drop_temp(c, regs[i], line);
                free_reg(c, regs[i]);
            }
            reg_clear_int(c, dst);
            c->reg_meta[dst].is_owned = 1;
            break;
        }

        case NODE_MAP_LIT: {
            if (dst < 0) dst = alloc_reg(c);
            emit_abc(c, OP_NEW_MAP, dst, 0, 0, line);
            reg_clear_int(c, dst);
            c->reg_meta[dst].is_owned = 1;
            break;
        }

        case NODE_TABLE: {
            // Table literal: {key=val, ...}
            // Check if it's all positional (array-like) or mixed
            bool all_positional = true;
            for (int i = 0; i < node->table.count; i++) {
                if (node->table.keys && node->table.keys[i]) {
                    all_positional = false;
                    break;
                }
            }

            /* Special case: {...} - creates table/array from all varargs */
            if (all_positional && node->table.count == 1 &&
                node->table.vals[0]->type == NODE_VARARG) {
                /* Emit: NEW_TABLE dst; VARARG dst 0 1 (pack mode: insert all into R[dst]) */
                emit_abc(c, OP_NEW_TABLE, dst, TABLE_MAP, 0, line);
                emit_abc(c, OP_VARARG, dst, 0, 1, line);  /* C=1 = pack all varargs into table */
                reg_clear_int(c, dst);
                c->reg_meta[dst].is_owned = 1;
                break;
            }

            if (all_positional) {
                emit_abc(c, OP_NEW_TABLE, dst, TABLE_MAP, 0, line);
                for (int i = 0; i < node->table.count; i++) {
                    int val_reg = alloc_reg(c);
                    compile_expr(c, node->table.vals[i], val_reg);
                    // Key is integer index (1-based for Lua-style tables)
                    int key_reg = alloc_reg(c);
                    emit_asbx(c, OP_LOAD_INT, key_reg, i, line);
                    emit_abc(c, OP_SET_TABLE, dst, key_reg, val_reg, line);
                    free_reg(c, key_reg);
                    /* Container retained val; release our temporary ownership. */
                    drop_temp(c, val_reg, line);
                    free_reg(c, val_reg);
                }
            } else {
                /* Detect struct literal: non-empty, ALL keys are string literals */
                bool all_string_keys = node->table.count > 0 && node->table.keys != NULL;
                if (all_string_keys) {
                    for (int i = 0; i < node->table.count; i++) {
                        if (!node->table.keys[i] || node->table.keys[i]->type != NODE_STRING) {
                            all_string_keys = false;
                            break;
                        }
                    }
                }

                if (all_string_keys) {
                    bool use_arena = esc_is_arena_candidate(&c->escape_info, node);
                    SeaOpcode new_op  = use_arena ? OP_NEW_STRUCT_LOCAL : OP_NEW_STRUCT;
                    SeaOpcode set_op  = use_arena ? OP_SET_FIELD_BORROW  : OP_SET_FIELD;
                    emit_abc(c, new_op, dst, node->table.count, 0, line);
                    for (int i = 0; i < node->table.count; i++) {
                        int val_reg = alloc_reg(c);
                        compile_expr(c, node->table.vals[i], val_reg);
                        SeaNode *key_node = node->table.keys[i];
                        int kidx = sea_chunk_add_const_strn(c->chunk, key_node->string.ptr, key_node->string.len);
                        if (kidx <= 255 && !use_arena && can_move_from(c, val_reg)) {
                            emit_abc(c, OP_SET_FIELD_MOVE, dst, kidx, val_reg, line);
                            c->reg_meta[val_reg].is_owned = 0;   /* consumed */
                        } else if (kidx <= 255) {
                            emit_abc(c, set_op, dst, kidx, val_reg, line);
                        } else {
                            int key_reg = alloc_reg(c);
                            emit_abx(c, OP_LOAD_K, key_reg, kidx, line);
                            emit_abc(c, OP_SET_TABLE, dst, key_reg, val_reg, line);
                            free_reg(c, key_reg);
                        }
                        /* The struct retained the value (arena structs too:
                         * their fields are owned and released with them) */
                        drop_temp(c, val_reg, line);
                        free_reg(c, val_reg);
                    }
                } else {
                    emit_abc(c, OP_NEW_TABLE, dst, TABLE_MAP, 0, line);
                    int auto_idx = 0;
                    for (int i = 0; i < node->table.count; i++) {
                        int val_reg = alloc_reg(c);
                        compile_expr(c, node->table.vals[i], val_reg);

                        if (!node->table.keys || !node->table.keys[i]) {
                            // Positional key
                            int key_reg = alloc_reg(c);
                            emit_asbx(c, OP_LOAD_INT, key_reg, auto_idx++, line);
                            emit_abc(c, OP_SET_TABLE, dst, key_reg, val_reg, line);
                            free_reg(c, key_reg);
                        } else {
                            SeaNode *key_node = node->table.keys[i];
                            if (key_node->type == NODE_STRING) {
                                // String key - use SET_FIELD
                                int kidx = sea_chunk_add_const_strn(c->chunk, key_node->string.ptr, key_node->string.len);
                                if (kidx <= 255) {
                                    emit_abc(c, OP_SET_FIELD, dst, kidx, val_reg, line);
                                } else {
                                    int key_reg = alloc_reg(c);
                                    emit_abx(c, OP_LOAD_K, key_reg, kidx, line);
                                    emit_abc(c, OP_SET_TABLE, dst, key_reg, val_reg, line);
                                    free_reg(c, key_reg);
                                }
                            } else {
                                int key_reg = compile_expr(c, key_node, -1);
                                emit_abc(c, OP_SET_TABLE, dst, key_reg, val_reg, line);
                                drop_temp(c, key_reg, line);
                                free_reg(c, key_reg);
                            }
                        }
                        /* Container retained val; release our temporary ownership. */
                        drop_temp(c, val_reg, line);
                        free_reg(c, val_reg);
                    }
                }
            }
            reg_clear_int(c, dst);
            c->reg_meta[dst].is_owned = 1;
            break;
        }

        case NODE_CHAN_RECV: {
            int ch = compile_expr(c, node->unop.expr, -1);
            emit_abc(c, OP_CHAN_RECV, dst, ch, 0, line);
            drop_temp(c, ch, line);
            free_reg(c, ch);
            reg_clear_int(c, dst);
            c->reg_meta[dst].is_owned = 1;    /* the channel's reference */
            break;
        }

        case NODE_CHAN_SEND: {
            int ch = compile_expr(c, node->binop.left, -1);
            int val = compile_expr(c, node->binop.right, -1);
            emit_abc(c, OP_CHAN_SEND, ch, val, 0, line);
            drop_temp(c, val, line);
            drop_temp(c, ch, line);
            free_reg(c, val);
            free_reg(c, ch);
            emit_abc(c, OP_LOAD_NIL, dst, 1, 0, line);
            reg_set_scalar(c, dst);
            break;
        }

        case NODE_PROPAGATE: {
            /* e?: when e is an error, return it from the function — first
             * releasing the locals and the owned temporaries still live (e.g.
             * arguments already evaluated), and making the error owned (it
             * is the return value). The main chunk just reports it. */
            int inner = compile_expr(c, node->propagate, dst);
            if (!c->parent) {
                emit_abc(c, OP_PROPAGATE, inner, 0, 0, line);
                break;
            }
            SeaRegMeta saved = c->reg_meta[inner];
            int skip = emit_jmp(c, OP_JMP_NOT_ERR, inner, line);
            if (!c->reg_meta[inner].is_owned)
                emit(c, SEA_MAKE_ABC(OP_RETAIN, inner, 0, 0), line);
            emit_temps_cleanup(c, inner, line);
            emit_locals_cleanup(c, 0, false, -1, line);
            emit_abc(c, OP_PROPAGATE, inner, 0, 0, line);
            patch_jmp(c, skip);
            c->reg_meta[inner] = saved;   /* the fall-through path is unchanged */
            break;
        }

        default:
            compiler_error(c, line, "cannot compile expression type %d", node->type);
            break;
    }

    return dst;
}

/* Store val_reg into an owning local `local` (its old value is released).
 * val_reg is a temporary, owned or borrowed, or a value needing no RC. */
static void store_local(SeaCompiler *c, int local, int val_reg, int line) {
    bool vsc = reg_scalar(c, val_reg), lsc = reg_scalar(c, local);
    if (local != val_reg) {
        if (vsc && lsc)
            emit_abc(c, OP_MOVE, local, val_reg, 0, line);   /* nothing to release */
        else
            emit_abc(c, OP_SETLOCAL, local, val_reg,
                     (!c->reg_meta[val_reg].is_owned && !vsc) ? 1 : 0, line);
    }
    reg_copy_int_meta(c, local, val_reg);
    c->reg_meta[local].is_owned = 0;
    if (!reg_is_named_local(c, val_reg)) c->reg_meta[val_reg].is_owned = 0;   /* consumed */
}

// Compile an assignment target (store val_reg into target). val_reg is
// consumed: an owned temporary moves into a local or is released after a
// store into a container (which retains).
static void compile_store(SeaCompiler *c, SeaNode *target, int val_reg) {
    int line = target->line;

    if (target->type == NODE_IDENT) {
        const char *name = target->ident;
        int local = resolve_local(c, name);
        if (local >= 0) {
            SeaLocal *loc = find_local(c, name);
            if (loc && loc->is_const) {
                compiler_error(c, line, "cannot assign to const '%s'", name);
                return;
            }
            materialize_copies_of(c, local, line);
            if (c->reg_meta[local].has_copy) {
                /* a scalar alias: the register holds nothing to release */
                c->reg_meta[local].has_copy = 0;
                c->reg_meta[local].is_scalar = 1;
            }
            c->reg_meta[local].is_const = 0;
            store_local(c, local, val_reg, line);
            mark_if_captured(c, name, local, line);
        } else {
            int upval = resolve_upvalue(c, name);
            if (upval >= 0) {
                if (c->upvalues[upval].is_const) {
                    compiler_error(c, line, "cannot assign to const '%s'", name);
                    return;
                }
                emit_abc(c, OP_STORE_UP, val_reg, upval, 0, line);   /* retains */
            } else {
                int kidx = add_const_str(c, name);
                emit_abx(c, OP_STORE_GLOBAL, val_reg, kidx, line);  /* retains */
            }
            drop_temp(c, val_reg, line);
        }
    } else if (target->type == NODE_FIELD) {
        int obj_local = expr_local_reg(c, target->field.obj);
        int obj = obj_local >= 0 ? obj_local : compile_expr(c, target->field.obj, -1);
        int kidx = add_const_str(c, target->field.field);
        if (kidx <= 255 && can_move_from(c, val_reg)) {
            emit_abc(c, OP_SET_FIELD_MOVE, obj, kidx, val_reg, line);
            c->reg_meta[val_reg].is_owned = 0;   /* consumed */
        } else if (kidx <= 255) {
            emit_abc(c, OP_SET_FIELD, obj, kidx, val_reg, line);
        } else {
            int key_reg = alloc_reg(c);
            emit_abx(c, OP_LOAD_K, key_reg, kidx, line);
            emit_abc(c, OP_SET_TABLE, obj, key_reg, val_reg, line);
            free_reg(c, key_reg);
        }
        drop_temp(c, val_reg, line);
        if (obj_local < 0) { drop_temp(c, obj, line); free_reg(c, obj); }
    } else if (target->type == NODE_INDEX) {
        int obj_local = expr_local_reg(c, target->index.obj);
        int obj = (obj_local >= 0) ? obj_local : compile_expr(c, target->index.obj, -1);
        if (target->index.key == NULL) {
            // arr[] = val — append
            if (can_move_from(c, val_reg)) {
                emit_abc(c, OP_ARRAY_PUSH_MOVE, obj, val_reg, 0, line);
                c->reg_meta[val_reg].is_owned = 0;   /* consumed */
            } else {
                emit_abc(c, OP_ARRAY_PUSH, obj, val_reg, 0, line);
            }
        } else {
            int key_local = expr_local_reg(c, target->index.key);
            int key = (key_local >= 0) ? key_local : compile_expr(c, target->index.key, -1);
            emit_abc(c, OP_SET_TABLE, obj, key, val_reg, line);
            if (key_local < 0) { drop_temp(c, key, line); free_reg(c, key); }
        }
        drop_temp(c, val_reg, line);
        if (obj_local < 0) { drop_temp(c, obj, line); free_reg(c, obj); }
    } else {
        compiler_error(c, line, "invalid assignment target");
    }
}

/* Can `e` be compiled straight into an owning local whose old value needs no
 * release? Only for results that need no RC either (arithmetic, comparisons,
 * literals...) — the result simply overwrites the register. */
static bool expr_is_scalar(SeaCompiler *c, SeaNode *e) {
    LoopAssigns none;
    none.n = 0; none.overflow = false;
    return loop_scalar_expr(c, e, &none, NULL);
}

/* Condition test that leaves no reference behind: an owned condition value
 * (e.g. a call result) is reduced to a bool and released first. Returns the
 * position of a jump taken when the condition is false. */
static int emit_cond_jump_false(SeaCompiler *c, int cond_reg, int line) {
    if (!c->reg_meta[cond_reg].is_owned || reg_scalar(c, cond_reg))
        return emit_jmp(c, OP_JMP_FALSE, cond_reg, line);
    int t = alloc_reg(c);
    emit_abc(c, OP_NOT, t, cond_reg, 0, line);
    drop_temp(c, cond_reg, line);
    int j = emit_jmp(c, OP_JMP_TRUE, t, line);
    free_reg(c, t);
    return j;
}

// ---- Statement compiler ----

bool sea_cover_enabled = false;

/* Coverage: mark the statement's line in the chunk's table and emit an
 * OP_COVER that records it ran (once per line in a row). */
static void cover_stmt(SeaCompiler *c, int line) {
    SeaChunk *ch = c->chunk;
    if (line <= 0 || line > 0xFFFF) return;
    if (ch->code_len > 0 && SEA_INSTR_OP(ch->code[ch->code_len - 1]) == OP_COVER &&
        (int)SEA_INSTR_Bx(ch->code[ch->code_len - 1]) == line) return;
    if (line >= ch->cover_len) {
        int n = ch->cover_len ? ch->cover_len : 64;
        while (n <= line) n *= 2;
        ch->cover = realloc(ch->cover, (size_t)n);
        memset(ch->cover + ch->cover_len, 0, (size_t)(n - ch->cover_len));
        ch->cover_len = n;
    }
    if (ch->cover[line] == 0) ch->cover[line] = 1;
    emit_abx(c, OP_COVER, 0, line, line);
}

static void compile_stmt(SeaCompiler *c, SeaNode *node) {
    if (!node || c->had_error) return;
    int line = node->line;
    if (sea_cover_enabled) cover_stmt(c, line);

    switch (node->type) {
        case NODE_EXPR_STMT: {
            // Call or channel send - discard result
            SeaNode *expr = node->expr_stmt;
            if (expr->type == NODE_CALL || expr->type == NODE_METHOD_CALL) {
                int saved_top = c->reg_top;
                int fn_reg = c->reg_top;

                if (expr->type == NODE_CALL) {
                    compile_expr(c, expr->call.fn, fn_reg);
                    c->reg_top = fn_reg + 1;
                    /* Check for trailing vararg expansion */
                    bool last_va = (expr->call.argc > 0 &&
                        expr->call.args[expr->call.argc-1]->type == NODE_VARARG);
                    int fixed = last_va ? expr->call.argc - 1 : expr->call.argc;
                    int fn_reg2 = fn_reg;
                    bool fn_held = c->reg_meta[fn_reg].is_owned && !reg_scalar(c, fn_reg);
                    if (fn_held) {   /* see NODE_CALL */
                        fn_reg2 = alloc_reg(c);
                        emit_abc(c, OP_MOVE, fn_reg2, fn_reg, 0, line);
                    }
                    for (int i = 0; i < fixed; i++) {
                        int arg = alloc_reg(c);
                        compile_expr(c, expr->call.args[i], arg);
                        own_temp(c, arg, line);
                    }
                    if (fn_held) {
                        if (last_va) {
                            int va_reg = alloc_reg(c);
                            emit_abc(c, OP_VARARG, va_reg, 0, 0, line);
                            emit_abc(c, OP_CALL, fn_reg2, 0, 255, line);
                        } else {
                            emit_abc(c, OP_CALL, fn_reg2, 0, expr->call.argc, line);
                        }
                        emit_abc(c, OP_RELEASE, fn_reg, 0, 0, line);
                    } else if (last_va) {
                        int va_reg = alloc_reg(c);
                        emit_abc(c, OP_VARARG, va_reg, 0, 0, line);
                        emit_abc(c, OP_CALL, fn_reg, 0, 255, line);
                    } else {
                        emit_abc(c, OP_CALL, fn_reg, 0, expr->call.argc, line);
                    }
                } else {
                    // Method call
                    int obj_reg = fn_reg;
                    compile_expr(c, expr->method.obj, obj_reg);
                    c->reg_top = obj_reg + 1;
                    int mfn_reg = alloc_reg(c);
                    int kidx = add_const_str(c, expr->method.method);
                    if (kidx <= 255) {
                        emit_abc(c, OP_GET_FIELD, mfn_reg, obj_reg, kidx, line);
                    } else {
                        int key_reg = alloc_reg(c);
                        emit_abx(c, OP_LOAD_K, key_reg, kidx, line);
                        emit_abc(c, OP_GET_TABLE, mfn_reg, obj_reg, key_reg, line);
                        free_reg(c, key_reg);
                    }
                    int self_reg = alloc_reg(c);
                    emit_abc(c, OP_MOVE, self_reg, obj_reg, 0, line);
                    if (c->reg_meta[obj_reg].is_owned) {     /* see NODE_METHOD_CALL */
                        c->reg_meta[obj_reg].is_owned = 0;
                        c->reg_meta[self_reg].is_owned = 1;
                    } else {
                        c->reg_meta[self_reg].is_scalar = c->reg_meta[obj_reg].is_scalar;
                        own_temp(c, self_reg, line);
                    }
                    for (int i = 0; i < expr->method.argc; i++) {
                        int arg = alloc_reg(c);
                        compile_expr(c, expr->method.args[i], arg);
                        own_temp(c, arg, line);
                    }
                    emit_abc(c, OP_CALL, mfn_reg, 0, expr->method.argc + 1, line);
                }

                free_regs_to(c, saved_top);
            } else if (expr->type == NODE_CHAN_SEND ||
                       (expr->type == NODE_BINOP && expr->binop.op == BINOP_CHAN_SEND)) {
                int ch = compile_expr(c, expr->binop.left, -1);
                int val = compile_expr(c, expr->binop.right, -1);
                emit_abc(c, OP_CHAN_SEND, ch, val, 0, line);
                drop_temp(c, val, line);
                drop_temp(c, ch, line);
                free_reg(c, val);
                free_reg(c, ch);
            } else {
                int tmp = compile_expr(c, expr, -1);
                drop_temp(c, tmp, line);
                free_reg(c, tmp);
            }
            break;
        }

        case NODE_LOCAL: {
            /* Special case: `local function name(...)` – pre-declare the name
             * as a local so the function body can capture itself as an upvalue. */
            bool self_ref_func = (node->local.count == 1 &&
                                  node->local.expr_count == 1 &&
                                  node->local.exprs[0]->type == NODE_FUNC);

            int first_reg = c->reg_top;

            if (self_ref_func) {
                /* Pre-declare: allocate the register and register the name first */
                int reg = alloc_reg(c);
                (void)reg; /* reg == first_reg */
                if (c->local_count >= SEA_MAX_REGISTERS) {
                    compiler_error(c, line, "too many local variables");
                    break;
                }
                const char *nm = node->local.names[0];
                SeaLocal *loc = &c->locals[c->local_count++];
                loc->name = (char*)sea_malloc(strlen(nm)+1);
                strcpy(loc->name, nm);
                loc->depth = c->scope_depth;
                loc->captured = false;
                loc->reg = first_reg;
                loc->is_const = false;
                c->reg_top = first_reg + 1;
                /* Now compile the function body – it refers to itself through
                 * OP_LOAD_SELF/CALL_SELF (or captures `name` if reassigned) */
                c->pending_self = nm;
                compile_expr(c, node->local.exprs[0], first_reg);
                c->pending_self = NULL;
                c->reg_meta[first_reg].is_owned = 0;   /* the local owns the closure */
                /* Done; the register is already named */
                break;
            }

            // Special case: local v, ok = m[k] or m.k  (Go-style two-value lookup)
            if (node->local.count == 2 && node->local.expr_count == 1 &&
                is_two_value_lookup(node->local.exprs[0])) {
                int r0 = alloc_reg(c);  // value
                int r1 = alloc_reg(c);  // ok
                (void)r1;  // r1 == r0+1 by construction
                int obj_reg, key_reg;
                compile_lookup_operands(c, node->local.exprs[0], &obj_reg, &key_reg, line);
                emit_abc(c, OP_GET_TABLE2, r0, obj_reg, key_reg, line);
                /* v owns its value (borrowed from the container); ok is a bool */
                emit_abc(c, OP_RETAIN, r0, 0, 0, line);
                reg_clear_int(c, r0);
                reg_set_scalar(c, r1);
                drop_temp(c, key_reg, line);
                drop_temp(c, obj_reg, line);
                free_reg(c, key_reg);
                free_reg(c, obj_reg);
                c->reg_top = first_reg;
                for (int i = 0; i < 2; i++) {
                    int reg = first_reg + i;
                    if (c->local_count >= SEA_MAX_REGISTERS) {
                        compiler_error(c, line, "too many local variables");
                        break;
                    }
                    SeaLocal *loc = &c->locals[c->local_count++];
                    loc->name = (char*)sea_malloc(strlen(node->local.names[i])+1);
                    strcpy(loc->name, node->local.names[i]);
                    loc->depth  = c->scope_depth;
                    loc->captured = false;
                    loc->is_const = false;
                    loc->reg    = reg;
                    c->reg_top  = reg + 1;
                    if (name_is_captured(c, loc->name)) reg_clear_int(c, reg);
                }
                break;
            }

            // General case:
            // For `local a, b, c = e1, e2, e3`:
            // 1. Evaluate each RHS expression into a temp register BEFORE declaring locals
            //    (to allow `local a = a` to reference the outer `a`)
            // 2. Declare the locals (the temps ARE the locals)

            // Step 1: evaluate RHS into fresh registers
            // Special case: if last expr is a call and we need more values than exprs,
            // request the call to return the extra values.
            int need_from_last = node->local.count - (node->local.expr_count - 1);
            bool last_is_call = (node->local.expr_count > 0 &&
                (node->local.exprs[node->local.expr_count-1]->type == NODE_CALL ||
                 node->local.exprs[node->local.expr_count-1]->type == NODE_METHOD_CALL));

            for (int i = 0; i < node->local.count; i++) {
                int reg = alloc_reg(c);
                if (i < node->local.expr_count) {
                    bool is_last_expr = (i == node->local.expr_count - 1);
                    SeaNode *init = node->local.exprs[i];
                    /* [Etapa 3] Copy propagation: local y = x where x is a local.
                     * Skip the MOVE and record that this register is an alias of x. */
                    bool copy_prop = false;
                    if (init->type == NODE_IDENT && !(is_last_expr && last_is_call)) {
                        int src = resolve_local(c, init->ident);
                        /* only for values that need no RC: an alias of an
                         * object would own nothing (docs/memory.md) */
                        if (src >= 0 && reg_scalar(c, resolve_copy_chain(c, src)) &&
                            !name_is_captured(c, init->ident)) {
                            src = resolve_copy_chain(c, src);
                            c->reg_meta[reg].has_copy = 1;
                            c->reg_meta[reg].copy_of  = src;
                            reg_copy_int_meta(c, reg, src);
                            copy_prop = true;
                        }
                    }
                    if (!copy_prop && is_last_expr && init->type == NODE_VARARG && need_from_last > 1) {
                        /* local a, b = ...: the first varargs, one per name
                         * (copies of the frame's: each local owns its own) */
                        for (int k = i + 1; k < node->local.count; k++) alloc_reg(c);
                        emit_abc(c, OP_VARARG, reg, need_from_last, 0, line);
                        for (int k = 0; k < need_from_last; k++) {
                            emit_abc(c, OP_RETAIN, reg + k, 0, 0, line);
                            reg_clear_int(c, reg + k);
                        }
                        break;
                    }
                    if (!copy_prop) {
                        compile_expr(c, init, reg);
                        own_temp(c, reg, line);          /* the local will own it */
                        /* If this is the last expression and it's a call, patch nret */
                        if (is_last_expr && last_is_call && need_from_last > 1) {
                            bool patched = patch_last_call_nret(c, need_from_last, line);
                            for (int k = i + 1; k < node->local.count; k++)
                                alloc_reg(c);
                            /* the extra results are owned too (or nil) */
                            if (!patched)
                                emit_abc(c, OP_LOAD_NIL, reg + 1, node->local.count - i - 1, 0, line);
                            break;
                        }
                    }
                } else {
                    emit_abc(c, OP_LOAD_NIL, reg, 1, 0, line);
                    reg_set_scalar(c, reg);
                }
            }

            // Step 2: name these registers as locals
            // Reset reg_top to first_reg (undo alloc_reg calls)
            c->reg_top = first_reg;

            for (int i = 0; i < node->local.count; i++) {
                int reg = first_reg + i;
                if (c->local_count >= SEA_MAX_REGISTERS) {
                    compiler_error(c, line, "too many local variables");
                    break;
                }
                SeaLocal *loc = &c->locals[c->local_count++];
                if (strcmp(node->local.names[i], "_") == 0) {
                    loc->name = (char*)sea_malloc(2);
                    strcpy(loc->name, "_");
                } else {
                    loc->name = (char*)sea_malloc(strlen(node->local.names[i])+1);
                    strcpy(loc->name, node->local.names[i]);
                }
                loc->depth = c->scope_depth;
                loc->captured = false;
                loc->is_const = node->local.is_const;
                loc->reg = reg;
                c->reg_top = reg + 1;
                c->reg_meta[reg].is_owned = 0;   /* the local owns its value */
                /* A local some closure captures can be changed by any call:
                 * keep no constant/int facts about it (they would let the
                 * folder or IIADD use a stale value). */
                if (name_is_captured(c, loc->name)) {
                    c->reg_meta[reg].is_int = 0;
                    c->reg_meta[reg].is_const = 0;
                    c->reg_meta[reg].is_scalar = 0;
                }
            }
            break;
        }

        case NODE_ASSIGN: {
            if (node->assign.op && !node->assign.op_wrapped) {
                compile_compound_assign(c, node);
                break;
            }
            /* Single assignment to a local: the local owns its value, so the
             * old one is released (OP_SETLOCAL) — skipped when both the old
             * and the new value need no RC, in which case the RHS is
             * compiled straight into the local's register (numeric code
             * keeps the old single-instruction form). Not for calls: they
             * load the callee into dst before evaluating the arguments
             * (`acc = fn(acc, v)` would read a clobbered register). */
            if (node->assign.count == 1 && node->assign.expr_count == 1 &&
                node->assign.targets[0]->type == NODE_IDENT) {
                const char *tname = node->assign.targets[0]->ident;
                int local = resolve_local(c, tname);
                SeaNode *rhs = node->assign.exprs[0];
                SeaLocal *tloc = local >= 0 ? find_local(c, tname) : NULL;
                if (local >= 0 && !(tloc && tloc->is_const)) {
                    /* Before writing: materialize any other locals copying from
                     * `local`, and `local` itself if it is a deferred copy (of
                     * a scalar) — the RHS may read it (e.g. i = i+1). */
                    materialize_copies_of(c, local, line);
                    if (c->reg_meta[local].has_copy) {
                        emit_abc(c, OP_MOVE, local, c->reg_meta[local].copy_of, 0, line);
                        c->reg_meta[local].has_copy = 0;
                    }
                    /* "x = nil": OP_RELEASE drops the old value and stores nil */
                    if (rhs->type == NODE_NIL) {
                        if (reg_scalar(c, local)) emit_abc(c, OP_LOAD_NIL, local, 1, 0, rhs->line);
                        else emit_abc(c, OP_RELEASE, local, 0, 0, rhs->line);
                        reg_set_scalar(c, local);
                        mark_if_captured(c, tname, local, line);
                        break;
                    }
                    /* "s = s .. x": append to the local's own string (in place
                     * when it is a rope nothing else holds) */
                    if (rhs->type == NODE_BINOP && rhs->binop.op == BINOP_CONCAT &&
                        rhs->binop.left->type == NODE_IDENT &&
                        resolve_local(c, rhs->binop.left->ident) == local) {
                        int t = compile_expr(c, rhs->binop.right, -1);
                        emit_abc(c, OP_APPEND, local, t, 0, line);
                        drop_temp(c, t, line);
                        free_reg(c, t);
                        reg_clear_int(c, local);
                        mark_if_captured(c, tname, local, line);
                        break;
                    }
                    /* Clear is_const before compiling RHS: the target register may
                     * appear in the expression (e.g. i = i+1). Without clearing,
                     * the fold would use the stale compile-time value instead of
                     * the actual runtime value, producing a constant store. */
                    c->reg_meta[local].is_const = 0;
                    /* Not for `and`/`or` either: they leave the left operand in
                     * dst before evaluating the right one, which may read the
                     * local (`k = k == n and 1 or k + 1`). */
                    bool short_circuit = rhs->type == NODE_BINOP &&
                        (rhs->binop.op == BINOP_AND || rhs->binop.op == BINOP_OR);
                    if (rhs->type != NODE_CALL && rhs->type != NODE_METHOD_CALL && !short_circuit &&
                        reg_scalar(c, local) && expr_is_scalar(c, rhs)) {
                        compile_expr(c, rhs, local);
                        c->reg_meta[local].is_owned = 0;
                    } else {
                        int t = compile_expr(c, rhs, -1);
                        store_local(c, local, t, line);
                        free_reg(c, t);
                    }
                    mark_if_captured(c, tname, local, line);
                    break;
                }
            }

            // Special case: v, ok = m[k] or m.k  (Go-style two-value lookup)
            if (node->assign.count == 2 && node->assign.expr_count == 1 &&
                is_two_value_lookup(node->assign.exprs[0])) {
                int sv = c->reg_top;
                int t0 = alloc_reg(c);  // value temp
                int t1 = alloc_reg(c);  // ok temp
                (void)t1;  // t1 == t0+1
                int obj_reg, key_reg;
                compile_lookup_operands(c, node->assign.exprs[0], &obj_reg, &key_reg, line);
                emit_abc(c, OP_GET_TABLE2, t0, obj_reg, key_reg, line);
                reg_clear_int(c, t0);
                reg_set_scalar(c, t1);
                own_temp(c, t0, line);   /* before obj may be dropped */
                drop_temp(c, key_reg, line);
                drop_temp(c, obj_reg, line);
                free_reg(c, key_reg);
                free_reg(c, obj_reg);
                compile_store(c, node->assign.targets[0], t0);
                compile_store(c, node->assign.targets[1], t0 + 1);
                free_regs_to(c, sv);
                break;
            }

            // General case (multiple targets, non-local target, etc.)
            // Compile RHS values into temporaries
            int saved_top = c->reg_top;
            int *val_regs = SEA_ALLOC_N(int, node->assign.count);

            int an_need_from_last = node->assign.count - (node->assign.expr_count - 1);
            bool an_last_is_call = (node->assign.expr_count > 0 &&
                (node->assign.exprs[node->assign.expr_count-1]->type == NODE_CALL ||
                 node->assign.exprs[node->assign.expr_count-1]->type == NODE_METHOD_CALL));

            for (int i = 0; i < node->assign.count; i++) {
                if (i < node->assign.expr_count) {
                    bool is_last_expr = (i == node->assign.expr_count - 1);
                    /* For single-assignment, borrow the local register to avoid MOVE.
                     * Multi-assignment must copy to temps first to prevent LHS/RHS aliasing
                     * (e.g. `a, b = b, a` would corrupt the swap if we borrowed). */
                    int rl = (node->assign.count == 1)
                             ? expr_local_reg(c, node->assign.exprs[i]) : -1;
                    val_regs[i] = (rl >= 0) ? rl : compile_expr(c, node->assign.exprs[i], -1);
                    if (is_last_expr && an_last_is_call && an_need_from_last > 1) {
                        /* Call occupies val_regs[i..i+need-1] consecutively */
                        bool patched = patch_last_call_nret(c, an_need_from_last, line);
                        /* The extra returns are at val_regs[i]+1, +2, ... */
                        for (int k = 1; k < an_need_from_last && (i + k) < node->assign.count; k++) {
                            val_regs[i + k] = alloc_reg(c);
                            if (patched) c->reg_meta[val_regs[i + k]].is_owned = 1;
                            else {
                                emit_abc(c, OP_LOAD_NIL, val_regs[i + k], 1, 0, line);
                                reg_set_scalar(c, val_regs[i + k]);
                            }
                        }
                        break;
                    }
                } else {
                    val_regs[i] = alloc_reg(c);
                    emit_abc(c, OP_LOAD_NIL, val_regs[i], 1, 0, line);
                    reg_set_scalar(c, val_regs[i]);
                }
            }
            /* Several targets: every value must be owned before the first
             * store — a store releases the target's old value, which may be
             * exactly what a later value borrowed (`a, b = b, a`). */
            if (node->assign.count > 1)
                for (int i = 0; i < node->assign.count; i++) own_temp(c, val_regs[i], line);

            // Store into targets
            for (int i = 0; i < node->assign.count; i++) {
                compile_store(c, node->assign.targets[i], val_regs[i]);
            }

            sea_free(val_regs, sizeof(int) * (size_t)node->assign.count);
            free_regs_to(c, saved_top);
            break;
        }

        case NODE_IF: {
            /* Materialize deferred copies (and drop constants) BEFORE branching:
             * a copy materialized inside one branch would be missing on the
             * other path (e.g. `local m = i; if c then m = l end; ... m ...`
             * read a stale register when the branch was not taken). */
            invalidate_locals_at_merge(c, line);
            int cond_reg = compile_expr(c, node->if_stmt.cond, -1);
            int jmp_false = emit_cond_jump_false(c, cond_reg, line);
            free_reg(c, cond_reg);

            IntSnap *if_entry = (IntSnap*)sea_malloc(sizeof(IntSnap));
            snap_take(c, if_entry);
            begin_scope(c);
            compile_block(c, node->if_stmt.then_body);
            end_scope(c, line);

            if (node->if_stmt.else_body) {
                int jmp_end = emit_jmp(c, OP_JMP, 0, line);
                patch_jmp(c, jmp_false);
                IntSnap *after_then = (IntSnap*)sea_malloc(sizeof(IntSnap));
                snap_take(c, after_then);
                snap_restore(c, if_entry);   /* `else` starts from the pre-branch state */
                clear_local_consts(c);       /* nothing learned in `then` holds here */
                begin_scope(c);
                compile_stmt(c, node->if_stmt.else_body);
                end_scope(c, line);
                patch_jmp(c, jmp_end);
                snap_meet(c, after_then);    /* int only if int on both paths */
                sea_free(after_then, sizeof(IntSnap));
            } else {
                patch_jmp(c, jmp_false);
                snap_meet(c, if_entry);      /* the branch may not have run */
            }
            sea_free(if_entry, sizeof(IntSnap));
            invalidate_locals_at_merge(c, line);
            break;
        }

        case NODE_WHILE: {
            /* Materialize deferred copies before the loop: the loop back-edge
             * would repeat any MOVE emitted inside the body. Emitting here
             * (before loop_start) ensures the MOVE runs exactly once. */
            invalidate_locals_at_merge(c, line);
            loop_head_ints(c, node->while_loop.body);
            IntSnap *loop_head = (IntSnap*)sea_malloc(sizeof(IntSnap));
            snap_take(c, loop_head);
            int loop_start = sea_chunk_pos(c->chunk);

            int cond_reg = compile_expr(c, node->while_loop.cond, -1);
            int jmp_exit = emit_cond_jump_false(c, cond_reg, line);
            free_reg(c, cond_reg);

            // Save break/continue lists
            int *saved_break = c->break_list;
            int saved_bc = c->break_count;
            int saved_bcap = c->break_cap;
            int *saved_cont = c->continue_list;
            int saved_cc = c->continue_count;
            int saved_ccap = c->continue_cap;

            c->break_list = NULL; c->break_count = 0; c->break_cap = 0;
            c->continue_list = NULL; c->continue_count = 0; c->continue_cap = 0;

            begin_scope(c);
            int while_body_start_local = c->local_count;
            int saved_lbl = c->loop_body_local;
            c->loop_body_local = while_body_start_local;
            compile_block(c, node->while_loop.body);
            c->loop_body_local = saved_lbl;
            /* Close upvalues opened in the body before backward jump */
            {
                int close_from = -1;
                for (int i = while_body_start_local; i < c->local_count; i++) {
                    if (c->locals[i].captured) {
                        int reg = c->locals[i].reg;
                        if (c->reg_meta[reg].has_copy) {
                            emit_abc(c, OP_MOVE, reg, c->reg_meta[reg].copy_of, 0, line);
                            c->reg_meta[reg].has_copy = 0;
                        }
                        if (close_from < 0 || reg < close_from)
                            close_from = reg;
                    }
                }
                if (close_from >= 0) {
                    emit_abx(c, OP_CLOSE_UP, close_from, 0, line);
                }
            }
            end_scope(c, line);

            // Patch continues to jump back to loop start
            for (int i = 0; i < c->continue_count; i++) {
                int offset = loop_start - (c->continue_list[i] + 1);
                uint32_t instr = c->chunk->code[c->continue_list[i]];
                instr = (instr & 0x0000FFFF) | ((uint32_t)(offset + 32767) << 16);
                c->chunk->code[c->continue_list[i]] = instr;
            }

            // Jump back to start
            emit_asbx(c, OP_JMP, 0, loop_start - sea_chunk_pos(c->chunk) - 1, line);
            patch_jmp(c, jmp_exit);

            // Patch breaks
            for (int i = 0; i < c->break_count; i++) {
                patch_jmp(c, c->break_list[i]);
            }

            if (c->break_list)    sea_free(c->break_list, sizeof(int) * (size_t)c->break_cap);
            if (c->continue_list) sea_free(c->continue_list, sizeof(int) * (size_t)c->continue_cap);

            c->break_list = saved_break; c->break_count = saved_bc; c->break_cap = saved_bcap;
            c->continue_list = saved_cont; c->continue_count = saved_cc; c->continue_cap = saved_ccap;
            snap_meet(c, loop_head);   /* the body may run zero times / exit early */
            sea_free(loop_head, sizeof(IntSnap));
            invalidate_locals_at_merge(c, line);
            break;
        }

        case NODE_FOR_NUM: {
            begin_scope(c);

            // Allocate 3 control vars: idx, limit, step
            int idx_reg = alloc_reg(c);
            int limit_reg = alloc_reg(c);
            int step_reg = alloc_reg(c);

            compile_expr(c, node->for_num.start, idx_reg);
            compile_expr(c, node->for_num.limit, limit_reg);

            if (node->for_num.step) {
                compile_expr(c, node->for_num.step, step_reg);
            } else {
                emit_asbx(c, OP_LOAD_INT, step_reg, 1, line);
                reg_set_const_int(c, step_reg, 1);
            }

            /* The loop variable lives at idx_reg+3, preserving the standard layout:
             *   R[A]   = internal counter (idx)
             *   R[A+1] = limit
             *   R[A+2] = step
             *   R[A+3] = user-visible variable (i)
             *
             * FOR_PREP initialises R[A+3]=R[A] before the first iteration.
             * FOR_LOOP advances R[A] and writes R[A+3]=R[A] before jumping back.
             * The explicit MOVE that used to sit at the top of the loop body is
             * therefore gone — one fewer bytecode instruction per iteration. */
            int var_reg = alloc_reg(c);
            int loop_var_local = c->local_count;
            SeaLocal *loc = &c->locals[c->local_count++];
            loc->name = (char*)sea_malloc(strlen(node->for_num.var)+1);
            strcpy(loc->name, node->for_num.var);
            loc->depth = c->scope_depth;
            loc->captured = false;
            loc->is_const = false;
            loc->reg = var_reg;
            reg_set_scalar(c, var_reg);   /* numeric for: always a number */
            if (name_is_captured(c, loc->name)) reg_clear_int(c, var_reg);

            /* Materialize outer-scope deferred copies before the loop. */
            invalidate_locals_at_merge(c, line);

            // FOR_PREP: copies R[A]→R[A+3]; jumps past loop if initial condition fails
            int prep_jmp = emit_jmp(c, OP_FOR_PREP, idx_reg, line);

            /* If all three bounds are known integers, the loop variable is also int. */
            if (c->reg_meta[idx_reg].is_int && c->reg_meta[limit_reg].is_int && c->reg_meta[step_reg].is_int) {
                c->reg_meta[var_reg].is_int = 1;
                c->reg_meta[var_reg].is_const = 0;
            }
            /* After the loop variable's type is known (it feeds e.g. `s = s + i`).
             * Only metadata changes here — no code is emitted. */
            loop_head_ints(c, node->for_num.body);
            IntSnap *loop_head = (IntSnap*)sea_malloc(sizeof(IntSnap));
            snap_take(c, loop_head);

            int loop_start = sea_chunk_pos(c->chunk);

            // Save break/continue
            int *saved_break = c->break_list;
            int saved_bc = c->break_count;
            int saved_bcap = c->break_cap;
            int *saved_cont = c->continue_list;
            int saved_cc = c->continue_count;
            int saved_ccap = c->continue_cap;
            c->break_list = NULL; c->break_count = 0; c->break_cap = 0;
            c->continue_list = NULL; c->continue_count = 0; c->continue_cap = 0;

            /* No MOVE emitted here: FOR_PREP/FOR_LOOP maintain R[A+3]. */

            /* Open a new scope for the loop body so that captured locals get
             * a fresh CLOSE_UP at the end of each iteration. */
            begin_scope(c);
            int saved_lbl = c->loop_body_local;
            c->loop_body_local = c->local_count;
            compile_block(c, node->for_num.body);
            c->loop_body_local = saved_lbl;
            /* Per-iteration variables: close the loop variable's and the body
             * locals' upvalues at the continue point (see loop_close_from). */
            int close_from = loop_close_from(c, loop_var_local, line);
            end_scope(c, line);  /* remove body locals from scope */

            // Continue point: close this iteration's upvalues, advance index
            int cont_point = sea_chunk_pos(c->chunk);
            for (int i = 0; i < c->continue_count; i++) {
                int offset = cont_point - (c->continue_list[i] + 1);
                uint32_t instr = c->chunk->code[c->continue_list[i]];
                instr = (instr & 0x0000FFFF) | ((uint32_t)(offset + 32767) << 16);
                c->chunk->code[c->continue_list[i]] = instr;
            }
            if (close_from >= 0) emit_abx(c, OP_CLOSE_UP, close_from, 0, line);

            // FOR_LOOP: adds step to idx, checks vs limit, jumps back if still in range
            bool all_int = c->reg_meta[idx_reg].is_int && c->reg_meta[limit_reg].is_int && c->reg_meta[step_reg].is_int;
            bool step_is_one = !node->for_num.step;  /* omitted step defaults to 1 */
            SeaOpcode for_loop_op;
            if (all_int && step_is_one)
                for_loop_op = OP_FOR_LOOP_STEP1;
            else if (all_int)
                for_loop_op = OP_FOR_LOOP_INT;
            else
                for_loop_op = OP_FOR_LOOP;
            int loop_jmp = emit_jmp(c, for_loop_op, idx_reg, line);
            // Patch to loop_start
            int offset = loop_start - sea_chunk_pos(c->chunk);
            sea_chunk_patch_jmp(c->chunk, loop_jmp, loop_start);
            (void)offset;

            // Break patches + FOR_PREP forward jump — all land here (after loop)
            patch_jmp(c, prep_jmp);
            for (int i = 0; i < c->break_count; i++) {
                patch_jmp(c, c->break_list[i]);
            }

            if (c->break_list)    sea_free(c->break_list, sizeof(int) * (size_t)c->break_cap);
            if (c->continue_list) sea_free(c->continue_list, sizeof(int) * (size_t)c->continue_cap);
            c->break_list = saved_break; c->break_count = saved_bc; c->break_cap = saved_bcap;
            c->continue_list = saved_cont; c->continue_count = saved_cc; c->continue_cap = saved_ccap;

            snap_meet(c, loop_head);   /* zero iterations / break */
            sea_free(loop_head, sizeof(IntSnap));
            end_scope(c, line);
            invalidate_locals_at_merge(c, line);
            break;
        }

        case NODE_FOR_IN: {
            begin_scope(c);

            // Compile iterator expression
            int iter_reg = alloc_reg(c);
            compile_expr(c, node->for_in.iter_expr, iter_reg);
            /* R[iter_reg] holds a reference to the container for the whole
             * loop (the body may reassign the variable it came from): a
             * hidden owning local, released when the loop ends (FOR_IN_NEXT
             * does it on exhaustion; end_scope on break) */
            own_temp(c, iter_reg, line);
            c->reg_meta[iter_reg].is_owned = 0;
            add_local_at(c, "(for)", iter_reg, line);
            // Reserve R[iter_reg+1] as the cursor slot (hidden, not a named local)
            int cursor_reg = alloc_reg(c);
            (void)cursor_reg;

            // FOR_IN_INIT: sets up state, clears the B loop variables
            emit_abc(c, OP_FOR_IN_INIT, iter_reg, node->for_in.var_count, 0, line);

            /* Materialize outer-scope deferred copies before the loop. */
            invalidate_locals_at_merge(c, line);
            loop_head_ints(c, node->for_in.body);
            IntSnap *loop_head = (IntSnap*)sea_malloc(sizeof(IntSnap));
            snap_take(c, loop_head);

            int loop_start = sea_chunk_pos(c->chunk);

            // Allocate result registers for loop variables
            int loop_var_local = c->local_count;
            for (int i = 0; i < node->for_in.var_count; i++) {
                int r = alloc_reg(c);
                SeaLocal *loc = &c->locals[c->local_count++];
                loc->name = (char*)sea_malloc(strlen(node->for_in.vars[i])+1);
                strcpy(loc->name, node->for_in.vars[i]);
                loc->depth = c->scope_depth;
                loc->captured = false;
                loc->is_const = false;
                loc->reg = r;
                reg_clear_int(c, r);   /* owns what FOR_IN_NEXT stores */
            }

            /* FOR_IN_NEXT in ABC format: A=iter_reg, B=var_count, C=0.
             * Full 8-bit A field — no truncation, supports up to 255 locals before the loop.
             * When done the VM releases the iterator state and sets REG(A)=nil.
             * The following OP_JMP_NIL (AsBx) handles the actual exit jump. */
            emit_abc(c, OP_FOR_IN_NEXT, iter_reg, node->for_in.var_count, 0, line);

            /* Exit jump: leave the loop when REG(iter_reg) == nil (iterator exhausted). */
            int exit_jmp = emit_jmp(c, OP_JMP_NIL, iter_reg, line);

            // Save break/continue
            int *saved_break = c->break_list;
            int saved_bc = c->break_count;
            int saved_bcap = c->break_cap;
            int *saved_cont = c->continue_list;
            int saved_cc = c->continue_count;
            int saved_ccap = c->continue_cap;
            c->break_list = NULL; c->break_count = 0; c->break_cap = 0;
            c->continue_list = NULL; c->continue_count = 0; c->continue_cap = 0;

            begin_scope(c);
            int saved_lbl = c->loop_body_local;
            c->loop_body_local = c->local_count;
            compile_block(c, node->for_in.body);
            c->loop_body_local = saved_lbl;
            /* Per-iteration variables (see loop_close_from) */
            int close_from = loop_close_from(c, loop_var_local, line);
            end_scope(c, line);

            // Continue: close this iteration's upvalues, jump back to loop start
            int cont_point = sea_chunk_pos(c->chunk);
            for (int i = 0; i < c->continue_count; i++) {
                int offset = cont_point - (c->continue_list[i] + 1);
                uint32_t instr = c->chunk->code[c->continue_list[i]];
                instr = (instr & 0x0000FFFF) | ((uint32_t)(offset + 32767) << 16);
                c->chunk->code[c->continue_list[i]] = instr;
            }
            if (close_from >= 0) emit_abx(c, OP_CLOSE_UP, close_from, 0, line);

            emit_asbx(c, OP_JMP, 0, loop_start - sea_chunk_pos(c->chunk) - 1, line);

            // Patch the exit jump
            patch_jmp(c, exit_jmp);

            // Break patches
            for (int i = 0; i < c->break_count; i++) {
                patch_jmp(c, c->break_list[i]);
            }

            if (c->break_list)    sea_free(c->break_list, sizeof(int) * (size_t)c->break_cap);
            if (c->continue_list) sea_free(c->continue_list, sizeof(int) * (size_t)c->continue_cap);
            c->break_list = saved_break; c->break_count = saved_bc; c->break_cap = saved_bcap;
            c->continue_list = saved_cont; c->continue_count = saved_cc; c->continue_cap = saved_ccap;

            snap_meet(c, loop_head);   /* zero iterations / break */
            sea_free(loop_head, sizeof(IntSnap));
            end_scope(c, line);
            invalidate_locals_at_merge(c, line);
            break;
        }

        case NODE_RETURN: {
            /* Return values are owned (+1); every local of the function is
             * released first (the captured ones move into their closures
             * when RETURN closes the upvalues). `return x` hands over the
             * reference x itself holds. */
            if (node->ret.count == 0) {
                emit_locals_cleanup(c, 0, false, -1, line);
                emit(c, SEA_MAKE_ABC(OP_RETURN0, 0, 0, 0), line);
            } else if (node->ret.count == 1) {
                SeaNode *e = node->ret.exprs[0];
                SeaLocal *rl = (e->type == NODE_IDENT) ? find_local(c, e->ident) : NULL;
                if (rl && !rl->captured) {
                    int reg = resolve_copy_chain(c, rl->reg);
                    emit_locals_cleanup(c, 0, false, rl->reg, line);
                    emit_abc(c, OP_RETURN, reg, 1, 0, line);
                } else {
                    int reg = compile_expr(c, e, -1);
                    /* `return f(x)` hands over every value f returns: the
                     * CALL asks for all (B=255) and RETURN takes the count
                     * from the VM. Not for method calls (their result is
                     * moved down by one register) nor calls that are not
                     * the last instruction. */
                    int nr = 1;
                    if (e->type == NODE_CALL && c->chunk->code_len > 0) {
                        uint32_t last = c->chunk->code[c->chunk->code_len - 1];
                        SeaOpcode lop = SEA_INSTR_OP(last);
                        if ((lop == OP_CALL || lop == OP_CALL_SELF) && SEA_INSTR_A(last) == reg &&
                            SEA_INSTR_B(last) == 1) {
                            c->chunk->code[c->chunk->code_len - 1] = (last & 0xFF00FFFF) | (255u << 16);
                            nr = 255;
                        }
                    }
                    own_temp(c, reg, line);
                    emit_locals_cleanup(c, 0, false, -1, line);
                    emit_abc(c, OP_RETURN, reg, nr, 0, line);
                    c->reg_meta[reg].is_owned = 0;
                    free_reg(c, reg);
                }
            } else {
                // Multiple returns
                int base = c->reg_top;
                for (int i = 0; i < node->ret.count; i++) {
                    int r = alloc_reg(c);
                    compile_expr(c, node->ret.exprs[i], r);
                    own_temp(c, r, line);
                }
                emit_locals_cleanup(c, 0, false, -1, line);
                emit_abc(c, OP_RETURN, base, node->ret.count, 0, line);
                for (int i = 0; i < node->ret.count; i++) c->reg_meta[base + i].is_owned = 0;
                free_regs_to(c, base);
            }
            break;
        }

        case NODE_BREAK: {
            /* leaving the body: release its locals (the loop's own variables
             * are released after the loop, where break lands) */
            if (c->loop_body_local >= 0) emit_locals_cleanup(c, c->loop_body_local, true, -1, line);
            int jmp = emit_jmp(c, OP_JMP, 0, line);
            SEA_DYNARRAY_PUSH(c->break_list, c->break_count, c->break_cap, int, jmp);
            break;
        }

        case NODE_CONTINUE: {
            if (c->loop_body_local >= 0) emit_locals_cleanup(c, c->loop_body_local, true, -1, line);
            int jmp = emit_jmp(c, OP_JMP, 0, line);
            SEA_DYNARRAY_PUSH(c->continue_list, c->continue_count, c->continue_cap, int, jmp);
            break;
        }

        case NODE_DO: {
            begin_scope(c);
            // DO node reuses block fields
            SeaNode block_node;
            block_node.type = NODE_BLOCK;
            block_node.line = line;
            block_node.block.stmts = node->block.stmts;
            block_node.block.count = node->block.count;
            compile_block(c, &block_node);
            end_scope(c, line);
            break;
        }

        case NODE_BLOCK:
            compile_block(c, node);
            break;

        case NODE_GO: {
            SeaNode *call = node->go_call;
            if (call->type != NODE_CALL && call->type != NODE_METHOD_CALL) {
                compiler_error(c, line, "'go' requires a function call");
                break;
            }
            /* OP_GO retains the function; the arguments are passed owned
             * (they move into the goroutine), as for a call */
            int saved_top = c->reg_top;
            int fn_reg = alloc_reg(c);
            if (call->type == NODE_CALL) {
                compile_expr(c, call->call.fn, fn_reg);
                c->reg_top = fn_reg + 1;
                int first_arg = c->reg_top;
                for (int i = 0; i < call->call.argc; i++) {
                    int arg = alloc_reg(c);
                    compile_expr(c, call->call.args[i], arg);
                    own_temp(c, arg, line);
                }
                emit_abc(c, OP_GO, fn_reg, first_arg, call->call.argc, line);
                for (int i = 0; i < call->call.argc; i++) c->reg_meta[first_arg + i].is_owned = 0;
                drop_temp(c, fn_reg, line);
            } else {
                // Method call as goroutine
                compile_expr(c, call->method.obj, fn_reg);
                c->reg_top = fn_reg + 1;
                int mfn_reg = alloc_reg(c);
                int kidx = add_const_str(c, call->method.method);
                if (kidx <= 255)
                    emit_abc(c, OP_GET_FIELD, mfn_reg, fn_reg, kidx, line);
                else {
                    int key_reg = alloc_reg(c);
                    emit_abx(c, OP_LOAD_K, key_reg, kidx, line);
                    emit_abc(c, OP_GET_TABLE, mfn_reg, fn_reg, key_reg, line);
                    free_reg(c, key_reg);
                }
                int self_reg = alloc_reg(c);
                emit_abc(c, OP_MOVE, self_reg, fn_reg, 0, line);
                if (c->reg_meta[fn_reg].is_owned) {     /* see NODE_METHOD_CALL */
                    c->reg_meta[fn_reg].is_owned = 0;
                    c->reg_meta[self_reg].is_owned = 1;
                } else {
                    c->reg_meta[self_reg].is_scalar = c->reg_meta[fn_reg].is_scalar;
                    own_temp(c, self_reg, line);
                }
                for (int i = 0; i < call->method.argc; i++) {
                    int arg = alloc_reg(c);
                    compile_expr(c, call->method.args[i], arg);
                    own_temp(c, arg, line);
                }
                /* the args (self first) start at self_reg */
                emit_abc(c, OP_GO, mfn_reg, self_reg, call->method.argc + 1, line);
                for (int i = 0; i <= call->method.argc; i++) c->reg_meta[self_reg + i].is_owned = 0;
            }
            free_regs_to(c, saved_top);
            break;
        }

        case NODE_SELECT: {
            int sel_n = node->select.count;
            if (sel_n == 0) break;
            invalidate_locals_at_merge(c, line);   /* cases are exclusive branches (see NODE_IF) */
            IntSnap *sel_entry = (IntSnap*)sea_malloc(sizeof(IntSnap));
            IntSnap *sel_meet  = (IntSnap*)sea_malloc(sizeof(IntSnap));
            snap_take(c, sel_entry);
            bool sel_first = true;

            /* Per-case metadata built during register allocation */
            typedef struct {
                int  ch_reg;       /* channel register */
                int  val_reg;      /* send value (send) or recv dest (recv) */
                int  flags;        /* 0=recv, 1=send, 2=default */
                const char *new_var;  /* new local name for recv, or NULL */
                int  assign_local; /* recv into an existing local: its register, else -1 */
            } SelCase;

            SelCase *sc_info = alloca(sizeof(SelCase) * sel_n);
            memset(sc_info, 0, sizeof(SelCase) * sel_n);

            int sel_has_def = 0;
            int saved_top   = c->reg_top;

            /* --- Pass 1: allocate registers + compile channel/value exprs --- */
            for (int i = 0; i < sel_n; i++) {
                SeaSelectCase *sc = &node->select.cases[i];
                if (sc->is_default) {
                    sc_info[i].flags   = 2;
                    sc_info[i].ch_reg  = 0;
                    sc_info[i].val_reg = 0;
                    sel_has_def = 1;
                    continue;
                }

                SeaNode *stmt = sc->stmt;
                if (!stmt) { compiler_error(c, line, "select: empty case"); break; }

                if (stmt->type == NODE_ASSIGN && stmt->assign.expr_count > 0) {
                    /* Recv: x = <-ch */
                    SeaNode *rhs = stmt->assign.exprs[0];
                    SeaNode *ch_node = NULL;
                    if (rhs->type == NODE_UNOP && rhs->unop.op == UNOP_CHAN_RECV)
                        ch_node = rhs->unop.expr;
                    else if (rhs->type == NODE_CHAN_RECV)
                        ch_node = rhs->unop.expr;
                    if (!ch_node) { compiler_error(c, line, "select: invalid recv case"); break; }

                    int ch_reg = alloc_reg(c);
                    compile_expr(c, ch_node, ch_reg);

                    /* Dest register: always a fresh one — the received value
                     * (+1 from the channel) moves into the existing local,
                     * becomes the case's new local, or is released, in the
                     * winning case's prologue */
                    int val_reg = alloc_reg(c);
                    const char *new_var = NULL;
                    int assign_local = -1;
                    if (stmt->assign.count > 0 && stmt->assign.targets[0]->type == NODE_IDENT) {
                        const char *nm = stmt->assign.targets[0]->ident;
                        if (strcmp(nm, "_") != 0) {
                            int ex = resolve_local(c, nm);
                            if (ex >= 0) assign_local = ex;   /* existing local */
                            else new_var = nm;                /* defined in case scope */
                        }
                    }

                    sc_info[i] = (SelCase){ch_reg, val_reg, 0, new_var, assign_local};

                } else if (stmt->type == NODE_EXPR_STMT) {
                    /* Send: ch <- val */
                    SeaNode *inner = stmt->expr_stmt;
                    /* inner is NODE_CHAN_SEND (which stores left/right in binop) */
                    if (inner->type != NODE_CHAN_SEND &&
                        !(inner->type == NODE_BINOP && inner->binop.op == BINOP_CHAN_SEND)) {
                        compiler_error(c, line, "select: invalid send case");
                        break;
                    }
                    int ch_reg  = alloc_reg(c);
                    int val_reg = alloc_reg(c);
                    compile_expr(c, inner->binop.left,  ch_reg);
                    compile_expr(c, inner->binop.right, val_reg);
                    sc_info[i] = (SelCase){ch_reg, val_reg, 1, NULL, -1};

                } else {
                    compiler_error(c, line, "select: unrecognised case statement");
                    break;
                }
            }
            if (c->had_error) break;

            int winner_reg     = alloc_reg(c);
            int case_top_saved = c->reg_top;  /* base for restoring after each body */

            /* --- Emit bytecode --- */
            emit_abc(c, OP_SELECT_BEGIN, sel_n, sel_has_def, 0, line);
            for (int i = 0; i < sel_n; i++)
                emit_abc(c, OP_SELECT_CASE, sc_info[i].ch_reg, sc_info[i].val_reg,
                         sc_info[i].flags, line);
            emit_abc(c, OP_SELECT_END, winner_reg, sel_n, sel_has_def, line);

            /* Jump table: one JMP per case (placeholder offsets) */
            int *jmp_pos  = alloca(sizeof(int) * sel_n);
            int *end_jmps = alloca(sizeof(int) * sel_n);
            for (int i = 0; i < sel_n; i++)
                jmp_pos[i] = emit_jmp(c, OP_JMP, 0, line);

            /* --- Case bodies --- */
            for (int i = 0; i < sel_n; i++) {
                SeaSelectCase *sc = &node->select.cases[i];
                patch_jmp(c, jmp_pos[i]);          /* jump table → body[i] */
                c->reg_top = case_top_saved;        /* protect case registers */
                clear_local_consts(c);              /* nothing from other cases holds */
                snap_restore(c, sel_entry);         /* each case starts from the entry state */

                begin_scope(c);
                /* The winning recv case's value: into the existing local, the
                 * case's new local, or released */
                if (!sc->is_default && sc_info[i].flags == 0) {
                    int vr = sc_info[i].val_reg;
                    reg_clear_int(c, vr);
                    c->reg_meta[vr].is_owned = 1;
                    if (sc_info[i].new_var) {
                        add_local_at(c, sc_info[i].new_var, vr, line);
                        c->reg_meta[vr].is_owned = 0;
                    } else if (sc_info[i].assign_local >= 0) {
                        int al = sc_info[i].assign_local;
                        materialize_copies_of(c, al, line);
                        if (c->reg_meta[al].has_copy) {
                            c->reg_meta[al].has_copy = 0;
                            c->reg_meta[al].is_scalar = 1;
                        }
                        c->reg_meta[al].is_const = 0;
                        store_local(c, al, vr, line);
                        for (int li = 0; li < c->local_count; li++)
                            if (c->locals[li].reg == al) { mark_if_captured(c, c->locals[li].name, al, line); break; }
                    } else {
                        drop_temp(c, vr, line);
                    }
                }

                if (sc->body) compile_block(c, sc->body);
                end_scope(c, line);
                c->reg_top = case_top_saved;        /* restore after end_scope */
                if (sel_first) { snap_take(c, sel_meet); sel_first = false; }
                else { snap_meet(c, sel_meet); snap_take(c, sel_meet); }

                end_jmps[i] = emit_jmp(c, OP_JMP, 0, line);  /* → after select */
            }

            for (int i = 0; i < sel_n; i++)
                patch_jmp(c, end_jmps[i]);

            /* Channel and send-value temporaries were evaluated for every
             * case: drop the owned ones (a channel that took a sent value
             * retained it) */
            for (int i = 0; i < sel_n; i++) {
                if (sc_info[i].flags == 2) continue;
                if (sc_info[i].flags == 1) drop_temp(c, sc_info[i].val_reg, line);
                drop_temp(c, sc_info[i].ch_reg, line);
            }
            free_regs_to(c, saved_top);
            if (!sel_first) snap_restore(c, sel_meet);   /* int only if int in every case */
            sea_free(sel_entry, sizeof(IntSnap));
            sea_free(sel_meet, sizeof(IntSnap));
            invalidate_locals_at_merge(c, line);
            break;
        }

        case NODE_DEFER: {
            /* defer f(args) / defer obj:m(args): the callee and every argument
             * are evaluated NOW (Go semantics) into R[fn_reg+1..]; OP_DEFER
             * copies them into the VM's defer entry, which holds at most
             * SEA_DEFER_MAX_ARGS arguments. */
            int saved_top = c->reg_top;
            SeaNode *call = node->defer_stmt;
            int fn_reg = alloc_reg(c);
            if (call->type == NODE_CALL) {
                if (call->call.argc > SEA_DEFER_MAX_ARGS) {
                    compiler_error(c, line, "defer supports at most %d arguments (got %d)",
                                   SEA_DEFER_MAX_ARGS, call->call.argc);
                    break;
                }
                /* OP_DEFER retains the callee; the arguments are passed
                 * owned and move into the defer entry */
                compile_expr(c, call->call.fn, fn_reg);
                c->reg_top = fn_reg + 1;
                for (int i = 0; i < call->call.argc; i++) {
                    int arg = alloc_reg(c);
                    compile_expr(c, call->call.args[i], arg);
                    own_temp(c, arg, line);
                }
                emit_abc(c, OP_DEFER, fn_reg, call->call.argc, 0, line);
                for (int i = 1; i <= call->call.argc; i++) c->reg_meta[fn_reg + i].is_owned = 0;
                drop_temp(c, fn_reg, line);
            } else if (call->type == NODE_METHOD_CALL) {
                /* obj:m(args) == obj.m(obj, args): self is the first argument */
                int argc = call->method.argc + 1;
                if (argc > SEA_DEFER_MAX_ARGS) {
                    compiler_error(c, line, "defer supports at most %d arguments (got %d, incl. self)",
                                   SEA_DEFER_MAX_ARGS, argc);
                    break;
                }
                int self_reg = alloc_reg(c);
                compile_expr(c, call->method.obj, self_reg);
                own_temp(c, self_reg, line);   /* self: an owned argument */
                int kidx = add_const_str(c, call->method.method);
                if (kidx <= 255) {
                    emit_abc(c, OP_GET_FIELD, fn_reg, self_reg, kidx, line);
                } else {
                    int key_reg = alloc_reg(c);
                    emit_abx(c, OP_LOAD_K, key_reg, kidx, line);
                    emit_abc(c, OP_GET_TABLE, fn_reg, self_reg, key_reg, line);
                    free_reg(c, key_reg);
                }
                c->reg_top = self_reg + 1;
                for (int i = 0; i < call->method.argc; i++) {
                    int arg = alloc_reg(c);
                    compile_expr(c, call->method.args[i], arg);
                    own_temp(c, arg, line);
                }
                emit_abc(c, OP_DEFER, fn_reg, argc, 0, line);
                for (int i = 1; i <= argc; i++) c->reg_meta[fn_reg + i].is_owned = 0;
            } else {
                // Defer any expression
                compile_expr(c, call, fn_reg);
                emit_abc(c, OP_DEFER, fn_reg, 0, 0, line);
                drop_temp(c, fn_reg, line);
            }
            free_regs_to(c, saved_top);
            break;
        }

        case NODE_PANIC: {
            int val = compile_expr(c, node->panic_expr, -1);
            emit_abc(c, OP_PANIC, val, 0, 0, line);   /* retains the value */
            drop_temp(c, val, line);
            free_reg(c, val);
            break;
        }

        case NODE_DEL: {
            SeaNode *idx_node = node->del_stmt;  // NODE_INDEX
            int obj = compile_expr(c, idx_node->index.obj, -1);
            int key = compile_expr(c, idx_node->index.key, -1);
            emit_abc(c, OP_DEL, obj, key, 0, line);
            drop_temp(c, key, line);
            drop_temp(c, obj, line);
            free_reg(c, key);
            free_reg(c, obj);
            break;
        }

        default:
            // Try as expression statement
            if (node->type != NODE_BLOCK) {
                int tmp = compile_expr(c, node, -1);
                drop_temp(c, tmp, line);
                free_reg(c, tmp);
            }
            break;
    }
}

static void compile_block(SeaCompiler *c, SeaNode *block) {
    if (!block || c->had_error) return;
    if (block->type != NODE_BLOCK) {
        compile_stmt(c, block);
        return;
    }
    for (int i = 0; i < block->block.count && !c->had_error; i++) {
        compile_stmt(c, block->block.stmts[i]);
    }
}

// ---- Top-level API ----

static SeaChunk* compile_program(SeaNode *program, const char *source_name, bool is_main);

SeaChunk* sea_compile(SeaNode *program, const char *source_name) {
    return compile_program(program, source_name, false);
}

/* The program's main script: its top-level locals live until the process
 * exits, so they are not released at the end (freeing a large structure
 * object by object only delays the exit). Modules still release theirs. */
SeaChunk* sea_compile_main(SeaNode *program, const char *source_name) {
    return compile_program(program, source_name, true);
}

static SeaChunk* compile_program(SeaNode *program, const char *source_name, bool is_main) {
    if (!program) return NULL;

    g_compile_error[0] = '\0';
    g_compile_error_line = 0;

    SeaCompiler c;
    memset(&c, 0, sizeof(c));
    c.parent = NULL;
    c.chunk = sea_chunk_new(source_name ? source_name : "<main>");
    c.loop_body_local = -1;
    c.body = program;

    // Pre-scan for non-escaping struct literals (arena allocation)
    sea_scan_escapes(program, &c.escape_info);
    sea_collect_captured(program, &c.captured);

    // Compile the program block
    compile_block(&c, program);
    sea_free_captured(&c.captured);

    if (!c.had_error && !block_ends_with_return(program)) {
        // End of the program: release the top-level locals, return
        if (!is_main) emit_locals_cleanup(&c, 0, false, -1, 0);
        sea_chunk_emit(c.chunk, SEA_MAKE_ABC(OP_RETURN0, 0, 0, 0), 0);
    }

    // Free local names
    for (int i = 0; i < c.local_count; i++) {
        if (c.locals[i].name) sea_free(c.locals[i].name, strlen(c.locals[i].name)+1);
    }
    for (int i = 0; i < c.upval_count; i++) {
        if (c.upvalues[i].name) sea_free(c.upvalues[i].name, strlen(c.upvalues[i].name)+1);
    }
    if (c.break_list)    sea_free(c.break_list, sizeof(int) * (size_t)c.break_cap);
    if (c.continue_list) sea_free(c.continue_list, sizeof(int) * (size_t)c.continue_cap);

    if (c.had_error) {
        sea_chunk_free(c.chunk);
        return NULL;
    }

    sea_chunk_set_source(c.chunk);
    return c.chunk;
}
