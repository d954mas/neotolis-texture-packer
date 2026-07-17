/* Phase 3 project model + .ntpacker_project load/save (ROADMAP Phase 3).
 *
 * Covers: programmatic round-trip (deep-equal + byte-identical re-save),
 * sparse storage (defaults absent from the text), determinism (same model ->
 * identical bytes), version refusal + malformed + forward-compat, absolute->
 * relative path handling with '/' separators, resolve_path, and the
 * atlas->tp_pack_settings bridge. A scratch dir is passed as argv[1].
 *
 * Unity constraint: no raw float equality asserts -- fabsf epsilon only. */

#define _CRT_SECURE_NO_WARNINGS

#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TP_TEST_MKDIR(p) _mkdir(p)
#define TP_TEST_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TP_TEST_MKDIR(p) mkdir((p), 0777)
#define TP_TEST_RMDIR(p) rmdir(p)
#endif

#include "tp_core/tp_export.h" /* TP_EXPORTER_ID_JSON_NEOTOLIS */
#include "tp_core/tp_id.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h"
#include "unity.h"
#include "../src/tp_project_internal.h"
#include "tp_project_mutation_internal.h"

void setUp(void) {
    tp_project__test_serialization_stats_reset();
}
void tearDown(void) {}

static const char *g_dir;

/* A writable session assigns final IDs before saving (master spec §5.5). Tests
 * that build a project programmatically and save it promote first, so the saved
 * file carries non-nil structural IDs and round-trips. */
static void promote(tp_project *p) {
    tp_rng rng = tp_rng_os();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, NULL));
}

static size_t append_json(char *buffer, size_t capacity, size_t used,
                          const char *format, ...) {
    TEST_ASSERT_TRUE(used < capacity);
    va_list args;
    va_start(args, format);
    /* format is this helper's own variadic parameter, not attacker input -- suppress
     * locally instead of dropping the target's warning flags. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    const int written =
        vsnprintf(buffer + used, capacity - used, format, args);
#pragma clang diagnostic pop
    va_end(args);
    TEST_ASSERT_TRUE(written >= 0);
    TEST_ASSERT_TRUE((size_t)written < capacity - used);
    return used + (size_t)written;
}

void test_project_load_lookup_work_is_linear(void) {
    enum { RECORDS = 512 };
    const size_t capacity = (size_t)RECORDS * 160U + 256U;
    char *json = (char *)malloc(capacity);
    TEST_ASSERT_NOT_NULL(json);
    size_t used = append_json(
        json, capacity, 0U,
        "{\"version\":4,\"atlases\":[{\"id\":\"atlas_"
        "ffffffffffffffffffffffffffffffff\",\"name\":\"a\","
        "\"sources\":[", 0U);
    for (unsigned i = 0U; i < (unsigned)RECORDS; i++) {
        used = append_json(
            json, capacity, used,
            "%s{\"id\":\"source_%032x\",\"path\":\"path/",
            i == 0U ? "" : ",", i + 1U);
        used = append_json(json, capacity, used, "%u\"}", i);
    }
    used = append_json(json, capacity, used, "],\"sprites\":[", 0U);
    for (unsigned i = 0U; i < (unsigned)RECORDS; i++) {
        used = append_json(json, capacity, used,
                           "%s{\"name\":\"sprite-",
                           i == 0U ? "" : ",");
        used = append_json(
            json, capacity, used,
            "%u\",\"origin\":[0.25,0.5]}", i);
    }
    used = append_json(json, capacity, used, "]}]}", 0U);

    tp_project__test_load_lookup_work_reset();
    tp_project__test_id_validation_work_reset();
    tp_project__test_load_resources_reset();
    tp_project *project = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_load_buffer(json, used, &project, &error), error.msg);
    const tp_project_load_lookup_work work =
        tp_project__test_load_lookup_work_take();
    const size_t id_probes = tp_project__test_id_validation_work_take();
    const tp_project_load_resources resources =
        tp_project__test_load_resources_take();
    TEST_ASSERT_LESS_OR_EQUAL_size_t(
        (size_t)RECORDS * 8U, work.source_path_comparisons);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(
        (size_t)RECORDS * 8U, work.pending_name_comparisons);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(
        ((size_t)RECORDS + 1U) * 8U, id_probes);
    TEST_ASSERT_GREATER_THAN_size_t(0U, resources.source_index_peak_bytes);
    TEST_ASSERT_GREATER_THAN_size_t(0U, resources.pending_index_peak_bytes);
    TEST_ASSERT_GREATER_THAN_size_t(0U, resources.id_refs_bytes);
    TEST_ASSERT_GREATER_THAN_size_t(0U, resources.id_index_bytes);
    TEST_ASSERT_EQUAL_size_t(0U, resources.legacy_peak_bytes);
    TEST_ASSERT_EQUAL_INT(RECORDS, project->atlases[0].source_count);
    TEST_ASSERT_EQUAL_INT(RECORDS, project->atlases[0].sprite_count);

    tp_project_destroy(project);
    free(json);
}

void test_legacy_load_reports_bounded_synthesis_storage(void) {
    static const char json[] =
        "{\"version\":1,\"atlases\":[{\"name\":\"a\","
        "\"sources\":[\"one\",\"two\"]}]}";
    tp_project__test_load_resources_reset();
    tp_project *project = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_load_buffer(json, sizeof json - 1U, &project, &error),
        error.msg);
    const tp_project_load_resources resources =
        tp_project__test_load_resources_take();
    TEST_ASSERT_GREATER_THAN_size_t(0U, resources.source_index_peak_bytes);
    TEST_ASSERT_EQUAL_size_t(0U, resources.pending_index_peak_bytes);
    TEST_ASSERT_GREATER_THAN_size_t(0U, resources.id_refs_bytes);
    TEST_ASSERT_GREATER_THAN_size_t(0U, resources.id_index_bytes);
    TEST_ASSERT_GREATER_THAN_size_t(0U, resources.legacy_peak_bytes);
    tp_project_destroy(project);
}

static uint64_t test_load_hash(const char *key) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; *key; key++) {
        hash ^= (uint64_t)(unsigned char)*key;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void test_project_load_adversarial_hash_cluster_is_bounded(void) {
    enum { RECORDS = 65, TABLE_MASK = 255 };
    char keys[RECORDS][32];
    unsigned found = 0U;
    for (unsigned candidate = 0U; found < (unsigned)RECORDS; candidate++) {
        char key[32];
        const int written = snprintf(key, sizeof key, "collision-%u", candidate);
        TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof key);
        if ((test_load_hash(key) & (uint64_t)TABLE_MASK) == 0U) {
            (void)snprintf(keys[found], sizeof keys[found], "%s", key);
            found++;
        }
    }

    char json[8192];
    size_t used = append_json(
        json, sizeof json, 0U,
        "{\"version\":4,\"atlases\":[{\"id\":\"atlas_"
        "ffffffffffffffffffffffffffffffff\",\"name\":\"a\","
        "\"sprites\":[");
    for (unsigned i = 0U; i < (unsigned)RECORDS; i++) {
        used = append_json(json, sizeof json, used,
                           "%s{\"name\":\"%s\"}", i == 0U ? "" : ",",
                           keys[i]);
    }
    used = append_json(json, sizeof json, used, "]}]}");

    tp_project__test_load_lookup_work_reset();
    tp_project *project = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_project_load_buffer(json, used, &project, &error));
    const tp_project_load_lookup_work work =
        tp_project__test_load_lookup_work_take();
    TEST_ASSERT_NULL(project);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(
        (size_t)RECORDS * 64U, work.pending_name_comparisons);
}

void test_legacy_load_rejects_overlong_source_paths(void) {
    char path[4097];
    memset(path, 'a', sizeof path - 1U);
    path[sizeof path - 1U] = '\0';

    char json[4600];
    for (unsigned version = 1U; version <= 2U; version++) {
        size_t used = append_json(
            json, sizeof json, 0U,
            "{\"version\":%u,\"atlases\":[{\"name\":\"a\"", version);
        if (version == 2U) {
            used = append_json(
                json, sizeof json, used,
                ",\"id\":\"atlas_ffffffffffffffffffffffffffffffff\"");
        }
        used = append_json(json, sizeof json, used,
                           ",\"sources\":[\"%s\"]}]}", path);
        tp_project *project = NULL;
        tp_error error = {{0}};
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OUT_OF_BOUNDS,
            tp_project_load_buffer(json, used, &project, &error));
        TEST_ASSERT_NULL(project);
    }
}

void test_v3_overlong_source_records_bypass_dedupe_index(void) {
    enum { RECORDS = 65, PATH_BYTES = 4096 };
    char path[PATH_BYTES + 1];
    memset(path, 'a', PATH_BYTES);
    path[PATH_BYTES] = '\0';
    const size_t capacity =
        (size_t)RECORDS * ((size_t)PATH_BYTES + 96U) + 256U;
    char *json = (char *)malloc(capacity);
    TEST_ASSERT_NOT_NULL(json);
    size_t used = append_json(
        json, capacity, 0U,
        "{\"version\":3,\"atlases\":[{\"id\":\"atlas_"
        "ffffffffffffffffffffffffffffffff\",\"name\":\"a\","
        "\"sources\":[");
    for (unsigned i = 0U; i < (unsigned)RECORDS; i++) {
        used = append_json(
            json, capacity, used,
            "%s{\"id\":\"source_%032x\",\"path\":\"%s\"}",
            i == 0U ? "" : ",", i + 1U, path);
    }
    used = append_json(json, capacity, used, "]}]}");

    tp_project *project = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_load_buffer(json, used, &project, &error), error.msg);
    TEST_ASSERT_EQUAL_INT(RECORDS, project->atlases[0].source_count);
    TEST_ASSERT_EQUAL_STRING(path, project->atlases[0].sources[0].path);
    TEST_ASSERT_EQUAL_STRING(
        path, project->atlases[0].sources[RECORDS - 1].path);
    tp_project_destroy(project);
    free(json);
}

void test_json_admission_exact_limits_and_escaped_punctuation(void) {
    static const char json[] =
        "{\"version\":4,\"x\":\"[,{\\\"}]\"}";
    const tp_project_json_limits limits = {
        sizeof json - 1U, 5U, 2U, 1U,
    };
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project__test_json_admit(
                              json, sizeof json - 1U, &limits, &error));
}

void test_json_admission_rejects_nodes_entries_and_depth(void) {
    static const char node_bomb[] =
        "{\"version\":4,\"x\":[0,0,0]}";
    static const char depth_bomb[] =
        "{\"version\":4,\"x\":[[0]]}";
    tp_error error = {{0}};

    const tp_project_json_limits node_limits = {
        sizeof node_bomb - 1U, 7U, 3U, 2U,
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_project__test_json_admit(
                              node_bomb, sizeof node_bomb - 1U,
                              &node_limits, &error));

    const tp_project_json_limits entry_limits = {
        sizeof node_bomb - 1U, 8U, 2U, 2U,
    };
    memset(&error, 0, sizeof error);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_project__test_json_admit(
                              node_bomb, sizeof node_bomb - 1U,
                              &entry_limits, &error));

    const tp_project_json_limits depth_limits = {
        sizeof depth_bomb - 1U, 8U, 3U, 2U,
    };
    memset(&error, 0, sizeof error);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_project__test_json_admit(
                              depth_bomb, sizeof depth_bomb - 1U,
                              &depth_limits, &error));
}

void test_json_admission_rejects_byte_limit_before_buffer_access(void) {
    const char byte = '{';
    tp_project *project = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_project_load_buffer(&byte,
                               (size_t)TP_IDENTITY_FILE_MAX_BYTES + 1U,
                               &project, &error));
    TEST_ASSERT_NULL(project);
}

void test_save_buffers_obey_project_json_admission_limits(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    char *bytes = NULL;
    size_t length = 0U;
    tp_error error = {{0}};
    const tp_project_json_limits limits = {
        (size_t)TP_IDENTITY_FILE_MAX_BYTES, SIZE_MAX, SIZE_MAX, 1U,
    };

    /* Every canonical project has a root object containing an atlases array. */
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_project__test_save_buffer_with_json_limits(
            project, false, &limits, &bytes, &length, &error));
    TEST_ASSERT_NULL(bytes);
    TEST_ASSERT_EQUAL_size_t(0U, length);

    memset(&error, 0, sizeof error);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_project__test_save_buffer_with_json_limits(
            project, true, &limits, &bytes, &length, &error));
    TEST_ASSERT_NULL(bytes);
    TEST_ASSERT_EQUAL_size_t(0U, length);
    tp_project_destroy(project);
}

void test_save_buffer_stops_at_byte_limit_before_growth(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    const tp_project_json_limits limits = {1U, SIZE_MAX, SIZE_MAX, 64U};
    char *bytes = NULL;
    size_t length = 0U;
    tp_error error = {{0}};

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_project__test_save_buffer_with_json_limits(
            project, false, &limits, &bytes, &length, &error));
    TEST_ASSERT_NULL(bytes);
    TEST_ASSERT_EQUAL_size_t(0U, length);
    TEST_ASSERT_EQUAL_size_t(1U, tp_project__test_serializer_allocations());
    TEST_ASSERT_EQUAL_size_t(2U,
                             tp_project__test_serializer_peak_capacity());
    tp_project_destroy(project);
}

#define EPS 1e-6F

static void join(char *out, size_t cap, const char *name) {
    (void)snprintf(out, cap, "%s/%s", g_dir, name);
}

/* Reads a whole file into a malloc'd NUL-terminated buffer; *len excludes NUL. */
static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    TEST_ASSERT_TRUE(n >= 0);
    char *buf = (char *)malloc((size_t)n + 1U);
    TEST_ASSERT_NOT_NULL(buf);
    size_t got = fread(buf, 1U, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len) {
        *len = got;
    }
    return buf;
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(text, 1U, strlen(text), f);
    fclose(f);
}

static void feq(float a, float b) { TEST_ASSERT_TRUE(fabsf(a - b) < EPS); }

static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1U;
    char *c = (char *)malloc(n);
    TEST_ASSERT_NOT_NULL(c);
    memcpy(c, s, n);
    return c;
}

/* --- a rich project built programmatically --- */
static tp_project *build_rich(void) {
    tp_project *p = tp_project_create(); /* one default atlas "atlas1" */
    TEST_ASSERT_NOT_NULL(p);

    /* atlas 0: "hero" -- default knobs, sources + one full sprite override + anim + targets */
    tp_project_atlas *hero = tp_project_get_atlas(p, 0);
    TEST_ASSERT_NOT_NULL(hero);
    free(hero->name);
    hero->name = dupstr("hero");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(hero, "art/hero"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(hero, "art/enemies"));

    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_pending_sprite(hero, "hero/walk_01", &sp));
    sp->origin_x = 0.25F;
    sp->origin_y = 0.75F;
    sp->slice9_lrtb[0] = 4;
    sp->slice9_lrtb[1] = 4;
    sp->slice9_lrtb[2] = 8;
    sp->slice9_lrtb[3] = 8;
    sp->rename = dupstr("player_walk_01"); /* export-name override */
    /* per-sprite packing overrides (exercise the sparse override round-trip) */
    sp->ov_shape = 0; /* RECT */
    sp->ov_allow_rotate = 0;
    sp->ov_max_vertices = 6;
    sp->ov_margin = 3;
    sp->ov_extrude = 5;

    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(hero, "walk", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero/walk_01"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero/walk_02"));
    an->fps = 24.0F;
    an->playback = 1;
    an->flip_h = true;

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(hero, "json-neotolis", "out/hero.json", NULL));
    tp_project_target *t2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(hero, "defold", "out/hero.tpinfo", &t2));
    t2->enabled = false;

    /* atlas 1: "ui" -- non-default knobs */
    int ui_idx = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "ui", &ui_idx));
    tp_project_atlas *ui = tp_project_get_atlas(p, ui_idx);
    ui->max_size = 1024;
    ui->padding = 4;
    ui->margin = 1;
    ui->extrude = 2;
    ui->alpha_threshold = 200;
    ui->max_vertices = 8;
    ui->shape = 0;
    ui->allow_transform = false;
    ui->power_of_two = false;
    ui->pixels_per_unit = 2.0F;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(ui, "art/ui"));
    promote(p); /* assign final structural IDs before any save (writable session) */
    return p;
}

static void assert_atlas_equal(const tp_project_atlas *a, const tp_project_atlas *b) {
    TEST_ASSERT_EQUAL_STRING(a->name, b->name);
    TEST_ASSERT_TRUE(tp_id128_eq(a->id, b->id)); /* structural id survives save/reload */
    TEST_ASSERT_EQUAL_INT(a->max_size, b->max_size);
    TEST_ASSERT_EQUAL_INT(a->padding, b->padding);
    TEST_ASSERT_EQUAL_INT(a->margin, b->margin);
    TEST_ASSERT_EQUAL_INT(a->extrude, b->extrude);
    TEST_ASSERT_EQUAL_INT(a->alpha_threshold, b->alpha_threshold);
    TEST_ASSERT_EQUAL_INT(a->max_vertices, b->max_vertices);
    TEST_ASSERT_EQUAL_INT(a->shape, b->shape);
    TEST_ASSERT_EQUAL_INT(a->allow_transform ? 1 : 0, b->allow_transform ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(a->power_of_two ? 1 : 0, b->power_of_two ? 1 : 0);
    feq(a->pixels_per_unit, b->pixels_per_unit);

    TEST_ASSERT_EQUAL_INT(a->source_count, b->source_count);
    for (int i = 0; i < a->source_count; i++) {
        TEST_ASSERT_EQUAL_STRING(a->sources[i].path, b->sources[i].path);
        TEST_ASSERT_TRUE(tp_id128_eq(a->sources[i].id, b->sources[i].id)); /* source id survives save/reload */
        TEST_ASSERT_EQUAL_INT((int)a->sources[i].kind, (int)b->sources[i].kind);
    }
    TEST_ASSERT_EQUAL_INT(a->sprite_count, b->sprite_count);
    for (int i = 0; i < a->sprite_count; i++) {
        TEST_ASSERT_EQUAL_STRING(a->sprites[i].name, b->sprites[i].name);
        feq(a->sprites[i].origin_x, b->sprites[i].origin_x);
        feq(a->sprites[i].origin_y, b->sprites[i].origin_y);
        for (int k = 0; k < 4; k++) {
            TEST_ASSERT_EQUAL_UINT16(a->sprites[i].slice9_lrtb[k], b->sprites[i].slice9_lrtb[k]);
        }
        if (a->sprites[i].rename == NULL) {
            TEST_ASSERT_NULL(b->sprites[i].rename);
        } else {
            TEST_ASSERT_NOT_NULL(b->sprites[i].rename);
            TEST_ASSERT_EQUAL_STRING(a->sprites[i].rename, b->sprites[i].rename);
        }
        TEST_ASSERT_EQUAL_INT(a->sprites[i].ov_shape, b->sprites[i].ov_shape);
        TEST_ASSERT_EQUAL_INT(a->sprites[i].ov_allow_rotate, b->sprites[i].ov_allow_rotate);
        TEST_ASSERT_EQUAL_INT(a->sprites[i].ov_max_vertices, b->sprites[i].ov_max_vertices);
        TEST_ASSERT_EQUAL_INT(a->sprites[i].ov_margin, b->sprites[i].ov_margin);
        TEST_ASSERT_EQUAL_INT(a->sprites[i].ov_extrude, b->sprites[i].ov_extrude);
    }
    TEST_ASSERT_EQUAL_INT(a->animation_count, b->animation_count);
    for (int i = 0; i < a->animation_count; i++) {
        TEST_ASSERT_EQUAL_STRING(a->animations[i].name, b->animations[i].name);
        TEST_ASSERT_TRUE(tp_id128_eq(a->animations[i].id, b->animations[i].id));
        feq(a->animations[i].fps, b->animations[i].fps);
        TEST_ASSERT_EQUAL_INT(a->animations[i].playback, b->animations[i].playback);
        TEST_ASSERT_EQUAL_INT(a->animations[i].flip_h ? 1 : 0, b->animations[i].flip_h ? 1 : 0);
        TEST_ASSERT_EQUAL_INT(a->animations[i].flip_v ? 1 : 0, b->animations[i].flip_v ? 1 : 0);
        TEST_ASSERT_EQUAL_INT(a->animations[i].frame_count, b->animations[i].frame_count);
        for (int k = 0; k < a->animations[i].frame_count; k++) {
            TEST_ASSERT_EQUAL_STRING(a->animations[i].frames[k].name, b->animations[i].frames[k].name);
        }
    }
    TEST_ASSERT_EQUAL_INT(a->target_count, b->target_count);
    for (int i = 0; i < a->target_count; i++) {
        TEST_ASSERT_TRUE(tp_id128_eq(a->targets[i].id, b->targets[i].id));
        TEST_ASSERT_EQUAL_STRING(a->targets[i].exporter_id, b->targets[i].exporter_id);
        TEST_ASSERT_EQUAL_STRING(a->targets[i].out_path, b->targets[i].out_path);
        TEST_ASSERT_EQUAL_INT(a->targets[i].enabled ? 1 : 0, b->targets[i].enabled ? 1 : 0);
    }
}

static void assert_project_equal(const tp_project *a, const tp_project *b) {
    TEST_ASSERT_EQUAL_INT(a->schema_version, b->schema_version);
    TEST_ASSERT_EQUAL_INT(a->atlas_count, b->atlas_count);
    for (int i = 0; i < a->atlas_count; i++) {
        assert_atlas_equal(&a->atlases[i], &b->atlases[i]);
    }
}

/* 1. round-trip: create -> save -> load -> deep-equal -> save -> byte-identical */
void test_roundtrip_and_byte_identical(void) {
    char p1[512];
    char p2[512];
    join(p1, sizeof p1, "rt1.ntpacker_project");
    join(p2, sizeof p2, "rt2.ntpacker_project");

    tp_project *p = build_rich();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_save(p, p1, &err), err.msg);

    tp_project *loaded = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(p1, &loaded, &err), err.msg);
    TEST_ASSERT_NOT_NULL(loaded);
    assert_project_equal(p, loaded);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(loaded, p2, &err));

    size_t n1 = 0;
    size_t n2 = 0;
    char *b1 = read_all(p1, &n1);
    char *b2 = read_all(p2, &n2);
    TEST_ASSERT_EQUAL_size_t(n1, n2);
    TEST_ASSERT_EQUAL_INT(0, memcmp(b1, b2, n1));
    /* trailing newline + LF-only */
    TEST_ASSERT_TRUE(n1 > 0 && b1[n1 - 1] == '\n');
    TEST_ASSERT_NULL(strchr(b1, '\r'));

    free(b1);
    free(b2);
    tp_project_destroy(loaded);
    tp_project_destroy(p);
}

/* 2. sparse: default-valued fields never appear in the JSON text */
void test_sparse_defaults_absent(void) {
    char path[512];
    join(path, sizeof path, "sparse.ntpacker_project");

    tp_project *p = tp_project_create(); /* atlas "atlas1", all-default knobs */
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art"));

    tp_project_sprite *sp = NULL; /* origin only -> slice9 stays default */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_pending_sprite(a, "s1", &sp));
    sp->origin_x = 0.1F;

    tp_project_anim *an = NULL; /* default fps/playback/flips */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "idle", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "s1"));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "json-neotolis", "out.json", NULL));

    promote(p);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err));

    size_t n = 0;
    char *buf = read_all(path, &n);
    /* default knobs absent */
    TEST_ASSERT_NULL(strstr(buf, "padding"));
    TEST_ASSERT_NULL(strstr(buf, "margin"));
    TEST_ASSERT_NULL(strstr(buf, "max_size"));
    TEST_ASSERT_NULL(strstr(buf, "power_of_two"));
    TEST_ASSERT_NULL(strstr(buf, "allow_transform"));
    TEST_ASSERT_NULL(strstr(buf, "pixels_per_unit"));
    /* default sub-fields absent */
    TEST_ASSERT_NULL(strstr(buf, "slice9"));   /* sprite kept default slice9 */
    TEST_ASSERT_NULL(strstr(buf, "rename"));   /* sprite kept default (NULL) rename */
    TEST_ASSERT_NULL(strstr(buf, "\"fps\""));  /* anim kept default fps */
    TEST_ASSERT_NULL(strstr(buf, "playback")); /* anim kept default playback */
    TEST_ASSERT_NULL(strstr(buf, "flip_h"));
    TEST_ASSERT_NULL(strstr(buf, "enabled")); /* target enabled default true */
    /* present, non-default */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"version\": 4"));
    TEST_ASSERT_NULL(strstr(buf, "\"kind\"")); /* source kept the folder default -> kind omitted (sparse) */
    TEST_ASSERT_NOT_NULL(strstr(buf, "origin"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"frames\""));
    /* "version" must be the first key */
    TEST_ASSERT_NOT_NULL(strstr(buf, "{\n  \"version\":"));

    free(buf);
    tp_project_destroy(p);
}

/* 3. determinism: same model saved twice -> identical bytes */
void test_determinism(void) {
    char pa[512];
    char pb[512];
    join(pa, sizeof pa, "det_a.ntpacker_project");
    join(pb, sizeof pb, "det_b.ntpacker_project");

    tp_project *p = build_rich();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, pa, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, pb, &err));

    size_t na = 0;
    size_t nb = 0;
    char *ba = read_all(pa, &na);
    char *bb = read_all(pb, &nb);
    TEST_ASSERT_EQUAL_size_t(na, nb);
    TEST_ASSERT_EQUAL_INT(0, memcmp(ba, bb, na));
    free(ba);
    free(bb);
    tp_project_destroy(p);
}

/* 4a. newer schema version is refused (schema v4 is current -> use a future v5) */
void test_version_newer_refused(void) {
    char path[512];
    join(path, sizeof path, "v5.ntpacker_project");
    write_text(path, "{\n  \"version\": 5,\n  \"atlases\": []\n}\n");

    tp_project *loaded = NULL;
    tp_error err = {0};
    tp_status st = tp_project_load(path, &loaded, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_VERSION, st);
    TEST_ASSERT_NULL(loaded);
    TEST_ASSERT_TRUE(strlen(err.msg) > 0);
}

/* 4b. malformed JSON -> BAD_PROJECT */
void test_malformed_bad_project(void) {
    char path[512];
    join(path, sizeof path, "garbage.ntpacker_project");
    write_text(path, "{ this is not valid json ]");

    tp_project *loaded = NULL;
    tp_error err = {0};
    tp_status st = tp_project_load(path, &loaded, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT, st);
    TEST_ASSERT_NULL(loaded);
}

/* 4c. wrong-typed field -> BAD_PROJECT */
void test_wrong_type_bad_project(void) {
    char path[512];
    join(path, sizeof path, "wrongtype.ntpacker_project");
    write_text(path, "{\n  \"version\": 1,\n  \"atlases\": [ { \"name\": \"a\", \"padding\": \"oops\" } ]\n}\n");

    tp_project *loaded = NULL;
    tp_error err = {0};
    tp_status st = tp_project_load(path, &loaded, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT, st);
    TEST_ASSERT_NULL(loaded);
}

static void assert_project_json_rejected(const char *json) {
    tp_project *loaded = NULL;
    tp_error err = {{0}};
    const tp_status status =
        tp_project_load_buffer(json, strlen(json), &loaded, &err);
    const bool returned_project = loaded != NULL;
    tp_project_destroy(loaded);

    /* TP_STATUS_BAD_PROJECT is the public structured parse/invalid-project
     * status promised by tp_project.h. A rejection must not leak a partial
     * model and must carry actionable context for headless clients. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BAD_PROJECT, status, json);
    TEST_ASSERT_FALSE_MESSAGE(returned_project, json);
    TEST_ASSERT_TRUE_MESSAGE(err.msg[0] != '\0', json);
}

void test_loader_rejects_trailing_non_whitespace(void) {
    assert_project_json_rejected(
        "{\"version\":4,\"atlases\":[]} trailing");
}

void test_loader_rejects_non_finite_atlas_float(void) {
    assert_project_json_rejected(
        "{\"version\":1,\"atlases\":[{\"name\":\"a\","
        "\"pixels_per_unit\":1e999}]}");
}

void test_loader_rejects_non_finite_sprite_origin(void) {
    assert_project_json_rejected(
        "{\"version\":1,\"atlases\":[{\"name\":\"a\",\"sprites\":["
        "{\"name\":\"hero\",\"origin\":[1e999,0.5]}]}]}");
}

void test_loader_rejects_fractional_version(void) {
    assert_project_json_rejected(
        "{\"version\":1.5,\"atlases\":[]}");
}

void test_loader_rejects_fractional_padding(void) {
    assert_project_json_rejected(
        "{\"version\":1,\"atlases\":[{\"name\":\"a\",\"padding\":1.5}]}");
}

void test_loader_rejects_out_of_int_range_max_size(void) {
    assert_project_json_rejected(
        "{\"version\":1,\"atlases\":[{\"name\":\"a\","
        "\"max_size\":2147483648}]}");
}

void test_loader_rejects_out_of_int_range_padding(void) {
    assert_project_json_rejected(
        "{\"version\":1,\"atlases\":[{\"name\":\"a\","
        "\"padding\":2147483648}]}");
}

void test_loader_rejects_non_numeric_origin_items(void) {
    assert_project_json_rejected(
        "{\"version\":1,\"atlases\":[{\"name\":\"a\",\"sprites\":["
        "{\"name\":\"hero\",\"origin\":[\"left\",null]}]}]}");
}

void test_loader_rejects_non_numeric_slice9_items(void) {
    assert_project_json_rejected(
        "{\"version\":1,\"atlases\":[{\"name\":\"a\",\"sprites\":["
        "{\"name\":\"hero\",\"slice9\":[1,\"right\",3,null]}]}]}");
}

void test_loader_rejects_fractional_slice9_items(void) {
    assert_project_json_rejected(
        "{\"version\":1,\"atlases\":[{\"name\":\"a\",\"sprites\":["
        "{\"name\":\"hero\",\"slice9\":[1.5,2,3,4]}]}]}");
}

void test_loader_rejects_out_of_uint16_range_slice9_items(void) {
    assert_project_json_rejected(
        "{\"version\":1,\"atlases\":[{\"name\":\"a\",\"sprites\":["
        "{\"name\":\"hero\",\"slice9\":[65536,2,3,4]}]}]}");
}

/* 4d. unknown extra keys ignored (forward compat) */
void test_unknown_keys_ignored(void) {
    char path[512];
    join(path, sizeof path, "future.ntpacker_project");
    write_text(path,
               "{\n"
               "  \"version\": 1,\n"
               "  \"future_root\": 42,\n"
               "  \"atlases\": [ { \"name\": \"a\", \"future_atlas\": true, \"padding\": 3 } ]\n"
               "}\n");

    tp_project *loaded = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &loaded, &err), err.msg);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_EQUAL_INT(1, loaded->atlas_count);
    TEST_ASSERT_EQUAL_STRING("a", loaded->atlases[0].name);
    TEST_ASSERT_EQUAL_INT(3, loaded->atlases[0].padding);
    tp_project_destroy(loaded);
}

/* 5. absolute source path loads; save relativizes it; output uses '/' */
void test_absolute_path_relativized(void) {
    char norm_dir[512];
    (void)snprintf(norm_dir, sizeof norm_dir, "%s", g_dir);
    for (char *c = norm_dir; *c; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }

    /* an absolute source under the project dir -> relativizes to "art/hero" */
    char json[1600];
    (void)snprintf(json, sizeof json,
                   "{\n  \"version\": 1,\n  \"atlases\": [ { \"name\": \"a\", \"sources\": [ \"%s/art/hero\" ] } ]\n}\n",
                   norm_dir);

    char path[512];
    join(path, sizeof path, "abs.ntpacker_project");
    write_text(path, json);

    tp_project *loaded = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &loaded, &err), err.msg);
    TEST_ASSERT_NOT_NULL(loaded);
    /* Loaded model holds the absolute spelling; Save relativizes only its staged output. */
    TEST_ASSERT_EQUAL_INT(1, loaded->atlases[0].source_count);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(loaded, path, &err));

    size_t n = 0;
    char *buf = read_all(path, &n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"art/hero\"")); /* relativized */
    TEST_ASSERT_NULL(strchr(buf, '\\'));               /* '/' separators only */
    TEST_ASSERT_NULL(strstr(buf, norm_dir));           /* absolute prefix gone */
    free(buf);

    /* Serialization normalization is staged; Save does not rewrite the live
     * source record behind history/client references. */
    char expected_source[1024];
    (void)snprintf(expected_source, sizeof expected_source, "%s/art/hero",
                   norm_dir);
    TEST_ASSERT_EQUAL_STRING(expected_source,
                             loaded->atlases[0].sources[0].path);
    tp_project_destroy(loaded);
}

/* 5b. resolve_path joins relative onto project_dir; passes absolute through */
void test_resolve_path(void) {
    char path[512];
    join(path, sizeof path, "resolve.ntpacker_project");

    tp_project *p = tp_project_create();
    promote(p);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err)); /* sets project_dir */
    TEST_ASSERT_NOT_NULL(p->project_dir);

    char out[1024];
    char expect[1024];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_resolve_path(p, "art/hero.png", out, sizeof out));
    (void)snprintf(expect, sizeof expect, "%s/art/hero.png", p->project_dir);
    TEST_ASSERT_EQUAL_STRING(expect, out);

    /* absolute passes through (normalized to '/') */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_resolve_path(p, "C:/abs/x.png", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("C:/abs/x.png", out);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_project_resolve_source_path(p, NULL, out,
                                                         sizeof out));

    tp_project_destroy(p);
}

void test_save_as_preserves_relative_source_target(void) {
    char old_path[512];
    char new_path[512];
    join(old_path, sizeof old_path, "relative-old.ntpacker_project");
    char new_dir[512];
    (void)snprintf(new_dir, sizeof new_dir, "%s", g_dir);
    char *slash = strrchr(new_dir, '/');
    if (!slash) {
        slash = strrchr(new_dir, '\\');
    }
    TEST_ASSERT_NOT_NULL(slash);
    *slash = '\0';
    (void)snprintf(new_path, sizeof new_path,
                   "%s/relative-new.ntpacker_project", new_dir);

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source(&project->atlases[0], "art/hero"));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_target(&project->atlases[0],
                                    TP_EXPORTER_ID_JSON_NEOTOLIS,
                                    "out/atlas", NULL));
    promote(project);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(project, old_path, &err));
    char old_base[1024];
    (void)snprintf(old_base, sizeof old_base, "%s", project->project_dir);
    char resolved_before[1024];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_resolve_source_path(project, "art/hero", resolved_before,
                                       sizeof resolved_before));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(project, new_path, &err));
    TEST_ASSERT_EQUAL_STRING(new_dir, project->project_dir);
    TEST_ASSERT_EQUAL_STRING(old_base, project->source_base_dir);
    char resolved_after[1024];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_resolve_source_path(
            project, project->atlases[0].sources[0].path, resolved_after,
            sizeof resolved_after));
    TEST_ASSERT_EQUAL_STRING(resolved_before, resolved_after);
    char target_after[1024];
    char expected_target[1024];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_resolve_path(project, "out/atlas", target_after,
                                sizeof target_after));
    (void)snprintf(expected_target, sizeof expected_target, "%s/out/atlas",
                   new_dir);
    TEST_ASSERT_EQUAL_STRING(expected_target, target_after);

    tp_project *reloaded = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_load(new_path, &reloaded, &err));
    char reloaded_target[1024];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_resolve_source_path(
            reloaded, reloaded->atlases[0].sources[0].path, reloaded_target,
            sizeof reloaded_target));
    TEST_ASSERT_EQUAL_STRING(resolved_before, reloaded_target);

    tp_project_destroy(reloaded);
    tp_project_destroy(project);
    (void)remove(new_path);
    (void)remove(old_path);
}

/* 6. atlas -> tp_pack_settings mapping; defaults flow from one place */
void test_to_settings_mapping(void) {
    tp_project *p = build_rich();

    tp_pack_settings s;
    tp_error err = {0};
    /* atlas 1 "ui" has the non-default knobs */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_to_settings(p, 1, &s, &err));
    TEST_ASSERT_EQUAL_STRING("ui", s.atlas_name);
    TEST_ASSERT_EQUAL_INT(1024, s.max_size);
    TEST_ASSERT_EQUAL_INT(4, s.padding);
    TEST_ASSERT_EQUAL_INT(1, s.margin);
    TEST_ASSERT_EQUAL_INT(2, s.extrude);
    TEST_ASSERT_EQUAL_INT(200, s.alpha_threshold);
    TEST_ASSERT_EQUAL_INT(8, s.max_vertices);
    TEST_ASSERT_EQUAL_INT(0, s.shape);
    TEST_ASSERT_FALSE(s.allow_transform);
    TEST_ASSERT_FALSE(s.power_of_two);
    feq(s.pixels_per_unit, 2.0F);
    /* the bridge fills knobs only -- work_dir/sprites are the call site's job */
    TEST_ASSERT_NULL(s.work_dir);
    TEST_ASSERT_NULL(s.sprites);
    TEST_ASSERT_EQUAL_INT(0, s.sprite_count);

    /* all-default atlas 0 "hero" -> knobs equal the shared defaults */
    tp_pack_settings def;
    tp_pack_settings_defaults(&def);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_to_settings(p, 0, &s, &err));
    TEST_ASSERT_EQUAL_INT(def.max_size, s.max_size);
    TEST_ASSERT_EQUAL_INT(def.padding, s.padding);
    TEST_ASSERT_EQUAL_INT(def.shape, s.shape);
    TEST_ASSERT_EQUAL_INT(def.allow_transform ? 1 : 0, s.allow_transform ? 1 : 0);
    feq(s.pixels_per_unit, def.pixels_per_unit);

    /* out-of-range index -> error, not a crash */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_atlas_to_settings(p, 9, &s, &err));

    tp_project_destroy(p);
}

/* 7. mutation helpers: add/find/remove behave and keep the model consistent */
void test_mutation_helpers(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_pending_sprite(a, "dup", NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_pending_sprite(a, "dup", NULL)); /* idempotent */
    TEST_ASSERT_EQUAL_INT(1, a->sprite_count);
    TEST_ASSERT_NOT_NULL(tp_project_atlas_find_pending_sprite(a, "dup"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_remove_pending_sprite(a, "dup"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_atlas_remove_pending_sprite(a, "dup"));
    TEST_ASSERT_NULL(tp_project_atlas_find_pending_sprite(a, "dup"));

    int idx = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "extra", &idx));
    TEST_ASSERT_EQUAL_INT(2, p->atlas_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_remove_atlas(p, idx));
    TEST_ASSERT_EQUAL_INT(1, p->atlas_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_remove_atlas(p, 5));

    tp_project_destroy(p);
}

/* 7b. animation frame edit: add preserves order; remove shifts; move reorders with clamping. */
void test_anim_frame_edit(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);

    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "walk", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "f0"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "f1"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "f2"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "f3"));
    TEST_ASSERT_EQUAL_INT(4, an->frame_count);

    /* move first frame down by 2 -> f1,f2,f0,f3 */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_move_frame(an, 0, 2));
    TEST_ASSERT_EQUAL_STRING("f1", an->frames[0].name);
    TEST_ASSERT_EQUAL_STRING("f2", an->frames[1].name);
    TEST_ASSERT_EQUAL_STRING("f0", an->frames[2].name);
    TEST_ASSERT_EQUAL_STRING("f3", an->frames[3].name);

    /* move last frame up by 1 -> f1,f2,f3,f0 */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_move_frame(an, 3, -1));
    TEST_ASSERT_EQUAL_STRING("f3", an->frames[2].name);
    TEST_ASSERT_EQUAL_STRING("f0", an->frames[3].name);

    /* over-clamp: moving index 0 up stays put (no-op), moving down past the end clamps to last */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_move_frame(an, 0, -5));
    TEST_ASSERT_EQUAL_STRING("f1", an->frames[0].name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_move_frame(an, 0, 99));
    TEST_ASSERT_EQUAL_STRING("f1", an->frames[3].name); /* f1 rode to the end */

    /* remove the middle -> shifts the tail down */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_remove_frame(an, 1));
    TEST_ASSERT_EQUAL_INT(3, an->frame_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_anim_remove_frame(an, 9));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_anim_move_frame(an, 9, 1));

    tp_project_destroy(p);
}

/* 8. buffer save/load: save_buffer bytes == file bytes; load_buffer deep-equals a
 * from-file load (project_dir aside). Sources are relative, so file == buffer. */
void test_buffer_roundtrip(void) {
    char p1[512];
    join(p1, sizeof p1, "buf1.ntpacker_project");

    tp_project *p = build_rich();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_save(p, p1, &err), err.msg);

    size_t nf = 0;
    char *bf = read_all(p1, &nf);

    char *bb = NULL;
    size_t nb = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_save_buffer(p, &bb, &nb, &err), err.msg);
    TEST_ASSERT_NOT_NULL(bb);
    TEST_ASSERT_EQUAL_size_t(nf, nb);
    TEST_ASSERT_EQUAL_INT(0, memcmp(bf, bb, nf));

    tp_project *from_file = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(p1, &from_file, &err), err.msg);
    tp_project *from_buf = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load_buffer(bb, nb, &from_buf, &err), err.msg);
    TEST_ASSERT_NULL(from_buf->project_dir);  /* buffer load leaves it NULL */
    TEST_ASSERT_NOT_NULL(from_file->project_dir);
    assert_project_equal(from_file, from_buf);

    free(bf);
    free(bb);
    tp_project_destroy(from_file);
    tp_project_destroy(from_buf);
    tp_project_destroy(p);
}

void test_serialized_size_bounded_matches_writer_without_materialization(void) {
    tp_project *p = build_rich();
    promote(p);
    tp_error err = {0};
    size_t measured = 0U;

    tp_project__test_serialization_stats_reset();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_serialized_size_bounded(p, SIZE_MAX, &measured, &err));
    TEST_ASSERT_GREATER_THAN_size_t(0U, measured);
    const size_t expected = measured;
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_save_buffer_calls());
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_serializer_allocations());
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_load_buffer_calls());

    measured = 123U;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_project_serialized_size_bounded(p, 0U, &measured, &err));
    TEST_ASSERT_EQUAL_size_t(0U, measured);

    char *bytes = NULL;
    size_t written = 0U;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save_buffer(p, &bytes, &written, &err));
    TEST_ASSERT_EQUAL_size_t(expected, written);
    TEST_ASSERT_EQUAL_size_t(1U, tp_project__test_save_buffer_calls());
    TEST_ASSERT_GREATER_THAN_size_t(0U,
                                    tp_project__test_serializer_allocations());
    free(bytes);

    tp_project__test_serialization_stats_reset();
    measured = 123U;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_project_serialized_size_bounded(p, written - 1U, &measured, &err));
    TEST_ASSERT_EQUAL_size_t(0U, measured);
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_save_buffer_calls());
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_serializer_allocations());
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_load_buffer_calls());
    tp_project_destroy(p);
}

/* 9. add_source dedupe: an exact ('/'-normalized) duplicate is an OK no-op. */
void test_add_source_dedupe(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art/hero"));
    TEST_ASSERT_EQUAL_INT(1, a->source_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art/hero")); /* dup no-op */
    TEST_ASSERT_EQUAL_INT(1, a->source_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art\\hero")); /* normalizes equal */
    TEST_ASSERT_EQUAL_INT(1, a->source_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art/ui")); /* distinct -> appends */
    TEST_ASSERT_EQUAL_INT(2, a->source_count);
    tp_project_destroy(p);
}

/* 9b (F1-02). tagged source round-trip: a folder source (default kind, omitted) and
 * a file source (kind=file, written) survive save/reload with id + kind intact. */
void test_source_kind_roundtrip(void) {
    char path[512];
    join(path, sizeof path, "srckind.ntpacker_project");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art/folder"));            /* folder (default) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source_kind(a, "art/one.png", TP_SOURCE_KIND_FILE)); /* file */
    TEST_ASSERT_EQUAL_INT(2, a->source_count);
    promote(p); /* assign real ids to the fresh sources */
    TEST_ASSERT_FALSE(tp_id128_is_nil(a->sources[0].id));
    TEST_ASSERT_FALSE(tp_id128_is_nil(a->sources[1].id));

    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err));

    /* on disk: the file source writes "kind": "file"; the folder source omits kind. */
    size_t n = 0;
    char *buf = read_all(path, &n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\": \"file\""));
    TEST_ASSERT_NULL(strstr(buf, "\"kind\": \"folder\"")); /* folder default is sparse */
    free(buf);

    tp_project *loaded = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &loaded, &err), err.msg);
    tp_project_atlas *la = tp_project_get_atlas(loaded, 0);
    TEST_ASSERT_EQUAL_INT(2, la->source_count);
    TEST_ASSERT_EQUAL_INT((int)TP_SOURCE_KIND_FOLDER, (int)la->sources[0].kind);
    TEST_ASSERT_EQUAL_INT((int)TP_SOURCE_KIND_FILE, (int)la->sources[1].kind);
    TEST_ASSERT_TRUE(tp_id128_eq(a->sources[0].id, la->sources[0].id)); /* id persisted */
    TEST_ASSERT_TRUE(tp_id128_eq(a->sources[1].id, la->sources[1].id));

    tp_project_destroy(loaded);
    tp_project_destroy(p);
}

/* 9c (F1-02 Fix A). A hand-written/corrupt v3 file that lists the same path twice
 * (two source objects with distinct valid ids) loads with the duplicate collapsed
 * to the FIRST object -- else `pack` (which does NOT run validate) would scan the
 * folder twice and double every sprite. The later object's id is dropped. */
void test_v3_source_dup_collapse(void) {
    /* 32-hex ids written as 4x8 concatenated literals so the length is self-evident. */
    static const char v3_dup[] =
        "{\n"
        "  \"version\": 3,\n"
        "  \"atlases\": [\n"
        "    {\n"
        "      \"name\": \"a1\",\n"
        "      \"id\": \"atlas_"
        "00000000"
        "00000000"
        "00000000"
        "00000abc"
        "\",\n"
        "      \"sources\": [\n"
        "        { \"id\": \"source_"
        "00000000"
        "00000000"
        "00000000"
        "00000001"
        "\", \"path\": \"dup/path\" },\n"
        "        { \"id\": \"source_"
        "00000000"
        "00000000"
        "00000000"
        "00000002"
        "\", \"path\": \"dup\\\\path\" }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n";
    tp_project *p = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load_buffer(v3_dup, sizeof v3_dup - 1U, &p, &err), err.msg);
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(1, a->source_count); /* duplicate path collapsed */
    TEST_ASSERT_EQUAL_STRING("dup/path", a->sources[0].path);

    /* The FIRST object survives; the later object's id is dropped. */
    tp_id_kind k = TP_ID_KIND_INVALID;
    tp_id128 id1;
    tp_id128 id2;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_id_parse("source_"
                                      "00000000"
                                      "00000000"
                                      "00000000"
                                      "00000001",
                                      &k, &id1, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_id_parse("source_"
                                      "00000000"
                                      "00000000"
                                      "00000000"
                                      "00000002",
                                      &k, &id2, NULL));
    TEST_ASSERT_TRUE(tp_id128_eq(a->sources[0].id, id1));
    TEST_ASSERT_FALSE(tp_id128_eq(a->sources[0].id, id2));

    tp_project_destroy(p);
}

void test_pending_sprite_duplicates_keep_first_position_and_merge_fields(void) {
    static const char json[] =
        "{\"version\":4,\"atlases\":[{"
        "\"id\":\"atlas_ffffffffffffffffffffffffffffffff\","
        "\"name\":\"a\",\"sprites\":["
        "{\"name\":\"hero\",\"origin\":[0.25,0.5]},"
        "{\"name\":\"one\"},{\"name\":\"two\"},{\"name\":\"three\"},"
        "{\"name\":\"four\"},{\"name\":\"five\"},{\"name\":\"six\"},"
        "{\"name\":\"seven\"},{\"name\":\"eight\"},{\"name\":\"nine\"},"
        "{\"name\":\"hero\",\"rename\":\"champ\"}]}]}";
    tp_project *project = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_load_buffer(json, sizeof json - 1U, &project, &error),
        error.msg);
    const tp_project_atlas *atlas = &project->atlases[0];
    TEST_ASSERT_EQUAL_INT(10, atlas->sprite_count);
    TEST_ASSERT_TRUE(fabsf(atlas->sprites[0].origin_x - 0.25F) < EPS);
    TEST_ASSERT_TRUE(fabsf(atlas->sprites[0].origin_y - 0.5F) < EPS);
    TEST_ASSERT_EQUAL_STRING("champ", atlas->sprites[0].rename);
    tp_project_destroy(project);
}

/* 10. sprite rename override: set creates the sparse entry; clear removes it when
 * the entry would then hold only defaults, else keeps it. */
void test_sprite_rename_override(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_set_pending_sprite_rename(a, "walk_01", "hero_walk"));
    tp_project_sprite *s = tp_project_atlas_find_pending_sprite(a, "walk_01");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("hero_walk", s->rename);
    TEST_ASSERT_EQUAL_INT(1, a->sprite_count);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_set_pending_sprite_rename(a, "walk_01", NULL)); /* clears + drops */
    TEST_ASSERT_NULL(tp_project_atlas_find_pending_sprite(a, "walk_01"));
    TEST_ASSERT_EQUAL_INT(0, a->sprite_count);

    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_pending_sprite(a, "walk_02", &sp));
    sp->origin_x = 0.1F; /* non-default -> the entry survives a rename clear */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_set_pending_sprite_rename(a, "walk_02", "hero_walk2"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_set_pending_sprite_rename(a, "walk_02", ""));
    sp = tp_project_atlas_find_pending_sprite(a, "walk_02");
    TEST_ASSERT_NOT_NULL(sp);
    TEST_ASSERT_NULL(sp->rename);

    tp_project_destroy(p);
}

/* 11. set_target replaces fields + is deterministic; sparse enabled default holds. */
void test_set_target(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    tp_project_target *t = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "json-neotolis", "out/a.json", &t));
    TEST_ASSERT_TRUE(t->enabled);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_set_target(a, 0, "defold", "out/a.tpinfo", false));
    TEST_ASSERT_EQUAL_STRING("defold", a->targets[0].exporter_id);
    TEST_ASSERT_EQUAL_STRING("out/a.tpinfo", a->targets[0].out_path);
    TEST_ASSERT_FALSE(a->targets[0].enabled);

    /* bounds + arg validation */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_atlas_set_target(a, 3, "defold", "x", true));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_project_atlas_set_target(a, 0, "", "x", true));
    tp_project_destroy(p);
}

/* C3: the shared project-wide duplicate-out_path detector. Two atlases whose targets
 * export to one out_path silently overwrite each other -- tp_project_out_path_shared is
 * the single source of truth CLI `validate` and the GUI target panel both surface. */
void test_out_path_shared(void) {
    tp_project *p = tp_project_create();
    /* atlas 0 (default "atlas1"): a target at out/x + one at a unique path. */
    tp_project_atlas *a0 = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a0, "defold", "out/x", NULL));       /* [0] */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a0, "json-neotolis", "out/uniq", NULL)); /* [1] */
    /* a SECOND atlas: a target REUSING out/x (cross-atlas collision), a unique out/y, and a target that
     * ALSO writes out/uniq but is DISABLED below (must NOT make out/uniq collide -- the exporter skips it). */
    int a1i = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "atlas2", &a1i));
    tp_project_atlas *a1 = tp_project_get_atlas(p, a1i);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a1, "defold", "out/x", NULL));       /* [0] */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a1, "json-neotolis", "out/y", NULL)); /* [1] */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a1, "defold", "out/uniq", NULL));    /* [2] disabled */

    /* NO promote(): ids stay NIL, proving the detector excludes `self` by POINTER identity (review finding
     * on the nil-id self-match), not by id. */
    a0 = tp_project_get_atlas(p, 0);
    a1 = tp_project_get_atlas(p, a1i);
    a1->targets[2].enabled = false; /* the 2nd out/uniq writer is OFF -> the exporter skips it */
    const tp_project_target *x0 = &a0->targets[0];    /* enabled, out/x */
    const tp_project_target *uniq0 = &a0->targets[1]; /* enabled, out/uniq */
    const tp_project_target *x1 = &a1->targets[0];    /* enabled, out/x (cross-atlas dup) */
    const tp_project_target *y1 = &a1->targets[1];    /* enabled, out/y */

    /* out/x is shared by two ENABLED targets across atlases: each sees the OTHER. */
    TEST_ASSERT_TRUE(tp_project_out_path_shared(p, "out/x", x0));
    TEST_ASSERT_TRUE(tp_project_out_path_shared(p, "out/x", x1));
    /* separator normalization: a backslash spelling resolves to the same file the exporter writes. */
    TEST_ASSERT_TRUE(tp_project_out_path_shared(p, "out\\x", x0));
    /* out/uniq's only OTHER writer is DISABLED -> no live overwrite. */
    TEST_ASSERT_FALSE(tp_project_out_path_shared(p, "out/uniq", uniq0));
    /* a unique path never collides; self excluded by identity. */
    TEST_ASSERT_FALSE(tp_project_out_path_shared(p, "out/y", y1));
    /* a path present on NO enabled target is not shared. */
    TEST_ASSERT_FALSE(tp_project_out_path_shared(p, "out/absent", NULL));
    /* empty / NULL out_path never collides (that is the separate empty-out_path check). */
    TEST_ASSERT_FALSE(tp_project_out_path_shared(p, "", NULL));
    TEST_ASSERT_FALSE(tp_project_out_path_shared(p, NULL, NULL));
    /* NULL project -> false. */
    TEST_ASSERT_FALSE(tp_project_out_path_shared(NULL, "out/x", NULL));

    tp_project_destroy(p);
}

/* 12. per-sprite packing overrides: absent stays absent (sparse), present round-trips
 * and re-saves byte-identical. */
void test_sprite_override_sparse(void) {
    char path[512];
    char path2[512];
    join(path, sizeof path, "ov1.ntpacker_project");
    join(path2, sizeof path2, "ov2.ntpacker_project");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_pending_sprite(a, "s1", &sp));
    /* defaults inherit -> all override keys absent */
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, sp->ov_shape);
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, sp->ov_extrude);
    sp->ov_shape = 0;        /* RECT */
    sp->ov_max_vertices = 4; /* explicit */

    promote(p);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err));
    size_t n = 0;
    char *buf = read_all(path, &n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"shape\""));        /* present override */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"max_vertices\"")); /* present override */
    TEST_ASSERT_NULL(strstr(buf, "\"margin\""));           /* inherited -> absent */
    TEST_ASSERT_NULL(strstr(buf, "\"allow_rotate\""));     /* inherited -> absent */
    free(buf);

    tp_project *loaded = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load(path, &loaded, &err));
    tp_project_sprite *ls = tp_project_atlas_find_pending_sprite(tp_project_get_atlas(loaded, 0), "s1");
    TEST_ASSERT_NOT_NULL(ls);
    TEST_ASSERT_EQUAL_INT(0, ls->ov_shape);
    TEST_ASSERT_EQUAL_INT(4, ls->ov_max_vertices);
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, ls->ov_margin);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(loaded, path2, &err));
    size_t n1 = 0;
    size_t n2 = 0;
    char *b1 = read_all(path, &n1);
    char *b2 = read_all(path2, &n2);
    TEST_ASSERT_EQUAL_size_t(n1, n2);
    TEST_ASSERT_EQUAL_INT(0, memcmp(b1, b2, n1));
    free(b1);
    free(b2);
    tp_project_destroy(loaded);
    tp_project_destroy(p);
}

/* 13. default-target seed helper (A5): seeds one json-neotolis target at
 * "out/<name>"; tp_project_create itself stays target-free (L-5). */
void test_seed_default_target(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, a->target_count, "tp_project_create must stay target-free (L-5)");

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_seed_default_target(p, 0));
    TEST_ASSERT_EQUAL_INT(1, a->target_count);
    TEST_ASSERT_EQUAL_STRING(TP_EXPORTER_ID_JSON_NEOTOLIS, a->targets[0].exporter_id);
    char want[128];
    (void)snprintf(want, sizeof want, "out/%s", a->name);
    TEST_ASSERT_EQUAL_STRING(want, a->targets[0].out_path);
    TEST_ASSERT_TRUE(a->targets[0].enabled);

    /* out-of-range atlas -> OUT_OF_BOUNDS, never a crash. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_atlas_seed_default_target(p, 5));
    tp_project_destroy(p);
}

/* 14. B4 trivial mutator: rename an atlas in place; reject empty/NULL. */
void test_set_atlas_name(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_STRING("atlas1", a->name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_set_atlas_name(a, "renamed"));
    TEST_ASSERT_EQUAL_STRING("renamed", a->name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_project_set_atlas_name(a, ""));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_project_set_atlas_name(a, NULL));
    TEST_ASSERT_EQUAL_STRING("renamed", a->name); /* unchanged on rejection */
    tp_project_destroy(p);
}

/* 15. B4 trivial mutator: prune drops an all-default override, keeps a non-default
 * one, and no-ops on an absent entry (the `sprite set <field>=inherit` sparse path). */
void test_prune_sprite(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    tp_project_sprite *s = NULL;

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_pending_sprite(a, "s1", &s));
    TEST_ASSERT_EQUAL_INT(1, a->sprite_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_prune_pending_sprite(a, "s1")); /* all-default -> dropped */
    TEST_ASSERT_EQUAL_INT(0, a->sprite_count);
    TEST_ASSERT_NULL(tp_project_atlas_find_pending_sprite(a, "s1"));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_prune_pending_sprite(a, "absent")); /* no-op OK */

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_pending_sprite(a, "s2", &s));
    s->slice9_lrtb[0] = 4; /* non-default -> survives prune */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_prune_pending_sprite(a, "s2"));
    TEST_ASSERT_EQUAL_INT(1, a->sprite_count);
    s = tp_project_atlas_find_pending_sprite(a, "s2");
    TEST_ASSERT_NOT_NULL(s);
    s->slice9_lrtb[0] = 0; /* cleared back to default -> now prunes */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_prune_pending_sprite(a, "s2"));
    TEST_ASSERT_EQUAL_INT(0, a->sprite_count);
    tp_project_destroy(p);
}

/* 16. The historical `<path>.savetmp` name is not owned by this process. A stale
 * directory (or another writer using that name) must neither block the save nor
 * be removed. This deterministically pins the multi-writer contract without a
 * timing-sensitive race: every save must create and clean up only its own unique
 * sibling temp. */
void test_project_save_ignores_unowned_fixed_temp_name(void) {
    char path[512];
    char tmpdir[600];
    char pin[640];
    join(path, sizeof path, "atomic.ntpacker_project");
    (void)snprintf(tmpdir, sizeof tmpdir, "%s.savetmp", path);
    (void)snprintf(pin, sizeof pin, "%s/pin", tmpdir);

    tp_project *p = build_rich();
    tp_error err = {0};

    /* 1. Initial good save and snapshot. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_save(p, path, &err), err.msg);
    size_t n1 = 0;
    char *v1 = read_all(path, &n1);
    TEST_ASSERT_TRUE(n1 > 0);

    /* 2. A foreign regular file at the old name is neither removed nor reused. */
    (void)remove(pin);
    (void)TP_TEST_RMDIR(tmpdir);
    (void)remove(tmpdir);
    write_text(tmpdir, "foreign-file");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_set_atlas_name(tp_project_get_atlas(p, 0), "after-file"));
    tp_error err2 = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_save(p, path, &err2), err2.msg);
    size_t foreign_len = 0;
    char *foreign_bytes = read_all(tmpdir, &foreign_len);
    TEST_ASSERT_EQUAL_size_t(strlen("foreign-file"), foreign_len);
    TEST_ASSERT_EQUAL_STRING("foreign-file", foreign_bytes);
    free(foreign_bytes);
    (void)remove(tmpdir);

    /* 3. An unremovable foreign directory at the old name does not block save. */
    TEST_ASSERT_EQUAL_INT(0, TP_TEST_MKDIR(tmpdir));
    write_text(pin, "foreign");

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_set_atlas_name(tp_project_get_atlas(p, 0), "after-dir"));
    err2 = (tp_error){0};
    tp_status st = tp_project_save(p, path, &err2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, st, err2.msg);

    /* 4. The target contains our new bytes, while the foreign directory is untouched. */
    size_t n2 = 0;
    char *v2 = read_all(path, &n2);
    TEST_ASSERT_TRUE(n2 > 0);
    TEST_ASSERT_FALSE(n1 == n2 && memcmp(v1, v2, n1) == 0);
    size_t pin_len = 0;
    char *pin_bytes = read_all(pin, &pin_len);
    TEST_ASSERT_EQUAL_size_t(strlen("foreign"), pin_len);
    TEST_ASSERT_EQUAL_STRING("foreign", pin_bytes);

    /* 5. Cleanup. */
    (void)remove(pin);
    (void)TP_TEST_RMDIR(tmpdir);
    (void)remove(path);
    free(v1);
    free(v2);
    free(pin_bytes);
    tp_project_destroy(p);
}

void test_project_save_temp_create_failure_keeps_destination(void) {
    char path[512];
    join(path, sizeof path, "atomic-fault.ntpacker_project");
    tp_project *p = build_rich();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err));
    size_t before_len = 0;
    char *before = read_all(path, &before_len);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_set_atlas_name(tp_project_get_atlas(p, 0), "must-not-land"));
    tp_project__test_fail_next_temp_create();
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_project_save(p, path, &err));
    size_t after_len = 0;
    char *after = read_all(path, &after_len);
    TEST_ASSERT_EQUAL_size_t(before_len, after_len);
    TEST_ASSERT_EQUAL_MEMORY(before, after, before_len);

    free(before);
    free(after);
    (void)remove(path);
    tp_project_destroy(p);
}

void test_failed_save_as_keeps_live_project_paths(void) {
    char first[512];
    char other[512];
    join(first, sizeof first, "stage-paths-first.ntpacker_project");
    (void)snprintf(other, sizeof other, "%s/missing-stage-dir/other.ntpacker_project", g_dir);
    tp_project *p = build_rich();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, first, &err));
    TEST_ASSERT_NOT_NULL(p->project_dir);
    char *before_dir = dupstr(p->project_dir);
    TEST_ASSERT_NOT_NULL(before_dir);
    const char *before_source = p->atlases[0].sources[0].path;
    char *source_copy = dupstr(before_source);
    TEST_ASSERT_NOT_NULL(source_copy);

    tp_project__test_fail_next_temp_create();
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_project_save(p, other, &err));
    TEST_ASSERT_EQUAL_STRING(before_dir, p->project_dir);
    TEST_ASSERT_EQUAL_STRING(source_copy, p->atlases[0].sources[0].path);

    free(before_dir);
    free(source_copy);
    (void)remove(first);
    tp_project_destroy(p);
}

void test_save_if_unchanged_rechecks_immediately_before_publish(void) {
    char path[512];
    join(path, sizeof path, "conditional-save.ntpacker_project");
    tp_project *p = build_rich();
    promote(p);
    tp_id128 baseline = {{0}};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save_with_fingerprint(p, path, &baseline, &err));
    char *before_dir = dupstr(p->project_dir);
    TEST_ASSERT_NOT_NULL(before_dir);
    write_text(path, "external-writer-won");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_set_atlas_name(tp_project_get_atlas(p, 0), "must-not-land"));

    tp_id128 output = {{0xFF}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_FILE_CHANGED_EXTERNALLY,
                          tp_project_save_if_unchanged(p, path, &baseline, &output, &err));
    TEST_ASSERT_TRUE(tp_id128_is_nil(output));
    TEST_ASSERT_EQUAL_STRING(before_dir, p->project_dir);
    size_t actual_len = 0;
    char *actual = read_all(path, &actual_len);
    TEST_ASSERT_EQUAL_size_t(strlen("external-writer-won"), actual_len);
    TEST_ASSERT_EQUAL_STRING("external-writer-won", actual);

    free(actual);
    free(before_dir);
    (void)remove(path);
    tp_project_destroy(p);
}

void test_atomic_replace_preserves_existing_posix_mode(void) {
#ifdef _WIN32
    TEST_IGNORE_MESSAGE("POSIX mode contract");
#else
    char path[512];
    join(path, sizeof path, "preserve-mode.ntpacker_project");
    tp_project *p = build_rich();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err));
    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0664));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_set_atlas_name(tp_project_get_atlas(p, 0), "mode-preserved"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err));
    struct stat actual;
    TEST_ASSERT_EQUAL_INT(0, stat(path, &actual));
    TEST_ASSERT_EQUAL_INT(0664, actual.st_mode & 0777);
    (void)remove(path);
    tp_project_destroy(p);
#endif
}

void test_load_with_fingerprint_returns_exact_consumed_bytes(void) {
    char path[512];
    join(path, sizeof path, "load-fingerprint.ntpacker_project");
    const char json[] = "{\n  \"version\": 4,\n  \"atlases\": []\n}\n";
    write_text(path, json);

    tp_project *p = NULL;
    tp_id128 actual = {{0}};
    tp_id128 expected = {{0}};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_project_load_with_fingerprint(path, &p, &actual, &err), err.msg);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_identity_bytes_fingerprint(json, sizeof json - 1U, &expected, NULL));
    TEST_ASSERT_TRUE(tp_id128_eq(expected, actual));

    tp_project_destroy(p);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

void test_save_with_fingerprint_returns_exact_written_bytes(void) {
    char path[512];
    join(path, sizeof path, "save-fingerprint.ntpacker_project");
    tp_project *p = build_rich();
    promote(p);
    tp_id128 actual = {{0}};
    tp_id128 expected = {{0}};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_project_save_with_fingerprint(p, path, &actual, &err), err.msg);

    size_t len = 0;
    char *bytes = read_all(path, &len);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_bytes_fingerprint(bytes, len, &expected, NULL));
    TEST_ASSERT_TRUE(tp_id128_eq(expected, actual));

    free(bytes);
    tp_project_destroy(p);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

void test_save_rejects_bytes_above_reader_limit_before_publish(void) {
    char path[512];
    join(path, sizeof path, "save-size-cap.ntpacker_project");
    (void)remove(path);
    tp_project *p = build_rich();
    promote(p);
    tp_id128 fingerprint = {{0xFF}};
    tp_error err = {0};

    tp_project__test_set_save_max_bytes(1U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_project_save_with_fingerprint(p, path, &fingerprint, &err));
    TEST_ASSERT_TRUE(tp_id128_is_nil(fingerprint));
    TEST_ASSERT_NULL(fopen(path, "rb")); /* no temp was published over the destination */

    tp_project_destroy(p);
}

void test_fingerprinted_io_clears_output_on_failure(void) {
    char path[512];
    join(path, sizeof path, "fingerprint-failure.ntpacker_project");
    write_text(path, "not json");

    tp_project *loaded = NULL;
    tp_id128 fingerprint = {{0xFF}};
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK,
                          tp_project_load_with_fingerprint(path, &loaded, &fingerprint, NULL));
    TEST_ASSERT_NULL(loaded);
    TEST_ASSERT_TRUE(tp_id128_is_nil(fingerprint));

    tp_project *p = build_rich();
    promote(p);
    fingerprint = (tp_id128){{0xFF}};
    tp_project__test_fail_next_temp_create();
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK,
                          tp_project_save_with_fingerprint(p, path, &fingerprint, NULL));
    TEST_ASSERT_TRUE(tp_id128_is_nil(fingerprint));

    tp_project_destroy(p);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

void test_core_owns_animation_and_target_defaults_without_truncation(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    promote(project);
    tp_project_atlas *atlas = &project->atlases[0];
    char name[128];
    tp_error err = {0};

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_next_animation_name(
                              project, atlas->id, NULL, name, sizeof name,
                              &err));
    TEST_ASSERT_EQUAL_STRING("anim1", name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_animation(atlas, name, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_next_animation_name(
                              project, atlas->id, NULL, name, sizeof name,
                              &err));
    TEST_ASSERT_EQUAL_STRING("anim2", name);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_animation(atlas, "walk", NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_next_animation_name(
                              project, atlas->id, "walk", name, sizeof name,
                              &err));
    TEST_ASSERT_EQUAL_STRING("walk2", name);

    char long_base[200];
    memset(long_base, 'x', sizeof long_base - 1U);
    long_base[sizeof long_base - 1U] = '\0';
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_project_next_animation_name(
                              project, atlas->id, long_base, name, sizeof name,
                              &err));
    TEST_ASSERT_EQUAL_CHAR('\0', name[0]);

    const char *exporter_id = NULL;
    char out_path[128];
    bool enabled = false;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_target_defaults(
                              project, atlas->id, &exporter_id, out_path,
                              sizeof out_path, &enabled, &err));
    TEST_ASSERT_EQUAL_STRING(TP_EXPORTER_ID_JSON_NEOTOLIS, exporter_id);
    TEST_ASSERT_EQUAL_STRING("out/atlas1", out_path);
    TEST_ASSERT_TRUE(enabled);
    tp_project_destroy(project);
}

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    UNITY_BEGIN();
    RUN_TEST(test_seed_default_target);
    RUN_TEST(test_project_load_lookup_work_is_linear);
    RUN_TEST(test_project_load_adversarial_hash_cluster_is_bounded);
    RUN_TEST(test_legacy_load_reports_bounded_synthesis_storage);
    RUN_TEST(test_legacy_load_rejects_overlong_source_paths);
    RUN_TEST(test_v3_overlong_source_records_bypass_dedupe_index);
    RUN_TEST(test_json_admission_exact_limits_and_escaped_punctuation);
    RUN_TEST(test_json_admission_rejects_nodes_entries_and_depth);
    RUN_TEST(test_json_admission_rejects_byte_limit_before_buffer_access);
    RUN_TEST(test_save_buffers_obey_project_json_admission_limits);
    RUN_TEST(test_save_buffer_stops_at_byte_limit_before_growth);
    RUN_TEST(test_roundtrip_and_byte_identical);
    RUN_TEST(test_sparse_defaults_absent);
    RUN_TEST(test_determinism);
    RUN_TEST(test_version_newer_refused);
    RUN_TEST(test_malformed_bad_project);
    RUN_TEST(test_wrong_type_bad_project);
    RUN_TEST(test_loader_rejects_trailing_non_whitespace);
    RUN_TEST(test_loader_rejects_non_finite_atlas_float);
    RUN_TEST(test_loader_rejects_non_finite_sprite_origin);
    RUN_TEST(test_loader_rejects_fractional_version);
    RUN_TEST(test_loader_rejects_fractional_padding);
    RUN_TEST(test_loader_rejects_out_of_int_range_max_size);
    RUN_TEST(test_loader_rejects_out_of_int_range_padding);
    RUN_TEST(test_loader_rejects_non_numeric_origin_items);
    RUN_TEST(test_loader_rejects_non_numeric_slice9_items);
    RUN_TEST(test_loader_rejects_fractional_slice9_items);
    RUN_TEST(test_loader_rejects_out_of_uint16_range_slice9_items);
    RUN_TEST(test_unknown_keys_ignored);
    RUN_TEST(test_absolute_path_relativized);
    RUN_TEST(test_resolve_path);
    RUN_TEST(test_save_as_preserves_relative_source_target);
    RUN_TEST(test_to_settings_mapping);
    RUN_TEST(test_mutation_helpers);
    RUN_TEST(test_anim_frame_edit);
    RUN_TEST(test_buffer_roundtrip);
    RUN_TEST(test_serialized_size_bounded_matches_writer_without_materialization);
    RUN_TEST(test_add_source_dedupe);
    RUN_TEST(test_source_kind_roundtrip);
    RUN_TEST(test_v3_source_dup_collapse);
    RUN_TEST(test_pending_sprite_duplicates_keep_first_position_and_merge_fields);
    RUN_TEST(test_sprite_rename_override);
    RUN_TEST(test_set_target);
    RUN_TEST(test_out_path_shared);
    RUN_TEST(test_sprite_override_sparse);
    RUN_TEST(test_set_atlas_name);
    RUN_TEST(test_prune_sprite);
    RUN_TEST(test_project_save_ignores_unowned_fixed_temp_name);
    RUN_TEST(test_project_save_temp_create_failure_keeps_destination);
    RUN_TEST(test_failed_save_as_keeps_live_project_paths);
    RUN_TEST(test_save_if_unchanged_rechecks_immediately_before_publish);
    RUN_TEST(test_atomic_replace_preserves_existing_posix_mode);
    RUN_TEST(test_load_with_fingerprint_returns_exact_consumed_bytes);
    RUN_TEST(test_save_with_fingerprint_returns_exact_written_bytes);
    RUN_TEST(test_save_rejects_bytes_above_reader_limit_before_publish);
    RUN_TEST(test_fingerprinted_io_clears_output_on_failure);
    RUN_TEST(test_core_owns_animation_and_target_defaults_without_truncation);
    return UNITY_END();
}
