/* Export output-SET publication contract (tp_core/tp_export.h).
 *
 * Drives tp_export_write_and_publish_set directly over a fixture exporter: none
 * of the properties below needs a real pack, and the publish seam is where all
 * of them are decided.
 *   - the declared output list is a CONTRACT, checked before the writer runs:
 *     an output outside the export directory and a case-insensitive duplicate
 *     are both structured rejections, not degraded publications;
 *   - an output the writer produced but the enumeration missed blocks the WHOLE
 *     publication, before the first irreversible rename;
 *   - the two-phase swap rolls back completely, so the caller sees either the
 *     whole new set or the whole old set;
 *   - private leftovers of a process that died mid-export are reclaimed by
 *     owner liveness, including finishing or undoing an interrupted swap.
 */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define test_getpid _getpid
#else
#include <unistd.h>
#define test_getpid getpid
#endif

#include "tp_core/tp_export.h"
#include "tp_core/tp_identity.h"
#include "tp_fs_internal.h"
#include "unity.h"

/* A pid no host can have running: below INT_MAX so POSIX kill(pid, 0) probes it
 * at all, above every platform's pid ceiling, and not a multiple of four so
 * Windows classifies it as an invalid id rather than an unknown one. */
#define DEAD_PID_HEX "7ffffffe"

static const char *g_dir;
static char g_base[TP_IDENTITY_PATH_MAX];

/* Suffixes the fixture writer appends to the base it is GIVEN. The writer must
 * use ctx->write_path_base, so a plan entry lands in the staging dir during a
 * real publication and next to the outputs when driven directly. */
#define PLAN_MAX 4
static const char *g_plan[PLAN_MAX];
static int g_plan_count;
static int g_write_calls;

static void plan_reset(void) {
    g_plan_count = 0;
    g_write_calls = 0;
}

static void plan_add(const char *suffix) {
    TEST_ASSERT_TRUE(g_plan_count < PLAN_MAX);
    g_plan[g_plan_count++] = suffix;
}

static tp_status plan_write(const tp_export_write_ctx *ctx, tp_error *err) {
    g_write_calls++;
    for (int i = 0; i < g_plan_count; i++) {
        char path[TP_IDENTITY_PATH_MAX];
        if (snprintf(path, sizeof path, "%s%s", ctx->write_path_base,
                     g_plan[i]) <= 0) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "fixture writer path overflow");
        }
        if (!tp_fs_write_file(path, "NEW", 3U)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "fixture writer could not write '%s'", path);
        }
    }
    return TP_STATUS_OK;
}

static const tp_exporter g_exp = {.id = "test-set-publish",
                                  .display_name = "set publish fixture",
                                  .extension = "json",
                                  .write = plan_write};

void setUp(void) { plan_reset(); }

void tearDown(void) { tp_export_publish__test_fail_rename_at(-1); }

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static tp_status publish(const char *const *outputs, int count,
                         tp_export_notices *notices, bool *writer_ran,
                         tp_error *err) {
    tp_export_prepared prep;
    memset(&prep, 0, sizeof prep);
    return tp_export_write_and_publish_set(&g_exp, &prep, g_base, outputs,
                                           count, notices, writer_ran, err);
}

static bool file_holds(const char *path, const char *text) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char buffer[64];
    const size_t read = fread(buffer, 1U, sizeof buffer, f);
    (void)fclose(f);
    return read == strlen(text) && memcmp(buffer, text, read) == 0;
}

/* Any directory entry whose name contains `infix` -- the private names the
 * publication creates must never survive it. */
static bool any_entry_containing(const char *infix) {
    tp_fs_dir *dir = tp_fs_dir_open(g_dir);
    if (!dir) {
        return false;
    }
    bool found = false;
    tp_fs_dir_entry entry;
    while (!found && tp_fs_dir_next(dir, &entry) == TP_FS_DIR_ENTRY) {
        found = strstr(entry.name, infix) != NULL;
    }
    tp_fs_dir_close(dir);
    return found;
}

static void seed(const char *path, const char *text) {
    TEST_ASSERT_TRUE(tp_fs_write_file(path, text, strlen(text)));
}

/* "<g_dir>/<name>" into a caller buffer. */
#define IN_DIR(buf, name) \
    TEST_ASSERT_TRUE(snprintf((buf), sizeof(buf), "%s/%s", g_dir, (name)) > 0)

/* "<g_base><suffix>" into a caller buffer. */
#define AT_BASE(buf, suffix) \
    TEST_ASSERT_TRUE(snprintf((buf), sizeof(buf), "%s%s", g_base, (suffix)) > 0)

/* ------------------------------------------------------------------ */
/* the happy path                                                     */
/* ------------------------------------------------------------------ */

/* The whole listed set is published and no private name survives. Guards the
 * happy path against the preflight and swap reordering below. */
static void test_listed_set_publishes(void) {
    char primary[TP_IDENTITY_PATH_MAX];
    char page[TP_IDENTITY_PATH_MAX];
    AT_BASE(primary, ".json");
    AT_BASE(page, "-0.png");
    (void)tp_fs_remove_file(primary);
    (void)tp_fs_remove_file(page);

    plan_add(".json");
    plan_add("-0.png");
    const char *outputs[2] = {primary, page};

    tp_export_notices notices;
    tp_export_notices_init(&notices);
    tp_error error = {{0}};
    bool writer_ran = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, publish(outputs, 2, &notices, &writer_ran, &error),
        error.msg);
    TEST_ASSERT_TRUE(writer_ran);
    TEST_ASSERT_TRUE(file_holds(primary, "NEW"));
    TEST_ASSERT_TRUE(file_holds(page, "NEW"));
    TEST_ASSERT_EQUAL_INT(0, notices.count);
    TEST_ASSERT_FALSE(any_entry_containing(".tp-stage-"));
    TEST_ASSERT_FALSE_MESSAGE(any_entry_containing(".tp-old-"),
                              "a published set must not keep its rollback copies");

    tp_export_notices_free(&notices);
    (void)tp_fs_remove_file(primary);
    (void)tp_fs_remove_file(page);
}

/* ------------------------------------------------------------------ */
/* the declared list is a contract, checked before the writer runs    */
/* ------------------------------------------------------------------ */

/* An output that is not a direct child of the export directory cannot go
 * through the staging dir. The whole-set guarantee is the product property, so
 * the list is REJECTED -- publishing that one file on its own would silently
 * hand back a weaker promise than the caller asked for. */
static void test_output_outside_the_export_directory_is_rejected(void) {
    char sub[TP_IDENTITY_PATH_MAX];
    char deep[TP_IDENTITY_PATH_MAX];
    char primary[TP_IDENTITY_PATH_MAX];
    IN_DIR(sub, "setpub_sub");
    TEST_ASSERT_TRUE(snprintf(deep, sizeof deep, "%s/deep.json", sub) > 0);
    AT_BASE(primary, ".json");
    (void)tp_fs_remove_file(deep);
    (void)tp_fs_remove_dir(sub);
    TEST_ASSERT_TRUE(tp_fs_create_dir(sub));
    (void)tp_fs_remove_file(primary);
    seed(primary, "OLD");

    plan_add(".json");
    const char *outputs[2] = {primary, deep};

    tp_export_notices notices;
    tp_export_notices_init(&notices);
    tp_error error = {{0}};
    bool writer_ran = true;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          publish(outputs, 2, &notices, &writer_ran, &error));
    TEST_ASSERT_FALSE_MESSAGE(
        writer_ran, "the list is rejected BEFORE the writer is given a chance");
    TEST_ASSERT_EQUAL_INT(0, g_write_calls);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(error.msg, "deep.json"),
                                 "the error must name the offending output");
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(deep),
                              "a rejected list must write nothing at all");
    TEST_ASSERT_TRUE_MESSAGE(file_holds(primary, "OLD"),
                             "a rejected list must leave the old set intact");
    TEST_ASSERT_FALSE(any_entry_containing(".tp-stage-"));
    TEST_ASSERT_EQUAL_INT(0, notices.count);

    tp_export_notices_free(&notices);
    (void)tp_fs_remove_file(primary);
    (void)tp_fs_remove_dir(sub);
}

/* Same rule for a path that merely LOOKS like a sibling: an entry from another
 * directory shares no prefix with the export directory. */
static void test_output_in_a_foreign_directory_is_rejected(void) {
    char primary[TP_IDENTITY_PATH_MAX];
    AT_BASE(primary, ".json");
    const char *outputs[2] = {primary, "elsewhere/other.json"};

    plan_add(".json");
    tp_error error = {{0}};
    bool writer_ran = true;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          publish(outputs, 2, NULL, &writer_ran, &error));
    TEST_ASSERT_FALSE(writer_ran);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(error.msg, "other.json"), error.msg);
}

/* Two entries naming one file cannot describe a SET. The second promote would
 * fail after the first already published, so this is decided up front. */
static void test_duplicate_output_is_rejected(void) {
    char primary[TP_IDENTITY_PATH_MAX];
    AT_BASE(primary, ".json");
    const char *outputs[2] = {primary, primary};

    plan_add(".json");
    tp_error error = {{0}};
    bool writer_ran = true;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          publish(outputs, 2, NULL, &writer_ran, &error));
    TEST_ASSERT_FALSE(writer_ran);
    TEST_ASSERT_EQUAL_INT(0, g_write_calls);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(error.msg, "setpub.json"), error.msg);
}

/* Windows and macOS resolve "Setpub.JSON" and "setpub.json" to ONE file, so a
 * list that collides only by case is not a set there. It is rejected on every
 * host, because the contract must not depend on which machine ran the export. */
static void test_case_insensitive_duplicate_output_is_rejected(void) {
    char lower[TP_IDENTITY_PATH_MAX];
    char upper[TP_IDENTITY_PATH_MAX];
    AT_BASE(lower, ".json");
    IN_DIR(upper, "SETPUB.JSON");
    const char *outputs[2] = {lower, upper};

    plan_add(".json");
    tp_error error = {{0}};
    bool writer_ran = true;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          publish(outputs, 2, NULL, &writer_ran, &error));
    TEST_ASSERT_FALSE(writer_ran);
    TEST_ASSERT_EQUAL_INT(0, g_write_calls);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(error.msg, "setpub.json"), error.msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(error.msg, "SETPUB.JSON"), error.msg);
    /* Deliberately no tp_fs_exists(upper) probe: on the very hosts this rule
     * exists for, that name resolves to the lowercase sibling. "The writer was
     * never given a chance" is the honest proof that nothing was written. */
}

/* ------------------------------------------------------------------ */
/* the produced set is verified before the first irreversible rename  */
/* ------------------------------------------------------------------ */

/* An unlisted staged file is a PREFLIGHT verdict, not a cleanup discovery: the
 * report says nothing was published, so nothing may have been. */
static void test_unlisted_output_blocks_the_whole_publication(void) {
    char primary[TP_IDENTITY_PATH_MAX];
    char extra[TP_IDENTITY_PATH_MAX];
    AT_BASE(primary, ".json");
    AT_BASE(extra, ".extra");
    (void)tp_fs_remove_file(extra);
    seed(primary, "OLD");

    /* The writer produces both; the enumeration knows only the primary. */
    plan_add(".json");
    plan_add(".extra");
    const char *outputs[1] = {primary};

    tp_error error = {{0}};
    bool writer_ran = false;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          publish(outputs, 1, NULL, &writer_ran, &error));
    TEST_ASSERT_TRUE_MESSAGE(writer_ran,
                             "the writer really did run and overproduce");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(error.msg, ".extra"),
                                 "the error must name the unlisted output");
    TEST_ASSERT_TRUE_MESSAGE(
        file_holds(primary, "OLD"),
        "a failed set publish must leave every previous output untouched");
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(extra),
                              "the unlisted output must not be published");
    TEST_ASSERT_FALSE(any_entry_containing(".tp-stage-"));

    (void)tp_fs_remove_file(primary);
}

/* A listed output the writer never produced is the mirror verdict. */
static void test_missing_declared_output_blocks_the_publication(void) {
    char primary[TP_IDENTITY_PATH_MAX];
    char missing[TP_IDENTITY_PATH_MAX];
    AT_BASE(primary, ".json");
    AT_BASE(missing, ".missing");
    (void)tp_fs_remove_file(missing);
    seed(primary, "OLD");

    plan_add(".json");
    const char *outputs[2] = {primary, missing};

    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          publish(outputs, 2, NULL, NULL, &error));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(error.msg, ".missing"), error.msg);
    TEST_ASSERT_TRUE(file_holds(primary, "OLD"));
    TEST_ASSERT_FALSE(tp_fs_exists(missing));
    TEST_ASSERT_FALSE(any_entry_containing(".tp-stage-"));

    (void)tp_fs_remove_file(primary);
}

/* ------------------------------------------------------------------ */
/* the two-phase swap rolls back completely                           */
/* ------------------------------------------------------------------ */

/* A rename that fails while the set is going in used to leave the directory
 * half old and half new (the documented "residual window"). It must roll back:
 * the caller sees the COMPLETE previous set. */
static void test_a_failed_promote_restores_the_previous_set(void) {
    char first[TP_IDENTITY_PATH_MAX];
    char second[TP_IDENTITY_PATH_MAX];
    AT_BASE(first, ".json");
    AT_BASE(second, "-0.png");
    seed(first, "OLD-ONE");
    seed(second, "OLD-TWO");

    plan_add(".json");
    plan_add("-0.png");
    const char *outputs[2] = {first, second};

    /* Ordinals 0,1 displace the two existing destinations; 2,3 move the staged
     * set in. Failing 3 means one output was already published. */
    tp_export_publish__test_fail_rename_at(3);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          publish(outputs, 2, NULL, NULL, &error));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(error.msg, "rolled back"), error.msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(error.msg, "previous outputs were restored"),
                                 error.msg);
    TEST_ASSERT_TRUE_MESSAGE(
        file_holds(first, "OLD-ONE"),
        "the already-promoted output must be rolled back to its old bytes");
    TEST_ASSERT_TRUE(file_holds(second, "OLD-TWO"));
    TEST_ASSERT_FALSE(any_entry_containing(".tp-stage-"));
    TEST_ASSERT_FALSE_MESSAGE(any_entry_containing(".tp-old-"),
                              "a completed rollback keeps no rollback copies");

    (void)tp_fs_remove_file(first);
    (void)tp_fs_remove_file(second);
}

/* The same guarantee when the failure lands in the FIRST phase, before any new
 * bytes were placed: whatever was already moved aside comes back. */
static void test_a_failed_displace_restores_the_previous_set(void) {
    char first[TP_IDENTITY_PATH_MAX];
    char second[TP_IDENTITY_PATH_MAX];
    AT_BASE(first, ".json");
    AT_BASE(second, "-0.png");
    seed(first, "OLD-ONE");
    seed(second, "OLD-TWO");

    plan_add(".json");
    plan_add("-0.png");
    const char *outputs[2] = {first, second};

    tp_export_publish__test_fail_rename_at(1); /* the second displace */
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          publish(outputs, 2, NULL, NULL, &error));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(error.msg, "nothing was published"),
                                 error.msg);
    TEST_ASSERT_TRUE(file_holds(first, "OLD-ONE"));
    TEST_ASSERT_TRUE(file_holds(second, "OLD-TWO"));
    TEST_ASSERT_FALSE(any_entry_containing(".tp-stage-"));
    TEST_ASSERT_FALSE(any_entry_containing(".tp-old-"));

    (void)tp_fs_remove_file(first);
    (void)tp_fs_remove_file(second);
}

/* Rolling back an output that had NO previous version means removing it: the
 * old set did not contain that file, so neither may the restored one. */
static void test_a_rolled_back_new_output_is_removed(void) {
    char fresh[TP_IDENTITY_PATH_MAX];
    char existing[TP_IDENTITY_PATH_MAX];
    AT_BASE(fresh, ".fresh");
    AT_BASE(existing, ".json");
    (void)tp_fs_remove_file(fresh);
    seed(existing, "OLD");

    plan_add(".fresh");
    plan_add(".json");
    const char *outputs[2] = {fresh, existing};

    /* `fresh` has no destination to displace, so ordinal 0 is the displace of
     * `existing`, 1 promotes `fresh`, and 2 -- the one that fails -- would have
     * promoted `existing`. */
    tp_export_publish__test_fail_rename_at(2);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          publish(outputs, 2, NULL, NULL, &error));
    TEST_ASSERT_FALSE_MESSAGE(
        tp_fs_exists(fresh),
        "an output the old set did not have must not survive the rollback");
    TEST_ASSERT_TRUE(file_holds(existing, "OLD"));
    TEST_ASSERT_FALSE(any_entry_containing(".tp-stage-"));
    TEST_ASSERT_FALSE(any_entry_containing(".tp-old-"));

    (void)tp_fs_remove_file(existing);
}

/* ------------------------------------------------------------------ */
/* orphan reaping and crash-mid-swap recovery                         */
/* ------------------------------------------------------------------ */

static void test_a_dead_owners_staging_dir_is_reaped(void) {
    char orphan[TP_IDENTITY_PATH_MAX];
    char inside[TP_IDENTITY_PATH_MAX];
    char primary[TP_IDENTITY_PATH_MAX];
    IN_DIR(orphan, ".tp-stage-" DEAD_PID_HEX "-00000001");
    TEST_ASSERT_TRUE(snprintf(inside, sizeof inside, "%s/half.json", orphan) > 0);
    AT_BASE(primary, ".json");
    tp_fs_remove_tree(orphan);
    TEST_ASSERT_TRUE(tp_fs_create_dir(orphan));
    seed(inside, "ABANDONED");

    plan_add(".json");
    const char *outputs[1] = {primary};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  publish(outputs, 1, NULL, NULL, &error),
                                  error.msg);
    TEST_ASSERT_FALSE_MESSAGE(
        tp_fs_exists(orphan),
        "a staging dir whose owning process is gone must be reclaimed");

    (void)tp_fs_remove_file(primary);
}

/* Our OWN leftovers are not garbage: the pid is alive, so a concurrent export
 * in this very process is never swept out from under itself. */
static void test_a_live_owners_staging_dir_is_kept(void) {
    char mine[TP_IDENTITY_PATH_MAX];
    char name[64];
    char primary[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(snprintf(name, sizeof name, ".tp-stage-%08lx-0000ffff",
                              (unsigned long)test_getpid() & 0xffffffffUL) > 0);
    IN_DIR(mine, name);
    AT_BASE(primary, ".json");
    tp_fs_remove_tree(mine);
    TEST_ASSERT_TRUE(tp_fs_create_dir(mine));

    plan_add(".json");
    const char *outputs[1] = {primary};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  publish(outputs, 1, NULL, NULL, &error),
                                  error.msg);
    TEST_ASSERT_TRUE_MESSAGE(
        tp_fs_is_dir(mine),
        "a live owner's staging dir must survive another export's sweep");

    tp_fs_remove_tree(mine);
    (void)tp_fs_remove_file(primary);
}

/* Killed between the displace and the promote: the destination is MISSING, so
 * the recorded old copy is the only surviving version and must come back. */
static void test_an_interrupted_swap_is_restored(void) {
    char destination[TP_IDENTITY_PATH_MAX];
    char record[TP_IDENTITY_PATH_MAX];
    char primary[TP_IDENTITY_PATH_MAX];
    IN_DIR(destination, "interrupted.json");
    IN_DIR(record, "interrupted.json.tp-old-" DEAD_PID_HEX "-00000002");
    AT_BASE(primary, ".json");
    (void)tp_fs_remove_file(destination);
    seed(record, "SURVIVOR");

    plan_add(".json");
    const char *outputs[1] = {primary};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  publish(outputs, 1, NULL, NULL, &error),
                                  error.msg);
    TEST_ASSERT_TRUE_MESSAGE(
        file_holds(destination, "SURVIVOR"),
        "an interrupted swap must put the previous output back");
    TEST_ASSERT_FALSE(tp_fs_exists(record));

    (void)tp_fs_remove_file(destination);
    (void)tp_fs_remove_file(primary);
}

/* Killed after the promote: the destination is PRESENT, so the swap completed
 * and the old copy is spent -- restoring it would undo a published export. */
static void test_a_completed_swap_record_is_discarded(void) {
    char destination[TP_IDENTITY_PATH_MAX];
    char record[TP_IDENTITY_PATH_MAX];
    char primary[TP_IDENTITY_PATH_MAX];
    IN_DIR(destination, "completed.json");
    IN_DIR(record, "completed.json.tp-old-" DEAD_PID_HEX "-00000003");
    AT_BASE(primary, ".json");
    seed(destination, "PUBLISHED");
    seed(record, "SUPERSEDED");

    plan_add(".json");
    const char *outputs[1] = {primary};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  publish(outputs, 1, NULL, NULL, &error),
                                  error.msg);
    TEST_ASSERT_TRUE_MESSAGE(
        file_holds(destination, "PUBLISHED"),
        "a completed swap must not be rolled back by the reaper");
    TEST_ASSERT_FALSE(tp_fs_exists(record));

    (void)tp_fs_remove_file(destination);
    (void)tp_fs_remove_file(primary);
}

/* ------------------------------------------------------------------ */
/* the primitives the publication is built on                         */
/* ------------------------------------------------------------------ */

/* The staging dir must own what it creates. tp_fs_create_dir ADOPTS an existing
 * directory (that is its contract and other callers want it), which is exactly
 * why the staging path uses the exclusive primitive instead. */
static void test_exclusive_create_never_adopts_an_existing_name(void) {
    char dir_path[TP_IDENTITY_PATH_MAX];
    char file_path[TP_IDENTITY_PATH_MAX];
    IN_DIR(dir_path, "excl_dir");
    IN_DIR(file_path, "excl_file");
    (void)tp_fs_remove_dir(dir_path);
    (void)tp_fs_remove_file(file_path);

    TEST_ASSERT_EQUAL_INT(TP_FS_CREATE_DIR_OK,
                          tp_fs_create_dir_exclusive(dir_path));
    TEST_ASSERT_TRUE(tp_fs_is_dir(dir_path));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_FS_CREATE_DIR_EXISTS, tp_fs_create_dir_exclusive(dir_path),
        "an existing directory must be reported, never adopted");
    TEST_ASSERT_TRUE_MESSAGE(
        tp_fs_create_dir(dir_path),
        "the adopting create is the contrast that makes the exclusive one "
        "necessary");

    TEST_ASSERT_TRUE(tp_fs_write_file(file_path, "x", 1U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_FS_CREATE_DIR_EXISTS, tp_fs_create_dir_exclusive(file_path),
        "a non-directory occupying the name is EXISTS, never OK");

    (void)tp_fs_remove_dir(dir_path);
    (void)tp_fs_remove_file(file_path);
}

/* Two staging dirs in one process never collide, and each is really created. */
static void test_stage_dirs_are_distinct_and_created(void) {
    char parent[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(snprintf(parent, sizeof parent, "%s/", g_dir) > 0);
    char first[TP_FS_STAGE_PATH_MAX];
    char second[TP_FS_STAGE_PATH_MAX];
    TEST_ASSERT_TRUE(tp_fs_stage_dir_create(parent, first, sizeof first));
    TEST_ASSERT_TRUE(tp_fs_stage_dir_create(parent, second, sizeof second));
    TEST_ASSERT_TRUE(tp_fs_is_dir(first));
    TEST_ASSERT_TRUE(tp_fs_is_dir(second));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp(first, second));
    tp_fs_remove_tree(first);
    tp_fs_remove_tree(second);
}

/* The displaced name is a SIBLING of the destination (a pure rename, same
 * volume) and never repeats within one process. */
static void test_displaced_names_are_siblings_and_distinct(void) {
    char destination[TP_IDENTITY_PATH_MAX];
    IN_DIR(destination, "swapme.json");
    char first[TP_FS_STAGE_PATH_MAX];
    char second[TP_FS_STAGE_PATH_MAX];
    TEST_ASSERT_TRUE(tp_fs_stage_old_path(destination, first, sizeof first));
    TEST_ASSERT_TRUE(tp_fs_stage_old_path(destination, second, sizeof second));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp(first, second));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, strncmp(first, destination, strlen(destination)),
        "the rollback copy must sit beside the file it replaces");
    TEST_ASSERT_NOT_NULL(strstr(first, ".tp-old-"));
}

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    if (snprintf(g_base, sizeof g_base, "%s/setpub", g_dir) <= 0) {
        return 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_listed_set_publishes);
    RUN_TEST(test_output_outside_the_export_directory_is_rejected);
    RUN_TEST(test_output_in_a_foreign_directory_is_rejected);
    RUN_TEST(test_duplicate_output_is_rejected);
    RUN_TEST(test_case_insensitive_duplicate_output_is_rejected);
    RUN_TEST(test_unlisted_output_blocks_the_whole_publication);
    RUN_TEST(test_missing_declared_output_blocks_the_publication);
    RUN_TEST(test_a_failed_promote_restores_the_previous_set);
    RUN_TEST(test_a_failed_displace_restores_the_previous_set);
    RUN_TEST(test_a_rolled_back_new_output_is_removed);
    RUN_TEST(test_a_dead_owners_staging_dir_is_reaped);
    RUN_TEST(test_a_live_owners_staging_dir_is_kept);
    RUN_TEST(test_an_interrupted_swap_is_restored);
    RUN_TEST(test_a_completed_swap_record_is_discarded);
    RUN_TEST(test_exclusive_create_never_adopts_an_existing_name);
    RUN_TEST(test_stage_dirs_are_distinct_and_created);
    RUN_TEST(test_displaced_names_are_siblings_and_distinct);
    return UNITY_END();
}
