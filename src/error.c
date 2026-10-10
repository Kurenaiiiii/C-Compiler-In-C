/* error.c - diagnostics with file:line:col + snippet + caret. */
#include "common.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char *src_file = "<input>";
static const char *src_text = NULL;
static int nerrors = 0;

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "ccc: out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) {
        fprintf(stderr, "ccc: out of memory\n");
        exit(1);
    }
    return p;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ccc: internal error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

void error_set_source(const char *file, const char *text) {
    src_file = file ? file : "<input>";
    src_text = text;
    (void)src_file;
}

int error_count(void) {
    return nerrors;
}

/* Print the source line for (file, line) plus a caret under `col`.
 * The source is preprocessed text containing GNU linemarkers
 * (`# <n> "<file>"`); track them so logical locations map to the right
 * physical line. Falls back to the raw line when no marker matches. */
static void print_snippet(const char *file, int line, int col) {
    if (!src_text)
        return;
    const char *p = src_text;
    const char *cur_file = NULL;
    int cur_line = 1;
    const char *match = NULL;
    const char *first_lines[3];
    int nf = 0;
    while (*p) {
        const char *ls = p;
        const char *le = ls;
        while (*le && *le != '\n')
            le++;
        /* Check for linemarker. */
        if (le - ls > 2 && ls[0] == '#') {
            const char *q = ls + 1;
            while (q < le && (*q == ' ' || *q == '\t'))
                q++;
            if (q < le && *q >= '0' && *q <= '9') {
                long n = 0;
                while (q < le && *q >= '0' && *q <= '9') {
                    n = n * 10 + (*q - '0');
                    q++;
                }
                while (q < le && (*q == ' ' || *q == '\t'))
                    q++;
                if (q < le && *q == '"') {
                    q++;
                    const char *s = q;
                    while (q < le && *q != '"')
                        q++;
                    size_t fnlen = (size_t)(q - s);
                    char *fn = (char *)malloc(fnlen + 1);
                    if (fn) {
                        memcpy(fn, s, fnlen);
                        fn[fnlen] = '\0';
                        free((void *)cur_file);
                        cur_file = fn;
                    }
                }
                cur_line = (int)n;
                /* marker lines are not source lines; do not increment */
                p = *le ? le + 1 : le;
                continue;
            }
        }
        if (nf < 3 && !cur_file)
            first_lines[nf++] = ls;
        if (cur_file && file && strcmp(cur_file, file) == 0 &&
            cur_line == line) {
            match = ls;
            break;
        }
        if (!cur_file && cur_line == line && !match) {
            match = ls;
            break;
        }
        cur_line++;
        p = *le ? le + 1 : le;
    }
    const char *ls = match ? match : (nf > 0 ? first_lines[0] : NULL);
    if (!ls)
        return;
    const char *le = ls;
    while (*le && *le != '\n')
        le++;
    size_t len = (size_t)(le - ls);
    if (len > 200)
        len = 200;
    fprintf(stderr, "  %.*s\n", (int)len, ls);
    fprintf(stderr, "  ");
    int c = 1;
    for (const char *q = ls; q < ls + (ptrdiff_t)len && c < col; q++, c++)
        fputc(*q == '\t' ? '\t' : ' ', stderr);
    fprintf(stderr, "^\n");
    free((void *)cur_file);
}

void error_at(Loc loc, const char *fmt, ...) {
    nerrors++;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: error: ", loc.file ? loc.file : src_file,
            loc.line, loc.col);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    print_snippet(loc.file ? loc.file : src_file, loc.line, loc.col);
    exit(1);
}

void error_at_token(const char *file, int line, int col, const char *fmt, ...) {
    nerrors++;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: error: ", file ? file : src_file, line, col);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    print_snippet(file ? file : src_file, line, col);
    exit(1);
}

void warn_at(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: warning: ", loc.file ? loc.file : src_file,
            loc.line, loc.col);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}
