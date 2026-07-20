#ifndef TP_BUILD_PROTO_INTERNAL_H
#define TP_BUILD_PROTO_INTERNAL_H

/* Versioned bounded private build-worker protocol (decision 0018, master spec
 * §10.6, ROADMAP H0.3). The parent serializes a request -- validated settings,
 * atlas/sprite metadata and raw RGBA8 pixel blocks, plus a relative ASCII output
 * name -- and the worker returns a versioned result. This slice (H0.3-a) defines
 * and pins the codec; the process transport lands in H0.3-b.
 *
 * Wire: little-endian, length-prefixed frames. Fixed headers are #pragma pack(1)
 * and memcpy'd through aligned locals exactly like the .ntpack reader
 * (docs/formats/ntpack-binary.md). The DECODER bounds-checks every field and
 * fails closed on magic/version/size mismatch or a corrupt payload -- it never
 * asserts on wire bytes. Encode + decode round-trip is exact. */

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frame magics read as ASCII in the little-endian byte stream: request bytes
 * "PTBW", response bytes "PTBR" (packer build worker / reply). */
#define TP_BUILD_PROTO_REQUEST_MAGIC 0x57425450u
#define TP_BUILD_PROTO_RESPONSE_MAGIC 0x52425450u
#define TP_BUILD_PROTO_VERSION 1u

/* Fail-closed bounds applied before any allocation. A frame that declares more
 * than the buffer holds, or a field past these caps, is rejected. */
#define TP_BUILD_PROTO_MAX_FRAME_BYTES ((size_t)1u << 31) /* 2 GiB hard cap */
#define TP_BUILD_PROTO_MAX_SPRITES 65535u                 /* mirrors tp_pack sprite_count cap */
#define TP_BUILD_PROTO_MAX_NAME_BYTES 4096u               /* atlas / sprite / artifact / message */
#define TP_BUILD_PROTO_MAX_DIMENSION 16384u               /* mirrors tp_image max dimension */

/* One sprite in a request. Pointers are borrowed for encode and owned after a
 * successful decode (freed by tp_build_proto_request_free). */
typedef struct tp_build_proto_sprite {
    const char *name;    /* NUL-terminated; wire length excludes the NUL */
    uint32_t width;      /* [1..TP_BUILD_PROTO_MAX_DIMENSION] */
    uint32_t height;     /* [1..TP_BUILD_PROTO_MAX_DIMENSION] */
    float origin_x;      /* pivot over source size, y-down */
    float origin_y;
    uint16_t slice9_lrtb[4]; /* [left,right,top,bottom] px; all-zero = none */
    uint8_t ov_mask;         /* TP_PACK_OV_* presence bits */
    uint8_t ov_shape;
    uint8_t ov_allow_rotate;
    uint8_t ov_max_vertices;
    uint8_t ov_margin;
    uint8_t ov_extrude;
    const uint8_t *rgba; /* width*height*4 canonical RGBA8 bytes */
} tp_build_proto_sprite;

/* A pack request: the validated atlas knobs (mirror the tp_pack_settings scalar
 * fields), the atlas display name, the relative ASCII output name, and the
 * sprite/pixel blocks. Never carries arbitrary source paths (decision 0018). */
typedef struct tp_build_proto_request {
    int32_t max_size;
    int32_t padding;
    int32_t margin;
    int32_t extrude;
    int32_t alpha_threshold;
    int32_t max_vertices;
    int32_t shape;
    uint8_t allow_transform;
    uint8_t power_of_two;
    float pixels_per_unit;
    const char *atlas_name; /* NUL-terminated */
    const char *out_name;   /* relative ASCII output name, NUL-terminated */
    const tp_build_proto_sprite *sprites;
    uint32_t sprite_count;
} tp_build_proto_request;

/* A pack result. `status` carries the tp_status the worker resolved (OK,
 * BUILDER_FAILED, or BUILDER_CRASHED synthesized by the parent on a bad reply);
 * `builder_code` is the raw nt_build_result_t for diagnostics. Strings are
 * borrowed for encode and owned after a successful decode. */
typedef struct tp_build_proto_response {
    int32_t status;
    int32_t builder_code;
    const char *artifact_name; /* relative ASCII artifact name ("" if none) */
    const char *message;       /* structured error prose ("" if none) */
} tp_build_proto_response;

/* Encode into a freshly malloc'd buffer (*out_bytes, free with free()). On any
 * failure returns a status, fills `err`, and leaves *out_bytes NULL / *out_len 0. */
tp_status tp_build_proto_encode_request(const tp_build_proto_request *req, uint8_t **out_bytes, size_t *out_len,
                                        tp_error *err);
tp_status tp_build_proto_encode_response(const tp_build_proto_response *resp, uint8_t **out_bytes, size_t *out_len,
                                         tp_error *err);

/* Decode a full frame. On success `out` owns its allocations (release with the
 * matching *_free); on failure returns a structured status, fills `err`, frees
 * any partial allocation, and zeroes `out`. Wrong magic -> BAD_MAGIC,
 * unsupported version -> BAD_VERSION, truncated / oversized-declared-length /
 * corrupt payload -> OUT_OF_BOUNDS or INVALID_ARGUMENT. */
tp_status tp_build_proto_decode_request(const uint8_t *bytes, size_t len, tp_build_proto_request *out, tp_error *err);
tp_status tp_build_proto_decode_response(const uint8_t *bytes, size_t len, tp_build_proto_response *out, tp_error *err);

/* Release allocations owned by a decoded request/response and zero it. NULL-safe
 * and safe on a partially built value. */
void tp_build_proto_request_free(tp_build_proto_request *req);
void tp_build_proto_response_free(tp_build_proto_response *resp);

#ifdef __cplusplus
}
#endif

#endif /* TP_BUILD_PROTO_INTERNAL_H */
