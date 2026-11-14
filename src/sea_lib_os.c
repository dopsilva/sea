/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_stdlib.h"
#include "sea_string.h"
#include "sea_array.h"
#include "sea_table.h"
#include "sea_memory.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include "sea_compat.h"
#include <pthread.h>
#include <stdbool.h>

// os.exit(code?)
static SeaValue os_exit(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int code = 0;
    if (argc > 0 && (sea_is_int(args[0]) || sea_is_number(args[0]))) {
        code = (int)(sea_is_int(args[0]) ? sea_as_int(args[0]) : (int64_t)sea_as_number(args[0]));
    }
    exit(code);
}

// os.getenv(name)
/* The environment is process-wide and setenv() may move it while another
 * scheduler thread reads it: every os.* access goes through this lock. */
static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;

static SeaValue os_getenv(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    pthread_mutex_lock(&env_lock);
    const char *val = getenv(SEA_AS_STRING(args[0])->data);
    SeaValue r = val ? sea_obj_val(&sea_string_from_cstr(val)->obj) : SEA_NIL_VAL;
    pthread_mutex_unlock(&env_lock);
    return r;
}

static bool valid_env_name(SeaString *s) {
    if (s->len == 0) return false;
    for (uint32_t i = 0; i < s->len; i++) {
        if (s->data[i] == '=' || s->data[i] == '\0') return false;
    }
    return true;
}

/* os.setenv(name, value) → true | error — for this process and the
 * processes it starts afterwards */
static SeaValue os_setenv(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !sea_is_string(args[0]) || !sea_is_string(args[1]))
        return sea_make_error_str(vm, "os.setenv: expects (name, value) strings");
    SeaString *name = SEA_AS_STRING(args[0]), *value = SEA_AS_STRING(args[1]);
    if (!valid_env_name(name)) return sea_make_error_str(vm, "os.setenv: invalid variable name");
    if (memchr(value->data, '\0', value->len)) return sea_make_error_str(vm, "os.setenv: value contains a NUL byte");
    pthread_mutex_lock(&env_lock);
    int rc = setenv(name->data, value->data, 1);
    pthread_mutex_unlock(&env_lock);
    if (rc != 0) return sea_make_error_str(vm, "os.setenv: cannot set the variable");
    return SEA_TRUE_VAL;
}

/* os.unsetenv(name) → true | error */
static SeaValue os_unsetenv(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_string(args[0])) return sea_make_error_str(vm, "os.unsetenv: expects a name");
    SeaString *name = SEA_AS_STRING(args[0]);
    if (!valid_env_name(name)) return sea_make_error_str(vm, "os.unsetenv: invalid variable name");
    pthread_mutex_lock(&env_lock);
    unsetenv(name->data);
    pthread_mutex_unlock(&env_lock);
    return SEA_TRUE_VAL;
}

extern char **environ;

/* os.environ() → map name → value (a copy) */
static SeaValue os_environ(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    SeaTable *t = sea_table_new(TABLE_MAP);
    pthread_mutex_lock(&env_lock);
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        SeaString *v = sea_string_from_cstr(eq + 1);
        SeaString *k = sea_string_new(*e, (uint32_t)(eq - *e));
        sea_table_set(t, sea_obj_val(&k->obj), sea_obj_val(&v->obj));
        sea_obj_release(&k->obj);
        sea_obj_release(&v->obj);
    }
    pthread_mutex_unlock(&env_lock);
    return sea_obj_val(&t->obj);
}

/* os.exec(cmd) → {code=N, output="..."}
 * Runs cmd via /bin/sh, captures stdout+stderr, returns exit code. */
static SeaValue os_exec(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    const char *cmd = SEA_AS_STRING(args[0])->data;

    /* Redirect stderr to stdout so we capture both. */
    char full[4096];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);

    FILE *fp = popen(full, "r");
    if (!fp) return SEA_NIL_VAL;

    char *buf = NULL;
    size_t len = 0, cap = 0;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
        if (len + n + 1 > cap) {
            size_t nc = cap ? cap : 4096;
            while (nc < len + n + 1) nc *= 2;
            buf = realloc(buf, nc);
            cap = nc;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    if (buf) buf[len] = '\0';

    int status = pclose(fp);
    int code   = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    SeaTable  *t   = sea_table_new(TABLE_MAP);
    SeaString *out = sea_string_new(buf ? buf : "", len);
    free(buf);

    sea_table_set_str(t, "code",   sea_int_val(code));
    sea_table_set_str(t, "output", sea_obj_val(&out->obj));
    sea_obj_release(&out->obj);
    return sea_obj_val(&t->obj);
}

/* os.readfile(path) → string or nil */
static SeaValue os_readfile(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    FILE *f = fopen(SEA_AS_STRING(args[0])->data, "rb");
    if (!f) return SEA_NIL_VAL;

    char *buf = NULL;
    size_t len = 0, cap = 0;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (len + n + 1 > cap) {
            size_t nc = cap ? cap : 4096;
            while (nc < len + n + 1) nc *= 2;
            buf = realloc(buf, nc);
            cap = nc;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    fclose(f);
    if (buf) buf[len] = '\0';

    SeaValue r = sea_obj_val(&sea_string_new(buf ? buf : "", len)->obj);
    free(buf);
    return r;
}

/* os.filesize(path) → integer bytes, or nil if file doesn't exist */
static SeaValue os_filesize(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    struct stat st;
    if (stat(SEA_AS_STRING(args[0])->data, &st) != 0) return SEA_NIL_VAL;
    return sea_int_val((int64_t)st.st_size);
}

/* os.glob(dir, suffix) → sorted array of full paths matching dir/ entries ending in suffix */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static SeaValue os_glob(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_string(args[0]) || !sea_is_string(args[1])) return SEA_NIL_VAL;
    const char *dir    = SEA_AS_STRING(args[0])->data;
    const char *suffix = SEA_AS_STRING(args[1])->data;
    size_t suflen      = strlen(suffix);

    DIR *d = opendir(dir);
    if (!d) return sea_obj_val(&sea_array_new(0)->obj);

    /* Collect matching names first, then sort. */
    char **names = NULL;
    size_t count = 0, ncap = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t nlen = strlen(name);
        if (nlen < suflen) continue;
        if (strcmp(name + nlen - suflen, suffix) != 0) continue;

        /* Build full path */
        size_t flen = strlen(dir) + 1 + nlen + 1;
        char *path  = malloc(flen);
        snprintf(path, flen, "%s/%s", dir, name);

        if (count >= ncap) {
            ncap = ncap ? ncap * 2 : 16;
            names = realloc(names, ncap * sizeof(char *));
        }
        names[count++] = path;
    }
    closedir(d);

    if (count > 0) qsort(names, count, sizeof(char *), cmp_str);

    SeaArray *arr = sea_array_new((uint32_t)count);
    for (size_t i = 0; i < count; i++) {
        SeaValue v = sea_obj_val(&sea_string_from_cstr(names[i])->obj);
        sea_array_push(arr, v);
        sea_obj_release(sea_as_obj(v));
        free(names[i]);
    }
    free(names);

    return sea_obj_val(&arr->obj);
}

/* Script-level args set before register; os.args[1] = first script arg. */
static int    g_os_argc = 0;
static char **g_os_argv = NULL;

void sea_lib_os_set_args(SeaVM *vm, int argc, char **argv) {
    (void)vm;
    g_os_argc = argc;
    g_os_argv = argv;
}

/* os.script: the running script's path (nil for sea -e and standard input) */
static const char *g_os_script = NULL;

void sea_lib_os_set_script(const char *path) {
    g_os_script = path;
}

/* ---- secure random bytes (getrandom, else /dev/urandom) ---- */

static bool fill_random(unsigned char *buf, size_t n) {
    if (sea_sys_random(buf, n)) return true;
    size_t got = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    return got == n;
}

/* os.time() → whole seconds since the epoch (the same as time.now():unix()) */
static SeaValue os_time(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    return sea_int_val((int64_t)time(NULL));
}

/* os.executable() → the absolute path of the running executable (nil if unknown) */
static SeaValue os_executable(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    char path[4096];
    long n = sea_exe_path(path, sizeof path);
    if (n <= 0) return SEA_NIL_VAL;
    SeaString *s = sea_string_new(path, (size_t)n);
    return sea_obj_val(&s->obj);
}

/* os.urandom(n) → n random bytes from the system's cryptographic generator */
static SeaValue os_urandom(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_int(args[0]) || sea_as_int(args[0]) < 0 || sea_as_int(args[0]) > (1 << 24))
        sea_vm_error_noreturn(vm, "os.urandom: expected a number of bytes (0 to 16777216)");
    size_t n = (size_t)sea_as_int(args[0]);
    unsigned char *buf = (unsigned char*)sea_malloc(n ? n : 1);
    if (!fill_random(buf, n)) {
        sea_free(buf, n ? n : 1);
        return sea_make_error_str(vm, "os.urandom: no random source");
    }
    SeaString *s = sea_string_new((const char*)buf, n);
    sea_free(buf, n ? n : 1);
    return sea_obj_val(&s->obj);
}

/* os.token([n]) → n random bytes (default 16) as lowercase hex: tokens, salts, ids */
static SeaValue os_token(SeaVM *vm, int argc, SeaValue *args) {
    int64_t n = 16;
    if (argc >= 1) {
        if (!sea_is_int(args[0]) || sea_as_int(args[0]) < 1 || sea_as_int(args[0]) > 4096)
            sea_vm_error_noreturn(vm, "os.token: expected a number of bytes (1 to 4096)");
        n = sea_as_int(args[0]);
    }
    unsigned char buf[4096];
    if (!fill_random(buf, (size_t)n)) return sea_make_error_str(vm, "os.token: no random source");
    char hex[8193];
    static const char *const D = "0123456789abcdef";
    for (int64_t i = 0; i < n; i++) {
        hex[2 * i] = D[buf[i] >> 4];
        hex[2 * i + 1] = D[buf[i] & 15];
    }
    hex[2 * n] = '\0';
    return sea_obj_val(&sea_string_new(hex, (size_t)(2 * n))->obj);
}

/* os.uuid() → a random UUID (version 4): "1b4e28ba-2fa1-41d2-883f-0016d3cca427" */
static SeaValue os_uuid(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    unsigned char b[16];
    if (!fill_random(b, sizeof b)) return sea_make_error_str(vm, "os.uuid: no random source");
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);   /* version 4 */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);   /* RFC 4122 variant */
    char s[37];
    snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return sea_obj_val(&sea_string_new(s, 36)->obj);
}

void sea_lib_os_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);

    mod_set(vm, mod, "exit",     os_exit);
    mod_set(vm, mod, "getenv",   os_getenv);
    mod_set(vm, mod, "setenv",   os_setenv);
    mod_set(vm, mod, "unsetenv", os_unsetenv);
    mod_set(vm, mod, "environ",  os_environ);
    mod_set(vm, mod, "exec",     os_exec);
    mod_set(vm, mod, "readfile", os_readfile);
    mod_set(vm, mod, "filesize", os_filesize);
    mod_set(vm, mod, "glob",     os_glob);
    mod_set(vm, mod, "executable", os_executable);
    mod_set(vm, mod, "time",     os_time);
    mod_set(vm, mod, "urandom",  os_urandom);
    mod_set(vm, mod, "token",    os_token);
    mod_set(vm, mod, "uuid",     os_uuid);

    /* os.args: index 0 = nil, [1..n] = script arguments */
    SeaArray *arr = sea_array_new((uint32_t)(g_os_argc + 1));
    sea_array_push(arr, SEA_NIL_VAL);
    for (int i = 0; i < g_os_argc; i++) {
        SeaString *s = sea_string_from_cstr(g_os_argv[i]);
        sea_array_push(arr, sea_obj_val(&s->obj));
        sea_obj_release(&s->obj);
    }
    sea_table_set_str(mod, "args", sea_obj_val(&arr->obj));
    sea_obj_release(&arr->obj);
    if (g_os_script && access(g_os_script, R_OK) == 0) {
        SeaString *s = sea_string_from_cstr(g_os_script);
        sea_table_set_str(mod, "script", sea_obj_val(&s->obj));
        sea_obj_release(&s->obj);
    }

    sea_vm_register_module(vm, "os", mod);
    sea_obj_release(&mod->obj);
}
