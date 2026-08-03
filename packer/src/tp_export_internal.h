#ifndef TP_CORE_SRC_TP_EXPORT_INTERNAL_H
#define TP_CORE_SRC_TP_EXPORT_INTERNAL_H

#include "tp_core/tp_export.h"

/* Native serializers are an implementation detail of the fixed built-in
 * table. Public clients discover descriptor metadata through tp_format_catalog_*.
 */
typedef struct tp_export_document {
    void *data;
    size_t size;
} tp_export_document;

typedef struct tp_export_serialize_ctx {
    const tp_export_ir *ir;
    const tp_format_descriptor *format;
    const tp_export_artifact_plan *plan;
    tp_export_notices *notices;
} tp_export_serialize_ctx;

typedef tp_status (*tp_export_serialize_fn)(const tp_export_serialize_ctx *ctx,
                                            tp_export_document *documents,
                                            int document_count,
                                            tp_error *err);

typedef struct tp_exporter {
    const tp_format_descriptor *format;
    tp_export_serialize_fn serialize;
} tp_exporter;

/* Fixed compiled-in handler table. Active lookup always goes through an
 * explicit tp_format_catalog; these helpers expose only the immutable native
 * prefix used to materialize/query a catalog. */
const tp_exporter *tp_native_exporter_find(const char *id);
int tp_native_exporter_count(void);
const tp_exporter *tp_native_exporter_at(int index);

tp_status tp_export_publish(const tp_exporter *exp,
                            const tp_export_ir *ir,
                            const tp_result *packed,
                            const tp_export_artifact_plan *plan,
                            tp_export_notices *notices,
                            bool *out_serializer_ran,
                            bool *out_publication_uncertain,
                            tp_error *err);

tp_status tp_export_write_page_artifact(const tp_page *page, int page_id,
                                        const char *path, bool premultiply,
                                        tp_error *err);

tp_status tp_export_json_neotolis_serialize(const tp_export_serialize_ctx *ctx,
                                            tp_export_document *documents,
                                            int document_count,
                                            tp_error *err);
tp_status tp_export_defold_serialize(const tp_export_serialize_ctx *ctx,
                                     tp_export_document *documents,
                                     int document_count,
                                     tp_error *err);

#ifdef TP_ENABLE_TEST_SEAMS
void tp_export_notices__test_fail_next_reserve(void);
void tp_export_publish__test_fail_rename_at(int nth);
#endif

#endif /* TP_CORE_SRC_TP_EXPORT_INTERNAL_H */
