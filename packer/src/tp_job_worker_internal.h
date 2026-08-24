#ifndef TP_JOB_WORKER_INTERNAL_H
#define TP_JOB_WORKER_INTERNAL_H

/* Versioned private protocol for the outer Pack/Export worker process.
 *
 * Frames are little-endian and length-prefixed. The parent and its re-exec'd
 * child share one architecture. Decoders validate every declared length and
 * enum/count field before allocation and never assert on wire bytes.
 *
 * Encoders borrow request/response fields. Successful decoders own every
 * variable-length field; release them with the matching *_free function. No
 * protocol DTO contains tp_session, tp_model, tp_session_snapshot, or mutable
 * result pointers. */

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_job.h"
#include "tp_export_command_report_internal.h"
#include "tp_format_binding_proto_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TP_JOB_WORKER_PROTO_REQUEST_MAGIC 0x574A5450u  /* "PTJW" */
#define TP_JOB_WORKER_PROTO_PROGRESS_MAGIC 0x504A5450u /* "PTJP" */
#define TP_JOB_WORKER_PROTO_FRAGMENT_MAGIC 0x464A5450u /* "PTJF" */
#define TP_JOB_WORKER_PROTO_RESPONSE_MAGIC 0x524A5450u /* "PTJR" */
/* v5: Export terminal aggregates were removed. Export outcomes travel only in
 * bounded fragment frames and the host-owned report is the sole result owner.
 * Parent and child are one re-exec'd binary, so older peers fail by version. */
#define TP_JOB_WORKER_PROTO_VERSION 5u

/* Exact admission caps. Project JSON uses the same cap as project-file
 * identity. Paths and exporter IDs reserve one byte for the decoded NUL. */
#define TP_JOB_WORKER_PROTO_MAX_PROJECT_JSON_BYTES                             \
  ((size_t)TP_IDENTITY_FILE_MAX_BYTES)
#define TP_JOB_WORKER_PROTO_MAX_PATH_BYTES ((size_t)TP_IDENTITY_PATH_MAX - 1U)
#define TP_JOB_WORKER_PROTO_MAX_EXPORTER_ID_BYTES                              \
  ((size_t)TP_EXPORTER_ID_MAX - 1U)
#define TP_JOB_WORKER_PROTO_MAX_FORMAT_BINDING_BYTES                            \
  (TP_FORMAT_BINDING_PROTO_MAX_FRAME_BYTES + 12ULL)
#define TP_JOB_WORKER_PROTO_MAX_ERROR_BYTES 255U
#define TP_JOB_WORKER_PROTO_MAX_RESULT_COUNT 1000000
#define TP_JOB_WORKER_PROTO_MAX_NAME_ENTRIES 65536U
#define TP_JOB_WORKER_PROTO_MAX_NAME_BYTES 4096U
/* Largest Pack artifact the transport admits, and therefore the largest the host
 * will ever allocate in one piece for it (tp_job.c TP_JOB_ARTIFACT_MAX_BYTES).
 * The size arrives as a u64 from a separate process, so without a cap a corrupt
 * or absurd reply turns straight into a multi-GB malloc. 2 GiB is far above any
 * real multi-page atlas and still safely below a 32-bit size_t. */
#define TP_JOB_WORKER_PROTO_MAX_ARTIFACT_BYTES ((uint64_t)2U << 30)
/* No payload is artifact-sized any more, so the frame cap is the largest field
 * the codec still admits (the request's project JSON) plus 16 MiB for every
 * fixed header, path, error and name-map byte around it. Every payload size is
 * accumulated against this cap (add_size), so a Pack name map that overshoots it
 * (>= 65536 sprites averaging 1.25 KiB of name) makes the encoder return a
 * structured OUT_OF_BOUNDS error, never a truncated frame; the worker turns that
 * error into a payload-less FAILED terminal rather than exiting frameless. The
 * stream cap adds one more 16 MiB window for the progress frames preceding the
 * terminal. */
#define TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES                                    \
  (TP_JOB_WORKER_PROTO_MAX_PROJECT_JSON_BYTES +                                \
   (size_t)TP_JOB_WORKER_PROTO_MAX_FORMAT_BINDING_BYTES +                       \
   ((size_t)16U << 20))
#define TP_JOB_WORKER_PROTO_MAX_STREAM_BYTES                                   \
  (TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES + ((size_t)16U << 20))
#define TP_JOB_WORKER_PROTO_MAX_PROGRESS_FRAMES 65536U

typedef struct tp_job_worker_proto_request {
  tp_session_job_kind kind;
  uint64_t session_instance_generation;
  uint64_t request_id;
  /* Process id of the HOST that issued this request, never the worker's own.
   * The per-request `req-` directory outlives the worker (the host adopts the
   * artifact in chunks after the worker exits), so the cross-run reaper must
   * key its liveness check on the host: a worker-pid name made another
   * instance's reaper delete an in-flight artifact. Must be non-zero. */
  uint32_t host_pid;
  /* Canonical schema-v5 JSON whose source paths were made absolute by the
   * host. Bytes need not be NUL-terminated for encode. */
  const uint8_t *project_json;
  size_t project_json_len;
  /* Exact admission-owned binding frame. The worker decodes this snapshot and
   * never reopens the startup formats root or resolves a later handler by ID. */
  const uint8_t *format_bindings;
  size_t format_bindings_len;
  tp_id128 atlas_id;
  /* Canonical project parent used to resolve relative Export out_paths. */
  const char *project_dir;
  const char *work_dir;
  const char *preview_exporter_id;
  const char *target_exporter_id;
  const char *out_dir;
  bool dry_run;
  tp_session_input_token input_token;
} tp_job_worker_proto_request;

typedef struct tp_job_worker_proto_pack_result {
  tp_id128 atlas_id;
  int missing_sources;
  tp_session_input_token input_token_at_start;
  tp_id128 pack_input_hash;
  tp_id128 current_pack_input_hash;
  tp_session_input_token freshness_token;
  tp_pack_freshness freshness;
  tp_pack_freshness_reason freshness_reason;
  tp_status freshness_status;
  tp_error freshness_error;
  char preview_exporter_id[TP_EXPORTER_ID_MAX];
  /* Reverse name map needed for host-side tp_pack_read_memory(). Names are
   * UTF-8, borrowed for encode, and individually owned after decode. It stays on
   * the wire on purpose: `.ntpack` stores hash-only names, so rebuilding the map
   * from the live model would relabel sprites renamed while the Pack ran. */
  const struct tp_job_worker_proto_name *names;
  uint32_t name_count;
  /* Exact path of the child-produced `.ntpack` inside the worker's private
   * per-request directory, plus the byte size the host must observe. Borrowed
   * for encode, owned after decode; empty unless the Pack SUCCEEDED. The bytes
   * never cross the wire: the host validates (regular file + exact size + pack
   * magic), reads them in bounded chunks, and then deletes file and directory.
   * A private directory per request is what keeps a native Pack, a preview Pack
   * and a second app instance from colliding on one `<atlas>.ntpack` name. */
  const char *artifact_path;
  uint64_t artifact_size;
} tp_job_worker_proto_pack_result;

typedef struct tp_job_worker_proto_name {
  uint64_t hash;
  const char *name;
} tp_job_worker_proto_name;

typedef struct tp_job_worker_proto_response {
  tp_session_job_kind kind;
  uint64_t session_instance_generation;
  uint64_t request_id;
  /* A response is terminal: RUNNING is rejected on encode and decode. */
  tp_session_job_state state;
  tp_status status;
  tp_error error;
  double elapsed_ms;
  tp_job_worker_proto_pack_result pack;
} tp_job_worker_proto_response;

typedef enum tp_job_worker_progress_phase {
  TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL = 1,
  TP_JOB_WORKER_PHASE_SOURCE_READ,
  TP_JOB_WORKER_PHASE_IMAGE_DECODE,
  TP_JOB_WORKER_PHASE_PACK_INPUT,
  TP_JOB_WORKER_PHASE_BUILD,
  TP_JOB_WORKER_PHASE_EXPORT_WRITE,
  /* Cross-process terminal linearization: the final eligible Export writer
   * returned and no output side effects remain. */
  TP_JOB_WORKER_PHASE_EXPORT_TERMINAL_BOUNDARY,
  TP_JOB_WORKER_PHASE_EXPORT_SERIALIZE,
  TP_JOB_WORKER_PHASE_EXPORT_READY,
  TP_JOB_WORKER_PHASE_EXPORT_PUBLICATION_BEGIN,
  TP_JOB_WORKER_PHASE_EXPORT_TARGET_COMPLETE,
  /* Private marker emitted only by the worker's lua_atpanic callback. It is
   * valid solely while a Lua target is in serialization. */
  TP_JOB_WORKER_PHASE_EXPORT_HANDLER_PANIC,
  /* Exact worker admission identified the active serializer as Lua. Kept as a
   * distinct wire phase so the panic marker cannot bless a generic crash. */
  TP_JOB_WORKER_PHASE_EXPORT_LUA_SERIALIZE
} tp_job_worker_progress_phase;

typedef struct tp_job_worker_proto_progress {
  uint64_t request_id;
  int current;
  int total;
  tp_job_worker_progress_phase phase;
} tp_job_worker_proto_progress;

typedef struct tp_job_worker_proto_fragment {
  uint64_t request_id;
  tp_export_command_outcome outcome;
} tp_job_worker_proto_fragment;

typedef enum tp_job_worker_stream_message_kind {
  TP_JOB_WORKER_STREAM_PROGRESS = 1,
  TP_JOB_WORKER_STREAM_FRAGMENT,
  TP_JOB_WORKER_STREAM_TERMINAL
} tp_job_worker_stream_message_kind;

typedef struct tp_job_worker_proto_stream_message {
  tp_job_worker_stream_message_kind kind;
  union {
    tp_job_worker_proto_progress progress;
    tp_job_worker_proto_fragment fragment;
    tp_job_worker_proto_response terminal;
  };
} tp_job_worker_proto_stream_message;

typedef struct tp_job_worker_proto_stream {
  uint8_t *bytes;
  size_t length;
  size_t capacity;
  size_t cumulative_bytes;
  uint32_t progress_frames;
  bool terminal_seen;
} tp_job_worker_proto_stream;

tp_status
tp_job_worker_proto_encode_request(const tp_job_worker_proto_request *request,
                                   uint8_t **out_bytes, size_t *out_len,
                                   tp_error *err);
tp_status tp_job_worker_proto_decode_request(const uint8_t *bytes, size_t len,
                                             tp_job_worker_proto_request *out,
                                             tp_error *err);
void tp_job_worker_proto_request_free(tp_job_worker_proto_request *request);

tp_status tp_job_worker_proto_encode_response(
    const tp_job_worker_proto_response *response, uint8_t **out_bytes,
    size_t *out_len, tp_error *err);
tp_status tp_job_worker_proto_decode_response(const uint8_t *bytes, size_t len,
                                              tp_job_worker_proto_response *out,
                                              tp_error *err);
void tp_job_worker_proto_response_free(tp_job_worker_proto_response *response);

tp_status tp_job_worker_proto_encode_progress(
    const tp_job_worker_proto_progress *progress, uint8_t **out_bytes,
    size_t *out_len, tp_error *err);
tp_status tp_job_worker_proto_decode_progress(const uint8_t *bytes, size_t len,
                                              tp_job_worker_proto_progress *out,
                                              tp_error *err);
tp_status tp_job_worker_proto_encode_fragment(
    const tp_job_worker_proto_fragment *fragment, uint8_t **out_bytes,
    size_t *out_len, tp_error *err);
tp_status tp_job_worker_proto_decode_fragment(
    const uint8_t *bytes, size_t len, tp_job_worker_proto_fragment *out,
    tp_error *err);
void tp_job_worker_proto_fragment_free(
    tp_job_worker_proto_fragment *fragment);

void tp_job_worker_proto_stream_init(tp_job_worker_proto_stream *stream);
void tp_job_worker_proto_stream_destroy(tp_job_worker_proto_stream *stream);
tp_status tp_job_worker_proto_stream_feed(tp_job_worker_proto_stream *stream,
                                          const uint8_t *bytes, size_t len,
                                          tp_error *err);
/* Returns OK with *out_ready=false for an incomplete frame. A ready terminal
 * message owns its dynamic fields and must be released with
 * tp_job_worker_proto_stream_message_free(). */
tp_status
tp_job_worker_proto_stream_next(tp_job_worker_proto_stream *stream,
                                tp_job_worker_proto_stream_message *out,
                                bool *out_ready, tp_error *err);
void tp_job_worker_proto_stream_message_free(
    tp_job_worker_proto_stream_message *message);

/* Private service selected by tp_build_worker_main after reading exactly one
 * request frame. Stdin remains open so the service can poll the reserved
 * cancellation byte without a buffered reader consuming it early. */
int tp_job_worker_main_request(const uint8_t *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* TP_JOB_WORKER_INTERNAL_H */
