#ifndef NTPACKER_GUI_CRASH_H
#define NTPACKER_GUI_CRASH_H

/* D2 (crash diagnostics): install a process crash handler so a fatal fault leaves the developer a
 * post-mortem. On a caught crash we write a dump (Windows MiniDump) or backtrace (POSIX) + a
 * `last-run.crashed` marker into <app-data>/crash; D3 reads the marker on the next launch to offer
 * the report. Today the app has NO crash handler -- a crash produces nothing to send.
 *
 * DEADLOCK / ASYNC-SIGNAL SAFETY (the crux): D1's file-log sink is guarded by a mutex and runs on
 * tp_pack's parallel encode workers, so a crash can happen while a worker holds that mutex mid-write.
 * The handler therefore NEVER touches the log (no nt_log, no D1 mutex -- it would deadlock), and on
 * POSIX calls only async-signal-safe libc (open/write/close/_exit, backtrace/backtrace_symbols_fd).
 * D1 already fflushes per line, so the log tail is durable without the handler's help. All paths are
 * pre-resolved at install time into static buffers; the signal path allocates nothing. See gui_crash.c. */

#ifdef __cplusplus
extern "C" {
#endif

/* Install the crash handler + pre-resolve <app-data>/crash and the marker/dump paths. Call ONCE early
 * in main(), right BEFORE the D1 log install, and only for a real windowed run (NOT the --parity/--shot
 * dev seams, which must stay side-effect-free). It is not literally main()'s first statement: a fault
 * before this point falls back to the OS default (same as not-installed) -- acceptable, since the value
 * is catching interactive-session crashes. No-op under NTPACKER_GUI_HEADLESS (CI runs the GUI selftest
 * under sanitizers whose own signal handlers must not be overridden, and headless must not need a
 * writable app-data dir). Degrades gracefully: if the crash dir can't be resolved/created it still
 * installs the handler (a crash re-raises cleanly) but writes no dump/marker; it never aborts startup
 * and never calls nt_log (keep it callable before nt_engine_init). */
void gui_crash_install(void);

/* Remove the crash marker on a CLEAN shutdown (main's normal exit path), so D3 sees the marker iff the
 * previous run actually crashed. Idempotent; a no-op if install was a no-op. */
void gui_crash_clear_marker(void);

/* D3: if the PREVIOUS run left a `last-run.crashed` marker (i.e. it crashed), show a one-time yes/no
 * prompt offering to open <app-data>, which contains the sibling crash/ and logs/ folders, so the user can send diagnostics to the
 * developer; open the folder in the OS file explorer on YES, and clear the marker on EITHER choice so
 * it never prompts twice. A clean previous run (no marker) shows nothing. Call ONCE early in main()
 * AFTER gui_crash_install and only for a real windowed run. Strict no-op when the crash dir never
 * resolved -- not installed, or headless (gui_crash_install no-ops there, leaving the marker path
 * empty) -- so the GUI selftest / CI stay silent. Safe to call before window init: the dialog is an
 * OS-native modal (tinyfiledialogs), independent of our GL window. */
void gui_crash_report_prompt(void);

/* Deliberately fault (deref a volatile null) to exercise the handler end-to-end. Behind the hidden
 * --selftest-crash dev arg ONLY -- never a shipped/live path. Self-guards to a no-op under
 * NTPACKER_GUI_HEADLESS so it can never fire in CI. */
void gui_crash_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_CRASH_H */
