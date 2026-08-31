#ifndef NTPACKER_APP_AUTOMATION_POLICY_H
#define NTPACKER_APP_AUTOMATION_POLICY_H

#include "tp_core/tp_error.h"

typedef enum app_automation_mode {
    APP_AUTOMATION_DISABLED = 0,
    APP_AUTOMATION_ASK,
    APP_AUTOMATION_ALLOW_ALL
} app_automation_mode;

/* Read <data_root>/automation/permissions.json on every call. Missing means
 * Ask; any invalid or unreadable document fails with Disabled in *mode.
 * This does not create directories, change policy, or cache an authorization. */
tp_status app_automation_policy_read(const char *data_root,
                                      app_automation_mode *mode,
                                      tp_error *err);

#endif
