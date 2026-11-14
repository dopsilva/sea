/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_stdlib.h"
#include "sea_string.h"
#include "sea_rope.h"
#include "sea_array.h"
#include "sea_table.h"
#include "sea_memory.h"
#include "sea_bigint.h"
#include <ctype.h>
#include <stdarg.h>
#include <string.h>
#include "sea_compat.h"
#define memmem sea_memmem

/* Convention (docs/memory.md): natives borrow their arguments and return an
 * owned value — a new object with rc=1, or sea_val_ref() of an argument. */

// string.len(s)
static SeaValue str_len(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    if (sea_is_string(args[0])) return sea_int_val(SEA_AS_STRING(args[0])->len);
    if (sea_is_rope(args[0]))   return sea_int_val((int64_t)SEA_AS_ROPE(args[0])->total_len);
    return sea_int_val(0);
}

// string.sub(s, i, j?)
static SeaValue str_sub(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_NIL_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return SEA_NIL_VAL;
    int64_t slen = (int64_t)s->len;
    int64_t i = sea_is_int(args[1]) ? sea_as_int(args[1]) : (int64_t)sea_as_number(args[1]);
    int64_t j = slen;
    if (argc >= 3) {
        j = sea_is_int(args[2]) ? sea_as_int(args[2]) : (int64_t)sea_as_number(args[2]);
    }
    if (i < 0) i = slen + i + 1;
    if (j < 0) j = slen + j + 1;
    if (i < 1) i = 1;
    if (j > slen) j = slen;
    SeaValue result;
    if (i > j) {
        result = sea_obj_val(&sea_string_new("", 0)->obj);
    } else {
        size_t start = (size_t)(i - 1);
        size_t len = (size_t)(j - i + 1);
        result = (len == 1)
            ? sea_obj_val(&sea_string_get_byte((unsigned char)s->data[start])->obj)
            : sea_obj_val(&sea_string_new(s->data + start, len)->obj);
    }
    sea_obj_release(&s->obj);
    return result;
}

// string.upper(s) — single allocation: write directly into SeaString's data[]
static SeaValue str_upper(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return SEA_NIL_VAL;
    SeaString *result = sea_string_alloc(s->len);
    for (uint32_t i = 0; i < s->len; i++)
        result->data[i] = (char)toupper((unsigned char)s->data[i]);
    result->data[s->len] = '\0';
    sea_obj_release(&s->obj);
    return sea_obj_val(&result->obj);
}

// string.lower(s) — single allocation
static SeaValue str_lower(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return SEA_NIL_VAL;
    SeaString *result = sea_string_alloc(s->len);
    for (uint32_t i = 0; i < s->len; i++)
        result->data[i] = (char)tolower((unsigned char)s->data[i]);
    result->data[s->len] = '\0';
    sea_obj_release(&s->obj);
    return sea_obj_val(&result->obj);
}

// string.rep(s, n, sep?)
static SeaValue str_rep(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_NIL_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return SEA_NIL_VAL;
    int64_t n = sea_is_int(args[1]) ? sea_as_int(args[1]) : (int64_t)sea_as_number(args[1]);
    const char *sep = "";
    size_t seplen = 0;
    if (argc >= 3 && sea_is_string(args[2])) {
        sep = SEA_AS_STRING(args[2])->data;
        seplen = SEA_AS_STRING(args[2])->len;
    }
    if (n <= 0) { sea_obj_release(&s->obj); return sea_obj_val(&sea_string_new("", 0)->obj); }
    size_t total = (size_t)s->len * (size_t)n + seplen * (size_t)(n - 1);
    char *buf = (char*)sea_malloc(total + 1);
    char *p = buf;
    for (int64_t i = 0; i < n; i++) {
        if (i > 0 && seplen > 0) { memcpy(p, sep, seplen); p += seplen; }
        memcpy(p, s->data, s->len);
        p += s->len;
    }
    *p = '\0';
    SeaString *result = sea_string_new(buf, total);
    sea_free(buf, total + 1);
    sea_obj_release(&s->obj);
    return sea_obj_val(&result->obj);
}

// string.byte(s, i?)
static SeaValue str_byte(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return SEA_NIL_VAL;
    int64_t i = 1;
    if (argc >= 2) i = sea_is_int(args[1]) ? sea_as_int(args[1]) : (int64_t)sea_as_number(args[1]);
    if (i < 0) i = (int64_t)s->len + i + 1;
    SeaValue result = (i < 1 || i > (int64_t)s->len) ? SEA_NIL_VAL : sea_int_val((uint8_t)s->data[i-1]);
    sea_obj_release(&s->obj);
    return result;
}

// string.char(...)
static SeaValue str_char(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    char *buf = (char*)sea_malloc((size_t)argc + 1);
    for (int i = 0; i < argc; i++) {
        int64_t c = sea_is_int(args[i]) ? sea_as_int(args[i]) : (int64_t)sea_as_number(args[i]);
        buf[i] = (char)(c & 0xFF);
    }
    buf[argc] = '\0';
    SeaString *result = sea_string_new(buf, (size_t)argc);
    sea_free(buf, (size_t)argc + 1);
    return sea_obj_val(&result->obj);
}

// string.format(fmt, ...)
/* Growable output of string.format */
typedef struct { char *p; size_t len, cap; } FmtBuf;

static void fmt_put(FmtBuf *b, const char *s, size_t n);

/* %d %i %u %x %X of a bigint: C's flags, width and precision applied to its
 * digits (a negative hex value is written with a minus sign). */
static void fmt_bigint(FmtBuf *out, const char *spec, char conv, SeaBigInt *b) {
    bool left = false, plus = false, space = false, zero = false;
    int width = 0, prec = -1;
    const char *p = spec + 1;
    for (; *p && strchr("-+ 0#", *p); p++) {
        if (*p == '-') left = true;
        else if (*p == '+') plus = true;
        else if (*p == ' ') space = true;
        else if (*p == '0') zero = true;
    }
    for (; isdigit((unsigned char)*p); p++) width = width * 10 + (*p - '0');
    if (*p == '.') {
        prec = 0;
        for (p++; isdigit((unsigned char)*p); p++) prec = prec * 10 + (*p - '0');
    }
    bool hex = conv == 'x' || conv == 'X';
    SeaString *s = sea_bigint_to_string(b, hex ? 16 : 10);
    const char *dig = s->data;
    size_t nd = s->len;
    bool neg = dig[0] == '-';
    if (neg) { dig++; nd--; }
    if (prec == 0 && nd == 1 && dig[0] == '0') nd = 0;      /* like C: %.0d of 0 */
    char sign = neg ? '-' : (!hex && plus) ? '+' : (!hex && space) ? ' ' : 0;
    size_t zeros = prec > 0 && (size_t)prec > nd ? (size_t)prec - nd : 0;
    size_t body = (sign ? 1 : 0) + zeros + nd;
    if (prec < 0 && zero && !left && (size_t)width > body) { zeros += (size_t)width - body; body = (size_t)width; }
    size_t pad = (size_t)width > body ? (size_t)width - body : 0;
    for (size_t i = 0; !left && i < pad; i++) fmt_put(out, " ", 1);
    if (sign) fmt_put(out, &sign, 1);
    for (size_t i = 0; i < zeros; i++) fmt_put(out, "0", 1);
    for (size_t i = 0; i < nd; i++) {
        char c = conv == 'X' ? (char)toupper((unsigned char)dig[i]) : dig[i];
        fmt_put(out, &c, 1);
    }
    for (size_t i = 0; left && i < pad; i++) fmt_put(out, " ", 1);
    sea_obj_release(&s->obj);
}

static void fmt_put(FmtBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        b->p = (char*)realloc(b->p, cap);
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

/* snprintf one conversion straight into the buffer */
static void fmt_printf(FmtBuf *b, const char *spec, ...) {
    va_list ap, ap2;
    va_start(ap, spec);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, spec, ap);
    va_end(ap);
    if (n > 0) {
        if (b->len + (size_t)n + 1 > b->cap) {
            size_t cap = b->cap ? b->cap : 256;
            while (b->len + (size_t)n + 1 > cap) cap *= 2;
            b->p = (char*)realloc(b->p, cap);
            b->cap = cap;
        }
        vsnprintf(b->p + b->len, (size_t)n + 1, spec, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

SeaValue sea_str_format(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *fmt_s = sea_value_get_flat_string(args[0]);
    if (!fmt_s) return SEA_NIL_VAL;
    const char *p = fmt_s->data, *end = fmt_s->data + fmt_s->len;

    FmtBuf out = {0};
    int arg_idx = 1;

    while (p < end) {
        const char *lit = p;
        while (p < end && *p != '%') p++;
        fmt_put(&out, lit, (size_t)(p - lit));
        if (p >= end) break;
        p++;                                       /* '%' */
        if (p >= end) { fmt_put(&out, "%", 1); break; }
        if (*p == '%') { fmt_put(&out, "%", 1); p++; continue; }

        /* the spec: flags, width and precision (at most 9 digits each) */
        char spec[48];
        int si = 0;
        spec[si++] = '%';
        while (p < end && strchr("-+ 0#", *p) && si < 8) spec[si++] = *p++;
        while (p < end && strchr("-+ 0#", *p)) p++;
        for (int d = 0; p < end && isdigit((unsigned char)*p); p++)
            if (d++ < 9) spec[si++] = *p;
        if (p < end && *p == '.') {
            spec[si++] = *p++;
            for (int d = 0; p < end && isdigit((unsigned char)*p); p++)
                if (d++ < 9) spec[si++] = *p;
        }
        bool plain = si == 1;                      /* no flags, width or precision */
        char conv = p < end ? *p++ : '\0';
        spec[si] = '\0';

        SeaValue arg = (arg_idx < argc) ? args[arg_idx++] : SEA_NIL_VAL;
        char full[64];

        switch (conv) {
            case 'd': case 'i': case 'u': case 'x': case 'X': {
                if (sea_is_bigint(arg)) {
                    fmt_bigint(&out, spec, conv, SEA_AS_BIGINT(arg));
                    break;
                }
                int64_t v = sea_is_int(arg) ? sea_as_int(arg) : (int64_t)sea_as_number(arg);
                snprintf(full, sizeof full, "%sll%c", spec, conv);
                if (conv == 'd' || conv == 'i') fmt_printf(&out, full, (long long)v);
                else fmt_printf(&out, full, (unsigned long long)v);
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                double v = sea_is_number(arg) ? sea_as_number(arg) :
                           sea_is_int(arg) ? (double)sea_as_int(arg) : 0.0;
                snprintf(full, sizeof full, "%s%c", spec, conv);
                fmt_printf(&out, full, v);
                break;
            }
            case 's': {
                SeaString *str = sea_vm_tostring(vm, arg);
                if (plain) {
                    fmt_put(&out, str->data, str->len);   /* any length, any bytes */
                } else {
                    snprintf(full, sizeof full, "%ss", spec);
                    fmt_printf(&out, full, str->data);
                }
                sea_obj_release(&str->obj);  /* tostring returns rc=1 */
                break;
            }
            case 'c': {
                int64_t v = sea_is_int(arg) ? sea_as_int(arg) : (int64_t)sea_as_number(arg);
                snprintf(full, sizeof full, "%sc", spec);
                fmt_printf(&out, full, (int)v);
                break;
            }
            default: {
                /* unknown conversion: written as is */
                char u[2] = { '%', conv };
                fmt_put(&out, u, conv ? 2 : 1);
                break;
            }
        }
    }

    sea_obj_release(&fmt_s->obj);
    SeaString *r = sea_string_new(out.p ? out.p : "", out.len);
    free(out.p);
    return sea_obj_val(&r->obj);
}

// string.find(s, pattern, init?)
static SeaValue str_find(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_NIL_VAL;
    SeaString *ss = sea_value_get_flat_string(args[0]);
    if (!ss || !sea_is_string(args[1])) { if (ss) sea_obj_release(&ss->obj); return SEA_NIL_VAL; }
    const char *s = ss->data;
    size_t slen = ss->len;
    const char *pat = SEA_AS_STRING(args[1])->data;
    size_t plen = SEA_AS_STRING(args[1])->len;
    int64_t init = 1;
    if (argc >= 3) init = sea_is_int(args[2]) ? sea_as_int(args[2]) : (int64_t)sea_as_number(args[2]);
    if (init < 0) init = (int64_t)slen + init + 1;
    if (init < 1) init = 1;
    if ((size_t)init > slen) { sea_obj_release(&ss->obj); return SEA_NIL_VAL; }
    /* bytes, not C strings: either side may hold a zero byte */
    const char *found = memmem(s + init - 1, slen - (size_t)(init - 1), pat, plen);
    sea_obj_release(&ss->obj);
    if (!found) return SEA_NIL_VAL;
    return sea_int_val((int64_t)(found - s) + 1);
}

// string.split(s, sep)
static SeaValue str_split(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_string(args[1])) return SEA_NIL_VAL;
    SeaString *ss = sea_value_get_flat_string(args[0]);
    if (!ss) return SEA_NIL_VAL;
    const char *s = ss->data;
    const char *end = s + ss->len;
    const char *sep = SEA_AS_STRING(args[1])->data;
    size_t seplen = SEA_AS_STRING(args[1])->len;
    /* split(s, sep, n): at most n pieces, the last holding the rest */
    int64_t limit = argc >= 3 && sea_is_int(args[2]) && sea_as_int(args[2]) > 0 ? sea_as_int(args[2]) : -1;

    SeaArray *arr = sea_array_new(4);
    if (seplen == 0) {
        while (s < end) {
            SeaValue sv = sea_obj_val(&sea_string_new(s, 1)->obj);
            sea_array_push(arr, sv);  /* array retains */
            sea_obj_release(sea_as_obj(sv));  /* release our rc */
            s++;
        }
    } else {
        /* n separators give n + 1 pieces, empty ones included at either end
           (as Go's strings.Split): split("a,", ",") is ["a", ""], split("", ",") [""] */
        const char *start = s;
        for (;;) {
            const char *found = limit > 0 && (int64_t)arr->len == limit - 1 ? NULL :
                memmem(start, (size_t)(end - start), sep, seplen);
            if (!found) {
                SeaValue sv = sea_obj_val(&sea_string_new(start, (size_t)(end - start))->obj);
                sea_array_push(arr, sv);
                sea_obj_release(sea_as_obj(sv));
                break;
            }
            SeaValue sv = sea_obj_val(&sea_string_new(start, (size_t)(found - start))->obj);
            sea_array_push(arr, sv);
            sea_obj_release(sea_as_obj(sv));
            start = found + seplen;
        }
    }
    sea_obj_release(&ss->obj);
    /* Return array rc=1; caller owns */
    return sea_obj_val(&arr->obj);
}

// string.splitlines(s) → the lines, without "\n" / "\r\n" (a final newline
// does not add an empty line)
static SeaValue str_splitlines(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "string.splitlines: expected a string");
    SeaString *ss = sea_value_get_flat_string(args[0]);
    if (!ss) sea_vm_error_noreturn(vm, "string.splitlines: expected a string, got %s", sea_type_name(args[0]));
    SeaArray *arr = sea_array_new(8);
    size_t i = 0, start = 0, n = ss->len;
    while (i <= n) {
        if (i == n || ss->data[i] == '\n') {
            if (i == n && start == n) break;   /* nothing after the last newline */
            size_t end = i;
            if (end > start && ss->data[end - 1] == '\r') end--;
            SeaString *line = sea_string_new(ss->data + start, end - start);
            sea_array_push(arr, sea_obj_val(&line->obj));
            sea_obj_release(&line->obj);
            start = i + 1;
        }
        i++;
    }
    sea_obj_release(&ss->obj);
    return sea_obj_val(&arr->obj);
}

// string.format_number(n [, decimals [, sep [, point]]]) → "1,234,567.89"
static SeaValue str_format_number(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || (!sea_is_int(args[0]) && !sea_is_number(args[0])))
        sea_vm_error_noreturn(vm, "string.format_number: expected a number");
    int decimals = argc >= 2 && sea_is_int(args[1]) ? (int)sea_as_int(args[1]) : (sea_is_int(args[0]) ? 0 : 2);
    if (decimals < 0) decimals = 0;
    if (decimals > 20) decimals = 20;
    const char *sep = argc >= 3 && sea_is_string(args[2]) ? SEA_AS_STRING(args[2])->data : ",";
    const char *point = argc >= 4 && sea_is_string(args[3]) ? SEA_AS_STRING(args[3])->data : ".";
    char digits[512];
    if (sea_is_int(args[0]) && decimals == 0)
        snprintf(digits, sizeof digits, "%lld", (long long)sea_as_int(args[0]));
    else
        snprintf(digits, sizeof digits, "%.*f", decimals,
                 sea_is_int(args[0]) ? (double)sea_as_int(args[0]) : sea_as_number(args[0]));
    const char *p = digits;
    bool neg = *p == '-';
    if (neg) p++;
    const char *dot = strchr(p, '.');
    size_t int_len = dot ? (size_t)(dot - p) : strlen(p);
    size_t seplen = strlen(sep), plen = strlen(point);
    char out[1024];
    size_t o = 0;
    if (neg) out[o++] = '-';
    for (size_t i = 0; i < int_len && o < sizeof out - 8; i++) {
        if (i > 0 && (int_len - i) % 3 == 0 && o + seplen < sizeof out - 8) { memcpy(out + o, sep, seplen); o += seplen; }
        out[o++] = p[i];
    }
    if (dot && o + plen + strlen(dot + 1) < sizeof out) {
        memcpy(out + o, point, plen);
        o += plen;
        size_t fl = strlen(dot + 1);
        memcpy(out + o, dot + 1, fl);
        o += fl;
    }
    return sea_obj_val(&sea_string_new(out, o)->obj);
}

// string.contains(s, sub)
static SeaValue str_contains(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_string(args[1])) return SEA_FALSE_VAL;
    SeaString *ss = sea_value_get_flat_string(args[0]);
    if (!ss) return SEA_FALSE_VAL;
    bool found = memmem(ss->data, ss->len, SEA_AS_STRING(args[1])->data, SEA_AS_STRING(args[1])->len) != NULL;
    sea_obj_release(&ss->obj);
    return sea_bool_val(found);
}

// string.trim(s)
static SeaValue str_trim(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *ss = sea_value_get_flat_string(args[0]);
    if (!ss) return SEA_NIL_VAL;
    const char *s = ss->data;
    while (isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len-1])) len--;
    SeaValue result = sea_obj_val(&sea_string_new(s, len)->obj);
    sea_obj_release(&ss->obj);
    return result;
}

// string.starts_with(s, prefix)
static SeaValue str_starts_with(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_string(args[1])) return SEA_FALSE_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return SEA_FALSE_VAL;
    SeaString *pre = SEA_AS_STRING(args[1]);
    bool ok = pre->len <= s->len && memcmp(s->data, pre->data, pre->len) == 0;
    sea_obj_release(&s->obj);
    return sea_bool_val(ok);
}

// string.ends_with(s, suffix)
static SeaValue str_ends_with(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_string(args[1])) return SEA_FALSE_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return SEA_FALSE_VAL;
    SeaString *suf = SEA_AS_STRING(args[1]);
    bool ok = suf->len <= s->len && memcmp(s->data + s->len - suf->len, suf->data, suf->len) == 0;
    sea_obj_release(&s->obj);
    return sea_bool_val(ok);
}

// string.reverse(s) — single allocation
static SeaValue str_reverse(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return SEA_NIL_VAL;
    SeaString *result = sea_string_alloc(s->len);
    for (uint32_t i = 0; i < s->len; i++)
        result->data[i] = s->data[s->len - 1 - i];
    result->data[s->len] = '\0';
    sea_obj_release(&s->obj);
    return sea_obj_val(&result->obj);
}

// string.pad_left(s, width, char?) — right-align: pad on the left
static SeaValue str_pad_left(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return argc > 0 ? sea_val_ref(args[0]) : SEA_NIL_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return sea_val_ref(args[0]);
    int64_t width = sea_is_int(args[1]) ? sea_as_int(args[1]) : (int64_t)sea_as_number(args[1]);
    char pad_ch = ' ';
    if (argc >= 3 && sea_is_string(args[2]) && SEA_AS_STRING(args[2])->len > 0)
        pad_ch = SEA_AS_STRING(args[2])->data[0];
    if (width <= (int64_t)s->len) { sea_obj_release(&s->obj); return sea_val_ref(args[0]); }
    size_t pad = (size_t)(width - (int64_t)s->len);
    SeaString *result = sea_string_alloc((size_t)width);
    memset(result->data, pad_ch, pad);
    memcpy(result->data + pad, s->data, s->len);
    result->data[width] = '\0';
    sea_obj_release(&s->obj);
    return sea_obj_val(&result->obj);
}

// string.pad_right(s, width, char?) — left-align: pad on the right
static SeaValue str_pad_right(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return argc > 0 ? sea_val_ref(args[0]) : SEA_NIL_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return sea_val_ref(args[0]);
    int64_t width = sea_is_int(args[1]) ? sea_as_int(args[1]) : (int64_t)sea_as_number(args[1]);
    char pad_ch = ' ';
    if (argc >= 3 && sea_is_string(args[2]) && SEA_AS_STRING(args[2])->len > 0)
        pad_ch = SEA_AS_STRING(args[2])->data[0];
    if (width <= (int64_t)s->len) { sea_obj_release(&s->obj); return sea_val_ref(args[0]); }
    size_t pad = (size_t)(width - (int64_t)s->len);
    SeaString *result = sea_string_alloc((size_t)width);
    memcpy(result->data, s->data, s->len);
    memset(result->data + s->len, pad_ch, pad);
    result->data[width] = '\0';
    sea_obj_release(&s->obj);
    return sea_obj_val(&result->obj);
}

// string.pad_center(s, width, char?) — center-align: even extra goes to the right
static SeaValue str_pad_center(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return argc > 0 ? sea_val_ref(args[0]) : SEA_NIL_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return sea_val_ref(args[0]);
    int64_t width = sea_is_int(args[1]) ? sea_as_int(args[1]) : (int64_t)sea_as_number(args[1]);
    char pad_ch = ' ';
    if (argc >= 3 && sea_is_string(args[2]) && SEA_AS_STRING(args[2])->len > 0)
        pad_ch = SEA_AS_STRING(args[2])->data[0];
    if (width <= (int64_t)s->len) { sea_obj_release(&s->obj); return sea_val_ref(args[0]); }
    size_t total_pad = (size_t)(width - (int64_t)s->len);
    size_t left_pad  = total_pad / 2;
    size_t right_pad = total_pad - left_pad;
    SeaString *result = sea_string_alloc((size_t)width);
    memset(result->data, pad_ch, left_pad);
    memcpy(result->data + left_pad, s->data, s->len);
    memset(result->data + left_pad + s->len, pad_ch, right_pad);
    result->data[width] = '\0';
    sea_obj_release(&s->obj);
    return sea_obj_val(&result->obj);
}

// string.replace(s, old, new)
static SeaValue str_replace(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 3 || !sea_is_string(args[1]) || !sea_is_string(args[2]))
        return argc > 0 ? sea_val_ref(args[0]) : SEA_NIL_VAL;
    SeaString *ss = sea_value_get_flat_string(args[0]);
    if (!ss) return argc > 0 ? sea_val_ref(args[0]) : SEA_NIL_VAL;
    const char *s = ss->data;
    size_t slen = ss->len;
    const char *old_str = SEA_AS_STRING(args[1])->data;
    size_t old_len = SEA_AS_STRING(args[1])->len;
    const char *new_str = SEA_AS_STRING(args[2])->data;
    size_t new_len = SEA_AS_STRING(args[2])->len;

    if (old_len == 0) { sea_obj_release(&ss->obj); return sea_val_ref(args[0]); }

    char *buf = NULL;
    int buf_len = 0, buf_cap = 0;
    const char *p = s;
    const char *end = s + slen;
    while (p < end) {
        const char *found = memmem(p, (size_t)(end - p), old_str, old_len);
        if (!found) {
            while (p < end) {
                SEA_DYNARRAY_PUSH(buf, buf_len, buf_cap, char, *p++);
            }
            break;
        }
        while (p < found) SEA_DYNARRAY_PUSH(buf, buf_len, buf_cap, char, *p++);
        for (size_t i = 0; i < new_len; i++) SEA_DYNARRAY_PUSH(buf, buf_len, buf_cap, char, new_str[i]);
        p = found + old_len;
    }
    SEA_DYNARRAY_PUSH(buf, buf_len, buf_cap, char, '\0');
    SeaString *result = sea_string_new(buf, (size_t)(buf_len - 1));
    sea_free(buf, (size_t)buf_cap);
    sea_obj_release(&ss->obj);
    return sea_obj_val(&result->obj);
}

// string.normalize_ws(s) → colapsa whitespace em espaço único, sem leading/trailing
// Equivalente nativo de normalize_ws em carolina_train.sea — O(N) sem objetos Sea intermediários.
static SeaValue str_normalize_ws(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_obj_val(&sea_string_from_cstr("")->obj);
    SeaString *input = sea_value_get_flat_string(args[0]);
    if (!input) return sea_obj_val(&sea_string_from_cstr("")->obj);
    const char *s    = input->data;
    size_t      slen = input->len;

    /* Allocate worst-case buffer (same length as input). */
    char  *buf = (char*)sea_malloc(slen + 1);
    size_t out = 0;
    int    in_ws      = 1;   /* start as if we had leading whitespace */
    int    has_content = 0;

    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!in_ws && has_content) { in_ws = 1; }
        } else {
            if (in_ws && has_content) buf[out++] = ' ';
            buf[out++]  = (char)c;
            in_ws       = 0;
            has_content = 1;
        }
    }
    buf[out] = '\0';

    SeaString *result = sea_string_new(buf, out);
    sea_free(buf, slen + 1);
    sea_obj_release(&input->obj);
    return sea_obj_val(&result->obj);
}

// string.join(arr, sep) → concatena elementos de arr com separador sep
// Evita O(N²) de concatenação incremental: constrói o resultado em um único buffer.
static SeaValue str_join(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_obj(args[0])) return sea_obj_val(&sea_string_from_cstr("")->obj);
    SeaObject *o = sea_as_obj(args[0]);
    /* An array, or a positional table ({"a", "b"}: keys 0, 1, ...), whose
     * values are borrowed into a temporary list */
    const SeaValue *vals = NULL;
    SeaValue *tvals = NULL;
    uint32_t n = 0, tcap = 0;
    if (o->type == OBJ_ARRAY) {
        vals = ((SeaArray*)o)->data;
        n = ((SeaArray*)o)->len;
    } else if (o->type == OBJ_TABLE) {
        SeaValue v;
        while (sea_table_get((SeaTable*)o, sea_int_val(n), &v)) {
            if (n == tcap) {
                uint32_t nc = tcap ? tcap * 2 : 16;
                tvals = (SeaValue*)sea_realloc(tvals, tcap * sizeof(SeaValue), nc * sizeof(SeaValue));
                tcap = nc;
            }
            tvals[n++] = v;
        }
        vals = tvals;
    } else {
        return sea_obj_val(&sea_string_from_cstr("")->obj);
    }

    const char *sep     = "";
    size_t      sep_len = 0;
    if (argc >= 2 && sea_is_string(args[1])) {
        sep     = SEA_AS_STRING(args[1])->data;
        sep_len = SEA_AS_STRING(args[1])->len;
    }

    /* Flatten any rope elements up front so we can measure and copy
     * correctly; other values (numbers, booleans) join as tostring shows them. */
    SeaString **flat = (SeaString**)sea_malloc(n * sizeof(SeaString*));
    size_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        SeaValue v = vals[i];
        flat[i] = sea_value_get_flat_string(v);
        if (!flat[i]) flat[i] = sea_vm_tostring(vm, v);
        if (flat[i]) total += flat[i]->len;
        if (i + 1 < n) total += sep_len;
    }

    char *buf = (char*)sea_malloc(total + 1);
    char *p   = buf;
    for (uint32_t i = 0; i < n; i++) {
        if (flat[i]) {
            memcpy(p, flat[i]->data, flat[i]->len);
            p += flat[i]->len;
            sea_obj_release(&flat[i]->obj);
        }
        if (i + 1 < n && sep_len > 0) {
            memcpy(p, sep, sep_len);
            p += sep_len;
        }
    }
    *p = '\0';
    sea_free(flat, n * sizeof(SeaString*));
    if (tvals) sea_free(tvals, tcap * sizeof(SeaValue));

    SeaString *result = sea_string_new(buf, total);
    sea_free(buf, total + 1);
    return sea_obj_val(&result->obj);
}

void sea_lib_string_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);  /* rc=1 */

    mod_set(vm, mod, "len",         str_len);
    mod_set(vm, mod, "sub",         str_sub);
    mod_set(vm, mod, "upper",       str_upper);
    mod_set(vm, mod, "lower",       str_lower);
    mod_set(vm, mod, "rep",         str_rep);
    mod_set(vm, mod, "byte",        str_byte);
    mod_set(vm, mod, "char",        str_char);
    mod_set(vm, mod, "format",      sea_str_format);
    mod_set(vm, mod, "find",        str_find);
    mod_set(vm, mod, "splitlines",  str_splitlines);
    mod_set(vm, mod, "format_number", str_format_number);
    mod_set(vm, mod, "split",       str_split);
    mod_set(vm, mod, "contains",    str_contains);
    mod_set(vm, mod, "trim",        str_trim);
    mod_set(vm, mod, "starts_with", str_starts_with);
    mod_set(vm, mod, "ends_with",   str_ends_with);
    mod_set(vm, mod, "reverse",     str_reverse);
    mod_set(vm, mod, "replace",     str_replace);
    mod_set(vm, mod, "pad_left",    str_pad_left);
    mod_set(vm, mod, "pad_right",   str_pad_right);
    mod_set(vm, mod, "pad_center",  str_pad_center);
    mod_set(vm, mod, "join",        str_join);
    mod_set(vm, mod, "normalize_ws", str_normalize_ws);

    sea_vm_register_module(vm, "string", mod);
    /* register_module stores in globals table (which retains mod).
     * Release our local ref. */
    sea_obj_release(&mod->obj);
}
