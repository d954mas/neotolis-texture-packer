#ifndef TP_CORE_TP_SOURCE_RUNTIME_H
#define TP_CORE_TP_SOURCE_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_source_runtime_projection tp_source_runtime_projection;

typedef struct tp_source_runtime_source {
    tp_id128 atlas_id;
    tp_id128 source_id;
    const char *absolute_path;
    bool folder;
    tp_status status;
    int first_entry;
    int entry_count;
} tp_source_runtime_source;

typedef struct tp_source_runtime_entry {
    tp_id128 atlas_id;
    tp_id128 source_id;
    const char *source_key;
    const char *absolute_path;
    int64_t size;
    int64_t mtime;
} tp_source_runtime_entry;

int tp_source_runtime_source_count(
    const tp_source_runtime_projection *projection);
const tp_source_runtime_source *tp_source_runtime_source_at(
    const tp_source_runtime_projection *projection, int index);
const tp_source_runtime_source *tp_source_runtime_source_by_id(
    const tp_source_runtime_projection *projection,
    tp_id128 atlas_id, tp_id128 source_id);
int tp_source_runtime_entry_count(
    const tp_source_runtime_projection *projection);
const tp_source_runtime_entry *tp_source_runtime_entry_at(
    const tp_source_runtime_projection *projection, int index);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_SOURCE_RUNTIME_H */
