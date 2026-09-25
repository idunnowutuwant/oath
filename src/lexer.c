#include "oath/lexer.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

OathLexer oath_lexer_create(const char* src) {
    OathLexer l = { .src = src, .cursor = 0, .line = 1, .col = 1 };
    return l;
}

static char peek_char(const OathLexer* l) {
    return l->src[l->cursor];
}

static char advance_char(OathLexer* l) {
    char c = l->src[l->cursor++];
    if (c == '\n') {
        l->line++;
        l->col = 1;
    } else {
        l->col++;
    }
    return c;
}

static void skip_whitespace_and_comments(OathLexer* l) {
    while (peek_char(l) != '\0') {
        char c = peek_char(l);
        if (isspace((unsigned char)c)) {
            advance_char(l);
        } else if (c == '/' && l->src[l->cursor + 1] == '/') {
            while (peek_char(l) != '\0' && peek_char(l) != '\n') {
                advance_char(l);
            }
        } else {
            break;
        }
    }
}

OathToken oath_lexer_next(OathLexer* l) {
    skip_whitespace_and_comments(l);

    OathToken tok;
    memset(&tok, 0, sizeof(tok));
    tok.line = l->line;
    tok.col = l->col;

    char c = peek_char(l);
    if (c == '\0') {
        tok.type = TOK_EOF;
        return tok;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        size_t len = 0;
        while (isalnum((unsigned char)peek_char(l)) || peek_char(l) == '_') {
            if (len < sizeof(tok.text) - 1) tok.text[len++] = advance_char(l);
            else advance_char(l);
        }
        tok.text[len] = '\0';

        if (strcmp(tok.text, "fn") == 0) tok.type = TOK_FN;
        else if (strcmp(tok.text, "extern") == 0) tok.type = TOK_EXTERN;
        else if (strcmp(tok.text, "struct") == 0) tok.type = TOK_STRUCT;
        else if (strcmp(tok.text, "enum") == 0) tok.type = TOK_ENUM;
        else if (strcmp(tok.text, "let") == 0) tok.type = TOK_LET;
        else if (strcmp(tok.text, "alloc") == 0) tok.type = TOK_ALLOC;
        else if (strcmp(tok.text, "free") == 0) tok.type = TOK_FREE;
        else if (strcmp(tok.text, "match") == 0) tok.type = TOK_MATCH;
        else if (strcmp(tok.text, "if") == 0) tok.type = TOK_IF;
        else if (strcmp(tok.text, "else") == 0) tok.type = TOK_ELSE;
        else if (strcmp(tok.text, "while") == 0) tok.type = TOK_WHILE;
        else if (strcmp(tok.text, "invariant") == 0) tok.type = TOK_INVARIANT;
        else if (strcmp(tok.text, "requires") == 0) tok.type = TOK_REQUIRES;
        else if (strcmp(tok.text, "disjoint") == 0) tok.type = TOK_DISJOINT;
        else if (strcmp(tok.text, "ensures") == 0) tok.type = TOK_ENSURES;
        else if (strcmp(tok.text, "return") == 0) tok.type = TOK_RETURN;
        else tok.type = TOK_IDENT;
        return tok;
    }

    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)l->src[l->cursor + 1]))) {
        size_t len = 0;
        if (c == '-') tok.text[len++] = advance_char(l);
        while (isdigit((unsigned char)peek_char(l))) {
            if (len < sizeof(tok.text) - 1) tok.text[len++] = advance_char(l);
            else advance_char(l);
        }
        tok.text[len] = '\0';
        tok.type = TOK_INT_LITERAL;
        tok.int_val = (int64_t)strtoll(tok.text, NULL, 10);
        return tok;
    }

    advance_char(l);
    tok.text[0] = c;
    tok.text[1] = '\0';

    switch (c) {
        case '.': tok.type = TOK_DOT; return tok;
        case '(': tok.type = TOK_LPAREN; return tok;
        case ')': tok.type = TOK_RPAREN; return tok;
        case '{': tok.type = TOK_LBRACE; return tok;
        case '}': tok.type = TOK_RBRACE; return tok;
        case '[': tok.type = TOK_LBRACKET; return tok;
        case ']': tok.type = TOK_RBRACKET; return tok;
        case ',': tok.type = TOK_COMMA; return tok;
        case ';': tok.type = TOK_SEMI; return tok;
        case '+': tok.type = TOK_PLUS; return tok;
        case '*': tok.type = TOK_STAR; return tok;
        case '/': tok.type = TOK_SLASH; return tok;
        case ':':
            if (peek_char(l) == ':') {
                advance_char(l);
                tok.text[1] = ':';
                tok.text[2] = '\0';
                tok.type = TOK_COLON_COLON;
            } else {
                tok.type = TOK_COLON;
            }
            return tok;
        case '&':
            if (peek_char(l) == '&') {
                advance_char(l);
                tok.text[1] = '&';
                tok.text[2] = '\0';
                tok.type = TOK_AND;
                return tok;
            }
            break;
        case '-':
            if (peek_char(l) == '>') {
                advance_char(l);
                tok.text[1] = '>';
                tok.text[2] = '\0';
                tok.type = TOK_ARROW;
            } else {
                tok.type = TOK_MINUS;
            }
            return tok;
        case '=':
            if (peek_char(l) == '>') {
                advance_char(l);
                tok.text[1] = '>';
                tok.text[2] = '\0';
                tok.type = TOK_FAT_ARROW;
            } else if (peek_char(l) == '=') {
                advance_char(l);
                tok.text[1] = '=';
                tok.text[2] = '\0';
                tok.type = TOK_EQ;
            } else {
                tok.type = TOK_ASSIGN;
            }
            return tok;
        case '!':
            if (peek_char(l) == '=') {
                advance_char(l);
                tok.text[1] = '=';
                tok.text[2] = '\0';
                tok.type = TOK_NE;
            }
            break;
        case '<':
            if (peek_char(l) == '=') {
                advance_char(l);
                tok.text[1] = '=';
                tok.text[2] = '\0';
                tok.type = TOK_LE;
            } else {
                tok.type = TOK_LT;
            }
            return tok;
        case '>':
            if (peek_char(l) == '=') {
                advance_char(l);
                tok.text[1] = '=';
                tok.text[2] = '\0';
                tok.type = TOK_GE;
            } else {
                tok.type = TOK_GT;
            }
            return tok;
    }

    return tok;
}