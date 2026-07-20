#include "tp_core/tp_build_worker.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include "log/nt_log.h"

#include "tp_build_driver_internal.h"
#include "tp_build_proto_internal.h"

/* The child reads at most one request frame: header(12) + payload. The proto
 * caps the payload at TP_BUILD_PROTO_MAX_FRAME_BYTES; anything larger is a
 * malformed request the child rejects before allocating for it. */
#define TP_BUILD_WORKER_REQUEST_CAP (TP_BUILD_PROTO_MAX_FRAME_BYTES + 16u)

bool tp_build_is_worker_invocation(int argc, char **argv) {
    return argc >= 2 && argv && argv[1] && strcmp(argv[1], TP_BUILD_WORKER_ARGV1) == 0;
}

/* The worker exists to die silently when the builder faults: no WER "has
 * stopped working" dialog, no Debug-CRT abort message box -- an interactive
 * prompt would block the parent until its safety timeout instead of yielding
 * an immediate builder_crashed. Error mode is inherited from the parent, so
 * a launcher that re-enables dialogs (ctest does) would otherwise wedge every
 * crashing worker on an invisible dialog. */
static void suppress_fault_dialogs(void) {
#if defined(_WIN32)
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    (void)_set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

/* Read stdin to EOF into a freshly malloc'd buffer, capped at `cap`. The parent
 * closes the child's stdin after the request, so EOF is the frame boundary.
 * Returns TP_STATUS_OK with an owned buffer, or a fail-closed status. */
static tp_status read_stdin_all(uint8_t **out_bytes, size_t *out_len, tp_error *err) {
    *out_bytes = NULL;
    *out_len = 0U;
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY); /* no CRLF translation of binary frames */
#endif
    size_t cap = 1u << 16;
    size_t len = 0U;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        return tp_error_set(err, TP_STATUS_OOM, "build worker: request buffer alloc failed");
    }
    for (;;) {
        if (len == cap) {
            if (cap >= TP_BUILD_WORKER_REQUEST_CAP) {
                free(buf);
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "build worker: request exceeds the frame cap");
            }
            size_t next = cap * 2u;
            if (next > TP_BUILD_WORKER_REQUEST_CAP) {
                next = TP_BUILD_WORKER_REQUEST_CAP;
            }
            uint8_t *grown = (uint8_t *)realloc(buf, next);
            if (!grown) {
                free(buf);
                return tp_error_set(err, TP_STATUS_OOM, "build worker: request buffer grow failed");
            }
            buf = grown;
            cap = next;
        }
        size_t got = fread(buf + len, 1u, cap - len, stdin);
        len += got;
        if (got == 0U) {
            break; /* EOF or error: either way the frame is complete or invalid */
        }
    }
    *out_bytes = buf;
    *out_len = len;
    return TP_STATUS_OK;
}

/* Emit one response frame to stdout. Best effort: returns true if the whole
 * frame was written. */
static bool write_response(tp_status status, const char *artifact, const char *message) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    tp_build_proto_response resp;
    memset(&resp, 0, sizeof resp);
    resp.status = (int32_t)status;
    resp.builder_code = 0;
    resp.artifact_name = artifact ? artifact : "";
    resp.message = message ? message : "";

    uint8_t *bytes = NULL;
    size_t len = 0U;
    if (tp_build_proto_encode_response(&resp, &bytes, &len, NULL) != TP_STATUS_OK) {
        return false;
    }
    bool ok = fwrite(bytes, 1u, len, stdout) == len && fflush(stdout) == 0;
    free(bytes);
    return ok;
}

/* Rebuild tp_pack_settings + a raw-pixel sprite table from a decoded request and
 * pack it through the shared driver. All sprites are raw (path == NULL), so the
 * driver uses the request pixels directly and loaded_images stays NULL. */
static tp_status pack_request(const tp_build_proto_request *req, tp_error *err) {
    tp_pack_sprite_desc *descs =
        (tp_pack_sprite_desc *)calloc(req->sprite_count ? req->sprite_count : 1u, sizeof *descs);
    if (!descs) {
        return tp_error_set(err, TP_STATUS_OOM, "build worker: sprite desc table alloc failed");
    }
    for (uint32_t i = 0; i < req->sprite_count; i++) {
        const tp_build_proto_sprite *ps = &req->sprites[i];
        tp_pack_sprite_desc *d = &descs[i];
        d->name = ps->name;
        d->path = NULL;
        d->rgba = ps->rgba;
        d->w = (int)ps->width;
        d->h = (int)ps->height;
        d->origin_x = ps->origin_x;
        d->origin_y = ps->origin_y;
        for (int k = 0; k < 4; k++) {
            d->slice9_lrtb[k] = ps->slice9_lrtb[k];
        }
        d->ov_mask = ps->ov_mask;
        d->ov_shape = ps->ov_shape;
        d->ov_allow_rotate = ps->ov_allow_rotate;
        d->ov_max_vertices = ps->ov_max_vertices;
        d->ov_margin = ps->ov_margin;
        d->ov_extrude = ps->ov_extrude;
    }

    tp_pack_settings s;
    memset(&s, 0, sizeof s);
    s.atlas_name = req->atlas_name;
    s.work_dir = "."; /* unused: the driver writes to the resolved out_name */
    s.sprites = descs;
    s.sprite_count = (int)req->sprite_count;
    s.max_size = req->max_size;
    s.padding = req->padding;
    s.margin = req->margin;
    s.extrude = req->extrude;
    s.alpha_threshold = req->alpha_threshold;
    s.max_vertices = req->max_vertices;
    s.shape = req->shape;
    s.allow_transform = req->allow_transform != 0u;
    s.power_of_two = req->power_of_two != 0u;
    s.pixels_per_unit = req->pixels_per_unit;

    /* loaded_images == NULL: every sprite is raw, so the driver never indexes it
     * and its free-of-NULL is a no-op (the request owns the pixels). */
    tp_status st = tp_build_driver_run(&s, NULL, req->out_name, err);
    free(descs);
    return st;
}

int tp_build_worker_main(void) {
    suppress_fault_dialogs();
    /* Keep stdout a pure response channel: nt_log INFO defaults to stdout and
     * would corrupt the frame, so silence logging entirely for the worker. */
    nt_log_set_level(NT_LOG_LEVEL_NONE);

    tp_error err = {{0}};
    uint8_t *req_bytes = NULL;
    size_t req_len = 0U;
    tp_status st = read_stdin_all(&req_bytes, &req_len, &err);
    if (st != TP_STATUS_OK) {
        (void)write_response(st, "", err.msg);
        free(req_bytes);
        return 0; /* a reply was emitted; the parent maps it (BUILDER_FAILED) */
    }

    tp_build_proto_request req;
    st = tp_build_proto_decode_request(req_bytes, req_len, &req, &err);
    free(req_bytes);
    if (st != TP_STATUS_OK) {
        (void)write_response(st, "", err.msg);
        return 0;
    }

    st = pack_request(&req, &err);
    bool wrote = write_response(st, st == TP_STATUS_OK ? req.out_name : "",
                                st == TP_STATUS_OK ? "" : err.msg);
    tp_build_proto_request_free(&req);
    return wrote ? 0 : 1; /* non-zero only if the reply itself could not be sent */
}
