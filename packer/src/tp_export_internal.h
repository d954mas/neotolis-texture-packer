#ifndef TP_CORE_SRC_TP_EXPORT_INTERNAL_H
#define TP_CORE_SRC_TP_EXPORT_INTERNAL_H

#include "tp_core/tp_export.h"
#include "tp_core/tp_cancel.h"

/* Native serializers are an implementation detail of the fixed built-in
 * table. Public clients discover descriptor metadata through tp_format_catalog_*.
 */
typedef struct tp_export_document {
    void *data;
    size_t size;
    /* Distinguishes a deliberately finished empty text document from a
     * serializer that never produced the declared output. Existing non-empty
     * serializers remain valid through their owned data pointer. */
    bool produced;
} tp_export_document;

/* Owned serializer result.  Native and Lua handlers cross the same lifetime
 * boundary: serialization completes in memory, the complete declared set is
 * validated, and only then may publication acquire leases or create staging. */
typedef struct tp_export_document_batch {
    tp_export_document *documents;
    int document_count;
} tp_export_document_batch;

typedef struct tp_export_publication_guard {
    void *leases;
    int lease_count;
} tp_export_publication_guard;

#define TP_EXPORT_HOST_FACT_VALUE_MAX_BYTES 4095U

typedef struct tp_export_host_fact {
    const char *id;
    const char *value;
} tp_export_host_fact;

typedef struct tp_export_serialize_ctx {
    const tp_export_ir *ir;
    const tp_format_descriptor *format;
    const tp_export_artifact_plan *plan;
    tp_export_notices *notices;
    tp_format_diagnostic_report **format_diagnostics;
    const tp_export_host_fact *host_facts;
    int host_fact_count;
    const void *handler_context;
    const tp_cancel_token *cancel;
} tp_export_serialize_ctx;

typedef tp_status (*tp_export_serialize_fn)(const tp_export_serialize_ctx *ctx,
                                            tp_export_document *documents,
                                            int document_count,
                                            tp_error *err);

typedef struct tp_exporter {
    const tp_format_descriptor *format;
    tp_export_serialize_fn serialize;
    const void *handler_context;
    bool lua_handler;
    void (*destroy)(struct tp_exporter *exporter);
} tp_exporter;

/* Fixed compiled-in handler table. Active lookup always goes through an
 * explicit tp_format_catalog; these helpers expose only the immutable native
 * prefix used to materialize/query a catalog. */
const tp_exporter *tp_native_exporter_find(const char *id);
int tp_native_exporter_count(void);
const tp_exporter *tp_native_exporter_at(int index);

/* Internal O(1) projection for an IR already admitted by the caller's owning
 * boundary. Public callers use tp_export_ir_project_for_caps(), which performs
 * full validation first. */
void tp_export_ir_project_for_caps_unchecked(const tp_export_ir *source,
                                             const tp_export_caps *caps,
                                             tp_export_ir *out);

tp_status tp_export_publish(const tp_exporter *exp,
                            const tp_export_ir *ir,
                            const tp_result *packed,
                            const tp_export_artifact_plan *plan,
                            tp_export_notices *notices,
                            bool *out_serializer_ran,
                            bool *out_publication_uncertain,
                            tp_error *err);

tp_status tp_export_validate_publication_inputs(
    const tp_exporter *exp, const tp_export_ir *ir, const tp_result *packed,
    const tp_export_artifact_plan *plan, tp_error *err);
tp_status tp_export_serialize_and_validate_documents(
    const tp_exporter *exp, const tp_export_ir *ir,
    const tp_export_artifact_plan *plan, tp_export_notices *notices,
    tp_format_diagnostic_report **out_format_diagnostics,
    tp_export_document_batch *out_batch, bool *out_serializer_ran,
    const tp_cancel_token *cancel, tp_error *err);
tp_status tp_export_publication_guard_acquire(
    const tp_export_artifact_plan *plan,
    tp_export_publication_guard *out_guard, tp_error *err);
void tp_export_publication_guard_release(tp_export_publication_guard *guard);
tp_status tp_export_publish_documents(
    const tp_exporter *exp, const tp_export_ir *ir, const tp_result *packed,
    const tp_export_artifact_plan *plan,
    const tp_export_document_batch *batch,
    const tp_export_publication_guard *guard,
    bool *out_publication_uncertain, tp_error *err);
void tp_export_document_batch_destroy(tp_export_document_batch *batch);

tp_status tp_export_write_page_artifact(const tp_page *page, int page_id,
                                        const char *path, bool premultiply,
                                        tp_error *err);

tp_status tp_export_json_neotolis_serialize(const tp_export_serialize_ctx *ctx,
                                            tp_export_document *documents,
                                            int document_count,
                                            tp_error *err);

#ifdef TP_ENABLE_TEST_SEAMS
void tp_export_notices__test_fail_next_reserve(void);
void tp_export_publish__test_fail_rename_at(int nth);
void tp_export_ir_projection__test_reset_work(void);
size_t tp_export_ir_projection__test_validation_count(void);
#endif

#endif /* TP_CORE_SRC_TP_EXPORT_INTERNAL_H */
