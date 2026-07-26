#ifndef NTPACKER_GUI_SHELL_QUOTE_H
#define NTPACKER_GUI_SHELL_QUOTE_H

/* POSIX shell-word quoting shared by every system()-dispatched "open in the OS file manager"
 * helper (gui_crash.c's crash report reveal, gui_view_chrome.c's "Show in Explorer"). The paths
 * are USER-DERIVED -- home dirs, imported asset filenames -- so an apostrophe (/home/o'brien/...)
 * or any shell metacharacter would otherwise break the quoting and let the path INJECT into the
 * command. Single source of truth so a fix lands once. Windows takes the UTF-16 path straight to
 * ShellExecuteW and never touches a shell, so this is POSIX-only. */

#ifndef _WIN32

#include <stdbool.h>
#include <stddef.h>

/* Wrap `in` as ONE POSIX shell word: surround it with single quotes and rewrite every embedded ' as
 * the '\'' escape. Writes out[out_size]; returns false (caller then skips the open) if it would not
 * fit. A fork+execlp with argv would avoid the shell entirely; escaping is the smaller change. */
static inline bool gui_shell_squote(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    if (out_size < 3) { /* need at least '' + NUL */
        return false;
    }
    out[o++] = '\'';
    for (const char *p = in; *p != '\0'; p++) {
        if (*p == '\'') {
            if (o + 6 > out_size) { /* 4 bytes for '\'' + the eventual closing ' + NUL */
                return false;
            }
            out[o++] = '\'';
            out[o++] = '\\';
            out[o++] = '\'';
            out[o++] = '\'';
        } else {
            if (o + 2 >= out_size) { /* this char + closing ' + NUL */
                return false;
            }
            out[o++] = *p;
        }
    }
    out[o++] = '\'';
    out[o] = '\0';
    return true;
}

#endif /* !_WIN32 */

#endif /* NTPACKER_GUI_SHELL_QUOTE_H */
