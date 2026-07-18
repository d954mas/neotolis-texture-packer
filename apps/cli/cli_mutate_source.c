#include "cli_mutate_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_exit.h"
#include "cli_out.h"
#include "tp_core/tp_source_plan.h"

/* ------------------------------------------------------------------ */
/* add / remove source                                                */
/* ------------------------------------------------------------------ */

int do_add(const char *const *pos, int npos, bool json, bool quiet) {
    if (npos < 4) {
        cli_emit_error(json, quiet, "usage", "add needs <project> <atlas> <path>... ; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[1];
    const char *atlas = pos[2];
    cli_edit edit;
    const tp_snapshot_atlas *atlas_dto = NULL;
    int rc = edit_open_atlas(&edit, path, atlas, &atlas_dto, json, quiet);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    tp_id128 aid = atlas_dto->id;

    int maxn = npos - 3;
    tp_source_batch_plan plan = {0};
    tp_error plan_error = {0};
    tp_status plan_status = tp_source_batch_plan_create(
        edit.snapshot, aid, &pos[3], maxn, &plan, &plan_error);
    if (plan_status != TP_STATUS_OK) {
        cli_emit_error(json, quiet, tp_status_id(plan_status), "%s",
                       plan_error.msg[0] ? plan_error.msg
                                         : tp_status_str(plan_status));
        edit_close(&edit);
        return status_is_internal_fault(plan_status) ? CLI_EXIT_INTERNAL
                                                     : CLI_EXIT_USAGE;
    }
    tp_operation *ops = plan.count > 0
                            ? (tp_operation *)calloc((size_t)plan.count,
                                                     sizeof *ops)
                            : NULL;
    if (plan.count > 0 && !ops) {
        free(ops);
        tp_source_batch_plan_free(&plan);
        cli_emit_error(json, quiet, "oom", "out of memory building sources");
        edit_close(&edit);
        return CLI_EXIT_INTERNAL;
    }

    int added = 0;
    const int dup = plan.duplicate_count;
    bool oom = false;
    bool rngfail = false;
    for (int i = 0; i < plan.count; i++) {
        tp_operation *op = &ops[added];
        memset(op, 0, sizeof *op);
        op->kind = TP_OP_SOURCE_ADD;
        op->atlas_id = aid;
        op->u.source_add.kind = TP_SOURCE_KIND_FOLDER; /* kind-agnostic default (matches add_source) */
        op->u.source_add.key = cli_strdup(plan.items[i].path);
        if (!op->u.source_add.key) {
            oom = true;
            break;
        }
        if (!cli_gen_id(&op->u.source_add.source_id)) { /* OS-RNG fault, not OOM (F4) */
            rngfail = true;
            break;
        }
        added++;
    }
    tp_source_batch_plan_free(&plan);

    if (oom || rngfail) {
        free_ops(ops, added + 1); /* +1: free the partially-built op the loop broke on */
        free(ops);
        if (rngfail) {
            cli_emit_error(json, quiet, "rng_failed", "could not generate a source id");
        } else {
            cli_emit_error(json, quiet, "oom", "out of memory building sources");
        }
        edit_close(&edit);
        return CLI_EXIT_INTERNAL;
    }

    char human[128];
    (void)snprintf(human, sizeof human, "Added %d source(s)%s to '%s'", added,
                   dup ? " (some already present)" : "", atlas);
    rc = commit_session_ops(&edit, ops, added, "add", added, human, json, quiet);
    free(ops);
    return rc;
}

int do_remove_source(const char *const *pos, int npos, bool json, bool quiet) {
    if (npos != 4) {
        cli_emit_error(json, quiet, "usage", "remove needs <project> <atlas> <source>; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[1];
    const char *atlas = pos[2];
    const char *src = pos[3];
    cli_edit edit;
    const tp_snapshot_atlas *atlas_dto = NULL;
    int rc = edit_open_atlas(&edit, path, atlas, &atlas_dto, json, quiet);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    const tp_snapshot_source *source = NULL;
    tp_error lookup_error = {0};
    const tp_status lookup_status = tp_source_snapshot_find(
        edit.snapshot, atlas_dto->id, src, &source, &lookup_error);
    if (lookup_status != TP_STATUS_OK) {
        if (lookup_status == TP_STATUS_NOT_FOUND) {
            cli_emit_error(json, quiet, "source_not_found",
                           "atlas '%s' has no source matching '%s'", atlas,
                           src);
        } else {
            cli_emit_error(json, quiet, tp_status_id(lookup_status), "%s",
                           lookup_error.msg[0]
                               ? lookup_error.msg
                               : tp_status_str(lookup_status));
        }
        edit_close(&edit);
        return status_is_internal_fault(lookup_status)
                   ? CLI_EXIT_INTERNAL
                   : (lookup_status == TP_STATUS_NOT_FOUND ? CLI_EXIT_PROJECT
                                                          : CLI_EXIT_USAGE);
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REMOVE;
    op.atlas_id = atlas_dto->id;
    op.u.source_ref.source_id = source->id;
    char human[128];
    (void)snprintf(human, sizeof human, "Removed source '%s' from '%s'", src, atlas);
    return commit_session_ops(&edit, &op, 1, "remove", 1, human, json, quiet);
}
