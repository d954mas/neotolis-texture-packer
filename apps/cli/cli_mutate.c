/* ntpacker wave-2 mutation verbs (plan docs/plans/op-layer-and-cli.md, step B4).
 *
 * Thin client over tp_core: every verb resolves a selector, calls ONE existing
 * tp_project_* mutator (R4), and re-saves the project with the byte-stable canonical
 * writer. No name/desc/exporter logic here (boundary gates); the two trivial mutators
 * this needed (tp_project_set_atlas_name, tp_project_atlas_prune_sprite) live in
 * tp_core with unit tests, not improvised here.
 *
 * Exit-code split (agents branch on these):
 *   2 usage   : bad grammar/vocabulary/value BEFORE the model is touched -- wrong arg
 *               count, unknown `set`/`sprite set` key, malformed `key=value`, a value
 *               that fails to parse or is out of range, an unknown exporter id, a
 *               duplicate atlas name.
 *   3 project : load/parse error, `new` on an existing path, or a selector that names a
 *               model element not present (atlas/source/anim/frame/target) -- i.e. a
 *               tp_project_* mutator returned a non-OOM failure. (Distinct from pack's
 *               --atlas FILTER flag, which is a usage error: these are positional state
 *               selectors, so "not found" is a project-state class, not a grammar one.)
 *   1 internal: OOM from a mutator or the save, or an environmental/internal id-promote
 *               fault (OS-RNG failure, id-collision-sweep exhaustion) -- never project-content.
 *   0 ok.
 *
 * `set`/`sprite set` value vocabularies = the project-file JSON keys (tp_emit_atlas /
 * tp_emit_sprite); the ranges mirror cli_validate's table. Both are earmarked to be
 * absorbed by packet B's X-macro settings schema.
 */
#include "cli_cmds.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define cli_getcwd _getcwd
#else
#include <unistd.h>
#define cli_getcwd getcwd
#endif

#include "cli_exit.h"
#include "cli_out.h"
#include "ntpacker_id_fmt.h" /* ntpacker_fmt_shape_id (shared with cli_inspect) */
#include "tp_core/tp_error.h"
#include "tp_core/tp_export.h" /* tp_exporter_find (target-id validation) */
#include "tp_core/tp_id.h"     /* tp_rng_os + shape-ID format for anim list */
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids (writable id assignment) */
#include "tp_core/tp_scan.h" /* tp_scan_exists (new-on-existing guard) */

/* DUPLICATES cli_validate.c's CLI_MAX_PAGE_DIM (== tp_pack's TP_PACK_MAX_PAGE_DIM ==
 * engine NT_BUILD_MAX_TEXTURE_SIZE). Re-stated as a data-driven range check; packet
 * B's X-macro settings schema is earmarked to absorb both copies. */
#define CLI_MAX_PAGE_DIM 4096
#define CLI_PATH_MAX 1024

static const char *const k_atlas_knobs =
    "max_size, padding, margin, extrude, alpha_threshold, max_vertices, shape, "
    "allow_transform, power_of_two, pixels_per_unit";
static const char *const k_sprite_fields =
    "origin, slice9, rename, shape, allow_rotate, max_vertices, margin, extrude";

/* ------------------------------------------------------------------ */
/* small parse + path helpers                                         */
/* ------------------------------------------------------------------ */

static void norm_slashes(char *s) {
    for (; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

static bool path_is_abs(const char *p) {
    if (!p || !p[0]) {
        return false;
    }
    if (p[0] == '/' || p[0] == '\\') {
        return true;
    }
    if (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) {
        return true;
    }
    return false;
}

/* Makes `p` absolute against the CWD (need not exist), '/'-normalized. Mirrors how the
 * GUI feeds absolute paths from its file dialogs into tp_project_atlas_add_source; save
 * then relativizes against the project dir. */
static void abspath_cwd(const char *p, char *out, size_t cap) {
    if (path_is_abs(p)) {
        (void)snprintf(out, cap, "%s", p);
    } else {
        char cwd[CLI_PATH_MAX];
        if (cli_getcwd(cwd, (int)sizeof cwd)) {
            (void)snprintf(out, cap, "%s/%s", cwd, p);
        } else {
            (void)snprintf(out, cap, "%s", p);
        }
    }
    norm_slashes(out);
}

/* Case-insensitive on Windows (paths are), case-sensitive elsewhere. */
static bool path_eq(const char *a, const char *b) {
#ifdef _WIN32
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
#else
    return strcmp(a, b) == 0;
#endif
}

/* Lexically canonicalizes `in` (collapse '.'/'..' and duplicate slashes) so two paths
 * that name the same location compare equal -- tp_project_resolve_path returns
 * project_dir + a "../.." relative source verbatim (unresolved), which would not
 * string-equal a clean absolute path. Preserves a POSIX '/' or Windows 'X:/' root;
 * '..' below the root is dropped; a relative input keeps leading '..'. */
static void path_canon(const char *in, char *out, size_t cap) {
    char tmp[CLI_PATH_MAX];
    (void)snprintf(tmp, sizeof tmp, "%s", in);
    norm_slashes(tmp);

    char root[8] = {0};
    char *p = tmp;
    if (isalpha((unsigned char)tmp[0]) && tmp[1] == ':') {
        root[0] = tmp[0];
        root[1] = ':';
        root[2] = '/';
        root[3] = '\0';
        p = tmp + 2;
        if (*p == '/') {
            p++;
        }
    } else if (tmp[0] == '/') {
        root[0] = '/';
        root[1] = '\0';
        p = tmp + 1;
    }

    char *comps[256];
    int n = 0;
    for (char *tok = strtok(p, "/"); tok && n < 256; tok = strtok(NULL, "/")) {
        if (strcmp(tok, ".") == 0) {
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            if (n > 0 && strcmp(comps[n - 1], "..") != 0) {
                n--; /* pop a real component */
            } else if (root[0] == '\0') {
                comps[n++] = tok; /* relative path: keep leading '..' */
            }
            continue; /* at an absolute root, '..' is dropped */
        }
        comps[n++] = tok;
    }

    size_t used = 0;
    out[0] = '\0';
    if (root[0]) {
        used = (size_t)snprintf(out, cap, "%s", root);
    }
    for (int i = 0; i < n && used < cap; i++) {
        int w = snprintf(out + used, cap - used, "%s%s", comps[i], (i + 1 < n) ? "/" : "");
        if (w < 0 || (size_t)w >= cap - used) {
            break;
        }
        used += (size_t)w;
    }
}

/* Splits "key=value" at the first '='. Copies the key into kbuf; returns the value
 * pointer (into `tok`) or NULL when there is no '='. */
static const char *split_kv(const char *tok, char *kbuf, size_t kcap) {
    const char *eq = strchr(tok, '=');
    if (!eq) {
        return NULL;
    }
    size_t klen = (size_t)(eq - tok);
    if (klen >= kcap) {
        klen = kcap - 1U;
    }
    memcpy(kbuf, tok, klen);
    kbuf[klen] = '\0';
    return eq + 1;
}

static bool to_long(const char *s, long *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

static bool to_bool(const char *s, bool *out) {
    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool to_float(const char *s, float *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    double v = strtod(s, &end); /* LC_NUMERIC="C" (main) -> '.' decimal */
    if (end == s || *end != '\0') {
        return false;
    }
    *out = (float)v;
    return true;
}

/* Parses exactly `want` comma-separated longs from `s` into out[]. Too few/many -> false. */
static bool to_longs_csv(const char *s, long *out, int want) {
    for (int i = 0; i < want; i++) {
        const char *comma = strchr(s, ',');
        char buf[64];
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        if (len >= sizeof buf) {
            return false;
        }
        memcpy(buf, s, len);
        buf[len] = '\0';
        if (!to_long(buf, &out[i])) {
            return false;
        }
        if (i < want - 1) {
            if (!comma) {
                return false; /* not enough components */
            }
            s = comma + 1;
        } else if (comma) {
            return false; /* too many components */
        }
    }
    return true;
}

static bool to_floats_csv(const char *s, float *out, int want) {
    for (int i = 0; i < want; i++) {
        const char *comma = strchr(s, ',');
        char buf[64];
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        if (len >= sizeof buf) {
            return false;
        }
        memcpy(buf, s, len);
        buf[len] = '\0';
        if (!to_float(buf, &out[i])) {
            return false;
        }
        if (i < want - 1) {
            if (!comma) {
                return false;
            }
            s = comma + 1;
        } else if (comma) {
            return false;
        }
    }
    return true;
}

/* Defold-pinned playback ids (gui_canvas.h): accepts 0..6 or the snake_case names. */
static bool parse_playback(const char *s, int *out) {
    static const char *const names[7] = {"once_forward",  "loop_forward",  "once_backward", "loop_backward",
                                         "once_pingpong", "loop_pingpong", "none"};
    long v = 0;
    if (to_long(s, &v)) {
        if (v < 0 || v > 6) {
            return false;
        }
        *out = (int)v;
        return true;
    }
    for (int i = 0; i < 7; i++) {
        if (strcmp(s, names[i]) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

static int resolve_atlas(tp_project *p, const char *name) {
    for (int i = 0; i < p->atlas_count; i++) {
        if (p->atlases[i].name && strcmp(p->atlases[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static tp_project_anim *find_anim(tp_project_atlas *a, const char *name) {
    for (int i = 0; i < a->animation_count; i++) {
        if (a->animations[i].name && strcmp(a->animations[i].name, name) == 0) {
            return &a->animations[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* load / commit / error plumbing                                     */
/* ------------------------------------------------------------------ */

/* Maps a failed tp_project_* mutator status to an exit code + emits its structured
 * error. OOM is internal (1); every other failure is a project-state error (3). */
static int fail_status(tp_project *p, tp_status st, const char *ctx, bool json, bool quiet) {
    cli_emit_error(json, quiet, tp_status_id(st), "%s: %s", ctx, tp_status_str(st));
    tp_project_destroy(p);
    return (st == TP_STATUS_OOM) ? CLI_EXIT_INTERNAL : CLI_EXIT_PROJECT;
}

static int fail_usage(tp_project *p, bool json, bool quiet, const char *id, const char *msg) {
    cli_emit_error(json, quiet, id, "%s", msg);
    tp_project_destroy(p);
    return CLI_EXIT_USAGE;
}

/* Saves the mutated project (byte-stable writer) and emits the shared success payload.
 * `human` is the text-mode confirmation line; `count` the affected-item count. */
static int commit(tp_project *p, const char *path, const char *verb, int count, const char *human, bool json,
                  bool quiet) {
    tp_error err = {0};
    /* Writable session: assign final random IDs to any freshly created structural
     * entity before persisting (master spec §5.5). Idempotent -- already-assigned
     * IDs are preserved, so re-saving is byte-stable. */
    tp_rng rng = tp_rng_os();
    tp_status pst = tp_project_promote_ids(p, &rng, &err);
    if (pst != TP_STATUS_OK) {
        cli_emit_error(json, quiet, tp_status_id(pst), "%s", err.msg[0] ? err.msg : tp_status_str(pst));
        tp_project_destroy(p);
        /* Promote faults are internal/environmental, NOT project-content faults: OS-RNG
         * failure and the (unreachable-with-random-ids) collision sweep exhaustion, plus
         * OOM, map to exit 1 internal -- never exit 3 project. */
        return (pst == TP_STATUS_OOM || pst == TP_STATUS_RNG_FAILED || pst == TP_STATUS_ID_COLLISION_EXHAUSTED)
                   ? CLI_EXIT_INTERNAL
                   : CLI_EXIT_PROJECT;
    }
    tp_status st = tp_project_save(p, path, &err);
    if (st != TP_STATUS_OK) {
        cli_emit_error(json, quiet, tp_status_id(st), "%s", err.msg[0] ? err.msg : tp_status_str(st));
        tp_project_destroy(p);
        return (st == TP_STATUS_OOM) ? CLI_EXIT_INTERNAL : CLI_EXIT_PROJECT;
    }
    if (json) {
        cli_emit_mutation(verb, count);
    } else if (!quiet) {
        (void)printf("%s\n", human);
    }
    tp_project_destroy(p);
    return CLI_EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* new                                                                */
/* ------------------------------------------------------------------ */

static int do_new(const char *path, bool json, bool quiet) {
    /* Refuse to clobber an existing file (plan B4 (1)): a structured error, exit 3,
     * never a silent overwrite. */
    if (tp_scan_exists(path)) {
        cli_emit_error(json, quiet, "file_exists", "refusing to overwrite existing path '%s'", path);
        return CLI_EXIT_PROJECT;
    }
    tp_project *p = tp_project_create();
    if (!p) {
        cli_emit_error(json, quiet, "oom", "out of memory creating project");
        return CLI_EXIT_INTERNAL;
    }
    /* L-5: tp_project_create stays target-free; seeding is the explicit helper. */
    tp_status st = tp_project_atlas_seed_default_target(p, 0);
    if (st != TP_STATUS_OK) {
        return fail_status(p, st, "seed default target", json, quiet);
    }
    char human[CLI_PATH_MAX + 32];
    (void)snprintf(human, sizeof human, "Created project %s", path);
    return commit(p, path, "new", 1, human, json, quiet);
}

/* ------------------------------------------------------------------ */
/* add / remove source                                                */
/* ------------------------------------------------------------------ */

/* True when input abs path matches an existing source resolved to abs (cross-form
 * dedupe: existing sources are project-relative post-save, the input is CWD-absolute). */
static bool source_matches(tp_project *p, const tp_project_atlas *a, const char *in_abs, const char *in_norm,
                           int *out_index) {
    char in_canon[CLI_PATH_MAX];
    path_canon(in_abs, in_canon, sizeof in_canon);
    for (int i = 0; i < a->source_count; i++) {
        char stored[CLI_PATH_MAX];
        (void)snprintf(stored, sizeof stored, "%s", a->sources[i]);
        norm_slashes(stored);
        if (path_eq(stored, in_norm)) { /* exact stored-string match (pasted inspect path) */
            if (out_index) {
                *out_index = i;
            }
            return true;
        }
        char abs[CLI_PATH_MAX];
        if (tp_project_resolve_path(p, a->sources[i], abs, sizeof abs) == TP_STATUS_OK) {
            char canon[CLI_PATH_MAX];
            path_canon(abs, canon, sizeof canon); /* collapse project_dir + "../.." to a clean abs */
            if (path_eq(canon, in_canon)) {       /* same on-disk target */
                if (out_index) {
                    *out_index = i;
                }
                return true;
            }
        }
    }
    return false;
}

static int do_add(const char *const *pos, int npos, bool json, bool quiet) {
    if (npos < 4) {
        cli_emit_error(json, quiet, "usage", "add needs <project> <atlas> <path>... ; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[1];
    const char *atlas = pos[2];
    tp_project *p = NULL;
    int rc = cli_load_project(path, json, quiet, &p);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ai = resolve_atlas(p, atlas);
    if (ai < 0) {
        cli_emit_error(json, quiet, "atlas_not_found", "no atlas named '%s'", atlas);
        tp_project_destroy(p);
        return CLI_EXIT_PROJECT;
    }
    tp_project_atlas *a = &p->atlases[ai];
    int added = 0;
    int dup = 0;
    for (int i = 3; i < npos; i++) {
        char in_abs[CLI_PATH_MAX];
        char in_norm[CLI_PATH_MAX];
        abspath_cwd(pos[i], in_abs, sizeof in_abs);
        (void)snprintf(in_norm, sizeof in_norm, "%s", pos[i]);
        norm_slashes(in_norm);
        if (source_matches(p, a, in_abs, in_norm, NULL)) {
            dup++;
            continue;
        }
        tp_status st = tp_project_atlas_add_source(a, in_abs); /* stored abs; save relativizes */
        if (st != TP_STATUS_OK) {
            return fail_status(p, st, "add source", json, quiet);
        }
        added++;
    }
    char human[128];
    (void)snprintf(human, sizeof human, "Added %d source(s)%s to '%s'", added,
                   dup ? " (some already present)" : "", atlas);
    return commit(p, path, "add", added, human, json, quiet);
}

static int do_remove_source(const char *const *pos, int npos, bool json, bool quiet) {
    if (npos != 4) {
        cli_emit_error(json, quiet, "usage", "remove needs <project> <atlas> <source>; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[1];
    const char *atlas = pos[2];
    const char *src = pos[3];
    tp_project *p = NULL;
    int rc = cli_load_project(path, json, quiet, &p);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ai = resolve_atlas(p, atlas);
    if (ai < 0) {
        cli_emit_error(json, quiet, "atlas_not_found", "no atlas named '%s'", atlas);
        tp_project_destroy(p);
        return CLI_EXIT_PROJECT;
    }
    tp_project_atlas *a = &p->atlases[ai];
    char in_abs[CLI_PATH_MAX];
    char in_norm[CLI_PATH_MAX];
    abspath_cwd(src, in_abs, sizeof in_abs);
    (void)snprintf(in_norm, sizeof in_norm, "%s", src);
    norm_slashes(in_norm);
    int idx = -1;
    if (!source_matches(p, a, in_abs, in_norm, &idx)) {
        cli_emit_error(json, quiet, "source_not_found", "atlas '%s' has no source matching '%s'", atlas, src);
        tp_project_destroy(p);
        return CLI_EXIT_PROJECT;
    }
    tp_status st = tp_project_atlas_remove_source(a, idx);
    if (st != TP_STATUS_OK) {
        return fail_status(p, st, "remove source", json, quiet);
    }
    char human[128];
    (void)snprintf(human, sizeof human, "Removed source '%s' from '%s'", src, atlas);
    return commit(p, path, "remove", 1, human, json, quiet);
}

/* ------------------------------------------------------------------ */
/* set (atlas knobs)                                                  */
/* ------------------------------------------------------------------ */

/* Applies one atlas knob key=value. Returns 0 (ok), or CLI_EXIT_USAGE after emitting
 * a structured error. EARMARK: this key->field table + ranges duplicate the project
 * serializer keys and cli_validate's ranges; packet B's X-macro schema absorbs both. */
static int apply_knob(tp_project_atlas *a, const char *key, const char *val, bool json, bool quiet) {
    long lv = 0;
    bool bv = false;
    float fv = 0.0F;
    char m[192];

#define BADVAL(fmt, ...)                                                                                                \
    do {                                                                                                               \
        (void)snprintf(m, sizeof m, fmt, __VA_ARGS__);                                                                 \
        cli_emit_error(json, quiet, "usage", "%s", m);                                                                 \
        return CLI_EXIT_USAGE;                                                                                         \
    } while (0)

    if (strcmp(key, "max_size") == 0) {
        if (!to_long(val, &lv) || lv < 1 || lv > CLI_MAX_PAGE_DIM) {
            BADVAL("max_size = '%s' must be an integer in [1..%d]", val, CLI_MAX_PAGE_DIM);
        }
        a->max_size = (int)lv;
    } else if (strcmp(key, "padding") == 0) {
        if (!to_long(val, &lv) || lv < 0) {
            BADVAL("padding = '%s' must be an integer >= 0", val);
        }
        a->padding = (int)lv;
    } else if (strcmp(key, "margin") == 0) {
        if (!to_long(val, &lv) || lv < 0) {
            BADVAL("margin = '%s' must be an integer >= 0", val);
        }
        a->margin = (int)lv;
    } else if (strcmp(key, "extrude") == 0) {
        if (!to_long(val, &lv) || lv < 0) {
            BADVAL("extrude = '%s' must be an integer >= 0", val);
        }
        a->extrude = (int)lv;
    } else if (strcmp(key, "alpha_threshold") == 0) {
        if (!to_long(val, &lv) || lv < 0 || lv > 255) {
            BADVAL("alpha_threshold = '%s' must be an integer in [0..255]", val);
        }
        a->alpha_threshold = (int)lv;
    } else if (strcmp(key, "max_vertices") == 0) {
        if (!to_long(val, &lv) || lv < 1 || lv > 16) {
            BADVAL("max_vertices = '%s' must be an integer in [1..16]", val);
        }
        a->max_vertices = (int)lv;
    } else if (strcmp(key, "shape") == 0) {
        if (!to_long(val, &lv) || lv < 0 || lv > 2) {
            BADVAL("shape = '%s' must be 0 (rect), 1 (convex), or 2 (concave)", val);
        }
        a->shape = (int)lv;
    } else if (strcmp(key, "allow_transform") == 0) {
        if (!to_bool(val, &bv)) {
            BADVAL("allow_transform = '%s' must be 0/1/true/false", val);
        }
        a->allow_transform = bv;
    } else if (strcmp(key, "power_of_two") == 0) {
        if (!to_bool(val, &bv)) {
            BADVAL("power_of_two = '%s' must be 0/1/true/false", val);
        }
        a->power_of_two = bv;
    } else if (strcmp(key, "pixels_per_unit") == 0) {
        if (!to_float(val, &fv) || !(fv > 0.0F) || !isfinite(fv)) {
            BADVAL("pixels_per_unit = '%s' must be a positive finite number", val);
        }
        a->pixels_per_unit = fv;
    } else if (strcmp(key, "name") == 0) {
        BADVAL("%s", "use 'ntpacker atlas rename <project> <old> <new>' to rename an atlas");
    } else {
        (void)snprintf(m, sizeof m, "unknown atlas key '%s' (known: %s)", key, k_atlas_knobs);
        cli_emit_error(json, quiet, "usage", "%s", m);
        return CLI_EXIT_USAGE;
    }
#undef BADVAL
    return 0;
}

static int do_set(const char *const *pos, int npos, bool json, bool quiet) {
    if (npos < 4) {
        cli_emit_error(json, quiet, "usage", "set needs <project> <atlas> <key>=<value>...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[1];
    const char *atlas = pos[2];
    tp_project *p = NULL;
    int rc = cli_load_project(path, json, quiet, &p);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ai = resolve_atlas(p, atlas);
    if (ai < 0) {
        cli_emit_error(json, quiet, "atlas_not_found", "no atlas named '%s'", atlas);
        tp_project_destroy(p);
        return CLI_EXIT_PROJECT;
    }
    tp_project_atlas *a = &p->atlases[ai];
    int applied = 0;
    for (int i = 3; i < npos; i++) {
        char key[64];
        const char *val = split_kv(pos[i], key, sizeof key);
        if (!val) {
            char m[128];
            (void)snprintf(m, sizeof m, "expected key=value, got '%s'", pos[i]);
            return fail_usage(p, json, quiet, "usage", m);
        }
        int kr = apply_knob(a, key, val, json, quiet); /* mutates in-memory only; no save yet */
        if (kr != 0) {
            tp_project_destroy(p);
            return kr;
        }
        applied++;
    }
    char human[128];
    (void)snprintf(human, sizeof human, "Set %d knob(s) on '%s'", applied, atlas);
    return commit(p, path, "set", applied, human, json, quiet);
}

/* ------------------------------------------------------------------ */
/* sprite set / unset                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    bool set_origin, origin_inherit;
    float ox, oy;
    bool set_slice9, slice9_inherit;
    uint16_t s9[4];
    bool set_rename, rename_inherit;
    const char *rename;
    bool set_shape, shape_inherit;
    int shape;
    bool set_rot, rot_inherit;
    int rot;
    bool set_maxv, maxv_inherit;
    int maxv;
    bool set_margin, margin_inherit;
    int margin;
    bool set_extrude, extrude_inherit;
    int extrude;
} sprite_edit;

/* Parses one `field=value` (value "inherit" clears). Returns 0 or CLI_EXIT_USAGE. */
static int parse_sprite_field(sprite_edit *e, const char *tok, bool json, bool quiet) {
    char key[64];
    const char *val = split_kv(tok, key, sizeof key);
    char m[160];
    if (!val) {
        (void)snprintf(m, sizeof m, "expected field=value, got '%s'", tok);
        cli_emit_error(json, quiet, "usage", "%s", m);
        return CLI_EXIT_USAGE;
    }
    const bool inherit = (strcmp(val, "inherit") == 0);

#define BADF(fmt, ...)                                                                                                  \
    do {                                                                                                               \
        (void)snprintf(m, sizeof m, fmt, __VA_ARGS__);                                                                 \
        cli_emit_error(json, quiet, "usage", "%s", m);                                                                 \
        return CLI_EXIT_USAGE;                                                                                         \
    } while (0)

    if (strcmp(key, "origin") == 0) {
        e->set_origin = true;
        e->origin_inherit = inherit;
        if (!inherit) {
            float xy[2];
            if (!to_floats_csv(val, xy, 2)) {
                BADF("origin = '%s' must be x,y (two numbers) or 'inherit'", val);
            }
            e->ox = xy[0];
            e->oy = xy[1];
        }
    } else if (strcmp(key, "slice9") == 0) {
        e->set_slice9 = true;
        e->slice9_inherit = inherit;
        if (!inherit) {
            long v[4];
            if (!to_longs_csv(val, v, 4)) {
                BADF("slice9 = '%s' must be l,r,t,b (four integers) or 'inherit'", val);
            }
            for (int k = 0; k < 4; k++) {
                if (v[k] < 0 || v[k] > 65535) {
                    BADF("slice9 component %ld out of range [0..65535]", v[k]);
                }
                e->s9[k] = (uint16_t)v[k];
            }
        }
    } else if (strcmp(key, "rename") == 0) {
        e->set_rename = true;
        e->rename_inherit = inherit || val[0] == '\0';
        e->rename = inherit ? NULL : val;
    } else if (strcmp(key, "shape") == 0) {
        e->set_shape = true;
        e->shape_inherit = inherit;
        long v = 0;
        if (!inherit && (!to_long(val, &v) || v < 0 || v > 2)) {
            BADF("shape = '%s' must be 0/1/2 or 'inherit'", val);
        }
        e->shape = (int)v;
    } else if (strcmp(key, "allow_rotate") == 0) {
        e->set_rot = true;
        e->rot_inherit = inherit;
        long v = 0;
        if (!inherit && (!to_long(val, &v) || v < 0 || v > 1)) {
            BADF("allow_rotate = '%s' must be 0/1 or 'inherit'", val);
        }
        e->rot = (int)v;
    } else if (strcmp(key, "max_vertices") == 0) {
        e->set_maxv = true;
        e->maxv_inherit = inherit;
        long v = 0;
        if (!inherit && (!to_long(val, &v) || v < 1 || v > 16)) {
            BADF("max_vertices = '%s' must be in [1..16] or 'inherit'", val);
        }
        e->maxv = (int)v;
    } else if (strcmp(key, "margin") == 0) {
        e->set_margin = true;
        e->margin_inherit = inherit;
        long v = 0;
        if (!inherit && (!to_long(val, &v) || v < 0 || v > 32767)) {
            BADF("margin = '%s' must be in [0..32767] or 'inherit'", val);
        }
        e->margin = (int)v;
    } else if (strcmp(key, "extrude") == 0) {
        e->set_extrude = true;
        e->extrude_inherit = inherit;
        long v = 0;
        if (!inherit && (!to_long(val, &v) || v < 0 || v > 32767)) {
            BADF("extrude = '%s' must be in [0..32767] or 'inherit'", val);
        }
        e->extrude = (int)v;
    } else {
        (void)snprintf(m, sizeof m, "unknown sprite field '%s' (known: %s)", key, k_sprite_fields);
        cli_emit_error(json, quiet, "usage", "%s", m);
        return CLI_EXIT_USAGE;
    }
#undef BADF
    return 0;
}

static int do_sprite_set(const char *const *pos, int npos, bool json, bool quiet) {
    /* sprite set <project> <atlas> <key> <field>=<value>... */
    if (npos < 6) {
        cli_emit_error(json, quiet, "usage",
                       "sprite set needs <project> <atlas> <key> <field>=<value>...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[2];
    const char *atlas = pos[3];
    const char *key = pos[4];
    tp_project *p = NULL;
    int rc = cli_load_project(path, json, quiet, &p);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ai = resolve_atlas(p, atlas);
    if (ai < 0) {
        cli_emit_error(json, quiet, "atlas_not_found", "no atlas named '%s'", atlas);
        tp_project_destroy(p);
        return CLI_EXIT_PROJECT;
    }
    tp_project_atlas *a = &p->atlases[ai];

    /* Parse ALL fields first (so a bad field never leaves a half-applied entry saved). */
    sprite_edit e;
    memset(&e, 0, sizeof e);
    for (int i = 5; i < npos; i++) {
        int pr = parse_sprite_field(&e, pos[i], json, quiet);
        if (pr != 0) {
            tp_project_destroy(p);
            return pr;
        }
    }

    tp_project_sprite *s = NULL;
    tp_status st = tp_project_atlas_add_sprite(a, key, &s); /* ensure the entry */
    if (st != TP_STATUS_OK) {
        return fail_status(p, st, "sprite set", json, quiet);
    }
    if (e.set_origin) {
        s->origin_x = e.origin_inherit ? TP_PROJECT_ORIGIN_DEFAULT : e.ox;
        s->origin_y = e.origin_inherit ? TP_PROJECT_ORIGIN_DEFAULT : e.oy;
    }
    if (e.set_slice9) {
        for (int k = 0; k < 4; k++) {
            s->slice9_lrtb[k] = e.slice9_inherit ? 0 : e.s9[k];
        }
    }
    if (e.set_shape) {
        s->ov_shape = e.shape_inherit ? TP_PROJECT_OV_INHERIT : (int16_t)e.shape;
    }
    if (e.set_rot) {
        s->ov_allow_rotate = e.rot_inherit ? TP_PROJECT_OV_INHERIT : (int16_t)e.rot;
    }
    if (e.set_maxv) {
        s->ov_max_vertices = e.maxv_inherit ? TP_PROJECT_OV_INHERIT : (int16_t)e.maxv;
    }
    if (e.set_margin) {
        s->ov_margin = e.margin_inherit ? TP_PROJECT_OV_INHERIT : (int16_t)e.margin;
    }
    if (e.set_extrude) {
        s->ov_extrude = e.extrude_inherit ? TP_PROJECT_OV_INHERIT : (int16_t)e.extrude;
    }
    /* rename LAST (its mutator re-finds the entry and may prune) -- then a final prune
     * drops the entry entirely if every field ended at its default (sparse invariant). */
    if (e.set_rename) {
        st = tp_project_atlas_set_sprite_rename(a, key, e.rename_inherit ? NULL : e.rename);
        if (st != TP_STATUS_OK) {
            return fail_status(p, st, "sprite rename", json, quiet);
        }
    }
    (void)tp_project_atlas_prune_sprite(a, key);

    char human[192];
    (void)snprintf(human, sizeof human, "Set override(s) on sprite '%s' in '%s'", key, atlas);
    return commit(p, path, "sprite", 1, human, json, quiet);
}

static int do_sprite_unset(const char *const *pos, int npos, bool json, bool quiet) {
    /* sprite unset <project> <atlas> <key> */
    if (npos != 5) {
        cli_emit_error(json, quiet, "usage", "sprite unset needs <project> <atlas> <key>; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[2];
    const char *atlas = pos[3];
    const char *key = pos[4];
    tp_project *p = NULL;
    int rc = cli_load_project(path, json, quiet, &p);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ai = resolve_atlas(p, atlas);
    if (ai < 0) {
        cli_emit_error(json, quiet, "atlas_not_found", "no atlas named '%s'", atlas);
        tp_project_destroy(p);
        return CLI_EXIT_PROJECT;
    }
    tp_project_atlas *a = &p->atlases[ai];
    /* Declarative clear: an absent entry means "already unset" -> idempotent OK no-op. */
    tp_status st = tp_project_atlas_remove_sprite(a, key);
    if (st != TP_STATUS_OK && st != TP_STATUS_OUT_OF_BOUNDS) {
        return fail_status(p, st, "sprite unset", json, quiet);
    }
    char human[160];
    (void)snprintf(human, sizeof human, "Cleared overrides on sprite '%s' in '%s'", key, atlas);
    return commit(p, path, "sprite", 1, human, json, quiet);
}

/* ------------------------------------------------------------------ */
/* anim                                                               */
/* ------------------------------------------------------------------ */

/* anim list is a QUERY: {"schema":CLI_INSPECT_SCHEMA,"animations":[...]} -- its animation
 * shape mirrors inspect's, so it shares inspect's query schema (id + name split). */
static int anim_list(tp_project_atlas *a, const char *atlas_name, bool json, bool quiet) {
    (void)quiet;
    if (!json) {
        (void)printf("atlas '%s': %d animation(s)\n", atlas_name, a->animation_count);
        for (int i = 0; i < a->animation_count; i++) {
            const tp_project_anim *an = &a->animations[i];
            (void)printf("  %s: %d frame(s), fps %.9g, playback %d%s%s\n", an->name, an->frame_count, (double)an->fps,
                         an->playback, an->flip_h ? ", flip_h" : "", an->flip_v ? ", flip_v" : "");
        }
        return CLI_EXIT_OK;
    }
    cli_sb sb = {0};
    bool first = true;
    cli_sb_putc(&sb, '{');
    cli_sb_str(&sb, "\n  \"schema\": ");
    cli_sb_int(&sb, CLI_INSPECT_SCHEMA);
    cli_sb_str(&sb, ",\n  \"animations\": ");
    if (a->animation_count == 0) {
        cli_sb_str(&sb, "[]");
    } else {
        cli_sb_putc(&sb, '[');
        for (int i = 0; i < a->animation_count; i++) {
            const tp_project_anim *an = &a->animations[i];
            cli_sb_str(&sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(&sb, 2);
            cli_sb_putc(&sb, '{');
            first = true;
            cli_sb_str(&sb, "\n");
            cli_sb_indent(&sb, 3);
            char idtext[TP_ID_TEXT_CAP];
            ntpacker_fmt_shape_id(idtext, sizeof idtext, TP_ID_KIND_ANIM, an->id);
            cli_sb_str(&sb, "\"id\": "); /* structural shape-ID */
            cli_sb_json_str(&sb, idtext);
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"name\": "); /* logical/display name (name-keyed) */
            cli_sb_json_str(&sb, an->name);
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"fps\": ");
            cli_sb_num(&sb, (double)an->fps);
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"playback\": ");
            cli_sb_int(&sb, an->playback);
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"flip_h\": ");
            cli_sb_str(&sb, an->flip_h ? "true" : "false");
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"flip_v\": ");
            cli_sb_str(&sb, an->flip_v ? "true" : "false");
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"frames\": ");
            if (an->frame_count == 0) {
                cli_sb_str(&sb, "[]");
            } else {
                cli_sb_putc(&sb, '[');
                for (int f = 0; f < an->frame_count; f++) {
                    cli_sb_str(&sb, f == 0 ? "\n" : ",\n");
                    cli_sb_indent(&sb, 4);
                    cli_sb_json_str(&sb, an->frames[f]);
                }
                cli_sb_str(&sb, "\n");
                cli_sb_indent(&sb, 3);
                cli_sb_putc(&sb, ']');
            }
            (void)first;
            cli_sb_str(&sb, "\n");
            cli_sb_indent(&sb, 2);
            cli_sb_putc(&sb, '}');
        }
        cli_sb_str(&sb, "\n  ]");
    }
    cli_sb_str(&sb, "\n}");
    if (sb.oom) {
        cli_sb_free(&sb);
        cli_emit_error(true, false, "oom", "out of memory building anim list");
        return CLI_EXIT_INTERNAL;
    }
    cli_out_stdout(&sb);
    cli_sb_free(&sb);
    return CLI_EXIT_OK;
}

/* Resolves a frame selector: an all-digit token is an index; anything else is matched
 * against frame names (first match). Returns the index or -1. */
static int resolve_frame(const tp_project_anim *an, const char *sel) {
    long v = 0;
    if (to_long(sel, &v)) {
        if (v >= 0 && v < an->frame_count) {
            return (int)v;
        }
        return -1;
    }
    for (int i = 0; i < an->frame_count; i++) {
        if (an->frames[i] && strcmp(an->frames[i], sel) == 0) {
            return i;
        }
    }
    return -1;
}

static int anim_set_fields(tp_project_anim *an, const char *const *pos, int npos, int first, bool json, bool quiet,
                           tp_project *p) {
    for (int i = first; i < npos; i++) {
        char key[32];
        const char *val = split_kv(pos[i], key, sizeof key);
        char m[160];
        if (!val) {
            (void)snprintf(m, sizeof m, "expected key=value, got '%s'", pos[i]);
            return fail_usage(p, json, quiet, "usage", m);
        }
        if (strcmp(key, "fps") == 0) {
            float fv = 0.0F;
            if (!to_float(val, &fv) || !(fv > 0.0F) || !isfinite(fv)) {
                (void)snprintf(m, sizeof m, "fps = '%s' must be a positive finite number", val);
                return fail_usage(p, json, quiet, "usage", m);
            }
            an->fps = fv;
        } else if (strcmp(key, "playback") == 0) {
            int pb = 0;
            if (!parse_playback(val, &pb)) {
                (void)snprintf(m, sizeof m, "playback = '%s' must be 0..6 or a mode name", val);
                return fail_usage(p, json, quiet, "usage", m);
            }
            an->playback = pb;
        } else if (strcmp(key, "flip_h") == 0) {
            bool bv = false;
            if (!to_bool(val, &bv)) {
                (void)snprintf(m, sizeof m, "flip_h = '%s' must be 0/1/true/false", val);
                return fail_usage(p, json, quiet, "usage", m);
            }
            an->flip_h = bv;
        } else if (strcmp(key, "flip_v") == 0) {
            bool bv = false;
            if (!to_bool(val, &bv)) {
                (void)snprintf(m, sizeof m, "flip_v = '%s' must be 0/1/true/false", val);
                return fail_usage(p, json, quiet, "usage", m);
            }
            an->flip_v = bv;
        } else {
            (void)snprintf(m, sizeof m, "unknown anim key '%s' (known: fps, playback, flip_h, flip_v)", key);
            return fail_usage(p, json, quiet, "usage", m);
        }
    }
    return 0;
}

static int do_anim(const char *const *pos, int npos, const char *opt_at, bool json, bool quiet) {
    /* anim <sub> <project> <atlas> ... */
    if (npos < 4) {
        cli_emit_error(json, quiet, "usage", "anim needs <sub> <project> <atlas> ...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *sub = pos[1];
    const char *path = pos[2];
    const char *atlas = pos[3];
    if (opt_at && strcmp(sub, "add-frame") != 0) {
        cli_emit_error(json, quiet, "usage", "--at is only valid for 'anim add-frame'");
        return CLI_EXIT_USAGE;
    }
    tp_project *p = NULL;
    int rc = cli_load_project(path, json, quiet, &p);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ai = resolve_atlas(p, atlas);
    if (ai < 0) {
        cli_emit_error(json, quiet, "atlas_not_found", "no atlas named '%s'", atlas);
        tp_project_destroy(p);
        return CLI_EXIT_PROJECT;
    }
    tp_project_atlas *a = &p->atlases[ai];

    if (strcmp(sub, "list") == 0) {
        if (npos != 4) {
            tp_project_destroy(p);
            cli_emit_error(json, quiet, "usage", "anim list takes no extra arguments");
            return CLI_EXIT_USAGE;
        }
        int r = anim_list(a, atlas, json, quiet);
        tp_project_destroy(p);
        return r; /* read-only: no save */
    }

    if (strcmp(sub, "create") == 0) {
        if (npos < 5) {
            return fail_usage(p, json, quiet, "usage", "anim create needs <id> [frame-key...]");
        }
        const char *id = pos[4];
        if (find_anim(a, id)) {
            char m[128];
            (void)snprintf(m, sizeof m, "animation '%s' already exists", id);
            return fail_usage(p, json, quiet, "usage", m);
        }
        tp_project_anim *an = NULL;
        tp_status st = tp_project_atlas_add_animation(a, id, &an);
        if (st != TP_STATUS_OK) {
            return fail_status(p, st, "anim create", json, quiet);
        }
        int frames = 0;
        for (int i = 5; i < npos; i++) {
            st = tp_project_anim_add_frame(an, pos[i]);
            if (st != TP_STATUS_OK) {
                return fail_status(p, st, "anim add frame", json, quiet);
            }
            frames++;
        }
        char human[160];
        (void)snprintf(human, sizeof human, "Created animation '%s' with %d frame(s)", id, frames);
        return commit(p, path, "anim", frames, human, json, quiet);
    }

    if (strcmp(sub, "remove") == 0) {
        if (npos != 5) {
            return fail_usage(p, json, quiet, "usage", "anim remove needs <id>");
        }
        tp_status st = tp_project_atlas_remove_animation(a, pos[4]);
        if (st != TP_STATUS_OK) {
            if (st == TP_STATUS_OUT_OF_BOUNDS) {
                cli_emit_error(json, quiet, "animation_not_found", "no animation named '%s'", pos[4]);
                tp_project_destroy(p);
                return CLI_EXIT_PROJECT;
            }
            return fail_status(p, st, "anim remove", json, quiet);
        }
        char human[128];
        (void)snprintf(human, sizeof human, "Removed animation '%s'", pos[4]);
        return commit(p, path, "anim", 1, human, json, quiet);
    }

    /* All the remaining sub-verbs operate on one existing animation. */
    if (npos < 5) {
        return fail_usage(p, json, quiet, "usage", "anim needs an <id>");
    }
    const char *id = pos[4];
    tp_project_anim *an = find_anim(a, id);
    if (!an) {
        cli_emit_error(json, quiet, "animation_not_found", "no animation named '%s'", id);
        tp_project_destroy(p);
        return CLI_EXIT_PROJECT;
    }

    if (strcmp(sub, "add-frame") == 0) {
        if (npos != 6) {
            return fail_usage(p, json, quiet, "usage", "anim add-frame needs <id> <key> [--at N]");
        }
        tp_status st = tp_project_anim_add_frame(an, pos[5]); /* appends */
        if (st != TP_STATUS_OK) {
            return fail_status(p, st, "anim add-frame", json, quiet);
        }
        if (opt_at) {
            long at = 0;
            if (!to_long(opt_at, &at) || at < 0) {
                return fail_usage(p, json, quiet, "usage", "--at must be a non-negative integer");
            }
            int last = an->frame_count - 1;
            (void)tp_project_anim_move_frame(an, last, (int)at - last); /* move clamps into range */
        }
        char human[160];
        (void)snprintf(human, sizeof human, "Added frame '%s' to animation '%s'", pos[5], id);
        return commit(p, path, "anim", 1, human, json, quiet);
    }

    if (strcmp(sub, "remove-frame") == 0) {
        if (npos != 6) {
            return fail_usage(p, json, quiet, "usage", "anim remove-frame needs <id> <N|key>");
        }
        int fi = resolve_frame(an, pos[5]);
        if (fi < 0) {
            cli_emit_error(json, quiet, "frame_not_found", "animation '%s' has no frame '%s'", id, pos[5]);
            tp_project_destroy(p);
            return CLI_EXIT_PROJECT;
        }
        tp_status st = tp_project_anim_remove_frame(an, fi);
        if (st != TP_STATUS_OK) {
            return fail_status(p, st, "anim remove-frame", json, quiet);
        }
        char human[160];
        (void)snprintf(human, sizeof human, "Removed frame '%s' from animation '%s'", pos[5], id);
        return commit(p, path, "anim", 1, human, json, quiet);
    }

    if (strcmp(sub, "move-frame") == 0) {
        if (npos != 7) {
            return fail_usage(p, json, quiet, "usage", "anim move-frame needs <id> <from> <to>");
        }
        long from = 0;
        long to = 0;
        if (!to_long(pos[5], &from) || !to_long(pos[6], &to)) {
            return fail_usage(p, json, quiet, "usage", "move-frame <from> and <to> must be integers");
        }
        tp_status st = tp_project_anim_move_frame(an, (int)from, (int)(to - from));
        if (st != TP_STATUS_OK) {
            if (st == TP_STATUS_OUT_OF_BOUNDS) {
                cli_emit_error(json, quiet, "frame_not_found", "animation '%s' has no frame at index %ld", id, from);
                tp_project_destroy(p);
                return CLI_EXIT_PROJECT;
            }
            return fail_status(p, st, "anim move-frame", json, quiet);
        }
        char human[128];
        (void)snprintf(human, sizeof human, "Moved frame %ld -> %ld in animation '%s'", from, to, id);
        return commit(p, path, "anim", 1, human, json, quiet);
    }

    if (strcmp(sub, "set") == 0) {
        if (npos < 6) {
            return fail_usage(p, json, quiet, "usage", "anim set needs <id> <key>=<value>...");
        }
        int sr = anim_set_fields(an, pos, npos, 5, json, quiet, p); /* emits + destroys on failure */
        if (sr != 0) {
            return sr;
        }
        char human[128];
        (void)snprintf(human, sizeof human, "Updated animation '%s'", id);
        return commit(p, path, "anim", 1, human, json, quiet);
    }

    char m[128];
    (void)snprintf(m, sizeof m, "unknown anim sub-command '%s'", sub);
    return fail_usage(p, json, quiet, "usage", m);
}

/* ------------------------------------------------------------------ */
/* target                                                             */
/* ------------------------------------------------------------------ */

static int do_target(const char *const *pos, int npos, bool json, bool quiet) {
    /* target <sub> <project> <atlas> ... */
    if (npos < 5) {
        cli_emit_error(json, quiet, "usage", "target needs <sub> <project> <atlas> ...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *sub = pos[1];
    const char *path = pos[2];
    const char *atlas = pos[3];
    tp_project *p = NULL;
    int rc = cli_load_project(path, json, quiet, &p);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    int ai = resolve_atlas(p, atlas);
    if (ai < 0) {
        cli_emit_error(json, quiet, "atlas_not_found", "no atlas named '%s'", atlas);
        tp_project_destroy(p);
        return CLI_EXIT_PROJECT;
    }
    tp_project_atlas *a = &p->atlases[ai];

    if (strcmp(sub, "add") == 0) {
        if (npos != 6) {
            return fail_usage(p, json, quiet, "usage", "target add needs <exporter-id> <out-path>");
        }
        const char *eid = pos[4];
        const char *out = pos[5];
        if (!tp_exporter_find(eid)) { /* validate id against the live registry (no literals here) */
            char m[160];
            (void)snprintf(m, sizeof m, "unknown exporter id '%s' (see 'ntpacker version --json' exporters)", eid);
            return fail_usage(p, json, quiet, "usage", m);
        }
        tp_status st = tp_project_atlas_add_target(a, eid, out, NULL);
        if (st != TP_STATUS_OK) {
            return fail_status(p, st, "target add", json, quiet);
        }
        char human[192];
        (void)snprintf(human, sizeof human, "Added target %s -> %s on '%s'", eid, out, atlas);
        return commit(p, path, "target", 1, human, json, quiet);
    }

    if (strcmp(sub, "remove") == 0) {
        if (npos != 5) {
            return fail_usage(p, json, quiet, "usage", "target remove needs <index-or-id>");
        }
        const char *sel = pos[4];
        long idx = 0;
        int ti = -1;
        if (to_long(sel, &idx)) {
            if (idx >= 0 && idx < a->target_count) {
                ti = (int)idx;
            }
        } else {
            for (int i = 0; i < a->target_count; i++) {
                if (a->targets[i].exporter_id && strcmp(a->targets[i].exporter_id, sel) == 0) {
                    ti = i;
                    break;
                }
            }
        }
        if (ti < 0) {
            cli_emit_error(json, quiet, "target_not_found", "atlas '%s' has no target '%s'", atlas, sel);
            tp_project_destroy(p);
            return CLI_EXIT_PROJECT;
        }
        tp_status st = tp_project_atlas_remove_target(a, ti);
        if (st != TP_STATUS_OK) {
            return fail_status(p, st, "target remove", json, quiet);
        }
        char human[160];
        (void)snprintf(human, sizeof human, "Removed target '%s' from '%s'", sel, atlas);
        return commit(p, path, "target", 1, human, json, quiet);
    }

    if (strcmp(sub, "set") == 0) {
        if (npos < 6) {
            return fail_usage(p, json, quiet, "usage",
                              "target set needs <index> [exporter=..] [out=..] [enabled=0|1]");
        }
        long idx = 0;
        if (!to_long(pos[4], &idx) || idx < 0 || idx >= a->target_count) {
            cli_emit_error(json, quiet, "target_not_found", "atlas '%s' has no target at index '%s'", atlas, pos[4]);
            tp_project_destroy(p);
            return CLI_EXIT_PROJECT;
        }
        tp_project_target *t = &a->targets[idx];
        /* Merge onto the current fields (set_target replaces all three atomically). */
        char eid[128];
        char out[CLI_PATH_MAX];
        (void)snprintf(eid, sizeof eid, "%s", t->exporter_id ? t->exporter_id : "");
        (void)snprintf(out, sizeof out, "%s", t->out_path ? t->out_path : "");
        bool enabled = t->enabled;
        for (int i = 5; i < npos; i++) {
            char key[32];
            const char *val = split_kv(pos[i], key, sizeof key);
            char m[192];
            if (!val) {
                (void)snprintf(m, sizeof m, "expected key=value, got '%s'", pos[i]);
                return fail_usage(p, json, quiet, "usage", m);
            }
            if (strcmp(key, "exporter") == 0) {
                if (!tp_exporter_find(val)) {
                    (void)snprintf(m, sizeof m, "unknown exporter id '%s'", val);
                    return fail_usage(p, json, quiet, "usage", m);
                }
                (void)snprintf(eid, sizeof eid, "%s", val);
            } else if (strcmp(key, "out") == 0) {
                if (val[0] == '\0') {
                    return fail_usage(p, json, quiet, "usage", "out must be non-empty");
                }
                (void)snprintf(out, sizeof out, "%s", val);
            } else if (strcmp(key, "enabled") == 0) {
                if (!to_bool(val, &enabled)) {
                    (void)snprintf(m, sizeof m, "enabled = '%s' must be 0/1/true/false", val);
                    return fail_usage(p, json, quiet, "usage", m);
                }
            } else {
                (void)snprintf(m, sizeof m, "unknown target key '%s' (known: exporter, out, enabled)", key);
                return fail_usage(p, json, quiet, "usage", m);
            }
        }
        tp_status st = tp_project_atlas_set_target(a, (int)idx, eid, out, enabled);
        if (st != TP_STATUS_OK) {
            return fail_status(p, st, "target set", json, quiet);
        }
        char human[192];
        (void)snprintf(human, sizeof human, "Updated target %ld on '%s'", idx, atlas);
        return commit(p, path, "target", 1, human, json, quiet);
    }

    char m[128];
    (void)snprintf(m, sizeof m, "unknown target sub-command '%s'", sub);
    return fail_usage(p, json, quiet, "usage", m);
}

/* ------------------------------------------------------------------ */
/* atlas                                                              */
/* ------------------------------------------------------------------ */

static int do_atlas(const char *const *pos, int npos, bool json, bool quiet) {
    /* atlas <sub> <project> ... (operates at project level, atlases keyed by name) */
    if (npos < 4) {
        cli_emit_error(json, quiet, "usage", "atlas needs <sub> <project> <name>...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *sub = pos[1];
    const char *path = pos[2];
    tp_project *p = NULL;
    int rc = cli_load_project(path, json, quiet, &p);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    if (strcmp(sub, "add") == 0) {
        if (npos != 4) {
            return fail_usage(p, json, quiet, "usage", "atlas add needs <name>");
        }
        const char *name = pos[3];
        if (resolve_atlas(p, name) >= 0) { /* CLI policy: keep name-based selection unambiguous */
            char m[128];
            (void)snprintf(m, sizeof m, "atlas '%s' already exists", name);
            return fail_usage(p, json, quiet, "usage", m);
        }
        tp_status st = tp_project_add_atlas(p, name, NULL);
        if (st != TP_STATUS_OK) {
            return fail_status(p, st, "atlas add", json, quiet);
        }
        char human[128];
        (void)snprintf(human, sizeof human, "Added atlas '%s'", name);
        return commit(p, path, "atlas", 1, human, json, quiet);
    }

    if (strcmp(sub, "remove") == 0) {
        if (npos != 4) {
            return fail_usage(p, json, quiet, "usage", "atlas remove needs <name>");
        }
        int ai = resolve_atlas(p, pos[3]);
        if (ai < 0) {
            cli_emit_error(json, quiet, "atlas_not_found", "no atlas named '%s'", pos[3]);
            tp_project_destroy(p);
            return CLI_EXIT_PROJECT;
        }
        tp_status st = tp_project_remove_atlas(p, ai);
        if (st != TP_STATUS_OK) {
            return fail_status(p, st, "atlas remove", json, quiet);
        }
        char human[128];
        (void)snprintf(human, sizeof human, "Removed atlas '%s'", pos[3]);
        return commit(p, path, "atlas", 1, human, json, quiet);
    }

    if (strcmp(sub, "rename") == 0) {
        if (npos != 5) {
            return fail_usage(p, json, quiet, "usage", "atlas rename needs <old> <new>");
        }
        const char *old = pos[3];
        const char *neu = pos[4];
        int ai = resolve_atlas(p, old);
        if (ai < 0) {
            cli_emit_error(json, quiet, "atlas_not_found", "no atlas named '%s'", old);
            tp_project_destroy(p);
            return CLI_EXIT_PROJECT;
        }
        int other = resolve_atlas(p, neu);
        if (other >= 0 && other != ai) {
            char m[128];
            (void)snprintf(m, sizeof m, "atlas '%s' already exists", neu);
            return fail_usage(p, json, quiet, "usage", m);
        }
        tp_status st = tp_project_set_atlas_name(&p->atlases[ai], neu); /* B4 trivial mutator */
        if (st != TP_STATUS_OK) {
            return fail_status(p, st, "atlas rename", json, quiet);
        }
        char human[160];
        (void)snprintf(human, sizeof human, "Renamed atlas '%s' -> '%s'", old, neu);
        return commit(p, path, "atlas", 1, human, json, quiet);
    }

    char m[128];
    (void)snprintf(m, sizeof m, "unknown atlas sub-command '%s'", sub);
    return fail_usage(p, json, quiet, "usage", m);
}

/* ------------------------------------------------------------------ */
/* dispatch                                                           */
/* ------------------------------------------------------------------ */

int cmd_mutate(int npos, const char *const *positionals, const char *opt_at, bool json, bool quiet) {
    const char *verb = positionals[0];

    if (opt_at && strcmp(verb, "anim") != 0) {
        cli_emit_error(json, quiet, "usage", "--at is only valid for 'anim add-frame'");
        return CLI_EXIT_USAGE;
    }

    if (strcmp(verb, "new") == 0) {
        if (npos != 2) {
            cli_emit_error(json, quiet, "usage", "new needs exactly one <path>; try 'ntpacker help'");
            return CLI_EXIT_USAGE;
        }
        return do_new(positionals[1], json, quiet);
    }
    if (strcmp(verb, "add") == 0) {
        return do_add(positionals, npos, json, quiet);
    }
    if (strcmp(verb, "remove") == 0) {
        return do_remove_source(positionals, npos, json, quiet);
    }
    if (strcmp(verb, "set") == 0) {
        return do_set(positionals, npos, json, quiet);
    }
    if (strcmp(verb, "sprite") == 0) {
        if (npos < 2) {
            cli_emit_error(json, quiet, "usage", "sprite needs 'set' or 'unset'");
            return CLI_EXIT_USAGE;
        }
        if (strcmp(positionals[1], "set") == 0) {
            return do_sprite_set(positionals, npos, json, quiet);
        }
        if (strcmp(positionals[1], "unset") == 0) {
            return do_sprite_unset(positionals, npos, json, quiet);
        }
        cli_emit_error(json, quiet, "usage", "unknown sprite sub-command '%s' (want set/unset)", positionals[1]);
        return CLI_EXIT_USAGE;
    }
    if (strcmp(verb, "anim") == 0) {
        return do_anim(positionals, npos, opt_at, json, quiet);
    }
    if (strcmp(verb, "target") == 0) {
        return do_target(positionals, npos, json, quiet);
    }
    if (strcmp(verb, "atlas") == 0) {
        return do_atlas(positionals, npos, json, quiet);
    }

    cli_emit_error(json, quiet, "usage", "unknown command '%s'; try 'ntpacker help'", verb);
    return CLI_EXIT_USAGE;
}
