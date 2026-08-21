#ifndef DICESCRIPT_INTERNAL_H
#define DICESCRIPT_INTERNAL_H

#include "dicescript/dicescript.h"

#include <stddef.h>
#include <stdint.h>

typedef enum ds_node_kind {
    DS_NODE_INTEGER,
    DS_NODE_FLOAT,
    DS_NODE_POSITIVE,
    DS_NODE_NEGATIVE,
    DS_NODE_ADD,
    DS_NODE_SUBTRACT,
    DS_NODE_MULTIPLY,
    DS_NODE_DIVIDE,
    DS_NODE_MODULUS,
    DS_NODE_POWER,
    DS_NODE_LT,
    DS_NODE_LE,
    DS_NODE_EQ,
    DS_NODE_NE,
    DS_NODE_GE,
    DS_NODE_GT,
    DS_NODE_BIT_AND,
    DS_NODE_BIT_OR,
    DS_NODE_LOGIC_AND,
    DS_NODE_LOGIC_OR,
    DS_NODE_TERNARY,
    DS_NODE_DICE,
    DS_NODE_MODIFIER,
    DS_NODE_COC_BONUS,
    DS_NODE_COC_PENALTY,
    DS_NODE_FATE,
    DS_NODE_WOD,
    DS_NODE_WOD_OPTION,
    DS_NODE_DOUBLE_CROSS,
    DS_NODE_DC_OPTION
} ds_node_kind;

typedef enum ds_modifier_kind {
    DS_MOD_KEEP_HIGH = 1,
    DS_MOD_KEEP_LOW,
    DS_MOD_DROP_HIGH,
    DS_MOD_DROP_LOW,
    DS_MOD_MIN,
    DS_MOD_MAX,
    DS_MOD_ADVANTAGE,
    DS_MOD_DISADVANTAGE
} ds_modifier_kind;

typedef enum ds_wod_option_kind {
    DS_WOD_POINTS = 1,
    DS_WOD_THRESHOLD_GE,
    DS_WOD_THRESHOLD_LE
} ds_wod_option_kind;

typedef struct ds_node ds_node_t;

struct ds_node {
    ds_node_kind kind;
    int auxiliary;
    int64_t integer;
    double number;
    size_t source_start;
    size_t source_end;
    ds_node_t *left;
    ds_node_t *right;
    ds_node_t *third;
    ds_node_t *next;
};

typedef struct ds_parser_state {
    const char *source;
    size_t source_length;
    size_t input_position;
    ds_node_t *nodes;
    uint32_t node_capacity;
    uint32_t node_count;
    ds_node_t *root;
    int parser_error;
    dicescript_error_kind error_kind;
    char error[DICESCRIPT_MAX_ERROR];
} ds_parser_state_t;

int ds_input_getchar(ds_parser_state_t *state);
void ds_parser_error(ds_parser_state_t *state);
ds_node_t *ds_node_new(ds_parser_state_t *state, ds_node_kind kind,
                       ds_node_t *left, ds_node_t *right, ds_node_t *third,
                       size_t start, size_t end);
ds_node_t *ds_node_integer(ds_parser_state_t *state, const char *text,
                           size_t start, size_t end);
ds_node_t *ds_node_float(ds_parser_state_t *state, const char *text,
                         size_t start, size_t end);
ds_node_t *ds_node_modifier(ds_parser_state_t *state, int kind,
                            ds_node_t *value, size_t start, size_t end);
ds_node_t *ds_node_chain(ds_node_t *head, ds_node_t *tail);

#endif
