#ifndef OATH_LEXER_H
#define OATH_LEXER_H

#include "oath/common.h"

typedef enum {
    TOK_EOF = 0,
    TOK_FN,
    TOK_EXTERN,
    TOK_STRUCT,
    TOK_ENUM,
    TOK_LET,
    TOK_ALLOC,
    TOK_FREE,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_INVARIANT,
    TOK_REQUIRES,
    TOK_DISJOINT,
    TOK_ENSURES,
    TOK_MATCH,
    TOK_FAT_ARROW,
    TOK_RETURN,
    TOK_IDENT,
    TOK_INT_LITERAL,
    TOK_ARROW,
    TOK_DOT,
    TOK_COLON_COLON,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_COLON,
    TOK_SEMI,
    TOK_ASSIGN,
    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_AND,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH
} OathTokenType;

typedef struct {
    OathTokenType type;
    char text[64];
    int64_t int_val;
    size_t line;
    size_t col;
} OathToken;

typedef struct {
    const char* src;
    size_t cursor;
    size_t line;
    size_t col;
} OathLexer;

OathLexer oath_lexer_create(const char* src);
OathToken oath_lexer_next(OathLexer* lexer);

#endif