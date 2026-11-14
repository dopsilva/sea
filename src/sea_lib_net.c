/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_stdlib.h"
#include "sea_netpoll.h"
#include "sea_string.h"
#include "sea_rope.h"
#include "sea_table.h"
#include "sea_memory.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include "sea_compat.h"
#include <sys/socket.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * The `net` module: TCP sockets on the netpoller (sea_netpoll.h). All sockets
 * are non-blocking; an operation that has to wait parks the goroutine (or, on
 * the main thread, blocks it) until the socket is ready or the timeout passes.
 * Failures return an error value (is_error); a timeout's message is "timeout".
 *
 *   net.connect(host, port [, timeout_ms])  → fd
 *   net.listen(port [, host [, backlog]])   → fd     (port 0: any free port)
 *   net.accept(fd [, timeout_ms])           → fd
 *   net.read(fd, max [, timeout_ms])        → string ("" at end of stream)
 *   net.write(fd, data [, timeout_ms])      → bytes written (all of data)
 *   net.close(fd),   net.shutdown(fd)   (wake its owner without freeing the fd)
 *   net.local_port(fd) → int,   net.peer(fd) → "ip:port"
 */

static int64_t opt_ms(int argc, SeaValue *args, int i) {
    if (argc <= i) return -1;
    SeaValue v = args[i];
    if (sea_is_int(v)) return sea_as_int(v) > 0 ? sea_as_int(v) : -1;
    if (sea_is_number(v)) return sea_as_number(v) > 0 ? (int64_t)sea_as_number(v) : -1;
    return -1;
}

static SeaValue net_err(SeaVM *vm, const char *what, int err) {
    char buf[160];
    if (err) snprintf(buf, sizeof buf, "%s: %s", what, strerror(err));
    else snprintf(buf, sizeof buf, "%s", what);
    return sea_make_error_str(vm, buf);
}

/* The common tail of a wait that did not park. */
static SeaValue wait_failed(SeaVM *vm, int w, const char *what) {
    sea_io_end(vm);
    if (w == SEA_IO_TIMEOUT) return sea_make_error_str(vm, "timeout");
    if (w == SEA_IO_CLOSED)  return net_err(vm, what, EBADF);
    return net_err(vm, what, 0);
}

static void set_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static bool arg_fd(int argc, SeaValue *args, int *fd) {
    if (argc < 1 || !sea_is_int(args[0])) return false;
    *fd = (int)sea_as_int(args[0]);
    return *fd >= 0;
}

/* ---- Name resolution off the scheduler threads ---------------------------
 * getaddrinfo blocks (DNS), so lookups run on a few resolver threads. Each
 * job has an eventfd the caller waits on through the netpoller (a goroutine
 * parks; the main thread blocks in poll, still honouring its timeout). A job
 * is shared by the caller and the resolver (refs = 2): a caller that times out
 * just drops its reference and the resolver frees the job when it finishes. */

typedef struct DnsJob {
    struct DnsJob   *next;
    atomic_int       refs;
    atomic_int       done;
    int              efd, wfd;      /* wake-up channel: read end, write end */
    int              gai;
    struct addrinfo *res;
    char             port[32];
    char             host[];
} DnsJob;

#define DNS_MAX_THREADS 8

static pthread_mutex_t dns_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  dns_cv = PTHREAD_COND_INITIALIZER;
static DnsJob *dns_head, *dns_tail;
static int dns_threads, dns_idle;

static void dns_job_unref(DnsJob *j) {
    if (atomic_fetch_sub(&j->refs, 1) != 1) return;
    if (j->res) freeaddrinfo(j->res);
    sea_netpoll_close(j->efd);
    sea_wake_close_wr(j->efd, j->wfd);
    free(j);
}

static void *dns_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&dns_mu);
        while (!dns_head) {
            dns_idle++;
            pthread_cond_wait(&dns_cv, &dns_mu);
            dns_idle--;
        }
        DnsJob *j = dns_head;
        dns_head = j->next;
        if (!dns_head) dns_tail = NULL;
        pthread_mutex_unlock(&dns_mu);

        struct addrinfo hints = {0};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        j->gai = getaddrinfo(j->host, j->port, &hints, &j->res);
        atomic_store(&j->done, 1);
        sea_wake_post(j->wfd);
        dns_job_unref(j);
    }
    return NULL;
}

static DnsJob *dns_submit(const char *host, const char *port) {
    size_t hl = strlen(host);
    DnsJob *j = calloc(1, sizeof *j + hl + 1);
    if (!j) return NULL;
    if (sea_wake_open(&j->efd, &j->wfd) < 0) { free(j); return NULL; }
    atomic_init(&j->refs, 2);
    atomic_init(&j->done, 0);
    memcpy(j->host, host, hl + 1);
    snprintf(j->port, sizeof j->port, "%s", port);

    pthread_mutex_lock(&dns_mu);
    if (dns_tail) dns_tail->next = j; else dns_head = j;
    dns_tail = j;
    if (dns_idle == 0 && dns_threads < DNS_MAX_THREADS) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, dns_worker, NULL) == 0) dns_threads++;
        pthread_attr_destroy(&at);
    }
    bool none = dns_threads == 0;
    if (none) {                         /* could not start any resolver thread */
        if (dns_head == j) dns_head = dns_tail = NULL;
    } else {
        pthread_cond_signal(&dns_cv);
    }
    pthread_mutex_unlock(&dns_mu);
    if (none) {
        close(j->efd);
        sea_wake_close_wr(j->efd, j->wfd);
        free(j);
        return NULL;
    }
    return j;
}

/* Resolve host:port for net.connect. The result is kept in io->aux[3] (job in
 * aux[2] while it runs) so a resumed call does not resolve again. Returns
 * SEA_IO_READY with *out set, SEA_IO_PARK, SEA_IO_TIMEOUT, or -1 with *gai
 * set (a getaddrinfo error). */
static int resolve(SeaVM *vm, SeaIoCall *io, const char *host, const char *port,
                   struct addrinfo **out, int *gai) {
    if (io->aux[3]) { *out = (struct addrinfo *)(intptr_t)io->aux[3]; return SEA_IO_READY; }

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    if (getaddrinfo(host, port, &hints, &res) == 0) {      /* an IP: no lookup */
        io->aux[3] = (int64_t)(intptr_t)res;
        *out = res;
        return SEA_IO_READY;
    }

    DnsJob *j = (DnsJob *)(intptr_t)io->aux[2];
    if (!j) {
        j = dns_submit(host, port);
        if (!j) {                                           /* fall back: block */
            hints.ai_flags = 0;
            *gai = getaddrinfo(host, port, &hints, &res);
            if (*gai != 0) return -1;
            io->aux[3] = (int64_t)(intptr_t)res;
            *out = res;
            return SEA_IO_READY;
        }
        io->aux[2] = (int64_t)(intptr_t)j;
    }
    for (;;) {
        if (atomic_load(&j->done)) {
            io->aux[2] = 0;
            *gai = j->gai;
            res = j->res;
            j->res = NULL;
            dns_job_unref(j);
            if (*gai != 0) return -1;
            io->aux[3] = (int64_t)(intptr_t)res;
            *out = res;
            return SEA_IO_READY;
        }
        int w = sea_io_wait(vm, j->efd, SEA_IO_READ);
        if (w == SEA_IO_PARK) return SEA_IO_PARK;
        if (w != SEA_IO_READY) {
            io->aux[2] = 0;
            dns_job_unref(j);
            return SEA_IO_TIMEOUT;
        }
    }
}

static void free_resolved(SeaIoCall *io) {
    if (io->aux[3]) freeaddrinfo((struct addrinfo *)(intptr_t)io->aux[3]);
    io->aux[3] = 0;
}

/* net.connect(host, port [, timeout_ms]). Across a park, aux[0] holds the
 * connecting fd + 1 and aux[1] the index of the address being tried. */
static SeaValue net_connect(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) return net_err(vm, "connect: host and port expected", 0);
    SeaString *host = sea_value_get_flat_string(args[0]);
    if (!host) return net_err(vm, "connect: host must be a string", 0);
    char port[32];
    if (sea_is_int(args[1])) snprintf(port, sizeof port, "%lld", (long long)sea_as_int(args[1]));
    else {
        SeaString *ps = sea_value_get_flat_string(args[1]);
        if (!ps) { sea_obj_release(&host->obj); return net_err(vm, "connect: bad port", 0); }
        snprintf(port, sizeof port, "%s", ps->data);
        sea_obj_release(&ps->obj);
    }

    SeaIoCall *io = sea_io_begin(vm, opt_ms(argc, args, 2));
    struct addrinfo *res = NULL;
    int gai = 0;
    int r = resolve(vm, io, host->data, port, &res, &gai);
    sea_obj_release(&host->obj);
    if (r == SEA_IO_PARK) return SEA_PARK_VAL;
    if (r == SEA_IO_TIMEOUT) {
        sea_io_end(vm);
        return sea_make_error_str(vm, "timeout");
    }
    if (r != SEA_IO_READY) {
        sea_io_end(vm);
        char buf[160];
        snprintf(buf, sizeof buf, "connect: %s", gai_strerror(gai));
        return sea_make_error_str(vm, buf);
    }

    int last_err = ECONNREFUSED;
    int idx = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next, idx++) {
        if (idx < io->aux[1]) continue;
        int fd;
        if (io->aux[0]) {                       /* resumed: connect in progress */
            fd = (int)io->aux[0] - 1;
            if (io->closed) { last_err = EBADF; io->aux[0] = 0; io->aux[1] = idx + 1; continue; }
            int err = 0; socklen_t el = sizeof err;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0) err = errno;
            if (err == 0) {
                /* writable and no error: connected? (poll may wake early) */
                struct sockaddr_storage peer; socklen_t pl = sizeof peer;
                if (getpeername(fd, (struct sockaddr*)&peer, &pl) == 0) {
                    free_resolved(io);
                    sea_io_end(vm);
                    set_nodelay(fd);
                    return sea_int_val(fd);
                }
                err = EINPROGRESS;
            }
            if (err != EINPROGRESS && err != EALREADY) {
                close(fd);
                io->aux[0] = 0; io->aux[1] = idx + 1;
                last_err = err;
                continue;
            }
        } else {
            fd = sea_socket_nb(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0) { last_err = errno; continue; }
            if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
                free_resolved(io);
                sea_io_end(vm);
                set_nodelay(fd);
                return sea_int_val(fd);
            }
            if (errno != EINPROGRESS) { last_err = errno; close(fd); io->aux[1] = idx + 1; continue; }
            io->aux[0] = fd + 1;
            io->aux[1] = idx;
        }
        int w = sea_io_wait(vm, fd, SEA_IO_WRITE);
        if (w == SEA_IO_PARK) return SEA_PARK_VAL;   /* res stays in aux[3] */
        if (w == SEA_IO_READY) {          /* blocking path: check it now */
            int err = 0; socklen_t el = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err == 0) {
                free_resolved(io);
                sea_io_end(vm);
                set_nodelay(fd);
                return sea_int_val(fd);
            }
            last_err = err;
        } else {
            last_err = (w == SEA_IO_TIMEOUT) ? ETIMEDOUT : EBADF;
        }
        close(fd);
        io->aux[0] = 0; io->aux[1] = idx + 1;
        if (w == SEA_IO_TIMEOUT) break;
    }
    free_resolved(io);
    sea_io_end(vm);
    if (last_err == ETIMEDOUT) return sea_make_error_str(vm, "timeout");
    return net_err(vm, "connect", last_err);
}

/* net.listen(port [, host [, backlog]]) */
static SeaValue net_listen(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !(sea_is_int(args[0]) || sea_is_number(args[0])))
        return net_err(vm, "listen: port expected", 0);
    int port = sea_is_int(args[0]) ? (int)sea_as_int(args[0]) : (int)sea_as_number(args[0]);
    char host[256] = "";
    if (argc >= 2 && !sea_is_nil(args[1])) {
        SeaString *h = sea_value_get_flat_string(args[1]);
        if (h) { snprintf(host, sizeof host, "%s", h->data); sea_obj_release(&h->obj); }
    }
    int backlog = (argc >= 3 && sea_is_int(args[2])) ? (int)sea_as_int(args[2]) : 512;

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = host[0] ? AF_UNSPEC : AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char pbuf[16];
    snprintf(pbuf, sizeof pbuf, "%d", port);
    if (getaddrinfo(host[0] ? host : NULL, pbuf, &hints, &res) != 0 || !res) {
        /* no IPv6: plain IPv4 any-address */
        hints.ai_family = AF_INET;
        if (getaddrinfo(host[0] ? host : NULL, pbuf, &hints, &res) != 0)
            return net_err(vm, "listen: cannot resolve address", 0);
    }
    int last_err = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        int fd = sea_socket_nb(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) { last_err = errno; continue; }
        int one = 1, zero = 0;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (p->ai_family == AF_INET6)            /* dual stack */
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0 && listen(fd, backlog) == 0) {
            freeaddrinfo(res);
            return sea_int_val(fd);
        }
        last_err = errno;
        close(fd);
    }
    freeaddrinfo(res);
    return net_err(vm, "listen", last_err);
}

/* net.accept(fd [, timeout_ms]) */
static SeaValue net_accept(SeaVM *vm, int argc, SeaValue *args) {
    int lfd;
    if (!arg_fd(argc, args, &lfd)) return net_err(vm, "accept: fd expected", 0);
    SeaIoCall *io = sea_io_begin(vm, opt_ms(argc, args, 1));
    if (io->closed) return wait_failed(vm, SEA_IO_CLOSED, "accept");
    for (;;) {
        int fd = sea_accept_nb(lfd);
        if (fd >= 0) {
            sea_io_end(vm);
            set_nodelay(fd);
            return sea_int_val(fd);
        }
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            int e = errno;
            sea_io_end(vm);
            return net_err(vm, "accept", e);
        }
        int w = sea_io_wait(vm, lfd, SEA_IO_READ);
        if (w == SEA_IO_PARK) return SEA_PARK_VAL;
        if (w != SEA_IO_READY) return wait_failed(vm, w, "accept");
    }
}

/* net.read(fd, max [, timeout_ms]) */
static SeaValue net_read(SeaVM *vm, int argc, SeaValue *args) {
    int fd;
    if (!arg_fd(argc, args, &fd)) return net_err(vm, "read: fd expected", 0);
    int64_t max = (argc >= 2 && sea_is_int(args[1])) ? sea_as_int(args[1]) : 65536;
    if (max <= 0) max = 65536;
    if (max > (1 << 24)) max = 1 << 24;
    SeaIoCall *io = sea_io_begin(vm, opt_ms(argc, args, 2));
    if (io->closed) return wait_failed(vm, SEA_IO_CLOSED, "read");
    char stackbuf[16384];
    char *buf = max <= (int64_t)sizeof stackbuf ? stackbuf : (char*)sea_malloc((size_t)max);
    SeaValue result;
    for (;;) {
        ssize_t n = recv(fd, buf, (size_t)max, 0);
        if (n >= 0) {
            sea_io_end(vm);
            result = sea_obj_val(&sea_string_new(buf, (size_t)n)->obj);
            break;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            int e = errno;
            sea_io_end(vm);
            result = (e == ECONNRESET) ? sea_obj_val(&sea_string_new("", 0)->obj)  /* as EOF */
                                       : net_err(vm, "read", e);
            break;
        }
        int w = sea_io_wait(vm, fd, SEA_IO_READ);
        if (w == SEA_IO_PARK) { result = SEA_PARK_VAL; break; }
        if (w != SEA_IO_READY) { result = wait_failed(vm, w, "read"); break; }
    }
    if (buf != stackbuf) sea_free(buf, (size_t)max);
    return result;
}

/* net.write(fd, data [, timeout_ms]): writes all of data. io->progress holds
 * the bytes already sent across parks. */
static SeaValue net_write(SeaVM *vm, int argc, SeaValue *args) {
    int fd;
    if (!arg_fd(argc, args, &fd)) return net_err(vm, "write: fd expected", 0);
    SeaString *s = argc >= 2 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!s) return net_err(vm, "write: string expected", 0);
    SeaIoCall *io = sea_io_begin(vm, opt_ms(argc, args, 2));
    SeaValue result;
    if (io->closed) { sea_obj_release(&s->obj); return wait_failed(vm, SEA_IO_CLOSED, "write"); }
    for (;;) {
        size_t sent = (size_t)io->progress;
        if (sent >= s->len) {
            sea_io_end(vm);
            result = sea_int_val((int64_t)s->len);
            break;
        }
        ssize_t n = send(fd, s->data + sent, s->len - sent, SEA_MSG_NOSIGNAL);
        if (n > 0) { io->progress += n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int w = sea_io_wait(vm, fd, SEA_IO_WRITE);
            if (w == SEA_IO_PARK) { result = SEA_PARK_VAL; break; }
            if (w != SEA_IO_READY) { result = wait_failed(vm, w, "write"); break; }
            continue;
        }
        int e = n < 0 ? errno : EPIPE;
        sea_io_end(vm);
        result = net_err(vm, "write", e);
        break;
    }
    sea_obj_release(&s->obj);
    return result;
}

static SeaValue net_close(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int fd;
    if (arg_fd(argc, args, &fd)) sea_netpoll_close(fd);
    return SEA_NIL_VAL;
}

/* net.shutdown(fd): end both directions without releasing the fd. A goroutine
 * reading or accepting on it wakes up (EOF / error) and closes it itself —
 * the way to stop a socket another goroutine owns: closing it from here could
 * let its number be reused under that goroutine's next read. */
static SeaValue net_shutdown(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int fd;
    if (arg_fd(argc, args, &fd) && shutdown(fd, SHUT_RDWR) != 0 && errno == ENOTCONN)
        sea_netpoll_shutdown(fd);      /* a listening socket off Linux: shutdown() did nothing */
    return SEA_NIL_VAL;
}

static SeaValue net_local_port(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int fd;
    if (!arg_fd(argc, args, &fd)) return SEA_NIL_VAL;
    struct sockaddr_storage sa; socklen_t sl = sizeof sa;
    if (getsockname(fd, (struct sockaddr*)&sa, &sl) < 0) return SEA_NIL_VAL;
    int port = sa.ss_family == AF_INET6 ? ntohs(((struct sockaddr_in6*)&sa)->sin6_port)
                                        : ntohs(((struct sockaddr_in*)&sa)->sin_port);
    return sea_int_val(port);
}

static SeaValue net_peer(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int fd;
    if (!arg_fd(argc, args, &fd)) return SEA_NIL_VAL;
    struct sockaddr_storage sa; socklen_t sl = sizeof sa;
    if (getpeername(fd, (struct sockaddr*)&sa, &sl) < 0) return SEA_NIL_VAL;
    char ip[INET6_ADDRSTRLEN] = "?", out[INET6_ADDRSTRLEN + 16];
    int port = 0;
    if (sa.ss_family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6*)&sa;
        if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) {
            struct in_addr v4;
            memcpy(&v4, &a->sin6_addr.s6_addr[12], 4);
            inet_ntop(AF_INET, &v4, ip, sizeof ip);
            snprintf(out, sizeof out, "%s:%d", ip, ntohs(a->sin6_port));
        } else {
            inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
            snprintf(out, sizeof out, "[%s]:%d", ip, ntohs(a->sin6_port));
        }
        (void)port;
    } else {
        struct sockaddr_in *a = (struct sockaddr_in*)&sa;
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
        snprintf(out, sizeof out, "%s:%d", ip, ntohs(a->sin_port));
    }
    return sea_obj_val(&sea_string_from_cstr(out)->obj);
}

void sea_lib_net_register(SeaVM *vm) {
    signal(SIGPIPE, SIG_IGN);   /* a peer that went away must not kill us */
    SeaTable *mod = sea_table_new(TABLE_MAP);
    mod_set(vm, mod, "connect",    net_connect);
    mod_set(vm, mod, "listen",     net_listen);
    mod_set(vm, mod, "accept",     net_accept);
    mod_set(vm, mod, "read",       net_read);
    mod_set(vm, mod, "write",      net_write);
    mod_set(vm, mod, "close",      net_close);
    mod_set(vm, mod, "shutdown",   net_shutdown);
    mod_set(vm, mod, "local_port", net_local_port);
    mod_set(vm, mod, "peer",       net_peer);
    sea_vm_register_module(vm, "net", mod);
    sea_obj_release(&mod->obj);
}
