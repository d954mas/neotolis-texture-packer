#ifndef NTPACKER_GUI_PACK_INTERNAL_H
#define NTPACKER_GUI_PACK_INTERNAL_H

#include "gui_pack.h"

#include "tp_core/tp_job.h"
#include "tp_core/tp_pack_result_cache.h"

/* Direct handoff from the session-job adapter to the preview result owner. */
bool gui_pack_publish_native(tp_session_job_result *job_result,
                             int atlas_index, double elapsed_ms,
                             gui_pack_result_info *out);
bool gui_pack_preview_publish(tp_session_job_result *job_result,
                              int atlas_index, double elapsed_ms,
                              gui_pack_result_info *out);
bool gui_pack_preview_belongs_to(int atlas_index);

#ifdef TP_ENABLE_TEST_SEAMS
/* Test-only observation of the native completion freshness decision. */
bool gui_pack__test_native_pack_input_changed_since(
    const tp_session_pack_job_result *pack);
/* Test-only observation of the pack-result store behind this boundary (S18):
 * residency and LRU accounting are not presentation state, so nothing outside a
 * test may read them. Zeroed when no store exists yet. */
void gui_pack__test_result_cache_stats(tp_pack_result_cache_stats *out);
/* Test-only: shrink the store's byte budget so an eviction is reachable without
 * packing hundreds of megabytes. Rebuilds an EMPTY store, so every previously
 * published result is released first; callers treat it as gui_pack_clear(-1).
 * 0 restores the shipped budget, which gui_pack_shutdown does on every teardown
 * so an override can never outlive the case that asked for it. */
void gui_pack__test_set_result_budget(uint64_t byte_budget);
#endif

#endif /* NTPACKER_GUI_PACK_INTERNAL_H */
