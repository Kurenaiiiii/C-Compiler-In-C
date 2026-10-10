/* common.h - shared utilities: arena allocator, source locations, errors.
 *
 * Reasoning for arena allocation: the compiler builds thousands of small,
 * short-lived objects (tokens, AST nodes, types) with highly variable
 * lifetimes tied to a single compilation. An arena (bump allocator) makes
 * allocation O(1), frees everything with one reset, and eliminates an
 * entire class of use-after-free / leak bugs. Individual free() is never
 * needed because nothing outlives one compilation. See arena.c.
 */
#ifndef CCC_COMMON_H
#define CCC_COMMON_H

#include <stddef.h>
#include <stdio.h>

/* ---------- arena ---------- */
typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used;
    size_t cap;
    char data[];
} ArenaBlock;

typedef struct {
    ArenaBlock *head;
    size_t total;
} Arena;

void arena_init(Arena *a);
void *arena_alloc(Arena *a, size_t n);
void *arena_calloc(Arena *a, size_t n, size_t sz);
char *arena_strdup(Arena *a, const char *s);
char *arena_strndup(Arena *a, const char *s, size_t n);
void arena_reset(Arena *a);
void arena_free(Arena *a);

/* ---------- source locations ---------- */
typedef struct {
    const char *file;
    int line;
    int col;
} Loc;

extern Arena *g_arena; /* active compilation arena (for short-lived strings) */

/* ---------- errors ---------- */
/* Register the full source text so diagnostics can print snippets. */
void error_set_source(const char *file, const char *text);
/* Print "file:line:col: error: msg", snippet + caret, then exit(1).
 * Never returns. Never crashes on invalid input. */
void error_at(Loc loc, const char *fmt, ...);

/* ---------- util ---------- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
char *xstrdup(const char *s);
void die(const char *fmt, ...); /* fatal compiler bug / OOM */

#endif
