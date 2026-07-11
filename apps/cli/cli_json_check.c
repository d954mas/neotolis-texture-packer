/* ctest helper (NOT shipped): parse a captured CLI --json payload with the
 * engine-vendored cJSON and assert the version manifest's structural shape. A
 * real parse (not substring matching) is the AI-first guarantee -- an agent must
 * be able to pipe stdout straight into a JSON parser. Only structural keys are
 * checked here (no exporter-id literals -> boundary gate R2 stays clean). */
#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"

static int fail(const char *why) {
    (void)fprintf(stderr, "cli_json_check: %s\n", why);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return fail("usage: cli_json_check <json-file>");
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        return fail("cannot open input file");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return fail("seek failed");
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return fail("tell/rewind failed");
    }
    char *buf = (char *)malloc((size_t)sz + 1U);
    if (!buf) {
        (void)fclose(f);
        return fail("out of memory");
    }
    size_t rd = fread(buf, 1U, (size_t)sz, f);
    (void)fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return fail("stdout payload is not valid JSON");
    }

    int rc = 0;
    if (!cJSON_IsObject(root)) {
        rc = fail("root is not an object");
    } else if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "schema"))) {
        rc = fail("missing/!number: schema");
    } else if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "app_version"))) {
        rc = fail("missing/!string: app_version");
    } else if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "project_schema"))) {
        rc = fail("missing/!number: project_schema");
    } else if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "verbs"))) {
        rc = fail("missing/!object: verbs");
    } else if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "formats"))) {
        rc = fail("missing/!object: formats");
    } else {
        const cJSON *exporters = cJSON_GetObjectItemCaseSensitive(root, "exporters");
        if (!cJSON_IsArray(exporters)) {
            rc = fail("missing/!array: exporters");
        } else if (cJSON_GetArraySize(exporters) < 1) {
            rc = fail("exporters array is empty");
        } else {
            const cJSON *e = NULL;
            cJSON_ArrayForEach(e, exporters) {
                if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(e, "id")) ||
                    !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(e, "name")) ||
                    !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(e, "ext")) ||
                    !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(e, "caps"))) {
                    rc = fail("an exporter entry is missing id/name/ext/caps");
                    break;
                }
            }
        }
    }

    cJSON_Delete(root);
    if (rc == 0) {
        (void)printf("cli_json_check: OK\n");
    }
    return rc;
}
