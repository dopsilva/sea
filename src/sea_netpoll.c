/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_netpoll.h"
#include "sea_vm.h"
#include "sea_goroutine.h"
#include "sea_sched.h"
#include "sea_memory.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include "sea_compat.h"
#include <time.h>
#include <unistd.h>

/* Netpoller — see sea_netpoll.h.
 *
 * One poller thread waits in the multiplexer (epoll, or kqueue off Linux: sea_compat.h) for the registered fds (one-shot) and
 * for the nearest deadline (a binary min-heap of goroutines). Everything that
 * decides a goroutine's wake-up runs on that thread under g_mu, so an fd event,
 * its deadline and a close cannot both complete the same wait. Woken
 * goroutines are requeued with sea_sched_requeue (the channel park handshake).
 * Each goroutine waits on at most one thing; the wait state lives in its
 * SeaGoroutine (io_*). */

int64_t sea_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- Poller state -------------------------------------------------------- */

static pthread_once_t  g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_mu   = PTHREAD_MUTEX_INITIALIZER;
static int             g_epfd = -1, g_evfd = -1, g_evwr = -1;
static bool            g_ok;

static SeaGoroutine  **g_fd_waiter;     /* fd -> goroutine waiting on it */
static uint32_t       *g_fd_gen;        /* fd -> close generation */
static uint8_t        *g_fd_shut;       /* fd -> shut down by sea_netpoll_shutdown */
static int             g_fd_cap;

static SeaGoroutine  **g_heap;          /* deadline min-heap */
static int             g_heap_n, g_heap_cap;

static void *poller_main(void *arg);

static void poller_init(void) {
    g_epfd = sea_mux_create();
    if (g_epfd < 0 || sea_wake_open(&g_evfd, &g_evwr) < 0) return;
    if (!sea_mux_add(g_epfd, g_evfd, SEA_MUX_READ, false)) return;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, poller_main, NULL) == 0) g_ok = true;
    pthread_attr_destroy(&attr);
}

static void fd_reserve(int fd) {          /* g_mu held */
    if (fd < g_fd_cap) return;
    int nc = g_fd_cap ? g_fd_cap : 1024;
    while (nc <= fd) nc *= 2;
    g_fd_waiter = (SeaGoroutine**)sea_realloc(g_fd_waiter, sizeof(*g_fd_waiter) * (size_t)g_fd_cap,
                                              sizeof(*g_fd_waiter) * (size_t)nc);
    g_fd_gen = (uint32_t*)sea_realloc(g_fd_gen, sizeof(*g_fd_gen) * (size_t)g_fd_cap,
                                      sizeof(*g_fd_gen) * (size_t)nc);
    memset(g_fd_waiter + g_fd_cap, 0, sizeof(*g_fd_waiter) * (size_t)(nc - g_fd_cap));
    memset(g_fd_gen + g_fd_cap, 0, sizeof(*g_fd_gen) * (size_t)(nc - g_fd_cap));
    g_fd_shut = (uint8_t*)sea_realloc(g_fd_shut, (size_t)g_fd_cap, (size_t)nc);
    memset(g_fd_shut + g_fd_cap, 0, (size_t)(nc - g_fd_cap));
    g_fd_cap = nc;
}

static bool fd_is_shut(int fd) {
    pthread_mutex_lock(&g_mu);
    bool r = fd >= 0 && fd < g_fd_cap && g_fd_shut[fd];
    pthread_mutex_unlock(&g_mu);
    return r;
}

static uint32_t fd_gen(int fd) {
    pthread_mutex_lock(&g_mu);
    uint32_t gen = (fd >= 0 && fd < g_fd_cap) ? g_fd_gen[fd] : 0;
    pthread_mutex_unlock(&g_mu);
    return gen;
}

/* ---- Deadline heap (g_mu held) ------------------------------------------- */

static void heap_swap(int i, int j) {
    SeaGoroutine *t = g_heap[i]; g_heap[i] = g_heap[j]; g_heap[j] = t;
    g_heap[i]->io_heap = i; g_heap[j]->io_heap = j;
}
static void heap_up(int i) {
    while (i > 0) {
        int p = (i - 1) / 2;
        if (g_heap[p]->io_deadline <= g_heap[i]->io_deadline) break;
        heap_swap(i, p); i = p;
    }
}
static void heap_down(int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < g_heap_n && g_heap[l]->io_deadline < g_heap[m]->io_deadline) m = l;
        if (r < g_heap_n && g_heap[r]->io_deadline < g_heap[m]->io_deadline) m = r;
        if (m == i) break;
        heap_swap(i, m); i = m;
    }
}
static void heap_push(SeaGoroutine *g) {
    if (g_heap_n == g_heap_cap) {
        int nc = g_heap_cap ? g_heap_cap * 2 : 256;
        g_heap = (SeaGoroutine**)sea_realloc(g_heap, sizeof(*g_heap) * (size_t)g_heap_cap,
                                             sizeof(*g_heap) * (size_t)nc);
        g_heap_cap = nc;
    }
    g_heap[g_heap_n] = g;
    g->io_heap = g_heap_n++;
    heap_up(g->io_heap);
}
static void heap_remove(SeaGoroutine *g) {
    int i = g->io_heap;
    if (i < 0) return;
    g->io_heap = -1;
    g_heap_n--;
    if (i == g_heap_n) return;
    g_heap[i] = g_heap[g_heap_n];
    g_heap[i]->io_heap = i;
    heap_down(i);
    heap_up(i);
}

/* ---- Completing a wait (g_mu held) --------------------------------------- */

typedef struct { SeaGoroutine **v; int n, cap; } WakeList;

static void complete(SeaGoroutine *g, WakeList *wl) {
    if (!g->io_waiting) return;
    g->io_waiting = false;
    heap_remove(g);
    int fd = g->io_fd;
    if (fd >= 0) {
        if (fd < g_fd_cap && g_fd_waiter[fd] == g) g_fd_waiter[fd] = NULL;
        sea_mux_del(g_epfd, fd);
    }
    if (wl->n == wl->cap) {
        int nc = wl->cap ? wl->cap * 2 : 16;
        wl->v = (SeaGoroutine**)sea_realloc(wl->v, sizeof(*wl->v) * (size_t)wl->cap,
                                            sizeof(*wl->v) * (size_t)nc);
        wl->cap = nc;
    }
    wl->v[wl->n++] = g;
}

static void wake_all(WakeList *wl) {       /* g_mu NOT held */
    for (int i = 0; i < wl->n; i++) sea_sched_requeue(sea_global_sched, wl->v[i]);
    if (wl->v) sea_free(wl->v, sizeof(*wl->v) * (size_t)wl->cap);
    wl->v = NULL; wl->n = wl->cap = 0;
}

static void *poller_main(void *arg) {
    (void)arg;
    int ready[128];
    for (;;) {
        pthread_mutex_lock(&g_mu);
        int timeout = -1;
        if (g_heap_n > 0) {
            int64_t d = g_heap[0]->io_deadline - sea_now_ms();
            timeout = d <= 0 ? 0 : (d > 60000 ? 60000 : (int)d);
        }
        pthread_mutex_unlock(&g_mu);

        int n = sea_mux_wait(g_epfd, ready, 128, timeout);
        if (n < 0) {
            if (errno != EINTR) usleep(1000);      /* do not spin on a failing multiplexer */
            n = 0;
        }

        WakeList wl = {0};
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < n; i++) {
            int fd = ready[i];
            if (fd == g_evfd) {
                sea_wake_drain(g_evfd);
                continue;
            }
            SeaGoroutine *g = (fd >= 0 && fd < g_fd_cap) ? g_fd_waiter[fd] : NULL;
            if (g && g->io_waiting && g->io_fd == fd) complete(g, &wl);
        }
        int64_t now = sea_now_ms();
        while (g_heap_n > 0 && g_heap[0]->io_deadline <= now) complete(g_heap[0], &wl);
        pthread_mutex_unlock(&g_mu);
        wake_all(&wl);
    }
    return NULL;
}

/* Register g's wait; false if the poller cannot take it (the caller then
 * blocks instead). g_mu not held. */
static bool poll_register(SeaGoroutine *g, int fd, int events, int64_t deadline) {
    pthread_once(&g_once, poller_init);
    if (!g_ok || !sea_global_sched) return false;
    if (fd < 0 && !deadline) return false;
    pthread_mutex_lock(&g_mu);
    if (fd >= 0) {
        fd_reserve(fd);
        if (g_fd_shut[fd]) {               /* shut down: the caller reports it */
            pthread_mutex_unlock(&g_mu);
            return false;
        }
        if (g_fd_waiter[fd] && g_fd_waiter[fd] != g) {   /* another waiter */
            pthread_mutex_unlock(&g_mu);
            return false;
        }
        if (!sea_mux_add(g_epfd, fd, ((events & SEA_IO_READ) ? SEA_MUX_READ : 0) |
                                     ((events & SEA_IO_WRITE) ? SEA_MUX_WRITE : 0), true)) {
            pthread_mutex_unlock(&g_mu);
            return false;                  /* e.g. a regular file */
        }
        g_fd_waiter[fd] = g;
    }
    g->io_fd = fd;
    g->io_deadline = deadline;
    g->io_heap = -1;
    g->io_waiting = true;
    bool new_first = false;
    if (deadline) {
        heap_push(g);
        new_first = (g->io_heap == 0);
    }
    pthread_mutex_unlock(&g_mu);
    if (new_first) {                       /* the poller may sleep past it */
        sea_wake_post(g_evwr);
    }
    return true;
}

void sea_netpoll_close(int fd) {
    if (fd < 0) return;
    WakeList wl = {0};
    pthread_mutex_lock(&g_mu);
    if (fd < g_fd_cap) {
        g_fd_gen[fd]++;
        g_fd_shut[fd] = 0;
        SeaGoroutine *g = g_fd_waiter[fd];
        if (g && g->io_waiting) {
            g->vm->io_call.closed = true;  /* parked: nobody else touches it */
            complete(g, &wl);
        }
    }
    pthread_mutex_unlock(&g_mu);
    close(fd);
    wake_all(&wl);
}

void sea_netpoll_shutdown(int fd) {
    if (fd < 0) return;
    WakeList wl = {0};
    pthread_mutex_lock(&g_mu);
    fd_reserve(fd);
    g_fd_shut[fd] = 1;
    g_fd_gen[fd]++;                        /* a thread blocked in poll notices too */
    SeaGoroutine *g = g_fd_waiter[fd];
    if (g && g->io_waiting) {
        g->vm->io_call.closed = true;
        complete(g, &wl);
    }
    pthread_mutex_unlock(&g_mu);
    wake_all(&wl);
}

/* ---- Native-call state --------------------------------------------------- */

SeaIoCall *sea_io_begin(SeaVM *vm, int64_t timeout_ms) {
    SeaIoCall *io = &vm->io_call;
    if (!io->active) {
        memset(io, 0, sizeof *io);
        io->active = true;
        io->deadline = timeout_ms >= 0 ? sea_now_ms() + timeout_ms : 0;
    }
    return io;
}

void sea_io_end(SeaVM *vm) {
    vm->io_call.active = false;
    vm->io_call.closed = false;
}

int sea_io_wait(SeaVM *vm, int fd, int events) {
    SeaIoCall *io = &vm->io_call;
    int64_t dl = io->deadline;
    if (io->closed) return SEA_IO_CLOSED;
    if (fd >= 0 && fd_is_shut(fd)) return SEA_IO_CLOSED;
    if (dl && sea_now_ms() >= dl) return SEA_IO_TIMEOUT;

    /* In a goroutine whose frame can be resumed: park. */
    SeaGoroutine *g = vm->goroutine;
    if (g && vm->native_depth == 0 && poll_register(g, fd, events, dl))
        return SEA_IO_PARK;

    /* Otherwise block this thread, in slices, so a close by another thread
     * (fd generation bump) is noticed. */
    uint32_t gen = fd >= 0 ? fd_gen(fd) : 0;
    for (;;) {
        if (fd >= 0 && fd_is_shut(fd)) return SEA_IO_CLOSED;
        int slice = 200;
        if (dl) {
            int64_t rem = dl - sea_now_ms();
            if (rem <= 0) return SEA_IO_TIMEOUT;
            if (rem < slice) slice = (int)rem;
        }
        if (fd < 0) {
            if (!dl) return SEA_IO_READY;
            struct timespec ts = { slice / 1000, (long)(slice % 1000) * 1000000L };
            nanosleep(&ts, NULL);
            continue;
        }
        struct pollfd p = { .fd = fd,
                            .events = (short)(((events & SEA_IO_READ) ? POLLIN : 0) |
                                              ((events & SEA_IO_WRITE) ? POLLOUT : 0)) };
        int r = poll(&p, 1, slice);
        if (r > 0) return (p.revents & POLLNVAL) ? SEA_IO_CLOSED : SEA_IO_READY;
        if (r < 0 && errno != EINTR) return SEA_IO_READY;   /* the syscall reports it */
        if (fd_gen(fd) != gen) return SEA_IO_CLOSED;
    }
}
