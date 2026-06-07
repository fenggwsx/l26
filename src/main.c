/* main.c - native CLI shell for the L26 compiler (l26c).
 *
 * Wiring is COMPLETE: it runs the full pipeline lexer -> parser -> semantic ->
 * codegen -> vm and renders diagnostics. The core modules it calls are
 * currently stubs; as they are filled in, this shell needs no changes.
 *
 * Usage:
 *   l26c [options] <source.l26>
 *   l26c [options] -            (read source from stdin)
 * Options:
 *   -a, --ast        print the AST and stop
 *   -S, --asm        print disassembled P-Code and stop
 *   -r, --run        compile and run (default)
 *   -h, --help       show help
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "codegen.h"
#include "vm.h"

typedef enum { MODE_RUN, MODE_AST, MODE_ASM } Mode;

static void usage(const char *prog) {
    fprintf(stderr,
        "L26 compiler (l26c)\n"
        "usage: %s [options] <source.l26 | ->\n"
        "  -a, --ast   print AST and stop\n"
        "  -S, --asm   print P-Code and stop\n"
        "  -r, --run   compile and run (default)\n"
        "  -h, --help  this help\n", prog);
}

/* Read an entire file (or stdin if path is "-") into a malloc'd NUL-terminated
 * buffer. Returns NULL on error. */
static char *read_all(const char *path) {
    FILE *f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
    if (!f) { fprintf(stderr, "l26c: cannot open '%s'\n", path); return NULL; }

    size_t cap = 1 << 16, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { if (f != stdin) fclose(f); return NULL; }

    size_t n;
    char tmp[4096];
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (len + n + 1 > cap) {
            while (len + n + 1 > cap) cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); if (f != stdin) fclose(f); return NULL; }
            buf = nb;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    buf[len] = '\0';
    if (f != stdin) fclose(f);
    return buf;
}

static void print_diags(const DiagList *dl) {
    char line[512];
    for (int i = 0; i < dl->count; ++i) {
        diag_format(&dl->items[i], line, sizeof(line));
        fprintf(stderr, "%s\n", line);
    }
    if (dl->overflow)
        fprintf(stderr, "note: additional diagnostics suppressed\n");
}

int main(int argc, char **argv) {
    Mode mode = MODE_RUN;
    const char *path = NULL;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) { usage(argv[0]); return 0; }
        else if (!strcmp(arg, "-a") || !strcmp(arg, "--ast")) mode = MODE_AST;
        else if (!strcmp(arg, "-S") || !strcmp(arg, "--asm")) mode = MODE_ASM;
        else if (!strcmp(arg, "-r") || !strcmp(arg, "--run")) mode = MODE_RUN;
        else if (arg[0] == '-' && arg[1] != '\0' && strcmp(arg, "-")) {
            fprintf(stderr, "l26c: unknown option '%s'\n", arg);
            usage(argv[0]); return 2;
        } else path = arg;
    }
    if (!path) { usage(argv[0]); return 2; }

    char *src = read_all(path);
    if (!src) return 1;

    DiagList dl;
    diag_init(&dl);

    /* ---- pipeline ---- */
    static TokenStream ts;   /* large; keep off the stack */
    lexer_scan(src, &ts, &dl);

    Node *root = parse(&ts, &dl);
    if (diag_has_errors(&dl) || !root) {
        print_diags(&dl);
        ast_free(root);
        free(src);
        return 1;
    }

    SymTab st;
    symtab_init(&st);
    semantic_check(root, &st, &dl);
    if (diag_has_errors(&dl)) {
        print_diags(&dl);
        ast_free(root);
        free(src);
        return 1;
    }

    if (mode == MODE_AST) {
        ast_print(root, stdout);
        print_diags(&dl);
        ast_free(root);
        free(src);
        return 0;
    }

    static Program prog;
    codegen_run(root, &st, &prog, &dl);
    if (diag_has_errors(&dl)) {
        print_diags(&dl);
        ast_free(root);
        free(src);
        return 1;
    }

    if (mode == MODE_ASM) {
        program_disassemble(&prog, stdout);
        print_diags(&dl);
        ast_free(root);
        free(src);
        return 0;
    }

    int rc = vm_run(&prog, NULL, &dl);
    print_diags(&dl);

    ast_free(root);
    free(src);
    return rc;
}
