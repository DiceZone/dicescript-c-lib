#include "dicescript/dicescript.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    dicescript_result result;
    dicescript_options options;
    if (argc > 2 && strcmp(argv[1], "--vm") == 0) {
        dicescript_context *context;
        dicescript_script_result script_result;
        dicescript_runtime_options runtime_options;
        dicescript_default_runtime_options(&runtime_options);
        context = dicescript_context_create(&runtime_options);
        if (context == NULL || !dicescript_context_run(context, argv[2], &script_result)) {
            fprintf(stderr, "error: %s\n", context != NULL ? script_result.error : "out of memory");
            dicescript_context_destroy(context);
            return 1;
        }
        printf("value: %s\ndetail: %s\nmatched: %s\nrest: %s\n",
               script_result.text, script_result.detail,
               script_result.matched, script_result.rest);
        dicescript_context_destroy(context);
        return 0;
    }
    const char *expression = argc > 1 ? argv[1] : "4d6kh3";
    dicescript_default_options(&options);
    if (!dicescript_eval(expression, &options, &result)) {
        fprintf(stderr, "error: %s\n", result.error);
        return 1;
    }
    printf("%s\n", result.detail);
    return 0;
}
