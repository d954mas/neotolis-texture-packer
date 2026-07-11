/* ctest helper (NOT shipped): parse a captured CLI --json payload with the
 * engine-vendored cJSON and assert its structural shape. A real parse (not
 * substring matching) is the AI-first guarantee -- an agent must be able to pipe
 * stdout straight into a JSON parser.
 *
 *   cli_json_check <file> [mode] [k=v ...]
 *     mode = manifest (default) | inspect | validate
 *     inspect : sprites=N  -> atlases[0].sprites has exactly N entries
 *     validate: error=N warning=N  -> counts assertions
 *               code=NAME (repeatable) -> a finding with that code exists
 *
 * No exporter-id literals here -> boundary gate R2 stays clean. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static int fail(const char *why) {
    (void)fprintf(stderr, "cli_json_check: %s\n", why);
    return 1;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1U);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1U, (size_t)sz, f);
    (void)fclose(f);
    buf[rd] = '\0';
    return buf;
}

/* --- manifest (version --json) --- */
static int check_manifest(const cJSON *root) {
    if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "schema"))) {
        return fail("missing/!number: schema");
    }
    if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "app_version"))) {
        return fail("missing/!string: app_version");
    }
    if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "project_schema"))) {
        return fail("missing/!number: project_schema");
    }
    if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "verbs"))) {
        return fail("missing/!object: verbs");
    }
    if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "formats"))) {
        return fail("missing/!object: formats");
    }
    const cJSON *exporters = cJSON_GetObjectItemCaseSensitive(root, "exporters");
    if (!cJSON_IsArray(exporters)) {
        return fail("missing/!array: exporters");
    }
    if (cJSON_GetArraySize(exporters) < 1) {
        return fail("exporters array is empty");
    }
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, exporters) {
        if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(e, "id")) ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(e, "name")) ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(e, "ext")) ||
            !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(e, "caps"))) {
            return fail("an exporter entry is missing id/name/ext/caps");
        }
    }
    return 0;
}

/* Optional "key=value" arg lookup (first match). Returns NULL if absent. */
static const char *arg_val(int argc, char **argv, const char *key) {
    size_t kl = strlen(key);
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], key, kl) == 0 && argv[i][kl] == '=') {
            return argv[i] + kl + 1;
        }
    }
    return NULL;
}

/* --- inspect --json --- */
static int check_inspect(const cJSON *root, int argc, char **argv) {
    if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "schema"))) {
        return fail("inspect: missing/!number: schema");
    }
    const cJSON *proj = cJSON_GetObjectItemCaseSensitive(root, "project");
    if (!cJSON_IsObject(proj)) {
        return fail("inspect: missing/!object: project");
    }
    if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(proj, "path")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(proj, "schema_version")) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(proj, "project_dir"))) {
        return fail("inspect: project missing path/schema_version/project_dir");
    }
    const cJSON *atlases = cJSON_GetObjectItemCaseSensitive(root, "atlases");
    if (!cJSON_IsArray(atlases) || cJSON_GetArraySize(atlases) < 1) {
        return fail("inspect: missing/empty: atlases");
    }
    const cJSON *a0 = cJSON_GetArrayItem(atlases, 0);
    if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(a0, "name")) ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(a0, "settings")) ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(a0, "sources")) ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(a0, "sprites")) ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(a0, "animations")) ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(a0, "targets"))) {
        return fail("inspect: atlas[0] missing name/settings/sources/sprites/animations/targets");
    }
    const cJSON *sprites = cJSON_GetObjectItemCaseSensitive(a0, "sprites");
    /* Every sprite carries the resolved identity trio. */
    const cJSON *sp = NULL;
    cJSON_ArrayForEach(sp, sprites) {
        if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(sp, "name")) ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(sp, "key")) ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(sp, "abs"))) {
            return fail("inspect: a sprite is missing name/key/abs");
        }
    }
    const char *want = arg_val(argc, argv, "sprites");
    if (want) {
        int got = cJSON_GetArraySize(sprites);
        if (got != atoi(want)) {
            (void)fprintf(stderr, "cli_json_check: inspect sprite count %d != expected %s\n", got, want);
            return 1;
        }
    }
    return 0;
}

/* --- validate --json --- */
static int check_validate(const cJSON *root, int argc, char **argv) {
    if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "schema"))) {
        return fail("validate: missing/!number: schema");
    }
    const cJSON *findings = cJSON_GetObjectItemCaseSensitive(root, "findings");
    if (!cJSON_IsArray(findings)) {
        return fail("validate: missing/!array: findings");
    }
    const cJSON *counts = cJSON_GetObjectItemCaseSensitive(root, "counts");
    const cJSON *cerr = cJSON_GetObjectItemCaseSensitive(counts, "error");
    const cJSON *cwarn = cJSON_GetObjectItemCaseSensitive(counts, "warning");
    if (!cJSON_IsObject(counts) || !cJSON_IsNumber(cerr) || !cJSON_IsNumber(cwarn)) {
        return fail("validate: counts missing error/warning numbers");
    }
    /* Every finding has the mandatory trio. */
    const cJSON *f = NULL;
    cJSON_ArrayForEach(f, findings) {
        const cJSON *sev = cJSON_GetObjectItemCaseSensitive(f, "severity");
        if (!cJSON_IsString(sev) ||
            (strcmp(sev->valuestring, "error") != 0 && strcmp(sev->valuestring, "warning") != 0) ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(f, "code")) ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(f, "message"))) {
            return fail("validate: a finding is missing severity/code/message");
        }
    }
    /* Optional assertions. */
    const char *ce = arg_val(argc, argv, "error");
    if (ce && cerr->valueint != atoi(ce)) {
        (void)fprintf(stderr, "cli_json_check: validate error count %d != expected %s\n", cerr->valueint, ce);
        return 1;
    }
    const char *cw = arg_val(argc, argv, "warning");
    if (cw && cwarn->valueint != atoi(cw)) {
        (void)fprintf(stderr, "cli_json_check: validate warning count %d != expected %s\n", cwarn->valueint, cw);
        return 1;
    }
    /* Each code=NAME must appear on some finding. */
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "code=", 5) != 0) {
            continue;
        }
        const char *want = argv[i] + 5;
        bool found = false;
        cJSON_ArrayForEach(f, findings) {
            const cJSON *code = cJSON_GetObjectItemCaseSensitive(f, "code");
            if (cJSON_IsString(code) && strcmp(code->valuestring, want) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            (void)fprintf(stderr, "cli_json_check: validate expected a finding with code '%s'\n", want);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return fail("usage: cli_json_check <json-file> [mode] [k=v ...]");
    }
    const char *mode = (argc >= 3 && strchr(argv[2], '=') == NULL) ? argv[2] : "manifest";

    char *buf = read_file(argv[1]);
    if (!buf) {
        return fail("cannot read input file");
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return fail("stdout payload is not valid JSON");
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return fail("root is not an object");
    }

    int rc;
    if (strcmp(mode, "inspect") == 0) {
        rc = check_inspect(root, argc, argv);
    } else if (strcmp(mode, "validate") == 0) {
        rc = check_validate(root, argc, argv);
    } else {
        rc = check_manifest(root);
    }
    cJSON_Delete(root);
    if (rc == 0) {
        (void)printf("cli_json_check: OK\n");
    }
    return rc;
}
