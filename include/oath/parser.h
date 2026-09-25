#ifndef OATH_PARSER_H
#define OATH_PARSER_H

#include "oath/ast.h"
#include "oath/arena.h"
#include "oath/lexer.h"

typedef struct {
    OathLexer lexer;
    OathToken current;
    OathArena* arena;
    bool has_error;
    char error_msg[128];
} OathParser;

OathParser oath_parser_create(const char* src, OathArena* arena);
OathModule oath_parser_parse_module(OathParser* p);

#endif