/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_lexer.h"
#include "sea_ast.h"

/*
 * Recursive descent parser for Sea.
 * Produces a SeaNode* AST from source code.
 */

typedef struct {
    SeaLexer  lexer;
    char     *err_msg;       // first error, without its position
    int       err_line;      // position of that error (1-based)
    int       err_col;
    bool      had_error;
    int       loop_depth;    // for break/continue validation
    int       func_depth;    // for return validation
    int       depth;         // expression/block nesting (limited)
    int       last_line;     // line of the last consumed token (for same-line check)
    int       last_tk;       // type of the last consumed token (for hints)
} SeaParser;

// Initialize parser with source code
void     sea_parser_init(SeaParser *p, const char *source);

// Free parser resources
void     sea_parser_free(SeaParser *p);

// Parse full program (returns block node or NULL on error)
SeaNode* sea_parser_parse(SeaParser *p);

// The error after a failed parse, as "name:line:col: message"
void     sea_parser_error_string(const SeaParser *p, const char *name,
                                 char *buf, size_t size);
