/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_prof.h"
#include "sea_sched.h"
#include "sea_gc.h"
#include "sea_goroutine.h"
#include "sea_vm.h"
#include "sea_memory.h"
#include "sea_table.h"
#include "sea_map.h"

/* Thread-local pointer to the worker running on this OS thread.
 * NULL when called from the main thread or outside any worker. */
static __thread SeaWorker *tl_worker = NULL;

/* ---- Run queue ----------------------------------------------------------- */

void sea_runq_init(SeaRunQueue *q) {
    pthread_mutex_init(&q->mu, NULL);
    q->head  = NULL;
    q->tail  = NULL;
    q->count = 0;
}

void sea_runq_destroy(SeaRunQueue *q) {
    pthread_mutex_destroy(&q->mu);
}

/*
 * Push goroutine to the back of the queue.
 * Caller must ensure g is not already in any queue.
 */
void sea_runq_push(SeaRunQueue *q, SeaGoroutine *g) {
    g->sched_next = NULL;
    pthread_mutex_lock(&q->mu);
    if (q->tail) {
        q->tail->sched_next = g;
    } else {
        q->head = g;
    }
    q->tail  = g;
    q->count++;
    pthread_mutex_unlock(&q->mu);
}

/*
 * Pop and return the front goroutine, or NULL if the queue is empty.
 */
SeaGoroutine *sea_runq_pop(SeaRunQueue *q) {
    pthread_mutex_lock(&q->mu);
    SeaGoroutine *g = q->head;
    if (g) {
        q->head = g->sched_next;
        if (!q->head) q->tail = NULL;
        q->count--;
        g->sched_next = NULL;
    }
    pthread_mutex_unlock(&q->mu);
    return g;
}

/* ---- Goroutine pool ------------------------------------------------------ */

static void goropool_init(SeaGoroPool *p) {
    pthread_mutex_init(&p->mu, NULL);
    p->head  = NULL;
    p->count = 0;
}

static void goropool_destroy(SeaGoroPool *p) {
    SeaGoroutine *g = p->head;
    while (g) {
        SeaGoroutine *next = g->sched_next;
        if (g->vm) sea_vm_free(g->vm);
        SEA_FREE(g, SeaGoroutine);
        g = next;
    }
    p->head  = NULL;
    p->count = 0;
    pthread_mutex_destroy(&p->mu);
}

SeaGoroutine *sea_sched_pool_get(SeaScheduler *s) {
    SeaGoroPool *p = &s->goro_pool;
    pthread_mutex_lock(&p->mu);
    SeaGoroutine *g = p->head;
    if (g) {
        p->head = g->sched_next;
        p->count--;
        g->sched_next = NULL;
    }
    pthread_mutex_unlock(&p->mu);
    return g;
}

void sea_sched_pool_put(SeaScheduler *s, SeaGoroutine *g) {
    SeaGoroPool *p = &s->goro_pool;
    pthread_mutex_lock(&p->mu);
    if (p->count >= SEA_GORO_POOL_MAX) {
        pthread_mutex_unlock(&p->mu);
        if (g->vm) sea_vm_free(g->vm);
        SEA_FREE(g, SeaGoroutine);
        return;
    }
    /* Reset VM for reuse (clears frame_count to 0 → signals "needs init") */
    sea_vm_reset_child(g->vm);
    /* Reset goroutine fields; pool holds the refcount=1 reference */
    g->done             = false;
    g->result           = SEA_NIL_VAL;
    g->fn               = SEA_NIL_VAL;
    g->args             = NULL;
    g->argc             = 0;
    g->status           = GORO_RUNNABLE;
    g->sched_next       = NULL;
    g->block_chan        = NULL;
    g->block_val         = SEA_NIL_VAL;
    g->has_pending_recv  = false;
    g->parent_shared     = NULL;
    g->obj.refcount      = 1;

    g->sched_next = p->head;
    p->head       = g;
    p->count++;
    pthread_mutex_unlock(&p->mu);
}

/* ---- Scheduler ----------------------------------------------------------- */

/* Global scheduler singleton */
SeaScheduler *sea_global_sched = NULL;

SeaScheduler *sea_sched_new(int worker_count) {
    if (worker_count < 1)               worker_count = 1;
    if (worker_count > SEA_MAX_WORKERS) worker_count = SEA_MAX_WORKERS;

    SeaScheduler *s = SEA_ALLOC(SeaScheduler);
    memset(s, 0, sizeof(*s));

    s->worker_count = worker_count;
    sea_runq_init(&s->global_q);
    goropool_init(&s->goro_pool);
    pthread_mutex_init(&s->cond_mu,    NULL);
    pthread_cond_init (&s->work_avail, NULL);
    pthread_cond_init (&s->drained,    NULL);
    atomic_store(&s->active, 0);
    atomic_store(&s->shutdown, false);
    atomic_store(&s->goroutine_count, 0);

    for (int i = 0; i < worker_count; i++) {
        s->workers[i].id      = i;
        s->workers[i].sched   = s;
        s->workers[i].current = NULL;
        sea_runq_init(&s->workers[i].local_q);
    }

    return s;
}

void sea_sched_free(SeaScheduler *s) {
    if (!s) return;
    sea_runq_destroy(&s->global_q);
    goropool_destroy(&s->goro_pool);
    pthread_mutex_destroy(&s->cond_mu);
    pthread_cond_destroy(&s->work_avail);
    pthread_cond_destroy(&s->drained);
    for (int i = 0; i < s->worker_count; i++) {
        sea_runq_destroy(&s->workers[i].local_q);
    }
    SEA_FREE(s, SeaScheduler);
}

/* ---- Worker thread (Fase 3+4) -------------------------------------------- */

/*
 * Worker loop:
 *   1. Pop a runnable goroutine from the global queue
 *   2. First run: create child VM, push initial call frame
 *      Resume:    VM already has frames from a previous suspension
 *   3. sea_vm_resume() — runs until the goroutine finishes (SEA_OK) or
 *      parks on a channel (SEA_SUSPENDED).
 *   4. On SEA_OK: clean up and decrement goroutine_count.
 *      On SEA_SUSPENDED: the goroutine is already in a channel wait queue;
 *      the worker just moves on to the next runnable goroutine.
 */
static SeaGoroutine *worker_dequeue(SeaWorker *w) {
    SeaScheduler *s = w->sched;
    SeaGoroutine *g;

    /* 1. Own local queue (no contention with other workers) */
    g = sea_runq_pop(&w->local_q);
    if (g) return g;

    /* 2. Steal from other workers' local queues */
    for (int i = 0; i < s->worker_count; i++) {
        if (i == w->id) continue;
        g = sea_runq_pop(&s->workers[i].local_q);
        if (g) return g;
    }

    /* 3. Global overflow queue */
    return sea_runq_pop(&s->global_q);
}

static void sched_push_and_wake(SeaScheduler *s, SeaGoroutine *g);

static void* worker_thread(void *arg) {
    SeaWorker *w = (SeaWorker*)arg;
    SeaScheduler *s = w->sched;
    tl_worker = w;
    sea_prof_thread_start();       /* `sea prof`: this thread's CPU timer */

    while (!atomic_load(&s->shutdown)) {
        SeaGoroutine *g = worker_dequeue(w);

        if (!g) {
            /* No work — wait for signal, re-check under lock to avoid races */
            pthread_mutex_lock(&s->cond_mu);
            g = worker_dequeue(w);
            if (!g && !atomic_load(&s->shutdown)) {
                pthread_cond_wait(&s->work_avail, &s->cond_mu);
            }
            pthread_mutex_unlock(&s->cond_mu);
            if (!g) continue;
        }

        w->current = g;
        g->status  = GORO_RUNNING;
        __atomic_store_n(&g->park_state, GORO_PARK_RUNNING, __ATOMIC_RELAXED);

        bool do_init;
        if (g->vm == NULL) {
            /* Fresh goroutine: allocate VM */
            g->vm = sea_vm_new_child(g->parent_shared);
            g->parent_shared = NULL;
            do_init = true;
        } else if (g->vm->frame_count == 0) {
            /* Pool-reused goroutine: VM struct exists but was reset */
            g->vm->shared      = g->parent_shared;
            g->vm->owns_shared = false;
            g->parent_shared   = NULL;
            do_init = true;
        } else {
            /* Resuming after suspension — frames already in place */
            do_init = false;
        }

        g->vm->goroutine = g;
        sea_gc_vm = g->vm;          /* this goroutine's cycle buffer */
        /* Cache the program-wide JIT flag per-VM (read shared once here, not in
         * the OP_CALL hot path — avoids cross-thread cache contention). */
        g->vm->jit_enabled = g->vm->shared->jit_enabled;
        bool has_frame = true;
        if (do_init) {
            has_frame = sea_vm_goroutine_init(g->vm, g->fn, g->args, g->argc);
            if (g->args) {
                sea_free(g->args, sizeof(SeaValue) * (size_t)g->argc);
                g->args = NULL;
            }
        }

        /* A native function already ran inside sea_vm_goroutine_init. */
        int result = has_frame ? sea_vm_resume(g->vm) : SEA_OK;
        sea_gc_vm = NULL;

        w->current = NULL;

        if (result == SEA_SUSPENDED) {
            /* Parked (channel wait queue) or yielded. This worker is done with
             * g now; if a waker already called sea_sched_requeue while we were
             * still unwinding, it left the push to us. */
            if (__atomic_exchange_n(&g->park_state, GORO_PARK_PARKED, __ATOMIC_ACQ_REL)
                    == GORO_PARK_WOKEN)
                sched_push_and_wake(s, g);
            continue;
        }

        /* Goroutine finished: drop its result (+1, nobody reads it) and its
         * reference to the function (docs/memory.md) */
        g->result = SEA_NIL_VAL;
        {
            SeaValue r = g->vm->regs[0];
            g->vm->regs[0] = SEA_NIL_VAL;
            if (sea_is_obj(r)) sea_obj_release(sea_as_obj(r));
            if (sea_is_obj(g->fn)) sea_obj_release(sea_as_obj(g->fn));
            g->fn = SEA_NIL_VAL;
            /* what is left in its cycle buffer is garbage or shared */
            sea_gc_vm = g->vm;
            sea_gc_vm_finish(g->vm);
            sea_gc_vm = NULL;
        }
        g->done   = true;
        g->status = GORO_DONE;

        int prev = atomic_fetch_sub(&s->goroutine_count, 1);
        if (prev == 1) {
            pthread_mutex_lock(&s->cond_mu);
            pthread_cond_broadcast(&s->drained);
            pthread_mutex_unlock(&s->cond_mu);
        }

        /* Return to pool instead of freeing (pool_put frees if full) */
        sea_sched_pool_put(s, g);
    }

    tl_worker = NULL;
    return NULL;
}

void sea_sched_start(SeaScheduler *s) {
    if (atomic_load(&s->active)) return;  /* already running */
    atomic_store(&s->active, 1);
    atomic_store(&s->shutdown, false);
    for (int i = 0; i < s->worker_count; i++) {
        pthread_create(&s->workers[i].thread, NULL, worker_thread, &s->workers[i]);
    }
}

void sea_sched_stop(SeaScheduler *s) {
    if (!atomic_load(&s->active)) return;
    atomic_store(&s->shutdown, true);

    /* Wake all sleeping workers so they see the shutdown flag */
    pthread_mutex_lock(&s->cond_mu);
    pthread_cond_broadcast(&s->work_avail);
    pthread_mutex_unlock(&s->cond_mu);

    for (int i = 0; i < s->worker_count; i++) {
        pthread_join(s->workers[i].thread, NULL);
    }
    atomic_store(&s->active, 0);
}

static void sched_push_and_wake(SeaScheduler *s, SeaGoroutine *g) {
    /* Push to current worker's local queue if on a worker, else global */
    if (tl_worker && tl_worker->sched == s) {
        sea_runq_push(&tl_worker->local_q, g);
    } else {
        sea_runq_push(&s->global_q, g);
    }
    pthread_mutex_lock(&s->cond_mu);
    pthread_cond_signal(&s->work_avail);
    pthread_mutex_unlock(&s->cond_mu);
}

void sea_sched_spawn(SeaScheduler *s, SeaGoroutine *g) {
    atomic_fetch_add(&s->goroutine_count, 1);
    sched_push_and_wake(s, g);
}

/* Make a parked goroutine runnable again. Wakers (channel ops, or the
 * goroutine itself for yield/select retry) can get here while the worker that
 * ran g is still unwinding its vm_run; pushing then would let another worker
 * resume the same VM concurrently. So only push if that worker has already
 * finished suspending g (GORO_PARK_PARKED); otherwise mark it WOKEN and the
 * worker pushes it as its last step (see worker_thread). */
void sea_sched_requeue(SeaScheduler *s, SeaGoroutine *g) {
    if (__atomic_exchange_n(&g->park_state, GORO_PARK_WOKEN, __ATOMIC_ACQ_REL)
            == GORO_PARK_PARKED)
        sched_push_and_wake(s, g);
}

void sea_sched_drain(SeaScheduler *s) {
    /* Block until all goroutines have completed */
    pthread_mutex_lock(&s->cond_mu);
    while (atomic_load(&s->goroutine_count) > 0) {
        pthread_cond_wait(&s->drained, &s->cond_mu);
    }
    pthread_mutex_unlock(&s->cond_mu);
}

/* ---- Shared state -------------------------------------------------------- */

SeaShared *sea_shared_new(void) {
    SeaShared *sh = SEA_ALLOC(SeaShared);
    sh->globals = sea_table_new(TABLE_MAP);
    sh->loaded  = sea_map_new();
    sh->jit_enabled = false;
    sh->module_chunks = NULL;
    sea_table_sync_init(&sh->globals_sync);
    pthread_mutex_init(&sh->loaded_lock, NULL);
    return sh;
}

void sea_shared_free(SeaShared *sh) {
    if (!sh) return;
    if (sh->globals) sea_obj_release(&sh->globals->obj);
    if (sh->loaded)  sea_obj_release(&sh->loaded->obj);
    /* after the cache: the modules' values (closures) point into these */
    SeaChunk *c = sh->module_chunks;
    while (c) {
        SeaChunk *next = c->next_owned;
        sea_chunk_free(c);
        c = next;
    }
    sea_table_sync_destroy(&sh->globals_sync);
    pthread_mutex_destroy(&sh->loaded_lock);
    SEA_FREE(sh, SeaShared);
}
