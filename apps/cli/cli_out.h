#ifndef NTPACKER_CLI_OUT_H
#define NTPACKER_CLI_OUT_H

#include <stdbool.h>
#include <stddef.h>

/* Minimal local JSON/text string builder for the CLI. Deliberately NOT
 * packer/src/tp_sb.h -- that is a tp_core-PRIVATE header and apps/ must not
 * reach into src/. Same output conventions (LF, %.9g floats, escaped strings).
 * Whether a public tp_core JSON writer is warranted is revisited in B3. */
typedef struct cli_sb {
    char *buf;
    size_t len;
    size_t cap;
    bool oom; /* sticky: a failed grow poisons the builder; callers check before emit */
} cli_sb;

void cli_sb_free(cli_sb *sb);
void cli_sb_putc(cli_sb *sb, char c);
void cli_sb_str(cli_sb *sb, const char *s);
void cli_sb_int(cli_sb *sb, long v);
void cli_sb_num(cli_sb *sb, double v);           /* %.9g; relies on LC_NUMERIC="C" (main) */
void cli_sb_json_str(cli_sb *sb, const char *s); /* quoted + JSON-escaped */
void cli_sb_indent(cli_sb *sb, int depth);       /* 2 spaces per depth level */

/* Writes sb's bytes to stdout followed by one trailing newline. */
void cli_out_stdout(const cli_sb *sb);

#if defined(__GNUC__) || defined(__clang__)
#define CLI_PRINTF_ATTR(f, a) __attribute__((format(printf, f, a)))
#else
#define CLI_PRINTF_ATTR(f, a)
#endif

/* One-line structured error. --json: the error IS the payload, emitted to STDOUT
 * as {"schema":1,"error":{"id":...,"message":...}} (an agent parses stdout).
 * Text mode: "ntpacker: error [id]: msg" to STDERR, suppressed by --quiet. Never
 * writes both streams. `id` is a stable machine token (usage/tp_status_id/...). */
void cli_emit_error(bool json, bool quiet, const char *id, const char *fmt, ...) CLI_PRINTF_ATTR(4, 5);

/* Minimal, shared success payload for the B4 mutation verbs, emitted to STDOUT:
 * {"schema":1,"ok":true,"verb":"<verb>","count":<count>}. Defined once, kept tiny
 * (plan B4 item 8). `count` = the number of primary items affected (sources added,
 * frames added, ...); a scalar edit passes 1. */
void cli_emit_mutation(const char *verb, int count);

#endif /* NTPACKER_CLI_OUT_H */
