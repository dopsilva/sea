/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_stdlib.h"
#include "sea_string.h"
#include "sea_table.h"
#include "sea_memory.h"
#include <math.h>
#include <time.h>

static SeaValue math_abs(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    if (sea_is_int(args[0])) {
        int64_t v = sea_as_int(args[0]);
        return sea_int_val(v < 0 ? -v : v);
    }
    double v = sea_as_number(args[0]);
    return sea_number_val(fabs(v));
}

static SeaValue math_ceil(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    if (sea_is_int(args[0])) return args[0];
    return sea_int_val((int64_t)ceil(sea_as_number(args[0])));
}

static SeaValue math_floor(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    if (sea_is_int(args[0])) return args[0];
    return sea_int_val((int64_t)floor(sea_as_number(args[0])));
}

static SeaValue math_sqrt(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    double v = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    return sea_number_val(sqrt(v));
}

static SeaValue math_sin(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    double v = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    return sea_number_val(sin(v));
}

static SeaValue math_cos(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    double v = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    return sea_number_val(cos(v));
}

static SeaValue math_tan(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    double v = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    return sea_number_val(tan(v));
}

static SeaValue math_asin(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    double v = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    return sea_number_val(asin(v));
}

static SeaValue math_acos(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    double v = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    return sea_number_val(acos(v));
}

/* math.atan2(y, x): the name most code from other languages reaches for first */
static SeaValue math_atan2(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_NIL_VAL;
    double y = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    double x = sea_is_int(args[1]) ? (double)sea_as_int(args[1]) : sea_as_number(args[1]);
    return sea_number_val(atan2(y, x));
}

static SeaValue math_atan(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    double v = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    if (argc >= 2) {
        double y = sea_is_int(args[1]) ? (double)sea_as_int(args[1]) : sea_as_number(args[1]);
        return sea_number_val(atan2(v, y));
    }
    return sea_number_val(atan(v));
}

static SeaValue math_log(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    double v = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    if (argc >= 2) {
        double base = sea_is_int(args[1]) ? (double)sea_as_int(args[1]) : sea_as_number(args[1]);
        return sea_number_val(log(v) / log(base));
    }
    return sea_number_val(log(v));
}

static SeaValue math_log2(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    double v = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    return sea_number_val(log2(v));
}

static SeaValue math_log10(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    double v = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    return sea_number_val(log10(v));
}

static SeaValue math_pow(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_NIL_VAL;
    double a = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    double b = sea_is_int(args[1]) ? (double)sea_as_int(args[1]) : sea_as_number(args[1]);
    return sea_number_val(pow(a, b));
}

static SeaValue math_max(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaValue m = args[0];
    for (int i = 1; i < argc; i++) {
        double mv = sea_is_int(m) ? (double)sea_as_int(m) : sea_as_number(m);
        double av = sea_is_int(args[i]) ? (double)sea_as_int(args[i]) : sea_as_number(args[i]);
        if (av > mv) m = args[i];
    }
    return m;
}

static SeaValue math_min(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaValue m = args[0];
    for (int i = 1; i < argc; i++) {
        double mv = sea_is_int(m) ? (double)sea_as_int(m) : sea_as_number(m);
        double av = sea_is_int(args[i]) ? (double)sea_as_int(args[i]) : sea_as_number(args[i]);
        if (av < mv) m = args[i];
    }
    return m;
}

static uint64_t g_rand_state = 1234567890ULL;

/* One xorshift64 step on the shared state: goroutines on several threads call
 * math.random concurrently, so the update is a compare-and-swap loop (each
 * call gets its own step; a single-threaded program sees the same sequence). */
static uint64_t rand_next(void) {
    uint64_t old = __atomic_load_n(&g_rand_state, __ATOMIC_RELAXED), x;
    do {
        x = old;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    } while (!__atomic_compare_exchange_n(&g_rand_state, &old, x, true,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return x;
}

static SeaValue math_random(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    uint64_t r = rand_next();

    if (argc == 0) {
        return sea_number_val((double)r / (double)UINT64_MAX);
    } else if (argc == 1) {
        int64_t n = sea_is_int(args[0]) ? sea_as_int(args[0]) : (int64_t)sea_as_number(args[0]);
        return sea_int_val((int64_t)(r % (uint64_t)n) + 1);
    } else {
        int64_t lo = sea_is_int(args[0]) ? sea_as_int(args[0]) : (int64_t)sea_as_number(args[0]);
        int64_t hi = sea_is_int(args[1]) ? sea_as_int(args[1]) : (int64_t)sea_as_number(args[1]);
        int64_t range = hi - lo + 1;
        return sea_int_val(lo + (int64_t)(r % (uint64_t)range));
    }
}

static SeaValue math_randomseed(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc > 0) {
        uint64_t seed = sea_is_int(args[0]) ? (uint64_t)sea_as_int(args[0]) : (uint64_t)sea_as_number(args[0]);
        __atomic_store_n(&g_rand_state, seed ? seed : 1, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&g_rand_state, (uint64_t)time(NULL), __ATOMIC_RELAXED);
    }
    return SEA_NIL_VAL;
}

static SeaValue math_fmod(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_NIL_VAL;
    double a = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    double b = sea_is_int(args[1]) ? (double)sea_as_int(args[1]) : sea_as_number(args[1]);
    return sea_number_val(fmod(a, b));
}

static SeaValue math_exp(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    double v = sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]);
    return sea_number_val(exp(v));
}

static SeaValue math_round(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    if (sea_is_int(args[0])) return args[0];
    return sea_int_val((int64_t)round(sea_as_number(args[0])));
}

static SeaValue math_trunc(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    if (sea_is_int(args[0])) return args[0];
    return sea_int_val((int64_t)trunc(sea_as_number(args[0])));
}

static SeaValue math_isnan(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_FALSE_VAL;
    if (!sea_is_number(args[0])) return SEA_FALSE_VAL;
    return sea_bool_val(isnan(sea_as_number(args[0])));
}

static SeaValue math_isinf(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_FALSE_VAL;
    if (!sea_is_number(args[0])) return SEA_FALSE_VAL;
    return sea_bool_val(isinf(sea_as_number(args[0])));
}

void sea_lib_math_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);

    mod_set(vm, mod, "abs",        math_abs);
    mod_set(vm, mod, "ceil",       math_ceil);
    mod_set(vm, mod, "floor",      math_floor);
    mod_set(vm, mod, "sqrt",       math_sqrt);
    mod_set(vm, mod, "sin",        math_sin);
    mod_set(vm, mod, "cos",        math_cos);
    mod_set(vm, mod, "tan",        math_tan);
    mod_set(vm, mod, "asin",       math_asin);
    mod_set(vm, mod, "acos",       math_acos);
    mod_set(vm, mod, "atan",       math_atan);
    mod_set(vm, mod, "atan2",      math_atan2);
    mod_set(vm, mod, "log",        math_log);
    mod_set(vm, mod, "log2",       math_log2);
    mod_set(vm, mod, "log10",      math_log10);
    mod_set(vm, mod, "pow",        math_pow);
    mod_set(vm, mod, "max",        math_max);
    mod_set(vm, mod, "min",        math_min);
    mod_set(vm, mod, "random",     math_random);
    mod_set(vm, mod, "randomseed", math_randomseed);
    mod_set(vm, mod, "fmod",       math_fmod);
    mod_set(vm, mod, "exp",        math_exp);
    mod_set(vm, mod, "round",      math_round);
    mod_set(vm, mod, "trunc",      math_trunc);
    mod_set(vm, mod, "isnan",      math_isnan);
    mod_set(vm, mod, "isinf",      math_isinf);

    // Constants
    SeaString *pi_key = sea_string_intern("pi", 2);
    sea_table_set(mod, sea_obj_val(&pi_key->obj), sea_number_val(M_PI));
    sea_obj_release(&pi_key->obj);

    SeaString *huge_key = sea_string_intern("huge", 4);
    sea_table_set(mod, sea_obj_val(&huge_key->obj), sea_number_val(HUGE_VAL));
    sea_obj_release(&huge_key->obj);

    SeaString *maxint_key = sea_string_intern("maxinteger", 10);
    sea_table_set(mod, sea_obj_val(&maxint_key->obj), sea_int_val(INT64_MAX));
    sea_obj_release(&maxint_key->obj);

    SeaString *minint_key = sea_string_intern("mininteger", 10);
    sea_table_set(mod, sea_obj_val(&minint_key->obj), sea_int_val(INT64_MIN));
    sea_obj_release(&minint_key->obj);

    sea_vm_register_module(vm, "math", mod);
    sea_obj_release(&mod->obj);

    /* Also expose as bare globals so code can write isnan(x) / isinf(x)
     * without qualifying with "math.", matching common usage in tests and demos. */
    sea_vm_register_native(vm, "isnan", math_isnan);
    sea_vm_register_native(vm, "isinf", math_isinf);
}
