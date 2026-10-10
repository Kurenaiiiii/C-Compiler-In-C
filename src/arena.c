/* arena.c - bump allocator. See common.h for reasoning. */
#include "common.h"
#include <stdlib.h>
#include <string.h>

#define ARENA_BLOCK_CAP (64 * 1024)

Arena *g_arena = NULL;

void arena_init(Arena *a) {
    a->head = NULL;
    a->total = 0;
}

static ArenaBlock *new_block(size_t cap) {
    ArenaBlock *b = (ArenaBlock *)xmalloc(sizeof(ArenaBlock) + cap);
    b->next = NULL;
    b->used = 0;
    b->cap = cap;
    return b;
}

void *arena_alloc(Arena *a, size_t n) {
    /* 8-byte alignment for all objects (pointers, long long). */
    n = (n + 7) & ~(size_t)7;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = ARENA_BLOCK_CAP;
        if (n > cap)
            cap = n;
        ArenaBlock *b = new_block(cap);
        b->next = a->head;
        a->head = b;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    a->total += n;
    return p;
}

void *arena_calloc(Arena *a, size_t n, size_t sz) {
    size_t tot = n * sz;
    void *p = arena_alloc(a, tot);
    memset(p, 0, tot);
    return p;
}

char *arena_strdup(Arena *a, const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)arena_alloc(a, n);
    memcpy(p, s, n);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = (char *)arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void arena_reset(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *n = b->next;
        free(b);
        b = n;
    }
    a->head = NULL;
    a->total = 0;
}

void arena_free(Arena *a) {
    arena_reset(a);
}
