#include "tp_validate_report_internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_validate_internal.h"

static _Thread_local int s_alloc_fail = -1;

void tp_validate__test_set_alloc_fail(int nth) { s_alloc_fail = nth; }

static size_t report_slot_limit(void) {
    size_t by_bytes = (size_t)TP_VALIDATION_REPORT_MAX_BYTES / sizeof(tp_validation_finding);
    return by_bytes < (size_t)TP_VALIDATION_REPORT_MAX_FINDINGS ? by_bytes
                                                                : (size_t)TP_VALIDATION_REPORT_MAX_FINDINGS;
}

static size_t report_ordinary_limit(void) {
    const size_t limit = report_slot_limit();
    return limit > 0U ? limit - 1U : 0U;
}

bool report_has_room(const validation_builder *fs) {
    return !fs->materialization_closed && fs->n < report_ordinary_limit();
}

#define TP_VALIDATION_SUMMARY_STORAGE_RESERVE 384U

static bool size_add(size_t *value, size_t increment) {
    if (*value > SIZE_MAX - increment) {
        return false;
    }
    *value += increment;
    return true;
}

static bool report_can_materialize(const validation_builder *fs,
                                   size_t storage_bytes) {
    /* Reserve the vector's maximum possible capacity, not only its current
     * logical length: realloc grows geometrically and the public byte limit is
     * a hard materialization bound, including unused capacity. */
    size_t bytes = report_slot_limit() * sizeof(tp_validation_finding);
    if (!size_add(&bytes, fs->storage_bytes) ||
        !size_add(&bytes, storage_bytes) ||
        !size_add(&bytes, TP_VALIDATION_SUMMARY_STORAGE_RESERVE)) {
        return false;
    }
    return bytes <= (size_t)TP_VALIDATION_REPORT_MAX_BYTES;
}

void add_omitted(validation_builder *fs, tp_validation_severity severity, size_t count) {
    fs->total += count;
    if (severity == TP_VALIDATION_ERROR) {
        fs->errors += count;
        fs->omitted_errors += count;
    } else {
        fs->warnings += count;
        fs->omitted_warnings += count;
    }
}

static tp_validation_finding *findings_new(validation_builder *fs) {
    if (fs->oom) {
        return NULL;
    }
    if (fs->n == fs->cap) {
        const size_t limit = report_slot_limit();
        size_t nc = fs->cap ? fs->cap * 2U : 16U;
        if (nc > limit) {
            nc = limit;
        }
        if (nc <= fs->cap) {
            return NULL;
        }
        if (s_alloc_fail == 0) {
            s_alloc_fail = -1;
            fs->oom = true;
            return NULL;
        }
        if (s_alloc_fail > 0) {
            s_alloc_fail--;
        }
        tp_validation_finding *nv = (tp_validation_finding *)realloc(fs->v, nc * sizeof *nv);
        if (!nv) {
            fs->oom = true;
            return NULL;
        }
        fs->v = nv;
        fs->cap = nc;
    }
    tp_validation_finding *f = &fs->v[fs->n++];
    memset(f, 0, sizeof *f);
    f->message = "";
    f->atlas = "";
    f->source = "";
    f->sprite = "";
    f->anim = "";
    f->frame = "";
    f->target = "";
    return f;
}

static bool storage_size_add_string(size_t *bytes, const char *value) {
    const size_t length = value ? strlen(value) : 0U;
    return length < SIZE_MAX && size_add(bytes, length + 1U);
}

static void storage_copy_string(char **cursor, const char *value,
                                const char **out) {
    const char *source = value ? value : "";
    const size_t bytes = strlen(source) + 1U;
    memcpy(*cursor, source, bytes);
    *out = *cursor;
    *cursor += bytes;
}

/* Appends one finding. NULL context fields are omitted. Never aborts: on OOM the
 * findings list poisons and the caller reports it as an internal error. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 5, 6)))
#endif
void add_finding(validation_builder *fs,
                        tp_validation_severity severity, const char *code,
                        finding_context context, const char *fmt, ...) {
    fs->total++;
    if (severity == TP_VALIDATION_ERROR) {
        fs->errors++;
    } else {
        fs->warnings++;
    }
    if (!report_has_room(fs)) {
        if (severity == TP_VALIDATION_ERROR) {
            fs->omitted_errors++;
        } else {
            fs->omitted_warnings++;
        }
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    va_list measure;
    va_copy(measure, ap);
    const int message_length = vsnprintf(NULL, 0U, fmt, measure);
    va_end(measure);
    if (message_length < 0) {
        va_end(ap);
        fs->oom = true;
        return;
    }
    size_t storage_bytes = (size_t)message_length + 1U;
    if (!storage_size_add_string(&storage_bytes, context.atlas) ||
        !storage_size_add_string(&storage_bytes, context.source) ||
        !storage_size_add_string(&storage_bytes, context.sprite) ||
        !storage_size_add_string(&storage_bytes, context.anim) ||
        !storage_size_add_string(&storage_bytes, context.frame) ||
        !storage_size_add_string(&storage_bytes, context.target)) {
        va_end(ap);
        fs->oom = true;
        return;
    }
    if (!report_can_materialize(fs, storage_bytes)) {
        va_end(ap);
        fs->materialization_closed = true;
        if (severity == TP_VALIDATION_ERROR) {
            fs->omitted_errors++;
        } else {
            fs->omitted_warnings++;
        }
        return;
    }

    tp_validation_finding *f = findings_new(fs);
    if (!f) {
        va_end(ap);
        return;
    }
    f->owned_storage = (char *)malloc(storage_bytes);
    if (!f->owned_storage) {
        va_end(ap);
        fs->oom = true;
        return;
    }
    f->severity = severity;
    (void)snprintf(f->code, sizeof f->code, "%s", code);
    char *cursor = f->owned_storage;
    f->message = cursor;
    (void)vsnprintf(cursor, (size_t)message_length + 1U, fmt, ap);
    cursor += (size_t)message_length + 1U;
    va_end(ap);
    storage_copy_string(&cursor, context.atlas, &f->atlas);
    storage_copy_string(&cursor, context.source, &f->source);
    storage_copy_string(&cursor, context.sprite, &f->sprite);
    storage_copy_string(&cursor, context.anim, &f->anim);
    storage_copy_string(&cursor, context.frame, &f->frame);
    storage_copy_string(&cursor, context.target, &f->target);
    f->atlas_id = context.atlas_id;
    f->source_id = context.source_id;
    f->animation_id = context.animation_id;
    f->target_id = context.target_id;
    fs->storage_bytes += storage_bytes;
}

static void findings_free(tp_validation_finding *findings, size_t count) {
    if (findings) {
        for (size_t i = 0U; i < count; ++i) {
            free(findings[i].owned_storage);
        }
    }
    free(findings);
}

void tp_validation_report_free(tp_validation_report *report) {
    if (!report) {
        return;
    }
    findings_free(report->findings, report->finding_count);
    memset(report, 0, sizeof *report);
}

static void add_truncation_summary(validation_builder *fs) {
    const size_t omitted = fs->omitted_errors + fs->omitted_warnings;
    if (omitted == 0U) {
        return;
    }
    tp_validation_finding *f = findings_new(fs);
    if (!f) {
        fs->oom = true;
        return;
    }
    f->severity = fs->omitted_errors > 0U ? TP_VALIDATION_ERROR : TP_VALIDATION_WARNING;
    (void)snprintf(f->code, sizeof f->code, "%s", TP_VALIDATION_CODE_TRUNCATED);
    const int message_length = snprintf(
        NULL, 0U,
        "validation report truncated: omitted %zu of %zu findings (%zu errors, %zu warnings); "
        "limits are %u findings and %u bytes",
        omitted, fs->total, fs->omitted_errors, fs->omitted_warnings,
        (unsigned)TP_VALIDATION_REPORT_MAX_FINDINGS,
        (unsigned)TP_VALIDATION_REPORT_MAX_BYTES);
    if (message_length < 0) {
        fs->oom = true;
        return;
    }
    const size_t storage_bytes = (size_t)message_length + 1U;
    f->owned_storage = (char *)malloc(storage_bytes);
    if (!f->owned_storage) {
        fs->oom = true;
        return;
    }
    f->message = f->owned_storage;
    (void)snprintf(f->owned_storage, storage_bytes,
                   "validation report truncated: omitted %zu of %zu findings (%zu errors, %zu warnings); "
                   "limits are %u findings and %u bytes",
                   omitted, fs->total, fs->omitted_errors,
                   fs->omitted_warnings,
                   (unsigned)TP_VALIDATION_REPORT_MAX_FINDINGS,
                   (unsigned)TP_VALIDATION_REPORT_MAX_BYTES);
    fs->storage_bytes += storage_bytes;
}

tp_status validation_builder_finish(validation_builder *builder,
                                    tp_validation_report *out,
                                    tp_error *err) {
    add_truncation_summary(builder);
    if (builder->oom) {
        findings_free(builder->v, builder->n);
        return tp_error_set(
            err, TP_STATUS_OOM,
            "out of memory collecting validation findings");
    }

    out->findings = builder->v;
    out->finding_count = builder->n;
    out->error_count = builder->errors;
    out->warning_count = builder->warnings;
    out->total_finding_count = builder->total;
    out->omitted_finding_count =
        builder->omitted_errors + builder->omitted_warnings;
    out->truncated = out->omitted_finding_count > 0U;
    return TP_STATUS_OK;
}
