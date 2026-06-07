/* symtab.c - scoped symbol table with single-activation-record offsets.
 *
 * Fully implemented. Each declaration gets a UNIQUE monotonic offset; closing
 * a scope only deactivates its symbols (offsets are never reclaimed), so the
 * VM needs no runtime descope. Resolution scans newest-to-oldest for the
 * innermost ACTIVE symbol, giving correct shadowing.
 */
#include "symtab.h"

#include <string.h>

void symtab_init(SymTab *st) {
    st->count = 0;
    st->next_offset = 0;
    st->depth = 0;
}

void symtab_enter_scope(SymTab *st) {
    if (st->depth < L26_MAX_SCOPES)
        st->scope_start[st->depth] = st->count;
    st->depth++;
}

void symtab_leave_scope(SymTab *st) {
    if (st->depth <= 0) return;
    st->depth--;
    int start = (st->depth < L26_MAX_SCOPES) ? st->scope_start[st->depth] : st->count;
    /* Deactivate everything declared at this depth (offsets kept). */
    for (int i = start; i < st->count; ++i)
        if (st->syms[i].scope_depth == st->depth + 1)
            st->syms[i].active = 0;
}

int symtab_declare(SymTab *st, const char *name, ValueType type,
                   SrcPos pos, DiagList *dl) {
    /* Redeclaration check: same name, still active, same depth. */
    for (int i = st->count - 1; i >= 0; --i) {
        if (st->syms[i].active &&
            st->syms[i].scope_depth == st->depth &&
            strncmp(st->syms[i].name, name, L26_MAX_IDENT) == 0) {
            diag_add(dl, DIAG_ERROR, DIAG_PHASE_SEMANTIC, pos,
                     "redeclaration of '%s' in the same scope", name);
            return i;
        }
    }
    if (st->count >= L26_MAX_SYMBOLS) {
        diag_add(dl, DIAG_ERROR, DIAG_PHASE_SEMANTIC, pos,
                 "too many declarations (max %d)", L26_MAX_SYMBOLS);
        return -1;
    }
    int size = value_type_size(type);
    if (st->next_offset + size < st->next_offset) { /* overflow guard */
        diag_add(dl, DIAG_ERROR, DIAG_PHASE_SEMANTIC, pos,
                 "frame size overflow");
        return -1;
    }
    Symbol *s = &st->syms[st->count];
    strncpy(s->name, name, L26_MAX_IDENT - 1);
    s->name[L26_MAX_IDENT - 1] = '\0';
    s->type = type;
    s->offset = st->next_offset;
    s->size = size;
    s->scope_depth = st->depth;
    s->active = 1;
    s->pos = pos;
    st->next_offset += size;
    return st->count++;
}

int symtab_resolve(SymTab *st, const char *name) {
    for (int i = st->count - 1; i >= 0; --i) {
        if (st->syms[i].active &&
            strncmp(st->syms[i].name, name, L26_MAX_IDENT) == 0)
            return i;
    }
    return -1;
}

const Symbol *symtab_get(const SymTab *st, int index) {
    if (index < 0 || index >= st->count) return NULL;
    return &st->syms[index];
}

int symtab_frame_size(const SymTab *st) {
    return st->next_offset;
}
