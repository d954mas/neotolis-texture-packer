#ifndef NTPACKER_CLI_CMDS_H
#define NTPACKER_CLI_CMDS_H

/* B2 read-verb entry points + the shared immutable project loader. Each verb owns its
 * --json payload (versioned schema) and human summary; the loader centralizes the
 * exit-3 structured error so inspect/validate/pack report a load failure identically
 * (plan "CLI v1 contract", ai-first.md items 2/4/7). */

#include <stdbool.h>

struct tp_session_snapshot;

/* Shared query-payload schema for `inspect --json` AND the `anim list --json` query
 * (its animation shape mirrors inspect's). SINGLE source of truth -- cli_inspect.c,
 * cli_mutate.c's anim_list, and main.c's `version --json` manifest all read this so
 * the number can never drift between the payloads and their advertised version.
 *
 * Bumped 1 -> 2: the animation object changed field semantics -- `.id` is now
 * an opaque structural shape-ID and the human/selector name moved to a new `.name`
 * field. Mutation verbs still select an animation BY NAME (`anim <name>`); id-based
 * selectors arrive later. An AI operator branches on this number to detect the break.
 *
 * Bumped 2 -> 3: the inspect `sources[]` object gained `id` (structural
 * source shape-ID) and `stored_kind` (persisted folder/file classification) keys --
 * a versioned-contract change, same reason the anim reshape bumped the number.
 * Bumped 3 -> 4: each resolved `sprites[]` entry gained `sprite_id` (the
 * derived deterministic id) and `source` (owning source shape-ID) keys. */
#define CLI_INSPECT_SCHEMA 4

/* `validate --json` schema 2 adds exact report-owned contexts plus stable
 * structural atlas/source/animation/target IDs. */
#define CLI_VALIDATE_SCHEMA 2

/* Loads `path` as an immutable snapshot into *out without taking the writer
 * lease. On failure emits a structured error (id = tp_status_id,
 * message = tp_error prose) honoring --json/--quiet, and returns the CLI exit code
 * (CLI_EXIT_PROJECT for a load/parse error, CLI_EXIT_INTERNAL for OOM). On success
 * returns CLI_EXIT_OK and the caller owns *out (tp_session_snapshot_destroy). */
int cli_load_snapshot(const char *path, bool json, bool quiet,
                      struct tp_session_snapshot **out);

/* inspect <project> [--json]: dump project state. Human output is cosmetic; the
 * --json payload (CLI_INSPECT_SCHEMA) is the contract. Returns CLI_EXIT_OK / _PROJECT / _INTERNAL. */
int cmd_inspect(const char *path, bool json, bool quiet);

/* validate <project> [--json] [--strict]: report every finding in one run. Exit 0
 * when the file parses and validation runs (findings live in the payload); exit 7
 * only when --strict AND at least one error-severity finding; exit 3 on load
 * failure (plan L-1). */
int cmd_validate(const char *path, bool json, bool quiet, bool strict);

/* pack <project> [--atlas <name>] [--target <id>] [--out-dir <dir>] [--dry-run]
 * [--json] [--quiet] (alias: export). Packs + exports every enabled target of every
 * atlas via the shared op layer and writes a structured report. `dry_run` packs +
 * predicts but writes NO files (report carries per-target would_write + predicted
 * notices; no dirs are created). Exit codes: 0 ok, 2 usage (unknown --atlas), 3
 * project load, 4 pack failure, 5 export failure, 6 partial. `opt_atlas`/
 * `opt_target`/`opt_out_dir` are NULL when the flag was not given. */
int cmd_pack(const char *path, const char *opt_atlas, const char *opt_target, const char *opt_out_dir, bool dry_run,
             bool json, bool quiet);

/* B4 wave-2 mutation verbs (new/add/remove/set/sprite/anim/target/atlas). `positionals`
 * is the whole operand vector (positionals[0] == the verb); `npos` its length; `opt_at`
 * is the `anim add-frame --at N` value (NULL when absent). Each verb loads the project,
 * applies typed session operations and re-saves (byte-stable). Exit codes: 0 ok, 2
 * usage (bad grammar/vocabulary/value), 3 project (load error, `new`-on-existing, or a
 * selector/state mutator failure), 1 internal (OOM). `anim list` is a read-only query. */
int cmd_mutate(int npos, const char *const *positionals, const char *opt_at, bool json, bool quiet);

#endif /* NTPACKER_CLI_CMDS_H */
