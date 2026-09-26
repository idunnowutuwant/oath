#include "oath/oir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

OirModule oir_module_create(size_t capacity) {
    OirModule mod;
    memset(&mod, 0, sizeof(mod));
    mod.functions = (OirFunction*)malloc(sizeof(OirFunction) * capacity);
    mod.function_capacity = capacity;
    mod.function_count = 0;
    return mod;
}

OirFunction* oir_module_add_function(OirModule* mod, const char* name) {
    if (mod->function_count >= mod->function_capacity) return NULL;
    OirFunction* fn = &mod->functions[mod->function_count++];
    memset(fn, 0, sizeof(*fn));
    snprintf(fn->name, sizeof(fn->name), "%s", name);
    fn->block_capacity = 32;
    fn->blocks = (OirBasicBlock*)malloc(sizeof(OirBasicBlock) * fn->block_capacity);
    fn->block_count = 0;
    fn->reg_count = 0;
    return fn;
}

uint32_t oir_function_add_block(OirFunction* fn, const char* label) {
    if (fn->block_count >= fn->block_capacity) {
        fn->block_capacity *= 2;
        fn->blocks = (OirBasicBlock*)realloc(fn->blocks, sizeof(OirBasicBlock) * fn->block_capacity);
    }
    uint32_t id = (uint32_t)fn->block_count++;
    OirBasicBlock* bb = &fn->blocks[id];
    memset(bb, 0, sizeof(*bb));
    bb->id = id;
    snprintf(bb->label, sizeof(bb->label), "%s", label);
    return id;
}

OirReg oir_function_alloc_reg(OirFunction* fn) {
    return (OirReg)(fn->reg_count++);
}

void oir_block_append_inst(OirBasicBlock* bb, OirInst* inst) {
    if (!bb->head) {
        bb->head = inst;
        bb->tail = inst;
    } else {
        bb->tail->next = inst;
        bb->tail = inst;
    }
    bb->inst_count++;
}

OirInst* oir_inst_create(OathArena* arena, OirOpcode op) {
    OirInst* inst = (OirInst*)oath_arena_alloc(arena, sizeof(OirInst));
    memset(inst, 0, sizeof(*inst));
    inst->op = op;
    return inst;
}

void oir_dump_module(FILE* out, const OirModule* mod) {
    static const char* op_names[] = {
        "nop", "const", "mov", "add", "sub", "mul", "div",
        "load.array", "store.array", "alloc", "free", "call",
        "br", "jmp", "ret", "phi"
    };

    fprintf(out, "; --- OATH IR (Universal SSA Bytecode) ---\n\n");
    for (size_t f = 0; f < mod->function_count; ++f) {
        const OirFunction* fn = &mod->functions[f];
        fprintf(out, "fn @%s(", fn->name);
        for (size_t p = 0; p < fn->param_count; ++p) {
            fprintf(out, "%%r%zu: [%" PRId64 ", %" PRId64 "]", p, fn->params[p].scalar_bounds.lo, fn->params[p].scalar_bounds.hi);
            if (p + 1 < fn->param_count) fprintf(out, ", ");
        }
        fprintf(out, ") -> [%" PRId64 ", %" PRId64 "] {\n", fn->postcondition.lo, fn->postcondition.hi);

        for (size_t b = 0; b < fn->block_count; ++b) {
            const OirBasicBlock* bb = &fn->blocks[b];
            fprintf(out, "  %s (bb_%u):\n", bb->label, bb->id);
            const OirInst* inst = bb->head;
            while (inst) {
                fprintf(out, "    ");
                switch (inst->op) {
                    case OIR_OP_CONST:
                        fprintf(out, "%%r%u = const %" PRId64 "\n", inst->dst, inst->imm);
                        break;
                    case OIR_OP_MOV:
                        fprintf(out, "%%r%u = mov %%r%u\n", inst->dst, inst->src1);
                        break;
                    case OIR_OP_ADD:
                    case OIR_OP_SUB:
                    case OIR_OP_MUL:
                    case OIR_OP_DIV:
                        if (inst->has_imm) {
                            fprintf(out, "%%r%u = %s %%r%u, %" PRId64 "\n", inst->dst, op_names[inst->op], inst->src1, inst->imm);
                        } else {
                            fprintf(out, "%%r%u = %s %%r%u, %%r%u\n", inst->dst, op_names[inst->op], inst->src1, inst->src2);
                        }
                        break;
                    case OIR_OP_LOAD_ARRAY:
                        fprintf(out, "%%r%u = load.array %%r%u[%%r%u]\n", inst->dst, inst->src1, inst->src2);
                        break;
                    case OIR_OP_STORE_ARRAY:
                        fprintf(out, "store.array %%r%u[%%r%u], %%r%u\n", inst->src1, inst->src2, inst->dst);
                        break;
                    case OIR_OP_ALLOC:
                        fprintf(out, "%%r%u = alloc %%r%u\n", inst->dst, inst->src1);
                        break;
                    case OIR_OP_FREE:
                        fprintf(out, "free %%r%u\n", inst->src1);
                        break;
                    case OIR_OP_CALL:
                        fprintf(out, "%%r%u = call @%s(", inst->dst, inst->call_name);
                        for (size_t i = 0; i < inst->call_arg_count; ++i) {
                            fprintf(out, "%%r%u", inst->call_args[i]);
                            if (i + 1 < inst->call_arg_count) fprintf(out, ", ");
                        }
                        fprintf(out, ")\n");
                        break;
                    case OIR_OP_BR:
                        fprintf(out, "br (");
                        for (size_t t = 0; t < inst->cond.count; ++t) {
                            if (inst->cond.terms[t].rhs_is_slot) {
                                fprintf(out, "%%r%zu %d %%r%zu", inst->cond.terms[t].lhs_slot, inst->cond.terms[t].op, inst->cond.terms[t].rhs.rhs_slot);
                            } else {
                                fprintf(out, "%%r%zu %d %" PRId64, inst->cond.terms[t].lhs_slot, inst->cond.terms[t].op, inst->cond.terms[t].rhs.const_val);
                            }
                            if (t + 1 < inst->cond.count) fprintf(out, " && ");
                        }
                        fprintf(out, ") ? bb_%u : bb_%u\n", inst->true_bb, inst->false_bb);
                        break;
                    case OIR_OP_JMP:
                        fprintf(out, "jmp bb_%u\n", inst->target_bb);
                        break;
                    case OIR_OP_RET:
                        fprintf(out, "ret %%r%u\n", inst->src1);
                        break;
                    default:
                        fprintf(out, "%s\n", op_names[inst->op]);
                        break;
                }
                inst = inst->next;
            }
        }
        fprintf(out, "}\n\n");
    }
}