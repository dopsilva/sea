/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_map.h"
#include "sea_rope.h"
#include "sea_string.h"
#include "sea_object.h"
#include "sea_bigint.h"

/*
 * Swiss table with SIMD-accelerated probing.
 *
 * Probe-window layout (Abseil/SwissTable style):
 *   - Each probe step loads SMAP_GROUP consecutive ctrl bytes starting at h1.
 *   - ctrl array is allocated as (cap + SMAP_GROUP) bytes.
 *   - ctrl[cap..cap+SMAP_GROUP-1] mirrors ctrl[0..SMAP_GROUP-1], so a load
 *     at any position p in [0, cap-1] that spans the wrap boundary reads the
 *     correct mirrored bytes — no branch needed.
 *   - After writing ctrl[i], if i < SMAP_GROUP, also write ctrl[cap + i]
 *     (ctrl_write helper).
 *   - Probe sequence: pos = h1, h1+G, h1+2G, ... (all mod cap).
 *     Starting at h1 (not a group-aligned boundary) avoids the false-early-
 *     stop bug that occurs when slots before h1 in the same aligned group are
 *     EMPTY.
 */

/* ---- SIMD group width ---- */
#if defined(__AVX2__)
#  include <immintrin.h>
#  define SMAP_GROUP    32u
typedef __m256i smap_vec_t;
static inline smap_vec_t smap_load(const uint8_t *p)          { return _mm256_loadu_si256((const __m256i*)p); }
static inline smap_vec_t smap_set1(uint8_t v)                 { return _mm256_set1_epi8((char)v); }
static inline smap_vec_t smap_cmpeq(smap_vec_t a, smap_vec_t b){ return _mm256_cmpeq_epi8(a, b); }
static inline uint32_t   smap_mask(smap_vec_t v)              { return (uint32_t)_mm256_movemask_epi8(v); }
#elif defined(__SSE2__)
#  include <emmintrin.h>
#  define SMAP_GROUP    16u
typedef __m128i smap_vec_t;
static inline smap_vec_t smap_load(const uint8_t *p)          { return _mm_loadu_si128((const __m128i*)p); }
static inline smap_vec_t smap_set1(uint8_t v)                 { return _mm_set1_epi8((char)v); }
static inline smap_vec_t smap_cmpeq(smap_vec_t a, smap_vec_t b){ return _mm_cmpeq_epi8(a, b); }
static inline uint32_t   smap_mask(smap_vec_t v)              { return (uint32_t)_mm_movemask_epi8(v); }
#elif defined(__aarch64__) && defined(__ARM_NEON)
#  include <arm_neon.h>
#  define SMAP_GROUP    16u
typedef uint8x16_t smap_vec_t;
static inline smap_vec_t smap_load(const uint8_t *p)          { return vld1q_u8(p); }
static inline smap_vec_t smap_set1(uint8_t v)                 { return vdupq_n_u8(v); }
static inline smap_vec_t smap_cmpeq(smap_vec_t a, smap_vec_t b){ return vceqq_u8(a, b); }
/* movemask: one bit per byte (lanes are 0x00 or 0xFF), like _mm_movemask_epi8 */
static inline uint32_t   smap_mask(smap_vec_t v) {
    static const uint8_t bit[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
    uint8x16_t m = vandq_u8(v, vld1q_u8(bit));
    return (uint32_t)vaddv_u8(vget_low_u8(m)) | ((uint32_t)vaddv_u8(vget_high_u8(m)) << 8);
}
#else
#  define SMAP_GROUP    1u
#endif

/* Initial capacity: must be >= SMAP_GROUP and a power of 2 */
#define SMAP_INIT_CAP  (SMAP_GROUP < 16u ? 16u : SMAP_GROUP)

/* ctrl array total allocation: cap + SMAP_GROUP (probe-window mirror) */
#define SMAP_CTRL_SIZE(cap)  ((cap) + SMAP_GROUP)

/* ---- Retain/release helpers ---- */
static inline void val_retain(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_retain(sea_as_obj(v));
}
static inline void val_release(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

/* ---- Key equality ---- */
static bool key_equal(SeaValue a, SeaValue b) {
    if (sea_is_number(a) && sea_is_number(b))
        return sea_as_number(a) == sea_as_number(b);
    if (sea_is_int(a) && sea_is_int(b))
        return sea_as_int(a) == sea_as_int(b);
    if (sea_is_int(a) && sea_is_number(b))
        return (double)sea_as_int(a) == sea_as_number(b);
    if (sea_is_number(a) && sea_is_int(b))
        return sea_as_number(a) == (double)sea_as_int(b);
    if (sea_is_string(a) && sea_is_string(b))
        return sea_string_equal(SEA_AS_STRING(a), SEA_AS_STRING(b));
    if (sea_is_bigint(a) && sea_is_bigint(b))     /* bigint keys by value (bigint(5) and 5 differ) */
        return sea_bigint_cmp(SEA_AS_BIGINT(a), SEA_AS_BIGINT(b)) == 0;
    return a == b;
}

/* ---- 64-bit hash (murmur3 finaliser) ---- */
static uint64_t hash64(SeaValue v) {
    uint64_t h;
    if (sea_is_number(v)) {
        double n = sea_as_number(v);
        memcpy(&h, &n, sizeof(h));
    } else if (sea_is_int(v)) {
        h = (uint64_t)(int64_t)sea_as_int(v);
    } else if (sea_is_string(v)) {
        SeaString *s = SEA_AS_STRING(v);
        sea_string_ensure_hash(s);
        h = (uint64_t)sea_string_pure_hash(s);
    } else if (sea_is_bigint(v)) {
        h = sea_bigint_hash(SEA_AS_BIGINT(v));
    } else {
        h = v;
    }
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    return h;
}

static inline uint8_t  h2_of(uint64_t hash)               { return (uint8_t)(hash & 0x7F); }
static inline uint32_t h1_slot(uint64_t hash, uint32_t cap){ return (uint32_t)((hash >> 7) & (cap - 1)); }

/*
 * Write a ctrl byte and keep the probe-window mirror in sync.
 * Slots 0..SMAP_GROUP-1 are mirrored at cap..cap+SMAP_GROUP-1.
 */
static inline void ctrl_write(uint8_t *ctrl, uint32_t cap, uint32_t idx, uint8_t val) {
    ctrl[idx] = val;
    if (idx < SMAP_GROUP)
        ctrl[cap + idx] = val;
}

/*
 * Probe the table for `key`.
 * Returns the slot index where:
 *   *found = true  → key exists there
 *   *found = false → best insertion slot (tombstone preferred over empty)
 */
static uint32_t smap_probe(SeaMap *m, SeaValue key, uint64_t hash, bool *found) {
    *found = false;

    uint8_t  needle = h2_of(hash);
    uint32_t h1     = h1_slot(hash, m->cap);
    uint32_t pos    = h1;
    uint32_t first_tomb = UINT32_MAX;

#if SMAP_GROUP > 1
    smap_vec_t v_needle = smap_set1(needle);
    smap_vec_t v_empty  = smap_set1(SMAP_CTRL_EMPTY);

    do {
        /* Load SMAP_GROUP ctrl bytes starting at pos.
         * mirror at ctrl[cap..] means loads that span cap wrap correctly. */
        smap_vec_t grp = smap_load(m->ctrl + pos);

        /* Check all SMAP_GROUP ctrl bytes against h2 needle */
        uint32_t match = smap_mask(smap_cmpeq(grp, v_needle));
        while (match) {
            uint32_t bit = (uint32_t)__builtin_ctz(match);
            uint32_t idx = (pos + bit) & (m->cap - 1);
            if (key_equal(m->slots[idx].key, key)) {
                *found = true;
                return idx;
            }
            match &= match - 1;
        }

        /* Any EMPTY in the window → key cannot exist further in probe chain */
        uint32_t empties = smap_mask(smap_cmpeq(grp, v_empty));
        if (empties) {
            if (first_tomb != UINT32_MAX) return first_tomb;
            uint32_t bit = (uint32_t)__builtin_ctz(empties);
            return (pos + bit) & (m->cap - 1);
        }

        /* Record first tombstone for potential insertion */
        if (first_tomb == UINT32_MAX) {
            smap_vec_t v_tomb = smap_set1(SMAP_CTRL_DELETED);
            uint32_t tombs = smap_mask(smap_cmpeq(grp, v_tomb));
            if (tombs) {
                uint32_t bit = (uint32_t)__builtin_ctz(tombs);
                first_tomb = (pos + bit) & (m->cap - 1);
            }
        }

        pos = (pos + SMAP_GROUP) & (m->cap - 1);
    } while (pos != h1);

    return (first_tomb != UINT32_MAX) ? first_tomb : 0;

#else
    /* Scalar fallback — identical to the original linear probe */
    uint32_t idx = h1;
    for (;;) {
        uint8_t c = m->ctrl[idx];
        if (c == SMAP_CTRL_EMPTY)
            return (first_tomb != UINT32_MAX) ? first_tomb : idx;
        if (c == SMAP_CTRL_DELETED) {
            if (first_tomb == UINT32_MAX) first_tomb = idx;
        } else if (c == needle && key_equal(m->slots[idx].key, key)) {
            *found = true;
            return idx;
        }
        idx = (idx + 1) & (m->cap - 1);
    }
#endif
}

static void smap_resize(SeaMap *m, uint32_t new_cap) {
    uint8_t     *nc = (uint8_t*)sea_malloc(SMAP_CTRL_SIZE(new_cap));
    SeaMapEntry *ns = (SeaMapEntry*)sea_malloc(sizeof(SeaMapEntry) * new_cap);
    memset(nc, SMAP_CTRL_EMPTY, SMAP_CTRL_SIZE(new_cap));

    m->count = 0;
    for (uint32_t i = 0; i < m->cap; i++) {
        if (m->ctrl[i] == SMAP_CTRL_EMPTY || m->ctrl[i] == SMAP_CTRL_DELETED)
            continue;
        uint64_t hash = hash64(m->slots[i].key);
        uint32_t idx  = h1_slot(hash, new_cap);
        while (nc[idx] != SMAP_CTRL_EMPTY)
            idx = (idx + 1) & (new_cap - 1);
        ctrl_write(nc, new_cap, idx, h2_of(hash));
        ns[idx] = m->slots[i];
        m->count++;
    }

    sea_free(m->ctrl,  SMAP_CTRL_SIZE(m->cap));
    sea_free(m->slots, sizeof(SeaMapEntry) * m->cap);
    m->ctrl  = nc;
    m->slots = ns;
    m->cap   = new_cap;
}

/* ---- Public API ---- */

SeaMap* sea_map_new(void) {
    SeaMap *m = SEA_ALLOC(SeaMap);
    m->obj.type     = OBJ_MAP;
    m->obj.gc = 0;
    m->obj.refcount = 1;
    m->count = 0;
    m->cap   = SMAP_INIT_CAP;
    m->ctrl  = (uint8_t*)sea_malloc(SMAP_CTRL_SIZE(SMAP_INIT_CAP));
    m->slots = (SeaMapEntry*)sea_malloc(sizeof(SeaMapEntry) * SMAP_INIT_CAP);
    memset(m->ctrl, SMAP_CTRL_EMPTY, SMAP_CTRL_SIZE(SMAP_INIT_CAP));
    return m;
}

bool sea_map_get(SeaMap *m, SeaValue key, SeaValue *out) {
    if (m->count == 0) return false;
    if (SEA_UNLIKELY(sea_is_rope(key))) key = sea_rope_key(key);
    uint64_t hash = hash64(key);
    bool found;
    uint32_t idx = smap_probe(m, key, hash, &found);
    if (!found) return false;
    *out = m->slots[idx].value;
    return true;
}

void sea_map_set(SeaMap *m, SeaValue key, SeaValue val) {
    if (SEA_UNLIKELY(sea_is_rope(key))) key = sea_rope_key(key);
    sea_store_barrier(&m->obj, key);
    sea_store_barrier(&m->obj, val);
    uint64_t hash = hash64(key);
    bool found = false;
    uint32_t idx = m->cap ? smap_probe(m, key, hash, &found) : 0;
    /* Grow only for a new key: overwriting one must not move the slots (a
     * for-in over the map would see one twice) */
    if (!found && m->count + 1 > m->cap * 3 / 4) {
        smap_resize(m, m->cap * 2);
        idx = smap_probe(m, key, hash, &found);
    }

    SeaValue old = SEA_NIL_VAL;
    if (found) {
        old = m->slots[idx].value;   /* released after the store (as tables) */
    } else {
        val_retain(key);
        m->slots[idx].key = key;
        ctrl_write(m->ctrl, m->cap, idx, h2_of(hash));
        m->count++;
    }
    val_retain(val);
    m->slots[idx].value = val;
    val_release(old);
}

bool sea_map_delete(SeaMap *m, SeaValue key) {
    if (m->count == 0) return false;
    if (SEA_UNLIKELY(sea_is_rope(key))) key = sea_rope_key(key);
    uint64_t hash = hash64(key);
    bool found;
    uint32_t idx = smap_probe(m, key, hash, &found);
    if (!found) return false;
    val_release(m->slots[idx].key);
    val_release(m->slots[idx].value);
    ctrl_write(m->ctrl, m->cap, idx, SMAP_CTRL_DELETED);
    m->slots[idx].key   = SEA_NIL_VAL;
    m->slots[idx].value = SEA_NIL_VAL;
    m->count--;
    return true;
}

bool sea_map_next(SeaMap *m, uint32_t *iter, SeaValue *key, SeaValue *val) {
    while (*iter < m->cap) {
        uint32_t i = (*iter)++;
        if (m->ctrl[i] != SMAP_CTRL_EMPTY && m->ctrl[i] != SMAP_CTRL_DELETED) {
            *key = m->slots[i].key;
            *val = m->slots[i].value;
            return true;
        }
    }
    return false;
}

uint32_t sea_map_count(SeaMap *m) { return m->count; }

/* Free the storage without releasing keys/values (cycle collector). */
void sea_map_free_storage(SeaMap *m) {
    if (m->ctrl)  sea_free(m->ctrl,  SMAP_CTRL_SIZE(m->cap));
    if (m->slots) sea_free(m->slots, sizeof(SeaMapEntry) * m->cap);
    m->ctrl = NULL;
    m->slots = NULL;
    m->count = m->cap = 0;
}

void sea_map_destroy(SeaMap *m) {
    if (!m->ctrl) return;
    for (uint32_t i = 0; i < m->cap; i++) {
        if (m->ctrl[i] != SMAP_CTRL_EMPTY && m->ctrl[i] != SMAP_CTRL_DELETED) {
            val_release(m->slots[i].key);
            val_release(m->slots[i].value);
        }
    }
    sea_free(m->ctrl,  SMAP_CTRL_SIZE(m->cap));
    sea_free(m->slots, sizeof(SeaMapEntry) * m->cap);
    m->ctrl  = NULL;
    m->slots = NULL;
    m->count = 0;
    m->cap   = 0;
}
