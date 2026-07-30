#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h"
#include "tp_job_worker_process_internal.h"
#include "tp_proc_internal.h"
#include "tp_fs_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_project_mutation_internal.h"
#include "unity.h"

#define TP_PROC_TEST_CHILD_ARG "__proc-job-transport-test"
#define TP_PROC_TEST_BUILD_ARG "__proc-job-transport-build"
#define TP_PROC_TEST_PROGRESS_ARG "__proc-job-transport-progress"
#define TP_PROC_TEST_TREE_ARG "__proc-job-transport-tree"
#define TP_PROC_TEST_REAPED_TREE_ARG "__proc-job-transport-reaped-tree"
#define TP_PROC_TEST_GRANDCHILD_ARG "__proc-job-transport-grandchild"
#define TP_PROC_TEST_SLOW_STDIN_ARG "__proc-job-transport-slow-stdin"
#define TP_PROC_TEST_PROGRESS_BYTES (256U * 1024U)
#define TP_PROC_TEST_TREE_MARKER "tp_proc_tree_survived.tmp"
#define TP_PROC_TEST_TREE_READY_MARKER "tp_proc_tree_ready.tmp"

static const uint8_t g_request[] = {'J', 'O', 'B', '!'};
static const uint8_t g_png_4x4[] = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
    0x00U, 0x00U, 0x00U, 0x0DU, 0x49U, 0x48U, 0x44U, 0x52U,
    0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x04U,
    0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0xA9U, 0xF1U, 0x9EU,
    0x7EU, 0x00U, 0x00U, 0x00U, 0x33U, 0x49U, 0x44U, 0x41U,
    0x54U, 0x78U, 0xDAU, 0x15U, 0xC8U, 0xA1U, 0x11U, 0x00U,
    0x30U, 0x08U, 0x04U, 0x41U, 0x34U, 0x3AU, 0x95U, 0x44U,
    0x53U, 0x09U, 0x9AU, 0x42U, 0x98U, 0xD7U, 0xF4U, 0x4CU,
    0x2EU, 0x62U, 0xCDU, 0x9AU, 0x9FU, 0xDEU, 0x8BU, 0x84U,
    0x60U, 0x1EU, 0x04U, 0x12U, 0x8AU, 0x1FU, 0x45U, 0x20U,
    0xA1U, 0xFAU, 0x31U, 0x04U, 0x12U, 0x9AU, 0xDEU, 0x07U,
    0x18U, 0xA8U, 0x21U, 0x51U, 0x4CU, 0xC2U, 0x35U, 0x74U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4EU, 0x44U,
    0xAEU, 0x42U, 0x60U, 0x82U,
};

void setUp(void) {}
void tearDown(void) {}

static void sleep_ms(unsigned milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    const struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L
    };
    (void)nanosleep(&delay, NULL);
#endif
}

static void sleep_one_ms(void) {
    sleep_ms(1U);
}

static void remove_tree(const char *path) {
    tp_fs_dir *dir = tp_fs_dir_open(path);
    if (dir) {
        tp_fs_dir_entry entry;
        while (tp_fs_dir_next(dir, &entry) == TP_FS_DIR_ENTRY) {
            char child[2048];
            const int length = snprintf(
                child, sizeof child, "%s/%s", path, entry.name);
            if (length < 0 || (size_t)length >= sizeof child) {
                continue;
            }
            if (entry.info.kind == TP_FS_KIND_DIRECTORY &&
                !entry.info.reparse) {
                remove_tree(child);
            } else {
                (void)tp_fs_remove_file(child);
            }
        }
        tp_fs_dir_close(dir);
    }
    (void)tp_fs_remove_dir(path);
}

static void export_scratch_dir(char *out, size_t capacity) {
    char cwd[1024];
#if defined(_WIN32)
    TEST_ASSERT_NOT_NULL(_getcwd(cwd, (int)sizeof cwd));
#else
    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof cwd));
#endif
    for (char *cursor = cwd; *cursor; ++cursor) {
        if (*cursor == '\\') {
            *cursor = '/';
        }
    }
    const int length = snprintf(
        out, capacity, "%s/tp_proc_real_export", cwd);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < capacity);
}

static int deterministic_fill(
    void *context, uint8_t *out, size_t length) {
    uint8_t *seed = context;
    for (size_t i = 0U; i < length; ++i) {
        out[i] = (uint8_t)(*seed + (uint8_t)i);
    }
    *seed = (uint8_t)(*seed + 31U);
    return (int)length;
}

static bool read_exact_stdin(void *out, size_t size) {
    uint8_t *bytes = (uint8_t *)out;
    size_t total = 0U;
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
    while (total < size) {
        const unsigned chunk =
            size - total > (size_t)INT_MAX ? (unsigned)INT_MAX : (unsigned)(size - total);
        const int got = _read(_fileno(stdin), bytes + total, chunk);
        if (got <= 0) {
            return false;
        }
        total += (size_t)got;
    }
#else
    while (total < size) {
        const ssize_t got = read(STDIN_FILENO, bytes + total, size - total);
        if (got <= 0) {
            return false;
        }
        total += (size_t)got;
    }
#endif
    return true;
}

static int run_control_child(void) {
    uint8_t request[sizeof g_request];
    if (!read_exact_stdin(request, sizeof request) ||
        memcmp(request, g_request, sizeof request) != 0) {
        return 2;
    }
    for (;;) {
        const tp_proc_stdin_event event = tp_proc_child_poll_stdin();
        if (event == TP_PROC_STDIN_EVENT_NONE) {
            sleep_one_ms();
            continue;
        }
        const char *reply = event == TP_PROC_STDIN_EVENT_CANCEL
                                ? "cancel"
                                : event == TP_PROC_STDIN_EVENT_CLOSED ? "closed" : "error";
        return fwrite(reply, 1U, strlen(reply), stdout) == strlen(reply) ? 0 : 3;
    }
}

static int run_build_child(void) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
#endif
    uint8_t request[sizeof g_request];
    const size_t got = fread(request, 1U, sizeof request, stdin);
    const int trailing = fgetc(stdin);
    if (got != sizeof request || trailing != EOF) {
        return 2;
    }
    return fwrite(request, 1U, got, stdout) == got ? 0 : 3;
}

static int run_progress_child(void) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    uint8_t block[4096];
    for (size_t i = 0U; i < sizeof block; i++) {
        block[i] = (uint8_t)i;
    }
    for (size_t off = 0U; off < TP_PROC_TEST_PROGRESS_BYTES; off += sizeof block) {
        if (fwrite(block, 1U, sizeof block, stdout) != sizeof block) {
            return 3;
        }
    }
    return fflush(stdout) == 0 ? 0 : 3;
}

static int run_grandchild(void) {
    FILE *ready = fopen(TP_PROC_TEST_TREE_READY_MARKER, "wb");
    if (!ready) {
        return 4;
    }
    const bool ready_wrote = fwrite("ready", 1U, 5U, ready) == 5U;
    const bool ready_closed = fclose(ready) == 0;
    if (!ready_wrote || !ready_closed) {
        return 4;
    }
    sleep_ms(500U);
    FILE *marker = fopen(TP_PROC_TEST_TREE_MARKER, "wb");
    if (!marker) {
        return 4;
    }
    const bool wrote = fwrite("survived", 1U, 8U, marker) == 8U;
    const bool closed = fclose(marker) == 0;
    return wrote && closed ? 0 : 4;
}

static int run_tree_child(bool exit_after_ready) {
    char self[4096];
    if (!tp_proc_self_path(self, sizeof self)) {
        return 2;
    }
    tp_proc *grandchild = NULL;
#if !defined(_WIN32)
    if (exit_after_ready) {
        const pid_t pid = fork();
        if (pid < 0) {
            return 2;
        }
        if (pid == 0) {
            _exit(run_grandchild());
        }
    } else
#endif
    {
        grandchild =
            tp_proc_spawn(self, TP_PROC_TEST_GRANDCHILD_ARG, NULL);
        if (!grandchild) {
            return 2;
        }
    }
    bool grandchild_ready = false;
    for (int i = 0; i < 2000 && !grandchild_ready; i++) {
        FILE *ready = fopen(TP_PROC_TEST_TREE_READY_MARKER, "rb");
        if (ready) {
            grandchild_ready = true;
            (void)fclose(ready);
        } else {
            sleep_one_ms();
        }
    }
    if (!grandchild_ready) {
        if (grandchild) {
            tp_proc_destroy(grandchild);
        }
        return 3;
    }
    if (fwrite("ready", 1U, 5U, stdout) != 5U || fflush(stdout) != 0) {
        if (grandchild) {
            tp_proc_destroy(grandchild);
        }
        return 3;
    }
    if (exit_after_ready) {
        return 0;
    }
    for (;;) {
        sleep_one_ms();
    }
}

static int run_slow_stdin_child(void) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
#endif
    sleep_ms(200U);
    uint8_t block[4096];
    size_t total = 0U;
    for (;;) {
        const size_t got = fread(block, 1U, sizeof block, stdin);
        total += got;
        if (got == 0U) {
            break;
        }
    }
    const char *reply = total == TP_PROC_TEST_PROGRESS_BYTES ? "written" : "short";
    return fwrite(reply, 1U, strlen(reply), stdout) == strlen(reply) ? 0 : 3;
}

static bool wait_finished(tp_proc *proc, tp_proc_result *out) {
    bool finished = false;
    for (int i = 0; i < 2000 && !finished; i++) {
        if (!tp_proc_wait_slice(proc, 1, out, &finished)) {
            return false;
        }
    }
    return finished;
}

static void read_reply(tp_proc *proc, uint8_t *reply, size_t cap, size_t *out_len) {
    bool eof = false;
    TEST_ASSERT_TRUE(tp_proc_read_stdout(proc, reply, cap, out_len, &eof));
    TEST_ASSERT_TRUE(eof);
}

void test_keep_open_allows_later_cancel_signal(void) {
    char self[4096];
    TEST_ASSERT_TRUE(tp_proc_self_path(self, sizeof self));
    tp_proc *proc = tp_proc_spawn(self, TP_PROC_TEST_CHILD_ARG, NULL);
    TEST_ASSERT_NOT_NULL(proc);
    TEST_ASSERT_TRUE(tp_proc_write_stdin_keep_open(proc, g_request, sizeof g_request));

    bool finished = true;
    TEST_ASSERT_TRUE(tp_proc_wait_slice(proc, 0, NULL, &finished));
    TEST_ASSERT_FALSE_MESSAGE(finished, "child observed EOF before explicit close");
    TEST_ASSERT_TRUE(tp_proc_send_cancel(proc));
    TEST_ASSERT_TRUE(tp_proc_close_stdin(proc));

    tp_proc_result result;
    TEST_ASSERT_TRUE(wait_finished(proc, &result));
    TEST_ASSERT_EQUAL_INT(TP_PROC_END_EXITED, result.how);
    TEST_ASSERT_EQUAL_INT(0, result.code);
    uint8_t reply[16];
    size_t reply_len = 0U;
    read_reply(proc, reply, sizeof reply, &reply_len);
    TEST_ASSERT_EQUAL_size_t(6U, reply_len);
    TEST_ASSERT_EQUAL_MEMORY("cancel", reply, reply_len);
    tp_proc_destroy(proc);
}

void test_explicit_close_is_observed_without_blocking(void) {
    char self[4096];
    TEST_ASSERT_TRUE(tp_proc_self_path(self, sizeof self));
    tp_proc *proc = tp_proc_spawn(self, TP_PROC_TEST_CHILD_ARG, NULL);
    TEST_ASSERT_NOT_NULL(proc);
    TEST_ASSERT_TRUE(tp_proc_write_stdin_keep_open(proc, g_request, sizeof g_request));
    TEST_ASSERT_TRUE(tp_proc_close_stdin(proc));

    tp_proc_result result;
    TEST_ASSERT_TRUE(wait_finished(proc, &result));
    TEST_ASSERT_EQUAL_INT(0, result.code);
    uint8_t reply[16];
    size_t reply_len = 0U;
    read_reply(proc, reply, sizeof reply, &reply_len);
    TEST_ASSERT_EQUAL_size_t(6U, reply_len);
    TEST_ASSERT_EQUAL_MEMORY("closed", reply, reply_len);
    tp_proc_destroy(proc);
}

void test_build_worker_write_closes_stdin_after_request(void) {
    char self[4096];
    TEST_ASSERT_TRUE(tp_proc_self_path(self, sizeof self));
    tp_proc *proc = tp_proc_spawn(self, TP_PROC_TEST_BUILD_ARG, NULL);
    TEST_ASSERT_NOT_NULL(proc);
    TEST_ASSERT_TRUE(tp_proc_write_stdin(proc, g_request, sizeof g_request));

    tp_proc_result result;
    TEST_ASSERT_TRUE(wait_finished(proc, &result));
    TEST_ASSERT_EQUAL_INT(0, result.code);
    uint8_t reply[16];
    size_t reply_len = 0U;
    read_reply(proc, reply, sizeof reply, &reply_len);
    TEST_ASSERT_EQUAL_size_t(sizeof g_request, reply_len);
    TEST_ASSERT_EQUAL_MEMORY(g_request, reply, reply_len);
    tp_proc_destroy(proc);
}

void test_nonblocking_stdout_pump_prevents_pipe_backpressure(void) {
    char self[4096];
    TEST_ASSERT_TRUE(tp_proc_self_path(self, sizeof self));
    tp_proc *proc = tp_proc_spawn(self, TP_PROC_TEST_PROGRESS_ARG, NULL);
    TEST_ASSERT_NOT_NULL(proc);
    TEST_ASSERT_TRUE(tp_proc_write_stdin(proc, NULL, 0U));

    uint8_t *reply = (uint8_t *)malloc(TP_PROC_TEST_PROGRESS_BYTES);
    TEST_ASSERT_NOT_NULL(reply);
    size_t total = 0U;
    bool eof = false;
    bool finished = false;
    tp_proc_result result;
    for (int i = 0; i < 5000 && (!finished || !eof); i++) {
        size_t got = 0U;
        TEST_ASSERT_TRUE(tp_proc_try_read_stdout(
            proc, reply + total, TP_PROC_TEST_PROGRESS_BYTES - total, &got, &eof));
        total += got;
        TEST_ASSERT_LESS_OR_EQUAL_size_t(TP_PROC_TEST_PROGRESS_BYTES, total);
        if (!finished) {
            TEST_ASSERT_TRUE(tp_proc_wait_slice(proc, 0, &result, &finished));
        }
        if (got == 0U && !eof) {
            sleep_one_ms();
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(finished, "large-output child remained blocked on stdout");
    TEST_ASSERT_TRUE_MESSAGE(eof, "stdout pump did not observe EOF");
    TEST_ASSERT_EQUAL_INT(0, result.code);
    TEST_ASSERT_EQUAL_size_t(TP_PROC_TEST_PROGRESS_BYTES, total);
    for (size_t i = 0U; i < total; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, reply[i]);
    }
    free(reply);
    tp_proc_destroy(proc);
}

void test_owned_tree_kill_reaches_nested_worker(void) {
    (void)remove(TP_PROC_TEST_TREE_MARKER);
    (void)remove(TP_PROC_TEST_TREE_READY_MARKER);
    char self[4096];
    TEST_ASSERT_TRUE(tp_proc_self_path(self, sizeof self));
    tp_proc *proc = tp_proc_spawn_owned_tree(self, TP_PROC_TEST_TREE_ARG, NULL);
    TEST_ASSERT_NOT_NULL(proc);
    TEST_ASSERT_TRUE(tp_proc_write_stdin(proc, NULL, 0U));

    uint8_t ready[5];
    size_t total = 0U;
    for (int i = 0; i < 2000 && total < sizeof ready; i++) {
        size_t got = 0U;
        bool eof = false;
        TEST_ASSERT_TRUE(tp_proc_try_read_stdout(
            proc, ready + total, sizeof ready - total, &got, &eof));
        TEST_ASSERT_FALSE_MESSAGE(eof, "outer worker exited before spawning nested worker");
        total += got;
        if (got == 0U) {
            sleep_one_ms();
        }
    }
    TEST_ASSERT_EQUAL_size_t(sizeof ready, total);
    TEST_ASSERT_EQUAL_MEMORY("ready", ready, sizeof ready);
    tp_proc_kill(proc);
    tp_proc_destroy(proc);

    sleep_ms(750U);
    FILE *marker = fopen(TP_PROC_TEST_TREE_MARKER, "rb");
    TEST_ASSERT_NULL_MESSAGE(marker, "nested worker escaped outer process-tree kill");
    if (marker) {
        (void)fclose(marker);
    }
    (void)remove(TP_PROC_TEST_TREE_MARKER);
    (void)remove(TP_PROC_TEST_TREE_READY_MARKER);
}

#if !defined(_WIN32)
void test_reaped_tree_leader_cleans_nested_worker_before_reap(void) {
    (void)remove(TP_PROC_TEST_TREE_MARKER);
    (void)remove(TP_PROC_TEST_TREE_READY_MARKER);
    char self[4096];
    TEST_ASSERT_TRUE(tp_proc_self_path(self, sizeof self));
    tp_proc *proc =
        tp_proc_spawn_owned_tree(self, TP_PROC_TEST_REAPED_TREE_ARG, NULL);
    TEST_ASSERT_NOT_NULL(proc);
    TEST_ASSERT_TRUE(tp_proc_write_stdin(proc, NULL, 0U));

    uint8_t ready[5];
    size_t total = 0U;
    for (int i = 0; i < 2000 && total < sizeof ready; i++) {
        size_t got = 0U;
        bool eof = false;
        TEST_ASSERT_TRUE(tp_proc_try_read_stdout(
            proc, ready + total, sizeof ready - total, &got, &eof));
        total += got;
        if (got == 0U) {
            sleep_one_ms();
        }
    }
    TEST_ASSERT_EQUAL_size_t(sizeof ready, total);
    TEST_ASSERT_EQUAL_MEMORY("ready", ready, sizeof ready);

    tp_proc_result result;
    TEST_ASSERT_TRUE(wait_finished(proc, &result));
    TEST_ASSERT_EQUAL_INT(TP_PROC_END_EXITED, result.how);
    TEST_ASSERT_EQUAL_INT(0, result.code);
    tp_proc_kill(proc);
    tp_proc_destroy(proc);

    sleep_ms(750U);
    FILE *marker = fopen(TP_PROC_TEST_TREE_MARKER, "rb");
    TEST_ASSERT_NULL_MESSAGE(
        marker, "nested worker escaped cleanup before leader reap");
    if (marker) {
        (void)fclose(marker);
    }
    (void)remove(TP_PROC_TEST_TREE_MARKER);
    (void)remove(TP_PROC_TEST_TREE_READY_MARKER);
}
#endif

/* Owned-tree spawn on purpose: that is the substrate the non-blocking stdin
 * writer belongs to (the outer job worker pumps its request from the UI thread).
 * A plain tp_proc_spawn keeps anonymous pipes, whose writes are synchronous by
 * construction and therefore have no backpressure to report. */
void test_nonblocking_stdin_pump_reports_backpressure_and_completes(void) {
    char self[4096];
    TEST_ASSERT_TRUE(tp_proc_self_path(self, sizeof self));
    tp_proc *proc =
        tp_proc_spawn_owned_tree(self, TP_PROC_TEST_SLOW_STDIN_ARG, NULL);
    TEST_ASSERT_NOT_NULL(proc);
    uint8_t *request = (uint8_t *)malloc(TP_PROC_TEST_PROGRESS_BYTES);
    TEST_ASSERT_NOT_NULL(request);
    memset(request, 0x5a, TP_PROC_TEST_PROGRESS_BYTES);

    size_t total = 0U;
    bool saw_backpressure = false;
    for (int i = 0; i < 5000 && total < TP_PROC_TEST_PROGRESS_BYTES; i++) {
        size_t consumed = 0U;
        bool would_block = false;
        TEST_ASSERT_TRUE(tp_proc_try_write_stdin(
            proc, request + total, TP_PROC_TEST_PROGRESS_BYTES - total,
            &consumed, &would_block));
        total += consumed;
        saw_backpressure = saw_backpressure || would_block;
        if (would_block) {
            sleep_one_ms();
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_backpressure, "large request never exercised pipe backpressure");
    TEST_ASSERT_EQUAL_size_t(TP_PROC_TEST_PROGRESS_BYTES, total);
    TEST_ASSERT_TRUE(tp_proc_close_stdin(proc));

    tp_proc_result result;
    TEST_ASSERT_TRUE(wait_finished(proc, &result));
    TEST_ASSERT_EQUAL_INT(0, result.code);
    uint8_t reply[16];
    size_t reply_len = 0U;
    read_reply(proc, reply, sizeof reply, &reply_len);
    TEST_ASSERT_EQUAL_size_t(7U, reply_len);
    TEST_ASSERT_EQUAL_MEMORY("written", reply, reply_len);
    free(request);
    tp_proc_destroy(proc);
}

void test_real_export_reports_committed_files_before_later_writer_failure(void) {
    char root[1200];
    char source[1300];
    char work[1300];
    char blocked[1300];
    char published_json[1400];
    char published_png[1400];
    export_scratch_dir(root, sizeof root);
    TEST_ASSERT_TRUE(
        snprintf(source, sizeof source, "%s/sprite.png", root) > 0);
    TEST_ASSERT_TRUE(
        snprintf(work, sizeof work, "%s/work", root) > 0);
    TEST_ASSERT_TRUE(
        snprintf(blocked, sizeof blocked, "%s/blocked", root) > 0);
    TEST_ASSERT_TRUE(
        snprintf(published_json, sizeof published_json,
                 "%s/published/atlas.json", root) > 0);
    TEST_ASSERT_TRUE(
        snprintf(published_png, sizeof published_png,
                 "%s/published/atlas-0.png", root) > 0);
    remove_tree(root);
    tp_mkdirs(root);
    tp_mkdirs(work);
    TEST_ASSERT_TRUE(tp_fs_write_file(
        source, g_png_4x4, sizeof g_png_4x4));
    TEST_ASSERT_TRUE(tp_fs_write_file(
        blocked, "not-a-directory", 15U));

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = tp_project_get_atlas(project, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source_kind(
            atlas, "sprite.png", TP_SOURCE_KIND_FILE));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_target(
            atlas, TP_EXPORTER_ID_JSON_NEOTOLIS,
            "published/atlas", NULL));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_target(
            atlas, TP_EXPORTER_ID_JSON_NEOTOLIS,
            "blocked/atlas", NULL));
    uint8_t seed = 7U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_assign_missing_ids(project, &rng, &error),
        error.msg);
    char *project_json = NULL;
    size_t project_json_len = 0U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_save_buffer(
            project, &project_json, &project_json_len, &error),
        error.msg);
    tp_project_destroy(project);

    const tp_job_worker_proto_request request = {
        .kind = TP_SESSION_JOB_EXPORT,
        .session_instance_generation = 12U,
        .request_id = 34U,
        .host_pid = 4321U,
        .project_json = (const uint8_t *)project_json,
        .project_json_len = project_json_len,
        .project_dir = root,
        .work_dir = work,
        .preview_exporter_id = "",
    };
    tp_job_worker_process *process = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_job_worker_process_start(&request, &process, &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(process);
    free(project_json);

    for (int elapsed = 0;
         elapsed < 5000 &&
         !tp_job_worker_process_terminal(process);
         ++elapsed) {
        tp_job_worker_process_pump(process);
        sleep_one_ms();
    }
    TEST_ASSERT_TRUE_MESSAGE(
        tp_job_worker_process_terminal(process),
        "real Export worker did not reach terminal state");
    const tp_job_worker_proto_response *response =
        tp_job_worker_process_response(process);
    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);
    TEST_ASSERT_EQUAL_INT(1, response->export_result.targets);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(
        2, response->export_result.files);
    TEST_ASSERT_EQUAL_INT(1, response->export_result.atlases_failed);
    TEST_ASSERT_TRUE(response->export_result.partial_publication);
    /* The RUN is partial -- one atlas target published and one failed -- but the
     * failed target's publication is not UNCERTAIN. Its output directory is a
     * regular file, so the staged-set publication cannot even create its private
     * staging dir and the writer is never attempted; nothing of that target was
     * written, and the report must not suggest artifacts might be lying around.
     * publication_uncertain is now reserved for a target whose writer really did
     * run (TP_EXPORT_WRITER_FAILED). */
    TEST_ASSERT_FALSE(response->export_result.publication_uncertain);
    TEST_ASSERT_NOT_EQUAL(
        '\0', response->export_result.first_error[0]);
    TEST_ASSERT_TRUE(tp_fs_exists(published_json));
    TEST_ASSERT_TRUE(tp_fs_exists(published_png));
    tp_job_worker_process_destroy(process);
    remove_tree(root);
}

/* Drive one real worker request to its terminal frame. */
static const tp_job_worker_proto_response *run_worker_request(
    const tp_job_worker_proto_request *request,
    tp_job_worker_process **out_process) {
    tp_error error = {{0}};
    tp_job_worker_process *process = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_job_worker_process_start(request, &process, &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(process);
    for (int elapsed = 0;
         elapsed < 20000 && !tp_job_worker_process_terminal(process);
         ++elapsed) {
        tp_job_worker_process_pump(process);
        sleep_one_ms();
    }
    TEST_ASSERT_TRUE_MESSAGE(
        tp_job_worker_process_terminal(process),
        "real worker did not reach terminal state");
    const tp_job_worker_proto_response *response =
        tp_job_worker_process_response(process);
    TEST_ASSERT_NOT_NULL(response);
    *out_process = process;
    return response;
}

static char *save_project_json(tp_project *project, size_t *out_length) {
    /* Advances across calls: re-saving a project after adding a record must not
     * mint the same structural id twice. */
    static uint8_t seed = 7U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_assign_missing_ids(project, &rng, &error), error.msg);
    char *json = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_save_buffer(project, &json, out_length, &error),
        error.msg);
    return json;
}

/* End-to-end freshness: a Pack the project did not change under reports
 * CURRENT/MATCH with pack_input_hash == current_pack_input_hash, and the hash is
 * a function of the pack input -- identical inputs repeat it, an added source
 * moves it. Also pins the artifact contract: the `.ntpack` is published into a
 * private per-request directory and its announced size is the file's size. */
void test_real_pack_reports_current_freshness_and_input_bound_hash(void) {
    char root[1200];
    char source[1300];
    char second_source[1300];
    char work[1300];
    char cwd[1024];
#if defined(_WIN32)
    TEST_ASSERT_NOT_NULL(_getcwd(cwd, (int)sizeof cwd));
#else
    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof cwd));
#endif
    for (char *cursor = cwd; *cursor; ++cursor) {
        if (*cursor == '\\') {
            *cursor = '/';
        }
    }
    TEST_ASSERT_TRUE(
        snprintf(root, sizeof root, "%s/tp_proc_real_pack", cwd) > 0);
    TEST_ASSERT_TRUE(
        snprintf(source, sizeof source, "%s/sprite.png", root) > 0);
    TEST_ASSERT_TRUE(
        snprintf(second_source, sizeof second_source, "%s/sprite2.png",
                 root) > 0);
    TEST_ASSERT_TRUE(snprintf(work, sizeof work, "%s/work", root) > 0);
    remove_tree(root);
    tp_mkdirs(root);
    tp_mkdirs(work);
    TEST_ASSERT_TRUE(
        tp_fs_write_file(source, g_png_4x4, sizeof g_png_4x4));

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = tp_project_get_atlas(project, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source_kind(
            atlas, "sprite.png", TP_SOURCE_KIND_FILE));
    size_t json_length = 0U;
    char *json = save_project_json(project, &json_length);
    const tp_id128 atlas_id = tp_project_get_atlas(project, 0)->id;

    tp_job_worker_proto_request request = {
        .kind = TP_SESSION_JOB_PACK,
        .session_instance_generation = 5U,
        .request_id = 61U,
        .host_pid = 4321U,
        .project_json = (const uint8_t *)json,
        .project_json_len = json_length,
        .atlas_id = atlas_id,
        .project_dir = root,
        .work_dir = work,
        .preview_exporter_id = "",
        .input_token = {.model_generation = 3U, .source_generation = 4U},
    };
    tp_job_worker_process *process = NULL;
    const tp_job_worker_proto_response *response =
        run_worker_request(&request, &process);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_SESSION_JOB_SUCCEEDED, response->state, response->error.msg);
    TEST_ASSERT_EQUAL_INT(TP_PACK_FRESHNESS_CURRENT,
                          response->pack.freshness);
    TEST_ASSERT_EQUAL_INT(TP_PACK_FRESHNESS_MATCH,
                          response->pack.freshness_reason);
    TEST_ASSERT_FALSE(tp_id128_is_nil(response->pack.pack_input_hash));
    TEST_ASSERT_TRUE(tp_id128_eq(response->pack.pack_input_hash,
                                 response->pack.current_pack_input_hash));
    TEST_ASSERT_EQUAL_UINT64(
        3U, response->pack.input_token_at_start.model_generation);
    /* Private per-request directory, exact announced size, real file. */
    TEST_ASSERT_NOT_NULL(response->pack.artifact_path);
    TEST_ASSERT_NOT_NULL(strstr(response->pack.artifact_path, "/req-"));
    tp_fs_info info;
    TEST_ASSERT_TRUE(tp_fs_stat(response->pack.artifact_path, &info));
    TEST_ASSERT_EQUAL_UINT64(response->pack.artifact_size, info.size);
    const tp_id128 first_hash = response->pack.pack_input_hash;
    char first_path[1400];
    TEST_ASSERT_TRUE(snprintf(first_path, sizeof first_path, "%s",
                              response->pack.artifact_path) > 0);
    tp_job_worker_process_destroy(process);

    /* Same inputs, new request: same hash, a DIFFERENT private directory. */
    request.request_id = 62U;
    response = run_worker_request(&request, &process);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_SESSION_JOB_SUCCEEDED, response->state, response->error.msg);
    TEST_ASSERT_TRUE(
        tp_id128_eq(first_hash, response->pack.pack_input_hash));
    TEST_ASSERT_TRUE(
        strcmp(first_path, response->pack.artifact_path) != 0);
    tp_job_worker_process_destroy(process);
    free(json);

    /* Pack input changed: the hash must move with it. */
    TEST_ASSERT_TRUE(
        tp_fs_write_file(second_source, g_png_4x4, sizeof g_png_4x4));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source_kind(
            tp_project_get_atlas(project, 0), "sprite2.png",
            TP_SOURCE_KIND_FILE));
    json = save_project_json(project, &json_length);
    request.project_json = (const uint8_t *)json;
    request.project_json_len = json_length;
    request.request_id = 63U;
    response = run_worker_request(&request, &process);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_SESSION_JOB_SUCCEEDED, response->state, response->error.msg);
    TEST_ASSERT_EQUAL_INT(TP_PACK_FRESHNESS_CURRENT,
                          response->pack.freshness);
    TEST_ASSERT_FALSE(
        tp_id128_eq(first_hash, response->pack.pack_input_hash));
    tp_job_worker_process_destroy(process);

    free(json);
    tp_project_destroy(project);
    remove_tree(root);
}

/* An atlas with no usable images is SKIPPED, not failed: the Export succeeds,
 * counts it as skipped, and writes nothing for it. */
void test_real_export_skips_an_empty_atlas_and_still_succeeds(void) {
    char root[1200];
    char source[1300];
    char work[1300];
    char packed_json[1400];
    char empty_json[1400];
    export_scratch_dir(root, sizeof root);
    TEST_ASSERT_TRUE(
        snprintf(source, sizeof source, "%s/sprite.png", root) > 0);
    TEST_ASSERT_TRUE(snprintf(work, sizeof work, "%s/work", root) > 0);
    TEST_ASSERT_TRUE(
        snprintf(packed_json, sizeof packed_json,
                 "%s/published/packed.json", root) > 0);
    TEST_ASSERT_TRUE(
        snprintf(empty_json, sizeof empty_json, "%s/published/empty.json",
                 root) > 0);
    remove_tree(root);
    tp_mkdirs(root);
    tp_mkdirs(work);
    TEST_ASSERT_TRUE(
        tp_fs_write_file(source, g_png_4x4, sizeof g_png_4x4));

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *packed = tp_project_get_atlas(project, 0);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source_kind(
            packed, "sprite.png", TP_SOURCE_KIND_FILE));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_target(
            packed, TP_EXPORTER_ID_JSON_NEOTOLIS, "published/packed",
            NULL));
    int empty_index = -1;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_add_atlas(project, "empty", &empty_index));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_target(
            tp_project_get_atlas(project, empty_index),
            TP_EXPORTER_ID_JSON_NEOTOLIS, "published/empty", NULL));
    size_t json_length = 0U;
    char *json = save_project_json(project, &json_length);
    tp_project_destroy(project);

    const tp_job_worker_proto_request request = {
        .kind = TP_SESSION_JOB_EXPORT,
        .session_instance_generation = 12U,
        .request_id = 71U,
        .host_pid = 4321U,
        .project_json = (const uint8_t *)json,
        .project_json_len = json_length,
        .project_dir = root,
        .work_dir = work,
        .preview_exporter_id = "",
    };
    tp_job_worker_process *process = NULL;
    const tp_job_worker_proto_response *response =
        run_worker_request(&request, &process);
    free(json);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_SESSION_JOB_SUCCEEDED, response->state, response->error.msg);
    TEST_ASSERT_EQUAL_INT(1, response->export_result.atlases_ok);
    TEST_ASSERT_EQUAL_INT(1, response->export_result.atlases_skipped);
    TEST_ASSERT_EQUAL_INT(0, response->export_result.atlases_failed);
    TEST_ASSERT_EQUAL_INT(1, response->export_result.targets);
    TEST_ASSERT_FALSE(response->export_result.partial_publication);
    TEST_ASSERT_TRUE(tp_fs_exists(packed_json));
    TEST_ASSERT_FALSE_MESSAGE(
        tp_fs_exists(empty_json),
        "a skipped empty atlas must not publish an export file");
    tp_job_worker_process_destroy(process);
    remove_tree(root);
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    if (argc >= 2 && strcmp(argv[1], TP_PROC_TEST_CHILD_ARG) == 0) {
        return run_control_child();
    }
    if (argc >= 2 && strcmp(argv[1], TP_PROC_TEST_BUILD_ARG) == 0) {
        return run_build_child();
    }
    if (argc >= 2 && strcmp(argv[1], TP_PROC_TEST_PROGRESS_ARG) == 0) {
        return run_progress_child();
    }
    if (argc >= 2 && strcmp(argv[1], TP_PROC_TEST_TREE_ARG) == 0) {
        return run_tree_child(false);
    }
    if (argc >= 2 && strcmp(argv[1], TP_PROC_TEST_REAPED_TREE_ARG) == 0) {
        return run_tree_child(true);
    }
    if (argc >= 2 && strcmp(argv[1], TP_PROC_TEST_GRANDCHILD_ARG) == 0) {
        return run_grandchild();
    }
    if (argc >= 2 && strcmp(argv[1], TP_PROC_TEST_SLOW_STDIN_ARG) == 0) {
        return run_slow_stdin_child();
    }

    UNITY_BEGIN();
    RUN_TEST(test_keep_open_allows_later_cancel_signal);
    RUN_TEST(test_explicit_close_is_observed_without_blocking);
    RUN_TEST(test_build_worker_write_closes_stdin_after_request);
    RUN_TEST(test_nonblocking_stdout_pump_prevents_pipe_backpressure);
    RUN_TEST(test_owned_tree_kill_reaches_nested_worker);
#if !defined(_WIN32)
    RUN_TEST(test_reaped_tree_leader_cleans_nested_worker_before_reap);
#endif
    RUN_TEST(test_nonblocking_stdin_pump_reports_backpressure_and_completes);
    RUN_TEST(
        test_real_export_reports_committed_files_before_later_writer_failure);
    RUN_TEST(test_real_export_skips_an_empty_atlas_and_still_succeeds);
    RUN_TEST(
        test_real_pack_reports_current_freshness_and_input_bound_hash);
    return UNITY_END();
}
