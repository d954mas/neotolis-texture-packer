/* tp_demo_export -- dev driver + smoke tool (NOT a ctest).
 *
 * Loads a `.ntpacker_project` and runs EVERY enabled target of EVERY atlas,
 * gathering each atlas's sprites by scanning its declared `sources` (a directory
 * recursively for *.png, or a single image file). Outputs are written to each
 * target's project-relative out_path, resolved against the project dir -- exactly
 * the path tp_export_run drives for the CLI/GUI. Used to (re)generate the Defold
 * demo's ntpacker exports (the co-located _nt files under examples/defold-demo)
 * deterministically; also a handy end-to-end smoke of load -> scan -> pack -> export.
 *
 * Usage: tp_demo_export <path/to.ntpacker_project> [work_dir]
 *   work_dir (default "<project_dir>/out") only holds the transient session
 *   .ntpack files (gitignored); target outputs land at the project out_paths.
 */

#define _CRT_SECURE_NO_WARNINGS

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define TP_MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#define TP_MKDIR(p) mkdir((p), 0777)
#endif

typedef struct {
    char name[512]; /* atlas-relative sprite name, '/'-separated, no extension */
    char path[1024];
} scan_entry;

typedef struct {
    scan_entry *items;
    int count;
    int cap;
} scan_list;

static bool ends_with_png(const char *s) {
    size_t n = strlen(s);
    if (n < 4) {
        return false;
    }
    const char *e = s + (n - 4);
    return (e[0] == '.') && (e[1] == 'p' || e[1] == 'P') && (e[2] == 'n' || e[2] == 'N') && (e[3] == 'g' || e[3] == 'G');
}

static bool list_push(scan_list *l, const char *name, const char *path) {
    if (l->count == l->cap) {
        int nc = (l->cap == 0) ? 32 : l->cap * 2;
        scan_entry *ni = (scan_entry *)realloc(l->items, (size_t)nc * sizeof(scan_entry));
        if (!ni) {
            return false;
        }
        l->items = ni;
        l->cap = nc;
    }
    scan_entry *e = &l->items[l->count++];
    (void)snprintf(e->name, sizeof e->name, "%s", name);
    (void)snprintf(e->path, sizeof e->path, "%s", path);
    return true;
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const scan_entry *)a)->name, ((const scan_entry *)b)->name);
}

static bool is_dir(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static void mkdir_p(const char *path) {
    char tmp[1024];
    (void)snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) {
        return;
    }
    if (tmp[len - 1] == '/' || tmp[len - 1] == '\\') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            (void)TP_MKDIR(tmp);
            *p = c;
        }
    }
    (void)TP_MKDIR(tmp);
}

/* Strip a trailing ".png" from `rel` into `out`. */
static void strip_png(const char *rel, char *out, size_t cap) {
    size_t n = strlen(rel);
    if (n >= 4) {
        n -= 4; /* drop ".png" */
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, rel, n);
    out[n] = '\0';
}

/* Recursively collect *.png under `root_abs`; sprite name = `prefix` + relative
 * path (minus extension). */
static bool scan_dir(const char *root_abs, const char *prefix, scan_list *out) {
#ifdef _WIN32
    char pattern[1024];
    (void)snprintf(pattern, sizeof pattern, "%s/*", root_abs);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return true; /* empty/inaccessible -> no sprites */
    }
    bool ok = true;
    do {
        const char *nm = fd.cFileName;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) {
            continue;
        }
        char full[1024];
        (void)snprintf(full, sizeof full, "%s/%s", root_abs, nm);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char sub[512];
            (void)snprintf(sub, sizeof sub, "%s%s/", prefix, nm);
            ok = scan_dir(full, sub, out);
        } else if (ends_with_png(nm)) {
            char rel[512];
            (void)snprintf(rel, sizeof rel, "%s%s", prefix, nm);
            char name[512];
            strip_png(rel, name, sizeof name);
            ok = list_push(out, name, full);
        }
    } while (ok && FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
#else
    DIR *d = opendir(root_abs);
    if (!d) {
        return true;
    }
    bool ok = true;
    struct dirent *de;
    while (ok && (de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) {
            continue;
        }
        char full[1024];
        (void)snprintf(full, sizeof full, "%s/%s", root_abs, nm);
        if (is_dir(full)) {
            char sub[512];
            (void)snprintf(sub, sizeof sub, "%s%s/", prefix, nm);
            ok = scan_dir(full, sub, out);
        } else if (ends_with_png(nm)) {
            char rel[512];
            (void)snprintf(rel, sizeof rel, "%s%s", prefix, nm);
            char name[512];
            strip_png(rel, name, sizeof name);
            ok = list_push(out, name, full);
        }
    }
    closedir(d);
    return ok;
#endif
}

/* Basename (no dir, no extension) of a file path into `out`. */
static void basename_noext(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *base = path;
    if (slash && slash + 1 > base) {
        base = slash + 1;
    }
    if (bslash && bslash + 1 > base) {
        base = bslash + 1;
    }
    char tmp[512];
    (void)snprintf(tmp, sizeof tmp, "%s", base);
    if (ends_with_png(tmp)) {
        tmp[strlen(tmp) - 4] = '\0';
    } else {
        char *dot = strrchr(tmp, '.');
        if (dot) {
            *dot = '\0';
        }
    }
    (void)snprintf(out, cap, "%s", tmp);
}

/* Gather all sprites for one atlas from its sources. */
static bool gather_atlas_sprites(const tp_project *proj, const tp_project_atlas *a, scan_list *out) {
    for (int s = 0; s < a->source_count; s++) {
        char abs[1024];
        if (tp_project_resolve_path(proj, a->sources[s], abs, sizeof abs) != TP_STATUS_OK) {
            (void)fprintf(stderr, "  cannot resolve source '%s'\n", a->sources[s]);
            return false;
        }
        if (is_dir(abs)) {
            if (!scan_dir(abs, "", out)) {
                return false;
            }
        } else {
            char name[512];
            basename_noext(abs, name, sizeof name);
            if (!list_push(out, name, abs)) {
                return false;
            }
        }
    }
    qsort(out->items, (size_t)out->count, sizeof(scan_entry), entry_cmp);
    return true;
}

static int run_atlas(tp_project *proj, int idx, const char *work_dir) {
    const tp_project_atlas *a = &proj->atlases[idx];
    scan_list sl = {0};
    if (!gather_atlas_sprites(proj, a, &sl)) {
        free(sl.items);
        return 1;
    }
    if (sl.count == 0) {
        (void)fprintf(stderr, "atlas '%s': no source sprites found\n", a->name ? a->name : "?");
        free(sl.items);
        return 1;
    }

    /* decode PNGs */
    tp_pack_sprite_desc *descs = (tp_pack_sprite_desc *)calloc((size_t)sl.count, sizeof(tp_pack_sprite_desc));
    uint8_t **pixels = (uint8_t **)calloc((size_t)sl.count, sizeof(uint8_t *));
    if (!descs || !pixels) {
        free(descs);
        free(pixels);
        free(sl.items);
        return 1;
    }
    int rc = 0;
    for (int i = 0; i < sl.count; i++) {
        int w = 0, h = 0, ch = 0;
        pixels[i] = stbi_load(sl.items[i].path, &w, &h, &ch, 4);
        if (!pixels[i]) {
            (void)fprintf(stderr, "atlas '%s': cannot decode %s\n", a->name, sl.items[i].path);
            rc = 1;
            break;
        }
        descs[i].name = sl.items[i].name;
        descs[i].rgba = pixels[i];
        descs[i].w = w;
        descs[i].h = h;
        descs[i].origin_x = 0.5F;
        descs[i].origin_y = 0.5F;
    }

    if (rc == 0) {
        tp_arena *ar = tp_arena_create(0);
        tp_export_notices notices;
        tp_export_notices_init(&notices);
        int pack_runs = 0;
        tp_error e = {{0}};
        tp_status st = tp_export_run(proj, idx, descs, sl.count, work_dir, ar, &notices, &pack_runs, &e);
        if (st != TP_STATUS_OK) {
            (void)fprintf(stderr, "atlas '%s': export failed: %s (%s)\n", a->name, tp_status_str(st), e.msg);
            rc = 1;
        } else {
            (void)printf("atlas '%s': %d sprite(s), %d target(s), %d pack run(s)\n", a->name, sl.count,
                         a->target_count, pack_runs);
            for (int n = 0; n < notices.count; n++) {
                (void)printf("    notice: %s\n", notices.items[n].msg);
            }
        }
        tp_export_notices_free(&notices);
        tp_arena_destroy(ar);
    }

    for (int i = 0; i < sl.count; i++) {
        if (pixels[i]) {
            stbi_image_free(pixels[i]);
        }
    }
    free(descs);
    free(pixels);
    free(sl.items);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        (void)fprintf(stderr, "usage: %s <project.ntpacker_project> [work_dir]\n", argv[0]);
        return 2;
    }
    const char *proj_path = argv[1];

    tp_project *proj = NULL;
    tp_error e = {{0}};
    if (tp_project_load(proj_path, &proj, &e) != TP_STATUS_OK) {
        (void)fprintf(stderr, "cannot load project '%s': %s\n", proj_path, e.msg);
        return 1;
    }

    /* work_dir holds only transient session .ntpack files (gitignored); default
     * to the current directory so we never resurrect an output tree. */
    char work_dir[1024];
    (void)snprintf(work_dir, sizeof work_dir, "%s", (argc > 2) ? argv[2] : ".");
    mkdir_p(work_dir);

    /* ensure every target's parent directory exists. */
    for (int i = 0; i < proj->atlas_count; i++) {
        const tp_project_atlas *a = &proj->atlases[i];
        for (int t = 0; t < a->target_count; t++) {
            char abs[1024];
            if (tp_project_resolve_path(proj, a->targets[t].out_path, abs, sizeof abs) != TP_STATUS_OK) {
                continue;
            }
            char *slash = strrchr(abs, '/');
            if (slash) {
                *slash = '\0';
                mkdir_p(abs);
            }
        }
    }

    int rc = 0;
    for (int i = 0; i < proj->atlas_count; i++) {
        rc |= run_atlas(proj, i, work_dir);
    }

    tp_project_destroy(proj);
    if (rc == 0) {
        (void)printf("tp_demo_export: OK\n");
    }
    return rc;
}
