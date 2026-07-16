#ifndef TP_VALIDATE_H
#define TP_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

struct tp_session_snapshot;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TP_VALIDATION_WARNING = 0,
    TP_VALIDATION_ERROR = 1,
} tp_validation_severity;

/* Append-only machine vocabulary. Adapters render these tokens verbatim. */
#define TP_VALIDATION_CODE_MISSING_SOURCE "missing_source"
#define TP_VALIDATION_CODE_DUPLICATE_SOURCE "duplicate_source"
#define TP_VALIDATION_CODE_SOURCE_COLLISION "source_collision"
#define TP_VALIDATION_CODE_SOURCE_PORTABILITY "source_portability"
#define TP_VALIDATION_CODE_SOURCE_ESCAPES_ROOT "source_escapes_root"
#define TP_VALIDATION_CODE_EMPTY_ATLAS "empty_atlas"
#define TP_VALIDATION_CODE_DANGLING_ANIM_FRAME "dangling_anim_frame"
#define TP_VALIDATION_CODE_DUPLICATE_EXPORT_KEY "duplicate_export_key"
#define TP_VALIDATION_CODE_EXPORT_NAME_COLLISION "export_name_collision"
#define TP_VALIDATION_CODE_UNKNOWN_EXPORTER "unknown_exporter"
#define TP_VALIDATION_CODE_TARGET_NO_OUT_PATH "target_no_out_path"
#define TP_VALIDATION_CODE_DUPLICATE_OUT_PATH "duplicate_out_path"
#define TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE "setting_out_of_range"
#define TP_VALIDATION_CODE_SPRITE_BAD_SOURCE "sprite_bad_source"
#define TP_VALIDATION_CODE_FRAME_BAD_SOURCE "frame_bad_source"
#define TP_VALIDATION_CODE_ORPHAN_SPRITE "orphan_sprite"
#define TP_VALIDATION_CODE_DUPLICATE_SPRITE_KEY "duplicate_sprite_key"
#define TP_VALIDATION_CODE_INVALID_SPRITE_KEY "invalid_sprite_key"
#define TP_VALIDATION_CODE_INVALID_FRAME_KEY "invalid_frame_key"
#define TP_VALIDATION_CODE_INPUT_BUILD_FAILED "input_build_failed"
#define TP_VALIDATION_CODE_TRUNCATED "validation_truncated"

/* Hard report-materialization limits. The count includes the deterministic
 * truncation summary when present; the byte limit covers the findings vector. */
#define TP_VALIDATION_REPORT_MAX_FINDINGS 2048U
#define TP_VALIDATION_REPORT_MAX_BYTES 4194304U

/* Owned, presentation-neutral validation result. Empty context strings mean
 * "not present"; adapters may omit them from their wire/UI representation. */
typedef struct {
    tp_validation_severity severity;
    char code[32];
    char message[256];
    char atlas[128];
    char sprite[256];
    char anim[128];
    char frame[256];
    char target[64];
} tp_validation_finding;

typedef struct {
    tp_validation_finding *findings;
    size_t finding_count;         /* materialized findings, including truncation summary */
    size_t error_count;           /* all discovered domain errors, including omitted ones */
    size_t warning_count;         /* all discovered domain warnings, including omitted ones */
    size_t total_finding_count;   /* error_count + warning_count; excludes synthetic summary */
    size_t omitted_finding_count; /* domain findings replaced by / after the summary */
    bool truncated;
} tp_validation_report;

#define TP_TARGET_VALIDATION_MAX_ISSUES 3U
typedef struct {
    tp_validation_severity severity;
    char code[32];
} tp_target_validation_issue;

typedef struct {
    tp_target_validation_issue issues[TP_TARGET_VALIDATION_MAX_ISSUES];
    size_t issue_count;
} tp_target_validation_report;

/* Loads and validates one saved project. The caller never receives a project
 * pointer. On failure, `out` remains safely freeable and owns no findings. */
tp_status tp_validate_project_file(const char *path, tp_validation_report *out, tp_error *err);
/* Validates one immutable session snapshot without reloading or exposing a model. */
tp_status tp_validate_session_snapshot(const struct tp_session_snapshot *snapshot,
                                       tp_validation_report *out, tp_error *err);
tp_status tp_validate_session_snapshot_target(
    const struct tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 target_id, tp_target_validation_report *out, tp_error *err);
void tp_validation_report_free(tp_validation_report *report);

#ifdef __cplusplus
}
#endif

#endif
