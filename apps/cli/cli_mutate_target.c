#include "cli_mutate_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_exit.h"
#include "cli_out.h"

/* ------------------------------------------------------------------ */
/* target                                                             */
/* ------------------------------------------------------------------ */

int do_target(const char *const *pos, int npos, bool dry_run, bool json, bool quiet) {
    /* target <sub> <project> <atlas> ... */
    if (npos < 5) {
        cli_emit_error(json, quiet, "usage", "target needs <sub> <project> <atlas> ...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *sub = pos[1];
    const char *path = pos[2];
    const char *atlas = pos[3];
    cli_edit edit;
    const tp_snapshot_atlas *atlas_dto = NULL;
    int rc = edit_open_atlas(&edit, path, atlas, &atlas_dto, dry_run, json, quiet);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    tp_id128 aid = atlas_dto->id;

    if (strcmp(sub, "add") == 0) {
        if (npos != 6) {
            return edit_fail_usage(&edit, json, quiet, "usage", "target add needs <exporter-id> <out-path>");
        }
        const char *eid = pos[4];
        const char *out = pos[5];
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_TARGET_CREATE;
        op.atlas_id = aid;
        op.u.target_create.exporter_id = cli_strdup(eid);
        op.u.target_create.out_path = cli_strdup(out);
        op.u.target_create.enabled = true;
        if (!op.u.target_create.exporter_id || !op.u.target_create.out_path) {
            tp_operation_free(&op);
            cli_emit_error(json, quiet, "oom", "out of memory building target");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        if (!cli_gen_id(&op.u.target_create.target_id)) { /* OS-RNG fault, not OOM (F4) */
            tp_operation_free(&op);
            cli_emit_error(json, quiet, "rng_failed", "could not generate a target id");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        char human[192];
        (void)snprintf(human, sizeof human, "Added target %s -> %s on '%s'", eid, out, atlas);
        return commit_session_ops(&edit, &op, 1, "target", 1, human, json, quiet);
    }

    if (strcmp(sub, "remove") == 0) {
        if (npos != 5) {
            return edit_fail_usage(&edit, json, quiet, "usage", "target remove needs <index-or-id>");
        }
        const char *sel = pos[4];
        const tp_snapshot_target *target = NULL;
        tp_error selector_error = {0};
        tp_status selector_status = tp_session_snapshot_resolve_target(
            edit.snapshot, aid, sel, &target, &selector_error);
        if (selector_status != TP_STATUS_OK) {
            cli_emit_error(json, quiet, tp_status_id(selector_status), "%s",
                           selector_error.msg[0] ? selector_error.msg
                                                 : tp_status_str(selector_status));
            edit_close(&edit);
            return selector_status == TP_STATUS_OOM ? CLI_EXIT_INTERNAL
                                                    : CLI_EXIT_PROJECT;
        }
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_TARGET_REMOVE;
        op.atlas_id = aid;
        op.u.target_ref.target_id = target->id;
        char human[160];
        (void)snprintf(human, sizeof human, "Removed target '%s' from '%s'", sel, atlas);
        return commit_session_ops(&edit, &op, 1, "target", 1, human, json, quiet);
    }

    if (strcmp(sub, "set") == 0) {
        if (npos < 6) {
            return edit_fail_usage(&edit, json, quiet, "usage",
                              "target set needs <index> [exporter=..] [out=..] [enabled=0|1]");
        }
        const tp_snapshot_target *target = NULL;
        tp_error selector_error = {0};
        tp_status selector_status = tp_session_snapshot_resolve_target(
            edit.snapshot, aid, pos[4], &target, &selector_error);
        if (selector_status != TP_STATUS_OK) {
            cli_emit_error(json, quiet, tp_status_id(selector_status), "%s",
                           selector_error.msg[0] ? selector_error.msg
                                                 : tp_status_str(selector_status));
            edit_close(&edit);
            return selector_status == TP_STATUS_OOM ? CLI_EXIT_INTERNAL
                                                    : CLI_EXIT_PROJECT;
        }
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_TARGET_SET;
        op.atlas_id = aid;
        op.u.target_set.target_id = target->id;
        for (int i = 5; i < npos; i++) {
            char key[32];
            const char *val = split_kv(pos[i], key, sizeof key);
            char m[192];
            if (!val) {
                (void)snprintf(m, sizeof m, "expected key=value, got '%s'", pos[i]);
                tp_operation_free(&op);
                return edit_fail_usage(&edit, json, quiet, "usage", m);
            }
            if (strcmp(key, "exporter") == 0) {
                op.u.target_set.mask |= TP_TF_EXPORTER;
                char *replacement = cli_strdup(val);
                if (!replacement) {
                    tp_operation_free(&op);
                    edit_close(&edit);
                    cli_emit_error(json, quiet, "oom", "out of memory building target");
                    return CLI_EXIT_INTERNAL;
                }
                free(op.u.target_set.exporter_id);
                op.u.target_set.exporter_id = replacement;
            } else if (strcmp(key, "out") == 0) {
                op.u.target_set.mask |= TP_TF_OUT_PATH;
                char *replacement = cli_strdup(val);
                if (!replacement) {
                    tp_operation_free(&op);
                    edit_close(&edit);
                    cli_emit_error(json, quiet, "oom", "out of memory building target");
                    return CLI_EXIT_INTERNAL;
                }
                free(op.u.target_set.out_path);
                op.u.target_set.out_path = replacement;
            } else if (strcmp(key, "enabled") == 0) {
                if (!to_bool(val, &op.u.target_set.enabled)) {
                    (void)snprintf(m, sizeof m, "enabled = '%s' must be 0/1/true/false", val);
                    tp_operation_free(&op);
                    return edit_fail_usage(&edit, json, quiet, "usage", m);
                }
                op.u.target_set.mask |= TP_TF_ENABLED;
            } else {
                (void)snprintf(m, sizeof m, "unknown target key '%s' (known: exporter, out, enabled)", key);
                tp_operation_free(&op);
                return edit_fail_usage(&edit, json, quiet, "usage", m);
            }
        }
        char human[192];
        (void)snprintf(human, sizeof human, "Updated target %s on '%s'", pos[4], atlas);
        return commit_session_ops(&edit, &op, 1, "target", 1, human, json, quiet);
    }

    char m[128];
    (void)snprintf(m, sizeof m, "unknown target sub-command '%s'", sub);
    return edit_fail_usage(&edit, json, quiet, "usage", m);
}
