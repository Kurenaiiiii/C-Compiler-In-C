/* codegen.c - stack-machine code generator.
 *
 * Strategy: every expression leaves a canonical 64-bit value in RAX
 * (sign/zero-extended per its type). Addresses use gen_lval into RAX.
 * Temporaries spill to the hardware stack (push/pop), so values survive
 * function calls. Locals live at [rbp - offset]; the first six integer
 * arguments arrive in rdi,rsi,rdx,rcx,r8,r9 and are spilled to slots.
 *
 * Limitations (all diagnosed cleanly, see README): no struct/union
 * by-value arguments or returns (use pointers), no va_list callee side,
 * no _Float/double (rejected in sema via type checks on float keywords —
 * actually the lexer accepts `double`? No: there is no TK for it, so it
 * lexes as an identifier and fails as unknown type. Same for float.).
 */
#include "codegen.h"
#include "ast.h"
#include "parser.h"
#include "sema.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct SwitchCtx SwitchCtx;
struct SwitchCtx {
    int id;
    int end_label;
    int disp_label;
    SwitchCtx *prev;
};

typedef struct {
    FILE *out;
    int seq;
    Decl *cur_fn;
    const char *cur_fn_name;
    int frame_size;
    int *breaks;
    int nbreaks, capbreaks;
    int *conts;
    int nconts, capconts;
    SwitchCtx *sw;
    int static_counter;
    /* globals to emit */
    Decl **globals;
    int nglobals;
} CG;

static void emit(CG *g, const char *fmt, ...);

static void emit(CG *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g->out, fmt, ap);
    va_end(ap);
    fputc('\n', g->out);
}

static int new_label(CG *g) {
    return g->seq++;
}

/* Forward declarations (defined near the bottom). */
static Type *decay_ty(Type *t);
static int count_is_one(Node *list);

/* --- type helpers --- */
static int ty_size(Type *t) {
    return type_size(t);
}

/* Load [rax] -> rax per type t (address in rax). */
static void emit_load(CG *g, Type *t) {
    t = t ? t : ty_int;
    if (t->kind == TY_ARRAY) {
        /* decayed already; address stays. */
        return;
    }
    switch (t->kind) {
    case TY_BOOL:
    case TY_UCHAR:
        emit(g, "  movzx eax, BYTE PTR [rax]");
        break;
    case TY_CHAR:
        emit(g, "  movsx rax, BYTE PTR [rax]");
        break;
    case TY_USHORT:
        emit(g, "  movzx eax, WORD PTR [rax]");
        break;
    case TY_SHORT:
        emit(g, "  movsx rax, WORD PTR [rax]");
        break;
    case TY_UINT:
    case TY_ENUM:
        emit(g, "  mov eax, DWORD PTR [rax]");
        break;
    case TY_INT:
        emit(g, "  movsxd rax, DWORD PTR [rax]");
        break;
    case TY_ULONG:
        emit(g, "  mov rax, QWORD PTR [rax]");
        break;
    case TY_LONG:
    case TY_PTR:
        emit(g, "  mov rax, QWORD PTR [rax]");
        break;
    case TY_VOID:
        break;
    default:
        break;
    }
}

/* Store rcx -> [rdi] per type t. Clobbers nothing else. */
static void emit_store(CG *g, Type *t) {
    switch (t->kind) {
    case TY_BOOL:
    case TY_CHAR:
    case TY_UCHAR:
        emit(g, "  mov BYTE PTR [rdi], cl");
        break;
    case TY_SHORT:
    case TY_USHORT:
        emit(g, "  mov WORD PTR [rdi], cx");
        break;
    case TY_INT:
    case TY_UINT:
    case TY_ENUM:
        emit(g, "  mov DWORD PTR [rdi], ecx");
        break;
    case TY_LONG:
    case TY_ULONG:
    case TY_PTR:
        emit(g, "  mov QWORD PTR [rdi], rcx");
        break;
    default:
        break;
    }
}

/* Convert value in rax from type `from` to type `to` (see design note). */
static void emit_convert(CG *g, Type *from, Type *to) {
    if (!from || !to)
        return;
    from = decay_ty(from);
    to = decay_ty(to);
    if (from == to)
        return;
    int fs = from->kind == TY_PTR ? 8 : type_size(from);
    int ts = to->kind == TY_PTR ? 8 : type_size(to);
    int fsign = is_signed_ty(from);
    int tsign = is_signed_ty(to);
    if (from->kind == TY_PTR)
        fsign = 0;
    if (to->kind == TY_PTR) {
        /* int -> pointer: extend per source signedness (already extended
         * except 32-bit negative-as-unsigned cases, which convert below
         * handles via the ts==8 paths). */
        if (fs == 8)
            return;
        if (fsign)
            return; /* already sign-extended */
        /* zero-extended already */
        return;
    }
    if (from->kind == TY_PTR && to->kind != TY_PTR) {
        /* pointer -> integer: truncate then extend per target. */
        if (ts >= 8)
            return;
        if (ts == 4) {
            if (tsign)
                emit(g, "  movsxd rax, eax");
            else
                emit(g, "  mov eax, eax");
        } else if (ts == 2) {
            if (tsign)
                emit(g, "  movsx rax, ax");
            else
                emit(g, "  movzx eax, ax");
        } else if (ts == 1) {
            if (tsign)
                emit(g, "  movsx rax, al");
            else
                emit(g, "  movzx eax, al");
        }
        return;
    }
    if (ts > fs) {
        if (fsign && !tsign) {
            /* signed -> wider unsigned: value mod 2^ts. */
            if (ts == 4)
                emit(g, "  mov eax, eax");
            else if (ts == 2)
                emit(g, "  movzx eax, ax");
            else if (ts == 1)
                emit(g, "  movzx eax, al");
            return;
        }
        return; /* same signedness or unsigned->signed: already extended */
    }
    if (ts == fs) {
        if (fsign == tsign)
            return;
        if (!tsign) {
            if (ts == 4)
                emit(g, "  mov eax, eax");
            else if (ts == 2)
                emit(g, "  movzx eax, ax");
            else if (ts == 1)
                emit(g, "  movzx eax, al");
        } else {
            if (ts == 4)
                emit(g, "  movsxd rax, eax");
            else if (ts == 2)
                emit(g, "  movsx rax, ax");
            else if (ts == 1)
                emit(g, "  movsx rax, al");
        }
        return;
    }
    /* ts < fs: truncate then extend per target signedness. */
    if (ts == 4) {
        if (tsign)
            emit(g, "  movsxd rax, eax");
        else
            emit(g, "  mov eax, eax");
    } else if (ts == 2) {
        if (tsign)
            emit(g, "  movsx rax, ax");
        else
            emit(g, "  movzx eax, ax");
    } else if (ts == 1) {
        if (tsign)
            emit(g, "  movsx rax, al");
        else
            emit(g, "  movzx eax, al");
    }
}

/* Forward. */
static void gen_expr(CG *g, Node *n);
static void gen_lval(CG *g, Node *n);
static void gen_stmt(CG *g, Node *n);

/* Switch-case collection: gather ND_CASE nodes directly enclosed in a
 * switch body (never crossing a nested switch), assigning each an asm
 * label in str_label. `*def_out` gets the default case, if any. */
static void cases_push(Node ***arr, int *n, int *cap, Node *c) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = (Node **)realloc(*arr, (size_t)*cap * sizeof(Node *));
        if (!*arr)
            die("out of memory");
    }
    (*arr)[(*n)++] = c;
}

static void collect_cases(CG *g, Node *n, Node ***arr, int *nc, int *capc,
                          Node **def_out) {
    if (!n)
        return;
    if (n->kind == ND_SWITCH)
        return; /* nested switch: its cases belong to it */
    if (n->kind == ND_CASE) {
        n->str_label = new_label(g);
        cases_push(arr, nc, capc, n);
        if (n->is_default)
            *def_out = n;
        for (int i = 0; i < n->nstmts; i++)
            collect_cases(g, n->stmts[i], arr, nc, capc, def_out);
        return;
    }
    switch (n->kind) {
    case ND_BLOCK:
    case ND_INIT_LIST:
        for (int i = 0; i < n->nstmts; i++)
            collect_cases(g, n->stmts[i], arr, nc, capc, def_out);
        return;
    case ND_IF:
        collect_cases(g, n->cond, arr, nc, capc, def_out);
        collect_cases(g, n->then, arr, nc, capc, def_out);
        if (n->els)
            collect_cases(g, n->els, arr, nc, capc, def_out);
        return;
    case ND_WHILE:
    case ND_DO:
        collect_cases(g, n->cond, arr, nc, capc, def_out);
        collect_cases(g, n->body, arr, nc, capc, def_out);
        return;
    case ND_FOR:
        if (n->init)
            collect_cases(g, n->init, arr, nc, capc, def_out);
        if (n->cond)
            collect_cases(g, n->cond, arr, nc, capc, def_out);
        if (n->inc)
            collect_cases(g, n->inc, arr, nc, capc, def_out);
        collect_cases(g, n->body, arr, nc, capc, def_out);
        return;
    case ND_LABEL:
        collect_cases(g, n->body, arr, nc, capc, def_out);
        return;
    default:
        return;
    }
}

/* Variable address (global/static/local/param/extern). */
static void gen_var_addr(CG *g, Decl *d) {
    if (!d) {
        emit(g, "  xor eax, eax");
        return;
    }
    if (d->is_global || d->is_extern || d->static_label) {
        const char *name = d->static_label ? d->static_label : d->name;
        emit(g, "  lea rax, [rip + %s]", name);
    } else {
        emit(g, "  lea rax, [rbp - %d]", d->offset);
    }
}

static void gen_lval(CG *g, Node *n) {
    switch (n->kind) {
    case ND_VAR:
        gen_var_addr(g, n->decl);
        return;
    case ND_STR:
        emit(g, "  lea rax, [rip + .Lstr%d]", n->str_label);
        return;
    case ND_UNARY:
        if (n->op == '*') {
            gen_expr(g, n->lhs);
            return;
        }
        break;
    case ND_MEMBER: {
        if (n->is_arrow) {
            gen_expr(g, n->lhs);
        } else {
            gen_lval(g, n->lhs);
        }
        if (n->member && n->member->offset)
            emit(g, "  add rax, %d", n->member->offset);
        return;
    }
    case ND_SUBSCRIPT: {
        /* base + index * size. Determine which side is the pointer. */
        Node *base = n->lhs, *idx = n->rhs;
        Type *lt = n->lhs->ty, *rt = n->rhs ? n->rhs->ty : NULL;
        int lptr = lt && (lt->kind == TY_PTR || lt->kind == TY_ARRAY);
        int rptr = rt && (rt->kind == TY_PTR || rt->kind == TY_ARRAY);
        if (!lptr && rptr) {
            base = n->rhs;
            idx = n->lhs;
            lt = base->ty;
        }
        Type *et = NULL;
        if (lt && lt->kind == TY_PTR)
            et = lt->base;
        else if (lt && lt->kind == TY_ARRAY)
            et = lt->base;
        int esz = et ? type_size(et) : 1;
        if (base->kind == ND_VAR && base->ty && base->ty->kind == TY_ARRAY &&
            base->decl && (base->decl->is_global || base->decl->is_extern ||
                            base->decl->static_label)) {
            gen_var_addr(g, base->decl);
        } else if (base->kind == ND_VAR && base->ty &&
                   base->ty->kind == TY_ARRAY && base->decl &&
                   !base->decl->is_global && !base->decl->is_extern &&
                   !base->decl->static_label) {
            emit(g, "  lea rax, [rbp - %d]", base->decl->offset);
        } else if (base->kind == ND_STR) {
            emit(g, "  lea rax, [rip + .Lstr%d]", base->str_label);
        } else if (lt && lt->kind == TY_ARRAY) {
            gen_lval(g, base);
        } else {
            gen_expr(g, base);
        }
        emit(g, "  push rax");
        gen_expr(g, idx);
        emit(g, "  mov rcx, rax");
        emit(g, "  pop rdi");
        if (esz != 1)
            emit(g, "  imul rcx, rcx, %d", esz);
        emit(g, "  mov rax, rdi");
        emit(g, "  add rax, rcx");
        return;
    }
    default:
        break;
    }
    error_at(n->loc, "not an assignable value");
}

/* struct copy [rdi] <- [rsi], size bytes. Clobbers rcx/rsi/rdi. */
static void emit_memcpy(CG *g, int size) {
    if (size <= 0)
        return;
    /* Move in 8-byte chunks + remainder for speed. */
    int off = 0;
    while (size - off >= 8) {
        emit(g, "  mov rcx, QWORD PTR [rsi + %d]", off);
        emit(g, "  mov QWORD PTR [rdi + %d], rcx", off);
        off += 8;
    }
    if (size - off >= 4) {
        emit(g, "  mov ecx, DWORD PTR [rsi + %d]", off);
        emit(g, "  mov DWORD PTR [rdi + %d], ecx", off);
        off += 4;
    }
    if (size - off >= 2) {
        emit(g, "  mov cx, WORD PTR [rsi + %d]", off);
        emit(g, "  mov WORD PTR [rdi + %d], cx", off);
        off += 2;
    }
    if (size - off >= 1) {
        emit(g, "  mov cl, BYTE PTR [rsi + %d]", off);
        emit(g, "  mov BYTE PTR [rdi + %d], cl", off);
    }
}

static const char *arg_reg(int i) {
    static const char *regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    return regs[i];
}

static void gen_call(CG *g, Node *n) {
    /* Function type + directness. */
    Type *ft = NULL;
    int direct = 0;
    const char *fname = NULL;
    if (n->lhs->kind == ND_VAR && n->lhs->decl &&
        n->lhs->decl->type->kind == TY_FUNC) {
        ft = n->lhs->decl->type;
        direct = 1;
        fname = n->lhs->decl->static_label ? n->lhs->decl->static_label
                                           : n->lhs->decl->name;
    } else {
        Type *lt = n->lhs->ty;
        if (lt && lt->kind == TY_FUNC) {
            /* shouldn't happen (decayed), be lenient */
            ft = lt;
        } else if (lt && lt->kind == TY_PTR && lt->base &&
                   lt->base->kind == TY_FUNC) {
            ft = lt->base;
        } else {
            error_at(n->loc, "called object is not a function");
        }
    }
    if (!direct) {
        /* Evaluate function expression first; address THREADS through
         * below (see stack layout note). Order: push fn, then args. */
        gen_expr(g, n->lhs);
        emit(g, "  push rax");
    }
    int nreg = n->nargs < 6 ? n->nargs : 6;
    int nstack = n->nargs - nreg;
    int fn_slot = direct ? 0 : 1;
    int remain = nstack + fn_slot;
    int pad = (remain % 2) ? 8 : 0;
    /* Padding must sit ABOVE the stack arguments (pushed earlier, i.e.
     * at higher addresses) so arg6 stays at [rsp] for the call. */
    if (pad)
        emit(g, "  sub rsp, 8");
    /* Evaluate args in REVERSE so arg0 ends on top (pop forward). */
    for (int i = n->nargs - 1; i >= 0; i--) {
        gen_expr(g, n->args[i]);
        /* Convert to parameter type when prototyped. */
        if (ft && ft->num_params > 0 && i < ft->num_params && ft->params &&
            ft->params[i]) {
            Type *want = ft->params[i];
            Type *have = n->args[i]->ty;
            if (want->kind != TY_STRUCT && want->kind != TY_UNION &&
                want->kind != TY_ARRAY && want->kind != TY_FUNC &&
                have->kind != TY_STRUCT && have->kind != TY_UNION)
                emit_convert(g, have, want);
        }
        emit(g, "  push rax");
    }
    /* Stack top is arg0 after reverse pushes: pop forward into regs. */
    for (int i = 0; i < nreg; i++)
        emit(g, "  pop %s", arg_reg(i));
    if (!direct) {
        /* Target sits below stack args AND the early padding. */
        emit(g, "  mov r11, [rsp + %d]", nstack * 8 + pad);
    }
    emit(g, "  xor eax, eax"); /* variadic al=0; harmless otherwise */
    if (direct) {
        emit(g, "  call %s", fname);
    } else {
        emit(g, "  call r11");
    }
    int cleanup = nstack * 8 + fn_slot * 8 + pad;
    if (cleanup)
        emit(g, "  add rsp, %d", cleanup);
    /* Canonicalize libc-style 32-bit returns (upper eax garbage). */
    Type *ret = ft ? ft->ret : ty_int;
    if (ret->kind == TY_VOID) {
        /* leave rax alone */
    } else if (ret->kind == TY_STRUCT || ret->kind == TY_UNION) {
        error_at(n->loc, "functions returning structs are not supported "
                          "(return a pointer)");
    } else {
        int rs = type_size(ret);
        if (rs == 4) {
            if (is_signed_ty(ret))
                emit(g, "  movsxd rax, eax");
            else
                emit(g, "  mov eax, eax");
        } else if (rs == 2) {
            if (is_signed_ty(ret))
                emit(g, "  movsx rax, ax");
            else
                emit(g, "  movzx eax, ax");
        } else if (rs == 1) {
            if (is_signed_ty(ret))
                emit(g, "  movsx rax, al");
            else
                emit(g, "  movzx eax, al");
        }
    }
}

static void gen_expr(CG *g, Node *n) {
    switch (n->kind) {
    case ND_NUM:
        emit(g, "  movabs rax, %lld", n->ival);
        return;
    case ND_STR:
        emit(g, "  lea rax, [rip + .Lstr%d]", n->str_label);
        return;
    case ND_VAR: {
        Decl *d = n->decl;
        if (!d)
            error_at(n->loc, "unresolved variable");
        if (d->type->kind == TY_ARRAY || d->type->kind == TY_FUNC) {
            gen_var_addr(g, d);
            return;
        }
        if (d->type->kind == TY_STRUCT || d->type->kind == TY_UNION)
            error_at(n->loc, "struct value used directly is not supported "
                              "(use &, ., or assign)");
        gen_var_addr(g, d);
        emit_load(g, d->type);
        return;
    }
    case ND_CALL:
        gen_call(g, n);
        return;
    case ND_ASSIGN: {
        Type *lt = n->lhs->ty;
        if (lt->kind == TY_STRUCT || lt->kind == TY_UNION) {
            gen_lval(g, n->lhs);
            emit(g, "  push rax");
            gen_lval(g, n->rhs);
            emit(g, "  mov rsi, rax");
            emit(g, "  pop rdi");
            emit_memcpy(g, type_size(lt));
            emit(g, "  mov rax, rdi"); /* value = destination address */
            return;
        }
        gen_lval(g, n->lhs);
        emit(g, "  push rax");
        gen_expr(g, n->rhs);
        emit_convert(g, n->rhs->ty, lt);
        emit(g, "  mov rcx, rax");
        emit(g, "  pop rdi");
        emit_store(g, lt);
        /* rax keeps the converted value. */
        return;
    }
    case ND_COMPOUND: {
        Type *lt = decay_ty(n->lhs->ty);
        Type *rt = decay_ty(n->rhs->ty);
        int is_ptr = lt->kind == TY_PTR;
        gen_lval(g, n->lhs);
        emit(g, "  push rax"); /* addr */
        emit(g, "  mov rax, [rsp]");
        emit_load(g, n->lhs->ty);
        emit(g, "  push rax"); /* old */
        gen_expr(g, n->rhs);
        emit(g, "  mov rcx, rax"); /* rhs */
        emit(g, "  pop rax");      /* old */
        if (is_ptr && (n->op == '+' || n->op == '-')) {
            int esz = type_size(lt->base);
            if (esz != 1)
                emit(g, "  imul rcx, rcx, %d", esz);
            if (n->op == '+')
                emit(g, "  add rax, rcx");
            else
                emit(g, "  sub rax, rcx");
        } else {
            /* integer op with usual conversions */
            Type *ct = usual_arith(n->lhs->ty, n->rhs->ty);
            emit_convert(g, lt, ct);
            emit(g, "  push rax");
            emit(g, "  mov rax, rcx");
            emit_convert(g, rt, ct);
            emit(g, "  mov rcx, rax");
            emit(g, "  pop rax");
            int un = !is_signed_ty(ct);
            switch (n->op) {
            case '+': emit(g, "  add rax, rcx"); break;
            case '-': emit(g, "  sub rax, rcx"); break;
            case '*': emit(g, "  imul rax, rcx"); break;
            case '/':
                if (un) {
                    emit(g, "  xor edx, edx");
                    emit(g, "  div rcx");
                } else {
                    emit(g, "  cqo");
                    emit(g, "  idiv rcx");
                }
                break;
            case '%':
                if (un) {
                    emit(g, "  xor edx, edx");
                    emit(g, "  div rcx");
                } else {
                    emit(g, "  cqo");
                    emit(g, "  idiv rcx");
                }
                emit(g, "  mov rax, rdx");
                break;
            case '&': emit(g, "  and rax, rcx"); break;
            case '|': emit(g, "  or rax, rcx"); break;
            case '^': emit(g, "  xor rax, rcx"); break;
            case 'L':
                emit(g, "  sal rax, cl");
                break;
            case 'R':
                if (un)
                    emit(g, "  shr rax, cl");
                else
                    emit(g, "  sar rax, cl");
                break;
            default:
                error_at(n->loc, "bad compound operator");
            }
            emit_convert(g, ct, lt);
        }
        emit(g, "  mov rcx, rax");
        emit(g, "  pop rdi");
        /* reload addr (rdi) — addr was pushed first, now on top */
        emit(g, "  push rcx");
        emit(g, "  mov rcx, rax");
        emit_store(g, n->lhs->ty);
        emit(g, "  pop rax");
        return;
    }
    case ND_TERNARY: {
        if (n->then->ty &&
            (n->then->ty->kind == TY_STRUCT || n->then->ty->kind == TY_UNION))
            error_at(n->loc, "ternary on structs is not supported");
        int l_else = new_label(g), l_end = new_label(g);
        gen_expr(g, n->cond);
        emit(g, "  test rax, rax");
        emit(g, "  je .L%d", l_else);
        gen_expr(g, n->then);
        /* convert branch to result type */
        if (n->ty && n->then->ty &&
            (is_arith(n->ty) || is_pointer(n->ty)))
            emit_convert(g, decay_ty(n->then->ty), decay_ty(n->ty));
        emit(g, "  jmp .L%d", l_end);
        emit(g, ".L%d:", l_else);
        gen_expr(g, n->els);
        if (n->ty && n->els->ty &&
            (is_arith(n->ty) || is_pointer(n->ty)))
            emit_convert(g, decay_ty(n->els->ty), decay_ty(n->ty));
        emit(g, ".L%d:", l_end);
        return;
    }
    case ND_COMMA:
        gen_expr(g, n->lhs);
        gen_expr(g, n->rhs);
        return;
    case ND_UNARY: {
        switch (n->op) {
        case '&':
            gen_lval(g, n->lhs);
            return;
        case '*': {
            gen_expr(g, n->lhs);
            Type *t = n->ty;
            if (t->kind == TY_STRUCT || t->kind == TY_UNION)
                error_at(n->loc, "struct value used directly is not "
                                  "supported");
            emit_load(g, t);
            return;
        }
        case '+':
            gen_expr(g, n->lhs);
            emit_convert(g, n->lhs->ty, n->ty);
            return;
        case '-':
            gen_expr(g, n->lhs);
            emit_convert(g, n->lhs->ty, n->ty);
            emit(g, "  neg rax");
            return;
        case '~':
            gen_expr(g, n->lhs);
            emit_convert(g, n->lhs->ty, n->ty);
            emit(g, "  not rax");
            return;
        case '!':
            gen_expr(g, n->lhs);
            emit(g, "  test rax, rax");
            emit(g, "  setz al");
            emit(g, "  movzx eax, al");
            return;
        case 'P':
        case 'M': {
            int delta = 1;
            Type *lt = decay_ty(n->lhs->ty);
            if (lt->kind == TY_PTR)
                delta = type_size(lt->base);
            gen_lval(g, n->lhs);
            emit(g, "  mov rdi, rax");
            emit(g, "  mov rax, rdi");
            emit_load(g, n->lhs->ty);
            if (n->op == 'P')
                emit(g, "  add rax, %d", delta);
            else
                emit(g, "  sub rax, %d", delta);
            emit(g, "  mov rcx, rax");
            emit_store(g, n->lhs->ty);
            emit(g, "  mov rax, rcx");
            return;
        }
        case 'p':
        case 'm': {
            int delta = 1;
            Type *lt = decay_ty(n->lhs->ty);
            if (lt->kind == TY_PTR)
                delta = type_size(lt->base);
            gen_lval(g, n->lhs);
            emit(g, "  mov rdi, rax");
            emit(g, "  mov rax, rdi");
            emit_load(g, n->lhs->ty);
            emit(g, "  mov rcx, rax");
            if (n->op == 'p')
                emit(g, "  add rcx, %d", delta);
            else
                emit(g, "  sub rcx, %d", delta);
            emit(g, "  push rax");
            emit(g, "  mov rax, rcx");
            emit(g, "  mov rcx, rax");
            emit_store(g, n->lhs->ty);
            emit(g, "  pop rax");
            return;
        }
        default:
            error_at(n->loc, "bad unary operator");
        }
        return;
    }
    case ND_BINARY: {
        /* Short-circuit logicals. */
        if (n->op == 'A' || n->op == 'O') {
            int l_false = new_label(g), l_end = new_label(g);
            gen_expr(g, n->lhs);
            emit(g, "  test rax, rax");
            emit(g, "  je .L%d", l_false);
            /* Trick: use one label as "decided 1/0"? Simpler classic: */
            /* Rework: A: lhs false -> 0; else rhs truthiness. */
            if (n->op == 'A') {
                gen_expr(g, n->rhs);
                emit(g, "  test rax, rax");
                emit(g, "  setnz al");
                emit(g, "  movzx eax, al");
                emit(g, "  jmp .L%d", l_end);
                emit(g, ".L%d:", l_false);
                emit(g, "  xor eax, eax");
                emit(g, ".L%d:", l_end);
            } else {
                /* lhs was true (fell through): result 1. */
                emit(g, "  mov eax, 1");
                emit(g, "  jmp .L%d", l_end);
                emit(g, ".L%d:", l_false);
                gen_expr(g, n->rhs);
                emit(g, "  test rax, rax");
                emit(g, "  setnz al");
                emit(g, "  movzx eax, al");
                emit(g, ".L%d:", l_end);
            }
            return;
        }
        Type *lt = decay_ty(n->lhs->ty);
        Type *rt = decay_ty(n->rhs->ty);
        int lptr = lt->kind == TY_PTR;
        int rptr = rt->kind == TY_PTR;

        /* Pointer arithmetic. */
        if (n->op == '+' && (lptr || rptr)) {
            Node *p = lptr ? n->lhs : n->rhs;
            Node *q = lptr ? n->rhs : n->lhs;
            Type *pt = lptr ? lt : rt;
            int esz = type_size(pt->base);
            gen_expr(g, p);
            emit(g, "  push rax");
            gen_expr(g, q);
            emit(g, "  mov rcx, rax");
            if (esz != 1)
                emit(g, "  imul rcx, rcx, %d", esz);
            emit(g, "  pop rax");
            emit(g, "  add rax, rcx");
            return;
        }
        if (n->op == '-' && lptr && !rptr) {
            int esz = type_size(lt->base);
            gen_expr(g, n->lhs);
            emit(g, "  push rax");
            gen_expr(g, n->rhs);
            emit(g, "  mov rcx, rax");
            emit(g, "  pop rax");
            emit(g, "  sub rax, rcx");
            if (esz != 1) {
                emit(g, "  mov rcx, %d", esz);
                emit(g, "  cqo");
                emit(g, "  idiv rcx");
            }
            return;
        }
        if (n->op == '-' && lptr && rptr) {
            int esz = type_size(lt->base);
            gen_expr(g, n->lhs);
            emit(g, "  push rax");
            gen_expr(g, n->rhs);
            emit(g, "  mov rcx, rax");
            emit(g, "  pop rax");
            emit(g, "  sub rax, rcx");
            if (esz != 1) {
                emit(g, "  mov rcx, %d", esz);
                emit(g, "  cqo");
                emit(g, "  idiv rcx");
            }
            return;
        }
        /* Comparisons (ints via usual conversions; pointers unsigned). */
        if (n->op == 'E' || n->op == 'N' || n->op == '<' || n->op == '>' ||
            n->op == 'l' || n->op == 'g') {
            int un = 0;
            const char *cc = NULL;
            if (lptr || rptr) {
                /* Addresses compare as unsigned; no conversion. */
                gen_expr(g, n->lhs);
                emit(g, "  push rax");
                gen_expr(g, n->rhs);
                emit(g, "  mov rcx, rax");
                emit(g, "  pop rax");
                un = 1;
                if (n->op == 'E')
                    cc = "e";
                else if (n->op == 'N')
                    cc = "ne";
                else if (n->op == '<')
                    cc = "b";
                else if (n->op == '>')
                    cc = "a";
                else if (n->op == 'l')
                    cc = "be";
                else
                    cc = "ae";
            } else {
                Type *ct = usual_arith(n->lhs->ty, n->rhs->ty);
                gen_expr(g, n->lhs);
                emit_convert(g, lt, ct);
                emit(g, "  push rax");
                gen_expr(g, n->rhs);
                emit_convert(g, rt, ct);
                emit(g, "  mov rcx, rax");
                emit(g, "  pop rax");
                un = !is_signed_ty(ct);
                if (n->op == 'E')
                    cc = "e";
                else if (n->op == 'N')
                    cc = "ne";
                else if (un) {
                    if (n->op == '<')
                        cc = "b";
                    else if (n->op == '>')
                        cc = "a";
                    else if (n->op == 'l')
                        cc = "be";
                    else
                        cc = "ae";
                } else {
                    if (n->op == '<')
                        cc = "l";
                    else if (n->op == '>')
                        cc = "g";
                    else if (n->op == 'l')
                        cc = "le";
                    else
                        cc = "ge";
                }
            }
            emit(g, "  cmp rax, rcx");
            emit(g, "  set%s al", cc);
            emit(g, "  movzx eax, al");
            (void)un;
            return;
        }
        /* Shifts. */
        if (n->op == 'L' || n->op == 'R') {
            int un = !is_signed_ty(lt);
            gen_expr(g, n->lhs);
            emit_convert(g, lt, n->ty);
            emit(g, "  push rax");
            gen_expr(g, n->rhs);
            emit(g, "  mov rcx, rax");
            emit(g, "  pop rax");
            if (n->op == 'L')
                emit(g, "  sal rax, cl");
            else if (un)
                emit(g, "  shr rax, cl");
            else
                emit(g, "  sar rax, cl");
            emit_convert(g, promote(n->lhs->ty), n->ty);
            return;
        }
        /* Arithmetic + bitwise with usual conversions. */
        {
            Type *ct = usual_arith(lt, rt);
            int un = !is_signed_ty(ct);
            gen_expr(g, n->lhs);
            emit_convert(g, lt, ct);
            emit(g, "  push rax");
            gen_expr(g, n->rhs);
            emit_convert(g, rt, ct);
            emit(g, "  mov rcx, rax");
            emit(g, "  pop rax");
            switch (n->op) {
            case '+': emit(g, "  add rax, rcx"); break;
            case '-': emit(g, "  sub rax, rcx"); break;
            case '*': emit(g, "  imul rax, rcx"); break;
            case '/':
                if (un) {
                    emit(g, "  xor edx, edx");
                    emit(g, "  div rcx");
                } else {
                    emit(g, "  cqo");
                    emit(g, "  idiv rcx");
                }
                break;
            case '%':
                if (un) {
                    emit(g, "  xor edx, edx");
                    emit(g, "  div rcx");
                } else {
                    emit(g, "  cqo");
                    emit(g, "  idiv rcx");
                }
                emit(g, "  mov rax, rdx");
                break;
            case '&': emit(g, "  and rax, rcx"); break;
            case '|': emit(g, "  or rax, rcx"); break;
            case '^': emit(g, "  xor rax, rcx"); break;
            default:
                error_at(n->loc, "bad binary operator");
            }
            emit_convert(g, ct, n->ty);
            return;
        }
    }
    case ND_CAST:
        gen_expr(g, n->lhs);
        emit_convert(g, n->lhs->ty, n->sz_type);
        return;
    case ND_SIZEOF: {
        long long v = 0;
        if (n->sz_type)
            v = n->sz_type->kind == TY_VOID ? 1 : type_size(n->sz_type);
        else if (n->lhs && n->lhs->ty)
            v = type_size(n->lhs->ty); /* no decay, like sema */
        emit(g, "  movabs rax, %lld", v);
        return;
    }
    case ND_MEMBER: {
        if (n->ty->kind == TY_STRUCT || n->ty->kind == TY_UNION)
            error_at(n->loc, "struct value used directly is not supported");
        gen_lval(g, n);
        emit_load(g, n->ty);
        return;
    }
    case ND_SUBSCRIPT: {
        if (n->ty->kind == TY_STRUCT || n->ty->kind == TY_UNION)
            error_at(n->loc, "struct value used directly is not supported");
        gen_lval(g, n);
        emit_load(g, n->ty);
        return;
    }
    default:
        error_at(n->loc, "cannot generate expression");
    }
}

/* --- statements --- */
static void gen_block(CG *g, Node *n);

static void gen_stmt(CG *g, Node *n) {
    switch (n->kind) {
    case ND_BLOCK:
        gen_block(g, n);
        return;
    case ND_NOP:
        return;
    case ND_EXPR_STMT:
        if (n->lhs)
            gen_expr(g, n->lhs);
        return;
    case ND_VARDECL: {
        Decl *d = n->decl;
        if (!d || d->is_typedef)
            return;
        if (d->type->kind == TY_FUNC)
            return; /* prototype */
        if (d->is_global || d->is_extern)
            return; /* emitted in data section */
        if (d->static_label)
            return; /* static local lives in data section */
        if (!d->init)
            return;
        Type *t = d->type;
        Node *init = d->init;
        if (init->kind == ND_INIT_LIST) {
            /* C zero-fills the tail of a partially initialized object,
             * so zero the whole object first, then store elements.
             * (d->offset is the END offset; base = rbp - offset.) */
            if (t->kind == TY_ARRAY) {
                int esz = type_size(t->base);
                int total = t->array_len >= 0 ? t->array_len : init->nstmts;
                if (init->nstmts > total)
                    error_at(n->loc, "too many initializers for array");
                emit(g, "  lea rdi, [rbp - %d]", d->offset);
                emit(g, "  mov ecx, %d", total * esz);
                emit(g, "  xor eax, eax");
                emit(g, "  rep stosb");
                for (int i = 0; i < init->nstmts; i++) {
                    Node *e = init->stmts[i];
                    if (e->kind == ND_INIT_LIST)
                        error_at(e->loc, "nested init lists for arrays are "
                                          "not supported in locals");
                    gen_expr(g, e);
                    emit_convert(g, e->ty, t->base);
                    emit(g, "  mov rcx, rax");
                    emit(g, "  lea rdi, [rbp - %d]", d->offset - i * esz);
                    emit_store(g, t->base);
                }
            } else if (t->kind == TY_STRUCT || t->kind == TY_UNION) {
                emit(g, "  lea rdi, [rbp - %d]", d->offset);
                emit(g, "  mov ecx, %d", type_size(t));
                emit(g, "  xor eax, eax");
                emit(g, "  rep stosb");
                if (t->kind == TY_UNION) {
                    if (init->nstmts > 0) {
                        Member *m = t->members;
                        Node *e = init->stmts[0];
                        if (e->kind == ND_INIT_LIST)
                            error_at(e->loc, "nested init lists for "
                                              "unions are not supported");
                        gen_expr(g, e);
                        emit_convert(g, e->ty, m->type);
                        emit(g, "  mov rcx, rax");
                        emit(g, "  lea rdi, [rbp - %d]", d->offset);
                        emit_store(g, m->type);
                    }
                } else {
                    int idx = 0;
                    for (Member *m = t->members; m; m = m->next) {
                        if (idx >= init->nstmts)
                            break;
                        Node *e = init->stmts[idx++];
                        if (e->kind == ND_INIT_LIST)
                            error_at(e->loc, "nested init lists for "
                                              "structs are not supported "
                                              "in locals");
                        gen_expr(g, e);
                        emit_convert(g, e->ty, m->type);
                        emit(g, "  mov rcx, rax");
                        emit(g, "  lea rdi, [rbp - %d]",
                             d->offset - m->offset);
                        emit_store(g, m->type);
                    }
                    if (idx < init->nstmts)
                        error_at(n->loc, "too many initializers for struct");
                }
            } else if (count_is_one(init)) {
                gen_expr(g, init->stmts[0]);
                emit_convert(g, init->stmts[0]->ty, t);
                emit(g, "  mov rcx, rax");
                emit(g, "  lea rdi, [rbp - %d]", d->offset);
                emit_store(g, t);
            } else {
                error_at(n->loc, "too many initializers for scalar");
            }
        } else if (init->kind == ND_STR && t->kind == TY_ARRAY &&
                   t->base->kind == TY_CHAR) {
            /* char arr[N] = "lit": copy bytes, zero-fill the tail. */
            int slen = (int)init->slen;
            int total = t->array_len >= 0 ? t->array_len : slen + 1;
            if (slen > total)
                error_at(init->loc, "string too long for array");
            emit(g, "  lea rdi, [rbp - %d]", d->offset);
            emit(g, "  lea rsi, [rip + .Lstr%d]", init->str_label);
            emit(g, "  mov ecx, %d", slen + 1);
            emit(g, "  rep movsb");
            if (total > slen + 1) {
                emit(g, "  mov ecx, %d", total - (slen + 1));
                emit(g, "  xor eax, eax");
                emit(g, "  rep stosb");
            }
        } else {
            gen_expr(g, init);
            if ((t->kind == TY_STRUCT || t->kind == TY_UNION)) {
                error_at(n->loc, "struct init needs a list");
            }
            emit_convert(g, init->ty, t);
            emit(g, "  mov rcx, rax");
            emit(g, "  lea rdi, [rbp - %d]", d->offset);
            emit_store(g, t);
        }
        return;
    }
    case ND_IF: {
        int l_else = new_label(g), l_end = new_label(g);
        gen_expr(g, n->cond);
        emit(g, "  test rax, rax");
        if (n->els)
            emit(g, "  je .L%d", l_else);
        else
            emit(g, "  je .L%d", l_end);
        gen_stmt(g, n->then);
        if (n->els) {
            emit(g, "  jmp .L%d", l_end);
            emit(g, ".L%d:", l_else);
            gen_stmt(g, n->els);
        }
        emit(g, ".L%d:", l_end);
        return;
    }
    case ND_WHILE: {
        int l_cond = new_label(g), l_end = new_label(g);
        /* continue -> cond, break -> end */
        g->conts[g->nconts++] = l_cond;
        g->breaks[g->nbreaks++] = l_end;
        emit(g, ".L%d:", l_cond);
        gen_expr(g, n->cond);
        emit(g, "  test rax, rax");
        emit(g, "  je .L%d", l_end);
        gen_stmt(g, n->body);
        emit(g, "  jmp .L%d", l_cond);
        emit(g, ".L%d:", l_end);
        g->nconts--;
        g->nbreaks--;
        return;
    }
    case ND_DO: {
        int l_start = new_label(g), l_cond = new_label(g),
            l_end = new_label(g);
        g->conts[g->nconts++] = l_cond;
        g->breaks[g->nbreaks++] = l_end;
        emit(g, ".L%d:", l_start);
        gen_stmt(g, n->body);
        emit(g, ".L%d:", l_cond);
        gen_expr(g, n->cond);
        emit(g, "  test rax, rax");
        emit(g, "  jne .L%d", l_start);
        emit(g, ".L%d:", l_end);
        g->nconts--;
        g->nbreaks--;
        return;
    }
    case ND_FOR: {
        int l_cond = new_label(g), l_inc = new_label(g), l_end = new_label(g);
        if (n->init)
            gen_stmt(g, n->init);
        g->conts[g->nconts++] = l_inc;
        g->breaks[g->nbreaks++] = l_end;
        emit(g, ".L%d:", l_cond);
        if (n->cond) {
            gen_expr(g, n->cond);
            emit(g, "  test rax, rax");
            emit(g, "  je .L%d", l_end);
        }
        gen_stmt(g, n->body);
        emit(g, ".L%d:", l_inc);
        if (n->inc)
            gen_expr(g, n->inc);
        emit(g, "  jmp .L%d", l_cond);
        emit(g, ".L%d:", l_end);
        g->nconts--;
        g->nbreaks--;
        return;
    }
    case ND_RETURN: {
        if (n->lhs) {
            gen_expr(g, n->lhs);
            Type *ret = g->cur_fn ? g->cur_fn->type->ret : ty_int;
            if (ret->kind == TY_STRUCT || ret->kind == TY_UNION)
                error_at(n->loc, "returning structs is not supported "
                                  "(return a pointer)");
            emit_convert(g, n->lhs->ty, ret);
        }
        emit(g, "  jmp .L%s_end", g->cur_fn_name);
        return;
    }
    case ND_BREAK:
        emit(g, "  jmp .L%d", g->breaks[g->nbreaks - 1]);
        return;
    case ND_CONTINUE:
        emit(g, "  jmp .L%d", g->conts[g->nconts - 1]);
        return;
    case ND_SWITCH: {
        /* The switch value spills to [rbp-8] (reserved in the frame),
         * which stays valid across calls and early returns — unlike a
         * callee-saved register, which would unbalance push/pop. */
        int l_end = new_label(g), l_disp = new_label(g);
        Node **cases = NULL;
        int ncases = 0, capcases = 0;
        Node *def = NULL;
        collect_cases(g, n->body, &cases, &ncases, &capcases, &def);
        gen_expr(g, n->cond);
        emit(g, "  mov QWORD PTR [rbp - 8], rax");
        emit(g, "  jmp .L%d", l_disp);
        g->breaks[g->nbreaks++] = l_end;
        gen_stmt(g, n->body);
        g->nbreaks--;
        emit(g, "  jmp .L%d", l_end);
        emit(g, ".L%d:", l_disp);
        for (int i = 0; i < ncases; i++) {
            Node *c = cases[i];
            if (c->is_default)
                continue;
            emit(g, "  movabs rax, %lld", c->ival);
            emit(g, "  cmp QWORD PTR [rbp - 8], rax");
            emit(g, "  je .L%d", c->str_label);
        }
        if (def)
            emit(g, "  jmp .L%d", def->str_label);
        else
            emit(g, "  jmp .L%d", l_end);
        emit(g, ".L%d:", l_end);
        free(cases);
        return;
    }
    case ND_CASE:
        if (n->str_label)
            emit(g, ".L%d:", n->str_label);
        for (int i = 0; i < n->nstmts; i++)
            gen_stmt(g, n->stmts[i]);
        return;
    case ND_LABEL:
        emit(g, ".L%s_%s:", g->cur_fn_name, n->name);
        gen_stmt(g, n->body);
        return;
    case ND_GOTO:
        emit(g, "  jmp .L%s_%s", g->cur_fn_name, n->name);
        return;
    default:
        gen_expr(g, n);
        return;
    }
}

static void gen_block(CG *g, Node *n) {
    for (int i = 0; i < n->nstmts; i++)
        gen_stmt(g, n->stmts[i]);
}

/* --- frame layout --- */
static void layout_decl(CG *g, Decl *d) {
    int sz = type_size(d->type);
    int al = type_align(d->type);
    if (al > 16)
        al = 16;
    g->frame_size = (g->frame_size + al - 1) & ~(al - 1);
    /* offset grows downward: var occupies [rbp-(off+sz), rbp-off)? Use
     * END-offset convention: offset = aligned_end. */
    g->frame_size += sz;
    d->offset = g->frame_size;
}

static void layout_walk(CG *g, Node *n) {
    if (!n)
        return;
    switch (n->kind) {
    case ND_VARDECL: {
        Decl *d = n->decl;
        if (!d || d->is_typedef || d->is_global || d->is_extern)
            return;
        if (d->type->kind == TY_FUNC)
            return;
        if (d->is_static) {
            char buf[128];
            snprintf(buf, sizeof buf, "__static_%s_%s_%d", g->cur_fn_name,
                     d->name, g->static_counter++);
            d->static_label = xstrdup(buf);
            return;
        }
        if (d->type->kind == TY_VOID)
            return;
        layout_decl(g, d);
        return;
    }
    case ND_BLOCK:
        for (int i = 0; i < n->nstmts; i++)
            layout_walk(g, n->stmts[i]);
        return;
    case ND_IF:
        layout_walk(g, n->then);
        if (n->els)
            layout_walk(g, n->els);
        return;
    case ND_WHILE:
    case ND_DO:
        layout_walk(g, n->body);
        return;
    case ND_FOR:
        layout_walk(g, n->init);
        layout_walk(g, n->body);
        return;
    case ND_SWITCH:
        layout_walk(g, n->body);
        return;
    case ND_CASE:
        for (int i = 0; i < n->nstmts; i++)
            layout_walk(g, n->stmts[i]);
        return;
    case ND_LABEL:
        layout_walk(g, n->body);
        return;
    default:
        return;
    }
}

/* --- strings --- */
static void collect_str(Node *n, int *next_id) {
    if (!n)
        return;
    if (n->kind == ND_STR) {
        n->str_label = (*next_id)++;
        return;
    }
    switch (n->kind) {
    case ND_CALL:
        collect_str(n->lhs, next_id);
        for (int i = 0; i < n->nargs; i++)
            collect_str(n->args[i], next_id);
        return;
    case ND_FUNCDEF:
        /* params have no exprs */
        collect_str(n->body, next_id);
        return;
    case ND_VARDECL:
        if (n->decl && n->decl->init)
            collect_str(n->decl->init, next_id);
        return;
    case ND_BLOCK:
    case ND_INIT_LIST:
    case ND_CASE:
        for (int i = 0; i < n->nstmts; i++)
            collect_str(n->stmts[i], next_id);
        return;
    case ND_IF:
        collect_str(n->cond, next_id);
        collect_str(n->then, next_id);
        if (n->els)
            collect_str(n->els, next_id);
        return;
    case ND_WHILE:
    case ND_DO:
        collect_str(n->cond, next_id);
        collect_str(n->body, next_id);
        return;
    case ND_FOR:
        if (n->init)
            collect_str(n->init, next_id);
        if (n->cond)
            collect_str(n->cond, next_id);
        if (n->inc)
            collect_str(n->inc, next_id);
        collect_str(n->body, next_id);
        return;
    case ND_RETURN:
    case ND_EXPR_STMT:
        if (n->lhs)
            collect_str(n->lhs, next_id);
        return;
    case ND_SWITCH:
        collect_str(n->cond, next_id);
        collect_str(n->body, next_id);
        return;
    case ND_LABEL:
        collect_str(n->body, next_id);
        return;
    case ND_ASSIGN:
    case ND_COMPOUND:
    case ND_COMMA:
    case ND_SUBSCRIPT:
        collect_str(n->lhs, next_id);
        collect_str(n->rhs, next_id);
        return;
    case ND_TERNARY:
        collect_str(n->cond, next_id);
        collect_str(n->then, next_id);
        collect_str(n->els, next_id);
        return;
    case ND_UNARY:
    case ND_CAST:
        collect_str(n->lhs, next_id);
        return;
    case ND_BINARY:
        collect_str(n->lhs, next_id);
        collect_str(n->rhs, next_id);
        return;
    case ND_MEMBER:
        collect_str(n->lhs, next_id);
        return;
    case ND_SIZEOF:
        if (n->lhs)
            collect_str(n->lhs, next_id);
        return;
    default:
        return;
    }
}

/* --- globals --- */
static int is_zero_init(Node *init) {
    if (!init)
        return 1;
    if (init->kind == ND_NUM)
        return init->ival == 0;
    if (init->kind == ND_INIT_LIST) {
        for (int i = 0; i < init->nstmts; i++) {
            if (!is_zero_init(init->stmts[i]))
                return 0;
        }
        return 1;
    }
    return 0;
}

/* Emit initialized scalar as directive. Returns 1 if emitted. */
static void emit_data_scalar(CG *g, Type *t, Node *init) {
    int ok = 0;
    long long v = sema_eval_const(init, &ok);
    /* address constants */
    if (!ok) {
        if (init->kind == ND_UNARY && init->op == '&' &&
            init->lhs->kind == ND_VAR && init->lhs->decl) {
            Decl *d = init->lhs->decl;
            const char *nm =
                d->static_label ? d->static_label : d->name;
            emit(g, "  .quad %s", nm);
            return;
        }
        if (init->kind == ND_VAR && init->decl &&
            (init->decl->is_global || init->decl->is_extern ||
             init->decl->static_label) &&
            (init->decl->type->kind == TY_ARRAY ||
             init->decl->type->kind == TY_FUNC)) {
            const char *nm = init->decl->static_label
                                 ? init->decl->static_label
                                 : init->decl->name;
            emit(g, "  .quad %s", nm);
            return;
        }
        if (init->kind == ND_STR) {
            emit(g, "  .quad .Lstr%d", init->str_label);
            return;
        }
        error_at(init->loc, "global initializer must be constant");
    }
    int sz = type_size(t);
    if (sz == 1)
        emit(g, "  .byte %lld", v & 0xFF);
    else if (sz == 2)
        emit(g, "  .value %lld", v & 0xFFFF);
    else if (sz == 4)
        emit(g, "  .long %lld", (long long)(int)v);
    else
        emit(g, "  .quad %lld", v);
}

static void emit_data_init(CG *g, Type *t, Node *init, int *poff);

static void emit_data_list(CG *g, Type *t, Node *list) {
    int off = 0;
    if (t->kind == TY_ARRAY) {
        int esz = type_size(t->base);
        int n = t->array_len >= 0 ? t->array_len : list->nstmts;
        for (int i = 0; i < n; i++) {
            if (i < list->nstmts) {
                Node *e = list->stmts[i];
                if (e->kind == ND_STR && t->base->kind == TY_CHAR) {
                    /* string element? only for char arrays as whole. */
                    emit_data_scalar(g, t->base, e);
                    off += esz;
                    continue;
                }
                int eoff = 0;
                emit_data_init(g, t->base, e, &eoff);
                off += esz;
            } else {
                emit(g, "  .zero %d", esz);
                off += esz;
            }
        }
    } else if (t->kind == TY_STRUCT) {
        int idx = 0;
        for (Member *m = t->members; m; m = m->next) {
            if (m->offset > off)
                emit(g, "  .zero %d", m->offset - off);
            off = m->offset;
            if (idx < list->nstmts) {
                int eoff = 0;
                emit_data_init(g, m->type, list->stmts[idx], &eoff);
            } else {
                emit(g, "  .zero %d", type_size(m->type));
            }
            off += type_size(m->type);
            idx++;
        }
        if (off < type_size(t))
            emit(g, "  .zero %d", type_size(t) - off);
    } else if (t->kind == TY_UNION) {
        if (list->nstmts > 0) {
            int eoff = 0;
            /* init first member */
            emit_data_init(g, t->members->type, list->stmts[0], &eoff);
            off = type_size(t->members->type);
        }
        if (off < type_size(t))
            emit(g, "  .zero %d", type_size(t) - off);
    } else {
        if (list->nstmts > 0)
            emit_data_scalar(g, t, list->stmts[0]);
        else
            emit(g, "  .zero %d", type_size(t));
    }
}

static void emit_data_init(CG *g, Type *t, Node *init, int *poff) {
    (void)poff;
    if (init->kind == ND_INIT_LIST) {
        emit_data_list(g, t, init);
        return;
    }
    if (init->kind == ND_STR && t->kind == TY_ARRAY &&
        t->base->kind == TY_CHAR) {
        /* bytes + NUL + padding */
        const char *s = init->sval;
        for (size_t i = 0; i < init->slen; i++)
            emit(g, "  .byte %d", (unsigned char)s[i]);
        int total = t->array_len >= 0 ? t->array_len : (int)init->slen + 1;
        int rest = total - (int)init->slen;
        for (int i = 0; i < rest; i++)
            emit(g, "  .byte 0");
        return;
    }
    emit_data_scalar(g, t, init);
}

/* Emit one string literal's bytes (called via recursive walk). */
static void emit_one_str(CG *g, Node *x) {
    emit(g, ".Lstr%d:", x->str_label);
    for (size_t k = 0; k < x->slen; k++)
        emit(g, "  .byte %d", (unsigned char)x->sval[k]);
    emit(g, "  .byte 0");
}

static void walk_strs(CG *g, Node *x) {
    if (!x)
        return;
    if (x->kind == ND_STR) {
        emit_one_str(g, x);
        return;
    }
    switch (x->kind) {
    case ND_CALL:
        walk_strs(g, x->lhs);
        for (int k = 0; k < x->nargs; k++)
            walk_strs(g, x->args[k]);
        break;
    case ND_FUNCDEF:
        walk_strs(g, x->body);
        break;
    case ND_VARDECL:
        if (x->decl && x->decl->init)
            walk_strs(g, x->decl->init);
        break;
    case ND_BLOCK:
    case ND_INIT_LIST:
    case ND_CASE:
        for (int k = 0; k < x->nstmts; k++)
            walk_strs(g, x->stmts[k]);
        break;
    case ND_IF:
        walk_strs(g, x->cond);
        walk_strs(g, x->then);
        if (x->els)
            walk_strs(g, x->els);
        break;
    case ND_WHILE:
    case ND_DO:
        walk_strs(g, x->cond);
        walk_strs(g, x->body);
        break;
    case ND_FOR:
        if (x->init)
            walk_strs(g, x->init);
        if (x->cond)
            walk_strs(g, x->cond);
        if (x->inc)
            walk_strs(g, x->inc);
        walk_strs(g, x->body);
        break;
    case ND_RETURN:
    case ND_EXPR_STMT:
        if (x->lhs)
            walk_strs(g, x->lhs);
        break;
    case ND_SWITCH:
        walk_strs(g, x->cond);
        walk_strs(g, x->body);
        break;
    case ND_LABEL:
        if (x->body)
            walk_strs(g, x->body);
        break;
    case ND_ASSIGN:
    case ND_COMPOUND:
    case ND_COMMA:
    case ND_SUBSCRIPT:
        walk_strs(g, x->lhs);
        walk_strs(g, x->rhs);
        break;
    case ND_TERNARY:
        walk_strs(g, x->cond);
        walk_strs(g, x->then);
        walk_strs(g, x->els);
        break;
    case ND_UNARY:
    case ND_CAST:
        if (x->lhs)
            walk_strs(g, x->lhs);
        break;
    case ND_BINARY:
        walk_strs(g, x->lhs);
        walk_strs(g, x->rhs);
        break;
    case ND_MEMBER:
        if (x->lhs)
            walk_strs(g, x->lhs);
        break;
    case ND_SIZEOF:
        if (x->lhs)
            walk_strs(g, x->lhs);
        break;
    default:
        break;
    }
}

static void emit_globals(CG *g) {
    /* rodata: strings (each literal gets its own label; simple waste). */
    Node **prog = parser_prog_list();
    int n = parser_prog_count();
    emit(g, "  .section .rodata");
    for (int i = 0; i < n; i++)
        walk_strs(g, prog[i]);

    /* data / bss */
    for (int i = 0; i < g->nglobals; i++) {
        Decl *d = g->globals[i];
        if (d->type->kind == TY_FUNC || d->is_typedef)
            continue;
        if (d->is_extern && !d->init)
            continue; /* mere declaration */
        const char *nm = d->static_label ? d->static_label : d->name;
        int sz = type_size(d->type);
        int al = type_align(d->type);
        if (!d->is_static && !d->static_label)
            emit(g, "  .globl %s", nm);
        if (!d->init || is_zero_init(d->init)) {
            if (d->init && d->init->kind != ND_INIT_LIST &&
                d->init->kind != ND_NUM) {
                /* e.g. address init of value 0? can't be zero-test passed
                 * unless numeric; fall to data. */
            } else {
                emit(g, "  .bss");
                emit(g, "  .align %d", al);
                emit(g, "%s:", nm);
                emit(g, "  .zero %d", sz ? sz : 1);
                continue;
            }
        }
        emit(g, "  .data");
        emit(g, "  .align %d", al);
        emit(g, "%s:", nm);
        if (!d->init) {
            emit(g, "  .zero %d", sz ? sz : 1);
        } else {
            int off = 0;
            emit_data_init(g, d->type, d->init, &off);
        }
    }
}

/* --- functions --- */
static void gen_function(CG *g, Node *fn) {
    Decl *d = fn->decl;
    g->cur_fn = d;
    g->cur_fn_name = d->static_label ? d->static_label : d->name;
    /* Reserve [rbp-8] as the switch-value spill slot (see ND_SWITCH). */
    g->frame_size = 8;
    /* params get slots first (spill area). */
    for (int i = 0; i < fn->nparams; i++) {
        Decl *pd = fn->params[i];
        if (!pd->name)
            continue; /* unnamed prototype param */
        if (pd->type->kind == TY_STRUCT || pd->type->kind == TY_UNION)
            error_at(fn->loc, "struct passed by value is not supported");
        layout_decl(g, pd);
    }
    layout_walk(g, fn->body);
    g->frame_size = (g->frame_size + 15) & ~15;

    if (!d->is_static)
        emit(g, "  .globl %s", d->name);
    emit(g, "  .text");
    emit(g, "%s:", d->name);
    emit(g, "  push rbp");
    emit(g, "  mov rbp, rsp");
    if (g->frame_size)
        emit(g, "  sub rsp, %d", g->frame_size);
    g->nbreaks = 0;
    g->nconts = 0;
    /* spill params */
    static const char *preg[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    static const char *preg8[] = {"dil", "sil", "dl", "cl", "r8b", "r9b"};
    static const char *preg16[] = {"di", "si", "dx", "cx", "r8w", "r9w"};
    static const char *preg32[] = {"edi", "esi", "edx", "ecx", "r8d", "r9d"};
    for (int i = 0; i < fn->nparams; i++) {
        Decl *pd = fn->params[i];
        if (!pd->name)
            continue;
        int sz = type_size(pd->type);
        if (i < 6) {
            if (sz == 1)
                emit(g, "  mov BYTE PTR [rbp - %d], %s", pd->offset, preg8[i]);
            else if (sz == 2)
                emit(g, "  mov WORD PTR [rbp - %d], %s", pd->offset, preg16[i]);
            else if (sz == 4)
                emit(g, "  mov DWORD PTR [rbp - %d], %s", pd->offset,
                     preg32[i]);
            else
                emit(g, "  mov QWORD PTR [rbp - %d], %s", pd->offset, preg[i]);
        } else {
            int sarg = 16 + (i - 6) * 8;
            emit(g, "  mov rax, QWORD PTR [rbp + %d]", sarg);
            if (sz == 1) {
                emit(g, "  mov BYTE PTR [rbp - %d], al", pd->offset);
            } else if (sz == 2) {
                emit(g, "  mov WORD PTR [rbp - %d], ax", pd->offset);
            } else if (sz == 4) {
                emit(g, "  mov DWORD PTR [rbp - %d], eax", pd->offset);
            } else {
                emit(g, "  mov QWORD PTR [rbp - %d], rax", pd->offset);
            }
        }
    }
    gen_block(g, fn->body);
    emit(g, ".L%s_end:", g->cur_fn_name);
    if (g->frame_size)
        emit(g, "  mov rsp, rbp");
    emit(g, "  pop rbp");
    emit(g, "  ret");
    g->cur_fn = NULL;
}

static int count_is_one(Node *list) {
    return list && list->kind == ND_INIT_LIST && list->nstmts == 1;
}

static Type *decay_ty(Type *t) {
    if (t && t->kind == TY_ARRAY)
        return ptr_to(t->base);
    if (t && t->kind == TY_FUNC)
        return ptr_to(t);
    return t;
}

/* Recursively gather static-local decls for data emission. */
static void gather_statics(CG *g, Node *x, int *capg) {
    if (!x)
        return;
    if (x->kind == ND_VARDECL) {
        if (x->decl->static_label) {
            if (g->nglobals == *capg) {
                *capg = *capg ? *capg * 2 : 32;
                g->globals = (Decl **)realloc(g->globals,
                                              (size_t)*capg * sizeof(Decl *));
                if (!g->globals)
                    die("out of memory");
            }
            g->globals[g->nglobals++] = x->decl;
        }
        return;
    }
    switch (x->kind) {
    case ND_BLOCK:
    case ND_CASE:
        for (int k = 0; k < x->nstmts; k++)
            gather_statics(g, x->stmts[k], capg);
        break;
    case ND_IF:
        gather_statics(g, x->then, capg);
        if (x->els)
            gather_statics(g, x->els, capg);
        break;
    case ND_WHILE:
    case ND_DO:
        gather_statics(g, x->body, capg);
        break;
    case ND_FOR:
        if (x->init)
            gather_statics(g, x->init, capg);
        gather_statics(g, x->body, capg);
        break;
    case ND_SWITCH:
        gather_statics(g, x->body, capg);
        break;
    case ND_LABEL:
        gather_statics(g, x->body, capg);
        break;
    default:
        break;
    }
}

void codegen(FILE *out) {
    CG g;
    memset(&g, 0, sizeof g);
    g.out = out;
    g.seq = 1; /* str_label 0 means "no switch label assigned" */
    g.breaks = (int *)xmalloc(1024 * sizeof(int));
    g.capbreaks = 1024;
    g.conts = (int *)xmalloc(1024 * sizeof(int));
    g.capconts = 1024;

    Node **prog = parser_prog_list();
    int n = parser_prog_count();

    /* assign string labels */
    int sid = 0;
    for (int i = 0; i < n; i++)
        collect_str(prog[i], &sid);

    /* collect globals (incl. static locals discovered during layout).
     * Layout runs per function in gen_function; statics found there must
     * be emitted too. Two-phase: layout all first, then emit. */
    Node **fns = NULL;
    int nfns = 0;
    g.nglobals = 0;
    int capg = 0;
    for (int i = 0; i < n; i++) {
        if (prog[i]->kind == ND_VARDECL) {
            /* Merged tentative definitions share one Decl: emit once. */
            int dup = 0;
            for (int k = 0; k < g.nglobals; k++) {
                if (g.globals[k] == prog[i]->decl) {
                    dup = 1;
                    break;
                }
            }
            if (dup)
                continue;
            if (g.nglobals == capg) {
                capg = capg ? capg * 2 : 32;
                g.globals = (Decl **)realloc(g.globals,
                                             (size_t)capg * sizeof(Decl *));
                if (!g.globals)
                    die("out of memory");
            }
            g.globals[g.nglobals++] = prog[i]->decl;
        } else if (prog[i]->kind == ND_FUNCDEF) {
            nfns++;
        }
    }
    fns = (Node **)xmalloc((size_t)(nfns ? nfns : 1) * sizeof(Node *));
    {
        int k = 0;
        for (int i = 0; i < n; i++) {
            if (prog[i]->kind == ND_FUNCDEF)
                fns[k++] = prog[i];
        }
    }
    /* Pre-layout every function to discover static locals before data. */
    for (int i = 0; i < nfns; i++) {
        Node *fn = fns[i];
        CG tmp = g;
        tmp.cur_fn_name = fn->decl->name;
        tmp.frame_size = 8; /* same reserve as gen_function */
        for (int j = 0; j < fn->nparams; j++) {
            Decl *pd = fn->params[j];
            if (!pd->name)
                continue;
            layout_decl(&tmp, pd);
        }
        layout_walk(&tmp, fn->body);
        /* NOTE: offsets on shared Decl nodes are recomputed identically
         * in gen_function; this pass only discovers static labels. */
    }
    /* gather static locals */
    for (int i = 0; i < nfns; i++)
        gather_statics(&g, fns[i]->body, &capg);

    emit(&g, "  .intel_syntax noprefix");
    for (int i = 0; i < nfns; i++)
        gen_function(&g, fns[i]);
    emit_globals(&g);

    free(g.breaks);
    free(g.conts);
    free(g.globals);
    free(fns);
    (void)ty_size; /* kept for debugging aid */
}
