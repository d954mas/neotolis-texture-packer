#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "tp_proc_internal.h"
#include "unity.h"

#define TP_PROC_TEST_CHILD_ARG "__proc-job-transport-test"
#define TP_PROC_TEST_BUILD_ARG "__proc-job-transport-build"
#define TP_PROC_TEST_PROGRESS_ARG "__proc-job-transport-progress"
#define TP_PROC_TEST_TREE_ARG "__proc-job-transport-tree"
#define TP_PROC_TEST_GRANDCHILD_ARG "__proc-job-transport-grandchild"
#define TP_PROC_TEST_SLOW_STDIN_ARG "__proc-job-transport-slow-stdin"
#define TP_PROC_TEST_PROGRESS_BYTES (256U * 1024U)
#define TP_PROC_TEST_TREE_MARKER "tp_proc_tree_survived.tmp"

static const uint8_t g_request[] = {'J', 'O', 'B', '!'};

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
    sleep_ms(500U);
    FILE *marker = fopen(TP_PROC_TEST_TREE_MARKER, "wb");
    if (!marker) {
        return 4;
    }
    const bool wrote = fwrite("survived", 1U, 8U, marker) == 8U;
    const bool closed = fclose(marker) == 0;
    return wrote && closed ? 0 : 4;
}

static int run_tree_child(void) {
    char self[4096];
    if (!tp_proc_self_path(self, sizeof self)) {
        return 2;
    }
    tp_proc *grandchild = tp_proc_spawn(self, TP_PROC_TEST_GRANDCHILD_ARG, NULL);
    if (!grandchild) {
        return 2;
    }
    if (fwrite("ready", 1U, 5U, stdout) != 5U || fflush(stdout) != 0) {
        tp_proc_destroy(grandchild);
        return 3;
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
}

void test_nonblocking_stdin_pump_reports_backpressure_and_completes(void) {
    char self[4096];
    TEST_ASSERT_TRUE(tp_proc_self_path(self, sizeof self));
    tp_proc *proc = tp_proc_spawn(self, TP_PROC_TEST_SLOW_STDIN_ARG, NULL);
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

int main(int argc, char **argv) {
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
        return run_tree_child();
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
    RUN_TEST(test_nonblocking_stdin_pump_reports_backpressure_and_completes);
    return UNITY_END();
}
