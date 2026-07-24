#ifndef TP_CORE_TP_SCAN_H
#define TP_CORE_TP_SCAN_H

/* Directory-walk + image-extension whitelist -- the single source shared by every
 * frontend that expands a folder source into sprites (arch review §3.1/A2: was
 * duplicated GUI-side). tp_core already does file I/O (project load/save); walking
 * a folder to find sprite images is not new surface area (AGENTS.md: no UI/CLI
 * parsing here, but plain FS access is fine -- see plan risk R6). */

#include <stdbool.h>
#include <stdint.h>

#include "tp_core/tp_cancel.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_srckey.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_scan_entry {
    char *rel;       /* malloc-owned path relative to root, '/'-normalized */
    char *abs;       /* malloc-owned absolute path on disk */
    long long size;  /* file size in bytes */
    long long mtime; /* platform last-write time (opaque; compare for equality only) */
} tp_scan_entry;

typedef struct tp_scan_result {
    tp_scan_entry *entries; /* malloc-owned, sorted by rel; NULL when count == 0 */
    int count;
} tp_scan_result;

/* Recursive listing of image files (.png/.jpg/.jpeg/.bmp/.tga, case-insensitive)
 * under `abs_dir`, sorted by rel path (byte-wise strcmp, not natural order).
 * Each entry owns exact-size rel/abs strings; rel is bounded by TP_SRCKEY_MAX and
 * abs by TP_IDENTITY_PATH_MAX. *out is zeroed first and remains empty on EVERY
 * failure (bad input, missing/inaccessible/read-failed directory, overflow, or
 * OOM), so callers never consume a partial scan. Free successful results with
 * tp_scan_free(). */
tp_status tp_scan_dir(const char *abs_dir, tp_scan_result *out, tp_error *err);

/* Cancellable form of tp_scan_dir: the recursive walk polls `cancel` once per
 * directory entry, so a caller (e.g. the async pack worker) can interrupt a slow /
 * network directory promptly instead of waiting the whole tree out. A NULL `cancel`
 * (or a token whose callback is NULL) means "never cancel" -- tp_scan_dir() is
 * exactly this with a NULL token. On cancellation the partial walk is freed, *out is
 * left empty, and TP_STATUS_CANCELLED is returned (a clean stop, not a failure). All
 * other semantics match tp_scan_dir. */
tp_status tp_scan_dir_cancellable(const char *abs_dir, tp_scan_result *out,
                                  const tp_cancel_token *cancel, tp_error *err);

/* Frees entries and zeroes *out. Safe to call on an already-empty result; safe on NULL. */
void tp_scan_free(tp_scan_result *out);

/* Streaming counterpart for bounded-memory consumers such as startup recovery. Calls `visit`
 * once per matching regular filename and includes its size so consumers can bound total I/O
 * before opening it. Owns no per-entry heap memory. Returning false from the callback stops
 * enumeration successfully. Directory-open/argument failure returns false. */
typedef bool (*tp_scan_name_visitor)(void *ctx, const char *name, uint64_t size);
bool tp_scan_visit_dir(const char *dir, const char *suffix, tp_scan_name_visitor visit, void *ctx);

/* True if abs points at an existing directory (distinguishes a folder source from a file
 * source without trusting the extension). False for files, missing paths, and NULL. */
bool tp_scan_is_dir(const char *abs);

/* True if `abs` exists on disk (file OR directory). */
bool tp_scan_exists(const char *abs);

typedef enum tp_scan_kind {
    TP_SCAN_KIND_MISSING = 0, /* absent, or the checked probe's error sentinel */
    TP_SCAN_KIND_DIRECTORY,   /* an existing directory (reparse points included) */
    TP_SCAN_KIND_FILE         /* an existing non-directory (regular/other/reparse) */
} tp_scan_kind;

/* Structured one-stat source classification. An absent path returns NOT_FOUND and
 * sets *out to MISSING; permission, I/O, invalid-path, and other stat failures keep
 * their distinct status instead of being collapsed into "missing". */
tp_status tp_scan_classify_checked(const char *abs, tp_scan_kind *out,
                                   tp_error *err);

/* Compatibility value-only classification. Prefer the checked form whenever a
 * caller must distinguish absence from an unreadable/unstatable path. Errors map
 * to MISSING because this legacy API has no status channel. */
tp_scan_kind tp_scan_classify(const char *abs);

/* Stats one regular file through the same UTF-8 filesystem boundary used by
 * directory scanning. Size and platform mtime are opaque comparison values;
 * either output may be NULL. Directories/special/missing paths return false. */
bool tp_scan_file_stat(const char *abs, long long *out_size,
                       long long *out_mtime);

/* mkdir -p of `dir` (every missing ancestor + `dir` itself). Best-effort: an
 * already-existing path or a permission error is ignored (the caller surfaces the
 * real error at the subsequent open/write). NULL/empty -> no-op. Accepts '/' or
 * '\\' separators. The single home for the dir-creation both frontends need before
 * writing target outputs (was copy-pasted GUI-side: gui_pack.c mkdirs_parent). */
void tp_mkdirs(const char *dir);

/* mkdir -p of the PARENT directory of the file path `file_path` (its dirname), so
 * a writer can create `file_path` itself. A path with no separator has no parent
 * -> no-op. */
void tp_mkdirs_parent(const char *file_path);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_SCAN_H */
