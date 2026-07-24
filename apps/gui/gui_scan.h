#ifndef NTPACKER_GUI_SCAN_H
#define NTPACKER_GUI_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "tp_core/tp_scan.h"

/* DISPLAY-ONLY recursive folder enumeration for the sprite panel (ux.md region D,
 * Task 3). A FOLDER source stores only its path in the model; the GUI scans it from
 * disk purely to SHOW child sprites. Scanning-for-packing stays a tp_core concern --
 * this module never feeds the packer, it only fills the left panel's folder rows.
 *
 * The walk itself (Win32 FindFirstFile/FindNextFile with a POSIX opendir/readdir
 * fallback, image-ext whitelist) lives in tp_core/tp_scan.h -- this module is now just
 * a per-directory result cache reused every frame; gui_scan_invalidate_all() drops the
 * cache after any source-set mutation (add/remove/open/new). */

#ifdef __cplusplus
extern "C" {
#endif

typedef tp_scan_entry gui_scan_entry;
typedef tp_scan_result gui_scan_result;

/* Recursive listing of image files under abs_dir. On success, *out_result is a
 * non-NULL module-owned cached result (possibly empty). Scan/open/read/OOM/path
 * failures are returned exactly and are never cached as an empty success. */
tp_status gui_scan_get(const char *abs_dir,
                       const gui_scan_result **out_result, tp_error *err);

/* True if abs points at an existing directory (distinguishes a folder source from a file
 * source without trusting the extension). False for files, missing paths, and NULL. */
bool gui_scan_is_dir(const char *abs);

/* True if `abs` exists on disk (file OR directory). Used to render missing-file rows
 * (ux.md §3.7) and to keep a stale argv Open from being fatal (F6b). */
bool gui_scan_exists(const char *abs);

/* Typed form used by source rows: preserves NOT_FOUND versus probe failures
 * while retaining the same benchmark accounting as gui_scan_exists(). */
tp_status gui_scan_classify_checked(const char *abs, tp_scan_kind *out,
                                    tp_error *err);

/* Stats a FILE at `abs`: writes size + platform mtime, returns true if it exists as a
 * regular file. Change detection for Refresh (F4). Any out pointer may be NULL. */
bool gui_scan_stat(const char *abs, long long *out_size, long long *out_mtime);

/* Drops every cached scan. Cheap; the next gui_scan_get rescans from disk. Call after any
 * mutation that could change what a folder contains or which folders exist. */
void gui_scan_invalidate_all(void);

/* Frees all cache memory (shutdown). */
void gui_scan_shutdown(void);

#if defined(NTPACKER_GUI_BENCH)
typedef struct gui_scan_bench_counters {
    uint64_t get_calls;
    uint64_t directory_walks;
    uint64_t exists_fs_calls;
    uint64_t is_dir_fs_calls;
} gui_scan_bench_counters;

/* Seeds one cached directory without walking it. Takes ownership of
 * owned_result->entries on success and zeroes *owned_result. */
bool gui_scan_bench_seed_owned(const char *abs_dir, gui_scan_result *owned_result);
void gui_scan_bench_reset_counters(void);
gui_scan_bench_counters gui_scan_bench_get_counters(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_SCAN_H */
