/* codegen.h - x86-64 Intel-syntax assembly emitter (System V ABI). */
#ifndef CCC_CODEGEN_H
#define CCC_CODEGEN_H

#include <stdio.h>

/* Emit the whole program as assembly to `out`. */
void codegen(FILE *out);

#endif
