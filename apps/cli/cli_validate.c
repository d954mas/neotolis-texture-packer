/* `ntpacker validate <project> [--json] [--strict]` (plan B2). Reports EVERY
 * finding in one run (ai-first.md item 7) as structured data, so an agent fixes
 * them in one edit cycle. Each check mirrors a real export-time failure or
 * ambiguity WITHOUT packing:
 *   missing_source        [error]   a source path that does not exist on disk
 *   duplicate_source      [warning] two sources with the same normalized path (§5.6)
 *   source_collision      [warning] two source paths that case-fold-collide (§5.3)
 *   source_portability    [warning] reserved-name/invalid-char/trailing-dot-space (§5.6)
 *   empty_atlas           [warning] no usable sprites resolved from the sources
 *   dangling_anim_frame   [error]   frame key matching no sprite (tp_normalize L-4)
 *   duplicate_export_key  [warning] two descs -> one key (per-sprite override ambiguity)
 *   export_name_collision [error]   two sprites -> one final name (tp_normalize collision)
 *   unknown_exporter      [error]   target exporter_id tp_exporter_find cannot resolve
 *   setting_out_of_range  [error]   a knob outside tp_pack's accepted range
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
#include "tp_core/tp_input.h"
#include "tp_core/tp_names.h"
#include "tp_core/tp_pack.h" /* tp_pack_settings + tp_project_atlas_to_settings */
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_srckey.h" /* source-path collision + portability validation (F1-02) */

#define CLI_VALIDATE_SCHEMA 1

/* Max page dim: DUPLICATES tp_pack.c's TP_PACK_MAX_PAGE_DIM (== engine
 * NT_BUILD_MAX_TEXTURE_SIZE). validate_settings() is private and inseparable from
 * packing, so the range is re-stated here as a data-driven check (plan B2 (g));
 * packet B's X-macro settings schema is earmarked to absorb this duplication. */
#define CLI_MAX_PAGE_DIM 4096

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

/* Local strdup (portable: no _strdup / POSIX strdup dependency). NULL on OOM. */
static char *dup_str(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1U;
    char *p = (char *)malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

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

/* Count of descs whose export key equals `key` (frame membership + dup detection). */
static bool key_in_set(char *const *keys, int n, const char *k) {
    for (int i = 0; i < n; i++) {
        if (strcmp(keys[i], k) == 0) {
            return true;
        }
    }
    return false;
}

/* Reports duplicated values in `vals[0..n)` once per distinct duplicate. `code`/
 * `severity` select which check; the shared field is `sprite` (the key or final name). */
static void report_duplicates(cli_findings *fs, const char *atlas, char *const *vals, int n, int severity,
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

static void free_strv(char **v, int n) {
    if (!v) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(v[i]);
    }
    free(v);
}

/* Copy `src` into `out` replacing '\\' with '/' so exact-duplicate + case-fold
 * comparison see one separator convention (saved sources are '/'-normalized, but a
 * hand-edited file may not be). Truncation is harmless for a comparison key. */
static void norm_src(const char *src, char *out, size_t cap) {
    size_t i = 0;
    for (; src[i] != '\0' && i + 1U < cap; i++) {
        out[i] = (src[i] == '\\') ? '/' : src[i];
    }
    out[i] = '\0';
}

/* (a2) §5.3/§5.6 source-path validation (all WARNINGS -- never flips the --strict
 * exit): duplicate path, cross-platform case-fold collision, and portability, via
 * the promoted tp_srckey primitives. O(n^2) over an atlas's sources (small). */
static void validate_sources(cli_findings *fs, const tp_project_atlas *a) {
    for (int i = 0; i < a->source_count; i++) {
        char ni[1024];
        norm_src(a->sources[i].path, ni, sizeof ni);
        unsigned flags = TP_SRCKEY_PORT_OK;
        if (tp_srckey_portability(ni, &flags, NULL) == TP_STATUS_OK && flags != TP_SRCKEY_PORT_OK) {
            add_finding(fs, SEV_WARN, "source_portability", a->name, NULL, NULL, NULL, NULL,
                        "source '%s' has non-portable path parts:%s%s%s", a->sources[i].path,
                        (flags & TP_SRCKEY_PORT_RESERVED_NAME) ? " reserved-name" : "",
                        (flags & TP_SRCKEY_PORT_INVALID_CHAR) ? " invalid-char" : "",
                        (flags & TP_SRCKEY_PORT_TRAILING_DOT_SPACE) ? " trailing-dot-space" : "");
        }
        for (int j = 0; j < i; j++) {
            char nj[1024];
            norm_src(a->sources[j].path, nj, sizeof nj);
            if (strcmp(ni, nj) == 0) {
                add_finding(fs, SEV_WARN, "duplicate_source", a->name, NULL, NULL, NULL, NULL,
                            "source '%s' is listed more than once", a->sources[i].path);
                continue;
            }
            bool collides = false;
            if (tp_srckey_collides(ni, nj, &collides, NULL) == TP_STATUS_OK && collides) {
                add_finding(fs, SEV_WARN, "source_collision", a->name, NULL, NULL, NULL, NULL,
                            "sources '%s' and '%s' collide case-insensitively (cross-platform name clash)",
                            a->sources[j].path, a->sources[i].path);
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

    /* Assemble the descs a pack would use (DRY: no packing, disk-touching only). */
    tp_pack_input input;
    tp_error err = {0};
    tp_status bst = tp_pack_input_build(p, ai, &input, &err);
    if (bst != TP_STATUS_OK) {
        add_finding(fs, SEV_ERR, "input_build_failed", a->name, NULL, NULL, NULL, NULL, "%s", err.msg);
    } else {
        if (input.count == 0) {
            /* (b) an atlas that resolves no sprites packs nothing. */
            add_finding(fs, SEV_WARN, "empty_atlas", a->name, NULL, NULL, NULL, NULL,
                        "atlas has no usable sprites (no images resolved from its sources)");
        }
        int n = input.count;
        char **keys = NULL;
        char **finals = NULL;
        bool alloc_ok = true;
        if (n > 0) {
            keys = (char **)calloc((size_t)n, sizeof *keys);
            finals = (char **)calloc((size_t)n, sizeof *finals);
            alloc_ok = keys && finals;
            for (int i = 0; alloc_ok && i < n; i++) {
                char kb[256];
                tp_sprite_export_key(input.descs[i].name, kb, sizeof kb);
                keys[i] = dup_str(kb);
                finals[i] = dup_str(kb); /* default final = key; rename applied below */
                if (!keys[i] || !finals[i]) {
                    alloc_ok = false;
                }
            }
            /* Final names mirror build_norm_opts: a project rename is keyed by the
             * export key and applied to the FIRST matching desc (same as export). */
            for (int si = 0; alloc_ok && si < a->sprite_count; si++) {
                const tp_project_sprite *ps = &a->sprites[si];
                if (!ps->rename || ps->rename[0] == '\0') {
                    continue;
                }
                for (int d = 0; d < n; d++) {
                    if (strcmp(keys[d], ps->name) == 0) {
                        char *nf = dup_str(ps->rename);
                        if (!nf) {
                            alloc_ok = false;
                        } else {
                            free(finals[d]);
                            finals[d] = nf;
                        }
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
                    const char *fr = pa->frames[f] ? pa->frames[f] : "";
                    if (!key_in_set(keys, n, fr)) {
                        add_finding(fs, SEV_ERR, "dangling_anim_frame", a->name, NULL, pa->name, fr, NULL,
                                    "animation '%s' references frame '%s' which matches no sprite export key",
                                    pa->name, fr);
                    }
                }
            }
        }
        free_strv(keys, n);
        free_strv(finals, n);
    }
    tp_pack_input_free(&input);

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
    if (tp_project_atlas_to_settings(p, ai, &sset, &err) == TP_STATUS_OK) {
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
