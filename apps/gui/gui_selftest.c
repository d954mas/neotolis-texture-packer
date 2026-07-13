/* ntpacker-gui dev seam: the headless self-test + auto-quit render/verify phase driver.
 * The whole TU compiles to nothing unless NTPACKER_GUI_SELFTEST is defined (a placeholder typedef
 * keeps it a legal ISO C translation unit). Moved verbatim out of main.c (GUI decomposition step 3);
 * only run_selftest/selftest_pre_frame/selftest_post_draw gained external linkage (the header hooks).
 * See gui_selftest.h. */

#include "gui_selftest.h"

#ifdef NTPACKER_GUI_SELFTEST

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "app/nt_app.h"         /* nt_app_quit */
#include "core/nt_assert.h"     /* NT_ASSERT */
#include "graphics/nt_gfx.h"    /* nt_gfx_read_pixels (overlay pixel probe) */
#include "log/nt_log.h"         /* nt_log_info (SELFTEST-* logging) */
#include "ui/nt_ui.h"           /* nt_ui_get_bbox / nt_ui_id / nt_ui_bbox_t */
#include "window/nt_window.h"   /* g_nt_window (phase-driven framebuffer dims) */

#include "tp_core/tp_error.h"   /* tp_status_str / tp_error */
#include "tp_core/tp_export.h"  /* tp_exporter_count/at (preview-target selector index) */
#include "tp_core/tp_id.h"      /* tp_id128_eq (F2-05b-ii-B append-fail identity check) */
#include "tp_core/tp_model.h"   /* tp_result */
#include "tp_core/tp_names.h"   /* tp_sprite_export_key (region -> override key) */
#include "tp_core/tp_project.h" /* tp_project* accessors */
#include "tp_core/tp_transaction.h" /* tp_semantic_identity (F2-05b-ii-B append-fail identity check) */
#include "tp_journal_internal.h"    /* F2-05b-ii-B memory-io fault seams (append-fail injection); from packer/src */

#include "gui_actions.h"  /* do_pack_blocking / reset_selection / preview_stop / anim ops + gui_request_gesture_commit */
#include "gui_canvas.h"   /* s_canvas ops + GUI_CANVAS_ATLAS */
#include "gui_pack.h"     /* gui_pack_* + GUI_PACK_ASYNC_* */
#include "gui_project.h"  /* gui_project_* + GUI_SPRITE_OV_SHAPE / GUI_ADD_DUPLICATE */
#include "gui_rows.h"     /* build_rows / multi_sel_* / select_row_for_region */
#include "gui_scan.h"     /* gui_scan_* */
#include "gui_shell.h"    /* UI_STATE_SLOTS / UI_STATE_PROBE_MAX / UI_ROW_ID_RING */
#include "gui_state.h"    /* s_canvas / s_sel_* / s_sec_* / s_about_open / s_export_open / s_ctx / s_id_* */

static void to_abs(const char *rel, char *out, size_t cap) {
#ifdef _WIN32
    if (GetFullPathNameA(rel, (DWORD)cap, out, NULL) == 0) {
        (void)snprintf(out, cap, "%s", rel);
    }
    normalize_slashes(out);
#else
    (void)snprintf(out, cap, "%s", rel);
#endif
}

/* Writes a tiny valid 2x2 32-bit uncompressed TGA (stb decodes it) -- cheap procedural sprite. */
static void write_tga_2x2(const char *path) {
    const unsigned char hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x28};
    unsigned char px[2 * 2 * 4];
    for (int i = 0; i < 4; i++) {
        px[i * 4 + 0] = 200; /* B */
        px[i * 4 + 1] = 180; /* G */
        px[i * 4 + 2] = 160; /* R */
        px[i * 4 + 3] = 255; /* A */
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        (void)fwrite(hdr, 1, sizeof hdr, f);
        (void)fwrite(px, 1, sizeof px, f);
        (void)fclose(f);
    }
}

/* Reads a whole file into a malloc'd NUL-terminated buffer (caller frees; NULL on miss). */
static char *selftest_slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = (sz >= 0) ? (char *)malloc((size_t)sz + 1) : NULL;
    size_t rd = buf ? fread(buf, 1, (size_t)sz, f) : 0;
    (void)fclose(f);
    if (!buf) {
        return NULL;
    }
    buf[rd] = '\0';
    return buf;
}

/* UTF-8 "тест_спрайт" (a Cyrillic sprite name) -- exercises multi-byte names end-to-end. */
#define CYR_STEM "\xD1\x82\xD0\xB5\xD1\x81\xD1\x82_\xD1\x81\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82"

void run_selftest(void) {
    nt_log_info("SELFTEST: begin");
    gui_project_init();
    tp_project *p = gui_project_get();
    NT_ASSERT(p && p->atlas_count == 1);
    (void)p;

    /* Absolute paths (from cwd=workspace) so they survive relativize-on-save + resolve-on-load. */
    char folder[512];
    char file[512];
    to_abs("examples/defold-demo/examples/anim_trim/anims", folder, sizeof folder);
    to_abs("examples/defold-demo/examples/anim_trim/anims/sq1.png", file, sizeof file);

    const gui_add_status a1 = gui_project_add_source(0, folder);
    nt_log_info("SELFTEST: add folder -> %d (dirty=%d stale=%d)", (int)a1, gui_project_is_dirty(), gui_project_is_stale());
    const gui_add_status a2 = gui_project_add_source(0, file);
    nt_log_info("SELFTEST: add file -> %d", (int)a2);
    const gui_add_status a3 = gui_project_add_source(0, folder); /* dedupe (F6c): expect DUPLICATE(2) */
    nt_log_info("SELFTEST: dedupe add folder again -> %d (expect %d)", (int)a3, (int)GUI_ADD_DUPLICATE);

    char err[256] = {0};
    const bool dec = gui_canvas_set_image(&s_canvas, file, err, sizeof err);
    nt_log_info("SELFTEST: decode+upload -> %d (%dx%d) %s", dec, gui_canvas_img_w(&s_canvas), gui_canvas_img_h(&s_canvas), dec ? "" : err);

    char save_path[1200];
    (void)snprintf(save_path, sizeof save_path, "%s/selftest.ntpacker_project", s_exe_dir);
    tp_status st = gui_project_save_as(save_path, err, sizeof err);
    nt_log_info("SELFTEST: save '%s' -> %s (dirty=%d)", save_path, tp_status_str(st), gui_project_is_dirty());

    st = gui_project_open(save_path, err, sizeof err);
    const int nsrc = gui_project_get() ? gui_project_get()->atlases[0].source_count : -1;
    nt_log_info("SELFTEST: reload -> %s, atlas0 sources=%d (dirty=%d)", tp_status_str(st), nsrc, gui_project_is_dirty());

    /* --- rename atlas + undo/redo THROUGH THE F2-03 DIFF HISTORY (b-ii-A): the model swaps its
     *     project on undo/redo; verify the name reverts/replays exactly and identity-dirty tracks
     *     (undo back to the saved baseline reads CLEAN even though the revision is higher). --- */
    char name0[64];
    (void)snprintf(name0, sizeof name0, "%s", gui_project_get()->atlases[0].name);
    NT_ASSERT(!gui_project_is_dirty() && "reloaded project is clean at its saved baseline");
    gui_project_set_atlas_name(0, "hero_atlas"); /* structural: commits immediately -> one history step */
    nt_log_info("SELFTEST: rename atlas '%s' -> '%s' (dirty=%d undo_depth=%d)", name0,
                gui_project_get()->atlases[0].name, gui_project_is_dirty(), gui_project_undo_depth());
    NT_ASSERT(gui_project_is_dirty() && strcmp(gui_project_get()->atlases[0].name, "hero_atlas") == 0 &&
              "rename dirties + applies");
    const bool undone = gui_project_undo();
    nt_log_info("SELFTEST: undo -> %d name='%s' (dirty=%d) [expect name reverted, dirty=0]", undone,
                gui_project_get()->atlases[0].name, gui_project_is_dirty());
    NT_ASSERT(undone && strcmp(gui_project_get()->atlases[0].name, name0) == 0 && !gui_project_is_dirty() &&
              "undo through F2-03 history restores the pre-rename name AND reads clean at the saved baseline");
    const bool redone = gui_project_redo();
    nt_log_info("SELFTEST: redo -> %d name='%s' (dirty=%d)", redone, gui_project_get()->atlases[0].name,
                gui_project_is_dirty());
    NT_ASSERT(redone && strcmp(gui_project_get()->atlases[0].name, "hero_atlas") == 0 && gui_project_is_dirty() &&
              "redo re-applies the rename + re-dirties");

    /* --- rename a region (sprite override), verify it is stored on the model --- */
    char folder_abs[512];
    if (tp_project_resolve_path(gui_project_get(), gui_project_get()->atlases[0].sources[0].path, folder_abs, sizeof folder_abs) == TP_STATUS_OK) {
        const gui_scan_result *sc = gui_scan_get(folder_abs);
        nt_log_info("SELFTEST: folder scan found %d image(s)", sc->count);
        if (sc->count > 0) {
            char sprite[192];
            (void)snprintf(sprite, sizeof sprite, "%s", sc->entries[0].rel);
            char *dot = strrchr(sprite, '.');
            if (dot) {
                *dot = '\0';
            }
            gui_project_set_sprite_rename(0, sprite, "renamed_region");
            tp_project_atlas *a0 = tp_project_get_atlas(gui_project_get(), 0);
            const tp_project_sprite *ov = tp_project_atlas_find_sprite(a0, sprite);
            nt_log_info("SELFTEST: rename region '%s' -> override='%s'", sprite, (ov && ov->rename) ? ov->rename : "(none)");
        }
    }

    /* --- save_buffer / load_buffer round-trip in-app --- */
    char *bb = NULL;
    size_t bl = 0;
    tp_error be = {0};
    const tp_status bst = tp_project_save_buffer(gui_project_get(), &bb, &bl, &be);
    tp_project *lp = NULL;
    tp_error le = {0};
    const tp_status lst = (bst == TP_STATUS_OK) ? tp_project_load_buffer(bb, bl, &lp, &le) : bst;
    nt_log_info("SELFTEST: save_buffer(%zuB)->%s; load_buffer->%s atlas0='%s'", bl, tp_status_str(bst), tp_status_str(lst),
                (lp && lp->atlas_count > 0) ? lp->atlases[0].name : "(none)");
    tp_project_destroy(lp);
    free(bb);

    /* --- refresh cycle: create + delete a temp png, observe the scan change --- */
    char rdir[600];
    char rfile[700];
    (void)snprintf(rdir, sizeof rdir, "%s/selftest_refresh", s_exe_dir);
#ifdef _WIN32
    (void)CreateDirectoryA(rdir, NULL);
#endif
    (void)snprintf(rfile, sizeof rfile, "%s/temp.png", rdir);
    FILE *tf = fopen(rfile, "wb");
    if (tf) {
        (void)fputs("PNGDATA", tf);
        (void)fclose(tf);
    }
    gui_scan_invalidate_all();
    const int before_n = gui_scan_get(rdir)->count;
    (void)remove(rfile);
    gui_scan_invalidate_all();
    const int after_n = gui_scan_get(rdir)->count;
    nt_log_info("SELFTEST: refresh cycle temp png before=%d after=%d (removed=%d)", before_n, after_n, before_n - after_n);
#ifdef _WIN32
    (void)RemoveDirectoryA(rdir);
#endif

    /* --- in-process pack of the demo atlases: real tp_pack via gui_pack (timing + assertions) --- */
    {
        char proj[600];
        to_abs("examples/defold-demo/defold-demo.ntpacker_project", proj, sizeof proj);
        char perr[256] = {0};
        if (gui_project_open(proj, perr, sizeof perr) == TP_STATUS_OK) {
            tp_project *dp = gui_project_get();
            int i_rotate = -1;
            int i_basic = -1;
            for (int i = 0; i < dp->atlas_count; i++) {
                if (strcmp(dp->atlases[i].name, "rotate") == 0) {
                    i_rotate = i;
                } else if (strcmp(dp->atlases[i].name, "basic") == 0) {
                    i_basic = i;
                }
            }
            gui_scan_invalidate_all();
            double ms_r = 0.0;
            double ms_b = 0.0;
            char pe[256] = {0};
            char note[128] = {0};
            const bool okr = (i_rotate >= 0) && gui_pack_atlas(i_rotate, &ms_r, pe, sizeof pe, note, sizeof note);
            const tp_result *rr = gui_pack_result(i_rotate);
            nt_log_info("SELFTEST: pack 'rotate' -> %d in %.1f ms sprites=%d pages=%d (find 'a'=%d) %s", okr, ms_r,
                        rr ? rr->sprite_count : -1, rr ? rr->page_count : -1, gui_pack_find_sprite(i_rotate, "a"),
                        okr ? "" : pe);
            NT_ASSERT(okr && rr && rr->sprite_count == 3 && rr->page_count >= 1 && "pack rotate");
            NT_ASSERT(gui_pack_find_sprite(i_rotate, "a") >= 0 && "region lookup 'a'");
            char pe2[256] = {0};
            const bool okb = (i_basic >= 0) && gui_pack_atlas(i_basic, &ms_b, pe2, sizeof pe2, note, sizeof note);
            const tp_result *rb = gui_pack_result(i_basic);
            nt_log_info("SELFTEST: pack 'basic' -> %d in %.1f ms sprites=%d pages=%d %s", okb, ms_b,
                        rb ? rb->sprite_count : -1, rb ? rb->page_count : -1, okb ? "" : pe2);

            /* export 'rotate' via gui_pack_export, ISOLATED to a throwaway base under the build dir so
             * the demo's committed exports (owned by another agent) are never touched: disable the
             * atlas's other targets, point json-neotolis at the temp base, then assert the files exist.
             * tp_export_run uses the target out_path as the exporter BASE and appends .json / -N.png. */
            tp_project_atlas *rot_a = tp_project_get_atlas(dp, i_rotate);
            int jtarget = -1;
            const int rtc = rot_a ? rot_a->target_count : 0;
            for (int k = 0; k < rtc; k++) {
                /* F2-05b-i: gui_project_set_target now clone-swaps the model, freeing the old
                 * project -- re-fetch the atlas each iteration (dp/rot_a would dangle). */
                tp_project_atlas *ra = tp_project_get_atlas(gui_project_get(), i_rotate);
                if (!ra) {
                    break;
                }
                if (strcmp(ra->targets[k].exporter_id, "json-neotolis") == 0) {
                    jtarget = k;
                } else {
                    gui_project_set_target(i_rotate, k, ra->targets[k].exporter_id, ra->targets[k].out_path, false);
                }
            }
            char tbase[700] = {0};
            (void)snprintf(tbase, sizeof tbase, "%s/selftest_rotate_export", s_exe_dir);
            if (jtarget >= 0) {
                gui_project_set_target(i_rotate, jtarget, "json-neotolis", tbase, true);
            }
            int etg = 0;
            int enc = 0;
            char eerr[256] = {0};
            char enote[128] = {0};
            const bool oke = (i_rotate >= 0 && jtarget >= 0) &&
                             gui_pack_export(i_rotate, &etg, &enc, eerr, sizeof eerr, enote, sizeof enote);
            char jpath[720] = {0};
            char ppath[720] = {0};
            (void)snprintf(jpath, sizeof jpath, "%s.json", tbase);
            (void)snprintf(ppath, sizeof ppath, "%s-0.png", tbase);
            bool jok = false;
            bool pok = false;
            {
                FILE *jf = fopen(jpath, "rb");
                if (jf) {
                    jok = (fgetc(jf) == '{'); /* lightweight parse check; full parse is in ctest test_export_json */
                    (void)fclose(jf);
                }
                FILE *pf = fopen(ppath, "rb");
                if (pf) {
                    pok = (fgetc(pf) != EOF); /* exists AND non-empty */
                    (void)fclose(pf);
                }
            }
            nt_log_info("SELFTEST: export 'rotate' -> ok=%d targets=%d notices=%d json{=%d png0=%d %s", oke, etg, enc,
                        jok, pok, oke ? "" : eerr);
            NT_ASSERT(oke && jok && pok && "export rotate: json + page png must exist");
            (void)remove(jpath); /* throwaway under the build dir */
            (void)remove(ppath);
        } else {
            nt_log_info("SELFTEST: demo project open failed: %s", perr);
        }
    }

    /* --- stress: 520 procedural sprites incl. a Cyrillic name -> pack + row model + Cyrillic RT --- */
    {
        char sdir[700];
        (void)snprintf(sdir, sizeof sdir, "%s/selftest_stress", s_exe_dir);
#ifdef _WIN32
        (void)CreateDirectoryA(sdir, NULL);
#endif
        const int N = 520;
        for (int i = 0; i < N; i++) {
            char fp[820];
            (void)snprintf(fp, sizeof fp, "%s/spr_%03d.tga", sdir, i);
            write_tga_2x2(fp);
        }
        char cfp[840];
        (void)snprintf(cfp, sizeof cfp, "%s/%s.tga", sdir, CYR_STEM);
        write_tga_2x2(cfp);

        const int sidx = gui_project_add_atlas();
        if (sidx >= 0) {
            (void)gui_project_add_source(sidx, sdir);
            gui_scan_invalidate_all();
            double sms = 0.0;
            char serr[256] = {0};
            char snote[128] = {0};
            const bool oks = gui_pack_atlas(sidx, &sms, serr, sizeof serr, snote, sizeof snote);
            const tp_result *sr = gui_pack_result(sidx);
            const int cyr_idx = gui_pack_find_sprite(sidx, CYR_STEM);
            nt_log_info("SELFTEST: stress pack -> %d in %.1f ms sprites=%d pages=%d cyr_idx=%d %s", oks, sms,
                        sr ? sr->sprite_count : -1, sr ? sr->page_count : -1, cyr_idx, oks ? "" : serr);
            NT_ASSERT(oks && sr && sr->sprite_count >= N + 1 && "stress pack 520+ sprites");
            NT_ASSERT(cyr_idx >= 0 && "Cyrillic-named region lookup");

            /* Cyrillic rename + save/load round-trip (multi-byte name survives serialization). */
            gui_project_set_sprite_rename(sidx, CYR_STEM, "\xD0\xB8\xD0\xBC\xD1\x8F"); /* "имя" */
            char *sbuf = NULL;
            size_t slen = 0;
            tp_error sbe = {0};
            tp_project *slp = NULL;
            tp_error sle = {0};
            const tp_status sbst = tp_project_save_buffer(gui_project_get(), &sbuf, &slen, &sbe);
            const tp_status slst = (sbst == TP_STATUS_OK) ? tp_project_load_buffer(sbuf, slen, &slp, &sle) : sbst;
            const tp_project_sprite *ov =
                (slp && slp->atlas_count > sidx) ? tp_project_atlas_find_sprite(&slp->atlases[sidx], CYR_STEM) : NULL;
            nt_log_info("SELFTEST: Cyrillic rename RT save=%s load=%s override='%s'", tp_status_str(sbst),
                        tp_status_str(slst), (ov && ov->rename) ? ov->rename : "(none)");
            NT_ASSERT(ov && ov->rename && strcmp(ov->rename, "\xD0\xB8\xD0\xBC\xD1\x8F") == 0 &&
                      "Cyrillic name survives save/load");
            tp_project_destroy(slp);
            free(sbuf);

            /* Row model materializes 520+ rows (incl. the Cyrillic label) without overflow. */
            s_sel_atlas = sidx;
            build_rows(gui_project_get(), tp_project_get_atlas(gui_project_get(), sidx));
            bool cyr_row = false;
            for (int i = 0; i < s_row_count; i++) {
                if (strcmp(s_rows[i].sprite_name, CYR_STEM) == 0) {
                    cyr_row = true;
                    break;
                }
            }
            nt_log_info("SELFTEST: stress rows=%d cyr_row=%d | state pool slots=%u probe=%u ring=%u (bounded, no overflow)",
                        s_row_count, cyr_row, (unsigned)UI_STATE_SLOTS, (unsigned)UI_STATE_PROBE_MAX,
                        (unsigned)UI_ROW_ID_RING);
            NT_ASSERT(s_row_count >= N + 1 && cyr_row && "stress row model incl. Cyrillic");
        }
        /* cleanup scratch sprites (keep the tree clean). The no-overflow guarantee is id_ring x
         * state_slots capacity, verified above + interactively. */
        for (int i = 0; i < N; i++) {
            char fp[820];
            (void)snprintf(fp, sizeof fp, "%s/spr_%03d.tga", sdir, i);
            (void)remove(fp);
        }
        (void)remove(cfp);
#ifdef _WIN32
        (void)RemoveDirectoryA(sdir);
#endif
    }

    /* --- large-N caps (P1 fix, decomposition step 7): the row / multi-select / preview-frame arrays
     *     used to silently DROP entries past fixed caps (4096 rows, 4096 multi-select, 512 preview
     *     frames) -- sprites packed fine but VANISHED from the UI. They are growable now; prove it with
     *     EXACT counts so a reintroduced fixed cap fails HERE. Two routes: (A) an in-memory synthetic
     *     project exceeds the 4096 row/multi-select caps without writing >4096 files (too heavy for CI);
     *     (B) a >512-frame animation over REAL packed sprites, which the preview idxs[] path must
     *     resolve end-to-end (a fake result cannot exercise gui_pack_find_sprite). --- */
    {
        const int BIG_N = 4200; /* > the old 4096 row / multi-select cap */

        /* (A1) rows: >4096 (missing) sources materialize >4096 rows -- build_rows grows s_rows.
         *      Sources added straight through tp_core (no per-add project touch/serialize). */
        gui_project_new();
        gui_pack_clear(-1);
        tp_project_atlas *ca = tp_project_get_atlas(gui_project_get(), 0);
        for (int i = 0; i < BIG_N; i++) {
            char sp[24];
            (void)snprintf(sp, sizeof sp, "cap/s%05d.png", i); /* distinct + missing -> exactly 1 row each */
            (void)tp_project_atlas_add_source(ca, sp);
        }
        s_sel_atlas = 0;
        build_rows(gui_project_get(), tp_project_get_atlas(gui_project_get(), 0));
        nt_log_info("SELFTEST: caps rows=%d (want %d; old cap 4096)", s_row_count, BIG_N);
        NT_ASSERT(s_row_count == BIG_N && "sprite rows grow past the old 4096 cap");

        /* (A2) multi-select: >4096 distinct names -- multi_sel_add grows s_multi_sel. */
        multi_sel_clear();
        for (int i = 0; i < BIG_N; i++) {
            char nm[24];
            (void)snprintf(nm, sizeof nm, "cap_%05d", i);
            multi_sel_add(nm);
        }
        nt_log_info("SELFTEST: caps multi_sel=%d (want %d; old cap 4096)", s_multi_sel_count, BIG_N);
        NT_ASSERT(s_multi_sel_count == BIG_N && "multi-select grows past the old 4096 cap");

        /* (A3) sort companions: create-animation natural-sorts the WHOLE selection through
         *      s_sel_sort_buf/ptr; if those did not grow with the set the sort path would re-truncate.
         *      The stored frame count must equal the selection exactly. */
        const int ca_anim = create_animation_from_selection();
        tp_project_atlas *caa = tp_project_get_atlas(gui_project_get(), 0);
        const int ca_frames =
            (caa && ca_anim >= 0 && ca_anim < caa->animation_count) ? caa->animations[ca_anim].frame_count : -1;
        nt_log_info("SELFTEST: caps sort->anim frames=%d (want %d)", ca_frames, BIG_N);
        NT_ASSERT(ca_frames == BIG_N && "sort companions hold the whole selection (no truncation via sort)");
        multi_sel_clear();

        /* (B) preview idxs[]: a >512-frame animation over REAL packed sprites resolves EVERY frame.
         *     Identical 2x2 sprites are NOT deduped (see the 520-sprite stress above), so M files pack
         *     to M regions. */
        gui_project_new();
        gui_pack_clear(-1);
        const int M = 600; /* > the old 512 preview-frame cap */
        char pdir[700];
        (void)snprintf(pdir, sizeof pdir, "%s/selftest_caps", s_exe_dir);
#ifdef _WIN32
        (void)CreateDirectoryA(pdir, NULL);
#endif
        for (int i = 0; i < M; i++) {
            char fp[820];
            (void)snprintf(fp, sizeof fp, "%s/f_%04d.tga", pdir, i);
            write_tga_2x2(fp);
        }
        (void)gui_project_add_source(0, pdir);
        gui_scan_invalidate_all();
        double cms = 0.0;
        char cerr[256] = {0};
        char cnote[128] = {0};
        const bool okc = gui_pack_atlas(0, &cms, cerr, sizeof cerr, cnote, sizeof cnote);
        const tp_result *cr = gui_pack_result(0);
        nt_log_info("SELFTEST: caps pack -> %d sprites=%d (want >= %d) %s", okc, cr ? cr->sprite_count : -1, M,
                    okc ? "" : cerr);
        NT_ASSERT(okc && cr && cr->sprite_count >= M && "caps: pack >512 real sprites");

        s_sel_atlas = 0;
        build_rows(gui_project_get(), tp_project_get_atlas(gui_project_get(), 0));
        multi_sel_clear();
        for (int i = 0; i < s_row_count; i++) { /* select-all the leaf sprites (the real UI gesture) */
            if (!s_rows[i].is_folder && !s_rows[i].missing && s_rows[i].sprite_name[0] != '\0') {
                multi_sel_add(s_rows[i].sprite_name);
            }
        }
        nt_log_info("SELFTEST: caps preview select-all=%d (want %d)", s_multi_sel_count, M);
        NT_ASSERT(s_multi_sel_count == M && "caps: select-all resolves M leaf rows");
        const int panim = create_animation_from_selection();
        NT_ASSERT(panim >= 0 && "caps: animation from M frames");
        open_preview(panim);
        update_preview();
        nt_log_info("SELFTEST: caps preview frames resolved=%d (want %d; old cap 512)", s_preview_frame_count, M);
        NT_ASSERT(s_preview_frame_count == M && "preview resolves all >512 frames (idxs[] grows)");
        preview_stop();
        multi_sel_clear();

        for (int i = 0; i < M; i++) {
            char fp[820];
            (void)snprintf(fp, sizeof fp, "%s/f_%04d.tga", pdir, i);
            (void)remove(fp);
        }
#ifdef _WIN32
        (void)RemoveDirectoryA(pdir);
#endif
        /* leave a clean fresh project for the phases below */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        s_sel_anim = -1;
        s_sel_anim_frame = -1;
    }

    /* --- settings panel: stale-on-change, effective-extrude, per-region RECT override,
     *     and a fresh-project seeded-target export (regions F/G, §3.3f, owner overrides) --- */
    {
        gui_project_new();
        gui_pack_clear(-1);
        tp_project *fp = gui_project_get();
        NT_ASSERT(fp && fp->atlas_count == 1 && fp->atlases[0].target_count == 1 &&
                  "fresh project seeds exactly one target (I1, single-seed invariant)");
        nt_log_info("SELFTEST: fresh target[0]=%s base=%s", fp->atlases[0].targets[0].exporter_id,
                    fp->atlases[0].targets[0].out_path);

        char afolder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
        (void)gui_project_add_source(0, afolder);
        gui_scan_invalidate_all();

        tp_project_atlas *a0 = tp_project_get_atlas(gui_project_get(), 0);
        gui_project_mark_packed(); /* pretend current, then a setting change must set stale */
        a0->padding = 7;
        gui_project_touch_setting();
        nt_log_info("SELFTEST: setting change stale=%d (expect 1)", gui_project_is_stale());
        NT_ASSERT(gui_project_is_stale() && "a setting change sets preview stale");

        /* shape=concave + extrude=3 -> preview pack succeeds via the effective-extrude-0 rule */
        a0->shape = 2; /* CONCAVE_CONTOUR */
        a0->extrude = 3;
        gui_project_touch_setting();
        double pms = 0.0;
        char perr[256] = {0};
        char pnote[128] = {0};
        const bool okc = gui_pack_atlas(0, &pms, perr, sizeof perr, pnote, sizeof pnote);
        nt_log_info("SELFTEST: concave+extrude3 pack -> %d in %.1fms (%s)", okc, pms, okc ? "effective extrude 0" : perr);
        NT_ASSERT(okc && "concave+extrude=3 packs (effective extrude 0)");

        /* per-sprite shape=RECT override -> that region packs as an exact 4-vert rect */
        char afabs[512];
        if (tp_project_resolve_path(gui_project_get(), gui_project_get()->atlases[0].sources[0].path, afabs, sizeof afabs) ==
            TP_STATUS_OK) {
            const gui_scan_result *sc = gui_scan_get(afabs);
            if (sc->count > 0) {
                char spn[192];
                (void)snprintf(spn, sizeof spn, "%s", sc->entries[0].rel);
                char *dot = strrchr(spn, '.');
                if (dot) {
                    *dot = '\0';
                }
                gui_project_set_sprite_override(0, spn, GUI_SPRITE_OV_SHAPE, 0 /* RECT */); /* coalescable: buffers */
                gui_project_flush_pending(); /* commit the buffered override before the raw pack reads the model */
                (void)gui_pack_atlas(0, &pms, perr, sizeof perr, pnote, sizeof pnote);
                const int rri = gui_pack_find_sprite(0, spn);
                const tp_result *rr = gui_pack_result(0);
                const int vc = (rr && rri >= 0) ? rr->sprites[rri].vert_count : -1;
                nt_log_info("SELFTEST: sprite '%s' RECT override -> vert_count=%d (expect 4)", spn, vc);
                NT_ASSERT(vc == 4 && "RECT per-sprite override packs a 4-vert rect");
            }
        }

        /* Restore a valid export state: the EXPORT path (tp_export_run) does not yet
         * apply the effective-extrude-0 rule (point-7 follow-up in the parallel exporter
         * agent's file), so concave+extrude>0 would be rejected at core validation. */
        a0 = tp_project_get_atlas(gui_project_get(), 0); /* F2-05b-i: the sprite-override op above clone-swapped */
        a0->extrude = 0;
        gui_project_touch_setting();

        /* save + export a fresh GUI project -> the seeded target writes files (audit I1) */
        char fpath[1200];
        (void)snprintf(fpath, sizeof fpath, "%s/selftest_fresh.ntpacker_project", s_exe_dir);
        char serr[256] = {0};
        (void)gui_project_save_as(fpath, serr, sizeof serr);
        int etg = 0;
        int enc = 0;
        char eerr[256] = {0};
        char enote[128] = {0};
        const bool oke = gui_pack_export(0, &etg, &enc, eerr, sizeof eerr, enote, sizeof enote);
        char jbase[600] = {0};
        char jpath[640] = {0};
        char ppath[640] = {0};
        bool jok = false;
        bool pok = false;
        if (tp_project_resolve_path(gui_project_get(), "out/atlas1", jbase, sizeof jbase) == TP_STATUS_OK) {
            (void)snprintf(jpath, sizeof jpath, "%s.json", jbase);
            (void)snprintf(ppath, sizeof ppath, "%s-0.png", jbase);
            FILE *jf = fopen(jpath, "rb");
            if (jf) {
                jok = (fgetc(jf) == '{');
                (void)fclose(jf);
            }
            FILE *pf = fopen(ppath, "rb");
            if (pf) {
                pok = true;
                (void)fclose(pf);
            }
        }
        nt_log_info("SELFTEST: fresh export ok=%d targets=%d json{=%d png0=%d %s", oke, etg, jok, pok, oke ? "" : eerr);
        NT_ASSERT(oke && jok && pok && "fresh GUI project exports its seeded target");
        (void)remove(jpath);
        (void)remove(ppath);
        (void)remove(fpath);
    }

    /* --- animations (ux.md §3.7b): pure playback map, create-from-selection natural sort, reorder,
     *     round-trip preserves frames order + playback + flips, remove-frame path --- */
    {
        bool fin = false;
        NT_ASSERT(gui_canvas_anim_frame_at(0.0, 10.0F, 2, 4, &fin) == 3 && !fin && "once_backward step0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.35, 10.0F, 2, 4, &fin) == 0 && fin && "once_backward finishes at 0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.45, 10.0F, 3, 4, &fin) == 3 && "loop_backward wraps");
        NT_ASSERT(gui_canvas_anim_frame_at(0.35, 10.0F, 4, 3, &fin) == 1 && "once_pingpong return leg");
        NT_ASSERT(gui_canvas_anim_frame_at(0.45, 10.0F, 4, 3, &fin) == 0 && fin && "once_pingpong finishes at 0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.55, 10.0F, 5, 3, &fin) == 1 && "loop_pingpong wraps");

        const int aidx = gui_project_add_atlas();
        s_sel_atlas = aidx;
        multi_sel_clear();
        multi_sel_add("walk_10"); /* deliberately out of natural order */
        multi_sel_add("walk_2");
        multi_sel_add("walk_1");
        const int ai = create_animation_from_selection();
        tp_project_atlas *aa = tp_project_get_atlas(gui_project_get(), aidx);
        NT_ASSERT(ai == 0 && aa && aa->animation_count == 1 && "create animation from selection");
        tp_project_anim *an = &aa->animations[0];
        nt_log_info("SELFTEST: anim '%s' frames [%s,%s,%s]", an->name, an->frames[0].name, an->frames[1].name,
                    an->frames[2].name);
        NT_ASSERT(an->frame_count == 3 && strcmp(an->frames[0].name, "walk_1") == 0 &&
                  strcmp(an->frames[1].name, "walk_2") == 0 && strcmp(an->frames[2].name, "walk_10") == 0 &&
                  "frames natural-sorted (walk_2 before walk_10)");

        gui_project_set_anim_playback(aidx, 0, 5); /* loop pingpong */
        gui_project_set_anim_flip(aidx, 0, true, false);
        gui_project_set_anim_fps(aidx, 0, 12.0F);
        gui_project_anim_move_frame(aidx, 0, 0, 2); /* walk_1 rides to the end */
        aa = tp_project_get_atlas(gui_project_get(), aidx);
        an = &aa->animations[0];
        NT_ASSERT(strcmp(an->frames[0].name, "walk_2") == 0 && strcmp(an->frames[2].name, "walk_1") == 0 &&
                  "reorder a frame");

        char *abuf = NULL;
        size_t alen = 0;
        tp_error abe = {0};
        tp_project *alp = NULL;
        tp_error ale = {0};
        const tp_status abs_st = tp_project_save_buffer(gui_project_get(), &abuf, &alen, &abe);
        const tp_status als_st = (abs_st == TP_STATUS_OK) ? tp_project_load_buffer(abuf, alen, &alp, &ale) : abs_st;
        const tp_project_anim *rl = (alp && alp->atlas_count > aidx && alp->atlases[aidx].animation_count > 0)
                                        ? &alp->atlases[aidx].animations[0]
                                        : NULL;
        nt_log_info("SELFTEST: anim RT save=%s load=%s playback=%d flip_h=%d fps=%g", tp_status_str(abs_st),
                    tp_status_str(als_st), rl ? rl->playback : -1, rl ? rl->flip_h : -1, rl ? (double)rl->fps : 0.0);
        NT_ASSERT(rl && rl->frame_count == 3 && rl->playback == 5 && rl->flip_h && !rl->flip_v && rl->fps == 12.0F &&
                  strcmp(rl->frames[0].name, "walk_2") == 0 && strcmp(rl->frames[2].name, "walk_1") == 0 &&
                  "round-trip preserves frame order + playback + flips");
        tp_project_destroy(alp);
        free(abuf);

        NT_ASSERT(gui_project_anim_remove_frame(aidx, 0, 1) && "remove a frame");
        aa = tp_project_get_atlas(gui_project_get(), aidx); /* F2-05b-i: remove-frame clone-swapped */
        NT_ASSERT(aa && aa->animations[0].frame_count == 2 && "remove a frame count");
        nt_log_info("SELFTEST: animation create/reorder/round-trip OK");

        multi_sel_clear();
        s_sel_anim = -1;
        s_sel_anim_frame = -1;
        s_sel_atlas = 0;
    }

    /* --- F2-05b-ii-A: deferred edit queue + GESTURE-SCOPED transaction coalescing (decision 0015) ---
     * The F2-03 diff history has no built-in coalescing (one commit = one undo step). A field-precise
     * pending-transaction buffer coalesces a gesture; the gesture BOUNDARY (a widget's release/blur/
     * discrete pick -- modelled here by gui_request_gesture_commit + apply_pending, or a direct
     * gui_project_flush_pending) commits it as ONE transaction = ONE undo step. */
    {
        /* (1) A gui_edit_* enqueue does not touch the model synchronously; the drained coalescable
         *     setter BUFFERS it (uncommitted); the model changes only when the gesture boundary flushes. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        reset_selection();
        const int pad0 = tp_project_get_atlas(gui_project_get(), 0)->padding;
        gui_edit_atlas_int(0, GUI_ATLAS_PADDING, pad0 + 5);
        const int pad_enq = tp_project_get_atlas(gui_project_get(), 0)->padding; /* enqueue: not applied */
        apply_pending();                                                          /* drains -> BUFFERS (no commit) */
        const int pad_buf = tp_project_get_atlas(gui_project_get(), 0)->padding; /* buffered, still uncommitted */
        gui_request_gesture_commit();                                             /* gesture end (e.g. slider release) */
        apply_pending();                                                          /* flush -> commit */
        const int pad_done = tp_project_get_atlas(gui_project_get(), 0)->padding;
        nt_log_info("SELFTEST: gesture pad %d ->(enqueue) %d ->(drain/buffer) %d ->(gesture-commit) %d", pad0, pad_enq,
                    pad_buf, pad_done);
        NT_ASSERT(pad_enq == pad0 && pad_buf == pad0 && pad_done == pad0 + 5 &&
                  "a coalescable edit buffers on drain and commits only at the gesture boundary");

        /* (2) ONE-control drag = ONE undo step. 8 same-key ticks buffer (latest wins, no commit); the
         *     gesture flush at release commits exactly ONE transaction; one undo reverts the WHOLE drag. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const int pad_pre = tp_project_get_atlas(gui_project_get(), 0)->padding;
        const int undo0 = gui_project_undo_depth();
        for (int v = 1; v <= 8; v++) {
            (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, v, 0.0F); /* same key -> coalesce, no commit */
        }
        const int undo_mid = gui_project_undo_depth();                          /* still undo0: buffered */
        const int pad_mid2 = tp_project_get_atlas(gui_project_get(), 0)->padding; /* still pad_pre */
        gui_project_flush_pending();                                            /* the release boundary */
        const int undo1 = gui_project_undo_depth();
        const int pad_drag = tp_project_get_atlas(gui_project_get(), 0)->padding;
        nt_log_info("SELFTEST: one-control drag: 8 ticks undo %d ->(buffered) %d ->(release) %d, final padding=%d", undo0,
                    undo_mid, undo1, pad_drag);
        NT_ASSERT(undo_mid == undo0 && pad_mid2 == pad_pre &&
                  "an in-flight drag stays buffered (uncommitted) until its gesture boundary");
        NT_ASSERT(undo1 - undo0 == 1 && pad_drag == 8 &&
                  "one control drag = ONE committed transaction = ONE undo step (final value wins)");
        NT_ASSERT(gui_project_undo() && tp_project_get_atlas(gui_project_get(), 0)->padding == pad_pre &&
                  "one undo reverts the ENTIRE coalesced drag (not one tick)");

        /* (2b) DIVERGENCE from b-i (decision 0015): b-i's tag-only key merged DISTINCT knobs edited
         *      within the window into one undo step; A's FIELD-PRECISE key makes each distinct field its
         *      own step. Two distinct knobs -> the second (different key) FLUSHES the first -> TWO steps. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const int undo_b0 = gui_project_undo_depth();
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, 3, 0.0F); /* key = PADDING (buffered) */
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_MARGIN, 4, 0.0F);  /* key = MARGIN: different -> flush PADDING */
        gui_project_flush_pending();                                        /* commit MARGIN */
        const int undo_b1 = gui_project_undo_depth();
        tp_project_atlas *dka = tp_project_get_atlas(gui_project_get(), 0);
        nt_log_info("SELFTEST: two distinct knobs padding=%d margin=%d undo delta=%d (want 2)", dka->padding, dka->margin,
                    undo_b1 - undo_b0);
        NT_ASSERT(undo_b1 - undo_b0 == 2 && dka->padding == 3 && dka->margin == 4 &&
                  "two distinct knobs back-to-back = TWO undo steps (field-precise divergence)");

        /* (2c) FLUSH-BEFORE-IS_DIRTY (the lost-edit trap New/Open/Exit must avoid): a buffered edit is
         *      not yet in the identity, so is_dirty reads CLEAN; the destructive gates flush FIRST, so the
         *      edit is committed (dirty) instead of silently discarded on a New/Open/Exit confirm. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        NT_ASSERT(!gui_project_is_dirty() && "fresh project is clean");
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, 9, 0.0F); /* buffered, uncommitted */
        const bool dirty_buffered = gui_project_is_dirty();
        gui_project_flush_pending();                                        /* what request_new/open/exit do first */
        const bool dirty_flushed = gui_project_is_dirty();
        nt_log_info("SELFTEST: flush-before-is_dirty: buffered dirty=%d, after pre-gate flush dirty=%d", dirty_buffered,
                    dirty_flushed);
        NT_ASSERT(!dirty_buffered && dirty_flushed &&
                  "the pre-gate flush commits a buffered edit so New/Open/Exit confirm instead of discarding it");

        /* (2d) SLICE9 RMW lost-edit is IMPOSSIBLE: slice9 is a read-modify-write (seeds all 4 components
         *      from the record). Two DIFFERENT components edited back-to-back must NOT drop the first --
         *      the component-precise key makes the second a different key, flushing the first BEFORE the
         *      second's RMW read, so the second seeds from the COMMITTED value. Both components survive. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        (void)gui_project_set_sprite_slice9(0, "s9sprite", 0 /* L */, 11); /* buffered */
        (void)gui_project_set_sprite_slice9(0, "s9sprite", 1 /* R */, 22); /* different component -> flush L, seed committed */
        gui_project_flush_pending();                                       /* commit R */
        tp_project_atlas *s9a = tp_project_get_atlas(gui_project_get(), 0);
        const tp_project_sprite *s9ov = tp_project_atlas_find_sprite(s9a, "s9sprite");
        const int s9l = s9ov ? s9ov->slice9_lrtb[0] : -1;
        const int s9r = s9ov ? s9ov->slice9_lrtb[1] : -1;
        nt_log_info("SELFTEST: slice9 RMW L=%d R=%d (want 11,22 -- neither lost)", s9l, s9r);
        NT_ASSERT(s9l == 11 && s9r == 22 &&
                  "slice9 two-component edit: the field-precise key prevents the RMW lost-edit (both survive)");

        /* (3) F1: "Add frames" is DEFERRED (was a synchronous commit -> UAF while declare_animation_editor
         *     held a live `an` it kept dereferencing). The enqueue captures COPIED keys, so the frames land
         *     only on the apply_pending drain AND clearing the live selection between enqueue and drain does
         *     NOT change what lands. If someone reverts to a synchronous commit, fc_mid becomes 2 and this
         *     assertion fails HERE -- the UAF cannot regress silently. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const int f1anim = gui_project_create_animation(0, "addf", NULL, 0); /* empty animation */
        s_sel_anim = f1anim;
        multi_sel_clear();
        multi_sel_add("f_2"); /* deliberately out of natural order */
        multi_sel_add("f_1");
        add_selection_frames_to_anim(f1anim); /* ENQUEUE ONLY -- must not commit synchronously */
        tp_project_atlas *f1a = tp_project_get_atlas(gui_project_get(), 0);
        const int fc_mid = (f1anim >= 0 && f1a) ? f1a->animations[f1anim].frame_count : -1;
        multi_sel_clear();                    /* mutate the selection AFTER the enqueue: copied keys stand */
        apply_pending();                      /* drains -> gui_project_anim_add_frames replays the copies */
        f1a = tp_project_get_atlas(gui_project_get(), 0);
        const int fc_after = (f1anim >= 0 && f1a) ? f1a->animations[f1anim].frame_count : -1;
        const char *ff0 = (fc_after == 2) ? f1a->animations[f1anim].frames[0].name : "";
        nt_log_info("SELFTEST: F1 add-frames deferred: mid=%d after=%d frame0='%s' (want mid=0 after=2 f_1)",
                    fc_mid, fc_after, ff0);
        NT_ASSERT(fc_mid == 0 && fc_after == 2 && strcmp(ff0, "f_1") == 0 &&
                  "Add frames is deferred + captures copied keys (F1 UAF fix)");

        /* (4) F2: a target toggle must carry the FULL out_path (heap), not truncate at 255. Set a >255-char
         *     path, then toggle `enabled` through the SAME deferred gui_edit_target the checkbox uses; after
         *     the drain the stored path must be byte-identical (the old fixed 256-byte queue slot corrupted
         *     any out_path > 255 on a mere enable/disable). */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        char longp[600];
        {
            size_t w = 0;
            w += (size_t)snprintf(longp + w, sizeof longp - w, "out/");
            for (int k = 0; k < 30 && w + 20 < sizeof longp; k++) { /* 30 * ~17 chars -> well over 255 */
                w += (size_t)snprintf(longp + w, sizeof longp - w, "deep_subdir_%02d/", k);
            }
            (void)snprintf(longp + w, sizeof longp - w, "atlas.json");
        }
        NT_ASSERT(strlen(longp) > 255 && "F2 test path must exceed the old 255-byte slot");
        tp_project_atlas *f2a = tp_project_get_atlas(gui_project_get(), 0);
        NT_ASSERT(f2a && f2a->target_count >= 1 && "fresh project seeds a target for the F2 toggle test");
        /* seed the long path via the setter (stored as a heap char*, up to TP_PATH_MAX) */
        (void)gui_project_set_target(0, 0, f2a->targets[0].exporter_id, longp, f2a->targets[0].enabled);
        f2a = tp_project_get_atlas(gui_project_get(), 0);
        const bool en_was = f2a->targets[0].enabled;
        /* the checkbox path: enqueue a toggle reading t->out_path (the full stored path) */
        gui_edit_target(0, 0, f2a->targets[0].exporter_id, f2a->targets[0].out_path, !en_was);
        apply_pending(); /* drains -> gui_project_set_target with the FULL path (no 255 truncation) */
        f2a = tp_project_get_atlas(gui_project_get(), 0);
        nt_log_info("SELFTEST: F2 out_path len=%zu after toggle enabled %d->%d (match=%d)", strlen(f2a->targets[0].out_path),
                    en_was, f2a->targets[0].enabled, strcmp(f2a->targets[0].out_path, longp) == 0);
        NT_ASSERT(strcmp(f2a->targets[0].out_path, longp) == 0 && f2a->targets[0].enabled == !en_was &&
                  "a target toggle preserves the full >255 out_path (F2 no truncation)");

        /* --- F2-05b-ii-A FIX pass (adversarial-review corrections) regressions --- */

        /* (#1) STALE-GUARD lost-edit: the view no longer guards `iv != committed`, so a control
         *      returned to its COMMITTED value still enqueues that value (latest wins). A gesture that
         *      nets back to committed commits NOTHING (the #3 flush-time no-op drop); a gesture that
         *      ends at a NEW final value commits EXACTLY that value -- never the stale intermediate.
         *      (Pre-fix: the guard skipped the correcting enqueue, so the buffer kept the intermediate
         *      and the flush committed the WRONG value = data loss.) */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const int g1_pad = tp_project_get_atlas(gui_project_get(), 0)->padding; /* committed */
        const int g1_u0 = gui_project_undo_depth();
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g1_pad + 36, 0.0F); /* typed "40" (buffered) */
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g1_pad, 0.0F);      /* corrected back to committed */
        gui_project_flush_pending();                                                 /* Enter / boundary */
        const int g1_u1 = gui_project_undo_depth();
        const int g1_pad1 = tp_project_get_atlas(gui_project_get(), 0)->padding;
        nt_log_info("SELFTEST: #1 revert-to-committed undo delta=%d padding=%d (want 0, %d)", g1_u1 - g1_u0, g1_pad1, g1_pad);
        NT_ASSERT(g1_u1 == g1_u0 && g1_pad1 == g1_pad &&
                  "#1: a gesture netting back to the committed value commits NOTHING (no phantom step / no wrong value)");
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g1_pad + 36, 0.0F); /* "40" */
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g1_pad + 3, 0.0F);  /* corrected to a real new value */
        gui_project_flush_pending();
        const int g1_pad2 = tp_project_get_atlas(gui_project_get(), 0)->padding;
        nt_log_info("SELFTEST: #1 revert-to-new final padding=%d undo delta=%d (want %d, 1)", g1_pad2, gui_project_undo_depth() - g1_u0,
                    g1_pad + 3);
        NT_ASSERT(gui_project_undo_depth() - g1_u0 == 1 && g1_pad2 == g1_pad + 3 &&
                  "#1: the final value the user leaves the control at is what commits (not the stale intermediate)");

        /* (#2) ORIGIN two-component RMW lost-edit: origin is now COMPONENT-keyed like slice9. Editing X
         *      then Y back-to-back (no flush between) -> the Y edit's different key flushes the buffered X
         *      first, then seeds the committed X -> BOTH survive. (Pre-fix: X and Y shared one key + a
         *      stale view read-modify-write, so Y replaced {x=new,y=old} with {x=old,y=new} and dropped X.) */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        (void)gui_project_set_sprite_origin(0, "osprite", 0 /* X */, 0.25F); /* buffered */
        (void)gui_project_set_sprite_origin(0, "osprite", 1 /* Y */, 0.75F); /* different key -> flush X, seed committed */
        gui_project_flush_pending();                                         /* commit Y */
        const tp_project_sprite *oov = tp_project_atlas_find_sprite(tp_project_get_atlas(gui_project_get(), 0), "osprite");
        const float g2_ox = oov ? oov->origin_x : -1.0F;
        const float g2_oy = oov ? oov->origin_y : -1.0F;
        nt_log_info("SELFTEST: #2 origin X=%g Y=%g (want 0.25,0.75 -- neither lost)", (double)g2_ox, (double)g2_oy);
        NT_ASSERT(g2_ox == 0.25F && g2_oy == 0.75F &&
                  "#2: origin two-component edit -- the component-precise key prevents the RMW lost-edit (both survive)");

        /* (#3) NET-ZERO gesture drops NO redo branch + pushes NO phantom undo step. Make an edit, undo it
         *      (a redo branch is now present), then run a gesture that nets back to the committed value:
         *      the flush must DISCARD the no-op, leaving the redo branch intact + undo depth unchanged.
         *      (Pre-fix: the unconditional commit pushed a phantom step AND dropped the redo branch.) */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const int g3_pad = tp_project_get_atlas(gui_project_get(), 0)->padding;
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g3_pad + 11, 0.0F);
        gui_project_flush_pending(); /* commit an edit (padding = +11) */
        NT_ASSERT(gui_project_undo() && "#3 setup: undo the committed edit");
        NT_ASSERT(gui_project_can_redo() && "#3 setup: a redo branch is present after the undo");
        const int g3_u = gui_project_undo_depth();
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g3_pad + 30, 0.0F); /* drag out */
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g3_pad, 0.0F);      /* back to committed */
        gui_project_flush_pending();                                                 /* release: must DISCARD the no-op */
        nt_log_info("SELFTEST: #3 net-zero gesture undo delta=%d can_redo=%d (want 0, 1)", gui_project_undo_depth() - g3_u,
                    gui_project_can_redo());
        NT_ASSERT(gui_project_undo_depth() == g3_u && "#3: a net-zero gesture pushes NO phantom undo step");
        NT_ASSERT(gui_project_can_redo() && "#3: a net-zero gesture preserves the redo branch (no unconditional commit)");
        NT_ASSERT(gui_project_redo() && tp_project_get_atlas(gui_project_get(), 0)->padding == g3_pad + 11 &&
                  "#3: the preserved redo branch still restores the edit");

        /* (#4) FLUSH-BEFORE-READ (the --parity UAF): mirror the fixed --parity target step. A buffered
         *      slice9 edit is still in flight; FLUSH first (the commit clone-swaps + frees the current
         *      project), re-get the atlas from the stable project, COPY exporter_id into a local, THEN
         *      set_target (its own internal flush is now a no-op). The pre-fix order read the target's
         *      exporter_id AFTER set_target's flush had freed the project it pointed into (UAF). */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        tp_project_atlas *g4a = tp_project_get_atlas(gui_project_get(), 0);
        NT_ASSERT(g4a && g4a->target_count > 0 && "#4: fresh project seeds a default target");
        (void)gui_project_set_sprite_slice9(0, "p4sprite", 0 /* L */, 5); /* buffered across the flush boundary */
        gui_project_flush_pending();                                      /* commit -> clone-swap + free the old project */
        tp_project_atlas *g4b = tp_project_get_atlas(gui_project_get(), 0); /* re-get from the now-stable project */
        char g4_exp[64];
        (void)snprintf(g4_exp, sizeof g4_exp, "%s", g4b->targets[0].exporter_id); /* COPY before set_target's flush */
        (void)gui_project_set_target(0, 0, g4_exp, "out/p4", true);
        tp_project_atlas *g4c = tp_project_get_atlas(gui_project_get(), 0);
        nt_log_info("SELFTEST: #4 flush-before-read target path='%s' exporter='%s'", g4c->targets[0].out_path,
                    g4c->targets[0].exporter_id);
        NT_ASSERT(strcmp(g4c->targets[0].out_path, "out/p4") == 0 && strcmp(g4c->targets[0].exporter_id, g4_exp) == 0 &&
                  "#4: flush-before-read commits the target with no dangling exporter read (parity UAF fix)");

        /* (#5) EFFECTIVE slice9 peek for the canvas guides: the peek returns the BUFFERED slice9 while a
         *      slice9 gesture is buffered (so the on-canvas guides move THIS frame), and false once the
         *      gesture flushes (the caller then reads the committed record). */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        int g5[4] = {-1, -1, -1, -1};
        NT_ASSERT(!gui_project_peek_pending_slice9(0, "p5sprite", g5) && "#5: no buffered slice9 -> peek returns false");
        (void)gui_project_set_sprite_slice9(0, "p5sprite", 2 /* T */, 9); /* buffered */
        const bool g5_hit = gui_project_peek_pending_slice9(0, "p5sprite", g5);
        nt_log_info("SELFTEST: #5 slice9 peek hit=%d L,R,T,B=%d,%d,%d,%d (want 1 and 0,0,9,0)", g5_hit, g5[0], g5[1], g5[2],
                    g5[3]);
        NT_ASSERT(g5_hit && g5[0] == 0 && g5[1] == 0 && g5[2] == 9 && g5[3] == 0 &&
                  "#5: peek returns the buffered slice9 (edited component + seeded others) while buffered");
        NT_ASSERT(!gui_project_peek_pending_slice9(0, "other", g5) && "#5: the peek is keyed on the sprite (a different key misses)");
        gui_project_flush_pending();
        NT_ASSERT(!gui_project_peek_pending_slice9(0, "p5sprite", g5) &&
                  "#5: after the gesture flush the peek returns false (read the committed record)");

        /* (#9) BROWSE-TARGET UAF (the REAL UI path do_browse_target_at, not just --parity): a coalescable
         *      edit is buffered but UNFLUSHED, then set_target is called with exporter_id pointing INTO the
         *      live project (as do_browse_target_at passes t->exporter_id). set_target's internal flush
         *      commits the buffered edit -> the clone-swap frees that project -> the pre-fix dupstr(exporter_id)
         *      read FREED memory. The sink now dups the caller strings BEFORE its flush, so this is clean.
         *      Under CI ASan the pre-fix order faults here; the value assertion also guards a garbage read. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        tp_project_atlas *g9a = tp_project_get_atlas(gui_project_get(), 0);
        NT_ASSERT(g9a && g9a->target_count > 0 && "#9: fresh project seeds a default target");
        char g9_want[64];
        (void)snprintf(g9_want, sizeof g9_want, "%s", g9a->targets[0].exporter_id); /* expected value (copied now) */
        const char *g9_exp = g9a->targets[0].exporter_id; /* project-owned pointer, as do_browse_target_at holds t->exporter_id */
        (void)gui_project_set_sprite_slice9(0, "g9sprite", 0 /* L */, 7); /* buffer a real (non-noop) pending op, UNFLUSHED */
        (void)gui_project_set_target(0, 0, g9_exp, "out/g9", true); /* its flush frees the project g9_exp points into */
        tp_project_atlas *g9c = tp_project_get_atlas(gui_project_get(), 0);
        nt_log_info("SELFTEST: #9 browse-target UAF exporter='%s' path='%s' (want '%s','out/g9')",
                    g9c->targets[0].exporter_id, g9c->targets[0].out_path, g9_want);
        NT_ASSERT(strcmp(g9c->targets[0].exporter_id, g9_want) == 0 && strcmp(g9c->targets[0].out_path, "out/g9") == 0 &&
                  "#9: set_target dups caller strings before its flush -> no dangling exporter read (browse-target UAF fix)");

        /* (#10) SIBLING-SINK UAF (gui_project_remove_animation, same class as #9): the setter flushes
         *       BEFORE reading its caller-supplied `id`, whose production caller passes
         *       a->animations[i].name (a pointer INTO the live project). A buffered gesture's flush
         *       clone-swaps + frees that project -> `id` dangles at find_anim_by_name. Fixed by
         *       dup-before-flush. Under CI ASan the pre-fix order faults; the count assertion guards
         *       a botched (garbage-matched or skipped) removal locally. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const int g10i = gui_project_create_animation(0, "g10anim", NULL, 0);
        NT_ASSERT(g10i >= 0 && "#10: created an animation to remove");
        tp_project_atlas *g10a = tp_project_get_atlas(gui_project_get(), 0);
        const int g10n0 = g10a->animation_count;
        const char *g10name = g10a->animations[g10i].name; /* project-owned ptr, as the remove handler passes */
        (void)gui_project_set_sprite_slice9(0, "g10sprite", 0 /* L */, 3); /* buffer a real (non-noop) op, UNFLUSHED */
        gui_project_remove_animation(0, g10name); /* its flush frees the project g10name points into */
        tp_project_atlas *g10c = tp_project_get_atlas(gui_project_get(), 0);
        nt_log_info("SELFTEST: #10 sibling-sink remove-anim count %d->%d (want %d)", g10n0, g10c->animation_count,
                    g10n0 - 1);
        NT_ASSERT(g10c->animation_count == g10n0 - 1 &&
                  "#10: remove_animation dups `id` before its flush -> the animation is removed with no dangling read");

        /* Restore a packable atlas-0 project for the render frames below (the pixel probe packs
         * atlas 0 and probes its region outlines) -- gui_project_new left it source-less. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        reset_selection();
        char pfolder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", pfolder, sizeof pfolder);
        (void)gui_project_add_source(0, pfolder);
        gui_scan_invalidate_all();
    }

    /* --- F2-05b-ii-B: LIVE recovery journal -- append-fail UX + crash-recovery round-trip (both ways) --- */
    {
        /* (J1) APPEND-FAIL UX: a recovery-journal append failure (full disk) must reject the commit
         *      cleanly -- the live model is BYTE-UNCHANGED, dirty is unchanged, a clear status is
         *      raised, no half-applied edit -- and the editor must keep working once the failure
         *      clears. Driven deterministically: attach a memory-io recovery journal to a fresh model,
         *      arm the NEXT journal write to fail, then commit a real (structural, immediate) edit. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        reset_selection();
        (void)gui_project_take_op_error(NULL, 0); /* clear any stale soft-error */
        tp_journal_io fio = gui_project__test_attach_memory_journal();
        NT_ASSERT(fio.ctx && "J1: memory recovery journal attached to the live model");
        char nm0[64];
        (void)snprintf(nm0, sizeof nm0, "%s", tp_project_get_atlas(gui_project_get(), 0)->name);
        const tp_id128 id_before = tp_semantic_identity(gui_project_get());
        const bool dirty_before = gui_project_is_dirty();
        tp_journal_io_memory__fail_next_writes(fio, 1); /* the NEXT journal append fails entirely */
        const bool committed = gui_project_set_atlas_name(0, "append_should_fail"); /* structural -> immediate commit */
        char emsg[256] = {0};
        const bool surfaced = gui_project_take_op_error(emsg, sizeof emsg);
        const tp_id128 id_after = tp_semantic_identity(gui_project_get());
        const char *nm1 = tp_project_get_atlas(gui_project_get(), 0)->name;
        nt_log_info("SELFTEST: J1 append-fail committed=%d surfaced=%d msg='%s' name '%s'->'%s' dirty %d->%d",
                    (int)committed, (int)surfaced, emsg, nm0, nm1, (int)dirty_before, (int)gui_project_is_dirty());
        NT_ASSERT(!committed && "J1: a journal append failure REJECTS the commit");
        NT_ASSERT(surfaced && emsg[0] && "J1: the append failure surfaces a status-bar error");
        NT_ASSERT(tp_id128_eq(id_before, id_after) && strcmp(nm1, nm0) == 0 &&
                  "J1: the live model is BYTE-UNCHANGED after the rejected append (no half-applied edit)");
        NT_ASSERT(gui_project_is_dirty() == dirty_before && "J1: dirty is unchanged after the rejected append");
        /* the fault was one-shot -> a further edit now commits normally (editor still live) */
        NT_ASSERT(gui_project_set_atlas_name(0, "works_after") &&
                  strcmp(tp_project_get_atlas(gui_project_get(), 0)->name, "works_after") == 0 &&
                  "J1: the editor keeps working once the append failure clears");
        (void)gui_project_take_op_error(NULL, 0); /* the recovered edit raised no error; clear defensively */

        /* (J3) CRASH-RECOVERY round-trip, BOTH ways, through the real GUI wiring (gui_project_enable_
         *      recovery + the adopt-at-init path + the clean-exit slot reset). ISOLATED slot under the
         *      build dir (never touches the production recovery path). A "crash" is simulated by tearing
         *      the model down with recovery DISABLED (shutdown leaves the slot on disk); a "clean exit"
         *      tears it down with recovery ENABLED (shutdown deletes the slot). */
        char jslot[1200];
        (void)snprintf(jslot, sizeof jslot, "%s/selftest_recovery.ntpjournal", s_exe_dir);
        (void)remove(jslot); /* start from a clean slot */

        /* Session 1: enable recovery + re-init -> fresh project + a live journal at the slot. */
        gui_project_enable_recovery("");   /* disable first so this teardown never deletes a slot */
        gui_project_shutdown();            /* tear down the current (memory-journal) model; no slot op */
        gui_project_enable_recovery(jslot);
        gui_project_init();                /* s_model NULL -> fresh + a recovery journal at jslot */
        NT_ASSERT(gui_project_get() && !gui_project_is_dirty() && "J3: a fresh journaled session starts clean");
        (void)gui_project_set_atlas_name(0, "recovered_atlas"); /* committed edit -> appended to the journal */
        NT_ASSERT(gui_project_is_dirty() && "J3: the committed edit dirties the session");

        /* Simulate a CRASH: tear the model down WITHOUT deleting the slot (disable recovery for the
         * teardown, so the slot file survives on disk exactly as it would after a real crash). */
        gui_project_enable_recovery("");
        gui_project_shutdown();            /* closes the journal file handle; the slot file remains */

        /* Session 2 (restart after crash): re-enable recovery + init -> the slot is adopted. */
        gui_project_enable_recovery(jslot);
        gui_project_init();                /* s_model NULL -> recovery adopts the slot's last committed state */
        char rnote[256] = {0};
        const bool got_notice = gui_project_take_recovery_notice(rnote, sizeof rnote);
        const bool recov_dirty = gui_project_is_dirty();
        const char *recov_name = gui_project_get() ? tp_project_get_atlas(gui_project_get(), 0)->name : "(none)";
        nt_log_info("SELFTEST: J3 recover-after-crash notice=%d dirty=%d name='%s' (want 1,1,'recovered_atlas')",
                    (int)got_notice, (int)recov_dirty, recov_name);
        NT_ASSERT(got_notice && "J3: a recovered session raises the one-shot recovery notice");
        NT_ASSERT(recov_dirty && "J3: the recovered model is DIRTY (unsaved recovered work -> prompt Save)");
        NT_ASSERT(gui_project_get() && strcmp(recov_name, "recovered_atlas") == 0 &&
                  "J3: recovery rebuilds the last committed state (the edit survived the crash)");

        /* CLEAN EXIT: tear down WITH recovery enabled -> shutdown deletes the slot. */
        gui_project_shutdown();

        /* Session 3 (relaunch after a clean exit): init -> NO recovery (slot gone) -> fresh + clean. */
        gui_project_init();
        const bool spurious = gui_project_take_recovery_notice(rnote, sizeof rnote);
        nt_log_info("SELFTEST: J3 after-clean-exit dirty=%d atlases=%d spurious_notice=%d (want 0,1,0)",
                    (int)gui_project_is_dirty(), gui_project_get() ? gui_project_get()->atlas_count : -1, (int)spurious);
        NT_ASSERT(!spurious && "J3: NO spurious recovery notice after a clean exit");
        NT_ASSERT(gui_project_get() && gui_project_get()->atlas_count == 1 && !gui_project_is_dirty() &&
                  "J3: a clean exit leaves nothing to recover -> the next launch is a fresh clean project");

        /* Done: disable recovery + restore a journal-LESS packable project for the render phases. */
        gui_project_enable_recovery("");
        gui_project_shutdown();
        (void)remove(jslot); /* belt-and-suspenders: leave no stray sidecar in the build dir */
        gui_project_init();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        reset_selection();
        char rfolder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", rfolder, sizeof rfolder);
        (void)gui_project_add_source(0, rfolder);
        gui_scan_invalidate_all();
    }

    /* --- About modal: open it so the auto-quit frames render it (OK/Esc close it interactively) --- */
    s_about_open = true;
    nt_log_info("SELFTEST: About modal opened=%d", s_about_open);

    /* --- Export dialog: exercise its data path (toggle a target the way the dialog checkbox does) and
     * leave it open so the warmup frames render the modal (a Clay layout bug there would crash them). --- */
    {
        tp_project *ep = gui_project_get();
        int e_atlas = -1;
        for (int i = 0; ep && i < ep->atlas_count; i++) {
            if (ep->atlases[i].target_count > 0) {
                e_atlas = i;
                break;
            }
        }
        if (e_atlas >= 0) {
            const tp_project_target *t0 = &gui_project_get()->atlases[e_atlas].targets[0];
            const bool was = t0->enabled;
            gui_project_set_target(e_atlas, 0, t0->exporter_id, t0->out_path, !was); /* dialog toggle path */
            const bool now = gui_project_get()->atlases[e_atlas].targets[0].enabled;
            gui_project_set_target(e_atlas, 0, gui_project_get()->atlases[e_atlas].targets[0].exporter_id,
                                   gui_project_get()->atlases[e_atlas].targets[0].out_path, was); /* restore */
            nt_log_info("SELFTEST: export-dialog toggle atlas=%d target0 %d->%d (restored=%d)", e_atlas, was, now, was);
        }
        s_export_open = true;
    }

    /* Leave a live selection so the auto-quit frames draw the decoded image. */
    tp_project *cur = gui_project_get();
    const int ns = cur ? cur->atlases[0].source_count : 0;
    if (cur && ns > 0) {
        char resolved[512];
        if (tp_project_resolve_path(cur, cur->atlases[0].sources[ns - 1].path, resolved, sizeof resolved) == TP_STATUS_OK) {
            (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", resolved);
            s_sel_atlas = 0;
            s_sel_src = ns - 1;
            s_sel_child = -1;
            s_sel_missing = false;
        }
    }
    /* Render coverage: leave a real animation selected + previewing so the auto-quit frames exercise the
     * left-panel animations rows, the right-panel editor, and the canvas preview (draw_anim_frame on the
     * packed regions) -- a Clay layout bug in the new UI would crash these frames. */
    {
        s_sel_atlas = 0;
        tp_project_atlas *pa = tp_project_get_atlas(gui_project_get(), 0);
        const tp_result *pr = gui_pack_result(0);
        if (pa && pr && pr->sprite_count > 0) {
            multi_sel_clear();
            for (int i = 0; i < pr->sprite_count && i < 4; i++) {
                char key[192];
                tp_sprite_export_key(pr->sprites[i].name, key, sizeof key);
                multi_sel_add(key);
            }
            const int pai = create_animation_from_selection();
            pa = tp_project_get_atlas(gui_project_get(), 0); /* F2-05b-i: create_animation clone-swapped */
            if (pai >= 0 && pa) {
                open_preview(pai);
                nt_log_info("SELFTEST: preview anim '%s' active=%d frames=%d", pa->animations[pai].name, s_preview_active,
                            pa->animations[pai].frame_count);
            }
            multi_sel_clear();
        }
    }
    g_ui_scale = 1.5F; /* exercise the scaled layout during the auto-quit frames */
    nt_log_info("SELFTEST: end (undo:%d redo:%d; selection '%s')", gui_project_undo_depth(),
                gui_project_redo_depth(), s_sel_abs);
}

/* --- Overlay pixel probe (F) + touch-on-render guard, driven across the auto-quit frames --- */
static int s_st_phase;      /* 0 warmup, 1 outline pixel probe, 2 touch-on-render guard, 3 done */
static int s_st_pf;         /* frames spent in the current phase */
static int s_st_cyan0;      /* outline-OFF cyan count (baseline of the diff test) */
static char *s_st_baseline; /* fresh-project bytes captured with zero input */
static size_t s_st_baseline_n;

/* Count blue/cyan overlay pixels in the current canvas box (framebuffer read, top-left origin). The
 * region-outline colour is (0.30,0.72,1.0): B high, B>>R, G>R -- distinct from grey checker + sprites. */
static int selftest_probe_cyan(void) {
    if (gui_canvas_get_mode(&s_canvas) != GUI_CANVAS_ATLAS || !gui_canvas_has_atlas(&s_canvas)) {
        return -1;
    }
    const float *bb = s_canvas.last_bb;
    int x = (int)bb[0];
    int y = (int)bb[1];
    int w = (int)bb[2];
    int h = (int)bb[3];
    if (w < 8 || h < 8) {
        return -1;
    }
    if (w > 900) {
        w = 900;
    }
    if (h > 900) {
        h = 900;
    }
    const uint32_t capn = (uint32_t)w * (uint32_t)h * 4u;
    uint8_t *px = (uint8_t *)malloc(capn);
    if (!px) {
        return -1;
    }
    int cyan = -1;
    if (nt_gfx_read_pixels(x, y, w, h, px, capn)) {
        cyan = 0;
        for (uint32_t i = 0; i + 3u < capn; i += 4u) {
            const int r = px[i];
            const int g = px[i + 1];
            const int b = px[i + 2];
            if (b > 150 && b > r + 40 && g > r + 25 && g > 110) {
                cyan++;
            }
        }
    }
    free(px);
    return cyan;
}

/* Overflow regression: the key containers must sit inside the window and the right-panel content must not
 * be wider than the panel (rows fit). Reads the PREVIOUS frame's committed layout, so the caller must have
 * held the target size for >= 2 frames. Fails (NT_ASSERT) before the layout fix, passes after. */
static void selftest_assert_no_overflow(float win_w, float win_h) {
    const struct {
        const char *name;
        uint32_t id;
    } items[4] = {{"left", s_id_left_panel}, {"strip", s_id_strip}, {"canvas", s_id_canvas},
                  {"right", s_id_right_panel}}; /* status bar removed (pass 2): messages float as a pill */
    for (int i = 0; i < 4; i++) {
        const nt_ui_bbox_t b = nt_ui_get_bbox(s_ctx, items[i].id);
        nt_log_info("SELFTEST-BOUNDS %-6s found=%d x=%.1f y=%.1f w=%.1f h=%.1f right=%.1f/%.0f bottom=%.1f/%.0f",
                    items[i].name, (int)b.found, (double)b.x, (double)b.y, (double)b.width, (double)b.height,
                    (double)(b.x + b.width), (double)win_w, (double)(b.y + b.height), (double)win_h);
        NT_ASSERT(b.found && "SELFTEST overflow: key container was not laid out");
        NT_ASSERT(b.x >= -1.0F && (b.x + b.width) <= win_w + 1.0F &&
                  "SELFTEST overflow: container spills past the window horizontally");
        NT_ASSERT(b.y >= -1.0F && (b.y + b.height) <= win_h + 1.0F &&
                  "SELFTEST overflow: container spills past the window vertically");
    }
    const nt_ui_bbox_t rp = nt_ui_get_bbox(s_ctx, s_id_right_panel);
    const nt_ui_bbox_t rc = nt_ui_get_bbox(s_ctx, s_id_right_content);
    NT_ASSERT(rp.found && rc.found && (rc.x + rc.width) <= (rp.x + rp.width) + 2.0F &&
              "SELFTEST overflow: right-panel rows bleed past the panel");
}

/* Top-of-frame phase driver: sets up each phase's scene BEFORE the layout/walk. */
void selftest_pre_frame(void) {
    s_st_pf++;
    if (s_st_phase == 0) {
        if (s_st_pf < 12) {
            return; /* warm up: first scene + GL page uploads settle */
        }
        s_about_open = false;
        s_export_open = false; /* close the Export dialog exercised during warmup before the pixel probe */
        preview_stop();
        int found = -1;
        tp_project *p = gui_project_get();
        for (int i = 0; p && i < p->atlas_count; i++) {
            const tp_result *r = gui_pack_result(i);
            if (r && r->sprite_count > 0 && r->page_count > 0) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            s_sel_atlas = 0;
            do_pack_blocking();
            found = (gui_pack_result(0) && gui_pack_result(0)->sprite_count > 0) ? 0 : -1;
        }
        s_sel_atlas = (found >= 0) ? found : 0;
        gui_canvas_select(&s_canvas, -1); /* no selection -> plain hull outlines */
        s_canvas.mode = GUI_CANVAS_ATLAS;
        s_canvas.show_outline = true;
        s_canvas.show_trim = false;
        s_canvas.show_frame = false;
        s_canvas.show_pivot = false;
        s_st_phase = 1;
        s_st_pf = 0;
    } else if (s_st_phase == 1) {
        s_canvas.mode = GUI_CANVAS_ATLAS; /* hold ATLAS mode through the probe frames */
        /* OFF for the first frames (settled diff baseline captured at pf 5), then ON for the whole retry
         * window. The readback + retry logic lives in selftest_post_draw (see the mechanism note there). */
        s_canvas.show_outline = (s_st_pf > 5);
    } else if (s_st_phase == 2) {
        if (s_st_pf > 10) {
            const bool dirty = gui_project_is_dirty();
            char *nb = NULL;
            size_t nn = 0;
            tp_error e = {0};
            const bool saved = tp_project_save_buffer(gui_project_get(), &nb, &nn, &e) == TP_STATUS_OK;
            const bool same = saved && s_st_baseline && nn == s_st_baseline_n && memcmp(nb, s_st_baseline, nn) == 0;
            nt_log_info("SELFTEST: touch-on-render guard dirty=%d bytes_match=%d (%zu vs %zu)", dirty, same, nn, s_st_baseline_n);
            NT_ASSERT(!dirty); /* a control that writes its widget value on first render flips this */
            NT_ASSERT(same);
            free(nb);
            free(s_st_baseline);
            s_st_baseline = NULL;
            s_st_phase = 3;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 3) {
        /* Section-toggle sweep at a CLAMPED panel width: collapsed/expanded + empty sections (the fresh
         * project has no sprites/anims) under the clipped scroll must never yield a degenerate float. */
        g_nt_window.fb_width = 520;
        g_nt_window.fb_height = 440;
        s_sec_atlas_open = (s_st_pf / 2) % 2 == 0;
        s_atlas_adv_open = (s_st_pf / 3) % 2 == 0;
        s_sec_region_open = (s_st_pf / 2) % 2 != 0;
        s_sec_anim_open = (s_st_pf / 4) % 2 == 0;
        s_sec_export_open = (s_st_pf / 3) % 2 != 0;
        if (s_st_pf > 16) {
            g_nt_window.fb_width = 1280;
            g_nt_window.fb_height = 800;
            s_sec_atlas_open = s_atlas_adv_open = s_sec_region_open = s_sec_anim_open = s_sec_export_open = true;
            nt_log_info("SELFTEST: section-toggle sweep OK (no empty-scissor assert)");
            s_st_phase = 4;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 4) {
        /* Tiny-window sweep: the layout solve must not assert (empty scissor) at any size. Override the
         * framebuffer dims for two frames each (restored at the end) -- nt_window_poll re-reads them next
         * frame, so this only affects the current frame's scale. Covers panels-declared-and-clamped down
         * to the have_room skip threshold. */
        static const int sizes[8][2] = {{700, 500}, {560, 420}, {480, 360}, {420, 320}, {360, 280}, {240, 180}, {120, 120}, {64, 64}};
        const int idx = s_st_pf / 2;
        if (idx >= 8) {
            g_nt_window.fb_width = 1280;
            g_nt_window.fb_height = 800;
            nt_log_info("SELFTEST: tiny-window sweep OK (no empty-scissor assert)");
            s_st_phase = 5;
            s_st_pf = 0;
        } else {
            g_nt_window.fb_width = (uint32_t)sizes[idx][0];
            g_nt_window.fb_height = (uint32_t)sizes[idx][1];
        }
    } else if (s_st_phase == 5) {
        /* Scaled 16:9 overflow regression (owner's case): at 1366x768 @ g_ui_scale 1.5 no key container may
         * leave the window and the right-panel rows must fit the panel. Pre-fix the strip forced the middle
         * row wider than the window -> the right panel was pushed off-screen (asserts fire here). */
        g_nt_window.fb_width = 1366;
        g_nt_window.fb_height = 768;
        if (s_st_pf == 1) { /* enter: exercise the normal atlas strip + a populated Region panel */
            preview_stop();
            s_sel_anim = -1;
            s_sel_anim_frame = -1;
            s_canvas.mode = GUI_CANVAS_ATLAS;
            const tp_result *pr = gui_pack_result(s_sel_atlas);
            if (pr && pr->sprite_count > 0) {
                gui_canvas_select(&s_canvas, 0);
                select_row_for_region(0);
            }
            s_sec_atlas_open = s_sec_region_open = s_sec_anim_open = s_sec_export_open = true;
            s_atlas_adv_open = false;
        }
        if (s_st_pf >= 3) { /* size held >= 2 frames -> the 1-frame-lagged bbox now reflects 1366x768 */
            selftest_assert_no_overflow(1366.0F, 768.0F);
            nt_log_info("SELFTEST: 16:9 @1.5 overflow check OK (1366x768, no container/right-panel spill)");
            s_st_phase = 6;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 6 || s_st_phase == 7 || s_st_phase == 8) {
        /* Stale-state overflow regression (owner's icon-strip case): a packed-but-stale atlas shows the amber
         * Pack + the "outdated" chip. The chip gate must keep the labeled+chip strip min-content inside the
         * strip's real budget (s_canvas_w - the canvas card's S(20) padding); pre-fix STRIP_CHIP_MIN_W ignored
         * the chip's own width, so at 1920x1080@1.5 the chip forced the middle row wider -> right panel off the
         * screen. Three stops, all @1.5 (page count varies with the project, so the chip visible/dropped assert
         * -- which depends only on the gate, not on the strip's pixel width -- is the deterministic fail-before):
         *  6) 1920x1080 -- chip does NOT fit; must be DROPPED (fail-before: chip shown -> overflow assert).
         *  7) 1366x768  -- compact two-row stale strip (chip already dropped); must still stay in-window.
         *  8) 2200x1080 -- wide enough that the chip DOES fit; must be SHOWN and still not overflow. (2200,
         *     not 2000: packet EXP-PREVIEW's fixed-width preview selector now also sits in this row, so the
         *     "roomy enough for the chip" stop -- STRIP_CHIP_MIN_W -- rose above the 2000@1.5 canvas width.) */
        const float win_w = (s_st_phase == 6) ? 1920.0F : (s_st_phase == 7) ? 1366.0F : 2200.0F;
        const float win_h = (s_st_phase == 7) ? 768.0F : 1080.0F;
        g_ui_scale = 1.5F;
        g_nt_window.fb_width = (uint32_t)win_w;
        g_nt_window.fb_height = (uint32_t)win_h;
        if (s_st_pf == 1) {
            /* Phase 1's handoff (selftest_post_draw) left a truly-fresh, source-less project, so build the
             * stale scene here: a MULTI-PAGE atlas (small max_size -> page buttons, matching the owner's
             * full-tier strip at 1920x1080) that is packed, then re-marked stale so the strip shows the amber
             * Pack + the "outdated" chip. mark_stale must run AFTER the pack (a successful pack clears stale). */
            preview_stop();
            s_sel_anim = -1;
            s_sel_anim_frame = -1;
            s_sel_atlas = 0;
            tp_project_atlas *sa = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
            if (sa && sa->source_count == 0) {
                char afolder[512];
                to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
                (void)gui_project_add_source(s_sel_atlas, afolder);
                sa = tp_project_get_atlas(gui_project_get(), s_sel_atlas); /* F2-05b-i: add_source clone-swapped */
                sa->max_size = 256; /* 128px sprites -> several pages -> pc>1 -> page buttons in the strip */
                gui_scan_invalidate_all();
            }
            double pms = 0.0;
            char perr[256] = {0};
            char pnote[128] = {0};
            (void)gui_pack_atlas(s_sel_atlas, &pms, perr, sizeof perr, pnote, sizeof pnote);
            s_canvas.mode = GUI_CANVAS_ATLAS;
            const tp_result *pr = gui_pack_result(s_sel_atlas);
            if (pr && pr->sprite_count > 0) {
                gui_canvas_select(&s_canvas, 0);
                select_row_for_region(0);
            }
            s_sec_atlas_open = s_sec_region_open = s_sec_anim_open = s_sec_export_open = true;
            s_atlas_adv_open = false;
            gui_project_mark_stale();
        }
        if (s_st_pf >= 3) { /* size + stale held >= 2 frames -> the lagged bbox reflects the stale strip here */
            const tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
            NT_ASSERT(a && a->source_count > 0 && gui_project_is_stale() &&
                      "SELFTEST: stale precondition (sources present + preview stale -> amber Pack + chip)");
            selftest_assert_no_overflow(win_w, win_h);
            /* The chip visible/dropped decision depends ONLY on the gate (accent && width), not on the strip's
             * pixel width, so it is the deterministic fail-before signal even where the page count would let the
             * bounds check pass: at 1920x1080@1.5 the chip must be DROPPED, at the wide 2200 it must be SHOWN. */
            const bool chip = nt_ui_get_bbox(s_ctx, nt_ui_id("ntpacker/stale_chip")).found;
            if (s_st_phase == 6) {
                NT_ASSERT(!chip && "SELFTEST: stale chip must be dropped where it would overflow the canvas budget");
            } else if (s_st_phase == 8) {
                NT_ASSERT(chip && "SELFTEST: stale chip must be shown when the canvas is wide enough to hold it");
            }
            nt_log_info("SELFTEST: stale-state overflow check OK (%.0fx%.0f@1.5, chip=%d)", (double)win_w,
                        (double)win_h, (int)chip);
            s_st_phase++;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 9) {
        /* Async-path equivalence (req 4): start an async pack, spin until it lands (poll_async in
         * apply_pending swaps it in), then a blocking reference pack of the same project must match --
         * determinism holds because only WHERE the pack ran changed. */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            s_sel_atlas = 0;
            char aerr[256] = {0};
            const bool started = gui_pack_async_start(0, aerr, sizeof aerr);
            nt_log_info("SELFTEST: async pack start -> %d (%s)", (int)started, started ? "ok" : aerr);
            NT_ASSERT(started && "SELFTEST: async pack must start");
        } else if (gui_pack_async_busy()) {
            NT_ASSERT(s_st_pf < 3000 && "SELFTEST: async pack did not finish within the frame cap");
        } else {
            const tp_result *ra = gui_pack_result(0);
            NT_ASSERT(ra && ra->sprite_count > 0 && ra->page_count > 0 && "SELFTEST: async pack produced no result");
            const int sc_a = ra->sprite_count;
            const int pc_a = ra->page_count;
            const int pw_a = ra->pages[0].w;
            const int ph_a = ra->pages[0].h;
            double bms = 0.0;
            char berr[256] = {0};
            char bnote[128] = {0};
            const bool okb = gui_pack_atlas(0, &bms, berr, sizeof berr, bnote, sizeof bnote);
            NT_ASSERT(okb && "SELFTEST: blocking reference pack failed");
            const tp_result *rb = gui_pack_result(0);
            NT_ASSERT(rb && rb->sprite_count == sc_a && rb->page_count == pc_a && rb->pages[0].w == pw_a &&
                      rb->pages[0].h == ph_a && "SELFTEST: async vs blocking result mismatch (non-deterministic)");
            nt_log_info("SELFTEST: async==blocking OK (sprites=%d pages=%d page0=%dx%d)", sc_a, pc_a, pw_a, ph_a);
            s_st_phase = 10;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 10) {
        /* Busy-strip overflow (req 6): the Packing.../Cancel strip must fit at the owner matrix. Forced
         * busy (no real worker) so the strip renders its busy tier deterministically. */
        const bool first = s_st_pf < 8;
        const float win_w = first ? 1366.0F : 1024.0F;
        const float win_h = 768.0F;
        g_ui_scale = first ? 1.5F : 2.0F;
        g_nt_window.fb_width = (uint32_t)win_w;
        g_nt_window.fb_height = (uint32_t)win_h;
        gui_pack_debug_force_busy(GUI_PACK_ASYNC_PACK);
        if (s_st_pf == 6 || s_st_pf == 13) { /* size held >= 2 frames -> the lagged bbox reflects the busy strip */
            selftest_assert_no_overflow(win_w, win_h);
            nt_log_info("SELFTEST: busy-strip overflow OK (%.0fx%.0f@%.1f)", (double)win_w, (double)win_h,
                        (double)g_ui_scale);
        }
        if (s_st_pf >= 14) {
            gui_pack_debug_force_busy(GUI_PACK_ASYNC_NONE);
            g_ui_scale = 1.0F;
            g_nt_window.fb_width = 1280;
            g_nt_window.fb_height = 800;
            s_st_phase = 11;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 11) {
        /* Export-target PREVIEW (packet EXP-PREVIEW): a defold preview must (a) exist with identity-only
         * placements (defold caps.flips=false -> the clamp turns allow_transform off -> tp_pack bakes no
         * rotated/flipped regions), (b) leave the native session result untouched (pointer + content),
         * (c) re-bind the native result WITHOUT a repack when switched back to Native, and (d) yield a
         * non-empty degradation summary. Blocking path (the dev seam), mirroring do_pack_blocking. */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_new();
            gui_pack_clear(-1);
            preview_target_reset();
            s_sel_atlas = 0;
            reset_selection();
            tp_project_atlas *a0 = tp_project_get_atlas(gui_project_get(), 0);
            if (a0 && a0->source_count == 0) {
                char afolder[512];
                to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
                (void)gui_project_add_source(0, afolder);
                a0 = tp_project_get_atlas(gui_project_get(), 0); /* F2-05b-i: add_source clone-swapped */
                a0->allow_transform = true; /* guarantee a rotate/flip for the defold clamp to strip (non-empty diff) */
                gui_scan_invalidate_all();
            }
            double nms = 0.0;
            char nerr[256] = {0};
            char nnote[128] = {0};
            const bool okn = gui_pack_atlas(0, &nms, nerr, sizeof nerr, nnote, sizeof nnote);
            const tp_result *native = gui_pack_result(0);
            nt_log_info("SELFTEST: preview native pack -> %d sprites=%d %s", (int)okn, native ? native->sprite_count : -1,
                        okn ? "" : nerr);
            NT_ASSERT(okn && native && native->sprite_count > 0 && "SELFTEST preview: native session pack");
            const void *native_ptr = (const void *)native;
            const int native_sc = native->sprite_count;
            const int native_pc = native->page_count;

            int defold_idx = -1;
            for (int i = 0; i < tp_exporter_count(); i++) {
                const tp_exporter *e = tp_exporter_at(i);
                if (e && strcmp(e->id, "defold") == 0) {
                    defold_idx = i;
                    break;
                }
            }
            NT_ASSERT(defold_idx >= 0 && "SELFTEST preview: defold exporter registered");

            char pverr[256] = {0};
            const bool okp = gui_pack_preview_blocking(0, "defold", pverr, sizeof pverr);
            const tp_result *pv = gui_pack_preview_result(0);
            nt_log_info("SELFTEST: preview defold pack -> %d sprites=%d %s", (int)okp, pv ? pv->sprite_count : -1,
                        okp ? "" : pverr);
            NT_ASSERT(okp && pv && pv->sprite_count > 0 && "SELFTEST preview: defold preview result present");

            /* (a) identity-only placements */
            int nonidentity = 0;
            for (int i = 0; i < pv->sprite_count; i++) {
                if (pv->sprites[i].transform != 0) {
                    nonidentity++;
                }
            }
            nt_log_info("SELFTEST: preview defold non-identity placements=%d (expect 0)", nonidentity);
            NT_ASSERT(nonidentity == 0 && "SELFTEST preview: defold packs identity-only (no flip/rotate)");

            /* (b) native session result untouched */
            const tp_result *native2 = gui_pack_result(0);
            NT_ASSERT((const void *)native2 == native_ptr && native2->sprite_count == native_sc &&
                      native2->page_count == native_pc && "SELFTEST preview: native session result untouched");

            /* (c) preview binds while active; back to Native re-binds the session result with no repack */
            s_preview_target = defold_idx + 1;
            s_preview_ver = gui_project_model_version();
            s_canvas_w = 700.0F; /* single-row tier (>= STRIP_SINGLE_MIN_W) so the preview binds, not compact */
            const tp_result *shown_pv = preview_target_result();
            NT_ASSERT((const void *)shown_pv == (const void *)pv && "SELFTEST preview: preview bound while active");
            preview_target_reset();
            const tp_result *shown_native = preview_target_result();
            NT_ASSERT((const void *)shown_native == native_ptr &&
                      "SELFTEST preview: back to Native re-binds the session result (no repack)");

            /* (d) degradation summary non-empty for defold */
            char chip[96] = {0};
            char tip[224] = {0};
            const int nd = gui_pack_preview_diff(0, "defold", chip, sizeof chip, tip, sizeof tip);
            nt_log_info("SELFTEST: preview defold degradation nd=%d chip='%s'", nd, chip);
            NT_ASSERT(nd > 0 && chip[0] != '\0' && "SELFTEST preview: defold degradation summary non-empty");

            gui_pack_preview_clear();
            preview_target_reset();
            nt_log_info("SELFTEST: export-target preview OK");
            s_st_phase = 12;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 12) {
        /* Async EXPORT (req 4a): mirror phase 9's async==blocking pattern for the export path. Start an
         * async export of a fresh single-atlas project whose seeded target points at an isolated tmp base
         * under the build dir, spin until it lands (poll_async in apply_pending reads the report + frees the
         * job), then assert the on-disk json + page png exist -- the export_worker / save_buffer clone /
         * mkdirs path is otherwise untested (only the blocking gui_pack_export was exercised). */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_new();
            gui_pack_clear(-1);
            s_sel_atlas = 0;
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(0, afolder);
            gui_scan_invalidate_all();
            char base[600];
            (void)snprintf(base, sizeof base, "%s/selftest_async_export/at0", s_exe_dir); /* ABSOLUTE -> resolves w/o a saved dir */
            gui_project_set_target(0, 0, "json-neotolis", base, true);
            char aerr[256] = {0};
            const bool started = gui_pack_export_async_start(aerr, sizeof aerr);
            nt_log_info("SELFTEST: async export start -> %d (%s)", (int)started, started ? "ok" : aerr);
            NT_ASSERT(started && "SELFTEST: async export must start");
        } else if (gui_pack_async_busy()) {
            NT_ASSERT(s_st_pf < 3000 && "SELFTEST: async export did not finish within the frame cap");
        } else {
            char base[600];
            char jpath[640] = {0};
            char ppath[640] = {0};
            (void)snprintf(base, sizeof base, "%s/selftest_async_export/at0", s_exe_dir);
            (void)snprintf(jpath, sizeof jpath, "%s.json", base);
            (void)snprintf(ppath, sizeof ppath, "%s-0.png", base);
            bool jok = false;
            bool pok = false;
            FILE *jf = fopen(jpath, "rb");
            if (jf) {
                jok = (fgetc(jf) == '{'); /* lightweight parse check (full parse is in the packer ctest) */
                (void)fclose(jf);
            }
            FILE *pf = fopen(ppath, "rb");
            if (pf) {
                pok = (fgetc(pf) != EOF); /* exists AND non-empty */
                (void)fclose(pf);
            }
            nt_log_info("SELFTEST: async export landed json{=%d png0=%d", (int)jok, (int)pok);
            NT_ASSERT(jok && pok && "SELFTEST: async export must write the json + page png");
            (void)remove(jpath);
            (void)remove(ppath);
            s_st_phase = 13;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 13) {
        /* Cancel mid-pack (req 4b): start an async pack over a CLEARED slot with stale set, cancel it
         * immediately, spin until it lands. gui_pack_poll must DISCARD the worker's result (no slot swap)
         * and poll_async must NOT clear stale -- the cancel-discard path (gui_pack.c) is otherwise never
         * hit (phase 9 waits for !busy first). */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_new();
            gui_pack_clear(-1);
            s_sel_atlas = 0;
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(0, afolder);
            gui_scan_invalidate_all();
            gui_project_mark_stale();
            char aerr[256] = {0};
            const bool started = gui_pack_async_start(0, aerr, sizeof aerr);
            NT_ASSERT(started && "SELFTEST: cancel-phase pack must start");
            gui_pack_async_cancel(); /* cancel before it can land -> result discarded on landing */
        } else if (gui_pack_async_busy()) {
            NT_ASSERT(s_st_pf < 3000 && "SELFTEST: cancelled pack did not land");
        } else {
            NT_ASSERT(gui_pack_result(0) == NULL && "SELFTEST: cancelled pack must not swap a result in");
            NT_ASSERT(gui_project_is_stale() && "SELFTEST: cancelled pack must leave stale honest (not cleared)");
            nt_log_info("SELFTEST: cancel-mid-pack discarded cleanly (no swap, stale kept)");
            s_st_phase = 14;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 14) {
        /* Mutate-then-land (req 4d): start an async pack, edit the model WHILE it flies, spin until it
         * lands. model_changed_since sees the edit, so poll_async must NOT clear stale (the just-landed
         * result reflects the PRE-edit model). Proves the memcmp-gated mark_packed. */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_new();
            gui_pack_clear(-1);
            s_sel_atlas = 0;
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(0, afolder);
            gui_scan_invalidate_all();
            char aerr[256] = {0};
            const bool started = gui_pack_async_start(0, aerr, sizeof aerr);
            NT_ASSERT(started && "SELFTEST: mutate-phase pack must start");
            tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), 0);
            if (a) {
                a->padding = (a->padding >= 8) ? 1 : (a->padding + 1); /* real model edit -> serialized bytes differ */
                gui_project_touch_setting();
            }
        } else if (gui_pack_async_busy()) {
            NT_ASSERT(s_st_pf < 3000 && "SELFTEST: mutate-phase pack did not land");
        } else {
            NT_ASSERT(gui_project_is_stale() && "SELFTEST: mutate-then-land must keep stale (model changed since pack)");
            nt_log_info("SELFTEST: mutate-then-land kept stale (model_changed honored)");
            s_st_phase = 15;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 15) {
        /* Rename-through-export (A4): a sprite an animation references is renamed; the export must carry the
         * rename into BOTH the sprite name and the animation frame. Mirrors phase 12's async-export driver
         * (isolated tmp base under the build dir). Kept before the teardown phase (16). */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_new();
            gui_pack_clear(-1);
            s_sel_atlas = 0;
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(0, afolder);
            gui_scan_invalidate_all();
            double pms = 0.0;
            char perr[256] = {0};
            char pnote[128] = {0};
            const bool okp = gui_pack_atlas(0, &pms, perr, sizeof perr, pnote, sizeof pnote);
            const tp_result *pr = gui_pack_result(0);
            NT_ASSERT(okp && pr && pr->sprite_count >= 2 && "SELFTEST A4: pack produced >=2 sprites");
            char k0[192];
            char k1[192];
            tp_sprite_export_key(pr->sprites[0].name, k0, sizeof k0); /* frame keys = packed-name export keys */
            tp_sprite_export_key(pr->sprites[1].name, k1, sizeof k1);
            multi_sel_clear();
            multi_sel_add(k0);
            multi_sel_add(k1);
            const int ai = create_animation_from_selection();
            NT_ASSERT(ai >= 0 && "SELFTEST A4: animation from two frames");
            gui_project_set_sprite_rename(0, k0, "a4_renamed"); /* rename one frame's sprite */
            multi_sel_clear();
            char base[600];
            (void)snprintf(base, sizeof base, "%s/selftest_a4_export/at0", s_exe_dir); /* ABSOLUTE -> resolves w/o a saved dir */
            gui_project_set_target(0, 0, "json-neotolis", base, true);
            char aerr[256] = {0};
            const bool started = gui_pack_export_async_start(aerr, sizeof aerr);
            nt_log_info("SELFTEST: A4 rename export start -> %d k0='%s' (%s)", (int)started, k0, started ? "ok" : aerr);
            NT_ASSERT(started && "SELFTEST A4: async export must start");
        } else if (gui_pack_async_busy()) {
            NT_ASSERT(s_st_pf < 3000 && "SELFTEST A4: rename export did not finish within the frame cap");
        } else {
            char base[600];
            char jpath[640] = {0};
            char ppath[640] = {0};
            (void)snprintf(base, sizeof base, "%s/selftest_a4_export/at0", s_exe_dir);
            (void)snprintf(jpath, sizeof jpath, "%s.json", base);
            (void)snprintf(ppath, sizeof ppath, "%s-0.png", base);
            char *js = selftest_slurp(jpath);
            NT_ASSERT(js && "SELFTEST A4: exported json must exist");
            int hits = 0;
            for (const char *p = js; (p = strstr(p, "a4_renamed")) != NULL; p += 10) {
                hits++; /* expect 2: once as the sprite name, once as the animation frame */
            }
            nt_log_info("SELFTEST: A4 rename export landed 'a4_renamed' hits=%d (expect >=2: sprite name + anim frame)", hits);
            NT_ASSERT(hits >= 2 && "SELFTEST A4: rename must appear as the sprite name AND the animation frame it follows");
            free(js);
            (void)remove(jpath);
            (void)remove(ppath);
            s_st_phase = 16;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 16) {
        /* Shutdown-while-busy (req 4c): start an async pack, then gui_pack_shutdown() must cancel + JOIN the
         * worker + free + reset without hanging (the window X-close path). Runs to completion in one frame --
         * gui_pack_shutdown joins synchronously, so afterward the job is idle. main() calls it AGAIN at exit
         * (idempotent -- the second call sees !busy). Kept LAST because it tears the pack session down. */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        reset_selection();
        char afolder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
        (void)gui_project_add_source(0, afolder);
        gui_scan_invalidate_all();
        char aerr[256] = {0};
        const bool started = gui_pack_async_start(0, aerr, sizeof aerr);
        NT_ASSERT(started && gui_pack_async_busy() && "SELFTEST: shutdown-phase pack must start busy");
        gui_pack_shutdown(); /* busy branch: cancel + join + free + reset */
        NT_ASSERT(!gui_pack_async_busy() && "SELFTEST: shutdown-while-busy must join + reset (no hang)");
        gui_shell_reset_shown_result(); /* gui_pack_shutdown cleared the slots -> drop the shell's freed bind ptr */
        nt_log_info("SELFTEST: shutdown-while-busy joined cleanly");
        s_st_phase = 17;
        s_st_pf = 0;
    } else {
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        nt_app_quit();
    }
}

/* Post-walk hook: pixel readbacks happen after nt_ui_walk has drawn the overlay. */
void selftest_post_draw(void) {
    if (s_st_phase != 1) {
        return;
    }
    /* Overlay pixel probe, HARDENED against transient GPU-readback stalls under load (a single-shot read
     * at one fixed frame flaked repeatedly under load today):
     *   (1) SETTLE  -- capture the OFF baseline only at a settled frame (pf 5, several frames after the
     *                  scene + page uploads land), and give the ON outline 2 frames to rasterize before
     *                  the first ON readback (pf 8).
     *   (2) RETRY   -- once outlines are ON, take the readback every frame across a wide window (pf 8..48)
     *                  and PASS the instant one frame clears the cyan-delta threshold. A lone stalled
     *                  readback (delta transiently low) just retries next frame instead of failing the run.
     * The test still FAILS on a real regression: if outlines never rasterize (e.g. the cam-on-plane
     * zero-width-line bug), EVERY frame in the window stays below threshold, the window expires at pf 48,
     * and the assert fires. Observed retries with this scheme: ~0 (passes at pf 8) even under load. */
    if (s_st_pf == 5) {
        s_st_cyan0 = selftest_probe_cyan(); /* settled OFF baseline */
        return;
    }
    if (s_st_pf < 8) {
        return; /* ON settle */
    }
    const int c1 = selftest_probe_cyan();
    const bool ok = (s_st_cyan0 >= 0 && c1 >= 0 && (c1 - s_st_cyan0) >= 8);
    if (!ok && s_st_pf < 48) {
        return; /* transient stall -> retry the readback next frame (outline is still ON) */
    }
    nt_log_info("SELFTEST: outline pixel probe cyan off=%d on=%d delta=%d (settled pf=%d, retries=%d)", s_st_cyan0, c1,
                c1 - s_st_cyan0, (int)s_st_pf, (int)(s_st_pf - 8));
    NT_ASSERT(s_st_cyan0 >= 0 && c1 >= 0);
    NT_ASSERT(ok && "hull outline must add cyan pixels (retry window expired -> outlines never rendered)");
    /* Hand off to the touch-on-render guard: a truly fresh project, no input, all sections expanded. */
    gui_project_new();
    s_sel_atlas = 0;
    reset_selection();
    s_about_open = false;
    s_sec_atlas_open = true;
    s_atlas_adv_open = true;
    s_sec_region_open = true;
    s_sec_anim_open = true;
    s_sec_export_open = true;
    free(s_st_baseline);
    s_st_baseline = NULL;
    s_st_baseline_n = 0;
    tp_error e = {0};
    (void)tp_project_save_buffer(gui_project_get(), &s_st_baseline, &s_st_baseline_n, &e);
    s_st_phase = 2;
    s_st_pf = 0;
}

#else

/* NTPACKER_GUI_SELFTEST off: this TU intentionally compiles to nothing. A file-scope typedef keeps it
 * a legal (non-empty) ISO C translation unit under -Wpedantic. */
typedef int gui_selftest_empty_translation_unit;

#endif /* NTPACKER_GUI_SELFTEST */
