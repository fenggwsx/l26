/* diag.h - diagnostic collection, free of I/O side effects.
 *
 * The front-end analysis core (lexer/parser/semantic) reports problems by
 * appending Diagnostic records to a DiagList.  Nothing here writes to a
 * stream; a SHELL (CLI, LSP, wasm) decides how to render them.  This is the
 * "one diagnostic library, many shells" decision from AGENTS.md.
 */
#ifndef L26_DIAG_H
#define L26_DIAG_H

#include "common.h"

typedef enum {
    DIAG_ERROR = 0,
    DIAG_WARNING,
    DIAG_NOTE
} DiagSeverity;

/* Which analysis phase produced the diagnostic (for nicer messages). */
typedef enum {
    DIAG_PHASE_LEX = 0,
    DIAG_PHASE_PARSE,
    DIAG_PHASE_SEMANTIC,
    DIAG_PHASE_CODEGEN,
    DIAG_PHASE_VM
} DiagPhase;

typedef struct {
    DiagSeverity severity;
    DiagPhase    phase;
    SrcPos       pos;
    char         message[256];   /* owned inline; no heap, no aliasing */
} Diagnostic;

typedef struct {
    Diagnostic items[L26_MAX_DIAGS];
    int        count;
    int        overflow;   /* set if more than L26_MAX_DIAGS were reported */
} DiagList;

/* Reset a diagnostic list to empty.  Safe to call on a fresh stack object. */
void diag_init(DiagList *dl);

/* Append a diagnostic.  `fmt` is printf-style.  Truncates safely to the
 * inline message buffer; never allocates.  Ignored (sets overflow) once the
 * list is full so the front end can keep going without crashing. */
void diag_add(DiagList *dl, DiagSeverity sev, DiagPhase phase, SrcPos pos,
              const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

/* Convenience: true if any DIAG_ERROR is present. */
int diag_has_errors(const DiagList *dl);

/* Number of diagnostics of a given severity. */
int diag_count(const DiagList *dl, DiagSeverity sev);

/* Format a single diagnostic into `buf` (e.g.
 * "error: 3:5: unexpected token ';'").  Returns the number of bytes that
 * would have been written (like snprintf).  No I/O. */
int diag_format(const Diagnostic *d, char *buf, size_t bufsz);

/* Severity / phase display names. Never NULL. */
const char *diag_severity_name(DiagSeverity s);
const char *diag_phase_name(DiagPhase p);

#endif /* L26_DIAG_H */
