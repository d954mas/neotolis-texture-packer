/* Contract test for the shared app scratch root (apps/common/app_scratch.h):
 * the location policy both shipped clients resolve, and the private per-request
 * directory that keeps two concurrent jobs from writing one `<atlas>.ntpack`.
 *
 * Sandboxing: every directory this test creates lives under a scratch tree in
 * the ctest working directory. The platform cache variable is overridden
 * in-process so the platform branch is exercised WITHOUT touching the real user
 * cache directory, and the root composition has its own explicit injection
 * point (app_scratch_root_in) rather than a hidden global. */

#include "app_scratch.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "nt_utf8_fs.h"
#include "tp_core/tp_scan.h"
#include "unity.h"

#ifdef _WIN32
#define CACHE_ENV "LOCALAPPDATA"
#else
#define CACHE_ENV "XDG_CACHE_HOME"
#endif

#define SCRATCH_MAX 1024

static char s_sandbox[SCRATCH_MAX];
static char s_saved_cache_env[SCRATCH_MAX];
static bool s_had_cache_env;

/* Assertion-free: main() calls these outside a Unity test frame, where a failing
 * TEST_ASSERT would long-jump with no frame to return to. An empty value removes
 * the variable on both platforms. */
static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    (void)_putenv_s(name, value ? value : "");
#else
    if (value) {
        (void)setenv(name, value, 1);
    } else {
        (void)unsetenv(name);
    }
#endif
}

static bool working_dir(char *out, size_t capacity) {
#ifdef _WIN32
    if (!_getcwd(out, (int)capacity)) {
        return false;
    }
#else
    if (!getcwd(out, capacity)) {
        return false;
    }
#endif
    for (char *cursor = out; *cursor; ++cursor) {
        if (*cursor == '\\') {
            *cursor = '/';
        }
    }
    return true;
}

static void write_file(const char *path, const char *content) {
    FILE *file = nt_utf8_fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(content),
                             fwrite(content, 1U, strlen(content), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* Removes the sandbox tree. Deliberately shape-specific rather than generic:
 * every test releases the directories it created, so all that is left is the
 * empty <sandbox>/ntpacker/work chain. */
static void remove_sandbox(void) {
    char path[SCRATCH_MAX];
    static const char *const levels[] = {"/ntpacker/work", "/ntpacker", ""};
    for (size_t i = 0U; i < sizeof levels / sizeof *levels; ++i) {
        const int length =
            snprintf(path, sizeof path, "%s%s", s_sandbox, levels[i]);
        if (length > 0 && (size_t)length < sizeof path) {
            (void)nt_utf8_rmdir(path);
        }
    }
}

void setUp(void) {}
void tearDown(void) {}

/* The platform cache directory is read from the platform's own variable --
 * %LOCALAPPDATA% on Windows, $XDG_CACHE_HOME elsewhere -- and nothing else. */
void test_cache_dir_comes_from_the_platform_cache_variable(void) {
    set_env(CACHE_ENV, s_sandbox);
    char cache[SCRATCH_MAX];
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, app_scratch_cache_dir(cache, sizeof cache, &error),
        error.msg);
    TEST_ASSERT_EQUAL_STRING(s_sandbox, cache);
}

/* The root is `<cache dir>/ntpacker/work` and both levels are created on first
 * run, so a fresh machine does not fail its first Pack. */
void test_root_is_created_under_the_cache_dir(void) {
    char expected[SCRATCH_MAX];
    const int length =
        snprintf(expected, sizeof expected, "%s/ntpacker/work", s_sandbox);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof expected);

    char root[SCRATCH_MAX];
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        app_scratch_root_in(s_sandbox, root, sizeof root, &error), error.msg);
    TEST_ASSERT_EQUAL_STRING(expected, root);
    TEST_ASSERT_TRUE(tp_scan_is_dir(root));
}

/* The production resolver is the composition of the two: the path a client
 * actually packs into sits under the platform cache root. */
void test_production_resolver_lands_under_the_platform_cache_root(void) {
    set_env(CACHE_ENV, s_sandbox);
    char expected[SCRATCH_MAX];
    const int length =
        snprintf(expected, sizeof expected, "%s/ntpacker/work", s_sandbox);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof expected);

    char root[SCRATCH_MAX];
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, app_scratch_root(root, sizeof root, &error), error.msg);
    TEST_ASSERT_EQUAL_STRING(expected, root);
    TEST_ASSERT_TRUE(tp_scan_is_dir(root));
}

/* No silent fallback to the exe dir or the system temp dir: an unresolvable
 * cache location is a structured failure with an empty output. */
void test_an_unresolvable_cache_dir_is_a_structured_error(void) {
    set_env(CACHE_ENV, NULL);
#ifndef _WIN32
    char saved_home[SCRATCH_MAX] = {0};
    const char *home = getenv("HOME");
    const bool had_home = home != NULL && home[0] != '\0';
    if (had_home) {
        (void)snprintf(saved_home, sizeof saved_home, "%s", home);
    }
    set_env("HOME", NULL);
#endif
    char root[SCRATCH_MAX];
    tp_error error = {{0}};
    const tp_status status = app_scratch_root(root, sizeof root, &error);
    TEST_ASSERT_NOT_EQUAL_INT(TP_STATUS_OK, status);
    TEST_ASSERT_EQUAL_CHAR('\0', root[0]);
    TEST_ASSERT_TRUE(error.msg[0] != '\0');
#ifndef _WIN32
    if (had_home) {
        set_env("HOME", saved_home);
    }
#endif
    set_env(CACHE_ENV, s_sandbox);
}

/* A path that does not fit is refused whole, never truncated into a directory
 * some other job owns. */
void test_a_request_directory_that_does_not_fit_is_refused(void) {
    char root[SCRATCH_MAX];
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        app_scratch_root_in(s_sandbox, root, sizeof root, &error));

    char tight[16];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        app_scratch_request_dir(root, 1U, tight, sizeof tight, &error));
    TEST_ASSERT_EQUAL_CHAR('\0', tight[0]);
}

/* The clobber this packet exists to kill: an Export request and a Pack request
 * that are live at the same time get DIFFERENT private directories, so the
 * `<atlas>.ntpack` each one stages through cannot overwrite the other's. */
void test_a_live_export_and_pack_request_never_share_a_directory(void) {
    char root[SCRATCH_MAX];
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        app_scratch_root_in(s_sandbox, root, sizeof root, &error), error.msg);

    char pack_dir[SCRATCH_MAX];
    char export_dir[SCRATCH_MAX];
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        app_scratch_request_dir(root, 41U, pack_dir, sizeof pack_dir, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        app_scratch_request_dir(root, 42U, export_dir, sizeof export_dir,
                                &error),
        error.msg);

    TEST_ASSERT_TRUE(strcmp(pack_dir, export_dir) != 0);
    TEST_ASSERT_TRUE(tp_scan_is_dir(pack_dir));
    TEST_ASSERT_TRUE(tp_scan_is_dir(export_dir));
    TEST_ASSERT_EQUAL_STRING_LEN(root, pack_dir, strlen(root));
    TEST_ASSERT_EQUAL_STRING_LEN(root, export_dir, strlen(root));
    /* The shared `req-<hexpid>-` naming contract: one reaper heals every owner
     * that follows it (packer/src/tp_build_worker_internal.h). */
    TEST_ASSERT_NOT_NULL(strstr(pack_dir, "/req-"));
    TEST_ASSERT_NOT_NULL(strstr(export_dir, "/req-"));

    char pack_artifact[SCRATCH_MAX];
    char export_artifact[SCRATCH_MAX];
    (void)snprintf(pack_artifact, sizeof pack_artifact, "%s/atlas1.ntpack",
                   pack_dir);
    (void)snprintf(export_artifact, sizeof export_artifact, "%s/atlas1.ntpack",
                   export_dir);
    write_file(pack_artifact, "pack");
    write_file(export_artifact, "export");
    TEST_ASSERT_TRUE(tp_scan_exists(pack_artifact));
    TEST_ASSERT_TRUE(tp_scan_exists(export_artifact));

    app_scratch_request_dir_release(pack_dir);
    app_scratch_request_dir_release(export_dir);
}

/* Releasing takes the staged artifacts with the directory: tp_pack does not
 * delete the `<atlas>.ntpack` it produced, so the owner of the directory must. */
void test_release_removes_the_directory_with_its_artifacts(void) {
    char root[SCRATCH_MAX];
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        app_scratch_root_in(s_sandbox, root, sizeof root, &error));

    char request_dir[SCRATCH_MAX];
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        app_scratch_request_dir(root, 77U, request_dir, sizeof request_dir,
                                &error),
        error.msg);
    char artifact[SCRATCH_MAX];
    (void)snprintf(artifact, sizeof artifact, "%s/atlas1.ntpack", request_dir);
    write_file(artifact, "artifact");
    char second[SCRATCH_MAX];
    (void)snprintf(second, sizeof second, "%s/atlas2.ntpack", request_dir);
    write_file(second, "artifact");

    app_scratch_request_dir_release(request_dir);
    TEST_ASSERT_FALSE(tp_scan_exists(artifact));
    TEST_ASSERT_FALSE(tp_scan_exists(second));
    TEST_ASSERT_FALSE(tp_scan_exists(request_dir));
    /* The root itself is shared and survives its requests. */
    TEST_ASSERT_TRUE(tp_scan_is_dir(root));
}

/* Release only ever removes a `req-` directory, so a wrong argument cannot take
 * a directory this module did not create. */
void test_release_refuses_a_directory_it_did_not_create(void) {
    char root[SCRATCH_MAX];
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        app_scratch_root_in(s_sandbox, root, sizeof root, &error));

    char foreign[SCRATCH_MAX];
    (void)snprintf(foreign, sizeof foreign, "%s/not-a-request", root);
    tp_mkdirs(foreign);
    TEST_ASSERT_TRUE(tp_scan_is_dir(foreign));
    char keeper[SCRATCH_MAX];
    (void)snprintf(keeper, sizeof keeper, "%s/keep.txt", foreign);
    write_file(keeper, "keep");

    app_scratch_request_dir_release(foreign);
    TEST_ASSERT_TRUE(tp_scan_is_dir(foreign));
    TEST_ASSERT_TRUE(tp_scan_exists(keeper));

    (void)nt_utf8_remove(keeper);
    (void)nt_utf8_rmdir(foreign);
    TEST_ASSERT_FALSE(tp_scan_exists(foreign));
}

int main(void) {
    char cwd[SCRATCH_MAX];
    if (!working_dir(cwd, sizeof cwd)) {
        (void)fprintf(stderr,
                      "app_scratch test: could not read the working directory\n");
        return 1;
    }
    const int length =
        snprintf(s_sandbox, sizeof s_sandbox, "%s/app_scratch_sandbox", cwd);
    if (length <= 0 || (size_t)length >= sizeof s_sandbox) {
        (void)fprintf(stderr, "app_scratch test: sandbox path is too long\n");
        return 1;
    }
    const char *existing = getenv(CACHE_ENV);
    s_had_cache_env = existing != NULL && existing[0] != '\0';
    if (s_had_cache_env) {
        (void)snprintf(s_saved_cache_env, sizeof s_saved_cache_env, "%s",
                       existing);
    }
    tp_mkdirs(s_sandbox);

    UNITY_BEGIN();
    RUN_TEST(test_cache_dir_comes_from_the_platform_cache_variable);
    RUN_TEST(test_root_is_created_under_the_cache_dir);
    RUN_TEST(test_production_resolver_lands_under_the_platform_cache_root);
    RUN_TEST(test_an_unresolvable_cache_dir_is_a_structured_error);
    RUN_TEST(test_a_request_directory_that_does_not_fit_is_refused);
    RUN_TEST(test_a_live_export_and_pack_request_never_share_a_directory);
    RUN_TEST(test_release_removes_the_directory_with_its_artifacts);
    RUN_TEST(test_release_refuses_a_directory_it_did_not_create);
    const int result = UNITY_END();

    if (s_had_cache_env) {
        set_env(CACHE_ENV, s_saved_cache_env);
    } else {
        set_env(CACHE_ENV, NULL);
    }
    remove_sandbox();
    return result;
}
