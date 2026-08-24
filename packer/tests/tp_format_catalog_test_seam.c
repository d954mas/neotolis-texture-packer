#ifndef TP_ENABLE_TEST_SEAMS
#error "tp_format_catalog_test_seam.c is test-only"
#endif

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_export.h"
#include "tp_export_internal.h"
#include "tp_format_catalog_internal.h"

static char *test_catalog_strdup(const char *text) {
    const size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1U);
    if (copy) {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

static void test_catalog_rows_destroy(tp_format_catalog_owned_row *rows,
                                      size_t row_count) {
    if (!rows) {
        return;
    }
    for (size_t i = 0U; i < row_count; ++i) {
        free(rows[i].key);
    }
    free(rows);
}

tp_status tp_format_catalog__test_create_with_keys(
    const tp_exporter *const *exporters, const char *const *keys,
    size_t exporter_count,
    tp_format_catalog **out, tp_error *error) {
    if (!out || (exporter_count > 0U && !exporters) ||
        exporter_count > TP_FORMAT_PACKAGE_MAX) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "test catalog requires valid exporters and output");
    }
    *out = NULL;
    tp_format_catalog_owned_row *rows =
        exporter_count > 0U
            ? (tp_format_catalog_owned_row *)calloc(exporter_count,
                                                     sizeof *rows)
            : NULL;
    if (exporter_count > 0U && !rows) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "test catalog row allocation failed");
    }
    for (size_t i = 0U; i < exporter_count; ++i) {
        if (!exporters[i] || !exporters[i]->format ||
            !exporters[i]->format->id || !exporters[i]->serialize) {
            test_catalog_rows_destroy(rows, i);
            return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                "test catalog exporter is invalid");
        }
        const tp_status id_status =
            tp_exporter_id_validate(exporters[i]->format->id, error);
        if (id_status != TP_STATUS_OK ||
            tp_native_exporter_find(exporters[i]->format->id)) {
            test_catalog_rows_destroy(rows, i);
            return id_status != TP_STATUS_OK
                       ? id_status
                       : tp_error_set(
                             error, TP_STATUS_INVALID_ARGUMENT,
                             "test catalog exporter is invalid or duplicate");
        }
        for (size_t j = 0U; j < i; ++j) {
            if (strcmp(exporters[j]->format->id,
                       exporters[i]->format->id) == 0) {
                test_catalog_rows_destroy(rows, i);
                return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                    "test catalog contains duplicate exporters");
            }
        }
        rows[i].implementation = TP_FORMAT_IMPLEMENTATION_NATIVE;
        rows[i].available = true;
        rows[i].exporter_binding = exporters[i];
        rows[i].key = test_catalog_strdup(
            keys && keys[i] ? keys[i] : exporters[i]->format->id);
        if (!rows[i].key) {
            test_catalog_rows_destroy(rows, i + 1U);
            return tp_error_set(error, TP_STATUS_OOM,
                                "test catalog key allocation failed");
        }
    }
    tp_format_catalog *catalog = tp_format_catalog_create_owned_internal(
        NULL, rows, exporter_count, NULL, false, false, error);
    if (!catalog) {
        test_catalog_rows_destroy(rows, exporter_count);
        return TP_STATUS_OOM;
    }
    *out = catalog;
    return TP_STATUS_OK;
}

tp_status tp_format_catalog__test_create(
    const tp_exporter *const *exporters, size_t exporter_count,
    tp_format_catalog **out, tp_error *error) {
    return tp_format_catalog__test_create_with_keys(
        exporters, NULL, exporter_count, out, error);
}
