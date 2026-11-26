/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_ast.h"
#include "sea_memory.h"

SeaNode* sea_node_alloc(SeaNodeType type, int line) {
    SeaNode *n = SEA_ALLOC(SeaNode);
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->line = line;
    return n;
}

static void free_node_array(SeaNode **arr, int count) {
    if (!arr) return;
    for (int i = 0; i < count; i++) {
        sea_node_free(arr[i]);
    }
    sea_free(arr, sizeof(SeaNode*) * (size_t)count);
}

void sea_node_free(SeaNode *n) {
    if (!n) return;

    switch (n->type) {
        case NODE_STRING:
            if (n->string.ptr) sea_free(n->string.ptr, n->string.len + 1);
            break;
        case NODE_IDENT:
            if (n->ident) sea_free(n->ident, strlen(n->ident) + 1);
            break;

        case NODE_UNOP:
            sea_node_free(n->unop.expr);
            break;
        case NODE_BINOP:
            sea_node_free(n->binop.left);
            sea_node_free(n->binop.right);
            break;
        case NODE_INDEX:
            sea_node_free(n->index.obj);
            sea_node_free(n->index.key);
            break;
        case NODE_FIELD:
            sea_node_free(n->field.obj);
            if (n->field.field) sea_free(n->field.field, strlen(n->field.field)+1);
            break;
        case NODE_CALL:
            sea_node_free(n->call.fn);
            free_node_array(n->call.args, n->call.argc);
            break;
        case NODE_METHOD_CALL:
            sea_node_free(n->method.obj);
            if (n->method.method) sea_free(n->method.method, strlen(n->method.method)+1);
            free_node_array(n->method.args, n->method.argc);
            break;
        case NODE_FUNC:
            if (n->func.params) {
                for (int i = 0; i < n->func.paramc; i++) {
                    if (n->func.params[i].name)
                        sea_free(n->func.params[i].name, strlen(n->func.params[i].name)+1);
                }
                sea_free(n->func.params, sizeof(SeaParam) * (size_t)n->func.paramc);
            }
            sea_node_free(n->func.body);
            if (n->func.name) sea_free(n->func.name, strlen(n->func.name)+1);
            break;
        case NODE_TABLE:
            if (n->table.keys) {
                for (int i = 0; i < n->table.count; i++) {
                    sea_node_free(n->table.keys[i]);
                }
                sea_free(n->table.keys, sizeof(SeaNode*) * (size_t)n->table.count);
            }
            free_node_array(n->table.vals, n->table.count);
            break;
        case NODE_MAP_LIT:
            free_node_array(n->array_lit.elems, n->array_lit.count);
            break;
        case NODE_TEMPLATE:
            for (int i = 0; i <= n->tmpl.count; i++) {
                sea_free(n->tmpl.texts[i], n->tmpl.lens[i] + 1);
                sea_free(n->tmpl.raws[i], strlen(n->tmpl.raws[i]) + 1);
            }
            sea_free(n->tmpl.texts, sizeof(char*) * (size_t)(n->tmpl.count + 1));
            sea_free(n->tmpl.lens, sizeof(size_t) * (size_t)(n->tmpl.count + 1));
            sea_free(n->tmpl.raws, sizeof(char*) * (size_t)(n->tmpl.count + 1));
            free_node_array(n->tmpl.exprs, n->tmpl.count);
            break;
        case NODE_ARRAY_LIT:
            free_node_array(n->array_lit.elems, n->array_lit.count);
            break;
        case NODE_BLOCK:
            free_node_array(n->block.stmts, n->block.count);
            break;
        case NODE_LOCAL:
            if (n->local.names) {
                for (int i = 0; i < n->local.count; i++) {
                    if (n->local.names[i])
                        sea_free(n->local.names[i], strlen(n->local.names[i])+1);
                }
                sea_free(n->local.names, sizeof(char*) * (size_t)n->local.count);
            }
            free_node_array(n->local.exprs, n->local.expr_count);
            break;
        case NODE_ASSIGN:
            free_node_array(n->assign.targets, n->assign.count);
            free_node_array(n->assign.exprs, n->assign.expr_count);
            break;
        case NODE_IF:
            sea_node_free(n->if_stmt.cond);
            sea_node_free(n->if_stmt.then_body);
            sea_node_free(n->if_stmt.else_body);
            break;
        case NODE_WHILE:
            sea_node_free(n->while_loop.cond);
            sea_node_free(n->while_loop.body);
            break;
        case NODE_FOR_NUM:
            if (n->for_num.var) sea_free(n->for_num.var, strlen(n->for_num.var)+1);
            sea_node_free(n->for_num.start);
            sea_node_free(n->for_num.limit);
            sea_node_free(n->for_num.step);
            sea_node_free(n->for_num.body);
            break;
        case NODE_FOR_IN:
            if (n->for_in.vars) {
                for (int i = 0; i < n->for_in.var_count; i++) {
                    if (n->for_in.vars[i])
                        sea_free(n->for_in.vars[i], strlen(n->for_in.vars[i])+1);
                }
                sea_free(n->for_in.vars, sizeof(char*) * (size_t)n->for_in.var_count);
            }
            sea_node_free(n->for_in.iter_expr);
            sea_node_free(n->for_in.body);
            break;
        case NODE_RETURN:
            free_node_array(n->ret.exprs, n->ret.count);
            break;
        case NODE_GO:
            sea_node_free(n->go_call);
            break;
        case NODE_SELECT:
            if (n->select.cases) {
                for (int i = 0; i < n->select.count; i++) {
                    sea_node_free(n->select.cases[i].stmt);
                    sea_node_free(n->select.cases[i].body);
                }
                sea_free(n->select.cases, sizeof(SeaSelectCase) * (size_t)n->select.count);
            }
            break;
        case NODE_DEFER:
            sea_node_free(n->defer_stmt);
            break;
        case NODE_PANIC:
            sea_node_free(n->panic_expr);
            break;
        case NODE_DEL:
            sea_node_free(n->del_stmt);
            break;
        case NODE_CHAN_RECV:
            sea_node_free(n->unop.expr);
            break;
        case NODE_CHAN_SEND:
            sea_node_free(n->binop.left);
            sea_node_free(n->binop.right);
            break;
        case NODE_PROPAGATE:
            sea_node_free(n->propagate);
            break;
        case NODE_EXPR_STMT:
            sea_node_free(n->expr_stmt);
            break;
        default:
            break;
    }

    SEA_FREE(n, SeaNode);
}
