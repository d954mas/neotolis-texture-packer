#include "cli_out.h"

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

void cli_sb_num(cli_sb *sb, double v) {
    char tmp[64];
    int n = snprintf(tmp, sizeof tmp, "%.9g", v);
    if (n > 0) {
        sb_write(sb, tmp, (size_t)n);
    }
}

void cli_sb_json_str(cli_sb *sb, const char *s) {
    cli_sb_putc(sb, '"');
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        switch (*c) {
            case '"': cli_sb_str(sb, "\\\""); break;
            case '\\': cli_sb_str(sb, "\\\\"); break;
            case '\b': cli_sb_str(sb, "\\b"); break;
            case '\f': cli_sb_str(sb, "\\f"); break;
            case '\n': cli_sb_str(sb, "\\n"); break;
            case '\r': cli_sb_str(sb, "\\r"); break;
            case '\t': cli_sb_str(sb, "\\t"); break;
            default:
                if (*c < 0x20U) {
                    char esc[8];
                    int n = snprintf(esc, sizeof esc, "\\u%04x", (unsigned)*c);
                    if (n > 0) {
                        sb_write(sb, esc, (size_t)n);
                    }
                } else {
                    cli_sb_putc(sb, (char)*c);
                }
                break;
        }
    }
    cli_sb_putc(sb, '"');
}

void cli_out_stdout(const cli_sb *sb) {
    if (sb->buf && sb->len > 0U) {
        (void)fwrite(sb->buf, 1U, sb->len, stdout);
    }
    (void)fputc('\n', stdout);
}

void cli_emit_error(bool json, bool quiet, const char *id, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (json) {
        cli_sb sb = {0};
        cli_sb_str(&sb, "{\"schema\":1,\"error\":{\"id\":");
        cli_sb_json_str(&sb, id);
        cli_sb_str(&sb, ",\"message\":");
        cli_sb_json_str(&sb, msg);
        cli_sb_str(&sb, "}}");
        cli_out_stdout(&sb);
        cli_sb_free(&sb);
    } else if (!quiet) {
        (void)fprintf(stderr, "ntpacker: error [%s]: %s\n", id, msg);
    }
}
