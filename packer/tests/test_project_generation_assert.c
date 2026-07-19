#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile the production owner directly so this death probe exercises the
 * Release assertion mode of the exact implementation under test. tp_core is
 * linked only for tp_project_destroy; its archive copy of this TU is not pulled. */
#include "../src/tp_project_generation.c"

static int run_invalid_release_child(void) {
    struct tp_project_generation generation;
    atomic_init(&generation.refs, 0U);
    generation.project = NULL;
    tp_project_generation_release(&generation);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--invalid-release-child") == 0) {
        return run_invalid_release_child();
    }

    char command[4096];
#ifdef _WIN32
    const int written = snprintf(command, sizeof command,
                                 "\"%s\" --invalid-release-child >NUL 2>&1",
                                 argv[0]);
#else
    const int written = snprintf(command, sizeof command,
                                 "\"%s\" --invalid-release-child >/dev/null 2>&1",
                                 argv[0]);
#endif
    if (written < 0 || (size_t)written >= sizeof command) {
        return 2;
    }
    return system(command) == 0 ? 1 : 0;
}
