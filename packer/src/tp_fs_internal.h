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
/* Same bytes, but the destination is never observed half-written: the content goes
 * to a sibling temp and is published with one tp_fs_replace. A failure at any step
 * removes the temp and leaves an existing destination byte-for-byte intact. Not
 * durability (no fsync) -- exports are reproducible, so the property that matters
 * is that a failed write cannot destroy the previous export. */
#define TP_FS_ATOMIC_TEMP_PATH_MAX 4160 /* TP_IDENTITY_PATH_MAX + ".tp-tmp-<pid>" */
bool tp_fs_write_file_atomic(const char *path_utf8, const void *data,
                             size_t size);

/* Staged output-SET publish (export set-atomicity). A staging dir is a private
 * ".tp-stage-<pid>-<serial>" child of `parent_utf8` -- the same directory (and
 * therefore volume) as the files it will replace, so promotion stays a pure
 * rename. `parent_utf8` must be "" or end with a path separator; "" means the
 * current directory. While a redirect is armed on the calling thread,
 * tp_fs_write_file_atomic calls whose destination is a DIRECT child of
 * `final_dir_utf8` write a plain file of the same name into `staging_dir_utf8`
 * instead (per-file temp+replace inside a private dir buys nothing); every
 * other path keeps the sibling-temp behavior. The publish pass is the caller's:
 * one tp_fs_replace per staged file, and the staging tree must be removed on
 * every exit path. Redirects do not nest. */
bool tp_fs_stage_dir_create(const char *parent_utf8, char *out, size_t out_cap);
void tp_fs_stage_redirect_begin(const char *final_dir_utf8,
                                const char *staging_dir_utf8);
void tp_fs_stage_redirect_end(void);
/* Maps a DIRECT child of `final_dir_utf8` ("" or trailing-separator form, as
 * above) to its staged sibling under `staging_dir_utf8`. False when `path_utf8`
 * is not a direct child or the result would overflow `out_cap`. The redirect
 * and the caller's publish pass share this one mapping rule. */
bool tp_fs_stage_child_path(const char *final_dir_utf8,
                            const char *staging_dir_utf8,
                            const char *path_utf8, char *out, size_t out_cap);
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
