#ifndef NTPACKER_GUI_LOG_FILE_H
#define NTPACKER_GUI_LOG_FILE_H

/* D1 (crash diagnostics): mirror every nt_log line to a rotating file under the app-data dir, so a
 * crash leaves the developer a log to inspect (today nt_log goes only to the console). An app-side
 * nt_log SINK -- no engine change. fflush per line keeps the tail intact across a hard crash (D2's
 * handler will additionally flush). The sink is thread-safe (a GUI pack logs from tp_pack's parallel
 * workers): it serializes its body with an internal mutex -- see gui_log_file.c. */

#ifdef __cplusplus
extern "C" {
#endif

/* Install the file sink: resolve <app-data>/logs, rotate if the active log is already over cap, open
 * ntpacker.log for append, and register the nt_log sink. No-op (and NO files/dirs created) when
 * NTPACKER_GUI_HEADLESS is set -- CI runs the GUI selftest headless and must not need a writable
 * app-data dir. Also a graceful no-op if the dir/file can't be created. Call once, near main() start;
 * subsequent lines are then mirrored to the file. */
void gui_log_file_install(void);

/* Unregister the sink and close the file (clean exit). Idempotent; safe if install was a no-op. */
void gui_log_file_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_LOG_FILE_H */
