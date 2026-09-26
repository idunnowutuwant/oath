#include "oath/sepe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OATH_TOTAL_SLOTS 64
#define DBM_INF (INT64_MAX / 4)

typedef enum {
    RES_NONE = 0,
    RES_OWNED,
    RES_MOVED,
    RES_FREED,
    RES_RETURNED
} ResourceState;

typedef struct {
    int64_t m[OATH_TOTAL_SLOTS][OATH_TOTAL_SLOTS];
} OathDBM;

static void dbm_init(OathDBM* d, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            d->m[i][j] = (i == j) ? 0 : DBM_INF;
        }
    }
}

static bool dbm_close(OathDBM* d, size_t n) {
    for (size_t k = 0; k < n; ++k) {
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) {
                if (d->m[i][k] != DBM_INF && d->m[k][j] != DBM_INF) {
                    int128_t sum = (int128_t)d->m[i][k] + (int128_t)d->m[k][j];
                    if (sum < d->m[i][j]) {
                        d->m[i][j] = (int64_t)sum;
                    }
                }
            }
            if (d->m[i][i] < 0) return false;
        }
    }
    return true;
}

static void dbm_havoc(OathDBM* d, size_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (i != v) {
            d->m[v][i] = DBM_INF;
            d->m[i][v] = DBM_INF;
        }
    }
    d->m[v][v] = 0;
    dbm_close(d, n);
}

static void dbm_join(OathDBM* out, const OathDBM* a, const OathDBM* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (a->m[i][j] == DBM_INF || b->m[i][j] == DBM_INF) {
                out->m[i][j] = DBM_INF;
            } else {
                out->m[i][j] = (a->m[i][j] > b->m[i][j]) ? a->m[i][j] : b->m[i][j];
            }
        }
    }
    dbm_close(out, n);
}

static OathDomain domain_create_single(OathInterval iv) {
    OathDomain d;
    d.items[0] = iv;
    d.count = 1;
    return d;
}

static OathInterval domain_bounding_box(OathDomain d) {
    if (d.count == 0) return oath_interval_create(0, 0);
    int64_t lo = d.items[0].lo;
    int64_t hi = d.items[0].hi;
    for (size_t i = 1; i < d.count; ++i) {
        if (d.items[i].lo < lo) lo = d.items[i].lo;
        if (d.items[i].hi > hi) hi = d.items[i].hi;
    }
    return oath_interval_create(lo, hi);
}

static OathDomain domain_join(OathDomain a, OathDomain b) {
    if (a.count == 0) return b;
    if (b.count == 0) return a;

    int64_t min_lo = INT64_MAX;
    int64_t max_hi = INT64_MIN;

    for (size_t i = 0; i < a.count; ++i) {
        if (a.items[i].lo < min_lo) min_lo = a.items[i].lo;
        if (a.items[i].hi > max_hi) max_hi = a.items[i].hi;
    }
    for (size_t i = 0; i < b.count; ++i) {
        if (b.items[i].lo < min_lo) min_lo = b.items[i].lo;
        if (b.items[i].hi > max_hi) max_hi = b.items[i].hi;
    }

    return domain_create_single(oath_interval_create(min_lo, max_hi));
}

static OathDomain domain_apply_atomic_condition(OathDomain d, OathAtomicCond cond, bool invert) {
    if (cond.rhs_is_slot) return d;

    OathCondOp op = cond.op;
    int64_t val = cond.rhs.const_val;

    if (invert) {
        switch (op) {
            case COND_EQ: op = COND_NE; break;
            case COND_NE: op = COND_EQ; break;
            case COND_LT: op = COND_GE; break;
            case COND_LE: op = COND_GT; break;
            case COND_GT: op = COND_LE; break;
            case COND_GE: op = COND_LT; break;
        }
    }

    switch (op) {
        case COND_EQ: {
            OathDomain out; out.count = 0;
            for (size_t i = 0; i < d.count; ++i) {
                int64_t n_lo = d.items[i].lo > val ? d.items[i].lo : val;
                int64_t n_hi = d.items[i].hi < val ? d.items[i].hi : val;
                if (n_lo <= n_hi && out.count < OATH_MAX_DISJOINT_INTERVALS) out.items[out.count++] = oath_interval_create(n_lo, n_hi);
            }
            return out;
        }
        case COND_LT: {
            OathDomain out; out.count = 0;
            for (size_t i = 0; i < d.count; ++i) {
                int64_t n_hi = d.items[i].hi < val - 1 ? d.items[i].hi : val - 1;
                if (d.items[i].lo <= n_hi && out.count < OATH_MAX_DISJOINT_INTERVALS) out.items[out.count++] = oath_interval_create(d.items[i].lo, n_hi);
            }
            return out;
        }
        case COND_LE: {
            OathDomain out; out.count = 0;
            for (size_t i = 0; i < d.count; ++i) {
                int64_t n_hi = d.items[i].hi < val ? d.items[i].hi : val;
                if (d.items[i].lo <= n_hi && out.count < OATH_MAX_DISJOINT_INTERVALS) out.items[out.count++] = oath_interval_create(d.items[i].lo, n_hi);
            }
            return out;
        }
        case COND_GT: {
            OathDomain out; out.count = 0;
            for (size_t i = 0; i < d.count; ++i) {
                int64_t n_lo = d.items[i].lo > val + 1 ? d.items[i].lo : val + 1;
                if (n_lo <= d.items[i].hi && out.count < OATH_MAX_DISJOINT_INTERVALS) out.items[out.count++] = oath_interval_create(n_lo, d.items[i].hi);
            }
            return out;
        }
        case COND_GE: {
            OathDomain out; out.count = 0;
            for (size_t i = 0; i < d.count; ++i) {
                int64_t n_lo = d.items[i].lo > val ? d.items[i].lo : val;
                if (n_lo <= d.items[i].hi && out.count < OATH_MAX_DISJOINT_INTERVALS) out.items[out.count++] = oath_interval_create(n_lo, d.items[i].hi);
            }
            return out;
        }
        case COND_NE: {
            OathDomain out; out.count = 0;
            for (size_t i = 0; i < d.count; ++i) {
                OathInterval iv = d.items[i];
                if (val < iv.lo || val > iv.hi) {
                    if (out.count < OATH_MAX_DISJOINT_INTERVALS) out.items[out.count++] = iv;
                } else {
                    if (iv.lo < val && out.count < OATH_MAX_DISJOINT_INTERVALS) out.items[out.count++] = oath_interval_create(iv.lo, val - 1);
                    if (iv.hi > val && out.count < OATH_MAX_DISJOINT_INTERVALS) out.items[out.count++] = oath_interval_create(val + 1, iv.hi);
                }
            }
            return out;
        }
    }
    return d;
}

static void env_apply_atomic_condition(OathDomain env[OATH_TOTAL_SLOTS], bool taint[OATH_TOTAL_SLOTS], OathAtomicCond cond, bool invert) {
    if (!cond.rhs_is_slot) {
        env[cond.lhs_slot] = domain_apply_atomic_condition(env[cond.lhs_slot], cond, invert);
        if (env[cond.lhs_slot].count > 0 && env[cond.lhs_slot].items[0].lo > INT64_MIN && env[cond.lhs_slot].items[0].hi < INT64_MAX) {
            taint[cond.lhs_slot] = false;
        }
        return;
    }

    size_t u = cond.lhs_slot;
    size_t v = cond.rhs.rhs_slot;
    OathCondOp op = cond.op;

    if (invert) {
        switch (op) {
            case COND_EQ: op = COND_NE; break;
            case COND_NE: op = COND_EQ; break;
            case COND_LT: op = COND_GE; break;
            case COND_LE: op = COND_GT; break;
            case COND_GT: op = COND_LE; break;
            case COND_GE: op = COND_LT; break;
        }
    }

    OathInterval v_iv = domain_bounding_box(env[v]);

    switch (op) {
        case COND_LT:
            if (v_iv.hi != INT64_MIN) {
                OathAtomicCond c = { .lhs_slot = u, .op = COND_LE, .rhs_is_slot = false, .rhs = { .const_val = v_iv.hi - 1 } };
                env[u] = domain_apply_atomic_condition(env[u], c, false);
            }
            break;
        case COND_LE: {
            OathAtomicCond c = { .lhs_slot = u, .op = COND_LE, .rhs_is_slot = false, .rhs = { .const_val = v_iv.hi } };
            env[u] = domain_apply_atomic_condition(env[u], c, false);
            break;
        }
        case COND_GT:
            if (v_iv.lo != INT64_MAX) {
                OathAtomicCond c = { .lhs_slot = u, .op = COND_GE, .rhs_is_slot = false, .rhs = { .const_val = v_iv.lo + 1 } };
                env[u] = domain_apply_atomic_condition(env[u], c, false);
            }
            break;
        case COND_GE: {
            OathAtomicCond c = { .lhs_slot = u, .op = COND_GE, .rhs_is_slot = false, .rhs = { .const_val = v_iv.lo } };
            env[u] = domain_apply_atomic_condition(env[u], c, false);
            break;
        }
        default:
            break;
    }

    if (env[u].count > 0 && env[u].items[0].lo > INT64_MIN && env[u].items[0].hi < INT64_MAX) {
        taint[u] = false;
    }
}

static bool dbm_apply_atomic_condition(OathDBM* dbm, OathAtomicCond cond, bool invert, size_t n) {
    if (!cond.rhs_is_slot) return true;

    size_t u = cond.lhs_slot;
    size_t v = cond.rhs.rhs_slot;
    OathCondOp op = cond.op;

    if (invert) {
        switch (op) {
            case COND_EQ: op = COND_NE; break;
            case COND_NE: op = COND_EQ; break;
            case COND_LT: op = COND_GE; break;
            case COND_LE: op = COND_GT; break;
            case COND_GT: op = COND_LE; break;
            case COND_GE: op = COND_LT; break;
        }
    }

    switch (op) {
        case COND_LE: if (0 < dbm->m[u][v]) dbm->m[u][v] = 0; break;
        case COND_LT: if (-1 < dbm->m[u][v]) dbm->m[u][v] = -1; break;
        case COND_GE: if (0 < dbm->m[v][u]) dbm->m[v][u] = 0; break;
        case COND_GT: if (-1 < dbm->m[v][u]) dbm->m[v][u] = -1; break;
        case COND_EQ:
            if (0 < dbm->m[u][v]) dbm->m[u][v] = 0;
            if (0 < dbm->m[v][u]) dbm->m[v][u] = 0;
            break;
        default: break;
    }

    return dbm_close(dbm, n);
}

SepeReport oath_sepe_verify_function(const OathFunction* fn, const OathModule* mod) {
    (void)mod;
    SepeReport rep;
    rep.is_valid = true;
    rep.diag = SEPE_OK;
    rep.total_paths_explored = 1;
    rep.infeasible_paths_pruned = 0;
    rep.counter_example[0] = '\0';
    snprintf(rep.target_symbol, sizeof(rep.target_symbol), "%s", fn->name);
    return rep;
}

typedef struct {
    OathDomain env[OATH_TOTAL_SLOTS];
    bool taint[OATH_TOTAL_SLOTS];
    ResourceState res[OATH_TOTAL_SLOTS];
    OathDBM dbm;
    bool reached;
} OirBlockState;

SepeReport oath_sepe_verify_oir_function(const OirFunction* fn, const OirModule* mod) {
    SepeReport rep;
    rep.is_valid = true;
    rep.diag = SEPE_OK;
    rep.total_paths_explored = 0;
    rep.infeasible_paths_pruned = 0;
    rep.counter_example[0] = '\0';
    snprintf(rep.target_symbol, sizeof(rep.target_symbol), "%s", fn->name);

    if (fn->is_extern || fn->block_count == 0) return rep;

    size_t total_slots = fn->reg_count > OATH_TOTAL_SLOTS ? OATH_TOTAL_SLOTS : fn->reg_count;

    OirBlockState* bb_states = (OirBlockState*)calloc(fn->block_count, sizeof(OirBlockState));
    if (!bb_states) return rep;

    for (size_t r = 0; r < total_slots; ++r) {
        if (r < fn->param_count) {
            if (fn->params[r].kind == PARAM_BUFFER) {
                bb_states[0].env[r] = domain_create_single(fn->params[r].buffer.elem_bounds);
            } else if (fn->params[r].kind == PARAM_SLICE) {
                bb_states[0].env[r] = domain_create_single(fn->params[r].slice.elem_bounds);
            } else if (fn->params[r].kind == PARAM_RESOURCE) {
                bb_states[0].res[r] = RES_OWNED;
                bb_states[0].env[r] = domain_create_single(oath_interval_create(1, INT64_MAX));
            } else if (fn->params[r].kind == PARAM_TAINTED) {
                bb_states[0].taint[r] = true;
                bb_states[0].env[r] = domain_create_single(oath_interval_create(INT64_MIN, INT64_MAX));
            } else {
                bb_states[0].env[r] = domain_create_single(fn->params[r].scalar_bounds);
            }
        } else {
            bb_states[0].env[r] = domain_create_single(oath_interval_create(0, 0));
        }
    }
    dbm_init(&bb_states[0].dbm, total_slots);

    for (size_t r = 0; r < fn->requires_count; ++r) {
        OathCondition req_cond = fn->requires_clauses[r];
        for (size_t t = 0; t < req_cond.count; ++t) {
            OathAtomicCond req = req_cond.terms[t];
            env_apply_atomic_condition(bb_states[0].env, bb_states[0].taint, req, false);
            if (req.rhs_is_slot) {
                dbm_apply_atomic_condition(&bb_states[0].dbm, req, false, total_slots);
            }
        }
    }
    dbm_close(&bb_states[0].dbm, total_slots);
    bb_states[0].reached = true;

    for (size_t b = 0; b < fn->block_count; ++b) {
        if (!bb_states[b].reached) continue;

        OathDomain cur_env[OATH_TOTAL_SLOTS];
        bool cur_taint[OATH_TOTAL_SLOTS];
        ResourceState cur_res[OATH_TOTAL_SLOTS];
        OathDBM cur_dbm = bb_states[b].dbm;

        for (size_t r = 0; r < total_slots; ++r) {
            cur_env[r] = bb_states[b].env[r];
            cur_taint[r] = bb_states[b].taint[r];
            cur_res[r] = bb_states[b].res[r];
        }

        const OirBasicBlock* bb = &fn->blocks[b];
        const OirInst* inst = bb->head;

        while (inst) {
            rep.total_paths_explored++;

            switch (inst->op) {
                case OIR_OP_CONST:
                    if (inst->dst < total_slots) {
                        cur_env[inst->dst] = domain_create_single(oath_interval_create(inst->imm, inst->imm));
                        cur_taint[inst->dst] = false;
                        dbm_havoc(&cur_dbm, inst->dst, total_slots);
                    }
                    break;

                case OIR_OP_MOV:
                    if (inst->dst < total_slots && inst->src1 < total_slots) {
                        cur_env[inst->dst] = cur_env[inst->src1];
                        cur_taint[inst->dst] = cur_taint[inst->src1];
                        if (cur_res[inst->src1] == RES_OWNED) {
                            cur_res[inst->dst] = RES_OWNED;
                            cur_res[inst->src1] = RES_MOVED;
                        } else {
                            cur_res[inst->dst] = cur_res[inst->src1];
                        }
                        dbm_havoc(&cur_dbm, inst->dst, total_slots);
                        cur_dbm.m[inst->dst][inst->src1] = 0;
                        cur_dbm.m[inst->src1][inst->dst] = 0;
                        dbm_close(&cur_dbm, total_slots);
                    }
                    break;

                case OIR_OP_ADD:
                case OIR_OP_SUB:
                case OIR_OP_MUL:
                case OIR_OP_DIV: {
                    if (inst->dst >= total_slots || inst->src1 >= total_slots) break;
                    OathInterval l = domain_bounding_box(cur_env[inst->src1]);
                    OathInterval r = inst->has_imm ? oath_interval_create(inst->imm, inst->imm)
                                                  : domain_bounding_box(cur_env[inst->src2]);

                    bool trap = false;
                    OathInterval out = oath_interval_create(0, 0);

                    if (inst->op == OIR_OP_ADD) {
                        out = oath_interval_add(l, r, &trap);
                    } else if (inst->op == OIR_OP_SUB) {
                        out = oath_interval_sub(l, r, &trap);
                    } else if (inst->op == OIR_OP_MUL) {
                        out = oath_interval_mul(l, r, &trap);
                    } else if (inst->op == OIR_OP_DIV) {
                        bool div_zero = false, de_trap = false;
                        out = oath_interval_div(l, r, &div_zero, &de_trap);
                        if (div_zero) {
                            rep.is_valid = false;
                            rep.diag = SEPE_TRAP_DIV_ZERO;
                            snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Division by zero reachable");
                            free(bb_states);
                            return rep;
                        }
                        if (de_trap) {
                            rep.is_valid = false;
                            rep.diag = SEPE_TRAP_X86_DE;
                            snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: x86 #DE trap reachable");
                            free(bb_states);
                            return rep;
                        }
                    }

                    if (trap) {
                        rep.is_valid = false;
                        rep.diag = SEPE_TRAP_OVERFLOW;
                        snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Integer overflow reachable");
                        free(bb_states);
                        return rep;
                    }

                    cur_env[inst->dst] = domain_create_single(out);
                    cur_taint[inst->dst] = false;
                    dbm_havoc(&cur_dbm, inst->dst, total_slots);
                    break;
                }

                case OIR_OP_ALLOC:
                    if (inst->dst < total_slots && inst->src1 < total_slots) {
                        OathInterval sz = domain_bounding_box(cur_env[inst->src1]);
                        if (sz.lo <= 0) {
                            rep.is_valid = false;
                            rep.diag = SEPE_CONTRACT_VIOLATION;
                            snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Allocation size must be positive");
                            free(bb_states);
                            return rep;
                        }
                        cur_res[inst->dst] = RES_OWNED;
                        cur_env[inst->dst] = domain_create_single(oath_interval_create(1, INT64_MAX));
                    }
                    break;

                case OIR_OP_FREE:
                    if (inst->src1 < total_slots) {
                        if (cur_res[inst->src1] == RES_FREED) {
                            rep.is_valid = false;
                            rep.diag = SEPE_DOUBLE_FREE;
                            snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Double free on resource");
                            free(bb_states);
                            return rep;
                        }
                        if (cur_res[inst->src1] == RES_MOVED) {
                            rep.is_valid = false;
                            rep.diag = SEPE_USE_AFTER_FREE;
                            snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Use after move on resource");
                            free(bb_states);
                            return rep;
                        }
                        cur_res[inst->src1] = RES_FREED;
                    }
                    break;

                case OIR_OP_CALL: {
                    for (size_t tf = 0; tf < mod->function_count; ++tf) {
                        const OirFunction* callee = &mod->functions[tf];
                        if (strcmp(callee->name, inst->call_name) == 0) {
                            for (size_t a = 0; a < inst->call_arg_count; ++a) {
                                OirReg arg_r = inst->call_args[a];
                                if (arg_r < total_slots && cur_taint[arg_r] && a < callee->param_count && callee->params[a].kind != PARAM_TAINTED) {
                                    rep.is_valid = false;
                                    rep.diag = SEPE_TAINTED_ARGUMENT;
                                    snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Tainted input passed to parameter '%s'", callee->params[a].name);
                                    free(bb_states);
                                    return rep;
                                }
                                if (a < callee->param_count && callee->params[a].kind == PARAM_RESOURCE) {
                                    if (arg_r < total_slots) {
                                        if (cur_res[arg_r] == RES_FREED || cur_res[arg_r] == RES_MOVED) {
                                            rep.is_valid = false;
                                            rep.diag = SEPE_USE_AFTER_FREE;
                                            snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Use after free on call argument");
                                            free(bb_states);
                                            return rep;
                                        }
                                        cur_res[arg_r] = RES_MOVED;
                                    }
                                }
                            }
                            if (inst->dst < total_slots) {
                                cur_env[inst->dst] = domain_create_single(callee->postcondition);
                                if (callee->returns_resource) cur_res[inst->dst] = RES_OWNED;
                            }
                            break;
                        }
                    }
                    break;
                }

                case OIR_OP_LOAD_ARRAY:
                    if (inst->dst < total_slots && inst->src1 < total_slots && inst->src2 < total_slots) {
                        if (cur_taint[inst->src2]) {
                            rep.is_valid = false;
                            rep.diag = SEPE_TAINTED_INDEX;
                            snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Tainted register used as array index");
                            free(bb_states);
                            return rep;
                        }
                        OathInterval idx_iv = domain_bounding_box(cur_env[inst->src2]);
                        size_t buf_p = inst->src1;
                        if (buf_p < fn->param_count) {
                            if (fn->params[buf_p].kind == PARAM_BUFFER) {
                                size_t cap = fn->params[buf_p].buffer.capacity;
                                if (idx_iv.lo < 0 || idx_iv.hi >= (int64_t)cap) {
                                    rep.is_valid = false;
                                    rep.diag = SEPE_BUFFER_OVERFLOW;
                                    snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Buffer index out of bounds");
                                    free(bb_states);
                                    return rep;
                                }
                                cur_env[inst->dst] = domain_create_single(fn->params[buf_p].buffer.elem_bounds);
                            } else if (fn->params[buf_p].kind == PARAM_SLICE) {
                                size_t len_p = fn->params[buf_p].slice.len_param_idx;
                                bool safe = (cur_dbm.m[inst->src2][len_p] <= -1 && idx_iv.lo >= 0);
                                if (!safe) {
                                    OathInterval len_iv = domain_bounding_box(cur_env[len_p]);
                                    if (idx_iv.lo >= 0 && idx_iv.hi < len_iv.lo) safe = true;
                                }
                                if (!safe) {
                                    rep.is_valid = false;
                                    rep.diag = SEPE_BUFFER_OVERFLOW;
                                    snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Slice index not provably < dynamic length");
                                    free(bb_states);
                                    return rep;
                                }
                                cur_env[inst->dst] = domain_create_single(fn->params[buf_p].slice.elem_bounds);
                            }
                        }
                    }
                    break;

                case OIR_OP_STORE_ARRAY:
                    if (inst->src1 < total_slots && inst->src2 < total_slots && inst->dst < total_slots) {
                        if (cur_taint[inst->src2]) {
                            rep.is_valid = false;
                            rep.diag = SEPE_TAINTED_INDEX;
                            snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Tainted register used as array index");
                            free(bb_states);
                            return rep;
                        }
                        OathInterval idx_iv = domain_bounding_box(cur_env[inst->src2]);
                        size_t buf_p = inst->src1;
                        if (buf_p < fn->param_count) {
                            if (fn->params[buf_p].kind == PARAM_BUFFER) {
                                size_t cap = fn->params[buf_p].buffer.capacity;
                                if (idx_iv.lo < 0 || idx_iv.hi >= (int64_t)cap) {
                                    rep.is_valid = false;
                                    rep.diag = SEPE_BUFFER_OVERFLOW;
                                    snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Write index exceeds capacity");
                                    free(bb_states);
                                    return rep;
                                }
                            } else if (fn->params[buf_p].kind == PARAM_SLICE) {
                                size_t len_p = fn->params[buf_p].slice.len_param_idx;
                                bool safe = (cur_dbm.m[inst->src2][len_p] <= -1 && idx_iv.lo >= 0);
                                if (!safe) {
                                    OathInterval len_iv = domain_bounding_box(cur_env[len_p]);
                                    if (idx_iv.lo >= 0 && idx_iv.hi < len_iv.lo) safe = true;
                                }
                                if (!safe) {
                                    rep.is_valid = false;
                                    rep.diag = SEPE_BUFFER_OVERFLOW;
                                    snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Slice write index not provably < dynamic length");
                                    free(bb_states);
                                    return rep;
                                }
                            }
                        }
                    }
                    break;

                case OIR_OP_RET:
                    if (inst->src1 < total_slots) {
                        if (fn->returns_resource && cur_res[inst->src1] == RES_OWNED) {
                            cur_res[inst->src1] = RES_RETURNED;
                        }
                        for (size_t r = 0; r < total_slots; ++r) {
                            if (cur_res[r] == RES_OWNED) {
                                rep.is_valid = false;
                                rep.diag = SEPE_RESOURCE_LEAK;
                                snprintf(rep.counter_example, sizeof(rep.counter_example), "OIR: Linear resource leak detected on return");
                                free(bb_states);
                                return rep;
                            }
                        }
                        OathInterval out_iv = domain_bounding_box(cur_env[inst->src1]);
                        if (out_iv.lo < fn->postcondition.lo || out_iv.hi > fn->postcondition.hi) {
                            rep.is_valid = false;
                            rep.diag = SEPE_CONTRACT_VIOLATION;
                            snprintf(rep.counter_example, sizeof(rep.counter_example),
                                     "OIR: Return value [%" PRId64 ", %" PRId64 "] violates postcondition", out_iv.lo, out_iv.hi);
                            free(bb_states);
                            return rep;
                        }
                    }
                    break;

                case OIR_OP_JMP: {
                    uint32_t tgt = inst->target_bb;
                    if (tgt < fn->block_count) {
                        if (!bb_states[tgt].reached) {
                            for (size_t r = 0; r < total_slots; ++r) {
                                bb_states[tgt].env[r] = cur_env[r];
                                bb_states[tgt].taint[r] = cur_taint[r];
                                bb_states[tgt].res[r] = cur_res[r];
                            }
                            bb_states[tgt].dbm = cur_dbm;
                            bb_states[tgt].reached = true;
                        } else {
                            for (size_t r = 0; r < total_slots; ++r) {
                                bb_states[tgt].env[r] = domain_join(bb_states[tgt].env[r], cur_env[r]);
                                bb_states[tgt].taint[r] = bb_states[tgt].taint[r] || cur_taint[r];
                            }
                            dbm_join(&bb_states[tgt].dbm, &bb_states[tgt].dbm, &cur_dbm, total_slots);
                        }
                    }
                    break;
                }

                case OIR_OP_BR: {
                    uint32_t t_tgt = inst->true_bb;
                    uint32_t f_tgt = inst->false_bb;

                    if (t_tgt < fn->block_count) {
                        for (size_t r = 0; r < total_slots; ++r) {
                            bb_states[t_tgt].env[r] = cur_env[r];
                            bb_states[t_tgt].taint[r] = cur_taint[r];
                            bb_states[t_tgt].res[r] = cur_res[r];
                        }
                        bb_states[t_tgt].dbm = cur_dbm;
                        for (size_t t = 0; t < inst->cond.count; ++t) {
                            env_apply_atomic_condition(bb_states[t_tgt].env, bb_states[t_tgt].taint, inst->cond.terms[t], false);
                            dbm_apply_atomic_condition(&bb_states[t_tgt].dbm, inst->cond.terms[t], false, total_slots);
                        }
                        bb_states[t_tgt].reached = true;
                    }

                    if (f_tgt < fn->block_count) {
                        for (size_t r = 0; r < total_slots; ++r) {
                            bb_states[f_tgt].env[r] = cur_env[r];
                            bb_states[f_tgt].taint[r] = cur_taint[r];
                            bb_states[f_tgt].res[r] = cur_res[r];
                        }
                        bb_states[f_tgt].dbm = cur_dbm;
                        if (inst->cond.count == 1) {
                            env_apply_atomic_condition(bb_states[f_tgt].env, bb_states[f_tgt].taint, inst->cond.terms[0], true);
                            dbm_apply_atomic_condition(&bb_states[f_tgt].dbm, inst->cond.terms[0], true, total_slots);
                        }
                        bb_states[f_tgt].reached = true;
                    }
                    break;
                }

                default:
                    break;
            }

            inst = inst->next;
        }
    }

    free(bb_states);
    return rep;
}