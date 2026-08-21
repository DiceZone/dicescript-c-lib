#include "dicescript/dicescript.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

typedef struct random_sequence {
    uint64_t values[32];
    size_t count;
    size_t position;
    uint32_t calls;
} random_sequence;

typedef struct host_values {
    char hp[128];
    unsigned loads;
    unsigned stores;
    int reject_blocked;
} host_values;

typedef struct st_record {
    char operation[16];
    char name[64];
    char value[256];
    char extra[256];
    char operator_text[8];
} st_record;

typedef struct st_capture {
    st_record items[32];
    size_t count;
} st_capture;

typedef struct custom_dice_state {
    char value_json[128];
    char detail[128];
    unsigned evaluations;
} custom_dice_state;

typedef struct native_state {
    unsigned calls;
    char self_json[256];
    char args_json[512];
    char set_attribute[64];
    char set_value[256];
} native_state;

typedef struct detail_callback_state {
    unsigned span_calls;
    unsigned root_calls;
    unsigned make_calls;
    int saw_load;
    int saw_computed;
    char buffer[1024];
} detail_callback_state;

typedef struct host_hook_state {
    unsigned pre_calls;
    unsigned post_calls;
    unsigned store_calls;
    char last_store_name[64];
    char last_store_value[128];
} host_hook_state;

static uint64_t random_max(void *userdata, uint64_t upper_bound) {
    uint32_t *calls = (uint32_t *)userdata;
    if (calls != NULL) ++*calls;
    return upper_bound - 1;
}

static uint64_t random_min(void *userdata, uint64_t upper_bound) {
    uint32_t *calls = (uint32_t *)userdata;
    (void)upper_bound;
    if (calls != NULL) ++*calls;
    return 0;
}

static uint64_t random_from_sequence(void *userdata, uint64_t upper_bound) {
    random_sequence *sequence = (random_sequence *)userdata;
    uint64_t value = 0;
    ++sequence->calls;
    if (sequence->position < sequence->count)
        value = sequence->values[sequence->position++];
    return value % upper_bound;
}

static size_t host_load_json(void *userdata, const char *name,
                             char *buffer, size_t capacity) {
    host_values *host = (host_values *)userdata;
    size_t required;
    ++host->loads;
    if (strcmp(name, "hp") != 0) return 0;
    required = strlen(host->hp) + 1;
    if (buffer != NULL && capacity >= required) memcpy(buffer, host->hp, required);
    return required;
}

static int host_store_json(void *userdata, const char *name,
                           const char *tagged_json) {
    host_values *host = (host_values *)userdata;
    if (strcmp(name, "blocked") == 0 && host->reject_blocked) return -1;
    if (strcmp(name, "hp") != 0) return 0;
    ++host->stores;
    if (strlen(tagged_json) + 1 > sizeof(host->hp)) return -1;
    memcpy(host->hp, tagged_json, strlen(tagged_json) + 1);
    return 1;
}

static int capture_st(void *userdata, const char *operation, const char *name,
                      const char *value_json, const char *extra_json,
                      const char *operator_text, const char *detail) {
    st_capture *capture = (st_capture *)userdata;
    st_record *record;
    (void)detail;
    if (capture->count >= sizeof(capture->items) / sizeof(capture->items[0])) return 1;
    record = &capture->items[capture->count++];
    snprintf(record->operation, sizeof(record->operation), "%s", operation);
    snprintf(record->name, sizeof(record->name), "%s", name);
    snprintf(record->value, sizeof(record->value), "%s", value_json);
    snprintf(record->extra, sizeof(record->extra), "%s", extra_json != NULL ? extra_json : "");
    snprintf(record->operator_text, sizeof(record->operator_text), "%s", operator_text);
    return 0;
}

static int match_e_dice(void *userdata, const char *input,
                        size_t input_length, size_t *consumed_bytes) {
    size_t i = 1;
    (void)userdata;
    if (input_length < 2 || input[0] != 'E' || input[1] < '0' || input[1] > '9') return 0;
    while (i < input_length && input[i] >= '0' && input[i] <= '9') ++i;
    *consumed_bytes = i;
    return 1;
}

static int eval_e_dice(void *userdata, const char *matched_text,
                       size_t matched_length,
                       dicescript_custom_dice_output *output) {
    custom_dice_state *state = (custom_dice_state *)userdata;
    char number[64];
    long value;
    if (matched_length <= 1 || matched_length >= sizeof(number)) return 0;
    memcpy(number, matched_text + 1, matched_length - 1);
    number[matched_length - 1] = '\0';
    value = strtol(number, NULL, 10);
    if (value == 13) {
        output->error = "custom boom";
        return 0;
    }
    ++state->evaluations;
    snprintf(state->value_json, sizeof(state->value_json),
             "{\"t\":0,\"v\":%ld}", value * 2);
    snprintf(state->detail, sizeof(state->detail), "custom:%.*s",
             (int)matched_length, matched_text);
    output->value_json = state->value_json;
    output->detail = state->detail;
    return 1;
}

static int native_add(void *userdata, const char *self_json,
                      const char *args_json,
                      dicescript_native_output *output) {
    native_state *state = (native_state *)userdata;
    ++state->calls;
    snprintf(state->self_json, sizeof(state->self_json), "%s",
             self_json != NULL ? self_json : "");
    snprintf(state->args_json, sizeof(state->args_json), "%s", args_json);
    output->value_json = "{\"t\":0,\"v\":5}";
    return 1;
}

static int native_object_get(void *userdata, const char *attribute,
                             dicescript_native_output *output) {
    (void)userdata;
    if (strcmp(attribute, "hp") == 0) {
        output->value_json = "{\"t\":0,\"v\":41}";
        return 1;
    }
    if (strcmp(attribute, "add") == 0) {
        output->value_json = "{\"t\":9,\"v\":{\"name\":\"hostAdd\"}}";
        return 1;
    }
    return 0;
}

static int native_object_set(void *userdata, const char *attribute,
                             const char *value_json) {
    native_state *state = (native_state *)userdata;
    snprintf(state->set_attribute, sizeof(state->set_attribute), "%s", attribute);
    snprintf(state->set_value, sizeof(state->set_value), "%s", value_json);
    return 0;
}

static int native_object_list(void *userdata,
                              dicescript_native_output *output) {
    (void)userdata;
    output->value_json = "{\"t\":6,\"v\":{\"list\":["
        "{\"t\":2,\"v\":\"hp\"},{\"t\":2,\"v\":\"add\"}]}}";
    return 1;
}

static const char *rewrite_detail_span(
    void *userdata, const char *default_detail,
    const dicescript_detail_span_view *span) {
    detail_callback_state *state = (detail_callback_state *)userdata;
    ++state->span_calls;
    if (strcmp(span->tag, "load") == 0) {
        state->saw_load = 1;
        snprintf(state->buffer, sizeof(state->buffer), "LOAD<%s>",
                 default_detail);
        return state->buffer;
    }
    if (strcmp(span->tag, "load.computed") == 0) {
        state->saw_computed = 1;
        snprintf(state->buffer, sizeof(state->buffer), "COMPUTED<%s>",
                 default_detail);
        return state->buffer;
    }
    return NULL;
}

static const char *rewrite_detail_root(
    void *userdata, const char *default_detail,
    const dicescript_detail_span_view *span) {
    detail_callback_state *state = (detail_callback_state *)userdata;
    (void)span;
    ++state->root_calls;
    return default_detail;
}

static const char *make_detail(
    void *userdata, const dicescript_detail_span_view *spans,
    size_t span_count, const char *source, size_t parsed_length,
    const char *result) {
    detail_callback_state *state = (detail_callback_state *)userdata;
    ++state->make_calls;
    snprintf(state->buffer, sizeof(state->buffer),
             "CUSTOM:%.*s:%s:%s", (int)parsed_length, source, result,
             span_count != 0 ? spans[0].tag : "none");
    return state->buffer;
}

static int host_load_pre(void *userdata, const char *name, int is_raw,
                         dicescript_host_load_pre_output *output) {
    host_hook_state *state = (host_hook_state *)userdata;
    (void)is_raw;
    ++state->pre_calls;
    if (strcmp(name, "alias") == 0) {
        output->new_name = "target";
        return 1;
    }
    if (strcmp(name, "forced") == 0) {
        output->value_json = "{\"t\":0,\"v\":9}";
        return 1;
    }
    return 0;
}

static int host_load_post(void *userdata, const char *name, int is_raw,
                          const char *current_value_json,
                          dicescript_host_load_post_output *output) {
    host_hook_state *state = (host_hook_state *)userdata;
    (void)is_raw;
    ++state->post_calls;
    if (strcmp(name, "missing") == 0) {
        CHECK(strstr(current_value_json, "\"t\":4") != NULL);
        output->value_json = "{\"t\":0,\"v\":123}";
        return 1;
    }
    return 0;
}

static int host_store_pre(void *userdata, const char *name,
                          const char *value_json,
                          dicescript_host_store_pre_output *output) {
    host_hook_state *state = (host_hook_state *)userdata;
    ++state->store_calls;
    snprintf(state->last_store_name, sizeof(state->last_store_name), "%s", name);
    snprintf(state->last_store_value, sizeof(state->last_store_value), "%s",
             value_json);
    if (strcmp(name, "overwrite") == 0) {
        output->value_json = "{\"t\":0,\"v\":3}";
        return 0;
    }
    if (strcmp(name, "handled") == 0) return 1;
    return 0;
}

static dicescript_result eval_with(const char *expression,
                                   dicescript_random_fn random,
                                   void *userdata) {
    dicescript_options options;
    dicescript_result result;
    dicescript_default_options(&options);
    options.random = random;
    options.random_userdata = userdata;
    (void)dicescript_eval(expression, &options, &result);
    return result;
}

static void test_arithmetic(void) {
    dicescript_result result = eval_with("1+2*3", NULL, NULL);
    CHECK(result.ok && result.is_integer && result.integer == 7);

    result = eval_with("3/2", NULL, NULL);
    CHECK(result.ok && result.is_integer && result.integer == 1);

    result = eval_with("3/2.0", NULL, NULL);
    CHECK(result.ok && !result.is_integer && fabs(result.number - 1.5) < 0.000001);

    result = eval_with("1|2&4", NULL, NULL);
    CHECK(result.ok && result.integer == 1);

    result = eval_with("2^3^2", NULL, NULL);
    CHECK(result.ok && result.integer == 64);
}

static void test_common_dice(void) {
    uint32_t calls = 0;
    dicescript_result result = eval_with("10d10d1", random_max, &calls);
    CHECK(result.ok && result.integer == 100);
    CHECK(calls == 110);

    calls = 0;
    result = eval_with("4d6kh3", random_max, &calls);
    CHECK(result.ok && result.integer == 18 && calls == 4);

    {
        random_sequence sequence = {{4, 14}, 2, 0, 0};
        result = eval_with("2d20kl1", random_from_sequence, &sequence);
        CHECK(result.ok && result.integer == 5);
    }

    {
        random_sequence sequence = {{1, 18}, 2, 0, 0};
        result = eval_with("d20优势", random_from_sequence, &sequence);
        CHECK(result.ok && result.integer == 19);
    }

    calls = 0;
    result = eval_with("2d20min10", random_min, &calls);
    CHECK(result.ok && result.integer == 20);
}

static void test_short_circuit_and_validation(void) {
    uint32_t calls = 0;
    dicescript_result result = eval_with("0 ? 1d6 : 7", random_max, &calls);
    CHECK(result.ok && result.integer == 7 && calls == 0);

    memset(&result, 0, sizeof(result));
    CHECK(dicescript_validate("4d6kh3/(1d2-1)", &result));
    CHECK(result.ok && result.dice_rolls == 0);

    calls = 0;
    result = eval_with("4d6kh3/(1d2-1)", random_max, &calls);
    CHECK(result.ok && result.integer == 18 && calls == 5);

    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_validate("1d6 2", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX);

    result = eval_with("1/0", random_max, &calls);
    CHECK(!result.ok && result.error_kind == DICESCRIPT_ERROR_EVALUATION);
}

static void test_special_dice(void) {
    uint32_t calls = 0;
    dicescript_result result = eval_with("f", random_max, &calls);
    CHECK(result.ok && result.integer == 4 && calls == 4);

    calls = 0;
    result = eval_with("3a0m10k8", random_max, &calls);
    CHECK(result.ok && result.integer == 3 && calls == 3);

    calls = 0;
    result = eval_with("3c8", random_min, &calls);
    CHECK(result.ok && result.integer == 1 && calls == 3);

    {
        random_sequence sequence = {{66, 9}, 2, 0, 0};
        result = eval_with("b1", random_from_sequence, &sequence);
        CHECK(result.ok && result.integer == 7 && sequence.calls == 2);
    }
}

static dicescript_context *new_context(void) {
    dicescript_runtime_options options;
    dicescript_default_runtime_options(&options);
    options.dice.random = random_max;
    return dicescript_context_create(&options);
}

static dicescript_script_result run_script(dicescript_context *context, const char *script) {
    dicescript_script_result result;
    memset(&result, 0, sizeof(result));
    (void)dicescript_context_run(context, script, &result);
    if (!result.ok) fprintf(stderr, "SCRIPT ERROR [%s]: %s\n", script, result.error);
    return result;
}

static void test_full_vm_values_and_variables(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;
    char json[256];
    CHECK(context != NULL);

    result = run_script(context, "a=1");
    CHECK(result.ok && result.integer == 1);
    result = run_script(context, "b=2.5");
    CHECK(result.ok && result.type == DICESCRIPT_VALUE_FLOAT);
    result = run_script(context, "text='Hello, '+'world!'");
    CHECK(result.ok && strcmp(result.text, "Hello, world!") == 0);
    result = run_script(context, "[a,b,text]");
    CHECK(result.ok && result.type == DICESCRIPT_VALUE_ARRAY &&
          strcmp(result.text, "[1, 2.5, 'Hello, world!']") == 0);

    result = run_script(context, "a=a+4; a");
    CHECK(result.ok && result.type == DICESCRIPT_VALUE_INT && result.integer == 5);
    CHECK(dicescript_context_set_string(context, "外部", "值"));
    result = run_script(context, "外部");
    CHECK(result.ok && strcmp(result.text, "值") == 0);

    result = run_script(context, "obj={'x':1,'name':'dice'}; obj.x=3; obj['extra']=[1,2,3]; obj.extra[1]");
    CHECK(result.ok && result.integer == 2);
    CHECK(dicescript_context_get_json(context, "obj", json, sizeof(json)));
    CHECK(strstr(json, "\"x\":3") != NULL && strstr(json, "\"extra\":[1,2,3]") != NULL);

    dicescript_context_destroy(context);
}

static void test_full_vm_control_flow_and_functions(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;

    result = run_script(context, "i=0; total=0; while i<5 { i=i+1; if i==3 { continue }; total=total+i }; total");
    CHECK(result.ok && result.integer == 12);

    result = run_script(context,
        "func fib(n) { if n<2 { return n }; return fib(n-1)+fib(n-2) }; fib(10)");
    CHECK(result.ok && result.integer == 55);

    result = run_script(context, "func local(n) { a=10; return a+n }; a=2; [local(3),a]");
    CHECK(result.ok && strcmp(result.text, "[13, 2]") == 0);

    dicescript_context_destroy(context);
}

static void test_full_vm_collections_and_methods(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;

    result = run_script(context, "a=[1,2,3]; a.push(4); [a.len(),a.sum(),a[-1],a[1:3]]");
    CHECK(result.ok && strcmp(result.text, "[4, 10, 4, [2, 3]]") == 0);

    result = run_script(context, "d={'a':1}; [d.has('a'),d.has('b'),d.get('b',9),d.keys()]");
    CHECK(result.ok && strcmp(result.text, "[1, 0, 9, ['a']]") == 0);

    result = run_script(context, "[[1,2],[3,4]][1][0]");
    CHECK(result.ok && result.integer == 3);

    dicescript_context_destroy(context);
}

static void test_full_vm_templates_computed_and_dice(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;

    result = run_script(context, "name='Alice'; hp=7; `玩家{name}的生命值为{hp}`");
    CHECK(result.ok && strcmp(result.text, "玩家Alice的生命值为7") == 0);

    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_format(context, "{name}: {hp+3}", &result));
    CHECK(strcmp(result.text, "Alice: 10") == 0);

    result = run_script(context, "&attack=1+2; [typeId(&attack),attack,repr(loadRaw('attack'))]");
    CHECK(result.ok && strcmp(result.text, "[5, 3, '&(1+2)']") == 0);

    result = run_script(context, "count=2; faces=6; count d faces");
    CHECK(result.ok && result.integer == 12);

    dicescript_context_destroy(context);
}

static void test_full_vm_expression_boundary(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;
    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run_complete(context, "1+2 trailing reason", &result));

    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_eval(context, "a=1; a", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX);
    CHECK(dicescript_context_eval(context, "1+2*3", &result));
    CHECK(result.integer == 7);
    dicescript_context_destroy(context);
}

static void expect_text(dicescript_context *context, const char *script, const char *expected) {
    dicescript_script_result result = run_script(context, script);
    if (!result.ok || strcmp(result.text, expected) != 0) {
        fprintf(stderr, "MISMATCH [%s]: got <%s>, expected <%s>\n", script, result.text, expected);
        ++failures;
    }
}

/* Directly mirrored from the pinned upstream rollvm/builtin/type-method tests. */
static void test_upstream_core_corpus(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;

    expect_text(context, "undefined_name", "null");
    expect_text(context, "if 0 { 1 } else if 2 { 3 } else { 4 }", "null");
    expect_text(context, "a=0; while a<5 { a=a+1; continue; a=a+10 }; a", "5");
    expect_text(context, "a=1; while a<5 { break; a=a+1 }; a", "1");
    expect_text(context, "1==1 ? 2 : 3", "2");
    expect_text(context, "1!=1 ? 2 : 3", "3");
    expect_text(context, "null ?? 9", "9");
    expect_text(context, "0 || 5", "5");
    expect_text(context, "1 && 2", "2");
    expect_text(context, "[1,2,3]kh", "3");
    expect_text(context, "[1,2,3]kl2", "3");
    expect_text(context, "[1..4]", "[1, 2, 3, 4]");
    expect_text(context, "[4..1]", "[4, 3, 2, 1]");
    expect_text(context, "'12345'[2:4]", "34");
    expect_text(context, "'甲乙丙'.len()", "3");
    expect_text(context, "'甲乙丙'[1]", "乙");
    expect_text(context, "a=[1,2,3]; a[0]=9; a", "[9, 2, 3]");
    expect_text(context, "a={'a':1}; a.a", "1");
    expect_text(context, "c='c'; a={c:1,'b':3}; a.c", "1");
    expect_text(context, "ceil(1.2)", "2");
    expect_text(context, "floor(1.6)", "1");
    expect_text(context, "round(1.6)", "2");
    expect_text(context, "abs(-3.5)", "3.5");
    expect_text(context, "int('12')", "12");
    expect_text(context, "float('1.5')", "1.5");
    expect_text(context, "str([1,2])", "[1, 2]");
    expect_text(context, "repr('hello')", "'hello'");
    expect_text(context, "store('stored',123); stored", "123");
    expect_text(context, "func add(a,b) { return this.a+this.b }; add(2,3)", "5");
    expect_text(context, "&cv=1+this.x; &cv.x=4; cv", "5");
    expect_text(context, "a=&(1+2); typeId(loadRaw('a'))", "5");
    expect_text(context, "a=&(1+2); a.compute()", "3");
    expect_text(context, "name='Alice'; `Hi {name}!`", "Hi Alice!");
    expect_text(context, "`{% a=2; a+3 %}`", "5");
    expect_text(context, "`1 {% if 1 {'test'} %} 2`", "1  2");
    expect_text(context, "`1 {% x=0; while x<3 { x=x+1 } %} 2`", "1  2");
    expect_text(context, "x=0; while x<3 { x=x+1 }", "null");
    expect_text(context, "1 && 2 && 3", "3");
    expect_text(context, "toInt(-1.9)", "-1");
    expect_text(context, "[1.2,2,3]kh", "3");
    expect_text(context, "[4.1,3.1,1]kl", "1");
    expect_text(context, "2*[1,2]", "[1, 2, 1, 2]");
    expect_text(context, "{'a':1,'b':[2]}=={'b':[2],'a':1}", "1");
    expect_text(context, "&(1+2)==&(1+2)", "1");
    expect_text(context, "dir([1])", "['kh', 'kl', 'sum', 'len', 'shuffle', 'rand', 'randSize', 'pop', 'shift', 'push']");
    expect_text(context, "dir({'own':1})", "['keys', 'values', 'items', 'len', 'has', 'get', 'getRaw']");

    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "while 1 {}", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_LIMIT);
    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "ceil('')", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_EVALUATION);
    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "[1,2,3][-4]", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_EVALUATION);
    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "'abc'[3]", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_EVALUATION);
    memset(&result, 0, sizeof(result));
    if (dicescript_context_run(context, "func f(a,b) { a+b }; f(1)", &result))
        fprintf(stderr, "MISSING-ARG RESULT: type=%d text=%s error=%s\n", (int)result.type, result.text, result.error);
    CHECK(!result.ok);
    CHECK(result.error_kind == DICESCRIPT_ERROR_EVALUATION);
    memset(&result, 0, sizeof(result));
    if (dicescript_context_run(context, "func f(a) { a }; f(1,2)", &result))
        fprintf(stderr, "EXTRA-ARG RESULT: type=%d text=%s error=%s\n", (int)result.type, result.text, result.error);
    CHECK(!result.ok);
    CHECK(result.error_kind == DICESCRIPT_ERROR_EVALUATION);

    dicescript_context_destroy(context);
}

static void test_upstream_assignment_ternary_and_flags(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;

    expect_text(context, "a=1; a==1?'A', a==2?'B'", "A");
    expect_text(context, "a=2; a==1?'A', a==2?'B', a==3?'C'", "B");
    expect_text(context, "a=4; a==1?'A', a==2?'B'", "");

    expect_text(context, "a=[1,2,3,4]; a[:]=[1,2,3]; a", "[1, 2, 3]");
    expect_text(context, "a=[1,2,3]; a[:1]=[4,5]; a", "[4, 5, 2, 3]");
    expect_text(context, "a=[1,2,3]; a[2:]=[4,5]; a", "[1, 2, 4, 5]");
    expect_text(context, "a=[1,2,3]; a[:]=a; a", "[1, 2, 3]");

    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "a=[1,2,3]; a[0:2:1]", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX);
    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "a=[1,2,3]; a[0:2:1]=[9]", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX);

    result = run_script(context, "// #EnableDice coc true\nb");
    CHECK(result.ok && result.type == DICESCRIPT_VALUE_INT);
    expect_text(context, "// #EnableDice coc false\nb", "null");
    expect_text(context, "// #EnableDice fate true\nf", "4");
    expect_text(context, "// #EnableDice fate false\nf", "null");
    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "// #EnableDice unknown true", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_EVALUATION);

    dicescript_context_destroy(context);
}

static void test_upstream_tagged_serialization(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;
    char json[2048];

    CHECK(dicescript_context_set_int(context, "n", 123));
    CHECK(dicescript_context_get_serialized(context, "n", json, sizeof(json)));
    CHECK(strcmp(json, "{\"t\":0,\"v\":123}") == 0);

    result = run_script(context, "a=[1,2.0,'test']; d={'v':a}; d");
    CHECK(result.ok);
    CHECK(dicescript_context_get_serialized(context, "a", json, sizeof(json)));
    CHECK(strcmp(json, "{\"t\":6,\"v\":{\"list\":[{\"t\":0,\"v\":1},{\"t\":1,\"v\":2},{\"t\":2,\"v\":\"test\"}]}}") == 0);
    CHECK(dicescript_context_get_serialized(context, "d", json, sizeof(json)));
    CHECK(strstr(json, "\"t\":7") != NULL && strstr(json, "\"dict\":{\"v\":") != NULL);

    result = run_script(context, "&cv=this.x+1; &cv.x=4; func five(x) { return 5 }");
    CHECK(result.ok);
    CHECK(dicescript_context_get_serialized(context, "cv", json, sizeof(json)));
    if (!(strstr(json, "{\"t\":5") == json && strstr(json, "\"expr\":\"this.x+1\"") != NULL &&
          strstr(json, "\"attrs\":{\"x\":{\"t\":0,\"v\":4}}") != NULL))
        fprintf(stderr, "COMPUTED JSON: %s\n", json);
    CHECK(strstr(json, "{\"t\":5") == json && strstr(json, "\"expr\":\"this.x+1\"") != NULL &&
          strstr(json, "\"attrs\":{\"x\":{\"t\":0,\"v\":4}}") != NULL);
    CHECK(dicescript_context_get_serialized(context, "five", json, sizeof(json)));
    CHECK(strstr(json, "{\"t\":8") == json && strstr(json, "\"name\":\"five\"") != NULL &&
          strstr(json, "\"params\":[\"x\"]") != NULL);
    CHECK(dicescript_context_get_serialized(context, "ceil", json, sizeof(json)));
    CHECK(strcmp(json, "{\"t\":9,\"v\":{\"name\":\"ceil\"}}") == 0);

    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_set_serialized(context, "loaded",
        "{\"t\":6,\"v\":{\"list\":[{\"t\":0,\"v\":7},{\"t\":2,\"v\":\"\\u4e2d\\u6587\"}]}}",
        &result));
    CHECK(result.ok);
    expect_text(context, "loaded", "[7, '中文']");

    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_set_serialized(context, "loadedFn",
        "{\"t\":8,\"v\":{\"expr\":\"return x+1\",\"name\":\"loadedFn\",\"params\":[\"x\"]}}",
        &result));
    expect_text(context, "loadedFn(4)", "5");

    result = run_script(context, "cycle=[]; cycle.push(cycle)");
    CHECK(result.ok);
    CHECK(!dicescript_context_get_serialized(context, "cycle", json, sizeof(json)));

    dicescript_context_destroy(context);
}

static void test_upstream_host_value_callbacks(void) {
    dicescript_context *context = new_context();
    dicescript_host_callbacks callbacks;
    dicescript_script_result result;
    host_values host;
    memset(&host, 0, sizeof(host));
    strcpy(host.hp, "{\"t\":0,\"v\":10}");
    host.reject_blocked = 1;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.load = host_load_json;
    callbacks.store = host_store_json;
    callbacks.userdata = &host;
    dicescript_context_set_host_callbacks(context, &callbacks);

    result = run_script(context, "hp");
    CHECK(result.ok && result.integer == 10 && host.loads >= 2);
    result = run_script(context, "hp=hp+2; hp");
    CHECK(result.ok && result.integer == 12 && host.stores == 1);
    CHECK(strcmp(host.hp, "{\"t\":0,\"v\":12}") == 0);
    result = run_script(context, "ceil(1.2)");
    CHECK(result.ok && result.integer == 2);

    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "blocked=1", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_EVALUATION);

    dicescript_context_set_host_callbacks(context, NULL);
    expect_text(context, "local=7; local", "7");
    dicescript_context_destroy(context);
}

static void test_upstream_st_syntax(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;
    st_capture capture;
    memset(&capture, 0, sizeof(capture));
    dicescript_context_set_st_callback(context, capture_st, &capture);

    result = run_script(context, "^stA:1");
    CHECK(result.ok && capture.count == 1);
    CHECK(strcmp(capture.items[0].operation, "set") == 0);
    CHECK(strcmp(capture.items[0].name, "A") == 0);
    CHECK(strcmp(capture.items[0].value, "{\"t\":0,\"v\":1}") == 0);

    memset(&capture, 0, sizeof(capture));
    result = run_script(context, "^st力量60敏捷70");
    CHECK(result.ok && capture.count == 2);
    CHECK(strcmp(capture.items[0].name, "力量") == 0 && strstr(capture.items[0].value, "\"v\":60") != NULL);
    CHECK(strcmp(capture.items[1].name, "敏捷") == 0 && strstr(capture.items[1].value, "\"v\":70") != NULL);

    memset(&capture, 0, sizeof(capture));
    result = run_script(context, "^st智力:80 知识=90");
    CHECK(result.ok && capture.count == 2);
    CHECK(strcmp(capture.items[0].name, "智力") == 0 && strcmp(capture.items[1].name, "知识") == 0);

    memset(&capture, 0, sizeof(capture));
    result = run_script(context, "^st&射击=1d6 &射击=(1d6)");
    CHECK(result.ok && capture.count == 2);
    CHECK(strstr(capture.items[0].value, "\"t\":5") != NULL && strstr(capture.items[0].value, "\"expr\":\"1d6\"") != NULL);
    CHECK(strstr(capture.items[1].value, "\"expr\":\"(1d6)\"") != NULL);

    memset(&capture, 0, sizeof(capture));
    result = run_script(context, "^stA*:3 A*2.1:3");
    CHECK(result.ok && capture.count == 2);
    CHECK(strcmp(capture.items[0].operation, "set.x0") == 0);
    CHECK(strcmp(capture.items[1].operation, "set.x1") == 0 && strstr(capture.items[1].extra, "\"v\":2.1") != NULL);

    memset(&capture, 0, sizeof(capture));
    result = run_script(context, "^st力量+3d1 敏捷+=3 力量-3d1-1 hp-=1-1");
    CHECK(result.ok && capture.count == 4);
    CHECK(strcmp(capture.items[0].operation, "mod") == 0 && strcmp(capture.items[0].operator_text, "+") == 0);
    CHECK(strcmp(capture.items[2].operator_text, "-") == 0);
    CHECK(strcmp(capture.items[3].operator_text, "-=") == 0);

    memset(&capture, 0, sizeof(capture));
    result = run_script(context, "^st'力量123'+=3");
    CHECK(result.ok && capture.count == 1 && strcmp(capture.items[0].name, "力量123") == 0);

    dicescript_context_destroy(context);
}

static void test_upstream_default_dice(void) {
    dicescript_runtime_options options;
    dicescript_context *context;
    dicescript_script_result result;
    dicescript_default_runtime_options(&options);
    options.dice.random = random_max;
    context = dicescript_context_create(&options);
    expect_text(context, "d", "100");
    expect_text(context, "3d", "300");
    expect_text(context, "d优势", "100");
    dicescript_context_destroy(context);

    dicescript_default_runtime_options(&options);
    options.dice.random = random_max;
    strcpy(options.default_dice_side_expression, "12d1-11");
    context = dicescript_context_create(&options);
    expect_text(context, "d", "1");
    dicescript_context_destroy(context);

    dicescript_default_runtime_options(&options);
    options.enable_default_dice = 0;
    context = dicescript_context_create(&options);
    result = run_script(context, "d");
    CHECK(result.ok && result.type == DICESCRIPT_VALUE_NULL);
    dicescript_context_destroy(context);
}

static void test_upstream_custom_dice(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;
    custom_dice_state state;
    memset(&state, 0, sizeof(state));
    CHECK(dicescript_context_register_custom_dice(context, "E dice",
                                                  match_e_dice, eval_e_dice,
                                                  &state));
    result = run_script(context, "E5");
    CHECK(result.ok && result.integer == 10 && state.evaluations == 1);
    CHECK(strstr(result.detail, "custom:E5") != NULL);
    result = run_script(context, "E5+1");
    CHECK(result.ok && result.integer == 11 && state.evaluations == 2);
    result = run_script(context, "Efoo");
    CHECK(result.ok && result.type == DICESCRIPT_VALUE_NULL);

    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "E13", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_EVALUATION &&
          strstr(result.error, "custom boom") != NULL);

    dicescript_context_clear_custom_dice(context);
    result = run_script(context, "E5");
    CHECK(result.ok && result.type == DICESCRIPT_VALUE_NULL);
    dicescript_context_destroy(context);
}

static void test_upstream_native_function_and_object(void) {
    dicescript_context *context = new_context();
    dicescript_native_object_callbacks callbacks;
    dicescript_script_result result;
    native_state state;
    char serialized[512];
    memset(&state, 0, sizeof(state));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.get = native_object_get;
    callbacks.set = native_object_set;
    callbacks.list = native_object_list;
    callbacks.userdata = &state;

    CHECK(dicescript_context_register_native_function(context, "hostAdd",
                                                       native_add, &state));
    result = run_script(context, "hostAdd(2,3)");
    CHECK(result.ok && result.type == DICESCRIPT_VALUE_INT && result.integer == 5);
    CHECK(state.calls == 1 && state.self_json[0] == '\0');
    CHECK(strcmp(state.args_json,
        "{\"t\":6,\"v\":{\"list\":[{\"t\":0,\"v\":2},{\"t\":0,\"v\":3}]}}") == 0);

    CHECK(dicescript_context_set_native_object(context, "obj", "demo", &callbacks));
    result = run_script(context, "obj.hp");
    CHECK(result.ok && result.integer == 41);
    result = run_script(context, "obj.add(1,2)");
    CHECK(result.ok && result.integer == 5 && state.calls == 2);
    CHECK(strcmp(state.self_json, "{\"t\":10,\"v\":{\"name\":\"demo\"}}") == 0);

    result = run_script(context, "obj.hp=9");
    CHECK(result.ok && result.integer == 9);
    CHECK(strcmp(state.set_attribute, "hp") == 0);
    CHECK(strcmp(state.set_value, "{\"t\":0,\"v\":9}") == 0);
    result = run_script(context, "dir(obj)");
    CHECK(result.ok && strcmp(result.text, "['hp', 'add']") == 0);

    memset(serialized, 0, sizeof(serialized));
    CHECK(dicescript_context_get_serialized(context, "obj", serialized,
                                             sizeof(serialized)));
    CHECK(strcmp(serialized, "{\"t\":10,\"v\":{\"name\":\"demo\"}}") == 0);
    CHECK(dicescript_context_set_serialized(context, "shell",
        "{\"t\":10,\"v\":{\"name\":\"detached\"}}", &result));
    result = run_script(context, "shell");
    CHECK(result.ok && strcmp(result.text, "nobject detached") == 0);
    result = run_script(context, "shell.unknown");
    CHECK(result.ok && result.type == DICESCRIPT_VALUE_NULL);

    dicescript_context_clear_native_functions(context);
    memset(&result, 0, sizeof(result));
    (void)dicescript_context_run(context, "hostAdd(1,2)", &result);
    CHECK(!result.ok);
    dicescript_context_destroy(context);
}

static void test_upstream_prefix_and_rest_input(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;

    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_run_prefix(context, "1+2 trailing reason", &result));
    CHECK(result.type == DICESCRIPT_VALUE_INT && result.integer == 3);
    CHECK(result.consumed_bytes == 3);
    CHECK(strcmp(result.matched, "1+2") == 0);
    CHECK(strcmp(result.rest, " trailing reason") == 0);

    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_run_prefix(context, "1+2   ", &result));
    CHECK(result.consumed_bytes == 3 && strcmp(result.rest, "   ") == 0);
    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_run(context, "1+2 because test", &result));
    CHECK(result.integer == 3 && strcmp(result.matched, "1+2") == 0 &&
          strcmp(result.rest, " because test") == 0);

    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_run_prefix(context, "1 2", &result));
    CHECK(result.integer == 1 && strcmp(result.rest, " 2") == 0);
    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_run_prefix(context, "i=0 if 1 {i=3}", &result));
    CHECK(strcmp(result.rest, " if 1 {i=3}") == 0);
    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_run_prefix(context, "i=0; if 1 {i=3}; i", &result));
    CHECK(result.integer == 3 && result.rest[0] == '\0');

    expect_text(context, "a=[1,2]; a [1]", "2");
    expect_text(context, "obj={}; obj  .  xx=1; obj.xx", "1");
    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "--1", &result));
    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_run_prefix(context, "3d1 k2", &result));
    CHECK(result.integer == 3 && strcmp(result.rest, " k2") == 0);
    dicescript_context_destroy(context);
}

static void test_upstream_runtime_flags(void) {
    dicescript_runtime_options options;
    dicescript_context *context;
    dicescript_script_result result;
    uint32_t calls = 0;

    dicescript_default_runtime_options(&options);
    options.ignore_divide_by_zero = 1;
    context = dicescript_context_create(&options);
    expect_text(context, "7/0", "7");
    dicescript_context_destroy(context);

    dicescript_default_runtime_options(&options);
    options.disable_bitwise_operations = 1;
    context = dicescript_context_create(&options);
    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_eval(context, "1|2", &result));
    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_run_prefix(context, "1|2 reason", &result));
    CHECK(result.integer == 1 && strcmp(result.rest, "|2 reason") == 0);
    dicescript_context_destroy(context);

    dicescript_default_runtime_options(&options);
    options.dice.random = random_min;
    options.dice.random_userdata = &calls;
    options.dice.dice_roll_mode = 1;
    context = dicescript_context_create(&options);
    expect_text(context, "2d6", "12");
    CHECK(calls == 0);
    dicescript_context_destroy(context);

    dicescript_default_runtime_options(&options);
    options.dice.random = random_max;
    options.dice.random_userdata = &calls;
    options.dice.dice_roll_mode = -1;
    context = dicescript_context_create(&options);
    expect_text(context, "2d6", "2");
    CHECK(calls == 0);
    dicescript_context_destroy(context);
}

static void test_upstream_detail_spans(void) {
    dicescript_runtime_options options;
    dicescript_context *context;
    dicescript_script_result result;

    dicescript_default_runtime_options(&options);
    options.dice.dice_roll_mode = 1;
    context = dicescript_context_create(&options);
    CHECK(context != NULL);

    result = run_script(context, "d1");
    CHECK(result.ok && strcmp(result.detail, "") == 0);
    result = run_script(context, "2d1");
    CHECK(result.ok && strcmp(result.detail, "2[2d1=1+1]") == 0);
    result = run_script(context, "(2d1)d1");
    CHECK(result.ok && strcmp(result.detail,
                              "2[(2d1)d1=1+1,2d1=2]") == 0);
    result = run_script(context, "d + 2d");
    CHECK(result.ok && strcmp(result.detail,
                              "100[D100] + 200[2D100=100+100]") == 0);
    result = run_script(context, "d + 1");
    CHECK(result.ok && strcmp(result.detail, "100[D100] + 1") == 0);
    result = run_script(context, "d");
    CHECK(result.ok && strcmp(result.detail, "100[D100]") == 0);
    result = run_script(context, "2dk1");
    CHECK(result.ok && strcmp(result.detail,
                              "100[2D100kh1={100 | 100}]") == 0);
    result = run_script(context, "a=1;a");
    CHECK(result.ok && strcmp(result.detail, "a=1;1") == 0);

    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_set_serialized(context, "a",
        "{\"t\":5,\"v\":{\"expr\":\"4d1\",\"attrs\":{}}}", &result));
    result = run_script(context, "a");
    CHECK(result.ok && strcmp(result.detail,
        "4[a=4[4d1=1+1+1+1]=4]") == 0);

    dicescript_context_destroy(context);
}

static void test_upstream_detail_callbacks(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;
    dicescript_detail_callbacks callbacks;
    detail_callback_state state;
    memset(&callbacks, 0, sizeof(callbacks));
    memset(&state, 0, sizeof(state));
    callbacks.span_rewrite = rewrite_detail_span;
    callbacks.root_rewrite = rewrite_detail_root;
    callbacks.userdata = &state;
    dicescript_context_set_detail_callbacks(context, &callbacks);

    CHECK(dicescript_context_set_int(context, "x", 5));
    result = run_script(context, "x");
    CHECK(result.ok && strcmp(result.detail, "5LOAD<>") == 0);
    CHECK(state.saw_load && state.span_calls == 1 && state.root_calls == 1);

    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_set_serialized(context, "a",
        "{\"t\":5,\"v\":{\"expr\":\"4d1\",\"attrs\":{}}}", &result));
    result = run_script(context, "a");
    CHECK(result.ok && strcmp(result.detail,
        "4COMPUTED<[a=4[4d1=1+1+1+1]=4]>") == 0);
    CHECK(state.saw_computed);

    memset(&state, 0, sizeof(state));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.make = make_detail;
    callbacks.span_rewrite = rewrite_detail_span;
    callbacks.userdata = &state;
    dicescript_context_set_detail_callbacks(context, &callbacks);
    result = run_script(context, "2d1");
    CHECK(result.ok && strcmp(result.detail,
                              "CUSTOM:2d1:2:dice") == 0);
    CHECK(state.make_calls == 1 && state.span_calls == 0);

    dicescript_context_set_detail_callbacks(context, NULL);
    result = run_script(context, "2d1");
    CHECK(result.ok && strcmp(result.detail, "2[2d1=1+1]") == 0);
    dicescript_context_destroy(context);
}

static void test_upstream_host_intercept_hooks(void) {
    dicescript_context *context = new_context();
    dicescript_host_callbacks callbacks;
    dicescript_script_result result;
    host_hook_state state;
    memset(&callbacks, 0, sizeof(callbacks));
    memset(&state, 0, sizeof(state));
    callbacks.load_pre = host_load_pre;
    callbacks.load_post = host_load_post;
    callbacks.store_pre = host_store_pre;
    callbacks.userdata = &state;
    dicescript_context_set_host_callbacks(context, &callbacks);
    CHECK(dicescript_context_set_int(context, "target", 7));

    result = run_script(context, "alias");
    CHECK(result.ok && result.integer == 7);
    result = run_script(context, "forced");
    CHECK(result.ok && result.integer == 9);
    result = run_script(context, "missing");
    CHECK(result.ok && result.integer == 123);
    result = run_script(context, "toStr(1)");
    CHECK(result.ok && strcmp(result.text, "1") == 0);

    result = run_script(context, "overwrite=1");
    CHECK(result.ok && strstr(state.last_store_value, "\"v\":1") != NULL);
    result = run_script(context, "&overwrite");
    CHECK(result.ok && result.integer == 3);
    result = run_script(context, "handled=8");
    CHECK(result.ok);

    dicescript_context_set_host_callbacks(context, NULL);
    result = run_script(context, "handled");
    CHECK(result.ok && result.type == DICESCRIPT_VALUE_NULL);
    CHECK(state.pre_calls != 0 && state.post_calls != 0 &&
          state.store_calls >= 2);
    dicescript_context_destroy(context);
}

static void test_upstream_extended_corpus(void) {
    dicescript_context *context = new_context();
    dicescript_script_result result;

    expect_text(context, "null ?? 5", "5");
    expect_text(context, "10 ?? 5", "10");
    expect_text(context, "2 ** 3", "8");
    expect_text(context, "+3.14", "3.14");
    expect_text(context, "'\\'test\\''", "'test'");
    expect_text(context, "\"\\\"test\\\"\"", "\"test\"");
    expect_text(context, "`test \\{ test \\}`", "test { test }");
    expect_text(context, "`12\\f3`", "12\f3");

    expect_text(context, "func empty() { return }; empty()", "null");
    expect_text(context, "if 1 {} 2", "2");
    expect_text(context, "1; if 1 {}; 2", "2");
    expect_text(context, "a={}; a[1]=10; toStr(a[1])", "10");
    expect_text(context, "a=[0,0,0]; i=0; while i<3 {a[i]=i+1; i=i+1} a",
                "[1, 2, 3]");
    expect_text(context, "func fib(n) { if n<2 { return n }; return fib(n-1)+fib(n-2) }; fib(8)",
                "21");
    expect_text(context, "a=[1,2]; 5||a.push(3); a", "[1, 2]");
    expect_text(context, "a=[1,2]; 5&&a.push(3); a", "[1, 2, 3]");
    expect_text(context, "'中文测试'[-3:3]", "文测");
    expect_text(context, "`{1} {2} {% 3;4;5;6 %}`", "1 2 6");
    expect_text(context, "$t（测试）=1; $t（测试）", "1");

    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "", &result));
    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_run(context, "while", &result));
    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_run(context, "i=0 if 1 {i=3}", &result));
    CHECK(strcmp(result.rest, " if 1 {i=3}") == 0);

    dicescript_context_destroy(context);

    {
        dicescript_runtime_options options;
        dicescript_default_runtime_options(&options);
        options.enable_dice_fate = 1;
        context = dicescript_context_create(&options);
        result = run_script(context, "f1");
        CHECK(result.ok && result.type == DICESCRIPT_VALUE_NULL && result.rest[0] == '\0');
        dicescript_context_destroy(context);
    }
}
static void test_full_vm_validation_and_samples(void) {
    dicescript_runtime_options options;
    dicescript_context *context;
    dicescript_script_result result;
    uint32_t calls = 0;
    dicescript_default_runtime_options(&options);
    options.dice.random = random_max;
    options.dice.random_userdata = &calls;
    context = dicescript_context_create(&options);
    CHECK(context != NULL);

    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_validate_expression(
        context, "[1, 2, 3].sum() + 2d6", &result));
    CHECK(result.ok && calls == 0 && result.dice_rolls == 0 &&
          result.sample_count == 0);
    memset(&result, 0, sizeof(result));
    CHECK(!dicescript_context_validate_expression(
        context, "a=1; a+1", &result));
    CHECK(result.error_kind == DICESCRIPT_ERROR_UNSUPPORTED_SYNTAX && calls == 0);
    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_validate_script(
        context, "a=1; func twice(x) { return x*2 }; twice(a)", &result));
    CHECK(result.ok && calls == 0);
    memset(&result, 0, sizeof(result));
    CHECK(dicescript_context_validate_expression_prefix(
        context, "[1, 2, 3].sum() + 2d6 attack", &result));
    CHECK(result.ok && calls == 0);
    CHECK(strcmp(result.matched, "[1, 2, 3].sum() + 2d6") == 0);
    CHECK(strcmp(result.rest, " attack") == 0);
    CHECK(result.consumed_bytes == strlen("[1, 2, 3].sum() + 2d6"));


    result = run_script(context, "2d6 + d4");
    CHECK(result.ok && result.integer == 16 && calls == 3);
    CHECK(result.dice_rolls == 3 && result.sample_count == 3);
    CHECK(result.samples[0] == 6 && result.samples[1] == 6 &&
          result.samples[2] == 4);
    dicescript_context_destroy(context);
}


int main(void) {
    test_arithmetic();
    test_common_dice();
    test_short_circuit_and_validation();
    test_special_dice();
    test_full_vm_values_and_variables();
    test_full_vm_control_flow_and_functions();
    test_full_vm_collections_and_methods();
    test_full_vm_templates_computed_and_dice();
    test_full_vm_expression_boundary();
    test_upstream_core_corpus();
    test_upstream_assignment_ternary_and_flags();
    test_upstream_tagged_serialization();
    test_upstream_host_value_callbacks();
    test_upstream_st_syntax();
    test_upstream_default_dice();
    test_upstream_custom_dice();
    test_upstream_native_function_and_object();
    test_upstream_prefix_and_rest_input();
    test_upstream_runtime_flags();
    test_upstream_detail_spans();
    test_upstream_detail_callbacks();
    test_full_vm_validation_and_samples();
    test_upstream_host_intercept_hooks();
    test_upstream_extended_corpus();
    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("dicescript-c-lib tests passed\n");
    return 0;
}
