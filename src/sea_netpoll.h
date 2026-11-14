/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_common.h"
#include "sea_value.h"

/*
 * Netpoller: non-blocking I/O integrated with the goroutine scheduler.
 *
 * A native function that would block on a file descriptor (a socket not yet
 * readable/writable, or a sleep) calls sea_io_wait(). Inside a goroutine that
 * registers the fd (or the deadline) with the poller thread (epoll + a timer
 * heap) and returns SEA_IO_PARK; the native then returns SEA_PARK_VAL, the VM
 * parks the goroutine and its worker thread runs others. When the fd is ready
 * (or the deadline passes, or the fd is closed through sea_netpoll_close) the
 * goroutine is requeued and the SAME call instruction runs again: the native
 * retries its syscall. Outside a goroutine (main thread, or a native called
 * back from C) sea_io_wait just blocks in poll().
 *
 * Re-running a native must be idempotent. Natives keep what has to survive a
 * park in the VM's SeaIoCall (sea_io_begin): a deadline fixed on the first
 * attempt, progress (e.g. bytes already written), small extra slots.
 *
 *   SeaIoCall *io = sea_io_begin(vm, timeout_ms);       // < 0: no timeout
 *   if (io->closed) { sea_io_end(vm); return error }     // fd closed while waiting
 *   for (;;) {
 *       ssize_t n = read(fd, ...);
 *       if (n >= 0 || errno != EAGAIN) break;           // done, EOF or error
 *       int w = sea_io_wait(vm, fd, SEA_IO_READ);
 *       if (w == SEA_IO_PARK)    return SEA_PARK_VAL;   // io stays active
 *       if (w == SEA_IO_TIMEOUT) { sea_io_end(vm); return timeout error }
 *       if (w == SEA_IO_CLOSED)  { sea_io_end(vm); return closed error }
 *   }
 *   sea_io_end(vm);
 */

#define SEA_IO_READ   1
#define SEA_IO_WRITE  2

enum { SEA_IO_READY = 0, SEA_IO_TIMEOUT = 1, SEA_IO_PARK = 2, SEA_IO_CLOSED = 3 };

/* Returned by a native to park the goroutine (never visible to Sea code). */
#define SEA_PARK_VAL  (SEA_NAN_BASE | UINT64_C(0x0004000000000000))

typedef struct SeaIoCall {
    bool     active;     /* a native call is in progress (possibly parked) */
    bool     closed;     /* its fd was closed while it waited */
    int64_t  deadline;   /* absolute, sea_now_ms() clock; 0 = none */
    int64_t  progress;   /* native-defined (bytes written, ...) */
    int64_t  aux[4];     /* native-defined */
} SeaIoCall;

/* Monotonic clock in milliseconds. */
int64_t    sea_now_ms(void);

/* Start (or, after a park, resume) the current native's I/O call. */
SeaIoCall *sea_io_begin(SeaVM *vm, int64_t timeout_ms);
/* The native call finished (any outcome but a park). */
void       sea_io_end(SeaVM *vm);
/* Wait until fd is ready for `events` (fd < 0: just the deadline). */
int        sea_io_wait(SeaVM *vm, int fd, int events);

/* Close fd through the poller: a goroutine waiting on it wakes up (its
 * native sees io->closed). Use instead of close() for pollable fds. */
void       sea_netpoll_close(int fd);

/* shutdown(2) that also reaches the netpoller: after it every wait on fd, now
 * or later, ends with SEA_IO_CLOSED until the fd is closed. A listening socket
 * needs it where shutdown() on one is an error (ENOTCONN: macOS, the BSDs) and
 * wakes nobody. */
void       sea_netpoll_shutdown(int fd);
