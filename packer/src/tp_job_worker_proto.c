#include "tp_job_worker_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tp_utf8_internal.h"

enum {
  FRAME_HEADER_BYTES = 12,
  REQUEST_FIXED_BYTES = 72,
  RESPONSE_COMMON_BYTES = 52,
  RESPONSE_PACK_BYTES = 132,
  RESPONSE_EXPORT_BYTES = 36,
  PROGRESS_PAYLOAD_BYTES = 24
};

_Static_assert(TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES <= UINT32_MAX,
               "job-worker frame length must fit u32");

typedef struct wire_writer {
  uint8_t *bytes;
  size_t offset;
} wire_writer;

typedef struct wire_reader {
  const uint8_t *bytes;
  size_t length;
  size_t offset;
} wire_reader;

static void wr_u16(wire_writer *writer, uint16_t value) {
  writer->bytes[writer->offset++] = (uint8_t)value;
  writer->bytes[writer->offset++] = (uint8_t)(value >> 8U);
}

static void wr_u32(wire_writer *writer, uint32_t value) {
  for (unsigned int i = 0U; i < 4U; ++i) {
    writer->bytes[writer->offset++] = (uint8_t)(value >> (i * 8U));
  }
}

static void wr_i32(wire_writer *writer, int32_t value) {
  wr_u32(writer, (uint32_t)value);
}

static void wr_u64(wire_writer *writer, uint64_t value) {
  for (unsigned int i = 0U; i < 8U; ++i) {
    writer->bytes[writer->offset++] = (uint8_t)(value >> (i * 8U));
  }
}

static void wr_bytes(wire_writer *writer, const void *bytes, size_t length) {
  if (length > 0U) {
    memcpy(writer->bytes + writer->offset, bytes, length);
    writer->offset += length;
  }
}

static bool rd_u16(wire_reader *reader, uint16_t *out) {
  if (reader->length - reader->offset < 2U) {
    return false;
  }
  const uint8_t *bytes = reader->bytes + reader->offset;
  *out = (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
  reader->offset += 2U;
  return true;
}

static bool rd_u32(wire_reader *reader, uint32_t *out) {
  if (reader->length - reader->offset < 4U) {
    return false;
  }
  const uint8_t *bytes = reader->bytes + reader->offset;
  *out = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
         ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
  reader->offset += 4U;
  return true;
}

static bool rd_i32(wire_reader *reader, int32_t *out) {
  uint32_t value = 0U;
  if (!rd_u32(reader, &value)) {
    return false;
  }
  *out = (int32_t)value;
  return true;
}

static bool rd_u64(wire_reader *reader, uint64_t *out) {
  if (reader->length - reader->offset < 8U) {
    return false;
  }
  uint64_t value = 0U;
  for (unsigned int i = 0U; i < 8U; ++i) {
    value |= (uint64_t)reader->bytes[reader->offset + i] << (i * 8U);
  }
  reader->offset += 8U;
  *out = value;
  return true;
}

static bool rd_bytes(wire_reader *reader, void *out, size_t length) {
  if (length > reader->length - reader->offset) {
    return false;
  }
  if (length > 0U) {
    memcpy(out, reader->bytes + reader->offset, length);
    reader->offset += length;
  }
  return true;
}

static bool rd_ref(wire_reader *reader, const uint8_t **out, size_t length) {
  if (length > reader->length - reader->offset) {
    return false;
  }
  *out = reader->bytes + reader->offset;
  reader->offset += length;
  return true;
}

/* Payload accumulator bounded by the frame cap rather than by SIZE_MAX: every
 * caller turns a false into a structured OUT_OF_BOUNDS error, so an oversized
 * name map or path is rejected at the field that overshoots instead of relying
 * on make_frame to catch the total. */
static bool add_size(size_t *value, size_t addition) {
  if (*value > TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES ||
      addition > TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES - *value) {
    return false;
  }
  *value += addition;
  return true;
}

static bool bounded_cstr_length(const char *text, size_t cap,
                                size_t *out_length) {
  if (!text) {
    return false;
  }
  for (size_t i = 0U; i <= cap; ++i) {
    if (text[i] == '\0') {
      *out_length = i;
      return true;
    }
  }
  return false;
}

static bool valid_job_kind(tp_session_job_kind kind) {
  return kind == TP_SESSION_JOB_PACK || kind == TP_SESSION_JOB_EXPORT;
}

static bool id_is_nil(tp_id128 id) {
  uint8_t folded = 0U;
  for (size_t i = 0U; i < sizeof id.bytes; ++i) {
    folded |= id.bytes[i];
  }
  return folded == 0U;
}

static bool valid_terminal_state(tp_session_job_state state) {
  return state == TP_SESSION_JOB_SUCCEEDED || state == TP_SESSION_JOB_FAILED ||
         state == TP_SESSION_JOB_CANCELLED;
}

static bool valid_status(int32_t status) {
  return status >= (int32_t)TP_STATUS_OK &&
         status <= (int32_t)TP_STATUS_CANCELLED;
}

static bool valid_file_phase(int32_t phase) {
  return phase >= (int32_t)TP_FILE_IO_PHASE_NONE &&
         phase <= (int32_t)TP_FILE_IO_PHASE_ATOMIC_CREATE;
}

static bool valid_progress_phase(int32_t phase) {
  return phase >= (int32_t)TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL &&
         phase <=
             (int32_t)TP_JOB_WORKER_PHASE_EXPORT_TERMINAL_BOUNDARY;
}

static bool valid_count(int32_t count) {
  return count >= 0 && count <= TP_JOB_WORKER_PROTO_MAX_RESULT_COUNT;
}

static tp_status validate_terminal_semantics(tp_session_job_state state,
                                             tp_status status, tp_error *err) {
  if (!valid_terminal_state(state) || !valid_status((int32_t)status)) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: invalid terminal state/status");
  }
  if ((state == TP_SESSION_JOB_SUCCEEDED && status != TP_STATUS_OK) ||
      (state == TP_SESSION_JOB_FAILED && status == TP_STATUS_OK) ||
      (state == TP_SESSION_JOB_CANCELLED && status != TP_STATUS_CANCELLED)) {
    return tp_error_set(
        err, TP_STATUS_INVALID_ARGUMENT,
        "tp_job_worker_proto: inconsistent terminal state/status");
  }
  return TP_STATUS_OK;
}

static tp_status make_frame(uint32_t magic, size_t payload_length,
                            uint8_t **out_bytes, size_t *out_length,
                            wire_writer *out_writer, tp_error *err) {
  if (payload_length > TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES ||
      payload_length > UINT32_MAX ||
      payload_length > SIZE_MAX - FRAME_HEADER_BYTES) {
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: frame payload exceeds cap");
  }
  const size_t total = FRAME_HEADER_BYTES + payload_length;
  uint8_t *bytes = (uint8_t *)malloc(total);
  if (!bytes) {
    return tp_error_set(err, TP_STATUS_OOM,
                        "tp_job_worker_proto: frame allocation failed");
  }
  wire_writer writer = {bytes, 0U};
  wr_u32(&writer, magic);
  wr_u16(&writer, (uint16_t)TP_JOB_WORKER_PROTO_VERSION);
  wr_u16(&writer, 0U);
  wr_u32(&writer, (uint32_t)payload_length);
  *out_bytes = bytes;
  *out_length = total;
  *out_writer = writer;
  return TP_STATUS_OK;
}

static tp_status decode_frame(const uint8_t *bytes, size_t length,
                              uint32_t expected_magic, wire_reader *out_reader,
                              tp_error *err) {
  if (!bytes) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: NULL frame");
  }
  if (length < FRAME_HEADER_BYTES) {
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: truncated frame header");
  }
  wire_reader reader = {bytes, length, 0U};
  uint32_t magic = 0U;
  uint16_t version = 0U;
  uint16_t reserved = 0U;
  uint32_t payload_length = 0U;
  (void)rd_u32(&reader, &magic);
  (void)rd_u16(&reader, &version);
  (void)rd_u16(&reader, &reserved);
  (void)rd_u32(&reader, &payload_length);
  if (magic != expected_magic) {
    return tp_error_set(err, TP_STATUS_BAD_MAGIC,
                        "tp_job_worker_proto: bad frame magic");
  }
  if (version != TP_JOB_WORKER_PROTO_VERSION) {
    return tp_error_set(err, TP_STATUS_BAD_VERSION,
                        "tp_job_worker_proto: unsupported version %u",
                        (unsigned int)version);
  }
  if (reserved != 0U) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: nonzero reserved field");
  }
  if ((size_t)payload_length > TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES ||
      (size_t)payload_length != length - FRAME_HEADER_BYTES) {
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: declared frame length mismatch");
  }
  out_reader->bytes = bytes + FRAME_HEADER_BYTES;
  out_reader->length = payload_length;
  out_reader->offset = 0U;
  return TP_STATUS_OK;
}

static tp_status validate_text(const char *text, size_t cap, bool allow_empty,
                               const char *label, size_t *out_length,
                               tp_error *err) {
  size_t length = 0U;
  if (!bounded_cstr_length(text, cap, &length) ||
      (!allow_empty && length == 0U)) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: invalid or oversized %s", label);
  }
  tp_status status = tp_utf8_validate_text_field(text, length, label, err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  *out_length = length;
  return TP_STATUS_OK;
}

static tp_status copy_text(wire_reader *reader, uint32_t length, size_t cap,
                           bool allow_empty, const char *label,
                           const char **out, tp_error *err) {
  *out = NULL;
  if ((size_t)length > cap || (!allow_empty && length == 0U)) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: invalid %s length", label);
  }
  const uint8_t *source = NULL;
  if (!rd_ref(reader, &source, length)) {
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: truncated %s", label);
  }
  tp_status status = tp_utf8_validate_text_field(source, length, label, err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  char *copy = (char *)malloc((size_t)length + 1U);
  if (!copy) {
    return tp_error_set(err, TP_STATUS_OOM,
                        "tp_job_worker_proto: %s allocation failed", label);
  }
  memcpy(copy, source, length);
  copy[length] = '\0';
  *out = copy;
  return TP_STATUS_OK;
}

static tp_status copy_fixed_text(wire_reader *reader, uint32_t length,
                                 char *out, size_t cap, const char *label,
                                 tp_error *err) {
  if ((size_t)length > cap) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: oversized %s", label);
  }
  const uint8_t *source = NULL;
  if (!rd_ref(reader, &source, length)) {
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: truncated %s", label);
  }
  tp_status status = tp_utf8_validate_text_field(source, length, label, err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  memcpy(out, source, length);
  out[length] = '\0';
  return TP_STATUS_OK;
}

void tp_job_worker_proto_request_free(tp_job_worker_proto_request *request) {
  if (!request) {
    return;
  }
  free((void *)request->project_json);
  free((void *)request->project_dir);
  free((void *)request->work_dir);
  free((void *)request->preview_exporter_id);
  memset(request, 0, sizeof *request);
}

tp_status
tp_job_worker_proto_encode_request(const tp_job_worker_proto_request *request,
                                   uint8_t **out_bytes, size_t *out_len,
                                   tp_error *err) {
  if (out_bytes) {
    *out_bytes = NULL;
  }
  if (out_len) {
    *out_len = 0U;
  }
  if (!request || !out_bytes || !out_len || !valid_job_kind(request->kind) ||
      request->request_id == 0U || request->host_pid == 0U ||
      !request->project_json || request->project_json_len == 0U ||
      request->project_json_len > TP_JOB_WORKER_PROTO_MAX_PROJECT_JSON_BYTES) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: invalid request");
  }
  tp_status status = tp_utf8_validate_text_field(
      request->project_json, request->project_json_len, "project JSON", err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  size_t project_dir_len = 0U;
  size_t work_len = 0U;
  size_t preview_len = 0U;
  status =
      validate_text(request->project_dir, TP_JOB_WORKER_PROTO_MAX_PATH_BYTES,
                    false, "project_dir", &project_dir_len, err);
  if (status == TP_STATUS_OK) {
    status =
        validate_text(request->work_dir, TP_JOB_WORKER_PROTO_MAX_PATH_BYTES,
                      false, "work_dir", &work_len, err);
  }
  if (status == TP_STATUS_OK) {
    status = validate_text(request->preview_exporter_id,
                           TP_JOB_WORKER_PROTO_MAX_EXPORTER_ID_BYTES, true,
                           "preview_exporter_id", &preview_len, err);
  }
  if (status != TP_STATUS_OK) {
    return status;
  }
  if (request->kind == TP_SESSION_JOB_PACK && id_is_nil(request->atlas_id)) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: Pack atlas_id is nil");
  }
  size_t payload = REQUEST_FIXED_BYTES;
  if (!add_size(&payload, request->project_json_len) ||
      !add_size(&payload, project_dir_len) || !add_size(&payload, work_len) ||
      !add_size(&payload, preview_len)) {
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: request size overflow");
  }
  wire_writer writer;
  status = make_frame(TP_JOB_WORKER_PROTO_REQUEST_MAGIC, payload, out_bytes,
                      out_len, &writer, err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  wr_u32(&writer, (uint32_t)request->kind);
  wr_u64(&writer, request->session_instance_generation);
  wr_u64(&writer, request->request_id);
  wr_u32(&writer, (uint32_t)request->project_json_len);
  wr_u32(&writer, (uint32_t)project_dir_len);
  wr_u32(&writer, (uint32_t)work_len);
  wr_u32(&writer, (uint32_t)preview_len);
  wr_u32(&writer, request->host_pid);
  wr_bytes(&writer, request->atlas_id.bytes, sizeof request->atlas_id.bytes);
  wr_u64(&writer, request->input_token.model_generation);
  wr_u64(&writer, request->input_token.source_generation);
  wr_bytes(&writer, request->project_json, request->project_json_len);
  wr_bytes(&writer, request->project_dir, project_dir_len);
  wr_bytes(&writer, request->work_dir, work_len);
  wr_bytes(&writer, request->preview_exporter_id, preview_len);
  return TP_STATUS_OK;
}

tp_status tp_job_worker_proto_decode_request(const uint8_t *bytes, size_t len,
                                             tp_job_worker_proto_request *out,
                                             tp_error *err) {
  if (!out) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: NULL request out");
  }
  memset(out, 0, sizeof *out);
  wire_reader reader;
  tp_status status =
      decode_frame(bytes, len, TP_JOB_WORKER_PROTO_REQUEST_MAGIC, &reader, err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  uint32_t kind = 0U;
  uint32_t project_len = 0U;
  uint32_t project_dir_len = 0U;
  uint32_t work_len = 0U;
  uint32_t preview_len = 0U;
  if (reader.length < REQUEST_FIXED_BYTES || !rd_u32(&reader, &kind) ||
      !rd_u64(&reader, &out->session_instance_generation) ||
      !rd_u64(&reader, &out->request_id) || !rd_u32(&reader, &project_len) ||
      !rd_u32(&reader, &project_dir_len) || !rd_u32(&reader, &work_len) ||
      !rd_u32(&reader, &preview_len) || !rd_u32(&reader, &out->host_pid) ||
      !rd_bytes(&reader, out->atlas_id.bytes, sizeof out->atlas_id.bytes) ||
      !rd_u64(&reader, &out->input_token.model_generation) ||
      !rd_u64(&reader, &out->input_token.source_generation)) {
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: truncated request");
  }
  out->kind = (tp_session_job_kind)kind;
  if (!valid_job_kind(out->kind) || out->request_id == 0U ||
      out->host_pid == 0U || project_len == 0U ||
      (size_t)project_len > TP_JOB_WORKER_PROTO_MAX_PROJECT_JSON_BYTES ||
      (size_t)project_dir_len > TP_JOB_WORKER_PROTO_MAX_PATH_BYTES ||
      project_dir_len == 0U ||
      (size_t)work_len > TP_JOB_WORKER_PROTO_MAX_PATH_BYTES || work_len == 0U ||
      (size_t)preview_len > TP_JOB_WORKER_PROTO_MAX_EXPORTER_ID_BYTES ||
      (out->kind == TP_SESSION_JOB_PACK && id_is_nil(out->atlas_id))) {
    status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                          "tp_job_worker_proto: invalid request fields");
    goto fail;
  }
  const uint8_t *project = NULL;
  if (!rd_ref(&reader, &project, project_len)) {
    status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                          "tp_job_worker_proto: truncated project JSON");
    goto fail;
  }
  status =
      tp_utf8_validate_text_field(project, project_len, "project JSON", err);
  if (status != TP_STATUS_OK) {
    goto fail;
  }
  uint8_t *project_copy = (uint8_t *)malloc((size_t)project_len + 1U);
  if (!project_copy) {
    status =
        tp_error_set(err, TP_STATUS_OOM,
                     "tp_job_worker_proto: project JSON allocation failed");
    goto fail;
  }
  memcpy(project_copy, project, project_len);
  project_copy[project_len] = '\0';
  out->project_json = project_copy;
  out->project_json_len = project_len;
  status =
      copy_text(&reader, project_dir_len, TP_JOB_WORKER_PROTO_MAX_PATH_BYTES,
                false, "project_dir", &out->project_dir, err);
  if (status == TP_STATUS_OK) {
    status = copy_text(&reader, work_len, TP_JOB_WORKER_PROTO_MAX_PATH_BYTES,
                       false, "work_dir", &out->work_dir, err);
  }
  if (status == TP_STATUS_OK) {
    status = copy_text(&reader, preview_len,
                       TP_JOB_WORKER_PROTO_MAX_EXPORTER_ID_BYTES, true,
                       "preview_exporter_id", &out->preview_exporter_id, err);
  }
  if (status != TP_STATUS_OK) {
    goto fail;
  }
  if (reader.offset != reader.length) {
    status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                          "tp_job_worker_proto: trailing request bytes");
    goto fail;
  }
  return TP_STATUS_OK;

fail:
  tp_job_worker_proto_request_free(out);
  return status;
}

static tp_status error_wire_lengths(const tp_error *error,
                                    size_t *message_length, size_t *path_length,
                                    tp_error *err) {
  tp_status status =
      validate_text(error->msg, TP_JOB_WORKER_PROTO_MAX_ERROR_BYTES, true,
                    "error message", message_length, err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  if (!valid_file_phase((int32_t)error->file_io.phase)) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: invalid file I/O phase");
  }
  return validate_text(error->file_io.path, TP_FILE_IO_PATH_MAX - 1U, true,
                       "error path", path_length, err);
}

static void write_error_bytes(wire_writer *writer, const tp_error *error,
                              size_t message_length, size_t path_length) {
  wr_bytes(writer, error->msg, message_length);
  if (path_length > 0U) {
    wr_bytes(writer, error->file_io.path, path_length);
  }
}

static tp_status read_error_bytes(wire_reader *reader, uint32_t message_length,
                                  uint32_t path_length, int32_t phase,
                                  int32_t native_code, tp_error *out,
                                  tp_error *err) {
  if (!valid_file_phase(phase)) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: invalid encoded error");
  }
  tp_status status =
      copy_fixed_text(reader, message_length, out->msg, sizeof out->msg - 1U,
                      "error message", err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  status = copy_fixed_text(reader, path_length, out->file_io.path,
                           sizeof out->file_io.path - 1U, "error path", err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  out->file_io.phase = (tp_file_io_phase)phase;
  out->file_io.native_code = native_code;
  return TP_STATUS_OK;
}

void tp_job_worker_proto_response_free(tp_job_worker_proto_response *response) {
  if (!response) {
    return;
  }
  if (response->kind == TP_SESSION_JOB_PACK) {
    if (response->pack.names) {
      for (uint32_t i = 0U; i < response->pack.name_count; ++i) {
        free((void *)response->pack.names[i].name);
      }
      free((void *)response->pack.names);
    }
    free((void *)response->pack.artifact_path);
  }
  memset(response, 0, sizeof *response);
}

tp_status tp_job_worker_proto_encode_response(
    const tp_job_worker_proto_response *response, uint8_t **out_bytes,
    size_t *out_len, tp_error *err) {
  if (out_bytes) {
    *out_bytes = NULL;
  }
  if (out_len) {
    *out_len = 0U;
  }
  if (!response || !out_bytes || !out_len || !valid_job_kind(response->kind) ||
      response->request_id == 0U || !isfinite(response->elapsed_ms) ||
      response->elapsed_ms < 0.0) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: invalid response");
  }
  tp_status status =
      validate_terminal_semantics(response->state, response->status, err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  size_t error_message_len = 0U;
  size_t error_path_len = 0U;
  status = error_wire_lengths(&response->error, &error_message_len,
                              &error_path_len, err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  size_t payload = RESPONSE_COMMON_BYTES;
  size_t preview_len = 0U;
  size_t freshness_message_len = 0U;
  size_t freshness_path_len = 0U;
  size_t artifact_path_len = 0U;
  size_t first_error_len = 0U;
  if (!add_size(&payload, error_message_len) ||
      !add_size(&payload, error_path_len)) {
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: response size overflow");
  }
  if (response->kind == TP_SESSION_JOB_PACK) {
    const tp_job_worker_proto_pack_result *pack = &response->pack;
    if (!valid_count(pack->missing_sources) ||
        pack->freshness < TP_PACK_FRESHNESS_STALE ||
        pack->freshness > TP_PACK_FRESHNESS_CURRENT ||
        pack->freshness_reason < TP_PACK_FRESHNESS_RESULT_HASH_NIL ||
        pack->freshness_reason > TP_PACK_FRESHNESS_TOKEN_CHANGED ||
        !valid_status((int32_t)pack->freshness_status) ||
        pack->name_count > TP_JOB_WORKER_PROTO_MAX_NAME_ENTRIES ||
        (pack->name_count > 0U && !pack->names) ||
        pack->artifact_size > TP_JOB_WORKER_PROTO_MAX_ARTIFACT_BYTES ||
        (response->state == TP_SESSION_JOB_SUCCEEDED &&
         (pack->artifact_size == 0U || !pack->artifact_path ||
          pack->artifact_path[0] == '\0'))) {
      return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                          "tp_job_worker_proto: invalid Pack response");
    }
    status = validate_text(pack->preview_exporter_id,
                           TP_JOB_WORKER_PROTO_MAX_EXPORTER_ID_BYTES, true,
                           "Pack preview exporter", &preview_len, err);
    if (status == TP_STATUS_OK) {
      status = validate_text(pack->artifact_path ? pack->artifact_path : "",
                             TP_JOB_WORKER_PROTO_MAX_PATH_BYTES, true,
                             "Pack artifact path", &artifact_path_len, err);
    }
    if (status == TP_STATUS_OK) {
      status =
          error_wire_lengths(&pack->freshness_error, &freshness_message_len,
                             &freshness_path_len, err);
    }
    if (status != TP_STATUS_OK) {
      return status;
    }
    if (!add_size(&payload, RESPONSE_PACK_BYTES) ||
        !add_size(&payload, preview_len) ||
        !add_size(&payload, freshness_message_len) ||
        !add_size(&payload, freshness_path_len) ||
        !add_size(&payload, artifact_path_len)) {
      return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                          "tp_job_worker_proto: Pack response size overflow");
    }
    for (uint32_t i = 0U; i < pack->name_count; ++i) {
      size_t name_len = 0U;
      status =
          validate_text(pack->names[i].name, TP_JOB_WORKER_PROTO_MAX_NAME_BYTES,
                        false, "name-map name", &name_len, err);
      if (status != TP_STATUS_OK || !add_size(&payload, 12U) ||
          !add_size(&payload, name_len)) {
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(
                         err, TP_STATUS_OUT_OF_BOUNDS,
                         "tp_job_worker_proto: name map size overflow");
      }
    }
  } else {
    const tp_session_export_job_result *export_result =
        &response->export_result;
    if (!valid_count(export_result->targets) ||
        !valid_count(export_result->files) ||
        !valid_count(export_result->notices) ||
        !valid_count(export_result->atlases_ok) ||
        !valid_count(export_result->atlases_failed) ||
        !valid_count(export_result->atlases_skipped)) {
      return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                          "tp_job_worker_proto: invalid Export counts");
    }
    status = validate_text(export_result->first_error,
                           TP_JOB_WORKER_PROTO_MAX_ERROR_BYTES, true,
                           "Export first error", &first_error_len, err);
    if (status != TP_STATUS_OK || !add_size(&payload, RESPONSE_EXPORT_BYTES) ||
        !add_size(&payload, first_error_len)) {
      return status != TP_STATUS_OK
                 ? status
                 : tp_error_set(
                       err, TP_STATUS_OUT_OF_BOUNDS,
                       "tp_job_worker_proto: Export response size overflow");
    }
  }

  wire_writer writer;
  status = make_frame(TP_JOB_WORKER_PROTO_RESPONSE_MAGIC, payload, out_bytes,
                      out_len, &writer, err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  wr_u32(&writer, (uint32_t)response->kind);
  wr_u32(&writer, (uint32_t)response->state);
  wr_i32(&writer, (int32_t)response->status);
  wr_u64(&writer, response->session_instance_generation);
  wr_u64(&writer, response->request_id);
  uint64_t elapsed_bits = 0U;
  memcpy(&elapsed_bits, &response->elapsed_ms, sizeof elapsed_bits);
  wr_u64(&writer, elapsed_bits);
  wr_u32(&writer, (uint32_t)error_message_len);
  wr_u32(&writer, (uint32_t)error_path_len);
  wr_i32(&writer, (int32_t)response->error.file_io.phase);
  wr_i32(&writer, response->error.file_io.native_code);

  if (response->kind == TP_SESSION_JOB_PACK) {
    const tp_job_worker_proto_pack_result *pack = &response->pack;
    wr_bytes(&writer, pack->atlas_id.bytes, sizeof pack->atlas_id.bytes);
    wr_i32(&writer, pack->missing_sources);
    wr_u64(&writer, pack->input_token_at_start.model_generation);
    wr_u64(&writer, pack->input_token_at_start.source_generation);
    wr_bytes(&writer, pack->pack_input_hash.bytes,
             sizeof pack->pack_input_hash.bytes);
    wr_bytes(&writer, pack->current_pack_input_hash.bytes,
             sizeof pack->current_pack_input_hash.bytes);
    wr_u64(&writer, pack->freshness_token.model_generation);
    wr_u64(&writer, pack->freshness_token.source_generation);
    wr_i32(&writer, (int32_t)pack->freshness);
    wr_i32(&writer, (int32_t)pack->freshness_reason);
    wr_i32(&writer, (int32_t)pack->freshness_status);
    wr_u32(&writer, (uint32_t)preview_len);
    wr_u32(&writer, (uint32_t)freshness_message_len);
    wr_u32(&writer, (uint32_t)freshness_path_len);
    wr_i32(&writer, (int32_t)pack->freshness_error.file_io.phase);
    wr_i32(&writer, pack->freshness_error.file_io.native_code);
    wr_u32(&writer, pack->name_count);
    wr_u32(&writer, (uint32_t)artifact_path_len);
    wr_u64(&writer, pack->artifact_size);
    write_error_bytes(&writer, &response->error, error_message_len,
                      error_path_len);
    wr_bytes(&writer, pack->preview_exporter_id, preview_len);
    write_error_bytes(&writer, &pack->freshness_error, freshness_message_len,
                      freshness_path_len);
    for (uint32_t i = 0U; i < pack->name_count; ++i) {
      size_t name_len = 0U;
      (void)bounded_cstr_length(pack->names[i].name,
                                TP_JOB_WORKER_PROTO_MAX_NAME_BYTES, &name_len);
      wr_u64(&writer, pack->names[i].hash);
      wr_u32(&writer, (uint32_t)name_len);
      wr_bytes(&writer, pack->names[i].name, name_len);
    }
    wr_bytes(&writer, pack->artifact_path, artifact_path_len);
  } else {
    const tp_session_export_job_result *export_result =
        &response->export_result;
    wr_i32(&writer, export_result->targets);
    wr_i32(&writer, export_result->files);
    wr_i32(&writer, export_result->notices);
    wr_i32(&writer, export_result->atlases_ok);
    wr_i32(&writer, export_result->atlases_failed);
    wr_i32(&writer, export_result->atlases_skipped);
    wr_u32(&writer, export_result->partial_publication ? 1U : 0U);
    wr_u32(&writer, export_result->publication_uncertain ? 1U : 0U);
    wr_u32(&writer, (uint32_t)first_error_len);
    write_error_bytes(&writer, &response->error, error_message_len,
                      error_path_len);
    wr_bytes(&writer, export_result->first_error, first_error_len);
  }
  return TP_STATUS_OK;
}

tp_status tp_job_worker_proto_decode_response(const uint8_t *bytes, size_t len,
                                              tp_job_worker_proto_response *out,
                                              tp_error *err) {
  if (!out) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: NULL response out");
  }
  memset(out, 0, sizeof *out);
  wire_reader reader;
  tp_status status = decode_frame(
      bytes, len, TP_JOB_WORKER_PROTO_RESPONSE_MAGIC, &reader, err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  uint32_t kind = 0U;
  uint32_t state = 0U;
  int32_t wire_status = 0;
  uint64_t elapsed_bits = 0U;
  uint32_t error_message_len = 0U;
  uint32_t error_path_len = 0U;
  int32_t error_phase = 0;
  int32_t error_native = 0;
  if (reader.length < RESPONSE_COMMON_BYTES || !rd_u32(&reader, &kind) ||
      !rd_u32(&reader, &state) || !rd_i32(&reader, &wire_status) ||
      !rd_u64(&reader, &out->session_instance_generation) ||
      !rd_u64(&reader, &out->request_id) || !rd_u64(&reader, &elapsed_bits) ||
      !rd_u32(&reader, &error_message_len) ||
      !rd_u32(&reader, &error_path_len) || !rd_i32(&reader, &error_phase) ||
      !rd_i32(&reader, &error_native)) {
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: truncated response");
  }
  out->kind = (tp_session_job_kind)kind;
  out->state = (tp_session_job_state)state;
  out->status = (tp_status)wire_status;
  memcpy(&out->elapsed_ms, &elapsed_bits, sizeof out->elapsed_ms);
  /* A diagnostic path rides in a fixed TP_FILE_IO_PATH_MAX buffer, so its real
   * bound is that buffer minus the NUL -- NOT the much larger identity-path cap
   * a general path carries. State the real limit here so the outer check reads
   * as the contract instead of deferring to copy_fixed_text's capacity. */
  if (!valid_job_kind(out->kind) || out->request_id == 0U ||
      !isfinite(out->elapsed_ms) || out->elapsed_ms < 0.0 ||
      (size_t)error_path_len > TP_FILE_IO_PATH_MAX - 1U) {
    status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                          "tp_job_worker_proto: invalid response fields");
    goto fail;
  }
  status = validate_terminal_semantics(out->state, out->status, err);
  if (status != TP_STATUS_OK) {
    goto fail;
  }

  if (out->kind == TP_SESSION_JOB_PACK) {
    tp_job_worker_proto_pack_result *pack = &out->pack;
    int32_t missing_sources = 0;
    int32_t freshness = 0;
    int32_t freshness_reason = 0;
    int32_t freshness_status = 0;
    uint32_t preview_len = 0U;
    uint32_t fresh_message_len = 0U;
    uint32_t fresh_path_len = 0U;
    int32_t fresh_phase = 0;
    int32_t fresh_native = 0;
    uint32_t artifact_path_len = 0U;
    uint64_t artifact_size = 0U;
    if (reader.length - reader.offset < RESPONSE_PACK_BYTES ||
        !rd_bytes(&reader, pack->atlas_id.bytes, sizeof pack->atlas_id.bytes) ||
        !rd_i32(&reader, &missing_sources) ||
        !rd_u64(&reader, &pack->input_token_at_start.model_generation) ||
        !rd_u64(&reader, &pack->input_token_at_start.source_generation) ||
        !rd_bytes(&reader, pack->pack_input_hash.bytes,
                  sizeof pack->pack_input_hash.bytes) ||
        !rd_bytes(&reader, pack->current_pack_input_hash.bytes,
                  sizeof pack->current_pack_input_hash.bytes) ||
        !rd_u64(&reader, &pack->freshness_token.model_generation) ||
        !rd_u64(&reader, &pack->freshness_token.source_generation) ||
        !rd_i32(&reader, &freshness) || !rd_i32(&reader, &freshness_reason) ||
        !rd_i32(&reader, &freshness_status) || !rd_u32(&reader, &preview_len) ||
        !rd_u32(&reader, &fresh_message_len) ||
        !rd_u32(&reader, &fresh_path_len) || !rd_i32(&reader, &fresh_phase) ||
        !rd_i32(&reader, &fresh_native) ||
        !rd_u32(&reader, &pack->name_count) ||
        !rd_u32(&reader, &artifact_path_len) ||
        !rd_u64(&reader, &artifact_size)) {
      status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_job_worker_proto: truncated Pack response");
      goto fail;
    }
    pack->missing_sources = missing_sources;
    pack->freshness = (tp_pack_freshness)freshness;
    pack->freshness_reason = (tp_pack_freshness_reason)freshness_reason;
    pack->freshness_status = (tp_status)freshness_status;
    pack->artifact_size = artifact_size;
    if (!valid_count(missing_sources) ||
        freshness < (int32_t)TP_PACK_FRESHNESS_STALE ||
        freshness > (int32_t)TP_PACK_FRESHNESS_CURRENT ||
        freshness_reason < (int32_t)TP_PACK_FRESHNESS_RESULT_HASH_NIL ||
        freshness_reason > (int32_t)TP_PACK_FRESHNESS_TOKEN_CHANGED ||
        !valid_status(freshness_status) ||
        preview_len > TP_JOB_WORKER_PROTO_MAX_EXPORTER_ID_BYTES ||
        fresh_message_len > TP_JOB_WORKER_PROTO_MAX_ERROR_BYTES ||
        (size_t)fresh_path_len > TP_FILE_IO_PATH_MAX - 1U ||
        !valid_file_phase(fresh_phase) ||
        pack->name_count > TP_JOB_WORKER_PROTO_MAX_NAME_ENTRIES ||
        (size_t)artifact_path_len > TP_JOB_WORKER_PROTO_MAX_PATH_BYTES ||
        pack->artifact_size > TP_JOB_WORKER_PROTO_MAX_ARTIFACT_BYTES ||
        (out->state == TP_SESSION_JOB_SUCCEEDED &&
         (pack->artifact_size == 0U || artifact_path_len == 0U))) {
      status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_job_worker_proto: invalid Pack fields");
      goto fail;
    }
    status = read_error_bytes(&reader, error_message_len, error_path_len,
                              error_phase, error_native, &out->error, err);
    if (status != TP_STATUS_OK) {
      goto fail;
    }
    const char *preview = NULL;
    status = copy_text(&reader, preview_len,
                       TP_JOB_WORKER_PROTO_MAX_EXPORTER_ID_BYTES, true,
                       "Pack preview exporter", &preview, err);
    if (status != TP_STATUS_OK) {
      goto fail;
    }
    memcpy(pack->preview_exporter_id, preview, preview_len + 1U);
    free((void *)preview);
    status = read_error_bytes(&reader, fresh_message_len, fresh_path_len,
                              fresh_phase, fresh_native, &pack->freshness_error,
                              err);
    if (status != TP_STATUS_OK) {
      goto fail;
    }
    if (pack->name_count > 0U) {
      tp_job_worker_proto_name *names =
          (tp_job_worker_proto_name *)calloc(pack->name_count, sizeof *names);
      if (!names) {
        status =
            tp_error_set(err, TP_STATUS_OOM,
                         "tp_job_worker_proto: name map allocation failed");
        goto fail;
      }
      pack->names = names;
      for (uint32_t i = 0U; i < pack->name_count; ++i) {
        uint32_t name_len = 0U;
        if (!rd_u64(&reader, &names[i].hash) || !rd_u32(&reader, &name_len)) {
          status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "tp_job_worker_proto: truncated name map");
          goto fail;
        }
        status =
            copy_text(&reader, name_len, TP_JOB_WORKER_PROTO_MAX_NAME_BYTES,
                      false, "name-map name", &names[i].name, err);
        if (status != TP_STATUS_OK) {
          goto fail;
        }
      }
    }
    status = copy_text(&reader, artifact_path_len,
                       TP_JOB_WORKER_PROTO_MAX_PATH_BYTES, true,
                       "Pack artifact path", &pack->artifact_path, err);
    if (status != TP_STATUS_OK) {
      goto fail;
    }
  } else {
    tp_session_export_job_result *export_result = &out->export_result;
    int32_t targets = 0;
    int32_t files = 0;
    int32_t notices = 0;
    int32_t atlases_ok = 0;
    int32_t atlases_failed = 0;
    int32_t atlases_skipped = 0;
    uint32_t partial = 0U;
    uint32_t uncertain = 0U;
    uint32_t first_error_len = 0U;
    if (reader.length - reader.offset < RESPONSE_EXPORT_BYTES ||
        !rd_i32(&reader, &targets) || !rd_i32(&reader, &files) ||
        !rd_i32(&reader, &notices) || !rd_i32(&reader, &atlases_ok) ||
        !rd_i32(&reader, &atlases_failed) ||
        !rd_i32(&reader, &atlases_skipped) || !rd_u32(&reader, &partial) ||
        !rd_u32(&reader, &uncertain) || !rd_u32(&reader, &first_error_len)) {
      status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_job_worker_proto: truncated Export response");
      goto fail;
    }
    if (!valid_count(targets) || !valid_count(files) || !valid_count(notices) ||
        !valid_count(atlases_ok) || !valid_count(atlases_failed) ||
        !valid_count(atlases_skipped) || partial > 1U || uncertain > 1U ||
        first_error_len > TP_JOB_WORKER_PROTO_MAX_ERROR_BYTES) {
      status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_job_worker_proto: invalid Export fields");
      goto fail;
    }
    export_result->targets = targets;
    export_result->files = files;
    export_result->notices = notices;
    export_result->atlases_ok = atlases_ok;
    export_result->atlases_failed = atlases_failed;
    export_result->atlases_skipped = atlases_skipped;
    export_result->partial_publication = partial != 0U;
    export_result->publication_uncertain = uncertain != 0U;
    status = read_error_bytes(&reader, error_message_len, error_path_len,
                              error_phase, error_native, &out->error, err);
    if (status != TP_STATUS_OK) {
      goto fail;
    }
    status =
        copy_fixed_text(&reader, first_error_len, export_result->first_error,
                        sizeof export_result->first_error - 1U,
                        "Export first error", err);
    if (status != TP_STATUS_OK) {
      goto fail;
    }
  }
  if (reader.offset != reader.length) {
    status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                          "tp_job_worker_proto: trailing response bytes");
    goto fail;
  }
  return TP_STATUS_OK;

fail:
  tp_job_worker_proto_response_free(out);
  return status;
}

tp_status tp_job_worker_proto_encode_progress(
    const tp_job_worker_proto_progress *progress, uint8_t **out_bytes,
    size_t *out_len, tp_error *err) {
  if (out_bytes) {
    *out_bytes = NULL;
  }
  if (out_len) {
    *out_len = 0U;
  }
  if (!progress || !out_bytes || !out_len || progress->request_id == 0U ||
      !valid_count(progress->current) || !valid_count(progress->total) ||
      (progress->total > 0 && progress->current > progress->total) ||
      !valid_progress_phase((int32_t)progress->phase)) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: invalid progress");
  }
  wire_writer writer;
  tp_status status =
      make_frame(TP_JOB_WORKER_PROTO_PROGRESS_MAGIC, PROGRESS_PAYLOAD_BYTES,
                 out_bytes, out_len, &writer, err);
  if (status != TP_STATUS_OK) {
    return status;
  }
  wr_u64(&writer, progress->request_id);
  wr_i32(&writer, progress->current);
  wr_i32(&writer, progress->total);
  wr_i32(&writer, (int32_t)progress->phase);
  wr_u32(&writer, 0U);
  return TP_STATUS_OK;
}

tp_status tp_job_worker_proto_decode_progress(const uint8_t *bytes, size_t len,
                                              tp_job_worker_proto_progress *out,
                                              tp_error *err) {
  if (!out) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: NULL progress out");
  }
  memset(out, 0, sizeof *out);
  wire_reader reader;
  tp_status status = decode_frame(
      bytes, len, TP_JOB_WORKER_PROTO_PROGRESS_MAGIC, &reader, err);
  int32_t current = 0;
  int32_t total = 0;
  int32_t phase = 0;
  uint32_t reserved = 0U;
  if (status != TP_STATUS_OK) {
    return status;
  }
  if (reader.length != PROGRESS_PAYLOAD_BYTES ||
      !rd_u64(&reader, &out->request_id) || !rd_i32(&reader, &current) ||
      !rd_i32(&reader, &total) || !rd_i32(&reader, &phase) ||
      !rd_u32(&reader, &reserved)) {
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: truncated progress");
  }
  if (out->request_id == 0U || !valid_count(current) || !valid_count(total) ||
      (total > 0 && current > total) || !valid_progress_phase(phase) ||
      reserved != 0U) {
    memset(out, 0, sizeof *out);
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: invalid progress fields");
  }
  out->current = current;
  out->total = total;
  out->phase = (tp_job_worker_progress_phase)phase;
  return TP_STATUS_OK;
}

void tp_job_worker_proto_stream_init(tp_job_worker_proto_stream *stream) {
  if (stream) {
    memset(stream, 0, sizeof *stream);
  }
}

void tp_job_worker_proto_stream_destroy(tp_job_worker_proto_stream *stream) {
  if (!stream) {
    return;
  }
  free(stream->bytes);
  memset(stream, 0, sizeof *stream);
}

tp_status tp_job_worker_proto_stream_feed(tp_job_worker_proto_stream *stream,
                                          const uint8_t *bytes, size_t len,
                                          tp_error *err) {
  if (!stream || (!bytes && len > 0U) || stream->terminal_seen) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: invalid stream feed");
  }
  if (stream->cumulative_bytes > TP_JOB_WORKER_PROTO_MAX_STREAM_BYTES ||
      len > TP_JOB_WORKER_PROTO_MAX_STREAM_BYTES - stream->cumulative_bytes ||
      len > SIZE_MAX - stream->length) {
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: cumulative stream cap exceeded");
  }
  const size_t needed = stream->length + len;
  if (needed > stream->capacity) {
    size_t capacity = stream->capacity ? stream->capacity : 4096U;
    while (capacity < needed) {
      if (capacity > TP_JOB_WORKER_PROTO_MAX_STREAM_BYTES / 2U) {
        capacity = TP_JOB_WORKER_PROTO_MAX_STREAM_BYTES;
        break;
      }
      capacity *= 2U;
    }
    uint8_t *grown = (uint8_t *)realloc(stream->bytes, capacity);
    if (!grown) {
      return tp_error_set(err, TP_STATUS_OOM,
                          "tp_job_worker_proto: stream allocation failed");
    }
    stream->bytes = grown;
    stream->capacity = capacity;
  }
  if (len > 0U) {
    memcpy(stream->bytes + stream->length, bytes, len);
    stream->length += len;
    stream->cumulative_bytes += len;
  }
  return TP_STATUS_OK;
}

tp_status
tp_job_worker_proto_stream_next(tp_job_worker_proto_stream *stream,
                                tp_job_worker_proto_stream_message *out,
                                bool *out_ready, tp_error *err) {
  if (!stream || !out || !out_ready) {
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "tp_job_worker_proto: invalid stream next");
  }
  memset(out, 0, sizeof *out);
  *out_ready = false;
  if (stream->length < FRAME_HEADER_BYTES) {
    return TP_STATUS_OK;
  }
  wire_reader header = {stream->bytes, stream->length, 0U};
  uint32_t magic = 0U;
  uint16_t version = 0U;
  uint16_t reserved = 0U;
  uint32_t payload_length = 0U;
  (void)rd_u32(&header, &magic);
  (void)rd_u16(&header, &version);
  (void)rd_u16(&header, &reserved);
  (void)rd_u32(&header, &payload_length);
  if (magic != TP_JOB_WORKER_PROTO_PROGRESS_MAGIC &&
      magic != TP_JOB_WORKER_PROTO_RESPONSE_MAGIC) {
    return tp_error_set(err, TP_STATUS_BAD_MAGIC,
                        "tp_job_worker_proto: unknown stream frame");
  }
  if (version != TP_JOB_WORKER_PROTO_VERSION) {
    return tp_error_set(err, TP_STATUS_BAD_VERSION,
                        "tp_job_worker_proto: stream version mismatch");
  }
  if (reserved != 0U ||
      (size_t)payload_length > TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES ||
      (magic == TP_JOB_WORKER_PROTO_PROGRESS_MAGIC &&
       payload_length != PROGRESS_PAYLOAD_BYTES)) {
    return tp_error_set(err,
                        reserved != 0U ? TP_STATUS_INVALID_ARGUMENT
                                       : TP_STATUS_OUT_OF_BOUNDS,
                        "tp_job_worker_proto: invalid stream frame header");
  }
  const size_t frame_length = FRAME_HEADER_BYTES + (size_t)payload_length;
  if (stream->length < frame_length) {
    return TP_STATUS_OK;
  }
  tp_status status;
  if (magic == TP_JOB_WORKER_PROTO_PROGRESS_MAGIC) {
    if (stream->progress_frames >= TP_JOB_WORKER_PROTO_MAX_PROGRESS_FRAMES) {
      return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                          "tp_job_worker_proto: progress frame cap exceeded");
    }
    out->kind = TP_JOB_WORKER_STREAM_PROGRESS;
    status = tp_job_worker_proto_decode_progress(stream->bytes, frame_length,
                                                 &out->progress, err);
    if (status == TP_STATUS_OK) {
      stream->progress_frames++;
    }
  } else {
    if (stream->terminal_seen || stream->length != frame_length) {
      return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                          "tp_job_worker_proto: terminal frame must be last");
    }
    out->kind = TP_JOB_WORKER_STREAM_TERMINAL;
    status = tp_job_worker_proto_decode_response(stream->bytes, frame_length,
                                                 &out->terminal, err);
    if (status == TP_STATUS_OK) {
      stream->terminal_seen = true;
    }
  }
  if (status != TP_STATUS_OK) {
    memset(out, 0, sizeof *out);
    return status;
  }
  const size_t remaining = stream->length - frame_length;
  if (remaining > 0U) {
    memmove(stream->bytes, stream->bytes + frame_length, remaining);
  }
  stream->length = remaining;
  *out_ready = true;
  return TP_STATUS_OK;
}

void tp_job_worker_proto_stream_message_free(
    tp_job_worker_proto_stream_message *message) {
  if (!message) {
    return;
  }
  if (message->kind == TP_JOB_WORKER_STREAM_TERMINAL) {
    tp_job_worker_proto_response_free(&message->terminal);
  }
  memset(message, 0, sizeof *message);
}
