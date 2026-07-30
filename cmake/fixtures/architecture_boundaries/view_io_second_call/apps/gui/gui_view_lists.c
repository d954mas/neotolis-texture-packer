/* Masked-second-call fixture: a view TU making TWO forbidden filesystem calls.
 * A whole-file MATCH reports only the FIRST, so the later call rode in behind
 * whatever allowance covered the earlier one. The registering ctest asserts two
 * hits, which is exactly one more than a first-hit-only scan can produce. */
void bad_view_io_twice(void) {
    (void)fopen("forbidden", "rb");
    (void)opendir("forbidden");
}
