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

tp_status tp_export_write_pages(const tp_result *result, const char *out_path_base, bool premultiply, tp_error *err) {
    if (!result || !out_path_base) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_export_write_pages: NULL result/out_path_base");
    }
    /* Validate the complete output set before encoding or writing page zero. */
    for (int p = 0; p < result->page_count; p++) {
        char path[TP_IDENTITY_PATH_MAX];
        tp_status st = tp_export_page_path(out_path_base, p, path, err);
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
        tp_status st = tp_export_page_path(out_path_base, p, path, err);
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
        bool wrote = tp_fs_write_file_atomic(path, png, (size_t)png_size);
        free(png);
        if (!wrote) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_export_write_pages: failed writing '%s'", path);
        }
    }
    return TP_STATUS_OK;
}

/* Every preflight verdict is reached before the first rename, so the STAGED set
 * is always intact when one fires. What is not always intact is the rest of the
 * output set: an output that could not be mapped into staging was published
 * per-file by the writer (and said so through its own notice), so the usual
 * "existing outputs are untouched" would be a lie once `bypassed` is non-zero.
 * `buf` backs the bypassed wording; the zero-bypass wording is a literal, so
 * that message stays exactly what it always was. */
static const char *publish_intact_clause(int bypassed, char *buf, size_t cap) {
    if (bypassed <= 0) {
        return "existing outputs are untouched";
    }
    (void)snprintf(buf, cap,
                   "%d bypassed output%s already published individually; no "
                   "staged output was promoted",
                   bypassed, bypassed == 1 ? " was" : "s were");
    return buf;
}

/* Contract in tp_core/tp_export.h. Lives beside the shared page writer because
 * this TU already owns the exporters' tp_fs boundary (R18): the run layer must
 * stay out of tp_fs internals. */
tp_status tp_export_write_and_publish_set(const tp_exporter *exp,
                                          const tp_export_prepared *prep,
                                          const char *out_path_base,
                                          const char *const *output_files,
                                          int output_file_count,
                                          tp_export_notices *notices,
                                          tp_error *err) {
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
        if (out_path_base[i] == '/' || out_path_base[i] == '\\') {
            cut = i + 1;
        }
    }
    if (cut >= sizeof out_dir) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "export output directory exceeds the canonical path limit");
    }
    memcpy(out_dir, out_path_base, cut);
    out_dir[cut] = '\0';

    char staging[TP_FS_ATOMIC_TEMP_PATH_MAX];
    if (!tp_fs_stage_dir_create(cut > 0 ? out_dir : "./", staging,
                                sizeof staging)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "cannot create the export staging directory under "
                            "'%s' (existing outputs are untouched)",
                            cut > 0 ? out_dir : ".");
    }

    tp_fs_stage_redirect_begin(out_dir, staging);
    tp_status st = exp->write(prep, &exp->caps, out_path_base, notices, err);
    tp_fs_stage_redirect_end();
    if (st != TP_STATUS_OK) {
        tp_fs_remove_tree(staging);
        return st;
    }

    /* Preflight the complete set before the first irreversible replace: every
     * staged file must exist (the enumeration and the writer agree) and no
     * destination may be a directory (the one replace failure a caller can
     * cause that is detectable up front). */
    char intact[192];
    int bypassed = 0;
    for (int f = 0; f < output_file_count; f++) {
        char staged[TP_FS_ATOMIC_TEMP_PATH_MAX];
        if (!tp_fs_stage_child_path(out_dir, staging, output_files[f], staged,
                                    sizeof staged)) {
            /* Not stageable: the writer published this one per-file, so it sits
             * outside the whole-set guarantee. Say so instead of degrading
             * silently (tp_core/tp_export.h contract). It also means a LATER
             * preflight failure may no longer claim the outputs are untouched,
             * so the count travels to every message below. */
            (void)tp_export_notice_add_ex(
                notices, TP_NOTICE_FIELD_SET_ATOMICITY,
                TP_NOTICE_REASON_PATH_NOT_STAGEABLE, NULL, exp->id,
                "'%s' bypassed whole-set staging and was published on its own; "
                "it is not covered by the export's all-or-nothing guarantee",
                output_files[f]);
            bypassed++;
            continue;
        }
        if (!tp_fs_exists(staged)) {
            tp_fs_remove_tree(staging);
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "exporter '%s' listed '%s' but did not produce "
                                "it (%s)",
                                exp->id, output_files[f],
                                publish_intact_clause(bypassed, intact,
                                                      sizeof intact));
        }
        if (tp_fs_is_dir(output_files[f])) {
            tp_fs_remove_tree(staging);
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "export destination '%s' is a directory (%s)",
                                output_files[f],
                                publish_intact_clause(bypassed, intact,
                                                      sizeof intact));
        }
    }

    /* A staged file the enumeration missed is an output that would silently
     * vanish with the staging dir. It is part of preflight, not cleanup: found
     * after the promote loop it would report "not published" over an already
     * republished set, so the whole staging dir is matched against the listed
     * set BEFORE the first rename. */
    bool leftover = false;
    bool unreadable = false;
    char leftover_name[TP_FS_NAME_MAX];
    leftover_name[0] = '\0';
    tp_fs_dir *dir = tp_fs_dir_open(staging);
    if (!dir) {
        unreadable = true;
    } else {
        tp_fs_dir_entry entry;
        tp_fs_dir_result step = TP_FS_DIR_END;
        while (!leftover &&
               (step = tp_fs_dir_next(dir, &entry)) == TP_FS_DIR_ENTRY) {
            char entry_path[TP_FS_ATOMIC_TEMP_PATH_MAX];
            const int n = snprintf(entry_path, sizeof entry_path, "%s/%s",
                                   staging, entry.name);
            bool listed = false;
            if (n > 0 && (size_t)n < sizeof entry_path) {
                for (int f = 0; f < output_file_count && !listed; f++) {
                    char staged[TP_FS_ATOMIC_TEMP_PATH_MAX];
                    listed = tp_fs_stage_child_path(out_dir, staging,
                                                    output_files[f], staged,
                                                    sizeof staged) &&
                             strcmp(staged, entry_path) == 0;
                }
            }
            if (!listed) {
                leftover = true;
                (void)snprintf(leftover_name, sizeof leftover_name, "%s",
                               entry.name);
            }
        }
        /* TP_FS_DIR_END is the only clean way out; TP_FS_DIR_ERROR means the
         * scan stopped early and the remaining entries were never compared. */
        unreadable = step == TP_FS_DIR_ERROR;
        tp_fs_dir_close(dir);
    }
    /* Nothing has been renamed yet, so an unverifiable staging dir fails CLOSED:
     * promoting here would publish a set no one checked for the very leftover
     * this scan exists to catch. */
    if (unreadable) {
        tp_fs_remove_tree(staging);
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "the export staging directory could not be read, so "
                            "the set exporter '%s' produced could not be "
                            "verified against its declared output list; nothing "
                            "was promoted (%s)",
                            exp->id,
                            publish_intact_clause(bypassed, intact,
                                                  sizeof intact));
    }
    if (leftover) {
        tp_fs_remove_tree(staging);
        if (bypassed > 0) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "exporter '%s' produced '%s' outside its "
                                "declared output list; %s",
                                exp->id, leftover_name,
                                publish_intact_clause(bypassed, intact,
                                                      sizeof intact));
        }
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "exporter '%s' produced '%s' outside its declared "
                            "output list; nothing was published (existing "
                            "outputs are untouched)",
                            exp->id, leftover_name);
    }

    int promoted = 0;
    for (int f = 0; f < output_file_count; f++) {
        char staged[TP_FS_ATOMIC_TEMP_PATH_MAX];
        if (!tp_fs_stage_child_path(out_dir, staging, output_files[f], staged,
                                    sizeof staged)) {
            continue;
        }
        if (!tp_fs_replace(staged, output_files[f])) {
            tp_fs_remove_tree(staging);
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "export publish failed replacing '%s' (%d of "
                                "%d files promoted); the output set may mix "
                                "old and new files",
                                output_files[f], promoted, output_file_count);
        }
        promoted++;
    }
    tp_fs_remove_tree(staging);
    return TP_STATUS_OK;
}
