#ifndef NTPACKER_GUI_BOOTSTRAP_H
#define NTPACKER_GUI_BOOTSTRAP_H

#include "material/nt_material.h"

void gui_bootstrap_init(const char *exe_dir);
const nt_material_info_t *gui_bootstrap_step(void);
void gui_bootstrap_restore(void);
void gui_bootstrap_shutdown(void);

#endif /* NTPACKER_GUI_BOOTSTRAP_H */
