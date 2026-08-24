#include "app_format_catalog.h"

#include "core/nt_assert.h"
#include "tp_core/tp_build_worker.h"

#include <string.h>

static void install_native(app_format_catalog *out,
                           app_format_catalog_state state,
                           tp_status reason_status,
                           const tp_error *reason,
                           tp_format_diagnostic_report *failure_diagnostics) {
    out->state = state;
    out->catalog = tp_format_catalog_retain(tp_format_catalog_native());
    out->reason_status = reason_status;
    out->reason = reason ? *reason : (tp_error){{0}};
    out->failure_diagnostics = failure_diagnostics;
}

static tp_status open_scan(tp_format_catalog_scan *scan,
                           app_format_catalog *out) {
    const size_t compile_count = tp_format_catalog_scan_compile_count(scan);
    if (compile_count != 0U) {
        tp_format_catalog *catalog = NULL;
        tp_error reason = {{0}};
        const tp_status status = tp_build_format_catalog_compile(
            &scan, &catalog, &reason);
        tp_format_catalog_scan_destroy(scan);
        if (status != TP_STATUS_OK) {
            install_native(out, APP_FORMAT_CATALOG_NATIVE_FALLBACK,
                           status, &reason, NULL);
            return TP_STATUS_OK;
        }
        NT_ASSERT(catalog != NULL);
        out->state = APP_FORMAT_CATALOG_ACTIVE;
        out->catalog = catalog;
        out->reason_status = TP_STATUS_OK;
        return TP_STATUS_OK;
    }

    tp_format_catalog *catalog = NULL;
    tp_error reason = {{0}};
    const tp_status status = tp_format_catalog_scan_finish_without_compile(
        &scan, &catalog, &reason);
    tp_format_catalog_scan_destroy(scan);
    if (status != TP_STATUS_OK) {
        install_native(out, APP_FORMAT_CATALOG_NATIVE_FALLBACK, status, &reason,
                       NULL);
        return TP_STATUS_OK;
    }
    NT_ASSERT(catalog != NULL);

    out->state = APP_FORMAT_CATALOG_ACTIVE;
    out->catalog = catalog;
    out->reason_status = TP_STATUS_OK;
    return TP_STATUS_OK;
}

tp_status app_format_catalog_open_startup_at_root(const char *root,
                                                  app_format_catalog *out,
                                                  tp_error *error) {
    if (!out || !root || root[0] == '\0') {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format catalog startup requires root and output");
    }
    memset(out, 0, sizeof *out);

    tp_format_catalog_scan *scan = NULL;
    tp_format_diagnostic_report *failure_diagnostics = NULL;
    tp_error reason = {{0}};
    const tp_status status = tp_format_catalog_scan_root(
        root, &scan, &failure_diagnostics, &reason);
    if (status != TP_STATUS_OK) {
        tp_format_catalog_scan_destroy(scan);
        install_native(out, APP_FORMAT_CATALOG_NATIVE_FALLBACK, status,
                       &reason, failure_diagnostics);
        return TP_STATUS_OK;
    }
    NT_ASSERT(scan != NULL);
    tp_format_diagnostic_report_destroy(failure_diagnostics);
    return open_scan(scan, out);
}

tp_status app_format_catalog_open_startup(app_format_catalog *out,
                                          tp_error *error) {
    if (!out) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format catalog startup requires an output");
    }
    memset(out, 0, sizeof *out);

    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    tp_error reason = {{0}};
    const tp_status status =
        tp_format_root_from_executable(root, sizeof root, &reason);
    if (status != TP_STATUS_OK) {
        install_native(out, APP_FORMAT_CATALOG_NATIVE_FALLBACK, status,
                       &reason, NULL);
        return TP_STATUS_OK;
    }
    return app_format_catalog_open_startup_at_root(root, out, error);
}

void app_format_catalog_close(app_format_catalog *catalog) {
    if (!catalog) {
        return;
    }
    tp_format_catalog_release(catalog->catalog);
    tp_format_diagnostic_report_destroy(catalog->failure_diagnostics);
    memset(catalog, 0, sizeof *catalog);
}
