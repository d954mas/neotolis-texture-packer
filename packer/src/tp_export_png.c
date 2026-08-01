#include "tp_core/tp_export.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_file_lease.h"
#include "tp_export_internal.h"
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

tp_status tp_export_write_page_artifact(const tp_page *pg, int page_id,
                                        const char *path, bool premultiply,
                                        tp_error *err) {
    if (!pg || !path || page_id < 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "page artifact requires page, logical id, and path");
    }
    if (!pg->rgba || pg->w <= 0 || pg->h <= 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "page artifact %d has no pixels", page_id);
    }
    if (pg->w > INT_MAX / 4 || (size_t)pg->w > SIZE_MAX / (size_t)pg->h ||
        (size_t)pg->w * (size_t)pg->h > SIZE_MAX / 4U) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "page artifact %d dimensions overflow", page_id);
    }
    const uint8_t *pixels = pg->rgba;
    uint8_t *tmp = NULL;
    if (premultiply) {
        tmp = (uint8_t *)malloc((size_t)pg->w * (size_t)pg->h * 4U);
        if (!tmp) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "page artifact: OOM premultiplying page %d", page_id);
        }
        premultiply_rgba(pg->rgba, tmp, pg->w, pg->h);
        pixels = tmp;
    }
    int png_size = 0;
    unsigned char *png = stbi_write_png_to_mem(pixels, pg->w * 4, pg->w,
                                                pg->h, 4, &png_size);
    free(tmp);
    if (!png || png_size <= 0) {
        free(png);
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "page artifact: failed encoding page %d", page_id);
    }
    const bool wrote = tp_fs_write_file(path, png, (size_t)png_size);
    free(png);
    if (!wrote) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "page artifact: failed writing '%s'", path);
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
    const char *destination; /* borrowed from the artifact plan                  */
    const char *leaf;        /* points into `destination`                        */
    char staged[TP_FS_STAGE_PATH_MAX];
    char displaced[TP_FS_STAGE_PATH_MAX];
    bool has_displaced; /* the destination existed and was renamed aside         */
    bool promoted;      /* the staged file was renamed onto the destination      */
} publish_entry;

#define TP_EXPORT_LEASE_SUFFIX ".ntpacker-export.lock"

typedef struct publish_lease {
    char path[TP_FS_STAGE_PATH_MAX];
    const char *destination;
    tp_file_lease *file;
} publish_lease;

static int publish_lease_compare(const void *left, const void *right) {
    const publish_lease *a = (const publish_lease *)left;
    const publish_lease *b = (const publish_lease *)right;
    return strcmp(a->path, b->path);
}

static bool publish_has_suffix(const char *value, const char *suffix) {
    const size_t value_len = strlen(value);
    const size_t suffix_len = strlen(suffix);
    if (value_len < suffix_len) {
        return false;
    }
    const char *tail = value + value_len - suffix_len;
    for (size_t i = 0; i < suffix_len; ++i) {
        const char left = tail[i] >= 'A' && tail[i] <= 'Z'
                              ? (char)(tail[i] - 'A' + 'a')
                              : tail[i];
        const char right = suffix[i] >= 'A' && suffix[i] <= 'Z'
                               ? (char)(suffix[i] - 'A' + 'a')
                               : suffix[i];
        if (left != right) {
            return false;
        }
    }
    return true;
}

static void publish_leases_release(publish_lease *leases, int count) {
    if (!leases) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        tp_file_lease_release(leases[i].file);
    }
    free(leases);
}

/* Own every destination slot before the serializer or staging writer runs.
 * Sorting gives overlapping sets one stable acquisition order. The permanent
 * sidecar is only a rendezvous name; the live OS handle is the ownership. */
static tp_status publish_leases_acquire(const publish_entry *entries, int count,
                                        publish_lease **out, tp_error *err) {
    *out = NULL;
    publish_lease *leases =
        (publish_lease *)calloc((size_t)count, sizeof *leases);
    if (!leases) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "export publish: OOM allocating destination leases");
    }
    for (int i = 0; i < count; ++i) {
        if (publish_has_suffix(entries[i].destination,
                               TP_EXPORT_LEASE_SUFFIX)) {
            free(leases);
            return tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "export output '%s' uses the reserved publication-lease suffix",
                entries[i].destination);
        }
        const int length = snprintf(leases[i].path, sizeof leases[i].path,
                                    "%s%s", entries[i].destination,
                                    TP_EXPORT_LEASE_SUFFIX);
        if (length < 0 || (size_t)length >= sizeof leases[i].path) {
            free(leases);
            return tp_error_set(
                err, TP_STATUS_OUT_OF_BOUNDS,
                "export publication lease path exceeds the canonical path limit for '%s'",
                entries[i].destination);
        }
        leases[i].destination = entries[i].destination;
    }
    qsort(leases, (size_t)count, sizeof *leases, publish_lease_compare);
    for (int i = 0; i < count; ++i) {
        const tp_status status = tp_file_lease_acquire(
            leases[i].path, TP_STATUS_EXPORT_BUSY, "export destination",
            leases[i].destination, &leases[i].file, err);
        if (status != TP_STATUS_OK) {
            publish_leases_release(leases, count);
            return status;
        }
    }
    *out = leases;
    return TP_STATUS_OK;
}

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
 * every file the common publisher wrote is present and no destination is an
 * existing directory. Serializers are memory-only and cannot create unlisted
 * staging files. */
static tp_status publish_verify_staged_set(const tp_exporter *exp,
                                           const publish_entry *entries,
                                           int count, tp_error *err) {
    for (int f = 0; f < count; f++) {
        if (!tp_fs_exists(entries[f].staged)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "exporter '%s' listed an output it did not "
                                "produce (existing outputs are untouched): '%s'",
                                exp->format->id, entries[f].destination);
        }
        if (tp_fs_is_dir(entries[f].destination)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "export destination is a directory (existing "
                                "outputs are untouched): '%s'",
                                entries[f].destination);
        }
    }

    return TP_STATUS_OK;
}

/* Phase one displaces every existing destination, phase two moves the staged set
 * in. Either phase failing rolls the whole thing back. */
static tp_status publish_swap(publish_entry *entries, int count,
                              bool *out_publication_uncertain,
                              tp_error *err) {
    char clause[160];
    for (int f = 0; f < count; f++) {
        publish_entry *e = &entries[f];
        if (!tp_fs_exists(e->destination)) {
            continue; /* nothing to preserve; rollback removes what we add */
        }
        if (!tp_fs_stage_old_path(e->destination, e->displaced,
                                  sizeof e->displaced)) {
            const int unrestored = publish_rollback(entries, count);
            *out_publication_uncertain = unrestored > 0;
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
            *out_publication_uncertain = unrestored > 0;
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
            *out_publication_uncertain = unrestored > 0;
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

tp_status tp_export_publish(const tp_exporter *exp,
                            const tp_export_ir *ir,
                            const tp_result *packed,
                            const tp_export_artifact_plan *plan,
                            tp_export_notices *notices,
                            bool *out_serializer_ran,
                            bool *out_publication_uncertain,
                            tp_error *err) {
    if (out_serializer_ran) {
        *out_serializer_ran = false;
    }
    bool ignored_publication_uncertain = false;
    if (!out_publication_uncertain) {
        out_publication_uncertain = &ignored_publication_uncertain;
    }
    *out_publication_uncertain = false;
    if (!exp || !exp->format || !exp->format->id || !exp->serialize || !ir ||
        !packed || !plan) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "export publish requires a matching exporter, IR, packed pages, and artifact plan");
    }
    tp_status validation = tp_export_format_admit(exp->format, ir, err);
    if (validation != TP_STATUS_OK) {
        return validation;
    }
    if (!plan->format_id || strcmp(plan->format_id, exp->format->id) != 0 ||
        !plan->out_path_base || !plan->artifacts ||
        plan->document_count < 0 ||
        plan->artifact_count < plan->document_count ||
        !exp->format->artifacts || exp->format->artifact_count <= 0 ||
        plan->document_count != exp->format->artifact_count ||
        plan->artifact_count - plan->document_count != ir->page_count ||
        packed->page_count != ir->page_count || !packed->pages) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "export publish requires a matching exporter, IR, packed pages, and artifact plan");
    }
    for (int f = 0; f < plan->artifact_count; ++f) {
        if (!plan->artifacts[f].id || !plan->artifacts[f].path) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "export artifact plan entry %d is incomplete",
                                f);
        }
    }
    const char *out_path_base = plan->out_path_base;
    const int output_file_count = plan->artifact_count;

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
        entries[f].destination = plan->artifacts[f].path;
        entries[f].leaf = publish_direct_child_leaf(out_dir, cut,
                                                     plan->artifacts[f].path);
        if (!entries[f].leaf) {
            const tp_status st = tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "exporter '%s' declared the output '%s', which is not a direct "
                "child of the export directory '%s'; the whole-set publication "
                "cannot cover it, so nothing was written",
                exp->format->id, plan->artifacts[f].path,
                cut > 0 ? out_dir : ".");
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
                exp->format->id, f, g, entries[f].leaf, entries[g].leaf);
            free(entries);
            return st;
        }
    }

    /* The plan is typed and ordered: documents match descriptor declarations;
     * pages match IR logical ids; every concrete path is derived from the one
     * output base. This is checked after generic path/collision diagnostics but
     * before any filesystem mutation or serializer call. */
    for (int d = 0; d < plan->document_count; ++d) {
        const tp_export_artifact *artifact = &plan->artifacts[d];
        const tp_format_artifact_decl *decl = &exp->format->artifacts[d];
        char expected[TP_IDENTITY_PATH_MAX];
        if (artifact->kind != TP_EXPORT_ARTIFACT_DOCUMENT ||
            artifact->logical_id != d || !artifact->id || !decl->id ||
            strcmp(artifact->id, decl->id) != 0 || !decl->suffix ||
            tp_export_output_path(out_path_base, decl->suffix, expected, err) !=
                TP_STATUS_OK ||
            strcmp(artifact->path, expected) != 0) {
            free(entries);
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "export document plan entry %d does not match format '%s'",
                                d, exp->format->id);
        }
    }
    for (int p = 0; p < ir->page_count; ++p) {
        const int index = plan->document_count + p;
        const tp_export_artifact *artifact = &plan->artifacts[index];
        char expected_path[TP_IDENTITY_PATH_MAX];
        char expected_id[32];
        const int id_len = snprintf(expected_id, sizeof expected_id, "page-%d",
                                    ir->pages[p].artifact_id);
        if (artifact->kind != TP_EXPORT_ARTIFACT_PAGE ||
            artifact->logical_id != ir->pages[p].artifact_id ||
            !artifact->id || id_len < 0 ||
            (size_t)id_len >= sizeof expected_id ||
            strcmp(artifact->id, expected_id) != 0 ||
            tp_export_page_path(out_path_base, ir->pages[p].artifact_id,
                                expected_path, err) != TP_STATUS_OK ||
            strcmp(artifact->path, expected_path) != 0 ||
            packed->pages[p].w != ir->pages[p].w ||
            packed->pages[p].h != ir->pages[p].h ||
            packed->pages[p].premultiplied != ir->pages[p].premultiplied) {
            free(entries);
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "export page plan entry %d does not match IR and packed page",
                                p);
        }
    }

    publish_lease *leases = NULL;
    tp_status st =
        publish_leases_acquire(entries, output_file_count, &leases, err);
    if (st != TP_STATUS_OK) {
        free(entries);
        return st;
    }

    char staging[TP_FS_STAGE_PATH_MAX];
    if (!tp_fs_stage_dir_create(out_dir, staging, sizeof staging)) {
        st = tp_error_set(
            err, TP_STATUS_BAD_PROJECT,
            "cannot create the export staging directory under '%s' (existing "
            "outputs are untouched)",
            cut > 0 ? out_dir : ".");
        publish_leases_release(leases, output_file_count);
        free(entries);
        return st;
    }

    /* Every planned output maps to exactly one staged file. Staged forms obey
     * the same canonical path bound as final artifacts. */
    bool mapped = true;
    for (int f = 0; mapped && f < output_file_count; f++) {
        mapped = tp_fs_stage_child_path(out_dir, staging, entries[f].destination,
                                        entries[f].staged, TP_IDENTITY_PATH_MAX);
    }
    if (!mapped) {
        tp_fs_remove_tree(staging);
        st = tp_error_set(
            err, TP_STATUS_OUT_OF_BOUNDS,
            "exporter '%s': the staged form of its output set exceeds the "
            "canonical path limit, so nothing was written",
            exp->format->id);
        publish_leases_release(leases, output_file_count);
        free(entries);
        return st;
    }

    tp_export_document *documents = (tp_export_document *)calloc(
        (size_t)plan->document_count, sizeof *documents);
    if (!documents) {
        tp_fs_remove_tree(staging);
        publish_leases_release(leases, output_file_count);
        free(entries);
        return tp_error_set(err, TP_STATUS_OOM,
                            "export publish: OOM allocating serializer documents");
    }
    const tp_export_serialize_ctx ctx = {
        .ir = ir,
        .format = exp->format,
        .plan = plan,
        .notices = notices,
    };
    if (out_serializer_ran) {
        *out_serializer_ran = true;
    }
    st = exp->serialize(&ctx, documents, plan->document_count, err);
    for (int d = 0; st == TP_STATUS_OK && d < plan->document_count; ++d) {
        if (!documents[d].data || documents[d].size == 0U) {
            st = tp_error_set(err, TP_STATUS_BAD_PROJECT,
                              "format '%s' did not serialize document '%s' for '%s'",
                              exp->format->id, plan->artifacts[d].id,
                              entries[d].destination);
        } else if (!tp_fs_write_file(entries[d].staged, documents[d].data,
                                     documents[d].size)) {
            st = tp_error_set(err, TP_STATUS_BAD_PROJECT,
                              "cannot stage export document '%s'",
                              entries[d].destination);
        }
    }
    for (int p = 0; st == TP_STATUS_OK && p < ir->page_count; ++p) {
        const int index = plan->document_count + p;
        st = tp_export_write_page_artifact(&packed->pages[p],
                                           ir->pages[p].artifact_id,
                                           entries[index].staged,
                                           false, err);
    }
    for (int d = 0; d < plan->document_count; ++d) {
        free(documents[d].data);
    }
    free(documents);
    if (st == TP_STATUS_OK) {
        st = publish_verify_staged_set(exp, entries, output_file_count, err);
    }
    if (st == TP_STATUS_OK) {
        st = publish_swap(entries, output_file_count,
                          out_publication_uncertain, err);
    }
    tp_fs_remove_tree(staging);
    publish_leases_release(leases, output_file_count);
    free(entries);
    return st;
}
