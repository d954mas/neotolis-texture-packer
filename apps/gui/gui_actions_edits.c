#include "gui_actions_internal.h"
#include "gui_project.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui_state.h"
// #region draft owner and deferred structural edits
/* One reducer owns every in-progress value edit. Animation frame operations
 * and narrow target toggles remain copied between-frame intents because they
 * are discrete commands, not editable values. No retained item holds a model
 * or session pointer. */

static bool edit_id_generate(tp_id128 *out, const char *what) {
    tp_rng rng = tp_rng_os();
    tp_error error = {{0}};
    const tp_status status =
        tp_id128_generate(&rng, out, &error);
    if (status == TP_STATUS_OK) {
        return true;
    }
    set_statusf_ex(
        STATUS_ERROR, "%s: %s",
        what, error.msg[0] ? error.msg
                           : tp_status_str(status));
    return false;
}

static void edit_transaction_id(
    tp_id128 id, char out[33]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0U; index < 16U; ++index) {
        out[index * 2U] =
            hex[id.bytes[index] >> 4U];
        out[index * 2U + 1U] =
            hex[id.bytes[index] & 0x0fU];
    }
    out[32] = '\0';
}

static gui_session_submit_identity draft_identity(
    const gui_edit_state *edit) {
    if (!edit || edit->phase == GUI_EDIT_IDLE) {
        return (gui_session_submit_identity){0};
    }
    return (gui_session_submit_identity){
        .origin_view_id = edit->view_id,
        .draft_instance_id = edit->draft_instance_id,
    };
}

static void draft_clear_if_idle(
    gui_draft_owner *draft) {
    if (draft->lifecycle.phase != GUI_EDIT_IDLE) {
        return;
    }
    draft->family = GUI_DRAFT_NONE;
    memset(&draft->atlas, 0, sizeof draft->atlas);
    memset(&draft->text, 0, sizeof draft->text);
    memset(&draft->sprite, 0, sizeof draft->sprite);
    memset(&draft->animation, 0, sizeof draft->animation);
}

/* ---- draft descriptor table -----------------------------------------------
 * One pure-data row per editable draft component. Target presence, "is this the
 * component I am editing", net-zero detection, and submit all read the SAME
 * rows, so adding a component is one row instead of four parallel switch arms.
 * There are no function-pointer columns; exactly two exhaustive (default-less)
 * switches over draft_value_type remain -- the payload write and the snapshot
 * read -- so -Wswitch still proves every value shape is handled. */

typedef enum draft_value_type {
    DRAFT_VALUE_INT = 0, /* int payload,   int snapshot */
    DRAFT_VALUE_BOOL,    /* bool payload,  bool snapshot */
    DRAFT_VALUE_FLOAT,   /* float payload, float snapshot */
    DRAFT_VALUE_TEXT,    /* char * payload, const char * snapshot */
    DRAFT_VALUE_INT16,   /* int payload,   int16_t snapshot (sprite overrides) */
    DRAFT_VALUE_UINT16   /* int payload,   uint16_t snapshot (sprite slice9) */
} draft_value_type;

/* Which immutable snapshot record a column addresses. Presence and value may
 * differ: a sprite draft exists while its SOURCE exists, but its committed
 * value lives on the per-key SPRITE record. */
typedef enum draft_record_kind {
    DRAFT_RECORD_ATLAS = 0,
    DRAFT_RECORD_ANIMATION,
    DRAFT_RECORD_SOURCE,
    DRAFT_RECORD_SPRITE,
    DRAFT_RECORD_TARGET
} draft_record_kind;

typedef struct draft_value {
    int integer;
    float real;
    const char *text;
} draft_value;

typedef struct draft_descriptor {
    gui_draft_family family;
    int kind;      /* atlas field / text kind / sprite kind / animation kind */
    int component; /* sub-index inside the kind (axis, slice9 slot, override) */
    tp_op_kind op_kind;
    uint32_t field_mask;
    draft_value_type value_type;
    /* offsetof inside the typed operation payload. Zero (and unused) where the
     * concrete mutation owner builds the payload itself: TEXT submits and the
     * sprite origin/slice9 read-modify-write against the live snapshot. */
    size_t payload_offset;
    draft_record_kind presence_record;
    draft_record_kind value_record;
    size_t value_offset; /* offsetof inside the value record */
    /* Committed value to compare against when the record is absent. When
     * `missing_comparable` is false an absent record is never net zero. TEXT
     * rows ignore the flag and use `missing.text` (NULL == not comparable). */
    bool missing_comparable;
    draft_value missing;
} draft_descriptor;

#define DRAFT_ATLAS_ROW(field, mask, type, member)                            \
    {GUI_DRAFT_ATLAS_SCALAR, (field), 0, TP_OP_ATLAS_SETTINGS_SET, (mask),    \
     (type), offsetof(tp_op_atlas_settings, member), DRAFT_RECORD_ATLAS,      \
     DRAFT_RECORD_ATLAS, offsetof(tp_snapshot_atlas, member), false,          \
     {0, 0.0F, NULL}}

#define DRAFT_SLICE9_ROW(slot)                                                \
    {GUI_DRAFT_SPRITE, GUI_SPRITE_EDIT_SLICE9, (slot),                        \
     TP_OP_SPRITE_OVERRIDE_SET, TP_SPF_SLICE9, DRAFT_VALUE_UINT16, 0U,        \
     DRAFT_RECORD_SOURCE, DRAFT_RECORD_SPRITE,                                \
     offsetof(tp_snapshot_sprite, slice9_lrtb) + (slot) * sizeof(uint16_t),   \
     true, {0, 0.0F, NULL}}

#define DRAFT_OVERRIDE_ROW(component, mask, payload, member)                  \
    {GUI_DRAFT_SPRITE, GUI_SPRITE_EDIT_OVERRIDE, (component),                 \
     TP_OP_SPRITE_OVERRIDE_SET, (mask), DRAFT_VALUE_INT16,                    \
     offsetof(tp_op_sprite_set, payload), DRAFT_RECORD_SOURCE,                \
     DRAFT_RECORD_SPRITE, offsetof(tp_snapshot_sprite, member), true,         \
     {TP_PROJECT_OV_INHERIT, 0.0F, NULL}}

#define DRAFT_ANIM_ROW(kind, component, mask, type, payload, member)          \
    {GUI_DRAFT_ANIMATION, (kind), (component), TP_OP_ANIMATION_SETTINGS_SET,  \
     (mask), (type), offsetof(tp_op_anim_settings, payload),                  \
     DRAFT_RECORD_ANIMATION, DRAFT_RECORD_ANIMATION,                          \
     offsetof(tp_snapshot_animation, member), false, {0, 0.0F, NULL}}

#define DRAFT_ORIGIN_ROW(axis, member)                                        \
    {GUI_DRAFT_SPRITE, GUI_SPRITE_EDIT_ORIGIN, (axis),                        \
     TP_OP_SPRITE_OVERRIDE_SET, TP_SPF_ORIGIN, DRAFT_VALUE_FLOAT, 0U,         \
     DRAFT_RECORD_SOURCE, DRAFT_RECORD_SPRITE,                                \
     offsetof(tp_snapshot_sprite, member), true,                              \
     {0, TP_PROJECT_ORIGIN_DEFAULT, NULL}}

static const draft_descriptor k_draft_rows[] = {
    DRAFT_ATLAS_ROW(GUI_ATLAS_MAX_SIZE, TP_AF_MAX_SIZE, DRAFT_VALUE_INT,
                    max_size),
    DRAFT_ATLAS_ROW(GUI_ATLAS_PADDING, TP_AF_PADDING, DRAFT_VALUE_INT, padding),
    DRAFT_ATLAS_ROW(GUI_ATLAS_MARGIN, TP_AF_MARGIN, DRAFT_VALUE_INT, margin),
    DRAFT_ATLAS_ROW(GUI_ATLAS_EXTRUDE, TP_AF_EXTRUDE, DRAFT_VALUE_INT, extrude),
    DRAFT_ATLAS_ROW(GUI_ATLAS_ALPHA_THRESHOLD, TP_AF_ALPHA_THRESHOLD,
                    DRAFT_VALUE_INT, alpha_threshold),
    DRAFT_ATLAS_ROW(GUI_ATLAS_MAX_VERTICES, TP_AF_MAX_VERTICES,
                    DRAFT_VALUE_INT, max_vertices),
    DRAFT_ATLAS_ROW(GUI_ATLAS_SHAPE, TP_AF_SHAPE, DRAFT_VALUE_INT, shape),
    DRAFT_ATLAS_ROW(GUI_ATLAS_ALLOW_TRANSFORM, TP_AF_ALLOW_TRANSFORM,
                    DRAFT_VALUE_BOOL, allow_transform),
    DRAFT_ATLAS_ROW(GUI_ATLAS_POWER_OF_TWO, TP_AF_POWER_OF_TWO,
                    DRAFT_VALUE_BOOL, power_of_two),
    DRAFT_ATLAS_ROW(GUI_ATLAS_PIXELS_PER_UNIT, TP_AF_PIXELS_PER_UNIT,
                    DRAFT_VALUE_FLOAT, pixels_per_unit),

    {GUI_DRAFT_TEXT, GUI_TEXT_EDIT_ATLAS_NAME, 0, TP_OP_ATLAS_RENAME, 0U,
     DRAFT_VALUE_TEXT, 0U, DRAFT_RECORD_ATLAS, DRAFT_RECORD_ATLAS,
     offsetof(tp_snapshot_atlas, name), true, {0, 0.0F, NULL}},
    {GUI_DRAFT_TEXT, GUI_TEXT_EDIT_ANIMATION_NAME, 0, TP_OP_ANIMATION_RENAME,
     0U, DRAFT_VALUE_TEXT, 0U, DRAFT_RECORD_ANIMATION, DRAFT_RECORD_ANIMATION,
     offsetof(tp_snapshot_animation, name), true, {0, 0.0F, NULL}},
    /* An absent sprite record reads as the EMPTY rename, so clearing a rename
     * on a sprite the snapshot no longer carries is still net zero. */
    {GUI_DRAFT_TEXT, GUI_TEXT_EDIT_SPRITE_RENAME, 0, TP_OP_SPRITE_NAME_SET, 0U,
     DRAFT_VALUE_TEXT, 0U, DRAFT_RECORD_SOURCE, DRAFT_RECORD_SPRITE,
     offsetof(tp_snapshot_sprite, rename), true, {0, 0.0F, ""}},
    {GUI_DRAFT_TEXT, GUI_TEXT_EDIT_TARGET_OUT_PATH, 0, TP_OP_TARGET_SET,
     TP_TF_OUT_PATH, DRAFT_VALUE_TEXT, 0U, DRAFT_RECORD_TARGET,
     DRAFT_RECORD_TARGET, offsetof(tp_snapshot_target, out_path), true,
     {0, 0.0F, NULL}},

    DRAFT_ORIGIN_ROW(0, origin_x),
    DRAFT_ORIGIN_ROW(1, origin_y),
    DRAFT_SLICE9_ROW(0),
    DRAFT_SLICE9_ROW(1),
    DRAFT_SLICE9_ROW(2),
    DRAFT_SLICE9_ROW(3),
    DRAFT_OVERRIDE_ROW(GUI_SPRITE_OV_SHAPE, TP_SPF_SHAPE, ov_shape,
                       override_shape),
    DRAFT_OVERRIDE_ROW(GUI_SPRITE_OV_ROTATE, TP_SPF_ALLOW_ROTATE,
                       ov_allow_rotate, override_allow_rotate),
    DRAFT_OVERRIDE_ROW(GUI_SPRITE_OV_MAXVERT, TP_SPF_MAX_VERTICES,
                       ov_max_vertices, override_max_vertices),
    DRAFT_OVERRIDE_ROW(GUI_SPRITE_OV_MARGIN, TP_SPF_MARGIN, ov_margin,
                       override_margin),
    DRAFT_OVERRIDE_ROW(GUI_SPRITE_OV_EXTRUDE, TP_SPF_EXTRUDE, ov_extrude,
                       override_extrude),

    DRAFT_ANIM_ROW(GUI_ANIMATION_EDIT_FPS, 0, TP_ANF_FPS, DRAFT_VALUE_FLOAT,
                   fps, fps),
    DRAFT_ANIM_ROW(GUI_ANIMATION_EDIT_PLAYBACK, 0, TP_ANF_PLAYBACK,
                   DRAFT_VALUE_INT, playback, playback),
    DRAFT_ANIM_ROW(GUI_ANIMATION_EDIT_FLIP, 0, TP_ANF_FLIP_H,
                   DRAFT_VALUE_BOOL, flip_h, flip_h),
    DRAFT_ANIM_ROW(GUI_ANIMATION_EDIT_FLIP, 1, TP_ANF_FLIP_V,
                   DRAFT_VALUE_BOOL, flip_v, flip_v),
};

#undef DRAFT_ATLAS_ROW
#undef DRAFT_SLICE9_ROW
#undef DRAFT_OVERRIDE_ROW
#undef DRAFT_ANIM_ROW
#undef DRAFT_ORIGIN_ROW

/* The draft's own coordinates, normalized so every table consumer addresses a
 * draft the same way regardless of family. */
typedef struct draft_key {
    gui_draft_family family;
    int kind;
    int component;
    tp_id128 atlas_id;
    tp_id128 entity_id; /* animation or export target */
    tp_id128 source_id;
    const char *source_key;
} draft_key;

static void draft_resolve(
    const gui_draft_owner *draft, draft_key *out) {
    memset(out, 0, sizeof *out);
    out->family = draft->family;
    out->atlas_id = tp_id128_nil();
    out->entity_id = tp_id128_nil();
    out->source_id = tp_id128_nil();
    out->source_key = "";
    if (draft->family == GUI_DRAFT_ATLAS_SCALAR) {
        out->kind = (int)draft->atlas.component;
        out->atlas_id = draft->lifecycle.target_id;
        return;
    }
    if (draft->family == GUI_DRAFT_TEXT) {
        out->kind = (int)draft->text.kind;
        out->atlas_id = draft->text.atlas_id;
        out->entity_id = draft->lifecycle.target_id;
        if (draft->text.kind ==
            GUI_TEXT_EDIT_SPRITE_RENAME) {
            out->source_id = draft->text.source_id;
            out->source_key = draft->text.source_key;
        }
        return;
    }
    if (draft->family == GUI_DRAFT_SPRITE) {
        out->kind = (int)draft->sprite.kind;
        out->component = draft->sprite.component;
        out->atlas_id = draft->sprite.atlas_id;
        out->entity_id = draft->lifecycle.target_id;
        out->source_id = draft->sprite.source_id;
        out->source_key = draft->sprite.source_key;
        return;
    }
    if (draft->family == GUI_DRAFT_ANIMATION) {
        out->kind = (int)draft->animation.kind;
        out->component = draft->animation.component;
        out->atlas_id = draft->animation.atlas_id;
        out->entity_id = draft->lifecycle.target_id;
    }
}

static const draft_descriptor *draft_row(
    const draft_key *key) {
    for (size_t index = 0U;
         index < sizeof k_draft_rows / sizeof k_draft_rows[0];
         ++index) {
        const draft_descriptor *row = &k_draft_rows[index];
        if (row->family == key->family &&
            row->kind == key->kind &&
            row->component == key->component) {
            return row;
        }
    }
    return NULL;
}

static const void *draft_record(
    draft_record_kind record, const draft_key *key,
    const tp_session_snapshot *snapshot) {
    switch (record) {
        case DRAFT_RECORD_ATLAS:
            return tp_session_snapshot_atlas_by_id(
                snapshot, key->atlas_id);
        case DRAFT_RECORD_ANIMATION:
            return tp_session_snapshot_animation_by_id(
                snapshot, key->atlas_id, key->entity_id);
        case DRAFT_RECORD_SOURCE:
            return tp_session_snapshot_source_by_id(
                snapshot, key->atlas_id, key->source_id);
        case DRAFT_RECORD_SPRITE:
            return tp_session_snapshot_sprite_by_key(
                snapshot, key->atlas_id, key->source_id,
                key->source_key);
        case DRAFT_RECORD_TARGET:
            return tp_session_snapshot_target_by_id(
                snapshot, key->atlas_id, key->entity_id);
    }
    return NULL;
}

/* SNAPSHOT READ -- one of the two exhaustive value-type switches. False means
 * the committed value is not comparable (the record is gone and the row has no
 * defined absent value). */
static bool draft_committed_value(
    const draft_descriptor *row, const void *record,
    draft_value *out) {
    /* Never form `NULL + offset`: an absent record answers from the row. */
    const char *field =
        record ? (const char *)record + row->value_offset
               : NULL;
    *out = row->missing;
    switch (row->value_type) {
        case DRAFT_VALUE_INT:
            if (record) {
                memcpy(&out->integer, field,
                       sizeof out->integer);
            }
            return record != NULL || row->missing_comparable;
        case DRAFT_VALUE_BOOL:
            if (record) {
                bool flag = false;
                memcpy(&flag, field, sizeof flag);
                out->integer = flag ? 1 : 0;
            }
            return record != NULL || row->missing_comparable;
        case DRAFT_VALUE_FLOAT:
            if (record) {
                memcpy(&out->real, field, sizeof out->real);
            }
            return record != NULL || row->missing_comparable;
        case DRAFT_VALUE_TEXT: {
            const char *text = NULL;
            if (record) {
                memcpy(&text, field, sizeof text);
            }
            if (text) {
                out->text = text;
            }
            return out->text != NULL;
        }
        case DRAFT_VALUE_INT16:
            if (record) {
                int16_t narrow = 0;
                memcpy(&narrow, field, sizeof narrow);
                out->integer = narrow;
            }
            return record != NULL || row->missing_comparable;
        case DRAFT_VALUE_UINT16:
            if (record) {
                uint16_t narrow = 0U;
                memcpy(&narrow, field, sizeof narrow);
                out->integer = (int)narrow;
            }
            return record != NULL || row->missing_comparable;
    }
    return false;
}

/* PAYLOAD WRITE -- the other exhaustive value-type switch. */
static void draft_write_payload(
    const draft_descriptor *row, void *payload,
    const draft_value *value) {
    char *field = (char *)payload + row->payload_offset;
    switch (row->value_type) {
        case DRAFT_VALUE_INT:
        case DRAFT_VALUE_INT16:
        case DRAFT_VALUE_UINT16: {
            const int integer = value->integer;
            memcpy(field, &integer, sizeof integer);
            break;
        }
        case DRAFT_VALUE_BOOL: {
            const bool flag = value->integer != 0;
            memcpy(field, &flag, sizeof flag);
            break;
        }
        case DRAFT_VALUE_FLOAT:
            memcpy(field, &value->real, sizeof value->real);
            break;
        case DRAFT_VALUE_TEXT: {
            char *text = (char *)value->text;
            memcpy(field, &text, sizeof text);
            break;
        }
    }
}

/* The in-progress value the draft is holding, in the row's shape. */
static void draft_current_value(
    const gui_draft_owner *draft,
    const draft_descriptor *row, draft_value *out) {
    memset(out, 0, sizeof *out);
    out->text = draft->text.value;
    if (row->family == GUI_DRAFT_ATLAS_SCALAR) {
        out->integer = draft->atlas.integer;
        out->real = draft->atlas.real;
    } else if (row->family == GUI_DRAFT_SPRITE) {
        out->integer = draft->sprite.integer;
        out->real = draft->sprite.real;
    } else if (row->family == GUI_DRAFT_ANIMATION) {
        out->integer =
            row->kind == GUI_ANIMATION_EDIT_FLIP
                ? (draft->animation.flag ? 1 : 0)
                : draft->animation.integer;
        out->real = draft->animation.real;
    }
}

/* True when the ONE active draft is exactly this family/component/target. */
static bool draft_matches(
    gui_draft_family family, int kind, int component,
    tp_id128 atlas_id, tp_id128 entity_id,
    tp_id128 source_id, const char *source_key) {
    const gui_draft_owner *draft = &s_actions.draft;
    if (draft->lifecycle.phase == GUI_EDIT_IDLE) {
        return false;
    }
    draft_key key;
    draft_resolve(draft, &key);
    return key.family == family && key.kind == kind &&
           key.component == component &&
           tp_id128_eq(key.atlas_id, atlas_id) &&
           tp_id128_eq(key.entity_id, entity_id) &&
           tp_id128_eq(key.source_id, source_id) &&
           strcmp(key.source_key,
                  source_key ? source_key : "") == 0;
}

static bool draft_target_present(
    const gui_draft_owner *draft,
    const tp_session_snapshot *snapshot) {
    if (!draft || !snapshot ||
        draft->lifecycle.phase == GUI_EDIT_IDLE) {
        return false;
    }
    draft_key key;
    draft_resolve(draft, &key);
    const draft_descriptor *row = draft_row(&key);
    return row != NULL &&
           draft_record(row->presence_record, &key,
                        snapshot) != NULL;
}

/* True when submitting the draft would change nothing in the committed model. */
static bool draft_is_net_zero(
    const gui_draft_owner *draft,
    const tp_session_snapshot *snapshot) {
    if (!draft || !snapshot) {
        return false;
    }
    draft_key key;
    draft_resolve(draft, &key);
    const draft_descriptor *row = draft_row(&key);
    if (!row) {
        return false;
    }
    draft_value committed;
    if (!draft_committed_value(
            row,
            draft_record(row->value_record, &key, snapshot),
            &committed)) {
        return false;
    }
    draft_value current;
    draft_current_value(draft, row, &current);
    if (row->value_type == DRAFT_VALUE_TEXT) {
        return strcmp(committed.text, current.text) == 0;
    }
    if (row->value_type == DRAFT_VALUE_FLOAT) {
        return current.real == committed.real;
    }
    return current.integer == committed.integer;
}

static bool ensure_draft_owner(void) {
    if (!s_actions.draft_initialized) {
        tp_id128 view_id = tp_id128_nil();
        if (!edit_id_generate(
                &view_id,
                "Could not create the settings view identity")) {
            return false;
        }
        gui_edit_state_init(
            &s_actions.draft.lifecycle,
            view_id);
        s_actions.draft_initialized = true;
    }
    return true;
}

static bool atlas_field_valid(gui_atlas_field field) {
    return field >= GUI_ATLAS_MAX_SIZE &&
           field <= GUI_ATLAS_PIXELS_PER_UNIT;
}

static bool component_draft_begin(
    gui_draft_family family, tp_id128 target_id,
    int64_t expected_revision, bool same_component,
    const char *what, bool *out_fresh);

static bool atlas_edit_matches(
    tp_id128 atlas_id, gui_atlas_field field) {
    return draft_matches(
        GUI_DRAFT_ATLAS_SCALAR, (int)field, 0, atlas_id,
        tp_id128_nil(), tp_id128_nil(), NULL);
}

void gui_edit_atlas_setting(
    tp_id128 atlas_id, int64_t expected_revision,
    gui_atlas_field field, int ivalue, float fvalue) {
    if (!atlas_field_valid(field)) {
        set_status_ex(
            STATUS_WARNING,
            "Atlas edit rejected: unknown scalar field.");
        return;
    }
    bool fresh = false;
    if (!component_draft_begin(
            GUI_DRAFT_ATLAS_SCALAR, atlas_id,
            expected_revision,
            atlas_edit_matches(atlas_id, field),
            "Could not create the atlas draft identity",
            &fresh)) {
        return;
    }
    gui_draft_owner *draft = &s_actions.draft;
    if (fresh) {
        draft->atlas.component = field;
    }
    if (field == GUI_ATLAS_PIXELS_PER_UNIT) {
        draft->atlas.real = fvalue;
    } else {
        draft->atlas.integer = ivalue;
    }
}

bool gui_atlas_edit_value(
    tp_id128 atlas_id, gui_atlas_field field,
    int *ivalue, float *fvalue) {
    const gui_draft_owner *draft = &s_actions.draft;
    if (draft->lifecycle.phase == GUI_EDIT_IDLE ||
        draft->family != GUI_DRAFT_ATLAS_SCALAR ||
        !tp_id128_eq(
            draft->lifecycle.target_id,
            atlas_id) ||
        draft->atlas.component != field) {
        return false;
    }
    if (field == GUI_ATLAS_PIXELS_PER_UNIT) {
        if (fvalue) {
            *fvalue = draft->atlas.real;
        }
    } else if (ivalue) {
        *ivalue = draft->atlas.integer;
    }
    return true;
}

static void reconcile_draft_view(void) {
    gui_draft_owner *draft = &s_actions.draft;
    gui_edit_state *edit = &draft->lifecycle;
    if (!s_actions.draft_initialized ||
        edit->phase == GUI_EDIT_IDLE) {
        return;
    }
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    if (!snapshot) {
        return;
    }
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);
    const bool target_present =
        draft_target_present(draft, snapshot);
    tp_error error = {{0}};
    (void)gui_edit_model_revision(
        edit, revision, target_present,
        false, &error);
    draft_clear_if_idle(draft);
}

gui_edit_phase gui_draft_phase(void) {
    reconcile_draft_view();
    return s_actions.draft.lifecycle.phase;
}

bool gui_draft_can_apply(void) {
    reconcile_draft_view();
    return s_actions.draft.lifecycle.phase ==
               GUI_EDIT_CONFLICTED &&
           s_actions.draft.lifecycle.target_present;
}

void gui_draft_apply_mine(void) {
    s_actions.draft_apply_mine = true;
}

void gui_draft_discard(void) {
    tp_error error = {{0}};
    const tp_status status =
        gui_edit_discard(
            &s_actions.draft.lifecycle,
            &error);
    if (status != TP_STATUS_OK) {
        set_statusf_ex(
            STATUS_WARNING, "GUI draft cannot be discarded: %s",
            error.msg[0] ? error.msg
                         : tp_status_str(status));
        return;
    }
    draft_clear_if_idle(&s_actions.draft);
}

static bool text_value_copy(
    char out[TP_IDENTITY_PATH_MAX],
    size_t capacity, const char *value,
    const char *what) {
    if (!value) {
        set_statusf_ex(
            STATUS_WARNING, "%s requires text.",
            what);
        return false;
    }
    const size_t length = strlen(value);
    if (length >= capacity) {
        set_statusf_ex(
            STATUS_WARNING,
            "%s exceeds the GUI text limit.",
            what);
        return false;
    }
    memcpy(out, value, length + 1U);
    return true;
}

static bool text_edit_begin(
    gui_text_edit_kind kind, tp_id128 target_id,
    tp_id128 atlas_id, tp_id128 source_id,
    const char *source_key, int64_t expected_revision,
    const char *initial_value) {
    if (!ensure_draft_owner()) {
        return false;
    }
    gui_draft_owner *draft = &s_actions.draft;
    if (draft->lifecycle.phase != GUI_EDIT_IDLE) {
        set_status_ex(
            STATUS_WARNING,
            "Finish or discard the active edit first.");
        return false;
    }
    if (tp_id128_is_nil(target_id) ||
        expected_revision < 0 ||
        !text_value_copy(
            draft->text.value,
            kind == GUI_TEXT_EDIT_TARGET_OUT_PATH
                ? sizeof draft->text.value
                : TP_SRCKEY_MAX,
            initial_value,
            "GUI text edit")) {
        return false;
    }
    if (source_key) {
        const size_t key_length = strlen(source_key);
        if (key_length == 0U ||
            key_length >= sizeof draft->text.source_key) {
            draft_clear_if_idle(draft);
            set_status_ex(
                STATUS_WARNING,
                "Sprite rename requires a valid source key.");
            return false;
        }
        memcpy(
            draft->text.source_key, source_key,
            key_length + 1U);
    }

    tp_id128 draft_id = tp_id128_nil();
    if (!edit_id_generate(
            &draft_id,
            "Could not create the text draft identity")) {
        draft_clear_if_idle(draft);
        return false;
    }
    tp_error error = {{0}};
    const tp_status status = gui_edit_begin(
        &draft->lifecycle, target_id,
        expected_revision, draft_id, &error);
    if (status != TP_STATUS_OK) {
        draft_clear_if_idle(draft);
        set_statusf_ex(
            STATUS_WARNING, "Text edit rejected: %s",
            error.msg[0] ? error.msg
                         : tp_status_str(status));
        return false;
    }
    draft->family = GUI_DRAFT_TEXT;
    draft->text.kind = kind;
    draft->text.atlas_id = atlas_id;
    draft->text.source_id = source_id;
    return true;
}

bool gui_text_edit_begin_atlas_name(
    tp_id128 atlas_id, int64_t expected_revision,
    const char *initial_value) {
    return text_edit_begin(
        GUI_TEXT_EDIT_ATLAS_NAME, atlas_id,
        atlas_id, tp_id128_nil(), NULL,
        expected_revision, initial_value);
}

bool gui_text_edit_begin_animation_name(
    const gui_animation_ref *animation,
    const char *initial_value) {
    return animation &&
           text_edit_begin(
               GUI_TEXT_EDIT_ANIMATION_NAME,
               animation->animation_id,
               animation->atlas_id, tp_id128_nil(),
               NULL, animation->expected_revision,
               initial_value);
}

bool gui_text_edit_begin_sprite_rename(
    const gui_sprite_ref *sprite,
    const char *initial_value) {
    return sprite &&
           text_edit_begin(
               GUI_TEXT_EDIT_SPRITE_RENAME,
               sprite->source_id, sprite->atlas_id,
               sprite->source_id, sprite->source_key,
               sprite->expected_revision,
               initial_value);
}

bool gui_text_edit_begin_target_out_path(
    const gui_target_ref *target,
    const char *initial_value) {
    return target &&
           text_edit_begin(
               GUI_TEXT_EDIT_TARGET_OUT_PATH,
               target->target_id, target->atlas_id,
               tp_id128_nil(), NULL,
               target->expected_revision,
               initial_value);
}

bool gui_text_edit_update(const char *value) {
    gui_draft_owner *draft = &s_actions.draft;
    if (draft->family != GUI_DRAFT_TEXT ||
        (draft->lifecycle.phase != GUI_EDIT_EDITING &&
         draft->lifecycle.phase != GUI_EDIT_CONFLICTED)) {
        set_status_ex(
            STATUS_WARNING,
            "No editable text draft is active.");
        return false;
    }
    return text_value_copy(
        draft->text.value,
        draft->text.kind ==
                GUI_TEXT_EDIT_TARGET_OUT_PATH
            ? sizeof draft->text.value
            : TP_SRCKEY_MAX,
        value, "GUI text edit");
}

const char *gui_text_edit_value(void) {
    return s_actions.draft.family == GUI_DRAFT_TEXT &&
                   s_actions.draft.lifecycle.phase != GUI_EDIT_IDLE
               ? s_actions.draft.text.value
               : NULL;
}

bool gui_target_path_edit_matches(
    const gui_target_ref *target) {
    return target &&
           draft_matches(
               GUI_DRAFT_TEXT,
               GUI_TEXT_EDIT_TARGET_OUT_PATH, 0,
               target->atlas_id, target->target_id,
               tp_id128_nil(), NULL);
}

bool gui_inline_text_edit_active(void) {
    return s_actions.draft.family == GUI_DRAFT_TEXT &&
           s_actions.draft.lifecycle.phase != GUI_EDIT_IDLE &&
           s_actions.draft.text.kind !=
               GUI_TEXT_EDIT_TARGET_OUT_PATH;
}

/* Local heap strdup (POSIX strdup is not ISO C17). NULL treated as ""; NULL on OOM. */
char *gui_actions__strdup(const char *s) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s) + 1U;
    char *c = (char *)malloc(n);
    if (c) {
        memcpy(c, s, n);
    }
    return c;
}

void gui_actions__frame_refs_dispose(tp_op_sprite_ref *frames, int count) {
    if (!frames) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        free(frames[i].src_key);
    }
    free(frames);
}

tp_op_sprite_ref *gui_actions__frame_refs_copy(const tp_op_sprite_ref *frames,
                                         int count) {
    if (!frames || count <= 0) {
        return NULL;
    }
    tp_op_sprite_ref *copy = calloc((size_t)count, sizeof *copy);
    if (!copy) {
        return NULL;
    }
    for (int i = 0; i < count; ++i) {
        if (!frames[i].src_key) {
            gui_actions__frame_refs_dispose(copy, count);
            return NULL;
        }
        copy[i].source_id = frames[i].source_id;
        copy[i].src_key = gui_actions__strdup(frames[i].src_key);
        if (!copy[i].src_key) {
            gui_actions__frame_refs_dispose(copy, count);
            return NULL;
        }
    }
    return copy;
}

/* Begins (or continues) the one draft for `family`/`target_id`/component.
 * `*out_fresh` reports whether a NEW draft was started, so the caller knows when
 * to stamp its component metadata -- it cannot be derived from the phase before
 * the call, because a sibling-field blur resolves the previous draft in here. */
static bool component_draft_begin(
    gui_draft_family family, tp_id128 target_id,
    int64_t expected_revision, bool same_component,
    const char *what, bool *out_fresh) {
    if (out_fresh) {
        *out_fresh = false;
    }
    if (!ensure_draft_owner()) {
        return false;
    }
    gui_draft_owner *draft = &s_actions.draft;
    gui_edit_state *edit = &draft->lifecycle;
    if (edit->phase != GUI_EDIT_IDLE &&
        (draft->family != family ||
         !tp_id128_eq(edit->target_id, target_id) ||
         !same_component)) {
        /* Spec §12.4 "Blur": moving to a SIBLING field is the active draft's
         * gesture boundary, so the draft submits instead of blocking forever on
         * an explicit Enter/Escape. Only a draft that cannot reach a terminal
         * answer (uncertain submit, or a conflict awaiting Apply Mine/Discard)
         * still blocks the new edit. */
        if (edit->phase != GUI_EDIT_EDITING) {
            set_status_ex(
                STATUS_WARNING,
                "Finish or discard the active edit before editing another field.");
            return false;
        }
        /* Settings-panel widgets raise these intents while DECLARING, inside the
         * pinned observation, where no mutation may run. There the blur is
         * requested as a gesture commit and lands at the next between-frame
         * boundary -- the same deferral every other in-frame edit uses. A caller
         * already at a safe boundary submits immediately. */
        if (gui_project_frame_is_pinned()) {
            gui_request_gesture_commit();
            set_status_ex(
                STATUS_WARNING,
                "Submitting the previous edit -- repeat this change once it lands.");
            return false;
        }
        if (!gui_actions__submit_draft()) {
            set_status_ex(
                STATUS_WARNING,
                "Finish or discard the active edit before editing another field.");
            return false;
        }
        /* The blur just advanced the model, so the caller's expected_revision --
         * read from the snapshot BEFORE this call -- is stale by construction.
         * The new draft's base is the revision the blur itself produced; keeping
         * the caller's would conflict the new draft against our own commit. */
        const tp_session_snapshot *after = gui_project_snapshot();
        if (!after) {
            set_status_ex(
                STATUS_WARNING,
                "No session is available for this edit.");
            return false;
        }
        expected_revision =
            tp_session_snapshot_revision(after);
    }
    if (edit->phase == GUI_EDIT_IDLE) {
        tp_id128 draft_id = tp_id128_nil();
        if (!edit_id_generate(&draft_id, what)) {
            return false;
        }
        tp_error error = {{0}};
        const tp_status status = gui_edit_begin(
            edit, target_id, expected_revision,
            draft_id, &error);
        if (status != TP_STATUS_OK) {
            set_statusf_ex(
                STATUS_WARNING, "%s: %s", what,
                error.msg[0] ? error.msg
                             : tp_status_str(status));
            return false;
        }
        draft->family = family;
        if (out_fresh) {
            *out_fresh = true;
        }
        return true;
    }
    if (edit->phase != GUI_EDIT_EDITING &&
        edit->phase != GUI_EDIT_CONFLICTED) {
        set_status_ex(
            STATUS_WARNING,
            "The edit is awaiting its exact session result.");
        return false;
    }
    return true;
}

static bool sprite_edit_matches(
    const gui_sprite_ref *sprite,
    gui_sprite_edit_kind kind, int component) {
    return sprite &&
           draft_matches(
               GUI_DRAFT_SPRITE, (int)kind, component,
               sprite->atlas_id, sprite->source_id,
               sprite->source_id, sprite->source_key);
}

static bool edit_sprite_component(
    const gui_sprite_ref *sprite,
    gui_sprite_edit_kind kind, int component,
    int integer, float real) {
    if (!sprite || tp_id128_is_nil(sprite->atlas_id) ||
        tp_id128_is_nil(sprite->source_id) || !sprite->source_key ||
        sprite->source_key[0] == '\0') {
        return false;
    }
    const size_t key_length = strlen(sprite->source_key);
    if (key_length >= sizeof s_actions.draft.sprite.source_key) {
        set_status_ex(
            STATUS_WARNING,
            "Sprite edit rejected: source key exceeds the GUI limit.");
        return false;
    }
    bool fresh = false;
    if (!component_draft_begin(
            GUI_DRAFT_SPRITE, sprite->source_id,
            sprite->expected_revision,
            sprite_edit_matches(sprite, kind, component),
            "Could not create the sprite draft identity",
            &fresh)) {
        return false;
    }
    gui_draft_owner *draft = &s_actions.draft;
    if (fresh) {
        draft->sprite.kind = kind;
        draft->sprite.atlas_id = sprite->atlas_id;
        draft->sprite.source_id = sprite->source_id;
        draft->sprite.component = component;
        memcpy(
            draft->sprite.source_key,
            sprite->source_key, key_length + 1U);
    }
    draft->sprite.integer = integer;
    draft->sprite.real = real;
    return true;
}

void gui_edit_sprite_origin(
    const gui_sprite_ref *sprite, int axis, float value) {
    if (axis < 0 || axis > 1) {
        set_status_ex(
            STATUS_WARNING,
            "Sprite origin edit rejected: invalid axis.");
        return;
    }
    (void)edit_sprite_component(
        sprite, GUI_SPRITE_EDIT_ORIGIN,
        axis, 0, value);
}
void gui_edit_sprite_slice9(
    const gui_sprite_ref *sprite, int component, int value) {
    if (component < 0 || component >= 4) {
        set_status_ex(
            STATUS_WARNING,
            "Sprite slice9 edit rejected: invalid component.");
        return;
    }
    (void)edit_sprite_component(
        sprite, GUI_SPRITE_EDIT_SLICE9,
        component, value, 0.0F);
}
void gui_edit_sprite_override(
    const gui_sprite_ref *sprite,
    gui_sprite_ov component, int value) {
    if (component < GUI_SPRITE_OV_SHAPE ||
        component > GUI_SPRITE_OV_EXTRUDE) {
        set_status_ex(
            STATUS_WARNING,
            "Sprite override edit rejected: invalid component.");
        return;
    }
    (void)edit_sprite_component(
        sprite, GUI_SPRITE_EDIT_OVERRIDE,
        (int)component, value, 0.0F);
}

bool gui_sprite_edit_value(
    const gui_sprite_ref *sprite,
    gui_sprite_edit_kind kind, int component,
    int *integer, float *real) {
    if (!sprite_edit_matches(sprite, kind, component)) {
        return false;
    }
    if (integer) {
        *integer = s_actions.draft.sprite.integer;
    }
    if (real) {
        *real = s_actions.draft.sprite.real;
    }
    return true;
}

/* An animation edit without a stable identity is rejected before it can reach
 * the one queue: the drain has no way to name its target. */
static bool animation_intent_push(gui_intent *intent) {
    if (tp_id128_is_nil(intent->payload.animation_edit.animation.atlas_id) ||
        tp_id128_is_nil(
            intent->payload.animation_edit.animation.animation_id)) {
        gui_actions__frame_refs_dispose(
            intent->payload.animation_edit.frames,
            intent->payload.animation_edit.frame_count);
        return false;
    }
    return gui_actions__intent_push(intent);
}

static bool animation_edit_matches(
    const gui_animation_ref *animation,
    gui_animation_edit_kind kind, int component) {
    return animation &&
           draft_matches(
               GUI_DRAFT_ANIMATION, (int)kind, component,
               animation->atlas_id,
               animation->animation_id,
               tp_id128_nil(), NULL);
}

static bool edit_animation_component(
    const gui_animation_ref *animation,
    gui_animation_edit_kind kind, int component,
    int integer, float real, bool flag) {
    if (!animation ||
        tp_id128_is_nil(animation->atlas_id) ||
        tp_id128_is_nil(animation->animation_id)) {
        return false;
    }
    bool fresh = false;
    if (!component_draft_begin(
            GUI_DRAFT_ANIMATION,
            animation->animation_id,
            animation->expected_revision,
            animation_edit_matches(
                animation, kind, component),
            "Could not create the animation draft identity",
            &fresh)) {
        return false;
    }
    gui_draft_owner *draft = &s_actions.draft;
    if (fresh) {
        draft->animation.kind = kind;
        draft->animation.atlas_id =
            animation->atlas_id;
        draft->animation.component = component;
    }
    draft->animation.integer = integer;
    draft->animation.real = real;
    draft->animation.flag = flag;
    return true;
}

void gui_edit_anim_fps(
    const gui_animation_ref *animation, float fps) {
    (void)edit_animation_component(
        animation, GUI_ANIMATION_EDIT_FPS,
        0, 0, fps, false);
}

void gui_edit_anim_playback(
    const gui_animation_ref *animation, int playback) {
    (void)edit_animation_component(
        animation, GUI_ANIMATION_EDIT_PLAYBACK,
        0, playback, 0.0F, false);
}

void gui_edit_anim_flip(
    const gui_animation_ref *animation,
    int axis, bool value) {
    if (axis < 0 || axis > 1) {
        set_status_ex(
            STATUS_WARNING,
            "Animation flip edit rejected: invalid axis.");
        return;
    }
    (void)edit_animation_component(
        animation, GUI_ANIMATION_EDIT_FLIP,
        axis, 0, 0.0F, value);
}

bool gui_animation_edit_value(
    const gui_animation_ref *animation,
    gui_animation_edit_kind kind, int component,
    int *integer, float *real) {
    if (!animation_edit_matches(
            animation, kind, component)) {
        return false;
    }
    if (integer) {
        *integer =
            kind == GUI_ANIMATION_EDIT_FLIP
                ? (s_actions.draft.animation.flag ? 1 : 0)
                : s_actions.draft.animation.integer;
    }
    if (real) {
        *real = s_actions.draft.animation.real;
    }
    return true;
}
void gui_edit_anim_frame_remove(const gui_animation_ref *animation, int frame_index) {
    gui_intent intent = {.kind = GUI_INTENT_ANIM_FRAME_REMOVE};
    if (animation) intent.payload.animation_edit.animation = *animation;
    intent.payload.animation_edit.first = frame_index;
    (void)animation_intent_push(&intent);
}
void gui_edit_anim_frame_move(const gui_animation_ref *animation, int frame_index, int delta) {
    gui_intent intent = {.kind = GUI_INTENT_ANIM_FRAME_MOVE};
    if (animation) {
        intent.payload.animation_edit.animation = *animation;
        intent.payload.animation_edit.follow_selection =
            gui_view_animation_frame(gui_project_snapshot()) == frame_index &&
            tp_id128_eq(gui_view_atlas_id(), animation->atlas_id) &&
            tp_id128_eq(gui_view_animation_id(), animation->animation_id);
    }
    intent.payload.animation_edit.first = frame_index;
    intent.payload.animation_edit.second = delta;
    (void)animation_intent_push(&intent);
}
static void edit_capture_target(gui_intent *edit,
                                 const gui_target_ref *target) {
    if (target) {
        edit->payload.target_edit.atlas_id = target->atlas_id;
        edit->payload.target_edit.target_id = target->target_id;
        edit->payload.target_edit.expected_revision = target->expected_revision;
    }
}

static bool edit_copy_exporter_id(char out[TP_EXPORTER_ID_MAX],
                                  const char *exporter_id) {
    tp_error error = {0};
    const tp_status status = tp_exporter_id_validate(exporter_id, &error);
    if (status != TP_STATUS_OK) {
        set_statusf_ex(STATUS_ERROR, "Export target edit rejected: %s",
                       error.msg);
        return false;
    }
    memcpy(out, exporter_id, strlen(exporter_id) + 1U);
    return true;
}

void gui_edit_target_enabled(const gui_target_ref *target, bool enabled) {
    gui_intent e = {.kind = GUI_INTENT_TARGET_ENABLED};
    edit_capture_target(&e, target);
    e.payload.target_edit.enabled = enabled;
    (void)gui_actions__intent_push(&e);
}

void gui_edit_target_exporter(const gui_target_ref *target,
                              const char *exporter_id) {
    gui_intent e = {.kind = GUI_INTENT_TARGET_EXPORTER};
    edit_capture_target(&e, target);
    if (!edit_copy_exporter_id(
            e.payload.target_edit.exporter_id, exporter_id)) {
        return;
    }
    (void)gui_actions__intent_push(&e);
}

/* Enqueue an "add frames" edit carrying a COPY of canonical selection refs (F1). "Add frames" used to
 * commit synchronously from inside declare_animation_editor, which clone-swaps + frees the project
 * under the live `an`/`a` the same declare invocation keeps dereferencing (frame_count, frames[].
 * name) -> a use-after-free on an ordinary click. Deferring it (drain replays via
 * gui_project_anim_add_frames at frame top, no live pointer held) closes that last synchronous
 * commit; refs are copied NOW so a selection change before the drain cannot alter what lands. */
void gui_edit_anim_add_frames(const gui_animation_ref *animation,
                              const tp_op_sprite_ref *frames, int count) {
    if (count <= 0) {
        return;
    }
    gui_intent intent = {.kind = GUI_INTENT_ANIM_ADD_FRAMES};
    if (animation) intent.payload.animation_edit.animation = *animation;
    intent.payload.animation_edit.frames =
        gui_actions__frame_refs_copy(frames, count);
    if (!intent.payload.animation_edit.frames) {
        set_status_ex(STATUS_ERROR, "Out of memory: add-frames not applied.");
        return;
    }
    intent.payload.animation_edit.frame_count = count;
    (void)animation_intent_push(&intent);
}

/* Builds the ONE typed operation payload this draft submits and hands it to its
 * concrete mutation owner. Nothing here knows a field name: the row does. */
static tp_status submit_draft_operation(
    const gui_draft_owner *draft,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal,
    tp_error *error) {
    draft_key key;
    draft_resolve(draft, &key);
    const draft_descriptor *row = draft_row(&key);
    if (!row) {
        return tp_error_set(
            error, TP_STATUS_INVALID_ARGUMENT, "%s",
            "unknown GUI draft component");
    }
    const int64_t base_revision =
        draft->lifecycle.base_revision;
    draft_value value;
    draft_current_value(draft, row, &value);
    if (key.family == GUI_DRAFT_TEXT) {
        const gui_text_ref ref = {
            key.atlas_id, key.entity_id, key.source_id,
            key.source_key, base_revision};
        return gui_project_submit_text(
            row->op_kind, &ref, value.text, identity,
            transaction_id, terminal, error);
    }
    if (key.family == GUI_DRAFT_ATLAS_SCALAR) {
        tp_op_atlas_settings settings = {0};
        settings.mask = row->field_mask;
        draft_write_payload(row, &settings, &value);
        return gui_project_submit_atlas_settings(
            key.atlas_id, base_revision, &settings,
            identity, transaction_id, terminal, error);
    }
    if (key.family == GUI_DRAFT_ANIMATION) {
        const gui_animation_ref animation = {
            key.atlas_id, key.entity_id, base_revision};
        tp_op_anim_settings settings = {0};
        settings.mask = row->field_mask;
        draft_write_payload(row, &settings, &value);
        return gui_project_submit_animation_settings(
            &animation, &settings, identity,
            transaction_id, terminal, error);
    }
    const gui_sprite_ref sprite = {
        key.atlas_id, key.source_id, key.source_key,
        base_revision};
    /* Origin and slice9 keep dedicated entry points: their untouched sibling
     * components are read from the LIVE snapshot by the mutation owner. */
    if (key.kind == GUI_SPRITE_EDIT_ORIGIN) {
        return gui_project_submit_sprite_origin(
            &sprite, key.component, value.real, identity,
            transaction_id, terminal, error);
    }
    if (key.kind == GUI_SPRITE_EDIT_SLICE9) {
        return gui_project_submit_sprite_slice9(
            &sprite, key.component, value.integer,
            identity, transaction_id, terminal, error);
    }
    tp_op_sprite_set settings = {0};
    settings.mask = row->field_mask;
    draft_write_payload(row, &settings, &value);
    return gui_project_submit_sprite_settings(
        &sprite, &settings, identity, transaction_id,
        terminal, error);
}

static bool submit_draft(bool apply_mine) {
    gui_draft_owner *draft = &s_actions.draft;
    gui_edit_state *edit = &draft->lifecycle;
    if (edit->phase == GUI_EDIT_IDLE) {
        return true;
    }
    if ((!apply_mine &&
         edit->phase != GUI_EDIT_EDITING) ||
        (apply_mine &&
         edit->phase != GUI_EDIT_CONFLICTED)) {
        set_status_ex(
            STATUS_WARNING,
            edit->phase == GUI_EDIT_CONFLICTED
                ? "Choose Apply Mine or Discard for the conflicted edit."
                : "The edit is still awaiting its exact session result.");
        return false;
    }

    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const int64_t revision =
        snapshot
            ? tp_session_snapshot_revision(snapshot)
            : -1;
    if (revision < 0) {
        set_status_ex(
            STATUS_ERROR,
            "GUI draft cannot resolve a valid current revision.");
        return false;
    }

    /* Preflight, BEFORE the FSM leaves EDITING: everything that would reject the
     * submit before the session ever sees it is checked here, so SUBMITTING is
     * entered only for a submit that actually reaches the session. A rejection
     * here leaves the draft EDITING/CONFLICTED -- still visible, still
     * discardable -- instead of stranding it in the uncertain phase. (The
     * receipt-postcondition branch below is the backstop for the rest.) */
    if (!draft_target_present(draft, snapshot)) {
        set_status_ex(
            STATUS_WARNING,
            "The edited item no longer exists -- discard the edit.");
        return false;
    }

    tp_id128 transaction = tp_id128_nil();
    if (!edit_id_generate(
            &transaction,
            "Could not create the GUI edit transaction")) {
        return false;
    }
    char transaction_id[33];
    edit_transaction_id(transaction, transaction_id);

    tp_error error = {{0}};
    tp_status status = TP_STATUS_OK;
    if (apply_mine) {
        status = gui_edit_apply_mine(
            edit, revision,
            draft_target_present(draft, snapshot),
            transaction_id, &error);
    } else {
        status = gui_edit_commit(
            edit,
            draft_is_net_zero(draft, snapshot),
            transaction_id, &error);
    }
    if (status != TP_STATUS_OK) {
        set_statusf_ex(
            STATUS_WARNING, "GUI edit not submitted: %s",
            error.msg[0] ? error.msg
                         : tp_status_str(status));
        return false;
    }
    if (edit->phase == GUI_EDIT_IDLE) {
        draft_clear_if_idle(draft);
        return true;
    }

    const gui_session_submit_identity identity =
        draft_identity(edit);
    gui_session_submit_terminal terminal = {0};
    status = submit_draft_operation(
        draft, identity, transaction_id,
        &terminal, &error);

    if (edit->phase == GUI_EDIT_SUBMITTING) {
        const tp_session_snapshot *after =
            gui_project_snapshot();
        const int64_t current_revision =
            after
                ? tp_session_snapshot_revision(after)
                : edit->base_revision;
        tp_error reduce_error = {{0}};
        const bool exact_owner =
            terminal.transaction_id[0] != '\0' &&
            strcmp(
                edit->submitted_transaction_id,
                terminal.transaction_id) == 0 &&
            tp_id128_eq(
                edit->view_id,
                terminal.identity.origin_view_id) &&
            tp_id128_eq(
                edit->draft_instance_id,
                terminal.identity.draft_instance_id);
        /* No receipt for THIS draft: the session never accepted the submit (an
         * empty transaction id) or answered something else. Either way the draft
         * is not uncertain, so it must leave SUBMITTING -- back to EDITING, or
         * CONFLICTED when the model moved under it. Without this it would be
         * stranded: Escape/Discard refuse, Apply Mine needs CONFLICTED, and every
         * outer action that requires a terminal draft is blocked forever. */
        const tp_status reduce_status =
            exact_owner
                ? gui_edit_submit_result(
                      edit, true, terminal.status,
                      terminal.committed, terminal.no_change,
                      terminal.committed,
                      terminal.revision, current_revision,
                      &reduce_error)
                : gui_edit_submit_result(
                      edit, true,
                      status != TP_STATUS_OK
                          ? status
                          : TP_STATUS_INVALID_ARGUMENT,
                      false, false, false, 0,
                      current_revision, &reduce_error);
        if (reduce_status != TP_STATUS_OK) {
            set_statusf_ex(
                STATUS_ERROR,
                "GUI submit receipt was invalid: %s",
                reduce_error.msg[0]
                    ? reduce_error.msg
                    : tp_status_str(reduce_status));
            return false;
        }
        draft_clear_if_idle(draft);
    }
    if (status != TP_STATUS_OK) {
        set_statusf_ex(
            STATUS_WARNING, "GUI edit rejected: %s",
            error.msg[0] ? error.msg
                         : tp_status_str(status));
        return false;
    }
    if (edit->phase != GUI_EDIT_IDLE) {
        set_status_ex(
            STATUS_WARNING,
            "GUI edit is waiting for its exact session result.");
        return false;
    }
    return true;
}

bool gui_actions__submit_draft(void) {
    return submit_draft(false);
}

bool gui_actions__apply_draft_mine(void) {
    return submit_draft(true);
}

void gui_actions__discard_edits(void) {
    gui_draft_discard();
    s_actions.draft_apply_mine = false;
    gui_actions__discard_deferred_edits();
    s_actions.gesture_commit = false;
}

/* Set by a view widget when its gesture ends. The next frame submits the one active draft. */
void gui_request_gesture_commit(void) { s_actions.gesture_commit = true; }
// #endregion
