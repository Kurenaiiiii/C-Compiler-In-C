# Learning guide: compiler ideas through this codebase

## Glossary

- **Lexer.** Turns raw characters into tokens (words with positions), so
  the parser never thinks about whitespace or comments. In this project
  `lexer.c` produces `1:21: num 2`, and `--dump-tokens` shows its output.
- **Parser.** Turns the flat token list into a tree shaped like the
  grammar, reporting the first syntax error in words. Here `parser.c`
  uses one function per grammar rule and prints with `--dump-ast`.
- **AST (abstract syntax tree).** The tree the parser builds: operators
  as parent nodes, operands as children, with punctuation forgotten.
  `2 + 3 * 4` becomes `(+ 2 (* 3 4))`; everything downstream consumes it.
- **Recursive descent.** Parsing by nested functions that mirror grammar
  rules (`parse_stmt` calls `parse_block`, ...). Readable and easy to give
  good errors, at the cost of one function per construct.
- **Precedence climbing.** Expression parsing as a ladder of functions,
  one per precedence level, each looping over its operators. `parse_add`
  calls `parse_mul`, which is exactly why `*` binds tighter than `+`.
- **Symbol table.** The map from names to declarations, scoped so inner
  blocks can shadow outer ones. This compiler rebuilds it twice: the
  parser tracks `typedef` names and tags, `sema.c` resolves everything.
- **Stack frame.** A function's private scratch area on the machine
  stack: saved registers, spilled parameters, locals. Ours is `push rbp;
  mov rbp, rsp; sub rsp, N`, with each local at a fixed `[rbp - offset]`.
- **Calling convention.** The contract for calls: which registers carry
  arguments, who cleans the stack, where the result lands. We implement
  the System V integer subset: `rdi, rsi, rdx, rcx, r8, r9`, rest on the
  stack, result in `rax`, `%rsp == 0` before `call`.
- **ABI (application binary interface).** The whole machine-level
  contract: calling convention plus type sizes, alignment, struct layout
  and symbol naming. Matching the platform ABI is what lets our code
  call libc's `printf` and be called back.
- **Register allocation vs stack machine.** A register allocator keeps
  values in CPU registers across operations (fast, complex); a stack
  machine like ours spills every temporary to memory (simple, obviously
  correct, slower). We chose the stack machine deliberately.
- **Constant folding.** Evaluating compile-time-known expressions during
  compilation. `opt.c` turns `2 + 3 * 4` into `14`, so the emitted
  assembly is a single `movabs rax, 14`.

## Interview questions

**1. Walk me through compiling `int main(){ return 2+3*4; }`.**
Preprocess (nothing to expand, one linemarker) → lex to 13 tokens →
parse to `(funcdef main (block (return (binary + 2 (* 3 4)))))` →
sema types it as `int` → opt folds it to `14` → codegen emits the
prologue, `movabs rax, 14`, and the epilogue → system `gcc` assembles
and links. Run it: exit code 14.

**2. Why does `*` bind tighter than `+` in your parser?**
Because `parse_add` calls `parse_mul` for its operands. Each precedence
level is a function calling the next-tighter level, so the tree shape
falls out of the call graph — that call structure *is* the precedence
table.

**3. How do you tell `A * b;` (declaration) from multiplication?**
The parser keeps typedef names in scope tables. `is_declspec_tok` returns
true for a visible typedef name, so the statement parser takes the
declaration path; otherwise it parses an expression. This is the classic
C typedef-name problem, solved with a symbol table during parsing.

**4. How does `int *a[3]` differ from `int (*a)[3]`, internally?**
Prefix stars stay *pending* until the direct part binds. For `*a[3]`
the star applies to the base first, then `[3]` wraps outside (array of
pointers). After a parenthesized group, a suffix pushes *inside* a group
pointer, so `(*a)[3]` becomes pointer to array. One rule, both cases.

**5. Where do locals live, concretely?**
At `[rbp - offset]`, assigned by one layout walk per function, each
offset aligned to its type. Parameters 1–6 are spilled from their
registers on entry; the frame is padded to 16 bytes to keep `call`
aligned. `add`'s `c` is at `[rbp-20]` in the documented assembly.

**6. How do you call a 9-argument function?**
Evaluate all args, push them, pop the first six into registers; args
7–9 stay on the stack with arg6 at `[rsp]`. If the leftover count is
odd, an 8-byte pad goes *above* the stack args (a real bug we fixed:
padding below them shifts every argument). `xor eax, eax` covers
variadic calls; the return is re-extended from `eax` to `rax`.

**7. Why a stack machine instead of register allocation?**
Correctness per line of code. Every temporary on the hardware stack
means values trivially survive calls and no liveness analysis exists to
be wrong — and all 201 tests agree bit-for-bit in behavior with `gcc`.
Speed of the *output* was traded for reliability of the *compiler*.

**8. What are your optimizations, and what did you leave out?**
AST constant folding (never across division by zero), dead code after
unconditional jumps, and `if (0)`/`if (1)` branch selection. No
inlining, CSE, loop opts, or register allocation — documented as
non-goals.

**9. How do pointers and arrays actually work in your codegen?**
They are the same thing: addresses. `a[i]` is base plus scaled index
with a sized load; a bare array decays to its base address while
`sizeof` still sees the full object. Pointer differences divide by
element size; struct member access adds a compile-time
offset.

**10. How do errors avoid crashing on hostile input?**
Every stage validates: the lexer rejects bad characters, the parser
expects exact tokens, sema checks types and scopes, codegen rejects
out-of-scope values (struct by value, void dereference) with messages.
`error_at` prints file:line:col plus snippet and caret, then exits 1.
The 24 `err-*` tests assert this, including signal detection.

**11. Why an arena allocator?**
Tokens, nodes, types and scopes share one lifetime: a single
compilation. One bump allocator is O(1) per object, frees everything
with a reset, and removes per-object ownership bugs — confirmed by a
LeakSanitizer-clean full-suite run.

**12. What stops this from compiling itself, and what would it take?**
Three of eleven files already compile to valid ELF. The rest need
`struct` passed/returned by value (`Loc`, `Token` in signatures),
`va_list` support, and real system headers. Struct ABI classification
(small structs in registers, big ones via hidden pointer) is the
largest piece — a well-understood, genuinely large feature, which is
why the report draws the line there instead of half-implementing it.
