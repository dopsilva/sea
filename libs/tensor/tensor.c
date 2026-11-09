/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * tensor.so — numeric kernels on float_array and float32_array for Sea.
 *
 * The heavy loops of a small transformer (matrix-vector and batched matrix
 * products and their backward passes, RMSNorm, causal attention, softmax,
 * Adam), so that a model written in Sea trains at C speed while the
 * orchestration stays in Sea. Every function works in place on arrays
 * addressed as (array, offset), so the same code can run on a Sea
 * implementation of these functions and compare results.
 *
 *   local tensor = require("../../libs/tensor/tensor")
 *   tensor.matvec(x, xo, w, nin, nout, out, oo)   -- out[oo+j] = Σ_i x[xo+i]·w[i·nout+j]
 *   tensor.matmul(x, xo, w, T, nin, nout, out, oo) -- the same for T rows at once
 *
 * Every function takes float_arrays (double) or float32_arrays (float), all
 * of the same kind in one call: float32 halves the memory and doubles the
 * width of the vector units. The loops are in tensor_kernels.h, compiled
 * once per element type.
 *
 * Matrices are row-major: w[i*nout + j] links input i to output j.
 * Arguments are borrowed; nothing is allocated (functions return nil, or a
 * number). An index out of range is a runtime error (a panic).
 */

#include "sea.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <unistd.h>

#define GEMM_KB 256
#define GEMM_MR 8
#define GEMM_PAR_MIN (1 << 20)   /* multiply-adds below which a product stays on one thread */

/* ---- thread pool for the matrix products ----
 *
 * Started on first use with one worker per CPU (SEA_TENSOR_THREADS or
 * tensor.threads(n) set how many take part; 1 turns it off). pool_run(fn,
 * arg, n) runs fn(arg, part, n) for part = 0..n-1, part 0 on the caller, and
 * returns when all are done. One call uses the pool at a time: a call from
 * another goroutine that finds it busy runs on its own thread, so goroutines
 * that each call matmul do not oversubscribe the CPUs. The parts write
 * disjoint memory and never touch the VM.
 *
 * Each worker has its own slot. The caller fills the slot of each worker that
 * takes part, then bumps the slot's gen (release); a worker reads only its
 * own slot, and only after seeing its gen move, and the caller rewrites a slot
 * only after that worker has finished its previous part (pool_done). Workers
 * spin on their gen for a short while before sleeping on a condition
 * variable, because a training step makes many calls back to back and waking
 * a sleeping thread costs tens of microseconds. */

typedef void (*pool_fn)(void *arg, int part, int nparts);

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define cpu_relax() __asm__ __volatile__("yield")
#else
#define cpu_relax() ((void)0)
#endif

#define POOL_MAX 64
#define POOL_SPIN 4000   /* pauses before a worker sleeps (~100 µs) */

typedef struct {
    pool_fn fn;
    void *arg;
    int part, nparts;
    unsigned gen;                      /* atomic: bumped when the slot holds a new part */
    char pad[64 - 2 * sizeof(void *) - 3 * sizeof(int)];
} PoolSlot;

static PoolSlot pool_slot[POOL_MAX];
static pthread_mutex_t pool_busy = PTHREAD_MUTEX_INITIALIZER;   /* held by the call using the pool */
static pthread_mutex_t pool_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_wake = PTHREAD_COND_INITIALIZER;
static pthread_once_t pool_once = PTHREAD_ONCE_INIT;
static int pool_max = 1;            /* caller + workers */
static int pool_active = 0;         /* threads taking part (≤ pool_max); 0 = not set yet */
static int pool_sleepers = 0;       /* workers waiting on pool_wake (under pool_m) */
static int pool_done = 0;           /* atomic: parts finished in the current call */

static void *pool_worker(void *p) {
    PoolSlot *slot = &pool_slot[(intptr_t)p];
    unsigned seen = 0;              /* gens start at 0, before any worker exists */
    for (;;) {
        int spins = 0;
        while (__atomic_load_n(&slot->gen, __ATOMIC_ACQUIRE) == seen) {
            if (++spins < POOL_SPIN) { cpu_relax(); continue; }
            pthread_mutex_lock(&pool_m);
            pool_sleepers++;
            while (__atomic_load_n(&slot->gen, __ATOMIC_ACQUIRE) == seen) pthread_cond_wait(&pool_wake, &pool_m);
            pool_sleepers--;
            pthread_mutex_unlock(&pool_m);
            spins = 0;
        }
        seen++;
        slot->fn(slot->arg, slot->part, slot->nparts);
        __atomic_fetch_add(&pool_done, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void pool_init(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > POOL_MAX) n = POOL_MAX;
    int started = 1;
    for (int i = 1; i < n; i++) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        int ok = pthread_create(&t, &at, pool_worker, (void *)(intptr_t)i) == 0;
        pthread_attr_destroy(&at);
        if (!ok) break;
        started++;
    }
    pool_max = started;
    if (pool_active == 0) {
        const char *env = getenv("SEA_TENSOR_THREADS");
        int want = env ? atoi(env) : pool_max;
        pool_active = want < 1 ? 1 : want > pool_max ? pool_max : want;
    } else if (pool_active > pool_max) {
        pool_active = pool_max;
    }
}

static int pool_threads(void) {
    if (pool_active == 1) return 1;   /* turned off: do not start the workers */
    pthread_once(&pool_once, pool_init);
    return pool_active;
}

static void pool_run(pool_fn fn, void *arg, int n) {
    if (n > pool_max) n = pool_max;
    if (n <= 1 || pthread_mutex_trylock(&pool_busy) != 0) {
        fn(arg, 0, 1);   /* the pool is busy (another goroutine): do it all here */
        return;
    }
    __atomic_store_n(&pool_done, 0, __ATOMIC_RELAXED);
    for (int i = 1; i < n; i++) {
        PoolSlot *s = &pool_slot[i];
        s->fn = fn;
        s->arg = arg;
        s->part = i;
        s->nparts = n;
        __atomic_fetch_add(&s->gen, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_lock(&pool_m);
    if (pool_sleepers > 0) pthread_cond_broadcast(&pool_wake);
    pthread_mutex_unlock(&pool_m);
    fn(arg, 0, n);
    while (__atomic_load_n(&pool_done, __ATOMIC_ACQUIRE) < n - 1) cpu_relax();
    pthread_mutex_unlock(&pool_busy);
}

#define REAL double
#define SFX _f64
#define LANES 8
#include "tensor_kernels.h"
#undef REAL
#undef SFX
#undef LANES

#define REAL float
#define SFX _f32
#define LANES 16
#include "tensor_kernels.h"
#undef REAL
#undef SFX
#undef LANES

/* ---- argument helpers ---- */

static int64_t iv(SeaValue v) {
    return sea_is_int(v) ? sea_as_int(v) : (int64_t)sea_as_number(v);
}

static double dv(SeaValue v) {
    return sea_is_number(v) ? sea_as_number(v) : (double)sea_as_int(v);
}

/* element size of a float_array (8) or a float32_array (4) */
static int kind_of(SeaVM *vm, SeaValue v, const char *fn) {
    if (sea_is_float_array(v)) return 8;
    if (sea_is_float32_array(v)) return 4;
    sea_vm_error_noreturn(vm, "tensor.%s: expected a float_array or a float32_array, got %s", fn, sea_type_name(v));
    return 0;
}

static uint32_t len_of(SeaValue v) {
    return sea_is_float_array(v) ? SEA_AS_FLOAT_ARRAY(v)->len : SEA_AS_FLOAT32_ARRAY(v)->len;
}

/* data of an array of the given kind, checked to hold [off, off+n) */
static void *arr(SeaVM *vm, SeaValue v, int kind, int64_t off, int64_t n, const char *fn) {
    int k = kind_of(vm, v, fn);
    if (k != kind)
        sea_vm_error_noreturn(vm, "tensor.%s: mixes float_array and float32_array (got %s)", fn, sea_type_name(v));
    int64_t len = len_of(v);
    if (off < 0 || n < 0 || off + n > len)
        sea_vm_error_noreturn(vm, "tensor.%s: range %lld+%lld outside a %s of %lld",
                              fn, (long long)off, (long long)n, sea_type_name(v), (long long)len);
    if (kind == 8) return SEA_AS_FLOAT_ARRAY(v)->data + off;
    return SEA_AS_FLOAT32_ARRAY(v)->data + off;
}

static int64_t *ia(SeaVM *vm, SeaValue v, int64_t n, const char *fn) {
    if (!sea_is_int_array(v))
        sea_vm_error_noreturn(vm, "tensor.%s: expected an int_array, got %s", fn, sea_type_name(v));
    SeaIntArray *a = SEA_AS_INT_ARRAY(v);
    if (n > (int64_t)a->len)
        sea_vm_error_noreturn(vm, "tensor.%s: int_array of %u, need %lld", fn, a->len, (long long)n);
    return a->data;
}

static void need(SeaVM *vm, int argc, int n, const char *fn) {
    if (argc < n) sea_vm_error_noreturn(vm, "tensor.%s: expected %d arguments, got %d", fn, n, argc);
}

/* P(i, off, n): argument i as a pointer of the call's element type (kind kd) */
#define P(T_, i, off, n) ((T_ *)arr(vm, args[i], kd, (off), (n), FN))
#define D(i, off, n) P(double, i, off, n)
#define F(i, off, n) P(float, i, off, n)

/* ---- linear algebra ---- */

/* matvec(x, xo, w, nin, nout, out, oo): out[oo..] = x[xo..] @ w */
static SeaValue t_matvec(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "matvec";
    need(vm, argc, 7, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t xo = iv(args[1]), nin = iv(args[3]), nout = iv(args[4]), oo = iv(args[6]);
    if (kd == 8) matvec_f64(D(0, xo, nin), D(2, 0, nin * nout), nin, nout, D(5, oo, nout));
    else         matvec_f32(F(0, xo, nin), F(2, 0, nin * nout), nin, nout, F(5, oo, nout));
    return SEA_NIL_VAL;
}

/* matvec_back(dout, doo, x, xo, w, dw, nin, nout, dx, dxo):
 * dw += x ⊗ dout, dx += w @ dout */
static SeaValue t_matvec_back(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "matvec_back";
    need(vm, argc, 10, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t doo = iv(args[1]), xo = iv(args[3]), nin = iv(args[6]), nout = iv(args[7]), dxo = iv(args[9]);
    if (kd == 8) matvec_back_f64(D(0, doo, nout), D(2, xo, nin), D(4, 0, nin * nout), D(5, 0, nin * nout), nin, nout, D(8, dxo, nin));
    else         matvec_back_f32(F(0, doo, nout), F(2, xo, nin), F(4, 0, nin * nout), F(5, 0, nin * nout), nin, nout, F(8, dxo, nin));
    return SEA_NIL_VAL;
}

/* matmul(x, xo, w, T, nin, nout, out, oo): T rows at once —
 * out[oo + t·nout + j] = Σ_i x[xo + t·nin + i]·w[i·nout + j] */
static SeaValue t_matmul(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "matmul";
    need(vm, argc, 8, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t xo = iv(args[1]), T = iv(args[3]), nin = iv(args[4]), nout = iv(args[5]), oo = iv(args[7]);
    if (kd == 8) gemm_f64(D(0, xo, T * nin), D(2, 0, nin * nout), D(6, oo, T * nout), T, nin, nout, 0);
    else         gemm_f32(F(0, xo, T * nin), F(2, 0, nin * nout), F(6, oo, T * nout), T, nin, nout, 0);
    return SEA_NIL_VAL;
}

/* matmul_back(dout, doo, x, xo, w, dw, T, nin, nout, dx, dxo):
 * dw += xᵀ · dout, dx += dout · wᵀ (dw or dx may be nil to skip it) */
static SeaValue t_matmul_back(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "matmul_back";
    need(vm, argc, 11, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t doo = iv(args[1]), xo = iv(args[3]), T = iv(args[6]), nin = iv(args[7]), nout = iv(args[8]), dxo = iv(args[10]);
    bool want_dw = !sea_is_nil(args[5]), want_dx = !sea_is_nil(args[9]);
    if (kd == 8) {
        const double *g = D(0, doo, T * nout);
        if (want_dw) {
            double *dw = D(5, 0, nin * nout);
            double *xt = transpose_f64(D(2, xo, T * nin), T, nin);
            if (!xt) sea_vm_error_noreturn(vm, "tensor.%s: out of memory", FN);
            gemm_f64(xt, g, dw, nin, T, nout, 1);
            free(xt);
        }
        if (want_dx) {
            double *dx = D(9, dxo, T * nin);
            double *wt = transpose_f64(D(4, 0, nin * nout), nin, nout);
            if (!wt) sea_vm_error_noreturn(vm, "tensor.%s: out of memory", FN);
            gemm_f64(g, wt, dx, T, nout, nin, 1);
            free(wt);
        }
    } else {
        const float *g = F(0, doo, T * nout);
        if (want_dw) {
            float *dw = F(5, 0, nin * nout);
            float *xt = transpose_f32(F(2, xo, T * nin), T, nin);
            if (!xt) sea_vm_error_noreturn(vm, "tensor.%s: out of memory", FN);
            gemm_f32(xt, g, dw, nin, T, nout, 1);
            free(xt);
        }
        if (want_dx) {
            float *dx = F(9, dxo, T * nin);
            float *wt = transpose_f32(F(4, 0, nin * nout), nin, nout);
            if (!wt) sea_vm_error_noreturn(vm, "tensor.%s: out of memory", FN);
            gemm_f32(g, wt, dx, T, nout, nin, 1);
            free(wt);
        }
    }
    return SEA_NIL_VAL;
}

static void check_cols(SeaVM *vm, const int64_t *cols, int64_t nc, int64_t nout, const char *fn) {
    for (int64_t k = 0; k < nc; k++)
        if (cols[k] < 0 || cols[k] >= nout) sea_vm_error_noreturn(vm, "tensor.%s: column out of range", fn);
}

/* matvec_cols(x, xo, w, nin, nout, cols, ncols, out, oo): only the outputs
 * listed in cols (int_array): out[oo+k] = Σ_i x[i]·w[i·nout + cols[k]] */
static SeaValue t_matvec_cols(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "matvec_cols";
    need(vm, argc, 9, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t xo = iv(args[1]), nin = iv(args[3]), nout = iv(args[4]), nc = iv(args[6]), oo = iv(args[8]);
    const int64_t *cols = ia(vm, args[5], nc, FN);
    check_cols(vm, cols, nc, nout, FN);
    if (kd == 8) matvec_cols_f64(D(0, xo, nin), D(2, 0, nin * nout), nin, nout, cols, nc, D(7, oo, nc));
    else         matvec_cols_f32(F(0, xo, nin), F(2, 0, nin * nout), nin, nout, cols, nc, F(7, oo, nc));
    return SEA_NIL_VAL;
}

/* matvec_cols_back(dout, doo, x, xo, w, dw, nin, nout, cols, ncols, dx, dxo) */
static SeaValue t_matvec_cols_back(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "matvec_cols_back";
    need(vm, argc, 12, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t doo = iv(args[1]), xo = iv(args[3]), nin = iv(args[6]), nout = iv(args[7]);
    int64_t nc = iv(args[9]), dxo = iv(args[11]);
    const int64_t *cols = ia(vm, args[8], nc, FN);
    check_cols(vm, cols, nc, nout, FN);
    if (kd == 8) matvec_cols_back_f64(D(0, doo, nc), D(2, xo, nin), D(4, 0, nin * nout), D(5, 0, nin * nout), nin, nout, cols, nc, D(10, dxo, nin));
    else         matvec_cols_back_f32(F(0, doo, nc), F(2, xo, nin), F(4, 0, nin * nout), F(5, 0, nin * nout), nin, nout, cols, nc, F(10, dxo, nin));
    return SEA_NIL_VAL;
}

/* ---- normalization and activations ---- */

/* rmsnorm(x, xo, y, yo, n) → r = 1/rms(x); y = x·r */
static SeaValue t_rmsnorm(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "rmsnorm";
    need(vm, argc, 5, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t xo = iv(args[1]), yo = iv(args[3]), n = iv(args[4]);
    double r = kd == 8 ? rmsnorm_f64(D(0, xo, n), D(2, yo, n), n) : rmsnorm_f32(F(0, xo, n), F(2, yo, n), n);
    return sea_number_val(r);
}

/* rmsnorm_back(dy, dyo, x, xo, r, n, dx, dxo): dx += r·dy − x·r³·(dy·x)/n */
static SeaValue t_rmsnorm_back(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "rmsnorm_back";
    need(vm, argc, 8, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t dyo = iv(args[1]), xo = iv(args[3]), n = iv(args[5]), dxo = iv(args[7]);
    double r = dv(args[4]);
    if (kd == 8) rmsnorm_back_f64(D(0, dyo, n), D(2, xo, n), r, n, D(6, dxo, n));
    else         rmsnorm_back_f32(F(0, dyo, n), F(2, xo, n), r, n, F(6, dxo, n));
    return SEA_NIL_VAL;
}

/* relu(x, xo, y, yo, n): y = max(x, 0) */
static SeaValue t_relu(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "relu";
    need(vm, argc, 5, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t xo = iv(args[1]), yo = iv(args[3]), n = iv(args[4]);
    if (kd == 8) relu_f64(D(0, xo, n), D(2, yo, n), n);
    else         relu_f32(F(0, xo, n), F(2, yo, n), n);
    return SEA_NIL_VAL;
}

/* relu_back(d, dofs, x, xo, n): d = 0 where x ≤ 0 */
static SeaValue t_relu_back(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "relu_back";
    need(vm, argc, 5, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t dofs = iv(args[1]), xo = iv(args[3]), n = iv(args[4]);
    if (kd == 8) relu_back_f64(D(0, dofs, n), D(2, xo, n), n);
    else         relu_back_f32(F(0, dofs, n), F(2, xo, n), n);
    return SEA_NIL_VAL;
}

/* softmax(p, off, n): in place, numerically stable */
static SeaValue t_softmax(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "softmax";
    need(vm, argc, 3, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t off = iv(args[1]), n = iv(args[2]);
    if (kd == 8) softmax_f64(D(0, off, n), n);
    else         softmax_f32(F(0, off, n), n);
    return SEA_NIL_VAL;
}

/* ---- attention ---- */

/* attention(q, k, v, att, y, T, C, H, B): causal multi-head attention of one
 * layer. q, k, v, y hold T rows of C (row t at t·C); head h uses columns
 * [h·C/H, (h+1)·C/H). att (H·B·B) keeps the probabilities for the backward:
 * att[(h·B + t)·B + s]. */
static SeaValue t_attention(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "attention";
    need(vm, argc, 9, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t T = iv(args[5]), C = iv(args[6]), H = iv(args[7]), B = iv(args[8]);
    if (T > B) sea_vm_error_noreturn(vm, "tensor.attention: %lld positions, context %lld", (long long)T, (long long)B);
    if (kd == 8) attention_f64(D(0, 0, T * C), D(1, 0, T * C), D(2, 0, T * C), D(3, 0, H * B * B), D(4, 0, T * C), T, C, H, B);
    else         attention_f32(F(0, 0, T * C), F(1, 0, T * C), F(2, 0, T * C), F(3, 0, H * B * B), F(4, 0, T * C), T, C, H, B);
    return SEA_NIL_VAL;
}

/* attention_back(q, k, v, att, dy, dq, dk, dv, T, C, H, B): accumulates into
 * dq, dk, dv the gradients of the last attention (the caller zeroes them). */
static SeaValue t_attention_back(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "attention_back";
    need(vm, argc, 12, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t T = iv(args[8]), C = iv(args[9]), H = iv(args[10]), B = iv(args[11]);
    double stack_tmp[256];
    double *dp = T <= 256 ? stack_tmp : (double *)malloc(sizeof(double) * (size_t)T);
    if (!dp) sea_vm_error_noreturn(vm, "tensor.%s: out of memory", FN);
    if (kd == 8)
        attention_back_f64(D(0, 0, T * C), D(1, 0, T * C), D(2, 0, T * C), D(3, 0, H * B * B), D(4, 0, T * C),
                           D(5, 0, T * C), D(6, 0, T * C), D(7, 0, T * C), T, C, H, B, dp);
    else
        attention_back_f32(F(0, 0, T * C), F(1, 0, T * C), F(2, 0, T * C), F(3, 0, H * B * B), F(4, 0, T * C),
                           F(5, 0, T * C), F(6, 0, T * C), F(7, 0, T * C), T, C, H, B, dp);
    if (dp != stack_tmp) free(dp);
    return SEA_NIL_VAL;
}

/* ---- vectors ---- */

/* zero(a [, off, n]) */
static SeaValue t_zero(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "zero";
    need(vm, argc, 1, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t len = len_of(args[0]);
    int64_t off = argc > 1 ? iv(args[1]) : 0, n = argc > 2 ? iv(args[2]) : len - off;
    memset(arr(vm, args[0], kd, off, n, FN), 0, (size_t)kd * (size_t)n);
    return SEA_NIL_VAL;
}

/* add(dst, doff, src, soff, n): dst[doff..] += src[soff..] */
static SeaValue t_add(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "add";
    need(vm, argc, 5, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t doff = iv(args[1]), soff = iv(args[3]), n = iv(args[4]);
    if (kd == 8) add_f64(D(0, doff, n), D(2, soff, n), n);
    else         add_f32(F(0, doff, n), F(2, soff, n), n);
    return SEA_NIL_VAL;
}

/* copy(dst, doff, src, soff, n) */
static SeaValue t_copy(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "copy";
    need(vm, argc, 5, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t doff = iv(args[1]), soff = iv(args[3]), n = iv(args[4]);
    void *d = arr(vm, args[0], kd, doff, n, FN);
    const void *s = arr(vm, args[2], kd, soff, n, FN);
    memmove(d, s, (size_t)kd * (size_t)n);
    return SEA_NIL_VAL;
}

/* scale(a, off, n, k): a[off+i] *= k */
static SeaValue t_scale(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "scale";
    need(vm, argc, 4, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t off = iv(args[1]), n = iv(args[2]);
    double k = dv(args[3]);
    if (kd == 8) scale_f64(D(0, off, n), n, k);
    else         scale_f32(F(0, off, n), n, k);
    return SEA_NIL_VAL;
}

/* adam(w, g, m, v, n, lr, beta1, beta2, c1, c2, eps): one Adam step, with
 * the bias corrections c1 = 1 − beta1^t, c2 = 1 − beta2^t */
static SeaValue t_adam(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "adam";
    need(vm, argc, 11, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t n = iv(args[4]);
    double lr = dv(args[5]), b1 = dv(args[6]), b2 = dv(args[7]), c1 = dv(args[8]), c2 = dv(args[9]), eps = dv(args[10]);
    if (kd == 8) adam_f64(D(0, 0, n), D(1, 0, n), D(2, 0, n), D(3, 0, n), n, lr, b1, b2, c1, c2, eps);
    else         adam_f32(F(0, 0, n), F(1, 0, n), F(2, 0, n), F(3, 0, n), n, lr, b1, b2, c1, c2, eps);
    return SEA_NIL_VAL;
}

/* ---- files ---- */

/* save(path, a [, append]): writes the array as raw elements (doubles for a
 * float_array, floats for a float32_array, native byte order); with
 * append = true, appends. Returns the bytes written, or an error value. */
static SeaValue t_save(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "save";
    need(vm, argc, 2, FN);
    if (!sea_is_string(args[0])) return sea_make_error_str(vm, "tensor.save: expected a path");
    int kd = kind_of(vm, args[1], FN);
    int64_t n = len_of(args[1]);
    const void *a = arr(vm, args[1], kd, 0, n, FN);
    FILE *f = fopen(SEA_AS_STRING(args[0])->data, argc > 2 && sea_is_truthy(args[2]) ? "ab" : "wb");
    if (!f) return sea_make_error_str(vm, "tensor.save: cannot open the file");
    size_t w = fwrite(a, (size_t)kd, (size_t)n, f);
    int err = fclose(f);
    if (w != (size_t)n || err != 0) return sea_make_error_str(vm, "tensor.save: write failed");
    return sea_int_val((int64_t)(w * (size_t)kd));
}

/* load(path, a, offset): fills the array (its whole length) with elements of
 * its type read at byte offset `offset`. Returns the next offset, or an error
 * value. */
static SeaValue t_load(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "load";
    need(vm, argc, 3, FN);
    if (!sea_is_string(args[0])) return sea_make_error_str(vm, "tensor.load: expected a path");
    int kd = kind_of(vm, args[1], FN);
    int64_t n = len_of(args[1]), off = iv(args[2]);
    void *a = arr(vm, args[1], kd, 0, n, FN);
    FILE *f = fopen(SEA_AS_STRING(args[0])->data, "rb");
    if (!f) return sea_make_error_str(vm, "tensor.load: cannot open the file");
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return sea_make_error_str(vm, "tensor.load: bad offset"); }
    size_t r = fread(a, (size_t)kd, (size_t)n, f);
    fclose(f);
    if (r != (size_t)n) return sea_make_error_str(vm, "tensor.load: file too short");
    return sea_int_val(off + (int64_t)(r * (size_t)kd));
}

/* threads([n]) → n: how many threads the matrix products use (default: one
 * per CPU, or SEA_TENSOR_THREADS); with n, sets it (1 = only the caller). */
static SeaValue t_threads(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc > 0 && !sea_is_nil(args[0])) {
        int64_t n = iv(args[0]);
        if (n < 1) n = 1;
        if (n == 1) {
            pool_active = 1;
        } else {
            if (pool_active == 1) pool_active = (int)n;   /* let pool_init start the workers */
            pthread_once(&pool_once, pool_init);
            pool_active = n > pool_max ? pool_max : (int)n;
        }
    }
    if (pool_active == 0) pthread_once(&pool_once, pool_init);
    return sea_int_val(pool_active);
}

/* ---- module ---- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "matvec",           t_matvec);
    sea_mod_set(mod, "matvec_back",      t_matvec_back);
    sea_mod_set(mod, "matmul",           t_matmul);
    sea_mod_set(mod, "matmul_back",      t_matmul_back);
    sea_mod_set(mod, "matvec_cols",      t_matvec_cols);
    sea_mod_set(mod, "matvec_cols_back", t_matvec_cols_back);
    sea_mod_set(mod, "rmsnorm",          t_rmsnorm);
    sea_mod_set(mod, "rmsnorm_back",     t_rmsnorm_back);
    sea_mod_set(mod, "relu",             t_relu);
    sea_mod_set(mod, "relu_back",        t_relu_back);
    sea_mod_set(mod, "softmax",          t_softmax);
    sea_mod_set(mod, "attention",        t_attention);
    sea_mod_set(mod, "attention_back",   t_attention_back);
    sea_mod_set(mod, "zero",             t_zero);
    sea_mod_set(mod, "add",              t_add);
    sea_mod_set(mod, "copy",             t_copy);
    sea_mod_set(mod, "scale",            t_scale);
    sea_mod_set(mod, "adam",             t_adam);
    sea_mod_set(mod, "save",             t_save);
    sea_mod_set(mod, "load",             t_load);
    sea_mod_set(mod, "threads",          t_threads);
    return sea_obj_val(&mod->obj);
}
