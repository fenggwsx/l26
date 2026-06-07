/* semantic.h - type checking and symbol-table population.
 *
 * Walks the AST built by the parser, opens/closes scopes in lockstep with
 * N_BLOCK nodes, declares variables into the SymTab (assigning unique frame
 * offsets), resolves every name to a symbol index (writing it back into the
 * node, e.g. as.var.sym), and assigns each expression node a ValueType.
 * Type errors are reported to the DiagList; TYPE_ERROR propagates to suppress
 * cascades.
 *
 * After a successful run the AST is fully annotated and `st` holds the final
 * frame layout, ready for codegen.
 *
 * Pure C99, no I/O.
 */
#ifndef L26_SEMANTIC_H
#define L26_SEMANTIC_H

#include "ast.h"
#include "symtab.h"
#include "diag.h"

/* Type-check `root` (the program block). Populates `st`. Returns 0 if no
 * errors were added to `dl`, non-zero otherwise. `st` must be freshly
 * symtab_init()'d by the caller. */
int semantic_check(Node *root, SymTab *st, DiagList *dl);

#endif /* L26_SEMANTIC_H */
