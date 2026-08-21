#include "dicescript/dicescript.h"

#include <stdio.h>

int main(int argc, char **argv) {
    dicescript_result result;
    dicescript_options options;
    const char *expression = argc > 1 ? argv[1] : "4d6kh3";
    dicescript_default_options(&options);
    if (!dicescript_eval(expression, &options, &result)) {
        fprintf(stderr, "error: %s\n", result.error);
        return 1;
    }
    printf("%s\n", result.detail);
    return 0;
}
