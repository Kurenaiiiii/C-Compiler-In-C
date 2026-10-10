/* lexer.h - tokenizer for the C subset. */
#ifndef CCC_LEXER_H
#define CCC_LEXER_H

#include "common.h"

typedef enum {
    TK_EOF = 0,
    TK_IDENT,
    TK_NUM,   /* integer literal; ival + is_unsigned + is_long */
    TK_CHARLIT,
    TK_STR,   /* string literal; sval + slen (unescaped bytes) */
    /* keywords */
    TK_INT, TK_CHAR, TK_SHORT, TK_LONG, TK_UNSIGNED, TK_SIGNED, TK_VOID,
    TK_STRUCT, TK_UNION, TK_ENUM, TK_TYPEDEF, TK_STATIC, TK_EXTERN,
    TK_CONST, TK_VOLATILE, TK_REGISTER, TK_AUTO, TK_INLINE,
    TK_IF, TK_ELSE, TK_WHILE, TK_DO, TK_FOR, TK_RETURN, TK_BREAK,
    TK_CONTINUE, TK_SWITCH, TK_CASE, TK_DEFAULT, TK_GOTO, TK_SIZEOF,
    TK_BOOL, /* _Bool */
    /* punctuators */
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_PLUSPLUS, TK_MINUSMINUS,
    TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PERCEQ,
    TK_AMPEQ, TK_PIPEEQ, TK_CARETEQ, TK_LSHIFTEQ, TK_RSHIFTEQ,
    TK_LSHIFT, TK_RSHIFT,
    TK_LT, TK_GT, TK_LTEQ, TK_GTEQ, TK_EQEQ, TK_NOTEQ,
    TK_ANDAND, TK_OROR,
    TK_AND, TK_OR, TK_CARET, TK_TILDE, TK_BANG,
    TK_QUESTION, TK_COLON, TK_SEMI, TK_COMMA, TK_DOT, TK_ARROW,
    TK_LPAREN, TK_RPAREN, TK_LBRACKET, TK_RBRACKET,
    TK_LBRACE, TK_RBRACE, TK_ASSIGN, TK_ELLIPSIS,
} TokKind;

typedef struct {
    TokKind kind;
    Loc loc;
    long long ival;   /* TK_NUM / TK_CHARLIT */
    int is_unsigned;  /* TK_NUM suffix U */
    int is_long;      /* TK_NUM suffix L (1) / LL (2) */
    char *sval;       /* TK_IDENT (NUL-terminated), TK_STR (bytes, may contain NUL) */
    size_t slen;      /* TK_STR byte length */
} Token;

typedef struct {
    Token *toks;
    size_t len;
    size_t cap;
} TokVec;

/* Tokenize `text` (NUL-terminated) as `filename`. Exits(1) with a
 * diagnostic on bad input; never crashes. Linemarkers of the form
 * `# <line> "<file>"` (emitted by the preprocessor) update locations. */
TokVec lex(Arena *arena, const char *filename, const char *text);

/* Human-readable token kind for --dump-tokens. */
const char *tokkind_name(TokKind k);

#endif
