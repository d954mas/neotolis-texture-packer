/* Masked-second-member fixture: a host queue retaining TWO raw sessions.
 * The rule used to span struct header and member in one pattern, whose greedy
 * body wildcard swallowed every member but the last -- so a queue holding two
 * sessions reported a single hit. The body is extracted first and its members
 * scanned one at a time; the registering ctest asserts two hits. */
typedef struct tp_session tp_session;

typedef struct gui_host_queue {
    tp_session *first;
    const tp_session *second;
} gui_host_queue;
