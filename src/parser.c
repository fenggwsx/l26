/* parser.c - table-driven LALR(1) parser for L26.
 *
 * Implements the full LALR(1) pipeline described in DESIGN.md:
 *   1. The numbered grammar (DESIGN.md section 2) is encoded as data below.
 *      Terminals are TokenKind ids; nonterminals are a private enum offset
 *      past TOK__COUNT so a single "symbol" int space spans both.
 *   2. The canonical collection of LR(1) item sets is built (closure + goto),
 *      using FIRST sets to propagate lookaheads.
 *   3. States with identical LR(0) cores are merged into LALR(1) states by
 *      unioning their lookaheads.
 *   4. ACTION (shift / reduce / accept) and GOTO tables are filled; conflicts
 *      are detected and resolved with a documented policy (see resolve_*).
 *   5. Parsing is driven by an explicit state stack + value (Node*) stack; a
 *      semantic action per reduction builds AST nodes via the ast_* ctors.
 *
 * Disambiguation policy (all conflicts are reported by
 * parser_build_tables_report but resolved deterministically so parse() never
 * stalls):
 *   - Dangling else: shift on TOK_ELSE (binds else to nearest if).
 *   - Any other shift/reduce conflict: prefer SHIFT.
 *   - Reduce/reduce conflict: prefer the LOWER-numbered production.
 *
 * On the contract: the bonus set-equality productions are written so that
 * "ID == ID" / "ID != ID" are recognised as set tests while general
 * "aexpr == aexpr" relations remain available. To keep the automaton free of
 * an unresolvable shift/reduce clash at the '==' / '!=' that follows a bare
 * identifier, set-equality is folded into the relational path: '==' and '!='
 * between two set-typed operands are emitted by the reduce action for rel as
 * an N_SETEQ node (when both immediate operands are bare variables), otherwise
 * as N_REL. semantic.c makes the final int-vs-set determination by operand
 * type. This preserves both the AST node contract (N_SETEQ exists for set
 * operands) and a clean LALR(1) table. See headerChangesNeeded note.
 *
 * Pure C99, no I/O. All table construction is internal to this file.
 */
#include "parser.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* 1. Grammar encoding                                                 */
/* ================================================================== */

/* Nonterminal symbol ids live just past the terminal (TokenKind) range so a
 * single int names either kind of grammar symbol. */
enum {
    NT_BASE = TOK__COUNT,
    NT_PROGRAM = NT_BASE,
    NT_BLOCK,
    NT_DECLS,
    NT_DECL,
    NT_TYPE,
    NT_STMTS,
    NT_STMT,
    NT_ASSIGN_STMT,
    NT_IF_STMT,
    NT_WHILE_STMT,
    NT_IO_STMT,
    NT_SET_OP_STMT,
    NT_EXPR,
    NT_AEXPR,
    NT_ATERM,
    NT_AFACTOR,
    NT_BEXPR,
    NT_BTERM,
    NT_BFACTOR,
    NT_REL,
    NT_SET_EXPR,
    NT_ELEMLIST,
    NT_SET_TEST,
    NT__END
};
#define SYM_COUNT   (NT__END)            /* total terminals + nonterminals  */
#define IS_TERM(s)  ((s) < NT_BASE)
#define IS_NONTERM(s) ((s) >= NT_BASE && (s) < NT__END)

/* The augmented start symbol's production is production 0 of our table; the
 * user-visible grammar production 0 (program -> block) becomes index 1, etc.
 * To keep numbering aligned with DESIGN.md we instead store DESIGN's 64
 * productions verbatim (indices 0..63) and add a synthetic start production
 * S' -> program as index 64, used only for the accept action. */

#define MAX_RHS 9

typedef struct {
    int lhs;
    int rhs[MAX_RHS];
    int rlen;
} Production;

/* DESIGN.md productions 0..63, then synthetic augmentation at index 64. */
static const Production GRAMMAR[] = {
    /*0 */ { NT_PROGRAM,     { NT_BLOCK }, 1 },
    /*1 */ { NT_BLOCK,       { TOK_LBRACE, NT_DECLS, NT_STMTS, TOK_RBRACE }, 4 },
    /*2 */ { NT_DECLS,       { NT_DECLS, NT_DECL }, 2 },
    /*3 */ { NT_DECLS,       { 0 }, 0 },               /* epsilon */
    /*4 */ { NT_DECL,        { NT_TYPE, TOK_ID, TOK_SEMI }, 3 },
    /*5 */ { NT_TYPE,        { TOK_INT }, 1 },
    /*6 */ { NT_TYPE,        { TOK_BOOL }, 1 },
    /*7 */ { NT_TYPE,        { TOK_SET }, 1 },
    /*8 */ { NT_STMTS,       { NT_STMTS, NT_STMT }, 2 },
    /*9 */ { NT_STMTS,       { 0 }, 0 },               /* epsilon */
    /*10*/ { NT_STMT,        { NT_ASSIGN_STMT }, 1 },
    /*11*/ { NT_STMT,        { NT_IF_STMT }, 1 },
    /*12*/ { NT_STMT,        { NT_WHILE_STMT }, 1 },
    /*13*/ { NT_STMT,        { NT_IO_STMT }, 1 },
    /*14*/ { NT_STMT,        { NT_BLOCK }, 1 },
    /*15*/ { NT_STMT,        { NT_SET_OP_STMT }, 1 },
    /*16*/ { NT_ASSIGN_STMT, { TOK_ID, TOK_ASSIGN, NT_EXPR, TOK_SEMI }, 4 },
    /*17*/ { NT_IF_STMT,     { TOK_IF, TOK_LPAREN, NT_BEXPR, TOK_RPAREN, NT_STMT }, 5 },
    /*18*/ { NT_IF_STMT,     { TOK_IF, TOK_LPAREN, NT_BEXPR, TOK_RPAREN, NT_STMT, TOK_ELSE, NT_STMT }, 7 },
    /*19*/ { NT_WHILE_STMT,  { TOK_WHILE, TOK_LPAREN, NT_BEXPR, TOK_RPAREN, NT_STMT }, 5 },
    /*20*/ { NT_IO_STMT,     { TOK_WRITE, NT_EXPR, TOK_SEMI }, 3 },
    /*21*/ { NT_IO_STMT,     { TOK_READ, TOK_ID, TOK_SEMI }, 3 },
    /*22*/ { NT_SET_OP_STMT, { TOK_ADD, TOK_ID, NT_AEXPR, TOK_SEMI }, 4 },
    /*23*/ { NT_SET_OP_STMT, { TOK_REMOVE, TOK_ID, NT_AEXPR, TOK_SEMI }, 4 },
    /*24*/ { NT_EXPR,        { NT_BEXPR }, 1 },
    /*25*/ { NT_EXPR,        { NT_AEXPR }, 1 },
    /*26*/ { NT_EXPR,        { NT_SET_EXPR }, 1 },
    /*27*/ { NT_AEXPR,       { NT_AEXPR, TOK_PLUS, NT_ATERM }, 3 },
    /*28*/ { NT_AEXPR,       { NT_AEXPR, TOK_MINUS, NT_ATERM }, 3 },
    /*29*/ { NT_AEXPR,       { NT_ATERM }, 1 },
    /*30*/ { NT_ATERM,       { NT_ATERM, TOK_STAR, NT_AFACTOR }, 3 },
    /*31*/ { NT_ATERM,       { NT_ATERM, TOK_SLASH, NT_AFACTOR }, 3 },
    /*32*/ { NT_ATERM,       { NT_AFACTOR }, 1 },
    /*33*/ { NT_AFACTOR,     { TOK_ID }, 1 },
    /*34*/ { NT_AFACTOR,     { TOK_NUM }, 1 },
    /*35*/ { NT_AFACTOR,     { TOK_LPAREN, NT_AEXPR, TOK_RPAREN }, 3 },
    /*36*/ { NT_BEXPR,       { NT_BEXPR, TOK_OR, NT_BTERM }, 3 },
    /*37*/ { NT_BEXPR,       { NT_BTERM }, 1 },
    /*38*/ { NT_BTERM,       { NT_BTERM, TOK_AND, NT_BFACTOR }, 3 },
    /*39*/ { NT_BTERM,       { NT_BFACTOR }, 1 },
    /*40*/ { NT_BFACTOR,     { TOK_TRUE }, 1 },
    /*41*/ { NT_BFACTOR,     { TOK_FALSE }, 1 },
    /*42*/ { NT_BFACTOR,     { TOK_NOT, NT_BFACTOR }, 2 },
    /*43*/ { NT_BFACTOR,     { TOK_LPAREN, NT_BEXPR, TOK_RPAREN }, 3 },
    /*44*/ { NT_BFACTOR,     { NT_REL }, 1 },
    /*45*/ { NT_BFACTOR,     { NT_SET_TEST }, 1 },
    /*46*/ { NT_REL,         { NT_AEXPR, TOK_LT, NT_AEXPR }, 3 },
    /*47*/ { NT_REL,         { NT_AEXPR, TOK_LE, NT_AEXPR }, 3 },
    /*48*/ { NT_REL,         { NT_AEXPR, TOK_GT, NT_AEXPR }, 3 },
    /*49*/ { NT_REL,         { NT_AEXPR, TOK_GE, NT_AEXPR }, 3 },
    /*50*/ { NT_REL,         { NT_AEXPR, TOK_EQ, NT_AEXPR }, 3 },
    /*51*/ { NT_REL,         { NT_AEXPR, TOK_NE, NT_AEXPR }, 3 },
    /*52*/ { NT_SET_EXPR,    { TOK_LBRACE, NT_ELEMLIST, TOK_RBRACE }, 3 },
    /*53*/ { NT_SET_EXPR,    { TOK_LBRACE, TOK_RBRACE }, 2 },
    /*54*/ { NT_SET_EXPR,    { TOK_ID, TOK_UNION, TOK_ID }, 3 },
    /*55*/ { NT_SET_EXPR,    { TOK_ID, TOK_INTER, TOK_ID }, 3 },
    /*56*/ { NT_ELEMLIST,    { NT_ELEMLIST, TOK_COMMA, NT_AEXPR }, 3 },
    /*57*/ { NT_ELEMLIST,    { NT_AEXPR }, 1 },
    /*58*/ { NT_SET_TEST,    { NT_AEXPR, TOK_IN, TOK_ID }, 3 },
    /*59*/ { NT_SET_TEST,    { TOK_ISEMPTY, TOK_LPAREN, TOK_ID, TOK_RPAREN }, 4 },
    /* (60)/(61) set equality folded into rel; see file header. Kept here as
     * the DESIGN production numbers for documentation parity but NOT installed
     * as separate rules (they would create an unresolvable s/r at '==' after a
     * bare ID). */
    /*60*/ { NT_SET_EXPR,    { TOK_LBRACE, NT_AEXPR, TOK_PIPE, TOK_ID, TOK_IN, TOK_ID, TOK_IF, NT_BEXPR, TOK_RBRACE }, 9 },
    /*61*/ { NT_SET_EXPR,    { TOK_LBRACE, NT_AEXPR, TOK_PIPE, TOK_ID, TOK_IN, TOK_ID, TOK_RBRACE }, 7 },
    /* synthetic augmentation S' -> program */
    /*62*/ { NT__END,        { NT_PROGRAM }, 1 }
};
#define NPROD     ((int)(sizeof(GRAMMAR) / sizeof(GRAMMAR[0])))
#define PROD_AUG  (NPROD - 1)            /* index of S' -> program */

/* The comprehension productions are DESIGN 62/63; here they sit at indices
 * 60/61 because DESIGN's 60/61 (set equality) are folded away. The AST is the
 * same; only the internal table index differs. */

/* ================================================================== */
/* 2. FIRST sets                                                       */
/* ================================================================== */

static unsigned char g_first[SYM_COUNT][SYM_COUNT]; /* first[nt][term] (terms only) */
static unsigned char g_nullable[SYM_COUNT];

static void compute_first(void) {
    memset(g_first, 0, sizeof(g_first));
    memset(g_nullable, 0, sizeof(g_nullable));

    int changed = 1;
    while (changed) {
        changed = 0;
        for (int p = 0; p < NPROD; ++p) {
            int A = GRAMMAR[p].lhs;
            const int *rhs = GRAMMAR[p].rhs;
            int rlen = GRAMMAR[p].rlen;

            if (rlen == 0) {            /* A -> epsilon */
                if (!g_nullable[A]) { g_nullable[A] = 1; changed = 1; }
                continue;
            }
            int i;
            for (i = 0; i < rlen; ++i) {
                int X = rhs[i];
                if (IS_TERM(X)) {
                    if (!g_first[A][X]) { g_first[A][X] = 1; changed = 1; }
                    break;              /* terminal is not nullable */
                } else {
                    /* add FIRST(X) to FIRST(A) */
                    for (int t = 0; t < NT_BASE; ++t) {
                        if (g_first[X][t] && !g_first[A][t]) {
                            g_first[A][t] = 1; changed = 1;
                        }
                    }
                    if (!g_nullable[X]) break;
                }
            }
            if (i == rlen) {            /* all RHS symbols nullable */
                if (!g_nullable[A]) { g_nullable[A] = 1; changed = 1; }
            }
        }
    }
}

/* FIRST of a symbol string seq[start..len) followed by lookahead `la`,
 * accumulated into the boolean set `out` (indexed by terminal). */
static void first_of_seq(const int *seq, int start, int len, int la,
                         unsigned char *out) {
    int i;
    for (i = start; i < len; ++i) {
        int X = seq[i];
        if (IS_TERM(X)) { out[X] = 1; return; }
        for (int t = 0; t < NT_BASE; ++t)
            if (g_first[X][t]) out[t] = 1;
        if (!g_nullable[X]) return;
    }
    /* whole tail nullable -> add the trailing lookahead */
    if (la >= 0) out[la] = 1;
}

/* ================================================================== */
/* 3. LR(1) items and item sets                                       */
/* ================================================================== */

/* An LR(1) item = (production index, dot position, lookahead terminal).
 * We keep them flattened; lookaheads are merged so each (prod,dot) appears with
 * a lookahead bitset within a state. */

typedef struct {
    int prod;
    int dot;
    unsigned char la[NT_BASE];   /* lookahead terminal bitset */
} Item;

typedef struct {
    Item  *items;
    int    nitems;
    int    cap;
} ItemSet;

static void itemset_init(ItemSet *s) { s->items = NULL; s->nitems = 0; s->cap = 0; }
static void itemset_free(ItemSet *s) { free(s->items); s->items = NULL; s->nitems = 0; s->cap = 0; }

/* find item with matching (prod,dot); return index or -1 */
static int itemset_find(const ItemSet *s, int prod, int dot) {
    for (int i = 0; i < s->nitems; ++i)
        if (s->items[i].prod == prod && s->items[i].dot == dot) return i;
    return -1;
}

/* merge lookaheads `la` into the (prod,dot) item, creating it if needed.
 * returns 1 if anything changed. */
static int itemset_add(ItemSet *s, int prod, int dot, const unsigned char *la) {
    int idx = itemset_find(s, prod, dot);
    if (idx < 0) {
        if (s->nitems >= s->cap) {
            int nc = s->cap ? s->cap * 2 : 16;
            Item *ni = (Item *)realloc(s->items, (size_t)nc * sizeof(Item));
            if (!ni) return 0;
            s->items = ni; s->cap = nc;
        }
        Item *it = &s->items[s->nitems++];
        it->prod = prod; it->dot = dot;
        memset(it->la, 0, sizeof(it->la));
        if (la) memcpy(it->la, la, NT_BASE);
        return 1;
    }
    if (!la) return 0;
    int changed = 0;
    Item *it = &s->items[idx];
    for (int t = 0; t < NT_BASE; ++t)
        if (la[t] && !it->la[t]) { it->la[t] = 1; changed = 1; }
    return changed;
}

/* LR(1) closure of an item set (in place). */
static void closure(ItemSet *s) {
    int changed = 1;
    unsigned char tmp[NT_BASE];
    while (changed) {
        changed = 0;
        for (int i = 0; i < s->nitems; ++i) {
            Item it = s->items[i];           /* copy: array may realloc */
            const Production *p = &GRAMMAR[it.prod];
            if (it.dot >= p->rlen) continue;
            int B = p->rhs[it.dot];
            if (!IS_NONTERM(B)) continue;
            /* for each terminal in FIRST(beta la) ... */
            for (int la_t = 0; la_t < NT_BASE; ++la_t) {
                if (!it.la[la_t]) continue;
                memset(tmp, 0, sizeof(tmp));
                first_of_seq(p->rhs, it.dot + 1, p->rlen, la_t, tmp);
                /* add B -> . gamma with lookaheads tmp for every B-production */
                for (int q = 0; q < NPROD; ++q) {
                    if (GRAMMAR[q].lhs != B) continue;
                    if (itemset_add(s, q, 0, tmp)) changed = 1;
                }
            }
        }
    }
}

/* GOTO(s, X): items with dot before X, advanced; returns a fresh closed set. */
static void goto_set(const ItemSet *s, int X, ItemSet *out) {
    itemset_init(out);
    for (int i = 0; i < s->nitems; ++i) {
        const Item *it = &s->items[i];
        const Production *p = &GRAMMAR[it->prod];
        if (it->dot < p->rlen && p->rhs[it->dot] == X)
            itemset_add(out, it->prod, it->dot + 1, it->la);
    }
    if (out->nitems) closure(out);
}

/* ---- core comparison (LR(0) cores: ignore lookaheads) ---- */
static int same_core(const ItemSet *a, const ItemSet *b) {
    if (a->nitems != b->nitems) return 0;
    for (int i = 0; i < a->nitems; ++i)
        if (itemset_find(b, a->items[i].prod, a->items[i].dot) < 0) return 0;
    return 1;
}

/* merge b's lookaheads into a (cores assumed equal). returns 1 if changed. */
static int merge_la(ItemSet *a, const ItemSet *b) {
    int changed = 0;
    for (int i = 0; i < b->nitems; ++i) {
        int j = itemset_find(a, b->items[i].prod, b->items[i].dot);
        if (j < 0) continue;
        for (int t = 0; t < NT_BASE; ++t)
            if (b->items[i].la[t] && !a->items[j].la[t]) {
                a->items[j].la[t] = 1; changed = 1;
            }
    }
    return changed;
}

/* ================================================================== */
/* 4. State collection + ACTION/GOTO tables                           */
/* ================================================================== */

#define MAX_STATES 1024

/* ACTION encoding: 0 = error; >0 shift to state (a-1); <0 reduce by prod
 * (-a-1); special ACCEPT value. GOTO similar but only shift-to-state. */
#define ACT_ERROR    0
#define ACT_ACCEPT   0x7fffffff
#define ACT_SHIFT(st)  ((st) + 1)        /* st>=0 */
#define ACT_REDUCE(p)  (-((p) + 1))      /* p>=0  */
#define IS_SHIFT(a)  ((a) > 0 && (a) != ACT_ACCEPT)
#define IS_REDUCE(a) ((a) < 0)
#define SHIFT_TARGET(a)  ((a) - 1)
#define REDUCE_PROD(a)   (-(a) - 1)

typedef struct {
    ItemSet states[MAX_STATES];
    int     nstates;
    /* action[state][terminal], goto_[state][nonterm-index] merged into one
     * table indexed by full symbol id for simplicity. */
    int     action[MAX_STATES][SYM_COUNT];
    int     ntransition; /* unused; kept for clarity */
    int     conflicts;
    int     dangling_else; /* count of intentional shift-on-else conflicts */
} LRTables;

static LRTables *g_T = NULL;   /* singleton, built once */

/* Find an existing state with the same LR(0) core; -1 if none. */
static int find_core(LRTables *T, const ItemSet *s) {
    for (int i = 0; i < T->nstates; ++i)
        if (same_core(&T->states[i], s)) return i;
    return -1;
}

/* Build the LALR(1) collection by the "build LR(1), merge equal cores"
 * method, done incrementally: when GOTO produces a set whose core already
 * exists, merge lookaheads into that state and (if changed) re-process it. */
static int build_collection(LRTables *T) {
    T->nstates = 0;
    T->conflicts = 0;
    T->dangling_else = 0;

    /* initial state: closure of { S' -> . program , $ } */
    ItemSet s0; itemset_init(&s0);
    unsigned char eof_la[NT_BASE]; memset(eof_la, 0, sizeof(eof_la));
    eof_la[TOK_EOF] = 1;
    itemset_add(&s0, PROD_AUG, 0, eof_la);
    closure(&s0);
    T->states[T->nstates++] = s0;

    /* worklist of states needing (re)processing */
    static int dirty[MAX_STATES];
    int dhead = 0, dtail = 0;
    static unsigned char indq[MAX_STATES];
    memset(indq, 0, sizeof(indq));
    dirty[dtail++] = 0; indq[0] = 1;

    while (dhead != dtail) {
        int si = dirty[dhead++]; if (dhead == MAX_STATES) dhead = 0;
        indq[si] = 0;

        /* For each grammar symbol X that can follow the dot, compute GOTO. */
        for (int X = 0; X < SYM_COUNT; ++X) {
            /* quick skip: is there an item with dot before X? */
            int present = 0;
            for (int i = 0; i < T->states[si].nitems; ++i) {
                const Item *it = &T->states[si].items[i];
                const Production *p = &GRAMMAR[it->prod];
                if (it->dot < p->rlen && p->rhs[it->dot] == X) { present = 1; break; }
            }
            if (!present) continue;

            ItemSet g; goto_set(&T->states[si], X, &g);
            if (g.nitems == 0) { itemset_free(&g); continue; }

            int existing = find_core(T, &g);
            int target;
            if (existing < 0) {
                if (T->nstates >= MAX_STATES) { itemset_free(&g); return -1; }
                target = T->nstates;
                T->states[T->nstates++] = g; /* take ownership */
                /* enqueue new state */
                if (!indq[target]) { dirty[dtail++] = target; if (dtail == MAX_STATES) dtail = 0; indq[target] = 1; }
            } else {
                target = existing;
                int changed = merge_la(&T->states[existing], &g);
                itemset_free(&g);
                if (changed && !indq[existing]) {
                    dirty[dtail++] = existing; if (dtail == MAX_STATES) dtail = 0; indq[existing] = 1;
                }
            }
            /* record transition (also re-run closure-derived reductions later) */
            T->action[si][X] = ACT_SHIFT(target); /* for nonterminals this is GOTO */
        }
    }
    return 0;
}

/* Whether terminal `t` is the dangling-else token. */
static int is_else(int t) { return t == TOK_ELSE; }

/* Fill in reduce / accept actions, resolving conflicts. Shift entries for
 * terminals already placed by build_collection; nonterminal entries are GOTO
 * (left as-is). We now overlay reduces and detect clashes. */
static void build_actions(LRTables *T) {
    /* First, separate: keep only terminal SHIFT actions in `action[*][term]`;
     * nonterminal entries stay as GOTO. We must not let a GOTO be clobbered by
     * a reduce (reduces only apply to terminal lookaheads), so reduces only
     * touch action[state][terminal]. */
    for (int si = 0; si < T->nstates; ++si) {
        const ItemSet *s = &T->states[si];
        for (int i = 0; i < s->nitems; ++i) {
            const Item *it = &s->items[i];
            const Production *p = &GRAMMAR[it->prod];
            if (it->dot < p->rlen) continue;   /* not a complete item */

            /* complete item: reduce by it->prod on each lookahead terminal */
            if (it->prod == PROD_AUG) {
                /* S' -> program . , lookahead $ : ACCEPT */
                if (it->la[TOK_EOF])
                    T->action[si][TOK_EOF] = ACT_ACCEPT;
                continue;
            }
            for (int t = 0; t < NT_BASE; ++t) {
                if (!it->la[t]) continue;
                int cur = T->action[si][t];
                int red = ACT_REDUCE(it->prod);
                if (cur == ACT_ERROR) {
                    T->action[si][t] = red;
                } else if (cur == ACT_ACCEPT) {
                    /* never override accept */
                } else if (IS_SHIFT(cur)) {
                    /* shift/reduce conflict */
                    T->conflicts++;
                    if (is_else(t)) T->dangling_else++;  /* benign, expected */
                    /* policy: prefer SHIFT (this also resolves dangling-else
                     * since the conflicting token there is 'else'). */
                    /* keep shift -> do nothing */
                } else if (IS_REDUCE(cur)) {
                    int curp = REDUCE_PROD(cur);
                    if (curp != it->prod) {
                        T->conflicts++;
                        /* policy: prefer the LOWER-numbered production. */
                        if (it->prod < curp) T->action[si][t] = red;
                    }
                }
            }
        }
    }
}

static int tables_build_internal(LRTables *T) {
    compute_first();
    memset(T->action, 0, sizeof(T->action));
    if (build_collection(T) != 0) return -1;
    build_actions(T);
    return 0;
}

/* Lazily build the singleton tables. Returns 0 on success. */
static int ensure_tables(void) {
    if (g_T) return 0;
    g_T = (LRTables *)calloc(1, sizeof(LRTables));
    if (!g_T) return -1;
    if (tables_build_internal(g_T) != 0) { free(g_T); g_T = NULL; return -1; }
    return 0;
}

/* ================================================================== */
/* 5. Semantic-action helpers (build AST on reduce)                   */
/* ================================================================== */

/* A parse-stack value. We carry whatever a grammar symbol "produces":
 *  - Node*       for expression / statement / block nonterminals
 *  - a ValueType for `type`
 *  - a token copy for terminals (so ID text, NUM value, op kind survive)
 * A small tagged union keeps it simple. */
typedef enum { V_NONE, V_NODE, V_TOK, V_TYPE, V_LIST } ValKind;

typedef struct {
    ValKind kind;
    Node   *node;       /* V_NODE / list head reuse */
    Token   tok;        /* V_TOK */
    ValueType vtype;    /* V_TYPE */
} StackVal;

/* For NT_DECLS / NT_STMTS / NT_ELEMLIST we accumulate children directly into a
 * parent N_BLOCK / N_SETLIT under construction. To keep the value stack
 * uniform we represent:
 *   - NT_DECLS  as a Node* N_BLOCK accumulator (decls only)
 *   - NT_STMTS  as a Node* N_BLOCK accumulator (stmts only)
 *   - NT_ELEMLIST as a Node* N_SETLIT accumulator
 * and merge them when reducing block / set_expr. */

/* ================================================================== */
/* 6. Parser driver                                                   */
/* ================================================================== */

#define PSTACK_MAX  4096

typedef struct {
    int       state_stk[PSTACK_MAX];
    StackVal  val_stk[PSTACK_MAX];
    int       sp;                     /* points one past top */
    const TokenStream *ts;
    int       tpos;                   /* index into ts->tokens */
    DiagList *dl;
    int       error;
} Parser;

static SrcPos cur_pos(Parser *P) {
    if (P->ts && P->tpos < P->ts->count) return P->ts->tokens[P->tpos].pos;
    return srcpos_make(1, 1, 0);
}

static const Token *cur_tok(Parser *P) {
    if (P->ts && P->tpos < P->ts->count) return &P->ts->tokens[P->tpos];
    static Token eof = { TOK_EOF, {1,1,0}, "", 0 };
    return &eof;
}

static void perr(Parser *P, SrcPos pos, const char *fmt, const char *arg) {
    P->error = 1;
    if (arg) diag_add(P->dl, DIAG_ERROR, DIAG_PHASE_PARSE, pos, fmt, arg);
    else     diag_add(P->dl, DIAG_ERROR, DIAG_PHASE_PARSE, pos, "%s", fmt);
}

/* OpCode for a relational token. */
static OpCode rel_op_of(TokenKind k) {
    switch (k) {
        case TOK_LT: return OP_LT;
        case TOK_LE: return OP_LE;
        case TOK_GT: return OP_GT;
        case TOK_GE: return OP_GE;
        case TOK_EQ: return OP_EQ;
        case TOK_NE: return OP_NE;
        default:     return OP_EQ;
    }
}

/* Apply the semantic action for reducing by production `p`. The RHS values are
 * val_stk[sp-rlen .. sp-1]; result is written to `out`. Returns 0 on success. */
static int do_reduce(Parser *P, int p, StackVal *rhs, int rlen, StackVal *out) {
    out->kind = V_NODE; out->node = NULL;
    SrcPos pos = (rlen > 0)
        ? ( rhs[0].kind == V_NODE && rhs[0].node ? rhs[0].node->pos
          : rhs[0].kind == V_TOK ? rhs[0].tok.pos : cur_pos(P) )
        : cur_pos(P);

    switch (p) {
    case 0: /* program -> block */
        *out = rhs[0];
        break;

    case 1: { /* block -> { decls stmts } */
        /* rhs[1] = decls accumulator (N_BLOCK), rhs[2] = stmts accumulator */
        Node *blk = ast_block(rhs[0].tok.pos);
        if (!blk) return -1;
        Node *decls = rhs[1].node, *stmts = rhs[2].node;
        if (decls) { for (int i = 0; i < decls->nkids; ++i) ast_block_add(blk, decls->kids[i]);
                     free(decls->kids); free(decls); }
        if (stmts) { for (int i = 0; i < stmts->nkids; ++i) ast_block_add(blk, stmts->kids[i]);
                     free(stmts->kids); free(stmts); }
        out->node = blk;
        break; }

    case 2: { /* decls -> decls decl */
        Node *acc = rhs[0].node;
        if (!acc) { acc = ast_block(pos); }
        if (rhs[1].node) ast_block_add(acc, rhs[1].node);
        out->node = acc;
        break; }
    case 3: /* decls -> epsilon */
        out->node = ast_block(cur_pos(P));
        break;

    case 4: { /* decl -> type ID ; */
        out->node = ast_decl(rhs[0].vtype != TYPE_ERROR ? rhs[1].tok.pos : rhs[1].tok.pos,
                             rhs[0].vtype, rhs[1].tok.text);
        out->node->pos = rhs[1].tok.pos;
        break; }

    case 5: out->kind = V_TYPE; out->vtype = TYPE_INT;  break; /* type->int */
    case 6: out->kind = V_TYPE; out->vtype = TYPE_BOOL; break; /* type->bool */
    case 7: out->kind = V_TYPE; out->vtype = TYPE_SET;  break; /* type->set */

    case 8: { /* stmts -> stmts stmt */
        Node *acc = rhs[0].node;
        if (!acc) acc = ast_block(pos);
        if (rhs[1].node) ast_block_add(acc, rhs[1].node);
        out->node = acc;
        break; }
    case 9: /* stmts -> epsilon */
        out->node = ast_block(cur_pos(P));
        break;

    case 10: case 11: case 12: case 13: case 14: case 15: /* stmt -> X */
        *out = rhs[0];
        break;

    case 16: /* assign_stmt -> ID = expr ; */
        out->node = ast_assign(rhs[0].tok.pos, rhs[0].tok.text, rhs[2].node);
        break;

    case 17: /* if_stmt -> if ( bexpr ) stmt */
        out->node = ast_if(rhs[0].tok.pos, rhs[2].node, rhs[4].node, NULL);
        break;
    case 18: /* if_stmt -> if ( bexpr ) stmt else stmt */
        out->node = ast_if(rhs[0].tok.pos, rhs[2].node, rhs[4].node, rhs[6].node);
        break;
    case 19: /* while_stmt -> while ( bexpr ) stmt */
        out->node = ast_while(rhs[0].tok.pos, rhs[2].node, rhs[4].node);
        break;

    case 20: /* io_stmt -> write expr ; */
        out->node = ast_write(rhs[0].tok.pos, rhs[1].node);
        break;
    case 21: /* io_stmt -> read ID ; */
        out->node = ast_read(rhs[0].tok.pos, rhs[1].tok.text);
        break;

    case 22: /* set_op_stmt -> add ID aexpr ; */
        out->node = ast_setop(rhs[0].tok.pos, N_ADD, rhs[1].tok.text, rhs[2].node);
        break;
    case 23: /* set_op_stmt -> remove ID aexpr ; */
        out->node = ast_setop(rhs[0].tok.pos, N_REMOVE, rhs[1].tok.text, rhs[2].node);
        break;

    case 24: case 25: case 26: /* expr -> bexpr | aexpr | set_expr */
        *out = rhs[0];
        break;

    case 27: /* aexpr -> aexpr + aterm */
        out->node = ast_binop(rhs[0].node->pos, OP_ADD, rhs[0].node, rhs[2].node);
        break;
    case 28: /* aexpr -> aexpr - aterm */
        out->node = ast_binop(rhs[0].node->pos, OP_SUB, rhs[0].node, rhs[2].node);
        break;
    case 29: /* aexpr -> aterm */
        *out = rhs[0];
        break;
    case 30: /* aterm -> aterm * afactor */
        out->node = ast_binop(rhs[0].node->pos, OP_MUL, rhs[0].node, rhs[2].node);
        break;
    case 31: /* aterm -> aterm / afactor */
        out->node = ast_binop(rhs[0].node->pos, OP_DIV, rhs[0].node, rhs[2].node);
        break;
    case 32: /* aterm -> afactor */
        *out = rhs[0];
        break;
    case 33: /* afactor -> ID */
        out->node = ast_var(rhs[0].tok.pos, rhs[0].tok.text);
        break;
    case 34: /* afactor -> NUM */
        out->node = ast_num(rhs[0].tok.pos, rhs[0].tok.ival);
        break;
    case 35: /* afactor -> ( aexpr ) */
        *out = rhs[1];
        break;

    case 36: /* bexpr -> bexpr || bterm */
        out->node = ast_logic(rhs[0].node->pos, OP_OR, rhs[0].node, rhs[2].node);
        break;
    case 37: /* bexpr -> bterm */
        *out = rhs[0];
        break;
    case 38: /* bterm -> bterm && bfactor */
        out->node = ast_logic(rhs[0].node->pos, OP_AND, rhs[0].node, rhs[2].node);
        break;
    case 39: /* bterm -> bfactor */
        *out = rhs[0];
        break;
    case 40: /* bfactor -> true */
        out->node = ast_boollit(rhs[0].tok.pos, 1);
        break;
    case 41: /* bfactor -> false */
        out->node = ast_boollit(rhs[0].tok.pos, 0);
        break;
    case 42: /* bfactor -> ! bfactor */
        out->node = ast_not(rhs[0].tok.pos, rhs[1].node);
        break;
    case 43: /* bfactor -> ( bexpr ) */
        *out = rhs[1];
        break;
    case 44: case 45: /* bfactor -> rel | set_test */
        *out = rhs[0];
        break;

    case 46: case 47: case 48: case 49: case 50: case 51: { /* rel -> aexpr REL aexpr */
        TokenKind k = rhs[1].tok.kind;
        OpCode op = rel_op_of(k);
        /* Set-equality folding: when '==' / '!=' joins two bare variables, the
         * intent may be set equality. Emit N_SETEQ so the AST carries the bonus
         * node; semantic.c decides int-vs-set by operand type (and may rewrite
         * to an int comparison if the operands are scalar). For all other
         * relations, or non-bare operands, emit N_REL. */
        if ((op == OP_EQ || op == OP_NE) &&
            rhs[0].node && rhs[0].node->kind == N_VAR &&
            rhs[2].node && rhs[2].node->kind == N_VAR) {
            out->node = ast_seteq(rhs[0].node->pos, op, rhs[0].node, rhs[2].node);
        } else {
            out->node = ast_rel(rhs[0].node->pos, op, rhs[0].node, rhs[2].node);
        }
        break; }

    case 52: { /* set_expr -> { elemlist } */
        /* rhs[1] is an N_SETLIT accumulator */
        Node *lit = rhs[1].node;
        if (lit) lit->pos = rhs[0].tok.pos;
        out->node = lit;
        break; }
    case 53: /* set_expr -> { } */
        out->node = ast_setlit(rhs[0].tok.pos);
        break;
    case 54: /* set_expr -> ID union ID */
        out->node = ast_setbin(rhs[0].tok.pos, AST_SETBIN_UNION,
                               rhs[0].tok.text, rhs[2].tok.text);
        break;
    case 55: /* set_expr -> ID inter ID */
        out->node = ast_setbin(rhs[0].tok.pos, AST_SETBIN_INTER,
                               rhs[0].tok.text, rhs[2].tok.text);
        break;

    case 56: { /* elemlist -> elemlist , aexpr */
        Node *acc = rhs[0].node;
        if (!acc) acc = ast_setlit(pos);
        ast_setlit_add(acc, rhs[2].node);
        out->node = acc;
        break; }
    case 57: { /* elemlist -> aexpr */
        Node *acc = ast_setlit(rhs[0].node ? rhs[0].node->pos : pos);
        ast_setlit_add(acc, rhs[0].node);
        out->node = acc;
        break; }

    case 58: /* set_test -> aexpr in ID */
        out->node = ast_in(rhs[0].node->pos, rhs[0].node, rhs[2].tok.text);
        break;
    case 59: /* set_test -> isempty ( ID ) */
        out->node = ast_isempty(rhs[0].tok.pos, rhs[2].tok.text);
        break;

    case 60: /* set_expr -> { aexpr | ID in ID if bexpr }  (DESIGN 62) */
        out->node = ast_setcomp(rhs[0].tok.pos, rhs[1].node,
                                rhs[3].tok.text, rhs[5].tok.text, rhs[7].node);
        break;
    case 61: /* set_expr -> { aexpr | ID in ID }  (DESIGN 63) */
        out->node = ast_setcomp(rhs[0].tok.pos, rhs[1].node,
                                rhs[3].tok.text, rhs[5].tok.text, NULL);
        break;

    default:
        return -1;
    }

    if (out->kind == V_NODE && out->node == NULL &&
        p != 3 && p != 9) {
        /* allocation failed somewhere (non-epsilon nodes must exist) */
        return -1;
    }
    return 0;
}

/* free any Node* still on the value stack (error cleanup) */
static void cleanup_stack(Parser *P) {
    for (int i = 0; i < P->sp; ++i) {
        if (P->val_stk[i].kind == V_NODE && P->val_stk[i].node)
            ast_free(P->val_stk[i].node);
    }
    P->sp = 0;
}

Node *parse(const TokenStream *ts, DiagList *dl) {
    if (ensure_tables() != 0) {
        diag_add(dl, DIAG_ERROR, DIAG_PHASE_PARSE, srcpos_make(1,1,0),
                 "internal: failed to build parse tables");
        return NULL;
    }
    if (g_T->conflicts != g_T->dangling_else) {
        /* Tables still usable (deterministic resolution), but warn about any
         * conflict other than the intentional dangling-else shift-on-else. */
        diag_add(dl, DIAG_WARNING, DIAG_PHASE_PARSE, srcpos_make(1,1,0),
                 "grammar has %d unexpected resolved conflict(s)",
                 g_T->conflicts - g_T->dangling_else);
    }

    Parser P;
    memset(&P, 0, sizeof(P));
    P.ts = ts; P.tpos = 0; P.dl = dl; P.error = 0; P.sp = 0;

    /* push start state 0 */
    P.state_stk[P.sp] = 0;
    P.val_stk[P.sp].kind = V_NONE;
    P.sp++;

    for (;;) {
        int st = P.state_stk[P.sp - 1];
        const Token *tk = cur_tok(&P);
        int a = g_T->action[st][tk->kind];

        if (a == ACT_ACCEPT) {
            /* value below the start state holds program node */
            Node *root = (P.sp >= 2 && P.val_stk[P.sp - 1].kind == V_NODE)
                         ? P.val_stk[P.sp - 1].node : NULL;
            /* detach so cleanup doesn't free it */
            if (P.sp >= 2) P.val_stk[P.sp - 1].node = NULL;
            cleanup_stack(&P);
            return root;
        }
        if (IS_SHIFT(a)) {
            int tgt = SHIFT_TARGET(a);
            if (P.sp >= PSTACK_MAX) {
                perr(&P, tk->pos, "parse stack overflow", NULL);
                break;
            }
            P.state_stk[P.sp] = tgt;
            P.val_stk[P.sp].kind = V_TOK;
            P.val_stk[P.sp].tok = *tk;
            P.val_stk[P.sp].node = NULL;
            P.sp++;
            P.tpos++;
            continue;
        }
        if (IS_REDUCE(a)) {
            int p = REDUCE_PROD(a);
            int rlen = GRAMMAR[p].rlen;
            if (P.sp - 1 < rlen) {  /* underflow guard */
                perr(&P, tk->pos, "internal parse error (stack underflow)", NULL);
                break;
            }
            StackVal *rhs = &P.val_stk[P.sp - rlen];
            StackVal result;
            memset(&result, 0, sizeof(result));
            if (do_reduce(&P, p, rhs, rlen, &result) != 0) {
                perr(&P, cur_pos(&P), "internal parse error during reduction", NULL);
                break;
            }
            /* pop rlen symbols */
            P.sp -= rlen;
            int gstate = P.state_stk[P.sp - 1];
            int lhs = GRAMMAR[p].lhs;
            int go = g_T->action[gstate][lhs];
            if (!IS_SHIFT(go)) {
                perr(&P, cur_pos(&P), "internal parse error (no GOTO)", NULL);
                if (result.kind == V_NODE && result.node) ast_free(result.node);
                break;
            }
            int gtgt = SHIFT_TARGET(go);
            P.state_stk[P.sp] = gtgt;
            P.val_stk[P.sp] = result;
            P.sp++;
            continue;
        }

        /* ACT_ERROR: syntax error. Report with the offending token. */
        {
            char near[L26_MAX_IDENT + 8];
            if (tk->kind == TOK_EOF)
                snprintf(near, sizeof(near), "end of input");
            else if (tk->text[0])
                snprintf(near, sizeof(near), "'%s'", tk->text);
            else
                snprintf(near, sizeof(near), "%s", token_kind_name(tk->kind));
            diag_add(dl, DIAG_ERROR, DIAG_PHASE_PARSE, tk->pos,
                     "syntax error: unexpected %s", near);
            P.error = 1;
        }
        break;
    }

    cleanup_stack(&P);
    return NULL;
}

int parser_build_tables_report(DiagList *dl) {
    if (ensure_tables() != 0) {
        diag_add(dl, DIAG_ERROR, DIAG_PHASE_PARSE, srcpos_make(1,1,0),
                 "internal: failed to build parse tables");
        return -1;
    }
    if (g_T->dangling_else > 0) {
        diag_add(dl, DIAG_NOTE, DIAG_PHASE_PARSE, srcpos_make(1,1,0),
                 "dangling-else resolved by preferring shift (%d state(s))",
                 g_T->dangling_else);
    }
    /* Return only UNEXPECTED conflicts: the intentional dangling-else
     * shift-on-else is the single accepted ambiguity, so a clean grammar
     * reports 0 here. */
    return g_T->conflicts - g_T->dangling_else;
}
