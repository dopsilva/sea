/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_typed_array.h"
#include "sea_memory.h"

/* --- SeaIntArray --- */

SeaIntArray *sea_int_array_new(uint32_t cap) {
    if (cap < 4) cap = 4;
    SeaIntArray *a = SEA_ALLOC(SeaIntArray);
    a->obj.type     = OBJ_INT_ARRAY;
    a->obj.gc = 0;
    a->obj.refcount = 1;
    a->len  = 0;
    a->cap  = cap;
    a->data = SEA_ALLOC_N(int64_t, cap);
    return a;
}

void sea_int_array_push(SeaIntArray *a, int64_t v) {
    if (a->len >= a->cap) {
        uint32_t nc = a->cap * 2;
        a->data = sea_realloc(a->data, sizeof(int64_t) * a->cap,
                                       sizeof(int64_t) * nc);
        a->cap  = nc;
    }
    a->data[a->len++] = v;
}

int64_t sea_int_array_get(SeaIntArray *a, int64_t idx) {
    if (idx < 0) idx += (int64_t)a->len;
    if (idx < 0 || (uint64_t)idx >= a->len) return 0;
    return a->data[(uint32_t)idx];
}

/* Writing past the end grows the array, filling the gap with zeros (a plain
 * array fills it with nil). */
void sea_int_array_set(SeaIntArray *a, int64_t idx, int64_t v) {
    if (idx < 0) idx += (int64_t)a->len;
    if (idx < 0 || idx > (int64_t)UINT32_MAX - 1) return;
    while ((uint64_t)idx >= a->len) sea_int_array_push(a, 0);
    a->data[(uint32_t)idx] = v;
}

void sea_int_array_free(SeaIntArray *a) {
    sea_free(a->data, sizeof(int64_t) * a->cap);
    sea_free(a, sizeof(SeaIntArray));
}

/* --- SeaFloatArray --- */

SeaFloatArray *sea_float_array_new(uint32_t cap) {
    if (cap < 4) cap = 4;
    SeaFloatArray *a = SEA_ALLOC(SeaFloatArray);
    a->obj.type     = OBJ_FLOAT_ARRAY;
    a->obj.gc = 0;
    a->obj.refcount = 1;
    a->len  = 0;
    a->cap  = cap;
    a->data = SEA_ALLOC_N(double, cap);
    return a;
}

void sea_float_array_push(SeaFloatArray *a, double v) {
    if (a->len >= a->cap) {
        uint32_t nc = a->cap * 2;
        a->data = sea_realloc(a->data, sizeof(double) * a->cap,
                                       sizeof(double) * nc);
        a->cap  = nc;
    }
    a->data[a->len++] = v;
}

double sea_float_array_get(SeaFloatArray *a, int64_t idx) {
    if (idx < 0) idx += (int64_t)a->len;
    if (idx < 0 || (uint64_t)idx >= a->len) return 0.0;
    return a->data[(uint32_t)idx];
}

void sea_float_array_set(SeaFloatArray *a, int64_t idx, double v) {
    if (idx < 0) idx += (int64_t)a->len;
    if (idx < 0 || idx > (int64_t)UINT32_MAX - 1) return;
    while ((uint64_t)idx >= a->len) sea_float_array_push(a, 0.0);
    a->data[(uint32_t)idx] = v;
}

void sea_float_array_free(SeaFloatArray *a) {
    sea_free(a->data, sizeof(double) * a->cap);
    sea_free(a, sizeof(SeaFloatArray));
}

/* --- SeaFloat32Array --- */

SeaFloat32Array *sea_float32_array_new(uint32_t cap) {
    if (cap < 4) cap = 4;
    SeaFloat32Array *a = SEA_ALLOC(SeaFloat32Array);
    a->obj.type     = OBJ_FLOAT32_ARRAY;
    a->obj.gc = 0;
    a->obj.refcount = 1;
    a->len  = 0;
    a->cap  = cap;
    a->data = SEA_ALLOC_N(float, cap);
    return a;
}

void sea_float32_array_push(SeaFloat32Array *a, double v) {
    if (a->len >= a->cap) {
        uint32_t nc = a->cap * 2;
        a->data = sea_realloc(a->data, sizeof(float) * a->cap,
                                       sizeof(float) * nc);
        a->cap  = nc;
    }
    a->data[a->len++] = (float)v;
}

double sea_float32_array_get(SeaFloat32Array *a, int64_t idx) {
    if (idx < 0) idx += (int64_t)a->len;
    if (idx < 0 || (uint64_t)idx >= a->len) return 0.0;
    return (double)a->data[(uint32_t)idx];
}

void sea_float32_array_set(SeaFloat32Array *a, int64_t idx, double v) {
    if (idx < 0) idx += (int64_t)a->len;
    if (idx < 0 || idx > (int64_t)UINT32_MAX - 1) return;
    while ((uint64_t)idx >= a->len) sea_float32_array_push(a, 0.0);
    a->data[(uint32_t)idx] = (float)v;
}

void sea_float32_array_free(SeaFloat32Array *a) {
    sea_free(a->data, sizeof(float) * a->cap);
    sea_free(a, sizeof(SeaFloat32Array));
}
