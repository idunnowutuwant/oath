#ifndef OATH_AST_H
#define OATH_AST_H

#include "oath/common.h"
#include "oath/interval.h"

#define OATH_MAX_PARAMS 16
#define OATH_MAX_LOCALS 32
#define OATH_MAX_IDENT_LEN 32
#define OATH_MAX_FUNCTIONS 32
#define OATH_MAX_INVARIANTS 8
#define OATH_MAX_CONTRACTS 8
#define OATH_MAX_DISJOINT_PAIRS 4
#define OATH_MAX_STRUCTS 16
#define OATH_MAX_FIELDS 8
#define OATH_MAX_ENUMS 16
#define OATH_MAX_VARIANTS 8
#define OATH_MAX_CONJUNCTS 4
#define OATH_MAX_MATCH_ARMS 8

typedef struct {
    char name[OATH_MAX_IDENT_LEN];
    OathInterval bounds;
} OathFieldDef;

typedef struct {
    char name[OATH_MAX_IDENT_LEN];
    OathFieldDef fields[OATH_MAX_FIELDS];
    size_t field_count;
} OathStructDef;

typedef struct {
    char name[OATH_MAX_IDENT_LEN];
    OathInterval bounds;
} OathVariantDef;

typedef struct {
    char name[OATH_MAX_IDENT_LEN];
    OathVariantDef variants[OATH_MAX_VARIANTS];
    size_t variant_count;
} OathEnumDef;

typedef enum {
    EXPR_LITERAL,
    EXPR_VAR,
    EXPR_FIELD,
    EXPR_ADD,
    EXPR_SUB,
    EXPR_MUL,
    EXPR_DIV,
    EXPR_INDEX,
    EXPR_CALL,
    EXPR_ALLOC,
    EXPR_ENUM_CONSTRUCT
} OathExprType;

typedef struct OathExpr {
    OathExprType type;
    int64_t literal;
    size_t var_idx;
    size_t field_idx;
    char field_name[OATH_MAX_IDENT_LEN];
    struct OathExpr* lhs;
    struct OathExpr* rhs;
    char call_name[64];
    struct OathExpr* args[OATH_MAX_PARAMS];
    size_t arg_count;
    struct {
        size_t enum_idx;
        size_t variant_idx;
        struct OathExpr* payload;
    } enum_construct;
} OathExpr;

typedef enum {
    COND_EQ,
    COND_NE,
    COND_LT,
    COND_LE,
    COND_GT,
    COND_GE
} OathCondOp;

typedef struct {
    size_t lhs_slot;
    bool lhs_is_return;
    OathCondOp op;
    bool rhs_is_slot;
    union {
        int64_t const_val;
        size_t rhs_slot;
    } rhs;
} OathAtomicCond;

typedef struct {
    OathAtomicCond terms[OATH_MAX_CONJUNCTS];
    size_t count;
} OathCondition;

typedef struct {
    size_t var_idx;
    OathInterval bound;
} OathLoopInvariant;

typedef struct {
    size_t param_a;
    size_t param_b;
} OathDisjointPair;

struct OathStmt;

typedef struct {
    size_t variant_idx;
    char bound_var_name[OATH_MAX_IDENT_LEN];
    size_t bound_var_idx;
    struct OathStmt* body;
} OathMatchArm;

typedef enum {
    STMT_LET,
    STMT_ASSIGN,
    STMT_ARRAY_SET,
    STMT_FREE,
    STMT_MATCH,
    STMT_IF_ELSE,
    STMT_WHILE,
    STMT_RETURN
} OathStmtType;

typedef struct OathStmt {
    OathStmtType type;
    struct OathStmt* next;
    union {
        struct {
            size_t var_idx;
            OathExpr* init_expr;
            bool is_linear_resource;
        } let_stmt;
        struct {
            size_t var_idx;
            OathExpr* expr;
        } assign_stmt;
        struct {
            size_t buf_var_idx;
            OathExpr* idx_expr;
            OathExpr* val_expr;
        } array_set;
        struct {
            size_t var_idx;
        } free_stmt;
        struct {
            size_t target_var_idx;
            size_t enum_idx;
            OathMatchArm arms[OATH_MAX_MATCH_ARMS];
            size_t arm_count;
        } match_stmt;
        struct {
            OathCondition cond;
            struct OathStmt* then_branch;
            struct OathStmt* else_branch;
        } if_else;
        struct {
            OathCondition cond;
            OathLoopInvariant invariants[OATH_MAX_INVARIANTS];
            size_t invariant_count;
            struct OathStmt* body;
        } while_loop;
        struct {
            OathExpr* expr;
        } ret;
    } as;
} OathStmt;

typedef enum {
    PARAM_SCALAR,
    PARAM_BUFFER,
    PARAM_SLICE,
    PARAM_STRUCT,
    PARAM_ENUM
} OathParamKind;

typedef struct {
    char name[OATH_MAX_IDENT_LEN];
    OathParamKind kind;
    OathInterval scalar_bounds;
    size_t struct_def_idx;
    size_t enum_def_idx;
    struct {
        size_t capacity;
        OathInterval elem_bounds;
    } buffer;
    struct {
        size_t len_param_idx;
        char len_param_name[OATH_MAX_IDENT_LEN];
        OathInterval elem_bounds;
    } slice;
} OathParam;

typedef struct {
    char name[OATH_MAX_IDENT_LEN];
} OathLocal;

typedef struct {
    char name[64];
    bool is_extern;
    OathParam params[OATH_MAX_PARAMS];
    size_t param_count;
    OathLocal locals[OATH_MAX_LOCALS];
    size_t local_count;
    OathCondition requires_clauses[OATH_MAX_CONTRACTS];
    size_t requires_count;
    OathCondition ensures_clauses[OATH_MAX_CONTRACTS];
    size_t ensures_count;
    OathDisjointPair disjoint_contracts[OATH_MAX_DISJOINT_PAIRS];
    size_t disjoint_count;
    OathStmt* root_stmt;
    OathInterval postcondition;
} OathFunction;

typedef struct {
    OathStructDef structs[OATH_MAX_STRUCTS];
    size_t struct_count;
    OathEnumDef enums[OATH_MAX_ENUMS];
    size_t enum_count;
    OathFunction functions[OATH_MAX_FUNCTIONS];
    size_t function_count;
} OathModule;

#endif