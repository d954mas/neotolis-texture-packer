#ifndef TP_FS_INTERNAL_H
#define TP_FS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "tp_core/tp_fs.h"

/* Internal UTF-8 filesystem boundary. Public/core paths stay UTF-8 on every
 * host; this module is the only generic packer-side place that translates them
 * to UTF-16 for Windows filesystem calls. */

typedef enum tp_fs_kind {
    TP_FS_KIND_OTHER = 0,
    TP_FS_KIND_REGULAR,
    TP_FS_KIND_DIRECTORY
} tp_fs_kind;

typedef struct tp_fs_info {
    tp_fs_kind kind;
    uint64_t size;
    int64_t mtime;
    bool reparse;
} tp_fs_info;

/* Validates the complete path before any multi-step caller starts mutating
 * prefixes. On failure returns false and sets errno to EINVAL or EILSEQ. */
bool tp_fs_path_is_valid_utf8(const char *path_utf8);

FILE *tp_fs_fopen(const char *path_utf8, const char *mode);
FILE *tp_fs_create_exclusive(const char *path_utf8, bool read_write);
bool tp_fs_read_all(FILE *file, void *data, size_t size);
bool tp_fs_write_all(FILE *file, const void *data, size_t size);
bool tp_fs_flush(FILE *file); /* checked stdio flush */
bool tp_fs_sync(FILE *file);  /* checked flush + durable file sync */
bool tp_fs_close(FILE *file);
bool tp_fs_write_file(const char *path_utf8, const void *data, size_t size);

/* Longest path this module composes out of a canonical path: the canonical limit
 * plus the longest private suffix below (".tp-old-<hexpid>-<hexserial>"). */
#define TP_FS_STAGE_PATH_MAX 4160 /* TP_IDENTITY_PATH_MAX + a private suffix */

/* ------------------------------------------------------------------ */
/* Staged output-SET publish (export set-atomicity).                    */
/* ------------------------------------------------------------------ */
/*
 * The publisher (tp_export_write_and_publish_set) owns the policy; this module
 * owns the three names it needs and the cross-run reaper. Every private name is
 * "<something>.tp-<role>-<hexpid>-<hexserial>", so a leftover always says which
 * process owned it and the reaper can decide by liveness alone.
 *
 * Staging is a SIBLING of the output directory on purpose: promotion must be a
 * pure rename, which requires the same volume, and the caller's request/work dir
 * is frequently on a different one.
 */

/* Creates a private ".tp-stage-<hexpid>-<hexserial>" directory under
 * `parent_utf8`, which must be "" or end with a path separator ("" means the
 * current directory). The exclusive create is the single atomic step that
 * decides ownership, so a leftover or foreign name is skipped, never adopted. */
bool tp_fs_stage_dir_create(const char *parent_utf8, char *out, size_t out_cap);

/* Maps a DIRECT child of `final_dir_utf8` ("" or trailing-separator form, as
 * above) to its staged sibling under `staging_dir_utf8`. False when `path_utf8`
 * is not a direct child or the result would overflow `out_cap`. */
bool tp_fs_stage_child_path(const char *final_dir_utf8,
                            const char *staging_dir_utf8,
                            const char *path_utf8, char *out, size_t out_cap);

/* Builds the DISPLACED name for `destination_utf8`:
 * "<destination>.tp-old-<hexpid>-<hexserial>", a sibling of the destination (so
 * the swap back is also a pure rename). Each call yields a fresh serial. False
 * only when the result would overflow `out_cap`. */
bool tp_fs_stage_old_path(const char *destination_utf8, char *out,
                          size_t out_cap);

/* Best-effort cross-run cleanup of the two private names above under
 * `parent_utf8` ("" or trailing-separator form). Only entries whose embedded pid
 * is definitively gone are touched, so a concurrent exporter's work is never
 * disturbed. A ".tp-stage-*" directory is removed. A ".tp-old-*" file is the
 * crash-mid-swap record: its destination present means the swap completed and
 * the old copy is deleted; its destination missing means the swap was
 * interrupted and the old copy is renamed back. Silent; never fails a run. */
void tp_fs_stage_reap_orphans(const char *parent_utf8);

/* True unless a process with `pid` is definitively gone. Conservative on
 * purpose: an access-denied or otherwise unanswerable probe reports LIVE, so the
 * reaper above never removes a directory that might still be in use. */
bool tp_fs_process_is_live(unsigned long pid);
/* Best-effort recursive removal; never follows directory reparse points. */
void tp_fs_remove_tree(const char *path_utf8);

bool tp_fs_stat(const char *path_utf8, tp_fs_info *out);
bool tp_fs_exists(const char *path_utf8);
bool tp_fs_is_dir(const char *path_utf8);
/* Creates the directory, ADOPTING an existing one (an already-present directory
 * is success). Use tp_fs_create_dir_exclusive when adoption is not allowed. */
bool tp_fs_create_dir(const char *path_utf8);

typedef enum tp_fs_create_dir_result {
    TP_FS_CREATE_DIR_OK = 0,
    TP_FS_CREATE_DIR_EXISTS, /* the name is already taken, by anything */
    TP_FS_CREATE_DIR_ERROR
} tp_fs_create_dir_result;

/* Exclusive create: only a directory this call brought into existence reports
 * OK. Any pre-existing name -- directory, file, dangling link -- is EXISTS and
 * is never adopted, so a caller that must own what it creates (the export
 * staging dir) can retry under a different name instead of writing into a
 * stranger's directory. Backed by the one atomic primitive each host has
 * (CreateDirectoryW / mkdir), so it is race-free, not check-then-create. */
tp_fs_create_dir_result tp_fs_create_dir_exclusive(const char *path_utf8);
bool tp_fs_remove_file(const char *path_utf8);
bool tp_fs_remove_dir(const char *path_utf8); /* removes an empty directory */

bool tp_fs_replace(const char *source_utf8, const char *destination_utf8);
typedef enum tp_fs_move_result {
    TP_FS_MOVE_OK = 0,
    TP_FS_MOVE_DESTINATION_EXISTS,
    TP_FS_MOVE_ERROR
} tp_fs_move_result;

tp_fs_move_result tp_fs_move_no_replace(const char *source_utf8, const char *destination_utf8);
bool tp_fs_sync_parent(const char *path_utf8);

#define TP_FS_NAME_MAX 1024

typedef struct tp_fs_dir tp_fs_dir;

typedef struct tp_fs_dir_entry {
    char name[TP_FS_NAME_MAX];
    tp_fs_info info;
} tp_fs_dir_entry;

typedef enum tp_fs_dir_result {
    TP_FS_DIR_ENTRY = 0,
    TP_FS_DIR_END,
    TP_FS_DIR_ERROR
} tp_fs_dir_result;

tp_fs_dir *tp_fs_dir_open(const char *path_utf8);
tp_fs_dir_result tp_fs_dir_next(tp_fs_dir *dir, tp_fs_dir_entry *out);
void tp_fs_dir_close(tp_fs_dir *dir);

#ifdef _WIN32
#include <wchar.h>

/* Specialized Windows code (identity handles/locks) may use these conversion
 * helpers, but must still issue only W-suffixed filesystem calls. */
bool tp_fs_win32_utf8_to_utf16(const char *utf8, wchar_t *out, size_t cap);
bool tp_fs_win32_utf16_to_utf8(const wchar_t *wide, char *out, size_t cap);
wchar_t *tp_fs_win32_path_alloc(const char *path_utf8);
#endif

#endif /* TP_FS_INTERNAL_H */
