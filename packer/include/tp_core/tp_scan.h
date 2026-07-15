#ifndef TP_CORE_TP_SCAN_H
#define TP_CORE_TP_SCAN_H

/* Directory-walk + image-extension whitelist -- the single source shared by every
 * frontend that expands a folder source into sprites (arch review §3.1/A2: was
 * duplicated GUI-side). tp_core already does file I/O (project load/save); walking
 * a folder to find sprite images is not new surface area (AGENTS.md: no UI/CLI
 * parsing here, but plain FS access is fine -- see plan risk R6). */

#include <stdbool.h>

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

/* R5b-2: a growable list of malloc-owned, NUL-terminated strings -- the plain-name result of
 * tp_scan_list_dir (the GUI recovery-folder scan joins each name with the folder). Init to {0}
 * before the first list call; free every string + the backing array via tp_str_list_free. */
typedef struct tp_str_list {
    char **items; /* malloc-owned array of malloc-owned names; NULL when count == 0 */
    int count;
    int cap;
} tp_str_list;

/* R5b-2: NON-RECURSIVE listing of the NAMES (not full paths) of REGULAR FILES in `dir` whose name
 * ends with `suffix` (case-SENSITIVE byte compare; "" matches every regular file). Skips "."/".." and
 * subdirectories (regular files only). APPENDS to *out (init to {0} first; the caller joins names with
 * `dir`). Returns false on a dir-OPEN failure (out left as-is -> the recovery scan degrades to no-adopt);
 * true otherwise (possibly zero appended). Deterministic order is NOT guaranteed -- sort before asserting.
 * Reuses the same platform dir-walk (Win32 FindFirstFile / POSIX opendir+stat) as tp_scan_dir. */
bool tp_scan_list_dir(const char *dir, const char *suffix, tp_str_list *out);

/* Frees every string + the backing array and zeroes *out. NULL-safe; safe on a zeroed list. */
void tp_str_list_free(tp_str_list *out);

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
