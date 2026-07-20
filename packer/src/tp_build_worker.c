#include "tp_build_worker_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_build_worker.h" /* TP_BUILD_WORKER_ARGV1 */
#include "tp_build_driver_internal.h"
#include "tp_build_proto_internal.h"
#include "tp_proc_internal.h"

/* A valid reply is header(12) + response_head(16) + artifact(<=4096) +
 * message(<=4096) = ~8.2 KiB; 64 KiB is a comfortable bound. A child that emits
 * more than this fills the buffer without EOF and is treated as a malformed,
 * fail-closed reply. */
#define TP_BUILD_WORKER_REPLY_CAP ((size_t)1u << 16)

static void free_loaded_images(tp_image_rgba8 *images, int count) {
    if (!images) {
        return;
    }
    for (int i = 0; i < count; i++) {
        tp_image_free(&images[i]);
    }
    free(images);
}

/* The worker out_name carries the resolved out_path verbatim for now, so it
 * must be pure 7-bit ASCII: the builder opens it through a narrow char path and
 * a non-ASCII name would not round-trip on Windows (that ASCII staging dir is
 * H0.4). Empty is not ASCII-usable. */
static bool path_is_ascii(const char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p >= 0x80u) {
            return false;
        }
    }
    return true;
}

/* The artifact the worker claims to have written must be a readable, non-empty
 * file before we accept the pack. out_path is ASCII in this branch, so a plain
 * fopen is correct (and matches what the builder itself can open). */
static bool artifact_readable(const char *out_path) {
    FILE *f = fopen(out_path, "rb");
    if (!f) {
        return false;
    }
    int c = fgetc(f);
    (void)fclose(f);
    return c != EOF;
}

/* Serialize settings + decoded pixels into a request frame. Path sprites take
 * their pixels from `loaded_images` (index-aligned); raw sprites from the desc.
 * The encoder copies every pixel block, so the caller may free loaded_images
 * immediately afterward. */
static tp_status encode_request(const tp_pack_settings *s,
                                const tp_image_rgba8 *loaded_images,
                                const char *out_path, uint8_t **out_bytes,
                                size_t *out_len, tp_error *err) {
    tp_build_proto_sprite *sprites =
        (tp_build_proto_sprite *)calloc((size_t)s->sprite_count, sizeof *sprites);
    if (!sprites) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_pack: worker request sprite table alloc failed");
    }
    for (int i = 0; i < s->sprite_count; i++) {
        const tp_pack_sprite_desc *sp = &s->sprites[i];
        tp_build_proto_sprite *ps = &sprites[i];
        ps->name = sp->name;
        ps->origin_x = sp->origin_x;
        ps->origin_y = sp->origin_y;
        for (int k = 0; k < 4; k++) {
            ps->slice9_lrtb[k] = sp->slice9_lrtb[k];
        }
        ps->ov_mask = sp->ov_mask;
        ps->ov_shape = sp->ov_shape;
        ps->ov_allow_rotate = sp->ov_allow_rotate;
        ps->ov_max_vertices = sp->ov_max_vertices;
        ps->ov_margin = sp->ov_margin;
        ps->ov_extrude = sp->ov_extrude;
        if (sp->path) {
            ps->width = (uint32_t)loaded_images[i].width;
            ps->height = (uint32_t)loaded_images[i].height;
            ps->rgba = loaded_images[i].pixels;
        } else {
            ps->width = (uint32_t)sp->w;
            ps->height = (uint32_t)sp->h;
            ps->rgba = sp->rgba;
        }
    }

    tp_build_proto_request req;
    memset(&req, 0, sizeof req);
    req.max_size = s->max_size;
    req.padding = s->padding;
    req.margin = s->margin;
    req.extrude = s->extrude;
    req.alpha_threshold = s->alpha_threshold;
    req.max_vertices = s->max_vertices;
    req.shape = s->shape;
    req.allow_transform = s->allow_transform ? 1u : 0u;
    req.power_of_two = s->power_of_two ? 1u : 0u;
    req.pixels_per_unit = s->pixels_per_unit;
    req.atlas_name = s->atlas_name;
    req.out_name = out_path;
    req.sprites = sprites;
    req.sprite_count = (uint32_t)s->sprite_count;

    tp_status st = tp_build_proto_encode_request(&req, out_bytes, out_len, err);
    free(sprites); /* sprite table is borrowed metadata; strings/pixels not owned here */
    return st;
}

/* Map the child's reply + how it terminated onto a tp_status (see the header
 * for the full table). Fills `err` on every non-OK outcome. */
static tp_status map_outcome(const uint8_t *reply, size_t reply_len, bool read_ok,
                             bool reply_eof, bool waited, tp_proc_result w,
                             const char *out_path, tp_error *err) {
    tp_build_proto_response resp;
    bool reply_valid = read_ok && reply_eof &&
                       tp_build_proto_decode_response(reply, reply_len, &resp, NULL) == TP_STATUS_OK;

    const bool exited_zero = waited && w.how == TP_PROC_END_EXITED && w.code == 0;
    const bool exited_nonzero = waited && w.how == TP_PROC_END_EXITED && w.code != 0;
    const bool abnormal = !waited || w.how == TP_PROC_END_ABNORMAL;

    if (reply_valid) {
        tp_status st;
        if (resp.status == TP_STATUS_OK) {
            if (exited_zero && artifact_readable(out_path)) {
                st = TP_STATUS_OK;
            } else {
                st = tp_error_set(err, TP_STATUS_BUILDER_CRASHED,
                                  "tp_pack: build worker reported success but %s",
                                  exited_zero ? "produced no readable artifact"
                                              : "terminated abnormally");
            }
        } else {
            /* The builder ran and resolved a structured failure -- carry it,
             * regardless of the exit code (a fault worker may pair it with a
             * non-zero exit). */
            st = tp_error_set(err, TP_STATUS_BUILDER_FAILED, "tp_pack: build worker failed: %s",
                              (resp.message && resp.message[0]) ? resp.message : "(no detail)");
        }
        tp_build_proto_response_free(&resp);
        return st;
    }

    if (abnormal) {
        return tp_error_set(err, TP_STATUS_BUILDER_CRASHED,
                            "tp_pack: build worker crashed (%s)",
                            waited ? "abnormal termination" : "could not be waited on");
    }
    if (exited_nonzero) {
        return tp_error_set(err, TP_STATUS_BUILDER_CRASHED,
                            "tp_pack: build worker exited with code %d and no valid reply", w.code);
    }
    /* Clean exit but no usable reply -> fail closed. */
    return tp_error_set(err, TP_STATUS_BUILDER_FAILED, "tp_pack: build worker returned a %s reply",
                        (read_ok && !reply_eof) ? "oversized" : "malformed/truncated");
}

tp_status tp_build_worker_run(const tp_pack_settings *settings, tp_image_rgba8 *loaded_images,
                              const char *out_path, tp_error *err) {
    return tp_build_worker_run_exe(settings, loaded_images, out_path, NULL, err);
}

tp_status tp_build_worker_run_exe(const tp_pack_settings *settings, tp_image_rgba8 *loaded_images,
                                  const char *out_path, const char *worker_exe, tp_error *err) {
    /* TEMPORARY H0.3-b: a non-ASCII out_path cannot round-trip through the ASCII
     * worker out_name yet (the ASCII staging dir + tp_fs publish move is H0.4),
     * so fall back to the in-process driver. Identical to the pre-worker path,
     * so no regression -- but ALSO no containment for non-ASCII destinations.
     * REMOVE this branch once H0.4 lands the staging directory. */
    if (!path_is_ascii(out_path)) {
        return tp_build_driver_run(settings, loaded_images, out_path, err);
    }

    /* Resolve the worker executable BEFORE consuming loaded_images, so a
     * resolution failure can still fall back to the in-process driver. */
    char self[4096];
    const char *exe = worker_exe;
    if (!exe) {
        if (!tp_proc_self_path(self, sizeof self)) {
            /* TODO(H0.4): if self cannot be located, containment is unavailable;
             * for now packing must still succeed, so run in-process. */
            return tp_build_driver_run(settings, loaded_images, out_path, err);
        }
        exe = self;
    }

    uint8_t *req_bytes = NULL;
    size_t req_len = 0U;
    tp_status st = encode_request(settings, loaded_images, out_path, &req_bytes, &req_len, err);
    /* The request now owns a copy of every pixel block; release the caller's
     * decode buffers (mirrors the driver freeing before encode/assembly). */
    free_loaded_images(loaded_images, settings->sprite_count);
    if (st != TP_STATUS_OK) {
        return st;
    }

    tp_proc *proc = tp_proc_spawn(exe, TP_BUILD_WORKER_ARGV1);
    if (!proc) {
        free(req_bytes);
        return tp_error_set(err, TP_STATUS_BUILDER_CRASHED, "tp_pack: could not spawn build worker '%s'", exe);
    }

    bool wrote = tp_proc_write_stdin(proc, req_bytes, req_len);
    free(req_bytes);
    (void)wrote; /* a failed write means the child died; the outcome map covers it */

    uint8_t *reply = (uint8_t *)malloc(TP_BUILD_WORKER_REPLY_CAP);
    size_t reply_len = 0U;
    bool reply_eof = false;
    bool read_ok = reply && tp_proc_read_stdout(proc, reply, TP_BUILD_WORKER_REPLY_CAP, &reply_len, &reply_eof);

    tp_proc_result w = {TP_PROC_END_ABNORMAL, -1};
    bool waited = tp_proc_wait(proc, &w);
    tp_proc_destroy(proc);

    st = map_outcome(reply, reply_len, read_ok, reply_eof, waited, w, out_path, err);
    free(reply);
    return st;
}
