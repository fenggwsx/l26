/* symtab.h - scoped symbol table with single-activation-record offsets.
 *
 * SCOPING MODEL (locked, see AGENTS.md decision #3):
 * The whole program is ONE procedure / ONE activation record. Every
 * declaration - even a shadowing one - gets its OWN unique storage offset.
 * Name resolution picks the innermost *currently open* declaration at COMPILE
 * time. After a block closes, its symbols are hidden from further lookups but
 * KEEP their offsets (the VM never reclaims them). The grand total of all
 * declared sizes is the single INT allocation at program start.
 *
 * Usage by the parser/semantic pass:
 *   symtab_init(st);
 *   symtab_enter_scope(st);              // on '{'
 *     idx = symtab_declare(st, name, type, pos, dl);   // on a decl
 *     idx = symtab_resolve(st, name);    // on a name use (-1 if undeclared)
 *   symtab_leave_scope(st);              // on '}'
 *   total = symtab_frame_size(st);       // operand to INT 0 A
 *
 * Pure C99, no I/O (errors go to a DiagList).
 */
#ifndef L26_SYMTAB_H
#define L26_SYMTAB_H

#include "common.h"
#include "diag.h"

typedef struct {
    char      name[L26_MAX_IDENT];
    ValueType type;
    int       offset;     /* unique cell offset in the single frame      */
    int       size;       /* cells: 1 scalar / L26_SET_CELLS set         */
    int       scope_depth;/* depth at which it was declared              */
    int       active;     /* 1 while its scope is open, 0 after it closes*/
    SrcPos    pos;        /* declaration site                            */
} Symbol;

typedef struct {
    Symbol syms[L26_MAX_SYMBOLS];
    int    count;            /* total symbols ever declared              */
    int    next_offset;      /* next free cell offset (monotonic)        */
    int    depth;            /* current open-scope depth (0 = none)      */
    int    scope_start[L26_MAX_SCOPES]; /* syms count when each scope opened */
} SymTab;

/* Reset to empty. Safe on a fresh stack object. */
void symtab_init(SymTab *st);

/* Open a new lexical scope. */
void symtab_enter_scope(SymTab *st);

/* Close the current scope: marks its symbols inactive (offsets retained).
 * Does NOT reclaim offsets. */
void symtab_leave_scope(SymTab *st);

/* Declare `name` of `type` in the current scope. Assigns a fresh unique
 * offset and size. Returns the new symbol index (>=0). On REDECLARATION
 * in the SAME scope, emits an error to `dl` and returns the existing index
 * without allocating a second slot. Returns -1 only on table overflow
 * (also emits a diagnostic). */
int symtab_declare(SymTab *st, const char *name, ValueType type,
                   SrcPos pos, DiagList *dl);

/* Resolve `name` to the innermost ACTIVE declaration. Returns the symbol
 * index, or -1 if not visible. No diagnostics (caller decides). */
int symtab_resolve(SymTab *st, const char *name);

/* Read-only access to a symbol by index (NULL if out of range). */
const Symbol *symtab_get(const SymTab *st, int index);

/* Total frame size in cells = sum of every declared symbol's size.
 * This is the operand A for `INT 0 A` emitted at program entry. */
int symtab_frame_size(const SymTab *st);

#endif /* L26_SYMTAB_H */
