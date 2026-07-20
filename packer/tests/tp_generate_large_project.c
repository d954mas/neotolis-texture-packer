/* Deterministic, user-openable large-project fixture generator.
 *
 * The committed fixture contains 100 atlases, 10 file sources per atlas (1,000
 * source memberships), and one target per atlas. It reuses the existing CC0
 * showcase PNGs so the repository and release archives do not gain 1,000
 * duplicate binary files. Construction uses the white-box model helpers;
 * validation deliberately comes back through the public snapshot/load API.
 *
 *   tp_generate_large_project --write <project.ntpacker_project>
 *   tp_generate_large_project --check <committed-project> <scratch-dir>
 */

#include "tp_core/tp_export.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_session.h"
#include "tp_project_mutation_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LARGE_ATLAS_COUNT 100
#define LARGE_SOURCES_PER_ATLAS 10

static const char *const SOURCE_PATHS[LARGE_SOURCES_PER_ATLAS] = {
    "../showcase/animals/round/elephant.png",
    "../showcase/animals/round/giraffe.png",
    "../showcase/animals/round/hippo.png",
    "../showcase/animals/round/monkey.png",
    "../showcase/animals/round/panda.png",
    "../showcase/animals/round/parrot.png",
    "../showcase/animals/round/penguin.png",
    "../showcase/animals/round/pig.png",
    "../showcase/animals/round/rabbit.png",
    "../showcase/animals/round/snake.png",
};

typedef struct deterministic_rng_state {
    uint64_t counter;
} deterministic_rng_state;

static int deterministic_fill(void *context, uint8_t *out, size_t length) {
    deterministic_rng_state *state = (deterministic_rng_state *)context;
    const uint64_t value = ++state->counter;
    for (size_t i = 0U; i < length; ++i) {
        const unsigned shift = (unsigned)((i % sizeof value) * 8U);
        out[i] = (uint8_t)(value >> shift) ^
                 (uint8_t)(0x6DU + (uint8_t)(i * 29U));
    }
    return (int)length;
}

static char *duplicate_text(const char *text) {
    const size_t length = strlen(text) + 1U;
    char *copy = (char *)malloc(length);
    if (copy) {
        memcpy(copy, text, length);
    }
    return copy;
}

static char *indexed_text(const char *prefix, int index) {
    char buffer[96];
    const int written = snprintf(buffer, sizeof buffer, "%s%03d", prefix,
                                 index);
    return written >= 0 && (size_t)written < sizeof buffer
               ? duplicate_text(buffer)
               : NULL;
}

static bool next_id(const tp_rng *rng, tp_id128 *out, tp_error *error) {
    return tp_id128_generate(rng, out, error) == TP_STATUS_OK;
}

static tp_status create_large_project(tp_project **out, tp_error *error) {
    *out = NULL;
    deterministic_rng_state rng_state = {0U};
    const tp_rng rng = {deterministic_fill, &rng_state};
    tp_project *project = tp_project_create();
    if (!project) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "large fixture project allocation failed");
    }

    tp_status status = TP_STATUS_OK;
    for (int atlas_index = 0; atlas_index < LARGE_ATLAS_COUNT;
         ++atlas_index) {
        char *atlas_name = indexed_text("atlas_", atlas_index);
        char *out_path = indexed_text("out/atlas_", atlas_index);
        int project_index = atlas_index;
        if (!atlas_name || !out_path) {
            free(atlas_name);
            free(out_path);
            status = TP_STATUS_OOM;
            break;
        }
        status = atlas_index == 0
                     ? tp_project_set_atlas_name(&project->atlases[0],
                                                 atlas_name)
                     : tp_project_add_atlas(project, atlas_name,
                                            &project_index);
        free(atlas_name);
        if (status != TP_STATUS_OK) {
            free(out_path);
            break;
        }
        tp_project_atlas *atlas = &project->atlases[project_index];
        if (!next_id(&rng, &atlas->id, error)) {
            free(out_path);
            status = TP_STATUS_RNG_FAILED;
            break;
        }
        tp_project_target *target = NULL;
        status = tp_project_atlas_add_target(
            atlas, TP_EXPORTER_ID_JSON_NEOTOLIS, out_path, &target);
        free(out_path);
        if (status != TP_STATUS_OK ||
            !next_id(&rng, &target->id, error)) {
            status = status != TP_STATUS_OK ? status : TP_STATUS_RNG_FAILED;
            break;
        }
        for (int source_index = 0;
             source_index < LARGE_SOURCES_PER_ATLAS; ++source_index) {
            status = tp_project_atlas_add_source_kind(
                atlas, SOURCE_PATHS[source_index], TP_SOURCE_KIND_FILE);
            tp_project_source *source =
                status == TP_STATUS_OK
                    ? &atlas->sources[atlas->source_count - 1]
                    : NULL;
            if (status != TP_STATUS_OK ||
                !next_id(&rng, &source->id, error)) {
                status = status != TP_STATUS_OK ? status
                                                : TP_STATUS_RNG_FAILED;
                break;
            }
        }
        if (status != TP_STATUS_OK) {
            break;
        }
    }
    if (status != TP_STATUS_OK) {
        tp_project_destroy(project);
        return error->msg[0]
                   ? status
                   : tp_error_set(error, status,
                                  "large fixture model construction failed");
    }
    *out = project;
    return TP_STATUS_OK;
}

static bool file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    return fclose(file) == 0;
}

static bool read_file(const char *path, uint8_t **out, size_t *out_length) {
    *out = NULL;
    *out_length = 0U;
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0L, SEEK_END) != 0) {
        if (file) {
            (void)fclose(file);
        }
        return false;
    }
    const long end = ftell(file);
    if (end < 0 || fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return false;
    }
    const size_t length = (size_t)end;
    uint8_t *bytes = (uint8_t *)malloc(length > 0U ? length : 1U);
    const bool read_ok = bytes && fread(bytes, 1U, length, file) == length;
    const bool close_ok = fclose(file) == 0;
    const bool ok = read_ok && close_ok;
    if (!ok) {
        free(bytes);
        return false;
    }
    *out = bytes;
    *out_length = length;
    return true;
}

static bool validate_committed_shape(const char *path, size_t *out_bytes) {
    tp_error error = {{0}};
    tp_session_snapshot *snapshot = NULL;
    const tp_status status = tp_session_snapshot_load(path, &snapshot, &error);
    if (status != TP_STATUS_OK || !snapshot ||
        tp_session_snapshot_project_schema_version(snapshot) != 5 ||
        tp_session_snapshot_atlas_count(snapshot) != LARGE_ATLAS_COUNT) {
        (void)fprintf(stderr, "large fixture load/shape failed: status=%s error=%s\n",
                      tp_status_id(status), error.msg);
        tp_session_snapshot_destroy(snapshot);
        return false;
    }

    int source_total = 0;
    int target_total = 0;
    bool valid = true;
    for (int atlas_index = 0; valid && atlas_index < LARGE_ATLAS_COUNT;
         ++atlas_index) {
        char expected_name[32];
        char expected_out[48];
        (void)snprintf(expected_name, sizeof expected_name, "atlas_%03d",
                       atlas_index);
        (void)snprintf(expected_out, sizeof expected_out, "out/atlas_%03d",
                       atlas_index);
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(snapshot, atlas_index);
        valid = atlas && strcmp(atlas->name, expected_name) == 0 &&
                atlas->source_count == LARGE_SOURCES_PER_ATLAS &&
                atlas->target_count == 1;
        for (int source_index = 0;
             valid && source_index < LARGE_SOURCES_PER_ATLAS; ++source_index) {
            const tp_snapshot_source *source = tp_session_snapshot_source_at(
                snapshot, atlas->id, source_index);
            char resolved[TP_IDENTITY_PATH_MAX];
            valid = source && source->kind == TP_SNAPSHOT_SOURCE_FILE &&
                    strcmp(source->path, SOURCE_PATHS[source_index]) == 0 &&
                    tp_session_snapshot_resolve_path(
                        snapshot, atlas->id, source->id, resolved,
                        sizeof resolved, &error) == TP_STATUS_OK &&
                    file_exists(resolved);
            source_total++;
        }
        const tp_snapshot_target *target = valid
                                               ? tp_session_snapshot_target_at(
                                                     snapshot, atlas->id, 0)
                                               : NULL;
        valid = valid && target && target->enabled &&
                strcmp(target->exporter_id,
                       TP_EXPORTER_ID_JSON_NEOTOLIS) == 0 &&
                strcmp(target->out_path, expected_out) == 0;
        target_total++;
    }
    tp_session_snapshot_destroy(snapshot);

    uint8_t *bytes = NULL;
    size_t length = 0U;
    valid = valid && source_total == 1000 && target_total == 100 &&
            read_file(path, &bytes, &length);
    free(bytes);
    if (!valid) {
        (void)fprintf(stderr,
                      "large fixture contract failed: atlases=%d sources=%d targets=%d error=%s\n",
                      LARGE_ATLAS_COUNT, source_total, target_total, error.msg);
        return false;
    }
    *out_bytes = length;
    return true;
}

static int write_fixture(const char *path) {
    tp_error error = {{0}};
    tp_project *project = NULL;
    tp_status status = create_large_project(&project, &error);
    if (status == TP_STATUS_OK) {
        status = tp_project_save(project, path, &error);
    }
    tp_project_destroy(project);
    if (status != TP_STATUS_OK) {
        (void)fprintf(stderr, "large fixture write failed: status=%s error=%s\n",
                      tp_status_id(status), error.msg);
        return 1;
    }
    return 0;
}

static int check_fixture(const char *committed_path, const char *scratch_dir) {
    char generated_path[TP_IDENTITY_PATH_MAX];
    const int written = snprintf(
        generated_path, sizeof generated_path,
        "%s/tp_large_project_generated.ntpacker_project", scratch_dir);
    if (written < 0 || (size_t)written >= sizeof generated_path) {
        (void)fprintf(stderr, "large fixture scratch path is too long\n");
        return 1;
    }
    (void)remove(generated_path);
    if (write_fixture(generated_path) != 0) {
        return 1;
    }

    uint8_t *expected = NULL;
    uint8_t *actual = NULL;
    size_t expected_length = 0U;
    size_t actual_length = 0U;
    size_t structural_bytes = 0U;
    const bool valid = read_file(generated_path, &expected, &expected_length) &&
                       read_file(committed_path, &actual, &actual_length) &&
                       expected_length == actual_length &&
                       memcmp(expected, actual, expected_length) == 0 &&
                       validate_committed_shape(committed_path,
                                                &structural_bytes);
    free(expected);
    free(actual);
    (void)remove(generated_path);
    if (!valid) {
        (void)fprintf(stderr,
                      "large fixture differs from deterministic generation: committed=%s\n",
                      committed_path);
        return 1;
    }
    (void)printf(
        "tp_large_project_contract: OK atlases=%d sources=%d targets=%d bytes=%zu\n",
        LARGE_ATLAS_COUNT, LARGE_ATLAS_COUNT * LARGE_SOURCES_PER_ATLAS,
        LARGE_ATLAS_COUNT, structural_bytes);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--write") == 0) {
        return write_fixture(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "--check") == 0) {
        return check_fixture(argv[2], argv[3]);
    }
    (void)fprintf(
        stderr,
        "usage: tp_generate_large_project --write <project>\n"
        "       tp_generate_large_project --check <committed-project> <scratch-dir>\n");
    return 2;
}
