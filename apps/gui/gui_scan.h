#ifndef NTPACKER_GUI_SCAN_H
#define NTPACKER_GUI_SCAN_H

#include <stdbool.h>

/* DISPLAY-ONLY recursive folder enumeration for the sprite panel (ux.md region D,
 * Task 3). A FOLDER source stores only its path in the model; the GUI scans it from
 * disk purely to SHOW child sprites. Scanning-for-packing stays a tp_core concern --
 * this module never feeds the packer, it only fills the left panel's folder rows.
 *
 * Win32 FindFirstFile/FindNextFile with a POSIX opendir/readdir fallback. Results are
 * cached per absolute directory and reused every frame; gui_scan_invalidate_all() drops
 * the cache after any source-set mutation (add/remove/open/new). */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gui_scan_entry {
    char rel[256];       /* path relative to the scanned root, '/'-normalized (e.g. "tank/walk_01.png") */
    char abs[512];       /* absolute path on disk (what the canvas decodes) */
    long long size;      /* file size in bytes (change detection for Refresh) */
    long long mtime;     /* platform last-write time (opaque; compared for equality only) */
} gui_scan_entry;

typedef struct gui_scan_result {
    gui_scan_entry *entries; /* sorted by rel for deterministic display; NULL when count==0 */
    int count;
} gui_scan_result;

/* Recursive listing of image files (.png/.jpg/.jpeg/.bmp/.tga, case-insensitive) under
 * abs_dir. Never NULL; count==0 for an empty/missing/inaccessible dir. The result is
 * owned by the module (cached) -- do NOT free or mutate it. */
const gui_scan_result *gui_scan_get(const char *abs_dir);

/* True if abs points at an existing directory (distinguishes a folder source from a file
 * source without trusting the extension). False for files, missing paths, and NULL. */
bool gui_scan_is_dir(const char *abs);

/* True if `abs` exists on disk (file OR directory). Used to render missing-file rows
 * (ux.md §3.7) and to keep a stale argv Open from being fatal (F6b). */
bool gui_scan_exists(const char *abs);

/* Stats a FILE at `abs`: writes size + platform mtime, returns true if it exists as a
 * regular file. Change detection for Refresh (F4). Any out pointer may be NULL. */
bool gui_scan_stat(const char *abs, long long *out_size, long long *out_mtime);

/* Drops every cached scan. Cheap; the next gui_scan_get rescans from disk. Call after any
 * mutation that could change what a folder contains or which folders exist. */
void gui_scan_invalidate_all(void);

/* Frees all cache memory (shutdown). */
void gui_scan_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_SCAN_H */
