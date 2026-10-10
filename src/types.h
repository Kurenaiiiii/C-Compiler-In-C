/* types.h - C type representation and helpers. */
#ifndef CCC_TYPES_H
#define CCC_TYPES_H

#include "common.h"

typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_CHAR,   /* signed char (x86 default) */
    TY_UCHAR,
    TY_SHORT,
    TY_USHORT,
    TY_INT,
    TY_UINT,
    TY_LONG,
    TY_ULONG,
    TY_PTR,
    TY_ARRAY,
    TY_STRUCT,
    TY_UNION,
    TY_ENUM,   /* same layout as int */
    TY_FUNC,
} TyKind;

typedef struct Type Type;
typedef struct Member Member;

struct Member {
    char *name;
    Type *type;
    int offset;
    Loc loc;
    Member *next;
};

struct Type {
    TyKind kind;
    int size;   /* bytes */
    int align;  /* bytes */
    Type *base; /* PTR / ARRAY element */
    int array_len; /* ARRAY: element count, -1 = incomplete */
    /* struct / union */
    char *tag;       /* may be NULL for anonymous */
    Member *members; /* linked list */
    int num_members;
    int is_complete; /* struct/union defined? */
    /* function */
    Type *ret;
    Type **params;
    int num_params;
    int is_variadic;
    int is_unsigned; /* integer kinds */
    int is_const;    /* qualifier (stored, mostly ignored) */
};

extern Type *ty_void;
extern Type *ty_bool;
extern Type *ty_char;
extern Type *ty_uchar;
extern Type *ty_short;
extern Type *ty_ushort;
extern Type *ty_int;
extern Type *ty_uint;
extern Type *ty_long;
extern Type *ty_ulong;

void types_init(Arena *arena);

Type *new_type(TyKind kind);
Type *ptr_to(Type *base);
Type *array_of(Type *base, int len);
Type *func_type(Type *ret);
int type_size(Type *t);   /* errors on incomplete */
int type_align(Type *t);

int is_integer(Type *t);
int is_pointer(Type *t);
int is_arith(Type *t);
int is_scalar(Type *t);
int is_signed_ty(Type *t);
int int_rank(Type *t); /* 0=bool 1=char 2=short 3=int 4=long 5=enum */
/* Usual arithmetic conversions: result type for binary op. */
Type *usual_arith(Type *a, Type *b);
/* Integer promotion. */
Type *promote(Type *t);

Member *find_member(Type *t, const char *name);
int struct_size_align(Type *t); /* fills size/align/offsets; 0 ok, -1 incomplete */

#endif
