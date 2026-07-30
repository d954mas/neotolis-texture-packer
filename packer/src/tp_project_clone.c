/* OOM-safe project clone. Persistent entity ownership is defined once in
 * tp_project_owned.c; this file supplies only the independent clone allocator
 * and its fault/accounting seam. */

#include "tp_core/tp_project.h"

#include <stdlib.h>

#include "tp_project_owned_internal.h"
#include "tp_txn_internal.h"

#ifdef TP_ENABLE_TEST_SEAMS
static _Thread_local int s_clone_fail = -1;
static _Thread_local int s_clone_allocs = 0;
static _Thread_local size_t s_clone_allocation_bytes = 0U;

void tp_project__test_set_clone_alloc_fail(int nth) {
    s_clone_fail = nth;
    s_clone_allocs = 0;
    s_clone_allocation_bytes = 0U;
}
int tp_project__test_clone_alloc_count(void) { return s_clone_allocs; }
size_t tp_project__test_clone_allocation_bytes(void) {
    return s_clone_allocation_bytes;
}
#endif

typedef struct clone_context {
    int fail;
    int allocations;
    size_t bytes;
} clone_context;

static void *clone_allocate(void *opaque, size_t size) {
    clone_context *context = opaque;
    context->allocations++;
    if (context->fail == 0) {
        context->fail = -1;
        return NULL;
    }
    if (context->fail > 0) {
        context->fail--;
    }
    void *allocation = calloc(1U, size);
    if (allocation) {
        context->bytes += size;
    }
    return allocation;
}

tp_project *tp_project_clone(const tp_project *source) {
    if (!source) {
        return NULL;
    }
#ifdef TP_ENABLE_TEST_SEAMS
    clone_context context = {s_clone_fail, 0, 0U};
#else
    clone_context context = {-1, 0, 0U};
#endif
    tp_project *copy = clone_allocate(&context, sizeof *copy);
    if (copy) {
        const tp_project_owned_allocator allocator = {
            clone_allocate, &context};
        if (tp_project_owned_copy_project(copy, source, allocator) !=
            TP_STATUS_OK) {
            tp_project_destroy(copy);
            copy = NULL;
        }
    }
#ifdef TP_ENABLE_TEST_SEAMS
    s_clone_fail = context.fail;
    s_clone_allocs = context.allocations;
    s_clone_allocation_bytes = context.bytes;
#endif
    return copy;
}
