#include "oath/backend.h"
#include <string.h>
#include <ctype.h>

static const char* get_var_name(const OathFunction* fn, size_t idx) {
    if (idx < fn->param_count) return fn->params[idx].name;
    return fn->locals[idx - fn->param_count].name;
}

static void emit_expr_c(FILE* out, const OathExpr* expr, const OathFunction* fn, const OathModule* mod) {
    switch (expr->type) {
        case EXPR_LITERAL: fprintf(out, "%" PRId64, expr->literal); break;
        case EXPR_VAR:     fprintf(out, "%s", get_var_name(fn, expr->var_idx)); break;
        case EXPR_FIELD:
            fprintf(out, "%s.%s", get_var_name(fn, expr->var_idx), expr->field_name);
            break;
        case EXPR_INDEX:
            fprintf(out, "%s[", get_var_name(fn, expr->lhs->var_idx));
            emit_expr_c(out, expr->rhs, fn, mod);
            fprintf(out, "]");
            break;
        case EXPR_ALLOC:
            fprintf(out, "(int64_t)malloc((size_t)(");
            emit_expr_c(out, expr->lhs, fn, mod);
            fprintf(out, ") * sizeof(int64_t))");
            break;
        case EXPR_ENUM_CONSTRUCT: {
            const OathEnumDef* ed = &mod->enums[expr->enum_construct.enum_idx];
            fprintf(out, "(%s){ .tag = %zu, .val = ", ed->name, expr->enum_construct.variant_idx);
            emit_expr_c(out, expr->enum_construct.payload, fn, mod);
            fprintf(out, " }");
            break;
        }
        case EXPR_CALL:
            fprintf(out, "%s(", expr->call_name);
            for (size_t i = 0; i < expr->arg_count; ++i) {
                emit_expr_c(out, expr->args[i], fn, mod);
                if (i + 1 < expr->arg_count) fprintf(out, ", ");
            }
            fprintf(out, ")");
            break;
        case EXPR_ADD:     fprintf(out, "("); emit_expr_c(out, expr->lhs, fn, mod); fprintf(out, " + "); emit_expr_c(out, expr->rhs, fn, mod); fprintf(out, ")"); break;
        case EXPR_SUB:     fprintf(out, "("); emit_expr_c(out, expr->lhs, fn, mod); fprintf(out, " - "); emit_expr_c(out, expr->rhs, fn, mod); fprintf(out, ")"); break;
        case EXPR_MUL:     fprintf(out, "("); emit_expr_c(out, expr->lhs, fn, mod); fprintf(out, " * "); emit_expr_c(out, expr->rhs, fn, mod); fprintf(out, ")"); break;
        case EXPR_DIV:     fprintf(out, "("); emit_expr_c(out, expr->lhs, fn, mod); fprintf(out, " / "); emit_expr_c(out, expr->rhs, fn, mod); fprintf(out, ")"); break;
    }
}

static void emit_atomic_condition_c(FILE* out, const OathAtomicCond* cond, const OathFunction* fn, const OathModule* mod) {
    static const char* op_sym[] = { "==", "!=", "<", "<=", ">", ">=" };

    size_t cur = 0;
    char lhs_str[64] = {0};
    char rhs_str[64] = {0};

    for (size_t p = 0; p < fn->param_count; ++p) {
        if (fn->params[p].kind == PARAM_STRUCT) {
            const OathStructDef* s = &mod->structs[fn->params[p].struct_def_idx];
            for (size_t f = 0; f < s->field_count; ++f) {
                if (cur == cond->lhs_slot) snprintf(lhs_str, sizeof(lhs_str), "%s.%s", fn->params[p].name, s->fields[f].name);
                if (cond->rhs_is_slot && cur == cond->rhs.rhs_slot) snprintf(rhs_str, sizeof(rhs_str), "%s.%s", fn->params[p].name, s->fields[f].name);
                cur++;
            }
        } else {
            if (cur == cond->lhs_slot) snprintf(lhs_str, sizeof(lhs_str), "%s", fn->params[p].name);
            if (cond->rhs_is_slot && cur == cond->rhs.rhs_slot) snprintf(rhs_str, sizeof(rhs_str), "%s", fn->params[p].name);
            cur++;
        }
    }
    for (size_t l = 0; l < fn->local_count; ++l) {
        if (cur == cond->lhs_slot) snprintf(lhs_str, sizeof(lhs_str), "%s", fn->locals[l].name);
        if (cond->rhs_is_slot && cur == cond->rhs.rhs_slot) snprintf(rhs_str, sizeof(rhs_str), "%s", fn->locals[l].name);
        cur++;
    }

    if (cond->rhs_is_slot) {
        fprintf(out, "%s %s %s", lhs_str, op_sym[cond->op], rhs_str);
    } else {
        fprintf(out, "%s %s %" PRId64, lhs_str, op_sym[cond->op], cond->rhs.const_val);
    }
}

static void emit_condition_c(FILE* out, const OathCondition* cond, const OathFunction* fn, const OathModule* mod) {
    for (size_t t = 0; t < cond->count; ++t) {
        emit_atomic_condition_c(out, &cond->terms[t], fn, mod);
        if (t + 1 < cond->count) fprintf(out, " && ");
    }
}

static void emit_stmt_c(FILE* out, const OathStmt* stmt, const OathFunction* fn, const OathModule* mod, int indent) {
    while (stmt) {
        for (int i = 0; i < indent; ++i) fprintf(out, "    ");

        switch (stmt->type) {
            case STMT_MATCH: {
                fprintf(out, "switch (%s.tag) {\n", get_var_name(fn, stmt->as.match_stmt.target_var_idx));
                for (size_t a = 0; a < stmt->as.match_stmt.arm_count; ++a) {
                    size_t v_idx = stmt->as.match_stmt.arms[a].variant_idx;
                    for (int i = 0; i < indent + 1; ++i) fprintf(out, "    ");
                    fprintf(out, "case %zu: {\n", v_idx);
                    for (int i = 0; i < indent + 2; ++i) fprintf(out, "    ");
                    fprintf(out, "int64_t %s = %s.val;\n",
                            fn->locals[stmt->as.match_stmt.arms[a].bound_var_idx - fn->param_count].name,
                            get_var_name(fn, stmt->as.match_stmt.target_var_idx));

                    emit_stmt_c(out, stmt->as.match_stmt.arms[a].body, fn, mod, indent + 2);
                    for (int i = 0; i < indent + 2; ++i) fprintf(out, "    ");
                    fprintf(out, "break;\n");
                    for (int i = 0; i < indent + 1; ++i) fprintf(out, "    ");
                    fprintf(out, "}\n");
                }
                for (int i = 0; i < indent; ++i) fprintf(out, "    ");
                fprintf(out, "}\n");
                break;
            }

            case STMT_LET:
                if (stmt->as.let_stmt.init_expr->type == EXPR_ENUM_CONSTRUCT) {
                    const OathEnumDef* ed = &mod->enums[stmt->as.let_stmt.init_expr->enum_construct.enum_idx];
                    fprintf(out, "%s %s = ", ed->name, get_var_name(fn, stmt->as.let_stmt.var_idx));
                } else {
                    fprintf(out, "int64_t %s = ", get_var_name(fn, stmt->as.let_stmt.var_idx));
                }
                emit_expr_c(out, stmt->as.let_stmt.init_expr, fn, mod);
                fprintf(out, ";\n");
                break;

            case STMT_FREE:
                fprintf(out, "free((void*)%s);\n", get_var_name(fn, stmt->as.free_stmt.var_idx));
                break;

            case STMT_CALL:
                emit_expr_c(out, stmt->as.call_stmt.expr, fn, mod);
                fprintf(out, ";\n");
                break;

            case STMT_ASSIGN:
                fprintf(out, "%s = ", get_var_name(fn, stmt->as.assign_stmt.var_idx));
                emit_expr_c(out, stmt->as.assign_stmt.expr, fn, mod);
                fprintf(out, ";\n");
                break;

            case STMT_ARRAY_SET:
                fprintf(out, "%s[", get_var_name(fn, stmt->as.array_set.buf_var_idx));
                emit_expr_c(out, stmt->as.array_set.idx_expr, fn, mod);
                fprintf(out, "] = ");
                emit_expr_c(out, stmt->as.array_set.val_expr, fn, mod);
                fprintf(out, ";\n");
                break;

            case STMT_RETURN:
                fprintf(out, "return ");
                emit_expr_c(out, stmt->as.ret.expr, fn, mod);
                fprintf(out, ";\n");
                break;

            case STMT_IF_ELSE:
                fprintf(out, "if (");
                emit_condition_c(out, &stmt->as.if_else.cond, fn, mod);
                fprintf(out, ") {\n");

                emit_stmt_c(out, stmt->as.if_else.then_branch, fn, mod, indent + 1);

                for (int i = 0; i < indent; ++i) fprintf(out, "    ");
                fprintf(out, "} else {\n");

                emit_stmt_c(out, stmt->as.if_else.else_branch, fn, mod, indent + 1);

                for (int i = 0; i < indent; ++i) fprintf(out, "    ");
                fprintf(out, "}\n");
                break;

            case STMT_WHILE:
                fprintf(out, "while (");
                emit_condition_c(out, &stmt->as.while_loop.cond, fn, mod);
                fprintf(out, ") {\n");

                emit_stmt_c(out, stmt->as.while_loop.body, fn, mod, indent + 1);

                for (int i = 0; i < indent; ++i) fprintf(out, "    ");
                fprintf(out, "}\n");
                break;
        }
        stmt = stmt->next;
    }
}

static bool is_param_disjoint(const OathFunction* fn, size_t p_idx) {
    for (size_t d = 0; d < fn->disjoint_count; ++d) {
        if (fn->disjoint_contracts[d].param_a == p_idx || fn->disjoint_contracts[d].param_b == p_idx) {
            return true;
        }
    }
    return false;
}

static void emit_param_type_c(FILE* stream, const OathFunction* fn, size_t p, const OathModule* mod) {
    const OathParam* param = &fn->params[p];
    if (param->kind == PARAM_STRUCT) {
        fprintf(stream, "%s %s", mod->structs[param->struct_def_idx].name, param->name);
    } else if (param->kind == PARAM_ENUM) {
        fprintf(stream, "%s %s", mod->enums[param->enum_def_idx].name, param->name);
    } else if (param->kind == PARAM_BUFFER) {
        if (is_param_disjoint(fn, p)) {
            fprintf(stream, "int64_t %s[static restrict %zu]", param->name, param->buffer.capacity);
        } else {
            fprintf(stream, "int64_t %s[static %zu]", param->name, param->buffer.capacity);
        }
    } else if (param->kind == PARAM_SLICE) {
        if (is_param_disjoint(fn, p)) {
            fprintf(stream, "int64_t* restrict %s", param->name);
        } else {
            fprintf(stream, "int64_t* %s", param->name);
        }
    } else {
        fprintf(stream, "int64_t %s", param->name);
    }
}

static bool stmt_always_returns(const OathStmt* stmt) {
    while (stmt) {
        if (stmt->type == STMT_RETURN) return true;
        if (stmt->type == STMT_IF_ELSE) {
            if (stmt->as.if_else.then_branch && stmt->as.if_else.else_branch) {
                if (stmt_always_returns(stmt->as.if_else.then_branch) &&
                    stmt_always_returns(stmt->as.if_else.else_branch)) {
                    return true;
                }
            }
        }
        stmt = stmt->next;
    }
    return false;
}

void oath_emit_c99_module(FILE* stream, const OathModule* mod) {
    fprintf(stream, "#include <stdint.h>\n");
    fprintf(stream, "#include <stdio.h>\n");
    fprintf(stream, "#include <stdlib.h>\n\n");

    for (size_t s = 0; s < mod->struct_count; ++s) {
        const OathStructDef* sd = &mod->structs[s];
        fprintf(stream, "typedef struct {\n");
        for (size_t f = 0; f < sd->field_count; ++f) {
            fprintf(stream, "    int64_t %s;\n", sd->fields[f].name);
        }
        fprintf(stream, "} %s;\n\n", sd->name);
    }

    for (size_t e = 0; e < mod->enum_count; ++e) {
        const OathEnumDef* ed = &mod->enums[e];
        fprintf(stream, "typedef struct {\n");
        fprintf(stream, "    size_t tag;\n");
        fprintf(stream, "    int64_t val;\n");
        fprintf(stream, "} %s;\n\n", ed->name);
    }

    for (size_t i = 0; i < mod->function_count; ++i) {
        const OathFunction* fn = &mod->functions[i];
        if (fn->is_extern) {
            fprintf(stream, "extern int64_t %s(", fn->name);
        } else {
            fprintf(stream, "int64_t %s(", fn->name);
        }
        for (size_t p = 0; p < fn->param_count; ++p) {
            emit_param_type_c(stream, fn, p, mod);
            if (p + 1 < fn->param_count) fprintf(stream, ", ");
        }
        fprintf(stream, ");\n");
    }
    fprintf(stream, "\n");

    for (size_t i = 0; i < mod->function_count; ++i) {
        const OathFunction* fn = &mod->functions[i];
        if (fn->is_extern) continue;

        fprintf(stream, "int64_t %s(", fn->name);
        for (size_t p = 0; p < fn->param_count; ++p) {
            emit_param_type_c(stream, fn, p, mod);
            if (p + 1 < fn->param_count) fprintf(stream, ", ");
        }
        fprintf(stream, ") {\n");
        emit_stmt_c(stream, fn->root_stmt, fn, mod, 1);
        if (!stmt_always_returns(fn->root_stmt)) {
            fprintf(stream, "    return 0;\n");
        } else {
            fprintf(stream, "    #if defined(__GNUC__) || defined(__clang__)\n");
            fprintf(stream, "    __builtin_unreachable();\n");
            fprintf(stream, "    #endif\n");
        }
        fprintf(stream, "}\n\n");
    }
}

void oath_emit_c_header(FILE* stream, const OathModule* mod, const char* prefix) {
    char guard[128];
    snprintf(guard, sizeof(guard), "OATH_EXPORT_%s_H", prefix);
    for (size_t i = 0; guard[i]; ++i) guard[i] = (char)toupper((unsigned char)guard[i]);

    fprintf(stream, "#ifndef %s\n", guard);
    fprintf(stream, "#define %s\n\n", guard);
    fprintf(stream, "#include <stdint.h>\n");
    fprintf(stream, "#include <stdlib.h>\n\n");
    fprintf(stream, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");

    for (size_t s = 0; s < mod->struct_count; ++s) {
        const OathStructDef* sd = &mod->structs[s];
        fprintf(stream, "typedef struct {\n");
        for (size_t f = 0; f < sd->field_count; ++f) {
            fprintf(stream, "    int64_t %s;\n", sd->fields[f].name);
        }
        fprintf(stream, "} %s;\n\n", sd->name);
    }

    for (size_t e = 0; e < mod->enum_count; ++e) {
        const OathEnumDef* ed = &mod->enums[e];
        fprintf(stream, "typedef struct {\n");
        fprintf(stream, "    size_t tag;\n");
        fprintf(stream, "    int64_t val;\n");
        fprintf(stream, "} %s;\n\n", ed->name);
    }

    for (size_t i = 0; i < mod->function_count; ++i) {
        const OathFunction* fn = &mod->functions[i];
        if (fn->is_extern) {
            fprintf(stream, "extern int64_t %s(", fn->name);
        } else {
            fprintf(stream, "int64_t %s(", fn->name);
        }
        for (size_t p = 0; p < fn->param_count; ++p) {
            emit_param_type_c(stream, fn, p, mod);
            if (p + 1 < fn->param_count) fprintf(stream, ", ");
        }
        fprintf(stream, ");\n\n");
    }

    fprintf(stream, "#ifdef __cplusplus\n}\n#endif\n\n");
    fprintf(stream, "#endif\n");
}

void oath_emit_b2b_certificate(FILE* stream, const OathFunction* fn, const SepeReport* report) {
    static const char* diag_str[] = {
        "VERIFIED_SOUND",
        "DIVIDE_BY_ZERO",
        "X86_DE_OVERFLOW",
        "INTEGER_OVERFLOW",
        "CONTRACT_VIOLATION",
        "BUFFER_OVERFLOW",
        "CALL_CONTRACT_VIOLATION",
        "UNKNOWN_FUNCTION",
        "LOOP_INIT_FAILED",
        "LOOP_INVARIANT_BROKEN",
        "ALIASING_VIOLATION",
        "RESOURCE_LEAK",
        "DOUBLE_FREE",
        "USE_AFTER_FREE",
        "NON_EXHAUSTIVE_MATCH",
        "ENSURES_VIOLATION",
        "BUFFER_CAPACITY_MISMATCH",
        "TAINTED_INDEX",
        "TAINTED_ARGUMENT",
        "BRANCH_RESOURCE_MISMATCH"
    };

    fprintf(stream, "{\n");
    fprintf(stream, "  \"schema\": \"OATH-FINAL-v2.0\",\n");
    fprintf(stream, "  \"target\": \"%s\",\n", fn->name);
    fprintf(stream, "  \"status\": \"%s\",\n", report->is_valid ? "PROVED" : "REJECTED");
    fprintf(stream, "  \"paths_explored\": %zu,\n", report->total_paths_explored);
    fprintf(stream, "  \"infeasible_paths_pruned\": %zu,\n", report->infeasible_paths_pruned);
    fprintf(stream, "  \"diagnostic\": \"%s\"", diag_str[report->diag]);
    if (!report->is_valid) {
        fprintf(stream, ",\n  \"counter_example\": \"%s\"\n", report->counter_example);
    } else {
        fprintf(stream, "\n");
    }
    fprintf(stream, "}\n");
}