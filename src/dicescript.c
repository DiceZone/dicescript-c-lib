#include "dicescript/dicescript.h"
#include "dicescript_internal.h"
#include "dicescript_parser.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DS_VALUE_DETAIL 2048

typedef struct ds_eval_value {
    int ok;
    int is_integer;
    int64_t integer;
    double number;
    char detail[DS_VALUE_DETAIL];
} ds_eval_value;

typedef struct ds_eval_context {
    dicescript_options options;
    dicescript_result *result;
    uint64_t random_state;
    dicescript_error_kind error_kind;
    char error[DICESCRIPT_MAX_ERROR];
} ds_eval_context;

static void ds_copy_text(char *target, size_t size, const char *text) {
    if (size == 0) return;
    if (text == NULL) text = "";
    (void)snprintf(target, size, "%s", text);
}

static void ds_set_parser_error(ds_parser_state_t *state,
                                dicescript_error_kind kind,
                                const char *format, ...) {
    va_list args;
    if (state == NULL || state->error_kind != DICESCRIPT_ERROR_NONE) return;
    state->error_kind = kind;
    va_start(args, format);
    (void)vsnprintf(state->error, sizeof(state->error), format, args);
    va_end(args);
}

static void ds_set_eval_error(ds_eval_context *ctx,
                              dicescript_error_kind kind,
                              const char *format, ...) {
    va_list args;
    if (ctx == NULL || ctx->error_kind != DICESCRIPT_ERROR_NONE) return;
    ctx->error_kind = kind;
    va_start(args, format);
    (void)vsnprintf(ctx->error, sizeof(ctx->error), format, args);
    va_end(args);
}

int ds_input_getchar(ds_parser_state_t *state) {
    if (state == NULL || state->input_position >= state->source_length) return -1;
    return (unsigned char)state->source[state->input_position++];
}

void ds_parser_error(ds_parser_state_t *state) {
    if (state == NULL) return;
    state->parser_error = 1;
    ds_set_parser_error(state, DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX,
                        "unsupported DiceScript V1 syntax");
}

ds_node_t *ds_node_new(ds_parser_state_t *state, ds_node_kind kind,
                       ds_node_t *left, ds_node_t *right, ds_node_t *third,
                       size_t start, size_t end) {
    ds_node_t *node;
    if (state == NULL || state->nodes == NULL || state->node_count >= state->node_capacity) {
        ds_set_parser_error(state, DICESCRIPT_ERROR_LIMIT, "expression AST is too large");
        return NULL;
    }
    node = &state->nodes[state->node_count++];
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->left = left;
    node->right = right;
    node->third = third;
    node->source_start = start;
    node->source_end = end;
    return node;
}

ds_node_t *ds_node_integer(ds_parser_state_t *state, const char *text,
                           size_t start, size_t end) {
    char *tail = NULL;
    ds_node_t *node = ds_node_new(state, DS_NODE_INTEGER, NULL, NULL, NULL, start, end);
    if (node == NULL) return NULL;
    errno = 0;
    node->integer = strtoll(text, &tail, 10);
    if (errno == ERANGE || tail == text || (tail != NULL && *tail != '\0')) {
        ds_set_parser_error(state, DICESCRIPT_ERROR_EVALUATION,
                            "integer literal is outside the int64 range");
    }
    node->number = (double)node->integer;
    return node;
}

ds_node_t *ds_node_float(ds_parser_state_t *state, const char *text,
                         size_t start, size_t end) {
    char *tail = NULL;
    ds_node_t *node = ds_node_new(state, DS_NODE_FLOAT, NULL, NULL, NULL, start, end);
    if (node == NULL) return NULL;
    errno = 0;
    node->number = strtod(text, &tail);
    if (errno == ERANGE || tail == text || (tail != NULL && *tail != '\0') || !isfinite(node->number)) {
        ds_set_parser_error(state, DICESCRIPT_ERROR_EVALUATION,
                            "floating point literal is outside the supported range");
    }
    return node;
}

ds_node_t *ds_node_modifier(ds_parser_state_t *state, int kind,
                            ds_node_t *value, size_t start, size_t end) {
    ds_node_t *node = ds_node_new(state, DS_NODE_MODIFIER, value, NULL, NULL, start, end);
    if (node != NULL) node->auxiliary = kind;
    return node;
}

ds_node_t *ds_node_chain(ds_node_t *head, ds_node_t *tail) {
    ds_node_t *node;
    if (head == NULL) return tail;
    node = head;
    while (node->next != NULL) node = node->next;
    node->next = tail;
    return head;
}

static ds_eval_value ds_bad_value(void) {
    ds_eval_value value;
    memset(&value, 0, sizeof(value));
    return value;
}

static ds_eval_value ds_int_value(int64_t number, const char *detail) {
    ds_eval_value value;
    memset(&value, 0, sizeof(value));
    value.ok = 1;
    value.is_integer = 1;
    value.integer = number;
    value.number = (double)number;
    ds_copy_text(value.detail, sizeof(value.detail), detail);
    return value;
}

static ds_eval_value ds_float_value(double number, const char *detail) {
    ds_eval_value value;
    memset(&value, 0, sizeof(value));
    value.ok = 1;
    value.number = number;
    ds_copy_text(value.detail, sizeof(value.detail), detail);
    return value;
}

static int ds_truthy(const ds_eval_value *value) {
    return value->is_integer ? value->integer != 0 : value->number != 0.0;
}

static int ds_to_integer(ds_eval_context *ctx, const ds_eval_value *value,
                         const char *what, int64_t *out) {
    if (!value->ok) return 0;
    if (!value->is_integer) {
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "%s must be an integer", what);
        return 0;
    }
    *out = value->integer;
    return 1;
}

static int ds_add_overflow(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return 1;
    *out = a + b;
    return 0;
}

static int ds_sub_overflow(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return 1;
    *out = a - b;
    return 0;
}

static int ds_mul_overflow(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) { *out = 0; return 0; }
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) return 1;
    if (a > 0) {
        if ((b > 0 && a > INT64_MAX / b) || (b < 0 && b < INT64_MIN / a)) return 1;
    } else {
        if ((b > 0 && a < INT64_MIN / b) || (b < 0 && a < INT64_MAX / b)) return 1;
    }
    *out = a * b;
    return 0;
}

static uint64_t ds_next_random(ds_eval_context *ctx) {
    uint64_t z;
    ctx->random_state += UINT64_C(0x9E3779B97F4A7C15);
    z = ctx->random_state;
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static uint64_t ds_bounded_random(ds_eval_context *ctx, uint64_t upper_bound) {
    uint64_t value;
    if (upper_bound == 0) return 0;
    if (ctx->options.random != NULL) {
        value = ctx->options.random(ctx->options.random_userdata, upper_bound);
        return value < upper_bound ? value : value % upper_bound;
    }
    value = ds_next_random(ctx);
    return value % upper_bound;
}

static int64_t ds_roll_die(ds_eval_context *ctx, int64_t faces) {
    int64_t value;
    if (faces < 1) {
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "dice faces must be at least 1");
        return 0;
    }
    value = (int64_t)ds_bounded_random(ctx, (uint64_t)faces) + 1;
    ctx->result->dice_rolls += 1;
    if (ctx->result->sample_count < DICESCRIPT_MAX_SAMPLES && value <= INT32_MAX) {
        ctx->result->samples[ctx->result->sample_count++] = (int32_t)value;
    }
    return value;
}

static void ds_detail_binary(char *target, size_t size,
                             const char *left, const char *op, const char *right) {
    (void)snprintf(target, size, "%s%s%s", left, op, right);
}

static int ds_compare_i64_asc(const void *left, const void *right) {
    const int64_t a = *(const int64_t *)left;
    const int64_t b = *(const int64_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static ds_eval_value ds_eval_node(ds_eval_context *ctx, const ds_node_t *node, uint32_t depth);

static ds_eval_value ds_eval_common_dice(ds_eval_context *ctx,
                                         const ds_node_t *node,
                                         uint32_t depth) {
    ds_eval_value count_value = node->left != NULL
        ? ds_eval_node(ctx, node->left, depth + 1) : ds_int_value(1, "1");
    ds_eval_value faces_value = node->right != NULL
        ? ds_eval_node(ctx, node->right, depth + 1)
        : ds_int_value(ctx->options.default_faces, "default");
    int64_t count = 0, faces = 0, selected = 0, sum = 0;
    int keep_kind = 0;
    int64_t keep_count = 0, clamp_min = INT64_MIN, clamp_max = INT64_MAX;
    const ds_node_t *modifier;
    int64_t *rolls;
    size_t offset = 0;
    ds_eval_value output;
    uint32_t i;

    if (!ds_to_integer(ctx, &count_value, "dice count", &count) ||
        !ds_to_integer(ctx, &faces_value, "dice faces", &faces)) return ds_bad_value();

    for (modifier = node->third; modifier != NULL; modifier = modifier->next) {
        int64_t amount = 1;
        if (modifier->auxiliary == DS_MOD_ADVANTAGE) {
            count = 2; keep_kind = DS_MOD_KEEP_HIGH; keep_count = 1; continue;
        }
        if (modifier->auxiliary == DS_MOD_DISADVANTAGE) {
            count = 2; keep_kind = DS_MOD_KEEP_LOW; keep_count = 1; continue;
        }
        if (modifier->left != NULL) {
            ds_eval_value value = ds_eval_node(ctx, modifier->left, depth + 1);
            if (!ds_to_integer(ctx, &value, "dice modifier", &amount)) return ds_bad_value();
        }
        switch (modifier->auxiliary) {
        case DS_MOD_KEEP_HIGH: case DS_MOD_KEEP_LOW:
        case DS_MOD_DROP_HIGH: case DS_MOD_DROP_LOW:
            keep_kind = modifier->auxiliary; keep_count = amount; break;
        case DS_MOD_MIN: clamp_min = amount; break;
        case DS_MOD_MAX: clamp_max = amount; break;
        default: break;
        }
    }

    if (count < 1 || (uint64_t)count > ctx->options.max_dice) {
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_LIMIT, "dice count is outside 1..%u", ctx->options.max_dice);
        return ds_bad_value();
    }
    if (faces < 1 || faces > INT32_MAX) {
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "dice faces are outside 1..%d", INT32_MAX);
        return ds_bad_value();
    }
    if (clamp_min > clamp_max) {
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "dice min modifier exceeds max modifier");
        return ds_bad_value();
    }

    rolls = (int64_t *)calloc((size_t)count, sizeof(*rolls));
    if (rolls == NULL) {
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_LIMIT, "not enough memory for dice pool");
        return ds_bad_value();
    }
    for (i = 0; i < (uint32_t)count; ++i) {
        int64_t roll = ds_roll_die(ctx, faces);
        if (ctx->error_kind != DICESCRIPT_ERROR_NONE) { free(rolls); return ds_bad_value(); }
        if (roll < clamp_min) roll = clamp_min;
        if (roll > clamp_max) roll = clamp_max;
        rolls[i] = roll;
    }
    if (keep_kind != 0) qsort(rolls, (size_t)count, sizeof(*rolls), ds_compare_i64_asc);
    selected = count;
    if (keep_count < 0) keep_count = 0;
    if (keep_count > count) keep_count = count;
    if (keep_kind == DS_MOD_KEEP_HIGH || keep_kind == DS_MOD_KEEP_LOW) selected = keep_count;
    else if (keep_kind == DS_MOD_DROP_HIGH || keep_kind == DS_MOD_DROP_LOW) selected = count - keep_count;

    for (i = 0; i < (uint32_t)selected; ++i) {
        size_t index = i;
        int64_t next;
        if (keep_kind == DS_MOD_KEEP_HIGH || keep_kind == DS_MOD_DROP_LOW) index = (size_t)count - selected + i;
        if (ds_add_overflow(sum, rolls[index], &next)) {
            free(rolls);
            ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "dice sum overflow");
            return ds_bad_value();
        }
        sum = next;
    }

    output = ds_int_value(sum, "");
    offset += (size_t)snprintf(output.detail + offset, sizeof(output.detail) - offset,
                              "%" PRId64 "D%" PRId64 "=[", count, faces);
    for (i = 0; i < (uint32_t)count && offset + 32 < sizeof(output.detail); ++i) {
        if (i >= 100) { offset += (size_t)snprintf(output.detail + offset, sizeof(output.detail) - offset, "..."); break; }
        offset += (size_t)snprintf(output.detail + offset, sizeof(output.detail) - offset,
                                  "%s%" PRId64, i == 0 ? "" : ",", rolls[i]);
    }
    (void)snprintf(output.detail + offset, sizeof(output.detail) - offset, "]=%" PRId64, sum);
    free(rolls);
    return output;
}

static ds_eval_value ds_eval_coc(ds_eval_context *ctx, const ds_node_t *node,
                                 uint32_t depth, int is_bonus) {
    ds_eval_value num_value = ds_eval_node(ctx, node->left, depth + 1);
    int64_t num = 0, base, tens, units, chosen, i;
    char detail[DS_VALUE_DETAIL];
    size_t offset = 0;
    if (!ds_to_integer(ctx, &num_value, "bonus/penalty dice count", &num)) return ds_bad_value();
    if (num < 0 || (uint64_t)num > ctx->options.max_dice) {
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_LIMIT, "bonus/penalty dice count is too large");
        return ds_bad_value();
    }
    base = ds_roll_die(ctx, 100);
    tens = base / 10;
    units = base % 10;
    chosen = tens;
    offset += (size_t)snprintf(detail + offset, sizeof(detail) - offset,
                              "(D100=%" PRId64 ",%s", base, is_bonus ? "奖励" : "惩罚");
    for (i = 0; i < num; ++i) {
        int64_t die = ds_roll_die(ctx, 10);
        int64_t digit = die == 10 ? 0 : die;
        if (offset + 32 < sizeof(detail)) {
            offset += (size_t)snprintf(detail + offset, sizeof(detail) - offset,
                                      "%s%" PRId64, i == 0 ? "" : " ", digit);
        }
        if (is_bonus) {
            if (digit < chosen || (units != 0 && digit == 0)) chosen = digit;
        } else {
            if (digit > chosen || (units == 0 && die == 10)) chosen = die;
        }
    }
    if (offset >= sizeof(detail)) offset = sizeof(detail) - 1;
    (void)snprintf(detail + offset, sizeof(detail) - offset, ")");
    return ds_int_value(chosen * 10 + units, detail);
}

static ds_eval_value ds_eval_fate(ds_eval_context *ctx) {
    int64_t sum = 0;
    char detail[32] = "4dF=[";
    size_t offset = strlen(detail);
    int i;
    for (i = 0; i < 4; ++i) {
        int64_t value = ds_roll_die(ctx, 3) - 2;
        sum += value;
        offset += (size_t)snprintf(detail + offset, sizeof(detail) - offset,
                                  "%s%c", i == 0 ? "" : ",", value < 0 ? '-' : value > 0 ? '+' : '0');
    }
    (void)snprintf(detail + offset, sizeof(detail) - offset, "]=%" PRId64, sum);
    return ds_int_value(sum, detail);
}

static ds_eval_value ds_eval_wod(ds_eval_context *ctx, const ds_node_t *node, uint32_t depth) {
    ds_eval_value pool_value = node->left != NULL ? ds_eval_node(ctx, node->left, depth + 1) : ds_int_value(1, "1");
    ds_eval_value add_value = ds_eval_node(ctx, node->right, depth + 1);
    int64_t pool = 0, add_line = 0, points = 10, threshold = 8;
    int threshold_ge = 1;
    int64_t successes = 0, total = 0;
    uint32_t rounds = 0;
    const ds_node_t *option;
    if (!ds_to_integer(ctx, &pool_value, "WOD pool", &pool) ||
        !ds_to_integer(ctx, &add_value, "WOD add line", &add_line)) return ds_bad_value();
    for (option = node->third; option != NULL; option = option->next) {
        ds_eval_value value = ds_eval_node(ctx, option->left, depth + 1);
        int64_t number = 0;
        if (!ds_to_integer(ctx, &value, "WOD option", &number)) return ds_bad_value();
        if (option->auxiliary == DS_WOD_POINTS) points = number;
        else { threshold = number; threshold_ge = option->auxiliary == DS_WOD_THRESHOLD_GE; }
    }
    if (pool < 1 || (uint64_t)pool > ctx->options.max_dice || points < 1 || threshold < 1 ||
        (add_line != 0 && add_line < 2)) {
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "invalid WOD pool parameters");
        return ds_bad_value();
    }
    while (pool > 0) {
        int64_t add_count = 0, i;
        if (++rounds > ctx->options.max_explosions) {
            ds_set_eval_error(ctx, DICESCRIPT_ERROR_LIMIT, "WOD explosion round limit reached");
            return ds_bad_value();
        }
        if ((uint64_t)(total + pool) > ctx->options.max_dice) {
            ds_set_eval_error(ctx, DICESCRIPT_ERROR_LIMIT, "WOD total dice limit reached");
            return ds_bad_value();
        }
        for (i = 0; i < pool; ++i) {
            int64_t roll = ds_roll_die(ctx, points);
            if ((threshold_ge && roll >= threshold) || (!threshold_ge && roll <= threshold)) ++successes;
            if (add_line != 0 && roll >= add_line) ++add_count;
        }
        total += pool;
        pool = add_count;
    }
    {
        char detail[128];
        (void)snprintf(detail, sizeof(detail), "WOD成功%" PRId64 "/%" PRId64 " 轮数:%u",
                       successes, total, rounds);
        return ds_int_value(successes, detail);
    }
}

static ds_eval_value ds_eval_double_cross(ds_eval_context *ctx, const ds_node_t *node, uint32_t depth) {
    ds_eval_value pool_value = ds_eval_node(ctx, node->left, depth + 1);
    ds_eval_value add_value = ds_eval_node(ctx, node->right, depth + 1);
    int64_t pool = 0, add_line = 0, points = 10, result = 0, total = 0;
    uint32_t rounds = 0;
    const ds_node_t *option;
    if (!ds_to_integer(ctx, &pool_value, "DoubleCross pool", &pool) ||
        !ds_to_integer(ctx, &add_value, "DoubleCross critical line", &add_line)) return ds_bad_value();
    for (option = node->third; option != NULL; option = option->next) {
        ds_eval_value value = ds_eval_node(ctx, option->left, depth + 1);
        if (!ds_to_integer(ctx, &value, "DoubleCross faces", &points)) return ds_bad_value();
    }
    if (pool < 1 || (uint64_t)pool > ctx->options.max_dice || add_line < 2 || points < 1) {
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "invalid DoubleCross pool parameters");
        return ds_bad_value();
    }
    while (pool > 0) {
        int64_t add_count = 0, round_max = 0, i;
        if (++rounds > ctx->options.max_explosions) {
            ds_set_eval_error(ctx, DICESCRIPT_ERROR_LIMIT, "DoubleCross explosion round limit reached");
            return ds_bad_value();
        }
        if ((uint64_t)(total + pool) > ctx->options.max_dice) {
            ds_set_eval_error(ctx, DICESCRIPT_ERROR_LIMIT, "DoubleCross total dice limit reached");
            return ds_bad_value();
        }
        for (i = 0; i < pool; ++i) {
            int64_t roll = ds_roll_die(ctx, points);
            if (roll > round_max) round_max = roll;
            if (roll >= add_line) { ++add_count; round_max = 10; }
        }
        if (ds_add_overflow(result, round_max, &result)) {
            ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "DoubleCross result overflow");
            return ds_bad_value();
        }
        total += pool;
        pool = add_count;
    }
    {
        char detail[128];
        (void)snprintf(detail, sizeof(detail), "DoubleCross出目%" PRId64 "/%" PRId64 " 轮数:%u",
                       result, total, rounds);
        return ds_int_value(result, detail);
    }
}

static ds_eval_value ds_eval_node(ds_eval_context *ctx, const ds_node_t *node, uint32_t depth) {
    ds_eval_value left, right;
    char detail[DS_VALUE_DETAIL];
    int64_t integer = 0;
    const char *op = "?";
    if (node == NULL) {
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "invalid expression node");
        return ds_bad_value();
    }
    if (depth > ctx->options.max_eval_depth) {
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_LIMIT, "expression nesting limit reached");
        return ds_bad_value();
    }
    switch (node->kind) {
    case DS_NODE_INTEGER: {
        char text[64];
        (void)snprintf(text, sizeof(text), "%" PRId64, node->integer);
        return ds_int_value(node->integer, text);
    }
    case DS_NODE_FLOAT: {
        char text[64];
        (void)snprintf(text, sizeof(text), "%.15g", node->number);
        return ds_float_value(node->number, text);
    }
    case DS_NODE_POSITIVE:
        return ds_eval_node(ctx, node->left, depth + 1);
    case DS_NODE_NEGATIVE:
        left = ds_eval_node(ctx, node->left, depth + 1);
        if (!left.ok) return left;
        detail[0] = '-';
        ds_copy_text(detail + 1, sizeof(detail) - 1, left.detail);
        if (left.is_integer) {
            if (left.integer == INT64_MIN) {
                ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "integer negation overflow");
                return ds_bad_value();
            }
            return ds_int_value(-left.integer, detail);
        }
        return ds_float_value(-left.number, detail);
    case DS_NODE_LOGIC_OR:
        left = ds_eval_node(ctx, node->left, depth + 1);
        if (!left.ok || ds_truthy(&left)) return left;
        return ds_eval_node(ctx, node->right, depth + 1);
    case DS_NODE_LOGIC_AND:
        left = ds_eval_node(ctx, node->left, depth + 1);
        if (!left.ok || !ds_truthy(&left)) return left;
        return ds_eval_node(ctx, node->right, depth + 1);
    case DS_NODE_TERNARY:
        left = ds_eval_node(ctx, node->left, depth + 1);
        if (!left.ok) return left;
        return ds_eval_node(ctx, ds_truthy(&left) ? node->right : node->third, depth + 1);
    case DS_NODE_DICE:
        return ds_eval_common_dice(ctx, node, depth);
    case DS_NODE_COC_BONUS:
        return ds_eval_coc(ctx, node, depth, 1);
    case DS_NODE_COC_PENALTY:
        return ds_eval_coc(ctx, node, depth, 0);
    case DS_NODE_FATE:
        return ds_eval_fate(ctx);
    case DS_NODE_WOD:
        return ds_eval_wod(ctx, node, depth);
    case DS_NODE_DOUBLE_CROSS:
        return ds_eval_double_cross(ctx, node, depth);
    default:
        break;
    }

    left = ds_eval_node(ctx, node->left, depth + 1);
    right = ds_eval_node(ctx, node->right, depth + 1);
    if (!left.ok || !right.ok) return ds_bad_value();

    if (node->kind == DS_NODE_BIT_AND || node->kind == DS_NODE_BIT_OR || node->kind == DS_NODE_MODULUS) {
        int64_t a = 0, b = 0;
        if (!ds_to_integer(ctx, &left, "left operand", &a) || !ds_to_integer(ctx, &right, "right operand", &b))
            return ds_bad_value();
        if (node->kind == DS_NODE_MODULUS) {
            if (b == 0) { ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "modulo by zero"); return ds_bad_value(); }
            if (a == INT64_MIN && b == -1) integer = 0; else integer = a % b;
            op = "%";
        } else if (node->kind == DS_NODE_BIT_AND) { integer = a & b; op = "&"; }
        else { integer = a | b; op = "|"; }
        ds_detail_binary(detail, sizeof(detail), left.detail, op, right.detail);
        return ds_int_value(integer, detail);
    }

    if (node->kind >= DS_NODE_LT && node->kind <= DS_NODE_GT) {
        const double a = left.is_integer ? (double)left.integer : left.number;
        const double b = right.is_integer ? (double)right.integer : right.number;
        switch (node->kind) {
        case DS_NODE_LT: integer = a < b; op = "<"; break;
        case DS_NODE_LE: integer = a <= b; op = "<="; break;
        case DS_NODE_EQ: integer = a == b; op = "=="; break;
        case DS_NODE_NE: integer = a != b; op = "!="; break;
        case DS_NODE_GE: integer = a >= b; op = ">="; break;
        default: integer = a > b; op = ">"; break;
        }
        ds_detail_binary(detail, sizeof(detail), left.detail, op, right.detail);
        return ds_int_value(integer, detail);
    }

    switch (node->kind) {
    case DS_NODE_ADD: op = "+"; break;
    case DS_NODE_SUBTRACT: op = "-"; break;
    case DS_NODE_MULTIPLY: op = "*"; break;
    case DS_NODE_DIVIDE: op = "/"; break;
    case DS_NODE_POWER: op = "^"; break;
    default:
        ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "unsupported AST operation");
        return ds_bad_value();
    }
    ds_detail_binary(detail, sizeof(detail), left.detail, op, right.detail);

    if (node->kind == DS_NODE_DIVIDE) {
        if ((right.is_integer && right.integer == 0) || (!right.is_integer && right.number == 0.0)) {
            ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "division by zero");
            return ds_bad_value();
        }
        if (left.is_integer && right.is_integer) {
            if (left.integer == INT64_MIN && right.integer == -1) {
                ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "integer division overflow");
                return ds_bad_value();
            }
            return ds_int_value(left.integer / right.integer, detail);
        }
        return ds_float_value((left.is_integer ? (double)left.integer : left.number) /
                              (right.is_integer ? (double)right.integer : right.number), detail);
    }

    if (node->kind == DS_NODE_POWER) {
        if (left.is_integer && right.is_integer && right.integer >= 0) {
            int64_t base = left.integer, exponent = right.integer, result = 1;
            while (exponent > 0) {
                if ((exponent & 1) && ds_mul_overflow(result, base, &result)) {
                    ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "integer power overflow");
                    return ds_bad_value();
                }
                exponent >>= 1;
                if (exponent > 0 && ds_mul_overflow(base, base, &base)) {
                    ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "integer power overflow");
                    return ds_bad_value();
                }
            }
            return ds_int_value(result, detail);
        }
        {
            double number = pow(left.is_integer ? (double)left.integer : left.number,
                                right.is_integer ? (double)right.integer : right.number);
            if (!isfinite(number)) {
                ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "invalid power result");
                return ds_bad_value();
            }
            return ds_float_value(number, detail);
        }
    }

    if (left.is_integer && right.is_integer) {
        int overflow = 0;
        if (node->kind == DS_NODE_ADD) overflow = ds_add_overflow(left.integer, right.integer, &integer);
        else if (node->kind == DS_NODE_SUBTRACT) overflow = ds_sub_overflow(left.integer, right.integer, &integer);
        else overflow = ds_mul_overflow(left.integer, right.integer, &integer);
        if (overflow) {
            ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "integer arithmetic overflow");
            return ds_bad_value();
        }
        return ds_int_value(integer, detail);
    }
    {
        const double a = left.is_integer ? (double)left.integer : left.number;
        const double b = right.is_integer ? (double)right.integer : right.number;
        double number = node->kind == DS_NODE_ADD ? a + b : node->kind == DS_NODE_SUBTRACT ? a - b : a * b;
        if (!isfinite(number)) {
            ds_set_eval_error(ctx, DICESCRIPT_ERROR_EVALUATION, "floating point arithmetic overflow");
            return ds_bad_value();
        }
        return ds_float_value(number, detail);
    }
}

static int ds_parse_expression(const char *expression, uint32_t max_nodes,
                               ds_parser_state_t *state) {
    dsv1_context_t *parser;
    ds_node_t *value = NULL;
    int parsed;
    memset(state, 0, sizeof(*state));
    state->source = expression;
    state->source_length = strlen(expression);
    state->node_capacity = max_nodes;
    state->nodes = (ds_node_t *)calloc(max_nodes, sizeof(*state->nodes));
    if (state->nodes == NULL) {
        state->error_kind = DICESCRIPT_ERROR_LIMIT;
        ds_copy_text(state->error, sizeof(state->error), "not enough memory for expression AST");
        return 0;
    }
    parser = dsv1_create(state);
    if (parser == NULL) {
        state->error_kind = DICESCRIPT_ERROR_LIMIT;
        ds_copy_text(state->error, sizeof(state->error), "failed to create PEG parser");
        free(state->nodes);
        state->nodes = NULL;
        return 0;
    }
    parsed = dsv1_parse(parser, &value);
    dsv1_destroy(parser);
    if (!parsed || state->parser_error || state->root == NULL || state->error_kind != DICESCRIPT_ERROR_NONE)
        return 0;
    return 1;
}

void dicescript_default_options(dicescript_options *options) {
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->default_faces = 100;
    options->max_dice = 20000;
    options->max_explosions = 100;
    options->max_ast_nodes = 4096;
    options->max_eval_depth = 128;
}

int dicescript_validate(const char *expression, dicescript_result *result) {
    ds_parser_state_t state;
    dicescript_options options;
    if (result == NULL) return 0;
    memset(result, 0, sizeof(*result));
    dicescript_default_options(&options);
    if (expression == NULL || expression[0] == '\0') {
        result->error_kind = DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX;
        ds_copy_text(result->error, sizeof(result->error), "expression is empty");
        return 0;
    }
    if (!ds_parse_expression(expression, options.max_ast_nodes, &state)) {
        result->error_kind = state.error_kind == DICESCRIPT_ERROR_NONE
            ? DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX : state.error_kind;
        ds_copy_text(result->error, sizeof(result->error),
                     state.error[0] != '\0' ? state.error : "unsupported DiceScript V1 syntax");
        free(state.nodes);
        return 0;
    }
    result->ok = 1;
    result->error_kind = DICESCRIPT_ERROR_NONE;
    ds_copy_text(result->detail, sizeof(result->detail), "valid DiceScript V1 expression");
    free(state.nodes);
    return 1;
}

int dicescript_eval(const char *expression, const dicescript_options *options,
                    dicescript_result *result) {
    dicescript_options actual;
    ds_parser_state_t state;
    ds_eval_context ctx;
    ds_eval_value value;
    if (result == NULL) return 0;
    memset(result, 0, sizeof(*result));
    dicescript_default_options(&actual);
    if (options != NULL) actual = *options;
    if (actual.default_faces < 1) actual.default_faces = 100;
    if (actual.max_dice == 0) actual.max_dice = 20000;
    if (actual.max_explosions == 0) actual.max_explosions = 100;
    if (actual.max_ast_nodes == 0) actual.max_ast_nodes = 4096;
    if (actual.max_eval_depth == 0) actual.max_eval_depth = 128;
    if (expression == NULL || expression[0] == '\0') {
        result->error_kind = DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX;
        ds_copy_text(result->error, sizeof(result->error), "expression is empty");
        return 0;
    }
    if (!ds_parse_expression(expression, actual.max_ast_nodes, &state)) {
        result->error_kind = state.error_kind == DICESCRIPT_ERROR_NONE
            ? DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX : state.error_kind;
        ds_copy_text(result->error, sizeof(result->error),
                     state.error[0] != '\0' ? state.error : "unsupported DiceScript V1 syntax");
        free(state.nodes);
        return 0;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.options = actual;
    ctx.result = result;
    ctx.random_state = actual.seed != 0 ? actual.seed
        : ((uint64_t)time(NULL) ^ (uint64_t)clock() ^ (uint64_t)(uintptr_t)&ctx);
    value = ds_eval_node(&ctx, state.root, 0);
    free(state.nodes);
    if (!value.ok || ctx.error_kind != DICESCRIPT_ERROR_NONE) {
        result->error_kind = ctx.error_kind == DICESCRIPT_ERROR_NONE
            ? DICESCRIPT_ERROR_EVALUATION : ctx.error_kind;
        ds_copy_text(result->error, sizeof(result->error),
                     ctx.error[0] != '\0' ? ctx.error : "DiceScript evaluation failed");
        return 0;
    }
    result->ok = 1;
    result->is_integer = value.is_integer;
    result->integer = value.is_integer ? value.integer : (int64_t)value.number;
    result->number = value.is_integer ? (double)value.integer : value.number;
    result->error_kind = DICESCRIPT_ERROR_NONE;
    ds_copy_text(result->detail, sizeof(result->detail), value.detail);
    return 1;
}

const char *dicescript_version(void) {
    return "0.2.0";
}
