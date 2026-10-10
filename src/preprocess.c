/* preprocess.c - line-based minimal preprocessor.
 *
 * Supported: #include (local + -I), object-like #define/#undef,
 * #ifdef/#ifndef/#if/#elif/#else/#endif, #error. #pragma/#line ignored.
 * Macro expansion is a bounded word-level substitution pass per line.
 * Includes recurse with a depth cap; standard `#ifndef GUARD` idioms work
 * because macro state is shared. Output keeps line counts (blank lines for
 * skipped/skipped-directive lines) and linemarkers for file tracking.
 */
#include "preprocess.h"
#include "lexer.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 8192, len = 0;
    char *buf = (char *)xmalloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (!buf)
                die("out of memory");
        }
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

typedef struct {
    char *name;
    char *value; /* replacement text (may be "") */
} Macro;

typedef struct {
    Macro *macs;
    int nmacs, capmacs;
    /* if-stack */
    int *active;   /* group currently emitting? per level */
    int *taken;    /* any branch taken? per level */
    int depth;
    int capdepth;
    Arena *arena;
    PPOptions *opt;
    char *out;
    size_t olen, ocap;
    char *curdir; /* dir of file being processed */
} PP;

static void pp_emit(PP *pp, const char *s, size_t n) {
    if (pp->olen + n + 1 >= pp->ocap) {
        size_t ncap = pp->ocap ? pp->ocap * 2 : 8192;
        while (ncap < pp->olen + n + 1)
            ncap *= 2;
        pp->out = (char *)realloc(pp->out, ncap);
        if (!pp->out)
            die("out of memory");
        pp->ocap = ncap;
    }
    memcpy(pp->out + pp->olen, s, n);
    pp->olen += n;
}

static void pp_emit_cstr(PP *pp, const char *s) {
    pp_emit(pp, s, strlen(s));
}

static Macro *find_macro(PP *pp, const char *name, size_t n) {
    for (int i = 0; i < pp->nmacs; i++) {
        if (strlen(pp->macs[i].name) == n &&
            memcmp(pp->macs[i].name, name, n) == 0)
            return &pp->macs[i];
    }
    return NULL;
}

static void define_macro(PP *pp, const char *name, const char *value) {
    Macro *m = find_macro(pp, name, strlen(name));
    if (m) {
        free(m->value);
        m->value = xstrdup(value);
        return;
    }
    if (pp->nmacs == pp->capmacs) {
        pp->capmacs = pp->capmacs ? pp->capmacs * 2 : 32;
        pp->macs =
            (Macro *)realloc(pp->macs, (size_t)pp->capmacs * sizeof(Macro));
        if (!pp->macs)
            die("out of memory");
    }
    pp->macs[pp->nmacs].name = xstrdup(name);
    pp->macs[pp->nmacs].value = xstrdup(value);
    pp->nmacs++;
}

static void undef_macro(PP *pp, const char *name) {
    for (int i = 0; i < pp->nmacs; i++) {
        if (strcmp(pp->macs[i].name, name) == 0) {
            free(pp->macs[i].name);
            free(pp->macs[i].value);
            pp->macs[i] = pp->macs[--pp->nmacs];
            return;
        }
    }
}

static int group_active(PP *pp) {
    for (int i = 0; i < pp->depth; i++) {
        if (!pp->active[i])
            return 0;
    }
    return 1;
}

/* Expand object-like macros in `line` (bounded iterations). */
static char *expand_line(PP *pp, const char *line) {
    char *cur = xstrdup(line);
    for (int iter = 0; iter < 50; iter++) {
        int changed = 0;
        size_t cap = strlen(cur) + 1, len = 0;
        char *out = (char *)xmalloc(cap * 2 + 64);
        size_t oc = cap * 2 + 64;
        const char *p = cur;
        while (*p) {
            if (isalpha((unsigned char)*p) || *p == '_') {
                const char *s = p;
                while (isalnum((unsigned char)*p) || *p == '_')
                    p++;
                size_t n = (size_t)(p - s);
                Macro *m = find_macro(pp, s, n);
                /* Do not expand function-like uses: NAME( ... ). */
                const char *q = p;
                while (*q == ' ' || *q == '\t')
                    q++;
                if (m && *q == '(' && strchr(m->value, '(')) {
                    /* Heuristic: if macro value looks function-like or
                     * use has paren args, skip (object-like only). */
                    /* Still skip expansion to avoid breaking calls. */
                    while (len + n + 1 >= oc) {
                        oc *= 2;
                        out = (char *)realloc(out, oc);
                    }
                    memcpy(out + len, s, n);
                    len += n;
                    continue;
                }
                if (m && !(p == s)) {
                    (void)0;
                }
                if (m) {
                    size_t vn = strlen(m->value);
                    while (len + vn + strlen(p) + 1 >= oc) {
                        oc *= 2;
                        out = (char *)realloc(out, oc);
                    }
                    memcpy(out + len, m->value, vn);
                    len += vn;
                    changed = 1;
                } else {
                    while (len + n + 1 >= oc) {
                        oc *= 2;
                        out = (char *)realloc(out, oc);
                    }
                    memcpy(out + len, s, n);
                    len += n;
                }
            } else {
                if (len + 2 >= oc) {
                    oc *= 2;
                    out = (char *)realloc(out, oc);
                }
                out[len++] = *p++;
            }
        }
        out[len] = '\0';
        free(cur);
        cur = out;
        if (!changed)
            break;
    }
    return cur;
}

/* Evaluate a simple #if expression: numbers, defined(X), !, &&, ||, ==. */
static long if_eval_expr(PP *pp, const char *s);

static void skip_spaces(const char **p) {
    while (**p == ' ' || **p == '\t')
        (*p)++;
}

static long if_primary(PP *pp, const char **p) {
    skip_spaces(p);
    if (strncmp(*p, "defined", 7) == 0 &&
        !isalnum((unsigned char)(*p)[7]) && (*p)[7] != '_') {
        *p += 7;
        skip_spaces(p);
        int paren = 0;
        if (**p == '(') {
            paren = 1;
            (*p)++;
            skip_spaces(p);
        }
        const char *s = *p;
        while (isalnum((unsigned char)**p) || **p == '_')
            (*p)++;
        size_t n = (size_t)(*p - s);
        skip_spaces(p);
        if (paren && **p == ')')
            (*p)++;
        return find_macro(pp, s, n) ? 1 : 0;
    }
    if (**p == '(') {
        (*p)++;
        long v = if_eval_expr(pp, *p);
        /* advance past matching paren: re-scan */
        int depth = 1;
        while (**p && depth) {
            if (**p == '(')
                depth++;
            else if (**p == ')') {
                depth--;
                if (!depth) {
                    (*p)++;
                    break;
                }
            }
            (*p)++;
        }
        return v;
    }
    if (**p == '!') {
        (*p)++;
        return !if_primary(pp, p);
    }
    if (**p == '-') {
        (*p)++;
        return -if_primary(pp, p);
    }
    if (isdigit((unsigned char)**p)) {
        long v = 0;
        if ((*p)[0] == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
            *p += 2;
            while (isxdigit((unsigned char)**p)) {
                char c = **p;
                v = v * 16 + (c >= '0' && c <= '9' ? c - '0'
                              : c >= 'a' ? c - 'a' + 10 : c - 'A' + 10);
                (*p)++;
            }
            return v;
        }
        while (isdigit((unsigned char)**p)) {
            v = v * 10 + (**p - '0');
            (*p)++;
        }
        return v;
    }
    if (isalpha((unsigned char)**p) || **p == '_') {
        /* Unknown identifiers in #if are 0 (except macros expanding to num). */
        const char *s = *p;
        while (isalnum((unsigned char)**p) || **p == '_')
            (*p)++;
        size_t n = (size_t)(*p - s);
        Macro *m = find_macro(pp, s, n);
        if (m && m->value[0]) {
            const char *v = m->value;
            return if_eval_expr(pp, v);
        }
        return 0;
    }
    return 0;
}

static long if_eval_expr(PP *pp, const char *s) {
    /* Handle ||, &&, ==, != with a tiny recursive split. */
    /* || */
    int depth = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '(')
            depth++;
        else if (*p == ')')
            depth--;
        else if (depth == 0 && p[0] == '|' && p[1] == '|') {
            char *l = arena_strndup(pp->arena, s, (size_t)(p - s));
            long a = if_eval_expr(pp, l);
            long b = if_eval_expr(pp, p + 2);
            return (a || b) ? 1 : 0;
        }
    }
    for (const char *p = s; *p; p++) {
        if (*p == '(')
            depth++;
        else if (*p == ')')
            depth--;
        else if (depth == 0 && p[0] == '&' && p[1] == '&') {
            char *l = arena_strndup(pp->arena, s, (size_t)(p - s));
            long a = if_eval_expr(pp, l);
            long b = if_eval_expr(pp, p + 2);
            return (a && b) ? 1 : 0;
        }
    }
    depth = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '(')
            depth++;
        else if (*p == ')')
            depth--;
        else if (depth == 0 && p[0] == '=' && p[1] == '=') {
            char *l = arena_strndup(pp->arena, s, (size_t)(p - s));
            const char *a = l, *b = p + 2;
            long x = if_primary(pp, &a);
            long y = if_primary(pp, &b);
            return (x == y) ? 1 : 0;
        } else if (depth == 0 && p[0] == '!' && p[1] == '=') {
            char *l = arena_strndup(pp->arena, s, (size_t)(p - s));
            const char *a = l, *b = p + 2;
            long x = if_primary(pp, &a);
            long y = if_primary(pp, &b);
            return (x != y) ? 1 : 0;
        }
    }
    const char *p = s;
    long v = if_primary(pp, &p);
    skip_spaces(&p);
    return v;
}

static void if_push(PP *pp, int cond) {
    if (pp->depth == pp->capdepth) {
        pp->capdepth = pp->capdepth ? pp->capdepth * 2 : 16;
        pp->active =
            (int *)realloc(pp->active, (size_t)pp->capdepth * sizeof(int));
        pp->taken =
            (int *)realloc(pp->taken, (size_t)pp->capdepth * sizeof(int));
        if (!pp->active || !pp->taken)
            die("out of memory");
    }
    int parent = group_active(pp);
    pp->active[pp->depth] = parent && cond;
    pp->taken[pp->depth] = parent && cond;
    pp->depth++;
}

static char *dir_of(const char *path) {
    const char *s = strrchr(path, '/');
    if (!s)
        return xstrdup(".");
    if (s == path)
        return xstrdup("/");
    size_t n = (size_t)(s - path);
    char *d = (char *)xmalloc(n + 1);
    memcpy(d, path, n);
    d[n] = '\0';
    return d;
}

static int is_ident(const char *s) {
    if (!s[0] || !(isalpha((unsigned char)s[0]) || s[0] == '_'))
        return 0;
    for (const char *p = s + 1; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_')
            return 0;
    }
    return 1;
}

static void process(PP *pp, const char *filename, const char *text, int depth,
                    int *lineno_out);

static void include_file(PP *pp, const char *from_file, const char *inc,
                         int angled, int depth, Loc loc) {
    char *path = NULL;
    FILE *test = NULL;
    if (!angled) {
        char *d = dir_of(from_file);
        size_t n = strlen(d) + 1 + strlen(inc) + 1;
        path = (char *)xmalloc(n);
        snprintf(path, n, "%s/%s", d, inc);
        free(d);
        test = fopen(path, "rb");
        if (test) {
            fclose(test);
        } else {
            free(path);
            path = NULL;
        }
    }
    if (!path) {
        /* Search -I dirs then current dir. */
        for (int i = 0; i < pp->opt->n_inc && !path; i++) {
            size_t n = strlen(pp->opt->inc_dirs[i]) + 1 + strlen(inc) + 1;
            char *c = (char *)xmalloc(n);
            snprintf(c, n, "%s/%s", pp->opt->inc_dirs[i], inc);
            test = fopen(c, "rb");
            if (test) {
                fclose(test);
                path = c;
            } else
                free(c);
        }
    }
    if (!path) {
        test = fopen(inc, "rb");
        if (test) {
            fclose(test);
            path = xstrdup(inc);
        }
    }
    if (!path) {
        error_at(loc, "cannot find include file '%s'", inc);
        return;
    }
    char *content = read_file(path);
    if (!content) {
        error_at(loc, "cannot read include file '%s'", path);
        free(path);
        return;
    }
    int dummy = 0;
    char marker[1024];
    snprintf(marker, sizeof marker, "# 1 \"%s\"\n", path);
    pp_emit_cstr(pp, marker);
    process(pp, path, content, depth + 1, &dummy);
    free(content);
    snprintf(marker, sizeof marker, "# 1 \"%s\"\n", pp->curdir ? from_file : from_file);
    pp_emit_cstr(pp, marker);
    free(path);
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* Process one file's text. Emits expanded lines + linemarkers. */
static void process(PP *pp, const char *filename, const char *text, int depth,
                    int *lineno_out) {
    if (depth > 32) {
        Loc l;
        l.file = filename;
        l.line = 1;
        l.col = 1;
        error_at(l, "include nesting too deep");
    }
    char *savedir = pp->curdir;
    pp->curdir = dir_of(filename);
    const char *p = text;
    int lineno = 1;
    char marker[1024];
    snprintf(marker, sizeof marker, "# 1 \"%s\"\n", filename);
    if (depth == 0)
        pp_emit_cstr(pp, marker);

    /* Split into lines keeping newlines. Handle backslash-newline splice. */
    size_t tlen = strlen(text);
    char *flat = (char *)xmalloc(tlen + 1);
    size_t fi = 0;
    for (size_t i = 0; i < tlen; i++) {
        if (text[i] == '\\' && text[i + 1] == '\n') {
            i++;
            continue;
        }
        if (text[i] == '\\' && text[i + 1] == '\r' && text[i + 2] == '\n') {
            i += 2;
            continue;
        }
        flat[fi++] = text[i];
    }
    flat[fi] = '\0';

    const char *line = flat;
    while (1) {
        const char *eol = strchr(line, '\n');
        int last = !eol;
        size_t llen = last ? strlen(line) : (size_t)(eol - line);
        char *lbuf = (char *)xmalloc(llen + 1);
        memcpy(lbuf, line, llen);
        lbuf[llen] = '\0';

        const char *s = skip_ws(lbuf);
        int is_directive = (s[0] == '#');
        if (is_directive) {
            const char *d = skip_ws(s + 1);
            char word[32];
            size_t wi = 0;
            while ((isalnum((unsigned char)*d) || *d == '_') && wi + 1 < sizeof word)
                word[wi++] = *d++;
            word[wi] = '\0';
            const char *rest = skip_ws(d);
            if (strcmp(word, "include") == 0) {
                if (group_active(pp)) {
                    char inc[512];
                    int angled = 0;
                    if (rest[0] == '"') {
                        size_t i = 1;
                        while (rest[i] && rest[i] != '"' && i + 1 < sizeof inc) {
                            inc[i - 1] = rest[i];
                            i++;
                        }
                        inc[i - 1] = '\0';
                    } else if (rest[0] == '<') {
                        angled = 1;
                        size_t i = 1, o = 0;
                        while (rest[i] && rest[i] != '>' && o + 1 < sizeof inc)
                            inc[o++] = rest[i++];
                        inc[o] = '\0';
                    } else {
                        Loc l;
                        l.file = filename;
                        l.line = lineno;
                        l.col = 1;
                        error_at(l, "malformed #include");
                        inc[0] = '\0';
                    }
                    /* Expand macros in include name (simple). */
                    char *exp = expand_line(pp, inc);
                    Loc l;
                    l.file = filename;
                    l.line = lineno;
                    l.col = 1;
                    include_file(pp, filename, exp, angled, depth, l);
                    free(exp);
                }
                pp_emit_cstr(pp, "\n");
            } else if (strcmp(word, "define") == 0) {
                if (group_active(pp)) {
                    const char *r = rest;
                    const char *ns = r;
                    while (isalnum((unsigned char)*r) || *r == '_')
                        r++;
                    char name[128];
                    size_t nn = (size_t)(r - ns);
                    if (nn == 0 || nn >= sizeof name || !is_ident(arena_strndup(pp->arena, ns, nn))) {
                        /* malformed; ignore with blank */
                    } else {
                        memcpy(name, ns, nn);
                        name[nn] = '\0';
                        if (*r == '(') {
                            /* Function-like macro: not supported; store raw
                             * so uses stay unexpanded (documented). Skip. */
                        } else {
                            const char *v = skip_ws(r);
                            /* rstrip */
                            char *vv = xstrdup(v);
                            size_t vn = strlen(vv);
                            while (vn && (vv[vn - 1] == ' ' || vv[vn - 1] == '\t' ||
                                          vv[vn - 1] == '\r'))
                                vv[--vn] = '\0';
                            define_macro(pp, name, vv);
                            free(vv);
                        }
                    }
                }
                pp_emit_cstr(pp, "\n");
            } else if (strcmp(word, "undef") == 0) {
                if (group_active(pp)) {
                    char name[128];
                    size_t i = 0;
                    while ((isalnum((unsigned char)*rest) || *rest == '_') &&
                           i + 1 < sizeof name)
                        name[i++] = *rest++;
                    name[i] = '\0';
                    if (i)
                        undef_macro(pp, name);
                }
                pp_emit_cstr(pp, "\n");
            } else if (strcmp(word, "ifdef") == 0) {
                char name[128];
                size_t i = 0;
                while ((isalnum((unsigned char)*rest) || *rest == '_') &&
                       i + 1 < sizeof name)
                    name[i++] = *rest++;
                name[i] = '\0';
                int cond = i && find_macro(pp, name, i);
                if_push(pp, group_active(pp) ? cond : 0);
                /* If parent inactive, force inactive regardless. */
                if (!group_active(pp) && pp->depth > 0) {
                    /* already handled: parent false => active false */
                }
                pp_emit_cstr(pp, "\n");
            } else if (strcmp(word, "ifndef") == 0) {
                char name[128];
                size_t i = 0;
                while ((isalnum((unsigned char)*rest) || *rest == '_') &&
                       i + 1 < sizeof name)
                    name[i++] = *rest++;
                name[i] = '\0';
                int cond = !(i && find_macro(pp, name, i));
                if_push(pp, group_active(pp) ? cond : 0);
                pp_emit_cstr(pp, "\n");
            } else if (strcmp(word, "if") == 0) {
                int cond = 0;
                if (group_active(pp)) {
                    char *exp = expand_line(pp, rest);
                    cond = (int)if_eval_expr(pp, exp);
                    free(exp);
                }
                if_push(pp, cond);
                pp_emit_cstr(pp, "\n");
            } else if (strcmp(word, "elif") == 0) {
                if (pp->depth == 0) {
                    Loc l;
                    l.file = filename;
                    l.line = lineno;
                    l.col = 1;
                    error_at(l, "#elif without #if");
                }
                int parent = 1;
                for (int i = 0; i < pp->depth - 1; i++)
                    parent = parent && pp->active[i];
                int cond = 0;
                if (parent && !pp->taken[pp->depth - 1]) {
                    char *exp = expand_line(pp, rest);
                    cond = (int)if_eval_expr(pp, exp);
                    free(exp);
                }
                pp->active[pp->depth - 1] = parent && cond;
                if (cond)
                    pp->taken[pp->depth - 1] = 1;
                pp_emit_cstr(pp, "\n");
            } else if (strcmp(word, "else") == 0) {
                if (pp->depth == 0) {
                    Loc l;
                    l.file = filename;
                    l.line = lineno;
                    l.col = 1;
                    error_at(l, "#else without #if");
                }
                int parent = 1;
                for (int i = 0; i < pp->depth - 1; i++)
                    parent = parent && pp->active[i];
                pp->active[pp->depth - 1] =
                    parent && !pp->taken[pp->depth - 1];
                pp->taken[pp->depth - 1] = 1;
                pp_emit_cstr(pp, "\n");
            } else if (strcmp(word, "endif") == 0) {
                if (pp->depth == 0) {
                    Loc l;
                    l.file = filename;
                    l.line = lineno;
                    l.col = 1;
                    error_at(l, "#endif without #if");
                }
                pp->depth--;
                pp_emit_cstr(pp, "\n");
            } else if (strcmp(word, "error") == 0) {
                if (group_active(pp)) {
                    Loc l;
                    l.file = filename;
                    l.line = lineno;
                    l.col = 1;
                    error_at(l, "#error %s", rest);
                }
                pp_emit_cstr(pp, "\n");
            } else if (strcmp(word, "pragma") == 0 || strcmp(word, "line") == 0) {
                pp_emit_cstr(pp, "\n");
            } else if (word[0] == '\0') {
                pp_emit_cstr(pp, "\n"); /* bare # */
            } else {
                if (group_active(pp)) {
                    Loc l;
                    l.file = filename;
                    l.line = lineno;
                    l.col = 1;
                    error_at(l, "unknown preprocessor directive '#%s'", word);
                }
                pp_emit_cstr(pp, "\n");
            }
        } else {
            if (group_active(pp)) {
                char *exp = expand_line(pp, lbuf);
                pp_emit(pp, exp, strlen(exp));
                free(exp);
                pp_emit_cstr(pp, "\n");
            } else {
                pp_emit_cstr(pp, "\n");
            }
        }
        free(lbuf);
        lineno++;
        if (last)
            break;
        line = eol + 1;
    }
    free(flat);
    free(pp->curdir);
    pp->curdir = savedir;
    if (lineno_out)
        *lineno_out = lineno;
    if (depth == 0 && pp->depth != 0) {
        Loc l;
        l.file = filename;
        l.line = lineno;
        l.col = 1;
        error_at(l, "unterminated #if");
    }
    (void)p;
}

char *preprocess(Arena *arena, const char *filename, const char *text,
                 PPOptions *opt) {
    PP pp;
    memset(&pp, 0, sizeof pp);
    pp.arena = arena;
    pp.opt = opt;
    for (int i = 0; i < opt->n_defs; i++) {
        char *d = opt->defs[i];
        char *eq = strchr(d, '=');
        if (eq) {
            char *name = arena_strndup(arena, d, (size_t)(eq - d));
            define_macro(&pp, name, eq + 1);
        } else {
            define_macro(&pp, d, "1");
        }
    }
    int dummy = 0;
    process(&pp, filename, text, 0, &dummy);
    pp_emit(&pp, "", 1);
    pp.out[pp.olen - 1] = '\0';
    char *res = arena_strndup(arena, pp.out, pp.olen - 1);
    for (int i = 0; i < pp.nmacs; i++) {
        free(pp.macs[i].name);
        free(pp.macs[i].value);
    }
    free(pp.macs);
    free(pp.active);
    free(pp.taken);
    free(pp.out);
    free(pp.curdir);
    return res;
}
