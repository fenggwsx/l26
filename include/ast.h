/* ast.h - abstract syntax tree node kinds and constructors.
 *
 * Covers the FULL L26 grammar from AGENTS.md PLUS the two bonus productions:
 *   - set equality / inequality on set operands (== / !=)
 *   - set comprehension  { expr | x in s if cond }
 *
 * The parser builds the tree; semantic.c annotates it (Node.vtype); codegen.c
 * walks it.  All nodes are heap-allocated via the constructors here and freed
 * by ast_free().  Pure C99, no I/O except ast_print (which takes a FILE*).
 */
#ifndef L26_AST_H
#define L26_AST_H

#include <stdio.h>
#include "common.h"

typedef struct Node Node;

/* ------------------------------------------------------------------ */
/* Node kinds                                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    /* ---- structural ---- */
    N_BLOCK,        /* { decls stmts }  : kids = decls... then stmts... */
    N_DECL,         /* <type> ID ;      : as.decl                       */

    /* ---- statements ---- */
    N_ASSIGN,       /* ID = expr ;      : as.assign                     */
    N_IF,           /* if(b) then [else]: as.if_                        */
    N_WHILE,        /* while(b) body    : as.while_                     */
    N_WRITE,        /* write expr ;     : kid[0] = expr                 */
    N_READ,         /* read ID ;        : as.read                       */
    N_ADD,          /* add ID aexpr ;   : as.setop                      */
    N_REMOVE,       /* remove ID aexpr ;: as.setop                      */

    /* ---- arithmetic expressions ---- */
    N_NUM,          /* integer literal  : as.num                        */
    N_VAR,          /* identifier use   : as.var                        */
    N_BINOP,        /* a (+ - * /) b    : as.binop (arith)              */
    N_NEG,          /* unary minus      : kid[0]                        */

    /* ---- boolean expressions ---- */
    N_BOOLLIT,      /* true / false     : as.boollit                    */
    N_NOT,          /* ! b              : kid[0]                        */
    N_LOGIC,        /* b && b , b || b  : as.binop (op = AND/OR)        */
    N_REL,          /* a <rel> a        : as.binop (op = rel)           */

    /* ---- set expressions ---- */
    N_SETLIT,       /* { a1, a2, ... }  : kids = element aexprs         */
    N_SETBINOP,     /* ID union ID / ID inter ID : as.setbin           */
    N_SETEQ,        /* set == / != set  : as.binop (op = EQ/NE) BONUS   */
    N_SETCOMP,      /* { expr | x in s if cond }      BONUS            */

    /* ---- set tests (yield bool) ---- */
    N_IN,           /* aexpr in ID      : as.intest                     */
    N_ISEMPTY       /* isempty(ID)      : as.intest (set side only)     */
} NodeKind;

/* Concrete operator codes used by N_BINOP / N_LOGIC / N_REL / N_SETEQ.
 * Kept distinct from TokenKind so codegen does not depend on lexer ids. */
typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV,          /* arithmetic            */
    OP_AND, OP_OR,                            /* logical               */
    OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE  /* relational / set eq   */
} OpCode;

const char *op_name(OpCode op);

/* ------------------------------------------------------------------ */
/* The node.  A small fixed-arity header plus a variable kid vector    */
/* (used by N_BLOCK and N_SETLIT).  Single tagged union for payloads.  */
/* ------------------------------------------------------------------ */
struct Node {
    NodeKind  kind;
    SrcPos    pos;
    ValueType vtype;     /* filled by semantic analysis; TYPE_ERROR until then */

    /* Variable-length children (owned). For most nodes nkids<=3 and the
     * fixed accessors below alias kids[]. N_BLOCK/N_SETLIT use it fully. */
    Node    **kids;
    int       nkids;
    int       kidcap;

    union {
        struct { ValueType type; char name[L26_MAX_IDENT]; int sym; } decl;
        struct { char name[L26_MAX_IDENT]; int sym; Node *rhs; } assign;
        struct { Node *cond; Node *then_s; Node *else_s; } if_;
        struct { Node *cond; Node *body; } while_;
        struct { char name[L26_MAX_IDENT]; int sym; } read;
        struct { char name[L26_MAX_IDENT]; int sym; Node *elem; } setop; /* add/remove */
        struct { long value; } num;
        struct { char name[L26_MAX_IDENT]; int sym; } var;
        struct { OpCode op; Node *lhs; Node *rhs; } binop; /* arith/logic/rel/seteq */
        struct { int value; } boollit;                     /* 0/1 */
        struct { OpCode op; char lname[L26_MAX_IDENT]; char rname[L26_MAX_IDENT];
                 int lsym; int rsym; } setbin;             /* union/inter */
        struct { char name[L26_MAX_IDENT]; int sym; Node *elem; } intest; /* in / isempty */
        /* Set comprehension: { gen | var in src if filter }
         * var is scoped to the comprehension; semantic.c assigns it a
         * temporary symbol slot (compvar_sym). filter may be NULL. */
        struct { char var[L26_MAX_IDENT]; char src[L26_MAX_IDENT];
                 int src_sym; int compvar_sym;
                 Node *gen; Node *filter; } comp;
    } as;
};

/* ------------------------------------------------------------------ */
/* Constructors. Every constructor sets pos and vtype=TYPE_ERROR and    */
/* takes ownership of any Node* arguments. Return NULL only on OOM.     */
/* ------------------------------------------------------------------ */
Node *ast_block(SrcPos pos);
void  ast_block_add(Node *block, Node *child);   /* append a decl or stmt */

Node *ast_decl(SrcPos pos, ValueType type, const char *name);
Node *ast_assign(SrcPos pos, const char *name, Node *rhs);
Node *ast_if(SrcPos pos, Node *cond, Node *then_s, Node *else_s /*may be NULL*/);
Node *ast_while(SrcPos pos, Node *cond, Node *body);
Node *ast_write(SrcPos pos, Node *expr);
Node *ast_read(SrcPos pos, const char *name);
Node *ast_setop(SrcPos pos, NodeKind kind /*N_ADD|N_REMOVE*/,
                const char *name, Node *elem);

Node *ast_num(SrcPos pos, long value);
Node *ast_var(SrcPos pos, const char *name);
Node *ast_binop(SrcPos pos, OpCode op, Node *lhs, Node *rhs); /* arithmetic */
Node *ast_neg(SrcPos pos, Node *operand);

Node *ast_boollit(SrcPos pos, int value);
Node *ast_not(SrcPos pos, Node *operand);
Node *ast_logic(SrcPos pos, OpCode op /*OP_AND|OP_OR*/, Node *lhs, Node *rhs);
Node *ast_rel(SrcPos pos, OpCode op, Node *lhs, Node *rhs);

Node *ast_setlit(SrcPos pos);                    /* then ast_block_add elems? */
void  ast_setlit_add(Node *setlit, Node *elem);  /* append an element aexpr */
Node *ast_setbin(SrcPos pos, OpCode op /*OP_*-coded union/inter*/,
                 const char *lname, const char *rname);
Node *ast_seteq(SrcPos pos, OpCode op /*OP_EQ|OP_NE*/, Node *lhs, Node *rhs);
Node *ast_setcomp(SrcPos pos, Node *gen, const char *var,
                  const char *src, Node *filter /*may be NULL*/);

Node *ast_in(SrcPos pos, Node *elem, const char *setname);
Node *ast_isempty(SrcPos pos, const char *setname);

/* Recursively free a tree (NULL-safe). */
void ast_free(Node *n);

/* Pretty-print a tree as an indented s-expression to `out`.  This is the
 * only function in ast.* that touches a stream, and only via the caller's
 * FILE*; the core remains side-effect free for analysis callers. */
void ast_print(const Node *n, FILE *out);

/* For N_SETBINOP, union vs inter is encoded in as.setbin.op using OP_OR
 * (union) / OP_AND (inter) as a compact reuse; helpers make intent clear. */
#define AST_SETBIN_UNION OP_OR
#define AST_SETBIN_INTER OP_AND

#endif /* L26_AST_H */
