/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * tensor_kernels.h — the numeric loops of tensor.c, written once for an
 * element type. tensor.c includes this file twice:
 *
 *   #define REAL double  + SFX _f64 + LANES 8    (float_array)
 *   #define REAL float   + SFX _f32 + LANES 16   (float32_array)
 *
 * K(name) gives the suffixed name (matmul_f64, matmul_f32). Reductions (sums
 * of squares, softmax sums, dot products of the attention) accumulate in
 * double for both types; the matrix products accumulate in REAL, as BLAS does.
 */

#define CAT2_(a, b) a##b
#define CAT_(a, b) CAT2_(a, b)
#define K(name) CAT_(name, SFX)

/* one 64-byte vector: an AVX-512 register (split into narrower registers elsewhere) */
typedef REAL K(vec) __attribute__((vector_size(64)));

static inline K(vec) K(vload)(const REAL *p) { K(vec) v; memcpy(&v, p, sizeof v); return v; }
static inline void K(vstore)(REAL *p, K(vec) v) { memcpy(p, &v, sizeof v); }

/* ---- matrix-vector ---- */

static void K(matvec)(const REAL *x, const REAL *w, int64_t nin, int64_t nout, REAL *out) {
    for (int64_t j = 0; j < nout; j++) out[j] = 0;
    for (int64_t i = 0; i < nin; i++) {
        REAL xi = x[i];
        if (xi == 0) continue;
        const REAL *row = w + i * nout;
        for (int64_t j = 0; j < nout; j++) out[j] += xi * row[j];
    }
}

static void K(matvec_back)(const REAL *g, const REAL *x, const REAL *w, REAL *dw,
                           int64_t nin, int64_t nout, REAL *dx) {
    for (int64_t i = 0; i < nin; i++) {
        REAL xi = x[i], acc = 0;
        const REAL *row = w + i * nout;
        REAL *drow = dw + i * nout;
        for (int64_t j = 0; j < nout; j++) {
            drow[j] += xi * g[j];
            acc += row[j] * g[j];
        }
        dx[i] += acc;
    }
}

static void K(matvec_cols)(const REAL *x, const REAL *w, int64_t nin, int64_t nout,
                           const int64_t *cols, int64_t nc, REAL *out) {
    for (int64_t k = 0; k < nc; k++) out[k] = 0;
    for (int64_t i = 0; i < nin; i++) {
        REAL xi = x[i];
        const REAL *row = w + i * nout;
        for (int64_t k = 0; k < nc; k++) out[k] += xi * row[cols[k]];
    }
}

static void K(matvec_cols_back)(const REAL *g, const REAL *x, const REAL *w, REAL *dw, int64_t nin,
                                int64_t nout, const int64_t *cols, int64_t nc, REAL *dx) {
    for (int64_t i = 0; i < nin; i++) {
        REAL xi = x[i], acc = 0;
        for (int64_t k = 0; k < nc; k++) {
            int64_t j = i * nout + cols[k];
            dw[j] += xi * g[k];
            acc += w[j] * g[k];
        }
        dx[i] += acc;
    }
}

/* ---- matrix products ----
 *
 * gemm: C[M×N] (+)= A[M×K] · B[K×N], row-major. Blocked for the cache: K in
 * blocks of GEMM_KB, then panels of NR = 2·LANES columns of B (GEMM_KB×NR
 * elements, 32 KB for doubles and for floats, stay in L1), then GEMM_MR rows
 * of A at a time; the GEMM_MR×NR tile of C lives in 16 vector registers. */

#define K_NR (2 * LANES)

static void K(gemm_tile)(const REAL *a, int64_t lda, const REAL *b, int64_t ldb,
                         REAL *c, int64_t ldc, int64_t kb, int64_t mr, int64_t nr) {
    if (mr == GEMM_MR && nr == K_NR) {
        K(vec) c0[GEMM_MR], c1[GEMM_MR];
        for (int m = 0; m < GEMM_MR; m++) { c0[m] = K(vload)(c + m * ldc); c1[m] = K(vload)(c + m * ldc + LANES); }
        for (int64_t k = 0; k < kb; k++) {
            K(vec) b0 = K(vload)(b + k * ldb), b1 = K(vload)(b + k * ldb + LANES);
            for (int m = 0; m < GEMM_MR; m++) {
                REAL x = a[m * lda + k];
                c0[m] += x * b0;
                c1[m] += x * b1;
            }
        }
        for (int m = 0; m < GEMM_MR; m++) { K(vstore)(c + m * ldc, c0[m]); K(vstore)(c + m * ldc + LANES, c1[m]); }
        return;
    }
    for (int64_t m = 0; m < mr; m++) {
        REAL *cr = c + m * ldc;
        const REAL *ar = a + m * lda;
        for (int64_t k = 0; k < kb; k++) {
            REAL x = ar[k];
            const REAL *br = b + k * ldb;
            for (int64_t j = 0; j < nr; j++) cr[j] += x * br[j];
        }
    }
}

/* C (+)= A·B with leading dimensions (A rows of lda, B rows of ldb, C rows of ldc) */
static void K(gemm_ld)(const REAL *a, int64_t lda, const REAL *b, int64_t ldb, REAL *c, int64_t ldc,
                       int64_t M, int64_t Kd, int64_t N, int accumulate) {
    if (!accumulate)
        for (int64_t m = 0; m < M; m++) memset(c + m * ldc, 0, (size_t)N * sizeof(REAL));
    for (int64_t k0 = 0; k0 < Kd; k0 += GEMM_KB) {
        int64_t kb = Kd - k0 < GEMM_KB ? Kd - k0 : GEMM_KB;
        for (int64_t j0 = 0; j0 < N; j0 += K_NR) {
            int64_t nr = N - j0 < K_NR ? N - j0 : K_NR;
            for (int64_t m0 = 0; m0 < M; m0 += GEMM_MR) {
                int64_t mr = M - m0 < GEMM_MR ? M - m0 : GEMM_MR;
                K(gemm_tile)(a + m0 * lda + k0, lda, b + k0 * ldb + j0, ldb, c + m0 * ldc + j0, ldc, kb, mr, nr);
            }
        }
    }
}

/* one thread's share of a gemm: a band of columns (whole tile panels) or of rows of C */
typedef struct {
    const REAL *a, *b;
    REAL *c;
    int64_t M, Kd, N;
    int accumulate, by_cols;
} K(gemm_job);

static void K(gemm_part)(void *arg, int part, int nparts) {
    K(gemm_job) *j = arg;
    if (j->by_cols) {
        int64_t panels = (j->N + K_NR - 1) / K_NR;
        int64_t p0 = panels * part / nparts, p1 = panels * (part + 1) / nparts;
        int64_t c0 = p0 * K_NR, c1 = p1 * K_NR < j->N ? p1 * K_NR : j->N;
        if (c1 > c0) K(gemm_ld)(j->a, j->Kd, j->b + c0, j->N, j->c + c0, j->N, j->M, j->Kd, c1 - c0, j->accumulate);
    } else {
        int64_t blocks = (j->M + GEMM_MR - 1) / GEMM_MR;
        int64_t b0 = blocks * part / nparts, b1 = blocks * (part + 1) / nparts;
        int64_t r0 = b0 * GEMM_MR, r1 = b1 * GEMM_MR < j->M ? b1 * GEMM_MR : j->M;
        if (r1 > r0) K(gemm_ld)(j->a + r0 * j->Kd, j->Kd, j->b, j->N, j->c + r0 * j->N, j->N, r1 - r0, j->Kd, j->N, j->accumulate);
    }
}

/* C[M×N] (+)= A[M×K]·B[K×N], split across the thread pool when it is large enough */
static void K(gemm)(const REAL *a, const REAL *b, REAL *c, int64_t M, int64_t Kd, int64_t N, int accumulate) {
    int nt = pool_threads();
    if (nt <= 1 || M * N * Kd < GEMM_PAR_MIN) {
        K(gemm_ld)(a, Kd, b, N, c, N, M, Kd, N, accumulate);
        return;
    }
    K(gemm_job) j = {a, b, c, M, Kd, N, accumulate, 0};
    int64_t panels = (N + K_NR - 1) / K_NR, blocks = (M + GEMM_MR - 1) / GEMM_MR;
    j.by_cols = panels >= blocks;   /* split the longer side: more, evener parts */
    int64_t parts = j.by_cols ? panels : blocks;
    if (parts < nt) nt = (int)parts;
    pool_run(K(gemm_part), &j, nt);
}

/* t[c×r] = transpose of m[r×c], rows [i0, i1) of m */
static void K(transpose_rows)(const REAL *m, REAL *t, int64_t r, int64_t c, int64_t i0, int64_t i1) {
    for (int64_t b0 = i0; b0 < i1; b0 += 32)
        for (int64_t j0 = 0; j0 < c; j0 += 32)
            for (int64_t i = b0; i < i1 && i < b0 + 32; i++)
                for (int64_t j = j0; j < c && j < j0 + 32; j++) t[j * r + i] = m[i * c + j];
}

typedef struct { const REAL *m; REAL *t; int64_t r, c; } K(transpose_job);

static void K(transpose_part)(void *arg, int part, int nparts) {
    K(transpose_job) *j = arg;
    int64_t blocks = (j->r + 31) / 32;
    int64_t i0 = blocks * part / nparts * 32, i1 = blocks * (part + 1) / nparts * 32;
    if (i1 > j->r) i1 = j->r;
    if (i1 > i0) K(transpose_rows)(j->m, j->t, j->r, j->c, i0, i1);
}

/* t[c×r] = transpose of m[r×c] (on the thread pool when large); NULL when out of memory */
static REAL *K(transpose)(const REAL *m, int64_t r, int64_t c) {
    REAL *t = malloc((size_t)(r * c) * sizeof(REAL) + 1);
    if (!t) return NULL;
    int nt = pool_threads();
    int64_t blocks = (r + 31) / 32;
    if (nt > blocks) nt = (int)blocks;
    if (nt <= 1 || r * c < (1 << 16)) {
        K(transpose_rows)(m, t, r, c, 0, r);
    } else {
        K(transpose_job) j = {m, t, r, c};
        pool_run(K(transpose_part), &j, nt);
    }
    return t;
}

/* ---- normalization and activations ---- */

static double K(rmsnorm)(const REAL *x, REAL *y, int64_t n) {
    double ss = 0.0;
    for (int64_t i = 0; i < n; i++) ss += (double)x[i] * x[i];
    double r = 1.0 / sqrt(ss / (double)n + 1e-5);
    for (int64_t i = 0; i < n; i++) y[i] = (REAL)(x[i] * r);
    return r;
}

static void K(rmsnorm_back)(const REAL *dy, const REAL *x, double r, int64_t n, REAL *dx) {
    double dot = 0.0;
    for (int64_t i = 0; i < n; i++) dot += (double)dy[i] * x[i];
    double k = r * r * r * dot / (double)n;
    for (int64_t i = 0; i < n; i++) dx[i] += (REAL)(r * dy[i] - x[i] * k);
}

static void K(relu)(const REAL *x, REAL *y, int64_t n) {
    for (int64_t i = 0; i < n; i++) y[i] = x[i] > 0 ? x[i] : 0;
}

static void K(relu_back)(REAL *d, const REAL *x, int64_t n) {
    for (int64_t i = 0; i < n; i++) if (x[i] <= 0) d[i] = 0;
}

static void K(softmax)(REAL *p, int64_t n) {
    double mx = -1e300;
    for (int64_t i = 0; i < n; i++) if (p[i] > mx) mx = p[i];
    double sum = 0.0;
    for (int64_t i = 0; i < n; i++) { double e = exp(p[i] - mx); p[i] = (REAL)e; sum += e; }
    double inv = 1.0 / sum;
    for (int64_t i = 0; i < n; i++) p[i] = (REAL)(p[i] * inv);
}

/* ---- attention ---- */

static void K(attention)(const REAL *q, const REAL *k, const REAL *v, REAL *att, REAL *y,
                         int64_t T, int64_t C, int64_t H, int64_t B) {
    int64_t HS = C / H;
    double scale = 1.0 / sqrt((double)HS);
    for (int64_t h = 0; h < H; h++) {
        int64_t ho = h * HS;
        for (int64_t t = 0; t < T; t++) {
            REAL *a = att + (h * B + t) * B;
            const REAL *qt = q + t * C + ho;
            double mx = -1e300;
            for (int64_t s = 0; s <= t; s++) {
                const REAL *ks = k + s * C + ho;
                double dot = 0.0;
                for (int64_t d = 0; d < HS; d++) dot += (double)qt[d] * ks[d];
                dot *= scale;
                a[s] = (REAL)dot;
                if (dot > mx) mx = dot;
            }
            double sum = 0.0;
            for (int64_t s = 0; s <= t; s++) { double e = exp(a[s] - mx); a[s] = (REAL)e; sum += e; }
            REAL *yt = y + t * C + ho;
            for (int64_t d = 0; d < HS; d++) yt[d] = 0;
            for (int64_t s = 0; s <= t; s++) {
                REAL pr = (REAL)(a[s] / sum);
                a[s] = pr;
                const REAL *vs = v + s * C + ho;
                for (int64_t d = 0; d < HS; d++) yt[d] += pr * vs[d];
            }
        }
    }
}

/* dp: scratch of T doubles */
static void K(attention_back)(const REAL *q, const REAL *k, const REAL *v, const REAL *att,
                              const REAL *dy, REAL *dq, REAL *dk, REAL *dvv,
                              int64_t T, int64_t C, int64_t H, int64_t B, double *dp) {
    int64_t HS = C / H;
    double scale = 1.0 / sqrt((double)HS);
    for (int64_t h = 0; h < H; h++) {
        int64_t ho = h * HS;
        for (int64_t t = 0; t < T; t++) {
            const REAL *a = att + (h * B + t) * B;
            const REAL *dyt = dy + t * C + ho;
            double pdp = 0.0;
            for (int64_t s = 0; s <= t; s++) {
                const REAL *vs = v + s * C + ho;
                REAL *dvs = dvv + s * C + ho;
                double d = 0.0;
                REAL pr = a[s];
                for (int64_t e = 0; e < HS; e++) {
                    d += (double)dyt[e] * vs[e];
                    dvs[e] += pr * dyt[e];
                }
                dp[s] = d;
                pdp += pr * d;
            }
            const REAL *qt = q + t * C + ho;
            REAL *dqt = dq + t * C + ho;
            for (int64_t s = 0; s <= t; s++) {
                REAL ds = (REAL)(a[s] * (dp[s] - pdp) * scale);
                const REAL *ks = k + s * C + ho;
                REAL *dks = dk + s * C + ho;
                for (int64_t e = 0; e < HS; e++) {
                    dqt[e] += ds * ks[e];
                    dks[e] += ds * qt[e];
                }
            }
        }
    }
}

/* The attention of the last of T positions (its query q) against the keys and
 * values of all T (a KV cache): the same operations as row T-1 of attention,
 * without keeping the probabilities. p: scratch of T elements. */
static void K(attention_step)(const REAL *q, const REAL *k, const REAL *v, REAL *y,
                              int64_t T, int64_t C, int64_t H, REAL *p) {
    int64_t HS = C / H;
    double scale = 1.0 / sqrt((double)HS);
    for (int64_t h = 0; h < H; h++) {
        int64_t ho = h * HS;
        const REAL *qt = q + ho;
        double mx = -1e300;
        for (int64_t s = 0; s < T; s++) {
            const REAL *ks = k + s * C + ho;
            double dot = 0.0;
            for (int64_t d = 0; d < HS; d++) dot += (double)qt[d] * ks[d];
            dot *= scale;
            p[s] = (REAL)dot;
            if (dot > mx) mx = dot;
        }
        double sum = 0.0;
        for (int64_t s = 0; s < T; s++) { double e = exp(p[s] - mx); p[s] = (REAL)e; sum += e; }
        REAL *yt = y + ho;
        for (int64_t d = 0; d < HS; d++) yt[d] = 0;
        for (int64_t s = 0; s < T; s++) {
            REAL pr = (REAL)(p[s] / sum);
            const REAL *vs = v + s * C + ho;
            for (int64_t d = 0; d < HS; d++) yt[d] += pr * vs[d];
        }
    }
}

/* ---- columns (embedding lookups, sampled softmax) ---- */

/* dst = the columns cols[0..nc) of src (nrows × ncols): dst[i·nc + k], or
 * transposed, one column per row: dst[k·nrows + i] */
static void K(gather_cols)(const REAL *src, int64_t nrows, int64_t ncols, const int64_t *cols, int64_t nc,
                           REAL *dst, int trans) {
    (void)ncols;
    if (trans) {
        for (int64_t k = 0; k < nc; k++) {
            const REAL *col = src + cols[k];
            REAL *out = dst + k * nrows;
            for (int64_t i = 0; i < nrows; i++) out[i] = col[i * ncols];
        }
    } else {
        for (int64_t i = 0; i < nrows; i++) {
            const REAL *row = src + i * ncols;
            REAL *out = dst + i * nc;
            for (int64_t k = 0; k < nc; k++) out[k] = row[cols[k]];
        }
    }
}

/* the reverse, accumulating (a column listed twice gets both) */
static void K(scatter_cols)(REAL *dst, int64_t nrows, int64_t ncols, const int64_t *cols, int64_t nc,
                            const REAL *src, int trans) {
    if (trans) {
        for (int64_t k = 0; k < nc; k++) {
            REAL *col = dst + cols[k];
            const REAL *in = src + k * nrows;
            for (int64_t i = 0; i < nrows; i++) col[i * ncols] += in[i];
        }
    } else {
        for (int64_t i = 0; i < nrows; i++) {
            REAL *row = dst + i * ncols;
            const REAL *in = src + i * nc;
            for (int64_t k = 0; k < nc; k++) row[cols[k]] += in[k];
        }
    }
}

/* ---- vectors ---- */

static void K(add)(REAL *d, const REAL *s, int64_t n) {
    for (int64_t i = 0; i < n; i++) d[i] += s[i];
}

/* Σ a[i]², accumulated in double (four partial sums, so it vectorizes) */
static double K(sumsq)(const REAL *a, int64_t n) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int64_t i = 0;
    for (; i + 4 <= n; i += 4) {
        double x0 = a[i], x1 = a[i + 1], x2 = a[i + 2], x3 = a[i + 3];
        s0 += x0 * x0;
        s1 += x1 * x1;
        s2 += x2 * x2;
        s3 += x3 * x3;
    }
    for (; i < n; i++) s0 += (double)a[i] * a[i];
    return (s0 + s1) + (s2 + s3);
}

static void K(scale)(REAL *a, int64_t n, double k) {
    REAL kk = (REAL)k;
    for (int64_t i = 0; i < n; i++) a[i] *= kk;
}

static void K(adam)(REAL *w, const REAL *g, REAL *m, REAL *v, int64_t n,
                    double lr, double b1, double b2, double c1, double c2, double eps) {
    for (int64_t i = 0; i < n; i++) {
        double gi = g[i];
        double mi = b1 * m[i] + (1.0 - b1) * gi;
        double vi = b2 * v[i] + (1.0 - b2) * gi * gi;
        m[i] = (REAL)mi;
        v[i] = (REAL)vi;
        w[i] = (REAL)(w[i] - lr * (mi / c1) / (sqrt(vi / c2) + eps));
    }
}

#undef K_NR
#undef K
#undef CAT_
#undef CAT2_
