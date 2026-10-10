/* opt.h - basic AST optimizations (documented in README). */
#ifndef CCC_OPT_H
#define CCC_OPT_H

/* 1. Constant folding: binary/unary/ternary/sizeof/cast over integer
 *    constants are evaluated at compile time.
 * 2. Dead code removal: statements after return/break/continue/goto in a
 *    block are dropped (up to the next label/case/default).
 * Both run in place over the whole program. */
void opt_run(void);

#endif
