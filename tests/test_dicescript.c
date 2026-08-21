#include "dicescript/dicescript.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
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
    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("dicescript-c-lib tests passed\n");
    return 0;
}
