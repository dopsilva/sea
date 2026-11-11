/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_table.h"
#include "sea_bigint.h"
#include <sched.h>
#include <stdlib.h>
#include "sea_rope.h"
#include "sea_string.h"
#include "sea_object.h"

/* Retain/release helpers for SeaValue */
SEA_INLINE void val_retain(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_retain(sea_as_obj(v));
}
SEA_INLINE void val_release(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

/*
 * Hash table implementation using open addressing + linear probing.
 * Load factor kept below 75%.
 */

// Tombstone: a special deleted marker distinct from NIL
// We use the TRUE value with a special tag as tombstone since we never
// use that as a key
#define TOMBSTONE_VAL  (SEA_NAN_BASE | SEA_TAG_NIL | UINT64_C(1))

static bool is_empty(SeaValue v) {
    return v == SEA_NIL_VAL;
}

static bool is_tombstone(SeaValue v) {
    return v == TOMBSTONE_VAL;
}

static bool key_equal(SeaValue a, SeaValue b) {
    /* Identical bits: same int/bool, or the same object — the common case for
     * interned string keys (constants, field names). NaN keys can't occur. */
    if (a == b) return true;
    if (sea_is_number(a) && sea_is_number(b))
        return sea_as_number(a) == sea_as_number(b);
    if (sea_is_int(a) && sea_is_int(b))
        return sea_as_int(a) == sea_as_int(b);
    // Number/int cross comparison
    if (sea_is_int(a) && sea_is_number(b))
        return (double)sea_as_int(a) == sea_as_number(b);
    if (sea_is_number(a) && sea_is_int(b))
        return sea_as_number(a) == (double)sea_as_int(b);
    // String comparison
    if (sea_is_string(a) && sea_is_string(b))
        return sea_string_equal(SEA_AS_STRING(a), SEA_AS_STRING(b));
    if (sea_is_bigint(a) && sea_is_bigint(b))     /* bigint keys by value (bigint(5) and 5 differ) */
        return sea_bigint_cmp(SEA_AS_BIGINT(a), SEA_AS_BIGINT(b)) == 0;
    return a == b;
}

static uint32_t hash_value(SeaValue v) {
    if (sea_is_number(v)) {
        double n = sea_as_number(v);
        // Mix the bits
        uint64_t bits;
        memcpy(&bits, &n, sizeof(bits));
        bits ^= bits >> 33;
        bits *= 0xff51afd7ed558ccdULL;
        bits ^= bits >> 33;
        return (uint32_t)(bits ^ (bits >> 32));
    }
    if (sea_is_int(v)) {
        int64_t i = sea_as_int(v);
        uint64_t u = (uint64_t)i;
        u ^= u >> 33;
        u *= 0xff51afd7ed558ccdULL;
        u ^= u >> 33;
        return (uint32_t)(u ^ (u >> 32));
    }
    if (sea_is_string(v)) {
        SeaString *s = SEA_AS_STRING(v);
        if (SEA_UNLIKELY(s->hash & SEA_HASH_UNCOMPUTED))
            sea_string_ensure_hash(s);   // compute lazily if needed
        return sea_string_pure_hash(s);
    }
    if (sea_is_bigint(v)) return sea_bigint_hash(SEA_AS_BIGINT(v));
    // For objects and booleans, use the raw bits
    return (uint32_t)(v ^ (v >> 32));
}

static SeaTableEntry* find_entry(SeaTableEntry *entries, uint32_t cap, SeaValue key) {
    uint32_t idx = hash_value(key) & (cap - 1);
    SeaTableEntry *tombstone = NULL;
    for (;;) {
        SeaTableEntry *e = &entries[idx];
        if (is_empty(e->key)) {
            return tombstone ? tombstone : e;
        }
        if (is_tombstone(e->key)) {
            if (!tombstone) tombstone = e;
        } else if (key_equal(e->key, key)) {
            return e;
        }
        idx = (idx + 1) & (cap - 1);
    }
}

static void table_resize(SeaTable *t, uint32_t new_cap) {
    SeaTableEntry *new_entries = SEA_ALLOC_N(SeaTableEntry, new_cap);
    for (uint32_t i = 0; i < new_cap; i++) {
        new_entries[i].key = SEA_NIL_VAL;
        new_entries[i].value = SEA_NIL_VAL;
    }
    // Re-insert all non-tombstone entries
    t->count = 0;
    for (uint32_t i = 0; i < t->cap; i++) {
        SeaTableEntry *e = &t->entries[i];
        if (is_empty(e->key) || is_tombstone(e->key)) continue;
        SeaTableEntry *dest = find_entry(new_entries, new_cap, e->key);
        dest->key = e->key;
        dest->value = e->value;
        t->count++;
    }
    if (t->entries) {
        sea_free(t->entries, sizeof(SeaTableEntry) * t->cap);
    }
    t->entries = new_entries;
    t->cap = new_cap;
}

SeaTable* sea_table_new(SeaTableMode mode) {
    SeaTable *t = SEA_ALLOC(SeaTable);
    t->obj.type = OBJ_TABLE;
    t->obj.gc = 0;
    t->obj.refcount = 1;
    t->mode = mode;
    t->count = 0;
    t->cap = 8;
    t->entries = SEA_ALLOC_N(SeaTableEntry, t->cap);
    for (uint32_t i = 0; i < t->cap; i++) {
        t->entries[i].key = SEA_NIL_VAL;
        t->entries[i].value = SEA_NIL_VAL;
    }
    return t;
}

bool sea_table_get(SeaTable *t, SeaValue key, SeaValue *out) {
    if (t->count == 0) return false;
    if (SEA_UNLIKELY(sea_is_rope(key))) key = sea_rope_key(key);
    SeaTableEntry *e = find_entry(t->entries, t->cap, key);
    if (is_empty(e->key) || is_tombstone(e->key)) return false;
    *out = e->value;
    return true;
}

bool sea_table_get_ic_slow(SeaTable *t, SeaValue key, SeaTableIC *ic, SeaValue *out) {
    if (t->count == 0) return false;
    if (SEA_UNLIKELY(sea_is_rope(key))) key = sea_rope_key(key);
    SeaTableEntry *e = find_entry(t->entries, t->cap, key);
    if (is_empty(e->key) || is_tombstone(e->key)) return false;
    __atomic_store_n(&ic->entries, t->entries, __ATOMIC_RELAXED);
    __atomic_store_n(&ic->idx, (uint32_t)(e - t->entries), __ATOMIC_RELAXED);
    *out = e->value;
    return true;
}

static void table_set(SeaTable *t, SeaValue key, SeaValue val, bool retain_val) {
    if (SEA_UNLIKELY(sea_is_rope(key))) key = sea_rope_key(key);
    sea_store_barrier(&t->obj, key);
    sea_store_barrier(&t->obj, val);
    SeaTableEntry *e = t->cap ? find_entry(t->entries, t->cap, key) : NULL;
    bool is_new = e == NULL || is_empty(e->key) || is_tombstone(e->key);
    // Grow if load factor > 75%, only for a new key: overwriting one must not
    // move the entries (a for-in over the table would see one twice)
    if (is_new && t->count + 1 > t->cap * 3 / 4) {
        table_resize(t, t->cap * 2);
        e = find_entry(t->entries, t->cap, key);
    }

    SeaValue old = SEA_NIL_VAL;
    if (is_new) {
        t->count++;
        val_retain(key);    /* table now owns a ref to the key */
        e->key = key;
    } else {
        /* Overwrite: the old value is released after the store (`t.x = t.x`
         * must not free what it is storing). The stored key stays: `key` may
         * be a different object with equal content that the table doesn't own
         * (assigning it here used to leak the old key and keep an unowned
         * pointer to the new one). */
        old = e->value;
    }
    if (retain_val) val_retain(val);   /* the table owns a ref to the value */
    e->value = val;
    val_release(old);
}

void sea_table_set(SeaTable *t, SeaValue key, SeaValue val) {
    table_set(t, key, val, true);
}

/* As sea_table_set, taking over the caller's reference to val. */
void sea_table_set_move(SeaTable *t, SeaValue key, SeaValue val) {
    table_set(t, key, val, false);
}

/* ---- Lock-free readers, locked writers (see SeaTableSync) ------------- */

void sea_table_sync_init(SeaTableSync *ts) {
    pthread_mutex_init(&ts->mu, NULL);
    ts->seq = 0;
    ts->retired = NULL;
    ts->retired_cap = NULL;
    ts->n_retired = ts->retired_size = 0;
}

void sea_table_sync_destroy(SeaTableSync *ts) {
    for (uint32_t i = 0; i < ts->n_retired; i++)
        sea_free(ts->retired[i], sizeof(SeaTableEntry) * ts->retired_cap[i]);
    free(ts->retired);
    free(ts->retired_cap);
    ts->retired = NULL;
    ts->retired_cap = NULL;
    ts->n_retired = ts->retired_size = 0;
    pthread_mutex_destroy(&ts->mu);
}

static void sync_retire(SeaTableSync *ts, SeaTableEntry *entries, uint32_t cap) {
    if (ts->n_retired == ts->retired_size) {
        uint32_t n = ts->retired_size ? ts->retired_size * 2 : 8;
        ts->retired = realloc(ts->retired, n * sizeof *ts->retired);
        ts->retired_cap = realloc(ts->retired_cap, n * sizeof *ts->retired_cap);
        ts->retired_size = n;
    }
    ts->retired[ts->n_retired] = entries;
    ts->retired_cap[ts->n_retired] = cap;
    ts->n_retired++;
}

static inline uint32_t seq_begin(SeaTableSync *ts) {
    uint32_t s = ts->seq;
    __atomic_store_n(&ts->seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return s;
}

static inline void seq_end(SeaTableSync *ts, uint32_t s) {
    __atomic_store_n(&ts->seq, s + 2, __ATOMIC_RELEASE);
}

void sea_table_set_sync(SeaTable *t, SeaTableSync *ts, SeaValue key, SeaValue val) {
    if (SEA_UNLIKELY(sea_is_rope(key))) key = sea_rope_key(key);
    sea_store_barrier(&t->obj, key);
    sea_store_barrier(&t->obj, val);
    val_retain(val);                       /* the table owns a ref to the value */
    SeaValue old = SEA_NIL_VAL;

    pthread_mutex_lock(&ts->mu);
    SeaTableEntry *e = find_entry(t->entries, t->cap, key);
    if (!is_empty(e->key) && !is_tombstone(e->key)) {
        old = e->value;                    /* released after the store */
        uint32_t s = seq_begin(ts);
        __atomic_store_n(&e->value, val, __ATOMIC_RELAXED);
        seq_end(ts, s);
    } else if (t->count + 1 > t->cap * 3 / 4) {
        /* Grow into an array nobody sees yet, then publish entries before cap. */
        val_retain(key);
        uint32_t new_cap = t->cap * 2;
        SeaTableEntry *ne = SEA_ALLOC_N(SeaTableEntry, new_cap);
        for (uint32_t i = 0; i < new_cap; i++) {
            ne[i].key = SEA_NIL_VAL;
            ne[i].value = SEA_NIL_VAL;
        }
        uint32_t count = 0;
        for (uint32_t i = 0; i < t->cap; i++) {
            SeaTableEntry *src = &t->entries[i];
            if (is_empty(src->key) || is_tombstone(src->key)) continue;
            SeaTableEntry *d = find_entry(ne, new_cap, src->key);
            d->key = src->key;
            d->value = src->value;
            count++;
        }
        SeaTableEntry *d = find_entry(ne, new_cap, key);
        d->key = key;
        d->value = val;
        SeaTableEntry *old_entries = t->entries;
        uint32_t old_cap = t->cap;
        uint32_t s = seq_begin(ts);
        __atomic_store_n(&t->entries, ne, __ATOMIC_RELEASE);
        __atomic_store_n(&t->cap, new_cap, __ATOMIC_RELEASE);
        t->count = count + 1;
        seq_end(ts, s);
        sync_retire(ts, old_entries, old_cap);
    } else {
        val_retain(key);                   /* table now owns a ref to the key */
        uint32_t s = seq_begin(ts);
        __atomic_store_n(&e->value, val, __ATOMIC_RELAXED);
        __atomic_store_n(&e->key, key, __ATOMIC_RELEASE);
        t->count++;
        seq_end(ts, s);
    }
    pthread_mutex_unlock(&ts->mu);
    val_release(old);
}

/* find_entry for a reader racing with sea_table_set_sync: atomic loads, and
 * NULL when the key is absent. Terminates on a torn (old cap, new entries)
 * snapshot too: the new array holds at most 3/4 of the old cap in entries. */
static SeaTableEntry *find_entry_ro(SeaTableEntry *entries, uint32_t cap, SeaValue key) {
    uint32_t idx = hash_value(key) & (cap - 1);
    for (;;) {
        SeaTableEntry *e = &entries[idx];
        SeaValue k = __atomic_load_n(&e->key, __ATOMIC_ACQUIRE);
        if (is_empty(k)) return NULL;
        if (!is_tombstone(k) && key_equal(k, key)) return e;
        idx = (idx + 1) & (cap - 1);
    }
}

bool sea_table_get_sync(SeaTable *t, SeaTableSync *ts, SeaValue key, SeaTableIC *ic, SeaValue *out) {
    if (SEA_UNLIKELY(sea_is_rope(key))) key = sea_rope_key(key);
    for (unsigned spins = 0;; spins++) {
        uint32_t s1 = __atomic_load_n(&ts->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1) {
            if (spins > 64) sched_yield();
            continue;
        }
        uint32_t cap = __atomic_load_n(&t->cap, __ATOMIC_ACQUIRE);
        SeaTableEntry *entries = __atomic_load_n(&t->entries, __ATOMIC_RELAXED);
        SeaTableEntry *e = find_entry_ro(entries, cap, key);
        SeaValue v = e ? __atomic_load_n(&e->value, __ATOMIC_RELAXED) : SEA_NIL_VAL;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&ts->seq, __ATOMIC_RELAXED) != s1) continue;
        if (!e) return false;
        if (ic) {
            __atomic_store_n(&ic->entries, entries, __ATOMIC_RELAXED);
            __atomic_store_n(&ic->idx, (uint32_t)(e - entries), __ATOMIC_RELAXED);
        }
        *out = v;
        return true;
    }
}

bool sea_table_delete(SeaTable *t, SeaValue key) {
    if (t->count == 0) return false;
    if (SEA_UNLIKELY(sea_is_rope(key))) key = sea_rope_key(key);
    SeaTableEntry *e = find_entry(t->entries, t->cap, key);
    if (is_empty(e->key) || is_tombstone(e->key)) return false;
    val_release(e->key);
    val_release(e->value);
    e->key = TOMBSTONE_VAL;
    e->value = SEA_NIL_VAL;
    t->count--;
    return true;
}

bool sea_table_next(SeaTable *t, uint32_t *iter, SeaValue *key, SeaValue *val) {
    while (*iter < t->cap) {
        SeaTableEntry *e = &t->entries[(*iter)++];
        if (!is_empty(e->key) && !is_tombstone(e->key)) {
            *key = e->key;
            *val = e->value;
            return true;
        }
    }
    return false;
}

void sea_table_merge(SeaTable *dst, SeaTable *src) {
    for (uint32_t i = 0; i < src->cap; i++) {
        SeaTableEntry *e = &src->entries[i];
        if (!is_empty(e->key) && !is_tombstone(e->key)) {
            sea_table_set(dst, e->key, e->value);
        }
    }
}

uint32_t sea_table_count(SeaTable *t) {
    return t->count;
}

void sea_table_destroy(SeaTable *t) {
    if (t->entries) {
        for (uint32_t i = 0; i < t->cap; i++) {
            SeaTableEntry *e = &t->entries[i];
            if (!is_empty(e->key) && !is_tombstone(e->key)) {
                val_release(e->key);
                val_release(e->value);
            }
        }
        sea_free(t->entries, sizeof(SeaTableEntry) * t->cap);
        t->entries = NULL;
    }
    t->count = 0;
    t->cap = 0;
}

/* Free the entry array without releasing keys/values (the cycle collector
 * accounts for them itself). */
void sea_table_free_storage(SeaTable *t) {
    if (t->entries) sea_free(t->entries, sizeof(SeaTableEntry) * t->cap);
    t->entries = NULL;
    t->count = t->cap = 0;
}

void sea_table_set_str(SeaTable *t, const char *key, SeaValue val) {
    SeaString *s = sea_string_intern(key, strlen(key));
    sea_table_set(t, sea_obj_val(&s->obj), val);
    // Release since intern added a ref and table holds the value
    sea_obj_release(&s->obj);
}

bool sea_table_get_str(SeaTable *t, const char *key, SeaValue *out) {
    SeaString *s = sea_string_intern(key, strlen(key));
    bool found = sea_table_get(t, sea_obj_val(&s->obj), out);
    sea_obj_release(&s->obj);
    return found;
}

void sea_table_add_lowercase_aliases(SeaTable *t) {
    /* Coleta os pares a adicionar em buffer local para não modificar a tabela
     * enquanto a iteramos (evita invalidar ponteiros internos do hash). */
    uint32_t iter = 0;
    SeaValue k, v;

    /* Primeira passagem: conta quantos aliases serão necessários */
    uint32_t n = 0;
    while (sea_table_next(t, &iter, &k, &v)) {
        if (!sea_is_string(k)) continue;
        const char *s = SEA_AS_STRING(k)->data;
        if (s[0] >= 'A' && s[0] <= 'Z') n++;
    }
    if (n == 0) return;

    typedef struct { SeaString *key; SeaValue val; } Pair;
    Pair *pairs = sea_malloc(n * sizeof(Pair));
    if (!pairs) return;

    uint32_t pi = 0;
    iter = 0;
    while (sea_table_next(t, &iter, &k, &v)) {
        if (!sea_is_string(k)) continue;
        SeaString *orig = SEA_AS_STRING(k);
        const char *s = orig->data;
        if (!(s[0] >= 'A' && s[0] <= 'Z')) continue;

        size_t len = orig->len;
        char *lower = sea_malloc(len + 1);
        if (!lower) continue;
        for (size_t i = 0; i < len; i++)
            lower[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
        lower[len] = '\0';

        /* Só adiciona se a chave minúscula ainda não existe */
        SeaValue existing;
        if (!sea_table_get_str(t, lower, &existing)) {
            pairs[pi].key = sea_string_intern(lower, len);
            pairs[pi].val = v;
            pi++;
        }
        sea_free(lower, len + 1);
    }

    for (uint32_t i = 0; i < pi; i++) {
        sea_table_set(t, sea_obj_val(&pairs[i].key->obj), pairs[i].val);
        sea_obj_release(&pairs[i].key->obj);
    }
    sea_free(pairs, n * sizeof(Pair));
}
