#include "tp_validate_rules_internal.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_identity.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_srckey.h"
#include "tp_srckey_internal.h"
#include "tp_validate_index_internal.h"

/* One source path's precomputed comparison keys (Fix B: canonicalize + case-fold
 * ONCE per source, not per pair). `canon` is the tp_srckey canonical NFC key; for a
 * source tp_srckey_normalize rejects (a legitimately absolute or '..'-escaping
 * external source) it falls back to a slash-normalized copy so the pairwise compare
 * still runs without aborting or false-positiving. `fold` is the case-fold of canon
 * (or a copy of canon when the fold would not fit -- degrades to an exact compare). */
typedef struct {
    char *canon;
    char *fold;
} src_key;

/* Tolerant fallback canonical: copy `src` into `out` (properly sized: TP_SRCKEY_MAX,
 * never a fixed 1024) replacing '\\' with '/'. Used when tp_srckey_normalize rejects
 * a path so exact-duplicate detection still works on it. */
static char *validate_strdup(const char *src) {
    const size_t len = strlen(src);
    char *copy = (char *)malloc(len + 1U);
    if (copy) {
        memcpy(copy, src, len + 1U);
    }
    return copy;
}

char *validation_slash_norm_owned(const char *src) {
    char *out = validate_strdup(src);
    if (!out) {
        return NULL;
    }
    for (char *c = out; *c; ++c) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    return out;
}

static void src_keys_free(src_key *keys, int count) {
    if (keys) {
        for (int i = 0; i < count; ++i) {
            free(keys[i].canon);
            free(keys[i].fold);
        }
    }
    free(keys);
}
/* (a2) §5.3/§5.6 source-path validation (all WARNINGS -- never flips the --strict
 * exit): exact duplicate, cross-platform case-fold collision, portability, and a
 * non-portable absolute/escaping source, via the tp_srckey primitives.
 * Each path is canonicalized + case-folded once into owned full-length storage, then
 * grouped by folded key. Work is linear for distinct keys; colliding pairs are emitted
 * only while the bounded report has room and the remainder is counted without walking
 * every pair. This catches './' / '//' / trailing-slash / NFC spellings and never
 * mistakes two distinct long paths with a shared prefix for a duplicate. */
void validate_source_domain(validation_builder *fs, const tp_project *project,
                            const tp_project_atlas *a) {
    /* Missing sources stay first in the stable finding order. */
    for (int s = 0; s < a->source_count; s++) {
        const tp_project_source *source = &a->sources[s];
        const char *path = source->path;
        char absolute[TP_IDENTITY_PATH_MAX];
        if (tp_project_resolve_source_path(project, path, absolute,
                                           sizeof absolute) != TP_STATUS_OK) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_MISSING_SOURCE,
                        context_source(a, source),
                        "source '%s' cannot be resolved to an absolute path",
                        path);
        } else if (!tp_scan_exists(absolute)) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_MISSING_SOURCE,
                        context_source(a, source),
                        "source '%s' does not exist on disk", path);
        }
    }
    int n = a->source_count;
    if (n <= 0) {
        return;
    }
    src_key *k = (src_key *)calloc((size_t)n, sizeof *k);
    if (!k) {
        fs->oom = true;
        return;
    }
    for (int i = 0; i < n; i++) {
        const char *path = a->sources[i].path ? a->sources[i].path : "";
        char normalized[TP_SRCKEY_MAX];
        tp_error err = {0};
        tp_status st = tp_srckey_normalize(path, normalized,
                                           sizeof normalized, &err);
        if (st == TP_STATUS_OK) {
            unsigned flags = TP_SRCKEY_PORT_OK;
            k[i].canon = validate_strdup(normalized);
            if (tp_srckey_portability(normalized, &flags, NULL) == TP_STATUS_OK && flags != TP_SRCKEY_PORT_OK) {
                add_finding(fs, TP_VALIDATION_WARNING,
                            TP_VALIDATION_CODE_SOURCE_PORTABILITY,
                            context_source(a, &a->sources[i]),
                            "source '%s' has non-portable path parts:%s%s%s", path,
                            (flags & TP_SRCKEY_PORT_RESERVED_NAME) ? " reserved-name" : "",
                            (flags & TP_SRCKEY_PORT_INVALID_CHAR) ? " invalid-char" : "",
                            (flags & TP_SRCKEY_PORT_TRAILING_DOT_SPACE) ? " trailing-dot-space" : "");
            }
        } else if (st == TP_STATUS_OOM) {
            fs->oom = true;
            break;
        } else {
            /* A source path is project-relative but MAY legitimately be absolute or
             * escape the project dir (a shared art folder): tp_srckey_normalize
             * rejects those. Surface it as a WARNING and fall back to a tolerant
             * slash-normalized key -- never abort validate, never false-positive a
             * duplicate. Invalid-UTF-8 / over-long paths land in the else branch. */
            if (st == TP_STATUS_KEY_ABSOLUTE || st == TP_STATUS_KEY_TRAVERSAL) {
                add_finding(fs, TP_VALIDATION_WARNING,
                            TP_VALIDATION_CODE_SOURCE_ESCAPES_ROOT,
                            context_source(a, &a->sources[i]),
                            "source '%s' is absolute or escapes the project directory (not portable across machines)",
                            path);
            } else {
                add_finding(fs, TP_VALIDATION_WARNING,
                            TP_VALIDATION_CODE_SOURCE_PORTABILITY,
                            context_source(a, &a->sources[i]),
                            "source '%s' could not be canonicalized (%s)", path, err.msg);
            }
            k[i].canon = validation_slash_norm_owned(path);
        }
        if (!k[i].canon) {
            fs->oom = true;
            break;
        }
        tp_error fold_err = {0};
        st = tp_srckey__casefold_owned(k[i].canon, &k[i].fold, &fold_err);
        if (st == TP_STATUS_OOM) {
            fs->oom = true;
            break;
        }
        if (st != TP_STATUS_OK) {
            /* Invalid UTF-8 was already surfaced as a portability warning. Keep
             * exact full bytes comparable without inventing a lossy fold. */
            k[i].fold = validate_strdup(k[i].canon);
            if (!k[i].fold) {
                fs->oom = true;
                break;
            }
        }
    }
    if (fs->oom) {
        src_keys_free(k, n);
        return;
    }
    /* Group by the case-folded key and retain prior indices in source order. This
     * emits the exact same small-report pair order as the old nested scan. Once
     * the materialization budget is full, the remaining same-fold pairs are
     * counted in O(1) per source and represented by the truncation summary. */
    str_index fold_groups = {0};
    int *next = (int *)malloc((size_t)n * sizeof *next);
    if (!next || !str_index_init(&fold_groups, n)) {
        free(next);
        str_index_free(&fold_groups);
        src_keys_free(k, n);
        fs->oom = true;
        return;
    }
    for (int i = 0; i < n; i++) {
        next[i] = -1;
    }
    for (int i = 0; i < n; i++) {
        str_slot *group = str_index_find(&fold_groups, k[i].fold);
        if (!group) {
            fs->oom = true;
            break;
        }
        const size_t previous = group->key ? group->count : 0U;
        size_t visited = 0U;
        int j = group->key ? group->first_index : -1;
        while (visited < previous && report_has_room(fs)) {
            tp_validate_work_probes++;
            if (strcmp(k[i].canon, k[j].canon) == 0) {
                add_finding(fs, TP_VALIDATION_WARNING,
                            TP_VALIDATION_CODE_DUPLICATE_SOURCE,
                            context_source(a, &a->sources[i]),
                            "source '%s' is listed more than once", a->sources[i].path);
            } else {
                add_finding(fs, TP_VALIDATION_WARNING,
                            TP_VALIDATION_CODE_SOURCE_COLLISION,
                            context_source(a, &a->sources[i]),
                            "sources '%s' and '%s' collide case-insensitively (cross-platform name clash)",
                            a->sources[j].path, a->sources[i].path);
            }
            visited++;
            j = next[j];
        }
        if (visited < previous) {
            add_omitted(fs, TP_VALIDATION_WARNING, previous - visited);
        }
        if (!group->key) {
            group->key = k[i].fold;
            group->first_index = i;
        } else {
            next[group->last_index] = i;
        }
        group->last_index = i;
        group->count++;
    }
    free(next);
    str_index_free(&fold_groups);
    src_keys_free(k, n);
}
