#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Compile the production owner directly so this death probe exercises the
 * Release assertion mode of the exact implementation under test. tp_core is
 * linked only for tp_project_destroy; its archive copy of this TU is not pulled. */
#include "../src/tp_project_generation.c"

static const char child_started_token[] = "tp-generation-release-assert-started\n";

enum child_probe_result {
    CHILD_PROBE_LAUNCH_FAILED,
    CHILD_PROBE_NOT_STARTED,
    CHILD_PROBE_EXITED_ZERO,
    CHILD_PROBE_TERMINATED_NONZERO,
    CHILD_PROBE_INTERNAL_ERROR,
};

static FILE *open_sentinel_file(const char *path, const char *mode) {
#ifdef _WIN32
    FILE *file = NULL;
    return fopen_s(&file, path, mode) == 0 ? file : NULL;
#else
    return fopen(path, mode);
#endif
}

static bool remove_sentinel(const char *path) {
    if (remove(path) == 0) {
        return true;
    }
    return errno == ENOENT;
}

static bool sentinel_was_written(const char *path) {
    char contents[sizeof child_started_token];
    FILE *file = open_sentinel_file(path, "rb");
    if (file == NULL) {
        return false;
    }

    const size_t size = fread(contents, 1, sizeof contents, file);
    const bool read_ok = !ferror(file);
    const bool closed = fclose(file) == 0;
    return read_ok && closed && size == sizeof child_started_token - 1U &&
           memcmp(contents, child_started_token, sizeof child_started_token - 1U) == 0;
}

static int run_invalid_release_child(const char *sentinel_path) {
    FILE *sentinel = open_sentinel_file(sentinel_path, "wb");
    if (sentinel == NULL) {
        return 3;
    }
    const bool written = fwrite(child_started_token, 1,
                                sizeof child_started_token - 1U,
                                sentinel) == sizeof child_started_token - 1U;
    const bool closed = fclose(sentinel) == 0;
    if (!written || !closed) {
        (void)remove(sentinel_path);
        return 3;
    }

    struct tp_project_generation generation;
    atomic_init(&generation.refs, 0U);
    generation.project = NULL;
    tp_project_generation_release(&generation);
    return 0;
}

#ifdef _WIN32
static enum child_probe_result run_child_probe(const char *executable,
                                                const char *sentinel_path) {
    char command_line[4096];
    const int written = snprintf(command_line, sizeof command_line,
                                 "\"%s\" --invalid-release-child \"%s\"",
                                 executable, sentinel_path);
    if (written < 0 || (size_t)written >= sizeof command_line) {
        return CHILD_PROBE_INTERNAL_ERROR;
    }

    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof startup;
    if (!CreateProcessA(executable, command_line, NULL, NULL, FALSE, 0,
                        NULL, NULL, &startup, &process)) {
        return CHILD_PROBE_LAUNCH_FAILED;
    }

    const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    const bool exit_read = GetExitCodeProcess(process.hProcess, &exit_code) != 0;
    (void)CloseHandle(process.hThread);
    (void)CloseHandle(process.hProcess);
    if (wait_result != WAIT_OBJECT_0 || !exit_read) {
        return CHILD_PROBE_INTERNAL_ERROR;
    }

    const bool started = sentinel_was_written(sentinel_path);
    if (!started) {
        return CHILD_PROBE_NOT_STARTED;
    }
    return exit_code == 0 ? CHILD_PROBE_EXITED_ZERO
                          : CHILD_PROBE_TERMINATED_NONZERO;
}
#else
static enum child_probe_result run_child_probe(const char *executable,
                                                const char *sentinel_path) {
    const pid_t child = fork();
    if (child < 0) {
        return CHILD_PROBE_LAUNCH_FAILED;
    }
    if (child == 0) {
        char *const child_argv[] = {
            (char *)executable,
            (char *)"--invalid-release-child",
            (char *)sentinel_path,
            NULL,
        };
        execvp(executable, child_argv);
        _exit(127);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        return CHILD_PROBE_INTERNAL_ERROR;
    }

    const bool started = sentinel_was_written(sentinel_path);
    if (!started) {
        return CHILD_PROBE_NOT_STARTED;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0
               ? CHILD_PROBE_EXITED_ZERO
               : CHILD_PROBE_TERMINATED_NONZERO;
}
#endif

static bool probe_proves_release_assert(enum child_probe_result result) {
    return result == CHILD_PROBE_TERMINATED_NONZERO;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--invalid-release-child") == 0) {
        return run_invalid_release_child(argv[2]);
    }
    if (argc != 1) {
        return 2;
    }

#ifdef _WIN32
    const unsigned long process_id = (unsigned long)GetCurrentProcessId();
#else
    const unsigned long process_id = (unsigned long)getpid();
#endif
    char sentinel_path[256];
    char missing_executable[4096];
    const int sentinel_written = snprintf(
        sentinel_path, sizeof sentinel_path,
        "tp_project_generation_assert_%lu.started", process_id);
    const int missing_written = snprintf(
        missing_executable, sizeof missing_executable,
        "%s.definitely-missing.%lu", argv[0], process_id);
    if (sentinel_written < 0 || (size_t)sentinel_written >= sizeof sentinel_path ||
        missing_written < 0 || (size_t)missing_written >= sizeof missing_executable) {
        return 2;
    }
    if (!remove_sentinel(sentinel_path)) {
        return 2;
    }

    const enum child_probe_result missing_result =
        run_child_probe(missing_executable, sentinel_path);
    if (!remove_sentinel(sentinel_path)) {
        return 2;
    }
    if (probe_proves_release_assert(missing_result)) {
        return 3;
    }

    const enum child_probe_result assert_result =
        run_child_probe(argv[0], sentinel_path);
    const bool cleaned = remove_sentinel(sentinel_path);
    return cleaned && probe_proves_release_assert(assert_result) ? 0 : 1;
}
