/* Deterministic, user-openable owner-scale bench-project fixture generator (U-01).
 *
 * The committed fixture assembles 36 atlases (5,480 total source memberships,
 * one JSON export target per atlas) out of the 2,020-image Kenney CC0 manifest
 * (examples/bench-assets/manifest.tsv), reusing those already-committed PNGs
 * so the repository does not gain duplicate binary assets. Five composition
 * groups model realistic owner-scale shapes, built in this exact order so
 * `--write` is byte-identical across runs:
 *
 *   1. Homogeneous  -- consecutive 120-source chunks of each source pack
 *                      (platformer-art-deluxe, ui-pack, tiny-dungeon in turn).
 *   2. Mixed        -- 10 atlases of 280 sources each, striding across the
 *                      concatenation of all three homogeneous packs so
 *                      sources are shared across atlases.
 *   3. Duplicate    -- 3 atlases that are exact source-list twins of
 *                      homogeneous atlases 0-2 (new identity, same sources).
 *   4. Proto multi-page -- the prototype-textures pack (large images, up to
 *                      1024x1024) split into 3 atlases; each holds enough big
 *                      textures to spill across several 2048px pages at pack
 *                      time. Multi-page is an emergent pack-time property of the
 *                      content volume, not a serialized knob, so the harness
 *                      detects it from the pack result (page_count > 1).
 *   5. Size-varied  -- 2 atlases each blending 60/60/20/10 sources from
 *                      plat/ui/proto/tiny via modulo indexing.
 *
 * Construction uses the white-box model helpers; validation deliberately
 * comes back through the public snapshot/load API (mirrors
 * tp_generate_large_project.c).
 *
 *   tp_generate_bench_project --manifest <manifest.tsv> --write <project.ntpacker_project>
 *   tp_generate_bench_project --manifest <manifest.tsv> --check <committed-project> <scratch-dir>
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

#define BENCH_ASSET_PREFIX "../bench-assets/kenney/"

#define BENCH_CHUNK 120
#define BENCH_MIX_ATLASES 10
#define BENCH_MIX_SIZE 280
#define BENCH_MIX_STRIDE 7
#define BENCH_DUP_ATLASES 3
#define BENCH_PROTO_ATLASES 3
#define BENCH_SV_ATLASES 2
#define BENCH_SV_SOURCES_PER_ATLAS (60 + 60 + 20 + 10)

/* The pinned manifest contract (examples/bench-assets/manifest.tsv). The
 * generator refuses to silently adapt to a different shape -- see the
 * pool-size check in create_bench_project. */
#define BENCH_EXPECTED_PLAT_COUNT 942
#define BENCH_EXPECTED_UI_COUNT 868
#define BENCH_EXPECTED_TINY_COUNT 132
#define BENCH_EXPECTED_PROTO_COUNT 78

#define BENCH_CEIL_DIV(n, d) (((n) + (d)-1) / (d))
#define BENCH_HOMOGENEOUS_ATLASES                                            \
    (BENCH_CEIL_DIV(BENCH_EXPECTED_PLAT_COUNT, BENCH_CHUNK) +                \
     BENCH_CEIL_DIV(BENCH_EXPECTED_UI_COUNT, BENCH_CHUNK) +                  \
     BENCH_CEIL_DIV(BENCH_EXPECTED_TINY_COUNT, BENCH_CHUNK))

#define BENCH_PROTO_ATLAS_START                                              \
    (BENCH_HOMOGENEOUS_ATLASES + BENCH_MIX_ATLASES + BENCH_DUP_ATLASES)
#define BENCH_EXPECTED_ATLAS_COUNT                                           \
    (BENCH_HOMOGENEOUS_ATLASES + BENCH_MIX_ATLASES + BENCH_DUP_ATLASES +     \
     BENCH_PROTO_ATLASES + BENCH_SV_ATLASES)

/* The first BENCH_DUP_ATLASES homogeneous atlases are always full BENCH_CHUNK
 * chunks (the platformer-art-deluxe pack alone yields more than that many
 * chunks), so the duplicate group always contributes exactly
 * BENCH_DUP_ATLASES * BENCH_CHUNK memberships. */
#define BENCH_EXPECTED_MEMBERSHIP_COUNT                                      \
    (BENCH_EXPECTED_PLAT_COUNT + BENCH_EXPECTED_UI_COUNT +                   \
     BENCH_EXPECTED_TINY_COUNT + (BENCH_MIX_ATLASES * BENCH_MIX_SIZE) +      \
     (BENCH_DUP_ATLASES * BENCH_CHUNK) + BENCH_EXPECTED_PROTO_COUNT +        \
     (BENCH_SV_ATLASES * BENCH_SV_SOURCES_PER_ATLAS))

/* Project-relative source paths for one manifest pool, in manifest order. */
typedef struct path_pool {
    char **paths;
    int count;
    int cap;
} path_pool;

static bool path_pool_push(path_pool *pool, char *path) {
    if (pool->count == pool->cap) {
        const int new_cap = pool->cap == 0 ? 64 : pool->cap * 2;
        char **grown = (char **)realloc(pool->paths, sizeof(char *) * (size_t)new_cap);
        if (!grown) {
            return false;
        }
        pool->paths = grown;
        pool->cap = new_cap;
    }
    pool->paths[pool->count] = path;
    pool->count += 1;
    return true;
}

static void path_pool_free(path_pool *pool) {
    for (int i = 0; i < pool->count; ++i) {
        free(pool->paths[i]);
    }
    free(pool->paths);
    pool->paths = NULL;
    pool->count = 0;
    pool->cap = 0;
}

/* A borrowed, non-owning view of one atlas's source-path list, captured
 * during the homogeneous group so the duplicate group can replay it. */
typedef struct atlas_slice {
    char *const *paths;
    int count;
} atlas_slice;

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
    const int written = snprintf(buffer, sizeof buffer, "%s%04d", prefix, index);
    return written >= 0 && (size_t)written < sizeof buffer
               ? duplicate_text(buffer)
               : NULL;
}

static bool next_id(const tp_rng *rng, tp_id128 *out, tp_error *error) {
    return tp_id128_generate(rng, out, error) == TP_STATUS_OK;
}

static bool has_prefix(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

/* Parses a decimal field of exactly `len` bytes at `field` into *out. Rejects
 * empty, non-numeric, non-positive, or unreasonably large values. */
static bool parse_uint_field(const char *field, size_t len, int *out) {
    if (len == 0U || len >= 32U) {
        return false;
    }
    char buffer[32];
    memcpy(buffer, field, len);
    buffer[len] = '\0';
    char *end = NULL;
    const long value = strtol(buffer, &end, 10);
    if (end == buffer || *end != '\0' || value <= 0 || value > 1000000L) {
        return false;
    }
    *out = (int)value;
    return true;
}

/* One non-header manifest row: "<relpath>\t<width>\t<height>\t<bytes>\t<sha256>".
 * Routes the project-relative path into the pool matching relpath's top-level
 * pack prefix; width/height are parsed and validated but otherwise unused --
 * pool routing is purely by prefix, preserving manifest order. */
static tp_status parse_manifest_row(const char *line, size_t line_len,
                                     path_pool *plat, path_pool *ui,
                                     path_pool *tiny, path_pool *proto,
                                     tp_error *error) {
    const char *tab1 = (const char *)memchr(line, '\t', line_len);
    if (!tab1) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "bench manifest row is missing tab-separated fields");
    }
    const size_t relpath_len = (size_t)(tab1 - line);
    const char *rest = tab1 + 1;
    const size_t rest_len = line_len - relpath_len - 1U;
    const char *tab2 = (const char *)memchr(rest, '\t', rest_len);
    if (!tab2) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "bench manifest row is missing the height field");
    }
    const size_t width_len = (size_t)(tab2 - rest);
    const char *rest2 = tab2 + 1;
    const size_t rest2_len = rest_len - width_len - 1U;
    const char *tab3 = (const char *)memchr(rest2, '\t', rest2_len);
    const size_t height_len = tab3 ? (size_t)(tab3 - rest2) : rest2_len;

    int width = 0;
    int height = 0;
    if (relpath_len == 0U || relpath_len >= TP_IDENTITY_PATH_MAX ||
        !parse_uint_field(rest, width_len, &width) ||
        !parse_uint_field(rest2, height_len, &height)) {
        return tp_error_set(
            error, TP_STATUS_INVALID_ARGUMENT,
            "bench manifest row has a malformed relpath/width/height field");
    }
    (void)width;
    (void)height;

    char relpath[TP_IDENTITY_PATH_MAX];
    memcpy(relpath, line, relpath_len);
    relpath[relpath_len] = '\0';

    char full_path[TP_IDENTITY_PATH_MAX];
    const int written =
        snprintf(full_path, sizeof full_path, "%s%s", BENCH_ASSET_PREFIX, relpath);
    if (written < 0 || (size_t)written >= sizeof full_path) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "bench manifest relpath is too long: %s", relpath);
    }

    path_pool *target_pool = NULL;
    if (has_prefix(relpath, "platformer-art-deluxe/")) {
        target_pool = plat;
    } else if (has_prefix(relpath, "ui-pack/")) {
        target_pool = ui;
    } else if (has_prefix(relpath, "tiny-dungeon/")) {
        target_pool = tiny;
    } else if (has_prefix(relpath, "prototype-textures/")) {
        target_pool = proto;
    } else {
        return tp_error_set(
            error, TP_STATUS_INVALID_ARGUMENT,
            "bench manifest relpath does not match any known pool prefix: %s",
            relpath);
    }

    char *stored_path = duplicate_text(full_path);
    if (!stored_path) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "bench manifest path allocation failed");
    }
    if (!path_pool_push(target_pool, stored_path)) {
        free(stored_path);
        return tp_error_set(error, TP_STATUS_OOM,
                            "bench manifest pool allocation failed");
    }
    return TP_STATUS_OK;
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

/* Reads the manifest file, skips its '#' header line, and partitions every
 * row into the pool matching its relpath prefix, preserving manifest order. */
static tp_status parse_manifest(const char *manifest_path, path_pool *plat,
                                path_pool *ui, path_pool *tiny, path_pool *proto,
                                tp_error *error) {
    uint8_t *buffer = NULL;
    size_t length = 0U;
    if (!read_file(manifest_path, &buffer, &length)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "bench manifest could not be read: %s", manifest_path);
    }

    tp_status status = TP_STATUS_OK;
    size_t pos = 0U;
    bool first_line = true;
    while (pos < length && status == TP_STATUS_OK) {
        const size_t line_start = pos;
        while (pos < length && buffer[pos] != (uint8_t)'\n') {
            ++pos;
        }
        size_t line_end = pos;
        if (pos < length) {
            ++pos;
        }
        if (line_end > line_start && buffer[line_end - 1U] == (uint8_t)'\r') {
            --line_end;
        }
        const size_t line_len = line_end - line_start;
        if (first_line) {
            first_line = false;
            if (line_len == 0U || buffer[line_start] != (uint8_t)'#') {
                status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                      "bench manifest is missing its '#' header line");
            }
            continue;
        }
        if (line_len == 0U) {
            continue;
        }
        status = parse_manifest_row((const char *)buffer + line_start, line_len,
                                    plat, ui, tiny, proto, error);
    }
    free(buffer);
    return status;
}

/* Creates one atlas (name "bench_%04d", target out_path "out/bench_%04d",
 * exactly `source_count` sources from `paths` in order), driving structural
 * IDs from `rng` in creation order (atlas, then target, then each source). */
static tp_status create_one_atlas(tp_project *project, const tp_rng *rng,
                                  int atlas_seq, char *const *paths,
                                  int source_count, tp_error *error) {
    char *atlas_name = indexed_text("bench_", atlas_seq);
    char *out_path = indexed_text("out/bench_", atlas_seq);
    if (!atlas_name || !out_path) {
        free(atlas_name);
        free(out_path);
        return tp_error_set(error, TP_STATUS_OOM,
                            "bench fixture atlas name allocation failed");
    }

    int project_index = atlas_seq;
    tp_status status = atlas_seq == 0
                            ? tp_project_set_atlas_name(&project->atlases[0],
                                                        atlas_name)
                            : tp_project_add_atlas(project, atlas_name,
                                                   &project_index);
    free(atlas_name);
    if (status != TP_STATUS_OK) {
        free(out_path);
        return status;
    }

    tp_project_atlas *atlas = &project->atlases[project_index];
    if (!next_id(rng, &atlas->id, error)) {
        free(out_path);
        return TP_STATUS_RNG_FAILED;
    }

    tp_project_target *target = NULL;
    status = tp_project_atlas_add_target(atlas, TP_EXPORTER_ID_JSON_NEOTOLIS,
                                         out_path, &target);
    free(out_path);
    if (status != TP_STATUS_OK || !next_id(rng, &target->id, error)) {
        return status != TP_STATUS_OK ? status : TP_STATUS_RNG_FAILED;
    }

    for (int i = 0; i < source_count; ++i) {
        const int before = atlas->source_count;
        status = tp_project_atlas_add_source_kind(atlas, paths[i],
                                                  TP_SOURCE_KIND_FILE);
        if (status == TP_STATUS_OK && atlas->source_count == before) {
            /* add_source_kind dedupes by normalized path. A silent no-op here would
             * misassign this source's id to the PRIOR source and shift the whole RNG
             * stream, regenerating a self-consistent WRONG file. The pinned manifest has
             * distinct paths (proven), so this only fails closed on a future manifest edit. */
            return tp_error_set(
                error, TP_STATUS_INVALID_ARGUMENT,
                "bench fixture: duplicate source path within one atlas: %s", paths[i]);
        }
        tp_project_source *source =
            status == TP_STATUS_OK ? &atlas->sources[atlas->source_count - 1]
                                   : NULL;
        if (status != TP_STATUS_OK || !next_id(rng, &source->id, error)) {
            return status != TP_STATUS_OK ? status : TP_STATUS_RNG_FAILED;
        }
    }
    return TP_STATUS_OK;
}

/* Group 1: consecutive BENCH_CHUNK-sized slices of [plat, ui, tiny] in turn.
 * Records the first BENCH_DUP_ATLASES atlases' source-path views for group 3. */
static tp_status build_homogeneous_group(
    tp_project *project, const tp_rng *rng, int *atlas_seq,
    const path_pool *plat, const path_pool *ui, const path_pool *tiny,
    atlas_slice dup_snapshots[BENCH_DUP_ATLASES], tp_error *error) {
    const path_pool *pools[3] = {plat, ui, tiny};
    tp_status status = TP_STATUS_OK;
    for (int pool_index = 0; pool_index < 3 && status == TP_STATUS_OK;
         ++pool_index) {
        const path_pool *pool = pools[pool_index];
        for (int offset = 0; offset < pool->count && status == TP_STATUS_OK;
             offset += BENCH_CHUNK) {
            const int remaining = pool->count - offset;
            const int count = remaining < BENCH_CHUNK ? remaining : BENCH_CHUNK;
            char *const *chunk_paths = pool->paths + offset;
            if (*atlas_seq < BENCH_DUP_ATLASES) {
                dup_snapshots[*atlas_seq].paths = chunk_paths;
                dup_snapshots[*atlas_seq].count = count;
            }
            status = create_one_atlas(project, rng, *atlas_seq, chunk_paths,
                                      count, error);
            if (status == TP_STATUS_OK) {
                *atlas_seq += 1;
            }
        }
    }
    return status;
}

/* Group 2: BENCH_MIX_ATLASES atlases of BENCH_MIX_SIZE sources each, striding
 * across the plat++ui++tiny concatenation. */
static tp_status build_mixed_group(tp_project *project, const tp_rng *rng,
                                   int *atlas_seq, char *const *concat,
                                   int concat_count, tp_error *error) {
    tp_status status = TP_STATUS_OK;
    for (int k = 0; k < BENCH_MIX_ATLASES && status == TP_STATUS_OK; ++k) {
        char *mix_paths[BENCH_MIX_SIZE];
        for (int i = 0; i < BENCH_MIX_SIZE; ++i) {
            const long long index =
                ((long long)k * BENCH_MIX_SIZE * BENCH_MIX_STRIDE +
                 (long long)i * BENCH_MIX_STRIDE) %
                concat_count;
            mix_paths[i] = concat[index];
        }
        status = create_one_atlas(project, rng, *atlas_seq, mix_paths,
                                  BENCH_MIX_SIZE, error);
        if (status == TP_STATUS_OK) {
            *atlas_seq += 1;
        }
    }
    return status;
}

/* Group 3: exact source-list twins of homogeneous atlases 0..BENCH_DUP_ATLASES-1,
 * with fresh identity (new name/out_path/ids) and default knobs. */
static tp_status build_duplicate_group(
    tp_project *project, const tp_rng *rng, int *atlas_seq,
    const atlas_slice dup_snapshots[BENCH_DUP_ATLASES], tp_error *error) {
    tp_status status = TP_STATUS_OK;
    for (int d = 0; d < BENCH_DUP_ATLASES && status == TP_STATUS_OK; ++d) {
        status = create_one_atlas(project, rng, *atlas_seq,
                                  dup_snapshots[d].paths, dup_snapshots[d].count,
                                  error);
        if (status == TP_STATUS_OK) {
            *atlas_seq += 1;
        }
    }
    return status;
}

/* Group 4: the prototype-textures pack (large images) split into
 * BENCH_PROTO_ATLASES consecutive chunks. Default knobs; multi-page is an
 * emergent pack-time property of the big-texture volume, not a project knob. */
static tp_status build_proto_group(tp_project *project, const tp_rng *rng,
                                   int *atlas_seq, const path_pool *proto,
                                   tp_error *error) {
    tp_status status = TP_STATUS_OK;
    const int chunk_size =
        (proto->count + BENCH_PROTO_ATLASES - 1) / BENCH_PROTO_ATLASES;
    for (int j = 0; j < BENCH_PROTO_ATLASES && status == TP_STATUS_OK; ++j) {
        const int offset = j * chunk_size;
        const int remaining = proto->count - offset;
        const int count = remaining < chunk_size ? remaining : chunk_size;
        status = create_one_atlas(project, rng, *atlas_seq,
                                  proto->paths + offset, count, error);
        if (status == TP_STATUS_OK) {
            *atlas_seq += 1;
        }
    }
    return status;
}

/* Group 5: BENCH_SV_ATLASES atlases blending 60 plat + 60 ui + 20 proto +
 * 10 tiny sources each, modulo-indexed so no pool ever overruns. */
static tp_status build_size_varied_group(tp_project *project, const tp_rng *rng,
                                         int *atlas_seq, const path_pool *plat,
                                         const path_pool *ui,
                                         const path_pool *proto,
                                         const path_pool *tiny, tp_error *error) {
    tp_status status = TP_STATUS_OK;
    for (int j = 0; j < BENCH_SV_ATLASES && status == TP_STATUS_OK; ++j) {
        char *sv_paths[BENCH_SV_SOURCES_PER_ATLAS];
        int n = 0;
        for (int t = 0; t < 60; ++t) {
            sv_paths[n++] = plat->paths[(j * 60 + t) % plat->count];
        }
        for (int t = 0; t < 60; ++t) {
            sv_paths[n++] = ui->paths[(j * 60 + t) % ui->count];
        }
        for (int t = 0; t < 20; ++t) {
            sv_paths[n++] = proto->paths[(j * 20 + t) % proto->count];
        }
        for (int t = 0; t < 10; ++t) {
            sv_paths[n++] = tiny->paths[(j * 10 + t) % tiny->count];
        }
        status =
            create_one_atlas(project, rng, *atlas_seq, sv_paths, n, error);
        if (status == TP_STATUS_OK) {
            *atlas_seq += 1;
        }
    }
    return status;
}

static tp_status create_bench_project(const char *manifest_path,
                                      tp_project **out, tp_error *error) {
    *out = NULL;
    path_pool plat = {NULL, 0, 0};
    path_pool ui = {NULL, 0, 0};
    path_pool tiny = {NULL, 0, 0};
    path_pool proto = {NULL, 0, 0};

    tp_status status = parse_manifest(manifest_path, &plat, &ui, &tiny, &proto,
                                      error);
    if (status == TP_STATUS_OK &&
        (plat.count != BENCH_EXPECTED_PLAT_COUNT ||
         ui.count != BENCH_EXPECTED_UI_COUNT ||
         tiny.count != BENCH_EXPECTED_TINY_COUNT ||
         proto.count != BENCH_EXPECTED_PROTO_COUNT)) {
        status = tp_error_set(
            error, TP_STATUS_INVALID_ARGUMENT,
            "bench manifest pool sizes differ from the pinned contract: "
            "plat=%d ui=%d tiny=%d proto=%d",
            plat.count, ui.count, tiny.count, proto.count);
    }

    tp_project *project = NULL;
    if (status == TP_STATUS_OK) {
        project = tp_project_create();
        if (!project) {
            status = TP_STATUS_OOM;
        }
    }

    if (status == TP_STATUS_OK) {
        deterministic_rng_state rng_state = {0U};
        const tp_rng rng = {deterministic_fill, &rng_state};
        atlas_slice dup_snapshots[BENCH_DUP_ATLASES] = {{NULL, 0}};
        int atlas_seq = 0;

        status = build_homogeneous_group(project, &rng, &atlas_seq, &plat, &ui,
                                         &tiny, dup_snapshots, error);

        char **concat = NULL;
        if (status == TP_STATUS_OK) {
            const int concat_count = plat.count + ui.count + tiny.count;
            concat = (char **)malloc(sizeof(char *) * (size_t)concat_count);
            if (!concat) {
                status = tp_error_set(
                    error, TP_STATUS_OOM,
                    "bench fixture mixed-group concat allocation failed");
            } else {
                memcpy(concat, plat.paths, sizeof(char *) * (size_t)plat.count);
                memcpy(concat + plat.count, ui.paths,
                       sizeof(char *) * (size_t)ui.count);
                memcpy(concat + plat.count + ui.count, tiny.paths,
                       sizeof(char *) * (size_t)tiny.count);
                status = build_mixed_group(project, &rng, &atlas_seq, concat,
                                          concat_count, error);
            }
        }
        free(concat);

        if (status == TP_STATUS_OK) {
            status = build_duplicate_group(project, &rng, &atlas_seq,
                                          dup_snapshots, error);
        }
        if (status == TP_STATUS_OK) {
            status = build_proto_group(project, &rng, &atlas_seq, &proto, error);
        }
        if (status == TP_STATUS_OK) {
            status = build_size_varied_group(project, &rng, &atlas_seq, &plat,
                                            &ui, &proto, &tiny, error);
        }
    }

    path_pool_free(&plat);
    path_pool_free(&ui);
    path_pool_free(&tiny);
    path_pool_free(&proto);

    if (status != TP_STATUS_OK) {
        tp_project_destroy(project);
        return error->msg[0]
                   ? status
                   : tp_error_set(error, status,
                                  "bench fixture model construction failed");
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

static bool validate_committed_shape(const char *path, size_t *out_bytes) {
    tp_error error = {{0}};
    tp_session_snapshot *snapshot = NULL;
    const tp_status status = tp_session_snapshot_load(path, &snapshot, &error);
    if (status != TP_STATUS_OK || !snapshot ||
        tp_session_snapshot_project_schema_version(snapshot) != 5 ||
        tp_session_snapshot_atlas_count(snapshot) != BENCH_EXPECTED_ATLAS_COUNT) {
        (void)fprintf(stderr, "bench fixture load/shape failed: status=%s error=%s\n",
                      tp_status_id(status), error.msg);
        tp_session_snapshot_destroy(snapshot);
        return false;
    }

    int source_total = 0;
    bool valid = true;
    for (int atlas_index = 0; valid && atlas_index < BENCH_EXPECTED_ATLAS_COUNT;
         ++atlas_index) {
        char expected_name[32];
        char expected_out[48];
        (void)snprintf(expected_name, sizeof expected_name, "bench_%04d",
                       atlas_index);
        (void)snprintf(expected_out, sizeof expected_out, "out/bench_%04d",
                       atlas_index);
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(snapshot, atlas_index);
        valid = atlas && strcmp(atlas->name, expected_name) == 0 &&
                atlas->target_count == 1;
        /* Group 4 (proto multi-page) lives at a known atlas-index range; assert
         * those atlases really are built from the large prototype-textures pool
         * (the byte-compare pins the exact composition; this pins the scenario). */
        const bool is_proto_atlas =
            atlas && atlas_index >= BENCH_PROTO_ATLAS_START &&
            atlas_index < BENCH_PROTO_ATLAS_START + BENCH_PROTO_ATLASES;
        for (int source_index = 0; valid && source_index < atlas->source_count;
             ++source_index) {
            const tp_snapshot_source *source = tp_session_snapshot_source_at(
                snapshot, atlas->id, source_index);
            char resolved[TP_IDENTITY_PATH_MAX];
            valid = source && source->kind == TP_SNAPSHOT_SOURCE_FILE &&
                    tp_session_snapshot_resolve_path(
                        snapshot, atlas->id, source->id, resolved,
                        sizeof resolved, &error) == TP_STATUS_OK &&
                    file_exists(resolved) &&
                    (!is_proto_atlas ||
                     strstr(source->path, "prototype-textures/") != NULL);
        }
        if (valid) {
            source_total += atlas->source_count;
        }
        const tp_snapshot_target *target =
            valid ? tp_session_snapshot_target_at(snapshot, atlas->id, 0) : NULL;
        valid = valid && target && target->enabled &&
                strcmp(target->exporter_id, TP_EXPORTER_ID_JSON_NEOTOLIS) == 0 &&
                strcmp(target->out_path, expected_out) == 0;
    }
    tp_session_snapshot_destroy(snapshot);

    uint8_t *bytes = NULL;
    size_t length = 0U;
    valid = valid && source_total == BENCH_EXPECTED_MEMBERSHIP_COUNT &&
            read_file(path, &bytes, &length);
    free(bytes);
    if (!valid) {
        (void)fprintf(stderr,
                      "bench fixture contract failed: atlases=%d memberships=%d error=%s\n",
                      BENCH_EXPECTED_ATLAS_COUNT, source_total, error.msg);
        return false;
    }
    *out_bytes = length;
    return true;
}

static int write_fixture(const char *manifest_path, const char *path) {
    tp_error error = {{0}};
    tp_project *project = NULL;
    tp_status status = create_bench_project(manifest_path, &project, &error);
    if (status == TP_STATUS_OK) {
        status = tp_project_save(project, path, &error);
    }
    tp_project_destroy(project);
    if (status != TP_STATUS_OK) {
        (void)fprintf(stderr, "bench fixture write failed: status=%s error=%s\n",
                      tp_status_id(status), error.msg);
        return 1;
    }
    return 0;
}

static int check_fixture(const char *manifest_path, const char *committed_path,
                         const char *scratch_dir) {
    char generated_path[TP_IDENTITY_PATH_MAX];
    const int written = snprintf(
        generated_path, sizeof generated_path,
        "%s/tp_bench_project_generated.ntpacker_project", scratch_dir);
    if (written < 0 || (size_t)written >= sizeof generated_path) {
        (void)fprintf(stderr, "bench fixture scratch path is too long\n");
        return 1;
    }
    (void)remove(generated_path);
    if (write_fixture(manifest_path, generated_path) != 0) {
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
                       validate_committed_shape(committed_path, &structural_bytes);
    free(expected);
    free(actual);
    (void)remove(generated_path);
    if (!valid) {
        (void)fprintf(stderr,
                      "bench fixture differs from deterministic generation: committed=%s\n",
                      committed_path);
        return 1;
    }
    (void)printf(
        "tp_bench_project_contract: OK atlases=%d memberships=%d bytes=%zu\n",
        BENCH_EXPECTED_ATLAS_COUNT, BENCH_EXPECTED_MEMBERSHIP_COUNT,
        structural_bytes);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "--manifest") == 0 &&
        strcmp(argv[3], "--write") == 0) {
        return write_fixture(argv[2], argv[4]);
    }
    if (argc == 6 && strcmp(argv[1], "--manifest") == 0 &&
        strcmp(argv[3], "--check") == 0) {
        return check_fixture(argv[2], argv[4], argv[5]);
    }
    (void)fprintf(
        stderr,
        "usage: tp_generate_bench_project --manifest <manifest.tsv> --write <project>\n"
        "       tp_generate_bench_project --manifest <manifest.tsv> --check <committed-project> <scratch-dir>\n");
    return 2;
}
