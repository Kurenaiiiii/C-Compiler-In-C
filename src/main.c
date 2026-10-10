/* main.c - driver: preprocess -> lex -> parse -> sema -> opt -> codegen.
 *
 * Usage:
 *   ccc input.c -o output        compile + assemble + link to executable
 *   ccc -S input.c [-o out.s]    emit assembly (stdout if no -o)
 *   ccc --dump-tokens input.c    print tokens
 *   ccc --dump-ast input.c       print AST (after sema, before opt)
 *   ccc -E input.c               print preprocessed source
 * Only the assemble/link step invokes the system gcc; user code is never
 * compiled by gcc.
 */
#define _GNU_SOURCE
#include "ast.h"
#include "codegen.h"
#include "common.h"
#include "lexer.h"
#include "opt.h"
#include "parser.h"
#include "preprocess.h"
#include "sema.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *fp) {
    fprintf(fp,
            "usage: ccc [options] file.c [-o out]\n"
            "  -o FILE            output file (executable, or .s with -S)\n"
            "  -S                 emit assembly instead of an executable\n"
            "  -c                 emit assembly object (.o) instead of executable\n"
            "  -E                 print preprocessed source\n"
            "  --dump-tokens      print token list\n"
            "  --dump-ast         print AST\n"
            "  -I DIR             add include search directory\n"
            "  -D NAME[=VALUE]    predefined macro\n"
            "  -O0                disable AST optimizations (default: on)\n"
            "  -o -               write to stdout (with -S/-E/dumps)\n"
            "  -h, --help         this help\n"
            "  -V, --version      version\n");
}

static const char *VERSION = "0.1.0";

static char *with_ext(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    size_t base = strlen(path);
    if (dot && (!slash || dot > slash))
        base = (size_t)(dot - path);
    char *r = (char *)xmalloc(base + strlen(ext) + 1);
    memcpy(r, path, base);
    strcpy(r + base, ext);
    return r;
}

int main(int argc, char **argv) {
    const char *input = NULL;
    const char *output = NULL;
    int emit_asm = 0;
    int emit_obj = 0;
    int dump_tokens = 0;
    int dump_ast_flag = 0;
    int run_pp_only = 0;
    int opt_on = 1;
    PPOptions ppopt;
    memset(&ppopt, 0, sizeof ppopt);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "ccc: -o needs an argument\n");
                return 1;
            }
            output = argv[i];
        } else if (strcmp(argv[i], "-S") == 0) {
            emit_asm = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            emit_obj = 1;
        } else if (strcmp(argv[i], "-E") == 0) {
            run_pp_only = 1;
        } else if (strcmp(argv[i], "--dump-tokens") == 0) {
            dump_tokens = 1;
        } else if (strcmp(argv[i], "--dump-ast") == 0) {
            dump_ast_flag = 1;
        } else if (strcmp(argv[i], "-O0") == 0) {
            opt_on = 0;
        } else if (strcmp(argv[i], "-O1") == 0 || strcmp(argv[i], "-O2") == 0 ||
                   strcmp(argv[i], "-O") == 0) {
            opt_on = 1;
        } else if (strcmp(argv[i], "-I") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "ccc: -I needs an argument\n");
                return 1;
            }
            ppopt.inc_dirs = (char **)realloc(
                ppopt.inc_dirs, (size_t)(ppopt.n_inc + 1) * sizeof(char *));
            if (!ppopt.inc_dirs)
                die("out of memory");
            ppopt.inc_dirs[ppopt.n_inc++] = argv[i];
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            ppopt.inc_dirs = (char **)realloc(
                ppopt.inc_dirs, (size_t)(ppopt.n_inc + 1) * sizeof(char *));
            if (!ppopt.inc_dirs)
                die("out of memory");
            ppopt.inc_dirs[ppopt.n_inc++] = argv[i] + 2;
        } else if (strncmp(argv[i], "-D", 2) == 0) {
            ppopt.defs = (char **)realloc(
                ppopt.defs, (size_t)(ppopt.n_defs + 1) * sizeof(char *));
            if (!ppopt.defs)
                die("out of memory");
            ppopt.defs[ppopt.n_defs++] = argv[i] + 2;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 ||
                   strcmp(argv[i], "--version") == 0) {
            printf("ccc %s\n", VERSION);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "ccc: unknown option '%s'\n", argv[i]);
            usage(stderr);
            return 1;
        } else {
            if (input) {
                fprintf(stderr, "ccc: only one input file supported\n");
                return 1;
            }
            input = argv[i];
        }
    }
    if (!input) {
        usage(stderr);
        return 1;
    }

    static Arena arena;
    arena_init(&arena);
    g_arena = &arena;
    types_init(&arena);

    char *srctext = read_file(input);
    if (!srctext) {
        fprintf(stderr, "ccc: cannot open '%s'\n", input);
        return 1;
    }
    char *ppsrc = preprocess(&arena, input, srctext, &ppopt);
    free(srctext);

    if (run_pp_only) {
        if (output && strcmp(output, "-") != 0) {
            FILE *f = fopen(output, "w");
            if (!f) {
                fprintf(stderr, "ccc: cannot write '%s'\n", output);
                return 1;
            }
            fputs(ppsrc, f);
            fclose(f);
        } else {
            fputs(ppsrc, stdout);
        }
        return 0;
    }

    error_set_source(input, ppsrc);
    TokVec tv = lex(&arena, input, ppsrc);

    if (dump_tokens) {
        FILE *fp = stdout;
        if (output && strcmp(output, "-") != 0) {
            fp = fopen(output, "w");
            if (!fp) {
                fprintf(stderr, "ccc: cannot write '%s'\n", output);
                return 1;
            }
        }
        for (size_t i = 0; i < tv.len; i++) {
            Token *t = &tv.toks[i];
            if (t->kind == TK_EOF) {
                fprintf(fp, "%d:%d: eof\n", t->loc.line, t->loc.col);
                break;
            }
            if (t->kind == TK_IDENT)
                fprintf(fp, "%d:%d: ident '%s'\n", t->loc.line, t->loc.col,
                        t->sval);
            else if (t->kind == TK_NUM)
                fprintf(fp, "%d:%d: num %lld%s%s\n", t->loc.line, t->loc.col,
                        t->ival, t->is_unsigned ? "U" : "",
                        t->is_long == 2 ? "LL" : t->is_long ? "L" : "");
            else if (t->kind == TK_CHARLIT)
                fprintf(fp, "%d:%d: char %lld\n", t->loc.line, t->loc.col,
                        t->ival);
            else if (t->kind == TK_STR)
                fprintf(fp, "%d:%d: str len=%zu\n", t->loc.line, t->loc.col,
                        t->slen);
            else
                fprintf(fp, "%d:%d: '%s'\n", t->loc.line, t->loc.col,
                        tokkind_name(t->kind));
        }
        if (fp != stdout)
            fclose(fp);
        return 0;
    }

    parse_program(&tv);
    sema_analyze();
    if (dump_ast_flag) {
        dump_ast(stdout);
        return 0;
    }
    if (opt_on)
        opt_run();

    /* Emit assembly to memory-backed temp, then either print or assemble. */
    char *asm_path = NULL;
    int need_tmp = !(emit_asm || emit_obj);
    FILE *af = NULL;
    if (emit_asm || emit_obj) {
        if (output && strcmp(output, "-") != 0) {
            af = fopen(output, "w");
            if (!af) {
                fprintf(stderr, "ccc: cannot write '%s'\n", output);
                return 1;
            }
            codegen(af);
            fclose(af);
        } else if (output && strcmp(output, "-") == 0) {
            codegen(stdout);
        } else if (emit_asm) {
            asm_path = with_ext(input, ".s");
            af = fopen(asm_path, "w");
            if (!af) {
                fprintf(stderr, "ccc: cannot write '%s'\n", asm_path);
                return 1;
            }
            codegen(af);
            fclose(af);
            fprintf(stderr, "ccc: wrote %s\n", asm_path);
            free(asm_path);
        } else {
            asm_path = with_ext(input, ".o.s");
            af = fopen(asm_path, "w");
            if (!af) {
                fprintf(stderr, "ccc: cannot write '%s'\n", asm_path);
                return 1;
            }
            codegen(af);
            fclose(af);
            /* assemble only */
            const char *obj = output ? output : with_ext(input, ".o");
            char cmd[4096];
            snprintf(cmd, sizeof cmd, "gcc -c -o '%s' '%s'", obj, asm_path);
            int rc = system(cmd);
            unlink(asm_path);
            free(asm_path);
            if (rc != 0) {
                fprintf(stderr, "ccc: assembler failed\n");
                return 1;
            }
        }
        return 0;
    } else {
        /* Full build: assembly -> temp file -> gcc link. */
        char tmpl[] = "/tmp/cccXXXXXX.s";
        int fd = mkstemps(tmpl, 2);
        if (fd < 0) {
            fprintf(stderr, "ccc: cannot create temp file\n");
            return 1;
        }
        af = fdopen(fd, "w");
        if (!af) {
            fprintf(stderr, "ccc: cannot write temp file\n");
            close(fd);
            return 1;
        }
        codegen(af);
        fclose(af);
        const char *out = output ? output : "a.out";
        char cmd[4096];
        snprintf(cmd, sizeof cmd, "gcc -o '%s' '%s'", out, tmpl);
        int rc = system(cmd);
        unlink(tmpl);
        (void)need_tmp;
        if (rc != 0) {
            fprintf(stderr, "ccc: link failed\n");
            return 1;
        }
        return 0;
    }
}
