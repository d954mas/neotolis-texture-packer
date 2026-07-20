#ifndef NTPACKER_CLI_EXIT_H
#define NTPACKER_CLI_EXIT_H

#include "tp_core/tp_error.h"

/* Process exit codes -- a stable contract (ai-first.md item 3, plan "CLI v1
 * contract"). Agents branch on these, so the numbers are frozen; 9+ is reserved
 * for future distinct failures. Every enumerator's WHY is the condition that
 * MUST map to it. */
typedef enum cli_exit {
    CLI_EXIT_OK = 0,       /* success: the requested work completed */
    CLI_EXIT_INTERNAL = 1, /* an unexpected internal error / bug (a tp_core call failed where it should not) */
    CLI_EXIT_USAGE = 2,    /* bad invocation: unknown verb/flag, missing or malformed arguments, no command */
    CLI_EXIT_PROJECT = 3,  /* project file load/parse error (TP_STATUS_BAD_PROJECT / BAD_VERSION) */
    CLI_EXIT_PACK = 4,     /* pack failure (tp_pack / nt_builder returned an error) */
    CLI_EXIT_EXPORT = 5,   /* export failure (writing an enabled target's output failed) */
    CLI_EXIT_PARTIAL = 6,  /* partial success: some atlases/targets succeeded, others failed */
    CLI_EXIT_VALIDATE = 7, /* `validate` found problems AND --strict was set (findings otherwise live in the payload) */
    CLI_EXIT_FILE_IO = 8   /* Save failed before publication; typed file_io error */
    /* 9+ reserved: add a distinct code, never overload an existing one. */
} cli_exit;

/* Maps a core rejection from a CLI-built transaction to the stable process
 * contract. Generated-ID failures are internal; missing model references are
 * project errors; malformed values/vocabulary are usage errors. */
int cli_exit_for_rejected_status(tp_status status);

/* Save owns a distinct pre-publication I/O exit. Other save faults preserve
 * the existing internal/project split. */
int cli_exit_for_save_status(tp_status status);

#endif /* NTPACKER_CLI_EXIT_H */
