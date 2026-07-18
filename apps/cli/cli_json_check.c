/* ctest helper (NOT shipped): parse a captured CLI --json payload with the
 * engine-vendored cJSON and assert its structural shape. A real parse (not
 * substring matching) is the AI-first guarantee -- an agent must be able to pipe
 * stdout straight into a JSON parser.
 *
 *   cli_json_check <file> [mode] [k=v ...]
 *     mode = manifest (default) | inspect | validate | pack | anim | mutation
 *     inspect : sprites=N  -> atlases[0].sprites has exactly N entries
 *     validate: error=N warning=N  -> counts assertions
 *               code=NAME (repeatable) -> a finding with that code exists
 *     pack    : targets_ok=N targets_failed=N -> totals assertions
 *               dry_run=1 -> report.dry_run true, every ok target carries a
 *                            would_write array + empty written_files, 0 files written
 *     anim    : animations[] well-formed; count=N -> exact animation count
 *     mutation: {schema,ok:true,verb,count}; count=N -> exact count
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
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema) || schema->valueint != 1) {
        return fail("manifest: schema must equal 1");
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
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema)) {
        return fail("inspect: missing/!number: schema");
    }
    if (schema->valueint != 4) {
        return fail("inspect: schema must equal 4");
    }
    const char *eschema = arg_val(argc, argv, "schema");
    if (eschema && schema->valueint != atoi(eschema)) {
        (void)fprintf(stderr, "cli_json_check: inspect schema %d != expected %s\n", schema->valueint, eschema);
        return 1;
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
    /* Every sprite carries its resolved keys + derived identity. */
    const cJSON *sp = NULL;
    cJSON_ArrayForEach(sp, sprites) {
        if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(sp, "name")) ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(sp, "key")) ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(sp, "abs")) ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(sp, "source")) ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(sp, "sprite_id"))) {
            return fail("inspect: a sprite is missing name/key/abs/source/sprite_id");
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
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema) || schema->valueint != 2) {
        return fail("validate: schema must equal 2");
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

/* --- anim list --json --- */
static int check_anim(const cJSON *root, int argc, char **argv) {
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema)) {
        return fail("anim: missing/!number: schema");
    }
    if (schema->valueint != 4) {
        return fail("anim: schema must equal 4");
    }
    const char *eschema = arg_val(argc, argv, "schema");
    if (eschema && schema->valueint != atoi(eschema)) {
        (void)fprintf(stderr, "cli_json_check: anim schema %d != expected %s\n", schema->valueint, eschema);
        return 1;
    }
    const cJSON *anims = cJSON_GetObjectItemCaseSensitive(root, "animations");
    if (!cJSON_IsArray(anims)) {
        return fail("anim: missing/!array: animations");
    }
    const cJSON *an = NULL;
    cJSON_ArrayForEach(an, anims) {
        /* schema 3: `id` is an opaque shape-ID, `name` is the human/selector key -- both mandatory. */
        if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(an, "id")) ||
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(an, "name")) ||
            !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(an, "fps")) ||
            !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(an, "playback")) ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(an, "flip_h")) ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(an, "flip_v")) ||
            !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(an, "frames"))) {
            return fail("anim: an entry is missing id/name/fps/playback/flip_h/flip_v/frames");
        }
    }
    const char *want = arg_val(argc, argv, "count");
    if (want && cJSON_GetArraySize(anims) != atoi(want)) {
        (void)fprintf(stderr, "cli_json_check: anim count %d != expected %s\n", cJSON_GetArraySize(anims), want);
        return 1;
    }
    return 0;
}

/* --- mutation success payload ({"schema":1,"ok":true,"verb":..,"count":..}) --- */
static int check_mutation(const cJSON *root, int argc, char **argv) {
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema) || schema->valueint != 1) {
        return fail("mutation: schema must equal 1");
    }
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsBool(ok) || !cJSON_IsTrue(ok)) {
        return fail("mutation: ok must be true");
    }
    if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "verb")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "count"))) {
        return fail("mutation: missing verb/count");
    }
    const char *want = arg_val(argc, argv, "count");
    if (want) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "count");
        if (c->valueint != atoi(want)) {
            (void)fprintf(stderr, "cli_json_check: mutation count %d != expected %s\n", c->valueint, want);
            return 1;
        }
    }
    return 0;
}

/* --- pack --json --- */
static int check_pack(const cJSON *root, int argc, char **argv) {
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema) || schema->valueint != 1) {
        return fail("pack: schema must equal 1");
    }
    const cJSON *atlases = cJSON_GetObjectItemCaseSensitive(root, "atlases");
    if (!cJSON_IsArray(atlases) || cJSON_GetArraySize(atlases) == 0) {
        return fail("pack: missing/empty: atlases");
    }
    const cJSON *a = cJSON_GetArrayItem(atlases, 0);
    if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(a, "name")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(a, "sprite_count")) ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(a, "targets"))) {
        return fail("pack: atlas[0] missing name/sprite_count/targets");
    }
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(a, "pages");
    if (!cJSON_IsArray(pages)) {
        return fail("pack: atlas[0] missing pages");
    }
    const cJSON *pg = NULL;
    cJSON_ArrayForEach(pg, pages) {
        const cJSON *occ = cJSON_GetObjectItemCaseSensitive(pg, "occupancy_pct");
        if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(pg, "w")) ||
            !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(pg, "h")) || !cJSON_IsNumber(occ) ||
            occ->valuedouble <= 0.0 || occ->valuedouble > 100.0) {
            return fail("pack: a page is missing w/h or occupancy_pct out of (0,100]");
        }
    }
    const cJSON *totals = cJSON_GetObjectItemCaseSensitive(root, "totals");
    const cJSON *tok = cJSON_GetObjectItemCaseSensitive(totals, "targets_ok");
    const cJSON *tfail = cJSON_GetObjectItemCaseSensitive(totals, "targets_failed");
    if (!cJSON_IsObject(totals) || !cJSON_IsNumber(tok) || !cJSON_IsNumber(tfail) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(totals, "files_written"))) {
        return fail("pack: totals missing targets_ok/targets_failed/files_written");
    }
    /* timings_ms must exist but its VALUES are never asserted (mask policy). */
    if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "timings_ms"))) {
        return fail("pack: missing timings_ms object");
    }
    /* dry_run is a mandatory bool on the root (B3b schema addition). */
    const cJSON *dry = cJSON_GetObjectItemCaseSensitive(root, "dry_run");
    if (!cJSON_IsBool(dry)) {
        return fail("pack: missing/!bool: dry_run");
    }
    const char *edry = arg_val(argc, argv, "dry_run");
    if (edry && atoi(edry) != 0) {
        if (!cJSON_IsTrue(dry)) {
            return fail("pack: expected dry_run true");
        }
        const cJSON *fw = cJSON_GetObjectItemCaseSensitive(root, "totals");
        fw = cJSON_GetObjectItemCaseSensitive(fw, "files_written");
        if (!cJSON_IsNumber(fw) || fw->valueint != 0) {
            return fail("pack: dry run must report files_written == 0");
        }
        /* every ok target: would_write array present + written_files empty. */
        const cJSON *at = NULL;
        cJSON_ArrayForEach(at, atlases) {
            const cJSON *tgts = cJSON_GetObjectItemCaseSensitive(at, "targets");
            const cJSON *tg = NULL;
            cJSON_ArrayForEach(tg, tgts) {
                const cJSON *stt = cJSON_GetObjectItemCaseSensitive(tg, "status");
                if (!cJSON_IsString(stt) || strcmp(stt->valuestring, "ok") != 0) {
                    continue;
                }
                const cJSON *ww = cJSON_GetObjectItemCaseSensitive(tg, "would_write");
                const cJSON *wf = cJSON_GetObjectItemCaseSensitive(tg, "written_files");
                if (!cJSON_IsArray(ww) || cJSON_GetArraySize(ww) < 1) {
                    return fail("pack: dry run ok target missing a non-empty would_write");
                }
                if (!cJSON_IsArray(wf) || cJSON_GetArraySize(wf) != 0) {
                    return fail("pack: dry run must leave written_files empty");
                }
            }
        }
    }
    const char *eok = arg_val(argc, argv, "targets_ok");
    if (eok && tok->valueint != atoi(eok)) {
        (void)fprintf(stderr, "cli_json_check: pack targets_ok %d != expected %s\n", tok->valueint, eok);
        return 1;
    }
    const char *efail = arg_val(argc, argv, "targets_failed");
    if (efail && tfail->valueint != atoi(efail)) {
        (void)fprintf(stderr, "cli_json_check: pack targets_failed %d != expected %s\n", tfail->valueint, efail);
        return 1;
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
    } else if (strcmp(mode, "pack") == 0) {
        rc = check_pack(root, argc, argv);
    } else if (strcmp(mode, "anim") == 0) {
        rc = check_anim(root, argc, argv);
    } else if (strcmp(mode, "mutation") == 0) {
        rc = check_mutation(root, argc, argv);
    } else {
        rc = check_manifest(root);
    }
    cJSON_Delete(root);
    if (rc == 0) {
        (void)printf("cli_json_check: OK\n");
    }
    return rc;
}
