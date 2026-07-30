/* Fail-closed fixture: a comment opener inside a STRING literal must not be
 * allowed to swallow include directives.
 *
 * The include scan keeps string literals (an include names its header inside
 * one) and can therefore only drop comments first, which makes the `/ *` below
 * look like a comment that runs to the `* /` after the include -- taking the
 * forbidden directive with it. The checker compares its directive count against
 * the correctly-lexed text and aborts instead of reporting "no violations"; the
 * registering ctest asserts that abort, so a silent pass fails. */
const char *open_marker = "/*";
#include "gui_project.h"
const char *close_marker = "*/";
