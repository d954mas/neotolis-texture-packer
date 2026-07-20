/* Fault-injecting build-worker binaries for the H0.3-b / H0.5 outcome tests
 * (decision 0018). Built as one target per TP_WORKER_FAULT_* mode set by a
 * compile definition -- the same test-only-compile-def pattern as the CLI's
 * NTPACKER_CLI_*_FAULT_SEAM binaries. Regardless of argv, each acts as a
 * misbehaving __build-worker child: it drains stdin so the parent's write-all
 * completes, then performs its fault. PRODUCTION binaries have NO such behavior
 * and no environment-driven mode -- this file is never linked into a ship exe.
 *
 * Modes:
 *   HANG      drain, then never reply/exit -> parent cancel/safety-timeout kills it
 *   CRASH     abort() with no reply        -> BUILDER_CRASHED, host survives
 *   MALFORMED clean exit + garbage bytes   -> BUILDER_FAILED (fail closed)
 *   NONZERO   non-zero exit + valid error reply -> BUILDER_FAILED (message carried)
 *   NOWRITE   partial staging file + valid BUILDER_FAILED reply (sink/full-disk
 *             seam) -> BUILDER_FAILED, nothing published, partial file cleaned
 *   OKNOART   exit 0 + valid OK reply but NO staged artifact -> BUILDER_CRASHED
 *   BIGLEN    valid header declaring a giant payload length -> BUILDER_FAILED
 *   TRUNC     valid header + fewer payload bytes than declared -> BUILDER_FAILED */

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

/* Modes that emit a versioned response frame need the codec + status enums. */
#if defined(TP_WORKER_FAULT_NONZERO) || defined(TP_WORKER_FAULT_NOWRITE) ||   \
    defined(TP_WORKER_FAULT_OKNOART) || defined(TP_WORKER_FAULT_BIGLEN) ||    \
    defined(TP_WORKER_FAULT_TRUNC)
#define TP_WORKER_FAULT_EMITS_FRAME 1
#include "tp_core/tp_error.h"
#include "tp_build_proto_internal.h"
#endif

/* The parent runs the child with its CWD set to the private ASCII staging dir and
 * verifies "staging/out.ntpack" before publishing. A fault worker that stages a
 * file writes this bare relative name (mirrors TP_BUILD_WORKER_OUT_NAME). */
#define TP_WORKER_FAULT_OUT_NAME "out.ntpack"

static void drain_stdin(void) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
#endif
    unsigned char buf[4096];
    while (fread(buf, 1u, sizeof buf, stdin) > 0u) {
        /* discard the request so the parent's write-all can complete */
    }
}

#if defined(TP_WORKER_FAULT_EMITS_FRAME)
/* Encode one response frame; *bytes owns the buffer (free with free()). Returns
 * the frame length, or 0 if encoding failed. */
static size_t encode_frame(int32_t status, int32_t builder_code, const char *artifact,
                           const char *message, uint8_t **bytes) {
    tp_build_proto_response resp;
    memset(&resp, 0, sizeof resp);
    resp.status = status;
    resp.builder_code = builder_code;
    resp.artifact_name = artifact ? artifact : "";
    resp.message = message ? message : "";
    *bytes = NULL;
    size_t len = 0u;
    if (tp_build_proto_encode_response(&resp, bytes, &len, NULL) != TP_STATUS_OK) {
        return 0u;
    }
    return len;
}

/* Write `len` raw bytes to stdout as a binary channel and flush. */
static void write_stdout_frame(const uint8_t *bytes, size_t len) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    (void)fwrite(bytes, 1u, len, stdout);
    (void)fflush(stdout);
}
#endif

int main(void) {
#if defined(_WIN32)
    /* Same dialog suppression as the production worker (tp_build_worker_main):
     * a faulting child must die silently, never wait on a WER/abort dialog. */
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    (void)_set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
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
    {
        uint8_t *bytes = NULL;
        size_t len = encode_frame(TP_STATUS_BUILDER_FAILED, 7, "",
                                  "worker fault seam: builder failed", &bytes);
        if (len) {
            write_stdout_frame(bytes, len);
        }
        free(bytes);
    }
    return 3; /* non-zero exit + a VALID error reply -> host maps BUILDER_FAILED (carried) */
#elif defined(TP_WORKER_FAULT_NOWRITE)
    /* Full-disk / sink-write-failure seam: leave a PARTIAL artifact in staging (as
     * a torn write would), then report a structured builder/sink failure. The
     * parent must map BUILDER_FAILED, publish NOTHING at the destination, and
     * remove the partial file with the staging dir. CWD is the ASCII staging dir. */
    {
        FILE *partial = fopen(TP_WORKER_FAULT_OUT_NAME, "wb");
        if (partial) {
            static const char torn[] = "PARTIAL-NTPACK-TORN-WRITE";
            (void)fwrite(torn, 1u, sizeof torn - 1u, partial);
            (void)fclose(partial);
        }
        uint8_t *bytes = NULL;
        size_t len = encode_frame(TP_STATUS_BUILDER_FAILED, 5, "",
                                  "worker fault seam: sink write failed mid-artifact", &bytes);
        if (len) {
            write_stdout_frame(bytes, len);
        }
        free(bytes);
    }
    return 0; /* clean exit + valid BUILDER_FAILED reply -> parent BUILDER_FAILED, no publish */
#elif defined(TP_WORKER_FAULT_OKNOART)
    /* Exit 0 with a VALID OK reply but NO artifact in staging. The parent verifies
     * the staged artifact before publishing; a missing/empty file must map to
     * BUILDER_CRASHED ("reported success but produced no readable artifact"). */
    {
        uint8_t *bytes = NULL;
        size_t len = encode_frame(TP_STATUS_OK, 0, TP_WORKER_FAULT_OUT_NAME, "", &bytes);
        if (len) {
            write_stdout_frame(bytes, len);
        }
        free(bytes);
    }
    return 0;
#elif defined(TP_WORKER_FAULT_BIGLEN)
    /* A well-formed OK frame whose header declares a GIANT payload length that does
     * not match the bytes actually sent. Goes THROUGH the process; the parent's
     * codec must reject it (declared != available) and FAIL CLOSED -> BUILDER_FAILED,
     * no publish. (Codec-level declared-length rejection is #43; this pins the
     * PARENT path.) payload_len is the LE u32 at frame-header offset 8. */
    {
        uint8_t *bytes = NULL;
        size_t len = encode_frame(TP_STATUS_OK, 0, TP_WORKER_FAULT_OUT_NAME, "", &bytes);
        if (len >= 12u) {
            uint32_t giant = 0x7fffffffu;
            memcpy(bytes + 8, &giant, sizeof giant);
            write_stdout_frame(bytes, len); /* actual payload is tiny -> declared != available */
        }
        free(bytes);
    }
    return 0;
#elif defined(TP_WORKER_FAULT_TRUNC)
    /* A valid frame header (magic+version+declared length) followed by FEWER payload
     * bytes than declared: a short/truncated frame on the wire. Parent codec must
     * reject (available < declared) and FAIL CLOSED -> BUILDER_FAILED, no publish. */
    {
        uint8_t *bytes = NULL;
        size_t len = encode_frame(TP_STATUS_OK, 0, TP_WORKER_FAULT_OUT_NAME, "", &bytes);
        if (len > 12u) {
            size_t send = 12u + (len - 12u) / 2u; /* header intact + half the payload */
            write_stdout_frame(bytes, send);
        }
        free(bytes);
    }
    return 0;
#else
#error "tp_build_worker_fault.c requires a TP_WORKER_FAULT_* mode"
#endif
}
