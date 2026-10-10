# Contributing

## Build

Requires `gcc`, `make` and `binutils` (any recent Linux x86-64):

```
make
```

The build must stay warning-free under `-Wall -Wextra -Werror`. The only
dependency beyond libc is the system toolchain used for assembling and
linking test programs.

## Run tests

```
make test
```

This compiles every program in `tests/` with both `ccc` and `gcc`, runs
both, and checks exit codes/stdout agree. Keep it green before pushing.

## Add a test program

- Valid programs: `tests/test-NNN-short-name.c` (next free number), with a
  first-line marker `// exit: N` for the expected exit code. For stdout
  checks, add `tests/test-NNN-name.expected` with the exact expected bytes
  instead (exit code defaults to 0 and is still compared).
- Invalid programs: `tests/err-NNN-name.c` with `// ERROR: <substring>`
  that must appear in the diagnostic; the compiler must exit non-zero
  without crashing.
- Every test must also compile and agree under plain `gcc` — the runner
  verifies this, so double-check any expectation the runner flags.

## Commit style

Conventional commits, one logical change each: `feat:`, `fix:`, `test:`,
`docs:`. No giant commits; docs and code for one change go together.
