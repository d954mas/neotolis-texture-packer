/* Masked-second-call fixture: a core TU calling TWO frontend entry points.
 * A first-hit-only scan reported one, so the core file's remaining frontend
 * dependencies stayed invisible. The registering ctest asserts two hits. */
void bad_core_frontend_twice(void) {
    gui_submit();
    cli_emit();
}
