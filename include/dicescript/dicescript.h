#ifndef DICESCRIPT_DICESCRIPT_H
#define DICESCRIPT_DICESCRIPT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DICESCRIPT_VERSION_MAJOR 0
#define DICESCRIPT_VERSION_MINOR 3
#define DICESCRIPT_VERSION_PATCH 0
#define DICESCRIPT_MAX_DETAIL 4096
#define DICESCRIPT_MAX_ERROR 256
#define DICESCRIPT_MAX_SAMPLES 256

typedef enum dicescript_error_kind {
    DICESCRIPT_ERROR_NONE = 0,
    DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX = 1,
    DICESCRIPT_ERROR_EVALUATION = 2,
    DICESCRIPT_ERROR_LIMIT = 3
} dicescript_error_kind;

/* Return a uniform integer in [0, upper_bound). */
typedef uint64_t (*dicescript_random_fn)(void *userdata, uint64_t upper_bound);

typedef struct dicescript_options {
    int64_t default_faces;
    uint32_t max_dice;
    uint32_t max_explosions;
    uint32_t max_ast_nodes;
    uint32_t max_eval_depth;
    uint64_t seed;
    /* -1 forces minimum rolls, 0 rolls normally, +1 forces maximum rolls. */
    int dice_roll_mode;
    dicescript_random_fn random;
    void *random_userdata;
} dicescript_options;

typedef struct dicescript_result {
    int ok;
    int is_integer;
    int64_t integer;
    double number;
    dicescript_error_kind error_kind;
    uint32_t dice_rolls;
    uint32_t sample_count;
    int32_t samples[DICESCRIPT_MAX_SAMPLES];
    char detail[DICESCRIPT_MAX_DETAIL];
    char error[DICESCRIPT_MAX_ERROR];
} dicescript_result;

/* Values use the same stable numeric ids as upstream DiceScript. */
typedef enum dicescript_value_type {
    DICESCRIPT_VALUE_INT = 0,
    DICESCRIPT_VALUE_FLOAT = 1,
    DICESCRIPT_VALUE_STRING = 2,
    DICESCRIPT_VALUE_NULL = 4,
    DICESCRIPT_VALUE_COMPUTED = 5,
    DICESCRIPT_VALUE_ARRAY = 6,
    DICESCRIPT_VALUE_DICT = 7,
    DICESCRIPT_VALUE_FUNCTION = 8,
    DICESCRIPT_VALUE_NATIVE_FUNCTION = 9,
    DICESCRIPT_VALUE_NATIVE_OBJECT = 10
} dicescript_value_type;

typedef struct dicescript_context dicescript_context;

typedef struct dicescript_runtime_options {
    dicescript_options dice;
    uint64_t max_steps;
    size_t max_memory_bytes;
    uint32_t max_container_items;
    uint32_t max_call_depth;
    int enable_statements;
    int enable_templates;
    int disable_bitwise_operations;
    int ignore_divide_by_zero;
    int enable_default_dice;
    char default_dice_side_expression[128];
    int enable_dice_coc;
    int enable_dice_fate;
    int enable_dice_wod;
    int enable_dice_double_cross;
} dicescript_runtime_options;

typedef struct dicescript_script_result {
    int ok;
    dicescript_value_type type;
    int64_t integer;
    double number;
    uint32_t dice_rolls;
    uint32_t sample_count;
    int32_t samples[DICESCRIPT_MAX_SAMPLES];
    dicescript_error_kind error_kind;
    uint64_t steps;
    char text[DICESCRIPT_MAX_DETAIL];
    char repr[DICESCRIPT_MAX_DETAIL];
    char detail[DICESCRIPT_MAX_DETAIL];
    char error[DICESCRIPT_MAX_ERROR];
    size_t consumed_bytes;
    char matched[DICESCRIPT_MAX_DETAIL];
    char rest[DICESCRIPT_MAX_DETAIL];
} dicescript_script_result;

/* A load callback returns the required byte count including the trailing NUL.
 * It is first queried with buffer=NULL/capacity=0, then called again with a
 * buffer of that size.  Return 0 when the host does not own the name.
 * JSON uses the upstream tagged VMValue representation. */
typedef size_t (*dicescript_host_load_json_fn)(void *userdata,
                                               const char *name,
                                               char *buffer,
                                               size_t capacity);
/* Return >0 when stored by the host, 0 to fall back to context-local storage,
 * or <0 to reject the assignment. */
typedef int (*dicescript_host_store_json_fn)(void *userdata,
                                             const char *name,
                                             const char *tagged_json);

typedef struct dicescript_host_load_pre_output {
    /* Optional remapped variable name used when value_json is NULL. */
    const char *new_name;
    /* Optional tagged VMValue JSON that immediately completes the load. */
    const char *value_json;
    const char *error;
} dicescript_host_load_pre_output;

/* Return >0 to apply output, 0 to keep the original name, or <0 on error. */
typedef int (*dicescript_host_load_pre_json_fn)(
    void *userdata, const char *name, int is_raw,
    dicescript_host_load_pre_output *output);

typedef struct dicescript_host_load_post_output {
    /* Tagged VMValue JSON replacing the normally loaded value. */
    const char *value_json;
    const char *error;
} dicescript_host_load_post_output;

/* current_value_json is the normally loaded, already computed tagged value.
 * Return >0 to replace it, 0 to keep it, or <0 on error. */
typedef int (*dicescript_host_load_post_json_fn)(
    void *userdata, const char *name, int is_raw,
    const char *current_value_json,
    dicescript_host_load_post_output *output);

typedef struct dicescript_host_store_pre_output {
    /* Optional tagged VMValue JSON replacing the assigned value. */
    const char *value_json;
    const char *error;
} dicescript_host_store_pre_output;

/* Return >0 when the hook completed the store and remaining storage must be
 * skipped, 0 to continue (using value_json when supplied), or <0 on error. */
typedef int (*dicescript_host_store_pre_json_fn)(
    void *userdata, const char *name, const char *value_json,
    dicescript_host_store_pre_output *output);

typedef struct dicescript_host_callbacks {
    dicescript_host_load_json_fn load;
    dicescript_host_store_json_fn store;
    dicescript_host_load_pre_json_fn load_pre;
    dicescript_host_load_post_json_fn load_post;
    dicescript_host_store_pre_json_fn store_pre;
    void *userdata;
} dicescript_host_callbacks;

/* Receives one ^st operation. value_json and optional extra_json use tagged
 * VMValue JSON.  Return 0 for success or nonzero to reject the operation. */
typedef int (*dicescript_st_callback_fn)(void *userdata, const char *operation,
                                         const char *name,
                                         const char *value_json,
                                         const char *extra_json,
                                         const char *operator_text,
                                         const char *detail);

typedef struct dicescript_custom_dice_output {
    /* Required on success: upstream tagged VMValue JSON. */
    const char *value_json;
    /* Optional; the matched source is used when NULL or empty. */
    const char *detail;
    /* Optional diagnostic used when the evaluator returns failure. */
    const char *error;
} dicescript_custom_dice_output;

/* Match only at input[0].  Return nonzero and set consumed_bytes > 0 when
 * matched.  The matcher can implement either regex or stream-style parsing;
 * it may be called more than once during PEG backtracking and must be pure. */
typedef int (*dicescript_custom_dice_match_fn)(void *userdata,
                                               const char *input,
                                               size_t input_length,
                                               size_t *consumed_bytes);
typedef int (*dicescript_custom_dice_eval_fn)(void *userdata,
                                              const char *matched_text,
                                              size_t matched_length,
                                              dicescript_custom_dice_output *output);

typedef struct dicescript_native_output {
    const char *value_json;
    const char *error;
} dicescript_native_output;

/* args_json is a tagged VMValue array. self_json is NULL for an unbound
 * function, otherwise it is the receiver's tagged VMValue JSON. Return
 * nonzero on success; value_json may be NULL to return null. */
typedef int (*dicescript_native_function_fn)(void *userdata,
                                             const char *self_json,
                                             const char *args_json,
                                             dicescript_native_output *output);

/* get/list return >0 on success, get may return 0 for a missing attribute,
 * and negative values are errors. set follows the ^st convention: return 0
 * on success and nonzero to reject the assignment. */
typedef int (*dicescript_native_object_get_fn)(void *userdata,
                                               const char *attribute,
                                               dicescript_native_output *output);
typedef int (*dicescript_native_object_set_fn)(void *userdata,
                                               const char *attribute,
                                               const char *value_json);
typedef int (*dicescript_native_object_dir_fn)(void *userdata,
                                               dicescript_native_output *output);
typedef struct dicescript_native_object_callbacks {
    dicescript_native_object_get_fn get;
    dicescript_native_object_set_fn set;
    dicescript_native_object_dir_fn list;
    void *userdata;
} dicescript_native_object_callbacks;

/* Transient view of one upstream-compatible calculation-detail span.  All
 * pointers are owned by the library and remain valid only for the duration of
 * the callback. source contains the complete input while begin/end select the
 * current span in UTF-8 byte offsets. */
typedef struct dicescript_detail_span_view {
    const char *tag;
    const char *source;
    size_t source_length;
    size_t parsed_length;
    size_t begin;
    size_t end;
    const char *result;
    const char *text;
    const char *expression;
    const char *expression_suffix;
    int text_only;
    int is_root;
} dicescript_detail_span_view;

/* Return NULL to keep default_detail.  A non-NULL result is copied before the
 * callback returns. span_rewrite runs for nested and root spans; root_rewrite
 * runs afterwards for root spans only. */
typedef const char *(*dicescript_detail_rewrite_fn)(
    void *userdata, const char *default_detail,
    const dicescript_detail_span_view *span);

/* Optional complete renderer.  When it returns non-NULL, the default renderer
 * and per-span callbacks are skipped. spans and their strings are transient. */
typedef const char *(*dicescript_detail_make_fn)(
    void *userdata, const dicescript_detail_span_view *spans,
    size_t span_count, const char *source, size_t parsed_length,
    const char *result);

typedef struct dicescript_detail_callbacks {
    dicescript_detail_make_fn make;
    dicescript_detail_rewrite_fn span_rewrite;
    dicescript_detail_rewrite_fn root_rewrite;
    void *userdata;
} dicescript_detail_callbacks;

void dicescript_default_options(dicescript_options *options);
void dicescript_default_runtime_options(dicescript_runtime_options *options);

/* Parse only. This never invokes the random callback. */
int dicescript_validate(const char *expression, dicescript_result *result);

/* Parse and evaluate one complete DiceScript V1 numeric expression. */
int dicescript_eval(const char *expression,
                    const dicescript_options *options,
                    dicescript_result *result);

/* Stateful full-language VM.  The legacy functions above intentionally stay
 * expression-only and side-effect free for fallback probing. */
dicescript_context *dicescript_context_create(const dicescript_runtime_options *options);
void dicescript_context_destroy(dicescript_context *context);
void dicescript_context_clear(dicescript_context *context);

void dicescript_context_set_host_callbacks(dicescript_context *context,
                                           const dicescript_host_callbacks *callbacks);
void dicescript_context_set_st_callback(dicescript_context *context,
                                        dicescript_st_callback_fn callback,
                                        void *userdata);
void dicescript_context_set_detail_callbacks(
    dicescript_context *context,
    const dicescript_detail_callbacks *callbacks);
int dicescript_context_register_custom_dice(dicescript_context *context,
                                            const char *name,
                                            dicescript_custom_dice_match_fn matcher,
                                            dicescript_custom_dice_eval_fn evaluator,
                                            void *userdata);
void dicescript_context_clear_custom_dice(dicescript_context *context);
int dicescript_context_register_native_function(dicescript_context *context,
                                                const char *name,
                                                dicescript_native_function_fn function,
                                                void *userdata);
int dicescript_context_set_native_object(dicescript_context *context,
                                         const char *variable_name,
                                         const char *object_name,
                                         const dicescript_native_object_callbacks *callbacks);
void dicescript_context_clear_native_functions(dicescript_context *context);

/* Matches upstream Context.Run: execute the longest valid statement prefix and
 * expose the untouched suffix through result.rest. */
int dicescript_context_run(dicescript_context *context,
                           const char *script,
                           dicescript_script_result *result);
/* Require the complete input to be valid DiceScript. */
int dicescript_context_run_complete(dicescript_context *context,
                                    const char *script,
                                    dicescript_script_result *result);
/* Upstream-style prefix execution. The parsed prefix and untouched suffix are
 * returned in result.matched/result.rest; consumed_bytes is never truncated. */
int dicescript_context_run_prefix(dicescript_context *context,
                                  const char *script,
                                  dicescript_script_result *result);
/* Parse a complete expression or script without evaluating it.  Validation
 * never consumes randomness, loads/stores host values, or mutates globals. */
int dicescript_context_validate_expression(
    dicescript_context *context, const char *expression,
    dicescript_script_result *result);
/* Parse the longest valid expression prefix without evaluating it. */
int dicescript_context_validate_expression_prefix(
    dicescript_context *context, const char *input,
    dicescript_script_result *result);
int dicescript_context_validate_script(
    dicescript_context *context, const char *script,
    dicescript_script_result *result);

int dicescript_context_eval(dicescript_context *context,
                            const char *expression,
                            dicescript_script_result *result);
int dicescript_context_format(dicescript_context *context,
                              const char *template_text,
                              dicescript_script_result *result);

int dicescript_context_set_int(dicescript_context *context,
                               const char *name, int64_t value);
int dicescript_context_set_float(dicescript_context *context,
                                 const char *name, double value);
int dicescript_context_set_string(dicescript_context *context,
                                  const char *name, const char *value);
int dicescript_context_unset(dicescript_context *context, const char *name);
int dicescript_context_get(dicescript_context *context,
                           const char *name,
                           dicescript_script_result *result);

/* Plain JSON interchange for embedding applications. */
int dicescript_context_set_json(dicescript_context *context,
                                const char *name, const char *json_text,
                                dicescript_script_result *result);
int dicescript_context_get_json(dicescript_context *context,
                                const char *name,
                                char *buffer, size_t buffer_size);

/* Upstream-compatible tagged VMValue JSON.  This preserves computed values,
 * arrays, dictionaries, script functions and known native function names.
 * The encoded shape is {"t": type_id, "v": ...}. */
int dicescript_context_set_serialized(dicescript_context *context,
                                      const char *name,
                                      const char *tagged_json,
                                      dicescript_script_result *result);
int dicescript_context_get_serialized(dicescript_context *context,
                                      const char *name,
                                      char *buffer, size_t buffer_size);

const char *dicescript_version(void);

#ifdef __cplusplus
}
#endif

#endif
