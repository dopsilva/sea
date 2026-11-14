/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * libs/sqlite/sqlite_native.c — SQLite for Sea (the native half; the API is
 * in sqlite.sea).
 *
 * SQLite is compiled into this module from the amalgamation (vendor/,
 * fetched by fetch.sh). Handles are ints naming slots in a registry, checked
 * on every use (a closed or forged handle is an error, never a crash);
 * closing a database closes its statements.
 *
 * In a goroutine a query runs on a worker thread and its rows are copied
 * into C memory; the goroutine parks on an eventfd through the netpoller and
 * the rows become Sea values back on its own thread. On the main thread (or
 * under a native callback) queries run in place. `each` steps in place and
 * calls the function per row without holding the connection lock, so the
 * function may use the same database.
 */

#include "sea.h"
#include "sea_netpoll.h"
#include "sea_rope.h"
#include "sqlite3.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sea_compat.h"
#include <unistd.h>

#define SEA_INT_MAX ((INT64_C(1) << 47) - 1)
#define SEA_INT_MIN (-(INT64_C(1) << 47))

/* ---- Helpers ---------------------------------------------------------------- */

static inline void val_release(SeaValue v) { if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v)); }

static SeaValue str_val(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }

static SeaValue err_val(SeaVM *vm, const char *fmt, ...) {
    char msg[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return sea_make_error_str(vm, msg);
}

static char *dupn(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = '\0'; }
    return d;
}

static char *cstr_dup(SeaValue v) {
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return NULL;
    char *d = dupn(s->data, s->len);
    sea_obj_release(&s->obj);
    return d;
}

/* An integer column: int within Sea's range, number while exact (2^53),
 * bigint beyond. */
static SeaValue int_value(int64_t v) {
    if (v >= SEA_INT_MIN && v <= SEA_INT_MAX) return sea_int_val(v);
    if (v >= -(INT64_C(1) << 53) && v <= (INT64_C(1) << 53)) return sea_number_val((double)v);
    return sea_obj_val(&sea_bigint_from_i64(v)->obj);
}

static bool kv_next(SeaValue o, uint32_t *it, SeaValue *k, SeaValue *v) {
    if (sea_is_table(o)) return sea_table_next(SEA_AS_TABLE(o), it, k, v);
    if (sea_is_struct(o)) return sea_struct_next(SEA_AS_STRUCT(o), it, k, v);
    if (sea_is_map(o)) return sea_map_next(SEA_AS_MAP(o), it, k, v);
    return false;
}

static bool is_seq_table(SeaValue v) {
    if (!sea_is_table(v)) return false;
    SeaTable *t = SEA_AS_TABLE(v);
    uint32_t count = sea_table_count(t), it = 0;
    if (count == 0) return false;
    SeaValue k, x;
    while (sea_table_next(t, &it, &k, &x))
        if (!sea_is_int(k) || sea_as_int(k) < 0 || (uint64_t)sea_as_int(k) >= count) return false;
    return true;
}

static bool is_seq(SeaValue v) { return sea_is_array(v) || is_seq_table(v); }

static uint32_t seq_len(SeaValue v) {
    return sea_is_array(v) ? sea_array_len(SEA_AS_ARRAY(v)) : sea_table_count(SEA_AS_TABLE(v));
}

static SeaValue seq_at(SeaValue v, uint32_t i) {
    if (sea_is_array(v)) return sea_array_get(SEA_AS_ARRAY(v), (int64_t)i);
    SeaValue x;
    if (!sea_table_get(SEA_AS_TABLE(v), sea_int_val((int64_t)i), &x)) x = SEA_NIL_VAL;
    return x;
}

static SeaValue field(SeaValue o, const char *name) {
    SeaValue v = SEA_NIL_VAL;
    if (sea_is_table(o)) sea_table_get_str(SEA_AS_TABLE(o), name, &v);
    else if (sea_is_struct(o)) {
        SeaString *k = sea_string_intern(name, strlen(name));
        v = sea_struct_get(SEA_AS_STRUCT(o), k);
        sea_obj_release(&k->obj);
    }
    return v;
}

/* ======================================================================== */
/* Handle registry                                                          */
/* ======================================================================== */

typedef enum { O_DB = 1, O_STMT } OKind;

typedef struct Obj Obj;
struct Obj {
    OKind          kind;
    atomic_int     refs;
    bool           closed;
    uint32_t       slot;
    Obj           *parent;
    Obj           *child, *sib;
    pthread_mutex_t mu;            /* database: one statement sequence at a time */
    sqlite3       *db;
    sqlite3_stmt  *stmt;
    char          *path;           /* database: for connect() */
    int            flags;
};

#define SLOT_BITS 20
#define MAX_SLOTS (1u << SLOT_BITS)

static pthread_mutex_t g_reg = PTHREAD_MUTEX_INITIALIZER;
static Obj      **g_slots;
static uint32_t  *g_gens;
static uint32_t   g_nslots, g_cap;
static uint32_t  *g_free;
static uint32_t   g_nfree;

static Obj *db_of(Obj *o) { return o->kind == O_DB ? o : o->parent; }

static void obj_destroy(Obj *o);

static void obj_unref(Obj *o) {
    if (o && atomic_fetch_sub(&o->refs, 1) == 1) obj_destroy(o);
}

static void obj_destroy(Obj *o) {
    if (o->kind == O_STMT) {
        Obj *d = db_of(o);
        pthread_mutex_lock(&d->mu);
        sqlite3_finalize(o->stmt);
        pthread_mutex_unlock(&d->mu);
    } else {
        sqlite3_close_v2(o->db);
        pthread_mutex_destroy(&o->mu);
        free(o->path);
    }
    Obj *p = o->parent;
    free(o);
    obj_unref(p);
}

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
        if (!na) return;
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
    for (size_t i = 0; i < n; i++)
        if (list[i]->kind == O_DB) sqlite3_interrupt(list[i]->db);
    for (size_t i = 0; i < n; i++) obj_unref(list[i]);
    free(list);
}

/* ======================================================================== */
/* Parameters: Sea values copied into C (bound later, maybe on a worker)    */
/* ======================================================================== */

enum { V_NULL, V_INT, V_REAL, V_TEXT, V_BLOB };

typedef struct {
    int     type;
    int64_t i;
    double  d;
    char   *s;          /* text/blob bytes */
    size_t  n;
    char   *name;       /* named parameter (without prefix), or NULL */
} Param;

static void params_free(Param *p, int n) {
    for (int i = 0; i < n; i++) { free(p[i].s); free(p[i].name); }
    free(p);
}

/* A Sea value as a parameter; false with err for a type SQLite cannot hold. */
static bool param_of(SeaValue v, Param *p, char *err, size_t errlen) {
    memset(p, 0, sizeof *p);
    if (sea_is_nil(v)) { p->type = V_NULL; return true; }
    if (sea_is_bool(v)) { p->type = V_INT; p->i = sea_as_bool(v); return true; }
    if (sea_is_int(v)) { p->type = V_INT; p->i = sea_as_int(v); return true; }
    if (sea_is_number(v)) { p->type = V_REAL; p->d = sea_as_number(v); return true; }
    if (sea_is_bigint(v)) {
        int64_t x;
        if (sea_bigint_to_i64(SEA_AS_BIGINT(v), &x)) { p->type = V_INT; p->i = x; return true; }
        SeaString *s = sea_bigint_to_string(SEA_AS_BIGINT(v), 10);       /* too wide: as text */
        p->type = V_TEXT;
        p->s = dupn(s->data, s->len);
        p->n = s->len;
        sea_obj_release(&s->obj);
        return true;
    }
    if (sea_is_string(v) || sea_is_rope(v)) {
        SeaString *s = sea_value_get_flat_string(v);
        p->type = V_TEXT;
        p->s = dupn(s->data, s->len);
        p->n = s->len;
        sea_obj_release(&s->obj);
        return true;
    }
    SeaValue b = field(v, "__sqlite_blob");
    if (sea_is_string(b) || sea_is_rope(b)) {
        SeaString *s = sea_value_get_flat_string(b);
        p->type = V_BLOB;
        p->s = dupn(s->data, s->len);
        p->n = s->len;
        sea_obj_release(&s->obj);
        return true;
    }
    snprintf(err, errlen, "a value of type %s cannot be a parameter (use sqlite.blob(s) for bytes)", sea_type_name(v));
    return false;
}

/* Parameters from args[first..argc): one container (a sequence, or a table
 * of named values) or several positional values. */
static bool collect_params(int argc, SeaValue *args, int first, Param **out, int *nout, char *err, size_t errlen) {
    *out = NULL;
    *nout = 0;
    int n = argc - first;
    if (n <= 0) return true;
    SeaValue p = args[first];
    bool blob = !sea_is_nil(field(p, "__sqlite_blob"));
    bool container = n == 1 && !blob && (is_seq(p) || sea_is_table(p) || sea_is_struct(p) || sea_is_map(p));
    if (container && !is_seq(p)) {
        uint32_t it = 0, cnt = 0;
        SeaValue k, v;
        while (kv_next(p, &it, &k, &v)) cnt++;
        Param *ps = calloc(cnt ? cnt : 1, sizeof *ps);
        it = 0;
        int i = 0;
        while (kv_next(p, &it, &k, &v)) {
            if (!sea_is_string(k)) { snprintf(err, errlen, "named parameters need string keys"); params_free(ps, i); return false; }
            if (!param_of(v, &ps[i], err, errlen)) { params_free(ps, i); return false; }
            const char *nm = SEA_AS_STRING(k)->data;
            if (*nm == ':' || *nm == '@' || *nm == '$') nm++;
            ps[i].name = dupn(nm, strlen(nm));
            i++;
        }
        *out = ps;
        *nout = i;
        return true;
    }
    uint32_t cnt = container ? seq_len(p) : (uint32_t)n;
    Param *ps = calloc(cnt ? cnt : 1, sizeof *ps);
    for (uint32_t i = 0; i < cnt; i++) {
        SeaValue v = container ? seq_at(p, i) : args[first + (int)i];
        if (!param_of(v, &ps[i], err, errlen)) {
            params_free(ps, (int)i);
            char tmp[300];
            snprintf(tmp, sizeof tmp, "parameter %u: %s", i + 1, err);
            snprintf(err, errlen, "%s", tmp);
            return false;
        }
    }
    *out = ps;
    *nout = (int)cnt;
    return true;
}

/* Bind params to st; false with err. */
static bool bind_params(sqlite3_stmt *st, Param *ps, int n, char *err, size_t errlen) {
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    int want = sqlite3_bind_parameter_count(st);
    bool named = n > 0 && ps[0].name;
    if (!named && n != want) {
        snprintf(err, errlen, "expected %d parameter%s, got %d", want, want == 1 ? "" : "s", n);
        return false;
    }
    for (int i = 0; i < n; i++) {
        int idx = i + 1;
        if (named) {
            static const char prefixes[] = ":@$";
            idx = 0;
            char buf[300];
            for (int k = 0; k < 3 && !idx; k++) {
                snprintf(buf, sizeof buf, "%c%s", prefixes[k], ps[i].name);
                idx = sqlite3_bind_parameter_index(st, buf);
            }
            if (!idx) { snprintf(err, errlen, "unknown parameter :%s", ps[i].name); return false; }
        }
        int rc;
        switch (ps[i].type) {
            case V_NULL: rc = sqlite3_bind_null(st, idx); break;
            case V_INT:  rc = sqlite3_bind_int64(st, idx, ps[i].i); break;
            case V_REAL: rc = sqlite3_bind_double(st, idx, ps[i].d); break;
            case V_TEXT: rc = sqlite3_bind_text64(st, idx, ps[i].s, ps[i].n, SQLITE_TRANSIENT, SQLITE_UTF8); break;
            default:     rc = sqlite3_bind_blob64(st, idx, ps[i].s, ps[i].n, SQLITE_TRANSIENT); break;
        }
        if (rc != SQLITE_OK) {
            snprintf(err, errlen, "could not bind parameter %d: %s", idx, sqlite3_errstr(rc));
            return false;
        }
    }
    return true;
}

/* ======================================================================== */
/* Query jobs                                                               */
/* ======================================================================== */

enum { R_ROWS, R_EXEC, R_ROW, R_VALUE };

typedef struct { int type; int64_t i; double d; char *s; size_t n; } Cell;

typedef struct Job {
    struct Job *next;
    atomic_int  refs, done;
    int         efd, wfd;
    Obj        *db, *stmt;
    char       *sql;
    Param      *params;
    int         nparams;
    int         mode;
    /* results */
    int         ncols;
    char      **names;
    Cell       *cells;
    size_t      nrows, cap;
    int64_t     changes, last_id;
    char       *err;
} Job;

static void job_free(Job *j) {
    params_free(j->params, j->nparams);
    for (int c = 0; c < j->ncols; c++) free(j->names[c]);
    free(j->names);
    for (size_t i = 0; i < j->nrows * (size_t)j->ncols; i++) free(j->cells[i].s);
    free(j->cells);
    free(j->sql);
    free(j->err);
    if (j->efd >= 0) {
        sea_netpoll_close(j->efd);
        sea_wake_close_wr(j->efd, j->wfd);
    }
    obj_unref(j->stmt);
    obj_unref(j->db);
    free(j);
}

static void job_unref(Job *j) {
    if (atomic_fetch_sub(&j->refs, 1) == 1) job_free(j);
}

static void job_fail(Job *j, const char *msg) {
    free(j->err);
    j->err = dupn(msg, strlen(msg));
}

/* Step st to the end (or one row), copying rows into j. SQLITE_DONE on success. */
static int step_rows(Job *j, sqlite3_stmt *st, bool keep) {
    int rc;
    bool named = false;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (!keep) continue;
        if (!named) {
            j->ncols = sqlite3_column_count(st);
            j->names = calloc(j->ncols ? (size_t)j->ncols : 1, sizeof *j->names);
            for (int c = 0; c < j->ncols; c++) {
                const char *nm = sqlite3_column_name(st, c);
                j->names[c] = dupn(nm ? nm : "", nm ? strlen(nm) : 0);
            }
            named = true;
        }
        if (j->nrows == j->cap) {
            j->cap = j->cap ? j->cap * 2 : 16;
            j->cells = realloc(j->cells, j->cap * (size_t)(j->ncols ? j->ncols : 1) * sizeof *j->cells);
        }
        Cell *row = j->cells + j->nrows * (size_t)j->ncols;
        for (int c = 0; c < j->ncols; c++) {
            Cell *cell = &row[c];
            memset(cell, 0, sizeof *cell);
            int t = sqlite3_column_type(st, c);
            switch (t) {
                case SQLITE_INTEGER: cell->type = V_INT; cell->i = sqlite3_column_int64(st, c); break;
                case SQLITE_FLOAT:   cell->type = V_REAL; cell->d = sqlite3_column_double(st, c); break;
                case SQLITE_TEXT: {
                    const unsigned char *s = sqlite3_column_text(st, c);
                    cell->type = V_TEXT;
                    cell->n = (size_t)sqlite3_column_bytes(st, c);
                    cell->s = dupn((const char *)s, cell->n);
                    break;
                }
                case SQLITE_BLOB: {
                    const void *b = sqlite3_column_blob(st, c);
                    cell->type = V_BLOB;
                    cell->n = (size_t)sqlite3_column_bytes(st, c);
                    cell->s = dupn(b ? b : "", cell->n);
                    break;
                }
                default: cell->type = V_NULL; break;
            }
        }
        j->nrows++;
        if (j->mode == R_ROW || j->mode == R_VALUE) { rc = SQLITE_DONE; break; }
    }
    if (!named && keep && j->ncols == 0) {
        j->ncols = sqlite3_column_count(st);
        j->names = calloc(j->ncols ? (size_t)j->ncols : 1, sizeof *j->names);
        for (int c = 0; c < j->ncols; c++) {
            const char *nm = sqlite3_column_name(st, c);
            j->names[c] = dupn(nm ? nm : "", nm ? strlen(nm) : 0);
        }
    }
    return rc;
}

static void job_run(Job *j) {
    Obj *d = j->db;
    sqlite3 *db = d->db;
    char err[600];
    pthread_mutex_lock(&d->mu);
    int64_t before = sqlite3_total_changes64(db);
    if (j->stmt) {
        sqlite3_stmt *st = j->stmt->stmt;
        if (!bind_params(st, j->params, j->nparams, err, sizeof err)) job_fail(j, err);
        else {
            int rc = step_rows(j, st, j->mode != R_EXEC);
            if (rc != SQLITE_DONE) job_fail(j, sqlite3_errmsg(db));
            sqlite3_reset(st);
        }
    } else {
        /* every statement of the SQL in turn (parameters go to the first) */
        const char *tail = j->sql;
        bool first = true;
        while (tail && *tail && !j->err) {
            sqlite3_stmt *st = NULL;
            const char *next = NULL;
            if (sqlite3_prepare_v2(db, tail, -1, &st, &next) != SQLITE_OK) { job_fail(j, sqlite3_errmsg(db)); break; }
            if (!st) { tail = next; continue; }             /* whitespace or a comment */
            if (first && !bind_params(st, j->params, j->nparams, err, sizeof err)) {
                job_fail(j, err);
            } else if (!first && sqlite3_bind_parameter_count(st) > 0) {
                job_fail(j, "parameters can only be used with a single statement");
            } else {
                /* rows come from the last statement that returns any */
                bool keep = j->mode != R_EXEC;
                if (keep && sqlite3_column_count(st) > 0 && !first) {
                    for (size_t i = 0; i < j->nrows * (size_t)j->ncols; i++) free(j->cells[i].s);
                    for (int c = 0; c < j->ncols; c++) free(j->names[c]);
                    free(j->names);
                    j->names = NULL;
                    j->nrows = 0;
                    j->ncols = 0;
                }
                int rc = step_rows(j, st, keep && sqlite3_column_count(st) > 0);
                if (rc != SQLITE_DONE) job_fail(j, sqlite3_errmsg(db));
            }
            sqlite3_finalize(st);
            first = false;
            tail = next;
        }
    }
    j->changes = sqlite3_total_changes64(db) - before;
    j->last_id = sqlite3_last_insert_rowid(db);
    pthread_mutex_unlock(&d->mu);
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

/* ---- results → Sea ------------------------------------------------------------ */

static SeaValue cell_value(const Cell *c) {
    switch (c->type) {
        case V_INT:  return int_value(c->i);
        case V_REAL: return sea_number_val(c->d);
        case V_TEXT: case V_BLOB: return str_val(c->s, c->n);
        default:     return SEA_NIL_VAL;
    }
}

static SeaValue row_value(SeaString **keys, int ncols, const Cell *row) {
    SeaTable *t = sea_table_new(TABLE_MAP);
    for (int c = 0; c < ncols; c++) {
        SeaValue v = cell_value(&row[c]);
        sea_table_set(t, sea_obj_val(&keys[c]->obj), v);
        val_release(v);
    }
    return sea_obj_val(&t->obj);
}

static SeaString **make_keys(char **names, int n) {
    SeaString **keys = calloc(n ? (size_t)n : 1, sizeof *keys);
    for (int c = 0; c < n; c++) {
        size_t len = strlen(names[c]);
        SeaString *k = sea_string_find_intern(names[c], len);
        keys[c] = k ? k : sea_string_new(names[c], len);
    }
    return keys;
}

static void free_keys(SeaString **keys, int n) {
    for (int c = 0; c < n; c++) sea_obj_release(&keys[c]->obj);
    free(keys);
}

static SeaValue job_result(SeaVM *vm, Job *j) {
    if (j->mode == R_EXEC) return int_value(j->changes);
    if (j->mode == R_VALUE) return j->nrows && j->ncols ? cell_value(&j->cells[0]) : SEA_NIL_VAL;
    SeaString **keys = make_keys(j->names, j->ncols);
    SeaValue r;
    if (j->mode == R_ROW) {
        r = j->nrows ? row_value(keys, j->ncols, j->cells) : SEA_NIL_VAL;
    } else {
        SeaArray *a = sea_array_new((uint32_t)(j->nrows ? j->nrows : 1));
        for (size_t i = 0; i < j->nrows; i++) {
            SeaValue row = row_value(keys, j->ncols, j->cells + i * (size_t)j->ncols);
            sea_array_push(a, row);
            val_release(row);
        }
        r = sea_obj_val(&a->obj);
    }
    free_keys(keys, j->ncols);
    (void)vm;
    return r;
}

/* ======================================================================== */
/* Native functions                                                         */
/* ======================================================================== */

static SeaValue fn_version(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    const char *v = sqlite3_libversion();
    return str_val(v, strlen(v));
}

static bool opt_bool(SeaValue opts, const char *k, bool def) {
    SeaValue v = field(opts, k);
    if (!sea_is_bool(v)) {
        if (sea_is_map(opts)) {
            SeaString *ks = sea_string_intern(k, strlen(k));
            sea_map_get(SEA_AS_MAP(opts), sea_obj_val(&ks->obj), &v);
            sea_obj_release(&ks->obj);
        }
        if (!sea_is_bool(v)) return def;
    }
    return sea_as_bool(v);
}

static int64_t opt_int(SeaValue opts, const char *k, int64_t def) {
    SeaValue v = field(opts, k);
    if (sea_is_int(v)) return sea_as_int(v);
    if (sea_is_number(v)) return (int64_t)sea_as_number(v);
    return def;
}

/* Open a connection to path with the options; an Obj or NULL with err. */
static Obj *open_db(const char *path, int flags, SeaValue opts, char *err, size_t errlen) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        snprintf(err, errlen, "%s: %s", path, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        sqlite3_close_v2(db);
        return NULL;
    }
    sqlite3_extended_result_codes(db, 1);
    sqlite3_busy_timeout(db, (int)opt_int(opts, "timeout", 5000));
    char *msg = NULL;
    const char *pragmas[4];
    int np = 0;
    pragmas[np++] = opt_bool(opts, "foreign_keys", true) ? "PRAGMA foreign_keys = ON" : "PRAGMA foreign_keys = OFF";
    if (opt_bool(opts, "wal", false)) pragmas[np++] = "PRAGMA journal_mode = WAL";
    for (int i = 0; i < np; i++) {
        if (sqlite3_exec(db, pragmas[i], NULL, NULL, &msg) != SQLITE_OK) {
            snprintf(err, errlen, "%s", msg ? msg : "pragma failed");
            sqlite3_free(msg);
            sqlite3_close_v2(db);
            return NULL;
        }
    }
    Obj *o = calloc(1, sizeof *o);
    o->kind = O_DB;
    o->db = db;
    o->path = dupn(path, strlen(path));
    o->flags = flags;
    pthread_mutex_init(&o->mu, NULL);
    return o;
}

/* native.open(path, opts) → db handle | error.  opts: read_only, create
 * (true), timeout (busy wait, ms; 5000), wal, foreign_keys (true) */
static SeaValue fn_open(SeaVM *vm, int argc, SeaValue *args) {
    char *path = argc > 0 && !sea_is_nil(args[0]) ? cstr_dup(args[0]) : dupn(":memory:", 8);
    if (!path) return err_val(vm, "sqlite.open: path must be a string");
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    int flags = SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI;
    if (opt_bool(opts, "read_only", false)) flags |= SQLITE_OPEN_READONLY;
    else flags |= SQLITE_OPEN_READWRITE | (opt_bool(opts, "create", true) ? SQLITE_OPEN_CREATE : 0);
    char err[600];
    Obj *o = open_db(path, flags, opts, err, sizeof err);
    free(path);
    if (!o) return err_val(vm, "sqlite.open: %s", err);
    int64_t h = obj_register(o, NULL);
    if (h < 0) { obj_destroy(o); return err_val(vm, "sqlite.open: too many open handles"); }
    return sea_int_val(h);
}

/* native.connect(db) → a new connection to the same database file */
static SeaValue fn_connect(SeaVM *vm, int argc, SeaValue *args) {
    Obj *d = argc > 0 ? obj_get(args[0], M(O_DB)) : NULL;
    if (!d) return err_val(vm, "sqlite.connect: the database is closed");
    if (strcmp(d->path, ":memory:") == 0) {
        obj_unref(d);
        return err_val(vm, "sqlite.connect: an in-memory database has a single connection (use a file, or file::memory:?cache=shared)");
    }
    char err[600];
    Obj *o = open_db(d->path, d->flags, SEA_NIL_VAL, err, sizeof err);
    obj_unref(d);
    if (!o) return err_val(vm, "sqlite.connect: %s", err);
    int64_t h = obj_register(o, NULL);
    if (h < 0) { obj_destroy(o); return err_val(vm, "sqlite.connect: too many open handles"); }
    return sea_int_val(h);
}

static SeaValue fn_close(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    Obj *o = argc > 0 ? obj_get(args[0], M(O_DB) | M(O_STMT)) : NULL;
    if (!o) return SEA_FALSE_VAL;
    obj_close(o);
    obj_unref(o);
    return SEA_TRUE_VAL;
}

static SeaValue fn_interrupt(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    Obj *o = argc > 0 ? obj_get(args[0], M(O_DB)) : NULL;
    if (!o) return SEA_FALSE_VAL;
    sqlite3_interrupt(o->db);
    obj_unref(o);
    return SEA_TRUE_VAL;
}

static const char *MODE_FN[] = { "query", "exec", "row", "value" };

static int parse_mode(SeaValue v) {
    static const char *names[] = { "rows", "exec", "row", "value" };
    for (int i = 0; i < 4; i++)
        if (sea_is_string(v) && strcmp(SEA_AS_STRING(v)->data, names[i]) == 0) return i;
    return -1;
}

/* native.run(h, sql, mode, params...): h is a database (with sql) or a
 * statement (sql nil). Modes: rows, exec, row, value. */
static SeaValue fn_run(SeaVM *vm, int argc, SeaValue *args) {
    SeaIoCall *io = sea_io_begin(vm, -1);
    Job *j = (Job *)(intptr_t)io->aux[0];
    int mode = argc > 2 ? parse_mode(args[2]) : -1;
    const char *fn = mode >= 0 ? MODE_FN[mode] : "query";
    if (!j) {
        if (mode < 0) { sea_io_end(vm); return err_val(vm, "sqlite: unknown query mode"); }
        Obj *o = argc > 0 ? obj_get(args[0], M(O_DB) | M(O_STMT)) : NULL;
        if (!o) { sea_io_end(vm); return err_val(vm, "sqlite.%s: the database or statement is closed", fn); }
        j = calloc(1, sizeof *j);
        atomic_init(&j->refs, 1);
        atomic_init(&j->done, 0);
        j->efd = j->wfd = -1;
        j->mode = mode;
        if (o->kind == O_STMT) {
            j->stmt = o;
            j->db = db_of(o);
            atomic_fetch_add(&j->db->refs, 1);
        } else {
            j->db = o;
            j->sql = argc > 1 ? cstr_dup(args[1]) : NULL;
            if (!j->sql) { job_unref(j); sea_io_end(vm); return err_val(vm, "sqlite.%s: SQL string expected", fn); }
        }
        char err[400];
        if (!collect_params(argc, args, 3, &j->params, &j->nparams, err, sizeof err)) {
            SeaValue r = err_val(vm, "sqlite.%s: %s", fn, err);
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
            job_unref(j);
            return err_val(vm, "sqlite.%s: interrupted while waiting", fn);
        }
    }
    io->aux[0] = 0;
    sea_io_end(vm);
    SeaValue r;
    if (j->err) r = err_val(vm, "sqlite.%s: %s", fn, j->err);
    else r = job_result(vm, j);
    job_unref(j);
    return r;
}

/* native.each(h, sql, fn, params...): fn(row) per row, stepping in place;
 * fn returning false stops. Returns the number of rows seen. */
static SeaValue fn_each(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 3 || (!sea_is_closure(args[2]) && !sea_is_function(args[2])))
        return err_val(vm, "sqlite.each: a function expected");
    Obj *o = obj_get(args[0], M(O_DB) | M(O_STMT));
    if (!o) return err_val(vm, "sqlite.each: the database or statement is closed");
    Obj *d = db_of(o);
    Param *ps = NULL;
    int np = 0;
    char err[400];
    if (!collect_params(argc, args, 3, &ps, &np, err, sizeof err)) {
        obj_unref(o);
        return err_val(vm, "sqlite.each: %s", err);
    }
    sqlite3_stmt *st = NULL;
    bool own = o->kind == O_DB;
    SeaValue result = SEA_NIL_VAL;
    pthread_mutex_lock(&d->mu);
    if (own) {
        char *sql = argc > 1 ? cstr_dup(args[1]) : NULL;
        const char *tail = NULL;
        if (!sql) snprintf(err, sizeof err, "SQL string expected");
        else if (sqlite3_prepare_v2(d->db, sql, -1, &st, &tail) != SQLITE_OK) snprintf(err, sizeof err, "%s", sqlite3_errmsg(d->db));
        else if (!st) snprintf(err, sizeof err, "no statement in the SQL");
        else if (tail && strspn(tail, " \t\r\n;") != strlen(tail)) snprintf(err, sizeof err, "each takes a single statement");
        else err[0] = '\0';
        free(sql);
        if (err[0]) result = err_val(vm, "sqlite.each: %s", err);
    } else {
        st = o->stmt;
    }
    if (sea_is_nil(result) && !bind_params(st, ps, np, err, sizeof err)) result = err_val(vm, "sqlite.each: %s", err);
    pthread_mutex_unlock(&d->mu);
    params_free(ps, np);
    int64_t count = 0;
    if (sea_is_nil(result)) {
        int ncols = sqlite3_column_count(st);
        SeaString **keys = NULL;
        int rc;
        /* SQLite serializes calls on a connection itself (FULLMUTEX), so the
         * callback may use the database while this statement is open */
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            if (!keys) {
                char **names = calloc((size_t)ncols + 1, sizeof *names);
                for (int c = 0; c < ncols; c++) {
                    const char *nm = sqlite3_column_name(st, c);
                    names[c] = dupn(nm ? nm : "", nm ? strlen(nm) : 0);
                }
                keys = make_keys(names, ncols);
                for (int c = 0; c < ncols; c++) free(names[c]);
                free(names);
            }
            SeaTable *t = sea_table_new(TABLE_MAP);
            for (int c = 0; c < ncols; c++) {
                SeaValue v = SEA_NIL_VAL;
                switch (sqlite3_column_type(st, c)) {
                    case SQLITE_INTEGER: v = int_value(sqlite3_column_int64(st, c)); break;
                    case SQLITE_FLOAT: v = sea_number_val(sqlite3_column_double(st, c)); break;
                    case SQLITE_TEXT: v = str_val((const char *)sqlite3_column_text(st, c), (size_t)sqlite3_column_bytes(st, c)); break;
                    case SQLITE_BLOB: {
                        const void *b = sqlite3_column_blob(st, c);
                        v = str_val(b ? b : "", (size_t)sqlite3_column_bytes(st, c));
                        break;
                    }
                    default: break;
                }
                sea_table_set(t, sea_obj_val(&keys[c]->obj), v);
                val_release(v);
            }
            count++;
            SeaValue row = sea_obj_val(&t->obj);
            SeaValue ret = sea_vm_call(vm, args[2], &row, 1);
            sea_obj_release(&t->obj);
            bool stop = sea_is_bool(ret) && !sea_as_bool(ret);
            val_release(ret);
            if (vm->panic_propagating) { rc = SQLITE_DONE; result = SEA_FALSE_VAL; break; }
            if (stop) { rc = SQLITE_DONE; break; }
        }
        if (rc != SQLITE_DONE) result = err_val(vm, "sqlite.each: %s", sqlite3_errmsg(d->db));
        if (keys) free_keys(keys, ncols);
        sqlite3_reset(st);
    }
    if (own && st) {
        pthread_mutex_lock(&d->mu);
        sqlite3_finalize(st);
        pthread_mutex_unlock(&d->mu);
    }
    obj_unref(o);
    if (result == SEA_FALSE_VAL) return SEA_NIL_VAL;    /* a panic is on its way */
    if (!sea_is_nil(result)) return result;
    return int_value(count);
}

/* native.prepare(db, sql) → statement handle | error */
static SeaValue fn_prepare(SeaVM *vm, int argc, SeaValue *args) {
    Obj *d = argc > 0 ? obj_get(args[0], M(O_DB)) : NULL;
    if (!d) return err_val(vm, "sqlite.prepare: the database is closed");
    char *sql = argc > 1 ? cstr_dup(args[1]) : NULL;
    if (!sql) { obj_unref(d); return err_val(vm, "sqlite.prepare: SQL string expected"); }
    sqlite3_stmt *st = NULL;
    const char *tail = NULL;
    SeaValue r = SEA_NIL_VAL;
    pthread_mutex_lock(&d->mu);
    if (sqlite3_prepare_v3(d->db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st, &tail) != SQLITE_OK)
        r = err_val(vm, "sqlite.prepare: %s", sqlite3_errmsg(d->db));
    else if (!st) r = err_val(vm, "sqlite.prepare: no statement in the SQL");
    else if (tail && strspn(tail, " \t\r\n;") != strlen(tail)) {
        sqlite3_finalize(st);
        st = NULL;
        r = err_val(vm, "sqlite.prepare: a prepared statement holds a single statement");
    }
    pthread_mutex_unlock(&d->mu);
    free(sql);
    if (sea_is_nil(r)) {
        Obj *o = calloc(1, sizeof *o);
        o->kind = O_STMT;
        o->stmt = st;
        int64_t h = obj_register(o, d);
        if (h < 0) {
            sqlite3_finalize(st);
            free(o);
            r = err_val(vm, "sqlite.prepare: too many open handles");
        } else {
            r = sea_int_val(h);
        }
    }
    obj_unref(d);
    return r;
}

/* native.info(stmt) → {params = [names], columns = [names]} */
static SeaValue fn_info(SeaVM *vm, int argc, SeaValue *args) {
    Obj *s = argc > 0 ? obj_get(args[0], M(O_STMT)) : NULL;
    if (!s) return err_val(vm, "sqlite.params: the statement is closed");
    Obj *d = db_of(s);
    SeaArray *params = sea_array_new(4), *cols = sea_array_new(4);
    pthread_mutex_lock(&d->mu);
    int n = sqlite3_bind_parameter_count(s->stmt);
    for (int i = 1; i <= n; i++) {
        const char *nm = sqlite3_bind_parameter_name(s->stmt, i);
        char num[24];
        if (!nm) { snprintf(num, sizeof num, "%d", i); nm = num; }
        else if (*nm == ':' || *nm == '@' || *nm == '$') nm++;
        sea_array_push_move(params, str_val(nm, strlen(nm)));
    }
    int nc = sqlite3_column_count(s->stmt);
    for (int c = 0; c < nc; c++) {
        const char *nm = sqlite3_column_name(s->stmt, c);
        sea_array_push_move(cols, str_val(nm ? nm : "", nm ? strlen(nm) : 0));
    }
    pthread_mutex_unlock(&d->mu);
    obj_unref(s);
    SeaTable *t = sea_table_new(TABLE_MAP);
    sea_table_set_str(t, "params", sea_obj_val(&params->obj));
    sea_table_set_str(t, "columns", sea_obj_val(&cols->obj));
    sea_obj_release(&params->obj);
    sea_obj_release(&cols->obj);
    return sea_obj_val(&t->obj);
}

/* native.last_insert_id(db), native.changes(db) */
static SeaValue fn_last_id(SeaVM *vm, int argc, SeaValue *args) {
    Obj *d = argc > 0 ? obj_get(args[0], M(O_DB)) : NULL;
    if (!d) return err_val(vm, "sqlite.last_insert_id: the database is closed");
    int64_t v = sqlite3_last_insert_rowid(d->db);
    obj_unref(d);
    return int_value(v);
}

/* native.backup(db, path) → true | error: a consistent copy of the database */
static SeaValue fn_backup(SeaVM *vm, int argc, SeaValue *args) {
    Obj *d = argc > 0 ? obj_get(args[0], M(O_DB)) : NULL;
    if (!d) return err_val(vm, "sqlite.backup: the database is closed");
    char *path = argc > 1 ? cstr_dup(args[1]) : NULL;
    if (!path) { obj_unref(d); return err_val(vm, "sqlite.backup: path expected"); }
    sqlite3 *dst = NULL;
    SeaValue r = SEA_TRUE_VAL;
    if (sqlite3_open_v2(path, &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        r = err_val(vm, "sqlite.backup: %s: %s", path, dst ? sqlite3_errmsg(dst) : "cannot open");
    } else {
        pthread_mutex_lock(&d->mu);
        sqlite3_backup *b = sqlite3_backup_init(dst, "main", d->db, "main");
        if (!b) r = err_val(vm, "sqlite.backup: %s", sqlite3_errmsg(dst));
        else {
            int rc = sqlite3_backup_step(b, -1);
            sqlite3_backup_finish(b);
            if (rc != SQLITE_DONE) r = err_val(vm, "sqlite.backup: %s", sqlite3_errstr(rc));
        }
        pthread_mutex_unlock(&d->mu);
    }
    sqlite3_close_v2(dst);
    free(path);
    obj_unref(d);
    return r;
}

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "version",        fn_version);
    sea_mod_set(mod, "open",           fn_open);
    sea_mod_set(mod, "connect",        fn_connect);
    sea_mod_set(mod, "close",          fn_close);
    sea_mod_set(mod, "interrupt",      fn_interrupt);
    sea_mod_set(mod, "run",            fn_run);
    sea_mod_set(mod, "each",           fn_each);
    sea_mod_set(mod, "prepare",        fn_prepare);
    sea_mod_set(mod, "info",           fn_info);
    sea_mod_set(mod, "last_insert_id", fn_last_id);
    sea_mod_set(mod, "backup",         fn_backup);
    return sea_obj_val(&mod->obj);
}
