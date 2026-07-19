#include "cli_out.h"

#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "tp_core/tp_utf8.h"

#include <stdarg.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sb_reserve(cli_sb *sb, size_t extra) {
    if (sb->oom) {
        return;
    }
    size_t need = sb->len + extra + 1U;
    if (need <= sb->cap) {
        return;
    }
    size_t new_cap = (sb->cap == 0U) ? 256U : sb->cap;
    while (new_cap < need) {
        new_cap *= 2U;
    }
    char *nb = (char *)realloc(sb->buf, new_cap);
    if (!nb) {
        sb->oom = true;
        return;
    }
    sb->buf = nb;
    sb->cap = new_cap;
}

static void sb_write(cli_sb *sb, const char *s, size_t n) {
    sb_reserve(sb, n);
    if (sb->oom) {
        return;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

void cli_sb_free(cli_sb *sb) {
    if (sb) {
        free(sb->buf);
        sb->buf = NULL;
        sb->len = 0U;
        sb->cap = 0U;
        sb->oom = false;
    }
}

void cli_sb_putc(cli_sb *sb, char c) { sb_write(sb, &c, 1U); }

void cli_sb_str(cli_sb *sb, const char *s) { sb_write(sb, s, strlen(s)); }

void cli_sb_int(cli_sb *sb, long v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%ld", v);
    if (n > 0) {
        sb_write(sb, tmp, (size_t)n);
    }
}

void cli_sb_size(cli_sb *sb, size_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%zu", v);
    if (n > 0) {
        sb_write(sb, tmp, (size_t)n);
    }
}

void cli_sb_num(cli_sb *sb, double v) {
    char tmp[64];
    int n = snprintf(tmp, sizeof tmp, "%.9g", v);
    if (n > 0) {
        sb_write(sb, tmp, (size_t)n);
    }
}

void cli_sb_indent(cli_sb *sb, int depth) {
    for (int i = 0; i < depth; i++) {
        sb_write(sb, "  ", 2U);
    }
}

void cli_sb_json_str(cli_sb *sb, const char *s) {
    if (!s) {
        s = "";
    }
    const size_t length = strlen(s);
    cli_sb_putc(sb, '"');
    for (size_t offset = 0U; offset < length;) {
        const unsigned char c = (unsigned char)s[offset];
        if (c >= 0x80U) {
            const size_t width =
                tp_utf8_codepoint_width(s + offset, length - offset);
            if (width == 0U) {
                cli_sb_str(sb, "\\ufffd");
                offset++;
            } else {
                sb_write(sb, s + offset, width);
                offset += width;
            }
            continue;
        }
        switch (c) {
            case '"': cli_sb_str(sb, "\\\""); break;
            case '\\': cli_sb_str(sb, "\\\\"); break;
            case '\b': cli_sb_str(sb, "\\b"); break;
            case '\f': cli_sb_str(sb, "\\f"); break;
            case '\n': cli_sb_str(sb, "\\n"); break;
            case '\r': cli_sb_str(sb, "\\r"); break;
            case '\t': cli_sb_str(sb, "\\t"); break;
            default:
                if (c < 0x20U) {
                    char esc[8];
                    int n = snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
                    if (n > 0) {
                        sb_write(sb, esc, (size_t)n);
                    }
                } else {
                    cli_sb_putc(sb, (char)c);
                }
                break;
        }
        offset++;
    }
    cli_sb_putc(sb, '"');
}

void cli_out_stdout(const cli_sb *sb) {
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
        cli_sb sb = {0};
        cli_sb_str(&sb, "{\"schema\":1,\"error\":{\"id\":");
        cli_sb_json_str(&sb, id);
        cli_sb_str(&sb, ",\"message\":");
        cli_sb_json_str(&sb, msg);
        if (has_loc) {
            cli_sb_str(&sb, ",\"field\":");
            cli_sb_json_str(&sb, field ? field : "");
            cli_sb_str(&sb, ",\"op_index\":");
            cli_sb_int(&sb, op_index);
        }
        if (file_io) {
            cli_sb_str(&sb, ",\"phase\":");
            cli_sb_json_str(&sb, tp_file_io_phase_id(file_io->phase));
            cli_sb_str(&sb, ",\"path\":");
            cli_sb_json_str(&sb, file_io->path ? file_io->path : "");
            cli_sb_str(&sb, ",\"native_code\":");
            cli_sb_int(&sb, file_io->native_code);
        }
        cli_sb_str(&sb, "}}");
        if (sb.oom) {
            cli_sb_free(&sb);
            (void)fputs("{\"schema\":1,\"error\":{\"id\":\"internal\"}}\n",
                        stdout);
            return;
        }
        cli_out_stdout(&sb);
        cli_sb_free(&sb);
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

void cli_emit_mutation(const char *verb, int count,
                       const tp_session_save_result *save_result) {
    cli_sb sb = {0};
    cli_sb_str(&sb, "{\"schema\":1,\"ok\":true,\"verb\":");
    cli_sb_json_str(&sb, verb);
    cli_sb_str(&sb, ",\"count\":");
    cli_sb_int(&sb, count);
    const bool file_notice =
        save_result && save_result->file_durability_degraded;
    const bool recovery_notice =
        save_result && save_result->recovery_degraded;
    if (file_notice || recovery_notice) {
        cli_sb_str(&sb, ",\"notices\":[");
        bool comma = false;
        if (file_notice) {
            cli_sb_str(
                &sb,
                "{\"id\":\"file_durability_uncertain\",\"message\":");
            cli_sb_json_str(
                &sb,
                "project file was published, but storage durability could not be confirmed");
            cli_sb_str(&sb, ",\"status\":");
            cli_sb_json_str(
                &sb, tp_status_id(save_result->file_durability_status));
            cli_sb_putc(&sb, '}');
            comma = true;
        }
        if (recovery_notice) {
            if (comma) {
                cli_sb_putc(&sb, ',');
            }
            cli_sb_str(&sb, "{\"id\":\"recovery_degraded\",\"message\":");
            cli_sb_json_str(
                &sb,
                "project was saved, but crash recovery is degraded");
            cli_sb_str(&sb, ",\"status\":");
            cli_sb_json_str(&sb,
                            tp_status_id(save_result->recovery_status));
            cli_sb_putc(&sb, '}');
        }
        cli_sb_putc(&sb, ']');
    }
    cli_sb_putc(&sb, '}');
    if (sb.oom) { /* the payload is tiny; OOM here is near-impossible, but never crash */
        cli_sb_free(&sb);
        (void)fputs("{\"schema\":1,\"ok\":true}\n", stdout);
        return;
    }
    cli_out_stdout(&sb);
    cli_sb_free(&sb);
}

static void preview_revision(cli_sb *sb, int64_t revision) {
    char text[32];
    (void)snprintf(text, sizeof text, "%" PRId64, revision);
    cli_sb_str(sb, text);
}

static void preview_id(cli_sb *sb, tp_id_kind kind, tp_id128 id) {
    char text[TP_ID_TEXT_CAP];
    tp_error err = {0};
    if (tp_id_format(kind, id, text, sizeof text, &err) == TP_STATUS_OK) {
        cli_sb_json_str(sb, text);
    }
}

void cli_emit_mutation_preview(const char *command,
                               const tp_txn_result *result,
                               int64_t revision_before,
                               const tp_id_kind *generated_kinds,
                               const tp_id128 *generated_ids,
                               int generated_count) {
    cli_sb sb = {0};
    cli_sb_str(&sb, "{\"schema\":2,\"command\":");
    cli_sb_json_str(&sb, command);
    cli_sb_str(&sb, ",\"dry_run\":true,\"would_change\":");
    cli_sb_str(&sb, result && result->no_change ? "false" : "true");
    cli_sb_str(&sb, ",\"operation_count\":");
    cli_sb_int(&sb, result ? result->op_count : 0);
    cli_sb_str(&sb, ",\"revision_before\":");
    preview_revision(&sb, revision_before);
    cli_sb_str(&sb, ",\"revision_after\":");
    preview_revision(&sb, result ? result->revision : revision_before);
    cli_sb_str(&sb, ",\"affected_ids\":[");
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
                    cli_sb_putc(&sb, ',');
                }
                preview_id(&sb, addr->idk, addr->id);
                comma = true;
            }
        }
    }
    cli_sb_str(&sb, "],\"generated_ids\":[");
    comma = false;
    for (int i = 0; i < generated_count; ++i) {
        if (comma) {
            cli_sb_putc(&sb, ',');
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
                    cli_sb_putc(&sb, ',');
                }
                preview_id(&sb, addr->idk, addr->id);
                comma = true;
                break;
            }
        }
    }
    cli_sb_str(&sb, "],\"notices\":[]}");
    if (sb.oom) {
        cli_sb_free(&sb);
        (void)fputs("{\"schema\":2,\"command\":\"mutation\",\"dry_run\":true,\"would_change\":true,\"operation_count\":0,\"revision_before\":0,\"revision_after\":0,\"affected_ids\":[],\"generated_ids\":[],\"notices\":[]}\n", stdout);
        return;
    }
    cli_out_stdout(&sb);
    cli_sb_free(&sb);
}
