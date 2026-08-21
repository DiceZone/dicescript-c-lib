#include "dicescript_vm_internal.h"
#include "dicescript_vm_parser.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ds_string ds_string;
typedef struct ds_array ds_array;
typedef struct ds_dict ds_dict;
typedef struct ds_function ds_function;
typedef struct ds_computed ds_computed;
typedef struct ds_callable ds_callable;

typedef struct ds_value {
    dicescript_value_type type;
    union {
        int64_t integer;
        double number;
        ds_string *string;
        ds_array *array;
        ds_dict *dict;
        ds_function *function;
        ds_computed *computed;
        ds_callable *callable;
        void *pointer;
    } as;
} ds_value;

struct ds_string {
    size_t length;
    char bytes[1];
};

struct ds_array {
    size_t count;
    size_t capacity;
    ds_value *items;
};

typedef struct ds_dict_entry {
    ds_string *key;
    ds_value value;
} ds_dict_entry;

struct ds_dict {
    size_t count;
    size_t capacity;
    ds_dict_entry *entries;
};

struct ds_function {
    ds_string *name;
    size_t param_count;
    ds_string **params;
    ds_string *body;
    int has_bound_self;
    ds_value bound_self;
};

struct ds_computed {
    ds_string *expression;
    ds_dict attributes;
    int evaluating;
};

struct ds_callable {
    ds_string *name;
    int has_receiver;
    ds_value receiver;
};

typedef union ds_alignment {
    void *pointer;
    double number;
    int64_t integer;
} ds_alignment;

typedef struct ds_allocation {
    struct ds_allocation *next;
    size_t size;
    ds_alignment alignment;
} ds_allocation;

typedef struct ds_frame {
    ds_dict locals;
    ds_value this_value;
    int function_scope;
    struct ds_frame *parent;
} ds_frame;

typedef enum ds_flow {
    DS_FLOW_NORMAL,
    DS_FLOW_RETURN,
    DS_FLOW_BREAK,
    DS_FLOW_CONTINUE,
    DS_FLOW_ERROR
} ds_flow;

typedef struct ds_exec {
    struct dicescript_context *context;
    ds_frame *frame;
    const char *source;
    size_t source_length;
    uint32_t call_depth;
    uint32_t loop_depth;
    ds_flow flow;
    ds_value flow_value;
} ds_exec;

struct dicescript_context {
    dicescript_runtime_options options;
    dicescript_host_callbacks host;
    dicescript_st_callback_fn st_callback;
    void *st_userdata;
    ds_allocation *allocations;
    size_t memory_used;
    ds_dict globals;
    uint64_t steps;
    dicescript_error_kind error_kind;
    char error[DICESCRIPT_MAX_ERROR];
    char detail[DICESCRIPT_MAX_DETAIL];
};

typedef struct ds_buffer {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} ds_buffer;

static ds_value ds_null(void) {
    ds_value value;
    memset(&value, 0, sizeof(value));
    value.type = DICESCRIPT_VALUE_NULL;
    return value;
}

static ds_value ds_int(int64_t number) {
    ds_value value = ds_null();
    value.type = DICESCRIPT_VALUE_INT;
    value.as.integer = number;
    return value;
}

static ds_value ds_float(double number) {
    ds_value value = ds_null();
    value.type = DICESCRIPT_VALUE_FLOAT;
    value.as.number = number;
    return value;
}

static void ds_set_error(dicescript_context *context,
                         dicescript_error_kind kind,
                         const char *message) {
    if (context == NULL || context->error_kind != DICESCRIPT_ERROR_NONE) return;
    context->error_kind = kind;
    if (message == NULL) message = "DiceScript error";
    (void)snprintf(context->error, sizeof(context->error), "%s", message);
}

static void *ds_alloc(dicescript_context *context, size_t size) {
    ds_allocation *allocation;
    size_t total;
    if (context == NULL || size == 0) return NULL;
    if (size > SIZE_MAX - sizeof(ds_allocation)) {
        ds_set_error(context, DICESCRIPT_ERROR_LIMIT, "allocation size overflow");
        return NULL;
    }
    total = sizeof(ds_allocation) + size;
    if (context->memory_used > context->options.max_memory_bytes ||
        total > context->options.max_memory_bytes - context->memory_used) {
        ds_set_error(context, DICESCRIPT_ERROR_LIMIT, "DiceScript memory limit reached");
        return NULL;
    }
    allocation = (ds_allocation *)calloc(1, total);
    if (allocation == NULL) {
        ds_set_error(context, DICESCRIPT_ERROR_LIMIT, "out of memory");
        return NULL;
    }
    allocation->next = context->allocations;
    allocation->size = size;
    context->allocations = allocation;
    context->memory_used += total;
    return (void *)(allocation + 1);
}

static void ds_free_all(dicescript_context *context) {
    ds_allocation *item;
    if (context == NULL) return;
    item = context->allocations;
    while (item != NULL) {
        ds_allocation *next = item->next;
        free(item);
        item = next;
    }
    context->allocations = NULL;
    context->memory_used = 0;
}

static ds_string *ds_new_string_n(dicescript_context *context,
                                  const char *text, size_t length) {
    ds_string *string;
    if (length > SIZE_MAX - sizeof(ds_string)) {
        ds_set_error(context, DICESCRIPT_ERROR_LIMIT, "string is too long");
        return NULL;
    }
    string = (ds_string *)ds_alloc(context, sizeof(ds_string) + length);
    if (string == NULL) return NULL;
    string->length = length;
    if (length != 0 && text != NULL) memcpy(string->bytes, text, length);
    string->bytes[length] = '\0';
    return string;
}

static ds_string *ds_new_string(dicescript_context *context, const char *text) {
    return ds_new_string_n(context, text != NULL ? text : "",
                           text != NULL ? strlen(text) : 0);
}

static ds_value ds_string_value(ds_string *string) {
    ds_value value = ds_null();
    value.type = DICESCRIPT_VALUE_STRING;
    value.as.string = string;
    return value;
}

static int ds_string_equals(const ds_string *left, const char *right) {
    size_t length = right != NULL ? strlen(right) : 0;
    return left != NULL && left->length == length &&
           memcmp(left->bytes, right != NULL ? right : "", length) == 0;
}

static int ds_dict_reserve(dicescript_context *context, ds_dict *dict, size_t need) {
    ds_dict_entry *entries;
    size_t capacity;
    if (dict->capacity >= need) return 1;
    if (need > context->options.max_container_items) {
        ds_set_error(context, DICESCRIPT_ERROR_LIMIT, "dictionary item limit reached");
        return 0;
    }
    capacity = dict->capacity == 0 ? 8 : dict->capacity * 2;
    if (capacity < need) capacity = need;
    if (capacity > context->options.max_container_items)
        capacity = context->options.max_container_items;
    entries = (ds_dict_entry *)ds_alloc(context, capacity * sizeof(*entries));
    if (entries == NULL) return 0;
    if (dict->count != 0) memcpy(entries, dict->entries, dict->count * sizeof(*entries));
    dict->entries = entries;
    dict->capacity = capacity;
    return 1;
}

static ptrdiff_t ds_dict_index_n(const ds_dict *dict, const char *key, size_t length) {
    size_t i;
    if (dict == NULL) return -1;
    for (i = 0; i < dict->count; ++i) {
        if (dict->entries[i].key->length == length &&
            memcmp(dict->entries[i].key->bytes, key, length) == 0)
            return (ptrdiff_t)i;
    }
    return -1;
}

static ptrdiff_t ds_dict_index(const ds_dict *dict, const char *key) {
    return ds_dict_index_n(dict, key != NULL ? key : "", key != NULL ? strlen(key) : 0);
}

static int ds_dict_set_n(dicescript_context *context, ds_dict *dict,
                         const char *key, size_t length, ds_value value) {
    ptrdiff_t index = ds_dict_index_n(dict, key, length);
    if (index >= 0) {
        dict->entries[index].value = value;
        return 1;
    }
    if (!ds_dict_reserve(context, dict, dict->count + 1)) return 0;
    dict->entries[dict->count].key = ds_new_string_n(context, key, length);
    if (dict->entries[dict->count].key == NULL) return 0;
    dict->entries[dict->count].value = value;
    ++dict->count;
    return 1;
}

static int ds_dict_set(dicescript_context *context, ds_dict *dict,
                       const char *key, ds_value value) {
    return ds_dict_set_n(context, dict, key != NULL ? key : "",
                         key != NULL ? strlen(key) : 0, value);
}

static int ds_dict_get_own(const ds_dict *dict, const char *key, ds_value *value) {
    ptrdiff_t index = ds_dict_index(dict, key);
    if (index < 0) return 0;
    if (value != NULL) *value = dict->entries[index].value;
    return 1;
}

static int ds_dict_remove(ds_dict *dict, const char *key) {
    ptrdiff_t index = ds_dict_index(dict, key);
    if (index < 0) return 0;
    if ((size_t)index + 1 < dict->count)
        memmove(&dict->entries[index], &dict->entries[index + 1],
                (dict->count - (size_t)index - 1) * sizeof(*dict->entries));
    --dict->count;
    return 1;
}

static ds_array *ds_new_array(dicescript_context *context) {
    return (ds_array *)ds_alloc(context, sizeof(ds_array));
}

static int ds_array_reserve(dicescript_context *context, ds_array *array, size_t need) {
    ds_value *items;
    size_t capacity;
    if (array->capacity >= need) return 1;
    if (need > context->options.max_container_items) {
        ds_set_error(context, DICESCRIPT_ERROR_LIMIT, "array item limit reached");
        return 0;
    }
    capacity = array->capacity == 0 ? 8 : array->capacity * 2;
    if (capacity < need) capacity = need;
    if (capacity > context->options.max_container_items)
        capacity = context->options.max_container_items;
    items = (ds_value *)ds_alloc(context, capacity * sizeof(*items));
    if (items == NULL) return 0;
    if (array->count != 0) memcpy(items, array->items, array->count * sizeof(*items));
    array->items = items;
    array->capacity = capacity;
    return 1;
}

static int ds_array_push(dicescript_context *context, ds_array *array, ds_value value) {
    if (!ds_array_reserve(context, array, array->count + 1)) return 0;
    array->items[array->count++] = value;
    return 1;
}

static ds_value ds_array_value(ds_array *array) {
    ds_value value = ds_null();
    value.type = DICESCRIPT_VALUE_ARRAY;
    value.as.array = array;
    return value;
}

static ds_value ds_dict_value(ds_dict *dict) {
    ds_value value = ds_null();
    value.type = DICESCRIPT_VALUE_DICT;
    value.as.dict = dict;
    return value;
}

static int ds_step(ds_exec *exec) {
    if (exec->context->error_kind != DICESCRIPT_ERROR_NONE) return 0;
    if (++exec->context->steps > exec->context->options.max_steps) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "DiceScript step limit reached");
        exec->flow = DS_FLOW_ERROR;
        return 0;
    }
    return 1;
}

static void ds_buffer_init(ds_buffer *buffer) {
    memset(buffer, 0, sizeof(*buffer));
}

static int ds_buffer_reserve(ds_buffer *buffer, size_t need) {
    char *data;
    size_t capacity;
    if (buffer->failed) return 0;
    if (buffer->capacity >= need) return 1;
    capacity = buffer->capacity == 0 ? 128 : buffer->capacity * 2;
    while (capacity < need && capacity <= SIZE_MAX / 2) capacity *= 2;
    if (capacity < need) {
        buffer->failed = 1;
        return 0;
    }
    data = (char *)realloc(buffer->data, capacity);
    if (data == NULL) {
        buffer->failed = 1;
        return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static int ds_buffer_append_n(ds_buffer *buffer, const char *text, size_t length) {
    if (!ds_buffer_reserve(buffer, buffer->length + length + 1)) return 0;
    if (length != 0) memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 1;
}

static int ds_buffer_append(ds_buffer *buffer, const char *text) {
    return ds_buffer_append_n(buffer, text != NULL ? text : "",
                              text != NULL ? strlen(text) : 0);
}

static int ds_buffer_char(ds_buffer *buffer, char value) {
    return ds_buffer_append_n(buffer, &value, 1);
}

static size_t ds_utf8_count(const char *text, size_t length) {
    size_t i, count = 0;
    for (i = 0; i < length; ++i)
        if (((unsigned char)text[i] & 0xc0u) != 0x80u) ++count;
    return count;
}

static size_t ds_utf8_offset(const char *text, size_t length, size_t rune_index) {
    size_t i, current = 0;
    for (i = 0; i < length; ++i) {
        if (((unsigned char)text[i] & 0xc0u) != 0x80u) {
            if (current == rune_index) return i;
            ++current;
        }
    }
    return length;
}

static size_t ds_utf8_rune_size(const char *text, size_t length, size_t offset) {
    unsigned char ch;
    if (offset >= length) return 0;
    ch = (unsigned char)text[offset];
    if (ch < 0x80u) return 1;
    if ((ch & 0xe0u) == 0xc0u && offset + 2 <= length) return 2;
    if ((ch & 0xf0u) == 0xe0u && offset + 3 <= length) return 3;
    if ((ch & 0xf8u) == 0xf0u && offset + 4 <= length) return 4;
    return 1;
}

static void ds_buffer_free(ds_buffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static void ds_value_format(ds_exec *exec, ds_value value,
                            ds_buffer *buffer, int repr, uint32_t depth);
static ds_value ds_eval(ds_exec *exec, ds_ast_t *node, int raw);
static ds_value ds_run_source(ds_exec *parent, ds_frame *frame,
                              const char *source, int expression_only);
static int ds_value_tagged_json(ds_exec *exec, ds_value value,
                                ds_buffer *buffer, uint32_t depth,
                                const void **ancestors);
static int ds_decode_tagged(ds_exec *exec, ds_value encoded,
                            ds_value *decoded, uint32_t depth);

static int ds_truthy(ds_value value) {
    switch (value.type) {
        case DICESCRIPT_VALUE_INT: return value.as.integer != 0;
        case DICESCRIPT_VALUE_FLOAT: return value.as.number != 0.0;
        case DICESCRIPT_VALUE_STRING: return value.as.string != NULL && value.as.string->length != 0;
        case DICESCRIPT_VALUE_NULL: return 0;
        case DICESCRIPT_VALUE_ARRAY: return value.as.array != NULL && value.as.array->count != 0;
        case DICESCRIPT_VALUE_DICT: return value.as.dict != NULL && value.as.dict->count != 0;
        default: return 1;
    }
}

static int ds_numeric(ds_exec *exec, ds_value value, double *number,
                      int64_t *integer, int *is_integer) {
    if (value.type == DICESCRIPT_VALUE_INT) {
        if (number != NULL) *number = (double)value.as.integer;
        if (integer != NULL) *integer = value.as.integer;
        if (is_integer != NULL) *is_integer = 1;
        return 1;
    }
    if (value.type == DICESCRIPT_VALUE_FLOAT) {
        if (number != NULL) *number = value.as.number;
        if (integer != NULL) *integer = (int64_t)value.as.number;
        if (is_integer != NULL) *is_integer = 0;
        return 1;
    }
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "numeric value required");
    exec->flow = DS_FLOW_ERROR;
    return 0;
}

static int ds_integer(ds_exec *exec, ds_value value, int64_t *integer) {
    if (value.type != DICESCRIPT_VALUE_INT) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "integer value required");
        exec->flow = DS_FLOW_ERROR;
        return 0;
    }
    *integer = value.as.integer;
    return 1;
}

static char *ds_slice_copy_malloc(const char *source, size_t start, size_t end) {
    char *copy;
    size_t length = end > start ? end - start : 0;
    copy = (char *)malloc(length + 1);
    if (copy == NULL) return NULL;
    if (length != 0) memcpy(copy, source + start, length);
    copy[length] = '\0';
    return copy;
}

static ds_string *ds_ast_text(ds_exec *exec, const ds_ast_t *node) {
    if (node == NULL || node->source_end < node->source_start ||
        node->source_end > exec->source_length) return ds_new_string(exec->context, "");
    return ds_new_string_n(exec->context, exec->source + node->source_start,
                           node->source_end - node->source_start);
}

static int ds_hex_digit(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (int)(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'F') return (int)(ch - 'A') + 10;
    return -1;
}

static int ds_buffer_utf8(ds_buffer *buffer, uint32_t codepoint) {
    char encoded[4]; size_t length;
    if (codepoint <= 0x7fu) { encoded[0] = (char)codepoint; length = 1; }
    else if (codepoint <= 0x7ffu) {
        encoded[0] = (char)(0xc0u | (codepoint >> 6));
        encoded[1] = (char)(0x80u | (codepoint & 0x3fu)); length = 2;
    } else if (codepoint <= 0xffffu) {
        encoded[0] = (char)(0xe0u | (codepoint >> 12));
        encoded[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        encoded[2] = (char)(0x80u | (codepoint & 0x3fu)); length = 3;
    } else if (codepoint <= 0x10ffffu) {
        encoded[0] = (char)(0xf0u | (codepoint >> 18));
        encoded[1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
        encoded[2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        encoded[3] = (char)(0x80u | (codepoint & 0x3fu)); length = 4;
    } else return 0;
    return ds_buffer_append_n(buffer, encoded, length);
}

static ds_string *ds_unescape_literal(ds_exec *exec, const ds_ast_t *node) {
    size_t i, end;
    ds_buffer buffer;
    ds_string *out;
    if (node->source_end <= node->source_start + 1) return ds_new_string(exec->context, "");
    i = node->source_start + 1;
    end = node->source_end - 1;
    ds_buffer_init(&buffer);
    while (i < end) {
        char ch = exec->source[i++];
        if (ch == '\\' && i < end) {
            char escaped = exec->source[i++];
            switch (escaped) {
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 'f': ch = '\f'; break;
                case 'b': ch = '\b'; break;
                case 't': ch = '\t'; break;
                case '\\': ch = '\\'; break;
                case '\'': ch = '\''; break;
                case '"': ch = '"'; break;
                case '/': ch = '/'; break;
                case '{': ch = '{'; break;
                case '}': ch = '}'; break;
                case 'u': {
                    uint32_t codepoint = 0; int digit; size_t j;
                    if (i + 4 > end) goto invalid_unicode;
                    for (j = 0; j < 4; ++j) {
                        digit = ds_hex_digit((unsigned char)exec->source[i + j]);
                        if (digit < 0) goto invalid_unicode;
                        codepoint = (codepoint << 4) | (uint32_t)digit;
                    }
                    i += 4;
                    if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                        uint32_t low = 0;
                        if (i + 6 > end || exec->source[i] != '\\' || exec->source[i + 1] != 'u') goto invalid_unicode;
                        i += 2;
                        for (j = 0; j < 4; ++j) {
                            digit = ds_hex_digit((unsigned char)exec->source[i + j]);
                            if (digit < 0) goto invalid_unicode;
                            low = (low << 4) | (uint32_t)digit;
                        }
                        i += 4;
                        if (low < 0xdc00u || low > 0xdfffu) goto invalid_unicode;
                        codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) + (low - 0xdc00u);
                    } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) goto invalid_unicode;
                    if (!ds_buffer_utf8(&buffer, codepoint)) break;
                    continue;
                }
                default:
                    if (!ds_buffer_char(&buffer, '\\')) break;
                    ch = escaped;
                    break;
            }
        }
        if (!ds_buffer_char(&buffer, ch)) break;
    }
    if (buffer.failed) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory while decoding string");
        ds_buffer_free(&buffer);
        return NULL;
    }
    out = ds_new_string_n(exec->context, buffer.data != NULL ? buffer.data : "", buffer.length);
    ds_buffer_free(&buffer);
    return out;
invalid_unicode:
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid Unicode escape");
    exec->flow = DS_FLOW_ERROR;
    ds_buffer_free(&buffer);
    return NULL;
}

static int ds_name_from_node(ds_exec *exec, const ds_ast_t *node,
                             const char **text, size_t *length) {
    if (node == NULL || node->kind != DS_AST_VARIABLE ||
        node->source_end > exec->source_length || node->source_end < node->source_start) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "identifier required");
        exec->flow = DS_FLOW_ERROR;
        return 0;
    }
    *text = exec->source + node->source_start;
    *length = node->source_end - node->source_start;
    return 1;
}

static int ds_is_builtin_name(const char *name, size_t length) {
    static const char *const names[] = {
        "floor", "ceil", "round", "abs", "int", "float", "str", "bool",
        "toInt", "toFloat", "toStr", "toBool", "repr", "load", "loadRaw", "store", "typeId", "dir",
        "loadRawAttr", "loadRawItem"
    };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (strlen(names[i]) == length && memcmp(names[i], name, length) == 0) return 1;
    return 0;
}

static ds_value ds_new_callable(dicescript_context *context,
                                const char *name, size_t length,
                                int has_receiver, ds_value receiver) {
    ds_callable *callable = (ds_callable *)ds_alloc(context, sizeof(*callable));
    ds_value value = ds_null();
    if (callable == NULL) return value;
    callable->name = ds_new_string_n(context, name, length);
    callable->has_receiver = has_receiver;
    callable->receiver = receiver;
    value.type = DICESCRIPT_VALUE_NATIVE_FUNCTION;
    value.as.callable = callable;
    return value;
}

static ds_value ds_resolve_computed(ds_exec *exec, ds_value value) {
    ds_frame frame;
    ds_value result;
    if (value.type != DICESCRIPT_VALUE_COMPUTED || value.as.computed == NULL) return value;
    if (value.as.computed->evaluating) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "computed value recursion detected");
        exec->flow = DS_FLOW_ERROR;
        return ds_null();
    }
    if (exec->call_depth >= exec->context->options.max_call_depth) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "DiceScript call depth limit reached");
        exec->flow = DS_FLOW_ERROR;
        return ds_null();
    }
    memset(&frame, 0, sizeof(frame));
    frame.this_value = ds_dict_value(&value.as.computed->attributes);
    frame.parent = exec->frame;
    value.as.computed->evaluating = 1;
    result = ds_run_source(exec, &frame, value.as.computed->expression->bytes, 1);
    value.as.computed->evaluating = 0;
    return result;
}

static int ds_host_load_value(ds_exec *exec, const char *name, size_t length,
                              ds_value *value) {
    dicescript_host_load_json_fn callback = exec->context->host.load;
    char *name_copy, *json;
    size_t required, written;
    ds_exec decoder; ds_frame frame; ds_value encoded;
    if (callback == NULL) return 0;
    name_copy = ds_slice_copy_malloc(name, 0, length);
    if (name_copy == NULL) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory");
        exec->flow = DS_FLOW_ERROR; return -1;
    }
    required = callback(exec->context->host.userdata, name_copy, NULL, 0);
    if (required == 0) { free(name_copy); return 0; }
    if (required > exec->context->options.max_memory_bytes) {
        free(name_copy);
        ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT,
                     "host value exceeds DiceScript memory limit");
        exec->flow = DS_FLOW_ERROR; return -1;
    }
    json = (char *)calloc(required, 1);
    if (json == NULL) {
        free(name_copy); ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory");
        exec->flow = DS_FLOW_ERROR; return -1;
    }
    written = callback(exec->context->host.userdata, name_copy, json, required);
    free(name_copy);
    if (written == 0 || written > required || json[required - 1] != '\0') {
        free(json); ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION,
                                 "host load callback returned invalid tagged JSON buffer");
        exec->flow = DS_FLOW_ERROR; return -1;
    }
    memset(&decoder, 0, sizeof(decoder)); memset(&frame, 0, sizeof(frame));
    decoder.context = exec->context; decoder.frame = &frame;
    decoder.source = json; decoder.source_length = strlen(json);
    encoded = ds_run_source(&decoder, &frame, json, 1);
    if (decoder.flow != DS_FLOW_ERROR && ds_decode_tagged(&decoder, encoded, value, 0)) {
        free(json); return 1;
    }
    free(json); exec->flow = DS_FLOW_ERROR; return -1;
}

static int ds_host_store_value(ds_exec *exec, const char *name, size_t length,
                               ds_value value) {
    dicescript_host_store_json_fn callback = exec->context->host.store;
    const void **ancestors;
    ds_buffer output;
    char *name_copy;
    int handled;
    if (callback == NULL) return 0;
    ancestors = (const void **)calloc((size_t)exec->context->options.max_call_depth + 1u,
                                      sizeof(*ancestors));
    if (ancestors == NULL) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory");
        exec->flow = DS_FLOW_ERROR; return -1;
    }
    ds_buffer_init(&output);
    if (!ds_value_tagged_json(exec, value, &output, 0, ancestors)) {
        free(ancestors); ds_buffer_free(&output); return -1;
    }
    free(ancestors);
    name_copy = ds_slice_copy_malloc(name, 0, length);
    if (name_copy == NULL) {
        ds_buffer_free(&output); ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory");
        exec->flow = DS_FLOW_ERROR; return -1;
    }
    handled = callback(exec->context->host.userdata, name_copy,
                       output.data != NULL ? output.data : "");
    free(name_copy); ds_buffer_free(&output);
    if (handled < 0) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION,
                     "host rejected DiceScript value assignment");
        exec->flow = DS_FLOW_ERROR; return -1;
    }
    return handled > 0 ? 1 : 0;
}

static int ds_lookup(ds_exec *exec, const char *name, size_t length,
                     int raw, ds_value *value) {
    ds_frame *frame;
    ptrdiff_t index;
    for (frame = exec->frame; frame != NULL; frame = frame->parent) {
        index = ds_dict_index_n(&frame->locals, name, length);
        if (index >= 0) {
            *value = frame->locals.entries[index].value;
            if (!raw) *value = ds_resolve_computed(exec, *value);
            return exec->flow != DS_FLOW_ERROR;
        }
    }
    index = ds_dict_index_n(&exec->context->globals, name, length);
    if (index >= 0) {
        *value = exec->context->globals.entries[index].value;
        if (!raw) *value = ds_resolve_computed(exec, *value);
        return exec->flow != DS_FLOW_ERROR;
    }
    {
        int host_result = ds_host_load_value(exec, name, length, value);
        if (host_result > 0) { if (!raw) *value = ds_resolve_computed(exec, *value); return exec->flow != DS_FLOW_ERROR; }
        if (host_result < 0) return 0;
    }
    if (ds_is_builtin_name(name, length)) {
        *value = ds_new_callable(exec->context, name, length, 0, ds_null());
        return exec->context->error_kind == DICESCRIPT_ERROR_NONE;
    }
    /* Upstream DiceScript deliberately treats an unknown variable as null. */
    *value = ds_null();
    return 1;
}

static int ds_store_name(ds_exec *exec, const char *name, size_t length, ds_value value) {
    ds_dict *target = exec->frame != NULL && exec->frame->function_scope
        ? &exec->frame->locals : &exec->context->globals;
    if (exec->frame != NULL && exec->frame->parent == NULL &&
        !exec->frame->function_scope && exec->context->host.store != NULL) {
        int host_result = ds_host_store_value(exec, name, length, value);
        if (host_result > 0) return 1;
        if (host_result < 0) return 0;
    }
    return ds_dict_set_n(exec->context, target, name, length, value);
}

static int ds_value_key(ds_exec *exec, ds_value value, ds_buffer *buffer) {
    if (value.type == DICESCRIPT_VALUE_STRING)
        return ds_buffer_append_n(buffer, value.as.string->bytes, value.as.string->length);
    if (value.type == DICESCRIPT_VALUE_INT) {
        char text[64]; (void)snprintf(text, sizeof(text), "%" PRId64, value.as.integer);
        return ds_buffer_append(buffer, text);
    }
    if (value.type == DICESCRIPT_VALUE_FLOAT) {
        char text[96]; (void)snprintf(text, sizeof(text), "%.15g", value.as.number);
        return ds_buffer_append(buffer, text);
    }
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION,
                 "dictionary key must be a string or number");
    exec->flow = DS_FLOW_ERROR;
    return 0;
}

static int ds_dict_lookup_chain(ds_exec *exec, ds_dict *dict,
                                const char *key, ds_value *value,
                                uint32_t depth) {
    ds_value proto;
    if (depth > exec->context->options.max_call_depth) return 0;
    if (ds_dict_get_own(dict, key, value)) return 1;
    if (ds_dict_get_own(dict, "__proto__", &proto) && proto.type == DICESCRIPT_VALUE_DICT)
        return ds_dict_lookup_chain(exec, proto.as.dict, key, value, depth + 1);
    return 0;
}

static ds_value ds_get_attr(ds_exec *exec, ds_value object,
                            const char *name, size_t length, int raw) {
    ds_value value;
    char local[128];
    const char *key = name;
    if (length >= sizeof(local)) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "attribute name is too long");
        exec->flow = DS_FLOW_ERROR;
        return ds_null();
    }
    memcpy(local, name, length);
    local[length] = '\0';
    key = local;
    if (object.type == DICESCRIPT_VALUE_COMPUTED && object.as.computed != NULL) {
        if (strcmp(key, "compute") == 0)
            return ds_new_callable(exec->context, key, strlen(key), 1, object);
        if (ds_dict_get_own(&object.as.computed->attributes, key, &value))
            return raw ? value : ds_resolve_computed(exec, value);
        return ds_null();
    }
    if (object.type == DICESCRIPT_VALUE_DICT && object.as.dict != NULL) {
        if (ds_dict_lookup_chain(exec, object.as.dict, key, &value, 0)) {
            if (!raw) value = ds_resolve_computed(exec, value);
            if (value.type == DICESCRIPT_VALUE_FUNCTION && value.as.function != NULL) {
                ds_function *bound = (ds_function *)ds_alloc(exec->context, sizeof(*bound));
                if (bound == NULL) return ds_null();
                *bound = *value.as.function;
                bound->has_bound_self = 1;
                bound->bound_self = object;
                value.as.function = bound;
            }
            return value;
        }
        if (strcmp(key, "len") == 0 || strcmp(key, "keys") == 0 ||
            strcmp(key, "values") == 0 || strcmp(key, "items") == 0 ||
            strcmp(key, "has") == 0 || strcmp(key, "get") == 0 ||
            strcmp(key, "getRaw") == 0)
            return ds_new_callable(exec->context, key, strlen(key), 1, object);
        return ds_null();
    }
    if (object.type == DICESCRIPT_VALUE_ARRAY && object.as.array != NULL) {
        if (strcmp(key, "sum") == 0 || strcmp(key, "len") == 0 ||
            strcmp(key, "kh") == 0 || strcmp(key, "kl") == 0 ||
            strcmp(key, "shuffle") == 0 || strcmp(key, "rand") == 0 ||
            strcmp(key, "randSize") == 0 || strcmp(key, "push") == 0 ||
            strcmp(key, "shift") == 0 || strcmp(key, "pop") == 0)
            return ds_new_callable(exec->context, key, strlen(key), 1, object);
        return ds_null();
    }
    if (object.type == DICESCRIPT_VALUE_STRING && object.as.string != NULL) {
        if (strcmp(key, "len") == 0)
            return ds_new_callable(exec->context, key, strlen(key), 1, object);
        return ds_null();
    }
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "attribute access requires an object");
    exec->flow = DS_FLOW_ERROR;
    return ds_null();
}

static int ds_normalize_index(int64_t index, size_t length, size_t *normalized) {
    int64_t actual = index;
    if (actual < 0) actual += (int64_t)length;
    if (actual < 0 || (uint64_t)actual >= (uint64_t)length) return 0;
    *normalized = (size_t)actual;
    return 1;
}

static int ds_slice_bounds(ds_exec *exec, size_t length,
                           ds_ast_t *start_node, ds_ast_t *end_node,
                           ds_ast_t *step_node, size_t *start_out,
                           size_t *end_out) {
    int64_t start = 0;
    int64_t end = (int64_t)length;
    ds_value value;
    if (step_node != NULL) {
        (void)ds_eval(exec, step_node, 0);
        if (exec->flow == DS_FLOW_ERROR) return 0;
        ds_set_error(exec->context, DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX,
                     "DiceScript does not support an explicit slice step");
        exec->flow = DS_FLOW_ERROR;
        return 0;
    }
    if (start_node != NULL) {
        value = ds_eval(exec, start_node, 0);
        if (!ds_integer(exec, value, &start)) return 0;
    }
    if (end_node != NULL) {
        value = ds_eval(exec, end_node, 0);
        if (!ds_integer(exec, value, &end)) return 0;
    }
    if (start < 0) start += (int64_t)length;
    if (end < 0) end += (int64_t)length;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (start > (int64_t)length) start = (int64_t)length;
    if (end > (int64_t)length) end = (int64_t)length;
    if (start > end) start = end;
    *start_out = (size_t)start; *end_out = (size_t)end;
    return 1;
}

static ds_value ds_get_item(ds_exec *exec, ds_value object, ds_value key, int raw) {
    int64_t index;
    size_t normalized;
    if (object.type == DICESCRIPT_VALUE_ARRAY && object.as.array != NULL) {
        if (!ds_integer(exec, key, &index)) return ds_null();
        if (!ds_normalize_index(index, object.as.array->count, &normalized)) {
            ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "array index out of range");
            exec->flow = DS_FLOW_ERROR; return ds_null();
        }
        key = object.as.array->items[normalized];
        return raw ? key : ds_resolve_computed(exec, key);
    }
    if (object.type == DICESCRIPT_VALUE_STRING && object.as.string != NULL) {
        size_t rune_count = ds_utf8_count(object.as.string->bytes, object.as.string->length);
        size_t byte_offset, byte_count;
        if (!ds_integer(exec, key, &index)) return ds_null();
        if (!ds_normalize_index(index, rune_count, &normalized)) {
            ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "string index out of range");
            exec->flow = DS_FLOW_ERROR; return ds_null();
        }
        byte_offset = ds_utf8_offset(object.as.string->bytes, object.as.string->length, normalized);
        byte_count = ds_utf8_rune_size(object.as.string->bytes, object.as.string->length, byte_offset);
        return ds_string_value(ds_new_string_n(exec->context,
                               object.as.string->bytes + byte_offset, byte_count));
    }
    if (object.type == DICESCRIPT_VALUE_DICT && object.as.dict != NULL) {
        ds_buffer buffer;
        ds_value value;
        ds_buffer_init(&buffer);
        if (!ds_value_key(exec, key, &buffer)) { ds_buffer_free(&buffer); return ds_null(); }
        if (!ds_dict_lookup_chain(exec, object.as.dict, buffer.data != NULL ? buffer.data : "", &value, 0)) {
            ds_buffer_free(&buffer);
            return ds_null();
        }
        ds_buffer_free(&buffer);
        return raw ? value : ds_resolve_computed(exec, value);
    }
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "item access requires array, string or dictionary");
    exec->flow = DS_FLOW_ERROR;
    return ds_null();
}

static ds_value ds_slice_value(ds_exec *exec, ds_value object,
                               ds_ast_t *start_node, ds_ast_t *end_node,
                               ds_ast_t *step_node) {
    size_t length, start, end, i;
    ds_value value = ds_null();
    if (object.type != DICESCRIPT_VALUE_ARRAY && object.type != DICESCRIPT_VALUE_STRING) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "slice requires array or string");
        exec->flow = DS_FLOW_ERROR;
        return ds_null();
    }
    length = object.type == DICESCRIPT_VALUE_ARRAY ? object.as.array->count
        : ds_utf8_count(object.as.string->bytes, object.as.string->length);
    if (!ds_slice_bounds(exec, length, start_node, end_node, step_node,
                         &start, &end)) return ds_null();
    if (object.type == DICESCRIPT_VALUE_STRING) {
        ds_buffer buffer;
        ds_buffer_init(&buffer);
        for (i = start; i < end; ++i) {
            size_t offset = ds_utf8_offset(object.as.string->bytes, object.as.string->length, i);
            ds_buffer_append_n(&buffer, object.as.string->bytes + offset,
                               ds_utf8_rune_size(object.as.string->bytes, object.as.string->length, offset));
        }
        value = ds_string_value(ds_new_string_n(exec->context,
                                buffer.data != NULL ? buffer.data : "", buffer.length));
        ds_buffer_free(&buffer);
        return value;
    }
    {
        ds_array *array = ds_new_array(exec->context);
        if (array == NULL) return ds_null();
        for (i = start; i < end; ++i)
            if (!ds_array_push(exec->context, array, object.as.array->items[i])) return ds_null();
        return ds_array_value(array);
    }
}

static int ds_set_slice(ds_exec *exec, ds_value object,
                        ds_ast_t *start_node, ds_ast_t *end_node,
                        ds_ast_t *step_node, ds_value replacement) {
    ds_array *target;
    ds_value *copy = NULL;
    size_t start, end, tail_count, new_count;
    if (object.type != DICESCRIPT_VALUE_ARRAY || object.as.array == NULL) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION,
                     "slice assignment requires an array");
        exec->flow = DS_FLOW_ERROR; return 0;
    }
    if (replacement.type != DICESCRIPT_VALUE_ARRAY || replacement.as.array == NULL) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION,
                     "slice assignment value must be an array");
        exec->flow = DS_FLOW_ERROR; return 0;
    }
    target = object.as.array;
    if (!ds_slice_bounds(exec, target->count, start_node, end_node, step_node,
                         &start, &end)) return 0;
    if (replacement.as.array->count != 0) {
        copy = (ds_value *)malloc(replacement.as.array->count * sizeof(*copy));
        if (copy == NULL) {
            ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory");
            exec->flow = DS_FLOW_ERROR; return 0;
        }
        memcpy(copy, replacement.as.array->items,
               replacement.as.array->count * sizeof(*copy));
    }
    new_count = target->count - (end - start) + replacement.as.array->count;
    if (!ds_array_reserve(exec->context, target, new_count)) { free(copy); exec->flow = DS_FLOW_ERROR; return 0; }
    tail_count = target->count - end;
    if (tail_count != 0)
        memmove(target->items + start + replacement.as.array->count,
                target->items + end, tail_count * sizeof(*target->items));
    if (replacement.as.array->count != 0)
        memcpy(target->items + start, copy,
               replacement.as.array->count * sizeof(*target->items));
    target->count = new_count;
    free(copy);
    return 1;
}

static ds_value ds_make_computed(ds_exec *exec, ds_ast_t *expression) {
    ds_computed *computed;
    ds_value value = ds_null();
    char *text;
    size_t start, end;
    if (expression == NULL) return value;
    start = expression->source_start;
    end = expression->source_end;
    text = ds_slice_copy_malloc(exec->source, start, end);
    if (text == NULL) { ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory"); exec->flow = DS_FLOW_ERROR; return value; }
    computed = (ds_computed *)ds_alloc(exec->context, sizeof(*computed));
    if (computed != NULL) computed->expression = ds_new_string(exec->context, text);
    free(text);
    if (computed == NULL || computed->expression == NULL) return value;
    value.type = DICESCRIPT_VALUE_COMPUTED;
    value.as.computed = computed;
    return value;
}

static int ds_values_equal(ds_exec *exec, ds_value left, ds_value right, uint32_t depth) {
    size_t i;
    if (depth > exec->context->options.max_call_depth) return 0;
    if ((left.type == DICESCRIPT_VALUE_INT || left.type == DICESCRIPT_VALUE_FLOAT) &&
        (right.type == DICESCRIPT_VALUE_INT || right.type == DICESCRIPT_VALUE_FLOAT)) {
        double a = left.type == DICESCRIPT_VALUE_INT ? (double)left.as.integer : left.as.number;
        double b = right.type == DICESCRIPT_VALUE_INT ? (double)right.as.integer : right.as.number;
        return a == b;
    }
    if (left.type != right.type) return 0;
    switch (left.type) {
        case DICESCRIPT_VALUE_NULL: return 1;
        case DICESCRIPT_VALUE_STRING:
            return left.as.string->length == right.as.string->length &&
                   memcmp(left.as.string->bytes, right.as.string->bytes, left.as.string->length) == 0;
        case DICESCRIPT_VALUE_ARRAY:
            if (left.as.array->count != right.as.array->count) return 0;
            for (i = 0; i < left.as.array->count; ++i)
                if (!ds_values_equal(exec, left.as.array->items[i], right.as.array->items[i], depth + 1)) return 0;
            return 1;
        default: return left.as.pointer == right.as.pointer;
    }
}

static ds_value ds_binary(ds_exec *exec, ds_ast_kind kind, ds_value left, ds_value right) {
    double a, b, result;
    int64_t ia, ib;
    int ai, bi;
    if (kind == DS_AST_EQ || kind == DS_AST_NE) {
        int equal = ds_values_equal(exec, left, right, 0);
        return ds_int(kind == DS_AST_EQ ? equal : !equal);
    }
    if (kind == DS_AST_ADD && left.type == DICESCRIPT_VALUE_STRING && right.type == DICESCRIPT_VALUE_STRING) {
        ds_buffer buffer;
        ds_string *string;
        ds_buffer_init(&buffer);
        ds_buffer_append_n(&buffer, left.as.string->bytes, left.as.string->length);
        ds_buffer_append_n(&buffer, right.as.string->bytes, right.as.string->length);
        string = ds_new_string_n(exec->context, buffer.data != NULL ? buffer.data : "", buffer.length);
        ds_buffer_free(&buffer);
        return ds_string_value(string);
    }
    if (kind == DS_AST_ADD && left.type == DICESCRIPT_VALUE_ARRAY && right.type == DICESCRIPT_VALUE_ARRAY) {
        ds_array *array = ds_new_array(exec->context);
        size_t i;
        if (array == NULL || !ds_array_reserve(exec->context, array, left.as.array->count + right.as.array->count)) return ds_null();
        for (i = 0; i < left.as.array->count; ++i) ds_array_push(exec->context, array, left.as.array->items[i]);
        for (i = 0; i < right.as.array->count; ++i) ds_array_push(exec->context, array, right.as.array->items[i]);
        return ds_array_value(array);
    }
    if (kind == DS_AST_MULTIPLY && left.type == DICESCRIPT_VALUE_ARRAY && right.type == DICESCRIPT_VALUE_INT) {
        ds_array *array = ds_new_array(exec->context);
        int64_t times = right.as.integer;
        size_t i;
        if (times < 0 || (uint64_t)times > exec->context->options.max_container_items ||
            left.as.array->count > exec->context->options.max_container_items / (size_t)(times == 0 ? 1 : times)) {
            ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "array repetition limit reached"); exec->flow = DS_FLOW_ERROR; return ds_null();
        }
        for (; times > 0; --times) for (i = 0; i < left.as.array->count; ++i)
            if (!ds_array_push(exec->context, array, left.as.array->items[i])) return ds_null();
        return ds_array_value(array);
    }
    if (kind == DS_AST_LT || kind == DS_AST_LE || kind == DS_AST_GE || kind == DS_AST_GT) {
        if (left.type == DICESCRIPT_VALUE_STRING && right.type == DICESCRIPT_VALUE_STRING) {
            int cmp = strcmp(left.as.string->bytes, right.as.string->bytes);
            if (kind == DS_AST_LT) return ds_int(cmp < 0);
            if (kind == DS_AST_LE) return ds_int(cmp <= 0);
            if (kind == DS_AST_GE) return ds_int(cmp >= 0);
            return ds_int(cmp > 0);
        }
    }
    if (!ds_numeric(exec, left, &a, &ia, &ai) || !ds_numeric(exec, right, &b, &ib, &bi)) return ds_null();
    switch (kind) {
        case DS_AST_SUBTRACT: return ai && bi ? ds_int(ia - ib) : ds_float(a - b);
        case DS_AST_ADD: return ai && bi ? ds_int(ia + ib) : ds_float(a + b);
        case DS_AST_MULTIPLY: return ai && bi ? ds_int(ia * ib) : ds_float(a * b);
        case DS_AST_DIVIDE:
            if (b == 0.0) { ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "division by zero"); exec->flow = DS_FLOW_ERROR; return ds_null(); }
            return ai && bi ? ds_int(ia / ib) : ds_float(a / b);
        case DS_AST_MODULUS:
            if (!ai || !bi || ib == 0) { ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid modulus operands"); exec->flow = DS_FLOW_ERROR; return ds_null(); }
            return ds_int(ia % ib);
        case DS_AST_POWER:
            result = pow(a, b);
            if (!isfinite(result)) { ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "power result is not finite"); exec->flow = DS_FLOW_ERROR; return ds_null(); }
            if (ai && bi && ib >= 0 && result >= (double)INT64_MIN && result <= (double)INT64_MAX) return ds_int((int64_t)result);
            return ds_float(result);
        case DS_AST_BIT_AND: if (!ai || !bi) break; return ds_int(ia & ib);
        case DS_AST_BIT_OR: if (!ai || !bi) break; return ds_int(ia | ib);
        case DS_AST_LT: return ds_int(a < b);
        case DS_AST_LE: return ds_int(a <= b);
        case DS_AST_GE: return ds_int(a >= b);
        case DS_AST_GT: return ds_int(a > b);
        default: break;
    }
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "unsupported operands");
    exec->flow = DS_FLOW_ERROR;
    return ds_null();
}

static int ds_compare_values(const void *left, const void *right) {
    const ds_value *a = (const ds_value *)left;
    const ds_value *b = (const ds_value *)right;
    double av = a->type == DICESCRIPT_VALUE_INT ? (double)a->as.integer : a->as.number;
    double bv = b->type == DICESCRIPT_VALUE_INT ? (double)b->as.integer : b->as.number;
    return av < bv ? -1 : (av > bv ? 1 : 0);
}

static ds_value ds_array_select(ds_exec *exec, ds_value receiver, size_t count, int high) {
    ds_value *copy;
    double total = 0.0;
    size_t i, start, numeric_count = 0;
    int all_int = 1;
    if (receiver.type != DICESCRIPT_VALUE_ARRAY || receiver.as.array == NULL) return ds_null();
    copy = (ds_value *)malloc(receiver.as.array->count * sizeof(*copy));
    if (copy == NULL && receiver.as.array->count != 0) { ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory"); exec->flow = DS_FLOW_ERROR; return ds_null(); }
    for (i = 0; i < receiver.as.array->count; ++i) {
        if (receiver.as.array->items[i].type == DICESCRIPT_VALUE_INT)
            copy[numeric_count++] = receiver.as.array->items[i];
        else if (receiver.as.array->items[i].type == DICESCRIPT_VALUE_FLOAT) {
            all_int = 0; copy[numeric_count++] = receiver.as.array->items[i];
        }
    }
    if (count > numeric_count) count = numeric_count;
    qsort(copy, numeric_count, sizeof(*copy), ds_compare_values);
    start = high ? numeric_count - count : 0;
    for (i = start; i < start + count; ++i)
        total += copy[i].type == DICESCRIPT_VALUE_INT ?
            (double)copy[i].as.integer : copy[i].as.number;
    free(copy);
    return all_int ? ds_int((int64_t)total) : ds_float(total);
}

static ds_value ds_eval_dice_via_numeric(ds_exec *exec, const char *expression) {
    dicescript_result result;
    if (!dicescript_eval(expression, &exec->context->options.dice, &result)) {
        ds_set_error(exec->context, result.error_kind, result.error);
        exec->flow = DS_FLOW_ERROR;
        return ds_null();
    }
    (void)snprintf(exec->context->detail, sizeof(exec->context->detail), "%s", result.detail);
    return result.is_integer ? ds_int(result.integer) : ds_float(result.number);
}

static ds_value ds_eval_dice(ds_exec *exec, ds_ast_t *node) {
    ds_value count_value = node->left != NULL ? ds_eval(exec, node->left, 0) : ds_int(1);
    ds_value faces_value = node->right != NULL ? ds_eval(exec, node->right, 0) : ds_int(exec->context->options.dice.default_faces);
    int64_t count, faces;
    ds_ast_t *modifier;
    ds_buffer expression;
    char number[64];
    ds_value output;
    if (!ds_integer(exec, count_value, &count) || !ds_integer(exec, faces_value, &faces)) return ds_null();
    ds_buffer_init(&expression);
    (void)snprintf(number, sizeof(number), "%" PRId64 "d%" PRId64, count, faces);
    ds_buffer_append(&expression, number);
    for (modifier = node->third; modifier != NULL; modifier = modifier->next) {
        ds_value amount_value;
        int64_t amount = 1;
        const char *name = "";
        if (modifier->left != NULL) {
            amount_value = ds_eval(exec, modifier->left, 0);
            if (!ds_integer(exec, amount_value, &amount)) { ds_buffer_free(&expression); return ds_null(); }
        }
        switch (modifier->auxiliary) {
            case DS_VM_KEEP_HIGH: name = "kh"; break;
            case DS_VM_KEEP_LOW: name = "kl"; break;
            case DS_VM_DROP_HIGH: name = "dh"; break;
            case DS_VM_DROP_LOW: name = "dl"; break;
            case DS_VM_MIN: name = "min"; break;
            case DS_VM_MAX: name = "max"; break;
            case DS_VM_ADVANTAGE: name = "优势"; amount = -1; break;
            case DS_VM_DISADVANTAGE: name = "劣势"; amount = -1; break;
            default: break;
        }
        ds_buffer_append(&expression, name);
        if (amount >= 0) { (void)snprintf(number, sizeof(number), "%" PRId64, amount); ds_buffer_append(&expression, number); }
    }
    output = ds_eval_dice_via_numeric(exec, expression.data != NULL ? expression.data : "");
    ds_buffer_free(&expression);
    return output;
}

static ds_value ds_eval_special_dice(ds_exec *exec, ds_ast_t *node) {
    ds_buffer expression;
    ds_value value;
    int64_t left = 1, right = 1;
    char text[64];
    ds_ast_t *option;
    ds_buffer_init(&expression);
    if (node->left != NULL) { value = ds_eval(exec, node->left, 0); if (!ds_integer(exec, value, &left)) goto fail; }
    if (node->right != NULL) { value = ds_eval(exec, node->right, 0); if (!ds_integer(exec, value, &right)) goto fail; }
    switch (node->kind) {
        case DS_AST_COC_BONUS: (void)snprintf(text, sizeof(text), "b%" PRId64, left); ds_buffer_append(&expression, text); break;
        case DS_AST_COC_PENALTY: (void)snprintf(text, sizeof(text), "p%" PRId64, left); ds_buffer_append(&expression, text); break;
        case DS_AST_FATE: ds_buffer_append(&expression, "f"); break;
        case DS_AST_WOD:
            (void)snprintf(text, sizeof(text), "%" PRId64 "a%" PRId64, left, right); ds_buffer_append(&expression, text);
            for (option = node->third; option != NULL; option = option->next) {
                value = ds_eval(exec, option->left, 0); if (!ds_integer(exec, value, &left)) goto fail;
                (void)snprintf(text, sizeof(text), "%c%" PRId64, option->auxiliary == 1 ? 'm' : (option->auxiliary == 2 ? 'k' : 'q'), left);
                ds_buffer_append(&expression, text);
            }
            break;
        case DS_AST_DOUBLE_CROSS:
            (void)snprintf(text, sizeof(text), "%" PRId64 "c%" PRId64, left, right); ds_buffer_append(&expression, text);
            for (option = node->third; option != NULL; option = option->next) {
                value = ds_eval(exec, option->left, 0); if (!ds_integer(exec, value, &left)) goto fail;
                (void)snprintf(text, sizeof(text), "m%" PRId64, left); ds_buffer_append(&expression, text);
            }
            break;
        default: break;
    }
    value = ds_eval_dice_via_numeric(exec, expression.data != NULL ? expression.data : "");
    ds_buffer_free(&expression);
    return value;
fail:
    ds_buffer_free(&expression);
    return ds_null();
}

static ds_value ds_call_native(ds_exec *exec, ds_callable *callable,
                               ds_value *args, size_t count) {
    const char *name = callable->name->bytes;
    ds_value receiver = callable->receiver;
    double number;
    int64_t integer;
    int is_integer;
    size_t i;
    if (!callable->has_receiver) {
        if (strcmp(name, "floor") == 0 || strcmp(name, "ceil") == 0 || strcmp(name, "round") == 0 || strcmp(name, "abs") == 0) {
            if (count != 1 || !ds_numeric(exec, args[0], &number, &integer, &is_integer)) return ds_null();
            if (strcmp(name, "floor") == 0) return ds_int((int64_t)floor(number));
            if (strcmp(name, "ceil") == 0) return ds_int((int64_t)ceil(number));
            if (strcmp(name, "round") == 0) return ds_int((int64_t)round(number));
            return is_integer ? ds_int(integer < 0 ? -integer : integer) : ds_float(fabs(number));
        }
        if (strcmp(name, "int") == 0 || strcmp(name, "toInt") == 0) {
            if (count != 1) goto bad_count;
            if (args[0].type == DICESCRIPT_VALUE_INT) return args[0];
            if (args[0].type == DICESCRIPT_VALUE_FLOAT) return ds_int((int64_t)args[0].as.number);
            if (args[0].type == DICESCRIPT_VALUE_STRING) {
                char *end = NULL; errno = 0; integer = strtoll(args[0].as.string->bytes, &end, 10);
                if (errno == 0 && end != args[0].as.string->bytes && *end == '\0') return ds_int(integer);
            }
            ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "cannot convert value to int"); exec->flow = DS_FLOW_ERROR; return ds_null();
        }
        if (strcmp(name, "float") == 0 || strcmp(name, "toFloat") == 0) {
            if (count != 1) goto bad_count;
            if (args[0].type == DICESCRIPT_VALUE_FLOAT) return args[0];
            if (args[0].type == DICESCRIPT_VALUE_INT) return ds_float((double)args[0].as.integer);
            if (args[0].type == DICESCRIPT_VALUE_STRING) {
                char *end = NULL; errno = 0; number = strtod(args[0].as.string->bytes, &end);
                if (errno == 0 && end != args[0].as.string->bytes && *end == '\0') return ds_float(number);
            }
            ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "cannot convert value to float"); exec->flow = DS_FLOW_ERROR; return ds_null();
        }
        if (strcmp(name, "bool") == 0 || strcmp(name, "toBool") == 0) { if (count != 1) goto bad_count; return ds_int(ds_truthy(args[0])); }
        if (strcmp(name, "str") == 0 || strcmp(name, "toStr") == 0 || strcmp(name, "repr") == 0) {
            ds_buffer buffer; ds_string *string;
            if (count != 1) goto bad_count;
            ds_buffer_init(&buffer); ds_value_format(exec, args[0], &buffer, strcmp(name, "repr") == 0, 0);
            string = ds_new_string_n(exec->context, buffer.data != NULL ? buffer.data : "", buffer.length);
            ds_buffer_free(&buffer); return ds_string_value(string);
        }
        if (strcmp(name, "typeId") == 0) { if (count != 1) goto bad_count; return ds_int((int64_t)args[0].type); }
        if (strcmp(name, "load") == 0 || strcmp(name, "loadRaw") == 0) {
            ds_value value;
            if (count != 1 || args[0].type != DICESCRIPT_VALUE_STRING) goto bad_arguments;
            if (!ds_lookup(exec, args[0].as.string->bytes, args[0].as.string->length,
                           strcmp(name, "loadRaw") == 0, &value)) return ds_null();
            return value;
        }
        if (strcmp(name, "store") == 0) {
            if (count != 2 || args[0].type != DICESCRIPT_VALUE_STRING) goto bad_arguments;
            if (!ds_store_name(exec, args[0].as.string->bytes, args[0].as.string->length, args[1])) return ds_null();
            return args[1];
        }
        if (strcmp(name, "loadRawAttr") == 0 || strcmp(name, "loadRawItem") == 0) {
            if (count != 2) goto bad_count;
            if (strcmp(name, "loadRawAttr") == 0) {
                if (args[1].type != DICESCRIPT_VALUE_STRING) goto bad_arguments;
                return ds_get_attr(exec, args[0], args[1].as.string->bytes, args[1].as.string->length, 1);
            }
            return ds_get_item(exec, args[0], args[1], 1);
        }
        if (strcmp(name, "dir") == 0) {
            ds_array *array = ds_new_array(exec->context);
            if (count != 1) goto bad_count;
            if (args[0].type == DICESCRIPT_VALUE_DICT) for (i = 0; i < args[0].as.dict->count; ++i)
                ds_array_push(exec->context, array, ds_string_value(args[0].as.dict->entries[i].key));
            return ds_array_value(array);
        }
    }
    if (receiver.type == DICESCRIPT_VALUE_COMPUTED && strcmp(name, "compute") == 0) {
        if (count != 0) goto bad_count;
        return ds_resolve_computed(exec, receiver);
    }
    if (receiver.type == DICESCRIPT_VALUE_ARRAY && receiver.as.array != NULL) {
        ds_array *array = receiver.as.array;
        if (strcmp(name, "len") == 0) { if (count != 0) goto bad_count; return ds_int((int64_t)array->count); }
        if (strcmp(name, "sum") == 0) {
            double total = 0.0; int all_int = 1; if (count != 0) goto bad_count;
            for (i = 0; i < array->count; ++i) {
                if (array->items[i].type == DICESCRIPT_VALUE_INT) total += (double)array->items[i].as.integer;
                else if (array->items[i].type == DICESCRIPT_VALUE_FLOAT) { total += array->items[i].as.number; all_int = 0; }
            }
            return all_int ? ds_int((int64_t)total) : ds_float(total);
        }
        if (strcmp(name, "kh") == 0 || strcmp(name, "kl") == 0) {
            size_t take = 1; if (count > 1) goto bad_count;
            if (count == 1) { if (!ds_integer(exec, args[0], &integer) || integer < 0) return ds_null(); take = (size_t)integer; }
            return ds_array_select(exec, receiver, take, strcmp(name, "kh") == 0);
        }
        if (strcmp(name, "push") == 0) { if (count != 1) goto bad_count; ds_array_push(exec->context, array, args[0]); return receiver; }
        if (strcmp(name, "pop") == 0) { if (count != 0) goto bad_count; return array->count == 0 ? ds_null() : array->items[--array->count]; }
        if (strcmp(name, "shift") == 0) {
            ds_value value; if (count != 0) goto bad_count; if (array->count == 0) return ds_null();
            value = array->items[0]; if (array->count > 1) memmove(array->items, array->items + 1, (array->count - 1) * sizeof(*array->items)); --array->count; return value;
        }
        if (strcmp(name, "shuffle") == 0) {
            if (count != 0) goto bad_count;
            for (i = array->count; i > 1; --i) {
                size_t j = (size_t)(exec->context->options.dice.random != NULL
                    ? exec->context->options.dice.random(exec->context->options.dice.random_userdata, i)
                    : (uint64_t)rand() % i);
                ds_value tmp = array->items[i - 1]; array->items[i - 1] = array->items[j]; array->items[j] = tmp;
            }
            return receiver;
        }
        if (strcmp(name, "rand") == 0) {
            size_t index; if (count != 0) goto bad_count; if (array->count == 0) return ds_null();
            index = (size_t)(exec->context->options.dice.random != NULL
                ? exec->context->options.dice.random(exec->context->options.dice.random_userdata, array->count)
                : (uint64_t)rand() % array->count); return array->items[index];
        }
        if (strcmp(name, "randSize") == 0) {
            ds_array *result; ds_value *copy; size_t take, j;
            if (count != 1 || !ds_integer(exec, args[0], &integer) || integer < 0) goto bad_arguments;
            take = (size_t)integer;
            if (take > array->count) { ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "randSize exceeds array length"); exec->flow = DS_FLOW_ERROR; return ds_null(); }
            copy = (ds_value *)malloc(array->count * sizeof(*copy));
            if (copy == NULL && array->count != 0) { ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory"); exec->flow = DS_FLOW_ERROR; return ds_null(); }
            if (array->count != 0) memcpy(copy, array->items, array->count * sizeof(*copy));
            for (i = array->count; i > 1; --i) {
                j = (size_t)(exec->context->options.dice.random != NULL ? exec->context->options.dice.random(exec->context->options.dice.random_userdata, i) : (uint64_t)rand() % i);
                { ds_value temp = copy[i - 1]; copy[i - 1] = copy[j]; copy[j] = temp; }
            }
            result = ds_new_array(exec->context);
            for (i = 0; i < take; ++i) ds_array_push(exec->context, result, copy[i]);
            free(copy);
            return ds_array_value(result);
        }
    }
    if (receiver.type == DICESCRIPT_VALUE_DICT && receiver.as.dict != NULL) {
        ds_dict *dict = receiver.as.dict;
        if (strcmp(name, "len") == 0) { if (count != 0) goto bad_count; return ds_int((int64_t)dict->count); }
        if (strcmp(name, "keys") == 0 || strcmp(name, "values") == 0 || strcmp(name, "items") == 0) {
            ds_array *array = ds_new_array(exec->context); if (count != 0) goto bad_count;
            for (i = 0; i < dict->count; ++i) {
                if (strcmp(name, "keys") == 0) ds_array_push(exec->context, array, ds_string_value(dict->entries[i].key));
                else if (strcmp(name, "values") == 0) ds_array_push(exec->context, array, dict->entries[i].value);
                else { ds_array *pair = ds_new_array(exec->context); ds_array_push(exec->context, pair, ds_string_value(dict->entries[i].key)); ds_array_push(exec->context, pair, dict->entries[i].value); ds_array_push(exec->context, array, ds_array_value(pair)); }
            }
            return ds_array_value(array);
        }
        if (strcmp(name, "has") == 0 || strcmp(name, "get") == 0 || strcmp(name, "getRaw") == 0) {
            ds_buffer key; ds_value value;
            if (count < 1 || count > 2) goto bad_count;
            ds_buffer_init(&key); ds_value_key(exec, args[0], &key);
            if (ds_dict_get_own(dict, key.data != NULL ? key.data : "", &value)) {
                ds_buffer_free(&key); return strcmp(name, "getRaw") == 0 ? value : ds_resolve_computed(exec, value);
            }
            ds_buffer_free(&key);
            if (strcmp(name, "has") == 0) return ds_int(0);
            return count == 2 ? args[1] : ds_null();
        }
    }
    if (receiver.type == DICESCRIPT_VALUE_STRING && strcmp(name, "len") == 0) {
        if (count != 0) goto bad_count;
        return ds_int((int64_t)ds_utf8_count(receiver.as.string->bytes, receiver.as.string->length));
    }
bad_arguments:
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid function arguments"); exec->flow = DS_FLOW_ERROR; return ds_null();
bad_count:
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid argument count"); exec->flow = DS_FLOW_ERROR; return ds_null();
}

static ds_value ds_call_function(ds_exec *exec, ds_function *function,
                                 ds_value *args, size_t count) {
    ds_frame frame;
    ds_dict *this_dict;
    ds_value result;
    size_t i;
    if (exec->call_depth + 1 > exec->context->options.max_call_depth) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "DiceScript call depth limit reached"); exec->flow = DS_FLOW_ERROR; return ds_null();
    }
    if (count != function->param_count) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION,
                     "function argument count does not match definition");
        exec->flow = DS_FLOW_ERROR; return ds_null();
    }
    memset(&frame, 0, sizeof(frame));
    frame.function_scope = 1;
    frame.parent = exec->frame;
    this_dict = (ds_dict *)ds_alloc(exec->context, sizeof(*this_dict));
    if (this_dict == NULL) return ds_null();
    frame.this_value = ds_dict_value(this_dict);
    if (function->has_bound_self && function->bound_self.type == DICESCRIPT_VALUE_DICT)
        ds_dict_set(exec->context, this_dict, "__proto__", function->bound_self);
    for (i = 0; i < function->param_count; ++i) {
        ds_value value = i < count ? args[i] : ds_null();
        ds_dict_set_n(exec->context, &frame.locals, function->params[i]->bytes, function->params[i]->length, value);
        ds_dict_set_n(exec->context, this_dict, function->params[i]->bytes, function->params[i]->length, value);
    }
    result = ds_run_source(exec, &frame, function->body->bytes, 0);
    return result;
}

static ds_value ds_call(ds_exec *exec, ds_value callable, ds_ast_t *arguments) {
    ds_value *args = NULL;
    size_t count = 0, capacity = 0;
    ds_ast_t *argument;
    ds_value result = ds_null();
    for (argument = arguments; argument != NULL; argument = argument->next) {
        if (count == capacity) {
            size_t next = capacity == 0 ? 4 : capacity * 2;
            ds_value *grown = (ds_value *)realloc(args, next * sizeof(*args));
            if (grown == NULL) { free(args); ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory"); exec->flow = DS_FLOW_ERROR; return ds_null(); }
            args = grown; capacity = next;
        }
        args[count++] = ds_eval(exec, argument, 0);
        if (exec->flow == DS_FLOW_ERROR) { free(args); return ds_null(); }
    }
    if (callable.type == DICESCRIPT_VALUE_NATIVE_FUNCTION && callable.as.callable != NULL)
        result = ds_call_native(exec, callable.as.callable, args, count);
    else if (callable.type == DICESCRIPT_VALUE_FUNCTION && callable.as.function != NULL)
        result = ds_call_function(exec, callable.as.function, args, count);
    else {
        ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "value is not callable");
        exec->flow = DS_FLOW_ERROR;
    }
    free(args);
    return result;
}

static ds_value ds_eval_postfix(ds_exec *exec, ds_ast_t *node, int raw) {
    int base_raw = raw;
    ds_value value;
    ds_ast_t *suffix;
    /* Computed values normally auto-evaluate on load.  Their own prototype
     * method is the exception: a.compute() must first expose the raw object. */
    if (!base_raw && node->right != NULL && node->right->kind == DS_AST_ATTR &&
        node->right->left != NULL) {
        const ds_ast_t *name_node = node->right->left;
        size_t name_length = name_node->source_end - name_node->source_start;
        if (name_length == 7 && name_node->source_end <= exec->source_length &&
            memcmp(exec->source + name_node->source_start, "compute", 7) == 0)
            base_raw = 1;
    }
    value = ds_eval(exec, node->left, base_raw);
    for (suffix = node->right; suffix != NULL && exec->flow != DS_FLOW_ERROR; suffix = suffix->next) {
        switch (suffix->kind) {
            case DS_AST_ATTR: {
                const char *name; size_t length;
                if (!ds_name_from_node(exec, suffix->left, &name, &length)) return ds_null();
                value = ds_get_attr(exec, value, name, length, raw);
                break;
            }
            case DS_AST_INDEX: value = ds_get_item(exec, value, ds_eval(exec, suffix->left, 0), raw); break;
            case DS_AST_SLICE: value = ds_slice_value(exec, value, suffix->left, suffix->right, suffix->third); break;
            case DS_AST_CALL: value = ds_call(exec, value, suffix->left); raw = 0; break;
            case DS_AST_ARRAY_KH:
            case DS_AST_ARRAY_KL: {
                int64_t count = 1; ds_value amount;
                if (suffix->left != NULL) { amount = ds_eval(exec, suffix->left, 0); if (!ds_integer(exec, amount, &count) || count < 0) return ds_null(); }
                value = ds_array_select(exec, value, (size_t)count, suffix->kind == DS_AST_ARRAY_KH);
                break;
            }
            default: break;
        }
    }
    return value;
}

static int ds_set_attr(ds_exec *exec, ds_value object,
                       const char *name, size_t length, ds_value value, int raw) {
    if (object.type == DICESCRIPT_VALUE_COMPUTED && object.as.computed != NULL)
        return ds_dict_set_n(exec->context, &object.as.computed->attributes, name, length, value);
    if (object.type == DICESCRIPT_VALUE_DICT && object.as.dict != NULL)
        return ds_dict_set_n(exec->context, object.as.dict, name, length, value);
    (void)raw;
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "attribute assignment requires dictionary or computed value");
    exec->flow = DS_FLOW_ERROR;
    return 0;
}

static int ds_set_item(ds_exec *exec, ds_value object, ds_value key, ds_value value) {
    int64_t index; size_t normalized;
    if (object.type == DICESCRIPT_VALUE_ARRAY && object.as.array != NULL) {
        if (!ds_integer(exec, key, &index)) return 0;
        if (!ds_normalize_index(index, object.as.array->count, &normalized)) {
            ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "array index out of range"); exec->flow = DS_FLOW_ERROR; return 0;
        }
        object.as.array->items[normalized] = value; return 1;
    }
    if (object.type == DICESCRIPT_VALUE_DICT && object.as.dict != NULL) {
        ds_buffer buffer; int ok;
        ds_buffer_init(&buffer); ds_value_key(exec, key, &buffer);
        ok = ds_dict_set(exec->context, object.as.dict, buffer.data != NULL ? buffer.data : "", value);
        ds_buffer_free(&buffer); return ok;
    }
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "item assignment requires array or dictionary"); exec->flow = DS_FLOW_ERROR; return 0;
}

static int ds_assign(ds_exec *exec, ds_ast_t *target, ds_value value, ds_ast_t *rhs) {
    int raw = 0;
    const char *name; size_t length;
    if (target != NULL && target->kind == DS_AST_RAW) {
        raw = 1;
        target = target->left;
        /* Only `&name = expr` creates a computed value.  In
         * `&name.attr = expr`, ampersand means raw object lookup. */
        if (target != NULL && target->kind == DS_AST_VARIABLE)
            value = ds_make_computed(exec, rhs);
    }
    if (target != NULL && target->kind == DS_AST_VARIABLE) {
        if (!ds_name_from_node(exec, target, &name, &length)) return 0;
        return ds_store_name(exec, name, length, value);
    }
    if (target != NULL && target->kind == DS_AST_POSTFIX) {
        ds_ast_t *suffix = target->right;
        ds_ast_t *last = suffix;
        ds_value object = ds_eval(exec, target->left, raw);
        while (last != NULL && last->next != NULL) last = last->next;
        while (suffix != NULL && suffix != last) {
            if (suffix->kind == DS_AST_ATTR) {
                if (!ds_name_from_node(exec, suffix->left, &name, &length)) return 0;
                object = ds_get_attr(exec, object, name, length, raw);
            } else if (suffix->kind == DS_AST_INDEX)
                object = ds_get_item(exec, object, ds_eval(exec, suffix->left, 0), raw);
            else { ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid assignment target"); exec->flow = DS_FLOW_ERROR; return 0; }
            suffix = suffix->next;
        }
        if (last != NULL && last->kind == DS_AST_ATTR) {
            if (!ds_name_from_node(exec, last->left, &name, &length)) return 0;
            return ds_set_attr(exec, object, name, length, value, raw);
        }
        if (last != NULL && last->kind == DS_AST_INDEX)
            return ds_set_item(exec, object, ds_eval(exec, last->left, 0), value);
        if (last != NULL && last->kind == DS_AST_SLICE)
            return ds_set_slice(exec, object, last->left, last->right,
                                last->third, value);
    }
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid assignment target");
    exec->flow = DS_FLOW_ERROR;
    return 0;
}

static ds_value ds_eval_template(ds_exec *exec, ds_ast_t *node) {
    size_t i = node->source_start + 1;
    size_t end = node->source_end > i ? node->source_end - 1 : i;
    ds_buffer output;
    ds_value result;
    ds_buffer_init(&output);
    if (!exec->context->options.enable_templates) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX, "DiceScript templates are disabled"); exec->flow = DS_FLOW_ERROR; return ds_null();
    }
    while (i < end && exec->flow != DS_FLOW_ERROR) {
        if (exec->source[i] == '\\' && i + 1 < end) {
            char ch = exec->source[i + 1];
            if (ch == 'n') ch = '\n'; else if (ch == 'r') ch = '\r'; else if (ch == 't') ch = '\t';
            ds_buffer_char(&output, ch); i += 2; continue;
        }
        if (exec->source[i] == '{') {
            int block = i + 1 < end && exec->source[i + 1] == '%';
            size_t code_start = i + (block ? 2 : 1), pos = code_start, depth = 1, code_end = end;
            while (pos < end) {
                if (block && pos + 1 < end && exec->source[pos] == '%' && exec->source[pos + 1] == '}') { code_end = pos; i = pos + 2; break; }
                if (!block) {
                    if (exec->source[pos] == '{') ++depth;
                    else if (exec->source[pos] == '}' && --depth == 0) { code_end = pos; i = pos + 1; break; }
                }
                ++pos;
            }
            if (code_end == end) { ds_set_error(exec->context, DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX, "unclosed template expression"); exec->flow = DS_FLOW_ERROR; break; }
            {
                char *code = ds_slice_copy_malloc(exec->source, code_start, code_end);
                ds_buffer rendered;
                if (code == NULL) { ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory"); exec->flow = DS_FLOW_ERROR; break; }
                result = ds_run_source(exec, exec->frame, code, !block);
                free(code);
                ds_buffer_init(&rendered);
                if (!(block && result.type == DICESCRIPT_VALUE_NULL)) ds_value_format(exec, result, &rendered, 0, 0);
                ds_buffer_append_n(&output, rendered.data != NULL ? rendered.data : "", rendered.length);
                ds_buffer_free(&rendered);
            }
            continue;
        }
        ds_buffer_char(&output, exec->source[i++]);
    }
    if (output.failed) { ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "template output is too large"); exec->flow = DS_FLOW_ERROR; }
    result = ds_string_value(ds_new_string_n(exec->context, output.data != NULL ? output.data : "", output.length));
    ds_buffer_free(&output);
    return result;
}

static int ds_special_dice_enabled(const dicescript_context *context, ds_ast_kind kind) {
    if (kind == DS_AST_COC_BONUS || kind == DS_AST_COC_PENALTY)
        return context->options.enable_dice_coc;
    if (kind == DS_AST_FATE) return context->options.enable_dice_fate;
    if (kind == DS_AST_WOD) return context->options.enable_dice_wod;
    if (kind == DS_AST_DOUBLE_CROSS) return context->options.enable_dice_double_cross;
    return 1;
}

static ds_value ds_disabled_special_as_identifier(ds_exec *exec, ds_ast_t *node) {
    size_t i, length;
    ds_value value;
    if (node->source_end > exec->source_length || node->source_end <= node->source_start)
        goto unsupported;
    length = node->source_end - node->source_start;
    for (i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)exec->source[node->source_start + i];
        if (!(isalnum(ch) || ch == '_' || ch == '$' || ch == ':' || ch >= 0x80))
            goto unsupported;
    }
    if (ds_lookup(exec, exec->source + node->source_start, length, 0, &value)) return value;
    return ds_null();
unsupported:
    ds_set_error(exec->context, DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX,
                 "special dice syntax is disabled");
    exec->flow = DS_FLOW_ERROR;
    return ds_null();
}

static ds_value ds_eval(ds_exec *exec, ds_ast_t *node, int raw) {
    ds_value left, right, value;
    const char *name; size_t length;
    char *text, *end;
    ds_ast_t *item;
    if (node == NULL || !ds_step(exec)) return ds_null();
    switch (node->kind) {
        case DS_AST_NULL: return ds_null();
        case DS_AST_TRUE: return ds_int(1);
        case DS_AST_FALSE: return ds_int(0);
        case DS_AST_INTEGER:
            text = ds_slice_copy_malloc(exec->source, node->source_start, node->source_end);
            if (text == NULL) return ds_null(); errno = 0; value = ds_int(strtoll(text, &end, 10));
            if (errno != 0 || *end != '\0') { ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid integer"); exec->flow = DS_FLOW_ERROR; }
            free(text); return value;
        case DS_AST_FLOAT:
            text = ds_slice_copy_malloc(exec->source, node->source_start, node->source_end);
            if (text == NULL) return ds_null(); errno = 0; value = ds_float(strtod(text, &end));
            if (errno != 0 || *end != '\0') { ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid float"); exec->flow = DS_FLOW_ERROR; }
            free(text); return value;
        case DS_AST_STRING: return ds_string_value(ds_unescape_literal(exec, node));
        case DS_AST_TEMPLATE: return ds_eval_template(exec, node);
        case DS_AST_VARIABLE:
            if (!ds_name_from_node(exec, node, &name, &length) || !ds_lookup(exec, name, length, raw, &value)) return ds_null();
            return value;
        case DS_AST_THIS: return exec->frame != NULL ? exec->frame->this_value : ds_null();
        case DS_AST_RAW: return ds_eval(exec, node->left, 1);
        case DS_AST_COMPUTED: return ds_make_computed(exec, node->left);
        case DS_AST_POSTFIX: return ds_eval_postfix(exec, node, raw);
        case DS_AST_ARRAY: {
            ds_array *array = ds_new_array(exec->context); if (array == NULL) return ds_null();
            for (item = node->left; item != NULL; item = item->next) if (!ds_array_push(exec->context, array, ds_eval(exec, item, 0))) return ds_null();
            return ds_array_value(array);
        }
        case DS_AST_RANGE: {
            ds_array *array = ds_new_array(exec->context); int64_t start, finish, step;
            left = ds_eval(exec, node->left, 0); right = ds_eval(exec, node->right, 0);
            if (!ds_integer(exec, left, &start) || !ds_integer(exec, right, &finish)) return ds_null();
            step = start <= finish ? 1 : -1;
            for (;;) { if (!ds_array_push(exec->context, array, ds_int(start))) return ds_null(); if (start == finish) break; start += step; }
            return ds_array_value(array);
        }
        case DS_AST_DICT: {
            ds_dict *dict = (ds_dict *)ds_alloc(exec->context, sizeof(*dict)); if (dict == NULL) return ds_null();
            for (item = node->left; item != NULL; item = item->next) {
                ds_buffer key; left = ds_eval(exec, item->left, 0); right = ds_eval(exec, item->right, 0);
                ds_buffer_init(&key); ds_value_key(exec, left, &key); ds_dict_set(exec->context, dict, key.data != NULL ? key.data : "", right); ds_buffer_free(&key);
            }
            return ds_dict_value(dict);
        }
        case DS_AST_POSITIVE: left = ds_eval(exec, node->left, 0); if (left.type != DICESCRIPT_VALUE_INT && left.type != DICESCRIPT_VALUE_FLOAT) { ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid unary plus"); exec->flow = DS_FLOW_ERROR; } return left;
        case DS_AST_NEGATIVE:
            left = ds_eval(exec, node->left, 0); if (left.type == DICESCRIPT_VALUE_INT) return ds_int(-left.as.integer); if (left.type == DICESCRIPT_VALUE_FLOAT) return ds_float(-left.as.number);
            ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid unary minus"); exec->flow = DS_FLOW_ERROR; return ds_null();
        case DS_AST_LOGIC_OR: left = ds_eval(exec, node->left, 0); return ds_truthy(left) ? left : ds_eval(exec, node->right, 0);
        case DS_AST_LOGIC_AND: left = ds_eval(exec, node->left, 0); return ds_truthy(left) ? ds_eval(exec, node->right, 0) : left;
        case DS_AST_NULL_COALESCE: left = ds_eval(exec, node->left, 0); return left.type != DICESCRIPT_VALUE_NULL ? left : ds_eval(exec, node->right, 0);
        case DS_AST_TERNARY:
            if (node->third != NULL) {
                left = ds_eval(exec, node->left, 0);
                return ds_truthy(left) ? ds_eval(exec, node->right, 0)
                                       : ds_eval(exec, node->third, 0);
            }
            for (item = node; item != NULL; item = item->next) {
                left = ds_eval(exec, item->left, 0);
                if (exec->flow == DS_FLOW_ERROR) return ds_null();
                if (ds_truthy(left)) return ds_eval(exec, item->right, 0);
            }
            return ds_string_value(ds_new_string(exec->context, ""));
        case DS_AST_ASSIGN: {
            int computed_assignment = node->left != NULL &&
                node->left->kind == DS_AST_RAW && node->left->left != NULL &&
                node->left->left->kind == DS_AST_VARIABLE;
            if (computed_assignment) right = ds_null();
            else right = ds_eval(exec, node->right, 0);
            if (!ds_assign(exec, node->left, right, node->right)) return ds_null();
            return computed_assignment ? ds_eval(exec, node->left, 1) : right;
        }
        case DS_AST_DICE: return ds_eval_dice(exec, node);
        case DS_AST_COC_BONUS: case DS_AST_COC_PENALTY: case DS_AST_FATE: case DS_AST_WOD: case DS_AST_DOUBLE_CROSS:
            if (!ds_special_dice_enabled(exec->context, node->kind))
                return ds_disabled_special_as_identifier(exec, node);
            return ds_eval_special_dice(exec, node);
        default:
            left = ds_eval(exec, node->left, 0); if (exec->flow == DS_FLOW_ERROR) return ds_null();
            right = ds_eval(exec, node->right, 0); if (exec->flow == DS_FLOW_ERROR) return ds_null();
            return ds_binary(exec, node->kind, left, right);
    }
}

static ds_value ds_execute_statement(ds_exec *exec, ds_ast_t *statement);

static ds_value ds_execute_list(ds_exec *exec, ds_ast_t *statement) {
    ds_value last = ds_null();
    for (; statement != NULL && exec->flow == DS_FLOW_NORMAL; statement = statement->next)
        last = ds_execute_statement(exec, statement);
    return last;
}

static ds_value ds_execute_statement(ds_exec *exec, ds_ast_t *statement) {
    ds_value value = ds_null();
    const char *name; size_t length, count, i;
    ds_ast_t *param;
    if (!ds_step(exec)) return value;
    switch (statement->kind) {
        case DS_AST_EXPR_STMT: return ds_eval(exec, statement->left, 0);
        case DS_AST_BLOCK: return ds_execute_list(exec, statement->left);
        case DS_AST_IF:
            value = ds_eval(exec, statement->left, 0);
            if (ds_truthy(value)) (void)ds_execute_statement(exec, statement->right);
            else if (statement->third != NULL) (void)ds_execute_statement(exec, statement->third);
            if (exec->flow == DS_FLOW_RETURN) return exec->flow_value;
            return ds_null();
        case DS_AST_WHILE:
            if (++exec->loop_depth > exec->context->options.max_call_depth) { ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "loop nesting limit reached"); exec->flow = DS_FLOW_ERROR; return ds_null(); }
            for (;;) {
                value = ds_eval(exec, statement->left, 0); if (exec->flow == DS_FLOW_ERROR || !ds_truthy(value)) break;
                value = ds_execute_statement(exec, statement->right);
                if (exec->flow == DS_FLOW_BREAK) { exec->flow = DS_FLOW_NORMAL; break; }
                if (exec->flow == DS_FLOW_CONTINUE) exec->flow = DS_FLOW_NORMAL;
                if (exec->flow != DS_FLOW_NORMAL) break;
            }
            --exec->loop_depth; return exec->flow == DS_FLOW_RETURN ? exec->flow_value : ds_null();
        case DS_AST_FUNCTION_DEF: {
            ds_function *function;
            char *body;
            if (!ds_name_from_node(exec, statement->left, &name, &length)) return ds_null();
            function = (ds_function *)ds_alloc(exec->context, sizeof(*function)); if (function == NULL) return ds_null();
            function->name = ds_new_string_n(exec->context, name, length);
            count = 0; for (param = statement->right; param != NULL; param = param->next) ++count;
            function->param_count = count;
            if (count != 0) function->params = (ds_string **)ds_alloc(exec->context, count * sizeof(*function->params));
            for (param = statement->right, i = 0; param != NULL; param = param->next, ++i) {
                if (!ds_name_from_node(exec, param, &name, &length)) return ds_null();
                function->params[i] = ds_new_string_n(exec->context, name, length);
            }
            if (statement->third->source_end <= statement->third->source_start + 1) body = ds_slice_copy_malloc("", 0, 0);
            else body = ds_slice_copy_malloc(exec->source, statement->third->source_start + 1, statement->third->source_end - 1);
            if (body == NULL) { ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory"); exec->flow = DS_FLOW_ERROR; return ds_null(); }
            function->body = ds_new_string(exec->context, body); free(body);
            value.type = DICESCRIPT_VALUE_FUNCTION; value.as.function = function;
            ds_store_name(exec, function->name->bytes, function->name->length, value);
            return value;
        }
        case DS_AST_DICE_FLAG:
            if (!ds_name_from_node(exec, statement->left, &name, &length)) return ds_null();
            if (length == 3 && memcmp(name, "wod", 3) == 0)
                exec->context->options.enable_dice_wod = statement->auxiliary;
            else if (length == 3 && memcmp(name, "coc", 3) == 0)
                exec->context->options.enable_dice_coc = statement->auxiliary;
            else if (length == 4 && memcmp(name, "fate", 4) == 0)
                exec->context->options.enable_dice_fate = statement->auxiliary;
            else if (length == 11 && memcmp(name, "doublecross", 11) == 0)
                exec->context->options.enable_dice_double_cross = statement->auxiliary;
            else {
                ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "unknown #EnableDice target");
                exec->flow = DS_FLOW_ERROR;
            }
            return ds_null();
        case DS_AST_RETURN:
            exec->flow_value = statement->left != NULL ? ds_eval(exec, statement->left, 0) : ds_null();
            if (exec->flow != DS_FLOW_ERROR) exec->flow = DS_FLOW_RETURN;
            return exec->flow_value;
        case DS_AST_BREAK:
            if (exec->loop_depth == 0) { ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "break is not allowed outside loop"); exec->flow = DS_FLOW_ERROR; }
            else exec->flow = DS_FLOW_BREAK; return ds_null();
        case DS_AST_CONTINUE:
            if (exec->loop_depth == 0) { ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "continue is not allowed outside loop"); exec->flow = DS_FLOW_ERROR; }
            else exec->flow = DS_FLOW_CONTINUE; return ds_null();
        default: return ds_eval(exec, statement, 0);
    }
}

static ds_string *ds_st_name(ds_exec *exec, const ds_ast_t *node) {
    if (node == NULL || node->kind != DS_AST_ST_NAME) return NULL;
    if (node->auxiliary) {
        ds_ast_t literal = *node;
        literal.kind = DS_AST_STRING;
        return ds_unescape_literal(exec, &literal);
    }
    return ds_ast_text(exec, node);
}

static int ds_st_serialize(ds_exec *exec, ds_value value, ds_buffer *output) {
    const void **ancestors = (const void **)calloc(
        (size_t)exec->context->options.max_call_depth + 1u,
        sizeof(*ancestors));
    int ok;
    if (ancestors == NULL) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT, "out of memory");
        exec->flow = DS_FLOW_ERROR; return 0;
    }
    ds_buffer_init(output);
    ok = ds_value_tagged_json(exec, value, output, 0, ancestors);
    free(ancestors);
    return ok;
}

static ds_value ds_execute_st(ds_exec *exec, ds_ast_t *root) {
    ds_ast_t *item;
    ds_value value = ds_null(), extra = ds_null();
    for (item = root != NULL ? root->left : NULL;
         item != NULL && exec->flow == DS_FLOW_NORMAL; item = item->next) {
        ds_string *name;
        const char *operation = "set";
        const char *operator_text = "";
        ds_buffer value_json, extra_json;
        int callback_result;
        if (!ds_step(exec) || item->kind != DS_AST_ST_ITEM ||
            item->right == NULL || item->right->kind != DS_AST_ST_VALUE) break;
        name = ds_st_name(exec, item->left);
        if (name == NULL) {
            ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid ^st attribute name");
            exec->flow = DS_FLOW_ERROR; break;
        }
        if (item->auxiliary == DS_VM_ST_SET_COMPUTED)
            value = ds_make_computed(exec, item->right);
        else value = ds_eval(exec, item->right->left, 0);
        if (exec->flow == DS_FLOW_ERROR) break;
        if (item->auxiliary == DS_VM_ST_SET_X1) {
            if (item->third == NULL || item->third->kind != DS_AST_ST_VALUE) {
                ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION, "invalid ^st extra value");
                exec->flow = DS_FLOW_ERROR; break;
            }
            extra = ds_eval(exec, item->third->left, 0);
            if (exec->flow == DS_FLOW_ERROR) break;
        }
        if (item->auxiliary == DS_VM_ST_MOD_ADD) { operation = "mod"; operator_text = "+"; }
        else if (item->auxiliary == DS_VM_ST_MOD_SUBTRACT) { operation = "mod"; operator_text = "-"; }
        else if (item->auxiliary == DS_VM_ST_MOD_SUBTRACT_ASSIGN) { operation = "mod"; operator_text = "-="; }
        else if (item->auxiliary == DS_VM_ST_SET_X0) operation = "set.x0";
        else if (item->auxiliary == DS_VM_ST_SET_X1) operation = "set.x1";
        if (exec->context->st_callback == NULL) continue;
        if (!ds_st_serialize(exec, value, &value_json)) break;
        ds_buffer_init(&extra_json);
        if (item->auxiliary == DS_VM_ST_SET_X1 &&
            !ds_st_serialize(exec, extra, &extra_json)) {
            ds_buffer_free(&value_json); break;
        }
        callback_result = exec->context->st_callback(
            exec->context->st_userdata, operation, name->bytes,
            value_json.data != NULL ? value_json.data : "",
            item->auxiliary == DS_VM_ST_SET_X1 ?
                (extra_json.data != NULL ? extra_json.data : "") : NULL,
            operator_text, exec->context->detail);
        ds_buffer_free(&value_json); ds_buffer_free(&extra_json);
        if (callback_result != 0) {
            ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION,
                         "host rejected DiceScript ^st operation");
            exec->flow = DS_FLOW_ERROR; break;
        }
    }
    return exec->flow == DS_FLOW_ERROR ? ds_null() : value;
}

static int ds_parse_full(const char *source, uint32_t max_nodes,
                         ds_vm_parser_t *state) {
    dsfull_context_t *parser;
    ds_ast_t *root = NULL;
    int parsed;
    memset(state, 0, sizeof(*state));
    state->source = source;
    state->source_length = strlen(source);
    state->node_capacity = max_nodes;
    state->nodes = (ds_ast_t *)calloc(max_nodes, sizeof(*state->nodes));
    if (state->nodes == NULL) { state->error_kind = DICESCRIPT_ERROR_LIMIT; (void)snprintf(state->error, sizeof(state->error), "out of memory"); return 0; }
    parser = dsfull_create(state);
    if (parser == NULL) { free(state->nodes); state->nodes = NULL; return 0; }
    parsed = dsfull_parse(parser, &root);
    dsfull_destroy(parser);
    if (!parsed || state->parser_error || state->root == NULL || state->error_kind != DICESCRIPT_ERROR_NONE) {
        if (state->error_kind == DICESCRIPT_ERROR_NONE) state->error_kind = DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX;
        if (state->error[0] == '\0') (void)snprintf(state->error, sizeof(state->error), "unsupported DiceScript syntax near byte %zu", state->input_position);
        free(state->nodes); state->nodes = NULL; return 0;
    }
    return 1;
}

static ds_value ds_run_source(ds_exec *parent, ds_frame *frame,
                              const char *source, int expression_only) {
    ds_vm_parser_t parser;
    ds_exec child;
    ds_value result = ds_null();
    if (!ds_parse_full(source, parent->context->options.dice.max_ast_nodes, &parser)) {
        ds_set_error(parent->context, parser.error_kind, parser.error);
        parent->flow = DS_FLOW_ERROR;
        return result;
    }
    memset(&child, 0, sizeof(child));
    child.context = parent->context;
    child.frame = frame;
    child.source = source;
    child.source_length = strlen(source);
    child.call_depth = parent->call_depth + 1;
    child.loop_depth = parent->loop_depth;
    if (expression_only) {
        ds_ast_t *only = parser.root->left;
        if (only == NULL || only->next != NULL || only->kind != DS_AST_EXPR_STMT) {
            ds_set_error(parent->context, DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX, "expression-only entry does not allow statements");
            child.flow = DS_FLOW_ERROR;
        } else result = ds_eval(&child, only->left, 0);
    } else if (parser.root->kind == DS_AST_ST_ROOT) result = ds_execute_st(&child, parser.root);
    else result = ds_execute_statement(&child, parser.root);
    if (child.flow == DS_FLOW_RETURN) { result = child.flow_value; child.flow = DS_FLOW_NORMAL; }
    if (child.flow == DS_FLOW_ERROR) parent->flow = DS_FLOW_ERROR;
    free(parser.nodes);
    return result;
}

static void ds_value_format(ds_exec *exec, ds_value value,
                            ds_buffer *buffer, int repr, uint32_t depth) {
    char number[96]; size_t i;
    if (depth > exec->context->options.max_call_depth) { ds_buffer_append(buffer, "..."); return; }
    switch (value.type) {
        case DICESCRIPT_VALUE_INT: (void)snprintf(number, sizeof(number), "%" PRId64, value.as.integer); ds_buffer_append(buffer, number); break;
        case DICESCRIPT_VALUE_FLOAT: (void)snprintf(number, sizeof(number), "%.15g", value.as.number); ds_buffer_append(buffer, number); break;
        case DICESCRIPT_VALUE_NULL: ds_buffer_append(buffer, "null"); break;
        case DICESCRIPT_VALUE_STRING:
            if (repr) ds_buffer_char(buffer, '\'');
            for (i = 0; i < value.as.string->length; ++i) {
                char ch = value.as.string->bytes[i];
                if (repr && (ch == '\\' || ch == '\'')) ds_buffer_char(buffer, '\\');
                ds_buffer_char(buffer, ch);
            }
            if (repr) ds_buffer_char(buffer, '\'');
            break;
        case DICESCRIPT_VALUE_ARRAY:
            ds_buffer_char(buffer, '[');
            for (i = 0; i < value.as.array->count; ++i) { if (i) ds_buffer_append(buffer, ", "); ds_value_format(exec, value.as.array->items[i], buffer, 1, depth + 1); }
            ds_buffer_char(buffer, ']'); break;
        case DICESCRIPT_VALUE_DICT:
            ds_buffer_char(buffer, '{');
            for (i = 0; i < value.as.dict->count; ++i) { if (i) ds_buffer_append(buffer, ", "); ds_buffer_char(buffer, '\''); ds_buffer_append_n(buffer, value.as.dict->entries[i].key->bytes, value.as.dict->entries[i].key->length); ds_buffer_append(buffer, "': "); ds_value_format(exec, value.as.dict->entries[i].value, buffer, 1, depth + 1); }
            ds_buffer_char(buffer, '}'); break;
        case DICESCRIPT_VALUE_COMPUTED: ds_buffer_append(buffer, "&("); ds_buffer_append_n(buffer, value.as.computed->expression->bytes, value.as.computed->expression->length); ds_buffer_char(buffer, ')'); break;
        case DICESCRIPT_VALUE_FUNCTION: ds_buffer_append(buffer, "function "); ds_buffer_append_n(buffer, value.as.function->name->bytes, value.as.function->name->length); break;
        case DICESCRIPT_VALUE_NATIVE_FUNCTION: ds_buffer_append(buffer, "nfunction "); ds_buffer_append_n(buffer, value.as.callable->name->bytes, value.as.callable->name->length); break;
        default: ds_buffer_append(buffer, "nobject"); break;
    }
}

static void ds_fill_script_result(dicescript_context *context, ds_value value,
                                  dicescript_script_result *result) {
    ds_exec exec; ds_buffer text, repr;
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->ok = context->error_kind == DICESCRIPT_ERROR_NONE;
    result->type = value.type;
    result->error_kind = context->error_kind;
    result->steps = context->steps;
    if (value.type == DICESCRIPT_VALUE_INT) { result->integer = value.as.integer; result->number = (double)value.as.integer; }
    if (value.type == DICESCRIPT_VALUE_FLOAT) result->number = value.as.number;
    memset(&exec, 0, sizeof(exec)); exec.context = context;
    ds_buffer_init(&text); ds_buffer_init(&repr);
    ds_value_format(&exec, value, &text, 0, 0); ds_value_format(&exec, value, &repr, 1, 0);
    (void)snprintf(result->text, sizeof(result->text), "%s", text.data != NULL ? text.data : "");
    (void)snprintf(result->repr, sizeof(result->repr), "%s", repr.data != NULL ? repr.data : "");
    (void)snprintf(result->detail, sizeof(result->detail), "%s", context->detail);
    (void)snprintf(result->error, sizeof(result->error), "%s", context->error);
    ds_buffer_free(&text); ds_buffer_free(&repr);
}

void dicescript_default_runtime_options(dicescript_runtime_options *options) {
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    dicescript_default_options(&options->dice);
    options->max_steps = 100000;
    options->max_memory_bytes = 16u * 1024u * 1024u;
    options->max_container_items = 20000;
    options->max_call_depth = 128;
    options->enable_statements = 1;
    options->enable_templates = 1;
    options->enable_dice_coc = 0;
    options->enable_dice_fate = 0;
    options->enable_dice_wod = 0;
    options->enable_dice_double_cross = 0;
}

dicescript_context *dicescript_context_create(const dicescript_runtime_options *options) {
    dicescript_context *context = (dicescript_context *)calloc(1, sizeof(*context));
    if (context == NULL) return NULL;
    if (options != NULL) context->options = *options;
    else dicescript_default_runtime_options(&context->options);
    if (context->options.dice.max_ast_nodes == 0) context->options.dice.max_ast_nodes = 4096;
    if (context->options.max_steps == 0) context->options.max_steps = 100000;
    if (context->options.max_memory_bytes == 0) context->options.max_memory_bytes = 16u * 1024u * 1024u;
    if (context->options.max_container_items == 0) context->options.max_container_items = 20000;
    if (context->options.max_call_depth == 0) context->options.max_call_depth = 128;
    return context;
}

void dicescript_context_destroy(dicescript_context *context) {
    if (context == NULL) return;
    ds_free_all(context);
    free(context);
}

void dicescript_context_clear(dicescript_context *context) {
    if (context == NULL) return;
    ds_free_all(context);
    memset(&context->globals, 0, sizeof(context->globals));
    context->error_kind = DICESCRIPT_ERROR_NONE;
    context->error[0] = '\0'; context->detail[0] = '\0'; context->steps = 0;
}

void dicescript_context_set_host_callbacks(dicescript_context *context,
                                           const dicescript_host_callbacks *callbacks) {
    if (context == NULL) return;
    if (callbacks != NULL) context->host = *callbacks;
    else memset(&context->host, 0, sizeof(context->host));
}

void dicescript_context_set_st_callback(dicescript_context *context,
                                        dicescript_st_callback_fn callback,
                                        void *userdata) {
    if (context == NULL) return;
    context->st_callback = callback;
    context->st_userdata = callback != NULL ? userdata : NULL;
}

static int ds_context_execute(dicescript_context *context, const char *source,
                              int expression_only, dicescript_script_result *result) {
    ds_exec root; ds_frame frame; ds_value value;
    if (context == NULL || source == NULL || result == NULL) return 0;
    context->error_kind = DICESCRIPT_ERROR_NONE; context->error[0] = '\0'; context->detail[0] = '\0'; context->steps = 0;
    memset(&root, 0, sizeof(root)); memset(&frame, 0, sizeof(frame));
    root.context = context; root.frame = &frame; root.source = source; root.source_length = strlen(source);
    if (!expression_only && !context->options.enable_statements) {
        ds_set_error(context, DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX, "DiceScript statements are disabled"); value = ds_null();
    } else value = ds_run_source(&root, &frame, source, expression_only);
    ds_fill_script_result(context, value, result);
    return result->ok;
}

int dicescript_context_run(dicescript_context *context, const char *script,
                           dicescript_script_result *result) {
    return ds_context_execute(context, script, 0, result);
}

int dicescript_context_eval(dicescript_context *context, const char *expression,
                            dicescript_script_result *result) {
    return ds_context_execute(context, expression, 1, result);
}

int dicescript_context_format(dicescript_context *context, const char *template_text,
                              dicescript_script_result *result) {
    ds_buffer script; int ok;
    if (template_text == NULL) return 0;
    ds_buffer_init(&script); ds_buffer_char(&script, 0x1e); ds_buffer_append(&script, template_text); ds_buffer_char(&script, 0x1e);
    ok = ds_context_execute(context, script.data != NULL ? script.data : "", 1, result); ds_buffer_free(&script); return ok;
}

int dicescript_context_set_int(dicescript_context *context, const char *name, int64_t value) {
    if (context == NULL || name == NULL || *name == '\0') return 0;
    return ds_dict_set(context, &context->globals, name, ds_int(value));
}

int dicescript_context_set_float(dicescript_context *context, const char *name, double value) {
    if (context == NULL || name == NULL || *name == '\0' || !isfinite(value)) return 0;
    return ds_dict_set(context, &context->globals, name, ds_float(value));
}

int dicescript_context_set_string(dicescript_context *context, const char *name, const char *value) {
    ds_string *string;
    if (context == NULL || name == NULL || *name == '\0') return 0;
    string = ds_new_string(context, value != NULL ? value : "");
    return string != NULL && ds_dict_set(context, &context->globals, name, ds_string_value(string));
}

int dicescript_context_unset(dicescript_context *context, const char *name) {
    return context != NULL && name != NULL && ds_dict_remove(&context->globals, name);
}

int dicescript_context_get(dicescript_context *context, const char *name,
                           dicescript_script_result *result) {
    ds_exec exec; ds_frame frame; ds_value value;
    if (context == NULL || name == NULL || result == NULL) return 0;
    context->error_kind = DICESCRIPT_ERROR_NONE; context->error[0] = '\0'; context->steps = 0;
    memset(&exec, 0, sizeof(exec)); memset(&frame, 0, sizeof(frame)); exec.context = context; exec.frame = &frame; exec.source = "";
    if (!ds_lookup(&exec, name, strlen(name), 0, &value)) value = ds_null();
    ds_fill_script_result(context, value, result); return result->ok;
}

int dicescript_context_set_json(dicescript_context *context, const char *name,
                                const char *json_text, dicescript_script_result *result) {
    ds_exec exec; ds_frame frame; ds_value value;
    if (context == NULL || name == NULL || json_text == NULL || result == NULL) return 0;
    context->error_kind = DICESCRIPT_ERROR_NONE; context->error[0] = '\0'; context->steps = 0;
    memset(&exec, 0, sizeof(exec)); memset(&frame, 0, sizeof(frame)); exec.context = context; exec.frame = &frame; exec.source = json_text; exec.source_length = strlen(json_text);
    value = ds_run_source(&exec, &frame, json_text, 1);
    if (exec.flow != DS_FLOW_ERROR) ds_dict_set(context, &context->globals, name, value);
    ds_fill_script_result(context, value, result); return result->ok;
}

static void ds_json_escape(ds_buffer *buffer, const ds_string *string) {
    size_t i; ds_buffer_char(buffer, '"');
    for (i = 0; i < string->length; ++i) {
        unsigned char ch = (unsigned char)string->bytes[i];
        if (ch == '"' || ch == '\\') { ds_buffer_char(buffer, '\\'); ds_buffer_char(buffer, (char)ch); }
        else if (ch == '\n') ds_buffer_append(buffer, "\\n");
        else if (ch == '\r') ds_buffer_append(buffer, "\\r");
        else if (ch == '\t') ds_buffer_append(buffer, "\\t");
        else if (ch < 0x20) { char escaped[8]; (void)snprintf(escaped, sizeof(escaped), "\\u%04x", ch); ds_buffer_append(buffer, escaped); }
        else ds_buffer_char(buffer, (char)ch);
    }
    ds_buffer_char(buffer, '"');
}

static int ds_value_json(ds_exec *exec, ds_value value, ds_buffer *buffer, uint32_t depth) {
    char number[96]; size_t i;
    if (depth > exec->context->options.max_call_depth) return 0;
    switch (value.type) {
        case DICESCRIPT_VALUE_NULL: return ds_buffer_append(buffer, "null");
        case DICESCRIPT_VALUE_INT: (void)snprintf(number, sizeof(number), "%" PRId64, value.as.integer); return ds_buffer_append(buffer, number);
        case DICESCRIPT_VALUE_FLOAT: (void)snprintf(number, sizeof(number), "%.15g", value.as.number); return ds_buffer_append(buffer, number);
        case DICESCRIPT_VALUE_STRING: ds_json_escape(buffer, value.as.string); return !buffer->failed;
        case DICESCRIPT_VALUE_ARRAY:
            ds_buffer_char(buffer, '['); for (i = 0; i < value.as.array->count; ++i) { if (i) ds_buffer_char(buffer, ','); if (!ds_value_json(exec, value.as.array->items[i], buffer, depth + 1)) return 0; } return ds_buffer_char(buffer, ']');
        case DICESCRIPT_VALUE_DICT:
            ds_buffer_char(buffer, '{'); for (i = 0; i < value.as.dict->count; ++i) { if (i) ds_buffer_char(buffer, ','); ds_json_escape(buffer, value.as.dict->entries[i].key); ds_buffer_char(buffer, ':'); if (!ds_value_json(exec, value.as.dict->entries[i].value, buffer, depth + 1)) return 0; } return ds_buffer_char(buffer, '}');
        default: return 0;
    }
}

int dicescript_context_get_json(dicescript_context *context, const char *name,
                                char *buffer, size_t buffer_size) {
    ds_exec exec; ds_frame frame; ds_value value; ds_buffer output; int ok;
    if (context == NULL || name == NULL || buffer == NULL || buffer_size == 0) return 0;
    memset(&exec, 0, sizeof(exec)); memset(&frame, 0, sizeof(frame)); exec.context = context; exec.frame = &frame; exec.source = "";
    if (!ds_lookup(&exec, name, strlen(name), 1, &value)) return 0;
    ds_buffer_init(&output); ok = ds_value_json(&exec, value, &output, 0);
    if (ok && output.length + 1 <= buffer_size) memcpy(buffer, output.data, output.length + 1); else ok = 0;
    ds_buffer_free(&output); return ok;
}

static int ds_tagged_cycle(ds_exec *exec, const void **ancestors,
                           uint32_t depth, const void *identity) {
    uint32_t i;
    if (depth > exec->context->options.max_call_depth) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT,
                     "DiceScript serialization depth limit reached");
        exec->flow = DS_FLOW_ERROR; return 1;
    }
    if (identity == NULL) return 0;
    for (i = 0; i < depth; ++i) if (ancestors[i] == identity) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION,
                     "cyclic value cannot be serialized");
        exec->flow = DS_FLOW_ERROR; return 1;
    }
    ancestors[depth] = identity;
    return 0;
}

static int ds_value_tagged_json(ds_exec *exec, ds_value value,
                                ds_buffer *buffer, uint32_t depth,
                                const void **ancestors) {
    char number[96]; size_t i; const void *identity = NULL;
    if (value.type == DICESCRIPT_VALUE_ARRAY) identity = value.as.array;
    else if (value.type == DICESCRIPT_VALUE_DICT) identity = value.as.dict;
    else if (value.type == DICESCRIPT_VALUE_COMPUTED) identity = value.as.computed;
    if (identity != NULL && ds_tagged_cycle(exec, ancestors, depth, identity)) return 0;
    switch (value.type) {
        case DICESCRIPT_VALUE_INT:
            (void)snprintf(number, sizeof(number), "{\"t\":0,\"v\":%" PRId64 "}", value.as.integer);
            return ds_buffer_append(buffer, number);
        case DICESCRIPT_VALUE_FLOAT:
            if (!isfinite(value.as.number)) goto unsupported;
            (void)snprintf(number, sizeof(number), "{\"t\":1,\"v\":%.15g}", value.as.number);
            return ds_buffer_append(buffer, number);
        case DICESCRIPT_VALUE_STRING:
            ds_buffer_append(buffer, "{\"t\":2,\"v\":");
            ds_json_escape(buffer, value.as.string);
            return ds_buffer_char(buffer, '}');
        case DICESCRIPT_VALUE_NULL:
            return ds_buffer_append(buffer, "{\"t\":4}");
        case DICESCRIPT_VALUE_COMPUTED:
            if (value.as.computed == NULL) goto unsupported;
            ds_buffer_append(buffer, "{\"t\":5,\"v\":{\"expr\":");
            ds_json_escape(buffer, value.as.computed->expression);
            if (value.as.computed->attributes.count != 0) {
                ds_buffer_append(buffer, ",\"attrs\":{");
                for (i = 0; i < value.as.computed->attributes.count; ++i) {
                    if (i != 0) ds_buffer_char(buffer, ',');
                    ds_json_escape(buffer, value.as.computed->attributes.entries[i].key);
                    ds_buffer_char(buffer, ':');
                    if (!ds_value_tagged_json(exec, value.as.computed->attributes.entries[i].value,
                                              buffer, depth + 1, ancestors)) return 0;
                }
                ds_buffer_char(buffer, '}');
            }
            return ds_buffer_append(buffer, "}}");
        case DICESCRIPT_VALUE_ARRAY:
            if (value.as.array == NULL) goto unsupported;
            ds_buffer_append(buffer, "{\"t\":6,\"v\":{\"list\":[");
            for (i = 0; i < value.as.array->count; ++i) {
                if (i != 0) ds_buffer_char(buffer, ',');
                if (!ds_value_tagged_json(exec, value.as.array->items[i], buffer,
                                          depth + 1, ancestors)) return 0;
            }
            return ds_buffer_append(buffer, "]}}");
        case DICESCRIPT_VALUE_DICT:
            if (value.as.dict == NULL) goto unsupported;
            ds_buffer_append(buffer, "{\"t\":7,\"v\":{\"dict\":{");
            for (i = 0; i < value.as.dict->count; ++i) {
                if (i != 0) ds_buffer_char(buffer, ',');
                ds_json_escape(buffer, value.as.dict->entries[i].key);
                ds_buffer_char(buffer, ':');
                if (!ds_value_tagged_json(exec, value.as.dict->entries[i].value,
                                          buffer, depth + 1, ancestors)) return 0;
            }
            return ds_buffer_append(buffer, "}}}");
        case DICESCRIPT_VALUE_FUNCTION:
            if (value.as.function == NULL) goto unsupported;
            ds_buffer_append(buffer, "{\"t\":8,\"v\":{\"expr\":");
            ds_json_escape(buffer, value.as.function->body);
            ds_buffer_append(buffer, ",\"name\":");
            ds_json_escape(buffer, value.as.function->name);
            ds_buffer_append(buffer, ",\"params\":[");
            for (i = 0; i < value.as.function->param_count; ++i) {
                if (i != 0) ds_buffer_char(buffer, ',');
                ds_json_escape(buffer, value.as.function->params[i]);
            }
            return ds_buffer_append(buffer, "]}}");
        case DICESCRIPT_VALUE_NATIVE_FUNCTION:
            if (value.as.callable == NULL) goto unsupported;
            ds_buffer_append(buffer, "{\"t\":9,\"v\":{\"name\":");
            ds_json_escape(buffer, value.as.callable->name);
            return ds_buffer_append(buffer, "}}");
        default: break;
    }
unsupported:
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION,
                 "value type is not serializable");
    exec->flow = DS_FLOW_ERROR; return 0;
}

static int ds_tagged_get(ds_exec *exec, ds_dict *dict, const char *key,
                         ds_value *value, int required) {
    if (dict != NULL && ds_dict_get_own(dict, key, value)) return 1;
    if (!required) return 0;
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION,
                 "invalid tagged DiceScript JSON");
    exec->flow = DS_FLOW_ERROR; return 0;
}

static int ds_decode_tagged(ds_exec *exec, ds_value encoded,
                            ds_value *decoded, uint32_t depth) {
    ds_value type_value, payload, field, item;
    ds_dict *envelope, *payload_dict, *source_dict;
    ds_array *source_array, *array;
    ds_computed *computed; ds_function *function;
    int64_t type_id; size_t i;
    if (depth > exec->context->options.max_call_depth) {
        ds_set_error(exec->context, DICESCRIPT_ERROR_LIMIT,
                     "DiceScript deserialization depth limit reached");
        exec->flow = DS_FLOW_ERROR; return 0;
    }
    if (encoded.type != DICESCRIPT_VALUE_DICT || encoded.as.dict == NULL) goto malformed;
    envelope = encoded.as.dict;
    if (!ds_tagged_get(exec, envelope, "t", &type_value, 1)) return 0;
    if (type_value.type != DICESCRIPT_VALUE_INT) goto malformed;
    type_id = type_value.as.integer;
    if (type_id == DICESCRIPT_VALUE_NULL) { *decoded = ds_null(); return 1; }
    if (!ds_tagged_get(exec, envelope, "v", &payload, 1)) return 0;
    if (type_id == DICESCRIPT_VALUE_INT) {
        if (payload.type != DICESCRIPT_VALUE_INT) goto malformed;
        *decoded = payload; return 1;
    }
    if (type_id == DICESCRIPT_VALUE_FLOAT) {
        if (payload.type == DICESCRIPT_VALUE_FLOAT) *decoded = payload;
        else if (payload.type == DICESCRIPT_VALUE_INT) *decoded = ds_float((double)payload.as.integer);
        else goto malformed;
        return 1;
    }
    if (type_id == DICESCRIPT_VALUE_STRING) {
        if (payload.type != DICESCRIPT_VALUE_STRING) goto malformed;
        *decoded = payload; return 1;
    }
    if (payload.type != DICESCRIPT_VALUE_DICT || payload.as.dict == NULL) goto malformed;
    payload_dict = payload.as.dict;
    if (type_id == DICESCRIPT_VALUE_COMPUTED) {
        if (!ds_tagged_get(exec, payload_dict, "expr", &field, 1) ||
            field.type != DICESCRIPT_VALUE_STRING) goto malformed;
        computed = (ds_computed *)ds_alloc(exec->context, sizeof(*computed));
        if (computed == NULL) return 0;
        computed->expression = ds_new_string_n(exec->context, field.as.string->bytes,
                                                field.as.string->length);
        if (computed->expression == NULL) return 0;
        if (ds_tagged_get(exec, payload_dict, "attrs", &field, 0)) {
            if (field.type != DICESCRIPT_VALUE_DICT || field.as.dict == NULL) goto malformed;
            source_dict = field.as.dict;
            for (i = 0; i < source_dict->count; ++i) {
                if (!ds_decode_tagged(exec, source_dict->entries[i].value,
                                      &item, depth + 1)) return 0;
                if (!ds_dict_set_n(exec->context, &computed->attributes,
                                   source_dict->entries[i].key->bytes,
                                   source_dict->entries[i].key->length, item)) return 0;
            }
        }
        decoded->type = DICESCRIPT_VALUE_COMPUTED; decoded->as.computed = computed; return 1;
    }
    if (type_id == DICESCRIPT_VALUE_ARRAY) {
        if (!ds_tagged_get(exec, payload_dict, "list", &field, 1) ||
            field.type != DICESCRIPT_VALUE_ARRAY || field.as.array == NULL) goto malformed;
        source_array = field.as.array; array = ds_new_array(exec->context);
        if (array == NULL) return 0;
        for (i = 0; i < source_array->count; ++i) {
            if (!ds_decode_tagged(exec, source_array->items[i], &item, depth + 1) ||
                !ds_array_push(exec->context, array, item)) return 0;
        }
        *decoded = ds_array_value(array); return 1;
    }
    if (type_id == DICESCRIPT_VALUE_DICT) {
        if (!ds_tagged_get(exec, payload_dict, "dict", &field, 1) ||
            field.type != DICESCRIPT_VALUE_DICT || field.as.dict == NULL) goto malformed;
        source_dict = field.as.dict; source_array = NULL;
        (void)source_array;
        source_dict = field.as.dict;
        {
            ds_dict *dict = (ds_dict *)ds_alloc(exec->context, sizeof(*dict));
            if (dict == NULL) return 0;
            for (i = 0; i < source_dict->count; ++i) {
                if (!ds_decode_tagged(exec, source_dict->entries[i].value,
                                      &item, depth + 1) ||
                    !ds_dict_set_n(exec->context, dict,
                                   source_dict->entries[i].key->bytes,
                                   source_dict->entries[i].key->length, item)) return 0;
            }
            *decoded = ds_dict_value(dict); return 1;
        }
    }
    if (type_id == DICESCRIPT_VALUE_FUNCTION) {
        ds_value name_value, params_value;
        if (!ds_tagged_get(exec, payload_dict, "expr", &field, 1) ||
            field.type != DICESCRIPT_VALUE_STRING ||
            !ds_tagged_get(exec, payload_dict, "name", &name_value, 1) ||
            name_value.type != DICESCRIPT_VALUE_STRING ||
            !ds_tagged_get(exec, payload_dict, "params", &params_value, 1) ||
            params_value.type != DICESCRIPT_VALUE_ARRAY || params_value.as.array == NULL) goto malformed;
        function = (ds_function *)ds_alloc(exec->context, sizeof(*function));
        if (function == NULL) return 0;
        function->body = ds_new_string_n(exec->context, field.as.string->bytes, field.as.string->length);
        function->name = ds_new_string_n(exec->context, name_value.as.string->bytes, name_value.as.string->length);
        function->param_count = params_value.as.array->count;
        if (function->param_count != 0)
            function->params = (ds_string **)ds_alloc(exec->context,
                function->param_count * sizeof(*function->params));
        if (function->body == NULL || function->name == NULL ||
            (function->param_count != 0 && function->params == NULL)) return 0;
        for (i = 0; i < function->param_count; ++i) {
            item = params_value.as.array->items[i];
            if (item.type != DICESCRIPT_VALUE_STRING) goto malformed;
            function->params[i] = ds_new_string_n(exec->context,
                item.as.string->bytes, item.as.string->length);
            if (function->params[i] == NULL) return 0;
        }
        decoded->type = DICESCRIPT_VALUE_FUNCTION; decoded->as.function = function; return 1;
    }
    if (type_id == DICESCRIPT_VALUE_NATIVE_FUNCTION) {
        if (!ds_tagged_get(exec, payload_dict, "name", &field, 1) ||
            field.type != DICESCRIPT_VALUE_STRING ||
            !ds_is_builtin_name(field.as.string->bytes, field.as.string->length)) goto malformed;
        *decoded = ds_new_callable(exec->context, field.as.string->bytes,
                                   field.as.string->length, 0, ds_null());
        return decoded->type == DICESCRIPT_VALUE_NATIVE_FUNCTION;
    }
malformed:
    ds_set_error(exec->context, DICESCRIPT_ERROR_EVALUATION,
                 "invalid tagged DiceScript JSON");
    exec->flow = DS_FLOW_ERROR; return 0;
}

int dicescript_context_set_serialized(dicescript_context *context,
                                      const char *name,
                                      const char *tagged_json,
                                      dicescript_script_result *result) {
    ds_exec exec; ds_frame frame; ds_value encoded = ds_null(), decoded = ds_null();
    if (context == NULL || name == NULL || *name == '\0' || tagged_json == NULL || result == NULL) return 0;
    context->error_kind = DICESCRIPT_ERROR_NONE; context->error[0] = '\0'; context->steps = 0;
    memset(&exec, 0, sizeof(exec)); memset(&frame, 0, sizeof(frame));
    exec.context = context; exec.frame = &frame; exec.source = tagged_json; exec.source_length = strlen(tagged_json);
    encoded = ds_run_source(&exec, &frame, tagged_json, 1);
    if (exec.flow != DS_FLOW_ERROR && ds_decode_tagged(&exec, encoded, &decoded, 0))
        (void)ds_dict_set(context, &context->globals, name, decoded);
    ds_fill_script_result(context, decoded, result); return result->ok;
}

int dicescript_context_get_serialized(dicescript_context *context,
                                      const char *name,
                                      char *buffer, size_t buffer_size) {
    ds_exec exec; ds_frame frame; ds_value value; ds_buffer output;
    const void **ancestors; int ok;
    if (context == NULL || name == NULL || buffer == NULL || buffer_size == 0) return 0;
    context->error_kind = DICESCRIPT_ERROR_NONE; context->error[0] = '\0'; context->steps = 0;
    memset(&exec, 0, sizeof(exec)); memset(&frame, 0, sizeof(frame));
    exec.context = context; exec.frame = &frame; exec.source = "";
    if (!ds_lookup(&exec, name, strlen(name), 1, &value)) return 0;
    ancestors = (const void **)calloc((size_t)context->options.max_call_depth + 1u,
                                      sizeof(*ancestors));
    if (ancestors == NULL) return 0;
    ds_buffer_init(&output); ok = ds_value_tagged_json(&exec, value, &output, 0, ancestors);
    if (ok && output.length + 1 <= buffer_size) memcpy(buffer, output.data, output.length + 1);
    else ok = 0;
    ds_buffer_free(&output); free(ancestors); return ok;
}

int ds_vm_input_getchar(ds_vm_parser_t *state) {
    if (state == NULL || state->input_position >= state->source_length) return -1;
    return (unsigned char)state->source[state->input_position++];
}

void ds_vm_parser_error(ds_vm_parser_t *state) {
    if (state == NULL) return;
    state->parser_error = 1;
    if (state->error_kind == DICESCRIPT_ERROR_NONE) state->error_kind = DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX;
    if (state->error[0] == '\0') (void)snprintf(state->error, sizeof(state->error), "unsupported DiceScript syntax near byte %zu", state->input_position);
}

ds_ast_t *ds_vm_ast(ds_vm_parser_t *state, ds_ast_kind kind,
                    ds_ast_t *left, ds_ast_t *right, ds_ast_t *third,
                    size_t start, size_t end) {
    ds_ast_t *node;
    if (state == NULL || state->node_count >= state->node_capacity) {
        if (state != NULL) { state->error_kind = DICESCRIPT_ERROR_LIMIT; (void)snprintf(state->error, sizeof(state->error), "DiceScript AST node limit reached"); }
        return NULL;
    }
    node = &state->nodes[state->node_count++]; node->kind = kind; node->left = left; node->right = right; node->third = third; node->source_start = start; node->source_end = end; return node;
}

ds_ast_t *ds_vm_chain(ds_ast_t *head, ds_ast_t *tail) {
    ds_ast_t *node;
    if (head == NULL) return tail;
    node = head; while (node->next != NULL) node = node->next; node->next = tail; return head;
}

ds_ast_t *ds_vm_binary(ds_vm_parser_t *state, ds_ast_kind kind,
                       ds_ast_t *left, ds_ast_t *right,
                       size_t start, size_t end) {
    return ds_vm_ast(state, kind, left, right, NULL, start, end);
}

ds_ast_t *ds_vm_modifier(ds_vm_parser_t *state, int kind, ds_ast_t *value,
                         size_t start, size_t end) {
    ds_ast_t *node = ds_vm_ast(state, DS_AST_DICE_MOD, value, NULL, NULL, start, end);
    if (node != NULL) node->auxiliary = kind; return node;
}
