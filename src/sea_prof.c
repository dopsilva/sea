/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* sea_prof.c — the sampling profiler behind `sea prof` (see sea_prof.h) */
#include "sea_prof.h"
#include "sea_vm.h"
#include "sea_sched.h"
#include "sea_chunk.h"
#include "sea_gc.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#if defined(__linux__)
#  include <sys/syscall.h>
#endif
#include <unistd.h>

bool sea_prof_enabled = false;

#define MAX_DEPTH  64
#define BUF_WORDS  (2u * 1024 * 1024)      /* 16 MB of samples, touched as used */
#define MAX_TIMERS 1024

#ifndef MAP_NORESERVE
#  define MAP_NORESERVE 0
#endif

static SeaProfOptions opts;
static uintptr_t     *buf;                  /* samples: [depth, (chunk, ip) * depth] */
static atomic_uint    buf_used;
static atomic_uint    n_samples, n_dropped, n_outside;
static atomic_bool    sampling;
#if defined(__linux__)
static pthread_mutex_t timers_mu = PTHREAD_MUTEX_INITIALIZER;
static timer_t        timers[MAX_TIMERS];
static int            n_timers;
#endif
static SeaVM         *root_vm;
static SeaChunk      *root_chunk;
static bool           finished;

/* ---- sampling (signal handler: no locks, no allocation) ---- */

static void on_sigprof(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    if (!atomic_load_explicit(&sampling, memory_order_relaxed)) return;
    int saved = errno;
    SeaVM *vm = sea_gc_vm;
    int fc = vm ? vm->frame_count : 0;
    if (fc <= 0) {
        atomic_fetch_add_explicit(&n_outside, 1, memory_order_relaxed);
        errno = saved;
        return;
    }
    if (fc > SEA_MAX_CALL_DEPTH) fc = SEA_MAX_CALL_DEPTH;
    int depth = fc < MAX_DEPTH ? fc : MAX_DEPTH;
    unsigned need = 1 + 2 * (unsigned)depth;
    unsigned at = atomic_fetch_add_explicit(&buf_used, need, memory_order_relaxed);
    if (at + need > BUF_WORDS) {
        atomic_fetch_add_explicit(&n_dropped, 1, memory_order_relaxed);
        errno = saved;
        return;
    }
    uintptr_t *w = buf + at + 1;
    for (int i = 0; i < depth; i++) {          /* innermost first */
        SeaCallFrame *f = &vm->frames[fc - 1 - i];
        SeaClosure *cl = f->closure;
        *w++ = (uintptr_t)(cl ? cl->chunk : NULL);
        *w++ = (uintptr_t)f->ip;
    }
    buf[at] = (uintptr_t)depth;
    atomic_fetch_add_explicit(&n_samples, 1, memory_order_relaxed);
    errno = saved;
}

#if defined(__linux__)
void sea_prof_thread_start(void) {
    if (!sea_prof_enabled) return;
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    timer_t t;
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &t) != 0) return;
    long ns = 1000000000L / (opts.hz > 0 ? opts.hz : 100);
    struct itimerspec its = { { ns / 1000000000L, ns % 1000000000L }, { ns / 1000000000L, ns % 1000000000L } };
    timer_settime(t, 0, &its, NULL);
    pthread_mutex_lock(&timers_mu);
    if (n_timers < MAX_TIMERS) timers[n_timers++] = t;
    else timer_delete(t);
    pthread_mutex_unlock(&timers_mu);
}
#else
/* No per-thread CPU timers here (timer_create is Linux's): one process-wide
 * CPU timer, whose SIGPROF the system hands to a thread that is running. The
 * handler samples the VM of the thread it lands on, so a program that keeps
 * several threads busy is sampled at random among them. */
void sea_prof_thread_start(void) {}
static void prof_process_timer(int hz) {
    long us = 1000000L / hz;
    struct itimerval it = { { us / 1000000L, us % 1000000L }, { us / 1000000L, us % 1000000L } };
    setitimer(ITIMER_PROF, &it, NULL);
}
static void prof_process_timer_stop(void) {
    struct itimerval it = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_PROF, &it, NULL);
}
#endif

static void at_exit(void) { sea_prof_finish(); }

void sea_prof_start(const SeaProfOptions *o) {
    opts = *o;
    if (opts.hz <= 0) opts.hz = 100;
    if (opts.top <= 0) opts.top = 20;
    buf = mmap(NULL, BUF_WORDS * sizeof *buf, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (buf == MAP_FAILED) {
        fprintf(stderr, "sea prof: cannot allocate the sample buffer\n");
        exit(1);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigprof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    sea_prof_enabled = true;
    atomic_store(&sampling, true);
    atexit(at_exit);
#if !defined(__linux__)
    prof_process_timer(opts.hz);
#endif
    sea_prof_thread_start();
}

void sea_prof_set_root(SeaVM *vm, SeaChunk *root) {
    root_vm = vm;
    root_chunk = root;
}

/* ---- resolving and reporting ---- */

/* Open-addressing set of the chunks the run created (sample pointers are
 * only trusted if they are in it). */
typedef struct { SeaChunk **k; size_t cap, n; } ChunkSet;

static size_t hptr(const void *p) { uintptr_t x = (uintptr_t)p; x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return (size_t)x; }

static bool cs_has(ChunkSet *s, SeaChunk *c) {
    if (!s->cap || !c) return false;
    for (size_t i = hptr(c) & (s->cap - 1);; i = (i + 1) & (s->cap - 1)) {
        if (s->k[i] == c) return true;
        if (!s->k[i]) return false;
    }
}

static void cs_add(ChunkSet *s, SeaChunk *c) {
    if (!c || cs_has(s, c)) return;
    if ((s->n + 1) * 2 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 256;
        SeaChunk **nk = calloc(nc, sizeof *nk);
        for (size_t i = 0; i < s->cap; i++) {
            if (!s->k[i]) continue;
            size_t j = hptr(s->k[i]) & (nc - 1);
            while (nk[j]) j = (j + 1) & (nc - 1);
            nk[j] = s->k[i];
        }
        free(s->k);
        s->k = nk;
        s->cap = nc;
    }
    size_t i = hptr(c) & (s->cap - 1);
    while (s->k[i]) i = (i + 1) & (s->cap - 1);
    s->k[i] = c;
    s->n++;
    for (int p = 0; p < c->proto_len; p++) cs_add(s, c->protos[p]);
}

/* Per-function and per-line counters */
typedef struct { SeaChunk *c; int line; unsigned self, total, seen_in; } Row;
typedef struct { Row *r; size_t cap, n; } RowMap;

static Row *rm_get(RowMap *m, SeaChunk *c, int line) {
    if ((m->n + 1) * 2 > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 256;
        Row *nr = calloc(nc, sizeof *nr);
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->r[i].c) continue;
            size_t j = (hptr(m->r[i].c) ^ (size_t)m->r[i].line * 0x9e3779b9u) & (nc - 1);
            while (nr[j].c) j = (j + 1) & (nc - 1);
            nr[j] = m->r[i];
        }
        free(m->r);
        m->r = nr;
        m->cap = nc;
    }
    size_t i = (hptr(c) ^ (size_t)line * 0x9e3779b9u) & (m->cap - 1);
    while (m->r[i].c && !(m->r[i].c == c && m->r[i].line == line)) i = (i + 1) & (m->cap - 1);
    if (!m->r[i].c) {
        m->r[i].c = c;
        m->r[i].line = line;
        m->n++;
    }
    return &m->r[i];
}

static bool is_root(SeaChunk *c) {
    return c->source ? c->source == c->name : (c->name && strchr(c->name, '/'));
}

static const char *fname(SeaChunk *c) {
    if (is_root(c)) return "main chunk";
    return c->name && c->name[0] ? c->name : "(anonymous)";
}

static const char *fsrc(SeaChunk *c) {
    return c->source ? c->source : (c->name ? c->name : "?");
}

static int first_line(SeaChunk *c) {
    if (c->line_defined > 0) return c->line_defined;
    if (is_root(c)) return 1;
    return c->lines_len > 0 ? c->lines[0] : 0;
}

static int line_of(SeaChunk *c, uintptr_t ip) {
    long idx = (long)(((uint32_t *)ip) - c->code) - 1;
    if (idx < 0 || idx >= c->lines_len) return -1;
    return c->lines[idx];
}

/* Folded stacks: string → count */
typedef struct { char *s; unsigned n; } Fold;
typedef struct { Fold *f; size_t cap, n; } FoldMap;

static size_t hstr(const char *s) { size_t h = 1469598103934665603ULL; while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; } return h; }

static void fm_add(FoldMap *m, const char *s) {
    if ((m->n + 1) * 2 > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 256;
        Fold *nf = calloc(nc, sizeof *nf);
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->f[i].s) continue;
            size_t j = hstr(m->f[i].s) & (nc - 1);
            while (nf[j].s) j = (j + 1) & (nc - 1);
            nf[j] = m->f[i];
        }
        free(m->f);
        m->f = nf;
        m->cap = nc;
    }
    size_t i = hstr(s) & (m->cap - 1);
    while (m->f[i].s && strcmp(m->f[i].s, s) != 0) i = (i + 1) & (m->cap - 1);
    if (!m->f[i].s) {
        m->f[i].s = strdup(s);
        m->n++;
    }
    m->f[i].n++;
}

static Row **sorted_rows(RowMap *m, int (*cmp)(const void *, const void *), size_t *n) {
    Row **v = malloc((m->n + 1) * sizeof *v);
    size_t k = 0;
    for (size_t i = 0; i < m->cap; i++) if (m->r[i].c) v[k++] = &m->r[i];
    qsort(v, k, sizeof *v, cmp);
    *n = k;
    return v;
}

static int by_self(const void *a, const void *b) {
    const Row *x = *(Row *const *)a, *y = *(Row *const *)b;
    if (x->self != y->self) return x->self < y->self ? 1 : -1;
    return x->total < y->total ? 1 : x->total > y->total ? -1 : 0;
}

static int by_total(const void *a, const void *b) {
    const Row *x = *(Row *const *)a, *y = *(Row *const *)b;
    if (x->total != y->total) return x->total < y->total ? 1 : -1;
    return x->self < y->self ? 1 : x->self > y->self ? -1 : 0;
}

void sea_prof_finish(void) {
    if (!sea_prof_enabled || finished) return;
    finished = true;
    atomic_store(&sampling, false);
#if defined(__linux__)
    pthread_mutex_lock(&timers_mu);
    for (int i = 0; i < n_timers; i++) timer_delete(timers[i]);
    n_timers = 0;
    pthread_mutex_unlock(&timers_mu);
#else
    prof_process_timer_stop();
#endif
    struct timespec pause = { 0, 2000000 };
    nanosleep(&pause, NULL);                /* let a handler in flight finish */

    ChunkSet known = { 0 };
    cs_add(&known, root_chunk);
    if (root_vm) for (SeaChunk *c = root_vm->shared->module_chunks; c; c = c->next_owned) cs_add(&known, c);

    RowMap funcs = { 0 }, lines = { 0 };
    FoldMap folds = { 0 };
    unsigned used = atomic_load(&buf_used);
    if (used > BUF_WORDS) used = BUF_WORDS;
    unsigned total = 0, sample_id = 0;
    char *stack = malloc(MAX_DEPTH * 256 + 1);
    for (unsigned at = 0; at < used;) {
        unsigned depth = (unsigned)buf[at];
        if (depth == 0 || depth > MAX_DEPTH || at + 1 + 2 * depth > used) break;
        uintptr_t *w = buf + at + 1;
        at += 1 + 2 * depth;
        sample_id++;
        SeaChunk *cs[MAX_DEPTH];
        int ls[MAX_DEPTH], n = 0;
        for (unsigned i = 0; i < depth; i++) {
            SeaChunk *c = (SeaChunk *)w[2 * i];
            if (!cs_has(&known, c)) continue;
            int l = line_of(c, w[2 * i + 1]);
            if (l < 0) continue;
            cs[n] = c;
            ls[n] = l;
            n++;
        }
        if (n == 0) continue;
        total++;
        Row *self = rm_get(&funcs, cs[0], 0);
        self->self++;
        rm_get(&lines, cs[0], ls[0])->self++;
        for (int i = 0; i < n; i++) {
            Row *r = rm_get(&funcs, cs[i], 0);
            if (r->seen_in != sample_id) { r->seen_in = sample_id; r->total++; }
        }
        if (opts.folded) {
            size_t wpos = 0;
            for (int i = n - 1; i >= 0; i--) {
                int k = snprintf(stack + wpos, 256, "%s%s (%s:%d)", i == n - 1 ? "" : ";",
                                 fname(cs[i]), fsrc(cs[i]), first_line(cs[i]));
                if (k > 0) wpos += (size_t)(k < 256 ? k : 255);
            }
            stack[wpos] = '\0';
            fm_add(&folds, stack);
        }
    }
    free(stack);

    FILE *out = stderr;
    if (opts.out) {
        out = fopen(opts.out, "w");
        if (!out) {
            fprintf(stderr, "sea prof: cannot write %s: %s\n", opts.out, strerror(errno));
            return;
        }
    }
    if (opts.folded) {
        for (size_t i = 0; i < folds.cap; i++)
            if (folds.f[i].s) fprintf(out, "%s %u\n", folds.f[i].s, folds.f[i].n);
    } else {
        double secs = (double)total / opts.hz;
        fprintf(out, "\nsea prof: %u samples, %.2f s of CPU at %d Hz", total, secs, opts.hz);
        unsigned dropped = atomic_load(&n_dropped);
        if (dropped) fprintf(out, " (%u dropped: buffer full)", dropped);
        fprintf(out, "\n");
        if (total > 0) {
            size_t n;
            Row **v = sorted_rows(&funcs, by_self, &n);
            fprintf(out, "\n   self%%  total%%   self s  function\n");
            for (size_t i = 0; i < n && (int)i < opts.top; i++) {
                if (v[i]->self == 0) break;
                fprintf(out, "  %5.1f%%  %5.1f%%  %7.2f  %s  %s:%d\n", 100.0 * v[i]->self / total,
                        100.0 * v[i]->total / total, (double)v[i]->self / opts.hz,
                        fname(v[i]->c), fsrc(v[i]->c), first_line(v[i]->c));
            }
            free(v);
            v = sorted_rows(&funcs, by_total, &n);
            fprintf(out, "\n  total%%  total s  function (including what it calls)\n");
            for (size_t i = 0; i < n && (int)i < opts.top; i++)
                fprintf(out, "  %5.1f%%  %7.2f  %s  %s:%d\n", 100.0 * v[i]->total / total,
                        (double)v[i]->total / opts.hz, fname(v[i]->c), fsrc(v[i]->c), first_line(v[i]->c));
            free(v);
            v = sorted_rows(&lines, by_self, &n);
            fprintf(out, "\n   self%%  line\n");
            for (size_t i = 0; i < n && (int)i < opts.top; i++)
                fprintf(out, "  %5.1f%%  %s:%d  (%s)\n", 100.0 * v[i]->self / total, fsrc(v[i]->c),
                        v[i]->line, fname(v[i]->c));
            free(v);
        }
    }
    if (out != stderr) fclose(out);
}
