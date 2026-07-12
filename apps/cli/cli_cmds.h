#ifndef NTPACKER_CLI_CMDS_H
#define NTPACKER_CLI_CMDS_H

/* B2 read-verb entry points + the shared project loader. Each verb owns its
 * --json payload (versioned schema) and human summary; the loader centralizes the
 * exit-3 structured error so inspect/validate report a load failure identically
 * (plan "CLI v1 contract", ai-first.md items 2/4/7). */

#include <stdbool.h>

struct tp_project;

/* Loads `path` into *out. On failure emits a structured error (id = tp_status_id,
 * message = tp_error prose) honoring --json/--quiet, and returns the CLI exit code
 * (CLI_EXIT_PROJECT for a load/parse error, CLI_EXIT_INTERNAL for OOM). On success
 * returns CLI_EXIT_OK and the caller owns *out (tp_project_destroy). */
int cli_load_project(const char *path, bool json, bool quiet, struct tp_project **out);

/* inspect <project> [--json]: dump project state. Human output is cosmetic; the
 * --json payload (schema 1) is the contract. Returns CLI_EXIT_OK / _PROJECT / _INTERNAL. */
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
 * mutates via a tp_project_* mutator, and re-saves (byte-stable). Exit codes: 0 ok, 2
 * usage (bad grammar/vocabulary/value), 3 project (load error, `new`-on-existing, or a
 * selector/state mutator failure), 1 internal (OOM). `anim list` is a read-only query. */
int cmd_mutate(int npos, const char *const *positionals, const char *opt_at, bool json, bool quiet);

#endif /* NTPACKER_CLI_CMDS_H */
