#include "gui_pack_internal.h"

#include <stdio.h>

#include "log/nt_log.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"

#include "gui_project.h"

typedef struct {
    tp_arena *arena;
    tp_result *result;
    tp_id128 atlas_id;
    tp_session_input_token input_token;
    bool valid;
    int atlas_index;
    char exporter_id[TP_EXPORTER_ID_MAX];
} preview_slot;

static preview_slot s_preview = {.atlas_index = -1};

typedef struct {
    const tp_exporter *exporter;
    tp_id128 atlas_id;
    uint64_t model_generation;
    uint64_t snapshot_lifetime;
    int count;
    char chip[128];
    char tip[512];
    bool valid;
} preview_diff_cache;

static preview_diff_cache s_preview_diff;

#ifdef NTPACKER_GUI_SELFTEST
static uint64_t s_preview_diff_rebuilds;

void gui_pack_preview_diff_work_reset(void) {
    s_preview_diff_rebuilds = 0U;
}

uint64_t gui_pack_preview_diff_rebuilds(void) {
    return s_preview_diff_rebuilds;
}
#endif

void gui_pack_preview_publish(tp_session_pack_job_result *pack,
                              int atlas_index, double elapsed_ms) {
    if (s_preview.arena) {
        tp_arena_destroy(s_preview.arena);
    }
    s_preview.arena = pack->arena;
    s_preview.result = pack->result;
    s_preview.atlas_id = pack->atlas_id;
    s_preview.input_token = pack->input_token_at_start;
    s_preview.valid = true;
    s_preview.atlas_index = atlas_index;
    (void)snprintf(s_preview.exporter_id, sizeof s_preview.exporter_id, "%s",
                   pack->preview_exporter_id);
    pack->arena = NULL;
    nt_log_info("gui_pack(async): preview '%s' via %s packed %d sprite(s), %d page(s) in %.1f ms",
                s_preview.result->atlas_name, s_preview.exporter_id,
                s_preview.result->sprite_count, s_preview.result->page_count,
                elapsed_ms);
}

bool gui_pack_preview_belongs_to(int atlas_index) {
    return s_preview.atlas_index == atlas_index;
}

const tp_result *gui_pack_preview_result(int atlas_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot ? tp_session_snapshot_atlas_at(snapshot, atlas_index) : NULL;
    if (!s_preview.valid || !atlas ||
        !tp_id128_eq(s_preview.atlas_id, atlas->id) ||
        !tp_session_input_token_equal(tp_session_snapshot_input_token(snapshot),
                                      s_preview.input_token)) {
        return NULL;
    }
    return s_preview.result;
}

void gui_pack_preview_clear(void) {
    if (s_preview.arena) {
        tp_arena_destroy(s_preview.arena);
    }
    s_preview.arena = NULL;
    s_preview.result = NULL;
    s_preview.atlas_id = tp_id128_nil();
    s_preview.input_token = (tp_session_input_token){0};
    s_preview.valid = false;
    s_preview.atlas_index = -1;
    s_preview.exporter_id[0] = '\0';
}

static bool preview_field_phrases(int field_id, const char **short_tok,
                                  const char **long_line) {
    switch (field_id) {
        case TP_NOTICE_FIELD_TRANSFORM:
            *short_tok = "no rotate/flip";
            *long_line = "Rotations/flips off -- this format can't encode the full D4 orientation set";
            return true;
        case TP_NOTICE_FIELD_POLYGON:
            *short_tok = "polygons \xE2\x86\x92 rect";
            *long_line = "Polygon hulls flattened to rectangles -- this format stores quads only";
            return true;
        case TP_NOTICE_FIELD_SLICE9:
            *short_tok = "slice9 dropped";
            *long_line = "9-slice borders dropped -- this format does not store them";
            return true;
        case TP_NOTICE_FIELD_PIVOT:
            *short_tok = "pivot dropped";
            *long_line = "Per-sprite pivots dropped -- this format does not store them";
            return true;
        default:
            return false;
    }
}

int gui_pack_preview_diff(int atlas_index, const char *exporter_id, char *chip,
                          size_t chip_cap, char *tip, size_t tip_cap) {
    if (chip && chip_cap) {
        chip[0] = '\0';
    }
    if (tip && tip_cap) {
        tip[0] = '\0';
    }
    const tp_exporter *exporter = tp_exporter_find(exporter_id);
    if (!exporter) {
        return 0;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot ? tp_session_snapshot_atlas_at(snapshot, atlas_index) : NULL;
    if (!atlas) {
        return 0;
    }
    const uint64_t model_generation =
        tp_session_snapshot_model_generation(snapshot);
    const uint64_t snapshot_lifetime =
        gui_project_snapshot_lifetime_generation();
    if (s_preview_diff.valid && s_preview_diff.exporter == exporter &&
        tp_id128_eq(s_preview_diff.atlas_id, atlas->id) &&
        s_preview_diff.model_generation == model_generation &&
        s_preview_diff.snapshot_lifetime == snapshot_lifetime) {
        if (chip && chip_cap) {
            (void)snprintf(chip, chip_cap, "%s", s_preview_diff.chip);
        }
        if (tip && tip_cap) {
            (void)snprintf(tip, tip_cap, "%s", s_preview_diff.tip);
        }
        return s_preview_diff.count;
    }

    s_preview_diff.valid = false;
    s_preview_diff.chip[0] = '\0';
    s_preview_diff.tip[0] = '\0';
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    tp_error error = {{0}};
#ifdef NTPACKER_GUI_SELFTEST
    s_preview_diff_rebuilds++;
#endif
    if (tp_export_predict_loss_snapshot(snapshot, atlas->id, &exporter->caps,
                                        exporter_id, NULL, &notices,
                                        &error) != TP_STATUS_OK) {
        tp_export_notices_free(&notices);
        return 0;
    }

    int count = 0;
    size_t chip_len = 0;
    size_t tip_len = 0;
    for (int i = 0; i < notices.count; i++) {
        const char *short_tok = NULL;
        const char *long_line = NULL;
        if (!preview_field_phrases(notices.items[i].field_id, &short_tok,
                                   &long_line)) {
            continue;
        }
        if (chip_len < sizeof s_preview_diff.chip) {
            const int written = snprintf(
                s_preview_diff.chip + chip_len,
                sizeof s_preview_diff.chip - chip_len, "%s%s",
                count > 0 ? ", " : "", short_tok);
            if (written > 0) {
                chip_len += (size_t)written;
            }
        }
        if (tip_len < sizeof s_preview_diff.tip) {
            const int written = snprintf(
                s_preview_diff.tip + tip_len,
                sizeof s_preview_diff.tip - tip_len, "%s%s",
                count > 0 ? "\n" : "", long_line);
            if (written > 0) {
                tip_len += (size_t)written;
            }
        }
        count++;
    }
    tp_export_notices_free(&notices);
    s_preview_diff.exporter = exporter;
    s_preview_diff.atlas_id = atlas->id;
    s_preview_diff.model_generation = model_generation;
    s_preview_diff.snapshot_lifetime = snapshot_lifetime;
    s_preview_diff.count = count;
    s_preview_diff.valid = true;
    if (chip && chip_cap) {
        (void)snprintf(chip, chip_cap, "%s", s_preview_diff.chip);
    }
    if (tip && tip_cap) {
        (void)snprintf(tip, tip_cap, "%s", s_preview_diff.tip);
    }
    return count;
}
