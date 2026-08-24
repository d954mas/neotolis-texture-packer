#ifndef NTPACKER_APP_FORMAT_CATALOG_H
#define NTPACKER_APP_FORMAT_CATALOG_H

#include "tp_core/tp_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Process-host catalog bootstrap. Discovery, isolated compilation, admission,
 * and fallback stay shared here instead of being duplicated by clients. */
typedef enum app_format_catalog_state {
    APP_FORMAT_CATALOG_CLOSED = 0,
    APP_FORMAT_CATALOG_ACTIVE = 1,
    APP_FORMAT_CATALOG_NATIVE_FALLBACK,
} app_format_catalog_state;

typedef struct app_format_catalog {
    app_format_catalog_state state;
    tp_format_catalog *catalog;
    tp_status reason_status;
    tp_error reason;
    tp_format_diagnostic_report *failure_diagnostics;
} app_format_catalog;

/* Both entry points return TP_STATUS_OK with an owned, non-NULL catalog once
 * `out` is valid. Discovery/path/admission failures are successful native
 * fallback states whose stable status, value-owned error, and optional owned
 * report remain in the result for the host's diagnostics surface. */
tp_status app_format_catalog_open_startup(app_format_catalog *out,
                                          tp_error *err);
tp_status app_format_catalog_open_startup_at_root(const char *root,
                                                  app_format_catalog *out,
                                                  tp_error *err);
void app_format_catalog_close(app_format_catalog *catalog);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_APP_FORMAT_CATALOG_H */
