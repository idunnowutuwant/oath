#ifndef OATH_SEPE_H
#define OATH_SEPE_H

#include "oath/ast.h"

#define OATH_MAX_DISJOINT_INTERVALS 4

typedef struct {
    OathInterval items[OATH_MAX_DISJOINT_INTERVALS];
    size_t count;
} OathDomain;

typedef enum {
    SEPE_OK = 0,
    SEPE_TRAP_DIV_ZERO,
    SEPE_TRAP_X86_DE,
    SEPE_TRAP_OVERFLOW,
    SEPE_CONTRACT_VIOLATION,
    SEPE_BUFFER_OVERFLOW,
    SEPE_CALL_CONTRACT_VIOLATION,
    SEPE_UNKNOWN_FUNCTION,
    SEPE_LOOP_INIT_FAILED,
    SEPE_LOOP_INVARIANT_BROKEN,
    SEPE_ALIASING_VIOLATION,
    SEPE_RESOURCE_LEAK,
    SEPE_DOUBLE_FREE,
    SEPE_NON_EXHAUSTIVE_MATCH,
    SEPE_ENSURES_VIOLATION
} SepeDiagCode;

typedef struct {
    bool is_valid;
    SepeDiagCode diag;
    size_t total_paths_explored;
    size_t infeasible_paths_pruned;
    char target_symbol[OATH_MAX_IDENT_LEN];
    char counter_example[256];
} SepeReport;

SepeReport oath_sepe_verify_function(const OathFunction* fn, const OathModule* mod);

#endif