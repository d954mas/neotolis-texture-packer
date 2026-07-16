#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "tp_core/tp_identity.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h"
#include "tp_core/tp_project_lease.h"
#include "tp_core/tp_scan.h"
#include "unity.h"

static const char *g_self;
static const char *g_dir;
static const char *g_cli;

void setUp(void) {}
void tearDown(void) {}

static void join_path(char *out, size_t cap, const char *dir, const char *leaf) {
    (void)snprintf(out, cap, "%s/%s", dir, leaf);
}

static void write_file(const char *path) {
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT(1, (int)fwrite("x", 1U, 1U, f));
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    (void)fclose(f);
    return true;
}

static void project_lock_path(const char *project, char *out, size_t cap) {
    char canonical[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_identity_path_canonical(project, canonical,
                                                     sizeof canonical, NULL));
    const int written = snprintf(out, cap, "%s.ntpacker.lock", canonical);
    TEST_ASSERT_GREATER_THAN_INT(0, written);
    TEST_ASSERT_LESS_THAN_INT((int)cap, written);
}

static int child_acquire_exit(const char *path) {
#ifdef _WIN32
    return (int)_spawnl(_P_WAIT, g_self, g_self, "--child-acquire", path, NULL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        (void)execl(g_self, g_self, "--child-acquire", path, (char *)NULL);
        _exit(127);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, (int)pid);
    int status = 0;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    return WEXITSTATUS(status);
#endif
}

static int cli_atlas_add_exit(const char *path, const char *name) {
#ifdef _WIN32
    return (int)_spawnl(_P_WAIT, g_cli, g_cli, "atlas", "add", path, name,
                        NULL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        (void)execl(g_cli, g_cli, "atlas", "add", path, name, (char *)NULL);
        _exit(127);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, (int)pid);
    int status = 0;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    return WEXITSTATUS(status);
#endif
}

static int cli_read_exit(const char *verb, const char *path,
                         const char *option) {
#ifdef _WIN32
    return option ? (int)_spawnl(_P_WAIT, g_cli, g_cli, verb, path, option, NULL)
                  : (int)_spawnl(_P_WAIT, g_cli, g_cli, verb, path, NULL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        if (option) {
            (void)execl(g_cli, g_cli, verb, path, option, (char *)NULL);
        } else {
            (void)execl(g_cli, g_cli, verb, path, (char *)NULL);
        }
        _exit(127);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, (int)pid);
    int status = 0;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    return WEXITSTATUS(status);
#endif
}

void test_acquires_existing_project_and_releases_without_unlink(void) {
    char project[TP_IDENTITY_PATH_MAX];
    join_path(project, sizeof project, g_dir, "existing.ntpacker_project");
    write_file(project);

    tp_project_lease *lease = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(project, &lease, &err));
    TEST_ASSERT_NOT_NULL(lease);

    char lock_path[TP_IDENTITY_PATH_MAX + 32];
    project_lock_path(project, lock_path, sizeof lock_path);
    tp_project_lease_release(lease);
    TEST_ASSERT_TRUE(file_exists(lock_path));

    lease = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(project, &lease, &err));
    tp_project_lease_release(lease);
}

void test_acquires_not_yet_created_project_destination(void) {
    char project[TP_IDENTITY_PATH_MAX];
    join_path(project, sizeof project, g_dir, "new-destination.ntpacker_project");
    (void)remove(project);

    tp_project_lease *lease = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(project, &lease, &err));
    TEST_ASSERT_NOT_NULL(lease);
    TEST_ASSERT_FALSE(file_exists(project));
    char lock_path[TP_IDENTITY_PATH_MAX + 32];
    project_lock_path(project, lock_path, sizeof lock_path);
    tp_project_lease_release(lease);
    TEST_ASSERT_TRUE(file_exists(lock_path));
}

void test_live_owner_returns_project_live_to_other_process(void) {
    char project[TP_IDENTITY_PATH_MAX];
    join_path(project, sizeof project, g_dir, "busy.ntpacker_project");
    write_file(project);

    tp_project_lease *lease = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(project, &lease, NULL));
    TEST_ASSERT_EQUAL_INT(23, child_acquire_exit(project));
    tp_project_lease_release(lease);
    TEST_ASSERT_EQUAL_INT(0, child_acquire_exit(project));
}

void test_stale_lock_file_is_not_a_live_owner(void) {
    char project[TP_IDENTITY_PATH_MAX];
    join_path(project, sizeof project, g_dir, "stale.ntpacker_project");
    write_file(project);

    char canonical[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_identity_path_canonical(project, canonical,
                                                     sizeof canonical, NULL));
    char lock_path[TP_IDENTITY_PATH_MAX + 32];
    (void)snprintf(lock_path, sizeof lock_path, "%s.ntpacker.lock", canonical);
    write_file(lock_path);

    tp_project_lease *lease = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(project, &lease, NULL));
    tp_project_lease_release(lease);
    TEST_ASSERT_TRUE(file_exists(lock_path));
}

void test_canonical_alias_uses_same_lock_domain(void) {
    char project[TP_IDENTITY_PATH_MAX];
    char alias[TP_IDENTITY_PATH_MAX];
    join_path(project, sizeof project, g_dir, "alias.ntpacker_project");
    join_path(alias, sizeof alias, g_dir, "./alias.ntpacker_project");
    write_file(project);

    tp_project_lease *lease = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(project, &lease, NULL));
    TEST_ASSERT_EQUAL_INT(23, child_acquire_exit(alias));
    tp_project_lease_release(lease);
}

void test_cli_mutation_respects_process_project_lease(void) {
    char project[TP_IDENTITY_PATH_MAX];
    join_path(project, sizeof project, g_dir, "cli-busy.ntpacker_project");
    tp_project *seed = tp_project_create();
    TEST_ASSERT_NOT_NULL(seed);
    tp_rng rng = tp_rng_os();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_promote_ids(seed, &rng, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(seed, project, NULL));
    tp_project_destroy(seed);

    tp_project_lease *lease = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(project, &lease, NULL));
    TEST_ASSERT_EQUAL_INT(3, cli_atlas_add_exit(project, "blocked"));
    tp_project_lease_release(lease);
    TEST_ASSERT_EQUAL_INT(0, cli_atlas_add_exit(project, "accepted"));

    tp_project *loaded = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_load(project, &loaded, NULL));
    TEST_ASSERT_EQUAL_INT(2, loaded->atlas_count);
    TEST_ASSERT_EQUAL_STRING("accepted", tp_project_get_atlas(loaded, 1)->name);
    tp_project_destroy(loaded);
    (void)remove(project);
}

void test_cli_reads_do_not_require_writer_lease(void) {
    char project[TP_IDENTITY_PATH_MAX];
    join_path(project, sizeof project, g_dir, "cli-read-busy.ntpacker_project");
    tp_project *seed = tp_project_create();
    TEST_ASSERT_NOT_NULL(seed);
    tp_rng rng = tp_rng_os();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_promote_ids(seed, &rng, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(seed, project, NULL));
    tp_project_destroy(seed);

    tp_project_lease *lease = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_lease_acquire(project, &lease, NULL));
    TEST_ASSERT_EQUAL_INT(0, cli_read_exit("inspect", project, NULL));
    TEST_ASSERT_EQUAL_INT(0, cli_read_exit("validate", project, NULL));
    TEST_ASSERT_NOT_EQUAL(3, cli_read_exit("pack", project, "--dry-run"));
    TEST_ASSERT_EQUAL_INT(3, cli_atlas_add_exit(project, "blocked-read-owner"));
    tp_project_lease_release(lease);
    (void)remove(project);
}

static int child_main(const char *path) {
    tp_project_lease *lease = NULL;
    tp_status status = tp_project_lease_acquire(path, &lease, NULL);
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
    if (argc == 3 && strcmp(argv[1], "--child-acquire") == 0) {
        return child_main(argv[2]);
    }
    if (argc != 3) {
        (void)fprintf(stderr,
                      "usage: test_project_lease <scratch-dir> <ntpacker>\n");
        return 2;
    }
    g_self = argv[0];
    g_dir = argv[1];
    g_cli = argv[2];
    tp_mkdirs(g_dir);
    TEST_ASSERT_TRUE(tp_scan_is_dir(g_dir));

    UNITY_BEGIN();
    RUN_TEST(test_acquires_existing_project_and_releases_without_unlink);
    RUN_TEST(test_acquires_not_yet_created_project_destination);
    RUN_TEST(test_live_owner_returns_project_live_to_other_process);
    RUN_TEST(test_stale_lock_file_is_not_a_live_owner);
    RUN_TEST(test_canonical_alias_uses_same_lock_domain);
    RUN_TEST(test_cli_mutation_respects_process_project_lease);
    RUN_TEST(test_cli_reads_do_not_require_writer_lease);
    return UNITY_END();
}
