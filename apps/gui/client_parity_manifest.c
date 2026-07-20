#include "client_parity_manifest.h"

#define REAL_ORACLE "tp_client_parity_real"

#define REAL_COVERAGE                                                         \
    (CLIENT_PARITY_REAL_CLI | CLIENT_PARITY_REAL_GUI |                       \
     CLIENT_PARITY_SUCCESS | CLIENT_PARITY_FINAL_BYTES)
#define SELECT_COVERAGE (REAL_COVERAGE | CLIENT_PARITY_SELECTOR)
#define SET_COVERAGE (REAL_COVERAGE | CLIENT_PARITY_SET_MASK)
#define SET_SELECT_COVERAGE (SET_COVERAGE | CLIENT_PARITY_SELECTOR)
#define SET_OMITTED_COVERAGE                                                  \
    (SET_COVERAGE | CLIENT_PARITY_OMITTED_PRESENT)
#define SET_OMITTED_SELECT_COVERAGE                                           \
    (SET_OMITTED_COVERAGE | CLIENT_PARITY_SELECTOR)

#define REAL_EVIDENCE                                                         \
    {[2] = REAL_ORACLE, [11] = REAL_ORACLE}
#define SELECT_EVIDENCE                                                       \
    {[2] = REAL_ORACLE, [7] = REAL_ORACLE, [11] = REAL_ORACLE}
#define SET_EVIDENCE                                                          \
    {[2] = REAL_ORACLE, [5] = REAL_ORACLE, [11] = REAL_ORACLE}
#define SET_SELECT_EVIDENCE                                                   \
    {[2] = REAL_ORACLE, [5] = REAL_ORACLE, [7] = REAL_ORACLE,                \
     [11] = REAL_ORACLE}
#define SET_OMITTED_EVIDENCE                                                  \
    {[2] = REAL_ORACLE, [5] = REAL_ORACLE, [6] = REAL_ORACLE,                \
     [11] = REAL_ORACLE}
#define SET_OMITTED_SELECT_EVIDENCE                                           \
    {[2] = REAL_ORACLE, [5] = REAL_ORACLE, [6] = REAL_ORACLE,                \
     [7] = REAL_ORACLE, [11] = REAL_ORACLE}

static const client_parity_manifest_row rows[] = {
    {TP_OP_ATLAS_CREATE, "atlas", REAL_COVERAGE, "cli_mutate_atlas",
     REAL_ORACLE, REAL_EVIDENCE},
    {TP_OP_ATLAS_REMOVE, "atlas", SELECT_COVERAGE, "cli_mutate_atlas",
     REAL_ORACLE, SELECT_EVIDENCE},
    {TP_OP_ATLAS_RENAME, "atlas", SELECT_COVERAGE, "cli_mutate_atlas",
     REAL_ORACLE, SELECT_EVIDENCE},
    {TP_OP_ATLAS_SETTINGS_SET, "atlas", SET_OMITTED_COVERAGE,
     "cli_mutate_set", REAL_ORACLE, SET_OMITTED_EVIDENCE},
    {TP_OP_SOURCE_ADD, "source", REAL_COVERAGE, "cli_mutate_source",
     REAL_ORACLE, REAL_EVIDENCE},
    {TP_OP_SOURCE_REMOVE, "source", SELECT_COVERAGE, "cli_mutate_source",
     REAL_ORACLE, SELECT_EVIDENCE},
    {TP_OP_SPRITE_OVERRIDE_SET, "sprite", SET_SELECT_COVERAGE,
     "cli_mutate_sprite", REAL_ORACLE, SET_SELECT_EVIDENCE},
    {TP_OP_SPRITE_OVERRIDE_CLEAR, "sprite", SELECT_COVERAGE,
     "cli_mutate_sprite", REAL_ORACLE, SELECT_EVIDENCE},
    {TP_OP_SPRITE_NAME_SET, "sprite", SELECT_COVERAGE,
     "cli_mutate_sprite", REAL_ORACLE, SELECT_EVIDENCE},
    {TP_OP_ANIMATION_CREATE, "animation", SELECT_COVERAGE, "cli_mutate_anim",
     REAL_ORACLE, SELECT_EVIDENCE},
    {TP_OP_ANIMATION_REMOVE, "animation", SELECT_COVERAGE, "cli_mutate_anim",
     REAL_ORACLE, SELECT_EVIDENCE},
    {TP_OP_ANIMATION_SETTINGS_SET, "animation", SET_SELECT_COVERAGE,
     "cli_mutate_anim", REAL_ORACLE, SET_SELECT_EVIDENCE},
    {TP_OP_ANIMATION_FRAME_ADD, "animation", SELECT_COVERAGE,
     "cli_mutate_anim", REAL_ORACLE, SELECT_EVIDENCE},
    {TP_OP_ANIMATION_FRAME_REMOVE, "animation", SELECT_COVERAGE,
     "cli_mutate_anim", REAL_ORACLE, SELECT_EVIDENCE},
    {TP_OP_ANIMATION_FRAME_MOVE, "animation", SELECT_COVERAGE,
     "cli_mutate_anim", REAL_ORACLE, SELECT_EVIDENCE},
    {TP_OP_TARGET_CREATE, "target", REAL_COVERAGE, "cli_mutate_target",
     REAL_ORACLE, REAL_EVIDENCE},
    {TP_OP_TARGET_REMOVE, "target", SELECT_COVERAGE, "cli_mutate_target",
     REAL_ORACLE, SELECT_EVIDENCE},
    {TP_OP_TARGET_SET, "target", SET_OMITTED_SELECT_COVERAGE,
     "cli_mutate_target", REAL_ORACLE, SET_OMITTED_SELECT_EVIDENCE},
    {TP_OP_ANIMATION_RENAME, "animation", SELECT_COVERAGE, "cli_mutate_anim",
     REAL_ORACLE, SELECT_EVIDENCE},
};

/* Outcome dimensions are representative contracts, not requirements on every
 * operation. Applicability is explicit so the CLI-only selector/exit surfaces
 * do not manufacture GUI claims. tp_client_parity_real executes every row. */
static const client_parity_outcome_row outcome_rows[] = {
    {"atlas/settings", CLIENT_PARITY_ERROR,
     CLIENT_PARITY_REAL_CLI | CLIENT_PARITY_REAL_GUI, REAL_ORACLE,
     REAL_ORACLE},
    {"atlas", CLIENT_PARITY_NO_OP,
     CLIENT_PARITY_REAL_CLI | CLIENT_PARITY_REAL_GUI, REAL_ORACLE,
     REAL_ORACLE},
    {"sprite/selector", CLIENT_PARITY_AMBIGUITY, CLIENT_PARITY_REAL_CLI,
     REAL_ORACLE, NULL},
    {"target/export", CLIENT_PARITY_NOTICE,
     CLIENT_PARITY_REAL_CLI | CLIENT_PARITY_REAL_GUI, REAL_ORACLE,
     REAL_ORACLE},
    {"cli/status", CLIENT_PARITY_EXIT_CODE, CLIENT_PARITY_REAL_CLI,
     REAL_ORACLE, NULL},
};

const client_parity_manifest_row *client_parity_manifest_rows(size_t *count) {
    if (count) {
        *count = sizeof rows / sizeof rows[0];
    }
    return rows;
}

const client_parity_outcome_row *client_parity_outcome_rows(size_t *count) {
    if (count) {
        *count = sizeof outcome_rows / sizeof outcome_rows[0];
    }
    return outcome_rows;
}
