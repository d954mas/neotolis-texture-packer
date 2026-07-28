#include "cli_out.h"

#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"

#include <stdarg.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test-only binary: the mutation/error payload builders below start poisoned, so
 * a render takes its allocation-free fallback and the process-level exit/payload
 * contract is exercised without an allocator hook in the shared builder. */
#if defined(NTPACKER_CLI_OUT_FAULT_SEAM)
#define CLI_SB_INIT {.oom = true}
#else
#define CLI_SB_INIT {0}
#endif

void cli_out_stdout(const tp_sb *sb) {
    if (sb->buf && sb->len > 0U) {
        (void)fwrite(sb->buf, 1U, sb->len, stdout);
    }
    (void)fputc('\n', stdout);
}

/* Shared body for cli_emit_error / cli_emit_reject: formats the message once, then
 * emits. JSON mode -> {"schema":1,"error":{"id":...,"message":...[,"field":...,
 * "op_index":...]}} to STDOUT; text mode -> "ntpacker: error [id]: msg" to STDERR
 * (suppressed by --quiet). `has_loc` gates the reject-only fields and `file_io`
 * gates Save-only fields, so generic cli_emit_error stays byte-identical. */
static void emit_error_message(bool json, bool quiet, const char *id,
                               bool has_loc, const char *field, int op_index,
                               const tp_file_io_context *file_io,
                               const char *msg) {
    if (json) {
        tp_sb sb = CLI_SB_INIT;
        tp_sb_str(&sb, "{\"schema\":1,\"error\":{\"id\":");
        tp_sb_json_string(&sb, id);
        tp_sb_str(&sb, ",\"message\":");
        tp_sb_json_string(&sb, msg);
        if (has_loc) {
            tp_sb_str(&sb, ",\"field\":");
            tp_sb_json_string(&sb, field ? field : "");
            tp_sb_str(&sb, ",\"op_index\":");
            tp_sb_int(&sb, op_index);
        }
        if (file_io) {
            tp_sb_str(&sb, ",\"phase\":");
            tp_sb_json_string(&sb, tp_file_io_phase_id(file_io->phase));
            tp_sb_str(&sb, ",\"path\":");
            tp_sb_json_string(&sb, file_io->path ? file_io->path : "");
            tp_sb_str(&sb, ",\"native_code\":");
            tp_sb_int(&sb, file_io->native_code);
        }
        tp_sb_str(&sb, "}}");
        if (sb.oom) {
            tp_sb_free(&sb);
            (void)fputs("{\"schema\":1,\"error\":{\"id\":\"internal\"}}\n",
                        stdout);
            return;
        }
        cli_out_stdout(&sb);
        tp_sb_free(&sb);
    } else if (!quiet) {
        (void)fprintf(stderr, "ntpacker: error [%s]: %s\n", id, msg);
    }
}

static void emit_error_v(bool json, bool quiet, const char *id, bool has_loc,
                         const char *field, int op_index, const char *fmt,
                         va_list ap) CLI_PRINTF_ATTR(7, 0);

static void emit_error_v(bool json, bool quiet, const char *id, bool has_loc, const char *field, int op_index,
                         const char *fmt, va_list ap) {
    char msg[256];
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    emit_error_message(json, quiet, id, has_loc, field, op_index, NULL, msg);
}

void cli_emit_file_io_error(bool json, bool quiet, const tp_error *error) {
    const tp_file_io_context fallback = {TP_FILE_IO_PHASE_NONE, "", 0};
    const tp_file_io_context *context = error ? &error->file_io : &fallback;
    const char *message = error && error->msg[0]
                              ? error->msg
                              : tp_status_str(TP_STATUS_FILE_IO_FAILED);
    emit_error_message(json, quiet, tp_status_id(TP_STATUS_FILE_IO_FAILED),
                       false, "", -1, context, message);
}

void cli_emit_error(bool json, bool quiet, const char *id, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit_error_v(json, quiet, id, false, "", -1, fmt, ap);
    va_end(ap);
}

void cli_emit_reject(bool json, bool quiet, const char *id, const char *field, int op_index, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit_error_v(json, quiet, id, true, field, op_index, fmt, ap);
    va_end(ap);
}

bool cli_emit_mutation(const char *verb, int count,
                       const tp_session_save_result *save_result) {
    tp_sb sb = CLI_SB_INIT;
    tp_sb_str(&sb, "{\"schema\":1,\"ok\":true,\"verb\":");
    tp_sb_json_string(&sb, verb);
    tp_sb_str(&sb, ",\"count\":");
    tp_sb_int(&sb, count);
    const bool file_notice =
        save_result && save_result->file_durability_degraded;
    const bool recovery_notice =
        save_result && save_result->recovery_degraded;
    if (file_notice || recovery_notice) {
        tp_sb_str(&sb, ",\"notices\":[");
        bool comma = false;
        if (file_notice) {
            tp_sb_str(
                &sb,
                "{\"id\":\"file_durability_uncertain\",\"message\":");
            tp_sb_json_string(
                &sb,
                "project file was published, but storage durability could not be confirmed");
            tp_sb_str(&sb, ",\"status\":");
            tp_sb_json_string(
                &sb, tp_status_id(save_result->file_durability_status));
            tp_sb_char(&sb, '}');
            comma = true;
        }
        if (recovery_notice) {
            if (comma) {
                tp_sb_char(&sb, ',');
            }
            tp_sb_str(&sb, "{\"id\":\"recovery_degraded\",\"message\":");
            tp_sb_json_string(
                &sb,
                "project was saved, but crash recovery is degraded");
            tp_sb_str(&sb, ",\"status\":");
            tp_sb_json_string(&sb,
                            tp_status_id(save_result->recovery_status));
            tp_sb_char(&sb, '}');
        }
        tp_sb_char(&sb, ']');
    }
    tp_sb_char(&sb, '}');
    if (sb.oom) { /* the payload is tiny; OOM here is near-impossible, but never crash */
        tp_sb_free(&sb);
        return false;
    }
    cli_out_stdout(&sb);
    tp_sb_free(&sb);
    return true;
}

static void preview_revision(tp_sb *sb, int64_t revision) {
    char text[32];
    (void)snprintf(text, sizeof text, "%" PRId64, revision);
    tp_sb_str(sb, text);
}

static void preview_id(tp_sb *sb, tp_id_kind kind, tp_id128 id) {
    char text[TP_ID_TEXT_CAP];
    tp_error err = {0};
    if (tp_id_format(kind, id, text, sizeof text, &err) == TP_STATUS_OK) {
        tp_sb_json_string(sb, text);
    }
}

bool cli_emit_mutation_preview(const char *command,
                               const tp_txn_result *result,
                               int64_t revision_before,
                               const tp_id_kind *generated_kinds,
                               const tp_id128 *generated_ids,
                               int generated_count,
                               const char *generated_ids_semantics) {
    tp_sb sb = CLI_SB_INIT;
    tp_sb_str(&sb, "{\"schema\":2,\"command\":");
    tp_sb_json_string(&sb, command);
    tp_sb_str(&sb, ",\"dry_run\":true,\"would_change\":");
    tp_sb_str(&sb, result && result->no_change ? "false" : "true");
    tp_sb_str(&sb, ",\"operation_count\":");
    tp_sb_int(&sb, result ? result->op_count : 0);
    tp_sb_str(&sb, ",\"revision_before\":");
    preview_revision(&sb, revision_before);
    tp_sb_str(&sb, ",\"revision_after\":");
    preview_revision(&sb, result ? result->revision : revision_before);
    tp_sb_str(&sb, ",\"affected_ids\":[");
    bool comma = false;
    if (result && !result->no_change) {
        for (int oi = 0; oi < result->op_count; ++oi) {
            for (int ai = 0; ai < result->ops[oi].addr_count; ++ai) {
                const tp_txn_addr *addr = &result->ops[oi].addr[ai];
                if (addr->idk == TP_ID_KIND_INVALID) {
                    continue;
                }
                bool duplicate = false;
                for (int po = 0; po <= oi && !duplicate; ++po) {
                    const int limit = po == oi ? ai : result->ops[po].addr_count;
                    for (int pa = 0; pa < limit; ++pa) {
                        const tp_txn_addr *prior = &result->ops[po].addr[pa];
                        if (prior->idk == addr->idk && tp_id128_eq(prior->id, addr->id)) {
                            duplicate = true;
                            break;
                        }
                    }
                }
                if (duplicate) {
                    continue;
                }
                if (comma) {
                    tp_sb_char(&sb, ',');
                }
                preview_id(&sb, addr->idk, addr->id);
                comma = true;
            }
        }
    }
    tp_sb_str(&sb, "],\"generated_ids\":[");
    comma = false;
    for (int i = 0; i < generated_count; ++i) {
        if (comma) {
            tp_sb_char(&sb, ',');
        }
        preview_id(&sb, generated_kinds[i], generated_ids[i]);
        comma = true;
    }
    if (result && !result->no_change) {
        for (int oi = 0; oi < result->op_count; ++oi) {
            const tp_op_info *info = tp_op_info_by_wire(result->ops[oi].wire);
            if (!info || info->effect != TP_OP_CLASS_CREATE) {
                continue;
            }
            for (int ai = 0; ai < result->ops[oi].addr_count; ++ai) {
                const tp_txn_addr *addr = &result->ops[oi].addr[ai];
                if (addr->idk != info->target_kind) {
                    continue;
                }
                if (comma) {
                    tp_sb_char(&sb, ',');
                }
                preview_id(&sb, addr->idk, addr->id);
                comma = true;
                break;
            }
        }
    }
    tp_sb_char(&sb, ']');
    if (generated_ids_semantics) {
        tp_sb_str(&sb, ",\"generated_ids_semantics\":");
        tp_sb_json_string(&sb, generated_ids_semantics);
    }
    tp_sb_str(&sb, ",\"notices\":[]}");
    if (sb.oom) {
        tp_sb_free(&sb);
        return false;
    }
    cli_out_stdout(&sb);
    tp_sb_free(&sb);
    return true;
}

void cli_emit_mutation_output_oom(bool side_effects) {
    if (side_effects) {
        (void)fputs(
            "{\"schema\":1,\"error\":{\"id\":\"oom\",\"message\":\"out of memory rendering mutation result after mutation was applied\",\"phase\":\"output\",\"side_effects\":true,\"mutation_state\":\"applied\"}}\n",
            stdout);
    } else {
        (void)fputs(
            "{\"schema\":1,\"error\":{\"id\":\"oom\",\"message\":\"out of memory rendering mutation preview\",\"phase\":\"output\",\"side_effects\":false,\"mutation_state\":\"not_applied\"}}\n",
            stdout);
    }
}
