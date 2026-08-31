#ifndef NTPACKER_AGENT_COMMANDS_H
#define NTPACKER_AGENT_COMMANDS_H

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "tp_core/tp_error.h"

#define AGENT_MAX_REQUEST_BYTES (2U * 1024U * 1024U)

/* Closed P1 command vocabulary, not an extension/registration interface. */
typedef enum agent_command_kind {
    AGENT_HELP,
    AGENT_CAPABILITIES,
    AGENT_OPERATIONS_LIST,
    AGENT_SESSION_BIND,
    AGENT_SESSION_STATUS,
    AGENT_SESSION_CLOSE,
    AGENT_PROJECT_SNAPSHOT,
    AGENT_PROJECT_APPLY,
    AGENT_HISTORY_LIST,
    AGENT_HISTORY_UNDO,
    AGENT_HISTORY_REDO,
    AGENT_COMMAND_COUNT
} agent_command_kind;

typedef struct agent_command {
    agent_command_kind kind;
    const char *name;
    const char *description;
    bool bound;
    bool generation;
} agent_command;

size_t agent_command_count(void);
const agent_command *agent_command_at(size_t index); /* NULL beyond the catalog */
const agent_command *agent_command_find(const char *name); /* NULL if unknown */

/* Flat shape admission from the same field rows that generate help. The caller
 * owns state/revision/generation checks and nested core transaction admission.
 * command must come from this catalog. Invalid params returns a diagnostic. */
bool agent_command_params_valid(const agent_command *command,
                                const cJSON *params, tp_error *err);

/* Owned machine-readable help; NULL on OOM or unknown optional_command. Check
 * find first when the caller needs to distinguish those two cases. */
char *agent_help_encode(const char *optional_command);

#endif
