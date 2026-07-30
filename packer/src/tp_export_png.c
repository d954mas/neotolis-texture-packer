#include "tp_core/tp_export.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_fs_internal.h"

/* Vendored builder-only writer, linked directly into tp_core (NOT via
 * nt_builder -- no basisu, no static-CRT pin leaks; #282). */
#include "stb_image_write.h"

/* stb exposes this implementation symbol but omits it from the public
 * declaration block. The vendored implementation uses the default malloc/free
 * hooks (stb_image_write_impl.c). */
extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels, int stride_bytes, int x, int y, int n,
                                            int *out_len);

/* Premultiplies straight-alpha RGBA8 into `dst` (w*h*4). Rounds to nearest. */
static void premultiply_rgba(const uint8_t *src, uint8_t *dst, int w, int h) {
    size_t px = (size_t)w * (size_t)h;
    for (size_t i = 0; i < px; i++) {
        uint32_t a = src[i * 4 + 3];
        dst[i * 4 + 0] = (uint8_t)((src[i * 4 + 0] * a + 127U) / 255U);
        dst[i * 4 + 1] = (uint8_t)((src[i * 4 + 1] * a + 127U) / 255U);
        dst[i * 4 + 2] = (uint8_t)((src[i * 4 + 2] * a + 127U) / 255U);
        dst[i * 4 + 3] = (uint8_t)a;
    }
}

tp_status tp_export_write_pages(const tp_result *result, const char *write_path_base, bool premultiply, tp_error *err) {
    if (!result || !write_path_base) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_export_write_pages: NULL result/write_path_base");
    }
    /* Validate the complete output set before encoding or writing page zero. */
    for (int p = 0; p < result->page_count; p++) {
        char path[TP_IDENTITY_PATH_MAX];
        tp_status st = tp_export_page_path(write_path_base, p, path, err);
        if (st != TP_STATUS_OK) {
            return st;
        }
    }
    for (int p = 0; p < result->page_count; p++) {
        const tp_page *pg = &result->pages[p];
        if (!pg->rgba || pg->w <= 0 || pg->h <= 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_export_write_pages: page %d has no pixels", p);
        }
        if (pg->w > INT_MAX / 4 || (size_t)pg->w > SIZE_MAX / (size_t)pg->h ||
            (size_t)pg->w * (size_t)pg->h > SIZE_MAX / 4U) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_export_write_pages: page %d dimensions overflow", p);
        }
        char path[TP_IDENTITY_PATH_MAX];
        tp_status st = tp_export_page_path(write_path_base, p, path, err);
        if (st != TP_STATUS_OK) {
            return st;
        }

        const uint8_t *pixels = pg->rgba;
        uint8_t *tmp = NULL;
        if (premultiply) {
            tmp = (uint8_t *)malloc((size_t)pg->w * (size_t)pg->h * 4U);
            if (!tmp) {
                return tp_error_set(err, TP_STATUS_OOM, "tp_export_write_pages: OOM premultiplying page %d", p);
            }
            premultiply_rgba(pg->rgba, tmp, pg->w, pg->h);
            pixels = tmp;
        }
        int png_size = 0;
        unsigned char *png = stbi_write_png_to_mem(pixels, pg->w * 4, pg->w, pg->h, 4, &png_size);
        free(tmp);
        if (!png || png_size <= 0) {
            free(png);
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_export_write_pages: failed encoding page %d", p);
        }
        bool wrote = tp_fs_write_file(path, png, (size_t)png_size);
        free(png);
        if (!wrote) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_export_write_pages: failed writing '%s'", path);
        }
    }
    return TP_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Output-SET publication (contract in tp_core/tp_export.h).            */
/* ------------------------------------------------------------------ */
/*
 * This lives beside the shared page writer because this TU already owns the
 * exporters' tp_fs boundary (R18): the run layer must stay out of tp_fs
 * internals.
 */

#ifdef TP_ENABLE_TEST_SEAMS
static _Thread_local int s_fail_rename_at = -1;
static _Thread_local int s_rename_ordinal;

void tp_export_publish__test_fail_rename_at(int nth) {
    s_fail_rename_at = nth;
    s_rename_ordinal = 0;
}

static bool publish_rename_allowed(void) {
    if (s_fail_rename_at < 0) {
        return true;
    }
    return s_rename_ordinal++ != s_fail_rename_at;
}
#else
#define publish_rename_allowed() true
#endif

/* One listed output, resolved once so no later phase re-derives a path. */
typedef struct publish_entry {
    const char *destination; /* borrowed from the caller's list                  */
    const char *leaf;        /* points into `destination`                        */
    char staged[TP_FS_STAGE_PATH_MAX];
    char displaced[TP_FS_STAGE_PATH_MAX];
    bool has_displaced; /* the destination existed and was renamed aside         */
    bool promoted;      /* the staged file was renamed onto the destination      */
} publish_entry;

static bool publish_is_sep(char c) {
    return c == '/' || c == '\\';
}

/* ASCII case fold. Paths here are already validated UTF-8; a non-ASCII case
 * collision (Turkish dotted I, full-width forms) is deliberately out of scope --
 * the contract in the header says so. */
static char publish_fold(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool publish_leaf_collides(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (publish_fold(*a) != publish_fold(*b)) {
            return false;
        }
    }
    return *a == *b;
}

/* The name `path` contributes to `out_dir` ("" or trailing-separator form), or
 * NULL when it is not a direct child of it. "." and ".." are not files. */
static const char *publish_direct_child_leaf(const char *out_dir, size_t dir_len,
                                             const char *path) {
    if (strncmp(path, out_dir, dir_len) != 0) {
        return NULL;
    }
    const char *leaf = path + dir_len;
    if (leaf[0] == '\0' || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
        return NULL;
    }
    for (const char *c = leaf; *c != '\0'; c++) {
        if (publish_is_sep(*c)) {
            return NULL;
        }
    }
    return leaf;
}

/* Undoes as much of the two-phase swap as ran. Returns the number of steps that
 * could NOT be undone: zero means the previous set is exactly back, and anything
 * else must reach the caller instead of being reported as a clean failure. */
static int publish_rollback(publish_entry *entries, int count) {
    int unrestored = 0;
    for (int f = 0; f < count; f++) {
        publish_entry *e = &entries[f];
        if (e->has_displaced) {
            /* Overwrites the newly promoted file when there was one. */
            if (!tp_fs_replace(e->displaced, e->destination)) {
                unrestored++;
                continue;
            }
            e->has_displaced = false;
            e->promoted = false;
        } else if (e->promoted) {
            /* Nothing was there before, so "restore" means remove. */
            if (!tp_fs_remove_file(e->destination)) {
                unrestored++;
                continue;
            }
            e->promoted = false;
        }
    }
    return unrestored;
}

static const char *publish_rollback_clause(int unrestored, char *buf, size_t cap) {
    if (unrestored <= 0) {
        return "the previous outputs were restored";
    }
    (void)snprintf(buf, cap,
                   "%d file%s could NOT be restored, so the output directory is "
                   "left mixed",
                   unrestored, unrestored == 1 ? "" : "s");
    return buf;
}

/* The complete listed set is verified BEFORE the first irreversible rename:
 * every staged file present, no destination an existing directory, and no staged
 * file the list missed. */
static tp_status publish_verify_staged_set(const tp_exporter *exp,
                                           const publish_entry *entries,
                                           int count, const char *staging,
                                           tp_error *err) {
    for (int f = 0; f < count; f++) {
        if (!tp_fs_exists(entries[f].staged)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "exporter '%s' listed an output it did not "
                                "produce (existing outputs are untouched): '%s'",
                                exp->id, entries[f].destination);
        }
        if (tp_fs_is_dir(entries[f].destination)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "export destination is a directory (existing "
                                "outputs are untouched): '%s'",
                                entries[f].destination);
        }
    }

    /* A staged file the enumeration missed is an output that would silently
     * vanish with the staging dir, so it is a verdict, not a cleanup discovery.
     * An unreadable staging dir fails CLOSED: an unverified set is exactly the
     * set this scan exists to reject. */
    tp_fs_dir *dir = tp_fs_dir_open(staging);
    if (!dir) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "exporter '%s': the export staging directory could "
                            "not be read, so its output set could not be "
                            "verified; nothing was published (existing outputs "
                            "are untouched)",
                            exp->id);
    }
    char leftover[TP_FS_NAME_MAX];
    leftover[0] = '\0';
    tp_fs_dir_entry entry;
    tp_fs_dir_result step = TP_FS_DIR_END;
    while (leftover[0] == '\0' &&
           (step = tp_fs_dir_next(dir, &entry)) == TP_FS_DIR_ENTRY) {
        bool listed = false;
        for (int f = 0; f < count && !listed; f++) {
            listed = strcmp(entries[f].leaf, entry.name) == 0;
        }
        if (!listed) {
            (void)snprintf(leftover, sizeof leftover, "%s", entry.name);
        }
    }
    /* TP_FS_DIR_END is the only clean way out; TP_FS_DIR_ERROR means the scan
     * stopped early and the remaining entries were never compared. */
    const bool unreadable = step == TP_FS_DIR_ERROR;
    tp_fs_dir_close(dir);
    if (unreadable) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "exporter '%s': the export staging directory could "
                            "not be enumerated to its end, so its output set "
                            "could not be verified; nothing was published "
                            "(existing outputs are untouched)",
                            exp->id);
    }
    if (leftover[0] != '\0') {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "exporter '%s' produced '%s' outside its declared "
                            "output list; nothing was published (existing "
                            "outputs are untouched)",
                            exp->id, leftover);
    }
    return TP_STATUS_OK;
}

/* Phase one displaces every existing destination, phase two moves the staged set
 * in. Either phase failing rolls the whole thing back. */
static tp_status publish_swap(publish_entry *entries, int count, tp_error *err) {
    char clause[160];
    for (int f = 0; f < count; f++) {
        publish_entry *e = &entries[f];
        if (!tp_fs_exists(e->destination)) {
            continue; /* nothing to preserve; rollback removes what we add */
        }
        if (!tp_fs_stage_old_path(e->destination, e->displaced,
                                  sizeof e->displaced)) {
            const int unrestored = publish_rollback(entries, count);
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "export publish cannot name a rollback copy of "
                                "'%s' within the path limit; %s",
                                e->destination,
                                publish_rollback_clause(unrestored, clause,
                                                        sizeof clause));
        }
        if (!publish_rename_allowed() ||
            tp_fs_move_no_replace(e->destination, e->displaced) !=
                TP_FS_MOVE_OK) {
            const int unrestored = publish_rollback(entries, count);
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "export publish could not move the previous "
                                "'%s' aside; nothing was published and %s",
                                e->destination,
                                publish_rollback_clause(unrestored, clause,
                                                        sizeof clause));
        }
        e->has_displaced = true;
    }
    for (int f = 0; f < count; f++) {
        publish_entry *e = &entries[f];
        if (!publish_rename_allowed() ||
            !tp_fs_replace(e->staged, e->destination)) {
            const int unrestored = publish_rollback(entries, count);
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "export publish failed placing '%s'; the whole "
                                "set was rolled back and %s",
                                e->destination,
                                publish_rollback_clause(unrestored, clause,
                                                        sizeof clause));
        }
        e->promoted = true;
    }
    /* The set is published; the displaced copies are now spent. */
    for (int f = 0; f < count; f++) {
        if (entries[f].has_displaced) {
            (void)tp_fs_remove_file(entries[f].displaced);
        }
    }
    return TP_STATUS_OK;
}

tp_status tp_export_write_and_publish_set(const tp_exporter *exp,
                                          const tp_export_prepared *prep,
                                          const char *out_path_base,
                                          const char *const *output_files,
                                          int output_file_count,
                                          tp_export_notices *notices,
                                          bool *out_writer_ran,
                                          tp_error *err) {
    if (out_writer_ran) {
        *out_writer_ran = false;
    }
    if (!exp || !exp->write || !prep || !out_path_base ||
        (!output_files && output_file_count > 0) || output_file_count < 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "set publication requires exporter, prep, base, "
                            "and the enumerated output list");
    }

    /* The whole set lands in out_path_base's own directory: every output is
     * "<base><suffix>". "" (no separator) means cwd-relative outputs. */
    char out_dir[TP_IDENTITY_PATH_MAX];
    size_t cut = 0;
    for (size_t i = 0; out_path_base[i]; i++) {
        if (publish_is_sep(out_path_base[i])) {
            cut = i + 1;
        }
    }
    if (cut >= sizeof out_dir) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "export output directory exceeds the canonical path limit");
    }
    memcpy(out_dir, out_path_base, cut);
    out_dir[cut] = '\0';

    publish_entry *entries = NULL;
    if (output_file_count > 0) {
        entries = (publish_entry *)calloc((size_t)output_file_count,
                                          sizeof *entries);
        if (!entries) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "export publish: OOM resolving the output set");
        }
    }

    /* PREFLIGHT, before the writer runs and before anything is created: the
     * declared list must describe a set this publication can actually deliver. */
    for (int f = 0; f < output_file_count; f++) {
        entries[f].destination = output_files[f];
        entries[f].leaf = publish_direct_child_leaf(out_dir, cut, output_files[f]);
        if (!entries[f].leaf) {
            const tp_status st = tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "exporter '%s' declared the output '%s', which is not a direct "
                "child of the export directory '%s'; the whole-set publication "
                "cannot cover it, so nothing was written",
                exp->id, output_files[f], cut > 0 ? out_dir : ".");
            free(entries);
            return st;
        }
    }
    for (int f = 0; f < output_file_count; f++) {
        for (int g = f + 1; g < output_file_count; g++) {
            if (!publish_leaf_collides(entries[f].leaf, entries[g].leaf)) {
                continue;
            }
            const tp_status st = tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "exporter '%s' declared outputs %d and %d as the same file "
                "('%s' and '%s' differ only by ASCII case, which Windows and "
                "macOS resolve to one file); nothing was written",
                exp->id, f, g, entries[f].leaf, entries[g].leaf);
            free(entries);
            return st;
        }
    }

    /* A process killed mid-export leaves its private names behind forever.
     * Reclaim the ones whose owner is definitively gone before adding our own;
     * that also completes or undoes a swap that crashed halfway. */
    tp_fs_stage_reap_orphans(out_dir);

    char staging[TP_FS_STAGE_PATH_MAX];
    if (!tp_fs_stage_dir_create(out_dir, staging, sizeof staging)) {
        const tp_status st = tp_error_set(
            err, TP_STATUS_BAD_PROJECT,
            "cannot create the export staging directory under '%s' (existing "
            "outputs are untouched)",
            cut > 0 ? out_dir : ".");
        free(entries);
        return st;
    }

    /* Same basename inside staging, so "<base>.<ext>" naming is unchanged and
     * every listed output maps to exactly one staged file. The staged forms are
     * bounded by the CANONICAL path limit, not by this module's buffers: a
     * writer composes its outputs with tp_export_output_path, which cannot
     * express anything longer, so a base too close to the limit has to fail here
     * -- with a message about the output set -- rather than deep inside a writer. */
    char stage_base[TP_FS_STAGE_PATH_MAX];
    const int sn = snprintf(stage_base, sizeof stage_base, "%s/%s", staging,
                            out_path_base + cut);
    bool mapped = sn > 0 && (size_t)sn < (size_t)TP_IDENTITY_PATH_MAX;
    for (int f = 0; mapped && f < output_file_count; f++) {
        mapped = tp_fs_stage_child_path(out_dir, staging, entries[f].destination,
                                        entries[f].staged, TP_IDENTITY_PATH_MAX);
    }
    if (!mapped) {
        tp_fs_remove_tree(staging);
        const tp_status st = tp_error_set(
            err, TP_STATUS_OUT_OF_BOUNDS,
            "exporter '%s': the staged form of its output set exceeds the "
            "canonical path limit, so nothing was written",
            exp->id);
        free(entries);
        return st;
    }

    const tp_export_write_ctx ctx = {
        .prep = prep,
        .caps = &exp->caps,
        .write_path_base = stage_base,
        .out_path_base = out_path_base,
        .notices = notices,
    };
    if (out_writer_ran) {
        *out_writer_ran = true;
    }
    tp_status st = exp->write(&ctx, err);
    if (st == TP_STATUS_OK) {
        st = publish_verify_staged_set(exp, entries, output_file_count, staging,
                                       err);
    }
    if (st == TP_STATUS_OK) {
        st = publish_swap(entries, output_file_count, err);
    }
    tp_fs_remove_tree(staging);
    free(entries);
    return st;
}
