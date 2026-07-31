#include "gui_actions_internal.h"

#include <stdio.h>
#include <string.h>

#include "gui_state.h"
#include "gui_project.h"
#include "tinyfiledialogs.h"
// #region R6b startup crash-recovery modal glue
void gui_actions_open_recovery(const gui_recovery_list *list) {
    s_actions.recovery = (gui_recovery_flow){
        .pending_row = -1,
    };
    if (!list || list->count == 0U) {
        return;
    }
    s_actions.recovery.list = *list;
    s_actions.recovery.phase =
        GUI_RECOVERY_CHOOSE;
}

gui_recovery_view gui_actions_recovery_view(void) {
    return (gui_recovery_view){
        .phase = s_actions.recovery.phase,
        .count =
            (int)s_actions.recovery.list.count,
        .has_more =
            s_actions.recovery.list.has_more,
    };
}

bool gui_actions_recovery_active(void) {
    return s_actions.recovery.phase !=
           GUI_RECOVERY_IDLE;
}

const gui_recovery_entry *gui_actions_recovery_at(int i) {
    if (i < 0 ||
        (size_t)i >=
            s_actions.recovery.list.count) {
        return NULL;
    }
    return &s_actions.recovery.list.items[i];
}

bool gui_actions_recovery_request(
    int row, gui_recovery_action action) {
    const gui_recovery_entry *entry =
        gui_actions_recovery_at(row);
    if (s_actions.recovery.phase !=
            GUI_RECOVERY_CHOOSE ||
        !entry ||
        action < GUI_RECOVERY_DISCARD ||
        action > GUI_RECOVERY_SAVE_AS ||
        (!entry->adoptable &&
         action != GUI_RECOVERY_DISCARD) ||
        (action == GUI_RECOVERY_SAVE_ORIGINAL &&
         entry->original_path[0] == '\0')) {
        return false;
    }
    s_actions.recovery.phase =
        GUI_RECOVERY_RESOLVING;
    s_actions.recovery.pending_row = row;
    s_actions.recovery.pending_action = action;
    return true;
}

void gui_actions_recovery_dismiss(void) {
    s_actions.recovery =
        (gui_recovery_flow){
            .pending_row = -1,
        };
}
/* Drop a resolved row (shift-down); close the modal when the list empties. */
static void recovery_remove_row(int row) {
    if (row < 0 ||
        (size_t)row >=
            s_actions.recovery.list.count) {
        return;
    }
    for (size_t i = (size_t)row;
         i + 1U < s_actions.recovery.list.count;
         ++i) {
        s_actions.recovery.list.items[i] =
            s_actions.recovery.list.items[i + 1];
    }
    s_actions.recovery.list.count--;
    if (s_actions.recovery.list.count == 0U &&
        !s_actions.recovery.list.has_more) {
        gui_actions_recovery_dismiss();
    }
}
// #endregion

void gui_actions__apply_recovery(void) {
    /* R6b: harvest a per-row recovery decision requested last frame. Runs here (same spot the confirm
     * harvest lands do_save()->tinyfd_*) so the Save-As dialog + disk-mutating resolve run outside
     * nt_ui_begin/end. NON-DESTRUCTIVE ON FAILURE: a failed save leaves the journal + the row for a retry. */
    if (s_actions.recovery.phase ==
        GUI_RECOVERY_RESOLVING) {
        const int row =
            s_actions.recovery.pending_row;
        const gui_recovery_action action =
            s_actions.recovery.pending_action;
        s_actions.recovery.phase =
            GUI_RECOVERY_CHOOSE;
        s_actions.recovery.pending_row = -1;
        const gui_recovery_entry *e = gui_actions_recovery_at(row);
        if (e != NULL) {
            /* Copy the typed row before the list may compact after resolution. */
            gui_recovery_entry entry = *e;
            char nm[256];
            (void)snprintf(nm, sizeof nm, "%s", e->name);
            const char *target = "";
            bool proceed = true;
            if (action == GUI_RECOVERY_DISCARD) {
                char prompt[GUI_RECOVERY_PATH_CAP + 320];
                (void)snprintf(prompt, sizeof prompt,
                               "Permanently discard recovered unsaved work for '%s'?\n\n%s\n\n"
                               "This cannot be undone.",
                               entry.name, entry.original_path[0] ? entry.original_path : "Untitled project");
                proceed = tinyfd_messageBox("Discard recovered work?", prompt, "yesno", "warning", 0) == 1;
            } else if (action == GUI_RECOVERY_SAVE_AS) {
                static const char *filt[] = {"*.ntpacker_project"};
                char recovered_default[GUI_RECOVERY_PATH_CAP + 32];
                const char *def = "recovered.ntpacker_project";
                if (entry.original_path[0] != '\0') {
                    static const char suffix[] = ".ntpacker_project";
                    const size_t path_len = strlen(entry.original_path);
                    const size_t suffix_len = sizeof suffix - 1u;
                    if (path_len >= suffix_len && strcmp(entry.original_path + path_len - suffix_len, suffix) == 0) {
                        (void)snprintf(recovered_default, sizeof recovered_default, "%.*s.recovered%s",
                                       (int)(path_len - suffix_len), entry.original_path, suffix);
                    } else {
                        (void)snprintf(recovered_default, sizeof recovered_default, "%s.recovered%s",
                                       entry.original_path, suffix);
                    }
                    def = recovered_default;
                }
                const char *picked = tinyfd_saveFileDialog("Save Recovered Project As", def, 1, filt, "ntpacker project");
                if (picked == NULL) {
                    proceed = false; /* cancelled -> keep the row, journal stays on disk */
                } else {
                    target = picked;
                }
            }
            if (proceed) {
                char err[256];
                tp_status st = gui_recovery_resolve_entry(&entry, action, target, err, sizeof err);
                if (st == TP_STATUS_OK) {
                    recovery_remove_row(row);
                    if (action == GUI_RECOVERY_DISCARD) {
                        set_statusf("Discarded recovered '%s'.", nm);
                    } else {
                        set_statusf("Recovered '%s' saved.", nm);
                    }
                } else {
                    /* NON-DESTRUCTIVE: the journal is still on disk; keep the row so the user can retry. */
                    set_statusf_ex(STATUS_ERROR, "Recover '%s' failed: %s", nm, err);
                }
            }
        }
    }

}
