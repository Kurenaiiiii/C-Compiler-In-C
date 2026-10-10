/* ast.c - node constructors and --dump-ast printer. */
#include "ast.h"
#include <string.h>

Node *new_node(NodeKind kind, Loc loc) {
    Node *n = (Node *)arena_calloc(g_arena, 1, sizeof(Node));
    n->kind = kind;
    n->loc = loc;
    n->ty = NULL;
    return n;
}

void block_push(Node *block, Node *stmt) {
    if (block->nstmts == block->cap_stmts) {
        int ncap = block->cap_stmts ? block->cap_stmts * 2 : 8;
        Node **nl =
            (Node **)arena_alloc(g_arena, (size_t)ncap * sizeof(Node *));
        if (block->stmts)
            memcpy(nl, block->stmts, (size_t)block->nstmts * sizeof(Node *));
        block->stmts = nl;
        block->cap_stmts = ncap;
    }
    block->stmts[block->nstmts++] = stmt;
}

void call_push_arg(Node *call, Node *arg) {
    Node **nl = (Node **)arena_alloc(g_arena,
                                     (size_t)(call->nargs + 1) * sizeof(Node *));
    if (call->args)
        memcpy(nl, call->args, (size_t)call->nargs * sizeof(Node *));
    nl[call->nargs++] = arg;
    call->args = nl;
}

void initlist_push(Node *list, Node *e) {
    block_push(list, e);
}

static void dump_type(FILE *fp, Type *t) {
    if (!t) {
        fprintf(fp, "?");
        return;
    }
    switch (t->kind) {
    case TY_VOID: fprintf(fp, "void"); break;
    case TY_BOOL: fprintf(fp, "_Bool"); break;
    case TY_CHAR: fprintf(fp, "char"); break;
    case TY_UCHAR: fprintf(fp, "uchar"); break;
    case TY_SHORT: fprintf(fp, "short"); break;
    case TY_USHORT: fprintf(fp, "ushort"); break;
    case TY_INT: fprintf(fp, "int"); break;
    case TY_UINT: fprintf(fp, "uint"); break;
    case TY_LONG: fprintf(fp, "long"); break;
    case TY_ULONG: fprintf(fp, "ulong"); break;
    case TY_PTR: fprintf(fp, "(ptr "); dump_type(fp, t->base); fprintf(fp, ")"); break;
    case TY_ARRAY:
        fprintf(fp, "(array %d ", t->array_len);
        dump_type(fp, t->base);
        fprintf(fp, ")");
        break;
    case TY_STRUCT:
        fprintf(fp, "(struct %s)", t->tag ? t->tag : "?");
        break;
    case TY_UNION: fprintf(fp, "(union %s)", t->tag ? t->tag : "?"); break;
    case TY_ENUM: fprintf(fp, "enum"); break;
    case TY_FUNC: fprintf(fp, "(func "); dump_type(fp, t->ret); fprintf(fp, ")"); break;
    }
}

static void dump_node(FILE *fp, Node *n, int d) {
    for (int i = 0; i < d; i++)
        fprintf(fp, "  ");
    if (!n) {
        fprintf(fp, "(null)\n");
        return;
    }
    switch (n->kind) {
    case ND_NUM:
        fprintf(fp, "(num %lld ", n->ival);
        dump_type(fp, n->ty);
        fprintf(fp, ")\n");
        break;
    case ND_STR:
        fprintf(fp, "(str len=%zu)\n", n->slen);
        break;
    case ND_VAR:
        fprintf(fp, "(var %s ", n->decl ? n->decl->name : (n->name ? n->name : "?"));
        dump_type(fp, n->ty);
        fprintf(fp, ")\n");
        break;
    case ND_CALL:
        fprintf(fp, "(call\n");
        dump_node(fp, n->lhs, d + 1);
        for (int i = 0; i < n->nargs; i++)
            dump_node(fp, n->args[i], d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_FUNCDEF:
        fprintf(fp, "(funcdef %s\n", n->decl ? n->decl->name : "?");
        dump_node(fp, n->body, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_VARDECL:
        fprintf(fp, "(vardecl %s ", n->decl ? n->decl->name : "?");
        dump_type(fp, n->decl ? n->decl->type : NULL);
        fprintf(fp, "\n");
        if (n->decl && n->decl->init)
            dump_node(fp, n->decl->init, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_BLOCK:
        fprintf(fp, "(block\n");
        for (int i = 0; i < n->nstmts; i++)
            dump_node(fp, n->stmts[i], d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_EXPR_STMT:
        fprintf(fp, "(expr-stmt\n");
        if (n->lhs)
            dump_node(fp, n->lhs, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_IF:
        fprintf(fp, "(if\n");
        dump_node(fp, n->cond, d + 1);
        dump_node(fp, n->then, d + 1);
        if (n->els)
            dump_node(fp, n->els, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_WHILE:
    case ND_DO:
        fprintf(fp, "(%s\n", n->kind == ND_WHILE ? "while" : "do");
        dump_node(fp, n->cond, d + 1);
        dump_node(fp, n->body, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_FOR:
        fprintf(fp, "(for\n");
        if (n->init)
            dump_node(fp, n->init, d + 1);
        if (n->cond)
            dump_node(fp, n->cond, d + 1);
        if (n->inc)
            dump_node(fp, n->inc, d + 1);
        dump_node(fp, n->body, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_RETURN:
        fprintf(fp, "(return\n");
        if (n->lhs)
            dump_node(fp, n->lhs, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_BREAK: fprintf(fp, "(break)\n"); break;
    case ND_CONTINUE: fprintf(fp, "(continue)\n"); break;
    case ND_SWITCH:
        fprintf(fp, "(switch\n");
        dump_node(fp, n->cond, d + 1);
        dump_node(fp, n->body, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_CASE:
        if (n->is_default)
            fprintf(fp, "(default\n");
        else
            fprintf(fp, "(case %lld\n", n->ival);
        for (int i = 0; i < n->nstmts; i++)
            dump_node(fp, n->stmts[i], d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_LABEL:
        fprintf(fp, "(label %s\n", n->name ? n->name : "?");
        if (n->body)
            dump_node(fp, n->body, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_GOTO: fprintf(fp, "(goto %s)\n", n->name ? n->name : "?"); break;
    case ND_ASSIGN:
        fprintf(fp, "(assign\n");
        dump_node(fp, n->lhs, d + 1);
        dump_node(fp, n->rhs, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_COMPOUND:
        fprintf(fp, "(compound-assign %c\n", n->op);
        dump_node(fp, n->lhs, d + 1);
        dump_node(fp, n->rhs, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_TERNARY:
        fprintf(fp, "(ternary\n");
        dump_node(fp, n->cond, d + 1);
        dump_node(fp, n->then, d + 1);
        dump_node(fp, n->els, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_COMMA:
        fprintf(fp, "(comma\n");
        dump_node(fp, n->lhs, d + 1);
        dump_node(fp, n->rhs, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_UNARY:
        fprintf(fp, "(unary %c\n", n->op);
        dump_node(fp, n->lhs, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_BINARY:
        fprintf(fp, "(binary %c\n", n->op);
        dump_node(fp, n->lhs, d + 1);
        dump_node(fp, n->rhs, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_CAST:
        fprintf(fp, "(cast ");
        dump_type(fp, n->sz_type);
        fprintf(fp, "\n");
        dump_node(fp, n->lhs, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_SIZEOF:
        fprintf(fp, "(sizeof ");
        if (n->sz_type)
            dump_type(fp, n->sz_type);
        else
            fprintf(fp, "expr");
        fprintf(fp, "\n");
        if (n->lhs)
            dump_node(fp, n->lhs, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_MEMBER:
        fprintf(fp, "(member %s%s\n", n->is_arrow ? "->" : ".",
                n->name ? n->name : "?");
        dump_node(fp, n->lhs, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_SUBSCRIPT:
        fprintf(fp, "(subscript\n");
        dump_node(fp, n->lhs, d + 1);
        dump_node(fp, n->rhs, d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_INIT_LIST:
        fprintf(fp, "(init-list\n");
        for (int i = 0; i < n->nstmts; i++)
            dump_node(fp, n->stmts[i], d + 1);
        for (int i = 0; i < d; i++)
            fprintf(fp, "  ");
        fprintf(fp, ")\n");
        break;
    case ND_NOP: fprintf(fp, "(nop)\n"); break;
    }
}

void dump_ast(FILE *fp) {
    extern Node **parser_prog_list(void);
    extern int parser_prog_count(void);
    Node **list = parser_prog_list();
    int n = parser_prog_count();
    for (int i = 0; i < n; i++)
        dump_node(fp, list[i], 0);
}
