/* preprocess.h - minimal C preprocessor. */
#ifndef CCC_PREPROCESS_H
#define CCC_PREPROCESS_H

#include "common.h"

typedef struct {
    char **inc_dirs;
    int n_inc;
    char **defs; /* "NAME" or "NAME=value" */
    int n_defs;
} PPOptions;

/* Expand `text` from `filename`. Returns new NUL-terminated text in the
 * arena. Handles #include "..."/<...>, object-like #define/#undef,
 * #ifdef/#ifndef/#if/#elif/#else/#endif, #error (fatal), and ignores
 * #pragma/#line. Emits GNU linemarkers so later stages keep file/line. */
char *preprocess(Arena *arena, const char *filename, const char *text,
                 PPOptions *opt);

char *read_file(const char *path);

#endif
