/* parser.h - recursive-descent parser producing an AST. */
#ifndef CCC_PARSER_H
#define CCC_PARSER_H

#include "ast.h"
#include "lexer.h"

/* Parse the token stream into top-level nodes. On success, use
 * parser_prog_list()/parser_prog_count(). Errors exit(1) via error_at. */
void parse_program(TokVec *tv);
Node **parser_prog_list(void);
int parser_prog_count(void);
/* All enum constants seen (for sema's global scope). */
Decl **parser_enum_consts(int *n);

#endif
