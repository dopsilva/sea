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
/* The matrix-product tile: GEMM_MR rows × two vectors of VEC_BYTES, held in
 * registers. It must fit the vector registers of the target: 16 accumulators of
 * 64 bytes in the 32 registers of AVX-512; 12 of 32 bytes in the 16 of AVX/AVX2;
 * 12 of 16 bytes in the 16 of SSE (32 on NEON). A tile that does not fit spills
 * its accumulators to memory and runs several times slower. */
#if defined(__AVX512F__)
#define VEC_BYTES 64
#define GEMM_MR 8
#elif defined(__AVX__)
#define VEC_BYTES 32
#define GEMM_MR 6
#else
#define VEC_BYTES 16
#define GEMM_MR 6
#endif
#define GEMM_PLAIN 0   /* A and B as they are */
#define GEMM_TA 1      /* A read transposed: element (m, k) at a[k*lda + m] */
#define GEMM_TB 2      /* B read transposed: element (k, n) at b[n*ldb + k] */
#define MATVEC_BLOCK 64        /* matvec splits its outputs in blocks of this many columns */
#define GEMM_PAR_MIN (1 << 20)
#define ELEM_PAR_MIN (1 << 16)    /* elements below which swiglu stays on one thread */
#define ATT_PAR_MIN (1 << 18)     /* T·T·C below which attention stays on one thread */   /* multiply-adds below which a product stays on one thread */

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
#define LANES (VEC_BYTES / 8)
#include "tensor_kernels.h"
#undef REAL
#undef SFX
#undef LANES

#define REAL float
#define SFX _f32
#define LANES (VEC_BYTES / 4)
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
    /* dw[nin×nout] += xᵀ·g reads x transposed in place; dx[T×nin] += g·wᵀ reads w transposed */
    if (kd == 8) {
        const double *g = D(0, doo, T * nout);
        if (want_dw) gemm_x_f64(D(2, xo, T * nin), nin, g, nout, D(5, 0, nin * nout), nin, T, nout, 1, GEMM_TA);
        if (want_dx) gemm_x_f64(g, nout, D(4, 0, nin * nout), nout, D(9, dxo, T * nin), T, nout, nin, 1, GEMM_TB);
    } else {
        const float *g = F(0, doo, T * nout);
        if (want_dw) gemm_x_f32(F(2, xo, T * nin), nin, g, nout, F(5, 0, nin * nout), nin, T, nout, 1, GEMM_TA);
        if (want_dx) gemm_x_f32(g, nout, F(4, 0, nin * nout), nout, F(9, dxo, T * nin), T, nout, nin, 1, GEMM_TB);
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

/* scratch memory of the calling thread (grows, never freed: one per scheduler thread) */
static __thread void *tls_scratch = NULL;
static __thread size_t tls_scratch_cap = 0;

static void *scratch(SeaVM *vm, size_t bytes, const char *fn) {
    if (bytes > tls_scratch_cap) {
        size_t cap = bytes < 4096 ? 4096 : bytes + bytes / 2;
        void *p = realloc(tls_scratch, cap);
        if (!p) sea_vm_error_noreturn(vm, "tensor.%s: out of memory", fn);
        tls_scratch = p;
        tls_scratch_cap = cap;
    }
    return tls_scratch;
}

/* swiglu(a, ao, b, bo, s, so, n): s = silu(a)·b, silu(a) = a·σ(a) (the gated MLP) */
static SeaValue t_swiglu(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "swiglu";
    need(vm, argc, 7, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t ao = iv(args[1]), bo = iv(args[3]), so = iv(args[5]), n = iv(args[6]);
    if (kd == 8) swiglu_f64(D(0, ao, n), D(2, bo, n), D(4, so, n), n);
    else         swiglu_f32(F(0, ao, n), F(2, bo, n), F(4, so, n), n);
    return SEA_NIL_VAL;
}

/* swiglu_back(ds, dso, a, ao, b, bo, da, dao, db, dbo, n): da += ds·b·silu'(a), db += ds·silu(a) */
static SeaValue t_swiglu_back(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "swiglu_back";
    need(vm, argc, 11, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t dso = iv(args[1]), ao = iv(args[3]), bo = iv(args[5]), dao = iv(args[7]), dbo = iv(args[9]), n = iv(args[10]);
    if (kd == 8) swiglu_back_f64(D(0, dso, n), D(2, ao, n), D(4, bo, n), D(6, dao, n), D(8, dbo, n), n);
    else         swiglu_back_f32(F(0, dso, n), F(2, ao, n), F(4, bo, n), F(6, dao, n), F(8, dbo, n), n);
    return SEA_NIL_VAL;
}

/* rope(x, xo, T, C, H, pos0 [, base [, inverse]]): rotary position embedding in place on T
 * rows of C (H heads, C/H even); row t is position pos0 + t; base 10000 by default;
 * inverse = true undoes it (the backward pass). */
static SeaValue t_rope(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "rope";
    need(vm, argc, 6, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t xo = iv(args[1]), T = iv(args[2]), C = iv(args[3]), H = iv(args[4]), pos0 = iv(args[5]);
    double base = argc > 6 && !sea_is_nil(args[6]) ? dv(args[6]) : 10000.0;
    int inverse = argc > 7 && sea_is_truthy(args[7]);
    if (T < 0 || C < 1 || H < 1 || C % H != 0 || (C / H) % 2 != 0 || pos0 < 0)
        sea_vm_error_noreturn(vm, "tensor.%s: bad sizes (T %lld, C %lld, H %lld)", FN, (long long)T, (long long)C, (long long)H);
    double *freq = scratch(vm, sizeof(double) * (size_t)(C / H / 2 + 1), FN);
    if (kd == 8) rope_f64(D(0, xo, T * C), T, C, H, pos0, base, inverse, freq);
    else         rope_f32(F(0, xo, T * C), T, C, H, pos0, base, inverse, freq);
    return SEA_NIL_VAL;
}

/* cross_entropy(logits, lo, targets, to, T, V [, scale]) → loss: T rows of V logits against
 * targets[to..to+T) (an int_array; a target < 0 is skipped); returns the summed loss and
 * replaces the logits by their gradient scale·(softmax − onehot), scale 1 by default. */
static SeaValue t_cross_entropy(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "cross_entropy";
    need(vm, argc, 6, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t lo = iv(args[1]), to = iv(args[3]), T = iv(args[4]), V = iv(args[5]);
    double scale = argc > 6 && !sea_is_nil(args[6]) ? dv(args[6]) : 1.0;
    if (to < 0) sea_vm_error_noreturn(vm, "tensor.%s: negative offset", FN);
    const int64_t *tg = ia(vm, args[2], to + T, FN) + to;
    for (int64_t t = 0; t < T; t++)
        if (tg[t] >= V) sea_vm_error_noreturn(vm, "tensor.%s: target %lld outside a vocabulary of %lld", FN, (long long)tg[t], (long long)V);
    double *losses = scratch(vm, sizeof(double) * (size_t)(T > 0 ? T : 1), FN);
    double loss = kd == 8 ? cross_entropy_f64(D(0, lo, T * V), tg, T, V, scale, losses)
                          : cross_entropy_f32(F(0, lo, T * V), tg, T, V, scale, losses);
    return sea_number_val(loss);
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

/* attention_step(q, qo, k, v, y, yo, T, C, H): the attention of one new position
 * (the last, T-1) against the T rows of keys k and values v (a KV cache):
 * y[yo..yo+C) = what row T-1 of attention gives, for inference (no backward). */
static SeaValue t_attention_step(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "attention_step";
    need(vm, argc, 9, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t qo = iv(args[1]), yo = iv(args[5]), T = iv(args[6]), C = iv(args[7]), H = iv(args[8]);
    if (T < 1 || C < 1 || H < 1 || C % H != 0)
        sea_vm_error_noreturn(vm, "tensor.%s: bad sizes (T %lld, C %lld, H %lld)", FN, (long long)T, (long long)C,
                              (long long)H);
    void *p = scratch(vm, (size_t)T * (size_t)kd, FN);
    if (kd == 8) attention_step_f64(D(0, qo, C), D(2, 0, T * C), D(3, 0, T * C), D(4, yo, C), T, C, H, (double *)p);
    else         attention_step_f32(F(0, qo, C), F(2, 0, T * C), F(3, 0, T * C), F(4, yo, C), T, C, H, (float *)p);
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

/* ---- columns ---- */

/* gather_cols(src, nrows, ncols, cols, coff, nc, dst [, trans]): the columns
 * cols[coff .. coff+nc) of src (nrows × ncols) into dst — dst[i·nc + k], or
 * with trans one column per row, dst[k·nrows + i] (an embedding lookup:
 * gather_cols(E, d, V, ids, off, T, x, true) puts E[:, ids[off+t]] in row t) */
static SeaValue t_gather_cols(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "gather_cols";
    need(vm, argc, 7, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t nrows = iv(args[1]), ncols = iv(args[2]), coff = iv(args[4]), nc = iv(args[5]);
    int trans = argc > 7 && sea_is_truthy(args[7]);
    if (coff < 0) sea_vm_error_noreturn(vm, "tensor.%s: negative offset", FN);
    const int64_t *cols = ia(vm, args[3], coff + nc, FN) + coff;
    check_cols(vm, cols, nc, ncols, FN);
    if (kd == 8) gather_cols_f64(D(0, 0, nrows * ncols), nrows, ncols, cols, nc, D(6, 0, nrows * nc), trans);
    else         gather_cols_f32(F(0, 0, nrows * ncols), nrows, ncols, cols, nc, F(6, 0, nrows * nc), trans);
    return SEA_NIL_VAL;
}

/* scatter_cols(dst, nrows, ncols, cols, coff, nc, src [, trans]): the reverse
 * of gather_cols, accumulating — dst[i·ncols + cols[coff+k]] += src[...] */
static SeaValue t_scatter_cols(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "scatter_cols";
    need(vm, argc, 7, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t nrows = iv(args[1]), ncols = iv(args[2]), coff = iv(args[4]), nc = iv(args[5]);
    int trans = argc > 7 && sea_is_truthy(args[7]);
    if (coff < 0) sea_vm_error_noreturn(vm, "tensor.%s: negative offset", FN);
    const int64_t *cols = ia(vm, args[3], coff + nc, FN) + coff;
    check_cols(vm, cols, nc, ncols, FN);
    if (kd == 8) scatter_cols_f64(D(0, 0, nrows * ncols), nrows, ncols, cols, nc, D(6, 0, nrows * nc), trans);
    else         scatter_cols_f32(F(0, 0, nrows * ncols), nrows, ncols, cols, nc, F(6, 0, nrows * nc), trans);
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

/* sumsq(a [, off, n]) → Σ a[i]² (in double; the whole array by default):
 * the squared norm, for gradient clipping */
static SeaValue t_sumsq(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "sumsq";
    need(vm, argc, 1, FN);
    int kd = kind_of(vm, args[0], FN);
    int64_t len = len_of(args[0]);
    int64_t off = argc > 1 ? iv(args[1]) : 0, n = argc > 2 ? iv(args[2]) : len - off;
    double r = kd == 8 ? sumsq_f64(D(0, off, n), n) : sumsq_f32(F(0, off, n), n);
    return sea_number_val(r);
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

/* save_u16(path, a [, n [, append]]) → bytes: the first n elements (all by default) of an
 * int_array as unsigned 16-bit little-endian values (token ids); a value outside
 * 0..65535 is a runtime error; an error value when the file cannot be written. */
static SeaValue t_save_u16(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "save_u16";
    need(vm, argc, 2, FN);
    if (!sea_is_string(args[0])) return sea_make_error_str(vm, "tensor.save_u16: expected a path");
    if (!sea_is_int_array(args[1])) sea_vm_error_noreturn(vm, "tensor.%s: expected an int_array, got %s", FN, sea_type_name(args[1]));
    SeaIntArray *ia_ = SEA_AS_INT_ARRAY(args[1]);
    int64_t n = argc > 2 && !sea_is_nil(args[2]) ? iv(args[2]) : (int64_t)ia_->len;
    if (n < 0 || n > (int64_t)ia_->len) sea_vm_error_noreturn(vm, "tensor.%s: %lld elements of an int_array of %u", FN, (long long)n, ia_->len);
    unsigned char *buf = malloc((size_t)n * 2 + 1);
    if (!buf) sea_vm_error_noreturn(vm, "tensor.%s: out of memory", FN);
    for (int64_t i = 0; i < n; i++) {
        int64_t v = ia_->data[i];
        if (v < 0 || v > 65535) {
            free(buf);
            sea_vm_error_noreturn(vm, "tensor.%s: value %lld at %lld does not fit 16 bits", FN, (long long)v, (long long)i);
        }
        buf[2 * i] = (unsigned char)(v & 255);
        buf[2 * i + 1] = (unsigned char)(v >> 8);
    }
    FILE *f = fopen(SEA_AS_STRING(args[0])->data, argc > 3 && sea_is_truthy(args[3]) ? "ab" : "wb");
    if (!f) { free(buf); return sea_make_error_str(vm, "tensor.save_u16: cannot open the file"); }
    size_t w = fwrite(buf, 1, (size_t)n * 2, f);
    int err = fclose(f);
    free(buf);
    if (w != (size_t)n * 2 || err != 0) return sea_make_error_str(vm, "tensor.save_u16: write failed");
    return sea_int_val(n * 2);
}

/* load_u16(path, a, offset, n) → next offset: n unsigned 16-bit little-endian values read at
 * byte offset into a[0..n) (an int_array with at least n elements); an error value when the
 * file cannot be read or is short. */
static SeaValue t_load_u16(SeaVM *vm, int argc, SeaValue *args) {
    const char *FN = "load_u16";
    need(vm, argc, 4, FN);
    if (!sea_is_string(args[0])) return sea_make_error_str(vm, "tensor.load_u16: expected a path");
    if (!sea_is_int_array(args[1])) sea_vm_error_noreturn(vm, "tensor.%s: expected an int_array, got %s", FN, sea_type_name(args[1]));
    SeaIntArray *ia_ = SEA_AS_INT_ARRAY(args[1]);
    int64_t off = iv(args[2]), n = iv(args[3]);
    if (off < 0 || n < 0 || n > (int64_t)ia_->len) sea_vm_error_noreturn(vm, "tensor.%s: %lld values into an int_array of %u", FN, (long long)n, ia_->len);
    unsigned char *buf = malloc((size_t)n * 2 + 1);
    if (!buf) sea_vm_error_noreturn(vm, "tensor.%s: out of memory", FN);
    FILE *f = fopen(SEA_AS_STRING(args[0])->data, "rb");
    if (!f) { free(buf); return sea_make_error_str(vm, "tensor.load_u16: cannot open the file"); }
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); free(buf); return sea_make_error_str(vm, "tensor.load_u16: bad offset"); }
    size_t r = fread(buf, 1, (size_t)n * 2, f);
    fclose(f);
    if (r != (size_t)n * 2) { free(buf); return sea_make_error_str(vm, "tensor.load_u16: file too short"); }
    for (int64_t i = 0; i < n; i++) ia_->data[i] = (int64_t)buf[2 * i] | ((int64_t)buf[2 * i + 1] << 8);
    free(buf);
    return sea_int_val(off + n * 2);
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
    sea_mod_set(mod, "swiglu",           t_swiglu);
    sea_mod_set(mod, "swiglu_back",      t_swiglu_back);
    sea_mod_set(mod, "rope",             t_rope);
    sea_mod_set(mod, "cross_entropy",    t_cross_entropy);
    sea_mod_set(mod, "attention",        t_attention);
    sea_mod_set(mod, "attention_back",   t_attention_back);
    sea_mod_set(mod, "attention_step",   t_attention_step);
    sea_mod_set(mod, "zero",             t_zero);
    sea_mod_set(mod, "add",              t_add);
    sea_mod_set(mod, "copy",             t_copy);
    sea_mod_set(mod, "scale",            t_scale);
    sea_mod_set(mod, "sumsq",            t_sumsq);
    sea_mod_set(mod, "adam",             t_adam);
    sea_mod_set(mod, "save",             t_save);
    sea_mod_set(mod, "load",             t_load);
    sea_mod_set(mod, "save_u16",         t_save_u16);
    sea_mod_set(mod, "load_u16",         t_load_u16);
    sea_mod_set(mod, "threads",          t_threads);
    sea_mod_set(mod, "gather_cols",      t_gather_cols);
    sea_mod_set(mod, "scatter_cols",     t_scatter_cols);
    return sea_obj_val(&mod->obj);
}
