#ifndef TP_VALIDATE_REPORT_INTERNAL_H
#define TP_VALIDATE_REPORT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_validate.h"

typedef struct validation_builder {
    tp_validation_finding *v;
    size_t n;
    size_t cap;
    size_t storage_bytes;
    size_t total;
    size_t errors;
    size_t warnings;
    size_t omitted_errors;
    size_t omitted_warnings;
    bool materialization_closed;
    bool oom;
} validation_builder;

typedef struct finding_context {
    const char *atlas;
    tp_id128 atlas_id;
    const char *source;
    tp_id128 source_id;
    const char *sprite;
    const char *anim;
    tp_id128 animation_id;
    const char *frame;
    const char *target;
    tp_id128 target_id;
} finding_context;

void add_omitted(validation_builder *builder,
                 tp_validation_severity severity, size_t count);
bool report_has_room(const validation_builder *builder);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 5, 6)))
#endif
void add_finding(validation_builder *builder,
                 tp_validation_severity severity, const char *code,
                 finding_context context, const char *format, ...);

tp_status validation_builder_finish(validation_builder *builder,
                                    tp_validation_report *out,
                                    tp_error *err);

#endif /* TP_VALIDATE_REPORT_INTERNAL_H */
