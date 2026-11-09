/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Module list: helpers over arrays (and positional tables {a, b}, int_array,
 * float_array) and over the keys and values of tables, maps and structs —
 * what Python's built-ins and list comprehensions do: map, filter, reduce,
 * sum, min, max, find, index, contains, any, all, count, unique, slice,
 * concat, flatten, reversed, chunk, zip, range, group_by, count_by, keys,
 * values, copy — and counters: inc, most_common.
 *
 * Functions return new arrays and never change their input. Callbacks get
 * (value, index) — a Sea function may take just the value. A panic in a
 * callback stops the function (vm->panic_propagating) after it releases
 * what it built.
 */

#include "sea_stdlib.h"
#include "sea_vm.h"
#include "sea_string.h"
#include "sea_rope.h"
#include "sea_table.h"
#include "sea_array.h"
#include "sea_struct.h"
#include "sea_map.h"
#include "sea_typed_array.h"
#include "sea_memory.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- the elements of a sequence ---- */

typedef struct {
    SeaValue *v;        /* borrowed values (or tmp) */
    uint32_t  n;
    SeaValue *tmp;      /* allocated for tables and typed arrays */
} Items;

static void items_free(Items *it) {
    if (it->tmp) free(it->tmp);
    it->tmp = NULL;
}

/* The elements of an array, a positional table, an int_array, a float_array or a float32_array */
static Items items(SeaVM *vm, const char *fn, SeaValue v) {
    Items it = {0};
    if (sea_is_array(v)) {
        it.v = SEA_AS_ARRAY(v)->data;
        it.n = SEA_AS_ARRAY(v)->len;
        return it;
    }
    if (sea_obj_is_type(v, OBJ_INT_ARRAY) || sea_obj_is_type(v, OBJ_FLOAT_ARRAY)) {
        bool ints = sea_obj_is_type(v, OBJ_INT_ARRAY);
        uint32_t n = ints ? ((SeaIntArray*)sea_as_obj(v))->len : ((SeaFloatArray*)sea_as_obj(v))->len;
        it.tmp = (SeaValue*)malloc(sizeof(SeaValue) * (n ? n : 1));
        for (uint32_t i = 0; i < n; i++)
            it.tmp[i] = ints ? sea_int_val(((SeaIntArray*)sea_as_obj(v))->data[i])
                             : sea_number_val(((SeaFloatArray*)sea_as_obj(v))->data[i]);
        it.v = it.tmp;
        it.n = n;
        return it;
    }
    if (sea_obj_is_type(v, OBJ_FLOAT32_ARRAY)) {
        SeaFloat32Array *fa = (SeaFloat32Array*)sea_as_obj(v);
        it.tmp = (SeaValue*)malloc(sizeof(SeaValue) * (fa->len ? fa->len : 1));
        for (uint32_t i = 0; i < fa->len; i++) it.tmp[i] = sea_number_val((double)fa->data[i]);
        it.v = it.tmp;
        it.n = fa->len;
        return it;
    }
    if (sea_obj_is_type(v, OBJ_TABLE)) {
        SeaTable *t = (SeaTable*)sea_as_obj(v);
        uint32_t n = t->count;
        it.tmp = (SeaValue*)malloc(sizeof(SeaValue) * (n ? n : 1));
        uint32_t k = 0;
        SeaValue e;
        while (k < n && sea_table_get(t, sea_int_val(k), &e)) it.tmp[k++] = e;
        if (k != n) {
            free(it.tmp);
            sea_vm_error_noreturn(vm, "list.%s: expected an array or a positional table, got a table with other keys", fn);
        }
        it.v = it.tmp;
        it.n = n;
        return it;
    }
    sea_vm_error_noreturn(vm, "list.%s: expected an array, got %s", fn, sea_type_name(v));
}

static bool is_fn(SeaValue v) {
    return sea_obj_is_type(v, OBJ_CLOSURE);
}

static void need_fn(SeaVM *vm, const char *fn, int argc, SeaValue *args, int at) {
    if (argc <= at || !is_fn(args[at]))
        sea_vm_error_noreturn(vm, "list.%s: expected a function as argument %d", fn, at + 1);
}

static void release(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

/* fn(v, i) → owned result; *stop when a panic is propagating */
static SeaValue call2(SeaVM *vm, SeaValue fn, SeaValue v, uint32_t i, bool *stop) {
    SeaValue a[2] = { v, sea_int_val(i) };
    SeaValue r = sea_vm_call(vm, fn, a, 2);
    if (vm->panic_propagating) {
        release(r);
        *stop = true;
        return SEA_NIL_VAL;
    }
    return r;
}

static SeaValue arr_val(SeaArray *a) { return sea_obj_val(&a->obj); }

static SeaValue fail_array(SeaArray *a) {
    sea_obj_release(&a->obj);
    return SEA_NIL_VAL;
}

/* == as Sea compares: numbers by value, strings by content, the rest by identity */
static bool same(SeaValue a, SeaValue b) {
    bool sa = sea_is_string(a) || sea_is_rope(a), sb = sea_is_string(b) || sea_is_rope(b);
    if (sa && sb) {
        SeaString *x = sea_value_get_flat_string(a), *y = sea_value_get_flat_string(b);
        bool eq = x && y && x->len == y->len && memcmp(x->data, y->data, x->len) == 0;
        if (x) sea_obj_release(&x->obj);
        if (y) sea_obj_release(&y->obj);
        return eq;
    }
    return sea_val_equal(a, b);
}

/* ---- transforming ---- */

/* list.map(a, fn) → [fn(v, i) for each element] */
static SeaValue list_map(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.map: expected an array and a function");
    need_fn(vm, "map", argc, args, 1);
    Items it = items(vm, "map", args[0]);
    SeaArray *out = sea_array_new(it.n);
    bool stop = false;
    for (uint32_t i = 0; i < it.n && !stop; i++) {
        SeaValue r = call2(vm, args[1], it.v[i], i, &stop);
        if (!stop) sea_array_push_move(out, r);
    }
    items_free(&it);
    return stop ? fail_array(out) : arr_val(out);
}

/* list.filter(a, fn) → the elements for which fn(v, i) is true */
static SeaValue list_filter(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.filter: expected an array and a function");
    need_fn(vm, "filter", argc, args, 1);
    Items it = items(vm, "filter", args[0]);
    SeaArray *out = sea_array_new(it.n < 16 ? it.n : 16);
    bool stop = false;
    for (uint32_t i = 0; i < it.n && !stop; i++) {
        SeaValue r = call2(vm, args[1], it.v[i], i, &stop);
        if (!stop && sea_is_truthy(r)) sea_array_push(out, it.v[i]);
        release(r);
    }
    items_free(&it);
    return stop ? fail_array(out) : arr_val(out);
}

/* list.reduce(a, fn [, init]) → fn(...fn(fn(init, a[0], 0), a[1], 1)...) */
static SeaValue list_reduce(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.reduce: expected an array, a function and an initial value");
    need_fn(vm, "reduce", argc, args, 1);
    Items it = items(vm, "reduce", args[0]);
    uint32_t i = 0;
    SeaValue acc;
    if (argc >= 3) {
        acc = sea_val_ref(args[2]);
    } else {
        if (it.n == 0) {
            items_free(&it);
            sea_vm_error_noreturn(vm, "list.reduce: an empty array and no initial value");
        }
        acc = sea_val_ref(it.v[0]);
        i = 1;
    }
    for (; i < it.n; i++) {
        SeaValue a[3] = { acc, it.v[i], sea_int_val(i) };
        SeaValue r = sea_vm_call(vm, args[1], a, 3);
        release(acc);
        acc = r;
        if (vm->panic_propagating) break;
    }
    items_free(&it);
    return acc;
}

/* ---- searching ---- */

/* list.find(a, fn) → the first v with fn(v, i) true, and its index | nil */
static SeaValue list_find(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.find: expected an array and a function");
    need_fn(vm, "find", argc, args, 1);
    Items it = items(vm, "find", args[0]);
    bool stop = false;
    for (uint32_t i = 0; i < it.n && !stop; i++) {
        SeaValue r = call2(vm, args[1], it.v[i], i, &stop);
        bool hit = !stop && sea_is_truthy(r);
        release(r);
        if (hit) {
            SeaValue v = sea_val_ref(it.v[i]);
            items_free(&it);
            return v;
        }
    }
    items_free(&it);
    return SEA_NIL_VAL;
}

/* list.index(a, x) → the index of the first element == x | nil */
static SeaValue list_index(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) sea_vm_error_noreturn(vm, "list.index: expected an array and a value");
    Items it = items(vm, "index", args[0]);
    for (uint32_t i = 0; i < it.n; i++)
        if (same(it.v[i], args[1])) {
            items_free(&it);
            return sea_int_val(i);
        }
    items_free(&it);
    return SEA_NIL_VAL;
}

/* list.contains(a, x) → whether an element == x */
static SeaValue list_contains(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) sea_vm_error_noreturn(vm, "list.contains: expected an array and a value");
    Items it = items(vm, "contains", args[0]);
    bool found = false;
    for (uint32_t i = 0; i < it.n && !found; i++) found = same(it.v[i], args[1]);
    items_free(&it);
    return sea_bool_val(found);
}

/* any / all / count: fn(v, i) over the elements (count also takes a value) */
static SeaValue predicate(SeaVM *vm, const char *name, int argc, SeaValue *args, int mode) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.%s: expected an array and a function", name);
    bool by_value = mode == 2 && argc >= 2 && !is_fn(args[1]);
    if (!by_value) need_fn(vm, name, argc, args, 1);
    Items it = items(vm, name, args[0]);
    int64_t hits = 0;
    bool stop = false;
    for (uint32_t i = 0; i < it.n && !stop; i++) {
        bool t;
        if (by_value) {
            t = same(it.v[i], args[1]);
        } else {
            SeaValue r = call2(vm, args[1], it.v[i], i, &stop);
            t = !stop && sea_is_truthy(r);
            release(r);
        }
        if (t) hits++;
        if (mode == 0 && t) break;                 /* any */
        if (mode == 1 && !t && !stop) { items_free(&it); return SEA_FALSE_VAL; }   /* all */
    }
    uint32_t n = it.n;
    items_free(&it);
    if (stop) return SEA_NIL_VAL;
    if (mode == 0) return sea_bool_val(hits > 0);
    if (mode == 1) return sea_bool_val(true);
    (void)n;
    return sea_int_val(hits);
}

static SeaValue list_any(SeaVM *vm, int argc, SeaValue *args) { return predicate(vm, "any", argc, args, 0); }
static SeaValue list_all(SeaVM *vm, int argc, SeaValue *args) { return predicate(vm, "all", argc, args, 1); }
/* list.count(a, fn_or_value) → how many elements fn(v, i) accepts / are == value */
static SeaValue list_count(SeaVM *vm, int argc, SeaValue *args) { return predicate(vm, "count", argc, args, 2); }

/* ---- numbers ---- */

/* list.sum(a [, fn]) → the sum (an int while every term is an int; 0 when empty) */
static SeaValue list_sum(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.sum: expected an array");
    Items it = items(vm, "sum", args[0]);
    bool use_fn = argc >= 2 && is_fn(args[1]);
    int64_t isum = 0;
    double fsum = 0;
    bool floating = false, stop = false;
    for (uint32_t i = 0; i < it.n && !stop; i++) {
        SeaValue v = use_fn ? call2(vm, args[1], it.v[i], i, &stop) : it.v[i];
        if (stop) break;
        if (sea_is_int(v)) {
            if (floating) fsum += (double)sea_as_int(v); else isum += sea_as_int(v);
        } else if (sea_is_number(v)) {
            if (!floating) { fsum = (double)isum; floating = true; }
            fsum += sea_as_number(v);
        } else {
            const char *t = sea_type_name(v);
            if (use_fn) release(v);
            items_free(&it);
            sea_vm_error_noreturn(vm, "list.sum: element %u is %s, not a number", i, t);
        }
        if (use_fn) release(v);
    }
    items_free(&it);
    if (stop) return SEA_NIL_VAL;
    return floating ? sea_number_val(fsum) : sea_int_val(isum);
}

/* min / max: the element with the smallest / largest key (the element itself,
 * or fn(v, i)); numbers or strings; nil when empty */
static SeaValue extreme(SeaVM *vm, const char *name, int argc, SeaValue *args, int sign) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.%s: expected an array", name);
    Items it = items(vm, name, args[0]);
    bool use_fn = argc >= 2 && is_fn(args[1]);
    SeaValue best = SEA_NIL_VAL, best_key = SEA_NIL_VAL;
    bool stop = false, have = false;
    for (uint32_t i = 0; i < it.n && !stop; i++) {
        SeaValue k = use_fn ? call2(vm, args[1], it.v[i], i, &stop) : sea_val_ref(it.v[i]);
        if (stop) break;
        if (!have) {
            best = it.v[i];
            best_key = k;
            have = true;
            continue;
        }
        bool ok;
        int c = sea_sort_natural(k, best_key, &ok);
        if (!ok) {
            const char *ta = sea_type_name(k), *tb = sea_type_name(best_key);
            release(k);
            release(best_key);
            items_free(&it);
            sea_vm_error_noreturn(vm, "list.%s: cannot compare %s with %s", name, ta, tb);
        }
        if (c * sign > 0) {
            release(best_key);
            best = it.v[i];
            best_key = k;
        } else {
            release(k);
        }
    }
    if (have && it.n == 1) {
        /* a single element: still check it can be compared */
        bool ok;
        sea_sort_natural(best_key, best_key, &ok);
        if (!ok) {
            const char *t = sea_type_name(best_key);
            release(best_key);
            items_free(&it);
            sea_vm_error_noreturn(vm, "list.%s: cannot compare %s values", name, t);
        }
    }
    release(best_key);
    SeaValue r = stop || !have ? SEA_NIL_VAL : sea_val_ref(best);
    items_free(&it);
    return r;
}

static SeaValue list_min(SeaVM *vm, int argc, SeaValue *args) { return extreme(vm, "min", argc, args, -1); }
static SeaValue list_max(SeaVM *vm, int argc, SeaValue *args) { return extreme(vm, "max", argc, args, 1); }

/* The elements as doubles (numbers only) — for mean, median, stdev */
static double *numbers(SeaVM *vm, const char *name, SeaValue a, uint32_t *n) {
    Items it = items(vm, name, a);
    double *x = (double*)malloc(sizeof(double) * (it.n ? it.n : 1));
    for (uint32_t i = 0; i < it.n; i++) {
        SeaValue v = it.v[i];
        if (sea_is_int(v)) x[i] = (double)sea_as_int(v);
        else if (sea_is_number(v)) x[i] = sea_as_number(v);
        else {
            const char *t = sea_type_name(v);
            free(x);
            items_free(&it);
            sea_vm_error_noreturn(vm, "list.%s: element %u is %s, not a number", name, i, t);
        }
    }
    *n = it.n;
    items_free(&it);
    return x;
}

/* list.mean(a) → the arithmetic mean (nil for []) */
static SeaValue list_mean(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.mean: expected an array");
    uint32_t n;
    double *x = numbers(vm, "mean", args[0], &n);
    double s = 0;
    for (uint32_t i = 0; i < n; i++) s += x[i];
    free(x);
    return n ? sea_number_val(s / n) : SEA_NIL_VAL;
}

static int dbl_cmp(const void *a, const void *b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* list.median(a) → the middle value (the mean of the two middle ones for an even count; nil for []) */
static SeaValue list_median(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.median: expected an array");
    uint32_t n;
    double *x = numbers(vm, "median", args[0], &n);
    if (!n) { free(x); return SEA_NIL_VAL; }
    qsort(x, n, sizeof(double), dbl_cmp);
    double m = n % 2 ? x[n / 2] : (x[n / 2 - 1] + x[n / 2]) / 2;
    free(x);
    return sea_number_val(m);
}

/* list.stdev(a) → the sample standard deviation (as Python's statistics.stdev;
 * nil with fewer than 2 elements) */
static SeaValue list_stdev(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.stdev: expected an array");
    uint32_t n;
    double *x = numbers(vm, "stdev", args[0], &n);
    if (n < 2) { free(x); return SEA_NIL_VAL; }
    double mean = 0, ss = 0;
    for (uint32_t i = 0; i < n; i++) mean += x[i];
    mean /= n;
    for (uint32_t i = 0; i < n; i++) ss += (x[i] - mean) * (x[i] - mean);
    free(x);
    return sea_number_val(sqrt(ss / (n - 1)));
}

/* list.range(n) → [0 .. n-1]; list.range(from, to [, step]) → from, from+step, ... up to
 * and including to (like a numeric for) */
static SeaValue list_range(SeaVM *vm, int argc, SeaValue *args) {
    for (int i = 0; i < argc && i < 3; i++)
        if (!sea_is_int(args[i])) sea_vm_error_noreturn(vm, "list.range: expected integers");
    if (argc < 1) sea_vm_error_noreturn(vm, "list.range: expected range(n) or range(from, to [, step])");
    int64_t from = 0, to, step = 1;
    if (argc == 1) {
        to = sea_as_int(args[0]) - 1;
    } else {
        from = sea_as_int(args[0]);
        to = sea_as_int(args[1]);
        if (argc >= 3) step = sea_as_int(args[2]);
    }
    if (step == 0) sea_vm_error_noreturn(vm, "list.range: step 0");
    int64_t count = step > 0 ? (to >= from ? (to - from) / step + 1 : 0)
                             : (to <= from ? (from - to) / -step + 1 : 0);
    if (count > 100000000) sea_vm_error_noreturn(vm, "list.range: %lld elements is too many", (long long)count);
    SeaArray *out = sea_array_new((uint32_t)count);
    for (int64_t i = 0, x = from; i < count; i++, x += step) sea_array_push(out, sea_int_val(x));
    return arr_val(out);
}

/* ---- reshaping ---- */

/* list.unique(a) → the elements without repeats (first occurrences kept; a
 * map decides, so 1 and 1.0 are different) */
static SeaValue list_unique(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.unique: expected an array");
    Items it = items(vm, "unique", args[0]);
    SeaMap *seen = sea_map_new();
    SeaArray *out = sea_array_new(it.n < 16 ? it.n : 16);
    for (uint32_t i = 0; i < it.n; i++) {
        SeaValue dummy;
        if (sea_map_get(seen, it.v[i], &dummy)) continue;
        sea_map_set(seen, it.v[i], SEA_TRUE_VAL);
        sea_array_push(out, it.v[i]);
    }
    sea_obj_release(&seen->obj);
    items_free(&it);
    return arr_val(out);
}

/* list.slice(a, from [, to]) → elements from..to-1 (to exclusive, as in
 * Python and JavaScript; negative indexes count from the end; to defaults to
 * the end) */
static SeaValue list_slice(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !sea_is_int(args[1])) sea_vm_error_noreturn(vm, "list.slice: expected an array and a start index");
    Items it = items(vm, "slice", args[0]);
    int64_t n = it.n;
    int64_t from = sea_as_int(args[1]);
    int64_t to = argc >= 3 && sea_is_int(args[2]) ? sea_as_int(args[2]) : n;
    if (from < 0) from += n;
    if (to < 0) to += n;
    if (from < 0) from = 0;
    if (to > n) to = n;
    SeaArray *out = sea_array_new(to > from ? (uint32_t)(to - from) : 0);
    for (int64_t i = from; i < to; i++) sea_array_push(out, it.v[i]);
    items_free(&it);
    return arr_val(out);
}

/* list.concat(a, b, ...) → one array with the elements of all */
static SeaValue list_concat(SeaVM *vm, int argc, SeaValue *args) {
    SeaArray *out = sea_array_new(16);
    for (int k = 0; k < argc; k++) {
        Items it = items(vm, "concat", args[k]);
        for (uint32_t i = 0; i < it.n; i++) sea_array_push(out, it.v[i]);
        items_free(&it);
    }
    return arr_val(out);
}

/* list.flatten(a) → the elements of the inner arrays, one level */
static SeaValue list_flatten(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.flatten: expected an array");
    Items it = items(vm, "flatten", args[0]);
    SeaArray *out = sea_array_new(it.n);
    for (uint32_t i = 0; i < it.n; i++) {
        if (sea_is_array(it.v[i])) {
            SeaArray *in = SEA_AS_ARRAY(it.v[i]);
            for (uint32_t j = 0; j < in->len; j++) sea_array_push(out, in->data[j]);
        } else {
            sea_array_push(out, it.v[i]);
        }
    }
    items_free(&it);
    return arr_val(out);
}

/* list.reversed(a) → a new array in reverse order (sort.reverse reverses in place) */
static SeaValue list_reversed(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.reversed: expected an array");
    Items it = items(vm, "reversed", args[0]);
    SeaArray *out = sea_array_new(it.n);
    for (uint32_t i = it.n; i > 0; i--) sea_array_push(out, it.v[i - 1]);
    items_free(&it);
    return arr_val(out);
}

/* list.copy(a) → a shallow copy */
static SeaValue list_copy(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.copy: expected an array");
    Items it = items(vm, "copy", args[0]);
    SeaArray *out = sea_array_new(it.n);
    for (uint32_t i = 0; i < it.n; i++) sea_array_push(out, it.v[i]);
    items_free(&it);
    return arr_val(out);
}

/* list.chunk(a, n) → arrays of n elements (the last may be shorter) */
static SeaValue list_chunk(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !sea_is_int(args[1]) || sea_as_int(args[1]) <= 0)
        sea_vm_error_noreturn(vm, "list.chunk: expected an array and a positive size");
    Items it = items(vm, "chunk", args[0]);
    uint32_t size = (uint32_t)sea_as_int(args[1]);
    SeaArray *out = sea_array_new(it.n / size + 1);
    for (uint32_t i = 0; i < it.n; i += size) {
        SeaArray *c = sea_array_new(size);
        for (uint32_t j = i; j < i + size && j < it.n; j++) sea_array_push(c, it.v[j]);
        sea_array_push_move(out, arr_val(c));
    }
    items_free(&it);
    return arr_val(out);
}

/* list.zip(a, b, ...) → [[a[0], b[0], ...], ...] as long as the shortest */
static SeaValue list_zip(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.zip: expected arrays");
    if (argc > 16) sea_vm_error_noreturn(vm, "list.zip: at most 16 arrays");
    Items its[16];
    uint32_t n = UINT32_MAX;
    for (int k = 0; k < argc; k++) {
        its[k] = items(vm, "zip", args[k]);
        if (its[k].n < n) n = its[k].n;
    }
    SeaArray *out = sea_array_new(n);
    for (uint32_t i = 0; i < n; i++) {
        SeaArray *row = sea_array_new((uint32_t)argc);
        for (int k = 0; k < argc; k++) sea_array_push(row, its[k].v[i]);
        sea_array_push_move(out, arr_val(row));
    }
    for (int k = 0; k < argc; k++) items_free(&its[k]);
    return arr_val(out);
}

/* group_by / count_by: a map from fn(v, i) to the elements / their number */
static SeaValue grouping(SeaVM *vm, const char *name, int argc, SeaValue *args, bool count) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.%s: expected an array and a function", name);
    need_fn(vm, name, argc, args, 1);
    Items it = items(vm, name, args[0]);
    SeaMap *out = sea_map_new();
    bool stop = false;
    for (uint32_t i = 0; i < it.n && !stop; i++) {
        SeaValue k = call2(vm, args[1], it.v[i], i, &stop);
        if (stop) break;
        SeaValue cur;
        bool have = sea_map_get(out, k, &cur);
        if (count) {
            sea_map_set(out, k, sea_int_val(have ? sea_as_int(cur) + 1 : 1));
        } else if (have) {
            sea_array_push(SEA_AS_ARRAY(cur), it.v[i]);
        } else {
            SeaArray *g = sea_array_new(4);
            sea_array_push(g, it.v[i]);
            sea_map_set(out, k, arr_val(g));
            sea_obj_release(&g->obj);
        }
        release(k);
    }
    items_free(&it);
    if (stop) {
        sea_obj_release(&out->obj);
        return SEA_NIL_VAL;
    }
    return sea_obj_val(&out->obj);
}

/* list.group_by(a, fn) → map: fn(v, i) → [the elements with that key] */
static SeaValue list_group_by(SeaVM *vm, int argc, SeaValue *args) { return grouping(vm, "group_by", argc, args, false); }
/* list.count_by(a, fn) → map: fn(v, i) → how many elements have that key */
static SeaValue list_count_by(SeaVM *vm, int argc, SeaValue *args) { return grouping(vm, "count_by", argc, args, true); }

/* ---- keys and values ---- */

static int key_rank(SeaValue k) {
    if (sea_is_int(k) || sea_is_number(k)) return 0;
    if (sea_is_string(k)) return 1;
    return 2;
}

static int pair_cmp(const void *pa, const void *pb) {
    SeaValue a = ((const SeaValue*)pa)[0], b = ((const SeaValue*)pb)[0];
    int ra = key_rank(a), rb = key_rank(b);
    if (ra != rb) return ra - rb;
    bool ok;
    if (ra < 2) return sea_sort_natural(a, b, &ok);
    return a < b ? -1 : a > b;
}

/* The (key, value) pairs of a table, map or struct: sorted by key for tables
 * and maps (numbers, then strings, then the rest), literal order for structs;
 * an array gives (index, value) */
static SeaValue pairs_of(SeaVM *vm, const char *name, SeaValue v, int which) {
    SeaValue *pairs = NULL;
    uint32_t n = 0, k = 0;
    bool sort = true;
    SeaValue key, val;
    uint32_t iter = 0;
    if (sea_obj_is_type(v, OBJ_TABLE)) {
        n = ((SeaTable*)sea_as_obj(v))->count;
        pairs = (SeaValue*)malloc(sizeof(SeaValue) * 2 * (n ? n : 1));
        while (k < n && sea_table_next((SeaTable*)sea_as_obj(v), &iter, &key, &val)) {
            pairs[2 * k] = key; pairs[2 * k + 1] = val; k++;
        }
    } else if (sea_obj_is_type(v, OBJ_MAP)) {
        n = sea_map_count((SeaMap*)sea_as_obj(v));
        pairs = (SeaValue*)malloc(sizeof(SeaValue) * 2 * (n ? n : 1));
        while (k < n && sea_map_next((SeaMap*)sea_as_obj(v), &iter, &key, &val)) {
            pairs[2 * k] = key; pairs[2 * k + 1] = val; k++;
        }
    } else if (sea_obj_is_type(v, OBJ_STRUCT)) {
        n = ((SeaStruct*)sea_as_obj(v))->field_count;
        pairs = (SeaValue*)malloc(sizeof(SeaValue) * 2 * (n ? n : 1));
        while (k < n && sea_struct_next((SeaStruct*)sea_as_obj(v), &iter, &key, &val)) {
            pairs[2 * k] = key; pairs[2 * k + 1] = val; k++;
        }
        sort = false;
    } else if (sea_is_array(v)) {
        SeaArray *a = SEA_AS_ARRAY(v);
        n = a->len;
        pairs = (SeaValue*)malloc(sizeof(SeaValue) * 2 * (n ? n : 1));
        for (; k < n; k++) { pairs[2 * k] = sea_int_val(k); pairs[2 * k + 1] = a->data[k]; }
        sort = false;
    } else {
        sea_vm_error_noreturn(vm, "list.%s: expected a table, map, struct or array, got %s", name, sea_type_name(v));
    }
    if (sort) qsort(pairs, k, 2 * sizeof(SeaValue), pair_cmp);
    SeaArray *out = sea_array_new(k);
    for (uint32_t i = 0; i < k; i++) {
        if (which == 2) {
            SeaArray *p = sea_array_new(2);
            sea_array_push(p, pairs[2 * i]);
            sea_array_push(p, pairs[2 * i + 1]);
            sea_array_push_move(out, arr_val(p));
        } else {
            sea_array_push(out, pairs[2 * i + which]);
        }
    }
    free(pairs);
    return arr_val(out);
}

/* list.inc(m, k [, n]) → the new count: m[k] = (m[k] or 0) + n (n = 1),
 * for a table, map or struct used as a counter */
static SeaValue list_inc(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) sea_vm_error_noreturn(vm, "list.inc: expected a table or map and a key");
    SeaValue m = args[0], k = args[1];
    SeaValue by = argc > 2 ? args[2] : sea_int_val(1);
    if (!sea_is_int(by) && !sea_is_number(by))
        sea_vm_error_noreturn(vm, "list.inc: the amount must be a number, got %s", sea_type_name(by));
    if (sea_is_nil(k)) sea_vm_error_noreturn(vm, "list.inc: the key is nil");
    SeaValue cur = SEA_NIL_VAL;
    bool have;
    if (sea_obj_is_type(m, OBJ_TABLE)) have = sea_table_get((SeaTable*)sea_as_obj(m), k, &cur);
    else if (sea_obj_is_type(m, OBJ_MAP)) have = sea_map_get((SeaMap*)sea_as_obj(m), k, &cur);
    else if (sea_obj_is_type(m, OBJ_STRUCT) && sea_is_string(k)) {
        /* a struct ({a = 0, b = 0}) has a fixed set of fields */
        cur = sea_struct_get((SeaStruct*)sea_as_obj(m), SEA_AS_STRING(k));
        have = true;
        if (sea_is_nil(cur))
            sea_vm_error_noreturn(vm, "list.inc: the struct has no field '%s'", SEA_AS_STRING(k)->data);
    }
    else sea_vm_error_noreturn(vm, "list.inc: expected a table, map or struct, got %s", sea_type_name(m));
    if (!have || sea_is_nil(cur)) cur = sea_int_val(0);
    SeaValue r;
    if (sea_is_int(cur) && sea_is_int(by)) {
        int64_t x = sea_as_int(cur) + sea_as_int(by);
        r = x >= -140737488355327LL && x <= 140737488355327LL ? sea_int_val(x) : sea_number_val((double)x);
    } else if (sea_is_int(cur) || sea_is_number(cur)) {
        double x = sea_is_int(cur) ? (double)sea_as_int(cur) : sea_as_number(cur);
        r = sea_number_val(x + (sea_is_int(by) ? (double)sea_as_int(by) : sea_as_number(by)));
    } else {
        sea_vm_error_noreturn(vm, "list.inc: the value of that key is a %s, not a number", sea_type_name(cur));
    }
    if (sea_obj_is_type(m, OBJ_TABLE)) sea_table_set((SeaTable*)sea_as_obj(m), k, r);
    else if (sea_obj_is_type(m, OBJ_MAP)) sea_map_set((SeaMap*)sea_as_obj(m), k, r);
    else sea_struct_set((SeaStruct*)sea_as_obj(m), SEA_AS_STRING(k), r);
    return r;
}

static int count_cmp(const void *pa, const void *pb) {
    SeaValue a = ((const SeaValue*)pa)[1], b = ((const SeaValue*)pb)[1];
    bool ok;
    int c = sea_sort_natural(b, a, &ok);      /* larger counts first */
    return c != 0 ? c : pair_cmp(pa, pb);     /* then by key */
}

/* list.most_common(m [, n]) → [[key, count], ...]: the entries of a counter
 * (a table, map or struct of numbers), largest count first, ties by key; the first
 * n only when n is given (Python's Counter.most_common) */
static SeaValue list_most_common(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || (!sea_obj_is_type(args[0], OBJ_TABLE) && !sea_obj_is_type(args[0], OBJ_MAP) && !sea_obj_is_type(args[0], OBJ_STRUCT)))
        sea_vm_error_noreturn(vm, "list.most_common: expected a table, map or struct, got %s", argc < 1 ? "nothing" : sea_type_name(args[0]));
    int64_t limit = -1;
    if (argc > 1 && !sea_is_nil(args[1])) {
        if (!sea_is_int(args[1]) || sea_as_int(args[1]) < 0)
            sea_vm_error_noreturn(vm, "list.most_common: n must be an int >= 0");
        limit = sea_as_int(args[1]);
    }
    SeaValue m = args[0], key, val;
    int t = sea_obj_is_type(m, OBJ_TABLE) ? 0 : sea_obj_is_type(m, OBJ_MAP) ? 1 : 2;
    uint32_t n = t == 0 ? ((SeaTable*)sea_as_obj(m))->count : t == 1 ? sea_map_count((SeaMap*)sea_as_obj(m))
                        : ((SeaStruct*)sea_as_obj(m))->field_count;
    SeaValue *pairs = (SeaValue*)malloc(sizeof(SeaValue) * 2 * (n ? n : 1));
    uint32_t k = 0, iter = 0;
    while (k < n && (t == 0 ? sea_table_next((SeaTable*)sea_as_obj(m), &iter, &key, &val)
                     : t == 1 ? sea_map_next((SeaMap*)sea_as_obj(m), &iter, &key, &val)
                     : sea_struct_next((SeaStruct*)sea_as_obj(m), &iter, &key, &val))) {
        if (!sea_is_int(val) && !sea_is_number(val)) {
            free(pairs);
            sea_vm_error_noreturn(vm, "list.most_common: the value of a key is a %s, not a number", sea_type_name(val));
        }
        pairs[2 * k] = key; pairs[2 * k + 1] = val; k++;
    }
    qsort(pairs, k, 2 * sizeof(SeaValue), count_cmp);
    uint32_t take = limit >= 0 && (uint64_t)limit < k ? (uint32_t)limit : k;
    SeaArray *out = sea_array_new(take);
    for (uint32_t i = 0; i < take; i++) {
        SeaArray *p = sea_array_new(2);
        sea_array_push(p, pairs[2 * i]);
        sea_array_push(p, pairs[2 * i + 1]);
        sea_array_push_move(out, arr_val(p));
    }
    free(pairs);
    return arr_val(out);
}

/* list.keys(t) → the keys (sorted for tables and maps; a struct's in order) */
static SeaValue list_keys(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.keys: expected a table, map or struct");
    return pairs_of(vm, "keys", args[0], 0);
}

/* list.values(t) → the values, in the order of list.keys(t) */
static SeaValue list_values(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.values: expected a table, map or struct");
    return pairs_of(vm, "values", args[0], 1);
}

/* list.items(t) → [[key, value], ...] in the order of list.keys(t) */
static SeaValue list_items(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "list.items: expected a table, map or struct");
    return pairs_of(vm, "items", args[0], 2);
}

void sea_lib_list_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);
    mod_set(vm, mod, "map",      list_map);
    mod_set(vm, mod, "filter",   list_filter);
    mod_set(vm, mod, "reduce",   list_reduce);
    mod_set(vm, mod, "find",     list_find);
    mod_set(vm, mod, "index",    list_index);
    mod_set(vm, mod, "contains", list_contains);
    mod_set(vm, mod, "any",      list_any);
    mod_set(vm, mod, "all",      list_all);
    mod_set(vm, mod, "count",    list_count);
    mod_set(vm, mod, "sum",      list_sum);
    mod_set(vm, mod, "min",      list_min);
    mod_set(vm, mod, "max",      list_max);
    mod_set(vm, mod, "mean",     list_mean);
    mod_set(vm, mod, "median",   list_median);
    mod_set(vm, mod, "stdev",    list_stdev);
    mod_set(vm, mod, "range",    list_range);
    mod_set(vm, mod, "unique",   list_unique);
    mod_set(vm, mod, "slice",    list_slice);
    mod_set(vm, mod, "concat",   list_concat);
    mod_set(vm, mod, "flatten",  list_flatten);
    mod_set(vm, mod, "reversed", list_reversed);
    mod_set(vm, mod, "copy",     list_copy);
    mod_set(vm, mod, "chunk",    list_chunk);
    mod_set(vm, mod, "zip",      list_zip);
    mod_set(vm, mod, "group_by", list_group_by);
    mod_set(vm, mod, "count_by", list_count_by);
    mod_set(vm, mod, "inc",      list_inc);
    mod_set(vm, mod, "most_common", list_most_common);
    mod_set(vm, mod, "keys",     list_keys);
    mod_set(vm, mod, "values",   list_values);
    mod_set(vm, mod, "items",    list_items);
    sea_vm_register_module(vm, "list", mod);
    sea_obj_release(&mod->obj);
}
