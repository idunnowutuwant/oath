#include "oath/parser.h"
#include <stdio.h>
#include <string.h>

static void safe_copy_str(char* dst, const char* src, size_t dst_cap) {
    if (dst_cap == 0) return;
    size_t i = 0;
    while (i + 1 < dst_cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void next_token(OathParser* p) {
    p->current = oath_lexer_next(&p->lexer);
}

static bool match_token(OathParser* p, OathTokenType type) {
    if (p->current.type == type) {
        next_token(p);
        return true;
    }
    return false;
}

static bool expect_token(OathParser* p, OathTokenType type, const char* expected_desc) {
    if (p->current.type != type) {
        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "Line %zu, Col %zu: Expected %s, got '%s'",
                 p->current.line, p->current.col, expected_desc, p->current.text);
        return false;
    }
    next_token(p);
    return true;
}

static OathInterval parse_interval_bounds(OathParser* p) {
    if (!expect_token(p, TOK_LBRACKET, "'['")) return oath_interval_create(0, 0);

    bool neg_lo = false;
    if (match_token(p, TOK_MINUS)) neg_lo = true;
    if (p->current.type != TOK_INT_LITERAL) {
        expect_token(p, TOK_INT_LITERAL, "integer lower bound");
        return oath_interval_create(0, 0);
    }
    int64_t lo = p->current.int_val;
    if (neg_lo && lo > 0) lo = -lo;
    next_token(p);

    if (!expect_token(p, TOK_COMMA, "','")) return oath_interval_create(0, 0);

    bool neg_hi = false;
    if (match_token(p, TOK_MINUS)) neg_hi = true;
    if (p->current.type != TOK_INT_LITERAL) {
        expect_token(p, TOK_INT_LITERAL, "integer upper bound");
        return oath_interval_create(0, 0);
    }
    int64_t hi = p->current.int_val;
    if (neg_hi && hi > 0) hi = -hi;
    next_token(p);

    return oath_interval_create(lo, hi);
}

static size_t resolve_variable(const OathFunction* fn, const char* name) {
    for (size_t i = 0; i < fn->param_count; ++i) {
        if (strcmp(fn->params[i].name, name) == 0) return i;
    }
    for (size_t i = 0; i < fn->local_count; ++i) {
        if (strcmp(fn->locals[i].name, name) == 0) return fn->param_count + i;
    }
    return (size_t)-1;
}

static size_t resolve_field(const OathStructDef* s, const char* field_name) {
    for (size_t f = 0; f < s->field_count; ++f) {
        if (strcmp(s->fields[f].name, field_name) == 0) return f;
    }
    return (size_t)-1;
}

static size_t get_param_slot_base(const OathFunction* fn, const OathModule* mod, size_t p_idx) {
    size_t slot = 0;
    for (size_t i = 0; i < p_idx; ++i) {
        if (fn->params[i].kind == PARAM_STRUCT) {
            slot += mod->structs[fn->params[i].struct_def_idx].field_count;
        } else {
            slot += 1;
        }
    }
    return slot;
}

static size_t get_local_slot(const OathFunction* fn, const OathModule* mod, size_t l_idx) {
    size_t base = get_param_slot_base(fn, mod, fn->param_count);
    return base + l_idx;
}

static OathExpr* parse_expr(OathParser* p, const OathFunction* fn, const OathModule* mod);

static OathExpr* parse_primary(OathParser* p, const OathFunction* fn, const OathModule* mod) {
    OathExpr* e = (OathExpr*)oath_arena_alloc(p->arena, sizeof(OathExpr));

    if (p->current.type == TOK_INT_LITERAL) {
        e->type = EXPR_LITERAL;
        e->literal = p->current.int_val;
        next_token(p);
        return e;
    }

    if (match_token(p, TOK_ALLOC)) {
        if (!expect_token(p, TOK_LPAREN, "'('")) return NULL;
        OathExpr* sz = parse_expr(p, fn, mod);
        if (!expect_token(p, TOK_RPAREN, "')'")) return NULL;
        e->type = EXPR_ALLOC;
        e->lhs = sz;
        return e;
    }

    if (p->current.type == TOK_IDENT) {
        char ident_name[64];
        safe_copy_str(ident_name, p->current.text, sizeof(ident_name));
        next_token(p);

        if (match_token(p, TOK_COLON_COLON)) {
            if (p->current.type != TOK_IDENT) {
                expect_token(p, TOK_IDENT, "variant name");
                return NULL;
            }
            char var_name[64];
            safe_copy_str(var_name, p->current.text, sizeof(var_name));
            next_token(p);

            size_t e_idx = (size_t)-1;
            size_t v_idx = (size_t)-1;
            for (size_t s = 0; s < mod->enum_count; ++s) {
                if (strcmp(mod->enums[s].name, ident_name) == 0) {
                    e_idx = s;
                    for (size_t v = 0; v < mod->enums[s].variant_count; ++v) {
                        if (strcmp(mod->enums[s].variants[v].name, var_name) == 0) {
                            v_idx = v;
                            break;
                        }
                    }
                    break;
                }
            }

            if (e_idx == (size_t)-1 || v_idx == (size_t)-1) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg), "Unknown enum variant '%s::%s'", ident_name, var_name);
                return NULL;
            }

            if (!expect_token(p, TOK_LPAREN, "'('")) return NULL;
            OathExpr* payload = parse_expr(p, fn, mod);
            if (!expect_token(p, TOK_RPAREN, "')'")) return NULL;

            e->type = EXPR_ENUM_CONSTRUCT;
            e->enum_construct.enum_idx = e_idx;
            e->enum_construct.variant_idx = v_idx;
            e->enum_construct.payload = payload;
            return e;
        }

        if (match_token(p, TOK_LPAREN)) {
            e->type = EXPR_CALL;
            safe_copy_str(e->call_name, ident_name, sizeof(e->call_name));
            e->arg_count = 0;
            while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
                if (e->arg_count >= OATH_MAX_PARAMS) {
                    p->has_error = true;
                    snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max call arguments");
                    return NULL;
                }
                e->args[e->arg_count++] = parse_expr(p, fn, mod);
                if (!match_token(p, TOK_COMMA)) break;
            }
            if (!expect_token(p, TOK_RPAREN, "')'")) return NULL;
            return e;
        }

        size_t idx = resolve_variable(fn, ident_name);
        if (idx != (size_t)-1) {
            if (match_token(p, TOK_DOT)) {
                if (idx >= fn->param_count || fn->params[idx].kind != PARAM_STRUCT) {
                    p->has_error = true;
                    snprintf(p->error_msg, sizeof(p->error_msg), "Line %zu: '%s' is not a struct", p->current.line, ident_name);
                    return NULL;
                }
                if (p->current.type != TOK_IDENT) {
                    expect_token(p, TOK_IDENT, "field name");
                    return NULL;
                }
                const OathStructDef* s = &mod->structs[fn->params[idx].struct_def_idx];
                size_t f_idx = resolve_field(s, p->current.text);
                if (f_idx == (size_t)-1) {
                    p->has_error = true;
                    snprintf(p->error_msg, sizeof(p->error_msg), "Line %zu: Unknown field '%s'", p->current.line, p->current.text);
                    return NULL;
                }
                e->type = EXPR_FIELD;
                e->var_idx = idx;
                e->field_idx = f_idx;
                safe_copy_str(e->field_name, p->current.text, sizeof(e->field_name));
                next_token(p);
                return e;
            }

            e->type = EXPR_VAR;
            e->var_idx = idx;

            if (match_token(p, TOK_LBRACKET)) {
                OathExpr* idx_expr = parse_expr(p, fn, mod);
                if (!expect_token(p, TOK_RBRACKET, "']'")) return NULL;

                OathExpr* index_node = (OathExpr*)oath_arena_alloc(p->arena, sizeof(OathExpr));
                index_node->type = EXPR_INDEX;
                index_node->lhs = e;
                index_node->rhs = idx_expr;
                return index_node;
            }

            return e;
        }

        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg), "Line %zu: Unknown identifier '%s'", p->current.line, ident_name);
        return NULL;
    }

    if (match_token(p, TOK_LPAREN)) {
        OathExpr* inner = parse_expr(p, fn, mod);
        if (!expect_token(p, TOK_RPAREN, "')'")) return NULL;
        return inner;
    }

    p->has_error = true;
    snprintf(p->error_msg, sizeof(p->error_msg), "Line %zu: Unexpected token in expression", p->current.line);
    return NULL;
}

static OathExpr* parse_multiplicative(OathParser* p, const OathFunction* fn, const OathModule* mod) {
    OathExpr* left = parse_primary(p, fn, mod);
    if (!left) return NULL;

    while (p->current.type == TOK_STAR || p->current.type == TOK_SLASH) {
        OathTokenType op = p->current.type;
        next_token(p);
        OathExpr* right = parse_primary(p, fn, mod);
        if (!right) return NULL;

        OathExpr* binary = (OathExpr*)oath_arena_alloc(p->arena, sizeof(OathExpr));
        binary->type = (op == TOK_STAR) ? EXPR_MUL : EXPR_DIV;
        binary->lhs = left;
        binary->rhs = right;
        left = binary;
    }
    return left;
}

static OathExpr* parse_expr(OathParser* p, const OathFunction* fn, const OathModule* mod) {
    OathExpr* left = parse_multiplicative(p, fn, mod);
    if (!left) return NULL;

    while (p->current.type == TOK_PLUS || p->current.type == TOK_MINUS) {
        OathTokenType op = p->current.type;
        next_token(p);
        OathExpr* right = parse_multiplicative(p, fn, mod);
        if (!right) return NULL;

        OathExpr* binary = (OathExpr*)oath_arena_alloc(p->arena, sizeof(OathExpr));
        binary->type = (op == TOK_PLUS) ? EXPR_ADD : EXPR_SUB;
        binary->lhs = left;
        binary->rhs = right;
        left = binary;
    }
    return left;
}

static OathStmt* parse_statement_list(OathParser* p, OathFunction* fn, const OathModule* mod);

static size_t resolve_target_slot(OathParser* p, const OathFunction* fn, const OathModule* mod, size_t v_idx) {
    if (match_token(p, TOK_DOT)) {
        if (v_idx >= fn->param_count || fn->params[v_idx].kind != PARAM_STRUCT) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Cannot access field on non-struct variable");
            return (size_t)-1;
        }
        if (p->current.type != TOK_IDENT) {
            expect_token(p, TOK_IDENT, "field name");
            return (size_t)-1;
        }
        const OathStructDef* s = &mod->structs[fn->params[v_idx].struct_def_idx];
        size_t f_idx = resolve_field(s, p->current.text);
        if (f_idx == (size_t)-1) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Unknown field '%s'", p->current.text);
            return (size_t)-1;
        }
        next_token(p);
        return get_param_slot_base(fn, mod, v_idx) + f_idx;
    }
    if (v_idx < fn->param_count) return get_param_slot_base(fn, mod, v_idx);
    return get_local_slot(fn, mod, v_idx - fn->param_count);
}

static OathAtomicCond parse_atomic_condition(OathParser* p, const OathFunction* fn, const OathModule* mod) {
    OathAtomicCond cond;
    memset(&cond, 0, sizeof(cond));

    if (p->current.type != TOK_IDENT) {
        expect_token(p, TOK_IDENT, "LHS variable in condition");
        return cond;
    }
    size_t u = resolve_variable(fn, p->current.text);
    if (u == (size_t)-1) {
        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg), "Unknown variable '%s' in condition", p->current.text);
        return cond;
    }
    next_token(p);

    cond.lhs_slot = resolve_target_slot(p, fn, mod, u);
    if (p->has_error) return cond;

    if (match_token(p, TOK_EQ)) cond.op = COND_EQ;
    else if (match_token(p, TOK_NE)) cond.op = COND_NE;
    else if (match_token(p, TOK_LT)) cond.op = COND_LT;
    else if (match_token(p, TOK_LE)) cond.op = COND_LE;
    else if (match_token(p, TOK_GT)) cond.op = COND_GT;
    else if (match_token(p, TOK_GE)) cond.op = COND_GE;
    else {
        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg), "Expected comparison operator");
        return cond;
    }

    if (p->current.type == TOK_IDENT) {
        size_t v = resolve_variable(fn, p->current.text);
        if (v == (size_t)-1) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Unknown RHS variable '%s'", p->current.text);
            return cond;
        }
        next_token(p);
        cond.rhs_is_slot = true;
        cond.rhs.rhs_slot = resolve_target_slot(p, fn, mod, v);
    } else {
        cond.rhs_is_slot = false;
        bool neg = match_token(p, TOK_MINUS);
        if (p->current.type != TOK_INT_LITERAL) {
            expect_token(p, TOK_INT_LITERAL, "integer in condition");
            return cond;
        }
        int64_t target_val = p->current.int_val;
        if (neg && target_val > 0) target_val = -target_val;
        cond.rhs.const_val = target_val;
        next_token(p);
    }

    return cond;
}

static OathCondition parse_condition(OathParser* p, const OathFunction* fn, const OathModule* mod) {
    OathCondition cond;
    cond.count = 0;

    cond.terms[cond.count++] = parse_atomic_condition(p, fn, mod);
    if (p->has_error) return cond;

    while (match_token(p, TOK_AND)) {
        if (cond.count >= OATH_MAX_CONJUNCTS) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max '&&' conjuncts");
            return cond;
        }
        cond.terms[cond.count++] = parse_atomic_condition(p, fn, mod);
        if (p->has_error) return cond;
    }

    return cond;
}

static OathStmt* parse_statement_single(OathParser* p, OathFunction* fn, const OathModule* mod) {
    if (match_token(p, TOK_MATCH)) {
        if (p->current.type != TOK_IDENT) {
            expect_token(p, TOK_IDENT, "target variable in match");
            return NULL;
        }

        size_t v_idx = resolve_variable(fn, p->current.text);
        if (v_idx == (size_t)-1 || v_idx >= fn->param_count || fn->params[v_idx].kind != PARAM_ENUM) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Line %zu: Target '%s' is not an enum parameter", p->current.line, p->current.text);
            return NULL;
        }
        size_t e_idx = fn->params[v_idx].enum_def_idx;
        const OathEnumDef* ed = &mod->enums[e_idx];
        next_token(p);

        if (!expect_token(p, TOK_LBRACE, "'{'")) return NULL;

        OathStmt* s = (OathStmt*)oath_arena_alloc(p->arena, sizeof(OathStmt));
        s->type = STMT_MATCH;
        s->next = NULL;
        s->as.match_stmt.target_var_idx = v_idx;
        s->as.match_stmt.enum_idx = e_idx;
        s->as.match_stmt.arm_count = 0;

        while (p->current.type != TOK_RBRACE && p->current.type != TOK_EOF && !p->has_error) {
            if (s->as.match_stmt.arm_count >= OATH_MAX_MATCH_ARMS) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max match arms");
                return NULL;
            }

            if (p->current.type != TOK_IDENT) {
                expect_token(p, TOK_IDENT, "variant name in match arm");
                return NULL;
            }

            size_t var_idx = (size_t)-1;
            for (size_t v = 0; v < ed->variant_count; ++v) {
                if (strcmp(ed->variants[v].name, p->current.text) == 0) {
                    var_idx = v;
                    break;
                }
            }
            if (var_idx == (size_t)-1) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg), "Unknown variant '%s' in match for enum '%s'", p->current.text, ed->name);
                return NULL;
            }
            next_token(p);

            if (!expect_token(p, TOK_LPAREN, "'('")) return NULL;
            if (p->current.type != TOK_IDENT) {
                expect_token(p, TOK_IDENT, "bound payload variable name");
                return NULL;
            }

            size_t l_idx = fn->local_count++;
            safe_copy_str(fn->locals[l_idx].name, p->current.text, sizeof(fn->locals[l_idx].name));
            next_token(p);
            if (!expect_token(p, TOK_RPAREN, "')'")) return NULL;
            if (!expect_token(p, TOK_FAT_ARROW, "'=>'")) return NULL;

            expect_token(p, TOK_LBRACE, "'{'");
            OathStmt* arm_body = parse_statement_list(p, fn, mod);
            expect_token(p, TOK_RBRACE, "'}'");
            match_token(p, TOK_COMMA);

            size_t ac = s->as.match_stmt.arm_count++;
            s->as.match_stmt.arms[ac].variant_idx = var_idx;
            s->as.match_stmt.arms[ac].bound_var_idx = fn->param_count + l_idx;
            s->as.match_stmt.arms[ac].body = arm_body;
        }

        expect_token(p, TOK_RBRACE, "'}'");
        return s;
    }

    if (match_token(p, TOK_LET)) {
        if (p->current.type != TOK_IDENT) {
            expect_token(p, TOK_IDENT, "variable name after 'let'");
            return NULL;
        }

        if (fn->local_count >= OATH_MAX_LOCALS) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max local variables");
            return NULL;
        }

        size_t l_idx = fn->local_count++;
        safe_copy_str(fn->locals[l_idx].name, p->current.text, sizeof(fn->locals[l_idx].name));
        next_token(p);

        if (!expect_token(p, TOK_ASSIGN, "'='")) return NULL;

        bool is_alloc = (p->current.type == TOK_ALLOC);
        OathExpr* init = parse_expr(p, fn, mod);
        if (!expect_token(p, TOK_SEMI, "';'")) return NULL;

        OathStmt* s = (OathStmt*)oath_arena_alloc(p->arena, sizeof(OathStmt));
        s->type = STMT_LET;
        s->next = NULL;
        s->as.let_stmt.var_idx = fn->param_count + l_idx;
        s->as.let_stmt.init_expr = init;
        s->as.let_stmt.is_linear_resource = is_alloc;
        return s;
    }

    if (match_token(p, TOK_FREE)) {
        if (!expect_token(p, TOK_LPAREN, "'('")) return NULL;
        if (p->current.type != TOK_IDENT) {
            expect_token(p, TOK_IDENT, "variable name in free");
            return NULL;
        }
        size_t v_idx = resolve_variable(fn, p->current.text);
        if (v_idx == (size_t)-1) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Unknown variable '%s' in free", p->current.text);
            return NULL;
        }
        next_token(p);
        if (!expect_token(p, TOK_RPAREN, "')'")) return NULL;
        if (!expect_token(p, TOK_SEMI, "';'")) return NULL;

        OathStmt* s = (OathStmt*)oath_arena_alloc(p->arena, sizeof(OathStmt));
        s->type = STMT_FREE;
        s->next = NULL;
        s->as.free_stmt.var_idx = v_idx;
        return s;
    }

    if (match_token(p, TOK_RETURN)) {
        OathStmt* s = (OathStmt*)oath_arena_alloc(p->arena, sizeof(OathStmt));
        s->type = STMT_RETURN;
        s->next = NULL;
        s->as.ret.expr = parse_expr(p, fn, mod);
        if (!expect_token(p, TOK_SEMI, "';'")) return NULL;
        return s;
    }

    if (p->current.type == TOK_IDENT) {
        char ident_name[64];
        safe_copy_str(ident_name, p->current.text, sizeof(ident_name));
        next_token(p);

        if (match_token(p, TOK_LPAREN)) {
            OathExpr* call_e = (OathExpr*)oath_arena_alloc(p->arena, sizeof(OathExpr));
            call_e->type = EXPR_CALL;
            safe_copy_str(call_e->call_name, ident_name, sizeof(call_e->call_name));
            call_e->arg_count = 0;
            while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
                if (call_e->arg_count >= OATH_MAX_PARAMS) {
                    p->has_error = true;
                    snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max call arguments");
                    return NULL;
                }
                call_e->args[call_e->arg_count++] = parse_expr(p, fn, mod);
                if (!match_token(p, TOK_COMMA)) break;
            }
            if (!expect_token(p, TOK_RPAREN, "')'")) return NULL;
            if (!expect_token(p, TOK_SEMI, "';'")) return NULL;

            OathStmt* s = (OathStmt*)oath_arena_alloc(p->arena, sizeof(OathStmt));
            s->type = STMT_CALL;
            s->next = NULL;
            s->as.call_stmt.expr = call_e;
            return s;
        }

        size_t v_idx = resolve_variable(fn, ident_name);
        if (v_idx == (size_t)-1) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Line %zu: Unknown variable '%s'", p->current.line, ident_name);
            return NULL;
        }

        if (match_token(p, TOK_LBRACKET)) {
            OathExpr* idx_e = parse_expr(p, fn, mod);
            if (!expect_token(p, TOK_RBRACKET, "']'")) return NULL;
            if (!expect_token(p, TOK_ASSIGN, "'='")) return NULL;
            OathExpr* val_e = parse_expr(p, fn, mod);
            if (!expect_token(p, TOK_SEMI, "';'")) return NULL;

            OathStmt* s = (OathStmt*)oath_arena_alloc(p->arena, sizeof(OathStmt));
            s->type = STMT_ARRAY_SET;
            s->next = NULL;
            s->as.array_set.buf_var_idx = v_idx;
            s->as.array_set.idx_expr = idx_e;
            s->as.array_set.val_expr = val_e;
            return s;
        }

        if (match_token(p, TOK_ASSIGN)) {
            OathExpr* val = parse_expr(p, fn, mod);
            if (!expect_token(p, TOK_SEMI, "';'")) return NULL;

            OathStmt* s = (OathStmt*)oath_arena_alloc(p->arena, sizeof(OathStmt));
            s->type = STMT_ASSIGN;
            s->next = NULL;
            s->as.assign_stmt.var_idx = v_idx;
            s->as.assign_stmt.expr = val;
            return s;
        }

        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "Line %zu: Unexpected identifier '%s' at statement position", p->current.line, ident_name);
        return NULL;
    }

    if (match_token(p, TOK_IF)) {
        OathStmt* s = (OathStmt*)oath_arena_alloc(p->arena, sizeof(OathStmt));
        s->type = STMT_IF_ELSE;
        s->next = NULL;

        bool has_paren = match_token(p, TOK_LPAREN);
        s->as.if_else.cond = parse_condition(p, fn, mod);
        if (p->has_error) return NULL;
        if (has_paren) expect_token(p, TOK_RPAREN, "')'");

        expect_token(p, TOK_LBRACE, "'{'");
        s->as.if_else.then_branch = parse_statement_list(p, fn, mod);
        expect_token(p, TOK_RBRACE, "'}'");

        expect_token(p, TOK_ELSE, "'else'");
        expect_token(p, TOK_LBRACE, "'{'");
        s->as.if_else.else_branch = parse_statement_list(p, fn, mod);
        expect_token(p, TOK_RBRACE, "'}'");

        return s;
    }

    if (match_token(p, TOK_WHILE)) {
        OathStmt* s = (OathStmt*)oath_arena_alloc(p->arena, sizeof(OathStmt));
        s->type = STMT_WHILE;
        s->next = NULL;
        s->as.while_loop.invariant_count = 0;

        bool has_paren = match_token(p, TOK_LPAREN);
        s->as.while_loop.cond = parse_condition(p, fn, mod);
        if (p->has_error) return NULL;
        if (has_paren) expect_token(p, TOK_RPAREN, "')'");

        if (match_token(p, TOK_INVARIANT)) {
            if (!expect_token(p, TOK_LPAREN, "'('")) return NULL;

            while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
                if (s->as.while_loop.invariant_count >= OATH_MAX_INVARIANTS) {
                    p->has_error = true;
                    snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max loop invariants");
                    return NULL;
                }

                if (p->current.type != TOK_IDENT) {
                    expect_token(p, TOK_IDENT, "invariant variable name");
                    return NULL;
                }
                size_t inv_v = resolve_variable(fn, p->current.text);
                if (inv_v == (size_t)-1) {
                    p->has_error = true;
                    snprintf(p->error_msg, sizeof(p->error_msg), "Unknown variable '%s' in invariant", p->current.text);
                    return NULL;
                }
                next_token(p);

                if (!expect_token(p, TOK_COLON, "':'")) return NULL;
                OathInterval iv = parse_interval_bounds(p);
                if (!expect_token(p, TOK_RBRACKET, "']'")) return NULL;

                size_t ic = s->as.while_loop.invariant_count++;
                s->as.while_loop.invariants[ic].var_idx = inv_v;
                s->as.while_loop.invariants[ic].bound = iv;

                if (!match_token(p, TOK_COMMA)) break;
            }

            if (!expect_token(p, TOK_RPAREN, "')'")) return NULL;
        }

        expect_token(p, TOK_LBRACE, "'{'");
        s->as.while_loop.body = parse_statement_list(p, fn, mod);
        expect_token(p, TOK_RBRACE, "'}'");

        return s;
    }

    p->has_error = true;
    snprintf(p->error_msg, sizeof(p->error_msg), "Expected statement (let, return, if, while, match, assignment, free, call)");
    return NULL;
}

static OathStmt* parse_statement_list(OathParser* p, OathFunction* fn, const OathModule* mod) {
    OathStmt* head = NULL;
    OathStmt* tail = NULL;

    while (p->current.type != TOK_RBRACE && p->current.type != TOK_EOF && !p->has_error) {
        OathStmt* s = parse_statement_single(p, fn, mod);
        if (!s) return NULL;

        if (!head) {
            head = s;
            tail = s;
        } else {
            tail->next = s;
            tail = s;
        }
    }
    return head;
}

OathParser oath_parser_create(const char* src, OathArena* arena) {
    OathParser p;
    p.lexer = oath_lexer_create(src);
    p.arena = arena;
    p.has_error = false;
    p.error_msg[0] = '\0';
    next_token(&p);
    return p;
}

static void parse_struct_decl(OathParser* p, OathModule* mod) {
    if (mod->struct_count >= OATH_MAX_STRUCTS) {
        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max struct declarations");
        return;
    }
    OathStructDef* s = &mod->structs[mod->struct_count++];
    memset(s, 0, sizeof(*s));

    next_token(p);

    if (p->current.type != TOK_IDENT) {
        expect_token(p, TOK_IDENT, "struct name");
        return;
    }
    safe_copy_str(s->name, p->current.text, sizeof(s->name));
    next_token(p);

    if (!expect_token(p, TOK_LBRACE, "'{'")) return;

    while (p->current.type != TOK_RBRACE && p->current.type != TOK_EOF) {
        if (s->field_count >= OATH_MAX_FIELDS) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max fields in struct");
            return;
        }
        if (p->current.type != TOK_IDENT) {
            expect_token(p, TOK_IDENT, "field name");
            return;
        }
        safe_copy_str(s->fields[s->field_count].name, p->current.text, sizeof(s->fields[s->field_count].name));
        next_token(p);

        if (!expect_token(p, TOK_COLON, "':'")) return;
        s->fields[s->field_count].bounds = parse_interval_bounds(p);
        if (!expect_token(p, TOK_RBRACKET, "']'")) return;

        s->field_count++;
        if (!match_token(p, TOK_COMMA)) break;
    }

    expect_token(p, TOK_RBRACE, "'}'");
}

static void parse_enum_decl(OathParser* p, OathModule* mod) {
    if (mod->enum_count >= OATH_MAX_ENUMS) {
        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max enum declarations");
        return;
    }
    OathEnumDef* ed = &mod->enums[mod->enum_count++];
    memset(ed, 0, sizeof(*ed));

    next_token(p);

    if (p->current.type != TOK_IDENT) {
        expect_token(p, TOK_IDENT, "enum name");
        return;
    }
    safe_copy_str(ed->name, p->current.text, sizeof(ed->name));
    next_token(p);

    if (!expect_token(p, TOK_LBRACE, "'{'")) return;

    while (p->current.type != TOK_RBRACE && p->current.type != TOK_EOF) {
        if (ed->variant_count >= OATH_MAX_VARIANTS) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max variants in enum");
            return;
        }
        if (p->current.type != TOK_IDENT) {
            expect_token(p, TOK_IDENT, "variant name");
            return;
        }
        safe_copy_str(ed->variants[ed->variant_count].name, p->current.text, sizeof(ed->variants[ed->variant_count].name));
        next_token(p);

        if (!expect_token(p, TOK_COLON, "':'")) return;
        ed->variants[ed->variant_count].bounds = parse_interval_bounds(p);
        if (!expect_token(p, TOK_RBRACKET, "']'")) return;

        ed->variant_count++;
        if (!match_token(p, TOK_COMMA)) break;
    }

    expect_token(p, TOK_RBRACE, "'}'");
}

static void parse_requires_clause(OathParser* p, OathFunction* fn, const OathModule* mod) {
    if (match_token(p, TOK_DISJOINT)) {
        if (!expect_token(p, TOK_LPAREN, "'('")) return;
        if (p->current.type != TOK_IDENT) {
            expect_token(p, TOK_IDENT, "first buffer name");
            return;
        }
        size_t p_a = resolve_variable(fn, p->current.text);
        next_token(p);
        if (!expect_token(p, TOK_COMMA, "','")) return;
        if (p->current.type != TOK_IDENT) {
            expect_token(p, TOK_IDENT, "second buffer name");
            return;
        }
        size_t p_b = resolve_variable(fn, p->current.text);
        next_token(p);
        if (!expect_token(p, TOK_RPAREN, "')'")) return;

        if (fn->disjoint_count < OATH_MAX_DISJOINT_PAIRS) {
            fn->disjoint_contracts[fn->disjoint_count].param_a = p_a;
            fn->disjoint_contracts[fn->disjoint_count].param_b = p_b;
            fn->disjoint_count++;
        }
    } else {
        if (fn->requires_count < OATH_MAX_CONTRACTS) {
            fn->requires_clauses[fn->requires_count++] = parse_condition(p, fn, mod);
        }
    }
}

static void parse_ensures_clause(OathParser* p, OathFunction* fn, const OathModule* mod) {
    (void)mod;
    if (fn->ensures_count >= OATH_MAX_CONTRACTS) {
        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max 'ensures' clauses");
        return;
    }

    if (p->current.type != TOK_RETURN) {
        expect_token(p, TOK_RETURN, "'return' in ensures clause");
        return;
    }
    next_token(p);

    OathCondition cond;
    memset(&cond, 0, sizeof(cond));
    cond.count = 1;
    cond.terms[0].lhs_is_return = true;
    cond.terms[0].lhs_slot = (size_t)-1;

    if (match_token(p, TOK_EQ)) cond.terms[0].op = COND_EQ;
    else if (match_token(p, TOK_NE)) cond.terms[0].op = COND_NE;
    else if (match_token(p, TOK_LT)) cond.terms[0].op = COND_LT;
    else if (match_token(p, TOK_LE)) cond.terms[0].op = COND_LE;
    else if (match_token(p, TOK_GT)) cond.terms[0].op = COND_GT;
    else if (match_token(p, TOK_GE)) cond.terms[0].op = COND_GE;
    else {
        p->has_error = true;
        snprintf(p->error_msg, sizeof(p->error_msg), "Expected comparison operator after 'return'");
        return;
    }

    bool neg = match_token(p, TOK_MINUS);
    if (p->current.type != TOK_INT_LITERAL) {
        expect_token(p, TOK_INT_LITERAL, "integer in ensures");
        return;
    }
    int64_t target_val = p->current.int_val;
    if (neg && target_val > 0) target_val = -target_val;
    cond.terms[0].rhs_is_slot = false;
    cond.terms[0].rhs.const_val = target_val;
    next_token(p);

    fn->ensures_clauses[fn->ensures_count++] = cond;
}

static OathFunction parse_function_internal(OathParser* p, const OathModule* mod, bool is_extern) {
    OathFunction fn;
    memset(&fn, 0, sizeof(fn));
    fn.is_extern = is_extern;

    if (!expect_token(p, TOK_FN, "'fn'")) return fn;

    if (p->current.type != TOK_IDENT) {
        expect_token(p, TOK_IDENT, "function name");
        return fn;
    }
    safe_copy_str(fn.name, p->current.text, sizeof(fn.name));
    next_token(p);

    if (!expect_token(p, TOK_LPAREN, "'('")) return fn;

    while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
        if (fn.param_count >= OATH_MAX_PARAMS) {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max param count");
            return fn;
        }

        if (p->current.type != TOK_IDENT) {
            expect_token(p, TOK_IDENT, "parameter name");
            return fn;
        }
        safe_copy_str(fn.params[fn.param_count].name, p->current.text, sizeof(fn.params[fn.param_count].name));
        next_token(p);

        if (!expect_token(p, TOK_COLON, "':'")) return fn;

        size_t s_idx = (size_t)-1;
        for (size_t s = 0; s < mod->struct_count; ++s) {
            if (strcmp(mod->structs[s].name, p->current.text) == 0) {
                s_idx = s;
                break;
            }
        }

        size_t e_idx = (size_t)-1;
        for (size_t e = 0; e < mod->enum_count; ++e) {
            if (strcmp(mod->enums[e].name, p->current.text) == 0) {
                e_idx = e;
                break;
            }
        }

        if (s_idx != (size_t)-1) {
            fn.params[fn.param_count].kind = PARAM_STRUCT;
            fn.params[fn.param_count].struct_def_idx = s_idx;
            next_token(p);
        } else if (e_idx != (size_t)-1) {
            fn.params[fn.param_count].kind = PARAM_ENUM;
            fn.params[fn.param_count].enum_def_idx = e_idx;
            next_token(p);
        } else if (match_token(p, TOK_RESOURCE)) {
            fn.params[fn.param_count].kind = PARAM_RESOURCE;
            fn.params[fn.param_count].scalar_bounds = oath_interval_create(1, INT64_MAX);
        } else if (match_token(p, TOK_TAINTED)) {
            fn.params[fn.param_count].kind = PARAM_TAINTED;
            fn.params[fn.param_count].scalar_bounds = oath_interval_create(INT64_MIN, INT64_MAX);
        } else {
            OathInterval iv = parse_interval_bounds(p);

            if (match_token(p, TOK_SEMI)) {
                if (p->current.type == TOK_INT_LITERAL) {
                    fn.params[fn.param_count].kind = PARAM_BUFFER;
                    fn.params[fn.param_count].buffer.capacity = (size_t)p->current.int_val;
                    fn.params[fn.param_count].buffer.elem_bounds = iv;
                    next_token(p);
                } else if (p->current.type == TOK_IDENT) {
                    fn.params[fn.param_count].kind = PARAM_SLICE;
                    safe_copy_str(fn.params[fn.param_count].slice.len_param_name, p->current.text, sizeof(fn.params[fn.param_count].slice.len_param_name));
                    fn.params[fn.param_count].slice.elem_bounds = iv;
                    next_token(p);
                } else {
                    p->has_error = true;
                    snprintf(p->error_msg, sizeof(p->error_msg), "Expected buffer capacity or dynamic length parameter name");
                    return fn;
                }
                if (!expect_token(p, TOK_RBRACKET, "']'")) return fn;
            } else {
                if (!expect_token(p, TOK_RBRACKET, "']'")) return fn;
                fn.params[fn.param_count].kind = PARAM_SCALAR;
                fn.params[fn.param_count].scalar_bounds = iv;
            }
        }

        fn.param_count++;
        if (!match_token(p, TOK_COMMA)) break;
    }

    if (!expect_token(p, TOK_RPAREN, "')'")) return fn;

    for (size_t i = 0; i < fn.param_count; ++i) {
        if (fn.params[i].kind == PARAM_SLICE) {
            size_t len_p = (size_t)-1;
            for (size_t k = 0; k < fn.param_count; ++k) {
                if (strcmp(fn.params[k].name, fn.params[i].slice.len_param_name) == 0) {
                    len_p = k;
                    break;
                }
            }
            if (len_p == (size_t)-1) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg), "Unknown length parameter '%s' for slice '%s'",
                         fn.params[i].slice.len_param_name, fn.params[i].name);
                return fn;
            }
            fn.params[i].slice.len_param_idx = len_p;
        }
    }

    while (p->current.type == TOK_REQUIRES || p->current.type == TOK_ENSURES) {
        if (match_token(p, TOK_REQUIRES)) {
            parse_requires_clause(p, &fn, mod);
            if (p->has_error) return fn;
        } else if (match_token(p, TOK_ENSURES)) {
            parse_ensures_clause(p, &fn, mod);
            if (p->has_error) return fn;
        }
    }

    if (!expect_token(p, TOK_ARROW, "'->'")) return fn;

    if (match_token(p, TOK_RESOURCE)) {
        fn.returns_resource = true;
        fn.postcondition = oath_interval_create(1, INT64_MAX);
    } else {
        OathInterval post = parse_interval_bounds(p);
        if (!expect_token(p, TOK_RBRACKET, "']'")) return fn;
        fn.postcondition = post;
    }

    while (p->current.type == TOK_REQUIRES || p->current.type == TOK_ENSURES) {
        if (match_token(p, TOK_REQUIRES)) {
            parse_requires_clause(p, &fn, mod);
            if (p->has_error) return fn;
        } else if (match_token(p, TOK_ENSURES)) {
            parse_ensures_clause(p, &fn, mod);
            if (p->has_error) return fn;
        }
    }

    if (is_extern) {
        if (!expect_token(p, TOK_SEMI, "';'")) return fn;
        fn.root_stmt = NULL;
    } else {
        if (!expect_token(p, TOK_LBRACE, "'{'")) return fn;
        fn.root_stmt = parse_statement_list(p, &fn, mod);
        if (!expect_token(p, TOK_RBRACE, "'}'")) return fn;
    }

    return fn;
}

OathModule oath_parser_parse_module(OathParser* p) {
    OathModule mod;
    memset(&mod, 0, sizeof(mod));

    while (p->current.type != TOK_EOF && !p->has_error) {
        if (p->current.type == TOK_STRUCT) {
            parse_struct_decl(p, &mod);
        } else if (p->current.type == TOK_ENUM) {
            parse_enum_decl(p, &mod);
        } else if (match_token(p, TOK_EXTERN)) {
            if (mod.function_count >= OATH_MAX_FUNCTIONS) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max function count");
                break;
            }
            mod.functions[mod.function_count++] = parse_function_internal(p, &mod, true);
        } else if (p->current.type == TOK_FN) {
            if (mod.function_count >= OATH_MAX_FUNCTIONS) {
                p->has_error = true;
                snprintf(p->error_msg, sizeof(p->error_msg), "Exceeded max function count");
                break;
            }
            mod.functions[mod.function_count++] = parse_function_internal(p, &mod, false);
        } else {
            p->has_error = true;
            snprintf(p->error_msg, sizeof(p->error_msg), "Expected 'struct', 'enum', 'extern' or 'fn', got '%s'", p->current.text);
            break;
        }
    }
    return mod;
}