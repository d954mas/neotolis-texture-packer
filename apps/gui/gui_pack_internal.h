#ifndef NTPACKER_GUI_PACK_INTERNAL_H
#define NTPACKER_GUI_PACK_INTERNAL_H

#include "gui_pack.h"

#include "tp_core/tp_job.h"

/* Direct handoff from the session-job adapter to the preview result owner. */
bool gui_pack_publish_native(tp_session_pack_job_result *pack,
                             int atlas_index, double elapsed_ms,
                             gui_pack_result_info *out);
void gui_pack_preview_publish(tp_session_pack_job_result *pack,
                              int atlas_index, double elapsed_ms);
bool gui_pack_preview_belongs_to(int atlas_index);

#endif /* NTPACKER_GUI_PACK_INTERNAL_H */
