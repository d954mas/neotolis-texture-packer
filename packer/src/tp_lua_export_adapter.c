#include "tp_lua_export_adapter_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nt_assert.h"
#include "tp_export_internal.h"
#include "tp_format_catalog_internal.h"
#include "tp_format_diagnostic_internal.h"
#include "tp_lua_host_internal.h"

typedef struct tp_lua_export_binding {
    const unsigned char *source;
    size_t source_byte_count;
    char *source_path;
} tp_lua_export_binding;

void tp_lua_export_panic_marker_set_worker(
    tp_lua_export_panic_marker_fn marker, void *context) {
    tp_lua_panic_marker_set_internal(marker, context);
}

static char *adapter_strdup(const char *text) {
    const size_t length = text ? strlen(text) : 0U;
    char *copy = malloc(length + 1U);
    if (copy) {
        memcpy(copy, text ? text : "", length + 1U);
    }
    return copy;
}

static const char *path_leaf(const char *path) {
    const char *leaf = path ? path : "";
    for (const char *cursor = leaf; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            leaf = cursor + 1;
        }
    }
    return leaf;
}

static tp_status lua_export_serialize(const tp_export_serialize_ctx *ctx,
                                      tp_export_document *documents,
                                      int document_count, tp_error *error) {
    const tp_lua_export_binding *binding = ctx ? ctx->handler_context : NULL;
    if (!ctx || !ctx->ir || !ctx->format || !ctx->plan || !binding ||
        document_count <= 0 ||
        (unsigned int)document_count > TP_FORMAT_OUTPUT_MAX) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "Lua export adapter input is incomplete");
    }
    const char *page_images[TP_PACK_MAX_PAGES] = {0};
    if (ctx->ir->page_count < 0 || ctx->ir->page_count > TP_PACK_MAX_PAGES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "Lua export page count exceeds its bound");
    }
    for (int p = 0; p < ctx->ir->page_count; ++p) {
        page_images[p] = path_leaf(
            ctx->plan->artifacts[ctx->plan->document_count + p].path);
    }
    tp_lua_document_decl declarations[TP_FORMAT_OUTPUT_MAX];
    for (int d = 0; d < document_count; ++d) {
        declarations[d].id = ctx->format->artifacts[d].id;
    }
    tp_lua_fact_value facts[TP_FORMAT_HOST_FACT_MAX];
    if (ctx->host_fact_count != ctx->format->host_fact_count ||
        (ctx->host_fact_count > 0 && !ctx->host_facts)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "Lua export host facts were not prepared by core");
    }
    for (int f = 0; f < ctx->host_fact_count; ++f) {
        facts[f] = (tp_lua_fact_value){.id = ctx->host_facts[f].id,
                                       .value = ctx->host_facts[f].value};
    }
    const tp_lua_projected_ir projected = {
        .value = ctx->ir,
        .page_images = page_images,
        .page_image_count = (size_t)ctx->ir->page_count,
        .polygons_visible = ctx->format->caps.polygons,
        .pivot_visible = ctx->format->caps.pivot,
        .slice9_visible = ctx->format->caps.slice9,
        .aliases_visible = ctx->format->caps.aliases,
    };
    const tp_lua_runtime_input input = {
        .source = binding->source,
        .source_byte_count = binding->source_byte_count,
        .format_id = ctx->format->id,
        .package_path = binding->source_path,
        .projected_ir = &projected,
        .documents = declarations,
        .document_count = (size_t)document_count,
        .facts = facts,
        .fact_count = (size_t)ctx->host_fact_count,
        .cancel = ctx->cancel,
    };
    tp_lua_runtime_result result = {0};
    tp_status status = tp_lua_runtime_serialize(&input, &result, error);
    if (status != TP_STATUS_OK) {
        const tp_format_diagnostic *diagnostic =
            tp_format_diagnostic_report_at(result.diagnostics, 0U);
        if (diagnostic && diagnostic->message) {
            (void)tp_error_set(error, status, "%s", diagnostic->message);
        }
        if (ctx->format_diagnostics) {
            *ctx->format_diagnostics = result.diagnostics;
            result.diagnostics = NULL;
        }
        tp_lua_runtime_result_destroy(&result);
        return status;
    }
    NT_ASSERT(result.document_count == (size_t)document_count);
    for (size_t n = 0U; n < result.notice_count; ++n) {
        status = ctx->notices
                     ? tp_export_notice_add_ex(
                           ctx->notices, TP_NOTICE_FIELD_NONE,
                           TP_NOTICE_REASON_NONE, NULL, ctx->format->id, "%s",
                           result.notices[n].message)
                     : TP_STATUS_OK;
        if (status != TP_STATUS_OK) {
            tp_lua_runtime_result_destroy(&result);
            return tp_error_set(error, status,
                                "Lua handler notice adoption failed");
        }
    }
    for (int d = 0; d < document_count; ++d) {
        NT_ASSERT(result.documents[d].id);
        NT_ASSERT(strcmp(result.documents[d].id,
                         ctx->format->artifacts[d].id) == 0);
        documents[d].data = result.documents[d].bytes;
        documents[d].size = result.documents[d].byte_count;
        documents[d].produced = true;
        result.documents[d].bytes = NULL;
        result.documents[d].byte_count = 0U;
    }
    tp_lua_runtime_result_destroy(&result);
    return TP_STATUS_OK;
}

static void lua_exporter_destroy(tp_exporter *exporter) {
    if (!exporter) {
        return;
    }
    tp_lua_export_binding *binding =
        (tp_lua_export_binding *)exporter->handler_context;
    if (binding) {
        free(binding->source_path);
        free(binding);
    }
    free(exporter);
}

static tp_status make_lua_exporter(
    const tp_format_descriptor *descriptor, const unsigned char *source,
    size_t source_byte_count, const char *package_path,
    tp_exporter **out, tp_error *error) {
    *out = NULL;
    tp_exporter *exporter = calloc(1U, sizeof *exporter);
    tp_lua_export_binding *context = calloc(1U, sizeof *context);
    if (!exporter || !context) {
        free(exporter);
        free(context);
        return tp_error_set(error, TP_STATUS_OOM,
                            "Lua worker exporter allocation failed");
    }
    const size_t path_length = strlen(package_path);
    static const char suffix[] = "/export.lua";
    context->source_path = malloc(path_length + sizeof suffix);
    if (!context->source_path) {
        free(context);
        free(exporter);
        return tp_error_set(error, TP_STATUS_OOM,
                            "Lua worker package path allocation failed");
    }
    memcpy(context->source_path, package_path, path_length);
    memcpy(context->source_path + path_length, suffix, sizeof suffix);
    context->source = source;
    context->source_byte_count = source_byte_count;
    exporter->format = descriptor;
    exporter->serialize = lua_export_serialize;
    exporter->handler_context = context;
    exporter->lua_handler = true;
    exporter->destroy = lua_exporter_destroy;
    *out = exporter;
    return TP_STATUS_OK;
}

static const tp_project_target *binding_project_target(
    const tp_project *project,
    const tp_format_binding_proto_target_ref *target_ref) {
    const tp_project_atlas *atlas = project
        ? tp_project_atlas_by_id(project, target_ref->atlas_id)
        : NULL;
    return atlas
        ? tp_project_atlas_target_by_id(atlas, target_ref->target_id)
        : NULL;
}

static tp_format_catalog_owned_row *worker_row_find(
    tp_format_catalog_owned_row *rows, size_t row_count, const char *key) {
    for (size_t i = 0U; i < row_count; ++i) {
        if (rows[i].key && strcmp(rows[i].key, key) == 0) {
            return &rows[i];
        }
    }
    return NULL;
}

static tp_status worker_add_resolution(
    tp_format_binding_proto_value *bindings,
    const tp_format_binding_proto_resolution *resolution,
    const char *expected_id, tp_format_catalog_owned_row *rows,
    size_t *row_count, tp_error *error) {
    if (!expected_id || expected_id[0] == '\0') {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "worker format resolution has no format id");
    }
    if (resolution->kind == TP_FORMAT_BINDING_RESOLUTION_ABSENT) {
        return TP_STATUS_OK;
    }
    if (resolution->kind == TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE) {
        tp_format_catalog_owned_row *existing = worker_row_find(
            rows, *row_count, expected_id);
        if (existing) {
            return existing->available
                ? tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                               "worker format resolutions disagree for '%s'",
                               expected_id)
                : TP_STATUS_OK;
        }
        if (*row_count >= TP_FORMAT_PACKAGE_MAX) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "worker format catalog exceeds its row cap");
        }
        tp_format_catalog_owned_row *row = &rows[(*row_count)++];
        row->implementation = TP_FORMAT_IMPLEMENTATION_LUA;
        row->key = adapter_strdup(expected_id);
        const size_t offset = resolution->diagnostic_offset;
        const size_t count = resolution->diagnostic_count;
        return row->key
            ? tp_format_diagnostic_report_materialize_internal(
                  bindings->diagnostics + offset, count, &row->diagnostics,
                  error)
            : tp_error_set(error, TP_STATUS_OOM,
                           "worker unavailable-format identity allocation failed");
    }

    tp_format_binding_proto_binding *binding =
        &bindings->bindings[resolution->binding_index];
    if (!binding->descriptor ||
        strcmp(binding->descriptor->id, expected_id) != 0) {
        return tp_error_set(
            error, TP_STATUS_INVALID_ARGUMENT,
            "worker exact binding does not match format '%s'", expected_id);
    }
    if (binding->implementation == TP_FORMAT_IMPLEMENTATION_NATIVE) {
        return TP_STATUS_OK;
    }
    tp_format_catalog_owned_row *existing = worker_row_find(
        rows, *row_count, expected_id);
    if (existing) {
        return existing->available &&
                       strcmp(existing->fingerprint, binding->fingerprint) == 0
            ? TP_STATUS_OK
            : tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                           "worker format resolutions disagree for '%s'",
                           expected_id);
    }
    if (*row_count >= TP_FORMAT_PACKAGE_MAX) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "worker format catalog exceeds its row cap");
    }
    tp_format_catalog_owned_row *row = &rows[(*row_count)++];
    row->implementation = TP_FORMAT_IMPLEMENTATION_LUA;
    row->available = true;
    row->key = adapter_strdup(expected_id);
    memcpy(row->fingerprint, binding->fingerprint, sizeof row->fingerprint);
    if (binding->owned_descriptor) {
        row->owned_descriptor = binding->owned_descriptor;
        binding->owned_descriptor = NULL;
        row->descriptor_bytes = (unsigned char *)binding->descriptor_bytes;
        row->descriptor_byte_count = binding->descriptor_byte_count;
        binding->descriptor_bytes = NULL;
        binding->descriptor_byte_count = 0U;
        row->source_bytes = (unsigned char *)binding->source_bytes;
        row->source_byte_count = binding->source_byte_count;
        binding->source_bytes = NULL;
        binding->source_byte_count = 0U;
        row->package_path = (char *)binding->package_path;
        binding->package_path = NULL;
    } else {
        row->package_path = adapter_strdup(binding->package_path);
        row->source_bytes = malloc(binding->source_byte_count);
        if (row->source_bytes) {
            memcpy(row->source_bytes, binding->source_bytes,
                   binding->source_byte_count);
            row->source_byte_count = binding->source_byte_count;
        }
    }
    if (!row->key || !row->package_path || !row->source_bytes) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "Lua worker catalog identity allocation failed");
    }
    const tp_format_descriptor *descriptor = row->owned_descriptor
        ? tp_format_owned_descriptor_view(row->owned_descriptor)
        : binding->descriptor;
    tp_exporter *exporter = NULL;
    tp_status status = make_lua_exporter(
        descriptor, row->source_bytes, row->source_byte_count,
        row->package_path, &exporter, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    row->exporter_binding = exporter;
    row->owns_exporter_binding = true;
    return TP_STATUS_OK;
}

tp_status tp_lua_export_catalog_create_worker(
    tp_format_binding_proto_value *bindings, const tp_project *project,
    const char *preview_format_id,
    tp_format_catalog **out_catalog, tp_error *error) {
    if (out_catalog) {
        *out_catalog = NULL;
    }
    if (!bindings || !out_catalog) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "Lua worker catalog requires bindings and output");
    }
    tp_format_catalog_owned_row *rows =
        calloc(TP_FORMAT_PACKAGE_MAX, sizeof *rows);
    if (!rows) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "Lua worker catalog row allocation failed");
    }
    size_t row_count = 0U;
    tp_status status = TP_STATUS_OK;
    if (bindings->preview.kind != TP_FORMAT_BINDING_RESOLUTION_ABSENT) {
        status = worker_add_resolution(
            bindings, &bindings->preview, preview_format_id, rows,
            &row_count, error);
    }
    if (status == TP_STATUS_OK && bindings->target_count > 0U && !project) {
        status = tp_error_set(
            error, TP_STATUS_INVALID_ARGUMENT,
            "worker target bindings require their admitted project");
    }
    for (size_t i = 0U; status == TP_STATUS_OK &&
                        i < bindings->target_count; ++i) {
        const tp_project_target *target = binding_project_target(
            project, &bindings->targets[i]);
        if (!target) {
            status = tp_error_set(
                error, TP_STATUS_INVALID_ARGUMENT,
                "worker target binding does not match a stable project target");
            break;
        }
        status = worker_add_resolution(
            bindings, &bindings->targets[i].resolution,
            target->exporter_id, rows, &row_count, error);
    }
    if (status != TP_STATUS_OK) {
        tp_format_catalog_owned_rows_destroy_internal(rows, row_count);
        return status;
    }
    tp_format_catalog *catalog = tp_format_catalog_create_owned_internal(
        NULL, rows, row_count, NULL, false, false, error);
    if (!catalog) {
        tp_format_catalog_owned_rows_destroy_internal(rows, row_count);
        return error && error->msg[0] ? TP_STATUS_OOM : TP_STATUS_INVALID_ARGUMENT;
    }
    *out_catalog = catalog;
    return TP_STATUS_OK;
}
