/* lexer.c - C tokenizer. Handles ints (dec/hex/oct/bin + U/L),
 * char literals + escapes, string literals + concatenation, comments,
 * all keywords/punctuators, and preprocessor linemarkers. */
#include "lexer.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char *tokkind_name(TokKind k) {
    switch (k) {
    case TK_EOF: return "eof";
    case TK_IDENT: return "ident";
    case TK_NUM: return "num";
    case TK_CHARLIT: return "char";
    case TK_STR: return "str";
    case TK_INT: return "int";
    case TK_CHAR: return "char-kw";
    case TK_SHORT: return "short";
    case TK_LONG: return "long";
    case TK_UNSIGNED: return "unsigned";
    case TK_SIGNED: return "signed";
    case TK_VOID: return "void";
    case TK_STRUCT: return "struct";
    case TK_UNION: return "union";
    case TK_ENUM: return "enum";
    case TK_TYPEDEF: return "typedef";
    case TK_STATIC: return "static";
    case TK_EXTERN: return "extern";
    case TK_CONST: return "const";
    case TK_VOLATILE: return "volatile";
    case TK_REGISTER: return "register";
    case TK_AUTO: return "auto";
    case TK_INLINE: return "inline";
    case TK_IF: return "if";
    case TK_ELSE: return "else";
    case TK_WHILE: return "while";
    case TK_DO: return "do";
    case TK_FOR: return "for";
    case TK_RETURN: return "return";
    case TK_BREAK: return "break";
    case TK_CONTINUE: return "continue";
    case TK_SWITCH: return "switch";
    case TK_CASE: return "case";
    case TK_DEFAULT: return "default";
    case TK_GOTO: return "goto";
    case TK_SIZEOF: return "sizeof";
    case TK_BOOL: return "_Bool";
    case TK_PLUS: return "+";
    case TK_MINUS: return "-";
    case TK_STAR: return "*";
    case TK_SLASH: return "/";
    case TK_PERCENT: return "%";
    case TK_PLUSPLUS: return "++";
    case TK_MINUSMINUS: return "--";
    case TK_PLUSEQ: return "+=";
    case TK_MINUSEQ: return "-=";
    case TK_STAREQ: return "*=";
    case TK_SLASHEQ: return "/=";
    case TK_PERCEQ: return "%=";
    case TK_AMPEQ: return "&=";
    case TK_PIPEEQ: return "|=";
    case TK_CARETEQ: return "^=";
    case TK_LSHIFTEQ: return "<<=";
    case TK_RSHIFTEQ: return ">>=";
    case TK_LSHIFT: return "<<";
    case TK_RSHIFT: return ">>";
    case TK_LT: return "<";
    case TK_GT: return ">";
    case TK_LTEQ: return "<=";
    case TK_GTEQ: return ">=";
    case TK_EQEQ: return "==";
    case TK_NOTEQ: return "!=";
    case TK_ANDAND: return "&&";
    case TK_OROR: return "||";
    case TK_AND: return "&";
    case TK_OR: return "|";
    case TK_CARET: return "^";
    case TK_TILDE: return "~";
    case TK_BANG: return "!";
    case TK_QUESTION: return "?";
    case TK_COLON: return ":";
    case TK_SEMI: return ";";
    case TK_COMMA: return ",";
    case TK_DOT: return ".";
    case TK_ARROW: return "->";
    case TK_LPAREN: return "(";
    case TK_RPAREN: return ")";
    case TK_LBRACKET: return "[";
    case TK_RBRACKET: return "]";
    case TK_LBRACE: return "{";
    case TK_RBRACE: return "}";
    case TK_ASSIGN: return "=";
    case TK_ELLIPSIS: return "...";
    }
    return "?";
}

typedef struct {
    const char *kw;
    TokKind kind;
} Kw;

static const Kw keywords[] = {
    {"_Bool", TK_BOOL}, {"auto", TK_AUTO}, {"break", TK_BREAK},
    {"case", TK_CASE}, {"char", TK_CHAR}, {"const", TK_CONST},
    {"continue", TK_CONTINUE}, {"default", TK_DEFAULT}, {"do", TK_DO},
    {"else", TK_ELSE}, {"enum", TK_ENUM}, {"extern", TK_EXTERN},
    {"for", TK_FOR}, {"goto", TK_GOTO}, {"if", TK_IF},
    {"inline", TK_INLINE}, {"int", TK_INT}, {"long", TK_LONG},
    {"register", TK_REGISTER}, {"return", TK_RETURN}, {"short", TK_SHORT},
    {"signed", TK_SIGNED}, {"sizeof", TK_SIZEOF}, {"static", TK_STATIC},
    {"struct", TK_STRUCT}, {"switch", TK_SWITCH}, {"typedef", TK_TYPEDEF},
    {"union", TK_UNION}, {"unsigned", TK_UNSIGNED}, {"void", TK_VOID},
    {"volatile", TK_VOLATILE}, {"while", TK_WHILE},
    {NULL, 0},
};

typedef struct {
    Arena *arena;
    const char *file;   /* current file (linemarkers may change it) */
    const char *text;
    const char *p;
    int line;
    int col;
    TokVec out;
} Lexer;

static void push(Lexer *lx, Token t) {
    if (lx->out.len == lx->out.cap) {
        size_t ncap = lx->out.cap ? lx->out.cap * 2 : 128;
        Token *nt = (Token *)arena_alloc(lx->arena, ncap * sizeof(Token));
        if (lx->out.toks)
            memcpy(nt, lx->out.toks, lx->out.len * sizeof(Token));
        lx->out.toks = nt;
        lx->out.cap = ncap;
    }
    lx->out.toks[lx->out.len++] = t;
}

static Loc here(Lexer *lx) {
    Loc l;
    l.file = lx->file;
    l.line = lx->line;
    l.col = lx->col;
    return l;
}

static void adv(Lexer *lx) {
    if (*lx->p == '\n') {
        lx->p++;
        lx->line++;
        lx->col = 1;
    } else {
        lx->p++;
        lx->col++;
    }
}

/* Escape value; *pp advanced past the escape. Errors are fatal diagnostics. */
static int escape_val(Lexer *lx, const char **pp, Loc loc) {
    (void)lx;
    const char *p = *pp; /* points after backslash */
    char c = *p;
    if (!c)
        error_at(loc, "unterminated escape sequence");
    if (c == 'n') { *pp = p + 1; return '\n'; }
    if (c == 't') { *pp = p + 1; return '\t'; }
    if (c == 'r') { *pp = p + 1; return '\r'; }
    if (c == '0' && (p[1] < '0' || p[1] > '7')) { *pp = p + 1; return '\0'; }
    if (c == 'a') { *pp = p + 1; return '\a'; }
    if (c == 'b') { *pp = p + 1; return '\b'; }
    if (c == 'f') { *pp = p + 1; return '\f'; }
    if (c == 'v') { *pp = p + 1; return '\v'; }
    if (c == '\\') { *pp = p + 1; return '\\'; }
    if (c == '\'') { *pp = p + 1; return '\''; }
    if (c == '"') { *pp = p + 1; return '"'; }
    if (c == '?') { *pp = p + 1; return '?'; }
    if (c >= '0' && c <= '7') {
        int v = 0, n = 0;
        while (n < 3 && *p >= '0' && *p <= '7') {
            v = v * 8 + (*p - '0');
            p++;
            n++;
        }
        *pp = p;
        return v & 0xFF;
    }
    if (c == 'x') {
        p++;
        if (!isxdigit((unsigned char)*p))
            error_at(loc, "invalid hex escape");
        int v = 0;
        while (isxdigit((unsigned char)*p)) {
            char d = *p++;
            v = v * 16 + (d >= '0' && d <= '9' ? d - '0'
                          : d >= 'a' && d <= 'f' ? d - 'a' + 10 : d - 'A' + 10);
        }
        *pp = p;
        return v & 0xFF;
    }
    error_at(loc, "unknown escape sequence '\\%c'", c);
    return 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

/* Try to consume a linemarker `# <n> "<file>" ...` at line start. */
static void maybe_linemarker(Lexer *lx, int at_line_start) {
    if (!at_line_start || *lx->p != '#')
        return;
    const char *q = lx->p + 1;
    while (*q == ' ' || *q == '\t')
        q++;
    if (*q < '0' || *q > '9')
        return;
    long n = 0;
    while (*q >= '0' && *q <= '9') {
        n = n * 10 + (*q - '0');
        q++;
    }
    while (*q == ' ' || *q == '\t')
        q++;
    if (*q == '"') {
        q++;
        const char *s = q;
        while (*q && *q != '"' && *q != '\n')
            q++;
        if (*q == '"') {
            lx->file = arena_strndup(lx->arena, s, (size_t)(q - s));
            while (*q && *q != '\n')
                q++;
            lx->p = q; /* leave '\n' for main loop */
            lx->line = (int)n - 1; /* next adv() on '\n' lands on n */
            return;
        }
    } else {
        /* `# 12` without filename: just adjust line. */
        const char *r = q;
        while (*r && *r != '\n')
            r++;
        /* Only treat as linemarker if rest of line is flags/blank. */
        lx->p = q;
        lx->line = (int)n - 1;
        /* do not consume to end of line; main loop skips to newline */
        return;
    }
}

TokVec lex(Arena *arena, const char *filename, const char *text) {
    Lexer lx;
    lx.arena = arena;
    lx.file = filename;
    lx.text = text;
    lx.p = text;
    lx.line = 1;
    lx.col = 1;
    lx.out.toks = NULL;
    lx.out.len = 0;
    lx.out.cap = 0;
    int at_line_start = 1;

    for (;;) {
        maybe_linemarker(&lx, at_line_start);
        char c = *lx.p;
        if (!c)
            break;
        if (c == '\n') {
            adv(&lx);
            at_line_start = 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
            adv(&lx);
            continue;
        }
        /* Comments. */
        if (c == '/' && lx.p[1] == '/') {
            while (*lx.p && *lx.p != '\n')
                adv(&lx);
            continue;
        }
        if (c == '/' && lx.p[1] == '*') {
            Loc l = here(&lx);
            adv(&lx);
            adv(&lx);
            int closed = 0;
            while (*lx.p) {
                if (lx.p[0] == '*' && lx.p[1] == '/') {
                    adv(&lx);
                    adv(&lx);
                    closed = 1;
                    break;
                }
                adv(&lx);
            }
            if (!closed)
                error_at(l, "unterminated block comment");
            continue;
        }
        at_line_start = 0;

        /* Identifiers / keywords. */
        if (isalpha((unsigned char)c) || c == '_') {
            Loc l = here(&lx);
            const char *s = lx.p;
            while (isalnum((unsigned char)*lx.p) || *lx.p == '_')
                adv(&lx);
            size_t n = (size_t)(lx.p - s);
            char *word = arena_strndup(arena, s, n);
            Token t;
            t.loc = l;
            t.ival = 0;
            t.is_unsigned = 0;
            t.is_long = 0;
            t.sval = word;
            t.slen = 0;
            t.kind = TK_IDENT;
            for (const Kw *k = keywords; k->kw; k++) {
                if (strlen(k->kw) == n && memcmp(k->kw, s, n) == 0) {
                    t.kind = k->kind;
                    break;
                }
            }
            push(&lx, t);
            continue;
        }

        /* Numbers. */
        if (isdigit((unsigned char)c)) {
            Loc l = here(&lx);
            long long v = 0;
            int base = 10;
            const char *s = lx.p;
            if (c == '0' && (lx.p[1] == 'x' || lx.p[1] == 'X')) {
                base = 16;
                adv(&lx);
                adv(&lx);
                if (!isxdigit((unsigned char)*lx.p))
                    error_at(l, "invalid hex literal");
                while (isxdigit((unsigned char)*lx.p)) {
                    v = v * 16 + hexval(*lx.p);
                    adv(&lx);
                }
            } else if (c == '0' && (lx.p[1] == 'b' || lx.p[1] == 'B')) {
                base = 2;
                adv(&lx);
                adv(&lx);
                if (*lx.p != '0' && *lx.p != '1')
                    error_at(l, "invalid binary literal");
                while (*lx.p == '0' || *lx.p == '1') {
                    v = v * 2 + (*lx.p - '0');
                    adv(&lx);
                }
            } else if (c == '0') {
                base = 8;
                while (*lx.p >= '0' && *lx.p <= '7') {
                    v = v * 8 + (*lx.p - '0');
                    adv(&lx);
                }
                if (*lx.p == '8' || *lx.p == '9')
                    error_at(l, "invalid octal digit");
            } else {
                while (isdigit((unsigned char)*lx.p)) {
                    v = v * 10 + (*lx.p - '0');
                    adv(&lx);
                }
            }
            (void)base;
            (void)s;
            int is_u = 0, is_l = 0;
            for (int i = 0; i < 3; i++) {
                char u = *lx.p;
                if ((u == 'u' || u == 'U') && !is_u) {
                    is_u = 1;
                    adv(&lx);
                } else if ((u == 'l' || u == 'L') && is_l < 2) {
                    /* L or LL. */
                    if ((u == 'l' && lx.p[1] == 'l') ||
                        (u == 'L' && lx.p[1] == 'L')) {
                        is_l = 2;
                        adv(&lx);
                        adv(&lx);
                    } else {
                        is_l++;
                        adv(&lx);
                    }
                } else
                    break;
            }
            Token t;
            t.loc = l;
            t.kind = TK_NUM;
            t.ival = v;
            t.is_unsigned = is_u;
            t.is_long = is_l;
            t.sval = NULL;
            t.slen = 0;
            push(&lx, t);
            continue;
        }

        /* Char literal. */
        if (c == '\'') {
            Loc l = here(&lx);
            adv(&lx);
            if (*lx.p == '\'')
                error_at(l, "empty character literal");
            if (*lx.p == '\n' || !*lx.p)
                error_at(l, "unterminated character literal");
            int v;
            if (*lx.p == '\\') {
                adv(&lx);
                const char *pp = lx.p;
                v = escape_val(&lx, &pp, l);
                while (lx.p < pp)
                    adv(&lx);
            } else {
                v = (unsigned char)*lx.p;
                adv(&lx);
            }
            if (*lx.p != '\'')
                error_at(l, "character literal must hold one character");
            adv(&lx);
            Token t;
            t.loc = l;
            t.kind = TK_CHARLIT;
            t.ival = v;
            t.is_unsigned = 0;
            t.is_long = 0;
            t.sval = NULL;
            t.slen = 0;
            push(&lx, t);
            continue;
        }

        /* String literal (with concatenation handled by caller loop). */
        if (c == '"') {
            Loc l = here(&lx);
            char *buf = NULL;
            size_t blen = 0, bcap = 0;
            for (;;) {
                adv(&lx); /* opening quote */
                for (;;) {
                    char d = *lx.p;
                    if (!d || d == '\n')
                        error_at(l, "unterminated string literal");
                    if (d == '"') {
                        adv(&lx);
                        break;
                    }
                    int v;
                    if (d == '\\') {
                        adv(&lx);
                        const char *pp = lx.p;
                        v = escape_val(&lx, &pp, l);
                        while (lx.p < pp)
                            adv(&lx);
                    } else {
                        v = (unsigned char)d;
                        adv(&lx);
                    }
                    if (blen + 1 >= bcap) {
                        size_t ncap = bcap ? bcap * 2 : 32;
                        char *nb = (char *)arena_alloc(arena, ncap);
                        if (buf)
                            memcpy(nb, buf, blen);
                        buf = nb;
                        bcap = ncap;
                    }
                    buf[blen++] = (char)v;
                }
                /* Skip whitespace/comments; concatenate adjacent strings. */
                const char *save_p = lx.p;
                int save_line = lx.line, save_col = lx.col;
                for (;;) {
                    char e = *lx.p;
                    if (e == ' ' || e == '\t' || e == '\r' || e == '\v' ||
                        e == '\f') {
                        adv(&lx);
                        continue;
                    }
                    if (e == '\n') {
                        adv(&lx);
                        continue;
                    }
                    if (e == '/' && lx.p[1] == '/') {
                        while (*lx.p && *lx.p != '\n')
                            adv(&lx);
                        continue;
                    }
                    if (e == '/' && lx.p[1] == '*') {
                        adv(&lx);
                        adv(&lx);
                        while (*lx.p && !(lx.p[0] == '*' && lx.p[1] == '/'))
                            adv(&lx);
                        if (*lx.p) {
                            adv(&lx);
                            adv(&lx);
                        }
                        continue;
                    }
                    break;
                }
                if (*lx.p != '"') {
                    lx.p = save_p;
                    lx.line = save_line;
                    lx.col = save_col;
                    break;
                }
            }
            char *sval = (char *)arena_alloc(arena, blen + 1);
            if (blen)
                memcpy(sval, buf, blen);
            sval[blen] = '\0';
            Token t;
            t.loc = l;
            t.kind = TK_STR;
            t.ival = 0;
            t.is_unsigned = 0;
            t.is_long = 0;
            t.sval = sval;
            t.slen = blen;
            push(&lx, t);
            continue;
        }

        /* Punctuators (longest match first). */
        {
            Loc l = here(&lx);
            Token t;
            t.loc = l;
            t.ival = 0;
            t.is_unsigned = 0;
            t.is_long = 0;
            t.sval = NULL;
            t.slen = 0;
            char d1 = lx.p[1], d2 = lx.p[2];
            switch (c) {
            case '+':
                if (d1 == '+') { t.kind = TK_PLUSPLUS; adv(&lx); adv(&lx); }
                else if (d1 == '=') { t.kind = TK_PLUSEQ; adv(&lx); adv(&lx); }
                else { t.kind = TK_PLUS; adv(&lx); }
                break;
            case '-':
                if (d1 == '-') { t.kind = TK_MINUSMINUS; adv(&lx); adv(&lx); }
                else if (d1 == '=') { t.kind = TK_MINUSEQ; adv(&lx); adv(&lx); }
                else if (d1 == '>') { t.kind = TK_ARROW; adv(&lx); adv(&lx); }
                else { t.kind = TK_MINUS; adv(&lx); }
                break;
            case '*':
                if (d1 == '=') { t.kind = TK_STAREQ; adv(&lx); adv(&lx); }
                else { t.kind = TK_STAR; adv(&lx); }
                break;
            case '/':
                if (d1 == '=') { t.kind = TK_SLASHEQ; adv(&lx); adv(&lx); }
                else { t.kind = TK_SLASH; adv(&lx); }
                break;
            case '%':
                if (d1 == '=') { t.kind = TK_PERCEQ; adv(&lx); adv(&lx); }
                else { t.kind = TK_PERCENT; adv(&lx); }
                break;
            case '<':
                if (d1 == '<' && d2 == '=') {
                    t.kind = TK_LSHIFTEQ; adv(&lx); adv(&lx); adv(&lx);
                } else if (d1 == '<') { t.kind = TK_LSHIFT; adv(&lx); adv(&lx); }
                else if (d1 == '=') { t.kind = TK_LTEQ; adv(&lx); adv(&lx); }
                else { t.kind = TK_LT; adv(&lx); }
                break;
            case '>':
                if (d1 == '>' && d2 == '=') {
                    t.kind = TK_RSHIFTEQ; adv(&lx); adv(&lx); adv(&lx);
                } else if (d1 == '>') { t.kind = TK_RSHIFT; adv(&lx); adv(&lx); }
                else if (d1 == '=') { t.kind = TK_GTEQ; adv(&lx); adv(&lx); }
                else { t.kind = TK_GT; adv(&lx); }
                break;
            case '=':
                if (d1 == '=') { t.kind = TK_EQEQ; adv(&lx); adv(&lx); }
                else { t.kind = TK_ASSIGN; adv(&lx); }
                break;
            case '!':
                if (d1 == '=') { t.kind = TK_NOTEQ; adv(&lx); adv(&lx); }
                else { t.kind = TK_BANG; adv(&lx); }
                break;
            case '&':
                if (d1 == '&') { t.kind = TK_ANDAND; adv(&lx); adv(&lx); }
                else if (d1 == '=') { t.kind = TK_AMPEQ; adv(&lx); adv(&lx); }
                else { t.kind = TK_AND; adv(&lx); }
                break;
            case '|':
                if (d1 == '|') { t.kind = TK_OROR; adv(&lx); adv(&lx); }
                else if (d1 == '=') { t.kind = TK_PIPEEQ; adv(&lx); adv(&lx); }
                else { t.kind = TK_OR; adv(&lx); }
                break;
            case '^':
                if (d1 == '=') { t.kind = TK_CARETEQ; adv(&lx); adv(&lx); }
                else { t.kind = TK_CARET; adv(&lx); }
                break;
            case '.':
                if (d1 == '.' && d2 == '.') {
                    t.kind = TK_ELLIPSIS; adv(&lx); adv(&lx); adv(&lx);
                } else
                    { t.kind = TK_DOT; adv(&lx); }
                break;
            case '?': t.kind = TK_QUESTION; adv(&lx); break;
            case ':': t.kind = TK_COLON; adv(&lx); break;
            case ';': t.kind = TK_SEMI; adv(&lx); break;
            case ',': t.kind = TK_COMMA; adv(&lx); break;
            case '~': t.kind = TK_TILDE; adv(&lx); break;
            case '(': t.kind = TK_LPAREN; adv(&lx); break;
            case ')': t.kind = TK_RPAREN; adv(&lx); break;
            case '[': t.kind = TK_LBRACKET; adv(&lx); break;
            case ']': t.kind = TK_RBRACKET; adv(&lx); break;
            case '{': t.kind = TK_LBRACE; adv(&lx); break;
            case '}': t.kind = TK_RBRACE; adv(&lx); break;
            case '#':
                error_at(l, "stray '#' (preprocessor directives must be "
                            "handled before lexing)");
                break;
            case '\\':
                error_at(l, "stray backslash");
                break;
            default:
                error_at(l, "invalid character '%c'", c);
                break;
            }
            push(&lx, t);
            continue;
        }
    }

    Token eof;
    eof.kind = TK_EOF;
    eof.loc.file = lx.file;
    eof.loc.line = lx.line;
    eof.loc.col = lx.col;
    eof.ival = 0;
    eof.is_unsigned = 0;
    eof.is_long = 0;
    eof.sval = NULL;
    eof.slen = 0;
    push(&lx, eof);
    return lx.out;
}
