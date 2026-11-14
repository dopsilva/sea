/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_lexer.h"
#include "sea_memory.h"
#include "sea_utf8.h"
#include "sea_ast.h"   /* SeaBinop, for compound assignment tokens */
#include <ctype.h>
#include <errno.h>

// ---- Keyword table ----

typedef struct { const char *word; SeaTK tk; } Keyword;

static const Keyword keywords[] = {
    {"and",      TK_AND},
    {"break",    TK_BREAK},
    {"const",    TK_CONST},
    {"continue", TK_CONTINUE},
    {"default",  TK_DEFAULT},
    {"del",      TK_DEL},
    {"defer",    TK_DEFER},
    {"do",       TK_DO},
    {"else",     TK_ELSE},
    {"elseif",   TK_ELSEIF},
    {"end",      TK_END},
    {"false",    TK_FALSE},
    {"for",      TK_FOR},
    {"function", TK_FUNCTION},
    {"go",       TK_GO},
    {"if",       TK_IF},
    {"in",       TK_IN},
    {"local",    TK_LOCAL},
    {"nil",      TK_NIL},
    {"not",      TK_NOT},
    {"or",       TK_OR},
    {"panic",    TK_PANIC},
    {"return",   TK_RETURN},
    {"select",   TK_SELECT},
    {"case",     TK_CASE},
    {"then",     TK_THEN},
    {"true",     TK_TRUE},
    {"while",    TK_WHILE},
    {NULL, TK_EOF},
};

// ---- Helpers ----

static char peek_char(SeaLexer *l) {
    return *l->cur;
}

static char peek_next(SeaLexer *l) {
    if (*l->cur == '\0') return '\0';
    return l->cur[1];
}

static char advance_char(SeaLexer *l) {
    char c = *l->cur++;
    if (c == '\n') { l->line++; l->line_start = l->cur; }
    return c;
}

static bool match_char(SeaLexer *l, char expected) {
    if (*l->cur != expected) return false;
    l->cur++;
    return true;
}

/* At p: "[[", "[=[", "[==[", ... — the level (number of '='), or -1. */
static int long_bracket_level(const char *p) {
    if (*p != '[') return -1;
    int level = 0;
    p++;
    while (*p == '=') { level++; p++; }
    return *p == '[' ? level : -1;
}

/* Skip a long comment whose "--" is at l->cur: up to the "]" "="*level "]"
 * that closes it, counting lines. False if the input ends first (l->cur is
 * then at the end). */
static bool skip_long_comment(SeaLexer *l, int level) {
    l->cur += 2 + 2 + level;                       /* "--[" "="... "[" */
    while (*l->cur != '\0') {
        char c = *l->cur++;
        if (c == '\n') { l->line++; l->line_start = l->cur; continue; }
        if (c == ']') {
            int k = 0;
            while (l->cur[k] == '=') k++;
            if (k == level && l->cur[k] == ']') { l->cur += k + 1; return true; }
        }
    }
    return false;
}

static void skip_whitespace_and_comments(SeaLexer *l) {
    for (;;) {
        char c = peek_char(l);
        switch (c) {
            case ' ': case '\t': case '\r':
                l->cur++;
                break;
            case '\n':
                l->cur++;
                l->line++;
                l->line_start = l->cur;
                break;
            case '-':
                // Comment: -- to the end of the line, or a long one: --[[ ... ]]
                if (peek_next(l) == '-') {
                    if (l->keep_comments) return;
                    int level = long_bracket_level(l->cur + 2);
                    if (level >= 0) {
                        const char *at = l->cur;
                        int line = l->line;
                        const char *ls = l->line_start;
                        if (!skip_long_comment(l, level)) {
                            l->bad_comment = at;
                            l->bad_comment_line = line;
                            l->bad_comment_col = (int)(at - ls) + 1;
                            return;
                        }
                        break;
                    }
                    l->cur += 2;
                    while (*l->cur != '\0' && *l->cur != '\n') l->cur++;
                } else {
                    return;
                }
                break;
            case '#':
                // Hash-bang: # at start of file
                if (l->cur == l->src || l->cur[-1] == '\n') {
                    if (l->keep_comments) return;
                    while (*l->cur != '\0' && *l->cur != '\n') l->cur++;
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static SeaTK check_keyword(const char *start, int len) {
    for (int i = 0; keywords[i].word; i++) {
        if ((int)strlen(keywords[i].word) == len &&
            memcmp(keywords[i].word, start, (size_t)len) == 0) {
            return keywords[i].tk;
        }
    }
    return TK_IDENT;
}

static SeaToken make_token(SeaLexer *l, SeaTK type) {
    SeaToken t;
    t.type = type;
    t.line = l->tok_line;
    t.col = l->tok_col;
    t.start = l->start;
    t.len = (int)(l->cur - l->start);
    t.is_float = false;
    t.val.number = 0;
    return t;
}

static SeaToken error_token(SeaLexer *l, const char *msg) {
    SeaToken t;
    t.type = TK_ERROR;
    t.line = l->tok_line;
    t.col = l->tok_col;
    t.is_float = false;
    t.start = msg;
    t.len = (int)strlen(msg);
    t.val.number = 0;
    if (!l->err_msg) {
        l->err_msg = (char*)sea_malloc(strlen(msg) + 1);
        strcpy(l->err_msg, msg);
    }
    return t;
}

// ---- String scanning ----

/* Decode the escape after a backslash: *pp points at the character after
 * it and is advanced past the escape. Writes 1 to 4 bytes to out and returns
 * their count, or 0 with *err set. In a template string (`...`) \` and \$
 * are escapes too. A backslash before a newline keeps the newline (the
 * caller counts the line). */
static int lex_escape(const char **pp, char out[4], bool tmpl, const char **err) {
    static _Thread_local char msg[192];   /* error_token copies it */
    const char *p = *pp;
    char esc = *p++;
    int n = 1;
    switch (esc) {
        case 'n':  out[0] = '\n'; break;
        case 't':  out[0] = '\t'; break;
        case 'r':  out[0] = '\r'; break;
        case '\\': out[0] = '\\'; break;
        case '\'': out[0] = '\''; break;
        case '"':  out[0] = '"';  break;
        case '0':  out[0] = '\0'; break;
        case 'a':  out[0] = '\a'; break;
        case 'b':  out[0] = '\b'; break;
        case 'f':  out[0] = '\f'; break;
        case 'v':  out[0] = '\v'; break;
        case '\n': out[0] = '\n'; break;
        case 'u': {
            /* \u{XXXX} — a code point (1 to 6 hex digits), written as UTF-8 */
            uint32_t cp = 0;
            int digits = 0;
            if (*p == '{') {
                p++;
                while (isxdigit((unsigned char)*p) && digits < 7) {
                    int h = isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10);
                    cp = cp * 16 + (uint32_t)h;
                    digits++;
                    p++;
                }
            }
            n = 0;
            if (digits >= 1 && digits <= 6 && *p == '}') n = sea_utf8_encode(cp, out);
            if (!n) {
                *err = "invalid \\u escape (use \\u{XXXX} with a code point up to 10FFFF, not a surrogate)";
                return 0;
            }
            p++;
            break;
        }
        case 'x': {
            /* \xNN — two hex digits */
            char h1 = p[0], h2 = h1 ? p[1] : 0;
            if (!isxdigit((unsigned char)h1) || !isxdigit((unsigned char)h2)) {
                *err = "invalid \\x escape (use \\xHH with two hex digits)";
                return 0;
            }
            p += 2;
            int hi = isdigit((unsigned char)h1) ? h1 - '0' : (tolower((unsigned char)h1) - 'a' + 10);
            int lo = isdigit((unsigned char)h2) ? h2 - '0' : (tolower((unsigned char)h2) - 'a' + 10);
            out[0] = (char)(hi * 16 + lo);
            break;
        }
        default:
            if (tmpl && (esc == '`' || esc == '$')) {
                out[0] = esc;
                break;
            }
            if (esc == '\0')
                snprintf(msg, sizeof msg, "unterminated string");
            else if ((unsigned char)esc >= 0x20 && (unsigned char)esc < 0x7F)
                snprintf(msg, sizeof msg, "invalid escape '\\%c' (valid: \\n \\t \\r \\\\ \\\" \\' \\0 \\a \\b \\f \\v \\xHH \\u{XXXX}%s; "
                         "write a backslash as \\\\)", esc, tmpl ? " \\` \\$" : "");
            else
                snprintf(msg, sizeof msg, "invalid escape (a backslash followed by byte 0x%02X)", (unsigned char)esc);
            *err = msg;
            return 0;
    }
    *pp = p;
    return n;
}

bool sea_lex_unescape(const char *s, size_t len, bool tmpl, char **out, size_t *out_len, const char **err) {
    char *buf = (char*)sea_malloc(len + 1);
    size_t n = 0;
    const char *p = s, *end = s + len;
    while (p < end) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            char enc[4];
            int k = lex_escape(&p, enc, tmpl, err);
            if (!k) {
                sea_free(buf, len + 1);
                return false;
            }
            for (int i = 0; i < k; i++) buf[n++] = enc[i];
        } else {
            buf[n++] = *p++;
        }
    }
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return true;
}

/* Skip a template literal starting after its opening backtick; returns false
 * at the end of the input. Counts lines. ${...} may hold strings, braces and
 * templates of their own. */
static bool skip_template(SeaLexer *l);

static void lex_newline(SeaLexer *l) {
    l->line++;
    l->line_start = l->cur;
}

static bool skip_quoted(SeaLexer *l, char q) {
    while (*l->cur && *l->cur != q) {
        if (*l->cur == '\\' && l->cur[1]) l->cur++;
        if (*l->cur == '\n') { l->cur++; lex_newline(l); continue; }
        l->cur++;
    }
    if (!*l->cur) return false;
    l->cur++;
    return true;
}

static bool skip_template(SeaLexer *l) {
    while (*l->cur && *l->cur != '`') {
        char c = *l->cur++;
        if (c == '\n') { lex_newline(l); continue; }
        if (c == '\\') {
            if (!*l->cur) return false;
            if (*l->cur == '\n') { l->cur++; lex_newline(l); continue; }
            l->cur++;
            continue;
        }
        if (c == '$' && *l->cur == '{') {
            l->cur++;
            int depth = 1;
            while (depth > 0) {
                char d = *l->cur;
                if (!d) return false;
                l->cur++;
                if (d == '\n') lex_newline(l);
                else if (d == '{') depth++;
                else if (d == '}') depth--;
                else if (d == '"' || d == '\'') { if (!skip_quoted(l, d)) return false; }
                else if (d == '`') { if (!skip_template(l)) return false; l->cur++; }
            }
        }
    }
    return *l->cur == '`';
}

/* A compound assignment token (+= ...): val.number is the operator (SeaBinop) */
static SeaToken op_assign(SeaLexer *l, int binop) {
    SeaToken t = make_token(l, TK_OP_ASSIGN);
    t.val.number = binop;
    return t;
}

static SeaToken scan_template(SeaLexer *l) {
    if (!skip_template(l)) return error_token(l, "unterminated template string (a ` without its closing `)");
    l->cur++;   /* the closing backtick */
    return make_token(l, TK_TEMPLATE);
}

static SeaToken scan_string(SeaLexer *l, char quote) {
    // We'll build the string in a dynamic buffer to handle escapes
    char *buf = NULL;
    int buf_len = 0, buf_cap = 0;

    while (*l->cur != '\0' && *l->cur != quote) {
        char c = *l->cur++;
        if (c == '\n') {
            l->line++;
            l->line_start = l->cur;
        }
        if (c == '\\') {
            if (*l->cur == '\n') {
                l->line++;
                l->line_start = l->cur + 1;
            }
            const char *err = NULL;
            char enc[4];
            const char *p = l->cur;
            int k = lex_escape(&p, enc, false, &err);
            if (!k) {
                if (buf) sea_free(buf, (size_t)buf_cap);
                return error_token(l, err);
            }
            l->cur = p;
            for (int i = 0; i < k - 1; i++) SEA_DYNARRAY_PUSH(buf, buf_len, buf_cap, char, enc[i]);
            c = enc[k - 1];
        }
        SEA_DYNARRAY_PUSH(buf, buf_len, buf_cap, char, c);
    }
    if (*l->cur != quote) {
        if (buf) sea_free(buf, (size_t)buf_cap);
        return error_token(l, "unterminated string");
    }
    l->cur++; // consume closing quote

    // Null-terminate
    SEA_DYNARRAY_PUSH(buf, buf_len, buf_cap, char, '\0');

    SeaToken t = make_token(l, TK_STRING);
    t.val.string.ptr = buf;    // caller must free
    t.val.string.len = (size_t)(buf_len - 1);
    return t;
}

// Long strings: [[ ... ]] or [=[ ... ]=]
static SeaToken scan_long_string(SeaLexer *l) {
    // Count '=' signs
    int level = 0;
    while (*l->cur == '=') { level++; l->cur++; }
    if (*l->cur != '[') {
        return error_token(l, "invalid long string delimiter");
    }
    l->cur++; // consume second [
    // Skip initial newline
    if (*l->cur == '\n') { l->cur++; l->line++; l->line_start = l->cur; }
    else if (*l->cur == '\r') {
        l->cur++;
        if (*l->cur == '\n') l->cur++;
        l->line++;
        l->line_start = l->cur;
    }

    char *buf = NULL;
    int buf_len = 0, buf_cap = 0;

    while (*l->cur != '\0') {
        char c = *l->cur++;
        if (c == '\n') { l->line++; l->line_start = l->cur; }
        if (c == ']') {
            int cnt = 0;
            while (*l->cur == '=' && cnt < level) { cnt++; l->cur++; }
            if (cnt == level && *l->cur == ']') {
                l->cur++; // consume closing ]
                SEA_DYNARRAY_PUSH(buf, buf_len, buf_cap, char, '\0');
                SeaToken t = make_token(l, TK_STRING);
                t.val.string.ptr = buf;
                t.val.string.len = (size_t)(buf_len - 1);
                return t;
            }
            // Not a match - add the ] and = signs we consumed
            SEA_DYNARRAY_PUSH(buf, buf_len, buf_cap, char, ']');
            for (int i = 0; i < cnt; i++) {
                SEA_DYNARRAY_PUSH(buf, buf_len, buf_cap, char, '=');
            }
            continue;
        }
        SEA_DYNARRAY_PUSH(buf, buf_len, buf_cap, char, c);
    }
    if (buf) sea_free(buf, (size_t)buf_cap);
    return error_token(l, "unterminated long string");
}

// ---- Number scanning ----

#define SEA_LITERAL_INT_MAX 140737488355327.0   /* 2^47 - 1: the largest int */
#define INT_TOO_LARGE "integer literal too large (an int is 48 bits, up to 140737488355327): " \
    "write a float (1e15, 999999999999999.0) or use bigint.new(\"...\")"

static SeaToken scan_number(SeaLexer *l) {
    // Hex?
    if (peek_char(l) == '0' && (peek_next(l) == 'x' || peek_next(l) == 'X')) {
        l->cur += 2;
        while (isxdigit((unsigned char)*l->cur)) l->cur++;
        SeaToken t = make_token(l, TK_NUMBER);
        // Parse hex
        char *end;
        t.val.number = (double)strtoull(l->start + 2, &end, 16);
        if (t.val.number > SEA_LITERAL_INT_MAX) return error_token(l, INT_TOO_LARGE);
        return t;
    }

    while (isdigit((unsigned char)*l->cur)) l->cur++;
    bool is_float = false;
    if (*l->cur == '.' && isdigit((unsigned char)l->cur[1])) {
        is_float = true;
        l->cur++;
        while (isdigit((unsigned char)*l->cur)) l->cur++;
    }
    if (*l->cur == 'e' || *l->cur == 'E') {
        is_float = true;
        l->cur++;
        if (*l->cur == '+' || *l->cur == '-') l->cur++;
        while (isdigit((unsigned char)*l->cur)) l->cur++;
    }
    SeaToken t = make_token(l, TK_NUMBER);
    t.is_float = is_float;
    char *end;
    t.val.number = strtod(l->start, &end);
    // an int is 48 bits: a wider literal would wrap around silently
    if (!is_float && t.val.number > SEA_LITERAL_INT_MAX) return error_token(l, INT_TOO_LARGE);
    return t;
}

// ---- Main scan function ----

static SeaToken scan_token(SeaLexer *l) {
    skip_whitespace_and_comments(l);
    l->start = l->cur;
    l->tok_line = l->line;
    l->tok_col = (int)(l->cur - l->line_start) + 1;
    if (l->bad_comment) {
        l->tok_line = l->bad_comment_line;
        l->tok_col = l->bad_comment_col;
        l->bad_comment = NULL;
        return error_token(l, "unfinished long comment");
    }

    if (*l->cur == '\0') {
        return make_token(l, TK_EOF);
    }

    /* Comments reach here only with keep_comments (a `--` comment, or the
     * `#` line at the start of the file or of a line) */
    if ((l->cur[0] == '-' && l->cur[1] == '-') ||
        (l->cur[0] == '#' && (l->cur == l->src || l->cur[-1] == '\n'))) {
        int level = l->cur[0] == '-' ? long_bracket_level(l->cur + 2) : -1;
        if (level >= 0) {
            if (!skip_long_comment(l, level)) return error_token(l, "unfinished long comment");
            return make_token(l, TK_COMMENT);
        }
        while (*l->cur != '\0' && *l->cur != '\n') l->cur++;
        return make_token(l, TK_COMMENT);
    }

    char c = advance_char(l);

    // Numbers
    if (isdigit((unsigned char)c)) {
        l->cur--; // back up
        l->cur = l->start;
        return scan_number(l);
    }
    // Decimal point starting a number
    if (c == '.' && isdigit((unsigned char)*l->cur)) {
        l->cur = l->start; // back up
        return scan_number(l);
    }

    // Identifiers and keywords
    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)*l->cur) || *l->cur == '_') l->cur++;
        int len = (int)(l->cur - l->start);
        SeaTK tk = check_keyword(l->start, len);
        return make_token(l, tk);
    }

    // Strings
    if (c == '"' || c == '\'') {
        return scan_string(l, c);
    }
    if (c == '`') {
        return scan_template(l);
    }

    // Long strings and comments
    if (c == '[') {
        if (*l->cur == '[' || *l->cur == '=') {
            const char *saved = l->cur;
            // Count = signs
            while (*l->cur == '=') l->cur++;
            if (*l->cur == '[') {
                l->cur = saved;
                return scan_long_string(l);
            }
            l->cur = saved;
        }
        return make_token(l, TK_LBRACKET);
    }

    switch (c) {
        case '(': return make_token(l, TK_LPAREN);
        case ')': return make_token(l, TK_RPAREN);
        case '{': return make_token(l, TK_LBRACE);
        case '}': return make_token(l, TK_RBRACE);
        case ']': return make_token(l, TK_RBRACKET);
        case ';': return make_token(l, TK_SEMICOLON);
        case ',': return make_token(l, TK_COMMA);
        case '%':
            if (match_char(l, '=')) return op_assign(l, BINOP_MOD);
            return make_token(l, TK_PERCENT);
        case '^':
            if (match_char(l, '=')) return op_assign(l, BINOP_POW);
            return make_token(l, TK_CARET);
        case '#': return error_token(l, "unexpected character '#': Sea has no length operator, use len(x)");
        case '&': return make_token(l, TK_AMP);
        case '|': return make_token(l, TK_PIPE);
        case '~':
            if (match_char(l, '=')) return make_token(l, TK_NEQ);
            return make_token(l, TK_TILDE);
        case '+':
            if (match_char(l, '=')) return op_assign(l, BINOP_ADD);
            return make_token(l, TK_PLUS);
        case '*':
            if (match_char(l, '=')) return op_assign(l, BINOP_MUL);
            return make_token(l, TK_STAR);
        case '?': return make_token(l, TK_QUESTION);
        case ':': return make_token(l, TK_COLON);
        case '/':
            if (match_char(l, '/')) {
                if (match_char(l, '=')) return op_assign(l, BINOP_IDIV);
                return make_token(l, TK_DSLASH);
            }
            if (match_char(l, '=')) return op_assign(l, BINOP_DIV);
            return make_token(l, TK_SLASH);
        case '-':
            if (match_char(l, '>')) return make_token(l, TK_ARROW_RIGHT);
            if (match_char(l, '=')) return op_assign(l, BINOP_SUB);
            return make_token(l, TK_MINUS);
        case '<':
            if (match_char(l, '-')) return make_token(l, TK_ARROW_LEFT);
            if (match_char(l, '<')) return make_token(l, TK_LSHIFT);
            if (match_char(l, '=')) return make_token(l, TK_LE);
            return make_token(l, TK_LT);
        case '>':
            if (match_char(l, '>')) return make_token(l, TK_RSHIFT);
            if (match_char(l, '=')) return make_token(l, TK_GE);
            return make_token(l, TK_GT);
        case '=':
            if (match_char(l, '=')) return make_token(l, TK_EQ);
            return make_token(l, TK_ASSIGN);
        case '!':
            if (match_char(l, '=')) return make_token(l, TK_NEQ);
            return error_token(l, "unexpected character '!': negation is `not x` (`!=` is not-equal)");
        case '.':
            if (match_char(l, '.')) {
                if (match_char(l, '.')) return make_token(l, TK_DOTDOTDOT);
                if (match_char(l, '=')) return op_assign(l, BINOP_CONCAT);
                return make_token(l, TK_DOTDOT);
            }
            return make_token(l, TK_DOT);
        default: {
            snprintf(l->msg_buf, sizeof(l->msg_buf), "unexpected character '%c'", c);
            return error_token(l, l->msg_buf);
        }
    }
}

// ---- Public API ----

void sea_lexer_init(SeaLexer *l, const char *source) {
    sea_lexer_init_ex(l, source, false);
}

void sea_lexer_init_ex(SeaLexer *l, const char *source, bool keep_comments) {
    l->src = source;
    l->cur = source;
    l->start = source;
    l->line = 1;
    l->line_start = source;
    l->tok_line = 1;
    l->tok_col = 1;
    l->keep_comments = keep_comments;
    l->has_lookahead = false;
    l->err_msg = NULL;
    l->bad_comment = NULL;
    /* Source is UTF-8: a byte-order mark is skipped, invalid bytes are an error */
    if ((unsigned char)source[0] == 0xEF && (unsigned char)source[1] == 0xBB && (unsigned char)source[2] == 0xBF) {
        l->cur = l->start = l->line_start = source + 3;
    }
    size_t n = strlen(l->cur);
    size_t bad = sea_utf8_check(l->cur, n);
    if (bad < n) {
        const char *at = l->cur + bad;
        for (const char *p = l->cur; p < at; p++)
            if (*p == '\n') { l->line++; l->line_start = p + 1; }
        l->tok_line = l->line;
        l->tok_col = (int)(at - l->line_start) + 1;
        snprintf(l->msg_buf, sizeof(l->msg_buf), "invalid UTF-8 in source (byte 0x%02X)", (unsigned char)*at);
        l->cur = l->start = at + strlen(at);       /* nothing after the error */
        l->current = error_token(l, l->msg_buf);
        return;
    }
    // Pre-scan first token
    l->current = scan_token(l);
}

SeaToken sea_lexer_next(SeaLexer *l) {
    SeaToken t = l->current;
    if (l->has_lookahead) {
        l->current = l->lookahead;
        l->has_lookahead = false;
    } else {
        l->current = scan_token(l);
    }
    return t;
}

SeaToken sea_lexer_peek(SeaLexer *l) {
    if (!l->has_lookahead) {
        l->lookahead = scan_token(l);
        l->has_lookahead = true;
    }
    return l->lookahead;
}

void sea_lexer_free(SeaLexer *l) {
    if (l->err_msg) {
        sea_free(l->err_msg, strlen(l->err_msg) + 1);
        l->err_msg = NULL;
    }
}

const char* sea_tk_name(SeaTK tk) {
    switch (tk) {
        case TK_NIL:       return "nil";
        case TK_TRUE:      return "true";
        case TK_FALSE:     return "false";
        case TK_NUMBER:    return "<number>";
        case TK_STRING:    return "<string>";
        case TK_TEMPLATE:  return "<template>";
        case TK_OP_ASSIGN: return "compound assignment";
        case TK_IDENT:     return "<identifier>";
        case TK_AND:       return "and";
        case TK_BREAK:     return "break";
        case TK_CHAN:      return "chan";
        case TK_CONST:     return "const";
        case TK_CONTINUE:  return "continue";
        case TK_DEFAULT:   return "default";
        case TK_DEL:       return "del";
        case TK_DEFER:     return "defer";
        case TK_DO:        return "do";
        case TK_ELSE:      return "else";
        case TK_ELSEIF:    return "elseif";
        case TK_END:       return "end";
        case TK_FOR:       return "for";
        case TK_FUNCTION:  return "function";
        case TK_GO:        return "go";
        case TK_IF:        return "if";
        case TK_IN:        return "in";
        case TK_LOCAL:     return "local";
        case TK_NOT:       return "not";
        case TK_OR:        return "or";
        case TK_PANIC:     return "panic";
        case TK_RECOVER:   return "recover";
        case TK_RETURN:    return "return";
        case TK_SELECT:    return "select";
        case TK_CASE:      return "case";
        case TK_THEN:      return "then";
        case TK_WHILE:     return "while";
        case TK_PLUS:      return "+";
        case TK_MINUS:     return "-";
        case TK_STAR:      return "*";
        case TK_SLASH:     return "/";
        case TK_DSLASH:    return "//";
        case TK_PERCENT:   return "%";
        case TK_CARET:     return "^";
        case TK_AMP:       return "&";
        case TK_PIPE:      return "|";
        case TK_TILDE:     return "~";
        case TK_LSHIFT:    return "<<";
        case TK_RSHIFT:    return ">>";
        case TK_EQ:        return "==";
        case TK_NEQ:       return "~=";
        case TK_LT:        return "<";
        case TK_LE:        return "<=";
        case TK_GT:        return ">";
        case TK_GE:        return ">=";
        case TK_ASSIGN:    return "=";
        case TK_LPAREN:    return "(";
        case TK_RPAREN:    return ")";
        case TK_LBRACE:    return "{";
        case TK_RBRACE:    return "}";
        case TK_LBRACKET:  return "[";
        case TK_RBRACKET:  return "]";
        case TK_SEMICOLON: return ";";
        case TK_COLON:     return ":";
        case TK_COMMA:     return ",";
        case TK_DOT:       return ".";
        case TK_DOTDOT:    return "..";
        case TK_DOTDOTDOT: return "...";
        case TK_ARROW_LEFT:  return "<-";
        case TK_ARROW_RIGHT: return "->";
        case TK_QUESTION:  return "?";
        case TK_COMMENT:   return "<comment>";
        case TK_EOF:       return "<eof>";
        case TK_ERROR:     return "<error>";
        default:           return "<unknown>";
    }
}
