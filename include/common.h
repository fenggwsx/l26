/* common.h - shared types, limits, and value-type enums for the L26 compiler.
 *
 * This header is the bedrock of the frozen contract. Every other module
 * includes it (directly or transitively). It contains NO function prototypes
 * that allocate or do I/O - only plain types, enums and compile-time limits.
 *
 * Pure C99, no side effects.
 */
#ifndef L26_COMMON_H
#define L26_COMMON_H

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Compile-time limits                                                */
/* ------------------------------------------------------------------ */

/* Maximum number of distinct elements a `set` may hold. */
#define L26_MAX_SET 200

/* A set occupies this many cells INLINE in the single activation record:
 *   cell[base+0]            = element count (0..L26_MAX_SET)
 *   cell[base+1 .. base+200]= up to 200 element values, sorted ascending.
 * Hence 1 + 200 = 201 cells. int/bool occupy 1 cell. */
#define L26_SET_CELLS (1 + L26_MAX_SET)

/* int / bool storage size, in stack cells. */
#define L26_SCALAR_CELLS 1

/* Hard ceilings so the runtime-built LALR tables and fixed buffers are
 * statically sized.  Bump if a test program ever exceeds them. */
#define L26_MAX_SRC       (1 << 20)   /* 1 MiB source text                 */
#define L26_MAX_TOKENS    65536       /* tokens per program                */
#define L26_MAX_IDENT     64          /* identifier length (incl. NUL)     */
#define L26_MAX_CODE      65536       /* emitted P-Code instructions       */
#define L26_MAX_SYMBOLS   4096        /* symbol-table entries              */
#define L26_MAX_SCOPES    256         /* nested block depth tracking       */
#define L26_MAX_DIAGS     1024        /* collected diagnostics             */

/* ------------------------------------------------------------------ */
/* Value / declared types                                             */
/* ------------------------------------------------------------------ */

/* The L26 type of a declared variable or an expression's static type.
 * TYPE_ERROR is the propagating "poison" type used by the semantic
 * checker so one error does not cascade into many. */
typedef enum {
    TYPE_ERROR = 0,   /* unknown / type error (poison)                  */
    TYPE_INT,         /* int  - 1 cell                                  */
    TYPE_BOOL,        /* bool - 1 cell                                  */
    TYPE_SET,         /* set  - L26_SET_CELLS cells                     */
    TYPE_VOID         /* statements / no value                          */
} ValueType;

/* Human-readable name for a ValueType (e.g. "int"). Never NULL. */
const char *value_type_name(ValueType t);

/* Number of activation-record cells a declared type occupies. */
static inline int value_type_size(ValueType t) {
    return (t == TYPE_SET) ? L26_SET_CELLS : L26_SCALAR_CELLS;
}

/* ------------------------------------------------------------------ */
/* Source positions                                                   */
/* ------------------------------------------------------------------ */

/* 1-based line/column into the original source, plus a byte offset.
 * Carried by tokens, AST nodes and diagnostics so any shell (CLI or LSP)
 * can map back to source coordinates. */
typedef struct {
    int line;     /* 1-based */
    int col;      /* 1-based */
    int offset;   /* 0-based byte offset into source */
} SrcPos;

static inline SrcPos srcpos_make(int line, int col, int offset) {
    SrcPos p; p.line = line; p.col = col; p.offset = offset; return p;
}

#endif /* L26_COMMON_H */
