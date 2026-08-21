#ifndef DICESCRIPT_VM_INTERNAL_H
#define DICESCRIPT_VM_INTERNAL_H

#include "dicescript/dicescript.h"

#include <stddef.h>
#include <stdint.h>

typedef enum ds_ast_kind {
    DS_AST_NULL,
    DS_AST_INTEGER,
    DS_AST_FLOAT,
    DS_AST_STRING,
    DS_AST_TEMPLATE,
    DS_AST_TRUE,
    DS_AST_FALSE,
    DS_AST_VARIABLE,
    DS_AST_THIS,
    DS_AST_RAW,
    DS_AST_COMPUTED,
    DS_AST_ARRAY,
    DS_AST_RANGE,
    DS_AST_DICT,
    DS_AST_PAIR,
    DS_AST_ST_ROOT,
    DS_AST_ST_ITEM,
    DS_AST_ST_NAME,
    DS_AST_ST_VALUE,
    DS_AST_BLOCK,
    DS_AST_EXPR_STMT,
    DS_AST_ASSIGN,
    DS_AST_IF,
    DS_AST_WHILE,
    DS_AST_FUNCTION_DEF,
    DS_AST_DICE_FLAG,
    DS_AST_RETURN,
    DS_AST_BREAK,
    DS_AST_CONTINUE,
    DS_AST_POSITIVE,
    DS_AST_NEGATIVE,
    DS_AST_ADD,
    DS_AST_SUBTRACT,
    DS_AST_MULTIPLY,
    DS_AST_DIVIDE,
    DS_AST_MODULUS,
    DS_AST_POWER,
    DS_AST_NULL_COALESCE,
    DS_AST_LT,
    DS_AST_LE,
    DS_AST_EQ,
    DS_AST_NE,
    DS_AST_GE,
    DS_AST_GT,
    DS_AST_BIT_AND,
    DS_AST_BIT_OR,
    DS_AST_LOGIC_AND,
    DS_AST_LOGIC_OR,
    DS_AST_TERNARY,
    DS_AST_POSTFIX,
    DS_AST_CALL,
    DS_AST_INDEX,
    DS_AST_SLICE,
    DS_AST_ATTR,
    DS_AST_ARRAY_KH,
    DS_AST_ARRAY_KL,
    DS_AST_CUSTOM_DICE,
    DS_AST_DICE,
    DS_AST_DICE_MOD,
    DS_AST_COC_BONUS,
    DS_AST_COC_PENALTY,
    DS_AST_FATE,
    DS_AST_WOD,
    DS_AST_WOD_OPTION,
    DS_AST_DOUBLE_CROSS,
    DS_AST_DC_OPTION
} ds_ast_kind;

typedef enum ds_vm_modifier_kind {
    DS_VM_KEEP_HIGH = 1,
    DS_VM_KEEP_LOW,
    DS_VM_DROP_HIGH,
    DS_VM_DROP_LOW,
    DS_VM_MIN,
    DS_VM_MAX,
    DS_VM_ADVANTAGE,
    DS_VM_DISADVANTAGE
} ds_vm_modifier_kind;

typedef enum ds_vm_st_kind {
    DS_VM_ST_SET = 1,
    DS_VM_ST_SET_COMPUTED,
    DS_VM_ST_MOD_ADD,
    DS_VM_ST_MOD_SUBTRACT,
    DS_VM_ST_MOD_SUBTRACT_ASSIGN,
    DS_VM_ST_SET_X0,
    DS_VM_ST_SET_X1
} ds_vm_st_kind;

typedef struct ds_ast ds_ast_t;
struct ds_ast {
    ds_ast_kind kind;
    int auxiliary;
    size_t source_start;
    size_t source_end;
    ds_ast_t *left;
    ds_ast_t *right;
    ds_ast_t *third;
    ds_ast_t *next;
};

typedef struct ds_vm_parser {
    const char *source;
    size_t source_length;
    size_t input_position;
    ds_ast_t *nodes;
    uint32_t node_capacity;
    uint32_t node_count;
    ds_ast_t *root;
    int parser_error;
    dicescript_error_kind error_kind;
    char error[DICESCRIPT_MAX_ERROR];
    dicescript_context *context;
    int allow_trailing;
    size_t custom_match_length;
    size_t custom_match_index;
} ds_vm_parser_t;

int ds_vm_input_getchar(ds_vm_parser_t *state);
void ds_vm_parser_error(ds_vm_parser_t *state);
ds_ast_t *ds_vm_ast(ds_vm_parser_t *state, ds_ast_kind kind,
                    ds_ast_t *left, ds_ast_t *right, ds_ast_t *third,
                    size_t start, size_t end);
ds_ast_t *ds_vm_chain(ds_ast_t *head, ds_ast_t *tail);
ds_ast_t *ds_vm_binary(ds_vm_parser_t *state, ds_ast_kind kind,
                       ds_ast_t *left, ds_ast_t *right,
                       size_t start, size_t end);
ds_ast_t *ds_vm_modifier(ds_vm_parser_t *state, int kind, ds_ast_t *value,
                         size_t start, size_t end);
int ds_vm_bitwise_enabled(ds_vm_parser_t *state);
size_t ds_vm_custom_match(ds_vm_parser_t *state, size_t start);
ds_ast_t *ds_vm_custom_ast(ds_vm_parser_t *state, size_t start, size_t end);

#endif
