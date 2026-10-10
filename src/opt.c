/* opt.c - constant folding + dead code after unconditional jumps. */
#include "opt.h"
#include "parser.h"
#include "sema.h"
#include <string.h>

static int g_folded = 0;

static Node *fold_expr(Node *n);

static Node *fold_expr(Node *n) {
    if (!n)
        return n;
    switch (n->kind) {
    case ND_BINARY:
        n->lhs = fold_expr(n->lhs);
        n->rhs = fold_expr(n->rhs);
        if (n->lhs->kind == ND_NUM && n->rhs->kind == ND_NUM) {
            /* Avoid folding division/modulo by zero: leave for runtime
             * trap instead of compile-time crash. */
            if ((n->op == '/' || n->op == '%') && n->rhs->ival == 0)
                return n;
            if ((n->op == 'L' || n->op == 'R') &&
                (n->rhs->ival < 0 || n->rhs->ival > 63))
                return n;
            int ok = 0;
            long long v = sema_eval_const(n, &ok);
            if (ok) {
                n->kind = ND_NUM;
                n->ival = v;
                n->lhs = n->rhs = NULL;
                g_folded++;
            }
        }
        return n;
    case ND_UNARY:
        n->lhs = fold_expr(n->lhs);
        if ((n->op == '-' || n->op == '+' || n->op == '~' || n->op == '!') &&
            n->lhs->kind == ND_NUM) {
            int ok = 0;
            long long v = sema_eval_const(n, &ok);
            if (ok) {
                n->kind = ND_NUM;
                n->ival = v;
                n->lhs = NULL;
                g_folded++;
            }
        }
        return n;
    case ND_TERNARY:
        n->cond = fold_expr(n->cond);
        n->then = fold_expr(n->then);
        n->els = fold_expr(n->els);
        if (n->cond->kind == ND_NUM) {
            Node *pick = n->cond->ival ? n->then : n->els;
            g_folded++;
            return pick;
        }
        return n;
    case ND_SIZEOF: {
        int ok = 0;
        long long v = sema_eval_const(n, &ok);
        if (ok) {
            n->kind = ND_NUM;
            n->ival = v;
            n->lhs = NULL;
            g_folded++;
        } else if (n->lhs) {
            n->lhs = fold_expr(n->lhs);
        }
        return n;
    }
    case ND_CAST:
        n->lhs = fold_expr(n->lhs);
        if (n->lhs->kind == ND_NUM) {
            int ok = 0;
            long long v = sema_eval_const(n, &ok);
            if (ok) {
                n->kind = ND_NUM;
                n->ival = v;
                n->lhs = NULL;
                g_folded++;
            }
        }
        return n;
    case ND_ASSIGN:
    case ND_COMPOUND:
        n->lhs = fold_expr(n->lhs);
        n->rhs = fold_expr(n->rhs);
        return n;
    case ND_COMMA:
        n->lhs = fold_expr(n->lhs);
        n->rhs = fold_expr(n->rhs);
        return n;
    case ND_CALL:
        n->lhs = fold_expr(n->lhs);
        for (int i = 0; i < n->nargs; i++)
            n->args[i] = fold_expr(n->args[i]);
        return n;
    case ND_MEMBER:
    case ND_SUBSCRIPT:
        n->lhs = fold_expr(n->lhs);
        if (n->rhs)
            n->rhs = fold_expr(n->rhs);
        return n;
    case ND_INIT_LIST:
        for (int i = 0; i < n->nstmts; i++)
            n->stmts[i] = fold_expr(n->stmts[i]);
        return n;
    default:
        return n;
    }
}

static int is_unconditional(Node *n) {
    return n->kind == ND_RETURN || n->kind == ND_BREAK ||
           n->kind == ND_CONTINUE || n->kind == ND_GOTO;
}

static int is_barrier(Node *n) {
    return n->kind == ND_LABEL || n->kind == ND_CASE;
}

/* Remove stmts after unconditional jumps (until a label/case). */
static void dce_block(Node *blk) {
    if (!blk || blk->kind != ND_BLOCK)
        return;
    int w = 0;
    int dead = 0;
    for (int i = 0; i < blk->nstmts; i++) {
        Node *s = blk->stmts[i];
        if (dead) {
            if (is_barrier(s)) {
                dead = 0;
                blk->stmts[w++] = s;
            } else {
                /* drop: ponytail — no resurrection path needed. */
                continue;
            }
        } else {
            blk->stmts[w++] = s;
        }
        if (!dead && is_unconditional(s))
            dead = 1;
    }
    blk->nstmts = w;
}

static void opt_stmt(Node *n);

static void opt_stmt(Node *n) {
    if (!n)
        return;
    switch (n->kind) {
    case ND_BLOCK:
        for (int i = 0; i < n->nstmts; i++)
            opt_stmt(n->stmts[i]);
        dce_block(n);
        break;
    case ND_IF:
        n->cond = fold_expr(n->cond);
        /* `if (0)` / `if (1)` simplify to the live branch. */
        if (n->cond->kind == ND_NUM) {
            Node *pick = n->cond->ival ? n->then : n->els;
            if (pick) {
                opt_stmt(pick);
                *n = *pick;
            } else {
                n->kind = ND_NOP;
                n->cond = n->then = n->els = NULL;
            }
            break;
        }
        opt_stmt(n->then);
        if (n->els)
            opt_stmt(n->els);
        break;
    case ND_WHILE:
        n->cond = fold_expr(n->cond);
        opt_stmt(n->body);
        break;
    case ND_DO:
        opt_stmt(n->body);
        n->cond = fold_expr(n->cond);
        break;
    case ND_FOR:
        if (n->init)
            opt_stmt(n->init);
        if (n->cond)
            n->cond = fold_expr(n->cond);
        if (n->inc)
            n->inc = fold_expr(n->inc);
        opt_stmt(n->body);
        break;
    case ND_RETURN:
        if (n->lhs)
            n->lhs = fold_expr(n->lhs);
        break;
    case ND_EXPR_STMT:
        if (n->lhs)
            n->lhs = fold_expr(n->lhs);
        break;
    case ND_SWITCH:
        n->cond = fold_expr(n->cond);
        opt_stmt(n->body);
        break;
    case ND_CASE:
        if (n->lhs)
            n->lhs = fold_expr(n->lhs);
        for (int i = 0; i < n->nstmts; i++)
            opt_stmt(n->stmts[i]);
        dce_block(n);
        break;
    case ND_LABEL:
        opt_stmt(n->body);
        break;
    case ND_VARDECL:
        if (n->decl && n->decl->init)
            n->decl->init = fold_expr(n->decl->init);
        break;
    default:
        break;
    }
}

void opt_run(void) {
    Node **prog = parser_prog_list();
    int n = parser_prog_count();
    for (int i = 0; i < n; i++) {
        Node *t = prog[i];
        if (t->kind == ND_FUNCDEF) {
            opt_stmt(t->body);
        } else if (t->kind == ND_VARDECL && t->decl && t->decl->init) {
            t->decl->init = fold_expr(t->decl->init);
        }
    }
    (void)g_folded;
}
