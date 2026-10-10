/* parser.c - recursive-descent + precedence-climbing expression parser.
 *
 * The parser handles scopes for typedef names, struct/union/enum tags, and
 * enum constants (needed to disambiguate declarations from expressions).
 * Expression type checking lives in sema.c; the parser only builds shapes.
 */
#include "parser.h"
#include <string.h>

typedef struct Tag Tag;
struct Tag {
    char *name; /* may be NULL for anonymous (stored separately) */
    Type *type;
    Tag *next;
};

typedef struct Scope Scope;
struct Scope {
    Decl *vars; /* vars + typedefs + enum consts */
    Tag *tags;
    Scope *parent;
};

typedef struct {
    Token *toks;
    size_t len;
    size_t pos;
    Scope *scope;      /* current scope */
    Scope *gscope;     /* global scope */
    int anon_id;       /* anonymous struct/union/enum counter */
    /* function-local label/goto fixups are handled in sema/codegen */
} Parser;

static Node **plist = NULL;
static int nlist = 0, caplist = 0;
static Decl **enum_consts = NULL;
static int nenum_consts = 0, cap_enum_consts = 0;

Node **parser_prog_list(void) {
    return plist;
}
int parser_prog_count(void) {
    return nlist;
}
Decl **parser_enum_consts(int *n) {
    *n = nenum_consts;
    return enum_consts;
}

static void emit_top(Node *n) {
    if (nlist == caplist) {
        int nc = caplist ? caplist * 2 : 64;
        Node **nl = (Node **)arena_alloc(g_arena, (size_t)nc * sizeof(Node *));
        if (plist)
            memcpy(nl, plist, (size_t)nlist * sizeof(Node *));
        plist = nl;
        caplist = nc;
    }
    plist[nlist++] = n;
}

static Token *peek(Parser *p, int off) {
    size_t i = p->pos + (size_t)off;
    if (i >= p->len)
        i = p->len - 1;
    return &p->toks[i];
}

static Token *cur(Parser *p) {
    return peek(p, 0);
}

static int at(Parser *p, TokKind k) {
    return cur(p)->kind == k;
}

static Token *next(Parser *p) {
    Token *t = cur(p);
    if (t->kind != TK_EOF)
        p->pos++;
    return t;
}

static Token *expect(Parser *p, TokKind k, const char *what) {
    Token *t = cur(p);
    if (t->kind != k)
        error_at(t->loc, "expected %s, found '%s'", what,
                 tokkind_name(t->kind));
    return next(p);
}

static int consume(Parser *p, TokKind k) {
    if (at(p, k)) {
        next(p);
        return 1;
    }
    return 0;
}

/* ---------- scopes ---------- */
static Scope *new_scope(Parser *p, Scope *parent) {
    Scope *s = (Scope *)arena_calloc(g_arena, 1, sizeof(Scope));
    s->parent = parent;
    (void)p;
    return s;
}

static void scope_add_var(Scope *s, Decl *d) {
    d->next = s->vars;
    s->vars = d;
}

static Decl *scope_find_var(Scope *s, const char *name) {
    for (; s; s = s->parent) {
        for (Decl *d = s->vars; d; d = d->next) {
            if (d->name && strcmp(d->name, name) == 0)
                return d;
        }
    }
    return NULL;
}

static Decl *scope_find_in(Scope *s, const char *name) {
    for (Decl *d = s->vars; d; d = d->next) {
        if (d->name && strcmp(d->name, name) == 0)
            return d;
    }
    return NULL;
}

static Tag *scope_find_tag(Scope *s, const char *name) {
    for (; s; s = s->parent) {
        for (Tag *t = s->tags; t; t = t->next) {
            if (t->name && name && strcmp(t->name, name) == 0)
                return t;
        }
    }
    return NULL;
}

static void scope_add_tag(Scope *s, Tag *t) {
    t->next = s->tags;
    s->tags = t;
}

/* True if `name` is a typedef name visible here. */
static int is_typedef_name(Parser *p, const char *name) {
    Decl *d = scope_find_var(p->scope, name);
    return d && d->is_typedef;
}

/* ---------- declspec ---------- */
typedef struct {
    Type *base;       /* resolved base type (may be NULL if invalid) */
    int storage;      /* bitmask */
    int has_base;
} DeclSpec;

#define ST_TYPEDEF 1
#define ST_STATIC 2
#define ST_EXTERN 4
#define ST_AUTO 8
#define ST_REGISTER 16
#define ST_INLINE 32
#define ST_CONST 64

static int is_declspec_tok(Parser *p, Token *t);

static Type *parse_struct_or_union(Parser *p, int is_union, Loc loc);
static Type *parse_enum(Parser *p, Loc loc);

static int is_declspec_tok(Parser *p, Token *t) {
    switch (t->kind) {
    case TK_INT:
    case TK_CHAR:
    case TK_SHORT:
    case TK_LONG:
    case TK_UNSIGNED:
    case TK_SIGNED:
    case TK_VOID:
    case TK_STRUCT:
    case TK_UNION:
    case TK_ENUM:
    case TK_BOOL:
    case TK_TYPEDEF:
    case TK_STATIC:
    case TK_EXTERN:
    case TK_CONST:
    case TK_VOLATILE:
    case TK_REGISTER:
    case TK_AUTO:
    case TK_INLINE:
        return 1;
    case TK_IDENT:
        return is_typedef_name(p, t->sval);
    default:
        return 0;
    }
}

/* Parse declaration specifiers. Fills storage flags and base type. */
static DeclSpec parse_declspec(Parser *p) {
    DeclSpec ds;
    memset(&ds, 0, sizeof ds);
    int n_void = 0, n_char = 0, n_short = 0, n_int = 0, n_long = 0;
    int n_signed = 0, n_unsigned = 0, n_bool = 0;
    Type *struct_ty = NULL;
    Type *typedef_ty = NULL;
    int n_struct = 0, n_typedef = 0, n_enum = 0;
    Loc loc = cur(p)->loc;

    while (is_declspec_tok(p, cur(p))) {
        Token *t = next(p);
        switch (t->kind) {
        case TK_TYPEDEF: ds.storage |= ST_TYPEDEF; break;
        case TK_STATIC: ds.storage |= ST_STATIC; break;
        case TK_EXTERN: ds.storage |= ST_EXTERN; break;
        case TK_AUTO: ds.storage |= ST_AUTO; break;
        case TK_REGISTER: ds.storage |= ST_REGISTER; break;
        case TK_INLINE: ds.storage |= ST_INLINE; break;
        case TK_CONST: ds.storage |= ST_CONST; break;
        case TK_VOLATILE: break; /* accepted, ignored */
        case TK_VOID: n_void++; break;
        case TK_CHAR: n_char++; break;
        case TK_SHORT: n_short++; break;
        case TK_INT: n_int++; break;
        case TK_LONG: n_long++; break;
        case TK_SIGNED: n_signed++; break;
        case TK_UNSIGNED: n_unsigned++; break;
        case TK_BOOL: n_bool++; break;
        case TK_STRUCT:
            struct_ty = parse_struct_or_union(p, 0, t->loc);
            n_struct++;
            break;
        case TK_UNION:
            struct_ty = parse_struct_or_union(p, 1, t->loc);
            n_struct++;
            break;
        case TK_ENUM:
            struct_ty = parse_enum(p, t->loc);
            n_enum++;
            break;
        case TK_IDENT: {
            Decl *td = scope_find_var(p->scope, t->sval);
            if (td && td->is_typedef) {
                typedef_ty = td->type;
                n_typedef++;
            } else {
                error_at(t->loc, "unknown type name '%s'", t->sval);
            }
            break;
        }
        default:
            break;
        }
    }
    if (!is_declspec_tok(p, cur(p)) && n_void + n_char + n_short + n_int +
                n_long + n_signed + n_unsigned + n_bool + n_struct +
                n_typedef + n_enum ==
            0) {
        /* No specifiers at all (e.g. stray `;` handled by caller). */
        ds.base = NULL;
        return ds;
    }
    /* Resolve to a base type. */
    int ntypes = n_void + n_char + n_short + n_int + (n_long ? 1 : 0) +
                 n_bool + n_struct + n_typedef + n_enum;
    (void)ntypes;
    if (n_typedef + n_struct + n_enum > 1) {
        error_at(loc, "multiple types in declaration");
    }
    if ((n_typedef + n_struct + n_enum) == 1 &&
        (n_void + n_char + n_short + n_int + n_long + n_signed + n_unsigned +
             n_bool >
         0)) {
        /* Allow `unsigned typedefname`? No: keep strict except const. */
        if (!(n_typedef == 1 && (n_signed || n_unsigned))) {
            if (!(n_struct == 1 && n_int == 0 && n_signed == 0 &&
                  n_unsigned == 0 && n_short == 0 && n_long == 0 &&
                  n_char == 0 && n_void == 0 && n_bool == 0)) {
                /* struct with qualifiers already consumed; plain
                 * `long struct` etc. is invalid. */
                if (n_struct + n_enum + n_typedef)
                    error_at(loc, "invalid combination of type specifiers");
            }
        }
    }
    if (n_typedef == 1 && (n_signed || n_unsigned) &&
        (n_void + n_char + n_short + n_int + n_long + n_bool + n_struct +
             n_enum ==
         0)) {
        /* `unsigned T` where T is typedef to int type: apply sign. */
        if (n_unsigned && typedef_ty == ty_int)
            typedef_ty = ty_uint;
        else if (n_unsigned && typedef_ty == ty_long)
            typedef_ty = ty_ulong;
        else if (n_unsigned && typedef_ty == ty_short)
            typedef_ty = ty_ushort;
        else if (n_unsigned && typedef_ty == ty_char)
            typedef_ty = ty_uchar;
        else if (n_signed) { /* signed alone: keep */
        } else
            error_at(loc, "invalid signed/unsigned with typedef");
        ds.base = typedef_ty;
        ds.has_base = 1;
        return ds;
    }
    if (n_typedef == 1) {
        ds.base = typedef_ty;
        ds.has_base = 1;
        return ds;
    }
    if (n_struct == 1 || n_enum == 1) {
        ds.base = struct_ty;
        ds.has_base = 1;
        return ds;
    }
    if (n_bool) {
        if (n_void + n_char + n_short + n_int + n_long + n_signed +
                n_unsigned >
            0)
            error_at(loc, "invalid combination with _Bool");
        ds.base = ty_bool;
        ds.has_base = 1;
        return ds;
    }
    if (n_void) {
        if (n_void > 1 || n_char + n_short + n_int + n_long + n_signed +
                    n_unsigned >
                0)
            error_at(loc, "invalid combination with void");
        ds.base = ty_void;
        ds.has_base = 1;
        return ds;
    }
    /* Integer combinations. */
    if (n_long > 2)
        error_at(loc, "too many 'long'");
    if (n_short && n_long)
        error_at(loc, "cannot combine short and long");
    if (n_char && (n_short || n_long || n_int > 1))
        error_at(loc, "invalid combination with char");
    if (n_short > 1 || n_char > 1 || n_int > 1 || n_signed > 1 || n_unsigned > 1)
        error_at(loc, "duplicate type specifier");
    if (n_signed && n_unsigned)
        error_at(loc, "cannot combine signed and unsigned");
    int is_uns = n_unsigned ? 1 : 0;
    if (n_long >= 1) {
        ds.base = is_uns ? ty_ulong : ty_long;
    } else if (n_short) {
        ds.base = is_uns ? ty_ushort : ty_short;
    } else if (n_char) {
        ds.base = is_uns ? ty_uchar : ty_char;
    } else {
        /* int / signed / unsigned / (empty defaults handled by caller) */
        ds.base = is_uns ? ty_uint : ty_int;
    }
    ds.has_base = 1;
    if (ds.storage & ST_CONST)
        ds.base = ds.base; /* const ignored for codegen */
    return ds;
}

/* Forward decls. */
static Node *parse_expr(Parser *p);
static Node *parse_assign(Parser *p);
static Node *parse_ternary(Parser *p);
static Node *parse_lor(Parser *p);
static Node *parse_land(Parser *p);
static Node *parse_bitor(Parser *p);
static Node *parse_bitxor(Parser *p);
static Node *parse_bitand(Parser *p);
static Node *parse_equality(Parser *p);
static Node *parse_relational(Parser *p);
static Node *parse_shift(Parser *p);
static Node *parse_add(Parser *p);
static Node *parse_mul(Parser *p);
static Node *parse_cast(Parser *p);
static Node *parse_unary(Parser *p);
static Node *parse_postfix(Parser *p);
static Node *parse_primary(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_block(Parser *p);
static Decl *parse_declarator(Parser *p, Type *base);
static Decl *parse_direct(Parser *p, Type *base, char **out_name, Loc *out_loc);

/* Apply `k` pending prefix stars to a direct-declarator type. Stars
 * distribute to the base position: through paren-group pointers and
 * function returns / array elements (`int *(*f)(int)` is pointer to
 * function returning pointer), or wrap outside a plain base. */
static Type *apply_pending(Type *t, int k) {
    if (k <= 0)
        return t;
    if (t->kind == TY_PTR)
        return ptr_to(apply_pending(t->base, k));
    if (t->kind == TY_FUNC) {
        Type *ft = func_type(apply_pending(t->ret, k));
        ft->params = t->params;
        ft->num_params = t->num_params;
        ft->is_variadic = t->is_variadic;
        return ft;
    }
    if (t->kind == TY_ARRAY)
        return array_of(apply_pending(t->base, k), t->array_len);
    Type *r = t;
    for (int i = 0; i < k; i++)
        r = ptr_to(r);
    return r;
}

static Decl *parse_declarator(Parser *p, Type *base) {
    /* Prefix stars are pending: the direct part decides where they bind. */
    int nstars = 0;
    while (at(p, TK_STAR)) {
        next(p);
        for (;;) {
            if (at(p, TK_CONST) || at(p, TK_VOLATILE)) {
                next(p);
                continue;
            }
            if (at(p, TK_IDENT) &&
                (strcmp(cur(p)->sval, "restrict") == 0 ||
                 strcmp(cur(p)->sval, "__restrict") == 0 ||
                 strcmp(cur(p)->sval, "__restrict__") == 0 ||
                 strcmp(cur(p)->sval, "_Atomic") == 0)) {
                next(p);
                continue;
            }
            break;
        }
        nstars++;
    }
    char *name = NULL;
    Loc loc = cur(p)->loc;
    Decl *d = parse_direct(p, base, &name, &loc);
    d->type = apply_pending(d->type, nstars);
    return d;
}
static Node *parse_initializer(Parser *p);
static Type *parse_typename(Parser *p);
static long long eval_const(Parser *p, Node *n, int *ok);

/* struct/union: `struct [tag] { members } [?]`. Tag lookup/creation. */
static Type *parse_struct_or_union(Parser *p, int is_union, Loc loc) {
    char *tag = NULL;
    if (at(p, TK_IDENT)) {
        Token *t = next(p);
        tag = t->sval;
    }
    if (!at(p, TK_LBRACE)) {
        /* Reference to existing (or incomplete) tag. */
        if (!tag)
            error_at(loc, "expected struct/union tag or '{'");
        Tag *tg = scope_find_tag(p->scope, tag);
        if (tg && ((is_union && tg->type->kind == TY_UNION) ||
                   (!is_union && tg->type->kind == TY_STRUCT)))
            return tg->type;
        /* Forward-declared incomplete type. */
        Type *t = new_type(is_union ? TY_UNION : TY_STRUCT);
        t->tag = tag;
        t->size = 0;
        t->align = 1;
        t->is_complete = 0;
        Tag *nt = (Tag *)arena_calloc(g_arena, 1, sizeof(Tag));
        nt->name = tag;
        nt->type = t;
        scope_add_tag(p->scope, nt);
        return t;
    }
    /* Definition. */
    next(p); /* { */
    Type *st = NULL;
    if (tag) {
        Tag *tg = scope_find_tag(p->scope, tag);
        if (tg && ((is_union && tg->type->kind == TY_UNION) ||
                   (!is_union && tg->type->kind == TY_STRUCT)) &&
            tg->type->is_complete)
            error_at(loc, "redefinition of '%s'", tag);
        if (tg && ((is_union && tg->type->kind == TY_UNION) ||
                   (!is_union && tg->type->kind == TY_STRUCT))) {
            st = tg->type;
        } else {
            st = new_type(is_union ? TY_UNION : TY_STRUCT);
            st->tag = tag;
            Tag *nt = (Tag *)arena_calloc(g_arena, 1, sizeof(Tag));
            nt->name = tag;
            nt->type = st;
            scope_add_tag(p->scope, nt);
        }
    } else {
        st = new_type(is_union ? TY_UNION : TY_STRUCT);
        st->tag = NULL;
    }
    st->members = NULL;
    st->num_members = 0;
    st->is_complete = 0;
    Member *head = NULL, *tail = NULL;
    Scope *mscope = new_scope(p, p->scope);
    Scope *saved = p->scope;
    p->scope = mscope;
    while (!at(p, TK_RBRACE)) {
        if (at(p, TK_EOF))
            error_at(cur(p)->loc, "unterminated struct body");
        DeclSpec ds = parse_declspec(p);
        if (!ds.has_base) {
            /* Anonymous nested struct/union without declarator:
             * `struct { ... };` -> splice members. */
            if (at(p, TK_SEMI)) {
                next(p);
                continue;
            }
            error_at(cur(p)->loc, "expected declaration in struct");
        }
        if (at(p, TK_SEMI)) {
            /* Anonymous member `struct S;` or `union {..};` -> splice if
             * anonymous, else ignore forward decl. */
            next(p);
            if (!ds.base)
                continue;
            if ((ds.base->kind == TY_STRUCT || ds.base->kind == TY_UNION) &&
                ds.base->tag == NULL && ds.base->is_complete) {
                for (Member *m = ds.base->members; m; m = m->next) {
                    Member *nm = (Member *)arena_calloc(g_arena, 1,
                                                        sizeof(Member));
                    *nm = *m;
                    nm->next = NULL;
                    if (!head)
                        head = tail = nm;
                    else {
                        tail->next = nm;
                        tail = nm;
                    }
                    st->num_members++;
                }
            }
            continue;
        }
        /* One or more declarators. */
        for (;;) {
            Decl *d = parse_declarator(p, ds.base);
            if (d->name == NULL) {
                /* Anonymous struct/union member: splice. */
                if ((d->type->kind == TY_STRUCT ||
                     d->type->kind == TY_UNION) &&
                    d->type->is_complete) {
                    for (Member *m = d->type->members; m; m = m->next) {
                        Member *nm = (Member *)arena_calloc(g_arena, 1,
                                                            sizeof(Member));
                        *nm = *m;
                        nm->next = NULL;
                        if (!head)
                            head = tail = nm;
                        else {
                            tail->next = nm;
                            tail = nm;
                        }
                        st->num_members++;
                    }
                } else {
                    error_at(d->loc, "anonymous member must be struct/union");
                }
            } else {
                /* Bitfields `x : 3` are not supported. */
                if (at(p, TK_COLON))
                    error_at(cur(p)->loc,
                             "bitfields are not supported");
                Member *m = (Member *)arena_calloc(g_arena, 1, sizeof(Member));
                m->name = d->name;
                m->type = d->type;
                m->loc = d->loc;
                m->next = NULL;
                /* Duplicate member check. */
                for (Member *q = head; q; q = q->next) {
                    if (strcmp(q->name, m->name) == 0)
                        error_at(d->loc, "duplicate member '%s'", m->name);
                }
                if (!head)
                    head = tail = m;
                else {
                    tail->next = m;
                    tail = m;
                }
                st->num_members++;
                /* Flexible array member `T x[]` allowed only last; mark
                 * size 0. Our array_of with len -1 handles it. */
            }
            if (consume(p, TK_COMMA))
                continue;
            break;
        }
        expect(p, TK_SEMI, "';'");
    }
    expect(p, TK_RBRACE, "'}'");
    p->scope = saved;
    st->members = head;
    if (struct_size_align(st) != 0)
        error_at(loc, "struct/union has incomplete member");
    return st;
}

static Type *parse_enum(Parser *p, Loc loc) {
    char *tag = NULL;
    if (at(p, TK_IDENT)) {
        Token *t = next(p);
        tag = t->sval;
    }
    Type *et = new_type(TY_ENUM);
    et->size = 4;
    et->align = 4;
    et->tag = tag;
    et->is_complete = 1;
    if (!at(p, TK_LBRACE)) {
        if (!tag)
            error_at(loc, "expected enum tag or '{'");
        Tag *tg = scope_find_tag(p->scope, tag);
        if (tg && tg->type->kind == TY_ENUM)
            return tg->type;
        Tag *nt = (Tag *)arena_calloc(g_arena, 1, sizeof(Tag));
        nt->name = tag;
        nt->type = et;
        scope_add_tag(p->scope, nt);
        return et;
    }
    next(p); /* { */
    if (tag) {
        Tag *tg = scope_find_tag(p->scope, tag);
        if (tg && tg->type->kind == TY_ENUM && tg->type->is_complete &&
            tg->type->num_members)
            error_at(loc, "redefinition of enum '%s'", tag);
        Tag *nt = (Tag *)arena_calloc(g_arena, 1, sizeof(Tag));
        nt->name = tag;
        nt->type = et;
        scope_add_tag(p->scope, nt);
    }
    long long nextval = 0;
    int count = 0;
    if (!at(p, TK_RBRACE)) {
        for (;;) {
            if (!at(p, TK_IDENT))
                error_at(cur(p)->loc, "expected enumerator");
            Token *t = next(p);
            long long v = nextval;
            if (consume(p, TK_ASSIGN)) {
                Node *e = parse_assign(p);
                int ok = 0;
                v = eval_const(p, e, &ok);
                if (!ok)
                    error_at(t->loc, "enumerator value must be constant");
            }
            if (scope_find_in(p->scope, t->sval))
                error_at(t->loc, "redefinition of '%s'", t->sval);
            Decl *d = (Decl *)arena_calloc(g_arena, 1, sizeof(Decl));
            d->name = t->sval;
            d->loc = t->loc;
            d->type = ty_int;
            d->is_enumconst = 1;
            d->enum_val = v;
            scope_add_var(p->scope, d);
            /* Enum constants need function/global visibility in sema,
             * which rebuilds scopes from top-level nodes only. Record
             * them in a side list (sema registers them globally). */
            if (nenum_consts == cap_enum_consts) {
                int ncap = cap_enum_consts ? cap_enum_consts * 2 : 32;
                Decl **nl = (Decl **)arena_alloc(
                    g_arena, (size_t)ncap * sizeof(Decl *));
                for (int k = 0; k < nenum_consts; k++)
                    nl[k] = enum_consts[k];
                enum_consts = nl;
                cap_enum_consts = ncap;
            }
            enum_consts[nenum_consts++] = d;
            nextval = v + 1;
            count++;
            if (consume(p, TK_COMMA)) {
                if (at(p, TK_RBRACE))
                    break;
                continue;
            }
            break;
        }
    }
    expect(p, TK_RBRACE, "'}'");
    et->num_members = count;
    return et;
}

/* Evaluate an initializer/constant expression to a number if possible.
 * Returns value; *ok=1 on success. Handles literals, enum consts,
 * sizeof, casts, arithmetic, ternary, parens. */
static long long eval_const(Parser *p, Node *n, int *ok) {
    *ok = 0;
    if (!n)
        return 0;
    switch (n->kind) {
    case ND_NUM:
        *ok = 1;
        return n->ival;
    case ND_VAR: {
        if (n->name) {
            Decl *d = scope_find_var(p->scope, n->name);
            if (d && d->is_enumconst) {
                *ok = 1;
                return d->enum_val;
            }
        }
        return 0;
    }
    case ND_UNARY: {
        int o = 0;
        long long v = eval_const(p, n->lhs, &o);
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
        long long l = eval_const(p, n->lhs, &a);
        long long r = eval_const(p, n->rhs, &b);
        if (!a || !b)
            return 0;
        *ok = 1;
        switch (n->op) {
        case '+': return l + r;
        case '-': return l - r;
        case '*': return l * r;
        case '/': return r ? l / r : 0;
        case '%': return r ? l % r : 0;
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
        case 'A': return l && r;
        case 'O': return l || r;
        default: *ok = 0; return 0;
        }
    }
    case ND_TERNARY: {
        int a = 0;
        long long c = eval_const(p, n->cond, &a);
        if (!a)
            return 0;
        return eval_const(p, c ? n->then : n->els, ok);
    }
    case ND_SIZEOF: {
        if (n->sz_type) {
            *ok = 1;
            return type_size(n->sz_type);
        }
        /* sizeof expr: only const if expr const? approximate with type
         * unknown here; fail so caller reports. */
        return 0;
    }
    case ND_CAST: {
        int a = 0;
        long long v = eval_const(p, n->lhs, &a);
        if (!a)
            return 0;
        *ok = 1;
        /* Truncate to target. */
        if (n->sz_type) {
            int sz = type_size(n->sz_type);
            if (sz == 1)
                v = (long long)(signed char)v;
            else if (sz == 2)
                v = (long long)(short)v;
            else if (sz == 4)
                v = (long long)(int)v;
        }
        return v;
    }
    default:
        return 0;
    }
}

/* ---------- declarators ----------
 *
 * C declarator rule used here: prefix `*`s bind to the base type first,
 * then direct-declarator suffixes (`[]`, `()`) wrap outside — EXCEPT a
 * suffix following a parenthesized group pushes INSIDE a pointer from
 * that group (`int (*fp)(int)` is pointer-to-function, and `int *a[3]`
 * is array-of-pointer). Validity (no function returning array/function)
 * is enforced when wrapping.
 */
static Decl *parse_direct(Parser *p, Type *base, char **out_name, Loc *out_loc);

/* Parse the `[N]` length (after `[` consumed); -1 = `[]`. */
static int parse_array_len(Parser *p) {
    int len = -1;
    if (!at(p, TK_RBRACKET)) {
        /* `static`/qualifiers inside []: skip. */
        while (at(p, TK_IDENT) || at(p, TK_CONST) || at(p, TK_STATIC))
            next(p);
        if (!at(p, TK_RBRACKET)) {
            Node *e = parse_assign(p);
            int ok = 0;
            long long v = eval_const(p, e, &ok);
            if (!ok)
                error_at(cur(p)->loc, "array size must be a constant");
            if (v < 0)
                error_at(cur(p)->loc, "negative array size");
            len = (int)v;
        }
    }
    expect(p, TK_RBRACKET, "']'");
    return len;
}

/* Parse `( params )` (after `(` consumed) into ft (ret already set). */
static void parse_param_list(Parser *p, Type *ft) {
    ft->params = NULL;
    ft->num_params = 0;
    ft->is_variadic = 0;
    if (at(p, TK_RPAREN))
        return; /* `()` = unspecified */
    if (at(p, TK_VOID) && peek(p, 1)->kind == TK_RPAREN) {
        next(p); /* `(void)` = no params */
        return;
    }
    int cap = 4, n = 0;
    Type **pts = (Type **)arena_alloc(g_arena, (size_t)cap * 8);
    for (;;) {
        if (at(p, TK_ELLIPSIS)) {
            next(p);
            ft->is_variadic = 1;
            break;
        }
        DeclSpec pdspec = parse_declspec(p);
        if (!pdspec.has_base)
            error_at(cur(p)->loc, "expected parameter declaration");
        Type *pt = pdspec.base;
        if (!at(p, TK_COMMA) && !at(p, TK_RPAREN) && !at(p, TK_ELLIPSIS) &&
            (at(p, TK_IDENT) || at(p, TK_STAR) || at(p, TK_LPAREN) ||
             at(p, TK_LBRACKET))) {
            Decl *pd = parse_declarator(p, pt);
            pt = pd->type;
        }
        /* array-to-pointer + function-to-pointer decay */
        if (pt->kind == TY_ARRAY)
            pt = ptr_to(pt->base);
        else if (pt->kind == TY_FUNC)
            pt = ptr_to(pt);
        if (n == cap) {
            cap *= 2;
            Type **n2 = (Type **)arena_alloc(g_arena, (size_t)cap * 8);
            memcpy(n2, pts, (size_t)n * 8);
            pts = n2;
        }
        pts[n++] = pt;
        if (at(p, TK_COMMA)) {
            next(p);
            if (at(p, TK_ELLIPSIS)) {
                next(p);
                ft->is_variadic = 1;
                break;
            }
            continue;
        }
        break;
    }
    ft->params = pts;
    ft->num_params = n;
}

/* Wrap `t` in an array. inside_paren: push through paren-group pointers. */
static Type *apply_array(Type *t, int len, int inside_paren, Loc loc) {
    if (inside_paren && t->kind == TY_PTR)
        return ptr_to(apply_array(t->base, len, 1, loc));
    if (t->kind == TY_FUNC)
        error_at(loc, "array of functions is not allowed");
    return array_of(t, len);
}

/* Wrap `ret` in a function type using an already-parsed shell.
 * inside_paren: descend through paren-group pointers first. */
static Type *wrap_func(Type *ret, Type *shell, int inside_paren, Loc loc) {
    if (inside_paren && ret->kind == TY_PTR)
        return ptr_to(wrap_func(ret->base, shell, 1, loc));
    if (ret->kind == TY_FUNC || ret->kind == TY_ARRAY)
        error_at(loc, "function returning array or function is not allowed");
    Type *ft = func_type(ret);
    ft->params = shell->params;
    ft->num_params = shell->num_params;
    ft->is_variadic = shell->is_variadic;
    return ft;
}

/* Parse `( params )` after `(` and wrap `ret` in the function type. */
static Type *apply_func(Parser *p, Type *ret, int inside_paren, Loc loc) {
    Type *shell = func_type(NULL);
    parse_param_list(p, shell);
    expect(p, TK_RPAREN, "')'");
    return wrap_func(ret, shell, inside_paren, loc);
}

/* Parse direct declarator given base; handles ident/paren + suffix loop.
 * Suffixes after a parenthesized group push inside group pointers. */
static Decl *parse_direct(Parser *p, Type *base, char **out_name, Loc *out_loc) {
    char *name = NULL;
    Loc loc = cur(p)->loc;
    Type *t = base;
    int from_paren = 0;
    if (at(p, TK_IDENT)) {
        Token *tok = next(p);
        name = tok->sval;
        loc = tok->loc;
    } else if (at(p, TK_LPAREN) && peek(p, 1)->kind != TK_RPAREN) {
        next(p);
        if (at(p, TK_STAR) || at(p, TK_IDENT) || at(p, TK_LPAREN)) {
            Decl *inner = parse_declarator(p, base);
            t = inner->type;
            name = inner->name;
            loc = inner->loc;
        }
        /* else: abstract group; treat as empty. */
        expect(p, TK_RPAREN, "')'");
        from_paren = 1;
    } else if (at(p, TK_LPAREN) && peek(p, 1)->kind == TK_RPAREN) {
        /* `()` here is a function suffix; fall through to suffix loop. */
    } else {
        /* Abstract declarator (param `int`, cast target): t = base. */
    }
    /* Suffixes (`[]` / `()`). */
    for (;;) {
        if (at(p, TK_LBRACKET)) {
            Loc l = next(p)->loc;
            int len = parse_array_len(p);
            t = apply_array(t, len, from_paren, l);
        } else if (at(p, TK_LPAREN)) {
            Loc l = next(p)->loc;
            t = apply_func(p, t, from_paren, l);
        } else {
            break;
        }
    }
    Decl *d = (Decl *)arena_calloc(g_arena, 1, sizeof(Decl));
    d->name = name;
    d->loc = loc;
    d->type = t;
    if (out_name)
        *out_name = name;
    if (out_loc)
        *out_loc = loc;
    return d;
}

/* (Function definitions keep param names via parse_function_def below.) */

/* ---------- expressions ---------- */
static Node *parse_comma(Parser *p);
static Node *parse_unary(Parser *p);
static Node *parse_postfix(Parser *p);
static Node *parse_primary(Parser *p);

/* Peek whether `(` starts a type name (cast/sizeof/compound literal). */
static int paren_is_type(Parser *p) {
    /* p at '('. Look at token after '('. */
    Token *t = peek(p, 1);
    switch (t->kind) {
    case TK_VOID:
    case TK_CHAR:
    case TK_SHORT:
    case TK_INT:
    case TK_LONG:
    case TK_SIGNED:
    case TK_UNSIGNED:
    case TK_STRUCT:
    case TK_UNION:
    case TK_ENUM:
    case TK_BOOL:
    case TK_CONST:
    case TK_VOLATILE:
        return 1;
    case TK_IDENT:
        return is_typedef_name(p, t->sval);
    default:
        return 0;
    }
}

static Node *parse_expr(Parser *p) {
    return parse_comma(p);
}

static Node *parse_comma(Parser *p) {
    Node *n = parse_assign(p);
    while (at(p, TK_COMMA)) {
        Loc l = next(p)->loc;
        Node *r = parse_assign(p);
        Node *c = new_node(ND_COMMA, l);
        c->lhs = n;
        c->rhs = r;
        n = c;
    }
    return n;
}

static Node *parse_assign(Parser *p) {
    /* assign -> ternary (right-assoc). */
    Node *n = parse_ternary(p);
    Token *t = cur(p);
    int op = 0;
    switch (t->kind) {
    case TK_ASSIGN: op = '='; break;
    case TK_PLUSEQ: op = '+'; break;
    case TK_MINUSEQ: op = '-'; break;
    case TK_STAREQ: op = '*'; break;
    case TK_SLASHEQ: op = '/'; break;
    case TK_PERCEQ: op = '%'; break;
    case TK_AMPEQ: op = '&'; break;
    case TK_PIPEEQ: op = '|'; break;
    case TK_CARETEQ: op = '^'; break;
    case TK_LSHIFTEQ: op = 'L'; break;
    case TK_RSHIFTEQ: op = 'R'; break;
    default: return n;
    }
    next(p);
    Node *r = parse_assign(p); /* right-assoc */
    if (op == '=') {
        Node *a = new_node(ND_ASSIGN, t->loc);
        a->lhs = n;
        a->rhs = r;
        return a;
    }
    Node *a = new_node(ND_COMPOUND, t->loc);
    a->op = op;
    a->lhs = n;
    a->rhs = r;
    return a;
}

static Node *parse_ternary(Parser *p) {
    Node *n = parse_lor(p);
    if (at(p, TK_QUESTION)) {
        Loc l = next(p)->loc;
        Node *t = parse_expr(p);
        expect(p, TK_COLON, "':'");
        Node *e = parse_ternary(p);
        Node *c = new_node(ND_TERNARY, l);
        c->cond = n;
        c->then = t;
        c->els = e;
        return c;
    }
    return n;
}

static Node *parse_bin(Parser *p, Node *(*next_level)(Parser *),
                       TokKind *ops, int *map, int nops) {
    Node *n = next_level(p);
    for (;;) {
        int found = -1;
        for (int i = 0; i < nops; i++) {
            if (at(p, ops[i])) {
                found = i;
                break;
            }
        }
        if (found < 0)
            return n;
        Loc l = next(p)->loc;
        Node *r = next_level(p);
        Node *b = new_node(ND_BINARY, l);
        b->op = map[found];
        b->lhs = n;
        b->rhs = r;
        n = b;
    }
}

static Node *parse_lor(Parser *p) {
    Node *n = parse_land(p);
    while (at(p, TK_OROR)) {
        Loc l = next(p)->loc;
        Node *r = parse_land(p);
        Node *b = new_node(ND_BINARY, l);
        b->op = 'O';
        b->lhs = n;
        b->rhs = r;
        n = b;
    }
    return n;
}

static Node *parse_land(Parser *p) {
    Node *n = parse_bitor(p);
    while (at(p, TK_ANDAND)) {
        Loc l = next(p)->loc;
        Node *r = parse_bitor(p);
        Node *b = new_node(ND_BINARY, l);
        b->op = 'A';
        b->lhs = n;
        b->rhs = r;
        n = b;
    }
    return n;
}

static Node *parse_bitor(Parser *p) {
    TokKind ops[] = {TK_OR};
    int map[] = {'|'};
    return parse_bin(p, parse_bitxor, ops, map, 1);
}

static Node *parse_bitxor(Parser *p) {
    TokKind ops[] = {TK_CARET};
    int map[] = {'^'};
    return parse_bin(p, parse_bitand, ops, map, 1);
}

static Node *parse_bitand(Parser *p) {
    TokKind ops[] = {TK_AND};
    int map[] = {'&'};
    return parse_bin(p, parse_equality, ops, map, 1);
}

static Node *parse_equality(Parser *p) {
    TokKind ops[] = {TK_EQEQ, TK_NOTEQ};
    int map[] = {'E', 'N'};
    return parse_bin(p, parse_relational, ops, map, 2);
}

static Node *parse_relational(Parser *p) {
    TokKind ops[] = {TK_LT, TK_GT, TK_LTEQ, TK_GTEQ};
    int map[] = {'<', '>', 'l', 'g'};
    return parse_bin(p, parse_shift, ops, map, 4);
}

static Node *parse_shift(Parser *p) {
    TokKind ops[] = {TK_LSHIFT, TK_RSHIFT};
    int map[] = {'L', 'R'};
    return parse_bin(p, parse_add, ops, map, 2);
}

static Node *parse_add(Parser *p) {
    TokKind ops[] = {TK_PLUS, TK_MINUS};
    int map[] = {'+', '-'};
    return parse_bin(p, parse_mul, ops, map, 2);
}

static Node *parse_mul(Parser *p) {
    TokKind ops[] = {TK_STAR, TK_SLASH, TK_PERCENT};
    int map[] = {'*', '/', '%'};
    return parse_bin(p, parse_cast, ops, map, 3);
}

static Node *parse_cast(Parser *p) {
    if (at(p, TK_LPAREN) && paren_is_type(p)) {
        Loc l = cur(p)->loc;
        next(p);
        Type *t = parse_typename(p);
        expect(p, TK_RPAREN, "')'");
        /* `{` after cast = compound literal: unsupported. */
        if (at(p, TK_LBRACE))
            error_at(l, "compound literals are not supported");
        Node *e = parse_cast(p);
        Node *c = new_node(ND_CAST, l);
        c->sz_type = t;
        c->lhs = e;
        return c;
    }
    return parse_unary(p);
}

static Node *parse_unary(Parser *p) {
    Token *t = cur(p);
    switch (t->kind) {
    case TK_PLUSPLUS: {
        next(p);
        Node *e = parse_unary(p);
        Node *n = new_node(ND_UNARY, t->loc);
        n->op = 'P';
        n->lhs = e;
        return n;
    }
    case TK_MINUSMINUS: {
        next(p);
        Node *e = parse_unary(p);
        Node *n = new_node(ND_UNARY, t->loc);
        n->op = 'M';
        n->lhs = e;
        return n;
    }
    case TK_AND: {
        next(p);
        Node *e = parse_cast(p);
        Node *n = new_node(ND_UNARY, t->loc);
        n->op = '&';
        n->lhs = e;
        return n;
    }
    case TK_STAR: {
        next(p);
        Node *e = parse_cast(p);
        Node *n = new_node(ND_UNARY, t->loc);
        n->op = '*';
        n->lhs = e;
        return n;
    }
    case TK_PLUS: {
        next(p);
        Node *e = parse_cast(p);
        Node *n = new_node(ND_UNARY, t->loc);
        n->op = '+';
        n->lhs = e;
        return n;
    }
    case TK_MINUS: {
        next(p);
        Node *e = parse_cast(p);
        Node *n = new_node(ND_UNARY, t->loc);
        n->op = '-';
        n->lhs = e;
        return n;
    }
    case TK_TILDE: {
        next(p);
        Node *e = parse_cast(p);
        Node *n = new_node(ND_UNARY, t->loc);
        n->op = '~';
        n->lhs = e;
        return n;
    }
    case TK_BANG: {
        next(p);
        Node *e = parse_cast(p);
        Node *n = new_node(ND_UNARY, t->loc);
        n->op = '!';
        n->lhs = e;
        return n;
    }
    case TK_SIZEOF: {
        next(p);
        Node *n = new_node(ND_SIZEOF, t->loc);
        if (at(p, TK_LPAREN) && paren_is_type(p)) {
            /* Lookahead: parse type, but `sizeof (expr)` with typedef-free
             * expr handled by paren_is_type. */
            next(p);
            Type *ty = parse_typename(p);
            expect(p, TK_RPAREN, "')'");
            n->sz_type = ty;
            n->lhs = NULL;
            return n;
        }
        if (at(p, TK_LPAREN)) {
            /* Could still be `sizeof(type)` where type starts with ident
             * typedef but paren check said no... fall to expr. */
        }
        /* `sizeof expr` or `sizeof (expr)`: parse unary to avoid
         * consuming binary? Standard says unary-expr. Use cast level. */
        n->lhs = parse_unary(p);
        return n;
    }
    default:
        return parse_postfix(p);
    }
}

static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);
    for (;;) {
        if (at(p, TK_LBRACKET)) {
            Loc l = next(p)->loc;
            Node *idx = parse_expr(p);
            expect(p, TK_RBRACKET, "']'");
            Node *s = new_node(ND_SUBSCRIPT, l);
            s->lhs = n;
            s->rhs = idx;
            n = s;
        } else if (at(p, TK_LPAREN)) {
            Loc l = next(p)->loc;
            Node *c = new_node(ND_CALL, l);
            c->lhs = n;
            c->nargs = 0;
            c->args = NULL;
            if (!at(p, TK_RPAREN)) {
                for (;;) {
                    Node *a = parse_assign(p);
                    call_push_arg(c, a);
                    if (at(p, TK_COMMA)) {
                        next(p);
                        continue;
                    }
                    break;
                }
            }
            expect(p, TK_RPAREN, "')'");
            n = c;
        } else if (at(p, TK_DOT)) {
            Loc l = next(p)->loc;
            if (!at(p, TK_IDENT))
                error_at(cur(p)->loc, "expected member name after '.'");
            Token *m = next(p);
            Node *e = new_node(ND_MEMBER, l);
            e->lhs = n;
            e->name = m->sval;
            e->is_arrow = 0;
            n = e;
        } else if (at(p, TK_ARROW)) {
            Loc l = next(p)->loc;
            if (!at(p, TK_IDENT))
                error_at(cur(p)->loc, "expected member name after '->'");
            Token *m = next(p);
            Node *e = new_node(ND_MEMBER, l);
            e->lhs = n;
            e->name = m->sval;
            e->is_arrow = 1;
            n = e;
        } else if (at(p, TK_PLUSPLUS)) {
            Loc l = next(p)->loc;
            Node *e = new_node(ND_UNARY, l);
            e->op = 'p';
            e->lhs = n;
            n = e;
        } else if (at(p, TK_MINUSMINUS)) {
            Loc l = next(p)->loc;
            Node *e = new_node(ND_UNARY, l);
            e->op = 'm';
            e->lhs = n;
            n = e;
        } else
            break;
    }
    return n;
}

static Node *parse_primary(Parser *p) {
    Token *t = cur(p);
    switch (t->kind) {
    case TK_NUM: {
        next(p);
        Node *n = new_node(ND_NUM, t->loc);
        n->ival = t->ival;
        if (t->is_long || t->ival > 2147483647LL ||
            (t->is_unsigned && t->ival > 4294967295LL))
            n->ty = t->is_unsigned ? ty_ulong : ty_long;
        else
            n->ty = t->is_unsigned ? ty_uint : ty_int;
        if (!t->is_long && !t->is_unsigned && t->ival <= 2147483647LL)
            n->ty = ty_int;
        return n;
    }
    case TK_CHARLIT: {
        next(p);
        Node *n = new_node(ND_NUM, t->loc);
        n->ival = t->ival;
        n->ty = ty_int;
        return n;
    }
    case TK_STR: {
        next(p);
        Node *n = new_node(ND_STR, t->loc);
        n->sval = t->sval;
        n->slen = t->slen;
        n->ty = array_of(ty_char, (int)t->slen + 1);
        return n;
    }
    case TK_IDENT: {
        next(p);
        Node *n = new_node(ND_VAR, t->loc);
        n->name = t->sval;
        n->decl = NULL;
        return n;
    }
    case TK_LPAREN: {
        Loc l = next(p)->loc;
        (void)l;
        Node *e = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        return e;
    }
    default:
        error_at(t->loc, "expected expression, found '%s'",
                 tokkind_name(t->kind));
        return NULL;
    }
}

/* Type name: specifier-qualifier list + abstract declarator. */
static Type *parse_typename(Parser *p) {
    DeclSpec ds = parse_declspec(p);
    if (!ds.has_base)
        error_at(cur(p)->loc, "expected type name");
    Type *t = ds.base;
    /* abstract declarator: pointers + suffixes */
    int nptr = 0;
    while (at(p, TK_STAR)) {
        next(p);
        while (at(p, TK_CONST) || at(p, TK_VOLATILE))
            next(p);
        if (at(p, TK_IDENT) && strcmp(cur(p)->sval, "restrict") == 0)
            next(p);
        nptr++;
    }
    if (at(p, TK_LPAREN) || at(p, TK_LBRACKET) || at(p, TK_IDENT)) {
        Decl *d = parse_declarator(p, t);
        t = d->type;
    }
    for (int i = 0; i < nptr; i++)
        t = ptr_to(t);
    /* `void` adjustment etc. in sema. */
    return t;
}

/* Initializer: assignment expr or braced list. */
static Node *parse_initializer(Parser *p) {
    if (at(p, TK_LBRACE)) {
        Loc l = next(p)->loc;
        Node *list = new_node(ND_INIT_LIST, l);
        list->stmts = NULL;
        list->nstmts = 0;
        list->cap_stmts = 0;
        if (!at(p, TK_RBRACE)) {
            for (;;) {
                Node *e = parse_initializer(p);
                initlist_push(list, e);
                if (at(p, TK_COMMA)) {
                    next(p);
                    if (at(p, TK_RBRACE))
                        break;
                    continue;
                }
                break;
            }
        }
        expect(p, TK_RBRACE, "'}'");
        return list;
    }
    return parse_assign(p);
}

/* ---------- statements ---------- */
static Node *parse_decl_stmt(Parser *p, DeclSpec *ds);

static int stmt_is_declstart(Parser *p) {
    Token *t = cur(p);
    if (!is_declspec_tok(p, t))
        return 0;
    /* `typedef` alone always decl. Type keywords always decl... except
     * expression statements starting with e.g. `*p = 1`? Those start with
     * `*`, not declspec. Ambiguity: `x = 1` where x is typedef? Can't be
     * both var and typedef. If IDENT is typedef name, treat as decl. */
    return 1;
}

static Node *parse_stmt(Parser *p) {
    Token *t = cur(p);
    switch (t->kind) {
    case TK_LBRACE:
        return parse_block(p);
    case TK_IF: {
        Loc l = next(p)->loc;
        expect(p, TK_LPAREN, "'('");
        Node *c = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        Node *th = parse_stmt(p);
        Node *el = NULL;
        if (at(p, TK_ELSE)) {
            next(p);
            el = parse_stmt(p);
        }
        Node *n = new_node(ND_IF, l);
        n->cond = c;
        n->then = th;
        n->els = el;
        return n;
    }
    case TK_WHILE: {
        Loc l = next(p)->loc;
        expect(p, TK_LPAREN, "'('");
        Node *c = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        Node *b = parse_stmt(p);
        Node *n = new_node(ND_WHILE, l);
        n->cond = c;
        n->body = b;
        return n;
    }
    case TK_DO: {
        Loc l = next(p)->loc;
        Node *b = parse_stmt(p);
        expect(p, TK_WHILE, "'while'");
        expect(p, TK_LPAREN, "'('");
        Node *c = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        expect(p, TK_SEMI, "';'");
        Node *n = new_node(ND_DO, l);
        n->body = b;
        n->cond = c;
        return n;
    }
    case TK_FOR: {
        Loc l = next(p)->loc;
        expect(p, TK_LPAREN, "'('");
        Scope *saved = p->scope;
        p->scope = new_scope(p, saved);
        Node *init = NULL;
        if (at(p, TK_SEMI)) {
            next(p);
        } else if (stmt_is_declstart(p)) {
            DeclSpec ds = parse_declspec(p);
            init = parse_decl_stmt(p, &ds);
            /* parse_decl_stmt returns BLOCK of vardecls; keep as init. */
        } else {
            Node *e = parse_expr(p);
            expect(p, TK_SEMI, "';'");
            Node *es = new_node(ND_EXPR_STMT, l);
            es->lhs = e;
            init = es;
        }
        Node *cond = NULL, *inc = NULL;
        if (!at(p, TK_SEMI)) {
            cond = parse_expr(p);
        }
        expect(p, TK_SEMI, "';'");
        if (!at(p, TK_RPAREN)) {
            inc = parse_expr(p);
        }
        expect(p, TK_RPAREN, "')'");
        Node *body = parse_stmt(p);
        p->scope = saved;
        Node *n = new_node(ND_FOR, l);
        n->init = init;
        n->cond = cond;
        n->inc = inc;
        n->body = body;
        return n;
    }
    case TK_RETURN: {
        Loc l = next(p)->loc;
        Node *e = NULL;
        if (!at(p, TK_SEMI))
            e = parse_expr(p);
        expect(p, TK_SEMI, "';'");
        Node *n = new_node(ND_RETURN, l);
        n->lhs = e;
        return n;
    }
    case TK_BREAK: {
        Loc l = next(p)->loc;
        expect(p, TK_SEMI, "';'");
        return new_node(ND_BREAK, l);
    }
    case TK_CONTINUE: {
        Loc l = next(p)->loc;
        expect(p, TK_SEMI, "';'");
        return new_node(ND_CONTINUE, l);
    }
    case TK_SWITCH: {
        Loc l = next(p)->loc;
        expect(p, TK_LPAREN, "'('");
        Node *c = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        Node *b = parse_stmt(p);
        Node *n = new_node(ND_SWITCH, l);
        n->cond = c;
        n->body = b;
        return n;
    }
    case TK_CASE: {
        Loc l = next(p)->loc;
        Node *e = parse_assign(p);
        /* Defer const eval to sema; store raw expr in lhs, val later. */
        expect(p, TK_COLON, "':'");
        Node *body = parse_stmt(p);
        Node *n = new_node(ND_CASE, l);
        n->lhs = e;
        n->is_default = 0;
        n->stmts = NULL;
        n->nstmts = 0;
        n->cap_stmts = 0;
        block_push(n, body);
        return n;
    }
    case TK_DEFAULT: {
        Loc l = next(p)->loc;
        expect(p, TK_COLON, "':'");
        Node *body = parse_stmt(p);
        Node *n = new_node(ND_CASE, l);
        n->is_default = 1;
        n->stmts = NULL;
        n->nstmts = 0;
        n->cap_stmts = 0;
        block_push(n, body);
        return n;
    }
    case TK_GOTO: {
        Loc l = next(p)->loc;
        if (!at(p, TK_IDENT))
            error_at(cur(p)->loc, "expected label after goto");
        Token *id = next(p);
        expect(p, TK_SEMI, "';'");
        Node *n = new_node(ND_GOTO, l);
        n->name = id->sval;
        return n;
    }
    case TK_SEMI: {
        Loc l = next(p)->loc;
        return new_node(ND_NOP, l);
    }
    case TK_IDENT: {
        /* label `name:` ? */
        if (peek(p, 1)->kind == TK_COLON) {
            Token *id = next(p);
            next(p);
            Node *body = parse_stmt(p);
            Node *n = new_node(ND_LABEL, id->loc);
            n->name = id->sval;
            n->body = body;
            return n;
        }
        break;
    }
    default:
        break;
    }
    if (stmt_is_declstart(p)) {
        DeclSpec ds = parse_declspec(p);
        if (at(p, TK_SEMI) && ds.has_base &&
            (ds.base->kind == TY_STRUCT || ds.base->kind == TY_UNION ||
             ds.base->kind == TY_ENUM)) {
            /* Bare `struct S;` / `enum E;` declaration: no declarators. */
            next(p);
            return new_node(ND_NOP, t->loc);
        }
        Node *blk = parse_decl_stmt(p, &ds);
        return blk;
    }
    /* Expression statement. */
    {
        Node *e = parse_expr(p);
        expect(p, TK_SEMI, "';'");
        Node *n = new_node(ND_EXPR_STMT, t->loc);
        n->lhs = e;
        return n;
    }
}

/* Parse `init-declarator-list ;` given declspec. Returns BLOCK of VARDECLs
 * (or single VARDECL). Registers decls in current scope (unless typedef). */
static Node *parse_decl_stmt(Parser *p, DeclSpec *ds) {
    if (!ds->has_base) {
        /* Implicit int is not supported; require specifiers. */
        error_at(cur(p)->loc, "expected declaration specifiers");
    }
    Node *blk = new_node(ND_BLOCK, cur(p)->loc);
    blk->stmts = NULL;
    blk->nstmts = 0;
    blk->cap_stmts = 0;
    if (at(p, TK_SEMI)) {
        /* `int;` — allowed, no-op. */
        next(p);
        return blk;
    }
    for (;;) {
        /* Skip `__attribute__((...))` / `_Alignas` etc. gracefully. */
        while (at(p, TK_IDENT) &&
               (strcmp(cur(p)->sval, "__attribute__") == 0 ||
                strcmp(cur(p)->sval, "__attribute") == 0 ||
                strcmp(cur(p)->sval, "_Alignas") == 0 ||
                strcmp(cur(p)->sval, "__alignof__") == 0)) {
            Loc al = next(p)->loc;
            (void)al;
            if (at(p, TK_LPAREN)) {
                int depth = 0;
                do {
                    if (at(p, TK_LPAREN))
                        depth++;
                    else if (at(p, TK_RPAREN))
                        depth--;
                    next(p);
                } while (depth > 0 && !at(p, TK_EOF));
            }
        }
        Decl *d = parse_declarator(p, ds->base);
        d->is_typedef = (ds->storage & ST_TYPEDEF) ? 1 : 0;
        d->is_static = (ds->storage & ST_STATIC) ? 1 : 0;
        d->is_extern = (ds->storage & ST_EXTERN) ? 1 : 0;
        if (d->is_typedef && d->name == NULL)
            error_at(d->loc, "typedef requires a name");
        if (!d->is_typedef && d->name == NULL)
            error_at(d->loc, "declarator requires a name");
        if (at(p, TK_ASSIGN)) {
            if (d->type->kind == TY_FUNC)
                error_at(d->loc, "cannot initialize function");
            next(p);
            d->init = parse_initializer(p);
        }
        if (d->is_typedef) {
            if (scope_find_in(p->scope, d->name))
                error_at(d->loc, "redefinition of '%s'", d->name);
            scope_add_var(p->scope, d);
            Node *vd = new_node(ND_VARDECL, d->loc);
            vd->decl = d;
            block_push(blk, vd);
        } else if (d->type->kind == TY_FUNC) {
            /* Prototype at any scope: register global. */
            Decl *ex = scope_find_var(p->gscope, d->name);
            if (!ex) {
                Decl *g = (Decl *)arena_calloc(g_arena, 1, sizeof(Decl));
                *g = *d;
                g->is_global = 1;
                scope_add_var(p->gscope, g);
                Node *vd = new_node(ND_VARDECL, d->loc);
                vd->decl = g;
                emit_top(vd);
            }
            Node *vd = new_node(ND_VARDECL, d->loc);
            vd->decl = d;
            block_push(blk, vd);
        } else {
            int is_global = (p->scope == p->gscope);
            d->is_global = is_global;
            if (!is_global && d->is_extern) {
                /* `extern` local: refers to global. */
                Decl *g = scope_find_var(p->gscope, d->name);
                if (!g) {
                    g = (Decl *)arena_calloc(g_arena, 1, sizeof(Decl));
                    *g = *d;
                    g->is_global = 1;
                    scope_add_var(p->gscope, g);
                }
                scope_add_var(p->scope, d);
            } else {
                if (scope_find_in(p->scope, d->name))
                    error_at(d->loc, "redefinition of '%s'", d->name);
                scope_add_var(p->scope, d);
            }
            Node *vd = new_node(ND_VARDECL, d->loc);
            vd->decl = d;
            block_push(blk, vd);
        }
        if (at(p, TK_COMMA)) {
            next(p);
            continue;
        }
        break;
    }
    expect(p, TK_SEMI, "';'");
    if (blk->nstmts == 1 && !blk->is_decls)
        return blk->stmts[0];
    blk->is_decls = 1;
    return blk;
}

static Node *parse_block(Parser *p) {
    Loc l = expect(p, TK_LBRACE, "'{'")->loc;
    Node *b = new_node(ND_BLOCK, l);
    Scope *saved = p->scope;
    p->scope = new_scope(p, saved);
    while (!at(p, TK_RBRACE)) {
        if (at(p, TK_EOF))
            error_at(cur(p)->loc, "unterminated block");
        Node *s = parse_stmt(p);
        if (s->kind == ND_BLOCK && s->is_decls) {
            /* Declaration list (`int x=1, y=2;`): splice into this
             * scope instead of nesting a new one. */
            for (int i = 0; i < s->nstmts; i++)
                block_push(b, s->stmts[i]);
            continue;
        }
        block_push(b, s);
    }
    expect(p, TK_RBRACE, "'}'");
    p->scope = saved;
    return b;
}

/* ---------- top level ---------- */
static void parse_external(Parser *p) {
    Token *t = cur(p);
    if (at(p, TK_SEMI)) {
        next(p);
        return;
    }
    /* Skip stray attributes at top level. */
    DeclSpec ds = parse_declspec(p);
    if (!ds.has_base)
        error_at(t->loc, "expected declaration");
    /* Bare `struct S;` / `enum E x;`? no declarators. */
    if (at(p, TK_SEMI)) {
        next(p);
        return;
    }
    /* Parse first declarator to see if it's a function definition. */
    Decl *d = parse_declarator(p, ds.base);
    d->is_typedef = (ds.storage & ST_TYPEDEF) ? 1 : 0;
    d->is_static = (ds.storage & ST_STATIC) ? 1 : 0;
    d->is_extern = (ds.storage & ST_EXTERN) ? 1 : 0;

    if (d->is_typedef) {
        if (!d->name)
            error_at(d->loc, "typedef requires a name");
        if (scope_find_in(p->scope, d->name))
            error_at(d->loc, "redefinition of '%s'", d->name);
        scope_add_var(p->scope, d);
        Node *vd = new_node(ND_VARDECL, d->loc);
        vd->decl = d;
        emit_top(vd);
        /* More declarators? `typedef int A, B;` */
        while (at(p, TK_COMMA)) {
            next(p);
            Decl *d2 = parse_declarator(p, ds.base);
            d2->is_typedef = 1;
            if (!d2->name)
                error_at(d2->loc, "typedef requires a name");
            if (scope_find_in(p->scope, d2->name))
                error_at(d2->loc, "redefinition of '%s'", d2->name);
            scope_add_var(p->scope, d2);
            Node *v2 = new_node(ND_VARDECL, d2->loc);
            v2->decl = d2;
            emit_top(v2);
        }
        expect(p, TK_SEMI, "';'");
        return;
    }

    if (d->type->kind == TY_FUNC && at(p, TK_LBRACE)) {
        /* Only reached for exotic declarators like `int (f)() {}`. */
        error_at(d->loc, "parenthesized function names are not supported");
    }

    /* Not a function definition (or prototype/global). */
    /* Could still be function definition if declarator was `(*f)(...)`?
     * No — function definitions must be direct identifier declarators.
     * If type is FUNC and name non-NULL and next is `{`, treat as def
     * (names lost issue above). Since we hit the error above, definitions
     * actually go through the dedicated path in parse_program. This
     * fallback handles globals/prototypes. */
    int is_global = 1;
    d->is_global = is_global;
    if (d->type->kind == TY_FUNC) {
        /* Prototype. */
        Decl *ex = scope_find_in(p->gscope, d->name);
        if (ex && ex->type->kind == TY_FUNC) {
            /* allow repeated prototypes */
        } else if (ex) {
            error_at(d->loc, "redefinition of '%s'", d->name);
        } else {
            scope_add_var(p->gscope, d);
        }
        Node *vd = new_node(ND_VARDECL, d->loc);
        vd->decl = d;
        emit_top(vd);
        while (at(p, TK_COMMA)) {
            next(p);
            Decl *d2 = parse_declarator(p, ds.base);
            d2->is_global = 1;
            d2->is_static = d->is_static;
            d2->is_extern = d->is_extern;
            Node *v2 = new_node(ND_VARDECL, d2->loc);
            v2->decl = d2;
            scope_add_var(p->gscope, d2);
            emit_top(v2);
        }
        expect(p, TK_SEMI, "';'");
        return;
    }
    /* Global variable(s). Tentative definitions (`int g;`) merge. */
    for (;;) {
        Decl *ex = d->name ? scope_find_in(p->gscope, d->name) : NULL;
        if (ex && ex->type->kind == TY_FUNC)
            error_at(d->loc, "redefinition of '%s'", d->name);
        if (at(p, TK_ASSIGN)) {
            next(p);
            d->init = parse_initializer(p);
        }
        if (ex) {
            if (ex->init && d->init)
                error_at(d->loc, "redefinition of '%s'", d->name);
            if (ex->type->kind != d->type->kind ||
                type_size(ex->type) != type_size(d->type))
                error_at(d->loc, "conflicting types for '%s'", d->name);
            if (d->init)
                ex->init = d->init;
            /* keep flags from the first declaration */
            Node *vd = new_node(ND_VARDECL, d->loc);
            vd->decl = ex;
            emit_top(vd);
        } else {
            scope_add_var(p->gscope, d);
            Node *vd = new_node(ND_VARDECL, d->loc);
            vd->decl = d;
            emit_top(vd);
        }
        if (at(p, TK_COMMA)) {
            next(p);
            d = parse_declarator(p, ds.base);
            d->is_global = 1;
            d->is_static = (ds.storage & ST_STATIC) ? 1 : 0;
            d->is_extern = (ds.storage & ST_EXTERN) ? 1 : 0;
            d->is_typedef = 0;
            continue;
        }
        break;
    }
    expect(p, TK_SEMI, "';'");
}

/* Dedicated function-definition parser that keeps param names. */
static void parse_function_def(Parser *p, DeclSpec *ds, size_t start_pos) {
    (void)start_pos;
    /* Re-parse declarator WITH param names. We already consumed declspec;
     * now parse pointers + name + params manually. */
    Type *base = ds->base;
    int nptr = 0;
    while (at(p, TK_STAR)) {
        next(p);
        while (at(p, TK_CONST) || at(p, TK_VOLATILE))
            next(p);
        if (at(p, TK_IDENT) && strcmp(cur(p)->sval, "restrict") == 0)
            next(p);
        nptr++;
    }
    if (!at(p, TK_IDENT))
        error_at(cur(p)->loc, "expected function name");
    Token *fname = next(p);
    /* Suffixes before params (arrays not allowed for functions). */
    if (at(p, TK_LBRACKET))
        error_at(cur(p)->loc, "function cannot return array");
    expect(p, TK_LPAREN, "'('");
    Decl **params = NULL;
    int nparams = 0, capparams = 0;
    Type **ptypes = NULL;
    int variadic = 0;
    if (!(at(p, TK_RPAREN))) {
        if (at(p, TK_VOID) && peek(p, 1)->kind == TK_RPAREN) {
            next(p);
        } else {
            for (;;) {
                if (at(p, TK_ELLIPSIS)) {
                    next(p);
                    variadic = 1;
                    break;
                }
                DeclSpec ps = parse_declspec(p);
                if (!ps.has_base)
                    error_at(cur(p)->loc, "expected parameter declaration");
                Decl *pd = NULL;
                if (!at(p, TK_COMMA) && !at(p, TK_RPAREN) &&
                    !at(p, TK_ELLIPSIS) &&
                    (at(p, TK_IDENT) || at(p, TK_STAR) ||
                     at(p, TK_LPAREN) || at(p, TK_LBRACKET))) {
                    pd = parse_declarator(p, ps.base);
                    if (pd->type->kind == TY_ARRAY)
                        pd->type = ptr_to(pd->type->base);
                    else if (pd->type->kind == TY_FUNC)
                        pd->type = ptr_to(pd->type);
                } else {
                    pd = (Decl *)arena_calloc(g_arena, 1, sizeof(Decl));
                    pd->loc = cur(p)->loc;
                    pd->type = ps.base;
                    if (pd->type->kind == TY_ARRAY)
                        pd->type = ptr_to(pd->type->base);
                }
                pd->is_param = 1;
                pd->index = nparams;
                if (nparams == capparams) {
                    capparams = capparams ? capparams * 2 : 4;
                    Decl **nn = (Decl **)arena_alloc(
                        g_arena, (size_t)capparams * sizeof(Decl *));
                    Type **tt = (Type **)arena_alloc(g_arena,
                                                     (size_t)capparams * 8);
                    if (params) {
                        memcpy(nn, params, (size_t)nparams * sizeof(Decl *));
                        memcpy(tt, ptypes, (size_t)nparams * 8);
                    }
                    params = nn;
                    ptypes = tt;
                }
                params[nparams] = pd;
                if (ptypes)
                    ptypes[nparams] = pd->type;
                nparams++;
                if (at(p, TK_COMMA)) {
                    next(p);
                    if (at(p, TK_ELLIPSIS)) {
                        next(p);
                        variadic = 1;
                        break;
                    }
                    continue;
                }
                break;
            }
        }
    }
    expect(p, TK_RPAREN, "')'");
    /* K&R style param declarations `int f(a, b) int a; ...` — reject. */
    if (at(p, TK_SEMI) || (stmt_is_declstart(p) && !at(p, TK_LBRACE)))
        error_at(cur(p)->loc, "K&R function definitions are not supported");

    Type *ret = base;
    for (int i = 0; i < nptr; i++)
        ret = ptr_to(ret);
    Type *ft = func_type(ret);
    ft->num_params = nparams;
    ft->params = ptypes;
    ft->is_variadic = variadic;

    Decl *fn = (Decl *)arena_calloc(g_arena, 1, sizeof(Decl));
    fn->name = fname->sval;
    fn->loc = fname->loc;
    fn->type = ft;
    fn->is_global = 1;
    fn->is_static = (ds->storage & ST_STATIC) ? 1 : 0;
    fn->is_extern = (ds->storage & ST_EXTERN) ? 1 : 0;

    Decl *ex = scope_find_in(p->gscope, fname->sval);
    if (ex && ex->type->kind == TY_FUNC) {
        /* merge: keep existing decl node identity? Replace type. */
        ex->type = ft;
        fn = ex;
    } else if (ex) {
        error_at(fname->loc, "redefinition of '%s'", fname->sval);
    } else {
        scope_add_var(p->gscope, fn);
    }

    /* Parse body with params in scope. */
    Scope *fscope = new_scope(p, p->gscope);
    for (int i = 0; i < nparams; i++) {
        if (params[i]->name) {
            if (scope_find_in(fscope, params[i]->name))
                error_at(params[i]->loc, "duplicate parameter '%s'",
                         params[i]->name);
            scope_add_var(fscope, params[i]);
        }
    }
    Scope *saved = p->scope;
    p->scope = fscope;
    if (!at(p, TK_LBRACE))
        error_at(cur(p)->loc, "expected function body");
    Node *body = parse_block(p);
    p->scope = saved;

    Node *fd = new_node(ND_FUNCDEF, fname->loc);
    fd->decl = fn;
    fd->body = body;
    fd->params = params;
    fd->nparams = nparams;
    emit_top(fd);
}

/* Decide whether the upcoming external decl is a function definition.
 * Heuristic without full backtracking: scan ahead for a top-level `(` ...
 * `)` followed by `{`. Handles `int f(...)`, `int *f(...)` is rejected
 * later, `unsigned long f(void)` etc. */
static int looks_like_funcdef(Parser *p) {
    size_t i = p->pos;
    /* Skip declspec tokens (incl. struct bodies!). Struct bodies contain
     * braces/semicolons — must skip balanced braces. */
    int saw_spec = 0;
    for (;;) {
        if (i >= p->len)
            return 0;
        TokKind k = p->toks[i].kind;
        if (k == TK_STRUCT || k == TK_UNION || k == TK_ENUM) {
            saw_spec = 1;
            i++;
            if (i < p->len && p->toks[i].kind == TK_IDENT)
                i++;
            if (i < p->len && p->toks[i].kind == TK_LBRACE) {
                int depth = 0;
                while (i < p->len) {
                    if (p->toks[i].kind == TK_LBRACE)
                        depth++;
                    else if (p->toks[i].kind == TK_RBRACE) {
                        depth--;
                        if (!depth) {
                            i++;
                            break;
                        }
                    }
                    i++;
                }
            }
            continue;
        }
        Token tmp;
        tmp.kind = k;
        tmp.sval = p->toks[i].sval;
        if (is_declspec_tok(p, &tmp) && k != TK_LPAREN && k != TK_STAR &&
            k != TK_IDENT) {
            saw_spec = 1;
            i++;
            continue;
        }
        if (k == TK_IDENT && is_typedef_name(p, p->toks[i].sval)) {
            saw_spec = 1;
            i++;
            continue;
        }
        /* storage/qualifiers already covered; const etc. */
        break;
    }
    if (!saw_spec)
        return 0;
    /* Skip pointers. */
    while (i < p->len && p->toks[i].kind == TK_STAR) {
        i++;
        while (i < p->len &&
               (p->toks[i].kind == TK_CONST || p->toks[i].kind == TK_VOLATILE ||
                (p->toks[i].kind == TK_IDENT &&
                 strcmp(p->toks[i].sval, "restrict") == 0)))
            i++;
    }
    /* Expect ident (function name). Allow `(` (e.g. `(f)()`)? keep simple. */
    if (i < p->len && p->toks[i].kind == TK_LPAREN) {
        /* `( *f )( ... )` function returning... not a definition. */
        return 0;
    }
    if (i >= p->len || p->toks[i].kind != TK_IDENT)
        return 0;
    i++;
    if (i >= p->len || p->toks[i].kind != TK_LPAREN)
        return 0;
    /* Skip balanced parens (params may contain nested parens/arrays). */
    int depth = 0;
    while (i < p->len) {
        if (p->toks[i].kind == TK_LPAREN)
            depth++;
        else if (p->toks[i].kind == TK_RPAREN) {
            depth--;
            if (!depth) {
                i++;
                break;
            }
        }
        i++;
    }
    return i < p->len && p->toks[i].kind == TK_LBRACE;
}

void parse_program(TokVec *tv) {
    Parser p;
    p.toks = tv->toks;
    p.len = tv->len;
    p.pos = 0;
    p.anon_id = 0;
    p.gscope = new_scope(&p, NULL);
    p.scope = p.gscope;
    plist = NULL;
    nlist = 0;
    caplist = 0;
    enum_consts = NULL;
    nenum_consts = 0;
    cap_enum_consts = 0;

    while (!at(&p, TK_EOF)) {
        if (at(&p, TK_SEMI)) {
            next(&p);
            continue;
        }
        if (looks_like_funcdef(&p)) {
            size_t start = p.pos;
            DeclSpec ds = parse_declspec(&p);
            (void)start;
            parse_function_def(&p, &ds, start);
            continue;
        }
        parse_external(&p);
    }
}
