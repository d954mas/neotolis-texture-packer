#include "unity.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gui_session_adapter.h"

static int deterministic_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *next = (uint8_t *)ctx;
    for (size_t i = 0U; i < len; ++i) {
        out[i] = (*next)++;
    }
    return (int)len;
}

void setUp(void) {}
void tearDown(void) {}

void test_shipping_atlas_rename_uses_session_admission_and_snapshot_read(void) {
    uint8_t seed = 1U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_create(&rng, &session, &err));
    gui_session_client client;
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, gui_session_client_attach(&client, session, &err));

    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &before, &err));
    const tp_snapshot_atlas *initial = tp_session_snapshot_atlas_at(before, 0);
    TEST_ASSERT_NOT_NULL(initial);
    const tp_id128 atlas_id = initial->id;
    const gui_session_submit_identity identity = {
        .origin_view_id = {{0x11U}},
        .draft_instance_id = {{0x12U}},
    };
    static const char transaction_id[] =
        "01010101010101010101010101010101";
    gui_session_submit_terminal terminal = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_submit_atlas_name(
            &client, atlas_id, tp_session_snapshot_revision(before),
            "gui-session-name", identity, transaction_id, &terminal, &err));
    TEST_ASSERT_EQUAL_STRING(transaction_id, terminal.transaction_id);
    TEST_ASSERT_TRUE(terminal.committed);
    TEST_ASSERT_FALSE(terminal.no_change);
    tp_session_snapshot_destroy(before);

    tp_session_event event;
    size_t event_count = 0U;
    bool resync = false;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_events_after(session, 0U, &event, 1U, &event_count, &resync, &err));
    TEST_ASSERT_FALSE(resync);
    TEST_ASSERT_EQUAL_UINT64(1U, event_count);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_EVENT_MODEL_COMMITTED, event.kind);
    TEST_ASSERT_EQUAL_UINT64(1U, event.admission_sequence);

    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &after, &err));
    char name[64];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          gui_session_copy_atlas_name(after, atlas_id, name, sizeof name, &err));
    TEST_ASSERT_EQUAL_STRING("gui-session-name", name);
    tp_session_snapshot_destroy(after);

    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_atlas_structural_and_settings_use_stable_id_revision_contract(void) {
    uint8_t seed = 31U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_create(&rng, &session, &err));
    gui_session_client client;
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, gui_session_client_attach(&client, session, &err));
    const tp_id128 atlas_id = {{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                                0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22}};
    const tp_id128 target_id = {{0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
                                 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44}};

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_create_atlas(&client, atlas_id, target_id, 0, "atlas2",
                                 "json-neotolis", "out/atlas2", true, NULL, &err));

    tp_op_atlas_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.mask = TP_AF_PADDING;
    settings.padding = 9;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_set_atlas_settings(
            &client, atlas_id, 1, &settings,
            (gui_session_submit_identity){0}, NULL, NULL, &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_REVISION_CONFLICT,
        gui_session_remove_atlas(&client, atlas_id, 1, &err));

    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_by_id(snapshot, atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(9, atlas->padding);
    TEST_ASSERT_EQUAL_INT64(2, tp_session_snapshot_revision(snapshot));
    tp_session_snapshot_destroy(snapshot);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_remove_atlas(&client, atlas_id, 2, &err));
    TEST_ASSERT_EQUAL_INT64(3, tp_session_revision(session));
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_source_family_uses_stable_ids_and_atomic_batch_admission(void) {
    uint8_t seed = 71U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_create(&rng, &session, &err));
    gui_session_client client;
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, gui_session_client_attach(&client, session, &err));
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &before, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(before, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const tp_id128 ids[2] = {
        {{0x51, 0x51, 0x51, 0x51, 0x51, 0x51, 0x51, 0x51,
          0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61}},
        {{0x52, 0x52, 0x52, 0x52, 0x52, 0x52, 0x52, 0x52,
          0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62}},
    };
    const char *paths[2] = {"sprites/a.png", "sprites/b.png"};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_add_sources(&client, atlas_id, ids, paths, 2,
                                TP_SNAPSHOT_SOURCE_FILE,
                                tp_session_snapshot_revision(before), &err));
    tp_session_snapshot_destroy(before);

    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &after, &err));
    TEST_ASSERT_EQUAL_INT64(1, tp_session_snapshot_revision(after));
    const tp_snapshot_source *first = tp_session_snapshot_source_by_id(
        after, atlas_id, ids[0]);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_INT(TP_SNAPSHOT_SOURCE_FILE, first->kind);
    TEST_ASSERT_EQUAL_STRING(paths[0], first->path);
    tp_session_snapshot_destroy(after);

    tp_op_sprite_set sprite_settings;
    memset(&sprite_settings, 0, sizeof sprite_settings);
    sprite_settings.mask = TP_SPF_ORIGIN;
    sprite_settings.origin_x = 0.25F;
    sprite_settings.origin_y = 0.75F;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_set_sprite_override(&client, atlas_id, ids[0], "a.png", 1,
                                        &sprite_settings,
                                        (gui_session_submit_identity){0},
                                        NULL, NULL, &err));
    const gui_session_submit_identity rename_identity = {
        .origin_view_id = {{0x21U}},
        .draft_instance_id = {{0x22U}},
    };
    static const char rename_transaction_id[] =
        "02020202020202020202020202020202";
    gui_session_submit_terminal rename_terminal = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_submit_sprite_name(
            &client, atlas_id, ids[0], "a.png", 2, "hero",
            rename_identity, rename_transaction_id, &rename_terminal, &err));
    TEST_ASSERT_EQUAL_STRING(
        rename_transaction_id, rename_terminal.transaction_id);
    TEST_ASSERT_TRUE(rename_terminal.committed);
    tp_session_event rename_event = {0};
    size_t rename_event_count = 0U;
    bool rename_resync = false;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_events_after(session, 2U, &rename_event, 1U,
                                &rename_event_count, &rename_resync, &err));
    TEST_ASSERT_FALSE(rename_resync);
    TEST_ASSERT_EQUAL_UINT64(1U, rename_event_count);
    TEST_ASSERT_EQUAL_STRING(tp_op_wire(TP_OP_SPRITE_NAME_SET),
                             rename_event.label);
    tp_session_snapshot *sprite_snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &sprite_snapshot, &err));
    const tp_snapshot_sprite *sprite = tp_session_snapshot_sprite_by_key(
        sprite_snapshot, atlas_id, ids[0], "a.png");
    TEST_ASSERT_NOT_NULL(sprite);
    TEST_ASSERT_FALSE(tp_id128_is_nil(sprite->id));
    TEST_ASSERT_EQUAL_STRING("hero", sprite->rename);
    TEST_ASSERT_TRUE(sprite->origin_x == 0.25F);
    tp_session_snapshot_destroy(sprite_snapshot);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_REVISION_CONFLICT,
        gui_session_remove_source(&client, atlas_id, ids[0], 0, &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_session_remove_source(&client, atlas_id, ids[0], 3, &err));
    TEST_ASSERT_EQUAL_INT64(3, tp_session_revision(session));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_remove_source(&client, atlas_id, ids[1], 3, &err));
    TEST_ASSERT_EQUAL_INT64(4, tp_session_revision(session));
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_animation_family_uses_stable_ids_revision_and_snapshot_read(void) {
    uint8_t seed = 101U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_create(&rng, &session, &err));
    gui_session_client client;
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, gui_session_client_attach(&client, session, &err));
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &before, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(before, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const tp_id128 animation_id = {
        {0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81,
         0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91}};
    const tp_id128 source_id = {
        {0x71, 0x71, 0x71, 0x71, 0x71, 0x71, 0x71, 0x71,
         0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72}};
    const char *source_path[1] = {"sprites"};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_add_sources(&client, atlas_id, &source_id, source_path, 1,
                                TP_SNAPSHOT_SOURCE_FOLDER, 0, &err));
    const tp_op_sprite_ref initial_frames[2] = {
        {source_id, "walk_1.png"}, {source_id, "walk_2.png"}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_create_animation(
            &client, atlas_id, animation_id,
            1, "walk", initial_frames, 2, NULL, &err));
    tp_session_snapshot_destroy(before);

    tp_op_anim_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.mask = TP_ANF_FPS;
    settings.fps = 24.0F;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_set_animation_settings(
            &client, atlas_id, animation_id, 2, &settings,
            (gui_session_submit_identity){0}, NULL, NULL, &err));
    const tp_op_sprite_ref extra[1] = {{source_id, "walk_3.png"}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_add_animation_frames(
            &client, atlas_id, animation_id, 3, extra, 1, &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_REVISION_CONFLICT,
        gui_session_remove_animation(
            &client, atlas_id, animation_id, 2, &err));

    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &after, &err));
    const tp_snapshot_animation *animation =
        tp_session_snapshot_animation_by_id(after, atlas_id, animation_id);
    TEST_ASSERT_NOT_NULL(animation);
    TEST_ASSERT_EQUAL_STRING("walk", animation->name);
    TEST_ASSERT_TRUE(animation->fps == 24.0F);
    TEST_ASSERT_EQUAL_INT(3, animation->frame_count);
    const tp_snapshot_frame *last = tp_session_snapshot_animation_frame_at(
        after, atlas_id, animation_id, 2);
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_STRING("walk_3", last->name);
    tp_session_snapshot_destroy(after);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_target_family_uses_stable_ids_revision_and_snapshot_read(void) {
    uint8_t seed = 131U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_create(&rng, &session, &err));
    gui_session_client client;
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, gui_session_client_attach(&client, session, &err));
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &before, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(before, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const tp_id128 target_id = {
        {0xa1, 0xa1, 0xa1, 0xa1, 0xa1, 0xa1, 0xa1, 0xa1,
         0xb1, 0xb1, 0xb1, 0xb1, 0xb1, 0xb1, 0xb1, 0xb1}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_create_target(&client, atlas_id, target_id,
                                  tp_session_snapshot_revision(before),
                                  "json-neotolis", "out/atlas", true, NULL, &err));
    tp_session_snapshot_destroy(before);

    tp_op_target_set settings;
    memset(&settings, 0, sizeof settings);
    settings.mask = TP_TF_OUT_PATH | TP_TF_ENABLED;
    settings.out_path = "out/final";
    settings.enabled = false;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_session_set_target(
            &client, atlas_id, target_id, 1, &settings,
            (gui_session_submit_identity){0}, NULL, NULL, &err));
    TEST_ASSERT_EQUAL_STRING(
        "target out path requires identified path submission", err.msg);
    TEST_ASSERT_EQUAL_INT64(1, tp_session_revision(session));

    const gui_session_submit_identity path_identity = {
        .origin_view_id = {{0x31U}},
        .draft_instance_id = {{0x32U}},
    };
    static const char path_transaction_id[] =
        "03030303030303030303030303030303";
    gui_session_submit_terminal path_terminal = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_submit_target_out_path(
            &client, atlas_id, target_id, 1, "out/final",
            path_identity, path_transaction_id, &path_terminal, &err));
    TEST_ASSERT_EQUAL_STRING(
        path_transaction_id, path_terminal.transaction_id);
    TEST_ASSERT_TRUE(path_terminal.committed);

    settings.mask = TP_TF_ENABLED;
    settings.out_path = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_set_target(
            &client, atlas_id, target_id, 2, &settings,
            (gui_session_submit_identity){0}, NULL, NULL, &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_REVISION_CONFLICT,
        gui_session_remove_target(&client, atlas_id, target_id, 1, &err));

    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &after, &err));
    const tp_snapshot_target *target = tp_session_snapshot_target_by_id(
        after, atlas_id, target_id);
    TEST_ASSERT_NOT_NULL(target);
    TEST_ASSERT_EQUAL_STRING("json-neotolis", target->exporter_id);
    TEST_ASSERT_EQUAL_STRING("out/final", target->out_path);
    TEST_ASSERT_FALSE(target->enabled);
    tp_session_snapshot_destroy(after);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_identified_text_submits_preserve_exact_receipts_and_stable_targets(void) {
    uint8_t seed = 181U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create(&rng, &session, &err));
    gui_session_client client;
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &err));

    tp_session_snapshot *initial = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &initial, &err));
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(initial, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    tp_session_snapshot_destroy(initial);

    const tp_id128 source_id = {{0x41U}};
    const char *source_path = "sprites/hero.png";
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_add_sources(
            &client, atlas_id, &source_id, &source_path, 1,
            TP_SNAPSHOT_SOURCE_FILE, 0, &err));

    const tp_id128 animation_id = {{0x51U}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_create_animation(
            &client, atlas_id, animation_id, 1,
            "idle", NULL, 0, NULL, &err));

    const tp_id128 target_id = {{0x61U}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_create_target(
            &client, atlas_id, target_id, 2,
            "json-neotolis", "out/original", true,
            NULL, &err));

    const gui_session_submit_identity identity = {
        .origin_view_id = {{0x71U}},
        .draft_instance_id = {{0x81U}},
    };
    static const char atlas_tx[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char animation_tx[] =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    static const char sprite_tx[] =
        "cccccccccccccccccccccccccccccccc";
    static const char sprite_settings_tx[] =
        "11111111111111111111111111111111";
    static const char animation_settings_tx[] =
        "22222222222222222222222222222222";
    static const char target_tx[] =
        "dddddddddddddddddddddddddddddddd";
    static const char no_change_tx[] =
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    gui_session_submit_terminal terminal = {0};

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_submit_atlas_name(
            &client, atlas_id, 3, "identified-atlas",
            identity, atlas_tx, &terminal, &err));
    TEST_ASSERT_EQUAL_STRING(atlas_tx, terminal.transaction_id);
    TEST_ASSERT_EQUAL_MEMORY(
        &identity, &terminal.identity, sizeof identity);
    TEST_ASSERT_TRUE(terminal.committed);
    TEST_ASSERT_FALSE(terminal.no_change);
    TEST_ASSERT_EQUAL_INT64(4, terminal.revision);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_submit_animation_name(
            &client, atlas_id, animation_id, 4,
            "identified-animation", identity, animation_tx,
            &terminal, &err));
    TEST_ASSERT_EQUAL_STRING(animation_tx, terminal.transaction_id);
    TEST_ASSERT_EQUAL_MEMORY(
        &identity, &terminal.identity, sizeof identity);
    TEST_ASSERT_EQUAL_INT64(5, terminal.revision);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_submit_sprite_name(
            &client, atlas_id, source_id, "hero.png", 5,
            "identified-sprite", identity, sprite_tx,
            &terminal, &err));
    TEST_ASSERT_EQUAL_STRING(sprite_tx, terminal.transaction_id);
    TEST_ASSERT_EQUAL_MEMORY(
        &identity, &terminal.identity, sizeof identity);
    TEST_ASSERT_EQUAL_INT64(6, terminal.revision);

    const tp_op_sprite_set sprite_settings = {
        .mask = TP_SPF_MARGIN,
        .ov_margin = 37,
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_set_sprite_override(
            &client, atlas_id, source_id, "hero.png", 6,
            &sprite_settings, identity, sprite_settings_tx,
            &terminal, &err));
    TEST_ASSERT_EQUAL_STRING(
        sprite_settings_tx, terminal.transaction_id);
    TEST_ASSERT_EQUAL_MEMORY(
        &identity, &terminal.identity, sizeof identity);
    TEST_ASSERT_TRUE(terminal.committed);
    TEST_ASSERT_EQUAL_INT64(7, terminal.revision);

    const tp_op_anim_settings animation_settings = {
        .mask = TP_ANF_FLIP_H,
        .flip_h = true,
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_set_animation_settings(
            &client, atlas_id, animation_id, 7,
            &animation_settings, identity,
            animation_settings_tx, &terminal, &err));
    TEST_ASSERT_EQUAL_STRING(
        animation_settings_tx, terminal.transaction_id);
    TEST_ASSERT_EQUAL_MEMORY(
        &identity, &terminal.identity, sizeof identity);
    TEST_ASSERT_TRUE(terminal.committed);
    TEST_ASSERT_EQUAL_INT64(8, terminal.revision);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_submit_target_out_path(
            &client, atlas_id, target_id, 8,
            "out/identified", identity, target_tx,
            &terminal, &err));
    TEST_ASSERT_EQUAL_STRING(target_tx, terminal.transaction_id);
    TEST_ASSERT_EQUAL_MEMORY(
        &identity, &terminal.identity, sizeof identity);
    TEST_ASSERT_EQUAL_INT64(9, terminal.revision);

    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &after, &err));
    atlas = tp_session_snapshot_atlas_by_id(after, atlas_id);
    const tp_snapshot_animation *animation =
        tp_session_snapshot_animation_by_id(
            after, atlas_id, animation_id);
    const tp_snapshot_sprite *sprite =
        tp_session_snapshot_sprite_by_key(
            after, atlas_id, source_id, "hero.png");
    const tp_snapshot_target *target =
        tp_session_snapshot_target_by_id(
            after, atlas_id, target_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_NOT_NULL(animation);
    TEST_ASSERT_NOT_NULL(sprite);
    TEST_ASSERT_NOT_NULL(target);
    TEST_ASSERT_EQUAL_STRING("identified-atlas", atlas->name);
    TEST_ASSERT_EQUAL_STRING(
        "identified-animation", animation->name);
    TEST_ASSERT_EQUAL_STRING(
        "identified-sprite", sprite->rename);
    TEST_ASSERT_EQUAL_INT(
        37, sprite->override_margin);
    TEST_ASSERT_TRUE(animation->flip_h);
    TEST_ASSERT_FALSE(animation->flip_v);
    TEST_ASSERT_EQUAL_STRING(
        "out/identified", target->out_path);
    TEST_ASSERT_EQUAL_STRING(
        "json-neotolis", target->exporter_id);
    TEST_ASSERT_TRUE(target->enabled);
    tp_session_snapshot_destroy(after);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_submit_atlas_name(
            &client, atlas_id, 9, "identified-atlas",
            identity, no_change_tx, &terminal, &err));
    TEST_ASSERT_EQUAL_STRING(
        no_change_tx, terminal.transaction_id);
    TEST_ASSERT_EQUAL_MEMORY(
        &identity, &terminal.identity, sizeof identity);
    TEST_ASSERT_FALSE(terminal.committed);
    TEST_ASSERT_TRUE(terminal.no_change);
    TEST_ASSERT_EQUAL_INT64(9, terminal.revision);

    gui_session_submit_identity wrong_identity = identity;
    wrong_identity.draft_instance_id.bytes[0]++;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_session_submit_target_out_path(
            &client, atlas_id, target_id, 9,
            "out/must-not-land", wrong_identity, target_tx,
            &terminal, &err));
    TEST_ASSERT_EQUAL_INT64(9, tp_session_revision(session));

    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_shipping_atlas_rename_uses_session_admission_and_snapshot_read);
    RUN_TEST(test_atlas_structural_and_settings_use_stable_id_revision_contract);
    RUN_TEST(test_source_family_uses_stable_ids_and_atomic_batch_admission);
    RUN_TEST(test_animation_family_uses_stable_ids_revision_and_snapshot_read);
    RUN_TEST(test_target_family_uses_stable_ids_revision_and_snapshot_read);
    RUN_TEST(
        test_identified_text_submits_preserve_exact_receipts_and_stable_targets);
    return UNITY_END();
}
