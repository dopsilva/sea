/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_parser.h"
#include "sea_memory.h"
#include <stdarg.h>

/* Nesting limits: the parser and the compiler recurse on the tree, and each
 * nested function costs the compiler a large frame, so deep input must give
 * an error instead of overflowing the C stack. */
#define SEA_PARSER_MAX_DEPTH 1000   /* expressions and blocks */
#define SEA_PARSER_MAX_FUNCS 100    /* nested functions */

// ---- Parser helpers ----

static SeaToken cur(SeaParser *p) {
    return p->lexer.current;
}

static SeaToken peek(SeaParser *p) {
    return sea_lexer_peek(&p->lexer);
}

/* A name after '.' or ':', in `function a.b:c`, or a table key before '=':
 * an identifier or a keyword (q:in, t.select, {or = f}), since the position
 * leaves no doubt that a name comes next. */
static bool is_name_tok(SeaToken t) {
    if (t.type == TK_IDENT) return true;
    if (t.len <= 0) return false;
    unsigned char c = (unsigned char)t.start[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) return false;
    for (int i = 1; i < t.len; i++) {
        c = (unsigned char)t.start[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

static SeaToken advance(SeaParser *p) {
    SeaToken t = sea_lexer_next(&p->lexer);
    p->last_line = t.line;
    p->last_tk = (int)t.type;
    return t;
}

static bool check(SeaParser *p, SeaTK tk) {
    return cur(p).type == tk;
}

static bool match(SeaParser *p, SeaTK tk) {
    if (cur(p).type == tk) {
        advance(p);
        return true;
    }
    return false;
}

/* Record the first error: the message (without position) goes in err_msg,
 * its position in err_line/err_col. */
static void parser_error_va(SeaParser *p, int line, int col,
                            const char *fmt, va_list args) {
    if (p->had_error) return;
    p->had_error = true;
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, args);
    if (p->err_msg) sea_free(p->err_msg, strlen(p->err_msg)+1);
    p->err_msg = (char*)sea_malloc(strlen(msg)+1);
    strcpy(p->err_msg, msg);
    p->err_line = line;
    p->err_col = col;
}

static void parser_error_at(SeaParser *p, int line, int col, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    parser_error_va(p, line, col, fmt, args);
    va_end(args);
}

/* An error at the current token. A lexer error token carries its own message. */
static void parser_error(SeaParser *p, const char *fmt, ...) {
    SeaToken t = cur(p);
    if (t.type == TK_ERROR) {
        parser_error_at(p, t.line, t.col, "%.*s", t.len, t.start);
        return;
    }
    va_list args;
    va_start(args, fmt);
    parser_error_va(p, t.line, t.col, fmt, args);
    va_end(args);
}

/* "expected a name": when the token is a reserved word, say so (a bare
 * "expected variable name" for `local default = 1` sends the reader hunting). */
static void parser_name_error(SeaParser *p, const char *what) {
    SeaToken t = cur(p);
    if (t.type != TK_ERROR && t.type != TK_IDENT && is_name_tok(t))
        parser_error(p, "%s: '%.*s' is a reserved word, pick another name", what, t.len, t.start);
    else
        parser_error(p, "%s", what);
}

static bool expect(SeaParser *p, SeaTK tk) {
    if (cur(p).type == tk) {
        advance(p);
        return true;
    }
    parser_error(p, "expected '%s' but got '%s'", sea_tk_name(tk), sea_tk_name(cur(p).type));
    return false;
}

// Synchronize on error (skip to next statement boundary)
static void synchronize(SeaParser *p) {
    p->had_error = false;
    while (!check(p, TK_EOF)) {
        SeaTK t = cur(p).type;
        if (t == TK_END || t == TK_ELSE || t == TK_ELSEIF ||
            t == TK_RETURN || t == TK_FUNCTION || t == TK_LOCAL ||
            t == TK_IF || t == TK_FOR || t == TK_WHILE || t == TK_DO) {
            return;
        }
        advance(p);
    }
}

static SeaNode* node_new(SeaNodeType type, int line, int col) {
    SeaNode *n = sea_node_alloc(type, line);
    n->col = col;
    return n;
}

static char* dup_str(const char *s, size_t len) {
    char *out = (char*)sea_malloc(len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static char* dup_cstr(const char *s) {
    return dup_str(s, strlen(s));
}

// ---- Forward declarations ----
static SeaNode* parse_expr(SeaParser *p);
static SeaNode* parse_template(SeaParser *p, SeaToken t);
static SeaNode* parse_stmt(SeaParser *p);
static SeaNode* parse_block(SeaParser *p);
static SeaNode* parse_simple_expr(SeaParser *p);

// ---- Expression parsing (Pratt parser) ----

typedef enum {
    PREC_NONE = 0,
    PREC_OR,       // or
    PREC_AND,      // and
    PREC_COMPARE,  // == ~= < <= > >=
    PREC_BOR,      // |
    PREC_BXOR,     // ~
    PREC_BAND,     // &
    PREC_SHIFT,    // << >>
    PREC_CONCAT,   // ..
    PREC_ADD,      // + -
    PREC_MUL,      // * / // %
    PREC_UNARY,    // not # ~ - <-
    PREC_POW,      // ^ (right assoc)
    PREC_POSTFIX,  // . [] () :
} Prec;

static Prec token_infix_prec(SeaTK tk) {
    switch (tk) {
        case TK_OR:        return PREC_OR;
        case TK_AND:       return PREC_AND;
        case TK_EQ:
        case TK_NEQ:
        case TK_LT:
        case TK_LE:
        case TK_GT:
        case TK_GE:        return PREC_COMPARE;
        case TK_PIPE:      return PREC_BOR;
        case TK_TILDE:     return PREC_BXOR;
        case TK_AMP:       return PREC_BAND;
        case TK_LSHIFT:
        case TK_RSHIFT:    return PREC_SHIFT;
        case TK_DOTDOT:    return PREC_CONCAT;
        case TK_PLUS:
        case TK_MINUS:     return PREC_ADD;
        case TK_STAR:
        case TK_SLASH:
        case TK_DSLASH:
        case TK_PERCENT:   return PREC_MUL;
        case TK_CARET:     return PREC_POW;
        case TK_DOT:
        case TK_LBRACKET:
        case TK_LPAREN:
        case TK_COLON:     return PREC_POSTFIX;
        case TK_ARROW_LEFT: return PREC_POSTFIX; // chan send in expression
        case TK_QUESTION:  return PREC_POSTFIX;
        default:           return PREC_NONE;
    }
}

static SeaBinop tk_to_binop(SeaTK tk) {
    switch (tk) {
        case TK_PLUS:    return BINOP_ADD;
        case TK_MINUS:   return BINOP_SUB;
        case TK_STAR:    return BINOP_MUL;
        case TK_SLASH:   return BINOP_DIV;
        case TK_DSLASH:  return BINOP_IDIV;
        case TK_PERCENT: return BINOP_MOD;
        case TK_CARET:   return BINOP_POW;
        case TK_EQ:      return BINOP_EQ;
        case TK_NEQ:     return BINOP_NEQ;
        case TK_LT:      return BINOP_LT;
        case TK_LE:      return BINOP_LE;
        case TK_GT:      return BINOP_GT;
        case TK_GE:      return BINOP_GE;
        case TK_AND:     return BINOP_AND;
        case TK_OR:      return BINOP_OR;
        case TK_DOTDOT:  return BINOP_CONCAT;
        case TK_AMP:     return BINOP_BAND;
        case TK_PIPE:    return BINOP_BOR;
        case TK_TILDE:   return BINOP_BXOR;
        case TK_LSHIFT:  return BINOP_SHL;
        case TK_RSHIFT:  return BINOP_SHR;
        default:         return BINOP_ADD; // unreachable
    }
}

// Parse argument list: (a, b, c) - returns array of expressions
static SeaNode** parse_arg_list(SeaParser *p, int *argc) {
    SeaNode **args = NULL;
    int count = 0, cap = 0;

    if (!check(p, TK_RPAREN)) {
        do {
            SeaNode *arg = parse_expr(p);
            if (!arg || p->had_error) break;
            SEA_DYNARRAY_PUSH(args, count, cap, SeaNode*, arg);
        } while (match(p, TK_COMMA) && !p->had_error);
    }
    *argc = count;
    return args;
}

// Parse a primary expression (literal, ident, grouped, etc.)
static SeaNode* parse_primary(SeaParser *p) {
    SeaToken t = cur(p);
    int line = t.line, col = t.col;

    switch (t.type) {
        case TK_NIL: {
            advance(p);
            return node_new(NODE_NIL, line, col);
        }
        case TK_TRUE: {
            advance(p);
            SeaNode *n = node_new(NODE_BOOL, line, col);
            n->boolean = true;
            return n;
        }
        case TK_FALSE: {
            advance(p);
            SeaNode *n = node_new(NODE_BOOL, line, col);
            n->boolean = false;
            return n;
        }
        case TK_NUMBER: {
            advance(p);
            SeaNode *n = node_new(NODE_NUMBER, line, col);
            n->number = t.val.number;
            n->num_is_float = t.is_float;
            return n;
        }
        case TK_STRING: {
            advance(p);
            SeaNode *n = node_new(NODE_STRING, line, col);
            n->string.ptr = dup_str(t.val.string.ptr, t.val.string.len);
            n->string.len = t.val.string.len;
            // Free the lexer-allocated string
            sea_free((void*)t.val.string.ptr, t.val.string.len + 1);
            return n;
        }
        case TK_IDENT: {
            advance(p);
            SeaNode *n = node_new(NODE_IDENT, line, col);
            n->ident = dup_str(t.start, (size_t)t.len);
            return n;
        }
        case TK_DOTDOTDOT: {
            advance(p);
            return node_new(NODE_VARARG, line, col);
        }
        case TK_LPAREN: {
            advance(p);
            SeaNode *inner = parse_expr(p);
            expect(p, TK_RPAREN);
            if (inner) inner->paren = true;
            return inner;
        }
        case TK_FUNCTION: {
            advance(p);
            return parse_simple_expr(p);  // will handle function body
        }
        case TK_LBRACE: {
            // Table literal
            advance(p);
            SeaNode *n = node_new(NODE_TABLE, line, col);
            SeaNode **keys = NULL;
            SeaNode **vals = NULL;
            int count = 0, keys_cap = 0, vals_cap = 0;

            while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
                SeaNode *key = NULL;
                SeaNode *val = NULL;

                if (is_name_tok(cur(p)) && peek(p).type == TK_ASSIGN) {
                    // name = expr
                    SeaToken kt = cur(p);
                    advance(p); advance(p); // consume ident and =
                    key = node_new(NODE_STRING, kt.line, kt.col);
                    key->string.ptr = dup_str(kt.start, (size_t)kt.len);
                    key->string.len = (size_t)kt.len;
                    val = parse_expr(p);
                } else if (check(p, TK_LBRACKET)) {
                    // [expr] = expr
                    advance(p);
                    key = parse_expr(p);
                    expect(p, TK_RBRACKET);
                    expect(p, TK_ASSIGN);
                    val = parse_expr(p);
                } else {
                    // positional
                    val = parse_expr(p);
                }

                if (!val || p->had_error) break;
                /* Ensure both arrays have capacity for index `count` */
                if (count >= keys_cap) {
                    int old = keys_cap;
                    keys_cap = old < 8 ? 8 : old * 2;
                    keys = SEA_REALLOC_N(keys, SeaNode*, old, keys_cap);
                }
                if (count >= vals_cap) {
                    int old = vals_cap;
                    vals_cap = old < 8 ? 8 : old * 2;
                    vals = SEA_REALLOC_N(vals, SeaNode*, old, vals_cap);
                }
                keys[count] = key;
                vals[count] = val;
                count++;

                if (!match(p, TK_COMMA)) match(p, TK_SEMICOLON);
            }
            n->table.end_line = cur(p).line;
            expect(p, TK_RBRACE);

            n->table.keys = keys;
            n->table.vals = vals;
            n->table.count = count;
            return n;
        }
        case TK_ARROW_LEFT: {
            // Channel receive: <-expr
            advance(p);
            SeaNode *n = node_new(NODE_CHAN_RECV, line, col);
            n->unop.op = UNOP_CHAN_RECV;
            n->unop.expr = parse_expr(p);
            return n;
        }
        case TK_TEMPLATE: {
            advance(p);
            return parse_template(p, t);
        }
        case TK_LBRACKET: {
            advance(p);
            if (check(p, TK_ASSIGN)) {
                // [=] → map literal
                advance(p);
                expect(p, TK_RBRACKET);
                return node_new(NODE_MAP_LIT, line, col);
            }
            // [] or [e1, e2, ...] → array literal (a trailing comma is allowed)
            SeaNode *n = node_new(NODE_ARRAY_LIT, line, col);
            SeaNode **elems = NULL;
            int count = 0, cap = 0;
            while (!check(p, TK_RBRACKET) && !check(p, TK_EOF)) {
                SeaNode *e = parse_expr(p);
                if (!e || p->had_error) {
                    sea_node_free(e);
                    break;
                }
                if (count >= cap) {
                    int old = cap;
                    cap = old < 8 ? 8 : old * 2;
                    elems = SEA_REALLOC_N(elems, SeaNode*, old, cap);
                }
                elems[count++] = e;
                if (!match(p, TK_COMMA)) break;
            }
            if (elems && count < cap) elems = SEA_REALLOC_N(elems, SeaNode*, cap, count);
            if (elems && count == 0) { sea_free(elems, sizeof(SeaNode*) * (size_t)cap); elems = NULL; }
            n->array_lit.elems = elems;
            n->array_lit.count = count;
            n->array_lit.end_line = cur(p).line;
            expect(p, TK_RBRACKET);
            return n;
        }
        default:
            parser_error(p, "unexpected token '%s' in expression", sea_tk_name(t.type));
            return NULL;
    }
}

/* The end of a ${...} expression in a template body: s points after "${";
 * returns the matching '}' (skipping strings and nested templates), or NULL. */
static const char *template_expr_end(const char *s, const char *end) {
    int depth = 1;
    while (s < end) {
        char c = *s++;
        if (c == '{') depth++;
        else if (c == '}') { if (--depth == 0) return s - 1; }
        else if (c == '"' || c == '\'') {
            while (s < end && *s != c) { if (*s == '\\') s++; s++; }
            s++;
        } else if (c == '`') {
            while (s < end && *s != '`') {
                if (*s == '\\') { s += 2; continue; }
                if (*s == '$' && s + 1 < end && s[1] == '{') {
                    const char *e = template_expr_end(s + 2, end);
                    if (!e) return NULL;
                    s = e + 1;
                    continue;
                }
                s++;
            }
            s++;
        }
    }
    return NULL;
}

/* `text ${expr} text`: the texts are decoded like string escapes (plus \`
 * and \$); each expression is parsed by a parser of its own, given the
 * expression preceded by the newlines and spaces that put it at its place in
 * the file, so its positions are the real ones. */
static SeaNode* parse_template(SeaParser *p, SeaToken t) {
    SeaNode *n = node_new(NODE_TEMPLATE, t.line, t.col);
    const char *body = t.start + 1, *end = t.start + t.len - 1;
    int cap = 4, count = 0;
    char **texts = SEA_ALLOC_N(char*, cap + 1);
    size_t *lens = SEA_ALLOC_N(size_t, cap + 1);
    char **raws = SEA_ALLOC_N(char*, cap + 1);
    SeaNode **exprs = SEA_ALLOC_N(SeaNode*, cap);
    const char *seg = body, *s = body;
    int line = t.line, col = t.col + 1;     /* position of *s */
    bool ok = true;
    for (;;) {
        bool at_end = s >= end;
        bool at_expr = !at_end && s[0] == '$' && s + 1 < end && s[1] == '{';
        if (at_end || at_expr) {
            /* the text before */
            const char *err = NULL;
            char *dec = NULL;
            size_t dlen = 0;
            if (!sea_lex_unescape(seg, (size_t)(s - seg), true, &dec, &dlen, &err)) {
                parser_error_at(p, t.line, t.col, "%s", err);
                ok = false;
                break;
            }
            texts[count] = dec;
            lens[count] = dlen;
            raws[count] = dup_str(seg, (size_t)(s - seg));
            if (at_end) break;
            const char *e_start = s + 2;
            const char *e_end = template_expr_end(e_start, end);
            if (!e_end) {
                parser_error_at(p, line, col, "unterminated ${ in template string");
                sea_free(texts[count], lens[count] + 1);
                sea_free(raws[count], strlen(raws[count]) + 1);
                ok = false;
                break;
            }
            /* the expression, placed at its line and column */
            int eline = line, ecol = col + 2;
            size_t elen = (size_t)(e_end - e_start);
            size_t pad = (size_t)(eline - 1) + (size_t)(ecol - 1);
            char *src = (char*)sea_malloc(pad + elen + 1);
            memset(src, '\n', (size_t)(eline - 1));
            memset(src + eline - 1, ' ', (size_t)(ecol - 1));
            memcpy(src + pad, e_start, elen);
            src[pad + elen] = '\0';
            SeaParser sub;
            sea_parser_init(&sub, src);
            sub.func_depth = p->func_depth;
            sub.depth = p->depth;
            SeaNode *e = NULL;
            if (check(&sub, TK_EOF)) {
                parser_error_at(p, eline, ecol, "empty ${} in template string");
            } else {
                e = parse_expr(&sub);
                if (!sub.had_error && !check(&sub, TK_EOF))
                    parser_error(&sub, "unexpected '%s' in ${...}: one expression expected", sea_tk_name(cur(&sub).type));
                if (sub.had_error) {
                    parser_error_at(p, sub.err_line, sub.err_col, "%s", sub.err_msg ? sub.err_msg : "syntax error");
                    sea_node_free(e);
                    e = NULL;
                }
            }
            sea_parser_free(&sub);
            sea_free(src, pad + elen + 1);
            if (!e) {
                sea_free(texts[count], lens[count] + 1);
                sea_free(raws[count], strlen(raws[count]) + 1);
                ok = false;
                break;
            }
            if (count == cap) {
                int nc = cap * 2;
                texts = SEA_REALLOC_N(texts, char*, cap + 1, nc + 1);
                lens = SEA_REALLOC_N(lens, size_t, cap + 1, nc + 1);
                raws = SEA_REALLOC_N(raws, char*, cap + 1, nc + 1);
                exprs = SEA_REALLOC_N(exprs, SeaNode*, cap, nc);
                cap = nc;
            }
            exprs[count++] = e;
            /* move past the expression */
            for (const char *q = s; q <= e_end; q++) {
                if (*q == '\n') { line++; col = 1; } else col++;
            }
            s = seg = e_end + 1;
            continue;
        }
        if (*s == '\\' && s + 1 < end) { s++; col++; }
        if (*s == '\n') { line++; col = 1; } else col++;
        s++;
    }
    if (!ok) {
        for (int i = 0; i < count; i++) {
            sea_free(texts[i], lens[i] + 1);
            sea_free(raws[i], strlen(raws[i]) + 1);
        }
        for (int i = 0; i < count; i++) sea_node_free(exprs[i]);
        sea_free(texts, sizeof(char*) * (size_t)(cap + 1));
        sea_free(lens, sizeof(size_t) * (size_t)(cap + 1));
        sea_free(raws, sizeof(char*) * (size_t)(cap + 1));
        sea_free(exprs, sizeof(SeaNode*) * (size_t)cap);
        n->type = NODE_NIL;
        return n;
    }
    /* exact sizes (sea_node_free frees count + 1 texts) */
    texts = SEA_REALLOC_N(texts, char*, cap + 1, count + 1);
    lens = SEA_REALLOC_N(lens, size_t, cap + 1, count + 1);
    raws = SEA_REALLOC_N(raws, char*, cap + 1, count + 1);
    if (count) exprs = SEA_REALLOC_N(exprs, SeaNode*, cap, count);
    else { sea_free(exprs, sizeof(SeaNode*) * (size_t)cap); exprs = NULL; }
    n->tmpl.texts = texts;
    n->tmpl.lens = lens;
    n->tmpl.raws = raws;
    n->tmpl.exprs = exprs;
    n->tmpl.count = count;
    return n;
}

// Parse a function body (after 'function')
// Called either from function statement or function expression
static SeaNode* parse_func_body(SeaParser *p, int line, int col, const char *name) {
    SeaNode *n = node_new(NODE_FUNC, line, col);
    n->func.name = name ? dup_cstr(name) : NULL;

    expect(p, TK_LPAREN);

    SeaParam *params = NULL;
    int paramc = 0, paramc_cap = 0;

    if (!check(p, TK_RPAREN)) {
        do {
            SeaParam param;
            if (check(p, TK_DOTDOTDOT)) {
                advance(p);
                param.name = dup_cstr("...");
                param.is_vararg = true;
                SEA_DYNARRAY_PUSH(params, paramc, paramc_cap, SeaParam, param);
                break;
            }
            SeaToken pt = cur(p);
            if (pt.type != TK_IDENT) {
                parser_error(p, "expected parameter name");
                break;
            }
            advance(p);
            param.name = dup_str(pt.start, (size_t)pt.len);
            param.is_vararg = false;
            SEA_DYNARRAY_PUSH(params, paramc, paramc_cap, SeaParam, param);
        } while (match(p, TK_COMMA) && !p->had_error);
    }
    expect(p, TK_RPAREN);

    n->func.params = params;
    n->func.paramc = paramc;

    if (p->func_depth >= SEA_PARSER_MAX_FUNCS)
        parser_error(p, "functions nested too deeply (limit %d)", SEA_PARSER_MAX_FUNCS);
    p->func_depth++;
    n->func.body = parse_block(p);
    p->func_depth--;

    expect(p, TK_END);
    return n;
}

// This handles the continuation of "function" keyword parsing
static SeaNode* parse_simple_expr(SeaParser *p) {
    // Called after consuming TK_FUNCTION
    // Just parse the function body
    int line = cur(p).line, col = cur(p).col;
    SeaNode *n = parse_func_body(p, line, col, NULL);
    return n;
}

// Parse postfix operations (calls, indexing, field access)
static SeaNode* parse_postfix(SeaParser *p, SeaNode *left) {
    for (;;) {
        SeaToken t = cur(p);
        int line = t.line, col = t.col;

        if (t.type == TK_DOT) {
            advance(p);
            SeaToken field_tok = cur(p);
            if (!is_name_tok(field_tok)) {
                parser_error(p, "expected field name after '.'");
                return left;
            }
            advance(p);
            SeaNode *n = node_new(NODE_FIELD, line, col);
            n->field.obj = left;
            n->field.field = dup_str(field_tok.start, (size_t)field_tok.len);
            left = n;
        } else if (t.type == TK_LBRACKET) {
            advance(p);
            SeaNode *key = NULL;
            if (!check(p, TK_RBRACKET)) {
                key = parse_expr(p);  // expr[expr] — normal indexing
            }
            // key=NULL means arr[] — append lvalue (only valid on assignment LHS)
            expect(p, TK_RBRACKET);
            SeaNode *n = node_new(NODE_INDEX, line, col);
            n->index.obj = left;
            n->index.key = key;
            left = n;
        } else if (t.type == TK_LPAREN) {
            advance(p);
            int argc = 0;
            SeaNode **args = parse_arg_list(p, &argc);
            int end_line = cur(p).line;
            expect(p, TK_RPAREN);
            SeaNode *n = node_new(NODE_CALL, line, col);
            n->call.end_line = end_line;
            n->call.fn = left;
            n->call.args = args;
            n->call.argc = argc;
            left = n;
        } else if (t.type == TK_COLON) {
            // Method call: expr:name(args)
            advance(p);
            SeaToken method_tok = cur(p);
            if (!is_name_tok(method_tok)) {
                parser_error(p, "expected method name after ':'");
                return left;
            }
            advance(p);
            expect(p, TK_LPAREN);
            int argc = 0;
            SeaNode **args = parse_arg_list(p, &argc);
            int end_line = cur(p).line;
            expect(p, TK_RPAREN);
            SeaNode *n = node_new(NODE_METHOD_CALL, line, col);
            n->method.end_line = end_line;
            n->method.obj = left;
            n->method.method = dup_str(method_tok.start, (size_t)method_tok.len);
            n->method.args = args;
            n->method.argc = argc;
            left = n;
        } else if (t.type == TK_ARROW_LEFT) {
            // Chan send expression: left <- right
            advance(p);
            SeaNode *right = parse_expr(p);
            SeaNode *n = node_new(NODE_CHAN_SEND, line, col);
            n->binop.op = BINOP_CHAN_SEND;
            n->binop.left = left;
            n->binop.right = right;
            left = n;
            break; // send is not further chained
        } else if (t.type == TK_QUESTION) {
            // Error propagation: left?
            advance(p);
            SeaNode *n = node_new(NODE_PROPAGATE, line, col);
            n->propagate = left;
            left = n;
        } else if (t.type == TK_STRING) {
            // Implicit call with string arg: f"string"
            advance(p);
            SeaNode *arg = node_new(NODE_STRING, line, col);
            arg->string.ptr = dup_str(t.val.string.ptr, t.val.string.len);
            arg->string.len = t.val.string.len;
            sea_free((void*)t.val.string.ptr, t.val.string.len + 1);

            SeaNode **args = SEA_ALLOC_N(SeaNode*, 1);
            args[0] = arg;
            SeaNode *n = node_new(NODE_CALL, line, col);
            n->call.end_line = line;
            n->call.fn = left;
            n->call.args = args;
            n->call.argc = 1;
            left = n;
        } else {
            break;
        }
    }
    return left;
}

static bool enter_level(SeaParser *p) {
    if (++p->depth > SEA_PARSER_MAX_DEPTH) {
        parser_error(p, "code nested too deeply (limit %d)", SEA_PARSER_MAX_DEPTH);
        return false;
    }
    return true;
}

static SeaNode* parse_expr_prec_at(SeaParser *p, Prec min_prec);

static SeaNode* parse_expr_prec(SeaParser *p, Prec min_prec) {
    SeaNode *n = enter_level(p) ? parse_expr_prec_at(p, min_prec) : NULL;
    p->depth--;
    return n;
}

// Pratt expression parser
static SeaNode* parse_expr_prec_at(SeaParser *p, Prec min_prec) {
    SeaToken t = cur(p);
    int line = t.line, col = t.col;
    SeaNode *left = NULL;

    // Prefix / unary
    switch (t.type) {
        case TK_MINUS: {
            advance(p);
            SeaNode *operand = parse_expr_prec(p, PREC_UNARY);
            SeaNode *n = node_new(NODE_UNOP, line, col);
            n->unop.op = UNOP_NEG;
            n->unop.expr = operand;
            left = n;
            break;
        }
        case TK_NOT: {
            advance(p);
            SeaNode *operand = parse_expr_prec(p, PREC_UNARY);
            SeaNode *n = node_new(NODE_UNOP, line, col);
            n->unop.op = UNOP_NOT;
            n->unop.expr = operand;
            left = n;
            break;
        }
        case TK_TILDE: {
            advance(p);
            SeaNode *operand = parse_expr_prec(p, PREC_UNARY);
            SeaNode *n = node_new(NODE_UNOP, line, col);
            n->unop.op = UNOP_BNOT;
            n->unop.expr = operand;
            left = n;
            break;
        }
        case TK_ARROW_LEFT: {
            // Channel receive as expression: <-ch
            advance(p);
            SeaNode *operand = parse_expr_prec(p, PREC_UNARY);
            SeaNode *n = node_new(NODE_CHAN_RECV, line, col);
            n->unop.op = UNOP_CHAN_RECV;
            n->unop.expr = operand;
            left = n;
            break;
        }
        case TK_FUNCTION: {
            advance(p);
            left = parse_func_body(p, line, col, NULL);
            break;
        }
        default:
            left = parse_primary(p);
            if (!left) return NULL;
            break;
    }

    if (!left || p->had_error) return left;
    left = parse_postfix(p, left);

    // Infix / binary
    while (!p->had_error) {
        SeaTK op_tk = cur(p).type;
        Prec prec = token_infix_prec(op_tk);
        if (prec <= min_prec) break;

        // Skip postfix ops (handled above)
        if (op_tk == TK_DOT || op_tk == TK_LBRACKET ||
            op_tk == TK_LPAREN || op_tk == TK_COLON ||
            op_tk == TK_QUESTION || op_tk == TK_ARROW_LEFT) break;

        int op_line = cur(p).line, op_col = cur(p).col;
        advance(p);

        SeaBinop binop = tk_to_binop(op_tk);

        // Right associative: ^ and ..
        Prec next_prec = (op_tk == TK_CARET || op_tk == TK_DOTDOT)
                         ? prec - 1 : prec;

        SeaNode *right = parse_expr_prec(p, next_prec);
        if (!right || p->had_error) return left;

        right = parse_postfix(p, right);

        SeaNode *n = node_new(NODE_BINOP, op_line, op_col);
        n->binop.op = binop;
        n->binop.left = left;
        n->binop.right = right;
        left = n;
    }

    return left;
}

static SeaNode* parse_expr(SeaParser *p) {
    return parse_expr_prec(p, PREC_NONE);
}

// ---- Statement parsing ----

static SeaNode* parse_local(SeaParser *p) {
    int line = cur(p).line, col = cur(p).col;
    // 'local' already consumed

    // local function name ... end
    if (check(p, TK_FUNCTION)) {
        advance(p);
        SeaToken name_tok = cur(p);
        if (name_tok.type != TK_IDENT) {
            parser_error(p, "expected function name");
            return NULL;
        }
        advance(p);
        char *fname = dup_str(name_tok.start, (size_t)name_tok.len);
        SeaNode *fn = parse_func_body(p, line, col, fname);
        sea_free(fname, strlen(fname)+1);

        // Wrap as: local name = function...
        SeaNode *n = node_new(NODE_LOCAL, line, col);
        n->local.names = SEA_ALLOC_N(char*, 1);
        n->local.names[0] = dup_str(name_tok.start, (size_t)name_tok.len);
        n->local.count = 1;
        n->local.exprs = SEA_ALLOC_N(SeaNode*, 1);
        n->local.exprs[0] = fn;
        n->local.expr_count = 1;
        return n;
    }

    // local (
    //     var1 = expr
    //     var2 = expr
    // )
    if (match(p, TK_LPAREN)) {
        SeaNode *n = node_new(NODE_LOCAL, line, col);
        char **names = NULL;
        int name_count = 0, name_cap = 0;
        SeaNode **exprs = NULL;
        int expr_count = 0, expr_cap = 0;

        while (!check(p, TK_RPAREN) && !check(p, TK_EOF) && !p->had_error) {
            SeaToken nt = cur(p);
            if (nt.type != TK_IDENT) {
                parser_name_error(p, "expected variable name");
                break;
            }
            advance(p);
            char *nm = dup_str(nt.start, (size_t)nt.len);
            SEA_DYNARRAY_PUSH(names, name_count, name_cap, char*, nm);

            if (!expect(p, TK_ASSIGN)) break;

            SeaNode *e = parse_expr(p);
            if (!e || p->had_error) break;
            SEA_DYNARRAY_PUSH(exprs, expr_count, expr_cap, SeaNode*, e);
        }
        expect(p, TK_RPAREN);

        n->local.names = names;
        n->local.count = name_count;
        n->local.exprs = exprs;
        n->local.expr_count = expr_count;
        return n;
    }

    // local a, b, c = e1, e2, e3
    SeaNode *n = node_new(NODE_LOCAL, line, col);
    char **names = NULL;
    int name_count = 0, name_cap = 0;

    do {
        SeaToken nt = cur(p);
        if (nt.type != TK_IDENT) {
            parser_name_error(p, "expected variable name");
            break;
        }
        advance(p);
        char *nm = dup_str(nt.start, (size_t)nt.len);
        SEA_DYNARRAY_PUSH(names, name_count, name_cap, char*, nm);
    } while (match(p, TK_COMMA) && !p->had_error);

    n->local.names = names;
    n->local.count = name_count;

    SeaNode **exprs = NULL;
    int expr_count = 0, expr_cap = 0;

    if (match(p, TK_ASSIGN)) {
        do {
            SeaNode *e = parse_expr(p);
            if (!e || p->had_error) break;
            SEA_DYNARRAY_PUSH(exprs, expr_count, expr_cap, SeaNode*, e);
        } while (match(p, TK_COMMA) && !p->had_error);
    }

    n->local.exprs = exprs;
    n->local.expr_count = expr_count;
    return n;
}

// Check if a node is a valid assignment target
static bool is_lvalue(SeaNode *n) {
    return n && (n->type == NODE_IDENT || n->type == NODE_INDEX || n->type == NODE_FIELD);
}

/* A target that can be evaluated twice with no effect: a name, or fields and
 * indexes of one with literal or name keys (a.b[i].c). */
static bool simple_target(SeaNode *n) {
    if (!n) return false;
    switch (n->type) {
    case NODE_IDENT: return true;
    case NODE_FIELD: return simple_target(n->field.obj);
    case NODE_INDEX: {
        SeaNode *k = n->index.key;
        if (!k) return false;
        bool simple_key = k->type == NODE_IDENT || k->type == NODE_NUMBER || k->type == NODE_STRING ||
                          k->type == NODE_BOOL;
        return simple_key && simple_target(n->index.obj);
    }
    default: return false;
    }
}

static SeaNode *clone_simple(SeaNode *n) {
    SeaNode *c = node_new(n->type, n->line, n->col);
    switch (n->type) {
    case NODE_IDENT: c->ident = dup_cstr(n->ident); break;
    case NODE_FIELD:
        c->field.obj = clone_simple(n->field.obj);
        c->field.field = dup_cstr(n->field.field);
        break;
    case NODE_INDEX:
        c->index.obj = clone_simple(n->index.obj);
        c->index.key = clone_simple(n->index.key);
        break;
    case NODE_NUMBER:
        c->number = n->number;
        c->num_is_float = n->num_is_float;
        break;
    case NODE_STRING:
        c->string.ptr = dup_str(n->string.ptr, n->string.len);
        c->string.len = n->string.len;
        break;
    case NODE_BOOL: c->boolean = n->boolean; break;
    default: break;
    }
    return c;
}

static SeaNode* parse_assign_or_call(SeaParser *p) {
    int line = cur(p).line, col = cur(p).col;
    SeaNode *first = parse_expr(p);
    if (!first || p->had_error) return first;

    // Compound assignment: a += b (one target, one value)
    if (check(p, TK_OP_ASSIGN)) {
        if (!is_lvalue(first)) {
            parser_error_at(p, line, col, "invalid assignment target");
            sea_node_free(first);
            return NULL;
        }
        int op = (int)cur(p).val.number;
        advance(p);
        SeaNode *e = parse_expr(p);
        if (!e || p->had_error) {
            sea_node_free(first);
            sea_node_free(e);
            return NULL;
        }
        if (check(p, TK_COMMA)) {
            parser_error(p, "a compound assignment (+=, ..=, ...) takes one target and one value");
            sea_node_free(first);
            sea_node_free(e);
            return NULL;
        }
        SeaNode *n = node_new(NODE_ASSIGN, line, col);
        n->assign.targets = SEA_ALLOC_N(SeaNode*, 1);
        n->assign.targets[0] = first;
        n->assign.count = 1;
        n->assign.exprs = SEA_ALLOC_N(SeaNode*, 1);
        n->assign.expr_count = 1;
        n->assign.op = op + 1;
        if (simple_target(first)) {
            /* a op= b is a = a op b: every analysis of the compiler then sees
             * the real value (x /= 2 is a float), and s ..= x appends in place */
            SeaNode *b = node_new(NODE_BINOP, e->line, e->col);
            b->binop.op = (SeaBinop)op;
            b->binop.left = clone_simple(first);
            b->binop.right = e;
            n->assign.exprs[0] = b;
            n->assign.op_wrapped = true;
        } else {
            n->assign.exprs[0] = e;   /* evaluated once by the compiler */
        }
        return n;
    }

    // Check for assignment: a, b = ...
    if (check(p, TK_COMMA) || check(p, TK_ASSIGN)) {
        // It's an assignment
        SeaNode **targets = NULL;
        int tcount = 0, tcap = 0;

        if (!is_lvalue(first)) {
            parser_error_at(p, line, col, "invalid assignment target");
            sea_node_free(first);
            return NULL;
        }
        SEA_DYNARRAY_PUSH(targets, tcount, tcap, SeaNode*, first);

        while (match(p, TK_COMMA)) {
            SeaNode *t2 = parse_expr(p);
            if (!t2 || !is_lvalue(t2)) {
                parser_error_at(p, line, col, "invalid assignment target");
                break;
            }
            SEA_DYNARRAY_PUSH(targets, tcount, tcap, SeaNode*, t2);
        }

        if (check(p, TK_OP_ASSIGN))
            parser_error(p, "a compound assignment (+=, ..=, ...) takes one target and one value");
        expect(p, TK_ASSIGN);

        SeaNode **exprs = NULL;
        int ecount = 0, ecap = 0;
        do {
            SeaNode *e = parse_expr(p);
            if (!e || p->had_error) break;
            SEA_DYNARRAY_PUSH(exprs, ecount, ecap, SeaNode*, e);
        } while (match(p, TK_COMMA) && !p->had_error);

        SeaNode *n = node_new(NODE_ASSIGN, line, col);
        n->assign.targets = targets;
        n->assign.count = tcount;
        n->assign.exprs = exprs;
        n->assign.expr_count = ecount;
        return n;
    }

    // It's a call expression used as statement (also `f()?`: an error
    // returns from the function, a value is discarded)
    bool propagated_call = first->type == NODE_PROPAGATE && first->propagate &&
                           (first->propagate->type == NODE_CALL || first->propagate->type == NODE_METHOD_CALL);
    if (first->type == NODE_CALL || first->type == NODE_METHOD_CALL ||
        first->type == NODE_CHAN_SEND || propagated_call) {
        SeaNode *n = node_new(NODE_EXPR_STMT, line, col);
        n->expr_stmt = first;
        return n;
    }

    // Single expression - might be an assignment with <- send
    if (first->type == NODE_BINOP && first->binop.op == BINOP_CHAN_SEND) {
        SeaNode *n = node_new(NODE_EXPR_STMT, line, col);
        n->expr_stmt = first;
        return n;
    }

    parser_error_at(p, line, col, "expression is not a statement");
    sea_node_free(first);
    return NULL;
}

static SeaNode* parse_if(SeaParser *p) {
    int line = cur(p).line, col = cur(p).col;
    // 'if' already consumed

    SeaNode *cond = parse_expr(p);
    expect(p, TK_THEN);
    SeaNode *then_body = parse_block(p);
    SeaNode *else_body = NULL;

    if (check(p, TK_ELSEIF)) {
        advance(p);
        else_body = parse_if(p);  // recursive for elseif chains
        // The "end" will be consumed by the inner if
    } else if (match(p, TK_ELSE)) {
        else_body = parse_block(p);
        expect(p, TK_END);
    } else {
        expect(p, TK_END);
    }

    SeaNode *n = node_new(NODE_IF, line, col);
    n->if_stmt.cond = cond;
    n->if_stmt.then_body = then_body;
    n->if_stmt.else_body = else_body;
    return n;
}

static SeaNode* parse_while(SeaParser *p) {
    int line = cur(p).line, col = cur(p).col;
    SeaNode *cond = parse_expr(p);
    expect(p, TK_DO);
    p->loop_depth++;
    SeaNode *body = parse_block(p);
    p->loop_depth--;
    expect(p, TK_END);

    SeaNode *n = node_new(NODE_WHILE, line, col);
    n->while_loop.cond = cond;
    n->while_loop.body = body;
    return n;
}

static SeaNode* parse_for(SeaParser *p) {
    int line = cur(p).line, col = cur(p).col;
    // 'for' already consumed

    SeaToken var_tok = cur(p);
    if (var_tok.type != TK_IDENT) {
        parser_name_error(p, "expected variable name in for");
        return NULL;
    }
    advance(p);

    if (check(p, TK_ASSIGN)) {
        // Numeric for: for i = start, limit[, step] do
        advance(p);
        SeaNode *start = parse_expr(p);
        expect(p, TK_COMMA);
        SeaNode *limit = parse_expr(p);
        SeaNode *step = NULL;
        if (match(p, TK_COMMA)) {
            step = parse_expr(p);
        }
        expect(p, TK_DO);
        p->loop_depth++;
        SeaNode *body = parse_block(p);
        p->loop_depth--;
        expect(p, TK_END);

        SeaNode *n = node_new(NODE_FOR_NUM, line, col);
        n->for_num.var = dup_str(var_tok.start, (size_t)var_tok.len);
        n->for_num.start = start;
        n->for_num.limit = limit;
        n->for_num.step = step;
        n->for_num.body = body;
        return n;
    }

    // For-in: for k[, v, ...] in expr do
    char **vars = NULL;
    int var_count = 0, var_cap = 0;
    char *first_var = dup_str(var_tok.start, (size_t)var_tok.len);
    SEA_DYNARRAY_PUSH(vars, var_count, var_cap, char*, first_var);

    while (match(p, TK_COMMA)) {
        SeaToken vt = cur(p);
        if (vt.type != TK_IDENT) {
            parser_name_error(p, "expected variable name in for-in");
            break;
        }
        advance(p);
        char *vn = dup_str(vt.start, (size_t)vt.len);
        SEA_DYNARRAY_PUSH(vars, var_count, var_cap, char*, vn);
    }

    expect(p, TK_IN);
    SeaNode *iter = parse_expr(p);
    expect(p, TK_DO);
    p->loop_depth++;
    SeaNode *body = parse_block(p);
    p->loop_depth--;
    expect(p, TK_END);

    SeaNode *n = node_new(NODE_FOR_IN, line, col);
    n->for_in.vars = vars;
    n->for_in.var_count = var_count;
    n->for_in.iter_expr = iter;
    n->for_in.body = body;
    return n;
}

static SeaNode* parse_return(SeaParser *p) {
    int line = cur(p).line, col = cur(p).col;

    SeaNode **exprs = NULL;
    int count = 0, cap = 0;

    // Return values: everything until end/else/elseif/EOF/;
    if (!check(p, TK_END) && !check(p, TK_ELSE) && !check(p, TK_ELSEIF) &&
        !check(p, TK_CASE) && !check(p, TK_EOF) && !check(p, TK_SEMICOLON)) {
        do {
            SeaNode *e = parse_expr(p);
            if (!e || p->had_error) break;
            SEA_DYNARRAY_PUSH(exprs, count, cap, SeaNode*, e);
        } while (match(p, TK_COMMA) && !p->had_error);
    }
    match(p, TK_SEMICOLON);

    SeaNode *n = node_new(NODE_RETURN, line, col);
    n->ret.exprs = exprs;
    n->ret.count = count;
    return n;
}

static SeaNode* parse_select(SeaParser *p) {
    int line = cur(p).line, col = cur(p).col;
    // 'select' already consumed

    SeaSelectCase *cases = NULL;
    int case_count = 0, case_cap = 0;

    while (!check(p, TK_END) && !check(p, TK_EOF)) {
        if (!expect(p, TK_CASE)) break;

        SeaSelectCase sc;
        sc.is_default = false;
        sc.stmt = NULL;
        sc.body = NULL;

        if (check(p, TK_DEFAULT)) {
            advance(p);
            sc.is_default = true;
        } else {
            // Case stmt can be:
            // - receive assignment: x = <-ch  or  x, ok = <-ch
            // - send: ch <- val
            sc.stmt = parse_assign_or_call(p);
        }

        expect(p, TK_THEN);
        sc.body = parse_block(p);

        SEA_DYNARRAY_PUSH(cases, case_count, case_cap, SeaSelectCase, sc);
    }

    expect(p, TK_END);

    SeaNode *n = node_new(NODE_SELECT, line, col);
    n->select.cases = cases;
    n->select.count = case_count;
    return n;
}

static SeaNode* parse_function_stmt(SeaParser *p) {
    int line = cur(p).line, col = cur(p).col;
    // 'function' already consumed

    // function name[.name...][method] (params) body end
    SeaToken name_tok = cur(p);
    if (name_tok.type != TK_IDENT) {
        parser_error(p, "expected function name");
        return NULL;
    }
    advance(p);

    char *base_name = dup_str(name_tok.start, (size_t)name_tok.len);
    SeaNode *target = node_new(NODE_IDENT, line, col);
    target->ident = dup_cstr(base_name);

    bool is_method = false;
    char *method_name = NULL;

    while (check(p, TK_DOT)) {
        advance(p);
        SeaToken ft = cur(p);
        if (!is_name_tok(ft)) {
            parser_error(p, "expected field name");
            break;
        }
        advance(p);
        SeaNode *field = node_new(NODE_FIELD, ft.line, ft.col);
        field->field.obj = target;
        field->field.field = dup_str(ft.start, (size_t)ft.len);
        target = field;
    }

    // Method definition: function obj:method(...)
    if (check(p, TK_COLON)) {
        advance(p);
        SeaToken mt = cur(p);
        if (!is_name_tok(mt)) {
            parser_error(p, "expected method name");
        } else {
            advance(p);
            method_name = dup_str(mt.start, (size_t)mt.len);
            is_method = true;
        }
    }

    SeaNode *fn = parse_func_body(p, line, col, base_name);

    // For method syntax, add 'self' as first parameter
    if (is_method && fn && fn->type == NODE_FUNC) {
        int old_count = fn->func.paramc;
        SeaParam *new_params = SEA_ALLOC_N(SeaParam, old_count + 1);
        new_params[0].name = dup_cstr("self");
        new_params[0].is_vararg = false;
        if (old_count > 0) {
            memcpy(new_params + 1, fn->func.params, sizeof(SeaParam) * (size_t)old_count);
            sea_free(fn->func.params, sizeof(SeaParam) * (size_t)old_count);
        }
        fn->func.params = new_params;
        fn->func.paramc = old_count + 1;
        fn->func.is_method = true;
    }

    sea_free(base_name, strlen(base_name)+1);

    // Build assignment: target = fn  (or target.method = fn)
    if (is_method) {
        SeaNode *field = node_new(NODE_FIELD, line, col);
        field->field.obj = target;
        field->field.field = method_name;
        target = field;
    }

    SeaNode **targets = SEA_ALLOC_N(SeaNode*, 1);
    targets[0] = target;
    SeaNode **exprs = SEA_ALLOC_N(SeaNode*, 1);
    exprs[0] = fn;

    SeaNode *assign = node_new(NODE_ASSIGN, line, col);
    assign->assign.targets = targets;
    assign->assign.count = 1;
    assign->assign.exprs = exprs;
    assign->assign.expr_count = 1;
    return assign;
}

static SeaNode* parse_stmt_at(SeaParser *p);

/* A statement's position is that of its first token (the keyword). */
static SeaNode* parse_stmt(SeaParser *p) {
    while (check(p, TK_SEMICOLON)) advance(p);
    SeaToken t = cur(p);
    SeaNode *n = parse_stmt_at(p);
    if (n) {
        n->line = t.line;
        n->col = t.col;
        /* function name() ... end: the function node too */
        if (t.type == TK_FUNCTION && n->type == NODE_ASSIGN &&
            n->assign.expr_count == 1 && n->assign.exprs[0]) {
            n->assign.exprs[0]->line = t.line;
            n->assign.exprs[0]->col = t.col;
        }
    }
    return n;
}

static SeaNode* parse_stmt_at(SeaParser *p) {
    SeaToken t = cur(p);
    int line = t.line, col = t.col;

    // Skip semicolons
    while (check(p, TK_SEMICOLON)) advance(p);

    switch (t.type) {
        case TK_LOCAL:
            advance(p);
            return parse_local(p);

        case TK_CONST: {
            advance(p);
            if (check(p, TK_FUNCTION)) {
                parser_error(p, "'const function' is not supported; use 'local function'");
                return NULL;
            }
            SeaNode *n = parse_local(p);
            if (n) n->local.is_const = true;
            return n;
        }

        case TK_IF:
            advance(p);
            return parse_if(p);

        case TK_WHILE:
            advance(p);
            return parse_while(p);

        case TK_FOR:
            advance(p);
            return parse_for(p);

        case TK_RETURN:
            advance(p);
            return parse_return(p);

        case TK_BREAK:
            advance(p);
            if (p->loop_depth == 0) {
                parser_error_at(p, line, col, "'break' outside of loop");
            }
            return node_new(NODE_BREAK, line, col);

        case TK_CONTINUE:
            advance(p);
            if (p->loop_depth == 0) {
                parser_error_at(p, line, col, "'continue' outside of loop");
            }
            return node_new(NODE_CONTINUE, line, col);

        case TK_DO: {
            advance(p);
            SeaNode *body = parse_block(p);
            expect(p, TK_END);
            SeaNode *n = node_new(NODE_DO, line, col);
            n->block = body->block;
            // Steal the block contents
            body->type = NODE_NIL; // prevent double free
            sea_node_free(body);
            return n;
        }

        case TK_FUNCTION:
            advance(p);
            return parse_function_stmt(p);

        case TK_GO: {
            advance(p);
            SeaNode *call = parse_expr(p);
            if (!call) return NULL;
            SeaNode *n = node_new(NODE_GO, line, col);
            n->go_call = call;
            return n;
        }

        case TK_SELECT:
            advance(p);
            return parse_select(p);

        case TK_DEFER: {
            advance(p);
            SeaNode *stmt = parse_expr(p);
            SeaNode *n = node_new(NODE_DEFER, line, col);
            n->defer_stmt = stmt;
            return n;
        }

        case TK_PANIC: {
            advance(p);
            expect(p, TK_LPAREN);
            SeaNode *val = parse_expr(p);
            expect(p, TK_RPAREN);
            SeaNode *n = node_new(NODE_PANIC, line, col);
            n->panic_expr = val;
            return n;
        }

        case TK_DEL: {
            advance(p);
            SeaNode *target = parse_expr(p);
            if (!target || target->type != NODE_INDEX || target->index.key == NULL) {
                parser_error_at(p, line, col, "'del' expects indexed expression (arr[i] or map[k])");
                return NULL;
            }
            SeaNode *n = node_new(NODE_DEL, line, col);
            n->del_stmt = target;
            return n;
        }

        default:
            return parse_assign_or_call(p);
    }
}

static bool is_block_end(SeaParser *p) {
    SeaTK t = cur(p).type;
    return t == TK_END || t == TK_ELSE || t == TK_ELSEIF || t == TK_CASE || t == TK_EOF;
}

static SeaNode* parse_block_at(SeaParser *p);

static SeaNode* parse_block(SeaParser *p) {
    if (!enter_level(p)) {
        p->depth--;
        return node_new(NODE_BLOCK, cur(p).line, cur(p).col);
    }
    SeaNode *n = parse_block_at(p);
    p->depth--;
    return n;
}

static SeaNode* parse_block_at(SeaParser *p) {
    int line = cur(p).line, col = cur(p).col;
    SeaNode **stmts = NULL;
    int count = 0, cap = 0;

    while (!is_block_end(p) && !p->had_error) {
        while (check(p, TK_SEMICOLON)) advance(p);
        if (is_block_end(p)) break;

        SeaNode *stmt = parse_stmt(p);
        if (p->had_error) {
            sea_node_free(stmt);
            break;
        }
        if (stmt) {
            SEA_DYNARRAY_PUSH(stmts, count, cap, SeaNode*, stmt);
        }

        // Return is always the last statement in a block
        if (stmt && stmt->type == NODE_RETURN) break;

        // Enforce one statement per line: the next token must either be on a
        // new line, be a semicolon (explicit separator), or end the block.
        if (!is_block_end(p) && !check(p, TK_SEMICOLON) && !p->had_error) {
            if (cur(p).line == p->last_line) {
                if (p->last_tk == TK_QUESTION)
                    parser_error(p, "multiple statements on the same line (`?` propagates an error; "
                                 "Sea has no `c ? a : b`: use if/else, or `c and a or b`)");
                else
                    parser_error(p, "multiple statements on the same line; "
                                 "use a newline (or `;`) to separate statements");
            }
        }
    }

    SeaNode *block = node_new(NODE_BLOCK, line, col);
    block->block.stmts = stmts;
    block->block.count = count;
    block->block.end_line = cur(p).line;
    return block;
}

// ---- Public API ----

void sea_parser_init(SeaParser *p, const char *source) {
    sea_lexer_init(&p->lexer, source);
    p->err_msg = NULL;
    p->err_line = 0;
    p->err_col = 0;
    p->had_error = false;
    p->loop_depth = 0;
    p->func_depth = 0;
    p->depth      = 0;
    p->last_line  = 0;
}

void sea_parser_free(SeaParser *p) {
    sea_lexer_free(&p->lexer);
    if (p->err_msg) {
        sea_free(p->err_msg, strlen(p->err_msg)+1);
        p->err_msg = NULL;
    }
}

SeaNode* sea_parser_parse(SeaParser *p) {
    SeaNode *prog = parse_block(p);
    if (p->had_error) {
        sea_node_free(prog);
        return NULL;
    }
    if (!check(p, TK_EOF)) {
        parser_error(p, "unexpected token '%s'", sea_tk_name(cur(p).type));
        sea_node_free(prog);
        return NULL;
    }
    return prog;
}

void sea_parser_error_string(const SeaParser *p, const char *name,
                             char *buf, size_t size) {
    snprintf(buf, size, "%s:%d:%d: %s", name ? name : "<input>",
             p->err_line, p->err_col, p->err_msg ? p->err_msg : "syntax error");
}
