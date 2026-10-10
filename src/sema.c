/* sema.c - name resolution and type checking.
 *
 * Parser scopes are discarded after parsing, so sema rebuilds its own scope
 * chain by walking declarations in order. Every expression node leaves with
 * a concrete `ty` and resolved `decl`/`member` links.
 */
#include "sema.h"
#include <string.h>

typedef struct SemaScope SemaScope;
struct SemaScope {
    Decl *vars;
    SemaScope *parent;
};

typedef struct {
    SemaScope *scope;
    SemaScope *gscope;
    Type *cur_ret;      /* current function return type */
    int loop_depth;
    int switch_depth;
    /* goto/label tracking per function */
    char **labels;
    int nlabels, caplabels;
    char **gotos;
    Loc *goto_locs;
    int ngotos, capgotos;
    Decl *cur_fn;
} Sema;

static int is_lvalue(Node *n) {
    if (!n)
        return 0;
    switch (n->kind) {
    case ND_VAR:
        return n->decl && !n->decl->is_enumconst;
    case ND_UNARY:
        return n->op == '*';
    case ND_MEMBER:
    case ND_SUBSCRIPT:
        return 1;
    default:
        return 0;
    }
}

/* Array/function -> pointer decay for rvalues. */
static Type *decay(Type *t) {
    if (!t)
        return t;
    if (t->kind == TY_ARRAY)
        return ptr_to(t->base);
    if (t->kind == TY_FUNC)
        return ptr_to(t);
    return t;
}

static void sscope_add(SemaScope *s, Decl *d) {
    d->next = s->vars;
    s->vars = d;
}

static Decl *sscope_find(SemaScope *s, const char *name) {
    for (; s; s = s->parent) {
        for (Decl *d = s->vars; d; d = d->next) {
            if (d->name && strcmp(d->name, name) == 0)
                return d;
        }
    }
    return NULL;
}

static void add_label(Sema *s, const char *name) {
    for (int i = 0; i < s->nlabels; i++) {
        if (strcmp(s->labels[i], name) == 0)
            return;
    }
    if (s->nlabels == s->caplabels) {
        int ncap = s->caplabels ? s->caplabels * 2 : 8;
        char **nl =
            (char **)arena_alloc(g_arena, (size_t)ncap * sizeof(char *));
        for (int i = 0; i < s->nlabels; i++)
            nl[i] = s->labels[i];
        s->labels = nl;
        s->caplabels = ncap;
    }
    s->labels[s->nlabels++] = (char *)name;
}

static void add_goto(Sema *s, const char *name, Loc loc) {
    if (s->ngotos == s->capgotos) {
        int ncap = s->capgotos ? s->capgotos * 2 : 8;
        char **ng = (char **)arena_alloc(g_arena, (size_t)ncap * sizeof(char *));
        Loc *ll = (Loc *)arena_alloc(g_arena, (size_t)ncap * sizeof(Loc));
        for (int i = 0; i < s->ngotos; i++) {
            ng[i] = s->gotos[i];
            ll[i] = s->goto_locs[i];
        }
        s->gotos = ng;
        s->goto_locs = ll;
        s->capgotos = ncap;
    }
    s->gotos[s->ngotos] = (char *)name;
    s->goto_locs[s->ngotos] = loc;
    s->ngotos++;
}

/* Forward. */
static Type *visit_expr(Sema *s, Node *n);
static void visit_stmt(Sema *s, Node *n);
static void visit_block(Sema *s, Node *n);

long long sema_eval_const(Node *n, int *ok) {
    *ok = 0;
    if (!n)
        return 0;
    switch (n->kind) {
    case ND_NUM:
        *ok = 1;
        return n->ival;
    case ND_VAR:
        if (n->decl && n->decl->is_enumconst) {
            *ok = 1;
            return n->decl->enum_val;
        }
        return 0;
    case ND_UNARY: {
        if (n->op == '&') {
            /* address constant: not numeric; caller handles specially. */
            return 0;
        }
        int o = 0;
        long long v = sema_eval_const(n->lhs, &o);
        if (!o)
            return 0;
        *ok = 1;
        switch (n->op) {
        case '-': return -v;
        case '+': return v;
        case '~': return ~v;
        case '!': return !v;
        default: *ok = 0; return 0;
        }
    }
    case ND_BINARY: {
        int a = 0, b = 0;
        long long l = sema_eval_const(n->lhs, &a);
        long long r = sema_eval_const(n->rhs, &b);
        if (!a || !b)
            return 0;
        *ok = 1;
        switch (n->op) {
        case '+': return l + r;
        case '-': return l - r;
        case '*': return l * r;
        case '/':
            if (!r) {
                *ok = 0;
                return 0;
            }
            return l / r;
        case '%':
            if (!r) {
                *ok = 0;
                return 0;
            }
            return l % r;
        case '&': return l & r;
        case '|': return l | r;
        case '^': return l ^ r;
        case 'L': return l << r;
        case 'R': return l >> r;
        case 'E': return l == r;
        case 'N': return l != r;
        case '<': return l < r;
        case '>': return l > r;
        case 'l': return l <= r;
        case 'g': return l >= r;
        case 'A': return (l && r) ? 1 : 0;
        case 'O': return (l || r) ? 1 : 0;
        default: *ok = 0; return 0;
        }
    }
    case ND_TERNARY: {
        int a = 0;
        long long c = sema_eval_const(n->cond, &a);
        if (!a)
            return 0;
        return sema_eval_const(c ? n->then : n->els, ok);
    }
    case ND_SIZEOF: {
        if (n->sz_type) {
            if (!n->sz_type->is_complete && n->sz_type->kind != TY_VOID)
                return 0;
            *ok = 1;
            return n->sz_type->kind == TY_VOID ? 1 : type_size(n->sz_type);
        }
        if (n->lhs && n->lhs->ty) {
            /* sizeof never decays: arrays keep their full size. */
            *ok = 1;
            return type_size(n->lhs->ty);
        }
        return 0;
    }
    case ND_CAST: {
        int a = 0;
        long long v = sema_eval_const(n->lhs, &a);
        if (!a)
            return 0;
        *ok = 1;
        if (n->sz_type) {
            int sz = type_size(n->sz_type);
            if (sz == 1)
                v = n->sz_type->is_unsigned ? (v & 0xFF)
                                            : (long long)(signed char)v;
            else if (sz == 2)
                v = n->sz_type->is_unsigned ? (v & 0xFFFF)
                                            : (long long)(short)v;
            else if (sz == 4)
                v = n->sz_type->is_unsigned ? (v & 0xFFFFFFFFLL)
                                            : (long long)(int)v;
        }
        return v;
    }
    case ND_COMMA:
        return sema_eval_const(n->rhs, ok);
    default:
        return 0;
    }
}

static void check_scalar(Node *n, Type *t, const char *ctx) {
    if (!is_scalar(decay(t)))
        error_at(n->loc, "%s requires scalar operand", ctx);
}

static Type *visit_expr(Sema *s, Node *n) {
    if (!n)
        return ty_int;
    switch (n->kind) {
    case ND_NUM:
        if (!n->ty)
            n->ty = ty_int;
        return n->ty;
    case ND_STR:
        n->ty = array_of(ty_char, (int)n->slen + 1);
        return n->ty;
    case ND_VAR: {
        Decl *d = sscope_find(s->scope, n->name);
        if (!d)
            error_at(n->loc, "undeclared identifier '%s'", n->name);
        n->decl = d;
        if (d->is_enumconst) {
            n->kind = ND_NUM;
            n->ival = d->enum_val;
            n->ty = ty_int;
            n->name = NULL;
            n->decl = NULL;
            return ty_int;
        }
        if (d->is_typedef)
            error_at(n->loc, "'%s' is a type name, not a variable", n->name);
        n->ty = d->type;
        return n->ty;
    }
    case ND_CALL: {
        Type *ft = visit_expr(s, n->lhs);
        ft = decay(ft);
        if (ft->kind != TY_PTR || ft->base->kind != TY_FUNC)
            error_at(n->loc, "called object is not a function");
        Type *f = ft->base;
        if (f->num_params > 0 && !f->is_variadic) {
            if (n->nargs < f->num_params)
                error_at(n->loc, "too few arguments to function call");
            if (n->nargs > f->num_params)
                error_at(n->loc, "too many arguments to function call");
        } else if (f->num_params == 0 && !f->is_variadic) {
            /* `()` unspecified: accept any count. */
        }
        for (int i = 0; i < n->nargs; i++) {
            Type *at = visit_expr(s, n->args[i]);
            (void)at;
            /* Array/function args decay; no further checks. */
            n->args[i]->ty = decay(n->args[i]->ty);
        }
        n->ty = f->ret;
        return n->ty;
    }
    case ND_ASSIGN: {
        Type *l = visit_expr(s, n->lhs);
        Type *r = visit_expr(s, n->rhs);
        if (!is_lvalue(n->lhs))
            error_at(n->loc, "left side of assignment is not assignable");
        if (l->kind == TY_ARRAY)
            error_at(n->loc, "cannot assign to array");
        if (l->kind == TY_FUNC)
            error_at(n->loc, "cannot assign to function");
        /* Allow scalar<->scalar, pointer<->pointer (any), pointer<->int
         * (with implicit conversion), struct same-type. */
        if (l->kind == TY_STRUCT || l->kind == TY_UNION) {
            if (r->kind != l->kind || r->tag != l->tag)
                error_at(n->loc, "incompatible struct assignment");
        } else if (is_pointer(decay(l)) || is_pointer(decay(r))) {
            /* pointer mixes allowed (void* generic, 0/NULL ok) */
        } else if (!is_arith(l) || !is_arith(r)) {
            error_at(n->loc, "incompatible types in assignment");
        }
        n->ty = l;
        return n->ty;
    }
    case ND_COMPOUND: {
        Type *l = visit_expr(s, n->lhs);
        Type *r = visit_expr(s, n->rhs);
        if (!is_lvalue(n->lhs))
            error_at(n->loc, "left side of assignment is not assignable");
        if (n->op == '+' || n->op == '-') {
            if (is_pointer(decay(l)) && is_integer(r)) {
                n->ty = l;
                return n->ty;
            }
        }
        if (!is_arith(l) || !is_arith(r))
            error_at(n->loc, "invalid operands to compound assignment");
        n->ty = l;
        return n->ty;
    }
    case ND_TERNARY: {
        Type *c = visit_expr(s, n->cond);
        check_scalar(n->cond, c, "ternary condition");
        Type *a = visit_expr(s, n->then);
        Type *b = visit_expr(s, n->els);
        a = decay(a);
        b = decay(b);
        if (is_arith(a) && is_arith(b))
            n->ty = usual_arith(a, b);
        else if (is_pointer(a) && is_pointer(b))
            n->ty = a;
        else if (is_pointer(a) && b->kind == TY_VOID)
            n->ty = a;
        else
            n->ty = a;
        return n->ty;
    }
    case ND_COMMA: {
        visit_expr(s, n->lhs);
        Type *r = visit_expr(s, n->rhs);
        n->ty = r;
        return n->ty;
    }
    case ND_UNARY: {
        if (n->op == 'P' || n->op == 'p' || n->op == 'M' || n->op == 'm') {
            Type *l = visit_expr(s, n->lhs);
            if (!is_lvalue(n->lhs))
                error_at(n->loc, "++ / -- need an assignable operand");
            if (!is_arith(l) && !is_pointer(decay(l)))
                error_at(n->loc, "invalid operand to ++/--");
            n->ty = decay(l);
            return n->ty;
        }
        if (n->op == '&') {
            Type *l = visit_expr(s, n->lhs);
            if (n->lhs->kind == ND_NUM)
                error_at(n->loc, "cannot take address of constant");
            if (!is_lvalue(n->lhs))
                error_at(n->loc, "cannot take address of rvalue");
            if (l->kind == TY_ARRAY) {
                /* &arr -> pointer to array. */
                n->ty = ptr_to(l);
                return n->ty;
            }
            n->ty = ptr_to(l);
            return n->ty;
        }
        if (n->op == '*') {
            Type *l = visit_expr(s, n->lhs);
            l = decay(l);
            if (l->kind != TY_PTR)
                error_at(n->loc, "cannot dereference non-pointer");
            if (l->base->kind == TY_VOID)
                error_at(n->loc, "cannot dereference void pointer");
            n->ty = l->base;
            return n->ty;
        }
        Type *l = visit_expr(s, n->lhs);
        if (n->op == '!') {
            check_scalar(n->lhs, l, "'!'");
            n->ty = ty_int;
            return n->ty;
        }
        if (!is_arith(l))
            error_at(n->loc, "invalid operand to unary operator");
        n->ty = promote(l);
        return n->ty;
    }
    case ND_BINARY: {
        Type *l = visit_expr(s, n->lhs);
        Type *r = visit_expr(s, n->rhs);
        Type *dl = decay(l), *dr = decay(r);
        switch (n->op) {
        case '*':
        case '/':
        case '%':
            if (!is_arith(l) || !is_arith(r))
                error_at(n->loc, "invalid operands to arithmetic");
            n->ty = usual_arith(l, r);
            return n->ty;
        case '+': {
            if (is_arith(l) && is_arith(r)) {
                n->ty = usual_arith(l, r);
                return n->ty;
            }
            if (is_pointer(dl) && is_integer(r)) {
                n->ty = dl;
                return n->ty;
            }
            if (is_pointer(dr) && is_integer(l)) {
                n->ty = dr;
                return n->ty;
            }
            error_at(n->loc, "invalid operands to '+'");
            break;
        }
        case '-': {
            if (is_arith(l) && is_arith(r)) {
                n->ty = usual_arith(l, r);
                return n->ty;
            }
            if (is_pointer(dl) && is_integer(r)) {
                n->ty = dl;
                return n->ty;
            }
            if (is_pointer(dl) && is_pointer(dr)) {
                n->ty = ty_long;
                return n->ty;
            }
            error_at(n->loc, "invalid operands to '-'");
            break;
        }
        case 'L':
        case 'R':
            if (!is_integer(l) || !is_integer(r))
                error_at(n->loc, "shifts need integer operands");
            n->ty = promote(l);
            return n->ty;
        case 'E':
        case 'N': {
            if (is_arith(l) && is_arith(r)) {
            } else if (is_pointer(dl) && is_pointer(dr)) {
            } else if ((is_pointer(dl) || is_pointer(dr)) &&
                       (n->op == 'E' || n->op == 'N')) {
                /* Null pointer constant comparison (`p == 0`). */
                Node *other = is_pointer(dl) ? n->rhs : n->lhs;
                int ok = 0;
                long long v = sema_eval_const(other, &ok);
                if (!ok || v != 0)
                    error_at(n->loc, "invalid operands to comparison "
                                      "(pointer needs address or 0)");
            } else {
                error_at(n->loc, "invalid operands to comparison");
            }
            n->ty = ty_int;
            return n->ty;
        }
        case '<':
        case '>':
        case 'l':
        case 'g': {
            if (is_arith(l) && is_arith(r)) {
            } else if (is_pointer(dl) && is_pointer(dr)) {
            } else {
                error_at(n->loc, "invalid operands to comparison");
            }
            n->ty = ty_int;
            return n->ty;
        }
        case '&':
        case '|':
        case '^':
            if (!is_integer(l) || !is_integer(r))
                error_at(n->loc, "bitwise ops need integer operands");
            n->ty = usual_arith(l, r);
            return n->ty;
        case 'A':
        case 'O':
            check_scalar(n->lhs, l, "logical operator");
            check_scalar(n->rhs, r, "logical operator");
            n->ty = ty_int;
            return n->ty;
        default:
            error_at(n->loc, "unknown binary operator");
        }
        break;
    }
    case ND_CAST: {
        Type *l = visit_expr(s, n->lhs);
        (void)l;
        if (!is_scalar(decay(n->sz_type)) && n->sz_type->kind != TY_STRUCT &&
            n->sz_type->kind != TY_UNION)
            error_at(n->loc, "invalid cast target");
        n->ty = n->sz_type;
        return n->ty;
    }
    case ND_SIZEOF: {
        if (n->sz_type) {
            if ((n->sz_type->kind == TY_STRUCT ||
                 n->sz_type->kind == TY_UNION) &&
                !n->sz_type->is_complete)
                error_at(n->loc, "sizeof incomplete type");
            if (n->sz_type->kind == TY_VOID)
                error_at(n->loc, "sizeof void");
            if (n->sz_type->kind == TY_FUNC)
                error_at(n->loc, "sizeof function");
        } else {
            visit_expr(s, n->lhs);
            if (n->lhs->ty->kind == TY_FUNC)
                error_at(n->loc, "sizeof function");
        }
        n->ty = ty_ulong;
        return n->ty;
    }
    case ND_MEMBER: {
        Type *l = visit_expr(s, n->lhs);
        Type *st = l;
        if (n->is_arrow) {
            st = decay(l);
            if (st->kind != TY_PTR)
                error_at(n->loc, "'->' needs a pointer");
            st = st->base;
        } else {
            if (st->kind == TY_PTR)
                error_at(n->loc, "'.' used on pointer; use '->'");
        }
        if (st->kind != TY_STRUCT && st->kind != TY_UNION)
            error_at(n->loc, "member access on non-struct");
        Member *m = find_member(st, n->name);
        if (!m)
            error_at(n->loc, "'%s' has no member '%s'",
                     st->tag ? st->tag : "struct", n->name);
        n->member = m;
        n->ty = m->type;
        return n->ty;
    }
    case ND_SUBSCRIPT: {
        Type *l = visit_expr(s, n->lhs);
        Type *r = visit_expr(s, n->rhs);
        Type *arr = NULL;
        if ((is_pointer(decay(l))) && is_integer(r))
            arr = decay(l);
        else if ((is_pointer(decay(r))) && is_integer(l)) {
            arr = decay(r);
            /* normalize a[i] == *(a+i): keep order, codegen handles both. */
        } else
            error_at(n->loc, "invalid subscript");
        if (arr->kind != TY_PTR)
            error_at(n->loc, "invalid subscript");
        n->ty = arr->base;
        return n->ty;
    }
    case ND_INIT_LIST: {
        for (int i = 0; i < n->nstmts; i++)
            visit_expr(s, n->stmts[i]);
        n->ty = ty_int; /* replaced by context */
        return n->ty;
    }
    default:
        error_at(n->loc, "cannot analyze expression");
    }
    return ty_int;
}

static void visit_vardecl(Sema *s, Node *n, int is_global);

static void visit_block(Sema *s, Node *n) {
    SemaScope *saved = s->scope;
    SemaScope *ns = (SemaScope *)arena_calloc(g_arena, 1, sizeof(SemaScope));
    ns->parent = saved;
    s->scope = ns;
    for (int i = 0; i < n->nstmts; i++)
        visit_stmt(s, n->stmts[i]);
    s->scope = saved;
}

static void visit_stmt(Sema *s, Node *n) {
    if (!n)
        return;
    switch (n->kind) {
    case ND_BLOCK:
        visit_block(s, n);
        break;
    case ND_NOP:
        break;
    case ND_EXPR_STMT:
        if (n->lhs)
            visit_expr(s, n->lhs);
        break;
    case ND_VARDECL:
        visit_vardecl(s, n, 0);
        break;
    case ND_IF:
        visit_expr(s, n->cond);
        check_scalar(n->cond, n->cond->ty, "if condition");
        visit_stmt(s, n->then);
        if (n->els)
            visit_stmt(s, n->els);
        break;
    case ND_WHILE:
        visit_expr(s, n->cond);
        check_scalar(n->cond, n->cond->ty, "while condition");
        s->loop_depth++;
        visit_stmt(s, n->body);
        s->loop_depth--;
        break;
    case ND_DO:
        s->loop_depth++;
        visit_stmt(s, n->body);
        s->loop_depth--;
        visit_expr(s, n->cond);
        check_scalar(n->cond, n->cond->ty, "do-while condition");
        break;
    case ND_FOR: {
        SemaScope *saved = s->scope;
        SemaScope *ns = (SemaScope *)arena_calloc(g_arena, 1, sizeof(SemaScope));
        ns->parent = saved;
        s->scope = ns;
        if (n->init) {
            if (n->init->kind == ND_BLOCK || n->init->kind == ND_VARDECL)
                visit_stmt(s, n->init);
            else
                visit_stmt(s, n->init);
        }
        if (n->cond) {
            visit_expr(s, n->cond);
            check_scalar(n->cond, n->cond->ty, "for condition");
        }
        if (n->inc)
            visit_expr(s, n->inc);
        s->loop_depth++;
        visit_stmt(s, n->body);
        s->loop_depth--;
        s->scope = saved;
        break;
    }
    case ND_RETURN: {
        if (n->lhs) {
            Type *t = visit_expr(s, n->lhs);
            (void)t;
            /* Conversions allowed; void-return with value is an error. */
            if (s->cur_ret && s->cur_ret->kind == TY_VOID)
                error_at(n->loc, "returning a value from void function");
        } else {
            if (s->cur_ret && s->cur_ret->kind != TY_VOID) {
                /* Missing value: allowed (UB at runtime), no error. */
            }
        }
        break;
    }
    case ND_BREAK:
        if (s->loop_depth == 0 && s->switch_depth == 0)
            error_at(n->loc, "'break' outside loop or switch");
        break;
    case ND_CONTINUE:
        if (s->loop_depth == 0)
            error_at(n->loc, "'continue' outside loop");
        break;
    case ND_SWITCH: {
        Type *t = visit_expr(s, n->cond);
        if (!is_integer(t))
            error_at(n->loc, "switch needs an integer");
        s->switch_depth++;
        s->loop_depth++; /* break allowed */
        visit_stmt(s, n->body);
        s->loop_depth--;
        s->switch_depth--;
        break;
    }
    case ND_CASE: {
        if (s->switch_depth == 0)
            error_at(n->loc, "'case' outside switch");
        if (!n->is_default) {
            Type *t = visit_expr(s, n->lhs);
            (void)t;
            int ok = 0;
            long long v = sema_eval_const(n->lhs, &ok);
            if (!ok)
                error_at(n->loc, "case label must be constant");
            n->ival = v;
        }
        for (int i = 0; i < n->nstmts; i++)
            visit_stmt(s, n->stmts[i]);
        break;
    }
    case ND_LABEL:
        add_label(s, n->name);
        visit_stmt(s, n->body);
        break;
    case ND_GOTO:
        add_goto(s, n->name, n->loc);
        break;
    default:
        visit_expr(s, n);
        break;
    }
}

/* Check initializer compat; complete `T arr[]` from init. */
static void check_init(Sema *s, Decl *d, Node *init) {
    if (!init)
        return;
    if (init->kind == ND_INIT_LIST) {
        for (int i = 0; i < init->nstmts; i++) {
            Node *e = init->stmts[i];
            if (e->kind == ND_INIT_LIST)
                continue;
            visit_expr(s, e);
        }
        Type *t = d->type;
        if (t->kind == TY_ARRAY && t->array_len < 0) {
            if (t->base->kind == TY_CHAR && init->nstmts == 1 &&
                init->stmts[0]->kind == ND_STR) {
                /* `char s[] = "hi"` handled below as scalar. */
            } else {
                t->array_len = init->nstmts;
                t->size = t->base->size * init->nstmts;
                t->is_complete = 1;
            }
        }
        /* String inside braces: `char s[] = {"hi"}` — same handling. */
        return;
    }
    Type *t = visit_expr(s, init);
    if (d->type->kind == TY_ARRAY) {
        if (init->kind == ND_STR && d->type->base->kind == TY_CHAR) {
            if (d->type->array_len < 0) {
                d->type->array_len = (int)init->slen + 1;
                d->type->size = d->type->base->size * d->type->array_len;
                d->type->is_complete = 1;
            } else if ((int)init->slen + 1 > d->type->array_len + 1 &&
                       (int)init->slen > d->type->array_len) {
                error_at(init->loc, "string too long for array");
            }
            return;
        }
        error_at(init->loc, "array initializer must be a list or string");
    }
    if ((d->type->kind == TY_STRUCT || d->type->kind == TY_UNION)) {
        /* struct/union scalar init like `= expr` of same type: allowed,
         * checked in codegen path via assignment rules. */
    }
    (void)t;
}

static void visit_vardecl(Sema *s, Node *n, int is_global) {
    Decl *d = n->decl;
    if (!d)
        return;
    if (d->is_typedef)
        return; /* no code, already in scope */
    if (d->type->kind == TY_FUNC && !d->init) {
        /* prototype inside function: register global. */
        Decl *g = sscope_find(s->gscope, d->name);
        if (!g) {
            sscope_add(s->gscope, d);
        }
        return;
    }
    if (d->type->kind == TY_VOID)
        error_at(n->loc, "variable '%s' has void type",
                 d->name ? d->name : "?");
    if (d->type->kind == TY_FUNC)
        error_at(n->loc, "function '%s' used as variable", d->name);
    if ((d->type->kind == TY_STRUCT || d->type->kind == TY_UNION) &&
        !d->type->is_complete)
        error_at(n->loc, "variable has incomplete type");
    if (d->type->kind == TY_ARRAY && d->type->array_len < 0 && !d->init &&
        !d->is_extern)
        error_at(n->loc, "array '%s' needs a size or initializer", d->name);
    /* Register. */
    if (is_global) {
        /* already in parser global scope; mirror into sema global. */
        if (!sscope_find(s->gscope, d->name))
            sscope_add(s->gscope, d);
        if (d->init) {
            if (d->init->kind == ND_INIT_LIST) {
                for (int i = 0; i < d->init->nstmts; i++) {
                    Node *e = d->init->stmts[i];
                    if (e->kind != ND_INIT_LIST)
                        visit_expr(s, e);
                    else {
                        /* nested list: visit leaves */
                        for (int j = 0; j < e->nstmts; j++)
                            visit_expr(s, e->stmts[j]);
                    }
                }
                if (d->type->kind == TY_ARRAY && d->type->array_len < 0) {
                    d->type->array_len = d->init->nstmts;
                    d->type->size = d->type->base->size * d->init->nstmts;
                    d->type->is_complete = 1;
                }
            } else {
                check_init(s, d, d->init);
            }
        }
    } else {
        sscope_add(s->scope, d);
        if (d->init)
            check_init(s, d, d->init);
    }
}

void sema_analyze(void) {
    extern Node **parser_prog_list(void);
    extern int parser_prog_count(void);
    extern Decl **parser_enum_consts(int *n);
    Node **prog = parser_prog_list();
    int n = parser_prog_count();

    Sema s;
    memset(&s, 0, sizeof s);
    s.gscope = (SemaScope *)arena_calloc(g_arena, 1, sizeof(SemaScope));
    s.scope = s.gscope;

    /* Enum constants are visible wherever the enum is visible; with our
     * single global side list, register them all globally. */
    {
        int ne = 0;
        Decl **ec = parser_enum_consts(&ne);
        for (int i = 0; i < ne; i++) {
            if (!sscope_find(s.gscope, ec[i]->name))
                sscope_add(s.gscope, ec[i]);
        }
    }

    /* First pass: register globals (functions + vars + typedefs). */
    for (int i = 0; i < n; i++) {
        Node *t = prog[i];
        if (t->kind == ND_FUNCDEF) {
            if (!sscope_find(s.gscope, t->decl->name))
                sscope_add(s.gscope, t->decl);
        } else if (t->kind == ND_VARDECL) {
            Decl *d = t->decl;
            if (d->is_typedef) {
                if (!sscope_find(s.gscope, d->name))
                    sscope_add(s.gscope, d);
            } else if (d->type->kind == TY_FUNC) {
                if (!sscope_find(s.gscope, d->name))
                    sscope_add(s.gscope, d);
            } else {
                Decl *old = sscope_find(s.gscope, d->name);
                if (old && old != d) {
                    /* Tentative definitions merge; real redefinition
                     * with initializer twice is an error. */
                    if (d->init && old->init)
                        error_at(t->loc, "redefinition of '%s'", d->name);
                    if (d->init)
                        old->init = d->init;
                    t->decl = old;
                } else if (!old) {
                    sscope_add(s.gscope, d);
                }
            }
        }
    }

    /* Second pass: check inits + function bodies. */
    const char *defined[1024];
    int ndefined = 0;
    for (int i = 0; i < n; i++) {
        Node *t = prog[i];
        if (t->kind == ND_VARDECL) {
            Decl *d = t->decl;
            if (d->is_typedef || d->type->kind == TY_FUNC)
                continue;
            s.scope = s.gscope;
            s.cur_ret = NULL;
            if (d->init) {
                if (d->init->kind == ND_INIT_LIST) {
                    for (int j = 0; j < d->init->nstmts; j++) {
                        Node *e = d->init->stmts[j];
                        if (e->kind != ND_INIT_LIST)
                            visit_expr(&s, e);
                        else {
                            for (int k = 0; k < e->nstmts; k++)
                                visit_expr(&s, e->stmts[k]);
                        }
                    }
                    if (d->type->kind == TY_ARRAY && d->type->array_len < 0) {
                        /* `char s[] = "hi"` as list {"hi"}? */
                        if (d->type->base->kind == TY_CHAR &&
                            d->init->nstmts == 1 &&
                            d->init->stmts[0]->kind == ND_STR) {
                            d->type->array_len =
                                (int)d->init->stmts[0]->slen + 1;
                            d->type->size =
                                d->type->array_len * d->type->base->size;
                            d->type->is_complete = 1;
                        } else {
                            d->type->array_len = d->init->nstmts;
                            d->type->size =
                                d->type->base->size * d->init->nstmts;
                            d->type->is_complete = 1;
                        }
                    }
                } else {
                    check_init(&s, d, d->init);
                }
            }
        } else if (t->kind == ND_FUNCDEF) {
            for (int k = 0; k < ndefined; k++) {
                if (strcmp(defined[k], t->decl->name) == 0)
                    error_at(t->loc, "redefinition of '%s'", t->decl->name);
            }
            if (ndefined < 1024)
                defined[ndefined++] = t->decl->name;
            s.cur_fn = t->decl;
            s.cur_ret = t->decl->type->ret;
            s.nlabels = 0;
            s.ngotos = 0;
            s.loop_depth = 0;
            s.switch_depth = 0;
            SemaScope *fs = (SemaScope *)arena_calloc(g_arena, 1, sizeof(SemaScope));
            fs->parent = s.gscope;
            s.scope = fs;
            /* params */
            for (int j = 0; j < t->nparams; j++) {
                Decl *pd = t->params[j];
                if (pd->name) {
                    if (sscope_find(fs, pd->name))
                        error_at(t->loc, "duplicate parameter");
                    /* `void` unnamed param already excluded. */
                    sscope_add(fs, pd);
                }
            }
            visit_block(&s, t->body);
            /* goto checks */
            for (int g = 0; g < s.ngotos; g++) {
                int found = 0;
                for (int l = 0; l < s.nlabels; l++) {
                    if (strcmp(s.gotos[g], s.labels[l]) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    error_at(s.goto_locs[g], "undefined label '%s'",
                             s.gotos[g]);
            }
            s.scope = s.gscope;
            s.cur_fn = NULL;
            s.labels = NULL;
            s.gotos = NULL;
            s.goto_locs = NULL;
            s.caplabels = s.capgotos = 0;
        }
    }
}
