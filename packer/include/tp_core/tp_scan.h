#ifndef TP_CORE_TP_SCAN_H
#define TP_CORE_TP_SCAN_H

/* Directory-walk + image-extension whitelist -- the single source shared by every
 * frontend that expands a folder source into sprites (arch review §3.1/A2: was
 * duplicated GUI-side). tp_core already does file I/O (project load/save); walking
 * a folder to find sprite images is not new surface area (AGENTS.md: no UI/CLI
 * parsing here, but plain FS access is fine -- see plan risk R6). */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_scan_entry {
    char rel[256];   /* path relative to the scanned root, '/'-normalized (e.g. "tank/walk_01.png") */
    char abs[512];   /* absolute path on disk */
    long long size;  /* file size in bytes */
    long long mtime; /* platform last-write time (opaque; compare for equality only) */
} tp_scan_entry;

typedef struct tp_scan_result {
    tp_scan_entry *entries; /* malloc-owned, sorted by rel; NULL when count == 0 */
    int count;
} tp_scan_result;

/* Recursive listing of image files (.png/.jpg/.jpeg/.bmp/.tga, case-insensitive) under
 * `abs_dir`, sorted by rel path (byte-wise strcmp, not natural order). *out is always
 * fully written (zeroed first) -- a missing/inaccessible dir, a NULL/empty abs_dir, or
 * an OOM mid-walk all yield count == 0 rather than a partial result. Free with
 * tp_scan_free(). */
void tp_scan_dir(const char *abs_dir, tp_scan_result *out);

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
