/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_chunk.h"
#include "sea_string.h"
#include "sea_object.h"
#include "sea_memory.h"
#include "sea_jit.h"
#include <inttypes.h>

// ---- Chunk management ----

SeaChunk* sea_chunk_new(const char *name) {
    SeaChunk *c = SEA_ALLOC(SeaChunk);
    memset(c, 0, sizeof(*c));
    c->name = name ? (char*)sea_malloc(strlen(name)+1) : NULL;
    if (name) strcpy(c->name, name);
    return c;
}

static void set_source(SeaChunk *c, const char *src) {
    c->source = src;
    for (int i = 0; i < c->proto_len; i++) set_source(c->protos[i], src);
}

void sea_chunk_set_source(SeaChunk *root) {
    if (root) set_source(root, root->name);
}

struct SeaTableICSlot *sea_chunk_ics(SeaChunk *c) {
    struct SeaTableICSlot *ics = __atomic_load_n(&c->ics, __ATOMIC_ACQUIRE);
    if (ics) return ics;
    size_t n = c->code_len > 0 ? (size_t)c->code_len : 1;
    ics = (struct SeaTableICSlot*)sea_malloc(sizeof(*ics) * n);
    memset(ics, 0, sizeof(*ics) * n);
    struct SeaTableICSlot *expected = NULL;
    if (!__atomic_compare_exchange_n(&c->ics, &expected, ics, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        sea_free(ics, sizeof(*ics) * n);   /* another thread won */
        ics = expected;
    }
    return ics;
}

void sea_chunk_free(SeaChunk *c) {
    if (!c) return;
    sea_jit_release(c);  /* free any native code owned by this chunk */
    if (c->ics) {
        sea_free(c->ics, sizeof(*c->ics) * (size_t)(c->code_len > 0 ? c->code_len : 1));
        c->ics = NULL;
    }
    if (c->name) sea_free(c->name, strlen(c->name)+1);
    free(c->cover);

    if (c->code)  sea_free(c->code, sizeof(uint32_t) * (size_t)c->code_cap);
    if (c->lines) sea_free(c->lines, sizeof(int) * (size_t)c->code_cap);

    // Release object constants (strings owned by this chunk)
    for (int i = 0; i < c->const_len; i++) {
        if (sea_is_obj(c->constants[i])) {
            sea_obj_release(sea_as_obj(c->constants[i]));
        }
    }

    if (c->constants) sea_free(c->constants, sizeof(SeaValue) * (size_t)c->const_cap);

    for (int i = 0; i < c->proto_len; i++) {
        sea_chunk_free(c->protos[i]);
    }
    if (c->protos) sea_free(c->protos, sizeof(SeaChunk*) * (size_t)c->proto_cap);

    if (c->upvals) {
        for (int i = 0; i < c->upval_count; i++) {
            if (c->upvals[i].name)
                sea_free(c->upvals[i].name, strlen(c->upvals[i].name)+1);
        }
        sea_free(c->upvals, sizeof(SeaUpvalDesc) * (size_t)c->upval_count);
    }

    SEA_FREE(c, SeaChunk);
}

// ---- Constants ----

// Check if a constant already exists in the pool (to deduplicate)
static int find_const(SeaChunk *c, SeaValue val) {
    for (int i = 0; i < c->const_len; i++) {
        SeaValue existing = c->constants[i];
        if (sea_is_string(val) && sea_is_string(existing)) {
            if (sea_string_equal(SEA_AS_STRING(val), SEA_AS_STRING(existing)))
                return i;
        } else if (sea_val_equal(existing, val)) {
            return i;
        }
    }
    return -1;
}

int sea_chunk_add_const(SeaChunk *c, SeaValue val) {
    int existing = find_const(c, val);
    if (existing >= 0) return existing;

    if (c->const_len >= c->const_cap) {
        int old = c->const_cap;
        c->const_cap = old < 8 ? 8 : old * 2;
        c->constants = SEA_REALLOC_N(c->constants, SeaValue, old, c->const_cap);
    }
    c->constants[c->const_len] = val;
    return c->const_len++;
}

int sea_chunk_add_const_str(SeaChunk *c, const char *s) {
    return sea_chunk_add_const_strn(c, s, strlen(s));
}

/* As sea_chunk_add_const_str, for len bytes (a literal may hold NUL bytes). */
int sea_chunk_add_const_strn(SeaChunk *c, const char *s, size_t len) {
    // Intern the string and keep a reference alive for the lifetime of this chunk
    // by NOT releasing the ref from intern. The chunk "owns" this reference.
    SeaString *str = sea_string_intern(s, len);
    // Check if it's already in the constant pool
    SeaValue val = sea_obj_val(&str->obj);
    int existing = find_const(c, val);
    if (existing >= 0) {
        // Already in pool - release the extra ref from intern
        sea_obj_release(&str->obj);
        return existing;
    }
    // New entry - keep the ref (chunk owns it now). Pinned: loading a
    // constant into a local needs no retain (docs/memory.md).
    sea_obj_pin(&str->obj);
    if (c->const_len >= c->const_cap) {
        int old = c->const_cap;
        c->const_cap = old < 8 ? 8 : old * 2;
        c->constants = SEA_REALLOC_N(c->constants, SeaValue, old, c->const_cap);
    }
    c->constants[c->const_len] = val;
    return c->const_len++;
}

// ---- Code emission ----

void sea_chunk_emit(SeaChunk *c, uint32_t instr, int line) {
    if (c->code_len >= c->code_cap) {
        int old = c->code_cap;
        c->code_cap = old < 16 ? 16 : old * 2;
        c->code  = SEA_REALLOC_N(c->code, uint32_t, old, c->code_cap);
        c->lines = SEA_REALLOC_N(c->lines, int, old, c->code_cap);
    }
    c->code[c->code_len] = instr;
    c->lines[c->code_len] = line;
    c->code_len++;
    c->lines_len = c->code_len;
}

int sea_chunk_emit_jmp(SeaChunk *c, SeaOpcode op, int a, int line) {
    int pos = c->code_len;
    sea_chunk_emit(c, SEA_MAKE_AsBx(op, a, 0), line);
    return pos;
}

void sea_chunk_patch_jmp(SeaChunk *c, int pos, int target) {
    // sBx = target - (pos + 1)
    int offset = target - (pos + 1);
    uint32_t instr = c->code[pos];
    // Clear upper 16 bits
    instr = (instr & 0x0000FFFF) | ((uint32_t)(offset + 32767) << 16);
    c->code[pos] = instr;
}

int sea_chunk_pos(SeaChunk *c) {
    return c->code_len;
}

// ---- Serialization ----

static void write_int(FILE *f, int32_t v) {
    fwrite(&v, sizeof(v), 1, f);
}

static void write_uint(FILE *f, uint32_t v) {
    fwrite(&v, sizeof(v), 1, f);
}

static void write_uint64(FILE *f, uint64_t v) {
    fwrite(&v, sizeof(v), 1, f);
}

static int32_t read_int(FILE *f) {
    int32_t v;
    if (fread(&v, sizeof(v), 1, f) != 1) return 0;
    return v;
}

static uint32_t read_uint(FILE *f) {
    uint32_t v;
    if (fread(&v, sizeof(v), 1, f) != 1) return 0;
    return v;
}


#define SEA_BYTECODE_MAGIC 0x53454143  // "SEAC"
/* v2: constants are tagged (v1 wrote string constants as raw pointers into the
 * compiling process, so any v1 file with a string constant was unusable). */
#define SEA_BYTECODE_VERSION 3   /* 3: ownership opcodes (docs/memory.md) */

/* Constant tags in the constant pool (v2). */
#define SEA_CONST_RAW    0   /* non-object SeaValue (nil/bool/int/number), 8 bytes */
#define SEA_CONST_STRING 1   /* uint32 length + bytes; re-interned on load */

/* Upper bounds when loading, so a corrupt file fails cleanly instead of
 * attempting huge allocations. */
#define SEA_SEAC_MAX_COUNT  (1 << 24)
#define SEA_SEAC_MAX_STRLEN (1 << 28)

int sea_chunk_write(SeaChunk *c, FILE *f) {
    write_uint(f, SEA_BYTECODE_MAGIC);
    write_uint(f, SEA_BYTECODE_VERSION);
    // Name
    int name_len = c->name ? (int)strlen(c->name) : 0;
    write_int(f, name_len);
    if (name_len > 0) fwrite(c->name, 1, (size_t)name_len, f);
    // Header
    fwrite(&c->arity, 1, 1, f);
    uint8_t va = c->is_vararg ? 1 : 0;
    fwrite(&va, 1, 1, f);
    fwrite(&c->max_stack, 1, 1, f);
    // Code
    write_int(f, c->code_len);
    fwrite(c->code, sizeof(uint32_t), (size_t)c->code_len, f);
    fwrite(c->lines, sizeof(int), (size_t)c->code_len, f);
    // Constants: the compiler only produces non-object values and interned
    // strings (sea_chunk_add_const / sea_chunk_add_const_str).
    write_int(f, c->const_len);
    for (int i = 0; i < c->const_len; i++) {
        SeaValue v = c->constants[i];
        uint8_t tag;
        if (!sea_is_obj(v)) {
            tag = SEA_CONST_RAW;
            fwrite(&tag, 1, 1, f);
            write_uint64(f, v);
        } else if (sea_is_string(v)) {
            SeaString *str = SEA_AS_STRING(v);
            tag = SEA_CONST_STRING;
            fwrite(&tag, 1, 1, f);
            write_uint(f, str->len);
            fwrite(str->data, 1, str->len, f);
        } else {
            return -1;   /* no serialized form for this object constant */
        }
    }
    // Protos
    write_int(f, c->proto_len);
    for (int i = 0; i < c->proto_len; i++) {
        if (sea_chunk_write(c->protos[i], f) != 0) return -1;
    }
    // Upvalues
    write_int(f, c->upval_count);
    for (int i = 0; i < c->upval_count; i++) {
        uint8_t il = c->upvals[i].is_local ? 1 : 0;
        fwrite(&il, 1, 1, f);
        fwrite(&c->upvals[i].idx, 1, 1, f);
    }
    return 0;
}

/* Read a .seac chunk (and its nested protos). Returns NULL on a bad magic,
 * an unsupported version, or a truncated/corrupt file. */
SeaChunk* sea_chunk_read(FILE *f) {
    uint32_t magic = read_uint(f);
    if (magic != SEA_BYTECODE_MAGIC) return NULL;
    uint32_t version = read_uint(f);
    if (version != SEA_BYTECODE_VERSION) return NULL;

    int name_len = read_int(f);
    if (name_len < 0 || name_len > SEA_SEAC_MAX_STRLEN) return NULL;
    char *name = NULL;
    if (name_len > 0) {
        name = (char*)sea_malloc((size_t)name_len + 1);
        if (fread(name, 1, (size_t)name_len, f) != (size_t)name_len) {
            sea_free(name, (size_t)name_len+1);
            return NULL;
        }
        name[name_len] = '\0';
    }

    SeaChunk *c = sea_chunk_new(name);
    if (name) sea_free(name, (size_t)name_len+1);

    uint8_t va;
    if (fread(&c->arity, 1, 1, f) != 1 || fread(&va, 1, 1, f) != 1 ||
        fread(&c->max_stack, 1, 1, f) != 1)
        goto fail;
    c->is_vararg = va != 0;

    int code_len = read_int(f);
    if (code_len < 0 || code_len > SEA_SEAC_MAX_COUNT) goto fail;
    c->code_cap = code_len;
    c->code = SEA_ALLOC_N(uint32_t, code_len);
    c->lines = SEA_ALLOC_N(int, code_len);
    if (fread(c->code, sizeof(uint32_t), (size_t)code_len, f) != (size_t)code_len ||
        fread(c->lines, sizeof(int), (size_t)code_len, f) != (size_t)code_len)
        goto fail;
    c->code_len = c->lines_len = code_len;

    int const_len = read_int(f);
    if (const_len < 0 || const_len > SEA_SEAC_MAX_COUNT) goto fail;
    c->const_cap = const_len;
    c->constants = SEA_ALLOC_N(SeaValue, const_len);
    /* const_len grows as constants load, so a failure frees only real ones */
    for (int i = 0; i < const_len; i++) {
        uint8_t tag;
        if (fread(&tag, 1, 1, f) != 1) goto fail;
        if (tag == SEA_CONST_RAW) {
            uint64_t raw;
            if (fread(&raw, sizeof raw, 1, f) != 1) goto fail;
            if (sea_is_obj(raw)) goto fail;   /* a pointer cannot cross processes */
            c->constants[c->const_len++] = raw;
        } else if (tag == SEA_CONST_STRING) {
            uint32_t len;
            if (fread(&len, sizeof len, 1, f) != 1 || len > SEA_SEAC_MAX_STRLEN) goto fail;
            char *buf = (char*)sea_malloc((size_t)len + 1);
            if (fread(buf, 1, len, f) != len) { sea_free(buf, (size_t)len + 1); goto fail; }
            /* the chunk owns the reference returned by intern (as in
             * sea_chunk_add_const_str); sea_chunk_free releases it */
            SeaString *str = sea_string_intern(buf, len);
            sea_obj_pin(&str->obj);
            sea_free(buf, (size_t)len + 1);
            c->constants[c->const_len++] = sea_obj_val(&str->obj);
        } else {
            goto fail;
        }
    }

    int proto_len = read_int(f);
    if (proto_len < 0 || proto_len > SEA_SEAC_MAX_COUNT) goto fail;
    if (proto_len > 0) {
        c->proto_cap = proto_len;
        c->protos = SEA_ALLOC_N(SeaChunk*, proto_len);
        for (int i = 0; i < proto_len; i++) {
            SeaChunk *p = sea_chunk_read(f);
            if (!p) goto fail;
            c->protos[c->proto_len++] = p;
        }
    }

    int upval_count = read_int(f);
    if (upval_count < 0 || upval_count > 255) goto fail;
    if (upval_count > 0) {
        /* zeroed (names NULL) and counted up front: sea_chunk_free sizes the
         * free by upval_count, so it must match even on a failed read */
        c->upvals = SEA_ALLOC_N(SeaUpvalDesc, upval_count);
        memset(c->upvals, 0, sizeof(SeaUpvalDesc) * (size_t)upval_count);
        c->upval_count = upval_count;
        for (int i = 0; i < upval_count; i++) {
            uint8_t il;
            if (fread(&il, 1, 1, f) != 1 || fread(&c->upvals[i].idx, 1, 1, f) != 1)
                goto fail;
            c->upvals[i].is_local = il != 0;
        }
    }
    return c;

fail:
    sea_chunk_free(c);
    return NULL;
}

// ---- Disassembly ----

static void rr_add(SeaRegRanges *r, int lo, int hi) {
    if (hi < lo || r->n >= 3) return;
    if (lo < 0) lo = 0;
    if (hi > 255) hi = 255;
    r->lo[r->n] = lo; r->hi[r->n] = hi; r->n++;
}

void sea_op_reads(uint32_t in, SeaRegRanges *r) {
    r->n = 0;
    int a = SEA_INSTR_A(in), b = SEA_INSTR_B(in), c = SEA_INSTR_C(in);
    switch (SEA_INSTR_OP(in)) {
    /* no register reads */
    case OP_LOAD_NIL: case OP_LOAD_TRUE: case OP_LOAD_FALSE: case OP_LOAD_INT:
    case OP_LOAD_K: case OP_LOAD_GLOBAL: case OP_LOAD_UP: case OP_JMP:
    case OP_NEW_TABLE: case OP_NEW_STRUCT: case OP_NEW_MAP: case OP_NEW_STRUCT_LOCAL:
    case OP_NEW_ARRAY_LOCAL: case OP_RECOVER: case OP_YIELD: case OP_SELECT_BEGIN:
    case OP_RETURN0:
    case OP_CLOSURE:    /* captures named locals by address (never eliminated) */
    case OP_CLOSE_UP:   /* A is a bound; captured registers are named locals */
    case OP_SELECT_END: /* reads the registers its SELECT_CASEs named */
        break;

    case OP_VARARG:     /* pack mode (C=1) appends to the table/array in R[A] */
        if (c == 1) rr_add(r, a, a);
        break;

    /* R[B] */
    case OP_MOVE: case OP_NEG: case OP_UNM: case OP_ADDI: case OP_SUBI: case OP_BNOT:
    case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI: case OP_NOT: case OP_GET_FIELD:
    case OP_NEW_CHAN: case OP_CHAN_RECV: case OP_CHAN_TRY_RECV:
    case OP_NEW_ERROR: case OP_IS_ERROR: case OP_ERROR_MSG:
    case OP_TYPE: case OP_TOSTRING: case OP_TONUMBER:
        rr_add(r, b, b); break;
    case OP_NEW_ARRAY:  /* capacity from R[B] when B != A */
        if (b != a) rr_add(r, b, b);
        break;

    /* R[B], R[C] */
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_IDIV: case OP_MOD:
    case OP_POW: case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT: case OP_IDIV_INT:
    case OP_MOD_INT: case OP_IIADD: case OP_IISUB: case OP_BAND: case OP_BOR:
    case OP_BXOR: case OP_SHL: case OP_SHR: case OP_EQ: case OP_NEQ: case OP_LT:
    case OP_LE: case OP_GT: case OP_GE: case OP_GET_TABLE: case OP_GET_TABLE2:
    case OP_GET_ARRAY: case OP_GET_IARRAY: case OP_GET_FARRAY:
        rr_add(r, b, b); rr_add(r, c, c); break;

    /* R[A] */
    case OP_STORE_GLOBAL: case OP_STORE_UP: case OP_JMP_TRUE: case OP_JMP_FALSE:
    case OP_JMP_NIL: case OP_CHAN_CLOSE: case OP_PANIC: case OP_PROPAGATE:
    case OP_RELEASE: case OP_FOR_IN_INIT: case OP_MARK_CAPTURED:
    case OP_RETAIN: case OP_JMP_NOT_ERR:
        rr_add(r, a, a); break;
    case OP_LOAD_SELF: case OP_COVER:
        break;

    /* R[A], R[B] */
    case OP_CHAN_SEND: case OP_CHAN_TRY_SEND: case OP_ARRAY_PUSH: case OP_DEL:
    case OP_ARRAY_PUSH_MOVE:
    case OP_SELECT_CASE:
    case OP_SETLOCAL:   /* the new value R[B]; the old R[A] is released */
    case OP_APPEND:
        rr_add(r, a, a); rr_add(r, b, b); break;
    case OP_ASSERT:     /* message register only when B > 0 */
        rr_add(r, a, a); if (b > 0) rr_add(r, b, b);
        break;

    /* R[A], R[C] */
    case OP_SET_FIELD: case OP_SET_FIELD_BORROW: case OP_SET_FIELD_MOVE:
        rr_add(r, a, a); rr_add(r, c, c); break;

    /* R[A], R[B], R[C] */
    case OP_SET_TABLE: case OP_SET_ARRAY: case OP_SET_IARRAY: case OP_SET_FARRAY:
        rr_add(r, a, a); rr_add(r, b, b); rr_add(r, c, c); break;

    /* ranges */
    case OP_CONCAT:                       /* R[B..B+C-1] (and R[A] when A == B) */
        rr_add(r, b, b + c - 1); rr_add(r, a, a); break;
    case OP_CALL: case OP_CALL_SELF: case OP_CALL_MULTI: case OP_TAIL_CALL:
        rr_add(r, a, c == 255 ? 255 : a + c); break;   /* fn + args (255: varargs) */
    case OP_RETURN: case OP_PRINT:        /* R[A..A+B-1] */
        rr_add(r, a, b == 255 ? 255 : a + b - 1); break;
    case OP_DEFER:                        /* fn + B args */
        rr_add(r, a, a + b); break;
    case OP_GO:                           /* fn R[A], args R[B..B+C-1] */
        rr_add(r, a, a); rr_add(r, b, b + c - 1); break;
    case OP_FOR_PREP: case OP_FOR_LOOP: case OP_FOR_LOOP_STEP1: case OP_FOR_LOOP_INT:
        rr_add(r, a, a + 2); break;       /* idx, limit, step */
    case OP_FOR_IN_NEXT:                  /* container, cursor, and the B loop
                                             variables (their old values are released) */
        rr_add(r, a, a + 1 + b); break;
    default:
        /* unknown/new opcode: assume it reads A, B and C (safe, may miss a
         * dead write but never drops a live one) */
        rr_add(r, a, a); rr_add(r, b, b); rr_add(r, c, c); break;
    }
}

const char* sea_opcode_name(SeaOpcode op) {
    static const char *names[OP_COUNT] = {
        [OP_LOAD_NIL]    = "LOAD_NIL",
        [OP_GET_TABLE2]  = "GET_TABLE2",
        [OP_NEW_MAP]     = "NEW_MAP",
        [OP_DEL]         = "DEL",
        [OP_MARK_CAPTURED] = "MARK_CAPTURED",
        [OP_RETAIN]      = "RETAIN",
        [OP_SETLOCAL]    = "SETLOCAL",
        [OP_APPEND]      = "APPEND",
        [OP_JMP_NOT_ERR] = "JMP_NOT_ERR",
        [OP_LOAD_SELF]   = "LOAD_SELF",
        [OP_SET_FIELD_MOVE]  = "SET_FIELD_MOVE",
        [OP_ARRAY_PUSH_MOVE] = "ARRAY_PUSH_MOVE",
        [OP_COVER]       = "COVER",
        [OP_LOAD_TRUE]   = "LOAD_TRUE",
        [OP_LOAD_FALSE]  = "LOAD_FALSE",
        [OP_LOAD_INT]    = "LOAD_INT",
        [OP_LOAD_K]      = "LOAD_K",
        [OP_MOVE]        = "MOVE",
        [OP_LOAD_GLOBAL] = "LOAD_GLOBAL",
        [OP_STORE_GLOBAL]= "STORE_GLOBAL",
        [OP_LOAD_UP]     = "LOAD_UP",
        [OP_STORE_UP]    = "STORE_UP",
        [OP_CLOSE_UP]    = "CLOSE_UP",
        [OP_ADD]         = "ADD",
        [OP_SUB]         = "SUB",
        [OP_MUL]         = "MUL",
        [OP_DIV]         = "DIV",
        [OP_IDIV]        = "IDIV",
        [OP_MOD]         = "MOD",
        [OP_POW]         = "POW",
        [OP_NEG]         = "NEG",
        [OP_UNM]         = "UNM",
        [OP_ADD_INT]     = "ADD_INT",
        [OP_SUB_INT]     = "SUB_INT",
        [OP_MUL_INT]     = "MUL_INT",
        [OP_IDIV_INT]    = "IDIV_INT",
        [OP_MOD_INT]     = "MOD_INT",
        [OP_IIADD]       = "IIADD",
        [OP_IISUB]       = "IISUB",
        [OP_ADDI]        = "ADDI",
        [OP_SUBI]        = "SUBI",
        [OP_BAND]        = "BAND",
        [OP_BOR]         = "BOR",
        [OP_BXOR]        = "BXOR",
        [OP_BNOT]        = "BNOT",
        [OP_SHL]         = "SHL",
        [OP_SHR]         = "SHR",
        [OP_CONCAT]      = "CONCAT",
        [OP_EQ]          = "EQ",
        [OP_NEQ]         = "NEQ",
        [OP_LT]          = "LT",
        [OP_LE]          = "LE",
        [OP_GT]          = "GT",
        [OP_GE]          = "GE",
        [OP_NOT]         = "NOT",
        [OP_LTI]         = "LTI",
        [OP_LEI]         = "LEI",
        [OP_GTI]         = "GTI",
        [OP_GEI]         = "GEI",
        [OP_JMP]         = "JMP",
        [OP_JMP_TRUE]    = "JMP_TRUE",
        [OP_JMP_FALSE]   = "JMP_FALSE",
        [OP_JMP_NIL]     = "JMP_NIL",
        [OP_NEW_TABLE]   = "NEW_TABLE",
        [OP_GET_TABLE]   = "GET_TABLE",
        [OP_SET_TABLE]   = "SET_TABLE",
        [OP_GET_FIELD]   = "GET_FIELD",
        [OP_SET_FIELD]   = "SET_FIELD",
        [OP_NEW_STRUCT]  = "NEW_STRUCT",
        [OP_NEW_ARRAY]   = "NEW_ARRAY",
        [OP_GET_ARRAY]   = "GET_ARRAY",
        [OP_SET_ARRAY]   = "SET_ARRAY",
        [OP_ARRAY_PUSH]  = "ARRAY_PUSH",
        [OP_GET_IARRAY]  = "GET_IARRAY",
        [OP_SET_IARRAY]  = "SET_IARRAY",
        [OP_GET_FARRAY]  = "GET_FARRAY",
        [OP_SET_FARRAY]  = "SET_FARRAY",
        [OP_CLOSURE]     = "CLOSURE",
        [OP_CALL]        = "CALL",
        [OP_CALL_SELF]   = "CALL_SELF",
        [OP_CALL_MULTI]  = "CALL_MULTI",
        [OP_RETURN]      = "RETURN",
        [OP_RETURN0]     = "RETURN0",
        [OP_TAIL_CALL]   = "TAIL_CALL",
        [OP_VARARG]      = "VARARG",
        [OP_FOR_PREP]        = "FOR_PREP",
        [OP_FOR_LOOP]        = "FOR_LOOP",
        [OP_FOR_LOOP_STEP1]  = "FOR_LOOP_STEP1",
        [OP_FOR_LOOP_INT]    = "FOR_LOOP_INT",
        [OP_FOR_IN_INIT]     = "FOR_IN_INIT",
        [OP_FOR_IN_NEXT] = "FOR_IN_NEXT",
        [OP_NEW_CHAN]     = "NEW_CHAN",
        [OP_CHAN_SEND]    = "CHAN_SEND",
        [OP_CHAN_RECV]    = "CHAN_RECV",
        [OP_CHAN_CLOSE]   = "CHAN_CLOSE",
        [OP_CHAN_TRY_SEND]= "CHAN_TRY_SEND",
        [OP_CHAN_TRY_RECV]= "CHAN_TRY_RECV",
        [OP_GO]           = "GO",
        [OP_YIELD]        = "YIELD",
        [OP_SELECT_BEGIN] = "SELECT_BEGIN",
        [OP_SELECT_CASE]  = "SELECT_CASE",
        [OP_SELECT_END]   = "SELECT_END",
        [OP_NEW_ERROR]    = "NEW_ERROR",
        [OP_IS_ERROR]     = "IS_ERROR",
        [OP_ERROR_MSG]    = "ERROR_MSG",
        [OP_PROPAGATE]    = "PROPAGATE",
        [OP_DEFER]        = "DEFER",
        [OP_PANIC]        = "PANIC",
        [OP_RECOVER]      = "RECOVER",
        [OP_TYPE]         = "TYPE",
        [OP_TOSTRING]     = "TOSTRING",
        [OP_TONUMBER]     = "TONUMBER",
        [OP_PRINT]        = "PRINT",
        [OP_ASSERT]       = "ASSERT",
        [OP_RELEASE]          = "RELEASE",
        [OP_NEW_STRUCT_LOCAL] = "NEW_STRUCT_LOCAL",
        [OP_SET_FIELD_BORROW] = "SET_FIELD_BORROW",
        [OP_NEW_ARRAY_LOCAL]  = "NEW_ARRAY_LOCAL",
    };
    if (op < OP_COUNT && names[op]) return names[op];
    return "???";
}

void sea_chunk_disasm_instr(SeaChunk *c, int idx, FILE *f) {
    uint32_t i = c->code[idx];
    SeaOpcode op = SEA_INSTR_OP(i);
    int a = SEA_INSTR_A(i);
    int b = SEA_INSTR_B(i);
    int bx = SEA_INSTR_Bx(i);
    int sbx = SEA_INSTR_sBx(i);
    int cc = SEA_INSTR_C(i);
    int line = (idx < c->lines_len) ? c->lines[idx] : -1;

    fprintf(f, "%4d  [%4d]  %-16s  ", idx, line, sea_opcode_name(op));

    switch (op) {
        case OP_LOAD_NIL:
            fprintf(f, "R[%d..%d] = nil\n", a, a+b-1);
            break;
        case OP_LOAD_INT:
            fprintf(f, "R[%d] = %d\n", a, sbx);
            break;
        case OP_LOAD_K:
            fprintf(f, "R[%d] = K[%d]\n", a, bx);
            break;
        case OP_LOAD_GLOBAL:
        case OP_STORE_GLOBAL:
            if (sea_is_string(c->constants[bx])) {
                fprintf(f, "R[%d] <-> G[\"%s\"]\n", a, sea_string_cstr(SEA_AS_STRING(c->constants[bx])));
            } else {
                fprintf(f, "R[%d] <-> G[K[%d]]\n", a, bx);
            }
            break;
        case OP_MOVE:
            fprintf(f, "R[%d] = R[%d]\n", a, b);
            break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_IDIV: case OP_MOD: case OP_POW:
        case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
        case OP_EQ: case OP_NEQ: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
        case OP_GET_TABLE: case OP_GET_ARRAY:
            fprintf(f, "R[%d] = R[%d] op R[%d]\n", a, b, cc);
            break;
        case OP_NEG: case OP_UNM: case OP_NOT: case OP_BNOT:
        case OP_TOSTRING: case OP_TONUMBER: case OP_TYPE: case OP_IS_ERROR:
        case OP_NEW_ERROR: case OP_ERROR_MSG: case OP_PROPAGATE: case OP_CHAN_RECV:
            fprintf(f, "R[%d] = op R[%d]\n", a, b);
            break;
        case OP_RELEASE:
            fprintf(f, "release R[%d]\n", a);
            break;
        case OP_RETAIN:
            fprintf(f, "retain R[%d]\n", a);
            break;
        case OP_SETLOCAL:
            fprintf(f, "R[%d] = R[%d]%s (release old)\n", a, b, cc ? " (retain)" : "");
            break;
        case OP_APPEND:
            fprintf(f, "R[%d] = R[%d] .. R[%d]\n", a, a, b);
            break;
        case OP_LOAD_SELF:
            fprintf(f, "R[%d] = self\n", a);
            break;
        case OP_COVER:
            fprintf(f, "line %d ran\n", bx);
            break;
        case OP_JMP:
            fprintf(f, "PC += %d  (-> %d)\n", sbx, idx+1+sbx);
            break;
        case OP_JMP_TRUE: case OP_JMP_FALSE: case OP_JMP_NIL: case OP_JMP_NOT_ERR:
            fprintf(f, "if R[%d] PC += %d  (-> %d)\n", a, sbx, idx+1+sbx);
            break;
        case OP_CALL:
            fprintf(f, "R[%d..%d] = R[%d](R[%d..%d])\n", a, a+b-1, a, a+1, a+cc);
            break;
        case OP_CALL_SELF:
            fprintf(f, "R[%d..%d] = self(R[%d..%d])\n", a, a+b-1, a+1, a+cc);
            break;
        case OP_RETURN:
            fprintf(f, "return R[%d..%d]\n", a, a+b-1);
            break;
        case OP_CLOSURE:
            fprintf(f, "R[%d] = closure(proto[%d])\n", a, bx);
            break;
        case OP_GET_FIELD:
            if (bx < c->const_len && sea_is_string(c->constants[bx])) {
                fprintf(f, "R[%d] = R[%d].\"%s\"\n", a, b, sea_string_cstr(SEA_AS_STRING(c->constants[bx])));
            } else {
                fprintf(f, "R[%d] = R[%d].K[%d]\n", a, b, bx);
            }
            break;
        case OP_SET_FIELD:
            if (b < c->const_len && sea_is_string(c->constants[b])) {
                fprintf(f, "R[%d].\"%s\" = R[%d]\n", a, sea_string_cstr(SEA_AS_STRING(c->constants[b])), cc);
            } else {
                fprintf(f, "R[%d].K[%d] = R[%d]\n", a, b, cc);
            }
            break;
        default:
            fprintf(f, "A=%d B=%d C=%d Bx=%d sBx=%d\n", a, b, cc, bx, sbx);
            break;
    }
}

void sea_chunk_disasm(SeaChunk *c, FILE *f) {
    fprintf(f, "=== chunk '%s' (arity=%d, vararg=%d, maxstack=%d) ===\n",
            c->name ? c->name : "?", c->arity, c->is_vararg, c->max_stack);
    fprintf(f, "Constants (%d):\n", c->const_len);
    for (int i = 0; i < c->const_len; i++) {
        SeaValue v = c->constants[i];
        fprintf(f, "  K[%d] = ", i);
        if (sea_is_nil(v))    fprintf(f, "nil\n");
        else if (sea_is_bool(v)) fprintf(f, "%s\n", sea_as_bool(v) ? "true" : "false");
        else if (sea_is_int(v))  fprintf(f, "%" PRId64 "\n", sea_as_int(v));
        else if (sea_is_number(v)) fprintf(f, "%.14g\n", sea_as_number(v));
        else if (sea_is_string(v)) fprintf(f, "\"%s\"\n", sea_string_cstr(SEA_AS_STRING(v)));
        else fprintf(f, "<obj>\n");
    }
    fprintf(f, "Code (%d instructions):\n", c->code_len);
    for (int i = 0; i < c->code_len; i++) {
        sea_chunk_disasm_instr(c, i, f);
    }
    for (int i = 0; i < c->proto_len; i++) {
        fprintf(f, "\n-- proto[%d] --\n", i);
        sea_chunk_disasm(c->protos[i], f);
    }
}
