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

#ifdef __cplusplus
extern "C" {
#endif

void tp_image__test_reset_decode_count(void);
uint64_t tp_image__test_decode_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TP_IMAGE_PRIV_H */
