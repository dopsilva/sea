/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * libs/embed/embed_native.c — text embeddings with llama.cpp (used by embed.sea).
 *
 * Build:  make -C libs/embed   (fetches and builds llama.cpp into vendor/ the first time)
 *
 * A model is a GGUF file loaded with a context made for embeddings (pooled
 * per text). Handles are ints that index a registry (slot + generation), so
 * a closed or made-up handle is an error, never a crash; the model is freed
 * when the last job using it finishes.
 *
 * Jobs (loading a model, embedding texts) take seconds on a CPU. In a
 * goroutine they run on a worker thread and only that goroutine parks
 * (sea_netpoll.h: an eventfd wakes it); on the main thread they run in
 * place. A model computes one job at a time (llama.cpp's context is not
 * thread-safe; the computation itself uses `threads` threads).
 */

#include "sea.h"
#include "sea_rope.h"
#include "sea_netpoll.h"
#include "llama.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sea_compat.h"
#include <unistd.h>

#ifndef EMBED_LLAMA_VERSION
#define EMBED_LLAMA_VERSION "unknown"
#endif

static SeaValue err_val(SeaVM *vm, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return sea_make_error_str(vm, msg);
}

static SeaValue str_val(const char *s) { return sea_obj_val(&sea_string_new(s, strlen(s))->obj); }

static char *cstr_dup(SeaValue v) {
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return NULL;
    char *d = malloc(s->len + 1);
    if (d) {
        memcpy(d, s->data, s->len);
        d[s->len] = '\0';
    }
    sea_obj_release(&s->obj);
    return d;
}

static char *dupstr(const char *s) {
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

/* ======================================================================== */
/* llama.cpp setup                                                          */
/* ======================================================================== */

/* llama.cpp logs every load in detail, and its errors come back as error
 * values: quiet unless SEA_EMBED_LOG is set */
static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)level; (void)ud;
    if (getenv("SEA_EMBED_LOG")) fputs(text, stderr);
}

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static void backend_init(void) {
    llama_log_set(quiet_log, NULL);
    llama_backend_init();
}

/* ======================================================================== */
/* Models and their registry                                               */
/* ======================================================================== */

typedef struct Model {
    atomic_int            refs;       /* the registry's + one per job */
    pthread_mutex_t       mu;         /* one job at a time */
    struct llama_model   *model;
    struct llama_context *ctx;
    int                   n_embd, n_ctx, n_batch, n_seq;
    bool                  encoder;    /* encoder-only (BERT): llama_encode */
    enum llama_pooling_type pooling;
} Model;

static void model_unref(Model *m) {
    if (!m || atomic_fetch_sub(&m->refs, 1) != 1) return;
    if (m->ctx) llama_free(m->ctx);
    if (m->model) llama_model_free(m->model);
    pthread_mutex_destroy(&m->mu);
    free(m);
}

#define MAX_MODELS 64
static pthread_mutex_t g_reg = PTHREAD_MUTEX_INITIALIZER;
static Model   *g_models[MAX_MODELS];
static uint32_t g_gen[MAX_MODELS];

static int64_t reg_add(Model *m) {
    pthread_mutex_lock(&g_reg);
    for (int i = 0; i < MAX_MODELS; i++) {
        if (!g_models[i]) {
            g_models[i] = m;
            g_gen[i]++;
            int64_t h = ((int64_t)g_gen[i] << 8) | i;
            pthread_mutex_unlock(&g_reg);
            return h;
        }
    }
    pthread_mutex_unlock(&g_reg);
    return -1;
}

/* the model of handle h with a reference for the caller, or NULL */
static Model *reg_get(SeaValue h) {
    if (!sea_is_int(h)) return NULL;
    int64_t v = sea_as_int(h);
    int i = (int)(v & 0xff);
    if (v < 0 || i >= MAX_MODELS) return NULL;
    pthread_mutex_lock(&g_reg);
    Model *m = g_models[i];
    if (m && (uint32_t)(v >> 8) == g_gen[i]) atomic_fetch_add(&m->refs, 1);
    else m = NULL;
    pthread_mutex_unlock(&g_reg);
    return m;
}

static Model *reg_remove(SeaValue h) {
    if (!sea_is_int(h)) return NULL;
    int64_t v = sea_as_int(h);
    int i = (int)(v & 0xff);
    if (v < 0 || i >= MAX_MODELS) return NULL;
    pthread_mutex_lock(&g_reg);
    Model *m = g_models[i];
    if (m && (uint32_t)(v >> 8) == g_gen[i]) g_models[i] = NULL;
    else m = NULL;
    pthread_mutex_unlock(&g_reg);
    return m;
}

/* ======================================================================== */
/* Jobs: run on a worker thread (goroutines) or in place                    */
/* ======================================================================== */

typedef enum { J_LOAD, J_EMBED } JobKind;

typedef struct Job {
    struct Job *next;
    atomic_int  refs, done;
    int         efd, wfd;
    JobKind     kind;
    char       *err;
    /* load */
    char       *path;
    int         threads, n_ctx, pooling, n_seq;
    Model      *loaded;
    /* embed */
    Model      *model;
    char      **texts;
    int         ntexts;
    bool        normalize;
    float      *out;                  /* ntexts × n_embd */
    int         truncated;            /* texts cut to the context size */
} Job;

static void job_fail(Job *j, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    free(j->err);
    j->err = dupstr(msg);
}

static void job_free(Job *j) {
    free(j->err);
    free(j->path);
    for (int i = 0; i < j->ntexts; i++) free(j->texts[i]);
    free(j->texts);
    free(j->out);
    model_unref(j->loaded);
    model_unref(j->model);
    if (j->efd >= 0) {
        sea_netpoll_close(j->efd);
        sea_wake_close_wr(j->efd, j->wfd);
    }
    free(j);
}

static void job_unref(Job *j) {
    if (atomic_fetch_sub(&j->refs, 1) == 1) job_free(j);
}

static void run_load(Job *j) {
    pthread_once(&g_init, backend_init);
    struct llama_model_params mp = llama_model_default_params();
    struct llama_model *model = llama_model_load_from_file(j->path, mp);
    if (!model) { job_fail(j, "cannot load the model %s", j->path); return; }
    if (llama_model_has_encoder(model) && llama_model_has_decoder(model)) {
        llama_model_free(model);
        job_fail(j, "encoder-decoder models do not give embeddings");
        return;
    }
    int train = llama_model_n_ctx_train(model);
    int n_ctx = j->n_ctx > 0 ? j->n_ctx : (train > 0 && train < 2048 ? train : 2048);
    struct llama_context_params cp = llama_context_default_params();
    cp.embeddings = true;
    cp.n_ctx = (uint32_t)n_ctx;
    cp.n_batch = (uint32_t)n_ctx;          /* a whole batch of texts in one pass */
    cp.n_ubatch = (uint32_t)n_ctx;         /* non-causal models need ubatch = batch */
    cp.n_seq_max = (uint32_t)(j->n_seq > 0 ? j->n_seq : 8);
    cp.kv_unified = true;
    /* all the cores lose to the other processes of the machine: llama.cpp's
     * threads wait for each other, and one busy core stalls them all (a short
     * text took ten times as long with 4 threads as with 3 on 4 cores) */
    int threads = j->threads;
    if (threads <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        threads = n > 1 ? (int)n - 1 : 1;
    }
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    if (j->pooling >= 0) cp.pooling_type = (enum llama_pooling_type)j->pooling;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        llama_model_free(model);
        job_fail(j, "cannot create a context for the model");
        return;
    }
    if (llama_pooling_type(ctx) == LLAMA_POOLING_TYPE_NONE) {
        llama_free(ctx);
        llama_model_free(model);
        job_fail(j, "the model does not pool its output: pass pooling = \"mean\", \"cls\" or \"last\"");
        return;
    }
    Model *m = calloc(1, sizeof *m);
    if (!m) {
        llama_free(ctx);
        llama_model_free(model);
        job_fail(j, "out of memory");
        return;
    }
    atomic_init(&m->refs, 1);
    pthread_mutex_init(&m->mu, NULL);
    m->model = model;
    m->ctx = ctx;
    m->n_embd = llama_model_n_embd_out(model);
    m->n_ctx = (int)llama_n_ctx(ctx);
    m->n_batch = (int)llama_n_batch(ctx);
    m->n_seq = (int)llama_n_seq_max(ctx);
    m->encoder = llama_model_has_encoder(model);
    m->pooling = llama_pooling_type(ctx);
    j->loaded = m;
}

/* one forward pass over the texts in the batch (sequence s = output row first + s) */
static bool decode_batch(Job *j, struct llama_batch *b, int first, int nseq) {
    Model *m = j->model;
    llama_memory_clear(llama_get_memory(m->ctx), true);
    int rc = m->encoder ? llama_encode(m->ctx, *b) : llama_decode(m->ctx, *b);
    if (rc != 0) { job_fail(j, "the model failed on a batch (%d)", rc); return false; }
    for (int s = 0; s < nseq; s++) {
        const float *e = llama_get_embeddings_seq(m->ctx, s);
        if (!e) { job_fail(j, "the model gave no embedding"); return false; }
        float *o = j->out + (size_t)(first + s) * m->n_embd;
        double norm = 0;
        for (int k = 0; k < m->n_embd; k++) norm += (double)e[k] * e[k];
        float scale = j->normalize && norm > 0 ? (float)(1.0 / sqrt(norm)) : 1.0f;
        for (int k = 0; k < m->n_embd; k++) o[k] = e[k] * scale;
    }
    return true;
}

static void run_embed(Job *j) {
    Model *m = j->model;
    pthread_mutex_lock(&m->mu);
    const struct llama_vocab *vocab = llama_model_get_vocab(m->model);
    int max_tok = m->n_batch;
    llama_token *tok = malloc(sizeof(llama_token) * (size_t)max_tok);
    struct llama_batch b = llama_batch_init(m->n_batch, 0, 1);
    j->out = calloc((size_t)j->ntexts * (size_t)m->n_embd, sizeof(float));
    if (!tok || !j->out) { job_fail(j, "out of memory"); goto done; }
    int first = 0, nseq = 0;
    b.n_tokens = 0;
    for (int i = 0; i < j->ntexts; i++) {
        int len = (int)strlen(j->texts[i]);
        int n = llama_tokenize(vocab, j->texts[i], len, tok, max_tok, true, true);
        if (n < 0) {            /* longer than the context: keep its first tokens */
            int need = -n;
            llama_token *all = malloc(sizeof(llama_token) * (size_t)need);
            if (!all) { job_fail(j, "out of memory"); goto done; }
            llama_tokenize(vocab, j->texts[i], len, all, need, true, true);
            memcpy(tok, all, sizeof(llama_token) * (size_t)max_tok);
            free(all);
            n = max_tok;
            j->truncated++;
        }
        if (n == 0) { job_fail(j, "text %d has no tokens", i); goto done; }
        if (b.n_tokens + n > m->n_batch || nseq >= m->n_seq) {
            if (!decode_batch(j, &b, first, nseq)) goto done;
            first += nseq;
            nseq = 0;
            b.n_tokens = 0;
        }
        for (int k = 0; k < n; k++) {
            int t = b.n_tokens++;
            b.token[t] = tok[k];
            b.pos[t] = k;
            b.n_seq_id[t] = 1;
            b.seq_id[t][0] = nseq;
            b.logits[t] = true;
        }
        nseq++;
    }
    if (nseq > 0) decode_batch(j, &b, first, nseq);
done:
    llama_batch_free(b);
    free(tok);
    pthread_mutex_unlock(&m->mu);
}

static void job_run(Job *j) {
    if (j->kind == J_LOAD) run_load(j);
    else run_embed(j);
}

#define POOL_MAX 4

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

/* Hand j to a worker (it takes a reference). False: run it in place. */
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

static Job *job_new(JobKind kind) {
    Job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    atomic_init(&j->refs, 1);
    atomic_init(&j->done, 0);
    j->efd = j->wfd = -1;
    j->kind = kind;
    return j;
}

/* Start j (first attempt) or wait for it (after a park). Returns NULL when
 * the job is done (the caller converts and unrefs it), or the value to
 * return now (SEA_PARK_VAL, an error). */
static SeaValue *job_wait(SeaVM *vm, SeaIoCall *io, Job *j, bool started, SeaValue *out, const char *fn) {
    if (!started) {
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
        if (w == SEA_IO_PARK) { *out = SEA_PARK_VAL; return out; }
        if (w != SEA_IO_READY) {
            io->aux[0] = 0;
            sea_io_end(vm);
            job_unref(j);                     /* the worker still holds one */
            *out = err_val(vm, "embed.%s: interrupted while waiting", fn);
            return out;
        }
    }
    io->aux[0] = 0;
    sea_io_end(vm);
    return NULL;
}

/* ======================================================================== */
/* Native functions                                                         */
/* ======================================================================== */

static int int_arg(int argc, SeaValue *args, int i, int fallback) {
    if (argc <= i) return fallback;
    if (sea_is_int(args[i])) return (int)sea_as_int(args[i]);
    if (sea_is_number(args[i])) return (int)sea_as_number(args[i]);
    return fallback;
}

/* native.version() → the llama.cpp release built into vendor/ */
static SeaValue fn_version(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    return str_val(EMBED_LLAMA_VERSION);
}

/* native.load(path, threads, n_ctx, pooling, n_seq) → handle | error
 * (0 or a negative number: the default; pooling -1 = the model's) */
static SeaValue fn_load(SeaVM *vm, int argc, SeaValue *args) {
    SeaIoCall *io = sea_io_begin(vm, -1);
    Job *j = (Job *)(intptr_t)io->aux[0];
    bool started = j != NULL;
    if (!j) {
        char *path = argc > 0 ? cstr_dup(args[0]) : NULL;
        if (!path) { sea_io_end(vm); return err_val(vm, "embed.load: the model's path expected"); }
        if (access(path, R_OK) != 0) {
            SeaValue r = err_val(vm, "embed.load: cannot read %s", path);
            free(path);
            sea_io_end(vm);
            return r;
        }
        j = job_new(J_LOAD);
        if (!j) { free(path); sea_io_end(vm); return err_val(vm, "embed.load: out of memory"); }
        j->path = path;
        j->threads = int_arg(argc, args, 1, 0);
        j->n_ctx = int_arg(argc, args, 2, 0);
        j->pooling = int_arg(argc, args, 3, -1);
        j->n_seq = int_arg(argc, args, 4, 0);
    }
    SeaValue now;
    if (job_wait(vm, io, j, started, &now, "load")) return now;
    SeaValue r;
    if (j->err) {
        r = err_val(vm, "embed.load: %s", j->err);
    } else {
        int64_t h = reg_add(j->loaded);
        if (h < 0) {
            r = err_val(vm, "embed.load: too many models open (%d)", MAX_MODELS);
        } else {
            j->loaded = NULL;                 /* the registry holds it now */
            r = sea_int_val(h);
        }
    }
    job_unref(j);
    return r;
}

/* native.info(h) → {dimension, context, pooling, description} | error */
static SeaValue fn_info(SeaVM *vm, int argc, SeaValue *args) {
    Model *m = argc > 0 ? reg_get(args[0]) : NULL;
    if (!m) return err_val(vm, "embed.info: the model is closed");
    static const char *POOL[] = {"none", "mean", "cls", "last", "rank"};
    char desc[256] = {0};
    llama_model_desc(m->model, desc, sizeof desc);
    SeaTable *t = sea_table_new(TABLE_MAP);
    sea_table_set_str(t, "dimension", sea_int_val(m->n_embd));
    sea_table_set_str(t, "context", sea_int_val(m->n_ctx));
    SeaValue p = str_val(m->pooling >= 0 && m->pooling <= 4 ? POOL[m->pooling] : "unknown");
    sea_table_set_str(t, "pooling", p);
    sea_obj_release(sea_as_obj(p));
    SeaValue d = str_val(desc);
    sea_table_set_str(t, "description", d);
    sea_obj_release(sea_as_obj(d));
    model_unref(m);
    return sea_obj_val(&t->obj);
}

/* native.embed(h, texts, normalize) → {vectors = [[number]], truncated} | error */
static SeaValue fn_embed(SeaVM *vm, int argc, SeaValue *args) {
    SeaIoCall *io = sea_io_begin(vm, -1);
    Job *j = (Job *)(intptr_t)io->aux[0];
    bool started = j != NULL;
    if (!j) {
        if (argc < 2 || !sea_is_array(args[1])) { sea_io_end(vm); return err_val(vm, "embed.embed: an array of texts expected"); }
        Model *m = reg_get(args[0]);
        if (!m) { sea_io_end(vm); return err_val(vm, "embed.embed: the model is closed"); }
        SeaArray *a = SEA_AS_ARRAY(args[1]);
        j = job_new(J_EMBED);
        if (!j) { model_unref(m); sea_io_end(vm); return err_val(vm, "embed.embed: out of memory"); }
        j->model = m;
        j->normalize = argc < 3 || sea_is_truthy(args[2]);
        j->texts = calloc(a->len ? a->len : 1, sizeof(char *));
        if (!j->texts) { job_unref(j); sea_io_end(vm); return err_val(vm, "embed.embed: out of memory"); }
        for (uint32_t i = 0; i < a->len; i++) {
            char *s = cstr_dup(a->data[i]);
            if (!s) {
                job_unref(j);
                sea_io_end(vm);
                return err_val(vm, "embed.embed: text %u is not a string", i);
            }
            j->texts[j->ntexts++] = s;
        }
        if (j->ntexts == 0) {
            job_unref(j);
            sea_io_end(vm);
            SeaTable *t = sea_table_new(TABLE_MAP);
            SeaArray *empty = sea_array_new(0);
            sea_table_set_str(t, "vectors", sea_obj_val(&empty->obj));
            sea_obj_release(&empty->obj);
            sea_table_set_str(t, "truncated", sea_int_val(0));
            return sea_obj_val(&t->obj);
        }
    }
    SeaValue now;
    if (job_wait(vm, io, j, started, &now, "embed")) return now;
    SeaValue r;
    if (j->err) {
        r = err_val(vm, "embed.embed: %s", j->err);
    } else {
        int d = j->model->n_embd;
        SeaArray *vs = sea_array_new((uint32_t)j->ntexts);
        for (int i = 0; i < j->ntexts; i++) {
            SeaArray *v = sea_array_new((uint32_t)d);
            const float *src = j->out + (size_t)i * d;
            for (int k = 0; k < d; k++) sea_array_push(v, sea_number_val((double)src[k]));
            sea_array_push(vs, sea_obj_val(&v->obj));
            sea_obj_release(&v->obj);
        }
        SeaTable *t = sea_table_new(TABLE_MAP);
        sea_table_set_str(t, "vectors", sea_obj_val(&vs->obj));
        sea_obj_release(&vs->obj);
        sea_table_set_str(t, "truncated", sea_int_val(j->truncated));
        r = sea_obj_val(&t->obj);
    }
    job_unref(j);
    return r;
}

/* native.close(h) → true (a closed handle: nil) */
static SeaValue fn_close(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    Model *m = argc > 0 ? reg_remove(args[0]) : NULL;
    if (!m) return SEA_NIL_VAL;
    model_unref(m);                           /* freed after the jobs still using it */
    return SEA_TRUE_VAL;
}

/* ---- Module entry point --------------------------------------------------- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "version", fn_version);
    sea_mod_set(mod, "load",    fn_load);
    sea_mod_set(mod, "info",    fn_info);
    sea_mod_set(mod, "embed",   fn_embed);
    sea_mod_set(mod, "close",   fn_close);
    return sea_obj_val(&mod->obj);
}
