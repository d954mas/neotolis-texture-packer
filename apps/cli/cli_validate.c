/* `ntpacker validate <project> [--json] [--strict]` (plan B2). Reports EVERY
 * finding in one run (ai-first.md item 7) as structured data, so an agent fixes
 * them in one edit cycle. Each check mirrors a real export-time failure or
 * ambiguity WITHOUT packing:
 *   missing_source        [error]   a source path that does not exist on disk
 *   duplicate_source      [warning] two sources with the same canonical path (§5.6)
 *   source_collision      [warning] two source paths that case-fold-collide (§5.3)
 *   source_portability    [warning] reserved-name/invalid-char/trailing-dot-space (§5.6)
 *   source_escapes_root   [warning] source is absolute or '..'-escapes the project dir
 *   empty_atlas           [warning] no usable sprites resolved from the sources
 *   dangling_anim_frame   [error]   frame key matching no sprite (tp_normalize L-4)
 *   duplicate_export_key  [warning] two descs -> one key (per-sprite override ambiguity)
 *   export_name_collision [error]   two sprites -> one final name (tp_normalize collision)
 *   unknown_exporter      [error]   target exporter_id tp_exporter_find cannot resolve
 *   setting_out_of_range  [error]   a knob outside tp_pack's accepted range
 *   sprite_bad_source     [warning] a v4 override's source id is absent from the atlas -- the
 *                                   source was removed; orphaned, reactivates by name (§5.2/§5.6)
 *   frame_bad_source      [error]   a v4 frame ref's source id is absent from the atlas -- the
 *                                   animation breaks at export (also flagged dangling_anim_frame) (§5.6)
 *   orphan_sprite         [warning] a v4 record resolves to no current sprite (stored orphan, §5.2/§5.6)
 *   duplicate_sprite_key  [warning] two v4 sprite records share one (source, key) (§5.6)
 *
 * Exit (plan L-1): parse+run OK -> 0 (findings in the payload); load failure -> 3;
 * --strict AND any error-severity finding -> 7. */
#include "cli_cmds.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_exit.h"
#include "cli_out.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_export.h" /* tp_exporter_find */
#include "tp_core/tp_pack.h"   /* tp_pack_settings + tp_project_atlas_to_settings */
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_sprite_index.h" /* single resolved index: export-key + §5.6 record checks (F1-03) */
#include "tp_core/tp_srckey.h"       /* source-path collision + portability validation (F1-02) */

#define CLI_VALIDATE_SCHEMA 1

/* Max page dim: DUPLICATES tp_pack.c's TP_PACK_MAX_PAGE_DIM (== engine
 * NT_BUILD_MAX_TEXTURE_SIZE). validate_settings() is private and inseparable from
 * packing, so the range is re-stated here as a data-driven check (plan B2 (g));
 * packet B's X-macro settings schema is earmarked to absorb this duplication.
 * TRACK the build constant (this build overrides it to 16384) rather than hardcode
 * 4096, so `validate` does not flag a page size `set`/`pack` accept (F2-05b-i F4). */
#ifndef NT_BUILD_MAX_TEXTURE_SIZE
#define CLI_MAX_PAGE_DIM 4096
#else
#define CLI_MAX_PAGE_DIM NT_BUILD_MAX_TEXTURE_SIZE
#endif

#define SEV_WARN 0
#define SEV_ERR 1

typedef struct {
    int severity;
    const char *code; /* static string literal */
    char msg[256];
    /* context fields; empty string = not present (names are never empty). */
    char atlas[128];
    char sprite[256];
    char anim[128];
    char frame[256];
    char target[64];
} cli_finding;

typedef struct {
    cli_finding *v;
    int n;
    int cap;
    bool oom; /* sticky: a failed grow stops collection; caller reports oom */
} cli_findings;

static cli_finding *findings_new(cli_findings *fs) {
    if (fs->oom) {
        return NULL;
    }
    if (fs->n == fs->cap) {
        int nc = fs->cap ? fs->cap * 2 : 16;
        cli_finding *nv = (cli_finding *)realloc(fs->v, (size_t)nc * sizeof *nv);
        if (!nv) {
            fs->oom = true;
            return NULL;
        }
        fs->v = nv;
        fs->cap = nc;
    }
    cli_finding *f = &fs->v[fs->n++];
    memset(f, 0, sizeof *f);
    return f;
}

static void set_field(char *dst, size_t cap, const char *v) {
    if (v) {
        (void)snprintf(dst, cap, "%s", v);
    }
}

/* Appends one finding. NULL context fields are omitted. Never aborts: on OOM the
 * findings list poisons and the caller reports it as an internal error. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 9, 10)))
#endif
static void
add_finding(cli_findings *fs, int severity, const char *code, const char *atlas, const char *sprite, const char *anim,
            const char *frame, const char *target, const char *fmt, ...) {
    cli_finding *f = findings_new(fs);
    if (!f) {
        return;
    }
    f->severity = severity;
    f->code = code;
    set_field(f->atlas, sizeof f->atlas, atlas);
    set_field(f->sprite, sizeof f->sprite, sprite);
    set_field(f->anim, sizeof f->anim, anim);
    set_field(f->frame, sizeof f->frame, frame);
    set_field(f->target, sizeof f->target, target);
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(f->msg, sizeof f->msg, fmt, ap);
    va_end(ap);
}

/* (g) one knob range check; matches tp_pack's own severity (INVALID_ARGUMENT -> a
 * pack failure), so an out-of-range knob is an ERROR. */
static void check_range(cli_findings *fs, const char *atlas, const char *knob, long val, long lo, long hi) {
    if (val < lo || val > hi) {
        add_finding(fs, SEV_ERR, "setting_out_of_range", atlas, NULL, NULL, NULL, NULL,
                    "%s = %ld is out of range [%ld..%ld]", knob, val, lo, hi);
    }
}

/* Lower-bound-only knob check (padding/margin/extrude have no documented max). */
static void check_min(cli_findings *fs, const char *atlas, const char *knob, long val, long lo) {
    if (val < lo) {
        add_finding(fs, SEV_ERR, "setting_out_of_range", atlas, NULL, NULL, NULL, NULL, "%s = %ld must be >= %ld", knob,
                    val, lo);
    }
}

/* Count of descs whose export key equals `key` (frame membership + dup detection).
 * `keys` borrow the resolved index / project strings -- read-only, not owned here. */
static bool key_in_set(const char *const *keys, int n, const char *k) {
    for (int i = 0; i < n; i++) {
        if (strcmp(keys[i], k) == 0) {
            return true;
        }
    }
    return false;
}

/* Reports duplicated values in `vals[0..n)` once per distinct duplicate. `code`/
 * `severity` select which check; the shared field is `sprite` (the key or final name). */
static void report_duplicates(cli_findings *fs, const char *atlas, const char *const *vals, int n, int severity,
                              const char *code, const char *what) {
    for (int i = 0; i < n; i++) {
        bool first_occurrence = true;
        for (int j = 0; j < i; j++) {
            if (strcmp(vals[i], vals[j]) == 0) {
                first_occurrence = false;
                break;
            }
        }
        if (!first_occurrence) {
            continue;
        }
        int count = 1;
        for (int j = i + 1; j < n; j++) {
            if (strcmp(vals[i], vals[j]) == 0) {
                count++;
            }
        }
        if (count > 1) {
            add_finding(fs, severity, code, atlas, vals[i], NULL, NULL, NULL, "%d sprites %s '%s'", count, what,
                        vals[i]);
        }
    }
}

/* One source path's precomputed comparison keys (Fix B: canonicalize + case-fold
 * ONCE per source, not per pair). `canon` is the tp_srckey canonical NFC key; for a
 * source tp_srckey_normalize rejects (a legitimately absolute or '..'-escaping
 * external source) it falls back to a slash-normalized copy so the pairwise compare
 * still runs without aborting or false-positiving. `fold` is the case-fold of canon
 * (or a copy of canon when the fold would not fit -- degrades to an exact compare). */
typedef struct {
    char canon[TP_SRCKEY_MAX];
    char fold[TP_SRCKEY_MAX];
} src_key;

/* Tolerant fallback canonical: copy `src` into `out` (properly sized: TP_SRCKEY_MAX,
 * never a fixed 1024) replacing '\\' with '/'. Used when tp_srckey_normalize rejects
 * a path so exact-duplicate detection still works on it. */
static void slash_norm(const char *src, char *out, size_t cap) {
    size_t i = 0;
    for (; src[i] != '\0' && i + 1U < cap; i++) {
        out[i] = (src[i] == '\\') ? '/' : src[i];
    }
    out[i] = '\0';
}

/* (a2) §5.3/§5.6 source-path validation (all WARNINGS -- never flips the --strict
 * exit): exact duplicate, cross-platform case-fold collision, portability, and a
 * non-portable absolute/escaping source, via the promoted tp_srckey primitives.
 * Each path is canonicalized + case-folded ONCE into a properly-sized buffer, then
 * compared pairwise on the precomputed forms (O(n) normalize + O(n) casefold +
 * O(n^2) memcmp, no per-pair allocation). This catches the './' / '//' / trailing-
 * slash / NFC spellings of one folder that the old fixed-1024 slash-only key missed,
 * and never mistakes two distinct >1024-byte paths for a duplicate. */
static void validate_sources(cli_findings *fs, const tp_project_atlas *a) {
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
        const char *path = a->sources[i].path;
        tp_error err = {0};
        tp_status st = tp_srckey_normalize(path, k[i].canon, sizeof k[i].canon, &err);
        if (st == TP_STATUS_OK) {
            unsigned flags = TP_SRCKEY_PORT_OK;
            if (tp_srckey_portability(k[i].canon, &flags, NULL) == TP_STATUS_OK && flags != TP_SRCKEY_PORT_OK) {
                add_finding(fs, SEV_WARN, "source_portability", a->name, NULL, NULL, NULL, NULL,
                            "source '%s' has non-portable path parts:%s%s%s", path,
                            (flags & TP_SRCKEY_PORT_RESERVED_NAME) ? " reserved-name" : "",
                            (flags & TP_SRCKEY_PORT_INVALID_CHAR) ? " invalid-char" : "",
                            (flags & TP_SRCKEY_PORT_TRAILING_DOT_SPACE) ? " trailing-dot-space" : "");
            }
        } else {
            /* A source path is project-relative but MAY legitimately be absolute or
             * escape the project dir (a shared art folder): tp_srckey_normalize
             * rejects those. Surface it as a WARNING and fall back to a tolerant
             * slash-normalized key -- never abort validate, never false-positive a
             * duplicate. Invalid-UTF-8 / over-long paths land in the else branch. */
            if (st == TP_STATUS_KEY_ABSOLUTE || st == TP_STATUS_KEY_TRAVERSAL) {
                add_finding(fs, SEV_WARN, "source_escapes_root", a->name, NULL, NULL, NULL, NULL,
                            "source '%s' is absolute or escapes the project directory (not portable across machines)",
                            path);
            } else {
                add_finding(fs, SEV_WARN, "source_portability", a->name, NULL, NULL, NULL, NULL,
                            "source '%s' could not be canonicalized (%s)", path, err.msg);
            }
            slash_norm(path, k[i].canon, sizeof k[i].canon);
        }
        /* Case-fold the canonical form once (for the O(n^2) collision compare). If it
         * will not fit (a near-limit non-ASCII key whose fold expands ~3x) degrade to
         * an exact compare for this entry -- never false-positive. */
        if (tp_srckey_casefold(k[i].canon, k[i].fold, sizeof k[i].fold, NULL) != TP_STATUS_OK) {
            (void)snprintf(k[i].fold, sizeof k[i].fold, "%s", k[i].canon);
        }
    }
    /* Pairwise on the precomputed keys: an exact duplicate (never also a collision),
     * else a case-fold collision. Mirrors the prior control flow, no per-pair alloc. */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < i; j++) {
            if (strcmp(k[i].canon, k[j].canon) == 0) {
                add_finding(fs, SEV_WARN, "duplicate_source", a->name, NULL, NULL, NULL, NULL,
                            "source '%s' is listed more than once", a->sources[i].path);
                continue;
            }
            if (strcmp(k[i].fold, k[j].fold) == 0) {
                add_finding(fs, SEV_WARN, "source_collision", a->name, NULL, NULL, NULL, NULL,
                            "sources '%s' and '%s' collide case-insensitively (cross-platform name clash)",
                            a->sources[j].path, a->sources[i].path);
            }
        }
    }
    free(k);
}

static bool source_id_in_atlas(const tp_project_atlas *a, tp_id128 sid) {
    for (int i = 0; i < a->source_count; i++) {
        if (tp_id128_eq(a->sources[i].id, sid)) {
            return true;
        }
    }
    return false;
}

/* (h) §5.6 sprite-record integrity, over the resolved index `idx`:
 *   MIGRATED records (stored {source,key}) are id-checked:
 *     sprite_bad_source / frame_bad_source            a stored source id absent from the
 *         atlas; a per-sprite override -> orphan [warning] (its source was removed on a
 *         routine edit; reactivates by the name bridge if the source returns, §5.2/§5.6);
 *         a frame -> [error] (a frame targeting a gone source breaks the animation, and
 *         tp_normalize hard-errors on it -- also flagged as dangling_anim_frame);
 *     orphan_sprite                        [warning] a valid (source,key) that resolves
 *         to no current sprite (stored orphan; reactivates when the key returns);
 *     duplicate_sprite_key                 [warning] two records sharing one (source,key).
 *   PENDING records have no stored {source,key} to id-check, so they are checked HERE by
 *   their name bridge (never silently skipped: a dangling pending override is an
 *   orphan_sprite [warning] just like a migrated one, and re-keys to {source,key} in F2). */
static void validate_sprite_records(cli_findings *fs, const tp_project_atlas *a, const tp_sprite_index *idx) {
    for (int i = 0; i < a->sprite_count; i++) {
        const tp_project_sprite *s = &a->sprites[i];
        if (tp_id128_is_nil(s->source_ref) || !s->src_key) {
            /* PENDING: no id to check, but it still applies by the name bridge. Verify the
             * bridge resolves to a live sprite so validate does not imply it id-checked a
             * record it could not -- a dangling pending override is an orphan too. */
            int matches = 0;
            if (s->name) {
                (void)tp_sprite_index_by_export_key(idx, s->name, &matches);
            }
            if (matches == 0) {
                add_finding(fs, SEV_WARN, "orphan_sprite", a->name, s->name, NULL, NULL, NULL,
                            "sprite override '%s' matches no current sprite "
                            "(orphaned by name; not yet id-validated, re-keys to {source, key} in F2)",
                            s->name ? s->name : "");
            }
            continue;
        }
        if (!source_id_in_atlas(a, s->source_ref)) {
            /* The owning source was removed from the atlas (a routine edit). This is an
             * ORPHAN, not a hard error: the override applies to nothing now and reactivates
             * by its name bridge if the source returns (§5.2/§5.6). WARNING, so a plain
             * source-removal does not flip `validate --strict` (exit 7). */
            add_finding(fs, SEV_WARN, "sprite_bad_source", a->name, s->name, NULL, NULL, NULL,
                        "sprite override '%s' references a source id not in this atlas "
                        "(source removed; orphaned, reactivates by name if the source returns)",
                        s->name ? s->name : "");
            continue;
        }
        if (!tp_sprite_index_by_source_key(idx, s->source_ref, s->src_key)) {
            add_finding(fs, SEV_WARN, "orphan_sprite", a->name, s->name, NULL, NULL, NULL,
                        "sprite override '%s' (key '%s') resolves to no current sprite "
                        "(orphaned; reactivates if the source key returns)",
                        s->name ? s->name : "", s->src_key);
        }
    }
    for (int i = 0; i < a->sprite_count; i++) {
        const tp_project_sprite *si = &a->sprites[i];
        if (tp_id128_is_nil(si->source_ref) || !si->src_key) {
            continue;
        }
        for (int j = 0; j < i; j++) {
            const tp_project_sprite *sj = &a->sprites[j];
            if (tp_id128_is_nil(sj->source_ref) || !sj->src_key) {
                continue;
            }
            if (tp_id128_eq(si->source_ref, sj->source_ref) && strcmp(si->src_key, sj->src_key) == 0) {
                add_finding(fs, SEV_WARN, "duplicate_sprite_key", a->name, si->src_key, NULL, NULL, NULL,
                            "two sprite overrides share the same (source, key) '%s'", si->src_key);
                break;
            }
        }
    }
    for (int an = 0; an < a->animation_count; an++) {
        const tp_project_anim *pa = &a->animations[an];
        for (int f = 0; f < pa->frame_count; f++) {
            const tp_project_frame *fr = &pa->frames[f];
            if (tp_id128_is_nil(fr->source_ref) || !fr->src_key) {
                continue;
            }
            if (!source_id_in_atlas(a, fr->source_ref)) {
                add_finding(fs, SEV_ERR, "frame_bad_source", a->name, NULL, pa->name, fr->name, NULL,
                            "animation '%s' frame '%s' references a source id not in this atlas",
                            pa->name ? pa->name : "", fr->name ? fr->name : "");
            }
        }
    }
}

static void validate_atlas(cli_findings *fs, tp_project *p, int ai) {
    tp_project_atlas *a = &p->atlases[ai];

    /* (a) missing sources -- walk per-source so the finding names the offender. */
    for (int s = 0; s < a->source_count; s++) {
        const char *sp = a->sources[s].path;
        char abs[512];
        if (tp_project_resolve_path(p, sp, abs, sizeof abs) != TP_STATUS_OK) {
            add_finding(fs, SEV_ERR, "missing_source", a->name, NULL, NULL, NULL, NULL,
                        "source '%s' cannot be resolved to an absolute path", sp);
        } else if (!tp_scan_exists(abs)) {
            add_finding(fs, SEV_ERR, "missing_source", a->name, NULL, NULL, NULL, NULL,
                        "source '%s' does not exist on disk", sp);
        }
    }
    validate_sources(fs, a); /* (a2) duplicate / case-fold collision / portability */

    /* Build ONE resolved sprite index (single disk scan) and feed BOTH the export-key /
     * dangling-frame checks AND the §5.6 record checks from it (fix [7]). The index mirrors
     * tp_pack_input_build's iteration EXACTLY (same sources, same order, same raw names), so
     * ref[i].export_key equals the export key of pack desc[i] -- the validate output is
     * identical to the old two-scan version, at one scan instead of two. */
    tp_sprite_index sidx;
    tp_error ierr = {0};
    if (tp_sprite_index_build(p, ai, &sidx, &ierr) != TP_STATUS_OK) {
        add_finding(fs, SEV_ERR, "input_build_failed", a->name, NULL, NULL, NULL, NULL, "%s", ierr.msg);
    } else {
        int n = sidx.count;
        if (n == 0) {
            /* (b) an atlas that resolves no sprites packs nothing. */
            add_finding(fs, SEV_WARN, "empty_atlas", a->name, NULL, NULL, NULL, NULL,
                        "atlas has no usable sprites (no images resolved from its sources)");
        }
        /* keys[]/finals[] BORROW the index's export keys (and any project rename string) --
         * no per-desc allocation. finals default to the key; a project rename replaces the
         * FIRST desc whose key matches, exactly as build_norm_opts / export does. */
        const char **keys = NULL;
        const char **finals = NULL;
        bool alloc_ok = true;
        if (n > 0) {
            keys = (const char **)calloc((size_t)n, sizeof *keys);
            finals = (const char **)calloc((size_t)n, sizeof *finals);
            alloc_ok = keys && finals;
            for (int i = 0; alloc_ok && i < n; i++) {
                keys[i] = sidx.refs[i].export_key;
                finals[i] = sidx.refs[i].export_key;
            }
            for (int si = 0; alloc_ok && si < a->sprite_count; si++) {
                const tp_project_sprite *ps = &a->sprites[si];
                if (!ps->rename || ps->rename[0] == '\0') {
                    continue;
                }
                for (int d = 0; d < n; d++) {
                    if (strcmp(keys[d], ps->name) == 0) {
                        finals[d] = ps->rename;
                        break;
                    }
                }
            }
        }
        if (!alloc_ok) {
            fs->oom = true;
        } else {
            /* (d) two descs -> one export key: per-sprite overrides become ambiguous. */
            report_duplicates(fs, a->name, keys, n, SEV_WARN, "duplicate_export_key", "map to export key");
            /* (e) two sprites -> one final name: tp_normalize would hard-error. */
            report_duplicates(fs, a->name, finals, n, SEV_ERR, "export_name_collision", "resolve to export name");
            /* (c) dangling anim frames: a frame key matching no sprite (== the
             * condition tp_normalize hard-errors on at export). */
            for (int an = 0; an < a->animation_count; an++) {
                const tp_project_anim *pa = &a->animations[an];
                for (int f = 0; f < pa->frame_count; f++) {
                    const char *fr = pa->frames[f].name ? pa->frames[f].name : "";
                    if (!key_in_set(keys, n, fr)) {
                        add_finding(fs, SEV_ERR, "dangling_anim_frame", a->name, NULL, pa->name, fr, NULL,
                                    "animation '%s' references frame '%s' which matches no sprite export key",
                                    pa->name, fr);
                    }
                }
            }
            /* (h) §5.6 sprite-record integrity over the SAME resolved index. */
            validate_sprite_records(fs, a, &sidx);
        }
        free((void *)keys);
        free((void *)finals);
    }
    tp_sprite_index_free(&sidx);

    /* (f) unknown exporter -- reported for every target (enabled or not): the id is
     * broken data regardless of enable state. */
    for (int t = 0; t < a->target_count; t++) {
        const tp_project_target *tg = &a->targets[t];
        if (!tp_exporter_find(tg->exporter_id)) {
            add_finding(fs, SEV_ERR, "unknown_exporter", a->name, NULL, NULL, NULL, tg->exporter_id,
                        "target references unknown exporter '%s'", tg->exporter_id);
        }
    }

    /* (g) knob ranges over the export-path settings (clamp applied). */
    tp_pack_settings sset;
    tp_error serr = {0};
    if (tp_project_atlas_to_settings(p, ai, &sset, &serr) == TP_STATUS_OK) {
        check_range(fs, a->name, "max_size", sset.max_size, 1, CLI_MAX_PAGE_DIM);
        check_min(fs, a->name, "padding", sset.padding, 0);
        check_min(fs, a->name, "margin", sset.margin, 0);
        check_min(fs, a->name, "extrude", sset.extrude, 0);
        check_range(fs, a->name, "alpha_threshold", sset.alpha_threshold, 0, 255);
        check_range(fs, a->name, "max_vertices", sset.max_vertices, 1, 16);
        check_range(fs, a->name, "shape", sset.shape, 0, 2);
        if (!(sset.pixels_per_unit > 0.0F) || !isfinite(sset.pixels_per_unit)) {
            add_finding(fs, SEV_ERR, "setting_out_of_range", a->name, NULL, NULL, NULL, NULL,
                        "pixels_per_unit must be positive and finite");
        }
    }
}

/* --- JSON payload --- */

static void key(cli_sb *sb, int depth, bool *first, const char *k) {
    cli_sb_str(sb, *first ? "\n" : ",\n");
    *first = false;
    cli_sb_indent(sb, depth);
    cli_sb_json_str(sb, k);
    cli_sb_str(sb, ": ");
}

static void emit_ctx(cli_sb *sb, int depth, bool *first, const char *k, const char *v) {
    if (v[0] == '\0') {
        return;
    }
    key(sb, depth, first, k);
    cli_sb_json_str(sb, v);
}

static void build_validate(cli_sb *sb, const cli_findings *fs, int errors, int warnings) {
    bool first = true;
    cli_sb_putc(sb, '{');
    key(sb, 1, &first, "schema");
    cli_sb_int(sb, CLI_VALIDATE_SCHEMA);

    key(sb, 1, &first, "findings");
    if (fs->n == 0) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = 0; i < fs->n; i++) {
            const cli_finding *f = &fs->v[i];
            cli_sb_str(sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(sb, 2);
            bool ff = true;
            cli_sb_putc(sb, '{');
            key(sb, 3, &ff, "severity");
            cli_sb_json_str(sb, f->severity == SEV_ERR ? "error" : "warning");
            key(sb, 3, &ff, "code");
            cli_sb_json_str(sb, f->code);
            key(sb, 3, &ff, "message");
            cli_sb_json_str(sb, f->msg);
            emit_ctx(sb, 3, &ff, "atlas", f->atlas);
            emit_ctx(sb, 3, &ff, "sprite", f->sprite);
            emit_ctx(sb, 3, &ff, "anim", f->anim);
            emit_ctx(sb, 3, &ff, "frame", f->frame);
            emit_ctx(sb, 3, &ff, "target", f->target);
            cli_sb_str(sb, "\n");
            cli_sb_indent(sb, 2);
            cli_sb_putc(sb, '}');
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, 1);
        cli_sb_putc(sb, ']');
    }

    key(sb, 1, &first, "counts");
    {
        bool cf = true;
        cli_sb_putc(sb, '{');
        key(sb, 2, &cf, "error");
        cli_sb_int(sb, errors);
        key(sb, 2, &cf, "warning");
        cli_sb_int(sb, warnings);
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, 1);
        cli_sb_putc(sb, '}');
    }
    cli_sb_str(sb, "\n}");
}

static void print_validate_human(const cli_findings *fs, const char *path, int errors, int warnings) {
    (void)printf("%s: %d error%s, %d warning%s\n", path, errors, errors == 1 ? "" : "s", warnings,
                 warnings == 1 ? "" : "s");
    for (int i = 0; i < fs->n; i++) {
        const cli_finding *f = &fs->v[i];
        (void)printf("  [%s] %s: %s\n", f->severity == SEV_ERR ? "error" : "warning", f->code, f->msg);
    }
}

int cmd_validate(const char *path, bool json, bool quiet, bool strict) {
    tp_project *p = NULL;
    int rc = cli_load_project(path, json, quiet, &p);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    cli_findings fs = {0};
    for (int ai = 0; ai < p->atlas_count; ai++) {
        validate_atlas(&fs, p, ai);
    }
    if (fs.oom) {
        free(fs.v);
        tp_project_destroy(p);
        cli_emit_error(json, quiet, "oom", "out of memory collecting validation findings");
        return CLI_EXIT_INTERNAL;
    }

    int errors = 0;
    int warnings = 0;
    for (int i = 0; i < fs.n; i++) {
        if (fs.v[i].severity == SEV_ERR) {
            errors++;
        } else {
            warnings++;
        }
    }

    if (json) {
        cli_sb sb = {0};
        build_validate(&sb, &fs, errors, warnings);
        if (sb.oom) {
            cli_sb_free(&sb);
            free(fs.v);
            tp_project_destroy(p);
            cli_emit_error(true, false, "oom", "out of memory building validate payload");
            return CLI_EXIT_INTERNAL;
        }
        cli_out_stdout(&sb);
        cli_sb_free(&sb);
    } else {
        print_validate_human(&fs, path, errors, warnings);
    }

    free(fs.v);
    tp_project_destroy(p);
    return (strict && errors > 0) ? CLI_EXIT_VALIDATE : CLI_EXIT_OK;
}
