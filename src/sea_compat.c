/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_compat.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(SEA_LINUX)
#  include <sys/epoll.h>
#  include <sys/eventfd.h>
#  include <sys/random.h>
#  include <sys/syscall.h>
#else
#  include <sys/event.h>
#  include <sys/time.h>
#endif
#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

/* ---- descriptors ---------------------------------------------------------- */

__attribute__((unused)) static int set_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD);
    return fl < 0 ? -1 : fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

__attribute__((unused)) static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int sea_pipe_cloexec(int p[2]) {
#if defined(SEA_LINUX)
    return pipe2(p, O_CLOEXEC);
#else
    if (pipe(p) != 0) return -1;
    if (set_cloexec(p[0]) != 0 || set_cloexec(p[1]) != 0) {
        int e = errno;
        close(p[0]); close(p[1]);
        errno = e;
        return -1;
    }
    return 0;
#endif
}

#if !defined(SEA_LINUX)
/* Finish a socket the way SOCK_NONBLOCK | SOCK_CLOEXEC would have. */
static int socket_finish(int fd) {
    if (fd < 0) return -1;
    if (set_nonblock(fd) != 0 || set_cloexec(fd) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
#  ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#  endif
    return fd;
}
#endif

int sea_socket_nb(int domain, int type, int protocol) {
#if defined(SEA_LINUX)
    return socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
#else
    return socket_finish(socket(domain, type, protocol));
#endif
}

int sea_accept_nb(int lfd) {
#if defined(SEA_LINUX)
    return accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    return socket_finish(accept(lfd, NULL, NULL));
#endif
}

/* ---- wake-up channel ------------------------------------------------------ */

int sea_wake_open(int *rd, int *wr) {
#if defined(SEA_LINUX)
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) return -1;
    *rd = *wr = fd;
    return 0;
#else
    int p[2];
    if (sea_pipe_cloexec(p) != 0) return -1;
    if (set_nonblock(p[0]) != 0 || set_nonblock(p[1]) != 0) {
        close(p[0]); close(p[1]);
        return -1;
    }
    *rd = p[0];
    *wr = p[1];
    return 0;
#endif
}

void sea_wake_post(int wr) {
#if defined(SEA_LINUX)
    uint64_t one = 1;
    ssize_t w = write(wr, &one, sizeof one);
    (void)w;
#else
    char b = 1;
    ssize_t w = write(wr, &b, 1);      /* a full pipe already means "woken" */
    (void)w;
#endif
}

void sea_wake_drain(int rd) {
#if defined(SEA_LINUX)
    uint64_t v;
    while (read(rd, &v, sizeof v) > 0) {}
#else
    char buf[256];
    while (read(rd, buf, sizeof buf) > 0) {}
#endif
}

void sea_wake_close_wr(int rd, int wr) {
    if (wr >= 0 && wr != rd) close(wr);
}

/* ---- system facilities ---------------------------------------------------- */

bool sea_sys_random(unsigned char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
#if defined(SEA_LINUX)
        ssize_t r = getrandom(buf + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += (size_t)r;
#elif defined(__APPLE__)
        size_t chunk = n - got > 256 ? 256 : n - got;     /* getentropy: at most 256 */
        if (getentropy(buf + got, chunk) != 0) return false;
        got += chunk;
#else
        arc4random_buf(buf + got, n - got);
        got = n;
#endif
    }
    return true;
}

long sea_exe_path(char *out, size_t cap) {
    if (cap < 2) return -1;
#if defined(SEA_LINUX)
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return -1;
    out[n] = '\0';
    return (long)n;
#elif defined(__APPLE__)
    char raw[PATH_MAX];
    uint32_t sz = sizeof raw;
    if (_NSGetExecutablePath(raw, &sz) != 0) return -1;
    char real[PATH_MAX];
    if (!realpath(raw, real)) return -1;
    size_t n = strlen(real);
    if (n >= cap) return -1;
    memcpy(out, real, n + 1);
    return (long)n;
#else
    ssize_t n = readlink("/proc/curproc/file", out, cap - 1);   /* FreeBSD with procfs */
    if (n <= 0) return -1;
    out[n] = '\0';
    return (long)n;
#endif
}

int sea_exe_open(void) {
#if defined(SEA_LINUX)
    return open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
#else
    char path[PATH_MAX];
    if (sea_exe_path(path, sizeof path) < 0) return -1;
    return open(path, O_RDONLY | O_CLOEXEC);
#endif
}

int sea_mem_library(const char *name, const void *data, size_t size, char *path, size_t pathcap, bool *tmp) {
    *tmp = false;
    int fd;
#if defined(SEA_LINUX)
    fd = (int)syscall(SYS_memfd_create, name, 1u /* MFD_CLOEXEC */);
    if (fd >= 0) snprintf(path, pathcap, "/proc/self/fd/%d", fd);
#else
    (void)name;
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(path, pathcap, "%s/sea-lib-XXXXXX", dir);
    fd = mkstemp(path);
    if (fd >= 0) { set_cloexec(fd); *tmp = true; }
#endif
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < size) {
        ssize_t w = write(fd, (const char *)data + done, size - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += (size_t)w;
    }
    if (done != size) {
        close(fd);
        if (*tmp) unlink(path);
        return -1;
    }
    return fd;
}

/* ---- multiplexer: epoll --------------------------------------------------- */
#if defined(SEA_MUX_EPOLL)

int sea_mux_create(void) { return epoll_create1(EPOLL_CLOEXEC); }

bool sea_mux_add(int mux, int fd, int events, bool oneshot) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = (oneshot ? EPOLLONESHOT | EPOLLRDHUP : 0) |
                ((events & SEA_MUX_READ) ? EPOLLIN : 0) |
                ((events & SEA_MUX_WRITE) ? EPOLLOUT : 0);
    ev.data.fd = fd;
    if (epoll_ctl(mux, EPOLL_CTL_ADD, fd, &ev) == 0) return true;
    return errno == EEXIST && epoll_ctl(mux, EPOLL_CTL_MOD, fd, &ev) == 0;
}

void sea_mux_del(int mux, int fd) { epoll_ctl(mux, EPOLL_CTL_DEL, fd, NULL); }

int sea_mux_wait(int mux, int *out, int max, int timeout_ms) {
    struct epoll_event evs[128];
    if (max > 128) max = 128;
    int n = epoll_wait(mux, evs, max, timeout_ms);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) out[i] = evs[i].data.fd;
    return n;
}

int sea_mux_add_pid(int mux, pid_t pid) {
    int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) return -1;
    if (!sea_mux_add(mux, pidfd, SEA_MUX_READ, false)) { close(pidfd); return -1; }
    return pidfd;
}

void sea_mux_del_pid(int mux, int handle, pid_t pid) {
    (void)pid;
    if (handle < 0) return;
    epoll_ctl(mux, EPOLL_CTL_DEL, handle, NULL);
    close(handle);
}

int sea_pid_waitfd(pid_t pid) { return (int)syscall(SYS_pidfd_open, pid, 0); }

/* ---- multiplexer: kqueue -------------------------------------------------- */
#else

int sea_mux_create(void) {
    int kq = kqueue();
    if (kq >= 0) set_cloexec(kq);
    return kq;
}

static bool kq_change(int kq, uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags) {
    struct kevent kev;
    EV_SET(&kev, ident, filter, flags, fflags, 0, NULL);
    return kevent(kq, &kev, 1, NULL, 0, NULL) == 0;
}

bool sea_mux_add(int mux, int fd, int events, bool oneshot) {
    /* replace any earlier interest in this fd (EV_ADD of an existing filter
     * would only update it, and a filter no longer wanted would stay) */
    sea_mux_del(mux, fd);
    uint16_t fl = (uint16_t)(EV_ADD | EV_ENABLE | (oneshot ? EV_ONESHOT : 0));
    bool ok = true;
    if (events & SEA_MUX_READ)  ok = kq_change(mux, (uintptr_t)fd, EVFILT_READ, fl, 0) && ok;
    if (events & SEA_MUX_WRITE) {
        if (!kq_change(mux, (uintptr_t)fd, EVFILT_WRITE, fl, 0)) ok = false;
    }
    if (!ok) sea_mux_del(mux, fd);
    return ok;
}

void sea_mux_del(int mux, int fd) {
    kq_change(mux, (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0);
    kq_change(mux, (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0);
}

int sea_mux_wait(int mux, int *out, int max, int timeout_ms) {
    struct kevent evs[128];
    if (max > 128) max = 128;
    struct timespec ts, *tp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tp = &ts;
    }
    int n = kevent(mux, NULL, 0, evs, max, tp);
    if (n < 0) return -1;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (evs[i].filter != EVFILT_READ && evs[i].filter != EVFILT_WRITE) continue;
        out[k++] = (int)evs[i].ident;
    }
    return k;
}

/* The exit is a one-shot event: process.run re-arms it on every turn of its
 * loop (the kqueue is waited on as a file descriptor and never read). */
int sea_mux_add_pid(int mux, pid_t pid) {
    if (!kq_change(mux, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT)) return -1;
    return (int)pid;
}

void sea_mux_del_pid(int mux, int handle, pid_t pid) {
    if (handle < 0) return;
    kq_change(mux, (uintptr_t)pid, EVFILT_PROC, EV_DELETE, 0);
}

int sea_pid_waitfd(pid_t pid) {
    int kq = sea_mux_create();
    if (kq < 0) return -1;
    if (!kq_change(kq, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT)) {
        close(kq);
        return -1;
    }
    return kq;
}

#endif
