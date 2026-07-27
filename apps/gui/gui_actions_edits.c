#include "gui_actions_internal.h"
#include "gui_project.h"

#include <limits.h>
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

static bool draft_target_present(
    const gui_draft_owner *draft,
    const tp_session_snapshot *snapshot) {
    if (!draft || !snapshot ||
        draft->lifecycle.phase == GUI_EDIT_IDLE) {
        return false;
    }
    if (draft->family == GUI_DRAFT_ATLAS_SCALAR ||
        (draft->family == GUI_DRAFT_TEXT &&
         draft->text.kind == GUI_TEXT_EDIT_ATLAS_NAME)) {
        return tp_session_snapshot_atlas_by_id(
                   snapshot, draft->lifecycle.target_id) != NULL;
    }
    if (draft->family == GUI_DRAFT_TEXT) {
        switch (draft->text.kind) {
            case GUI_TEXT_EDIT_ATLAS_NAME:
                return false;
            case GUI_TEXT_EDIT_ANIMATION_NAME:
                return tp_session_snapshot_animation_by_id(
                           snapshot, draft->text.atlas_id,
                           draft->lifecycle.target_id) != NULL;
            case GUI_TEXT_EDIT_SPRITE_RENAME:
                return tp_session_snapshot_source_by_id(
                           snapshot, draft->text.atlas_id,
                           draft->text.source_id) != NULL;
            case GUI_TEXT_EDIT_TARGET_OUT_PATH:
                return tp_session_snapshot_target_by_id(
                           snapshot, draft->text.atlas_id,
                           draft->lifecycle.target_id) != NULL;
        }
    }
    if (draft->family == GUI_DRAFT_SPRITE) {
        return tp_session_snapshot_source_by_id(
                   snapshot, draft->sprite.atlas_id,
                   draft->sprite.source_id) != NULL;
    }
    if (draft->family == GUI_DRAFT_ANIMATION) {
        return tp_session_snapshot_animation_by_id(
                   snapshot, draft->animation.atlas_id,
                   draft->lifecycle.target_id) != NULL;
    }
    return false;
}

static void reduce_draft_observation(
    void *context,
    const tp_session_observation *observation,
    uint64_t instance_generation) {
    (void)instance_generation;
    gui_draft_owner *draft = context;
    gui_edit_state *edit =
        draft ? &draft->lifecycle : NULL;
    if (!edit || edit->phase == GUI_EDIT_IDLE) {
        return;
    }
    const tp_session_snapshot *snapshot =
        tp_session_observation_snapshot(observation);
    const int64_t revision =
        snapshot
            ? tp_session_snapshot_revision(snapshot)
            : edit->base_revision;
    const bool target_present =
        snapshot ? draft_target_present(draft, snapshot)
                 : edit->target_present;
    tp_error error = {{0}};
    if (tp_session_observation_resync_required(observation)) {
        gui_session_submit_terminal terminal = {0};
        const bool exact_terminal =
            edit->phase == GUI_EDIT_SUBMITTING &&
            gui_project_submit_receipt_query(
                edit->submitted_transaction_id,
                draft_identity(edit),
                &terminal) &&
            (terminal.no_change ||
             terminal.echo_state ==
                 GUI_SESSION_SUBMIT_ECHO_OBSERVED);
        (void)gui_edit_resync(
            edit, revision, target_present,
            exact_terminal, &error);
        draft_clear_if_idle(draft);
        return;
    }

    const size_t event_count =
        tp_session_observation_event_count(observation);
    for (size_t index = 0U;
         edit->phase != GUI_EDIT_IDLE &&
         index < event_count;
         ++index) {
        const tp_session_event *source =
            tp_session_observation_event_at(
                observation, index);
        if (!source ||
            source->revision_after ==
                source->revision_before) {
            continue;
        }
        bool exact_echo = false;
        if (edit->phase == GUI_EDIT_SUBMITTING &&
            strcmp(
                edit->submitted_transaction_id,
                source->transaction_id) == 0) {
            gui_session_submit_terminal terminal = {0};
            exact_echo =
            gui_project_submit_receipt_query(
                source->transaction_id,
                draft_identity(edit),
                &terminal);
        }
        (void)gui_edit_model_revision(
            edit, source->revision_after,
            target_present, exact_echo, &error);
    }
    draft_clear_if_idle(draft);
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
    if (!s_actions.draft_reducer_registered) {
        tp_error error = {{0}};
        const tp_status status =
            gui_project_register_observation_reducer(
                reduce_draft_observation,
                &s_actions.draft, &error);
        if (status != TP_STATUS_OK) {
            set_statusf_ex(
                STATUS_ERROR,
                "Could not observe GUI drafts: %s",
                error.msg[0] ? error.msg
                             : tp_status_str(status));
            return false;
        }
        s_actions.draft_reducer_registered = true;
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
    const gui_draft_owner *draft = &s_actions.draft;
    return draft->family == GUI_DRAFT_ATLAS_SCALAR &&
           draft->lifecycle.phase != GUI_EDIT_IDLE &&
           tp_id128_eq(
               draft->lifecycle.target_id, atlas_id) &&
           draft->atlas.component == field;
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

gui_edit_phase gui_draft_phase(void) {
    return s_actions.draft.lifecycle.phase;
}

bool gui_draft_can_apply(void) {
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
    const gui_draft_owner *draft = &s_actions.draft;
    return target &&
           draft->family == GUI_DRAFT_TEXT &&
           draft->lifecycle.phase != GUI_EDIT_IDLE &&
           draft->text.kind ==
               GUI_TEXT_EDIT_TARGET_OUT_PATH &&
           tp_id128_eq(
               draft->text.atlas_id,
               target->atlas_id) &&
           tp_id128_eq(
               draft->lifecycle.target_id,
               target->target_id);
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

static bool target_intent_push(target_edit_intent *e) {
    if (s_actions.target_intent_count == s_actions.target_intent_cap) {
        int nc = s_actions.target_intent_cap ? s_actions.target_intent_cap * 2 : 8;
        target_edit_intent *ne = (target_edit_intent *)realloc(
            s_actions.target_intents, (size_t)nc * sizeof *ne);
        if (!ne) {
            set_status_ex(STATUS_ERROR, "Out of memory: this edit could not be queued (change not applied).");
            return false;
        }
        s_actions.target_intents = ne;
        s_actions.target_intent_cap = nc;
    }
    s_actions.target_intents[s_actions.target_intent_count++] = *e;
    return true;
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
    const gui_draft_owner *draft = &s_actions.draft;
    return sprite &&
           draft->family == GUI_DRAFT_SPRITE &&
           draft->lifecycle.phase != GUI_EDIT_IDLE &&
           tp_id128_eq(
               draft->lifecycle.target_id,
               sprite->source_id) &&
           tp_id128_eq(
               draft->sprite.atlas_id,
               sprite->atlas_id) &&
           draft->sprite.kind == kind &&
           draft->sprite.component == component &&
           strcmp(
               draft->sprite.source_key,
               sprite->source_key ? sprite->source_key : "") == 0;
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

static bool animation_intent_push(animation_edit_intent *intent) {
    if (!intent || tp_id128_is_nil(intent->animation.atlas_id) ||
        tp_id128_is_nil(intent->animation.animation_id)) {
        return false;
    }
    if (s_actions.animation_intent_count == s_actions.animation_intent_cap) {
        const int capacity = s_actions.animation_intent_cap ? s_actions.animation_intent_cap * 2 : 8;
        animation_edit_intent *intents = (animation_edit_intent *)realloc(
            s_actions.animation_intents, (size_t)capacity * sizeof *intents);
        if (!intents) {
            gui_actions__frame_refs_dispose(intent->frames, intent->frame_count);
            set_status_ex(STATUS_ERROR,
                          "Out of memory: this animation edit could not be queued (change not applied).");
            return false;
        }
        s_actions.animation_intents = intents;
        s_actions.animation_intent_cap = capacity;
    }
    s_actions.animation_intents[s_actions.animation_intent_count++] = *intent;
    return true;
}

static bool animation_edit_matches(
    const gui_animation_ref *animation,
    gui_animation_edit_kind kind, int component) {
    const gui_draft_owner *draft = &s_actions.draft;
    return animation &&
           draft->family == GUI_DRAFT_ANIMATION &&
           draft->lifecycle.phase != GUI_EDIT_IDLE &&
           tp_id128_eq(
               draft->lifecycle.target_id,
               animation->animation_id) &&
           tp_id128_eq(
               draft->animation.atlas_id,
               animation->atlas_id) &&
           draft->animation.kind == kind &&
           draft->animation.component == component;
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
    animation_edit_intent intent = {0};
    intent.kind = ANIMATION_INTENT_FRAME_REMOVE;
    if (animation) intent.animation = *animation;
    intent.first = frame_index;
    (void)animation_intent_push(&intent);
}
void gui_edit_anim_frame_move(const gui_animation_ref *animation, int frame_index, int delta) {
    animation_edit_intent intent = {0};
    intent.kind = ANIMATION_INTENT_FRAME_MOVE;
    if (animation) {
        intent.animation = *animation;
        intent.follow_selection =
            gui_view_animation_frame(gui_project_snapshot()) == frame_index &&
            tp_id128_eq(gui_view_atlas_id(), animation->atlas_id) &&
            tp_id128_eq(gui_view_animation_id(), animation->animation_id);
    }
    intent.first = frame_index;
    intent.second = delta;
    (void)animation_intent_push(&intent);
}
static void edit_capture_target(target_edit_intent *edit,
                                 const gui_target_ref *target) {
    if (target) {
        edit->atlas_id = target->atlas_id;
        edit->target_id = target->target_id;
        edit->expected_revision = target->expected_revision;
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
    target_edit_intent e = {0};
    e.kind = TARGET_INTENT_ENABLED;
    edit_capture_target(&e, target);
    e.enabled = enabled;
    (void)target_intent_push(&e);
}

void gui_edit_target_exporter(const gui_target_ref *target,
                              const char *exporter_id) {
    target_edit_intent e = {0};
    e.kind = TARGET_INTENT_EXPORTER;
    edit_capture_target(&e, target);
    if (!edit_copy_exporter_id(
            e.exporter_id, exporter_id)) {
        return;
    }
    (void)target_intent_push(&e);
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
    animation_edit_intent intent = {0};
    intent.kind = ANIMATION_INTENT_ADD_FRAMES;
    if (animation) intent.animation = *animation;
    intent.frames = gui_actions__frame_refs_copy(frames, count);
    if (!intent.frames) {
        set_status_ex(STATUS_ERROR, "Out of memory: add-frames not applied.");
        return;
    }
    intent.frame_count = count;
    (void)animation_intent_push(&intent);
}

static bool atlas_draft_is_net_zero(
    const gui_draft_owner *draft,
    const tp_snapshot_atlas *atlas) {
    if (!atlas) {
        return false;
    }
    switch (draft->atlas.component) {
        case GUI_ATLAS_MAX_SIZE:
            return draft->atlas.integer == atlas->max_size;
        case GUI_ATLAS_PADDING:
            return draft->atlas.integer == atlas->padding;
        case GUI_ATLAS_MARGIN:
            return draft->atlas.integer == atlas->margin;
        case GUI_ATLAS_EXTRUDE:
            return draft->atlas.integer == atlas->extrude;
        case GUI_ATLAS_ALPHA_THRESHOLD:
            return draft->atlas.integer ==
                   atlas->alpha_threshold;
        case GUI_ATLAS_MAX_VERTICES:
            return draft->atlas.integer ==
                   atlas->max_vertices;
        case GUI_ATLAS_SHAPE:
            return draft->atlas.integer == atlas->shape;
        case GUI_ATLAS_ALLOW_TRANSFORM:
            return draft->atlas.integer ==
                   (atlas->allow_transform ? 1 : 0);
        case GUI_ATLAS_POWER_OF_TWO:
            return draft->atlas.integer ==
                   (atlas->power_of_two ? 1 : 0);
        case GUI_ATLAS_PIXELS_PER_UNIT:
            return draft->atlas.real ==
                   atlas->pixels_per_unit;
    }
    return false;
}

static bool text_draft_is_net_zero(
    const gui_draft_owner *draft,
    const tp_session_snapshot *snapshot) {
    const char *committed = NULL;
    switch (draft->text.kind) {
        case GUI_TEXT_EDIT_ATLAS_NAME: {
            const tp_snapshot_atlas *atlas =
                tp_session_snapshot_atlas_by_id(
                    snapshot,
                    draft->lifecycle.target_id);
            committed = atlas ? atlas->name : NULL;
            break;
        }
        case GUI_TEXT_EDIT_ANIMATION_NAME: {
            const tp_snapshot_animation *animation =
                tp_session_snapshot_animation_by_id(
                    snapshot, draft->text.atlas_id,
                    draft->lifecycle.target_id);
            committed = animation ? animation->name
                                  : NULL;
            break;
        }
        case GUI_TEXT_EDIT_SPRITE_RENAME: {
            const tp_snapshot_sprite *sprite =
                tp_session_snapshot_sprite_by_key(
                    snapshot, draft->text.atlas_id,
                    draft->text.source_id,
                    draft->text.source_key);
            committed =
                sprite && sprite->rename
                    ? sprite->rename
                    : "";
            break;
        }
        case GUI_TEXT_EDIT_TARGET_OUT_PATH: {
            const tp_snapshot_target *target =
                tp_session_snapshot_target_by_id(
                    snapshot, draft->text.atlas_id,
                    draft->lifecycle.target_id);
            committed = target ? target->out_path : NULL;
            break;
        }
    }
    return committed &&
           strcmp(committed, draft->text.value) == 0;
}

static bool sprite_draft_is_net_zero(
    const gui_draft_owner *draft,
    const tp_session_snapshot *snapshot) {
    const tp_snapshot_sprite *sprite =
        tp_session_snapshot_sprite_by_key(
            snapshot, draft->sprite.atlas_id,
            draft->sprite.source_id,
            draft->sprite.source_key);
    switch (draft->sprite.kind) {
        case GUI_SPRITE_EDIT_ORIGIN:
            return draft->sprite.real ==
                   (draft->sprite.component == 0
                        ? (sprite ? sprite->origin_x
                                  : TP_PROJECT_ORIGIN_DEFAULT)
                        : (sprite ? sprite->origin_y
                                  : TP_PROJECT_ORIGIN_DEFAULT));
        case GUI_SPRITE_EDIT_SLICE9:
            return draft->sprite.integer ==
                   (sprite
                        ? sprite->slice9_lrtb[
                              draft->sprite.component]
                        : 0);
        case GUI_SPRITE_EDIT_OVERRIDE: {
            int committed = TP_PROJECT_OV_INHERIT;
            if (sprite) {
                switch ((gui_sprite_ov)
                            draft->sprite.component) {
                    case GUI_SPRITE_OV_SHAPE:
                        committed = sprite->override_shape;
                        break;
                    case GUI_SPRITE_OV_ROTATE:
                        committed =
                            sprite->override_allow_rotate;
                        break;
                    case GUI_SPRITE_OV_MAXVERT:
                        committed =
                            sprite->override_max_vertices;
                        break;
                    case GUI_SPRITE_OV_MARGIN:
                        committed = sprite->override_margin;
                        break;
                    case GUI_SPRITE_OV_EXTRUDE:
                        committed = sprite->override_extrude;
                        break;
                }
            }
            return draft->sprite.integer == committed;
        }
    }
    return false;
}

static bool animation_draft_is_net_zero(
    const gui_draft_owner *draft,
    const tp_session_snapshot *snapshot) {
    const tp_snapshot_animation *animation =
        tp_session_snapshot_animation_by_id(
            snapshot, draft->animation.atlas_id,
            draft->lifecycle.target_id);
    if (!animation) {
        return false;
    }
    switch (draft->animation.kind) {
        case GUI_ANIMATION_EDIT_FPS:
            return draft->animation.real ==
                   animation->fps;
        case GUI_ANIMATION_EDIT_PLAYBACK:
            return draft->animation.integer ==
                   animation->playback;
        case GUI_ANIMATION_EDIT_FLIP:
            return draft->animation.flag ==
                   (draft->animation.component == 0
                        ? animation->flip_h
                        : animation->flip_v);
    }
    return false;
}

static bool draft_is_net_zero(
    const gui_draft_owner *draft,
    const tp_session_snapshot *snapshot) {
    if (!draft || !snapshot) {
        return false;
    }
    if (draft->family == GUI_DRAFT_ATLAS_SCALAR) {
        return atlas_draft_is_net_zero(
            draft,
            tp_session_snapshot_atlas_by_id(
                snapshot,
                draft->lifecycle.target_id));
    }
    if (draft->family == GUI_DRAFT_TEXT) {
        return text_draft_is_net_zero(
            draft, snapshot);
    }
    if (draft->family == GUI_DRAFT_SPRITE) {
        return sprite_draft_is_net_zero(
            draft, snapshot);
    }
    return draft->family == GUI_DRAFT_ANIMATION &&
           animation_draft_is_net_zero(
               draft, snapshot);
}

static tp_status submit_draft_operation(
    const gui_draft_owner *draft,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal,
    tp_error *error) {
    const gui_edit_state *edit = &draft->lifecycle;
    if (draft->family == GUI_DRAFT_ATLAS_SCALAR) {
        return gui_project_submit_atlas_setting(
            edit->target_id, edit->base_revision,
            draft->atlas.component,
            draft->atlas.component ==
                    GUI_ATLAS_PIXELS_PER_UNIT
                ? 0
                : draft->atlas.integer,
            draft->atlas.component ==
                    GUI_ATLAS_PIXELS_PER_UNIT
                ? draft->atlas.real
                : 0.0F,
            identity, transaction_id, terminal,
            error);
    }
    if (draft->family == GUI_DRAFT_SPRITE) {
        const gui_sprite_ref sprite = {
            draft->sprite.atlas_id,
            draft->sprite.source_id,
            draft->sprite.source_key,
            edit->base_revision,
        };
        switch (draft->sprite.kind) {
            case GUI_SPRITE_EDIT_ORIGIN:
                return gui_project_submit_sprite_origin(
                    &sprite, draft->sprite.component,
                    draft->sprite.real, identity,
                    transaction_id, terminal, error);
            case GUI_SPRITE_EDIT_SLICE9:
                return gui_project_submit_sprite_slice9(
                    &sprite, draft->sprite.component,
                    draft->sprite.integer, identity,
                    transaction_id, terminal, error);
            case GUI_SPRITE_EDIT_OVERRIDE:
                return gui_project_submit_sprite_override(
                    &sprite,
                    (gui_sprite_ov)
                        draft->sprite.component,
                    draft->sprite.integer, identity,
                    transaction_id, terminal, error);
        }
    }
    if (draft->family == GUI_DRAFT_ANIMATION) {
        const gui_animation_ref animation = {
            draft->animation.atlas_id,
            edit->target_id,
            edit->base_revision,
        };
        switch (draft->animation.kind) {
            case GUI_ANIMATION_EDIT_FPS:
                return gui_project_submit_animation_fps(
                    &animation, draft->animation.real,
                    identity, transaction_id,
                    terminal, error);
            case GUI_ANIMATION_EDIT_PLAYBACK:
                return gui_project_submit_animation_playback(
                    &animation,
                    draft->animation.integer,
                    identity, transaction_id,
                    terminal, error);
            case GUI_ANIMATION_EDIT_FLIP:
                return gui_project_submit_animation_flip(
                    &animation,
                    draft->animation.component,
                    draft->animation.flag, identity,
                    transaction_id, terminal, error);
        }
    }
    if (draft->family != GUI_DRAFT_TEXT) {
        return tp_error_set(
            error, TP_STATUS_INVALID_ARGUMENT,
            "%s", "unknown GUI draft family");
    }
    switch (draft->text.kind) {
        case GUI_TEXT_EDIT_ATLAS_NAME:
            return gui_project_submit_atlas_name(
                edit->target_id, edit->base_revision,
                draft->text.value, identity,
                transaction_id, terminal, error);
        case GUI_TEXT_EDIT_ANIMATION_NAME: {
            const gui_animation_ref animation = {
                draft->text.atlas_id, edit->target_id,
                edit->base_revision};
            return gui_project_submit_animation_name(
                &animation, draft->text.value,
                identity, transaction_id, terminal,
                error);
        }
        case GUI_TEXT_EDIT_SPRITE_RENAME: {
            const gui_sprite_ref sprite = {
                draft->text.atlas_id,
                draft->text.source_id,
                draft->text.source_key,
                edit->base_revision};
            return gui_project_submit_sprite_name(
                &sprite, draft->text.value,
                identity, transaction_id, terminal,
                error);
        }
        case GUI_TEXT_EDIT_TARGET_OUT_PATH: {
            const gui_target_ref target = {
                draft->text.atlas_id,
                edit->target_id,
                edit->base_revision};
            return gui_project_submit_target_out_path(
                &target, draft->text.value,
                identity, transaction_id, terminal,
                error);
        }
    }
    return tp_error_set(
        error, TP_STATUS_INVALID_ARGUMENT,
        "%s", "unknown GUI draft family");
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
    if (revision < 0 || revision == INT64_MAX) {
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
            edit, revision, revision + 1,
            draft_target_present(draft, snapshot),
            transaction_id, &error);
    } else {
        status = gui_edit_commit(
            edit,
            draft_is_net_zero(draft, snapshot),
            transaction_id, revision + 1, &error);
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
                      terminal.echo_state ==
                          GUI_SESSION_SUBMIT_ECHO_OBSERVED,
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

/* Discrete commands captured in the same frame as a successful draft submit
 * advance only from that exact revision. Already-stale commands stay stale. */
void gui_actions__rebase_deferred_edits(
    int64_t revision_before,
    int64_t revision_after) {
    if (revision_after == revision_before) {
        return;
    }
    /* A submit commits exactly one transaction, but the observation refresh it
     * performs can also fold in FOREIGN commits (another client on the same live
     * session). More than one revision therefore means these intents really are
     * stale -- leave them stale so the session rejects them on expected_revision.
     * Not an invariant: a concurrent writer must never abort the GUI. */
    if (revision_after != revision_before + 1) {
        return;
    }
    for (int i = 0;
         i < s_actions.animation_intent_count;
         ++i) {
        if (s_actions.animation_intents[i]
                .animation.expected_revision ==
            revision_before) {
            s_actions.animation_intents[i]
                .animation.expected_revision =
                revision_after;
        }
    }
    for (int i = 0;
         i < s_actions.target_intent_count;
         ++i) {
        if (s_actions.target_intents[i]
                .expected_revision ==
            revision_before) {
            s_actions.target_intents[i]
                .expected_revision =
                revision_after;
        }
    }
}

/* Replays discrete structural/target commands after any draft prerequisite. */
void gui_actions__drain_edits(void) {
    for (int i = 0; i < s_actions.animation_intent_count; i++) {
        animation_edit_intent *intent = &s_actions.animation_intents[i];
        switch (intent->kind) {
            case ANIMATION_INTENT_FRAME_REMOVE:
                {
                    const bool removes_selection =
                        gui_view_animation_frame(
                            gui_project_snapshot()) ==
                            intent->first &&
                        tp_id128_eq(
                            gui_view_atlas_id(),
                            intent->animation.atlas_id) &&
                        tp_id128_eq(
                            gui_view_animation_id(),
                            intent->animation.animation_id);
                    if (gui_project_anim_remove_frame(
                            &intent->animation,
                            intent->first) &&
                        removes_selection) {
                        gui_view_select_animation_frame(
                            gui_project_snapshot(), -1);
                    }
                }
                break;
            case ANIMATION_INTENT_FRAME_MOVE:
                if (gui_project_anim_move_frame(&intent->animation,
                                                intent->first,
                                                intent->second) &&
                    intent->follow_selection) {
                    gui_view_select_animation_frame(
                        gui_project_snapshot(),
                        intent->first + intent->second);
                }
                break;
            case ANIMATION_INTENT_ADD_FRAMES:
                (void)gui_project_anim_add_frames(
                    &intent->animation, intent->frames,
                    intent->frame_count);
                break;
        }
        gui_actions__frame_refs_dispose(intent->frames, intent->frame_count);
        intent->frames = NULL;
    }
    s_actions.animation_intent_count = 0;
    for (int i = 0; i < s_actions.target_intent_count; i++) {
        target_edit_intent *e = &s_actions.target_intents[i];
        const gui_target_ref target = {e->atlas_id, e->target_id,
                                       e->expected_revision};
        switch (e->kind) {
            case TARGET_INTENT_ENABLED:
                (void)gui_project_set_target_enabled(
                    &target, e->enabled);
                break;
            case TARGET_INTENT_EXPORTER:
                (void)gui_project_set_target_exporter(
                    &target, e->exporter_id);
                break;
        }
    }
    s_actions.target_intent_count = 0;
}

void gui_actions__discard_deferred_edits(void) {
    for (int i = 0;
         i < s_actions.animation_intent_count;
         ++i) {
        gui_actions__frame_refs_dispose(
            s_actions.animation_intents[i]
                .frames,
            s_actions.animation_intents[i]
                .frame_count);
        s_actions.animation_intents[i].frames =
            NULL;
    }
    s_actions.animation_intent_count = 0;
    s_actions.target_intent_count = 0;
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

void gui_actions__apply_structural_edits(void) {
    if (s_pending_add_atlas) {
        const gui_project_create_result created =
            gui_project_add_atlas();
        if (created.committed) {
            gui_view_select_atlas(created.created_id);
            const tp_snapshot_atlas *added =
                !tp_id128_is_nil(created.created_id)
                    ? tp_session_snapshot_atlas_by_id(
                          gui_project_snapshot(),
                          created.created_id)
                    : NULL;
            set_statusf("Added atlas '%s'", added ? added->name : "?");
        }
    }
    if (s_pending_remove_source) {
        /* Side effects and success text run only after a real removal. */
        if (gui_project_remove_source(s_pending_remove_source_atlas_id,
                                      s_pending_remove_source_id,
                                      s_pending_remove_source_revision)) {
            reset_selection();
            set_status("Removed source (Ctrl+Z to undo).");
        }
    }
    if (s_pending_remove_atlas) {
        if (gui_project_remove_atlas(s_pending_remove_atlas_id,
                                     s_pending_remove_atlas_revision)) {
            gui_view_reconcile_observation(
                gui_project_snapshot());
            set_status("Removed atlas (Ctrl+Z to undo).");
        }
    }
    if (s_actions.pending_add_target) {
        const gui_project_create_result created =
            gui_project_add_target(
                s_actions.pending_add_target_atlas_id,
                s_actions.pending_add_target_revision);
        if (created.committed) {
            set_status("Added export target (Ctrl+Z to undo).");
        }
    }
    if (s_actions.pending_remove_target) {
        if (gui_project_remove_target(&s_actions.pending_remove_target_ref)) {
            set_status("Removed export target (Ctrl+Z to undo).");
        }
    }
    if (s_actions.pending_browse_target) {
        gui_actions__browse_target(&s_actions.pending_browse_target_ref);
    }
    if (s_actions.pending_add_anim) {
        const gui_project_create_result created =
            gui_project_create_animation(
            s_actions.pending_add_anim_atlas_id, s_actions.pending_add_anim_revision,
            NULL, NULL, 0);
        if (created.committed) {
            const tp_session_snapshot *after_snapshot = gui_project_snapshot();
            const tp_snapshot_atlas *after_atlas = after_snapshot
                ? tp_session_snapshot_atlas_by_id(
                      after_snapshot, s_actions.pending_add_anim_atlas_id)
                : NULL;
            const tp_snapshot_animation *animation =
                after_atlas
                    ? tp_session_snapshot_animation_by_id(
                          after_snapshot, after_atlas->id,
                          created.created_id)
                    : NULL;
            if (tp_id128_eq(
                    gui_view_atlas_id(),
                    s_actions.pending_add_anim_atlas_id)) {
                gui_view_select_animation(created.created_id);
            }
            set_statusf("Added animation '%s' (Ctrl+Z to undo).", animation ? animation->name : "?");
        }
    }
    if (s_actions.pending_create_anim.active) {
        const gui_project_create_result created =
            gui_project_create_animation(
            s_actions.pending_create_anim.atlas_id,
            s_actions.pending_create_anim.expected_revision,
            s_actions.pending_create_anim.name[0] ? s_actions.pending_create_anim.name : NULL,
            s_actions.pending_create_anim.frames,
            s_actions.pending_create_anim.frame_count);
        if (created.committed) {
            const tp_session_snapshot *after_snapshot = gui_project_snapshot();
            const tp_snapshot_atlas *after_atlas = after_snapshot
                ? tp_session_snapshot_atlas_by_id(
                      after_snapshot, s_actions.pending_create_anim.atlas_id)
                : NULL;
            const tp_snapshot_animation *animation =
                after_atlas
                    ? tp_session_snapshot_animation_by_id(
                          after_snapshot, after_atlas->id,
                          created.created_id)
                    : NULL;
            if (tp_id128_eq(
                    gui_view_atlas_id(),
                    s_actions.pending_create_anim.atlas_id)) {
                gui_view_select_animation(created.created_id);
            }
            set_statusf("Created animation '%s' with %d frame(s) (Ctrl+Z to undo).",
                        animation ? animation->name : "?",
                        s_actions.pending_create_anim.frame_count);
        }
    }
    gui_actions__pending_create_animation_dispose(
        &s_actions.pending_create_anim);
    if (s_actions.pending_remove_anim) {
            /* preview_stop + selection reset + success text run only after a real removal.
             * On any operation rejection the animation remains, so we must not clear its UI state.
             * (preview_stop only resets flags,
             * so running it AFTER the removal is safe -- no project deref.) */
            const bool was_previewing =
                s_preview_active &&
                tp_id128_eq(s_actions.preview_animation_ref.atlas_id,
                            s_actions.pending_remove_anim_ref.atlas_id) &&
                tp_id128_eq(s_actions.preview_animation_ref.animation_id,
                            s_actions.pending_remove_anim_ref.animation_id);
            if (gui_project_remove_animation(&s_actions.pending_remove_anim_ref)) {
                if (was_previewing) {
                    preview_stop();
                }
                if (tp_id128_eq(
                        gui_view_animation_id(),
                        s_actions.pending_remove_anim_ref.animation_id)) {
                    gui_view_select_animation(
                        tp_id128_nil());
                }
                set_status("Removed animation (Ctrl+Z to undo).");
            }
    }
    if (s_actions.pending_open_preview) {
        open_preview_ref(
            &s_actions.pending_open_preview_ref);
    }
}

void gui_actions__clear_pending(void) {
    gui_actions__clear_history_request();
    s_actions.pending_lifecycle_request =
        GUI_LIFECYCLE_REQUEST_NONE;
    s_pending_open = s_pending_save = s_pending_save_as = false;
    s_pending_add_files = s_pending_add_folder = s_pending_add_atlas = false;
    s_pending_refresh = s_pending_pack = s_pending_export = false;
    s_actions.pending_add_target = false;
    s_actions.pending_add_target_atlas_id = tp_id128_nil();
    s_actions.pending_add_target_revision = 0;
    s_actions.pending_add_anim = false;
    s_actions.pending_open_preview = false;
    memset(&s_actions.pending_open_preview_ref, 0,
           sizeof s_actions.pending_open_preview_ref);
    s_actions.pending_add_anim_atlas_id = tp_id128_nil();
    s_actions.pending_add_anim_revision = 0;
    s_pending_remove_source = false;
    s_pending_remove_source_atlas_id = tp_id128_nil();
    s_pending_remove_source_id = tp_id128_nil();
    s_pending_remove_source_revision = 0;
    s_pending_remove_atlas = false;
    s_pending_remove_atlas_id = tp_id128_nil();
    s_pending_remove_atlas_revision = 0;
    s_actions.pending_remove_target = false;
    s_actions.pending_remove_anim = false;
    gui_actions__pending_create_animation_dispose(
        &s_actions.pending_create_anim);
    s_actions.pending_browse_target = false;
    memset(&s_actions.pending_browse_target_ref, 0,
           sizeof s_actions.pending_browse_target_ref);
    s_pending_preview_target = -1;
}
