/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* sea_bundle.c — programs bundled by `sea build` (see sea_bundle.h) */
#include "sea_bundle.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include "sea_compat.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static SeaBundleEntry *entries = NULL;
static int n_entries = 0;

bool sea_bundle_open(void) {
    int fd = sea_exe_open();
    if (fd < 0) return false;
    struct stat st;
    char trailer[28];
    if (fstat(fd, &st) != 0 || st.st_size < 28 ||
        pread(fd, trailer, 28, st.st_size - 28) != 28 ||
        memcmp(trailer + 20, SEA_BUNDLE_MAGIC, 8) != 0) {
        close(fd);
        return false;
    }
    char digits[21];
    memcpy(digits, trailer, 20);
    digits[20] = '\0';
    long long off = strtoll(digits, NULL, 10);
    long long size = (long long)st.st_size - 28 - off;
    if (off <= 0 || size <= 0) { close(fd); return false; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { close(fd); return false; }
    long long got = 0;
    while (got < size) {
        ssize_t r = pread(fd, buf + got, (size_t)(size - got), off + got);
        if (r <= 0) break;
        got += r;
    }
    close(fd);
    if (got != size) { free(buf); return false; }
    buf[size] = '\0';

    /* Parse the index. Each entry's data is copied out so that source can be
     * NUL-terminated; the buffer is then freed. */
    int cap = 16;
    entries = malloc(sizeof *entries * (size_t)cap);
    char *p = buf, *end = buf + size;
    while (p < end && *p != 'E') {
        char kind = *p;
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl || (kind != 'S' && kind != 'N') || p[1] != ' ') goto bad;
        char *q = p + 2;
        unsigned long long n = strtoull(q, &q, 10);
        if (*q != ' ' || q + 1 >= nl || n > (unsigned long long)(end - nl - 1)) goto bad;
        if (n_entries == cap) {
            cap *= 2;
            entries = realloc(entries, sizeof *entries * (size_t)cap);
        }
        SeaBundleEntry *e = &entries[n_entries++];
        e->kind = kind;
        e->name = strndup(q + 1, (size_t)(nl - q - 1));
        char *data = malloc((size_t)n + 1);
        memcpy(data, nl + 1, (size_t)n);
        data[n] = '\0';
        e->data = data;
        e->size = (size_t)n;
        p = nl + 1 + n;
    }
    free(buf);
    if (n_entries == 0 || entries[0].kind != 'S') goto bad_freed;
    return true;
bad:
    free(buf);
bad_freed:
    fprintf(stderr, "sea: this program's bundle is damaged\n");
    exit(1);
}

const SeaBundleEntry *sea_bundle_main(void) {
    return n_entries > 0 ? &entries[0] : NULL;
}

/* Normalize a relative path in place: drop "." and empty elements, resolve
 * ".." against the element before it (kept at the start). */
static void normalize(char *s) {
    char *parts[256];
    int n = 0;
    for (char *tok = strtok(s, "/"); tok; tok = strtok(NULL, "/")) {
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0 && n > 0 && strcmp(parts[n - 1], "..") != 0) { n--; continue; }
        if (n < 256) parts[n++] = tok;
    }
    char out[4096];
    size_t w = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        int k = snprintf(out + w, sizeof out - w, "%s%s", i ? "/" : "", parts[i]);
        if (k < 0 || (size_t)k >= sizeof out - w) break;
        w += (size_t)k;
    }
    strcpy(s, out);
}

static const SeaBundleEntry *lookup(const char *name) {
    for (int i = 0; i < n_entries; i++)
        if (strcmp(entries[i].name, name) == 0) return &entries[i];
    return NULL;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && memcmp(s + a - b, suffix, b) == 0 && (a == b || s[a - b - 1] == '/');
}

const SeaBundleEntry *sea_bundle_find(const char *from, const char *name) {
    if (n_entries == 0 || name[0] == '/') return NULL;
    static const char *const EXT[] = { ".sea", "/init.sea", ".so" };
    char base[4096], cand[4200];
    const char *slash = from ? strrchr(from, '/') : NULL;
    if (slash) snprintf(base, sizeof base, "%.*s/%s", (int)(slash - from), from, name);
    else snprintf(base, sizeof base, "%s", name);
    normalize(base);
    for (int i = 0; i < 3; i++) {
        snprintf(cand, sizeof cand, "%s%s", base, EXT[i]);
        const SeaBundleEntry *e = lookup(cand);
        if (e) return e;
    }
    snprintf(base, sizeof base, "%s", name);
    normalize(base);
    const char *tail = base;   /* without leading ../ (they led out of a directory of the build) */
    while (strncmp(tail, "../", 3) == 0) tail += 3;
    if (strcmp(tail, "..") == 0 || !*tail) return NULL;
    for (int i = 0; i < 3; i++) {
        snprintf(cand, sizeof cand, "%s%s", tail, EXT[i]);
        for (int j = 0; j < n_entries; j++)
            if (ends_with(entries[j].name, cand)) return &entries[j];
    }
    /* a package's name/name.sea (sea_modules/name/name.sea) */
    const char *last = strrchr(tail, '/');
    if ((size_t)snprintf(cand, sizeof cand, "%s/%s.sea", tail, last ? last + 1 : tail) >= sizeof cand) return NULL;
    for (int j = 0; j < n_entries; j++)
        if (ends_with(entries[j].name, cand)) return &entries[j];
    return NULL;
}
