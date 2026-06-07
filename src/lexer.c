/* lexer.c - L26 hand-written scanner.
 *
 * Recognises: identifiers, integer literals, the keyword set
 * {int,bool,set,if,else,while,write,read,add,remove,true,false,union,inter,
 * in,isempty}, multi-char operators (<=,>=,==,!=,&&,||) before single-char
 * ones, punctuation { } ( ) ; , | =, arithmetic + - * /, and // line comments
 * (used in AGENTS.md example 2). Fills `out->tokens`, always appends a trailing
 * TOK_EOF, and emits diagnostics for stray characters via diag_add.
 *
 * Pure C99, no I/O.
 */
#include "lexer.h"

#include <string.h>
#include <stdlib.h>
#include <limits.h>

const char *token_kind_name(TokenKind k) {
    switch (k) {
        case TOK_EOF:     return "end of input";
        case TOK_ID:      return "identifier";
        case TOK_NUM:     return "number";
        case TOK_INT:     return "int";
        case TOK_BOOL:    return "bool";
        case TOK_SET:     return "set";
        case TOK_IF:      return "if";
        case TOK_ELSE:    return "else";
        case TOK_WHILE:   return "while";
        case TOK_WRITE:   return "write";
        case TOK_READ:    return "read";
        case TOK_ADD:     return "add";
        case TOK_REMOVE:  return "remove";
        case TOK_TRUE:    return "true";
        case TOK_FALSE:   return "false";
        case TOK_UNION:   return "union";
        case TOK_INTER:   return "inter";
        case TOK_IN:      return "in";
        case TOK_ISEMPTY: return "isempty";
        case TOK_LBRACE:  return "{";
        case TOK_RBRACE:  return "}";
        case TOK_LPAREN:  return "(";
        case TOK_RPAREN:  return ")";
        case TOK_SEMI:    return ";";
        case TOK_COMMA:   return ",";
        case TOK_PIPE:    return "|";
        case TOK_ASSIGN:  return "=";
        case TOK_PLUS:    return "+";
        case TOK_MINUS:   return "-";
        case TOK_STAR:    return "*";
        case TOK_SLASH:   return "/";
        case TOK_LT:      return "<";
        case TOK_LE:      return "<=";
        case TOK_GT:      return ">";
        case TOK_GE:      return ">=";
        case TOK_EQ:      return "==";
        case TOK_NE:      return "!=";
        case TOK_AND:     return "&&";
        case TOK_OR:      return "||";
        case TOK_NOT:     return "!";
        case TOK_ERROR:   return "<error>";
        default:          return "<?>";
    }
}

/* ------------------------------------------------------------------ */
/* Character classification (locale-independent, ASCII).               */
/* ------------------------------------------------------------------ */

static int is_space_ch(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\v' || c == '\f';
}

static int is_digit_ch(int c) {
    return c >= '0' && c <= '9';
}

static int is_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_ident_cont(int c) {
    return is_ident_start(c) || is_digit_ch(c);
}

/* ------------------------------------------------------------------ */
/* Keyword lookup. Returns the keyword's TokenKind, or TOK_ID if `s`   */
/* is an ordinary identifier.                                          */
/* ------------------------------------------------------------------ */

static TokenKind keyword_kind(const char *s) {
    static const struct { const char *kw; TokenKind kind; } table[] = {
        { "int",     TOK_INT     },
        { "bool",    TOK_BOOL    },
        { "set",     TOK_SET     },
        { "if",      TOK_IF      },
        { "else",    TOK_ELSE    },
        { "while",   TOK_WHILE   },
        { "write",   TOK_WRITE   },
        { "read",    TOK_READ    },
        { "add",     TOK_ADD     },
        { "remove",  TOK_REMOVE  },
        { "true",    TOK_TRUE    },
        { "false",   TOK_FALSE   },
        { "union",   TOK_UNION   },
        { "inter",   TOK_INTER   },
        { "in",      TOK_IN      },
        { "isempty", TOK_ISEMPTY },
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (strcmp(s, table[i].kw) == 0) {
            return table[i].kind;
        }
    }
    return TOK_ID;
}

/* ------------------------------------------------------------------ */
/* Scanner state.                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *src;
    int   pos;     /* 0-based byte offset of the next char to read */
    int   line;    /* 1-based line of the next char                */
    int   col;     /* 1-based column of the next char              */
} Scanner;

static int sc_peek(const Scanner *s) {
    /* Treat the byte as unsigned so high-bit bytes don't sign-extend. */
    return (unsigned char)s->src[s->pos];
}

static int sc_peek2(const Scanner *s) {
    if (s->src[s->pos] == '\0') {
        return '\0';
    }
    return (unsigned char)s->src[s->pos + 1];
}

static SrcPos sc_here(const Scanner *s) {
    return srcpos_make(s->line, s->col, s->pos);
}

/* Advance one character, maintaining line/column. Returns the char read. */
static int sc_advance(Scanner *s) {
    int c = (unsigned char)s->src[s->pos];
    if (c == '\0') {
        return '\0';
    }
    s->pos++;
    if (c == '\n') {
        s->line++;
        s->col = 1;
    } else {
        s->col++;
    }
    return c;
}

/* Append a token to the stream if there is room. Returns 1 on success,
 * 0 if the stream is full (leaving the trailing EOF slot reserved). */
static int emit(TokenStream *out, TokenKind kind, SrcPos pos,
                const char *text, int textlen, long ival) {
    Token *t;
    /* Always keep one slot in reserve for the trailing TOK_EOF. */
    if (out->count >= L26_MAX_TOKENS - 1) {
        return 0;
    }
    t = &out->tokens[out->count++];
    t->kind = kind;
    t->pos = pos;
    t->ival = ival;
    if (text != NULL && textlen > 0) {
        int n = textlen;
        if (n > L26_MAX_IDENT - 1) {
            n = L26_MAX_IDENT - 1;
        }
        memcpy(t->text, text, (size_t)n);
        t->text[n] = '\0';
    } else {
        t->text[0] = '\0';
    }
    return 1;
}

int lexer_scan(const char *src, TokenStream *out, DiagList *dl) {
    Scanner s;
    Token eof;

    s.src = (src != NULL) ? src : "";
    s.pos = 0;
    s.line = 1;
    s.col = 1;

    out->count = 0;

    for (;;) {
        int c = sc_peek(&s);

        /* End of source. */
        if (c == '\0') {
            break;
        }

        /* Whitespace. */
        if (is_space_ch(c)) {
            sc_advance(&s);
            continue;
        }

        /* // line comment: skip to end of line (or end of input). */
        if (c == '/' && sc_peek2(&s) == '/') {
            while (sc_peek(&s) != '\0' && sc_peek(&s) != '\n') {
                sc_advance(&s);
            }
            continue;
        }

        /* Identifier or keyword. */
        if (is_ident_start(c)) {
            SrcPos start = sc_here(&s);
            int begin = s.pos;
            char buf[L26_MAX_IDENT];
            int len;
            TokenKind kind;
            while (is_ident_cont(sc_peek(&s))) {
                sc_advance(&s);
            }
            len = s.pos - begin;
            {
                int n = len;
                if (n > L26_MAX_IDENT - 1) {
                    n = L26_MAX_IDENT - 1;
                }
                memcpy(buf, s.src + begin, (size_t)n);
                buf[n] = '\0';
            }
            kind = keyword_kind(buf);
            emit(out, kind, start, buf, len, 0);
            continue;
        }

        /* Integer literal. */
        if (is_digit_ch(c)) {
            SrcPos start = sc_here(&s);
            int begin = s.pos;
            int len;
            long val = 0;
            int overflow = 0;
            char buf[L26_MAX_IDENT];
            while (is_digit_ch(sc_peek(&s))) {
                int d = sc_advance(&s) - '0';
                if (!overflow) {
                    if (val > (LONG_MAX - d) / 10) {
                        overflow = 1;
                    } else {
                        val = val * 10 + d;
                    }
                }
            }
            len = s.pos - begin;
            {
                int n = len;
                if (n > L26_MAX_IDENT - 1) {
                    n = L26_MAX_IDENT - 1;
                }
                memcpy(buf, s.src + begin, (size_t)n);
                buf[n] = '\0';
            }
            if (overflow) {
                diag_add(dl, DIAG_WARNING, DIAG_PHASE_LEX, start,
                         "integer literal '%s' overflows; clamped to %ld",
                         buf, LONG_MAX);
                val = LONG_MAX;
            }
            emit(out, TOK_NUM, start, buf, len, val);
            continue;
        }

        /* Operators and punctuation. Multi-char forms first. */
        {
            SrcPos start = sc_here(&s);
            int c2 = sc_peek2(&s);

            /* Two-character operators. */
            if (c == '<' && c2 == '=') {
                sc_advance(&s); sc_advance(&s);
                emit(out, TOK_LE, start, "<=", 2, 0);
                continue;
            }
            if (c == '>' && c2 == '=') {
                sc_advance(&s); sc_advance(&s);
                emit(out, TOK_GE, start, ">=", 2, 0);
                continue;
            }
            if (c == '=' && c2 == '=') {
                sc_advance(&s); sc_advance(&s);
                emit(out, TOK_EQ, start, "==", 2, 0);
                continue;
            }
            if (c == '!' && c2 == '=') {
                sc_advance(&s); sc_advance(&s);
                emit(out, TOK_NE, start, "!=", 2, 0);
                continue;
            }
            if (c == '&' && c2 == '&') {
                sc_advance(&s); sc_advance(&s);
                emit(out, TOK_AND, start, "&&", 2, 0);
                continue;
            }
            if (c == '|' && c2 == '|') {
                sc_advance(&s); sc_advance(&s);
                emit(out, TOK_OR, start, "||", 2, 0);
                continue;
            }

            /* Single-character operators and punctuation. */
            sc_advance(&s);
            switch (c) {
                case '{': emit(out, TOK_LBRACE, start, "{", 1, 0); continue;
                case '}': emit(out, TOK_RBRACE, start, "}", 1, 0); continue;
                case '(': emit(out, TOK_LPAREN, start, "(", 1, 0); continue;
                case ')': emit(out, TOK_RPAREN, start, ")", 1, 0); continue;
                case ';': emit(out, TOK_SEMI,   start, ";", 1, 0); continue;
                case ',': emit(out, TOK_COMMA,  start, ",", 1, 0); continue;
                case '|': emit(out, TOK_PIPE,   start, "|", 1, 0); continue;
                case '=': emit(out, TOK_ASSIGN, start, "=", 1, 0); continue;
                case '+': emit(out, TOK_PLUS,   start, "+", 1, 0); continue;
                case '-': emit(out, TOK_MINUS,  start, "-", 1, 0); continue;
                case '*': emit(out, TOK_STAR,   start, "*", 1, 0); continue;
                case '/': emit(out, TOK_SLASH,  start, "/", 1, 0); continue;
                case '<': emit(out, TOK_LT,     start, "<", 1, 0); continue;
                case '>': emit(out, TOK_GT,     start, ">", 1, 0); continue;
                case '!': emit(out, TOK_NOT,    start, "!", 1, 0); continue;
                default:
                    /* Stray character: report and keep the stream
                     * well-formed by emitting a TOK_ERROR token. */
                    if (c >= 0x20 && c < 0x7f) {
                        diag_add(dl, DIAG_ERROR, DIAG_PHASE_LEX, start,
                                 "unexpected character '%c'", c);
                    } else {
                        diag_add(dl, DIAG_ERROR, DIAG_PHASE_LEX, start,
                                 "unexpected character (byte 0x%02x)",
                                 (unsigned)c);
                    }
                    {
                        char one[2];
                        one[0] = (char)c;
                        one[1] = '\0';
                        emit(out, TOK_ERROR, start, one, 1, 0);
                    }
                    continue;
            }
        }
    }

    /* Always append the trailing TOK_EOF. The reserved slot guarantees
     * room even if the stream filled up. */
    memset(&eof, 0, sizeof(eof));
    eof.kind = TOK_EOF;
    eof.pos = sc_here(&s);
    out->tokens[out->count++] = eof;
    return out->count;
}
