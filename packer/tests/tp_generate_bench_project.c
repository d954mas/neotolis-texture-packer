/* Deterministic, user-openable owner-scale bench-project fixture generator (U-01).
 *
 * The committed fixture assembles 37 atlases (5,481 total source memberships,
 * one JSON export target per atlas) out of the 2,020-image Kenney CC0 manifest
 * (examples/bench-assets/manifest.tsv), reusing those already-committed PNGs
 * so the repository does not gain duplicate binary assets. Six composition
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
 *   6. Folder       -- 1 atlas whose single source is a TP_SOURCE_KIND_FOLDER
 *                      pointing at the committed tiny-dungeon tree. This is the
 *                      only source that exercises the worker-side folder walk
 *                      (tp_scan_dir + the descriptor loop -- the U-01a cost) under
 *                      --bench-perf / --pack-hash-probe. It stays LFS-independent
 *                      for the byte-stable contract: that check only verifies the
 *                      folder resolves to a real directory (which exists in every
 *                      checkout), never scanning the *.png inside it -- so an
 *                      unmaterialized-LFS checkout degrades like F17, never fails.
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
#include "tp_core/tp_scan.h" /* tp_scan_is_dir: folder-source shape check */
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
#define BENCH_FOLDER_ATLASES 1
/* Group 6's lone folder source. BENCH_ASSET_PREFIX already ends in '/', so this
 * resolves (project-relative) to the committed tiny-dungeon directory. The tree
 * exists in every checkout even when its *.png are unmaterialized LFS pointers. */
#define BENCH_FOLDER_SOURCE_PATH BENCH_ASSET_PREFIX "tiny-dungeon"

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
/* The folder group is built LAST, so its atlas occupies the final index range;
 * placing it after size-varied keeps BENCH_PROTO_ATLAS_START (and every earlier
 * atlas index) unchanged. */
#define BENCH_FOLDER_ATLAS_START                                             \
    (BENCH_HOMOGENEOUS_ATLASES + BENCH_MIX_ATLASES + BENCH_DUP_ATLASES +     \
     BENCH_PROTO_ATLASES + BENCH_SV_ATLASES)
#define BENCH_EXPECTED_ATLAS_COUNT                                           \
    (BENCH_FOLDER_ATLAS_START + BENCH_FOLDER_ATLASES)

/* The first BENCH_DUP_ATLASES homogeneous atlases are always full BENCH_CHUNK
 * chunks (the platformer-art-deluxe pack alone yields more than that many
 * chunks), so the duplicate group always contributes exactly
 * BENCH_DUP_ATLASES * BENCH_CHUNK memberships. */
/* The folder group contributes one MEMBERSHIP per atlas (a folder source is a
 * single source entry regardless of how many files it enumerates at pack time). */
#define BENCH_EXPECTED_MEMBERSHIP_COUNT                                      \
    (BENCH_EXPECTED_PLAT_COUNT + BENCH_EXPECTED_UI_COUNT +                   \
     BENCH_EXPECTED_TINY_COUNT + (BENCH_MIX_ATLASES * BENCH_MIX_SIZE) +      \
     (BENCH_DUP_ATLASES * BENCH_CHUNK) + BENCH_EXPECTED_PROTO_COUNT +        \
     (BENCH_SV_ATLASES * BENCH_SV_SOURCES_PER_ATLAS) + BENCH_FOLDER_ATLASES)

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
 * exactly `source_count` sources from `paths` in order, each added with
 * `source_kind`), driving structural IDs from `rng` in creation order (atlas,
 * then target, then each source). */
static tp_status create_one_atlas(tp_project *project, const tp_rng *rng,
                                  int atlas_seq, char *const *paths,
                                  int source_count, tp_source_kind source_kind,
                                  tp_error *error) {
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
        status = tp_project_atlas_add_source_kind(atlas, paths[i], source_kind);
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
                                      count, TP_SOURCE_KIND_FILE, error);
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
                                  BENCH_MIX_SIZE, TP_SOURCE_KIND_FILE, error);
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
                                  TP_SOURCE_KIND_FILE, error);
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
                                  proto->paths + offset, count,
                                  TP_SOURCE_KIND_FILE, error);
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
        status = create_one_atlas(project, rng, *atlas_seq, sv_paths, n,
                                  TP_SOURCE_KIND_FILE, error);
        if (status == TP_STATUS_OK) {
            *atlas_seq += 1;
        }
    }
    return status;
}

/* Group 6: one atlas whose single source is a FOLDER (recursively scanned at pack
 * time), pointing at the committed tiny-dungeon tree. The folder-enumeration path
 * (tp_scan_dir + the descriptor loop) is exercised only when this atlas is packed
 * with real bytes (--bench-perf / --pack-hash-probe, the LFS perf leg). The byte-
 * stable contract never scans it: validate_committed_shape only checks the source
 * resolves to a real directory, so the check stays LFS-independent. */
static tp_status build_folder_group(tp_project *project, const tp_rng *rng,
                                    int *atlas_seq, tp_error *error) {
    char *const paths[1] = {(char *)BENCH_FOLDER_SOURCE_PATH};
    tp_status status = create_one_atlas(project, rng, *atlas_seq, paths, 1,
                                        TP_SOURCE_KIND_FOLDER, error);
    if (status == TP_STATUS_OK) {
        *atlas_seq += 1;
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
        if (status == TP_STATUS_OK) {
            status = build_folder_group(project, &rng, &atlas_seq, error);
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
        /* Group 6 (folder) lives at the final index range. Its lone source is a
         * FOLDER, checked LFS-independently: resolve + directory existence, never a
         * scan of the *.png inside (which may be unmaterialized LFS pointers). */
        const bool is_folder_atlas =
            atlas && atlas_index >= BENCH_FOLDER_ATLAS_START &&
            atlas_index < BENCH_FOLDER_ATLAS_START + BENCH_FOLDER_ATLASES;
        for (int source_index = 0; valid && source_index < atlas->source_count;
             ++source_index) {
            const tp_snapshot_source *source = tp_session_snapshot_source_at(
                snapshot, atlas->id, source_index);
            char resolved[TP_IDENTITY_PATH_MAX];
            const tp_snapshot_source_kind expected_kind =
                is_folder_atlas ? TP_SNAPSHOT_SOURCE_FOLDER
                                : TP_SNAPSHOT_SOURCE_FILE;
            valid = source && source->kind == expected_kind &&
                    tp_session_snapshot_resolve_path(
                        snapshot, atlas->id, source->id, resolved,
                        sizeof resolved, &error) == TP_STATUS_OK &&
                    /* A folder resolves to a directory that exists in any checkout;
                     * a file source resolves to a (possibly LFS-pointer) file that
                     * still exists on disk. Both stay green without `git lfs pull`. */
                    (is_folder_atlas ? tp_scan_is_dir(resolved)
                                     : file_exists(resolved)) &&
                    (!is_proto_atlas ||
                     strstr(source->path, "prototype-textures/") != NULL) &&
                    (!is_folder_atlas ||
                     strstr(source->path, "tiny-dungeon") != NULL);
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

/* ---- F17: on-disk asset <-> manifest content verification ------------------------
 * The structural contract above only checks that each source path RESOLVES and the
 * file EXISTS -- so a source PNG could be swapped for different bytes without touching
 * the manifest and the contract would stay green. This pass reads each asset the
 * manifest names and verifies its byte size, SHA-256, and PNG dimensions against the
 * manifest's columns, failing on any mismatch. It stays LFS-independent: an
 * unmaterialized git-lfs POINTER (the state in CI legs without `git lfs pull`) is
 * detected and skipped -- but never silently: the count is logged. A compact SHA-256
 * (FIPS 180-4), self-checked against a known vector before use. */

static uint32_t sha256_rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32U - n));
}

static void sha256_compress(uint32_t state[8], const uint8_t *p) {
    static const uint32_t K[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
        0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
        0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
        0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = sha256_rotr(w[i - 15], 7U) ^
                            sha256_rotr(w[i - 15], 18U) ^ (w[i - 15] >> 3);
        const uint32_t s1 = sha256_rotr(w[i - 2], 17U) ^
                            sha256_rotr(w[i - 2], 19U) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t big1 =
            sha256_rotr(e, 6U) ^ sha256_rotr(e, 11U) ^ sha256_rotr(e, 25U);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + big1 + ch + K[i] + w[i];
        const uint32_t big0 =
            sha256_rotr(a, 2U) ^ sha256_rotr(a, 13U) ^ sha256_rotr(a, 22U);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = big0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint32_t state[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                         0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    size_t i = 0;
    for (; i + 64U <= len; i += 64U) {
        sha256_compress(state, data + i);
    }
    uint8_t tail[128];
    const size_t rem = len - i; /* 0..63 */
    memcpy(tail, data + i, rem);
    tail[rem] = (uint8_t)0x80;
    const size_t tail_len = (rem < 56U) ? 64U : 128U;
    for (size_t j = rem + 1U; j < tail_len - 8U; ++j) {
        tail[j] = 0U;
    }
    const uint64_t bits = (uint64_t)len * 8U;
    for (unsigned b = 0; b < 8U; ++b) {
        tail[tail_len - 8U + b] = (uint8_t)(bits >> (56U - 8U * b));
    }
    for (size_t off = 0; off < tail_len; off += 64U) {
        sha256_compress(state, tail + off);
    }
    for (int j = 0; j < 8; ++j) {
        out[j * 4] = (uint8_t)(state[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(state[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(state[j] >> 8);
        out[j * 4 + 3] = (uint8_t)state[j];
    }
}

static void sha256_hex(const uint8_t digest[32], char out[65]) {
    static const char hexchars[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2] = hexchars[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hexchars[digest[i] & 0x0F];
    }
    out[64] = '\0';
}

static bool sha256_selftest(void) {
    uint8_t digest[32];
    char hex[65];
    sha256_compute((const uint8_t *)"abc", 3U, digest);
    sha256_hex(digest, hex);
    return strcmp(
               hex,
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ==
           0;
}

/* An unmaterialized Git LFS pointer file starts with this spec line. */
static bool looks_like_lfs_pointer(const uint8_t *bytes, size_t len) {
    static const char sig[] = "version https://git-lfs";
    const size_t n = sizeof sig - 1U;
    return len >= n && memcmp(bytes, sig, n) == 0;
}

/* Reads a PNG's IHDR width/height. False for anything not a PNG with an IHDR. */
static bool png_dimensions(const uint8_t *b, size_t len, int *width, int *height) {
    static const uint8_t sig[8] = {0x89U, 0x50U, 0x4EU, 0x47U,
                                   0x0DU, 0x0AU, 0x1AU, 0x0AU};
    if (len < 24U || memcmp(b, sig, sizeof sig) != 0 ||
        memcmp(b + 12, "IHDR", 4) != 0) {
        return false;
    }
    *width = (int)(((uint32_t)b[16] << 24) | ((uint32_t)b[17] << 16) |
                   ((uint32_t)b[18] << 8) | (uint32_t)b[19]);
    *height = (int)(((uint32_t)b[20] << 24) | ((uint32_t)b[21] << 16) |
                    ((uint32_t)b[22] << 8) | (uint32_t)b[23]);
    return true;
}

/* Directory component of `path` (without the trailing separator); "." if none. */
static bool path_dirname(const char *path, char *out, size_t cap) {
    size_t len = strlen(path);
    while (len > 0U && path[len - 1U] != '/' && path[len - 1U] != '\\') {
        --len;
    }
    if (len == 0U) {
        if (cap < 2U) {
            return false;
        }
        out[0] = '.';
        out[1] = '\0';
        return true;
    }
    const size_t dlen = len - 1U; /* drop the separator */
    if (dlen + 1U > cap) {
        return false;
    }
    memcpy(out, path, dlen);
    out[dlen] = '\0';
    return true;
}

/* Splits `line` into exactly 5 tab-separated fields; false if not exactly 5. */
static bool split_tsv5(const char *line, size_t len, const char *fields[5],
                       size_t lens[5]) {
    size_t start = 0U;
    int idx = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || line[i] == '\t') {
            if (idx >= 5) {
                return false;
            }
            fields[idx] = line + start;
            lens[idx] = i - start;
            ++idx;
            start = i + 1U;
        }
    }
    return idx == 5;
}

/* Verifies every manifest-named asset's on-disk bytes against the manifest columns.
 * Assets live at <dirname(manifest)>/kenney/<relpath> (BENCH_ASSET_PREFIX resolves the
 * same tree from the project side). Materialized assets are fully checked (size +
 * SHA-256 + PNG dimensions); LFS pointer files are skipped and counted (logged). Any
 * present-but-mismatching asset fails. */
static bool verify_manifest_assets(const char *manifest_path, int *out_verified,
                                   int *out_skipped) {
    *out_verified = 0;
    *out_skipped = 0;
    if (!sha256_selftest()) {
        (void)fprintf(stderr,
                      "bench asset verify: internal SHA-256 self-test failed\n");
        return false;
    }
    char base_dir[TP_IDENTITY_PATH_MAX];
    if (!path_dirname(manifest_path, base_dir, sizeof base_dir)) {
        (void)fprintf(stderr, "bench asset verify: manifest dir path too long\n");
        return false;
    }

    uint8_t *buffer = NULL;
    size_t length = 0U;
    if (!read_file(manifest_path, &buffer, &length)) {
        (void)fprintf(stderr, "bench asset verify: manifest unreadable: %s\n",
                      manifest_path);
        return false;
    }

    bool ok = true;
    int verified = 0;
    int skipped = 0;
    int rows = 0;
    size_t pos = 0U;
    bool first_line = true;
    while (pos < length && ok) {
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
        const char *line = (const char *)buffer + line_start;
        if (first_line) {
            first_line = false;
            continue; /* '#' header, already validated by parse_manifest */
        }
        if (line_len == 0U) {
            continue;
        }

        const char *field[5];
        size_t flen[5];
        int width = 0;
        int height = 0;
        int bytes = 0;
        char relpath[TP_IDENTITY_PATH_MAX];
        if (!split_tsv5(line, line_len, field, flen) || flen[0] == 0U ||
            flen[0] >= sizeof relpath || flen[4] != 64U ||
            !parse_uint_field(field[1], flen[1], &width) ||
            !parse_uint_field(field[2], flen[2], &height) ||
            !parse_uint_field(field[3], flen[3], &bytes)) {
            (void)fprintf(stderr,
                          "bench asset verify: malformed manifest row #%d\n",
                          rows + 1);
            ok = false;
            break;
        }
        memcpy(relpath, field[0], flen[0]);
        relpath[flen[0]] = '\0';
        char sha_expected[65];
        memcpy(sha_expected, field[4], 64U);
        sha_expected[64] = '\0';
        ++rows;

        char asset_path[TP_IDENTITY_PATH_MAX];
        const int written = snprintf(asset_path, sizeof asset_path, "%s/kenney/%s",
                                     base_dir, relpath);
        if (written < 0 || (size_t)written >= sizeof asset_path) {
            (void)fprintf(stderr,
                          "bench asset verify: asset path too long: %s\n", relpath);
            ok = false;
            break;
        }

        uint8_t *content = NULL;
        size_t clen = 0U;
        if (!read_file(asset_path, &content, &clen)) {
            (void)fprintf(stderr,
                          "bench asset verify: missing/unreadable asset: %s\n",
                          asset_path);
            ok = false;
            break;
        }
        if (looks_like_lfs_pointer(content, clen)) {
            ++skipped; /* unmaterialized LFS pointer: cannot verify content here */
            free(content);
            continue;
        }
        if (clen != (size_t)bytes) {
            (void)fprintf(stderr,
                          "bench asset verify: size mismatch for %s: manifest=%d "
                          "on_disk=%zu\n",
                          relpath, bytes, clen);
            ok = false;
        }
        if (ok) {
            uint8_t digest[32];
            char hex[65];
            sha256_compute(content, clen, digest);
            sha256_hex(digest, hex);
            if (strcmp(hex, sha_expected) != 0) {
                (void)fprintf(stderr,
                              "bench asset verify: sha256 mismatch for %s:\n"
                              "  manifest=%s\n  on_disk =%s\n",
                              relpath, sha_expected, hex);
                ok = false;
            }
        }
        if (ok) {
            int dw = 0;
            int dh = 0;
            if (png_dimensions(content, clen, &dw, &dh) &&
                (dw != width || dh != height)) {
                (void)fprintf(stderr,
                              "bench asset verify: dimension mismatch for %s: "
                              "manifest=%dx%d on_disk=%dx%d\n",
                              relpath, width, height, dw, dh);
                ok = false;
            }
        }
        free(content);
        if (ok) {
            ++verified;
        }
    }
    free(buffer);
    if (!ok) {
        return false;
    }
    /* No silent skip: always report how many assets could not be content-verified
     * because they are unmaterialized LFS pointers (the CI legs without `git lfs
     * pull`). verified==0 there is expected and NOT a failure. */
    if (skipped > 0) {
        (void)fprintf(stderr,
                      "bench asset verify: %d/%d assets are unmaterialized git-lfs "
                      "pointers (content check skipped; run `git lfs pull` to "
                      "verify them)\n",
                      skipped, rows);
    }
    *out_verified = verified;
    *out_skipped = skipped;
    return true;
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
    /* F17: content-verify each manifest-named asset (size + SHA-256 + PNG dims) so a
     * swapped source PNG cannot pass with a stale manifest. LFS-pointer assets are
     * skipped-with-log, keeping the contract green in CI legs without `git lfs pull`. */
    int assets_verified = 0;
    int assets_skipped = 0;
    if (!verify_manifest_assets(manifest_path, &assets_verified, &assets_skipped)) {
        (void)fprintf(stderr,
                      "bench fixture asset content check failed: manifest=%s\n",
                      manifest_path);
        return 1;
    }
    (void)printf(
        "tp_bench_project_contract: OK atlases=%d memberships=%d bytes=%zu "
        "assets_verified=%d assets_skipped_lfs=%d\n",
        BENCH_EXPECTED_ATLAS_COUNT, BENCH_EXPECTED_MEMBERSHIP_COUNT,
        structural_bytes, assets_verified, assets_skipped);
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
