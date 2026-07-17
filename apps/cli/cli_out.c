#include "cli_out.h"

#include "tp_core/tp_session.h"
#include "tp_core/tp_utf8.h"

#include <stdarg.h>
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
 * (suppressed by --quiet). `has_loc` gates the two reject-only fields so the generic
 * cli_emit_error output stays byte-identical. */
static void emit_error_v(bool json, bool quiet, const char *id, bool has_loc, const char *field, int op_index,
                         const char *fmt, va_list ap) CLI_PRINTF_ATTR(7, 0);

static void emit_error_v(bool json, bool quiet, const char *id, bool has_loc, const char *field, int op_index,
                         const char *fmt, va_list ap) {
    char msg[256];
    (void)vsnprintf(msg, sizeof msg, fmt, ap);

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
        cli_sb_str(&sb, "}}");
        if (sb.oom) { /* a failed grow poisons the builder -> the buffer is TRUNCATED. Never emit a
                       * partial (unparseable) --json line on the machine contract: fall back to a
                       * minimal VALID error object, mirroring cli_emit_mutation's OOM guard. */
            cli_sb_free(&sb);
            (void)fputs("{\"schema\":1,\"error\":{\"id\":\"internal\"}}\n", stdout);
            return;
        }
        cli_out_stdout(&sb);
        cli_sb_free(&sb);
    } else if (!quiet) {
        (void)fprintf(stderr, "ntpacker: error [%s]: %s\n", id, msg);
    }
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
