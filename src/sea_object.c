/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_object.h"
#include "sea_string.h"
#include "sea_rope.h"
#include "sea_array.h"
#include "sea_table.h"
#include "sea_map.h"
#include "sea_vm.h"
#include "sea_channel.h"
#include "sea_goroutine.h"
#include "sea_memory.h"
#include "sea_struct.h"
#include "sea_typed_array.h"
#include "sea_gc.h"
#include "sea_bigint.h"

// These are defined in their respective files; we need them for freeing
extern void sea_array_destroy(SeaArray *a);
extern void sea_table_destroy(SeaTable *t);
extern void sea_map_destroy(SeaMap *m);
extern void sea_string_remove_intern(SeaString *s);

/* Provide out-of-line copies so dlopen'd native .so libraries can link them. */
extern inline void sea_obj_retain(SeaObject *obj);
extern inline void sea_obj_release(SeaObject *obj);

void sea_obj_mark_shared(SeaObject *obj) {
    if (!obj) return;
    if (__atomic_load_n(&obj->refcount, __ATOMIC_RELAXED) & SEA_RC_SHARED_BIT)
        return; /* already shared; subgraph also marked */
    /* In this goroutine's cycle buffer: once shared, another goroutine could
     * drop it to zero while the buffer still points at it — so the buffer
     * takes a reference (released when the collector discards the entry). */
    if (obj->gc & SEA_GC_BUFFERED) {
        obj->gc |= SEA_GC_BUFREF;
        obj->refcount++;
    }
    __atomic_fetch_or(&obj->refcount, SEA_RC_SHARED_BIT, __ATOMIC_RELEASE);

    switch (obj->type) {
        case OBJ_STRUCT: {
            SeaStruct *s = (SeaStruct*)obj;
            SeaValue *vals = sea_struct_values(s);
            for (uint32_t i = 0; i < s->field_count; i++)
                if (sea_is_obj(vals[i])) sea_obj_mark_shared(sea_as_obj(vals[i]));
            break;
        }
        case OBJ_ARRAY: {
            SeaArray *a = (SeaArray*)obj;
            for (uint32_t i = 0; i < a->len; i++)
                if (sea_is_obj(a->data[i])) sea_obj_mark_shared(sea_as_obj(a->data[i]));
            break;
        }
        case OBJ_TABLE: {
            SeaTable *t = (SeaTable*)obj;
            uint32_t iter = 0;
            SeaValue k, v;
            while (sea_table_next(t, &iter, &k, &v)) {
                if (sea_is_obj(k)) sea_obj_mark_shared(sea_as_obj(k));
                if (sea_is_obj(v)) sea_obj_mark_shared(sea_as_obj(v));
            }
            break;
        }
        case OBJ_MAP: {
            SeaMap *m = (SeaMap*)obj;
            uint32_t iter = 0;
            SeaValue k, v;
            while (sea_map_next(m, &iter, &k, &v)) {
                if (sea_is_obj(k)) sea_obj_mark_shared(sea_as_obj(k));
                if (sea_is_obj(v)) sea_obj_mark_shared(sea_as_obj(v));
            }
            break;
        }
        case OBJ_CLOSURE: {
            SeaClosure *cl = (SeaClosure*)obj;
            for (int i = 0; i < cl->upval_count; i++)
                if (cl->upvalues[i]) sea_obj_mark_shared(&cl->upvalues[i]->obj);
            break;
        }
        case OBJ_UPVALUE: {
            /* Open or closed: an open upvalue points at a live register of the
             * capturing frame, and the goroutine reaches that value through it
             * just the same. (Only marking closed ones left e.g. a waitgroup
             * held in a still-running main-chunk local unshared, so its
             * refcounts raced across scheduler threads.) */
            SeaUpvalue *up = (SeaUpvalue*)obj;
            SeaValue v = sea_upvalue_get(up);
            if (sea_is_obj(v)) sea_obj_mark_shared(sea_as_obj(v));
            break;
        }
        case OBJ_ROPE: {
            SeaRope *r = (SeaRope*)obj;
            for (uint32_t i = 0; i < r->count; i++)
                sea_obj_mark_shared(&r->parts[i]->obj);
            break;
        }
        case OBJ_ERROR: {
            SeaValue m = ((SeaError*)obj)->message;
            if (sea_is_obj(m)) sea_obj_mark_shared(sea_as_obj(m));
            break;
        }
        default:
            break;
    }
}

void sea_obj_arena_teardown(SeaObject *obj) {
    switch (obj->type) {
    case OBJ_ARRAY:
        sea_array_destroy((SeaArray*)obj);
        break;
    case OBJ_STRUCT: {
        SeaStruct *st = (SeaStruct*)obj;
        SeaValue *vals = sea_struct_values(st);
        for (uint32_t j = 0; j < st->field_count; j++) {
            SeaValue fv = vals[j];
            vals[j] = SEA_NIL_VAL;
            if (sea_is_obj(fv)) sea_obj_release(sea_as_obj(fv));
        }
        break;
    }
    default:
        break;
    }
}

void sea_obj_free(SeaObject *obj) {
    if (!obj) return;
    if (SEA_UNLIKELY(obj->gc & SEA_GC_BUFFERED) && !sea_gc_unbuffer_dying(obj)) {
        /* in a cycle buffer at an unknown slot: release the contents now,
         * leave the header to the collector (sea_gc_collect) */
        sea_gc_release_contents(obj);
        obj->gc |= SEA_GC_DEAD;
        return;
    }
    switch (obj->type) {
        case OBJ_STRING: {
            SeaString *s = (SeaString*)obj;
            if (sea_string_is_interned(s)) sea_string_remove_intern(s);
            sea_free(obj, sizeof(SeaString) + s->len + 1);
            break;
        }
        case OBJ_ARRAY: {
            SeaArray *a = (SeaArray*)obj;
            sea_array_destroy(a);
            SEA_FREE(a, SeaArray);
            break;
        }
        case OBJ_TABLE: {
            SeaTable *t = (SeaTable*)obj;
            sea_table_destroy(t);
            SEA_FREE(t, SeaTable);
            break;
        }
        case OBJ_CLOSURE: {
            SeaClosure *cl = (SeaClosure*)obj;
            /* Release all captured upvalues */
            for (int j = 0; j < cl->upval_count; j++) {
                if (cl->upvalues[j]) {
                    sea_obj_release(&cl->upvalues[j]->obj);
                }
            }
            if (cl->upvalues) {
                sea_free(cl->upvalues, sizeof(SeaUpvalue*) * cl->upval_count);
            }
            /* Note: cl->chunk is NOT owned by the closure (it's in the proto pool) */
            sea_free(obj, sizeof(SeaClosure));
            break;
        }
        case OBJ_UPVALUE: {
            SeaUpvalue *up = (SeaUpvalue*)obj;
            /* If closed, we own the value — release it. */
            if (up->location == &up->closed && sea_is_obj(up->closed)) {
                sea_obj_release(sea_as_obj(up->closed));
            }
            sea_free(obj, sizeof(SeaUpvalue));
            break;
        }
        case OBJ_ERROR: {
            SeaValue m = ((SeaError*)obj)->message;
            if (sea_is_obj(m)) sea_obj_release(sea_as_obj(m));
            sea_free(obj, sizeof(SeaError));
            break;
        }
        case OBJ_CHANNEL: {
            sea_chan_destroy((SeaChannel*)obj);   /* buffer, buffered values, locks */
            sea_free(obj, sizeof(SeaChannel));
            break;
        }
        case OBJ_GOROUTINE: {
            sea_free(obj, sizeof(SeaGoroutine));
            break;
        }
        case OBJ_MUTEX: {
            sea_free(obj, 64); // approximate; actual size in sea_lib_sync.c
            break;
        }
        case OBJ_WAITGROUP: {
            sea_free(obj, 64); // approximate
            break;
        }
        case OBJ_MAP: {
            SeaMap *m = (SeaMap*)obj;
            sea_map_destroy(m);
            SEA_FREE(m, SeaMap);
            break;
        }
        case OBJ_ROPE: {
            sea_rope_free((SeaRope*)obj);
            break;
        }
        case OBJ_STRUCT: {
            sea_struct_free((SeaStruct*)obj);
            return; /* sea_struct_free frees the object itself */
        }
        case OBJ_INT_ARRAY: {
            sea_int_array_free((SeaIntArray*)obj);
            return;
        }
        case OBJ_FLOAT32_ARRAY: {
            sea_float32_array_free((SeaFloat32Array*)obj);
            break;
        }
        case OBJ_FLOAT_ARRAY: {
            sea_float_array_free((SeaFloatArray*)obj);
            return;
        }
        case OBJ_BIGINT:
            sea_free(obj, sea_bigint_size((SeaBigInt*)obj));
            break;
        default:
            sea_free(obj, sizeof(SeaObject));
            break;
    }
}

const char* sea_type_name(SeaValue v) {
    if (sea_is_nil(v))    return "nil";
    if (sea_is_bool(v))   return "bool";
    if (sea_is_int(v))    return "int";
    if (sea_is_number(v)) return "number";
    if (!sea_is_obj(v))   return "unknown";
    switch (sea_as_obj(v)->type) {
        case OBJ_STRING:    return "string";
        case OBJ_ARRAY:     return "array";
        case OBJ_TABLE:     return "table";
        case OBJ_FUNCTION:  return "function";
        case OBJ_CLOSURE:   return "function";
        case OBJ_UPVALUE:   return "upvalue";
        case OBJ_ERROR:     return "error";
        case OBJ_CHANNEL:   return "channel";
        case OBJ_GOROUTINE: return "goroutine";
        case OBJ_MUTEX:     return "mutex";
        case OBJ_WAITGROUP: return "waitgroup";
        case OBJ_MAP:         return "map";
        case OBJ_ROPE:        return "string";
        case OBJ_STRUCT:      return "struct";
        case OBJ_INT_ARRAY:   return "int_array";
        case OBJ_FLOAT_ARRAY: return "float_array";
        case OBJ_FLOAT32_ARRAY: return "float32_array";
        case OBJ_BIGINT:      return "bigint";
        default:              return "object";
    }
}
