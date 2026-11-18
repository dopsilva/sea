/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * sea_lib_re.c — the built-in `re` module: regular expressions.
 *
 * RE2-style syntax matched by a Pike VM (Thompson NFA simulation with
 * submatch tracking): time linear in the input for every pattern, no
 * backtracking blow-up, leftmost-first semantics (like Perl, Go, RE2). No
 * backreferences or lookaround. Patterns and subjects are UTF-8; `.` and
 * classes match code points (an invalid byte is one character).
 *
 *   re.compile(pattern [, flags]) → regex | error
 *   re.test(p, s) → bool            re.match(p, s [, init]) → match | nil
 *   re.captures(p, s [, init]) → [whole, group 1, ...] | nil (no match table)
 *   re.all(p, s [, n]) → array of matches     re.findall(p, s [, n]) → array of strings
 *   re.gsub(p, s, repl [, n]) → string        re.split(p, s [, n]) → array
 *   re.escape(s) → string
 *
 * p is a pattern string or a compiled regex (whose methods are the same
 * functions: rx:test(s), rx:match(s), ...). A match is a table: text, start,
 * stop (1-based byte positions, inclusive, like string.find), groups (array:
 * [0] the whole match, [i] group i or nil), spans ([i] = [start, stop] or
 * nil) and named ({name = text}).
 *
 * The compiled program is kept as a string in the regex table (field
 * __prog): immutable and freed by reference counting. It is validated
 * on its first use in a thread (prog_load), so a replaced field cannot crash
 * the VM.
 *
 * Short texts (and windows of long ones) are searched by a bounded
 * backtracker, linear like the Pike VM; a split between branches that start
 * with disjoint characters (I_SPLITP: \S+ then a space) reads the whole run
 * of a loop in one go.
 */

#include "sea_array.h"
#include "sea_map.h"
#include "sea_rope.h"
#include "sea_stdlib.h"
#include "sea_struct.h"
#include "sea_unicode.h"
#include "sea_utf8.h"
#include "sea_vm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RE_MAX_INST    100000    /* program size */
#define RE_MAX_REPEAT  1000      /* {n,m} bounds */
#define RE_MAX_DEPTH   1000      /* parser nesting */
#define RE_MAX_GROUPS  1000
#define RE_MAGIC       0x52454733u   /* "REG3" */

/* ======================================================================== */
/* Parse: pattern → AST                                                     */
/* ======================================================================== */

enum { N_EMPTY, N_LIT, N_ANY, N_CLASS, N_CAT, N_ALT, N_REPEAT, N_GROUP, N_ASSERT };
enum { A_BOL, A_EOL, A_BOT, A_EOT, A_WORDB, A_NWORDB };

typedef struct { uint32_t lo, hi; } Range;

typedef struct {
    Range *r;
    size_t n, cap;
    bool   neg, fold;
} CharSet;

typedef struct Node Node;
struct Node {
    int      type;
    uint32_t cp;          /* N_LIT */
    bool     fold;        /* N_LIT, N_CLASS: case-insensitive */
    bool     dotnl;       /* N_ANY: matches \n too */
    int      klass;       /* N_CLASS: index into the parser's sets */
    int      min, max;    /* N_REPEAT (max -1: unbounded) */
    bool     greedy;
    int      group;       /* N_GROUP: capture index, -1 non-capturing */
    int      assert;      /* N_ASSERT */
    Node   **kids;
    size_t   n, cap;
};

typedef struct {
    const char *s;
    size_t      len, pos;
    bool        fold, multiline, dotnl, ungreedy;   /* current flags */
    int         ngroups;
    char      **names;            /* names[i]: group i+1's name or NULL */
    CharSet    *sets;
    size_t      nsets, capsets;
    int         depth;
    const char *err;
    char        errbuf[160];
    size_t      err_pos;
    Node      **all;              /* every node, for freeing */
    size_t      nall, capall;
} Parser;

static Node *node_new(Parser *p, int type) {
    Node *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->type = type;
    if (p->nall == p->capall) {
        p->capall = p->capall ? p->capall * 2 : 64;
        p->all = realloc(p->all, p->capall * sizeof *p->all);
    }
    p->all[p->nall++] = n;
    return n;
}

static void node_add(Node *parent, Node *kid) {
    if (parent->n == parent->cap) {
        parent->cap = parent->cap ? parent->cap * 2 : 4;
        parent->kids = realloc(parent->kids, parent->cap * sizeof *parent->kids);
    }
    parent->kids[parent->n++] = kid;
}

static void parser_free(Parser *p) {
    for (size_t i = 0; i < p->nall; i++) {
        free(p->all[i]->kids);
        free(p->all[i]);
    }
    free(p->all);
    for (size_t i = 0; i < p->nsets; i++) free(p->sets[i].r);
    free(p->sets);
    for (int i = 0; i < p->ngroups; i++) free(p->names[i]);
    free(p->names);
}

static Node *perr(Parser *p, const char *msg) {
    if (!p->err) { p->err = msg; p->err_pos = p->pos; }
    return NULL;
}

static bool more(Parser *p) { return p->pos < p->len; }
static int peekc(Parser *p) { return more(p) ? (unsigned char)p->s[p->pos] : -1; }

/* The next code point of the pattern (an invalid byte stands for itself as
 * U+FFFD would lose it: patterns must be UTF-8). */
static bool next_cp(Parser *p, uint32_t *cp) {
    uint32_t c;
    int k = sea_utf8_decode((const unsigned char *)p->s + p->pos, p->len - p->pos, &c);
    if (!k) { perr(p, "invalid UTF-8 in pattern"); return false; }
    p->pos += (size_t)k;
    *cp = c;
    return true;
}

static void set_add(CharSet *cs, uint32_t lo, uint32_t hi) {
    if (cs->n == cs->cap) {
        cs->cap = cs->cap ? cs->cap * 2 : 8;
        cs->r = realloc(cs->r, cs->cap * sizeof *cs->r);
    }
    cs->r[cs->n++] = (Range){ lo, hi };
}

static int range_cmp(const void *a, const void *b) {
    const Range *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

static void set_normalize(CharSet *cs) {
    if (cs->n < 2) return;
    qsort(cs->r, cs->n, sizeof *cs->r, range_cmp);
    size_t w = 0;
    for (size_t i = 1; i < cs->n; i++) {
        if (cs->r[i].lo <= cs->r[w].hi + 1) {
            if (cs->r[i].hi > cs->r[w].hi) cs->r[w].hi = cs->r[i].hi;
        } else {
            cs->r[++w] = cs->r[i];
        }
    }
    cs->n = w + 1;
}

/* Add the complement of src's ranges (within 0..10FFFF) to dst. */
static void set_add_negated(CharSet *dst, const Range *r, size_t n) {
    uint32_t next = 0;
    for (size_t i = 0; i < n; i++) {
        if (r[i].lo > next) set_add(dst, next, r[i].lo - 1);
        next = r[i].hi + 1;
    }
    if (next <= 0x10FFFF) set_add(dst, next, 0x10FFFF);
}

static const Range R_DIGIT[] = { { '0', '9' } };
static const Range R_WORD[]  = { { '0', '9' }, { 'A', 'Z' }, { '_', '_' }, { 'a', 'z' } };
static const Range R_SPACE[] = { { '\t', '\n' }, { '\f', '\r' }, { ' ', ' ' } };

/* \d \D \w \W \s \S into cs; false if c is not one of them. */
static bool perl_class(CharSet *cs, int c) {
    const Range *r;
    size_t n;
    switch (c | 0x20) {
        case 'd': r = R_DIGIT; n = 1; break;
        case 'w': r = R_WORD; n = 4; break;
        case 's': r = R_SPACE; n = 3; break;
        default: return false;
    }
    if (c >= 'a') for (size_t i = 0; i < n; i++) set_add(cs, r[i].lo, r[i].hi);
    else set_add_negated(cs, r, n);
    return true;
}

static int new_set(Parser *p) {
    if (p->nsets == p->capsets) {
        p->capsets = p->capsets ? p->capsets * 2 : 8;
        p->sets = realloc(p->sets, p->capsets * sizeof *p->sets);
    }
    memset(&p->sets[p->nsets], 0, sizeof(CharSet));
    return (int)p->nsets++;
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* After a backslash: a literal code point escape (\n, \x41, \x{263A}, \.,
 * ...). Returns 1 with *cp, 0 if c starts another kind of escape, -1 on error. */
static int escape_cp(Parser *p, int c, uint32_t *cp) {
    switch (c) {
        case 'n': *cp = '\n'; return 1;
        case 't': *cp = '\t'; return 1;
        case 'r': *cp = '\r'; return 1;
        case 'f': *cp = '\f'; return 1;
        case 'v': *cp = '\v'; return 1;
        case 'a': *cp = '\a'; return 1;
        case 'e': *cp = 0x1B; return 1;
        case '0': *cp = 0; return 1;
        case 'x': case 'u': {
            uint32_t v = 0;
            if (peekc(p) == '{') {
                p->pos++;
                int digits = 0;
                while (more(p) && hexv(peekc(p)) >= 0 && digits < 7) {
                    v = v * 16 + (uint32_t)hexv(peekc(p));
                    p->pos++;
                    digits++;
                }
                if (!digits || digits > 6 || peekc(p) != '}' || v > 0x10FFFF) { perr(p, "invalid \\x{...} escape"); return -1; }
                p->pos++;
            } else if (c == 'x') {
                if (p->pos + 2 > p->len || hexv(p->s[p->pos]) < 0 || hexv(p->s[p->pos + 1]) < 0) {
                    perr(p, "invalid \\x escape (two hex digits or \\x{...})");
                    return -1;
                }
                v = (uint32_t)(hexv(p->s[p->pos]) * 16 + hexv(p->s[p->pos + 1]));
                p->pos += 2;
            } else {
                perr(p, "invalid \\u escape (use \\u{...})");
                return -1;
            }
            *cp = v;
            return 1;
        }
        default:
            if (c < 0x80 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                *cp = (uint32_t)c;                     /* escaped punctuation */
                return 1;
            }
            return 0;
    }
}

static const struct { const char *name; const char *spec; } POSIX[] = {
    { "alnum", "09AZaz" }, { "alpha", "AZaz" }, { "ascii", "\x01\x7f" }, { "blank", "\t\t  " },
    { "cntrl", "\x01\x1f\x7f\x7f" }, { "digit", "09" }, { "graph", "!~" }, { "lower", "az" },
    { "print", " ~" }, { "punct", "!/:@[`{~" }, { "space", "\t\r  " }, { "upper", "AZ" },
    { "word", "09AZ__az" }, { "xdigit", "09AFaf" },
};

/* Unicode properties: \p{L} \pL \P{Lu} ... (a subset: letters, case, digits, punctuation
 * and a few scripts). The tables of L, Lu, Ll and Nd are generated (src/sea_unicode.h). */
#define UNI_TABLE(t) t, sizeof t / sizeof t[0]
static const uint32_t UNI_P[][2] = {
    {0x21, 0x23}, {0x25, 0x2A}, {0x2C, 0x2F}, {0x3A, 0x3B}, {0x3F, 0x40}, {0x5B, 0x5D}, {0x5F, 0x5F},
    {0x7B, 0x7B}, {0x7D, 0x7D}, {0xA1, 0xA1}, {0xA7, 0xA7}, {0xAB, 0xAB}, {0xB6, 0xB7}, {0xBB, 0xBB},
    {0xBF, 0xBF}, {0x2010, 0x2027}, {0x2030, 0x205E}, {0x3001, 0x3003}, {0x3008, 0x3011}, {0xFF01, 0xFF03},
    {0xFF05, 0xFF0A}, {0xFF0C, 0xFF0F}, {0xFF1A, 0xFF1B}, {0xFF1F, 0xFF20},
};
static const uint32_t UNI_LATIN[][2] = {
    {0x41, 0x5A}, {0x61, 0x7A}, {0xAA, 0xAA}, {0xBA, 0xBA}, {0xC0, 0xD6}, {0xD8, 0xF6}, {0xF8, 0x2B8},
    {0x1E00, 0x1EFF}, {0x2C60, 0x2C7F}, {0xA720, 0xA7FF}, {0xFF21, 0xFF3A}, {0xFF41, 0xFF5A},
};
static const uint32_t UNI_GREEK[][2] = { {0x370, 0x3FF}, {0x1F00, 0x1FFF} };
static const uint32_t UNI_CYRILLIC[][2] = { {0x400, 0x52F}, {0x1C80, 0x1C8F}, {0x2DE0, 0x2DFF}, {0xA640, 0xA69F} };
static const uint32_t UNI_HEBREW[][2] = { {0x591, 0x5F4}, {0xFB1D, 0xFB4F} };
static const uint32_t UNI_ARABIC[][2] = { {0x600, 0x6FF}, {0x750, 0x77F}, {0xFB50, 0xFDFF}, {0xFE70, 0xFEFF} };
static const uint32_t UNI_HAN[][2] = { {0x2E80, 0x2FDF}, {0x3005, 0x3007}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xF900, 0xFAFF}, {0x20000, 0x2FA1F} };
static const uint32_t UNI_HIRAGANA[][2] = { {0x3041, 0x309F} };
static const uint32_t UNI_KATAKANA[][2] = { {0x30A0, 0x30FF}, {0x31F0, 0x31FF}, {0xFF66, 0xFF9F} };
static const uint32_t UNI_HANGUL[][2] = { {0x1100, 0x11FF}, {0x3130, 0x318F}, {0xAC00, 0xD7AF} };

static const struct { const char *name; const uint32_t (*r)[2]; size_t n; const char *alias; } UNI_PROPS[] = {
    { "L", UNI_TABLE(SEA_UNI_L), "Letter" },       { "Lu", UNI_TABLE(SEA_UNI_LU), "Uppercase_Letter" },
    { "Ll", UNI_TABLE(SEA_UNI_LL), "Lowercase_Letter" },
    { "N", UNI_TABLE(SEA_UNI_ND), "Number" },      { "Nd", UNI_TABLE(SEA_UNI_ND), "Decimal_Number" },
    { "P", UNI_TABLE(UNI_P), "Punctuation" },
    { "Latin", UNI_TABLE(UNI_LATIN), NULL },       { "Greek", UNI_TABLE(UNI_GREEK), NULL },
    { "Cyrillic", UNI_TABLE(UNI_CYRILLIC), NULL }, { "Hebrew", UNI_TABLE(UNI_HEBREW), NULL },
    { "Arabic", UNI_TABLE(UNI_ARABIC), NULL },     { "Han", UNI_TABLE(UNI_HAN), NULL },
    { "Hiragana", UNI_TABLE(UNI_HIRAGANA), NULL }, { "Katakana", UNI_TABLE(UNI_KATAKANA), NULL },
    { "Hangul", UNI_TABLE(UNI_HANGUL), NULL },
};

/* After "\p" or "\P" (negate): `{Name}` or one letter. Adds the property to cs; false with
 * the error set on a name that is not known. */
static bool uni_prop(Parser *p, CharSet *cs, bool negate) {
    char name[24];
    size_t n = 0;
    if (peekc(p) == '{') {
        p->pos++;
        if (peekc(p) == '^') { negate = !negate; p->pos++; }
        while (more(p) && peekc(p) != '}' && n < sizeof name - 1) name[n++] = p->s[p->pos++];
        if (peekc(p) != '}') { perr(p, "invalid or unsupported Unicode class"); return false; }
        p->pos++;
    } else if (more(p)) {
        name[n++] = p->s[p->pos++];
    }
    name[n] = 0;
    for (size_t i = 0; i < sizeof UNI_PROPS / sizeof *UNI_PROPS; i++) {
        if (strcmp(UNI_PROPS[i].name, name) != 0 && !(UNI_PROPS[i].alias && strcmp(UNI_PROPS[i].alias, name) == 0))
            continue;
        CharSet tmp = { 0 };
        for (size_t k = 0; k < UNI_PROPS[i].n; k++) set_add(&tmp, UNI_PROPS[i].r[k][0], UNI_PROPS[i].r[k][1]);
        set_normalize(&tmp);
        if (negate) set_add_negated(cs, tmp.r, tmp.n);
        else for (size_t k = 0; k < tmp.n; k++) set_add(cs, tmp.r[k].lo, tmp.r[k].hi);
        free(tmp.r);
        return true;
    }
    perr(p, "unknown Unicode class (supported: L Lu Ll N Nd P, and the scripts Latin Greek Cyrillic Hebrew Arabic Han Hiragana Katakana Hangul)");
    return false;
}

/* [ ... ] after the '[' */
static Node *parse_class(Parser *p) {
    size_t start = p->pos - 1;
    int idx = new_set(p);
    bool neg = false;
    if (peekc(p) == '^') { neg = true; p->pos++; }
    bool first = true;
    for (;;) {
        if (!more(p)) { p->pos = start; return perr(p, "missing closing ]"); }
        int c = peekc(p);
        if (c == ']' && !first) { p->pos++; break; }
        first = false;
        /* POSIX class [:name:] */
        if (c == '[' && p->pos + 1 < p->len && p->s[p->pos + 1] == ':') {
            const char *end = memchr(p->s + p->pos + 2, ']', p->len - p->pos - 2);
            if (end && end[-1] == ':') {
                size_t nlen = (size_t)(end - 1 - (p->s + p->pos + 2));
                const char *nm = p->s + p->pos + 2;
                bool pneg = nlen > 0 && nm[0] == '^';
                if (pneg) { nm++; nlen--; }
                bool found = false;
                for (size_t i = 0; i < sizeof POSIX / sizeof *POSIX; i++) {
                    if (strlen(POSIX[i].name) == nlen && memcmp(POSIX[i].name, nm, nlen) == 0) {
                        CharSet tmp = { 0 };
                        const char *sp = POSIX[i].spec;
                        for (size_t k = 0; sp[k]; k += 2) set_add(&tmp, (unsigned char)sp[k], (unsigned char)sp[k + 1]);
                        set_normalize(&tmp);
                        CharSet *cs = &p->sets[idx];
                        if (pneg) set_add_negated(cs, tmp.r, tmp.n);
                        else for (size_t k = 0; k < tmp.n; k++) set_add(cs, tmp.r[k].lo, tmp.r[k].hi);
                        free(tmp.r);
                        found = true;
                        break;
                    }
                }
                if (!found) return perr(p, "unknown POSIX class name");
                p->pos = (size_t)(end - p->s) + 1;
                continue;
            }
        }
        uint32_t lo;
        if (c == '\\') {
            p->pos++;
            if (!more(p)) return perr(p, "trailing backslash");
            int e = peekc(p);
            p->pos++;
            if (perl_class(&p->sets[idx], e)) continue;
            if (e == 'p' || e == 'P') {
                if (!uni_prop(p, &p->sets[idx], e == 'P')) return NULL;
                continue;
            }
            int r = escape_cp(p, e, &lo);
            if (r < 0) return NULL;
            if (r == 0) { p->pos -= 1; return perr(p, "invalid escape in character class"); }
        } else if (!next_cp(p, &lo)) {
            return NULL;
        }
        uint32_t hi = lo;
        if (peekc(p) == '-' && p->pos + 1 < p->len && p->s[p->pos + 1] != ']') {
            p->pos++;
            int c2 = peekc(p);
            if (c2 == '\\') {
                p->pos++;
                if (!more(p)) return perr(p, "trailing backslash");
                int e = peekc(p);
                p->pos++;
                int r = escape_cp(p, e, &hi);
                if (r <= 0) { if (r == 0) perr(p, "invalid range end in character class"); return NULL; }
            } else if (!next_cp(p, &hi)) {
                return NULL;
            }
            if (hi < lo) return perr(p, "invalid character class range (end before start)");
        }
        set_add(&p->sets[idx], lo, hi);
    }
    CharSet *cs = &p->sets[idx];
    set_normalize(cs);
    cs->neg = neg;
    cs->fold = p->fold;
    Node *n = node_new(p, N_CLASS);
    if (!n) return perr(p, "out of memory");
    n->klass = idx;
    n->fold = p->fold;
    return n;
}

static Node *parse_alt(Parser *p);

/* (?flags) or (?flags:...) after "(?"; returns the group node or NULL with
 * *flags_only set when it only changed flags. */
static Node *parse_flags(Parser *p, bool *flags_only) {
    bool on = true;
    bool f = p->fold, m = p->multiline, s = p->dotnl, u = p->ungreedy;
    bool any = false;
    for (;;) {
        int c = peekc(p);
        if (c < 0) return perr(p, "missing closing )");
        p->pos++;
        switch (c) {
            case 'i': f = on; any = true; break;
            case 'm': m = on; any = true; break;
            case 's': s = on; any = true; break;
            case 'U': u = on; any = true; break;
            case '-':
                if (!on) return perr(p, "invalid flags group");
                on = false;
                any = false;
                break;
            case ')':
                if (!any) return perr(p, "invalid flags group");
                p->fold = f; p->multiline = m; p->dotnl = s; p->ungreedy = u;
                *flags_only = true;
                return NULL;
            case ':': {
                bool sf = p->fold, sm = p->multiline, ss = p->dotnl, su = p->ungreedy;
                p->fold = f; p->multiline = m; p->dotnl = s; p->ungreedy = u;
                Node *inner = parse_alt(p);
                p->fold = sf; p->multiline = sm; p->dotnl = ss; p->ungreedy = su;
                if (!inner) return NULL;
                if (peekc(p) != ')') return perr(p, "missing closing )");
                p->pos++;
                Node *g = node_new(p, N_GROUP);
                if (!g) return perr(p, "out of memory");
                g->group = -1;
                node_add(g, inner);
                return g;
            }
            default:
                p->pos--;
                return perr(p, "invalid or unsupported group syntax (no lookaround or backreferences)");
        }
    }
}

static Node *parse_atom(Parser *p, bool *none) {
    *none = false;
    int c = peekc(p);
    if (c == '(') {
        size_t open = p->pos;
        p->pos++;
        if (++p->depth > RE_MAX_DEPTH) return perr(p, "pattern nested too deeply");
        int group = -1;
        char *name = NULL;
        if (peekc(p) == '?') {
            p->pos++;
            if (peekc(p) == 'P' || peekc(p) == '<') {
                if (peekc(p) == 'P') p->pos++;
                if (peekc(p) != '<') return perr(p, "invalid named group (use (?P<name>...) or (?<name>...))");
                p->pos++;
                size_t ns = p->pos;
                while (more(p) && (isalnum((unsigned char)p->s[p->pos]) || p->s[p->pos] == '_')) p->pos++;
                if (p->pos == ns || peekc(p) != '>' || isdigit((unsigned char)p->s[ns]))
                    return perr(p, "invalid group name (letters, digits and _, not starting with a digit)");
                name = strndup(p->s + ns, p->pos - ns);
                for (int i = 0; i < p->ngroups; i++)
                    if (p->names[i] && strcmp(p->names[i], name) == 0) { free(name); return perr(p, "duplicate group name"); }
                p->pos++;
            } else if (peekc(p) == ':') {
                p->pos++;
                bool sf = p->fold, sm = p->multiline, ss = p->dotnl, su = p->ungreedy;
                Node *inner = parse_alt(p);
                p->fold = sf; p->multiline = sm; p->dotnl = ss; p->ungreedy = su;   /* (?i) inside ends here */
                if (!inner) return NULL;
                if (peekc(p) != ')') { p->pos = open; return perr(p, "missing closing )"); }
                p->pos++;
                p->depth--;
                Node *g = node_new(p, N_GROUP);
                if (!g) return perr(p, "out of memory");
                g->group = -1;
                node_add(g, inner);
                return g;
            } else {
                bool flags_only = false;
                Node *g = parse_flags(p, &flags_only);
                p->depth--;
                if (flags_only) { *none = true; return NULL; }
                return g;
            }
        }
        if (p->ngroups >= RE_MAX_GROUPS) { free(name); return perr(p, "too many groups"); }
        group = ++p->ngroups;
        p->names = realloc(p->names, (size_t)p->ngroups * sizeof *p->names);
        p->names[group - 1] = name;
        bool sf = p->fold, sm = p->multiline, ss = p->dotnl, su = p->ungreedy;
        Node *inner = parse_alt(p);
        p->fold = sf; p->multiline = sm; p->dotnl = ss; p->ungreedy = su;
        if (!inner) return NULL;
        if (peekc(p) != ')') { p->pos = open; return perr(p, "missing closing )"); }
        p->pos++;
        p->depth--;
        Node *g = node_new(p, N_GROUP);
        if (!g) return perr(p, "out of memory");
        g->group = group;
        node_add(g, inner);
        return g;
    }
    if (c == '[') { p->pos++; return parse_class(p); }
    if (c == '.') {
        p->pos++;
        Node *n = node_new(p, N_ANY);
        if (!n) return perr(p, "out of memory");
        n->dotnl = p->dotnl;
        return n;
    }
    if (c == '^' || c == '$') {
        p->pos++;
        Node *n = node_new(p, N_ASSERT);
        if (!n) return perr(p, "out of memory");
        n->assert = c == '^' ? (p->multiline ? A_BOL : A_BOT) : (p->multiline ? A_EOL : A_EOT);
        return n;
    }
    if (c == '\\') {
        p->pos++;
        if (!more(p)) return perr(p, "trailing backslash at end of pattern");
        int e = peekc(p);
        p->pos++;
        int as = e == 'A' ? A_BOT : e == 'z' ? A_EOT : e == 'b' ? A_WORDB : e == 'B' ? A_NWORDB : -1;
        if (as >= 0) {
            Node *n = node_new(p, N_ASSERT);
            if (!n) return perr(p, "out of memory");
            n->assert = as;
            return n;
        }
        int idx = new_set(p);
        if (e == 'p' || e == 'P') {
            if (!uni_prop(p, &p->sets[idx], e == 'P')) return NULL;
            set_normalize(&p->sets[idx]);
            p->sets[idx].fold = p->fold;
            Node *n = node_new(p, N_CLASS);
            if (!n) return perr(p, "out of memory");
            n->klass = idx;
            n->fold = p->fold;
            return n;
        }
        if (perl_class(&p->sets[idx], e)) {
            set_normalize(&p->sets[idx]);
            Node *n = node_new(p, N_CLASS);
            if (!n) return perr(p, "out of memory");
            n->klass = idx;
            return n;
        }
        p->nsets--;
        uint32_t cp;
        int r = escape_cp(p, e, &cp);
        if (r < 0) return NULL;
        if (r == 0) {
            p->pos -= 1;
            if (e >= '1' && e <= '9') return perr(p, "backreferences are not supported");
            return perr(p, "invalid escape sequence");
        }
        Node *n = node_new(p, N_LIT);
        if (!n) return perr(p, "out of memory");
        n->cp = cp;
        n->fold = p->fold;
        return n;
    }
    if (c == '*' || c == '+' || c == '?') return perr(p, "missing argument to repetition operator");
    uint32_t cp;
    if (!next_cp(p, &cp)) return NULL;
    Node *n = node_new(p, N_LIT);
    if (!n) return perr(p, "out of memory");
    n->cp = cp;
    n->fold = p->fold;
    return n;
}

/* {n}, {n,}, {n,m} at p->pos (after '{'); false (pos unchanged) if not a repeat. */
static bool parse_braces(Parser *p, int *min, int *max) {
    size_t save = p->pos;
    long a = -1, b;
    if (!more(p) || !isdigit((unsigned char)peekc(p))) { p->pos = save; return false; }
    a = 0;
    while (more(p) && isdigit((unsigned char)peekc(p))) { a = a * 10 + (peekc(p) - '0'); if (a > 100000) a = 100000; p->pos++; }
    b = a;
    if (peekc(p) == ',') {
        p->pos++;
        if (peekc(p) == '}') b = -1;
        else {
            if (!isdigit((unsigned char)peekc(p))) { p->pos = save; return false; }
            b = 0;
            while (more(p) && isdigit((unsigned char)peekc(p))) { b = b * 10 + (peekc(p) - '0'); if (b > 100000) b = 100000; p->pos++; }
        }
    }
    if (peekc(p) != '}') { p->pos = save; return false; }
    p->pos++;
    *min = (int)a;
    *max = (int)b;
    return true;
}

static Node *parse_repeat(Parser *p) {
    bool none;
    Node *atom = parse_atom(p, &none);
    if (none) return NULL;          /* a flags group: no node */
    if (!atom) return NULL;
    for (;;) {
        int c = peekc(p);
        int min, max;
        size_t at = p->pos;
        if (c == '*') { min = 0; max = -1; p->pos++; }
        else if (c == '+') { min = 1; max = -1; p->pos++; }
        else if (c == '?') { min = 0; max = 1; p->pos++; }
        else if (c == '{') {
            p->pos++;
            if (!parse_braces(p, &min, &max)) { p->pos = at; break; }    /* a literal '{' */
            if (min > RE_MAX_REPEAT || max > RE_MAX_REPEAT) { p->pos = at; return perr(p, "repeat count too large (at most 1000)"); }
            if (max >= 0 && max < min) { p->pos = at; return perr(p, "invalid repeat count (max below min)"); }
        } else break;
        if (atom->type == N_ASSERT || atom->type == N_EMPTY) { p->pos = at; return perr(p, "missing argument to repetition operator"); }
        bool greedy = true;
        if (peekc(p) == '?') { greedy = false; p->pos++; }
        if (p->ungreedy) greedy = !greedy;
        if (atom->type == N_REPEAT) { p->pos = at; return perr(p, "invalid nested repetition operator"); }
        Node *r = node_new(p, N_REPEAT);
        if (!r) return perr(p, "out of memory");
        r->min = min;
        r->max = max;
        r->greedy = greedy;
        node_add(r, atom);
        atom = r;
    }
    return atom;
}

static Node *parse_cat(Parser *p) {
    Node *cat = node_new(p, N_CAT);
    if (!cat) return perr(p, "out of memory");
    while (more(p) && peekc(p) != '|' && peekc(p) != ')') {
        size_t before = p->pos;
        Node *n = parse_repeat(p);
        if (!n) {
            if (p->err) return NULL;
            if (p->pos == before) return perr(p, "unexpected character");
            continue;                       /* a flags group */
        }
        node_add(cat, n);
    }
    return cat;
}

static Node *parse_alt(Parser *p) {
    Node *first = parse_cat(p);
    if (!first) return NULL;
    if (peekc(p) != '|') return first;
    Node *alt = node_new(p, N_ALT);
    if (!alt) return perr(p, "out of memory");
    node_add(alt, first);
    while (peekc(p) == '|') {
        p->pos++;
        Node *n = parse_cat(p);
        if (!n) return NULL;
        node_add(alt, n);
    }
    return alt;
}

/* ======================================================================== */
/* Compile: AST → program                                                   */
/* ======================================================================== */

/* I_SPLITP is an I_SPLIT whose two branches start with characters that
 * cannot both match (see mark_exclusive_splits): the backtracker reads the
 * next character and takes the one branch that can go on, pushing nothing */
enum { I_CHAR, I_CHARF, I_ANY, I_ANYNL, I_CLASS, I_MATCH, I_JMP, I_SPLIT, I_SAVE, I_ASSERT, I_NOP, I_SPLITP };

typedef struct { int32_t op, x, y; } Inst;

typedef struct {
    Inst  *inst;
    size_t n, cap;
    bool   overflow;
} Code;

static int emit(Code *c, int op, int x, int y) {
    if (c->n >= RE_MAX_INST) { c->overflow = true; return (int)c->n; }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->inst = realloc(c->inst, c->cap * sizeof *c->inst);
    }
    c->inst[c->n] = (Inst){ op, x, y };
    return (int)c->n++;
}

static void compile_node(Code *c, Node *n);

/* Can n match the empty string? */
static bool nullable(Node *n) {
    switch (n->type) {
        case N_EMPTY: case N_ASSERT: return true;
        case N_LIT: case N_ANY: case N_CLASS: return false;
        case N_CAT:
            for (size_t i = 0; i < n->n; i++) if (!nullable(n->kids[i])) return false;
            return true;
        case N_ALT:
            for (size_t i = 0; i < n->n; i++) if (nullable(n->kids[i])) return true;
            return false;
        case N_REPEAT: return n->min == 0 || nullable(n->kids[0]);
        default: return nullable(n->kids[0]);
    }
}

/* x+: x; split back to x or out */
static void compile_plus(Code *c, Node *body, bool greedy) {
    int start = (int)c->n;
    compile_node(c, body);
    int s = emit(c, I_SPLIT, 0, 0);
    if (c->overflow) return;
    c->inst[s].x = greedy ? start : s + 1;
    c->inst[s].y = greedy ? s + 1 : start;
}

static void compile_repeat_body(Code *c, Node *body, int min, int max, bool greedy) {
    if (max < 0) {
        if (min == 0) {
            if (nullable(body)) {
                /* x* with an x that can match empty is (x+)?, so an empty
                 * iteration still sets its groups (as RE2 and Go do) */
                int q = emit(c, I_SPLIT, 0, 0);
                compile_plus(c, body, greedy);
                int out = (int)c->n;
                if (c->overflow) return;
                c->inst[q].x = greedy ? q + 1 : out;
                c->inst[q].y = greedy ? out : q + 1;
                return;
            }
            /* L: split body, out; body; jmp L */
            int L = emit(c, I_SPLIT, 0, 0);
            compile_node(c, body);
            emit(c, I_JMP, L, 0);
            int out = (int)c->n;
            if (c->overflow) return;
            c->inst[L].x = greedy ? L + 1 : out;
            c->inst[L].y = greedy ? out : L + 1;
            return;
        }
        /* x{n,}: n-1 copies, then x+ */
        for (int i = 0; i < min - 1 && !c->overflow; i++) compile_node(c, body);
        compile_plus(c, body, greedy);
        return;
    }
    for (int i = 0; i < min && !c->overflow; i++) compile_node(c, body);
    /* (body(body(...)?)?)?: each optional copy jumps out on skip */
    int pending[RE_MAX_REPEAT];
    int np = 0;
    for (int i = min; i < max && !c->overflow; i++) {
        int s = emit(c, I_SPLIT, 0, 0);
        pending[np++] = s;
        compile_node(c, body);
        if (c->overflow) return;
        c->inst[s].x = greedy ? s + 1 : -1;
        c->inst[s].y = greedy ? -1 : s + 1;
    }
    int out = (int)c->n;
    for (int i = 0; i < np; i++) {
        Inst *in = &c->inst[pending[i]];
        if (in->x == -1) in->x = out;
        if (in->y == -1) in->y = out;
    }
}

static void compile_node(Code *c, Node *n) {
    if (c->overflow) return;
    switch (n->type) {
        case N_EMPTY: break;
        case N_LIT:
            if (n->fold && sea_utf8_lower(n->cp) != sea_utf8_upper(n->cp))
                emit(c, I_CHARF, (int)sea_utf8_lower(n->cp), 0);
            else
                emit(c, I_CHAR, (int)n->cp, 0);
            break;
        case N_ANY: emit(c, n->dotnl ? I_ANYNL : I_ANY, 0, 0); break;
        case N_CLASS: emit(c, I_CLASS, n->klass, 0); break;
        case N_ASSERT: emit(c, I_ASSERT, n->assert, 0); break;
        case N_CAT:
            for (size_t i = 0; i < n->n; i++) compile_node(c, n->kids[i]);
            break;
        case N_ALT: {
            /* split L1, next; L1: a; jmp end; next: split L2, ... */
            size_t nj = 0;
            int *J = malloc(n->n * sizeof *J);
            for (size_t i = 0; i < n->n; i++) {
                if (i + 1 < n->n) {
                    int s = emit(c, I_SPLIT, 0, 0);
                    compile_node(c, n->kids[i]);
                    J[nj++] = emit(c, I_JMP, 0, 0);
                    if (c->overflow) break;
                    c->inst[s].x = s + 1;
                    c->inst[s].y = (int)c->n;
                } else {
                    compile_node(c, n->kids[i]);
                }
            }
            if (!c->overflow)
                for (size_t i = 0; i < nj; i++) c->inst[J[i]].x = (int)c->n;
            free(J);
            break;
        }
        case N_GROUP:
            if (n->group >= 0) emit(c, I_SAVE, 2 * n->group, 0);
            compile_node(c, n->kids[0]);
            if (n->group >= 0) emit(c, I_SAVE, 2 * n->group + 1, 0);
            break;
        case N_REPEAT:
            compile_repeat_body(c, n->kids[0], n->min, n->max, n->greedy);
            break;
    }
}

/* The instruction that consumes the first character on the path from pc,
 * following saves, nops and jumps; -1 when the path splits, asserts or
 * matches before it reads a character. */
static int first_char_inst(const Code *c, int pc) {
    for (int steps = 0; steps < 64 && pc >= 0 && (size_t)pc < c->n; steps++) {
        const Inst *in = &c->inst[pc];
        switch (in->op) {
            case I_CHAR: case I_CHARF: case I_ANY: case I_ANYNL: case I_CLASS: return pc;
            case I_SAVE: case I_NOP: pc++; break;
            case I_JMP: pc = in->x; break;
            default: return -1;
        }
    }
    return -1;
}

/* The characters a consuming instruction accepts: its ASCII members, and
 * whether it may accept a character >= 128 */
static void char_set_of(const Inst *in, const Parser *p, uint32_t ascii[4], bool *high) {
    memset(ascii, 0, 4 * sizeof *ascii);
    *high = true;
    switch (in->op) {
        case I_CHAR:
            if (in->x < 128) { ascii[in->x >> 5] |= 1u << (in->x & 31); *high = false; }
            break;
        case I_CHARF:      /* 'k' also matches U+212A: keep high */
            if (in->x < 128) {
                uint32_t u = sea_utf8_upper((uint32_t)in->x);
                ascii[in->x >> 5] |= 1u << (in->x & 31);
                if (u < 128) ascii[u >> 5] |= 1u << (u & 31);
            }
            break;
        case I_ANY: case I_ANYNL:
            for (int k = 0; k < 4; k++) ascii[k] = ~0u;
            if (in->op == I_ANY) ascii[0] &= ~(1u << '\n');
            break;
        case I_CLASS: {
            const CharSet *cs = &p->sets[in->x];
            bool hi = cs->neg || cs->fold;
            for (size_t k = 0; k < cs->n; k++) if (cs->r[k].hi >= 128) hi = true;
            *high = hi;
            for (uint32_t ch = 0; ch < 128; ch++) {
                bool on = false;
                for (size_t k = 0; k < cs->n && !on; k++) {
                    uint32_t lo = cs->r[k].lo, h = cs->r[k].hi;
                    on = (ch >= lo && ch <= h) ||
                         (cs->fold && ((sea_utf8_lower(ch) >= lo && sea_utf8_lower(ch) <= h) ||
                                       (sea_utf8_upper(ch) >= lo && sea_utf8_upper(ch) <= h)));
                }
                if (cs->neg) on = !on;
                if (on) ascii[ch >> 5] |= 1u << (ch & 31);
            }
            break;
        }
    }
}

/* A split whose branches both start by reading a character, from sets with
 * nothing in common (\S+ followed by a space, [^"]*", \d+ before a dot):
 * at most one branch can get past the next character, so the backtracker
 * need not remember the other. Becomes I_SPLITP. */
static void mark_exclusive_splits(Code *c, const Parser *p) {
    for (size_t i = 0; i < c->n; i++) {
        if (c->inst[i].op != I_SPLIT) continue;
        int a = first_char_inst(c, c->inst[i].x), b = first_char_inst(c, c->inst[i].y);
        if (a < 0 || b < 0) continue;
        uint32_t sa[4], sb[4];
        bool ha, hb;
        char_set_of(&c->inst[a], p, sa, &ha);
        char_set_of(&c->inst[b], p, sb, &hb);
        bool overlap = ha && hb;
        for (int k = 0; k < 4; k++) if (sa[k] & sb[k]) overlap = true;
        if (!overlap) c->inst[i].op = I_SPLITP;
    }
}

/* The literal prefix every match starts with (for a quick skip), when the
 * pattern begins with case-sensitive literals. */
static size_t literal_prefix(Node *root, char *out, size_t cap) {
    size_t len = 0;
    Node *n = root;
    while (n && n->type == N_GROUP) n = n->kids[0];
    if (!n) return 0;
    if (n->type == N_LIT && !n->fold) {
        char b[4];
        int k = sea_utf8_encode(n->cp, b);
        if (k && (size_t)k <= cap) { memcpy(out, b, (size_t)k); return (size_t)k; }
        return 0;
    }
    if (n->type != N_CAT) return 0;
    for (size_t i = 0; i < n->n; i++) {
        Node *k = n->kids[i];
        if (k->type != N_LIT || k->fold) break;
        char b[4];
        int m = sea_utf8_encode(k->cp, b);
        if (!m || len + (size_t)m > cap) break;
        memcpy(out + len, b, (size_t)m);
        len += (size_t)m;
    }
    return len;
}

/* ---- The program blob ----------------------------------------------------------
 * uint32 words: magic, ninst, nclasses, nranges, ngroups, prefix_len,
 * insts (3 each), classes (3 each: range offset, count, flags), ranges (2
 * each), prefix bytes (padded). */

#define HDR 6

typedef struct {
    const uint32_t *w;
    uint32_t ninst, nclasses, nranges, ngroups, prefix_len;
    const int32_t *inst;          /* op, x, y */
    const uint32_t *klass;        /* off, count, flags */
    const uint32_t *ranges;       /* lo, hi */
    const uint32_t *ascii;        /* per class: 4 words, bit c set when c (< 128) is in it */
    const char *prefix;
} Prog;

#define CF_NEG  1u
#define CF_FOLD 2u

static SeaString *build_blob(Code *c, Parser *p, const char *prefix, size_t plen) {
    size_t nranges = 0;
    for (size_t i = 0; i < p->nsets; i++) nranges += p->sets[i].n;
    size_t words = HDR + c->n * 3 + p->nsets * 3 + nranges * 2 + p->nsets * 4 + (plen + 3) / 4;
    SeaString *s = sea_string_alloc(words * 4);
    uint32_t *w = (uint32_t *)(void *)s->data;
    memset(w, 0, words * 4);
    w[0] = RE_MAGIC;
    w[1] = (uint32_t)c->n;
    w[2] = (uint32_t)p->nsets;
    w[3] = (uint32_t)nranges;
    w[4] = (uint32_t)p->ngroups;
    w[5] = (uint32_t)plen;
    uint32_t *q = w + HDR;
    for (size_t i = 0; i < c->n; i++) {
        *q++ = (uint32_t)c->inst[i].op;
        *q++ = (uint32_t)c->inst[i].x;
        *q++ = (uint32_t)c->inst[i].y;
    }
    uint32_t off = 0;
    for (size_t i = 0; i < p->nsets; i++) {
        *q++ = off;
        *q++ = (uint32_t)p->sets[i].n;
        *q++ = (p->sets[i].neg ? CF_NEG : 0) | (p->sets[i].fold ? CF_FOLD : 0);
        off += (uint32_t)p->sets[i].n;
    }
    for (size_t i = 0; i < p->nsets; i++)
        for (size_t k = 0; k < p->sets[i].n; k++) {
            *q++ = p->sets[i].r[k].lo;
            *q++ = p->sets[i].r[k].hi;
        }
    /* the ASCII members of each class, negation and case folding applied:
     * one bit test per ASCII character at match time */
    for (size_t i = 0; i < p->nsets; i++) {
        const CharSet *cs = &p->sets[i];
        uint32_t bits[4] = {0, 0, 0, 0};
        for (uint32_t ch = 0; ch < 128; ch++) {
            uint32_t probes[3] = { ch, ch, ch };
            int np = 1;
            if (cs->fold) {
                probes[1] = sea_utf8_lower(ch);
                probes[2] = sea_utf8_upper(ch);
                np = 3;
            }
            bool in = false;
            for (int t = 0; t < np && !in; t++)
                for (size_t k = 0; k < cs->n && !in; k++)
                    if (probes[t] >= cs->r[k].lo && probes[t] <= cs->r[k].hi) in = true;
            if (cs->neg) in = !in;
            if (in) bits[ch >> 5] |= 1u << (ch & 31);
        }
        for (int k = 0; k < 4; k++) *q++ = bits[k];
    }
    memcpy(q, prefix, plen);
    s->data[words * 4] = '\0';
    return s;
}

/* The fields of a blob (one prog_check accepted) */
static bool prog_view(SeaString *s, Prog *pg) {
    const uint32_t *w = (const uint32_t *)(const void *)s->data;
    uint32_t ninst = w[1], ncl = w[2], nr = w[3];
    pg->w = w;
    pg->ninst = ninst;
    pg->nclasses = ncl;
    pg->nranges = nr;
    pg->ngroups = w[4];
    pg->prefix_len = w[5];
    pg->inst = (const int32_t *)(w + HDR);
    pg->klass = w + HDR + (size_t)ninst * 3;
    pg->ranges = pg->klass + (size_t)ncl * 3;
    pg->ascii = pg->ranges + (size_t)nr * 2;
    pg->prefix = (const char *)(pg->ascii + (size_t)ncl * 4);
    return true;
}

/* Check a blob's structure; false if it is not a program we built. */
static bool prog_check(SeaString *s, Prog *pg) {
    if (!s || s->len < HDR * 4 || s->len % 4) return false;
    const uint32_t *w = (const uint32_t *)(const void *)s->data;
    if (((uintptr_t)w & 3) || w[0] != RE_MAGIC) return false;
    uint64_t ninst = w[1], ncl = w[2], nr = w[3], ng = w[4], pl = w[5];
    if (ninst == 0 || ninst > RE_MAX_INST || ng > RE_MAX_GROUPS || pl > 64) return false;
    uint64_t words = HDR + ninst * 3 + ncl * 3 + nr * 2 + ncl * 4 + (pl + 3) / 4;
    if (words * 4 != s->len) return false;
    prog_view(s, pg);
    for (uint32_t i = 0; i < ncl; i++) {
        uint64_t off = pg->klass[i * 3], cnt = pg->klass[i * 3 + 1];
        if (off + cnt > nr) return false;
    }
    uint32_t ncap = 2 * (pg->ngroups + 1);
    for (uint32_t i = 0; i < pg->ninst; i++) {
        int32_t op = pg->inst[i * 3], x = pg->inst[i * 3 + 1], y = pg->inst[i * 3 + 2];
        switch (op) {
            case I_CHAR: case I_CHARF: if (x < 0 || x > 0x10FFFF) return false; break;
            case I_ANY: case I_ANYNL: case I_MATCH: case I_NOP: break;
            case I_CLASS: if (x < 0 || (uint32_t)x >= ncl) return false; break;
            case I_JMP: if (x < 0 || (uint32_t)x >= pg->ninst) return false; break;
            case I_SPLIT: case I_SPLITP:
                if (x < 0 || (uint32_t)x >= pg->ninst || y < 0 || (uint32_t)y >= pg->ninst) return false;
                break;
            case I_SAVE: if (x < 0 || (uint32_t)x >= ncap) return false; break;
            case I_ASSERT: if (x < A_BOL || x > A_NWORDB) return false; break;
            default: return false;
        }
        /* every instruction but the last must not fall off the end */
        if (i + 1 == pg->ninst && op != I_MATCH && op != I_JMP) return false;
    }
    return true;
}

/* prog_check, once per blob: a regex's __prog is checked on its first use
 * in a thread, then read in place. Strings are immutable, and the cache
 * holds a reference, so a blob seen here cannot change or be replaced at
 * the same address. */
#define CHECKED_MAX 16
static _Thread_local SeaString *checked[CHECKED_MAX];
static _Thread_local unsigned checked_next;

static bool prog_load(SeaString *s, Prog *pg) {
    for (int i = 0; i < CHECKED_MAX; i++)
        if (checked[i] == s) return prog_view(s, pg);
    if (!prog_check(s, pg)) return false;
    SeaString **slot = &checked[checked_next++ % CHECKED_MAX];
    if (*slot) sea_obj_release(&(*slot)->obj);
    sea_obj_retain(&s->obj);
    *slot = s;
    return true;
}

/* ======================================================================== */
/* Pike VM                                                                  */
/* ======================================================================== */

static bool class_has(const Prog *pg, uint32_t k, uint32_t cp) {
    if (cp < 128) return (pg->ascii[k * 4 + (cp >> 5)] >> (cp & 31)) & 1u;
    const uint32_t *r = pg->ranges + pg->klass[k * 3] * 2;
    uint32_t n = pg->klass[k * 3 + 1], flags = pg->klass[k * 3 + 2];
    uint32_t probes[3] = { cp, cp, cp };
    int np = 1;
    if (flags & CF_FOLD) {
        probes[1] = sea_utf8_lower(cp);
        probes[2] = sea_utf8_upper(cp);
        np = 3;
    }
    bool in = false;
    for (int t = 0; t < np && !in; t++) {
        uint32_t c = probes[t];
        size_t lo = 0, hi = n;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (c < r[mid * 2]) hi = mid;
            else if (c > r[mid * 2 + 1]) lo = mid + 1;
            else { in = true; break; }
        }
    }
    return (flags & CF_NEG) ? !in : in;
}

static bool is_word(int c) {
    return c >= 0 && c < 0x80 && (isalnum(c) || c == '_');
}

typedef struct {
    const Prog *pg;
    const unsigned char *s;
    size_t len;
    int ncap;                 /* capture slots tracked */
    /* thread lists */
    int *pcs[2];
    int *caps[2];             /* ncap per thread */
    int count[2];
    uint32_t *mark;           /* per pc: generation of the list it was last added to */
    uint32_t gen;             /* generation addthread marks with */
    uint32_t list_gen[2];     /* each list's generation */
    uint32_t gen_counter;
    /* addthread work stack: (pc) or (restore slot, value) */
    int *stack;
    size_t stack_cap;
    int *cur;                 /* working capture array */
    /* bounded backtracker (see bt_search) */
    uint64_t *bt_vis;         /* visited (pc, pos) bits */
    size_t bt_vis_words;
    int *bt_jobs;             /* (pc, pos) or (-(slot+1), value) pairs */
    size_t bt_jobs_cap;
    size_t bt_edge;           /* a path that reaches this position leaves the window */
    bool bt_hit_edge;
} VM;

/* Decode the character at i: its code point (an invalid byte: U+FFFD) and width. */
static inline uint32_t cp_at(const unsigned char *s, size_t len, size_t i, int *w) {
    if (s[i] < 0x80) { *w = 1; return s[i]; }
    uint32_t cp;
    int k = sea_utf8_decode(s + i, len - i, &cp);
    if (!k) { *w = 1; return 0xFFFD; }
    *w = k;
    return cp;
}

/* Whether the consuming instruction in accepts the character at pos (< len) */
static inline bool char_ok(const Prog *pg, const int32_t *in, const unsigned char *s, size_t len, size_t pos) {
    int w;
    uint32_t c = cp_at(s, len, pos, &w);
    switch (in[0]) {
        case I_CHAR:  return c == (uint32_t)in[1];
        case I_CHARF: return sea_utf8_lower(c) == (uint32_t)in[1];
        case I_ANY:   return c != '\n';
        case I_ANYNL: return true;
        case I_CLASS: return class_has(pg, (uint32_t)in[1], c);
        default:      return false;
    }
}

static bool assert_ok(VM *v, int kind, size_t pos) {
    switch (kind) {
        case A_BOT: return pos == 0;
        case A_EOT: return pos == v->len;
        case A_BOL: return pos == 0 || v->s[pos - 1] == '\n';
        case A_EOL: return pos == v->len || v->s[pos] == '\n';
        default: {
            bool a = pos > 0 && is_word(v->s[pos - 1]);
            bool b = pos < v->len && is_word(v->s[pos]);
            return kind == A_WORDB ? a != b : a == b;
        }
    }
}

static void push_stack(VM *v, size_t *sp, int a) {
    if (*sp >= v->stack_cap) {
        v->stack_cap *= 2;
        v->stack = realloc(v->stack, v->stack_cap * sizeof *v->stack);
    }
    v->stack[(*sp)++] = a;
}

/* Follow pc through jumps/splits/saves/asserts at pos, adding the threads
 * that consume input (or match) to list l, in priority order. caps is the
 * capture state the thread arrives with. */
static void addthread(VM *v, int l, int pc0, const int *caps, size_t pos) {
    memcpy(v->cur, caps, (size_t)v->ncap * sizeof *v->cur);
    size_t sp = 0;
    push_stack(v, &sp, pc0);
    while (sp > 0) {
        int a = v->stack[--sp];
        if (a < 0) {                    /* restore: -(slot+1), value below */
            int slot = -a - 1;
            v->cur[slot] = v->stack[--sp];
            continue;
        }
        int pc = a;
        if (v->mark[pc] == v->gen) continue;
        v->mark[pc] = v->gen;
        const int32_t *in = v->pg->inst + pc * 3;
        switch (in[0]) {
            case I_JMP: push_stack(v, &sp, in[1]); break;
            case I_SPLIT: case I_SPLITP:
                push_stack(v, &sp, in[2]);
                push_stack(v, &sp, in[1]);
                break;
            case I_NOP: push_stack(v, &sp, pc + 1); break;
            case I_SAVE:
                if (in[1] < v->ncap) {
                    push_stack(v, &sp, v->cur[in[1]]);
                    push_stack(v, &sp, -(in[1] + 1));
                    push_stack(v, &sp, pc + 1);
                    v->cur[in[1]] = (int)pos;
                } else {
                    push_stack(v, &sp, pc + 1);
                }
                break;
            case I_ASSERT:
                if (assert_ok(v, in[1], pos)) push_stack(v, &sp, pc + 1);
                break;
            default: {
                int k = v->count[l]++;
                v->pcs[l][k] = pc;
                memcpy(v->caps[l] + (size_t)k * v->ncap, v->cur, (size_t)v->ncap * sizeof *v->cur);
                break;
            }
        }
    }
}

/* ---- Bounded backtracker (RE2's BitState) ----
 * For short texts: a depth-first search in priority order (the first match
 * found is the leftmost-first one, as the Pike VM finds), never visiting a
 * (pc, position) pair twice — a pair that failed once fails again, whatever
 * the captures — so it stays linear: O(instructions × text). Used when that
 * product fits BT_MAX_BITS; it is several times faster than the Pike VM,
 * which copies capture arrays between thread lists at every character. */
#define BT_MAX_BITS (256 * 1024)

static void bt_push(VM *v, size_t *sp, int a, int b) {
    if (*sp + 2 > v->bt_jobs_cap) {
        v->bt_jobs_cap = v->bt_jobs_cap ? v->bt_jobs_cap * 2 : 256;
        v->bt_jobs = realloc(v->bt_jobs, v->bt_jobs_cap * sizeof *v->bt_jobs);
    }
    v->bt_jobs[(*sp)++] = a;
    v->bt_jobs[(*sp)++] = b;
}

/* I_SPLITP at pc, position *pos: 1 to take the first branch, 2 the second
 * (after reading a whole loop's run: *pos moves past it), 0 when the path
 * fails (a (pc, position) visited before), -1 at the window edge. */
static __attribute__((noinline)) int bt_splitp(VM *v, int pc, size_t *ppos, size_t base, size_t span) {
    const Prog *pg = v->pg;
    const int32_t *in = pg->inst + pc * 3;
    size_t pos = *ppos;
    int body = in[1];
    bool loop = (body == pc - 1 || (body == pc + 1 && pg->inst[(pc + 2) * 3] == I_JMP &&
                                    pg->inst[(pc + 2) * 3 + 1] == pc)) &&
                pg->inst[body * 3] <= I_CLASS;
    if (!loop) {
        int f = body;
        for (;;) {
            int op = pg->inst[f * 3];
            if (op == I_JMP) f = pg->inst[f * 3 + 1];
            else if (op == I_SAVE || op == I_NOP) f++;
            else break;
        }
        return pos < v->len && char_ok(pg, pg->inst + f * 3, v->s, v->len, pos) ? 1 : 2;
    }
    /* x+ or x*: read the whole run of x here, marking (pc, position)
     * visited as the loop would */
    const int32_t *bi = pg->inst + body * 3;
    const uint32_t *bm = bi[0] == I_CLASS ? pg->ascii + (size_t)bi[1] * 4 : NULL;
    for (;;) {
        if (pos >= v->len) break;
        unsigned ch = v->s[pos];
        int cw = 1;
        if (bm && ch < 128) {
            if (!((bm[ch >> 5] >> (ch & 31)) & 1u)) break;
        } else {
            if (!char_ok(pg, bi, v->s, v->len, pos)) break;
            cp_at(v->s, v->len, pos, &cw);
        }
        if (pos >= v->bt_edge) {
            v->bt_hit_edge = true;
            return -1;
        }
        pos += (size_t)cw;
        size_t bb = (size_t)pc * span + (pos - base);
        uint64_t bm2 = 1ull << (bb & 63);
        if (v->bt_vis[bb >> 6] & bm2) return 0;
        v->bt_vis[bb >> 6] |= bm2;
    }
    *ppos = pos;
    return 2;
}

/* One attempt from position s (base: the position bit 0 stands for) */
static bool bt_run(VM *v, size_t s, size_t base, size_t span, int *cap, int *out) {
    const Prog *pg = v->pg;
    size_t sp = 0;
    bt_push(v, &sp, 0, (int)s);
    while (sp > 0) {
        int b = v->bt_jobs[--sp];
        int a = v->bt_jobs[--sp];
        if (a < 0) {                       /* restore a capture slot */
            cap[-a - 1] = b;
            continue;
        }
        int pc = a;
        size_t pos = (size_t)b;
        for (;;) {
            size_t bit = (size_t)pc * span + (pos - base);
            uint64_t m = 1ull << (bit & 63);
            if (v->bt_vis[bit >> 6] & m) break;
            v->bt_vis[bit >> 6] |= m;
            const int32_t *in = pg->inst + pc * 3;
            int w = 0;
            uint32_t c = 0;
            bool at_end = pos >= v->len;
            if (pos >= v->bt_edge && in[0] <= I_CLASS) {
                /* this path reads past the window: let the Pike VM decide */
                v->bt_hit_edge = true;
                return false;
            }
            switch (in[0]) {
                case I_MATCH:
                    memcpy(out, cap, (size_t)v->ncap * sizeof *out);
                    return true;
                case I_CHAR:
                    if (at_end) goto fail;
                    c = cp_at(v->s, v->len, pos, &w);
                    if (c != (uint32_t)in[1]) goto fail;
                    pos += (size_t)w; pc++; continue;
                case I_CHARF:
                    if (at_end) goto fail;
                    c = cp_at(v->s, v->len, pos, &w);
                    if (sea_utf8_lower(c) != (uint32_t)in[1]) goto fail;
                    pos += (size_t)w; pc++; continue;
                case I_ANY:
                    if (at_end) goto fail;
                    c = cp_at(v->s, v->len, pos, &w);
                    if (c == '\n') goto fail;
                    pos += (size_t)w; pc++; continue;
                case I_ANYNL:
                    if (at_end) goto fail;
                    cp_at(v->s, v->len, pos, &w);
                    pos += (size_t)w; pc++; continue;
                case I_CLASS:
                    if (at_end) goto fail;
                    if (v->s[pos] < 128) {
                        unsigned ch = v->s[pos];
                        if (!((pg->ascii[(size_t)in[1] * 4 + (ch >> 5)] >> (ch & 31)) & 1u)) goto fail;
                        pos++; pc++; continue;
                    }
                    c = cp_at(v->s, v->len, pos, &w);
                    if (!class_has(pg, (uint32_t)in[1], c)) goto fail;
                    pos += (size_t)w; pc++; continue;
                case I_JMP: pc = in[1]; continue;
                case I_SPLIT:
                    bt_push(v, &sp, in[2], (int)pos);   /* the lower-priority branch, for later */
                    pc = in[1];
                    continue;
                case I_SPLITP:
                    /* only the branch whose first character matches can go on */
                    switch (bt_splitp(v, pc, &pos, base, span)) {
                        case 0: goto fail;
                        case 1: pc = in[1]; continue;
                        case 2: pc = in[2]; continue;
                        default: return false;   /* reached the window edge */
                    }
                case I_NOP: pc++; continue;
                case I_SAVE:
                    if (in[1] < v->ncap) {
                        bt_push(v, &sp, -(in[1] + 1), cap[in[1]]);
                        cap[in[1]] = (int)pos;
                    }
                    pc++;
                    continue;
                case I_ASSERT:
                    if (!assert_ok(v, in[1], pos)) goto fail;
                    pc++;
                    continue;
                default: goto fail;
            }
        }
    fail:;
    }
    return false;
}

static bool pike_search(VM *v, size_t start, int *out);
static void pike_alloc(VM *v);

/* The backtracker over windows of the text: each window's bitmap holds
 * BT_MAX_BITS bits, so long texts (findall, gsub over a file) are searched a
 * window at a time. A path that would read past its window hands the search
 * from that start over to the Pike VM. */
static bool bt_search(VM *v, size_t start, int *out) {
    const Prog *pg = v->pg;
    size_t Wmax = BT_MAX_BITS / pg->ninst;     /* positions per window, at most */
    if (Wmax < 2) return pike_search(v, start, out);
    /* small windows first (clearing the bitmap is then cheap: findall over a
     * big text runs one search per match); a path reaching the edge retries
     * from its start with a window 4 times larger */
    size_t W = Wmax < 256 ? Wmax : 256;
    bool anchored = pg->inst[0] == I_SAVE && pg->ninst > 1 && pg->inst[3] == I_ASSERT && pg->inst[4] == A_BOT;
    int *cap = v->cur;
    size_t s = start;
    for (;;) {
        size_t wend = v->len - s + 1 <= W ? v->len : s + W - 1;   /* positions s..wend */
        size_t span = wend - s + 1;
        size_t words = ((size_t)pg->ninst * span + 63) / 64;
        if (words > v->bt_vis_words) {
            free(v->bt_vis);
            v->bt_vis = malloc(words * sizeof *v->bt_vis);
            v->bt_vis_words = words;
        }
        memset(v->bt_vis, 0, words * sizeof *v->bt_vis);
        v->bt_edge = wend < v->len ? wend : (size_t)-1;
        v->bt_hit_edge = false;
        size_t t = s;
        for (;;) {
            if (anchored && t != 0) return false;
            if (pg->prefix_len && !anchored) {
                const void *hit = t <= v->len ? memmem(v->s + t, v->len - t, pg->prefix, pg->prefix_len) : NULL;
                if (!hit) return false;
                t = (size_t)((const unsigned char *)hit - v->s);
                if (t >= wend && wend < v->len) break;      /* in a later window */
            }
            for (int i = 0; i < v->ncap; i++) cap[i] = -1;
            if (bt_run(v, t, s, span, cap, out)) return true;
            if (v->bt_hit_edge) {
                if (W >= Wmax) return pike_search(v, t, out);
                W = W * 4 < Wmax ? W * 4 : Wmax;
                break;   /* the starts before t failed for good: a larger window from t */
            }
            if (t >= v->len) return false;
            int w;
            cp_at(v->s, v->len, t, &w);
            t += (size_t)w;
            if (t >= wend && wend < v->len) break;
        }
        s = t;   /* the next window starts where this one's starts ended */
    }
}

/* Search s from byte start; true with caps[0..ncap) filled on a match. */
static bool vm_search(VM *v, size_t start, int *out) {
    /* SEA_RE_PIKE=1 forces the Pike VM (to test the backtracker against it) */
    static int force_pike = -1;
    if (force_pike < 0) force_pike = getenv("SEA_RE_PIKE") != NULL;
    if (!force_pike && start <= v->len && v->len < INT32_MAX) return bt_search(v, start, out);
    return pike_search(v, start, out);
}

static bool pike_search(VM *v, size_t start, int *out) {
    const Prog *pg = v->pg;
    pike_alloc(v);
    int *empty = v->cur + v->ncap;            /* a capture array of -1s */
    for (int i = 0; i < v->ncap; i++) empty[i] = -1;
    int cl = 0, nl = 1;
    v->count[0] = v->count[1] = 0;
    v->list_gen[cl] = ++v->gen_counter;
    bool matched = false;
    bool anchored = pg->inst[0] == I_SAVE && pg->ninst > 1 && pg->inst[3] == I_ASSERT && pg->inst[4] == A_BOT;
    size_t pos = start;
    for (;;) {
        if (!matched && !(anchored && pos != 0)) {
            if (v->count[cl] == 0 && pg->prefix_len && !anchored) {
                /* nothing in flight: jump to the next place the prefix occurs */
                const void *hit = pos <= v->len ? memmem(v->s + pos, v->len - pos, pg->prefix, pg->prefix_len) : NULL;
                if (!hit) break;
                pos = (size_t)((const unsigned char *)hit - v->s);
            }
            /* same generation as the threads already in this list, so a pc
             * they hold is not added twice */
            if (v->count[cl] == 0) v->list_gen[cl] = ++v->gen_counter;   /* a fresh list */
            v->gen = v->list_gen[cl];
            addthread(v, cl, 0, empty, pos);
        }
        if (v->count[cl] == 0) {
            /* nothing in flight here: on to the next position, unless done */
            if (matched || anchored || pos >= v->len) break;
            int w0;
            cp_at(v->s, v->len, pos, &w0);
            pos += (size_t)w0;
            continue;
        }
        int w = 0;
        uint32_t c = pos < v->len ? cp_at(v->s, v->len, pos, &w) : 0;
        bool at_end = pos >= v->len;
        v->gen = v->list_gen[nl] = ++v->gen_counter;
        v->count[nl] = 0;
        for (int t = 0; t < v->count[cl]; t++) {
            int pc = v->pcs[cl][t];
            int *tc = v->caps[cl] + (size_t)t * v->ncap;
            const int32_t *in = pg->inst + pc * 3;
            bool ok = false;
            switch (in[0]) {
                case I_MATCH:
                    memcpy(out, tc, (size_t)v->ncap * sizeof *out);
                    matched = true;
                    t = v->count[cl];             /* drop lower-priority threads */
                    continue;
                case I_CHAR:  ok = !at_end && c == (uint32_t)in[1]; break;
                case I_CHARF: ok = !at_end && sea_utf8_lower(c) == (uint32_t)in[1]; break;
                case I_ANY:   ok = !at_end && c != '\n'; break;
                case I_ANYNL: ok = !at_end; break;
                case I_CLASS: ok = !at_end && class_has(pg, (uint32_t)in[1], c); break;
                default: break;
            }
            if (ok) addthread(v, nl, pc + 1, tc, pos + (size_t)w);
        }
        int tmp = cl; cl = nl; nl = tmp;
        if (at_end) {
            /* threads in the new list can only be MATCH (nothing consumes at the end) */
            for (int t = 0; t < v->count[cl]; t++) {
                if (pg->inst[v->pcs[cl][t] * 3] == I_MATCH) {
                    memcpy(out, v->caps[cl] + (size_t)t * v->ncap, (size_t)v->ncap * sizeof *out);
                    matched = true;
                    break;
                }
            }
            break;
        }
        pos += (size_t)w;
    }
    return matched;
}

/* The backtracker's buffers outlive a search: one set per thread, taken by
 * vm_init and given back by vm_free (a search nested in a gsub callback
 * finds the slot empty and allocates its own) */
static _Thread_local struct { uint64_t *vis; size_t vis_words; int *jobs; size_t jobs_cap; } bt_spare;

static void vm_init(VM *v, const Prog *pg, const char *s, size_t len, int ncap) {
    memset(v, 0, sizeof *v);
    v->pg = pg;
    v->s = (const unsigned char *)s;
    v->len = len;
    v->ncap = ncap;
    v->cur = malloc((size_t)ncap * 2 * sizeof *v->cur + sizeof(int));
    v->bt_vis = bt_spare.vis;
    v->bt_vis_words = bt_spare.vis_words;
    v->bt_jobs = bt_spare.jobs;
    v->bt_jobs_cap = bt_spare.jobs_cap;
    bt_spare.vis = NULL;
    bt_spare.jobs = NULL;
    bt_spare.vis_words = bt_spare.jobs_cap = 0;
    /* the Pike VM's thread lists are allocated when a search needs them
     * (short texts use the backtracker) */
}

static void pike_alloc(VM *v) {
    if (v->pcs[0]) return;
    size_t n = v->pg->ninst;
    for (int i = 0; i < 2; i++) {
        v->pcs[i] = malloc(n * sizeof(int));
        v->caps[i] = malloc(n * (size_t)v->ncap * sizeof(int) + sizeof(int));
    }
    v->mark = calloc(n, sizeof *v->mark);
    v->stack_cap = 64;
    v->stack = malloc(v->stack_cap * sizeof *v->stack);
}

static void vm_free(VM *v) {
    for (int i = 0; i < 2; i++) { free(v->pcs[i]); free(v->caps[i]); }
    free(v->mark);
    free(v->stack);
    free(v->cur);
    /* keep buffers up to 64 KB each for the next search */
    if (!bt_spare.vis && v->bt_vis_words <= 8192) {
        bt_spare.vis = v->bt_vis;
        bt_spare.vis_words = v->bt_vis_words;
    } else {
        free(v->bt_vis);
    }
    if (!bt_spare.jobs && v->bt_jobs_cap <= 16384) {
        bt_spare.jobs = v->bt_jobs;
        bt_spare.jobs_cap = v->bt_jobs_cap;
    } else {
        free(v->bt_jobs);
    }
}

/* ======================================================================== */
/* The module                                                               */
/* ======================================================================== */

/* A compiled pattern: the program blob plus group names (borrowed from the
 * regex table, or owned when compiled for one call). */
typedef struct {
    SeaString *blob;             /* +1 */
    Prog       pg;
    SeaValue   names;            /* +1: table name → index, or nil */
} Rx;

static void rx_free(Rx *r) {
    if (r->blob) sea_obj_release(&r->blob->obj);
    if (sea_is_obj(r->names)) sea_obj_release(sea_as_obj(r->names));
}

static bool parse_flag_string(const char *f, Parser *p) {
    for (; *f; f++) {
        switch (*f) {
            case 'i': p->fold = true; break;
            case 'm': p->multiline = true; break;
            case 's': p->dotnl = true; break;
            case 'U': p->ungreedy = true; break;
            default: return false;
        }
    }
    return true;
}

/* Compile pattern (with flags) into r; NULL, or an error message in msg. */
static bool compile_pattern(const char *pat, size_t plen, const char *flags, Rx *r, char *msg, size_t msglen) {
    Parser p;
    memset(&p, 0, sizeof p);
    p.s = pat;
    p.len = plen;
    if (flags && !parse_flag_string(flags, &p)) {
        snprintf(msg, msglen, "re: unknown flag in \"%s\" (use i, m, s, U)", flags);
        return false;
    }
    Node *root = parse_alt(&p);
    if (root && more(&p)) {
        root = NULL;
        perr(&p, peekc(&p) == ')' ? "unexpected )" : "unexpected character");
    }
    if (!root) {
        snprintf(msg, msglen, "re: %s at position %zu in /%.*s/", p.err ? p.err : "invalid pattern",
                 p.err_pos + 1, plen > 80 ? 80 : (int)plen, pat);
        parser_free(&p);
        return false;
    }
    Code c = { 0 };
    emit(&c, I_SAVE, 0, 0);
    compile_node(&c, root);
    emit(&c, I_SAVE, 1, 0);
    emit(&c, I_MATCH, 0, 0);
    if (c.overflow) {
        snprintf(msg, msglen, "re: pattern too large (more than %d instructions)", RE_MAX_INST);
        free(c.inst);
        parser_free(&p);
        return false;
    }
    mark_exclusive_splits(&c, &p);
    char prefix[64];
    size_t pl = literal_prefix(root, prefix, sizeof prefix);
    r->blob = build_blob(&c, &p, prefix, pl);
    free(c.inst);
    r->names = SEA_NIL_VAL;
    bool any_name = false;
    for (int i = 0; i < p.ngroups; i++) if (p.names[i]) any_name = true;
    if (any_name) {
        SeaTable *t = sea_table_new(TABLE_MAP);
        for (int i = 0; i < p.ngroups; i++)
            if (p.names[i]) sea_table_set_str(t, p.names[i], sea_int_val(i + 1));
        r->names = sea_obj_val(&t->obj);
    }
    parser_free(&p);
    prog_load(r->blob, &r->pg);
    return true;
}

static SeaValue new_str(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }

static void tset(SeaTable *t, const char *k, SeaValue v) {
    sea_table_set_str(t, k, v);
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

/* The keys of regex and match tables, interned once (sea_table_set_str
 * would intern them on every call, under the intern table's lock) */
enum { K_PROG, K_NAMES, K_TEXT, K_START, K_STOP, K_GROUPS, K_SPANS, K_NAMED, K_COUNT };
static const char *const KEY_NAMES[K_COUNT] = { "__prog", "names", "text", "start", "stop", "groups", "spans", "named" };
static SeaValue KEY[K_COUNT];

static bool table_field(SeaValue t, int k, SeaValue *out) {
    *out = SEA_NIL_VAL;
    if (sea_is_table(t)) return sea_table_get(SEA_AS_TABLE(t), KEY[k], out);
    return false;
}

/* t[KEY[k]] = v, taking over v */
static void kset(SeaTable *t, int k, SeaValue v) { sea_table_set_move(t, KEY[k], v); }

/* The pattern argument: a string (compiled now) or a regex from re.compile. */
/* Patterns given as strings are compiled once per thread and kept in a
 * small cache (as Python's re does), so re.gsub("\\d+", ...) in a loop does
 * not recompile. The key is a private copy of the pattern's bytes; the blob
 * and names are this thread's objects. */
#define RX_CACHE 32
typedef struct {
    char      *pat;      /* malloc'd copy, NULL when the slot is empty */
    size_t     len;
    SeaString *blob;     /* +1 */
    SeaValue   names;    /* +1 or nil */
} RxCacheEnt;
static _Thread_local RxCacheEnt rx_cache[RX_CACHE];
static _Thread_local unsigned rx_cache_next;

static bool rx_cache_get(const char *pat, size_t len, Rx *r) {
    for (int i = 0; i < RX_CACHE; i++) {
        RxCacheEnt *e = &rx_cache[i];
        if (e->pat && e->len == len && memcmp(e->pat, pat, len) == 0) {
            r->blob = e->blob;
            sea_obj_retain(&r->blob->obj);
            r->names = e->names;
            if (sea_is_obj(r->names)) sea_obj_retain(sea_as_obj(r->names));
            prog_load(r->blob, &r->pg);
            return true;
        }
    }
    return false;
}

static void rx_cache_put(const char *pat, size_t len, const Rx *r) {
    RxCacheEnt *e = &rx_cache[rx_cache_next++ % RX_CACHE];
    if (e->pat) {
        free(e->pat);
        sea_obj_release(&e->blob->obj);
        if (sea_is_obj(e->names)) sea_obj_release(sea_as_obj(e->names));
    }
    e->pat = malloc(len + 1);
    memcpy(e->pat, pat, len);
    e->len = len;
    e->blob = r->blob;
    sea_obj_retain(&e->blob->obj);
    e->names = r->names;
    if (sea_is_obj(e->names)) sea_obj_retain(sea_as_obj(e->names));
}

static void get_rx(SeaVM *vm, const char *fn, int argc, SeaValue *args, Rx *r) {
    memset(r, 0, sizeof *r);
    r->names = SEA_NIL_VAL;
    if (argc < 1) sea_vm_error_noreturn(vm, "re.%s: pattern expected", fn);
    SeaValue blob;
    if (sea_is_table(args[0]) && table_field(args[0], K_PROG, &blob)) {
        if (!sea_is_string(blob) || !prog_load(SEA_AS_STRING(blob), &r->pg))
            sea_vm_error_noreturn(vm, "re.%s: not a compiled regex (its __prog was changed)", fn);
        r->blob = SEA_AS_STRING(blob);
        sea_obj_retain(&r->blob->obj);
        SeaValue names;
        if (table_field(args[0], K_NAMES, &names) && sea_is_table(names)) {
            r->names = names;
            sea_obj_retain(sea_as_obj(names));
        }
        return;
    }
    SeaString *pat = sea_value_get_flat_string(args[0]);
    if (!pat) sea_vm_error_noreturn(vm, "re.%s: pattern (string or regex) expected, got %s", fn, sea_type_name(args[0]));
    if (rx_cache_get(pat->data, pat->len, r)) {
        sea_obj_release(&pat->obj);
        return;
    }
    char msg[256];
    bool ok = compile_pattern(pat->data, pat->len, NULL, r, msg, sizeof msg);
    if (ok) rx_cache_put(pat->data, pat->len, r);
    sea_obj_release(&pat->obj);
    if (!ok) sea_vm_error_noreturn(vm, "%s", msg);
}

static SeaString *subject_arg(SeaVM *vm, const char *fn, Rx *r, int argc, SeaValue *args, int i) {
    SeaString *s = argc > i ? sea_value_get_flat_string(args[i]) : NULL;
    if (!s) {
        rx_free(r);
        sea_vm_error_noreturn(vm, "re.%s: string expected, got %s", fn, argc > i ? sea_type_name(args[i]) : "nothing");
    }
    return s;
}

static int64_t int_opt(int argc, SeaValue *args, int i, int64_t def) {
    if (argc <= i || sea_is_nil(args[i])) return def;
    if (sea_is_int(args[i])) return sea_as_int(args[i]);
    if (sea_is_number(args[i])) return (int64_t)sea_as_number(args[i]);
    return def;
}

/* A match table from capture offsets. */
static SeaValue make_match(Rx *r, SeaString *s, const int *caps) {
    int ng = (int)r->pg.ngroups;
    SeaTable *m = sea_table_new(TABLE_MAP);
    SeaArray *groups = sea_array_new((uint32_t)ng + 1);
    SeaArray *spans = sea_array_new((uint32_t)ng + 1);
    for (int g = 0; g <= ng; g++) {
        int a = caps[2 * g], b = caps[2 * g + 1];
        if (a >= 0 && b >= a) {
            SeaValue t = new_str(s->data + a, (size_t)(b - a));
            sea_array_push(groups, t);
            if (g == 0) sea_table_set(m, KEY[K_TEXT], t);   /* the same string as groups[0] (retained) */
            sea_obj_release(sea_as_obj(t));
            SeaArray *sp = sea_array_new(2);   /* [start, stop] */
            sea_array_push(sp, sea_int_val(a + 1));
            sea_array_push(sp, sea_int_val(b));
            sea_array_push_move(spans, sea_obj_val(&sp->obj));
        } else {
            sea_array_push(groups, SEA_NIL_VAL);
            sea_array_push(spans, SEA_NIL_VAL);
        }
    }
    kset(m, K_START, sea_int_val(caps[0] + 1));
    kset(m, K_STOP, sea_int_val(caps[1]));
    kset(m, K_GROUPS, sea_obj_val(&groups->obj));
    kset(m, K_SPANS, sea_obj_val(&spans->obj));
    SeaTable *named = sea_table_new(TABLE_MAP);
    if (sea_is_table(r->names)) {
        uint32_t it = 0;
        SeaValue k, idx;
        while (sea_table_next(SEA_AS_TABLE(r->names), &it, &k, &idx)) {
            if (!sea_is_int(idx)) continue;
            int g = (int)sea_as_int(idx);
            if (g < 1 || g > ng) continue;
            int a = caps[2 * g], b = caps[2 * g + 1];
            if (a >= 0 && b >= a) {
                SeaValue t = new_str(s->data + a, (size_t)(b - a));
                sea_table_set(named, k, t);
                sea_obj_release(sea_as_obj(t));
            }
        }
    }
    kset(m, K_NAMED, sea_obj_val(&named->obj));
    return sea_obj_val(&m->obj);
}

/* Iterate successive non-overlapping matches (an empty match right after the
 * previous one is skipped, as in Go). cb returns false to stop. */
typedef bool (*MatchFn)(void *ctx, const int *caps);

static void each_match(VM *v, int64_t limit, MatchFn cb, void *ctx) {
    int *caps = malloc((size_t)v->ncap * sizeof *caps);
    size_t pos = 0;
    int64_t count = 0;
    long prev_end = -1;
    while (pos <= v->len && (limit < 0 || count < limit)) {
        if (!vm_search(v, pos, caps)) break;
        size_t a = (size_t)caps[0], b = (size_t)caps[1];
        bool skip = a == b && (long)a == prev_end;
        if (!skip) {
            count++;
            if (!cb(ctx, caps)) break;
            prev_end = (long)b;
        }
        if (b > a) pos = b;
        else {
            if (a >= v->len) break;
            int w;
            cp_at(v->s, v->len, a, &w);
            pos = a + (size_t)w;
        }
    }
    free(caps);
}

/* ---- re.compile -------------------------------------------------------------- */

static SeaValue re_test(SeaVM *vm, int argc, SeaValue *args);
static SeaValue re_match(SeaVM *vm, int argc, SeaValue *args);
static SeaValue re_captures(SeaVM *vm, int argc, SeaValue *args);
static SeaValue re_all(SeaVM *vm, int argc, SeaValue *args);
static SeaValue re_findall(SeaVM *vm, int argc, SeaValue *args);
static SeaValue re_each(SeaVM *vm, int argc, SeaValue *args);
static SeaValue re_gsub(SeaVM *vm, int argc, SeaValue *args);
static SeaValue re_split(SeaVM *vm, int argc, SeaValue *args);

static SeaValue re_compile(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *pat = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!pat) sea_vm_error_noreturn(vm, "re.compile: pattern (string) expected, got %s", argc > 0 ? sea_type_name(args[0]) : "nothing");
    SeaString *fl = argc > 1 && !sea_is_nil(args[1]) ? sea_value_get_flat_string(args[1]) : NULL;
    Rx r;
    memset(&r, 0, sizeof r);
    char msg[256];
    bool ok = compile_pattern(pat->data, pat->len, fl ? fl->data : NULL, &r, msg, sizeof msg);
    SeaValue result;
    if (!ok) {
        result = sea_make_error_str(vm, msg);
    } else {
        SeaTable *t = sea_table_new(TABLE_MAP);
        sea_table_set_str(t, "pattern", sea_obj_val(&pat->obj));
        tset(t, "flags", fl ? sea_val_ref(sea_obj_val(&fl->obj)) : new_str("", 0));
        tset(t, "groups", sea_int_val(r.pg.ngroups));
        if (sea_is_table(r.names)) sea_table_set_str(t, "names", r.names);
        else tset(t, "names", sea_obj_val(&sea_table_new(TABLE_MAP)->obj));
        sea_table_set_str(t, "__prog", sea_obj_val(&r.blob->obj));
        mod_set(vm, t, "test",    re_test);
        mod_set(vm, t, "match",   re_match);
        mod_set(vm, t, "captures", re_captures);
        mod_set(vm, t, "all",     re_all);
        mod_set(vm, t, "findall", re_findall);
        mod_set(vm, t, "each",    re_each);
        mod_set(vm, t, "gsub",    re_gsub);
        mod_set(vm, t, "split",   re_split);
        result = sea_obj_val(&t->obj);
        rx_free(&r);
    }
    if (fl) sea_obj_release(&fl->obj);
    sea_obj_release(&pat->obj);
    return result;
}

/* ---- matching ------------------------------------------------------------------ */

static SeaValue re_test(SeaVM *vm, int argc, SeaValue *args) {
    Rx r;
    get_rx(vm, "test", argc, args, &r);
    SeaString *s = subject_arg(vm, "test", &r, argc, args, 1);
    VM v;
    vm_init(&v, &r.pg, s->data, s->len, 2);
    int caps[2];
    bool ok = vm_search(&v, 0, caps);
    vm_free(&v);
    sea_obj_release(&s->obj);
    rx_free(&r);
    return sea_bool_val(ok);
}

/* re.match(p, s [, init]): the first match at or after byte init (1-based) */
static SeaValue re_match(SeaVM *vm, int argc, SeaValue *args) {
    Rx r;
    get_rx(vm, "match", argc, args, &r);
    SeaString *s = subject_arg(vm, "match", &r, argc, args, 1);
    int64_t init = int_opt(argc, args, 2, 1);
    if (init < 0) init += (int64_t)s->len + 1;
    if (init < 1) init = 1;
    SeaValue result = SEA_NIL_VAL;
    if ((size_t)(init - 1) <= s->len) {
        int ncap = 2 * ((int)r.pg.ngroups + 1);
        VM v;
        vm_init(&v, &r.pg, s->data, s->len, ncap);
        int *caps = malloc((size_t)ncap * sizeof *caps);
        if (vm_search(&v, (size_t)(init - 1), caps)) result = make_match(&r, s, caps);
        free(caps);
        vm_free(&v);
    }
    sea_obj_release(&s->obj);
    rx_free(&r);
    return result;
}

/* re.captures(p, s [, init]): just the strings of the first match, as an
 * array [whole, group 1, ...] (nil for a group that did not take part), or
 * nil: re.match without the table (Go's FindStringSubmatch) */
static SeaValue re_captures(SeaVM *vm, int argc, SeaValue *args) {
    Rx r;
    get_rx(vm, "captures", argc, args, &r);
    SeaString *s = subject_arg(vm, "captures", &r, argc, args, 1);
    int64_t init = int_opt(argc, args, 2, 1);
    if (init < 0) init += (int64_t)s->len + 1;
    if (init < 1) init = 1;
    SeaValue result = SEA_NIL_VAL;
    if ((size_t)(init - 1) <= s->len) {
        int ng = (int)r.pg.ngroups, ncap = 2 * (ng + 1);
        VM v;
        vm_init(&v, &r.pg, s->data, s->len, ncap);
        int *caps = malloc((size_t)ncap * sizeof *caps);
        if (vm_search(&v, (size_t)(init - 1), caps)) {
            SeaArray *out = sea_array_new((uint32_t)ng + 1);
            for (int g = 0; g <= ng; g++) {
                int a = caps[2 * g], b = caps[2 * g + 1];
                sea_array_push_move(out, a >= 0 && b >= a ? new_str(s->data + a, (size_t)(b - a)) : SEA_NIL_VAL);
            }
            result = sea_obj_val(&out->obj);
        }
        free(caps);
        vm_free(&v);
    }
    sea_obj_release(&s->obj);
    rx_free(&r);
    return result;
}

typedef struct { Rx *r; SeaString *s; SeaArray *out; bool tables; } AllCtx;

static bool all_cb(void *ctx, const int *caps) {
    AllCtx *a = ctx;
    SeaValue v = a->tables ? make_match(a->r, a->s, caps) : new_str(a->s->data + caps[0], (size_t)(caps[1] - caps[0]));
    sea_array_push(a->out, v);
    sea_obj_release(sea_as_obj(v));
    return true;
}

static SeaValue all_impl(SeaVM *vm, const char *fn, int argc, SeaValue *args, bool tables) {
    Rx r;
    get_rx(vm, fn, argc, args, &r);
    SeaString *s = subject_arg(vm, fn, &r, argc, args, 1);
    int64_t limit = int_opt(argc, args, 2, -1);
    int ncap = tables ? 2 * ((int)r.pg.ngroups + 1) : 2;
    VM v;
    vm_init(&v, &r.pg, s->data, s->len, ncap);
    AllCtx ctx = { &r, s, sea_array_new(4), tables };
    each_match(&v, limit, all_cb, &ctx);
    vm_free(&v);
    sea_obj_release(&s->obj);
    rx_free(&r);
    return sea_obj_val(&ctx.out->obj);
}

static SeaValue re_all(SeaVM *vm, int argc, SeaValue *args) { return all_impl(vm, "all", argc, args, true); }
static SeaValue re_findall(SeaVM *vm, int argc, SeaValue *args) { return all_impl(vm, "findall", argc, args, false); }

/* re.each(p, s, fn [, n]): fn(match) for each successive match (as in re.all), one at a
 * time: no array of every match is built. fn returning false stops; the result is how many
 * matches fn saw. */
typedef struct { SeaVM *vm; Rx *r; SeaString *s; SeaValue fn; int64_t count; } EachCtx;

static bool each_cb(void *ctx, const int *caps) {
    EachCtx *e = ctx;
    SeaValue m = make_match(e->r, e->s, caps);
    e->count++;
    SeaValue ret = sea_vm_call(e->vm, e->fn, &m, 1);
    sea_obj_release(sea_as_obj(m));
    bool halt = sea_is_bool(ret) && !sea_as_bool(ret);
    if (sea_is_obj(ret)) sea_obj_release(sea_as_obj(ret));
    return !halt && !e->vm->panic_propagating;
}

static SeaValue re_each(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 3 || !sea_is_obj(args[2]))
        return sea_make_error_str(vm, "re.each: a function expected as the third argument");
    Rx r;
    get_rx(vm, "each", argc, args, &r);
    SeaString *s = subject_arg(vm, "each", &r, argc, args, 1);
    int64_t limit = int_opt(argc, args, 3, -1);
    VM v;
    vm_init(&v, &r.pg, s->data, s->len, 2 * ((int)r.pg.ngroups + 1));
    EachCtx ctx = { vm, &r, s, args[2], 0 };
    each_match(&v, limit, each_cb, &ctx);
    vm_free(&v);
    sea_obj_release(&s->obj);
    rx_free(&r);
    return sea_int_val(ctx.count);
}

/* ---- gsub ---------------------------------------------------------------------- */

typedef struct { char *d; size_t len, cap; } Out;

static void out_push(Out *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 64;
        while (nc < o->len + n + 1) nc *= 2;
        o->d = realloc(o->d, nc);
        o->cap = nc;
    }
    memcpy(o->d + o->len, s, n);
    o->len += n;
}

typedef struct {
    SeaVM     *vm;
    Rx        *r;
    SeaString *s;
    SeaValue   repl;
    SeaString *tmpl;          /* string replacement */
    Out        out;
    size_t     last;          /* end of the previous match */
    bool       failed;
    char       err[160];
} GsubCtx;

/* Expand $0 $1 ${12} ${name} $$ in the template. */
static bool expand(GsubCtx *g, const int *caps) {
    const char *t = g->tmpl->data;
    size_t n = g->tmpl->len;
    int ng = (int)g->r->pg.ngroups;
    for (size_t i = 0; i < n; i++) {
        if (t[i] != '$') { out_push(&g->out, t + i, 1); continue; }
        if (i + 1 < n && t[i + 1] == '$') { out_push(&g->out, "$", 1); i++; continue; }
        int grp = -1;
        size_t j = i + 1;
        if (j < n && t[j] == '{') {
            size_t e = j + 1;
            while (e < n && t[e] != '}') e++;
            if (e >= n) { snprintf(g->err, sizeof g->err, "re.gsub: unclosed ${ in the replacement"); return false; }
            const char *nm = t + j + 1;
            size_t nl = e - j - 1;
            bool digits = nl > 0;
            for (size_t k = 0; k < nl; k++) if (!isdigit((unsigned char)nm[k])) digits = false;
            if (digits) grp = atoi(nm);
            else {
                SeaValue idx = SEA_NIL_VAL;
                char name[64];
                snprintf(name, sizeof name, "%.*s", (int)(nl < 63 ? nl : 63), nm);
                if (sea_is_table(g->r->names)) sea_table_get_str(SEA_AS_TABLE(g->r->names), name, &idx);
                if (!sea_is_int(idx)) { snprintf(g->err, sizeof g->err, "re.gsub: unknown group name '%s' in the replacement", name); return false; }
                grp = (int)sea_as_int(idx);
            }
            i = e;
        } else if (j < n && isdigit((unsigned char)t[j])) {
            grp = 0;
            while (j < n && isdigit((unsigned char)t[j]) && grp < 100000) grp = grp * 10 + (t[j++] - '0');
            i = j - 1;
        } else {
            out_push(&g->out, "$", 1);
            continue;
        }
        if (grp > ng) { snprintf(g->err, sizeof g->err, "re.gsub: no group %d in the pattern", grp); return false; }
        int a = caps[2 * grp], b = caps[2 * grp + 1];
        if (a >= 0 && b >= a) out_push(&g->out, g->s->data + a, (size_t)(b - a));
    }
    return true;
}

static bool gsub_cb(void *ctx, const int *caps) {
    GsubCtx *g = ctx;
    out_push(&g->out, g->s->data + g->last, (size_t)caps[0] - g->last);
    g->last = (size_t)caps[1];
    const char *whole = g->s->data + caps[0];
    size_t wlen = (size_t)(caps[1] - caps[0]);
    if (g->tmpl) {
        if (!expand(g, caps)) { g->failed = true; return false; }
        return true;
    }
    SeaValue rep = SEA_NIL_VAL;
    bool owned = false;
    if (sea_is_table(g->repl) || sea_is_map(g->repl) || sea_is_struct(g->repl)) {
        SeaValue key = new_str(whole, wlen);
        if (sea_is_table(g->repl)) sea_table_get(SEA_AS_TABLE(g->repl), key, &rep);
        else if (sea_is_map(g->repl)) sea_map_get(SEA_AS_MAP(g->repl), key, &rep);
        else {
            SeaString *k = sea_string_find_intern(whole, wlen);   /* struct fields are interned */
            if (k) {
                rep = sea_struct_get(SEA_AS_STRUCT(g->repl), k);
                sea_obj_release(&k->obj);
            }
        }
        sea_obj_release(sea_as_obj(key));
    } else {
        SeaValue m = make_match(g->r, g->s, caps);
        rep = sea_vm_call(g->vm, g->repl, &m, 1);
        sea_obj_release(sea_as_obj(m));
        owned = true;
        if (g->vm->panic_propagating) {
            if (sea_is_obj(rep)) sea_obj_release(sea_as_obj(rep));
            g->failed = true;
            g->err[0] = '\0';
            return false;
        }
    }
    if (sea_is_nil(rep) || (sea_is_bool(rep) && !sea_as_bool(rep))) {
        out_push(&g->out, whole, wlen);                /* keep the match */
    } else {
        SeaString *rs = sea_is_string(rep) || sea_is_rope(rep) ? sea_value_get_flat_string(rep) : sea_vm_tostring(g->vm, rep);
        out_push(&g->out, rs->data, rs->len);
        sea_obj_release(&rs->obj);
    }
    if (owned && sea_is_obj(rep)) sea_obj_release(sea_as_obj(rep));
    return true;
}

/* re.gsub(p, s, repl [, n]): repl is a string with $1 / ${name} / $0 / $$,
 * a function(match) → string (nil or false keeps the match), or a table
 * mapping the matched text to its replacement */
static SeaValue re_gsub(SeaVM *vm, int argc, SeaValue *args) {
    Rx r;
    get_rx(vm, "gsub", argc, args, &r);
    SeaString *s = subject_arg(vm, "gsub", &r, argc, args, 1);
    if (argc < 3) {
        sea_obj_release(&s->obj);
        rx_free(&r);
        sea_vm_error_noreturn(vm, "re.gsub: replacement expected");
    }
    GsubCtx g;
    memset(&g, 0, sizeof g);
    g.vm = vm;
    g.r = &r;
    g.s = s;
    g.repl = args[2];
    if (sea_is_string(args[2]) || sea_is_rope(args[2])) g.tmpl = sea_value_get_flat_string(args[2]);
    else if (!sea_is_table(args[2]) && !sea_is_map(args[2]) && !sea_is_struct(args[2]) &&
             !sea_is_function(args[2]) && !sea_is_closure(args[2])) {
        sea_obj_release(&s->obj);
        rx_free(&r);
        sea_vm_error_noreturn(vm, "re.gsub: replacement must be a string, function or table, got %s", sea_type_name(args[2]));
    }
    int64_t limit = int_opt(argc, args, 3, -1);
    VM v;
    vm_init(&v, &r.pg, s->data, s->len, 2 * ((int)r.pg.ngroups + 1));
    each_match(&v, limit, gsub_cb, &g);
    vm_free(&v);
    SeaValue result = SEA_NIL_VAL;
    if (!g.failed) {
        out_push(&g.out, s->data + g.last, s->len - g.last);
        result = new_str(g.out.d, g.out.len);
    }
    free(g.out.d);
    if (g.tmpl) sea_obj_release(&g.tmpl->obj);
    sea_obj_release(&s->obj);
    rx_free(&r);
    if (g.failed && g.err[0]) sea_vm_error_noreturn(vm, "%s", g.err);
    return result;
}

/* ---- split --------------------------------------------------------------------- */

typedef struct { SeaString *s; SeaArray *out; size_t beg, end; int64_t n; } SplitCtx;

static bool split_cb(void *ctx, const int *caps) {
    SplitCtx *c = ctx;
    if (c->n > 0 && (int64_t)sea_array_len(c->out) == c->n - 1) return false;
    c->end = (size_t)caps[0];
    if (caps[1] != 0) {
        SeaValue part = new_str(c->s->data + c->beg, c->end - c->beg);
        sea_array_push(c->out, part);
        sea_obj_release(sea_as_obj(part));
    }
    c->beg = (size_t)caps[1];
    return true;
}

static void push_piece(SeaArray *a, const char *s, size_t n) {
    SeaValue v = new_str(s, n);
    sea_array_push(a, v);
    sea_obj_release(sea_as_obj(v));
}

/* re.split(p, s [, n]): the pieces between matches (n > 0: at most n
 * pieces; n = 0: none), as Go's regexp.Split */
static SeaValue re_split(SeaVM *vm, int argc, SeaValue *args) {
    Rx r;
    get_rx(vm, "split", argc, args, &r);
    SeaString *s = subject_arg(vm, "split", &r, argc, args, 1);
    int64_t n = int_opt(argc, args, 2, -1);
    SplitCtx c = { s, sea_array_new(4), 0, 0, n };
    bool empty_pattern = r.pg.ninst == 3;              /* SAVE 0, SAVE 1, MATCH */
    if (n != 0) {
        if (s->len == 0 && !empty_pattern) {
            push_piece(c.out, "", 0);
        } else {
            VM v;
            vm_init(&v, &r.pg, s->data, s->len, 2);
            each_match(&v, n, split_cb, &c);
            vm_free(&v);
            if (c.end != s->len) push_piece(c.out, s->data + c.beg, s->len - c.beg);
        }
    }
    sea_obj_release(&s->obj);
    rx_free(&r);
    return sea_obj_val(&c.out->obj);
}

/* re.escape(s): s with every regex metacharacter escaped */
static SeaValue re_escape(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) sea_vm_error_noreturn(vm, "re.escape: string expected, got %s", argc > 0 ? sea_type_name(args[0]) : "nothing");
    Out o = { 0 };
    for (size_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        if (strchr("\\.+*?()|[]{}^$", c) && c) out_push(&o, "\\", 1);
        out_push(&o, &c, 1);
    }
    SeaValue r = new_str(o.d ? o.d : "", o.len);
    free(o.d);
    sea_obj_release(&s->obj);
    return r;
}

void sea_lib_re_register(SeaVM *vm) {
    if (KEY[0] == 0)
        for (int k = 0; k < K_COUNT; k++) {
            /* borrowed: the intern table keeps interned strings until exit */
            SeaString *key = sea_string_intern(KEY_NAMES[k], strlen(KEY_NAMES[k]));
            KEY[k] = sea_obj_val(&key->obj);
            sea_obj_release(&key->obj);
        }
    SeaTable *mod = sea_table_new(TABLE_MAP);
    mod_set(vm, mod, "compile", re_compile);
    mod_set(vm, mod, "test",    re_test);
    mod_set(vm, mod, "match",   re_match);
    mod_set(vm, mod, "captures", re_captures);
    mod_set(vm, mod, "all",     re_all);
    mod_set(vm, mod, "findall", re_findall);
    mod_set(vm, mod, "each", re_each);
    mod_set(vm, mod, "gsub",    re_gsub);
    mod_set(vm, mod, "split",   re_split);
    mod_set(vm, mod, "escape",  re_escape);
    sea_vm_register_module(vm, "re", mod);
    sea_obj_release(&mod->obj);
}
