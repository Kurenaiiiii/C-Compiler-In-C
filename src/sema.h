/* sema.h - semantic analysis: name resolution + type checking. */
#ifndef CCC_SEMA_H
#define CCC_SEMA_H

#include "ast.h"

/* Walk the whole program, resolve identifiers, fill node->ty / decl links,
 * evaluate case labels, and report errors via error_at (exit 1). */
void sema_analyze(void);

/* Constant evaluator for codegen (global inits, case labels). */
long long sema_eval_const(Node *n, int *ok);

#endif
