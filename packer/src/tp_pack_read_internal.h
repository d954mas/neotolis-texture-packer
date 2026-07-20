#ifndef TP_PACK_READ_INTERNAL_H
#define TP_PACK_READ_INTERNAL_H

/* Private UV recovery and page-name helpers shared with focused pack-reader
 * tests. Canonical D4 geometry is public in tp_core/tp_transform.h. */

#include <stdint.h>

#include "tp_core/tp_error.h"

struct tp_arena;

#ifdef __cplusplus
extern "C" {
#endif

/* Exact UV<->pixel conversions for page dims <= 4096 (plan §2.5). Encode is the
 * builder's idealized round-half-up; decode inverts it exactly. */
uint16_t tp_px_to_uv(int32_t px, int32_t page_dim);
int32_t tp_uv_to_px(uint16_t u, int32_t page_dim);

/* Builds the arena-owned public page identifier without a fixed intermediate
 * buffer. Long atlas names remain exact; allocation/overflow is reported. */
tp_status tp_pack_read_page_name(const char *atlas_name, uint16_t page_count,
                                 uint16_t page_index, struct tp_arena *arena,
                                 const char **out_name, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_PACK_READ_INTERNAL_H */
