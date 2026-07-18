#include "client_parity_manifest.h"

#define COMMON (CLIENT_PARITY_REAL_CLI | CLIENT_PARITY_REAL_GUI |             \
                CLIENT_PARITY_SUCCESS | CLIENT_PARITY_ERROR |                 \
                CLIENT_PARITY_NOTICE | CLIENT_PARITY_EXIT_CODE |              \
                CLIENT_PARITY_FINAL_BYTES)
#define SET_COVERAGE (COMMON | CLIENT_PARITY_NO_OP | CLIENT_PARITY_SET_MASK | \
                      CLIENT_PARITY_OMITTED_PRESENT)
#define SELECT_COVERAGE (COMMON | CLIENT_PARITY_SELECTOR)

static const client_parity_manifest_row rows[] = {
    {TP_OP_ATLAS_CREATE, "atlas", COMMON, "cli_mutate_atlas", "tp_client_parity_real"},
    {TP_OP_ATLAS_REMOVE, "atlas", SELECT_COVERAGE, "cli_mutate_atlas", "tp_client_parity_real"},
    {TP_OP_ATLAS_RENAME, "atlas", SELECT_COVERAGE, "cli_mutate_atlas", "tp_client_parity_real"},
    {TP_OP_ATLAS_SETTINGS_SET, "atlas", SET_COVERAGE, "cli_mutate_set", "tp_client_parity_real"},
    {TP_OP_SOURCE_ADD, "source", COMMON | CLIENT_PARITY_NO_OP, "cli_mutate_source", "tp_client_parity_real"},
    {TP_OP_SOURCE_REMOVE, "source", SELECT_COVERAGE, "cli_mutate_source", "tp_client_parity_real"},
    {TP_OP_SPRITE_OVERRIDE_SET, "sprite", SET_COVERAGE | CLIENT_PARITY_SELECTOR | CLIENT_PARITY_AMBIGUITY,
     "cli_mutate_sprite", "tp_client_parity_real"},
    {TP_OP_SPRITE_OVERRIDE_CLEAR, "sprite", SELECT_COVERAGE, "cli_mutate_sprite", "tp_client_parity_real"},
    {TP_OP_SPRITE_NAME_SET, "sprite", SET_COVERAGE | CLIENT_PARITY_SELECTOR,
     "cli_mutate_sprite", "tp_client_parity_real"},
    {TP_OP_ANIMATION_CREATE, "animation", COMMON | CLIENT_PARITY_SELECTOR,
     "cli_mutate_anim", "tp_client_parity_real"},
    {TP_OP_ANIMATION_REMOVE, "animation", SELECT_COVERAGE, "cli_mutate_anim", "tp_client_parity_real"},
    {TP_OP_ANIMATION_SETTINGS_SET, "animation", SET_COVERAGE | CLIENT_PARITY_SELECTOR,
     "cli_mutate_anim", "tp_client_parity_real"},
    {TP_OP_ANIMATION_FRAME_ADD, "animation", SELECT_COVERAGE, "cli_mutate_anim", "tp_client_parity_real"},
    {TP_OP_ANIMATION_FRAME_REMOVE, "animation", SELECT_COVERAGE, "cli_mutate_anim", "tp_client_parity_real"},
    {TP_OP_ANIMATION_FRAME_MOVE, "animation", SELECT_COVERAGE, "cli_mutate_anim", "tp_client_parity_real"},
    {TP_OP_TARGET_CREATE, "target", COMMON, "cli_mutate_target", "tp_client_parity_real"},
    {TP_OP_TARGET_REMOVE, "target", SELECT_COVERAGE | CLIENT_PARITY_AMBIGUITY,
     "cli_mutate_target", "tp_client_parity_real"},
    {TP_OP_TARGET_SET, "target", SET_COVERAGE | CLIENT_PARITY_SELECTOR,
     "cli_mutate_target", "tp_client_parity_real"},
    {TP_OP_ANIMATION_RENAME, "animation", SELECT_COVERAGE,
     "cli_mutate_anim", "tp_client_parity_real"},
};

const client_parity_manifest_row *client_parity_manifest_rows(size_t *count) {
    if (count) {
        *count = sizeof rows / sizeof rows[0];
    }
    return rows;
}
