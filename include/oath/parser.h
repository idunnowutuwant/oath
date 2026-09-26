#ifndef OATH_PARSER_H
#define OATH_PARSER_H

#include "oath/ast.h"
#include "oath/arena.h"
#include "oath/lexer.h"
#include "oath/diagnostic.h"

typedef struct {
    OathLexer lexer;
    OathToken current;
    OathArena* arena;
    OathDiagContext* diag;
    bool has_error;
    char error_msg[128];
} OathParser;

OathParser oath_parser_create(const char* src, OathArena* arena, OathDiagContext* diag);
OathModule oath_parser_parse_module(OathParser* p);

#endif