/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * sea/parser — the Sea lexer and parser as a library, built into the VM:
 *
 *   local parser = require("sea/parser")
 *
 *   parser.parse(src [, opts])          → AST (a "block" node), or an error value
 *   parser.parse_file(path [, opts])    → the same, reading the file
 *   parser.tokenize(src [, opts])       → array of tokens, or an error value
 *   parser.check(src [, opts])          → nil, or {phase, message, line, col, text}
 *   parser.children(node)               → the node's child nodes, in source order
 *   parser.walk(node, fn)               → calls fn(node, parent) on every node
 *   parser.format(src_or_ast [, opts])  → formatted source, or an error value
 *
 * It is the parser the compiler uses (sea_parser.c), so the AST always
 * matches the language. Nodes are tables {type, line, col, ...}; the node
 * format is documented in docs/reference.md ("sea/parser").
 */
#include "sea_stdlib.h"
#include "sea_parser.h"
#include "sea_compiler.h"
#include "sea_chunk.h"
#include "sea_string.h"
#include "sea_rope.h"
#include "sea_table.h"
#include "sea_array.h"
#include "sea_struct.h"
#include "sea_map.h"
#include "sea_sched.h"
#include "sea_memory.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- value helpers ---- */

static SeaValue str_val(const char *s) {
    return sea_obj_val(&sea_string_from_cstr(s)->obj);
}

static SeaValue str_n(const char *s, size_t n) {
    return sea_obj_val(&sea_string_new(s, n)->obj);
}

/* Store an owned value: the container retains it, drop our reference. */
static void tset(SeaTable *t, const char *k, SeaValue v) {
    sea_table_set_str(t, k, v);
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

static void apush(SeaArray *a, SeaValue v) {
    sea_array_push_move(a, v);
}

/* Nodes and option tables are read whether they are tables, structs
 * ({type = "ident", name = "x"} builds a struct) or maps. */
typedef SeaObject Obj;

static bool is_record(SeaValue v) {
    return sea_is_table(v) || sea_is_struct(v) || sea_is_map(v);
}

static Obj *as_record(SeaValue v) {
    return is_record(v) ? sea_as_obj(v) : NULL;
}

/* A field, borrowed (nil when absent). */
static SeaValue tget(Obj *o, const char *k) {
    SeaValue v = SEA_NIL_VAL;
    if (!o) return v;
    if (o->type == OBJ_TABLE) {
        if (!sea_table_get_str((SeaTable*)o, k, &v)) v = SEA_NIL_VAL;
        return v;
    }
    SeaString *key = sea_string_intern(k, strlen(k));
    if (o->type == OBJ_STRUCT) v = sea_struct_get((SeaStruct*)o, key);
    else if (o->type == OBJ_MAP && !sea_map_get((SeaMap*)o, sea_obj_val(&key->obj), &v)) v = SEA_NIL_VAL;
    sea_obj_release(&key->obj);
    return v;
}

/* Borrowed bytes of a string or rope value; NULL for anything else. */
static const char *str_data(SeaValue v, size_t *len) {
    SeaString *s;
    if (sea_is_string(v)) s = SEA_AS_STRING(v);
    else if (sea_is_rope(v)) s = sea_rope_flat(SEA_AS_ROPE(v));
    else return NULL;
    if (len) *len = s->len;
    return s->data;
}

static const char *tget_str(Obj *o, const char *k) {
    return str_data(tget(o, k), NULL);
}

static int64_t tget_int(Obj *o, const char *k) {
    SeaValue v = tget(o, k);
    if (sea_is_int(v)) return sea_as_int(v);
    if (sea_is_number(v)) return (int64_t)sea_as_number(v);
    return 0;
}

/* Lists in nodes are arrays. */
static SeaArray *tget_arr(Obj *o, const char *k) {
    SeaValue v = tget(o, k);
    return sea_is_array(v) ? SEA_AS_ARRAY(v) : NULL;
}

/* A node is a record with a string "type". */
static Obj *as_node(SeaValue v) {
    Obj *o = as_record(v);
    return (o && tget_str(o, "type")) ? o : NULL;
}

static bool is_type(Obj *n, const char *type) {
    const char *t = n ? tget_str(n, "type") : NULL;
    return t && strcmp(t, type) == 0;
}

static Obj *opts_table(int argc, SeaValue *args, int i) {
    return argc > i ? as_record(args[i]) : NULL;
}

static const char *opt_name(Obj *opts, const char *dflt) {
    const char *s = opts ? tget_str(opts, "name") : NULL;
    return s ? s : dflt;
}

static bool opt_bool(Obj *opts, const char *k) {
    return opts && sea_is_truthy(tget(opts, k));
}

static SeaValue errorf(SeaVM *vm, const char *fmt, ...) {
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return sea_make_error_str(vm, msg);
}

/* ---- names ---- */

static const char *binop_name(SeaBinop op) {
    switch (op) {
        case BINOP_ADD:  return "+";   case BINOP_SUB:  return "-";
        case BINOP_MUL:  return "*";   case BINOP_DIV:  return "/";
        case BINOP_IDIV: return "//";  case BINOP_MOD:  return "%";
        case BINOP_POW:  return "^";   case BINOP_EQ:   return "==";
        case BINOP_NEQ:  return "~=";  case BINOP_LT:   return "<";
        case BINOP_LE:   return "<=";  case BINOP_GT:   return ">";
        case BINOP_GE:   return ">=";  case BINOP_AND:  return "and";
        case BINOP_OR:   return "or";  case BINOP_CONCAT: return "..";
        case BINOP_BAND: return "&";   case BINOP_BOR:  return "|";
        case BINOP_BXOR: return "~";   case BINOP_SHL:  return "<<";
        case BINOP_SHR:  return ">>";  case BINOP_CHAN_SEND: return "<-";
    }
    return "?";
}

static const char *unop_name(SeaUnop op) {
    switch (op) {
        case UNOP_NEG: return "-";
        case UNOP_NOT: return "not";
        case UNOP_BNOT: return "~";
        case UNOP_CHAN_RECV: return "<-";
    }
    return "?";
}

static const char *node_type_name(SeaNodeType t) {
    switch (t) {
        case NODE_NIL:         return "nil";
        case NODE_BOOL:        return "bool";
        case NODE_NUMBER:      return "number";
        case NODE_STRING:      return "string";
        case NODE_IDENT:       return "ident";
        case NODE_VARARG:      return "vararg";
        case NODE_UNOP:        return "unop";
        case NODE_BINOP:       return "binop";
        case NODE_INDEX:       return "index";
        case NODE_FIELD:       return "field";
        case NODE_CALL:        return "call";
        case NODE_METHOD_CALL: return "method_call";
        case NODE_FUNC:        return "func";
        case NODE_TABLE:       return "table";
        case NODE_ARRAY_LIT:   return "array_lit";
        case NODE_MAP_LIT:     return "map_lit";
        case NODE_TEMPLATE:    return "template";
        case NODE_CHAN_RECV:   return "chan_recv";
        case NODE_CHAN_SEND:   return "chan_send";
        case NODE_PROPAGATE:   return "propagate";
        case NODE_BLOCK:       return "block";
        case NODE_LOCAL:       return "local";
        case NODE_ASSIGN:      return "assign";
        case NODE_IF:          return "if";
        case NODE_WHILE:       return "while";
        case NODE_FOR_NUM:     return "for_num";
        case NODE_FOR_IN:      return "for_in";
        case NODE_RETURN:      return "return";
        case NODE_BREAK:       return "break";
        case NODE_CONTINUE:    return "continue";
        case NODE_DO:          return "do";
        case NODE_GO:          return "go";
        case NODE_SELECT:      return "select";
        case NODE_DEFER:       return "defer";
        case NODE_PANIC:       return "panic";
        case NODE_DEL:         return "del";
        case NODE_EXPR_STMT:   return "expr_stmt";
    }
    return "unknown";
}

/* ---- source positions ---- */

/* Offsets of the source's lines, to find a node's text from line:col. */
typedef struct {
    const char *src;
    size_t      len;
    size_t     *off;     /* off[i]: first byte of line i+1 */
    int         nlines;
} SrcIndex;

static void src_index_init(SrcIndex *ix, const char *src, size_t len) {
    ix->src = src;
    ix->len = len;
    int n = 1;
    for (size_t i = 0; i < len; i++) if (src[i] == '\n') n++;
    ix->off = (size_t*)malloc(sizeof(size_t) * (size_t)n);
    ix->nlines = n;
    int k = 0;
    ix->off[k++] = 0;
    for (size_t i = 0; i < len; i++) if (src[i] == '\n') ix->off[k++] = i + 1;
}

static void src_index_free(SrcIndex *ix) {
    free(ix->off);
    ix->off = NULL;
}

static const char *src_at(const SrcIndex *ix, int line, int col) {
    if (!ix || !ix->off || line < 1 || line > ix->nlines || col < 1) return NULL;
    size_t o = ix->off[line - 1] + (size_t)(col - 1);
    return o < ix->len ? ix->src + o : NULL;
}

/* The source text of the number or string literal at line:col (nil if there
 * is none: a table key written as a name, a node built by hand). */
static SeaValue literal_raw(const SrcIndex *ix, int line, int col) {
    const char *p = src_at(ix, line, col);
    if (!p) return SEA_NIL_VAL;
    SeaLexer l;
    sea_lexer_init(&l, p);
    SeaToken t = l.current;
    SeaValue raw = SEA_NIL_VAL;
    if (t.type == TK_NUMBER || t.type == TK_STRING) raw = str_n(t.start, (size_t)t.len);
    if (t.type == TK_STRING) sea_free((void*)t.val.string.ptr, t.val.string.len + 1);
    sea_lexer_free(&l);
    return raw;
}

/* ---- C AST → Sea tables ---- */

static SeaValue node_to_val(SeaNode *n, const SrcIndex *ix);

static SeaValue nodes_to_arr(SeaNode **nodes, int count, const SrcIndex *ix) {
    SeaArray *a = sea_array_new((uint32_t)(count < 0 ? 0 : count));
    for (int i = 0; i < count; i++) apush(a, node_to_val(nodes[i], ix));
    return sea_obj_val(&a->obj);
}

static SeaValue strs_to_arr(char **strs, int count) {
    SeaArray *a = sea_array_new((uint32_t)(count < 0 ? 0 : count));
    for (int i = 0; i < count; i++) apush(a, str_val(strs[i]));
    return sea_obj_val(&a->obj);
}

#define SEA_INT_LIMIT 140737488355327.0   /* 2^47 - 1: the int range */

static SeaValue node_to_val(SeaNode *n, const SrcIndex *ix) {
    if (!n) return SEA_NIL_VAL;

    SeaTable *t = sea_table_new(TABLE_MAP);
    tset(t, "type", str_val(node_type_name(n->type)));
    tset(t, "line", sea_int_val(n->line));
    tset(t, "col",  sea_int_val(n->col));
    if (n->paren) tset(t, "paren", SEA_TRUE_VAL);

    switch (n->type) {
        case NODE_NIL: case NODE_BREAK: case NODE_CONTINUE:
        case NODE_VARARG: case NODE_MAP_LIT:
            break;

        case NODE_BOOL:
            tset(t, "value", sea_bool_val(n->boolean));
            break;

        case NODE_NUMBER: {
            double d = n->number;
            bool as_int = !n->num_is_float && d == trunc(d) && fabs(d) <= SEA_INT_LIMIT;
            tset(t, "value", as_int ? sea_int_val((int64_t)d) : sea_number_val(d));
            tset(t, "is_float", sea_bool_val(n->num_is_float));
            tset(t, "raw", literal_raw(ix, n->line, n->col));
            break;
        }

        case NODE_STRING:
            tset(t, "value", str_n(n->string.ptr, n->string.len));
            tset(t, "raw", literal_raw(ix, n->line, n->col));
            break;

        case NODE_IDENT:
            tset(t, "name", str_val(n->ident));
            break;

        case NODE_UNOP:
            tset(t, "op",      str_val(unop_name(n->unop.op)));
            tset(t, "operand", node_to_val(n->unop.expr, ix));
            break;

        case NODE_BINOP:
            tset(t, "op",    str_val(binop_name(n->binop.op)));
            tset(t, "left",  node_to_val(n->binop.left, ix));
            tset(t, "right", node_to_val(n->binop.right, ix));
            break;

        case NODE_INDEX:
            tset(t, "object", node_to_val(n->index.obj, ix));
            tset(t, "index",  node_to_val(n->index.key, ix));
            break;

        case NODE_FIELD:
            tset(t, "object", node_to_val(n->field.obj, ix));
            tset(t, "field",  str_val(n->field.field));
            break;

        case NODE_CALL:
            tset(t, "callee", node_to_val(n->call.fn, ix));
            tset(t, "args",   nodes_to_arr(n->call.args, n->call.argc, ix));
            tset(t, "end_line", sea_int_val(n->call.end_line));
            break;

        case NODE_METHOD_CALL:
            tset(t, "object", node_to_val(n->method.obj, ix));
            tset(t, "method", str_val(n->method.method));
            tset(t, "args",   nodes_to_arr(n->method.args, n->method.argc, ix));
            tset(t, "end_line", sea_int_val(n->method.end_line));
            break;

        case NODE_FUNC: {
            SeaArray *params = sea_array_new((uint32_t)n->func.paramc);
            for (int i = 0; i < n->func.paramc; i++) {
                SeaTable *p = sea_table_new(TABLE_MAP);
                tset(p, "name",      str_val(n->func.params[i].name));
                tset(p, "is_vararg", sea_bool_val(n->func.params[i].is_vararg));
                apush(params, sea_obj_val(&p->obj));
            }
            tset(t, "params", sea_obj_val(&params->obj));
            tset(t, "body", node_to_val(n->func.body, ix));
            tset(t, "name", n->func.name ? str_val(n->func.name) : SEA_NIL_VAL);
            tset(t, "is_method", sea_bool_val(n->func.is_method));
            break;
        }

        case NODE_TABLE: {
            SeaArray *fields = sea_array_new((uint32_t)n->table.count);
            for (int i = 0; i < n->table.count; i++) {
                SeaTable *f = sea_table_new(TABLE_MAP);
                tset(f, "key",   node_to_val(n->table.keys[i], ix));
                tset(f, "value", node_to_val(n->table.vals[i], ix));
                apush(fields, sea_obj_val(&f->obj));
            }
            tset(t, "fields", sea_obj_val(&fields->obj));
            tset(t, "end_line", sea_int_val(n->table.end_line));
            break;
        }

        case NODE_TEMPLATE: {
            SeaArray *parts = sea_array_new((uint32_t)n->tmpl.count + 1);
            SeaArray *raws = sea_array_new((uint32_t)n->tmpl.count + 1);
            SeaArray *exprs = sea_array_new((uint32_t)n->tmpl.count);
            for (int i = 0; i <= n->tmpl.count; i++) {
                apush(parts, str_n(n->tmpl.texts[i], n->tmpl.lens[i]));
                apush(raws, str_val(n->tmpl.raws[i]));
                if (i < n->tmpl.count) apush(exprs, node_to_val(n->tmpl.exprs[i], ix));
            }
            tset(t, "parts", sea_obj_val(&parts->obj));
            tset(t, "raws", sea_obj_val(&raws->obj));
            tset(t, "exprs", sea_obj_val(&exprs->obj));
            break;
        }

        case NODE_ARRAY_LIT: {
            SeaArray *elems = sea_array_new(n->array_lit.count);
            for (int i = 0; i < n->array_lit.count; i++) apush(elems, node_to_val(n->array_lit.elems[i], ix));
            tset(t, "elems", sea_obj_val(&elems->obj));
            tset(t, "end_line", sea_int_val(n->array_lit.end_line));
            break;
        }

        case NODE_CHAN_RECV:
            tset(t, "channel", node_to_val(n->unop.expr, ix));
            break;

        case NODE_CHAN_SEND:
            tset(t, "channel", node_to_val(n->binop.left, ix));
            tset(t, "value",   node_to_val(n->binop.right, ix));
            break;

        case NODE_PROPAGATE:
            tset(t, "expr", node_to_val(n->propagate, ix));
            break;

        case NODE_BLOCK:
        case NODE_DO:
            tset(t, "stmts", nodes_to_arr(n->block.stmts, n->block.count, ix));
            tset(t, "end_line", sea_int_val(n->block.end_line));
            break;

        case NODE_LOCAL:
            tset(t, "names",    strs_to_arr(n->local.names, n->local.count));
            tset(t, "values",   nodes_to_arr(n->local.exprs, n->local.expr_count, ix));
            tset(t, "is_const", sea_bool_val(n->local.is_const));
            break;

        case NODE_ASSIGN:
            tset(t, "targets", nodes_to_arr(n->assign.targets, n->assign.count, ix));
            if (n->assign.op_wrapped)   /* a op= b: the parser built a op b; show b */
                tset(t, "values", nodes_to_arr(&n->assign.exprs[0]->binop.right, 1, ix));
            else
                tset(t, "values",  nodes_to_arr(n->assign.exprs, n->assign.expr_count, ix));
            if (n->assign.op) tset(t, "op", str_val(binop_name((SeaBinop)(n->assign.op - 1))));
            break;

        case NODE_IF:
            tset(t, "cond",      node_to_val(n->if_stmt.cond, ix));
            tset(t, "then_body", node_to_val(n->if_stmt.then_body, ix));
            tset(t, "else_body", node_to_val(n->if_stmt.else_body, ix));
            break;

        case NODE_WHILE:
            tset(t, "cond", node_to_val(n->while_loop.cond, ix));
            tset(t, "body", node_to_val(n->while_loop.body, ix));
            break;

        case NODE_FOR_NUM:
            tset(t, "var",   str_val(n->for_num.var));
            tset(t, "start", node_to_val(n->for_num.start, ix));
            tset(t, "limit", node_to_val(n->for_num.limit, ix));
            tset(t, "step",  node_to_val(n->for_num.step, ix));
            tset(t, "body",  node_to_val(n->for_num.body, ix));
            break;

        case NODE_FOR_IN:
            tset(t, "vars", strs_to_arr(n->for_in.vars, n->for_in.var_count));
            tset(t, "iter", node_to_val(n->for_in.iter_expr, ix));
            tset(t, "body", node_to_val(n->for_in.body, ix));
            break;

        case NODE_RETURN:
            tset(t, "values", nodes_to_arr(n->ret.exprs, n->ret.count, ix));
            break;

        case NODE_GO:
            tset(t, "call", node_to_val(n->go_call, ix));
            break;

        case NODE_SELECT: {
            SeaArray *cases = sea_array_new((uint32_t)n->select.count);
            for (int i = 0; i < n->select.count; i++) {
                SeaSelectCase *sc = &n->select.cases[i];
                SeaTable *c = sea_table_new(TABLE_MAP);
                tset(c, "stmt",       node_to_val(sc->stmt, ix));
                tset(c, "body",       node_to_val(sc->body, ix));
                tset(c, "is_default", sea_bool_val(sc->is_default));
                apush(cases, sea_obj_val(&c->obj));
            }
            tset(t, "cases", sea_obj_val(&cases->obj));
            break;
        }

        case NODE_DEFER:
            tset(t, "call", node_to_val(n->defer_stmt, ix));
            break;

        case NODE_PANIC:
            tset(t, "value", node_to_val(n->panic_expr, ix));
            break;

        case NODE_DEL:
            tset(t, "target", node_to_val(n->del_stmt, ix));
            break;

        case NODE_EXPR_STMT:
            tset(t, "expr", node_to_val(n->expr_stmt, ix));
            break;
    }
    return sea_obj_val(&t->obj);
}

/* ---- comments ---- */

/* The comments of src as an array of {text, line, col, inline} (inline: code
 * comes before it on its line). The source has parsed, so it lexes cleanly. */
static SeaValue collect_comments(const char *src) {
    SeaArray *out = sea_array_new(8);
    SeaLexer l;
    sea_lexer_init_ex(&l, src, true);
    int prev_line = 0;
    for (;;) {
        SeaToken t = sea_lexer_next(&l);
        if (t.type == TK_EOF || t.type == TK_ERROR) break;
        if (t.type == TK_STRING) sea_free((void*)t.val.string.ptr, t.val.string.len + 1);
        if (t.type == TK_COMMENT) {
            SeaTable *c = sea_table_new(TABLE_MAP);
            tset(c, "text",   str_n(t.start, (size_t)t.len));
            tset(c, "line",   sea_int_val(t.line));
            tset(c, "col",    sea_int_val(t.col));
            tset(c, "inline", sea_bool_val(prev_line == t.line));
            apush(out, sea_obj_val(&c->obj));
        }
        /* a string or comment can span lines: what follows is on its last */
        prev_line = t.line;
        for (int i = 0; i < t.len; i++) if (t.start[i] == '\n') prev_line++;
    }
    if (l.current.type == TK_STRING)
        sea_free((void*)l.current.val.string.ptr, l.current.val.string.len + 1);
    sea_lexer_free(&l);
    return sea_obj_val(&out->obj);
}

/* ---- parse ---- */

/* Parse src (len bytes) into a Sea AST: an owned block node, or an error
 * value "name:line:col: message". */
static SeaValue parse_source(SeaVM *vm, const char *src, size_t len,
                             const char *name, bool comments) {
    SeaParser p;
    sea_parser_init(&p, src);
    SeaNode *root = sea_parser_parse(&p);
    if (!root) {
        char msg[768];
        sea_parser_error_string(&p, name, msg, sizeof msg);
        sea_parser_free(&p);
        return sea_make_error_str(vm, msg);
    }
    sea_parser_free(&p);

    SrcIndex ix;
    src_index_init(&ix, src, len);
    SeaValue ast = node_to_val(root, &ix);
    src_index_free(&ix);
    sea_node_free(root);
    if (comments) tset(SEA_AS_TABLE(ast), "comments", collect_comments(src));
    return ast;
}

/* Read a whole file; NULL (errno set) on failure. */
static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (cap - n < 65536) {
            cap = cap ? cap * 2 : 65536;
            char *nb = (char*)realloc(buf, cap + 1);
            if (!nb) { free(buf); fclose(f); errno = ENOMEM; return NULL; }
            buf = nb;
        }
        size_t r = fread(buf + n, 1, cap - n, f);
        n += r;
        if (r == 0) break;
    }
    bool err = ferror(f);
    fclose(f);
    if (err) { free(buf); errno = EIO; return NULL; }
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* parse(src [, {name, comments}]) */
typedef SeaValue (*NativeFn)(SeaVM *vm, int argc, SeaValue *args);

static SeaValue fn_parse(SeaVM *vm, int argc, SeaValue *args) {
    size_t len;
    const char *src = argc > 0 ? str_data(args[0], &len) : NULL;
    if (!src) return errorf(vm, "parser.parse: expected a string");
    Obj *opts = opts_table(argc, args, 1);
    return parse_source(vm, src, len, opt_name(opts, "<input>"), opt_bool(opts, "comments"));
}

/* parse_file(path [, {comments}]) */
static SeaValue fn_parse_file(SeaVM *vm, int argc, SeaValue *args) {
    const char *path = argc > 0 ? str_data(args[0], NULL) : NULL;
    if (!path) return errorf(vm, "parser.parse_file: expected a path");
    size_t len;
    char *src = read_file(path, &len);
    if (!src) return errorf(vm, "parser.parse_file: cannot read '%s': %s", path, strerror(errno));
    Obj *opts = opts_table(argc, args, 1);
    SeaValue r = parse_source(vm, src, len, opt_name(opts, path), opt_bool(opts, "comments"));
    free(src);
    return r;
}

/* ---- tokenize ---- */

static const char *token_kind(SeaTK tk) {
    switch (tk) {
        case TK_IDENT:   return "name";
        case TK_NUMBER:  return "number";
        case TK_STRING:  return "string";
        case TK_COMMENT: return "comment";
        case TK_NIL: case TK_TRUE: case TK_FALSE:
        case TK_AND: case TK_BREAK: case TK_CHAN: case TK_CONST: case TK_CONTINUE:
        case TK_DEFAULT: case TK_DEL: case TK_DEFER: case TK_DO: case TK_ELSE:
        case TK_ELSEIF: case TK_END: case TK_FOR: case TK_FUNCTION: case TK_GO:
        case TK_IF: case TK_IN: case TK_LOCAL: case TK_NOT: case TK_OR:
        case TK_PANIC: case TK_RECOVER: case TK_RETURN: case TK_SELECT:
        case TK_CASE: case TK_THEN: case TK_WHILE:
            return "keyword";
        default:
            return "op";
    }
}

/* tokenize(src [, {name, comments}]) → [{kind, text, line, col, value?}] */
static SeaValue fn_tokenize(SeaVM *vm, int argc, SeaValue *args) {
    const char *src = argc > 0 ? str_data(args[0], NULL) : NULL;
    if (!src) return errorf(vm, "parser.tokenize: expected a string");
    Obj *opts = opts_table(argc, args, 1);
    const char *name = opt_name(opts, "<input>");

    SeaArray *out = sea_array_new(64);
    SeaLexer l;
    sea_lexer_init_ex(&l, src, opt_bool(opts, "comments"));
    SeaValue result = sea_obj_val(&out->obj);
    for (;;) {
        SeaToken t = sea_lexer_next(&l);
        if (t.type == TK_EOF) break;
        if (t.type == TK_ERROR) {
            sea_obj_release(&out->obj);
            result = errorf(vm, "%s:%d:%d: %.*s", name, t.line, t.col, t.len, t.start);
            break;
        }
        SeaTable *tok = sea_table_new(TABLE_MAP);
        tset(tok, "kind", str_val(token_kind(t.type)));
        tset(tok, "text", str_n(t.start, (size_t)t.len));
        tset(tok, "line", sea_int_val(t.line));
        tset(tok, "col",  sea_int_val(t.col));
        if (t.type == TK_NUMBER) {
            double d = t.val.number;
            bool as_int = !t.is_float && d == trunc(d) && fabs(d) <= SEA_INT_LIMIT;
            tset(tok, "value", as_int ? sea_int_val((int64_t)d) : sea_number_val(d));
        } else if (t.type == TK_STRING) {
            tset(tok, "value", str_n(t.val.string.ptr, t.val.string.len));
            sea_free((void*)t.val.string.ptr, t.val.string.len + 1);
        }
        apush(out, sea_obj_val(&tok->obj));
    }
    if (l.current.type == TK_STRING)
        sea_free((void*)l.current.val.string.ptr, l.current.val.string.len + 1);
    sea_lexer_free(&l);
    return result;
}

/* ---- check ---- */

static SeaValue diagnostic(const char *phase, const char *name, const char *msg,
                           int line, int col) {
    char text[1024];
    if (col > 0) snprintf(text, sizeof text, "%s:%d:%d: %s", name, line, col, msg);
    else         snprintf(text, sizeof text, "%s:%d: %s", name, line, msg);
    SeaTable *d = sea_table_new(TABLE_MAP);
    tset(d, "phase",   str_val(phase));
    tset(d, "message", str_val(msg));
    tset(d, "line",    sea_int_val(line));
    tset(d, "col",     col > 0 ? sea_int_val(col) : SEA_NIL_VAL);
    tset(d, "text",    str_val(text));
    return sea_obj_val(&d->obj);
}

/* check(src [, {name}]): parse and compile without running → nil, or the
 * first error as {phase = "syntax"|"compile", message, line, col, text}. */
static SeaValue fn_check(SeaVM *vm, int argc, SeaValue *args) {
    const char *src = argc > 0 ? str_data(args[0], NULL) : NULL;
    if (!src) return errorf(vm, "parser.check: expected a string");
    const char *name = opt_name(opts_table(argc, args, 1), "<input>");

    SeaParser p;
    sea_parser_init(&p, src);
    SeaNode *root = sea_parser_parse(&p);
    if (!root) {
        SeaValue d = diagnostic("syntax", name, p.err_msg ? p.err_msg : "syntax error",
                                p.err_line, p.err_col);
        sea_parser_free(&p);
        return d;
    }
    sea_parser_free(&p);

    SeaChunk *chunk = sea_compile(root, name);
    sea_node_free(root);
    if (!chunk) {
        const char *msg = sea_compile_error();
        return diagnostic("compile", name, msg ? msg : "compile error",
                          sea_compile_error_line(), 0);
    }
    sea_chunk_free(chunk);
    return SEA_NIL_VAL;
}

/* ---- children / walk ---- */

/* Which fields of each node type hold child nodes, in source order:
 * "x" one node, "x*" an array of nodes, "fields%" table fields {key, value},
 * "cases%" select cases {stmt, body}. */
static const struct { const char *type; const char *fields[4]; } CHILD_FIELDS[] = {
    {"unop",        {"operand"}},
    {"binop",       {"left", "right"}},
    {"index",       {"object", "index"}},
    {"field",       {"object"}},
    {"call",        {"callee", "args*"}},
    {"method_call", {"object", "args*"}},
    {"func",        {"body"}},
    {"table",       {"fields%"}},
    {"array_lit",   {"elems*"}},
    {"template",    {"exprs*"}},
    {"chan_recv",   {"channel"}},
    {"chan_send",   {"channel", "value"}},
    {"propagate",   {"expr"}},
    {"block",       {"stmts*"}},
    {"do",          {"stmts*"}},
    {"local",       {"values*"}},
    {"assign",      {"targets*", "values*"}},
    {"if",          {"cond", "then_body", "else_body"}},
    {"while",       {"cond", "body"}},
    {"for_num",     {"start", "limit", "step", "body"}},
    {"for_in",      {"iter", "body"}},
    {"return",      {"values*"}},
    {"go",          {"call"}},
    {"defer",       {"call"}},
    {"select",      {"cases%"}},
    {"panic",       {"value"}},
    {"del",         {"target"}},
    {"expr_stmt",   {"expr"}},
};

static void push_node(SeaArray *out, SeaValue v) {
    if (as_node(v)) sea_array_push(out, v);
}

static void collect_children(Obj *n, SeaArray *out) {
    const char *type = tget_str(n, "type");
    if (!type) return;
    for (size_t i = 0; i < sizeof CHILD_FIELDS / sizeof CHILD_FIELDS[0]; i++) {
        if (strcmp(CHILD_FIELDS[i].type, type) != 0) continue;
        for (int f = 0; f < 4 && CHILD_FIELDS[i].fields[f]; f++) {
            char name[16];
            const char *spec = CHILD_FIELDS[i].fields[f];
            size_t sl = strlen(spec);
            char kind = spec[sl - 1];
            if (kind != '*' && kind != '%') { push_node(out, tget(n, spec)); continue; }
            memcpy(name, spec, sl - 1);
            name[sl - 1] = '\0';
            SeaArray *a = tget_arr(n, name);
            for (uint32_t k = 0; a && k < a->len; k++) {
                if (kind == '*') { push_node(out, a->data[k]); continue; }
                if (!is_record(a->data[k])) continue;
                Obj *rec = as_record(a->data[k]);
                bool is_table = strcmp(name, "fields") == 0;
                push_node(out, tget(rec, is_table ? "key" : "stmt"));
                push_node(out, tget(rec, is_table ? "value" : "body"));
            }
        }
        return;
    }
}

/* children(node) → array of its child nodes */
static SeaValue fn_children(SeaVM *vm, int argc, SeaValue *args) {
    Obj *n = argc > 0 ? as_node(args[0]) : NULL;
    if (!n) return errorf(vm, "parser.children: expected a node");
    SeaArray *out = sea_array_new(4);
    collect_children(n, out);
    return sea_obj_val(&out->obj);
}

#define WALK_MAX_DEPTH 10000   /* a deeper tree is a cycle */

/* 1: done, 0: a panic is unwinding, -1: too deep */
static int walk_node(SeaVM *vm, SeaValue node, SeaValue parent, SeaValue fn, int depth) {
    if (depth > WALK_MAX_DEPTH) return -1;
    SeaValue args[2] = { node, parent };
    SeaValue r = sea_vm_call(vm, fn, args, 2);
    if (vm->panic_propagating) {
        if (sea_is_obj(r)) sea_obj_release(sea_as_obj(r));
        return 0;
    }
    bool skip = (r == SEA_FALSE_VAL);
    if (sea_is_obj(r)) sea_obj_release(sea_as_obj(r));
    Obj *n = as_node(node);
    if (skip || !n) return 1;

    /* children are collected after fn, so fn may replace them */
    SeaArray *kids = sea_array_new(4);
    collect_children(n, kids);
    int rc = 1;
    for (uint32_t i = 0; i < kids->len && rc == 1; i++)
        rc = walk_node(vm, kids->data[i], node, fn, depth + 1);
    sea_obj_release(&kids->obj);
    return rc;
}

/* walk(node, fn): fn(node, parent) on every node, parents before children;
 * returning false skips the node's children. */
static SeaValue fn_walk(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !as_node(args[0])) return errorf(vm, "parser.walk: expected a node");
    if (argc < 2 || !sea_obj_is_type(args[1], OBJ_CLOSURE))
        return errorf(vm, "parser.walk: expected a function");
    int rc = walk_node(vm, args[0], SEA_NIL_VAL, args[1], 0);
    if (rc < 0) return errorf(vm, "parser.walk: tree too deep (a cycle?)");
    return SEA_NIL_VAL;
}

/* ---- format ----
 *
 * Prints an AST (Sea tables) as source code: one statement per line, blocks
 * indented, canonical spacing, parentheses only where precedence needs them.
 * From source text it keeps the comments, single blank lines and the written
 * form of literals (quotes, escapes, hex numbers): the AST carries comments
 * (parse with comments = true) and each literal's `raw` text, used when it
 * still matches `value`. */

enum {
    PR_NONE, PR_OR, PR_AND, PR_CMP, PR_BOR, PR_BXOR, PR_BAND, PR_SHIFT,
    PR_CONCAT, PR_ADD, PR_MUL, PR_UNARY, PR_POW, PR_POSTFIX
};

#define FMT_MAX_NEST 5000

typedef struct {
    char       *buf;
    size_t      len, cap;
    const char *ind;          /* one indentation level */
    int         depth;
    bool        at_start;     /* nothing printed yet in this block: no blank line */
    /* comments still to print: an array of {text, line, col, inline} */
    SeaArray   *comments;
    uint32_t    ci;
    /* blank lines: from the source when formatting text, else estimated */
    const bool *blank;        /* blank[i]: line i+1 is empty */
    int         nblank;
    int         last_line;    /* last source line printed (estimate) */
    int         stmt_col;     /* the statement being printed: column, next line */
    int         limit;
    int         nest;
    /* trailing comments printed, to align the ones on consecutive lines:
     * offset of the space before each comment */
    size_t     *tc;
    int         ntc, ctc;
    bool        failed;
    char        err[256];
} Printer;

static void fail(Printer *P, const char *fmt, ...) {
    if (P->failed) return;
    P->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(P->err, sizeof P->err, fmt, ap);
    va_end(ap);
}

static void out(Printer *P, const char *s, size_t n) {
    if (P->len + n + 1 > P->cap) {
        size_t cap = P->cap ? P->cap : 4096;
        while (P->len + n + 1 > cap) cap *= 2;
        P->buf = (char*)realloc(P->buf, cap);
        P->cap = cap;
    }
    memcpy(P->buf + P->len, s, n);
    P->len += n;
}

static void outs(Printer *P, const char *s) { out(P, s, strlen(s)); }
static void outc(Printer *P, char c) { out(P, &c, 1); }

static void indent(Printer *P) {
    for (int i = 0; i < P->depth; i++) outs(P, P->ind);
}

static int node_line(Obj *n) { return n ? (int)tget_int(n, "line") : 0; }

/* The line an expression's text ends on, as far as the AST tells: a call
 * knows its ')'; anything else is taken to end on the line it starts. */
static int node_end_line(Obj *n) {
    if (!n) return 0;
    int e = (int)tget_int(n, "end_line");
    return e > 0 ? e : node_line(n);
}

/* A chained ':method()' or '.field' that the source put on a line of its
 * own stays there, one level in:
 *     b:add("a")
 *         :add("b") */
/* A ':' or '.' that the source put on a new line stays there, one level in.
 * Returns 1 when it broke the line: the caller then prints the method's
 * arguments one level in too, so a function passed there keeps its body
 * under the chain. */
static int chain_break(Printer *P, Obj *n, Obj *obj) {
    int l = node_line(n), e = node_end_line(obj);
    if (l <= 0 || e <= 0 || l <= e) return 0;
    outc(P, '\n');
    P->depth++;
    indent(P);
    P->depth--;
    return 1;
}
static int node_col(Obj *n)  { return n ? (int)tget_int(n, "col") : 0; }

/* The largest line number in a subtree, and whether it holds a select. */
static int node_end(Obj *n) { return n ? (int)tget_int(n, "end_line") : 0; }

static void span(Obj *n, int depth, int *maxl, bool *sel) {
    int l = node_line(n), e = node_end(n);
    if (l > *maxl) *maxl = l;
    if (e > *maxl) *maxl = e;
    if (is_type(n, "select")) *sel = true;
    if (depth > FMT_MAX_NEST) return;
    SeaArray *kids = sea_array_new(4);
    collect_children(n, kids);
    for (uint32_t i = 0; i < kids->len; i++) span(as_record(kids->data[i]), depth + 1, maxl, sel);
    sea_obj_release(&kids->obj);
}

/* The smallest line number in a subtree (0 if none). */
static int min_line(Obj *n, int depth) {
    int m = node_line(n);
    if (depth > FMT_MAX_NEST) return m;
    SeaArray *kids = sea_array_new(4);
    collect_children(n, kids);
    for (uint32_t i = 0; i < kids->len; i++) {
        int k = min_line(as_record(kids->data[i]), depth + 1);
        if (k > 0 && (m == 0 || k < m)) m = k;
    }
    sea_obj_release(&kids->obj);
    return m;
}

static int max_line(Obj *n, int depth) {
    int m = 0;
    bool sel = false;
    span(n, depth, &m, &sel);
    return m;
}

/* A block statement or function written on one source line stays on one. */
static bool one_line(Obj *n) {
    int l = node_line(n), m = 0;
    bool sel = false;
    if (l <= 0) return false;
    span(n, 0, &m, &sel);
    return m == l && !sel;
}

/* -- comments and blank lines -- */

static Obj *cm_peek(Printer *P) {
    while (P->comments && P->ci < P->comments->len) {
        SeaValue v = P->comments->data[P->ci];
        if (is_record(v) && tget_str(as_record(v), "text")) return as_record(v);
        P->ci++;
    }
    return NULL;
}

static bool blank_before(Printer *P, int line) {
    if (line <= 1) return false;
    if (P->blank) return line - 2 < P->nblank && P->blank[line - 2];
    return P->last_line > 0 && line > P->last_line + 1;
}

/* Start a line for an item (statement, comment, table row) at a source line. */
static void begin_item(Printer *P, int line) {
    if (!P->at_start && line > 0 && blank_before(P, line)) outc(P, '\n');
    P->at_start = false;
    indent(P);
}

static void note_line(Printer *P, int line) {
    if (line > P->last_line) P->last_line = line;
}

static void emit_comment_line(Printer *P, Obj *c) {
    int line = node_line(c);
    begin_item(P, line);
    outs(P, tget_str(c, "text"));
    outc(P, '\n');
    note_line(P, line);
    P->ci++;
}

/* Comments that come before source line `line`, each on its own line. */
static void flush_before(Printer *P, int line) {
    Obj *c;
    while ((c = cm_peek(P)) && node_line(c) < line) emit_comment_line(P, c);
}

/* At the end of a block: the comments that belong in it (before the next
 * statement, on their own line, and indented deeper than the statement that
 * opened the block). */
static void flush_block_end(Printer *P, int limit, int parent_col) {
    Obj *c;
    while ((c = cm_peek(P)) && node_line(c) < limit &&
           !sea_is_truthy(tget(c, "inline")) && node_col(c) > parent_col)
        emit_comment_line(P, c);
}

static void emit_trailing(Printer *P, Obj *c) {
    if (P->ntc == P->ctc) {
        P->ctc = P->ctc ? P->ctc * 2 : 16;
        P->tc = (size_t*)realloc(P->tc, sizeof(size_t) * (size_t)P->ctc);
    }
    P->tc[P->ntc++] = P->len;
    outc(P, ' ');
    outs(P, tget_str(c, "text"));
    P->ci++;
}

/* A comment that followed code on one of the source lines lo..hi goes at the
 * end of the current output line. */
static void attach_trailing(Printer *P, int lo, int hi) {
    Obj *c = cm_peek(P);
    if (!c || lo <= 0 || !sea_is_truthy(tget(c, "inline"))) return;
    int line = node_line(c);
    if (line < lo || line > hi) return;
    emit_trailing(P, c);
}

/* Width of buf[a..b) in characters (UTF-8 continuation bytes don't count). */
static size_t text_width(const char *buf, size_t a, size_t b) {
    size_t w = 0;
    for (size_t i = a; i < b; i++) if (((unsigned char)buf[i] & 0xC0) != 0x80) w++;
    return w;
}

/* Align the trailing comments of consecutive lines in one column. Returns a
 * new buffer (malloc) of *len bytes. */
static char *align_comments(Printer *P, size_t *len) {
    char *src = P->buf;
    size_t n = P->len;
    size_t *pad = (size_t*)calloc((size_t)P->ntc + 1, sizeof(size_t));
    size_t total = n;
    for (int i = 0; i < P->ntc; ) {
        /* the run of comments on consecutive lines starting at i */
        int j = i + 1;
        while (j < P->ntc) {
            const char *nl = memchr(src + P->tc[j - 1], '\n', n - P->tc[j - 1]);
            if (!nl) break;
            size_t next_start = (size_t)(nl - src) + 1;
            size_t line_start = P->tc[j];
            while (line_start > 0 && src[line_start - 1] != '\n') line_start--;
            if (line_start != next_start) break;
            j++;
        }
        size_t maxw = 0, w[64];
        if (j - i > 1) {
            for (int k = i; k < j; k++) {
                size_t ls = P->tc[k];
                while (ls > 0 && src[ls - 1] != '\n') ls--;
                size_t wk = text_width(src, ls, P->tc[k]);
                if (k - i < 64) w[k - i] = wk;
                if (wk > maxw) maxw = wk;
            }
            for (int k = i; k < j && k - i < 64; k++) {
                pad[k] = maxw - w[k - i];
                total += pad[k];
            }
        }
        i = j;
    }
    char *outb = (char*)malloc(total + 1);
    size_t o = 0, from = 0;
    for (int k = 0; k < P->ntc; k++) {
        memcpy(outb + o, src + from, P->tc[k] - from);
        o += P->tc[k] - from;
        memset(outb + o, ' ', pad[k]);
        o += pad[k];
        from = P->tc[k];
    }
    memcpy(outb + o, src + from, n - from);
    o += n - from;
    free(pad);
    *len = o;
    return outb;
}

/* -- literals and names -- */

/* letters, digits and _, not starting with a digit: a field or method name,
 * or a table key before '=' (keywords allowed there: q:in, {or = f}) */
static bool is_word(const char *s, size_t len) {
    if (!s || len == 0) return false;
    if (!(s[0] == '_' || (s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')))
        return false;
    for (size_t i = 1; i < len; i++) {
        char c = s[i];
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

/* a word that is not a keyword: a variable or parameter name */
static bool is_name(const char *s, size_t len) {
    if (!is_word(s, len)) return false;
    SeaLexer l;
    sea_lexer_init(&l, s);
    bool ok = l.current.type == TK_IDENT && (size_t)l.current.len == len;
    sea_lexer_free(&l);
    return ok;
}

static void print_quoted(Printer *P, const char *s, size_t len) {
    outc(P, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  outs(P, "\\\""); break;
            case '\\': outs(P, "\\\\"); break;
            case '\n': outs(P, "\\n"); break;
            case '\t': outs(P, "\\t"); break;
            case '\r': outs(P, "\\r"); break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char esc[8];
                    snprintf(esc, sizeof esc, "\\x%02x", c);
                    outs(P, esc);
                } else {
                    outc(P, (char)c);
                }
        }
    }
    outc(P, '"');
}

/* Does `raw` lex as exactly one literal token of this kind and value? */
static bool raw_matches(SeaValue raw, SeaValue value, bool is_string) {
    size_t rl;
    const char *r = str_data(raw, &rl);
    if (!r || rl == 0) return false;
    SeaLexer l;
    sea_lexer_init(&l, r);
    SeaToken t = l.current;
    bool ok = false;
    if ((size_t)t.len == rl) {
        if (is_string && t.type == TK_STRING) {
            size_t vl;
            const char *v = str_data(value, &vl);
            ok = v && vl == t.val.string.len && memcmp(v, t.val.string.ptr, vl) == 0;
        } else if (!is_string && t.type == TK_NUMBER) {
            if (sea_is_int(value))
                ok = !t.is_float && t.val.number == (double)sea_as_int(value);
            else if (sea_is_number(value))
                ok = t.is_float && t.val.number == sea_as_number(value);
        }
    }
    if (t.type == TK_STRING) sea_free((void*)t.val.string.ptr, t.val.string.len + 1);
    sea_lexer_free(&l);
    return ok;
}

static void print_number_value(Printer *P, SeaValue v) {
    char b[64];
    if (sea_is_int(v)) {
        snprintf(b, sizeof b, "%lld", (long long)sea_as_int(v));
    } else {
        double d = sea_as_number(v);
        if (isnan(d)) { outs(P, "(0/0)"); return; }
        if (isinf(d)) { outs(P, d > 0 ? "1e999" : "-1e999"); return; }
        for (int prec = 1; prec <= 17; prec++) {      /* shortest exact form */
            snprintf(b, sizeof b, "%.*g", prec, d);
            if (strtod(b, NULL) == d) break;
        }
        if (!strpbrk(b, ".e")) strcat(b, ".0");
    }
    outs(P, b);
}

/* -- expressions -- */

static void print_expr(Printer *P, SeaValue v);
static void print_body(Printer *P, Obj *block, int parent_col, int limit);
static SeaArray *block_stmts(Obj *block);
static void print_inline_stmts(Printer *P, SeaArray *stmts);

static int binop_prec(const char *op) {
    static const struct { const char *op; int prec; } T[] = {
        {"or", PR_OR}, {"and", PR_AND},
        {"==", PR_CMP}, {"!=", PR_CMP}, {"~=", PR_CMP}, {"<", PR_CMP},
        {"<=", PR_CMP}, {">", PR_CMP}, {">=", PR_CMP},
        {"|", PR_BOR}, {"~", PR_BXOR}, {"&", PR_BAND}, {"<<", PR_SHIFT}, {">>", PR_SHIFT},
        {"..", PR_CONCAT}, {"+", PR_ADD}, {"-", PR_ADD},
        {"*", PR_MUL}, {"/", PR_MUL}, {"//", PR_MUL}, {"%", PR_MUL}, {"^", PR_POW},
    };
    for (size_t i = 0; op && i < sizeof T / sizeof T[0]; i++)
        if (strcmp(T[i].op, op) == 0) return T[i].prec;
    return PR_NONE;
}

static bool right_assoc(const char *op) {
    return op && (strcmp(op, "^") == 0 || strcmp(op, "..") == 0);
}

/* How tightly an expression binds, to decide on parentheses. */
static bool has_paren(Obj *n) {
    return n && sea_is_truthy(tget(n, "paren"));
}

static int expr_prec(Obj *n) {
    const char *type = n ? tget_str(n, "type") : NULL;
    if (!type || has_paren(n)) return PR_POSTFIX;   /* printed in its parentheses */
    if (strcmp(type, "binop") == 0) return binop_prec(tget_str(n, "op"));
    if (strcmp(type, "chan_send") == 0 || strcmp(type, "func") == 0) return PR_NONE;
    if (strcmp(type, "unop") == 0 || strcmp(type, "chan_recv") == 0) return PR_UNARY;
    if (strcmp(type, "number") == 0) {
        SeaValue v = tget(n, "value");
        if ((sea_is_int(v) && sea_as_int(v) < 0) ||
            (sea_is_number(v) && signbit(sea_as_number(v)) && !isnan(sea_as_number(v))))
            return PR_UNARY;   /* printed with a leading minus */
    }
    return PR_POSTFIX;
}

/* The object of a postfix operation (call, index, field, method, `?`). */
static bool postfix_paren(Obj *obj) {
    return expr_prec(obj) < PR_POSTFIX || is_type(obj, "number");
}

static bool binop_paren(Obj *binop, Obj *child, bool is_left) {
    if (!child) return false;
    const char *op = tget_str(binop, "op");
    int prec = binop_prec(op), cp = expr_prec(child);
    /* a prefix operator on the right delimits itself: a ^ -b, a * not b */
    if (!is_left && cp == PR_UNARY) return false;
    if (cp < prec) return true;
    return cp == prec && cp != PR_POSTFIX && (is_left ? right_assoc(op) : !right_assoc(op));
}

static void print_paren(Printer *P, SeaValue v, bool paren) {
    paren = paren && !has_paren(as_node(v));
    if (paren) outc(P, '(');
    print_expr(P, v);
    if (paren) outc(P, ')');
}

static Obj *need(Printer *P, Obj *n, const char *field) {
    Obj *c = as_node(tget(n, field));
    if (!c) fail(P, "'%s' node without '%s'", tget_str(n, "type"), field);
    return c;
}

static void print_list(Printer *P, SeaArray *a) {
    for (uint32_t i = 0; a && i < a->len; i++) {
        if (i) outs(P, ", ");
        print_expr(P, a->data[i]);
    }
}

/* A line break the source had inside an expression: continue on the next
 * line. The caller raised the depth one level for the continuation (so the
 * blocks of functions written there indent from it). `line` is the source
 * line being left. */
static void cont_break(Printer *P, int line) {
    attach_trailing(P, line, line);
    outc(P, '\n');
    indent(P);
}

/* Ends with `end` or `}` on a line of its own (the next item follows it). */
static bool ends_multiline(Obj *n) {
    if (has_paren(n)) return false;
    if (is_type(n, "func")) return !one_line(n);
    if (is_type(n, "table")) {
        SeaArray *f = tget_arr(n, "fields");
        return f && f->len && max_line(n, 0) > node_line(n);
    }
    if (is_type(n, "array_lit")) {
        SeaArray *e = tget_arr(n, "elems");
        return e && e->len && max_line(n, 0) > node_line(n);
    }
    return false;
}

/* Call arguments, keeping the line breaks between them. `open` is the line
 * of the opening parenthesis. */
static void print_args(Printer *P, Obj *call) {
    SeaArray *a = tget_arr(call, "args");
    int open = node_line(call);
    int prev_max = open;
    bool broke = false;
    for (uint32_t i = 0; a && i < a->len && !P->failed; i++) {
        Obj *arg = as_node(a->data[i]);
        int lo = arg ? min_line(arg, 0) : 0;
        bool brk = lo > 0 && prev_max > 0 && lo > prev_max &&
                   !(i > 0 && ends_multiline(as_node(a->data[i - 1])));
        if (i) outc(P, ',');
        if (brk) {
            if (!broke) { P->depth++; broke = true; }
            cont_break(P, prev_max);
        } else if (i) {
            outc(P, ' ');
        }
        print_expr(P, a->data[i]);
        prev_max = arg ? max_line(arg, 0) : 0;
    }
    if (broke) P->depth--;
    /* `)` on a line of its own */
    int end = node_end(call);
    if (end > 0 && prev_max > 0 && end > prev_max && a && a->len &&
        !ends_multiline(as_node(a->data[a->len - 1]))) {
        attach_trailing(P, prev_max, prev_max);
        outc(P, '\n');
        indent(P);
    }
}

static void print_name(Printer *P, const char *s, const char *what) {
    if (!s || !(strcmp(what, "method") == 0 ? is_word(s, strlen(s)) : is_name(s, strlen(s)))) {
        fail(P, "invalid %s name '%s'", what, s ? s : "(none)");
        return;
    }
    outs(P, s);
}

static void print_params(Printer *P, Obj *fn, uint32_t first) {
    SeaArray *params = tget_arr(fn, "params");
    outc(P, '(');
    for (uint32_t i = first; params && i < params->len; i++) {
        if (i > first) outs(P, ", ");
        Obj *p = is_record(params->data[i]) ? as_record(params->data[i]) : NULL;
        if (p && sea_is_truthy(tget(p, "is_vararg"))) outs(P, "...");
        else print_name(P, tget_str(p, "name"), "parameter");
    }
    outc(P, ')');
}

/* `function(params)` already printed: the body and `end`. */
static void print_func_rest(Printer *P, Obj *fn) {
    Obj *body = as_node(tget(fn, "body"));
    SeaArray *stmts = tget_arr(body, "stmts");
    if (one_line(fn)) {
        print_inline_stmts(P, stmts);
        outs(P, "end");
        return;
    }
    Obj *c = cm_peek(P);
    int end = node_end(body) > 0 ? node_end(body) : P->limit;
    bool inner_comment = c && node_line(c) > node_line(fn) && node_line(c) < end &&
                         !sea_is_truthy(tget(c, "inline"));
    if ((!stmts || stmts->len == 0) && !inner_comment) {
        outs(P, " end");
        return;
    }
    attach_trailing(P, node_line(fn), node_line(fn));
    outc(P, '\n');
    print_body(P, body, P->stmt_col, P->limit);
    indent(P);
    outs(P, "end");
}

static int field_line(Obj *f) {
    int l = node_line(as_node(tget(f, "key")));
    return l > 0 ? l : node_line(as_node(tget(f, "value")));
}

static void print_field(Printer *P, Obj *f) {
    Obj *key = as_node(tget(f, "key"));
    SeaValue value = tget(f, "value");
    if (key) {
        size_t kl;
        const char *ks = is_type(key, "string") ? str_data(tget(key, "value"), &kl) : NULL;
        if (ks && is_word(ks, kl)) {
            outs(P, ks);
        } else {
            outc(P, '[');
            print_expr(P, sea_obj_val(key));
            outc(P, ']');
        }
        outs(P, " = ");
    }
    print_expr(P, value);
}

static void print_table(Printer *P, Obj *t) {
    SeaArray *fields = tget_arr(t, "fields");
    uint32_t n = fields ? fields->len : 0;
    if (n == 0) { outs(P, "{}"); return; }
    for (uint32_t i = 0; i < n; i++)
        if (!is_record(fields->data[i])) { fail(P, "table field is not a table"); return; }

    /* several lines when the source had them, or a field holds a function */
    int tline = node_line(t);
    bool multi = tline > 0 && node_end(t) > tline;
    for (uint32_t i = 0; i < n && !multi; i++) {
        Obj *f = as_record(fields->data[i]);
        Obj *v = as_node(tget(f, "value"));
        if ((tline > 0 && field_line(f) > tline) || (is_type(v, "func") && !one_line(v)))
            multi = true;
    }
    if (!multi) {
        outc(P, '{');
        for (uint32_t i = 0; i < n; i++) {
            if (i) outs(P, ", ");
            print_field(P, as_record(fields->data[i]));
        }
        outc(P, '}');
        return;
    }

    /* one row per source line: fields written on one line stay together */
    outc(P, '{');
    attach_trailing(P, tline, tline);
    outc(P, '\n');
    P->depth++;
    P->at_start = true;
    for (uint32_t i = 0; i < n; ) {
        Obj *f = as_record(fields->data[i]);
        int line = field_line(f);
        if (line > 0) flush_before(P, line);
        begin_item(P, line);
        print_field(P, f);
        uint32_t j = i + 1;
        while (j < n && line > 0 && field_line(as_record(fields->data[j])) == line &&
               !is_type(as_node(tget(f, "value")), "func") &&
               !is_type(as_node(tget(as_record(fields->data[j]), "value")), "func")) {
            outs(P, ", ");
            print_field(P, as_record(fields->data[j]));
            j++;
        }
        outc(P, ',');
        attach_trailing(P, line, line);
        outc(P, '\n');
        note_line(P, line);
        i = j;
    }
    if (node_end(t) > 0) flush_before(P, node_end(t));   /* comments before `}` */
    P->depth--;
    indent(P);
    outc(P, '}');
}

/* An array literal: one line, or (when the source had several) one row per
 * source line, as table literals do. */
static void print_array(Printer *P, Obj *t) {
    SeaArray *elems = tget_arr(t, "elems");
    uint32_t n = elems ? elems->len : 0;
    if (n == 0) { outs(P, "[]"); return; }
    int tline = node_line(t);
    bool multi = tline > 0 && node_end(t) > tline;
    for (uint32_t i = 0; i < n && !multi; i++) {
        Obj *e = as_node(elems->data[i]);
        if (is_type(e, "func") && !one_line(e)) multi = true;
    }
    /* "[[" would open a long string: [ [1, 2], [3] ] keeps a space */
    Obj *first = as_node(elems->data[0]);
    bool pad = false;
    if (is_type(first, "array_lit") || is_type(first, "map_lit")) pad = true;
    if (is_type(first, "string")) {
        const char *raw = tget_str(first, "raw");
        if (raw && raw[0] == '[') pad = true;
    }
    if (!multi) {
        outs(P, pad ? "[ " : "[");
        print_list(P, elems);
        outs(P, pad ? " ]" : "]");
        return;
    }
    outc(P, '[');
    attach_trailing(P, tline, tline);
    outc(P, '\n');
    P->depth++;
    P->at_start = true;
    for (uint32_t i = 0; i < n && !P->failed; ) {
        Obj *e = as_node(elems->data[i]);
        int line = e ? min_line(e, 0) : 0;
        if (line > 0) flush_before(P, line);
        begin_item(P, line);
        print_expr(P, elems->data[i]);
        uint32_t j = i + 1;
        while (j < n && line > 0 && !is_type(e, "func")) {
            Obj *next = as_node(elems->data[j]);
            if (!next || min_line(next, 0) != line || is_type(next, "func")) break;
            outs(P, ", ");
            print_expr(P, elems->data[j]);
            j++;
        }
        outc(P, ',');
        attach_trailing(P, line, line);
        outc(P, '\n');
        note_line(P, line);
        i = j;
    }
    if (node_end(t) > 0) flush_before(P, node_end(t));   /* comments before `]` */
    P->depth--;
    indent(P);
    outc(P, ']');
}

/* `text ${expr} text`: the texts as written (raws), or escaped from the
 * decoded parts for a tree built by hand. */
static void print_template(Printer *P, Obj *n) {
    SeaArray *parts = tget_arr(n, "parts");
    SeaArray *raws = tget_arr(n, "raws");
    SeaArray *exprs = tget_arr(n, "exprs");
    uint32_t ne = exprs ? exprs->len : 0;
    if (!parts || parts->len != ne + 1) { fail(P, "'template' node needs one more part than exprs"); return; }
    outc(P, '`');
    for (uint32_t i = 0; i <= ne && !P->failed; i++) {
        size_t len;
        const char *raw = raws && raws->len == ne + 1 ? str_data(raws->data[i], &len) : NULL;
        if (raw) {
            outs(P, raw);
        } else {
            const char *s = str_data(parts->data[i], &len);
            if (!s) { fail(P, "template part is not a string"); return; }
            for (size_t k = 0; k < len; k++) {
                char ch = s[k];
                if (ch == '\\') outs(P, "\\\\");
                else if (ch == '`') outs(P, "\\`");
                else if (ch == '$' && k + 1 < len && s[k + 1] == '{') outs(P, "\\$");
                else if (ch == '\t') outs(P, "\\t");
                else if (ch == '\r') outs(P, "\\r");
                else if (ch == '\0') outs(P, "\\0");
                else outc(P, ch);
            }
        }
        if (i < ne) {
            outs(P, "${");
            print_expr(P, exprs->data[i]);
            outc(P, '}');
        }
    }
    outc(P, '`');
}

static void print_string_node(Printer *P, Obj *n) {
    SeaValue v = tget(n, "value");
    size_t len;
    const char *s = str_data(v, &len);
    if (!s) { fail(P, "'string' node without a string value"); return; }
    SeaValue raw = tget(n, "raw");
    if (!sea_is_nil(raw) && raw_matches(raw, v, true)) { outs(P, str_data(raw, NULL)); return; }
    print_quoted(P, s, len);
}

static void print_number_node(Printer *P, Obj *n) {
    SeaValue v = tget(n, "value");
    if (!sea_is_int(v) && !sea_is_number(v)) { fail(P, "'number' node without a number value"); return; }
    SeaValue raw = tget(n, "raw");
    if (!sea_is_nil(raw) && raw_matches(raw, v, false)) { outs(P, str_data(raw, NULL)); return; }
    print_number_value(P, v);
}

static void print_expr(Printer *P, SeaValue v) {
    if (P->failed) return;
    Obj *n = as_node(v);
    if (!n) { fail(P, "expected a node, got a %s", sea_type_name(v)); return; }
    if (++P->nest > FMT_MAX_NEST) { fail(P, "tree too deep (a cycle?)"); P->nest--; return; }
    note_line(P, node_line(n));
    const char *type = tget_str(n, "type");
    bool user_paren = has_paren(n);
    if (user_paren) outc(P, '(');

    if (strcmp(type, "nil") == 0) outs(P, "nil");
    else if (strcmp(type, "bool") == 0) outs(P, sea_is_truthy(tget(n, "value")) ? "true" : "false");
    else if (strcmp(type, "vararg") == 0) outs(P, "...");
    else if (strcmp(type, "number") == 0) print_number_node(P, n);
    else if (strcmp(type, "string") == 0) print_string_node(P, n);
    else if (strcmp(type, "ident") == 0) print_name(P, tget_str(n, "name"), "variable");
    else if (strcmp(type, "map_lit") == 0) outs(P, "[=]");
    else if (strcmp(type, "array_lit") == 0) {
        print_array(P, n);
    } else if (strcmp(type, "template") == 0) {
        print_template(P, n);
    } else if (strcmp(type, "table") == 0) {
        print_table(P, n);
    } else if (strcmp(type, "unop") == 0 || strcmp(type, "chan_recv") == 0) {
        bool recv = type[0] == 'c';
        const char *op = recv ? "<-" : tget_str(n, "op");
        Obj *operand = need(P, n, recv ? "channel" : "operand");
        if (!operand) goto done;
        if (!op || (strcmp(op, "-") && strcmp(op, "not") && strcmp(op, "~") && strcmp(op, "<-"))) {
            fail(P, "unknown unary operator '%s'", op ? op : "(none)");
            goto done;
        }
        outs(P, op);
        if (strcmp(op, "not") == 0) outc(P, ' ');
        int cp = expr_prec(operand);
        /* -(-x): two minus signs in a row would start a comment */
        const char *oop = tget_str(operand, "op");
        bool minus = !has_paren(operand) &&
                     ((is_type(operand, "unop") && oop && strcmp(oop, "-") == 0) ||
                      (is_type(operand, "number") && cp == PR_UNARY));
        print_paren(P, sea_obj_val(operand),
                    (cp < PR_UNARY) || (minus && op[strlen(op) - 1] == '-'));
    } else if (strcmp(type, "binop") == 0) {
        const char *op = tget_str(n, "op");
        Obj *l = need(P, n, "left"), *r = need(P, n, "right");
        if (!l || !r) goto done;
        if (binop_prec(op) == PR_NONE) { fail(P, "unknown binary operator '%s'", op ? op : "(none)"); goto done; }
        print_paren(P, sea_obj_val(l), binop_paren(n, l, true));
        outc(P, ' ');
        outs(P, op);
        /* a break around the operator in the source: after it here */
        int opl = node_line(n), rl = min_line(r, 0), ll = max_line(l, 0);
        if (opl > 0 && ((rl > opl) || (ll > 0 && opl > ll)) && !ends_multiline(l)) {
            P->depth++;
            cont_break(P, rl > opl ? opl : ll);
            print_paren(P, sea_obj_val(r), binop_paren(n, r, false));
            P->depth--;
        } else {
            outc(P, ' ');
            print_paren(P, sea_obj_val(r), binop_paren(n, r, false));
        }
    } else if (strcmp(type, "chan_send") == 0) {
        Obj *ch = need(P, n, "channel"), *val = need(P, n, "value");
        if (!ch || !val) goto done;
        print_paren(P, sea_obj_val(ch), postfix_paren(ch));
        outs(P, " <- ");
        print_expr(P, sea_obj_val(val));
    } else if (strcmp(type, "index") == 0) {
        Obj *obj = need(P, n, "object");
        if (!obj) goto done;
        print_paren(P, sea_obj_val(obj), postfix_paren(obj));
        outc(P, '[');
        if (as_node(tget(n, "index"))) print_expr(P, tget(n, "index"));
        outc(P, ']');
    } else if (strcmp(type, "field") == 0) {
        Obj *obj = need(P, n, "object");
        if (!obj) goto done;
        print_paren(P, sea_obj_val(obj), postfix_paren(obj));
        chain_break(P, n, obj);
        size_t fl;
        const char *f = str_data(tget(n, "field"), &fl);
        if (!f) { fail(P, "'field' node without 'field'"); goto done; }
        if (is_word(f, fl)) { outc(P, '.'); outs(P, f); }
        else { outc(P, '['); print_quoted(P, f, fl); outc(P, ']'); }
    } else if (strcmp(type, "call") == 0) {
        Obj *callee = need(P, n, "callee");
        if (!callee) goto done;
        print_paren(P, sea_obj_val(callee), postfix_paren(callee));
        outc(P, '(');
        print_args(P, n);
        outc(P, ')');
    } else if (strcmp(type, "method_call") == 0) {
        Obj *obj = need(P, n, "object");
        if (!obj) goto done;
        print_paren(P, sea_obj_val(obj), postfix_paren(obj));
        int broke = chain_break(P, n, obj);
        outc(P, ':');
        print_name(P, tget_str(n, "method"), "method");
        outc(P, '(');
        P->depth += broke;
        print_args(P, n);
        P->depth -= broke;
        outc(P, ')');
    } else if (strcmp(type, "propagate") == 0) {
        Obj *e = need(P, n, "expr");
        if (!e) goto done;
        print_paren(P, sea_obj_val(e), postfix_paren(e));
        outc(P, '?');
    } else if (strcmp(type, "func") == 0) {
        outs(P, "function");
        print_params(P, n, 0);
        print_func_rest(P, n);
    } else {
        fail(P, "'%s' is not an expression", type);
    }
done:
    if (user_paren) outc(P, ')');
    P->nest--;
}

/* Would the statement's text start with '('? Then the parser would read it
 * as a call continuing the previous line. */
static bool starts_with_paren(Obj *n) {
    for (int guard = 0; n && guard < FMT_MAX_NEST; guard++) {
        if (has_paren(n)) return true;
        Obj *child;
        bool paren;
        if (is_type(n, "call")) { child = as_node(tget(n, "callee")); paren = postfix_paren(child); }
        else if (is_type(n, "method_call") || is_type(n, "index") || is_type(n, "field")) {
            child = as_node(tget(n, "object")); paren = postfix_paren(child);
        } else if (is_type(n, "propagate")) { child = as_node(tget(n, "expr")); paren = postfix_paren(child); }
        else if (is_type(n, "chan_send")) { child = as_node(tget(n, "channel")); paren = postfix_paren(child); }
        else if (is_type(n, "binop")) { child = as_node(tget(n, "left")); paren = binop_paren(n, child, true); }
        else return false;
        if (!child) return false;
        if (paren) return true;
        n = child;
    }
    return false;
}

/* -- statements -- */

static void print_stmt(Printer *P, Obj *s, int limit);

/* The statements of a block (or `do` node), one level deeper. `limit` is the
 * line where the block ends when the node does not say (end_line). */
static void print_body(Printer *P, Obj *block, int parent_col, int limit) {
    SeaArray *stmts = block_stmts(block);
    int end = node_end(block);
    if (end > 0) limit = end;
    P->depth++;
    P->at_start = true;
    uint32_t n = stmts ? stmts->len : 0;
    for (uint32_t i = 0; i < n && !P->failed; i++) {
        Obj *s = as_node(stmts->data[i]);
        if (!s) { fail(P, "a block holds a %s, not a node", sea_type_name(stmts->data[i])); break; }
        int line = node_line(s);
        int next = limit;
        if (i + 1 < n) {
            int nl = node_line(as_node(stmts->data[i + 1]));
            if (nl > 0) next = nl;
        }
        if (line > 0) flush_before(P, line);
        print_stmt(P, s, next);
    }
    if (end > 0) flush_before(P, end);     /* comments before its `end` */
    else flush_block_end(P, limit, parent_col);
    P->depth--;
}

static SeaArray *block_stmts(Obj *block) {
    return tget_arr(block, "stmts");
}

/* first line of a block (to bound the comments of the block before it) */
static int block_first_line(Obj *block, int dflt) {
    SeaArray *a = block_stmts(block);
    int l = (a && a->len) ? node_line(as_node(a->data[0])) : 0;
    return l > 0 ? l : dflt;
}

/* a.b.c — only names */
static bool is_dotted(Obj *n) {
    for (int guard = 0; n && guard < FMT_MAX_NEST; guard++) {
        if (has_paren(n)) return false;
        if (is_type(n, "ident")) {
            const char *nm = tget_str(n, "name");
            return nm && is_name(nm, strlen(nm));
        }
        if (!is_type(n, "field")) return false;
        size_t fl;
        const char *f = str_data(tget(n, "field"), &fl);
        if (!f || !is_word(f, fl)) return false;
        n = as_node(tget(n, "object"));
    }
    return false;
}

/* A one-line statement, without indentation or newline (also used for the
 * statement of a select case). False if `s` is not one. */
static bool print_simple(Printer *P, Obj *s) {
    const char *type = tget_str(s, "type");
    if (strcmp(type, "local") == 0) {
        outs(P, sea_is_truthy(tget(s, "is_const")) ? "const " : "local ");
        SeaArray *names = tget_arr(s, "names");
        if (!names || names->len == 0) { fail(P, "'local' node without names"); return true; }
        for (uint32_t i = 0; i < names->len; i++) {
            if (i) outs(P, ", ");
            print_name(P, str_data(names->data[i], NULL), "variable");
        }
        SeaArray *values = tget_arr(s, "values");
        if (values && values->len) { outs(P, " = "); print_list(P, values); }
    } else if (strcmp(type, "assign") == 0) {
        SeaArray *targets = tget_arr(s, "targets"), *values = tget_arr(s, "values");
        if (!targets || !targets->len || !values || !values->len) {
            fail(P, "'assign' node without targets or values");
            return true;
        }
        if (starts_with_paren(as_node(targets->data[0]))) outc(P, ';');
        print_list(P, targets);
        const char *op = tget_str(s, "op");
        if (op) {
            static const char *const OPS[] = {"+", "-", "*", "/", "//", "%", "^", ".."};
            bool known = false;
            for (size_t k = 0; k < sizeof OPS / sizeof OPS[0]; k++)
                if (strcmp(op, OPS[k]) == 0) known = true;
            if (!known || targets->len != 1 || values->len != 1) {
                fail(P, "'assign' with op '%s' needs one target and one value (op one of + - * / // %% ^ ..)", op);
                return true;
            }
            outc(P, ' ');
            outs(P, op);
            outs(P, "= ");
        } else {
            outs(P, " = ");
        }
        print_list(P, values);
    } else if (strcmp(type, "expr_stmt") == 0) {
        Obj *e = need(P, s, "expr");
        if (!e) return true;
        if (starts_with_paren(e)) outc(P, ';');
        print_expr(P, sea_obj_val(e));
    } else if (strcmp(type, "return") == 0) {
        outs(P, "return");
        SeaArray *values = tget_arr(s, "values");
        if (values && values->len) { outc(P, ' '); print_list(P, values); }
    } else if (strcmp(type, "break") == 0) {
        outs(P, "break");
    } else if (strcmp(type, "continue") == 0) {
        outs(P, "continue");
    } else if (strcmp(type, "go") == 0 || strcmp(type, "defer") == 0) {
        Obj *c = need(P, s, "call");
        if (!c) return true;
        outs(P, type);
        outc(P, ' ');
        print_expr(P, sea_obj_val(c));
    } else if (strcmp(type, "panic") == 0) {
        outs(P, "panic(");
        print_expr(P, tget(s, "value"));
        outc(P, ')');
    } else if (strcmp(type, "del") == 0) {
        outs(P, "del ");
        print_expr(P, tget(s, "target"));
    } else {
        return false;
    }
    return true;
}

/* The body of a block statement after its header: on the same line (inl),
 * or indented on the next lines, ending with the indentation of the closing
 * keyword, which the caller prints. */
static void block_part(Printer *P, Obj *hdr, Obj *block, int parent_col, int limit, bool inl) {
    if (inl) { print_inline_stmts(P, block_stmts(block)); return; }
    if (hdr) attach_trailing(P, node_line(hdr), node_line(hdr));
    outc(P, '\n');
    print_body(P, block, parent_col, limit);
    indent(P);
}

/* Function statements: `local function f`, `function a.b:c`. */
static bool print_func_stmt(Printer *P, Obj *s, int limit, bool inl) {
    bool is_local = is_type(s, "local");
    SeaArray *names = tget_arr(s, is_local ? "names" : "targets");
    SeaArray *values = tget_arr(s, "values");
    if (!names || !values || names->len != 1 || values->len != 1) return false;
    Obj *fn = as_node(values->data[0]);
    const char *fname = fn && is_type(fn, "func") ? tget_str(fn, "name") : NULL;
    if (!fname || has_paren(fn)) return false;
    bool method = sea_is_truthy(tget(fn, "is_method"));

    if (is_local) {
        const char *nm = str_data(names->data[0], NULL);
        if (method || sea_is_truthy(tget(s, "is_const")) || !nm || strcmp(nm, fname) != 0 ||
            !is_name(nm, strlen(nm)))
            return false;
        outs(P, "local function ");
        outs(P, nm);
        print_params(P, fn, 0);
    } else {
        Obj *target = as_node(names->data[0]);
        if (method) {
            SeaArray *params = tget_arr(fn, "params");
            Obj *p0 = (params && params->len && is_record(params->data[0]))
                           ? as_record(params->data[0]) : NULL;
            const char *self = p0 ? tget_str(p0, "name") : NULL;
            if (!is_type(target, "field") || !self || strcmp(self, "self") != 0 ||
                !is_dotted(target))
                return false;
            Obj *obj = as_node(tget(target, "object"));
            outs(P, "function ");
            print_expr(P, sea_obj_val(obj));
            outc(P, ':');
            outs(P, tget_str(target, "field"));
            print_params(P, fn, 1);
        } else {
            if (!is_dotted(target)) return false;
            outs(P, "function ");
            print_expr(P, sea_obj_val(target));
            print_params(P, fn, 0);
        }
    }
    block_part(P, s, as_node(tget(fn, "body")), node_col(s), limit, inl);
    outs(P, "end");
    return true;
}

static void print_select(Printer *P, Obj *s, int limit) {
    int line = node_line(s);
    outs(P, "select");
    attach_trailing(P, line, line);
    outc(P, '\n');
    SeaArray *cases = tget_arr(s, "cases");
    P->depth++;
    for (uint32_t i = 0; cases && i < cases->len && !P->failed; i++) {
        Obj *c = as_record(cases->data[i]);
        if (!c) { fail(P, "select case is not a table"); break; }
        Obj *cs = as_node(tget(c, "stmt"));
        Obj *body = as_node(tget(c, "body"));
        int cline = cs ? node_line(cs) : block_first_line(body, 0) - 1;
        int next = limit;
        Obj *nc = i + 1 < cases->len ? as_record(cases->data[i + 1]) : NULL;
        if (nc) {
            Obj *ns = as_node(tget(nc, "stmt"));
            int nl = ns ? node_line(ns) : block_first_line(as_node(tget(nc, "body")), 0) - 1;
            if (nl > 0) next = nl;
        }
        if (cline > 0) flush_before(P, cline);
        indent(P);
        outs(P, "case ");
        if (sea_is_truthy(tget(c, "is_default"))) outs(P, "default");
        else if (!cs || !print_simple(P, cs)) { fail(P, "select case without a statement"); break; }
        outs(P, " then");
        attach_trailing(P, cline, cline);
        outc(P, '\n');
        print_body(P, body, P->stmt_col + 1, next);
    }
    P->depth--;
    indent(P);
    outs(P, "end");
}

/* Block statements (if, loops, do, select, function statements). False if
 * `s` is not one. */
static bool print_compound(Printer *P, Obj *s, int limit, bool inl) {
    const char *type = tget_str(s, "type");
    if (strcmp(type, "local") == 0 || strcmp(type, "assign") == 0)
        return print_func_stmt(P, s, limit, inl);
    if (strcmp(type, "if") == 0) {
        Obj *n = s;
        outs(P, "if ");
        for (;;) {
            P->depth++;     /* a broken condition goes one level deeper than the body */
            print_expr(P, tget(n, "cond"));
            P->depth--;
            outs(P, " then");
            Obj *els = as_node(tget(n, "else_body"));
            int then_limit = !els ? limit
                           : is_type(els, "if") ? (node_line(els) > 0 ? node_line(els) : limit)
                           : block_first_line(els, limit);
            block_part(P, n, as_node(tget(n, "then_body")), P->stmt_col, then_limit, inl);
            if (!els || P->failed) break;
            if (is_type(els, "if")) {
                outs(P, "elseif ");
                n = els;
                continue;
            }
            outs(P, "else");
            block_part(P, NULL, els, P->stmt_col, limit, inl);
            break;
        }
    } else if (strcmp(type, "while") == 0) {
        outs(P, "while ");
        P->depth++;
        print_expr(P, tget(s, "cond"));
        P->depth--;
        outs(P, " do");
        block_part(P, s, as_node(tget(s, "body")), P->stmt_col, limit, inl);
    } else if (strcmp(type, "for_num") == 0) {
        outs(P, "for ");
        print_name(P, tget_str(s, "var"), "variable");
        outs(P, " = ");
        print_expr(P, tget(s, "start"));
        outs(P, ", ");
        print_expr(P, tget(s, "limit"));
        if (as_node(tget(s, "step"))) { outs(P, ", "); print_expr(P, tget(s, "step")); }
        outs(P, " do");
        block_part(P, s, as_node(tget(s, "body")), P->stmt_col, limit, inl);
    } else if (strcmp(type, "for_in") == 0) {
        outs(P, "for ");
        SeaArray *vars = tget_arr(s, "vars");
        if (!vars || !vars->len) fail(P, "'for_in' node without vars");
        for (uint32_t i = 0; vars && i < vars->len; i++) {
            if (i) outs(P, ", ");
            print_name(P, str_data(vars->data[i], NULL), "variable");
        }
        outs(P, " in ");
        print_expr(P, tget(s, "iter"));
        outs(P, " do");
        block_part(P, s, as_node(tget(s, "body")), P->stmt_col, limit, inl);
    } else if (strcmp(type, "do") == 0) {
        outs(P, "do");
        block_part(P, s, s, P->stmt_col, limit, inl);
    } else if (strcmp(type, "select") == 0) {
        print_select(P, s, limit);
        return true;
    } else {
        return false;
    }
    outs(P, "end");
    return true;
}

/* A statement inside a one-line block. */
static void print_stmt_inline(Printer *P, Obj *s) {
    if (P->failed) return;
    if (print_compound(P, s, 0, true) || print_simple(P, s)) return;
    fail(P, "'%s' is not a statement", tget_str(s, "type"));
}

/* " a; b " — the statements of a one-line block */
static void print_inline_stmts(Printer *P, SeaArray *stmts) {
    uint32_t n = stmts ? stmts->len : 0;
    for (uint32_t i = 0; i < n && !P->failed; i++) {
        Obj *s = as_node(stmts->data[i]);
        if (!s) { fail(P, "a block holds a %s, not a node", sea_type_name(stmts->data[i])); return; }
        outc(P, ' ');
        print_stmt_inline(P, s);
        if (i + 1 < n) outc(P, ';');
    }
    outc(P, ' ');
}

static void print_stmt(Printer *P, Obj *s, int limit) {
    if (P->failed) return;
    if (++P->nest > FMT_MAX_NEST) { fail(P, "tree too deep (a cycle?)"); P->nest--; return; }
    int saved_col = P->stmt_col, saved_limit = P->limit;
    int line = node_line(s);
    P->stmt_col = node_col(s);
    P->limit = limit;
    size_t start = P->len;

    begin_item(P, line);
    note_line(P, line);

    bool inl = one_line(s);
    if (print_compound(P, s, limit, inl)) {
        if (inl) {
            attach_trailing(P, line, line);
        } else {
            /* a comment after `end` on its line */
            int cl = max_line(s, 0);
            if (cl > line) {
                attach_trailing(P, cl, cl);
            } else {
                Obj *c = cm_peek(P);
                if (c && sea_is_truthy(tget(c, "inline")) && node_line(c) < limit)
                    emit_trailing(P, c);
            }
        }
    } else if (print_simple(P, s)) {
        attach_trailing(P, line, line > 0 ? max_line(s, 0) : 0);
    } else {
        fail(P, "'%s' is not a statement", tget_str(s, "type"));
    }
    outc(P, '\n');

    /* estimate where the statement ended in the source (AST without text) */
    if (line > 0 && !P->blank) {
        int ml = max_line(s, 0), nl = 0;
        for (size_t i = start; i < P->len; i++) if (P->buf[i] == '\n') nl++;
        if (nl > 1 && ml == line) ml++;   /* no end positions: guess */
        note_line(P, ml);
    }
    P->stmt_col = saved_col;
    P->limit = saved_limit;
    P->nest--;
}

static bool is_statement(Obj *n) {
    static const char *const T[] = {
        "local", "assign", "expr_stmt", "return", "break", "continue", "go", "defer",
        "panic", "del", "if", "while", "for_num", "for_in", "do", "select",
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (is_type(n, T[i])) return true;
    return false;
}

/* format(src_or_ast [, {indent}]) */
static SeaValue fn_format(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) return errorf(vm, "parser.format: expected source text or a node");
    Obj *opts = opts_table(argc, args, 1);

    Printer P;
    memset(&P, 0, sizeof P);
    P.ind = "    ";
    char indbuf[17];
    SeaValue iv = tget(opts, "indent");
    if (sea_is_int(iv) && sea_as_int(iv) >= 0 && sea_as_int(iv) <= 16) {
        memset(indbuf, ' ', (size_t)sea_as_int(iv));
        indbuf[sea_as_int(iv)] = '\0';
        P.ind = indbuf;
    } else if (str_data(iv, NULL)) {
        P.ind = str_data(iv, NULL);
    }

    SeaValue ast;
    bool *blank = NULL;
    size_t len;
    const char *src = str_data(args[0], &len);
    if (src) {
        ast = parse_source(vm, src, len, opt_name(opts, "<input>"), true);
        if (sea_is_error(ast)) return ast;
        /* the source's empty lines */
        int nl = 1;
        for (size_t i = 0; i < len; i++) if (src[i] == '\n') nl++;
        blank = (bool*)calloc((size_t)nl, sizeof(bool));
        int line = 0;
        bool empty = true;
        for (size_t i = 0; i <= len; i++) {
            char c = i < len ? src[i] : '\n';
            if (c == '\n') { blank[line++] = empty; empty = true; }
            else if (c != ' ' && c != '\t' && c != '\r') empty = false;
        }
        P.blank = blank;
        P.nblank = nl;
    } else if (as_node(args[0])) {
        ast = sea_val_ref(args[0]);
    } else {
        return errorf(vm, "parser.format: expected source text or a node");
    }

    Obj *root = as_record(ast);
    P.comments = tget_arr(root, "comments");
    P.limit = 1 << 30;
    if (is_type(root, "block")) {
        P.depth = -1;          /* print_body indents one level */
        print_body(&P, root, 0, 1 << 30);
        P.depth = 0;
        P.at_start = P.len == 0;
        Obj *c;
        while ((c = cm_peek(&P))) emit_comment_line(&P, c);
    } else if (is_statement(root)) {
        print_stmt(&P, root, 1 << 30);
    } else {
        print_expr(&P, ast);
    }

    SeaValue result;
    if (P.failed) {
        result = errorf(vm, "parser.format: %s", P.err);
    } else if (P.ntc > 1) {
        size_t n;
        char *aligned = align_comments(&P, &n);
        result = str_n(aligned, n);
        free(aligned);
    } else {
        result = str_n(P.buf ? P.buf : "", P.len);
    }
    free(P.buf);
    free(P.tc);
    free(blank);
    sea_obj_release(root);
    return result;
}

/* ---- globals ---- */

static int cmp_str_ptr(const void *a, const void *b) {
    const SeaString *x = *(SeaString *const *)a, *y = *(SeaString *const *)b;
    return strcmp(x->data, y->data);
}

/* globals() → the names of the global variables defined now (the built-in
 * functions and modules, and any the program set), sorted. For linters. */
static SeaValue fn_globals(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    SeaTableSync *ts = &vm->shared->globals_sync;
    pthread_mutex_lock(&ts->mu);
    SeaTable *g = vm_globals(vm);
    uint32_t n = 0, cap = sea_table_count(g) + 1;
    SeaString **names = malloc(cap * sizeof *names);
    uint32_t it = 0;
    SeaValue k, v;
    while (names && sea_table_next(g, &it, &k, &v)) {
        if (!sea_is_string(k) || sea_is_nil(v) || n >= cap) continue;
        SeaString *s = SEA_AS_STRING(k);
        bool ident = s->len > 0 && !(s->data[0] >= '0' && s->data[0] <= '9');
        for (uint32_t i = 0; ident && i < s->len; i++) {
            unsigned char c = (unsigned char)s->data[i];
            ident = c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        }
        if (!ident) continue;
        sea_obj_retain(&s->obj);
        names[n++] = s;
    }
    pthread_mutex_unlock(&ts->mu);
    SeaArray *out = sea_array_new(n);
    if (names) {
        qsort(names, n, sizeof *names, cmp_str_ptr);
        for (uint32_t i = 0; i < n; i++) {
            sea_array_push(out, sea_obj_val(&names[i]->obj));
            sea_obj_release(&names[i]->obj);
        }
        free(names);
    }
    return sea_obj_val(&out->obj);
}

/* ---- registration ---- */

void sea_lib_parser_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);
    static const struct { const char *name; NativeFn fn; } F[] = {
        {"parse",      fn_parse},
        {"parse_file", fn_parse_file},
        {"tokenize",   fn_tokenize},
        {"check",      fn_check},
        {"children",   fn_children},
        {"walk",       fn_walk},
        {"format",     fn_format},
        {"globals",    fn_globals},
    };
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
        SeaClosure *cl = sea_native_closure(F[i].name, F[i].fn);
        tset(mod, F[i].name, sea_obj_val(&cl->obj));
    }
    sea_vm_register_module(vm, "sea/parser", mod);
    sea_obj_release(&mod->obj);
}
