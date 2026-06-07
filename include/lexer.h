/* lexer.h - token kinds and the scanner public API.
 *
 * The token kinds here double as the TERMINAL symbols of the grammar driven
 * by the LALR(1) parser, so the ordering and completeness matter: parser.c
 * maps TokenKind directly onto grammar terminal ids.
 *
 * Pure C99, no I/O.  The lexer reads a NUL-terminated source buffer and
 * fills a caller-provided token array, reporting errors into a DiagList.
 */
#ifndef L26_LEXER_H
#define L26_LEXER_H

#include "common.h"
#include "diag.h"

/* ------------------------------------------------------------------ */
/* Token kinds.  Keep TOK_EOF last-but-one and TOK__COUNT last.        */
/* The first entry MUST be 0 so arrays indexed by TokenKind are dense. */
/* ------------------------------------------------------------------ */
typedef enum {
    TOK_EOF = 0,     /* end of input (also the grammar's $ terminal)   */

    /* literals & identifiers */
    TOK_ID,          /* identifier                                     */
    TOK_NUM,         /* integer literal                                */

    /* type keywords */
    TOK_INT,         /* int                                            */
    TOK_BOOL,        /* bool                                           */
    TOK_SET,         /* set                                            */

    /* statement / control keywords */
    TOK_IF,          /* if                                             */
    TOK_ELSE,        /* else                                           */
    TOK_WHILE,       /* while                                          */
    TOK_WRITE,       /* write                                          */
    TOK_READ,        /* read                                           */
    TOK_ADD,         /* add                                            */
    TOK_REMOVE,      /* remove                                         */

    /* boolean / set keyword operators */
    TOK_TRUE,        /* true                                           */
    TOK_FALSE,       /* false                                          */
    TOK_UNION,       /* union                                          */
    TOK_INTER,       /* inter                                          */
    TOK_IN,          /* in                                            */
    TOK_ISEMPTY,     /* isempty                                        */

    /* punctuation */
    TOK_LBRACE,      /* {                                              */
    TOK_RBRACE,      /* }                                              */
    TOK_LPAREN,      /* (                                              */
    TOK_RPAREN,      /* )                                              */
    TOK_SEMI,        /* ;                                              */
    TOK_COMMA,       /* ,                                              */
    TOK_PIPE,        /* |   (set comprehension separator, BONUS)       */

    /* assignment */
    TOK_ASSIGN,      /* =                                              */

    /* arithmetic operators */
    TOK_PLUS,        /* +                                              */
    TOK_MINUS,       /* -                                              */
    TOK_STAR,        /* *                                              */
    TOK_SLASH,       /* /                                              */

    /* relational operators */
    TOK_LT,          /* <                                              */
    TOK_LE,          /* <=                                            */
    TOK_GT,          /* >                                              */
    TOK_GE,          /* >=                                            */
    TOK_EQ,          /* ==                                            */
    TOK_NE,          /* !=                                            */

    /* logical operators */
    TOK_AND,         /* &&                                            */
    TOK_OR,          /* ||                                            */
    TOK_NOT,         /* !                                             */

    TOK_ERROR,       /* malformed lexeme (diagnostic already emitted)  */
    TOK__COUNT       /* number of token kinds; NOT a real token        */
} TokenKind;

/* One lexical token. `text` is a copy of the lexeme (NUL-terminated,
 * truncated to L26_MAX_IDENT). `ival` is valid only when kind==TOK_NUM. */
typedef struct {
    TokenKind kind;
    SrcPos    pos;
    char      text[L26_MAX_IDENT];
    long      ival;
} Token;

/* A scanned token stream.  Always terminated by a TOK_EOF token, so
 * consumers may rely on tokens[count-1].kind == TOK_EOF. */
typedef struct {
    Token tokens[L26_MAX_TOKENS];
    int   count;
} TokenStream;

/* Display name for a token kind, e.g. "==" or "identifier". Never NULL.
 * Used by parser error messages and the diagnostics. */
const char *token_kind_name(TokenKind k);

/* Scan `src` (NUL-terminated) into `out`, appending lexical errors to `dl`.
 * Returns the number of tokens produced (including the trailing TOK_EOF).
 * Never performs I/O.  On lexical error it emits a diagnostic and continues,
 * emitting a TOK_ERROR token, so the stream is always well-formed. */
int lexer_scan(const char *src, TokenStream *out, DiagList *dl);

#endif /* L26_LEXER_H */
