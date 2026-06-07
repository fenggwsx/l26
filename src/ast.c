/* ast.c - AST node constructors, free, and pretty-printer.
 *
 * Fully implemented (not a stub). Heap-allocated nodes; ast_free walks the
 * kids vector and the union's owned Node* fields. The pretty-printer emits an
 * indented s-expression for debugging and for the visualizer's AST view.
 */
#include "ast.h"

#include <stdlib.h>
#include <string.h>

const char *op_name(OpCode op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_DIV: return "/";
        case OP_AND: return "&&";
        case OP_OR:  return "||";
        case OP_LT:  return "<";
        case OP_LE:  return "<=";
        case OP_GT:  return ">";
        case OP_GE:  return ">=";
        case OP_EQ:  return "==";
        case OP_NE:  return "!=";
        default:     return "?";
    }
}

/* ---- allocation helpers ---- */

static Node *node_new(NodeKind kind, SrcPos pos) {
    Node *n = (Node *)calloc(1, sizeof(Node));
    if (!n) return NULL;
    n->kind = kind;
    n->pos = pos;
    n->vtype = TYPE_ERROR;
    n->kids = NULL;
    n->nkids = 0;
    n->kidcap = 0;
    return n;
}

static void node_push_kid(Node *n, Node *kid) {
    if (!n) return;
    if (n->nkids >= n->kidcap) {
        int newcap = n->kidcap ? n->kidcap * 2 : 4;
        Node **k = (Node **)realloc(n->kids, (size_t)newcap * sizeof(Node *));
        if (!k) return; /* leak-free best effort; OOM treated as fatal upstream */
        n->kids = k;
        n->kidcap = newcap;
    }
    n->kids[n->nkids++] = kid;
}

static void copy_ident(char *dst, const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, L26_MAX_IDENT - 1);
    dst[L26_MAX_IDENT - 1] = '\0';
}

/* ---- constructors ---- */

Node *ast_block(SrcPos pos) { return node_new(N_BLOCK, pos); }

void ast_block_add(Node *block, Node *child) { node_push_kid(block, child); }

Node *ast_decl(SrcPos pos, ValueType type, const char *name) {
    Node *n = node_new(N_DECL, pos);
    if (!n) return NULL;
    n->as.decl.type = type;
    copy_ident(n->as.decl.name, name);
    n->as.decl.sym = -1;
    return n;
}

Node *ast_assign(SrcPos pos, const char *name, Node *rhs) {
    Node *n = node_new(N_ASSIGN, pos);
    if (!n) return NULL;
    copy_ident(n->as.assign.name, name);
    n->as.assign.sym = -1;
    n->as.assign.rhs = rhs;
    return n;
}

Node *ast_if(SrcPos pos, Node *cond, Node *then_s, Node *else_s) {
    Node *n = node_new(N_IF, pos);
    if (!n) return NULL;
    n->as.if_.cond = cond;
    n->as.if_.then_s = then_s;
    n->as.if_.else_s = else_s;
    return n;
}

Node *ast_while(SrcPos pos, Node *cond, Node *body) {
    Node *n = node_new(N_WHILE, pos);
    if (!n) return NULL;
    n->as.while_.cond = cond;
    n->as.while_.body = body;
    return n;
}

Node *ast_write(SrcPos pos, Node *expr) {
    Node *n = node_new(N_WRITE, pos);
    if (!n) return NULL;
    node_push_kid(n, expr);
    return n;
}

Node *ast_read(SrcPos pos, const char *name) {
    Node *n = node_new(N_READ, pos);
    if (!n) return NULL;
    copy_ident(n->as.read.name, name);
    n->as.read.sym = -1;
    return n;
}

Node *ast_setop(SrcPos pos, NodeKind kind, const char *name, Node *elem) {
    Node *n = node_new(kind, pos);   /* N_ADD or N_REMOVE */
    if (!n) return NULL;
    copy_ident(n->as.setop.name, name);
    n->as.setop.sym = -1;
    n->as.setop.elem = elem;
    return n;
}

Node *ast_num(SrcPos pos, long value) {
    Node *n = node_new(N_NUM, pos);
    if (!n) return NULL;
    n->as.num.value = value;
    n->vtype = TYPE_INT;
    return n;
}

Node *ast_var(SrcPos pos, const char *name) {
    Node *n = node_new(N_VAR, pos);
    if (!n) return NULL;
    copy_ident(n->as.var.name, name);
    n->as.var.sym = -1;
    return n;
}

Node *ast_binop(SrcPos pos, OpCode op, Node *lhs, Node *rhs) {
    Node *n = node_new(N_BINOP, pos);
    if (!n) return NULL;
    n->as.binop.op = op;
    n->as.binop.lhs = lhs;
    n->as.binop.rhs = rhs;
    return n;
}

Node *ast_neg(SrcPos pos, Node *operand) {
    Node *n = node_new(N_NEG, pos);
    if (!n) return NULL;
    node_push_kid(n, operand);
    return n;
}

Node *ast_boollit(SrcPos pos, int value) {
    Node *n = node_new(N_BOOLLIT, pos);
    if (!n) return NULL;
    n->as.boollit.value = value ? 1 : 0;
    n->vtype = TYPE_BOOL;
    return n;
}

Node *ast_not(SrcPos pos, Node *operand) {
    Node *n = node_new(N_NOT, pos);
    if (!n) return NULL;
    node_push_kid(n, operand);
    return n;
}

Node *ast_logic(SrcPos pos, OpCode op, Node *lhs, Node *rhs) {
    Node *n = node_new(N_LOGIC, pos);
    if (!n) return NULL;
    n->as.binop.op = op;
    n->as.binop.lhs = lhs;
    n->as.binop.rhs = rhs;
    return n;
}

Node *ast_rel(SrcPos pos, OpCode op, Node *lhs, Node *rhs) {
    Node *n = node_new(N_REL, pos);
    if (!n) return NULL;
    n->as.binop.op = op;
    n->as.binop.lhs = lhs;
    n->as.binop.rhs = rhs;
    return n;
}

Node *ast_setlit(SrcPos pos) { return node_new(N_SETLIT, pos); }

void ast_setlit_add(Node *setlit, Node *elem) { node_push_kid(setlit, elem); }

Node *ast_setbin(SrcPos pos, OpCode op, const char *lname, const char *rname) {
    Node *n = node_new(N_SETBINOP, pos);
    if (!n) return NULL;
    n->as.setbin.op = op;
    copy_ident(n->as.setbin.lname, lname);
    copy_ident(n->as.setbin.rname, rname);
    n->as.setbin.lsym = -1;
    n->as.setbin.rsym = -1;
    n->vtype = TYPE_SET;
    return n;
}

Node *ast_seteq(SrcPos pos, OpCode op, Node *lhs, Node *rhs) {
    Node *n = node_new(N_SETEQ, pos);
    if (!n) return NULL;
    n->as.binop.op = op;     /* OP_EQ or OP_NE */
    n->as.binop.lhs = lhs;
    n->as.binop.rhs = rhs;
    n->vtype = TYPE_BOOL;
    return n;
}

Node *ast_setcomp(SrcPos pos, Node *gen, const char *var,
                  const char *src, Node *filter) {
    Node *n = node_new(N_SETCOMP, pos);
    if (!n) return NULL;
    n->as.comp.gen = gen;
    copy_ident(n->as.comp.var, var);
    copy_ident(n->as.comp.src, src);
    n->as.comp.filter = filter;
    n->as.comp.src_sym = -1;
    n->as.comp.compvar_sym = -1;
    n->vtype = TYPE_SET;
    return n;
}

Node *ast_in(SrcPos pos, Node *elem, const char *setname) {
    Node *n = node_new(N_IN, pos);
    if (!n) return NULL;
    n->as.intest.elem = elem;
    copy_ident(n->as.intest.name, setname);
    n->as.intest.sym = -1;
    n->vtype = TYPE_BOOL;
    return n;
}

Node *ast_isempty(SrcPos pos, const char *setname) {
    Node *n = node_new(N_ISEMPTY, pos);
    if (!n) return NULL;
    n->as.intest.elem = NULL;
    copy_ident(n->as.intest.name, setname);
    n->as.intest.sym = -1;
    n->vtype = TYPE_BOOL;
    return n;
}

/* ---- free ---- */

void ast_free(Node *n) {
    if (!n) return;

    /* free children vector */
    for (int i = 0; i < n->nkids; ++i)
        ast_free(n->kids[i]);
    free(n->kids);

    /* free union-owned subtrees */
    switch (n->kind) {
        case N_ASSIGN:  ast_free(n->as.assign.rhs); break;
        case N_IF:      ast_free(n->as.if_.cond);
                        ast_free(n->as.if_.then_s);
                        ast_free(n->as.if_.else_s); break;
        case N_WHILE:   ast_free(n->as.while_.cond);
                        ast_free(n->as.while_.body); break;
        case N_ADD:
        case N_REMOVE:  ast_free(n->as.setop.elem); break;
        case N_BINOP:
        case N_LOGIC:
        case N_REL:
        case N_SETEQ:   ast_free(n->as.binop.lhs);
                        ast_free(n->as.binop.rhs); break;
        case N_IN:
        case N_ISEMPTY: ast_free(n->as.intest.elem); break;
        case N_SETCOMP: ast_free(n->as.comp.gen);
                        ast_free(n->as.comp.filter); break;
        default: break; /* N_NEG/N_NOT/N_WRITE own via kids[]; others scalar */
    }
    free(n);
}

/* ---- pretty-printer ---- */

static void indent(FILE *out, int depth) {
    for (int i = 0; i < depth; ++i) fputs("  ", out);
}

static void print_rec(const Node *n, FILE *out, int depth) {
    if (!n) { indent(out, depth); fputs("(null)\n", out); return; }
    indent(out, depth);
    switch (n->kind) {
        case N_BLOCK:
            fprintf(out, "(block :%s\n", value_type_name(n->vtype));
            for (int i = 0; i < n->nkids; ++i) print_rec(n->kids[i], out, depth + 1);
            indent(out, depth); fputs(")\n", out); break;
        case N_DECL:
            fprintf(out, "(decl %s %s @%d)\n",
                    value_type_name(n->as.decl.type), n->as.decl.name,
                    n->as.decl.sym); break;
        case N_ASSIGN:
            fprintf(out, "(assign %s @%d\n", n->as.assign.name, n->as.assign.sym);
            print_rec(n->as.assign.rhs, out, depth + 1);
            indent(out, depth); fputs(")\n", out); break;
        case N_IF:
            fputs("(if\n", out);
            print_rec(n->as.if_.cond, out, depth + 1);
            print_rec(n->as.if_.then_s, out, depth + 1);
            if (n->as.if_.else_s) print_rec(n->as.if_.else_s, out, depth + 1);
            indent(out, depth); fputs(")\n", out); break;
        case N_WHILE:
            fputs("(while\n", out);
            print_rec(n->as.while_.cond, out, depth + 1);
            print_rec(n->as.while_.body, out, depth + 1);
            indent(out, depth); fputs(")\n", out); break;
        case N_WRITE:
            fputs("(write\n", out);
            for (int i = 0; i < n->nkids; ++i) print_rec(n->kids[i], out, depth + 1);
            indent(out, depth); fputs(")\n", out); break;
        case N_READ:
            fprintf(out, "(read %s @%d)\n", n->as.read.name, n->as.read.sym); break;
        case N_ADD:
        case N_REMOVE:
            fprintf(out, "(%s %s @%d\n", n->kind == N_ADD ? "add" : "remove",
                    n->as.setop.name, n->as.setop.sym);
            print_rec(n->as.setop.elem, out, depth + 1);
            indent(out, depth); fputs(")\n", out); break;
        case N_NUM:
            fprintf(out, "(num %ld)\n", n->as.num.value); break;
        case N_VAR:
            fprintf(out, "(var %s @%d :%s)\n", n->as.var.name, n->as.var.sym,
                    value_type_name(n->vtype)); break;
        case N_BINOP:
        case N_LOGIC:
        case N_REL:
        case N_SETEQ:
            fprintf(out, "(%s :%s\n", op_name(n->as.binop.op),
                    value_type_name(n->vtype));
            print_rec(n->as.binop.lhs, out, depth + 1);
            print_rec(n->as.binop.rhs, out, depth + 1);
            indent(out, depth); fputs(")\n", out); break;
        case N_NEG:
            fputs("(neg\n", out);
            for (int i = 0; i < n->nkids; ++i) print_rec(n->kids[i], out, depth + 1);
            indent(out, depth); fputs(")\n", out); break;
        case N_NOT:
            fputs("(not\n", out);
            for (int i = 0; i < n->nkids; ++i) print_rec(n->kids[i], out, depth + 1);
            indent(out, depth); fputs(")\n", out); break;
        case N_BOOLLIT:
            fprintf(out, "(bool %s)\n", n->as.boollit.value ? "true" : "false"); break;
        case N_SETLIT:
            fputs("(setlit\n", out);
            for (int i = 0; i < n->nkids; ++i) print_rec(n->kids[i], out, depth + 1);
            indent(out, depth); fputs(")\n", out); break;
        case N_SETBINOP:
            fprintf(out, "(%s %s %s @%d @%d)\n",
                    n->as.setbin.op == AST_SETBIN_UNION ? "union" : "inter",
                    n->as.setbin.lname, n->as.setbin.rname,
                    n->as.setbin.lsym, n->as.setbin.rsym); break;
        case N_SETCOMP:
            fprintf(out, "(comp var=%s src=%s @%d compvar@%d\n",
                    n->as.comp.var, n->as.comp.src,
                    n->as.comp.src_sym, n->as.comp.compvar_sym);
            indent(out, depth + 1); fputs(":gen\n", out);
            print_rec(n->as.comp.gen, out, depth + 2);
            if (n->as.comp.filter) {
                indent(out, depth + 1); fputs(":if\n", out);
                print_rec(n->as.comp.filter, out, depth + 2);
            }
            indent(out, depth); fputs(")\n", out); break;
        case N_IN:
            fprintf(out, "(in %s @%d\n", n->as.intest.name, n->as.intest.sym);
            print_rec(n->as.intest.elem, out, depth + 1);
            indent(out, depth); fputs(")\n", out); break;
        case N_ISEMPTY:
            fprintf(out, "(isempty %s @%d)\n", n->as.intest.name,
                    n->as.intest.sym); break;
        default:
            fprintf(out, "(?kind=%d)\n", (int)n->kind); break;
    }
}

void ast_print(const Node *n, FILE *out) { print_rec(n, out, 0); }
