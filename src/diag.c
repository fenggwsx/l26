/* diag.c - diagnostic collection & formatting, no I/O side effects. */
#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void diag_init(DiagList *dl) {
    dl->count = 0;
    dl->overflow = 0;
}

void diag_add(DiagList *dl, DiagSeverity sev, DiagPhase phase, SrcPos pos,
              const char *fmt, ...) {
    if (dl->count >= L26_MAX_DIAGS) {
        dl->overflow = 1;
        return;
    }
    Diagnostic *d = &dl->items[dl->count++];
    d->severity = sev;
    d->phase = phase;
    d->pos = pos;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->message, sizeof(d->message), fmt, ap);
    va_end(ap);
}

int diag_has_errors(const DiagList *dl) {
    for (int i = 0; i < dl->count; ++i)
        if (dl->items[i].severity == DIAG_ERROR)
            return 1;
    return 0;
}

int diag_count(const DiagList *dl, DiagSeverity sev) {
    int n = 0;
    for (int i = 0; i < dl->count; ++i)
        if (dl->items[i].severity == sev)
            ++n;
    return n;
}

const char *diag_severity_name(DiagSeverity s) {
    switch (s) {
        case DIAG_ERROR:   return "error";
        case DIAG_WARNING: return "warning";
        case DIAG_NOTE:    return "note";
        default:           return "diag";
    }
}

const char *diag_phase_name(DiagPhase p) {
    switch (p) {
        case DIAG_PHASE_LEX:      return "lex";
        case DIAG_PHASE_PARSE:    return "parse";
        case DIAG_PHASE_SEMANTIC: return "semantic";
        case DIAG_PHASE_CODEGEN:  return "codegen";
        case DIAG_PHASE_VM:       return "vm";
        default:                  return "?";
    }
}

int diag_format(const Diagnostic *d, char *buf, size_t bufsz) {
    /* e.g. "error: 3:5: [semantic] message" */
    return snprintf(buf, bufsz, "%s: %d:%d: [%s] %s",
                    diag_severity_name(d->severity),
                    d->pos.line, d->pos.col,
                    diag_phase_name(d->phase),
                    d->message);
}
