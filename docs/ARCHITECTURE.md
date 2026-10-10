# Architecture: how C Compiler in C works

This document walks through the whole compiler in plain English. The only
program you need to follow along is this one:

```c
int main() { return 2 + 3 * 4; }
```

It returns 14. We will watch the compiler turn those 33 characters into a
running program.

## The pipeline in one picture

```
source text
    |  preprocess.c   (#include, #define, #ifdef  ->  flat text + linemarkers)
    v
tokens                  (lexer.c: words, numbers, punctuation with locations)
    |  parser.c        (recursive descent + precedence climbing)
    v
AST                     (ast.c: tree of what the program MEANS)
    |  sema.c          (who is each name? what type is each expression?)
    v
typed AST
    |  opt.c           (fold constants, drop dead code)
    v
smaller typed AST
    |  codegen.c       (x86-64 Intel-syntax assembly, System V ABI)
    v
assembly text  ->  system gcc (assembler + linker only)  ->  executable
```

The compiler itself never uses `gcc` to understand the user's C. It only
calls `gcc file.s -o program` at the very end, the same way you would call
an assembler by hand.

## Stage 1: preprocessing (preprocess.c)

Real C files start with lines like `#include "helper.h"`. The lexer does
not want to see those, so a small line-based pass runs first:

- `#include "file"` pastes the file in (searched next to the includer,
  then in `-I` directories). Depth is capped at 32 against cycles.
- `#define NAME replacement` stores object-like macros; later lines get
  word-level substitution (bounded at 50 passes, so recursive macros
  terminate).
- `#ifdef` / `#ifndef` / `#if` / `#else` / `#endif` skip inactive groups.
  `#if` understands numbers, `defined(X)`, `!`, `&&`, `||`, `==`, `!=`.
- `#error` stops with your message. `#pragma` / `#line` are ignored.

Two details keep everything else honest. Skipped lines are emitted as
*blank* lines so line numbers survive, and file boundaries are marked with
GNU linemarkers:

```
# 1 "demo.c"
int main() { return 2 + 3 * 4; }
```

The lexer reads those markers, so every later error still says the right
file and line even after includes and macros.

Trade-off: function-like macros (`#define ADD(a,b) ...`) are parsed and
skipped, never expanded. Supporting them would mean a token-level
macro engine; object-like macros cover constants and include guards, which
is what small programs need.

## Stage 2: lexing (lexer.c)

The lexer turns characters into *tokens*: typed words with a file, line
and column. For our program, `--dump-tokens` prints exactly this:

```
1:1: 'int'
1:5: ident 'main'
1:9: '('
1:10: ')'
1:12: '{'
1:14: 'return'
1:21: num 2
1:23: '+'
1:25: num 3
1:27: '*'
1:29: num 4
1:30: ';'
1:32: '}'
3:1: eof
```

It handles decimal/hex/octal/binary integers with `U`/`L` suffixes, char
literals (`'a'`, `'\n'`, `'\x41'`, octal escapes), string literals with
escape processing and adjacent concatenation (`"a" "b"` becomes `"ab"`),
`//` and `/* */` comments, and every keyword and punctuator the parser
needs. Anything else (`@`, a stray backslash, an unterminated string) is
a clean error with a snippet and a caret, never a crash.

## Stage 3: parsing (parser.c, ast.c, types.c)

The parser turns the flat token list into a tree (the AST) that mirrors
the program's meaning. `--dump-ast` prints ours like this:

```
(funcdef main
  (block
    (return
      (binary +
        (num 2 int)
        (binary *
          (num 3 int)
          (num 4 int)))))
```

Notice `3 * 4` grouped first: the parser climbs precedence levels
(`*` binds tighter than `+`), the same precedence table C uses, so
`2 + 3 * 4` means `2 + (3 * 4)`.

How it is organized:

- *Recursive descent* for structure: a function per grammar rule
  (`parse_stmt` calls `parse_block`, `parse_if`, ...). This is easy to
  read and gives good error messages ("expected ';', found '}'").
- *Precedence climbing* for expressions: one function per level
  (`parse_add` calls `parse_mul` calls `parse_cast` ... down to
  `parse_primary`). Each level loops over its operators, building
  left-leaning `ND_BINARY` nodes; assignment is right-leaning by
  recursing.
- C's hard part, declarators, is handled by one rule: prefix `*`s are
  *pending* until the direct part decides where they bind. That single
  rule makes `int *a[3]` (array of pointers), `int (*f)(int)` (pointer
  to function) and `Node *(*next)(Parser *)` all come out right.
- `typedef` names live in scope tables during parsing, because C needs
  to know whether `A * b;` declares `b` or multiplies `A` by `b`.
  Struct/union/enum tags and enum constants are collected the same way.

Types (`types.c`) are small descriptors: kind, size, alignment, and for
pointers/arrays/structs a link to the element or member list. `int` is
4 bytes aligned to 4; pointers and `long` are 8 and 8; structs get member
offsets with padding exactly like the platform ABI requires
(`struct { char c; int x; }` is 8 bytes, `x` at offset 4).

## Stage 4: semantic analysis (sema.c)

The parser knows shapes; sema knows *meanings*. It rebuilds scopes from
the top-level declarations (globals first, so forward references work),
then walks every function:

- each `x` becomes a link to its declaration (or a clean "undeclared
  identifier" error);
- each expression gets a concrete type (`2 + 3 * 4` is `int`);
- rules are enforced: you cannot assign to `1`, dereference an `int`,
  `break` outside a loop, `case` outside a `switch`, call with the wrong
  argument count, or jump to a missing label;
- `p == 0` is accepted for pointers (null constant), `sizeof` never
  decays arrays, enum constants become plain numbers.

`sema_eval_const` evaluates anything compile-time-known (case labels,
global initializers, array sizes, enumerators). If it cannot prove a
value constant, the relevant error fires here, in words, instead of a
crash later.

## Stage 5: optimization (opt.c)

Two small, documented passes run on the typed AST:

1. *Constant folding.* `2 + 3 * 4` becomes the single node `14` before
   any assembly is emitted (that is why the assembly below says
   `movabs rax, 14`). It folds arithmetic, comparisons, shifts,
   `sizeof`, casts and `cond ? a : b` when the condition is constant —
   but never folds division by zero, leaving the trap for runtime.
2. *Dead-code removal.* Statements after `return`, `break`, `continue`
   or `goto` are dropped up to the next label/`case`. `if (0)` and
   `if (1)` simplify to the live branch.

Deliberately absent: register allocation, inlining, loop optimization.
The generated code is straightforward rather than fast; correctness and
readability of the compiler win over speed of its output.

## Stage 6: code generation (codegen.c)

The back end is a *stack machine*: every expression leaves a canonical
64-bit value in `RAX` (sign- or zero-extended for its type), and
temporaries spill to the hardware stack with `push`/`pop`, so values
survive function calls. Addresses are computed into `RAX` by a separate
`gen_lval` path (`&x`, `*p`, `a[i]`, `s.member`).

Our demo needs no variables, so it is short:

```asm
  .intel_syntax noprefix
  .globl main
  .text
main:
  push rbp
  mov rbp, rsp
  sub rsp, 16
  movabs rax, 14
  jmp .Lmain_end
.Lmain_end:
  mov rsp, rbp
  pop rbp
  ret
  .section .rodata
```

### Stack frames: where locals live

```
high addresses
  ... caller's frame ...
  [rbp+16]  7th argument (arg6)
  [rbp+8]   return address
  [rbp]     saved rbp  <-- rbp points here
  [rbp-8]   switch-value spill slot (always reserved)
  [rbp-12]  int a      (param spilled from edi)
  [rbp-16]  int b      (param spilled from esi)
  [rbp-20]  int c      (local)
  ...         (frame padded to a multiple of 16)
low addresses  <-- rsp points here
```

Layout walks each function once, assigning every local and spilled
parameter an end-offset from `rbp`, each aligned to its type (capped at
16). The frame size is rounded up to 16 so the stack stays aligned for
calls. Parameters 1-6 arrive in `rdi, rsi, rdx, rcx, r8, r9` and are
immediately spilled to their slots with sized stores (`mov BYTE PTR`
for `char`, etc.); parameters 7+ are copied down from above `rbp`.
Static locals and string literals live outside the frame (see below),
addressed relative to the instruction pointer (`lea rax, [rip + label]`),
which keeps them working under position-independent executables.

### Calling convention (System V, integer side)

```
caller:  evaluate args right-to-left, push all
         pop first six into rdi,rsi,rdx,rcx,r8,r9
         leftover args 7+ sit at [rsp],[rsp+8],... (arg6 at [rsp])
         pad with 8 bytes if that count is odd (alignment!)
         xor eax, eax        # no vector registers used
         call target
         add rsp, cleanup
         canonicalize eax -> rax for the return type
callee:  push rbp; mov rbp, rsp; sub rsp, frame
         spill registers, run body, leave result in rax
         mov rsp, rbp; pop rbp; ret
```

The padding rule is the subtle one: `call` needs `%rsp == 0`, and an odd
number of leftover 8-byte slots would break it, so the compiler emits an
extra `sub rsp, 8` *above* the stack arguments (never below them, or the
callee would read the wrong slots). Indirect calls (`fptr(...)`) load
the target into `r11` for the same reason. Return values come back in
`rax`, re-extended to 64 bits because libc helpers like `printf` only
define `eax`.

### Pointers and arrays in generated code

There is no separate "pointer machine": an array *is* its address, and
every access is address arithmetic plus a sized load/store.

- `int *p = &x;` → `lea rax, [rbp - off]` (address of `x`).
- `*p` (rvalue) → evaluate `p` to an address, then `movsxd rax,
  DWORD PTR [rax]` (or `movzx` for unsigned narrow types).
- `p + 3` for `int *p` → `imul rcx, rcx, 4` then `add`. Subtracting two
  pointers divides the byte difference by the element size. `p == 0`
  compares the raw address.
- `a[i]` → base address (leaq for locals/globals, value for pointers)
  plus `i * element-size`, then the same sized load. `a` alone in an
  expression decays to that base address; `sizeof(a)` still sees all
  40 bytes of an `int[10]`.
- `s.member` adds the compile-time member offset (structs were laid out
  in `types.c`); `p->m` first loads the pointer. Struct assignment is an
  inline copy loop. Structs by value as arguments or returns are
  rejected with a message — pointers cover every real use in scope.

### Globals, strings, switches

- Globals and static locals go to `.data` (initialized) or `.bss`
  (zero), with `.globl` unless `static`. Initializers must be constant;
  `int *p = &x` becomes `.quad x` for the linker to fill in.
- Each string literal gets a `.LstrN` label in `.rodata` with exact
  `.byte` contents plus terminator. `char s[] = "hi"` copies (and
  zero-fills the tail); `char *s = "hi"` just takes the address.
- `switch` spills its value once to `[rbp-8]`, jumps over the body to a
  dispatcher of `cmp/je` pairs (equality needs no signedness), and every
  `break` rejoins at one label. Cases are collected before emission, so
  nesting and fall-through work; Duff-style interleavings do not.

## Design decisions and trade-offs

- *One value in RAX, everything else on the stack.* A real register
  allocator would be faster but is a project of its own; the stack
  machine is obviously correct (every one of the 201 tests agrees with
  `gcc`), and the assembly stays easy to read in this document.
- *Spill all parameters.* Keeping arguments in registers across the
  function would need liveness analysis. Spilling costs a few moves and
  removes the entire problem class.
- *Arena allocation.* Tokens, nodes, types and scopes all die together
  when compilation ends, so one bump allocator replaces thousands of
  `malloc`/`free` pairs and an entire category of leaks (verified with
  LeakSanitizer: zero leaks on success paths).
- *Exit-on-first-error.* The compiler prints one precise diagnostic and
  stops instead of cascading guesses. Simpler than recovery, and for a
  compiler this size, more accurate.
- *Intel syntax.* AT&T is the GNU default, but `mov rax, [rbp - 8]`
  reads better in documentation and `gcc` assembles it identically.

## Memory and safety notes

- The fixed-size walk stacks that existed in early drafts were replaced
  with recursion after review: deep but valid input must not silently
  drop nodes.
- Break/continue label stacks are 1024 deep; deeper nesting is rejected
  only by running out, which no real program approaches.
- The only known crash class during development (mixed struct layouts
  from stale objects) was eliminated with compiler-generated header
  dependencies (`-MMD -MP`) in the Makefile.
