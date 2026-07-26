#ifndef TP_IMAGE_PRIV_H
#define TP_IMAGE_PRIV_H

/* Test-only decode accounting seam (tp_*__test_* pattern, always compiled).
 * Counts successful tp_image_load_file decodes in THIS process so a test can pin
 * "exactly one decode per source per pack" -- the redundant hash-compute decode
 * this guards against was removed when the Pack worker moved to a single decode
 * pass feeding the pack_input_hash. Never consulted by production code.
 *
 * Named *_priv.h (not *_internal.h) as a deliberately unregistered private header,
 * like tp_txn_parse_priv.h -- kept out of the check_boundaries R18 registry scan. */

#include <stdint.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private ingress variants used by the Pack/hash cache. The fingerprint is the
 * exact encoded byte stream read from the file, not filesystem metadata. */
tp_status tp_image_load_file_fingerprinted(const char *path_utf8,
                                           tp_image_rgba8 *out,
                                           tp_id128 *out_fingerprint,
                                           tp_error *err);
tp_status tp_image_file_fingerprint(const char *path_utf8,
                                    tp_id128 *out_fingerprint,
                                    tp_error *err);

void tp_image__test_reset_decode_count(void);
uint64_t tp_image__test_decode_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TP_IMAGE_PRIV_H */
