/* Pure fail-closed tests for the versioned outer Pack/Export worker protocol.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tp_job_worker_internal.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static tp_id128 sample_id(uint8_t seed) {
  tp_id128 id;
  for (size_t i = 0U; i < sizeof id.bytes; ++i) {
    id.bytes[i] = (uint8_t)(seed + i);
  }
  return id;
}

static void put_u16_le(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8U);
}

static void put_u32_le(uint8_t *bytes, uint32_t value) {
  for (unsigned int i = 0U; i < 4U; ++i) {
    bytes[i] = (uint8_t)(value >> (i * 8U));
  }
}

static void assert_id_equal(tp_id128 expected, tp_id128 actual) {
  TEST_ASSERT_EQUAL_MEMORY(expected.bytes, actual.bytes, sizeof expected.bytes);
}

static tp_job_worker_proto_request sample_request(tp_session_job_kind kind) {
  static const uint8_t project[] =
      "{\"schema\":5,\"sources\":[{\"path\":\"C:/abs/a.png\"}]}\n";
  tp_job_worker_proto_request request;
  memset(&request, 0, sizeof request);
  request.kind = kind;
  request.session_instance_generation = 17U;
  request.request_id = 42U;
  request.project_json = project;
  request.project_json_len = sizeof project - 1U;
  request.atlas_id =
      kind == TP_SESSION_JOB_PACK ? sample_id(10U) : (tp_id128){{0}};
  request.project_dir = "C:/absolute/project";
  request.work_dir = "C:/absolute/work";
  request.preview_exporter_id = kind == TP_SESSION_JOB_PACK ? "defold" : "";
  request.input_token.model_generation = 101U;
  request.input_token.source_generation = 202U;
  return request;
}

static tp_job_worker_proto_response sample_pack_response(void) {
  static const uint8_t artifact[] = {0x4e, 0x54, 0x50, 0x4b, 1, 2, 3, 4};
  static const tp_job_worker_proto_name names[] = {
      {UINT64_C(0x1122334455667788), "atlas"},
      {UINT64_C(0x8877665544332211), "héros"}};
  tp_job_worker_proto_response response;
  memset(&response, 0, sizeof response);
  response.kind = TP_SESSION_JOB_PACK;
  response.session_instance_generation = 17U;
  response.request_id = 42U;
  response.state = TP_SESSION_JOB_SUCCEEDED;
  response.status = TP_STATUS_OK;
  response.elapsed_ms = 12.5;
  response.pack.atlas_id = sample_id(10U);
  response.pack.missing_sources = 2;
  response.pack.input_token_at_start.model_generation = 101U;
  response.pack.input_token_at_start.source_generation = 202U;
  response.pack.pack_input_hash = sample_id(30U);
  response.pack.current_pack_input_hash = sample_id(50U);
  response.pack.freshness_token.model_generation = 303U;
  response.pack.freshness_token.source_generation = 404U;
  response.pack.freshness = TP_PACK_FRESHNESS_CURRENT;
  response.pack.freshness_reason = TP_PACK_FRESHNESS_MATCH;
  response.pack.freshness_status = TP_STATUS_OK;
  memcpy(response.pack.preview_exporter_id, "defold", 7U);
  response.pack.names = names;
  response.pack.name_count = 2U;
  response.pack.artifact = artifact;
  response.pack.artifact_size = sizeof artifact;
  return response;
}

static uint8_t *encode_pack(size_t *out_len) {
  tp_job_worker_proto_response response = sample_pack_response();
  uint8_t *bytes = NULL;
  tp_error err = {{0}};
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      TP_STATUS_OK,
      tp_job_worker_proto_encode_response(&response, &bytes, out_len, &err),
      err.msg);
  return bytes;
}

void test_request_roundtrip_pack_and_export(void) {
  const tp_session_job_kind kinds[] = {TP_SESSION_JOB_PACK,
                                       TP_SESSION_JOB_EXPORT};
  for (size_t i = 0U; i < 2U; ++i) {
    tp_job_worker_proto_request input = sample_request(kinds[i]);
    uint8_t *bytes = NULL;
    size_t length = 0U;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_job_worker_proto_encode_request(&input, &bytes, &length, &err),
        err.msg);
    tp_job_worker_proto_request output;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_job_worker_proto_decode_request(bytes, length, &output, &err),
        err.msg);
    TEST_ASSERT_EQUAL_INT(input.kind, output.kind);
    TEST_ASSERT_EQUAL_UINT64(input.session_instance_generation,
                             output.session_instance_generation);
    TEST_ASSERT_EQUAL_UINT64(input.request_id, output.request_id);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)input.project_json_len,
                             (uint64_t)output.project_json_len);
    TEST_ASSERT_EQUAL_MEMORY(input.project_json, output.project_json,
                             input.project_json_len);
    assert_id_equal(input.atlas_id, output.atlas_id);
    TEST_ASSERT_EQUAL_STRING(input.project_dir, output.project_dir);
    TEST_ASSERT_EQUAL_STRING(input.work_dir, output.work_dir);
    TEST_ASSERT_EQUAL_STRING(input.preview_exporter_id,
                             output.preview_exporter_id);
    TEST_ASSERT_EQUAL_UINT64(input.input_token.model_generation,
                             output.input_token.model_generation);
    TEST_ASSERT_EQUAL_UINT64(input.input_token.source_generation,
                             output.input_token.source_generation);
    tp_job_worker_proto_request_free(&output);
    free(bytes);
  }
}

void test_pack_terminal_roundtrip_owns_artifact_and_utf8_name_map(void) {
  tp_job_worker_proto_response input = sample_pack_response();
  input.error.file_io.phase = TP_FILE_IO_PHASE_TEMP_OPEN;
  input.error.file_io.path = "C:/source/a.png";
  memcpy(input.error.msg, "read detail", 12U);
  input.pack.freshness_error.file_io.phase = TP_FILE_IO_PHASE_NONE;
  memcpy(input.pack.freshness_error.msg, "fresh", 6U);

  uint8_t *bytes = NULL;
  size_t length = 0U;
  tp_error err = {{0}};
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      TP_STATUS_OK,
      tp_job_worker_proto_encode_response(&input, &bytes, &length, &err),
      err.msg);
  tp_job_worker_proto_response output;
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      TP_STATUS_OK,
      tp_job_worker_proto_decode_response(bytes, length, &output, &err),
      err.msg);
  TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_PACK, output.kind);
  TEST_ASSERT_EQUAL_UINT64(42U, output.request_id);
  TEST_ASSERT_EQUAL_MEMORY(&input.elapsed_ms, &output.elapsed_ms,
                           sizeof input.elapsed_ms);
  TEST_ASSERT_EQUAL_STRING("C:/source/a.png", output.error.file_io.path);
  assert_id_equal(input.pack.pack_input_hash, output.pack.pack_input_hash);
  TEST_ASSERT_EQUAL_UINT32(2U, output.pack.name_count);
  TEST_ASSERT_EQUAL_HEX64(input.pack.names[1].hash, output.pack.names[1].hash);
  TEST_ASSERT_EQUAL_STRING("héros", output.pack.names[1].name);
  TEST_ASSERT_EQUAL_UINT64((uint64_t)input.pack.artifact_size,
                           (uint64_t)output.pack.artifact_size);
  TEST_ASSERT_EQUAL_MEMORY(input.pack.artifact, output.pack.artifact,
                           input.pack.artifact_size);
  TEST_ASSERT_NOT_EQUAL(input.pack.artifact, output.pack.artifact);
  tp_job_worker_proto_response_free(&output);
  free(bytes);
}

void test_export_terminal_roundtrip_counts_and_flags(void) {
  tp_job_worker_proto_response input;
  memset(&input, 0, sizeof input);
  input.kind = TP_SESSION_JOB_EXPORT;
  input.session_instance_generation = 9U;
  input.request_id = 77U;
  input.state = TP_SESSION_JOB_FAILED;
  input.status = TP_STATUS_FILE_IO_FAILED;
  input.elapsed_ms = 8.0;
  memcpy(input.error.msg, "writer failed", 14U);
  input.export_result.targets = 3;
  input.export_result.files = 2;
  input.export_result.notices = 1;
  input.export_result.atlases_ok = 1;
  input.export_result.atlases_failed = 1;
  input.export_result.atlases_skipped = 1;
  input.export_result.partial_publication = true;
  input.export_result.publication_uncertain = true;
  memcpy(input.export_result.first_error, "disk full", 10U);

  uint8_t *bytes = NULL;
  size_t length = 0U;
  tp_error err = {{0}};
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      TP_STATUS_OK,
      tp_job_worker_proto_encode_response(&input, &bytes, &length, &err),
      err.msg);
  tp_job_worker_proto_response output;
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      TP_STATUS_OK,
      tp_job_worker_proto_decode_response(bytes, length, &output, &err),
      err.msg);
  TEST_ASSERT_EQUAL_INT(3, output.export_result.targets);
  TEST_ASSERT_EQUAL_INT(2, output.export_result.files);
  TEST_ASSERT_TRUE(output.export_result.partial_publication);
  TEST_ASSERT_TRUE(output.export_result.publication_uncertain);
  TEST_ASSERT_EQUAL_STRING("disk full", output.export_result.first_error);
  tp_job_worker_proto_response_free(&output);
  free(bytes);
}

void test_progress_roundtrip_is_fixed_and_bounded(void) {
  tp_job_worker_proto_progress input = {42U, 3, 9,
                                        TP_JOB_WORKER_PHASE_IMAGE_DECODE};
  uint8_t *bytes = NULL;
  size_t length = 0U;
  tp_error err = {{0}};
  TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_job_worker_proto_encode_progress(
                                          &input, &bytes, &length, &err));
  TEST_ASSERT_EQUAL_UINT64(36U, (uint64_t)length);
  tp_job_worker_proto_progress output;
  TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_job_worker_proto_decode_progress(
                                          bytes, length, &output, &err));
  TEST_ASSERT_EQUAL_UINT64(42U, output.request_id);
  TEST_ASSERT_EQUAL_INT(3, output.current);
  TEST_ASSERT_EQUAL_INT(9, output.total);
  TEST_ASSERT_EQUAL_INT(TP_JOB_WORKER_PHASE_IMAGE_DECODE, output.phase);
  free(bytes);
}

void test_stream_parser_accepts_fragmented_progress_then_terminal(void) {
  tp_job_worker_proto_progress progress = {42U, 1, 2,
                                           TP_JOB_WORKER_PHASE_SOURCE_READ};
  uint8_t *progress_bytes = NULL;
  size_t progress_len = 0U;
  uint8_t *terminal_bytes = NULL;
  size_t terminal_len = 0U;
  tp_error err = {{0}};
  TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                        tp_job_worker_proto_encode_progress(
                            &progress, &progress_bytes, &progress_len, &err));
  terminal_bytes = encode_pack(&terminal_len);

  tp_job_worker_proto_stream stream;
  tp_job_worker_proto_stream_init(&stream);
  TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_job_worker_proto_stream_feed(
                                          &stream, progress_bytes, 5U, &err));
  tp_job_worker_proto_stream_message message;
  bool ready = true;
  TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_job_worker_proto_stream_next(
                                          &stream, &message, &ready, &err));
  TEST_ASSERT_FALSE(ready);
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_OK, tp_job_worker_proto_stream_feed(
                        &stream, progress_bytes + 5U, progress_len - 5U, &err));
  TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                        tp_job_worker_proto_stream_feed(&stream, terminal_bytes,
                                                        terminal_len, &err));
  TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_job_worker_proto_stream_next(
                                          &stream, &message, &ready, &err));
  TEST_ASSERT_TRUE(ready);
  TEST_ASSERT_EQUAL_INT(TP_JOB_WORKER_STREAM_PROGRESS, message.kind);
  tp_job_worker_proto_stream_message_free(&message);
  TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_job_worker_proto_stream_next(
                                          &stream, &message, &ready, &err));
  TEST_ASSERT_TRUE(ready);
  TEST_ASSERT_EQUAL_INT(TP_JOB_WORKER_STREAM_TERMINAL, message.kind);
  TEST_ASSERT_EQUAL_UINT64(8U, (uint64_t)message.terminal.pack.artifact_size);
  tp_job_worker_proto_stream_message_free(&message);
  tp_job_worker_proto_stream_destroy(&stream);
  free(progress_bytes);
  free(terminal_bytes);
}

void test_every_truncated_request_and_terminal_is_rejected(void) {
  tp_job_worker_proto_request request = sample_request(TP_SESSION_JOB_PACK);
  uint8_t *request_bytes = NULL;
  size_t request_len = 0U;
  tp_error err = {{0}};
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_OK, tp_job_worker_proto_encode_request(&request, &request_bytes,
                                                       &request_len, &err));
  uint8_t *terminal_bytes = NULL;
  size_t terminal_len = 0U;
  terminal_bytes = encode_pack(&terminal_len);
  for (size_t i = 0U; i < request_len; ++i) {
    tp_job_worker_proto_request out;
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_job_worker_proto_decode_request(
                                            request_bytes, i, &out, &err));
  }
  for (size_t i = 0U; i < terminal_len; ++i) {
    tp_job_worker_proto_response out;
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_job_worker_proto_decode_response(
                                            terminal_bytes, i, &out, &err));
  }
  free(request_bytes);
  free(terminal_bytes);
}

void test_bad_magic_version_and_oversized_fields_fail_closed(void) {
  size_t length = 0U;
  uint8_t *bytes = encode_pack(&length);
  tp_job_worker_proto_response out;
  tp_error err = {{0}};
  bytes[0] ^= 0xffU;
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_BAD_MAGIC,
      tp_job_worker_proto_decode_response(bytes, length, &out, &err));
  free(bytes);

  tp_job_worker_proto_request request = sample_request(TP_SESSION_JOB_PACK);
  TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_job_worker_proto_encode_request(
                                          &request, &bytes, &length, &err));
  /* Request project_json_len: frame(12) + kind/gen/request(20). */
  put_u32_le(bytes + 32U,
             (uint32_t)(TP_JOB_WORKER_PROTO_MAX_PROJECT_JSON_BYTES + 1U));
  tp_job_worker_proto_request request_out;
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_INVALID_ARGUMENT,
      tp_job_worker_proto_decode_request(bytes, length, &request_out, &err));
  free(bytes);

  bytes = encode_pack(&length);
  put_u32_le(bytes + 8U, (uint32_t)(TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES + 1U));
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_OUT_OF_BOUNDS,
      tp_job_worker_proto_decode_response(bytes, length, &out, &err));
  free(bytes);

  bytes = encode_pack(&length);
  put_u16_le(bytes + 4U, (uint16_t)(TP_JOB_WORKER_PROTO_VERSION + 1U));
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_BAD_VERSION,
      tp_job_worker_proto_decode_response(bytes, length, &out, &err));
  free(bytes);

  bytes = encode_pack(&length);
  /* Pack name_count: frame(12) + common(52) + offset 116. */
  put_u32_le(bytes + 180U, TP_JOB_WORKER_PROTO_MAX_NAME_ENTRIES + 1U);
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_INVALID_ARGUMENT,
      tp_job_worker_proto_decode_response(bytes, length, &out, &err));
  free(bytes);

  bytes = encode_pack(&length);
  /* Pack artifact_size immediately follows name_count. */
  put_u32_le(bytes + 184U,
             (uint32_t)(TP_JOB_WORKER_PROTO_MAX_ARTIFACT_BYTES + 1U));
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_INVALID_ARGUMENT,
      tp_job_worker_proto_decode_response(bytes, length, &out, &err));
  free(bytes);
}

void test_invalid_utf8_name_is_rejected_on_encode(void) {
  static const char invalid_name[] = {(char)0xc0, (char)0xaf, '\0'};
  tp_job_worker_proto_name name = {1U, invalid_name};
  tp_job_worker_proto_response response = sample_pack_response();
  response.pack.names = &name;
  response.pack.name_count = 1U;
  uint8_t *bytes = NULL;
  size_t length = 0U;
  tp_error err = {{0}};
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_INVALID_UTF8,
      tp_job_worker_proto_encode_response(&response, &bytes, &length, &err));
  TEST_ASSERT_NULL(bytes);
}

void test_encode_caps_reject_oversized_project_names_counts_and_artifact(void) {
  tp_job_worker_proto_request request = sample_request(TP_SESSION_JOB_PACK);
  request.project_json_len = TP_JOB_WORKER_PROTO_MAX_PROJECT_JSON_BYTES + 1U;
  uint8_t *bytes = NULL;
  size_t length = 0U;
  tp_error err = {{0}};
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_INVALID_ARGUMENT,
      tp_job_worker_proto_encode_request(&request, &bytes, &length, &err));
  TEST_ASSERT_NULL(bytes);

  tp_job_worker_proto_response response = sample_pack_response();
  response.pack.name_count = TP_JOB_WORKER_PROTO_MAX_NAME_ENTRIES + 1U;
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_INVALID_ARGUMENT,
      tp_job_worker_proto_encode_response(&response, &bytes, &length, &err));
  response = sample_pack_response();
  response.pack.artifact_size = TP_JOB_WORKER_PROTO_MAX_ARTIFACT_BYTES + 1U;
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_INVALID_ARGUMENT,
      tp_job_worker_proto_encode_response(&response, &bytes, &length, &err));
}

void test_stream_cumulative_and_progress_count_caps_fail_closed(void) {
  tp_job_worker_proto_stream stream;
  tp_job_worker_proto_stream_init(&stream);
  stream.cumulative_bytes = TP_JOB_WORKER_PROTO_MAX_STREAM_BYTES;
  const uint8_t byte = 0U;
  tp_error err = {{0}};
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_OUT_OF_BOUNDS,
      tp_job_worker_proto_stream_feed(&stream, &byte, 1U, &err));
  tp_job_worker_proto_stream_destroy(&stream);

  tp_job_worker_proto_progress progress = {42U, 1, 1,
                                           TP_JOB_WORKER_PHASE_BUILD};
  uint8_t *bytes = NULL;
  size_t length = 0U;
  TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_job_worker_proto_encode_progress(
                                          &progress, &bytes, &length, &err));
  tp_job_worker_proto_stream_init(&stream);
  stream.progress_frames = TP_JOB_WORKER_PROTO_MAX_PROGRESS_FRAMES;
  TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_job_worker_proto_stream_feed(
                                          &stream, bytes, length, &err));
  tp_job_worker_proto_stream_message message;
  bool ready = false;
  TEST_ASSERT_EQUAL_INT(
      TP_STATUS_OUT_OF_BOUNDS,
      tp_job_worker_proto_stream_next(&stream, &message, &ready, &err));
  TEST_ASSERT_FALSE(ready);
  tp_job_worker_proto_stream_destroy(&stream);
  free(bytes);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_request_roundtrip_pack_and_export);
  RUN_TEST(test_pack_terminal_roundtrip_owns_artifact_and_utf8_name_map);
  RUN_TEST(test_export_terminal_roundtrip_counts_and_flags);
  RUN_TEST(test_progress_roundtrip_is_fixed_and_bounded);
  RUN_TEST(test_stream_parser_accepts_fragmented_progress_then_terminal);
  RUN_TEST(test_every_truncated_request_and_terminal_is_rejected);
  RUN_TEST(test_bad_magic_version_and_oversized_fields_fail_closed);
  RUN_TEST(test_invalid_utf8_name_is_rejected_on_encode);
  RUN_TEST(test_encode_caps_reject_oversized_project_names_counts_and_artifact);
  RUN_TEST(test_stream_cumulative_and_progress_count_caps_fail_closed);
  return UNITY_END();
}
