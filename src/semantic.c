/* semantic.c - type checker & symbol-table population.
 *
 * Recursive AST walk that:
 *   - opens a scope on entering each N_BLOCK, closes it on exit;
 *   - declares each N_DECL into the SymTab (assigning a unique frame offset
 *     and size: 1 for int/bool, L26_SET_CELLS for set);
 *   - resolves every name use to a symbol index, written back into the node's
 *     `.sym`/`.lsym`/`.rsym`/`.src_sym`/`.compvar_sym` field; undeclared names
 *     are reported as errors;
 *   - computes a ValueType for every expression node (Node.vtype) and enforces
 *     the L26 type rules;
 *   - handles set comprehension by declaring its loop var (int) in a temporary
 *     inner scope visible only to the generator and filter.
 *
 * TYPE_ERROR is the propagating poison value: once an operand is poisoned its
 * containing expression is poisoned too, and NO new diagnostic is emitted for
 * the dependent expression (only the root cause is reported). Returns non-zero
 * iff any DIAG_ERROR was added to `dl`.
 *
 * Pure C99, no I/O.
 */
#include "semantic.h"

/* --- forward declarations --- */
static void check_block(Node *n, SymTab *st, DiagList *dl);
static void check_stmt(Node *n, SymTab *st, DiagList *dl);
static ValueType check_expr(Node *n, SymTab *st, DiagList *dl);

/* Convenience: emit a semantic error at a node's position. */
#define SEM_ERR(dl, pos, ...) \
    diag_add((dl), DIAG_ERROR, DIAG_PHASE_SEMANTIC, (pos), __VA_ARGS__)

/* Resolve a named variable, writing the index into *symout. On success
 * returns the symbol's declared type; on failure emits "undeclared" and
 * returns TYPE_ERROR (with *symout = -1). */
static ValueType resolve_var(SymTab *st, const char *name, int *symout,
                             SrcPos pos, DiagList *dl) {
    int idx = symtab_resolve(st, name);
    *symout = idx;
    if (idx < 0) {
        SEM_ERR(dl, pos, "use of undeclared identifier '%s'", name);
        return TYPE_ERROR;
    }
    const Symbol *s = symtab_get(st, idx);
    return s ? s->type : TYPE_ERROR;
}

/* ------------------------------------------------------------------ */
/* Expressions                                                        */
/* ------------------------------------------------------------------ */

/* Type-check an arithmetic binary operator: both operands must be int. */
static ValueType check_arith(Node *n, SymTab *st, DiagList *dl) {
    ValueType lt = check_expr(n->as.binop.lhs, st, dl);
    ValueType rt = check_expr(n->as.binop.rhs, st, dl);
    if (lt == TYPE_ERROR || rt == TYPE_ERROR) return TYPE_ERROR;
    if (lt != TYPE_INT || rt != TYPE_INT) {
        SEM_ERR(dl, n->pos,
                "arithmetic operator '%s' requires int operands (got %s and %s)",
                op_name(n->as.binop.op),
                value_type_name(lt), value_type_name(rt));
        return TYPE_ERROR;
    }
    return TYPE_INT;
}

/* Relational operator: both operands int, yields bool. */
static ValueType check_rel(Node *n, SymTab *st, DiagList *dl) {
    ValueType lt = check_expr(n->as.binop.lhs, st, dl);
    ValueType rt = check_expr(n->as.binop.rhs, st, dl);
    if (lt == TYPE_ERROR || rt == TYPE_ERROR) return TYPE_ERROR;
    if (lt != TYPE_INT || rt != TYPE_INT) {
        SEM_ERR(dl, n->pos,
                "relational operator '%s' requires int operands (got %s and %s)",
                op_name(n->as.binop.op),
                value_type_name(lt), value_type_name(rt));
        return TYPE_ERROR;
    }
    return TYPE_BOOL;
}

/* Logical && / || : both operands bool, yields bool. */
static ValueType check_logic(Node *n, SymTab *st, DiagList *dl) {
    ValueType lt = check_expr(n->as.binop.lhs, st, dl);
    ValueType rt = check_expr(n->as.binop.rhs, st, dl);
    if (lt == TYPE_ERROR || rt == TYPE_ERROR) return TYPE_ERROR;
    if (lt != TYPE_BOOL || rt != TYPE_BOOL) {
        SEM_ERR(dl, n->pos,
                "logical operator '%s' requires bool operands (got %s and %s)",
                op_name(n->as.binop.op),
                value_type_name(lt), value_type_name(rt));
        return TYPE_ERROR;
    }
    return TYPE_BOOL;
}

/* Set literal { a1, a2, ... }: each element must be int; yields set. */
static ValueType check_setlit(Node *n, SymTab *st, DiagList *dl) {
    int ok = 1;
    for (int i = 0; i < n->nkids; ++i) {
        ValueType et = check_expr(n->kids[i], st, dl);
        if (et == TYPE_ERROR) { ok = 0; continue; }
        if (et != TYPE_INT) {
            SEM_ERR(dl, n->kids[i]->pos,
                    "set element must be int (got %s)", value_type_name(et));
            ok = 0;
        }
    }
    return ok ? TYPE_SET : TYPE_ERROR;
}

/* ID union ID / ID inter ID: both names must resolve to sets; yields set. */
static ValueType check_setbin(Node *n, SymTab *st, DiagList *dl) {
    ValueType lt = resolve_var(st, n->as.setbin.lname, &n->as.setbin.lsym,
                               n->pos, dl);
    ValueType rt = resolve_var(st, n->as.setbin.rname, &n->as.setbin.rsym,
                               n->pos, dl);
    if (lt == TYPE_ERROR || rt == TYPE_ERROR) return TYPE_ERROR;
    const char *opn = (n->as.setbin.op == AST_SETBIN_UNION) ? "union" : "inter";
    if (lt != TYPE_SET || rt != TYPE_SET) {
        SEM_ERR(dl, n->pos,
                "'%s' requires set operands (got %s and %s)",
                opn, value_type_name(lt), value_type_name(rt));
        return TYPE_ERROR;
    }
    return TYPE_SET;
}

/* set == set / set != set (BONUS): both names must be sets; yields bool.
 * Encoded as N_SETEQ with as.binop where lhs/rhs are N_VAR nodes. */
static ValueType check_seteq(Node *n, SymTab *st, DiagList *dl) {
    ValueType lt = check_expr(n->as.binop.lhs, st, dl);
    ValueType rt = check_expr(n->as.binop.rhs, st, dl);
    if (lt == TYPE_ERROR || rt == TYPE_ERROR) return TYPE_ERROR;
    if (lt != TYPE_SET || rt != TYPE_SET) {
        SEM_ERR(dl, n->pos,
                "set %s requires set operands (got %s and %s)",
                (n->as.binop.op == OP_EQ) ? "==" : "!=",
                value_type_name(lt), value_type_name(rt));
        return TYPE_ERROR;
    }
    return TYPE_BOOL;
}

/* aexpr in ID: element must be int, ID must be a set; yields bool. */
static ValueType check_in(Node *n, SymTab *st, DiagList *dl) {
    ValueType et = check_expr(n->as.intest.elem, st, dl);
    ValueType st_t = resolve_var(st, n->as.intest.name, &n->as.intest.sym,
                                 n->pos, dl);
    if (et == TYPE_ERROR || st_t == TYPE_ERROR) return TYPE_ERROR;
    if (et != TYPE_INT) {
        SEM_ERR(dl, n->pos, "left operand of 'in' must be int (got %s)",
                value_type_name(et));
        return TYPE_ERROR;
    }
    if (st_t != TYPE_SET) {
        SEM_ERR(dl, n->pos, "right operand of 'in' must be a set (got %s)",
                value_type_name(st_t));
        return TYPE_ERROR;
    }
    return TYPE_BOOL;
}

/* isempty(ID): ID must be a set; yields bool. */
static ValueType check_isempty(Node *n, SymTab *st, DiagList *dl) {
    ValueType st_t = resolve_var(st, n->as.intest.name, &n->as.intest.sym,
                                 n->pos, dl);
    if (st_t == TYPE_ERROR) return TYPE_ERROR;
    if (st_t != TYPE_SET) {
        SEM_ERR(dl, n->pos, "isempty requires a set argument (got %s)",
                value_type_name(st_t));
        return TYPE_ERROR;
    }
    return TYPE_BOOL;
}

/* Set comprehension { gen | var in src if filter } (BONUS).
 *   - src must resolve to a set;
 *   - loop var is an int declared in a fresh inner scope visible only to
 *     gen and filter;
 *   - gen must be int (its values become the new set's elements);
 *   - filter (if present) must be bool.
 * Yields set. */
static ValueType check_comp(Node *n, SymTab *st, DiagList *dl) {
    /* Resolve the source set in the OUTER scope first. */
    ValueType src_t = resolve_var(st, n->as.comp.src, &n->as.comp.src_sym,
                                  n->pos, dl);
    int src_ok = 1;
    if (src_t == TYPE_ERROR) {
        src_ok = 0;
    } else if (src_t != TYPE_SET) {
        SEM_ERR(dl, n->pos,
                "comprehension source '%s' must be a set (got %s)",
                n->as.comp.src, value_type_name(src_t));
        src_ok = 0;
    }

    /* Declare the loop variable (int) in a temporary inner scope so it is
     * visible only to gen and filter. */
    symtab_enter_scope(st);
    n->as.comp.compvar_sym =
        symtab_declare(st, n->as.comp.var, TYPE_INT, n->pos, dl);

    int body_ok = 1;
    ValueType gt = check_expr(n->as.comp.gen, st, dl);
    if (gt == TYPE_ERROR) {
        body_ok = 0;
    } else if (gt != TYPE_INT) {
        SEM_ERR(dl, n->as.comp.gen->pos,
                "comprehension generator must be int (got %s)",
                value_type_name(gt));
        body_ok = 0;
    }
    if (n->as.comp.filter) {
        ValueType ft = check_expr(n->as.comp.filter, st, dl);
        if (ft == TYPE_ERROR) {
            body_ok = 0;
        } else if (ft != TYPE_BOOL) {
            SEM_ERR(dl, n->as.comp.filter->pos,
                    "comprehension filter must be bool (got %s)",
                    value_type_name(ft));
            body_ok = 0;
        }
    }

    symtab_leave_scope(st);
    return (src_ok && body_ok) ? TYPE_SET : TYPE_ERROR;
}

/* Dispatch over expression node kinds, annotating n->vtype. */
static ValueType check_expr(Node *n, SymTab *st, DiagList *dl) {
    ValueType t = TYPE_ERROR;
    if (!n) return TYPE_ERROR;

    switch (n->kind) {
    case N_NUM:
        t = TYPE_INT;
        break;
    case N_VAR:
        t = resolve_var(st, n->as.var.name, &n->as.var.sym, n->pos, dl);
        break;
    case N_BINOP:
        t = check_arith(n, st, dl);
        break;
    case N_NEG: {
        ValueType ot = check_expr(n->kids[0], st, dl);
        if (ot == TYPE_ERROR) t = TYPE_ERROR;
        else if (ot != TYPE_INT) {
            SEM_ERR(dl, n->pos, "unary '-' requires an int operand (got %s)",
                    value_type_name(ot));
            t = TYPE_ERROR;
        } else t = TYPE_INT;
        break;
    }
    case N_BOOLLIT:
        t = TYPE_BOOL;
        break;
    case N_NOT: {
        ValueType ot = check_expr(n->kids[0], st, dl);
        if (ot == TYPE_ERROR) t = TYPE_ERROR;
        else if (ot != TYPE_BOOL) {
            SEM_ERR(dl, n->pos, "'!' requires a bool operand (got %s)",
                    value_type_name(ot));
            t = TYPE_ERROR;
        } else t = TYPE_BOOL;
        break;
    }
    case N_LOGIC:
        t = check_logic(n, st, dl);
        break;
    case N_REL:
        t = check_rel(n, st, dl);
        break;
    case N_SETLIT:
        t = check_setlit(n, st, dl);
        break;
    case N_SETBINOP:
        t = check_setbin(n, st, dl);
        break;
    case N_SETEQ:
        t = check_seteq(n, st, dl);
        break;
    case N_SETCOMP:
        t = check_comp(n, st, dl);
        break;
    case N_IN:
        t = check_in(n, st, dl);
        break;
    case N_ISEMPTY:
        t = check_isempty(n, st, dl);
        break;
    default:
        /* A statement node where an expression was expected: poison. */
        t = TYPE_ERROR;
        break;
    }

    n->vtype = t;
    return t;
}

/* ------------------------------------------------------------------ */
/* Statements                                                         */
/* ------------------------------------------------------------------ */

static void check_stmt(Node *n, SymTab *st, DiagList *dl) {
    if (!n) return;
    switch (n->kind) {
    case N_BLOCK:
        check_block(n, st, dl);
        break;

    case N_ASSIGN: {
        ValueType lt = resolve_var(st, n->as.assign.name, &n->as.assign.sym,
                                   n->pos, dl);
        ValueType rt = check_expr(n->as.assign.rhs, st, dl);
        if (lt != TYPE_ERROR && rt != TYPE_ERROR && lt != rt) {
            SEM_ERR(dl, n->pos,
                    "cannot assign %s to '%s' of type %s",
                    value_type_name(rt), n->as.assign.name,
                    value_type_name(lt));
        }
        n->vtype = TYPE_VOID;
        break;
    }

    case N_IF: {
        ValueType ct = check_expr(n->as.if_.cond, st, dl);
        if (ct != TYPE_ERROR && ct != TYPE_BOOL)
            SEM_ERR(dl, n->as.if_.cond->pos,
                    "if condition must be bool (got %s)", value_type_name(ct));
        check_stmt(n->as.if_.then_s, st, dl);
        if (n->as.if_.else_s) check_stmt(n->as.if_.else_s, st, dl);
        n->vtype = TYPE_VOID;
        break;
    }

    case N_WHILE: {
        ValueType ct = check_expr(n->as.while_.cond, st, dl);
        if (ct != TYPE_ERROR && ct != TYPE_BOOL)
            SEM_ERR(dl, n->as.while_.cond->pos,
                    "while condition must be bool (got %s)",
                    value_type_name(ct));
        check_stmt(n->as.while_.body, st, dl);
        n->vtype = TYPE_VOID;
        break;
    }

    case N_WRITE:
        /* write accepts any (well-typed) value: int, bool or set. */
        check_expr(n->kids[0], st, dl);
        n->vtype = TYPE_VOID;
        break;

    case N_READ:
        /* read accepts any variable: int, bool or set. */
        resolve_var(st, n->as.read.name, &n->as.read.sym, n->pos, dl);
        n->vtype = TYPE_VOID;
        break;

    case N_ADD:
    case N_REMOVE: {
        ValueType vt = resolve_var(st, n->as.setop.name, &n->as.setop.sym,
                                   n->pos, dl);
        ValueType et = check_expr(n->as.setop.elem, st, dl);
        const char *kw = (n->kind == N_ADD) ? "add" : "remove";
        if (vt != TYPE_ERROR && vt != TYPE_SET)
            SEM_ERR(dl, n->pos, "'%s' requires a set variable (got %s)",
                    kw, value_type_name(vt));
        if (et != TYPE_ERROR && et != TYPE_INT)
            SEM_ERR(dl, n->pos, "'%s' element must be int (got %s)",
                    kw, value_type_name(et));
        n->vtype = TYPE_VOID;
        break;
    }

    default:
        /* Should not occur for a well-formed parse; ignore defensively. */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Block: { decls stmts }                                             */
/* ------------------------------------------------------------------ */

static void check_block(Node *n, SymTab *st, DiagList *dl) {
    symtab_enter_scope(st);
    for (int i = 0; i < n->nkids; ++i) {
        Node *kid = n->kids[i];
        if (!kid) continue;
        if (kid->kind == N_DECL) {
            kid->as.decl.sym = symtab_declare(st, kid->as.decl.name,
                                              kid->as.decl.type, kid->pos, dl);
            kid->vtype = kid->as.decl.type;
        } else {
            check_stmt(kid, st, dl);
        }
    }
    n->vtype = TYPE_VOID;
    symtab_leave_scope(st);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int semantic_check(Node *root, SymTab *st, DiagList *dl) {
    if (!root) return 0;
    /* program -> block */
    check_block(root, st, dl);
    return diag_has_errors(dl) ? 1 : 0;
}
