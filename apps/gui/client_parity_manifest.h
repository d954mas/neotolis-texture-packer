#ifndef NTPACKER_CLIENT_PARITY_MANIFEST_H
#define NTPACKER_CLIENT_PARITY_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_operation.h"

enum client_parity_coverage {
    CLIENT_PARITY_REAL_CLI = 1u << 0,
    CLIENT_PARITY_REAL_GUI = 1u << 1,
    CLIENT_PARITY_SUCCESS = 1u << 2,
    CLIENT_PARITY_ERROR = 1u << 3,
    CLIENT_PARITY_NO_OP = 1u << 4,
    CLIENT_PARITY_SET_MASK = 1u << 5,
    CLIENT_PARITY_OMITTED_PRESENT = 1u << 6,
    CLIENT_PARITY_SELECTOR = 1u << 7,
    CLIENT_PARITY_AMBIGUITY = 1u << 8,
    CLIENT_PARITY_NOTICE = 1u << 9,
    CLIENT_PARITY_EXIT_CODE = 1u << 10,
    CLIENT_PARITY_FINAL_BYTES = 1u << 11,
    CLIENT_PARITY_REQUIRED_COVERAGE = (1u << 12) - 1u
};

#define CLIENT_PARITY_DIMENSION_COUNT 12U

typedef struct client_parity_manifest_row {
    tp_op_kind kind;
    const char *family;
    uint32_t coverage;
    const char *cli_oracle;
    const char *gui_oracle;
    const char *dimension_oracles[CLIENT_PARITY_DIMENSION_COUNT];
} client_parity_manifest_row;

typedef struct client_parity_outcome_row {
    const char *family;
    uint32_t dimension;
    uint32_t applicable_clients;
    const char *cli_oracle;
    const char *gui_oracle;
} client_parity_outcome_row;

const client_parity_manifest_row *client_parity_manifest_rows(size_t *count);
const client_parity_outcome_row *client_parity_outcome_rows(size_t *count);

#endif
