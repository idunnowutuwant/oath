#include "oath/sepe.h"
#include <stdio.h>
#include <string.h>

#define OATH_TOTAL_SLOTS 64
#define DBM_INF (INT64_MAX / 4)
#define OATH_MAX_UNROLL_STEPS 64

typedef enum {
    RES_NONE = 0,
    RES_ALLOCATED,
    RES_FREED
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

static size_t get_total_slots(const OathFunction* fn, const OathModule* mod) {
    return get_local_slot(fn, mod, fn->local_count);
}

static OathDomain domain_create_single(OathInterval iv) {
    OathDomain d;
    d.items[0] = iv;
    d.count = 1;
    return d;
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

static OathDomain domain_intersect_interval(OathDomain d, OathInterval filter) {
    OathDomain out;
    out.count = 0;

    for (size_t i = 0; i < d.count; ++i) {
        int64_t n_lo = d.items[i].lo > filter.lo ? d.items[i].lo : filter.lo;
        int64_t n_hi = d.items[i].hi < filter.hi ? d.items[i].hi : filter.hi;

        if (n_lo <= n_hi) {
            if (out.count < OATH_MAX_DISJOINT_INTERVALS) {
                out.items[out.count++] = oath_interval_create(n_lo, n_hi);
            }
        }
    }
    return out;
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
        case COND_EQ:
            return domain_intersect_interval(d, oath_interval_create(val, val));

        case COND_LT:
            if (val == INT64_MIN) { OathDomain empty = { .count = 0 }; return empty; }
            return domain_intersect_interval(d, oath_interval_create(INT64_MIN, val - 1));

        case COND_LE:
            return domain_intersect_interval(d, oath_interval_create(INT64_MIN, val));

        case COND_GT:
            if (val == INT64_MAX) { OathDomain empty = { .count = 0 }; return empty; }
            return domain_intersect_interval(d, oath_interval_create(val + 1, INT64_MAX));

        case COND_GE:
            return domain_intersect_interval(d, oath_interval_create(val, INT64_MAX));

        case COND_NE: {
            OathDomain out;
            out.count = 0;
            for (size_t i = 0; i < d.count; ++i) {
                OathInterval iv = d.items[i];
                if (!oath_interval_contains(iv, val)) {
                    if (out.count < OATH_MAX_DISJOINT_INTERVALS) out.items[out.count++] = iv;
                } else {
                    if (iv.lo < val && out.count < OATH_MAX_DISJOINT_INTERVALS) {
                        out.items[out.count++] = oath_interval_create(iv.lo, val - 1);
                    }
                    if (iv.hi > val && out.count < OATH_MAX_DISJOINT_INTERVALS) {
                        out.items[out.count++] = oath_interval_create(val + 1, iv.hi);
                    }
                }
            }
            return out;
        }
    }
    return d;
}

static void env_apply_atomic_condition(OathDomain env[OATH_TOTAL_SLOTS], OathAtomicCond cond, bool invert) {
    if (!cond.rhs_is_slot) {
        env[cond.lhs_slot] = domain_apply_atomic_condition(env[cond.lhs_slot], cond, invert);
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

    OathInterval v_iv = (env[v].count > 0) ? env[v].items[0] : oath_interval_create(INT64_MIN, INT64_MAX);

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
        case COND_LE:
            if (0 < dbm->m[u][v]) dbm->m[u][v] = 0;
            break;
        case COND_LT:
            if (-1 < dbm->m[u][v]) dbm->m[u][v] = -1;
            break;
        case COND_GE:
            if (0 < dbm->m[v][u]) dbm->m[v][u] = 0;
            break;
        case COND_GT:
            if (-1 < dbm->m[v][u]) dbm->m[v][u] = -1;
            break;
        case COND_EQ:
            if (0 < dbm->m[u][v]) dbm->m[u][v] = 0;
            if (0 < dbm->m[v][u]) dbm->m[v][u] = 0;
            break;
        case COND_NE:
            break;
    }

    return dbm_close(dbm, n);
}

static size_t expr_to_slot(const OathExpr* e, const OathFunction* fn, const OathModule* mod) {
    if (e->type == EXPR_VAR) {
        if (e->var_idx < fn->param_count) return get_param_slot_base(fn, mod, e->var_idx);
        return get_local_slot(fn, mod, e->var_idx - fn->param_count);
    }
    if (e->type == EXPR_FIELD) {
        size_t base = 0;
        if (e->var_idx < fn->param_count) base = get_param_slot_base(fn, mod, e->var_idx);
        else base = get_local_slot(fn, mod, e->var_idx - fn->param_count);
        return base + e->field_idx;
    }
    return (size_t)-1;
}

static size_t map_target_slot_to_caller_slot(size_t target_slot,
                                            const OathFunction* target,
                                            const OathExpr* call_expr,
                                            const OathFunction* caller,
                                            const OathModule* mod) {
    size_t cur = 0;
    for (size_t p = 0; p < target->param_count; ++p) {
        if (target->params[p].kind == PARAM_STRUCT) {
            size_t fc = mod->structs[target->params[p].struct_def_idx].field_count;
            if (target_slot >= cur && target_slot < cur + fc) {
                size_t f_offset = target_slot - cur;
                size_t caller_base = expr_to_slot(call_expr->args[p], caller, mod);
                return caller_base + f_offset;
            }
            cur += fc;
        } else {
            if (target_slot == cur) {
                return expr_to_slot(call_expr->args[p], caller, mod);
            }
            cur++;
        }
    }
    return (size_t)-1;
}

static OathInterval eval_expr_under_env(const OathExpr* expr,
                                       const OathInterval* env,
                                       const OathDBM* dbm,
                                       const OathFunction* fn,
                                       const OathModule* mod,
                                       SepeReport* rep) {
    if (!rep->is_valid) return oath_interval_create(0, 0);

    switch (expr->type) {
        case EXPR_LITERAL:
            return oath_interval_create(expr->literal, expr->literal);
        case EXPR_VAR: {
            size_t slot = expr_to_slot(expr, fn, mod);
            return env[slot];
        }
        case EXPR_FIELD: {
            size_t slot = expr_to_slot(expr, fn, mod);
            return env[slot];
        }
        case EXPR_ALLOC: {
            OathInterval sz = eval_expr_under_env(expr->lhs, env, dbm, fn, mod, rep);
            if (!rep->is_valid) return oath_interval_create(0, 0);
            if (sz.lo <= 0) {
                rep->is_valid = false;
                rep->diag = SEPE_CONTRACT_VIOLATION;
                snprintf(rep->counter_example, sizeof(rep->counter_example), "Allocation size must be positive (> 0)");
                return oath_interval_create(0, 0);
            }
            return oath_interval_create(1, INT64_MAX);
        }
        case EXPR_ENUM_CONSTRUCT: {
            const OathEnumDef* ed = &mod->enums[expr->enum_construct.enum_idx];
            OathInterval req_b = ed->variants[expr->enum_construct.variant_idx].bounds;
            OathInterval p_iv = eval_expr_under_env(expr->enum_construct.payload, env, dbm, fn, mod, rep);
            if (!rep->is_valid) return oath_interval_create(0, 0);

            if (p_iv.lo < req_b.lo || p_iv.hi > req_b.hi) {
                rep->is_valid = false;
                rep->diag = SEPE_CONTRACT_VIOLATION;
                snprintf(rep->counter_example, sizeof(rep->counter_example),
                         "Enum '%s::%s' payload [%" PRId64 ", %" PRId64 "] violates variant contract [%" PRId64 ", %" PRId64 "]",
                         ed->name, ed->variants[expr->enum_construct.variant_idx].name,
                         p_iv.lo, p_iv.hi, req_b.lo, req_b.hi);
                return oath_interval_create(0, 0);
            }
            return p_iv;
        }
        case EXPR_CALL: {
            const OathFunction* target = NULL;
            for (size_t i = 0; i < mod->function_count; ++i) {
                if (strcmp(mod->functions[i].name, expr->call_name) == 0) {
                    target = &mod->functions[i];
                    break;
                }
            }
            if (!target) {
                rep->is_valid = false;
                rep->diag = SEPE_UNKNOWN_FUNCTION;
                snprintf(rep->counter_example, sizeof(rep->counter_example),
                         "Call to undeclared function '%s'", expr->call_name);
                return oath_interval_create(0, 0);
            }
            if (target->param_count != expr->arg_count) {
                rep->is_valid = false;
                rep->diag = SEPE_CALL_CONTRACT_VIOLATION;
                snprintf(rep->counter_example, sizeof(rep->counter_example),
                         "Function '%s' requires %zu args, got %zu",
                         expr->call_name, target->param_count, expr->arg_count);
                return oath_interval_create(0, 0);
            }

            for (size_t d = 0; d < target->disjoint_count; ++d) {
                size_t p_a = target->disjoint_contracts[d].param_a;
                size_t p_b = target->disjoint_contracts[d].param_b;

                const OathExpr* arg_a = expr->args[p_a];
                const OathExpr* arg_b = expr->args[p_b];

                if (arg_a->type == EXPR_VAR && arg_b->type == EXPR_VAR) {
                    if (arg_a->var_idx == arg_b->var_idx) {
                        rep->is_valid = false;
                        rep->diag = SEPE_ALIASING_VIOLATION;
                        snprintf(rep->counter_example, sizeof(rep->counter_example),
                                 "Memory aliasing detected: arguments '%s' and '%s' both reference buffer '%s'",
                                 target->params[p_a].name, target->params[p_b].name,
                                 (arg_a->var_idx < fn->param_count) ? fn->params[arg_a->var_idx].name : fn->locals[arg_a->var_idx - fn->param_count].name);
                        return oath_interval_create(0, 0);
                    }
                }
            }

            for (size_t i = 0; i < expr->arg_count; ++i) {
                if (target->params[i].kind == PARAM_STRUCT) {
                    const OathStructDef* sd = &mod->structs[target->params[i].struct_def_idx];
                    size_t caller_base = expr_to_slot(expr->args[i], fn, mod);

                    for (size_t f = 0; f < sd->field_count; ++f) {
                        OathInterval f_req = sd->fields[f].bounds;
                        OathInterval f_cur = env[caller_base + f];
                        if (f_cur.lo < f_req.lo || f_cur.hi > f_req.hi) {
                            rep->is_valid = false;
                            rep->diag = SEPE_CALL_CONTRACT_VIOLATION;
                            snprintf(rep->counter_example, sizeof(rep->counter_example),
                                     "Call '%s' field '%s': caller [%" PRId64 ", %" PRId64 "] out of contract [%" PRId64 ", %" PRId64 "]",
                                     expr->call_name, sd->fields[f].name,
                                     f_cur.lo, f_cur.hi, f_req.lo, f_req.hi);
                            return oath_interval_create(0, 0);
                        }
                    }
                } else {
                    OathInterval arg_iv = eval_expr_under_env(expr->args[i], env, dbm, fn, mod, rep);
                    if (!rep->is_valid) return oath_interval_create(0, 0);

                    OathInterval req_iv;
                    if (target->params[i].kind == PARAM_BUFFER) req_iv = target->params[i].buffer.elem_bounds;
                    else if (target->params[i].kind == PARAM_SLICE) req_iv = target->params[i].slice.elem_bounds;
                    else req_iv = target->params[i].scalar_bounds;

                    if (arg_iv.lo < req_iv.lo || arg_iv.hi > req_iv.hi) {
                        rep->is_valid = false;
                        rep->diag = SEPE_CALL_CONTRACT_VIOLATION;
                        snprintf(rep->counter_example, sizeof(rep->counter_example),
                                 "Call '%s' arg '%s': caller [%" PRId64 ", %" PRId64 "] out of contract [%" PRId64 ", %" PRId64 "]",
                                 expr->call_name, target->params[i].name,
                                 arg_iv.lo, arg_iv.hi, req_iv.lo, req_iv.hi);
                        return oath_interval_create(0, 0);
                    }
                }
            }

            for (size_t r = 0; r < target->requires_count; ++r) {
                OathCondition req_cond = target->requires_clauses[r];
                for (size_t t = 0; t < req_cond.count; ++t) {
                    OathAtomicCond req = req_cond.terms[t];
                    size_t caller_u = map_target_slot_to_caller_slot(req.lhs_slot, target, expr, fn, mod);

                    if (req.rhs_is_slot) {
                        size_t caller_v = map_target_slot_to_caller_slot(req.rhs.rhs_slot, target, expr, fn, mod);
                        if (req.op == COND_GE) {
                            if (dbm->m[caller_v][caller_u] > 0) {
                                rep->is_valid = false;
                                rep->diag = SEPE_CALL_CONTRACT_VIOLATION;
                                snprintf(rep->counter_example, sizeof(rep->counter_example),
                                         "Caller cannot prove 'requires' condition of '%s'", target->name);
                                return oath_interval_create(0, 0);
                            }
                        } else if (req.op == COND_GT) {
                            if (dbm->m[caller_v][caller_u] >= 0) {
                                rep->is_valid = false;
                                rep->diag = SEPE_CALL_CONTRACT_VIOLATION;
                                snprintf(rep->counter_example, sizeof(rep->counter_example),
                                         "Caller cannot prove 'requires' condition of '%s'", target->name);
                                return oath_interval_create(0, 0);
                            }
                        }
                    } else {
                        OathInterval u_iv = env[caller_u];
                        if (req.op == COND_EQ) {
                            if (!(u_iv.lo == req.rhs.const_val && u_iv.hi == req.rhs.const_val)) {
                                rep->is_valid = false;
                                rep->diag = SEPE_CALL_CONTRACT_VIOLATION;
                                snprintf(rep->counter_example, sizeof(rep->counter_example),
                                         "Caller cannot prove 'requires' equality of '%s'", target->name);
                                return oath_interval_create(0, 0);
                            }
                        }
                    }
                }
            }

            return target->postcondition;
        }
        case EXPR_INDEX: {
            OathInterval idx_iv = eval_expr_under_env(expr->rhs, env, dbm, fn, mod, rep);
            if (!rep->is_valid) return oath_interval_create(0, 0);

            size_t buf_idx = expr->lhs->var_idx;

            if (fn->params[buf_idx].kind == PARAM_BUFFER) {
                size_t cap = fn->params[buf_idx].buffer.capacity;
                if (idx_iv.lo < 0 || idx_iv.hi >= (int64_t)cap) {
                    rep->is_valid = false;
                    rep->diag = SEPE_BUFFER_OVERFLOW;
                    snprintf(rep->counter_example, sizeof(rep->counter_example),
                             "Index [%" PRId64 ", %" PRId64 "] violates buffer '%s' bounds [0, %zu]",
                             idx_iv.lo, idx_iv.hi, fn->params[buf_idx].name, cap - 1);
                    return oath_interval_create(0, 0);
                }
                return fn->params[buf_idx].buffer.elem_bounds;
            } else if (fn->params[buf_idx].kind == PARAM_SLICE) {
                size_t len_param = fn->params[buf_idx].slice.len_param_idx;
                size_t len_slot = get_param_slot_base(fn, mod, len_param);
                size_t idx_slot = expr_to_slot(expr->rhs, fn, mod);

                bool safe = false;
                if (idx_slot != (size_t)-1) {
                    if (dbm->m[idx_slot][len_slot] <= -1 && idx_iv.lo >= 0) safe = true;
                }
                if (!safe) {
                    OathInterval len_iv = env[len_slot];
                    if (idx_iv.lo >= 0 && idx_iv.hi < len_iv.lo) safe = true;
                }
                if (!safe) {
                    rep->is_valid = false;
                    rep->diag = SEPE_BUFFER_OVERFLOW;
                    snprintf(rep->counter_example, sizeof(rep->counter_example),
                             "Index cannot be mathematically proven < dynamic length '%s'", fn->params[len_param].name);
                    return oath_interval_create(0, 0);
                }

                return fn->params[buf_idx].slice.elem_bounds;
            }
            return oath_interval_create(0, 0);
        }
        case EXPR_ADD: {
            OathInterval l = eval_expr_under_env(expr->lhs, env, dbm, fn, mod, rep);
            OathInterval r = eval_expr_under_env(expr->rhs, env, dbm, fn, mod, rep);
            if (!rep->is_valid) return oath_interval_create(0, 0);

            bool trap = false;
            OathInterval out = oath_interval_add(l, r, &trap);
            if (OATH_UNLIKELY(trap)) {
                rep->is_valid = false;
                rep->diag = SEPE_TRAP_OVERFLOW;
                snprintf(rep->counter_example, sizeof(rep->counter_example),
                         "[%" PRId64 ", %" PRId64 "] + [%" PRId64 ", %" PRId64 "]", l.lo, l.hi, r.lo, r.hi);
            }
            return out;
        }
        case EXPR_SUB: {
            OathInterval l = eval_expr_under_env(expr->lhs, env, dbm, fn, mod, rep);
            OathInterval r = eval_expr_under_env(expr->rhs, env, dbm, fn, mod, rep);
            if (!rep->is_valid) return oath_interval_create(0, 0);

            bool trap = false;
            OathInterval out = oath_interval_sub(l, r, &trap);
            if (OATH_UNLIKELY(trap)) {
                rep->is_valid = false;
                rep->diag = SEPE_TRAP_OVERFLOW;
                snprintf(rep->counter_example, sizeof(rep->counter_example),
                         "[%" PRId64 ", %" PRId64 "] - [%" PRId64 ", %" PRId64 "]", l.lo, l.hi, r.lo, r.hi);
                return out;
            }

            size_t u = expr_to_slot(expr->lhs, fn, mod);
            size_t v = expr_to_slot(expr->rhs, fn, mod);

            if (u != (size_t)-1 && v != (size_t)-1) {
                if (dbm->m[v][u] != DBM_INF) {
                    int64_t rel_lo = -dbm->m[v][u];
                    if (rel_lo > out.lo) out.lo = rel_lo;
                }
                if (dbm->m[u][v] != DBM_INF) {
                    int64_t rel_hi = dbm->m[u][v];
                    if (rel_hi < out.hi) out.hi = rel_hi;
                }
            }

            return out;
        }
        case EXPR_MUL: {
            OathInterval l = eval_expr_under_env(expr->lhs, env, dbm, fn, mod, rep);
            OathInterval r = eval_expr_under_env(expr->rhs, env, dbm, fn, mod, rep);
            if (!rep->is_valid) return oath_interval_create(0, 0);

            bool trap = false;
            OathInterval out = oath_interval_mul(l, r, &trap);
            if (OATH_UNLIKELY(trap)) {
                rep->is_valid = false;
                rep->diag = SEPE_TRAP_OVERFLOW;
                snprintf(rep->counter_example, sizeof(rep->counter_example),
                         "[%" PRId64 ", %" PRId64 "] * [%" PRId64 ", %" PRId64 "]", l.lo, l.hi, r.lo, r.hi);
            }
            return out;
        }
        case EXPR_DIV: {
            OathInterval l = eval_expr_under_env(expr->lhs, env, dbm, fn, mod, rep);
            OathInterval r = eval_expr_under_env(expr->rhs, env, dbm, fn, mod, rep);
            if (!rep->is_valid) return oath_interval_create(0, 0);

            bool div_zero = false;
            bool de_trap = false;
            OathInterval out = oath_interval_div(l, r, &div_zero, &de_trap);

            if (OATH_UNLIKELY(div_zero)) {
                rep->is_valid = false;
                rep->diag = SEPE_TRAP_DIV_ZERO;
                snprintf(rep->counter_example, sizeof(rep->counter_example),
                         "Divisor [%" PRId64 ", %" PRId64 "] intersects 0", r.lo, r.hi);
            } else if (OATH_UNLIKELY(de_trap)) {
                rep->is_valid = false;
                rep->diag = SEPE_TRAP_X86_DE;
                snprintf(rep->counter_example, sizeof(rep->counter_example),
                         "LHS reaches INT64_MIN and RHS contains -1");
            }
            return out;
        }
    }
    return oath_interval_create(0, 0);
}

static bool explore_stmt_sequence(const OathStmt* stmt,
                                  OathDomain env[OATH_TOTAL_SLOTS],
                                  ResourceState res[OATH_TOTAL_SLOTS],
                                  OathDBM dbm,
                                  const OathFunction* fn,
                                  const OathModule* mod,
                                  SepeReport* rep) {
    if (!rep->is_valid || stmt == NULL) return true;

    size_t total_slots = get_total_slots(fn, mod);

    switch (stmt->type) {
        case STMT_MATCH: {
            const OathEnumDef* ed = &mod->enums[stmt->as.match_stmt.enum_idx];

            // 1. Exhaustiveness Check
            bool covered[OATH_MAX_VARIANTS] = {false};
            for (size_t a = 0; a < stmt->as.match_stmt.arm_count; ++a) {
                size_t v_idx = stmt->as.match_stmt.arms[a].variant_idx;
                if (v_idx < ed->variant_count) covered[v_idx] = true;
            }

            for (size_t v = 0; v < ed->variant_count; ++v) {
                if (!covered[v]) {
                    rep->is_valid = false;
                    rep->diag = SEPE_NON_EXHAUSTIVE_MATCH;
                    snprintf(rep->counter_example, sizeof(rep->counter_example),
                             "Non-exhaustive match on enum '%s': variant '%s' is not handled", ed->name, ed->variants[v].name);
                    return false;
                }
            }

            // 2. Branch exploration for each match arm
            OathDomain merged_env[OATH_TOTAL_SLOTS];
            ResourceState merged_res[OATH_TOTAL_SLOTS];
            bool first_arm_flowed = false;

            for (size_t a = 0; a < stmt->as.match_stmt.arm_count; ++a) {
                OathDomain arm_env[OATH_TOTAL_SLOTS];
                ResourceState arm_res[OATH_TOTAL_SLOTS];
                for (size_t i = 0; i < total_slots; ++i) { arm_env[i] = env[i]; arm_res[i] = res[i]; }

                size_t b_slot = get_local_slot(fn, mod, stmt->as.match_stmt.arms[a].bound_var_idx - fn->param_count);
                size_t v_idx = stmt->as.match_stmt.arms[a].variant_idx;
                arm_env[b_slot] = domain_create_single(ed->variants[v_idx].bounds);

                bool arm_flows = explore_stmt_sequence(stmt->as.match_stmt.arms[a].body, arm_env, arm_res, dbm, fn, mod, rep);
                if (!rep->is_valid) return false;

                if (arm_flows) {
                    if (!first_arm_flowed) {
                        for (size_t i = 0; i < total_slots; ++i) {
                            merged_env[i] = arm_env[i];
                            merged_res[i] = arm_res[i];
                        }
                        first_arm_flowed = true;
                    } else {
                        for (size_t i = 0; i < total_slots; ++i) {
                            merged_env[i] = domain_join(merged_env[i], arm_env[i]);
                        }
                    }
                }
            }

            if (!first_arm_flowed) return false;
            return explore_stmt_sequence(stmt->next, merged_env, merged_res, dbm, fn, mod, rep);
        }

        case STMT_LET:
        case STMT_ASSIGN: {
            size_t v_idx = (stmt->type == STMT_LET) ? stmt->as.let_stmt.var_idx : stmt->as.assign_stmt.var_idx;
            const OathExpr* target_expr = (stmt->type == STMT_LET) ? stmt->as.let_stmt.init_expr : stmt->as.assign_stmt.expr;

            OathInterval point_env[OATH_TOTAL_SLOTS];
            for (size_t i = 0; i < total_slots; ++i) {
                point_env[i] = (env[i].count > 0) ? env[i].items[0] : oath_interval_create(0, 0);
            }

            OathInterval val_iv = eval_expr_under_env(target_expr, point_env, &dbm, fn, mod, rep);
            if (!rep->is_valid) return false;

            size_t slot = get_local_slot(fn, mod, v_idx - fn->param_count);
            env[slot] = domain_create_single(val_iv);

            if (stmt->type == STMT_LET && stmt->as.let_stmt.is_linear_resource) {
                res[slot] = RES_ALLOCATED;
            }

            return explore_stmt_sequence(stmt->next, env, res, dbm, fn, mod, rep);
        }

        case STMT_FREE: {
            size_t slot = get_local_slot(fn, mod, stmt->as.free_stmt.var_idx - fn->param_count);
            if (res[slot] == RES_FREED) {
                rep->is_valid = false;
                rep->diag = SEPE_DOUBLE_FREE;
                snprintf(rep->counter_example, sizeof(rep->counter_example),
                         "Double free detected on linear resource '%s'", fn->locals[stmt->as.free_stmt.var_idx - fn->param_count].name);
                return false;
            }
            res[slot] = RES_FREED;
            return explore_stmt_sequence(stmt->next, env, res, dbm, fn, mod, rep);
        }

        case STMT_ARRAY_SET: {
            size_t buf_idx = stmt->as.array_set.buf_var_idx;
            OathInterval point_env[OATH_TOTAL_SLOTS];
            for (size_t i = 0; i < total_slots; ++i) {
                point_env[i] = (env[i].count > 0) ? env[i].items[0] : oath_interval_create(0, 0);
            }

            OathInterval idx_iv = eval_expr_under_env(stmt->as.array_set.idx_expr, point_env, &dbm, fn, mod, rep);
            if (!rep->is_valid) return false;

            OathInterval elem_bounds;
            if (fn->params[buf_idx].kind == PARAM_BUFFER) {
                size_t cap = fn->params[buf_idx].buffer.capacity;
                if (idx_iv.lo < 0 || idx_iv.hi >= (int64_t)cap) {
                    rep->is_valid = false;
                    rep->diag = SEPE_BUFFER_OVERFLOW;
                    snprintf(rep->counter_example, sizeof(rep->counter_example),
                             "Write index [%" PRId64 ", %" PRId64 "] violates buffer '%s' bounds [0, %zu]",
                             idx_iv.lo, idx_iv.hi, fn->params[buf_idx].name, cap - 1);
                    return false;
                }
                elem_bounds = fn->params[buf_idx].buffer.elem_bounds;
            } else if (fn->params[buf_idx].kind == PARAM_SLICE) {
                size_t len_param = fn->params[buf_idx].slice.len_param_idx;
                size_t len_slot = get_param_slot_base(fn, mod, len_param);
                size_t idx_slot = expr_to_slot(stmt->as.array_set.idx_expr, fn, mod);

                bool safe = false;
                if (idx_slot != (size_t)-1) {
                    if (dbm.m[idx_slot][len_slot] <= -1 && idx_iv.lo >= 0) safe = true;
                }
                if (!safe) {
                    OathInterval len_iv = env[len_slot].count > 0 ? env[len_slot].items[0] : oath_interval_create(0, 0);
                    if (idx_iv.lo >= 0 && idx_iv.hi < len_iv.lo) safe = true;
                }
                if (!safe) {
                    rep->is_valid = false;
                    rep->diag = SEPE_BUFFER_OVERFLOW;
                    snprintf(rep->counter_example, sizeof(rep->counter_example),
                             "Write index cannot be proven < dynamic length '%s'", fn->params[len_param].name);
                    return false;
                }
                elem_bounds = fn->params[buf_idx].slice.elem_bounds;
            }

            OathInterval val_iv = eval_expr_under_env(stmt->as.array_set.val_expr, point_env, &dbm, fn, mod, rep);
            if (!rep->is_valid) return false;

            if (val_iv.lo < elem_bounds.lo || val_iv.hi > elem_bounds.hi) {
                rep->is_valid = false;
                rep->diag = SEPE_CONTRACT_VIOLATION;
                snprintf(rep->counter_example, sizeof(rep->counter_example),
                         "Value [%" PRId64 ", %" PRId64 "] written to '%s' violates element bounds [%" PRId64 ", %" PRId64 "]",
                         val_iv.lo, val_iv.hi, fn->params[buf_idx].name, elem_bounds.lo, elem_bounds.hi);
                return false;
            }

            return explore_stmt_sequence(stmt->next, env, res, dbm, fn, mod, rep);
        }

        case STMT_RETURN: {
            rep->total_paths_explored++;

            for (size_t l = 0; l < fn->local_count; ++l) {
                size_t slot = get_local_slot(fn, mod, l);
                if (res[slot] == RES_ALLOCATED) {
                    rep->is_valid = false;
                    rep->diag = SEPE_RESOURCE_LEAK;
                    snprintf(rep->counter_example, sizeof(rep->counter_example),
                             "Resource leak: linear resource '%s' was allocated but not freed before return", fn->locals[l].name);
                    return false;
                }
            }

            OathInterval point_env[OATH_TOTAL_SLOTS];
            for (size_t i = 0; i < total_slots; ++i) {
                point_env[i] = (env[i].count > 0) ? env[i].items[0] : oath_interval_create(0, 0);
            }

            OathInterval out = eval_expr_under_env(stmt->as.ret.expr, point_env, &dbm, fn, mod, rep);
            if (!rep->is_valid) return false;

            // 1. Check interval postcondition
            if (out.lo < fn->postcondition.lo || out.hi > fn->postcondition.hi) {
                rep->is_valid = false;
                rep->diag = SEPE_CONTRACT_VIOLATION;
                snprintf(rep->counter_example, sizeof(rep->counter_example),
                         "Inferred [%" PRId64 ", %" PRId64 "] violates contract [%" PRId64 ", %" PRId64 "]",
                         out.lo, out.hi, fn->postcondition.lo, fn->postcondition.hi);
                return false;
            }

            // 2. [FCE] Check relational 'ensures' clauses!
            for (size_t e = 0; e < fn->ensures_count; ++e) {
                OathCondition ens_cond = fn->ensures_clauses[e];
                for (size_t t = 0; t < ens_cond.count; ++t) {
                    OathAtomicCond ens = ens_cond.terms[t];
                    if (ens.lhs_is_return) {
                        if (!ens.rhs_is_slot) {
                            int64_t target = ens.rhs.const_val;
                            if (ens.op == COND_GE && out.lo < target) {
                                rep->is_valid = false;
                                rep->diag = SEPE_ENSURES_VIOLATION;
                                snprintf(rep->counter_example, sizeof(rep->counter_example),
                                         "Return value [%" PRId64 ", %" PRId64 "] violates 'ensures return >= %" PRId64 "'",
                                         out.lo, out.hi, target);
                                return false;
                            } else if (ens.op == COND_GT && out.lo <= target) {
                                rep->is_valid = false;
                                rep->diag = SEPE_ENSURES_VIOLATION;
                                snprintf(rep->counter_example, sizeof(rep->counter_example),
                                         "Return value [%" PRId64 ", %" PRId64 "] violates 'ensures return > %" PRId64 "'",
                                         out.lo, out.hi, target);
                                return false;
                            }
                        }
                    }
                }
            }

            return false;
        }

        case STMT_IF_ELSE: {
            OathDomain then_env[OATH_TOTAL_SLOTS];
            ResourceState then_res[OATH_TOTAL_SLOTS];
            for (size_t i = 0; i < total_slots; ++i) { then_env[i] = env[i]; then_res[i] = res[i]; }
            OathDBM then_dbm = dbm;
            bool then_ok = true;

            for (size_t t = 0; t < stmt->as.if_else.cond.count; ++t) {
                OathAtomicCond term = stmt->as.if_else.cond.terms[t];
                env_apply_atomic_condition(then_env, term, false);
                if (then_env[term.lhs_slot].count == 0 || !dbm_apply_atomic_condition(&then_dbm, term, false, total_slots)) {
                    then_ok = false;
                    break;
                }
            }

            bool then_flows = false;
            if (!then_ok) {
                rep->infeasible_paths_pruned++;
            } else {
                then_flows = explore_stmt_sequence(stmt->as.if_else.then_branch, then_env, then_res, then_dbm, fn, mod, rep);
                if (!rep->is_valid) return false;
            }

            OathDomain else_env[OATH_TOTAL_SLOTS];
            ResourceState else_res[OATH_TOTAL_SLOTS];
            for (size_t i = 0; i < total_slots; ++i) { else_env[i] = env[i]; else_res[i] = res[i]; }
            OathDBM else_dbm = dbm;

            if (stmt->as.if_else.cond.count == 1) {
                OathAtomicCond term = stmt->as.if_else.cond.terms[0];
                env_apply_atomic_condition(else_env, term, true);
                dbm_apply_atomic_condition(&else_dbm, term, true, total_slots);
            }

            bool else_flows = explore_stmt_sequence(stmt->as.if_else.else_branch, else_env, else_res, else_dbm, fn, mod, rep);
            if (!rep->is_valid) return false;

            if (!then_flows && !else_flows) return false;

            OathDomain joined_env[OATH_TOTAL_SLOTS];
            ResourceState joined_res[OATH_TOTAL_SLOTS];
            for (size_t i = 0; i < total_slots; ++i) {
                if (then_flows && else_flows) {
                    joined_env[i] = domain_join(then_env[i], else_env[i]);
                    joined_res[i] = then_res[i];
                } else if (then_flows) {
                    joined_env[i] = then_env[i];
                    joined_res[i] = then_res[i];
                } else {
                    joined_env[i] = else_env[i];
                    joined_res[i] = else_res[i];
                }
            }

            return explore_stmt_sequence(stmt->next, joined_env, joined_res, dbm, fn, mod, rep);
        }

        case STMT_WHILE: {
            if (stmt->as.while_loop.invariant_count == 0 && stmt->as.while_loop.cond.count == 1 && !stmt->as.while_loop.cond.terms[0].rhs_is_slot) {
                size_t c_slot = stmt->as.while_loop.cond.terms[0].lhs_slot;
                OathDomain curr_env[OATH_TOTAL_SLOTS];
                ResourceState curr_res[OATH_TOTAL_SLOTS];
                for (size_t i = 0; i < total_slots; ++i) { curr_env[i] = env[i]; curr_res[i] = res[i]; }

                for (int step = 0; step < OATH_MAX_UNROLL_STEPS; ++step) {
                    OathDomain active_env[OATH_TOTAL_SLOTS];
                    for (size_t i = 0; i < total_slots; ++i) active_env[i] = curr_env[i];
                    env_apply_atomic_condition(active_env, stmt->as.while_loop.cond.terms[0], false);

                    if (active_env[c_slot].count == 0) break;

                    explore_stmt_sequence(stmt->as.while_loop.body, active_env, curr_res, dbm, fn, mod, rep);
                    if (!rep->is_valid) return false;

                    for (size_t i = 0; i < total_slots; ++i) curr_env[i] = active_env[i];
                }

                OathDomain exit_env[OATH_TOTAL_SLOTS];
                for (size_t i = 0; i < total_slots; ++i) exit_env[i] = curr_env[i];
                env_apply_atomic_condition(exit_env, stmt->as.while_loop.cond.terms[0], true);

                return explore_stmt_sequence(stmt->next, exit_env, curr_res, dbm, fn, mod, rep);
            }

            for (size_t k = 0; k < stmt->as.while_loop.invariant_count; ++k) {
                size_t v = stmt->as.while_loop.invariants[k].var_idx;
                size_t slot = (v < fn->param_count) ? get_param_slot_base(fn, mod, v) : get_local_slot(fn, mod, v - fn->param_count);
                OathInterval req = stmt->as.while_loop.invariants[k].bound;
                OathInterval cur = (env[slot].count > 0) ? env[slot].items[0] : oath_interval_create(0, 0);

                if (cur.lo < req.lo || cur.hi > req.hi) {
                    rep->is_valid = false;
                    rep->diag = SEPE_LOOP_INIT_FAILED;
                    snprintf(rep->counter_example, sizeof(rep->counter_example),
                             "Loop invariant for '%s' fails on entry", (v < fn->param_count) ? fn->params[v].name : fn->locals[v - fn->param_count].name);
                    return false;
                }
            }

            OathDomain inv_env[OATH_TOTAL_SLOTS];
            for (size_t i = 0; i < total_slots; ++i) inv_env[i] = env[i];
            for (size_t k = 0; k < stmt->as.while_loop.invariant_count; ++k) {
                size_t v = stmt->as.while_loop.invariants[k].var_idx;
                size_t slot = (v < fn->param_count) ? get_param_slot_base(fn, mod, v) : get_local_slot(fn, mod, v - fn->param_count);
                inv_env[slot] = domain_create_single(stmt->as.while_loop.invariants[k].bound);
            }

            size_t c_slot = stmt->as.while_loop.cond.terms[0].lhs_slot;
            OathDomain body_env[OATH_TOTAL_SLOTS];
            for (size_t i = 0; i < total_slots; ++i) body_env[i] = inv_env[i];
            
            env_apply_atomic_condition(body_env, stmt->as.while_loop.cond.terms[0], false);

            OathDBM body_dbm = dbm;
            dbm_apply_atomic_condition(&body_dbm, stmt->as.while_loop.cond.terms[0], false, total_slots);

            if (body_env[c_slot].count > 0) {
                bool body_flows = explore_stmt_sequence(stmt->as.while_loop.body, body_env, res, body_dbm, fn, mod, rep);
                if (!rep->is_valid) return false;

                if (body_flows) {
                    for (size_t k = 0; k < stmt->as.while_loop.invariant_count; ++k) {
                        size_t v = stmt->as.while_loop.invariants[k].var_idx;
                        size_t slot = (v < fn->param_count) ? get_param_slot_base(fn, mod, v) : get_local_slot(fn, mod, v - fn->param_count);
                        OathInterval req = stmt->as.while_loop.invariants[k].bound;
                        OathInterval post = (body_env[slot].count > 0) ? body_env[slot].items[0] : oath_interval_create(0, 0);

                        if (post.lo < req.lo || post.hi > req.hi) {
                            rep->is_valid = false;
                            rep->diag = SEPE_LOOP_INVARIANT_BROKEN;
                            snprintf(rep->counter_example, sizeof(rep->counter_example),
                                     "Loop invariant broken after iteration: yields [%" PRId64 ", %" PRId64 "], not in [%" PRId64 ", %" PRId64 "]",
                                     post.lo, post.hi, req.lo, req.hi);
                            return false;
                        }
                    }
                }
            }

            OathDomain exit_env[OATH_TOTAL_SLOTS];
            for (size_t i = 0; i < total_slots; ++i) exit_env[i] = inv_env[i];
            env_apply_atomic_condition(exit_env, stmt->as.while_loop.cond.terms[0], true);

            OathDBM exit_dbm = dbm;
            dbm_apply_atomic_condition(&exit_dbm, stmt->as.while_loop.cond.terms[0], true, total_slots);

            return explore_stmt_sequence(stmt->next, exit_env, res, exit_dbm, fn, mod, rep);
        }
    }
    return true;
}

SepeReport oath_sepe_verify_function(const OathFunction* fn, const OathModule* mod) {
    SepeReport rep;
    rep.is_valid = true;
    rep.diag = SEPE_OK;
    rep.total_paths_explored = 0;
    rep.infeasible_paths_pruned = 0;
    rep.counter_example[0] = '\0';
    strncpy(rep.target_symbol, fn->name, sizeof(rep.target_symbol) - 1);

    if (fn->is_extern) {
        return rep;
    }

    size_t total_slots = get_total_slots(fn, mod);

    OathDomain env[OATH_TOTAL_SLOTS];
    ResourceState res[OATH_TOTAL_SLOTS];
    memset(res, 0, sizeof(res));
    size_t cur_slot = 0;

    for (size_t i = 0; i < fn->param_count; ++i) {
        if (fn->params[i].kind == PARAM_STRUCT) {
            const OathStructDef* s = &mod->structs[fn->params[i].struct_def_idx];
            for (size_t f = 0; f < s->field_count; ++f) {
                env[cur_slot++] = domain_create_single(s->fields[f].bounds);
            }
        } else if (fn->params[i].kind == PARAM_ENUM) {
            const OathEnumDef* ed = &mod->enums[fn->params[i].enum_def_idx];
            int64_t min_lo = INT64_MAX, max_hi = INT64_MIN;
            for (size_t v = 0; v < ed->variant_count; ++v) {
                if (ed->variants[v].bounds.lo < min_lo) min_lo = ed->variants[v].bounds.lo;
                if (ed->variants[v].bounds.hi > max_hi) max_hi = ed->variants[v].bounds.hi;
            }
            env[cur_slot++] = domain_create_single(oath_interval_create(min_lo, max_hi));
        } else if (fn->params[i].kind == PARAM_BUFFER) {
            env[cur_slot++] = domain_create_single(fn->params[i].buffer.elem_bounds);
        } else if (fn->params[i].kind == PARAM_SLICE) {
            env[cur_slot++] = domain_create_single(fn->params[i].slice.elem_bounds);
        } else {
            env[cur_slot++] = domain_create_single(fn->params[i].scalar_bounds);
        }
    }

    for (size_t i = 0; i < fn->local_count; ++i) {
        env[cur_slot++] = domain_create_single(oath_interval_create(0, 0));
    }

    OathDBM dbm;
    dbm_init(&dbm, total_slots);

    for (size_t r = 0; r < fn->requires_count; ++r) {
        OathCondition req_cond = fn->requires_clauses[r];
        for (size_t t = 0; t < req_cond.count; ++t) {
            OathAtomicCond req = req_cond.terms[t];
            env_apply_atomic_condition(env, req, false);
            if (!dbm_apply_atomic_condition(&dbm, req, false, total_slots)) {
                rep.is_valid = false;
                rep.diag = SEPE_CONTRACT_VIOLATION;
                snprintf(rep.counter_example, sizeof(rep.counter_example),
                         "Precondition 'requires' clause %zu is self-contradictory", r + 1);
                return rep;
            }
        }
    }

    dbm_close(&dbm, total_slots);

    explore_stmt_sequence(fn->root_stmt, env, res, dbm, fn, mod, &rep);
    return rep;
}