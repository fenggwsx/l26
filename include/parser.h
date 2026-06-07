/* parser.h - LALR(1) parse entry.
 *
 * The parser builds its ACTION/GOTO tables AT RUNTIME from a hardcoded
 * numbered grammar (see parser.c / DESIGN.md): canonical LR(1) item sets,
 * then merge equal-core states to LALR(1), then table-driven shift/reduce
 * with an explicit state+symbol stack.  ALL of that is INTERNAL to parser.c;
 * the only public surface is parse().
 *
 * Pure C99, no I/O.  The parser does not run the lexer itself - the caller
 * provides a scanned TokenStream so shells can reuse tokens (e.g. for syntax
 * highlighting) without re-lexing.
 */
#ifndef L26_PARSER_H
#define L26_PARSER_H

#include "ast.h"
#include "lexer.h"
#include "diag.h"

/* Parse `ts` into an AST. On success returns the root N_BLOCK node (owned by
 * the caller; free with ast_free). On syntax error returns NULL (or a partial
 * tree may be returned per recovery policy) and appends diagnostics to `dl`.
 * Callers should check diag_has_errors(dl) rather than only the return. */
Node *parse(const TokenStream *ts, DiagList *dl);

/* OPTIONAL diagnostic hook for developers: build the LALR tables and report
 * any grammar conflicts (shift/reduce, reduce/reduce) into `dl`. Returns the
 * number of conflicts found (0 == clean LALR(1) grammar). Not needed for
 * normal compilation - parse() builds tables lazily on first call - but
 * exposed so a self-test can assert the frozen grammar stays conflict-free. */
int parser_build_tables_report(DiagList *dl);

#endif /* L26_PARSER_H */
