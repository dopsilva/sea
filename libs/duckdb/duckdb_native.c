/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * libs/duckdb/duckdb_native.c — DuckDB bindings for Sea (used by duckdb.sea).
 *
 * Build:  make -C libs/duckdb   (fetches libduckdb into vendor/ the first time)
 *
 * Handles: databases, connections, prepared statements and appenders are
 * ints that index a registry (slot + generation), so a closed or made-up
 * handle is an error, never a crash. Closing a database closes its
 * connections; closing a connection closes its statements and appenders.
 * The DuckDB objects are destroyed when the last in-flight query using them
 * finishes.
 *
 * Queries: each connection is used by one thread at a time (a mutex). In a
 * goroutine a query runs on a worker thread and only that goroutine parks
 * (sea_netpoll.h: an eventfd wakes it); on the main thread it runs in place.
 * The result is turned into Sea values on the calling thread.
 *
 * Values: integers are exact: int within Sea's ±2^47, number up to ±2^53,
 * bigint beyond (BIGNUM is always a bigint); floats and decimals → number,
 * VARCHAR/BLOB → string, dates and times → ISO strings, INTERVAL → table,
 * UUID → string, ENUM → string, LIST/ARRAY → array, STRUCT → table,
 * MAP → map, UNION → the member's value, GEOMETRY → a GeoJSON-shaped table,
 * VARIANT → whatever value it holds, NULL → nil.
 */

#include "sea.h"
#include "sea_rope.h"
#include "sea_netpoll.h"
#include "duckdb.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sea_compat.h"
#include <unistd.h>

#define SEA_INT_MAX ((INT64_C(1) << 47) - 1)
#define SEA_INT_MIN (-(INT64_C(1) << 47))

static inline void val_release(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

static SeaValue str_val(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }

static SeaValue err_val(SeaVM *vm, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return sea_make_error_str(vm, msg);
}

static char *dupstr(const char *s) {
    if (!s) s = "unknown error";
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static SeaValue int_or_number(int64_t v) {
    if (v >= SEA_INT_MIN && v <= SEA_INT_MAX) return sea_int_val(v);
    return sea_number_val((double)v);
}

/* Integers keep every digit: an int within Sea's range, a number while a
 * double holds the value exactly (|v| <= 2^53), a bigint beyond. */
static SeaValue exact_i64(int64_t v) {
    if (v >= SEA_INT_MIN && v <= SEA_INT_MAX) return sea_int_val(v);
    if (v >= -(INT64_C(1) << 53) && v <= (INT64_C(1) << 53)) return sea_number_val((double)v);
    return sea_obj_val(&sea_bigint_from_i64(v)->obj);
}

static SeaValue exact_u64(uint64_t v) {
    if (v <= (uint64_t)INT64_MAX) return exact_i64((int64_t)v);
    return sea_obj_val(&sea_bigint_from_u64(v)->obj);
}

static SeaValue exact_u128(uint64_t upper, uint64_t lower, bool neg) {
    uint8_t be[16];
    for (int i = 0; i < 8; i++) {
        be[7 - i] = (uint8_t)(upper >> (8 * i));
        be[15 - i] = (uint8_t)(lower >> (8 * i));
    }
    return sea_obj_val(&sea_bigint_from_bytes(be, 16, neg)->obj);
}

static SeaValue exact_i128(int64_t upper, uint64_t lower) {
    if ((upper == 0 && lower <= (uint64_t)INT64_MAX) || (upper == -1 && lower > (uint64_t)INT64_MAX))
        return exact_i64((int64_t)lower);
    unsigned __int128 x = ((unsigned __int128)(uint64_t)upper << 64) | lower;
    bool neg = upper < 0;
    if (neg) x = (unsigned __int128)0 - x;
    return exact_u128((uint64_t)(x >> 64), (uint64_t)x, neg);
}

/* A BIGNUM's storage: 3 header bytes (top bit set = positive, then the
 * byte count) and the magnitude big-endian; a negative one has every byte
 * inverted. */
static SeaValue bignum_val(const uint8_t *d, size_t n) {
    if (n < 3) return SEA_NIL_VAL;
    bool neg = !(d[0] & 0x80);
    uint8_t *t = malloc(n);
    if (!t) return SEA_NIL_VAL;
    for (size_t i = 0; i < n; i++) t[i] = neg ? (uint8_t)~d[i] : d[i];
    size_t size = ((size_t)(t[0] & 0x7F) << 16) | ((size_t)t[1] << 8) | t[2];
    if (size > n - 3) size = n - 3;
    SeaValue r = sea_obj_val(&sea_bigint_from_bytes(t + 3, size, neg)->obj);
    free(t);
    return r;
}

/* Borrowed field of a table, map or struct (nil when absent). */
static SeaValue field(SeaValue o, const char *name) {
    SeaValue out = SEA_NIL_VAL;
    if (!sea_is_table(o) && !sea_is_map(o) && !sea_is_struct(o)) return out;
    SeaString *k = sea_string_intern(name, strlen(name));
    if (sea_is_table(o)) sea_table_get(SEA_AS_TABLE(o), sea_obj_val(&k->obj), &out);
    else if (sea_is_map(o)) sea_map_get(SEA_AS_MAP(o), sea_obj_val(&k->obj), &out);
    else out = sea_struct_get(SEA_AS_STRUCT(o), k);
    sea_obj_release(&k->obj);
    return out;
}

/* Iterate a table, map or struct. */
static bool kv_next(SeaValue o, uint32_t *it, SeaValue *k, SeaValue *v) {
    if (sea_is_table(o)) return sea_table_next(SEA_AS_TABLE(o), it, k, v);
    if (sea_is_map(o)) return sea_map_next(SEA_AS_MAP(o), it, k, v);
    if (sea_is_struct(o)) return sea_struct_next(SEA_AS_STRUCT(o), it, k, v);
    return false;
}

/* A table whose keys are exactly 0..n-1 (a positional literal {a, b}). */
static bool is_seq_table(SeaValue v) {
    if (!sea_is_table(v)) return false;
    SeaTable *t = SEA_AS_TABLE(v);
    uint32_t n = sea_table_count(t), it = 0;
    if (!n) return false;
    SeaValue k, x;
    while (sea_table_next(t, &it, &k, &x)) {
        if (!sea_is_int(k) || sea_as_int(k) < 0 || (uint64_t)sea_as_int(k) >= n) return false;
    }
    return true;
}

/* Sequence views: arrays, typed arrays and positional tables. */
static bool is_seq(SeaValue v) {
    return sea_is_array(v) || sea_is_int_array(v) || sea_obj_is_type(v, OBJ_FLOAT_ARRAY) || is_seq_table(v);
}

static uint32_t seq_len(SeaValue v) {
    if (sea_is_array(v)) return sea_array_len(SEA_AS_ARRAY(v));
    if (sea_is_int_array(v)) return ((SeaIntArray *)sea_as_obj(v))->len;
    if (sea_obj_is_type(v, OBJ_FLOAT_ARRAY)) return ((SeaFloatArray *)sea_as_obj(v))->len;
    return sea_table_count(SEA_AS_TABLE(v));
}

/* Element i (borrowed, or an unboxed number). */
static SeaValue seq_at(SeaValue v, uint32_t i) {
    if (sea_is_array(v)) return sea_array_get(SEA_AS_ARRAY(v), i);
    if (sea_is_int_array(v)) return int_or_number(((SeaIntArray *)sea_as_obj(v))->data[i]);
    if (sea_obj_is_type(v, OBJ_FLOAT_ARRAY)) return sea_number_val(((SeaFloatArray *)sea_as_obj(v))->data[i]);
    SeaValue x = SEA_NIL_VAL;
    sea_table_get(SEA_AS_TABLE(v), sea_int_val(i), &x);
    return x;
}

/* ======================================================================== */
/* Handle registry                                                          */
/* ======================================================================== */

typedef enum { O_DB = 1, O_CONN, O_STMT, O_APP } OKind;

typedef struct Obj Obj;
struct Obj {
    OKind             kind;
    atomic_int        refs;            /* the registry's (while open) + in-flight uses */
    bool              closed;          /* under g_reg */
    uint32_t          slot;
    Obj              *parent;          /* holds a reference */
    Obj              *child, *sib;     /* open children, under g_reg */
    pthread_mutex_t   mu;              /* connection: one thread at a time */
    duckdb_database   db;
    duckdb_connection con;
    duckdb_prepared_statement stmt;
    duckdb_appender   app;
    idx_t             ncols;           /* appender: the table's columns */
    char            **colnames;
};

#define SLOT_BITS 20
#define MAX_SLOTS (1u << SLOT_BITS)

static pthread_mutex_t g_reg = PTHREAD_MUTEX_INITIALIZER;
static Obj      **g_slots;
static uint32_t  *g_gens;
static uint32_t   g_nslots, g_cap;
static uint32_t  *g_free;
static uint32_t   g_nfree;

static Obj *conn_of(Obj *o) { return o->kind == O_CONN ? o : o->parent; }

static void obj_destroy(Obj *o);

static void obj_unref(Obj *o) {
    if (o && atomic_fetch_sub(&o->refs, 1) == 1) obj_destroy(o);
}

static void obj_destroy(Obj *o) {
    switch (o->kind) {
        case O_APP: {
            Obj *c = conn_of(o);
            pthread_mutex_lock(&c->mu);
            duckdb_appender_destroy(&o->app);
            pthread_mutex_unlock(&c->mu);
            for (idx_t i = 0; i < o->ncols; i++) free(o->colnames[i]);
            free(o->colnames);
            break;
        }
        case O_STMT: {
            Obj *c = conn_of(o);
            pthread_mutex_lock(&c->mu);
            duckdb_destroy_prepare(&o->stmt);
            pthread_mutex_unlock(&c->mu);
            break;
        }
        case O_CONN:
            duckdb_disconnect(&o->con);
            pthread_mutex_destroy(&o->mu);
            break;
        case O_DB:
            duckdb_close(&o->db);
            break;
    }
    Obj *p = o->parent;
    free(o);
    obj_unref(p);
}

/* Register o (refs = 1, the registry's) under parent; returns the handle. */
static int64_t obj_register(Obj *o, Obj *parent) {
    atomic_init(&o->refs, 1);
    pthread_mutex_lock(&g_reg);
    uint32_t slot;
    if (g_nfree) {
        slot = g_free[--g_nfree];
    } else {
        if (g_nslots == g_cap) {
            uint32_t nc = g_cap ? g_cap * 2 : 64;
            if (nc > MAX_SLOTS) nc = MAX_SLOTS;
            Obj **ns = realloc(g_slots, nc * sizeof *ns);
            uint32_t *ng = ns ? realloc(g_gens, nc * sizeof *ng) : NULL;
            uint32_t *nf = ng ? realloc(g_free, nc * sizeof *nf) : NULL;
            if (ns) g_slots = ns;
            if (ng) g_gens = ng;
            if (nf) g_free = nf;
            if (!ns || !ng || !nf || g_nslots == nc) { pthread_mutex_unlock(&g_reg); return -1; }
            g_cap = nc;
        }
        slot = g_nslots++;
        g_gens[slot] = 0;
    }
    g_gens[slot] = (g_gens[slot] + 1) & 0x3FFFFFF;
    if (!g_gens[slot]) g_gens[slot] = 1;
    g_slots[slot] = o;
    o->slot = slot;
    if (parent) {
        atomic_fetch_add(&parent->refs, 1);
        o->parent = parent;
        o->sib = parent->child;
        parent->child = o;
    }
    int64_t h = ((int64_t)g_gens[slot] << SLOT_BITS) | slot;
    pthread_mutex_unlock(&g_reg);
    return h;
}

/* The open object behind handle h (any of the kinds in mask), with a
 * reference the caller drops with obj_unref; NULL if closed or invalid. */
static Obj *obj_get(SeaValue h, unsigned mask) {
    if (!sea_is_int(h)) return NULL;
    int64_t v = sea_as_int(h);
    if (v <= 0) return NULL;
    uint32_t slot = (uint32_t)(v & (MAX_SLOTS - 1)), gen = (uint32_t)(v >> SLOT_BITS);
    Obj *o = NULL;
    pthread_mutex_lock(&g_reg);
    if (slot < g_nslots && g_gens[slot] == gen && g_slots[slot] && (mask & (1u << g_slots[slot]->kind))) {
        o = g_slots[slot];
        atomic_fetch_add(&o->refs, 1);
    }
    pthread_mutex_unlock(&g_reg);
    return o;
}

#define M(k) (1u << (k))

/* Close o and its open children (under g_reg); collects them in out. */
static void close_locked(Obj *o, Obj ***out, size_t *n, size_t *cap) {
    if (o->closed) return;
    o->closed = true;
    g_slots[o->slot] = NULL;
    g_free[g_nfree++] = o->slot;
    if (o->parent) {
        Obj **pp = &o->parent->child;
        while (*pp && *pp != o) pp = &(*pp)->sib;
        if (*pp) *pp = o->sib;
    }
    while (o->child) close_locked(o->child, out, n, cap);
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        Obj **na = realloc(*out, *cap * sizeof *na);
        if (!na) return;                 /* leak rather than crash */
        *out = na;
    }
    (*out)[(*n)++] = o;
}

static void obj_close(Obj *o) {
    Obj **list = NULL;
    size_t n = 0, cap = 0;
    pthread_mutex_lock(&g_reg);
    close_locked(o, &list, &n, &cap);
    pthread_mutex_unlock(&g_reg);
    /* stop queries still running on the connections being closed */
    for (size_t i = 0; i < n; i++)
        if (list[i]->kind == O_CONN) duckdb_interrupt(list[i]->con);
    for (size_t i = 0; i < n; i++) obj_unref(list[i]);   /* the registry's references */
    free(list);
}

static SeaValue closed_err(SeaVM *vm, const char *fn, const char *what) {
    return err_val(vm, "duckdb.%s: the %s is closed (or not a %s)", fn, what, what);
}

/* ======================================================================== */
/* Sea values → DuckDB values (parameters, appender)                        */
/* ======================================================================== */

typedef struct { char msg[256]; } VErr;

static duckdb_value to_dvalue(SeaVM *vm, SeaValue v, VErr *e, int depth);

/* A list value; its element type is the elements' common type. */
static duckdb_value to_dlist(SeaVM *vm, SeaValue v, VErr *e, int depth) {
    uint32_t n = seq_len(v);
    duckdb_value *items = calloc(n ? n : 1, sizeof *items);
    if (!items) { snprintf(e->msg, sizeof e->msg, "out of memory"); return NULL; }
    /* numbers: all ints → BIGINT, any float → DOUBLE */
    bool any_float = false, all_num = n > 0;
    for (uint32_t i = 0; i < n; i++) {
        SeaValue x = seq_at(v, i);
        if (sea_is_number(x)) any_float = true;
        else if (!sea_is_int(x) && !sea_is_nil(x)) all_num = false;
    }
    duckdb_logical_type et = NULL;
    duckdb_value r = NULL;
    uint32_t made = 0;
    for (; made < n; made++) {
        SeaValue x = seq_at(v, made);
        if (all_num && any_float && sea_is_int(x)) items[made] = duckdb_create_double((double)sea_as_int(x));
        else items[made] = to_dvalue(vm, x, e, depth + 1);
        if (!items[made]) goto done;
        if (!et && !sea_is_nil(x)) et = duckdb_get_value_type(items[made]);
    }
    if (!et) {
        duckdb_logical_type t = duckdb_create_logical_type(DUCKDB_TYPE_SQLNULL);
        r = duckdb_create_list_value(t, items, n);
        duckdb_destroy_logical_type(&t);
    } else {
        /* the value-owned type: create_list_value copies it */
        r = duckdb_create_list_value(et, items, n);
    }
    if (!r) snprintf(e->msg, sizeof e->msg, "list elements must all have the same type");
done:
    for (uint32_t i = 0; i < made; i++) duckdb_destroy_value(&items[i]);
    free(items);
    return r;
}

static int cmp_cstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* A struct value from a table/struct with string keys (members by name). */
static duckdb_value to_dstruct(SeaVM *vm, SeaValue v, VErr *e, int depth) {
    uint32_t it = 0, n = 0;
    SeaValue k, x;
    while (kv_next(v, &it, &k, &x)) n++;
    if (!n) { snprintf(e->msg, sizeof e->msg, "an empty table cannot be a STRUCT"); return NULL; }
    char **names = calloc(n, sizeof *names);
    duckdb_value *vals = calloc(n, sizeof *vals);
    duckdb_logical_type *types = calloc(n, sizeof *types);
    duckdb_value r = NULL;
    uint32_t m = 0;
    if (!names || !vals || !types) { snprintf(e->msg, sizeof e->msg, "out of memory"); goto done; }
    it = 0;
    while (kv_next(v, &it, &k, &x)) {
        if (!sea_is_string(k)) { snprintf(e->msg, sizeof e->msg, "STRUCT keys must be strings"); goto done; }
        names[m++] = SEA_AS_STRING(k)->data;
    }
    qsort(names, m, sizeof *names, cmp_cstr);
    for (uint32_t i = 0; i < m; i++) {
        vals[i] = to_dvalue(vm, field(v, names[i]), e, depth + 1);
        if (!vals[i]) { m = i; goto done_vals; }
        types[i] = duckdb_get_value_type(vals[i]);
    }
    {
        duckdb_logical_type st = duckdb_create_struct_type(types, (const char **)names, m);
        r = duckdb_create_struct_value(st, vals);
        duckdb_destroy_logical_type(&st);
        if (!r) snprintf(e->msg, sizeof e->msg, "could not build a STRUCT value");
    }
done_vals:
    for (uint32_t i = 0; i < m; i++) duckdb_destroy_value(&vals[i]);
done:
    free(names);
    free(vals);
    free(types);
    return r;
}

/* A map value from a Sea map [=]. */
static duckdb_value to_dmap(SeaVM *vm, SeaValue v, VErr *e, int depth) {
    SeaMap *mp = SEA_AS_MAP(v);
    uint32_t n = sea_map_count(mp), it = 0, m = 0;
    if (!n) { snprintf(e->msg, sizeof e->msg, "an empty map has no type; use a typed NULL or cast in SQL"); return NULL; }
    duckdb_value *ks = calloc(n, sizeof *ks), *vs = calloc(n, sizeof *vs);
    duckdb_value r = NULL;
    SeaValue k, x;
    if (!ks || !vs) { snprintf(e->msg, sizeof e->msg, "out of memory"); goto done; }
    while (m < n && sea_map_next(mp, &it, &k, &x)) {
        ks[m] = to_dvalue(vm, k, e, depth + 1);
        if (!ks[m]) goto done;
        vs[m] = to_dvalue(vm, x, e, depth + 1);
        if (!vs[m]) { duckdb_destroy_value(&ks[m]); goto done; }
        m++;
    }
    {
        duckdb_logical_type kt = duckdb_get_value_type(ks[0]), vt = duckdb_get_value_type(vs[0]);
        duckdb_logical_type mt = duckdb_create_map_type(kt, vt);
        r = duckdb_create_map_value(mt, ks, vs, m);
        duckdb_destroy_logical_type(&mt);
        if (!r) snprintf(e->msg, sizeof e->msg, "map keys and values must each have one type");
    }
done:
    for (uint32_t i = 0; i < m; i++) { duckdb_destroy_value(&ks[i]); duckdb_destroy_value(&vs[i]); }
    free(ks);
    free(vs);
    return r;
}

static duckdb_value to_dvalue(SeaVM *vm, SeaValue v, VErr *e, int depth) {
    if (depth > 64) { snprintf(e->msg, sizeof e->msg, "value nested too deep"); return NULL; }
    if (sea_is_nil(v)) return duckdb_create_null_value();
    if (sea_is_bool(v)) return duckdb_create_bool(sea_as_bool(v));
    if (sea_is_int(v)) return duckdb_create_int64(sea_as_int(v));
    if (sea_is_number(v)) return duckdb_create_double(sea_as_number(v));
    if (sea_is_bigint(v)) {                  /* BIGINT, HUGEINT or BIGNUM: the narrowest */
        SeaBigInt *b = SEA_AS_BIGINT(v);
        int64_t i64;
        if (sea_bigint_to_i64(b, &i64)) return duckdb_create_int64(i64);
        size_t len = sea_bigint_byte_len(b);
        uint8_t *be = malloc(len ? len : 1);
        if (!be) { snprintf(e->msg, sizeof e->msg, "out of memory"); return NULL; }
        sea_bigint_to_bytes(b, be, len);
        duckdb_value r;
        if (len < 16 || (len == 16 && be[0] < 0x80)) {
            unsigned __int128 x = 0;
            for (size_t i = 0; i < len; i++) x = (x << 8) | be[i];
            if (b->neg) x = (unsigned __int128)0 - x;
            duckdb_hugeint h = { (uint64_t)x, (int64_t)(uint64_t)(x >> 64) };
            r = duckdb_create_hugeint(h);
        } else {
            duckdb_bignum bn = { be, len, b->neg != 0 };
            r = duckdb_create_bignum(bn);
        }
        free(be);
        return r;
    }
    if (sea_is_string(v) || sea_is_rope(v)) {
        SeaString *s = sea_value_get_flat_string(v);
        duckdb_value r = duckdb_create_varchar_length(s->data, s->len);
        sea_obj_release(&s->obj);
        if (!r) snprintf(e->msg, sizeof e->msg, "invalid UTF-8 in a string (a BLOB needs a cast in SQL)");
        return r;
    }
    if (is_seq(v)) return to_dlist(vm, v, e, depth);
    if (sea_is_map(v)) return to_dmap(vm, v, e, depth);
    if (sea_is_table(v) || sea_is_struct(v)) return to_dstruct(vm, v, e, depth);
    snprintf(e->msg, sizeof e->msg, "cannot pass a %s to DuckDB", sea_type_name(v));
    return NULL;
}

/* ======================================================================== */
/* Query jobs: run on a worker thread (goroutines) or in place             */
/* ======================================================================== */

typedef struct Job {
    struct Job     *next;
    atomic_int      refs, done;
    int             efd, wfd;
    Obj            *conn, *stmt;      /* references held */
    char           *sql;              /* when there is no stmt */
    int             nparams;
    duckdb_value   *pvals;
    char          **pnames;           /* named parameters, or NULL */
    bool            has_result;
    duckdb_result   res;
    char           *err;
} Job;

static void job_free(Job *j) {
    for (int i = 0; i < j->nparams; i++) {
        duckdb_destroy_value(&j->pvals[i]);
        if (j->pnames) free(j->pnames[i]);
    }
    free(j->pvals);
    free(j->pnames);
    free(j->sql);
    free(j->err);
    if (j->has_result) duckdb_destroy_result(&j->res);
    if (j->efd >= 0) {
        sea_netpoll_close(j->efd);
        sea_wake_close_wr(j->efd, j->wfd);
    }
    obj_unref(j->stmt);
    obj_unref(j->conn);
    free(j);
}

static void job_unref(Job *j) {
    if (atomic_fetch_sub(&j->refs, 1) == 1) job_free(j);
}

static void job_fail(Job *j, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    free(j->err);
    j->err = dupstr(msg);
}

/* Bind j's parameters to ps (under the connection's lock). */
static bool job_bind(Job *j, duckdb_prepared_statement ps) {
    duckdb_clear_bindings(ps);
    idx_t want = duckdb_nparams(ps);
    if (!j->pnames && (idx_t)j->nparams != want) {
        job_fail(j, "expected %llu parameter%s, got %d", (unsigned long long)want, want == 1 ? "" : "s", j->nparams);
        return false;
    }
    for (int i = 0; i < j->nparams; i++) {
        idx_t idx = (idx_t)i + 1;
        if (j->pnames) {
            const char *nm = j->pnames[i];
            if (nm[0] == '$') nm++;
            if (duckdb_bind_parameter_index(ps, &idx, nm) == DuckDBError) {
                job_fail(j, "unknown parameter $%s", nm);
                return false;
            }
        }
        if (duckdb_bind_value(ps, idx, j->pvals[i]) == DuckDBError) {
            job_fail(j, "could not bind parameter %llu", (unsigned long long)idx);
            return false;
        }
    }
    return true;
}

static void job_run(Job *j) {
    Obj *c = j->conn;
    pthread_mutex_lock(&c->mu);
    if (j->stmt) {
        if (job_bind(j, j->stmt->stmt)) {
            if (duckdb_execute_prepared(j->stmt->stmt, &j->res) == DuckDBError) {
                job_fail(j, "%s", duckdb_result_error(&j->res));
                duckdb_destroy_result(&j->res);
            } else {
                j->has_result = true;
            }
        }
    } else if (j->nparams || j->pnames) {
        duckdb_prepared_statement ps = NULL;
        if (duckdb_prepare(c->con, j->sql, &ps) == DuckDBError) {
            job_fail(j, "%s", duckdb_prepare_error(ps));
        } else if (job_bind(j, ps)) {
            if (duckdb_execute_prepared(ps, &j->res) == DuckDBError) {
                job_fail(j, "%s", duckdb_result_error(&j->res));
                duckdb_destroy_result(&j->res);
            } else {
                j->has_result = true;
            }
        }
        duckdb_destroy_prepare(&ps);
    } else {
        if (duckdb_query(c->con, j->sql, &j->res) == DuckDBError) {
            job_fail(j, "%s", duckdb_result_error(&j->res));
            duckdb_destroy_result(&j->res);
        } else {
            j->has_result = true;
        }
    }
    pthread_mutex_unlock(&c->mu);
}

#define POOL_MAX 8

static pthread_mutex_t pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  pool_cv = PTHREAD_COND_INITIALIZER;
static Job *pool_head, *pool_tail;
static int pool_threads, pool_idle;

static void *pool_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&pool_mu);
        while (!pool_head) {
            pool_idle++;
            pthread_cond_wait(&pool_cv, &pool_mu);
            pool_idle--;
        }
        Job *j = pool_head;
        pool_head = j->next;
        if (!pool_head) pool_tail = NULL;
        pthread_mutex_unlock(&pool_mu);

        job_run(j);
        atomic_store(&j->done, 1);
        sea_wake_post(j->wfd);
        job_unref(j);
    }
    return NULL;
}

/* Hand j to a worker (it takes a reference). False: run it in place. */
static bool pool_submit(Job *j) {
    if (sea_wake_open(&j->efd, &j->wfd) < 0) { j->efd = j->wfd = -1; return false; }
    atomic_fetch_add(&j->refs, 1);
    pthread_mutex_lock(&pool_mu);
    if (pool_idle == 0 && pool_threads < POOL_MAX) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, pool_worker, NULL) == 0) pool_threads++;
        pthread_attr_destroy(&at);
    }
    bool ok = pool_threads > 0;
    if (ok) {
        if (pool_tail) pool_tail->next = j; else pool_head = j;
        pool_tail = j;
        pthread_cond_signal(&pool_cv);
    }
    pthread_mutex_unlock(&pool_mu);
    if (!ok) {
        atomic_fetch_sub(&j->refs, 1);
        close(j->efd);
        sea_wake_close_wr(j->efd, j->wfd);
        j->efd = j->wfd = -1;
    }
    return ok;
}

/* ======================================================================== */
/* DuckDB values → Sea values                                               */
/* ======================================================================== */

typedef struct TI TI;
struct TI {
    duckdb_type         id;
    duckdb_logical_type lt;           /* owned */
    int                 nchild;
    TI                 *child;
    SeaString         **names;        /* struct members / union members */
    SeaString         **dict;         /* enum */
    uint32_t            ndict;
    duckdb_type         internal;     /* enum and decimal storage */
    uint8_t             width, scale;
    idx_t               array_size;
};

static const char *type_name(duckdb_type t) {
    switch (t) {
        case DUCKDB_TYPE_BOOLEAN: return "BOOLEAN";
        case DUCKDB_TYPE_TINYINT: return "TINYINT";
        case DUCKDB_TYPE_SMALLINT: return "SMALLINT";
        case DUCKDB_TYPE_INTEGER: return "INTEGER";
        case DUCKDB_TYPE_BIGINT: return "BIGINT";
        case DUCKDB_TYPE_UTINYINT: return "UTINYINT";
        case DUCKDB_TYPE_USMALLINT: return "USMALLINT";
        case DUCKDB_TYPE_UINTEGER: return "UINTEGER";
        case DUCKDB_TYPE_UBIGINT: return "UBIGINT";
        case DUCKDB_TYPE_FLOAT: return "FLOAT";
        case DUCKDB_TYPE_DOUBLE: return "DOUBLE";
        case DUCKDB_TYPE_TIMESTAMP: return "TIMESTAMP";
        case DUCKDB_TYPE_DATE: return "DATE";
        case DUCKDB_TYPE_TIME: return "TIME";
        case DUCKDB_TYPE_INTERVAL: return "INTERVAL";
        case DUCKDB_TYPE_HUGEINT: return "HUGEINT";
        case DUCKDB_TYPE_UHUGEINT: return "UHUGEINT";
        case DUCKDB_TYPE_VARCHAR: return "VARCHAR";
        case DUCKDB_TYPE_BLOB: return "BLOB";
        case DUCKDB_TYPE_DECIMAL: return "DECIMAL";
        case DUCKDB_TYPE_TIMESTAMP_S: return "TIMESTAMP_S";
        case DUCKDB_TYPE_TIMESTAMP_MS: return "TIMESTAMP_MS";
        case DUCKDB_TYPE_TIMESTAMP_NS: return "TIMESTAMP_NS";
        case DUCKDB_TYPE_ENUM: return "ENUM";
        case DUCKDB_TYPE_LIST: return "LIST";
        case DUCKDB_TYPE_STRUCT: return "STRUCT";
        case DUCKDB_TYPE_MAP: return "MAP";
        case DUCKDB_TYPE_ARRAY: return "ARRAY";
        case DUCKDB_TYPE_UUID: return "UUID";
        case DUCKDB_TYPE_UNION: return "UNION";
        case DUCKDB_TYPE_BIT: return "BIT";
        case DUCKDB_TYPE_TIME_TZ: return "TIME WITH TIME ZONE";
        case DUCKDB_TYPE_TIMESTAMP_TZ: return "TIMESTAMP WITH TIME ZONE";
        case DUCKDB_TYPE_TIME_NS: return "TIME_NS";
        case DUCKDB_TYPE_SQLNULL: return "NULL";
        case DUCKDB_TYPE_BIGNUM: return "BIGNUM";
        case DUCKDB_TYPE_GEOMETRY: return "GEOMETRY";
        case DUCKDB_TYPE_VARIANT: return "VARIANT";
        default: return "UNKNOWN";
    }
}

static SeaString *cstr_take(char *s) {        /* a DuckDB-allocated string */
    SeaString *r = sea_string_new(s ? s : "", s ? strlen(s) : 0);
    if (s) duckdb_free(s);
    return r;
}

/* Build the type tree; returns the name of an unsupported type, or NULL. */
static const char *ti_build(TI *t, duckdb_logical_type lt) {
    memset(t, 0, sizeof *t);
    t->lt = lt;
    t->id = duckdb_get_type_id(lt);
    const char *bad = NULL;
    switch (t->id) {
        case DUCKDB_TYPE_LIST:
        case DUCKDB_TYPE_ARRAY:
        case DUCKDB_TYPE_MAP: {
            t->nchild = t->id == DUCKDB_TYPE_MAP ? 2 : 1;
            t->child = calloc((size_t)t->nchild, sizeof *t->child);
            if (!t->child) return "out of memory";
            if (t->id == DUCKDB_TYPE_LIST) bad = ti_build(&t->child[0], duckdb_list_type_child_type(lt));
            else if (t->id == DUCKDB_TYPE_ARRAY) {
                t->array_size = duckdb_array_type_array_size(lt);
                bad = ti_build(&t->child[0], duckdb_array_type_child_type(lt));
            } else {
                bad = ti_build(&t->child[0], duckdb_map_type_key_type(lt));
                if (!bad) bad = ti_build(&t->child[1], duckdb_map_type_value_type(lt));
            }
            break;
        }
        case DUCKDB_TYPE_STRUCT:
        case DUCKDB_TYPE_UNION: {
            bool u = t->id == DUCKDB_TYPE_UNION;
            t->nchild = (int)(u ? duckdb_union_type_member_count(lt) : duckdb_struct_type_child_count(lt));
            t->child = calloc((size_t)(t->nchild ? t->nchild : 1), sizeof *t->child);
            t->names = calloc((size_t)(t->nchild ? t->nchild : 1), sizeof *t->names);
            if (!t->child || !t->names) return "out of memory";
            for (int i = 0; i < t->nchild && !bad; i++) {
                t->names[i] = cstr_take(u ? duckdb_union_type_member_name(lt, (idx_t)i)
                                          : duckdb_struct_type_child_name(lt, (idx_t)i));
                bad = ti_build(&t->child[i], u ? duckdb_union_type_member_type(lt, (idx_t)i)
                                               : duckdb_struct_type_child_type(lt, (idx_t)i));
            }
            break;
        }
        case DUCKDB_TYPE_ENUM:
            t->internal = duckdb_enum_internal_type(lt);
            t->ndict = duckdb_enum_dictionary_size(lt);
            t->dict = calloc(t->ndict ? t->ndict : 1, sizeof *t->dict);
            if (!t->dict) return "out of memory";
            for (uint32_t i = 0; i < t->ndict; i++) t->dict[i] = cstr_take(duckdb_enum_dictionary_value(lt, i));
            break;
        case DUCKDB_TYPE_DECIMAL:
            t->internal = duckdb_decimal_internal_type(lt);
            t->width = duckdb_decimal_width(lt);
            t->scale = duckdb_decimal_scale(lt);
            break;
        case DUCKDB_TYPE_ANY:
        case DUCKDB_TYPE_INVALID:
            return type_name(t->id);
        default:
            break;
    }
    return bad;
}

static void ti_free(TI *t) {
    for (int i = 0; i < t->nchild; i++) {
        if (t->child) ti_free(&t->child[i]);
        if (t->names && t->names[i]) sea_obj_release(&t->names[i]->obj);
    }
    for (uint32_t i = 0; i < t->ndict; i++)
        if (t->dict && t->dict[i]) sea_obj_release(&t->dict[i]->obj);
    free(t->child);
    free(t->names);
    free(t->dict);
    if (t->lt) duckdb_destroy_logical_type(&t->lt);
}

/* SQL spelling of a type, for `columns`. */
static void ti_str(const TI *t, char *buf, size_t cap) {
    size_t n = strlen(buf);
    if (n + 1 >= cap) return;
    char *p = buf + n;
    size_t left = cap - n;
    switch (t->id) {
        case DUCKDB_TYPE_DECIMAL:
            snprintf(p, left, "DECIMAL(%u,%u)", t->width, t->scale);
            break;
        case DUCKDB_TYPE_LIST:
            ti_str(&t->child[0], buf, cap);
            strncat(buf, "[]", cap - strlen(buf) - 1);
            break;
        case DUCKDB_TYPE_ARRAY:
            ti_str(&t->child[0], buf, cap);
            n = strlen(buf);
            snprintf(buf + n, cap - n, "[%llu]", (unsigned long long)t->array_size);
            break;
        case DUCKDB_TYPE_MAP:
            strncat(buf, "MAP(", cap - strlen(buf) - 1);
            ti_str(&t->child[0], buf, cap);
            strncat(buf, ", ", cap - strlen(buf) - 1);
            ti_str(&t->child[1], buf, cap);
            strncat(buf, ")", cap - strlen(buf) - 1);
            break;
        case DUCKDB_TYPE_STRUCT:
        case DUCKDB_TYPE_UNION:
            strncat(buf, t->id == DUCKDB_TYPE_STRUCT ? "STRUCT(" : "UNION(", cap - strlen(buf) - 1);
            for (int i = 0; i < t->nchild; i++) {
                if (i) strncat(buf, ", ", cap - strlen(buf) - 1);
                strncat(buf, t->names[i]->data, cap - strlen(buf) - 1);
                strncat(buf, " ", cap - strlen(buf) - 1);
                ti_str(&t->child[i], buf, cap);
            }
            strncat(buf, ")", cap - strlen(buf) - 1);
            break;
        default:
            snprintf(p, left, "%s", type_name(t->id));
    }
}

/* ---- dates and times ------------------------------------------------------ */

/* Proleptic Gregorian date of a day count since 1970-01-01 (H. Hinnant). */
static void civil(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}

static int fmt_date(char *out, int64_t days) {
    int64_t y;
    unsigned m, d;
    civil(days, &y, &m, &d);
    if (y > 0) return sprintf(out, "%04lld-%02u-%02u", (long long)y, m, d);
    return sprintf(out, "%04lld-%02u-%02u (BC)", (long long)(1 - y), m, d);
}

/* HH:MM:SS plus a fraction of `digits` digits (trailing zeros dropped). */
static int fmt_clock(char *out, int64_t units, int64_t per_sec, int digits) {
    int64_t secs = units / per_sec, frac = units % per_sec;
    int n = sprintf(out, "%02lld:%02lld:%02lld", (long long)(secs / 3600), (long long)(secs / 60 % 60),
                    (long long)(secs % 60));
    if (frac) {
        char f[16];
        snprintf(f, sizeof f, "%0*lld", digits, (long long)frac);
        int k = digits;
        while (k > 0 && f[k - 1] == '0') k--;
        f[k] = '\0';
        n += sprintf(out + n, ".%s", f);
    }
    return n;
}

static SeaValue fmt_timestamp(int64_t v, int64_t per_sec, int digits, bool tz) {
    if (v == INT64_MAX) return str_val("infinity", 8);
    if (v == -INT64_MAX) return str_val("-infinity", 9);
    int64_t per_day = per_sec * 86400;
    int64_t days = v / per_day, rem = v % per_day;
    if (rem < 0) { rem += per_day; days--; }
    char buf[80];
    int n = fmt_date(buf, days);
    bool bc = buf[n - 1] == ')';
    if (bc) n -= 5;                                /* the time goes before " (BC)" */
    buf[n++] = ' ';
    n += fmt_clock(buf + n, rem, per_sec, digits);
    if (tz) n += sprintf(buf + n, "+00");
    if (bc) n += sprintf(buf + n, " (BC)");
    return str_val(buf, (size_t)n);
}

static SeaValue fmt_uuid(duckdb_hugeint h) {
    uint64_t hi = (uint64_t)h.upper ^ (UINT64_C(1) << 63), lo = h.lower;
    char buf[40];
    snprintf(buf, sizeof buf, "%08llx-%04llx-%04llx-%04llx-%012llx", (unsigned long long)(hi >> 32),
             (unsigned long long)((hi >> 16) & 0xFFFF), (unsigned long long)(hi & 0xFFFF),
             (unsigned long long)(lo >> 48), (unsigned long long)(lo & UINT64_C(0xFFFFFFFFFFFF)));
    return str_val(buf, 36);
}

static const double POW10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12,
                                1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22, 1e23,
                                1e24, 1e25, 1e26, 1e27, 1e28, 1e29, 1e30, 1e31, 1e32, 1e33, 1e34,
                                1e35, 1e36, 1e37, 1e38 };

static SeaValue str_t_val(duckdb_string_t *s) {
    uint32_t n = s->value.inlined.length;
    const char *d = n <= 12 ? s->value.inlined.inlined : s->value.pointer.ptr;
    return str_val(d, n);
}

/* A BIT value: a padding count byte, then the bits (the first `pad` unused). */
static SeaValue bit_bytes(const unsigned char *d, size_t n) {
    if (n < 1) return str_val("", 0);
    unsigned pad = d[0];
    size_t bits = (size_t)(n - 1) * 8 - pad;
    char *out = malloc(bits + 1);
    if (!out) return str_val("", 0);
    for (size_t i = 0; i < bits; i++) {
        size_t b = i + pad;
        out[i] = (d[1 + b / 8] >> (7 - b % 8)) & 1 ? '1' : '0';
    }
    SeaValue r = str_val(out, bits);
    free(out);
    return r;
}

static const unsigned char *str_t_data(duckdb_string_t *s, uint32_t *n) {
    *n = s->value.inlined.length;
    return (const unsigned char *)(*n <= 12 ? s->value.inlined.inlined : s->value.pointer.ptr);
}

static SeaValue bit_val(duckdb_string_t *s) {
    uint32_t n;
    const unsigned char *d = str_t_data(s, &n);
    return bit_bytes(d, n);
}

typedef struct { duckdb_vector v; void *data; uint64_t *valid; } Vec;

/* ---- shared scalar formatting (columns and VARIANT values) ---------------- */

static SeaValue date_val(int32_t days) {
    if (days == INT32_MAX) return str_val("infinity", 8);
    if (days == -INT32_MAX) return str_val("-infinity", 9);
    char buf[40];
    int n = fmt_date(buf, days);
    return str_val(buf, (size_t)n);
}

static SeaValue clock_val(int64_t units, int64_t per_sec, int digits) {
    char buf[40];
    int n = fmt_clock(buf, units, per_sec, digits);
    return str_val(buf, (size_t)n);
}

static SeaValue timetz_val(uint64_t bits) {
    duckdb_time_tz tz = { bits };
    duckdb_time_tz_struct s = duckdb_from_time_tz(tz);
    int64_t micros = ((int64_t)s.time.hour * 3600 + s.time.min * 60 + s.time.sec) * 1000000 + s.time.micros;
    char buf[48];
    int n = fmt_clock(buf, micros, 1000000, 6);
    int off = s.offset, a = off < 0 ? -off : off;
    n += sprintf(buf + n, "%c%02d", off < 0 ? '-' : '+', a / 3600);
    if (a % 3600) n += sprintf(buf + n, ":%02d", a / 60 % 60);
    if (a % 60) n += sprintf(buf + n, ":%02d", a % 60);
    return str_val(buf, (size_t)n);
}

static SeaValue interval_val(int32_t months, int32_t days, int64_t micros) {
    SeaTable *tb = sea_table_new(TABLE_MAP);
    sea_table_set_str(tb, "months", sea_int_val(months));
    sea_table_set_str(tb, "days", sea_int_val(days));
    sea_table_set_str(tb, "micros", int_or_number(micros));
    return sea_obj_val(&tb->obj);
}

static SeaValue decimal_val(double raw, unsigned scale) {
    return sea_number_val(raw / POW10[scale <= 38 ? scale : 38]);
}

/* ---- GEOMETRY: WKB → {type, coordinates} (or {type, geometries}) ---------- */

typedef struct { const uint8_t *p; size_t n, pos; bool ok; } Wkb;

static uint32_t wkb_u32(Wkb *w, bool le) {
    if (w->pos + 4 > w->n) { w->ok = false; return 0; }
    const uint8_t *b = w->p + w->pos;
    w->pos += 4;
    return le ? (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24
              : (uint32_t)b[3] | (uint32_t)b[2] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[0] << 24;
}

static double wkb_f64(Wkb *w, bool le) {
    if (w->pos + 8 > w->n) { w->ok = false; return 0; }
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = w->p[w->pos + (le ? (size_t)i : (size_t)(7 - i))];
    w->pos += 8;
    double d;
    memcpy(&d, b, 8);
    return d;
}

/* Byte order and type of a geometry header: the base type (1..7) and the
 * number of ordinates (2, 3 with Z or M, 4 with both). */
static uint32_t wkb_header(Wkb *w, bool *le, int *nc) {
    if (w->pos >= w->n) { w->ok = false; return 0; }
    *le = w->p[w->pos++] == 1;
    uint32_t t = wkb_u32(w, *le);
    bool z = (t & 0x80000000u) != 0, m = (t & 0x40000000u) != 0;      /* EWKB flags */
    if (t & 0x20000000u) wkb_u32(w, *le);                              /* EWKB SRID */
    t &= 0x0FFFFFFFu;
    if (t >= 3000) { z = m = true; t -= 3000; }                        /* ISO */
    else if (t >= 2000) { m = true; t -= 2000; }
    else if (t >= 1000) { z = true; t -= 1000; }
    *nc = 2 + z + m;
    return t;
}

static SeaValue wkb_position(Wkb *w, bool le, int nc, bool point) {
    double c[4];
    bool all_nan = true;
    for (int i = 0; i < nc; i++) {
        c[i] = wkb_f64(w, le);
        if (!isnan(c[i])) all_nan = false;
    }
    SeaArray *a = sea_array_new((uint32_t)nc);
    if (!(point && all_nan))                  /* POINT EMPTY: no ordinates */
        for (int i = 0; i < nc; i++) sea_array_push(a, sea_number_val(c[i]));
    return sea_obj_val(&a->obj);
}

static SeaValue wkb_coords(Wkb *w, uint32_t type, bool le, int nc) {
    if (type == 1) return wkb_position(w, le, nc, true);
    uint32_t count = wkb_u32(w, le);
    if (!w->ok || count > w->n) { w->ok = false; return SEA_NIL_VAL; }
    SeaArray *a = sea_array_new(count);
    for (uint32_t i = 0; i < count && w->ok; i++) {
        if (type == 2) {
            sea_array_push_move(a, wkb_position(w, le, nc, false));
        } else if (type == 3) {
            sea_array_push_move(a, wkb_coords(w, 2, le, nc));          /* a ring: like a line */
        } else {                                                        /* 4, 5, 6: sub-geometries */
            bool sle;
            int snc;
            uint32_t st = wkb_header(w, &sle, &snc);
            if (st != type - 3) { w->ok = false; break; }
            sea_array_push_move(a, wkb_coords(w, st, sle, snc));
        }
    }
    return sea_obj_val(&a->obj);
}

static SeaValue wkb_geom(Wkb *w, int depth) {
    static const char *names[] = { "", "Point", "LineString", "Polygon", "MultiPoint",
                                   "MultiLineString", "MultiPolygon", "GeometryCollection" };
    bool le;
    int nc;
    uint32_t t = wkb_header(w, &le, &nc);
    if (!w->ok || t < 1 || t > 7 || depth > 32) { w->ok = false; return SEA_NIL_VAL; }
    SeaTable *g = sea_table_new(TABLE_MAP);
    SeaValue name = str_val(names[t], strlen(names[t]));
    sea_table_set_str(g, "type", name);
    val_release(name);
    SeaValue body;
    if (t == 7) {
        uint32_t count = wkb_u32(w, le);
        if (count > w->n) w->ok = false;
        SeaArray *a = sea_array_new(w->ok ? count : 0);
        for (uint32_t i = 0; i < count && w->ok; i++) sea_array_push_move(a, wkb_geom(w, depth + 1));
        body = sea_obj_val(&a->obj);
        sea_table_set_str(g, "geometries", body);
    } else {
        body = wkb_coords(w, t, le, nc);
        sea_table_set_str(g, "coordinates", body);
    }
    val_release(body);
    return sea_obj_val(&g->obj);
}

/* A GEOMETRY value; malformed WKB comes back as its bytes. */
static SeaValue geometry_val(const uint8_t *d, size_t n) {
    Wkb w = { d, n, 0, true };
    SeaValue g = wkb_geom(&w, 0);
    if (w.ok) return g;
    val_release(g);
    return str_val((const char *)d, n);
}

/* ---- VARIANT ------------------------------------------------------------------
 * Stored as STRUCT(keys VARCHAR[], children STRUCT(keys_index, values_index)[],
 * values STRUCT(type_id UTINYINT, byte_offset UINTEGER)[], data BLOB): values[0]
 * is the root; scalars live in data at byte_offset (lengths as ULEB128);
 * objects and arrays hold a count and the first of their entries in children.
 * (DuckDB 1.5's layout, which the C API does not describe: libs/duckdb/test.sea
 * checks every type.) */

typedef struct {
    duckdb_string_t *keys;
    idx_t            koff, kn;
    uint32_t        *ckey, *cval;
    uint64_t        *ckey_valid;
    idx_t            coff, cn;
    uint8_t         *vtype;
    uint32_t        *vbyte;
    idx_t            voff, vn;
    const uint8_t   *data;
    size_t           dn;
} Var;

static bool var_uleb(const Var *v, size_t *pos, uint64_t *out) {
    uint64_t r = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (*pos >= v->dn) return false;
        uint8_t b = v->data[(*pos)++];
        r |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *out = r; return true; }
    }
    return false;
}

static SeaValue var_value(const Var *v, uint64_t idx, int depth);

static SeaValue var_entries(const Var *v, size_t pos, bool object, int depth) {
    uint64_t count, start;
    if (!var_uleb(v, &pos, &count) || !var_uleb(v, &pos, &start) || start + count > v->cn)
        return SEA_NIL_VAL;
    if (!object) {
        SeaArray *a = sea_array_new((uint32_t)count);
        for (uint64_t i = 0; i < count; i++)
            sea_array_push_move(a, var_value(v, v->cval[v->coff + start + i], depth + 1));
        return sea_obj_val(&a->obj);
    }
    SeaTable *t = sea_table_new(TABLE_MAP);
    for (uint64_t i = 0; i < count; i++) {
        idx_t c = v->coff + start + i;
        if (v->ckey_valid && !duckdb_validity_row_is_valid(v->ckey_valid, c)) continue;
        if (v->ckey[c] >= v->kn) continue;
        uint32_t kl;
        const unsigned char *kd = str_t_data(&v->keys[v->koff + v->ckey[c]], &kl);
        SeaValue k = str_val((const char *)kd, kl), x = var_value(v, v->cval[c], depth + 1);
        sea_table_set(t, k, x);
        val_release(k);
        val_release(x);
    }
    return sea_obj_val(&t->obj);
}

static SeaValue var_value(const Var *v, uint64_t idx, int depth) {
    if (idx >= v->vn || depth > 100) return SEA_NIL_VAL;
    uint8_t type = v->vtype[v->voff + idx];
    size_t pos = v->vbyte[v->voff + idx];
    const uint8_t *d = v->data;
    static const uint8_t width[] = { 0, 0, 0, 1, 2, 4, 8, 16, 1, 2, 4, 8, 16, 4, 8 };   /* fixed sizes */
    if (type < sizeof width && pos + width[type] > v->dn) return SEA_NIL_VAL;
    #define RD(T) ({ T _x; memcpy(&_x, d + pos, sizeof _x); _x; })
    switch (type) {
        case 0:  return SEA_NIL_VAL;
        case 1:  return SEA_TRUE_VAL;
        case 2:  return SEA_FALSE_VAL;
        case 3:  return sea_int_val(RD(int8_t));
        case 4:  return sea_int_val(RD(int16_t));
        case 5:  return sea_int_val(RD(int32_t));
        case 6:  return exact_i64(RD(int64_t));
        case 7:  { uint64_t lo = RD(uint64_t); pos += 8; return exact_i128(RD(int64_t), lo); }
        case 8:  return sea_int_val(RD(uint8_t));
        case 9:  return sea_int_val(RD(uint16_t));
        case 10: return sea_int_val(RD(uint32_t));
        case 11: return exact_u64(RD(uint64_t));
        case 12: { uint64_t lo = RD(uint64_t); pos += 8; uint64_t hi = RD(uint64_t);
                   return hi ? exact_u128(hi, lo, false) : exact_u64(lo); }
        case 13: return sea_number_val(RD(float));
        case 14: return sea_number_val(RD(double));
        case 15: {                                                      /* DECIMAL */
            uint64_t w, sc;
            if (!var_uleb(v, &pos, &w) || !var_uleb(v, &pos, &sc)) return SEA_NIL_VAL;
            size_t size = w <= 4 ? 2 : w <= 9 ? 4 : w <= 18 ? 8 : 16;
            if (pos + size > v->dn) return SEA_NIL_VAL;
            double raw;
            if (size == 2) raw = RD(int16_t);
            else if (size == 4) raw = RD(int32_t);
            else if (size == 8) raw = (double)RD(int64_t);
            else { uint64_t lo = RD(uint64_t); pos += 8; raw = (double)RD(int64_t) * 18446744073709551616.0 + (double)lo; }
            return decimal_val(raw, (unsigned)sc);
        }
        case 16: case 17: case 31: case 32: case 33: {                  /* length-prefixed */
            uint64_t n;
            if (!var_uleb(v, &pos, &n) || pos + n > v->dn) return SEA_NIL_VAL;
            if (type == 31) return bignum_val(d + pos, n);
            if (type == 32) return bit_bytes(d + pos, n);
            if (type == 33) return geometry_val(d + pos, n);
            return str_val((const char *)d + pos, n);
        }
        case 18: {                                                      /* UUID: hugeint */
            if (pos + 16 > v->dn) return SEA_NIL_VAL;
            duckdb_hugeint h;
            memcpy(&h.lower, d + pos, 8);
            memcpy(&h.upper, d + pos + 8, 8);
            return fmt_uuid(h);
        }
        case 19: if (pos + 4 > v->dn) return SEA_NIL_VAL; return date_val(RD(int32_t));
        case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27: {
            if (pos + 8 > v->dn) return SEA_NIL_VAL;
            int64_t x = RD(int64_t);
            switch (type) {
                case 20: return clock_val(x, 1000000, 6);
                case 21: return clock_val(x, 1000000000, 9);
                case 22: return fmt_timestamp(x, 1, 0, false);
                case 23: return fmt_timestamp(x, 1000, 3, false);
                case 24: return fmt_timestamp(x, 1000000, 6, false);
                case 25: return fmt_timestamp(x, 1000000000, 9, false);
                case 26: return timetz_val((uint64_t)x);
                default: return fmt_timestamp(x, 1000000, 6, true);
            }
        }
        case 28: {
            if (pos + 16 > v->dn) return SEA_NIL_VAL;
            int32_t mo = RD(int32_t); pos += 4;
            int32_t dy = RD(int32_t); pos += 4;
            return interval_val(mo, dy, RD(int64_t));
        }
        case 29: return var_entries(v, pos, true, depth);
        case 30: return var_entries(v, pos, false, depth);
        default: return SEA_NIL_VAL;
    }
    #undef RD
}

static SeaValue variant_val(const Vec *col, idx_t row) {
    duckdb_vector keys = duckdb_struct_vector_get_child(col->v, 0), children = duckdb_struct_vector_get_child(col->v, 1),
                  values = duckdb_struct_vector_get_child(col->v, 2), data = duckdb_struct_vector_get_child(col->v, 3);
    duckdb_list_entry ke = ((duckdb_list_entry *)duckdb_vector_get_data(keys))[row];
    duckdb_list_entry ce = ((duckdb_list_entry *)duckdb_vector_get_data(children))[row];
    duckdb_list_entry ve = ((duckdb_list_entry *)duckdb_vector_get_data(values))[row];
    duckdb_vector cs = duckdb_list_vector_get_child(children), vs = duckdb_list_vector_get_child(values);
    duckdb_vector ckv = duckdb_struct_vector_get_child(cs, 0);
    Var v;
    v.keys = duckdb_vector_get_data(duckdb_list_vector_get_child(keys));
    v.koff = ke.offset;
    v.kn = ke.length;
    v.ckey = duckdb_vector_get_data(ckv);
    v.ckey_valid = duckdb_vector_get_validity(ckv);
    v.cval = duckdb_vector_get_data(duckdb_struct_vector_get_child(cs, 1));
    v.coff = ce.offset;
    v.cn = ce.length;
    v.vtype = duckdb_vector_get_data(duckdb_struct_vector_get_child(vs, 0));
    v.vbyte = duckdb_vector_get_data(duckdb_struct_vector_get_child(vs, 1));
    v.voff = ve.offset;
    v.vn = ve.length;
    uint32_t dn;
    v.data = str_t_data(&((duckdb_string_t *)duckdb_vector_get_data(data))[row], &dn);
    v.dn = dn;
    return var_value(&v, 0, 0);
}

static Vec vec(duckdb_vector v) {
    Vec r = { v, duckdb_vector_get_data(v), duckdb_vector_get_validity(v) };
    return r;
}

static SeaValue read_val(const TI *t, const Vec *v, idx_t row, int depth);

static SeaValue read_list(const TI *ct, duckdb_vector child, idx_t off, idx_t len, int depth) {
    Vec cv = vec(child);
    SeaArray *a = sea_array_new((uint32_t)len);
    for (idx_t i = 0; i < len; i++) sea_array_push_move(a, read_val(ct, &cv, off + i, depth + 1));
    return sea_obj_val(&a->obj);
}

static SeaValue read_val(const TI *t, const Vec *v, idx_t row, int depth) {
    if (v->valid && !duckdb_validity_row_is_valid(v->valid, row)) return SEA_NIL_VAL;
    void *d = v->data;
    switch (t->id) {
        case DUCKDB_TYPE_BOOLEAN:   return sea_bool_val(((bool *)d)[row]);
        case DUCKDB_TYPE_TINYINT:   return sea_int_val(((int8_t *)d)[row]);
        case DUCKDB_TYPE_SMALLINT:  return sea_int_val(((int16_t *)d)[row]);
        case DUCKDB_TYPE_INTEGER:   return sea_int_val(((int32_t *)d)[row]);
        case DUCKDB_TYPE_BIGINT:    return exact_i64(((int64_t *)d)[row]);
        case DUCKDB_TYPE_UTINYINT:  return sea_int_val(((uint8_t *)d)[row]);
        case DUCKDB_TYPE_USMALLINT: return sea_int_val(((uint16_t *)d)[row]);
        case DUCKDB_TYPE_UINTEGER:  return sea_int_val(((uint32_t *)d)[row]);
        case DUCKDB_TYPE_UBIGINT:   return exact_u64(((uint64_t *)d)[row]);
        case DUCKDB_TYPE_HUGEINT: {
            duckdb_hugeint h = ((duckdb_hugeint *)d)[row];
            return exact_i128(h.upper, h.lower);
        }
        case DUCKDB_TYPE_UHUGEINT: {
            duckdb_uhugeint h = ((duckdb_uhugeint *)d)[row];
            return h.upper ? exact_u128(h.upper, h.lower, false) : exact_u64(h.lower);
        }
        case DUCKDB_TYPE_BIGNUM: {
            uint32_t n;
            const unsigned char *b = str_t_data(&((duckdb_string_t *)d)[row], &n);
            return bignum_val(b, n);
        }
        case DUCKDB_TYPE_GEOMETRY: {
            uint32_t n;
            const unsigned char *b = str_t_data(&((duckdb_string_t *)d)[row], &n);
            return geometry_val(b, n);
        }
        case DUCKDB_TYPE_VARIANT:   return variant_val(v, row);
        case DUCKDB_TYPE_FLOAT:     return sea_number_val(((float *)d)[row]);
        case DUCKDB_TYPE_DOUBLE:    return sea_number_val(((double *)d)[row]);
        case DUCKDB_TYPE_DECIMAL: {
            double raw;
            switch (t->internal) {
                case DUCKDB_TYPE_SMALLINT: raw = ((int16_t *)d)[row]; break;
                case DUCKDB_TYPE_INTEGER:  raw = ((int32_t *)d)[row]; break;
                case DUCKDB_TYPE_BIGINT:   raw = (double)((int64_t *)d)[row]; break;
                default: {
                    duckdb_hugeint h = ((duckdb_hugeint *)d)[row];
                    raw = (double)h.upper * 18446744073709551616.0 + (double)h.lower;
                }
            }
            return sea_number_val(raw / POW10[t->scale <= 38 ? t->scale : 38]);
        }
        case DUCKDB_TYPE_VARCHAR:
        case DUCKDB_TYPE_BLOB:      return str_t_val(&((duckdb_string_t *)d)[row]);
        case DUCKDB_TYPE_BIT:       return bit_val(&((duckdb_string_t *)d)[row]);
        case DUCKDB_TYPE_DATE: {
            int32_t days = ((int32_t *)d)[row];
            if (days == INT32_MAX) return str_val("infinity", 8);
            if (days == -INT32_MAX) return str_val("-infinity", 9);
            char buf[40];
            int n = fmt_date(buf, days);
            return str_val(buf, (size_t)n);
        }
        case DUCKDB_TYPE_TIME: {
            char buf[40];
            int n = fmt_clock(buf, ((int64_t *)d)[row], 1000000, 6);
            return str_val(buf, (size_t)n);
        }
        case DUCKDB_TYPE_TIME_NS: {
            char buf[40];
            int n = fmt_clock(buf, ((int64_t *)d)[row], 1000000000, 9);
            return str_val(buf, (size_t)n);
        }
        case DUCKDB_TYPE_TIME_TZ: {
            duckdb_time_tz_struct s = duckdb_from_time_tz(((duckdb_time_tz *)d)[row]);
            int64_t micros = ((int64_t)s.time.hour * 3600 + s.time.min * 60 + s.time.sec) * 1000000 + s.time.micros;
            char buf[48];
            int n = fmt_clock(buf, micros, 1000000, 6);
            int off = s.offset, a = off < 0 ? -off : off;
            n += sprintf(buf + n, "%c%02d", off < 0 ? '-' : '+', a / 3600);
            if (a % 3600) n += sprintf(buf + n, ":%02d", a / 60 % 60);
            if (a % 60) n += sprintf(buf + n, ":%02d", a % 60);
            return str_val(buf, (size_t)n);
        }
        case DUCKDB_TYPE_TIMESTAMP:    return fmt_timestamp(((int64_t *)d)[row], 1000000, 6, false);
        case DUCKDB_TYPE_TIMESTAMP_TZ: return fmt_timestamp(((int64_t *)d)[row], 1000000, 6, true);
        case DUCKDB_TYPE_TIMESTAMP_S:  return fmt_timestamp(((int64_t *)d)[row], 1, 0, false);
        case DUCKDB_TYPE_TIMESTAMP_MS: return fmt_timestamp(((int64_t *)d)[row], 1000, 3, false);
        case DUCKDB_TYPE_TIMESTAMP_NS: return fmt_timestamp(((int64_t *)d)[row], 1000000000, 9, false);
        case DUCKDB_TYPE_INTERVAL: {
            duckdb_interval iv = ((duckdb_interval *)d)[row];
            SeaTable *tb = sea_table_new(TABLE_MAP);
            sea_table_set_str(tb, "months", sea_int_val(iv.months));
            sea_table_set_str(tb, "days", sea_int_val(iv.days));
            sea_table_set_str(tb, "micros", int_or_number(iv.micros));
            return sea_obj_val(&tb->obj);
        }
        case DUCKDB_TYPE_UUID: return fmt_uuid(((duckdb_hugeint *)d)[row]);
        case DUCKDB_TYPE_ENUM: {
            uint32_t k;
            switch (t->internal) {
                case DUCKDB_TYPE_UTINYINT:  k = ((uint8_t *)d)[row]; break;
                case DUCKDB_TYPE_USMALLINT: k = ((uint16_t *)d)[row]; break;
                default:                    k = ((uint32_t *)d)[row]; break;
            }
            if (k >= t->ndict) return SEA_NIL_VAL;
            sea_obj_retain(&t->dict[k]->obj);
            return sea_obj_val(&t->dict[k]->obj);
        }
        case DUCKDB_TYPE_LIST: {
            duckdb_list_entry e = ((duckdb_list_entry *)d)[row];
            return read_list(&t->child[0], duckdb_list_vector_get_child(v->v), e.offset, e.length, depth);
        }
        case DUCKDB_TYPE_ARRAY:
            return read_list(&t->child[0], duckdb_array_vector_get_child(v->v), row * t->array_size,
                             t->array_size, depth);
        case DUCKDB_TYPE_MAP: {
            duckdb_list_entry e = ((duckdb_list_entry *)d)[row];
            duckdb_vector kv = duckdb_list_vector_get_child(v->v);
            Vec kvec = vec(duckdb_struct_vector_get_child(kv, 0)), vvec = vec(duckdb_struct_vector_get_child(kv, 1));
            SeaMap *m = sea_map_new();
            for (idx_t i = 0; i < e.length; i++) {
                SeaValue k = read_val(&t->child[0], &kvec, e.offset + i, depth + 1);
                SeaValue x = read_val(&t->child[1], &vvec, e.offset + i, depth + 1);
                sea_map_set(m, k, x);
                val_release(k);
                val_release(x);
            }
            return sea_obj_val(&m->obj);
        }
        case DUCKDB_TYPE_STRUCT: {
            SeaTable *tb = sea_table_new(TABLE_MAP);
            for (int i = 0; i < t->nchild; i++) {
                Vec cv = vec(duckdb_struct_vector_get_child(v->v, (idx_t)i));
                SeaValue x = read_val(&t->child[i], &cv, row, depth + 1);
                sea_table_set(tb, sea_obj_val(&t->names[i]->obj), x);
                val_release(x);
            }
            return sea_obj_val(&tb->obj);
        }
        case DUCKDB_TYPE_UNION: {
            Vec tag = vec(duckdb_struct_vector_get_child(v->v, 0));
            uint8_t k = ((uint8_t *)tag.data)[row];
            if ((int)k >= t->nchild) return SEA_NIL_VAL;
            Vec mv = vec(duckdb_struct_vector_get_child(v->v, (idx_t)k + 1));
            return read_val(&t->child[k], &mv, row, depth + 1);
        }
        default:
            return SEA_NIL_VAL;               /* SQLNULL */
    }
}

/* ======================================================================== */
/* Results                                                                  */
/* ======================================================================== */

enum { R_ROWS, R_EXEC, R_ROW, R_VALUE, R_COLUMNS, R_EACH };

typedef struct {
    idx_t       ncols;
    TI         *types;
    SeaString **names;
} Cols;

static void cols_free(Cols *c) {
    for (idx_t i = 0; i < c->ncols; i++) {
        if (c->types) ti_free(&c->types[i]);
        if (c->names && c->names[i]) sea_obj_release(&c->names[i]->obj);
    }
    free(c->types);
    free(c->names);
}

/* Column names (the program's interned instance when there is one, so
 * row.name compares pointers) and type trees; an error message or NULL. */
static const char *cols_init(Cols *c, duckdb_result *res, char *err, size_t errlen) {
    memset(c, 0, sizeof *c);
    c->ncols = duckdb_column_count(res);
    c->types = calloc(c->ncols ? c->ncols : 1, sizeof *c->types);
    c->names = calloc(c->ncols ? c->ncols : 1, sizeof *c->names);
    if (!c->types || !c->names) return "out of memory";
    for (idx_t i = 0; i < c->ncols; i++) {
        const char *nm = duckdb_column_name(res, i);
        size_t nl = strlen(nm);
        SeaString *s = sea_string_find_intern(nm, nl);
        c->names[i] = s ? s : sea_string_new(nm, nl);
        const char *bad = ti_build(&c->types[i], duckdb_column_logical_type(res, i));
        if (bad) {
            snprintf(err, errlen, "column \"%s\" has type %s, which Sea cannot hold yet; cast it in SQL "
                     "(for example %s::VARCHAR)", nm, bad, nm);
            return err;
        }
    }
    return NULL;
}

static SeaValue make_row(const Cols *c, const Vec *vs, idx_t row) {
    SeaTable *t = sea_table_new(TABLE_MAP);
    for (idx_t j = 0; j < c->ncols; j++) {
        SeaValue x = read_val(&c->types[j], &vs[j], row, 0);
        sea_table_set(t, sea_obj_val(&c->names[j]->obj), x);
        val_release(x);
    }
    return sea_obj_val(&t->obj);
}

static bool is_int_type(duckdb_type t) {
    return t == DUCKDB_TYPE_TINYINT || t == DUCKDB_TYPE_SMALLINT || t == DUCKDB_TYPE_INTEGER ||
           t == DUCKDB_TYPE_BIGINT || t == DUCKDB_TYPE_UTINYINT || t == DUCKDB_TYPE_USMALLINT ||
           t == DUCKDB_TYPE_UINTEGER;
}

static bool is_float_type(duckdb_type t) {
    return t == DUCKDB_TYPE_FLOAT || t == DUCKDB_TYPE_DOUBLE || t == DUCKDB_TYPE_DECIMAL;
}

/* Turn a finished result into what `mode` asks for (on the VM thread). */
static SeaValue convert(SeaVM *vm, const char *fn, duckdb_result *res, int mode, SeaValue each_fn) {
    if (mode == R_EXEC) return int_or_number((int64_t)duckdb_rows_changed(res));
    char err[512];
    Cols c;
    const char *bad = cols_init(&c, res, err, sizeof err);
    if (bad) {
        cols_free(&c);
        return err_val(vm, "duckdb.%s: %s", fn, bad);
    }
    /* the chunks, kept for the columnar mode's two passes */
    size_t nch = 0, cch = 0;
    duckdb_data_chunk *chunks = NULL;
    for (;;) {
        duckdb_data_chunk ch = duckdb_fetch_chunk(*res);
        if (!ch) break;
        if (nch == cch) {
            cch = cch ? cch * 2 : 16;
            duckdb_data_chunk *nc = realloc(chunks, cch * sizeof *nc);
            if (!nc) { duckdb_destroy_data_chunk(&ch); break; }
            chunks = nc;
        }
        chunks[nch++] = ch;
        if (mode == R_ROW || mode == R_VALUE) break;     /* one row is enough */
    }
    Vec *vs = calloc(c.ncols ? c.ncols : 1, sizeof *vs);
    SeaValue r = SEA_NIL_VAL;
    if (!vs) {
        r = err_val(vm, "duckdb.%s: out of memory", fn);
        goto done;
    }
    if (mode == R_COLUMNS) {
        idx_t total = 0;
        for (size_t k = 0; k < nch; k++) total += duckdb_data_chunk_get_size(chunks[k]);
        SeaTable *out = sea_table_new(TABLE_MAP), *data = sea_table_new(TABLE_MAP);
        SeaArray *names = sea_array_new((uint32_t)c.ncols), *types = sea_array_new((uint32_t)c.ncols);
        for (idx_t j = 0; j < c.ncols; j++) {
            TI *t = &c.types[j];
            bool nulls = false;
            if (is_int_type(t->id) || is_float_type(t->id)) {
                for (size_t k = 0; k < nch && !nulls; k++) {
                    uint64_t *valid = duckdb_vector_get_validity(duckdb_data_chunk_get_vector(chunks[k], j));
                    idx_t n = duckdb_data_chunk_get_size(chunks[k]);
                    for (idx_t i = 0; valid && i < n && !nulls; i++)
                        nulls = !duckdb_validity_row_is_valid(valid, i);
                }
            }
            SeaValue col;
            if (!nulls && is_int_type(t->id)) {
                SeaIntArray *a = sea_int_array_new((uint32_t)total);
                for (size_t k = 0; k < nch; k++) {
                    Vec v = vec(duckdb_data_chunk_get_vector(chunks[k], j));
                    idx_t n = duckdb_data_chunk_get_size(chunks[k]);
                    for (idx_t i = 0; i < n; i++) {
                        SeaValue x = read_val(t, &v, i, 0);
                        int64_t iv = 0;
                        if (sea_is_int(x)) iv = sea_as_int(x);
                        else if (sea_is_number(x)) iv = (int64_t)sea_as_number(x);
                        else if (sea_is_bigint(x)) sea_bigint_to_i64(SEA_AS_BIGINT(x), &iv);   /* BIGINT fits */
                        val_release(x);
                        sea_int_array_push(a, iv);
                    }
                }
                col = sea_obj_val(&a->obj);
            } else if (!nulls && is_float_type(t->id)) {
                SeaFloatArray *a = sea_float_array_new((uint32_t)total);
                for (size_t k = 0; k < nch; k++) {
                    Vec v = vec(duckdb_data_chunk_get_vector(chunks[k], j));
                    idx_t n = duckdb_data_chunk_get_size(chunks[k]);
                    for (idx_t i = 0; i < n; i++) sea_float_array_push(a, sea_as_number(read_val(t, &v, i, 0)));
                }
                col = sea_obj_val(&a->obj);
            } else {
                SeaArray *a = sea_array_new((uint32_t)total);
                for (size_t k = 0; k < nch; k++) {
                    Vec v = vec(duckdb_data_chunk_get_vector(chunks[k], j));
                    idx_t n = duckdb_data_chunk_get_size(chunks[k]);
                    for (idx_t i = 0; i < n; i++) sea_array_push_move(a, read_val(t, &v, i, 0));
                }
                col = sea_obj_val(&a->obj);
            }
            sea_table_set(data, sea_obj_val(&c.names[j]->obj), col);
            val_release(col);
            sea_array_push(names, sea_obj_val(&c.names[j]->obj));
            char tn[512] = "";
            ti_str(t, tn, sizeof tn);
            sea_array_push_move(types, str_val(tn, strlen(tn)));
        }
        sea_table_set_str(out, "names", sea_obj_val(&names->obj));
        sea_table_set_str(out, "types", sea_obj_val(&types->obj));
        sea_table_set_str(out, "data", sea_obj_val(&data->obj));
        sea_table_set_str(out, "count", int_or_number((int64_t)total));
        sea_obj_release(&names->obj);
        sea_obj_release(&types->obj);
        sea_obj_release(&data->obj);
        r = sea_obj_val(&out->obj);
        goto done;
    }
    if (mode == R_ROW || mode == R_VALUE) {
        if (nch && duckdb_data_chunk_get_size(chunks[0]) > 0 && c.ncols > 0) {
            for (idx_t j = 0; j < c.ncols; j++) vs[j] = vec(duckdb_data_chunk_get_vector(chunks[0], j));
            r = mode == R_ROW ? make_row(&c, vs, 0) : read_val(&c.types[0], &vs[0], 0, 0);
        }
        goto done;
    }
    {
        SeaArray *rows = mode == R_ROWS ? sea_array_new(0) : NULL;
        int64_t count = 0;
        bool stop = false;
        for (size_t k = 0; k < nch && !stop; k++) {
            idx_t n = duckdb_data_chunk_get_size(chunks[k]);
            for (idx_t j = 0; j < c.ncols; j++) vs[j] = vec(duckdb_data_chunk_get_vector(chunks[k], j));
            for (idx_t i = 0; i < n && !stop; i++) {
                SeaValue row = make_row(&c, vs, i);
                count++;
                if (rows) {
                    sea_array_push_move(rows, row);
                    continue;
                }
                SeaValue rv = sea_vm_call(vm, each_fn, &row, 1);
                val_release(row);
                if (vm->panic_propagating) { stop = true; break; }
                if (sea_is_bool(rv) && !sea_as_bool(rv)) stop = true;
                val_release(rv);
            }
        }
        if (rows) r = sea_obj_val(&rows->obj);
        else r = vm->panic_propagating ? SEA_NIL_VAL : int_or_number(count);
    }
done:
    free(vs);
    for (size_t k = 0; k < nch; k++) duckdb_destroy_data_chunk(&chunks[k]);
    free(chunks);
    cols_free(&c);
    return r;
}

/* ======================================================================== */
/* Native functions                                                         */
/* ======================================================================== */

static char *sea_cstr_dup(SeaValue v) {
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return NULL;
    char *d = malloc(s->len + 1);
    if (d) {
        memcpy(d, s->data, s->len);
        d[s->len] = '\0';
    }
    sea_obj_release(&s->obj);
    return d;
}

/* native.version() */
static SeaValue fn_version(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    const char *v = duckdb_library_version();
    return str_val(v, strlen(v));
}

/* native.open(path, opts) → db handle | error */
static SeaValue fn_open(SeaVM *vm, int argc, SeaValue *args) {
    char *path = argc > 0 && !sea_is_nil(args[0]) ? sea_cstr_dup(args[0]) : NULL;
    if (argc > 0 && !sea_is_nil(args[0]) && !path) return err_val(vm, "duckdb.open: path must be a string");
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    duckdb_config cfg = NULL;
    if (duckdb_create_config(&cfg) == DuckDBError) {
        free(path);
        return err_val(vm, "duckdb.open: could not create a configuration");
    }
    uint32_t it = 0;
    SeaValue k, v;
    while (kv_next(opts, &it, &k, &v)) {
        if (!sea_is_string(k) || sea_is_nil(v)) continue;
        const char *name = SEA_AS_STRING(k)->data;
        char val[256];
        if (strcmp(name, "read_only") == 0) {
            if (!sea_is_truthy(v)) continue;
            name = "access_mode";
            snprintf(val, sizeof val, "READ_ONLY");
        } else if (sea_is_bool(v)) {
            snprintf(val, sizeof val, "%s", sea_as_bool(v) ? "true" : "false");
        } else {
            SeaString *s = sea_vm_tostring(vm, v);
            snprintf(val, sizeof val, "%s", s->data);
            sea_obj_release(&s->obj);
        }
        if (duckdb_set_config(cfg, name, val) == DuckDBError) {
            duckdb_destroy_config(&cfg);
            free(path);
            return err_val(vm, "duckdb.open: unknown option or invalid value: %s = %s", name, val);
        }
    }
    Obj *o = calloc(1, sizeof *o);
    if (!o) {
        duckdb_destroy_config(&cfg);
        free(path);
        return err_val(vm, "duckdb.open: out of memory");
    }
    o->kind = O_DB;
    char *err = NULL;
    duckdb_state st = duckdb_open_ext(path, &o->db, cfg, &err);
    duckdb_destroy_config(&cfg);
    if (st == DuckDBError) {
        SeaValue r = err_val(vm, "duckdb.open: %s", err ? err : "could not open the database");
        if (err) duckdb_free(err);
        free(path);
        free(o);
        return r;
    }
    free(path);
    int64_t h = obj_register(o, NULL);
    if (h < 0) {
        duckdb_close(&o->db);
        free(o);
        return err_val(vm, "duckdb.open: too many open handles");
    }
    return sea_int_val(h);
}

/* native.connect(db) → connection handle | error */
static SeaValue fn_connect(SeaVM *vm, int argc, SeaValue *args) {
    Obj *db = argc > 0 ? obj_get(args[0], M(O_DB)) : NULL;
    if (!db) return closed_err(vm, "connect", "database");
    Obj *o = calloc(1, sizeof *o);
    SeaValue r;
    if (!o) {
        r = err_val(vm, "duckdb.connect: out of memory");
    } else if (duckdb_connect(db->db, &o->con) == DuckDBError) {
        free(o);
        r = err_val(vm, "duckdb.connect: could not connect");
    } else {
        o->kind = O_CONN;
        pthread_mutex_init(&o->mu, NULL);
        int64_t h = obj_register(o, db);
        if (h < 0) {
            duckdb_disconnect(&o->con);
            pthread_mutex_destroy(&o->mu);
            free(o);
            r = err_val(vm, "duckdb.connect: too many open handles");
        } else {
            r = sea_int_val(h);
        }
    }
    obj_unref(db);
    return r;
}

/* native.close(h): any handle; closing twice is fine */
static SeaValue fn_close(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    Obj *o = argc > 0 ? obj_get(args[0], M(O_DB) | M(O_CONN) | M(O_STMT) | M(O_APP)) : NULL;
    if (!o) return SEA_FALSE_VAL;
    obj_close(o);
    obj_unref(o);
    return SEA_TRUE_VAL;
}

/* native.interrupt(conn): stop the query running on it */
static SeaValue fn_interrupt(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    Obj *o = argc > 0 ? obj_get(args[0], M(O_CONN)) : NULL;
    if (!o) return SEA_FALSE_VAL;
    duckdb_interrupt(o->con);
    obj_unref(o);
    return SEA_TRUE_VAL;
}

static const char *MODE_FN[] = { "query", "exec", "row", "value", "columns", "each" };

static int parse_mode(SeaValue v) {
    static const char *names[] = { "rows", "exec", "row", "value", "columns", "each" };
    for (int i = 0; i < 6; i++)
        if (sea_is_string(v) && strcmp(SEA_AS_STRING(v)->data, names[i]) == 0) return i;
    return -1;
}

/* Parameters from args[first..argc): one container (positional sequence, or
 * a table/struct/map of named values) or several positional values. */
static const char *job_params(SeaVM *vm, Job *j, int argc, SeaValue *args, int first, char *err, size_t errlen) {
    int n = argc - first;
    if (n <= 0) return NULL;
    SeaValue p = args[first];
    bool container = n == 1 && (is_seq(p) || sea_is_table(p) || sea_is_struct(p) || sea_is_map(p));
    VErr ve = { "" };
    if (container && !is_seq(p)) {
        uint32_t it = 0, cnt = 0;
        SeaValue k, v;
        while (kv_next(p, &it, &k, &v)) cnt++;
        j->pvals = calloc(cnt ? cnt : 1, sizeof *j->pvals);
        j->pnames = calloc(cnt ? cnt : 1, sizeof *j->pnames);
        if (!j->pvals || !j->pnames) return "out of memory";
        it = 0;
        while (kv_next(p, &it, &k, &v)) {
            if (!sea_is_string(k)) {
                snprintf(err, errlen, "named parameters need string keys");
                return err;
            }
            duckdb_value dv = to_dvalue(vm, v, &ve, 0);
            if (!dv) {
                snprintf(err, errlen, "parameter $%s: %s", SEA_AS_STRING(k)->data, ve.msg);
                return err;
            }
            j->pnames[j->nparams] = dupstr(SEA_AS_STRING(k)->data);
            j->pvals[j->nparams++] = dv;
        }
        if (!cnt) { free(j->pnames); j->pnames = calloc(1, sizeof *j->pnames); }
        return NULL;
    }
    uint32_t cnt = container ? seq_len(p) : (uint32_t)n;
    j->pvals = calloc(cnt ? cnt : 1, sizeof *j->pvals);
    if (!j->pvals) return "out of memory";
    for (uint32_t i = 0; i < cnt; i++) {
        SeaValue v = container ? seq_at(p, i) : args[first + (int)i];
        duckdb_value dv = to_dvalue(vm, v, &ve, 0);
        if (!dv) {
            snprintf(err, errlen, "parameter %u: %s", i + 1, ve.msg);
            return err;
        }
        j->pvals[j->nparams++] = dv;
    }
    return NULL;
}

/* native.run(h, sql, mode, fn, params...): h is a connection (with sql) or
 * a prepared statement (sql nil). Modes: rows, exec, row, value, columns,
 * each (fn called per row). In a goroutine the query runs on a worker
 * thread; this call parks and runs again when it is done (io->aux[0] holds
 * the job across the park). */
static SeaValue fn_run(SeaVM *vm, int argc, SeaValue *args) {
    SeaIoCall *io = sea_io_begin(vm, -1);
    Job *j = (Job *)(intptr_t)io->aux[0];
    int mode = argc > 2 ? parse_mode(args[2]) : -1;
    const char *fn = mode >= 0 ? MODE_FN[mode] : "query";
    if (!j) {
        if (mode < 0) { sea_io_end(vm); return err_val(vm, "duckdb: unknown query mode"); }
        if (mode == R_EACH && (argc < 4 || (!sea_is_closure(args[3]) && !sea_is_function(args[3])))) {
            sea_io_end(vm);
            return err_val(vm, "duckdb.each: a function expected");
        }
        Obj *o = argc > 0 ? obj_get(args[0], M(O_CONN) | M(O_STMT)) : NULL;
        if (!o) { sea_io_end(vm); return err_val(vm, "duckdb.%s: the connection or statement is closed", fn); }
        j = calloc(1, sizeof *j);
        if (!j) { obj_unref(o); sea_io_end(vm); return err_val(vm, "duckdb.%s: out of memory", fn); }
        atomic_init(&j->refs, 1);
        atomic_init(&j->done, 0);
        j->efd = j->wfd = -1;
        if (o->kind == O_STMT) {
            j->stmt = o;
            j->conn = conn_of(o);
            atomic_fetch_add(&j->conn->refs, 1);
        } else {
            j->conn = o;
            j->sql = argc > 1 ? sea_cstr_dup(args[1]) : NULL;
            if (!j->sql) {
                job_unref(j);
                sea_io_end(vm);
                return err_val(vm, "duckdb.%s: SQL string expected", fn);
            }
        }
        char err[512];
        const char *perr = job_params(vm, j, argc, args, 4, err, sizeof err);
        if (perr) {
            SeaValue r = err_val(vm, "duckdb.%s: %s", fn, perr);
            job_unref(j);
            sea_io_end(vm);
            return r;
        }
        bool offload = vm->goroutine != NULL && vm->native_depth == 0;
        if (offload && pool_submit(j)) {
            io->aux[0] = (int64_t)(intptr_t)j;
        } else {
            job_run(j);
            atomic_store(&j->done, 1);
        }
    }
    while (!atomic_load(&j->done)) {
        int w = sea_io_wait(vm, j->efd, SEA_IO_READ);
        if (w == SEA_IO_PARK) return SEA_PARK_VAL;
        if (w != SEA_IO_READY) {
            io->aux[0] = 0;
            sea_io_end(vm);
            job_unref(j);                     /* the worker still holds one */
            return err_val(vm, "duckdb.%s: interrupted while waiting", fn);
        }
    }
    io->aux[0] = 0;
    sea_io_end(vm);                           /* before any callback into Sea */
    SeaValue r;
    if (j->err) r = err_val(vm, "duckdb.%s: %s", fn, j->err);
    else r = convert(vm, fn, &j->res, mode, argc > 3 ? args[3] : SEA_NIL_VAL);
    job_unref(j);
    return r;
}

/* native.prepare(conn, sql) → statement handle | error */
static SeaValue fn_prepare(SeaVM *vm, int argc, SeaValue *args) {
    Obj *c = argc > 0 ? obj_get(args[0], M(O_CONN)) : NULL;
    if (!c) return closed_err(vm, "prepare", "connection");
    char *sql = argc > 1 ? sea_cstr_dup(args[1]) : NULL;
    if (!sql) { obj_unref(c); return err_val(vm, "duckdb.prepare: SQL string expected"); }
    Obj *o = calloc(1, sizeof *o);
    SeaValue r;
    if (!o) {
        r = err_val(vm, "duckdb.prepare: out of memory");
    } else {
        o->kind = O_STMT;
        pthread_mutex_lock(&c->mu);
        duckdb_state st = duckdb_prepare(c->con, sql, &o->stmt);
        if (st == DuckDBError) {
            r = err_val(vm, "duckdb.prepare: %s", duckdb_prepare_error(o->stmt));
            duckdb_destroy_prepare(&o->stmt);
        }
        pthread_mutex_unlock(&c->mu);
        if (st == DuckDBError) {
            free(o);
        } else {
            int64_t h = obj_register(o, c);
            if (h < 0) {
                pthread_mutex_lock(&c->mu);
                duckdb_destroy_prepare(&o->stmt);
                pthread_mutex_unlock(&c->mu);
                free(o);
                r = err_val(vm, "duckdb.prepare: too many open handles");
            } else {
                r = sea_int_val(h);
            }
        }
    }
    free(sql);
    obj_unref(c);
    return r;
}

/* native.params(stmt) → array of parameter names ("1", "2"... when positional) */
static SeaValue fn_params(SeaVM *vm, int argc, SeaValue *args) {
    Obj *s = argc > 0 ? obj_get(args[0], M(O_STMT)) : NULL;
    if (!s) return closed_err(vm, "params", "statement");
    Obj *c = conn_of(s);
    SeaArray *a = sea_array_new(0);
    pthread_mutex_lock(&c->mu);
    idx_t n = duckdb_nparams(s->stmt);
    for (idx_t i = 1; i <= n; i++) {
        const char *nm = duckdb_parameter_name(s->stmt, i);
        char num[24];
        if (!nm) { snprintf(num, sizeof num, "%llu", (unsigned long long)i); nm = num; }
        sea_array_push_move(a, str_val(nm, strlen(nm)));
        if (nm != num) duckdb_free((void *)nm);
    }
    pthread_mutex_unlock(&c->mu);
    obj_unref(s);
    return sea_obj_val(&a->obj);
}

/* native.appender(conn, table, schema) → appender handle | error */
static SeaValue fn_appender(SeaVM *vm, int argc, SeaValue *args) {
    Obj *c = argc > 0 ? obj_get(args[0], M(O_CONN)) : NULL;
    if (!c) return closed_err(vm, "appender", "connection");
    char *table = argc > 1 ? sea_cstr_dup(args[1]) : NULL;
    char *schema = argc > 2 && !sea_is_nil(args[2]) ? sea_cstr_dup(args[2]) : NULL;
    Obj *o = table ? calloc(1, sizeof *o) : NULL;
    SeaValue r = SEA_NIL_VAL;
    if (!table) {
        r = err_val(vm, "duckdb.appender: table name expected");
    } else if (!o) {
        r = err_val(vm, "duckdb.appender: out of memory");
    } else {
        o->kind = O_APP;
        pthread_mutex_lock(&c->mu);
        if (duckdb_appender_create(c->con, schema, table, &o->app) == DuckDBError) {
            r = err_val(vm, "duckdb.appender: %s", duckdb_appender_error(o->app));
            duckdb_appender_destroy(&o->app);
        } else {
            duckdb_table_description desc = NULL;
            o->ncols = duckdb_appender_column_count(o->app);
            o->colnames = calloc(o->ncols ? o->ncols : 1, sizeof *o->colnames);
            if (o->colnames && duckdb_table_description_create(c->con, schema, table, &desc) == DuckDBSuccess) {
                for (idx_t i = 0; i < o->ncols; i++) {
                    char *nm = duckdb_table_description_get_column_name(desc, i);
                    o->colnames[i] = dupstr(nm ? nm : "");
                    if (nm) duckdb_free(nm);
                }
            }
            duckdb_table_description_destroy(&desc);
        }
        pthread_mutex_unlock(&c->mu);
        if (!sea_is_nil(r)) {
            free(o->colnames);
            free(o);
        } else {
            int64_t h = obj_register(o, c);
            if (h < 0) {
                pthread_mutex_lock(&c->mu);
                duckdb_appender_destroy(&o->app);
                pthread_mutex_unlock(&c->mu);
                for (idx_t i = 0; i < o->ncols; i++) free(o->colnames[i]);
                free(o->colnames);
                free(o);
                r = err_val(vm, "duckdb.appender: too many open handles");
            } else {
                r = sea_int_val(h);
            }
        }
    }
    free(table);
    free(schema);
    obj_unref(c);
    return r;
}

/* Append one value (under the connection's lock). */
static bool append_one(SeaVM *vm, Obj *a, SeaValue v, char *err, size_t errlen) {
    duckdb_state st;
    if (sea_is_nil(v)) st = duckdb_append_null(a->app);
    else if (sea_is_bool(v)) st = duckdb_append_bool(a->app, sea_as_bool(v));
    else if (sea_is_int(v)) st = duckdb_append_int64(a->app, sea_as_int(v));
    else if (sea_is_number(v)) st = duckdb_append_double(a->app, sea_as_number(v));
    else if (sea_is_string(v) || sea_is_rope(v)) {
        SeaString *s = sea_value_get_flat_string(v);
        st = duckdb_append_varchar_length(a->app, s->data, s->len);
        sea_obj_release(&s->obj);
    } else {
        VErr ve = { "" };
        duckdb_value dv = to_dvalue(vm, v, &ve, 0);
        if (!dv) {
            snprintf(err, errlen, "%s", ve.msg);
            return false;
        }
        st = duckdb_append_value(a->app, dv);
        duckdb_destroy_value(&dv);
    }
    if (st == DuckDBError) {
        snprintf(err, errlen, "%s", duckdb_appender_error(a->app));
        return false;
    }
    return true;
}

/* One row: a sequence in column order, or a table/struct/map by column name
 * (absent columns get their default). */
static bool append_row(SeaVM *vm, Obj *a, SeaValue row, char *err, size_t errlen) {
    if (is_seq(row)) {
        uint32_t n = seq_len(row);
        if ((idx_t)n != a->ncols) {
            snprintf(err, errlen, "the table has %llu columns, the row %u values", (unsigned long long)a->ncols, n);
            return false;
        }
        for (uint32_t i = 0; i < n; i++)
            if (!append_one(vm, a, seq_at(row, i), err, errlen)) return false;
    } else if (sea_is_table(row) || sea_is_struct(row) || sea_is_map(row)) {
        uint32_t it = 0, known = 0;
        SeaValue k, v;
        while (kv_next(row, &it, &k, &v)) {
            bool found = false;
            for (idx_t i = 0; sea_is_string(k) && i < a->ncols && !found; i++)
                found = a->colnames[i] && strcmp(a->colnames[i], SEA_AS_STRING(k)->data) == 0;
            if (!found) {
                SeaString *ks = sea_vm_tostring(vm, k);
                snprintf(err, errlen, "the table has no column \"%s\"", ks->data);
                sea_obj_release(&ks->obj);
                return false;
            }
            known++;
        }
        for (idx_t i = 0; i < a->ncols; i++) {
            SeaValue v2 = a->colnames[i] ? field(row, a->colnames[i]) : SEA_NIL_VAL;
            if (sea_is_nil(v2)) {
                if (duckdb_append_default(a->app) == DuckDBError) {
                    snprintf(err, errlen, "%s", duckdb_appender_error(a->app));
                    return false;
                }
            } else if (!append_one(vm, a, v2, err, errlen)) {
                return false;
            }
        }
    } else {
        snprintf(err, errlen, "a row must be a sequence or a table, got %s", sea_type_name(row));
        return false;
    }
    if (duckdb_appender_end_row(a->app) == DuckDBError) {
        snprintf(err, errlen, "%s", duckdb_appender_error(a->app));
        return false;
    }
    return true;
}

/* native.append(app, row, many): many = true → row is a sequence of rows */
static SeaValue fn_append(SeaVM *vm, int argc, SeaValue *args) {
    Obj *a = argc > 0 ? obj_get(args[0], M(O_APP)) : NULL;
    if (!a) return closed_err(vm, "append", "appender");
    bool many = argc > 2 && sea_is_truthy(args[2]);
    SeaValue rows = argc > 1 ? args[1] : SEA_NIL_VAL;
    char err[512] = "";
    bool ok = true;
    int64_t n = 0;
    Obj *c = conn_of(a);
    pthread_mutex_lock(&c->mu);
    if (many) {
        if (!is_seq(rows)) {
            snprintf(err, sizeof err, "a sequence of rows expected");
            ok = false;
        }
        for (uint32_t i = 0; ok && i < seq_len(rows); i++) {
            ok = append_row(vm, a, seq_at(rows, i), err, sizeof err);
            if (ok) n++;
        }
    } else {
        ok = append_row(vm, a, rows, err, sizeof err);
        n = ok;
    }
    pthread_mutex_unlock(&c->mu);
    obj_unref(a);
    if (!ok) {
        if (many) return err_val(vm, "duckdb.append_rows: row %lld: %s", (long long)n + 1, err);
        return err_val(vm, "duckdb.append: %s", err);
    }
    return int_or_number(n);
}

/* native.flush(app) → true | error */
static SeaValue fn_flush(SeaVM *vm, int argc, SeaValue *args) {
    Obj *a = argc > 0 ? obj_get(args[0], M(O_APP)) : NULL;
    if (!a) return closed_err(vm, "flush", "appender");
    Obj *c = conn_of(a);
    pthread_mutex_lock(&c->mu);
    SeaValue r = SEA_TRUE_VAL;
    if (duckdb_appender_flush(a->app) == DuckDBError)
        r = err_val(vm, "duckdb.flush: %s", duckdb_appender_error(a->app));
    pthread_mutex_unlock(&c->mu);
    obj_unref(a);
    return r;
}

/* ---- Module entry point --------------------------------------------------- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "version",   fn_version);
    sea_mod_set(mod, "open",      fn_open);
    sea_mod_set(mod, "connect",   fn_connect);
    sea_mod_set(mod, "close",     fn_close);
    sea_mod_set(mod, "interrupt", fn_interrupt);
    sea_mod_set(mod, "run",       fn_run);
    sea_mod_set(mod, "prepare",   fn_prepare);
    sea_mod_set(mod, "params",    fn_params);
    sea_mod_set(mod, "appender",  fn_appender);
    sea_mod_set(mod, "append",    fn_append);
    sea_mod_set(mod, "flush",     fn_flush);
    return sea_obj_val(&mod->obj);
}
