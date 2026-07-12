/* C0-02 tasks 3/5/6: versioned transaction request/result JSON round-trip
 * (decode -> encode byte comparison), the fault vocabulary with distinct tokens,
 * full-batch validation ordering, and the before/after diff shapes. Executable
 * form of C0-02-contract.md §3, §5, §6. */

/* System headers BEFORE unity.h: unity pulls <stdnoreturn.h>, which #defines
 * `noreturn` and then collides with ucrt <stdlib.h>'s __declspec(noreturn). */
#include <stdlib.h>
#include <string.h>

#include "tp_c0/tp_c0_txn.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* 32-hex bodies built in 10-char chunks so the lengths are auditable. */
#define Z30 "0000000000" "0000000000" "0000000000"
#define TID "0123456789abcdef" "0123456789abcdef"
#define ATLAS2 "atlas_" Z30 "02"
#define TARGET1 "target_" Z30 "01"
#define ANIM1 "anim_" Z30 "01"
#define ANIMAA "anim_" Z30 "aa"
#define BADATLAS "atlas_zz" Z30
#define FRAME_A "aaaaaaaaaa" "aaaaaaaaaa" "aaaaaaaaaa" "aa"
#define FRAME_B "bbbbbbbbbb" "bbbbbbbbbb" "bbbbbbbbbb" "bb"

/* ---- canonical golden fixtures ------------------------------------------- */

static const char *REQ_CREATE =
    "{\n"
    "  \"schema\": 1,\n"
    "  \"transaction\": {\n"
    "    \"author\": \"cli\",\n"
    "    \"expected_revision\": 7,\n"
    "    \"id\": \"" TID "\",\n"
    "    \"label\": \"make idle\",\n"
    "    \"operations\": [\n"
    "      {\n"
    "        \"op\": \"animation.create\",\n"
    "        \"anim_id\": \"" ANIM1 "\",\n"
    "        \"atlas_id\": \"" ATLAS2 "\",\n"
    "        \"fps\": 12,\n"
    "        \"frames\": [\n"
    "          \"" FRAME_A "\",\n"
    "          \"" FRAME_B "\"\n"
    "        ],\n"
    "        \"id\": \"enemy_idle\",\n"
    "        \"playback\": 1\n"
    "      }\n"
    "    ]\n"
    "  }\n"
    "}\n";

/* Same operation, object keys shuffled + compact: must canonicalize to REQ_CREATE.
 * Array element ORDER is preserved (frame order is semantic). */
static const char *REQ_CREATE_MESSY =
    "{ \"transaction\": { \"operations\": [ { \"op\": \"animation.create\","
    " \"playback\": 1, \"id\": \"enemy_idle\", \"frames\": [\"" FRAME_A "\",\"" FRAME_B "\"],"
    " \"fps\": 12, \"atlas_id\": \"" ATLAS2 "\", \"anim_id\": \"" ANIM1 "\" } ],"
    " \"label\": \"make idle\", \"id\": \"" TID "\", \"expected_revision\": 7,"
    " \"author\": \"cli\" }, \"schema\": 1 }";

static const char *REQ_MOVE =
    "{\n"
    "  \"schema\": 1,\n"
    "  \"transaction\": {\n"
    "    \"expected_revision\": 3,\n"
    "    \"id\": \"" TID "\",\n"
    "    \"operations\": [\n"
    "      {\n"
    "        \"op\": \"animation.frame.move\",\n"
    "        \"anim_id\": \"" ANIMAA "\",\n"
    "        \"from_index\": 0,\n"
    "        \"to_index\": 2\n"
    "      }\n"
    "    ]\n"
    "  }\n"
    "}\n";

/* Committed result: a create diff (after + position) and a move diff (indices). */
static const char *RES_OK =
    "{\n"
    "  \"schema\": 1,\n"
    "  \"result\": {\n"
    "    \"operations\": [\n"
    "      {\n"
    "        \"op\": \"atlas.create\",\n"
    "        \"atlas_id\": \"" ATLAS2 "\",\n"
    "        \"diff\": {\n"
    "          \"after\": {\n"
    "            \"atlas_id\": \"" ATLAS2 "\",\n"
    "            \"name\": \"hud\"\n"
    "          },\n"
    "          \"class\": \"create\",\n"
    "          \"position\": 1\n"
    "        }\n"
    "      },\n"
    "      {\n"
    "        \"op\": \"animation.frame.move\",\n"
    "        \"anim_id\": \"" ANIMAA "\",\n"
    "        \"diff\": {\n"
    "          \"after_index\": 2,\n"
    "          \"before_index\": 0,\n"
    "          \"class\": \"move\"\n"
    "        }\n"
    "      }\n"
    "    ],\n"
    "    \"revision\": 8,\n"
    "    \"status\": \"committed\",\n"
    "    \"transaction_id\": \"" TID "\"\n"
    "  }\n"
    "}\n";

/* Rejected result: one envelope-level revision error. */
static const char *RES_REJ =
    "{\n"
    "  \"schema\": 1,\n"
    "  \"result\": {\n"
    "    \"errors\": [\n"
    "      {\n"
    "        \"code\": \"revision_conflict\",\n"
    "        \"message\": \"stale base\",\n"
    "        \"op_index\": -1\n"
    "      }\n"
    "    ],\n"
    "    \"revision\": 8,\n"
    "    \"status\": \"rejected\",\n"
    "    \"transaction_id\": \"" TID "\"\n"
    "  }\n"
    "}\n";

/* ---- round-trip helpers -------------------------------------------------- */

static void rt_request(const char *golden, const char *input) {
    tp_c0_detail d = TP_C0_OK;
    tp_error err = {0};
    tp_c0_txn_request *req = tp_c0_txn_request_decode(input, &d, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_C0_OK, d, err.msg);
    TEST_ASSERT_NOT_NULL(req);
    char *out = tp_c0_txn_request_encode(req, &d);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING(golden, out);
    free(out);
    tp_c0_txn_request_free(req);
}

static void rt_result(const char *golden) {
    tp_c0_detail d = TP_C0_OK;
    tp_error err = {0};
    tp_c0_txn_result *res = tp_c0_txn_result_decode(golden, &d, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_C0_OK, d, err.msg);
    TEST_ASSERT_NOT_NULL(res);
    char *out = tp_c0_txn_result_encode(res, &d);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING(golden, out);
    free(out);
    tp_c0_txn_result_free(res);
}

void test_request_roundtrip(void) {
    rt_request(REQ_CREATE, REQ_CREATE);
    rt_request(REQ_MOVE, REQ_MOVE);
}

void test_request_canonicalizes(void) { rt_request(REQ_CREATE, REQ_CREATE_MESSY); }

void test_result_roundtrip(void) {
    rt_result(RES_OK);
    rt_result(RES_REJ);
}

/* ---- structural decode faults -------------------------------------------- */

static tp_c0_detail decode_req_fault(const char *json) {
    tp_c0_detail d = TP_C0_OK;
    tp_error err = {0};
    tp_c0_txn_request *req = tp_c0_txn_request_decode(json, &d, &err);
    TEST_ASSERT_NULL(req);
    return d;
}

void test_decode_faults(void) {
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_BAD_JSON, decode_req_fault("{ not json"));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_TXN_BAD_VERSION,
                          decode_req_fault("{\"schema\":2,\"transaction\":{\"id\":\"" TID
                                           "\",\"expected_revision\":0,\"operations\":[]}}"));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_TXN_MISSING_FIELD, decode_req_fault("{\"transaction\":{}}"));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_TXN_BAD_ID,
                          decode_req_fault("{\"schema\":1,\"transaction\":{\"id\":\"XYZ\","
                                           "\"expected_revision\":0,\"operations\":[]}}"));
    /* unknown-field policy is REJECT, demonstrated at the envelope level */
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_UNKNOWN_FIELD,
                          decode_req_fault("{\"schema\":1,\"priority\":5,\"transaction\":{\"id\":\"" TID
                                           "\",\"expected_revision\":0,\"operations\":[]}}"));
}

/* ---- per-op validation faults (collected in stable order) ---------------- */

static tp_c0_txn_request *decode_ok(const char *json) {
    tp_c0_detail d = TP_C0_OK;
    tp_error err = {0};
    tp_c0_txn_request *req = tp_c0_txn_request_decode(json, &d, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_C0_OK, d, err.msg);
    TEST_ASSERT_NOT_NULL(req);
    return req;
}

void test_unknown_operation(void) {
    tp_c0_txn_request *req = decode_ok("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                       "\",\"expected_revision\":0,\"operations\":[{\"op\":\"atlas.explode\","
                                       "\"atlas_id\":\"" ATLAS2 "\"}]}}");
    static tp_c0_txn_result res;
    tp_error err = {0};
    tp_c0_detail d = tp_c0_txn_validate(req, 0, NULL, 0, &res, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_OP_UNKNOWN, d);
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT(1, res.error_count);
    TEST_ASSERT_EQUAL_INT(0, res.errors[0].op_index);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_OP_UNKNOWN, res.errors[0].code);
    tp_c0_txn_request_free(req);
}

void test_op_unknown_field(void) {
    tp_c0_txn_request *req = decode_ok("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                       "\",\"expected_revision\":0,\"operations\":[{\"op\":\"atlas.remove\","
                                       "\"atlas_id\":\"" ATLAS2 "\",\"bogus\":true}]}}");
    static tp_c0_txn_result res;
    tp_error err = {0};
    tp_c0_detail d = tp_c0_txn_validate(req, 0, NULL, 0, &res, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_UNKNOWN_FIELD, d);
    TEST_ASSERT_EQUAL_INT(1, res.error_count);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_UNKNOWN_FIELD, res.errors[0].code);
    tp_c0_txn_request_free(req);
}

void test_malformed_id(void) {
    tp_c0_txn_request *req = decode_ok("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                       "\",\"expected_revision\":0,\"operations\":[{\"op\":\"atlas.remove\","
                                       "\"atlas_id\":\"" BADATLAS "\"}]}}");
    static tp_c0_txn_result res;
    tp_error err = {0};
    tp_c0_detail d = tp_c0_txn_validate(req, 0, NULL, 0, &res, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_ID_BAD_HEX, d);
    tp_c0_txn_request_free(req);
}

void test_selector_is_not_canonical(void) {
    tp_c0_txn_request *req = decode_ok("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                       "\",\"expected_revision\":0,\"operations\":[{\"op\":\"atlas.remove\","
                                       "\"selector\":{\"by\":\"name\",\"value\":\"hud\"}}]}}");
    /* validate flags it; canonical encode refuses it -- both selector_unresolved */
    static tp_c0_txn_result res;
    tp_error err = {0};
    tp_c0_detail d = tp_c0_txn_validate(req, 0, NULL, 0, &res, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_SELECTOR_UNRESOLVED, d);
    tp_c0_detail ed = TP_C0_OK;
    char *out = tp_c0_txn_request_encode(req, &ed);
    TEST_ASSERT_NULL(out);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_SELECTOR_UNRESOLVED, ed);
    tp_c0_txn_request_free(req);
}

/* ---- revision precondition ----------------------------------------------- */

void test_revision_conflict_and_invalid(void) {
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_revision_check(8, 8, &err));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_REVISION_CONFLICT, tp_c0_revision_check(5, 8, &err));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_INVALID_REVISION, tp_c0_revision_check(9, 8, &err));

    /* whole-batch: a revision mismatch rejects alone, before per-op checks. Even
     * with a bad op present, only the revision error is reported. */
    tp_c0_txn_request *req = decode_ok("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                       "\",\"expected_revision\":5,\"operations\":[{\"op\":\"atlas.explode\"}]}}");
    static tp_c0_txn_result res;
    tp_c0_detail d = tp_c0_txn_validate(req, 8, NULL, 0, &res, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_REVISION_CONFLICT, d);
    TEST_ASSERT_EQUAL_INT(1, res.error_count);
    TEST_ASSERT_EQUAL_INT(-1, res.errors[0].op_index);
    TEST_ASSERT_EQUAL_INT(8, res.revision); /* unchanged */
    tp_c0_txn_request_free(req);
}

/* ---- idempotency retention set ------------------------------------------- */

void test_duplicate_transaction_id(void) {
    tp_c0_txn_idset set = {0};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_txn_idset_add(&set, TID, &err));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_TXN_DUPLICATE_ID, tp_c0_txn_idset_add(&set, TID, &err));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_TXN_BAD_ID, tp_c0_txn_idset_add(&set, "nothex", &err));
    /* a different id is fine */
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_txn_idset_add(&set, "ffffffffffffffffffffffffffffffff", &err));
}

/* ---- stable error ordering across a multi-error request ------------------ */

void test_stable_error_ordering(void) {
    /* op0: unknown field; op1: malformed id; op2: unknown operation. */
    tp_c0_txn_request *req =
        decode_ok("{\"schema\":1,\"transaction\":{\"id\":\"" TID "\",\"expected_revision\":0,\"operations\":["
                  "{\"op\":\"atlas.remove\",\"atlas_id\":\"" ATLAS2 "\",\"bogus\":true},"
                  "{\"op\":\"atlas.remove\",\"atlas_id\":\"" BADATLAS "\"},"
                  "{\"op\":\"atlas.explode\"}]}}");
    static tp_c0_txn_result res;
    tp_error err = {0};
    tp_c0_detail d = tp_c0_txn_validate(req, 0, NULL, 0, &res, &err);
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT(3, res.error_count);
    TEST_ASSERT_EQUAL_INT(0, res.errors[0].op_index);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_UNKNOWN_FIELD, res.errors[0].code);
    TEST_ASSERT_EQUAL_INT(1, res.errors[1].op_index);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_ID_BAD_HEX, res.errors[1].code);
    TEST_ASSERT_EQUAL_INT(2, res.errors[2].op_index);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_OP_UNKNOWN, res.errors[2].code);
    TEST_ASSERT_EQUAL_INT(res.errors[0].code, d); /* returns the first error's code */

    /* the rejected result encodes deterministically */
    tp_c0_detail ed = TP_C0_OK;
    char *out = tp_c0_txn_result_encode(&res, &ed);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(strstr(out, "\"status\": \"rejected\"") != NULL);
    free(out);
    tp_c0_txn_request_free(req);
}

/* ---- committed stub + id-reference existence ----------------------------- */

void test_validate_commits(void) {
    tp_c0_txn_request *req = decode_ok(REQ_CREATE);
    static tp_c0_txn_result res;
    tp_error err = {0};
    /* atlas.create mints its own atlas_id; the parent id needs no table here. */
    tp_c0_detail d = tp_c0_txn_validate(req, 7, NULL, 0, &res, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    TEST_ASSERT_TRUE(res.committed);
    TEST_ASSERT_EQUAL_INT(8, res.revision); /* one new revision */
    TEST_ASSERT_EQUAL_INT(1, res.op_count);
    tp_c0_txn_request_free(req);
}

void test_unknown_id_reference(void) {
    /* atlas.remove targets an existing id; an empty-but-provided table makes the
     * reference dangle -> selector_unresolved (spec §5.6). */
    tp_c0_txn_request *req = decode_ok("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                       "\",\"expected_revision\":0,\"operations\":[{\"op\":\"atlas.remove\","
                                       "\"atlas_id\":\"" ATLAS2 "\"}]}}");
    tp_c0_entity_ref other[1];
    other[0].kind = TP_C0_ID_KIND_ATLAS;
    other[0].id = tp_c0_id128_nil();
    other[0].id.bytes[15] = 0x77; /* some other atlas, not ATLAS2 */
    other[0].name = "world";
    other[0].index = 0;
    static tp_c0_txn_result res;
    tp_error err = {0};
    tp_c0_detail d = tp_c0_txn_validate(req, 0, other, 1, &res, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_SELECTOR_UNRESOLVED, d);
    tp_c0_txn_request_free(req);
}

/* ---- target ops carry a string exporter_id, not an id token (F1) ---------- */

static bool addr_has(const tp_c0_result_op *ro, const char *key) {
    for (int i = 0; i < ro->addr_count; i++) {
        if (strcmp(ro->addr[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

void test_target_create_commits_with_exporter_id(void) {
    /* exporter_id is a registry NAME ("defold"), not a 32-hex id. It must not be
     * routed through the id validator, or target.create can never commit. */
    tp_c0_txn_request *req = decode_ok("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                       "\",\"expected_revision\":0,\"operations\":[{\"op\":\"target.create\","
                                       "\"atlas_id\":\"" ATLAS2 "\",\"target_id\":\"" TARGET1 "\","
                                       "\"exporter_id\":\"defold\",\"out_path\":\"hud.json\",\"enabled\":true}]}}");
    static tp_c0_txn_result res;
    tp_error err = {0};
    tp_c0_detail d = tp_c0_txn_validate(req, 0, NULL, 0, &res, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_C0_OK, d, err.msg);
    TEST_ASSERT_TRUE(res.committed);
    TEST_ASSERT_EQUAL_INT(1, res.op_count);
    /* only the real addressing ids echo; exporter_id/out_path do not */
    TEST_ASSERT_EQUAL_INT(2, res.ops[0].addr_count);
    TEST_ASSERT_TRUE(addr_has(&res.ops[0], "atlas_id"));
    TEST_ASSERT_TRUE(addr_has(&res.ops[0], "target_id"));
    TEST_ASSERT_FALSE(addr_has(&res.ops[0], "exporter_id"));
    tp_c0_txn_request_free(req);
}

void test_target_set_commits_with_exporter_id(void) {
    tp_c0_txn_request *req = decode_ok("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                       "\",\"expected_revision\":0,\"operations\":[{\"op\":\"target.set\","
                                       "\"target_id\":\"" TARGET1 "\",\"exporter_id\":\"json-neotolis\","
                                       "\"out_path\":\"hud.json\",\"enabled\":false}]}}");
    static tp_c0_txn_result res;
    tp_error err = {0};
    tp_c0_detail d = tp_c0_txn_validate(req, 0, NULL, 0, &res, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_C0_OK, d, err.msg);
    TEST_ASSERT_TRUE(res.committed);
    TEST_ASSERT_EQUAL_INT(1, res.ops[0].addr_count);
    TEST_ASSERT_TRUE(addr_has(&res.ops[0], "target_id"));
    TEST_ASSERT_FALSE(addr_has(&res.ops[0], "exporter_id"));
    tp_c0_txn_request_free(req);
}

/* ---- number classification: UB-free + cross-OS byte-stable (F2) ----------- */

void test_number_classification_cross_os(void) {
    /* A large integral value stays INT and emits width-stable digits (not "5e+09"
     * as 32-bit-long Windows would): the cross-OS determinism pin (contract §3). */
    tp_c0_txn_request *req = decode_ok("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                       "\",\"expected_revision\":0,\"operations\":[{\"op\":\"animation.settings.set\","
                                       "\"anim_id\":\"" ANIM1 "\",\"fps\":5000000000}]}}");
    tp_c0_detail d = TP_C0_OK;
    char *out = tp_c0_txn_request_encode(req, &d);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE_MESSAGE(strstr(out, "\"fps\": 5000000000") != NULL, out);
    free(out);
    tp_c0_txn_request_free(req);

    /* A value beyond 2^53 (here inf-scale) falls back to NUM and is emitted via
     * %.9g -- never a UB double->int cast (an UBSan abort in Debug CI). */
    tp_c0_txn_request *req2 = decode_ok("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                        "\",\"expected_revision\":0,\"operations\":[{\"op\":\"animation.settings.set\","
                                        "\"anim_id\":\"" ANIM1 "\",\"fps\":1e300}]}}");
    char *out2 = tp_c0_txn_request_encode(req2, &d);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    TEST_ASSERT_NOT_NULL(out2);
    TEST_ASSERT_TRUE_MESSAGE(strstr(out2, "1e+300") != NULL, out2); /* NUM path, no abort */
    free(out2);
    tp_c0_txn_request_free(req2);
}

void test_expected_revision_out_of_range(void) {
    /* An out-of-range expected_revision is a structured txn_bad_type, never a UB
     * double->int64 cast. */
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_TXN_BAD_TYPE,
                          decode_req_fault("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                           "\",\"expected_revision\":1e300,\"operations\":[]}}"));
}

/* ---- label/author are strictly typed + bounded, never silently dropped (F3) - */

void test_label_author_strict(void) {
    /* present-but-wrong-type is REJECT, not a silent drop */
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_TXN_BAD_TYPE,
                          decode_req_fault("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                           "\",\"expected_revision\":0,\"label\":42,\"operations\":[]}}"));
    /* an over-long label is buffer_too_small, not a silent truncation */
    char big[220];
    memset(big, 'x', 200);
    big[200] = '\0';
    char json[512];
    (void)snprintf(json, sizeof json,
                   "{\"schema\":1,\"transaction\":{\"id\":\"" TID
                   "\",\"expected_revision\":0,\"label\":\"%s\",\"operations\":[]}}",
                   big);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_BUFFER_TOO_SMALL, decode_req_fault(json));
}

/* ---- collect-all overflow is marked, not silently truncated (F4) ---------- */

void test_errors_truncated_marker(void) {
    /* 32 ops * 2 unknown fields = 64 faults > TP_C0_MAX_ERRORS (32). The dropped
     * faults must be flagged so a client does not resubmit into a hidden reject. */
    char json[8192];
    int off = snprintf(json, sizeof json,
                       "{\"schema\":1,\"transaction\":{\"id\":\"" TID "\",\"expected_revision\":0,\"operations\":[");
    for (int i = 0; i < TP_C0_MAX_OPS; i++) {
        off += snprintf(json + off, sizeof json - (size_t)off, "%s{\"op\":\"atlas.remove\",\"b1\":true,\"b2\":true}",
                        i ? "," : "");
    }
    (void)snprintf(json + off, sizeof json - (size_t)off, "]}}");

    tp_c0_txn_request *req = decode_ok(json);
    static tp_c0_txn_result res;
    tp_error err = {0};
    (void)tp_c0_txn_validate(req, 0, NULL, 0, &res, &err);
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT(TP_C0_MAX_ERRORS, res.error_count); /* capped */
    TEST_ASSERT_TRUE(res.errors_truncated);

    /* the marker is emitted and round-trips through decode */
    tp_c0_detail ed = TP_C0_OK;
    char *out = tp_c0_txn_result_encode(&res, &ed);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE_MESSAGE(strstr(out, "\"errors_truncated\": true") != NULL, out);
    tp_c0_txn_result *rd = tp_c0_txn_result_decode(out, &ed, &err);
    TEST_ASSERT_NOT_NULL(rd);
    TEST_ASSERT_TRUE(rd->errors_truncated);
    tp_c0_txn_result_free(rd);
    free(out);
    tp_c0_txn_request_free(req);
}

/* ---- duplicate object keys are rejected, not both accepted (F5) ----------- */

void test_duplicate_field_rejected(void) {
    /* {"name":"a","name":"b"}: cJSON keeps both; without a dedup check both pass
     * validation and the encoder emits a record cross-language consumers disagree
     * on (first- vs last-wins). */
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_TXN_BAD_TYPE,
                          decode_req_fault("{\"schema\":1,\"transaction\":{\"id\":\"" TID
                                           "\",\"expected_revision\":0,\"operations\":[{\"op\":\"atlas.rename\","
                                           "\"atlas_id\":\"" ATLAS2 "\",\"name\":\"a\",\"name\":\"b\"}]}}"));
}

/* ---- result decode tolerates the whole append-only token space (F6) ------- */

void test_result_decodes_last_error_token(void) {
    /* The LAST real token (currently invalid_revision) must decode: code_from_str
     * iterates [0, TP_C0_DETAIL_COUNT), not a hardcoded last enumerator, so a
     * client's structured error report survives version skew. */
    const char *json = "{\"schema\":1,\"result\":{\"errors\":[{\"code\":\"invalid_revision\","
                       "\"message\":\"x\",\"op_index\":-1}],\"revision\":8,\"status\":\"rejected\","
                       "\"transaction_id\":\"" TID "\"}}";
    tp_c0_detail d = TP_C0_OK;
    tp_error err = {0};
    tp_c0_txn_result *res = tp_c0_txn_result_decode(json, &d, &err);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_INVALID_REVISION, res->errors[0].code);
    tp_c0_txn_result_free(res);
}

/* ---- result status + diff shapes are strictly typed (F7, F8) -------------- */

void test_result_bad_status(void) {
    /* a status typo must not silently read as rejected */
    const char *json = "{\"schema\":1,\"result\":{\"errors\":[],\"revision\":8,\"status\":\"comitted\","
                       "\"transaction_id\":\"" TID "\"}}";
    tp_c0_detail d = TP_C0_OK;
    tp_error err = {0};
    tp_c0_txn_result *res = tp_c0_txn_result_decode(json, &d, &err);
    TEST_ASSERT_NULL(res);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_TXN_BAD_TYPE, d);
}

void test_result_diff_before_wrong_type(void) {
    /* a present-but-non-object "before" must fault, not be silently skipped */
    const char *json = "{\"schema\":1,\"result\":{\"operations\":[{\"op\":\"atlas.create\",\"atlas_id\":\"" ATLAS2
                       "\",\"diff\":{\"before\":\"oops\",\"class\":\"create\"}}],\"revision\":8,"
                       "\"status\":\"committed\",\"transaction_id\":\"" TID "\"}}";
    tp_c0_detail d = TP_C0_OK;
    tp_error err = {0};
    tp_c0_txn_result *res = tp_c0_txn_result_decode(json, &d, &err);
    TEST_ASSERT_NULL(res);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_TXN_BAD_TYPE, d);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_request_roundtrip);
    RUN_TEST(test_request_canonicalizes);
    RUN_TEST(test_result_roundtrip);
    RUN_TEST(test_decode_faults);
    RUN_TEST(test_unknown_operation);
    RUN_TEST(test_op_unknown_field);
    RUN_TEST(test_malformed_id);
    RUN_TEST(test_selector_is_not_canonical);
    RUN_TEST(test_revision_conflict_and_invalid);
    RUN_TEST(test_duplicate_transaction_id);
    RUN_TEST(test_stable_error_ordering);
    RUN_TEST(test_validate_commits);
    RUN_TEST(test_unknown_id_reference);
    RUN_TEST(test_target_create_commits_with_exporter_id);
    RUN_TEST(test_target_set_commits_with_exporter_id);
    RUN_TEST(test_number_classification_cross_os);
    RUN_TEST(test_expected_revision_out_of_range);
    RUN_TEST(test_label_author_strict);
    RUN_TEST(test_errors_truncated_marker);
    RUN_TEST(test_duplicate_field_rejected);
    RUN_TEST(test_result_decodes_last_error_token);
    RUN_TEST(test_result_bad_status);
    RUN_TEST(test_result_diff_before_wrong_type);
    return UNITY_END();
}
