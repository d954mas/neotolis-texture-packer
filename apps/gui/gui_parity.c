#include "gui_parity.h"

#include <stdio.h>
#include <string.h>

#include "gui_project.h"
#include "gui_rows.h"

#include "tp_core/tp_export.h"
// #region dev seam: --parity (headless saved-bytes byte-parity check)
/* Applies a FIXED non-creating edit sequence to `in` through the OP-based gui_project_*
 * setters (the typed-op cutover) and saves it to `out`. Non-creating (rename/settings/
 * override/target on an already-fixed-id project) => deterministic bytes: no random ids
 * are minted, so `out` is byte-comparable to the same logical edits applied by the
 * byte-identity-proven CLI (scripts/gui_parity_check.sh). Runs before any window/GL init
 * (pure model layer). Returns 0 on success, 2 on open/save failure. */
static bool parity_atlas_name(const char *name) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, 0)
                                         : NULL;
    return atlas && gui_project_set_atlas_name(
                        atlas->id, tp_session_snapshot_revision(snapshot), name);
}

static bool parity_atlas_setting(gui_atlas_field field, int ivalue, float fvalue) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, 0)
                                         : NULL;
    return atlas && gui_project_set_atlas_setting(
                        atlas->id, tp_session_snapshot_revision(snapshot), field,
                        ivalue, fvalue);
}

static bool parity_sprite_ref(const char *source_key, gui_sprite_ref *out) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, 0)
                                         : NULL;
    const tp_snapshot_source *source = atlas && atlas->source_count > 0
                                           ? tp_session_snapshot_source_at(snapshot, atlas->id, 0)
                                           : NULL;
    if (!source) {
        return false;
    }
    *out = (gui_sprite_ref){atlas->id, source->id, source_key,
                            tp_session_snapshot_revision(snapshot)};
    return true;
}

int gui_run_parity(const char *in, const char *out) {
    char err[256] = {0};
    gui_project_init();
    if (gui_project_open(in, err, sizeof err) != TP_STATUS_OK) {
        (void)fprintf(stderr, "parity: open '%s' failed: %s\n", in, err);
        return 2;
    }
    /* atlas rename + all 10 knobs (each a single-knob atlas.settings.set transaction; the
     * final atlas is identical to the CLI's one multi-knob `set`). */
    (void)parity_atlas_name("hero_atlas");
    (void)parity_atlas_setting(GUI_ATLAS_PADDING, 7, 0.0F);
    (void)parity_atlas_setting(GUI_ATLAS_MARGIN, 3, 0.0F);
    (void)parity_atlas_setting(GUI_ATLAS_EXTRUDE, 2, 0.0F);
    (void)parity_atlas_setting(GUI_ATLAS_SHAPE, 0, 0.0F);
    (void)parity_atlas_setting(GUI_ATLAS_ALPHA_THRESHOLD, 42, 0.0F);
    (void)parity_atlas_setting(GUI_ATLAS_MAX_VERTICES, 5, 0.0F);
    (void)parity_atlas_setting(GUI_ATLAS_MAX_SIZE, 2048, 0.0F);
    (void)parity_atlas_setting(GUI_ATLAS_ALLOW_TRANSFORM, 0, 0.0F);
    (void)parity_atlas_setting(GUI_ATLAS_POWER_OF_TWO, 1, 0.0F);
    (void)parity_atlas_setting(GUI_ATLAS_PIXELS_PER_UNIT, 0, 64.0F);
    /* Canonical source-owned sprite override: shape + origin + rename on the
     * resolved `psp` source key, matching the CLI selector contract. */
    gui_sprite_ref parity_sprite;
    if (parity_sprite_ref("psp", &parity_sprite)) {
        (void)gui_project_set_sprite_rename(&parity_sprite, "psp_final");
    }
    if (parity_sprite_ref("psp", &parity_sprite)) {
        (void)gui_project_set_sprite_override(&parity_sprite, GUI_SPRITE_OV_SHAPE, 0);
    }
    /* origin is now COMPONENT-keyed (#2): set X then Y (matches the CLI `origin=0.25,0.75`). The final
     * override record is byte-identical -- editing Y flushes the buffered X (X committed), then Y seeds
     * the committed X, so the saved sprite carries origin=(0.25,0.75) exactly as the single-op form did. */
    if (parity_sprite_ref("psp", &parity_sprite)) {
        (void)gui_project_set_sprite_origin(&parity_sprite, 0 /* X */, 0.25F);
    }
    if (parity_sprite_ref("psp", &parity_sprite)) {
        (void)gui_project_set_sprite_origin(&parity_sprite, 1 /* Y */, 0.75F);
    }
    if (parity_sprite_ref("psp", &parity_sprite)) {
        (void)gui_project_set_sprite_slice9(&parity_sprite, 0, 4);
    }
    /* Target 0 keeps its exporter from the immutable snapshot. Flush the buffered
     * slice9 first, then capture a fresh stable-ID target ref and copy the DTO string
     * before the next operation can invalidate the cached snapshot. */
    {
        gui_project_flush_pending();
        const tp_session_snapshot *snapshot = gui_project_snapshot();
        const tp_snapshot_atlas *a = snapshot ? tp_session_snapshot_atlas_at(snapshot, 0) : NULL;
        const tp_snapshot_target *t = a ? tp_session_snapshot_target_at(snapshot, a->id, 0) : NULL;
        if (t) {
            gui_target_ref target;
            char exporter[TP_EXPORTER_ID_MAX];
            memcpy(exporter, t->exporter_id, strlen(t->exporter_id) + 1U);
            if (gui_project_target_ref_at(0, 0, &target)) {
                (void)gui_project_set_target(&target, exporter, "out/hero", true);
            }
        }
    }
    if (gui_project_save_as(out, err, sizeof err) != TP_STATUS_OK) {
        (void)fprintf(stderr, "parity: save '%s' failed: %s\n", out, err);
        return 2;
    }
    (void)fprintf(stdout, "parity: wrote %s\n", out);
    gui_rows_shutdown();
    gui_project_shutdown();
    return 0;
}
// #endregion
