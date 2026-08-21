#ifndef DICESCRIPT_DICESCRIPT_H
#define DICESCRIPT_DICESCRIPT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DICESCRIPT_VERSION_MAJOR 0
#define DICESCRIPT_VERSION_MINOR 2
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
    dicescript_error_kind error_kind;
    uint64_t steps;
    char text[DICESCRIPT_MAX_DETAIL];
    char repr[DICESCRIPT_MAX_DETAIL];
    char detail[DICESCRIPT_MAX_DETAIL];
    char error[DICESCRIPT_MAX_ERROR];
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
typedef struct dicescript_host_callbacks {
    dicescript_host_load_json_fn load;
    dicescript_host_store_json_fn store;
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

int dicescript_context_run(dicescript_context *context,
                           const char *script,
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
