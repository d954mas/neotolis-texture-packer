/* Masked-second-declaration fixture: an async TU retaining TWO raw sessions.
 * A first-hit-only scan reported one raw `tp_session *`, so every further
 * cross-thread session declaration in the same TU was unreported. Each hit
 * carries the DECLARED name, so the two are distinct records rather than
 * duplicates the debt report would fold back into one. The registering ctest
 * asserts two hits. */
typedef struct bad_worker_payload {
    tp_session *first;
    const tp_session *second;
} bad_worker_payload;
