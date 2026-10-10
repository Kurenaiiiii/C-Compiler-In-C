/* ast.h - abstract syntax tree nodes and declarations. */
#ifndef CCC_AST_H
#define CCC_AST_H

#include "common.h"
#include "types.h"

typedef enum {
    ND_NUM,        /* integer constant: ival, ty */
    ND_STR,        /* string literal: sval/slen, str_label (codegen) */
    ND_VAR,        /* variable use: decl */
    ND_CALL,       /* func(args): lhs=func expr, args */
    ND_FUNCDEF,    /* function definition: decl + body */
    ND_VARDECL,    /* declaration (also used for params/globals) */
    ND_BLOCK,      /* { stmts } */
    ND_EXPR_STMT,
    ND_IF,
    ND_WHILE,
    ND_DO,
    ND_FOR,        /* init/cond/inc/body */
    ND_RETURN,
    ND_BREAK,
    ND_CONTINUE,
    ND_SWITCH,
    ND_CASE,       /* ival + is_default; body = next stmt(s) via switch list */
    ND_LABEL,
    ND_GOTO,
    ND_ASSIGN,     /* lhs = rhs */
    ND_COMPOUND,   /* lhs op= rhs; op = '+','-','*','/','%','&','|','^','L','R' */
    ND_TERNARY,
    ND_COMMA,
    ND_UNARY,      /* op: '-','~','!','*','&','+','P'(pre++),'p'(post++),'M'(pre--),'m'(post--) */
    ND_BINARY,     /* op: '+','-','*','/','%','&','|','^','L'(<<),'R'(>>),
                      'E'(==),'N'(!=),'<','>','l'(<=),'g'(>=),'A'(&&),'O'(||) */
    ND_CAST,
    ND_SIZEOF,     /* lhs = expr or NULL; sz_type = type operand */
    ND_MEMBER,     /* lhs=object expr; member name/decl; is_arrow */
    ND_SUBSCRIPT,  /* lhs=array, rhs=index */
    ND_INIT_LIST,  /* elements for { ... } */
    ND_NOP,        /* empty statement */
} NodeKind;

typedef struct Node Node;
typedef struct Decl Decl;

struct Decl {
    char *name;
    Type *type;
    Loc loc;
    Node *init;      /* initializer expr or ND_INIT_LIST (may be NULL) */
    int is_global;
    int is_static;   /* file-static or static local */
    int is_extern;
    int is_typedef;
    int is_param;
    int is_enumconst;
    long long enum_val;
    int offset;      /* locals: rbp - offset; set by codegen layout */
    int index;       /* param index */
    char *static_label; /* mangled label for globals/static locals */
    Decl *next;      /* scope chain / param list / global list */
};

struct Node {
    NodeKind kind;
    Loc loc;
    Type *ty;        /* resolved type (sema) */
    long long ival;  /* ND_NUM, ND_CASE */
    int op;          /* ND_UNARY / ND_BINARY / ND_COMPOUND */
    /* operands */
    Node *lhs;
    Node *rhs;
    Node *cond;
    Node *then;
    Node *els;
    Node *init;      /* FOR init (stmt or expr), RETURN expr */
    Node *inc;       /* FOR inc */
    Node *body;      /* loop/switch body, FUNCDEF body */
    /* lists */
    Node **stmts;
    int nstmts, cap_stmts;
    Node **args;
    int nargs;
    Decl *decl;      /* VAR / VARDECL / FUNCDEF / LABEL name decl */
    char *name;      /* CALL func name (pre-resolve), MEMBER name, GOTO/LABEL */
    Member *member;  /* resolved struct member (ND_MEMBER) */
    int is_arrow;
    int is_default;  /* ND_CASE */
    char *sval;      /* ND_STR bytes */
    size_t slen;
    int str_label;   /* codegen string id */
    Type *sz_type;   /* ND_SIZEOF type operand / ND_CAST target */
    Decl **params;   /* FUNCDEF params */
    int nparams;
    int is_decls;    /* BLOCK came from a declaration list: splice, don't nest */
};

Node *new_node(NodeKind kind, Loc loc);
void block_push(Node *block, Node *stmt);
void call_push_arg(Node *call, Node *arg);
void initlist_push(Node *list, Node *e);

/* Debug dump (s-expression) used by --dump-ast. */
void dump_ast(FILE *fp);

#endif
