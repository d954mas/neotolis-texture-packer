#ifndef TP_BUILD_SRC_TP_LUA_EXPORT_ADAPTER_INTERNAL_H
#define TP_BUILD_SRC_TP_LUA_EXPORT_ADAPTER_INTERNAL_H

#include "tp_core/tp_error.h"
#include "tp_core/tp_format.h"
#include "tp_core/tp_project.h"
#include "tp_format_binding_proto_internal.h"

/* Worker-only materialization. Decoded Lua package ownership moves into the
 * returned catalog; `bindings` remains valid and can be freed immediately. */
tp_status tp_lua_export_catalog_create_worker(
    tp_format_binding_proto_value *bindings, const tp_project *project,
    const char *preview_format_id,
    tp_format_catalog **out_catalog, tp_error *error);

typedef void (*tp_lua_export_panic_marker_fn)(void *context);
void tp_lua_export_panic_marker_set_worker(
    tp_lua_export_panic_marker_fn marker, void *context);

#endif
