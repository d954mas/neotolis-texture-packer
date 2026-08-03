#include "tp_format_catalog_internal.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_export.h"
#include "tp_export_internal.h"

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
    free(row->key);
    free(row->package_path);
    tp_format_owned_descriptor_destroy(row->owned_descriptor);
    tp_format_diagnostic_report_destroy(row->diagnostics);
    free(row->descriptor_bytes);
    free(row->source_bytes);
    memset(row, 0, sizeof *row);
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
    for (size_t i = 0U; i < catalog->row_count; ++i) {
        owned_row_destroy(&catalog->rows[i]);
    }
    free(catalog->rows);
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
    if (!out->descriptor && row->native_binding) {
        out->descriptor = row->native_binding->format;
    }
    return true;
}

static const tp_format_catalog_owned_row *find_owned_row(
    const tp_format_catalog *catalog, const char *id) {
    if (!catalog || !id) {
        return NULL;
    }
    for (size_t i = 0U; i < catalog->row_count; ++i) {
        const tp_format_catalog_owned_row *row = &catalog->rows[i];
        const tp_format_descriptor *descriptor =
            tp_format_owned_descriptor_view(row->owned_descriptor);
        if (!descriptor && row->native_binding) {
            descriptor = row->native_binding->format;
        }
        if (descriptor && descriptor->id && strcmp(descriptor->id, id) == 0) {
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
    const tp_format_catalog_owned_row *row = find_owned_row(catalog, id);
    if (!row || !row->available) {
        return NULL;
    }
    if (row->owned_descriptor) {
        return tp_format_owned_descriptor_view(row->owned_descriptor);
    }
    return row->native_binding ? row->native_binding->format : NULL;
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
    const tp_format_catalog_owned_row *row = find_owned_row(catalog, id);
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
                              : row->native_binding->format;
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
    const tp_format_catalog_owned_row *row = find_owned_row(catalog, id);
    return row && row->available ? row->native_binding : NULL;
}

tp_status tp_format_catalog_package_snapshot_create(
    const tp_format_catalog *catalog, const char *id,
    tp_format_package_snapshot *out, tp_error *error) {
    if (!catalog || !id || !out) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format package snapshot requires catalog, id, and output");
    }
    memset(out, 0, sizeof *out);
    const tp_format_catalog_owned_row *row = find_owned_row(catalog, id);
    if (!row || !row->available ||
        row->implementation != TP_FORMAT_IMPLEMENTATION_LUA ||
        !row->owned_descriptor) {
        return tp_error_set(error, TP_STATUS_NOT_FOUND,
                            "available runtime format package '%s' was not found", id);
    }
    if (row->descriptor_byte_count >
        SIZE_MAX - row->source_byte_count) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "format package snapshot size overflow");
    }
    tp_format_owned_descriptor *descriptor =
        tp_format_owned_descriptor_clone(
            tp_format_owned_descriptor_view(row->owned_descriptor), error);
    if (!descriptor) {
        return TP_STATUS_OOM;
    }
    const size_t total = row->descriptor_byte_count + row->source_byte_count;
    unsigned char *storage =
        total > 0U ? (unsigned char *)malloc(total) : NULL;
    if (total > 0U && !storage) {
        tp_format_owned_descriptor_destroy(descriptor);
        return tp_error_set(error, TP_STATUS_OOM,
                            "format package snapshot allocation failed");
    }
    if (row->descriptor_byte_count > 0U) {
        memcpy(storage, row->descriptor_bytes, row->descriptor_byte_count);
    }
    if (row->source_byte_count > 0U) {
        memcpy(storage + row->descriptor_byte_count, row->source_bytes,
               row->source_byte_count);
    }
    out->implementation = row->implementation;
    out->descriptor = tp_format_owned_descriptor_view(descriptor);
    memcpy(out->fingerprint, row->fingerprint, sizeof out->fingerprint);
    out->descriptor_bytes = storage;
    out->descriptor_byte_count = row->descriptor_byte_count;
    out->source_bytes = storage + row->descriptor_byte_count;
    out->source_byte_count = row->source_byte_count;
    /* One small owner tuple keeps the public DTO layout opaque. */
    void **owners = (void **)malloc(2U * sizeof *owners);
    if (!owners) {
        free(storage);
        tp_format_owned_descriptor_destroy(descriptor);
        memset(out, 0, sizeof *out);
        return tp_error_set(error, TP_STATUS_OOM,
                            "format package snapshot owner allocation failed");
    }
    owners[0] = descriptor;
    owners[1] = storage;
    out->owned_storage = owners;
    return TP_STATUS_OK;
}

void tp_format_package_snapshot_destroy(tp_format_package_snapshot *snapshot) {
    if (!snapshot) {
        return;
    }
    void **owners = (void **)snapshot->owned_storage;
    if (owners) {
        tp_format_owned_descriptor_destroy(
            (tp_format_owned_descriptor *)owners[0]);
        free(owners[1]);
        free(owners);
    }
    memset(snapshot, 0, sizeof *snapshot);
}
