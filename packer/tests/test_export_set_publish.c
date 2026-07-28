/* Export output-SET publication contract (tp_core/tp_export.h).
 *
 * Drives tp_export_write_and_publish_set directly over a fixture exporter: none
 * of the properties below needs a real pack, and the publish seam is where all
 * three are decided.
 *   - the staging dir is created EXCLUSIVELY: an existing name is never adopted;
 *   - an output the writer produced but the enumeration missed blocks the WHOLE
 *     publication, before the first irreversible rename;
 *   - an output whose path cannot be staged is published per-file and says so
 *     with a structured notice instead of degrading silently. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_export.h"
#include "tp_core/tp_identity.h"
#include "tp_fs_internal.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const char *g_dir;
static char g_base[TP_IDENTITY_PATH_MAX];

/* Absolute paths the fixture writer produces, in order. */
#define PLAN_MAX 4
static char g_plan[PLAN_MAX][TP_IDENTITY_PATH_MAX];
static int g_plan_count;

static void plan_reset(void) { g_plan_count = 0; }

static void plan_add(const char *path) {
    TEST_ASSERT_TRUE(g_plan_count < PLAN_MAX);
    TEST_ASSERT_TRUE(
        snprintf(g_plan[g_plan_count++], TP_IDENTITY_PATH_MAX, "%s", path) > 0);
}

static tp_status plan_write(const tp_export_prepared *prep,
                            const tp_export_caps *caps,
                            const char *out_path_base,
                            tp_export_notices *notices, tp_error *err) {
    (void)prep;
    (void)caps;
    (void)out_path_base;
    (void)notices;
    for (int i = 0; i < g_plan_count; i++) {
        if (!tp_fs_write_file_atomic(g_plan[i], "NEW", 3U)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "fixture writer could not write '%s'",
                                g_plan[i]);
        }
    }
    return TP_STATUS_OK;
}

static const tp_exporter g_exp = {.id = "test-set-publish",
                                  .display_name = "set publish fixture",
                                  .extension = "json",
                                  .write = plan_write};

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

static bool any_stage_dir_left(void) {
    tp_fs_dir *dir = tp_fs_dir_open(g_dir);
    if (!dir) {
        return false;
    }
    bool found = false;
    tp_fs_dir_entry entry;
    while (!found && tp_fs_dir_next(dir, &entry) == TP_FS_DIR_ENTRY) {
        found = strstr(entry.name, ".tp-stage-") != NULL;
    }
    tp_fs_dir_close(dir);
    return found;
}

static int notice_index(const tp_export_notices *n, int field_id,
                        int reason_id) {
    for (int i = 0; i < n->count; i++) {
        if (n->items[i].field_id == field_id &&
            n->items[i].reason_id == reason_id) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */

/* The whole listed set is published and the staging dir is gone. Guards the
 * happy path against the preflight reordering below. */
static void test_listed_set_publishes(void) {
    char primary[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(snprintf(primary, sizeof primary, "%s.json", g_base) > 0);
    (void)tp_fs_remove_file(primary);

    plan_reset();
    plan_add(primary);
    const char *outputs[1] = {primary};

    tp_export_prepared prep;
    memset(&prep, 0, sizeof prep);
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_export_write_and_publish_set(&g_exp, &prep, g_base, outputs, 1,
                                        &notices, &error),
        error.msg);
    TEST_ASSERT_TRUE(file_holds(primary, "NEW"));
    TEST_ASSERT_EQUAL_INT(0, notices.count);
    TEST_ASSERT_FALSE(any_stage_dir_left());

    tp_export_notices_free(&notices);
    (void)tp_fs_remove_file(primary);
}

/* An unlisted staged file is a PREFLIGHT verdict, not a cleanup discovery: the
 * report says nothing was published, so nothing may have been. Checked after
 * the promote loop it reported failure over a fully republished set. */
static void test_unlisted_output_blocks_the_whole_publication(void) {
    char primary[TP_IDENTITY_PATH_MAX];
    char extra[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(snprintf(primary, sizeof primary, "%s.json", g_base) > 0);
    TEST_ASSERT_TRUE(snprintf(extra, sizeof extra, "%s.extra", g_base) > 0);
    (void)tp_fs_remove_file(extra);
    TEST_ASSERT_TRUE(tp_fs_write_file(primary, "OLD", 3U));

    /* The writer produces both; the enumeration knows only the primary. */
    plan_reset();
    plan_add(primary);
    plan_add(extra);
    const char *outputs[1] = {primary};

    tp_export_prepared prep;
    memset(&prep, 0, sizeof prep);
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_BAD_PROJECT,
        tp_export_write_and_publish_set(&g_exp, &prep, g_base, outputs, 1,
                                        &notices, &error));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(error.msg, ".extra"),
                                 "the error must name the unlisted output");
    TEST_ASSERT_TRUE_MESSAGE(
        file_holds(primary, "OLD"),
        "a failed set publish must leave every previous output untouched");
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(extra),
                              "the unlisted output must not be published");
    TEST_ASSERT_FALSE(any_stage_dir_left());

    tp_export_notices_free(&notices);
    (void)tp_fs_remove_file(primary);
}

/* An output that is not a direct child of the output directory cannot go
 * through the staging dir. Full support for arbitrarily deep outputs is out of
 * scope; the degradation must be OBSERVABLE, not silent. */
static void test_unstageable_output_is_published_with_a_notice(void) {
    char sub[TP_IDENTITY_PATH_MAX];
    char deep[TP_IDENTITY_PATH_MAX];
    char primary[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(snprintf(sub, sizeof sub, "%s/setpub_sub", g_dir) > 0);
    TEST_ASSERT_TRUE(snprintf(deep, sizeof deep, "%s/deep.json", sub) > 0);
    TEST_ASSERT_TRUE(snprintf(primary, sizeof primary, "%s.json", g_base) > 0);
    (void)tp_fs_remove_file(deep);
    (void)tp_fs_remove_dir(sub);
    TEST_ASSERT_TRUE(tp_fs_create_dir(sub));
    (void)tp_fs_remove_file(primary);

    plan_reset();
    plan_add(primary);
    plan_add(deep);
    const char *outputs[2] = {primary, deep};

    tp_export_prepared prep;
    memset(&prep, 0, sizeof prep);
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_export_write_and_publish_set(&g_exp, &prep, g_base, outputs, 2,
                                        &notices, &error),
        error.msg);
    TEST_ASSERT_TRUE(file_holds(primary, "NEW"));
    TEST_ASSERT_TRUE(file_holds(deep, "NEW"));

    const int at = notice_index(&notices, TP_NOTICE_FIELD_SET_ATOMICITY,
                                TP_NOTICE_REASON_PATH_NOT_STAGEABLE);
    TEST_ASSERT_TRUE_MESSAGE(
        at >= 0, "an output that bypassed set staging must raise a notice");
    TEST_ASSERT_EQUAL_STRING(g_exp.id, notices.items[at].target);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(notices.items[at].msg, "deep.json"),
                                 "the notice must name the bypassed output");
    /* The staged member keeps the guarantee, so it raises nothing. */
    TEST_ASSERT_EQUAL_INT(1, notices.count);
    TEST_ASSERT_FALSE(any_stage_dir_left());

    tp_export_notices_free(&notices);
    (void)tp_fs_remove_file(primary);
    (void)tp_fs_remove_file(deep);
    (void)tp_fs_remove_dir(sub);
}

/* Once an output has bypassed staging it IS published, so a preflight failure
 * that fires afterwards may not go on claiming the existing outputs are
 * untouched -- the operator would read "nothing happened" over a directory that
 * already changed. The whole-set half of the promise is still true and still
 * said out loud: no staged output was promoted. */
static void test_a_failure_after_a_bypass_does_not_claim_nothing_happened(void) {
    char sub[TP_IDENTITY_PATH_MAX];
    char deep[TP_IDENTITY_PATH_MAX];
    char missing[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(snprintf(sub, sizeof sub, "%s/setpub_sub2", g_dir) > 0);
    TEST_ASSERT_TRUE(snprintf(deep, sizeof deep, "%s/deep.json", sub) > 0);
    TEST_ASSERT_TRUE(
        snprintf(missing, sizeof missing, "%s.missing", g_base) > 0);
    (void)tp_fs_remove_file(deep);
    (void)tp_fs_remove_dir(sub);
    TEST_ASSERT_TRUE(tp_fs_create_dir(sub));
    (void)tp_fs_remove_file(missing);

    /* The bypassed output comes FIRST, so it is already published by the time
     * the enumeration's second entry turns out never to have been written. */
    plan_reset();
    plan_add(deep);
    const char *outputs[2] = {deep, missing};

    tp_export_prepared prep;
    memset(&prep, 0, sizeof prep);
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_BAD_PROJECT,
        tp_export_write_and_publish_set(&g_exp, &prep, g_base, outputs, 2,
                                        &notices, &error));
    TEST_ASSERT_TRUE_MESSAGE(
        file_holds(deep, "NEW"),
        "the bypassed output really is published, which is why the message "
        "may not say otherwise");
    TEST_ASSERT_NULL_MESSAGE(
        strstr(error.msg, "existing outputs are untouched"),
        error.msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(error.msg, "1 bypassed output was already published "
                          "individually"),
        error.msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(error.msg, "no staged output was promoted"), error.msg);
    TEST_ASSERT_FALSE(tp_fs_exists(missing));
    TEST_ASSERT_FALSE(any_stage_dir_left());

    tp_export_notices_free(&notices);
    (void)tp_fs_remove_file(deep);
    (void)tp_fs_remove_dir(sub);
}

/* The staging dir must own what it creates. tp_fs_create_dir ADOPTS an existing
 * directory (that is its contract and other callers want it), which is exactly
 * why the staging path uses the exclusive primitive instead. */
static void test_exclusive_create_never_adopts_an_existing_name(void) {
    char dir_path[TP_IDENTITY_PATH_MAX];
    char file_path[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(snprintf(dir_path, sizeof dir_path, "%s/excl_dir", g_dir) >
                     0);
    TEST_ASSERT_TRUE(
        snprintf(file_path, sizeof file_path, "%s/excl_file", g_dir) > 0);
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
    char first[TP_FS_ATOMIC_TEMP_PATH_MAX];
    char second[TP_FS_ATOMIC_TEMP_PATH_MAX];
    TEST_ASSERT_TRUE(tp_fs_stage_dir_create(parent, first, sizeof first));
    TEST_ASSERT_TRUE(tp_fs_stage_dir_create(parent, second, sizeof second));
    TEST_ASSERT_TRUE(tp_fs_is_dir(first));
    TEST_ASSERT_TRUE(tp_fs_is_dir(second));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp(first, second));
    tp_fs_remove_tree(first);
    tp_fs_remove_tree(second);
}

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    if (snprintf(g_base, sizeof g_base, "%s/setpub", g_dir) <= 0) {
        return 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_listed_set_publishes);
    RUN_TEST(test_unlisted_output_blocks_the_whole_publication);
    RUN_TEST(test_unstageable_output_is_published_with_a_notice);
    RUN_TEST(test_a_failure_after_a_bypass_does_not_claim_nothing_happened);
    RUN_TEST(test_exclusive_create_never_adopts_an_existing_name);
    RUN_TEST(test_stage_dirs_are_distinct_and_created);
    return UNITY_END();
}
