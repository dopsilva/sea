/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * ndarray_native.so — the C core of libs/ndarray (N-dimensional arrays for
 * Sea, in the manner of NumPy).
 *
 * An ndarray is a Sea table:
 *
 *   data     float_array (dtype "float64") or int_array ("int64", "bool": 0/1;
 *            "float32": two 4-byte floats packed in each element), or float_array
 *            ("complex128": real and imaginary parts side by side)
 *   dtype    "float64" | "float32" | "complex128" | "int64" | "bool"
 *   shape    array of ints
 *   strides  array of ints, in elements (not bytes)
 *   offset   index in data of the first element
 *   ndim, size
 *
 * plus its methods (the table given to init(), copied into every array:
 * the native functions here and the ones ndarray.sea writes). A view shares
 * data with the array it came from: Sea's reference counting keeps the data
 * alive while any view of it lives.
 *
 * Every function takes ndarrays, Sea scalars (int, number, bool) or Sea
 * arrays (nested, converted as np.array would) where it takes an array. A
 * result with no dimensions is returned as a Sea scalar. Misuse (shapes
 * that do not match, an index out of range, a wrong type) is a panic;
 * data-dependent failures (a singular matrix, bytes that are short) are
 * error values.
 */

#include "sea.h"
#include "sea_rope.h"
#include <complex.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* float32 elements take 4 bytes (packed two to an int_array element),
 * complex128 16 (real and imaginary parts side by side in a float_array),
 * the others 8 (bool and int64 in an int_array, float64 in a float_array) */
enum { DT_BOOL = 0, DT_INT = 1, DT_F32 = 2, DT_F64 = 3, DT_C128 = 4, DT_COUNT = 5 };
#define MAXD 32
#define ESZ(dt) ((dt) == DT_F32 ? 4 : (dt) == DT_C128 ? 16 : 8)
#define IN_FLOATS(dt) ((dt) == DT_F64 || (dt) == DT_C128)   /* data in a float_array */

typedef double complex zcplx;

static const char *DT_NAME[DT_COUNT] = { "bool", "int64", "float32", "float64", "complex128" };
static SeaValue dt_str[DT_COUNT]; /* interned dtype names (shared) */
static SeaTable *proto = NULL;    /* the methods every array gets, and its fields */

/* the field names of an array (shared strings, also the keys in proto) */
enum { F_DATA, F_DTYPE, F_SHAPE, F_STRIDES, F_OFFSET, F_NDIM, F_SIZE, F_COUNT };
static const char *FIELD[F_COUNT] = { "data", "dtype", "shape", "strides", "offset", "ndim", "size" };
static SeaValue fkey[F_COUNT];

typedef struct {
    int dt, nd;
    int64_t shape[MAXD];
    int64_t strides[MAXD];
    int64_t off;          /* element offset into data */
    int64_t size;
    char *p;              /* address of the first element */
    SeaValue data;        /* borrowed */
    bool weak;            /* a Sea scalar: it does not widen an array's dtype */
} ND;

/* owned values a call holds until it returns (conversions, casts) */
typedef struct {
    SeaValue hold[8];
    int n;
    double d[4];
    int64_t i[4];
    zcplx z[4];
    int nd_, ni, nz;
} Tmp;

static void tmp_hold(Tmp *t, SeaValue v) { if (t->n < 8) t->hold[t->n++] = v; }
static void tmp_free(Tmp *t) {
    for (int i = 0; i < t->n; i++) if (sea_is_obj(t->hold[i])) sea_obj_release(sea_as_obj(t->hold[i]));
    t->n = 0;
}

/* ---- small helpers ------------------------------------------------------------- */

#define FAIL(...) sea_vm_error_noreturn(vm, __VA_ARGS__)

static bool is_num(SeaValue v) { return sea_is_int(v) || sea_is_number(v); }
static int64_t iv(SeaValue v) { return sea_is_int(v) ? sea_as_int(v) : (int64_t)sea_as_number(v); }
static double dv(SeaValue v) {
    if (sea_is_int(v)) return (double)sea_as_int(v);
    if (sea_is_bool(v)) return sea_as_bool(v) ? 1.0 : 0.0;
    return sea_as_number(v);
}

static SeaValue int_out(int64_t x) {
    if (x >= -(INT64_C(1) << 47) && x < (INT64_C(1) << 47)) return sea_int_val(x);
    return sea_number_val((double)x);
}

static SeaValue fld(SeaTable *t, const char *k) {
    SeaValue v = SEA_NIL_VAL;
    if (!sea_table_get_str(t, k, &v)) return SEA_NIL_VAL;
    return v;
}

static bool str_eq(SeaValue v, const char *s) {
    SeaString *f = sea_value_get_flat_string(v);
    if (!f) return false;
    bool eq = f->len == strlen(s) && memcmp(f->data, s, f->len) == 0;
    sea_obj_release(&f->obj);
    return eq;
}

static int dt_parse(SeaVM *vm, SeaValue v, const char *fn) {
    if (sea_is_nil(v)) return -1;
    static const char *names[][5] = {
        { "bool", "bool_", "b1", "?", NULL },
        { "int64", "int", "i8", "int_", NULL },
        { "float32", "f4", "single", NULL, NULL },
        { "float64", "float", "f8", "double", NULL },
        { "complex128", "complex", "c16", "complex_", NULL },
    };
    for (int d = 0; d < DT_COUNT; d++)
        for (int k = 0; names[d][k]; k++)
            if (str_eq(v, names[d][k])) return d;
    if (sea_is_string(v) || sea_is_rope(v))
        FAIL("ndarray.%s: unknown dtype (float64, float32, int64, bool or complex128)", fn);
    FAIL("ndarray.%s: dtype expected as a string, got %s", fn, sea_type_name(v));
}

static inline bool is_flt(int dt) { return dt == DT_F32 || dt == DT_F64; }

/* bool < int64 < float32 < float64 < complex128, but int64 with float32 is float64 */
static inline int promote(int a, int b) {
    if ((a == DT_INT && b == DT_F32) || (a == DT_F32 && b == DT_INT)) return DT_F64;
    return a > b ? a : b;
}

/* an element as a double / an int64, and stored from one */
static inline double load_d(int dt, const char *p) {
    return IN_FLOATS(dt) ? *(const double *)p : dt == DT_F32 ? (double)*(const float *)p : (double)*(const int64_t *)p;
}
static inline zcplx load_z(int dt, const char *p) {
    return dt == DT_C128 ? *(const zcplx *)p : (zcplx)load_d(dt, p);
}
static inline int64_t f2i(double x) {
    return (x >= -9.2233720368547758e18 && x < 9.2233720368547758e18) ? (int64_t)x : INT64_MIN;
}
static inline void store_d(int dt, char *p, double x) {
    if (dt == DT_F64) *(double *)p = x;
    else if (dt == DT_C128) *(zcplx *)p = x;
    else if (dt == DT_F32) *(float *)p = (float)x;
    else if (dt == DT_BOOL) *(int64_t *)p = x != 0;
    else *(int64_t *)p = f2i(x);
}
static inline void store_i(int dt, char *p, int64_t x) {
    if (dt == DT_F64) *(double *)p = (double)x;
    else if (dt == DT_C128) *(zcplx *)p = (double)x;
    else if (dt == DT_F32) *(float *)p = (float)x;
    else *(int64_t *)p = dt == DT_BOOL ? x != 0 : x;
}

static int64_t prod(const int64_t *s, int n) {
    int64_t p = 1;
    for (int i = 0; i < n; i++) p *= s[i];
    return p;
}

static void contig_strides(int nd, const int64_t *shape, int64_t *st) {
    int64_t s = 1;
    for (int i = nd - 1; i >= 0; i--) { st[i] = s; s *= shape[i] > 0 ? shape[i] : 1; }
}

static bool is_contig(const ND *a) {
    int64_t s = 1;
    for (int i = a->nd - 1; i >= 0; i--) {
        if (a->shape[i] != 1 && a->strides[i] != s) return false;
        s *= a->shape[i];
    }
    return true;
}

static char *shape_str(const int64_t *s, int n, char *buf, size_t cap) {
    size_t k = 0;
    k += snprintf(buf + k, cap - k, "(");
    for (int i = 0; i < n && k < cap; i++)
        k += snprintf(buf + k, cap - k, i ? ", %lld" : "%lld", (long long)s[i]);
    if (n == 1 && k < cap) k += snprintf(buf + k, cap - k, ",");
    if (k < cap) snprintf(buf + k, cap - k, ")");
    return buf;
}

static int norm_axis(SeaVM *vm, int64_t ax, int nd, const char *fn) {
    if (ax < 0) ax += nd;
    if (ax < 0 || ax >= nd) FAIL("ndarray.%s: axis %lld is out of bounds for an array of %d dimensions", fn,
                                 (long long)(ax < 0 ? ax - nd : ax), nd);
    return (int)ax;
}

/* a shape argument: an int, an array of ints, or nil (no dimensions) */
static int shape_arg(SeaVM *vm, SeaValue v, int64_t *shape, const char *fn) {
    if (sea_is_nil(v)) return 0;
    if (is_num(v)) {
        shape[0] = iv(v);
        if (shape[0] < 0) FAIL("ndarray.%s: negative dimensions are not allowed", fn);
        return 1;
    }
    if (sea_is_array(v) || sea_is_int_array(v)) {
        int n = sea_is_array(v) ? (int)SEA_AS_ARRAY(v)->len : (int)SEA_AS_INT_ARRAY(v)->len;
        if (n > MAXD) FAIL("ndarray.%s: at most %d dimensions", fn, MAXD);
        for (int i = 0; i < n; i++) {
            if (sea_is_array(v)) {
                SeaValue e = SEA_AS_ARRAY(v)->data[i];
                if (!is_num(e)) FAIL("ndarray.%s: a shape is ints, got %s", fn, sea_type_name(e));
                shape[i] = iv(e);
            } else {
                shape[i] = SEA_AS_INT_ARRAY(v)->data[i];
            }
            if (shape[i] < 0) FAIL("ndarray.%s: negative dimensions are not allowed", fn);
        }
        return n;
    }
    FAIL("ndarray.%s: a shape is an int or an array of ints, got %s", fn, sea_type_name(v));
}

/* ---- arrays: reading and making ------------------------------------------------- */

static SeaValue ffld(SeaTable *t, int f) {
    SeaValue v = SEA_NIL_VAL;
    if (!sea_table_get(t, fkey[f], &v)) return SEA_NIL_VAL;
    return v;
}

static bool is_nd(SeaValue v) {
    if (!sea_is_table(v)) return false;
    SeaValue d = ffld(SEA_AS_TABLE(v), F_DATA);
    return sea_is_float_array(d) || sea_is_int_array(d);
}

static void nd_read(SeaVM *vm, SeaValue v, ND *a, const char *fn) {
    SeaTable *t = SEA_AS_TABLE(v);
    SeaValue d = ffld(t, F_DATA), sh = ffld(t, F_SHAPE), st = ffld(t, F_STRIDES), off = ffld(t, F_OFFSET);
    SeaValue dt = ffld(t, F_DTYPE);
    a->dt = -1;
    a->weak = false;
    for (int k = 0; k < DT_COUNT; k++) if (dt == dt_str[k]) a->dt = k;
    for (int k = 0; k < DT_COUNT && a->dt < 0; k++) if (str_eq(dt, DT_NAME[k])) a->dt = k;
    if (a->dt < 0 || !sea_is_array(sh) || !sea_is_array(st) || !is_num(off))
        FAIL("ndarray.%s: not an ndarray (a table without its data, dtype, shape, strides and offset)", fn);
    if (IN_FLOATS(a->dt) != sea_is_float_array(d))
        FAIL("ndarray.%s: the data of a %s array must be a%s", fn, DT_NAME[a->dt],
             IN_FLOATS(a->dt) ? " float_array" : "n int_array");
    SeaArray *sa = SEA_AS_ARRAY(sh), *ta = SEA_AS_ARRAY(st);
    if (sa->len > MAXD || sa->len != ta->len) FAIL("ndarray.%s: bad shape or strides", fn);
    a->nd = (int)sa->len;
    a->off = iv(off);
    a->data = d;
    int64_t lo = 0, hi = 0;
    a->size = 1;
    for (int i = 0; i < a->nd; i++) {
        a->shape[i] = iv(sa->data[i]);
        a->strides[i] = iv(ta->data[i]);
        if (a->shape[i] < 0) FAIL("ndarray.%s: negative dimension in shape", fn);
        a->size *= a->shape[i];
        if (a->shape[i] > 0) {
            int64_t e = a->strides[i] * (a->shape[i] - 1);
            if (e < 0) lo += e; else hi += e;
        }
    }
    int64_t len = IN_FLOATS(a->dt) ? SEA_AS_FLOAT_ARRAY(d)->len : SEA_AS_INT_ARRAY(d)->len;
    if (a->dt == DT_F32) len *= 2;
    if (a->dt == DT_C128) len /= 2;
    if (a->size > 0 && (a->off + lo < 0 || a->off + hi >= len))
        FAIL("ndarray.%s: the shape and strides reach outside the data (%lld elements)", fn, (long long)len);
    char *base = IN_FLOATS(a->dt) ? (char *)SEA_AS_FLOAT_ARRAY(d)->data : (char *)SEA_AS_INT_ARRAY(d)->data;
    a->p = base + a->off * ESZ(a->dt);
}

/* fresh data of n elements (owned), zeroed unless the caller writes them all */
static SeaValue alloc_data(SeaVM *vm, int dt, int64_t n, bool zero) {
    if (n >= (int64_t)UINT32_MAX) FAIL("ndarray: an array of %lld elements is too large", (long long)n);
    if (dt == DT_F32) n = (n + 1) / 2;
    if (dt == DT_C128) n *= 2;
    if (n >= (int64_t)UINT32_MAX) FAIL("ndarray: an array of %lld elements is too large", (long long)n);
    uint32_t cap = n > 0 ? (uint32_t)n : 1;
    if (IN_FLOATS(dt)) {
        SeaFloatArray *a = sea_float_array_new(cap);
        if (zero || n == 0) memset(a->data, 0, (size_t)a->cap * 8);
        a->len = (uint32_t)n;
        return sea_obj_val(&a->obj);
    }
    SeaIntArray *a = sea_int_array_new(cap);
    if (zero || n == 0) memset(a->data, 0, (size_t)a->cap * 8);
    a->len = (uint32_t)n;
    return sea_obj_val(&a->obj);
}

static char *data_ptr(SeaValue d) {
    return sea_is_float_array(d) ? (char *)SEA_AS_FLOAT_ARRAY(d)->data : (char *)SEA_AS_INT_ARRAY(d)->data;
}

static SeaValue int_array_of(const int64_t *x, int n) {
    SeaArray *a = sea_array_new((uint32_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) sea_array_push(a, int_out(x[i]));
    return sea_obj_val(&a->obj);
}

/* a new table with proto's entries: its hash layout copied as is, each key
 * and value retained (proto holds the field keys too, so setting them does
 * not grow the table) */
static SeaTable *clone_proto(void) {
    SeaTable *t = sea_table_new(TABLE_MAP);
    if (!proto) return t;
    if (proto->count + 1 > proto->cap * 3 / 4) { sea_table_merge(t, proto); return t; }
    SEA_FREE_N(t->entries, SeaTableEntry, t->cap);
    t->entries = SEA_ALLOC_N(SeaTableEntry, proto->cap);
    memcpy(t->entries, proto->entries, sizeof(SeaTableEntry) * proto->cap);
    t->cap = proto->cap;
    t->count = proto->count;
    const SeaValue tomb = SEA_NAN_BASE | SEA_TAG_NIL | UINT64_C(1);
    for (uint32_t i = 0; i < t->cap; i++) {
        SeaTableEntry *e = &t->entries[i];
        if (e->key == SEA_NIL_VAL || e->key == tomb) continue;
        if (sea_is_obj(e->key)) sea_obj_retain(sea_as_obj(e->key));
        if (sea_is_obj(e->value)) sea_obj_retain(sea_as_obj(e->value));
    }
    return t;
}

static void set_owned(SeaTable *t, int f, SeaValue v) {
    sea_table_set(t, fkey[f], v);
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

/* a new ndarray table over data (borrowed): owned */
static SeaValue wrap(SeaValue data, int dt, int nd, const int64_t *shape, const int64_t *strides, int64_t off) {
    SeaTable *t = clone_proto();
    sea_table_set(t, fkey[F_DATA], data);
    sea_table_set(t, fkey[F_DTYPE], dt_str[dt]);
    set_owned(t, F_SHAPE, int_array_of(shape, nd));
    set_owned(t, F_STRIDES, int_array_of(strides, nd));
    sea_table_set(t, fkey[F_OFFSET], sea_int_val(off));
    sea_table_set(t, fkey[F_NDIM], sea_int_val(nd));
    sea_table_set(t, fkey[F_SIZE], int_out(prod(shape, nd)));
    return sea_obj_val(&t->obj);
}

/* a new contiguous array → table (owned); o describes it. nd_new zeroes
 * it, nd_raw leaves it for the caller to write every element */
static SeaValue nd_alloc(SeaVM *vm, int dt, int nd, const int64_t *shape, ND *o, bool zero) {
    int64_t n = prod(shape, nd);
    SeaValue d = alloc_data(vm, dt, n, zero);
    o->dt = dt; o->nd = nd; o->size = n; o->off = 0; o->data = d; o->weak = false;
    if (nd > 0) memcpy(o->shape, shape, sizeof(int64_t) * nd);
    contig_strides(nd, shape, o->strides);
    o->p = data_ptr(d);
    SeaValue t = wrap(d, dt, nd, o->shape, o->strides, 0);
    sea_obj_release(sea_as_obj(d));
    return t;
}
static SeaValue nd_new(SeaVM *vm, int dt, int nd, const int64_t *shape, ND *o) {
    return nd_alloc(vm, dt, nd, shape, o, true);
}
static SeaValue nd_raw(SeaVM *vm, int dt, int nd, const int64_t *shape, ND *o) {
    return nd_alloc(vm, dt, nd, shape, o, false);
}

/* the same data seen with another geometry (owned) */
static SeaValue view(const ND *a, int nd, const int64_t *shape, const int64_t *strides, int64_t off) {
    return wrap(a->data, a->dt, nd, shape, strides, off);
}

/* a complex scalar in Sea: a table {re, im} */
static SeaValue cpx_val(zcplx z) {
    SeaTable *t = sea_table_new(TABLE_MAP);
    sea_table_set_str(t, "re", sea_number_val(creal(z)));
    sea_table_set_str(t, "im", sea_number_val(cimag(z)));
    return sea_obj_val(&t->obj);
}

static bool is_opts(SeaValue v);
static SeaValue opt(SeaValue v, const char *name);

/* whether v is a complex scalar ({re, im}, a table or a struct) → *z */
static bool cpx_of(SeaValue v, zcplx *z) {
    if (!is_opts(v)) return false;
    SeaValue re = opt(v, "re"), im = opt(v, "im");
    if (!is_num(re) || !(is_num(im) || sea_is_nil(im))) return false;
    *z = CMPLX(dv(re), sea_is_nil(im) ? 0.0 : dv(im));
    return true;
}

static SeaValue scalar_at(int dt, const char *p) {
    if (dt == DT_F64) return sea_number_val(*(const double *)p);
    if (dt == DT_C128) return cpx_val(*(const zcplx *)p);
    if (dt == DT_F32) return sea_number_val((double)*(const float *)p);
    int64_t x = *(const int64_t *)p;
    return dt == DT_BOOL ? sea_bool_val(x != 0) : int_out(x);
}

/* a result: a scalar when it has no dimensions */
static SeaValue finish(SeaValue t, const ND *o) {
    if (o->nd != 0) return t;
    SeaValue s = scalar_at(o->dt, o->p);
    sea_obj_release(sea_as_obj(t));
    return s;
}

/* ---- iteration with broadcasting ------------------------------------------------- */

typedef void (*Kern)(int64_t n, char **p, const int64_t *s, void *ctx);

typedef struct {
    int nd, k;
    int64_t shape[MAXD];
    int64_t st[4][MAXD];   /* in elements */
    char *p[4];
    int esz[4];            /* each operand's element size */
} It;

static bool bshape(ND **ops, int n, int *nd, int64_t *shape) {
    int m = 0;
    for (int j = 0; j < n; j++) if (ops[j]->nd > m) m = ops[j]->nd;
    for (int d = 0; d < m; d++) shape[d] = 1;
    for (int j = 0; j < n; j++) {
        for (int d = 0; d < ops[j]->nd; d++) {
            int od = m - ops[j]->nd + d;
            int64_t s = ops[j]->shape[d];
            if (s == shape[od] || s == 1) continue;
            if (shape[od] == 1) { shape[od] = s; continue; }
            return false;
        }
    }
    *nd = m;
    return true;
}

static void bfail(SeaVM *vm, const char *fn, ND **ops, int n) {
    char msg[512], b[160];
    size_t k = 0;
    for (int j = 0; j < n; j++)
        k += snprintf(msg + k, sizeof msg - k, " %s", shape_str(ops[j]->shape, ops[j]->nd, b, sizeof b));
    FAIL("ndarray.%s: operands could not be broadcast together with shapes%s", fn, msg);
}

/* iterate the shape over k operands, each broadcast to it (strides 0 where
 * it has size 1 or no such dimension); size-1 dimensions dropped and
 * neighbours that are contiguous for every operand merged */
static void it_init(It *it, ND **ops, int k, int nd, const int64_t *shape) {
    int64_t st[4][MAXD];
    for (int j = 0; j < k; j++) {
        for (int d = 0; d < nd; d++) {
            int od = d - (nd - ops[j]->nd);
            if (od < 0 || (ops[j]->shape[od] == 1 && shape[d] != 1)) st[j][d] = 0;
            else st[j][d] = ops[j]->strides[od];
        }
        it->p[j] = ops[j]->p;
        it->esz[j] = ESZ(ops[j]->dt);
    }
    int m = 0;
    for (int d = 0; d < nd; d++) {
        if (shape[d] == 1) continue;
        bool merge = m > 0;
        for (int j = 0; j < k && merge; j++)
            if (it->st[j][m - 1] != st[j][d] * shape[d]) merge = false;
        if (merge) {
            it->shape[m - 1] *= shape[d];
            for (int j = 0; j < k; j++) it->st[j][m - 1] = st[j][d];
        } else {
            it->shape[m] = shape[d];
            for (int j = 0; j < k; j++) it->st[j][m] = st[j][d];
            m++;
        }
    }
    it->nd = m;
    it->k = k;
}

static void it_run(It *it, Kern f, void *ctx) {
    int m = it->nd, k = it->k;
    for (int d = 0; d < m; d++) if (it->shape[d] == 0) return;
    int64_t s[4] = { 0, 0, 0, 0 };
    char *p[4];
    for (int j = 0; j < k; j++) p[j] = it->p[j];
    if (m == 0) { f(1, p, s, ctx); return; }
    for (int j = 0; j < k; j++) s[j] = it->st[j][m - 1];
    int64_t inner = it->shape[m - 1];
    int64_t idx[MAXD] = { 0 };
    for (;;) {
        f(inner, p, s, ctx);
        int d = m - 2;
        for (; d >= 0; d--) {
            for (int j = 0; j < k; j++) p[j] += it->st[j][d] * it->esz[j];
            if (++idx[d] < it->shape[d]) break;
            for (int j = 0; j < k; j++) p[j] -= it->st[j][d] * it->shape[d] * it->esz[j];
            idx[d] = 0;
        }
        if (d < 0) return;
    }
}

/* run f over ops (ops[0] the output) broadcast to shape */
static void run(ND **ops, int k, int nd, const int64_t *shape, Kern f, void *ctx) {
    It it;
    it_init(&it, ops, k, nd, shape);
    it_run(&it, f, ctx);
}

/* ---- casts and copies ------------------------------------------------------------ */

typedef struct { int from, to; } Cast;

#define CAST_LOOP(TO, TA, EXPR)                                                       \
    do {                                                                              \
        TO *o = (TO *)p[0]; const TA *a = (const TA *)p[1];                           \
        for (int64_t i = 0; i < n; i++) { TA x = a[i * sa]; o[i * so] = (TO)(EXPR); } \
    } while (0)

static void k_cast(int64_t n, char **p, const int64_t *s, void *ctx) {
    const Cast *c = ctx;
    int64_t so = s[0], sa = s[1];
    int to = c->to, from = c->from;
    if (from == DT_C128) {
        /* to a real dtype: the real part (NumPy's ComplexWarning, silently) */
        if (to == DT_C128) CAST_LOOP(zcplx, zcplx, x);
        else if (to == DT_F64) CAST_LOOP(double, zcplx, creal(x));
        else if (to == DT_F32) CAST_LOOP(float, zcplx, creal(x));
        else if (to == DT_BOOL) CAST_LOOP(int64_t, zcplx, x != 0);
        else CAST_LOOP(int64_t, zcplx, f2i(creal(x)));
    } else if (to == DT_C128) {
        if (from == DT_F64) CAST_LOOP(zcplx, double, x);
        else if (from == DT_F32) CAST_LOOP(zcplx, float, x);
        else CAST_LOOP(zcplx, int64_t, (double)x);
    } else if (from == DT_F64) {
        if (to == DT_F64) CAST_LOOP(double, double, x);
        else if (to == DT_F32) CAST_LOOP(float, double, x);
        else if (to == DT_BOOL) CAST_LOOP(int64_t, double, x != 0);
        else CAST_LOOP(int64_t, double, f2i(x));
    } else if (from == DT_F32) {
        if (to == DT_F64) CAST_LOOP(double, float, x);
        else if (to == DT_F32) CAST_LOOP(float, float, x);
        else if (to == DT_BOOL) CAST_LOOP(int64_t, float, x != 0);
        else CAST_LOOP(int64_t, float, f2i(x));
    } else {
        if (to == DT_F64) CAST_LOOP(double, int64_t, x);
        else if (to == DT_F32) CAST_LOOP(float, int64_t, x);
        else if (to == DT_BOOL && from != DT_BOOL) CAST_LOOP(int64_t, int64_t, x != 0);
        else CAST_LOOP(int64_t, int64_t, x);
    }
}

/* dst[...] = src, src broadcast to dst's shape, cast to dst's dtype */
static void assign(SeaVM *vm, ND *dst, ND *src, const char *fn) {
    ND *ops[2] = { dst, src };
    int nd; int64_t shape[MAXD];
    if (!bshape(ops, 2, &nd, shape) || nd != dst->nd || memcmp(shape, dst->shape, sizeof(int64_t) * nd) != 0) {
        char a[160], b[160];
        FAIL("ndarray.%s: could not broadcast an array of shape %s into shape %s", fn,
             shape_str(src->shape, src->nd, a, sizeof a), shape_str(dst->shape, dst->nd, b, sizeof b));
    }
    Cast c = { src->dt, dst->dt };
    run(ops, 2, dst->nd, dst->shape, k_cast, &c);
}

/* a contiguous copy of a with dtype dt (owned table) */
static SeaValue copy_as(SeaVM *vm, ND *a, int dt, ND *o) {
    SeaValue t = nd_raw(vm, dt, a->nd, a->shape, o);
    assign(vm, o, a, "copy");
    return t;
}

/* ---- operands --------------------------------------------------------------------- */

static SeaValue from_value(SeaVM *vm, SeaValue v, int dt, const char *fn);

/* any array-like argument as an ND (scalars as 0-d, Sea arrays converted) */
static void opnd(SeaVM *vm, SeaValue v, ND *a, Tmp *t, const char *fn) {
    memset(a, 0, sizeof *a);
    a->size = 1;
    if (sea_is_int(v) || sea_is_bool(v)) {
        int64_t *slot = &t->i[t->ni++ & 3];
        *slot = sea_is_int(v) ? sea_as_int(v) : sea_as_bool(v);
        a->dt = sea_is_int(v) ? DT_INT : DT_BOOL;
        a->p = (char *)slot;
        a->weak = true;
        return;
    }
    if (sea_is_number(v)) {
        double *slot = &t->d[t->nd_++ & 3];
        *slot = sea_as_number(v);
        a->dt = DT_F64;
        a->p = (char *)slot;
        a->weak = true;
        return;
    }
    zcplx z;
    if (cpx_of(v, &z)) {
        zcplx *slot = &t->z[t->nz++ & 3];
        *slot = z;
        a->dt = DT_C128;
        a->p = (char *)slot;
        a->weak = true;
        return;
    }
    if (is_nd(v)) { nd_read(vm, v, a, fn); return; }
    if (sea_is_array(v) || sea_is_float_array(v) || sea_is_int_array(v)) {
        SeaValue c = from_value(vm, v, -1, fn);
        tmp_hold(t, c);
        nd_read(vm, c, a, fn);
        return;
    }
    FAIL("ndarray.%s: expected an array or a number, got %s", fn, sea_type_name(v));
}

/* a as dtype dt (a converted copy held in t when it differs) */
static void as_dt(SeaVM *vm, ND *a, int dt, Tmp *t) {
    if (a->dt == dt) return;
    /* bool → int64: the same storage (0/1) */
    if (a->dt == DT_BOOL && dt == DT_INT) { a->dt = DT_INT; return; }
    if (a->nd == 0) {
        /* (a double slot holds a float32 too) */
        char *slot = dt == DT_C128 ? (char *)&t->z[t->nz++ & 3]
                   : is_flt(dt) ? (char *)&t->d[t->nd_++ & 3] : (char *)&t->i[t->ni++ & 3];
        if (dt == DT_C128) *(zcplx *)slot = load_z(a->dt, a->p);
        else if (a->dt == DT_C128) {
            zcplx z = *(zcplx *)a->p;
            if (dt == DT_BOOL) *(int64_t *)slot = z != 0;
            else store_d(dt, slot, creal(z));
        } else if (is_flt(a->dt)) store_d(dt, slot, load_d(a->dt, a->p));
        else store_i(dt, slot, *(int64_t *)a->p);
        a->p = slot;
        a->dt = dt;
        return;
    }
    ND o;
    SeaValue c = copy_as(vm, a, dt, &o);
    tmp_hold(t, c);
    *a = o;
}

/* the dtype of an operation on a and b: a Sea scalar keeps an array's dtype
 * when it is of the same kind or narrower (NumPy's rule for Python scalars:
 * float32 array * 2.5 stays float32, int64 array * 2.5 is float64) */
static int weak_with(int arr, int sc) {
    if (sc == DT_C128) return DT_C128;
    if (sc == DT_F64) return is_flt(arr) ? arr : DT_F64;
    if (sc == DT_INT) return arr == DT_BOOL ? DT_INT : arr;
    return arr;
}
static int promote_w(const ND *a, const ND *b) {
    if (a->weak && !b->weak) return weak_with(b->dt, a->dt);
    if (b->weak && !a->weak) return weak_with(a->dt, b->dt);
    return promote(a->dt, b->dt);
}

/* ---- conversion from Sea values --------------------------------------------------- */

typedef struct { int nd; int64_t shape[MAXD]; int dt; } Probe;

static void probe(SeaVM *vm, SeaValue v, Probe *pr, int depth, const char *fn) {
    if (depth >= MAXD) FAIL("ndarray.%s: nested more than %d levels", fn, MAXD);
    if (sea_is_array(v)) {
        SeaArray *a = SEA_AS_ARRAY(v);
        if (depth == pr->nd) pr->shape[pr->nd++] = a->len;
        for (uint32_t i = 0; i < a->len; i++) probe(vm, a->data[i], pr, depth + 1, fn);
    } else if (sea_is_float_array(v) || sea_is_int_array(v)) {
        if (depth == pr->nd) pr->shape[pr->nd++] = sea_is_float_array(v) ? SEA_AS_FLOAT_ARRAY(v)->len
                                                                         : SEA_AS_INT_ARRAY(v)->len;
        pr->dt = promote(pr->dt, sea_is_float_array(v) ? DT_F64 : DT_INT);
    } else if (is_nd(v)) {
        ND a;
        nd_read(vm, v, &a, fn);
        for (int i = 0; i < a.nd && depth + i <= pr->nd; i++)
            if (depth + i == pr->nd) pr->shape[pr->nd++] = a.shape[i];
        pr->dt = promote(pr->dt, a.dt);
    } else if (sea_is_int(v)) pr->dt = promote(pr->dt, DT_INT);
    else if (sea_is_number(v)) pr->dt = promote(pr->dt, DT_F64);
    else if (sea_is_bool(v)) pr->dt = promote(pr->dt, DT_BOOL);
    else {
        zcplx z;
        if (!cpx_of(v, &z)) FAIL("ndarray.%s: an array holds numbers, bools and complex {re, im}, got %s", fn, sea_type_name(v));
        pr->dt = DT_C128;
    }
}

static void ragged(SeaVM *vm, const char *fn) {
    FAIL("ndarray.%s: the nested arrays do not form a rectangle (each level must have the same length)", fn);
}

static void fill_from(SeaVM *vm, SeaValue v, const Probe *pr, int depth, int dt, char *p, const int64_t *st,
                      const char *fn) {
    if (sea_is_array(v)) {
        SeaArray *a = SEA_AS_ARRAY(v);
        if (depth >= pr->nd || a->len != pr->shape[depth]) ragged(vm, fn);
        for (uint32_t i = 0; i < a->len; i++)
            fill_from(vm, a->data[i], pr, depth + 1, dt, p + i * st[depth] * ESZ(dt), st, fn);
        return;
    }
    if (sea_is_float_array(v) || sea_is_int_array(v)) {
        uint32_t n = sea_is_float_array(v) ? SEA_AS_FLOAT_ARRAY(v)->len : SEA_AS_INT_ARRAY(v)->len;
        if (depth != pr->nd - 1 || n != pr->shape[depth]) ragged(vm, fn);
        for (uint32_t i = 0; i < n; i++) {
            char *q = p + i * st[depth] * ESZ(dt);
            if (sea_is_float_array(v)) store_d(dt, q, SEA_AS_FLOAT_ARRAY(v)->data[i]);
            else store_i(dt, q, SEA_AS_INT_ARRAY(v)->data[i]);
        }
        return;
    }
    if (is_nd(v)) {
        ND src, dst;
        nd_read(vm, v, &src, fn);
        if (depth + src.nd != pr->nd) ragged(vm, fn);
        for (int i = 0; i < src.nd; i++) if (src.shape[i] != pr->shape[depth + i]) ragged(vm, fn);
        memset(&dst, 0, sizeof dst);
        dst.dt = dt; dst.nd = src.nd; dst.p = p;
        memcpy(dst.shape, src.shape, sizeof(int64_t) * src.nd);
        memcpy(dst.strides, st + depth, sizeof(int64_t) * src.nd);
        assign(vm, &dst, &src, fn);
        return;
    }
    if (depth != pr->nd) ragged(vm, fn);
    zcplx z;
    if (sea_is_number(v)) store_d(dt, p, sea_as_number(v));
    else if (cpx_of(v, &z)) {
        if (dt == DT_C128) *(zcplx *)p = z;
        else if (dt == DT_BOOL) *(int64_t *)p = z != 0;
        else store_d(dt, p, creal(z));
    } else store_i(dt, p, sea_is_int(v) ? sea_as_int(v) : sea_as_bool(v));
}

/* np.array(v [, dtype]) (owned table) */
static SeaValue from_value(SeaVM *vm, SeaValue v, int dt, const char *fn) {
    Probe pr;
    memset(&pr, 0, sizeof pr);
    pr.dt = DT_BOOL;
    if (sea_is_array(v) && SEA_AS_ARRAY(v)->len == 0) pr.dt = DT_F64;
    probe(vm, v, &pr, 0, fn);
    if (dt < 0) dt = pr.dt;
    ND o;
    SeaValue t = nd_new(vm, dt, pr.nd, pr.shape, &o);
    fill_from(vm, v, &pr, 0, dt, o.p, o.strides, fn);
    return t;
}

/* ---- element-wise binary operations ------------------------------------------------ */

#define K2(name, TA, TO, EXPR)                                                                       \
    static void name(int64_t n, char **p, const int64_t *s, void *ctx) {                             \
        (void)ctx;                                                                                   \
        TO *o = (TO *)p[0]; const TA *a = (const TA *)p[1], *b = (const TA *)p[2];                  \
        int64_t so = s[0], sa = s[1], sb = s[2];                                                     \
        if (so == 1 && sa == 1 && sb == 1) {                                                         \
            for (int64_t i = 0; i < n; i++) { TA x = a[i], y = b[i]; o[i] = (TO)(EXPR); }            \
        } else if (so == 1 && sa == 1 && sb == 0) {                                                  \
            TA y = b[0];                                                                             \
            for (int64_t i = 0; i < n; i++) { TA x = a[i]; o[i] = (TO)(EXPR); }                      \
        } else if (so == 1 && sa == 0 && sb == 1) {                                                  \
            TA x = a[0];                                                                             \
            for (int64_t i = 0; i < n; i++) { TA y = b[i]; o[i] = (TO)(EXPR); }                      \
        } else {                                                                                     \
            for (int64_t i = 0; i < n; i++) { TA x = a[i * sa], y = b[i * sb]; o[i * so] = (TO)(EXPR); } \
        }                                                                                            \
    }

static inline double py_fmod(double x, double y) {
    double r = fmod(x, y);
    if (r != 0 && ((r < 0) != (y < 0))) r += y;
    return r;
}
static inline double py_fdiv(double x, double y) {
    if (y == 0) return x / y;
    double m = py_fmod(x, y);
    double q = (x - m) / y;
    double f = floor(q);
    if (q - f > 0.5) f += 1;
    return f;
}
static inline int64_t i_add(int64_t x, int64_t y) { return (int64_t)((uint64_t)x + (uint64_t)y); }
static inline int64_t i_sub(int64_t x, int64_t y) { return (int64_t)((uint64_t)x - (uint64_t)y); }
static inline int64_t i_mul(int64_t x, int64_t y) { return (int64_t)((uint64_t)x * (uint64_t)y); }
static inline int64_t i_fdiv(int64_t x, int64_t y) {
    if (y == 0) return 0;
    if (y == -1) return (int64_t)(0 - (uint64_t)x);
    int64_t q = x / y;
    if ((x % y != 0) && ((x < 0) != (y < 0))) q--;
    return q;
}
static inline int64_t i_mod(int64_t x, int64_t y) {
    if (y == 0 || y == -1) return 0;
    int64_t r = x % y;
    if (r != 0 && ((r < 0) != (y < 0))) r += y;
    return r;
}
static inline int64_t i_pow(int64_t x, int64_t y) {
    if (y < 0) return x == 1 ? 1 : x == -1 ? ((y & 1) ? -1 : 1) : 0;
    uint64_t r = 1, b = (uint64_t)x;
    while (y) { if (y & 1) r *= b; b *= b; y >>= 1; }
    return (int64_t)r;
}
static inline double f_max(double x, double y) { return (x != x || x > y) ? x : (y != y ? y : (x == y ? x : y)); }
static inline double f_min(double x, double y) { return (x != x || x < y) ? x : (y != y ? y : (x == y ? x : y)); }

/* each float kernel twice: f_ on float64, g_ on float32 */
#define KF2(n, EXPR) K2(f_##n, double, double, EXPR) K2(g_##n, float, float, EXPR)
#define KC2(n, EXPR) K2(f_##n, double, int64_t, EXPR) K2(g_##n, float, int64_t, EXPR)

KF2(add, x + y)
KF2(sub, x - y)
KF2(mul, x * y)
KF2(div, x / y)
KF2(fdiv, py_fdiv(x, y))
KF2(mod, py_fmod(x, y))
KF2(pow, pow(x, y))
KF2(maximum, f_max(x, y))
KF2(minimum, f_min(x, y))
KF2(fmax, fmax(x, y))
KF2(fmin, fmin(x, y))
KF2(atan2, atan2(x, y))
KF2(hypot, hypot(x, y))
KF2(copysign, copysign(x, y))
KC2(eq, x == y)
KC2(ne, x != y)
KC2(lt, x < y)
KC2(le, x <= y)
KC2(gt, x > y)
KC2(ge, x >= y)
KC2(land, (x != 0) && (y != 0))
KC2(lor, (x != 0) || (y != 0))
KC2(lxor, (x != 0) != (y != 0))

K2(i_add_k, int64_t, int64_t, i_add(x, y))
K2(i_sub_k, int64_t, int64_t, i_sub(x, y))
K2(i_mul_k, int64_t, int64_t, i_mul(x, y))
K2(i_fdiv_k, int64_t, int64_t, i_fdiv(x, y))
K2(i_mod_k, int64_t, int64_t, i_mod(x, y))
K2(i_pow_k, int64_t, int64_t, i_pow(x, y))
K2(i_maximum, int64_t, int64_t, x > y ? x : y)
K2(i_minimum, int64_t, int64_t, x < y ? x : y)
K2(i_eq, int64_t, int64_t, x == y)
K2(i_ne, int64_t, int64_t, x != y)
K2(i_lt, int64_t, int64_t, x < y)
K2(i_le, int64_t, int64_t, x <= y)
K2(i_gt, int64_t, int64_t, x > y)
K2(i_ge, int64_t, int64_t, x >= y)
K2(i_land, int64_t, int64_t, (x != 0) && (y != 0))
K2(i_lor, int64_t, int64_t, (x != 0) || (y != 0))
K2(i_lxor, int64_t, int64_t, (x != 0) != (y != 0))
K2(i_band, int64_t, int64_t, x & y)
K2(i_bor, int64_t, int64_t, x | y)
K2(i_bxor, int64_t, int64_t, x ^ y)
K2(i_shl, int64_t, int64_t, (y < 0 || y > 63) ? 0 : (int64_t)((uint64_t)x << y))
K2(i_shr, int64_t, int64_t, (y < 0 || y > 63) ? (x < 0 ? -1 : 0) : x >> y)

/* complex128 kernels (none: not defined for complex numbers) */
K2(c_add, zcplx, zcplx, x + y)
K2(c_sub, zcplx, zcplx, x - y)
K2(c_mul, zcplx, zcplx, x * y)
K2(c_div, zcplx, zcplx, x / y)
K2(c_pow, zcplx, zcplx, (y == 0 ? (zcplx)1 : cpow(x, y)))
K2(c_eq, zcplx, int64_t, x == y)
K2(c_ne, zcplx, int64_t, x != y)
K2(c_land, zcplx, int64_t, (x != 0) && (y != 0))
K2(c_lor, zcplx, int64_t, (x != 0) || (y != 0))
K2(c_lxor, zcplx, int64_t, (x != 0) != (y != 0))

/* kind: what dtype the result has */
enum { B_ARITH, B_FLOAT, B_CMP, B_LOGIC, B_INT };

/* f: float64 kernel, g: float32, i: int64 (and bool), c: complex128 */
typedef struct { const char *name; Kern f, g, i; int kind; Kern c; } Bin;

#define FG(n) f_##n, g_##n
static const Bin BIN[] = {
    { "add", FG(add), i_add_k, B_ARITH, c_add },   { "subtract", FG(sub), i_sub_k, B_ARITH, c_sub },
    { "multiply", FG(mul), i_mul_k, B_ARITH, c_mul }, { "divide", FG(div), NULL, B_FLOAT, c_div },
    { "floor_divide", FG(fdiv), i_fdiv_k, B_ARITH }, { "mod", FG(mod), i_mod_k, B_ARITH },
    { "power", FG(pow), i_pow_k, B_ARITH, c_pow }, { "maximum", FG(maximum), i_maximum, B_ARITH },
    { "minimum", FG(minimum), i_minimum, B_ARITH }, { "fmax", FG(fmax), i_maximum, B_ARITH },
    { "fmin", FG(fmin), i_minimum, B_ARITH },      { "arctan2", FG(atan2), NULL, B_FLOAT },
    { "hypot", FG(hypot), NULL, B_FLOAT },         { "copysign", FG(copysign), NULL, B_FLOAT },
    { "equal", FG(eq), i_eq, B_CMP, c_eq },        { "not_equal", FG(ne), i_ne, B_CMP, c_ne },
    { "less", FG(lt), i_lt, B_CMP },               { "less_equal", FG(le), i_le, B_CMP },
    { "greater", FG(gt), i_gt, B_CMP },            { "greater_equal", FG(ge), i_ge, B_CMP },
    { "logical_and", FG(land), i_land, B_LOGIC, c_land }, { "logical_or", FG(lor), i_lor, B_LOGIC, c_lor },
    { "logical_xor", FG(lxor), i_lxor, B_LOGIC, c_lxor }, { "bitwise_and", NULL, NULL, i_band, B_INT },
    { "bitwise_or", NULL, NULL, i_bor, B_INT },    { "bitwise_xor", NULL, NULL, i_bxor, B_INT },
    { "left_shift", NULL, NULL, i_shl, B_INT },    { "right_shift", NULL, NULL, i_shr, B_INT },
};
#define NBIN ((int)(sizeof BIN / sizeof BIN[0]))

/* an optional `out` argument: the array the result goes into */
static bool out_arg(SeaVM *vm, int argc, SeaValue *args, int i, ND *o, const char *fn) {
    if (argc <= i || sea_is_nil(args[i])) return false;
    if (!is_nd(args[i])) FAIL("ndarray.%s: out must be an ndarray, got %s", fn, sea_type_name(args[i]));
    nd_read(vm, args[i], o, fn);
    return true;
}

/* the result of an operation into `out` when given (cast to its dtype) or a
 * new array: *r is the array to compute into (a temporary when out's dtype
 * differs) */
static SeaValue result_for(SeaVM *vm, int argc, SeaValue *args, int oi, int dt, int nd, const int64_t *shape,
                           ND *r, ND *out, bool *has_out, const char *fn) {
    *has_out = out_arg(vm, argc, args, oi, out, fn);
    if (*has_out) {
        if (out->nd != nd || memcmp(out->shape, shape, sizeof(int64_t) * nd) != 0) {
            char a[160], b[160];
            FAIL("ndarray.%s: out has shape %s, the result has %s", fn, shape_str(out->shape, out->nd, a, sizeof a),
                 shape_str(shape, nd, b, sizeof b));
        }
        if (out->dt == dt) { *r = *out; return SEA_NIL_VAL; }
    }
    return nd_raw(vm, dt, nd, shape, r);
}

static SeaValue result_done(SeaVM *vm, int argc, SeaValue *args, int oi, SeaValue t, ND *r, ND *out, bool has_out,
                            const char *fn) {
    if (has_out) {
        if (!sea_is_nil(t)) { assign(vm, out, r, fn); sea_obj_release(sea_as_obj(t)); }
        return sea_val_ref(args[oi]);
    }
    return finish(t, r);
}

static SeaValue binary(SeaVM *vm, int argc, SeaValue *args, const Bin *op) {
    if (argc < 2) FAIL("ndarray.%s: expected 2 arrays, got %d arguments", op->name, argc);
    Tmp t = { 0 };
    ND a, b;
    opnd(vm, args[0], &a, &t, op->name);
    opnd(vm, args[1], &b, &t, op->name);
    int c = promote_w(&a, &b);
    if (c == DT_BOOL) c = DT_INT;
    if (op->kind == B_FLOAT && c < DT_F32) c = DT_F64;
    if (op->kind == B_INT && c >= DT_F32) FAIL("ndarray.%s: only for int and bool arrays", op->name);
    if (c == DT_C128 && !op->c) FAIL("ndarray.%s: not defined for complex numbers", op->name);
    int odt = c;
    if (op->kind == B_CMP || op->kind == B_LOGIC) odt = DT_BOOL;
    if (op->kind == B_INT && a.dt == DT_BOOL && b.dt == DT_BOOL) odt = DT_BOOL;
    ND *ops0[2] = { &a, &b };
    int nd; int64_t shape[MAXD];
    if (!bshape(ops0, 2, &nd, shape)) bfail(vm, op->name, ops0, 2);
    as_dt(vm, &a, c, &t);
    as_dt(vm, &b, c, &t);
    ND r, out; bool has_out;
    SeaValue res = result_for(vm, argc, args, 2, odt, nd, shape, &r, &out, &has_out, op->name);
    ND *ops[3] = { &r, &a, &b };
    run(ops, 3, nd, shape, c == DT_C128 ? op->c : c == DT_F64 ? op->f : c == DT_F32 ? op->g : op->i, NULL);
    tmp_free(&t);
    return result_done(vm, argc, args, 2, res, &r, &out, has_out, op->name);
}

#define BIN_FN(idx, cname) static SeaValue cname(SeaVM *vm, int argc, SeaValue *args) { return binary(vm, argc, args, &BIN[idx]); }
BIN_FN(0, n_add) BIN_FN(1, n_subtract) BIN_FN(2, n_multiply) BIN_FN(3, n_divide) BIN_FN(4, n_floor_divide)
BIN_FN(5, n_mod) BIN_FN(6, n_power) BIN_FN(7, n_maximum) BIN_FN(8, n_minimum) BIN_FN(9, n_fmax)
BIN_FN(10, n_fmin) BIN_FN(11, n_arctan2) BIN_FN(12, n_hypot) BIN_FN(13, n_copysign) BIN_FN(14, n_equal)
BIN_FN(15, n_not_equal) BIN_FN(16, n_less) BIN_FN(17, n_less_equal) BIN_FN(18, n_greater)
BIN_FN(19, n_greater_equal) BIN_FN(20, n_logical_and) BIN_FN(21, n_logical_or) BIN_FN(22, n_logical_xor)
BIN_FN(23, n_bitwise_and) BIN_FN(24, n_bitwise_or) BIN_FN(25, n_bitwise_xor) BIN_FN(26, n_left_shift)
BIN_FN(27, n_right_shift)

/* ---- element-wise unary operations ------------------------------------------------- */

#define K1(name, TA, TO, EXPR)                                                                       \
    static void name(int64_t n, char **p, const int64_t *s, void *ctx) {                             \
        (void)ctx;                                                                                   \
        TO *o = (TO *)p[0]; const TA *a = (const TA *)p[1];                                          \
        int64_t so = s[0], sa = s[1];                                                                \
        if (so == 1 && sa == 1) for (int64_t i = 0; i < n; i++) { TA x = a[i]; o[i] = (TO)(EXPR); }  \
        else for (int64_t i = 0; i < n; i++) { TA x = a[i * sa]; o[i * so] = (TO)(EXPR); }           \
    }

/* each float kernel twice: u_ on float64, v_ on float32 */
#define KF1(n, EXPR) K1(u_##n, double, double, EXPR) K1(v_##n, float, float, EXPR)
#define KB1(n, EXPR) K1(u_##n, double, int64_t, EXPR) K1(v_##n, float, int64_t, EXPR)

KF1(negf, -x)
KF1(absf, fabs(x))
KF1(signf, x > 0 ? 1.0 : x < 0 ? -1.0 : x)
KF1(squaref, x * x)
KF1(sqrt, sqrt(x))
KF1(cbrt, cbrt(x))
KF1(recip, 1.0 / x)
KF1(exp, exp(x))
KF1(exp2, exp2(x))
KF1(expm1, expm1(x))
KF1(log, log(x))
KF1(log2, log2(x))
KF1(log10, log10(x))
KF1(log1p, log1p(x))
KF1(sin, sin(x))
KF1(cos, cos(x))
KF1(tan, tan(x))
KF1(asin, asin(x))
KF1(acos, acos(x))
KF1(atan, atan(x))
KF1(sinh, sinh(x))
KF1(cosh, cosh(x))
KF1(tanh, tanh(x))
KF1(asinh, asinh(x))
KF1(acosh, acosh(x))
KF1(atanh, atanh(x))
KF1(floor, floor(x))
KF1(ceil, ceil(x))
KF1(trunc, trunc(x))
KF1(rint, nearbyint(x))
KB1(isnan, x != x)
KB1(isinf, isinf(x) != 0)
KB1(isfinite, isfinite(x) != 0)
KB1(notf, x == 0)
KB1(signbit, signbit(x) != 0)
K1(u_negi, int64_t, int64_t, (int64_t)(0 - (uint64_t)x))
K1(u_absi, int64_t, int64_t, x < 0 ? (int64_t)(0 - (uint64_t)x) : x)
K1(u_signi, int64_t, int64_t, (x > 0) - (x < 0))
K1(u_squarei, int64_t, int64_t, i_mul(x, x))
K1(u_copyi, int64_t, int64_t, x)
K1(u_falsei, int64_t, int64_t, ((void)x, 0))
K1(u_truei, int64_t, int64_t, ((void)x, 1))
K1(u_noti, int64_t, int64_t, x == 0)
K1(u_signbiti, int64_t, int64_t, x < 0)
K1(u_inverti, int64_t, int64_t, ~x)
K1(u_invertb, int64_t, int64_t, x == 0)
KF1(copyf, x)
KF1(anglef, x < 0 ? M_PI : (x != x ? x : 0.0))

/* complex128 kernels */
K1(z_neg, zcplx, zcplx, -x)
K1(z_abs, zcplx, double, cabs(x))
K1(z_sign, zcplx, zcplx, x == 0 ? (zcplx)0 : x / cabs(x))
K1(z_square, zcplx, zcplx, x * x)
K1(z_sqrt, zcplx, zcplx, csqrt(x))
K1(z_recip, zcplx, zcplx, 1.0 / x)
K1(z_exp, zcplx, zcplx, cexp(x))
K1(z_exp2, zcplx, zcplx, cpow(2.0, x))
K1(z_expm1, zcplx, zcplx, cexp(x) - 1.0)
K1(z_log, zcplx, zcplx, clog(x))
K1(z_log2, zcplx, zcplx, clog(x) / M_LN2)
K1(z_log10, zcplx, zcplx, clog(x) / M_LN10)
K1(z_log1p, zcplx, zcplx, clog(1.0 + x))
K1(z_sin, zcplx, zcplx, csin(x))
K1(z_cos, zcplx, zcplx, ccos(x))
K1(z_tan, zcplx, zcplx, ctan(x))
K1(z_asin, zcplx, zcplx, casin(x))
K1(z_acos, zcplx, zcplx, cacos(x))
K1(z_atan, zcplx, zcplx, catan(x))
K1(z_sinh, zcplx, zcplx, csinh(x))
K1(z_cosh, zcplx, zcplx, ccosh(x))
K1(z_tanh, zcplx, zcplx, ctanh(x))
K1(z_asinh, zcplx, zcplx, casinh(x))
K1(z_acosh, zcplx, zcplx, cacosh(x))
K1(z_atanh, zcplx, zcplx, catanh(x))
K1(z_isnan, zcplx, int64_t, isnan(creal(x)) || isnan(cimag(x)))
K1(z_isinf, zcplx, int64_t, isinf(creal(x)) || isinf(cimag(x)))
K1(z_isfinite, zcplx, int64_t, isfinite(creal(x)) && isfinite(cimag(x)))
K1(z_not, zcplx, int64_t, x == 0)
K1(z_conj, zcplx, zcplx, conj(x))
K1(z_angle, zcplx, double, carg(x))

/* kind: U_FLOAT (ints go to float), U_SAME (int stays int; bool → int),
 * U_ROUND (ints unchanged), U_BOOL (a bool result), U_INT (ints only) */
enum { U_FLOAT, U_SAME, U_ROUND, U_BOOL, U_INT };

/* f: float64 kernel, g: float32, i: int64 (and bool), c: complex128 (zf:
 * its result is float64, as abs and angle) */
typedef struct { const char *name; Kern f, g, i; int kind; Kern c; bool zf; } Un;

#define UV(n) u_##n, v_##n
static const Un UN[] = {
    { "negative", UV(negf), u_negi, U_SAME, z_neg },  { "absolute", UV(absf), u_absi, U_SAME, z_abs, true },
    { "sign", UV(signf), u_signi, U_SAME, z_sign },   { "square", UV(squaref), u_squarei, U_SAME, z_square },
    { "sqrt", UV(sqrt), NULL, U_FLOAT, z_sqrt },      { "cbrt", UV(cbrt), NULL, U_FLOAT },
    { "reciprocal", UV(recip), NULL, U_FLOAT, z_recip }, { "exp", UV(exp), NULL, U_FLOAT, z_exp },
    { "exp2", UV(exp2), NULL, U_FLOAT, z_exp2 },      { "expm1", UV(expm1), NULL, U_FLOAT, z_expm1 },
    { "log", UV(log), NULL, U_FLOAT, z_log },         { "log2", UV(log2), NULL, U_FLOAT, z_log2 },
    { "log10", UV(log10), NULL, U_FLOAT, z_log10 },   { "log1p", UV(log1p), NULL, U_FLOAT, z_log1p },
    { "sin", UV(sin), NULL, U_FLOAT, z_sin },         { "cos", UV(cos), NULL, U_FLOAT, z_cos },
    { "tan", UV(tan), NULL, U_FLOAT, z_tan },         { "arcsin", UV(asin), NULL, U_FLOAT, z_asin },
    { "arccos", UV(acos), NULL, U_FLOAT, z_acos },    { "arctan", UV(atan), NULL, U_FLOAT, z_atan },
    { "sinh", UV(sinh), NULL, U_FLOAT, z_sinh },      { "cosh", UV(cosh), NULL, U_FLOAT, z_cosh },
    { "tanh", UV(tanh), NULL, U_FLOAT, z_tanh },      { "arcsinh", UV(asinh), NULL, U_FLOAT, z_asinh },
    { "arccosh", UV(acosh), NULL, U_FLOAT, z_acosh }, { "arctanh", UV(atanh), NULL, U_FLOAT, z_atanh },
    { "floor", UV(floor), u_copyi, U_ROUND },         { "ceil", UV(ceil), u_copyi, U_ROUND },
    { "trunc", UV(trunc), u_copyi, U_ROUND },         { "rint", UV(rint), u_copyi, U_ROUND },
    { "isnan", UV(isnan), u_falsei, U_BOOL, z_isnan }, { "isinf", UV(isinf), u_falsei, U_BOOL, z_isinf },
    { "isfinite", UV(isfinite), u_truei, U_BOOL, z_isfinite }, { "logical_not", UV(notf), u_noti, U_BOOL, z_not },
    { "signbit", UV(signbit), u_signbiti, U_BOOL },   { "invert", NULL, NULL, u_inverti, U_INT },
    { "conjugate", UV(copyf), u_copyi, U_ROUND, z_conj }, { "angle", UV(anglef), NULL, U_FLOAT, z_angle, true },
};

static SeaValue unary(SeaVM *vm, int argc, SeaValue *args, const Un *op) {
    if (argc < 1) FAIL("ndarray.%s: expected an array", op->name);
    Tmp t = { 0 };
    ND a;
    opnd(vm, args[0], &a, &t, op->name);
    Kern k;
    int odt;
    if (a.dt == DT_C128) {
        if (!op->c) FAIL("ndarray.%s: not defined for complex numbers", op->name);
        k = op->c;
        odt = op->kind == U_BOOL ? DT_BOOL : op->zf ? DT_F64 : DT_C128;
    } else if (op->kind == U_INT) {
        if (is_flt(a.dt)) FAIL("ndarray.%s: only for int and bool arrays", op->name);
        k = a.dt == DT_BOOL ? u_invertb : op->i;
        odt = a.dt;
    } else if (is_flt(a.dt)) {
        k = a.dt == DT_F32 ? op->g : op->f;
        odt = op->kind == U_BOOL ? DT_BOOL : a.dt;
    } else if (op->kind == U_FLOAT) {
        as_dt(vm, &a, DT_F64, &t);
        k = op->f;
        odt = DT_F64;
    } else {
        k = op->i;
        odt = op->kind == U_BOOL ? DT_BOOL : (op->kind == U_ROUND ? a.dt : DT_INT);
        if (a.dt == DT_BOOL && op->kind == U_ROUND) odt = DT_BOOL;
    }
    ND r, out; bool has_out;
    SeaValue res = result_for(vm, argc, args, 1, odt, a.nd, a.shape, &r, &out, &has_out, op->name);
    ND *ops[2] = { &r, &a };
    run(ops, 2, a.nd, a.shape, k, NULL);
    tmp_free(&t);
    return result_done(vm, argc, args, 1, res, &r, &out, has_out, op->name);
}

#define UN_FN(idx, cname) static SeaValue cname(SeaVM *vm, int argc, SeaValue *args) { return unary(vm, argc, args, &UN[idx]); }
UN_FN(0, n_negative) UN_FN(1, n_absolute) UN_FN(2, n_sign) UN_FN(3, n_square) UN_FN(4, n_sqrt) UN_FN(5, n_cbrt)
UN_FN(6, n_reciprocal) UN_FN(7, n_exp) UN_FN(8, n_exp2) UN_FN(9, n_expm1) UN_FN(10, n_log) UN_FN(11, n_log2)
UN_FN(12, n_log10) UN_FN(13, n_log1p) UN_FN(14, n_sin) UN_FN(15, n_cos) UN_FN(16, n_tan) UN_FN(17, n_arcsin)
UN_FN(18, n_arccos) UN_FN(19, n_arctan) UN_FN(20, n_sinh) UN_FN(21, n_cosh) UN_FN(22, n_tanh)
UN_FN(23, n_arcsinh) UN_FN(24, n_arccosh) UN_FN(25, n_arctanh) UN_FN(26, n_floor) UN_FN(27, n_ceil)
UN_FN(28, n_trunc) UN_FN(29, n_rint) UN_FN(30, n_isnan) UN_FN(31, n_isinf) UN_FN(32, n_isfinite)
UN_FN(33, n_logical_not) UN_FN(34, n_signbit) UN_FN(35, n_invert) UN_FN(36, n_conjugate) UN_FN(37, n_angle)

/* where(cond, x, y) */
static void k_where_f(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    double *o = (double *)p[0]; const int64_t *c = (const int64_t *)p[1];
    const double *x = (const double *)p[2], *y = (const double *)p[3];
    for (int64_t i = 0; i < n; i++) o[i * s[0]] = c[i * s[1]] ? x[i * s[2]] : y[i * s[3]];
}
static void k_where_g(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    float *o = (float *)p[0]; const int64_t *c = (const int64_t *)p[1];
    const float *x = (const float *)p[2], *y = (const float *)p[3];
    for (int64_t i = 0; i < n; i++) o[i * s[0]] = c[i * s[1]] ? x[i * s[2]] : y[i * s[3]];
}
static void k_where_c(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    zcplx *o = (zcplx *)p[0]; const int64_t *c = (const int64_t *)p[1];
    const zcplx *x = (const zcplx *)p[2], *y = (const zcplx *)p[3];
    for (int64_t i = 0; i < n; i++) o[i * s[0]] = c[i * s[1]] ? x[i * s[2]] : y[i * s[3]];
}
static void k_where_i(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    int64_t *o = (int64_t *)p[0]; const int64_t *c = (const int64_t *)p[1];
    const int64_t *x = (const int64_t *)p[2], *y = (const int64_t *)p[3];
    for (int64_t i = 0; i < n; i++) o[i * s[0]] = c[i * s[1]] ? x[i * s[2]] : y[i * s[3]];
}

static SeaValue n_where(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 3) FAIL("ndarray.where: expected (cond, x, y)");
    Tmp t = { 0 };
    ND c, x, y;
    opnd(vm, args[0], &c, &t, "where");
    opnd(vm, args[1], &x, &t, "where");
    opnd(vm, args[2], &y, &t, "where");
    int dt = promote_w(&x, &y);
    ND *ops0[3] = { &c, &x, &y };
    int nd; int64_t shape[MAXD];
    if (!bshape(ops0, 3, &nd, shape)) bfail(vm, "where", ops0, 3);
    as_dt(vm, &c, DT_BOOL, &t);
    as_dt(vm, &x, dt, &t);
    as_dt(vm, &y, dt, &t);
    ND r;
    SeaValue res = nd_raw(vm, dt, nd, shape, &r);
    ND *ops[4] = { &r, &c, &x, &y };
    run(ops, 4, nd, shape, dt == DT_C128 ? k_where_c : dt == DT_F64 ? k_where_f : dt == DT_F32 ? k_where_g : k_where_i, NULL);
    tmp_free(&t);
    return finish(res, &r);
}

/* ---- reductions -------------------------------------------------------------------- */

enum { R_SUM, R_PROD, R_MIN, R_MAX, R_ANY, R_ALL, R_NANSUM };

/* float reductions accumulate into float64 (k_*_f from float64, k_*_g from
 * float32: a float32 result is rounded at the end) */
#define RED_FLOAT(SUFFIX, TA)                                                                          \
    static void k_sum_##SUFFIX(int64_t n, char **p, const int64_t *s, void *ctx) {                     \
        (void)ctx;                                                                                     \
        double *o = (double *)p[0]; const TA *a = (const TA *)p[1];                                    \
        int64_t so = s[0], sa = s[1];                                                                  \
        if (so == 0) {                                                                                 \
            double s0 = 0, s1 = 0, s2 = 0, s3 = 0;                                                     \
            int64_t i = 0;                                                                             \
            if (sa == 1) for (; i + 4 <= n; i += 4) { s0 += a[i]; s1 += a[i + 1]; s2 += a[i + 2]; s3 += a[i + 3]; } \
            for (; i < n; i++) s0 += a[i * sa];                                                        \
            o[0] += (s0 + s1) + (s2 + s3);                                                             \
        } else if (so == 1 && sa == 1) {                                                               \
            for (int64_t i = 0; i < n; i++) o[i] += a[i];                                              \
        } else {                                                                                       \
            for (int64_t i = 0; i < n; i++) o[i * so] += a[i * sa];                                    \
        }                                                                                              \
    }                                                                                                  \
    static void k_nansum_##SUFFIX(int64_t n, char **p, const int64_t *s, void *ctx) {                  \
        (void)ctx;                                                                                     \
        double *o = (double *)p[0]; const TA *a = (const TA *)p[1];                                    \
        for (int64_t i = 0; i < n; i++) { double x = a[i * s[1]]; if (x == x) o[i * s[0]] += x; }      \
    }                                                                                                  \
    static void k_prod_##SUFFIX(int64_t n, char **p, const int64_t *s, void *ctx) {                    \
        (void)ctx;                                                                                     \
        double *o = (double *)p[0]; const TA *a = (const TA *)p[1];                                    \
        for (int64_t i = 0; i < n; i++) o[i * s[0]] *= a[i * s[1]];                                    \
    }                                                                                                  \
    static void k_min_##SUFFIX(int64_t n, char **p, const int64_t *s, void *ctx) {                     \
        (void)ctx;                                                                                     \
        double *o = (double *)p[0]; const TA *a = (const TA *)p[1];                                    \
        for (int64_t i = 0; i < n; i++) { double x = a[i * s[1]], *q = &o[i * s[0]]; if (x < *q || x != x) *q = x; } \
    }                                                                                                  \
    static void k_max_##SUFFIX(int64_t n, char **p, const int64_t *s, void *ctx) {                     \
        (void)ctx;                                                                                     \
        double *o = (double *)p[0]; const TA *a = (const TA *)p[1];                                    \
        for (int64_t i = 0; i < n; i++) { double x = a[i * s[1]], *q = &o[i * s[0]]; if (x > *q || x != x) *q = x; } \
    }

RED_FLOAT(f, double)
RED_FLOAT(g, float)

static void k_sum_c(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    zcplx *o = (zcplx *)p[0]; const zcplx *a = (const zcplx *)p[1];
    for (int64_t i = 0; i < n; i++) o[i * s[0]] += a[i * s[1]];
}
static void k_prod_c(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    zcplx *o = (zcplx *)p[0]; const zcplx *a = (const zcplx *)p[1];
    for (int64_t i = 0; i < n; i++) o[i * s[0]] *= a[i * s[1]];
}

/* complex numbers have no order: min, max, sorting... refuse them */
static void unordered(SeaVM *vm, const ND *a, const char *fn) {
    if (a->dt == DT_C128) FAIL("ndarray.%s: complex numbers are not ordered", fn);
}

static void k_sum_i(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    int64_t *o = (int64_t *)p[0]; const int64_t *a = (const int64_t *)p[1];
    int64_t so = s[0], sa = s[1];
    if (so == 0) {
        uint64_t acc = (uint64_t)o[0];
        for (int64_t i = 0; i < n; i++) acc += (uint64_t)a[i * sa];
        o[0] = (int64_t)acc;
    } else {
        for (int64_t i = 0; i < n; i++) o[i * so] = i_add(o[i * so], a[i * sa]);
    }
}

static void k_prod_i(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    int64_t *o = (int64_t *)p[0]; const int64_t *a = (const int64_t *)p[1];
    for (int64_t i = 0; i < n; i++) o[i * s[0]] = i_mul(o[i * s[0]], a[i * s[1]]);
}

static void k_min_i(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    int64_t *o = (int64_t *)p[0]; const int64_t *a = (const int64_t *)p[1];
    for (int64_t i = 0; i < n; i++) { int64_t x = a[i * s[1]]; if (x < o[i * s[0]]) o[i * s[0]] = x; }
}
static void k_max_i(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    int64_t *o = (int64_t *)p[0]; const int64_t *a = (const int64_t *)p[1];
    for (int64_t i = 0; i < n; i++) { int64_t x = a[i * s[1]]; if (x > o[i * s[0]]) o[i * s[0]] = x; }
}
static void k_any_i(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    int64_t *o = (int64_t *)p[0]; const int64_t *a = (const int64_t *)p[1];
    for (int64_t i = 0; i < n; i++) if (a[i * s[1]]) o[i * s[0]] = 1;
}
static void k_all_i(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    int64_t *o = (int64_t *)p[0]; const int64_t *a = (const int64_t *)p[1];
    for (int64_t i = 0; i < n; i++) if (!a[i * s[1]]) o[i * s[0]] = 0;
}

/* the axes argument: nil (all), an int, or an array of ints → mask */
static void axes_arg(SeaVM *vm, SeaValue v, int nd, bool *mask, const char *fn) {
    for (int i = 0; i < nd; i++) mask[i] = sea_is_nil(v);
    if (sea_is_nil(v)) return;
    if (is_num(v)) { mask[norm_axis(vm, iv(v), nd, fn)] = true; return; }
    if (sea_is_array(v)) {
        SeaArray *a = SEA_AS_ARRAY(v);
        for (uint32_t i = 0; i < a->len; i++) {
            if (!is_num(a->data[i])) FAIL("ndarray.%s: axes are ints", fn);
            int ax = norm_axis(vm, iv(a->data[i]), nd, fn);
            if (mask[ax]) FAIL("ndarray.%s: repeated axis", fn);
            mask[ax] = true;
        }
        return;
    }
    FAIL("ndarray.%s: axis is an int or an array of ints, got %s", fn, sea_type_name(v));
}

/* an options value: a table or a struct literal ({axis = 0}) */
static bool is_opts(SeaValue v) { return sea_is_struct(v) || (sea_is_table(v) && !is_nd(v)); }

static SeaValue opt(SeaValue v, const char *name) {
    if (sea_is_table(v)) return fld(SEA_AS_TABLE(v), name);
    if (sea_is_struct(v)) {
        SeaStruct *s = SEA_AS_STRUCT(v);
        SeaString **names = sea_struct_names(s);
        size_t n = strlen(name);
        for (uint32_t i = 0; i < s->field_count; i++)
            if (names[i]->len == n && memcmp(names[i]->data, name, n) == 0) return sea_struct_values(s)[i];
    }
    return SEA_NIL_VAL;
}

/* the options of a reduction: (a, axis, keepdims) or (a, {axis, keepdims, ddof}) */
typedef struct { SeaValue axis; bool keepdims; double ddof; SeaValue out; } ROpts;

static void ropts(SeaVM *vm, int argc, SeaValue *args, ROpts *o) {
    o->axis = SEA_NIL_VAL; o->keepdims = false; o->ddof = 0; o->out = SEA_NIL_VAL;
    if (argc > 1 && is_opts(args[1])) {
        o->axis = opt(args[1], "axis");
        o->keepdims = sea_is_truthy(opt(args[1], "keepdims"));
        SeaValue d = opt(args[1], "ddof");
        if (is_num(d)) o->ddof = dv(d);
        return;
    }
    if (argc > 1) o->axis = args[1];
    if (argc > 2) o->keepdims = sea_is_truthy(args[2]);
}

/* the shapes of a reduction: kept (the reduced axes as 1) and final */
static int64_t red_shapes(const ND *a, const bool *mask, bool keepdims, int64_t *kshape, int *fnd, int64_t *fshape) {
    int64_t count = 1;
    int m = 0;
    for (int i = 0; i < a->nd; i++) {
        kshape[i] = mask[i] ? 1 : a->shape[i];
        if (mask[i]) count *= a->shape[i];
        if (!mask[i] || keepdims) fshape[m++] = mask[i] ? 1 : a->shape[i];
    }
    *fnd = m;
    return count;
}

/* reduce a (already in the compute dtype; float32 reduces into float64)
 * into a new array of dtype odt with the kept shape; *o describes it */
static SeaValue reduce_into(SeaVM *vm, ND *a, const bool *mask, int op, int odt, ND *o, const char *fn) {
    int64_t kshape[MAXD], fshape[MAXD];
    int fnd;
    int64_t count = red_shapes(a, mask, true, kshape, &fnd, fshape);
    if ((op == R_MIN || op == R_MAX) && count == 0)
        FAIL("ndarray.%s: zero-size array to a reduction with no identity", fn);
    SeaValue t = nd_new(vm, odt, a->nd, kshape, o);
    int64_t n = o->size;
    if (odt == DT_C128) {
        if (op == R_PROD) for (int64_t i = 0; i < n; i++) ((zcplx *)o->p)[i] = 1;
    } else if (odt == DT_F64) {
        double init = op == R_PROD ? 1.0 : op == R_MIN ? INFINITY : op == R_MAX ? -INFINITY : 0.0;
        for (int64_t i = 0; i < n; i++) ((double *)o->p)[i] = init;
    } else {
        int64_t init = op == R_PROD || op == R_ALL ? 1 : op == R_MIN ? INT64_MAX : op == R_MAX ? INT64_MIN : 0;
        for (int64_t i = 0; i < n; i++) ((int64_t *)o->p)[i] = init;
    }
    Kern k;
    bool f = a->dt == DT_F64, g = a->dt == DT_F32;
    if (a->dt == DT_C128) {
        if (op != R_SUM && op != R_PROD && op != R_NANSUM) unordered(vm, a, fn);
        ND *ops[2] = { o, a };
        run(ops, 2, a->nd, a->shape, op == R_PROD ? k_prod_c : k_sum_c, NULL);
        return t;
    }
    switch (op) {
    case R_SUM: k = f ? k_sum_f : g ? k_sum_g : k_sum_i; break;
    case R_NANSUM: k = f ? k_nansum_f : g ? k_nansum_g : k_sum_i; break;
    case R_PROD: k = f ? k_prod_f : g ? k_prod_g : k_prod_i; break;
    case R_MIN: k = f ? k_min_f : g ? k_min_g : k_min_i; break;
    case R_MAX: k = f ? k_max_f : g ? k_max_g : k_max_i; break;
    case R_ANY: k = k_any_i; break;
    default: k = k_all_i; break;
    }
    ND *ops[2] = { o, a };
    run(ops, 2, a->nd, a->shape, k, NULL);
    return t;
}

/* the reduced array (kept shape in o) as the final result (owned) */
static SeaValue red_final(SeaValue t, ND *o, const ND *a, const bool *mask, bool keepdims) {
    if (keepdims) return finish(t, o);
    int64_t kshape[MAXD], fshape[MAXD], st[MAXD];
    int fnd;
    red_shapes(a, mask, false, kshape, &fnd, fshape);
    contig_strides(fnd, fshape, st);
    if (fnd == 0) return finish(t, &(ND){ .dt = o->dt, .nd = 0, .p = o->p });
    SeaValue r = wrap(o->data, o->dt, fnd, fshape, st, 0);
    sea_obj_release(sea_as_obj(t));
    return r;
}

/* a result computed in float64 for a float32 input, as float32 (owned in,
 * owned out; a scalar is rounded to float32's precision) */
static SeaValue as_f32_result(SeaVM *vm, SeaValue r) {
    if (sea_is_number(r)) return sea_number_val((double)(float)sea_as_number(r));
    if (!is_nd(r)) return r;
    ND a, o;
    nd_read(vm, r, &a, "float32");
    SeaValue c = copy_as(vm, &a, DT_F32, &o);
    sea_obj_release(sea_as_obj(r));
    return c;
}

static SeaValue reduction(SeaVM *vm, int argc, SeaValue *args, int op, const char *fn) {
    if (argc < 1) FAIL("ndarray.%s: expected an array", fn);
    Tmp t = { 0 };
    ND a;
    opnd(vm, args[0], &a, &t, fn);
    ROpts ro;
    ropts(vm, argc, args, &ro);
    bool mask[MAXD];
    axes_arg(vm, ro.axis, a.nd, mask, fn);
    /* the result's dtype: bool for any/all, the input's for min/max, int64
     * for the sum or product of bools; computed on int64 storage */
    int odt;
    if (op == R_ANY || op == R_ALL) { as_dt(vm, &a, DT_BOOL, &t); odt = DT_BOOL; }
    else if (op == R_MIN || op == R_MAX) { unordered(vm, &a, fn); odt = a.dt; }
    else odt = a.dt == DT_BOOL ? DT_INT : a.dt;
    if (a.dt == DT_BOOL) a.dt = DT_INT;
    ND o;
    SeaValue r = reduce_into(vm, &a, mask, op, a.dt == DT_F32 ? DT_F64 : a.dt, &o, fn);
    if (odt == DT_BOOL) {
        /* bool result: same storage, relabelled */
        SeaValue b = wrap(o.data, DT_BOOL, o.nd, o.shape, o.strides, 0);
        sea_obj_release(sea_as_obj(r));
        r = b;
        o.dt = DT_BOOL;
    }
    SeaValue res = red_final(r, &o, &a, mask, ro.keepdims);
    tmp_free(&t);
    return odt == DT_F32 ? as_f32_result(vm, res) : res;
}

static SeaValue n_sum(SeaVM *vm, int argc, SeaValue *args) { return reduction(vm, argc, args, R_SUM, "sum"); }
static SeaValue n_nansum(SeaVM *vm, int argc, SeaValue *args) { return reduction(vm, argc, args, R_NANSUM, "nansum"); }
static SeaValue n_prod(SeaVM *vm, int argc, SeaValue *args) { return reduction(vm, argc, args, R_PROD, "prod"); }
static SeaValue n_min(SeaVM *vm, int argc, SeaValue *args) { return reduction(vm, argc, args, R_MIN, "min"); }
static SeaValue n_max(SeaVM *vm, int argc, SeaValue *args) { return reduction(vm, argc, args, R_MAX, "max"); }
static SeaValue n_any(SeaVM *vm, int argc, SeaValue *args) { return reduction(vm, argc, args, R_ANY, "any"); }
static SeaValue n_all(SeaVM *vm, int argc, SeaValue *args) { return reduction(vm, argc, args, R_ALL, "all"); }

static void k_divn(int64_t n, char **p, const int64_t *s, void *ctx) {
    double d = *(double *)ctx;
    double *o = (double *)p[0];
    for (int64_t i = 0; i < n; i++) o[i * s[0]] /= d;
}
static void k_sqdev(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    double *o = (double *)p[0]; const double *a = (const double *)p[1], *m = (const double *)p[2];
    for (int64_t i = 0; i < n; i++) { double d = a[i * s[1]] - m[i * s[2]]; o[i * s[0]] += d * d; }
}
static void k_count_ok(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    double *o = (double *)p[0]; const double *a = (const double *)p[1];
    for (int64_t i = 0; i < n; i++) if (a[i * s[1]] == a[i * s[1]]) o[i * s[0]] += 1;
}
static void k_divn_c(int64_t n, char **p, const int64_t *s, void *ctx) {
    double d = *(double *)ctx;
    zcplx *o = (zcplx *)p[0];
    for (int64_t i = 0; i < n; i++) o[i * s[0]] /= d;
}
/* |a - m|² for complex a and mean m, into float64 */
static void k_sqdev_c(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    double *o = (double *)p[0]; const zcplx *a = (const zcplx *)p[1], *m = (const zcplx *)p[2];
    for (int64_t i = 0; i < n; i++) {
        zcplx d = a[i * s[1]] - m[i * s[2]];
        o[i * s[0]] += creal(d) * creal(d) + cimag(d) * cimag(d);
    }
}
static void k_div_arr(int64_t n, char **p, const int64_t *s, void *ctx) {
    (void)ctx;
    double *o = (double *)p[0]; const double *c = (const double *)p[1];
    for (int64_t i = 0; i < n; i++) o[i * s[0]] /= c[i * s[1]];
}

/* mean / var / std (what 0..2); nanmean is 3 */
static SeaValue moments(SeaVM *vm, int argc, SeaValue *args, int what, const char *fn) {
    if (argc < 1) FAIL("ndarray.%s: expected an array", fn);
    Tmp t = { 0 };
    ND a;
    opnd(vm, args[0], &a, &t, fn);
    ROpts ro;
    ropts(vm, argc, args, &ro);
    bool mask[MAXD];
    axes_arg(vm, ro.axis, a.nd, mask, fn);
    bool f32 = a.dt == DT_F32;
    int64_t kshape[MAXD], fshape[MAXD];
    int fnd;
    int64_t count = red_shapes(&a, mask, true, kshape, &fnd, fshape);
    if (a.dt == DT_C128) {
        /* the mean is complex; var and std are of |x - mean|, float64 */
        if (what == 3) FAIL("ndarray.%s: not for complex numbers", fn);
        ND m;
        SeaValue mt = reduce_into(vm, &a, mask, R_SUM, DT_C128, &m, fn);
        double n = (double)count;
        ND *mo[1] = { &m };
        run(mo, 1, m.nd, m.shape, k_divn_c, &n);
        if (what == 1 || what == 2) {
            ND v;
            SeaValue vt = nd_new(vm, DT_F64, a.nd, kshape, &v);
            ND *ops[3] = { &v, &a, &m };
            run(ops, 3, a.nd, a.shape, k_sqdev_c, NULL);
            double dn = (double)count - ro.ddof;
            ND *vo[1] = { &v };
            run(vo, 1, v.nd, v.shape, k_divn, &dn);
            if (what == 2) {
                ND *ops2[2] = { &v, &v };
                run(ops2, 2, v.nd, v.shape, u_sqrt, NULL);
            }
            sea_obj_release(sea_as_obj(mt));
            mt = vt;
            m = v;
        }
        SeaValue res = red_final(mt, &m, &a, mask, ro.keepdims);
        tmp_free(&t);
        return res;
    }
    as_dt(vm, &a, DT_F64, &t);
    ND m;
    SeaValue mt = reduce_into(vm, &a, mask, what == 3 ? R_NANSUM : R_SUM, DT_F64, &m, fn);
    ND *mo[1] = { &m };
    if (what == 3) {
        ND c;
        SeaValue ct = nd_new(vm, DT_F64, a.nd, kshape, &c);
        ND *ops[2] = { &c, &a };
        run(ops, 2, a.nd, a.shape, k_count_ok, NULL);
        ND *dv_[2] = { &m, &c };
        run(dv_, 2, m.nd, m.shape, k_div_arr, NULL);
        sea_obj_release(sea_as_obj(ct));
    } else {
        double n = (double)count;
        run(mo, 1, m.nd, m.shape, k_divn, &n);
    }
    if (what == 1 || what == 2) {
        ND v;
        SeaValue vt = nd_new(vm, DT_F64, a.nd, kshape, &v);
        ND *ops[3] = { &v, &a, &m };
        run(ops, 3, a.nd, a.shape, k_sqdev, NULL);
        double n = (double)count - ro.ddof;
        ND *vo[1] = { &v };
        run(vo, 1, v.nd, v.shape, k_divn, &n);
        if (what == 2) {
            ND *ops2[2] = { &v, &v };
            run(ops2, 2, v.nd, v.shape, u_sqrt, NULL);
        }
        sea_obj_release(sea_as_obj(mt));
        mt = vt;
        m = v;
    }
    SeaValue res = red_final(mt, &m, &a, mask, ro.keepdims);
    tmp_free(&t);
    return f32 ? as_f32_result(vm, res) : res;
}

static SeaValue n_mean(SeaVM *vm, int argc, SeaValue *args) { return moments(vm, argc, args, 0, "mean"); }
static SeaValue n_var(SeaVM *vm, int argc, SeaValue *args) { return moments(vm, argc, args, 1, "var"); }
static SeaValue n_std(SeaVM *vm, int argc, SeaValue *args) { return moments(vm, argc, args, 2, "std"); }
static SeaValue n_nanmean(SeaVM *vm, int argc, SeaValue *args) { return moments(vm, argc, args, 3, "nanmean"); }

/* ---- along one axis ----------------------------------------------------------------- */

typedef void (*LineFn)(void *ctx, char *pa, int64_t sa, char *pb, int64_t sb, int64_t n, int64_t k);

/* f over every line of a along axis (and the same line of b, same shape
 * except maybe along axis), in C order of the other dimensions */
static void lines2(ND *a, ND *b, int axis, LineFn f, void *ctx) {
    int m = 0;
    int64_t sh[MAXD], sa[MAXD], sb[MAXD], idx[MAXD] = { 0 };
    for (int d = 0; d < a->nd; d++) {
        if (d == axis) continue;
        sh[m] = a->shape[d]; sa[m] = a->strides[d]; sb[m] = b ? b->strides[d] : 0; m++;
    }
    int64_t total = prod(sh, m);
    char *pa = a->p, *pb = b ? b->p : NULL;
    int64_t n = a->shape[axis], ast = a->strides[axis], bst = b ? b->strides[axis] : 0;
    int64_t ea = ESZ(a->dt), eb = b ? ESZ(b->dt) : 8;
    for (int64_t k = 0; k < total; k++) {
        f(ctx, pa, ast, pb, bst, n, k);
        for (int d = m - 1; d >= 0; d--) {
            pa += sa[d] * ea; if (pb) pb += sb[d] * eb;
            if (++idx[d] < sh[d]) break;
            pa -= sa[d] * sh[d] * ea; if (pb) pb -= sb[d] * sh[d] * eb;
            idx[d] = 0;
        }
    }
}

typedef struct { int dt; bool max; } ArgCtx;

static void line_arg(void *ctx, char *pa, int64_t sa, char *pb, int64_t sb, int64_t n, int64_t k) {
    (void)sb; (void)k;
    const ArgCtx *c = ctx;
    int64_t best = 0;
    if (c->dt == DT_F64) {
        const double *a = (const double *)pa;
        double bv = a[0];
        if (bv == bv)
            for (int64_t i = 1; i < n; i++) {
                double x = a[i * sa];
                if (x != x) { best = i; break; }
                if (c->max ? x > bv : x < bv) { bv = x; best = i; }
            }
    } else {
        const int64_t *a = (const int64_t *)pa;
        int64_t bv = a[0];
        for (int64_t i = 1; i < n; i++) {
            int64_t x = a[i * sa];
            if (c->max ? x > bv : x < bv) { bv = x; best = i; }
        }
    }
    *(int64_t *)pb = best;
}

static SeaValue argext(SeaVM *vm, int argc, SeaValue *args, bool max, const char *fn) {
    if (argc < 1) FAIL("ndarray.%s: expected an array", fn);
    Tmp t = { 0 };
    ND a;
    opnd(vm, args[0], &a, &t, fn);
    unordered(vm, &a, fn);
    if (a.dt == DT_F32) as_dt(vm, &a, DT_F64, &t);
    ROpts ro;
    ropts(vm, argc, args, &ro);
    if (sea_is_nil(ro.axis)) {
        if (!is_contig(&a) || a.nd != 1) {
            ND c;
            SeaValue ct = copy_as(vm, &a, a.dt, &c);
            tmp_hold(&t, ct);
            a = c;
        }
        a.nd = 1; a.shape[0] = a.size; a.strides[0] = 1;
    }
    int axis = sea_is_nil(ro.axis) ? 0 : norm_axis(vm, iv(ro.axis), a.nd, fn);
    if (a.shape[axis] == 0) FAIL("ndarray.%s: an empty sequence has no %s", fn, max ? "maximum" : "minimum");
    int64_t kshape[MAXD];
    for (int d = 0; d < a.nd; d++) kshape[d] = d == axis ? 1 : a.shape[d];
    ND o;
    SeaValue r = nd_new(vm, DT_INT, a.nd, kshape, &o);
    ArgCtx c = { a.dt, max };
    lines2(&a, &o, axis, line_arg, &c);
    bool mask[MAXD] = { false };
    mask[axis] = true;
    SeaValue res = red_final(r, &o, &a, mask, ro.keepdims && !sea_is_nil(ro.axis));
    tmp_free(&t);
    return res;
}

static SeaValue n_argmax(SeaVM *vm, int argc, SeaValue *args) { return argext(vm, argc, args, true, "argmax"); }
static SeaValue n_argmin(SeaVM *vm, int argc, SeaValue *args) { return argext(vm, argc, args, false, "argmin"); }

static void line_cumsum(void *ctx, char *pa, int64_t sa, char *pb, int64_t sb, int64_t n, int64_t k) {
    (void)k;
    int what = *(int *)ctx;
    if (what & 4) {
        const zcplx *a = (const zcplx *)pa; zcplx *b = (zcplx *)pb;
        zcplx acc = what & 1 ? 1 : 0;
        for (int64_t i = 0; i < n; i++) { acc = what & 1 ? acc * a[i * sa] : acc + a[i * sa]; b[i * sb] = acc; }
    } else if (what & 2) {
        const int64_t *a = (const int64_t *)pa; int64_t *b = (int64_t *)pb;
        int64_t acc = what & 1 ? 1 : 0;
        for (int64_t i = 0; i < n; i++) { acc = what & 1 ? i_mul(acc, a[i * sa]) : i_add(acc, a[i * sa]); b[i * sb] = acc; }
    } else {
        const double *a = (const double *)pa; double *b = (double *)pb;
        double acc = what & 1 ? 1 : 0;
        for (int64_t i = 0; i < n; i++) { acc = what & 1 ? acc * a[i * sa] : acc + a[i * sa]; b[i * sb] = acc; }
    }
}

static SeaValue cumulative(SeaVM *vm, int argc, SeaValue *args, int prodp, const char *fn) {
    if (argc < 1) FAIL("ndarray.%s: expected an array", fn);
    Tmp t = { 0 };
    ND a;
    opnd(vm, args[0], &a, &t, fn);
    SeaValue axv = argc > 1 ? args[1] : SEA_NIL_VAL;
    if (a.dt == DT_BOOL) as_dt(vm, &a, DT_INT, &t);
    bool f32 = a.dt == DT_F32;
    if (f32) as_dt(vm, &a, DT_F64, &t);
    if (sea_is_nil(axv)) {
        if (!is_contig(&a)) {
            ND c;
            tmp_hold(&t, copy_as(vm, &a, a.dt, &c));
            a = c;
        }
        a.nd = 1; a.shape[0] = a.size; a.strides[0] = 1;
    }
    int axis = sea_is_nil(axv) ? 0 : norm_axis(vm, iv(axv), a.nd, fn);
    ND o;
    SeaValue r = nd_new(vm, a.dt, a.nd, a.shape, &o);
    int what = prodp | (a.dt == DT_C128 ? 4 : a.dt == DT_F64 ? 0 : 2);
    if (a.nd > 0 && a.size > 0) lines2(&a, &o, axis, line_cumsum, &what);
    tmp_free(&t);
    r = finish(r, &o);
    return f32 ? as_f32_result(vm, r) : r;
}

static SeaValue n_cumsum(SeaVM *vm, int argc, SeaValue *args) { return cumulative(vm, argc, args, 0, "cumsum"); }
static SeaValue n_cumprod(SeaVM *vm, int argc, SeaValue *args) { return cumulative(vm, argc, args, 1, "cumprod"); }

/* ---- sorting ------------------------------------------------------------------------ */

static int cmp_f(const void *x, const void *y) {
    double a = *(const double *)x, b = *(const double *)y;
    if (a < b) return -1;
    if (a > b) return 1;
    if (a == b) return 0;
    return (a != a) - (b != b);   /* NaN last */
}
static int cmp_i(const void *x, const void *y) {
    int64_t a = *(const int64_t *)x, b = *(const int64_t *)y;
    return (a > b) - (a < b);
}

typedef struct { int dt; char *buf; int64_t *idx, *tmp; } SortCtx;

static void line_sort(void *ctx, char *pa, int64_t sa, char *pb, int64_t sb, int64_t n, int64_t k) {
    (void)pb; (void)sb; (void)k;
    SortCtx *c = ctx;
    for (int64_t i = 0; i < n; i++) memcpy(c->buf + i * 8, pa + i * sa * 8, 8);
    qsort(c->buf, (size_t)n, 8, c->dt == DT_F64 ? cmp_f : cmp_i);
    for (int64_t i = 0; i < n; i++) memcpy(pa + i * sa * 8, c->buf + i * 8, 8);
}

/* stable merge sort of idx[0..n) by the values in buf */
static bool less_at(const SortCtx *c, int64_t i, int64_t j) {
    if (c->dt == DT_F64) {
        double a = ((double *)c->buf)[i], b = ((double *)c->buf)[j];
        return a < b || (a == a && b != b);
    }
    return ((int64_t *)c->buf)[i] < ((int64_t *)c->buf)[j];
}
static void msort(SortCtx *c, int64_t lo, int64_t hi) {
    if (hi - lo < 2) return;
    if (hi - lo <= 16) {
        for (int64_t i = lo + 1; i < hi; i++) {
            int64_t v = c->idx[i], j = i;
            while (j > lo && less_at(c, v, c->idx[j - 1])) { c->idx[j] = c->idx[j - 1]; j--; }
            c->idx[j] = v;
        }
        return;
    }
    int64_t mid = (lo + hi) / 2;
    msort(c, lo, mid);
    msort(c, mid, hi);
    if (!less_at(c, c->idx[mid], c->idx[mid - 1])) return;
    int64_t i = lo, j = mid, k = 0;
    while (i < mid && j < hi) c->tmp[k++] = less_at(c, c->idx[j], c->idx[i]) ? c->idx[j++] : c->idx[i++];
    while (i < mid) c->tmp[k++] = c->idx[i++];
    while (j < hi) c->tmp[k++] = c->idx[j++];
    memcpy(c->idx + lo, c->tmp, (size_t)k * 8);
}

static void line_argsort(void *ctx, char *pa, int64_t sa, char *pb, int64_t sb, int64_t n, int64_t k) {
    (void)k;
    SortCtx *c = ctx;
    for (int64_t i = 0; i < n; i++) { memcpy(c->buf + i * 8, pa + i * sa * 8, 8); c->idx[i] = i; }
    msort(c, 0, n);
    for (int64_t i = 0; i < n; i++) ((int64_t *)pb)[i * sb] = c->idx[i];
}

static SeaValue sorting(SeaVM *vm, int argc, SeaValue *args, bool arg, const char *fn) {
    if (argc < 1) FAIL("ndarray.%s: expected an array", fn);
    Tmp t = { 0 };
    ND a;
    opnd(vm, args[0], &a, &t, fn);
    unordered(vm, &a, fn);
    bool f32 = a.dt == DT_F32;
    if (f32) as_dt(vm, &a, DT_F64, &t);
    SeaValue axv = argc > 1 ? args[1] : sea_int_val(-1);
    if (sea_is_nil(axv)) {
        ND c;
        tmp_hold(&t, copy_as(vm, &a, a.dt, &c));
        a = c;
        a.nd = 1; a.shape[0] = a.size; a.strides[0] = 1;
        axv = sea_int_val(0);
    }
    if (a.nd == 0) FAIL("ndarray.%s: an array with no dimensions cannot be sorted", fn);
    int axis = norm_axis(vm, iv(axv), a.nd, fn);
    ND o;
    SeaValue r;
    int64_t n = a.shape[axis];
    if (arg) r = nd_new(vm, DT_INT, a.nd, a.shape, &o);
    else r = copy_as(vm, &a, a.dt, &o);
    SortCtx c = { a.dt, malloc((size_t)(n + 1) * 8), malloc((size_t)(n + 1) * 8), malloc((size_t)(n + 1) * 8) };
    if (a.size > 0) {
        if (arg) lines2(&a, &o, axis, line_argsort, &c);
        else lines2(&o, NULL, axis, line_sort, &c);
    }
    free(c.buf); free(c.idx); free(c.tmp);
    tmp_free(&t);
    return f32 && !arg ? as_f32_result(vm, r) : r;
}

static SeaValue n_sort(SeaVM *vm, int argc, SeaValue *args) { return sorting(vm, argc, args, false, "sort"); }
static SeaValue n_argsort(SeaVM *vm, int argc, SeaValue *args) { return sorting(vm, argc, args, true, "argsort"); }

/* equal values (NaNs equal to each other, as unique groups them) */
static bool same_val(int dt, const char *p, const char *q) {
    if (dt == DT_F64) {
        double x = *(const double *)p, y = *(const double *)q;
        return x == y || (x != x && y != y);
    }
    return *(const int64_t *)p == *(const int64_t *)q;
}

/* unique(a) → [values, first index, inverse, counts] (a flattened) */
static SeaValue n_unique(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.unique: expected an array");
    Tmp t = { 0 };
    ND a, c;
    opnd(vm, args[0], &a, &t, "unique");
    unordered(vm, &a, "unique");
    bool f32 = a.dt == DT_F32;
    tmp_hold(&t, copy_as(vm, &a, f32 ? DT_F64 : a.dt, &c));
    int64_t n = c.size;
    SortCtx sc = { c.dt, c.p, malloc((size_t)(n + 1) * 8), malloc((size_t)(n + 1) * 8) };
    for (int64_t i = 0; i < n; i++) sc.idx[i] = i;
    msort(&sc, 0, n);
    int64_t groups = 0;
    for (int64_t i = 0; i < n; i++)
        if (i == 0 || !same_val(c.dt, c.p + sc.idx[i] * 8, c.p + sc.idx[i - 1] * 8)) groups++;
    ND v, first, inv, cnt;
    int64_t gs[1] = { groups }, ns[1] = { n };
    SeaValue tv = nd_new(vm, c.dt, 1, gs, &v);
    SeaValue tf = nd_new(vm, DT_INT, 1, gs, &first);
    SeaValue ti = nd_new(vm, DT_INT, 1, ns, &inv);
    SeaValue tc = nd_new(vm, DT_INT, 1, gs, &cnt);
    int64_t g = -1;
    for (int64_t i = 0; i < n; i++) {
        int64_t j = sc.idx[i];
        if (i == 0 || !same_val(c.dt, c.p + j * 8, c.p + sc.idx[i - 1] * 8)) {
            g++;
            memcpy(v.p + g * 8, c.p + j * 8, 8);
            ((int64_t *)first.p)[g] = j;
        }
        ((int64_t *)inv.p)[j] = g;
        ((int64_t *)cnt.p)[g]++;
    }
    free(sc.idx); free(sc.tmp);
    tmp_free(&t);
    if (f32) tv = as_f32_result(vm, tv);
    SeaArray *out = sea_array_new(4);
    sea_array_push_move(out, tv);
    sea_array_push_move(out, tf);
    sea_array_push_move(out, ti);
    sea_array_push_move(out, tc);
    return sea_obj_val(&out->obj);
}

static inline bool nan_lt(double a, double b) { return a < b || (b != b && a == a); }

/* searchsorted(sorted, v, side) */
static SeaValue n_searchsorted(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) FAIL("ndarray.searchsorted: expected (a, v [, side])");
    Tmp t = { 0 };
    ND a, v;
    opnd(vm, args[0], &a, &t, "searchsorted");
    opnd(vm, args[1], &v, &t, "searchsorted");
    if (a.nd != 1) FAIL("ndarray.searchsorted: a must be one-dimensional");
    unordered(vm, &a, "searchsorted");
    unordered(vm, &v, "searchsorted");
    bool right = argc > 2 && str_eq(args[2], "right");
    as_dt(vm, &a, DT_F64, &t);
    as_dt(vm, &v, DT_F64, &t);
    ND vc;
    if (!is_contig(&v)) { tmp_hold(&t, copy_as(vm, &v, DT_F64, &vc)); v = vc; }
    ND o;
    SeaValue r = nd_new(vm, DT_INT, v.nd, v.shape, &o);
    for (int64_t k = 0; k < v.size; k++) {
        double x = ((double *)v.p)[k];
        int64_t lo = 0, hi = a.shape[0];
        while (lo < hi) {
            int64_t mid = (lo + hi) / 2;
            double y = *(double *)(a.p + mid * a.strides[0] * 8);
            /* NaN sorts last; left: the first place y >= x, right: y > x */
            bool go_right = right ? !nan_lt(x, y) : nan_lt(y, x);
            if (go_right) lo = mid + 1; else hi = mid;
        }
        ((int64_t *)o.p)[k] = lo;
    }
    tmp_free(&t);
    return finish(r, &o);
}

/* ---- matrix products ------------------------------------------------------------------ */

typedef struct {
    int dt;
    int64_t n, k, m;
    const char *A, *B;
    char *C;
    int64_t r0, r1;
} Gemm;

/* the rows r0..r1 of C += A·B for float elements of type T */
#define GEMM_FLOAT(T)                                                                                  \
    do {                                                                                               \
        const T *A = (const T *)g->A, *B = (const T *)g->B;                                            \
        T *C = (T *)g->C;                                                                              \
        if (m == 1) {                                                                                  \
            for (int64_t i = g->r0; i < g->r1; i++) {                                                  \
                const T *a = A + i * k;                                                                \
                T s0 = 0, s1 = 0, s2 = 0, s3 = 0;                                                      \
                int64_t p = 0;                                                                         \
                for (; p + 4 <= k; p += 4) { s0 += a[p] * B[p]; s1 += a[p + 1] * B[p + 1]; s2 += a[p + 2] * B[p + 2]; s3 += a[p + 3] * B[p + 3]; } \
                for (; p < k; p++) s0 += a[p] * B[p];                                                  \
                C[i] = (s0 + s1) + (s2 + s3);                                                          \
            }                                                                                          \
            break;                                                                                     \
        }                                                                                              \
        const int64_t KB = 256, JB = 1024;                                                             \
        for (int64_t j0 = 0; j0 < m; j0 += JB) {                                                       \
            int64_t j1 = j0 + JB < m ? j0 + JB : m;                                                    \
            for (int64_t p0 = 0; p0 < k; p0 += KB) {                                                   \
                int64_t p1 = p0 + KB < k ? p0 + KB : k;                                                \
                for (int64_t i = g->r0; i < g->r1; i++) {                                              \
                    T *c = C + i * m;                                                                  \
                    const T *a = A + i * k;                                                            \
                    for (int64_t p = p0; p < p1; p++) {                                                \
                        T x = a[p];                                                                    \
                        const T *b = B + p * m;                                                        \
                        for (int64_t j = j0; j < j1; j++) c[j] += x * b[j];                            \
                    }                                                                                  \
                }                                                                                      \
            }                                                                                          \
        }                                                                                              \
    } while (0)

static void gemm_rows(Gemm *g) {
    int64_t k = g->k, m = g->m;
    if (g->dt == DT_F64) {
        GEMM_FLOAT(double);
    } else if (g->dt == DT_F32) {
        GEMM_FLOAT(float);
    } else if (g->dt == DT_C128) {
        GEMM_FLOAT(zcplx);
    } else {
        const int64_t *A = (const int64_t *)g->A, *B = (const int64_t *)g->B;
        int64_t *C = (int64_t *)g->C;
        for (int64_t i = g->r0; i < g->r1; i++)
            for (int64_t p = 0; p < k; p++) {
                uint64_t x = (uint64_t)A[i * k + p];
                const int64_t *b = B + p * m;
                uint64_t *c = (uint64_t *)C + i * m;
                for (int64_t j = 0; j < m; j++) c[j] += x * (uint64_t)b[j];
            }
    }
}

static void *gemm_thread(void *arg) { gemm_rows(arg); return NULL; }

static int ncpu(void) {
    static int n = 0;
    if (!n) { long c = sysconf(_SC_NPROCESSORS_ONLN); n = c > 0 ? (int)c : 1; if (n > 16) n = 16; }
    return n;
}

/* C (n×m, zeroed) = A (n×k) · B (k×m), all contiguous */
static void gemm(int dt, int64_t n, int64_t k, int64_t m, const char *A, const char *B, char *C) {
    Gemm g = { dt, n, k, m, A, B, C, 0, n };
    double flops = (double)n * k * m;
    int nt = ncpu();
    if (flops < 4e6 || n < 8 || nt < 2) { gemm_rows(&g); return; }
    if (nt > n / 4) nt = (int)(n / 4) > 1 ? (int)(n / 4) : 1;
    pthread_t th[16];
    Gemm parts[16];
    int started = 0;
    for (int t = 0; t < nt; t++) {
        parts[t] = g;
        parts[t].r0 = n * t / nt;
        parts[t].r1 = n * (t + 1) / nt;
        if (t == nt - 1 || pthread_create(&th[t], NULL, gemm_thread, &parts[t]) != 0) {
            gemm_rows(&parts[t]);
        } else {
            started |= 1 << t;
        }
    }
    for (int t = 0; t < nt; t++) if (started & (1 << t)) pthread_join(th[t], NULL);
}

static SeaValue n_matmul(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) FAIL("ndarray.matmul: expected 2 arrays");
    Tmp t = { 0 };
    ND a, b;
    opnd(vm, args[0], &a, &t, "matmul");
    opnd(vm, args[1], &b, &t, "matmul");
    if (a.nd == 0 || b.nd == 0) FAIL("ndarray.matmul: an operand has no dimensions (use multiply)");
    int dt = promote(a.dt, b.dt);
    if (dt == DT_BOOL) dt = DT_INT;
    bool a1 = a.nd == 1, b1 = b.nd == 1;
    int64_t n = a1 ? 1 : a.shape[a.nd - 2], k = a.shape[a.nd - 1];
    int64_t kb = b1 ? b.shape[0] : b.shape[b.nd - 2], m = b1 ? 1 : b.shape[b.nd - 1];
    if (k != kb) {
        char x[160], y[160];
        FAIL("ndarray.matmul: shapes %s and %s not aligned: %lld != %lld", shape_str(a.shape, a.nd, x, sizeof x),
             shape_str(b.shape, b.nd, y, sizeof y), (long long)k, (long long)kb);
    }
    /* batch dimensions, broadcast */
    ND ba, bb;
    memset(&ba, 0, sizeof ba); memset(&bb, 0, sizeof bb);
    ba.nd = a1 ? 0 : a.nd - 2; bb.nd = b1 ? 0 : b.nd - 2;
    memcpy(ba.shape, a.shape, sizeof(int64_t) * ba.nd);
    memcpy(bb.shape, b.shape, sizeof(int64_t) * bb.nd);
    ND *bops[2] = { &ba, &bb };
    int bnd; int64_t bshp[MAXD];
    if (!bshape(bops, 2, &bnd, bshp)) bfail(vm, "matmul", (ND *[]){ &a, &b }, 2);
    /* contiguous copies in the compute dtype */
    ND ac, bc;
    tmp_hold(&t, copy_as(vm, &a, dt, &ac));
    tmp_hold(&t, copy_as(vm, &b, dt, &bc));
    int64_t oshape[MAXD];
    int ond = 0;
    for (int i = 0; i < bnd; i++) oshape[ond++] = bshp[i];
    if (!a1) oshape[ond++] = n;
    if (!b1) oshape[ond++] = m;
    ND o;
    SeaValue r = nd_new(vm, dt, ond, oshape, &o);
    int64_t nb = prod(bshp, bnd);
    int64_t ast[MAXD], bst[MAXD], idx[MAXD] = { 0 };
    /* each operand's batch strides in matrices (0 where broadcast) */
    for (int d = 0; d < bnd; d++) {
        int da = d - (bnd - ba.nd), db = d - (bnd - bb.nd);
        int64_t sa = 1, sb = 1;
        for (int e = da + 1; e < ba.nd; e++) sa *= ba.shape[e];
        for (int e = db + 1; e < bb.nd; e++) sb *= bb.shape[e];
        ast[d] = (da < 0 || ba.shape[da] == 1) ? 0 : sa;
        bst[d] = (db < 0 || bb.shape[db] == 1) ? 0 : sb;
    }
    for (int64_t q = 0; q < nb; q++) {
        int64_t ia = 0, ib = 0;
        for (int d = 0; d < bnd; d++) { ia += idx[d] * ast[d]; ib += idx[d] * bst[d]; }
        int64_t e = ESZ(dt);
        gemm(dt, n, k, m, ac.p + ia * n * k * e, bc.p + ib * k * m * e, o.p + q * n * m * e);
        for (int d = bnd - 1; d >= 0; d--) { if (++idx[d] < bshp[d]) break; idx[d] = 0; }
    }
    tmp_free(&t);
    return finish(r, &o);
}

/* ---- linear algebra (float64, 2-D) ------------------------------------------------------ */

/* a square or rectangular matrix as a contiguous float64 copy (malloc'd) */
static double *mat_of(SeaVM *vm, SeaValue v, int64_t *rows, int64_t *cols, bool square, const char *fn) {
    Tmp t = { 0 };
    ND a, c;
    opnd(vm, v, &a, &t, fn);
    if (a.nd != 2) FAIL("ndarray.linalg.%s: expected a 2-D array, got %d dimensions", fn, a.nd);
    if (square && a.shape[0] != a.shape[1]) FAIL("ndarray.linalg.%s: the matrix must be square", fn);
    if (a.dt == DT_C128) FAIL("ndarray.linalg.%s: not for complex matrices", fn);
    tmp_hold(&t, copy_as(vm, &a, DT_F64, &c));
    double *m = malloc((size_t)(c.size + 1) * 8);
    memcpy(m, c.p, (size_t)c.size * 8);
    *rows = a.shape[0]; *cols = a.shape[1];
    tmp_free(&t);
    return m;
}

static SeaValue mat_val(SeaVM *vm, const double *m, int64_t r, int64_t c) {
    ND o;
    int64_t s[2] = { r, c };
    SeaValue t = nd_new(vm, DT_F64, 2, s, &o);
    memcpy(o.p, m, (size_t)(r * c) * 8);
    return t;
}
static SeaValue vec_val(SeaVM *vm, const double *v, int64_t n) {
    ND o;
    int64_t s[1] = { n };
    SeaValue t = nd_new(vm, DT_F64, 1, s, &o);
    memcpy(o.p, v, (size_t)n * 8);
    return t;
}

/* LU with partial pivoting in place; piv[i] the row swapped at step i;
 * returns the sign of the permutation, 0 when singular */
static int lu(double *a, int64_t n, int64_t *piv) {
    int sign = 1;
    for (int64_t j = 0; j < n; j++) {
        int64_t p = j;
        double big = fabs(a[j * n + j]);
        for (int64_t i = j + 1; i < n; i++) if (fabs(a[i * n + j]) > big) { big = fabs(a[i * n + j]); p = i; }
        piv[j] = p;
        if (big == 0) return 0;
        if (p != j) {
            for (int64_t c = 0; c < n; c++) { double x = a[j * n + c]; a[j * n + c] = a[p * n + c]; a[p * n + c] = x; }
            sign = -sign;
        }
        double d = a[j * n + j];
        for (int64_t i = j + 1; i < n; i++) {
            double f = a[i * n + j] /= d;
            if (f == 0) continue;
            for (int64_t c = j + 1; c < n; c++) a[i * n + c] -= f * a[j * n + c];
        }
    }
    return sign;
}

/* solve LU x = b for nrhs columns of b (n×nrhs, row-major), in place */
static void lu_solve(const double *a, int64_t n, const int64_t *piv, double *b, int64_t nrhs) {
    for (int64_t i = 0; i < n; i++)
        if (piv[i] != i)
            for (int64_t c = 0; c < nrhs; c++) { double x = b[i * nrhs + c]; b[i * nrhs + c] = b[piv[i] * nrhs + c]; b[piv[i] * nrhs + c] = x; }
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < i; j++) {
            double f = a[i * n + j];
            if (f != 0) for (int64_t c = 0; c < nrhs; c++) b[i * nrhs + c] -= f * b[j * nrhs + c];
        }
    for (int64_t i = n - 1; i >= 0; i--) {
        for (int64_t j = i + 1; j < n; j++) {
            double f = a[i * n + j];
            if (f != 0) for (int64_t c = 0; c < nrhs; c++) b[i * nrhs + c] -= f * b[j * nrhs + c];
        }
        for (int64_t c = 0; c < nrhs; c++) b[i * nrhs + c] /= a[i * n + i];
    }
}

/* ---- complex matrices -------------------------------------------------------------- */

/* the dtype of an array argument */
static int arg_dt(SeaVM *vm, SeaValue v, const char *fn) {
    Tmp t = { 0 };
    ND a;
    opnd(vm, v, &a, &t, fn);
    int dt = a.dt;
    tmp_free(&t);
    return dt;
}

/* a matrix as a contiguous complex128 copy (malloc'd) */
static zcplx *zmat_of(SeaVM *vm, SeaValue v, int64_t *rows, int64_t *cols, bool square, const char *fn) {
    Tmp t = { 0 };
    ND a, c;
    opnd(vm, v, &a, &t, fn);
    if (a.nd != 2) FAIL("ndarray.linalg.%s: expected a 2-D array, got %d dimensions", fn, a.nd);
    if (square && a.shape[0] != a.shape[1]) FAIL("ndarray.linalg.%s: the matrix must be square", fn);
    tmp_hold(&t, copy_as(vm, &a, DT_C128, &c));
    zcplx *m = malloc((size_t)(c.size + 1) * sizeof(zcplx));
    memcpy(m, c.p, (size_t)c.size * sizeof(zcplx));
    *rows = a.shape[0]; *cols = a.shape[1];
    tmp_free(&t);
    return m;
}

static SeaValue zmat_val(SeaVM *vm, const zcplx *m, int nd, int64_t r, int64_t c) {
    ND o;
    int64_t s[2] = { r, c };
    SeaValue t = nd_new(vm, DT_C128, nd, s, &o);
    memcpy(o.p, m, (size_t)(nd == 1 ? r : r * c) * sizeof(zcplx));
    return t;
}

static int zlu(zcplx *a, int64_t n, int64_t *piv) {
    int sign = 1;
    for (int64_t j = 0; j < n; j++) {
        int64_t p = j;
        double big = cabs(a[j * n + j]);
        for (int64_t i = j + 1; i < n; i++) if (cabs(a[i * n + j]) > big) { big = cabs(a[i * n + j]); p = i; }
        piv[j] = p;
        if (big == 0) return 0;
        if (p != j) {
            for (int64_t c = 0; c < n; c++) { zcplx x = a[j * n + c]; a[j * n + c] = a[p * n + c]; a[p * n + c] = x; }
            sign = -sign;
        }
        zcplx d = a[j * n + j];
        for (int64_t i = j + 1; i < n; i++) {
            zcplx f = a[i * n + j] /= d;
            if (f == 0) continue;
            for (int64_t c = j + 1; c < n; c++) a[i * n + c] -= f * a[j * n + c];
        }
    }
    return sign;
}

static void zlu_solve(const zcplx *a, int64_t n, const int64_t *piv, zcplx *b, int64_t nrhs) {
    for (int64_t i = 0; i < n; i++)
        if (piv[i] != i)
            for (int64_t c = 0; c < nrhs; c++) { zcplx x = b[i * nrhs + c]; b[i * nrhs + c] = b[piv[i] * nrhs + c]; b[piv[i] * nrhs + c] = x; }
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < i; j++)
            if (a[i * n + j] != 0) for (int64_t c = 0; c < nrhs; c++) b[i * nrhs + c] -= a[i * n + j] * b[j * nrhs + c];
    for (int64_t i = n - 1; i >= 0; i--) {
        for (int64_t j = i + 1; j < n; j++)
            if (a[i * n + j] != 0) for (int64_t c = 0; c < nrhs; c++) b[i * nrhs + c] -= a[i * n + j] * b[j * nrhs + c];
        for (int64_t c = 0; c < nrhs; c++) b[i * nrhs + c] /= a[i * n + i];
    }
}

static SeaValue z_det(SeaVM *vm, SeaValue m) {
    int64_t n, c;
    zcplx *a = zmat_of(vm, m, &n, &c, true, "det");
    int64_t *piv = malloc((size_t)(n + 1) * 8);
    int s = zlu(a, n, piv);
    zcplx d = s;
    for (int64_t i = 0; i < n && s; i++) d *= a[i * n + i];
    free(a); free(piv);
    return cpx_val(s ? d : 0);
}

static SeaValue z_solve(SeaVM *vm, SeaValue m, SeaValue rhs) {
    int64_t n, c;
    Tmp t = { 0 };
    ND b;
    opnd(vm, rhs, &b, &t, "solve");
    if (b.nd < 1 || b.nd > 2) FAIL("ndarray.linalg.solve: b must be 1-D or 2-D");
    zcplx *a = zmat_of(vm, m, &n, &c, true, "solve");
    if (b.shape[0] != n) { free(a); FAIL("ndarray.linalg.solve: b has %lld rows, a has %lld", (long long)b.shape[0], (long long)n); }
    ND bc;
    SeaValue r = copy_as(vm, &b, DT_C128, &bc);
    int64_t *piv = malloc((size_t)(n + 1) * 8);
    int s = zlu(a, n, piv);
    if (s) zlu_solve(a, n, piv, (zcplx *)bc.p, b.nd == 1 ? 1 : b.shape[1]);
    free(a); free(piv);
    tmp_free(&t);
    if (!s) { sea_obj_release(sea_as_obj(r)); return sea_make_error_str(vm, "ndarray.linalg.solve: singular matrix"); }
    return r;
}

static SeaValue z_inv(SeaVM *vm, SeaValue m) {
    int64_t n, c;
    zcplx *a = zmat_of(vm, m, &n, &c, true, "inv");
    int64_t *piv = malloc((size_t)(n + 1) * 8);
    int s = zlu(a, n, piv);
    if (!s) { free(a); free(piv); return sea_make_error_str(vm, "ndarray.linalg.inv: singular matrix"); }
    ND o;
    int64_t sh[2] = { n, n };
    SeaValue r = nd_new(vm, DT_C128, 2, sh, &o);
    zcplx *x = (zcplx *)o.p;
    for (int64_t i = 0; i < n; i++) x[i * n + i] = 1;
    zlu_solve(a, n, piv, x, n);
    free(a); free(piv);
    return r;
}

/* the Schur form of a (n×n, overwritten by the upper triangular T) and the
 * unitary z with A = Z T Zᴴ: Householder reduction to Hessenberg form, then
 * QR steps with Wilkinson shifts (Givens rotations, deflating at the bottom).
 * false when it does not converge */
static bool zschur(zcplx *a, zcplx *z, int64_t n) {
#define A_(i, j) a[(i) * n + (j)]
#define Z_(i, j) z[(i) * n + (j)]
    for (int64_t i = 0; i < n; i++) for (int64_t j = 0; j < n; j++) Z_(i, j) = i == j;
    double anorm = 0;
    for (int64_t i = 0; i < n * n; i++) if (cabs(a[i]) > anorm) anorm = cabs(a[i]);
    if (anorm == 0) return true;
    zcplx *v = malloc((size_t)(n + 1) * sizeof(zcplx));
    for (int64_t k = 0; k + 2 < n; k++) {
        double alpha = 0;
        for (int64_t i = k + 1; i < n; i++) alpha += creal(A_(i, k) * conj(A_(i, k)));
        alpha = sqrt(alpha);
        if (alpha == 0) continue;
        zcplx x0 = A_(k + 1, k);
        zcplx ph = cabs(x0) > 0 ? x0 / cabs(x0) : 1;
        for (int64_t i = 0; i < n; i++) v[i] = 0;
        for (int64_t i = k + 1; i < n; i++) v[i] = A_(i, k);
        v[k + 1] += ph * alpha;
        double vn = 0;
        for (int64_t i = k + 1; i < n; i++) vn += creal(v[i] * conj(v[i]));
        vn = sqrt(vn);
        if (vn == 0) continue;
        for (int64_t i = k + 1; i < n; i++) v[i] /= vn;
        for (int64_t j = 0; j < n; j++) {          /* A = H A */
            zcplx s = 0;
            for (int64_t i = k + 1; i < n; i++) s += conj(v[i]) * A_(i, j);
            for (int64_t i = k + 1; i < n; i++) A_(i, j) -= 2 * v[i] * s;
        }
        for (int64_t i = 0; i < n; i++) {          /* A = A H, Z = Z H */
            zcplx s = 0, sz = 0;
            for (int64_t j = k + 1; j < n; j++) { s += A_(i, j) * v[j]; sz += Z_(i, j) * v[j]; }
            for (int64_t j = k + 1; j < n; j++) { A_(i, j) -= 2 * s * conj(v[j]); Z_(i, j) -= 2 * sz * conj(v[j]); }
        }
        for (int64_t i = k + 2; i < n; i++) A_(i, k) = 0;
    }
    free(v);
    zcplx *cs = malloc((size_t)(n + 1) * sizeof(zcplx)), *sn = malloc((size_t)(n + 1) * sizeof(zcplx));
    int64_t hi = n - 1, iter = 0, total = 0;
    bool ok = true;
    while (hi > 0) {
        int64_t l = hi;
        while (l > 0) {
            double s = cabs(A_(l - 1, l - 1)) + cabs(A_(l, l));
            if (s == 0) s = anorm;
            if (cabs(A_(l, l - 1)) <= DBL_EPSILON * s) { A_(l, l - 1) = 0; break; }
            l--;
        }
        if (l == hi) { hi--; iter = 0; continue; }
        if (++total > 60 * n + 100) { ok = false; break; }
        iter++;
        zcplx mu;
        if (iter % 11 == 0) {
            mu = A_(hi, hi) + 0.75 * cabs(A_(hi, hi - 1));   /* an exceptional shift */
        } else {
            zcplx p = A_(hi - 1, hi - 1), q = A_(hi - 1, hi), r = A_(hi, hi - 1), d = A_(hi, hi);
            zcplx half = (p + d) / 2, disc = csqrt(half * half - (p * d - q * r));
            zcplx m1 = half + disc, m2 = half - disc;
            mu = cabs(m1 - d) < cabs(m2 - d) ? m1 : m2;
        }
        for (int64_t k = l; k <= hi; k++) A_(k, k) -= mu;
        for (int64_t k = l; k < hi; k++) {
            zcplx x = A_(k, k), y = A_(k + 1, k);
            double r = hypot(cabs(x), cabs(y));
            zcplx c = 1, s = 0;
            if (r != 0) { c = x / r; s = y / r; }
            cs[k] = c; sn[k] = s;
            for (int64_t j = k; j < n; j++) {
                zcplx p = A_(k, j), q = A_(k + 1, j);
                A_(k, j) = conj(c) * p + conj(s) * q;
                A_(k + 1, j) = -s * p + c * q;
            }
        }
        for (int64_t k = l; k < hi; k++) {
            zcplx c = cs[k], s = sn[k];
            for (int64_t i = 0; i <= k + 1; i++) {
                zcplx p = A_(i, k), q = A_(i, k + 1);
                A_(i, k) = p * c + q * s;
                A_(i, k + 1) = -p * conj(s) + q * conj(c);
            }
            for (int64_t i = 0; i < n; i++) {
                zcplx p = Z_(i, k), q = Z_(i, k + 1);
                Z_(i, k) = p * c + q * s;
                Z_(i, k + 1) = -p * conj(s) + q * conj(c);
            }
        }
        for (int64_t k = l; k <= hi; k++) A_(k, k) += mu;
    }
    free(cs); free(sn);
    return ok;
#undef A_
#undef Z_
}

/* eig(a [, vectors]) → [w, v] (or [w]): the eigenvalues and right
 * eigenvectors (unit columns, the largest component real) of a square
 * matrix; float64 when a is real and every eigenvalue is real */
static SeaValue n_eig(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.linalg.eig: expected a matrix");
    bool vectors = argc < 2 || sea_is_truthy(args[1]);
    bool real_in = arg_dt(vm, args[0], "eig") != DT_C128;
    int64_t n, c;
    zcplx *a = zmat_of(vm, args[0], &n, &c, true, "eig");
    double anorm = 0;
    for (int64_t i = 0; i < n * n; i++) if (cabs(a[i]) > anorm) anorm = cabs(a[i]);
    zcplx *z = malloc((size_t)(n * n + 1) * sizeof(zcplx));
    if (!zschur(a, z, n)) {
        free(a); free(z);
        return sea_make_error_str(vm, "ndarray.linalg.eig: the eigenvalues did not converge");
    }
    zcplx *w = malloc((size_t)(n + 1) * sizeof(zcplx));
    double tol = 64 * DBL_EPSILON * (anorm > 0 ? anorm : 1);
    bool all_real = real_in;
    for (int64_t k = 0; k < n; k++) {
        w[k] = a[k * n + k];
        if (real_in && fabs(cimag(w[k])) <= tol) w[k] = creal(w[k]);
        if (cimag(w[k]) != 0) all_real = false;
    }
    zcplx *vecs = NULL;
    if (vectors) {
        /* T y = λk y by back substitution, then v = Z y */
        vecs = calloc((size_t)(n * n + 1), sizeof(zcplx));
        zcplx *y = malloc((size_t)(n + 1) * sizeof(zcplx));
        double small = DBL_EPSILON * (anorm > 0 ? anorm : 1);
        for (int64_t k = 0; k < n; k++) {
            zcplx lam = a[k * n + k];
            for (int64_t i = 0; i < n; i++) y[i] = 0;
            y[k] = 1;
            for (int64_t i = k - 1; i >= 0; i--) {
                zcplx s = 0;
                for (int64_t j = i + 1; j <= k; j++) s += a[i * n + j] * y[j];
                zcplx d = a[i * n + i] - lam;
                if (cabs(d) < small) d = small;
                y[i] = -s / d;
            }
            double nn = 0;
            int64_t big = 0;
            for (int64_t i = 0; i < n; i++) {
                zcplx s = 0;
                for (int64_t j = 0; j <= k; j++) s += z[i * n + j] * y[j];
                vecs[i * n + k] = s;
                nn += creal(s * conj(s));
                if (cabs(s) > cabs(vecs[big * n + k])) big = i;
            }
            nn = sqrt(nn);
            zcplx ph = cabs(vecs[big * n + k]) > 0 ? conj(vecs[big * n + k]) / cabs(vecs[big * n + k]) : 1;
            for (int64_t i = 0; i < n; i++) {
                zcplx e = vecs[i * n + k] * ph / (nn > 0 ? nn : 1);
                /* (+ 0.0 turns -0 into 0) */
                vecs[i * n + k] = CMPLX(creal(e) + 0.0, real_in && cimag(w[k]) == 0 ? 0.0 : cimag(e) + 0.0);
            }
        }
        free(y);
    }
    SeaArray *out = sea_array_new(2);
    if (all_real) {
        double *wr = malloc((size_t)(n + 1) * 8);
        for (int64_t k = 0; k < n; k++) wr[k] = creal(w[k]);
        sea_array_push_move(out, vec_val(vm, wr, n));
        free(wr);
        if (vectors) {
            double *vr = malloc((size_t)(n * n + 1) * 8);
            for (int64_t i = 0; i < n * n; i++) vr[i] = creal(vecs[i]);
            sea_array_push_move(out, mat_val(vm, vr, n, n));
            free(vr);
        }
    } else {
        sea_array_push_move(out, zmat_val(vm, w, 1, n, 1));
        if (vectors) sea_array_push_move(out, zmat_val(vm, vecs, 2, n, n));
    }
    free(a); free(z); free(w); free(vecs);
    return sea_obj_val(&out->obj);
}

static SeaValue n_det(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.linalg.det: expected a matrix");
    if (arg_dt(vm, args[0], "det") == DT_C128) return z_det(vm, args[0]);
    int64_t n, c;
    double *a = mat_of(vm, args[0], &n, &c, true, "det");
    int64_t *piv = malloc((size_t)(n + 1) * 8);
    int s = lu(a, n, piv);
    double d = s;
    for (int64_t i = 0; i < n && s; i++) d *= a[i * n + i];
    free(a); free(piv);
    return sea_number_val(s ? d : 0.0);
}

static SeaValue n_solve(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) FAIL("ndarray.linalg.solve: expected (a, b)");
    if (arg_dt(vm, args[0], "solve") == DT_C128 || arg_dt(vm, args[1], "solve") == DT_C128)
        return z_solve(vm, args[0], args[1]);
    int64_t n, c;
    Tmp t = { 0 };
    ND b, bc;
    opnd(vm, args[1], &b, &t, "solve");
    if (b.nd < 1 || b.nd > 2) FAIL("ndarray.linalg.solve: b must be 1-D or 2-D");
    double *a = mat_of(vm, args[0], &n, &c, true, "solve");
    if (b.shape[0] != n) { free(a); FAIL("ndarray.linalg.solve: b has %lld rows, a has %lld", (long long)b.shape[0], (long long)n); }
    SeaValue r = copy_as(vm, &b, DT_F64, &bc);
    int64_t *piv = malloc((size_t)(n + 1) * 8);
    int s = lu(a, n, piv);
    if (s) lu_solve(a, n, piv, (double *)bc.p, b.nd == 1 ? 1 : b.shape[1]);
    free(a); free(piv);
    tmp_free(&t);
    if (!s) { sea_obj_release(sea_as_obj(r)); return sea_make_error_str(vm, "ndarray.linalg.solve: singular matrix"); }
    return r;
}

static SeaValue n_inv(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.linalg.inv: expected a matrix");
    if (arg_dt(vm, args[0], "inv") == DT_C128) return z_inv(vm, args[0]);
    int64_t n, c;
    double *a = mat_of(vm, args[0], &n, &c, true, "inv");
    int64_t *piv = malloc((size_t)(n + 1) * 8);
    int s = lu(a, n, piv);
    if (!s) { free(a); free(piv); return sea_make_error_str(vm, "ndarray.linalg.inv: singular matrix"); }
    ND o;
    int64_t sh[2] = { n, n };
    SeaValue r = nd_new(vm, DT_F64, 2, sh, &o);
    double *x = (double *)o.p;
    for (int64_t i = 0; i < n; i++) x[i * n + i] = 1;
    lu_solve(a, n, piv, x, n);
    free(a); free(piv);
    return r;
}

static SeaValue n_cholesky(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.linalg.cholesky: expected a matrix");
    int64_t n, c;
    double *a = mat_of(vm, args[0], &n, &c, true, "cholesky");
    double *l = calloc((size_t)(n * n + 1), 8);
    for (int64_t j = 0; j < n; j++) {
        double s = a[j * n + j];
        for (int64_t p = 0; p < j; p++) s -= l[j * n + p] * l[j * n + p];
        if (!(s > 0)) { free(a); free(l); return sea_make_error_str(vm, "ndarray.linalg.cholesky: the matrix is not positive definite"); }
        double d = sqrt(s);
        l[j * n + j] = d;
        for (int64_t i = j + 1; i < n; i++) {
            double x = a[i * n + j];
            for (int64_t p = 0; p < j; p++) x -= l[i * n + p] * l[j * n + p];
            l[i * n + j] = x / d;
        }
    }
    SeaValue r = mat_val(vm, l, n, n);
    free(a); free(l);
    return r;
}

/* Householder QR of a (m×n): q (m×qc), r (rr×n); mode: 0 reduced, 1 complete */
static SeaValue n_qr(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.linalg.qr: expected a matrix");
    bool complete = argc > 1 && str_eq(args[1], "complete");
    int64_t m, n;
    double *a = mat_of(vm, args[0], &m, &n, false, "qr");
    int64_t kk = m < n ? m : n;
    double *v = calloc((size_t)(kk * m + 1), 8);
    for (int64_t j = 0; j < kk; j++) {
        double norm = 0;
        for (int64_t i = j; i < m; i++) norm += a[i * n + j] * a[i * n + j];
        norm = sqrt(norm);
        double *vj = v + j * m;
        if (norm == 0) continue;
        double alpha = a[j * n + j] > 0 ? -norm : norm;
        for (int64_t i = j; i < m; i++) vj[i] = a[i * n + j];
        vj[j] -= alpha;
        double vn = 0;
        for (int64_t i = j; i < m; i++) vn += vj[i] * vj[i];
        vn = sqrt(vn);
        if (vn == 0) continue;
        for (int64_t i = j; i < m; i++) vj[i] /= vn;
        for (int64_t c = j; c < n; c++) {
            double s = 0;
            for (int64_t i = j; i < m; i++) s += vj[i] * a[i * n + c];
            for (int64_t i = j; i < m; i++) a[i * n + c] -= 2 * s * vj[i];
        }
    }
    int64_t qc = complete ? m : kk;
    double *q = calloc((size_t)(m * qc + 1), 8);
    for (int64_t i = 0; i < qc && i < m; i++) q[i * qc + i] = 1;
    for (int64_t j = kk - 1; j >= 0; j--) {
        double *vj = v + j * m;
        for (int64_t c = 0; c < qc; c++) {
            double s = 0;
            for (int64_t i = j; i < m; i++) s += vj[i] * q[i * qc + c];
            if (s != 0) for (int64_t i = j; i < m; i++) q[i * qc + c] -= 2 * s * vj[i];
        }
    }
    int64_t rr = complete ? m : kk;
    double *r = calloc((size_t)(rr * n + 1), 8);
    for (int64_t i = 0; i < rr && i < m; i++)
        for (int64_t c = i; c < n; c++) r[i * n + c] = a[i * n + c];
    SeaArray *out = sea_array_new(2);
    sea_array_push_move(out, mat_val(vm, q, m, qc));
    sea_array_push_move(out, mat_val(vm, r, rr, n));
    free(a); free(v); free(q); free(r);
    return sea_obj_val(&out->obj);
}

/* symmetric eigen-decomposition by cyclic Jacobi → [w (ascending), V (columns)] */
static SeaValue n_eigh(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.linalg.eigh: expected a matrix");
    int64_t n, c;
    double *a = mat_of(vm, args[0], &n, &c, true, "eigh");
    /* use the lower triangle, as NumPy does */
    for (int64_t i = 0; i < n; i++) for (int64_t j = i + 1; j < n; j++) a[i * n + j] = a[j * n + i];
    double *V = calloc((size_t)(n * n + 1), 8);
    for (int64_t i = 0; i < n; i++) V[i * n + i] = 1;
    for (int sweep = 0; sweep < 100; sweep++) {
        double off = 0, diag = 0;
        for (int64_t i = 0; i < n; i++) for (int64_t j = 0; j < n; j++)
            if (i != j) off += a[i * n + j] * a[i * n + j]; else diag += a[i * n + i] * a[i * n + i];
        if (off <= 1e-30 * (diag > 0 ? diag : 1)) break;
        for (int64_t p = 0; p < n; p++)
            for (int64_t q = p + 1; q < n; q++) {
                double apq = a[p * n + q];
                if (fabs(apq) < 1e-300) continue;
                double app = a[p * n + p], aqq = a[q * n + q];
                double theta = (aqq - app) / (2 * apq);
                double tt = (theta >= 0 ? 1 : -1) / (fabs(theta) + sqrt(theta * theta + 1));
                double cs = 1 / sqrt(tt * tt + 1), sn = tt * cs;
                for (int64_t k = 0; k < n; k++) {
                    double akp = a[k * n + p], akq = a[k * n + q];
                    a[k * n + p] = cs * akp - sn * akq;
                    a[k * n + q] = sn * akp + cs * akq;
                }
                for (int64_t k = 0; k < n; k++) {
                    double apk = a[p * n + k], aqk = a[q * n + k];
                    a[p * n + k] = cs * apk - sn * aqk;
                    a[q * n + k] = sn * apk + cs * aqk;
                }
                for (int64_t k = 0; k < n; k++) {
                    double vkp = V[k * n + p], vkq = V[k * n + q];
                    V[k * n + p] = cs * vkp - sn * vkq;
                    V[k * n + q] = sn * vkp + cs * vkq;
                }
            }
    }
    /* sort ascending */
    double *w = malloc((size_t)(n + 1) * 8), *W = malloc((size_t)(n * n + 1) * 8);
    int64_t *ord = malloc((size_t)(n + 1) * 8);
    for (int64_t i = 0; i < n; i++) { w[i] = a[i * n + i]; ord[i] = i; }
    for (int64_t i = 1; i < n; i++) {
        int64_t o = ord[i], j = i;
        while (j > 0 && w[ord[j - 1]] > w[o]) { ord[j] = ord[j - 1]; j--; }
        ord[j] = o;
    }
    double *ws = malloc((size_t)(n + 1) * 8);
    for (int64_t j = 0; j < n; j++) {
        ws[j] = w[ord[j]];
        /* a sign convention: the largest component of each vector positive */
        int64_t big = 0;
        for (int64_t i = 1; i < n; i++) if (fabs(V[i * n + ord[j]]) > fabs(V[big * n + ord[j]])) big = i;
        double sg = V[big * n + ord[j]] < 0 ? -1 : 1;
        for (int64_t i = 0; i < n; i++) W[i * n + j] = sg * V[i * n + ord[j]];
    }
    SeaArray *out = sea_array_new(2);
    sea_array_push_move(out, vec_val(vm, ws, n));
    sea_array_push_move(out, mat_val(vm, W, n, n));
    free(a); free(V); free(w); free(W); free(ord); free(ws);
    return sea_obj_val(&out->obj);
}

/* complete the k orthonormal columns of q (m×k, stored as k rows of m: qt)
 * to m: Gram-Schmidt against the unit vectors */
static void complete_basis(double *qt, int64_t m, int64_t k) {
    int64_t have = k;
    for (int64_t e = 0; e < m && have < m; e++) {
        double *v = qt + have * m;
        for (int64_t i = 0; i < m; i++) v[i] = i == e ? 1 : 0;
        for (int pass = 0; pass < 2; pass++)
            for (int64_t j = 0; j < have; j++) {
                double d = 0;
                for (int64_t i = 0; i < m; i++) d += v[i] * qt[j * m + i];
                for (int64_t i = 0; i < m; i++) v[i] -= d * qt[j * m + i];
            }
        double nn = 0;
        for (int64_t i = 0; i < m; i++) nn += v[i] * v[i];
        nn = sqrt(nn);
        if (nn < 1e-10) continue;
        for (int64_t i = 0; i < m; i++) v[i] /= nn;
        have++;
    }
}

/* SVD by one-sided Jacobi → [U, S, Vt] (full_matrices when args[1]) */
static SeaValue n_svd(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.linalg.svd: expected a matrix");
    bool full = argc > 1 && sea_is_truthy(args[1]);
    int64_t m0, n0;
    double *a = mat_of(vm, args[0], &m0, &n0, false, "svd");
    bool tr = m0 < n0;
    int64_t m = tr ? n0 : m0, n = tr ? m0 : n0;   /* m >= n */
    /* columns of the (m×n) matrix as rows: ut[j*m + i] */
    double *ut = calloc((size_t)(m * m + 1), 8);
    for (int64_t i = 0; i < m0; i++)
        for (int64_t j = 0; j < n0; j++) {
            if (tr) ut[i * m + j] = a[i * n0 + j];   /* column i of aᵀ is row i of a */
            else ut[j * m + i] = a[i * n0 + j];
        }
    double *vt = calloc((size_t)(n * n + 1), 8);
    for (int64_t i = 0; i < n; i++) vt[i * n + i] = 1;
    for (int sweep = 0; sweep < 80; sweep++) {
        double rot = 0;
        for (int64_t p = 0; p < n; p++)
            for (int64_t q = p + 1; q < n; q++) {
                double al = 0, be = 0, ga = 0;
                double *up = ut + p * m, *uq = ut + q * m;
                for (int64_t i = 0; i < m; i++) { al += up[i] * up[i]; be += uq[i] * uq[i]; ga += up[i] * uq[i]; }
                if (fabs(ga) <= 1e-15 * sqrt(al * be) || ga == 0) continue;
                rot += 1;
                double zeta = (be - al) / (2 * ga);
                double t = (zeta >= 0 ? 1 : -1) / (fabs(zeta) + sqrt(1 + zeta * zeta));
                double cs = 1 / sqrt(1 + t * t), sn = cs * t;
                for (int64_t i = 0; i < m; i++) { double x = up[i], y = uq[i]; up[i] = cs * x - sn * y; uq[i] = sn * x + cs * y; }
                double *vp = vt + p * n, *vq = vt + q * n;
                for (int64_t i = 0; i < n; i++) { double x = vp[i], y = vq[i]; vp[i] = cs * x - sn * y; vq[i] = sn * x + cs * y; }
            }
        if (rot == 0) break;
    }
    double *s = malloc((size_t)(n + 1) * 8);
    int64_t *ord = malloc((size_t)(n + 1) * 8);
    for (int64_t j = 0; j < n; j++) {
        double nn = 0;
        for (int64_t i = 0; i < m; i++) nn += ut[j * m + i] * ut[j * m + i];
        s[j] = sqrt(nn);
        ord[j] = j;
    }
    for (int64_t i = 1; i < n; i++) {
        int64_t o = ord[i], j = i;
        while (j > 0 && s[ord[j - 1]] < s[o]) { ord[j] = ord[j - 1]; j--; }
        ord[j] = o;
    }
    /* sorted: U columns (as rows of us), singular values, V rows (as vs) */
    double *us = calloc((size_t)(m * m + 1), 8), *ss = malloc((size_t)(n + 1) * 8), *vs = malloc((size_t)(n * n + 1) * 8);
    int64_t good = 0;
    double smax = n > 0 ? s[ord[0]] : 0;
    for (int64_t j = 0; j < n; j++) {
        int64_t o = ord[j];
        ss[j] = s[o];
        memcpy(vs + j * n, vt + o * n, (size_t)n * 8);
        if (s[o] > smax * 1e-14 && s[o] > 0) {
            for (int64_t i = 0; i < m; i++) us[j * m + i] = ut[o * m + i] / s[o];
            good = j + 1;
        }
    }
    /* columns for zero singular values, and the rest for full matrices */
    complete_basis(us, m, good);
    int64_t uc = full ? m : n;
    /* the result for the original (m0×n0) matrix */
    SeaValue U, Vt;
    if (!tr) {
        double *Um = malloc((size_t)(m * uc + 1) * 8);
        for (int64_t i = 0; i < m; i++) for (int64_t j = 0; j < uc; j++) Um[i * uc + j] = us[j * m + i];
        U = mat_val(vm, Um, m, uc);
        free(Um);
        /* Vt rows = V columns: vs rows are the V columns already */
        Vt = mat_val(vm, vs, n, n);
    } else {
        /* a = (aᵀ)ᵀ = (U S Vᵀ)ᵀ = V S Uᵀ: U of a is V, Vt of a is Uᵀ */
        double *Um = malloc((size_t)(n * n + 1) * 8);
        for (int64_t i = 0; i < n; i++) for (int64_t j = 0; j < n; j++) Um[i * n + j] = vs[j * n + i];
        U = mat_val(vm, Um, n, n);
        free(Um);
        Vt = mat_val(vm, us, uc, m);
    }
    SeaArray *out = sea_array_new(3);
    sea_array_push_move(out, U);
    sea_array_push_move(out, vec_val(vm, ss, n));
    sea_array_push_move(out, Vt);
    free(a); free(ut); free(vt); free(s); free(ord); free(us); free(ss); free(vs);
    return sea_obj_val(&out->obj);
}

/* ---- random numbers (xoshiro256**) ---------------------------------------------------- */

static uint64_t *rng_state(SeaVM *vm, SeaValue v) {
    if (!sea_is_int_array(v) || SEA_AS_INT_ARRAY(v)->len < 4) FAIL("ndarray.random: bad generator state");
    return (uint64_t *)SEA_AS_INT_ARRAY(v)->data;
}
static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
static inline uint64_t next64(uint64_t *s) {
    uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t; s[3] = rotl(s[3], 45);
    return r;
}
static inline double next_double(uint64_t *s) { return (double)(next64(s) >> 11) * 0x1.0p-53; }
static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
/* uniform in [0, n) without bias (Lemire) */
static uint64_t bounded(uint64_t *s, uint64_t n) {
    if (n == 0) return 0;
    __uint128_t m = (__uint128_t)next64(s) * n;
    uint64_t l = (uint64_t)m;
    if (l < n) {
        uint64_t t = (0 - n) % n;
        while (l < t) { m = (__uint128_t)next64(s) * n; l = (uint64_t)m; }
    }
    return (uint64_t)(m >> 64);
}
static double gauss(uint64_t *s) {
    double u, v, q;
    do { u = 2 * next_double(s) - 1; v = 2 * next_double(s) - 1; q = u * u + v * v; } while (q >= 1 || q == 0);
    return u * sqrt(-2 * log(q) / q);
}
static int64_t poisson(uint64_t *s, double lam) {
    if (lam <= 0) return 0;
    if (lam < 30) {
        double L = exp(-lam), p = 1;
        int64_t k = 0;
        do { k++; p *= next_double(s); } while (p > L);
        return k - 1;
    }
    /* PTRS (Hörmann 1993) */
    double slam = sqrt(lam), loglam = log(lam);
    double b = 0.931 + 2.53 * slam, a = -0.059 + 0.02483 * b, invalpha = 1.1239 + 1.1328 / (b - 3.4), vr = 0.9277 - 3.6224 / (b - 2);
    for (;;) {
        double U = next_double(s) - 0.5, V = next_double(s);
        double us = 0.5 - fabs(U);
        int64_t k = (int64_t)floor((2 * a / us + b) * U + lam + 0.43);
        if (us >= 0.07 && V <= vr) return k;
        if (k < 0 || (us < 0.013 && V > us)) continue;
        if (log(V) + log(invalpha) - log(a / (us * us) + b) <= -lam + k * loglam - lgamma(k + 1)) return k;
    }
}

/* seed(state, n): n an int, or nil for the system's entropy */
static SeaValue n_rng_seed(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.random.seed: expected a state");
    uint64_t *s = rng_state(vm, args[0]);
    uint64_t x;
    if (argc > 1 && is_num(args[1])) x = (uint64_t)iv(args[1]);
    else {
        x = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^ (uint64_t)(uintptr_t)s;
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) { if (fread(&x, 8, 1, f) != 1) x ^= 0x5eed; fclose(f); }
    }
    for (int i = 0; i < 4; i++) s[i] = splitmix(&x);
    return SEA_NIL_VAL;
}

enum { RD_RANDOM, RD_UNIFORM, RD_NORMAL, RD_INTEGERS, RD_EXPONENTIAL, RD_POISSON };

/* draw(state, kind, shape, p1, p2) */
static SeaValue n_rng_draw(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 3) FAIL("ndarray.random: expected (state, kind, shape, ...)");
    uint64_t *s = rng_state(vm, args[0]);
    int kind = (int)iv(args[1]);
    int64_t shape[MAXD];
    int nd = shape_arg(vm, args[2], shape, "random");
    double p1 = argc > 3 && !sea_is_nil(args[3]) ? dv(args[3]) : 0;
    double p2 = argc > 4 && !sea_is_nil(args[4]) ? dv(args[4]) : 1;
    int dt = kind == RD_INTEGERS || kind == RD_POISSON ? DT_INT : DT_F64;
    if (dt == DT_F64 && argc > 5 && !sea_is_nil(args[5])) {
        dt = dt_parse(vm, args[5], "random");
        if (!is_flt(dt)) FAIL("ndarray.random: dtype must be float64 or float32");
    }
    int64_t lo = 0, span = 0;
    if (kind == RD_INTEGERS) {
        lo = iv(args[3]);
        int64_t hi = iv(args[4]);
        if (hi <= lo) FAIL("ndarray.random.integers: high must be greater than low");
        span = hi - lo;
    }
    ND o;
    SeaValue r = nd_new(vm, dt, nd, shape, &o);
    int e = ESZ(dt);
    for (int64_t i = 0; i < o.size; i++) {
        char *q = o.p + i * e;
        switch (kind) {
        case RD_RANDOM: store_d(dt, q, next_double(s)); break;
        case RD_UNIFORM: store_d(dt, q, p1 + (p2 - p1) * next_double(s)); break;
        case RD_NORMAL: store_d(dt, q, p1 + p2 * gauss(s)); break;
        case RD_INTEGERS: *(int64_t *)q = lo + (int64_t)bounded(s, (uint64_t)span); break;
        case RD_EXPONENTIAL: store_d(dt, q, -p1 * log1p(-next_double(s))); break;
        default: *(int64_t *)q = poisson(s, p1); break;
        }
    }
    return finish(r, &o);
}

/* shuffle(state, a): in place along the first axis */
static SeaValue n_rng_shuffle(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) FAIL("ndarray.random.shuffle: expected (state, a)");
    uint64_t *s = rng_state(vm, args[0]);
    if (!is_nd(args[1])) FAIL("ndarray.random.shuffle: expected an ndarray");
    ND a;
    nd_read(vm, args[1], &a, "shuffle");
    if (a.nd == 0) return SEA_NIL_VAL;
    int64_t n = a.shape[0];
    ND row;
    row = a;
    row.nd = a.nd - 1;
    memmove(row.shape, a.shape + 1, sizeof(int64_t) * row.nd);
    memmove(row.strides, a.strides + 1, sizeof(int64_t) * row.nd);
    int64_t rs = prod(row.shape, row.nd);
    char *tmp = malloc((size_t)(rs + 1) * 8);
    for (int64_t i = n - 1; i > 0; i--) {
        int64_t j = (int64_t)bounded(s, (uint64_t)(i + 1));
        if (i == j) continue;
        ND ri = row, rj = row, tb;
        ri.p = a.p + i * a.strides[0] * ESZ(a.dt);
        rj.p = a.p + j * a.strides[0] * ESZ(a.dt);
        memset(&tb, 0, sizeof tb);
        tb.dt = a.dt; tb.nd = row.nd; tb.p = tmp;
        memcpy(tb.shape, row.shape, sizeof(int64_t) * row.nd);
        contig_strides(row.nd, row.shape, tb.strides);
        Cast c = { a.dt, a.dt };
        ND *o1[2] = { &tb, &ri }; run(o1, 2, row.nd, row.shape, k_cast, &c);
        ND *o2[2] = { &ri, &rj }; run(o2, 2, row.nd, row.shape, k_cast, &c);
        ND *o3[2] = { &rj, &tb }; run(o3, 2, row.nd, row.shape, k_cast, &c);
    }
    free(tmp);
    return SEA_NIL_VAL;
}

/* ---- creation --------------------------------------------------------------------------- */

static SeaValue n_array(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.array: expected a value");
    int dt = argc > 1 ? dt_parse(vm, args[1], "array") : -1;
    if (is_nd(args[0])) {
        ND a, o;
        nd_read(vm, args[0], &a, "array");
        return copy_as(vm, &a, dt < 0 ? a.dt : dt, &o);
    }
    if (is_num(args[0]) || sea_is_bool(args[0])) {
        ND o;
        int d = dt >= 0 ? dt : sea_is_number(args[0]) ? DT_F64 : sea_is_int(args[0]) ? DT_INT : DT_BOOL;
        SeaValue t = nd_new(vm, d, 0, NULL, &o);
        if (sea_is_number(args[0])) store_d(d, o.p, sea_as_number(args[0]));
        else store_i(d, o.p, sea_is_int(args[0]) ? sea_as_int(args[0]) : sea_as_bool(args[0]));
        return t;
    }
    return from_value(vm, args[0], dt, "array");
}

/* full(shape, value, dtype) */
static SeaValue n_full(SeaVM *vm, int argc, SeaValue *args) {
    int64_t shape[MAXD];
    int nd = shape_arg(vm, argc > 0 ? args[0] : SEA_NIL_VAL, shape, "full");
    SeaValue v = argc > 1 ? args[1] : sea_int_val(0);
    int dt = argc > 2 ? dt_parse(vm, args[2], "full") : -1;
    zcplx z;
    bool cpx = cpx_of(v, &z);
    if (dt < 0) dt = cpx ? DT_C128 : sea_is_number(v) ? DT_F64 : sea_is_bool(v) ? DT_BOOL : DT_INT;
    if (!is_num(v) && !sea_is_bool(v) && !cpx) FAIL("ndarray.full: the value must be a number, a bool or a complex {re, im}");
    if (!cpx) z = dv(v);
    ND o;
    SeaValue t = nd_new(vm, dt, nd, shape, &o);
    if (cpx && dt != DT_C128) { v = sea_number_val(creal(z)); }
    if (dt == DT_C128) { for (int64_t i = 0; i < o.size; i++) ((zcplx *)o.p)[i] = z; }
    else if (dt == DT_F64) { double x = dv(v); for (int64_t i = 0; i < o.size; i++) ((double *)o.p)[i] = x; }
    else if (dt == DT_F32) { float x = (float)dv(v); for (int64_t i = 0; i < o.size; i++) ((float *)o.p)[i] = x; }
    else {
        int64_t x = sea_is_int(v) ? sea_as_int(v) : sea_is_bool(v) ? sea_as_bool(v) : (int64_t)dv(v);
        if (dt == DT_BOOL) x = x != 0 || (sea_is_number(v) && dv(v) != 0);
        if (x) for (int64_t i = 0; i < o.size; i++) ((int64_t *)o.p)[i] = x;
    }
    return t;
}

/* arange(start, stop, step, dtype) — all given by ndarray.sea */
static SeaValue n_arange(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 3) FAIL("ndarray.arange: expected (start, stop, step)");
    int dt = argc > 3 ? dt_parse(vm, args[3], "arange") : -1;
    bool ints = sea_is_int(args[0]) && sea_is_int(args[1]) && sea_is_int(args[2]);
    if (dt < 0) dt = ints ? DT_INT : DT_F64;
    double a = dv(args[0]), b = dv(args[1]), st = dv(args[2]);
    if (st == 0) FAIL("ndarray.arange: step must not be zero");
    int64_t n = (int64_t)ceil((b - a) / st);
    if (ints) {
        int64_t ia = sea_as_int(args[0]), ib = sea_as_int(args[1]), is = sea_as_int(args[2]);
        n = is > 0 ? (ib - ia + is - 1) / is : (ia - ib - is - 1) / -is;
    }
    if (n < 0) n = 0;
    ND o;
    int64_t sh[1] = { n };
    SeaValue t = nd_new(vm, dt, 1, sh, &o);
    if (dt >= DT_F32) for (int64_t i = 0; i < n; i++) store_d(dt, o.p + i * ESZ(dt), a + (double)i * st);
    else if (ints) {
        int64_t ia = sea_as_int(args[0]), is = sea_as_int(args[2]);
        for (int64_t i = 0; i < n; i++) ((int64_t *)o.p)[i] = ia + i * is;
    } else for (int64_t i = 0; i < n; i++) ((int64_t *)o.p)[i] = (int64_t)(a + (double)i * st);
    return t;
}

/* linspace(start, stop, num, endpoint) */
static SeaValue n_linspace(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 3) FAIL("ndarray.linspace: expected (start, stop, num)");
    double a = dv(args[0]), b = dv(args[1]);
    int64_t n = iv(args[2]);
    bool endpoint = argc < 4 || sea_is_nil(args[3]) || sea_is_truthy(args[3]);
    int dt = argc > 4 ? dt_parse(vm, args[4], "linspace") : -1;
    if (dt < 0) dt = DT_F64;
    if (n < 0) FAIL("ndarray.linspace: num must not be negative");
    ND o;
    int64_t sh[1] = { n };
    SeaValue t = nd_new(vm, dt, 1, sh, &o);
    double div = endpoint ? (double)(n - 1) : (double)n;
    double step = div > 0 ? (b - a) / div : 0;
    int e = ESZ(dt);
    for (int64_t i = 0; i < n; i++) store_d(dt, o.p + i * e, a + (double)i * step);
    if (endpoint && n > 1) store_d(dt, o.p + (n - 1) * e, b);
    return t;
}

/* eye(n, m, k, dtype) */
static SeaValue n_eye(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.eye: expected n");
    int64_t n = iv(args[0]);
    int64_t m = argc > 1 && !sea_is_nil(args[1]) ? iv(args[1]) : n;
    int64_t k = argc > 2 && !sea_is_nil(args[2]) ? iv(args[2]) : 0;
    int dt = argc > 3 ? dt_parse(vm, args[3], "eye") : -1;
    if (dt < 0) dt = DT_F64;
    if (n < 0 || m < 0) FAIL("ndarray.eye: negative dimensions are not allowed");
    ND o;
    int64_t sh[2] = { n, m };
    SeaValue t = nd_new(vm, dt, 2, sh, &o);
    for (int64_t i = 0; i < n; i++) {
        int64_t j = i + k;
        if (j < 0 || j >= m) continue;
        store_i(dt, o.p + (i * m + j) * ESZ(dt), 1);
    }
    return t;
}

/* from_data(data, shape, dtype): an array over a float_array or int_array, not copied */
static SeaValue n_from_data(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !(sea_is_float_array(args[0]) || sea_is_int_array(args[0])))
        FAIL("ndarray.from_data: expected a float_array or an int_array");
    int dt = sea_is_float_array(args[0]) ? DT_F64 : DT_INT;
    if (argc > 2 && !sea_is_nil(args[2])) {
        int d = dt_parse(vm, args[2], "from_data");
        if (d == DT_F32) FAIL("ndarray.from_data: float32 data is not a float_array (use astype)");
        if ((d == DT_F64) != (dt == DT_F64)) FAIL("ndarray.from_data: that dtype needs the other kind of data");
        dt = d;
    }
    uint32_t len = dt == DT_F64 ? SEA_AS_FLOAT_ARRAY(args[0])->len : SEA_AS_INT_ARRAY(args[0])->len;
    int64_t shape[MAXD], st[MAXD];
    int nd = 1;
    shape[0] = len;
    if (argc > 1 && !sea_is_nil(args[1])) nd = shape_arg(vm, args[1], shape, "from_data");
    if (prod(shape, nd) != (int64_t)len) FAIL("ndarray.from_data: a shape of %lld elements over data of %u",
                                              (long long)prod(shape, nd), len);
    contig_strides(nd, shape, st);
    return wrap(args[0], dt, nd, shape, st, 0);
}

/* ---- shape and views ------------------------------------------------------------------- */

/* the array argument as an ND, required to be an ndarray (views share data) */
static void self_nd(SeaVM *vm, int argc, SeaValue *args, ND *a, Tmp *t, const char *fn) {
    if (argc < 1) FAIL("ndarray.%s: expected an array", fn);
    opnd(vm, args[0], a, t, fn);
    if (a->nd == 0 && !is_nd(args[0])) {
        /* a scalar: make it a 0-d array so views have data */
        ND o;
        SeaValue c = copy_as(vm, a, a->dt, &o);
        tmp_hold(t, c);
        *a = o;
    }
}

static SeaValue ret_view(Tmp *t, const ND *a, int nd, const int64_t *shape, const int64_t *strides, int64_t off) {
    SeaValue v = view(a, nd, shape, strides, off);
    tmp_free(t);
    return v;
}

/* reshape(a, shape) — a view when the data allows it, a copy otherwise */
static SeaValue n_reshape(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a;
    self_nd(vm, argc, args, &a, &t, "reshape");
    int64_t shape[MAXD];
    int nd = 0;
    if (argc == 2) {
        /* reshape(a, [2, 3]) or reshape(a, 6), with one -1 allowed */
        SeaValue v = args[1];
        if (is_num(v)) { shape[0] = iv(v); nd = 1; }
        else if (sea_is_array(v)) {
            SeaArray *arr = SEA_AS_ARRAY(v);
            if (arr->len > MAXD) FAIL("ndarray.reshape: at most %d dimensions", MAXD);
            for (uint32_t i = 0; i < arr->len; i++) shape[nd++] = iv(arr->data[i]);
        } else FAIL("ndarray.reshape: a shape is an int or an array of ints");
    } else {
        for (int i = 1; i < argc && nd < MAXD; i++) shape[nd++] = iv(args[i]);
    }
    int neg = -1;
    int64_t known = 1;
    for (int i = 0; i < nd; i++) {
        if (shape[i] == -1) { if (neg >= 0) FAIL("ndarray.reshape: only one dimension can be -1"); neg = i; }
        else if (shape[i] < 0) FAIL("ndarray.reshape: negative dimensions are not allowed");
        else known *= shape[i];
    }
    if (neg >= 0) {
        if (known == 0 || a.size % known != 0) goto bad;
        shape[neg] = a.size / known;
    }
    if (prod(shape, nd) != a.size) goto bad;
    int64_t st[MAXD];
    if (is_contig(&a)) {
        contig_strides(nd, shape, st);
        return ret_view(&t, &a, nd, shape, st, a.off);
    } else {
        ND c;
        SeaValue cv = copy_as(vm, &a, a.dt, &c);
        contig_strides(nd, shape, st);
        SeaValue r = view(&c, nd, shape, st, 0);
        sea_obj_release(sea_as_obj(cv));
        tmp_free(&t);
        return r;
    }
bad:;
    char x[160], y[160];
    FAIL("ndarray.reshape: cannot reshape an array of shape %s into shape %s", shape_str(a.shape, a.nd, x, sizeof x),
         shape_str(shape, nd, y, sizeof y));
}

/* transpose(a [, axes]) */
static SeaValue n_transpose(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a;
    self_nd(vm, argc, args, &a, &t, "transpose");
    int perm[MAXD];
    if (argc > 1 && !sea_is_nil(args[1])) {
        if (!sea_is_array(args[1]) || (int)SEA_AS_ARRAY(args[1])->len != a.nd)
            FAIL("ndarray.transpose: axes must list each of the %d axes once", a.nd);
        bool seen[MAXD] = { false };
        for (int i = 0; i < a.nd; i++) {
            int ax = norm_axis(vm, iv(SEA_AS_ARRAY(args[1])->data[i]), a.nd, "transpose");
            if (seen[ax]) FAIL("ndarray.transpose: repeated axis in axes");
            seen[ax] = true;
            perm[i] = ax;
        }
    } else {
        for (int i = 0; i < a.nd; i++) perm[i] = a.nd - 1 - i;
    }
    int64_t sh[MAXD], st[MAXD];
    for (int i = 0; i < a.nd; i++) { sh[i] = a.shape[perm[i]]; st[i] = a.strides[perm[i]]; }
    return ret_view(&t, &a, a.nd, sh, st, a.off);
}

static SeaValue n_swapaxes(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a;
    self_nd(vm, argc, args, &a, &t, "swapaxes");
    if (argc < 3) FAIL("ndarray.swapaxes: expected (a, axis1, axis2)");
    int x = norm_axis(vm, iv(args[1]), a.nd, "swapaxes"), y = norm_axis(vm, iv(args[2]), a.nd, "swapaxes");
    int64_t sh[MAXD], st[MAXD];
    memcpy(sh, a.shape, sizeof sh); memcpy(st, a.strides, sizeof st);
    sh[x] = a.shape[y]; sh[y] = a.shape[x]; st[x] = a.strides[y]; st[y] = a.strides[x];
    return ret_view(&t, &a, a.nd, sh, st, a.off);
}

/* squeeze(a [, axis]) */
static SeaValue n_squeeze(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a;
    self_nd(vm, argc, args, &a, &t, "squeeze");
    bool mask[MAXD];
    if (argc > 1 && !sea_is_nil(args[1])) {
        axes_arg(vm, args[1], a.nd, mask, "squeeze");
        for (int i = 0; i < a.nd; i++)
            if (mask[i] && a.shape[i] != 1) FAIL("ndarray.squeeze: cannot select an axis to squeeze out which has size not equal to one");
    } else for (int i = 0; i < a.nd; i++) mask[i] = a.shape[i] == 1;
    int64_t sh[MAXD], st[MAXD];
    int m = 0;
    for (int i = 0; i < a.nd; i++) if (!mask[i]) { sh[m] = a.shape[i]; st[m] = a.strides[i]; m++; }
    return ret_view(&t, &a, m, sh, st, a.off);
}

/* expand_dims(a, axis) */
static SeaValue n_expand_dims(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a;
    self_nd(vm, argc, args, &a, &t, "expand_dims");
    if (argc < 2) FAIL("ndarray.expand_dims: expected (a, axis)");
    if (a.nd >= MAXD) FAIL("ndarray.expand_dims: at most %d dimensions", MAXD);
    int ax = norm_axis(vm, iv(args[1]), a.nd + 1, "expand_dims");
    int64_t sh[MAXD], st[MAXD];
    for (int i = 0, j = 0; i <= a.nd; i++) {
        if (i == ax) { sh[i] = 1; st[i] = 0; continue; }
        sh[i] = a.shape[j]; st[i] = a.strides[j]; j++;
    }
    return ret_view(&t, &a, a.nd + 1, sh, st, a.off);
}

/* broadcast_to(a, shape): a view with strides 0 (do not write into it) */
static SeaValue n_broadcast_to(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a;
    self_nd(vm, argc, args, &a, &t, "broadcast_to");
    int64_t shape[MAXD];
    int nd = shape_arg(vm, argc > 1 ? args[1] : SEA_NIL_VAL, shape, "broadcast_to");
    if (nd < a.nd) goto bad;
    int64_t st[MAXD];
    for (int d = 0; d < nd; d++) {
        int od = d - (nd - a.nd);
        if (od < 0) { st[d] = 0; continue; }
        if (a.shape[od] == shape[d]) st[d] = a.strides[od];
        else if (a.shape[od] == 1) st[d] = 0;
        else goto bad;
    }
    return ret_view(&t, &a, nd, shape, st, a.off);
bad:;
    char x[160], y[160];
    FAIL("ndarray.broadcast_to: cannot broadcast shape %s to %s", shape_str(a.shape, a.nd, x, sizeof x),
         shape_str(shape, nd, y, sizeof y));
}

/* as_strided(a, shape, strides [, offset]): a view of a's data with that
 * geometry (strides in elements, offset added to a's); it must stay inside
 * the data */
static SeaValue n_as_strided(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 3 || !is_nd(args[0])) FAIL("ndarray.as_strided: expected (array, shape, strides [, offset])");
    ND a;
    nd_read(vm, args[0], &a, "as_strided");
    int64_t shape[MAXD], st[MAXD];
    int nd = shape_arg(vm, args[1], shape, "as_strided");
    if (!sea_is_array(args[2]) || (int)SEA_AS_ARRAY(args[2])->len != nd)
        FAIL("ndarray.as_strided: strides must be an array as long as the shape");
    for (int i = 0; i < nd; i++) st[i] = iv(SEA_AS_ARRAY(args[2])->data[i]);
    int64_t off = a.off + (argc > 3 && !sea_is_nil(args[3]) ? iv(args[3]) : 0);
    SeaValue v = view(&a, nd, shape, st, off);
    ND check;
    nd_read(vm, v, &check, "as_strided");   /* panics when it reaches outside */
    return v;
}

/* flip(a [, axis]) */
static SeaValue n_flip(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a;
    self_nd(vm, argc, args, &a, &t, "flip");
    bool mask[MAXD];
    axes_arg(vm, argc > 1 ? args[1] : SEA_NIL_VAL, a.nd, mask, "flip");
    int64_t st[MAXD], off = a.off;
    for (int i = 0; i < a.nd; i++) {
        st[i] = a.strides[i];
        if (mask[i] && a.shape[i] > 0) { off += a.strides[i] * (a.shape[i] - 1); st[i] = -a.strides[i]; }
    }
    return ret_view(&t, &a, a.nd, a.shape, st, off);
}

/* diagonal(a, offset): the view of a 2-D array's diagonal */
static SeaValue n_diagonal(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a;
    self_nd(vm, argc, args, &a, &t, "diagonal");
    if (a.nd != 2) FAIL("ndarray.diagonal: expected a 2-D array");
    int64_t k = argc > 1 && !sea_is_nil(args[1]) ? iv(args[1]) : 0;
    int64_t r0 = k < 0 ? -k : 0, c0 = k > 0 ? k : 0;
    int64_t n = 0;
    if (r0 < a.shape[0] && c0 < a.shape[1]) {
        n = a.shape[0] - r0 < a.shape[1] - c0 ? a.shape[0] - r0 : a.shape[1] - c0;
    }
    int64_t sh[1] = { n }, st[1] = { a.strides[0] + a.strides[1] };
    return ret_view(&t, &a, 1, sh, st, n > 0 ? a.off + r0 * a.strides[0] + c0 * a.strides[1] : a.off);
}

static SeaValue n_copy(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a, o;
    self_nd(vm, argc, args, &a, &t, "copy");
    SeaValue r = copy_as(vm, &a, a.dt, &o);
    tmp_free(&t);
    return r;
}

static SeaValue n_astype(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a, o;
    self_nd(vm, argc, args, &a, &t, "astype");
    int dt = dt_parse(vm, argc > 1 ? args[1] : SEA_NIL_VAL, "astype");
    if (dt < 0) FAIL("ndarray.astype: expected a dtype");
    SeaValue r = copy_as(vm, &a, dt, &o);
    tmp_free(&t);
    return r;
}

/* ravel(a): a 1-D view when contiguous, else a copy */
static SeaValue n_ravel(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a;
    self_nd(vm, argc, args, &a, &t, "ravel");
    int64_t sh[1] = { a.size }, st[1] = { 1 };
    if (is_contig(&a)) return ret_view(&t, &a, 1, sh, st, a.off);
    ND o;
    SeaValue c = copy_as(vm, &a, a.dt, &o);
    SeaValue r = view(&o, 1, sh, st, 0);
    sea_obj_release(sea_as_obj(c));
    tmp_free(&t);
    return r;
}

static SeaValue n_ascontiguous(SeaVM *vm, int argc, SeaValue *args) {
    if (argc > 0 && is_nd(args[0])) {
        ND a;
        nd_read(vm, args[0], &a, "ascontiguousarray");
        if (is_contig(&a)) return sea_val_ref(args[0]);
    }
    return n_copy(vm, argc, args);
}

/* tolist(a): nested Sea arrays (a scalar for 0-d) */
static SeaValue tolist_rec(const ND *a, int d, const char *p) {
    if (d == a->nd) return scalar_at(a->dt, p);
    SeaArray *arr = sea_array_new((uint32_t)(a->shape[d] > 0 ? a->shape[d] : 1));
    for (int64_t i = 0; i < a->shape[d]; i++) sea_array_push_move(arr, tolist_rec(a, d + 1, p + i * a->strides[d] * ESZ(a->dt)));
    return sea_obj_val(&arr->obj);
}

static SeaValue n_tolist(SeaVM *vm, int argc, SeaValue *args) {
    Tmp t = { 0 };
    ND a;
    if (argc < 1) FAIL("ndarray.tolist: expected an array");
    opnd(vm, args[0], &a, &t, "tolist");
    SeaValue r = tolist_rec(&a, 0, a.p);
    tmp_free(&t);
    return r;
}

/* fill(a, value): every element, in place */
static SeaValue n_fill(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !is_nd(args[0])) FAIL("ndarray.fill: expected (array, value)");
    Tmp t = { 0 };
    ND a, v;
    nd_read(vm, args[0], &a, "fill");
    opnd(vm, args[1], &v, &t, "fill");
    assign(vm, &a, &v, "fill");
    tmp_free(&t);
    return SEA_NIL_VAL;
}

/* concatenate(arrays, axis) and stack(arrays, axis) */
static SeaValue joining(SeaVM *vm, int argc, SeaValue *args, bool stack, const char *fn) {
    if (argc < 1 || !sea_is_array(args[0])) FAIL("ndarray.%s: expected an array of arrays", fn);
    SeaArray *list = SEA_AS_ARRAY(args[0]);
    int n = (int)list->len;
    if (n == 0) FAIL("ndarray.%s: need at least one array", fn);
    if (n > 4096) FAIL("ndarray.%s: at most 4096 arrays", fn);
    ND *parts = calloc((size_t)n, sizeof(ND));
    Tmp *tmps = calloc((size_t)n, sizeof(Tmp));
    for (int i = 0; i < n; i++) opnd(vm, list->data[i], &parts[i], &tmps[i], fn);
    int dt = DT_BOOL;
    for (int i = 0; i < n; i++) dt = promote(dt, parts[i].dt);
    int nd = parts[0].nd;
    SeaValue axv = argc > 1 && !sea_is_nil(args[1]) ? args[1] : sea_int_val(0);
    bool flat = argc > 1 && sea_is_nil(args[1]) && !stack;
    char x[160], y[160];
    for (int i = 0; i < n && !flat; i++) {
        if (parts[i].nd != nd) {
            int pnd = parts[i].nd;
            free(parts); free(tmps);
            FAIL("ndarray.%s: all the arrays must have the same number of dimensions (%d and %d)", fn, nd, pnd);
        }
    }
    int axis;
    int64_t oshape[MAXD];
    int ond;
    if (flat) {
        int64_t tot = 0;
        for (int i = 0; i < n; i++) tot += parts[i].size;
        oshape[0] = tot; ond = 1; axis = 0;
    } else if (stack) {
        axis = norm_axis(vm, iv(axv), nd + 1, fn);
        for (int i = 1; i < n; i++)
            if (memcmp(parts[i].shape, parts[0].shape, sizeof(int64_t) * nd) != 0) {
                free(parts); free(tmps);
                FAIL("ndarray.%s: all the arrays must have the same shape", fn);
            }
        ond = nd + 1;
        for (int d = 0, j = 0; d < ond; d++) oshape[d] = d == axis ? n : parts[0].shape[j++];
    } else {
        if (nd == 0) { free(parts); free(tmps); FAIL("ndarray.%s: arrays with no dimensions cannot be concatenated", fn); }
        axis = norm_axis(vm, iv(axv), nd, fn);
        memcpy(oshape, parts[0].shape, sizeof(int64_t) * nd);
        oshape[axis] = 0;
        for (int i = 0; i < n; i++) {
            for (int d = 0; d < nd; d++)
                if (d != axis && parts[i].shape[d] != parts[0].shape[d]) {
                    shape_str(parts[0].shape, nd, x, sizeof x);
                    shape_str(parts[i].shape, nd, y, sizeof y);
                    free(parts); free(tmps);
                    FAIL("ndarray.%s: shapes %s and %s differ outside axis %d", fn, x, y, axis);
                }
            oshape[axis] += parts[i].shape[axis];
        }
        ond = nd;
    }
    ND o;
    SeaValue r = nd_new(vm, dt, ond, oshape, &o);
    int64_t pos = 0;
    for (int i = 0; i < n; i++) {
        ND dst = o;
        if (flat) {
            ND src = parts[i];
            if (!is_contig(&src)) { ND c; tmp_hold(&tmps[i], copy_as(vm, &src, src.dt, &c)); src = c; }
            src.nd = 1; src.shape[0] = src.size; src.strides[0] = 1;
            dst.shape[0] = src.size; dst.p = o.p + pos * ESZ(o.dt); pos += src.size;
            assign(vm, &dst, &src, fn);
        } else if (stack) {
            dst.nd = nd;
            for (int d = 0, j = 0; d < ond; d++) if (d != axis) { dst.shape[j] = o.shape[d]; dst.strides[j] = o.strides[d]; j++; }
            dst.p = o.p + (int64_t)i * o.strides[axis] * ESZ(o.dt);
            assign(vm, &dst, &parts[i], fn);
        } else {
            dst.shape[axis] = parts[i].shape[axis];
            dst.p = o.p + pos * o.strides[axis] * ESZ(o.dt);
            pos += parts[i].shape[axis];
            assign(vm, &dst, &parts[i], fn);
        }
    }
    for (int i = 0; i < n; i++) tmp_free(&tmps[i]);
    free(parts); free(tmps);
    return r;
}

static SeaValue n_concatenate(SeaVM *vm, int argc, SeaValue *args) { return joining(vm, argc, args, false, "concatenate"); }
static SeaValue n_stack(SeaVM *vm, int argc, SeaValue *args) { return joining(vm, argc, args, true, "stack"); }

/* take(a, indices, axis): the elements at those indices along axis (a
 * flattened when axis is nil); the result's shape has indices' shape in
 * place of that axis */
static SeaValue take_nd(SeaVM *vm, ND *a, ND *idx, int axis, bool flat) {
    ND src = *a;
    Tmp t = { 0 };
    if (flat) {
        if (!is_contig(&src)) { ND c; tmp_hold(&t, copy_as(vm, &src, src.dt, &c)); src = c; }
        src.nd = 1; src.shape[0] = src.size; src.strides[0] = 1;
        axis = 0;
    }
    int64_t n = src.shape[axis];
    ND ix = *idx;
    if (!is_contig(&ix) || ix.dt != DT_INT) { ND c; tmp_hold(&t, copy_as(vm, &ix, DT_INT, &c)); ix = c; }
    const int64_t *iv_ = (const int64_t *)ix.p;
    for (int64_t i = 0; i < ix.size; i++) {
        int64_t k = iv_[i];
        if (k < -n || k >= n) {
            tmp_free(&t);
            FAIL("ndarray.take: index %lld is out of bounds for axis %d with size %lld", (long long)k, axis, (long long)n);
        }
    }
    if (src.nd - 1 + ix.nd > MAXD) FAIL("ndarray.take: too many dimensions");
    int64_t oshape[MAXD];
    int ond = 0;
    for (int d = 0; d < axis; d++) oshape[ond++] = src.shape[d];
    for (int d = 0; d < ix.nd; d++) oshape[ond++] = ix.shape[d];
    for (int d = axis + 1; d < src.nd; d++) oshape[ond++] = src.shape[d];
    ND o;
    SeaValue r = nd_new(vm, src.dt, ond, oshape, &o);
    /* for each index: copy the sub-array src[..., k, ...] into o[..., i..., ...] */
    ND sub = src, dst = o;
    sub.nd = src.nd - 1;
    for (int d = 0, j = 0; d < src.nd; d++) if (d != axis) { sub.shape[j] = src.shape[d]; sub.strides[j] = src.strides[d]; j++; }
    dst.nd = sub.nd;
    for (int d = 0, j = 0; d < ond; d++) if (d < axis || d >= axis + ix.nd) { dst.shape[j] = o.shape[d]; dst.strides[j] = o.strides[d]; j++; }
    int64_t istride = ix.nd > 0 ? o.strides[axis + ix.nd - 1] : 0;
    Cast c = { src.dt, src.dt };
    for (int64_t i = 0; i < ix.size; i++) {
        int64_t k = iv_[i] < 0 ? iv_[i] + n : iv_[i];
        ND s = sub, d_ = dst;
        s.p = src.p + k * src.strides[axis] * ESZ(src.dt);
        d_.p = o.p + i * istride * ESZ(o.dt);
        ND *ops[2] = { &d_, &s };
        run(ops, 2, sub.nd, sub.shape, k_cast, &c);
    }
    tmp_free(&t);
    return finish(r, &o);
}

static SeaValue n_take(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) FAIL("ndarray.take: expected (a, indices [, axis])");
    Tmp t = { 0 };
    ND a, ix;
    opnd(vm, args[0], &a, &t, "take");
    opnd(vm, args[1], &ix, &t, "take");
    if (ix.dt == DT_F64) FAIL("ndarray.take: indices must be ints");
    bool flat = argc < 3 || sea_is_nil(args[2]);
    int axis = flat ? 0 : norm_axis(vm, iv(args[2]), a.nd, "take");
    SeaValue r = take_nd(vm, &a, &ix, axis, flat);
    tmp_free(&t);
    return r;
}

/* ---- indexing ------------------------------------------------------------------------- */

enum { IX_INT, IX_SLICE, IX_NEW, IX_ELLIPSIS, IX_ARRAY };

typedef struct {
    int kind;
    int64_t i, start, stop, step;
    bool has_start, has_stop, has_step;
    SeaValue arr;   /* IX_ARRAY: borrowed */
} Ix;

static const char *skip_ws(const char *s, const char *e) { while (s < e && (*s == ' ' || *s == '\t')) s++; return s; }

static bool parse_int(const char *s, const char *e, int64_t *out) {
    s = skip_ws(s, e);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    if (s == e) return false;
    bool neg = false;
    if (*s == '-' || *s == '+') { neg = *s == '-'; s++; }
    if (s == e) return false;
    int64_t v = 0;
    for (; s < e; s++) { if (*s < '0' || *s > '9') return false; v = v * 10 + (*s - '0'); }
    *out = neg ? -v : v;
    return true;
}

/* one index written as text: "3", "1:5", "::2", "...", "newaxis" */
static void parse_text_ix(SeaVM *vm, const char *s, const char *e, Ix *ix) {
    memset(ix, 0, sizeof *ix);
    s = skip_ws(s, e);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    size_t n = (size_t)(e - s);
    if ((n == 3 && memcmp(s, "...", 3) == 0)) { ix->kind = IX_ELLIPSIS; return; }
    if ((n == 7 && memcmp(s, "newaxis", 7) == 0) || (n == 4 && memcmp(s, "None", 4) == 0)) { ix->kind = IX_NEW; return; }
    const char *c1 = memchr(s, ':', n);
    if (!c1) {
        if (!parse_int(s, e, &ix->i)) FAIL("ndarray: bad index \"%.*s\"", (int)n, s);
        ix->kind = IX_INT;
        return;
    }
    ix->kind = IX_SLICE;
    const char *c2 = memchr(c1 + 1, ':', (size_t)(e - c1 - 1));
    const char *e1 = c1, *s2 = c1 + 1, *e2 = c2 ? c2 : e;
    if (skip_ws(s, e1) != e1) { if (!parse_int(s, e1, &ix->start)) goto bad; ix->has_start = true; }
    if (skip_ws(s2, e2) != e2) { if (!parse_int(s2, e2, &ix->stop)) goto bad; ix->has_stop = true; }
    if (c2 && skip_ws(c2 + 1, e) != e) { if (!parse_int(c2 + 1, e, &ix->step)) goto bad; ix->has_step = true; }
    if (ix->has_step && ix->step == 0) FAIL("ndarray: slice step cannot be zero");
    return;
bad:
    FAIL("ndarray: bad slice \"%.*s\"", (int)n, s);
}

/* the index arguments → a list of Ix */
static int parse_ix(SeaVM *vm, int argc, SeaValue *args, Ix *ixs, int cap) {
    int n = 0;
    for (int i = 0; i < argc; i++) {
        SeaValue v = args[i];
        if (n >= cap) FAIL("ndarray: too many indices");
        if (sea_is_nil(v)) { memset(&ixs[n], 0, sizeof(Ix)); ixs[n++].kind = IX_SLICE; continue; }
        if (is_num(v)) {
            if (sea_is_number(v) && sea_as_number(v) != floor(sea_as_number(v))) FAIL("ndarray: an index must be an integer");
            memset(&ixs[n], 0, sizeof(Ix)); ixs[n].kind = IX_INT; ixs[n].i = iv(v); n++;
            continue;
        }
        if (sea_is_string(v) || sea_is_rope(v)) {
            SeaString *s = sea_value_get_flat_string(v);
            const char *p = s->data, *e = s->data + s->len;
            while (p <= e) {
                const char *comma = memchr(p, ',', (size_t)(e - p));
                const char *end = comma ? comma : e;
                if (n >= cap) { sea_obj_release(&s->obj); FAIL("ndarray: too many indices"); }
                parse_text_ix(vm, p, end, &ixs[n++]);
                if (!comma) break;
                p = comma + 1;
            }
            sea_obj_release(&s->obj);
            continue;
        }
        if (is_nd(v) || sea_is_array(v) || sea_is_int_array(v)) {
            memset(&ixs[n], 0, sizeof(Ix)); ixs[n].kind = IX_ARRAY; ixs[n].arr = v; n++;
            continue;
        }
        FAIL("ndarray: an index is an int, a slice string (\"1:3\"), nil or an array, got %s", sea_type_name(v));
    }
    return n;
}

typedef struct {
    ND v;               /* the view after the basic indices */
    int adv;            /* -1, or the axis of v the index array applies to */
    ND advix;           /* the index array (ints) */
    bool mask;          /* a whole-array bool mask */
    ND maskv;
    Tmp t;
} Sel;

/* apply indices to a → a view (and maybe one index array / a mask) */
static void select_(SeaVM *vm, ND *a, int argc, SeaValue *args, Sel *sel, const char *fn) {
    Ix ixs[MAXD * 2];
    int n = parse_ix(vm, argc, args, ixs, MAXD * 2);
    memset(sel, 0, sizeof *sel);
    sel->adv = -1;
    /* a single bool array with the array's shape: a mask */
    if (n == 1 && ixs[0].kind == IX_ARRAY) {
        ND m;
        opnd(vm, ixs[0].arr, &m, &sel->t, fn);
        if (m.dt == DT_BOOL && (m.nd > 1 || (m.nd == a->nd && m.nd > 0))) {
            if (m.nd != a->nd || memcmp(m.shape, a->shape, sizeof(int64_t) * a->nd) != 0) {
                char x[160], y[160];
                FAIL("ndarray.%s: a boolean index of shape %s for an array of shape %s", fn,
                     shape_str(m.shape, m.nd, x, sizeof x), shape_str(a->shape, a->nd, y, sizeof y));
            }
            sel->mask = true;
            sel->maskv = m;
            sel->v = *a;
            return;
        }
    }
    int consumed = 0, ell = -1;
    for (int i = 0; i < n; i++) {
        if (ixs[i].kind == IX_ELLIPSIS) { if (ell >= 0) FAIL("ndarray.%s: an index can only have one ellipsis", fn); ell = i; }
        else if (ixs[i].kind != IX_NEW) consumed++;
    }
    if (consumed > a->nd) FAIL("ndarray.%s: too many indices for an array of %d dimensions", fn, a->nd);
    ND *v = &sel->v;
    *v = *a;
    v->nd = 0;
    int64_t off = a->off;
    char *p = a->p;
    int d = 0;
    for (int i = 0; i < n; i++) {
        Ix *x = &ixs[i];
        if (x->kind == IX_ELLIPSIS) {
            int fill = a->nd - consumed;
            for (int k = 0; k < fill; k++, d++) { v->shape[v->nd] = a->shape[d]; v->strides[v->nd] = a->strides[d]; v->nd++; }
            continue;
        }
        if (v->nd >= MAXD) FAIL("ndarray.%s: too many dimensions", fn);
        if (x->kind == IX_NEW) { v->shape[v->nd] = 1; v->strides[v->nd] = 0; v->nd++; continue; }
        int64_t len = a->shape[d], st = a->strides[d];
        if (x->kind == IX_INT) {
            int64_t k = x->i < 0 ? x->i + len : x->i;
            if (k < 0 || k >= len)
                FAIL("ndarray.%s: index %lld is out of bounds for axis %d with size %lld", fn, (long long)x->i, d, (long long)len);
            off += k * st; p += k * st * ESZ(a->dt);
        } else if (x->kind == IX_SLICE) {
            int64_t step = x->has_step ? x->step : 1, start, stop;
            if (step > 0) {
                start = x->has_start ? x->start : 0;
                stop = x->has_stop ? x->stop : len;
                if (start < 0) start += len;
                start = start < 0 ? 0 : start > len ? len : start;
                if (stop < 0) stop += len;
                stop = stop < 0 ? 0 : stop > len ? len : stop;
            } else {
                start = x->has_start ? x->start : len - 1;
                stop = x->has_stop ? x->stop : -len - 1;
                if (start < 0) start += len;
                start = start < -1 ? -1 : start >= len ? len - 1 : start;
                if (stop < 0) stop += len;
                stop = stop < -1 ? -1 : stop >= len ? len - 1 : stop;
            }
            int64_t cnt = step > 0 ? (stop > start ? (stop - start + step - 1) / step : 0)
                                   : (start > stop ? (start - stop - step - 1) / -step : 0);
            if (cnt > 0) { off += start * st; p += start * st * ESZ(a->dt); }
            v->shape[v->nd] = cnt; v->strides[v->nd] = st * step; v->nd++;
        } else {
            if (sel->adv >= 0) FAIL("ndarray.%s: only one index array is supported (use take for more)", fn);
            ND ix;
            opnd(vm, x->arr, &ix, &sel->t, fn);
            if (ix.dt == DT_F64) FAIL("ndarray.%s: index arrays must be ints or bools", fn);
            if (ix.dt == DT_BOOL) {
                if (ix.nd != 1 || ix.shape[0] != len)
                    FAIL("ndarray.%s: a boolean index along axis %d must be 1-D with %lld elements", fn, d, (long long)len);
                /* the positions that are true */
                int64_t cnt = 0;
                for (int64_t k = 0; k < len; k++) cnt += *(int64_t *)(ix.p + k * ix.strides[0] * 8) != 0;
                ND w;
                int64_t sh[1] = { cnt };
                tmp_hold(&sel->t, nd_new(vm, DT_INT, 1, sh, &w));
                for (int64_t k = 0, j = 0; k < len; k++)
                    if (*(int64_t *)(ix.p + k * ix.strides[0] * 8)) ((int64_t *)w.p)[j++] = k;
                ix = w;
            }
            sel->adv = v->nd;
            sel->advix = ix;
            v->shape[v->nd] = len; v->strides[v->nd] = st; v->nd++;
        }
        d++;
    }
    for (; d < a->nd; d++) {
        if (v->nd >= MAXD) FAIL("ndarray.%s: too many dimensions", fn);
        v->shape[v->nd] = a->shape[d]; v->strides[v->nd] = a->strides[d]; v->nd++;
    }
    v->off = off;
    v->p = p;
    v->size = prod(v->shape, v->nd);
}

typedef struct { int dt; char *out; int64_t k; } MaskCtx;

static void k_mask_get(int64_t n, char **p, const int64_t *s, void *ctx) {
    MaskCtx *c = ctx;
    const int64_t *m = (const int64_t *)p[0];
    int e = ESZ(c->dt);
    for (int64_t i = 0; i < n; i++) if (m[i * s[0]]) { memcpy(c->out + c->k * e, p[1] + i * s[1] * e, e); c->k++; }
}
static void k_mask_count(int64_t n, char **p, const int64_t *s, void *ctx) {
    MaskCtx *c = ctx;
    const int64_t *m = (const int64_t *)p[0];
    for (int64_t i = 0; i < n; i++) if (m[i * s[0]]) c->k++;
}

/* a[...] → a view, a scalar, or a copy (index array, mask) */
static SeaValue n_get(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !is_nd(args[0])) FAIL("ndarray.get: expected an ndarray");
    ND a;
    nd_read(vm, args[0], &a, "get");
    Sel sel;
    select_(vm, &a, argc - 1, args + 1, &sel, "get");
    SeaValue r;
    if (sel.mask) {
        MaskCtx c = { a.dt, NULL, 0 };
        ND *ops[2] = { &sel.maskv, &a };
        run(ops, 2, a.nd, a.shape, k_mask_count, &c);
        ND o;
        int64_t sh[1] = { c.k };
        r = nd_new(vm, a.dt, 1, sh, &o);
        c.out = o.p; c.k = 0;
        run(ops, 2, a.nd, a.shape, k_mask_get, &c);
    } else if (sel.adv >= 0) {
        r = take_nd(vm, &sel.v, &sel.advix, sel.adv, false);
    } else if (sel.v.nd == 0) {
        r = scalar_at(a.dt, sel.v.p);
    } else {
        r = view(&sel.v, sel.v.nd, sel.v.shape, sel.v.strides, sel.v.off);
    }
    tmp_free(&sel.t);
    return r;
}

typedef struct { int dt; ND *val; int64_t k; bool scalar; } MaskSet;

static void k_mask_set(int64_t n, char **p, const int64_t *s, void *ctx) {
    MaskSet *c = ctx;
    const int64_t *m = (const int64_t *)p[0];
    for (int64_t i = 0; i < n; i++)
        if (m[i * s[0]]) {
            int e = ESZ(c->dt);
            const char *src = c->scalar ? c->val->p : c->val->p + c->k * c->val->strides[0] * e;
            memcpy(p[1] + i * s[1] * e, src, e);
            c->k++;
        }
}

/* a[...] = value (the last argument), broadcast */
static SeaValue n_set(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !is_nd(args[0])) FAIL("ndarray.set: expected (array, index..., value)");
    ND a;
    nd_read(vm, args[0], &a, "set");
    Tmp t = { 0 };
    ND val;
    opnd(vm, args[argc - 1], &val, &t, "set");
    Sel sel;
    select_(vm, &a, argc - 2, args + 1, &sel, "set");
    as_dt(vm, &val, a.dt, &t);
    if (sel.mask) {
        MaskCtx cnt = { a.dt, NULL, 0 };
        ND *ops[2] = { &sel.maskv, &a };
        run(ops, 2, a.nd, a.shape, k_mask_count, &cnt);
        bool scalar = val.size == 1;
        if (!scalar && (val.nd != 1 || val.shape[0] != cnt.k))
            FAIL("ndarray.set: %lld values for %lld places of a boolean mask", (long long)val.size, (long long)cnt.k);
        if (scalar) val.nd = 0;
        MaskSet c = { a.dt, &val, 0, scalar };
        run(ops, 2, a.nd, a.shape, k_mask_set, &c);
    } else if (sel.adv >= 0) {
        /* the shape of the result: v with axis adv replaced by the index's shape */
        ND *v = &sel.v, *ix = &sel.advix;
        ND ixc = *ix;
        if (!is_contig(&ixc) || ixc.dt != DT_INT) { ND c; tmp_hold(&t, copy_as(vm, ix, DT_INT, &c)); ixc = c; }
        int64_t rshape[MAXD];
        int rnd = 0;
        for (int d = 0; d < sel.adv; d++) rshape[rnd++] = v->shape[d];
        for (int d = 0; d < ixc.nd; d++) rshape[rnd++] = ixc.shape[d];
        for (int d = sel.adv + 1; d < v->nd; d++) rshape[rnd++] = v->shape[d];
        /* value broadcast to that shape */
        ND bv = val;
        int64_t bst[MAXD];
        if (val.nd > rnd) FAIL("ndarray.set: the value has more dimensions than the selection");
        for (int d = 0; d < rnd; d++) {
            int od = d - (rnd - val.nd);
            if (od < 0) bst[d] = 0;
            else if (val.shape[od] == rshape[d]) bst[d] = val.strides[od];
            else if (val.shape[od] == 1) bst[d] = 0;
            else FAIL("ndarray.set: could not broadcast the value into the selection");
        }
        bv.nd = rnd;
        memcpy(bv.shape, rshape, sizeof(int64_t) * rnd);
        memcpy(bv.strides, bst, sizeof(int64_t) * rnd);
        int64_t n = v->shape[sel.adv];
        ND sub = *v;
        sub.nd = v->nd - 1;
        for (int d = 0, j = 0; d < v->nd; d++) if (d != sel.adv) { sub.shape[j] = v->shape[d]; sub.strides[j] = v->strides[d]; j++; }
        ND vs = bv;
        vs.nd = sub.nd;
        for (int d = 0, j = 0; d < rnd; d++) if (d < sel.adv || d >= sel.adv + ixc.nd) { vs.shape[j] = bv.shape[d]; vs.strides[j] = bv.strides[d]; j++; }
        int64_t idx[MAXD] = { 0 };
        const int64_t *iv_ = (const int64_t *)ixc.p;
        for (int64_t q = 0; q < ixc.size; q++) {
            int64_t k = iv_[q] < 0 ? iv_[q] + n : iv_[q];
            if (k < 0 || k >= n) FAIL("ndarray.set: index %lld is out of bounds for size %lld", (long long)iv_[q], (long long)n);
            ND dst = sub, src = vs;
            dst.p = v->p + k * v->strides[sel.adv] * ESZ(a.dt);
            int64_t so = 0;
            for (int d = 0; d < ixc.nd; d++) so += idx[d] * bv.strides[sel.adv + d];
            src.p = bv.p + so * ESZ(a.dt);
            Cast c = { a.dt, a.dt };
            ND *ops[2] = { &dst, &src };
            run(ops, 2, sub.nd, sub.shape, k_cast, &c);
            for (int d = ixc.nd - 1; d >= 0; d--) { if (++idx[d] < ixc.shape[d]) break; idx[d] = 0; }
        }
    } else {
        assign(vm, &sel.v, &val, "set");
    }
    tmp_free(&sel.t);
    tmp_free(&t);
    return sea_val_ref(args[0]);
}

/* nonzero(a) → array of index arrays, one per dimension */
static SeaValue n_nonzero(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.nonzero: expected an array");
    Tmp t = { 0 };
    ND a;
    opnd(vm, args[0], &a, &t, "nonzero");
    as_dt(vm, &a, DT_BOOL, &t);
    ND c;
    if (!is_contig(&a)) { tmp_hold(&t, copy_as(vm, &a, DT_BOOL, &c)); a = c; }
    int64_t cnt = 0;
    for (int64_t i = 0; i < a.size; i++) cnt += ((int64_t *)a.p)[i] != 0;
    int nd = a.nd > 0 ? a.nd : 1;
    SeaArray *out = sea_array_new((uint32_t)nd);
    ND outs[MAXD];
    int64_t sh[1] = { cnt };
    for (int d = 0; d < nd; d++) sea_array_push_move(out, nd_new(vm, DT_INT, 1, sh, &outs[d]));
    int64_t idx[MAXD] = { 0 }, j = 0;
    for (int64_t i = 0; i < a.size; i++) {
        if (((int64_t *)a.p)[i]) {
            for (int d = 0; d < a.nd; d++) ((int64_t *)outs[d].p)[j] = idx[d];
            j++;
        }
        for (int d = a.nd - 1; d >= 0; d--) { if (++idx[d] < a.shape[d]) break; idx[d] = 0; }
    }
    tmp_free(&t);
    return sea_obj_val(&out->obj);
}

/* ---- bytes ------------------------------------------------------------------------------ */

/* to_bytes(a, kind): contiguous little-endian bytes; kind "f8" "i8" "b1"
 * "f4" "i4" "u1" "c16" "c8" */
static int kind_size(const char *k) { return atoi(k + 1); }

static SeaValue n_to_bytes(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.to_bytes: expected an array");
    Tmp t = { 0 };
    ND a, c;
    opnd(vm, args[0], &a, &t, "to_bytes");
    char kind[4] = "";
    if (argc > 1 && !sea_is_nil(args[1])) {
        SeaString *s = sea_value_get_flat_string(args[1]);
        if (!s || s->len < 2 || s->len > 3) FAIL("ndarray.to_bytes: kind is like \"f8\", \"i4\" or \"b1\"");
        memcpy(kind, s->data, s->len);
        sea_obj_release(&s->obj);
    } else strcpy(kind, a.dt == DT_F64 ? "f8" : a.dt == DT_F32 ? "f4" : a.dt == DT_C128 ? "c16" : a.dt == DT_INT ? "i8" : "b1");
    tmp_hold(&t, copy_as(vm, &a, kind[0] == 'c' ? DT_C128 : kind[0] == 'f' ? DT_F64 : DT_INT, &c));
    int w = kind_size(kind);
    size_t n = (size_t)c.size * (size_t)w;
    char *buf = malloc(n + 1);
    for (int64_t i = 0; i < c.size; i++) {
        char *q = buf + i * w;
        if (kind[0] == 'c') {
            zcplx x = ((zcplx *)c.p)[i];
            if (w == 16) memcpy(q, &x, 16);
            else { float f[2] = { (float)creal(x), (float)cimag(x) }; memcpy(q, f, 8); }
        } else if (kind[0] == 'f') {
            double x = ((double *)c.p)[i];
            if (w == 8) memcpy(q, &x, 8); else { float f = (float)x; memcpy(q, &f, 4); }
        } else {
            int64_t x = ((int64_t *)c.p)[i];
            if (kind[0] == 'b') x = x != 0;
            for (int b = 0; b < w; b++) q[b] = (char)((uint64_t)x >> (8 * b));
        }
    }
    SeaValue r = sea_obj_val(&sea_string_new(buf, n)->obj);
    free(buf);
    tmp_free(&t);
    return r;
}

/* from_bytes(bytes, offset, kind, shape, big_endian, fortran) */
static SeaValue n_from_bytes(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 4) FAIL("ndarray.from_bytes: expected (bytes, offset, kind, shape)");
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) FAIL("ndarray.from_bytes: expected a string of bytes");
    int64_t off = iv(args[1]);
    SeaString *ks = sea_value_get_flat_string(args[2]);
    char kind[4] = "";
    if (ks && ks->len >= 2 && ks->len <= 3) memcpy(kind, ks->data, ks->len);
    if (ks) sea_obj_release(&ks->obj);
    bool good = kind[0] == 'c' ? strcmp(kind + 1, "16") == 0 || strcmp(kind + 1, "8") == 0
              : kind[0] && strchr("fiub", kind[0]) && kind[1] && strchr("1248", kind[1]) && !kind[2] &&
                    !(kind[0] == 'f' && kind[1] != '4' && kind[1] != '8');
    if (!good) {
        sea_obj_release(&s->obj);
        return sea_make_error_str(vm, "ndarray: unsupported dtype in the data");
    }
    int64_t shape[MAXD];
    int nd = shape_arg(vm, args[3], shape, "from_bytes");
    bool big = argc > 4 && sea_is_truthy(args[4]);
    bool fortran = argc > 5 && sea_is_truthy(args[5]);
    int w = kind_size(kind);
    int64_t n = prod(shape, nd);
    if (off < 0 || off + n * w > (int64_t)s->len) {
        sea_obj_release(&s->obj);
        return sea_make_error_str(vm, "ndarray: the data is shorter than its shape says");
    }
    int dt = kind[0] == 'c' ? DT_C128 : kind[0] == 'f' ? (kind[1] == '4' ? DT_F32 : DT_F64) : kind[0] == 'b' ? DT_BOOL : DT_INT;
    ND o;
    int64_t sh[MAXD];
    /* Fortran order: read as the reversed shape, then transpose */
    for (int i = 0; i < nd; i++) sh[i] = fortran ? shape[nd - 1 - i] : shape[i];
    SeaValue r = nd_new(vm, dt, nd, sh, &o);
    const unsigned char *src = (const unsigned char *)s->data + off;
    for (int64_t i = 0; i < n; i++) {
        unsigned char b[16];
        if (kind[0] == 'c') {
            /* two floats, each in the file's byte order */
            int h = w / 2;
            for (int part = 0; part < 2; part++)
                for (int k = 0; k < h; k++) b[part * h + k] = big ? src[i * w + part * h + h - 1 - k] : src[i * w + part * h + k];
            double re, im;
            if (h == 8) { memcpy(&re, b, 8); memcpy(&im, b + 8, 8); }
            else { float f[2]; memcpy(f, b, 8); re = f[0]; im = f[1]; }
            ((zcplx *)o.p)[i] = CMPLX(re, im);
            continue;
        }
        for (int k = 0; k < w; k++) b[k] = big ? src[i * w + w - 1 - k] : src[i * w + k];
        if (kind[0] == 'f') {
            double x;
            if (w == 8) {
                memcpy(&x, b, 8);
                ((double *)o.p)[i] = x;
            } else {
                memcpy(((float *)o.p) + i, b, 4);
            }
        } else {
            uint64_t u = 0;
            for (int k = 0; k < w; k++) u |= (uint64_t)b[k] << (8 * k);
            int64_t x = (int64_t)u;
            if (kind[0] == 'i' && w < 8 && (u >> (8 * w - 1)) & 1) x = (int64_t)(u | (~UINT64_C(0) << (8 * w)));
            if (kind[0] == 'b') x = u != 0;
            ((int64_t *)o.p)[i] = x;
        }
    }
    sea_obj_release(&s->obj);
    if (fortran && nd > 1) {
        int64_t tsh[MAXD], tst[MAXD];
        for (int i = 0; i < nd; i++) { tsh[i] = o.shape[nd - 1 - i]; tst[i] = o.strides[nd - 1 - i]; }
        SeaValue v = view(&o, nd, tsh, tst, 0);
        sea_obj_release(sea_as_obj(r));
        return v;
    }
    return r;
}

/* f32_shortest(a) → a float64 copy of a float32 array, each value the
 * shortest decimal that rounds back to the same float32 (0.6, not
 * 0.60000002384): what printing shows */
static SeaValue n_f32_shortest(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !is_nd(args[0])) FAIL("ndarray.f32_shortest: expected an array");
    ND a, c, o;
    nd_read(vm, args[0], &a, "f32_shortest");
    Tmp t = { 0 };
    tmp_hold(&t, copy_as(vm, &a, DT_F32, &c));
    SeaValue r = nd_raw(vm, DT_F64, a.nd, a.shape, &o);
    for (int64_t i = 0; i < c.size; i++) {
        float x = ((float *)c.p)[i];
        double best = x;
        if (isfinite(x)) {
            char buf[40];
            for (int p = 1; p <= 9; p++) {
                snprintf(buf, sizeof buf, "%.*g", p, (double)x);
                double y = strtod(buf, NULL);
                if ((float)y == x) { best = y; break; }
            }
        }
        ((double *)o.p)[i] = best;
    }
    tmp_free(&t);
    return r;
}

/* ---- FFT -------------------------------------------------------------------------------- */

/* a transform of one length: radix-2 for powers of two, a direct DFT for
 * other short lengths, Bluestein's chirp-z (through a power-of-two
 * transform of at least 2n-1) for the rest */
typedef struct {
    int64_t n, m;          /* the length, and the power of two Bluestein uses (0: n is one) */
    zcplx *tw;             /* twiddles of the power-of-two transform (of n or m) */
    zcplx *chirp, *bf;     /* Bluestein: w_k = e^(-iπk²/n), and the transform of conj(w) */
    zcplx *buf;
    zcplx *scratch;        /* the power-of-two transform's second buffer */
} FftPlan;

static bool pow2(int64_t n) { return n > 0 && (n & (n - 1)) == 0; }

/* (the C99 product checks for NaN and infinity on every call: too slow here) */
static inline zcplx cmul(zcplx a, zcplx b) {
    double ar = creal(a), ai = cimag(a), br = creal(b), bi = cimag(b);
    return CMPLX(ar * br - ai * bi, ar * bi + ai * br);
}

/* below this length a length that is not a power of two is a direct DFT */
#define DFT_MAX 64

static zcplx *twiddles(int64_t n) {
    /* the whole circle: radix-4 passes use angles up to 3/4 of a turn. Only
     * the first eighth is computed (a million cexp calls took longer than the
     * transform); the rest follows from symmetries, exactly: w(n/4 - k) =
     * -i·conj(w(k)) and w(k + n/4) = -i·w(k) */
    zcplx *t = malloc((size_t)(n + 1) * sizeof(zcplx));
    if (n < 8) {
        for (int64_t k = 0; k < n; k++) t[k] = cexp(CMPLX(0, -2 * M_PI * (double)k / (double)n));
        return t;
    }
    int64_t q = n / 4, e = n / 8;
    for (int64_t k = 0; k <= e; k++) {
        double s, c;
        double ang = 2 * M_PI * (double)k / (double)n;
        s = sin(ang);        /* sincos() is a GNU extension; the compiler fuses the pair anyway */
        c = cos(ang);
        t[k] = CMPLX(c, -s);
    }
    for (int64_t k = 1; k < e; k++) t[q - k] = CMPLX(-cimag(t[k]), -creal(t[k]));   /* -i·conj(w) */
    for (int64_t k = q; k < n; k++) t[k] = CMPLX(cimag(t[k - q]), -creal(t[k - q]));  /* -i·w */
    return t;
}

/* i·z */
static inline zcplx times_i(zcplx z) { return CMPLX(-cimag(z), creal(z)); }

/* in place, forward, n a power of two (scratch: n more elements). Stockham's
 * autosort, radix 4 (and one radix-2 pass when log2 n is odd): each pass
 * reads and writes the memory in order between two buffers, with no
 * bit-reversal permutation; a large transform is bound by memory traffic,
 * so fewer passes (radix 4: half as many) is what makes it faster */
static void fft2(zcplx *x, int64_t n, const zcplx *tw, zcplx *scratch) {
    zcplx *a = x, *b = scratch;
    int64_t s = 1;   /* the stride: how many interleaved transforms */
    int64_t len = n;
    for (; len >= 4; len /= 4) {
        int64_t m = len / 4, step = n / len;
        for (int64_t p = 0; p < m; p++) {
            zcplx w1 = tw[p * step], w2 = tw[2 * p * step], w3 = tw[3 * p * step];   /* e^(-2πi kp/len) */
            const zcplx *x0 = a + s * p, *x1 = a + s * (p + m), *x2 = a + s * (p + 2 * m), *x3 = a + s * (p + 3 * m);
            zcplx *y0 = b + s * 4 * p, *y1 = y0 + s, *y2 = y0 + 2 * s, *y3 = y0 + 3 * s;
            for (int64_t q = 0; q < s; q++) {
                zcplx c0 = x0[q], c1 = x1[q], c2 = x2[q], c3 = x3[q];
                zcplx apc = c0 + c2, amc = c0 - c2, bpd = c1 + c3, jbmd = times_i(c1 - c3);
                y0[q] = apc + bpd;
                y1[q] = cmul(amc - jbmd, w1);
                y2[q] = cmul(apc - bpd, w2);
                y3[q] = cmul(amc + jbmd, w3);
            }
        }
        zcplx *t = a;
        a = b;
        b = t;
        s *= 4;
    }
    if (len == 2) {
        /* the last pass, radix 2 (its twiddle is 1) */
        for (int64_t q = 0; q < s; q++) {
            zcplx u = a[q], v = a[q + s];
            b[q] = u + v;
            b[q + s] = u - v;
        }
        zcplx *t = a;
        a = b;
        b = t;
    }
    if (a != x) memcpy(x, a, (size_t)n * sizeof(zcplx));
}

static void plan_init(FftPlan *p, int64_t n) {
    memset(p, 0, sizeof *p);
    p->n = n;
    if (n <= 1) return;
    if (pow2(n)) {
        p->tw = twiddles(n);
        p->scratch = malloc((size_t)n * sizeof(zcplx));
        return;
    }
    if (n <= DFT_MAX) {
        /* all n roots of unity, and a buffer */
        p->tw = malloc((size_t)n * sizeof(zcplx));
        for (int64_t k = 0; k < n; k++) p->tw[k] = cexp(CMPLX(0, -2 * M_PI * (double)k / (double)n));
        p->buf = malloc((size_t)n * sizeof(zcplx));
        return;
    }
    int64_t m = 1;
    while (m < 2 * n - 1) m <<= 1;
    p->m = m;
    p->tw = twiddles(m);
    p->chirp = malloc((size_t)n * sizeof(zcplx));
    for (int64_t k = 0; k < n; k++) {
        /* k² mod 2n keeps the angle exact for large k */
        int64_t k2 = (int64_t)(((__int128)k * k) % (2 * n));
        p->chirp[k] = cexp(CMPLX(0, -M_PI * (double)k2 / (double)n));
    }
    p->bf = calloc((size_t)m, sizeof(zcplx));
    p->bf[0] = conj(p->chirp[0]);
    for (int64_t k = 1; k < n; k++) p->bf[k] = p->bf[m - k] = conj(p->chirp[k]);
    p->scratch = malloc((size_t)m * sizeof(zcplx));
    fft2(p->bf, m, p->tw, p->scratch);
    p->buf = malloc((size_t)m * sizeof(zcplx));
}

static void plan_free(FftPlan *p) { free(p->tw); free(p->chirp); free(p->bf); free(p->buf); free(p->scratch); }

/* x (n elements) transformed in place, forward */
static void plan_run(FftPlan *p, zcplx *x) {
    int64_t n = p->n;
    if (n <= 1) return;
    if (pow2(n)) { fft2(x, n, p->tw, p->scratch); return; }
    if (!p->m) {
        zcplx *y = p->buf;
        for (int64_t k = 0; k < n; k++) {
            zcplx s = x[0];
            for (int64_t j = 1; j < n; j++) s += cmul(x[j], p->tw[(j * k) % n]);
            y[k] = s;
        }
        memcpy(x, y, (size_t)n * sizeof(zcplx));
        return;
    }
    int64_t m = p->m;
    zcplx *a = p->buf;
    for (int64_t k = 0; k < n; k++) a[k] = cmul(x[k], p->chirp[k]);
    for (int64_t k = n; k < m; k++) a[k] = 0;
    fft2(a, m, p->tw, p->scratch);
    for (int64_t k = 0; k < m; k++) a[k] = conj(cmul(a[k], p->bf[k]));
    /* the inverse transform: conj, forward, conj, / m */
    fft2(a, m, p->tw, p->scratch);
    for (int64_t k = 0; k < n; k++) x[k] = cmul(conj(a[k]), p->chirp[k]) / (double)m;
}

typedef struct { FftPlan plan; zcplx *line; bool inverse; double scale; } FftCtx;

static void line_fft(void *ctx, char *pa, int64_t sa, char *pb, int64_t sb, int64_t n, int64_t k) {
    (void)pb; (void)sb; (void)k;
    FftCtx *c = ctx;
    zcplx *x = (zcplx *)pa, *l = c->line;
    for (int64_t i = 0; i < n; i++) l[i] = c->inverse ? conj(x[i * sa]) : x[i * sa];
    plan_run(&c->plan, l);
    for (int64_t i = 0; i < n; i++) x[i * sa] = (c->inverse ? conj(l[i]) : l[i]) * c->scale;
}

/* fft(a, n, axis, inverse, norm) → complex128: the discrete Fourier
 * transform along one axis, a cut or padded with zeros to n there; norm
 * "backward" (the default: the inverse divides by n), "ortho" or "forward" */
static SeaValue n_fft(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) FAIL("ndarray.fft: expected an array");
    Tmp t = { 0 };
    ND a;
    opnd(vm, args[0], &a, &t, "fft");
    if (a.nd == 0) FAIL("ndarray.fft: an array with no dimensions has no transform");
    int axis = norm_axis(vm, argc > 2 && !sea_is_nil(args[2]) ? iv(args[2]) : -1, a.nd, "fft");
    int64_t n = argc > 1 && !sea_is_nil(args[1]) ? iv(args[1]) : a.shape[axis];
    if (n < 1) FAIL("ndarray.fft: invalid number of data points (%lld)", (long long)n);
    bool inverse = argc > 3 && sea_is_truthy(args[3]);
    int mode = 0;   /* 0 backward, 1 ortho, 2 forward */
    if (argc > 4 && !sea_is_nil(args[4])) {
        if (str_eq(args[4], "ortho")) mode = 1;
        else if (str_eq(args[4], "forward")) mode = 2;
        else if (!str_eq(args[4], "backward")) FAIL("ndarray.fft: norm is \"backward\", \"ortho\" or \"forward\"");
    }
    int64_t shape[MAXD];
    memcpy(shape, a.shape, sizeof(int64_t) * a.nd);
    shape[axis] = n;
    ND o;
    SeaValue r = nd_new(vm, DT_C128, a.nd, shape, &o);
    /* a's first min(n, len) elements along axis into the zeroed result */
    int64_t keep = a.shape[axis] < n ? a.shape[axis] : n;
    ND dst = o, src = a;
    dst.shape[axis] = keep;
    src.shape[axis] = keep;
    assign(vm, &dst, &src, "fft");
    FftCtx c;
    plan_init(&c.plan, n);
    c.line = malloc((size_t)n * sizeof(zcplx));
    c.inverse = inverse;
    c.scale = mode == 1 ? 1 / sqrt((double)n) : (mode == 0) == inverse ? 1 / (double)n : 1;
    if (o.size > 0) lines2(&o, NULL, axis, line_fft, &c);
    plan_free(&c.plan);
    free(c.line);
    tmp_free(&t);
    return r;
}

/* real(a), imag(a): views of a complex array's parts (float64, writable);
 * of a real array: a itself, and zeros */
static SeaValue complex_part(SeaVM *vm, int argc, SeaValue *args, int part, const char *fn) {
    if (argc < 1) FAIL("ndarray.%s: expected an array", fn);
    Tmp t = { 0 };
    ND a;
    opnd(vm, args[0], &a, &t, fn);
    SeaValue r;
    if (a.dt == DT_C128 && is_nd(args[0])) {
        int64_t st[MAXD];
        for (int i = 0; i < a.nd; i++) st[i] = a.strides[i] * 2;
        r = wrap(a.data, DT_F64, a.nd, a.shape, st, a.off * 2 + part);
    } else if (a.dt == DT_C128) {
        zcplx z = *(zcplx *)a.p;
        r = sea_number_val(part ? cimag(z) : creal(z));
    } else if (part == 0) {
        r = sea_val_ref(args[0]);
    } else {
        ND o;
        r = finish(nd_new(vm, a.dt == DT_F32 ? DT_F32 : DT_F64, a.nd, a.shape, &o), &o);
    }
    tmp_free(&t);
    return r;
}
static SeaValue n_real(SeaVM *vm, int argc, SeaValue *args) { return complex_part(vm, argc, args, 0, "real"); }
static SeaValue n_imag(SeaVM *vm, int argc, SeaValue *args) { return complex_part(vm, argc, args, 1, "imag"); }

/* ---- module ----------------------------------------------------------------------------- */

/* init(methods): the table copied into every array */
static SeaValue n_init(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_table(args[0])) FAIL("ndarray.init: expected the methods table");
    SeaTable *p = sea_table_new(TABLE_MAP);
    sea_table_merge(p, SEA_AS_TABLE(args[0]));
    for (int f = 0; f < F_COUNT; f++) sea_table_set(p, fkey[f], SEA_FALSE_VAL);
    sea_obj_mark_shared(&p->obj);
    /* the method names and functions live as long as the program: pinned,
     * copying them into every new array costs no reference counting */
    for (uint32_t i = 0; i < p->cap; i++) {
        SeaTableEntry *e = &p->entries[i];
        if (sea_is_obj(e->key)) sea_obj_pin(sea_as_obj(e->key));
        if (sea_is_obj(e->value)) sea_obj_pin(sea_as_obj(e->value));
    }
    if (proto) sea_obj_release(&proto->obj);
    proto = p;
    return SEA_NIL_VAL;
}

static SeaValue n_is_ndarray(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    return sea_bool_val(argc > 0 && is_nd(args[0]));
}

/* is_contiguous(a): whether a's elements are in C order with no gaps */
static SeaValue n_contiguous(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !is_nd(args[0])) return SEA_FALSE_VAL;
    ND a;
    nd_read(vm, args[0], &a, "is_contiguous");
    return sea_bool_val(is_contig(&a));
}

typedef struct { const char *name; SeaNativeFn fn; } Reg;

static const Reg REG[] = {
    { "init", n_init }, { "is_ndarray", n_is_ndarray }, { "is_contiguous", n_contiguous },
    { "array", n_array }, { "full", n_full }, { "arange", n_arange }, { "linspace", n_linspace },
    { "eye", n_eye }, { "from_data", n_from_data },
    { "add", n_add }, { "subtract", n_subtract }, { "multiply", n_multiply }, { "divide", n_divide },
    { "floor_divide", n_floor_divide }, { "mod", n_mod }, { "power", n_power }, { "maximum", n_maximum },
    { "minimum", n_minimum }, { "fmax", n_fmax }, { "fmin", n_fmin }, { "arctan2", n_arctan2 },
    { "hypot", n_hypot }, { "copysign", n_copysign }, { "equal", n_equal }, { "not_equal", n_not_equal },
    { "less", n_less }, { "less_equal", n_less_equal }, { "greater", n_greater },
    { "greater_equal", n_greater_equal }, { "logical_and", n_logical_and }, { "logical_or", n_logical_or },
    { "logical_xor", n_logical_xor }, { "bitwise_and", n_bitwise_and }, { "bitwise_or", n_bitwise_or },
    { "bitwise_xor", n_bitwise_xor }, { "left_shift", n_left_shift }, { "right_shift", n_right_shift },
    { "negative", n_negative }, { "absolute", n_absolute }, { "sign", n_sign }, { "square", n_square },
    { "sqrt", n_sqrt }, { "cbrt", n_cbrt }, { "reciprocal", n_reciprocal }, { "exp", n_exp }, { "exp2", n_exp2 },
    { "expm1", n_expm1 }, { "log", n_log }, { "log2", n_log2 }, { "log10", n_log10 }, { "log1p", n_log1p },
    { "sin", n_sin }, { "cos", n_cos }, { "tan", n_tan }, { "arcsin", n_arcsin }, { "arccos", n_arccos },
    { "arctan", n_arctan }, { "sinh", n_sinh }, { "cosh", n_cosh }, { "tanh", n_tanh },
    { "arcsinh", n_arcsinh }, { "arccosh", n_arccosh }, { "arctanh", n_arctanh }, { "floor", n_floor },
    { "ceil", n_ceil }, { "trunc", n_trunc }, { "rint", n_rint }, { "isnan", n_isnan }, { "isinf", n_isinf },
    { "isfinite", n_isfinite }, { "logical_not", n_logical_not }, { "signbit", n_signbit },
    { "invert", n_invert }, { "where", n_where },
    { "sum", n_sum }, { "nansum", n_nansum }, { "prod", n_prod }, { "min", n_min }, { "max", n_max },
    { "any", n_any }, { "all", n_all }, { "mean", n_mean }, { "var", n_var }, { "std", n_std },
    { "nanmean", n_nanmean }, { "argmax", n_argmax }, { "argmin", n_argmin }, { "cumsum", n_cumsum },
    { "cumprod", n_cumprod }, { "sort", n_sort }, { "argsort", n_argsort }, { "unique", n_unique },
    { "searchsorted", n_searchsorted },
    { "matmul", n_matmul }, { "det", n_det }, { "solve", n_solve }, { "inv", n_inv }, { "cholesky", n_cholesky },
    { "qr", n_qr }, { "eigh", n_eigh }, { "svd", n_svd },
    { "rng_seed", n_rng_seed }, { "rng_draw", n_rng_draw }, { "rng_shuffle", n_rng_shuffle },
    { "reshape", n_reshape }, { "transpose", n_transpose }, { "swapaxes", n_swapaxes }, { "squeeze", n_squeeze },
    { "expand_dims", n_expand_dims }, { "broadcast_to", n_broadcast_to }, { "flip", n_flip },
    { "as_strided", n_as_strided }, { "f32_shortest", n_f32_shortest },
    { "conjugate", n_conjugate }, { "angle", n_angle }, { "real", n_real }, { "imag", n_imag }, { "eig", n_eig },
    { "fft", n_fft },
    { "diagonal", n_diagonal }, { "copy", n_copy }, { "astype", n_astype }, { "ravel", n_ravel },
    { "ascontiguousarray", n_ascontiguous }, { "tolist", n_tolist }, { "fill", n_fill },
    { "concatenate", n_concatenate }, { "stack", n_stack }, { "take", n_take }, { "get", n_get }, { "set", n_set },
    { "nonzero", n_nonzero }, { "to_bytes", n_to_bytes }, { "from_bytes", n_from_bytes },
};

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    if (!sea_is_obj(dt_str[0]))
        for (int i = 0; i < DT_COUNT; i++) {
            SeaString *s = sea_string_intern(DT_NAME[i], strlen(DT_NAME[i]));
            sea_obj_mark_shared(&s->obj);
            sea_obj_pin(&s->obj);
            dt_str[i] = sea_obj_val(&s->obj);
        }
    if (!sea_is_obj(fkey[0]))
        for (int i = 0; i < F_COUNT; i++) {
            SeaString *s = sea_string_intern(FIELD[i], strlen(FIELD[i]));
            sea_obj_mark_shared(&s->obj);
            sea_obj_pin(&s->obj);
            fkey[i] = sea_obj_val(&s->obj);
        }
    SeaTable *mod = sea_mod_new();
    for (size_t i = 0; i < sizeof REG / sizeof REG[0]; i++) sea_mod_set(mod, REG[i].name, REG[i].fn);
    return sea_obj_val(&mod->obj);
}
