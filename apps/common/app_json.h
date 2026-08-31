#ifndef NTPACKER_APP_JSON_H
#define NTPACKER_APP_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "tp_core/tp_error.h"

/* Owned cJSON tree or NULL with a structured error. Inputs are length-bounded
 * by the owning file/stream reader before this shared syntax boundary. */
cJSON *app_json_parse(const char *bytes, size_t length, tp_error *err);
/* Closed object key set, including duplicate rejection. Required fields and
 * values remain the caller's typed command/policy boundary. */
bool app_json_object_fields(const cJSON *object,
                            const char *const *keys, size_t count);

/* Borrow the original value spelling of a validated object's member. `object`
 * must be the tree parsed from these same bytes. Useful for nested versioned
 * payloads whose byte limits/numbers must reach their own decoder unchanged. */
const char *app_json_member_span(const char *bytes, size_t length,
    const cJSON *object, const char *key, size_t *value_length);

/* Exact nonnegative JSON-safe integer, tested on the original decimal token
 * from an already parsed JSON document.
 * Unlike a double round-trip, this cannot accept a fractional revision. */
bool app_json_safe_integer(const char *number, size_t length);

#endif
