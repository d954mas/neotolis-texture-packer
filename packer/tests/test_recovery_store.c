#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "tp_core/tp_journal.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_lease.h"
#include "tp_core/tp_project_migrate.h"
#include "tp_core/tp_recovery.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "tp_journal_internal.h"
#include "tp_model_seam.h"
#include "tp_project_internal.h"
#include "tp_recovery_internal.h"
#include "unity.h"

static const char *g_self;
static const char *g_root;

void setUp(void) {}
void tearDown(void) {}

static tp_id128 recovery_key(void) {
    tp_id128 key;
    static const uint8_t bytes[16] = {'n', 't', 'p', 'k', '_', 'r', 'e', 'c',
                                      'o', 'v', 'e', 'r', 'y', '_', '0', '1'};
    memcpy(key.bytes, bytes, sizeof bytes);
    return key;
}

static void join_path(char *out, size_t cap, const char *root, const char *leaf) {
    (void)snprintf(out, cap, "%s/%s", root, leaf);
}

#ifndef _WIN32
static bool create_isolated_root(char *out, size_t cap, const char *label) {
    for (unsigned attempt = 0U; attempt < 16U; ++attempt) {
        const int written = snprintf(out, cap, "%s/%s-%ld-%u", g_root, label,
                                     (long)getpid(), attempt);
        if (written < 0 || (size_t)written >= cap) {
            return false;
        }
        if (mkdir(out, 0700) == 0) {
            return true;
        }
        if (errno != EEXIST) {
            return false;
        }
    }
    return false;
}
#endif

static void write_file(const char *path, const char *bytes) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    const size_t len = strlen(bytes);
    TEST_ASSERT_EQUAL_UINT64(len, fwrite(bytes, 1U, len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static bool file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    (void)fclose(file);
    return true;
}

static int det_fill(void *ctx, uint8_t *out, size_t len);
static void make_preserved_live_journal(tp_recovery_store *store,
                                        const char *journal,
                                        int64_t timestamp);
static void craft_orphan(const char *path, int64_t timestamp);

static void cleanup_known_files(void) {
    static const char *names[] = {
        "live.ntpjournal", "live.ntpjournal.lock", "scan.ntpjournal",
        "scan.ntpjournal.lock", "claim.ntpjournal", "claim.ntpjournal.lock",
        "stale.ntpjournal.lock", "link.ntpjournal.lock", "link-target.txt",
        "live-owner.ntpjournal", "live-owner.ntpjournal.lock",
        "live-preserve.ntpjournal", "live-preserve.ntpjournal.lock",
        "live-degraded.ntpjournal.lock",
        "live-replace.ntpjournal", "live-replace.ntpjournal.lock",
        "candidate.ntpjournal", "candidate.ntpjournal.lock",
        "candidate-first.ntpacker_project", "candidate-second.ntpacker_project",
        "candidate-self-target.ntpjournal", "candidate-self-target.ntpjournal.lock",
        "candidate-stale.ntpjournal", "candidate-stale.ntpjournal.lock",
        "candidate-stale.ntpacker_project",
        "original.ntpjournal", "original.ntpjournal.lock",
        "original.ntpacker_project",
        "long-metadata.ntpjournal",
        "discard.ntpjournal", "discard.ntpjournal.lock",
        "version.ntpjournal", "version.ntpjournal.lock",
        "scan-target.ntpjournal", "scan-target.ntpjournal.lock",
        "scan-link.ntpjournal",
        "flow-live.ntpjournal", "flow-live.ntpjournal.lock",
        "flow-second.ntpjournal", "flow-second.ntpjournal.lock",
        "flow-scan.ntpjournal", "flow-scan.ntpjournal.lock",
        "flow-resolve.ntpjournal", "flow-resolve.ntpjournal.lock",
        "flow-resolve.ntpacker_project",
        "scan-valid-corrupt.ntpjournal", "scan-corrupt.ntpjournal",
        "scan-valid-unreadable.ntpjournal", "scan-unreadable.ntpjournal",
    };
    char path[TP_IDENTITY_PATH_MAX];
    for (size_t i = 0U; i < sizeof names / sizeof names[0]; ++i) {
        join_path(path, sizeof path, g_root, names[i]);
        (void)remove(path);
    }
    for (unsigned i = 0U; i < 257U; ++i) {
        char name[64];
        (void)snprintf(name, sizeof name, "budget-%03u.ntpjournal", i);
        join_path(path, sizeof path, g_root, name);
        (void)remove(path);
        (void)snprintf(name, sizeof name, "noise-%03u.tmp", i);
        join_path(path, sizeof path, g_root, name);
        (void)remove(path);
    }
}

#ifdef _WIN32
static bool win_utf8_path(const char *path, WCHAR *wide, size_t cap) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                               (int)cap) != 0;
}

static bool win_utf8_file_exists(const char *path) {
    WCHAR wide[TP_IDENTITY_PATH_MAX];
    return win_utf8_path(path, wide, TP_IDENTITY_PATH_MAX) &&
           GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
}

static void win_utf8_delete_file(const char *path) {
    WCHAR wide[TP_IDENTITY_PATH_MAX];
    if (win_utf8_path(path, wide, TP_IDENTITY_PATH_MAX)) {
        (void)DeleteFileW(wide);
    }
}

static void win_utf8_remove_dir(const char *path) {
    WCHAR wide[TP_IDENTITY_PATH_MAX];
    if (win_utf8_path(path, wide, TP_IDENTITY_PATH_MAX)) {
        (void)RemoveDirectoryW(wide);
    }
}

static bool win_utf8_make_dir(const char *path) {
    WCHAR wide[TP_IDENTITY_PATH_MAX];
    if (!win_utf8_path(path, wide, TP_IDENTITY_PATH_MAX)) {
        return false;
    }
    return CreateDirectoryW(wide, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}
#endif

void test_frontend_flow_attaches_scans_and_resolves_without_owner_handles(void) {
    char scan_path[TP_IDENTITY_PATH_MAX];
    char resolve_path[TP_IDENTITY_PATH_MAX];
    char target_path[TP_IDENTITY_PATH_MAX];
    join_path(scan_path, sizeof scan_path, g_root, "flow-scan.ntpjournal");
    join_path(resolve_path, sizeof resolve_path, g_root, "flow-resolve.ntpjournal");
    join_path(target_path, sizeof target_path, g_root,
              "flow-resolve.ntpacker_project");

    tp_session *session = NULL;
    uint8_t seed = 71U;
    tp_rng rng = {det_fill, &seed};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create_default_project(&rng, &session, &err));
    uint8_t domain_seed = 17U;
    const tp_rng domain_rng = {det_fill, &domain_seed};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_root_validate(g_root, recovery_key(),
                                                    &err));
    const tp_recovery_metadata metadata = {
        .timestamp = 200,
        .project_path = "",
        .project_name = "live-flow",
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_recovery_session_attach(g_root, recovery_key(), &domain_rng,
                                   session, &metadata, &err));
    TEST_ASSERT_TRUE(tp_session_recovery_available(session));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_recovery_session_attach(g_root, recovery_key(), &domain_rng,
                                   session, &metadata, &err));

    tp_recovery_store *fixture_store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(),
                                                   &fixture_store, &err));
    craft_orphan(scan_path, 201);
    make_preserved_live_journal(fixture_store, resolve_path, 202);
    tp_recovery_store_destroy(fixture_store);

    tp_recovery_candidates candidates;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_scan_root(g_root, recovery_key(), session,
                                                &candidates, &err));
    bool saw_scan = false;
    for (size_t i = 0U; i < candidates.count; ++i) {
        saw_scan = saw_scan || strstr(candidates.items[i].journal_path,
                                      "flow-scan.ntpjournal") != NULL;
    }
    TEST_ASSERT_TRUE(saw_scan);

    tp_recovery_resolve_result result;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_recovery_resolve_journal(
            g_root, recovery_key(), resolve_path, session,
            TP_RECOVERY_ACTION_SAVE_AS, target_path, &rng, &result, &err));
    TEST_ASSERT_TRUE(result.journal_deleted);
    TEST_ASSERT_TRUE(result.project_saved);
    TEST_ASSERT_TRUE(result.has_file_fingerprint);
    TEST_ASSERT_EQUAL_STRING(target_path, result.target_path);
    TEST_ASSERT_FALSE(file_exists(resolve_path));
    TEST_ASSERT_TRUE(file_exists(target_path));

    tp_session_destroy(session);
    (void)remove(scan_path);
    (void)remove(target_path);
}

void test_frontend_attach_failure_requires_recovery_before_storage_error(void) {
    tp_error err = {{0}};
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "flow-live.ntpjournal");
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(),
                                                   &store, &err));
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, &err));
    uint8_t seed = 81U;
    tp_rng rng = {det_fill, &seed};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create_default_project(&rng, &session, &err));
    const tp_recovery_metadata metadata = {
        .timestamp = 203,
        .project_path = "",
        .project_name = "busy-flow",
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_RECOVERY_BUSY,
        tp_recovery__test_session_attach_at(
            g_root, recovery_key(), journal, session, &metadata, &err));
    TEST_ASSERT_FALSE(tp_session_recovery_available(session));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    op.u.atlas_rename.name = "must-not-land";
    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "20000000000000000000000000000003", 33U);
    request.expected_revision = 0;
    request.ops = &op;
    request.op_count = 1U;
    tp_txn_result txn_result;
    memset(&txn_result, 0, sizeof txn_result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED,
                          tp_session_apply(session, &request, &txn_result, &err));
    tp_txn_result_free(&txn_result);
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
    tp_recovery_claim_release(claim);
    tp_recovery_store_destroy(store);
}

static int det_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *counter = (uint8_t *)ctx;
    for (size_t i = 0U; i < len; ++i) {
        out[i] = (uint8_t)(*counter + (uint8_t)i + 1U);
    }
    (*counter)++;
    return (int)len;
}

static tp_model *create_model(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    uint8_t counter = 1U;
    tp_rng rng = {det_fill, &counter};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_promote_ids(project, &rng, &err));
    tp_model *model = tp_model_wrap(project);
    TEST_ASSERT_NOT_NULL(model);
    return model;
}

static tp_status save_model_candidate_with_fingerprint(
    tp_model *model, const char *path, tp_id128 *out_fingerprint,
    tp_error *err) {
    tp_project *candidate = tp_project_clone(tp_model_project(model));
    if (!candidate) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "model save candidate clone failed");
    }
    const tp_status status = tp_project_save_candidate_with_fingerprint(
        candidate, path, NULL, false, out_fingerprint, err);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(candidate);
        return status;
    }
    tp_model__adopt_project(model, candidate);
    return TP_STATUS_OK;
}

static void commit_recovery_fixture_change(tp_model *model) {
    const tp_project *project = tp_model_project(model);
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_GREATER_THAN_INT(0, project->atlas_count);

    tp_operation op = {0};
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = project->atlases[0].id;
    op.u.atlas_rename.name = "recovery-fixture-dirty";
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "f1000000000000000000000000000001", 33U);
    request.expected_revision = tp_model_revision(model);
    request.ops = &op;
    request.op_count = 1;
    tp_txn_result result = {0};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_apply(model, &request, &result, &err));
    tp_txn_result_free(&result);
}

static void make_preserved_live_journal_with_metadata(
    tp_recovery_store *store, const char *journal,
    const tp_recovery_metadata *metadata) {
    tp_recovery_live *live = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &live, NULL));
    tp_model *model = create_model();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_attach(live, model, metadata, NULL));
    /* Recovery discovery deliberately ignores checkpoint-only clean sessions.
     * Persist one real transaction so this fixture represents unsaved work. */
    commit_recovery_fixture_change(model);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_finish(live, true, NULL));
    tp_recovery_live_destroy(live);
    tp_model_destroy(model);
}

static void make_preserved_live_journal(tp_recovery_store *store,
                                        const char *journal, int64_t timestamp) {
    const tp_recovery_metadata metadata = {
        .timestamp = timestamp,
        .project_path = "",
        .project_name = "candidate",
    };
    make_preserved_live_journal_with_metadata(store, journal, &metadata);
}

static void craft_orphan(const char *path, int64_t timestamp) {
    tp_journal_io io = tp_journal_io_file(path);
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, recovery_key());
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {{0}};
    static const uint8_t snapshot[] = "{}";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(journal, snapshot,
                                                     sizeof snapshot - 1U, 0, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(journal,
                                                "a1000000000000000000000000000001", 1,
                                                snapshot, sizeof snapshot - 1U, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(journal, timestamp, "", "orphan", &err));
    tp_journal_destroy(journal);
}

static int child_claim_exit(const char *path, bool leak_claim) {
    char leak_text[2] = {leak_claim ? '1' : '0', '\0'};
#ifdef _WIN32
    return (int)_spawnl(_P_WAIT, g_self, g_self, "--child-claim", g_root, path,
                        leak_text, NULL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        (void)execl(g_self, g_self, "--child-claim", g_root, path, leak_text,
                    (char *)NULL);
        _exit(127);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, (int)pid);
    int status = 0;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    return WEXITSTATUS(status);
#endif
}

static int child_project_lease_exit(const char *path) {
#ifdef _WIN32
    return (int)_spawnl(_P_WAIT, g_self, g_self, "--child-project-lease", path,
                        NULL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        (void)execl(g_self, g_self, "--child-project-lease", path, (char *)NULL);
        _exit(127);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, (int)pid);
    int status = 0;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    return WEXITSTATUS(status);
#endif
}

void test_live_slot_competition_and_permanent_lock(void) {
    char slot[TP_IDENTITY_PATH_MAX];
    char lock[TP_IDENTITY_PATH_MAX];
    join_path(slot, sizeof slot, g_root, "live.ntpjournal");
    (void)snprintf(lock, sizeof lock, "%s.lock", slot);
    (void)remove(slot);
    (void)remove(lock);

    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, slot, &claim, NULL));
    TEST_ASSERT_EQUAL_INT(23, child_claim_exit(slot, false));
    tp_recovery_claim_release(claim);
    TEST_ASSERT_TRUE(file_exists(lock));
    TEST_ASSERT_EQUAL_INT(0, child_claim_exit(slot, false));
    TEST_ASSERT_TRUE(file_exists(lock));
    tp_recovery_store_destroy(store);
}

void test_scan_skips_live_then_lists_dead_owner(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "scan.ntpjournal");
    craft_orphan(journal, 42);
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, NULL));
    tp_recovery_candidates candidates;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_scan(store, NULL, &candidates, NULL));
    bool found = false;
    for (size_t i = 0U; i < candidates.count; ++i) {
        if (strstr(candidates.items[i].journal_path, "scan.ntpjournal") != NULL) {
            found = true;
        }
    }
    TEST_ASSERT_FALSE(found);
    tp_recovery_claim_release(claim);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_scan(store, NULL, &candidates, NULL));
    found = false;
    for (size_t i = 0U; i < candidates.count; ++i) {
        if (strstr(candidates.items[i].journal_path, "scan.ntpjournal") != NULL) {
            found = candidates.items[i].adoptable && candidates.items[i].timestamp == 42;
        }
    }
    TEST_ASSERT_TRUE(found);
    tp_recovery_store_destroy(store);
}

void test_competing_orphan_claim_and_process_death(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "claim.ntpjournal");
    craft_orphan(journal, 7);
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, NULL));
    TEST_ASSERT_EQUAL_INT(23, child_claim_exit(journal, false));
    tp_recovery_claim_release(claim);
    TEST_ASSERT_EQUAL_INT(0, child_claim_exit(journal, true));
    claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, NULL));
    tp_recovery_claim_release(claim);
    tp_recovery_store_destroy(store);
}

void test_stale_lock_is_reused_not_recreated(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    char lock[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "stale.ntpjournal");
    (void)snprintf(lock, sizeof lock, "%s.lock", journal);
    write_file(lock, "sentinel");
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, NULL));
    tp_recovery_claim_release(claim);
    char content[16] = {0};
    FILE *file = fopen(lock, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_UINT64(8, fread(content, 1U, sizeof content, file));
    (void)fclose(file);
    TEST_ASSERT_EQUAL_STRING("sentinel", content);
    tp_recovery_store_destroy(store);
}

void test_live_owner_attaches_updates_and_cleanly_closes(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    char lock[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "live-owner.ntpjournal");
    (void)snprintf(lock, sizeof lock, "%s.lock", journal);
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_live *live = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &live, NULL));
    TEST_ASSERT_NOT_NULL(live);
    TEST_ASSERT_EQUAL_STRING(journal, tp_recovery_live_journal_path(live));

    tp_recovery_live *competitor = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_RECOVERY_BUSY,
                          tp_recovery_store_create_live(store, journal, &competitor, NULL));
    TEST_ASSERT_NULL(competitor);

    tp_model *model = create_model();
    tp_recovery_metadata metadata = {
        .timestamp = 10,
        .project_path = "C:/project/first.ntpacker_project",
        .project_name = "first",
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_attach(live, model, &metadata, NULL));
    TEST_ASSERT_TRUE(tp_recovery_live_healthy(live));
    TEST_ASSERT_TRUE(tp_model_has_journal(model));

    metadata.timestamp = 20;
    metadata.project_path = "C:/project/second.ntpacker_project";
    metadata.project_name = "second";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_update_metadata(live, &metadata, NULL));
    tp_journal_peek_result peek;
    memset(&peek, 0, sizeof peek);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_peek(tp_journal_io_file_read(journal), &peek, NULL));
    TEST_ASSERT_TRUE(peek.has_meta);
    TEST_ASSERT_EQUAL_INT64(20, peek.meta.timestamp);
    TEST_ASSERT_EQUAL_STRING("C:/project/second.ntpacker_project", peek.meta.path);
    TEST_ASSERT_EQUAL_STRING("second", peek.meta.name);
    tp_journal_peek_free(&peek);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_finish(live, false, NULL));
    TEST_ASSERT_FALSE(tp_model_has_journal(model));
    TEST_ASSERT_FALSE(file_exists(journal));
    TEST_ASSERT_TRUE(file_exists(lock));
    tp_recovery_live_destroy(live);
    tp_model_destroy(model);
    tp_recovery_store_destroy(store);
}

#ifdef _WIN32
void test_live_owner_clean_close_uses_unicode_handle_delete(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root,
              "\xD0\xB6\xD0\xB8\xD0\xB2\xD0\xBE\xD0\xB9.ntpjournal");
    tp_recovery_store *store = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(),
                                                   &store, &err));
    tp_recovery_live *live = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &live,
                                                        &err));
    tp_model *model = create_model();
    const tp_recovery_metadata metadata = {
        .timestamp = 21,
        .project_path = "",
        .project_name = "unicode-live",
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_attach(live, model, &metadata, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_finish(live, false, &err));
    tp_recovery_live_destroy(live);
    tp_model_destroy(model);
    tp_recovery_store_destroy(store);
}
#endif

void test_live_owner_preserves_dirty_journal_and_releases_liveness(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "live-preserve.ntpjournal");
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_live *live = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &live, NULL));
    tp_model *model = create_model();
    tp_recovery_metadata metadata = {
        .timestamp = 30,
        .project_path = "",
        .project_name = "dirty",
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_attach(live, model, &metadata, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_finish(live, true, NULL));
    TEST_ASSERT_TRUE(tp_scan_exists(journal));
    TEST_ASSERT_FALSE(tp_model_has_journal(model));

    tp_recovery_live *next = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &next, NULL));
    tp_recovery_live_destroy(next);
    tp_recovery_live_destroy(live);
    tp_model_destroy(model);
    tp_recovery_store_destroy(store);
}

void test_live_attach_never_replaces_an_existing_orphan(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "live-existing.ntpjournal");
    char lock[TP_IDENTITY_PATH_MAX];
    (void)snprintf(lock, sizeof lock, "%s.lock", journal);
    (void)remove(journal);
    (void)remove(lock);

    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(),
                                                   &store, NULL));
    make_preserved_live_journal(store, journal, 31);
    tp_id128 before;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_identity_file_fingerprint(journal, &before, NULL));

    tp_recovery_live *next = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &next,
                                                        NULL));
    tp_model *model = create_model();
    const tp_recovery_metadata metadata = {
        .timestamp = 32,
        .project_path = "",
        .project_name = "must-not-replace",
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_RECOVERY_BUSY,
                          tp_recovery_live_attach(next, model, &metadata, NULL));
    tp_id128 after;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_identity_file_fingerprint(journal, &after, NULL));
    TEST_ASSERT_TRUE(tp_id128_eq(before, after));
    TEST_ASSERT_FALSE(tp_model_has_journal(model));

    tp_recovery_live_destroy(next);
    tp_model_destroy(model);
    tp_recovery_store_destroy(store);
    (void)remove(journal);
    (void)remove(lock);
}

#ifdef _WIN32
void test_unicode_root_and_orphan_scan_resolve_end_to_end(void) {
    char root[TP_IDENTITY_PATH_MAX];
    char lock[TP_IDENTITY_PATH_MAX];
    (void)snprintf(root, sizeof root, "%s/%s", g_root,
                   "\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xBB\xD0\xB8\xD1\x89\xD0\xB5");
    win_utf8_remove_dir(root);
    TEST_ASSERT_TRUE(win_utf8_make_dir(root));

    uint8_t counter = 41U;
    tp_rng rng = {det_fill, &counter};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create_default_project(&rng, &session,
                                                            &err));
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, root, "unicode-live.ntpjournal");
    (void)snprintf(lock, sizeof lock, "%s.lock", journal);
    const tp_recovery_metadata metadata = {
        .timestamp = 33,
        .project_path = "",
        .project_name = "unicode-orphan",
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery__test_session_attach_at(
                              root, recovery_key(), journal, session,
                              &metadata, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    tp_operation op = {0};
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = tp_session_snapshot_atlas_at(snapshot, 0)->id;
    op.u.atlas_rename.name = "unicode-dirty";
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "33000000000000000000000000000001", 33U);
    request.expected_revision = 0;
    request.ops = &op;
    request.op_count = 1U;
    tp_txn_result transaction = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_apply(session, &request, &transaction,
                                           &err));
    tp_txn_result_free(&transaction);
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
    TEST_ASSERT_TRUE(win_utf8_file_exists(journal));

    tp_recovery_candidates candidates = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_scan_root(root, recovery_key(), NULL,
                                                &candidates, &err));
    TEST_ASSERT_EQUAL_size_t(1U, candidates.count);
    TEST_ASSERT_EQUAL_STRING(journal, candidates.items[0].journal_path);
    TEST_ASSERT_TRUE(candidates.items[0].adoptable);

    tp_recovery_resolve_result resolved = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_resolve_journal(
                              root, recovery_key(), journal, NULL,
                              TP_RECOVERY_ACTION_DISCARD, NULL, NULL,
                              &resolved, &err));
    TEST_ASSERT_TRUE(resolved.journal_deleted);
    TEST_ASSERT_FALSE(win_utf8_file_exists(journal));

    win_utf8_delete_file(lock);
    win_utf8_remove_dir(root);
}
#endif

void test_live_owner_degradation_preserves_stale_slot(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "live-degraded.ntpjournal");
    (void)remove(journal);
    tp_mkdirs(journal);
    TEST_ASSERT_TRUE(tp_scan_is_dir(journal));
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_live *live = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &live, NULL));
    tp_model *model = create_model();
    tp_recovery_metadata metadata = {
        .timestamp = 40,
        .project_path = "",
        .project_name = "degraded",
    };
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK,
                          tp_recovery_live_attach(live, model, &metadata, NULL));
    TEST_ASSERT_FALSE(tp_recovery_live_healthy(live));
    TEST_ASSERT_FALSE(tp_model_has_journal(model));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_finish(live, false, NULL));
    TEST_ASSERT_TRUE(tp_scan_is_dir(journal));
    tp_recovery_live_destroy(live);
    tp_model_destroy(model);
    tp_recovery_store_destroy(store);
#ifdef _WIN32
    TEST_ASSERT_EQUAL_INT(0, _rmdir(journal));
#else
    TEST_ASSERT_EQUAL_INT(0, rmdir(journal));
#endif
}

void test_live_owner_never_deletes_a_replacement_path(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "live-replace.ntpjournal");
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_live *live = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &live, NULL));
    tp_model *model = create_model();
    tp_recovery_metadata metadata = {
        .timestamp = 50,
        .project_path = "",
        .project_name = "replacement",
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_attach(live, model, &metadata, NULL));

    if (remove(journal) == 0) {
        /* POSIX permits unlinking an open inode. The model keeps writing its
         * pinned inode while this path now names unrelated bytes. */
        write_file(journal, "replacement");
        TEST_ASSERT_EQUAL_INT(TP_STATUS_RECOVERY_CLEANUP_FAILED,
                              tp_recovery_live_finish(live, false, NULL));
        char content[16] = {0};
        FILE *file = fopen(journal, "rb");
        TEST_ASSERT_NOT_NULL(file);
        TEST_ASSERT_EQUAL_UINT64(11, fread(content, 1U, sizeof content, file));
        (void)fclose(file);
        TEST_ASSERT_EQUAL_STRING("replacement", content);
    } else {
        /* Windows pins the path against replacement while the model owns the
         * journal handle; after detach the verified original can be removed. */
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_recovery_live_finish(live, false, NULL));
        TEST_ASSERT_FALSE(file_exists(journal));
    }
    tp_recovery_live_destroy(live);
    tp_model_destroy(model);
    tp_recovery_store_destroy(store);
}

#ifndef _WIN32
void test_failed_live_cleanup_keeps_journal_discoverable_after_restart(void) {
    char root[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(create_isolated_root(root, sizeof root, "cleanup-fail"));
    char journal[TP_IDENTITY_PATH_MAX];
    char lock[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, root, "live-cleanup-fail.ntpjournal");
    (void)snprintf(lock, sizeof lock, "%s.lock", journal);
    (void)remove(journal);
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(root, recovery_key(), &store, NULL));
    tp_recovery_live *live = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create_live(store, journal, &live, NULL));
    tp_model *model = create_model();
    tp_recovery_metadata metadata = {
        .timestamp = 55,
        .project_path = "",
        .project_name = "cleanup-failure",
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_live_attach(live, model, &metadata, NULL));

    commit_recovery_fixture_change(model);

    tp_recovery__test_fail_next_quarantine_unlink();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_RECOVERY_CLEANUP_FAILED,
                          tp_recovery_live_retire(live, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_RECOVERY_CLEANUP_FAILED,
                          tp_recovery_live_retire(live, NULL));
    tp_recovery_live_destroy(live);
    tp_model_destroy(model);
    tp_recovery_store_destroy(store);

    /* Simulate restart: a fresh store must still discover the only durable
     * journal, whether cleanup restored its public name or retained the
     * scan-visible quarantine fallback. */
    store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(root, recovery_key(), &store, NULL));
    tp_recovery_candidates candidates;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_scan(store, NULL, &candidates, NULL));
    const size_t candidate_count = candidates.count;
    const bool metadata_matches =
        candidate_count == 1U &&
        strcmp(candidates.items[0].name, "cleanup-failure") == 0;
    char discovered[TP_IDENTITY_PATH_MAX] = {0};
    if (candidate_count == 1U) {
        (void)snprintf(discovered, sizeof discovered, "%s",
                       candidates.items[0].journal_path);
    }
    tp_recovery_store_destroy(store);
    (void)remove(journal);
    if (discovered[0] != '\0') {
        (void)remove(discovered);
    }
    (void)remove(lock);
    TEST_ASSERT_EQUAL_INT(0, rmdir(root));
    TEST_ASSERT_EQUAL_size_t(1U, candidate_count);
    TEST_ASSERT_TRUE(metadata_matches);
}
#endif

void test_claimed_candidate_requires_bound_current_save_receipt(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    char first_target[TP_IDENTITY_PATH_MAX];
    char second_target[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "candidate.ntpjournal");
    join_path(first_target, sizeof first_target, g_root,
              "candidate-first.ntpacker_project");
    join_path(second_target, sizeof second_target, g_root,
              "candidate-second.ntpacker_project");
    (void)remove(journal);
    (void)remove(first_target);
    (void)remove(second_target);
    tp_recovery_store *store = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, &err));
    make_preserved_live_journal(store, journal, 90);
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, &err));
    tp_recovery_owned_candidate *candidate = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_claim_recover(claim, &candidate, &err));
    TEST_ASSERT_NOT_NULL(candidate);
    uint8_t seed = 9U;
    tp_rng rng = {det_fill, &seed};
    tp_recovery_resolution *resolution = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_candidate_create_resolution(
                              candidate, &rng, &resolution, &err));
    tp_session_save_result receipt;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_resolution_save_as(
                              resolution, first_target, &receipt, &err));
    TEST_ASSERT_TRUE(receipt.saved);
    TEST_ASSERT_TRUE(receipt.has_recovery_token);
    write_file(first_target, "replacement");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_FILE_CHANGED_EXTERNALLY,
                          tp_recovery_resolution_finalize(resolution, &receipt, &err));
    TEST_ASSERT_TRUE(tp_scan_exists(journal));
    tp_recovery_resolution_destroy(resolution);

    resolution = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_candidate_create_resolution(
                              candidate, &rng, &resolution, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_resolution_save_as(
                              resolution, second_target, &receipt, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_resolution_finalize(resolution, &receipt, &err));
    TEST_ASSERT_FALSE(tp_scan_exists(journal));
    tp_recovery_resolution_destroy(resolution);
    tp_recovery_claim_release(claim);
    tp_recovery_store_destroy(store);
    (void)remove(first_target);
    (void)remove(second_target);
}

void test_resolution_refuses_save_over_pinned_journal_before_write(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root,
              "candidate-self-target.ntpjournal");
    (void)remove(journal);
    tp_recovery_store *store = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, &err));
    make_preserved_live_journal(store, journal, 93);
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, &err));
    tp_recovery_owned_candidate *candidate = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_claim_recover(claim, &candidate, &err));
    uint8_t seed = 11U;
    tp_rng rng = {det_fill, &seed};
    tp_recovery_resolution *resolution = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_candidate_create_resolution(
                              candidate, &rng, &resolution, &err));
    tp_session_save_result receipt;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_recovery_resolution_save_as(
                              resolution, journal, &receipt, &err));
    TEST_ASSERT_TRUE(tp_scan_exists(journal));

    tp_recovery_resolution_cancel(resolution);
    tp_recovery_resolution_destroy(resolution);
    tp_recovery_claim_release(claim);
    tp_recovery_store_destroy(store);
}

void test_resolution_cancel_invalidates_receipt_and_releases_process_lease(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    char target[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "candidate-stale.ntpjournal");
    join_path(target, sizeof target, g_root,
              "candidate-stale.ntpacker_project");
    (void)remove(journal);
    (void)remove(target);
    tp_recovery_store *store = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, &err));
    make_preserved_live_journal(store, journal, 91);
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, &err));
    tp_recovery_owned_candidate *candidate = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_claim_recover(claim, &candidate, &err));
    uint8_t seed = 10U;
    tp_rng rng = {det_fill, &seed};
    tp_recovery_resolution *resolution = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_candidate_create_resolution(
                              candidate, &rng, &resolution, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_RECOVERY_BUSY,
                          tp_recovery_claim_discard(claim, &err));
    TEST_ASSERT_TRUE(tp_scan_exists(journal));
    tp_session_save_result copied_receipt;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_resolution_save_as(
                              resolution, target, &copied_receipt, &err));
    TEST_ASSERT_EQUAL_INT(23, child_project_lease_exit(target));

    tp_recovery_resolution_cancel(resolution);
    TEST_ASSERT_EQUAL_INT(0, child_project_lease_exit(target));
    tp_project_lease *competitor = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(target, &competitor, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_recovery_resolution_finalize(
                              resolution, &copied_receipt, &err));
    TEST_ASSERT_TRUE(tp_scan_exists(journal));
    tp_project_lease_release(competitor);
    tp_recovery_resolution_destroy(resolution);
    tp_recovery_claim_release(claim);
    tp_recovery_store_destroy(store);
    (void)remove(target);
}

void test_save_original_requires_lease_and_exact_fingerprint(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    char original[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "original.ntpjournal");
    join_path(original, sizeof original, g_root, "original.ntpacker_project");
    (void)remove(journal);
    (void)remove(original);
    tp_model *original_model = create_model();
    tp_id128 baseline;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          save_model_candidate_with_fingerprint(
                              original_model, original, &baseline, &err));

    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, &err));
    const tp_recovery_metadata metadata = {
        .timestamp = 92,
        .project_path = original,
        .project_name = "original",
        .file_fingerprint = &baseline,
    };
    make_preserved_live_journal_with_metadata(store, journal, &metadata);
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, &err));
    tp_recovery_owned_candidate *candidate = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_claim_recover(claim, &candidate, &err));
    uint8_t seed = 11U;
    tp_rng rng = {det_fill, &seed};
    tp_recovery_resolution *resolution = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_candidate_create_resolution(
                              candidate, &rng, &resolution, &err));
    tp_session_save_result receipt;
    tp_project_lease *competitor = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(original, &competitor, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_PROJECT_LIVE,
                          tp_recovery_resolution_save_original(
                              resolution, &receipt, &err));
    tp_project_lease_release(competitor);

    write_file(original, "changed");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_FILE_CHANGED_EXTERNALLY,
                          tp_recovery_resolution_save_original(
                              resolution, &receipt, &err));
    TEST_ASSERT_TRUE(tp_scan_exists(journal));

    tp_id128 restored;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          save_model_candidate_with_fingerprint(
                              original_model, original, &restored, &err));
    TEST_ASSERT_TRUE(tp_id128_eq(baseline, restored));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_resolution_save_original(
                              resolution, &receipt, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_resolution_finalize(
                              resolution, &receipt, &err));
    TEST_ASSERT_FALSE(tp_scan_exists(journal));
    tp_recovery_resolution_destroy(resolution);
    tp_recovery_claim_release(claim);
    tp_recovery_store_destroy(store);
    tp_model_destroy(original_model);
    (void)remove(original);
}

void test_claim_discard_deletes_only_journal_and_keeps_lock_domain(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    char lock[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "discard.ntpjournal");
    (void)snprintf(lock, sizeof lock, "%s.lock", journal);
    (void)remove(journal);
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    make_preserved_live_journal(store, journal, 100);
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_claim_discard(claim, NULL));
    TEST_ASSERT_FALSE(tp_scan_exists(journal));
    TEST_ASSERT_TRUE(tp_scan_exists(lock));
    tp_recovery_claim_release(claim);
    tp_recovery_store_destroy(store);
}

void test_scan_surfaces_only_same_key_version_mismatch(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "version.ntpjournal");
    (void)remove(journal);
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    make_preserved_live_journal(store, journal, 110);
    FILE *file = fopen(journal, "r+b");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, TP_JRN_MAGIC_LEN, SEEK_SET));
    uint8_t version[4];
    tp_jrn_put_u32(version, (uint32_t)TP_JOURNAL_FORMAT_VERSION + 1U);
    TEST_ASSERT_EQUAL_UINT64(sizeof version,
                             fwrite(version, 1U, sizeof version, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    tp_recovery_candidates candidates;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_scan(store, NULL, &candidates, NULL));
    bool mismatch_found = false;
    for (size_t i = 0U; i < candidates.count; ++i) {
        mismatch_found = mismatch_found ||
                         (strstr(candidates.items[i].journal_path,
                                 "version.ntpjournal") != NULL &&
                          candidates.items[i].status ==
                              TP_JOURNAL_RECOVERY_VERSION_MISMATCH &&
                          !candidates.items[i].adoptable);
    }
    TEST_ASSERT_TRUE(mismatch_found);

    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_claim_discard(claim, NULL));
    TEST_ASSERT_FALSE(tp_scan_exists(journal));
    tp_recovery_claim_release(claim);

    make_preserved_live_journal(store, journal, 111);
    file = fopen(journal, "r+b");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, TP_JRN_MAGIC_LEN, SEEK_SET));
    TEST_ASSERT_EQUAL_UINT64(sizeof version,
                             fwrite(version, 1U, sizeof version, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    file = fopen(journal, "r+b");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, TP_JRN_KEY_OFF, SEEK_SET));
    const uint8_t foreign = 0xFFU;
    TEST_ASSERT_EQUAL_UINT64(1, fwrite(&foreign, 1U, 1U, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_scan(store, NULL, &candidates, NULL));
    for (size_t i = 0U; i < candidates.count; ++i) {
        TEST_ASSERT_NULL(strstr(candidates.items[i].journal_path,
                                "version.ntpjournal"));
    }
    claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_claim(store, journal, &claim, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          tp_recovery_claim_discard(claim, NULL));
    TEST_ASSERT_TRUE(tp_scan_exists(journal));
    tp_recovery_claim_release(claim);
    tp_recovery_store_destroy(store);
}

void test_scan_reports_corrupt_journal_without_hiding_valid_candidate(void) {
    char valid[TP_IDENTITY_PATH_MAX];
    char corrupt[TP_IDENTITY_PATH_MAX];
    join_path(valid, sizeof valid, g_root, "scan-valid-corrupt.ntpjournal");
    join_path(corrupt, sizeof corrupt, g_root, "scan-corrupt.ntpjournal");
    (void)remove(valid);
    (void)remove(corrupt);
    craft_orphan(valid, 113);
    write_file(corrupt, "not a recovery journal");

    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_candidates candidates;
    tp_error err = {{0}};
    const tp_status scan_status =
        tp_recovery_store_scan(store, NULL, &candidates, &err);

    bool valid_found = false;
    for (size_t i = 0U; i < candidates.count; ++i) {
        valid_found = valid_found ||
                      strstr(candidates.items[i].journal_path,
                             "scan-valid-corrupt.ntpjournal") != NULL;
    }
    bool diagnostic_found = false;
    for (size_t i = 0U; i < candidates.diagnostic_count; ++i) {
        diagnostic_found = diagnostic_found ||
                           (strstr(candidates.diagnostics[i].journal_path,
                                   "scan-corrupt.ntpjournal") != NULL &&
                            candidates.diagnostics[i].status ==
                                TP_STATUS_BAD_PROJECT);
    }

    tp_recovery_store_destroy(store);
    (void)remove(valid);
    (void)remove(corrupt);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, scan_status, err.msg);
    TEST_ASSERT_TRUE(valid_found);
    TEST_ASSERT_TRUE(diagnostic_found);
}

void test_scan_reports_unreadable_entry_without_hiding_valid_candidate(void) {
    char valid[TP_IDENTITY_PATH_MAX];
    char unreadable[TP_IDENTITY_PATH_MAX];
    join_path(valid, sizeof valid, g_root, "scan-valid-unreadable.ntpjournal");
    join_path(unreadable, sizeof unreadable, g_root,
              "scan-unreadable.ntpjournal");
    (void)remove(valid);
    (void)remove(unreadable);
    craft_orphan(valid, 114);
#ifdef _WIN32
    TEST_ASSERT_EQUAL_INT(0, _mkdir(unreadable));
#else
    TEST_ASSERT_EQUAL_INT(0, mkdir(unreadable, 0700));
#endif

    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_candidates candidates;
    tp_error err = {{0}};
    const tp_status scan_status =
        tp_recovery_store_scan(store, NULL, &candidates, &err);

    bool valid_found = false;
    for (size_t i = 0U; i < candidates.count; ++i) {
        valid_found = valid_found ||
                      strstr(candidates.items[i].journal_path,
                             "scan-valid-unreadable.ntpjournal") != NULL;
    }
    bool diagnostic_found = false;
    for (size_t i = 0U; i < candidates.diagnostic_count; ++i) {
        diagnostic_found = diagnostic_found ||
                           (strstr(candidates.diagnostics[i].journal_path,
                                   "scan-unreadable.ntpjournal") != NULL &&
                            candidates.diagnostics[i].status ==
                                TP_STATUS_PATH_RESOLVE_FAILED);
    }

    tp_recovery_store_destroy(store);
    (void)remove(valid);
#ifdef _WIN32
    (void)_rmdir(unreadable);
#else
    (void)rmdir(unreadable);
#endif
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, scan_status, err.msg);
    TEST_ASSERT_TRUE(valid_found);
    TEST_ASSERT_TRUE(diagnostic_found);
}

void test_candidate_preserves_project_path_beyond_legacy_gui_capacity(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "long-metadata.ntpjournal");
    char original_path[1501];
    memset(original_path, 'p', sizeof original_path - 1U);
    original_path[0] = 'C';
    original_path[1] = ':';
    original_path[2] = '/';
    original_path[sizeof original_path - 1U] = '\0';

    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_recovery__test_craft_metadata_journal(
            journal, recovery_key(), 112, original_path, "long-path", &err));
    tp_recovery_candidate candidate;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_recovery__test_peek_candidate(journal, &candidate, &err));
    TEST_ASSERT_EQUAL_STRING(original_path, candidate.original_path);

    (void)remove(journal);
}

#ifndef _WIN32
void test_scan_never_follows_journal_symlink(void) {
    char root[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(create_isolated_root(root, sizeof root, "symlink-scan"));
    char target[TP_IDENTITY_PATH_MAX];
    char link[TP_IDENTITY_PATH_MAX];
    char target_lock[TP_IDENTITY_PATH_MAX];
    join_path(target, sizeof target, root, "scan-target.ntpjournal");
    join_path(link, sizeof link, root, "scan-link.ntpjournal");
    (void)snprintf(target_lock, sizeof target_lock, "%s.lock", target);
    (void)unlink(link);
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(root, recovery_key(), &store, NULL));
    make_preserved_live_journal(store, target, 120);
    TEST_ASSERT_EQUAL_INT(0, symlink(target, link));
    tp_recovery_candidates candidates;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_scan(store, NULL, &candidates, NULL));
    bool target_found = false;
    bool link_found = false;
    for (size_t i = 0U; i < candidates.count; ++i) {
        link_found = link_found ||
                     strstr(candidates.items[i].journal_path,
                            "scan-link.ntpjournal") != NULL;
        target_found = target_found ||
                       strstr(candidates.items[i].journal_path,
                              "scan-target.ntpjournal") != NULL;
    }
    tp_recovery_store_destroy(store);
    (void)unlink(link);
    (void)remove(target);
    (void)remove(target_lock);
    TEST_ASSERT_EQUAL_INT(0, rmdir(root));
    TEST_ASSERT_FALSE(link_found);
    TEST_ASSERT_TRUE(target_found);
}
#endif

void test_scan_stops_at_file_budget_and_reports_more(void) {
    char path[TP_IDENTITY_PATH_MAX];
    for (unsigned i = 0U; i < 257U; ++i) {
        char name[64];
        (void)snprintf(name, sizeof name, "budget-%03u.ntpjournal", i);
        join_path(path, sizeof path, g_root, name);
        write_file(path, "foreign");
    }
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_candidates candidates;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_scan(store, NULL, &candidates, NULL));
    TEST_ASSERT_TRUE(candidates.has_more);
    tp_recovery_store_destroy(store);
}

void test_scan_budget_counts_non_journal_entries(void) {
    cleanup_known_files();
    char path[TP_IDENTITY_PATH_MAX];
    for (unsigned i = 0U; i < 257U; ++i) {
        char name[64];
        (void)snprintf(name, sizeof name, "noise-%03u.tmp", i);
        join_path(path, sizeof path, g_root, name);
        write_file(path, "unrelated");
    }
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_candidates candidates;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_scan(store, NULL, &candidates, NULL));
    TEST_ASSERT_EQUAL_size_t(0U, candidates.count);
    TEST_ASSERT_TRUE(candidates.has_more);
    tp_recovery_store_destroy(store);
    cleanup_known_files();
}

#ifndef _WIN32
void test_claim_rejects_lock_symlink_without_touching_target(void) {
    char journal[TP_IDENTITY_PATH_MAX];
    char lock[TP_IDENTITY_PATH_MAX];
    char target[TP_IDENTITY_PATH_MAX];
    join_path(journal, sizeof journal, g_root, "link.ntpjournal");
    join_path(target, sizeof target, g_root, "link-target.txt");
    (void)snprintf(lock, sizeof lock, "%s.lock", journal);
    write_file(target, "target");
    (void)unlink(lock);
    TEST_ASSERT_EQUAL_INT(0, symlink(target, lock));
    tp_recovery_store *store = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_recovery_store_create(g_root, recovery_key(), &store, NULL));
    tp_recovery_claim *claim = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_RECOVERY_CLAIM_FAILED,
                          tp_recovery_store_claim(store, journal, &claim, NULL));
    TEST_ASSERT_NULL(claim);
    TEST_ASSERT_TRUE(file_exists(target));
    tp_recovery_store_destroy(store);
}
#endif

static int child_main(const char *root, const char *path, bool leak_claim) {
    tp_recovery_store *store = NULL;
    if (tp_recovery_store_create(root, recovery_key(), &store, NULL) != TP_STATUS_OK) {
        return 24;
    }
    tp_recovery_claim *claim = NULL;
    tp_status status = tp_recovery_store_claim(store, path, &claim, NULL);
    if (status == TP_STATUS_RECOVERY_BUSY) {
        tp_recovery_store_destroy(store);
        return 23;
    }
    if (status != TP_STATUS_OK) {
        tp_recovery_store_destroy(store);
        return 24;
    }
    if (!leak_claim) {
        tp_recovery_claim_release(claim);
        tp_recovery_store_destroy(store);
    }
    return 0;
}

static int child_project_lease_main(const char *path) {
    tp_project_lease *lease = NULL;
    const tp_status status = tp_project_lease_acquire(path, &lease, NULL);
    if (status == TP_STATUS_PROJECT_LIVE) {
        return 23;
    }
    if (status != TP_STATUS_OK) {
        return 24;
    }
    tp_project_lease_release(lease);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "--child-claim") == 0) {
        return child_main(argv[2], argv[3], strcmp(argv[4], "1") == 0);
    }
    if (argc == 3 && strcmp(argv[1], "--child-project-lease") == 0) {
        return child_project_lease_main(argv[2]);
    }
    if (argc != 2) {
        (void)fprintf(stderr, "usage: test_recovery_store <scratch-dir>\n");
        return 2;
    }
    g_self = argv[0];
    g_root = argv[1];
    tp_mkdirs(g_root);
    TEST_ASSERT_TRUE(tp_scan_is_dir(g_root));
    cleanup_known_files();
    UNITY_BEGIN();
    RUN_TEST(test_live_slot_competition_and_permanent_lock);
    RUN_TEST(test_frontend_flow_attaches_scans_and_resolves_without_owner_handles);
    RUN_TEST(test_frontend_attach_failure_requires_recovery_before_storage_error);
    RUN_TEST(test_scan_skips_live_then_lists_dead_owner);
    RUN_TEST(test_competing_orphan_claim_and_process_death);
    RUN_TEST(test_stale_lock_is_reused_not_recreated);
    RUN_TEST(test_live_owner_attaches_updates_and_cleanly_closes);
#ifdef _WIN32
    RUN_TEST(test_live_owner_clean_close_uses_unicode_handle_delete);
#endif
    RUN_TEST(test_live_owner_preserves_dirty_journal_and_releases_liveness);
    RUN_TEST(test_live_attach_never_replaces_an_existing_orphan);
#ifdef _WIN32
    RUN_TEST(test_unicode_root_and_orphan_scan_resolve_end_to_end);
#endif
    RUN_TEST(test_live_owner_degradation_preserves_stale_slot);
    RUN_TEST(test_live_owner_never_deletes_a_replacement_path);
#ifndef _WIN32
    RUN_TEST(test_failed_live_cleanup_keeps_journal_discoverable_after_restart);
#endif
    RUN_TEST(test_claimed_candidate_requires_bound_current_save_receipt);
    RUN_TEST(test_resolution_refuses_save_over_pinned_journal_before_write);
    RUN_TEST(test_resolution_cancel_invalidates_receipt_and_releases_process_lease);
    RUN_TEST(test_save_original_requires_lease_and_exact_fingerprint);
    RUN_TEST(test_claim_discard_deletes_only_journal_and_keeps_lock_domain);
    RUN_TEST(test_scan_surfaces_only_same_key_version_mismatch);
    RUN_TEST(test_scan_reports_corrupt_journal_without_hiding_valid_candidate);
    RUN_TEST(test_scan_reports_unreadable_entry_without_hiding_valid_candidate);
    RUN_TEST(test_candidate_preserves_project_path_beyond_legacy_gui_capacity);
#ifndef _WIN32
    RUN_TEST(test_scan_never_follows_journal_symlink);
#endif
    RUN_TEST(test_scan_budget_counts_non_journal_entries);
    RUN_TEST(test_scan_stops_at_file_budget_and_reports_more);
#ifndef _WIN32
    RUN_TEST(test_claim_rejects_lock_symlink_without_touching_target);
#endif
    return UNITY_END();
}
