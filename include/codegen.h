/* codegen.h - P-Code emission from an annotated AST.
 *
 * Consumes a (semantically valid) AST plus the SymTab that gave every
 * declaration its frame offset, and fills a Program with P-Code.  The very
 * first instruction it emits is `INT 0 <frame_size>` and the last is
 * `OPR 0 0` (return/halt).  Set variables are addressed by their base
 * offset; set instructions (SADD/SUNION/...) take that base offset in `a`.
 *
 * Pure C99, no I/O.  Errors (should be rare after semantic analysis) go to dl.
 */
#ifndef L26_CODEGEN_H
#define L26_CODEGEN_H

#include "ast.h"
#include "symtab.h"
#include "vm.h"
#include "diag.h"

/* Generate code for `root` (the program's top N_BLOCK) using the offsets in
 * `st`. Writes into `out` (caller-zeroed or fresh). Returns 0 on success,
 * non-zero if a diagnostic error was emitted. */
int codegen_run(const Node *root, const SymTab *st, Program *out, DiagList *dl);

#endif /* L26_CODEGEN_H */
