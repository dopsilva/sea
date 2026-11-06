/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * tensor.so — numeric kernels on float_array for Sea.
 *
 * The heavy loops of a small transformer (matrix-vector products and their
 * backward passes, RMSNorm, causal attention, softmax, Adam), so that a
 * model written in Sea trains at C speed while the orchestration stays in
 * Sea. Every function works in place on float_arrays addressed as
 * (array, offset), so the same code can run on a Sea implementation of these
 * functions and compare results.
 *
 *   local tensor = require("../../libs/tensor/tensor")
 *   tensor.matvec(x, xo, w, nin, nout, out, oo)   -- out[oo+j] = Σ_i x[xo+i]·w[i·nout+j]
 *   tensor.matmul(x, xo, w, T, nin, nout, out, oo) -- the same for T rows at once
 *
 * Matrices are row-major: w[i*nout + j] links input i to output j.
 * Arguments are borrowed; nothing is allocated (functions return nil, or a
 * number). An index out of range is a runtime error (a panic).
 */

#include "sea.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- argument helpers ---- */

static int64_t iv(SeaValue v) {
    return sea_is_int(v) ? sea_as_int(v) : (int64_t)sea_as_number(v);
}

static double dv(SeaValue v) {
    return sea_is_number(v) ? sea_as_number(v) : (double)sea_as_int(v);
}

/* data of a float_array, checked to hold [off, off+n) */
static double *fa(SeaVM *vm, SeaValue v, int64_t off, int64_t n, const char *fn) {
    if (!sea_is_float_array(v))
        sea_vm_error_noreturn(vm, "tensor.%s: expected a float_array, got %s", fn, sea_type_name(v));
    SeaFloatArray *a = SEA_AS_FLOAT_ARRAY(v);
    if (off < 0 || n < 0 || off + n > (int64_t)a->len)
        sea_vm_error_noreturn(vm, "tensor.%s: range %lld+%lld outside a float_array of %u",
                              fn, (long long)off, (long long)n, a->len);
    return a->data + off;
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

/* ---- linear algebra ---- */

/* matvec(x, xo, w, nin, nout, out, oo): out[oo..] = x[xo..] @ w */
static SeaValue t_matvec(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 7, "matvec");
    int64_t xo = iv(args[1]), nin = iv(args[3]), nout = iv(args[4]), oo = iv(args[6]);
    const double *x = fa(vm, args[0], xo, nin, "matvec");
    const double *w = fa(vm, args[2], 0, nin * nout, "matvec");
    double *out = fa(vm, args[5], oo, nout, "matvec");
    for (int64_t j = 0; j < nout; j++) out[j] = 0.0;
    for (int64_t i = 0; i < nin; i++) {
        double xi = x[i];
        if (xi == 0.0) continue;
        const double *row = w + i * nout;
        for (int64_t j = 0; j < nout; j++) out[j] += xi * row[j];
    }
    return SEA_NIL_VAL;
}

/* matvec_back(dout, doo, x, xo, w, dw, nin, nout, dx, dxo):
 * dw += x ⊗ dout, dx += w @ dout */
static SeaValue t_matvec_back(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 10, "matvec_back");
    int64_t doo = iv(args[1]), xo = iv(args[3]), nin = iv(args[6]), nout = iv(args[7]), dxo = iv(args[9]);
    const double *g = fa(vm, args[0], doo, nout, "matvec_back");
    const double *x = fa(vm, args[2], xo, nin, "matvec_back");
    const double *w = fa(vm, args[4], 0, nin * nout, "matvec_back");
    double *dw = fa(vm, args[5], 0, nin * nout, "matvec_back");
    double *dx = fa(vm, args[8], dxo, nin, "matvec_back");
    for (int64_t i = 0; i < nin; i++) {
        double xi = x[i], acc = 0.0;
        const double *row = w + i * nout;
        double *drow = dw + i * nout;
        for (int64_t j = 0; j < nout; j++) {
            drow[j] += xi * g[j];
            acc += row[j] * g[j];
        }
        dx[i] += acc;
    }
    return SEA_NIL_VAL;
}

/* ---- batched products (matmul) ----
 *
 * gemm: C[M×N] (+)= A[M×K] · B[K×N], row-major. Blocked for the cache: K in
 * blocks of GEMM_KB, then panels of GEMM_NR columns of B (GEMM_KB×16 doubles,
 * 32 KB, stay in L1), then GEMM_MR rows of A at a time; the 8×16 tile of C
 * lives in registers. */

#define GEMM_KB 256
#define GEMM_MR 8
#define GEMM_NR 16

/* 8 doubles: one AVX-512 register (two AVX2 or four NEON registers elsewhere) */
typedef double v8d __attribute__((vector_size(64)));

static inline v8d v8_load(const double *p) { v8d v; memcpy(&v, p, sizeof v); return v; }
static inline void v8_store(double *p, v8d v) { memcpy(p, &v, sizeof v); }

static void gemm_tile(const double *a, int64_t lda, const double *b, int64_t ldb,
                      double *c, int64_t ldc, int64_t kb, int64_t mr, int64_t nr) {
    if (mr == GEMM_MR && nr == GEMM_NR) {
        /* 8×16 tile of C in 16 vector registers */
        v8d c0[GEMM_MR], c1[GEMM_MR];
        for (int m = 0; m < GEMM_MR; m++) { c0[m] = v8_load(c + m * ldc); c1[m] = v8_load(c + m * ldc + 8); }
        for (int64_t k = 0; k < kb; k++) {
            v8d b0 = v8_load(b + k * ldb), b1 = v8_load(b + k * ldb + 8);
            for (int m = 0; m < GEMM_MR; m++) {
                double x = a[m * lda + k];
                c0[m] += x * b0;
                c1[m] += x * b1;
            }
        }
        for (int m = 0; m < GEMM_MR; m++) { v8_store(c + m * ldc, c0[m]); v8_store(c + m * ldc + 8, c1[m]); }
        return;
    }
    for (int64_t m = 0; m < mr; m++) {
        double *cr = c + m * ldc;
        const double *ar = a + m * lda;
        for (int64_t k = 0; k < kb; k++) {
            double x = ar[k];
            const double *br = b + k * ldb;
            for (int64_t j = 0; j < nr; j++) cr[j] += x * br[j];
        }
    }
}

static void gemm(const double *a, const double *b, double *c, int64_t M, int64_t K, int64_t N, int accumulate) {
    if (!accumulate) memset(c, 0, (size_t)(M * N) * sizeof(double));
    for (int64_t k0 = 0; k0 < K; k0 += GEMM_KB) {
        int64_t kb = K - k0 < GEMM_KB ? K - k0 : GEMM_KB;
        for (int64_t j0 = 0; j0 < N; j0 += GEMM_NR) {
            int64_t nr = N - j0 < GEMM_NR ? N - j0 : GEMM_NR;
            for (int64_t m0 = 0; m0 < M; m0 += GEMM_MR) {
                int64_t mr = M - m0 < GEMM_MR ? M - m0 : GEMM_MR;
                gemm_tile(a + m0 * K + k0, K, b + k0 * N + j0, N, c + m0 * N + j0, N, kb, mr, nr);
            }
        }
    }
}

/* t[c×r] = transpose of m[r×c] */
static double *transpose(SeaVM *vm, const double *m, int64_t r, int64_t c, const char *fn) {
    double *t = malloc((size_t)(r * c) * sizeof(double) + 1);
    if (!t) sea_vm_error_noreturn(vm, "tensor.%s: out of memory", fn);
    for (int64_t i0 = 0; i0 < r; i0 += 32)
        for (int64_t j0 = 0; j0 < c; j0 += 32)
            for (int64_t i = i0; i < r && i < i0 + 32; i++)
                for (int64_t j = j0; j < c && j < j0 + 32; j++) t[j * r + i] = m[i * c + j];
    return t;
}

/* matmul(x, xo, w, T, nin, nout, out, oo): T rows at once —
 * out[oo + t·nout + j] = Σ_i x[xo + t·nin + i]·w[i·nout + j] */
static SeaValue t_matmul(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 8, "matmul");
    int64_t xo = iv(args[1]), T = iv(args[3]), nin = iv(args[4]), nout = iv(args[5]), oo = iv(args[7]);
    const double *x = fa(vm, args[0], xo, T * nin, "matmul");
    const double *w = fa(vm, args[2], 0, nin * nout, "matmul");
    double *out = fa(vm, args[6], oo, T * nout, "matmul");
    gemm(x, w, out, T, nin, nout, 0);
    return SEA_NIL_VAL;
}

/* matmul_back(dout, doo, x, xo, w, dw, T, nin, nout, dx, dxo):
 * dw += xᵀ · dout, dx += dout · wᵀ (dw or dx may be nil to skip it) */
static SeaValue t_matmul_back(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 11, "matmul_back");
    int64_t doo = iv(args[1]), xo = iv(args[3]), T = iv(args[6]), nin = iv(args[7]), nout = iv(args[8]), dxo = iv(args[10]);
    const double *g = fa(vm, args[0], doo, T * nout, "matmul_back");
    if (!sea_is_nil(args[5])) {
        const double *x = fa(vm, args[2], xo, T * nin, "matmul_back");
        double *dw = fa(vm, args[5], 0, nin * nout, "matmul_back");
        double *xt = transpose(vm, x, T, nin, "matmul_back");
        gemm(xt, g, dw, nin, T, nout, 1);
        free(xt);
    }
    if (!sea_is_nil(args[9])) {
        const double *w = fa(vm, args[4], 0, nin * nout, "matmul_back");
        double *dx = fa(vm, args[9], dxo, T * nin, "matmul_back");
        double *wt = transpose(vm, w, nin, nout, "matmul_back");
        gemm(g, wt, dx, T, nout, nin, 1);
        free(wt);
    }
    return SEA_NIL_VAL;
}

/* matvec_cols(x, xo, w, nin, nout, cols, ncols, out, oo): only the outputs
 * listed in cols (int_array): out[oo+k] = Σ_i x[i]·w[i·nout + cols[k]] */
static SeaValue t_matvec_cols(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 9, "matvec_cols");
    int64_t xo = iv(args[1]), nin = iv(args[3]), nout = iv(args[4]), nc = iv(args[6]), oo = iv(args[8]);
    const double *x = fa(vm, args[0], xo, nin, "matvec_cols");
    const double *w = fa(vm, args[2], 0, nin * nout, "matvec_cols");
    const int64_t *cols = ia(vm, args[5], nc, "matvec_cols");
    double *out = fa(vm, args[7], oo, nc, "matvec_cols");
    for (int64_t k = 0; k < nc; k++) {
        if (cols[k] < 0 || cols[k] >= nout) sea_vm_error_noreturn(vm, "tensor.matvec_cols: column out of range");
        out[k] = 0.0;
    }
    for (int64_t i = 0; i < nin; i++) {
        double xi = x[i];
        const double *row = w + i * nout;
        for (int64_t k = 0; k < nc; k++) out[k] += xi * row[cols[k]];
    }
    return SEA_NIL_VAL;
}

/* matvec_cols_back(dout, doo, x, xo, w, dw, nin, nout, cols, ncols, dx, dxo) */
static SeaValue t_matvec_cols_back(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 12, "matvec_cols_back");
    int64_t doo = iv(args[1]), xo = iv(args[3]), nin = iv(args[6]), nout = iv(args[7]);
    int64_t nc = iv(args[9]), dxo = iv(args[11]);
    const double *g = fa(vm, args[0], doo, nc, "matvec_cols_back");
    const double *x = fa(vm, args[2], xo, nin, "matvec_cols_back");
    const double *w = fa(vm, args[4], 0, nin * nout, "matvec_cols_back");
    double *dw = fa(vm, args[5], 0, nin * nout, "matvec_cols_back");
    const int64_t *cols = ia(vm, args[8], nc, "matvec_cols_back");
    double *dx = fa(vm, args[10], dxo, nin, "matvec_cols_back");
    for (int64_t i = 0; i < nin; i++) {
        double xi = x[i], acc = 0.0;
        for (int64_t k = 0; k < nc; k++) {
            int64_t j = i * nout + cols[k];
            dw[j] += xi * g[k];
            acc += w[j] * g[k];
        }
        dx[i] += acc;
    }
    return SEA_NIL_VAL;
}

/* ---- normalization and activations ---- */

/* rmsnorm(x, xo, y, yo, n) → r = 1/rms(x); y = x·r */
static SeaValue t_rmsnorm(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 5, "rmsnorm");
    int64_t xo = iv(args[1]), yo = iv(args[3]), n = iv(args[4]);
    const double *x = fa(vm, args[0], xo, n, "rmsnorm");
    double *y = fa(vm, args[2], yo, n, "rmsnorm");
    double ss = 0.0;
    for (int64_t i = 0; i < n; i++) ss += x[i] * x[i];
    double r = 1.0 / sqrt(ss / (double)n + 1e-5);
    for (int64_t i = 0; i < n; i++) y[i] = x[i] * r;
    return sea_number_val(r);
}

/* rmsnorm_back(dy, dyo, x, xo, r, n, dx, dxo): dx += r·dy − x·r³·(dy·x)/n */
static SeaValue t_rmsnorm_back(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 8, "rmsnorm_back");
    int64_t dyo = iv(args[1]), xo = iv(args[3]), n = iv(args[5]), dxo = iv(args[7]);
    double r = dv(args[4]);
    const double *dy = fa(vm, args[0], dyo, n, "rmsnorm_back");
    const double *x = fa(vm, args[2], xo, n, "rmsnorm_back");
    double *dx = fa(vm, args[6], dxo, n, "rmsnorm_back");
    double dot = 0.0;
    for (int64_t i = 0; i < n; i++) dot += dy[i] * x[i];
    double k = r * r * r * dot / (double)n;
    for (int64_t i = 0; i < n; i++) dx[i] += r * dy[i] - x[i] * k;
    return SEA_NIL_VAL;
}

/* relu(x, xo, y, yo, n): y = max(x, 0) */
static SeaValue t_relu(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 5, "relu");
    int64_t xo = iv(args[1]), yo = iv(args[3]), n = iv(args[4]);
    const double *x = fa(vm, args[0], xo, n, "relu");
    double *y = fa(vm, args[2], yo, n, "relu");
    for (int64_t i = 0; i < n; i++) y[i] = x[i] > 0.0 ? x[i] : 0.0;
    return SEA_NIL_VAL;
}

/* relu_back(d, dofs, x, xo, n): d = 0 where x ≤ 0 */
static SeaValue t_relu_back(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 5, "relu_back");
    int64_t dofs = iv(args[1]), xo = iv(args[3]), n = iv(args[4]);
    double *d = fa(vm, args[0], dofs, n, "relu_back");
    const double *x = fa(vm, args[2], xo, n, "relu_back");
    for (int64_t i = 0; i < n; i++) if (x[i] <= 0.0) d[i] = 0.0;
    return SEA_NIL_VAL;
}

/* softmax(p, off, n): in place, numerically stable */
static SeaValue t_softmax(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 3, "softmax");
    int64_t off = iv(args[1]), n = iv(args[2]);
    double *p = fa(vm, args[0], off, n, "softmax");
    double mx = -1e300;
    for (int64_t i = 0; i < n; i++) if (p[i] > mx) mx = p[i];
    double sum = 0.0;
    for (int64_t i = 0; i < n; i++) { p[i] = exp(p[i] - mx); sum += p[i]; }
    for (int64_t i = 0; i < n; i++) p[i] /= sum;
    return SEA_NIL_VAL;
}

/* ---- attention ---- */

/* attention(q, k, v, att, y, T, C, H, B): causal multi-head attention of one
 * layer. q, k, v, y hold T rows of C (row t at t·C); head h uses columns
 * [h·C/H, (h+1)·C/H). att (H·B·B) keeps the probabilities for the backward:
 * att[(h·B + t)·B + s]. */
static SeaValue t_attention(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 9, "attention");
    int64_t T = iv(args[5]), C = iv(args[6]), H = iv(args[7]), B = iv(args[8]);
    int64_t HS = C / H;
    const double *q = fa(vm, args[0], 0, T * C, "attention");
    const double *k = fa(vm, args[1], 0, T * C, "attention");
    const double *v = fa(vm, args[2], 0, T * C, "attention");
    double *att = fa(vm, args[3], 0, H * B * B, "attention");
    double *y = fa(vm, args[4], 0, T * C, "attention");
    if (T > B) sea_vm_error_noreturn(vm, "tensor.attention: %lld positions, context %lld", (long long)T, (long long)B);
    double scale = 1.0 / sqrt((double)HS);
    for (int64_t h = 0; h < H; h++) {
        int64_t ho = h * HS;
        for (int64_t t = 0; t < T; t++) {
            double *a = att + (h * B + t) * B;
            const double *qt = q + t * C + ho;
            double mx = -1e300;
            for (int64_t s = 0; s <= t; s++) {
                const double *ks = k + s * C + ho;
                double dot = 0.0;
                for (int64_t d = 0; d < HS; d++) dot += qt[d] * ks[d];
                dot *= scale;
                a[s] = dot;
                if (dot > mx) mx = dot;
            }
            double sum = 0.0;
            for (int64_t s = 0; s <= t; s++) { a[s] = exp(a[s] - mx); sum += a[s]; }
            double *yt = y + t * C + ho;
            for (int64_t d = 0; d < HS; d++) yt[d] = 0.0;
            for (int64_t s = 0; s <= t; s++) {
                double pr = a[s] / sum;
                a[s] = pr;
                const double *vs = v + s * C + ho;
                for (int64_t d = 0; d < HS; d++) yt[d] += pr * vs[d];
            }
        }
    }
    return SEA_NIL_VAL;
}

/* attention_back(q, k, v, att, dy, dq, dk, dv, T, C, H, B): accumulates into
 * dq, dk, dv the gradients of the last attention (the caller zeroes them). */
static SeaValue t_attention_back(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 12, "attention_back");
    int64_t T = iv(args[8]), C = iv(args[9]), H = iv(args[10]), B = iv(args[11]);
    int64_t HS = C / H;
    const double *q = fa(vm, args[0], 0, T * C, "attention_back");
    const double *k = fa(vm, args[1], 0, T * C, "attention_back");
    const double *v = fa(vm, args[2], 0, T * C, "attention_back");
    const double *att = fa(vm, args[3], 0, H * B * B, "attention_back");
    const double *dy = fa(vm, args[4], 0, T * C, "attention_back");
    double *dq = fa(vm, args[5], 0, T * C, "attention_back");
    double *dk = fa(vm, args[6], 0, T * C, "attention_back");
    double *dvv = fa(vm, args[7], 0, T * C, "attention_back");
    double scale = 1.0 / sqrt((double)HS);
    double stack_tmp[256];
    double *dp = T <= 256 ? stack_tmp : (double *)malloc(sizeof(double) * (size_t)T);
    for (int64_t h = 0; h < H; h++) {
        int64_t ho = h * HS;
        for (int64_t t = 0; t < T; t++) {
            const double *a = att + (h * B + t) * B;
            const double *dyt = dy + t * C + ho;
            double pdp = 0.0;
            for (int64_t s = 0; s <= t; s++) {
                const double *vs = v + s * C + ho;
                double *dvs = dvv + s * C + ho;
                double d = 0.0, pr = a[s];
                for (int64_t e = 0; e < HS; e++) {
                    d += dyt[e] * vs[e];
                    dvs[e] += pr * dyt[e];
                }
                dp[s] = d;
                pdp += pr * d;
            }
            const double *qt = q + t * C + ho;
            double *dqt = dq + t * C + ho;
            for (int64_t s = 0; s <= t; s++) {
                double ds = a[s] * (dp[s] - pdp) * scale;
                const double *ks = k + s * C + ho;
                double *dks = dk + s * C + ho;
                for (int64_t e = 0; e < HS; e++) {
                    dqt[e] += ds * ks[e];
                    dks[e] += ds * qt[e];
                }
            }
        }
    }
    if (dp != stack_tmp) free(dp);
    return SEA_NIL_VAL;
}

/* ---- vectors ---- */

/* zero(a [, off, n]) */
static SeaValue t_zero(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 1, "zero");
    if (!sea_is_float_array(args[0])) sea_vm_error_noreturn(vm, "tensor.zero: expected a float_array");
    int64_t len = SEA_AS_FLOAT_ARRAY(args[0])->len;
    int64_t off = argc > 1 ? iv(args[1]) : 0, n = argc > 2 ? iv(args[2]) : len - off;
    double *a = fa(vm, args[0], off, n, "zero");
    memset(a, 0, sizeof(double) * (size_t)n);
    return SEA_NIL_VAL;
}

/* add(dst, doff, src, soff, n): dst[doff..] += src[soff..] */
static SeaValue t_add(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 5, "add");
    int64_t doff = iv(args[1]), soff = iv(args[3]), n = iv(args[4]);
    double *d = fa(vm, args[0], doff, n, "add");
    const double *s = fa(vm, args[2], soff, n, "add");
    for (int64_t i = 0; i < n; i++) d[i] += s[i];
    return SEA_NIL_VAL;
}

/* copy(dst, doff, src, soff, n) */
static SeaValue t_copy(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 5, "copy");
    int64_t doff = iv(args[1]), soff = iv(args[3]), n = iv(args[4]);
    double *d = fa(vm, args[0], doff, n, "copy");
    const double *s = fa(vm, args[2], soff, n, "copy");
    memmove(d, s, sizeof(double) * (size_t)n);
    return SEA_NIL_VAL;
}

/* scale(a, off, n, k): a[off+i] *= k */
static SeaValue t_scale(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 4, "scale");
    int64_t off = iv(args[1]), n = iv(args[2]);
    double k = dv(args[3]);
    double *a = fa(vm, args[0], off, n, "scale");
    for (int64_t i = 0; i < n; i++) a[i] *= k;
    return SEA_NIL_VAL;
}

/* adam(w, g, m, v, n, lr, beta1, beta2, c1, c2, eps): one Adam step, with
 * the bias corrections c1 = 1 − beta1^t, c2 = 1 − beta2^t */
static SeaValue t_adam(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 11, "adam");
    int64_t n = iv(args[4]);
    double *w = fa(vm, args[0], 0, n, "adam");
    const double *g = fa(vm, args[1], 0, n, "adam");
    double *m = fa(vm, args[2], 0, n, "adam");
    double *v = fa(vm, args[3], 0, n, "adam");
    double lr = dv(args[5]), b1 = dv(args[6]), b2 = dv(args[7]), c1 = dv(args[8]), c2 = dv(args[9]), eps = dv(args[10]);
    for (int64_t i = 0; i < n; i++) {
        double mi = b1 * m[i] + (1.0 - b1) * g[i];
        double vi = b2 * v[i] + (1.0 - b2) * g[i] * g[i];
        m[i] = mi;
        v[i] = vi;
        w[i] -= lr * (mi / c1) / (sqrt(vi / c2) + eps);
    }
    return SEA_NIL_VAL;
}

/* ---- files ---- */

/* save(path, a [, append]): writes the float_array as raw doubles
 * (native byte order); with append = true, appends. Returns the bytes
 * written, or an error value. */
static SeaValue t_save(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 2, "save");
    if (!sea_is_string(args[0])) return sea_make_error_str(vm, "tensor.save: expected a path");
    const double *a = fa(vm, args[1], 0, 0, "save");
    int64_t n = SEA_AS_FLOAT_ARRAY(args[1])->len;
    FILE *f = fopen(SEA_AS_STRING(args[0])->data, argc > 2 && sea_is_truthy(args[2]) ? "ab" : "wb");
    if (!f) return sea_make_error_str(vm, "tensor.save: cannot open the file");
    size_t w = fwrite(a, sizeof(double), (size_t)n, f);
    int err = fclose(f);
    if (w != (size_t)n || err != 0) return sea_make_error_str(vm, "tensor.save: write failed");
    return sea_int_val((int64_t)(w * sizeof(double)));
}

/* load(path, a, offset): fills the float_array (its whole length)
 * with doubles read at byte offset `offset`. Returns the next offset,
 * or an error value. */
static SeaValue t_load(SeaVM *vm, int argc, SeaValue *args) {
    need(vm, argc, 3, "load");
    if (!sea_is_string(args[0])) return sea_make_error_str(vm, "tensor.load: expected a path");
    double *a = fa(vm, args[1], 0, 0, "load");
    int64_t n = SEA_AS_FLOAT_ARRAY(args[1])->len, off = iv(args[2]);
    FILE *f = fopen(SEA_AS_STRING(args[0])->data, "rb");
    if (!f) return sea_make_error_str(vm, "tensor.load: cannot open the file");
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return sea_make_error_str(vm, "tensor.load: bad offset"); }
    size_t r = fread(a, sizeof(double), (size_t)n, f);
    fclose(f);
    if (r != (size_t)n) return sea_make_error_str(vm, "tensor.load: file too short");
    return sea_int_val(off + (int64_t)(r * sizeof(double)));
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
    return sea_obj_val(&mod->obj);
}
