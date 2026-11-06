/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include "sea_bigint.h"
#include "sea_stdlib.h"
#include "sea_show.h"
#include "sea_string.h"
#include "sea_rope.h"
#include "sea_array.h"
#include "sea_table.h"
#include "sea_map.h"
#include "sea_struct.h"
#include "sea_typed_array.h"
#include "sea_channel.h"
#include "sea_memory.h"
#include "sea_parser.h"
#include "sea_compiler.h"
#include <pthread.h>
#include "sea_ast.h"
#include "sea_chunk.h"
#include "sea_sched.h"
#include "sea_gc.h"
#include <math.h>
#include <unistd.h>
#include <limits.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include "sea_bundle.h"

// ---- Module helpers ----

void mod_set(SeaVM *vm, SeaTable *mod, const char *name,
             SeaValue (*fn)(SeaVM*, int, SeaValue*)) {
    (void)vm;
    sea_mod_set(mod, name, fn);
}

/* Public API: create an empty module table (rc=1). */
SeaTable *sea_mod_new(void) {
    return sea_table_new(TABLE_MAP);
}

/* Public API: register a native function in a module table. */
void sea_mod_set(SeaTable *mod, const char *name,
                 SeaValue (*fn)(SeaVM*, int, SeaValue*)) {
    SeaClosure *cl  = sea_native_closure(name, fn);
    SeaString  *key = sea_string_intern(name, strlen(name));
    sea_table_set(mod, sea_obj_val(&key->obj), sea_obj_val(&cl->obj));
    sea_obj_release(&key->obj);
    sea_obj_release(&cl->obj);
}

// ---- Core library ----

// print(...)
static SeaValue fn_print(SeaVM *vm, int argc, SeaValue *args) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) putchar('\t');
        SeaString *s = sea_vm_tostring(vm, args[i]);
        fwrite(s->data, 1, s->len, stdout);
        sea_obj_release(&s->obj);
    }
    putchar('\n');
    fflush(stdout);
    return SEA_NIL_VAL;
}

// dump(...): each value on its own, containers indented when they don't fit
// in 80 columns, strings quoted
static SeaValue fn_dump(SeaVM *vm, int argc, SeaValue *args) {
    for (int i = 0; i < argc; i++) {
        SeaString *s = sea_dump(vm, args[i]);
        fwrite(s->data, 1, s->len, stdout);
        putchar('\n');
        sea_obj_release(&s->obj);
    }
    if (argc == 0) puts("nil");
    fflush(stdout);
    return SEA_NIL_VAL;
}

// printf(fmt, ...)
static SeaValue fn_printf(SeaVM *vm, int argc, SeaValue *args) {
    SeaValue s = sea_str_format(vm, argc, args);
    if (sea_is_string(s)) {
        fputs(SEA_AS_STRING(s)->data, stdout);
        fflush(stdout);
        sea_obj_release(sea_as_obj(s));
    }
    return SEA_NIL_VAL;
}

// type(v)
static SeaValue fn_type(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_obj_val(&sea_string_from_cstr("nil")->obj);
    const char *name = sea_type_name(args[0]);
    SeaString *s = sea_string_intern(name, strlen(name));
    return sea_obj_val(&s->obj);   /* intern's reference goes to the caller */
}

// tostring(v)
static SeaValue fn_tostring(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) return sea_obj_val(&sea_string_from_cstr("nil")->obj);
    SeaString *s = sea_vm_tostring(vm, args[0]);
    /* Return rc=1; caller owns the reference */
    return sea_obj_val(&s->obj);
}

// tonumber(v)
static SeaValue fn_tonumber(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaValue v = args[0];
    if (sea_is_number(v) || sea_is_int(v)) return v;
    if (sea_is_bigint(v)) return sea_number_val(sea_bigint_to_double(SEA_AS_BIGINT(v)));
    if (sea_is_string(v)) {
        char *end;
        const char *str = SEA_AS_STRING(v)->data;
        // Try integer first (within Sea's 48-bit ints; larger ones become numbers).
        // Decimal unless "0x": leading zeros are not octal ("010" is 10, "08" is 8)
        const char *p = str;
        while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
        if (*p == '+' || *p == '-') p++;
        int base = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ? 16 : 10;
        errno = 0;
        long long iv = strtoll(str, &end, base);
        if (*end == '\0' && end != str && errno == 0 && iv >= -(1LL << 47) && iv < (1LL << 47))
            return sea_int_val((int64_t)iv);
        double n = strtod(str, &end);
        if (*end == '\0' && end != str) return sea_number_val(n);
    }
    return SEA_NIL_VAL;
}

// error(msg)
static SeaValue fn_error(SeaVM *vm, int argc, SeaValue *args) {
    SeaValue msg = (argc > 0) ? args[0] : SEA_NIL_VAL;
    return sea_make_error(vm, msg);
}

// is_error(v)
static SeaValue fn_is_error(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_FALSE_VAL;
    return sea_bool_val(sea_is_error(args[0]));
}

// assert(v, msg?)
static SeaValue fn_assert(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_truthy(args[0])) {
        const char *msg = "assertion failed";
        if (argc > 1) {
            SeaString *s = sea_vm_tostring(vm, args[1]);
            fprintf(stderr, "assertion failed: %s\n", s->data);
            sea_obj_release(&s->obj);
            exit(1);
        }
        fprintf(stderr, "%s\n", msg);
        exit(1);
    }
    return sea_val_ref(args[0]);
}

// len(v)
static SeaValue fn_len(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    SeaValue v = args[0];
    if (sea_is_string(v))     return sea_int_val(SEA_AS_STRING(v)->len);
    if (sea_is_rope(v))       return sea_int_val((int64_t)SEA_AS_ROPE(v)->total_len);
    if (sea_is_array(v))      return sea_int_val(SEA_AS_ARRAY(v)->len);
    if (sea_is_table(v))      return sea_int_val(SEA_AS_TABLE(v)->count);
    if (sea_is_map(v))        return sea_int_val(SEA_AS_MAP(v)->count);
    if (sea_is_struct(v))     return sea_int_val(SEA_AS_STRUCT(v)->field_count);
    if (sea_is_int_array(v))  return sea_int_val(SEA_AS_INT_ARRAY(v)->len);
    if (sea_is_float_array(v)) return sea_int_val(SEA_AS_FLOAT_ARRAY(v)->len);
    if (sea_is_float32_array(v)) return sea_int_val(SEA_AS_FLOAT32_ARRAY(v)->len);
    return sea_int_val(0);
}

// chan(n?)
static SeaValue fn_chan(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    uint32_t cap = 0;
    if (argc > 0 && (sea_is_int(args[0]) || sea_is_number(args[0]))) {
        cap = sea_is_int(args[0]) ? (uint32_t)sea_as_int(args[0]) : (uint32_t)sea_as_number(args[0]);
    }
    SeaChannel *ch = sea_chan_new(cap);
    /* Return rc=1; the VM register is the sole owner */
    return sea_obj_val(&ch->obj);
}

// close(ch)
static SeaValue fn_close(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc > 0 && sea_is_channel(args[0])) {
        sea_chan_close(SEA_AS_CHANNEL(args[0]));
    }
    return SEA_NIL_VAL;
}

// panic(v)
/* panic(v) called as a function value (the statement form is OP_PANIC):
 * the same unwinding (sea_vm_raise takes an owned value) */
static SeaValue fn_panic(SeaVM *vm, int argc, SeaValue *args) {
    SeaValue v = (argc > 0) ? args[0] : SEA_NIL_VAL;
    sea_vm_raise(vm, sea_val_ref(v));
}

// recover()
static SeaValue fn_recover(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    return sea_vm_recover(vm);
}

/* ---- require() helpers ---- */

#include "sea_modules.h"   /* gen/: Sea modules built in (sea/test) */

/* Return values for require_try_* helpers */
#define REQUIRE_NONE    0
#define REQUIRE_SOURCE  1   /* .sea source file */
#define REQUIRE_NATIVE  2   /* .so / .dylib native library */

/* Try dir/name.sea then dir/name/init.sea; write found path into out.
 * dir NULL: name is a path of its own (an absolute one). */
static int require_try_dir(const char *dir, const char *name,
                            char *out, size_t outsz) {
    const char *sep = dir ? "/" : "";
    if (!dir) dir = "";
    if ((size_t)snprintf(out, outsz, "%s%s%s.sea", dir, sep, name) < outsz &&
        access(out, R_OK) == 0) return REQUIRE_SOURCE;
    if ((size_t)snprintf(out, outsz, "%s%s%s/init.sea", dir, sep, name) < outsz &&
        access(out, R_OK) == 0) return REQUIRE_SOURCE;
    return REQUIRE_NONE;
}

/* Try dir/name.so (or .dylib on macOS). */
static int require_try_native(const char *dir, const char *name,
                               char *out, size_t outsz) {
    const char *sep = dir ? "/" : "";
    if (!dir) dir = "";
#if defined(__APPLE__)
    if ((size_t)snprintf(out, outsz, "%s%s%s.dylib", dir, sep, name) < outsz &&
        access(out, R_OK) == 0) return REQUIRE_NATIVE;
#endif
    if ((size_t)snprintf(out, outsz, "%s%s%s.so", dir, sep, name) < outsz &&
        access(out, R_OK) == 0) return REQUIRE_NATIVE;
    return REQUIRE_NONE;
}

/* The directory of installed libraries: <dir of the executable>/../lib/sea,
 * when it exists. */
static bool require_libdir(char *out, size_t outsz) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return false;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return false;
    *slash = '\0';                         /* .../bin */
    if ((size_t)snprintf(out, outsz, "%s/../lib/sea", exe) >= outsz) return false;
    if (access(out, X_OK) == 0) return true;
    /* Not installed: a sea run from its source tree (bin/sea next to src/)
     * finds the libraries where they are built, libs/<name>/, in the same
     * layout as lib/sea, so require("http") works without make install */
    char marker[PATH_MAX];
    if ((size_t)snprintf(marker, sizeof marker, "%s/../src/sea_main.c", exe) >= sizeof marker) return false;
    if (access(marker, R_OK) != 0) return false;
    if ((size_t)snprintf(out, outsz, "%s/../libs", exe) >= outsz) return false;
    return access(out, X_OK) == 0;
}

/* Parse, compile and run a module's source; cache its value under key
 * (consumed). name is its source name (a path, or sea/... when built in). */
static SeaValue require_run_source(SeaVM *vm, SeaString *key, const char *src,
                                   const char *name) {
    SeaValue key_val = sea_obj_val(&key->obj);
    SeaParser parser;
    sea_parser_init(&parser, src);
    SeaNode *program = sea_parser_parse(&parser);

    if (!program) {
        char perr[PATH_MAX + 600];
        sea_parser_error_string(&parser, name, perr, sizeof(perr));
        fprintf(stderr, "sea: %s\n", perr);
        sea_parser_free(&parser);
        sea_obj_release(&key->obj);
        return SEA_NIL_VAL;
    }
    sea_parser_free(&parser);

    /* the chunk keeps its source name: the interned key's bytes outlive it
     * (name may be the caller's buffer) */
    SeaChunk *chunk = sea_compile(program, key->data);
    sea_node_free(program);

    if (!chunk) {
        fprintf(stderr, "sea: %s:%d: %s\n", name, sea_compile_error_line(),
                sea_compile_error() ? sea_compile_error() : "compile error");
        sea_obj_release(&key->obj);
        return SEA_NIL_VAL;
    }

    /* Execute module in the current VM (re-entrant). The chunk's source name
     * must outlive it: use the interned key's bytes when name is a buffer. */
    const char *saved_source = vm->source_name;
    vm->source_name = key->data;

    SeaValue result = SEA_NIL_VAL;
    sea_vm_exec_mod(vm, chunk, &result);

    /* The chunk belongs to the shared state, not to this VM: closures from
     * the module point into its proto tree, and they live in the shared
     * module cache and in any goroutine. (Owned by a goroutine's VM, a module
     * first required in a goroutine had its code freed when that goroutine
     * ended, while others still ran it.) */
    pthread_mutex_lock(&vm->shared->loaded_lock);
    chunk->next_owned         = vm->shared->module_chunks;
    vm->shared->module_chunks = chunk;
    pthread_mutex_unlock(&vm->shared->loaded_lock);

    vm->source_name = saved_source;

    /* The cache retains; the module's return value (+1) goes to the caller. */
    sea_map_set(vm_loaded(vm), key_val, result);
    sea_obj_release(&key->obj);
    return result;
}

/* dlopen a native library and call its sea_open; cache the module under key
 * (consumed). */
static SeaValue require_load_native(SeaVM *vm, SeaString *key, const char *abspath) {
    SeaValue key_val = sea_obj_val(&key->obj);
    void *handle = dlopen(abspath, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "sea: dlopen '%s': %s\n", abspath, dlerror());
        sea_obj_release(&key->obj);
        return SEA_NIL_VAL;
    }

    typedef SeaValue (*SeaOpenFn)(SeaVM *vm);
    SeaOpenFn open_fn = (SeaOpenFn)(uintptr_t)dlsym(handle, "sea_open");
    if (!open_fn) {
        fprintf(stderr, "sea: '%s' does not export 'sea_open'\n", abspath);
        dlclose(handle);
        sea_obj_release(&key->obj);
        return SEA_NIL_VAL;
    }

    SeaValue result = open_fn(vm);

    /* Cache the module (sea_map_set retains); sea_open's reference goes
     * to the caller. */
    sea_map_set(vm_loaded(vm), key_val, result);
    sea_obj_release(&key->obj);
    return result;
}

/* A module of a program bundled by `sea build`: source is run, a native
 * library is written to a memory file and loaded from there. */
static SeaValue require_bundled(SeaVM *vm, const SeaBundleEntry *e) {
    SeaString *key = sea_string_intern(e->name, strlen(e->name));
    SeaValue cached = SEA_NIL_VAL;
    if (sea_map_get(vm_loaded(vm), sea_obj_val(&key->obj), &cached)) {
        sea_obj_release(&key->obj);
        return sea_val_ref(cached);
    }
    sea_map_set(vm_loaded(vm), sea_obj_val(&key->obj), SEA_NIL_VAL);
    if (e->kind == 'S') return require_run_source(vm, key, e->data, e->name);
    const char *base = strrchr(e->name, '/');
    int fd = memfd_create(base ? base + 1 : e->name, MFD_CLOEXEC);
    size_t done = 0;
    while (fd >= 0 && done < e->size) {
        ssize_t w = write(fd, e->data + done, e->size - done);
        if (w <= 0) break;
        done += (size_t)w;
    }
    if (fd < 0 || done != e->size) {
        fprintf(stderr, "sea: cannot load the bundled library '%s'\n", e->name);
        if (fd >= 0) close(fd);
        sea_obj_release(&key->obj);
        return SEA_NIL_VAL;
    }
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    /* fd stays open: closed, its number (so the path) would be reused by the
     * next library, and dlopen would return this one for it */
    return require_load_native(vm, key, path);
}

// require(name)
static SeaValue fn_require(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    const char *name = SEA_AS_STRING(args[0])->data;
    /* the requiring file: the source of the function that calls require (a
     * module's function may require later, while another file is running) */
    const char *requiring = vm->source_name;
    if (vm->frame_count > 0) {
        SeaCallFrame *f = &vm->frames[vm->frame_count - 1];
        if (f->closure && !f->closure->is_native && f->closure->chunk && f->closure->chunk->source)
            requiring = f->closure->chunk->source;
    }

    /* 1. Built-in / pre-registered modules (math, string, map, …) */
    SeaValue mod = SEA_NIL_VAL;
    if (sea_globals_get_str(vm, name, &mod))
        return sea_val_ref(mod);

    /* 1b. Sea modules built into the binary (gen/sea_modules.h): sea/test */
    for (int i = 0; SEA_MODULES[i].name; i++) {
        if (strcmp(name, SEA_MODULES[i].name) != 0) continue;
        SeaString *key = sea_string_intern(name, strlen(name));
        SeaValue cached = SEA_NIL_VAL;
        if (sea_map_get(vm_loaded(vm), sea_obj_val(&key->obj), &cached)) {
            sea_obj_release(&key->obj);
            return sea_val_ref(cached);
        }
        sea_map_set(vm_loaded(vm), sea_obj_val(&key->obj), SEA_NIL_VAL);
        return require_run_source(vm, key, SEA_MODULES[i].src, SEA_MODULES[i].name);
    }

    /* 1c. The modules of a program bundled by `sea build` */
    const SeaBundleEntry *be = sea_bundle_find(requiring, name);
    if (be) return require_bundled(vm, be);

    /* 2. Resolve file path
     *    For each search directory, try .sea source first, then .so native.
     *    Search order:
     *      a) directory of the currently-executing source file
     *      b) current working directory
     *      c) each directory in SEA_PATH (colon-separated)            */
    char path[PATH_MAX];
    int  found = REQUIRE_NONE;

    /* Helper: try both .sea and .so in a given directory */
#define TRY_DIR(dir_) do { \
    found = require_try_dir((dir_), name, path, sizeof(path)); \
    if (!found) found = require_try_native((dir_), name, path, sizeof(path)); \
} while (0)

    /* An absolute path names the file itself: no search */
    bool absolute = name[0] == '/';
    if (absolute) TRY_DIR(NULL);

    if (!found && !absolute && requiring) {
        char dir[PATH_MAX];
        strncpy(dir, requiring, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0'; else strcpy(dir, ".");
        TRY_DIR(dir);
    }

    if (!found && !absolute) TRY_DIR(".");

    /* A package installed by `sea get`: sea_modules/ in the requiring file's
     * directory or any directory above it (bare names only) */
    if (!found && !absolute && name[0] != '.') {
        char start[PATH_MAX], dir[PATH_MAX];
        const char *src = requiring ? requiring : "./x";
        const char *slash = strrchr(src, '/');
        if (slash) snprintf(start, sizeof start, "%.*s", (int)(slash - src), src);
        else snprintf(start, sizeof start, ".");
        if (!realpath(start, dir)) snprintf(dir, sizeof dir, "%s", start);
        for (;;) {
            char mods[PATH_MAX];
            size_t dl = strlen(dir);
            if (dl + sizeof "/sea_modules" > sizeof mods) break;
            memcpy(mods, dir, dl);
            memcpy(mods + dl, "/sea_modules", sizeof "/sea_modules");
            if (access(mods, X_OK) == 0) {
                TRY_DIR(mods);
                if (!found) {
                    /* sea_modules/<name>/<last part of name>.sea */
                    const char *last = strrchr(name, '/');
                    last = last ? last + 1 : name;
                    if (snprintf(path, sizeof path, "%s/%s/%s.sea", mods, name, last) < (int)sizeof path &&
                        access(path, R_OK) == 0) found = REQUIRE_SOURCE;
                }
                if (found) break;
            }
            char *up = strrchr(dir, '/');
            if (!up || up == dir) break;
            *up = '\0';
        }
    }

    if (!found && !absolute) {
        const char *sea_path = getenv("SEA_PATH");
        if (sea_path) {
            char buf[4096];
            strncpy(buf, sea_path, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char *tok = strtok(buf, ":");
            while (tok && !found) {
                TRY_DIR(tok);
                tok = strtok(NULL, ":");
            }
        }
    }
    /* The libraries installed with the binary (make install): lib/sea next
     * to its bin/ directory, found from the executable so that any PREFIX
     * works (or libs/ of the source tree it runs from). Bare names only:
     * "json" is lib/sea/json/json.so */
    if (!found && !absolute && name[0] != '.') {
        char libdir[PATH_MAX];
        if (require_libdir(libdir, sizeof libdir)) {
            TRY_DIR(libdir);
            if (!found) {
                const char *last = strrchr(name, '/');
                last = last ? last + 1 : name;
                char sub[PATH_MAX];
                if (snprintf(sub, sizeof sub, "%s/%s", libdir, name) < (int)sizeof sub) {
                    found = require_try_dir(sub, last, path, sizeof(path));
                    if (!found) found = require_try_native(sub, last, path, sizeof(path));
                }
            }
        }
    }
#undef TRY_DIR

    if (!found) {
        fprintf(stderr, "sea: module '%s' not found\n", name);
        return SEA_NIL_VAL;
    }

    /* 3. Resolve to canonical absolute path (cache key) */
    char abspath[PATH_MAX];
    if (!realpath(path, abspath))
        snprintf(abspath, sizeof(abspath), "%s", path);

    /* 4. Check module cache */
    SeaString *key     = sea_string_intern(abspath, strlen(abspath));
    SeaValue   key_val = sea_obj_val(&key->obj);
    SeaValue   cached  = SEA_NIL_VAL;
    if (sea_map_get(vm_loaded(vm), key_val, &cached)) {
        sea_obj_release(&key->obj);
        return sea_val_ref(cached);   /* natives return owned values */
    }

    /* 5. Guard against circular require: cache NIL before executing */
    sea_map_set(vm_loaded(vm), key_val, SEA_NIL_VAL);

    if (found == REQUIRE_NATIVE) return require_load_native(vm, key, abspath);

    /* 6. Read source file */
    FILE *f = fopen(abspath, "rb");
    if (!f) {
        fprintf(stderr, "sea: cannot open '%s'\n", abspath);
        sea_obj_release(&key->obj);
        return SEA_NIL_VAL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char*)sea_malloc((size_t)sz + 1);
    size_t nr = fread(src, 1, (size_t)sz, f);
    src[nr] = '\0';
    fclose(f);

    SeaValue result = require_run_source(vm, key, src, abspath);
    sea_free(src, (size_t)sz + 1);
    return result;
}

// int_array(cap?) → SeaIntArray
static SeaValue fn_int_array(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    uint32_t cap = 8;
    if (argc > 0 && (sea_is_int(args[0]) || sea_is_number(args[0])))
        cap = (uint32_t)(sea_is_int(args[0]) ? sea_as_int(args[0]) : sea_as_number(args[0]));
    SeaIntArray *a = sea_int_array_new(cap);
    return sea_obj_val(&a->obj);
}

// float_array(cap?) → SeaFloatArray
static SeaValue fn_float_array(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    uint32_t cap = 8;
    if (argc > 0 && (sea_is_int(args[0]) || sea_is_number(args[0])))
        cap = (uint32_t)(sea_is_int(args[0]) ? sea_as_int(args[0]) : sea_as_number(args[0]));
    SeaFloatArray *a = sea_float_array_new(cap);
    return sea_obj_val(&a->obj);
}

// float32_array(cap?) → SeaFloat32Array (elements rounded to float)
static SeaValue fn_float32_array(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    uint32_t cap = 8;
    if (argc > 0 && (sea_is_int(args[0]) || sea_is_number(args[0])))
        cap = (uint32_t)(sea_is_int(args[0]) ? sea_as_int(args[0]) : sea_as_number(args[0]));
    SeaFloat32Array *a = sea_float32_array_new(cap);
    return sea_obj_val(&a->obj);
}

/* Native libraries compiled into the binary (EMBED_LIBS in the Makefile):
 * libs/json/json.c and libs/csv/csv.c, whose sea_open is renamed. */
SeaValue sea_open_json(SeaVM *vm);
SeaValue sea_open_csv(SeaVM *vm);

static void sea_lib_embedded_register(SeaVM *vm) {
    static const struct { const char *name; SeaValue (*open)(SeaVM *); } LIBS[] = {
        {"json", sea_open_json},
        {"csv",  sea_open_csv},
    };
    for (size_t i = 0; i < sizeof LIBS / sizeof LIBS[0]; i++) {
        SeaValue m = LIBS[i].open(vm);
        if (sea_is_obj(m) && sea_obj_is_type(m, OBJ_TABLE)) {
            sea_vm_register_module(vm, LIBS[i].name, (SeaTable*)sea_as_obj(m));
        }
        if (sea_is_obj(m)) sea_obj_release(sea_as_obj(m));   /* register_module retained it */
    }
}

/* load(src [, name]) → function | error: compile Sea source into a
 * function that runs it (its top-level `return` is the function's result;
 * it sees the globals, not the caller's locals). name is the source name in
 * errors and tracebacks (default "load"). The compiled code is kept for the
 * life of the process: the function may outlive this goroutine. */
static pthread_mutex_t g_load_mu = PTHREAD_MUTEX_INITIALIZER;
static SeaChunk *g_load_chunks;

static SeaValue fn_load(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *src = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!src) sea_vm_error_noreturn(vm, "load: expected a string of Sea code, got %s", argc > 0 ? sea_type_name(args[0]) : "nothing");
    SeaString *nm = argc > 1 && !sea_is_nil(args[1]) ? sea_value_get_flat_string(args[1]) : NULL;
    char *name = strdup(nm ? nm->data : "load");     /* the chunk keeps a pointer to it */
    if (nm) sea_obj_release(&nm->obj);
    SeaParser parser;
    sea_parser_init(&parser, src->data);
    SeaNode *program = sea_parser_parse(&parser);
    if (!program) {
        char perr[1200];
        sea_parser_error_string(&parser, name, perr, sizeof perr);
        sea_parser_free(&parser);
        sea_obj_release(&src->obj);
        free(name);
        return sea_make_error_str(vm, perr);
    }
    sea_parser_free(&parser);
    SeaChunk *chunk = sea_compile(program, name);
    sea_node_free(program);
    sea_obj_release(&src->obj);
    if (!chunk) {
        char msg[1200];
        snprintf(msg, sizeof msg, "%s:%d: %s", name, sea_compile_error_line(),
                 sea_compile_error() ? sea_compile_error() : "compile error");
        free(name);
        return sea_make_error_str(vm, msg);
    }
    chunk->is_vararg = true;              /* the arguments are its ... */
    pthread_mutex_lock(&g_load_mu);
    chunk->next_owned = g_load_chunks;
    g_load_chunks = chunk;
    pthread_mutex_unlock(&g_load_mu);
    SeaClosure *cl = SEA_ALLOC(SeaClosure);
    cl->obj.type      = OBJ_CLOSURE;
    cl->obj.gc        = 0;
    cl->obj.refcount  = 1;
    cl->chunk         = chunk;
    cl->is_native     = false;
    cl->native        = NULL;
    cl->native_name   = NULL;
    cl->upvalues      = NULL;
    cl->upval_count   = 0;
    return sea_obj_val(&cl->obj);
}

void sea_lib_core_register(SeaVM *vm) {
    sea_vm_register_native(vm, "print",    fn_print);
    sea_vm_register_native(vm, "printf",   fn_printf);
    sea_vm_register_native(vm, "type",     fn_type);
    sea_vm_register_native(vm, "tostring", fn_tostring);
    sea_vm_register_native(vm, "dump", fn_dump);
    sea_vm_register_native(vm, "tonumber", fn_tonumber);
    sea_vm_register_native(vm, "error",    fn_error);
    sea_vm_register_native(vm, "is_error", fn_is_error);
    sea_vm_register_native(vm, "assert",   fn_assert);
    sea_vm_register_native(vm, "len",      fn_len);
    sea_vm_register_native(vm, "chan",      fn_chan);
    sea_vm_register_native(vm, "close",    fn_close);
    sea_vm_register_native(vm, "panic",    fn_panic);
    sea_vm_register_native(vm, "recover",  fn_recover);
    sea_vm_register_native(vm, "require",  fn_require);
    sea_vm_register_native(vm, "load",     fn_load);
    sea_vm_register_native(vm, "int_array",   fn_int_array);
    sea_vm_register_native(vm, "float_array", fn_float_array);
    sea_vm_register_native(vm, "float32_array", fn_float32_array);
}

void sea_stdlib_register(SeaVM *vm) {
    sea_lib_core_register(vm);
    sea_lib_math_register(vm);
    sea_lib_string_register(vm);
    sea_lib_map_register(vm);
    sea_lib_io_register(vm);
    sea_lib_os_register(vm);
    sea_lib_sync_register(vm);
    sea_lib_time_register(vm);
    sea_lib_gc_register(vm);
    sea_lib_net_register(vm);
    sea_lib_parser_register(vm);
    sea_lib_sort_register(vm);
    sea_lib_bigint_register(vm);
    sea_lib_utf8_register(vm);
    sea_lib_fs_register(vm);
    sea_lib_re_register(vm);
    sea_lib_process_register(vm);
    sea_lib_hash_register(vm);
    sea_lib_list_register(vm);
    sea_lib_embedded_register(vm);
}
