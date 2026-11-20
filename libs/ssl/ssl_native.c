/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * libs/ssl/ssl_native.c — Sea SSL/TLS + Cryptography library (OpenSSL backend).
 *
 * Build:  cd libs/ssl && make
 *
 * Groups:
 *   TLS transport  — tls_client, tls_server, tls_handshake, tls_recv,
 *                    tls_send, tls_close (on sockets from the `net` module,
 *                    non-blocking through the netpoller), tls_peer_cert,
 *                    tls_version, tls_cipher
 *   TLS server ctx — server_new, server_free
 *   Certificates   — cert_load_file, cert_parse, cert_info, cert_pem, pkcs12_parse,
 *                    cert_save, cert_free, cert_verify, cert_pubkey, cert_self_signed,
 *                    cert_sign
 *   Keys           — key_generate, key_load_file, key_parse, key_pem,
 *                    key_pub_pem, key_save, key_free
 *   CSR            — csr_new, csr_pem, csr_free
 *   Crypto         — hash, hash_raw, hmac, hmac_raw, aes_encrypt, aes_decrypt,
 *                    sign, verify, rand_bytes, pbkdf2, hkdf,
 *                    base64_encode, base64_decode, hex_encode, hex_decode
 */

#include "sea.h"
#include "sea_rope.h"
#include "sea_struct.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pkcs12.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- Pointer packing ---------------------------------------------------- */

#define PACK_PTR(p)    sea_int_val((int64_t)(uintptr_t)(p))
#define UNPACK(v, T)   ((T)(uintptr_t)(uint64_t)sea_as_int(v))

/* ---- Dynamic buffer ----------------------------------------------------- */

typedef struct { char *data; size_t len; size_t cap; } Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void buf_free(Buf *b) { free(b->data); buf_init(b); }

static int buf_push(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char *p = realloc(b->data, nc);
        if (!p) return 0;
        b->data = p; b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

/* ---- String / value helpers --------------------------------------------- */

/* Copy SeaValue string into a C buffer. Returns 1 on success, 0 on failure. */
static int val_to_cstr(SeaValue v, char *buf, size_t bufsz) {
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return 0;
    size_t len = s->len < bufsz - 1 ? s->len : bufsz - 1;
    memcpy(buf, s->data, len);
    buf[len] = '\0';
    sea_obj_release(&s->obj);
    return 1;
}

/* Create a SeaValue string (RC=1) for direct return. */
static SeaValue make_str(const char *data, size_t len) {
    SeaString *s = sea_string_new(data, len);
    return sea_obj_val(&s->obj);
}

/* Set a string table field, correctly managing RC. */
static void tbl_set_str(SeaTable *t, const char *key, const char *data, size_t len) {
    SeaString *s = sea_string_new(data, len);
    sea_table_set_str(t, key, sea_obj_val(&s->obj));
    sea_obj_release(&s->obj);
}

/* Push a C string to an array, correctly managing RC. */
static void arr_push_str(SeaArray *arr, const char *data, size_t len) {
    SeaString *s = sea_string_new(data, len);
    sea_array_push(arr, sea_obj_val(&s->obj));
    sea_obj_release(&s->obj);
}

/* ---- Options table lookup (handles both TABLE_MAP and OBJ_STRUCT) ------- */

static bool opts_get(SeaValue opts, const char *field, SeaValue *out) {
    if (sea_is_table(opts)) {
        return sea_table_get_str(SEA_AS_TABLE(opts), field, out);
    }
    if (sea_is_map(opts)) {                  /* [=] maps */
        SeaString *k = sea_string_intern(field, strlen(field));
        bool found = sea_map_get(SEA_AS_MAP(opts), sea_obj_val(&k->obj), out);
        sea_obj_release(&k->obj);
        return found && !sea_is_nil(*out);
    }
    if (sea_is_struct(opts)) {
        SeaString *k = sea_string_intern(field, strlen(field));
        *out = sea_struct_get(SEA_AS_STRUCT(opts), k);
        sea_obj_release(&k->obj);
        return !sea_is_nil(*out);
    }
    return false;
}

/* ---- Hex helpers --------------------------------------------------------- */

static void bin_to_hex(const unsigned char *in, size_t n, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i*2]   = hx[in[i] >> 4];
        out[i*2+1] = hx[in[i] & 0xf];
    }
    out[n*2] = '\0';
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---- Algorithm lookup --------------------------------------------------- */

static const EVP_MD *lookup_md(const char *algo) {
    if (!strcmp(algo, "sha256")    || !strcmp(algo, "SHA256"))    return EVP_sha256();
    if (!strcmp(algo, "sha384")    || !strcmp(algo, "SHA384"))    return EVP_sha384();
    if (!strcmp(algo, "sha512")    || !strcmp(algo, "SHA512"))    return EVP_sha512();
    if (!strcmp(algo, "sha1")      || !strcmp(algo, "SHA1"))      return EVP_sha1();
    if (!strcmp(algo, "md5")       || !strcmp(algo, "MD5"))       return EVP_md5();
    if (!strcmp(algo, "sha3-256")  || !strcmp(algo, "SHA3-256"))  return EVP_sha3_256();
    if (!strcmp(algo, "sha3-512")  || !strcmp(algo, "SHA3-512"))  return EVP_sha3_512();
    return NULL;
}

static const EVP_CIPHER *lookup_cipher(const char *algo) {
    if (!strcmp(algo, "aes-128-gcm"))       return EVP_aes_128_gcm();
    if (!strcmp(algo, "aes-256-gcm"))       return EVP_aes_256_gcm();
    if (!strcmp(algo, "aes-128-cbc"))       return EVP_aes_128_cbc();
    if (!strcmp(algo, "aes-256-cbc"))       return EVP_aes_256_cbc();
    if (!strcmp(algo, "chacha20-poly1305")) return EVP_chacha20_poly1305();
    return NULL;
}

/* Strip "rsa-", "rsa-pss-", "ecdsa-" prefix to find the hash algo. */
static const EVP_MD *lookup_sign_md(const char *algo) {
    const char *p = strrchr(algo, '-');
    return lookup_md(p ? p + 1 : algo);
}

/* ---- Global client SSL_CTX (lazy-initialized) --------------------------- */

static SSL_CTX        *g_client_ctx  = NULL;
static pthread_once_t  g_client_once = PTHREAD_ONCE_INIT;

static void client_ctx_init(void) {
    g_client_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_client_ctx) return;
    SSL_CTX_set_verify(g_client_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(g_client_ctx);
    SSL_CTX_set_min_proto_version(g_client_ctx, TLS1_2_VERSION);
    /* Every connection of every thread shares these CA certificates, and
     * OpenSSL fills each one's extension cache (key identifiers...) the first
     * time a handshake uses it, while another handshake may be reading it
     * (ThreadSanitizer saw it). Fill them all now, before any sharing. */
    STACK_OF(X509_OBJECT) *objs = X509_STORE_get0_objects(SSL_CTX_get_cert_store(g_client_ctx));
    for (int i = 0; objs && i < sk_X509_OBJECT_num(objs); i++) {
        X509 *x = X509_OBJECT_get0_X509(sk_X509_OBJECT_value(objs, i));
        if (x) X509_check_purpose(x, -1, 0);
    }
}

static SSL_CTX *get_client_ctx(void) {
    pthread_once(&g_client_once, client_ctx_init);
    return g_client_ctx;
}

/* =========================================================================
 * TLS TRANSPORT — over non-blocking sockets from the `net` module.
 *
 * Every function that may wait goes through the netpoller (sea_netpoll.h):
 * when OpenSSL wants to read or write the socket, the goroutine parks and
 * the same native runs again once the socket is ready (OpenSSL keeps the
 * handshake/record state in the SSL object between attempts). tls_send keeps
 * the bytes already written in the VM's I/O call. Failures return error
 * values; a timeout's message is "timeout".
 * ========================================================================= */

static int64_t opt_timeout(int argc, SeaValue *args, int i) {
    if (argc <= i) return -1;
    if (sea_is_int(args[i])) return sea_as_int(args[i]) > 0 ? sea_as_int(args[i]) : -1;
    if (sea_is_number(args[i])) return sea_as_number(args[i]) > 0 ? (int64_t)sea_as_number(args[i]) : -1;
    return -1;
}

static SeaValue tls_error(SeaVM *vm, const char *what) {
    char buf[256];
    unsigned long e = ERR_get_error();
    if (e) {
        char es[160];
        ERR_error_string_n(e, es, sizeof es);
        snprintf(buf, sizeof buf, "%s: %s", what, es);
    } else {
        snprintf(buf, sizeof buf, "%s", what);
    }
    ERR_clear_error();
    return sea_make_error_str(vm, buf);
}

/* After an SSL call returned `r`: wait for the socket if OpenSSL asked to.
 * Returns 1 to retry, 0 if the caller should return *out. */
static int tls_wait(SeaVM *vm, SSL *ssl, int r, const char *what, SeaValue *out) {
    int err = SSL_get_error(ssl, r);
    int ev = err == SSL_ERROR_WANT_READ ? SEA_IO_READ : err == SSL_ERROR_WANT_WRITE ? SEA_IO_WRITE : 0;
    if (!ev) {
        sea_io_end(vm);
        *out = tls_error(vm, what);
        return 0;
    }
    int w = sea_io_wait(vm, SSL_get_fd(ssl), ev);
    if (w == SEA_IO_PARK) { *out = SEA_PARK_VAL; return 0; }
    if (w == SEA_IO_READY) return 1;
    sea_io_end(vm);
    ERR_clear_error();
    *out = sea_make_error_str(vm, w == SEA_IO_TIMEOUT ? "timeout" : "connection closed");
    return 0;
}

/* A cert or key option: a PEM file's path, or the PEM itself (it starts
 * with "-----BEGIN": a certificate kept in a database, never on disk). A
 * certificate's PEM may carry its chain after it. */
static bool is_pem(SeaString *s) {
    return s->len > 10 && !memcmp(s->data, "-----BEGIN", 10);
}

static int ctx_use_cert(SSL_CTX *ctx, SeaValue v) {
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return 0;
    int ok = 0;
    if (is_pem(s)) {
        BIO  *b = BIO_new_mem_buf(s->data, (int)s->len);
        X509 *x = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
        ok = x && SSL_CTX_use_certificate(ctx, x) == 1;
        X509_free(x);
        X509 *extra;
        while (ok && (extra = PEM_read_bio_X509(b, NULL, NULL, NULL)) != NULL) {
            if (SSL_CTX_add_extra_chain_cert(ctx, extra) != 1) {   /* takes it on success */
                X509_free(extra);
                ok = 0;
            }
        }
        if (ok) ERR_clear_error();   /* the end of the PEM */
        BIO_free(b);
    } else {
        char p[1024];
        size_t n = s->len < sizeof(p) - 1 ? s->len : sizeof(p) - 1;
        memcpy(p, s->data, n);
        p[n] = '\0';
        ok = SSL_CTX_use_certificate_chain_file(ctx, p) == 1;
    }
    sea_obj_release(&s->obj);
    return ok;
}

static int ctx_use_key(SSL_CTX *ctx, SeaValue v) {
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return 0;
    int ok = 0;
    if (is_pem(s)) {
        BIO      *b = BIO_new_mem_buf(s->data, (int)s->len);
        EVP_PKEY *k = b ? PEM_read_bio_PrivateKey(b, NULL, NULL, NULL) : NULL;
        ok = k && SSL_CTX_use_PrivateKey(ctx, k) == 1;
        EVP_PKEY_free(k);
        BIO_free(b);
    } else {
        char p[1024];
        size_t n = s->len < sizeof(p) - 1 ? s->len : sizeof(p) - 1;
        memcpy(p, s->data, n);
        p[n] = '\0';
        ok = SSL_CTX_use_PrivateKey_file(ctx, p, SSL_FILETYPE_PEM) == 1;
    }
    sea_obj_release(&s->obj);
    return ok;
}

static SSL *tls_new(SSL_CTX *ctx, int fd) {
    SSL *ssl = SSL_new(ctx);
    if (!ssl) return NULL;
    SSL_set_fd(ssl, fd);
    /* tls_send retries with the same (possibly moved) buffer and accepts
     * partial writes */
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return ssl;
}

/* tls_client(fd, host [, opts]) → conn: client TLS on a connected socket
 * (no I/O yet; call tls_handshake). opts: verify, verify_host, ca, sni,
 * cert, key (a path or the PEM), min_version. On failure the fd is left open
 * (the caller owns it). */
static SeaValue fn_tls_client(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !sea_is_int(args[0])) return sea_make_error_str(vm, "tls_client: fd and host expected");
    int fd = (int)sea_as_int(args[0]);
    char host[256] = {0};
    if (!val_to_cstr(args[1], host, sizeof(host))) return sea_make_error_str(vm, "tls_client: bad host");

    bool verify      = true;
    bool verify_host = true;
    char ca_p[512]   = {0};
    char sni[256]    = {0};
    SeaValue cert_v  = SEA_NIL_VAL;   /* borrowed from the options */
    SeaValue key_v   = SEA_NIL_VAL;
    long min_ver     = TLS1_2_VERSION;

    if (argc >= 3 && (sea_is_table(args[2]) || sea_is_struct(args[2]) || sea_is_map(args[2]))) {
        SeaValue v;
        if (opts_get(args[2], "verify",      &v)) verify = sea_is_truthy(v);
        if (opts_get(args[2], "verify_host", &v)) verify_host = sea_is_truthy(v);
        if (opts_get(args[2], "ca",          &v)) val_to_cstr(v, ca_p, sizeof(ca_p));
        if (opts_get(args[2], "sni",         &v)) val_to_cstr(v, sni,    sizeof(sni));
        if (opts_get(args[2], "cert",        &v) && !sea_is_nil(v)) cert_v = v;
        if (opts_get(args[2], "key",         &v) && !sea_is_nil(v)) key_v = v;
        if (opts_get(args[2], "min_version", &v)) {
            char mv[32]; val_to_cstr(v, mv, sizeof(mv));
            if (!strcmp(mv, "TLSv1.3") || !strcmp(mv, "1.3")) min_ver = TLS1_3_VERSION;
        }
    }

    bool    has_cert    = !sea_is_nil(cert_v);
    bool    has_key     = !sea_is_nil(key_v);
    bool    needs_clone = !verify || ca_p[0] || has_cert || has_key || min_ver != TLS1_2_VERSION;
    SSL_CTX *ctx        = needs_clone ? NULL : get_client_ctx();
    if (needs_clone) {
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) return tls_error(vm, "tls_client");
        SSL_CTX_set_min_proto_version(ctx, min_ver);
        if (!verify)
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        else {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
            if (ca_p[0]) {
                if (SSL_CTX_load_verify_locations(ctx, ca_p, NULL) != 1) {
                    SSL_CTX_free(ctx);
                    return tls_error(vm, "tls_client: ca");
                }
            } else
                SSL_CTX_set_default_verify_paths(ctx);
        }
        if (has_cert && !ctx_use_cert(ctx, cert_v)) {
            SSL_CTX_free(ctx);
            return tls_error(vm, "tls_client: cert");
        }
        if (has_key && !ctx_use_key(ctx, key_v)) {
            SSL_CTX_free(ctx);
            return tls_error(vm, "tls_client: key");
        }
    }
    if (!ctx) return tls_error(vm, "tls_client");

    SSL *ssl = tls_new(ctx, fd);
    if (needs_clone) SSL_CTX_free(ctx);   /* SSL_new holds its own reference */
    if (!ssl) return tls_error(vm, "tls_client");

    const char *sni_name = sni[0] ? sni : host;
    SSL_set_tlsext_host_name(ssl, sni_name);
    if (verify && verify_host) SSL_set1_host(ssl, sni_name);
    SSL_set_connect_state(ssl);
    return PACK_PTR(ssl);
}

/* tls_server(ctx, fd) → conn: server TLS on an accepted socket (no I/O). */
static SeaValue fn_tls_server(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !sea_is_int(args[0]) || !sea_is_int(args[1]))
        return sea_make_error_str(vm, "tls_server: ctx and fd expected");
    SSL *ssl = tls_new(UNPACK(args[0], SSL_CTX*), (int)sea_as_int(args[1]));
    if (!ssl) return tls_error(vm, "tls_server");
    SSL_set_accept_state(ssl);
    return PACK_PTR(ssl);
}

/* tls_handshake(conn [, timeout_ms]) → true */
static SeaValue fn_tls_handshake(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_int(args[0])) return sea_make_error_str(vm, "tls_handshake: conn expected");
    SSL *ssl = UNPACK(args[0], SSL*);
    SeaIoCall *io = sea_io_begin(vm, opt_timeout(argc, args, 1));
    if (io->closed) { sea_io_end(vm); return sea_make_error_str(vm, "connection closed"); }
    for (;;) {
        ERR_clear_error();
        int r = SSL_do_handshake(ssl);
        if (r == 1) { sea_io_end(vm); return SEA_TRUE_VAL; }
        SeaValue out;
        if (!tls_wait(vm, ssl, r, "TLS handshake failed", &out)) return out;
    }
}

/* tls_recv(conn, max [, timeout_ms]) → string ("" at the end of the stream) */
static SeaValue fn_tls_recv(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_int(args[0])) return sea_make_error_str(vm, "tls_recv: conn expected");
    SSL *ssl = UNPACK(args[0], SSL*);
    int max = (argc >= 2 && sea_is_int(args[1]) && sea_as_int(args[1]) > 0) ? (int)sea_as_int(args[1]) : 65536;
    if (max > (1 << 24)) max = 1 << 24;
    SeaIoCall *io = sea_io_begin(vm, opt_timeout(argc, args, 2));
    if (io->closed) { sea_io_end(vm); return sea_make_error_str(vm, "connection closed"); }
    char stackbuf[16384];
    char *buf = max <= (int)sizeof stackbuf ? stackbuf : malloc((size_t)max);
    if (!buf) { sea_io_end(vm); return sea_make_error_str(vm, "tls_recv: out of memory"); }
    SeaValue out;
    for (;;) {
        ERR_clear_error();
        int r = SSL_read(ssl, buf, max);
        if (r > 0) { sea_io_end(vm); out = make_str(buf, (size_t)r); break; }
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_ZERO_RETURN ||
            (err == SSL_ERROR_SYSCALL && ERR_peek_error() == 0)) {   /* peer went away */
            sea_io_end(vm);
            ERR_clear_error();
            out = make_str("", 0);
            break;
        }
        if (!tls_wait(vm, ssl, r, "TLS read failed", &out)) break;
    }
    if (buf != stackbuf) free(buf);
    return out;
}

/* tls_send(conn, data [, timeout_ms]) → bytes written (all of data).
 * io->progress holds the bytes already written across parks. */
static SeaValue fn_tls_send(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !sea_is_int(args[0])) return sea_make_error_str(vm, "tls_send: conn and data expected");
    SeaString *str = sea_value_get_flat_string(args[1]);
    if (!str) return sea_make_error_str(vm, "tls_send: string expected");
    SSL *ssl = UNPACK(args[0], SSL*);
    SeaIoCall *io = sea_io_begin(vm, opt_timeout(argc, args, 2));
    SeaValue out;
    if (io->closed) {
        sea_io_end(vm);
        out = sea_make_error_str(vm, "connection closed");
    } else for (;;) {
        size_t sent = (size_t)io->progress;
        if (sent >= str->len) { sea_io_end(vm); out = sea_int_val((int64_t)str->len); break; }
        size_t chunk = str->len - sent;
        if (chunk > (1u << 30)) chunk = 1u << 30;
        ERR_clear_error();
        int r = SSL_write(ssl, str->data + sent, (int)chunk);
        if (r > 0) { io->progress += r; continue; }
        if (!tls_wait(vm, ssl, r, "TLS write failed", &out)) break;
    }
    sea_obj_release(&str->obj);
    return out;
}

/* tls_close(conn): best-effort close_notify (never waits), free, close. */
static SeaValue fn_tls_close(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    SSL *ssl = UNPACK(args[0], SSL*);
    int fd = SSL_get_fd(ssl);
    ERR_clear_error();
    SSL_shutdown(ssl);
    ERR_clear_error();
    SSL_free(ssl);
    if (fd >= 0) sea_netpoll_close(fd);
    return SEA_NIL_VAL;
}

static SeaValue fn_tls_peer_cert(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    SSL *ssl = UNPACK(args[0], SSL*);
    X509 *cert = SSL_get_peer_certificate(ssl);
    return cert ? PACK_PTR(cert) : SEA_NIL_VAL;
}

static SeaValue fn_tls_version(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    const char *v = SSL_get_version(UNPACK(args[0], SSL*));
    return make_str(v, strlen(v));
}

static SeaValue fn_tls_cipher(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    const char *c = SSL_get_cipher(UNPACK(args[0], SSL*));
    return c ? make_str(c, strlen(c)) : SEA_NIL_VAL;
}

/* =========================================================================
 * TLS SERVER
 * ========================================================================= */

static SeaValue fn_server_new(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaValue opts_v = args[0];
    if (!sea_is_table(opts_v) && !sea_is_struct(opts_v) && !sea_is_map(opts_v)) return SEA_NIL_VAL;

    char cert[512] = {0}, key[512] = {0}, ca[512] = {0};
    long min_ver = TLS1_2_VERSION;
    SeaValue v;

    if (opts_get(opts_v, "cert",        &v)) val_to_cstr(v, cert, sizeof(cert));
    if (opts_get(opts_v, "key",         &v)) val_to_cstr(v, key,  sizeof(key));
    if (opts_get(opts_v, "ca",          &v)) val_to_cstr(v, ca,   sizeof(ca));
    if (opts_get(opts_v, "min_version", &v)) {
        char mv[32]; val_to_cstr(v, mv, sizeof(mv));
        if (!strcmp(mv, "TLSv1.3") || !strcmp(mv, "1.3")) min_ver = TLS1_3_VERSION;
    }

    if (!cert[0] || !key[0]) return SEA_NIL_VAL;

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return SEA_NIL_VAL;

    SSL_CTX_set_min_proto_version(ctx, min_ver);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        return SEA_NIL_VAL;
    }

    if (ca[0]) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        SSL_CTX_load_verify_locations(ctx, ca, NULL);
    }

    return PACK_PTR(ctx);
}

static SeaValue fn_server_free(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    SSL_CTX_free(UNPACK(args[0], SSL_CTX*));
    return SEA_NIL_VAL;
}

/* =========================================================================
 * CERTIFICATES
 * ========================================================================= */

static void push_asn1_time(SeaTable *t, const char *key, const ASN1_TIME *at) {
    if (!at) return;
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) return;
    ASN1_TIME_print(bio, at);
    char buf[64]; int n = BIO_read(bio, buf, (int)sizeof(buf) - 1);
    BIO_free(bio);
    if (n > 0) { buf[n] = '\0'; tbl_set_str(t, key, buf, (size_t)n); }
}

/* Build SAN string from opts.san SeaArray and add to cert/req extensions. */
static void build_san_buf(SeaArray *arr, Buf *san) {
    for (uint32_t i = 0; i < arr->len; i++) {
        SeaString *s = sea_value_get_flat_string(arr->data[i]);
        if (!s) continue;
        if (san->len > 0) buf_push(san, ",", 1);
        /* Heuristic: 4 dotted segments ≤ 15 chars → IP, else DNS */
        int dots = 0;
        for (uint32_t j = 0; j < s->len; j++) if (s->data[j] == '.') dots++;
        if (dots == 3 && s->len <= 15)
            buf_push(san, "IP:", 3);
        else
            buf_push(san, "DNS:", 4);
        buf_push(san, s->data, s->len);
        sea_obj_release(&s->obj);
    }
}

/* Helper: read subject name fields from opts and populate X509_NAME. */
static void name_from_opts(X509_NAME *name, SeaValue opts_v) {
    static const struct { const char *field; const char *nid; } map[] = {
        {"cn",      "CN"}, {"org",     "O"},  {"country", "C"},
        {"state",   "ST"}, {"city",    "L"},  {"email",   "emailAddress"},
        {NULL, NULL}
    };
    SeaValue v;
    char buf[256];
    for (int i = 0; map[i].field; i++) {
        if (opts_get(opts_v, map[i].field, &v) && val_to_cstr(v, buf, sizeof(buf)))
            X509_NAME_add_entry_by_txt(name, map[i].nid, MBSTRING_ASC,
                                       (unsigned char*)buf, -1, -1, 0);
    }
}

static SeaValue fn_cert_load_file(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    char path[512];
    if (!val_to_cstr(args[0], path, sizeof(path))) return SEA_NIL_VAL;
    FILE *f = fopen(path, "r");
    if (!f) return SEA_NIL_VAL;
    X509 *cert = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    return cert ? PACK_PTR(cert) : SEA_NIL_VAL;
}

static SeaValue fn_cert_parse(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *pem = sea_value_get_flat_string(args[0]);
    if (!pem) return SEA_NIL_VAL;
    BIO *bio = BIO_new_mem_buf(pem->data, (int)pem->len);
    sea_obj_release(&pem->obj);
    if (!bio) return SEA_NIL_VAL;
    X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return cert ? PACK_PTR(cert) : SEA_NIL_VAL;
}

static SeaValue fn_cert_info(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    X509 *cert = UNPACK(args[0], X509*);

    SeaTable *t = sea_table_new(TABLE_MAP);

    /* subject / issuer */
    char buf[512];
    X509_NAME *subj = X509_get_subject_name(cert);
    if (subj) {
        X509_NAME_oneline(subj, buf, (int)sizeof(buf));
        tbl_set_str(t, "subject", buf, strlen(buf));
    }
    X509_NAME *iss = X509_get_issuer_name(cert);
    if (iss) {
        X509_NAME_oneline(iss, buf, (int)sizeof(buf));
        tbl_set_str(t, "issuer", buf, strlen(buf));
    }

    push_asn1_time(t, "not_before", X509_get_notBefore(cert));
    push_asn1_time(t, "not_after",  X509_get_notAfter(cert));

    /* serial */
    const ASN1_INTEGER *ser = X509_get_serialNumber(cert);
    if (ser) sea_table_set_str(t, "serial", sea_int_val(ASN1_INTEGER_get(ser)));

    /* SHA-256 fingerprint */
    unsigned char fp[32]; unsigned int fplen = sizeof(fp);
    if (X509_digest(cert, EVP_sha256(), fp, &fplen)) {
        char hex[65]; bin_to_hex(fp, fplen, hex);
        tbl_set_str(t, "fingerprint", hex, 64);
    }

    /* SAN */
    GENERAL_NAMES *sans = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
    if (sans) {
        int n = sk_GENERAL_NAME_num(sans);
        SeaArray *arr = sea_array_new((uint32_t)n);
        for (int i = 0; i < n; i++) {
            GENERAL_NAME *gn = sk_GENERAL_NAME_value(sans, i);
            char ipbuf[64] = {0};
            if (gn->type == GEN_DNS) {
                const char *dns = (const char*)ASN1_STRING_get0_data(gn->d.dNSName);
                arr_push_str(arr, dns, strlen(dns));
            } else if (gn->type == GEN_IPADD) {
                const unsigned char *ip = ASN1_STRING_get0_data(gn->d.iPAddress);
                int ilen = ASN1_STRING_length(gn->d.iPAddress);
                if (ilen == 4)
                    snprintf(ipbuf, sizeof(ipbuf), "%d.%d.%d.%d",
                             ip[0], ip[1], ip[2], ip[3]);
                else if (ilen == 16)
                    snprintf(ipbuf, sizeof(ipbuf),
                             "%02x%02x:%02x%02x:%02x%02x:%02x%02x"
                             ":%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                             ip[0],ip[1],ip[2],ip[3],ip[4],ip[5],ip[6],ip[7],
                             ip[8],ip[9],ip[10],ip[11],ip[12],ip[13],ip[14],ip[15]);
                if (ipbuf[0]) arr_push_str(arr, ipbuf, strlen(ipbuf));
            }
        }
        GENERAL_NAMES_free(sans);
        sea_table_set_str(t, "san", sea_obj_val(&arr->obj));
        sea_obj_release(&arr->obj);
    }

    return sea_obj_val(&t->obj);
}

/* drops a value this function owns (stored elsewhere, or not needed) */
static void val_drop(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

/* the PEM of a certificate or key written by fn, or nil */
static SeaValue bio_pem(int (*write)(BIO *, void *), void *obj) {
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) return SEA_NIL_VAL;
    write(bio, obj);
    char *ptr; long len = BIO_get_mem_data(bio, &ptr);
    SeaValue r = len > 0 ? make_str(ptr, (size_t)len) : SEA_NIL_VAL;
    BIO_free(bio);
    return r;
}
static int write_x509(BIO *b, void *o) { return PEM_write_bio_X509(b, (X509 *)o); }
static int write_key(BIO *b, void *o) { return PEM_write_bio_PrivateKey(b, (EVP_PKEY *)o, NULL, NULL, 0, NULL, NULL); }

/* pkcs12_parse(der, password) → {cert_pem, key_pem, chain = [pem, ...]} | error.
 * A .pfx/.p12 (an A1 certificate): the certificate, its private key and the
 * chain. Older files (RC2, 3DES: most A1 certificates in Brazil) need
 * OpenSSL 3's legacy provider, loaded on first use. */
static SeaValue fn_pkcs12_parse(SeaVM *vm, int argc, SeaValue *args) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    static int providers = 0;
    if (!providers) {
        OSSL_PROVIDER_load(NULL, "legacy");
        OSSL_PROVIDER_load(NULL, "default");
        providers = 1;
    }
#endif
    if (argc < 1) return sea_make_error_str(vm, "pkcs12: the file's bytes expected");
    SeaString *der = sea_value_get_flat_string(args[0]);
    if (!der) return sea_make_error_str(vm, "pkcs12: the file's bytes expected");
    char pass[512] = {0};
    if (argc >= 2) val_to_cstr(args[1], pass, sizeof(pass));
    BIO *bio = BIO_new_mem_buf(der->data, (int)der->len);
    sea_obj_release(&der->obj);
    if (!bio) return sea_make_error_str(vm, "pkcs12: out of memory");
    PKCS12 *p12 = d2i_PKCS12_bio(bio, NULL);
    BIO_free(bio);
    if (!p12) return sea_make_error_str(vm, "pkcs12: not a PKCS#12 (.pfx/.p12) file");
    if (!PKCS12_verify_mac(p12, pass, -1)) {
        PKCS12_free(p12);
        ERR_clear_error();
        return sea_make_error_str(vm, "pkcs12: wrong password");
    }
    EVP_PKEY *key = NULL;
    X509 *cert = NULL;
    STACK_OF(X509) *ca = NULL;
    int ok = PKCS12_parse(p12, pass, &key, &cert, &ca);
    PKCS12_free(p12);
    if (!ok || !cert || !key) {
        if (key) EVP_PKEY_free(key);
        if (cert) X509_free(cert);
        if (ca) sk_X509_pop_free(ca, X509_free);
        unsigned long e = ERR_get_error();
        char buf[256];
        snprintf(buf, sizeof(buf), "pkcs12: cannot read it (%s)", e ? ERR_reason_error_string(e) : "no certificate or key");
        ERR_clear_error();
        return sea_make_error_str(vm, buf);
    }
    SeaTable *t = sea_table_new(TABLE_MAP);
    SeaValue v = bio_pem(write_x509, cert);
    sea_table_set_str(t, "cert_pem", v);
    val_drop(v);
    v = bio_pem(write_key, key);
    sea_table_set_str(t, "key_pem", v);
    val_drop(v);
    int n = ca ? sk_X509_num(ca) : 0;
    SeaArray *chain = sea_array_new((uint32_t)n);
    for (int i = 0; i < n; i++) {
        SeaValue p = bio_pem(write_x509, sk_X509_value(ca, i));
        sea_array_push(chain, p);
        val_drop(p);
    }
    sea_table_set_str(t, "chain", sea_obj_val(&chain->obj));
    sea_obj_release(&chain->obj);
    EVP_PKEY_free(key);
    X509_free(cert);
    if (ca) sk_X509_pop_free(ca, X509_free);
    return sea_obj_val(&t->obj);
}

static SeaValue fn_cert_pem(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) return SEA_NIL_VAL;
    PEM_write_bio_X509(bio, UNPACK(args[0], X509*));
    char *ptr; long len = BIO_get_mem_data(bio, &ptr);
    SeaValue r = len > 0 ? make_str(ptr, (size_t)len) : SEA_NIL_VAL;
    BIO_free(bio);
    return r;
}

static SeaValue fn_cert_save(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_int(args[0])) return SEA_FALSE_VAL;
    char path[512];
    if (!val_to_cstr(args[1], path, sizeof(path))) return SEA_FALSE_VAL;
    FILE *f = fopen(path, "w");
    if (!f) return SEA_FALSE_VAL;
    int ok = PEM_write_X509(f, UNPACK(args[0], X509*));
    fclose(f);
    return sea_bool_val(ok == 1);
}

static SeaValue fn_cert_free(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc >= 1 && sea_is_int(args[0])) X509_free(UNPACK(args[0], X509*));
    return SEA_NIL_VAL;
}

static SeaValue fn_cert_verify(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_int(args[0]) || !sea_is_int(args[1])) return SEA_FALSE_VAL;
    X509 *cert = UNPACK(args[0], X509*);
    X509 *ca   = UNPACK(args[1], X509*);
    X509_STORE *store = X509_STORE_new();
    X509_STORE_add_cert(store, ca);
    X509_STORE_CTX *ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, cert, NULL);
    int ok = X509_verify_cert(ctx);
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    return sea_bool_val(ok == 1);
}

/* cert_pubkey(cert) → key: the certificate's public key (to verify) */
static SeaValue fn_cert_pubkey(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    EVP_PKEY *k = X509_get_pubkey(UNPACK(args[0], X509*));   /* a reference of our own */
    return k ? PACK_PTR(k) : SEA_NIL_VAL;
}

static SeaValue fn_cert_self_signed(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    SeaValue opts_v = args[1];
    if (!sea_is_table(opts_v) && !sea_is_struct(opts_v) && !sea_is_map(opts_v)) return SEA_NIL_VAL;

    EVP_PKEY *key = UNPACK(args[0], EVP_PKEY*);
    long days = 365; bool is_ca = false;
    SeaValue v;
    if (opts_get(opts_v, "days",  &v) && sea_is_int(v)) days  = (long)sea_as_int(v);
    if (opts_get(opts_v, "is_ca", &v)) is_ca = sea_is_truthy(v);

    X509 *cert = X509_new();
    if (!cert) return SEA_NIL_VAL;
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), (long)60 * 60 * 24 * days);
    X509_set_pubkey(cert, key);

    X509_NAME *name = X509_get_subject_name(cert);
    name_from_opts(name, opts_v);
    X509_set_issuer_name(cert, name);

    X509V3_CTX x3ctx;
    X509V3_set_ctx_nodb(&x3ctx);
    X509V3_set_ctx(&x3ctx, cert, cert, NULL, NULL, 0);

    const char *bc = is_ca ? "critical,CA:TRUE" : "CA:FALSE";
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &x3ctx, NID_basic_constraints, bc);
    if (ext) { X509_add_ext(cert, ext, -1); X509_EXTENSION_free(ext); }

    if (is_ca) {
        ext = X509V3_EXT_conf_nid(NULL, &x3ctx, NID_key_usage, "critical,keyCertSign,cRLSign");
        if (ext) { X509_add_ext(cert, ext, -1); X509_EXTENSION_free(ext); }
    }

    ext = X509V3_EXT_conf_nid(NULL, &x3ctx, NID_subject_key_identifier, "hash");
    if (ext) { X509_add_ext(cert, ext, -1); X509_EXTENSION_free(ext); }

    /* SAN */
    if (opts_get(opts_v, "san", &v) && sea_is_array(v)) {
        Buf san; buf_init(&san);
        build_san_buf(SEA_AS_ARRAY(v), &san);
        if (san.len > 0) {
            ext = X509V3_EXT_conf_nid(NULL, &x3ctx, NID_subject_alt_name, san.data);
            if (ext) { X509_add_ext(cert, ext, -1); X509_EXTENSION_free(ext); }
        }
        buf_free(&san);
    }

    if (X509_sign(cert, key, EVP_sha256()) <= 0) {
        X509_free(cert); return SEA_NIL_VAL;
    }
    return PACK_PTR(cert);
}

static SeaValue fn_cert_sign(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    /* cert_sign(csr, ca_key, ca_cert, opts) */
    if (argc < 4 || !sea_is_int(args[0]) || !sea_is_int(args[1]) || !sea_is_int(args[2]))
        return SEA_NIL_VAL;
    SeaValue opts_v = args[3];
    if (!sea_is_table(opts_v) && !sea_is_struct(opts_v) && !sea_is_map(opts_v)) return SEA_NIL_VAL;

    X509_REQ *csr      = UNPACK(args[0], X509_REQ*);
    EVP_PKEY *ca_key   = UNPACK(args[1], EVP_PKEY*);
    X509     *ca_cert  = UNPACK(args[2], X509*);

    long days = 365, serial = 1;
    SeaValue v;
    if (opts_get(opts_v, "days",   &v) && sea_is_int(v)) days   = (long)sea_as_int(v);
    if (opts_get(opts_v, "serial", &v) && sea_is_int(v)) serial = (long)sea_as_int(v);

    X509 *cert = X509_new();
    if (!cert) return SEA_NIL_VAL;
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), serial);
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), (long)60 * 60 * 24 * days);

    EVP_PKEY *pub = X509_REQ_get_pubkey(csr);
    X509_set_pubkey(cert, pub);
    EVP_PKEY_free(pub);

    X509_set_subject_name(cert, X509_REQ_get_subject_name(csr));
    X509_set_issuer_name(cert, X509_get_subject_name(ca_cert));

    X509V3_CTX x3ctx;
    X509V3_set_ctx_nodb(&x3ctx);
    X509V3_set_ctx(&x3ctx, ca_cert, cert, NULL, NULL, 0);

    X509_EXTENSION *ext;
    ext = X509V3_EXT_conf_nid(NULL, &x3ctx, NID_basic_constraints, "CA:FALSE");
    if (ext) { X509_add_ext(cert, ext, -1); X509_EXTENSION_free(ext); }
    ext = X509V3_EXT_conf_nid(NULL, &x3ctx, NID_subject_key_identifier, "hash");
    if (ext) { X509_add_ext(cert, ext, -1); X509_EXTENSION_free(ext); }
    ext = X509V3_EXT_conf_nid(NULL, &x3ctx, NID_authority_key_identifier, "keyid:always");
    if (ext) { X509_add_ext(cert, ext, -1); X509_EXTENSION_free(ext); }

    if (X509_sign(cert, ca_key, EVP_sha256()) <= 0) {
        X509_free(cert); return SEA_NIL_VAL;
    }
    return PACK_PTR(cert);
}

/* =========================================================================
 * KEYS
 * ========================================================================= */

static SeaValue fn_key_generate(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaValue opts_v = args[0];
    if (!sea_is_table(opts_v) && !sea_is_struct(opts_v) && !sea_is_map(opts_v)) return SEA_NIL_VAL;

    char type[16]  = "rsa";
    int  bits      = 2048;
    char curve[32] = "P-256";
    SeaValue v;
    if (opts_get(opts_v, "type",  &v)) val_to_cstr(v, type,  sizeof(type));
    if (opts_get(opts_v, "bits",  &v) && sea_is_int(v)) bits = (int)sea_as_int(v);
    if (opts_get(opts_v, "curve", &v)) val_to_cstr(v, curve, sizeof(curve));

    EVP_PKEY     *pkey = NULL;
    EVP_PKEY_CTX *ctx  = NULL;

    if (!strcmp(type, "rsa") || !strcmp(type, "RSA")) {
        ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
        if (ctx &&
            EVP_PKEY_keygen_init(ctx) > 0 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) > 0)
            EVP_PKEY_keygen(ctx, &pkey);
    } else if (!strcmp(type, "ec") || !strcmp(type, "EC")) {
        int nid = OBJ_txt2nid(curve);
        if (nid == NID_undef) {
            if      (!strcmp(curve, "P-256")) nid = NID_X9_62_prime256v1;
            else if (!strcmp(curve, "P-384")) nid = NID_secp384r1;
            else if (!strcmp(curve, "P-521")) nid = NID_secp521r1;
        }
        if (nid != NID_undef) {
            ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
            if (ctx &&
                EVP_PKEY_keygen_init(ctx) > 0 &&
                EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, nid) > 0)
                EVP_PKEY_keygen(ctx, &pkey);
        }
    } else if (!strcmp(type, "ed25519") || !strcmp(type, "x25519")) {
        ctx = EVP_PKEY_CTX_new_id(!strcmp(type, "ed25519") ? EVP_PKEY_ED25519 : EVP_PKEY_X25519, NULL);
        if (ctx && EVP_PKEY_keygen_init(ctx) > 0) EVP_PKEY_keygen(ctx, &pkey);
    }

    if (ctx) EVP_PKEY_CTX_free(ctx);
    return pkey ? PACK_PTR(pkey) : SEA_NIL_VAL;
}

static SeaValue fn_key_load_file(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    char path[512], pass[256] = {0};
    if (!val_to_cstr(args[0], path, sizeof(path))) return SEA_NIL_VAL;
    if (argc >= 2) val_to_cstr(args[1], pass, sizeof(pass));
    FILE *f = fopen(path, "r");
    if (!f) return SEA_NIL_VAL;
    EVP_PKEY *key = PEM_read_PrivateKey(f, NULL, NULL, pass[0] ? pass : NULL);
    fclose(f);
    return key ? PACK_PTR(key) : SEA_NIL_VAL;
}

static SeaValue fn_key_parse(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *pem = sea_value_get_flat_string(args[0]);
    if (!pem) return SEA_NIL_VAL;
    char pass[256] = {0};
    if (argc >= 2) val_to_cstr(args[1], pass, sizeof(pass));
    BIO *bio = BIO_new_mem_buf(pem->data, (int)pem->len);
    sea_obj_release(&pem->obj);
    if (!bio) return SEA_NIL_VAL;
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, NULL, NULL, pass[0] ? pass : NULL);
    BIO_free(bio);
    return key ? PACK_PTR(key) : SEA_NIL_VAL;
}

static SeaValue fn_key_pem(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    EVP_PKEY *key = UNPACK(args[0], EVP_PKEY*);
    char pass[256] = {0};
    if (argc >= 2) val_to_cstr(args[1], pass, sizeof(pass));
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) return SEA_NIL_VAL;
    if (pass[0])
        PEM_write_bio_PrivateKey(bio, key, EVP_aes_256_cbc(),
                                 (unsigned char*)pass, (int)strlen(pass), NULL, NULL);
    else
        PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL);
    char *ptr; long len = BIO_get_mem_data(bio, &ptr);
    SeaValue r = len > 0 ? make_str(ptr, (size_t)len) : SEA_NIL_VAL;
    BIO_free(bio);
    return r;
}

static SeaValue fn_key_pub_pem(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) return SEA_NIL_VAL;
    PEM_write_bio_PUBKEY(bio, UNPACK(args[0], EVP_PKEY*));
    char *ptr; long len = BIO_get_mem_data(bio, &ptr);
    SeaValue r = len > 0 ? make_str(ptr, (size_t)len) : SEA_NIL_VAL;
    BIO_free(bio);
    return r;
}

static SeaValue fn_key_save(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_int(args[0])) return SEA_FALSE_VAL;
    char path[512], pass[256] = {0};
    if (!val_to_cstr(args[1], path, sizeof(path))) return SEA_FALSE_VAL;
    if (argc >= 3) val_to_cstr(args[2], pass, sizeof(pass));
    EVP_PKEY *key = UNPACK(args[0], EVP_PKEY*);
    FILE *f = fopen(path, "w");
    if (!f) return SEA_FALSE_VAL;
    int ok = pass[0]
        ? PEM_write_PrivateKey(f, key, EVP_aes_256_cbc(),
                               (unsigned char*)pass, (int)strlen(pass), NULL, NULL)
        : PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL);
    fclose(f);
    return sea_bool_val(ok == 1);
}

static SeaValue fn_key_free(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc >= 1 && sea_is_int(args[0])) EVP_PKEY_free(UNPACK(args[0], EVP_PKEY*));
    return SEA_NIL_VAL;
}

/* Raw keys (Ed25519 and X25519, as SSH carries them): key_raw(key, "public"|"private") → 32 bytes,
 * key_from_raw(type, bytes, is_private) → key, x25519(private_key, peer_public_key) → shared secret */
static SeaValue fn_key_raw(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    char which[16];
    if (!val_to_cstr(args[1], which, sizeof(which))) return SEA_NIL_VAL;
    EVP_PKEY *key = UNPACK(args[0], EVP_PKEY*);
    unsigned char buf[64];
    size_t n = sizeof(buf);
    int ok = !strcmp(which, "private") ? EVP_PKEY_get_raw_private_key(key, buf, &n)
                                       : EVP_PKEY_get_raw_public_key(key, buf, &n);
    return ok == 1 ? make_str((char*)buf, n) : SEA_NIL_VAL;
}

static SeaValue fn_key_from_raw(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 3) return SEA_NIL_VAL;
    char type[16];
    if (!val_to_cstr(args[0], type, sizeof(type))) return SEA_NIL_VAL;
    int id = !strcmp(type, "ed25519") ? EVP_PKEY_ED25519 : !strcmp(type, "x25519") ? EVP_PKEY_X25519 : 0;
    if (!id) return SEA_NIL_VAL;
    SeaString *raw = sea_value_get_flat_string(args[1]);
    if (!raw) return SEA_NIL_VAL;
    int priv = sea_is_truthy(args[2]);
    EVP_PKEY *key = priv ? EVP_PKEY_new_raw_private_key(id, NULL, (const unsigned char*)raw->data, raw->len)
                         : EVP_PKEY_new_raw_public_key(id, NULL, (const unsigned char*)raw->data, raw->len);
    sea_obj_release(&raw->obj);
    return key ? PACK_PTR(key) : SEA_NIL_VAL;
}

static SeaValue fn_x25519(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_int(args[0]) || !sea_is_int(args[1])) return SEA_NIL_VAL;
    EVP_PKEY *mine = UNPACK(args[0], EVP_PKEY*);
    EVP_PKEY *peer = UNPACK(args[1], EVP_PKEY*);
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(mine, NULL);
    unsigned char out[32];
    size_t n = sizeof(out);
    int ok = ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, peer) > 0 &&
             EVP_PKEY_derive(ctx, out, &n) > 0;
    if (ctx) EVP_PKEY_CTX_free(ctx);
    return ok ? make_str((char*)out, n) : SEA_NIL_VAL;
}

/* =========================================================================
 * CSR
 * ========================================================================= */

static SeaValue fn_csr_new(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    SeaValue opts_v = args[1];
    if (!sea_is_table(opts_v) && !sea_is_struct(opts_v) && !sea_is_map(opts_v)) return SEA_NIL_VAL;

    EVP_PKEY *key = UNPACK(args[0], EVP_PKEY*);
    X509_REQ *req = X509_REQ_new();
    if (!req) return SEA_NIL_VAL;
    X509_REQ_set_version(req, 0);
    X509_REQ_set_pubkey(req, key);

    X509_NAME *name = X509_REQ_get_subject_name(req);
    name_from_opts(name, opts_v);

    /* SAN via request extensions */
    SeaValue v;
    if (opts_get(opts_v, "san", &v) && sea_is_array(v)) {
        Buf san; buf_init(&san);
        build_san_buf(SEA_AS_ARRAY(v), &san);
        if (san.len > 0) {
            STACK_OF(X509_EXTENSION) *exts = sk_X509_EXTENSION_new_null();
            X509V3_CTX ctx;
            X509V3_set_ctx_nodb(&ctx);
            X509V3_set_ctx(&ctx, NULL, NULL, req, NULL, 0);
            X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &ctx,
                                                       NID_subject_alt_name, san.data);
            if (ext) {
                sk_X509_EXTENSION_push(exts, ext);
                X509_REQ_add_extensions(req, exts);
            }
            sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
        }
        buf_free(&san);
    }

    if (X509_REQ_sign(req, key, EVP_sha256()) <= 0) {
        X509_REQ_free(req); return SEA_NIL_VAL;
    }
    return PACK_PTR(req);
}

static SeaValue fn_csr_pem(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) return SEA_NIL_VAL;
    PEM_write_bio_X509_REQ(bio, UNPACK(args[0], X509_REQ*));
    char *ptr; long len = BIO_get_mem_data(bio, &ptr);
    SeaValue r = len > 0 ? make_str(ptr, (size_t)len) : SEA_NIL_VAL;
    BIO_free(bio);
    return r;
}

static SeaValue fn_csr_free(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc >= 1 && sea_is_int(args[0])) X509_REQ_free(UNPACK(args[0], X509_REQ*));
    return SEA_NIL_VAL;
}

/* =========================================================================
 * CRYPTOGRAPHY — hash, HMAC
 * ========================================================================= */

static SeaValue do_hash(int argc, SeaValue *args, bool hex_out) {
    if (argc < 2) return SEA_NIL_VAL;
    char algo[32];
    if (!val_to_cstr(args[0], algo, sizeof(algo))) return SEA_NIL_VAL;
    const EVP_MD *md = lookup_md(algo);
    if (!md) return SEA_NIL_VAL;
    SeaString *data = sea_value_get_flat_string(args[1]);
    if (!data) return SEA_NIL_VAL;

    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int dlen = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, md, NULL);
    EVP_DigestUpdate(ctx, data->data, data->len);
    EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);
    sea_obj_release(&data->obj);

    if (!hex_out) return make_str((char*)digest, dlen);
    char hex[EVP_MAX_MD_SIZE * 2 + 1];
    bin_to_hex(digest, dlen, hex);
    return make_str(hex, dlen * 2);
}

static SeaValue fn_hash(SeaVM *vm, int argc, SeaValue *args)     { (void)vm; return do_hash(argc, args, true);  }
static SeaValue fn_hash_raw(SeaVM *vm, int argc, SeaValue *args) { (void)vm; return do_hash(argc, args, false); }

static SeaValue do_hmac(int argc, SeaValue *args, bool hex_out) {
    if (argc < 3) return SEA_NIL_VAL;
    char algo[32];
    if (!val_to_cstr(args[0], algo, sizeof(algo))) return SEA_NIL_VAL;
    const EVP_MD *md = lookup_md(algo);
    if (!md) return SEA_NIL_VAL;
    SeaString *key  = sea_value_get_flat_string(args[1]);
    SeaString *data = sea_value_get_flat_string(args[2]);
    if (!key || !data) {
        if (key)  sea_obj_release(&key->obj);
        if (data) sea_obj_release(&data->obj);
        return SEA_NIL_VAL;
    }
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int dlen = 0;
    HMAC(md, key->data, (int)key->len,
         (unsigned char*)data->data, data->len, digest, &dlen);
    sea_obj_release(&key->obj);
    sea_obj_release(&data->obj);

    if (!hex_out) return make_str((char*)digest, dlen);
    char hex[EVP_MAX_MD_SIZE * 2 + 1];
    bin_to_hex(digest, dlen, hex);
    return make_str(hex, dlen * 2);
}

static SeaValue fn_hmac(SeaVM *vm, int argc, SeaValue *args)     { (void)vm; return do_hmac(argc, args, true);  }
static SeaValue fn_hmac_raw(SeaVM *vm, int argc, SeaValue *args) { (void)vm; return do_hmac(argc, args, false); }

/* =========================================================================
 * CRYPTOGRAPHY — symmetric (AES/ChaCha20)
 * ========================================================================= */

static SeaValue fn_aes_encrypt(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    /* encrypt(algo, key, plaintext, [iv], [aad]) */
    if (argc < 3) return SEA_NIL_VAL;
    char algo[32];
    if (!val_to_cstr(args[0], algo, sizeof(algo))) return SEA_NIL_VAL;
    const EVP_CIPHER *cipher = lookup_cipher(algo);
    if (!cipher) return SEA_NIL_VAL;

    SeaString *key_s  = sea_value_get_flat_string(args[1]);
    SeaString *plain  = sea_value_get_flat_string(args[2]);
    if (!key_s || !plain) {
        if (key_s)  sea_obj_release(&key_s->obj);
        if (plain)  sea_obj_release(&plain->obj);
        return SEA_NIL_VAL;
    }

    bool is_aead = strstr(algo, "gcm") || strstr(algo, "poly1305");
    int  iv_len  = is_aead ? 12 : EVP_CIPHER_iv_length(cipher);
    int  key_len = EVP_CIPHER_key_length(cipher);

    if ((int)key_s->len < key_len) {
        sea_obj_release(&key_s->obj); sea_obj_release(&plain->obj);
        return SEA_NIL_VAL;
    }

    unsigned char iv[32] = {0};
    if (argc >= 4) {
        SeaString *iv_s = sea_value_get_flat_string(args[3]);
        if (iv_s) {
            int n = (int)iv_s->len < iv_len ? (int)iv_s->len : iv_len;
            memcpy(iv, iv_s->data, (size_t)n);
            sea_obj_release(&iv_s->obj);
        } else {
            RAND_bytes(iv, iv_len);
        }
    } else {
        RAND_bytes(iv, iv_len);
    }

    int outlen = (int)plain->len + EVP_CIPHER_block_size(cipher) + 16;
    unsigned char *out = malloc((size_t)outlen);
    if (!out) {
        sea_obj_release(&key_s->obj); sea_obj_release(&plain->obj);
        return SEA_NIL_VAL;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, cipher, NULL, (unsigned char*)key_s->data, iv);
    int len1 = 0, len2 = 0;
    /* additional authenticated data: bound to the tag, not encrypted */
    if (is_aead && argc >= 5) {
        SeaString *aad = sea_value_get_flat_string(args[4]);
        if (aad) {
            int alen = 0;
            EVP_EncryptUpdate(ctx, NULL, &alen, (unsigned char*)aad->data, (int)aad->len);
            sea_obj_release(&aad->obj);
        }
    }
    EVP_EncryptUpdate(ctx, out, &len1, (unsigned char*)plain->data, (int)plain->len);
    EVP_EncryptFinal_ex(ctx, out + len1, &len2);

    unsigned char tag[16] = {0};
    if (is_aead) EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(ctx);
    sea_obj_release(&key_s->obj);
    sea_obj_release(&plain->obj);

    SeaTable *t = sea_table_new(TABLE_MAP);
    tbl_set_str(t, "data", (char*)out,  (size_t)(len1 + len2));
    tbl_set_str(t, "iv",   (char*)iv,   (size_t)iv_len);
    tbl_set_str(t, "tag",  (char*)tag,  is_aead ? 16 : 0);
    free(out);
    return sea_obj_val(&t->obj);
}

static SeaValue fn_aes_decrypt(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    /* decrypt(algo, key, iv, ciphertext, [tag], [aad]) */
    if (argc < 4) return SEA_NIL_VAL;
    char algo[32];
    if (!val_to_cstr(args[0], algo, sizeof(algo))) return SEA_NIL_VAL;
    const EVP_CIPHER *cipher = lookup_cipher(algo);
    if (!cipher) return SEA_NIL_VAL;

    SeaString *key_s  = sea_value_get_flat_string(args[1]);
    SeaString *iv_s   = sea_value_get_flat_string(args[2]);
    SeaString *cipher_s = sea_value_get_flat_string(args[3]);
    if (!key_s || !iv_s || !cipher_s) {
        if (key_s)    sea_obj_release(&key_s->obj);
        if (iv_s)     sea_obj_release(&iv_s->obj);
        if (cipher_s) sea_obj_release(&cipher_s->obj);
        return SEA_NIL_VAL;
    }

    bool is_aead = strstr(algo, "gcm") || strstr(algo, "poly1305");
    /* a key or an IV shorter than the cipher's would be read past its end */
    if ((int)key_s->len < EVP_CIPHER_key_length(cipher) ||
        (int)iv_s->len < (is_aead ? 12 : EVP_CIPHER_iv_length(cipher))) {
        sea_obj_release(&key_s->obj); sea_obj_release(&iv_s->obj);
        sea_obj_release(&cipher_s->obj);
        return SEA_NIL_VAL;
    }
    int outlen = (int)cipher_s->len + EVP_CIPHER_block_size(cipher);
    unsigned char *out = malloc((size_t)outlen);
    if (!out) {
        sea_obj_release(&key_s->obj); sea_obj_release(&iv_s->obj);
        sea_obj_release(&cipher_s->obj);
        return SEA_NIL_VAL;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, cipher, NULL,
                       (unsigned char*)key_s->data, (unsigned char*)iv_s->data);

    if (is_aead && argc >= 5) {
        SeaString *tag_s = sea_value_get_flat_string(args[4]);
        if (tag_s) {
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                                (int)tag_s->len, (void*)tag_s->data);
            sea_obj_release(&tag_s->obj);
        }
    }

    int len1 = 0, len2 = 0;
    if (is_aead && argc >= 6) {
        SeaString *aad = sea_value_get_flat_string(args[5]);
        if (aad) {
            int alen = 0;
            EVP_DecryptUpdate(ctx, NULL, &alen, (unsigned char*)aad->data, (int)aad->len);
            sea_obj_release(&aad->obj);
        }
    }
    EVP_DecryptUpdate(ctx, out, &len1, (unsigned char*)cipher_s->data, (int)cipher_s->len);
    int ok = EVP_DecryptFinal_ex(ctx, out + len1, &len2);
    EVP_CIPHER_CTX_free(ctx);
    sea_obj_release(&key_s->obj);
    sea_obj_release(&iv_s->obj);
    sea_obj_release(&cipher_s->obj);

    SeaValue r = ok > 0 ? make_str((char*)out, (size_t)(len1 + len2)) : SEA_NIL_VAL;
    free(out);
    return r;
}

/* =========================================================================
 * CRYPTOGRAPHY — asymmetric sign/verify
 * ========================================================================= */

static SeaValue fn_sign(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    /* sign(algo, key, data) */
    if (argc < 3 || !sea_is_int(args[1])) return SEA_NIL_VAL;
    char algo[32];
    if (!val_to_cstr(args[0], algo, sizeof(algo))) return SEA_NIL_VAL;
    int is_ed = !strcmp(algo, "ed25519");
    const EVP_MD *md = is_ed ? NULL : lookup_sign_md(algo);
    if (!md && !is_ed) return SEA_NIL_VAL;
    EVP_PKEY  *key  = UNPACK(args[1], EVP_PKEY*);
    SeaString *data = sea_value_get_flat_string(args[2]);
    if (!data) return SEA_NIL_VAL;
    if (is_ed) {
        EVP_MD_CTX *ectx = EVP_MD_CTX_new();
        unsigned char esig[64];
        size_t en = sizeof(esig);
        int eok = ectx && EVP_DigestSignInit(ectx, NULL, NULL, NULL, key) == 1 &&
                  EVP_DigestSign(ectx, esig, &en, (const unsigned char*)data->data, data->len) == 1;
        if (ectx) EVP_MD_CTX_free(ectx);
        sea_obj_release(&data->obj);
        return eok ? make_str((char*)esig, en) : SEA_NIL_VAL;
    }

    EVP_MD_CTX   *ctx  = EVP_MD_CTX_new();
    EVP_PKEY_CTX *pctx = NULL;
    EVP_DigestSignInit(ctx, &pctx, md, NULL, key);

    if (strstr(algo, "pss")) {
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING);
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST);
    }

    EVP_DigestSignUpdate(ctx, data->data, data->len);
    size_t siglen = 0;
    EVP_DigestSignFinal(ctx, NULL, &siglen);
    unsigned char *sig = malloc(siglen);
    int ok = sig ? EVP_DigestSignFinal(ctx, sig, &siglen) : 0;
    EVP_MD_CTX_free(ctx);
    sea_obj_release(&data->obj);

    SeaValue r = ok > 0 ? make_str((char*)sig, siglen) : SEA_NIL_VAL;
    free(sig);
    return r;
}

static SeaValue fn_verify(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    /* verify(algo, key, data, signature) */
    if (argc < 4 || !sea_is_int(args[1])) return SEA_FALSE_VAL;
    char algo[32];
    if (!val_to_cstr(args[0], algo, sizeof(algo))) return SEA_FALSE_VAL;
    int is_ed = !strcmp(algo, "ed25519");
    const EVP_MD *md = is_ed ? NULL : lookup_sign_md(algo);
    if (!md && !is_ed) return SEA_FALSE_VAL;
    EVP_PKEY  *key  = UNPACK(args[1], EVP_PKEY*);
    SeaString *data = sea_value_get_flat_string(args[2]);
    SeaString *sig  = sea_value_get_flat_string(args[3]);
    if (!data || !sig) {
        if (data) sea_obj_release(&data->obj);
        if (sig)  sea_obj_release(&sig->obj);
        return SEA_FALSE_VAL;
    }
    if (is_ed) {
        EVP_MD_CTX *ectx = EVP_MD_CTX_new();
        int eok = ectx && EVP_DigestVerifyInit(ectx, NULL, NULL, NULL, key) == 1 &&
                  EVP_DigestVerify(ectx, (const unsigned char*)sig->data, sig->len, (const unsigned char*)data->data, data->len) == 1;
        if (ectx) EVP_MD_CTX_free(ectx);
        sea_obj_release(&data->obj);
        sea_obj_release(&sig->obj);
        return sea_bool_val(eok);
    }

    EVP_MD_CTX   *ctx  = EVP_MD_CTX_new();
    EVP_PKEY_CTX *pctx = NULL;
    EVP_DigestVerifyInit(ctx, &pctx, md, NULL, key);

    if (strstr(algo, "pss")) {
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING);
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST);
    }

    EVP_DigestVerifyUpdate(ctx, data->data, data->len);
    int ok = EVP_DigestVerifyFinal(ctx, (unsigned char*)sig->data, sig->len);
    EVP_MD_CTX_free(ctx);
    sea_obj_release(&data->obj);
    sea_obj_release(&sig->obj);
    return sea_bool_val(ok == 1);
}

/* =========================================================================
 * CRYPTOGRAPHY — KDF, random
 * ========================================================================= */

static SeaValue fn_rand_bytes(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    int n = (int)sea_as_int(args[0]);
    if (n <= 0) return make_str("", 0);
    unsigned char *buf = malloc((size_t)n);
    if (!buf) return SEA_NIL_VAL;
    RAND_bytes(buf, n);
    SeaValue r = make_str((char*)buf, (size_t)n);
    free(buf);
    return r;
}

static SeaValue fn_pbkdf2(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    /* pbkdf2(algo, password, salt, iterations, key_len) */
    if (argc < 5) return SEA_NIL_VAL;
    char algo[32];
    if (!val_to_cstr(args[0], algo, sizeof(algo))) return SEA_NIL_VAL;
    const EVP_MD *md = lookup_md(algo);
    if (!md) return SEA_NIL_VAL;
    SeaString *pass = sea_value_get_flat_string(args[1]);
    SeaString *salt = sea_value_get_flat_string(args[2]);
    if (!pass || !salt) {
        if (pass) sea_obj_release(&pass->obj);
        if (salt) sea_obj_release(&salt->obj);
        return SEA_NIL_VAL;
    }
    int iters   = sea_is_int(args[3]) ? (int)sea_as_int(args[3]) : 10000;
    int key_len = sea_is_int(args[4]) ? (int)sea_as_int(args[4]) : 32;
    if (iters < 1 || key_len < 1) {
        sea_obj_release(&pass->obj); sea_obj_release(&salt->obj);
        return SEA_NIL_VAL;
    }
    unsigned char *out = malloc((size_t)key_len);
    PKCS5_PBKDF2_HMAC(pass->data, (int)pass->len,
                      (unsigned char*)salt->data, (int)salt->len,
                      iters, md, key_len, out);
    sea_obj_release(&pass->obj);
    sea_obj_release(&salt->obj);
    SeaValue r = make_str((char*)out, (size_t)key_len);
    free(out);
    return r;
}

static SeaValue fn_hkdf(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    /* hkdf(algo, ikm, salt, info, length) */
    if (argc < 5) return SEA_NIL_VAL;
    char algo[32];
    if (!val_to_cstr(args[0], algo, sizeof(algo))) return SEA_NIL_VAL;
    const EVP_MD *md = lookup_md(algo);
    if (!md) return SEA_NIL_VAL;
    SeaString *ikm  = sea_value_get_flat_string(args[1]);
    SeaString *salt = sea_value_get_flat_string(args[2]);
    SeaString *info = sea_value_get_flat_string(args[3]);
    int length = sea_is_int(args[4]) ? (int)sea_as_int(args[4]) : 32;
    if (!ikm || !salt || !info || length < 1) {
        if (ikm)  sea_obj_release(&ikm->obj);
        if (salt) sea_obj_release(&salt->obj);
        if (info) sea_obj_release(&info->obj);
        return SEA_NIL_VAL;
    }
    unsigned char *out = malloc((size_t)length);
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    int ok = 0;
    if (pctx) {
        size_t outlen = (size_t)length;
        ok = EVP_PKEY_derive_init(pctx) > 0 &&
             EVP_PKEY_CTX_set_hkdf_md(pctx, md) > 0 &&
             EVP_PKEY_CTX_set1_hkdf_salt(pctx, (unsigned char*)salt->data, (int)salt->len) > 0 &&
             EVP_PKEY_CTX_set1_hkdf_key(pctx, (unsigned char*)ikm->data,  (int)ikm->len)  > 0 &&
             EVP_PKEY_CTX_add1_hkdf_info(pctx, (unsigned char*)info->data, (int)info->len) > 0 &&
             EVP_PKEY_derive(pctx, out, &outlen) > 0;
        EVP_PKEY_CTX_free(pctx);
    }
    sea_obj_release(&ikm->obj);
    sea_obj_release(&salt->obj);
    sea_obj_release(&info->obj);
    SeaValue r = ok ? make_str((char*)out, (size_t)length) : SEA_NIL_VAL;
    free(out);
    return r;
}

/* =========================================================================
 * ENCODING — base64, hex
 * ========================================================================= */

static SeaValue fn_base64_encode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *data = sea_value_get_flat_string(args[0]);
    if (!data) return SEA_NIL_VAL;
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, mem);
    BIO_write(b64, data->data, (int)data->len);
    BIO_flush(b64);
    char *ptr; long len = BIO_get_mem_data(mem, &ptr);
    SeaValue r = len >= 0 ? make_str(ptr, (size_t)len) : make_str("", 0);
    BIO_free_all(b64);
    sea_obj_release(&data->obj);
    return r;
}

static SeaValue fn_base64_decode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *str = sea_value_get_flat_string(args[0]);
    if (!str) return SEA_NIL_VAL;
    size_t outmax = (str->len * 3) / 4 + 4;
    unsigned char *out = malloc(outmax);
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new_mem_buf(str->data, (int)str->len);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, mem);
    int n = BIO_read(b64, out, (int)outmax);
    BIO_free_all(b64);
    sea_obj_release(&str->obj);
    SeaValue r = n > 0 ? make_str((char*)out, (size_t)n) : SEA_NIL_VAL;
    free(out);
    return r;
}

static SeaValue fn_hex_encode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *data = sea_value_get_flat_string(args[0]);
    if (!data) return SEA_NIL_VAL;
    char *hex = malloc(data->len * 2 + 1);
    if (!hex) { sea_obj_release(&data->obj); return SEA_NIL_VAL; }
    bin_to_hex((unsigned char*)data->data, data->len, hex);
    SeaValue r = make_str(hex, data->len * 2);
    free(hex);
    sea_obj_release(&data->obj);
    return r;
}

static SeaValue fn_hex_decode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *str = sea_value_get_flat_string(args[0]);
    if (!str) return SEA_NIL_VAL;
    if (str->len % 2 != 0) { sea_obj_release(&str->obj); return SEA_NIL_VAL; }
    size_t outlen = str->len / 2;
    unsigned char *out = malloc(outlen + 1);
    int ok = 1;
    for (size_t i = 0; i < outlen; i++) {
        int hi = hex_nibble(str->data[i*2]);
        int lo = hex_nibble(str->data[i*2+1]);
        if (hi < 0 || lo < 0) { ok = 0; break; }
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    out[outlen] = '\0';
    sea_obj_release(&str->obj);
    SeaValue r = ok ? make_str((char*)out, outlen) : SEA_NIL_VAL;
    free(out);
    return r;
}

/* =========================================================================
 * MODULE ENTRY POINT
 * ========================================================================= */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();

    /* TLS client */
    sea_mod_set(mod, "tls_client",    fn_tls_client);
    sea_mod_set(mod, "tls_server",    fn_tls_server);
    sea_mod_set(mod, "tls_handshake", fn_tls_handshake);
    sea_mod_set(mod, "tls_recv",      fn_tls_recv);
    sea_mod_set(mod, "tls_send",      fn_tls_send);
    sea_mod_set(mod, "tls_close",     fn_tls_close);
    sea_mod_set(mod, "tls_peer_cert", fn_tls_peer_cert);
    sea_mod_set(mod, "tls_version",   fn_tls_version);
    sea_mod_set(mod, "tls_cipher",    fn_tls_cipher);

    /* TLS server */
    sea_mod_set(mod, "server_new",    fn_server_new);
    sea_mod_set(mod, "server_free",   fn_server_free);

    /* Certificate */
    sea_mod_set(mod, "cert_load_file",   fn_cert_load_file);
    sea_mod_set(mod, "cert_parse",       fn_cert_parse);
    sea_mod_set(mod, "cert_info",        fn_cert_info);
    sea_mod_set(mod, "cert_pem",         fn_cert_pem);
    sea_mod_set(mod, "pkcs12_parse",     fn_pkcs12_parse);
    sea_mod_set(mod, "cert_save",        fn_cert_save);
    sea_mod_set(mod, "cert_free",        fn_cert_free);
    sea_mod_set(mod, "cert_verify",      fn_cert_verify);
    sea_mod_set(mod, "cert_pubkey",      fn_cert_pubkey);
    sea_mod_set(mod, "cert_self_signed", fn_cert_self_signed);
    sea_mod_set(mod, "cert_sign",        fn_cert_sign);

    /* Key */
    sea_mod_set(mod, "key_generate",  fn_key_generate);
    sea_mod_set(mod, "key_load_file", fn_key_load_file);
    sea_mod_set(mod, "key_parse",     fn_key_parse);
    sea_mod_set(mod, "key_pem",       fn_key_pem);
    sea_mod_set(mod, "key_pub_pem",   fn_key_pub_pem);
    sea_mod_set(mod, "key_save",      fn_key_save);
    sea_mod_set(mod, "key_free",      fn_key_free);
    sea_mod_set(mod, "key_raw",       fn_key_raw);
    sea_mod_set(mod, "key_from_raw",  fn_key_from_raw);
    sea_mod_set(mod, "x25519",        fn_x25519);

    /* CSR */
    sea_mod_set(mod, "csr_new",  fn_csr_new);
    sea_mod_set(mod, "csr_pem",  fn_csr_pem);
    sea_mod_set(mod, "csr_free", fn_csr_free);

    /* Crypto */
    sea_mod_set(mod, "hash",           fn_hash);
    sea_mod_set(mod, "hash_raw",       fn_hash_raw);
    sea_mod_set(mod, "hmac",           fn_hmac);
    sea_mod_set(mod, "hmac_raw",       fn_hmac_raw);
    sea_mod_set(mod, "aes_encrypt",    fn_aes_encrypt);
    sea_mod_set(mod, "aes_decrypt",    fn_aes_decrypt);
    sea_mod_set(mod, "sign",           fn_sign);
    sea_mod_set(mod, "verify",         fn_verify);
    sea_mod_set(mod, "rand_bytes",     fn_rand_bytes);
    sea_mod_set(mod, "pbkdf2",         fn_pbkdf2);
    sea_mod_set(mod, "hkdf",           fn_hkdf);
    sea_mod_set(mod, "base64_encode",  fn_base64_encode);
    sea_mod_set(mod, "base64_decode",  fn_base64_decode);
    sea_mod_set(mod, "hex_encode",     fn_hex_encode);
    sea_mod_set(mod, "hex_decode",     fn_hex_decode);

    return sea_obj_val(&mod->obj);
}
