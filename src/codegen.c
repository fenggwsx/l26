/* codegen.c - emit P-Code from the annotated AST.
 *
 * Walks the type-checked AST and produces the Program instruction image.
 * Program layout:  INT 0 <frame_size>  ... body ...  OPR 0 OPR_RET.
 *
 * Scalars (int/bool) live in one frame cell each and travel on the operand
 * stack.  Sets live INLINE in their 201-cell frame region addressed by a base
 * offset; set-valued expressions are materialised directly into a destination
 * region rather than pushed onto the operand stack (see gen_set_into).
 *
 * Boolean operators && / || use SHORT-CIRCUIT evaluation: the right operand is
 * skipped (via JPC/JMP) once the left operand determines the result.  This
 * keeps guard idioms such as `x != 0 && 10 / x > 0` safe from runtime traps
 * (e.g. division by zero) in the logically-skipped operand.
 *
 * Pure C99, no I/O.  Diagnostics (rare after semantic analysis) go to dl.
 */
#include "codegen.h"

/* ------------------------------------------------------------------ */
/* Emission context                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    Program       *out;
    const SymTab  *st;
    DiagList      *dl;
    int            error;   /* sticky: set once any diagnostic is emitted */
} Ctx;

/* Emit one instruction; return its index (for backpatching), or -1 on
 * code-image overflow (diagnostic emitted, error latched). */
static int emit(Ctx *c, VmOp op, int l, int a) {
    if (c->out->count >= L26_MAX_CODE) {
        if (!c->error) {
            SrcPos z = srcpos_make(0, 0, 0);
            diag_add(c->dl, DIAG_ERROR, DIAG_PHASE_CODEGEN, z,
                     "code image overflow (>%d instructions)", L26_MAX_CODE);
        }
        c->error = 1;
        return -1;
    }
    int idx = c->out->count;
    Instruction ins;
    ins.op = op; ins.l = l; ins.a = a;
    c->out->code[idx] = ins;
    c->out->count++;
    return idx;
}

/* Current next instruction address (jump target). */
static int here(Ctx *c) { return c->out->count; }

/* Patch the address operand of a previously emitted jump. */
static void patch(Ctx *c, int idx, int target) {
    if (idx >= 0 && idx < c->out->count)
        c->out->code[idx].a = target;
}

/* Base/frame offset of the symbol referenced by index `sym`. */
static int sym_offset(Ctx *c, int sym) {
    const Symbol *s = symtab_get(c->st, sym);
    return s ? s->offset : 0;
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */
static void gen_expr(Ctx *c, const Node *n);        /* scalar -> stack */
static void gen_set_into(Ctx *c, int dest, const Node *n); /* set -> dest */
static void gen_stmt(Ctx *c, const Node *n);
static void gen_block(Ctx *c, const Node *n);

/* Map an arithmetic/relational OpCode to its OPR sub-function. */
static OprFunc opr_of(OpCode op) {
    switch (op) {
        case OP_ADD: return OPR_ADD;
        case OP_SUB: return OPR_SUB;
        case OP_MUL: return OPR_MUL;
        case OP_DIV: return OPR_DIV;
        case OP_LT:  return OPR_LT;
        case OP_LE:  return OPR_LE;
        case OP_GT:  return OPR_GT;
        case OP_GE:  return OPR_GE;
        case OP_EQ:  return OPR_EQ;
        case OP_NE:  return OPR_NE;
        default:     return OPR_RET; /* unreachable for valid ASTs */
    }
}

/* ------------------------------------------------------------------ */
/* Scalar (int/bool) expression -> one value on the operand stack      */
/* ------------------------------------------------------------------ */
static void gen_expr(Ctx *c, const Node *n) {
    if (!n) return;
    switch (n->kind) {
        case N_NUM:
            emit(c, OP_LIT, 0, (int)n->as.num.value);
            break;

        case N_VAR:
            /* Scalar variable use.  (Set variables only ever appear in set
             * contexts, handled by gen_set_into / set instructions.) */
            emit(c, OP_LOD, 0, sym_offset(c, n->as.var.sym));
            break;

        case N_NEG:
            gen_expr(c, n->kids[0]);
            emit(c, OP_OPR, 0, OPR_NEG);
            break;

        case N_BINOP:   /* arithmetic */
        case N_REL:     /* relational  */
            gen_expr(c, n->as.binop.lhs);
            gen_expr(c, n->as.binop.rhs);
            emit(c, OP_OPR, 0, opr_of(n->as.binop.op));
            break;

        case N_BOOLLIT:
            emit(c, OP_LIT, 0, n->as.boollit.value ? 1 : 0);
            break;

        case N_NOT:
            /* !x : (x == 0) -> push 0, compare equal.  OPR_EQ leaves 1 when
             * x==0, else 0 -> exactly logical negation of a 0/1 bool. */
            gen_expr(c, n->kids[0]);
            emit(c, OP_LIT, 0, 0);
            emit(c, OP_OPR, 0, OPR_EQ);
            break;

        case N_LOGIC:
            /* Short-circuit evaluation of && / ||. The right operand is NOT
             * evaluated when the left already determines the result, so a guard
             * idiom like `x != 0 && 10 / x > 0` never executes the division
             * when x == 0. Operands are 0/1 bools; the result is normalised to
             * 0/1. JPC pops its test value, so each path leaves exactly one
             * value on the stack. */
            if (n->as.binop.op == OP_AND) {
                /* a && b : if a==0 -> 0 ; else (b>0) */
                gen_expr(c, n->as.binop.lhs);
                int jfalse = emit(c, OP_JPC, 0, 0);   /* a==0 -> false      */
                gen_expr(c, n->as.binop.rhs);
                emit(c, OP_LIT, 0, 0);
                emit(c, OP_OPR, 0, OPR_GT);           /* (b>0) -> 0/1       */
                int jend = emit(c, OP_JMP, 0, 0);
                patch(c, jfalse, here(c));
                emit(c, OP_LIT, 0, 0);                /* false branch -> 0  */
                patch(c, jend, here(c));
            } else { /* OP_OR : if a!=0 -> 1 ; else (b>0) */
                gen_expr(c, n->as.binop.lhs);
                int jcheck = emit(c, OP_JPC, 0, 0);   /* a==0 -> check b    */
                emit(c, OP_LIT, 0, 1);                /* a true -> 1        */
                int jend = emit(c, OP_JMP, 0, 0);
                patch(c, jcheck, here(c));
                gen_expr(c, n->as.binop.rhs);
                emit(c, OP_LIT, 0, 0);
                emit(c, OP_OPR, 0, OPR_GT);           /* (b>0) -> 0/1       */
                patch(c, jend, here(c));
            }
            break;

        case N_IN:
            /* aexpr in ID : push element, SIN on the set's base offset. */
            gen_expr(c, n->as.intest.elem);
            emit(c, OP_SIN, 0, sym_offset(c, n->as.intest.sym));
            break;

        case N_ISEMPTY:
            emit(c, OP_SEMPTY, 0, sym_offset(c, n->as.intest.sym));
            break;

        case N_SETEQ: {
            /* set == / != set : push both base offsets, SEQ -> 1/0.
             * For != negate the boolean result via (r == 0). */
            int loff = sym_offset(c, n->as.binop.lhs->as.var.sym);
            int roff = sym_offset(c, n->as.binop.rhs->as.var.sym);
            emit(c, OP_LIT, 0, loff);
            emit(c, OP_LIT, 0, roff);
            emit(c, OP_SEQ, 0, 0);
            if (n->as.binop.op == OP_NE) {
                emit(c, OP_LIT, 0, 0);
                emit(c, OP_OPR, 0, OPR_EQ);   /* equal? 0 : 1 */
            }
            break;
        }

        default:
            /* Should not occur for a semantically valid scalar context. */
            if (!c->error) {
                diag_add(c->dl, DIAG_ERROR, DIAG_PHASE_CODEGEN, n->pos,
                         "codegen: unexpected node in scalar context");
            }
            c->error = 1;
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Set-valued expression -> materialise into destination region `dest` */
/* ------------------------------------------------------------------ */
static void gen_set_into(Ctx *c, int dest, const Node *n) {
    if (!n) return;
    switch (n->kind) {
        case N_SETLIT:
            /* { a1, a2, ... } : clear, then SADD each element value. */
            emit(c, OP_SCLR, 0, dest);
            for (int i = 0; i < n->nkids; ++i) {
                gen_expr(c, n->kids[i]);
                emit(c, OP_SADD, 0, dest);
            }
            break;

        case N_VAR:
            /* set s = t;  copy whole region of source set into dest. */
            emit(c, OP_LIT, 0, sym_offset(c, n->as.var.sym));
            emit(c, OP_SCOPY, 0, dest);
            break;

        case N_SETBINOP: {
            /* ID union ID / ID inter ID : push left & right base offsets,
             * SUNION/SINTER writes result into dest. */
            int loff = sym_offset(c, n->as.setbin.lsym);
            int roff = sym_offset(c, n->as.setbin.rsym);
            emit(c, OP_LIT, 0, loff);
            emit(c, OP_LIT, 0, roff);
            if (n->as.setbin.op == AST_SETBIN_UNION)
                emit(c, OP_SUNION, 0, dest);
            else /* AST_SETBIN_INTER */
                emit(c, OP_SINTER, 0, dest);
            break;
        }

        case N_SETCOMP: {
            /* { gen | x in src if filter }
             *
             * Source layout: cell[src]=count, cell[src+1..src+count]=values
             * (sorted, deduped).  We materialise the result into `dest`.
             *
             * Lowered as a RUNTIME loop over the source elements using the
             * LODX indexed load (pop offset, push cell value), with a hidden
             * loop-index cell (comp.idx_sym):
             *
             *         SCLR tmp                  tmp := {}
             *         LIT 1 ; STO idx           i := 1
             *   loop: LOD idx ; LOD src
             *         OPR le ; JPC end          while (i <= count)
             *         LIT src ; LOD idx
             *         OPR add ; LODX            push cell[src+i]
             *         STO x                     x := element
             *         [filter] ; JPC next      (optional)
             *         [gen] ; SADD tmp          tmp += gen(x)
             *   next: LOD idx ; LIT 1
             *         OPR add ; STO idx         i := i + 1
             *         JMP loop
             *   end:  LIT tmp ; SCOPY dest
             *
             * The result is built into a private scratch set (comp.temp_sym)
             * and copied into `dest` only at the end, so a self-assigning
             * comprehension (s = { e | x in s ... }) does not clear or mutate
             * src while the loop is still reading it.
             *
             * x (compvar_sym) is the comprehension variable's private scalar
             * cell, visible only inside gen/filter (semantic.c scoped it). */
            int xoff   = sym_offset(c, n->as.comp.compvar_sym);
            int srcoff = sym_offset(c, n->as.comp.src_sym);
            int tmpoff = sym_offset(c, n->as.comp.temp_sym);
            int idxoff = sym_offset(c, n->as.comp.idx_sym);

            emit(c, OP_SCLR, 0, tmpoff);           /* tmp := {}             */
            emit(c, OP_LIT, 0, 1);
            emit(c, OP_STO, 0, idxoff);            /* i := 1                */

            int loop = here(c);
            emit(c, OP_LOD, 0, idxoff);            /* push i                */
            emit(c, OP_LOD, 0, srcoff);            /* push count            */
            emit(c, OP_OPR, 0, OPR_LE);            /* i <= count ? 1 : 0    */
            int jend = emit(c, OP_JPC, 0, 0);      /* false -> end          */

            /* x = cell[src + i] (indexed load) */
            emit(c, OP_LIT, 0, srcoff);
            emit(c, OP_LOD, 0, idxoff);
            emit(c, OP_OPR, 0, OPR_ADD);           /* src + i               */
            emit(c, OP_LODX, 0, 0);                /* push cell[src+i]      */
            emit(c, OP_STO, 0, xoff);

            /* optional filter; NULL means always true */
            int skip = -1;
            if (n->as.comp.filter) {
                gen_expr(c, n->as.comp.filter);
                skip = emit(c, OP_JPC, 0, 0);      /* false -> skip element */
            }

            gen_expr(c, n->as.comp.gen);           /* value of gen expr     */
            emit(c, OP_SADD, 0, tmpoff);

            if (skip >= 0) patch(c, skip, here(c));

            /* i := i + 1 ; goto loop */
            emit(c, OP_LOD, 0, idxoff);
            emit(c, OP_LIT, 0, 1);
            emit(c, OP_OPR, 0, OPR_ADD);
            emit(c, OP_STO, 0, idxoff);
            emit(c, OP_JMP, 0, loop);

            /* end: copy the scratch result into dest (safe even if dest==src). */
            patch(c, jend, here(c));
            emit(c, OP_LIT, 0, tmpoff);
            emit(c, OP_SCOPY, 0, dest);
            break;
        }

        default:
            if (!c->error) {
                diag_add(c->dl, DIAG_ERROR, DIAG_PHASE_CODEGEN, n->pos,
                         "codegen: unexpected node in set context");
            }
            c->error = 1;
            break;
    }
}

/* True if the node denotes a set-typed value. */
static int is_set_node(const Node *n) {
    if (!n) return 0;
    if (n->vtype == TYPE_SET) return 1;
    /* Robust fallback by shape in case vtype annotation is absent. */
    switch (n->kind) {
        case N_SETLIT: case N_SETBINOP: case N_SETCOMP:
            return 1;
        default:
            return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */
static void gen_stmt(Ctx *c, const Node *n) {
    if (!n) return;
    switch (n->kind) {
        case N_BLOCK:
            gen_block(c, n);
            break;

        case N_ASSIGN: {
            int off = sym_offset(c, n->as.assign.sym);
            const Symbol *s = symtab_get(c->st, n->as.assign.sym);
            int lhs_is_set = s && s->type == TYPE_SET;
            if (lhs_is_set) {
                gen_set_into(c, off, n->as.assign.rhs);
            } else {
                gen_expr(c, n->as.assign.rhs);
                emit(c, OP_STO, 0, off);
            }
            break;
        }

        case N_IF: {
            gen_expr(c, n->as.if_.cond);
            int jfalse = emit(c, OP_JPC, 0, 0);     /* cond==0 -> skip then  */
            gen_stmt(c, n->as.if_.then_s);
            if (n->as.if_.else_s) {
                int jend = emit(c, OP_JMP, 0, 0);    /* skip else after then  */
                patch(c, jfalse, here(c));
                gen_stmt(c, n->as.if_.else_s);
                patch(c, jend, here(c));
            } else {
                patch(c, jfalse, here(c));
            }
            break;
        }

        case N_WHILE: {
            int top = here(c);
            gen_expr(c, n->as.while_.cond);
            int jexit = emit(c, OP_JPC, 0, 0);
            gen_stmt(c, n->as.while_.body);
            emit(c, OP_JMP, 0, top);
            patch(c, jexit, here(c));
            break;
        }

        case N_WRITE: {
            const Node *e = n->kids[0];
            if (is_set_node(e)) {
                /* write a set: a bare set variable -> SWRITE its region;
                 * a set-valued expression -> materialise into a scratch?  L26
                 * `write` of a set is, per the grammar, `write expr;` where the
                 * common case is a set variable.  For a set variable we SWRITE
                 * directly.  (Set literals/union/inter in write position are
                 * not produced by the example programs; if they appear we have
                 * no scratch region, so report rather than miscompile.) */
                if (e->kind == N_VAR) {
                    emit(c, OP_SWRITE, 0, sym_offset(c, e->as.var.sym));
                } else if (!c->error) {
                    diag_add(c->dl, DIAG_ERROR, DIAG_PHASE_CODEGEN, e->pos,
                             "codegen: writing a non-variable set is unsupported");
                    c->error = 1;
                }
            } else {
                gen_expr(c, e);
                emit(c, OP_OPR, 0, OPR_WRITE);
            }
            emit(c, OP_OPR, 0, OPR_NL);
            break;
        }

        case N_READ: {
            const Symbol *s = symtab_get(c->st, n->as.read.sym);
            int off = s ? s->offset : 0;
            if (s && s->type == TYPE_SET) {
                emit(c, OP_SREAD, 0, off);
            } else {
                emit(c, OP_OPR, 0, OPR_READ);
                emit(c, OP_STO, 0, off);
            }
            break;
        }

        case N_ADD:
            gen_expr(c, n->as.setop.elem);
            emit(c, OP_SADD, 0, sym_offset(c, n->as.setop.sym));
            break;

        case N_REMOVE:
            gen_expr(c, n->as.setop.elem);
            emit(c, OP_SREM, 0, sym_offset(c, n->as.setop.sym));
            break;

        default:
            if (!c->error) {
                diag_add(c->dl, DIAG_ERROR, DIAG_PHASE_CODEGEN, n->pos,
                         "codegen: unexpected statement node");
            }
            c->error = 1;
            break;
    }
}

/* A block holds decls followed by stmts as kids.  Declarations emit no code
 * (storage is pre-allocated by the single program-entry INT). */
static void gen_block(Ctx *c, const Node *n) {
    for (int i = 0; i < n->nkids; ++i) {
        const Node *k = n->kids[i];
        if (!k || k->kind == N_DECL) continue;   /* skip declarations */
        gen_stmt(c, k);
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */
int codegen_run(const Node *root, const SymTab *st, Program *out, DiagList *dl) {
    Ctx c;
    c.out = out;
    c.st = st;
    c.dl = dl;
    c.error = 0;

    out->count = 0;

    /* Program prologue: allocate the single activation record. */
    emit(&c, OP_INT, 0, symtab_frame_size(st));

    /* Body: the program's top-level block. */
    if (root) gen_block(&c, root);

    /* Epilogue: return / halt. */
    emit(&c, OP_OPR, 0, OPR_RET);

    return c.error ? 1 : 0;
}
