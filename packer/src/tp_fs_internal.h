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

bool tp_fs_stat(const char *path_utf8, tp_fs_info *out);
bool tp_fs_exists(const char *path_utf8);
bool tp_fs_is_dir(const char *path_utf8);
bool tp_fs_create_dir(const char *path_utf8);
bool tp_fs_remove_file(const char *path_utf8);

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
