/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

/*
 * The few places where Linux and the other POSIX systems (macOS, the BSDs)
 * differ. Linux keeps its fast paths (eventfd, accept4, pipe2, pidfd, memfd);
 * everywhere else the same behaviour is built from portable calls, so the rest
 * of the runtime does not need #ifdefs.
 *
 * "Multiplexer" (sea_mux_*) is the readiness API behind the netpoller and
 * process.run: epoll on Linux, kqueue elsewhere. Interest is one-shot or
 * level-triggered; the multiplexer's own fd can itself be waited on (it
 * becomes readable when an event is pending), on both systems.
 */

/* SEA_FORCE_PORTABLE builds the non-Linux code paths on Linux (a syntax check
 * of them with a stub <sys/event.h>; they cannot run there). */
#if defined(__linux__) && !defined(SEA_FORCE_PORTABLE)
#  define SEA_LINUX 1
#endif

#ifdef SEA_LINUX
#  define SEA_MUX_EPOLL 1
#else
#  define SEA_MUX_KQUEUE 1
#endif

/* send() flag that suppresses SIGPIPE (0 where the socket option does it). */
#ifdef MSG_NOSIGNAL
#  define SEA_MSG_NOSIGNAL MSG_NOSIGNAL
#else
#  define SEA_MSG_NOSIGNAL 0
#endif

/* Last-modification/access/change time of a struct stat. */
#if defined(__APPLE__)
#  define SEA_ST_MTIM(st) ((st).st_mtimespec)
#  define SEA_ST_ATIM(st) ((st).st_atimespec)
#  define SEA_ST_CTIM(st) ((st).st_ctimespec)
#else
#  define SEA_ST_MTIM(st) ((st).st_mtim)
#  define SEA_ST_ATIM(st) ((st).st_atim)
#  define SEA_ST_CTIM(st) ((st).st_ctim)
#endif

/* pipe with both ends close-on-exec. 0 or -1 (errno). */
int  sea_pipe_cloexec(int p[2]);

/* A socket / accepted connection that is non-blocking, close-on-exec and
 * never raises SIGPIPE. -1 on error (errno). */
int  sea_socket_nb(int domain, int type, int protocol);
int  sea_accept_nb(int listen_fd);

/* A wake-up channel: wait for readability of *rd (non-blocking), signal with
 * sea_wake_post(*wr), clear with sea_wake_drain(*rd). On Linux *rd == *wr
 * (an eventfd); elsewhere it is a pipe. sea_wake_close closes what is not
 * closed already (rd is closed with sea_netpoll_close when it was waited on). */
int  sea_wake_open(int *rd, int *wr);
void sea_wake_post(int wr);
void sea_wake_drain(int rd);
void sea_wake_close_wr(int rd, int wr);   /* the write end only (no-op when rd == wr) */

/* Cryptographic random bytes; false if the system has none to give. */
bool sea_sys_random(unsigned char *buf, size_t n);

/* Absolute path of the running executable into out; length, or -1. */
long sea_exe_path(char *out, size_t cap);
/* Open the running executable read-only (close-on-exec); fd or -1. */
int  sea_exe_open(void);

/* A file descriptor holding `size` bytes of data that can be dlopen()ed:
 * writes them to an anonymous file and puts a path that names it in path.
 * Returns the fd (keep it open), or -1. *tmp is set to true when `path` is a
 * temporary file on disk that the caller should unlink after dlopen. */
int  sea_mem_library(const char *name, const void *data, size_t size, char *path, size_t pathcap, bool *tmp);

/* ---- multiplexer ---- */
#define SEA_MUX_READ  1
#define SEA_MUX_WRITE 2

int  sea_mux_create(void);                          /* -1 on error */
/* Watch fd for readiness, level-triggered (persistent) when `oneshot` is
 * false, else until it fires once. Re-adding an fd replaces its interest. */
bool sea_mux_add(int mux, int fd, int events, bool oneshot);
void sea_mux_del(int mux, int fd);                  /* forget fd (no error if unknown) */
/* Wait up to timeout_ms (-1: forever); stores the ready fds in out[0..max)
 * and returns how many, or -1 on EINTR/error. */
int  sea_mux_wait(int mux, int *out, int max, int timeout_ms);

/* Watch a child's exit through the multiplexer. Returns a handle >= 0 to pass
 * to sea_mux_del_pid, or -1 when the child cannot be watched (already gone,
 * or unsupported): the caller then waits in waitpid. */
int  sea_mux_add_pid(int mux, pid_t pid);
void sea_mux_del_pid(int mux, int handle, pid_t pid);
/* An fd that becomes readable when the child exits (for sea_io_wait), or -1.
 * Close it with sea_netpoll_close. */
int  sea_pid_waitfd(pid_t pid);
