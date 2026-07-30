#ifndef TP_CORE_SRC_TP_UTF8_INTERNAL_H
#define TP_CORE_SRC_TP_UTF8_INTERNAL_H

#include <stddef.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_utf8.h"

/* Strict Unicode-scalar UTF-8 admission shared by raw JSON ingress and every
 * persistent C-string boundary.  The caller chooses the domain-specific status
 * while this seam owns one byte-level definition of well-formed UTF-8. */
tp_status tp_utf8_validate_bytes(const char *text, size_t length,
                                 tp_status rejection_status,
                                 const char *context, tp_error *error);
tp_status tp_utf8_validate_c_string(const char *text,
                                    tp_status rejection_status,
                                    const char *context, tp_error *error);

/* Text-field admission shared by the worker wire codecs.  A wire text field
 * carries its length out of band, so an embedded NUL is a framing fault
 * (INVALID_ARGUMENT) and malformed bytes are INVALID_UTF8.  The per-field byte
 * cap stays with the codec that owns the field; this seam owns the byte-level
 * definition of an admissible text field so both codecs cannot drift. */
tp_status tp_utf8_validate_text_field(const void *bytes, size_t length,
                                      const char *context, tp_error *error);

#endif /* TP_CORE_SRC_TP_UTF8_INTERNAL_H */
