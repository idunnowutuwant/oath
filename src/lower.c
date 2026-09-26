#include "oath/lower.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    OirFunction* fn;
    uint32_t current_bb;
    OathArena* arena;
    const OathFunction* ast_fn;
    const OathModule* ast_mod;
    OirReg canonical_reg[OATH_MAX_PARAMS + OATH_MAX_LOCALS];
} LowerContext;

static OirReg lower_expr(LowerContext* ctx, const OathExpr* expr) {
    OirFunction* fn = ctx->fn;
    OirBasicBlock* bb = &fn->blocks[ctx->current_bb];

    switch (expr->type) {
        case EXPR_LITERAL: {
            OirReg r = oir_function_alloc_reg(fn);
            OirInst* inst = oir_inst_create(ctx->arena, OIR_OP_CONST);
            inst->dst = r;
            inst->imm = expr->literal;
            inst->bound = oath_interval_create(expr->literal, expr->literal);
            oir_block_append_inst(bb, inst);
            return r;
        }
        case EXPR_VAR:
        case EXPR_FIELD:
            return ctx->canonical_reg[expr->var_idx];

        case EXPR_ADD:
        case EXPR_SUB:
        case EXPR_MUL:
        case EXPR_DIV: {
            OirReg r1 = lower_expr(ctx, expr->lhs);
            OirReg r2 = lower_expr(ctx, expr->rhs);
            bb = &fn->blocks[ctx->current_bb];

            OirOpcode op = OIR_OP_ADD;
            if (expr->type == EXPR_SUB) op = OIR_OP_SUB;
            else if (expr->type == EXPR_MUL) op = OIR_OP_MUL;
            else if (expr->type == EXPR_DIV) op = OIR_OP_DIV;

            OirReg dst = oir_function_alloc_reg(fn);
            OirInst* inst = oir_inst_create(ctx->arena, op);
            inst->dst = dst;
            inst->src1 = r1;
            inst->src2 = r2;
            inst->has_imm = false;
            oir_block_append_inst(bb, inst);
            return dst;
        }

        case EXPR_INDEX: {
            OirReg r_buf = lower_expr(ctx, expr->lhs);
            OirReg r_idx = lower_expr(ctx, expr->rhs);
            bb = &fn->blocks[ctx->current_bb];

            OirReg dst = oir_function_alloc_reg(fn);
            OirInst* inst = oir_inst_create(ctx->arena, OIR_OP_LOAD_ARRAY);
            inst->dst = dst;
            inst->src1 = r_buf;
            inst->src2 = r_idx;
            oir_block_append_inst(bb, inst);
            return dst;
        }

        case EXPR_ALLOC: {
            OirReg r_sz = lower_expr(ctx, expr->lhs);
            bb = &fn->blocks[ctx->current_bb];

            OirReg dst = oir_function_alloc_reg(fn);
            OirInst* inst = oir_inst_create(ctx->arena, OIR_OP_ALLOC);
            inst->dst = dst;
            inst->src1 = r_sz;
            oir_block_append_inst(bb, inst);
            return dst;
        }

        case EXPR_CALL: {
            OirReg args[OATH_MAX_PARAMS];
            for (size_t i = 0; i < expr->arg_count; ++i) {
                args[i] = lower_expr(ctx, expr->args[i]);
            }
            bb = &fn->blocks[ctx->current_bb];

            OirReg dst = oir_function_alloc_reg(fn);
            OirInst* inst = oir_inst_create(ctx->arena, OIR_OP_CALL);
            inst->dst = dst;
            snprintf(inst->call_name, sizeof(inst->call_name), "%s", expr->call_name);
            inst->call_arg_count = expr->arg_count;
            for (size_t i = 0; i < expr->arg_count; ++i) inst->call_args[i] = args[i];
            oir_block_append_inst(bb, inst);
            return dst;
        }

        case EXPR_ENUM_CONSTRUCT:
            return lower_expr(ctx, expr->enum_construct.payload);
    }
    return 0;
}

static void lower_stmt(LowerContext* ctx, const OathStmt* stmt);

static void lower_stmt_list(LowerContext* ctx, const OathStmt* stmt) {
    while (stmt) {
        lower_stmt(ctx, stmt);
        stmt = stmt->next;
    }
}

static void lower_stmt(LowerContext* ctx, const OathStmt* stmt) {
    OirFunction* fn = ctx->fn;
    OirBasicBlock* bb = &fn->blocks[ctx->current_bb];

    switch (stmt->type) {
        case STMT_LET: {
            OirReg r_val = lower_expr(ctx, stmt->as.let_stmt.init_expr);
            OirReg r_dst = ctx->canonical_reg[stmt->as.let_stmt.var_idx];
            bb = &fn->blocks[ctx->current_bb];

            OirInst* mov = oir_inst_create(ctx->arena, OIR_OP_MOV);
            mov->dst = r_dst;
            mov->src1 = r_val;
            oir_block_append_inst(bb, mov);
            break;
        }

        case STMT_ASSIGN: {
            OirReg r_val = lower_expr(ctx, stmt->as.assign_stmt.expr);
            OirReg r_dst = ctx->canonical_reg[stmt->as.assign_stmt.var_idx];
            bb = &fn->blocks[ctx->current_bb];

            OirInst* mov = oir_inst_create(ctx->arena, OIR_OP_MOV);
            mov->dst = r_dst;
            mov->src1 = r_val;
            oir_block_append_inst(bb, mov);
            break;
        }

        case STMT_ARRAY_SET: {
            OirReg r_buf = ctx->canonical_reg[stmt->as.array_set.buf_var_idx];
            OirReg r_idx = lower_expr(ctx, stmt->as.array_set.idx_expr);
            OirReg r_val = lower_expr(ctx, stmt->as.array_set.val_expr);
            bb = &fn->blocks[ctx->current_bb];

            OirInst* inst = oir_inst_create(ctx->arena, OIR_OP_STORE_ARRAY);
            inst->src1 = r_buf;
            inst->src2 = r_idx;
            inst->dst = r_val;
            oir_block_append_inst(bb, inst);
            break;
        }

        case STMT_FREE: {
            OirReg r = ctx->canonical_reg[stmt->as.free_stmt.var_idx];
            bb = &fn->blocks[ctx->current_bb];
            OirInst* inst = oir_inst_create(ctx->arena, OIR_OP_FREE);
            inst->src1 = r;
            oir_block_append_inst(bb, inst);
            break;
        }

        case STMT_CALL:
            lower_expr(ctx, stmt->as.call_stmt.expr);
            break;

        case STMT_RETURN: {
            OirReg r = lower_expr(ctx, stmt->as.ret.expr);
            bb = &fn->blocks[ctx->current_bb];
            OirInst* inst = oir_inst_create(ctx->arena, OIR_OP_RET);
            inst->src1 = r;
            oir_block_append_inst(bb, inst);
            break;
        }

        case STMT_IF_ELSE: {
            uint32_t then_id = oir_function_add_block(fn, "then");
            uint32_t else_id = oir_function_add_block(fn, "else");
            uint32_t merge_id = oir_function_add_block(fn, "if.merge");

            OirInst* br = oir_inst_create(ctx->arena, OIR_OP_BR);
            br->cond = stmt->as.if_else.cond;
            for (size_t t = 0; t < br->cond.count; ++t) {
                br->cond.terms[t].lhs_slot = ctx->canonical_reg[br->cond.terms[t].lhs_slot];
                if (br->cond.terms[t].rhs_is_slot) {
                    br->cond.terms[t].rhs.rhs_slot = ctx->canonical_reg[br->cond.terms[t].rhs.rhs_slot];
                }
            }
            br->true_bb = then_id;
            br->false_bb = else_id;
            oir_block_append_inst(bb, br);

            ctx->current_bb = then_id;
            lower_stmt_list(ctx, stmt->as.if_else.then_branch);
            OirInst* jmp_then = oir_inst_create(ctx->arena, OIR_OP_JMP);
            jmp_then->target_bb = merge_id;
            oir_block_append_inst(&fn->blocks[ctx->current_bb], jmp_then);

            ctx->current_bb = else_id;
            lower_stmt_list(ctx, stmt->as.if_else.else_branch);
            OirInst* jmp_else = oir_inst_create(ctx->arena, OIR_OP_JMP);
            jmp_else->target_bb = merge_id;
            oir_block_append_inst(&fn->blocks[ctx->current_bb], jmp_else);

            ctx->current_bb = merge_id;
            break;
        }

        case STMT_WHILE: {
            uint32_t head_id = oir_function_add_block(fn, "while.head");
            uint32_t body_id = oir_function_add_block(fn, "while.body");
            uint32_t exit_id = oir_function_add_block(fn, "while.exit");

            OirInst* jmp_head = oir_inst_create(ctx->arena, OIR_OP_JMP);
            jmp_head->target_bb = head_id;
            oir_block_append_inst(bb, jmp_head);

            ctx->current_bb = head_id;
            OirInst* br = oir_inst_create(ctx->arena, OIR_OP_BR);
            br->cond = stmt->as.while_loop.cond;
            for (size_t t = 0; t < br->cond.count; ++t) {
                br->cond.terms[t].lhs_slot = ctx->canonical_reg[br->cond.terms[t].lhs_slot];
                if (br->cond.terms[t].rhs_is_slot) {
                    br->cond.terms[t].rhs.rhs_slot = ctx->canonical_reg[br->cond.terms[t].rhs.rhs_slot];
                }
            }
            br->true_bb = body_id;
            br->false_bb = exit_id;
            oir_block_append_inst(&fn->blocks[head_id], br);

            ctx->current_bb = body_id;
            lower_stmt_list(ctx, stmt->as.while_loop.body);
            OirInst* jmp_back = oir_inst_create(ctx->arena, OIR_OP_JMP);
            jmp_back->target_bb = head_id;
            oir_block_append_inst(&fn->blocks[ctx->current_bb], jmp_back);

            ctx->current_bb = exit_id;
            break;
        }

        case STMT_MATCH: {
            uint32_t merge_id = oir_function_add_block(fn, "match.exit");
            for (size_t a = 0; a < stmt->as.match_stmt.arm_count; ++a) {
                char arm_label[32];
                snprintf(arm_label, sizeof(arm_label), "match.arm_%zu", a);
                uint32_t arm_id = oir_function_add_block(fn, arm_label);
                ctx->current_bb = arm_id;
                lower_stmt_list(ctx, stmt->as.match_stmt.arms[a].body);
                OirInst* jmp_merge = oir_inst_create(ctx->arena, OIR_OP_JMP);
                jmp_merge->target_bb = merge_id;
                oir_block_append_inst(&fn->blocks[ctx->current_bb], jmp_merge);
            }
            ctx->current_bb = merge_id;
            break;
        }
    }
}

OirModule oath_lower_ast_to_oir(const OathModule* ast_mod, OathArena* arena) {
    OirModule oir_mod = oir_module_create(ast_mod->function_count);
    oir_mod.struct_count = ast_mod->struct_count;
    memcpy(oir_mod.structs, ast_mod->structs, sizeof(OathStructDef) * ast_mod->struct_count);
    oir_mod.enum_count = ast_mod->enum_count;
    memcpy(oir_mod.enums, ast_mod->enums, sizeof(OathEnumDef) * ast_mod->enum_count);

    for (size_t f = 0; f < ast_mod->function_count; ++f) {
        const OathFunction* ast_fn = &ast_mod->functions[f];
        OirFunction* fn = oir_module_add_function(&oir_mod, ast_fn->name);
        fn->is_extern = ast_fn->is_extern;
        fn->returns_resource = ast_fn->returns_resource;
        fn->postcondition = ast_fn->postcondition;
        fn->param_count = ast_fn->param_count;
        memcpy(fn->params, ast_fn->params, sizeof(OathParam) * ast_fn->param_count);
        fn->local_count = ast_fn->local_count;
        memcpy(fn->locals, ast_fn->locals, sizeof(OathLocal) * ast_fn->local_count);
        fn->requires_count = ast_fn->requires_count;
        memcpy(fn->requires_clauses, ast_fn->requires_clauses, sizeof(OathCondition) * ast_fn->requires_count);

        if (ast_fn->is_extern) continue;

        LowerContext ctx;
        ctx.fn = fn;
        ctx.arena = arena;
        ctx.ast_fn = ast_fn;
        ctx.ast_mod = ast_mod;

        for (size_t p = 0; p < ast_fn->param_count; ++p) {
            ctx.canonical_reg[p] = oir_function_alloc_reg(fn);
        }
        for (size_t l = 0; l < ast_fn->local_count; ++l) {
            ctx.canonical_reg[ast_fn->param_count + l] = oir_function_alloc_reg(fn);
        }

        uint32_t entry_id = oir_function_add_block(fn, "entry");
        ctx.current_bb = entry_id;

        lower_stmt_list(&ctx, ast_fn->root_stmt);
    }

    return oir_mod;
}