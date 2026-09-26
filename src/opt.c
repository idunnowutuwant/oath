#include "oath/opt.h"

static void optimize_block(OirBasicBlock* bb) {
    OirInst* inst = bb->head;
    while (inst) {
        if (inst->op == OIR_OP_ADD && inst->has_imm && inst->imm == 0) {
            inst->op = OIR_OP_MOV;
        } else if (inst->op == OIR_OP_SUB && inst->has_imm && inst->imm == 0) {
            inst->op = OIR_OP_MOV;
        } else if (inst->op == OIR_OP_MUL && inst->has_imm) {
            if (inst->imm == 1) {
                inst->op = OIR_OP_MOV;
            } else if (inst->imm == 0) {
                inst->op = OIR_OP_CONST;
                inst->imm = 0;
            }
        }
        inst = inst->next;
    }
}

void oir_optimize_module(OirModule* mod) {
    for (size_t f = 0; f < mod->function_count; ++f) {
        OirFunction* fn = &mod->functions[f];
        if (fn->is_extern) continue;
        for (size_t b = 0; b < fn->block_count; ++b) {
            optimize_block(&fn->blocks[b]);
        }
    }
}