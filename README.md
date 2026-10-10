# C Compiler in C

An independent educational project: a compiler for a substantial subset of
C11, written in C11, that emits x86-64 assembly and produces executables
through the system assembler and linker. It has no affiliation with any
compiler vendor; it exists to learn how C compilers work by building one.

## Features

- Complete pipeline: preprocess, lex, parse (AST), semantic analysis,
  constant folding, x86-64 code generation (System V ABI), assemble and
  link via the system `gcc` driver (used only as assembler/linker).
- Integers: `char`, `short`, `int`, `long` and `unsigned` variants, with
  promotions and usual arithmetic conversions.
- Functions with up to 6 register arguments plus stack arguments,
  recursion, forward declarations, function pointers, calls into libc
  (`printf`, `putchar`, ...).
- Pointers (including pointer-to-pointer and function pointers),
  arrays (including multidimensional), structs, unions, enums, `typedef`.
- `if`/`else`, `while`, `do`-`while`, `for`, `switch`/`case`, `break`,
  `continue`, `goto` with labels, blocks with proper scoping.
- Globals with initializers, `static` locals, `extern`, string literals
  with escapes, `sizeof`.
- Minimal preprocessor: local `#include`, object-like `#define`,
  `#ifdef`/`#ifndef`/`#if`/`#else`/`#endif`, include guards.
- Diagnostics with file, line, column, source snippet and caret; invalid
  input never crashes the compiler.
- Zero dependencies beyond libc. Single `Makefile`. Clean build with
  `-Wall -Wextra -Werror`.

## Quick start

Requires `gcc`, `make`, `binutils` and `git` (all present on a normal
Fedora install).

```
make
printf 'int main() { return 2 + 3 * 4; }\n' > hello.c
./ccc hello.c -o hello
./hello; echo $?
```

Expected output:

```
14
```

## Usage

```
ccc [options] file.c [-o out]
  -o FILE         output file (executable, or assembly with -S)
  -S              emit assembly instead of an executable
  -c              assemble to an object file instead of linking
  -E              print preprocessed source
  --dump-tokens   print the token list
  --dump-ast      print the abstract syntax tree
  -I DIR          add an include search directory
  -D NAME[=VAL]   predefine a macro
  -O0             disable AST optimizations (on by default)
```

Examples with real input and output:

```
$ cat fib.c
int printf(char *fmt, ...);
int fib(int n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
int main() {
  for (int i = 0; i < 8; i++) printf("%d ", fib(i));
  printf("\n");
  return 0;
}
$ ./ccc fib.c -o fib && ./fib
0 1 1 2 3 5 8 13
$ ./ccc -S fib.c -o fib.s && head -12 fib.s
  .intel_syntax noprefix
  .globl fib
  .text
fib:
  push rbp
  mov rbp, rsp
  sub rsp, 32
  ...
$ ./ccc --dump-ast hello.c
(funcdef main
  (block
    (return
      (binary +
        (num 2 int)
        (binary *
          (num 3 int)
          (num 4 int)))))
$ ./ccc bad.c -o bad
bad.c:2:10: error: undeclared identifier 'x'
    return x + 1;
           ^
```

## What this program becomes

`int main() { return 2 + 3 * 4; }` compiles to (constant-folded to 14):

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

With locals, each variable gets a stack slot (`add` example):

```c
int add(int a, int b) {
  int c = a + b;
  return c;
}
```

```asm
add:
  push rbp
  mov rbp, rsp
  sub rsp, 32
  mov DWORD PTR [rbp - 12], edi      # spill param a
  mov DWORD PTR [rbp - 16], esi      # spill param b
  lea rax, [rbp - 12]
  movsxd rax, DWORD PTR [rax]
  push rax
  lea rax, [rbp - 16]
  movsxd rax, DWORD PTR [rax]
  mov rcx, rax
  pop rax
  add rax, rcx
  mov rcx, rax
  lea rdi, [rbp - 20]                # &c
  mov DWORD PTR [rdi], ecx
  lea rax, [rbp - 20]
  movsxd rax, DWORD PTR [rax]
  jmp .Ladd_end
.Ladd_end:
  mov rsp, rbp
  pop rbp
  ret
```

## Supported language

| Area | Status |
| ---- | ------ |
| `int` literals, `return`, arithmetic, parens, unary `- ~ !`, comparisons, logical ops with short-circuiting | yes |
| Local variables, assignment, compound assignment, `++`/`--`, comma, ternary | yes |
| `if`/`else`, `while`, `do`-`while`, `for`, `break`, `continue`, blocks with scoping | yes |
| Functions: definitions, calls (6 register + stack args), recursion, forward declarations, libc calls, function pointers | yes |
| Pointers: `&`, `*`, arithmetic, differences, pointer-to-pointer | yes |
| Arrays incl. multidimensional, decay, `sizeof` | yes |
| `char`, `short`, `int`, `long`, `unsigned` variants, promotions, conversions, casts | yes |
| String literals, char literals, escapes, adjacent concatenation | yes |
| Globals with initializers, tentative definitions, `static` locals, `extern` | yes |
| Structs/unions: declaration, `.`/`->`, nesting, pointers, assignment, init lists | yes (`struct` by value as argument/return: pointers only) |
| Enums, `typedef`, `switch`/`case`/`default`, `goto`/labels | yes |
| Preprocessor: local `#include`, object-like `#define`, `#ifdef`/`#ifndef`/`#if`/`#else`/`#endif`, `#undef`, `#error` | yes |
| Optimizations: AST constant folding, dead code after `return`/`break`/`continue`/`goto`, `if (0)` simplification | yes |

## Tests

`make test` compiles and runs every program in `tests/` with both `ccc`
and `gcc`, compares exit codes/stdout, and checks that both agree. Error
cases must produce a diagnostic and a non-zero exit without crashing.

Current results (Fedora 44, gcc 16, x86-64):

```
tests: 201 passed, 0 failed, 201 total (177 valid programs)
```

177 valid programs plus 24 invalid programs. The 150-program minimum is
enforced by the runner itself.

Sanitizers: the suite was also run under AddressSanitizer, UBSan and
LeakSanitizer with zero findings on success paths (the stock Fedora
`libasan` symlinks are dangling, so the documented workaround is to
download the `libasan`/`libubsan` RPMs and link with `-L`/`-rpath`; see
`docs/ARCHITECTURE.md`). Error exits intentionally skip leak cleanup,
matching what production compilers do on fatal errors.

## Limitations (honest list)

- No floating point (`float`/`double` are rejected with a diagnostic).
- No `struct`/`union` passed or returned by value (use pointers); no
  `_Complex`, `_Atomic` semantics, bitfields, `volatile` semantics,
  `restrict` checking, or variable-length arrays.
- No K&R function definitions, no nested functions, no function
  declarators returning function pointers (`int (*f())()`), no compound
  literals, no designated initializers, no `goto` across initializations
  checking beyond label existence.
- Preprocessor is intentionally small: object-like macros only (no
  function-like macros, no `#elif` arithmetic beyond integers and
  `defined()`), no predefined architecture macros, no system headers.
  Glibc headers are therefore out of reach by design.
- Initializer lists for locals support one nesting level for arrays of
  scalars and struct members (globals support full nesting).
- `va_list` on the callee side is unsupported (calling variadic libc
  functions such as `printf` works; defining them does not).
- Optimizations are basic: folding plus dead-code removal only. No
  register allocation (stack machine), no inlining, no loop opts.

## Self-hosting

Stretch goal, attempted honestly: with small stub headers standing in for
the C library, `ccc -c` compiles 3 of its own 11 translation units
(`arena.c`, `types.c`, `opt.c`) into valid ELF objects with correct symbol
tables. The remaining 8 stop at exactly the documented limitations above:
`struct` passed/returned by value (`Loc`, `Token`, `TokVec` in signatures),
`va_list` (`error.c`, `codegen.c`), and glibc system headers. No silent
miscompiles were observed; every failure is a clean diagnostic.

## Project structure

```
ccc                  the compiler binary
src/                 compiler sources (C11, libc only)
  common.h           arena, locations, diagnostics interface
  arena.c            bump allocator (frees everything at once)
  error.c            file:line:col diagnostics with snippet + caret
  lexer.h/.c         tokenizer, --dump-tokens
  types.h/.c         type representation, sizes, promotions
  ast.h/.c           AST nodes, --dump-ast printer
  parser.h/.c        recursive-descent + precedence-climbing parser
  sema.h/.c          name resolution and type checking
  opt.h/.c           constant folding, dead-code removal
  codegen.h/.c       x86-64 Intel-syntax emitter (System V ABI)
  preprocess.h/.c    minimal preprocessor (-E)
  main.c             driver: flags, pipeline, gcc invocation
tests/               177 valid + 24 invalid programs, run.sh, inc/
docs/                ARCHITECTURE.md, LEARNING.md
Makefile             single file: build, test, clean
```

Arena reasoning in one line: tokens, nodes and types all die together at
the end of a compilation, so a bump allocator is faster than `malloc` and
cannot leak per-object.

## License

MIT, see LICENSE.
