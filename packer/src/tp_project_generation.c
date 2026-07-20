#include "tp_project_generation_internal.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "core/nt_assert.h"

struct tp_project_generation {
    atomic_uint refs;
    tp_project *project;
};

static _Thread_local bool s_fail_next_allocation;

tp_project_generation *tp_project_generation_create_owned(tp_project *project) {
    if (!project) {
        return NULL;
    }
    if (s_fail_next_allocation) {
        s_fail_next_allocation = false;
        return NULL;
    }
    tp_project_generation *generation = calloc(1U, sizeof *generation);
    if (!generation) {
        return NULL;
    }
    atomic_init(&generation->refs, 1U);
    generation->project = project;
    return generation;
}

void tp_project_generation_retain(tp_project_generation *generation) {
    if (!generation) {
        return;
    }
    const unsigned previous = atomic_fetch_add_explicit(
        &generation->refs, 1U, memory_order_relaxed);
    (void)previous;
    NT_ASSERT(previous > 0U && previous < UINT_MAX);
}

void tp_project_generation_release(tp_project_generation *generation) {
    if (!generation) {
        return;
    }
    const unsigned previous = atomic_fetch_sub_explicit(
        &generation->refs, 1U, memory_order_acq_rel);
    NT_ASSERT(previous > 0U);
    if (previous == 1U) {
        tp_project_destroy(generation->project);
        free(generation);
    }
}

const tp_project *tp_project_generation_project(
    const tp_project_generation *generation) {
    return generation ? generation->project : NULL;
}

void tp_project_generation__test_fail_next_allocation(void) {
    s_fail_next_allocation = true;
}
