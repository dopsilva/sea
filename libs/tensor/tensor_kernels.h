/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * tensor_kernels.h — the numeric loops of tensor.c, written once for an
 * element type. tensor.c includes this file twice:
 *
 *   #define REAL double  + SFX _f64 + LANES VEC_BYTES/8   (float_array)
 *   #define REAL float   + SFX _f32 + LANES VEC_BYTES/4   (float32_array)
 *
 * K(name) gives the suffixed name (matmul_f64, matmul_f32). Reductions (sums
 * of squares, softmax sums, dot products of the attention) accumulate in
 * double for both types; the matrix products accumulate in REAL, as BLAS does.
 */

#define CAT2_(a, b) a##b
#define CAT_(a, b) CAT2_(a, b)
#define K(name) CAT_(name, SFX)

/* one vector register (VEC_BYTES: 64 with AVX-512, 32 with AVX, 16 otherwise) */
typedef REAL K(vec) __attribute__((vector_size(VEC_BYTES)));

static inline K(vec) K(vload)(const REAL *p) { K(vec) v; memcpy(&v, p, sizeof v); return v; }
static inline void K(vstore)(REAL *p, K(vec) v) { memcpy(p, &v, sizeof v); }

/* ---- matrix-vector ---- */

/* the outputs [j0, j1) of matvec */
static void K(matvec_range)(const REAL *x, const REAL *w, int64_t nin, int64_t nout, REAL *out,
                            int64_t j0, int64_t j1) {
    for (int64_t j = j0; j < j1; j++) out[j] = 0;
    for (int64_t i = 0; i < nin; i++) {
        REAL xi = x[i];
        if (xi == 0) continue;
        const REAL *row = w + i * nout;
        for (int64_t j = j0; j < j1; j++) out[j] += xi * row[j];
    }
}

typedef struct { const REAL *x, *w; REAL *out; int64_t nin, nout; } K(matvec_job);

/* one thread's band of outputs, in whole blocks of MATVEC_BLOCK columns */
static void K(matvec_part)(void *arg, int part, int nparts) {
    K(matvec_job) *j = arg;
    int64_t blocks = (j->nout + MATVEC_BLOCK - 1) / MATVEC_BLOCK;
    int64_t b0 = blocks * part / nparts, b1 = blocks * (part + 1) / nparts;
    int64_t j0 = b0 * MATVEC_BLOCK, j1 = b1 * MATVEC_BLOCK < j->nout ? b1 * MATVEC_BLOCK : j->nout;
    if (j1 > j0) K(matvec_range)(j->x, j->w, j->nin, j->nout, j->out, j0, j1);
}

/* out = x @ w; large products split their outputs across the thread pool (each output is
 * computed by the same operations, so the result does not depend on the number of threads) */
static void K(matvec)(const REAL *x, const REAL *w, int64_t nin, int64_t nout, REAL *out) {
    int nt = pool_threads();
    if (nt <= 1 || nin * nout < GEMM_PAR_MIN) {
        K(matvec_range)(x, w, nin, nout, out, 0, nout);
        return;
    }
    int64_t blocks = (nout + MATVEC_BLOCK - 1) / MATVEC_BLOCK;
    if (blocks < nt) nt = (int)blocks;
    K(matvec_job) j = {x, w, out, nin, nout};
    pool_run(K(matvec_part), &j, nt);
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

/* ---- int8 weights ----
 *
 * A matrix nin × nout quantized by input row: q[i·nout + j] = round(w[i·nout + j] / s[i]) with
 * s[i] = max_j |w[i·nout + j]| / 127. Then out[j] = Σ_i (x[i]·s[i])·q[i·nout + j]: the scale
 * leaves the sum, and the matrix takes a quarter of the memory of floats. */
static void K(quantize_q8)(const REAL *w, int64_t nin, int64_t nout, int8_t *q, REAL *sc) {
    for (int64_t i = 0; i < nin; i++) {
        const REAL *row = w + i * nout;
        double mx = 0.0;
        for (int64_t j = 0; j < nout; j++) {
            double a = fabs((double)row[j]);
            if (a > mx) mx = a;
        }
        double s = mx / 127.0;
        sc[i] = (REAL)s;
        int8_t *qr = q + i * nout;
        for (int64_t j = 0; j < nout; j++) {
            long v = s > 0 ? lround((double)row[j] / s) : 0;
            if (v > 127) v = 127;
            if (v < -127) v = -127;
            qr[j] = (int8_t)v;
        }
    }
}

static void K(matvec_q8_range)(const REAL *x, const int8_t *q, const REAL *sc, int64_t nin, int64_t nout,
                               REAL *out, int64_t j0, int64_t j1) {
    for (int64_t j = j0; j < j1; j++) out[j] = 0;
    for (int64_t i = 0; i < nin; i++) {
        REAL xs = x[i] * sc[i];
        if (xs == 0) continue;
        const int8_t *row = q + i * nout;
        for (int64_t j = j0; j < j1; j++) out[j] += xs * (REAL)row[j];
    }
}

typedef struct { const REAL *x; const int8_t *q; const REAL *sc; REAL *out; int64_t nin, nout; } K(q8_job);

static void K(matvec_q8_part)(void *arg, int part, int nparts) {
    K(q8_job) *j = arg;
    int64_t blocks = (j->nout + MATVEC_BLOCK - 1) / MATVEC_BLOCK;
    int64_t b0 = blocks * part / nparts, b1 = blocks * (part + 1) / nparts;
    int64_t j0 = b0 * MATVEC_BLOCK, j1 = b1 * MATVEC_BLOCK < j->nout ? b1 * MATVEC_BLOCK : j->nout;
    if (j1 > j0) K(matvec_q8_range)(j->x, j->q, j->sc, j->nin, j->nout, j->out, j0, j1);
}

/* out = x @ (q·s); split across the pool like matvec */
static void K(matvec_q8)(const REAL *x, const int8_t *q, const REAL *sc, int64_t nin, int64_t nout, REAL *out) {
    int nt = pool_threads();
    if (nt <= 1 || nin * nout < GEMM_PAR_MIN) {
        K(matvec_q8_range)(x, q, sc, nin, nout, out, 0, nout);
        return;
    }
    int64_t blocks = (nout + MATVEC_BLOCK - 1) / MATVEC_BLOCK;
    if (blocks < nt) nt = (int)blocks;
    K(q8_job) j = {x, q, sc, out, nin, nout};
    pool_run(K(matvec_q8_part), &j, nt);
}

/* ---- matrix products ----
 *
 * gemm: C[M×N] (+)= A[M×K] · B[K×N], row-major. Blocked for the cache: K in
 * blocks of GEMM_KB, then panels of NR = 2·LANES columns of B (GEMM_KB×NR
 * elements, at most 32 KB, stay in L1), then GEMM_MR rows of A at a time; the
 * GEMM_MR×NR tile of C lives in vector registers (see VEC_BYTES in tensor.c).
 *
 * The backward products read their operands transposed in place, nothing is
 * allocated: with GEMM_TA, element (m, k) of A is a[k·lda + m] (dW = Xᵀ·G: a
 * tile reads GEMM_MR consecutive elements of a row of X); with GEMM_TB, element
 * (k, n) of B is b[n·ldb + k] (dX = G·Wᵀ), and each GEMM_KB×NR panel of B is
 * first copied into a buffer of the calling thread, laid out as a plain panel. */

#define K_NR (2 * LANES)

static __thread REAL K(panel)[GEMM_KB * K_NR] __attribute__((aligned(64)));

static inline __attribute__((always_inline)) void K(tile_g)(const REAL *a, int64_t lda, const int ta,
                                                            const REAL *b, int64_t ldb, REAL *c, int64_t ldc,
                                                            int64_t kb, int64_t mr, int64_t nr) {
    if (mr == GEMM_MR && nr == K_NR) {
        K(vec) c0[GEMM_MR], c1[GEMM_MR];
        for (int m = 0; m < GEMM_MR; m++) { c0[m] = K(vload)(c + m * ldc); c1[m] = K(vload)(c + m * ldc + LANES); }
        for (int64_t k = 0; k < kb; k++) {
            K(vec) b0 = K(vload)(b + k * ldb), b1 = K(vload)(b + k * ldb + LANES);
            for (int m = 0; m < GEMM_MR; m++) {
                REAL x = ta ? a[k * lda + m] : a[m * lda + k];
                c0[m] += x * b0;
                c1[m] += x * b1;
            }
        }
        for (int m = 0; m < GEMM_MR; m++) { K(vstore)(c + m * ldc, c0[m]); K(vstore)(c + m * ldc + LANES, c1[m]); }
        return;
    }
    for (int64_t m = 0; m < mr; m++) {
        REAL *cr = c + m * ldc;
        for (int64_t k = 0; k < kb; k++) {
            REAL x = ta ? a[k * lda + m] : a[m * lda + k];
            const REAL *br = b + k * ldb;
            for (int64_t j = 0; j < nr; j++) cr[j] += x * br[j];
        }
    }
}

static void K(tile_n)(const REAL *a, int64_t lda, const REAL *b, int64_t ldb, REAL *c, int64_t ldc,
                      int64_t kb, int64_t mr, int64_t nr) {
    K(tile_g)(a, lda, 0, b, ldb, c, ldc, kb, mr, nr);
}

static void K(tile_t)(const REAL *a, int64_t lda, const REAL *b, int64_t ldb, REAL *c, int64_t ldc,
                      int64_t kb, int64_t mr, int64_t nr) {
    K(tile_g)(a, lda, 1, b, ldb, c, ldc, kb, mr, nr);
}

/* C (+)= A·B with C rows of ldc; A and B as the mode says (see above) */
static void K(gemm_ld)(const REAL *a, int64_t lda, const REAL *b, int64_t ldb, REAL *c, int64_t ldc,
                       int64_t M, int64_t Kd, int64_t N, int accumulate, int mode) {
    if (!accumulate)
        for (int64_t m = 0; m < M; m++) memset(c + m * ldc, 0, (size_t)N * sizeof(REAL));
    REAL *panel = K(panel);
    for (int64_t k0 = 0; k0 < Kd; k0 += GEMM_KB) {
        int64_t kb = Kd - k0 < GEMM_KB ? Kd - k0 : GEMM_KB;
        for (int64_t j0 = 0; j0 < N; j0 += K_NR) {
            int64_t nr = N - j0 < K_NR ? N - j0 : K_NR;
            const REAL *bp;
            int64_t bld;
            if (mode == GEMM_TB) {
                for (int64_t jj = 0; jj < nr; jj++) {
                    const REAL *src = b + (j0 + jj) * ldb + k0;
                    for (int64_t kk = 0; kk < kb; kk++) panel[kk * K_NR + jj] = src[kk];
                }
                bp = panel;
                bld = K_NR;
            } else {
                bp = b + k0 * ldb + j0;
                bld = ldb;
            }
            for (int64_t m0 = 0; m0 < M; m0 += GEMM_MR) {
                int64_t mr = M - m0 < GEMM_MR ? M - m0 : GEMM_MR;
                if (mode == GEMM_TA)
                    K(tile_t)(a + k0 * lda + m0, lda, bp, bld, c + m0 * ldc + j0, ldc, kb, mr, nr);
                else
                    K(tile_n)(a + m0 * lda + k0, lda, bp, bld, c + m0 * ldc + j0, ldc, kb, mr, nr);
            }
        }
    }
}

/* one thread's share of a gemm: a band of columns (whole tile panels) or of rows of C */
typedef struct {
    const REAL *a, *b;
    REAL *c;
    int64_t lda, ldb, M, Kd, N;
    int accumulate, by_cols, mode;
} K(gemm_job);

static void K(gemm_part)(void *arg, int part, int nparts) {
    K(gemm_job) *j = arg;
    if (j->by_cols) {
        int64_t panels = (j->N + K_NR - 1) / K_NR;
        int64_t p0 = panels * part / nparts, p1 = panels * (part + 1) / nparts;
        int64_t c0 = p0 * K_NR, c1 = p1 * K_NR < j->N ? p1 * K_NR : j->N;
        const REAL *b = j->mode == GEMM_TB ? j->b + c0 * j->ldb : j->b + c0;
        if (c1 > c0) K(gemm_ld)(j->a, j->lda, b, j->ldb, j->c + c0, j->N, j->M, j->Kd, c1 - c0, j->accumulate, j->mode);
    } else {
        int64_t blocks = (j->M + GEMM_MR - 1) / GEMM_MR;
        int64_t b0 = blocks * part / nparts, b1 = blocks * (part + 1) / nparts;
        int64_t r0 = b0 * GEMM_MR, r1 = b1 * GEMM_MR < j->M ? b1 * GEMM_MR : j->M;
        const REAL *a = j->mode == GEMM_TA ? j->a + r0 : j->a + r0 * j->lda;
        if (r1 > r0) K(gemm_ld)(a, j->lda, j->b, j->ldb, j->c + r0 * j->N, j->N, r1 - r0, j->Kd, j->N, j->accumulate, j->mode);
    }
}

/* C[M×N] (+)= A·B (A and B as the mode says), split across the thread pool when large enough */
static void K(gemm_x)(const REAL *a, int64_t lda, const REAL *b, int64_t ldb, REAL *c,
                      int64_t M, int64_t Kd, int64_t N, int accumulate, int mode) {
    int nt = pool_threads();
    if (nt <= 1 || M * N * Kd < GEMM_PAR_MIN) {
        K(gemm_ld)(a, lda, b, ldb, c, N, M, Kd, N, accumulate, mode);
        return;
    }
    K(gemm_job) j = {a, b, c, lda, ldb, M, Kd, N, accumulate, 0, mode};
    int64_t panels = (N + K_NR - 1) / K_NR, blocks = (M + GEMM_MR - 1) / GEMM_MR;
    j.by_cols = panels >= blocks;   /* split the longer side: more, evener parts */
    int64_t parts = j.by_cols ? panels : blocks;
    if (parts < nt) nt = (int)parts;
    pool_run(K(gemm_part), &j, nt);
}

/* C[M×N] (+)= A[M×K]·B[K×N] */
static void K(gemm)(const REAL *a, const REAL *b, REAL *c, int64_t M, int64_t Kd, int64_t N, int accumulate) {
    K(gemm_x)(a, Kd, b, N, c, M, Kd, N, accumulate, GEMM_PLAIN);
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

/* SwiGLU: s = silu(a)·b, silu(a) = a·σ(a) */
static void K(swiglu_seq)(const REAL *a, const REAL *b, REAL *s, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        double x = a[i];
        double sg = 1.0 / (1.0 + exp(-x));
        s[i] = (REAL)(x * sg * b[i]);
    }
}

/* its backward, accumulating: da += ds·b·silu'(a), db += ds·silu(a) */
static void K(swiglu_back_seq)(const REAL *ds, const REAL *a, const REAL *b, REAL *da, REAL *db, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        double x = a[i];
        double sg = 1.0 / (1.0 + exp(-x));
        double silu = x * sg;
        double dsilu = sg * (1.0 + x * (1.0 - sg));
        da[i] += (REAL)(ds[i] * b[i] * dsilu);
        db[i] += (REAL)(ds[i] * silu);
    }
}

/* Both split their elements across the pool when there are many (element by element: the
 * result does not depend on the number of threads). */
typedef struct { const REAL *ds, *a, *b; REAL *s, *da, *db; int64_t n; } K(glu_job);

static void K(swiglu_part)(void *arg, int part, int nparts) {
    K(glu_job) *j = arg;
    int64_t i0 = j->n * part / nparts, i1 = j->n * (part + 1) / nparts;
    if (i1 > i0) K(swiglu_seq)(j->a + i0, j->b + i0, j->s + i0, i1 - i0);
}

static void K(swiglu_back_part)(void *arg, int part, int nparts) {
    K(glu_job) *j = arg;
    int64_t i0 = j->n * part / nparts, i1 = j->n * (part + 1) / nparts;
    if (i1 > i0) K(swiglu_back_seq)(j->ds + i0, j->a + i0, j->b + i0, j->da + i0, j->db + i0, i1 - i0);
}

static void K(swiglu)(const REAL *a, const REAL *b, REAL *s, int64_t n) {
    int nt = pool_threads();
    if (nt <= 1 || n < ELEM_PAR_MIN) {
        K(swiglu_seq)(a, b, s, n);
        return;
    }
    K(glu_job) j = {NULL, a, b, s, NULL, NULL, n};
    pool_run(K(swiglu_part), &j, nt);
}

static void K(swiglu_back)(const REAL *ds, const REAL *a, const REAL *b, REAL *da, REAL *db, int64_t n) {
    int nt = pool_threads();
    if (nt <= 1 || n < ELEM_PAR_MIN) {
        K(swiglu_back_seq)(ds, a, b, da, db, n);
        return;
    }
    K(glu_job) j = {ds, a, b, NULL, da, db, n};
    pool_run(K(swiglu_back_part), &j, nt);
}

/* Rotary position embedding, in place, on T rows of C (H heads): in each head the
 * pairs (i, i + HS/2) turn by the angle pos·base^(−2i/HS), pos = pos0 + t;
 * inverse turns them back (the backward pass). */
static void K(rope)(REAL *x, int64_t T, int64_t C, int64_t H, int64_t pos0, double base, int inverse,
                    double *freq) {
    int64_t HS = C / H, half = HS / 2;
    for (int64_t i = 0; i < half; i++) freq[i] = pow(base, -2.0 * (double)i / (double)HS);
    for (int64_t t = 0; t < T; t++) {
        double pos = (double)(pos0 + t);
        for (int64_t i = 0; i < half; i++) {
            double ang = pos * freq[i];
            double c = cos(ang), s = sin(ang);
            if (inverse) s = -s;
            for (int64_t h = 0; h < H; h++) {
                REAL *p = x + t * C + h * HS;
                double x1 = p[i], x2 = p[i + half];
                p[i] = (REAL)(x1 * c - x2 * s);
                p[i + half] = (REAL)(x1 * s + x2 * c);
            }
        }
    }
}

/* Cross-entropy of T rows of V logits against targets (a target < 0 is skipped):
 * returns Σ −log softmax(row)[target], in double, and replaces each row by its gradient
 * scale·(softmax − onehot) (a skipped row becomes zeros). */
static void K(cross_entropy_rows)(REAL *logits, const int64_t *targets, int64_t V, double scale,
                                   double *losses, int64_t t0, int64_t t1) {
    for (int64_t t = t0; t < t1; t++) {
        REAL *row = logits + t * V;
        int64_t y = targets[t];
        if (y < 0) {
            for (int64_t j = 0; j < V; j++) row[j] = 0;
            losses[t] = 0.0;
            continue;
        }
        double mx = -1e300;
        for (int64_t j = 0; j < V; j++) if (row[j] > mx) mx = row[j];
        double target = (double)row[y];
        double sum = 0.0;
        for (int64_t j = 0; j < V; j++) {
            double e = exp((double)row[j] - mx);
            row[j] = (REAL)e;
            sum += e;
        }
        losses[t] = log(sum) + mx - target;
        double k = scale / sum;
        for (int64_t j = 0; j < V; j++) row[j] = (REAL)(row[j] * k);
        row[y] -= (REAL)scale;
    }
}

typedef struct { REAL *logits; const int64_t *targets; int64_t T, V; double scale; double *losses; } K(ce_job);

static void K(cross_entropy_part)(void *arg, int part, int nparts) {
    K(ce_job) *j = arg;
    int64_t t0 = j->T * part / nparts, t1 = j->T * (part + 1) / nparts;
    if (t1 > t0) K(cross_entropy_rows)(j->logits, j->targets, j->V, j->scale, j->losses, t0, t1);
}

/* losses: scratch of T doubles */
static double K(cross_entropy)(REAL *logits, const int64_t *targets, int64_t T, int64_t V, double scale,
                               double *losses) {
    int nt = pool_threads();
    if (nt > T) nt = (int)T;
    if (nt <= 1 || T * V < GEMM_PAR_MIN) {
        K(cross_entropy_rows)(logits, targets, V, scale, losses, 0, T);
    } else {
        K(ce_job) j = {logits, targets, T, V, scale, losses};
        pool_run(K(cross_entropy_part), &j, nt);
    }
    double loss = 0.0;
    for (int64_t t = 0; t < T; t++) loss += losses[t];
    return loss;
}

/* ---- attention ---- */

/* the heads [h0, h1) of attention */
static void K(attention_heads)(const REAL *q, const REAL *k, const REAL *v, REAL *att, REAL *y,
                               int64_t T, int64_t C, int64_t H, int64_t B, int64_t h0, int64_t h1) {
    int64_t HS = C / H;
    double scale = 1.0 / sqrt((double)HS);
    for (int64_t h = h0; h < h1; h++) {
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
/* the heads [h0, h1) of attention_back */
static void K(attention_back_heads)(const REAL *q, const REAL *k, const REAL *v, const REAL *att,
                                    const REAL *dy, REAL *dq, REAL *dk, REAL *dvv,
                                    int64_t T, int64_t C, int64_t H, int64_t B, double *dp, int64_t h0, int64_t h1) {
    int64_t HS = C / H;
    double scale = 1.0 / sqrt((double)HS);
    for (int64_t h = h0; h < h1; h++) {
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

/* Attention splits its heads across the thread pool: each head reads and writes only its
 * own columns, so the result does not depend on the number of threads. */
typedef struct {
    const REAL *q, *k, *v, *att, *dy;
    REAL *attw, *y, *dq, *dk, *dvv;
    int64_t T, C, H, B;
} K(att_job);

static int K(att_threads)(int64_t T, int64_t C, int64_t H) {
    int nt = pool_threads();
    if (nt > H) nt = (int)H;
    if (T * T * C < ATT_PAR_MIN) nt = 1;
    return nt;
}

static void K(attention_part)(void *arg, int part, int nparts) {
    K(att_job) *j = arg;
    int64_t h0 = j->H * part / nparts, h1 = j->H * (part + 1) / nparts;
    if (h1 > h0) K(attention_heads)(j->q, j->k, j->v, j->attw, j->y, j->T, j->C, j->H, j->B, h0, h1);
}

static void K(attention)(const REAL *q, const REAL *k, const REAL *v, REAL *att, REAL *y,
                         int64_t T, int64_t C, int64_t H, int64_t B) {
    int nt = K(att_threads)(T, C, H);
    if (nt <= 1) {
        K(attention_heads)(q, k, v, att, y, T, C, H, B, 0, H);
        return;
    }
    K(att_job) j = {q, k, v, NULL, NULL, att, y, NULL, NULL, NULL, T, C, H, B};
    pool_run(K(attention_part), &j, nt);
}

static void K(attention_back_part)(void *arg, int part, int nparts) {
    K(att_job) *j = arg;
    int64_t h0 = j->H * part / nparts, h1 = j->H * (part + 1) / nparts;
    if (h1 <= h0) return;
    double *dp = malloc(sizeof(double) * (size_t)(j->T > 0 ? j->T : 1));
    if (!dp) abort();
    K(attention_back_heads)(j->q, j->k, j->v, j->att, j->dy, j->dq, j->dk, j->dvv, j->T, j->C, j->H, j->B, dp, h0, h1);
    free(dp);
}

/* dp: scratch of T doubles (for the single-thread path) */
static void K(attention_back)(const REAL *q, const REAL *k, const REAL *v, const REAL *att,
                              const REAL *dy, REAL *dq, REAL *dk, REAL *dvv,
                              int64_t T, int64_t C, int64_t H, int64_t B, double *dp) {
    int nt = K(att_threads)(T, C, H);
    if (nt <= 1) {
        K(attention_back_heads)(q, k, v, att, dy, dq, dk, dvv, T, C, H, B, dp, 0, H);
        return;
    }
    K(att_job) j = {q, k, v, att, dy, NULL, NULL, dq, dk, dvv, T, C, H, B};
    pool_run(K(attention_back_part), &j, nt);
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
