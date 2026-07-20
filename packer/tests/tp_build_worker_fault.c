/* Fault-injecting build-worker binaries for the H0.3-b outcome tests (decision
 * 0018). Built as three separate targets, one per TP_WORKER_FAULT_* mode set by
 * a compile definition -- the same test-only-compile-def pattern as the CLI's
 * NTPACKER_CLI_*_FAULT_SEAM binaries. Regardless of argv, each acts as a
 * misbehaving __build-worker child: it drains stdin so the parent's write-all
 * completes, then performs its fault. PRODUCTION binaries have NO such behavior
 * and no environment-driven mode -- this file is never linked into a ship exe. */

#define _CRT_SECURE_NO_WARNINGS

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
#endif

#if defined(TP_WORKER_FAULT_NONZERO)
#include "tp_core/tp_error.h"
#include "tp_build_proto_internal.h"
#endif

static void drain_stdin(void) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
#endif
    unsigned char buf[4096];
    while (fread(buf, 1u, sizeof buf, stdin) > 0u) {
        /* discard the request so the parent's write-all can complete */
    }
}

int main(void) {
    drain_stdin();

#if defined(TP_WORKER_FAULT_HANG)
    /* Drained the request, then never reply and never exit: the parent's wait
     * loop must observe cancellation / the safety timeout, tree-kill this child,
     * and reach EOF on the now-broken stdout pipe. Sleep in bounded chunks so a
     * missed kill cannot wedge CI forever. */
    for (int i = 0; i < 6000; i++) {
#if defined(_WIN32)
        Sleep(100u);
#else
        struct timespec ts = {0, 100 * 1000 * 1000};
        (void)nanosleep(&ts, NULL);
#endif
    }
    return 0; /* ~10 min ceiling; the parent kills this long before */
#elif defined(TP_WORKER_FAULT_CRASH)
    /* POSIX: SIGABRT (killed); Windows: exit code 3. No reply is written, so the
     * host maps this to BUILDER_CRASHED. */
    abort();
    return 0; /* unreachable */
#elif defined(TP_WORKER_FAULT_MALFORMED)
#if defined(_WIN32)
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    static const char garbage[] = "not-a-valid-response-frame-payload";
    (void)fwrite(garbage, 1u, sizeof garbage - 1u, stdout);
    (void)fflush(stdout);
    return 0; /* clean exit + malformed reply -> host maps BUILDER_FAILED */
#elif defined(TP_WORKER_FAULT_NONZERO)
#if defined(_WIN32)
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    tp_build_proto_response resp;
    memset(&resp, 0, sizeof resp);
    resp.status = TP_STATUS_BUILDER_FAILED;
    resp.builder_code = 7;
    resp.artifact_name = "";
    resp.message = "worker fault seam: builder failed";
    uint8_t *bytes = NULL;
    size_t len = 0u;
    if (tp_build_proto_encode_response(&resp, &bytes, &len, NULL) == TP_STATUS_OK) {
        (void)fwrite(bytes, 1u, len, stdout);
        (void)fflush(stdout);
        free(bytes);
    }
    return 3; /* non-zero exit + a VALID error reply -> host maps BUILDER_FAILED (carried) */
#else
#error "tp_build_worker_fault.c requires a TP_WORKER_FAULT_* mode"
#endif
}
