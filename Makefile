# Makefile for ccc - C Compiler in C. Zero dependencies beyond libc.
CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -MMD -MP
SRCS = src/arena.c src/error.c src/lexer.c src/types.c src/ast.c \
       src/preprocess.c src/parser.c src/sema.c src/opt.c src/codegen.c \
       src/main.c
OBJS = $(SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)

all: ccc

ccc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEPS)

ccc-asan: $(SRCS)
	$(CC) -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
	    -fno-omit-frame-pointer -o $@ $(SRCS)

test: ccc
	tests/run.sh

test-asan: ccc-asan
	CCC=./ccc-asan ASAN=1 tests/run.sh

clean:
	rm -f $(OBJS) $(DEPS) ccc ccc-asan src/*.o src/*.d
	rm -f tests/tmp/* 2>/dev/null; true

.PHONY: all test test-asan clean
