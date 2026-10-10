/* types.c - type constructors, sizes, promotions. */
#include "types.h"
#include <string.h>

Type *ty_void;
Type *ty_bool;
Type *ty_char;
Type *ty_uchar;
Type *ty_short;
Type *ty_ushort;
Type *ty_int;
Type *ty_uint;
Type *ty_long;
Type *ty_ulong;

static Arena *tarena;

static Type *mk(TyKind k, int size, int align) {
    Type *t = (Type *)arena_calloc(tarena, 1, sizeof(Type));
    t->kind = k;
    t->size = size;
    t->align = align;
    t->array_len = -1;
    t->is_complete = 1;
    return t;
}

void types_init(Arena *arena) {
    tarena = arena;
    ty_void = mk(TY_VOID, 1, 1);
    ty_void->is_complete = 0;
    ty_bool = mk(TY_BOOL, 1, 1);
    ty_char = mk(TY_CHAR, 1, 1);
    ty_uchar = mk(TY_UCHAR, 1, 1);
    ty_short = mk(TY_SHORT, 2, 2);
    ty_ushort = mk(TY_USHORT, 2, 2);
    ty_int = mk(TY_INT, 4, 4);
    ty_uint = mk(TY_UINT, 4, 4);
    ty_uint->is_unsigned = 1;
    ty_uchar->is_unsigned = 1;
    ty_ushort->is_unsigned = 1;
    ty_long = mk(TY_LONG, 8, 8);
    ty_ulong = mk(TY_ULONG, 8, 8);
    ty_ulong->is_unsigned = 1;
}

Type *new_type(TyKind kind) {
    Type *t = (Type *)arena_calloc(tarena, 1, sizeof(Type));
    t->kind = kind;
    t->array_len = -1;
    return t;
}

Type *ptr_to(Type *base) {
    Type *t = new_type(TY_PTR);
    t->base = base;
    t->size = 8;
    t->align = 8;
    t->is_complete = 1;
    return t;
}

Type *array_of(Type *base, int len) {
    Type *t = new_type(TY_ARRAY);
    t->base = base;
    t->array_len = len;
    if (len >= 0 && base->is_complete) {
        t->size = base->size * len;
        t->align = base->align;
        t->is_complete = 1;
    } else {
        t->size = 0;
        t->align = base ? base->align : 1;
        t->is_complete = 0;
    }
    return t;
}

Type *func_type(Type *ret) {
    Type *t = new_type(TY_FUNC);
    t->ret = ret;
    t->size = 1;
    t->align = 1;
    t->is_complete = 1;
    return t;
}

int type_size(Type *t) {
    if (!t)
        return 8;
    if ((t->kind == TY_STRUCT || t->kind == TY_UNION) && !t->is_complete)
        return 0;
    return t->size;
}

int type_align(Type *t) {
    if (!t)
        return 8;
    int a = t->align;
    if (a <= 0)
        a = 1;
    if (a > 16)
        a = 16;
    return a;
}

int is_integer(Type *t) {
    switch (t->kind) {
    case TY_BOOL:
    case TY_CHAR:
    case TY_UCHAR:
    case TY_SHORT:
    case TY_USHORT:
    case TY_INT:
    case TY_UINT:
    case TY_LONG:
    case TY_ULONG:
    case TY_ENUM:
        return 1;
    default:
        return 0;
    }
}

int is_pointer(Type *t) {
    return t->kind == TY_PTR || t->kind == TY_ARRAY;
}

int is_arith(Type *t) {
    return is_integer(t);
}

int is_scalar(Type *t) {
    return is_arith(t) || is_pointer(t);
}

int is_signed_ty(Type *t) {
    if (t->is_unsigned)
        return 0;
    switch (t->kind) {
    case TY_UCHAR:
    case TY_USHORT:
    case TY_UINT:
    case TY_ULONG:
        return 0;
    default:
        return 1;
    }
}

int int_rank(Type *t) {
    switch (t->kind) {
    case TY_BOOL: return 0;
    case TY_CHAR:
    case TY_UCHAR: return 1;
    case TY_SHORT:
    case TY_USHORT: return 2;
    case TY_INT:
    case TY_UINT:
    case TY_ENUM: return 3;
    case TY_LONG:
    case TY_ULONG: return 4;
    default: return -1;
    }
}

Type *promote(Type *t) {
    if (!t)
        return ty_int;
    switch (t->kind) {
    case TY_BOOL:
    case TY_CHAR:
    case TY_UCHAR:
    case TY_SHORT:
    case TY_USHORT:
        return ty_int;
    case TY_ENUM:
        return ty_int;
    default:
        return t;
    }
}

Type *usual_arith(Type *a, Type *b) {
    a = promote(a);
    b = promote(b);
    if (a == b)
        return a;
    /* Unsigned wins on ties; long wins over int. */
    int ra = int_rank(a), rb = int_rank(b);
    int sa = is_signed_ty(a), sb = is_signed_ty(b);
    if (!sa || !sb) {
        /* If ranks equal, unsigned version. Otherwise the wider,
         * with unsigned-ness of the wider, except int+ulong -> ulong. */
        if (ra == rb)
            return sa ? b : a;
        if (ra > rb)
            return sa ? (ra >= 4 ? a : b) : a;
        return sb ? (rb >= 4 ? b : a) : b;
    }
    return ra > rb ? a : b;
}

Member *find_member(Type *t, const char *name) {
    for (Member *m = t->members; m; m = m->next) {
        if (strcmp(m->name, name) == 0)
            return m;
    }
    return NULL;
}

/* Lay out struct/union: offsets, size, align. Returns 0, or -1 if a
 * member type is incomplete. */
int struct_size_align(Type *t) {
    if (t->kind == TY_UNION) {
        int sz = 0, al = 1;
        for (Member *m = t->members; m; m = m->next) {
            if (!m->type->is_complete && m->type->kind != TY_ARRAY)
                return -1;
            int ms = type_size(m->type);
            int ma = type_align(m->type);
            m->offset = 0;
            if (ms > sz)
                sz = ms;
            if (ma > al)
                al = ma;
        }
        if (al > 16)
            al = 16;
        t->align = al;
        t->size = (sz + al - 1) & ~(al - 1);
        if (t->size == 0)
            t->size = 0;
        t->is_complete = 1;
        return 0;
    }
    int off = 0, al = 1;
    for (Member *m = t->members; m; m = m->next) {
        if (!m->type->is_complete && m->type->kind != TY_ARRAY)
            return -1;
        int ma = type_align(m->type);
        int ms = type_size(m->type);
        if (ma > al)
            al = ma;
        off = (off + ma - 1) & ~(ma - 1);
        m->offset = off;
        off += ms;
    }
    if (al > 16)
        al = 16;
    t->align = al;
    t->size = (off + al - 1) & ~(al - 1);
    t->is_complete = 1;
    return 0;
}
