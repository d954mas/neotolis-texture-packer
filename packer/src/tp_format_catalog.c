#include "tp_format_catalog_internal.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/nt_assert.h"
#include "tp_core/tp_export.h"
#include "tp_export_internal.h"
#include "tp_format_binding_proto_internal.h"

struct tp_format_catalog {
    atomic_size_t reference_count;
    bool immortal;
    char *root;
    tp_format_catalog_owned_row *rows;
    size_t row_count;
    tp_format_diagnostic_report *root_diagnostics;
    bool root_missing;
    bool limit_fail_closed;
};

static tp_format_catalog g_native_catalog = {
    .reference_count = 1U,
    .immortal = true,
};

static void owned_row_destroy(tp_format_catalog_owned_row *row) {
    if (!row) {
        return;
    }
    if (row->owns_exporter_binding && row->exporter_binding) {
        tp_exporter *binding = (tp_exporter *)row->exporter_binding;
        if (binding->destroy) {
            binding->destroy(binding);
        } else {
            free(binding);
        }
    }
    free(row->key);
    free(row->package_path);
    tp_format_owned_descriptor_destroy(row->owned_descriptor);
    tp_format_diagnostic_report_destroy(row->diagnostics);
    free(row->descriptor_bytes);
    free(row->source_bytes);
    memset(row, 0, sizeof *row);
}

void tp_format_catalog_owned_rows_destroy_internal(
    tp_format_catalog_owned_row *rows, size_t row_count) {
    for (size_t i = 0U; i < row_count; ++i) {
        owned_row_destroy(&rows[i]);
    }
    free(rows);
}

tp_format_catalog *tp_format_catalog_native(void) {
    return &g_native_catalog;
}

tp_format_catalog *tp_format_catalog_retain(tp_format_catalog *catalog) {
    if (!catalog || catalog->immortal) {
        return catalog;
    }
    size_t current = atomic_load_explicit(&catalog->reference_count,
                                          memory_order_relaxed);
    for (;;) {
        if (current == 0U || current == SIZE_MAX) {
            return NULL;
        }
        if (atomic_compare_exchange_weak_explicit(
                &catalog->reference_count, &current, current + 1U,
                memory_order_relaxed, memory_order_relaxed)) {
            return catalog;
        }
    }
}

void tp_format_catalog_release(tp_format_catalog *catalog) {
    if (!catalog || catalog->immortal) {
        return;
    }
    if (atomic_fetch_sub_explicit(&catalog->reference_count, 1U,
                                  memory_order_acq_rel) != 1U) {
        return;
    }
    tp_format_catalog_owned_rows_destroy_internal(
        catalog->rows, catalog->row_count);
    free(catalog->root);
    tp_format_diagnostic_report_destroy(catalog->root_diagnostics);
    free(catalog);
}

tp_format_catalog *tp_format_catalog_create_owned_internal(
    char *owned_root, tp_format_catalog_owned_row *owned_rows,
    size_t owned_row_count,
    tp_format_diagnostic_report *owned_root_diagnostics,
    bool root_missing, bool limit_fail_closed, tp_error *error) {
    if ((owned_row_count > 0U && !owned_rows) ||
        owned_row_count > TP_FORMAT_PACKAGE_MAX) {
        tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                     "invalid owned format catalog materialization");
        return NULL;
    }
    tp_format_catalog *catalog =
        (tp_format_catalog *)calloc(1, sizeof *catalog);
    if (!catalog) {
        tp_error_set(error, TP_STATUS_OOM,
                     "format catalog allocation failed");
        return NULL;
    }
    atomic_init(&catalog->reference_count, 1U);
    catalog->root = owned_root;
    catalog->rows = owned_rows;
    catalog->row_count = owned_row_count;
    catalog->root_diagnostics = owned_root_diagnostics;
    catalog->root_missing = root_missing;
    catalog->limit_fail_closed = limit_fail_closed;
    return catalog;
}

size_t tp_format_catalog_row_count(const tp_format_catalog *catalog) {
    return catalog ? (size_t)tp_native_exporter_count() + catalog->row_count
                   : 0U;
}

bool tp_format_catalog_row_at(const tp_format_catalog *catalog, size_t index,
                              tp_format_catalog_row *out) {
    if (!catalog || !out) {
        return false;
    }
    memset(out, 0, sizeof *out);
    const size_t native_count = (size_t)tp_native_exporter_count();
    if (index < native_count) {
        const tp_exporter *exporter = tp_native_exporter_at((int)index);
        if (!exporter) {
            return false;
        }
        *out = (tp_format_catalog_row){
            .implementation = TP_FORMAT_IMPLEMENTATION_NATIVE,
            .available = true,
            .key = exporter->format->id,
            .descriptor = exporter->format,
        };
        return true;
    }
    index -= native_count;
    if (index >= catalog->row_count) {
        return false;
    }
    const tp_format_catalog_owned_row *row = &catalog->rows[index];
    *out = (tp_format_catalog_row){
        .implementation = row->implementation,
        .available = row->available,
        .key = row->key,
        .package_path = row->package_path,
        .fingerprint = row->fingerprint[0] ? row->fingerprint : NULL,
        .descriptor = tp_format_owned_descriptor_view(row->owned_descriptor),
        .diagnostics = row->diagnostics,
    };
    if (!out->descriptor && row->exporter_binding) {
        out->descriptor = row->exporter_binding->format;
    }
    return true;
}

static const tp_format_catalog_owned_row *find_owned_format_row(
    const tp_format_catalog *catalog, const char *id) {
    if (!catalog || !id) {
        return NULL;
    }
    for (size_t i = 0U; i < catalog->row_count; ++i) {
        const tp_format_catalog_owned_row *row = &catalog->rows[i];
        const tp_format_descriptor *descriptor =
            tp_format_owned_descriptor_view(row->owned_descriptor);
        if (!descriptor && row->exporter_binding) {
            descriptor = row->exporter_binding->format;
        }
        if (descriptor && descriptor->id && strcmp(descriptor->id, id) == 0) {
            return row;
        }
    }
    return NULL;
}

static const tp_format_catalog_owned_row *find_unavailable_package_row(
    const tp_format_catalog *catalog, const char *key) {
    if (!catalog || !key) {
        return NULL;
    }
    for (size_t i = 0U; i < catalog->row_count; ++i) {
        const tp_format_catalog_owned_row *row = &catalog->rows[i];
        if (!row->available && row->key && strcmp(row->key, key) == 0) {
            return row;
        }
    }
    return NULL;
}

const tp_format_descriptor *tp_format_catalog_find_available(
    const tp_format_catalog *catalog, const char *id) {
    if (!catalog || tp_exporter_id_validate(id, NULL) != TP_STATUS_OK) {
        return NULL;
    }
    const tp_exporter *native = tp_native_exporter_find(id);
    if (native) {
        return native->format;
    }
    const tp_format_catalog_owned_row *row =
        find_owned_format_row(catalog, id);
    if (!row || !row->available) {
        return NULL;
    }
    if (row->owned_descriptor) {
        return tp_format_owned_descriptor_view(row->owned_descriptor);
    }
    return row->exporter_binding ? row->exporter_binding->format : NULL;
}

tp_status tp_format_catalog_resolve(const tp_format_catalog *catalog,
                                    const char *id,
                                    tp_format_resolution *out,
                                    tp_error *error) {
    if (!catalog || !out) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format resolution requires a catalog and valid id");
    }
    const tp_status id_status = tp_exporter_id_validate(id, error);
    if (id_status != TP_STATUS_OK) {
        return id_status;
    }
    memset(out, 0, sizeof *out);
    const tp_exporter *native = tp_native_exporter_find(id);
    if (native) {
        out->state = TP_FORMAT_RESOLUTION_AVAILABLE;
        out->implementation = TP_FORMAT_IMPLEMENTATION_NATIVE;
        out->descriptor = native->format;
        return TP_STATUS_OK;
    }
    const tp_format_catalog_owned_row *row =
        find_owned_format_row(catalog, id);
    if (!row) {
        row = find_unavailable_package_row(catalog, id);
    }
    if (!row) {
        out->state = TP_FORMAT_RESOLUTION_ABSENT;
        return TP_STATUS_OK;
    }
    out->state = row->available ? TP_FORMAT_RESOLUTION_AVAILABLE
                               : TP_FORMAT_RESOLUTION_UNAVAILABLE;
    out->implementation = row->implementation;
    out->diagnostics = row->diagnostics;
    if (row->available) {
        out->descriptor = row->owned_descriptor
                              ? tp_format_owned_descriptor_view(
                                    row->owned_descriptor)
                              : row->exporter_binding->format;
    }
    return TP_STATUS_OK;
}

const tp_format_diagnostic_report *tp_format_catalog_root_diagnostics(
    const tp_format_catalog *catalog) {
    return catalog ? catalog->root_diagnostics : NULL;
}

const char *tp_format_catalog_root(const tp_format_catalog *catalog) {
    return catalog ? catalog->root : NULL;
}

bool tp_format_catalog_root_missing(const tp_format_catalog *catalog) {
    return catalog && catalog->root_missing;
}

bool tp_format_catalog_limit_fail_closed(const tp_format_catalog *catalog) {
    return catalog && catalog->limit_fail_closed;
}

const tp_exporter *tp_format_catalog_exporter_find(
    const tp_format_catalog *catalog, const char *id) {
    if (!catalog) {
        return NULL;
    }
    const tp_exporter *native = tp_native_exporter_find(id);
    if (native) {
        return native;
    }
    const tp_format_catalog_owned_row *row =
        find_owned_format_row(catalog, id);
    return row && row->available ? row->exporter_binding : NULL;
}

static tp_status capture_append_diagnostics(
    const tp_format_diagnostic_report *report,
    tp_format_binding_proto_value *value,
    tp_format_binding_proto_resolution *resolution, tp_error *error) {
    const size_t count = tp_format_diagnostic_report_count(report);
    if (count == 0U) {
        resolution->diagnostic_offset = 0U;
        resolution->diagnostic_count = 0U;
        return TP_STATUS_OK;
    }
    if (count > TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS -
                    value->diagnostic_count) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "format binding diagnostic count exceeds its cap");
    }
    const size_t needed = value->diagnostic_count + count;
    tp_format_diagnostic *grown = realloc(
        value->diagnostics, needed * sizeof *grown);
    if (!grown) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "format binding diagnostic capture failed");
    }
    value->diagnostics = grown;
    resolution->diagnostic_offset = (uint32_t)value->diagnostic_count;
    resolution->diagnostic_count = (uint32_t)count;
    for (size_t i = 0U; i < count; ++i) {
        const tp_format_diagnostic *diagnostic =
            tp_format_diagnostic_report_at(report, i);
        NT_ASSERT(diagnostic);
        value->diagnostics[value->diagnostic_count++] = *diagnostic;
    }
    return TP_STATUS_OK;
}

static tp_status capture_resolution(
    const tp_format_catalog *catalog, const char *format_id,
    tp_format_binding_proto_value *value,
    tp_format_binding_proto_resolution *out, tp_error *error) {
    memset(out, 0, sizeof *out);
    if (!format_id || format_id[0] == '\0') {
        out->kind = TP_FORMAT_BINDING_RESOLUTION_ABSENT;
        return TP_STATUS_OK;
    }
    const tp_exporter *native = tp_native_exporter_find(format_id);
    const tp_format_catalog_owned_row *row =
        find_owned_format_row(catalog, format_id);
    if (!row) {
        row = find_unavailable_package_row(catalog, format_id);
    }
    const tp_format_descriptor *descriptor =
        native ? native->format
               : row && row->owned_descriptor
                     ? tp_format_owned_descriptor_view(row->owned_descriptor)
                     : row && row->exporter_binding
                           ? row->exporter_binding->format
                           : NULL;
    if (!native && (!row || !row->available)) {
        out->kind = row ? TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE
                        : TP_FORMAT_BINDING_RESOLUTION_ABSENT;
        out->binding_index = UINT32_MAX;
        return row ? capture_append_diagnostics(row->diagnostics, value, out,
                                                error)
                   : TP_STATUS_OK;
    }
    NT_ASSERT(descriptor);
    size_t binding_index = value->binding_count;
    for (size_t i = 0U; i < value->binding_count; ++i) {
        if (value->bindings[i].descriptor &&
            strcmp(value->bindings[i].descriptor->id, descriptor->id) == 0 &&
            (native || strcmp(value->bindings[i].fingerprint,
                              row->fingerprint) == 0)) {
            binding_index = i;
            break;
        }
    }
    if (binding_index == value->binding_count) {
        if (value->binding_count >= TP_FORMAT_BINDING_PROTO_MAX_BINDINGS) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "format binding count exceeds its cap");
        }
        tp_format_binding_proto_binding *grown = realloc(
            value->bindings,
            (value->binding_count + 1U) * sizeof *grown);
        if (!grown) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "format binding capture allocation failed");
        }
        value->bindings = grown;
        tp_format_binding_proto_binding *binding =
            &value->bindings[value->binding_count++];
        memset(binding, 0, sizeof *binding);
        binding->implementation = native
                                      ? TP_FORMAT_IMPLEMENTATION_NATIVE
                                      : row->implementation;
        binding->descriptor = descriptor;
        if (!native && row->implementation == TP_FORMAT_IMPLEMENTATION_LUA) {
            binding->api_version = descriptor->api_version;
            binding->package_path = row->package_path;
            memcpy(binding->fingerprint, row->fingerprint,
                   sizeof binding->fingerprint);
            binding->descriptor_bytes = row->descriptor_bytes;
            binding->descriptor_byte_count = row->descriptor_byte_count;
            binding->source_bytes = row->source_bytes;
            binding->source_byte_count = row->source_byte_count;
        }
    }
    out->kind = TP_FORMAT_BINDING_RESOLUTION_BINDING;
    out->binding_index = (uint32_t)binding_index;
    return TP_STATUS_OK;
}

tp_status tp_format_catalog_encode_bindings_internal(
    const tp_format_catalog *catalog, const char *preview_format_id,
    const tp_format_binding_capture_target *targets, size_t target_count,
    uint8_t **out_bytes, size_t *out_length, tp_error *error) {
    if (out_bytes) {
        *out_bytes = NULL;
    }
    if (out_length) {
        *out_length = 0U;
    }
    if (!catalog || !out_bytes || !out_length ||
        target_count > TP_FORMAT_BINDING_PROTO_MAX_TARGET_REFS ||
        (target_count > 0U && !targets)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format binding capture requires a catalog and bounded targets");
    }
    tp_format_binding_proto_value value = {0};
    tp_status status = capture_resolution(catalog, preview_format_id, &value,
                                          &value.preview, error);
    if (status == TP_STATUS_OK && target_count > 0U) {
        value.targets = calloc(target_count, sizeof *value.targets);
        if (!value.targets) {
            status = tp_error_set(error, TP_STATUS_OOM,
                                  "format binding target capture failed");
        }
    }
    for (size_t i = 0U; status == TP_STATUS_OK && i < target_count; ++i) {
        value.targets[i].atlas_id = targets[i].atlas_id;
        value.targets[i].target_id = targets[i].target_id;
        status = capture_resolution(catalog, targets[i].format_id, &value,
                                    &value.targets[i].resolution, error);
    }
    value.target_count = status == TP_STATUS_OK ? target_count : 0U;
    if (status == TP_STATUS_OK) {
        status = tp_format_binding_proto_encode(&value, out_bytes, out_length,
                                                error);
    }
    free(value.bindings);
    free(value.targets);
    free(value.diagnostics);
    return status;
}
