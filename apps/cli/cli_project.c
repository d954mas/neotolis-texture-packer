/* Shared immutable project loader for the read verbs. One home for the exit-3
 * structured-error path so inspect, validate, and pack fail identically. */
#include "cli_cmds.h"

#include "cli_exit.h"
#include "cli_out.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_session.h"

int cli_load_snapshot(const char *path, bool json, bool quiet,
                      tp_session_snapshot **out) {
    *out = NULL;
    tp_error err = {0};
    tp_status st = tp_session_snapshot_load(path, out, &err);
    if (st == TP_STATUS_OK) {
        return CLI_EXIT_OK;
    }
    /* tp_status_id is the stable machine token; err.msg carries the context the
     * loader filled (falls back to the generic prose if it did not). */
    cli_emit_error(json, quiet, tp_status_id(st), "%s", err.msg[0] ? err.msg : tp_status_str(st));
    /* OOM is an internal failure; every other load status is a project error. */
    return (st == TP_STATUS_OOM) ? CLI_EXIT_INTERNAL : CLI_EXIT_PROJECT;
}
