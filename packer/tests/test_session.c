#define _CRT_SECURE_NO_WARNINGS

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "tp_core/tp_operation.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_project_lease.h"
#include "tp_project_identity_internal.h"
#include "tp_core/tp_recovery.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_source_plan.h"
#include "tp_core/tp_sprite_index.h"
#include "tp_core/tp_transaction.h"
#include "tp_journal_internal.h"
#include "tp_project_internal.h"
#include "tp_project_mutation_internal.h"
#include "tp_recovery_internal.h"
#include "tp_session_internal.h"
#include "tp_source_plan_internal.h"
#include "tp_txn_internal.h"
#include "unity.h"

static const char *g_scratch = NULL;

void setUp(void) {
    tp_journal__test_set_record_limit(0U);
}
void tearDown(void) {
    tp_journal__test_set_record_limit(0U);
}

static void assert_canonical_path(const char *input, const char *actual) {
    char expected[TP_IDENTITY_PATH_MAX];
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_identity_path_canonical(input, expected, sizeof expected, &error),
        error.msg);
    TEST_ASSERT_EQUAL_STRING(expected, actual);
}

static int deterministic_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *next = (uint8_t *)ctx;
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(*next + (uint8_t)i);
    }
    *next = (uint8_t)(*next + 17U);
    return (int)len;
}

static tp_session *make_session(void) {
    uint8_t seed = 1U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err;
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_create(&rng, &session, &err));
    TEST_ASSERT_NOT_NULL(session);
    return session;
}

static tp_operation rename_op(tp_id128 atlas_id, const char *name);

void test_snapshot_preview_apply_isolated_and_revision_exact(void) {
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *before =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(before);
    char original_name[128];
    (void)snprintf(original_name, sizeof original_name, "%s", before->name);

    tp_operation operation = rename_op(before->id, "preview-only-name");
    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "10000000000000000000000000000091", 33U);
    request.expected_revision = tp_session_snapshot_revision(snapshot);
    request.ops = &operation;
    request.op_count = 1;
    tp_txn_result result;
    memset(&result, 0, sizeof result);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_apply_preview(snapshot, &request, &result, &err));
    TEST_ASSERT_TRUE(result.committed);
    TEST_ASSERT_FALSE(result.no_change);
    TEST_ASSERT_EQUAL_INT64(request.expected_revision + 1, result.revision);
    TEST_ASSERT_EQUAL_STRING(
        original_name, tp_session_snapshot_atlas_at(snapshot, 0)->name);
    TEST_ASSERT_EQUAL_INT64(0, tp_session_revision(session));

    tp_txn_result_free(&result);
    tp_operation_free(&operation);
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_snapshot_allocation_failures_return_structured_oom(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = &project->atlases[0];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source_kind(atlas, "sprites",
                                         TP_SOURCE_KIND_FOLDER));
    uint8_t seed = 41U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &err));
    const tp_id128 source_id = atlas->sources[0].id;
    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(atlas, source_id,
                                                  "hero.png", &sprite));
    tp_project_anim *animation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_animation(atlas, "walk", &animation));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_anim_add_frame(animation, source_id,
                                                    "hero.png"));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_target(atlas, "json-neotolis", "out", NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &err));
    char path[1024];
    (void)snprintf(path, sizeof path,
                   "%s/tp_session_snapshot_oom.ntpacker_project", g_scratch);
    (void)remove(path);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(project, path, &err));
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_adopt_owned(project, &rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    tp_session__test_reset_snapshot_allocations();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &err));
    const size_t allocation_count =
        tp_session__test_snapshot_allocation_count();
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(8U, allocation_count);
    tp_session_snapshot_destroy(snapshot);

    for (size_t i = 0U; i < allocation_count; ++i) {
        snapshot = (tp_session_snapshot *)(uintptr_t)1U;
        tp_session__test_fail_snapshot_allocation_after(i);
        memset(&err, 0, sizeof err);
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OOM,
            tp_session_snapshot_create(session, &snapshot, &err));
        TEST_ASSERT_NULL(snapshot);
        TEST_ASSERT_NOT_EQUAL('\0', err.msg[0]);
    }
    tp_session_destroy(session);

    tp_session__test_reset_snapshot_allocations();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_load(path, &snapshot, &err));
    const size_t load_allocation_count =
        tp_session__test_snapshot_allocation_count();
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(8U, load_allocation_count);
    tp_session_snapshot_destroy(snapshot);
    for (size_t i = 0U; i < load_allocation_count; ++i) {
        snapshot = (tp_session_snapshot *)(uintptr_t)1U;
        tp_session__test_fail_snapshot_allocation_after(i);
        memset(&err, 0, sizeof err);
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OOM,
            tp_session_snapshot_load(path, &snapshot, &err));
        TEST_ASSERT_NULL(snapshot);
        TEST_ASSERT_NOT_EQUAL('\0', err.msg[0]);
    }
    (void)remove(path);
}

static tp_id128 recovery_key(void) {
    tp_id128 key;
    static const uint8_t bytes[16] = {'n', 't', 'p', 'k', '_', 'r', 'e', 'c',
                                      'o', 'v', 'e', 'r', 'y', '_', '0', '1'};
    memcpy(key.bytes, bytes, sizeof bytes);
    return key;
}

static bool test_file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    (void)fclose(file);
    return true;
}

static tp_operation rename_op(tp_id128 atlas_id, const char *name) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = atlas_id;
    const size_t len = strlen(name) + 1U;
    op.u.atlas_rename.name = (char *)malloc(len);
    TEST_ASSERT_NOT_NULL(op.u.atlas_rename.name);
    memcpy(op.u.atlas_rename.name, name, len);
    return op;
}

/* Apply one atlas.rename as a whole session transaction; asserts the apply
 * status equals expected_status. Writes the txn result to *out -- the caller
 * owns it and must tp_txn_result_free() it -- so result-field assertions
 * (committed, revision, no_change) some call sites still need stay possible.
 * A void helper that freed *out internally would silently drop those. */
static void session_apply_rename(tp_session *session, const char *id_hex,
                                 int64_t expected_rev, tp_id128 atlas_id,
                                 const char *name, tp_status expected_status,
                                 tp_txn_result *out, tp_error *err) {
    tp_operation op = rename_op(atlas_id, name);
    tp_txn_request req;
    memset(&req, 0, sizeof req);
    req.schema = TP_TXN_SCHEMA;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", id_hex);
    req.expected_revision = expected_rev;
    req.ops = &op;
    req.op_count = 1;
    memset(out, 0, sizeof *out);
    TEST_ASSERT_EQUAL_INT(expected_status,
                          tp_session_apply(session, &req, out, err));
    tp_operation_free(&op);
}

static char *test_dup(const char *text) {
    const size_t len = strlen(text) + 1U;
    char *copy = (char *)malloc(len);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, text, len);
    return copy;
}

static void save_project_with_ids(const char *path, uint8_t seed) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(project, &rng, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(project, path, &err));
    tp_project_destroy(project);
}

static void write_text_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    const size_t length = strlen(text);
    TEST_ASSERT_EQUAL_size_t(length, fwrite(text, 1U, length, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void make_test_dir(const char *path) {
#ifdef _WIN32
    (void)_mkdir(path);
#else
    (void)mkdir(path, 0777);
#endif
}

void test_read_snapshot_rejects_unsupported_schema(void) {
    char path[1024];
    (void)snprintf(path, sizeof path,
                   "%s/tp_session_old_schema.ntpacker_project", g_scratch);
    (void)remove(path);
    write_text_file(path,
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"atlases\": []\n"
                    "}\n");

    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_VERSION,
                          tp_session_snapshot_load(path, &snapshot, &err));
    TEST_ASSERT_NULL(snapshot);
    (void)remove(path);
}

void test_writable_open_save_preserves_canonical_orphan(void) {
    char source_dir[1024];
    char sprite_path[1024];
    char project_path[1024];
    (void)snprintf(source_dir, sizeof source_dir, "%s/session_canonical_orphan",
                   g_scratch);
    (void)snprintf(sprite_path, sizeof sprite_path, "%s/ghost.png", source_dir);
    (void)snprintf(project_path, sizeof project_path,
                   "%s/session_canonical_orphan.ntpacker_project", g_scratch);
    make_test_dir(source_dir);
    (void)remove(sprite_path);
    (void)remove(project_path);

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = &project->atlases[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, source_dir));
    uint8_t assign_seed = 37U;
    tp_rng assign_rng = {deterministic_fill, &assign_seed};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &assign_rng,
                                                        &err));
    const tp_id128 source_id = atlas->sources[0].id;
    tp_project_sprite *orphan_record = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(
            atlas, source_id, "ghost.png", &orphan_record));
    orphan_record->origin_x = 0.25F;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(project, project_path, &err));
    tp_project_destroy(project);

    uint8_t open_seed = 71U;
    tp_rng open_rng = {deterministic_fill, &open_seed};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_open(project_path, &open_rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *opened_atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(opened_atlas);
    const tp_snapshot_sprite *orphan =
        tp_session_snapshot_sprite_at(snapshot, opened_atlas->id, 0);
    TEST_ASSERT_NOT_NULL(orphan);
    TEST_ASSERT_TRUE(tp_id128_eq(source_id, orphan->source_id));
    TEST_ASSERT_EQUAL_STRING("ghost.png", orphan->source_key);
    TEST_ASSERT_FALSE(tp_session_snapshot_dirty(snapshot));
    tp_session_snapshot_destroy(snapshot);

    tp_session_save_result save_result = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save(session, &save_result, &err));
    TEST_ASSERT_TRUE(save_result.saved);
    tp_session_destroy(session);

    /* Physical return reactivates the same canonical identity without rewriting
     * the project graph. */
    write_text_file(sprite_path, "sprite");
    open_seed = 91U;
    session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_open(project_path, &open_rng, &session, &err));
    snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    opened_atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    orphan = tp_session_snapshot_sprite_at(snapshot, opened_atlas->id, 0);
    TEST_ASSERT_NOT_NULL(orphan);
    TEST_ASSERT_TRUE(tp_id128_eq(source_id, orphan->source_id));
    TEST_ASSERT_EQUAL_STRING("ghost.png", orphan->source_key);
    TEST_ASSERT_FALSE(tp_session_snapshot_dirty(snapshot));
    tp_session_snapshot_destroy(snapshot);
    memset(&save_result, 0, sizeof save_result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save(session, &save_result, &err));
    tp_session_destroy(session);

    project = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_load(project_path, &project, &err));
    TEST_ASSERT_NOT_NULL(tp_project_atlas_find_sprite_by_source_key(
        &project->atlases[0], source_id, "ghost.png"));
    tp_project_destroy(project);
    (void)remove(sprite_path);
    (void)remove(project_path);
}

void test_source_batch_plan_collapses_lexical_and_symlink_aliases(void) {
    char real_path[TP_IDENTITY_PATH_MAX];
    char alias_path[TP_IDENTITY_PATH_MAX];
    char dot_path[TP_IDENTITY_PATH_MAX];
    char parent_alias[TP_IDENTITY_PATH_MAX];
    char new_path[TP_IDENTITY_PATH_MAX];
#ifdef _WIN32
    char case_alias[TP_IDENTITY_PATH_MAX];
#endif
    (void)snprintf(real_path, sizeof real_path, "%s/source_real", g_scratch);
    (void)snprintf(alias_path, sizeof alias_path, "%s/source_alias", g_scratch);
    (void)snprintf(dot_path, sizeof dot_path, "%s/source_real/.", g_scratch);
    (void)snprintf(parent_alias, sizeof parent_alias,
                   "%s/not_created/../source_real", g_scratch);
    (void)snprintf(new_path, sizeof new_path, "%s/source_new", g_scratch);
    tp_mkdirs(real_path);
    (void)remove(alias_path);

    bool has_symlink = false;
#ifdef _WIN32
    has_symlink = CreateSymbolicLinkA(alias_path, real_path,
                                      SYMBOLIC_LINK_FLAG_DIRECTORY) != 0;
    (void)snprintf(case_alias, sizeof case_alias, "%s", real_path);
    for (char *c = case_alias + 3; *c; ++c) {
        if (*c >= 'a' && *c <= 'z') {
            *c = (char)(*c - ('a' - 'A'));
            break;
        }
        if (*c >= 'A' && *c <= 'Z') {
            *c = (char)(*c + ('a' - 'A'));
            break;
        }
    }
#else
    has_symlink = symlink(real_path, alias_path) == 0;
#endif

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source_kind(&project->atlases[0], real_path,
                                         TP_SOURCE_KIND_FOLDER));
    uint8_t seed = 19U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &err));
    const char *operation_alias = has_symlink ? alias_path :
#ifdef _WIN32
                                                    case_alias;
#else
                                                    NULL;
#endif
    if (operation_alias) {
        tp_operation add_alias = {0};
        add_alias.kind = TP_OP_SOURCE_ADD;
        add_alias.atlas_id = project->atlases[0].id;
        add_alias.u.source_add.source_id = (tp_id128){{0x71}};
        add_alias.u.source_add.kind = TP_SOURCE_KIND_FOLDER;
        add_alias.u.source_add.key = (char *)operation_alias;
        tp_op_reject reject;
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_INVALID_ARGUMENT,
            tp_operation_validate(project, &add_alias, &reject));
        TEST_ASSERT_EQUAL_STRING("key", reject.field);
    }
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_adopt_owned(project, &rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    const char *inputs[4] = {dot_path, parent_alias,
                             has_symlink ? alias_path : dot_path, new_path};
    tp_source_batch_plan plan = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_source_batch_plan_create(snapshot, atlas->id, inputs, 4, &plan,
                                    &err));
    TEST_ASSERT_EQUAL_INT(1, plan.count);
    TEST_ASSERT_EQUAL_INT(3, plan.duplicate_count);
    TEST_ASSERT_EQUAL_INT(3, plan.items[0].input_index);
    TEST_ASSERT_EQUAL_STRING(new_path, plan.items[0].path);
    tp_source_batch_plan_free(&plan);

    if (has_symlink) {
        const tp_snapshot_source *found = NULL;
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            tp_source_snapshot_find(snapshot, atlas->id, alias_path, &found,
                                    &err));
        TEST_ASSERT_NOT_NULL(found);
    }

    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
    (void)remove(alias_path);
}

void test_source_batch_plan_rejects_empty_inputs_atomically(void) {
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    const char *inputs[] = {g_scratch, ""};
    tp_source_batch_plan plan = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_source_batch_plan_create(snapshot, atlas->id, inputs, 2, &plan,
                                    &err));
    TEST_ASSERT_EQUAL_INT(0, plan.count);
    TEST_ASSERT_NULL(plan.items);
    TEST_ASSERT_NULL(plan.storage);
    TEST_ASSERT_NOT_EQUAL(0, err.msg[0]);

    const char *null_input[] = {NULL};
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_source_batch_plan_create(snapshot, atlas->id, null_input, 1,
                                    &plan, &err));
    TEST_ASSERT_EQUAL_INT(0, plan.count);
    TEST_ASSERT_NULL(plan.items);
    TEST_ASSERT_NULL(plan.storage);
    TEST_ASSERT_NOT_EQUAL(0, err.msg[0]);

    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_owned_snapshot_survives_later_commit(void) {
    tp_session *session = make_session();
    tp_error err;
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &before, &err));
    TEST_ASSERT_NOT_NULL(before);
    TEST_ASSERT_EQUAL_INT64(0, tp_session_snapshot_revision(before));
    TEST_ASSERT_EQUAL_UINT64(0, tp_session_snapshot_model_generation(before));
    TEST_ASSERT_EQUAL_INT(1, tp_session_snapshot_atlas_count(before));

    const tp_snapshot_atlas *old_atlas = tp_session_snapshot_atlas_at(before, 0);
    TEST_ASSERT_NOT_NULL(old_atlas);
    TEST_ASSERT_EQUAL_STRING("atlas1", old_atlas->name);

    tp_txn_result result;
    session_apply_rename(session, "00112233445566778899aabbccddeeff", 0,
                         old_atlas->id, "renamed", TP_STATUS_OK, &result, &err);
    TEST_ASSERT_TRUE(result.committed);
    TEST_ASSERT_EQUAL_INT64(1, result.revision);
    tp_txn_result_free(&result);

    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &after, &err));
    TEST_ASSERT_EQUAL_UINT64(1, tp_session_snapshot_model_generation(after));
    TEST_ASSERT_EQUAL_STRING("atlas1", old_atlas->name);
    TEST_ASSERT_EQUAL_STRING("renamed", tp_session_snapshot_atlas_by_id(after, old_atlas->id)->name);

    tp_session_snapshot_destroy(after);
    tp_session_snapshot_destroy(before);
    tp_session_destroy(session);
}

void test_snapshot_creation_does_not_clone_and_same_generation_shares_project(void) {
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *first = NULL;
    tp_session_snapshot *second = NULL;

    /* Snapshot admission must retain the committed generation, not deep-clone it. */
    tp_project__test_set_clone_alloc_fail(0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &first, &err));
    TEST_ASSERT_EQUAL_INT(0, tp_project__test_clone_alloc_count());

    tp_project__test_set_clone_alloc_fail(0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &second, &err));
    TEST_ASSERT_EQUAL_INT(0, tp_project__test_clone_alloc_count());
    TEST_ASSERT_EQUAL_PTR(tp_session_snapshot_project_internal(first),
                          tp_session_snapshot_project_internal(second));

    tp_session_snapshot_destroy(second);
    tp_session_snapshot_destroy(first);
    tp_session_destroy(session);
    tp_project__test_set_clone_alloc_fail(-1);
}

void test_snapshot_survives_session_destroy(void) {
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 identity = tp_session_snapshot_semantic_identity(snapshot);

    tp_session_destroy(session);

    TEST_ASSERT_EQUAL_INT(1, tp_session_snapshot_atlas_count(snapshot));
    TEST_ASSERT_TRUE(tp_id128_eq(identity,
                                 tp_session_snapshot_semantic_identity(snapshot)));
    char *serialized = NULL;
    size_t serialized_len = 0U;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_serialize(snapshot, &serialized,
                                                        &serialized_len, &err));
    TEST_ASSERT_GREATER_THAN_size_t(0U, serialized_len);
    free(serialized);
    tp_session_snapshot_destroy(snapshot);
}

void test_generation_owner_oom_is_structured_and_retryable(void) {
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = (tp_session_snapshot *)(uintptr_t)1U;
    tp_session__test_fail_next_generation_owner_allocation();

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM,
                          tp_session_snapshot_create(session, &snapshot, &err));
    TEST_ASSERT_NULL(snapshot);
    TEST_ASSERT_NOT_EQUAL('\0', err.msg[0]);
    TEST_ASSERT_EQUAL_INT64(0, tp_session_revision(session));

    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    TEST_ASSERT_NOT_NULL(snapshot);
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_save_as_does_not_mutate_held_snapshot_project_dir(void) {
    char path[1024];
    (void)snprintf(path, sizeof path,
                   "%s/tp_session_generation_save_as.ntpacker_project",
                   g_scratch);
    (void)remove(path);
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &before, &err));
    TEST_ASSERT_EQUAL_STRING("", tp_session_snapshot_project_dir(before));

    tp_session_save_result result = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, path, &result, &err));
    TEST_ASSERT_TRUE(result.saved);
    TEST_ASSERT_EQUAL_STRING("", tp_session_snapshot_project_dir(before));

    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &after, &err));
    TEST_ASSERT_NOT_EQUAL(0, tp_session_snapshot_project_dir(after)[0]);
    TEST_ASSERT_NOT_EQUAL(tp_session_snapshot_project_internal(before),
                          tp_session_snapshot_project_internal(after));

    tp_session_snapshot_destroy(after);
    tp_session_snapshot_destroy(before);
    tp_session_destroy(session);
    (void)remove(path);
}

void test_detached_recovery_save_publishes_new_model_generation(void) {
    char path[1024];
    (void)snprintf(path, sizeof path,
                   "%s/tp_session_detached_generation.ntpacker_project",
                   g_scratch);
    (void)remove(path);
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    uint8_t seed = 73U;
    tp_rng rng = {deterministic_fill, &seed};
    const tp_id128 recovery_token = recovery_key();
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_create_detached_recovery(project, &rng, recovery_token,
                                            &session, &err));
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &before, &err));
    TEST_ASSERT_EQUAL_UINT64(0U,
                             tp_session_snapshot_model_generation(before));
    TEST_ASSERT_EQUAL_STRING("", tp_session_snapshot_project_dir(before));

    tp_session_save_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_save_detached_recovery(session, path, NULL, &result, &err));
    TEST_ASSERT_TRUE(result.saved);

    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &after, &err));
    TEST_ASSERT_EQUAL_UINT64(1U,
                             tp_session_snapshot_model_generation(after));
    TEST_ASSERT_EQUAL_UINT64(1U,
                             tp_session_snapshot_admission_sequence(after));
    TEST_ASSERT_EQUAL_STRING("", tp_session_snapshot_project_dir(before));
    TEST_ASSERT_NOT_EQUAL(0, tp_session_snapshot_project_dir(after)[0]);
    TEST_ASSERT_NOT_EQUAL(tp_session_snapshot_project_internal(before),
                          tp_session_snapshot_project_internal(after));

    tp_session_snapshot_destroy(after);
    tp_session_snapshot_destroy(before);
    tp_session_destroy(session);
    (void)remove(path);
}

void test_rejected_commit_does_not_publish_event_or_generation(void) {
    tp_session *session = make_session();
    tp_error err;
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    tp_txn_result result;
    session_apply_rename(session, "ffeeddccbbaa99887766554433221100", 99,
                         atlas->id, "not-committed", TP_STATUS_INVALID_REVISION,
                         &result, &err);
    TEST_ASSERT_FALSE(result.committed);
    tp_txn_result_free(&result);

    TEST_ASSERT_EQUAL_UINT64(0, tp_session_event_sequence(session));
    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &after, &err));
    TEST_ASSERT_EQUAL_UINT64(0, tp_session_snapshot_model_generation(after));
    TEST_ASSERT_EQUAL_STRING("atlas1", tp_session_snapshot_atlas_at(after, 0)->name);

    tp_session_snapshot_destroy(after);
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_invalid_operation_reject_publishes_no_event_and_id_stays_retryable(void) {
    static const char transaction_id[] =
        "bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbd";
    tp_session *session = make_session();
    tp_error error = {{0}};
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &before, &error));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(before, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    char invalid_name[] = {'b', (char)0xC3, 'x', '\0'};
    tp_operation operation = {0};
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = atlas->id;
    operation.atlas_id.bytes[0] ^= 0xFFU;
    operation.u.atlas_rename.name = invalid_name;
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, transaction_id, sizeof transaction_id);
    request.ops = &operation;
    request.op_count = 1;

    tp_txn_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_UTF8,
        tp_session_apply(session, &request, &result, &error));
    TEST_ASSERT_FALSE(result.committed);
    TEST_ASSERT_EQUAL_INT(1, result.error_count);
    TEST_ASSERT_EQUAL_INT(0, result.errors[0].op_index);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_UTF8, result.errors[0].code);
    TEST_ASSERT_EQUAL_STRING("name", result.errors[0].field);
    TEST_ASSERT_EQUAL_STRING("name contains invalid UTF-8 at offset 1",
                             result.errors[0].message);
    tp_txn_result_free(&result);

    TEST_ASSERT_EQUAL_UINT64(0, tp_session_event_sequence(session));
    TEST_ASSERT_FALSE(tp_session_can_undo(session));
    TEST_ASSERT_FALSE(tp_session_can_redo(session));
    tp_session_snapshot *rejected = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &rejected, &error));
    TEST_ASSERT_EQUAL_INT64(0, tp_session_snapshot_revision(rejected));
    TEST_ASSERT_EQUAL_UINT64(0,
                             tp_session_snapshot_model_generation(rejected));
    TEST_ASSERT_EQUAL_UINT64(1,
                             tp_session_snapshot_admission_sequence(rejected));
    TEST_ASSERT_FALSE(tp_session_snapshot_dirty(rejected));
    TEST_ASSERT_EQUAL_STRING(
        tp_session_snapshot_atlas_at(before, 0)->name,
        tp_session_snapshot_atlas_at(rejected, 0)->name);

    operation.atlas_id = atlas->id;
    operation.u.atlas_rename.name = (char *)"renamed";
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_apply(session, &request, &result, &error));
    TEST_ASSERT_TRUE(result.committed);
    TEST_ASSERT_EQUAL_INT64(1, result.revision);
    tp_txn_result_free(&result);
    TEST_ASSERT_EQUAL_UINT64(1, tp_session_event_sequence(session));
    TEST_ASSERT_TRUE(tp_session_can_undo(session));
    TEST_ASSERT_FALSE(tp_session_can_redo(session));

    tp_session_event event = {0};
    size_t event_count = 0U;
    bool resync = true;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_events_after(session, 0U, &event, 1U, &event_count,
                                &resync, &error));
    TEST_ASSERT_FALSE(resync);
    TEST_ASSERT_EQUAL_size_t(1U, event_count);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_EVENT_MODEL_COMMITTED, event.kind);
    TEST_ASSERT_EQUAL_STRING(transaction_id, event.transaction_id);
    TEST_ASSERT_EQUAL_INT64(0, event.revision_before);
    TEST_ASSERT_EQUAL_INT64(1, event.revision_after);

    tp_session_snapshot_destroy(rejected);
    tp_session_snapshot_destroy(before);
    tp_session_destroy(session);
}

/* A batch of 2+ ops is ONE transaction: it publishes exactly one MODEL_COMMITTED
 * event and advances the revision once (0 -> 1), not once per op. */
void test_multi_op_batch_publishes_single_commit_event(void) {
    tp_session *session = make_session();
    tp_error error = {{0}};
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &before, &error));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(before, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    tp_operation ops[2];
    memset(ops, 0, sizeof ops);
    ops[0].kind = TP_OP_ATLAS_RENAME;
    ops[0].atlas_id = atlas->id;
    ops[0].u.atlas_rename.name = (char *)"renamed";
    ops[1].kind = TP_OP_ATLAS_SETTINGS_SET;
    ops[1].atlas_id = atlas->id;
    ops[1].u.atlas_settings.mask = TP_AF_MAX_SIZE;
    ops[1].u.atlas_settings.max_size = 2048;

    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "20000000000000000000000000000001", 33U);
    request.ops = ops;
    request.op_count = 2;

    tp_txn_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_apply(session, &request, &result, &error));
    TEST_ASSERT_TRUE(result.committed);
    TEST_ASSERT_EQUAL_INT64(1, result.revision);
    tp_txn_result_free(&result);

    /* Exactly one event for the whole 2-op batch. */
    TEST_ASSERT_EQUAL_UINT64(1, tp_session_event_sequence(session));

    tp_session_event event = {0};
    size_t event_count = 0U;
    bool resync = true;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_events_after(session, 0U, &event, 1U, &event_count,
                                &resync, &error));
    TEST_ASSERT_FALSE(resync);
    TEST_ASSERT_EQUAL_size_t(1U, event_count);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_EVENT_MODEL_COMMITTED, event.kind);
    TEST_ASSERT_EQUAL_INT64(0, event.revision_before);
    TEST_ASSERT_EQUAL_INT64(1, event.revision_after);

    tp_session_snapshot_destroy(before);
    tp_session_destroy(session);
}

void test_no_change_apply_does_not_publish_event_or_generation(void) {
    tp_session *session = make_session();
    tp_error err = {0};
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &before, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(before, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    tp_txn_result result;
    session_apply_rename(session, "abcdefabcdefabcdefabcdefabcdefab",
                         tp_session_snapshot_revision(before), atlas->id,
                         atlas->name, TP_STATUS_OK, &result, &err);
    TEST_ASSERT_FALSE(result.committed);
    TEST_ASSERT_TRUE(result.no_change);
    tp_txn_result_free(&result);

    TEST_ASSERT_EQUAL_UINT64(0, tp_session_event_sequence(session));
    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &after, &err));
    TEST_ASSERT_EQUAL_INT64(tp_session_snapshot_revision(before),
                            tp_session_snapshot_revision(after));
    TEST_ASSERT_EQUAL_UINT64(tp_session_snapshot_model_generation(before),
                             tp_session_snapshot_model_generation(after));
    TEST_ASSERT_FALSE(tp_session_snapshot_dirty(after));
    tp_session_snapshot_destroy(after);
    tp_session_snapshot_destroy(before);
    tp_session_destroy(session);
}

void test_undo_redo_are_session_commands_with_ordered_events(void) {
    tp_session *session = make_session();
    tp_error err;
    tp_session_snapshot *initial = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &initial, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(initial, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    tp_txn_result result;
    session_apply_rename(session, "1234567890abcdef1234567890abcdef", 0,
                         atlas->id, "renamed", TP_STATUS_OK, &result, &err);
    tp_txn_result_free(&result);
    TEST_ASSERT_TRUE(tp_session_can_undo(session));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_undo(session, &err));
    tp_session_snapshot *undone = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &undone, &err));
    TEST_ASSERT_EQUAL_STRING("atlas1", tp_session_snapshot_atlas_by_id(undone, atlas->id)->name);
    TEST_ASSERT_EQUAL_INT64(2, tp_session_snapshot_revision(undone));
    TEST_ASSERT_EQUAL_UINT64(2, tp_session_snapshot_model_generation(undone));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_redo(session, &err));
    tp_session_snapshot *redone = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &redone, &err));
    TEST_ASSERT_EQUAL_STRING("renamed", tp_session_snapshot_atlas_by_id(redone, atlas->id)->name);
    TEST_ASSERT_EQUAL_INT64(3, tp_session_snapshot_revision(redone));

    tp_session_event events[3];
    size_t count = 0U;
    bool resync = true;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_events_after(session, 0U, events, 3U, &count, &resync, &err));
    TEST_ASSERT_FALSE(resync);
    TEST_ASSERT_EQUAL_UINT64(3, count);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_EVENT_MODEL_COMMITTED, events[0].kind);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_EVENT_UNDONE, events[1].kind);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_EVENT_REDONE, events[2].kind);
    TEST_ASSERT_EQUAL_INT64(0, events[0].revision_before);
    TEST_ASSERT_EQUAL_INT64(1, events[0].revision_after);
    TEST_ASSERT_EQUAL_INT64(1, events[1].revision_before);
    TEST_ASSERT_EQUAL_INT64(2, events[1].revision_after);
    TEST_ASSERT_EQUAL_INT64(2, events[2].revision_before);
    TEST_ASSERT_EQUAL_INT64(3, events[2].revision_after);

    /* Every retained generation remains immutable after later history swaps. */
    TEST_ASSERT_EQUAL_STRING("atlas1",
                             tp_session_snapshot_atlas_by_id(initial,
                                                             atlas->id)->name);
    TEST_ASSERT_EQUAL_STRING("atlas1",
                             tp_session_snapshot_atlas_by_id(undone,
                                                             atlas->id)->name);
    TEST_ASSERT_EQUAL_STRING("renamed",
                             tp_session_snapshot_atlas_by_id(redone,
                                                             atlas->id)->name);
    TEST_ASSERT_NOT_EQUAL(tp_session_snapshot_project_internal(initial),
                          tp_session_snapshot_project_internal(undone));
    TEST_ASSERT_NOT_EQUAL(tp_session_snapshot_project_internal(undone),
                          tp_session_snapshot_project_internal(redone));

    tp_session_snapshot_destroy(redone);
    tp_session_snapshot_destroy(undone);
    tp_session_snapshot_destroy(initial);
    tp_session_destroy(session);
}

void test_journal_append_failure_commits_and_sticky_degradation_skips_later_recovery_work(void) {
    tp_session *session = make_session();
    tp_error err;
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &before, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(before, 0);

    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_id128 key;
    memset(&key, 0xA5, sizeof key);
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_attach_journal(session, journal, &err));

    tp_journal_io_memory__fail_next_writes(io, 1);
    tp_txn_result result;
    session_apply_rename(session, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0,
                         atlas->id, "committed-despite-journal", TP_STATUS_OK,
                         &result, &err);
    TEST_ASSERT_TRUE(result.committed);
    TEST_ASSERT_EQUAL_INT64(1, result.revision);
    tp_txn_result_free(&result);

    TEST_ASSERT_EQUAL_UINT64(1, tp_session_event_sequence(session));
    TEST_ASSERT_FALSE(tp_session_recovery_available(session));
    TEST_ASSERT_TRUE(tp_session_can_undo(session));

    const size_t syncs_after_failure = tp_journal_io_memory__sync_count(io);
    tp_txn__test_encode_stats_reset();
    session_apply_rename(session, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 1,
                         atlas->id, "later-edit", TP_STATUS_OK, &result, &err);
    TEST_ASSERT_TRUE(result.committed);
    TEST_ASSERT_EQUAL_INT64(2, result.revision);
    tp_txn_result_free(&result);
    TEST_ASSERT_EQUAL_size_t(0U, tp_txn__test_request_encode_calls());
    TEST_ASSERT_EQUAL_size_t(syncs_after_failure,
                             tp_journal_io_memory__sync_count(io));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_undo(session, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_redo(session, &err));
    TEST_ASSERT_EQUAL_size_t(syncs_after_failure,
                             tp_journal_io_memory__sync_count(io));

    session_apply_rename(session, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 4,
                         atlas->id, "duplicate-must-not-reapply",
                         TP_STATUS_DUPLICATE_ID, &result, &err);
    TEST_ASSERT_FALSE(result.committed);
    tp_txn_result_free(&result);

    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &after, &err));
    TEST_ASSERT_EQUAL_INT64(4, tp_session_snapshot_revision(after));
    TEST_ASSERT_EQUAL_UINT64(4, tp_session_snapshot_model_generation(after));
    TEST_ASSERT_EQUAL_STRING("later-edit", tp_session_snapshot_atlas_at(after, 0)->name);

    tp_session_snapshot_destroy(after);
    tp_session_snapshot_destroy(before);
    tp_session_destroy(session);
}

static void assert_recovery_fault_commits(tp_session *session, tp_id128 atlas_id,
                                          const char *transaction_id,
                                          const char *name) {
    tp_error err = {{0}};
    tp_txn_result result;
    session_apply_rename(session, transaction_id, 0, atlas_id, name,
                         TP_STATUS_OK, &result, &err);
    TEST_ASSERT_TRUE(result.committed);
    TEST_ASSERT_EQUAL_INT64(1, result.revision);
    TEST_ASSERT_FALSE(tp_session_recovery_available(session));
    TEST_ASSERT_TRUE(tp_session_can_undo(session));
    tp_txn_result_free(&result);
}

void test_journal_admission_failure_commits_and_degrades_recovery(void) {
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);
    tp_journal_io io = tp_journal_io_memory();
    tp_id128 key = {{0}};
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_attach_journal(session, journal, &err));
    tp_journal__test_set_record_limit(1U);
    assert_recovery_fault_commits(session, atlas_id,
                                  "cccccccccccccccccccccccccccccccc",
                                  "admission-fault");
    tp_session_destroy(session);
}

void test_journal_encode_failure_commits_and_degrades_recovery(void) {
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);
    tp_journal_io io = tp_journal_io_memory();
    tp_id128 key = {{0}};
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_attach_journal(session, journal, &err));
    tp_txn__test_fail_next_request_encode();
    assert_recovery_fault_commits(session, atlas_id,
                                  "dddddddddddddddddddddddddddddddd",
                                  "encode-fault");
    tp_session_destroy(session);
}

void test_journal_sync_failure_commits_and_degrades_recovery(void) {
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);
    tp_journal_io io = tp_journal_io_memory();
    tp_id128 key = {{0}};
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_attach_journal(session, journal, &err));
    tp_journal_io_memory__fail_next_sync(io);
    assert_recovery_fault_commits(session, atlas_id,
                                  "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
                                  "sync-fault");
    tp_session_destroy(session);
}

void test_source_invalidation_is_runtime_only_and_event_window_resyncs(void) {
    tp_session *session = make_session();
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_invalidate_sources(session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &snapshot, &err));
    TEST_ASSERT_EQUAL_INT64(0, tp_session_snapshot_revision(snapshot));
    TEST_ASSERT_EQUAL_UINT64(0, tp_session_snapshot_model_generation(snapshot));
    TEST_ASSERT_EQUAL_UINT64(1, tp_session_snapshot_source_generation(snapshot));
    TEST_ASSERT_EQUAL_UINT64(1, tp_session_snapshot_admission_sequence(snapshot));
    tp_session_snapshot_destroy(snapshot);

    for (int i = 0; i < 70; ++i) {
        char id[33];
        char name[32];
        (void)snprintf(id, sizeof id, "%032x", i + 1);
        (void)snprintf(name, sizeof name, "atlas-%d", i);
        tp_session_snapshot *current = NULL;
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &current, &err));
        const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(current, 0);
        tp_txn_result result;
        session_apply_rename(session, id, tp_session_snapshot_revision(current),
                             atlas->id, name, TP_STATUS_OK, &result, &err);
        tp_txn_result_free(&result);
        tp_session_snapshot_destroy(current);
    }

    tp_session_event event;
    size_t count = 99U;
    bool resync = false;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_events_after(session, 0U, &event, 1U, &count, &resync, &err));
    TEST_ASSERT_TRUE(resync);
    TEST_ASSERT_EQUAL_UINT64(0, count);
    tp_session_destroy(session);
}

void test_save_as_and_open_are_session_owned_commands(void) {
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/tp_session_saved.ntpacker_project", g_scratch);
    (void)remove(path);
    tp_session *session = make_session();
    tp_error err;
    tp_session_snapshot *initial = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &initial, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(initial, 0);
    tp_txn_result result;
    session_apply_rename(session, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 0,
                         atlas->id, "persisted", TP_STATUS_OK, &result, &err);
    tp_txn_result_free(&result);
    tp_session_snapshot *dirty = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &dirty, &err));
    TEST_ASSERT_TRUE(tp_session_snapshot_dirty(dirty));
    tp_session_snapshot_destroy(dirty);

    tp_session_save_result save_result;
    memset(&save_result, 0, sizeof save_result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_save_as(session, path, &save_result, &err));
    TEST_ASSERT_TRUE(save_result.saved);
    TEST_ASSERT_FALSE(save_result.recovery_degraded);
    tp_session_snapshot *saved = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &saved, &err));
    TEST_ASSERT_FALSE(tp_session_snapshot_dirty(saved));
    TEST_ASSERT_EQUAL_INT64(1, tp_session_snapshot_revision(saved));
    TEST_ASSERT_EQUAL_UINT64(2, tp_session_snapshot_model_generation(saved));
    TEST_ASSERT_EQUAL_INT(TP_IDENTITY_SAVED, tp_session_snapshot_identity(saved).kind);

    TEST_ASSERT_TRUE(tp_session_recovery_available(session));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_discard(session, &err));
    TEST_ASSERT_TRUE(tp_session_recovery_available(session));
    TEST_ASSERT_FALSE(tp_session_can_undo(session));
    TEST_ASSERT_FALSE(tp_session_can_redo(session));
    tp_session_snapshot_destroy(saved);
    tp_session_snapshot_destroy(initial);
    tp_session_destroy(session);

    uint8_t seed = 91U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_session *opened = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_open(path, &rng, &opened, &err));
    tp_session_snapshot *reloaded = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(opened, &reloaded, &err));
    TEST_ASSERT_EQUAL_STRING("persisted", tp_session_snapshot_atlas_at(reloaded, 0)->name);
    TEST_ASSERT_FALSE(tp_session_snapshot_dirty(reloaded));
    TEST_ASSERT_EQUAL_INT64(0, tp_session_snapshot_revision(reloaded));
    tp_session_snapshot_destroy(reloaded);
    tp_session_destroy(opened);
    (void)remove(path);
}

void test_snapshot_has_id_addressed_nested_dtos(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = &project->atlases[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source_kind(atlas, "sprites", TP_SOURCE_KIND_FOLDER));
    uint8_t seed = 33U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &err));
    const tp_id128 source_id = atlas->sources[0].id;
    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(atlas, source_id,
                                                  "hero.png", &sprite));
    sprite->rename = test_dup("hero-final");
    tp_project_anim *animation = NULL;
    for (int i = 0; i < 32; ++i) {
        char name[32];
        (void)snprintf(name, sizeof name, "sibling-%d", i);
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            tp_project_atlas_add_animation(atlas, name, &animation));
    }
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(atlas, "walk", &animation));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_anim_add_frame(animation, source_id,
                                                    "hero.png"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_target(atlas, "json-neotolis", "out/hero", NULL));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(project, &rng, &err));
    atlas = &project->atlases[0];
    sprite = &atlas->sprites[0];
    animation = &atlas->animations[atlas->animation_count - 1];
    const tp_id128 animation_id = animation->id;
    const tp_id128 target_id = atlas->targets[0].id;

    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_adopt_owned(project, &rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas_dto = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas_dto);
    const tp_snapshot_source *source = tp_session_snapshot_source_by_id(
        snapshot, atlas_dto->id, source_id);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_STRING("sprites", source->path);
    const tp_snapshot_source *matched_source = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_source_snapshot_find(snapshot, atlas_dto->id,
                                                  "sprites", &matched_source,
                                                  &err));
    TEST_ASSERT_EQUAL_PTR(source, matched_source);
    const tp_snapshot_sprite *sprite_dto = tp_session_snapshot_sprite_by_key(
        snapshot, atlas_dto->id, source->id, "hero.png");
    TEST_ASSERT_NOT_NULL(sprite_dto);
    TEST_ASSERT_EQUAL_STRING("hero-final", sprite_dto->rename);
    TEST_ASSERT_TRUE(tp_id128_eq(sprite_dto->id, tp_sprite_id(source->id, "hero.png")));
    TEST_ASSERT_EQUAL_PTR(sprite_dto, tp_session_snapshot_sprite_by_id(snapshot, atlas_dto->id,
                                                                       sprite_dto->id));
    const tp_snapshot_animation *animation_dto = tp_session_snapshot_animation_by_id(
        snapshot, atlas_dto->id, animation_id);
    TEST_ASSERT_NOT_NULL(animation_dto);
    TEST_ASSERT_EQUAL_STRING("walk", animation_dto->name);
    const tp_snapshot_frame *frame = tp_session_snapshot_animation_frame_at(
        snapshot, atlas_dto->id, animation_dto->id, 0);
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_EQUAL_STRING("hero.png", frame->source_key);
    TEST_ASSERT_TRUE(tp_id128_eq(frame->sprite_id, sprite_dto->id));
    int frame_count = -1;
    const tp_snapshot_frame *frames = tp_session_snapshot_animation_frames(
        snapshot, atlas_dto->id, animation_dto->id, &frame_count);
    TEST_ASSERT_EQUAL_INT(1, frame_count);
    TEST_ASSERT_EQUAL_PTR(frame, frames);
    const tp_snapshot_target *target = tp_session_snapshot_target_by_id(
        snapshot, atlas_dto->id, target_id);
    TEST_ASSERT_NOT_NULL(target);
    TEST_ASSERT_EQUAL_STRING("out/hero", target->out_path);

    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_snapshot_resolves_source_by_direct_indices(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
#ifdef _WIN32
    const char *source_path = "C:/ntpacker-bench/source.png";
#else
    const char *source_path = "/tmp/ntpacker-bench/source.png";
#endif
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source_kind(&project->atlases[0], source_path,
                                         TP_SOURCE_KIND_FILE));
    uint8_t seed = 34U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &err));
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_adopt_owned(project, &rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));

    const tp_snapshot_source *source = NULL;
    char resolved[TP_IDENTITY_PATH_MAX] = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_source_resolved_at(snapshot, 0, 0, &source,
                                               resolved, sizeof resolved, &err));
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_STRING(source_path, source->path);
    TEST_ASSERT_EQUAL_STRING(source_path, resolved);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_session_snapshot_source_resolved_at(snapshot, 0, 1, &source,
                                               resolved, sizeof resolved, &err));

    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_snapshot_source_resolution_failures_fill_error_context(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
#ifdef _WIN32
    const char *absolute_path = "C:/ntpacker/source.png";
#else
    const char *absolute_path = "/tmp/ntpacker/source.png";
#endif
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source_kind(&project->atlases[0], absolute_path,
                                         TP_SOURCE_KIND_FILE));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source_kind(&project->atlases[0], "relative.png",
                                         TP_SOURCE_KIND_FILE));
    uint8_t seed = 93U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &err));
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_adopt_owned(project, &rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    const tp_snapshot_source *absolute =
        tp_session_snapshot_source_at(snapshot, atlas->id, 0);
    const tp_snapshot_source *relative =
        tp_session_snapshot_source_at(snapshot, atlas->id, 1);
    TEST_ASSERT_NOT_NULL(absolute);
    TEST_ASSERT_NOT_NULL(relative);

    const tp_snapshot_source *resolved_source = NULL;
    char resolved[1] = {0};
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_session_snapshot_source_resolved_at(snapshot, 0, 0,
                                               &resolved_source, resolved,
                                               sizeof resolved, &err));
    TEST_ASSERT_NOT_EQUAL('\0', err.msg[0]);

    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_session_snapshot_resolve_path(snapshot, atlas->id, absolute->id,
                                         resolved, sizeof resolved, &err));
    TEST_ASSERT_NOT_EQUAL('\0', err.msg[0]);

    char relative_out[TP_IDENTITY_PATH_MAX] = {0};
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_PATH_NOT_ABSOLUTE,
        tp_session_snapshot_resolve_path(snapshot, atlas->id, relative->id,
                                         relative_out, sizeof relative_out,
                                         &err));
    TEST_ASSERT_NOT_EQUAL('\0', err.msg[0]);

    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_snapshot_selector_uses_canonical_ambiguity_and_atlas_scope(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    int second = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_add_atlas(project, "second", &second));
    tp_project_anim *animation = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_animation(&project->atlases[0],
                                                         "walk", &animation));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_animation(&project->atlases[second],
                                                         "walk", &animation));
    uint8_t seed = 34U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_adopt_owned(project, &rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    tp_selector_result result;
    tp_selector_candidates candidates = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_AMBIGUOUS_SELECTOR,
                          tp_session_snapshot_resolve_selector(
                              snapshot, tp_id128_nil(), TP_SEL_ANIM, "walk",
                              &result, &candidates, &err));
    TEST_ASSERT_EQUAL_INT(2, candidates.count);
    tp_selector_candidates_free(&candidates);

    const tp_snapshot_atlas *first = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_resolve_selector(
                              snapshot, first->id, TP_SEL_ANIM, "walk",
                              &result, &candidates, &err));
    TEST_ASSERT_EQUAL_INT(TP_SEL_ANIM, result.kind);
    TEST_ASSERT_EQUAL_INT(0, result.atlas_index);
    tp_selector_candidates_free(&candidates);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_resolve_selector(
                              snapshot, first->id, TP_SEL_ANIM, "anim:walk",
                              &result, &candidates, &err));
    TEST_ASSERT_EQUAL_INT(TP_SEL_ANIM, result.kind);
    tp_selector_candidates_free(&candidates);
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_snapshot_sprite_selector_matches_headless_canonical_operation(void) {
    char root_a[1024];
    char root_b[1024];
    char file_a[1200];
    char file_b[1200];
    char save_path[1200];
    (void)snprintf(root_a, sizeof root_a, "%s/sprite_selector_a", g_scratch);
    (void)snprintf(root_b, sizeof root_b, "%s/sprite_selector_b", g_scratch);
    (void)snprintf(file_a, sizeof file_a, "%s/shared.png", root_a);
    (void)snprintf(file_b, sizeof file_b, "%s/shared.png", root_b);
    (void)snprintf(save_path, sizeof save_path,
                   "%s/sprite_selector.ntpacker_project", g_scratch);
    make_test_dir(root_a);
    make_test_dir(root_b);
    write_text_file(file_a, "a");
    write_text_file(file_b, "b");
    (void)remove(save_path);

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(&project->atlases[0],
                                                      root_a));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(&project->atlases[0],
                                                      root_b));
    uint8_t seed = 77U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_adopt_owned(project, &rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    const tp_snapshot_source *source_a =
        tp_session_snapshot_source_at(snapshot, atlas->id, 0);
    TEST_ASSERT_NOT_NULL(source_a);
    const tp_id128 source_a_id = source_a->id;

    tp_selector_result resolved;
    tp_selector_candidates candidates = {0};
    tp_id128 resolved_source = tp_id128_nil();
    char resolved_key[TP_SRCKEY_MAX];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_AMBIGUOUS_SELECTOR,
        tp_session_snapshot_resolve_sprite_selector(
            snapshot, atlas->id, "shared", &resolved, &resolved_source,
            resolved_key, sizeof resolved_key, &candidates, &err));
    TEST_ASSERT_EQUAL_INT(2, candidates.count);
    TEST_ASSERT_EQUAL_INT64(0, tp_session_revision(session));
    tp_selector_candidates_free(&candidates);

    char selector[TP_ID_TEXT_CAP + 32U];
    char source_text[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_id_format(TP_ID_KIND_SOURCE, source_a_id,
                                       source_text, sizeof source_text, &err));
    (void)snprintf(selector, sizeof selector, "%s:shared.png", source_text);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_resolve_sprite_selector(
            snapshot, atlas->id, selector, &resolved, &resolved_source,
            resolved_key, sizeof resolved_key, &candidates, &err));
    TEST_ASSERT_EQUAL_INT(TP_SEL_SPRITE, resolved.kind);
    TEST_ASSERT_TRUE(tp_id128_eq(source_a_id, resolved_source));
    TEST_ASSERT_EQUAL_STRING("shared.png", resolved_key);
    TEST_ASSERT_TRUE(tp_id128_eq(resolved.id,
                                 tp_sprite_id(source_a_id, resolved_key)));

    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SPRITE_OVERRIDE_SET;
    operation.atlas_id = atlas->id;
    operation.u.sprite_set.source_id = resolved_source;
    operation.u.sprite_set.src_key = resolved_key;
    operation.u.sprite_set.mask = TP_SPF_ORIGIN;
    operation.u.sprite_set.origin_x = 0.25F;
    operation.u.sprite_set.origin_y = 0.75F;
    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "10000000000000000000000000000091", 33U);
    request.expected_revision = 0;
    request.ops = &operation;
    request.op_count = 1;
    tp_txn_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_apply(session, &request, &result, &err));
    tp_txn_result_free(&result);
    tp_session_snapshot_destroy(snapshot);

    tp_session_save_result save_result;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, save_path, &save_result,
                                             &err));
    tp_session_destroy(session);

    tp_rng reopen_rng = tp_rng_os();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_open(save_path, &reopen_rng, &session, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(tp_session_snapshot_sprite_by_key(
        snapshot, atlas->id, source_a_id, "shared.png"));
    (void)remove(file_a);
    char sprite_text[TP_ID_TEXT_CAP];
    tp_sprite_id_format(tp_sprite_id(source_a_id, "shared.png"), sprite_text,
                        sizeof sprite_text);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_resolve_sprite_selector(
            snapshot, atlas->id, sprite_text, &resolved, &resolved_source,
            resolved_key, sizeof resolved_key, &candidates, &err));
    TEST_ASSERT_TRUE(tp_id128_eq(source_a_id, resolved_source));
    TEST_ASSERT_EQUAL_STRING("shared.png", resolved_key);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_resolve_sprite_selector(
            snapshot, atlas->id, selector, &resolved, &resolved_source,
            resolved_key, sizeof resolved_key, &candidates, &err));

    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SPRITE_OVERRIDE_CLEAR;
    operation.atlas_id = atlas->id;
    operation.u.sprite_clear.source_id = resolved_source;
    operation.u.sprite_clear.src_key = resolved_key;
    operation.u.sprite_clear.mask = TP_SPF_ALL;
    request.expected_revision = tp_session_snapshot_revision(snapshot);
    request.ops = &operation;
    memcpy(request.id_hex, "10000000000000000000000000000092", 33U);
    tp_session_snapshot_destroy(snapshot);
    snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_apply(session, &request, &result, &err));
    tp_txn_result_free(&result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NULL(tp_session_snapshot_sprite_by_key(
        snapshot, atlas->id, source_a_id, "shared.png"));
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_snapshot_frontend_queries_delegate_core_rules(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(&project->atlases[0],
                                                      "sprites\\hero"));
    tp_project_target *first = NULL;
    tp_project_target *second = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_target(&project->atlases[0],
                                                      "json-neotolis",
                                                      "out/atlas2", &first));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_target(&project->atlases[0],
                                                      "json-neotolis",
                                                      "out\\atlas2", &second));
    uint8_t seed = 91U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_adopt_owned(project, &rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_snapshot_source *matched = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_source_snapshot_find(snapshot, atlas->id,
                                                  "sprites/hero", &matched,
                                                  &err));
    const tp_snapshot_target *target = tp_session_snapshot_target_at(
        snapshot, atlas->id, 0);
    TEST_ASSERT_NOT_NULL(target);
    TEST_ASSERT_TRUE(tp_session_snapshot_target_out_path_shared(
        snapshot, atlas->id, target->id, target->out_path));
    char name[64];
    char out_path[128];
    const char *exporter_id = NULL;
    bool target_enabled = false;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_next_atlas_defaults(
                              snapshot, name, sizeof name, out_path,
                              sizeof out_path, &exporter_id,
                              &target_enabled, &err));
    TEST_ASSERT_EQUAL_STRING("atlas3", name);
    TEST_ASSERT_EQUAL_STRING("out/atlas3", out_path);
    TEST_ASSERT_EQUAL_STRING("json-neotolis", exporter_id);
    TEST_ASSERT_TRUE(target_enabled);
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_source_plan_propagates_stored_base_overflow(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source(&project->atlases[0], "sprites"));

    char long_base[TP_IDENTITY_PATH_MAX];
    memset(long_base, 'a', sizeof long_base);
#ifdef _WIN32
    long_base[0] = 'C';
    long_base[1] = ':';
    long_base[2] = '/';
#else
    long_base[0] = '/';
#endif
    long_base[sizeof long_base - 1U] = '\0';
    project->source_base_dir = test_dup(long_base);

    tp_source_path_identity identity = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_source_path_identity_from_stored(project, "sprites", false,
                                            &identity, &error));

    uint8_t seed = 92U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_adopt_owned(project, &rng, &session, &error));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &error));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    const tp_snapshot_source *matched =
        tp_session_snapshot_source_at(snapshot, atlas->id, 0);
    TEST_ASSERT_NOT_NULL(matched);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_source_snapshot_find(snapshot, atlas->id, "sprites", &matched,
                                &error));
    TEST_ASSERT_NULL(matched);

    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_future_event_cursor_requires_resync(void) {
    tp_session *session = make_session();
    tp_error err;
    tp_session_event event;
    size_t count = 99U;
    bool resync = false;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_events_after(session, 100U, &event, 1U, &count, &resync, &err));
    TEST_ASSERT_TRUE(resync);
    TEST_ASSERT_EQUAL_UINT64(0, count);
    tp_session_destroy(session);
}

void test_save_reports_recovery_degradation_and_keeps_later_mutation_available(void) {
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/tp_session_degraded.ntpacker_project", g_scratch);
    (void)remove(path);
    tp_session *session = make_session();
    tp_error err;
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &before, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(before, 0);

    tp_journal_io io = tp_journal_io_memory();
    tp_id128 key;
    memset(&key, 0x5A, sizeof key);
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_attach_journal(session, journal, &err));
    tp_journal_io_memory__fail_next_writes(io, 1);

    tp_session_save_result save_result;
    memset(&save_result, 0, sizeof save_result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_save_as(session, path, &save_result, &err));
    TEST_ASSERT_TRUE(save_result.saved);
    TEST_ASSERT_TRUE(save_result.recovery_degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, save_result.recovery_status);
    TEST_ASSERT_FALSE(tp_session_recovery_available(session));

    tp_txn_result txn_result;
    session_apply_rename(session, "cccccccccccccccccccccccccccccccc", 0,
                         atlas->id, "unsafe-after-degrade",
                         TP_STATUS_OK, &txn_result, &err);
    TEST_ASSERT_TRUE(txn_result.committed);
    TEST_ASSERT_EQUAL_INT64(1, txn_result.revision);
    tp_txn_result_free(&txn_result);
    tp_session_snapshot_destroy(before);
    tp_session_destroy(session);
    (void)remove(path);
}

void test_save_heals_degraded_recovery_and_resumes_journaling(void) {
    char path[1024];
    (void)snprintf(path, sizeof path,
                   "%s/tp_session_heals_recovery.ntpacker_project",
                   g_scratch);
    (void)remove(path);
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);

    tp_journal_io io = tp_journal_io_memory();
    tp_id128 key = {{0x61}};
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_attach_journal(session, journal, &err));
    tp_journal_io_memory__fail_next_sync(io);
    tp_txn_result txn_result;
    session_apply_rename(session, "61000000000000000000000000000001",
                         0, atlas_id, "degraded-before-save", TP_STATUS_OK,
                         &txn_result, &err);
    TEST_ASSERT_TRUE(txn_result.committed);
    tp_txn_result_free(&txn_result);
    TEST_ASSERT_FALSE(tp_session_recovery_available(session));

    tp_session_save_result save_result = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, path, &save_result, &err));
    TEST_ASSERT_TRUE(save_result.saved);
    TEST_ASSERT_FALSE(save_result.recovery_degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, save_result.recovery_status);
    TEST_ASSERT_TRUE(tp_session_recovery_available(session));

    uint8_t *healed_bytes = NULL;
    size_t healed_len = 0U;
    TEST_ASSERT_EQUAL_INT(
        0, io.read_all(io.ctx, (size_t)TP_JOURNAL_MAX_FILE_BYTES,
                       &healed_bytes, &healed_len));
    tp_journal_io recovered_io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(recovered_io.ctx);
    TEST_ASSERT_EQUAL_INT64(
        (int64_t)healed_len,
        recovered_io.write(recovered_io.ctx, healed_bytes, healed_len));
    TEST_ASSERT_EQUAL_INT(0, recovered_io.sync(recovered_io.ctx));
    free(healed_bytes);
    tp_model *recovered = NULL;
    tp_journal_recovery recovery = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_model_recover(recovered_io, key, &recovered, &recovery, &err));
    TEST_ASSERT_NOT_NULL(recovered);
    TEST_ASSERT_TRUE(tp_journal_contains(
        tp_model_journal(recovered),
        "61000000000000000000000000000001"));
    tp_journal_recovery_free(&recovery);
    tp_model_destroy(recovered);

    tp_txn__test_encode_stats_reset();
    session_apply_rename(session, "61000000000000000000000000000002",
                         1, atlas_id, "journaled-after-heal", TP_STATUS_OK,
                         &txn_result, &err);
    TEST_ASSERT_TRUE(txn_result.committed);
    tp_txn_result_free(&txn_result);
    TEST_ASSERT_EQUAL_size_t(1U, tp_txn__test_request_encode_calls());
    TEST_ASSERT_TRUE(tp_journal_contains(journal,
                                         "61000000000000000000000000000002"));

    tp_session_destroy(session);
    (void)remove(path);
}

void test_save_heal_failure_preserves_evidence_and_first_degradation_cause(void) {
    char path[1024];
    (void)snprintf(path, sizeof path,
                   "%s/tp_session_heal_failure.ntpacker_project", g_scratch);
    (void)remove(path);
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);

    tp_journal_io io = tp_journal_io_memory();
    tp_id128 key = {{0x62}};
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_attach_journal(session, journal, &err));
    tp_journal__test_set_record_limit(1U);
    tp_txn_result txn_result;
    session_apply_rename(session, "62000000000000000000000000000001",
                         0, atlas_id, "degraded-before-failed-heal",
                         TP_STATUS_OK, &txn_result, &err);
    tp_txn_result_free(&txn_result);
    TEST_ASSERT_FALSE(tp_session_recovery_available(session));
    const tp_session_recovery_health degraded_health =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_TRUE(degraded_health.degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          degraded_health.first_cause);
    TEST_ASSERT_TRUE(degraded_health.has_last_durable_revision);
    TEST_ASSERT_EQUAL_INT64(0, degraded_health.last_durable_revision);

    uint8_t *evidence_before = NULL;
    size_t evidence_before_len = 0U;
    TEST_ASSERT_EQUAL_INT(
        0, io.read_all(io.ctx, (size_t)TP_JOURNAL_MAX_FILE_BYTES,
                       &evidence_before, &evidence_before_len));
    TEST_ASSERT_GREATER_THAN_size_t(0U, evidence_before_len);
    const size_t syncs_before = tp_journal_io_memory__sync_count(io);
    tp_journal_io_memory__fail_next_writes(io, 1);

    tp_session_save_result save_result = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, path, &save_result, &err));
    TEST_ASSERT_TRUE(save_result.saved);
    TEST_ASSERT_TRUE(save_result.recovery_degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          save_result.recovery_status);
    TEST_ASSERT_FALSE(tp_session_recovery_available(session));
    const tp_session_recovery_health failed_heal_health =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_TRUE(failed_heal_health.degraded);
    TEST_ASSERT_EQUAL_INT(degraded_health.first_cause,
                          failed_heal_health.first_cause);
    TEST_ASSERT_EQUAL_INT64(degraded_health.last_durable_revision,
                            failed_heal_health.last_durable_revision);
    TEST_ASSERT_EQUAL_UINT64(degraded_health.generation,
                             failed_heal_health.generation);
    TEST_ASSERT_GREATER_THAN_size_t(syncs_before,
                                    tp_journal_io_memory__sync_count(io));

    uint8_t *evidence_after = NULL;
    size_t evidence_after_len = 0U;
    TEST_ASSERT_EQUAL_INT(
        0, io.read_all(io.ctx, (size_t)TP_JOURNAL_MAX_FILE_BYTES,
                       &evidence_after, &evidence_after_len));
    TEST_ASSERT_EQUAL_size_t(evidence_before_len, evidence_after_len);
    TEST_ASSERT_EQUAL_MEMORY(evidence_before, evidence_after,
                             evidence_before_len);
    TEST_ASSERT_FALSE(tp_journal_contains(
        journal, "62000000000000000000000000000001"));
    free(evidence_before);
    free(evidence_after);

    tp_journal__test_set_record_limit(0U);
    tp_txn__test_encode_stats_reset();
    session_apply_rename(session, "62000000000000000000000000000002",
                         1, atlas_id, "still-editable-after-heal-failure",
                         TP_STATUS_OK, &txn_result, &err);
    tp_txn_result_free(&txn_result);
    TEST_ASSERT_EQUAL_size_t(0U, tp_txn__test_request_encode_calls());

    tp_session_destroy(session);
    (void)remove(path);
}

void test_recovery_health_dto_tracks_durable_prefix_and_heal_transition(void) {
    char path[1024];
    (void)snprintf(path, sizeof path,
                   "%s/tp_session_recovery_health.ntpacker_project",
                   g_scratch);
    (void)remove(path);
    tp_session *session = make_session();
    tp_error err = {{0}};

    tp_session_recovery_health health =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_EQUAL_STRING(TP_SESSION_NOTICE_RECOVERY_DEGRADED,
                             health.notice_id);
    TEST_ASSERT_TRUE(health.available);
    TEST_ASSERT_FALSE(health.degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, health.first_cause);
    TEST_ASSERT_FALSE(health.has_last_durable_revision);
    TEST_ASSERT_FALSE(health.has_last_durable_time);

    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);

    tp_journal_io io = tp_journal_io_memory();
    tp_id128 key = {{0x63}};
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_attach_journal(session, journal, &err));
    tp_session_recovery_health attached =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_TRUE(attached.available);
    TEST_ASSERT_FALSE(attached.degraded);
    TEST_ASSERT_TRUE(attached.has_last_durable_revision);
    TEST_ASSERT_EQUAL_INT64(0, attached.last_durable_revision);
    TEST_ASSERT_FALSE(attached.has_last_durable_time);
    TEST_ASSERT_GREATER_THAN_UINT64(health.generation, attached.generation);

    tp_txn_result txn_result;
    session_apply_rename(session, "63000000000000000000000000000001",
                         0, atlas_id, "durable-txn", TP_STATUS_OK,
                         &txn_result, &err);
    TEST_ASSERT_TRUE(txn_result.committed);
    tp_txn_result_free(&txn_result);
    tp_session_recovery_health after_txn =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_EQUAL_INT64(1, after_txn.last_durable_revision);
    TEST_ASSERT_GREATER_THAN_UINT64(attached.generation,
                                    after_txn.generation);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_undo(session, &err));
    tp_session_recovery_health after_history =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_EQUAL_INT64(2, after_history.last_durable_revision);
    TEST_ASSERT_GREATER_THAN_UINT64(after_txn.generation,
                                    after_history.generation);

    tp_journal_io_memory__fail_next_sync(io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_redo(session, &err));
    tp_session_recovery_health degraded =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_FALSE(degraded.available);
    TEST_ASSERT_TRUE(degraded.degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, degraded.first_cause);
    TEST_ASSERT_TRUE(degraded.has_last_durable_revision);
    TEST_ASSERT_EQUAL_INT64(2, degraded.last_durable_revision);
    TEST_ASSERT_FALSE(degraded.has_last_durable_time);
    TEST_ASSERT_GREATER_THAN_UINT64(after_history.generation,
                                    degraded.generation);

    session_apply_rename(session, "63000000000000000000000000000002",
                         3, atlas_id, "suppressed-after-degrade", TP_STATUS_OK,
                         &txn_result, &err);
    TEST_ASSERT_TRUE(txn_result.committed);
    tp_txn_result_free(&txn_result);
    tp_session_recovery_health suppressed =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_EQUAL_INT64(2, suppressed.last_durable_revision);
    TEST_ASSERT_EQUAL_UINT64(degraded.generation, suppressed.generation);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, suppressed.first_cause);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    tp_session_recovery_health snap_health =
        tp_session_snapshot_recovery_health_query(snapshot);
    TEST_ASSERT_EQUAL_UINT64(suppressed.generation, snap_health.generation);
    TEST_ASSERT_EQUAL_INT64(suppressed.last_durable_revision,
                            snap_health.last_durable_revision);
    TEST_ASSERT_EQUAL_INT(suppressed.first_cause, snap_health.first_cause);
    TEST_ASSERT_TRUE(snap_health.degraded);
    tp_session_snapshot_destroy(snapshot);

    tp_session_save_result save_result = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, path, &save_result, &err));
    TEST_ASSERT_FALSE(save_result.recovery_degraded);
    tp_session_recovery_health healed =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_TRUE(healed.available);
    TEST_ASSERT_FALSE(healed.degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, healed.first_cause);
    TEST_ASSERT_EQUAL_INT64(4, healed.last_durable_revision);
    TEST_ASSERT_FALSE(healed.has_last_durable_time);
    TEST_ASSERT_GREATER_THAN_UINT64(suppressed.generation,
                                    healed.generation);

    tp_session_destroy(session);
    (void)remove(path);
}

void test_open_holds_project_lease_until_session_close(void) {
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/tp_session_lease_open.ntpacker_project", g_scratch);
    (void)remove(path);
    tp_error err = {{0}};
    save_project_with_ids(path, 31U);

    uint8_t first_seed = 41U;
    uint8_t second_seed = 99U;
    tp_rng first_rng = {deterministic_fill, &first_seed};
    tp_rng second_rng = {deterministic_fill, &second_seed};
    tp_session *first = NULL;
    tp_session *second = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_open(path, &first_rng, &first, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_PROJECT_LIVE,
                          tp_session_open(path, &second_rng, &second, &err));
    TEST_ASSERT_NULL(second);
    tp_session_destroy(first);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_open(path, &second_rng, &second, &err));
    tp_session_destroy(second);
    (void)remove(path);
}

void test_save_new_never_replaces_existing_destination(void) {
    char path[1024];
    (void)snprintf(path, sizeof path,
                   "%s/tp_session_create_only.ntpacker_project", g_scratch);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    static const char sentinel[] = "created-by-another-writer";
    TEST_ASSERT_EQUAL_UINT64(sizeof sentinel - 1U,
                             fwrite(sentinel, 1U, sizeof sentinel - 1U, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    tp_session *session = make_session();
    tp_session_save_result result;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_FILE_EXISTS,
                          tp_session_save_new(session, path, &result, &err));
    TEST_ASSERT_FALSE(result.saved);
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    TEST_ASSERT_EQUAL_INT(TP_IDENTITY_UNSAVED,
                          tp_session_snapshot_identity(snapshot).kind);
    tp_session_snapshot_destroy(snapshot);
    file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    char actual[sizeof sentinel] = {0};
    TEST_ASSERT_EQUAL_UINT64(sizeof sentinel - 1U,
                             fread(actual, 1U, sizeof sentinel - 1U, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_STRING(sentinel, actual);
    tp_session_destroy(session);
    (void)remove(path);
}

void test_save_as_acquires_destination_before_releasing_old_lease(void) {
    char original[1024];
    char destination[1024];
    (void)snprintf(original, sizeof original, "%s/tp_session_lease_a.ntpacker_project", g_scratch);
    (void)snprintf(destination, sizeof destination, "%s/tp_session_lease_b.ntpacker_project", g_scratch);
    (void)remove(original);
    (void)remove(destination);
    tp_error err = {{0}};
    save_project_with_ids(original, 32U);

    uint8_t seed = 51U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_open(original, &rng, &session, &err));

    tp_session_save_result result;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, destination, &result, &err));
    tp_project_lease *probe = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(original, &probe, &err));
    tp_project_lease_release(probe);
    probe = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_PROJECT_LIVE,
                          tp_project_lease_acquire(destination, &probe, &err));
    TEST_ASSERT_NULL(probe);
    tp_session_destroy(session);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(destination, &probe, &err));
    tp_project_lease_release(probe);
    (void)remove(original);
    (void)remove(destination);
}

void test_save_as_lease_conflict_preserves_old_identity_lease(void) {
    char original[1024];
    char destination[1024];
    (void)snprintf(original, sizeof original, "%s/tp_session_lease_keep.ntpacker_project", g_scratch);
    (void)snprintf(destination, sizeof destination, "%s/tp_session_lease_busy.ntpacker_project", g_scratch);
    (void)remove(original);
    (void)remove(destination);
    tp_error err = {{0}};
    save_project_with_ids(original, 33U);

    uint8_t seed = 61U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_open(original, &rng, &session, &err));
    tp_project_lease *busy = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(destination, &busy, &err));
    tp_session_save_result result;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_PROJECT_LIVE,
                          tp_session_save_as(session, destination, &result, &err));
    tp_project_lease *old_probe = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_PROJECT_LIVE,
                          tp_project_lease_acquire(original, &old_probe, &err));
    TEST_ASSERT_NULL(old_probe);
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &snapshot, &err));
    assert_canonical_path(
        original, tp_session_snapshot_identity(snapshot).canonical_path);
    tp_session_snapshot_destroy(snapshot);
    tp_project_lease_release(busy);
    tp_session_destroy(session);
    (void)remove(original);
    (void)remove(destination);
}

void test_save_as_publish_failure_releases_destination_and_keeps_old_lease(void) {
    char original[1024];
    char destination[1024];
    (void)snprintf(original, sizeof original, "%s/tp_session_lease_publish_a.ntpacker_project", g_scratch);
    (void)snprintf(destination, sizeof destination, "%s/tp_session_lease_publish_b.ntpacker_project", g_scratch);
    (void)remove(original);
    (void)remove(destination);
    tp_error err = {{0}};
    save_project_with_ids(original, 34U);

    uint8_t seed = 71U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_open(original, &rng, &session, &err));
    tp_project__test_fail_next_temp_create();
    tp_session_save_result result;
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK,
                          tp_session_save_as(session, destination, &result, &err));

    tp_project_lease *probe = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(destination, &probe, &err));
    tp_project_lease_release(probe);
    probe = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_PROJECT_LIVE,
                          tp_project_lease_acquire(original, &probe, &err));
    TEST_ASSERT_NULL(probe);
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_snapshot_create(session, &snapshot, &err));
    assert_canonical_path(
        original, tp_session_snapshot_identity(snapshot).canonical_path);
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
    (void)remove(original);
    (void)remove(destination);
}

void test_save_parent_sync_warning_advances_authoritative_session_state(void) {
    char path[1024];
    (void)snprintf(path, sizeof path,
                   "%s/tp_session_parent_sync_warning.ntpacker_project",
                   g_scratch);
    (void)remove(path);
    save_project_with_ids(path, 36U);

    tp_error error = {{0}};
    uint8_t seed = 75U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_open(path, &rng, &session, &error));
    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &before, &error));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(before, 0)->id;
    const int64_t revision = tp_session_snapshot_revision(before);
    tp_session_snapshot_destroy(before);

    tp_txn_result transaction;
    session_apply_rename(session, "82828282828282828282828282828282",
                         revision, atlas_id, "published-with-warning",
                         TP_STATUS_OK, &transaction, &error);
    tp_txn_result_free(&transaction);

    tp_project__test_fail_next_parent_sync();
    tp_session_save_result save_result;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_save(session, &save_result, &error));
    TEST_ASSERT_TRUE(save_result.saved);
    TEST_ASSERT_TRUE(save_result.file_durability_degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_FILE_DURABILITY_UNCERTAIN,
                          save_result.file_durability_status);
    TEST_ASSERT_EQUAL_CHAR('\0', error.msg[0]);

    tp_session_snapshot *saved = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &saved, &error));
    TEST_ASSERT_FALSE(tp_session_snapshot_dirty(saved));
    tp_id128 saved_fingerprint = tp_id128_nil();
    TEST_ASSERT_TRUE(tp_session_snapshot_saved_file_fingerprint(
        saved, &saved_fingerprint));
    TEST_ASSERT_TRUE(tp_id128_eq(saved_fingerprint,
                                 save_result.file_fingerprint));
    tp_session_snapshot_destroy(saved);

    tp_project *published = NULL;
    tp_id128 disk_fingerprint = tp_id128_nil();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_load_with_fingerprint(path, &published, &disk_fingerprint,
                                         &error));
    TEST_ASSERT_EQUAL_STRING("published-with-warning",
                             tp_project_get_atlas(published, 0)->name);
    TEST_ASSERT_TRUE(tp_id128_eq(saved_fingerprint, disk_fingerprint));
    tp_project_destroy(published);

    /* A retry is safe because the session advanced its optimistic-concurrency
     * baseline to the bytes that were actually published. */
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_save(session, &save_result, &error));
    TEST_ASSERT_FALSE(save_result.file_durability_degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          save_result.file_durability_status);

    tp_session_destroy(session);
    (void)remove(path);
}

void test_save_rejects_external_change_without_overwriting_it(void) {
    char path[1024];
    (void)snprintf(path, sizeof path,
                   "%s/tp_session_external_change.ntpacker_project", g_scratch);
    (void)remove(path);
    save_project_with_ids(path, 35U);

    tp_error err = {{0}};
    uint8_t seed = 81U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_open(path, &rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    tp_id128 opened_fingerprint = tp_id128_nil();
    TEST_ASSERT_TRUE(tp_session_snapshot_saved_file_fingerprint(
        snapshot, &opened_fingerprint));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    tp_txn_result txn_result;
    session_apply_rename(session, "81818181818181818181818181818181",
                         tp_session_snapshot_revision(snapshot), atlas->id,
                         "local-change", TP_STATUS_OK, &txn_result, &err);
    tp_txn_result_free(&txn_result);
    tp_session_snapshot_destroy(snapshot);

    static const char external[] = "external-owner\n";
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_UINT(sizeof external - 1U,
                           fwrite(external, 1U, sizeof external - 1U, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    tp_session_save_result save_result;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_FILE_CHANGED_EXTERNALLY,
                          tp_session_save(session, &save_result, &err));
    char actual[sizeof external] = {0};
    file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_UINT(sizeof external - 1U,
                           fread(actual, 1U, sizeof external - 1U, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_STRING(external, actual);
    snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    tp_id128 retained_fingerprint = tp_id128_nil();
    TEST_ASSERT_TRUE(tp_session_snapshot_saved_file_fingerprint(
        snapshot, &retained_fingerprint));
    TEST_ASSERT_TRUE(tp_id128_eq(opened_fingerprint, retained_fingerprint));
    tp_session_snapshot_destroy(snapshot);

    tp_session_destroy(session);
    (void)remove(path);
}

/* Create a recovery store rooted at g_scratch, a live journal at
 * `journal_path`, and a fresh session attached to it with the given
 * metadata; asserts each step succeeds. Returns the session and writes the
 * store to *store_out (both owned by the caller as usual). */
static tp_session *attach_live_recovery(const char *journal_path,
                                        int64_t timestamp,
                                        const char *project_name,
                                        tp_recovery_store **store_out,
                                        tp_error *err) {
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_recovery_store_create(g_scratch, recovery_key(), store_out, err));
    tp_recovery_live *live = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_recovery_store_create_live(*store_out, journal_path, &live, err));
    tp_session *session = make_session();
    tp_recovery_metadata metadata = {
        .timestamp = timestamp,
        .project_path = "",
        .project_name = project_name,
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_attach_recovery_live(session, live, &metadata, err));
    return session;
}

void test_session_owns_live_recovery_clean_close_order(void) {
    char journal[1024];
    char lock[1050];
    (void)snprintf(journal, sizeof journal, "%s/session-live.ntpjournal", g_scratch);
    (void)snprintf(lock, sizeof lock, "%s.lock", journal);
    (void)remove(journal);
    (void)remove(lock);
    tp_recovery_store *store = NULL;
    tp_error err = {{0}};
    tp_session *session = attach_live_recovery(journal, 60, "session-live",
                                               &store, &err);
    TEST_ASSERT_TRUE(tp_session_recovery_available(session));
    TEST_ASSERT_TRUE(test_file_exists(journal));
    tp_session_destroy(session);
    TEST_ASSERT_FALSE(test_file_exists(journal));
    TEST_ASSERT_TRUE(test_file_exists(lock));
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, &err));
    tp_recovery_claim_release(claim);
    tp_recovery_store_destroy(store);
}

void test_session_preserves_dirty_live_recovery_on_destroy(void) {
    char journal[1024];
    (void)snprintf(journal, sizeof journal, "%s/session-dirty.ntpjournal", g_scratch);
    (void)remove(journal);
    tp_recovery_store *store = NULL;
    tp_error err = {{0}};
    tp_session *session = attach_live_recovery(journal, 70, "session-dirty",
                                               &store, &err);
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);
    tp_txn_result result;
    session_apply_rename(session, "10000000000000000000000000000070", 0,
                         atlas_id, "dirty-live", TP_STATUS_OK, &result, &err);
    tp_session_destroy(session);
    TEST_ASSERT_TRUE(test_file_exists(journal));
    tp_recovery_candidates candidates;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_scan(store, NULL, &candidates, &err));
    bool found = false;
    for (size_t i = 0U; i < candidates.count; ++i) {
        found = found || strstr(candidates.items[i].journal_path,
                                "session-dirty.ntpjournal") != NULL;
    }
    TEST_ASSERT_TRUE(found);
    tp_recovery_store_destroy(store);
}

void test_session_preserves_live_recovery_after_uncertain_save_on_destroy(void) {
    char journal[1024];
    char target[1024];
    (void)snprintf(journal, sizeof journal,
                   "%s/session-uncertain-save.ntpjournal", g_scratch);
    (void)snprintf(target, sizeof target,
                   "%s/session-uncertain-save.ntpacker_project", g_scratch);
    (void)remove(journal);
    (void)remove(target);

    tp_recovery_store *store = NULL;
    tp_error err = {{0}};
    tp_session *session = attach_live_recovery(
        journal, 72, "session-uncertain-save", &store, &err);
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);

    tp_txn_result transaction;
    session_apply_rename(session, "10000000000000000000000000000072",
                         0, atlas_id, "published-but-not-directory-durable",
                         TP_STATUS_OK, &transaction, &err);
    tp_txn_result_free(&transaction);

    tp_project__test_fail_next_parent_sync();
    tp_session_save_result save_result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_save_as(session, target, &save_result, &err));
    TEST_ASSERT_TRUE(save_result.saved);
    TEST_ASSERT_TRUE(save_result.file_durability_degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_FILE_DURABILITY_UNCERTAIN,
                          save_result.file_durability_status);

    tp_session_destroy(session);
    TEST_ASSERT_TRUE_MESSAGE(
        test_file_exists(journal),
        "uncertain directory durability must preserve recovery evidence on close");

    tp_recovery_store_destroy(store);
    (void)remove(journal);
    (void)remove(target);
}

void test_durable_retry_clears_uncertain_save_recovery_preservation(void) {
    char journal[1024];
    char target[1024];
    (void)snprintf(journal, sizeof journal,
                   "%s/session-uncertain-retry.ntpjournal", g_scratch);
    (void)snprintf(target, sizeof target,
                   "%s/session-uncertain-retry.ntpacker_project", g_scratch);
    (void)remove(journal);
    (void)remove(target);

    tp_recovery_store *store = NULL;
    tp_error err = {{0}};
    tp_session *session = attach_live_recovery(
        journal, 73, "session-uncertain-retry", &store, &err);

    tp_project__test_fail_next_parent_sync();
    tp_session_save_result save_result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_save_as(session, target, &save_result, &err));
    TEST_ASSERT_TRUE(save_result.file_durability_degraded);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_save(session, &save_result, &err));
    TEST_ASSERT_FALSE(save_result.file_durability_degraded);

    tp_session_destroy(session);
    TEST_ASSERT_FALSE_MESSAGE(
        test_file_exists(journal),
        "fully durable retry must release uncertain-save preservation");

    tp_recovery_store_destroy(store);
    (void)remove(target);
}

void test_save_as_updates_live_recovery_identity_before_compaction(void) {
    char journal[1024];
    char target[1024];
    (void)snprintf(journal, sizeof journal,
                   "%s/session-save-identity.ntpjournal", g_scratch);
    (void)snprintf(target, sizeof target,
                   "%s/session-save-identity.ntpacker_project", g_scratch);
    (void)remove(journal);
    (void)remove(target);
    tp_recovery_store *store = NULL;
    tp_error err = {{0}};
    tp_session *session = attach_live_recovery(journal, 75,
                                               "session-save-identity",
                                               &store, &err);
    tp_session_save_result save_result;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, target, &save_result, &err));
    TEST_ASSERT_TRUE(save_result.saved);
    TEST_ASSERT_FALSE(save_result.recovery_degraded);

    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    const int64_t revision = tp_session_snapshot_revision(snapshot);
    tp_session_snapshot_destroy(snapshot);
    tp_journal__test_set_record_limit(2U);
    tp_txn_result txn_result;
    session_apply_rename(session, "10000000000000000000000000000075",
                         revision, atlas_id, "dirty-before-live-heal",
                         TP_STATUS_OK, &txn_result, &err);
    tp_txn_result_free(&txn_result);
    TEST_ASSERT_FALSE(tp_session_recovery_available(session));
    tp_journal__test_set_record_limit(0U);

    tp_session_save_result healed_result = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save(session, &healed_result, &err));
    TEST_ASSERT_TRUE(healed_result.saved);
    TEST_ASSERT_FALSE(healed_result.recovery_degraded);
    TEST_ASSERT_TRUE(tp_session_recovery_available(session));
    session_apply_rename(session, "10000000000000000000000000000077",
                         revision + 1, atlas_id, "dirty-after-live-heal",
                         TP_STATUS_OK, &txn_result, &err);
    tp_txn_result_free(&txn_result);
    tp_session_destroy(session);

    tp_recovery_candidates candidates;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_scan(store, NULL, &candidates, &err));
    char canonical[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_identity_path_canonical(target, canonical,
                                                     sizeof canonical, &err));
    bool found = false;
    for (size_t i = 0U; i < candidates.count; ++i) {
        if (strstr(candidates.items[i].journal_path,
                   "session-save-identity.ntpjournal") != NULL) {
            found = true;
            TEST_ASSERT_EQUAL_STRING(canonical, candidates.items[i].original_path);
            TEST_ASSERT_EQUAL_STRING("session-save-identity.ntpacker_project",
                                     candidates.items[i].name);
            TEST_ASSERT_TRUE(candidates.items[i].has_file_fingerprint);
            TEST_ASSERT_TRUE(tp_id128_eq(healed_result.file_fingerprint,
                                         candidates.items[i].file_fingerprint));
        }
    }
    TEST_ASSERT_TRUE(found);
    tp_recovery_store_destroy(store);
    (void)remove(journal);
    (void)remove(target);
}

void test_cross_identity_save_retires_journal_after_metadata_failure(void) {
    char journal[1024];
    char original[1024];
    char target[1024];
    (void)snprintf(journal, sizeof journal,
                   "%s/session-predegraded.ntpjournal", g_scratch);
    (void)snprintf(original, sizeof original,
                   "%s/session-predegraded-a.ntpacker_project", g_scratch);
    (void)snprintf(target, sizeof target,
                   "%s/session-predegraded-b.ntpacker_project", g_scratch);
    (void)remove(journal);
    (void)remove(original);
    (void)remove(target);
    tp_recovery_store *store = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_scratch, recovery_key(), &store, &err));
    tp_session *session = make_session();
    tp_session_save_result original_result;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, original,
                                             &original_result, &err));
    tp_recovery_live *live = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &live, &err));
    const tp_recovery_metadata metadata = {
        .timestamp = 76,
        .project_path = original_result.target_path,
        .project_name = "session-predegraded-a.ntpacker_project",
        .file_fingerprint = &original_result.file_fingerprint,
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_attach_recovery_live(session, live, &metadata, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    const int64_t expected_rev = tp_session_snapshot_revision(snapshot);
    tp_session_snapshot_destroy(snapshot);
    tp_txn_result txn_result;
    session_apply_rename(session, "10000000000000000000000000000076",
                         expected_rev, atlas_id, "dirty-before-save-as",
                         TP_STATUS_OK, &txn_result, &err);
    tp_txn_result_free(&txn_result);
    /* Checkpoint + initial META + transaction consume all three slots, so the
     * Save As identity META fails only after B was published. */
    tp_journal__test_set_record_limit(3U);
    tp_session_save_result result;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, target, &result, &err));
    tp_journal__test_set_record_limit(0U);
    TEST_ASSERT_TRUE(result.saved);
    TEST_ASSERT_TRUE(result.recovery_degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, result.recovery_status);
    tp_id128 original_after;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_identity_file_fingerprint(original,
                                                       &original_after, &err));
    TEST_ASSERT_TRUE(tp_id128_eq(original_result.file_fingerprint,
                                 original_after));
    tp_session_destroy(session);
    TEST_ASSERT_FALSE(test_file_exists(journal));
    tp_recovery_candidates candidates;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_scan(store, NULL, &candidates, &err));
    for (size_t i = 0U; i < candidates.count; ++i) {
        TEST_ASSERT_NULL(strstr(candidates.items[i].journal_path,
                                "session-predegraded.ntpjournal"));
    }
    tp_recovery_store_destroy(store);
    (void)remove(journal);
    (void)remove(original);
    (void)remove(target);
}

void test_degraded_cross_identity_save_as_rebinds_fresh_recovery_owner(void) {
    char retired_journal[1024];
    char fresh_journal[1024];
    char target[1024];
    (void)snprintf(retired_journal, sizeof retired_journal,
                   "%s/session-rebind-retired.ntpjournal", g_scratch);
    (void)snprintf(fresh_journal, sizeof fresh_journal,
                   "%s/session-rebind-fresh.ntpjournal", g_scratch);
    (void)snprintf(target, sizeof target,
                   "%s/session-rebind.ntpacker_project", g_scratch);
    (void)remove(retired_journal);
    (void)remove(fresh_journal);
    (void)remove(target);

    tp_error err = {{0}};
    tp_recovery_store *store = NULL;
    tp_session *session = attach_live_recovery(
        retired_journal, 82, "session-rebind", &store, &err);
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);

    tp_journal__test_set_record_limit(2U);
    tp_txn_result txn_result;
    session_apply_rename(session, "10000000000000000000000000000082",
                         0, atlas_id, "degraded-before-rebind",
                         TP_STATUS_OK, &txn_result, &err);
    tp_txn_result_free(&txn_result);
    TEST_ASSERT_TRUE(tp_session_recovery_health_query(session).degraded);
    tp_journal__test_set_record_limit(0U);

    tp_session_save_result result = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, target, &result, &err));
    TEST_ASSERT_TRUE(result.saved);
    TEST_ASSERT_TRUE(result.recovery_degraded);
    TEST_ASSERT_TRUE_MESSAGE(
        result.recovery_rebind_required,
        "cross-identity degraded Save As must request a fresh recovery slot");
    TEST_ASSERT_FALSE_MESSAGE(
        tp_session__has_recovery_owner(session),
        "retired recovery handle must not remain the session owner");

    const tp_recovery_metadata metadata = {
        .timestamp = 83,
        .project_path = result.target_path,
        .project_name = "session-rebind.ntpacker_project",
        .file_fingerprint = &result.file_fingerprint,
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_recovery__test_session_attach_at(
            g_scratch, recovery_key(), fresh_journal, session, &metadata,
            &err));
    const tp_session_recovery_health rebound =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_TRUE(rebound.available);
    TEST_ASSERT_FALSE(rebound.degraded);

    session_apply_rename(session, "10000000000000000000000000000083",
                         1, atlas_id, "recorded-after-rebind",
                         TP_STATUS_OK, &txn_result, &err);
    tp_txn_result_free(&txn_result);

    tp_session_destroy(session);
    tp_recovery_store_destroy(store);
    (void)remove(retired_journal);
    (void)remove(fresh_journal);
    (void)remove(target);
}

void test_cross_identity_retire_preserves_sticky_recovery_first_cause(void) {
    char journal[1024];
    char target[1024];
    (void)snprintf(journal, sizeof journal,
                   "%s/session-sticky-retire.ntpjournal", g_scratch);
    (void)snprintf(target, sizeof target,
                   "%s/session-sticky-retire.ntpacker_project", g_scratch);
    (void)remove(journal);
    (void)remove(target);
    tp_error err = {{0}};
    tp_recovery_store *store = NULL;
    tp_session *session = attach_live_recovery(
        journal, 81, "session-sticky-retire", &store, &err);

    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);
    tp_journal__test_set_record_limit(2U);
    tp_txn_result txn_result;
    session_apply_rename(session, "10000000000000000000000000000081",
                         0, atlas_id, "degraded-before-retire", TP_STATUS_OK,
                         &txn_result, &err);
    tp_txn_result_free(&txn_result);
    const tp_session_recovery_health before =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_TRUE(before.degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, before.first_cause);
    tp_journal__test_set_record_limit(0U);

    tp_session_save_result result = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, target, &result, &err));
    TEST_ASSERT_TRUE(result.saved);
    TEST_ASSERT_TRUE(result.recovery_degraded);
    const tp_session_recovery_health after =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_TRUE(after.degraded);
    TEST_ASSERT_EQUAL_INT(before.first_cause, after.first_cause);
    TEST_ASSERT_EQUAL_UINT64(before.generation, after.generation);

    tp_session_destroy(session);
    tp_recovery_store_destroy(store);
    (void)remove(journal);
    (void)remove(target);
}

void test_save_as_keeps_published_destination_when_recovery_retire_fails(void) {
    char journal[1024];
    char original[1024];
    char target[1024];
    (void)snprintf(journal, sizeof journal,
                   "%s/session-retire-fail.ntpjournal", g_scratch);
    (void)snprintf(original, sizeof original,
                   "%s/session-retire-fail-a.ntpacker_project", g_scratch);
    (void)snprintf(target, sizeof target,
                   "%s/session-retire-fail-b.ntpacker_project", g_scratch);
    (void)remove(journal);
    (void)remove(original);
    (void)remove(target);

    tp_error err = {{0}};
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_scratch, recovery_key(),
                                                   &store, &err));
    tp_session *session = make_session();
    tp_session_save_result original_result;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, original,
                                             &original_result, &err));

    tp_recovery_live *live = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &live,
                                                        &err));
    const tp_recovery_metadata metadata = {
        .timestamp = 77,
        .project_path = original_result.target_path,
        .project_name = "session-retire-fail-a.ntpacker_project",
        .file_fingerprint = &original_result.file_fingerprint,
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_attach_recovery_live(session, live,
                                                          &metadata, &err));
    tp_recovery_live__mark_degraded(live);
    tp_recovery__test_fail_next_live_retire_cleanup();

    tp_session_save_result result;
    const tp_status status =
        tp_session_save_as(session, target, &result, &err);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, status);
    TEST_ASSERT_TRUE(result.saved);
    TEST_ASSERT_TRUE(result.recovery_degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_RECOVERY_CLEANUP_FAILED,
                          result.recovery_status);
    TEST_ASSERT_TRUE(result.recovery_rebind_required);
    TEST_ASSERT_FALSE(tp_session__has_recovery_owner(session));
    TEST_ASSERT_TRUE(test_file_exists(target));

    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    char canonical[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_identity_path_canonical(target, canonical,
                                                     sizeof canonical, &err));
    TEST_ASSERT_EQUAL_STRING(
        canonical, tp_session_snapshot_identity(snapshot).canonical_path);
    TEST_ASSERT_FALSE(tp_session_snapshot_dirty(snapshot));
    tp_session_snapshot_destroy(snapshot);

    tp_session_destroy(session);
    tp_recovery_store_destroy(store);
    (void)remove(journal);
    (void)remove(original);
    (void)remove(target);
}

void test_degraded_session_live_keeps_mutation_available_and_preserves_slot(void) {
    char journal[1024];
    (void)snprintf(journal, sizeof journal, "%s/session-degraded.ntpjournal", g_scratch);
    (void)remove(journal);
    tp_mkdirs(journal);
    TEST_ASSERT_TRUE(tp_scan_is_dir(journal));
    tp_recovery_store *store = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_scratch, recovery_key(), &store, &err));
    tp_recovery_live *live = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &live, &err));
    tp_session *session = make_session();
    tp_recovery_metadata metadata = {
        .timestamp = 80,
        .project_path = "",
        .project_name = "session-degraded",
    };
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK,
                          tp_session_attach_recovery_live(session, live, &metadata, &err));
    TEST_ASSERT_FALSE(tp_session_recovery_available(session));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);
    tp_txn_result result;
    session_apply_rename(session, "10000000000000000000000000000080", 0,
                         atlas_id, "commits-without-recovery", TP_STATUS_OK,
                         &result, &err);
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);
    tp_session_destroy(session);
    TEST_ASSERT_TRUE(tp_scan_is_dir(journal));
    tp_recovery_store_destroy(store);
#ifdef _WIN32
    TEST_ASSERT_EQUAL_INT(0, _rmdir(journal));
#else
    TEST_ASSERT_EQUAL_INT(0, rmdir(journal));
#endif
}

void test_required_recovery_reports_unavailable_without_blocking_model_commands(void) {
    tp_session *session = make_session();
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_id128 atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    tp_session_snapshot_destroy(snapshot);

    tp_operation committed = rename_op(atlas_id, "before-required-recovery");
    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "10000000000000000000000000000081", 33U);
    request.expected_revision = 0;
    request.ops = &committed;
    request.op_count = 1;
    tp_txn_result result;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_apply(session, &request, &result, &err));
    tp_operation_free(&committed);
    TEST_ASSERT_EQUAL_INT64(1, tp_session_revision(session));

    tp_operation committed_again = rename_op(atlas_id, "before-required-recovery-2");
    request.ops = &committed_again;
    request.expected_revision = 1;
    memcpy(request.id_hex, "10000000000000000000000000000082", 33U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_apply(session, &request, &result, &err));
    tp_operation_free(&committed_again);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_undo(session, &err));
    TEST_ASSERT_TRUE(tp_session_can_undo(session));
    TEST_ASSERT_TRUE(tp_session_can_redo(session));
    TEST_ASSERT_EQUAL_INT64(3, tp_session_revision(session));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_require_recovery(session, &err));
    TEST_ASSERT_FALSE(tp_session_recovery_available(session));
    const tp_session_recovery_health missing =
        tp_session_recovery_health_query(session);
    TEST_ASSERT_FALSE(missing.available);
    TEST_ASSERT_FALSE(missing.degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, missing.first_cause);
    TEST_ASSERT_TRUE(tp_session_can_undo(session));
    TEST_ASSERT_TRUE(tp_session_can_redo(session));

    tp_operation rejected = rename_op(atlas_id, "must-not-commit");
    request.ops = &rejected;
    request.expected_revision = 3;
    memcpy(request.id_hex, "10000000000000000000000000000083", 33U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_apply(session, &request, &result, &err));
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);
    tp_operation_free(&rejected);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_undo(session, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_redo(session, &err));
    TEST_ASSERT_EQUAL_INT64(6, tp_session_revision(session));
    tp_session_destroy(session);
}

int main(int argc, char **argv) {
    TEST_ASSERT_TRUE(argc >= 2);
    g_scratch = argv[1];
    UNITY_BEGIN();
    RUN_TEST(test_snapshot_preview_apply_isolated_and_revision_exact);
    RUN_TEST(test_snapshot_allocation_failures_return_structured_oom);
    RUN_TEST(test_snapshot_creation_does_not_clone_and_same_generation_shares_project);
    RUN_TEST(test_snapshot_survives_session_destroy);
    RUN_TEST(test_generation_owner_oom_is_structured_and_retryable);
    RUN_TEST(test_owned_snapshot_survives_later_commit);
    RUN_TEST(test_read_snapshot_rejects_unsupported_schema);
    RUN_TEST(test_writable_open_save_preserves_canonical_orphan);
    RUN_TEST(test_source_batch_plan_collapses_lexical_and_symlink_aliases);
    RUN_TEST(test_source_batch_plan_rejects_empty_inputs_atomically);
    RUN_TEST(test_rejected_commit_does_not_publish_event_or_generation);
    RUN_TEST(test_invalid_operation_reject_publishes_no_event_and_id_stays_retryable);
    RUN_TEST(test_multi_op_batch_publishes_single_commit_event);
    RUN_TEST(test_no_change_apply_does_not_publish_event_or_generation);
    RUN_TEST(test_undo_redo_are_session_commands_with_ordered_events);
    RUN_TEST(test_journal_append_failure_commits_and_sticky_degradation_skips_later_recovery_work);
    RUN_TEST(test_journal_admission_failure_commits_and_degrades_recovery);
    RUN_TEST(test_journal_encode_failure_commits_and_degrades_recovery);
    RUN_TEST(test_journal_sync_failure_commits_and_degrades_recovery);
    RUN_TEST(test_source_invalidation_is_runtime_only_and_event_window_resyncs);
    RUN_TEST(test_save_as_and_open_are_session_owned_commands);
    RUN_TEST(test_save_as_does_not_mutate_held_snapshot_project_dir);
    RUN_TEST(test_detached_recovery_save_publishes_new_model_generation);
    RUN_TEST(test_snapshot_has_id_addressed_nested_dtos);
    RUN_TEST(test_snapshot_resolves_source_by_direct_indices);
    RUN_TEST(test_snapshot_source_resolution_failures_fill_error_context);
    RUN_TEST(test_snapshot_selector_uses_canonical_ambiguity_and_atlas_scope);
    RUN_TEST(test_snapshot_sprite_selector_matches_headless_canonical_operation);
    RUN_TEST(test_snapshot_frontend_queries_delegate_core_rules);
    RUN_TEST(test_source_plan_propagates_stored_base_overflow);
    RUN_TEST(test_future_event_cursor_requires_resync);
    RUN_TEST(test_save_reports_recovery_degradation_and_keeps_later_mutation_available);
    RUN_TEST(test_save_heals_degraded_recovery_and_resumes_journaling);
    RUN_TEST(test_save_heal_failure_preserves_evidence_and_first_degradation_cause);
    RUN_TEST(test_recovery_health_dto_tracks_durable_prefix_and_heal_transition);
    RUN_TEST(test_open_holds_project_lease_until_session_close);
    RUN_TEST(test_save_new_never_replaces_existing_destination);
    RUN_TEST(test_save_as_acquires_destination_before_releasing_old_lease);
    RUN_TEST(test_save_as_lease_conflict_preserves_old_identity_lease);
    RUN_TEST(test_save_as_publish_failure_releases_destination_and_keeps_old_lease);
    RUN_TEST(test_save_parent_sync_warning_advances_authoritative_session_state);
    RUN_TEST(test_save_rejects_external_change_without_overwriting_it);
    RUN_TEST(test_session_owns_live_recovery_clean_close_order);
    RUN_TEST(test_session_preserves_dirty_live_recovery_on_destroy);
    RUN_TEST(test_session_preserves_live_recovery_after_uncertain_save_on_destroy);
    RUN_TEST(test_durable_retry_clears_uncertain_save_recovery_preservation);
    RUN_TEST(test_save_as_updates_live_recovery_identity_before_compaction);
    RUN_TEST(test_cross_identity_save_retires_journal_after_metadata_failure);
    RUN_TEST(test_degraded_cross_identity_save_as_rebinds_fresh_recovery_owner);
    RUN_TEST(test_cross_identity_retire_preserves_sticky_recovery_first_cause);
    RUN_TEST(test_save_as_keeps_published_destination_when_recovery_retire_fails);
    RUN_TEST(test_degraded_session_live_keeps_mutation_available_and_preserves_slot);
    RUN_TEST(test_required_recovery_reports_unavailable_without_blocking_model_commands);
    return UNITY_END();
}
