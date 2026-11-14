/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_common.h"
#include "sea_table.h"
#include <pthread.h>
#include <stdatomic.h>

/*
 * M:N green-thread scheduler for Sea.
 *
 * M goroutines are scheduled across N OS worker threads.
 * Each goroutine carries its own SeaVM (register file + call stack),
 * so "suspending" a goroutine = stopping vm_run_slice() — no stack-
 * switching or ucontext needed.
 *
 * Implementation phases:
 *   Fase 1 (this file): types + run-queue helpers only.
 *                       Worker threads NOT started yet.
 *                       Zero behavior change from caller's perspective.
 *   Fase 2: sea_vm_new_child — goroutines share parent globals/loaded.
 *   Fase 3: worker threads + enqueue-on-spawn (replace pthread_create).
 *   Fase 4: non-blocking channel wait lists.
 *   Fase 5: atomic reference counting.
 *   Fase 6: real OP_YIELD.
 *   Fase 7: non-blocking select.
 *   Fase 8: optimisations (VM pool, smaller frames, false-sharing).
 */

/* ---- Goroutine lifecycle state ------------------------------------------ */

typedef enum {
    GORO_RUNNABLE,        /* ready to run, sitting in a run queue           */
    GORO_RUNNING,         /* currently executing on a worker thread         */
    GORO_BLOCKED_SEND,    /* blocked waiting for channel send to complete   */
    GORO_BLOCKED_RECV,    /* blocked waiting for channel recv to deliver    */
    GORO_BLOCKED_SELECT,  /* blocked in select waiting for any case to fire */
    GORO_DONE,            /* function returned; result stored in goroutine  */
} GoroStatus;

/* ---- Run queue ------------------------------------------------------------ */

/*
 * Mutex-protected FIFO of runnable goroutines.
 * Goroutines are linked via their sched_next pointer.
 */
typedef struct SeaRunQueue {
    pthread_mutex_t  mu;
    SeaGoroutine    *head;   /* front (next to dequeue) */
    SeaGoroutine    *tail;   /* back  (last enqueued)   */
    int              count;
} SeaRunQueue;

/* ---- Worker thread ------------------------------------------------------- */

struct SeaScheduler;  /* forward declaration */

typedef struct SeaWorker {
    pthread_t            thread;
    int                  id;
    struct SeaScheduler *sched;    /* back-pointer to owning scheduler */
    SeaRunQueue          local_q;  /* per-worker run queue             */
    SeaGoroutine        *current;  /* goroutine executing right now    */
} SeaWorker;

/* ---- Goroutine pool ------------------------------------------------------ */

#define SEA_GORO_POOL_MAX 256

typedef struct SeaGoroPool {
    pthread_mutex_t  mu;
    SeaGoroutine    *head;   /* linked via sched_next */
    int              count;
} SeaGoroPool;

/* ---- Scheduler ----------------------------------------------------------- */

#define SEA_MAX_WORKERS 64

typedef struct SeaScheduler {
    SeaWorker    workers[SEA_MAX_WORKERS];
    int          worker_count;

    SeaRunQueue  global_q;        /* overflow / main-thread enqueue        */
    SeaGoroPool  goro_pool;       /* reusable SeaGoroutine+SeaVM structs   */

    pthread_mutex_t cond_mu;
    pthread_cond_t  work_avail;   /* signalled when new work is pushed     */
    pthread_cond_t  drained;      /* goroutine_count reached 0 (sea_sched_drain waits here: it must
                                   * not share work_avail, or a signal meant for a worker can wake
                                   * the drainer instead and be lost) */

    _Atomic int  active;          /* 1 while workers are running, 0 = idle */
    _Atomic bool shutdown;        /* true → workers should exit            */
    _Atomic int  goroutine_count; /* live goroutines (for drain)           */
} SeaScheduler;

/* ---- Shared state (globals, module cache) ------------------------------- */

/*
 * SeaShared holds state shared by all goroutines (and the main VM).
 * Before green threads, each child VM got its own retained copy of globals
 * and loaded.  Now all VMs reference the same SeaShared.
 */
typedef struct SeaShared {
    SeaTable        *globals;       /* global variable table               */
    SeaMap          *loaded;        /* module cache (require)              */
    SeaTableSync     globals_sync;  /* globals: lock-free reads, locked writes */
    pthread_mutex_t  loaded_lock;   /* protects loaded map and module_chunks */
    SeaChunk        *module_chunks; /* the required modules' chunks: their
                                       closures live in the shared cache and in
                                       any goroutine, so they outlive the VM that
                                       loaded them (freed with SeaShared) */
    bool             jit_enabled;   /* program-wide JIT flag (--jit); goroutine
                                       child VMs share this, so they JIT too */
} SeaShared;

/* ---- Global scheduler singleton ----------------------------------------- */

extern SeaScheduler *sea_global_sched;

/* ---- API ----------------------------------------------------------------- */

/* Run-queue operations (thread-safe) */
void          sea_runq_init   (SeaRunQueue *q);
void          sea_runq_destroy(SeaRunQueue *q);
void          sea_runq_push   (SeaRunQueue *q, SeaGoroutine *g);
SeaGoroutine *sea_runq_pop    (SeaRunQueue *q);  /* NULL if empty */

/* Scheduler lifecycle */
SeaScheduler *sea_sched_new (int worker_count);
void          sea_sched_free(SeaScheduler *s);

/* Start/stop worker threads */
void          sea_sched_start(SeaScheduler *s);
void          sea_sched_stop (SeaScheduler *s);  /* sets shutdown, joins workers */

/* Enqueue a new goroutine (increments goroutine_count) */
void          sea_sched_spawn(SeaScheduler *s, SeaGoroutine *g);

/* Re-enqueue a previously suspended goroutine (does NOT increment count) */
void          sea_sched_requeue(SeaScheduler *s, SeaGoroutine *g);

/* Goroutine pool: get a reusable (reset) goroutine, or NULL if pool empty */
SeaGoroutine *sea_sched_pool_get(SeaScheduler *s);
/* Return a finished goroutine to the pool (frees if pool is full) */
void          sea_sched_pool_put(SeaScheduler *s, SeaGoroutine *g);

/* Wait for all goroutines to finish (blocks calling thread) */
void          sea_sched_drain(SeaScheduler *s);

/* Shared state lifecycle */
SeaShared    *sea_shared_new (void);
void          sea_shared_free(SeaShared *sh);
