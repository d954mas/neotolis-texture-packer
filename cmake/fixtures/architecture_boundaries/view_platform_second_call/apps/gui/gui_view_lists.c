/* Masked-second-call fixture: a view TU making TWO forbidden platform calls.
 * `system` is a real pre-SR-BASE debt symbol (gui_view_chrome.c is allowed to
 * name it), so under a first-hit-only scan it was the only hit reported and
 * `popen` rode in behind an allowance that never covered it. The registering
 * ctest asserts two hits. */
void bad_view_platform_twice(void) {
    (void)system("forbidden");
    (void)popen("forbidden", "r");
}
