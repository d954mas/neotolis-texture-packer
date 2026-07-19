#ifndef NTPACKER_CLI_OUT_H
#define NTPACKER_CLI_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

struct tp_session_save_result;
struct tp_txn_result;

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
void cli_sb_size(cli_sb *sb, size_t v);
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

/* Typed pre-publication Save error. JSON adds stable phase/path/native_code
 * fields; text keeps the ordinary one-line error form. */
void cli_emit_file_io_error(bool json, bool quiet, const tp_error *error);

/* Structured error for a REJECTED transaction: like cli_emit_error but the JSON
 * payload also carries the offending op's `field` (closed-vocabulary key; "" if
 * none) and `op_index` (>=0 = op position; -1 = envelope/revision level). Only the
 * transaction-reject path uses this. Text mode (stderr) is IDENTICAL to cli_emit_error. */
void cli_emit_reject(bool json, bool quiet, const char *id, const char *field, int op_index, const char *fmt, ...)
    CLI_PRINTF_ATTR(6, 7);

/* Minimal, shared success payload for the B4 mutation verbs, emitted to STDOUT:
 * {"schema":1,"ok":true,"verb":"<verb>","count":<count>}. Defined once, kept tiny
 * (plan B4 item 8). `count` = the number of primary items affected (sources added,
 * frames added, ...); a scalar edit passes 1. Save degradation is appended as
 * a structured `notices` array without turning an already-published mutation
 * into an error. Returns false without writing a payload if rendering runs out
 * of memory; the caller then owns the allocation-free error + exit contract. */
bool cli_emit_mutation(const char *verb, int count,
                       const struct tp_session_save_result *save_result);

bool cli_emit_mutation_preview(const char *command,
                               const struct tp_txn_result *result,
                               int64_t revision_before,
                               const tp_id_kind *generated_kinds,
                               const tp_id128 *generated_ids,
                               int generated_count);

/* Allocation-free structured fallback for an OOM at the mutation JSON output
 * boundary. Apply callers invoke this only after Save/Save As succeeds, so the
 * exact applied/not_applied state is explicit in prose and machine fields. */
void cli_emit_mutation_output_oom(bool side_effects);

#endif /* NTPACKER_CLI_OUT_H */
