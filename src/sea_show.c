/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * The text of containers for print / tostring (one line) and dump (indented
 * when long): [1, "two"], {name = "ana"}, [="k" = 1]. Scalars and other
 * objects use sea_vm_tostring.
 *
 * Limits keep a print readable and bounded: at most SHOW_MAX_ITEMS items per
 * container ("... (N more)"), nesting deeper than the depth limit shows as
 * [...] / {...}, and a container inside itself as <cycle>. Table and map
 * keys are sorted (numbers, then strings, then the rest), so the text does
 * not depend on hash order; a struct keeps the order of its literal.
 */

#include "sea_show.h"
#include "sea_vm.h"
#include "sea_string.h"
#include "sea_table.h"
#include "sea_map.h"
#include "sea_struct.h"
#include "sea_array.h"
#include "sea_typed_array.h"
#include "sea_rope.h"
#include "sea_memory.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* sea_lib_time.c: a Time value as text, or NULL */
SeaString *sea_time_show(SeaValue v);

#define SHOW_MAX_ITEMS 100
#define SHOW_MAX_STACK 64

typedef struct {
    SeaVM     *vm;
    char      *buf;
    size_t     len, cap;
    int        max_depth;
    SeaObject *stack[SHOW_MAX_STACK];
    int        sp;
} Show;

static void out_n(Show *S, const char *s, size_t n) {
    if (S->len + n + 1 > S->cap) {
        size_t nc = S->cap ? S->cap * 2 : 128;
        while (nc < S->len + n + 1) nc *= 2;
        S->buf = (char*)sea_realloc(S->buf, S->cap, nc);
        S->cap = nc;
    }
    memcpy(S->buf + S->len, s, n);
    S->len += n;
    S->buf[S->len] = '\0';
}

static void out(Show *S, const char *s) { out_n(S, s, strlen(s)); }

static void out_quoted(Show *S, const char *s, size_t n) {
    out(S, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  out(S, "\\\""); break;
            case '\\': out(S, "\\\\"); break;
            case '\n': out(S, "\\n"); break;
            case '\t': out(S, "\\t"); break;
            case '\r': out(S, "\\r"); break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char hex[8];
                    snprintf(hex, sizeof hex, "\\x%02X", c);
                    out(S, hex);
                } else {
                    out_n(S, (const char*)&s[i], 1);
                }
        }
    }
    out(S, "\"");
}

static const char *const RESERVED[] = {
    "and", "break", "case", "const", "continue", "default", "defer", "del", "do", "else",
    "elseif", "end", "false", "for", "function", "go", "if", "in", "local", "nil", "not",
    "or", "panic", "return", "select", "then", "true", "while", NULL,
};

static bool is_name(const char *s, size_t n) {
    if (n == 0 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_')) return false;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    for (int i = 0; RESERVED[i]; i++)
        if (strlen(RESERVED[i]) == n && memcmp(RESERVED[i], s, n) == 0) return false;
    return true;
}

static void show(Show *S, SeaValue v, int depth, bool nested);

/* A scalar or an object without contents, as tostring shows it */
static void show_plain(Show *S, SeaValue v, bool nested) {
    if (nested && (sea_is_string(v) || sea_is_rope(v))) {
        SeaString *s = sea_vm_tostring(S->vm, v);
        out_quoted(S, s->data, s->len);
        sea_obj_release(&s->obj);
        return;
    }
    SeaString *s = sea_vm_tostring(S->vm, v);
    out_n(S, s->data, s->len);
    sea_obj_release(&s->obj);
}

/* Order of keys: numbers by value, then strings by bytes, then bools, then the rest */
static int key_rank(SeaValue k) {
    if (sea_is_int(k) || sea_is_number(k)) return 0;
    if (sea_is_string(k)) return 1;
    if (sea_is_bool(k)) return 2;
    return 3;
}

static int key_cmp(const void *pa, const void *pb) {
    SeaValue a = ((const SeaValue*)pa)[0], b = ((const SeaValue*)pb)[0];
    int ra = key_rank(a), rb = key_rank(b);
    if (ra != rb) return ra - rb;
    if (ra == 0) {
        double x = sea_is_int(a) ? (double)sea_as_int(a) : sea_as_number(a);
        double y = sea_is_int(b) ? (double)sea_as_int(b) : sea_as_number(b);
        return x < y ? -1 : x > y;
    }
    if (ra == 1) {
        SeaString *x = SEA_AS_STRING(a), *y = SEA_AS_STRING(b);
        size_t n = x->len < y->len ? x->len : y->len;
        int c = memcmp(x->data, y->data, n);
        if (c) return c;
        return x->len < y->len ? -1 : x->len > y->len;
    }
    if (ra == 2) return (int)sea_as_bool(a) - (int)sea_as_bool(b);
    return a < b ? -1 : a > b;
}

static void show_more(Show *S, uint32_t total, uint32_t shown) {
    if (total > shown) {
        char more[48];
        snprintf(more, sizeof more, "%s... (%u more)", shown ? ", " : "", total - shown);
        out(S, more);
    }
}

/* {key = value, ...} or [=key = value, ...] from (key, value) pairs */
static void show_pairs(Show *S, SeaValue *pairs, uint32_t n, bool is_map, int depth) {
    out(S, is_map ? "[=" : "{");
    uint32_t shown = n < SHOW_MAX_ITEMS ? n : SHOW_MAX_ITEMS;
    for (uint32_t i = 0; i < shown; i++) {
        SeaValue k = pairs[2 * i], v = pairs[2 * i + 1];
        if (i) out(S, ", ");
        if (!is_map && sea_is_string(k) && is_name(SEA_AS_STRING(k)->data, SEA_AS_STRING(k)->len)) {
            out_n(S, SEA_AS_STRING(k)->data, SEA_AS_STRING(k)->len);
        } else if (is_map) {
            show(S, k, depth + 1, true);
        } else {
            out(S, "[");
            show(S, k, depth + 1, true);
            out(S, "]");
        }
        out(S, " = ");
        show(S, v, depth + 1, true);
    }
    show_more(S, n, shown);
    out(S, is_map ? "]" : "}");
}

static void show(Show *S, SeaValue v, int depth, bool nested) {
    if (!sea_is_obj(v) || sea_is_string(v) || sea_is_rope(v)) { show_plain(S, v, nested); return; }
    SeaObject *o = sea_as_obj(v);
    int t = o->type;
    if (t != OBJ_ARRAY && t != OBJ_TABLE && t != OBJ_MAP && t != OBJ_STRUCT &&
        t != OBJ_INT_ARRAY && t != OBJ_FLOAT_ARRAY && t != OBJ_FLOAT32_ARRAY) {
        show_plain(S, v, nested);
        return;
    }
    if (t == OBJ_TABLE) {   /* a Time of module time: time(2026-10-18T14:30:00-03:00) */
        SeaString *ts = sea_time_show(v);
        if (ts) {
            out_n(S, ts->data, ts->len);
            sea_obj_release(&ts->obj);
            return;
        }
    }
    for (int i = 0; i < S->sp; i++)
        if (S->stack[i] == o) { out(S, "<cycle>"); return; }
    bool brackets = t == OBJ_ARRAY || t == OBJ_INT_ARRAY || t == OBJ_FLOAT_ARRAY || t == OBJ_FLOAT32_ARRAY;
    if (depth >= S->max_depth || S->sp >= SHOW_MAX_STACK) {
        out(S, t == OBJ_MAP ? "[=...]" : brackets ? "[...]" : "{...}");
        return;
    }
    S->stack[S->sp++] = o;

    if (t == OBJ_ARRAY) {
        SeaArray *a = (SeaArray*)o;
        uint32_t n = a->len, shown = n < SHOW_MAX_ITEMS ? n : SHOW_MAX_ITEMS;
        /* "[[" would read as a long string: [ [1], [2] ], as sea fmt writes it */
        bool pad = n > 0 && (sea_obj_is_type(a->data[0], OBJ_ARRAY) || sea_obj_is_type(a->data[0], OBJ_MAP) ||
                             sea_obj_is_type(a->data[0], OBJ_INT_ARRAY) || sea_obj_is_type(a->data[0], OBJ_FLOAT_ARRAY) ||
                             sea_obj_is_type(a->data[0], OBJ_FLOAT32_ARRAY));
        out(S, pad ? "[ " : "[");
        for (uint32_t i = 0; i < shown; i++) {
            if (i) out(S, ", ");
            show(S, a->data[i], depth + 1, true);
        }
        show_more(S, n, shown);
        out(S, pad ? " ]" : "]");
    } else if (t == OBJ_INT_ARRAY || t == OBJ_FLOAT_ARRAY || t == OBJ_FLOAT32_ARRAY) {
        uint32_t n = t == OBJ_INT_ARRAY ? ((SeaIntArray*)o)->len
                   : t == OBJ_FLOAT_ARRAY ? ((SeaFloatArray*)o)->len : ((SeaFloat32Array*)o)->len;
        uint32_t shown = n < SHOW_MAX_ITEMS ? n : SHOW_MAX_ITEMS;
        out(S, t == OBJ_INT_ARRAY ? "int_array[" : t == OBJ_FLOAT_ARRAY ? "float_array[" : "float32_array[");
        for (uint32_t i = 0; i < shown; i++) {
            if (i) out(S, ", ");
            SeaValue e = t == OBJ_INT_ARRAY ? sea_int_val(((SeaIntArray*)o)->data[i])
                       : t == OBJ_FLOAT_ARRAY ? sea_number_val(((SeaFloatArray*)o)->data[i])
                       : sea_number_val((double)((SeaFloat32Array*)o)->data[i]);
            show_plain(S, e, true);
        }
        show_more(S, n, shown);
        out(S, "]");
    } else if (t == OBJ_STRUCT) {
        SeaStruct *st = (SeaStruct*)o;
        uint32_t n = st->field_count;
        SeaValue *pairs = (SeaValue*)sea_malloc(sizeof(SeaValue) * 2 * (n ? n : 1));
        uint32_t iter = 0, k = 0;
        SeaValue key, val;
        while (k < n && sea_struct_next(st, &iter, &key, &val)) {
            pairs[2 * k] = key;
            pairs[2 * k + 1] = val;
            k++;
        }
        show_pairs(S, pairs, k, false, depth);
        sea_free(pairs, sizeof(SeaValue) * 2 * (n ? n : 1));
    } else if (t == OBJ_TABLE) {
        SeaTable *tb = (SeaTable*)o;
        uint32_t n = tb->count;
        /* positional ({"a", "b"}: keys 0..n-1) */
        bool positional = n > 0;
        for (uint32_t i = 0; i < n && positional; i++) {
            SeaValue e;
            if (!sea_table_get(tb, sea_int_val(i), &e)) positional = false;
        }
        if (positional) {
            uint32_t shown = n < SHOW_MAX_ITEMS ? n : SHOW_MAX_ITEMS;
            out(S, "{");
            for (uint32_t i = 0; i < shown; i++) {
                SeaValue e;
                sea_table_get(tb, sea_int_val(i), &e);
                if (i) out(S, ", ");
                show(S, e, depth + 1, true);
            }
            show_more(S, n, shown);
            out(S, "}");
        } else {
            SeaValue *pairs = (SeaValue*)sea_malloc(sizeof(SeaValue) * 2 * (n ? n : 1));
            uint32_t iter = 0, k = 0;
            SeaValue key, val;
            while (k < n && sea_table_next(tb, &iter, &key, &val)) {
                pairs[2 * k] = key;
                pairs[2 * k + 1] = val;
                k++;
            }
            qsort(pairs, k, 2 * sizeof(SeaValue), key_cmp);
            show_pairs(S, pairs, k, false, depth);
            sea_free(pairs, sizeof(SeaValue) * 2 * (n ? n : 1));
        }
    } else {   /* OBJ_MAP */
        SeaMap *m = (SeaMap*)o;
        uint32_t n = sea_map_count(m);
        SeaValue *pairs = (SeaValue*)sea_malloc(sizeof(SeaValue) * 2 * (n ? n : 1));
        uint32_t iter = 0, k = 0;
        SeaValue key, val;
        while (k < n && sea_map_next(m, &iter, &key, &val)) {
            pairs[2 * k] = key;
            pairs[2 * k + 1] = val;
            k++;
        }
        qsort(pairs, k, 2 * sizeof(SeaValue), key_cmp);
        show_pairs(S, pairs, k, true, depth);
        sea_free(pairs, sizeof(SeaValue) * 2 * (n ? n : 1));
    }
    S->sp--;
}

static SeaString *finish(Show *S) {
    SeaString *s = sea_string_new(S->buf ? S->buf : "", S->len);
    if (S->buf) sea_free(S->buf, S->cap);
    return s;
}

SeaString *sea_show(SeaVM *vm, SeaValue v, int max_depth) {
    Show S;
    memset(&S, 0, sizeof S);
    S.vm = vm;
    S.max_depth = max_depth > 0 ? max_depth : SEA_SHOW_DEPTH;
    show(&S, v, 0, false);
    return finish(&S);
}

/* ---- dump: indented when a container does not fit in the width ---- */

#define DUMP_WIDTH 80

static void dump(Show *S, SeaValue v, int indent, int depth, bool nested);

static void dump_indent(Show *S, int n) {
    for (int i = 0; i < n; i++) out(S, " ");
}

/* The one-line text of v (full depth, the item limit still applies) */
static SeaString *one_line(Show *S, SeaValue v, bool nested) {
    Show T;
    memset(&T, 0, sizeof T);
    T.vm = S->vm;
    T.max_depth = 32;
    memcpy(T.stack, S->stack, sizeof(SeaObject*) * (size_t)S->sp);
    T.sp = S->sp;
    show(&T, v, 0, nested);
    return finish(&T);
}

static void dump_item(Show *S, SeaValue v, int indent, int depth) {
    dump_indent(S, indent);
    dump(S, v, indent, depth, true);
    out(S, ",\n");
}

static void dump(Show *S, SeaValue v, int indent, int depth, bool nested) {
    SeaString *line = one_line(S, v, nested);
    bool container = sea_is_obj(v) && !sea_is_string(v) && !sea_is_rope(v) &&
                     (sea_obj_is_type(v, OBJ_ARRAY) || sea_obj_is_type(v, OBJ_TABLE) ||
                      sea_obj_is_type(v, OBJ_MAP) || sea_obj_is_type(v, OBJ_STRUCT));
    if (!container || (int)line->len + indent <= DUMP_WIDTH || depth >= 32 ||
        strcmp(line->data, "<cycle>") == 0) {
        out_n(S, line->data, line->len);
        sea_obj_release(&line->obj);
        return;
    }
    sea_obj_release(&line->obj);
    SeaObject *o = sea_as_obj(v);
    S->stack[S->sp++] = o;
    int in = indent + 4;
    if (o->type == OBJ_ARRAY) {
        SeaArray *a = (SeaArray*)o;
        out(S, "[\n");
        uint32_t shown = a->len < SHOW_MAX_ITEMS ? a->len : SHOW_MAX_ITEMS;
        bool flat = true;   /* only scalars: fill the lines, as pprint(compact=True) */
        for (uint32_t i = 0; i < shown && flat; i++) {
            SeaValue e = a->data[i];
            if (sea_is_obj(e) && !sea_is_string(e) && !sea_is_rope(e) &&
                (sea_obj_is_type(e, OBJ_ARRAY) || sea_obj_is_type(e, OBJ_TABLE) || sea_obj_is_type(e, OBJ_MAP) ||
                 sea_obj_is_type(e, OBJ_STRUCT) || sea_obj_is_type(e, OBJ_INT_ARRAY) || sea_obj_is_type(e, OBJ_FLOAT_ARRAY) ||
                 sea_obj_is_type(e, OBJ_FLOAT32_ARRAY)))
                flat = false;
        }
        if (flat) {
            int col = 0;
            for (uint32_t i = 0; i < shown; i++) {
                SeaString *item = one_line(S, a->data[i], true);
                if (col > 0 && col + 1 + (int)item->len + 1 > DUMP_WIDTH) {
                    out(S, "\n");
                    col = 0;
                }
                if (col == 0) {
                    dump_indent(S, in);
                    col = in;
                } else {
                    out(S, " ");
                    col++;
                }
                out_n(S, item->data, item->len);
                out(S, ",");
                col += (int)item->len + 1;
                sea_obj_release(&item->obj);
            }
            out(S, "\n");
        } else {
            for (uint32_t i = 0; i < shown; i++) dump_item(S, a->data[i], in, depth + 1);
        }
        if (a->len > shown) {
            char more[48];
            dump_indent(S, in);
            snprintf(more, sizeof more, "... (%u more)\n", a->len - shown);
            out(S, more);
        }
        dump_indent(S, indent);
        out(S, "]");
    } else {
        /* the pairs, as show orders them */
        bool is_map = o->type == OBJ_MAP;
        uint32_t n = o->type == OBJ_TABLE ? ((SeaTable*)o)->count :
                     o->type == OBJ_STRUCT ? ((SeaStruct*)o)->field_count : sea_map_count((SeaMap*)o);
        SeaValue *pairs = (SeaValue*)sea_malloc(sizeof(SeaValue) * 2 * (n ? n : 1));
        uint32_t iter = 0, k = 0;
        SeaValue key, val;
        while (k < n) {
            bool more = o->type == OBJ_TABLE ? sea_table_next((SeaTable*)o, &iter, &key, &val) :
                        o->type == OBJ_STRUCT ? sea_struct_next((SeaStruct*)o, &iter, &key, &val) :
                        sea_map_next((SeaMap*)o, &iter, &key, &val);
            if (!more) break;
            pairs[2 * k] = key;
            pairs[2 * k + 1] = val;
            k++;
        }
        if (o->type != OBJ_STRUCT) qsort(pairs, k, 2 * sizeof(SeaValue), key_cmp);
        out(S, is_map ? "[=\n" : "{\n");
        uint32_t shown = k < SHOW_MAX_ITEMS ? k : SHOW_MAX_ITEMS;
        for (uint32_t i = 0; i < shown; i++) {
            SeaValue kk = pairs[2 * i];
            dump_indent(S, in);
            if (!is_map && sea_is_string(kk) && is_name(SEA_AS_STRING(kk)->data, SEA_AS_STRING(kk)->len)) {
                out_n(S, SEA_AS_STRING(kk)->data, SEA_AS_STRING(kk)->len);
            } else {
                if (!is_map) out(S, "[");
                show(S, kk, depth + 1, true);
                if (!is_map) out(S, "]");
            }
            out(S, " = ");
            dump(S, pairs[2 * i + 1], in, depth + 1, true);
            out(S, ",\n");
        }
        if (k > shown) {
            char more[48];
            dump_indent(S, in);
            snprintf(more, sizeof more, "... (%u more)\n", k - shown);
            out(S, more);
        }
        sea_free(pairs, sizeof(SeaValue) * 2 * (n ? n : 1));
        dump_indent(S, indent);
        out(S, is_map ? "]" : "}");
    }
    S->sp--;
}

SeaString *sea_dump(SeaVM *vm, SeaValue v) {
    Show S;
    memset(&S, 0, sizeof S);
    S.vm = vm;
    S.max_depth = 32;
    dump(&S, v, 0, 0, true);
    return finish(&S);
}
