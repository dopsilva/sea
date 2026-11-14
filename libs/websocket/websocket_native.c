/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * libs/websocket/websocket_native.c — WebSocket frames (RFC 6455) for
 * websocket.sea: encoding with optional masking, decoding with the
 * protocol checks, and random bytes for keys and masks.
 *
 *   native.encode(opcode, payload, fin, mask) → bytes
 *   native.decode(buf, max_payload, want_mask) → nil (incomplete)
 *       | {fin, opcode, data, size}                      (size: bytes used)
 *       | {close = code, reason = text}                  (a protocol violation)
 *   native.random(n) → n random bytes
 *   native.utf8_tail(s) → -1 if s is not UTF-8, else how many bytes at its end
 *       start a character that is not complete yet (0-3)
 */

#include "sea.h"
#include "sea_rope.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "sea_compat.h"

static void tset(SeaTable *t, const char *k, SeaValue v) {
    sea_table_set_str(t, k, v);
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

static void fill_random(unsigned char *p, size_t n) {
    if (sea_sys_random(p, n)) return;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) abort();
    while (fd >= 0 && n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) continue;
        p += r;
        n -= (size_t)r;
    }
    if (fd >= 0) close(fd);
}

static SeaValue fn_random(SeaVM *vm, int argc, SeaValue *args) {
    int64_t n = argc > 0 && sea_is_int(args[0]) ? sea_as_int(args[0]) : 16;
    if (n < 0 || n > 4096) sea_vm_error_noreturn(vm, "websocket.random: 0 to 4096 bytes");
    SeaString *s = sea_string_alloc((size_t)n);
    fill_random((unsigned char *)s->data, (size_t)n);
    s->data[n] = '\0';
    return sea_obj_val(&s->obj);
}

/* encode(opcode, payload, fin, mask) */
static SeaValue fn_encode(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !sea_is_int(args[0])) sea_vm_error_noreturn(vm, "websocket.encode: opcode and payload expected");
    int op = (int)sea_as_int(args[0]);
    SeaString *p = sea_value_get_flat_string(args[1]);
    if (!p) sea_vm_error_noreturn(vm, "websocket.encode: payload must be a string");
    bool fin = argc < 3 || !sea_is_bool(args[2]) || sea_as_bool(args[2]);
    bool mask = argc > 3 && sea_is_bool(args[3]) && sea_as_bool(args[3]);
    size_t n = p->len;
    size_t hdr = 2 + (n > 65535 ? 8 : n > 125 ? 2 : 0) + (mask ? 4 : 0);
    SeaString *out = sea_string_alloc(hdr + n);
    unsigned char *w = (unsigned char *)out->data;
    w[0] = (unsigned char)((fin ? 0x80 : 0) | (op & 0x0F));
    size_t i = 2;
    if (n > 65535) {
        w[1] = 127;
        for (int k = 7; k >= 0; k--) w[i++] = (unsigned char)((uint64_t)n >> (8 * k));
    } else if (n > 125) {
        w[1] = 126;
        w[i++] = (unsigned char)(n >> 8);
        w[i++] = (unsigned char)n;
    } else {
        w[1] = (unsigned char)n;
    }
    if (mask) {
        w[1] |= 0x80;
        unsigned char key[4];
        fill_random(key, 4);
        memcpy(w + i, key, 4);
        i += 4;
        const unsigned char *src = (const unsigned char *)p->data;
        for (size_t k = 0; k < n; k++) w[i + k] = src[k] ^ key[k & 3];
    } else {
        memcpy(w + i, p->data, n);
    }
    out->data[hdr + n] = '\0';
    sea_obj_release(&p->obj);
    return sea_obj_val(&out->obj);
}

static SeaValue violation(int code, const char *reason) {
    SeaTable *t = sea_table_new(TABLE_MAP);
    tset(t, "close", sea_int_val(code));
    tset(t, "reason", sea_obj_val(&sea_string_new(reason, strlen(reason))->obj));
    return sea_obj_val(&t->obj);
}

/* decode(buf, max_payload, want_mask): one frame from the front of buf */
static SeaValue fn_decode(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *b = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!b) sea_vm_error_noreturn(vm, "websocket.decode: buffer expected");
    int64_t max = argc > 1 && sea_is_int(args[1]) ? sea_as_int(args[1]) : (INT64_C(1) << 24);
    int want_mask = argc > 2 && sea_is_bool(args[2]) ? sea_as_bool(args[2]) : -1;   /* -1: either */
    const unsigned char *s = (const unsigned char *)b->data;
    size_t n = b->len;
    SeaValue r = SEA_NIL_VAL;
    if (n < 2) goto done;
    bool fin = s[0] & 0x80;
    int rsv = s[0] & 0x70, op = s[0] & 0x0F;
    bool masked = s[1] & 0x80;
    uint64_t len = s[1] & 0x7F;
    size_t i = 2;
    if (rsv) { r = violation(1002, "reserved bits set (no extension was negotiated)"); goto done; }
    if ((op > 2 && op < 8) || op > 10) { r = violation(1002, "unknown opcode"); goto done; }
    if (op >= 8 && (!fin || len > 125)) { r = violation(1002, "control frames must be whole and at most 125 bytes"); goto done; }
    if (want_mask == 1 && !masked) { r = violation(1002, "client frames must be masked"); goto done; }
    if (want_mask == 0 && masked) { r = violation(1002, "server frames must not be masked"); goto done; }
    if (len == 126) {
        if (n < 4) goto done;
        len = (uint64_t)s[2] << 8 | s[3];
        i = 4;
    } else if (len == 127) {
        if (n < 10) goto done;
        len = 0;
        for (int k = 0; k < 8; k++) len = len << 8 | s[2 + k];
        i = 10;
        if (len >> 63) { r = violation(1002, "invalid frame length"); goto done; }
    }
    if (len > (uint64_t)max) { r = violation(1009, "message too big"); goto done; }
    unsigned char key[4] = { 0, 0, 0, 0 };
    if (masked) {
        if (n < i + 4) goto done;
        memcpy(key, s + i, 4);
        i += 4;
    }
    if (n - i < len) goto done;
    SeaString *data = sea_string_alloc((size_t)len);
    unsigned char *d = (unsigned char *)data->data;
    if (masked) for (uint64_t k = 0; k < len; k++) d[k] = s[i + k] ^ key[k & 3];
    else memcpy(d, s + i, (size_t)len);
    data->data[len] = '\0';
    SeaTable *t = sea_table_new(TABLE_MAP);
    tset(t, "fin", sea_bool_val(fin));
    tset(t, "opcode", sea_int_val(op));
    tset(t, "data", sea_obj_val(&data->obj));
    tset(t, "size", sea_int_val((int64_t)(i + len)));
    r = sea_obj_val(&t->obj);
done:
    sea_obj_release(&b->obj);
    return r;
}

/* Can s[0..n) start a valid UTF-8 sequence (a prefix of one)? */
static bool utf8_prefix(const unsigned char *s, size_t n) {
    unsigned char c = s[0];
    size_t need;
    if (c >= 0xC2 && c <= 0xDF) need = 2;
    else if (c >= 0xE0 && c <= 0xEF) need = 3;
    else if (c >= 0xF0 && c <= 0xF4) need = 4;
    else return false;
    if (n >= need) return false;
    for (size_t i = 1; i < n; i++) {
        unsigned char lo = 0x80, hi = 0xBF;
        if (i == 1) {
            if (c == 0xE0) lo = 0xA0;
            else if (c == 0xED) hi = 0x9F;
            else if (c == 0xF0) lo = 0x90;
            else if (c == 0xF4) hi = 0x8F;
        }
        if (s[i] < lo || s[i] > hi) return false;
    }
    return true;
}

/* utf8_tail(s): for checking a text message fragment by fragment */
static SeaValue fn_utf8_tail(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) sea_vm_error_noreturn(vm, "websocket.utf8_tail: string expected");
    size_t bad = sea_utf8_check(s->data, s->len);
    int64_t r = 0;
    if (bad < s->len) r = (s->len - bad < 4 && utf8_prefix((const unsigned char *)s->data + bad, s->len - bad))
                          ? (int64_t)(s->len - bad) : -1;
    sea_obj_release(&s->obj);
    return sea_int_val(r);
}

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "encode", fn_encode);
    sea_mod_set(mod, "decode", fn_decode);
    sea_mod_set(mod, "random", fn_random);
    sea_mod_set(mod, "utf8_tail", fn_utf8_tail);
    return sea_obj_val(&mod->obj);
}
