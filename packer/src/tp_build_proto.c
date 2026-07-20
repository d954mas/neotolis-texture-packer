#include "tp_build_proto_internal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Fixed wire headers. #pragma pack(1) + memcpy through aligned locals mirrors
 * the .ntpack reader; sizes are _Static_assert-pinned so a field edit that
 * changes the layout fails to build instead of silently shifting the wire. All
 * multi-byte fields are little-endian; the parent and its re-exec'd worker share
 * one architecture, so encode and decode never disagree on byte order. */
#pragma pack(push, 1)
typedef struct proto_frame_header {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved; /* 0; keeps the header explicit and 4-wide aligned */
    uint32_t payload_len;
} proto_frame_header;

typedef struct proto_request_head {
    int32_t max_size;
    int32_t padding;
    int32_t margin;
    int32_t extrude;
    int32_t alpha_threshold;
    int32_t max_vertices;
    int32_t shape;
    uint8_t allow_transform;
    uint8_t power_of_two;
    uint16_t reserved;
    float pixels_per_unit;
    uint32_t atlas_name_len;
    uint32_t out_name_len;
    uint32_t sprite_count;
} proto_request_head;

typedef struct proto_sprite_head {
    uint32_t name_len;
    uint32_t width;
    uint32_t height;
    float origin_x;
    float origin_y;
    uint16_t slice9_lrtb[4];
    uint8_t ov_mask;
    uint8_t ov_shape;
    uint8_t ov_allow_rotate;
    uint8_t ov_max_vertices;
    uint8_t ov_margin;
    uint8_t ov_extrude;
    uint16_t reserved;
    uint32_t rgba_len;
} proto_sprite_head;

typedef struct proto_response_head {
    int32_t status;
    int32_t builder_code;
    uint32_t artifact_len;
    uint32_t message_len;
} proto_response_head;
#pragma pack(pop)

_Static_assert(sizeof(proto_frame_header) == 12, "proto frame header is 12 packed bytes");
_Static_assert(sizeof(proto_request_head) == 48, "proto request head is 48 packed bytes");
_Static_assert(sizeof(proto_sprite_head) == 40, "proto sprite head is 40 packed bytes");
_Static_assert(sizeof(proto_response_head) == 16, "proto response head is 16 packed bytes");

// #region bounded cursors + owned copies
typedef struct proto_rd {
    const uint8_t *base;
    size_t len;
    size_t off; /* invariant: off <= len */
} proto_rd;

/* Copies `n` bytes out; false (no state change) if fewer than `n` remain. The
 * `n > len - off` test is overflow-safe because off <= len always holds. */
static bool rd_bytes(proto_rd *r, void *dst, size_t n) {
    if (n > r->len - r->off) {
        return false;
    }
    memcpy(dst, r->base + r->off, n);
    r->off += n;
    return true;
}

static bool rd_ref(proto_rd *r, const uint8_t **out, size_t n) {
    if (n > r->len - r->off) {
        return false;
    }
    *out = r->base + r->off;
    r->off += n;
    return true;
}

static char *dup_cstr(const uint8_t *src, size_t n) {
    char *s = (char *)malloc(n + 1U);
    if (!s) {
        return NULL;
    }
    if (n) {
        memcpy(s, src, n);
    }
    s[n] = '\0';
    return s;
}

static uint8_t *dup_bytes(const uint8_t *src, size_t n) {
    uint8_t *b = (uint8_t *)malloc(n ? n : 1U);
    if (!b) {
        return NULL;
    }
    if (n) {
        memcpy(b, src, n);
    }
    return b;
}
// #endregion

void tp_build_proto_request_free(tp_build_proto_request *req) {
    if (!req) {
        return;
    }
    free((void *)req->atlas_name);
    free((void *)req->out_name);
    if (req->sprites) {
        for (uint32_t i = 0; i < req->sprite_count; i++) {
            free((void *)req->sprites[i].name);
            free((void *)req->sprites[i].rgba);
        }
        free((void *)req->sprites);
    }
    memset(req, 0, sizeof *req);
}

void tp_build_proto_response_free(tp_build_proto_response *resp) {
    if (!resp) {
        return;
    }
    free((void *)resp->artifact_name);
    free((void *)resp->message);
    memset(resp, 0, sizeof *resp);
}

// #region encode
tp_status tp_build_proto_encode_request(const tp_build_proto_request *req, uint8_t **out_bytes, size_t *out_len,
                                        tp_error *err) {
    if (out_bytes) {
        *out_bytes = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (!req || !out_bytes || !out_len) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: encode NULL argument");
    }
    if (!req->atlas_name || !req->out_name) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: atlas_name and out_name are required");
    }
    if (req->sprite_count > TP_BUILD_PROTO_MAX_SPRITES) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: sprite_count %u exceeds %u",
                            (unsigned)req->sprite_count, (unsigned)TP_BUILD_PROTO_MAX_SPRITES);
    }
    if (req->sprite_count > 0 && !req->sprites) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: sprites is NULL");
    }
    size_t atlas_len = strlen(req->atlas_name);
    size_t out_name_len = strlen(req->out_name);
    if (atlas_len > TP_BUILD_PROTO_MAX_NAME_BYTES || out_name_len > TP_BUILD_PROTO_MAX_NAME_BYTES) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: atlas/out name length exceeds cap");
    }

    /* Size pass: accumulate the payload with overflow guards and per-sprite
     * field validation (dimensions/name length) so encode fails closed too. */
    size_t payload = sizeof(proto_request_head) + atlas_len + out_name_len;
    for (uint32_t i = 0; i < req->sprite_count; i++) {
        const tp_build_proto_sprite *sp = &req->sprites[i];
        if (!sp->name) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: sprite %u name is NULL", (unsigned)i);
        }
        if (!sp->rgba) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: sprite %u rgba is NULL", (unsigned)i);
        }
        if (sp->width == 0 || sp->width > TP_BUILD_PROTO_MAX_DIMENSION || sp->height == 0 ||
            sp->height > TP_BUILD_PROTO_MAX_DIMENSION) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: sprite %u size %ux%u out of range",
                                (unsigned)i, (unsigned)sp->width, (unsigned)sp->height);
        }
        size_t name_len = strlen(sp->name);
        if (name_len > TP_BUILD_PROTO_MAX_NAME_BYTES) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: sprite %u name length exceeds cap",
                                (unsigned)i);
        }
        size_t rgba_len = (size_t)sp->width * (size_t)sp->height * 4U; /* <= 2^30, bounded above */
        size_t add = sizeof(proto_sprite_head);
        if (add > SIZE_MAX - name_len || (add += name_len) > SIZE_MAX - rgba_len ||
            (add += rgba_len) > SIZE_MAX - payload) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: request size overflows");
        }
        payload += add;
    }
    if (payload > TP_BUILD_PROTO_MAX_FRAME_BYTES) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: request payload %zu exceeds cap", payload);
    }
    size_t total = sizeof(proto_frame_header) + payload;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_build_proto: request buffer alloc failed (%zu bytes)", total);
    }

    size_t off = 0;
    proto_frame_header fh = {TP_BUILD_PROTO_REQUEST_MAGIC, (uint16_t)TP_BUILD_PROTO_VERSION, 0U, (uint32_t)payload};
    memcpy(buf + off, &fh, sizeof fh);
    off += sizeof fh;

    proto_request_head rh;
    memset(&rh, 0, sizeof rh);
    rh.max_size = req->max_size;
    rh.padding = req->padding;
    rh.margin = req->margin;
    rh.extrude = req->extrude;
    rh.alpha_threshold = req->alpha_threshold;
    rh.max_vertices = req->max_vertices;
    rh.shape = req->shape;
    rh.allow_transform = req->allow_transform;
    rh.power_of_two = req->power_of_two;
    rh.pixels_per_unit = req->pixels_per_unit;
    rh.atlas_name_len = (uint32_t)atlas_len;
    rh.out_name_len = (uint32_t)out_name_len;
    rh.sprite_count = req->sprite_count;
    memcpy(buf + off, &rh, sizeof rh);
    off += sizeof rh;
    memcpy(buf + off, req->atlas_name, atlas_len);
    off += atlas_len;
    memcpy(buf + off, req->out_name, out_name_len);
    off += out_name_len;

    for (uint32_t i = 0; i < req->sprite_count; i++) {
        const tp_build_proto_sprite *sp = &req->sprites[i];
        size_t name_len = strlen(sp->name);
        size_t rgba_len = (size_t)sp->width * (size_t)sp->height * 4U;
        proto_sprite_head sh;
        memset(&sh, 0, sizeof sh);
        sh.name_len = (uint32_t)name_len;
        sh.width = sp->width;
        sh.height = sp->height;
        sh.origin_x = sp->origin_x;
        sh.origin_y = sp->origin_y;
        for (int k = 0; k < 4; k++) {
            sh.slice9_lrtb[k] = sp->slice9_lrtb[k];
        }
        sh.ov_mask = sp->ov_mask;
        sh.ov_shape = sp->ov_shape;
        sh.ov_allow_rotate = sp->ov_allow_rotate;
        sh.ov_max_vertices = sp->ov_max_vertices;
        sh.ov_margin = sp->ov_margin;
        sh.ov_extrude = sp->ov_extrude;
        sh.rgba_len = (uint32_t)rgba_len;
        memcpy(buf + off, &sh, sizeof sh);
        off += sizeof sh;
        memcpy(buf + off, sp->name, name_len);
        off += name_len;
        memcpy(buf + off, sp->rgba, rgba_len);
        off += rgba_len;
    }

    *out_bytes = buf;
    *out_len = total;
    return TP_STATUS_OK;
}

tp_status tp_build_proto_encode_response(const tp_build_proto_response *resp, uint8_t **out_bytes, size_t *out_len,
                                         tp_error *err) {
    if (out_bytes) {
        *out_bytes = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (!resp || !out_bytes || !out_len) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: encode NULL argument");
    }
    const char *artifact = resp->artifact_name ? resp->artifact_name : "";
    const char *message = resp->message ? resp->message : "";
    size_t artifact_len = strlen(artifact);
    size_t message_len = strlen(message);
    if (artifact_len > TP_BUILD_PROTO_MAX_NAME_BYTES || message_len > TP_BUILD_PROTO_MAX_NAME_BYTES) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: artifact/message length exceeds cap");
    }
    size_t payload = sizeof(proto_response_head) + artifact_len + message_len;
    size_t total = sizeof(proto_frame_header) + payload;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_build_proto: response buffer alloc failed (%zu bytes)", total);
    }
    size_t off = 0;
    proto_frame_header fh = {TP_BUILD_PROTO_RESPONSE_MAGIC, (uint16_t)TP_BUILD_PROTO_VERSION, 0U, (uint32_t)payload};
    memcpy(buf + off, &fh, sizeof fh);
    off += sizeof fh;
    proto_response_head rh;
    memset(&rh, 0, sizeof rh);
    rh.status = resp->status;
    rh.builder_code = resp->builder_code;
    rh.artifact_len = (uint32_t)artifact_len;
    rh.message_len = (uint32_t)message_len;
    memcpy(buf + off, &rh, sizeof rh);
    off += sizeof rh;
    memcpy(buf + off, artifact, artifact_len);
    off += artifact_len;
    memcpy(buf + off, message, message_len);
    off += message_len;

    *out_bytes = buf;
    *out_len = total;
    return TP_STATUS_OK;
}
// #endregion

// #region decode
/* Reads and checks the frame header, returning the payload cursor. */
static tp_status decode_frame(const uint8_t *bytes, size_t len, uint32_t expect_magic, proto_rd *out_reader,
                              tp_error *err) {
    if (!bytes) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: decode NULL bytes");
    }
    if (len < sizeof(proto_frame_header)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: frame %zu bytes < header %zu", len,
                            sizeof(proto_frame_header));
    }
    proto_frame_header fh;
    memcpy(&fh, bytes, sizeof fh);
    if (fh.magic != expect_magic) {
        return tp_error_set(err, TP_STATUS_BAD_MAGIC, "tp_build_proto: magic 0x%08x != 0x%08x", (unsigned)fh.magic,
                            (unsigned)expect_magic);
    }
    if (fh.version != TP_BUILD_PROTO_VERSION) {
        return tp_error_set(err, TP_STATUS_BAD_VERSION, "tp_build_proto: version %u != %u", (unsigned)fh.version,
                            (unsigned)TP_BUILD_PROTO_VERSION);
    }
    size_t payload = len - sizeof(proto_frame_header);
    if (fh.payload_len != payload) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_build_proto: declared payload %u != %zu available (truncated/oversized)",
                            (unsigned)fh.payload_len, payload);
    }
    if (payload > TP_BUILD_PROTO_MAX_FRAME_BYTES) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: payload %zu exceeds cap", payload);
    }
    out_reader->base = bytes + sizeof(proto_frame_header);
    out_reader->len = payload;
    out_reader->off = 0;
    return TP_STATUS_OK;
}

tp_status tp_build_proto_decode_request(const uint8_t *bytes, size_t len, tp_build_proto_request *out, tp_error *err) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: decode NULL out");
    }
    proto_rd r;
    tp_status st = decode_frame(bytes, len, TP_BUILD_PROTO_REQUEST_MAGIC, &r, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    proto_request_head rh;
    if (!rd_bytes(&r, &rh, sizeof rh)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: request head overruns payload");
    }
    if (rh.atlas_name_len > TP_BUILD_PROTO_MAX_NAME_BYTES || rh.out_name_len > TP_BUILD_PROTO_MAX_NAME_BYTES) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: atlas/out name length exceeds cap");
    }
    if (rh.sprite_count > TP_BUILD_PROTO_MAX_SPRITES) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: sprite_count %u exceeds %u",
                            (unsigned)rh.sprite_count, (unsigned)TP_BUILD_PROTO_MAX_SPRITES);
    }

    out->max_size = rh.max_size;
    out->padding = rh.padding;
    out->margin = rh.margin;
    out->extrude = rh.extrude;
    out->alpha_threshold = rh.alpha_threshold;
    out->max_vertices = rh.max_vertices;
    out->shape = rh.shape;
    out->allow_transform = rh.allow_transform;
    out->power_of_two = rh.power_of_two;
    out->pixels_per_unit = rh.pixels_per_unit;
    out->sprite_count = rh.sprite_count;

    const uint8_t *ref = NULL;
    if (!rd_ref(&r, &ref, rh.atlas_name_len)) {
        st = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: atlas_name overruns payload");
        goto fail;
    }
    out->atlas_name = dup_cstr(ref, rh.atlas_name_len);
    if (!out->atlas_name) {
        st = tp_error_set(err, TP_STATUS_OOM, "tp_build_proto: atlas_name alloc failed");
        goto fail;
    }
    if (!rd_ref(&r, &ref, rh.out_name_len)) {
        st = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: out_name overruns payload");
        goto fail;
    }
    out->out_name = dup_cstr(ref, rh.out_name_len);
    if (!out->out_name) {
        st = tp_error_set(err, TP_STATUS_OOM, "tp_build_proto: out_name alloc failed");
        goto fail;
    }

    if (rh.sprite_count > 0) {
        tp_build_proto_sprite *arr = (tp_build_proto_sprite *)calloc(rh.sprite_count, sizeof *arr);
        if (!arr) {
            st = tp_error_set(err, TP_STATUS_OOM, "tp_build_proto: sprite table alloc failed");
            goto fail;
        }
        out->sprites = arr; /* now owned + freeable (entries zero-initialized) */
        for (uint32_t i = 0; i < rh.sprite_count; i++) {
            proto_sprite_head sh;
            if (!rd_bytes(&r, &sh, sizeof sh)) {
                st = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: sprite %u head overruns payload",
                                  (unsigned)i);
                goto fail;
            }
            if (sh.name_len > TP_BUILD_PROTO_MAX_NAME_BYTES) {
                st = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: sprite %u name length exceeds cap",
                                  (unsigned)i);
                goto fail;
            }
            if (sh.width == 0 || sh.width > TP_BUILD_PROTO_MAX_DIMENSION || sh.height == 0 ||
                sh.height > TP_BUILD_PROTO_MAX_DIMENSION) {
                st = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: sprite %u size %ux%u out of range",
                                  (unsigned)i, (unsigned)sh.width, (unsigned)sh.height);
                goto fail;
            }
            uint64_t expect_rgba = (uint64_t)sh.width * (uint64_t)sh.height * 4U;
            if ((uint64_t)sh.rgba_len != expect_rgba) {
                st = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                  "tp_build_proto: sprite %u rgba_len %u != %llu for %ux%u", (unsigned)i,
                                  (unsigned)sh.rgba_len, (unsigned long long)expect_rgba, (unsigned)sh.width,
                                  (unsigned)sh.height);
                goto fail;
            }
            arr[i].width = sh.width;
            arr[i].height = sh.height;
            arr[i].origin_x = sh.origin_x;
            arr[i].origin_y = sh.origin_y;
            for (int k = 0; k < 4; k++) {
                arr[i].slice9_lrtb[k] = sh.slice9_lrtb[k];
            }
            arr[i].ov_mask = sh.ov_mask;
            arr[i].ov_shape = sh.ov_shape;
            arr[i].ov_allow_rotate = sh.ov_allow_rotate;
            arr[i].ov_max_vertices = sh.ov_max_vertices;
            arr[i].ov_margin = sh.ov_margin;
            arr[i].ov_extrude = sh.ov_extrude;
            if (!rd_ref(&r, &ref, sh.name_len)) {
                st = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: sprite %u name overruns payload",
                                  (unsigned)i);
                goto fail;
            }
            arr[i].name = dup_cstr(ref, sh.name_len);
            if (!arr[i].name) {
                st = tp_error_set(err, TP_STATUS_OOM, "tp_build_proto: sprite %u name alloc failed", (unsigned)i);
                goto fail;
            }
            if (!rd_ref(&r, &ref, sh.rgba_len)) {
                st = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: sprite %u pixels overrun payload",
                                  (unsigned)i);
                goto fail;
            }
            arr[i].rgba = dup_bytes(ref, sh.rgba_len);
            if (!arr[i].rgba) {
                st = tp_error_set(err, TP_STATUS_OOM, "tp_build_proto: sprite %u pixels alloc failed", (unsigned)i);
                goto fail;
            }
        }
    }

    if (r.off != r.len) {
        st = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: %zu trailing request bytes", r.len - r.off);
        goto fail;
    }
    return TP_STATUS_OK;

fail:
    tp_build_proto_request_free(out);
    return st;
}

tp_status tp_build_proto_decode_response(const uint8_t *bytes, size_t len, tp_build_proto_response *out, tp_error *err) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: decode NULL out");
    }
    proto_rd r;
    tp_status st = decode_frame(bytes, len, TP_BUILD_PROTO_RESPONSE_MAGIC, &r, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    proto_response_head rh;
    if (!rd_bytes(&r, &rh, sizeof rh)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: response head overruns payload");
    }
    if (rh.artifact_len > TP_BUILD_PROTO_MAX_NAME_BYTES || rh.message_len > TP_BUILD_PROTO_MAX_NAME_BYTES) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_build_proto: artifact/message length exceeds cap");
    }
    out->status = rh.status;
    out->builder_code = rh.builder_code;

    const uint8_t *ref = NULL;
    if (!rd_ref(&r, &ref, rh.artifact_len)) {
        st = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: artifact_name overruns payload");
        goto fail;
    }
    out->artifact_name = dup_cstr(ref, rh.artifact_len);
    if (!out->artifact_name) {
        st = tp_error_set(err, TP_STATUS_OOM, "tp_build_proto: artifact_name alloc failed");
        goto fail;
    }
    if (!rd_ref(&r, &ref, rh.message_len)) {
        st = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: message overruns payload");
        goto fail;
    }
    out->message = dup_cstr(ref, rh.message_len);
    if (!out->message) {
        st = tp_error_set(err, TP_STATUS_OOM, "tp_build_proto: message alloc failed");
        goto fail;
    }
    if (r.off != r.len) {
        st = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_build_proto: %zu trailing response bytes", r.len - r.off);
        goto fail;
    }
    return TP_STATUS_OK;

fail:
    tp_build_proto_response_free(out);
    return st;
}
// #endregion
